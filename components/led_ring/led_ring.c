#include "led_ring.h"

#ifdef CONFIG_LED_RING_ENABLE

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rtsp_events.h"
#include <math.h>
#include <string.h>

static const char *TAG = "led_ring";

#define NUM_LEDS CONFIG_LED_RING_COUNT
#define PIN_DATA CONFIG_LED_RING_DATA_GPIO
#define PIN_CLK  CONFIG_LED_RING_CLK_GPIO

// rgb_scale multipliers, straight from the radio's config.h. The APA102 global
// brightness field is pinned to its 1/31 minimum and fine control happens here,
// which is what keeps the ring from being blinding indoors.
#define SCALE_FLASHLIGHT 255

// 20ms gate = ~50fps. The radio measured a full ring update well under 1ms of
// bit-banging; anything over 10ms means the ring is not the cheap subsystem it
// looks like, so it warns rather than silently stealing time from audio.
#define FRAME_MS      20
#define SLOW_WARN_US  10000

#define NVS_NS      "led_ring"
#define NVS_KEY_FX  "fx"
#define NVS_KEY_SCALE "scale"

typedef struct {
  uint8_t r, g, b;
} rgb_t;

static const char *s_fx_names[LED_FX_COUNT] = {
    "BREATHE", "RAINBOW", "COMET", "PULSE",
    "FIRE",    "SPARKLE", "OFF",   "FLASHLIGHT"};

static rgb_t s_leds[NUM_LEDS];
static volatile led_ring_state_t s_state = LED_RING_BOOT;
static volatile int s_fx = LED_FX_BREATHE;
// Runtime brightness so it can be tuned without a reflash: the difference
// between the self-test (255) and the shipped default is large, and the right
// value depends on the room.
static volatile int s_scale = CONFIG_LED_RING_SCALE;
static int64_t s_boot_us = 0;

// ── APA102 software SPI ───────────────────────────────────────────────────────
// Bit-banged rather than routed through an SPI peripheral: this is exactly what
// the radio firmware does on this hardware, it needs no bus (both SPI hosts are
// already spoken for by the display), and a full frame is 36 bytes.

// Arduino's digitalWrite takes ~1us per call, so the radio firmware clocked this
// ring at roughly 500kHz without ever intending to. gpio_set_level is ~20x
// faster, which clocks the part far harder than the known-good code did and
// nothing latched — hence an explicit delay.
//
// esp_rom_delay_us(1) per half-bit was measured at 10ms per frame (half the 20ms
// budget), because 576 yield-free busy-waits also collect preemption. NOPs give
// ~500ns per half-bit — a ~1MHz clock, the same ballpark as the Arduino build,
// well inside the APA102's ~20MHz limit — for ~290us per frame.
#define BIT_DELAY_NOPS 120

static inline void bit_delay(void) {
  for (int i = 0; i < BIT_DELAY_NOPS; i++) {
    __asm__ __volatile__("nop");
  }
}

static inline void write_byte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    gpio_set_level(PIN_CLK, 0);
    gpio_set_level(PIN_DATA, (b >> i) & 1);
    bit_delay();
    gpio_set_level(PIN_CLK, 1);
    bit_delay();
  }
}

static void show(uint8_t rgb_scale) {
  for (int i = 0; i < 4; i++) {
    write_byte(0x00); // start frame — 32 zero bits
  }
  for (int i = 0; i < NUM_LEDS; i++) {
    write_byte(0xE0 | 1); // global brightness pinned to minimum
    write_byte((uint8_t)((s_leds[i].b * rgb_scale) >> 8));
    write_byte((uint8_t)((s_leds[i].g * rgb_scale) >> 8));
    write_byte((uint8_t)((s_leds[i].r * rgb_scale) >> 8));
  }
  for (int i = 0; i < 4; i++) {
    write_byte(0xFF); // end frame — enough for <= 64 LEDs
  }
}

static void fill(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NUM_LEDS; i++) {
    s_leds[i].r = r;
    s_leds[i].g = g;
    s_leds[i].b = b;
  }
}

static uint32_t now_ms(void) {
  return (uint32_t)((esp_timer_get_time() - s_boot_us) / 1000);
}

/// Sine brightness for breathing effects. period_ms is a full cycle.
static uint8_t breathe(uint32_t period_ms) {
  float t = (float)(now_ms() % period_ms) / (float)period_ms;
  return (uint8_t)(15.0f + 112.5f * (1.0f + sinf(t * 2.0f * (float)M_PI)));
}

/// HSV -> RGB, all channels 0-255.
static rgb_t hsv(uint8_t h, uint8_t s, uint8_t v) {
  rgb_t out;
  if (s == 0) {
    out.r = out.g = out.b = v;
    return out;
  }
  uint8_t region = h / 43;
  uint8_t rem = (uint8_t)((h - region * 43) * 6);
  uint8_t p = (uint8_t)((v * (255 - s)) >> 8);
  uint8_t q = (uint8_t)((v * (255 - ((s * rem) >> 8))) >> 8);
  uint8_t t = (uint8_t)((v * (255 - ((s * (255 - rem)) >> 8))) >> 8);
  switch (region) {
  case 0: out = (rgb_t){v, t, p}; break;
  case 1: out = (rgb_t){q, v, p}; break;
  case 2: out = (rgb_t){p, v, t}; break;
  case 3: out = (rgb_t){p, q, v}; break;
  case 4: out = (rgb_t){t, p, v}; break;
  default: out = (rgb_t){v, p, q}; break;
  }
  return out;
}

