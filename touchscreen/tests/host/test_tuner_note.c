// The tuner page's arithmetic (firmware/main/ui/tuner_note.c): note names and octaves across the
// guitar, cents, the median against a stray reading, a fast change of string, the needle easing,
// and the hold after the string fades.
#include <math.h>
#include <string.h>
#include "test.h"
#include "tuner_note.h"

static float detune(float hz, float cents) { return hz * powf(2.0f, cents / 1200.0f); }

void test_tuner_note(void) {
    // Names and octaves: the six open strings, drop D, a 7-string's low B, the 24th-fret E.
    const struct { float hz; const char *name; int octave; } notes[] = {
        { 82.407f, "E", 2 }, { 110.0f, "A", 2 }, { 146.832f, "D", 3 }, { 195.998f, "G", 3 }, { 246.942f, "B", 3 },
        { 329.628f, "E", 4 }, { 73.416f, "D", 2 }, { 61.735f, "B", 1 }, { 1318.51f, "E", 6 }, { 277.183f, "C#", 4 },
        { 440.0f, "A", 4 }, { 261.626f, "C", 4 }, { 123.471f, "B", 2 },
    };
    for (size_t i = 0; i < sizeof(notes) / sizeof(notes[0]); i++) {
        float c;
        const int midi = tuner_nearest(notes[i].hz, &c);
        CHECK(strcmp(tuner_note_name(midi), notes[i].name) == 0);
        CHECK_EQ(tuner_note_octave(midi), notes[i].octave);
        CHECK(fabsf(c) < 0.05f);
    }
    // Cents either side, and the switch to the next note past 50.
    float c;
    CHECK_EQ(tuner_nearest(detune(82.407f, 23.0f), &c), 40);
    CHECK(fabsf(c - 23.0f) < 0.05f);
    CHECK_EQ(tuner_nearest(detune(82.407f, -49.0f), &c), 40);
    CHECK(fabsf(c + 49.0f) < 0.05f);
    CHECK_EQ(tuner_nearest(detune(82.407f, 51.0f), &c), 41); // F2, 49 cents flat
    CHECK(fabsf(c + 49.0f) < 0.05f);

    // A steady low E 8 cents sharp: shown at once, and the needle settles there.
    tuner_smooth_t s;
    tuner_view_t v;
    tuner_smooth_reset(&s);
    uint32_t t = 1000;
    const float e8 = detune(82.407f, 8.0f);
    tuner_smooth_feed(&s, e8, t, &v);
    CHECK(v.note && strcmp(v.name, "E") == 0 && v.octave == 2 && fabsf(v.cents - 8.0f) < 0.1f && !v.in_tune);

    // A stray reading (a coarse one, 5 cents off) in a steady stream barely moves the needle.
    for (int i = 0; i < 5; i++) tuner_smooth_feed(&s, e8, t += 50, &v);
    tuner_smooth_feed(&s, detune(82.407f, 13.0f), t += 50, &v);
    CHECK(fabsf(v.cents - 8.0f) < 0.1f);

    // Tuning down to pitch: the needle eases, then reads in tune.
    for (int i = 0; i < 20; i++) tuner_smooth_feed(&s, detune(82.407f, 1.0f), t += 50, &v);
    CHECK(fabsf(v.cents - 1.0f) < 0.1f && v.in_tune);

    // Straight to the A string: the new note within two readings, not dragged through the E.
    tuner_smooth_feed(&s, 110.0f, t += 50, &v);
    CHECK(v.note && strcmp(v.name, "A") == 0 && fabsf(v.cents) < 0.1f);
    tuner_smooth_feed(&s, 110.0f, t += 50, &v);
    CHECK(strcmp(v.name, "A") == 0 && fabsf(v.cents) < 0.1f && fabsf(v.hz - 110.0f) < 0.01f);

    // The string fades: the note stays up for the hold time, held (dimmed) once readings stop,
    // then goes.
    CHECK(!v.held);
    tuner_smooth_feed(&s, 0, t += 50, &v);
    CHECK(v.note && strcmp(v.name, "A") == 0 && !v.held);
    tuner_smooth_view(&s, t + TUNER_STALE_MS + 50, &v);
    CHECK(v.note && v.held);
    tuner_smooth_view(&s, t + TUNER_HOLD_MS - 100, &v);
    CHECK(v.note);
    tuner_smooth_view(&s, t + TUNER_HOLD_MS + 100, &v);
    CHECK(!v.note);
    tuner_smooth_feed(&s, 0, t + TUNER_HOLD_MS + 200, &v);
    CHECK(!v.note && s.n == 0);
    // The next string after silence starts fresh.
    tuner_smooth_feed(&s, 146.832f, t + 2000, &v);
    CHECK(v.note && strcmp(v.name, "D") == 0 && v.octave == 3);
}
