# amp-firmware

Firmware for the capstone's portable touchscreen practice guitar amp. The hardware is in
[amp-hardware](https://github.com/B1narySolutions/amp-hardware).

| Folder | What it is | Start with |
| --- | --- | --- |
| [`seed/`](seed/) | The Daisy Seed's audio firmware: five A2-Lite NAM amp models at 48 kHz, the cabinet EQ, the volume and tone knobs, and (in `make link` / `make link-fx`) the touchscreen link, the six effects and the tuner | [`seed/README.md`](seed/README.md) |
| [`touchscreen/`](touchscreen/) | The ESP32-S3 touchscreen's UI (ESP-IDF and LVGL), the link protocol both ends compile (`touchscreen/firmware/main/seed_link_proto/`), and the effects and tuner the Seed runs (`touchscreen/seed-dsp/`) | [`touchscreen/README.md`](touchscreen/README.md) |

This repository joins `realtime-nam-seed3` (now `seed/`) and `esp32-touchscreen-ui` (now
`touchscreen/`), with both histories kept: `git log -- seed/` and `git log -- touchscreen/` show
each one's commits with their original authors.

CI (`.github/workflows/ci.yml`) runs the Seed firmware's `make check` and both link builds on every
pull request and every push to `main`.
