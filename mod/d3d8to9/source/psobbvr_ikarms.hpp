// IK arms: the character's own arms reach for the controllers.
//
// The game skins the player's body on the CPU while it walks the bone
// tree: per bone it applies the bone's transform to the matrix stack top,
// calls the per-bone callback (0x7A9E0C, which copies the top into the
// entity's bone array), then skins that bone from the top; children push
// from it. Rewriting the top inside the callback therefore poses the bone,
// its skin and everything below it. All player bodies share one 64-bone
// skeleton.
//
// Only the local player's draw walks (0x7AA8C8, 0x7AA984 at the other LOD)
// are posed; the draw-free walks and other characters keep the game's
// pose. Per arm (right / left bones):
//  - upper arm 41 / 28 and forearm 42 / 29: a two-bone solve from the
//    shoulder to the wrist target, the elbow pushed toward a pole
//    direction. Bones run along +X and hinge about Z; the forearm bends
//    toward the upper arm's -Y. The wrist can sit off the forearm's axis
//    (the HUcaseal's and RAcast's by 10-15 degrees, their forearm pieces
//    being modelled along X), so the forearm is placed to put that point,
//    not its axis, on the target. The forearm also rolls by ik_twist of the
//    hand's roll, about the elbow-to-wrist line. With [vr] ik_elbow the
//    elbow first turns about the shoulder-to-wrist line by a share of that
//    roll (ElbowSwivel), as a person's elbow lifts or tucks when the wrist
//    runs out of twist.
//  - wrist 44 / 31: placed so the grip point (48 / 35) lands on the
//    controller's grip frame, the one the weapon is seated on
//    (weapongrip::ActiveGripOffset); with [vr] gun_seat, for a held gun, on
//    inverse(gun seat) x that frame, so the hand holds the gun by its handle
//    (weapongrip::GunSeatForHand; with [vr] gun_to_hand the gun's frame is
//    itself built from the hand, so that lands where the hand holds a melee
//    weapon, and a left hand with no gun half in it is placed as an empty
//    hand; with [vr] bare_hand_on_line an empty hand is where it holds a
//    weapon); all of it in weapongrip::HandOffset. With [vr]
//    melee_hand_seat, for a held melee weapon, on the frame that keeps the
//    weapon in the fist while the weapon stays on the controller's line
//    (weapongrip::HandOnMelee).
//
// The solve runs in character space (stack top x inverse(walk root), the
// root being the stack top at the walk's start: feet, facing and body
// proportions), where bones are rigid. Stack matrices are camera-relative,
// so world targets enter as world x inverse(root x [0xACD420]). Bone
// lengths and the wrist-to-grip relation are measured from the walks.
//
// The controllers are at life-size distances from the eyes while the arm is
// the character's size (0.7x to 1.6x the player's across the classes and
// builds), so the upper arm and forearm are lengthened or shortened along
// their bones to the player's reach (LengthFactor), and stretched a little
// more rather than leave the hand off the wrist. The game's skinning
// stretches the sleeve; the hand keeps its size.
//
// [vr] ik_arms: 1 on, 2 a fixed test pose. ik_anchor holds the shoulders
// and hand bones at the idle pose (TrackIdle, HoldHand); its mode 2 also
// moves the shoulders with the headset (BeginWalk). ik_raise_cm and
// ik_shoulder_*_cm sit the arms higher under the raised camera.
// ik_reach_cm, ik_arm_match and ik_stretch_max size the arm. ik_torso turns
// the chest, and the shoulders and arms on it, toward the player's own torso
// (TorsoStep), the head chains staying under the camera.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "MinHook.h"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_weapongrip.hpp"

