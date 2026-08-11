#include "archive.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_ring.h"
#include "radio_source.h"
#include "rtsp_events.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "archive";

// The live response measured 31KB on 2026-08-11 and grows as shows are recorded,
// so 64KB leaves real headroom. A response that outgrows this is truncated and
// fails to parse, which is why the margin is generous rather than snug. The radio firmware kept its buffer alive between
// fetches to avoid fragmenting SRAM — here it is PSRAM and freed each time,
// because the catalogue is fetched rarely and the memory is better spent on the
// audio ring.
#define JSON_BUF_BYTES 65536

#define LABEL_MAX 24
#define NAME_MAX  40
#define URL_MAX   160

typedef struct {
  char url[URL_MAX];
  char label[LABEL_MAX]; // "MAR 27 - 114m"
  uint32_t duration_secs;
} episode_t;

typedef struct {
  char name[NAME_MAX];
  episode_t eps[ARCHIVE_MAX_EPISODES];
  int ep_count;
} show_t;

static show_t *s_shows = NULL; // PSRAM: 24 shows x 5 episodes is ~22KB
static int s_show_count = 0;

static volatile bool s_ready = false;
static volatile bool s_loading = false;
static void (*s_ready_cb)(void) = NULL;

// Currently playing episode, and where in the show it sits.
static int s_cur_show = -1;
static int s_cur_ep = -1;

// Work items for the worker task. Episode advance cannot happen on the drain
// task (radio_source_stop() waits for it), so it is posted here instead.
typedef enum {
  WORK_FETCH = 0,
  WORK_NEXT_EPISODE,
  WORK_STOP,
} work_t;

static QueueHandle_t s_q = NULL;

// ── Label formatting ──────────────────────────────────────────────────────────

static const char *const MONTHS[] = {"???", "JAN", "FEB", "MAR", "APR", "MAY",
                                     "JUN", "JUL", "AUG", "SEP", "OCT", "NOV",
                                     "DEC"};

/// "2026-03-27T18:30:00+00:00" + 6840s -> "MAR 27 - 114m"
static void format_label(const char *recorded_at, uint32_t dur_secs, char *out,
                         size_t out_len) {
  int month = 0, day = 0;
  if (recorded_at && strlen(recorded_at) >= 10) {
    month = atoi(recorded_at + 5);
    day = atoi(recorded_at + 8);
  }
  const char *mon = (month >= 1 && month <= 12) ? MONTHS[month] : MONTHS[0];
  snprintf(out, out_len, "%s %d - %um", mon, day,
           (unsigned)(dur_secs / 60));
}

// ── Fetch ─────────────────────────────────────────────────────────────────────

