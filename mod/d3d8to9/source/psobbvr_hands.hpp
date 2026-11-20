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

#include "psobbvr_eyeheight.hpp"
#include "psobbvr_handref.hpp"
#include "psobbvr_handweld.hpp"
#include "psobbvr_ikarms.hpp"
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
// BEGIN hand band tables (generated)
// The left hand's band per class: the numbered body draws its glove mesh
// (the node on bone 34, the right hand's 47) arrives as, wrist cuffs left
// out. Generated: do not edit by hand. The right hand's bands are
// KNOWN_BANDS above.
struct HandBandRow {
    int class_id;
    int first, last;   // draw numbers, from 1
};
constexpr HandBandRow LEFT_BANDS[CLASS_COUNT] = {
    { 0, 25, 34},  // HUmar
    { 1, 8, 18},  // HUnewearl
    { 2, 25, 38},  // HUcast
    { 3, 16, 25},  // RAmar
    { 4, 26, 36},  // RAcast
    { 5, 19, 29},  // RAcaseal
    { 6, 23, 32},  // FOmarl
    { 7, 13, 21},  // FOnewm
    { 8, 23, 35},  // FOnewearl
    { 9, 20, 27},  // HUcaseal
    {10, 19, 30},  // FOmar
    {11, 18, 26},  // RAmarl
};
// Draws right after a band that the game draws before the walk has written
// the grip bone (the wrist cuffs left out of the band): the capture reads the
// grip bone's frame only after them. Per class, [0] left, [1] right.
constexpr unsigned char IK_RESOLVE_DELAY[2][CLASS_COUNT] = {
    {0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0},  // left
    {0, 0, 0, 0, 0, 0, 2, 1, 0, 0, 0, 0},  // right
};
// END hand band tables (generated)
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
                    // No verdict from this weapon; it stays marked as checked
                    // so the scan does not repeat every draw until it changes.
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


// ---- IK-arms mode: both hands as the game draws them
// With ik_arms on, the game poses and draws both hands with the bones the IK
// writes. A hand is one rigid mesh on its glove node, so its shape in the
// grip point's frame never changes: the first complete band run gives it,
// with no settle wait, whatever the controllers are doing. Per hand (0 =
// left, 1 = right, as the controllers and ikarms::ARM_BONES) the capture
// moves the band's vertices into the grip frame, welds them (handweld) and
// keeps the welded mesh; every body run then records the transform from that
// frame to the run's root-local space, which is what a later per-vertex
// change needs to be written back into the band's draws.
//
// The bands: the right hand's is KNOWN_BANDS, the left's LEFT_BANDS, at the
// tables' own draw numbers. The replay capture's learned shift is not
// applied: a classifier run that ends a draw early would read as a shift.
// A mesh is replaced after the player has been out of gameplay a while,
// when the class changes, or when a band draw stops matching it.
//
// The grip bone's frame is read from the bone array, which a body run's first
// draw finds holding the previous walk's matrices: the walk writes each bone
// as it reaches it, and draws the bone's mesh right after. So a capture is
// taken in two steps. The band's draws are collected as drawn (root-local),
// and the grip frame is read at the first draw after the band, when the walk
// has written the grip bone, and only then are the vertices moved into it.
// The finished capture is then held against the reference hand of the
// class's build (psobbvr_handref.hpp) and kept only if it matches.
constexpr int IK_FAIL_BACKOFF_RUNS = 60;   // body runs to wait after a rejected capture
constexpr int IK_MAX_TRIES = 6;            // rejected captures before the retries slow down
constexpr int IK_SLOW_BACKOFF_RUNS = 300;  // body runs between retries after that
constexpr float IK_BOX_MIN = 0.5f;         // the hand's box: each axis between these (units)
constexpr float IK_BOX_MAX = 5.0f;
constexpr int IK_MIN_WELDED = 12;          // a hand welds to 29-36 vertices, 52-68 triangles
constexpr int IK_MIN_TRIS = 20;

