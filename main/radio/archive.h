/**
 * @file archive.h
 * @brief Recorded shows — fetch the catalogue, play episodes.
 *
 * Ports archive.h from the radio firmware. The endpoint returns a JSON object
 * keyed by show name, each value an array of episodes:
 *
 *   { "Morning Vibes": [ {"url": "/rec/...", "recorded_at": "2026-03-27T18:30:00+00:00",
 *                         "duration_secs": 6840}, ... ], ... }
 *
 * Playback runs through radio_source: an episode is just an HTTP MP3. What this
 * module adds is the catalogue, the episode sequencing (auto-advance, then fall
 * back to the live stream), and the on-screen metadata for a recorded show.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// The live catalogue returns 40 shows; 24 silently truncated the list.
#define ARCHIVE_MAX_SHOWS    40
#define ARCHIVE_MAX_EPISODES 5

/// Create the worker task. Does not fetch — the catalogue is only worth the
/// round trip once someone opens the menu.
void archive_init(void);

/// Kick off a fetch in the background. Poll archive_is_ready(), or register a
/// callback to be told.
void archive_start_fetch(void);

/// True once a fetch has completed successfully and the lists are populated.
bool archive_is_ready(void);

/// True while a fetch is in flight.
bool archive_is_loading(void);

/// Called when a fetch finishes (either way). Runs on the worker task — keep it
/// short and do not block.
void archive_set_ready_cb(void (*cb)(void));

int archive_show_count(void);
const char *archive_show_name(int show);
int archive_episode_count(int show);
const char *archive_episode_label(int show, int episode);

/// Start playing an episode, publish its metadata, and auto-advance through the
/// rest of the show. Safe to call from the menu.
esp_err_t archive_play(int show, int episode);

/// Stop archive playback and return to the live stream.
void archive_stop(void);

/// True while an episode is playing.
bool archive_is_playing(void);
