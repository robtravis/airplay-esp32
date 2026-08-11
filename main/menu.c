#include "menu.h"

#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_ring.h"
#include "playback_control.h"
#include "settings.h"
#include "wifi.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_RADIO_ENABLED
#include "radio/archive.h"
#include "radio/radio_source.h"
#endif

static const char *TAG = "menu";

// A "< BACK" row sits at index 0 of every level, exactly as the radio's lists
// did — with one button and one encoder there is no other way back out.
#define BACK_LABEL "< BACK"

typedef enum {
  LEVEL_NONE = 0, // closed
  LEVEL_ROOT,
  LEVEL_LED_FX,
  LEVEL_BRIGHTNESS,
  LEVEL_SOURCE,
  LEVEL_ARCHIVE_SHOWS,
  LEVEL_ARCHIVE_EPISODES,
  LEVEL_INFO,
  LEVEL_NETWORK,
  LEVEL_FORGET_CONFIRM,
} menu_level_t;

// Each row carries what it does, rather than the caller re-deriving it from the
// row index. Index arithmetic broke as soon as rows became conditional.
typedef enum {
  ACT_NONE = 0,
  ACT_BACK,
  ACT_OPEN_LEVEL,
  ACT_SET_FX,
  ACT_SET_SCALE,
  ACT_SET_IDLE_MODE,
  ACT_OPEN_SHOW,
  ACT_PLAY_EPISODE,
  ACT_STOP_ARCHIVE,
  ACT_FORGET_NETWORK,
} action_t;

// Must hold the whole archive show list plus "< BACK": 40 shows truncated at 16
// would have hidden most of the catalogue with no indication.
#define MAX_ROWS  48
#define LABEL_MAX 26

static menu_level_t s_level = LEVEL_NONE;
static int s_sel = 0;
static int s_show = -1; // show being browsed in LEVEL_ARCHIVE_EPISODES

static char s_rows[MAX_ROWS][LABEL_MAX];
static const char *s_row_ptrs[MAX_ROWS];
static action_t s_row_action[MAX_ROWS];
static int s_row_payload[MAX_ROWS];
static int s_row_count = 0;
static char s_header[LABEL_MAX];

// The archive fetch completes on its own task and wants to redraw the list, so
// every entry point is serialised.
static SemaphoreHandle_t s_lock = NULL;
#define MENU_LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define MENU_UNLOCK() xSemaphoreGive(s_lock)

static const int s_brightness_steps[] = {24, 48, 96, 160, 255};
#define N_BRIGHTNESS (int)(sizeof(s_brightness_steps) / sizeof(int))

static void row_add(action_t act, int payload, const char *fmt, ...) {
  if (s_row_count >= MAX_ROWS) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_rows[s_row_count], LABEL_MAX, fmt, ap);
  va_end(ap);
  s_row_ptrs[s_row_count] = s_rows[s_row_count];
  s_row_action[s_row_count] = act;
  s_row_payload[s_row_count] = payload;
  s_row_count++;
}

