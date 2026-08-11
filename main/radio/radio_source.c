/**
 * @file radio_source.c
 * @brief HTTP MP3 internet-radio source (see radio_source.h for design notes).
 *
 *   http ──[fill task]──▶ ring (PSRAM) ──[drain task]──▶ mp3 decode ──▶ I2S
 */

#include "radio_source.h"

#include "audio_output.h"
#include "settings.h"
#include "rtsp_events.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "audio_vis.h"
#include "cJSON.h"
#include <math.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "decoder/impl/esp_mp3_dec.h"

#include <string.h>

static const char *TAG = "radio";

// Read chunk from the socket, and the working buffer the decoder consumes from.
#define HTTP_READ_CHUNK 2048
// One MP3 frame decodes to at most 1152 samples * 2ch * 2 bytes = 4608 bytes.
// Double it for headroom against needed_size growth.
#define PCM_BUF_BYTES 9216

// The rate audio_output_init() brings I2S up at. Matching it means the radio
// never has to retune the shared clock.
#define AUDIO_OUTPUT_NATIVE_RATE 44100

static StreamBufferHandle_t s_ring = NULL;
static uint8_t *s_ring_storage = NULL;
static StaticStreamBuffer_t s_ring_struct;

static void *s_mp3 = NULL;
static esp_http_client_handle_t s_client = NULL;

static TaskHandle_t s_fill_task = NULL;
static TaskHandle_t s_drain_task = NULL;
static volatile bool s_running = false;
static volatile bool s_connected = false;
static uint32_t s_reconnects = 0;

// ── Archive playback ──────────────────────────────────────────────────────────
// The same pipeline plays recorded shows: an episode is just an HTTP MP3, and a
// file is an easier case than a live stream because it can be re-read. The one
// real difference is that it ENDS, where a live stream stalling means "reconnect".
static char s_url[192] = CONFIG_RADIO_STREAM_URL;
static volatile bool s_archive_mode = false;
static volatile bool s_stream_ended = false;
static void (*s_ended_cb)(void) = NULL;

// ── ID3v2 ─────────────────────────────────────────────────────────────────────
// Archive episodes begin with an ID3v2 tag, and it is not small: the first file
// checked carried 2.1MB of it (embedded artwork). Those bytes are not MP3 frames,
// and feeding them to the decoder makes the resync loop hunt for a sync word
// through megabytes of tag and JPEG data — where it periodically finds a byte
// pair that looks like a valid frame header and decodes noise. That is heard as
// static, not silence, which is what made it look like a format problem.
//
// Live streams do not start with a tag, so this never showed up before.
static bool s_id3_checked = false;
// Byte offset to start the request at, so the tag is never transferred. Skipping
// it on arrival still cost the whole download: 2.1MB took ~15s before the first
// sample, which is most of a listener's patience.
static uint32_t s_range_offset = 0;
static uint32_t s_id3_skip = 0;

/// Returns the total bytes to skip for a leading ID3v2 tag, or 0 if there is none.
/// Needs at least 10 bytes.
uint32_t radio_id3_tag_size(const uint8_t *b, size_t len) {
  if (len < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3') {
    return 0;
  }
  // Size is four synchsafe bytes: 7 significant bits each, high bit always 0.
  uint32_t size = ((uint32_t)(b[6] & 0x7F) << 21) |
                  ((uint32_t)(b[7] & 0x7F) << 14) |
                  ((uint32_t)(b[8] & 0x7F) << 7) | (uint32_t)(b[9] & 0x7F);
  uint32_t total = 10 + size;
  if (b[5] & 0x10) {
    total += 10; // v2.4 footer
  }
  return total;
}
static uint32_t s_last_bytes_ms = 0;
static uint32_t s_sample_rate = 0;

// Defined in the coexistence section at the end of this file.
static void radio_coex_init(void);
static void radio_meta_task(void *arg);
static void notify_ended(void);

// The display leaves its "AirPlay Ready" standby screen only on
// RTSP_EVENT_PLAYING, so the radio has to announce itself on the same event bus
// it listens to. That is a loop: our own coex handler reads PLAYING as "an
// AirPlay session started, yield the channel" and the radio would hand off to
// itself.
//
// rtsp_events_emit() dispatches synchronously in the calling task, so recording
// the task handle for the duration of the emit identifies our own events
// exactly. A bare bool would race: a real AirPlay event arriving on another task
// mid-emit would be discarded.
static volatile TaskHandle_t s_self_emit_task = NULL;

