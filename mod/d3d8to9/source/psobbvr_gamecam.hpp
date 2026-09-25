#pragma once

// Game-camera takeover: writes the tracked head pose into the game's
// camera object every frame, so the game's CPU-side work (billboards, lens
// flares, particles, culling, audio panning) follows the real view.
//
// The camera object has two position/target pairs: +0x178/+0x184 builds
// the view matrix, and the game's update smooths it toward a goal pair at
// +0x1A0/+0x1AC set by the chase-cam logic, building the view before our
// BeginScene hook. So the takeover:
//   1. writes both pairs, so the smoothing converges on our pose;
//   2. lets d3d8to9_device.cpp substitute the view matrix at the game's
//      SetTransform(D3DTS_VIEW, &0x00ACBF80) with the inverse head pose;
//   3. with VR running, lets stereo::BeginEye build each eye's view from
//      the pose directly (the game's look-at, up = +y, cannot carry roll).
//
// Seated-space anchor: origin at the character's feet raised to eye level
// (measured head height + eye_offset_m with [vr] eye_height_auto, else
// eye_height_m); seated forward (-z) maps to the character's facing at
// capture (Recenter() re-captures). World scale is applied by the VR
// backend before poses arrive here.
//
// Every read is guarded: on a bad frame (transition, no entity, camera
// being rebuilt) nothing is written and the renderer appends the pose to
// the game's view instead.
//
// Called from BeginScene and from Present right after WaitPoses.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <windows.h>
#include <d3d9.h>

#include "psobbvr_eyeheight.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_vr.hpp"