struct IkHand {
    bool have;           // a welded mesh is held
    bool stale;          // it is to be replaced by the next clean capture
    uint32_t generation; // counts the captures kept (changes when one replaces another)
    int class_id;        // the class the mesh came from
    int first, last;     // the band's draw numbers it came from
    int cap_count;       // band draws collected in the run in flight
    float frame_lag;     // how far the previous walk's grip frame was from the fresh one (units)
    bool capturing;
    bool finish_pending;   // the band is collected; the grip frame is read at the next draw
    int tries, backoff;
    unsigned stride[handweld::MAX_DRAWS];   // per band draw, for the substitution's check
    DWORD fvf[handweld::MAX_DRAWS];
    handweld::Mesh mesh;                    // welded, in the grip point's frame
    // This body run's frame and band (valid from the run's first draw).
    bool pass_valid;
    int run_first, run_last;   // the band's draw numbers this run
    int run_resolve;           // the capture's grip frame is read at the first draw after this one
    D3DMATRIX run_root;        // the body root matrix of this run
    D3DMATRIX to_grip;         // root-local -> grip frame: bodyRoot x inverse(gripBoneWorld)
    D3DMATRIX to_root;         // grip frame -> root-local: the rigid inverse
    float rot[9];              // to_root's rotation, row-vector order
    // The shape change the substitution applies (SetReshape).
    const float (*moves)[3];
    const float (*normals)[3];
    const float (*turns)[9];   // set: SubstituteTurned, the normals unused
    uint32_t moves_generation;
};
inline IkHand ik_hand[2];
inline int ik_run = 0;                  // body runs begun (any pass)
inline int ik_gap_passes = 0;           // passes out of gameplay
inline bool ik_recapture_request = false;   // capture both hands again
constexpr int IK_LOG_BUDGET = 40;       // capture lines per character
inline int ik_log_budget = IK_LOG_BUDGET;
inline int ik_run_class = -1;           // the class of the last body run
inline int band_shift = 0;              // the replay capture's learned shift from KNOWN_BANDS (AutoStep); reported only
// Capture scratch: the band's positions in the grip frame, and each draw's
// shape.
struct IkScratchDraw {
    int type;
    unsigned prims, verts, stride;
    DWORD fvf;
};
inline float ik_scratch_pos[2][handweld::MAX_DRAWS][handweld::MAX_VERTS][3];
inline IkScratchDraw ik_scratch_draw[2][handweld::MAX_DRAWS];
inline handweld::Mesh ik_mesh_tmp;

inline const char* IkSideName(int side) { return side == 0 ? "left" : "right"; }
inline const char* const ik_hold_name[3] = {"unrecorded", "held", "recording"};

// The band's draw numbers of a hand for a class. False for a class with no
// row.
inline bool IkBandFor(int side, int class_id, int& first, int& last) {
    if (class_id < 0)
        return false;
    if (side == 1) {
        const KnownBand* k = MatchKnownBandByClass(class_id);
        if (k == nullptr)
            return false;
        first = k->first;
        last = k->last;
        return true;
    }
    for (const HandBandRow& r : LEFT_BANDS) {
        if (r.class_id == class_id) {
            first = r.first;
            last = r.last;
            return true;
        }
    }
    return false;
}