// True while an AirPlay session owns the output. Used to decide whether
// stopping the radio should return the display to standby: when AirPlay is the
// reason we stopped, its own events drive the screen and ours would blank a live
// session.
static volatile bool s_airplay_active = false;

// ── Volume ────────────────────────────────────────────────────────────────────
// audio_output.c applies volume inside its playback task (apply_volume), but the
// radio owns I2S directly and never goes through that task — so without this the
// encoder turned a control that did nothing to radio audio.
//
// Q15 fixed point, ramped toward the target rather than applied instantly: a
// step change scales the signal by its current amplitude and clicks (the
// "zipper" audio_output.c documents).
#define GAIN_UNITY     32768
#define GAIN_RAMP_STEP 32 // ~23ms for a full-scale change at 44.1kHz

static volatile int32_t s_target_gain = GAIN_UNITY;
static volatile bool s_muted = false;
static int32_t s_cur_gain = GAIN_UNITY;

void radio_source_set_volume_db(float db) {
  float lin = powf(10.0f, db / 20.0f);
  int32_t q = (int32_t)(lin * GAIN_UNITY);
  s_target_gain = q < 0 ? 0 : (q > GAIN_UNITY ? GAIN_UNITY : q);
}

void radio_source_set_muted(bool muted) { s_muted = muted; }

static void apply_gain(int16_t *buf, size_t samples) {
  int32_t target = s_muted ? 0 : s_target_gain;
  if (s_cur_gain == target && target == GAIN_UNITY) {
    return; // unity and settled — leave the samples untouched
  }
  for (size_t i = 0; i < samples; i++) {
    if (s_cur_gain < target) {
      s_cur_gain += GAIN_RAMP_STEP;
      if (s_cur_gain > target) {
        s_cur_gain = target;
      }
    } else if (s_cur_gain > target) {
      s_cur_gain -= GAIN_RAMP_STEP;
      if (s_cur_gain < target) {
        s_cur_gain = target;
      }
    }
    buf[i] = (int16_t)(((int32_t)buf[i] * s_cur_gain) >> 15);
  }
}

static void radio_emit(rtsp_event_t event, const rtsp_event_data_t *data) {
  s_self_emit_task = xTaskGetCurrentTaskHandle();
  rtsp_events_emit(event, data);
  s_self_emit_task = NULL;
}
static TaskHandle_t s_meta_task = NULL;

// ============================================================================
// Connection handling — owned entirely by the fill task
// ============================================================================

static void radio_disconnect(void) {
  s_connected = false;
  if (s_client) {
    esp_http_client_close(s_client);
    esp_http_client_cleanup(s_client);
    s_client = NULL;
  }
}

static bool radio_connect(void) {
  radio_disconnect();

  esp_http_client_config_t cfg = {
      .url = s_url,
      .timeout_ms = 5000,
      .buffer_size = HTTP_READ_CHUNK,
      // Icecast responds to a plain GET with an unbounded body; we never expect
      // it to end, so no redirect or content-length handling is needed.
      .disable_auto_redirect = false,
  };

  s_client = esp_http_client_init(&cfg);
  if (!s_client) {
    ESP_LOGE(TAG, "http client init failed");
    return false;
  }

  char range[32];
  if (s_range_offset > 0) {
    snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)s_range_offset);
    esp_http_client_set_header(s_client, "Range", range);
  }

  esp_err_t err = esp_http_client_open(s_client, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
    radio_disconnect();
    return false;
  }

  // Must fetch headers before reading the body, or the first read returns them.
  int64_t len = esp_http_client_fetch_headers(s_client);
  int status = esp_http_client_get_status_code(s_client);
  // 206 Partial Content is the success case for a ranged request.
  if (status != 200 && status != 206) {
    ESP_LOGW(TAG, "unexpected status %d", status);
    radio_disconnect();
    return false;
  }

  ESP_LOGI(TAG, "connected: %s (status %d, len %lld)", s_url,
           status, (long long)len);
  s_connected = true;
  s_last_bytes_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  return true;
}

// ============================================================================
// Fill task — socket -> ring. Reconnects itself.
// ============================================================================