namespace gamecam {

// Camera object pointer (constructed at 0x004D2BA2 / 0x004D345A).
constexpr uintptr_t CAMERA_PTR_ADDR = 0x00A48A54;
// The camera's vtable: checked before any write so a freed or
// half-constructed object is never touched.
constexpr uintptr_t CAMERA_VTABLE_ADDR = 0x00AFC590;
constexpr uintptr_t CAMERA_POS_OFFSET = 0x178;       // vec3 + vec3 target at +0x184
constexpr uintptr_t CAMERA_GOAL_POS_OFFSET = 0x1A0;  // vec3 + vec3 target at +0x1AC

// Vtable slot of the camera's per-frame update method (entry 1, original
// 0x004D386C, thiscall, no args). Its smoothing and goal recompute
// (camera-mode state machine under 0x4d3abc) overwrite our writes and the
// smoothing rates at +0x1B8 every frame, so the slot is patched to
// re-assert the pose right after the original returns.
constexpr uintptr_t CAMERA_VTABLE_UPDATE_SLOT = 0x00AFC594;
constexpr uintptr_t CAMERA_UPDATE_ORIGINAL = 0x004D386C;

// The update's third sub-call (0x004D3EF4) is not hooked: the renderer
// doesn't use the game's derived camera state, only audio and culling do.

// After a map change the takeover waits for this many consecutive good
// Apply() calls (twice per frame, ~0.4 s) so the game's spawn placement
// has landed (the floor id can flip a few frames before position/facing).
constexpr int SETTLE_APPLIES = 24;

// The game's view matrix slot (filled by 0x0082F130, then
// SetTransform(D3DTS_VIEW, &slot) at 0x0082F1A2); the pointer identifies
// the per-frame view set to substitute. The projection slot (set at
// 0x0082EE92) holds the scene projection - the game sets other
// projections in a frame too (the backdrop pass uses the window aspect,
// half the horizontal scale on our double-wide window).
constexpr uintptr_t GAME_VIEW_SLOT_ADDR = 0x00ACBF80;
constexpr uintptr_t GAME_PROJ_SLOT_ADDR = 0x00ACBF40;

// Player entity table + local player index; feet position vec3 at +0x38,
// facing at +0x60 as an int32 binary angle (65536 = 360 degrees, 0 =
// facing +z). Forward = (sin theta, 0, cos theta).
constexpr uintptr_t ENTITY_TABLE_ADDR = 0x00A94254;
constexpr uintptr_t ENTITY_INDEX_ADDR = 0x00A9C4F4;
constexpr uintptr_t ENTITY_POS_OFFSET = 0x38;
constexpr uintptr_t ENTITY_FACING_OFFSET = 0x60;
// Aim point vec3 (x, feet_y + 8.75, z): chest height, fixed for all
// builds; logged by the eye-height tracker.
constexpr uintptr_t ENTITY_AIM_OFFSET = 0x74;
constexpr uintptr_t ENTITY_FLOOR_OFFSET = 0x3F0;  // area/floor id

// The chase-cam keeps its look-at target ~50 units ahead; the same
// spacing is written.
constexpr float TARGET_DISTANCE = 50.0f;

// Pitch cap for the game's up=+y look-at only (the rendered view has no
// limit), so it never degenerates looking straight up or down.
constexpr float MIN_HORIZONTAL = 0.09f;  // ~ +/-85 degrees

// Seated forward's world direction (unit xz) and the head position in
// seated space at capture. Recenter() re-captures both, putting the
// wearer's current head position on the character's eye point.
inline bool anchor_valid = false;
inline bool want_recenter = true;
inline float anchor_dx = 0.0f, anchor_dz = -1.0f;
inline float anchor_p0[3] = {};
// The wearer's yaw at capture, tracking space, unit xz ((0,-1) = tracking
// forward). See the capture site.
inline float anchor_hx = 0.0f, anchor_hz = -1.0f;

// Yaw follow: the seated forward, smoothed toward the
// character's facing, so turning the character turns the view. Vector
// smoothing avoids angle wraparound; an exact 180 snaps.
inline float follow_dx = 0.0f, follow_dz = -1.0f;
constexpr float FOLLOW_ALPHA = 0.2f;  // per Apply (~2x/frame): ~0.2 s settle

// HUD lock ([vr] hud_lock): the level forward direction the HUD faces,
// sent to the backend each Apply (VRInterface::SetHudAnchor).
// Mode 1: the character's facing minus the attack view hold, so attack
// turns don't move the HUD. Mode 2: the view's forward. Smoothed with
// FOLLOW_ALPHA; recenter re-seeds.
inline float hud_fx = 0.0f, hud_fz = -1.0f;
inline bool hud_reseed = true;

// Attack view hold ([vr] attack_view_hold): an attack turns the
// character toward its target without turning the view. Facing changes
// while the action mode is 5..7 (attacking) accumulate in hold_offset
// (BAMS) and the view follows facing - hold_offset. Afterwards
// movement.hpp turns the character back in capped steps, reported in
// hold_drain_pending so the hold absorbs only those and stick turns
// still move the view. Recenter zeroes it.
constexpr uintptr_t ENTITY_ACTION_MODE_OFFSET = 0x32E;  // short: 5..7 = attack
inline int32_t hold_offset = 0;
inline int32_t hold_prev_facing = 0;
inline bool hold_prev_valid = false;
inline bool hold_was_attacking = false;
inline int32_t hold_attack_start = 0;  // offset at the attack's start (log)
inline int32_t hold_drain_pending = 0;  // |BAMS| movement commanded this tick

// Head-directed walking ([vr] head_move = 2): while walking, movement.hpp
// turns the character toward the head's yaw without turning the view.
// That turn accumulates in follow_offset (BAMS, same sign as hold_offset)
// and never drains; the view follows facing - hold_offset - follow_offset.
// Each tick's step is handed over in follow_pending_step and moved into
// follow_offset when the game gets that facing (movement.hpp
// AbsorbFollowStep). Recenter zeroes it.
inline int32_t follow_offset = 0;
inline int32_t follow_pending_step = 0;    // signed BAMS commanded this tick
inline int32_t follow_pending_target = 0;  // the facing it lands on (16-bit)

// Run lean: smoothed feet movement drives a forward camera offset, as the
// run animation leans the head forward and would show the torso.
inline float prev_feet[3] = {};
inline bool prev_feet_valid = false;
inline float run_smooth = 0.0f;
// Per-Apply feet step at full run (about half the per-frame step); full
// lean at or above it.
constexpr float RUN_STEP_FULL = 0.25f;
constexpr float RUN_ALPHA = 0.08f;  // ~0.5 s ramp

// Head pose in world space this frame (seated pose x anchor), valid while
// written_this_frame. stereo::BeginEye builds the eye views from it and
// SetTransform substitutes its inverse for the game's view.
inline D3DMATRIX world_from_head = vrmod::Identity();
inline bool written_this_frame = false;
// The raw tracked head pose world_from_head was built from this frame.
// See WorldFromTracking.
inline D3DMATRIX head_at_apply = vrmod::Identity();

// Position + target last written by Apply(), re-written by the update
// hook after the game's camera logic runs (only while written_this_frame,
// so it goes quiet during map transitions).
inline float pending_fields[6] = {};

// What the anchor was captured against. The camera object survives area
// changes, so those are detected by the entity pointer and floor id.
// settle_counter counts the spawn warm-up (SETTLE_APPLIES).
inline uintptr_t anchor_camera_obj = 0;
inline uintptr_t anchor_entity = 0;
inline uint32_t anchor_floor = 0xFFFFFFFF;
inline int settle_counter = 0;

inline bool Enabled() {
    switch (vrmod::config.game_camera) {
    case 1: return vrmod::Get()->Ready();
    case 2: return true;
    default: return false;
    }
}

// cam[0..2] = position, cam[3..5] = target. Null unless the object is
// readable and still carries the camera vtable.
inline float* ResolveCameraPair(uintptr_t offset) {
    if (!diag::Accessible(CAMERA_PTR_ADDR, 4, false))
        return nullptr;
    const uintptr_t obj = *reinterpret_cast<const uintptr_t*>(CAMERA_PTR_ADDR);
    if (obj == 0 || !diag::Accessible(obj, 4, false))
        return nullptr;
    if (*reinterpret_cast<const uintptr_t*>(obj) != CAMERA_VTABLE_ADDR)
        return nullptr;
    if (!diag::Accessible(obj + offset, 24, true))
        return nullptr;
    return reinterpret_cast<float*>(obj + offset);
}

inline uintptr_t ResolveEntity() {
    if (!diag::Accessible(ENTITY_INDEX_ADDR, 4, false))
        return 0;
    const uint32_t index = *reinterpret_cast<const uint32_t*>(ENTITY_INDEX_ADDR);
    if (index >= 12)  // lobby holds at most 12 players
        return 0;
    if (!diag::Accessible(ENTITY_TABLE_ADDR + 4 * index, 4, false))
        return 0;
    const uintptr_t entity = *reinterpret_cast<const uintptr_t*>(ENTITY_TABLE_ADDR + 4 * index);
    if (entity == 0 || !diag::Accessible(entity + ENTITY_POS_OFFSET, 0x30, false))
        return 0;
    return entity;
}

// Writes the stashed pose into both pairs, only if this frame's Apply()
// succeeded and the object being updated is the live, active camera.
inline void ReassertFields(void* self) {
    if (!written_this_frame || !Enabled())
        return;
    if (!diag::Accessible(CAMERA_PTR_ADDR, 4, false) ||
        *reinterpret_cast<void**>(CAMERA_PTR_ADDR) != self)
        return;
    if (float* cam = ResolveCameraPair(CAMERA_POS_OFFSET))
        memcpy(cam, pending_fields, 24);
    if (float* goal = ResolveCameraPair(CAMERA_GOAL_POS_OFFSET))
        memcpy(goal, pending_fields, 24);
}

// __fastcall stands in for __thiscall: this arrives in ecx, edx is unused.
using ThiscallFn = void(__fastcall*)(void* self, void* edx);
inline ThiscallFn original_camera_update = nullptr;

// Re-assert after the update so later readers (culling, effects) see the
// head pose.
inline void __fastcall CameraUpdateHook(void* self, void* edx) {
    original_camera_update(self, edx);
    ReassertFields(self);
}

inline void InstallHooks() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;