static uint32_t rnd(uint32_t n) { return n ? (esp_random() % n) : 0; }

// ── Effects ───────────────────────────────────────────────────────────────────

static void render_streaming(void) {
  switch (s_fx) {
  case LED_FX_BREATHE: { // slow breathe, hue drifting through the spectrum
    uint8_t b = breathe(4000);
    // ~31s for a full lap of the colour wheel, against a 4s breath. Slow enough
    // that it reads as the ring changing colour rather than as an animation —
    // PULSE is the fast-hue version (7.7s lap on a 2s breath).
    uint8_t hue = (uint8_t)((now_ms() / 120) & 0xFF);
    rgb_t c = hsv(hue, 255, b);
    fill(c.r, c.g, c.b);
    break;
  }
  case LED_FX_RAINBOW: { // hue sweeps around the ring
    uint8_t base = (uint8_t)((now_ms() / 20) & 0xFF);
    for (int i = 0; i < NUM_LEDS; i++) {
      s_leds[i] = hsv((uint8_t)(base + i * (255 / NUM_LEDS)), 255, 180);
    }
    break;
  }
  case LED_FX_COMET: { // colour-shifting comet with a fading tail
    for (int i = 0; i < NUM_LEDS; i++) {
      s_leds[i].r = (uint8_t)(s_leds[i].r * 2 / 3);
      s_leds[i].g = (uint8_t)(s_leds[i].g * 2 / 3);
      s_leds[i].b = (uint8_t)(s_leds[i].b * 2 / 3);
    }
    uint8_t pos = (uint8_t)(NUM_LEDS - 1 - (now_ms() / 200) % NUM_LEDS);
    uint8_t hue = (uint8_t)((now_ms() / 20) & 0xFF);
    s_leds[pos] = hsv(hue, 255, 255);
    break;
  }
  case LED_FX_PULSE: { // all LEDs pulse, hue drifting
    uint8_t b = breathe(2000);
    uint8_t hue = (uint8_t)((now_ms() / 30) & 0xFF);
    rgb_t c = hsv(hue, 255, b);
    fill(c.r, c.g, c.b);
    break;
  }
  case LED_FX_FIRE: { // warm flicker at ~8fps
    static uint32_t fire_ms = 0;
    if (now_ms() - fire_ms >= 120) {
      fire_ms = now_ms();
      for (int i = 0; i < NUM_LEDS; i++) {
        uint8_t f = (uint8_t)(80 + rnd(175));
        s_leds[i] = (rgb_t){f, (uint8_t)(f / 8), 0};
      }
    }
    break;
  }
  case LED_FX_SPARKLE: { // random flashes with decay
    for (int i = 0; i < NUM_LEDS; i++) {
      s_leds[i].r = s_leds[i].r > 30 ? (uint8_t)(s_leds[i].r - 30) : 0;
      s_leds[i].g = s_leds[i].g > 30 ? (uint8_t)(s_leds[i].g - 30) : 0;
      s_leds[i].b = s_leds[i].b > 30 ? (uint8_t)(s_leds[i].b - 30) : 0;
    }
    if (rnd(3) == 0) {
      s_leds[rnd(NUM_LEDS)] = hsv((uint8_t)rnd(256), 200, 255);
    }
    break;
  }
  case LED_FX_OFF:
    fill(0, 0, 0);
    break;
  case LED_FX_FLASHLIGHT:
    fill(255, 255, 255);
    break;
  default:
    break;
  }
}

static void render(void) {
  switch (s_state) {
  case LED_RING_OFF:
    fill(0, 0, 0);
    break;
  case LED_RING_BOOT: { // white fade-in over ~1.2s
    uint32_t ms = now_ms() / 5;
    uint8_t v = (uint8_t)(ms > 255 ? 255 : ms);
    fill(v, v, v);
    break;
  }
  case LED_RING_WIFI_PORTAL: // amber steady — waiting to be set up
    fill(255, 140, 0);
    break;
  case LED_RING_WIFI_CONNECTING: {
    uint8_t b = breathe(1500);
    fill(b, (uint8_t)(b / 4), 0);
    break;
  }
  case LED_RING_STREAMING:
    render_streaming();
    break;
  case LED_RING_ARCHIVE: { // magenta breathe
    uint8_t b = breathe(3000);
    fill(b, 0, b);
    break;
  }
  }

  uint8_t scale = (s_fx == LED_FX_FLASHLIGHT && s_state == LED_RING_STREAMING)
                      ? SCALE_FLASHLIGHT
                      : (uint8_t)s_scale;
  show(scale);
}