static void radio_fill_task(void *arg) {
  (void)arg;
  uint8_t *chunk = heap_caps_malloc(HTTP_READ_CHUNK, MALLOC_CAP_DEFAULT);
  if (!chunk) {
    ESP_LOGE(TAG, "fill buffer alloc failed");
    s_fill_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  uint32_t health_ms = 0;

  while (s_running) {
    if (!s_connected) {
      if (!radio_connect()) {
        vTaskDelay(pdMS_TO_TICKS(2000)); // back off before retrying
        continue;
      }
    }

    int n = esp_http_client_read(s_client, (char *)chunk, HTTP_READ_CHUNK);
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (n > 0) {
      s_last_bytes_ms = now;

      uint8_t *p = chunk;
      size_t len = (size_t)n;

      // Discard any leading ID3v2 tag here rather than in the decoder, so the
      // ring holds audio and the prefill cushion means what it says.
      if (!s_id3_checked && len >= 10) {
        s_id3_skip = radio_id3_tag_size(chunk, len);
        s_id3_checked = true;
        if (s_id3_skip) {
          ESP_LOGI(TAG, "skipping %lu byte ID3v2 tag",
                   (unsigned long)s_id3_skip);
        }
      }
      if (s_id3_skip > 0) {
        uint32_t drop = s_id3_skip < len ? s_id3_skip : (uint32_t)len;
        s_id3_skip -= drop;
        p += drop;
        len -= drop;
      }

      // Send EVERYTHING, retrying until it fits. The return value used to be
      // ignored with a 200ms timeout, which silently discarded whatever did not
      // fit — invisible on a live stream, because the server paces delivery and
      // the ring rarely fills, but an archive episode arrives at megabytes per
      // second and saturates the ring almost continuously. The dropped bytes were
      // the glitching.
      //
      // Retrying in bounded steps rather than blocking forever keeps the task
      // responsive to s_running, which radio_source_stop() waits on.
      size_t sent = 0;
      while (sent < len && s_running) {
        sent += xStreamBufferSend(s_ring, p + sent, len - sent,
                                  pdMS_TO_TICKS(100));
      }
    } else if (n == 0) {
      // An archive episode is finite: a complete body is the end of the show, not
      // a stall to reconnect through. Stop reading but leave the ring alone — the
      // drain task still has several seconds of audio to play out.
      if (s_archive_mode && esp_http_client_is_complete_data_received(s_client)) {
        ESP_LOGI(TAG, "episode complete — draining %u bytes",
                 (unsigned)xStreamBufferBytesAvailable(s_ring));
        s_stream_ended = true;
        break;
      }
      // No data this pass. Only a sustained silence means the flow is dead —
      // the reconnect happens on this task so nothing else can free s_client
      // underneath us.
      if (now - s_last_bytes_ms > CONFIG_RADIO_STALL_MS) {
        ESP_LOGW(TAG, "flow dead %lums — reconnecting (ring=%u)",
                 (unsigned long)(now - s_last_bytes_ms),
                 (unsigned)xStreamBufferBytesAvailable(s_ring));
        s_reconnects++;
        radio_disconnect();
      } else {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    } else {
      if (s_archive_mode) {
        // Reconnecting mid-file would restart the episode from the top.
        ESP_LOGW(TAG, "archive read error %d — ending episode", n);
        s_stream_ended = true;
        break;
      }
      ESP_LOGW(TAG, "read error %d — reconnecting", n);
      s_reconnects++;
      radio_disconnect();
    }

    if (now - health_ms >= 5000) {
      health_ms = now;
      ESP_LOGI(TAG, "ring=%u/%d rate=%lu reconnects=%lu",
               (unsigned)xStreamBufferBytesAvailable(s_ring),
               CONFIG_RADIO_RING_BYTES, (unsigned long)s_sample_rate,
               (unsigned long)s_reconnects);
    }
  }

  heap_caps_free(chunk);
  radio_disconnect();
  s_fill_task = NULL;
  vTaskDelete(NULL);
}

// Reported once the episode's audio has actually finished playing, not when its
// download completed.
//
// The callback runs on the drain task, which radio_source_stop() waits for — so
// it MUST NOT call stop(), start() or anything that does. Post to a queue and
// return.
static void notify_ended(void) {
  if (s_archive_mode && s_stream_ended && s_ended_cb) {
    s_ended_cb();
  }
}

// ============================================================================
// Drain task — ring -> decode -> I2S
// ============================================================================

static void radio_drain_task(void *arg) {
  (void)arg;
  uint8_t *enc = heap_caps_malloc(HTTP_READ_CHUNK, MALLOC_CAP_DEFAULT);
  uint8_t *pcm = heap_caps_malloc(PCM_BUF_BYTES, MALLOC_CAP_DEFAULT);
  if (!enc || !pcm) {
    ESP_LOGE(TAG, "decode buffer alloc failed");
    goto done;
  }

  // Prefill. Without this the ring holds nothing: fill and drain both run at the
  // stream's real-time rate, so the depth present when draining starts is the
  // depth that persists.
  {
    uint32_t waited = 0;
    while (s_running &&
           xStreamBufferBytesAvailable(s_ring) < CONFIG_RADIO_PREFILL_BYTES) {
      vTaskDelay(pdMS_TO_TICKS(20));
      waited += 20;
      if (waited > 15000) {
        ESP_LOGW(TAG, "prefill timeout at %u bytes — starting anyway",
                 (unsigned)xStreamBufferBytesAvailable(s_ring));
        break;
      }
    }
    if (s_running) {
      ESP_LOGI(TAG, "primed %u bytes — playback starting",
               (unsigned)xStreamBufferBytesAvailable(s_ring));
    }
  }

  // Take exclusive ownership of I2S before writing a single sample.
  //
  // audio_output_write() is NOT a queue — it calls i2s_channel_write() directly.
  // Meanwhile main.c has started AirPlay's playback task, whose underflow branch
  // writes a frame of SILENCE to the same channel whenever the receiver has
  // nothing (i.e. constantly, with no AirPlay session). Two writers on one I2S
  // channel interleave at frame granularity — music, silence, music, silence —
  // which is precisely the garbling.
  //
  // It also throttled us: both writers block for DMA space, so decode ran at
  // ~29 frames/sec against the 38.3 that 44.1kHz MP3 needs, and the ring backed
  // up to full.
  //
  // So stop that task and do NOT restart it. audio_output_init() already enabled
  // the channel and we are not changing the clock, so there is nothing to race.
  // Handing back to AirPlay later means radio_source_stop() followed by
  // audio_output_start() — that is the arbitration work.
  audio_output_stop();
  ESP_LOGI(TAG, "took I2S ownership (AirPlay playback task stopped)");

  // Apply the persisted volume so the level survives a power cycle and matches
  // what the screen reports.
  float saved_db;
  if (settings_get_volume(&saved_db) != ESP_OK) {
    saved_db = 0.0f; // same default playback_control uses (100%)
  }
  radio_source_set_volume_db(saved_db);

  size_t held = 0;            // bytes carried over from a partial frame
  uint32_t resync_bytes = 0;  // bytes skipped hunting for frame sync
  uint32_t frames = 0;        // successfully decoded frames
  uint32_t write_errors = 0;  // i2s write failures
  while (s_running) {
    size_t want = HTTP_READ_CHUNK - held;
    size_t got =
        xStreamBufferReceive(s_ring, enc + held, want, pdMS_TO_TICKS(100));
    size_t avail = held + got;
    if (avail == 0) {
      // Ring dry. For a live stream that is just a pause in delivery; for an
      // archive episode whose body already completed, it means the audio has all
      // been played.
      if (s_archive_mode && s_stream_ended) {
        ESP_LOGI(TAG, "episode played out");
        break;
      }
      continue;
    }

    esp_audio_dec_in_raw_t raw = {.buffer = enc, .len = (uint32_t)avail};

    while (raw.len > 0 && s_running) {
      esp_audio_dec_out_frame_t frame = {.buffer = pcm, .len = PCM_BUF_BYTES};
      esp_audio_dec_info_t info = {0};

      esp_audio_err_t err = esp_mp3_dec_decode(s_mp3, &raw, &frame, &info);

      if (err == ESP_AUDIO_ERR_DATA_LACK) {
        // Partial frame at the end of the buffer — keep it and append more.
        break;
      }
      if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        ESP_LOGW(TAG, "pcm buffer too small (needed %u, have %d)",
                 (unsigned)frame.needed_size, PCM_BUF_BYTES);
        break;
      }
      if (err != ESP_AUDIO_ERR_OK) {
        // Corrupt or not yet frame-aligned. We join a live stream at an arbitrary
        // offset, so the very first bytes are almost never a frame header.
        //
        // Skip one byte and look for the next sync word. Previously this branch
        // just broke out and kept the whole buffer, so the same bad byte was
        // retried forever: the decoder never advanced, logged
        // "Not supported format", and eventually misread noise as a valid header
        // (reporting 32000 Hz on a 44100 Hz stream). That was the garbling.
        raw.buffer += 1;
        raw.len -= 1;
        resync_bytes++;
        continue;
      }

      if (info.sample_rate && info.sample_rate != s_sample_rate) {
        s_sample_rate = info.sample_rate;
        ESP_LOGI(TAG, "stream format: %lu Hz, %d ch",
                 (unsigned long)info.sample_rate, info.channel);

        // Deliberately do NOT touch the output clock here.
        //
        // audio_output_init() already brings I2S up at 44100 Hz and enables the
        // channel; audio_output_start() only spawns the writer task and does not
        // enable anything. The playback task also has its own
        // disable/enable underrun-recovery path. So calling
        // audio_output_set_sample_rate() from here — even wrapped in stop/start —
        // races AirPlay's management of a channel we share, which produced a
        // stream of "i2s_channel_write: The channel is not enabled" and garbled
        // audio. And it was pointless: the stream is 44100, which is exactly what
        // I2S is already configured for.
        //
        // If a stream ever arrives at another rate we want to know rather than
        // silently play it at the wrong pitch. Retuning safely needs proper
        // ownership of the output, which is what the coex work will introduce.
        if (info.sample_rate != AUDIO_OUTPUT_NATIVE_RATE) {
          ESP_LOGW(TAG,
                   "stream is %lu Hz but the output runs at %d Hz — pitch will "
                   "be wrong; retuning needs output arbitration",
                   (unsigned long)info.sample_rate, AUDIO_OUTPUT_NATIVE_RATE);
        }
      }

      if (frame.decoded_size > 0) {
        // portMAX_DELAY, as a2dp_sink does: real backpressure. A timeout here
        // silently discards audio and caps throughput below real time.
        // 16-bit stereo interleaved, so decoded_size/2 samples.
      apply_gain((int16_t *)pcm, frame.decoded_size / sizeof(int16_t));
      // Post-gain, so the bars follow what is actually audible. This only copies
      // a 128-sample window and returns; the transform runs on the display task.
      audio_vis_push((const int16_t *)pcm,
                     frame.decoded_size / sizeof(int16_t));
      esp_err_t werr = audio_output_write(pcm, frame.decoded_size, portMAX_DELAY);
        if (werr != ESP_OK) {
          write_errors++;
        }
      }

      frames++;
      if (raw.consumed == 0) {
        // Decoder reported success without consuming input: force progress
        // rather than spin on the same bytes.
        raw.buffer += 1;
        raw.len -= 1;
      } else {
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
      }
    }

    // Carry the undecoded tail to the front for the next read.
    if (frames > 0 && (frames % 400) == 0) {
      ESP_LOGI(TAG, "decoded %lu frames, %lu resync bytes, %lu write errors",
               (unsigned long)frames, (unsigned long)resync_bytes,
               (unsigned long)write_errors);
    }

    held = raw.len;
    if (held > 0 && raw.buffer != enc) {
      memmove(enc, raw.buffer, held);
    }
    if (held >= HTTP_READ_CHUNK) {
      held = 0; // desynced beyond recovery; drop and resync on the next frame
    }
  }

done:
  if (enc) {
    heap_caps_free(enc);
  }
  if (pcm) {
    heap_caps_free(pcm);
  }
  notify_ended();
  s_drain_task = NULL;
  vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t radio_source_init(void) {
  if (s_ring) {
    return ESP_OK;
  }

  // Ring storage in PSRAM; the FreeRTOS control struct stays in internal RAM.
  s_ring_storage =
      heap_caps_malloc(CONFIG_RADIO_RING_BYTES + 1, MALLOC_CAP_SPIRAM);
  if (!s_ring_storage) {
    ESP_LOGE(TAG, "failed to allocate %d byte ring in PSRAM",
             CONFIG_RADIO_RING_BYTES);
    return ESP_ERR_NO_MEM;
  }
  s_ring = xStreamBufferCreateStatic(CONFIG_RADIO_RING_BYTES, 1, s_ring_storage,
                                     &s_ring_struct);
  if (!s_ring) {
    heap_caps_free(s_ring_storage);
    s_ring_storage = NULL;
    return ESP_ERR_NO_MEM;
  }

  if (esp_mp3_dec_open(NULL, 0, &s_mp3) != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "mp3 decoder open failed");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "init: ring %d bytes (PSRAM), prefill %d bytes",
           CONFIG_RADIO_RING_BYTES, CONFIG_RADIO_PREFILL_BYTES);
  return ESP_OK;
}

esp_err_t radio_source_start(void) {
  if (s_running) {
    return ESP_OK;
  }
  if (!s_ring || !s_mp3) {
    return ESP_ERR_INVALID_STATE;
  }

  xStreamBufferReset(s_ring);
  s_running = true;
  s_sample_rate = 0;

  // Drain at a higher priority than fill: if either must wait, the task feeding
  // the DAC should win.
  xTaskCreatePinnedToCore(radio_drain_task, "radio_drain", 4096, NULL, 6,
                          &s_drain_task, 0);
  s_stream_ended = false;
  s_id3_checked = false;
  s_id3_skip = 0;
  xTaskCreatePinnedToCore(radio_fill_task, "radio_fill", 4096, NULL, 5,
                          &s_fill_task, 0);
  // Low priority: metadata must never compete with audio.
  xTaskCreatePinnedToCore(radio_meta_task, "radio_meta", 5120, NULL, 3,
                          &s_meta_task, 0);
  ESP_LOGI(TAG, "started");
  return ESP_OK;
}

void radio_source_stop(void) {
  if (!s_running) {
    return;
  }
  s_running = false;
  // Tasks observe s_running and exit on their own; wait briefly so the socket is
  // closed and I2S released before a caller starts another source.
  for (int i = 0; i < 50 && (s_fill_task || s_drain_task || s_meta_task); i++) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  // Give the output back to AirPlay: restart the playback task we stopped when
  // taking ownership.
  audio_output_start();
  ESP_LOGI(TAG, "stopped, I2S returned to AirPlay");
  // Hand the screen back to standby, but not when AirPlay is why we stopped —
  // it publishes its own state and ours would wipe a connecting session.
  if (!s_airplay_active) {
    radio_emit(RTSP_EVENT_DISCONNECTED, NULL);
  }
}

bool radio_source_is_playing(void) {
  return s_running;
}

void radio_source_set_ended_cb(void (*cb)(void)) { s_ended_cb = cb; }

bool radio_source_is_archive(void) { return s_archive_mode; }

esp_err_t radio_source_play_url(const char *url, bool archive,
                                uint32_t start_offset) {
  if (!url || !url[0]) {
    return ESP_ERR_INVALID_ARG;
  }
  radio_source_stop(); // releases I2S and waits for the tasks to exit
  strlcpy(s_url, url, sizeof(s_url));
  s_archive_mode = archive;
  s_range_offset = start_offset;
  ESP_LOGI(TAG, "play %s: %s (offset %lu)", archive ? "episode" : "stream",
           s_url, (unsigned long)start_offset);
  return radio_source_start();
}

esp_err_t radio_source_play_live(void) {
  return radio_source_play_url(CONFIG_RADIO_STREAM_URL, false, 0);
}

uint32_t radio_source_reconnects(void) {
  return s_reconnects;
}

// ============================================================================
// Source mode
// ============================================================================

static uint8_t s_mode = SOURCE_MODE_RADIO;

uint8_t radio_source_get_mode(void) {
  return s_mode;
}

esp_err_t radio_source_set_mode(uint8_t mode) {
  s_mode = mode ? SOURCE_MODE_AIRPLAY : SOURCE_MODE_RADIO;
  settings_set_source_mode(s_mode);
  ESP_LOGI(TAG, "source mode -> %s", s_mode ? "airplay" : "radio");

  if (s_mode == SOURCE_MODE_RADIO) {
    return radio_source_start(); // takes the I2S channel
  }
  radio_source_stop(); // returns the channel to AirPlay's playback task
  return ESP_OK;
}

esp_err_t radio_source_apply_saved_mode(void) {
  radio_coex_init();

  uint8_t mode = SOURCE_MODE_RADIO;
  if (settings_get_source_mode(&mode) != ESP_OK) {
    mode = SOURCE_MODE_RADIO; // unset in NVS: radio is the default
  }
  s_mode = mode ? SOURCE_MODE_AIRPLAY : SOURCE_MODE_RADIO;
  ESP_LOGI(TAG, "source mode at boot: %s", s_mode ? "airplay" : "radio");

  if (s_mode == SOURCE_MODE_RADIO) {
    return radio_source_start();
  }
  return ESP_OK; // AirPlay keeps the output it already started
}

// ============================================================================
// AirPlay coexistence — automatic handover
// ============================================================================
//
// Only one source may hold the I2S channel. An AirPlay session takes it; when the
// session ends the radio takes it back after a short delay, so a brief reconnect
// does not thrash the handover (the same reasoning bt_coex.c applies to the BT
// radio).
//
// The work happens on a dedicated task because radio_source_stop() waits for the
// fill and drain tasks to exit — blocking the RTSP callback or the esp_timer task
// for that long would be a bug.

#define COEX_TARGET_RADIO 0
#define COEX_TARGET_YIELD 1
#define COEX_RESUME_DELAY_US (3 * 1000 * 1000)

static QueueHandle_t s_coex_q = NULL;
static esp_timer_handle_t s_resume_timer = NULL;

static void radio_coex_task(void *arg) {
  (void)arg;
  uint8_t target;
  while (xQueueReceive(s_coex_q, &target, portMAX_DELAY) == pdTRUE) {
    if (target == COEX_TARGET_RADIO) {
      if (s_mode == SOURCE_MODE_RADIO && !s_running) {
        ESP_LOGI(TAG, "AirPlay idle — resuming radio");
        radio_source_start();
      }
    } else {
      if (s_running) {
        ESP_LOGI(TAG, "AirPlay session — yielding I2S");
        radio_source_stop();
      }
    }
  }
}

static void coex_post(uint8_t target) {
  if (s_coex_q) {
    xQueueSend(s_coex_q, &target, 0); // never block the caller
  }
}

static void coex_resume_timer_cb(void *arg) {
  (void)arg;
  coex_post(COEX_TARGET_RADIO);
}

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  // Ignore the events we published ourselves (see radio_emit).
  if (s_self_emit_task == xTaskGetCurrentTaskHandle()) {
    return;
  }
  (void)data;
  (void)user_data;
  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    s_airplay_active = true;
    if (s_resume_timer) {
      esp_timer_stop(s_resume_timer); // cancel any pending resume
    }
    coex_post(COEX_TARGET_YIELD);
    break;

  case RTSP_EVENT_PLAYING:
    // PLAYING alone does NOT mean a session started: playback_control emits it
    // when a local mute is released, and treating that as an AirPlay takeover
    // made the button stop the radio. Only a real client (CLIENT_CONNECTED)
    // arms the yield.
    if (!s_airplay_active) {
      break;
    }
    if (s_resume_timer) {
      esp_timer_stop(s_resume_timer);
    }
    coex_post(COEX_TARGET_YIELD);
    break;
  case RTSP_EVENT_PAUSED:
    // Session is still open — stay yielded so the phone resumes into AirPlay
    // rather than fighting the radio for the channel.
    break;
  case RTSP_EVENT_DISCONNECTED:
    s_airplay_active = false;
    if (s_mode == SOURCE_MODE_RADIO && s_resume_timer) {
      esp_timer_start_once(s_resume_timer, COEX_RESUME_DELAY_US);
    }
    break;
  default:
    break;
  }
}