    // Vtable hook on the update method (data patch, no code bytes).
    const ThiscallFn current =
        *reinterpret_cast<ThiscallFn*>(CAMERA_VTABLE_UPDATE_SLOT);
    if (reinterpret_cast<uintptr_t>(current) != CAMERA_UPDATE_ORIGINAL)
        probe::Log("gamecam: camera vtable update entry is %p, expected 0x%08X - hooking anyway",
                   current, (unsigned)CAMERA_UPDATE_ORIGINAL);
    DWORD old_protect;
    if (VirtualProtect(reinterpret_cast<void*>(CAMERA_VTABLE_UPDATE_SLOT), 4,
                       PAGE_READWRITE, &old_protect)) {
        original_camera_update = current;
        *reinterpret_cast<ThiscallFn*>(CAMERA_VTABLE_UPDATE_SLOT) = CameraUpdateHook;
        VirtualProtect(reinterpret_cast<void*>(CAMERA_VTABLE_UPDATE_SLOT), 4,
                       old_protect, &old_protect);
        probe::Log("gamecam: camera-update vtable hook installed (original %p)", current);
    } else {
        probe::Log("gamecam: VirtualProtect on the camera vtable FAILED (%lu)", GetLastError());
    }
}

inline void Recenter() {
    want_recenter = true;
}