// A body run's first draw: this run's grip frames and bands. The bone array
// holds the IK-posed bones of the walk that just ran.
inline void IkBeginRun() {
    ik_run++;
    const int class_id = ReadPlayerClass();
    const D3DMATRIX body_root = partmap::current_world;
    if (class_id >= 0 && class_id != ik_run_class) {
        // Another character: its captures start over at once, whatever the
        // last one's were doing, and are logged afresh.
        if (ik_run_class >= 0) {
            ik_log_budget = IK_LOG_BUDGET;
            for (IkHand& h : ik_hand) {
                h.tries = 0;
                h.backoff = 0;
            }
        }
        ik_run_class = class_id;
    }
    for (int side = 0; side < 2; side++) {
        IkHand& h = ik_hand[side];
        h.pass_valid = false;
        h.run_root = body_root;
        if (h.finish_pending) {
            // The run ended before the draw that reads the frame.
            h.finish_pending = false;
            h.capturing = false;
        }
        if (class_id < 0)
            continue;
        if (h.have && !h.stale && h.class_id != class_id) {
            h.stale = true;
            h.tries = 0;
            h.backoff = 0;
            if (ik_log_budget > 0) {
                ik_log_budget--;
                diag::Log("ikhand: %s hand's class changed %d -> %d - mesh to be replaced",
                          IkSideName(side), h.class_id, class_id);
            }
        }
        D3DMATRIX grip_world;
        if (!ArrayBoneWorld(ikarms::ARM_BONES[side].grip, grip_world))
            continue;
        if (!IkBandFor(side, class_id, h.run_first, h.run_last))
            continue;
        h.run_resolve = h.run_last + (class_id < CLASS_COUNT ? IK_RESOLVE_DELAY[side][class_id] : 0);
        h.to_grip = vrmod::Multiply(body_root, vrmod::RigidInverse(grip_world));
        h.to_root = vrmod::RigidInverse(h.to_grip);
        h.rot[0] = h.to_root._11; h.rot[1] = h.to_root._12; h.rot[2] = h.to_root._13;
        h.rot[3] = h.to_root._21; h.rot[4] = h.to_root._22; h.rot[5] = h.to_root._23;
        h.rot[6] = h.to_root._31; h.rot[7] = h.to_root._32; h.rot[8] = h.to_root._33;
        h.pass_valid = true;
    }
}

// A capture is dropped; the next try waits, and after IK_MAX_TRIES rejections
// in a row the retries only slow down, so a hand is never locked out for good.
// Rejections are logged for the first tries and then every tenth.
inline void IkReject(int side, const char* why, float error = -1.0f) {
    IkHand& h = ik_hand[side];
    h.capturing = false;
    h.finish_pending = false;
    h.tries++;
    h.backoff = h.tries >= IK_MAX_TRIES ? IK_SLOW_BACKOFF_RUNS : IK_FAIL_BACKOFF_RUNS;
    if (ik_log_budget > 0 && (h.tries <= IK_MAX_TRIES || h.tries % 10 == 0)) {
        ik_log_budget--;
        if (error >= 0.0f)
            diag::Log("ikhand: %s hand capture REJECTED (%s, %.3f units off the reference), try %d",
                      IkSideName(side), why, error, h.tries);
        else
            diag::Log("ikhand: %s hand capture REJECTED (%s), try %d", IkSideName(side), why,
                      h.tries);
    }
}

// The reference hand of the seeded character for a side and class, as the
// nine numbers handref::Stats gives a capture. False without the pad
// position (no proportion seed, or one for another class).
inline bool IkReference(int side, int class_id, float want[handref::STATS]) {
    handref::Weights w;
    if (!eyeheight::SeedPadWeights(class_id, w.shape, w.w, w.kept))
        return false;
    handref::Expected(side, class_id, w, want);
    return true;
}