static void radio_coex_init(void) {
  if (s_coex_q) {
    return;
  }
  s_coex_q = xQueueCreate(4, sizeof(uint8_t));
  if (!s_coex_q) {
    ESP_LOGE(TAG, "coex queue alloc failed — automatic handover disabled");
    return;
  }
  const esp_timer_create_args_t targs = {.callback = coex_resume_timer_cb,
                                         .name = "radio_resume"};
  esp_timer_create(&targs, &s_resume_timer);
  xTaskCreatePinnedToCore(radio_coex_task, "radio_coex", 3072, NULL, 4, NULL, 0);
  rtsp_events_register(on_rtsp_event, NULL);
  ESP_LOGI(TAG, "automatic AirPlay handover armed (resume delay %ds)",
           COEX_RESUME_DELAY_US / 1000000);
}

// ============================================================================
// Now-playing metadata
// ============================================================================
//
// Polls the station's AzuraCast JSON and republishes it as RTSP_EVENT_METADATA.
// The display component subscribes to that event bus already, so this needs no
// display changes at all — a2dp_sink.c publishes Bluetooth track info the same
// way.
//
// Runs only while the radio holds the output. An AirPlay session publishes its
// own metadata, and polling through it would overwrite the screen with whatever
// the station happens to be playing.

#define META_JSON_MAX 8192