// True while the renderer should build eye views from world_from_head
// (needs a tracked pose; flat debug mode 2 uses the game's matrices).
inline bool DrivesView() {
    return written_this_frame && vrmod::Get()->Ready();
}

inline void GetWorldFromHead(D3DMATRIX &out) {
    out = world_from_head;
}

// Takes a tracked pose (row-vector, tracking space, game units) into game
// world space relative to the final head placement (world_from_head,
// comfort offsets included), so the eye-to-hand relationship stays exact
// under eye_forward_m and run lean. Used for the controller hands; valid
// only while the takeover resolved this frame.
inline bool WorldFromTracking(const D3DMATRIX &tracked_pose, D3DMATRIX &out) {
    if (!written_this_frame)
        return false;
    out = vrmod::Multiply(
        vrmod::Multiply(tracked_pose, vrmod::RigidInverse(head_at_apply)),
        world_from_head);
    return true;
}

// The substitute for the game's view matrix while the takeover runs: the
// inverse head pose (with roll). Used by SetTransform when the game sets
// its view from GAME_VIEW_SLOT_ADDR.
inline bool SubstituteGameView(const D3DMATRIX *source, D3DMATRIX &out) {
    if (!written_this_frame ||
        reinterpret_cast<uintptr_t>(source) != GAME_VIEW_SLOT_ADDR)
        return false;
    out = vrmod::RigidInverse(world_from_head);
    return true;
}

