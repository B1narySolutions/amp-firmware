#include "tuner_note.h"

#include <math.h>
#include <string.h>

static const char *const k_names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

int tuner_nearest(float hz, float *cents) {
    const float semis = 12.0f * log2f(hz / TUNER_A4_HZ);
    const int midi = 69 + (int)lroundf(semis);
    if (cents) *cents = 100.0f * (semis - (float)(midi - 69));
    return midi;
}

const char *tuner_note_name(int midi) { return k_names[((midi % 12) + 12) % 12]; }

int tuner_note_octave(int midi) { return midi / 12 - 1; }

void tuner_smooth_reset(tuner_smooth_t *s) { memset(s, 0, sizeof(*s)); }

static float median(const tuner_smooth_t *s) {
    float v[TUNER_HISTORY];
    memcpy(v, s->hist, sizeof(v[0]) * (size_t)s->n);
    for (int i = 1; i < s->n; i++) { // insertion sort: five values at most
        const float x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
    return s->n % 2 ? v[s->n / 2] : 0.5f * (v[s->n / 2 - 1] + v[s->n / 2]);
}

static void fill(const tuner_smooth_t *s, tuner_view_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s->showing) return;
    out->note = true;
    out->midi = s->midi_shown;
    out->name = tuner_note_name(s->midi_shown);
    out->octave = tuner_note_octave(s->midi_shown);
    out->cents = s->cents_shown;
    out->hz = s->n ? median(s) : 0;
    out->in_tune = fabsf(s->cents_shown) <= TUNER_IN_TUNE_CENTS;
}

static void mark_held(const tuner_smooth_t *s, uint32_t now_ms, tuner_view_t *out) {
    out->held = out->note && now_ms - s->last_ms > TUNER_STALE_MS;
}

void tuner_smooth_view(const tuner_smooth_t *s, uint32_t now_ms, tuner_view_t *out) {
    tuner_smooth_t t = *s;
    if (t.showing && now_ms - t.last_ms > TUNER_HOLD_MS) t.showing = false;
    fill(&t, out);
    mark_held(&t, now_ms, out);
}

void tuner_smooth_feed(tuner_smooth_t *s, float hz, uint32_t now_ms, tuner_view_t *out) {
    if (hz > 0) {
        // A different note: forget the old one's readings.
        if (s->n && fabsf(1200.0f * log2f(hz / median(s))) > 70.0f) s->n = s->pos = 0;
        s->hist[s->pos] = hz;
        s->pos = (s->pos + 1) % TUNER_HISTORY;
        if (s->n < TUNER_HISTORY) s->n++;
        s->last_ms = now_ms;

        float cents;
        const int midi = tuner_nearest(median(s), &cents);
        if (!s->showing || midi != s->midi_shown) {
            s->midi_shown = midi; // a new note: the needle starts where it is, not mid-swing
            s->cents_shown = cents;
        } else {
            s->cents_shown += (cents - s->cents_shown) / 3.0f;
        }
        s->showing = true;
    } else if (s->showing && now_ms - s->last_ms > TUNER_HOLD_MS) {
        s->showing = false;
        s->n = s->pos = 0;
    }
    tuner_smooth_view(s, now_ms, out);
}
