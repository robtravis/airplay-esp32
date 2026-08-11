#include "audio_vis.h"

#include "esp_timer.h"
#include <math.h>
#include <string.h>

// Latest window of mono samples, newest-wins. Written by the decoder task, read
// by the display task; see the header for why this is deliberately lock-free.
static int16_t s_window[AUDIO_VIS_WINDOW];
static volatile uint32_t s_seq = 0;
static volatile int64_t s_last_push_us = 0;

// Decay state, so bars fall smoothly instead of flickering between frames. Lives
// on the reader side.
static float s_level[AUDIO_VIS_BANDS];

// Band edges in HERTZ, not bin numbers — roughly third-octave, the spacing real
// spectrum analysers use, because pitch is logarithmic and equal-width bins put
// most of the display above 10kHz where music has almost no energy.
//
// Defining these in Hz is the actual fix: simply lengthening the window while
// keeping proportional bin edges changes nothing, since the bands would still
// cover the same frequency ranges.
// ISO octave centres 31.5, 63, 125, 250, 500, 1k, 2k, 4k, 8k, 16k, with edges at
// the geometric means between them.
static const float BAND_HZ[AUDIO_VIS_BANDS + 1] = {
    22.0f,   44.0f,   88.0f,    177.0f,   354.0f,
    707.0f,  1414.0f, 2828.0f,  5657.0f,  11314.0f,
    20000.0f};

// FFT magnitude for a full-scale sine at a bin centre is N/4 with a Hann window
// (coherent gain 0.5, then single-sided). Without dividing by this, a loud band
// measures about +48dB and EVERY column clamps to full height — which is exactly
// what happened: the display pegged on all frequencies regardless of content.
#define MAG_FULL_SCALE ((float)AUDIO_VIS_WINDOW * 0.25f)

#define SAMPLE_RATE 44100.0f

// 8KB of working buffers. Static, not stack: the display task does not have room
// for two 1024-float arrays.
static float s_re[AUDIO_VIS_WINDOW];
static float s_im[AUDIO_VIS_WINDOW];

void audio_vis_push(const int16_t *pcm, size_t samples) {
  if (!pcm || samples < 2) {
    return;
  }
  // Take the newest AUDIO_VIS_WINDOW frames from the end of the buffer, mixed to
  // mono. Averaging L+R rather than dropping a channel keeps centre-panned
  // material from looking quiet.
  size_t frames = samples / 2;
  size_t take = frames < AUDIO_VIS_WINDOW ? frames : AUDIO_VIS_WINDOW;
  const int16_t *src = pcm + (frames - take) * 2;

  for (size_t i = 0; i < take; i++) {
    s_window[i] = (int16_t)(((int32_t)src[i * 2] + src[i * 2 + 1]) / 2);
  }
  if (take < AUDIO_VIS_WINDOW) {
    memset(&s_window[take], 0, (AUDIO_VIS_WINDOW - take) * sizeof(int16_t));
  }
  s_seq++;
  s_last_push_us = esp_timer_get_time();
}

/// In-place iterative radix-2 FFT. 1024 points in float is ~5k butterflies —
/// well under a millisecond on this part, at 20fps, on the display task. The S3
/// has hardware float, so there is no reason to go fixed point.
static void fft(float *re, float *im, int n) {
  // Bit-reversal permutation.
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      float tr = re[i];
      re[i] = re[j];
      re[j] = tr;
      float ti = im[i];
      im[i] = im[j];
      im[j] = ti;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * (float)M_PI / (float)len;
    float wr = cosf(ang);
    float wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k;
        int b = i + k + len / 2;
        float xr = re[b] * cr - im[b] * ci;
        float xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
        float ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

bool audio_vis_get_bands(uint8_t *bands, int count) {
  if (!bands || count <= 0) {
    return false;
  }
  // Nothing for a while means silence or a stopped source: report idle so the
  // caller can rest the bars rather than animate noise.
  if (esp_timer_get_time() - s_last_push_us > 500000) {
    memset(s_level, 0, sizeof(s_level));
    memset(bands, 0, (size_t)count);
    return false;
  }

  float *re = s_re;
  float *im = s_im;
  for (int i = 0; i < AUDIO_VIS_WINDOW; i++) {
    // Hann window: without it every bar jumps on transients, because a hard-cut
    // window smears energy across the whole spectrum.
    float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i /
                                 (float)(AUDIO_VIS_WINDOW - 1));
    re[i] = ((float)s_window[i] / 32768.0f) * w;
    im[i] = 0.0f;
  }
  fft(re, im, AUDIO_VIS_WINDOW);

  const float bin_hz = SAMPLE_RATE / (float)AUDIO_VIS_WINDOW;
  int n = count < AUDIO_VIS_BANDS ? count : AUDIO_VIS_BANDS;
  for (int b = 0; b < n; b++) {
    float sum = 0.0f;
    int lo = (int)(BAND_HZ[b] / bin_hz);
    int hi = (int)(BAND_HZ[b + 1] / bin_hz);
    if (lo < 1) {
      lo = 1; // bin 0 is DC
    }
    if (hi <= lo) {
      hi = lo + 1; // the lowest bands are narrower than one bin
    }
    if (hi > AUDIO_VIS_WINDOW / 2) {
      hi = AUDIO_VIS_WINDOW / 2;
    }
    // Peak bin, not the mean: averaging dilutes a tone across a wide band, so
    // the top octaves would read quiet purely because they span more bins.
    for (int k = lo; k < hi; k++) {
      float m = sqrtf(re[k] * re[k] + im[k] * im[k]);
      if (m > sum) {
        sum = m;
      }
    }
    float mag = sum / MAG_FULL_SCALE;

    // dB-ish compression: linear magnitude makes everything but the bass look
    // dead, since music's energy is heavily weighted to the low end.
    float db = 20.0f * log10f(mag + 1e-6f);
    // Window -58dB..-18dB.
    //
    // The first calibration used -50..0 because a pure sine at half scale reads
    // about -6dB in its band. That was the wrong reference signal: a tone puts all
    // its energy in ONE bin, while music spreads it across many, so real per-bin
    // peaks are 15-20dB lower. Measured on broadband material at the same peak
    // level, bands sit at -41 to -21dB — which under -50..0 produced columns
    // peaking around row 7 of 15, so the yellow zone was only ever grazed and
    // magenta was unreachable.
    //
    // Under this window that same material lands at rows 6-13, which is where the
    // colour zones actually live. Loud passages will touch the top occasionally;
    // that is what the peak-hold marker is for.
    float norm = (db + 58.0f) / 40.0f;
    if (norm < 0.0f) {
      norm = 0.0f;
    }
    if (norm > 1.0f) {
      norm = 1.0f;
    }

    // Fast attack, slow release — the standard trick that makes bars read as
    // rhythm rather than flicker.
    if (norm > s_level[b]) {
      s_level[b] = norm;
    } else {
      // Faster fall than the original 0.35: with a peak marker holding the
      // maximum, the columns themselves can be lively without looking jittery.
      s_level[b] += (norm - s_level[b]) * 0.45f;
    }
    bands[b] = (uint8_t)(s_level[b] * 255.0f);
  }
  for (int b = n; b < count; b++) {
    bands[b] = 0;
  }
  return true;
}