static void build_and_draw(void) {
  s_row_count = 0;

  switch (s_level) {
  case LEVEL_ROOT:
    snprintf(s_header, sizeof(s_header), "MENU");
    row_add(ACT_BACK, 0, BACK_LABEL);
#ifdef CONFIG_RADIO_ENABLED
    row_add(ACT_OPEN_LEVEL, LEVEL_ARCHIVE_SHOWS, "ARCHIVE SHOWS");
    if (archive_is_playing()) {
      row_add(ACT_STOP_ARCHIVE, 0, "BACK TO LIVE");
    }
#endif
    row_add(ACT_OPEN_LEVEL, LEVEL_LED_FX, "LED FX: %s",
            led_ring_effect_name(led_ring_get_effect()));
    row_add(ACT_OPEN_LEVEL, LEVEL_BRIGHTNESS, "BRIGHTNESS: %d",
            led_ring_get_scale());
#ifdef CONFIG_RADIO_ENABLED
    row_add(ACT_OPEN_LEVEL, LEVEL_SOURCE, "IDLE: %s",
            radio_source_get_mode() == SOURCE_MODE_RADIO ? "RADIO" : "SILENT");
#endif
    row_add(ACT_NONE, 0, "VOLUME: %d%%",
            playback_control_get_volume_percent());
    row_add(ACT_OPEN_LEVEL, LEVEL_NETWORK, "NETWORK");
    row_add(ACT_OPEN_LEVEL, LEVEL_INFO, "INFO");
    break;

  case LEVEL_LED_FX:
    snprintf(s_header, sizeof(s_header), "LED FX");
    row_add(ACT_BACK, 0, BACK_LABEL);
    for (int i = 0; i < LED_FX_COUNT; i++) {
      // Mark the running one: the cursor does not remember where it was, so the
      // list has to say what is active.
      row_add(ACT_SET_FX, i, "%s%s", i == led_ring_get_effect() ? "* " : "  ",
              led_ring_effect_name(i));
    }
    break;

  case LEVEL_BRIGHTNESS:
    snprintf(s_header, sizeof(s_header), "BRIGHTNESS");
    row_add(ACT_BACK, 0, BACK_LABEL);
    for (int i = 0; i < N_BRIGHTNESS; i++) {
      row_add(ACT_SET_SCALE, s_brightness_steps[i], "%s%d",
              s_brightness_steps[i] == led_ring_get_scale() ? "* " : "  ",
              s_brightness_steps[i]);
    }
    break;

#ifdef CONFIG_RADIO_ENABLED
  case LEVEL_SOURCE:
    snprintf(s_header, sizeof(s_header), "WHEN IDLE");
    row_add(ACT_BACK, 0, BACK_LABEL);
    row_add(ACT_SET_IDLE_MODE, SOURCE_MODE_RADIO, "%sPLAY RADIO",
            radio_source_get_mode() == SOURCE_MODE_RADIO ? "* " : "  ");
    row_add(ACT_SET_IDLE_MODE, SOURCE_MODE_AIRPLAY, "%sSTAY SILENT",
            radio_source_get_mode() == SOURCE_MODE_AIRPLAY ? "* " : "  ");
    break;

  case LEVEL_ARCHIVE_SHOWS:
    snprintf(s_header, sizeof(s_header), "ARCHIVE");
    row_add(ACT_BACK, 0, BACK_LABEL);
    if (archive_is_loading()) {
      row_add(ACT_NONE, 0, "LOADING...");
    } else if (!archive_is_ready()) {
      row_add(ACT_NONE, 0, "UNAVAILABLE");
    } else {
      for (int i = 0; i < archive_show_count(); i++) {
        row_add(ACT_OPEN_SHOW, i, "%s", archive_show_name(i));
      }
    }
    break;

  case LEVEL_ARCHIVE_EPISODES:
    snprintf(s_header, sizeof(s_header), "%s", archive_show_name(s_show));
    row_add(ACT_BACK, 0, BACK_LABEL);
    for (int i = 0; i < archive_episode_count(s_show); i++) {
      row_add(ACT_PLAY_EPISODE, i, "%s", archive_episode_label(s_show, i));
    }
    break;
#endif

  case LEVEL_NETWORK: {
    snprintf(s_header, sizeof(s_header), "NETWORK");
    row_add(ACT_BACK, 0, BACK_LABEL);
    // The address, on the device itself. Telling someone to "find the IP" is not
    // an instruction a gift recipient can follow.
    char ip[24] = {0};
    if (wifi_is_connected() && wifi_get_ip_str(ip, sizeof(ip)) == ESP_OK) {
      row_add(ACT_NONE, 0, "%s", ip);
      char host[33] = {0};
      wifi_get_hostname(host, sizeof(host));
      row_add(ACT_NONE, 0, "%s.local", host);
    } else {
      row_add(ACT_NONE, 0, "NOT CONNECTED");
    }
    row_add(ACT_OPEN_LEVEL, LEVEL_FORGET_CONFIRM, "FORGET NETWORK");
    break;
  }

  case LEVEL_FORGET_CONFIRM:
    // Two steps, because this is one click from a list people browse casually and
    // it cannot be undone without re-entering the WiFi password.
    snprintf(s_header, sizeof(s_header), "FORGET WIFI?");
    row_add(ACT_BACK, 0, "NO, KEEP IT");
    row_add(ACT_FORGET_NETWORK, 0, "YES, FORGET");
    break;

  case LEVEL_INFO: {
    snprintf(s_header, sizeof(s_header), "INFO");
    row_add(ACT_BACK, 0, BACK_LABEL);
    char name[40] = {0};
    if (settings_get_device_name(name, sizeof(name)) == ESP_OK) {
      row_add(ACT_NONE, 0, "%s", name);
    }
    row_add(ACT_NONE, 0, "VOL %d%%", playback_control_get_volume_percent());
    row_add(ACT_NONE, 0, "FX %s", led_ring_effect_name(led_ring_get_effect()));
    break;
  }

  case LEVEL_NONE:
    display_menu_hide();
    return;

  default:
    break;
  }

  if (s_sel >= s_row_count) {
    s_sel = s_row_count - 1;
  }
  if (s_sel < 0) {
    s_sel = 0;
  }
  display_menu_show(s_header, s_row_ptrs, s_row_count, s_sel);
}

