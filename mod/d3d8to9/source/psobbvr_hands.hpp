// Motion-controlled hands: the character's own right-hand geometry,
// captured once from the draw stream and re-drawn each pass at the
// controller poses; the left hand is the capture mirrored. No forearms.
//
// Capture: the body is 142-199 user-pointer draws under one root matrix,
// vertices pre-posed in root-local space. The right-hand band's draws
// (per class, KNOWN_BANDS) are copied and moved into hand-local space:
//   hand_local = root_local x bodyRoot x RigidInverse(handBoneWorld)
// handBoneWorld = the weapon bracket root (weapongrip) or, unarmed, the
// entity's bone array. The bone must hold still a few passes. The body
// draws twice per pass; only the first run is taken. Stage-0 textures are
// AddRef'd to keep them alive across area changes.
//
// Replay ([vr] hand_presence): once per pass, per eye, z-tested, device
// state restored.
//   world = Scale(hand_scale) x GripOffset(active weapon tuple) x
//           hand trim x WorldFromTracking(GetHandPose(hand))
// The weapon's grip tuple fits the hand too, since the weapon's origin is
// its attach point on the same bone.
//
// The hand is an open shell cut at the wrist, so each draw goes out twice:
// back faces flat dark (the inside), then the textured front.
//
// Mirroring reflects the hand-local x axis (positions + normals), which
// flips the winding, so the left hand uses the opposite cull mode.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "psobbvr_partmap.hpp"
#include "psobbvr_weapongrip.hpp"