// The band's last draw has been collected: weld, validate, keep.
inline void IkFinish(int side, int class_id) {
    IkHand& h = ik_hand[side];
    h.capturing = false;
    handweld::DrawIn in[handweld::MAX_DRAWS];
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    for (int d = 0; d < h.cap_count; d++) {
        in[d].type = ik_scratch_draw[side][d].type;
        in[d].prims = ik_scratch_draw[side][d].prims;
        in[d].verts = ik_scratch_draw[side][d].verts;
        in[d].pos = &ik_scratch_pos[side][d][0][0];
        for (unsigned v = 0; v < in[d].verts; v++)
            for (int a = 0; a < 3; a++) {
                const float c = ik_scratch_pos[side][d][v][a];
                if (c < mn[a]) mn[a] = c;
                if (c > mx[a]) mx[a] = c;
            }
    }
    for (int a = 0; a < 3; a++) {
        const float span = mx[a] - mn[a];
        if (span < IK_BOX_MIN || span > IK_BOX_MAX) {
            IkReject(side, "its box is not hand-sized");
            return;
        }
    }
    // The reference check: three means of the vertices against the class's
    // build blended from the creation pad. Without the pad position, against
    // the range the class's shapes span.
    float stats[handref::STATS], want[handref::STATS];
    unsigned vert_counts[handweld::MAX_DRAWS];
    for (int d = 0; d < h.cap_count; d++)
        vert_counts[d] = ik_scratch_draw[side][d].verts;
    float ref_error = 0.0f;
    bool by_pad = false;
    if (class_id < 0 || class_id >= 12 ||
        !handref::Stats(ik_scratch_pos[side], vert_counts, h.cap_count, stats)) {
        IkReject(side, "its class or vertices are unusable");
        return;
    }
    if (IkReference(side, class_id, want)) {
        by_pad = true;
        ref_error = handref::Error(stats, want);
    } else {
        ref_error = handref::OutsideClassRange(side, class_id, stats);
    }
    if (ref_error > handref::TOLERANCE) {
        IkReject(side, by_pad ? "it is not the hand of the pad's build"
                              : "it lies outside the class's hands", ref_error);
        return;
    }
    if (!handweld::Build(ik_mesh_tmp, in, h.cap_count)) {
        IkReject(side, "the welding failed");
        return;
    }
    if (ik_mesh_tmp.vertex_count < IK_MIN_WELDED || ik_mesh_tmp.tri_count < IK_MIN_TRIS) {
        IkReject(side, "too few welded vertices or triangles");
        return;
    }
    memcpy(&h.mesh, &ik_mesh_tmp, sizeof(handweld::Mesh));
    for (int d = 0; d < h.cap_count; d++) {
        h.stride[d] = ik_scratch_draw[side][d].stride;
        h.fvf[d] = ik_scratch_draw[side][d].fvf;
    }
    h.have = true;
    h.stale = false;
    h.finish_pending = false;
    h.generation++;
    h.class_id = class_id;
    h.first = h.run_first;
    h.last = h.run_last;
    h.tries = 0;
    h.backoff = 0;
    if (ik_log_budget > 0) {
        ik_log_budget--;
        diag::Log("ikhand: %s hand CAPTURED class %d band d%d..d%d, %d draws, %d welded vertices, "
                  "%d triangles, grip-frame box x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f], generation %u, "
                  "%.3f units from the %s reference, frame lag %.3f, grip %s",
                  IkSideName(side), class_id, h.first, h.last, h.cap_count, h.mesh.vertex_count,
                  h.mesh.tri_count, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], (unsigned)h.generation,
                  ref_error, by_pad ? "pad" : "class-range", h.frame_lag,
                  ik_hold_name[ikarms::HandHoldState(side)]);
    }
}

// The first draw after a band and the wrist cuffs the game draws with its
// glove node (IK_RESOLVE_DELAY): the walk has written the grip bone, so the
// frame read now is the one the band's vertices were skinned with. Moves the
// collected vertices into it and finishes the capture.
inline void IkResolve(int side) {
    IkHand& h = ik_hand[side];
    h.finish_pending = false;
    D3DMATRIX grip_world;
    if (!ArrayBoneWorld(ikarms::ARM_BONES[side].grip, grip_world)) {
        IkReject(side, "the grip bone is unreadable");
        return;
    }
    const D3DMATRIX fresh = vrmod::Multiply(h.run_root, vrmod::RigidInverse(grip_world));
    // The earlier frame (read at the run's first draw) against this one, in
    // the grip frame's units: the translation of fresh x inverse(earlier).
    const D3DMATRIX lag = vrmod::Multiply(fresh, vrmod::RigidInverse(h.to_grip));
    h.frame_lag = sqrtf(lag._41 * lag._41 + lag._42 * lag._42 + lag._43 * lag._43);
    const D3DMATRIX& m = fresh;
    for (int d = 0; d < h.cap_count; d++)
        for (unsigned i = 0; i < ik_scratch_draw[side][d].verts; i++) {
            float* o = ik_scratch_pos[side][d][i];
            const float x = o[0], y = o[1], z = o[2];
            o[0] = x * m._11 + y * m._21 + z * m._31 + m._41;
            o[1] = x * m._12 + y * m._22 + z * m._32 + m._42;
            o[2] = x * m._13 + y * m._23 + z * m._33 + m._43;
        }
    IkFinish(side, ReadPlayerClass());
}

