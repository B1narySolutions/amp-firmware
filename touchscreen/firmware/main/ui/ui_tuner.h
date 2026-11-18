#pragma once
#include <stdbool.h>
#include "lvgl.h"

// The tuner page (docs/TUNER.md): a full-screen page over everything else, built when it opens and
// deleted when it closes (its LVGL objects would otherwise sit in scarce internal RAM). While open
// it asks the Seed for pitch readings (seed_link_set_tuner) and shows the note, a cents needle and
// the input level. Open it only when seed_link_get_tuner() reports the Seed supports it.
void ui_tuner_open(void);
void ui_tuner_close(void);
bool ui_tuner_is_open(void);
