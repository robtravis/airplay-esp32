#include "menu.h"

#include "display.h"
#include "esp_log.h"
#include "led_ring.h"
#include "playback_control.h"
#include "settings.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_RADIO_ENABLED
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
  LEVEL_INFO,
} menu_level_t;

#define MAX_ROWS 12
#define LABEL_MAX 24

static menu_level_t s_level = LEVEL_NONE;
static int s_sel = 0;

// Rendered rows for the current level, rebuilt on every change. Held as storage
// rather than pointers into other modules so nothing dangles mid-redraw.
static char s_rows[MAX_ROWS][LABEL_MAX];
static const char *s_row_ptrs[MAX_ROWS];
static int s_row_count = 0;
static char s_header[LABEL_MAX];

// Brightness steps offered on device. Fine control stays in the web UI.
static const int s_brightness_steps[] = {24, 48, 96, 160, 255};
#define N_BRIGHTNESS (int)(sizeof(s_brightness_steps) / sizeof(int))

static void row_add(const char *fmt, ...) {
  if (s_row_count >= MAX_ROWS) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_rows[s_row_count], LABEL_MAX, fmt, ap);
  va_end(ap);
  s_row_ptrs[s_row_count] = s_rows[s_row_count];
  s_row_count++;
}

static void build_and_draw(void) {
  s_row_count = 0;

  switch (s_level) {
  case LEVEL_ROOT:
    snprintf(s_header, sizeof(s_header), "MENU");
    row_add(BACK_LABEL);
    row_add("LED FX: %s", led_ring_effect_name(led_ring_get_effect()));
    row_add("BRIGHTNESS: %d", led_ring_get_scale());
#ifdef CONFIG_RADIO_ENABLED
    row_add("IDLE: %s", radio_source_get_mode() == SOURCE_MODE_RADIO
                            ? "RADIO"
                            : "SILENT");
#endif
    row_add("VOLUME: %d%%", playback_control_get_volume_percent());
    row_add("INFO");
    break;

  case LEVEL_LED_FX:
    snprintf(s_header, sizeof(s_header), "LED FX");
    row_add(BACK_LABEL);
    for (int i = 0; i < LED_FX_COUNT; i++) {
      // Mark the active one: with no cursor memory across opens, the list has to
      // say which effect is currently running.
      row_add("%s%s", i == led_ring_get_effect() ? "* " : "  ",
              led_ring_effect_name(i));
    }
    break;

  case LEVEL_BRIGHTNESS:
    snprintf(s_header, sizeof(s_header), "BRIGHTNESS");
    row_add(BACK_LABEL);
    for (int i = 0; i < N_BRIGHTNESS; i++) {
      row_add("%s%d", s_brightness_steps[i] == led_ring_get_scale() ? "* " : "  ",
              s_brightness_steps[i]);
    }
    break;

  case LEVEL_SOURCE:
    snprintf(s_header, sizeof(s_header), "WHEN IDLE");
    row_add(BACK_LABEL);
#ifdef CONFIG_RADIO_ENABLED
    row_add("%sPLAY RADIO",
            radio_source_get_mode() == SOURCE_MODE_RADIO ? "* " : "  ");
    row_add("%sSTAY SILENT",
            radio_source_get_mode() == SOURCE_MODE_AIRPLAY ? "* " : "  ");
#endif
    break;

  case LEVEL_INFO: {
    snprintf(s_header, sizeof(s_header), "INFO");
    row_add(BACK_LABEL);
    char name[40] = {0};
    if (settings_get_device_name(name, sizeof(name)) == ESP_OK) {
      row_add("%s", name);
    }
    row_add("VOL %d%%", playback_control_get_volume_percent());
    row_add("FX %s", led_ring_effect_name(led_ring_get_effect()));
    break;
  }

  case LEVEL_NONE:
    display_menu_hide();
    return;
  }

  if (s_sel >= s_row_count) {
    s_sel = s_row_count - 1;
  }
  if (s_sel < 0) {
    s_sel = 0;
  }
  display_menu_show(s_header, s_row_ptrs, s_row_count, s_sel);
}

void menu_init(void) { s_level = LEVEL_NONE; }

bool menu_is_open(void) { return s_level != LEVEL_NONE; }

void menu_back_or_open(void) {
  switch (s_level) {
  case LEVEL_NONE:
    s_level = LEVEL_ROOT;
    s_sel = 1; // skip < BACK — nobody opens a menu to leave it
    break;
  case LEVEL_ROOT:
    s_level = LEVEL_NONE;
    break;
  default:
    s_level = LEVEL_ROOT;
    s_sel = 1;
    break;
  }
  ESP_LOGI(TAG, "level -> %d", (int)s_level);
  build_and_draw();
}

void menu_scroll(int detents) {
  if (s_level == LEVEL_NONE || s_row_count == 0) {
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
}

void menu_select(void) {
  if (s_level == LEVEL_NONE) {
    return;
  }
  if (s_sel == 0) { // < BACK
    menu_back_or_open();
    return;
  }

  switch (s_level) {
  case LEVEL_ROOT:
    switch (s_sel) {
    case 1: s_level = LEVEL_LED_FX; s_sel = 1; break;
    case 2: s_level = LEVEL_BRIGHTNESS; s_sel = 1; break;
#ifdef CONFIG_RADIO_ENABLED
    case 3: s_level = LEVEL_SOURCE; s_sel = 1; break;
    case 4: break; // VOLUME is a readout; the encoder changes it when closed
    case 5: s_level = LEVEL_INFO; s_sel = 1; break;
#else
    case 3: break;
    case 4: s_level = LEVEL_INFO; s_sel = 1; break;
#endif
    default: break;
    }
    break;

  case LEVEL_LED_FX:
    led_ring_set_effect(s_sel - 1); // row 0 is < BACK
    break;

  case LEVEL_BRIGHTNESS:
    led_ring_set_scale(s_brightness_steps[s_sel - 1]);
    break;

#ifdef CONFIG_RADIO_ENABLED
  case LEVEL_SOURCE:
    radio_source_set_mode(s_sel == 1 ? SOURCE_MODE_RADIO : SOURCE_MODE_AIRPLAY);
    break;
#endif

  default:
    break;
  }
  build_and_draw();
}
