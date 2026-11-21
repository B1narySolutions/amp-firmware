# CLAUDE.md

This repository holds two firmwares for the capstone amp, each with its own build and rules:

- `seed/`: the Daisy Seed audio firmware. Read `seed/CLAUDE.md` before changing anything there, and
  run its commands from `seed/`.
- `touchscreen/`: the ESP32-S3 touchscreen. Read `touchscreen/docs/HANDOFF.md` first.

The link between them is one protocol, `touchscreen/firmware/main/seed_link_proto/`, compiled
unchanged by both: the touchscreen's ESP-IDF build and the Seed's build, which also compiles
`touchscreen/seed-dsp/effects` and `tuner`. A change to any of those is a change to both ends:
build the Seed (CI does) and run the touchscreen's host tests (`touchscreen/tests/host/`).

Hardware context (pins, audio routing, power) is in
[amp-hardware](https://github.com/B1narySolutions/amp-hardware).