static bool fetch_catalogue(void) {
  char *buf = heap_caps_malloc(JSON_BUF_BYTES, MALLOC_CAP_SPIRAM);
  if (!buf) {
    ESP_LOGE(TAG, "json buffer alloc failed");
    return false;
  }

  esp_http_client_config_t cfg = {
      .url = CONFIG_ARCHIVE_URL,
      .timeout_ms = 10000,
  };
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) {
    heap_caps_free(buf);
    return false;
  }

  int total = -1;
  if (esp_http_client_open(c, 0) == ESP_OK) {
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status == 200) {
      total = 0;
      while (total < JSON_BUF_BYTES - 1) {
        int n = esp_http_client_read(c, buf + total, JSON_BUF_BYTES - 1 - total);
        if (n <= 0) {
          break;
        }
        total += n;
      }
      buf[total > 0 ? total : 0] = 0;
    } else {
      ESP_LOGW(TAG, "HTTP %d from %s", status, CONFIG_ARCHIVE_URL);
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);

  if (total <= 0) {
    heap_caps_free(buf);
    return false;
  }
  ESP_LOGI(TAG, "fetched %d bytes", total);

  cJSON *root = cJSON_Parse(buf);
  heap_caps_free(buf); // cJSON copies strings, so the raw text can go now
  if (!root) {
    ESP_LOGW(TAG, "json parse failed");
    return false;
  }

  int count = 0;
  cJSON *show_node = NULL;
  cJSON_ArrayForEach(show_node, root) {
    if (count >= ARCHIVE_MAX_SHOWS) {
      break;
    }
    if (!cJSON_IsArray(show_node) || !show_node->string) {
      continue;
    }
    show_t *sh = &s_shows[count];
    strlcpy(sh->name, show_node->string, sizeof(sh->name));
    sh->ep_count = 0;

    cJSON *ep_node = NULL;
    cJSON_ArrayForEach(ep_node, show_node) {
      if (sh->ep_count >= ARCHIVE_MAX_EPISODES) {
        break;
      }
      const cJSON *url = cJSON_GetObjectItem(ep_node, "url");
      const cJSON *rec = cJSON_GetObjectItem(ep_node, "recorded_at");
      const cJSON *dur = cJSON_GetObjectItem(ep_node, "duration_secs");
      if (!cJSON_IsString(url)) {
        continue;
      }
      episode_t *e = &sh->eps[sh->ep_count];
      // Paths come back relative to the station host.
      if (url->valuestring[0] == '/') {
        snprintf(e->url, sizeof(e->url), "%s%s", CONFIG_ARCHIVE_BASE,
                 url->valuestring);
      } else {
        strlcpy(e->url, url->valuestring, sizeof(e->url));
      }
      e->duration_secs = cJSON_IsNumber(dur) ? (uint32_t)dur->valuedouble : 0;
      format_label(cJSON_IsString(rec) ? rec->valuestring : "",
                   e->duration_secs, e->label, sizeof(e->label));
      sh->ep_count++;
    }
    if (sh->ep_count > 0) {
      count++; // skip shows with no playable episodes
    }
  }
  cJSON_Delete(root);

  s_show_count = count;
  ESP_LOGI(TAG, "loaded %d shows", s_show_count);
  return s_show_count > 0;
}

// ── Playback ──────────────────────────────────────────────────────────────────

static void publish_metadata(int show, int ep) {
  const show_t *sh = &s_shows[show];
  const episode_t *e = &sh->eps[ep];

  rtsp_event_data_t data = {0};
  strlcpy(data.metadata.station, "ARCHIVE", sizeof(data.metadata.station));
  strlcpy(data.metadata.title, sh->name, sizeof(data.metadata.title));
  strlcpy(data.metadata.artist, e->label, sizeof(data.metadata.artist));
  // Duration is known up front, so the progress bar is a real position for a
  // recorded show rather than a track timer.
  data.metadata.duration_secs = e->duration_secs;
  data.metadata.position_secs = 0;
  rtsp_events_emit(RTSP_EVENT_METADATA, &data);

  // Leaves the standby screen. The radio's coex handler ignores PLAYING unless a
  // real AirPlay client is connected, so this cannot trigger a handover.
  rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);

  led_ring_set_state(LED_RING_ARCHIVE);
}

/// Read just the first bytes of an episode to measure its ID3v2 tag, so playback
/// can start past it. One tiny round trip beats transferring megabytes of tag.
static uint32_t probe_id3_size(const char *url) {
  esp_http_client_config_t cfg = {
      .url = url,
      .timeout_ms = 5000,
  };
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) {
    return 0;
  }
  esp_http_client_set_header(c, "Range", "bytes=0-15");

  uint32_t size = 0;
  if (esp_http_client_open(c, 0) == ESP_OK) {
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status == 200 || status == 206) {
      uint8_t head[16] = {0};
      int got = 0;
      while (got < (int)sizeof(head)) {
        int n = esp_http_client_read(c, (char *)head + got,
                                     (int)sizeof(head) - got);
        if (n <= 0) {
          break;
        }
        got += n;
      }
      if (got >= 10) {
        size = radio_id3_tag_size(head, (size_t)got);
      }
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return size;
}