// One body-run draw (from CaptureObserve, before the arm hide counts it).
inline void IkObserve(const void* data, UINT stride, UINT verts, D3DPRIMITIVETYPE type,
                      UINT prims, DWORD fvf) {
    if (!ikarms::Active() || !partmap::body_run_open || data == nullptr)
        return;
    if (partmap::body_run_draws == 0)
        IkBeginRun();
    const int idx = partmap::body_run_draws + 1;
    for (int side = 0; side < 2; side++) {
        IkHand& h = ik_hand[side];
        if (!h.pass_valid)
            continue;
        if (h.finish_pending && idx > h.run_resolve)
            IkResolve(side);
        if (idx < h.run_first || idx > h.run_last)
            continue;
        const int ord = idx - h.run_first;
        if (h.have && !h.stale) {
            // Held: the draw must still be the one the mesh came from.
            if (h.first == h.run_first && h.last == h.run_last && ord < h.mesh.draw_count &&
                h.mesh.draws[ord].verts == verts && h.mesh.draws[ord].prims == prims &&
                h.mesh.draws[ord].type == (int)type && h.stride[ord] == stride && h.fvf[ord] == fvf)
                continue;
            h.stale = true;
            h.tries = 0;
            h.backoff = 0;
            if (ik_log_budget > 0) {
                ik_log_budget--;
                diag::Log("ikhand: %s hand's band draw d%d no longer matches the mesh (verts %u, was %u) - mesh to be replaced",
                          IkSideName(side), idx, (unsigned)verts,
                          ord < h.mesh.draw_count ? h.mesh.draws[ord].verts : 0u);
            }
            continue;
        }
        if (idx == h.run_first && h.backoff > 0)
            h.backoff--;
        if (h.backoff > 0)
            continue;
        if (idx == h.run_first) {
            h.cap_count = 0;
            h.capturing = true;
        }
        if (!h.capturing)
            continue;
        if (ord != h.cap_count || ord >= handweld::MAX_DRAWS || verts == 0 ||
            verts > (UINT)handweld::MAX_VERTS || stride < 12 || stride > (UINT)handweld::MAX_STRIDE) {
            IkReject(side, "a band draw is out of order or too big");
            continue;
        }
        IkScratchDraw& sd = ik_scratch_draw[side][ord];
        sd.type = (int)type;
        sd.prims = prims;
        sd.verts = verts;
        sd.stride = stride;
        sd.fvf = fvf;
        // Root-local for now; IkResolve moves them into the grip frame.
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (UINT i = 0; i < verts; i++, p += stride) {
            const float* v = reinterpret_cast<const float*>(p);
            float* o = ik_scratch_pos[side][ord][i];
            o[0] = v[0];
            o[1] = v[1];
            o[2] = v[2];
        }
        h.cap_count++;
        if (idx == h.run_last)
            h.finish_pending = true;
    }
}

// Once per pass: the staleness rules and a pending recapture request.
inline void IkOnFrame() {
    if (ik_recapture_request) {
        ik_recapture_request = false;
        for (IkHand& h : ik_hand) {
            if (h.have)
                h.stale = true;
            h.tries = 0;
            h.backoff = 0;
        }
        ik_log_budget = IK_LOG_BUDGET;
        diag::Log("ikhand: recapture requested - both meshes to be replaced");
    }
    if (ikarms::Active()) {
        ik_gap_passes = 0;
        return;
    }
    if (ik_gap_passes < 100000)
        ik_gap_passes++;
    if (ik_gap_passes == STALE_GAP_PASSES) {
        for (IkHand& h : ik_hand) {
            if (h.have)
                h.stale = true;
            h.tries = 0;
            h.backoff = 0;
        }
    }
}

