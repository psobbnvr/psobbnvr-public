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
//    toward the upper arm's -Y. The forearm also rolls by ik_twist of the
//    hand's roll.
//  - wrist 44 / 31: placed so the grip point (48 / 35) lands on the
//    controller's grip frame, the one the weapon is seated on
//    (weapongrip::ActiveGripOffset).
//
// The solve runs in character space (stack top x inverse(walk root), the
// root being the stack top at the walk's start: feet, facing and body
// proportions), where bones are rigid. Stack matrices are camera-relative,
// so world targets enter as world x inverse(root x [0xACD420]). Bone
// lengths and the wrist-to-grip relation are measured from the walks.
//
// [vr] ik_arms: 1 on, 2 a fixed test pose. ik_anchor holds the shoulders
// and hand bones at the idle pose (TrackIdle, HoldHand); its mode 2 also
// moves the shoulders with the headset (BeginWalk). ik_raise_cm and
// ik_shoulder_*_cm sit the arms higher under the raised camera.
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
constexpr int HAND_BONE_COUNT = 4;
struct ArmBones {
    int upper, fore, wrist, grip, hand_first;
};
constexpr ArmBones ARM_BONES[2] = {{28, 29, 31, 35, 32}, {41, 42, 44, 48, 45}};
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
    D3DMATRIX wrist_from_grip = {};
    bool have_wrist_from_grip = false;
    // This frame's target: the grip point's world frame.
    D3DMATRIX target_world = {};
    bool have_target = false;
    // This walk: the solved frames and what each bone ended up as.
    bool posed = false;
    D3DMATRIX upper_out = {}, fore_out = {}, wrist_out = {};
    D3DMATRIX upper_used = {}, fore_used = {}, wrist_used = {};
    // The last good pole side, for a target lined up with the pole.
    Vec3 prev_pole_side = {0.0f, 0.0f, 0.0f};
    bool have_prev_pole_side = false;
    // The hand's roll about the forearm last walk (radians, unwrapped), so
    // a roll past half a turn saturates instead of flipping.
    float prev_roll = 0.0f;
    bool have_prev_roll = false;
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
    Vec3 prev_anim_shoulder = {0.0f, 0.0f, 0.0f};
    bool have_prev_anim_shoulder = false;
    double still_since = 0.0;  // 0 = moving
    // Diagnostics.
    float last_anchor_gap = 0.0f;  // held shoulder vs the animated one
    float last_reach = 0.0f;       // shoulder-to-wrist-target distance
    float last_grip_error = 0.0f;  // posed grip point vs the target
    float last_roll_deg = 0.0f;    // the hand's roll about the forearm
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
inline int walk_callbacks = 0;
inline float walk_raise = 0.0f;              // ik_raise_cm in model units
inline Vec3 walk_shoulder = {0.0f, 0.0f, 0.0f};  // the right shoulder's shift, model units
// The game says idle and the feet have not moved since the last walk.
inline bool walk_idle_state = false;
inline float prev_feet[3] = {};
inline bool have_prev_feet = false;
// ik_anchor 2: this frame's neck point (world), and the shoulders' move
// for this walk (character space).
inline Vec3 neck_world = {0.0f, 0.0f, 0.0f};
inline bool have_neck = false;
inline Vec3 walk_head_shift = {0.0f, 0.0f, 0.0f};

// Counters (per pass, plus the last pass's for the status line).
inline int walks_this_pass = 0, walks_last_pass = 0;
inline int posed_this_pass = 0, posed_last_pass = 0;
inline int callbacks_last_walk = 0;
inline bool logged_first_pose = false;
inline int trace_walks = 0;  // developer build: log this many walks

inline bool Active() {
    return installed && vrmod::config.ik_arms != 0 && gamecam::DrivesView();
}