static bool start_episode(int show, int ep) {
  if (show < 0 || show >= s_show_count) {
    return false;
  }
  if (ep < 0 || ep >= s_shows[show].ep_count) {
    return false;
  }
  s_cur_show = show;
  s_cur_ep = ep;
  ESP_LOGI(TAG, "playing %s / %s", s_shows[show].name, s_shows[show].eps[ep].label);
  uint32_t skip = probe_id3_size(s_shows[show].eps[ep].url);
  if (skip) {
    ESP_LOGI(TAG, "starting past %lu byte ID3v2 tag", (unsigned long)skip);
  }
  if (radio_source_play_url(s_shows[show].eps[ep].url, true, skip) != ESP_OK) {
    return false;
  }
  publish_metadata(show, ep);
  return true;
}

/// Runs on the radio's drain task — post and return, never block here.
static void on_episode_ended(void) {
  work_t w = WORK_NEXT_EPISODE;
  if (s_q) {
    xQueueSend(s_q, &w, 0);
  }
}

static void archive_task(void *arg) {
  (void)arg;
  work_t w;
  while (1) {
    if (xQueueReceive(s_q, &w, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    switch (w) {
    case WORK_FETCH:
      s_loading = true;
      s_ready = fetch_catalogue();
      s_loading = false;
      if (s_ready_cb) {
        s_ready_cb();
      }
      break;

    case WORK_NEXT_EPISODE: {
      int next = s_cur_ep + 1;
      if (s_cur_show >= 0 && next < s_shows[s_cur_show].ep_count) {
        ESP_LOGI(TAG, "episode ended — advancing");
        start_episode(s_cur_show, next);
      } else {
        // End of the show: back to live radio rather than silence.
        ESP_LOGI(TAG, "show ended — returning to live radio");
        s_cur_show = s_cur_ep = -1;
        radio_source_play_live();
        led_ring_set_state(LED_RING_STREAMING);
      }
      break;
    }

    case WORK_STOP:
      s_cur_show = s_cur_ep = -1;
      radio_source_play_live();
      led_ring_set_state(LED_RING_STREAMING);
      break;
    }
  }
}

// ── Public API ────────────────────────────────────────────────────────────────

void archive_init(void) {
  if (s_q) {
    return;
  }
  s_shows = heap_caps_calloc(ARCHIVE_MAX_SHOWS, sizeof(show_t), MALLOC_CAP_SPIRAM);
  if (!s_shows) {
    ESP_LOGE(TAG, "show table alloc failed — archive disabled");
    return;
  }
  s_q = xQueueCreate(4, sizeof(work_t));
  if (!s_q) {
    ESP_LOGE(TAG, "queue alloc failed — archive disabled");
    return;
  }
  radio_source_set_ended_cb(on_episode_ended);
  // Priority 3, same as the metadata poller: below audio, above nothing that
  // matters.
  xTaskCreatePinnedToCore(archive_task, "archive", 6144, NULL, 3, NULL, 0);
  ESP_LOGI(TAG, "ready (%s)", CONFIG_ARCHIVE_URL);
}

void archive_start_fetch(void) {
  if (!s_q || s_loading) {
    return;
  }
  work_t w = WORK_FETCH;
  xQueueSend(s_q, &w, 0);
}

bool archive_is_ready(void) { return s_ready; }
bool archive_is_loading(void) { return s_loading; }
void archive_set_ready_cb(void (*cb)(void)) { s_ready_cb = cb; }

int archive_show_count(void) { return s_show_count; }

const char *archive_show_name(int show) {
  return (show >= 0 && show < s_show_count) ? s_shows[show].name : "";
}

int archive_episode_count(int show) {
  return (show >= 0 && show < s_show_count) ? s_shows[show].ep_count : 0;
}

const char *archive_episode_label(int show, int episode) {
  if (show < 0 || show >= s_show_count) {
    return "";
  }
  if (episode < 0 || episode >= s_shows[show].ep_count) {
    return "";
  }
  return s_shows[show].eps[episode].label;
}

esp_err_t archive_play(int show, int episode) {
  if (!s_shows) {
    return ESP_ERR_INVALID_STATE;
  }
  return start_episode(show, episode) ? ESP_OK : ESP_FAIL;
}

void archive_stop(void) {
  if (!s_q) {
    return;
  }
  work_t w = WORK_STOP;
  xQueueSend(s_q, &w, 0);
}

bool archive_is_playing(void) { return s_cur_show >= 0; }