// Fetch the whole body into buf. Returns length, or -1.
static int meta_fetch(char *buf, size_t cap) {
  esp_http_client_config_t cfg = {
      .url = CONFIG_RADIO_NOWPLAYING_URL,
      .timeout_ms = 5000,
  };
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) {
    return -1;
  }

  int total = -1;
  if (esp_http_client_open(c, 0) == ESP_OK) {
    esp_http_client_fetch_headers(c);
    if (esp_http_client_get_status_code(c) == 200) {
      total = 0;
      while ((size_t)total < cap - 1) {
        int n = esp_http_client_read(c, buf + total, (int)(cap - 1 - total));
        if (n <= 0) {
          break;
        }
        total += n;
      }
      buf[total > 0 ? total : 0] = 0;
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return total;
}

static void radio_meta_task(void *arg) {
  (void)arg;
  char *buf = heap_caps_malloc(META_JSON_MAX, MALLOC_CAP_SPIRAM);
  if (!buf) {
    ESP_LOGW(TAG, "metadata buffer alloc failed — no now-playing info");
    s_meta_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  char last_title[128] = {0};
  char last_artist[128] = {0};
  uint32_t wait_ms = 0; // poll immediately on start

  // An archive episode has its own metadata, published by archive.c. Polling the
  // live now-playing API over it would label a recorded show with whatever the
  // station happens to be broadcasting right now.
  if (s_archive_mode) {
    ESP_LOGI(TAG, "archive mode — now-playing poller idle");
    heap_caps_free(buf);
    s_meta_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  // Announce before the first fetch so the screen stops claiming "AirPlay
  // Ready" the moment audio starts. The real track replaces this within a
  // second, and it stays honest if the station's API is unreachable.
  {
    rtsp_event_data_t ev = {0};
    strlcpy(ev.metadata.title, CONFIG_RADIO_STATION_NAME,
            sizeof(ev.metadata.title));
    strlcpy(ev.metadata.station, CONFIG_RADIO_STATION_NAME,
            sizeof(ev.metadata.station));
    radio_emit(RTSP_EVENT_METADATA, &ev);
    radio_emit(RTSP_EVENT_PLAYING, NULL);
  }

  while (s_running) {
    if (wait_ms > 0) {
      vTaskDelay(pdMS_TO_TICKS(250));
      wait_ms = wait_ms > 250 ? wait_ms - 250 : 0;
      continue;
    }
    wait_ms = CONFIG_RADIO_NOWPLAYING_MS;

    int len = meta_fetch(buf, META_JSON_MAX);
    if (len <= 0) {
      ESP_LOGD(TAG, "now-playing fetch failed");
      continue;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
      ESP_LOGW(TAG, "now-playing JSON parse failed (%d bytes)", len);
      continue;
    }

    cJSON *np = cJSON_GetObjectItem(root, "now_playing");
    cJSON *song = np ? cJSON_GetObjectItem(np, "song") : NULL;
    if (song) {
      const cJSON *t = cJSON_GetObjectItem(song, "title");
      const cJSON *a = cJSON_GetObjectItem(song, "artist");
      const cJSON *al = cJSON_GetObjectItem(song, "album");
      const cJSON *dur = cJSON_GetObjectItem(np, "duration");
      // Station identity comes from the API rather than a compile-time string,
      // so a unit pointed at a different station labels itself correctly.
      cJSON *st = cJSON_GetObjectItem(root, "station");
      const cJSON *st_name = st ? cJSON_GetObjectItem(st, "name") : NULL;
      const cJSON *playlist = cJSON_GetObjectItem(np, "playlist");
      const cJSON *ela = cJSON_GetObjectItem(np, "elapsed");

      const char *title = cJSON_IsString(t) ? t->valuestring : "";
      const char *artist = cJSON_IsString(a) ? a->valuestring : "";
      const char *album = cJSON_IsString(al) ? al->valuestring : "";

      // Only publish on a track change: the display re-renders on every event and
      // a 15s heartbeat of identical text would restart its scroll animation.
      if (strcmp(title, last_title) != 0 || strcmp(artist, last_artist) != 0) {
        strlcpy(last_title, title, sizeof(last_title));
        strlcpy(last_artist, artist, sizeof(last_artist));

        rtsp_event_data_t ev = {0};
        strlcpy(ev.metadata.title, title, sizeof(ev.metadata.title));
        strlcpy(ev.metadata.artist, artist, sizeof(ev.metadata.artist));
        strlcpy(ev.metadata.album, album, sizeof(ev.metadata.album));
        ev.metadata.duration_secs =
            cJSON_IsNumber(dur) ? (uint32_t)dur->valuedouble : 0;
        ev.metadata.position_secs =
            cJSON_IsNumber(ela) ? (uint32_t)ela->valuedouble : 0;

        // Masthead carries the station only. The show/playlist goes on the
        // album row, where it reads as programme information rather than being
        // crammed into the station line.
        const char *st_txt = cJSON_IsString(st_name) && st_name->valuestring[0]
                                 ? st_name->valuestring
                                 : CONFIG_RADIO_STATION_NAME;
        strlcpy(ev.metadata.station, st_txt, sizeof(ev.metadata.station));
        if (cJSON_IsString(playlist) && playlist->valuestring[0]) {
          strlcpy(ev.metadata.album, playlist->valuestring,
                  sizeof(ev.metadata.album));
        }

        ESP_LOGI(TAG, "now playing: %s - %s [%s]", artist, title, st_txt);
        radio_emit(RTSP_EVENT_METADATA, &ev);
      }
    }
    cJSON_Delete(root);
  }

  heap_caps_free(buf);
  s_meta_task = NULL;
  vTaskDelete(NULL);
}
