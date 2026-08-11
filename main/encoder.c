/**
 * @file encoder.c
 * @brief Rotary encoder → volume, using the PCNT peripheral.
 *
 * The T-Embed's only continuous control is its rotary encoder. Decoding it in
 * software means either an ISR per edge or a fast poll; PCNT does full
 * quadrature decode in hardware, so a turn cannot be missed while the audio
 * tasks hold the CPU. That matters here — this firmware's whole failure mode is
 * starving the audio pump.
 *
 * Pins come from the radio firmware's config.h, known-good on this hardware:
 *   PIN_ENC_A 2 (CLK), PIN_ENC_B 1 (DT), PIN_ENC_BTN 0 (also the BOOT button,
 *   which is why the button is wired through the normal buttons.c path rather
 *   than here).
 */
#include "encoder.h"

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "menu.h"
#include "playback_control.h"

static const char *TAG = "encoder";

#if defined(CONFIG_ENCODER_A_GPIO) && (CONFIG_ENCODER_A_GPIO >= 0)

// Quadrature gives 4 counts per detent; anything else feels like the volume
// jumps four steps per click.
#define COUNTS_PER_DETENT 4
#define POLL_MS           20
#define PCNT_LIMIT        1000

// The encoder's push-switch, polled here rather than through buttons.c: this
// module needs short vs long press on the same pin, and one owner per pin keeps
// the two from fighting over it. 600ms matches the radio's NAV_LONG_PRESS_MS.
#define LONG_PRESS_MS 600

static pcnt_unit_handle_t s_unit = NULL;

static void handle_rotation(int detents) {
  if (menu_is_open()) {
    menu_scroll(detents);
    return;
  }
  for (int i = 0; i < (detents > 0 ? detents : -detents); i++) {
    if (detents > 0) {
      playback_control_volume_up();
    } else {
      playback_control_volume_down();
    }
  }
  ESP_LOGD(TAG, "%+d detents -> volume %d%%", detents,
           playback_control_get_volume_percent());
}

static void handle_short_press(void) {
  if (menu_is_open()) {
    menu_select();
  } else {
    // No DACP channel on AirPlay 2, so play/pause is a local mute anyway —
    // which is the useful behaviour for a radio.
    playback_control_toggle_mute();
  }
}

static void encoder_task(void *arg) {
  (void)arg;
  int last = 0;
  bool btn_down = false;
  bool long_fired = false;
  int64_t down_us = 0;

  while (1) {
    int count = 0;
    if (pcnt_unit_get_count(s_unit, &count) == ESP_OK) {
      int delta = count - last;
      int detents = delta / COUNTS_PER_DETENT;
      if (detents != 0) {
        // Keep the remainder so slow turns still accumulate to a full detent
        // instead of being discarded.
        last += detents * COUNTS_PER_DETENT;
        handle_rotation(detents);
      }
    }

#if CONFIG_ENCODER_BTN_GPIO >= 0
    bool pressed = (gpio_get_level(CONFIG_ENCODER_BTN_GPIO) == 0);
    if (pressed && !btn_down) {
      btn_down = true;
      long_fired = false;
      down_us = esp_timer_get_time();
    } else if (btn_down) {
      if (!pressed) {
        btn_down = false;
        if (!long_fired) {
          handle_short_press();
        }
      } else if (!long_fired &&
                 (esp_timer_get_time() - down_us) / 1000 >= LONG_PRESS_MS) {
        // Fire on the way down, not on release: holding for the menu should feel
        // like it opened when it opened.
        long_fired = true;
        menu_back_or_open();
      }
    }
#endif

    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
}

esp_err_t encoder_init(void) {
  // PCNT does not configure pull-ups, and the encoder's commons are grounded, so
  // without these the inputs float and generate phantom counts.
  gpio_config_t io = {
      .pin_bit_mask = (1ULL << CONFIG_ENCODER_A_GPIO) |
                      (1ULL << CONFIG_ENCODER_B_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&io));

#if CONFIG_ENCODER_BTN_GPIO >= 0
  gpio_config_t btn = {
      .pin_bit_mask = 1ULL << CONFIG_ENCODER_BTN_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE, // active-low switch to ground
  };
  ESP_ERROR_CHECK(gpio_config(&btn));
#endif

  pcnt_unit_config_t unit_cfg = {
      .high_limit = PCNT_LIMIT,
      .low_limit = -PCNT_LIMIT,
  };
  ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_unit));

  // Mechanical encoders bounce; 1us of filtering removes the contact noise
  // without touching a real turn.
  pcnt_glitch_filter_config_t filter = {.max_glitch_ns = 1000};
  ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_unit, &filter));

  // Two channels watching each other's level is the standard x4 quadrature
  // decode: direction comes from the other phase's level at each edge.
  pcnt_chan_config_t chan_a_cfg = {
      .edge_gpio_num = CONFIG_ENCODER_A_GPIO,
      .level_gpio_num = CONFIG_ENCODER_B_GPIO,
  };
  pcnt_channel_handle_t chan_a = NULL;
  ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chan_a_cfg, &chan_a));
  ESP_ERROR_CHECK(pcnt_channel_set_edge_action(
      chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
      PCNT_CHANNEL_EDGE_ACTION_INCREASE));
  ESP_ERROR_CHECK(pcnt_channel_set_level_action(
      chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
      PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

  pcnt_chan_config_t chan_b_cfg = {
      .edge_gpio_num = CONFIG_ENCODER_B_GPIO,
      .level_gpio_num = CONFIG_ENCODER_A_GPIO,
  };
  pcnt_channel_handle_t chan_b = NULL;
  ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chan_b_cfg, &chan_b));
  ESP_ERROR_CHECK(pcnt_channel_set_edge_action(
      chan_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
      PCNT_CHANNEL_EDGE_ACTION_DECREASE));
  ESP_ERROR_CHECK(pcnt_channel_set_level_action(
      chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
      PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

  ESP_ERROR_CHECK(pcnt_unit_enable(s_unit));
  ESP_ERROR_CHECK(pcnt_unit_clear_count(s_unit));
  ESP_ERROR_CHECK(pcnt_unit_start(s_unit));

  // 6144, not 3072: this task drives the menu, which writes NVS (volume on every
  // detent, LED effect and brightness on selection) and calls into the display.
  xTaskCreatePinnedToCore(encoder_task, "encoder", 6144, NULL, 3, NULL, 0);
  ESP_LOGI(TAG, "rotary on GPIO %d/%d, button %d (long press %dms = menu)",
           CONFIG_ENCODER_A_GPIO, CONFIG_ENCODER_B_GPIO,
           CONFIG_ENCODER_BTN_GPIO, LONG_PRESS_MS);
  return ESP_OK;
}

#else  // encoder disabled

esp_err_t encoder_init(void) { return ESP_OK; }

#endif
