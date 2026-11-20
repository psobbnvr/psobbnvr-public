#pragma once

// The left hand's aim direction for techniques, in game-world space: the
// line from an estimated left shoulder through the left hand, so the aim
// follows the angle of the whole arm and a tilted wrist does not steer it.
// Both the lock-on pick (psobbvr_targetaim.hpp) and the unlocked cast's
// flight direction (psobbvr_techcast.hpp) read it, so they always agree.
//
// The shoulder is placed from the tracked head pose: down along the
// tracking space's vertical, back and to the left along the head's yaw
// only, so looking up or down does not move it. The head and grip poses
// come from the same tracking space in game units. With the hand close to
// the shoulder the line is unstable, so the direction blends toward the
// controller's own pointing direction as the arm shortens.

#include <cmath>

#include "psobbvr_gamecam.hpp"
#include "psobbvr_vr.hpp"

namespace armray {

// Shoulder-to-hand length (metres) at or below which the controller's
// pointing direction is used alone, and at or above which the shoulder line
// is used alone; linear in between.
constexpr float BLEND_NEAR_M = 0.10f;
constexpr float BLEND_FAR_M = 0.25f;

struct Result {
    float dir[3];       // unit aim direction, game world
    float ctrl_dir[3];  // unit controller pointing direction, game world
    float head[3];      // tracking space, game units
    float shoulder[3];  // tracking space, game units
    float hand[3];      // tracking space, game units
    float arm_m;        // shoulder-to-hand length, metres
    float weight;       // 0 = controller pointing only, 1 = shoulder line only
    const char* source; // "controller" | "shoulder" | "blend"
};

namespace detail {

inline bool Normalize(float v[3]) {
    const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len > 1e-6f))
        return false;
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
    return true;
}

inline D3DMATRIX TranslationOnly(const float p[3]) {
    D3DMATRIX m = vrmod::Identity();
    m._41 = p[0];
    m._42 = p[1];
    m._43 = p[2];
    return m;
}

}  // namespace detail

// The left aim direction. False when there is no pose to build it from; the
// caller then runs stock.
inline bool LeftAim(Result& r) {
    using namespace detail;
    D3DMATRIX aim, world;
    if (!vrmod::Get()->GetHandAimPose(0, aim) ||
        !gamecam::WorldFromTracking(aim, world))
        return false;
    r.ctrl_dir[0] = -world._31;
    r.ctrl_dir[1] = -world._32;
    r.ctrl_dir[2] = -world._33;
    if (!Normalize(r.ctrl_dir))
        return false;
    r.head[0] = r.head[1] = r.head[2] = 0.0f;
    r.shoulder[0] = r.shoulder[1] = r.shoulder[2] = 0.0f;
    r.hand[0] = r.hand[1] = r.hand[2] = 0.0f;
    r.arm_m = 0.0f;
    r.weight = 0.0f;
    r.source = "controller";
    r.dir[0] = r.ctrl_dir[0];
    r.dir[1] = r.ctrl_dir[1];
    r.dir[2] = r.ctrl_dir[2];
    if (vrmod::config.cast_aim_ray == 0)
        return true;

    D3DMATRIX grip;
    if (!gamecam::written_this_frame || !vrmod::Get()->GetHandPose(0, grip))
        return true;  // no grip pose: the controller's pointing stands

    // Head yaw's forward on the horizontal plane; kept from the last good
    // frame while the head looks (nearly) straight up or down.
    static float yaw_fwd[2] = {0.0f, -1.0f};
    const D3DMATRIX& head = gamecam::head_at_apply;
    const float hx = -head._31, hz = -head._33;
    const float hlen = sqrtf(hx * hx + hz * hz);
    if (hlen > 0.2f) {
        yaw_fwd[0] = hx / hlen;
        yaw_fwd[1] = hz / hlen;
    }
    const float s = vrmod::config.world_scale;
    const float down = vrmod::config.cast_aim_shoulder_down_m * s;
    const float side = vrmod::config.cast_aim_shoulder_side_m * s;
    const float back = vrmod::config.cast_aim_shoulder_back_m * s;
    // Left of a forward (fx, fz) with y up: (fz, -fx).
    r.head[0] = head._41;
    r.head[1] = head._42;
    r.head[2] = head._43;
    r.shoulder[0] = head._41 - yaw_fwd[0] * back + yaw_fwd[1] * side;
    r.shoulder[1] = head._42 - down;
    r.shoulder[2] = head._43 - yaw_fwd[1] * back - yaw_fwd[0] * side;
    r.hand[0] = grip._41;
    r.hand[1] = grip._42;
    r.hand[2] = grip._43;

    const float dx = r.hand[0] - r.shoulder[0];
    const float dy = r.hand[1] - r.shoulder[1];
    const float dz = r.hand[2] - r.shoulder[2];
    r.arm_m = sqrtf(dx * dx + dy * dy + dz * dz) / (s > 1e-6f ? s : 1.0f);
    float w = (r.arm_m - BLEND_NEAR_M) / (BLEND_FAR_M - BLEND_NEAR_M);
    w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    r.weight = w;

    // Both points through the same mapping; the difference is the arm's
    // direction in the world.
    D3DMATRIX ws, wh;
    if (!gamecam::WorldFromTracking(TranslationOnly(r.shoulder), ws) ||
        !gamecam::WorldFromTracking(TranslationOnly(r.hand), wh))
        return true;
    float line[3] = {wh._41 - ws._41, wh._42 - ws._42, wh._43 - ws._43};
    if (!Normalize(line))
        return true;
    float d[3] = {r.ctrl_dir[0] * (1.0f - w) + line[0] * w,
                  r.ctrl_dir[1] * (1.0f - w) + line[1] * w,
                  r.ctrl_dir[2] * (1.0f - w) + line[2] * w};
    if (!Normalize(d))
        return true;  // the two exactly opposed: keep the controller's
    r.dir[0] = d[0];
    r.dir[1] = d[1];
    r.dir[2] = d[2];
    r.source = w >= 1.0f ? "shoulder" : (w <= 0.0f ? "controller" : "blend");
    return true;
}

}  // namespace armray