inline void ResetModel() {
    for (Arm& a : arms) {
        a.have_upper_len = a.have_fore_len = false;
        a.have_wrist_from_grip = false;
        a.have_prev_pole_side = false;
        a.have_prev_roll = false;
        a.have_idle_shoulder = false;
        a.have_idle_wrist = false;
        a.idle_hand_mask = 0;
        a.have_prev_anim_shoulder = false;
        a.still_since = 0.0;
    }
    logged_first_pose = false;
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

// Two-bone solve in character space. S = shoulder, P = wrist target, a/b =
// upper arm / forearm length, pole = the side the elbow points to. Builds
// the upper arm and forearm frames: x along the bone, z the elbow hinge,
// the forearm bending toward the upper arm's -y.
inline bool Solve(Arm& arm, Vec3 S, Vec3 P, float a, float b, Vec3 pole,
                  D3DMATRIX& upper, D3DMATRIX& fore) {
    Vec3 u = P - S;
    const float d = Length(u);
    if (d < 1e-4f || a < 1e-3f || b < 1e-3f)
        return false;
    u = u * (1.0f / d);
    float dc = d;
    const float d_min = fabsf(a - b) + 1e-3f, d_max = a + b - 1e-3f;
    if (dc < d_min) dc = d_min;
    if (dc > d_max) dc = d_max;
    float cos_a = (a * a + dc * dc - b * b) / (2.0f * a * dc);
    if (cos_a > 1.0f) cos_a = 1.0f;
    if (cos_a < -1.0f) cos_a = -1.0f;
    const float sin_a = sqrtf(1.0f - cos_a * cos_a);
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
    const Vec3 x1 = u * cos_a + v * sin_a;  // shoulder to elbow
    const Vec3 y1 = v * cos_a - u * sin_a;  // the elbow's outer side
    const Vec3 z1 = Cross(x1, y1);          // the hinge
    const Vec3 E = S + x1 * a;
    Vec3 x2 = (S + u * dc) - E;  // elbow to the reached wrist point
    const float l2 = Length(x2);
    if (l2 < 1e-4f)
        return false;
    x2 = x2 * (1.0f / l2);
    const Vec3 y2 = Cross(z1, x2);
    upper = Frame(x1, y1, z1, S);
    fore = Frame(x2, y2, z1, E);
    arm.last_reach = d;
    return true;
}

// Forearm twist. In the rest pose the wrist's x axis lies along the
// forearm's hinge (z) on both arms, at any elbow bend. The wrist target's
// x, projected across the forearm, gives the hand's roll from that rest
// relation; the forearm is rolled about its own axis (x) by `fraction` of
// it.
inline void TwistForearm(Arm& arm, D3DMATRIX& fore, const D3DMATRIX& wrist,
                         float fraction) {
    const Vec3 x2 = Row(fore, 0), y2 = Row(fore, 1), z2 = Row(fore, 2);
    const Vec3 wx = Row(wrist, 0);
    const Vec3 p = wx - x2 * Dot(wx, x2);
    if (Length(p) < 0.2f) {
        arm.have_prev_roll = false;  // the hand bent along the forearm: no reading
        return;
    }
    float roll = atan2f(-Dot(p, y2), Dot(p, z2));  // z2 turned by roll = p
    if (arm.have_prev_roll) {
        float step = roll - arm.prev_roll;
        while (step > 3.14159265f) step -= 6.2831853f;
        while (step < -3.14159265f) step += 6.2831853f;
        roll = arm.prev_roll + step;
        if (roll > 3.14159265f) roll = 3.14159265f;
        if (roll < -3.14159265f) roll = -3.14159265f;
    }
    arm.prev_roll = roll;
    arm.have_prev_roll = true;
    arm.last_roll_deg = roll * 57.2958f;
    if (fraction == 0.0f)
        return;
    const float c = cosf(roll * fraction), s = sinf(roll * fraction);
    SetRow(fore, 1, y2 * c + z2 * s);
    SetRow(fore, 2, z2 * c - y2 * s);
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
    return {out, -vrmod::config.ik_pole_down, -vrmod::config.ik_pole_back};
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
// below in the same walk (HoldHand, OnGrip) - once the game says idle, the
// feet are still and the shoulder has held still for IDLE_HOLD_S (attacks
// hold still poses between combo steps, so stillness alone is not idle).
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
    arm.idle_shoulder = anim_shoulder;
    arm.have_idle_shoulder = true;
    arm.recording_idle = true;
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
            arm.idle_hand[k] = vrmod::Multiply(vrmod::Multiply(*top, walk_root_inv), wrist_inv);
            arm.idle_hand_mask |= 1u << k;
        }
        return;
    }
    if (arm.idle_hand_mask & (1u << k))
        *top = vrmod::Multiply(vrmod::Multiply(arm.idle_hand[k], arm.wrist_used), walk_root);
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
        TrackIdle(arm, S - shift);
        Vec3 held = arm.have_idle_shoulder ? arm.idle_shoulder + shift : S;
        if (vrmod::config.ik_anchor == 2)
            held = held + walk_head_shift;
        arm.last_anchor_gap = Length(held - S);
        S = held;
    }
    D3DMATRIX wrist;
    if (vrmod::config.ik_arms == 2) {
        wrist = TestWrist(side, S);
    } else {
        if (!arm.have_target || !arm.have_wrist_from_grip)
            return;
        D3DMATRIX grip = vrmod::Multiply(arm.target_world, walk_char_from_world);
        if (!Orthonormalize(grip))
            return;
        const D3DMATRIX& rel = anchor && arm.have_idle_wrist ? arm.idle_wrist_from_grip
                                                             : arm.wrist_from_grip;
        wrist = vrmod::Multiply(rel, grip);
    }
    D3DMATRIX upper, fore;
    if (!Solve(arm, S, Row(wrist, 3), arm.upper_len, arm.fore_len, PoleFor(side),
               upper, fore))
        return;
    TwistForearm(arm, fore, wrist, vrmod::config.ik_twist);
    arm.upper_out = upper;
    arm.fore_out = fore;
    arm.wrist_out = wrist;
    arm.posed = true;
    arm.upper_used = upper;
    *top = vrmod::Multiply(upper, walk_root);
    TraceArm(side, "posed", S, Row(wrist, 3));
}