namespace hands {

constexpr int MAX_DRAWS = 24;    // biggest band seen: RAcast 15
constexpr int MAX_VERTS = 200;   // biggest band draw seen: 39 verts
constexpr int MAX_STRIDE = 48;   // body fvf 0x152 = 36 bytes/vert
// The bone must hold still (within EPS units) this many passes first.
constexpr float CAP_SETTLE_EPS = 0.05f;
constexpr int CAP_SETTLE_PASSES = 4;
constexpr int CAP_TIMEOUT_PASSES = 900;   // ~30 s of drawing, then give up
constexpr DWORD INTERIOR_COLOR = 0xFF262020;  // the glove's dark inside

struct CapturedDraw {
    D3DPRIMITIVETYPE type;
    UINT prims;
    UINT verts;
    UINT stride;
    DWORD fvf;
    IDirect3DBaseTexture9* texture;             // AddRef'd D3D9 binding
    uint8_t raw[MAX_VERTS * MAX_STRIDE];        // hand-local, right hand
    uint8_t mirrored[MAX_VERTS * MAX_STRIDE];   // hand-local, left hand
};
// Capture scratch: the band in flight lands here, then moves into `hand`.
inline CapturedDraw draws[MAX_DRAWS];
inline int draw_count = 0;

// One captured hand per session: the game never re-poses the hand mesh
// per weapon, so only its seat differs (DrawNow). Redone after a relog
// gap (cache_stale) or a manual 'handcap'.
struct HandSet {
    CapturedDraw draws[MAX_DRAWS];
    int draw_count;  // zero-initialized on purpose: a non-zero default here
};                   // moves the 460 KB buffer into the DLL's initialized
                     // data (+460 KB on disk)
inline HandSet hand;
inline bool HaveHand() { return hand.draw_count > 0; }

// Capture state machine (the automatic capture arms it; OnFrame runs it).
inline bool cap_armed = false;
inline int cap_first = 0, cap_last = 0;
inline int cap_settle = 0;
inline int cap_timeout = 0;
inline float cap_last_bone[3] = {};
inline bool cap_have_last_bone = false;
inline bool cap_this_pass = false;      // capturing during the current pass
inline bool cap_done_pending = false;   // band complete - finalize next frame
inline D3DMATRIX cap_bone = {};         // bone snapshot for normalization
inline D3DMATRIX cap_body_root = {};    // body root at the captured run
inline bool cap_have_root = false;
inline bool cap_auto = false;           // armed by the auto machinery (no timeout)

// Auto-capture state. The band is per model, not per weapon, learned once
// per session by a silent classifier run.
inline bool band_known = false;
inline int band_first = 0, band_last = 0;
inline bool band_running = false;       // classifier window in flight
inline int auto_backoff = 0;            // passes to wait after a failure
inline int auto_tries = 0;              // failed auto captures this session
constexpr int AUTO_MAX_TRIES = 3;
constexpr int AUTO_BACKOFF_PASSES = 150;   // ~5 s
constexpr int AUTO_BAND_PASSES = 40;       // classifier window (~1.5 s idle)
// Leaving gameplay this long (character select, relog) marks the capture
// stale: it stays drawn until a re-learned band's capture replaces it.
inline int gap_passes = 0;
inline bool cache_stale = false;
constexpr int STALE_GAP_PASSES = 150;
inline int auto_log_budget = 40;

// Unarmed hand frame: the entity's bone array (entity+0xE0, 64 bytes per
// bone), bone 48 x the global root matrix [0xA48980], equals the weapon
// bracket's root, so the hand bone is readable with nothing equipped.
// Published at the first draw of each body run (when the global root is
// the player's). With a weapon drawn the bracket root is the source and
// the array bone is checked against it once per weapon, adopting another
// index if a model's hand is not bone 48 (the index lives in weapongrip).
inline int& hand_bone_index = weapongrip::hand_bone_index;
inline void* hand_bone_checked_weapon = nullptr;   // confirmation latch, per weapon
inline D3DMATRIX array_bone = {};          // this pass (valid from the body run's first draw)
inline bool have_array_bone = false;
inline bool array_bone_published = false;   // the right-hand stash currently holds the array bone
inline D3DMATRIX array_bone_prev = {};     // last published, for OnFrame's settle test
inline bool have_array_bone_prev = false;
// The world draws on alternating passes, so the published bone stays
// valid for a short gap (as the bracket stash does); otherwise the settle
// count would reset every other pass.
inline int array_bone_age = 0;
constexpr int ARRAY_BONE_KEEPALIVE = 3;
inline bool bone_from_array_logged = false;

// Verified right-hand bands for all 12 classes, keyed on the class byte
// at entity+0x961 (the character's visual block is at entity+0x940:
// section id +0x960, class +0x961, class flags +0x964, proportion
// sliders +0x978/+0x97C, name +0x980). The classifier's run includes
// wrist and cuff chunks no distance threshold separates from the hand,
// so each row records the run the classifier sees and the band that is
// the hand. If a run ends elsewhere (other costume parts shift the draw
// numbering) the band shifts with it and the log says so. Unreadable
// class byte: match by run end, then the distance trim.
// Class order = the game's: HUmar 0, HUnewearl 1, HUcast 2, RAmar 3,
// RAcast 4, RAcaseal 5, FOmarl 6, FOnewm 7, FOnewearl 8, HUcaseal 9,
// FOmar 10, RAmarl 11.
struct KnownBand {
    int class_id;
    int run_first, run_last;   // the classifier run (run_first varies a little with body proportions)
    int first, last;           // the verified capture band
    const char* name;
};
constexpr uintptr_t ENTITY_CLASS_OFFSET = 0x961;   // u8 character class, see above
constexpr int CLASS_COUNT = 12;
constexpr KnownBand KNOWN_BANDS[] = {
    { 0, 46, 58, 47, 58, "HUmar"},      // tall runs d46..d58, short d44..d58 (forearm closer); d46 wrist
    { 1, 33, 43, 33, 43, "HUnewearl"},  // clean run
    { 2, 58, 69, 58, 69, "HUcast"},     // clean run
    { 3, 41, 53, 42, 53, "RAmar"},      // d41 wrist
    { 4, 60, 74, 61, 74, "RAcast"},     // d60 is a cuff
    { 5, 46, 54, 46, 53, "RAcaseal"},   // d54 not hand
    { 6, 55, 69, 55, 66, "FOmarl"},     // d67-69 cuff at the run's END (1.0-1.2 units)
    { 7, 32, 44, 34, 43, "FOnewm"},     // cuff BOTH ends (d33 robe texture); d34 wrist cap; d43 = 35-vert palm
    { 8, 50, 61, 50, 61, "FOnewearl"},  // clean run
    { 9, 38, 61, 47, 61, "HUcaseal"},   // forearm pieces d38-46 interleaved; d47 = thumb-base patch
    {10, 45, 55, 49, 55, "FOmar"},      // d45-48 forearm (1.4-2.1 units)
    {11, 37, 46, 37, 46, "RAmarl"},     // clean run
};
constexpr int RUN_FIRST_SLACK = 6;
// The player's class byte, or -1 when the entity cannot be read.
inline int ReadPlayerClass() {
    const uintptr_t entity = gamecam::ResolveEntity();
    if (entity == 0 || !diag::Accessible(entity + ENTITY_CLASS_OFFSET, 1, false))
        return -1;
    const int c = *reinterpret_cast<const uint8_t*>(entity + ENTITY_CLASS_OFFSET);
    return c < CLASS_COUNT ? c : -1;
}
inline const KnownBand* MatchKnownBandByClass(int class_id) {
    for (const KnownBand& k : KNOWN_BANDS)
        if (k.class_id == class_id)
            return &k;
    return nullptr;
}
// Fallback when the class byte is unreadable: match the run's last index,
// with slack on the first (which separates HUcast/FOmarl on 69 and
// FOnewearl/HUcaseal on 61).
inline const KnownBand* MatchKnownBandByRun(int run_first, int run_last) {
    for (const KnownBand& k : KNOWN_BANDS)
        if (k.run_last == run_last && run_first <= k.run_first + 1 &&
            run_first >= k.run_first - RUN_FIRST_SLACK)
            return &k;
    return nullptr;
}

// Replay per-pass state.
inline bool drawn_this_pass = false;
inline bool world_seen_this_pass = false;  // a full-viewport 3D draw ran
inline int log_budget = 0;
inline bool prev_enabled = false;

inline void ReleaseCapture() {   // the scratch
    for (int i = 0; i < draw_count; i++) {
        if (draws[i].texture != nullptr) {
            draws[i].texture->Release();
            draws[i].texture = nullptr;
        }
    }
    draw_count = 0;
}

inline void ReleaseHand() {
    for (int i = 0; i < hand.draw_count; i++) {
        if (hand.draws[i].texture != nullptr) {
            hand.draws[i].texture->Release();
            hand.draws[i].texture = nullptr;
        }
    }
    hand.draw_count = 0;
}

// Builds the left-hand vertex set: the hand-local x of positions (and
// normals) negated.
inline void BuildMirrored(HandSet& h) {
    constexpr int axis = 0;
    for (int d = 0; d < h.draw_count; d++) {
        CapturedDraw& c = h.draws[d];
        memcpy(c.mirrored, c.raw, c.verts * c.stride);
        const bool has_normal = (c.fvf & D3DFVF_NORMAL) != 0;
        uint8_t* p = c.mirrored;
        for (UINT i = 0; i < c.verts; i++, p += c.stride) {
            float* pos = reinterpret_cast<float*>(p);
            pos[axis] = -pos[axis];
            if (has_normal) {
                float* n = reinterpret_cast<float*>(p + 12);
                n[axis] = -n[axis];
            }
        }
    }
}

// Moves the captured band into hand-local space, validates it and makes
// it the hand.
inline void Finalize() {
    cap_done_pending = false;
    cap_armed = false;
    const bool was_auto = cap_auto;
    cap_auto = false;
    if (draw_count == 0 || !cap_have_root) {
        diag::Log("handcap: FAILED - band ran but nothing stored");
        ReleaseCapture();
        return;
    }
    // hand_local = root_local x bodyRoot x RigidInverse(bone); character
    // matrices are rotation + translation, so the rigid inverse is exact.
    const D3DMATRIX m =
        vrmod::Multiply(cap_body_root, vrmod::RigidInverse(cap_bone));
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    UINT total_verts = 0;
    for (int d = 0; d < draw_count; d++) {
        CapturedDraw& c = draws[d];
        const bool has_normal = (c.fvf & D3DFVF_NORMAL) != 0;
        uint8_t* p = c.raw;
        for (UINT i = 0; i < c.verts; i++, p += c.stride) {
            float* v = reinterpret_cast<float*>(p);
            const float x = v[0], y = v[1], z = v[2];
            v[0] = x * m._11 + y * m._21 + z * m._31 + m._41;
            v[1] = x * m._12 + y * m._22 + z * m._32 + m._42;
            v[2] = x * m._13 + y * m._23 + z * m._33 + m._43;
            for (int a = 0; a < 3; a++) {
                if (v[a] < mn[a]) mn[a] = v[a];
                if (v[a] > mx[a]) mx[a] = v[a];
            }
            if (has_normal) {
                float* n = reinterpret_cast<float*>(p + 12);
                const float nx = n[0], ny = n[1], nz = n[2];
                n[0] = nx * m._11 + ny * m._21 + nz * m._31;
                n[1] = nx * m._12 + ny * m._22 + nz * m._32;
                n[2] = nx * m._13 + ny * m._23 + nz * m._33;
            }
        }
        total_verts += c.verts;
    }
    // First vertex's diffuse (offset 12, or 24 with NORMAL), logged only:
    // it is the game's per-frame lighting, not a validity signal.
    DWORD diffuse0 = 0;
    if ((draws[0].fvf & D3DFVF_DIFFUSE) != 0)
        diffuse0 = *reinterpret_cast<const DWORD*>(
            draws[0].raw + ((draws[0].fvf & D3DFVF_NORMAL) != 0 ? 24 : 12));
    // Valid = the box is hand-sized (0.5..5 units per axis) and centred
    // within CAP_MAX_CENTRE of the origin; further out, the bone used was
    // not the hand's (e.g. a stale stash).
    bool sane = true;
    float centre_d2 = 0.0f;
    for (int a = 0; a < 3; a++) {
        const float span = mx[a] - mn[a];
        if (span < 0.5f || span > 5.0f)
            sane = false;
        const float c = (mn[a] + mx[a]) * 0.5f;
        centre_d2 += c * c;
    }
    constexpr float CAP_MAX_CENTRE = 4.0f;
    const bool off_frame = centre_d2 > CAP_MAX_CENTRE * CAP_MAX_CENTRE;
    if (off_frame)
        sane = false;
    diag::Log("handcap: %s %d draws, %u verts, hand-local box "
              "x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f] (units; hand ~2-3), tex0=%p diffuse0=%08X class 0x%02X%s",
              sane ? "CAPTURED" : (off_frame ? "REJECTED (box off the hand frame)" : "REJECTED"),
              draw_count, total_verts, mn[0], mx[0], mn[1],
              mx[1], mn[2], mx[2], draws[0].texture, (unsigned)diffuse0,
              weapongrip::current_weapon != nullptr ? weapongrip::current_category : 0,
              was_auto ? " (auto)" : "");
    if (!sane) {
        ReleaseCapture();
        if (was_auto) {
            auto_tries++;
            band_known = false;      // re-learn the band before the next try
            auto_backoff = AUTO_BACKOFF_PASSES;
        }
        return;
    }
    // Move the scratch draws (texture refs included) into the hand.
    ReleaseHand();
    for (int d = 0; d < draw_count; d++) {
        memcpy(&hand.draws[d], &draws[d], sizeof(CapturedDraw));
        draws[d].texture = nullptr;   // ownership moved
    }
    hand.draw_count = draw_count;
    draw_count = 0;
    BuildMirrored(hand);
    cache_stale = false;
    if (log_budget < 2)
        log_budget = 2;
}

// Bone reader + offsets shared with the weapon seat (weapongrip.hpp).
using weapongrip::ENTITY_BONES_OFFSET;
using weapongrip::GLOBAL_ROOT_MATRIX;
using weapongrip::ArrayBoneWorld;

// Called from the device UP path at every body-run draw, before the
// classifier's observe (which needs the bone at the run's first draw).
inline void PublishArrayBone() {
    if (!partmap::body_run_open || partmap::body_run_draws != 0)
        return;
    if (vrmod::have_hand_bone_right) {
        // A weapon is drawn: check the array bone against the bracket's raw
        // first matrix once per weapon. A match confirms the index; another
        // bone matching is adopted; a position-only match means the weapon's
        // first node has its own rotation (some parasols and partisans), so
        // the check waits for the next weapon.
        array_bone_published = false;
        if (weapongrip::have_bracket_root_raw &&
            hand_bone_checked_weapon != weapongrip::current_weapon &&
            weapongrip::seat_on_fist) {
            // The weapon attaches to another bone (a claw on the fist):
            // its first matrix says nothing about the hand bone index.
            hand_bone_checked_weapon = weapongrip::current_weapon;
            diag::Log("handbone: this weapon attaches to bone %d, not the hand bone - check deferred to the next weapon",
                      weapongrip::seat_attach_bone);
        } else if (weapongrip::have_bracket_root_raw &&
                   hand_bone_checked_weapon != weapongrip::current_weapon) {
            hand_bone_checked_weapon = weapongrip::current_weapon;
            const D3DMATRIX& raw = weapongrip::bracket_root_raw;
            D3DMATRIX a;
            float d = 1e9f, deg = 1e9f;
            if (ArrayBoneWorld(hand_bone_index, a))
                weapongrip::RigidDelta(raw, a, d, deg);
            if (d < 0.5f && deg < 2.0f) {
                diag::Log("handbone: bone %d confirmed against the weapon bracket (%.2f units, %.1f deg)",
                          hand_bone_index, d, deg);
            } else {
                int best_i = -1;
                float best_d = 1e9f, best_deg = 1e9f;
                for (int i = 0; i < 160; i++) {
                    D3DMATRIX b;
                    if (!ArrayBoneWorld(i, b))
                        break;
                    float bd = 0.0f, bdeg = 0.0f;
                    weapongrip::RigidDelta(raw, b, bd, bdeg);
                    if (bd < best_d) {
                        best_d = bd;
                        best_deg = bdeg;
                        best_i = i;
                    }
                }
                if (best_i >= 0 && best_i != hand_bone_index && best_d < 0.5f && best_deg < 2.0f) {
                    diag::Log("handbone: bone %d was %.2f units / %.1f deg off - this model's hand bone is %d (%.2f units), adopted",
                              hand_bone_index, d, deg, best_i, best_d);
                    hand_bone_index = best_i;
                } else if (d < 0.5f || (best_i >= 0 && best_d < 0.5f)) {
                    diag::Log("handbone: this weapon's first matrix is turned %.1f deg on bone %d (a model with its own node transform) - index kept, check deferred to the next weapon",
                              d < 0.5f ? deg : best_deg, d < 0.5f ? hand_bone_index : best_i);
                    hand_bone_checked_weapon = nullptr;   // no verdict from this weapon
                } else {
                    diag::Log("handbone: WARNING bone %d is %.2f units off the bracket and no bone matches (best %d at %.2f) - unarmed captures would be wrong on this model",
                              hand_bone_index, d, best_i, best_d);
                }
            }
        }
        return;
    }
    // No bracket this pass: the array bone stands in for the classifier
    // (partmap reads vrmod::hand_bone_world_right) and the capture.
    if (ArrayBoneWorld(hand_bone_index, array_bone)) {
        have_array_bone = true;
        array_bone_published = true;          // the stash is the array bone, not a bracket
        vrmod::hand_bone_world_right = array_bone;
        vrmod::have_hand_bone_right = true;   // weapongrip clears it again next BeginScene
        if (!bone_from_array_logged) {
            bone_from_array_logged = true;
            diag::Log("handbone: unarmed - hand frame from bone %d x global root, t=(%.2f,%.2f,%.2f)",
                      hand_bone_index, array_bone._41, array_bone._42, array_bone._43);
        }
    }
}


// One body-run user-pointer draw (device UP path, just before
// HideArmDrawNow, so it also sees draws the arm hide then drops).
inline void CaptureObserve(IDirect3DDevice9* dev, const void* data,
                           UINT stride, UINT verts, D3DPRIMITIVETYPE type,
                           UINT prims, DWORD fvf) {
    if (!cap_this_pass || !partmap::body_run_open || data == nullptr ||
        stride < 12 || stride > MAX_STRIDE || verts == 0)
        return;
    const int idx = partmap::body_run_draws + 1;
    if (idx < cap_first || idx > cap_last)
        return;
    if (idx == cap_first) {
        // current_world is the body root while the run is open.
        cap_body_root = partmap::current_world;
        cap_have_root = true;
        // No weapon drawn (unarmed, or holstered in town): use this run's
        // array bone, same pass as the root.
        if (have_array_bone && (weapongrip::current_weapon == nullptr || array_bone_published))
            cap_bone = array_bone;
    }
    if (draw_count >= MAX_DRAWS)
        return;
    CapturedDraw& c = draws[draw_count++];
    c.type = type;
    c.prims = prims;
    c.verts = verts > MAX_VERTS ? MAX_VERTS : verts;
    if (verts > MAX_VERTS)
        diag::Log("handcap: d%d clamped %u->%d verts", idx, verts, MAX_VERTS);
    c.stride = stride;
    c.fvf = fvf;
    c.texture = nullptr;
    dev->GetTexture(0, &c.texture);  // real D3D9 binding, AddRef'd
    memcpy(c.raw, data, c.verts * stride);
    if (idx == cap_last) {
        // Band complete; stopping here takes only the pass's first body
        // run.
        cap_this_pass = false;
        cap_done_pending = true;
    }
}

// Arm a capture of band [first, last].
inline bool ArmCapture(int first, int last, bool is_auto) {
    if (last < first || last - first >= MAX_DRAWS || first <= 0) {
        diag::Log("handcap: bad band d%d..d%d (max %d draws)", first, last, MAX_DRAWS);
        return false;
    }
    ReleaseCapture();
    cap_first = first;
    cap_last = last;
    cap_auto = is_auto;
    cap_armed = true;
    cap_settle = 0;
    cap_timeout = CAP_TIMEOUT_PASSES;
    cap_have_last_bone = false;
    cap_this_pass = false;
    cap_done_pending = false;
    cap_have_root = false;
    if (!is_auto || auto_log_budget > 0) {
        if (is_auto)
            auto_log_budget--;
        diag::Log("handcap: armed for band d%d..d%d%s - stand idle, weapon equipped",
                  first, last, is_auto ? " (auto)" : "");
    }
    return true;
}

// Auto capture, once per pass: learns the band with a silent classifier
// run once the body draws in gameplay, then captures the hand once.
// Bounded retries with a backoff; re-learned after a relog.
inline void AutoStep() {
    const bool playing = gamecam::DrivesView();
    if (!playing) {
        if (gap_passes < 100000)
            gap_passes++;
        if (gap_passes == STALE_GAP_PASSES && (HaveHand() || band_known)) {
            cache_stale = true;
            band_known = false;
            if (auto_log_budget > 0) {
                auto_log_budget--;
                diag::Log("handcap: cache marked stale (left gameplay) - the next capture re-learns the band");
            }
        }
        return;
    }
    gap_passes = 0;
    if (!vrmod::config.hand_presence)
        return;
    if (auto_backoff > 0) {
        auto_backoff--;
        return;
    }
    if (cap_armed || cap_done_pending)
        return;
    if (!vrmod::have_hand_bone_right && !have_array_bone_prev)
        return;   // the body has not drawn yet (loading, warp-in)

    // Band first (a classifier run needs a valid hand bone).
    if (band_running) {
        if (!partmap::handband_done)
            return;
        partmap::handband_done = false;
        partmap::handband_quiet = false;
        band_running = false;
        if (partmap::handband_best_n >= 4) {
            band_first = partmap::handband_best_first;
            band_last = partmap::handband_best_last;
            const int full_last = band_last;
            const int class_id = ReadPlayerClass();
            const KnownBand* k = class_id >= 0 ? MatchKnownBandByClass(class_id)
                                               : MatchKnownBandByRun(band_first, band_last);
            char known[128] = "";
            if (k != nullptr) {
                // A run ending elsewhere means the draw numbering shifted;
                // shift the verified band by the same amount.
                const int shift = band_last - k->run_last;
                band_first = k->first + shift;
                band_last = k->last + shift;
                if (shift == 0)
                    _snprintf_s(known, sizeof(known), _TRUNCATE, " (%s%s, verified band)", k->name,
                                class_id >= 0 ? "" : " by run end - class byte unreadable");
                else
                    _snprintf_s(known, sizeof(known), _TRUNCATE,
                                " (%s, run end shifted %+d from the sweep's d%d - band shifted too; LOOK at the hands)",
                                k->name, shift, k->run_last);
            } else {
                // Unknown model: the classifier's cutoff (3.0) admits wrist
                // draws, so drop end draws with a mean bone distance over
                // HAND_CORE while at least 4 remain. Not exact for every
                // model; add verified models to KNOWN_BANDS.
                constexpr float HAND_CORE = 1.5f;
                while (band_last - band_first + 1 > 4 && partmap::HandBandMean(band_first) > HAND_CORE)
                    band_first++;
                while (band_last - band_first + 1 > 4 && partmap::HandBandMean(band_last) > HAND_CORE)
                    band_last--;
                _snprintf_s(known, sizeof(known), _TRUNCATE,
                            " (UNKNOWN class %d - look at the hands, then add it to KNOWN_BANDS)", class_id);
            }
            band_known = true;
            diag::Log("handcap: auto band d%d..d%d (classifier run d%d..d%d, %d draws, mean bone dist %.2f)%s",
                      band_first, band_last, partmap::handband_best_first, full_last,
                      partmap::handband_best_n, partmap::handband_best_mean, known);
            // The arm-hide range ends at the run's untrimmed end (arms draw
            // before hands), so a trailing wrist chunk hides too.
            if (vrmod::config.hide_arms_count != full_last) {
                diag::Log("hidearms: count %d -> %d (auto, from the hand band)",
                          vrmod::config.hide_arms_count, full_last);
                vrmod::config.hide_arms_count = full_last;
            }
        } else {
            if (auto_log_budget > 0) {
                auto_log_budget--;
                diag::Log("handcap: auto band not found (best run %d draws) - retry in %d passes",
                          partmap::handband_best_n, AUTO_BACKOFF_PASSES);
            }
            auto_backoff = AUTO_BACKOFF_PASSES;
        }
        return;
    }
    if (HaveHand() && !cache_stale)
        return;   // captured already
    if (auto_tries >= AUTO_MAX_TRIES)
        return;   // gave up for this session (manual 'handcap' still works)
    if (!band_known) {
        if (vrmod::config.handband_request == 0 && partmap::handband_passes == 0) {
            partmap::handband_quiet = true;
            partmap::handband_done = false;
            vrmod::config.handband_request = AUTO_BAND_PASSES;
            band_running = true;
        }
        return;
    }
    ArmCapture(band_first, band_last, true);
}

// Once per BeginScene pass, after weapongrip::OnFrame.
inline void OnFrame() {
    drawn_this_pass = false;
    world_seen_this_pass = false;
    // The previous pass's array bone serves the settle test below.
    if (have_array_bone) {
        have_array_bone_prev = true;
        array_bone_prev = array_bone;
        array_bone_age = 0;
    } else if (have_array_bone_prev && ++array_bone_age > ARRAY_BONE_KEEPALIVE) {
        have_array_bone_prev = false;
    }
    have_array_bone = false;

    if (vrmod::config.hand_presence != prev_enabled) {
        prev_enabled = vrmod::config.hand_presence;
        if (vrmod::config.hand_presence)
            log_budget = 8;
    }

    // Manual arm (developer build) - also teaches the band.
    if (vrmod::config.handcap_first > 0 && vrmod::config.handcap_last > 0) {
        const int first = vrmod::config.handcap_first, last = vrmod::config.handcap_last;
        vrmod::config.handcap_first = 0;
        vrmod::config.handcap_last = 0;
        if (ArmCapture(first, last, false)) {
            band_first = first;
            band_last = last;
            band_known = true;
        }
    }

    if (cap_done_pending)
        Finalize();

    AutoStep();


    if (!cap_armed)
        return;

    // Wait for the hand bone (bracket stash, else the previous pass's
    // array bone) to settle, then capture one pass.
    cap_this_pass = false;
    const bool bracket = vrmod::have_hand_bone_right;
    const D3DMATRIX* bone = bracket ? &vrmod::hand_bone_world_right
                                    : (have_array_bone_prev ? &array_bone_prev : nullptr);
    if (bone != nullptr) {
        const float dx = bone->_41 - cap_last_bone[0];
        const float dy = bone->_42 - cap_last_bone[1];
        const float dz = bone->_43 - cap_last_bone[2];
        if (cap_have_last_bone &&
            dx * dx + dy * dy + dz * dz <
                CAP_SETTLE_EPS * CAP_SETTLE_EPS)
            cap_settle++;
        else
            cap_settle = 0;
        cap_last_bone[0] = bone->_41;
        cap_last_bone[1] = bone->_42;
        cap_last_bone[2] = bone->_43;
        cap_have_last_bone = true;
        if (cap_settle >= CAP_SETTLE_PASSES) {
            cap_bone = *bone;   // unarmed: replaced in-pass by CaptureObserve
            cap_this_pass = true;
        }
    } else {
        cap_settle = 0;
        cap_have_last_bone = false;
    }
    // An auto capture waits indefinitely; a manual one times out.
    if (cap_auto)
        return;
    if (--cap_timeout <= 0) {
        cap_armed = false;
        cap_this_pass = false;
        diag::Log("handcap: TIMED OUT (%s) - stand still in gameplay, then re-arm",
                  bone != nullptr ? "bone never settled" : "no hand bone - body not drawing");
    }
}

inline D3DMATRIX UniformScale(float s) {
    D3DMATRIX m = vrmod::Identity();
    m._11 = m._22 = m._33 = s;
    return m;
}

// Both hands draw once per pass, just before the first 2D (RHW) draw
// after the scene: the HUD always follows the scene and precedes the
// radar's view change, so the eye views are still current and the whole
// scene is in the depth buffer (the body can draw before the level, so
// drawing after it would let backdrops paint over the hands). The draw
// proxies report each draw before it runs (MaybeDraw); EndScene covers a
// pass with no 2D draw (OnPassEnd).
inline void DrawNow(IDirect3DDevice9* dev, const char* trigger);

inline bool Gated() {
    if (!vrmod::config.hand_presence || !HaveHand())
        return true;
    if (resolution::passthrough || !stereo::WantsDuplication())
        return true;
    if (!gamecam::DrivesView())
        return true;
    return false;
}

inline void MaybeDraw(IDirect3DDevice9* dev, bool scene_draw) {
    if (Gated())
        return;
    if (scene_draw) {
        if (!stereo::GameViewportIsBoxed())
            world_seen_this_pass = true;
        return;
    }
    if (drawn_this_pass || !world_seen_this_pass)
        return;
    if (stereo::GameViewportIsBoxed())
        return;  // a boxed 3D-UI pass is on: its view is not the scene's
    DrawNow(dev, "first 2D draw");
}

// EndScene: a pass that drew the scene but never a 2D draw.
inline void OnPassEnd(IDirect3DDevice9* dev) {
    if (Gated() || drawn_this_pass || !world_seen_this_pass)
        return;
    if (stereo::GameViewportIsBoxed())
        return;
    DrawNow(dev, "end of scene");
}

inline void DrawNow(IDirect3DDevice9* dev, const char* trigger) {
    drawn_this_pass = true;

    D3DMATRIX hand_world[2];
    bool have[2] = {};
    for (int h = 0; h < 2; h++) {
        D3DMATRIX pose;
        if (vrmod::Get()->GetHandPose(h, pose))
            have[h] = gamecam::WorldFromTracking(pose, hand_world[h]);
    }
    if (log_budget > 0) {
        log_budget--;
        diag::Log("handpresence: draw at %s, pose L=%d R=%d draws=%d "
                  "viewport %ux%u", trigger, have[0] ? 1 : 0, have[1] ? 1 : 0,
                  hand.draw_count, (unsigned)resolution::last_game_viewport.Width,
                  (unsigned)resolution::last_game_viewport.Height);
    }
    if (!have[0] && !have[1])
        return;

    // The hand rides the same grip tuple as the weapon in hand
    // (weapongrip::ActiveGripOffset), which keeps the game's hand-on-weapon
    // relation, plus the hand-only trim ([vr] hand_*).
    const D3DMATRIX offset = weapongrip::ActiveGripOffset();
    const D3DMATRIX trim_delta = weapongrip::GripOffsetMatrix(
        vrmod::config.hand_pitch_deg, vrmod::config.hand_roll_deg,
        vrmod::config.hand_yaw_deg, vrmod::config.hand_fwd_cm,
        vrmod::config.hand_up_cm);
    const D3DMATRIX scale = UniformScale(vrmod::config.hand_scale);
    const D3DMATRIX local =
        vrmod::Multiply(vrmod::Multiply(scale, offset), trim_delta);

    // Save the device state both passes touch.
    D3DMATRIX saved_world;
    dev->GetTransform(D3DTS_WORLD, &saved_world);
    DWORD saved_fvf = 0;
    dev->GetFVF(&saved_fvf);
    IDirect3DBaseTexture9* saved_tex = nullptr;
    dev->GetTexture(0, &saved_tex);
    IDirect3DVertexBuffer9* saved_vb = nullptr;
    UINT saved_vb_offset = 0, saved_vb_stride = 0;
    dev->GetStreamSource(0, &saved_vb, &saved_vb_offset, &saved_vb_stride);
    DWORD saved_lighting, saved_blend, saved_fog, saved_cull, saved_tfactor;
    dev->GetRenderState(D3DRS_LIGHTING, &saved_lighting);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &saved_blend);
    dev->GetRenderState(D3DRS_FOGENABLE, &saved_fog);
    dev->GetRenderState(D3DRS_CULLMODE, &saved_cull);
    dev->GetRenderState(D3DRS_TEXTUREFACTOR, &saved_tfactor);
    // The trigger point is a 2D draw (depth often off): set the full 3D
    // state explicitly.
    DWORD saved_z, saved_zwrite, saved_zfunc, saved_atest, saved_stencil,
          saved_clip, saved_cwrite;
    dev->GetRenderState(D3DRS_ZENABLE, &saved_z);
    dev->GetRenderState(D3DRS_ZWRITEENABLE, &saved_zwrite);
    dev->GetRenderState(D3DRS_ZFUNC, &saved_zfunc);
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &saved_atest);
    dev->GetRenderState(D3DRS_STENCILENABLE, &saved_stencil);
    dev->GetRenderState(D3DRS_CLIPPLANEENABLE, &saved_clip);
    dev->GetRenderState(D3DRS_COLORWRITEENABLE, &saved_cwrite);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    DWORD saved_colorop, saved_colorarg1, saved_colorarg2;
    DWORD saved_alphaop, saved_alphaarg2, saved_stage1op;
    dev->GetTextureStageState(0, D3DTSS_COLOROP, &saved_colorop);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &saved_colorarg1);
    dev->GetTextureStageState(0, D3DTSS_COLORARG2, &saved_colorarg2);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &saved_alphaop);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG2, &saved_alphaarg2);
    dev->GetTextureStageState(1, D3DTSS_COLOROP, &saved_stage1op);

    dev->SetRenderState(D3DRS_LIGHTING, FALSE);   // prelit vertex diffuse
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, INTERIOR_COLOR);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

    for (int h = 0; h < 2; h++) {
        if (!have[h])
            continue;
        const D3DMATRIX world = vrmod::Multiply(local, hand_world[h]);
        dev->SetTransform(D3DTS_WORLD, &world);
        // Body front faces wind CCW (body_cull=1 culls CW to hide the
        // interior); the mirrored set is flipped. hand_cull_flip
        // swaps the convention.
        DWORD cull_front = h == 0 ? D3DCULL_CCW : D3DCULL_CW;
        DWORD cull_back = h == 0 ? D3DCULL_CW : D3DCULL_CCW;
        if (vrmod::config.hand_cull_flip) {
            const DWORD t = cull_front;
            cull_front = cull_back;
            cull_back = t;
        }
        for (int d = 0; d < hand.draw_count; d++) {
            const CapturedDraw& c = hand.draws[d];
            const uint8_t* data = h == 0 ? c.mirrored : c.raw;
            dev->SetFVF(c.fvf);
            // Pass 1: the shell's inside, flat dark.
            dev->SetRenderState(D3DRS_CULLMODE, cull_back);
            dev->SetTexture(0, nullptr);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
            stereo::Duplicate(dev, [&](int) {
                dev->DrawPrimitiveUP(c.type, c.prims, data, c.stride);
            });
            // Pass 2: texture only. The captured vertex diffuse is the
            // game's runtime lighting and can be black, so no modulate.
            dev->SetRenderState(D3DRS_CULLMODE, cull_front);
            dev->SetTexture(0, c.texture);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            stereo::Duplicate(dev, [&](int) {
                dev->DrawPrimitiveUP(c.type, c.prims, data, c.stride);
            });
        }
    }

    // Restore. DrawPrimitiveUP clobbered stream 0; put the game's back.
    dev->SetTransform(D3DTS_WORLD, &saved_world);
    dev->SetFVF(saved_fvf);
    dev->SetTexture(0, saved_tex);
    if (saved_tex != nullptr)
        saved_tex->Release();
    if (saved_vb != nullptr) {
        dev->SetStreamSource(0, saved_vb, saved_vb_offset, saved_vb_stride);
        saved_vb->Release();
    }
    dev->SetRenderState(D3DRS_LIGHTING, saved_lighting);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, saved_blend);
    dev->SetRenderState(D3DRS_FOGENABLE, saved_fog);
    dev->SetRenderState(D3DRS_CULLMODE, saved_cull);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, saved_tfactor);
    dev->SetRenderState(D3DRS_ZENABLE, saved_z);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, saved_zwrite);
    dev->SetRenderState(D3DRS_ZFUNC, saved_zfunc);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, saved_atest);
    dev->SetRenderState(D3DRS_STENCILENABLE, saved_stencil);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, saved_clip);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, saved_cwrite);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, saved_colorop);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, saved_colorarg1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, saved_colorarg2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, saved_alphaop);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, saved_alphaarg2);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, saved_stage1op);
}

}  // namespace hands