static void led_ring_task(void *arg) {
  (void)arg;

  // Self-test: full white at maximum scale, unmistakable if the data is
  // landing. If this does not light, the problem is wiring, power or pins —
  // not the effects or the brightness scale.
  ESP_LOGI(TAG, "self-test: full white 600ms");
  fill(255, 255, 255);
  show(255);
  vTaskDelay(pdMS_TO_TICKS(600));

  while (1) {
    int64_t t0 = esp_timer_get_time();
    render();
    int64_t el = esp_timer_get_time() - t0;
    if (el > SLOW_WARN_US) {
      ESP_LOGW(TAG, "slow frame: %lldus (state=%d fx=%d)", el, (int)s_state,
               s_fx);
    }
    vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
  }
}

// ── Playback events ───────────────────────────────────────────────────────────
// Subscribing here rather than being driven from main keeps the wiring in one
// place: the radio and AirPlay both already publish on this bus.

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)data;
  (void)user_data;
  switch (event) {
  case RTSP_EVENT_PLAYING:
    led_ring_set_state(LED_RING_STREAMING);
    break;
  case RTSP_EVENT_CLIENT_CONNECTED:
    led_ring_set_state(LED_RING_STREAMING);
    break;
  case RTSP_EVENT_DISCONNECTED:
    led_ring_set_state(LED_RING_OFF);
    break;
  default:
    break;
  }
}

// ── Public API ────────────────────────────────────────────────────────────────

static const char *state_name(led_ring_state_t st) {
  switch (st) {
  case LED_RING_OFF: return "OFF";
  case LED_RING_BOOT: return "BOOT";
  case LED_RING_WIFI_PORTAL: return "WIFI_PORTAL";
  case LED_RING_WIFI_CONNECTING: return "WIFI_CONNECTING";
  case LED_RING_STREAMING: return "STREAMING";
  case LED_RING_ARCHIVE: return "ARCHIVE";
  }
  return "?";
}

void led_ring_set_state(led_ring_state_t state) {
  if (s_state != state) {
    ESP_LOGI(TAG, "state %s -> %s", state_name(s_state), state_name(state));
  }
  s_state = state;
}
led_ring_state_t led_ring_get_state(void) { return s_state; }

const char *led_ring_effect_name(int fx) {
  return (fx >= 0 && fx < LED_FX_COUNT) ? s_fx_names[fx] : "";
}

int led_ring_get_effect(void) { return s_fx; }

void led_ring_set_effect(int fx) {
  if (fx < 0) {
    fx = 0;
  }
  if (fx >= LED_FX_COUNT) {
    fx = LED_FX_COUNT - 1;
  }
  s_fx = fx;

  nvs_handle_t nvs;
  if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_i32(nvs, NVS_KEY_FX, fx);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
  ESP_LOGI(TAG, "effect -> %s", led_ring_effect_name(fx));
}

int led_ring_get_scale(void) { return s_scale; }

void led_ring_set_scale(int scale) {
  if (scale < 0) {
    scale = 0;
  }
  if (scale > 255) {
    scale = 255;
  }
  s_scale = scale;

  nvs_handle_t nvs;
  if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_i32(nvs, NVS_KEY_SCALE, scale);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
  ESP_LOGI(TAG, "brightness -> %d", scale);
}

void led_ring_init(void) {
  s_boot_us = esp_timer_get_time();

  gpio_config_t io = {
      .pin_bit_mask = (1ULL << PIN_DATA) | (1ULL << PIN_CLK),
      .mode = GPIO_MODE_OUTPUT,
  };
  ESP_ERROR_CHECK(gpio_config(&io));
  gpio_set_level(PIN_DATA, 0);
  gpio_set_level(PIN_CLK, 0);

  nvs_handle_t nvs;
  if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
    int32_t stored = 0;
    if (nvs_get_i32(nvs, NVS_KEY_FX, &stored) == ESP_OK) {
      s_fx = (stored >= 0 && stored < LED_FX_COUNT) ? (int)stored
                                                    : LED_FX_BREATHE;
    }
    int32_t stored_scale = 0;
    if (nvs_get_i32(nvs, NVS_KEY_SCALE, &stored_scale) == ESP_OK &&
        stored_scale >= 0 && stored_scale <= 255) {
      s_scale = (int)stored_scale;
    }
    nvs_close(nvs);
  }

  fill(0, 0, 0);
  show((uint8_t)s_scale);

  if (rtsp_events_register(on_rtsp_event, NULL) != 0) {
    ESP_LOGE(TAG, "event registration FAILED — effects will not follow "
                  "playback (MAX_LISTENERS reached)");
  }

  // Priority 2: below audio and the display. A dropped frame is invisible; a
  // dropped audio buffer is not.
  xTaskCreatePinnedToCore(led_ring_task, "led_ring", 3072, NULL, 2, NULL, 0);
  ESP_LOGI(TAG, "%d APA102 LEDs on data=%d clk=%d, effect=%s", NUM_LEDS,
           PIN_DATA, PIN_CLK, led_ring_effect_name(s_fx));
}

#endif // CONFIG_LED_RING_ENABLE