// What the reshaping stage reads. side: 0 = left, 1 = right.
struct IkWelded {
    bool valid;                    // a mesh is held and is not due for replacement
    uint32_t generation;           // changes whenever a capture replaces the mesh
    int class_id;
    int first, last;               // the band's draw numbers
    const handweld::Mesh* mesh;    // positions in the grip point's frame, triangles, per-draw map
};
inline IkWelded Welded(int side) {
    const IkHand& h = ik_hand[side & 1];
    return {h.have && !h.stale, h.generation, h.class_id, h.first, h.last, &h.mesh};
}
// This body run's transform from the hand's grip frame to root-local space
// (valid once the run's first draw has been seen, until the next run).
inline bool IkGripToRoot(int side, D3DMATRIX& out) {
    const IkHand& h = ik_hand[side & 1];
    if (!h.pass_valid)
        return false;
    out = h.to_root;
    return true;
}
// Which draw of the hand's band this run's draw number is (0-based), or -1:
// not in the band, or no mesh to map it with.
inline int IkBandDraw(int side, int draw_number) {
    const IkHand& h = ik_hand[side & 1];
    if (!h.have || h.stale || !h.pass_valid || h.first != h.run_first || h.last != h.run_last ||
        draw_number < h.first || draw_number > h.last)
        return -1;
    return draw_number - h.first;
}

// The shape change to write into the hand's band draws: one move per welded
// vertex in the grip frame (and optionally a normal per welded vertex in the
// same frame), valid for the mesh generation given. The arrays are the
// caller's and must stay put until replaced; null moves clears it.
// With turns (one 3x3 rotation per welded vertex in the grip frame, row-vector
// order, identity or zeros for none) the draws go through
// handweld::SubstituteTurned: the moves are the whole displacement, the
// game's own normals are turned, and normals is not used. Without turns, the
// moved vertices take the given welded normals (handweld::Substitute).
inline void IkSetReshape(int side, const float (*moves)[3], const float (*normals)[3],
                         uint32_t generation, const float (*turns)[9] = nullptr) {
    IkHand& h = ik_hand[side & 1];
    h.moves = moves;
    h.normals = normals;
    h.turns = turns;
    h.moves_generation = generation;
}
// The master switch for the substitution: off, SubstituteBandDraw returns its
// input.
inline bool ik_reshape_enabled = false;

inline uint8_t ik_subst_buffer[2][handweld::MAX_VERTS * handweld::MAX_STRIDE];

// The proxy's one call: the draw's vertex data, or - when the reshaping is
// on, this draw is in a hand's band, the band mesh is current and a shape
// change is set for it - a copy with the welded vertices moved. The game's
// buffer (its skinning cache) is never written. Call it before the arm hide
// counts the draw.
inline const void* SubstituteBandDraw(const void* data, UINT stride, UINT verts,
                                      D3DPRIMITIVETYPE type, UINT prims, DWORD fvf) {
    if (!ik_reshape_enabled || data == nullptr || !partmap::body_run_open)
        return data;
    const int idx = partmap::body_run_draws + 1;
    for (int side = 0; side < 2; side++) {
        const IkHand& h = ik_hand[side];
        const int ord = IkBandDraw(side, idx);
        if (ord < 0 || h.moves == nullptr || h.moves_generation != h.generation)
            continue;
        const handweld::Draw& d = h.mesh.draws[ord];
        if (d.verts != verts || d.prims != prims || d.type != (int)type || h.stride[ord] != stride ||
            h.fvf[ord] != fvf)
            continue;
        const bool has_normal = (fvf & D3DFVF_NORMAL) != 0;
        if (h.turns != nullptr)
            handweld::SubstituteTurned(d, static_cast<const uint8_t*>(data), stride, has_normal,
                                       h.moves, h.turns, h.rot, ik_subst_buffer[side]);
        else
            handweld::Substitute(d, static_cast<const uint8_t*>(data), stride, has_normal,
                                 h.moves, h.normals, h.rot, ik_subst_buffer[side]);
        return ik_subst_buffer[side];
    }
    return data;
}

