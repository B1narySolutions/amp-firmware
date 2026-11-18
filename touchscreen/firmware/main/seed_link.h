#pragma once
#include <stdbool.h>
#include <stdint.h>

// ESP32-S3 <-> Daisy Seed ("SEED3", running NAM) link over UART, protocol v1
// (docs/SEED_LINK_PROTOCOL.md). Test Mode, the DIAG line and the IN/OUT meters read everything
// through seed_link_get_stats(); audio state reaches the Seed through rig_state, which wakes
// the link task on every change. Nothing here blocks: callers in the LVGL task only copy
// counters or raise flags.
typedef struct {
    bool     connected;          // HELLO received and a valid frame within the last 3 s
    uint32_t packets_tx;
    uint32_t packets_rx;
    uint32_t link_errors;        // frames dropped by the decoder (bad COBS/CRC/length/version)
    float    rtt_ms;             // last PING round trip, 0 until measured
    char     dsp_fw_version[17];
    uint32_t sample_rate_hz;
    uint16_t block_size;
    float    dsp_cpu_pct;        // average DSP load, % of the audio block budget
    char     nam_model[40];
    float    input_peak_dbfs;
    float    output_peak_dbfs;
    uint32_t clip_count;

    // Added with the UART transport
    bool     peer_mock;          // the peer is tools/mock_seed.py: STATUS and METERS are simulated
    bool     meters_valid;       // a METERS frame arrived within the last second
    bool     status_valid;       // a STATUS arrived since this Seed booted (else the DSP/model fields mean nothing)
    uint32_t peer_boot_id;
    uint32_t uart_overruns;      // bytes lost in the UART driver before the decoder saw them
    uint32_t snapshots_sent;
    uint16_t snapshot_applied;   // last snapshot the Seed reported as fully applied
    uint32_t dsp_overruns;       // audio callback overruns the Seed reports since its boot
    float    dsp_cpu_peak_pct;
    uint8_t  status_flags;       // SLP_STATUS_* from the last STATUS
    uint8_t  active_kind;        // the model the Seed reports running: SLP_MODEL_*
    uint8_t  active_builtin;     // its AmpId, for built-ins
    uint32_t active_hash;        // its CRC32, for uploaded SD profiles
    uint8_t  upload_state;       // SEED_UPLOAD_*
    uint8_t  upload_pct;
    uint32_t upload_hash;        // the SD profile being (or last) uploaded
    char     upload_note[48];    // its name while running, the reason after a failure
} seed_link_stats_t;

enum { SEED_UPLOAD_NONE, SEED_UPLOAD_RUNNING, SEED_UPLOAD_DONE, SEED_UPLOAD_FAILED };

// Starts the link task. Call once, after rig_state holds the boot state.
void seed_link_init(void);

void seed_link_get_stats(seed_link_stats_t *out);
// Requests a ping; rtt_ms updates when the reply arrives. Returns false if not connected.
bool seed_link_ping(void);
void seed_link_reset_counters(void);
// Human-readable transport description for Test Mode, e.g. "UART0 1000000 baud, GPIO43/44".
const char *seed_link_transport(void);

// The Seed's built-in models, as announced in its HELLO (AmpId order). Returns the count copied.
// The version number changes whenever the list changes, so the UI knows to rebuild.
#define SEED_LINK_MAX_BUILTINS 8
typedef struct {
    uint8_t id;
    char name[25];
} seed_link_builtin_t;
int seed_link_get_builtins(seed_link_builtin_t *out, int max, uint32_t *version);

// "Seed controls only" (any task): while hidden, every effect but the AMP and the CAB is sent to
// the Seed as off, so an effects build of the Seed plays only what the page shows. The rig keeps
// the effects' own settings; showing them again sends those.
void seed_link_set_effects_hidden(bool hidden);

// Tuner (docs/TUNER.md): the Seed hears the guitar and sends its pitch; this side only shows it.
// seed_link_set_tuner (any task): the tuner page opened (on) or closed. The link sends it at once
// and repeats it each second while on; a Seed that stops hearing it turns its tuner off and
// unmutes after 2.5 s.
void seed_link_set_tuner(bool on, bool mute);
typedef struct {
    bool  connected;   // the link is up
    bool  supported;   // connected to a Seed that reports SLP_STATUS_HAS_TUNER
    bool  fresh;       // a reading arrived within the last 300 ms
    float freq_hz;     // 0 = no clear pitch
    float level_dbfs;  // input RMS
    uint8_t clarity;   // 0..100
    bool  mock;        // from tools/mock_seed.py: simulated
    uint32_t seq;      // counts readings: a new value means a new reading
} seed_link_tuner_t;
void seed_link_get_tuner(seed_link_tuner_t *out);
