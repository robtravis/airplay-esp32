/**
 * @file display_st7789.c
 * @brief ST7789 TFT display driver using esp_lcd + LVGL 9 (esp_lvgl_port)
 *
 * Implements the display_init() API for ST7789-based TFT displays.
 * Display: 320x170 pixels, landscape orientation, SPI interface.
 *
 * Background image is loaded at startup from SPIFFS (/spiffs/bg/background.bin)
 * — a raw RGB565 little-endian binary file. If the file is absent, the display
 * initialises normally with a blank (black) background. The background can be
 * updated without reflashing by uploading a new file via the HTTP file API.
 *
 * GPIO assignments (configured via sdkconfig / menuconfig):
 *   CLK  -> CONFIG_DISPLAY_SPI_CLK  (default 18)
 *   MOSI -> CONFIG_DISPLAY_SPI_MOSI (default 17)
 *   CS   -> CONFIG_DISPLAY_SPI_CS   (default 15)
 *   DC   -> CONFIG_DISPLAY_SPI_DC   (default 16)
 *   RST  -> CONFIG_DISPLAY_SPI_RST  (default 21)
 *   BL   -> CONFIG_DISPLAY_BL_GPIO  (default 38)
 */

#include "display.h"
#include "audio_output.h"
#include "board_common.h"
#include "audio_vis.h"
#include "playback_control.h"
#include "rtsp_events.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "display_st7789";

// Bitcount Single, converted from the OFL variable TTF in fonts/. A dot-matrix
// face: it keeps the pixel character without unscii's weight, and it is narrower
// than Silkscreen so a long track title fits before it has to scroll.
//
// Converted at 4bpp, not 1bpp: Bitcount's elements are round dots, and hard
// on/off pixels square them off into exactly the blockiness it avoids.
//
// Symbol glyphs (the bolt) are outside the converted ASCII range, so the bolt
// label alone stays on montserrat — it is an icon, not type.
LV_FONT_DECLARE(bitcount_16);
LV_FONT_DECLARE(bitcount_24);
LV_FONT_DECLARE(bitcount_32);

// ============================================================================
// Hardware configuration
// ============================================================================

#define DISPLAY_WIDTH      CONFIG_DISPLAY_ST7789_WIDTH
#define DISPLAY_HEIGHT     CONFIG_DISPLAY_ST7789_HEIGHT
#define LCD_HOST           SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define DRAW_BUF_LINES     10

// Background image path on SPIFFS
#define BG_SPIFFS_PATH   "/spiffs/bg/background.bin"
#define BG_EXPECTED_SIZE ((long)DISPLAY_WIDTH * DISPLAY_HEIGHT * 2) // RGB565

// ============================================================================
// Layout constants
// ============================================================================
// Margins match the radio firmware's DISPLAY_MARGIN of 8 (bezel inset) rather
// than the stock 22, which wastes horizontal room on a 320px-wide panel.
#define X_MARGIN   8
#define X_MARGIN_R (-8)
// Station line sits at the very top as a masthead, with the track below it.
// Row heights assume ~1.2x the font size: 14pt ~ 17px, 18pt ~ 22px, 28pt ~ 34px.
#define Y_STATION  2
// Width reserved for the bolt glyph plus its trailing space.
#define BOLT_W     22
#define MASTHEAD_H 20
// Long enough to read, short enough not to delay a device whose whole point is
// playing on power-up.
#define SPLASH_MS  2500
#define Y_TITLE    24
#define Y_ARTIST   58
#define Y_ALBUM    82
// Keep the status controls on-screen for both the original 320x170 ST7789
// layout and the Waveshare 240x240 panel.
#define Y_PROGRESS ((DISPLAY_HEIGHT >= 220) ? 148 : 114)
#define Y_TIME     (Y_PROGRESS + 18)
#define Y_STATUS   ((DISPLAY_HEIGHT >= 220) ? 188 : (DISPLAY_HEIGHT - 18))
#define BAR_HEIGHT 12

// ── Vibe Radio palette ─────────────────────────────────────────────────────────
// Matches the radio firmware's brand colours so this device looks like the rest
// of the family. RGB values are the hex comments from its config.h, converted
// from the RGB565 constants it uses:
//   C_CYAN        0x07FF  #00FFFF  primary — track title
//   C_YELLOW      0xFFE0  #FFFF00  artist, and the lightning bolt
//   C_MAGENTA     0xF81F  #FF00FF  accents, alerts
//   C_MAGENTA_DIM 0x780F  #780080
//   C_CYAN_DIM    0x0398  #007070  secondary text
//   C_DARK_GREY   0x2104           dividers, progress trough
#define VIBE_CYAN        lv_color_make(0x00, 0xFF, 0xFF)
#define VIBE_CYAN_DIM    lv_color_make(0x00, 0x70, 0x70)
#define VIBE_YELLOW      lv_color_make(0xFF, 0xFF, 0x00)
#define VIBE_MAGENTA     lv_color_make(0xFF, 0x00, 0xFF)
#define VIBE_MAGENTA_DIM lv_color_make(0x78, 0x00, 0x80)
#define VIBE_DARK_GREY   lv_color_make(0x21, 0x21, 0x21)
// Mid tints: knocked back from full brightness but still clearly brighter than
// the progress bar, which is VIBE_MAGENTA_DIM at 70% opacity.
#define VIBE_CYAN_MID    lv_color_make(0x00, 0xC8, 0xC8)
#define VIBE_YELLOW_MID  lv_color_make(0xC8, 0xC8, 0x00)

// Cyan through yellow to magenta across the spectrum — the Vibe palette read as a
// ramp, so the bars belong to the same design as the text.
#define VIS_COLOR(i)                                                           \
  ((i) < VIS_BARS / 3 ? VIBE_CYAN                                              \
                      : ((i) < (2 * VIS_BARS) / 3 ? VIBE_YELLOW_MID            \
                                                  : VIBE_MAGENTA))

// ============================================================================
// Display state
// ============================================================================

typedef enum {
  DISPLAY_STATE_SPLASH,
  DISPLAY_STATE_MENU,
  DISPLAY_STATE_STANDBY,
  DISPLAY_STATE_SETUP,
  DISPLAY_STATE_CONNECTED,
  DISPLAY_STATE_PLAYING,
  DISPLAY_STATE_PAUSED,
} display_state_t;

static struct {
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  char station[METADATA_STRING_MAX];
  char setup_ssid[METADATA_STRING_MAX];
  char setup_ip[24];
  uint32_t duration_secs;
  uint32_t position_secs;
  display_state_t state;
  volatile bool dirty;  // written by RTSP callback, polled by display_task
  int64_t sync_time_us; // 64-bit — torn reads would cause position jumps
} s_display;

// Protects s_display against concurrent access from the RTSP event callback
// thread and the display_task. Must be held around any multi-field read or
// write (strings, sync_time_us, state+position together). Dirty is volatile
// so the polling loop picks up the flag without holding the mutex.
static SemaphoreHandle_t s_state_mutex = NULL;