inline void Apply() {
    // Only goes true again if everything below resolves, so the update
    // hook and the renderer never act on stale state.
    written_this_frame = false;
    if (!Enabled()) {
        want_recenter = true;  // re-capture forward on the next activation
        settle_counter = 0;
        // A character-select relog can reuse the same entity/camera memory
        // on the same floor, so the area-change test below would miss it;
        // drop the old character's height samples here.
        eyeheight::OnAnchorReset();
        return;
    }
    InstallHooks();
    float* cam = ResolveCameraPair(CAMERA_POS_OFFSET);
    const uintptr_t entity = ResolveEntity();
    const float* feet = entity != 0
        ? reinterpret_cast<const float*>(entity + ENTITY_POS_OFFSET) : nullptr;
    if (cam == nullptr || feet == nullptr) {
        settle_counter = 0;  // transition in progress - restart the warm-up
        eyeheight::OnAnchorReset();  // same stale-ring rule as the menu gap
        return;
    }

    // A new camera object, entity or floor id means a fresh spawn:
    // re-anchor after the settle.
    const uintptr_t camera_obj = reinterpret_cast<uintptr_t>(cam) - CAMERA_POS_OFFSET;
    uint32_t floor_id = anchor_floor;
    if (diag::Accessible(entity + ENTITY_FLOOR_OFFSET, 4, false))
        floor_id = *reinterpret_cast<const uint32_t*>(entity + ENTITY_FLOOR_OFFSET);
    if (camera_obj != anchor_camera_obj || entity != anchor_entity ||
        floor_id != anchor_floor) {
        anchor_camera_obj = camera_obj;
        anchor_entity = entity;
        anchor_floor = floor_id;
        want_recenter = true;
        settle_counter = 0;
        eyeheight::OnAnchorReset();  // fresh spawn: don't mix areas/poses
    }
    // A runtime recenter moves the seated origin, invalidating anchor_p0:
    // re-anchor at once.
    if (vrmod::Get()->PollRecentered())
        want_recenter = true;
    // A live world-scale change leaves anchor_p0 in the old scale.
    if (vrmod::config.recenter_request) {
        vrmod::config.recenter_request = false;
        want_recenter = true;
    }
    if (settle_counter < SETTLE_APPLIES) {
        settle_counter++;
        // Settle done: the character has stood idle since the spawn, so
        // capture its standing head height.
        if (settle_counter == SETTLE_APPLIES)
            eyeheight::CaptureStanding("settle");
        return;
    }

    D3DMATRIX head = vrmod::Identity();
    const bool have_pose = vrmod::Get()->GetHeadPose(head);
    if (!have_pose && vrmod::config.game_camera != 2)
        return;  // VR running but no pose yet this session

    if (want_recenter || !anchor_valid) {
        // Seated forward anchors to the character's facing, not the chase
        // camera (spawn cameras can view the character side-on or from
        // the front).
        const int32_t facing_bams =
            *reinterpret_cast<const int32_t*>(entity + ENTITY_FACING_OFFSET);
        const float theta = facing_bams * (6.2831853f / 65536.0f);
        anchor_dx = sinf(theta);
        anchor_dz = cosf(theta);
        anchor_p0[0] = head._41;
        anchor_p0[1] = head._42;
        anchor_p0[2] = head._43;
        // The wearer's yaw at capture. Tracking zero is wherever the
        // headset faced at session start (OpenXR LOCAL space), so the
        // head's own forward is captured: recenter then means "the way I
        // face now is the character's forward". Head forward is -(row 3);
        // looking straight up or down keeps tracking forward.
        float hx = -head._31, hz = -head._33;
        const float hn = sqrtf(hx * hx + hz * hz);
        if (hn > 0.2f) {
            anchor_hx = hx / hn;
            anchor_hz = hz / hn;
        } else {
            anchor_hx = 0.0f;
            anchor_hz = -1.0f;
        }
        follow_dx = anchor_dx;
        follow_dz = anchor_dz;
        prev_feet_valid = false;
        run_smooth = 0.0f;
        hold_offset = 0;
        hold_prev_valid = false;
        hold_drain_pending = 0;
        follow_offset = 0;
        follow_pending_step = 0;
        hud_reseed = true;
        anchor_valid = true;
        want_recenter = false;
        diag::Log("gamecam: anchor facing=%d (theta %.1f deg) fwd=(%.3f,%.3f) headyaw=(%.3f,%.3f) feet=(%.1f,%.1f,%.1f)",
                  facing_bams, theta * 57.2958f,
                  anchor_dx, anchor_dz, anchor_hx, anchor_hz,
                  feet[0], feet[1], feet[2]);
    }

    // Current character facing (used by yaw-follow and the run lean).
    const int32_t live_bams =
        *reinterpret_cast<const int32_t*>(entity + ENTITY_FACING_OFFSET);
    const float live_theta = live_bams * (6.2831853f / 65536.0f);
    const float live_fx = sinf(live_theta);
    const float live_fz = cosf(live_theta);

    // Yaw follow: smooth the seated forward toward the live facing, less
    // the view offsets (hold_offset, follow_offset).
    float fwd_dx, fwd_dz;
    {
        // The direction the view chases.
        float target_fx = live_fx, target_fz = live_fz;
        if (vrmod::config.attack_view_hold) {
            bool attacking = false;
            if (diag::Accessible(entity + ENTITY_ACTION_MODE_OFFSET, 2, false)) {
                const short mode = *reinterpret_cast<const short*>(
                    entity + ENTITY_ACTION_MODE_OFFSET);
                attacking = mode >= 5 && mode <= 7;
            }
            const int32_t delta = hold_prev_valid
                ? static_cast<int16_t>(static_cast<uint32_t>(live_bams - hold_prev_facing))
                : 0;
            hold_prev_facing = live_bams;
            hold_prev_valid = true;
            if (attacking) {
                if (!hold_was_attacking)
                    hold_attack_start = hold_offset;
                hold_offset = static_cast<int16_t>(
                    static_cast<uint32_t>(hold_offset + delta));
            } else {
                if (hold_was_attacking && hold_offset != hold_attack_start)
                    diag::Log("viewhold: attack turned the character %+.1f deg, "
                              "view held (offset now %+.1f deg)",
                              static_cast<int16_t>(static_cast<uint32_t>(
                                  hold_offset - hold_attack_start)) *
                                  (360.0f / 65536.0f),
                              hold_offset * (360.0f / 65536.0f));
                if (delta != 0 && hold_offset != 0 && hold_drain_pending > 0 &&
                    (delta > 0) != (hold_offset > 0)) {
                    // movement.hpp's re-align step: drain the offset by it
                    // (the view holds); anything beyond is a real turn.
                    const int32_t mag = delta > 0 ? delta : -delta;
                    const int32_t omag = hold_offset > 0 ? hold_offset : -hold_offset;
                    int32_t a = mag < omag ? mag : omag;
                    if (a > hold_drain_pending)
                        a = hold_drain_pending;
                    hold_offset += delta > 0 ? a : -a;
                }
                hold_drain_pending = 0;
            }
            hold_was_attacking = attacking;
        }
        // The view's target: facing - hold_offset - follow_offset.
        const int32_t view_off =
            (vrmod::config.attack_view_hold ? hold_offset : 0) + follow_offset;
        if (view_off != 0) {
            const float t_theta =
                (live_bams - view_off) * (6.2831853f / 65536.0f);
            target_fx = sinf(t_theta);
            target_fz = cosf(t_theta);
        }
        follow_dx += (target_fx - follow_dx) * FOLLOW_ALPHA;
        follow_dz += (target_fz - follow_dz) * FOLLOW_ALPHA;
        const float norm = sqrtf(follow_dx * follow_dx + follow_dz * follow_dz);
        if (norm > 0.05f) {
            follow_dx /= norm;
            follow_dz /= norm;
        } else {  // degenerate mid-180: snap
            follow_dx = live_fx;
            follow_dz = live_fz;
        }
        fwd_dx = follow_dx;
        fwd_dz = follow_dz;
    }

    // Seated -> world: a yaw taking the wearer's captured forward
    // (anchor_hx/hz) to (fwd_dx, fwd_dz), placed so the captured head
    // position lands on the character's eye point. eff = fwd rotated back
    // by the captured head yaw (eff = fwd when the head faced tracking -z).
    const float eff_dx = -fwd_dx * anchor_hz + fwd_dz * anchor_hx;
    const float eff_dz = -fwd_dz * anchor_hz - fwd_dx * anchor_hx;
    D3DMATRIX anchor = vrmod::Identity();
    anchor._11 = -eff_dz; anchor._13 = eff_dx;
    anchor._31 = -eff_dx; anchor._33 = -eff_dz;
    const float eye_x = feet[0];
    // Eye level above the feet: the measured head height + eye_offset_m
    // with auto on, else the fixed eye_height_m (also the fallback before
    // the first capture).
    float eye_units = vrmod::config.eye_height_m * vrmod::config.world_scale;
    if (vrmod::config.eye_height_auto && eyeheight::Valid())
        eye_units = eyeheight::Units() +
                    vrmod::config.eye_offset_m * vrmod::config.world_scale;
    const float eye_y = feet[1] + eye_units;
    const float eye_z = feet[2];
    anchor._41 = eye_x - (anchor_p0[0] * anchor._11 + anchor_p0[2] * anchor._31);
    anchor._42 = eye_y - anchor_p0[1];
    anchor._43 = eye_z - (anchor_p0[0] * anchor._13 + anchor_p0[2] * anchor._33);

    // HUD lock: the view's forward; then world -> tracking by the
    // transposed anchor rotation.
    {
        float wx = fwd_dx, wz = fwd_dz;
        if (vrmod::config.hud_lock == 1 && vrmod::config.head_move == 2) {
            // head_move 2: facing less the attack hold, so the HUD comes
            // round with the walked head-follow turns (the view does not).
            const float h_theta =
                (live_bams - (vrmod::config.attack_view_hold ? hold_offset : 0)) *
                (6.2831853f / 65536.0f);
            wx = sinf(h_theta);
            wz = cosf(h_theta);
        }
        const float tx = wx * anchor._11 + wz * anchor._13;
        const float tz = wx * anchor._31 + wz * anchor._33;
        if (hud_reseed || vrmod::config.hud_lock != 1) {
            hud_fx = tx;
            hud_fz = tz;
            hud_reseed = false;
        } else {
            hud_fx += (tx - hud_fx) * FOLLOW_ALPHA;
            hud_fz += (tz - hud_fz) * FOLLOW_ALPHA;
            const float hn = sqrtf(hud_fx * hud_fx + hud_fz * hud_fz);
            if (hn > 0.05f) {
                hud_fx /= hn;
                hud_fz /= hn;
            } else {  // degenerate mid-180: snap
                hud_fx = tx;
                hud_fz = tz;
            }
        }
        vrmod::Get()->SetHudAnchor(vrmod::config.hud_lock != 0, hud_fx, hud_fz);
    }

    head_at_apply = head;  // for WorldFromTracking

    world_from_head = vrmod::Multiply(head, anchor);

    // The eyes sit forward of the tracked head center: push the pose along
    // the head's own forward (follows pitch).
    const float fwd_units = vrmod::config.eye_forward_m * vrmod::config.world_scale;
    world_from_head._41 -= world_from_head._31 * fwd_units;
    world_from_head._42 -= world_from_head._32 * fwd_units;
    world_from_head._43 -= world_from_head._33 * fwd_units;

    // Run lean: the run animation puts the head forward of and below the
    // standing eye point, so shift the camera along the facing and down by
    // run_down_m, ramped by smoothed feet speed. Warps are ignored.
    if (vrmod::config.run_forward_m != 0.0f ||
        vrmod::config.run_down_m != 0.0f) {
        float step = 0.0f;
        if (prev_feet_valid) {
            const float sdx = feet[0] - prev_feet[0];
            const float sdz = feet[2] - prev_feet[2];
            step = sqrtf(sdx * sdx + sdz * sdz);
            if (step > 5.0f)  // warp, not locomotion
                step = 0.0f;
        }
        prev_feet[0] = feet[0];
        prev_feet[1] = feet[1];
        prev_feet[2] = feet[2];
        prev_feet_valid = true;
        run_smooth += (step - run_smooth) * RUN_ALPHA;
        float lean = run_smooth / RUN_STEP_FULL;
        if (lean > 1.0f)
            lean = 1.0f;
        const float lean_units = lean * vrmod::config.run_forward_m * vrmod::config.world_scale;
        world_from_head._41 += live_fx * lean_units;
        world_from_head._43 += live_fz * lean_units;
        world_from_head._42 -=
            lean * vrmod::config.run_down_m * vrmod::config.world_scale;
    }

    // Head forward = -(z axis) in the row-vector pose. Clamp the pitch for
    // the game's look-at only; the rendered view keeps the full pose.
    float fx = -world_from_head._31;
    const float fy = -world_from_head._32;
    float fz = -world_from_head._33;
    const float horizontal = sqrtf(fx * fx + fz * fz);
    if (horizontal < MIN_HORIZONTAL) {
        if (horizontal > 1e-6f) {
            fx *= MIN_HORIZONTAL / horizontal;
            fz *= MIN_HORIZONTAL / horizontal;
        } else {
            fx = anchor_dx * MIN_HORIZONTAL;
            fz = anchor_dz * MIN_HORIZONTAL;
        }
    }
    const float scale = TARGET_DISTANCE / sqrtf(fx * fx + fy * fy + fz * fz);

    cam[0] = world_from_head._41;
    cam[1] = world_from_head._42;
    cam[2] = world_from_head._43;
    cam[3] = cam[0] + fx * scale;
    cam[4] = cam[1] + fy * scale;
    cam[5] = cam[2] + fz * scale;
    // Write the goal pair too, and stash the values for the update hook to
    // re-assert after the game's camera logic.
    if (float* goal = ResolveCameraPair(CAMERA_GOAL_POS_OFFSET))
        memcpy(goal, cam, 24);
    memcpy(pending_fields, cam, 24);
    written_this_frame = true;

    static bool logged = false;
    if (!logged) {
        logged = true;
        probe::Log("gamecam: takeover active (mode %d): camera (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f)",
                   vrmod::config.game_camera, cam[0], cam[1], cam[2], cam[3], cam[4], cam[5]);
    }
}

} // namespace gamecam