// One body-run user-pointer draw (device UP path, just before
// HideArmDrawNow, so it also sees draws the arm hide then drops).
inline void CaptureObserve(IDirect3DDevice9* dev, const void* data,
                           UINT stride, UINT verts, D3DPRIMITIVETYPE type,
                           UINT prims, DWORD fvf) {
    IkObserve(data, stride, verts, type, prims, fvf);
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
                band_shift = shift;
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
    IkOnFrame();


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

// The replay's frame relative to the controller, without its scale: the
// grip tuple of the weapon in hand (weapongrip::ActiveGripOffset), which
// keeps the game's hand-on-weapon relation, then the hand-only trim
// ([vr] hand_*).
inline D3DMATRIX ReplayOffset() {
    const D3DMATRIX trim_delta = weapongrip::GripOffsetMatrix(
        vrmod::config.hand_pitch_deg, vrmod::config.hand_roll_deg,
        vrmod::config.hand_yaw_deg, vrmod::config.hand_fwd_cm,
        vrmod::config.hand_up_cm);
    return vrmod::Multiply(weapongrip::ActiveGripOffset(), trim_delta);
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
    if (vrmod::config.ik_arms != 0)
        return true;  // the game's own hands are posed at the controllers
    if (resolution::passthrough || !stereo::WantsDuplication())
        return true;
    if (!gamecam::DrivesView())
        return true;
    return false;
}

// What a hand is drawn by this pass.
enum class HandSource { IkTarget, Replay, BareGrip };

inline const char* HandSourceName(HandSource s) {
    return s == HandSource::IkTarget ? "IK target"
           : s == HandSource::Replay ? "hand replay"
                                     : "bare grip";
}

// The world frame a hand (0 = left, 1 = right) is drawn in this pass, for
// things placed on it, and the mesh scale that applies to positions in
// it (the frame itself is rigid):
// - IK arms posing the game's hand: their grip-point target, grip tuple x
//   controller (the hand is rigid to that bone), scale 1;
// - the replay drawing (exactly when Gated() lets it): its own chain,
//   ReplayOffset() x controller, scale hand_scale;
// - neither: the grip tuple on the controller, scale 1.
// All three include the active grip tuple, so a gun's tuple moves the
// frame too. False with no controller pose in the world.
inline bool DrawnHandFrame(int side, D3DMATRIX& frame, float& mesh_scale,
                           HandSource& source) {
    side &= 1;
    if (ikarms::Active() && ikarms::arms[side].have_target) {
        frame = ikarms::arms[side].target_world;
        mesh_scale = 1.0f;
        source = HandSource::IkTarget;
        return true;
    }
    D3DMATRIX pose, hand_world;
    if (!vrmod::Get()->GetHandPose(side, pose) ||
        !gamecam::WorldFromTracking(pose, hand_world))
        return false;
    if (!Gated()) {
        frame = vrmod::Multiply(ReplayOffset(), hand_world);
        mesh_scale = vrmod::config.hand_scale;
        source = HandSource::Replay;
    } else {
        frame = vrmod::Multiply(weapongrip::ActiveGripOffset(side), hand_world);
        mesh_scale = 1.0f;
        source = HandSource::BareGrip;
    }
    return true;
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

    // The scale is uniform and outermost, so DrawnHandFrame can hand out
    // the rigid frame and the scale apart.
    const D3DMATRIX local =
        vrmod::Multiply(UniformScale(vrmod::config.hand_scale), ReplayOffset());

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
