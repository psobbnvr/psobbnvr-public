#pragma once

// First-person head trim: hides the player's own head, face and hair.
//
// At idle the head parts draw within ~0.25 m of the head anchor and
// nothing else comes that close, but in attack lunges the head swings out
// to ~0.5 m while the mag hovers down to ~0.38 m, so position alone cannot
// tell them apart. Position bootstraps; texture identity decides:
//  - Learn zone (trim_radius_m around the anchor): draws are hidden and
//    their stage-0 texture is learned as head content.
//  - Wide zone (trim_wide_m): draws are hidden only if their texture was
//    learned, so the mag and weapon are never hidden.
//  - The body, arms included, draws under the root matrix at the feet,
//    outside both zones.
// The learned set clears on floor change (textures are recreated). Only
// active while VR drives the view. RHW/UI draws must not reach
// SuppressNow (it learns as a side effect).

#include "psobbvr_eyeheight.hpp"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_vr.hpp"

namespace trim {

// Head point and feet in world space; valid only when the view was
// written this frame.
inline bool valid = false;
inline float head[3] = {};
inline float feet[3] = {};

// Zone of the most recent WORLD matrix: 0 = outside, 1 = wide, 2 = learn.
inline int zone = 0;
// Most recent WORLD translation (for the diagnostic logs).
inline float last_world[3] = {};

// The body root matrix sits at the feet (the drop shadow 0.3 units above;
// lunges move it horizontally). When the current WORLD matrix is the body
// root and body_cull is set, the draw paths cull backfaces so the torso
// interior vanishes when the camera is inside it (the game draws
// characters two-sided).
inline bool body_zone = false;
inline DWORD game_cullmode = 3;  // shadows the game's D3DRS_CULLMODE

// Stage-0 texture currently bound (wrapper pointer, identity only - never
// dereferenced).
inline const void* current_texture = nullptr;

// Learned head textures (usually 2-6); the cap limits damage if the anchor
// goes wrong.
inline const void* learned[16] = {};
inline int learned_count = 0;
inline uint32_t last_floor = 0xFFFFFFFF;


// A texture is learned only after this many consecutive frames in the
// learn zone: the head is there every frame, while the mag, a swinging
// weapon or an enemy in melee only pass through.
constexpr int LEARN_STREAK = 30;
struct Candidate {
    const void* tex;
    int last_frame;
    int streak;
};
inline Candidate candidates[8] = {};
inline int frame_counter = 0;

inline void ResetLearned() {
    learned_count = 0;
    for (Candidate& c : candidates)
        c = {};
}

inline void OnFrame() {
    valid = false;
    zone = 0;
    frame_counter++;
    if (vrmod::config.trim_reset_pending) {
        vrmod::config.trim_reset_pending = false;
        ResetLearned();
    }
    if (gamecam::anchor_floor != last_floor) {
        last_floor = gamecam::anchor_floor;
        ResetLearned();
    }
    if (!vrmod::config.trim_head || !gamecam::written_this_frame)
        return;
    const uintptr_t entity = gamecam::ResolveEntity();
    if (entity == 0)
        return;
    const float* entity_feet = reinterpret_cast<const float*>(entity + gamecam::ENTITY_POS_OFFSET);
    feet[0] = entity_feet[0];
    feet[1] = entity_feet[1];
    feet[2] = entity_feet[2];
    head[0] = feet[0];
    // The anchor is the character's real head height (~16 units), not the
    // view's eye height: measured per character with eye_height_auto,
    // else trim_anchor_m (meters, so it scales with world_scale).
    float anchor_units = vrmod::config.trim_anchor_m * vrmod::config.world_scale;
    if (vrmod::config.eye_height_auto && eyeheight::Valid())
        anchor_units = eyeheight::Units();
    head[1] = feet[1] + anchor_units;
    head[2] = feet[2];
    valid = true;
}

// boxed = the viewport is a sub-box (the minimap radar), which draws our
// arrow at our x/z at height 0 - never body or head content.
inline void OnWorldTransform(const D3DMATRIX& m, bool boxed) {
    zone = 0;
    body_zone = false;
    if (!valid || boxed)
        return;
    last_world[0] = m._41;
    last_world[1] = m._42;
    last_world[2] = m._43;
    const float dx = m._41 - head[0];
    const float dy = m._42 - head[1];
    const float dz = m._43 - head[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    const float learn_r = vrmod::config.trim_radius_m * vrmod::config.world_scale;
    const float wide_r = vrmod::config.trim_wide_m * vrmod::config.world_scale;
    if (d2 < learn_r * learn_r)
        zone = 2;
    else if (d2 < wide_r * wide_r)
        zone = 1;
    // Body root: at the feet, allowing horizontal root motion; the
    // vertical window excludes the drop shadow (+0.3 units).
    const float fdy = m._42 - feet[1];
    const float fdx = m._41 - feet[0];
    const float fdz = m._43 - feet[2];
    body_zone = fdy > -0.15f && fdy < 0.15f && fdx * fdx + fdz * fdz < 4.0f;
}

inline void OnSetTexture(DWORD stage, const void* texture) {
    if (stage == 0) {
        current_texture = texture;
        diag::current_texture0 = texture;
    }
}

inline bool Known(const void* t) {
    for (int i = 0; i < learned_count; i++)
        if (learned[i] == t)
            return true;
    return false;
}

// Whether to hide the current world-space draw. Call sites must exclude
// RHW draws first (this learns as a side effect).
inline bool SuppressNow() {
    if (!valid)
        return false;
    if (zone == 0) {
        return false;
    }
    if (zone == 2) {
        // Always hidden here; learning needs a LEARN_STREAK.
        if (current_texture != nullptr && learned_count < 16 && !Known(current_texture)) {
            Candidate* slot = nullptr;
            for (Candidate& c : candidates)
                if (c.tex == current_texture) { slot = &c; break; }
            if (slot == nullptr) {
                slot = &candidates[0];
                for (Candidate& c : candidates)
                    if (c.last_frame < slot->last_frame)
                        slot = &c;
                *slot = { current_texture, 0, 0 };
            }
            if (slot->last_frame != frame_counter) {
                // Allows a stride of 2: BeginScene runs twice per game
                // frame, so world draws land on every other count.
                slot->streak = (slot->last_frame >= frame_counter - 2) ? slot->streak + 1 : 1;
                slot->last_frame = frame_counter;
                if (slot->streak >= LEARN_STREAK) {
                    learned[learned_count++] = current_texture;
                    diag::Log("trim: learned texture %p at (%.1f, %.1f, %.1f), head (%.1f, %.1f, %.1f), %d total",
                              current_texture, last_world[0], last_world[1], last_world[2],
                              head[0], head[1], head[2], learned_count);
                }
            }
        }
        return true;
    }
    return current_texture != nullptr && Known(current_texture);
}

// True when the current world-space draw is the player body and
// [vr] body_cull is set (1 culls clockwise faces, 2 counterclockwise).
// Applied only by the user-pointer draw paths: the body is drawn from
// CPU-built vertex arrays, while a map piece pivoted under the feet (drawn
// from vertex buffers) would also pass the position match. The game draws
// everything with cull mode NONE, so game_cullmode stays valid even though
// the overlay's state-block restore bypasses it.
inline bool BodyCullActive() {
    if (!(valid && body_zone && vrmod::config.body_cull != 0))
        return false;
    diag::bodycull_applied++;
    return true;
}

// Probe-mode log of the cull state at a draw: device cull mode, the
// shadow, the body override and the WORLD translation.
inline void ProbeCull(IDirect3DDevice9* dev, bool body) {
    if (!probe::enabled)
        return;
    DWORD c = 0;
    dev->GetRenderState(D3DRS_CULLMODE, &c);
    probe::Log("cull=%lu shadow=%lu body=%d world=(%.2f,%.2f,%.2f)", c, game_cullmode,
               body ? 1 : 0, last_world[0], last_world[1], last_world[2]);
}

inline DWORD BodyCullMode() {
    return vrmod::config.body_cull == 1 ? D3DCULL_CW : D3DCULL_CCW;
}

} // namespace trim