inline void OnFore(int side, D3DMATRIX* top) {
    Arm& arm = arms[side];
    const D3DMATRIX c = vrmod::Multiply(*top, walk_root_inv);
    arm.upper_len = Length(Row(c, 3) - Row(arm.upper_used, 3));
    arm.have_upper_len = true;
    if (arm.posed) {
        arm.fore_used = arm.fore_out;
        *top = vrmod::Multiply(arm.fore_out, walk_root);
    } else {
        arm.fore_used = c;
    }
}

inline void OnWrist(int side, D3DMATRIX* top) {
    Arm& arm = arms[side];
    const D3DMATRIX c = vrmod::Multiply(*top, walk_root_inv);
    arm.fore_len = Length(Row(c, 3) - Row(arm.fore_used, 3));
    arm.have_fore_len = true;
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
    if (walk_raise != 0.0f &&
        (index == CHEST_BONE || index == NECK_FRONT_BONE || index == HEAD_CHAIN_BONE)) {
        D3DMATRIX* top = *reinterpret_cast<D3DMATRIX* const*>(MATRIX_STACK_TOP_ADDR);
        if (top != nullptr &&
            diag::Accessible(reinterpret_cast<uintptr_t>(top), sizeof(D3DMATRIX), true))
            Shift(top, {0.0f, index == CHEST_BONE ? walk_raise : -walk_raise, 0.0f});
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
            Shift(top, d);
        }
        return;
    }
    for (int side = 0; side < 2; side++) {
        const ArmBones& b = ARM_BONES[side];
        const int i = static_cast<int>(index);
        const bool hand = i >= b.hand_first && i < b.hand_first + HAND_BONE_COUNT;
        if (i != b.upper && i != b.fore && i != b.wrist && !hand)
            continue;
        D3DMATRIX* top = *reinterpret_cast<D3DMATRIX* const*>(MATRIX_STACK_TOP_ADDR);
        if (top == nullptr ||
            !diag::Accessible(reinterpret_cast<uintptr_t>(top), sizeof(D3DMATRIX), true))
            return;
        if (i == b.upper) OnUpper(side, top);
        else if (i == b.fore) OnFore(side, top);
        else if (i == b.wrist) OnWrist(side, top);
        else if (i == b.grip) OnGrip(side, i, top);
        else HoldHand(side, i, top);
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
    walk_bones = bones;
    walk_callbacks = 0;
    const bool have_feet = diag::Accessible(entity + ENTITY_FEET_OFFSET, 12, false);
    const float* feet = reinterpret_cast<const float*>(entity + ENTITY_FEET_OFFSET);
    {
        bool idle = false;
        if (diag::Accessible(entity + ENTITY_ACTION_MODE_OFFSET, 2, false))
            idle = *reinterpret_cast<const short*>(entity + ENTITY_ACTION_MODE_OFFSET) ==
                   ACTION_MODE_IDLE;
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
        Vec3 facing = Row(root_world, 2);
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
    const D3DMATRIX offset = weapongrip::ActiveGripOffset();
    for (int side = 0; side < 2; side++) {
        D3DMATRIX pose, hand_world;
        if (vrmod::Get()->GetHandPose(side, pose) &&
            gamecam::WorldFromTracking(pose, hand_world)) {
            arms[side].target_world = vrmod::Multiply(offset, hand_world);
            arms[side].have_target = true;
        }
    }
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
