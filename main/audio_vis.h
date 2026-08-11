/**
 * @file audio_vis.h
 * @brief Spectrum tap for the display's visualiser.
 *
 * The audio side does as little as possible: it copies a short window of mono
 * samples into a static buffer and returns. All the arithmetic (windowing, FFT,
 * band mapping, decay) happens on the display task, because this firmware's
 * characteristic failure is stealing time from the audio pump — the original
 * radio's stutter was display work starving the decoder.
 *
 * Lock-free by design: the writer overwrites the window and bumps a sequence
 * counter, the reader takes whatever is current. A torn window costs one slightly
 * wrong frame of a decorative bar graph, which is not worth a mutex on the audio
 * path.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Samples per analysis window.
///
/// 1024 at 44.1kHz gives 43Hz bins over a 23ms window. 128 was tried first and is
/// useless for music: at 345Hz per bin the lowest bar starts at 345Hz, so a 60Hz
/// kick, a 200Hz bassline and a 440Hz vocal all land in the same bar and nothing
/// below that is represented at all.
///
/// A 1024-point transform is ~5k butterflies, well under a millisecond on this
/// part, and it runs on the display task at 20fps.
#define AUDIO_VIS_WINDOW 1024

/// Number of bars the display draws. Ten octave bands, the spacing every
/// hi-fi spectrum analyser used (31.5Hz to 16kHz) — wider columns read better on
/// a 320px panel than sixteen thin ones.
#define AUDIO_VIS_BANDS 10

/**
 * Feed decoded PCM. Called from the decoder task.
 *
 * @param pcm     16-bit stereo interleaved.
 * @param samples Total int16 count (i.e. frames * 2).
 */
void audio_vis_push(const int16_t *pcm, size_t samples);

/**
 * Compute band magnitudes 0-255 from the most recent window.
 *
 * Runs the FFT, so call it from the display task, not the audio path.
 *
 * @return false if no audio has arrived recently, so the caller can idle the
 *         bars instead of drawing noise.
 */
bool audio_vis_get_bands(uint8_t *bands, int count);
