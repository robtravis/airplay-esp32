/**
 * @file radio_source.h
 * @brief HTTP MP3 internet-radio source.
 *
 * Plays a fixed HTTP MP3 stream through the shared audio_output, so the device
 * is useful with no phone attached. Intended to run by default and be suspended
 * while an AirPlay session is active (see radio_coex).
 *
 * Design notes, learned the hard way on the Arduino radio firmware:
 *
 *   - TWO TASKS, NOT ONE. One task reads the socket, another decodes and writes
 *     to I2S. A single task doing both leaves the socket unserviced whenever
 *     decode blocks on DMA, which lets the receive window close and the flow
 *     die. Splitting them cut the observed dead-flow rate roughly 18x.
 *
 *   - PREFILL IS THE CUSHION. A live stream arrives at real-time rate, so fill
 *     and drain run at the same speed and a buffer never accumulates on its
 *     own — it sits near empty and protects nothing. Whatever depth exists when
 *     draining starts is the depth that persists, so we wait for
 *     CONFIG_RADIO_PREFILL_BYTES before the first sample.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

/// Allocate the ring buffer and open the decoder. Call once at startup.
esp_err_t radio_source_init(void);

/// Connect and begin playback. Idempotent.
esp_err_t radio_source_start(void);

/// Stop playback and close the connection. Idempotent. Keeps the ring allocated.
void radio_source_stop(void);

/// True while the stream is running (regardless of whether audio has started).
bool radio_source_is_playing(void);

/// Reconnect count since boot — useful for spotting a flaky link.
uint32_t radio_source_reconnects(void);

// ── Source mode ────────────────────────────────────────────────────────────────
// 0 = internet radio, 1 = AirPlay. Persisted in NVS, defaults to RADIO so a unit
// plays the station on power-up with no phone involved.
//
// "AirPlay mode" simply means the radio is not holding the I2S channel: the radio
// takes exclusive ownership when it starts (audio_output_stop) and returns it when
// it stops (audio_output_start). AirPlay is advertised in both modes.
#define SOURCE_MODE_RADIO   0
#define SOURCE_MODE_AIRPLAY 1

/// Read the saved mode and start the radio if that is what it says. Call at boot.
esp_err_t radio_source_apply_saved_mode(void);

/// Switch mode now and persist it.
esp_err_t radio_source_set_mode(uint8_t mode);

/// Current mode.
uint8_t radio_source_get_mode(void);
