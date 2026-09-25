#pragma once

// Dynamic eye height: the character's head height in game units (~16 on
// a HUmar), measured instead of derived from a fixed meters value, so it
// fits every build and survives world-scale changes.
//
// The game draws the head under its own world matrix, set from the same
// character-model site as the body root (0x844AFD; two matrices per
// character there - root at the feet, head ~16 units up). partmap
// fingerprints the head matrix and feeds its height here once per
// BeginScene pass.
//
// The head animates, so the camera uses a frozen STANDING height: the
// median of recent samples, captured when gamecam's spawn settle window
// ends and kept across anchor resets until the next capture.
//
// Used by gamecam::Apply (eye point) and trim::OnFrame (learn-zone
// anchor), both gated on [vr] eye_height_auto.

#include <algorithm>
#include <cstring>

#include "psobbvr_log.hpp"
#include "psobbvr_vr.hpp"

namespace eyeheight {

// Median window of 48 per-pass samples (~1.6 s; the character draws on
// every other pass at idle). Below MIN_SAMPLES (e.g. a cutscene camera
// looking away) a capture is deferred until enough have arrived.
constexpr int RING = 48;
constexpr int MIN_SAMPLES = 6;

inline float ring[RING] = {};
inline int ring_pos = 0;
inline int ring_count = 0;

// This pass's sample; the game may set the head matrix several times per
// pass, the last one wins.
inline bool pass_sample_valid = false;
inline float pass_sample = 0.0f;
inline float last_sample = 0.0f;

inline bool captured_valid = false;
inline float captured_units = 0.0f;
inline bool capture_pending = false;


inline bool Valid() { return captured_valid; }
inline float Units() { return captured_units; }

// A head-matrix set that passed partmap's fingerprint.
inline void OnSample(float dy_units) {
    pass_sample = dy_units;
    pass_sample_valid = true;
}

inline float MedianUnits() {  // 0 when the ring is empty
    if (ring_count == 0)
        return 0.0f;
    float sorted[RING];
    memcpy(sorted, ring, sizeof(float) * ring_count);
    std::nth_element(sorted, sorted + ring_count / 2, sorted + ring_count);
    return sorted[ring_count / 2];
}

inline void CaptureStanding(const char* reason) {
    if (ring_count < MIN_SAMPLES) {
        capture_pending = true;
        diag::Log("eyeheight: capture (%s) deferred - %d/%d samples so far",
                  reason, ring_count, MIN_SAMPLES);
        return;
    }
    captured_units = MedianUnits();
    captured_valid = true;
    capture_pending = false;
    diag::Log("eyeheight: captured standing head %.2f units (%.3f m at scale %.2f), %d samples (%s)",
              captured_units, captured_units / vrmod::config.world_scale,
              vrmod::config.world_scale, ring_count, reason);
}

// Fresh spawn: empty the ring so areas and poses don't mix. The captured
// value is kept until the next settle re-captures.
inline void OnAnchorReset() {
    ring_count = 0;
    ring_pos = 0;
    pass_sample_valid = false;
    capture_pending = false;
}

// Once per BeginScene pass (from partmap::OnFrame): pushes the last
// pass's sample into the ring and services the developer tools' requests.
// aim_dy_units =
// the entity aim point's height above the feet (entity +0x78, logged for
// comparison), or <= -999 when unreadable.
inline void OnFrame(float aim_dy_units) {
    if (pass_sample_valid) {
        ring[ring_pos] = pass_sample;
        ring_pos = (ring_pos + 1) % RING;
        if (ring_count < RING)
            ring_count++;
        last_sample = pass_sample;
    }
    pass_sample_valid = false;
    if (capture_pending && ring_count >= MIN_SAMPLES)
        CaptureStanding("deferred");
}

} // namespace eyeheight
