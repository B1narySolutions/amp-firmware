#pragma once
// The tuner page's arithmetic, free of LVGL so the host tests can run it (tests/host): the
// Seed's readings (about 20 a second) to a steady note, octave and cents.
//
// - Median of the last five readings: one stray reading (a pick's attack runs a few cents sharp)
//   never moves the needle.
// - A reading more than 70 cents from the current median starts a new note at once, so moving to
//   the next string doesn't drag through the old one.
// - The needle eases towards the median (a third of the way per reading).
// - After the string fades, the last note stays up for TUNER_HOLD_MS, then the page shows none;
//   from TUNER_STALE_MS without a reading it is marked held, so the page can dim it.
#include <stdbool.h>
#include <stdint.h>

#define TUNER_A4_HZ       440.0f
#define TUNER_HISTORY     5
#define TUNER_HOLD_MS     600
#define TUNER_STALE_MS    250   // readings come every 50 ms: five missed means the string has faded
#define TUNER_IN_TUNE_CENTS 3.0f  // the green zone: +-3 cents, as most clip-on and pedal tuners

typedef struct {
    float hist[TUNER_HISTORY];
    int n, pos;
    float cents_shown;
    int midi_shown;
    bool showing;
    uint32_t last_ms;  // the last reading with a pitch
} tuner_smooth_t;

typedef struct {
    bool note;         // false: nothing to show (no string, or it has faded)
    int midi;          // MIDI note number (69 = A4)
    const char *name;  // "E", "C#", ...
    int octave;        // scientific pitch notation: the low E string is E2
    float cents;       // eased, -50 .. +50 from the note
    float hz;          // the median reading
    bool in_tune;      // |cents| <= TUNER_IN_TUNE_CENTS
    bool held;         // no reading for TUNER_STALE_MS: the last note, kept up a little longer
} tuner_view_t;

// The nearest equal-tempered note (A4 = TUNER_A4_HZ) to hz > 0, and how far from it in cents.
int tuner_nearest(float hz, float *cents);
const char *tuner_note_name(int midi);
int tuner_note_octave(int midi);

void tuner_smooth_reset(tuner_smooth_t *s);
// One reading from the Seed (hz 0: no pitch) at now_ms. Fills *out with what to show.
void tuner_smooth_feed(tuner_smooth_t *s, float hz, uint32_t now_ms, tuner_view_t *out);
// Between readings (the page refreshes faster than they come): the hold timeout only.
void tuner_smooth_view(const tuner_smooth_t *s, uint32_t now_ms, tuner_view_t *out);