namespace ikarms {

// The local player's draw walks (thiscall, this = the entity): set the
// bone cursor to [entity+0xE0], install the callback 0x7A9E0C, walk the
// tree [entity+0x34] (0x83046C, or the blended 0x8302C4), clear it. The
// player draw 0x689DEC calls 0x7AA984 at LOD 2, else 0x7AA8C8.
constexpr uintptr_t PLAYER_DRAW_WALK = 0x007AA8C8;
constexpr uintptr_t PLAYER_DRAW_WALK_LOD = 0x007AA984;
// The per-bone callback (cdecl, the node as its argument, unused): copies
// the stack top to the cursor (0x831650) and advances the cursor by 0x40.
constexpr uintptr_t BONE_CALLBACK = 0x007A9E0C;
// The bone-array cursor the callback writes through.
constexpr uintptr_t BONE_CURSOR_ADDR = 0x00AAB530;
// The matrix stack's top pointer (64-byte entries; push 0x82D7B0, pop
// 0x82DA80).
constexpr uintptr_t MATRIX_STACK_TOP_ADDR = 0x00AF0328;
// inverse(view), stored with the view by 0x837688; the model draw's base
// setup (0x844B14) places the walk's vertices with root x this.
constexpr uintptr_t INVERSE_VIEW_ADDR = 0x00ACD420;
// Entity fields: the bone array (64 bytes per bone, walk order) and the
// body's model tree.
constexpr uintptr_t ENTITY_BONES_OFFSET = 0xE0;
constexpr uintptr_t ENTITY_TREE_OFFSET = 0x34;
constexpr int BONE_COUNT = 64;

// The arm chains in the shared skeleton, indexed like the controllers
// (0 = left, 1 = right). The wrist's subtree is contiguous in walk order:
// the pivot (32 / 45, the wrist bend), its end node, the hand piece
// (34 / 47) and the grip point.
// The other children of the upper arm and forearm have no skin of their
// own: the upper arm's attachment point (37 / 50; shields hang on 37), and
// the forearm's end node (30 / 43) and attachment point (36 / 49).
constexpr int HAND_BONE_COUNT = 4;
struct ArmBones {
    int upper, fore, wrist, grip, hand_first;
    int upper_attach;
    int fore_attach[2];
};
constexpr ArmBones ARM_BONES[2] = {{28, 29, 31, 35, 32, 37, {30, 36}},
                                   {41, 42, 44, 48, 45, 50, {43, 49}}};
// The arm's length factor stays within this range of the character's own.
constexpr float LENGTH_FACTOR_MIN = 0.5f;
constexpr float LENGTH_FACTOR_MAX = 2.0f;
// The chest (parent of both shoulders, the neck and the head) and the two
// chains on it that lead to the head.
constexpr int CHEST_BONE = 25;
constexpr int NECK_FRONT_BONE = 52;
constexpr int HEAD_CHAIN_BONE = 55;
// The shoulder joints (parents of the arm chains), left and right.
constexpr int SHOULDER_BONES[2] = {26, 39};
// Idle detection for ik_anchor: the animated shoulder (character space,
// model units) moving at most this far between walks, for this long.
constexpr float IDLE_STEP = 0.03f;
constexpr double IDLE_HOLD_S = 0.6;
// [entity+0x32E] word = the action mode (1 = idle, 5..7 = the melee
// attack steps); entity+0x38 = the feet position.
constexpr uintptr_t ENTITY_ACTION_MODE_OFFSET = 0x32E;
constexpr short ACTION_MODE_IDLE = 1;
constexpr uintptr_t ENTITY_FEET_OFFSET = 0x38;
constexpr float FEET_STILL_STEP = 0.01f;  // units between walks
// [entity+0x30] bit 0x100000 = the battle stance: set each tick by 0x6898CC
// while the targeted enemy is near and ahead, cleared once it is lost or
// farther off. The stand setup 0x6A1C8C picks its idle clip by it (0x69D1BC:
// the weapon object's vtbl+0x48 battle idle, else vtbl+0x54), so standing near
// an enemy plays a hunched, arms-in idle that is not the rest pose.
constexpr uintptr_t ENTITY_STATE_FLAGS_OFFSET = 0x30;
constexpr uint32_t STATE_BATTLE_STANCE = 0x00100000;
// [entity+0xB8] word = the current animation clip (written by 0x694390).
constexpr uintptr_t ENTITY_CLIP_OFFSET = 0xB8;

struct Vec3 {
    float x, y, z;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(Vec3 a) { return sqrtf(Dot(a, a)); }
inline Vec3 Row(const D3DMATRIX& m, int r) {
    const float* p = &m._11 + r * 4;
    return {p[0], p[1], p[2]};
}
inline Vec3 TransformPoint(Vec3 p, const D3DMATRIX& m) {
    return Row(m, 0) * p.x + Row(m, 1) * p.y + Row(m, 2) * p.z + Row(m, 3);
}
inline void SetRow(D3DMATRIX& m, int r, Vec3 v) {
    float* p = &m._11 + r * 4;
    p[0] = v.x;
    p[1] = v.y;
    p[2] = v.z;
}
inline D3DMATRIX Frame(Vec3 x, Vec3 y, Vec3 z, Vec3 t) {
    D3DMATRIX m = vrmod::Identity();
    SetRow(m, 0, x);
    SetRow(m, 1, y);
    SetRow(m, 2, z);
    SetRow(m, 3, t);
    return m;
}

// Inverse of an affine row-vector matrix (rotation, scale, translation).
inline bool AffineInverse(const D3DMATRIX& m, D3DMATRIX& out) {
    const float a = m._11, b = m._12, c = m._13;
    const float d = m._21, e = m._22, f = m._23;
    const float g = m._31, h = m._32, i = m._33;
    const float c0 = e * i - f * h, c1 = f * g - d * i, c2 = d * h - e * g;
    const float det = a * c0 + b * c1 + c * c2;
    if (fabsf(det) < 1e-12f)
        return false;
    const float k = 1.0f / det;
    out = vrmod::Identity();
    out._11 = c0 * k; out._12 = (c * h - b * i) * k; out._13 = (b * f - c * e) * k;
    out._21 = c1 * k; out._22 = (a * i - c * g) * k; out._23 = (c * d - a * f) * k;
    out._31 = c2 * k; out._32 = (b * g - a * h) * k; out._33 = (a * e - b * d) * k;
    out._41 = -(m._41 * out._11 + m._42 * out._21 + m._43 * out._31);
    out._42 = -(m._41 * out._12 + m._42 * out._22 + m._43 * out._32);
    out._43 = -(m._41 * out._13 + m._42 * out._23 + m._43 * out._33);
    return true;
}

// Rotation rows made orthonormal and right-handed (z = x cross y, as in
// every bone frame of the skeleton), translation kept.
inline bool Orthonormalize(D3DMATRIX& m) {
    Vec3 x = Row(m, 0), y = Row(m, 1);
    const float lx = Length(x);
    if (lx < 1e-6f)
        return false;
    x = x * (1.0f / lx);
    y = y - x * Dot(y, x);
    const float ly = Length(y);
    if (ly < 1e-6f)
        return false;
    y = y * (1.0f / ly);
    SetRow(m, 0, x);
    SetRow(m, 1, y);
    SetRow(m, 2, Cross(x, y));
    return true;
}

// Torso twist. The body walk turns the character's chest, and everything on
// it, about the vertical by the angle between the character's facing and the
// player's real torso, so the shoulders stay with the hands when the body
// faces another way from the player. The torso's direction is estimated in
// tracking space on the horizontal plane (x, z; forward = -z at the
// session's start) from the head and the two hands. The functions below take
// everything as arguments.
struct TorsoParams {
    float neck_back;                   // head to the neck point (tracking units)
    float hands_near, hands_far;       // the hands cue's ramp (tracking units)
    float smooth_s;                    // the follow time
    float hands;                       // the hands cue's weight (0..1)
    float neck_range;                  // the head's turn from the torso (radians)
};
struct TorsoInput {
    Vec3 head;                         // head position
    float head_fx, head_fz;            // head forward on the plane (any length)
    bool have_hands;
    Vec3 hand[2];
};
struct TorsoState {
    bool valid = false;
    float tx = 0.0f, tz = -1.0f;       // the torso's direction (unit)
    float conf = 0.0f;                 // the hands cue's confidence last step
    float cue_x = 0.0f, cue_z = 0.0f;  // the hands cue's direction
    float head_x = 0.0f, head_z = -1.0f;
};

inline float SmoothStep01(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

// (x, z) turned by `a` radians; positive turns +x toward +z.
inline void Turn2(float& x, float& z, float a) {
    const float c = cosf(a), s = sinf(a);
    const float nx = x * c - z * s, nz = x * s + z * c;
    x = nx;
    z = nz;
}

// The signed angle that turns (ax, az) onto (bx, bz), in the sense of Turn2.
inline float Angle2(float ax, float az, float bx, float bz) {
    return atan2f(ax * bz - az * bx, ax * bx + az * bz);
}

// One step of the estimate. The torso is seeded from the head's yaw (call
// with a fresh state after a recenter). The hands held out
// in front of the neck point pull it along its follow time; a head turned
// past the neck's range drags it, except while the hands vouch for it (a
// seated player looking aside keeps the hands in front of the real chest).
inline void TorsoStep(TorsoState& s, const TorsoInput& in, const TorsoParams& p, float dt) {
    float hx = in.head_fx, hz = in.head_fz;
    const float hn = sqrtf(hx * hx + hz * hz);
    const bool head_ok = hn > 0.2f;  // looking straight up or down gives no yaw
    if (head_ok) {
        hx /= hn;
        hz /= hn;
        s.head_x = hx;
        s.head_z = hz;
    }
    if (!s.valid) {
        if (!head_ok)
            return;
        s.tx = hx;
        s.tz = hz;
        s.conf = 0.0f;
        s.valid = true;
        return;
    }
    // The neck point sits behind the head, so turning the head does not
    // swing the reference.
    const float fx = head_ok ? hx : s.tx, fz = head_ok ? hz : s.tz;
    const float nx = in.head.x - fx * p.neck_back, nz = in.head.z - fz * p.neck_back;
    s.conf = 0.0f;
    if (in.have_hands && p.hands > 0.0f) {
        const float mx = 0.5f * (in.hand[0].x + in.hand[1].x);
        const float mz = 0.5f * (in.hand[0].z + in.hand[1].z);
        const float vx = mx - nx, vz = mz - nz;
        const float d = sqrtf(vx * vx + vz * vz);
        const float a0 = (in.hand[0].x - nx) * s.tx + (in.hand[0].z - nz) * s.tz;
        const float a1 = (in.hand[1].x - nx) * s.tx + (in.hand[1].z - nz) * s.tz;
        const float am = vx * s.tx + vz * s.tz;
        if (a0 > 0.0f && a1 > 0.0f && d >= p.hands_near && d > 1e-5f) {
            const float span = p.hands_far - p.hands_near;
            float conf = span > 1e-5f ? SmoothStep01((am - p.hands_near) / span)
                                      : (am >= p.hands_near ? 1.0f : 0.0f);
            conf *= p.hands;
            if (conf > 1.0f) conf = 1.0f;
            s.conf = conf;
            s.cue_x = vx / d;
            s.cue_z = vz / d;
        }
    }
    if (s.conf > 0.0f) {
        const float tau = p.smooth_s > 1e-3f ? p.smooth_s : 1e-3f;
        const float k = 1.0f - expf(-dt * s.conf / tau);
        Turn2(s.tx, s.tz, Angle2(s.tx, s.tz, s.cue_x, s.cue_z) * k);
    }
    if (head_ok) {
        const float a = Angle2(s.tx, s.tz, hx, hz);
        const float limit = p.neck_range + s.conf * (3.14159265f - p.neck_range);
        if (fabsf(a) > limit) {
            // The head sits at the neck's limit on its own side of the torso.
            s.tx = hx;
            s.tz = hz;
            Turn2(s.tx, s.tz, a > 0.0f ? -limit : limit);
        }
    }
    const float n = sqrtf(s.tx * s.tx + s.tz * s.tz);
    if (n > 1e-6f) {
        s.tx /= n;
        s.tz /= n;
    }
}

// The torso's direction as an angle about the character's vertical from its
// +z (positive toward +x, the model's left): the direction carried through
// the rotation parts of the world-from-tracking and character-from-world
// transforms. False when it comes out vertical.
inline bool TorsoAngleInCharacter(float tx, float tz, const D3DMATRIX& world_from_tracking,
                                  const D3DMATRIX& char_from_world, float& angle) {
    const Vec3 w = Row(world_from_tracking, 0) * tx + Row(world_from_tracking, 2) * tz;
    const Vec3 c = Row(char_from_world, 0) * w.x + Row(char_from_world, 1) * w.y +
                   Row(char_from_world, 2) * w.z;
    if (c.x * c.x + c.z * c.z < 1e-12f)
        return false;
    angle = atan2f(c.x, c.z);
    return true;
}

inline float ClampAngle(float a, float limit) {
    return a > limit ? limit : (a < -limit ? -limit : a);
}

// `prev` moved toward `target` by at most `max_step`.
inline float EaseAngle(float prev, float target, float max_step) {
    const float d = target - prev;
    return d > max_step ? prev + max_step : (d < -max_step ? prev - max_step : target);
}

// A direction turned about the vertical by the angle whose sine and cosine
// are given (positive toward +x from +z; the character's frame: +z forward,
// +y up, the model's left +x).
inline Vec3 TurnDir(Vec3 v, float s, float c) {
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}
inline Vec3 TurnPoint(Vec3 p, Vec3 pivot, float s, float c) {
    return pivot + TurnDir(p - pivot, s, c);
}
// A bone frame turned the same way about `pivot` (rows and translation).
inline D3DMATRIX TurnFrame(const D3DMATRIX& m, Vec3 pivot, float s, float c) {
    D3DMATRIX r = m;
    for (int i = 0; i < 3; i++)
        SetRow(r, i, TurnDir(Row(m, i), s, c));
    SetRow(r, 3, TurnPoint(Row(m, 3), pivot, s, c));
    return r;
}

using WalkFn = void(__fastcall*)(void* entity, void* edx);
using BoneCallbackFn = void(__cdecl*)(void* node);
inline WalkFn original_walk = nullptr;
inline WalkFn original_walk_lod = nullptr;
inline BoneCallbackFn original_bone_callback = nullptr;
inline bool installed = false;

struct Arm {
    // Measured from the walks (constant per body): the bone lengths and the
    // wrist's frame relative to the grip point's (wrist x inverse(grip),
    // character space, last walk - the hand's own animation).
    float upper_len = 0.0f, fore_len = 0.0f;
    bool have_upper_len = false, have_fore_len = false;
    // The wrist in the forearm's own frame (fore_len is its length).
    Vec3 fore_off = {0.0f, 0.0f, 0.0f};
    D3DMATRIX wrist_from_grip = {};
    bool have_wrist_from_grip = false;
    // This frame's target: the grip point's world frame.
    D3DMATRIX target_world = {};
    bool have_target = false;
    // This walk: the solved frames (rigid), the length scale along the
    // upper arm and forearm, and what each bone ended up as (the upper arm
    // and forearm with that scale).
    bool posed = false;
    D3DMATRIX upper_out = {}, fore_out = {}, wrist_out = {};
    float length_scale = 1.0f;
    D3DMATRIX upper_used = {}, fore_used = {}, wrist_used = {};
    // The last good pole side, for a target lined up with the pole.
    Vec3 prev_pole_side = {0.0f, 0.0f, 0.0f};
    bool have_prev_pole_side = false;
    // The hand's roll about the forearm last walk (radians, unwrapped), so
    // a roll past half a turn saturates instead of flipping.
    float prev_roll = 0.0f;
    bool have_prev_roll = false;
    // ik_elbow: the same reading against the forearm of the fixed-pole
    // solve, and the elbow's eased turn about the shoulder-to-wrist line.
    float prev_base_roll = 0.0f;
    bool have_prev_base_roll = false;
    float swivel = 0.0f;
    double swivel_time = 0.0;
    bool have_swivel = false;
    // ik_anchor: the idle shoulder (animated, before our shifts), the hand
    // bones relative to the wrist and the wrist-to-grip relation, and the
    // stillness tracking that records them (recording_idle: this walk is
    // an idle one, so its hand pose is recorded).
    Vec3 idle_shoulder = {0.0f, 0.0f, 0.0f};
    bool have_idle_shoulder = false;
    D3DMATRIX idle_wrist_from_grip = {};
    bool have_idle_wrist = false;
    D3DMATRIX idle_hand[HAND_BONE_COUNT] = {};
    unsigned idle_hand_mask = 0;  // bit k: idle_hand[k] recorded
    bool recording_idle = false;
    int idle_clip = -1;           // the animation clip the whole pose was last recorded in
    // This walk, for the trace: how far a recorded hand bone moved from its
    // last record, and how far the animated hand sat from the held one.
    float walk_record_step = 0.0f;
    float walk_hold_gap = 0.0f;
    Vec3 prev_anim_shoulder = {0.0f, 0.0f, 0.0f};
    bool have_prev_anim_shoulder = false;
    double still_since = 0.0;  // 0 = moving
    // Diagnostics.
    float last_anchor_gap = 0.0f;  // held shoulder vs the animated one
    float last_reach = 0.0f;       // shoulder-to-wrist-target distance
    float last_grip_error = 0.0f;  // posed grip point vs the target
    float last_roll_deg = 0.0f;    // the hand's roll about the forearm
    float last_base_roll_deg = 0.0f;  // the same before the elbow's turn
    float last_swivel_deg = 0.0f;  // the elbow's turn about the reach
    float last_fit = 1.0f;         // length factor for the player's reach
    float last_stretch = 1.0f;     // extra stretch past it
};
inline Arm arms[2];

// The model the measurements belong to.
inline uintptr_t model_entity = 0;
inline uintptr_t model_tree = 0;

// Entering or leaving a game builds a new player entity, but the same
// character has the same skeleton and idle animation, so its rest pose is
// carried over: the old model's is stashed with its character key, and
// restored once the new model's first walk has measured bone lengths that
// match. The character's visual block (entity +0x940): class byte +0x961,
// proportion sliders +0x978 / +0x97C.
constexpr uintptr_t ENTITY_CLASS_OFFSET = 0x961;
constexpr uintptr_t ENTITY_PROPORTION_OFFSET = 0x978;
constexpr float REST_LENGTH_EPS = 0.01f;  // model units
struct CharacterKey {
    bool valid;
    int class_id;
    float proportion[2];
};
struct RestPose {
    Vec3 idle_shoulder;
    bool have_idle_shoulder;
    D3DMATRIX idle_wrist_from_grip;
    bool have_idle_wrist;
    D3DMATRIX idle_hand[HAND_BONE_COUNT];
    unsigned idle_hand_mask;
    float upper_len, fore_len;
};
inline CharacterKey model_key = {};
inline CharacterKey stash_key = {};
inline RestPose stash[2] = {};
inline bool stash_pending = false;

inline CharacterKey ReadCharacterKey(uintptr_t entity) {
    CharacterKey k = {};
    if (diag::Accessible(entity + ENTITY_CLASS_OFFSET, 1, false) &&
        diag::Accessible(entity + ENTITY_PROPORTION_OFFSET, 8, false)) {
        k.class_id = *reinterpret_cast<const uint8_t*>(entity + ENTITY_CLASS_OFFSET);
        memcpy(k.proportion, reinterpret_cast<const void*>(entity + ENTITY_PROPORTION_OFFSET),
               sizeof(k.proportion));
        k.valid = true;
    }
    return k;
}

inline bool SameCharacter(const CharacterKey& a, const CharacterKey& b) {
    return a.valid && b.valid && a.class_id == b.class_id &&
           a.proportion[0] == b.proportion[0] && a.proportion[1] == b.proportion[1];
}

// Walk state.
inline bool walk_active = false;
inline uintptr_t walk_bones = 0;
inline D3DMATRIX walk_root = {};             // camera-relative
inline D3DMATRIX walk_root_inv = {};
inline D3DMATRIX walk_char_from_world = {};  // inverse(walk root x inverse view)
inline float walk_char_per_world = 1.0f;     // its scale
inline int walk_callbacks = 0;
inline float walk_raise = 0.0f;              // ik_raise_cm in model units
inline Vec3 walk_shoulder = {0.0f, 0.0f, 0.0f};  // the right shoulder's shift, model units
// The game says idle in the relaxed stance and the feet have not moved since
// the last walk.
inline bool walk_idle_state = false;
inline float prev_feet[3] = {};
inline bool have_prev_feet = false;
// The battle stance this walk, and whether the current clip began outside it
// with the stance off ever since (the clip chosen at its start stays until
// the next one, so the flag alone does not say which idle is playing).
inline bool walk_battle_stance = false;
inline bool walk_relaxed_clip = false;
inline short prev_clip = 0, prev_mode = 0;
inline bool have_prev_clip = false;
inline int walk_clip = -1;  // this walk's animation clip, -1 unreadable
// ik_anchor 2: this frame's neck point (world), and the shoulders' move
// for this walk (character space).
inline Vec3 neck_world = {0.0f, 0.0f, 0.0f};
inline bool have_neck = false;
inline Vec3 walk_head_shift = {0.0f, 0.0f, 0.0f};
// The torso estimate (tracking space), refreshed each frame, and the
// recenter count and time it was last stepped at.
inline TorsoState torso;
inline unsigned torso_recenters = 0;
inline double torso_step_time = 0.0;
// The upper body's twist this walk (radians, sine, cosine), the walk before's
// for the rate limit, and the chest's origin as the walk reaches it, which
// the twist turns about.
inline float walk_twist = 0.0f, walk_twist_sin = 0.0f, walk_twist_cos = 1.0f;
inline float walk_twist_raw = 0.0f;  // before the clamp
inline float twist_prev = 0.0f;
inline double twist_prev_time = 0.0;
inline bool have_twist_prev = false;
inline Vec3 walk_chest_pivot = {0.0f, 0.0f, 0.0f};
inline bool have_chest_pivot = false;

// Counters (per pass, plus the last pass's for the status line).
inline int walks_this_pass = 0, walks_last_pass = 0;
inline int posed_this_pass = 0, posed_last_pass = 0;
inline int callbacks_last_walk = 0;
inline bool logged_first_pose = false;
inline int trace_walks = 0;  // developer build: log this many walks

inline bool Active() {
    return installed && vrmod::config.ik_arms != 0 && gamecam::DrivesView();
}

// Whether a hand's bones are being held at the recorded idle pose (the hand
// is then one rigid piece at a fixed place in the wrist's frame), for the
// hand capture's log: 2 = the idle pose is being recorded (the hand is as
// animated), 1 = recorded and held, 0 = nothing recorded yet.
inline int HandHoldState(int side) {
    const Arm& a = arms[side];
    if (a.recording_idle)
        return 2;
    return a.idle_hand_mask != 0 ? 1 : 0;
}

inline void ResetModel() {
    for (Arm& a : arms) {
        a.have_upper_len = a.have_fore_len = false;
        a.have_wrist_from_grip = false;
        a.have_prev_pole_side = false;
        a.have_prev_roll = false;
        a.have_prev_base_roll = false;
        a.have_swivel = false;
        a.have_idle_shoulder = false;
        a.have_idle_wrist = false;
        a.idle_hand_mask = 0;
        a.idle_clip = -1;
        a.have_prev_anim_shoulder = false;
        a.still_since = 0.0;
    }
    have_prev_clip = false;
    logged_first_pose = false;
    have_twist_prev = false;  // the torso estimate carries over: the player has not moved
}

// A new model replaces the current one: keep the rest pose for the check
// after its first walk. A model that never recorded one leaves an earlier
// stash pending.
inline void StashRestPose() {
    if (!arms[0].have_idle_shoulder && !arms[1].have_idle_shoulder)
        return;
    for (int side = 0; side < 2; side++) {
        const Arm& a = arms[side];
        RestPose& s = stash[side];
        s.idle_shoulder = a.idle_shoulder;
        s.have_idle_shoulder = a.have_idle_shoulder;
        s.idle_wrist_from_grip = a.idle_wrist_from_grip;
        s.have_idle_wrist = a.have_idle_wrist;
        memcpy(s.idle_hand, a.idle_hand, sizeof(s.idle_hand));
        s.idle_hand_mask = a.idle_hand_mask;
        s.upper_len = a.upper_len;
        s.fore_len = a.fore_len;
    }
    stash_key = model_key;
    stash_pending = true;
}

// After a walk: once the new model's bone lengths are measured, restore
// the stashed rest pose if it is the same character, else drop it.
inline void CheckRestStash() {
    if (!stash_pending)
        return;
    for (const Arm& a : arms)
        if (!a.have_upper_len || !a.have_fore_len)
            return;  // not measured yet
    stash_pending = false;
    float worst = 0.0f;
    for (int side = 0; side < 2; side++) {
        const float du = fabsf(arms[side].upper_len - stash[side].upper_len);
        const float df = fabsf(arms[side].fore_len - stash[side].fore_len);
        worst = du > worst ? du : worst;
        worst = df > worst ? df : worst;
    }
    if (!SameCharacter(model_key, stash_key) || worst > REST_LENGTH_EPS) {
        diag::Log("ikarms: rest pose dropped (%s; bone lengths differ by up to %.3f)",
                  SameCharacter(model_key, stash_key) ? "same character" : "another character",
                  worst);
        return;
    }
    for (int side = 0; side < 2; side++) {
        Arm& a = arms[side];
        const RestPose& s = stash[side];
        a.idle_shoulder = s.idle_shoulder;
        a.have_idle_shoulder = s.have_idle_shoulder;
        a.idle_wrist_from_grip = s.idle_wrist_from_grip;
        a.have_idle_wrist = s.have_idle_wrist;
        memcpy(a.idle_hand, s.idle_hand, sizeof(a.idle_hand));
        a.idle_hand_mask = s.idle_hand_mask;
    }
    diag::Log("ikarms: rest pose kept (same character, bone lengths within %.3f)", worst);
}

// Two-bone solve in character space. S = shoulder, P = wrist target, a =
// upper arm length (the elbow lies on the upper arm's x in every body), f =
// the wrist in the forearm's own frame, pole = the side the elbow points
// to. Builds the upper arm and forearm frames: x along the bone, z the
// elbow hinge shared by both, the forearm bending toward the upper arm's
// -y, and f landing on P. The wrist's offset along the hinge (f.z) puts it
// off the plane the arm bends in, so the hinge leans toward the reach by
// asin(f.z / reach); its offset across the forearm (f.y) turns the forearm
// within that plane by atan2(f.y, f.x). `swivel` turns the elbow's side
// about the reach (radians, right-handed about shoulder-to-wrist).
inline bool Solve(Arm& arm, Vec3 S, Vec3 P, float a, Vec3 f, Vec3 pole, float swivel,
                  D3DMATRIX& upper, D3DMATRIX& fore) {
    Vec3 u = P - S;
    const float d = Length(u);
    const float b = sqrtf(f.x * f.x + f.y * f.y);  // the wrist's reach in the plane
    if (d < 1e-4f || a < 1e-3f || b < 1e-3f)
        return false;
    u = u * (1.0f / d);
    // The pole side, perpendicular to the reach; with the target lined up
    // with the pole, keep the last one.
    Vec3 v = pole - u * Dot(pole, u);
    const float lv = Length(v);
    if (lv > 0.05f) {
        v = v * (1.0f / lv);
        arm.prev_pole_side = v;
        arm.have_prev_pole_side = true;
    } else if (arm.have_prev_pole_side) {
        v = arm.prev_pole_side - u * Dot(arm.prev_pole_side, u);
        const float lp = Length(v);
        if (lp < 1e-3f)
            return false;
        v = v * (1.0f / lp);
    } else {
        return false;
    }
    if (swivel != 0.0f)
        v = v * cosf(swivel) + Cross(u, v) * sinf(swivel);
    // The hinge leaned toward the reach, and the in-plane reach w that is
    // left once the wrist's offset along it is taken off (w = u at f.z 0).
    // A target nearer the shoulder than that offset is out of reach: the
    // lean is capped and the arm folds as far as it goes.
    const Vec3 n = Cross(u, v);
    float s_lean = f.z / d;
    if (s_lean > 0.99f) s_lean = 0.99f;
    if (s_lean < -0.99f) s_lean = -0.99f;
    const float c_lean = sqrtf(1.0f - s_lean * s_lean);
    const Vec3 w = u * c_lean - n * s_lean;
    float dc = d * c_lean;
    const float d_min = fabsf(a - b) + 1e-3f, d_max = a + b - 1e-3f;
    if (dc < d_min) dc = d_min;
    if (dc > d_max) dc = d_max;
    float cos_a = (a * a + dc * dc - b * b) / (2.0f * a * dc);
    if (cos_a > 1.0f) cos_a = 1.0f;
    if (cos_a < -1.0f) cos_a = -1.0f;
    const float sin_a = sqrtf(1.0f - cos_a * cos_a);
    const Vec3 x1 = w * cos_a + v * sin_a;  // shoulder to elbow
    const Vec3 y1 = v * cos_a - w * sin_a;  // the elbow's outer side
    const Vec3 z1 = Cross(x1, y1);          // the hinge
    const Vec3 E = S + x1 * a;
    Vec3 l = (S + w * dc) - E;  // elbow to the wrist's place in the plane
    const float ll = Length(l);
    if (ll < 1e-4f)
        return false;
    l = l * (1.0f / ll);
    // The forearm's x, turned back from that line by the wrist's angle off it.
    const float cos_g = f.x / b, sin_g = f.y / b;
    const Vec3 x2 = l * cos_g - Cross(z1, l) * sin_g;
    const Vec3 y2 = Cross(z1, x2);
    upper = Frame(x1, y1, z1, S);
    fore = Frame(x2, y2, z1, E);
    arm.last_reach = d;
    return true;
}

// The farthest the wrist reaches from the shoulder, the arm straight, with
// the upper arm and forearm lengthened by s along their bones.
inline float MaxReach(const Arm& arm, float s) {
    const float fx = arm.fore_off.x * s, fy = arm.fore_off.y, fz = arm.fore_off.z;
    const float in_plane = arm.upper_len * s + sqrtf(fx * fx + fy * fy);
    return sqrtf(in_plane * in_plane + fz * fz);
}

// The hand's roll about the forearm (radians, right-handed about the
// forearm's x). In the rest pose the wrist's x axis lies along the forearm's
// hinge (z) on both arms, at any elbow bend; the wrist target's x, projected
// across the forearm, gives the roll from that rest relation. `prev` unwraps
// it, so a roll past half a turn saturates instead of flipping. False with
// the hand bent along the forearm (no reading).
inline bool HandRoll(const D3DMATRIX& fore, const D3DMATRIX& wrist, float& prev,
                     bool& have_prev, float& roll) {
    const Vec3 x2 = Row(fore, 0), y2 = Row(fore, 1), z2 = Row(fore, 2);
    const Vec3 wx = Row(wrist, 0);
    const Vec3 p = wx - x2 * Dot(wx, x2);
    if (Length(p) < 0.2f) {
        have_prev = false;
        return false;
    }
    roll = atan2f(-Dot(p, y2), Dot(p, z2));  // z2 turned by roll = p
    if (have_prev) {
        float step = roll - prev;
        while (step > 3.14159265f) step -= 6.2831853f;
        while (step < -3.14159265f) step += 6.2831853f;
        roll = prev + step;
        if (roll > 3.14159265f) roll = 3.14159265f;
        if (roll < -3.14159265f) roll = -3.14159265f;
    }
    prev = roll;
    have_prev = true;
    return true;
}

// ik_elbow: how far the elbow turns about the shoulder-to-wrist line, from
// the hand's roll against the forearm of the fixed-pole solve (`fore`). A
// wrist only twists so far; past ik_elbow_dead_deg either way of
// ik_elbow_center_deg (mirrored for the left arm) the elbow follows by
// ik_elbow_gain of the rest, up to ik_elbow_max_deg, which takes that much
// roll off the forearm and wrist. Reading against the fixed-pole forearm,
// not the turned one, keeps the turn from feeding back into itself. Eased at
// ik_elbow_rate_dps; with no reading the turn holds.
inline float ElbowSwivel(Arm& arm, int side, const D3DMATRIX& fore, const D3DMATRIX& wrist) {
    const float rad = 0.0174532925f;
    float target = arm.have_swivel ? arm.swivel : 0.0f;
    float roll;
    if (HandRoll(fore, wrist, arm.prev_base_roll, arm.have_prev_base_roll, roll)) {
        arm.last_base_roll_deg = roll * 57.2958f;
        const float center = vrmod::config.ik_elbow_center_deg * rad;
        const float r = roll - (side == 1 ? center : -center);
        float over = fabsf(r) - vrmod::config.ik_elbow_dead_deg * rad;
        if (over < 0.0f)
            over = 0.0f;
        target = ClampAngle((r < 0.0f ? -over : over) * vrmod::config.ik_elbow_gain,
                            vrmod::config.ik_elbow_max_deg * rad);
    }
    const double now = vrmod::NowSeconds();
    float swivel = target;
    if (arm.have_swivel) {
        double dt = now - arm.swivel_time;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.1) dt = 0.1;
        swivel = EaseAngle(arm.swivel, target,
                           vrmod::config.ik_elbow_rate_dps * rad * (float)dt);
    }
    arm.swivel = swivel;
    arm.swivel_time = now;
    arm.have_swivel = true;
    arm.last_swivel_deg = swivel * 57.2958f;
    return swivel;
}

// Forearm twist: the forearm is rolled by `fraction` of the hand's roll
// about the line from the elbow to the wrist (f, the wrist in the forearm's
// frame), so the wrist stays where the solve put it.
inline void TwistForearm(Arm& arm, D3DMATRIX& fore, const D3DMATRIX& wrist,
                         float fraction, Vec3 f) {
    const Vec3 x2 = Row(fore, 0), y2 = Row(fore, 1), z2 = Row(fore, 2);
    float roll;
    if (!HandRoll(fore, wrist, arm.prev_roll, arm.have_prev_roll, roll))
        return;
    arm.last_roll_deg = roll * 57.2958f;
    if (fraction == 0.0f)
        return;
    Vec3 k = x2 * f.x + y2 * f.y + z2 * f.z;
    const float lk = Length(k);
    if (lk < 1e-4f)
        return;
    k = k * (1.0f / lk);
    // Each axis turned about k (Rodrigues); with k = x this is y c + z s,
    // z c - y s.
    const float c = cosf(roll * fraction), s = sinf(roll * fraction);
    for (int r = 0; r < 3; r++) {
        const Vec3 a = Row(fore, r);
        SetRow(fore, r, a * c + Cross(k, a) * s + k * (Dot(k, a) * (1.0f - c)));
    }
}

// A bone frame lengthened by `s` along its own x (the bone's direction), so
// the bone's skin stretches with it.
inline D3DMATRIX ScaleAlongBone(D3DMATRIX m, float s) {
    m._11 *= s;
    m._12 *= s;
    m._13 *= s;
    return m;
}

// A bone's offset from the parent in the parent's own frame, so a
// lengthened parent does not change the reading; its length is the bone's.
inline bool BoneOffset(const D3DMATRIX& child, const D3DMATRIX& parent, Vec3& out) {
    D3DMATRIX parent_inv;
    if (!AffineInverse(parent, parent_inv))
        return false;
    out = Row(vrmod::Multiply(child, parent_inv), 3);
    return true;
}
inline bool BoneLength(const D3DMATRIX& child, const D3DMATRIX& parent, float& out) {
    Vec3 off;
    if (!BoneOffset(child, parent, off))
        return false;
    out = Length(off);
    return true;
}

// The length factor that fits this arm to the player's reach (ik_reach_cm:
// shoulder to the middle of the fist, arm straight). The hand keeps its
// size, so the arm makes up the reach less the wrist-to-grip distance (rel
// = the wrist in the grip point's frame). ik_arm_match blends from the
// character's own length (0) to the fitted one (1).
inline float LengthFactor(const Arm& arm, const D3DMATRIX& rel) {
    const float match = vrmod::config.ik_arm_match;
    const float arm_len = arm.upper_len + arm.fore_len;
    if (match == 0.0f || arm_len < 1e-3f)
        return 1.0f;
    const float reach = vrmod::config.ik_reach_cm * 0.01f * vrmod::config.world_scale *
                        walk_char_per_world;
    float fit = (reach - Length(Row(rel, 3))) / arm_len;
    if (fit < LENGTH_FACTOR_MIN) fit = LENGTH_FACTOR_MIN;
    if (fit > LENGTH_FACTOR_MAX) fit = LENGTH_FACTOR_MAX;
    return 1.0f + match * (fit - 1.0f);
}

// A child of a lengthened arm bone: its place moves with the length, its
// axes are rebuilt from the parent without the length scale, so it (and
// anything drawn on it, such as a shield) is not skewed. `written` is the
// parent as set, `rigid` the same frame unscaled.
inline void Unstretch(D3DMATRIX* top, const D3DMATRIX& written, const D3DMATRIX& rigid) {
    D3DMATRIX written_inv;
    if (!AffineInverse(written, written_inv))
        return;
    const D3DMATRIX c = vrmod::Multiply(*top, walk_root_inv);
    D3DMATRIX fixed = vrmod::Multiply(vrmod::Multiply(c, written_inv), rigid);
    fixed._41 = c._41;
    fixed._42 = c._42;
    fixed._43 = c._43;
    *top = vrmod::Multiply(fixed, walk_root);
}

// The fixed test pose: the wrist in front of the chest, in its rest
// orientation (character space: +z forward, +y up, the model's right -x).
inline D3DMATRIX TestWrist(int side, Vec3 shoulder) {
    D3DMATRIX m = vrmod::Identity();
    const float inward = side == 1 ? 1.0f : -1.0f;  // toward the middle
    SetRow(m, 3, shoulder + Vec3{inward * 1.0f, 1.0f, 3.5f});
    return m;
}

inline Vec3 PoleFor(int side) {
    const float out = side == 1 ? -vrmod::config.ik_pole_out : vrmod::config.ik_pole_out;
    const Vec3 pole = {out, -vrmod::config.ik_pole_down, -vrmod::config.ik_pole_back};
    return walk_twist != 0.0f ? TurnDir(pole, walk_twist_sin, walk_twist_cos) : pole;
}

inline void TraceArm(int side, const char* what, Vec3 S, Vec3 P) {
    (void)side; (void)what; (void)S; (void)P;
}

// What our own shifts (ik_raise_cm, ik_shoulder_*) added to this arm's
// shoulder, character space.
inline Vec3 ShoulderShift(int side) {
    return {side == 0 ? -walk_shoulder.x : walk_shoulder.x,
            walk_shoulder.y + walk_raise, walk_shoulder.z};
}

// ik_anchor: records the idle pose - this shoulder, and the hand bones
// below in the same walk (HoldHand, OnGrip) - once the game says idle in the
// relaxed stance, the feet are still and the shoulder has held still for
// IDLE_HOLD_S (attacks hold still poses between combo steps, and the battle
// stance's idle is still too, so stillness alone is not the rest pose).
inline void TrackIdle(Arm& arm, Vec3 anim_shoulder) {
    const double now = vrmod::NowSeconds();
    const bool still = walk_idle_state && arm.have_prev_anim_shoulder &&
                       Length(anim_shoulder - arm.prev_anim_shoulder) <= IDLE_STEP;
    arm.prev_anim_shoulder = anim_shoulder;
    arm.have_prev_anim_shoulder = true;
    if (!still) {
        arm.still_since = 0.0;
        return;
    }
    if (arm.still_since == 0.0)
        arm.still_since = now;
    if (now - arm.still_since < IDLE_HOLD_S)
        return;
    // ik_idle_hold: a pose once recorded whole is held on idle walks too, so
    // neither the idle animation's loop nor a newly equipped weapon's stance
    // reaches the arm. 1 records again, once, when the idle clip is another
    // than the record's; 2 never does; 0 records on every idle walk.
    const bool recorded = arm.have_idle_shoulder && arm.have_idle_wrist && arm.idle_hand_mask != 0;
    if (recorded && (vrmod::config.ik_idle_hold == 2 ||
                     (vrmod::config.ik_idle_hold == 1 && arm.idle_clip == walk_clip)))
        return;
    arm.idle_shoulder = anim_shoulder;
    arm.have_idle_shoulder = true;
    arm.recording_idle = true;
}

// Drops the recorded idle pose: the arms follow the animation until the next
// relaxed standstill records it again.
inline void ForgetIdlePose() {
    for (Arm& a : arms) {
        a.have_idle_shoulder = false;
        a.have_idle_wrist = false;
        a.idle_hand_mask = 0;
        a.idle_clip = -1;
        a.still_since = 0.0;
    }
}

// ik_anchor: a bone of the wrist's subtree. On an idle walk its frame
// relative to the posed wrist is recorded (the pose is left as animated);
// otherwise it is set to the recorded one.
inline void HoldHand(int side, int i, D3DMATRIX* top) {
    Arm& arm = arms[side];
    if (vrmod::config.ik_anchor == 0 || !arm.posed)
        return;
    const int k = i - ARM_BONES[side].hand_first;
    if (arm.recording_idle) {
        D3DMATRIX wrist_inv;
        if (AffineInverse(arm.wrist_used, wrist_inv)) {
            const D3DMATRIX rel = vrmod::Multiply(vrmod::Multiply(*top, walk_root_inv), wrist_inv);
            if (arm.idle_hand_mask & (1u << k)) {
                const float step = Length(Row(rel, 3) - Row(arm.idle_hand[k], 3));
                if (step > arm.walk_record_step)
                    arm.walk_record_step = step;
            }
            arm.idle_hand[k] = rel;
            arm.idle_hand_mask |= 1u << k;
        }
        return;
    }
    if (arm.idle_hand_mask & (1u << k)) {
        *top = vrmod::Multiply(vrmod::Multiply(arm.idle_hand[k], arm.wrist_used), walk_root);
    }
}

inline void OnUpper(int side, D3DMATRIX* top) {
    Arm& arm = arms[side];
    arm.posed = false;
    const D3DMATRIX c = vrmod::Multiply(*top, walk_root_inv);
    arm.upper_used = c;
    if (!arm.have_upper_len || !arm.have_fore_len)
        return;
    Vec3 S = Row(c, 3);
    const bool anchor = vrmod::config.ik_anchor != 0;
    arm.last_anchor_gap = 0.0f;
    if (anchor) {
        const Vec3 shift = ShoulderShift(side);
        // The idle record lives in the untwisted frame: the animated
        // shoulder is turned back out of the twist for it, and the held
        // point turned into it again.
        const bool twisted = walk_twist != 0.0f && have_chest_pivot;
        const Vec3 untwisted =
            twisted ? TurnPoint(S, walk_chest_pivot, -walk_twist_sin, walk_twist_cos) : S;
        TrackIdle(arm, untwisted - shift);
        Vec3 held = arm.have_idle_shoulder ? arm.idle_shoulder + shift : untwisted;
        if (twisted)
            held = TurnPoint(held, walk_chest_pivot, walk_twist_sin, walk_twist_cos);
        if (vrmod::config.ik_anchor == 2)
            held = held + walk_head_shift;
        arm.last_anchor_gap = Length(held - S);
        S = held;
    }
    D3DMATRIX wrist;
    const D3DMATRIX* rel = nullptr;  // the wrist in the grip point's frame
    if (vrmod::config.ik_arms == 2) {
        wrist = TestWrist(side, S);
        if (arm.have_wrist_from_grip)
            rel = &arm.wrist_from_grip;
    } else {
        if (!arm.have_target || !arm.have_wrist_from_grip)
            return;
        D3DMATRIX grip = vrmod::Multiply(arm.target_world, walk_char_from_world);
        if (!Orthonormalize(grip))
            return;
        rel = anchor && arm.have_idle_wrist ? &arm.idle_wrist_from_grip
                                            : &arm.wrist_from_grip;
        wrist = vrmod::Multiply(*rel, grip);
    }
    // The arm's length: fitted to the player's reach, then stretched up to
    // ik_stretch_max toward a target still past it.
    const Vec3 P = Row(wrist, 3);
    const float fit = rel != nullptr ? LengthFactor(arm, *rel) : 1.0f;
    const float full = MaxReach(arm, fit);
    const float reach = Length(P - S);
    float stretch = 1.0f;
    if (full > 1e-3f && reach > full) {
        // The wrist's offset across the forearm does not lengthen with it,
        // so the reach is not proportional to the stretch: two steps get
        // within a thousandth.
        stretch = reach / full;
        for (int step = 0; step < 2; step++) {
            const float r = MaxReach(arm, fit * stretch);
            if (r > 1e-3f)
                stretch *= reach / r;
        }
        const float cap = 1.0f + vrmod::config.ik_stretch_max;
        if (stretch > cap)
            stretch = cap;
    }
    const float scale = fit * stretch;
    const Vec3 f = {arm.fore_off.x * scale, arm.fore_off.y, arm.fore_off.z};
    D3DMATRIX upper, fore;
    const Vec3 pole = PoleFor(side);
    if (!Solve(arm, S, P, arm.upper_len * scale, f, pole, 0.0f, upper, fore))
        return;
    if (vrmod::config.ik_elbow != 0) {
        const float swivel = ElbowSwivel(arm, side, fore, wrist);
        D3DMATRIX upper_turned, fore_turned;
        if (swivel != 0.0f && Solve(arm, S, P, arm.upper_len * scale, f, pole, swivel,
                                    upper_turned, fore_turned)) {
            upper = upper_turned;
            fore = fore_turned;
        }
    } else {
        arm.have_swivel = false;
        arm.have_prev_base_roll = false;
        arm.last_swivel_deg = 0.0f;
    }
    TwistForearm(arm, fore, wrist, vrmod::config.ik_twist, f);
    arm.upper_out = upper;
    arm.fore_out = fore;
    arm.wrist_out = wrist;
    arm.length_scale = scale;
    arm.last_fit = fit;
    arm.last_stretch = stretch;
    arm.posed = true;
    arm.upper_used = ScaleAlongBone(upper, scale);
    *top = vrmod::Multiply(arm.upper_used, walk_root);
    TraceArm(side, "posed", S, P);
}

inline void OnFore(int side, D3DMATRIX* top) {
    Arm& arm = arms[side];
    const D3DMATRIX c = vrmod::Multiply(*top, walk_root_inv);
    if (BoneLength(c, arm.upper_used, arm.upper_len))
        arm.have_upper_len = true;
    if (arm.posed) {
        arm.fore_used = ScaleAlongBone(arm.fore_out, arm.length_scale);
        *top = vrmod::Multiply(arm.fore_used, walk_root);
    } else {
        arm.fore_used = c;
    }
}

inline void OnWrist(int side, D3DMATRIX* top) {
    Arm& arm = arms[side];
    const D3DMATRIX c = vrmod::Multiply(*top, walk_root_inv);
    if (BoneOffset(c, arm.fore_used, arm.fore_off)) {
        arm.fore_len = Length(arm.fore_off);
        arm.have_fore_len = true;
    }
    if (arm.posed) {
        arm.wrist_used = arm.wrist_out;
        *top = vrmod::Multiply(arm.wrist_out, walk_root);
    } else {
        arm.wrist_used = c;
    }
}

inline void OnGrip(int side, int i, D3DMATRIX* top) {
    Arm& arm = arms[side];
    const D3DMATRIX c = vrmod::Multiply(*top, walk_root_inv);
    D3DMATRIX grip_inv;
    if (AffineInverse(c, grip_inv)) {
        D3DMATRIX rel = vrmod::Multiply(arm.wrist_used, grip_inv);
        if (Orthonormalize(rel)) {
            arm.wrist_from_grip = rel;
            arm.have_wrist_from_grip = true;
            // Recorded with the hand bones, so the next walks' wrist target
            // puts the held grip point on the controller.
            if (arm.recording_idle && arm.posed) {
                arm.idle_wrist_from_grip = rel;
                arm.have_idle_wrist = true;
                arm.idle_clip = walk_clip;
            }
        }
    }
    HoldHand(side, i, top);
    if (arm.posed && vrmod::config.ik_arms == 1) {
        D3DMATRIX grip = vrmod::Multiply(arm.target_world, walk_char_from_world);
        arm.last_grip_error =
            Length(Row(vrmod::Multiply(*top, walk_root_inv), 3) - Row(grip, 3));
    }
}

// The skinless children of the upper arm and forearm, under a lengthened
// arm.
inline void OnAttach(int side, int i, D3DMATRIX* top) {
    const Arm& arm = arms[side];
    if (!arm.posed || arm.length_scale == 1.0f)
        return;
    if (i == ARM_BONES[side].upper_attach)
        Unstretch(top, arm.upper_used, arm.upper_out);
    else
        Unstretch(top, arm.fore_used, arm.fore_out);
}

// Moves a bone in the character's frame.
inline void Shift(D3DMATRIX* top, Vec3 d) {
    D3DMATRIX c = vrmod::Multiply(*top, walk_root_inv);
    c._41 += d.x;
    c._42 += d.y;
    c._43 += d.z;
    *top = vrmod::Multiply(c, walk_root);
}

inline void OnBone() {
    walk_callbacks++;
    const uintptr_t cursor = *reinterpret_cast<const uintptr_t*>(BONE_CURSOR_ADDR);
    if (cursor < walk_bones || ((cursor - walk_bones) & 63) != 0)
        return;
    const uintptr_t index = (cursor - walk_bones) / 64;
    if (index >= BONE_COUNT)
        return;
    // ik_raise_cm: the chest and everything on it up, the neck and head
    // chains back down, so the head and the eye height measured from it stay.
    // The torso twist turns the chest, and so everything on it, about the
    // vertical through its origin; the neck and head chains turn back out of
    // it first, so the head stays where the camera is.
    if ((walk_raise != 0.0f || walk_twist != 0.0f) &&
        (index == CHEST_BONE || index == NECK_FRONT_BONE || index == HEAD_CHAIN_BONE)) {
        D3DMATRIX* top = *reinterpret_cast<D3DMATRIX* const*>(MATRIX_STACK_TOP_ADDR);
        if (top == nullptr ||
            !diag::Accessible(reinterpret_cast<uintptr_t>(top), sizeof(D3DMATRIX), true))
            return;
        if (index != CHEST_BONE && walk_twist != 0.0f && have_chest_pivot)
            *top = vrmod::Multiply(
                TurnFrame(vrmod::Multiply(*top, walk_root_inv), walk_chest_pivot,
                          -walk_twist_sin, walk_twist_cos),
                walk_root);
        if (walk_raise != 0.0f)
            Shift(top, {0.0f, index == CHEST_BONE ? walk_raise : -walk_raise, 0.0f});
        if (index == CHEST_BONE && walk_twist != 0.0f) {
            const D3DMATRIX c = vrmod::Multiply(*top, walk_root_inv);
            walk_chest_pivot = Row(c, 3);
            have_chest_pivot = true;
            *top = vrmod::Multiply(
                TurnFrame(c, walk_chest_pivot, walk_twist_sin, walk_twist_cos), walk_root);
        }
        return;
    }
    // ik_shoulder_*_cm: the shoulder joints have no skin, so only the arms move.
    if (index == SHOULDER_BONES[0] || index == SHOULDER_BONES[1]) {
        if (walk_shoulder.x == 0.0f && walk_shoulder.y == 0.0f && walk_shoulder.z == 0.0f)
            return;
        D3DMATRIX* top = *reinterpret_cast<D3DMATRIX* const*>(MATRIX_STACK_TOP_ADDR);
        if (top != nullptr &&
            diag::Accessible(reinterpret_cast<uintptr_t>(top), sizeof(D3DMATRIX), true)) {
            // The model's right is -x, so inward is +x on the right, -x on the left.
            Vec3 d = walk_shoulder;
            if (index == SHOULDER_BONES[0])
                d.x = -d.x;
            if (walk_twist != 0.0f)
                d = TurnDir(d, walk_twist_sin, walk_twist_cos);
            Shift(top, d);
        }
        return;
    }
    for (int side = 0; side < 2; side++) {
        const ArmBones& b = ARM_BONES[side];
        const int i = static_cast<int>(index);
        const bool hand = i >= b.hand_first && i < b.hand_first + HAND_BONE_COUNT;
        const bool attach =
            i == b.upper_attach || i == b.fore_attach[0] || i == b.fore_attach[1];
        if (i != b.upper && i != b.fore && i != b.wrist && !hand && !attach)
            continue;
        D3DMATRIX* top = *reinterpret_cast<D3DMATRIX* const*>(MATRIX_STACK_TOP_ADDR);
        if (top == nullptr ||
            !diag::Accessible(reinterpret_cast<uintptr_t>(top), sizeof(D3DMATRIX), true))
            return;
        if (i == b.upper) OnUpper(side, top);
        else if (i == b.fore) OnFore(side, top);
        else if (i == b.wrist) OnWrist(side, top);
        else if (i == b.grip) OnGrip(side, i, top);
        else if (hand) HoldHand(side, i, top);
        else OnAttach(side, i, top);
        return;
    }
}

inline void __cdecl HookBoneCallback(void* node) {
    if (walk_active)
        OnBone();
    original_bone_callback(node);
}

// Sets up a walk of the local player's body; false leaves it to the game.
inline bool BeginWalk(uintptr_t entity) {
    if (!diag::Accessible(entity + ENTITY_BONES_OFFSET, 4, false) ||
        !diag::Accessible(entity + ENTITY_TREE_OFFSET, 4, false) ||
        !diag::Accessible(MATRIX_STACK_TOP_ADDR, 4, false) ||
        !diag::Accessible(INVERSE_VIEW_ADDR, 64, false))
        return false;
    const uintptr_t bones = *reinterpret_cast<const uintptr_t*>(entity + ENTITY_BONES_OFFSET);
    const uintptr_t tree = *reinterpret_cast<const uintptr_t*>(entity + ENTITY_TREE_OFFSET);
    const D3DMATRIX* top = *reinterpret_cast<D3DMATRIX* const*>(MATRIX_STACK_TOP_ADDR);
    if (bones == 0 || tree == 0 || top == nullptr ||
        !diag::Accessible(reinterpret_cast<uintptr_t>(top), sizeof(D3DMATRIX), false))
        return false;
    if (entity != model_entity || tree != model_tree) {
        StashRestPose();
        model_entity = entity;
        model_tree = tree;
        ResetModel();
        model_key = ReadCharacterKey(entity);
    }
    walk_root = *top;
    if (!AffineInverse(walk_root, walk_root_inv))
        return false;
    const D3DMATRIX root_world = vrmod::Multiply(
        walk_root, *reinterpret_cast<const D3DMATRIX*>(INVERSE_VIEW_ADDR));
    if (!AffineInverse(root_world, walk_char_from_world))
        return false;
    walk_char_per_world = Length(Row(walk_char_from_world, 0));
    // The torso twist: the estimated torso's direction in the character's
    // frame, clamped and eased so a jump of the estimate does not pop the
    // body. Off, or without an estimate, it eases back to none.
    have_chest_pivot = false;
    {
        float target = 0.0f;
        walk_twist_raw = 0.0f;
        if (vrmod::config.ik_torso != 0 && torso.valid) {
            D3DMATRIX world_from_tracking;
            float angle;
            if (gamecam::WorldFromTracking(vrmod::Identity(), world_from_tracking) &&
                TorsoAngleInCharacter(torso.tx, torso.tz, world_from_tracking,
                                      walk_char_from_world, angle)) {
                // Past about 150 degrees either way the angle's sign flips as
                // the torso crosses behind the body; the twist keeps its side
                // until the torso comes back round, so the body does not
                // swing across.
                if (have_twist_prev && twist_prev != 0.0f && fabsf(angle) > 2.618f &&
                    (angle > 0.0f) != (twist_prev > 0.0f))
                    angle += twist_prev > 0.0f ? 6.2831853f : -6.2831853f;
                walk_twist_raw = angle;
                target = ClampAngle(angle, vrmod::config.ik_torso_max_deg * 0.0174532925f);
            }
        }
        const double now = vrmod::NowSeconds();
        float twist = target;
        if (have_twist_prev) {
            double dt = now - twist_prev_time;
            if (dt < 0.0) dt = 0.0;
            if (dt > 0.1) dt = 0.1;
            twist = EaseAngle(twist_prev, target,
                              vrmod::config.ik_torso_rate_dps * 0.0174532925f * (float)dt);
        }
        twist_prev = twist;
        twist_prev_time = now;
        have_twist_prev = true;
        walk_twist = twist;
        walk_twist_sin = sinf(twist);
        walk_twist_cos = cosf(twist);
    }
    walk_bones = bones;
    walk_callbacks = 0;
    const bool have_feet = diag::Accessible(entity + ENTITY_FEET_OFFSET, 12, false);
    const float* feet = reinterpret_cast<const float*>(entity + ENTITY_FEET_OFFSET);
    {
        bool idle = false;
        if (diag::Accessible(entity + ENTITY_ACTION_MODE_OFFSET, 2, false) &&
            diag::Accessible(entity + ENTITY_STATE_FLAGS_OFFSET, 4, false) &&
            diag::Accessible(entity + ENTITY_CLIP_OFFSET, 2, false)) {
            const short mode = *reinterpret_cast<const short*>(entity + ENTITY_ACTION_MODE_OFFSET);
            const short clip = *reinterpret_cast<const short*>(entity + ENTITY_CLIP_OFFSET);
            walk_battle_stance =
                (*reinterpret_cast<const uint32_t*>(entity + ENTITY_STATE_FLAGS_OFFSET) &
                 STATE_BATTLE_STANCE) != 0;
            if (!have_prev_clip || clip != prev_clip || mode != prev_mode)
                walk_relaxed_clip = !walk_battle_stance;
            else if (walk_battle_stance)
                walk_relaxed_clip = false;
            prev_clip = clip;
            prev_mode = mode;
            have_prev_clip = true;
            walk_clip = clip;
            idle = mode == ACTION_MODE_IDLE && walk_relaxed_clip;
        } else {
            walk_clip = -1;
        }
        if (have_feet) {
            const float dx = feet[0] - prev_feet[0], dy = feet[1] - prev_feet[1],
                        dz = feet[2] - prev_feet[2];
            if (!have_prev_feet ||
                dx * dx + dy * dy + dz * dz > FEET_STILL_STEP * FEET_STILL_STEP)
                idle = false;
            memcpy(prev_feet, feet, sizeof(prev_feet));
            have_prev_feet = true;
        } else {
            idle = false;
        }
        walk_idle_state = idle;
    }
    // ik_anchor 2: the neck point's offset from where it sits with the head
    // at its recentered place (the camera's eye point over these feet,
    // upright, facing the body's way). Moving the shoulders by it keeps
    // them under the view through the run lean, the player's own lean, or
    // the camera and body placed a step apart while moving.
    walk_head_shift = {0.0f, 0.0f, 0.0f};
    if (vrmod::config.ik_anchor == 2 && have_neck && have_feet) {
        // The neck belongs to the torso, so it is the torso's facing.
        Vec3 facing = walk_twist != 0.0f
                          ? Row(root_world, 0) * walk_twist_sin + Row(root_world, 2) * walk_twist_cos
                          : Row(root_world, 2);
        facing.y = 0.0f;
        const float lf = Length(facing);
        if (lf > 1e-4f) {
            const float k = 0.01f * vrmod::config.world_scale;
            const Vec3 neutral =
                Vec3{feet[0],
                     feet[1] + gamecam::eye_units_at_apply - vrmod::config.ik_neck_down_cm * k,
                     feet[2]} -
                facing * (vrmod::config.ik_neck_back_cm * k / lf);
            walk_head_shift = TransformPoint(neck_world, walk_char_from_world) -
                              TransformPoint(neutral, walk_char_from_world);
        }
    }
    walk_raise = vrmod::config.ik_raise_cm * 0.01f * vrmod::config.world_scale;
    {
        const float k = 0.01f * vrmod::config.world_scale;
        walk_shoulder = {vrmod::config.ik_shoulder_in_cm * k,
                         vrmod::config.ik_shoulder_up_cm * k,
                         vrmod::config.ik_shoulder_fwd_cm * k};
    }
    for (Arm& a : arms) {
        a.posed = false;
        a.recording_idle = false;
        a.walk_record_step = 0.0f;
        a.walk_hold_gap = 0.0f;
    }
    return true;
}

inline void EndWalk() {
    walks_this_pass++;
    callbacks_last_walk = walk_callbacks;
    CheckRestStash();
    const bool any = arms[0].posed || arms[1].posed;
    if (any)
        posed_this_pass++;
    if (any && !logged_first_pose) {
        logged_first_pose = true;
        diag::Log("ikarms: posing the arms (mode %d, %d bones walked; left upper %.2f "
                  "fore %.2f posed %d, right upper %.2f fore %.2f posed %d)",
                  vrmod::config.ik_arms, walk_callbacks, arms[0].upper_len,
                  arms[0].fore_len, arms[0].posed ? 1 : 0, arms[1].upper_len,
                  arms[1].fore_len, arms[1].posed ? 1 : 0);
    }
}

inline void WalkBracket(WalkFn original, void* self, void* edx) {
    const uintptr_t entity = reinterpret_cast<uintptr_t>(self);
    const bool mine = !walk_active && entity != 0 && Active() &&
                      entity == gamecam::ResolveEntity() && BeginWalk(entity);
    if (mine)
        walk_active = true;
    original(self, edx);
    if (mine) {
        walk_active = false;
        EndWalk();
    }
}

inline void __fastcall HookWalk(void* self, void* edx) {
    WalkBracket(original_walk, self, edx);
}

inline void __fastcall HookWalkLod(void* self, void* edx) {
    WalkBracket(original_walk_lod, self, edx);
}


// The torso estimate, stepped once per frame from the tracked head and the
// hand positions (tracking space) in `in`.
inline void StepTorso(TorsoInput in) {
    if (vrmod::config.ik_torso == 0) {
        torso = TorsoState();
        return;
    }
    if (torso_recenters != gamecam::recenter_count) {
        torso_recenters = gamecam::recenter_count;
        torso = TorsoState();
    }
    const double now = vrmod::NowSeconds();
    double dt = now - torso_step_time;
    torso_step_time = now;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;
    const D3DMATRIX& h = gamecam::head_at_apply;
    in.head = {h._41, h._42, h._43};
    in.head_fx = -h._31;
    in.head_fz = -h._33;
    const float units = vrmod::config.world_scale;
    TorsoParams p;
    p.neck_back = vrmod::config.ik_torso_neck_back_m * units;
    p.hands_near = vrmod::config.ik_torso_hands_near_m * units;
    p.hands_far = vrmod::config.ik_torso_hands_far_m * units;
    p.smooth_s = vrmod::config.ik_torso_smooth_s;
    p.hands = vrmod::config.ik_torso_hands;
    p.neck_range = vrmod::config.ik_torso_neck_deg * 0.0174532925f;
    TorsoStep(torso, in, p, (float)dt);
}


// Once per BeginScene pass, after weapongrip::OnFrame (the grip tuple and
// this frame's takeover pose).
inline void OnFrame() {
    walks_last_pass = walks_this_pass;
    posed_last_pass = posed_this_pass;
    walks_this_pass = 0;
    posed_this_pass = 0;
    for (Arm& a : arms)
        a.have_target = false;
    have_neck = false;
    // For the weapon's placement next pass (weapongrip::melee_on_line).
    weapongrip::ik_poses_hands = Active();
    // The torso estimate is kept while inactive (loading, menus): the
    // player's own torso has not moved, and re-seeding from the head would
    // start it wrong for a player looking aside.
    if (!Active())
        return;
    {
        // The neck point: the tracked head (the eyes' forward push undone,
        // the run lean kept) moved down and back in the head's own frame.
        // Rows: 1 = up, 2 = back.
        D3DMATRIX head;
        gamecam::GetWorldFromHead(head);
        const float s = vrmod::config.world_scale;
        const Vec3 center = Row(head, 3) + Row(head, 2) * (vrmod::config.eye_forward_m * s);
        neck_world = center - Row(head, 1) * (vrmod::config.ik_neck_down_cm * 0.01f * s) +
                     Row(head, 2) * (vrmod::config.ik_neck_back_cm * 0.01f * s);
        have_neck = true;
    }
    TorsoInput torso_in = {};
    torso_in.have_hands = true;
    for (int side = 0; side < 2; side++) {
        D3DMATRIX pose, hand_world;
        if (vrmod::Get()->GetHandPose(side, pose)) {
            torso_in.hand[side] = {pose._41, pose._42, pose._43};
            if (gamecam::WorldFromTracking(pose, hand_world)) {
                // The hand's place for what it holds or does not hold: on a
                // gun's seat, on the melee weapon kept on the controller's
                // line, or as an empty hand (weapongrip::HandOffset).
                arms[side].target_world = vrmod::Multiply(weapongrip::HandOffset(side), hand_world);
                arms[side].have_target = true;
            }
        } else {
            torso_in.have_hands = false;
        }
    }
    StepTorso(torso_in);
}

// Called from every BeginScene; hooks nothing until ik_arms is on.
inline void Install() {
    static bool tried = false;
    if (tried || vrmod::config.ik_arms == 0)
        return;
    tried = true;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        probe::Log("ikarms: MH_Initialize failed (%d)", (int)status);
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(PLAYER_DRAW_WALK),
                      reinterpret_cast<void*>(&HookWalk),
                      reinterpret_cast<void**>(&original_walk)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(PLAYER_DRAW_WALK_LOD),
                      reinterpret_cast<void*>(&HookWalkLod),
                      reinterpret_cast<void**>(&original_walk_lod)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(BONE_CALLBACK),
                      reinterpret_cast<void*>(&HookBoneCallback),
                      reinterpret_cast<void**>(&original_bone_callback)) != MH_OK) {
        probe::Log("ikarms: MH_CreateHook failed");
        diag::Log("ikarms: MH_CreateHook failed - arms keep the game's pose");
        return;
    }
    if (MH_EnableHook(reinterpret_cast<void*>(PLAYER_DRAW_WALK)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(PLAYER_DRAW_WALK_LOD)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(BONE_CALLBACK)) != MH_OK) {
        probe::Log("ikarms: MH_EnableHook failed");
        diag::Log("ikarms: MH_EnableHook failed - arms keep the game's pose");
        return;
    }
    installed = true;
    probe::Log("ikarms: player draw walks (0x%08X / 0x%08X) + bone callback 0x%08X hooked",
               (unsigned)PLAYER_DRAW_WALK, (unsigned)PLAYER_DRAW_WALK_LOD,
               (unsigned)BONE_CALLBACK);
}

}  // namespace ikarms