#ifdef CONFIG_RADIO_ENABLED
/// Runs on the archive worker task when a fetch finishes.
static void on_archive_ready(void) {
  MENU_LOCK();
  if (s_level == LEVEL_ARCHIVE_SHOWS) {
    build_and_draw(); // replace "LOADING..." with the shows
  }
  MENU_UNLOCK();
}
#endif

void menu_init(void) {
  s_lock = xSemaphoreCreateMutex();
  s_level = LEVEL_NONE;
#ifdef CONFIG_RADIO_ENABLED
  archive_set_ready_cb(on_archive_ready);
#endif
}

bool menu_is_open(void) { return s_level != LEVEL_NONE; }

static void go_level(menu_level_t level) {
  s_level = level;
  s_sel = 1; // skip < BACK — nobody enters a list to leave it
  if (level == LEVEL_FORGET_CONFIRM) {
    s_sel = 0; // start on "NO, KEEP IT": a stray click must not wipe the network
  }
#ifdef CONFIG_RADIO_ENABLED
  if (level == LEVEL_ARCHIVE_SHOWS && !archive_is_ready() &&
      !archive_is_loading()) {
    // Only fetch when someone actually asks for the catalogue.
    archive_start_fetch();
  }
#endif
}

void menu_back_or_open(void) {
  MENU_LOCK();
  switch (s_level) {
  case LEVEL_NONE:
    go_level(LEVEL_ROOT);
    break;
  case LEVEL_ROOT:
    s_level = LEVEL_NONE;
    break;
  case LEVEL_ARCHIVE_EPISODES:
    go_level(LEVEL_ARCHIVE_SHOWS); // back to the show list, not all the way out
    break;
  case LEVEL_FORGET_CONFIRM:
    go_level(LEVEL_NETWORK);
    break;
  default:
    go_level(LEVEL_ROOT);
    break;
  }
  build_and_draw();
  MENU_UNLOCK();
}

void menu_scroll(int detents) {
  MENU_LOCK();
  if (s_level == LEVEL_NONE || s_row_count == 0) {
    MENU_UNLOCK();
    return;
  }
  s_sel += detents;
  // Clamp rather than wrap: wrapping past the ends of a list you cannot see all
  // of is disorienting.
  if (s_sel < 0) {
    s_sel = 0;
  }
  if (s_sel >= s_row_count) {
    s_sel = s_row_count - 1;
  }
  build_and_draw();
  MENU_UNLOCK();
}

void menu_select(void) {
  MENU_LOCK();
  if (s_level == LEVEL_NONE || s_sel < 0 || s_sel >= s_row_count) {
    MENU_UNLOCK();
    return;
  }

  action_t act = s_row_action[s_sel];
  int payload = s_row_payload[s_sel];

  switch (act) {
  case ACT_BACK:
    MENU_UNLOCK();
    menu_back_or_open();
    return;

  case ACT_OPEN_LEVEL:
    go_level((menu_level_t)payload);
    break;

  case ACT_SET_FX:
    led_ring_set_effect(payload);
    break;

  case ACT_SET_SCALE:
    led_ring_set_scale(payload);
    break;

#ifdef CONFIG_RADIO_ENABLED
  case ACT_SET_IDLE_MODE:
    radio_source_set_mode((uint8_t)payload);
    break;

  case ACT_OPEN_SHOW:
    s_show = payload;
    go_level(LEVEL_ARCHIVE_EPISODES);
    break;

  case ACT_PLAY_EPISODE:
    ESP_LOGI(TAG, "play show %d episode %d", s_show, payload);
    archive_play(s_show, payload);
    // Close the menu so the episode's own screen is visible.
    s_level = LEVEL_NONE;
    break;

  case ACT_STOP_ARCHIVE:
    archive_stop();
    s_level = LEVEL_NONE;
    break;
#endif

  case ACT_FORGET_NETWORK:
    ESP_LOGW(TAG, "forget network confirmed from the device menu");
    s_level = LEVEL_NONE; // the setup screen replaces the menu
    build_and_draw();
    MENU_UNLOCK();
    // Async and outside the lock: it must not run on the encoder task (too
    // little stack) nor with the menu mutex held.
    wifi_forget_network_async();
    return;

  case ACT_NONE:
  default:
    break;
  }

  build_and_draw();
  MENU_UNLOCK();
}