#define STATE_LOCK()   xSemaphoreTake(s_state_mutex, portMAX_DELAY)
#define STATE_UNLOCK() xSemaphoreGive(s_state_mutex)

// ============================================================================
// LVGL handles and widgets
// ============================================================================

static lv_display_t *s_lvgl_disp = NULL;

static uint8_t *s_bg_buf = NULL;
static lv_image_dsc_t s_bg_dsc;

static lv_obj_t *s_label_title = NULL;
static lv_obj_t *s_label_artist = NULL;
static lv_obj_t *s_label_album = NULL;
static lv_obj_t *s_label_muted = NULL;
static lv_obj_t *s_label_status = NULL;
static lv_obj_t *s_label_station = NULL;
static lv_obj_t *s_label_bolt = NULL;
// Splash widgets, centred and independent of the now-playing rows so the main
// layout does not have to be re-aligned for two seconds of branding.
static lv_obj_t *s_label_splash_1 = NULL;
static lv_obj_t *s_label_splash_2 = NULL;
static lv_obj_t *s_label_splash_3 = NULL;
static int64_t s_splash_until_us = 0;

// Menu widgets. A fixed set of row labels rather than lv_list: the rows never
// change count, so recycling labels avoids allocating and freeing widgets on
// every scroll tick.
#define MENU_VISIBLE_ROWS 6
#define MENU_ROW_H        18
static lv_obj_t *s_menu_box = NULL;
static lv_obj_t *s_menu_header = NULL;
static lv_obj_t *s_menu_rows[MENU_VISIBLE_ROWS];
// The masthead container has to be hidden while the menu is up, so keep a handle.
static lv_obj_t *s_masthead = NULL;
static lv_obj_t *s_bar_progress = NULL;

// Visualiser: a dot matrix, to match Bitcount's dot-matrix type rather than
// fight it. Individual objects rather than a canvas because LVGL repaints only
// what changed — a dot that stays lit costs nothing, and there is no full-strip
// blit. The original radio's stutter was a 43KB sprite push at 25Hz starving the
// audio pump; here a typical frame touches a handful of 8x6 dots.
//
// Colour runs bottom to top by intensity — green, amber, red — which is the
// convention every hi-fi spectrum analyser used, and what the reference units do.
// Height alone tells you how loud a band is, independent of which band it is.
#define VIS_COLS    AUDIO_VIS_BANDS
// 15 fine dashes rather than 11 chunky dots: the reference display's segments are
// thin horizontal bars in a dense grid, which is what makes it read as a
// fluorescent panel instead of a row of LEDs. More rows also means more visible
// movement for the same height.
#define VIS_ROWS    13
#define VIS_DOT_W   20 // of a 30px pitch, so columns are clearly separate
#define VIS_DOT_H   3
#define VIS_PITCH_Y 5 // 2px gaps: at 1px the segments merged into hatching
#define VIS_H       (VIS_ROWS * VIS_PITCH_Y)
// Sits on the bottom edge: the whole lower strip is the visualiser, and the
// progress bar and time readouts hide while it is on rather than colliding.
#define VIS_BOTTOM  (DISPLAY_HEIGHT - 1)
#define VIS_TOP     (VIS_BOTTOM - VIS_H)

static lv_obj_t *s_vis_dots[VIS_COLS][VIS_ROWS];
static lv_obj_t *s_vis_peak[VIS_COLS];
static bool s_vis_dot_lit[VIS_COLS][VIS_ROWS];
static int s_vis_peak_row[VIS_COLS];
static float s_vis_peak_f[VIS_COLS];
static bool s_vis_enabled = false;

static void vis_hide_displaced(bool hide);

// Colour by height, in roughly even thirds so colour shows on ordinary material
// rather than only on peaks. Expressed as rows counted DOWN FROM THE TOP, so the
// proportions survive a change to VIS_ROWS.
#define VIS_OVER_ROWS   4 // magenta: top 4 of 13
#define VIS_YELLOW_ROWS 8 // yellow starts at row 5

static inline lv_color_t vis_row_color(int row) {
  if (row >= VIS_ROWS - VIS_OVER_ROWS) {
    return VIBE_MAGENTA;
  }
  if (row >= VIS_ROWS - VIS_YELLOW_ROWS) {
    return VIBE_YELLOW;
  }
  return VIBE_CYAN;
}
static lv_obj_t *s_label_time_elapsed = NULL;
static lv_obj_t *s_label_time_remaining = NULL;
static lv_obj_t *s_label_battery = NULL;
static lv_obj_t *s_label_volume = NULL;

// ============================================================================
// Background loading
// ============================================================================

static bool bg_load_from_spiffs(void) {
  FILE *f = fopen(BG_SPIFFS_PATH, "rb");
  if (!f) {
    ESP_LOGI(TAG, "No background file at %s — using blank background",
             BG_SPIFFS_PATH);
    return false;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size != BG_EXPECTED_SIZE) {
    ESP_LOGW(TAG, "Background file wrong size: %ld (expected %d) — skipping",
             size, BG_EXPECTED_SIZE);
    fclose(f);
    return false;
  }

  s_bg_buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (!s_bg_buf) {
    ESP_LOGE(TAG, "Failed to allocate %ld bytes in PSRAM for background", size);
    fclose(f);
    return false;
  }

  if (fread(s_bg_buf, 1, size, f) != (size_t)size) {
    ESP_LOGE(TAG, "Failed to read background file");
    fclose(f);
    heap_caps_free(s_bg_buf);
    s_bg_buf = NULL;
    return false;
  }

  fclose(f);

  memset(&s_bg_dsc, 0, sizeof(s_bg_dsc));
  s_bg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  s_bg_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  s_bg_dsc.header.w = DISPLAY_WIDTH;
  s_bg_dsc.header.h = DISPLAY_HEIGHT;
  s_bg_dsc.data_size = BG_EXPECTED_SIZE;
  s_bg_dsc.data = s_bg_buf;

  ESP_LOGI(TAG, "Background loaded from SPIFFS (%ld bytes, PSRAM)", size);
  return true;
}

// ============================================================================
// Helpers
// ============================================================================

static uint32_t get_estimated_position(void) {
  uint32_t pos = s_display.position_secs;
  if (s_display.state == DISPLAY_STATE_PLAYING && s_display.sync_time_us > 0) {
    int64_t elapsed_us = esp_timer_get_time() - s_display.sync_time_us;
    uint32_t elapsed_secs = (uint32_t)(elapsed_us / 1000000);
    pos += elapsed_secs;
    if (s_display.duration_secs > 0 && pos > s_display.duration_secs) {
      pos = s_display.duration_secs;
    }
  }
  return pos;
}

static void format_time(uint32_t secs, char *buf, size_t len) {
  snprintf(buf, len, "%lu:%02lu", secs / 60, secs % 60);
}

static void format_remaining(uint32_t remaining_secs, char *buf, size_t len) {
  snprintf(buf, len, "-%lu:%02lu", remaining_secs / 60, remaining_secs % 60);
}

