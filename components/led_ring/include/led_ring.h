/**
 * @file led_ring.h
 * @brief APA102 LED ring (7 LEDs on the T-Embed) with selectable effects.
 *
 * Ported from the radio firmware's led.h, which is known-good on this hardware.
 * Named led_ring_* rather than led_* because main/led.c already owns a simple
 * status LED — the radio port hit exactly this class of symbol collision.
 */
#pragma once

#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>

/// Streaming effects, selectable by the user. Indices are persisted, so do not
/// reorder them.
typedef enum {
  LED_FX_BREATHE = 0,
  LED_FX_RAINBOW,
  LED_FX_COMET,
  LED_FX_PULSE,
  LED_FX_FIRE,
  LED_FX_SPARKLE,
  LED_FX_OFF,
  LED_FX_FLASHLIGHT,
  LED_FX_COUNT,
} led_fx_t;

/// What the device is doing. Drives the ring when not streaming.
typedef enum {
  LED_RING_OFF = 0,
  LED_RING_BOOT,
  LED_RING_WIFI_PORTAL,
  LED_RING_WIFI_CONNECTING,
  LED_RING_STREAMING, ///< Radio or AirPlay — runs the user's chosen effect.
  LED_RING_ARCHIVE,
} led_ring_state_t;

#ifdef CONFIG_LED_RING_ENABLE

/// Start the ring and its render task, and subscribe to playback events.
void led_ring_init(void);

void led_ring_set_state(led_ring_state_t state);
led_ring_state_t led_ring_get_state(void);

/// Select an effect (persisted to NVS). Out-of-range values are clamped.
void led_ring_set_effect(int fx);
int led_ring_get_effect(void);
const char *led_ring_effect_name(int fx);

/// Per-channel brightness 0-255 (persisted). The APA102 global brightness field
/// is pinned to its minimum, so this is the real brightness control.
void led_ring_set_scale(int scale);
int led_ring_get_scale(void);

#else

static inline void led_ring_init(void) {}
static inline void led_ring_set_state(led_ring_state_t state) {
  (void)state;
}
static inline led_ring_state_t led_ring_get_state(void) {
  return LED_RING_OFF;
}
static inline void led_ring_set_effect(int fx) {
  (void)fx;
}
static inline int led_ring_get_effect(void) {
  return 0;
}
static inline const char *led_ring_effect_name(int fx) {
  (void)fx;
  return "";
}
static inline void led_ring_set_scale(int scale) {
  (void)scale;
}
static inline int led_ring_get_scale(void) {
  return 0;
}

#endif