// ============================================================================
// UI creation - called once after LVGL init, with lock held
// ============================================================================

static void ui_create(void) {
  lv_obj_t *scr = lv_screen_active();

  // Always set a solid black background — ensures a clean screen whether or
  // not a background image was loaded from SPIFFS. Without this, LVGL's
  // default theme leaves the screen white with unrendered display RAM noise.
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Background image — loaded from SPIFFS at display_init() time.
  // If no file was found, s_bg_buf is NULL and we skip the image widget,
  // leaving the screen black. All other widgets render normally on top.
  if (s_bg_buf) {
    lv_obj_t *bg = lv_image_create(scr);
    lv_image_set_src(bg, &s_bg_dsc);
    lv_obj_align(bg, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  // Muted indicator — top-right corner, red, hidden by default
  s_label_muted = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_muted, &bitcount_16, 0);
  lv_obj_set_style_text_color(s_label_muted, VIBE_MAGENTA, 0);
  lv_obj_align(s_label_muted, LV_ALIGN_TOP_RIGHT, X_MARGIN_R, Y_STATION);
  lv_obj_add_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(s_label_muted, "MUTED");

  // Title — largest font, white, scrolling
  // Type scale. The 28/18/14 steps give the track title clear dominance at
  // arm's length, which 24/16/14 did not. Long strings scroll, so a bigger
  // title costs nothing but vertical space, and the rows above were reflowed
  // for it. Requires CONFIG_LV_FONT_MONTSERRAT_18/_28.
  const lv_font_t *title_font =
      (DISPLAY_HEIGHT >= 220) ? &bitcount_16 : &bitcount_32;
  const lv_font_t *artist_font = &bitcount_24;
  s_label_title = lv_label_create(scr);
  lv_obj_set_width(s_label_title, DISPLAY_WIDTH - (X_MARGIN * 2));
  lv_label_set_long_mode(s_label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_font(s_label_title, title_font, 0);
  lv_obj_set_style_text_color(s_label_title, VIBE_CYAN, 0);
  lv_obj_align(s_label_title, LV_ALIGN_TOP_LEFT, X_MARGIN, Y_TITLE);
  lv_label_set_text(s_label_title, "AirPlay Ready");

  // Artist — medium font, light grey, scrolling
  s_label_artist = lv_label_create(scr);
  lv_obj_set_width(s_label_artist, DISPLAY_WIDTH - (X_MARGIN * 2));
  lv_label_set_long_mode(s_label_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_font(s_label_artist, artist_font, 0);
  lv_obj_set_style_text_color(s_label_artist, VIBE_YELLOW, 0);
  lv_obj_align(s_label_artist, LV_ALIGN_TOP_LEFT, X_MARGIN, Y_ARTIST);
  lv_label_set_text(s_label_artist, "");

  // Album — small font, dimmer grey, scrolling
  s_label_album = lv_label_create(scr);
  lv_obj_set_width(s_label_album, DISPLAY_WIDTH - (X_MARGIN * 2) - 60);
  lv_label_set_long_mode(s_label_album, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_font(s_label_album, &bitcount_16, 0);
  // Same brightness as the station masthead: for radio this row carries the
  // show/playlist name, which is programme information rather than chrome.
  lv_obj_set_style_text_color(s_label_album, VIBE_CYAN_MID, 0);
  lv_obj_align(s_label_album, LV_ALIGN_TOP_LEFT, X_MARGIN, Y_ALBUM);
  lv_label_set_text(s_label_album, "");

  // Paused status indicator — right side at album row, amber
  s_label_status = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_status, &bitcount_16, 0);
  lv_obj_set_style_text_color(s_label_status, VIBE_MAGENTA, 0);
  lv_obj_align(s_label_status, LV_ALIGN_TOP_RIGHT, X_MARGIN_R, Y_ALBUM);
  lv_label_set_text(s_label_status, "");

  // Station masthead. The bolt is a separate label because LVGL 9 dropped
  // lv_label_set_recolor — two colours in one line means two widgets (or a
  // spangroup, which is heavier for a two-token string).
  // Flex row so the bolt and the name centre together as a unit. Aligning each
  // label separately cannot centre a two-colour line, and LVGL 9 dropped
  // lv_label_set_recolor.
  lv_obj_t *masthead = lv_obj_create(scr);
  s_masthead = masthead;
  lv_obj_remove_style_all(masthead);
  lv_obj_set_size(masthead, DISPLAY_WIDTH, MASTHEAD_H);
  lv_obj_align(masthead, LV_ALIGN_TOP_MID, 0, Y_STATION);
  lv_obj_set_flex_flow(masthead, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(masthead, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(masthead, 6, 0);
  lv_obj_clear_flag(masthead, LV_OBJ_FLAG_SCROLLABLE);

  s_label_bolt = lv_label_create(masthead);
  lv_obj_set_style_text_font(s_label_bolt, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_label_bolt, VIBE_YELLOW_MID, 0);
  lv_label_set_text(s_label_bolt, "");

  s_label_station = lv_label_create(masthead);
  lv_obj_set_style_text_font(s_label_station, &bitcount_16, 0);
  lv_obj_set_style_text_color(s_label_station, VIBE_CYAN_MID, 0);
  lv_label_set_text(s_label_station, "");

  // Boot splash — "VIBE" cyan over "RADIO" magenta with the URL beneath, the
  // same arrangement the radio firmware's display_boot() drew.
  s_label_splash_1 = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_splash_1, &bitcount_32, 0);
  lv_obj_set_style_text_color(s_label_splash_1, VIBE_CYAN, 0);
  lv_obj_align(s_label_splash_1, LV_ALIGN_CENTER, 0, -28);
  lv_label_set_text(s_label_splash_1, "VIBE");
  lv_obj_add_flag(s_label_splash_1, LV_OBJ_FLAG_HIDDEN);

  s_label_splash_2 = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_splash_2, &bitcount_16, 0);
  lv_obj_set_style_text_color(s_label_splash_2, VIBE_MAGENTA, 0);
  lv_obj_align(s_label_splash_2, LV_ALIGN_CENTER, 0, 4);
  lv_label_set_text(s_label_splash_2, "RADIO");
  lv_obj_add_flag(s_label_splash_2, LV_OBJ_FLAG_HIDDEN);

  s_label_splash_3 = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_splash_3, &bitcount_16, 0);
  lv_obj_set_style_text_color(s_label_splash_3, VIBE_CYAN_DIM, 0);
  lv_obj_align(s_label_splash_3, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_label_set_text(s_label_splash_3, "viberadio.one");
  lv_obj_add_flag(s_label_splash_3, LV_OBJ_FLAG_HIDDEN);

  // Progress bar — inset from border on both sides, rounded
  s_bar_progress = lv_bar_create(scr);
  lv_obj_set_size(s_bar_progress, DISPLAY_WIDTH - (X_MARGIN * 2), BAR_HEIGHT);
  lv_obj_align(s_bar_progress, LV_ALIGN_TOP_LEFT, X_MARGIN, Y_PROGRESS);
  lv_bar_set_range(s_bar_progress, 0, 100);
  lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s_bar_progress, VIBE_DARK_GREY, 0);
  lv_obj_set_style_bg_opa(s_bar_progress, LV_OPA_50, 0);
  // Dim magenta at 70% rather than full cyan: the bar is a background detail and
  // the bright cyan was pulling attention off the track title.
  lv_obj_set_style_bg_color(s_bar_progress, VIBE_MAGENTA_DIM,
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_bar_progress, LV_OPA_70, LV_PART_INDICATOR);
  lv_obj_set_style_radius(s_bar_progress, 3, 0);
  lv_obj_set_style_radius(s_bar_progress, 3, LV_PART_INDICATOR);

  // Elapsed time — below bar, left aligned
  s_label_time_elapsed = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_time_elapsed, &bitcount_16, 0);
  lv_obj_set_style_text_color(s_label_time_elapsed,
                              VIBE_CYAN_DIM, 0);
  lv_obj_align(s_label_time_elapsed, LV_ALIGN_TOP_LEFT, X_MARGIN, Y_TIME);
  lv_label_set_text(s_label_time_elapsed, "");

  // Remaining time — below bar, right aligned
  s_label_time_remaining = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_time_remaining, &bitcount_16, 0);
  lv_obj_set_style_text_color(s_label_time_remaining,
                              VIBE_CYAN_DIM, 0);
  lv_obj_align(s_label_time_remaining, LV_ALIGN_TOP_RIGHT, X_MARGIN_R, Y_TIME);
  lv_label_set_text(s_label_time_remaining, "");

  // Volume — status row, bottom-left
  s_label_volume = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_volume, &bitcount_16, 0);
  lv_obj_set_style_text_color(s_label_volume, VIBE_CYAN_DIM, 0);
  lv_obj_align(s_label_volume, LV_ALIGN_TOP_LEFT, X_MARGIN, Y_STATUS);
  lv_label_set_text(s_label_volume, "");

  // ── Visualiser ──────────────────────────────────────────────────────────────
  {
    int total_w = DISPLAY_WIDTH - (X_MARGIN * 2);
    int pitch_x = total_w / VIS_COLS;
    int x0 = X_MARGIN + (pitch_x - VIS_DOT_W) / 2;
    for (int c = 0; c < VIS_COLS; c++) {
      for (int r = 0; r < VIS_ROWS; r++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, VIS_DOT_W, VIS_DOT_H);
        // Row 0 is the bottom row, so level maps directly to rows lit upward.
        lv_obj_align(d, LV_ALIGN_TOP_LEFT, x0 + c * pitch_x,
                     VIS_BOTTOM - VIS_DOT_H - r * VIS_PITCH_Y);
        lv_obj_set_style_bg_color(d, vis_row_color(r), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        // Square: a 3px dash with rounded ends turns into a blur. The grid reads
        // as segments, which is the point.
        lv_obj_set_style_radius(d, 0, 0);
        // Bloom, for the look of a backlit panel rather than flat pixels. LVGL
        // draws this as a blurred box shadow in the segment's own colour, so lit
        // dashes bleed into each other the way phosphor or an LCD backlight does.
        // Kept modest: shadow rendering is the expensive part of LVGL drawing, and
        // there are 130 of these.
        lv_obj_set_style_shadow_width(d, 6, 0);
        lv_obj_set_style_shadow_color(d, vis_row_color(r), 0);
        lv_obj_set_style_shadow_opa(d, LV_OPA_40, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        s_vis_dots[c][r] = d;
        s_vis_dot_lit[c][r] = false;
      }
      // Peak marker: holds the recent maximum and falls slowly. Cheap, and it is
      // what makes the display read as dynamics rather than a level meter.
      lv_obj_t *pk = lv_obj_create(scr);
      lv_obj_remove_style_all(pk);
      lv_obj_set_size(pk, VIS_DOT_W, 1);
      lv_obj_align(pk, LV_ALIGN_TOP_LEFT, x0 + c * pitch_x, VIS_TOP);
      // Colour is set per-frame from the zone it lands in: a white dash hanging in
      // empty space above a short column read as debris rather than a peak.
      lv_obj_set_style_bg_color(pk, VIBE_CYAN, 0);
      lv_obj_set_style_bg_opa(pk, LV_OPA_COVER, 0);
      lv_obj_set_style_shadow_width(pk, 5, 0);
      lv_obj_set_style_shadow_opa(pk, LV_OPA_40, 0);
      lv_obj_add_flag(pk, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(pk, LV_OBJ_FLAG_SCROLLABLE);
      s_vis_peak[c] = pk;
      s_vis_peak_row[c] = -1;
      s_vis_peak_f[c] = 0.0f;
    }
  }

  // ── Menu ────────────────────────────────────────────────────────────────────
  s_menu_box = lv_obj_create(scr);
  lv_obj_remove_style_all(s_menu_box);
  lv_obj_set_size(s_menu_box, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  lv_obj_align(s_menu_box, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(s_menu_box, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_menu_box, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_menu_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_menu_box, LV_OBJ_FLAG_HIDDEN);

  s_menu_header = lv_label_create(s_menu_box);
  lv_obj_set_style_text_font(s_menu_header, &bitcount_16, 0);
  lv_obj_set_style_text_color(s_menu_header, VIBE_MAGENTA, 0);
  lv_obj_align(s_menu_header, LV_ALIGN_TOP_LEFT, X_MARGIN, 4);
  lv_label_set_text(s_menu_header, "MENU");

  for (int i = 0; i < MENU_VISIBLE_ROWS; i++) {
    s_menu_rows[i] = lv_label_create(s_menu_box);
    lv_obj_set_width(s_menu_rows[i], DISPLAY_WIDTH - (X_MARGIN * 2));
    lv_obj_set_style_text_font(s_menu_rows[i], &bitcount_16, 0);
    lv_obj_set_style_text_color(s_menu_rows[i], VIBE_CYAN_DIM, 0);
    // The highlight is a background on the selected row — cheaper than moving a
    // cursor widget and it survives text of any width.
    lv_obj_set_style_bg_color(s_menu_rows[i], VIBE_CYAN, 0);
    lv_obj_set_style_bg_opa(s_menu_rows[i], LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(s_menu_rows[i], 2, 0);
    lv_obj_align(s_menu_rows[i], LV_ALIGN_TOP_LEFT, X_MARGIN,
                 26 + i * MENU_ROW_H);
    lv_label_set_text(s_menu_rows[i], "");
  }

  // Battery — status row, bottom-right
  s_label_battery = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_battery, &bitcount_16, 0);
  lv_obj_set_style_text_color(s_label_battery, VIBE_CYAN_DIM, 0);
  lv_obj_align(s_label_battery, LV_ALIGN_TOP_RIGHT, X_MARGIN_R, Y_STATUS);
  lv_label_set_text(s_label_battery, "");
}

// Update the battery + volume status row. Called from ui_update() with the
// LVGL port lock already held.
static void ui_update_status_row(void) {
  // Volume — show as percentage, with the channel mode (L+R / L / R)
  const char *chan;
  switch (audio_output_get_channel_mode()) {
  case AUDIO_CHANNEL_LEFT:
    chan = "L";
    break;
  case AUDIO_CHANNEL_RIGHT:
    chan = "R";
    break;
  default:
    chan = "L+R";
    break;
  }
  char vol_str[24];
  snprintf(vol_str, sizeof(vol_str), LV_SYMBOL_VOLUME_MAX " %d%%  %s",
           playback_control_get_volume_percent(), chan);
  lv_label_set_text(s_label_volume, vol_str);

  // Battery — only if the board reports one
  int pct = 0;
  bool charging = false;
  if (board_battery_read(&pct, &charging)) {
    const char *icon;
    if (charging) {
      icon = LV_SYMBOL_USB; // plugged into USB / charging
    } else if (pct >= 80) {
      icon = LV_SYMBOL_BATTERY_FULL;
    } else if (pct >= 60) {
      icon = LV_SYMBOL_BATTERY_3;
    } else if (pct >= 40) {
      icon = LV_SYMBOL_BATTERY_2;
    } else if (pct >= 20) {
      icon = LV_SYMBOL_BATTERY_1;
    } else {
      icon = LV_SYMBOL_BATTERY_EMPTY;
    }

    char bat_str[16];
    snprintf(bat_str, sizeof(bat_str), "%s %d%%", icon, pct);
    lv_label_set_text(s_label_battery, bat_str);

    // Tint red when low and not charging
    lv_color_t color = (!charging && pct <= 20) ? lv_color_make(255, 60, 60)
                                                : VIBE_CYAN_DIM;
    lv_obj_set_style_text_color(s_label_battery, color, 0);
  } else {
    lv_label_set_text(s_label_battery, "");
  }
}

// ============================================================================
// UI update
// ============================================================================

static void ui_update(void) {
  // Snapshot shared state under s_state_mutex. This avoids torn reads of
  // sync_time_us (int64, two-word on Xtensa) and guarantees consistent
  // strings vs. state. The snapshot is then rendered without the state
  // mutex held, so the RTSP callback can't be blocked by LVGL rendering.
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  char station[METADATA_STRING_MAX];
  char setup_ssid[METADATA_STRING_MAX];
  char setup_ip[24];
  uint32_t duration_secs;
  uint32_t position_secs;
  int64_t sync_time_us;
  display_state_t state;

  STATE_LOCK();
  memcpy(title, s_display.title, sizeof(title));
  memcpy(artist, s_display.artist, sizeof(artist));
  memcpy(album, s_display.album, sizeof(album));
  memcpy(station, s_display.station, sizeof(station));
  memcpy(setup_ssid, s_display.setup_ssid, sizeof(setup_ssid));
  memcpy(setup_ip, s_display.setup_ip, sizeof(setup_ip));
  duration_secs = s_display.duration_secs;
  position_secs = s_display.position_secs;
  sync_time_us = s_display.sync_time_us;
  state = s_display.state;
  STATE_UNLOCK();

  // Defensive NUL termination — if the RTSP producer ever fills all
  // METADATA_STRING_MAX bytes without a terminator, lv_label_set_text
  // would read off the end.
  title[METADATA_STRING_MAX - 1] = '\0';
  artist[METADATA_STRING_MAX - 1] = '\0';
  album[METADATA_STRING_MAX - 1] = '\0';
  station[METADATA_STRING_MAX - 1] = '\0';
  setup_ssid[METADATA_STRING_MAX - 1] = '\0';
  setup_ip[sizeof(setup_ip) - 1] = '\0';

  if (!lvgl_port_lock(100)) {
    ESP_LOGW(TAG, "ui_update: lock timeout");
    return;
  }

  // The splash widgets overlay every other screen unless they are explicitly
  // hidden: LVGL keeps them on the screen object regardless of state, and the
  // per-state branches below only set text on the now-playing rows.
  if (state != DISPLAY_STATE_SPLASH) {
    lv_obj_add_flag(s_label_splash_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_label_splash_2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_label_splash_3, LV_OBJ_FLAG_HIDDEN);
  }

  // The menu is a full-screen overlay, so everything else must go behind it.
  if (s_menu_box) {
    if (state == DISPLAY_STATE_MENU) {
      lv_obj_clear_flag(s_menu_box, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_menu_box, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (s_masthead) {
    if (state == DISPLAY_STATE_MENU) {
      lv_obj_add_flag(s_masthead, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(s_masthead, LV_OBJ_FLAG_HIDDEN);
    }
  }

  switch (state) {
  case DISPLAY_STATE_MENU:
    // Rows are written by display_menu_show(); nothing to do per-frame.
    lv_obj_add_flag(s_bar_progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
    break;

  case DISPLAY_STATE_STANDBY:
    lv_label_set_text(s_label_title, "AirPlay Ready");
    vis_hide_displaced(s_vis_enabled);
    lv_label_set_text(s_label_station, "");
    lv_label_set_text(s_label_artist, "");
    lv_label_set_text(s_label_album, "");
    lv_label_set_text(s_label_status, "");
    lv_label_set_text(s_label_time_elapsed, "");
    lv_label_set_text(s_label_time_remaining, "");
    lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
    lv_obj_add_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
    break;

  case DISPLAY_STATE_SPLASH:
    lv_obj_clear_flag(s_label_splash_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_label_splash_2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_label_splash_3, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_label_bolt, "");
    lv_label_set_text(s_label_station, "");
    lv_label_set_text(s_label_title, "");
    lv_label_set_text(s_label_artist, "");
    lv_label_set_text(s_label_album, "");
    lv_label_set_text(s_label_status, "");
    lv_label_set_text(s_label_time_elapsed, "");
    lv_label_set_text(s_label_time_remaining, "");
    lv_obj_add_flag(s_bar_progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
    break;

  case DISPLAY_STATE_SETUP:
    lv_label_set_text(s_label_bolt, "");
    lv_label_set_text(s_label_station, LV_SYMBOL_WIFI " WIFI SETUP");
    lv_label_set_text(s_label_title, "Join WiFi");
    lv_label_set_text(s_label_artist, setup_ssid);
    lv_label_set_text_fmt(s_label_album, "then open %s", setup_ip);
    lv_label_set_text(s_label_status, "");
    lv_label_set_text(s_label_time_elapsed, "");
    lv_label_set_text(s_label_time_remaining, "");
    lv_obj_add_flag(s_bar_progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
    break;

  case DISPLAY_STATE_CONNECTED:
    lv_label_set_text(s_label_title, "Connected");
    vis_hide_displaced(s_vis_enabled);
    lv_label_set_text(s_label_station, "");
    lv_label_set_text(s_label_artist, "");
    lv_label_set_text(s_label_album, "");
    lv_label_set_text(s_label_status, "");
    lv_label_set_text(s_label_time_elapsed, "");
    lv_label_set_text(s_label_time_remaining, "");
    lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
    lv_obj_add_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
    break;

  case DISPLAY_STATE_PLAYING:
  case DISPLAY_STATE_PAUSED: {
    lv_label_set_text(s_label_title, title[0] ? title : "---");
    lv_label_set_text(s_label_artist, artist[0] ? artist : "");
    lv_label_set_text(s_label_album, album[0] ? album : "");
    // The visualiser owns the bottom strip; the bar and readouts stay hidden
    // while it is on rather than drawing through it.
    vis_hide_displaced(s_vis_enabled);
    lv_label_set_text(s_label_status,
                      state == DISPLAY_STATE_PAUSED ? "|| " : "");
    // LV_SYMBOL_CHARGE is the lightning bolt from LVGL's built-in symbol set,
    // which ships inside the montserrat fonts — no extra font needed.
    lv_label_set_text(s_label_bolt, station[0] ? LV_SYMBOL_CHARGE : "");
    lv_label_set_text(s_label_station, station[0] ? station : "");

    // Muted indicator
    if (playback_control_is_muted()) {
      lv_obj_clear_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
    }

    uint32_t pos = position_secs;
    if (state == DISPLAY_STATE_PLAYING && sync_time_us > 0) {
      int64_t elapsed_us = esp_timer_get_time() - sync_time_us;
      uint32_t elapsed_secs = (uint32_t)(elapsed_us / 1000000);
      pos += elapsed_secs;
      if (duration_secs > 0 && pos > duration_secs) {
        pos = duration_secs;
      }
    }

    if (duration_secs > 0) {
      int pct = (int)((uint64_t)pos * 100 / duration_secs);
      lv_bar_set_value(s_bar_progress, pct, LV_ANIM_OFF);

      char elapsed_str[12];
      format_time(pos, elapsed_str, sizeof(elapsed_str));
      lv_label_set_text(s_label_time_elapsed, elapsed_str);

      uint32_t remaining = (pos <= duration_secs) ? duration_secs - pos : 0;
      char remaining_str[16];
      format_remaining(remaining, remaining_str, sizeof(remaining_str));
      lv_label_set_text(s_label_time_remaining, remaining_str);
    } else {
      lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
      lv_label_set_text(s_label_time_elapsed, "");
      lv_label_set_text(s_label_time_remaining, "");
    }
    break;
  }
  }

  ui_update_status_row();

  lvgl_port_unlock();
}

// ============================================================================
// RTSP event callback
// ============================================================================

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)user_data;

  // All mutations of s_display happen under the state mutex so reads in
  // display_task see a consistent snapshot. The callback runs in the RTSP
  // event thread (not an ISR), so blocking on a FreeRTOS mutex is safe.
  STATE_LOCK();

  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    s_display.state = DISPLAY_STATE_CONNECTED;
    strlcpy(s_display.station, "AIRPLAY", sizeof(s_display.station));
    memset(s_display.title, 0, sizeof(s_display.title));
    memset(s_display.artist, 0, sizeof(s_display.artist));
    memset(s_display.album, 0, sizeof(s_display.album));
    s_display.duration_secs = 0;
    s_display.position_secs = 0;
    s_display.sync_time_us = 0;
    s_display.dirty = true;
    break;

  case RTSP_EVENT_PLAYING:
    s_display.state = DISPLAY_STATE_PLAYING;
    s_display.sync_time_us = esp_timer_get_time();
    s_display.dirty = true;
    break;

  case RTSP_EVENT_PAUSED:
    s_display.position_secs = get_estimated_position();
    s_display.sync_time_us = 0;
    s_display.state = DISPLAY_STATE_PAUSED;
    s_display.dirty = true;
    break;

  case RTSP_EVENT_DISCONNECTED:
    s_display.state = DISPLAY_STATE_STANDBY;
    memset(s_display.station, 0, sizeof(s_display.station));
    memset(s_display.title, 0, sizeof(s_display.title));
    memset(s_display.artist, 0, sizeof(s_display.artist));
    memset(s_display.album, 0, sizeof(s_display.album));
    s_display.duration_secs = 0;
    s_display.position_secs = 0;
    s_display.sync_time_us = 0;
    s_display.dirty = true;
    break;

  case RTSP_EVENT_METADATA:
    if (data) {
      bool track_changed = data->metadata.title[0] &&
                           strcmp(data->metadata.title, s_display.title) != 0;

      if (data->metadata.title[0]) {
        memcpy(s_display.title, data->metadata.title, METADATA_STRING_MAX);
        s_display.title[METADATA_STRING_MAX - 1] = '\0';
      }
      if (data->metadata.artist[0]) {
        memcpy(s_display.artist, data->metadata.artist, METADATA_STRING_MAX);
        s_display.artist[METADATA_STRING_MAX - 1] = '\0';
      }
      if (data->metadata.album[0]) {
        memcpy(s_display.album, data->metadata.album, METADATA_STRING_MAX);
        s_display.album[METADATA_STRING_MAX - 1] = '\0';
      }
      if (data->metadata.station[0]) {
        memcpy(s_display.station, data->metadata.station, METADATA_STRING_MAX);
        s_display.station[METADATA_STRING_MAX - 1] = '\0';
      }
      if (data->metadata.duration_secs) {
        s_display.duration_secs = data->metadata.duration_secs;
      }

      if (track_changed || data->metadata.position_secs ||
          s_display.position_secs == 0) {
        s_display.position_secs = data->metadata.position_secs;
      }

      s_display.sync_time_us = esp_timer_get_time();
      s_display.dirty = true;
    }
    break;
  }

  STATE_UNLOCK();
}

/// Redraw the matrix. Only dots whose state changed are touched, so a steady
/// signal costs almost nothing and LVGL has almost no dirty area to flush.
static void vis_update(void) {
  uint8_t bands[VIS_COLS];
  bool active = audio_vis_get_bands(bands, VIS_COLS);
  if (!lvgl_port_lock(20)) {
    return; // skip a frame rather than block the display task
  }
  for (int c = 0; c < VIS_COLS; c++) {
    int lit = active ? (bands[c] * VIS_ROWS + 127) / 255 : 0;

    for (int r = 0; r < VIS_ROWS; r++) {
      bool want = r < lit;
      if (want != s_vis_dot_lit[c][r]) {
        s_vis_dot_lit[c][r] = want;
        if (want) {
          lv_obj_clear_flag(s_vis_dots[c][r], LV_OBJ_FLAG_HIDDEN);
        } else {
          lv_obj_add_flag(s_vis_dots[c][r], LV_OBJ_FLAG_HIDDEN);
        }
      }
    }

    // Peak: jump up instantly, fall at a steady rate so it trails the music.
    float lvl = (float)lit;
    if (lvl > s_vis_peak_f[c]) {
      s_vis_peak_f[c] = lvl;
    } else {
      // Falls nearly as fast as the columns do. At 0.12 the marker lagged several
      // rows behind and hovered detached from its column.
      s_vis_peak_f[c] -= 0.30f;
      if (s_vis_peak_f[c] < 0.0f) {
        s_vis_peak_f[c] = 0.0f;
      }
    }
    int prow = (int)s_vis_peak_f[c] - 1;
    if (prow >= VIS_ROWS) {
      prow = VIS_ROWS - 1;
    }
    if (prow != s_vis_peak_row[c]) {
      s_vis_peak_row[c] = prow;
      if (prow < 1) {
        lv_obj_add_flag(s_vis_peak[c], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_clear_flag(s_vis_peak[c], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_vis_peak[c], vis_row_color(prow), 0);
        lv_obj_set_style_shadow_color(s_vis_peak[c], vis_row_color(prow), 0);
        lv_obj_set_y(s_vis_peak[c],
                     VIS_BOTTOM - VIS_DOT_H - prow * VIS_PITCH_Y - 2);
      }
    }
  }
  lvgl_port_unlock();
}

/// Widgets the visualiser displaces. The bottom strip is shared, so these hide
/// while it runs instead of drawing through it.
static void vis_hide_displaced(bool hide) {
  lv_obj_t *displaced[] = {s_bar_progress, s_label_time_elapsed,
                           s_label_time_remaining, s_label_volume,
                           s_label_battery};
  for (size_t i = 0; i < sizeof(displaced) / sizeof(displaced[0]); i++) {
    if (!displaced[i]) {
      continue;
    }
    if (hide) {
      lv_obj_add_flag(displaced[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(displaced[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void display_set_visualizer(bool enabled) {
  if (!s_state_mutex) {
    return;
  }
  s_vis_enabled = enabled;
  if (!lvgl_port_lock(100)) {
    return;
  }
  for (int c = 0; c < VIS_COLS; c++) {
    for (int r = 0; r < VIS_ROWS; r++) {
      lv_obj_add_flag(s_vis_dots[c][r], LV_OBJ_FLAG_HIDDEN);
      s_vis_dot_lit[c][r] = false;
    }
    lv_obj_add_flag(s_vis_peak[c], LV_OBJ_FLAG_HIDDEN);
    s_vis_peak_row[c] = -1;
    s_vis_peak_f[c] = 0.0f;
  }
  vis_hide_displaced(enabled);
  lvgl_port_unlock();
  STATE_LOCK();
  s_display.dirty = true;
  STATE_UNLOCK();
}

bool display_get_visualizer(void) { return s_vis_enabled; }

// ============================================================================
// Display task
// ============================================================================

static void display_task(void *pvParameters) {
  (void)pvParameters;

  while (1) {
    // Leave the splash on time. A real event (playback, setup) replaces it
    // earlier on its own — this only handles the quiet case.
    if (s_splash_until_us && esp_timer_get_time() > s_splash_until_us) {
      s_splash_until_us = 0;
      STATE_LOCK();
      if (s_display.state == DISPLAY_STATE_SPLASH) {
        s_display.state = DISPLAY_STATE_STANDBY;
        s_display.dirty = true;
      }
      STATE_UNLOCK();
    }

    // Consume dirty under the state mutex so a concurrent set in the
    // RTSP callback is never lost (clear-after-set ordering).
    bool need_update = false;
    display_state_t state;
    STATE_LOCK();
    if (s_display.dirty) {
      s_display.dirty = false;
      need_update = true;
    }
    state = s_display.state;
    STATE_UNLOCK();

    if (need_update) {
      ui_update();
    }

    // Visualiser frames. 50ms (20fps) reads as motion without the SPI traffic of
    // a full redraw: only bars that changed height are repainted.
    static TickType_t last_vis = 0;
    TickType_t vnow = xTaskGetTickCount();
    if (s_vis_enabled && state == DISPLAY_STATE_PLAYING &&
        (vnow - last_vis) >= pdMS_TO_TICKS(50)) {
      last_vis = vnow;
      vis_update();
    }

    static TickType_t last_progress_update = 0;
    TickType_t now = xTaskGetTickCount();
    if (state == DISPLAY_STATE_PLAYING &&
        (now - last_progress_update) >= pdMS_TO_TICKS(1000)) {
      last_progress_update = now;
      ui_update();
    }

    // Refresh battery + volume + channel mode periodically even when idle.
    // These are not tied to RTSP events; poll at 1s so an interactive change
    // (volume, double-click channel toggle) shows up promptly.
    static TickType_t last_status_update = 0;
    if (!need_update && (now - last_status_update) >= pdMS_TO_TICKS(1000)) {
      last_status_update = now;
      if (lvgl_port_lock(100)) {
        ui_update_status_row();
        lvgl_port_unlock();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// ============================================================================
// Initialization
// ============================================================================

void display_menu_show(const char *header, const char **items, int count,
                       int sel) {
  if (!s_state_mutex || !s_menu_box) {
    return;
  }
  if (!lvgl_port_lock(100)) {
    ESP_LOGW(TAG, "menu_show: lock timeout");
    return;
  }

  // Window the list so the selection is always visible: scroll only once the
  // cursor would leave the visible rows, which keeps the list still while
  // moving within a page.
  int first = 0;
  if (count > MENU_VISIBLE_ROWS) {
    first = sel - (MENU_VISIBLE_ROWS / 2);
    if (first < 0) {
      first = 0;
    }
    if (first > count - MENU_VISIBLE_ROWS) {
      first = count - MENU_VISIBLE_ROWS;
    }
  }

  lv_label_set_text(s_menu_header, header ? header : "MENU");
  for (int row = 0; row < MENU_VISIBLE_ROWS; row++) {
    int idx = first + row;
    if (idx < count && items[idx]) {
      lv_label_set_text(s_menu_rows[row], items[idx]);
      bool on = (idx == sel);
      lv_obj_set_style_text_color(s_menu_rows[row],
                                  on ? lv_color_black() : VIBE_CYAN_DIM, 0);
      lv_obj_set_style_bg_opa(s_menu_rows[row], on ? LV_OPA_COVER : LV_OPA_TRANSP,
                              0);
    } else {
      lv_label_set_text(s_menu_rows[row], "");
      lv_obj_set_style_bg_opa(s_menu_rows[row], LV_OPA_TRANSP, 0);
    }
  }
  lvgl_port_unlock();

  STATE_LOCK();
  s_display.state = DISPLAY_STATE_MENU;
  s_display.dirty = true;
  STATE_UNLOCK();
}

void display_menu_hide(void) {
  if (!s_state_mutex) {
    return;
  }
  STATE_LOCK();
  if (s_display.state == DISPLAY_STATE_MENU) {
    // Back to the track screen if something is playing, else standby.
    s_display.state =
        s_display.title[0] ? DISPLAY_STATE_PLAYING : DISPLAY_STATE_STANDBY;
    s_display.dirty = true;
  }
  STATE_UNLOCK();
}

void display_show_setup(const char *ssid, const char *ip) {
  if (!s_state_mutex) {
    return; // display not up yet
  }
  STATE_LOCK();
  strlcpy(s_display.setup_ssid, ssid ? ssid : "",
          sizeof(s_display.setup_ssid));
  strlcpy(s_display.setup_ip, ip ? ip : "192.168.4.1",
          sizeof(s_display.setup_ip));
  s_display.state = DISPLAY_STATE_SETUP;
  s_display.dirty = true;
  STATE_UNLOCK();
}

void display_clear_setup(void) {
  if (!s_state_mutex) {
    return;
  }
  STATE_LOCK();
  if (s_display.state == DISPLAY_STATE_SETUP) {
    s_display.state = DISPLAY_STATE_STANDBY;
    s_display.dirty = true;
  }
  STATE_UNLOCK();
}

void display_init(void *bus) {
  s_state_mutex = xSemaphoreCreateMutex();
  assert(s_state_mutex != NULL);

  // The main/main.c contract (from PR #59) is:
  //   bus != NULL → a pre-initialised spi_host_device_t passed as
  //                 (void*)(intptr_t)host. Use it and skip our own
  //                 spi_bus_initialize() to share the board's SPI bus.
  //   bus == NULL → fall back to initialising our own SPI bus from the
  //                 GPIO pins in Kconfig. Used by boards that don't
  //                 expose a shared SPI bus for the display.
  spi_host_device_t spi_host =
      (bus != NULL) ? (spi_host_device_t)(intptr_t)bus : LCD_HOST;

  ESP_LOGI(TAG,
           "Initializing ST7789 (%dx%d landscape) host=%d "
           "CLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d",
           DISPLAY_WIDTH, DISPLAY_HEIGHT, (int)spi_host, CONFIG_DISPLAY_SPI_CLK,
           CONFIG_DISPLAY_SPI_MOSI, CONFIG_DISPLAY_SPI_CS,
           CONFIG_DISPLAY_SPI_DC, CONFIG_DISPLAY_SPI_RST,
           CONFIG_DISPLAY_BL_GPIO);

  // Backlight GPIO is optional — a value of -1 means "not wired / always on",
  // which matches the Kconfig default. BIT64(-1) is undefined, and
  // gpio_set_level(-1, ...) returns ESP_ERR_INVALID_ARG, so skip the config
  // entirely when the pin is not set.
  if (CONFIG_DISPLAY_BL_GPIO >= 0) {
    gpio_config_t bl_cfg = {
        .pin_bit_mask = BIT64(CONFIG_DISPLAY_BL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(CONFIG_DISPLAY_BL_GPIO, 0);
  }

  bg_load_from_spiffs();

  // Only initialise the SPI bus ourselves if the caller didn't pass a
  // pre-initialised one. Calling spi_bus_initialize() on a host that is
  // already initialised returns ESP_ERR_INVALID_STATE.
  if (bus == NULL) {
    spi_bus_config_t buscfg = {
        .mosi_io_num = CONFIG_DISPLAY_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_DISPLAY_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO));
  }

  esp_lcd_panel_io_handle_t io_handle = NULL;
  esp_lcd_panel_io_spi_config_t io_cfg = {
      .dc_gpio_num = CONFIG_DISPLAY_SPI_DC,
      .cs_gpio_num = CONFIG_DISPLAY_SPI_CS,
      .pclk_hz = LCD_PIXEL_CLOCK_HZ,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .spi_mode = 0,
      .trans_queue_depth = 4,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host,
                                           &io_cfg, &io_handle));

  esp_lcd_panel_handle_t panel_handle = NULL;
  esp_lcd_panel_dev_config_t panel_cfg = {
      .reset_gpio_num = CONFIG_DISPLAY_SPI_RST,
      .rgb_endian = LCD_RGB_ENDIAN_RGB,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

  // Pin the LVGL task to Core 0. Default task_affinity=-1 allows migration
  // to Core 1 where it interferes with the audio task (priority 7).
  const lvgl_port_cfg_t lvgl_cfg = {
      .task_priority = 4,
      .task_stack = 6144,
      .task_affinity = 0,
      .task_max_sleep_ms = 500,
      .timer_period_ms = 5,
  };
  ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

  // buff_dma=true, buff_spiram=false: draw buffers in DMA-capable internal
  // SRAM. esp_lvgl_port passes the buffer pointer directly to
  // esp_lcd_panel_draw_bitmap — PSRAM buffers cause SPI master to allocate
  // a private DMA buffer at runtime, which fails under memory pressure.
  const lvgl_port_display_cfg_t disp_cfg = {
      .io_handle = io_handle,
      .panel_handle = panel_handle,
      .buffer_size = DISPLAY_WIDTH * DRAW_BUF_LINES,
      .double_buffer = true,
      .trans_size = 0,
      .hres = DISPLAY_WIDTH,
      .vres = DISPLAY_HEIGHT,
      .monochrome = false,
      .flags =
          {
              .buff_dma = true,
              .buff_spiram = false,
              .swap_bytes = true,
          },
  };
  s_lvgl_disp = lvgl_port_add_disp(&disp_cfg);
  assert(s_lvgl_disp != NULL);

  // Rotation MUST be applied after lvgl_port_add_disp() — the port resets
  // the ST7789 MADCTL register during display registration.
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
#ifdef CONFIG_DISPLAY_FLIP
  // 180° rotation: invert both mirror axes relative to the landscape default of
  // (true, false). CONFIG_DISPLAY_FLIP was previously honoured only by the
  // u8g2/OLED path (u8g2_SetFlipMode) and silently ignored here, so setting it
  // had no effect on an ST7789.
  //
  // The Y gap needs no adjustment: 35 + 170 + 35 = 240, so the visible window is
  // centred in the controller's 240 rows and is symmetric under mirroring.
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));
#else
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
#endif
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(
      panel_handle, CONFIG_DISPLAY_ST7789_GAP_X, CONFIG_DISPLAY_ST7789_GAP_Y));

  // Acquire the LVGL port mutex before touching LVGL widgets. The port task
  // is already running at this point, so we must wait on the lock rather
  // than pass 0 (non-waiting try). If the lock cannot be acquired within a
  // generous timeout, init has gone wrong — abort rather than corrupt LVGL
  // state by calling ui_create() unlocked.
  if (lvgl_port_lock(1000)) {
    ui_create();
    lvgl_port_unlock();
  } else {
    ESP_LOGE(TAG, "Failed to acquire LVGL lock during init — UI not built");
    abort();
  }

  if (CONFIG_DISPLAY_BL_GPIO >= 0) {
    gpio_set_level(CONFIG_DISPLAY_BL_GPIO, 1);
  }

  // Boot into the splash. Any real event (playback starting, WiFi setup needed)
  // replaces it immediately; otherwise display_task times it out.
  s_display.state = DISPLAY_STATE_SPLASH;
  s_splash_until_us = esp_timer_get_time() + (int64_t)SPLASH_MS * 1000;
  s_display.dirty = true;

  rtsp_events_register(on_rtsp_event, NULL);

  // Pinned to Core 0 — audio runs on Core 1
  xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 3, NULL, 0);

  ESP_LOGI(TAG, "ST7789 display initialized (LVGL 9 + esp_lvgl_port)");
}
