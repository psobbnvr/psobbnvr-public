#pragma once

// Fitting the character's hand to the held weapon, with IK arms on: the two
// finger blocks open by the least angle, and the thumb slides by the least
// amount, that leave the hand clear of the weapon (psobbvr_handfit_core.hpp);
// every other point keeps the game's shape.
//
// With [vr] hand_fit = 1 this module owns the hands' reshape and the fist
// closing (psobbvr_handclose.hpp) stands down; with 0 it holds nothing and
// Step only runs closefist::Step.
//
// A fit takes tens of milliseconds, a few hundred at worst, against a 33 ms
// pass, so it runs on a worker thread. The render thread copies everything the
// fit reads into a job (the worker never touches game, capture or config
// state, and never writes the log), hands it over and later picks up the
// result: the two numbers (the fingers' angle, the thumb's slide) and the pose
// data that turns any two numbers into moved points. One thread runs one fit
// at a time; a newer request for a hand raises that hand's counter, which the
// running fit checks between search steps, so it stops early and the newest
// request runs next. The render thread logs what the worker hands back.
//
// The hand shown eases its two numbers toward the fit's and rebuilds the
// points from them each pass (Pose), moves and turns per welded point, into
// the hand-draw substitution. Until a first fit lands the hand is the game's
// plain hand; after a tuning change or a change of weapon the fit shown stays
// until the new one lands, and the numbers ease from the one to the other
// (two fits that pose the hand differently go by way of the plain hand). A
// small in-memory cache keeps one fit per hold (the item, the hand, the seat
// and the options): a weapon held before gets its fit back at once, before
// its snapshot is taken again, and the fit run on the new snapshot then takes
// its place.
//
// Hands: side 0 = left, 1 = right (the hands module's numbering); the weapon
// snapshot numbers them the other way round. The left hand holds the
// off-hand's parts on the mirror of the main seat and is solved through the
// mirror (SolveMirrored). A gun is fitted on the hand's gun seat while [vr]
// gun_seat moves the hand onto it (weapongrip::GunSeatForHand: the right hand,
// and the left only with a mechgun half drawn in it), the fist's channel line
// as for any weapon; otherwise, and for a gun with no handle line, the hand is
// left plain.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "psobbvr_eyeheight.hpp"
#include "psobbvr_handclose.hpp"
#include "psobbvr_handfit_adapt.hpp"
#include "psobbvr_hands.hpp"
#include "psobbvr_ikarms.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_weapongrip.hpp"
#include "psobbvr_weaponsnap.hpp"

namespace fithand {

constexpr int MAX_W = handweld::MAX_WELDED;
// The full range of each number eases in over this many passes, the time the
// fist closing takes to ease in; a smaller change takes less.
constexpr int EASE_PASSES = closefist::EASE_PASSES;
constexpr double EASE_DEG_PER_PASS = handfit::Options{}.block_max_deg / EASE_PASSES;  // the finger limit
constexpr double EASE_CM_PER_PASS = handfit::Options{}.thumb_max_cm / EASE_PASSES;    // the thumb limit
constexpr int CACHE_SIZE = 16;
// How long the device's teardown waits for a running fit to stop (it stops at
// its next search step, normally within a few milliseconds).
constexpr DWORD SHUTDOWN_WAIT_MS = 2000;

inline const char* SideName(int side) { return side == 0 ? "left" : "right"; }

// Everything a fit depends on. Filled from zeroed storage and compared
// bytewise: two equal keys give the same fit.
struct Key {
    uint32_t side;
    int32_t class_id;
    uint32_t kind;
    uint32_t roles_version;
    uint32_t mesh_points;
    uint32_t mesh_tris;
    uint64_t mesh_hash;     // the welded points' and triangles' bytes
    uint32_t weapon_hash;   // the snapshot's own geometry hash
    uint32_t weapon_draws;
    uint32_t item;          // the held item's code (gunseat::ItemCode)
    double weapon[16];      // weapon scale x seat, the hand's grip frame
    double seat[16];
    double channel[5];      // the handle line: point (x, y), direction
    double world_scale;
    double weapon_scale;
    double options[19];
};

// FNV-1a, 64-bit.
inline uint64_t Fnv64(uint64_t h, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
inline uint64_t MeshHash(const handweld::Mesh& m) {
    uint64_t h = 0xCBF29CE484222325ull;
    h = Fnv64(h, m.pos, sizeof(float) * 3 * (size_t)m.vertex_count);
    return Fnv64(h, m.tri, sizeof(uint16_t) * 3 * (size_t)m.tri_count);
}

// The most the thumb slides, cm: less than the core's own 3, at which the
// thumb's tip stands out too far from a weapon that touches the thumb near
// the palm. Where no slide up to it clears the weapon the thumb takes the
// least bad one.
constexpr double THUMB_MAX_DEFAULT_CM = 2.0;
// How far a thumb whose slide clears the weapon is brought back in, cm (not
// past the game's own thumb), so it rests slightly in the grip instead of
// standing off it.
constexpr double THUMB_SINK_DEFAULT_CM = 1.0;

// The thumb's tuning values (not read from the ini): the core's own
// defaults, except the most the thumb slides.
struct ThumbTune {
    double max_cm = THUMB_MAX_DEFAULT_CM;                      // the most the thumb slides
    double sink_cm = THUMB_SINK_DEFAULT_CM;                    // a clear thumb is brought back in by this much
    double judge_share = handfit::Options{}.thumb_judge_share; // a thumb point is tested from this share of the slide
    double share_from = handfit::Options{}.thumb_share_from;   // the slide starts this far along the thumb ...
    double share_to = handfit::Options{}.thumb_share_to;       // ... and is whole this far along
};
inline ThumbTune thumb_tune;

// The fit's options: the core's own, with the two rules for an unclear fit,
// their limits and each finger block's own angle from the ini, and the
// thumb's live tuning values.
inline handfit::Options CurrentOptions() {
    handfit::Options o;
    o.block_unclear = vrmod::config.hand_fit_fingers_unclear != 0 ? handfit::Unclear::Rest : handfit::Unclear::Least;
    o.thumb_unclear = vrmod::config.hand_fit_thumb_unclear != 0 ? handfit::Unclear::Rest : handfit::Unclear::Least;
    o.block_unclear_limit = vrmod::config.hand_fit_fingers_unclear_limit;
    o.thumb_unclear_limit = vrmod::config.hand_fit_thumb_unclear_limit;
    o.thumb_max_cm = thumb_tune.max_cm;
    o.thumb_sink_cm = thumb_tune.sink_cm;
    o.block_each = vrmod::config.hand_fit_fingers_each == 1;
    o.thumb_judge_share = thumb_tune.judge_share;
    o.thumb_share_from = thumb_tune.share_from;
    o.thumb_share_to = thumb_tune.share_to;
    return o;
}
inline const char* UnclearWord(int v) { return v != 0 ? "rest" : "least"; }
// A rule for an unclear fit as text: "rest", or "least up to 0.50 of the limit".
struct UnclearText {
    char s[48];
    UnclearText(int rule, double limit) {
        if (rule != 0)
            snprintf(s, sizeof s, "rest");
        else
            snprintf(s, sizeof s, "least up to %.2f of the limit", limit);
    }
};

// ------------------------------------------------------------------ the worker
namespace worker {

struct Job {
    int side = 0;
    uint32_t generation = 0;
    bool mirrored = false;
    Key key = {};
    std::shared_ptr<const handfit::Input> in;   // the job's own copy
};

// A finished fit. Immutable once handed back.
struct Result {
    Key key = {};
    uint32_t generation = 0;
    bool mirrored = false;
    handfit::Fit fit;
    std::shared_ptr<const handfit::Input> in;   // the inputs exactly as the fit got them
    double ms = 0;                              // the fit's time on the worker
};

// Shared between the render thread and the worker, under lock.
inline SRWLOCK lock = SRWLOCK_INIT;
inline Job* pending[2] = {nullptr, nullptr};    // the newest request per hand, not yet started
inline std::shared_ptr<const Result> done[2];  // the newest finished fit per hand, not yet collected
// Raised by every request and cancel for a hand; a fit whose number is no
// longer current stops at its next step and is thrown away.
inline std::atomic<uint32_t> generation[2];
inline std::atomic<bool> stop{false};
// Render thread only.
inline HANDLE thread = nullptr;
inline HANDLE wake = nullptr;   // auto-reset: a job is waiting, or stop is set
inline bool start_failed = false;

inline Job* Take(int& next) {
    Job* j = nullptr;
    AcquireSRWLockExclusive(&lock);
    for (int k = 0; k < 2 && j == nullptr; k++) {
        const int s = (next + k) & 1;
        if (pending[s] != nullptr) {
            j = pending[s];
            pending[s] = nullptr;
            next = s ^ 1;   // the other hand first next time
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return j;
}

inline void Run(const Job& j) {
    const int s = j.side;
    const auto t0 = std::chrono::steady_clock::now();
    handfit::Fit fit = j.mirrored ? handfit::SolveMirrored(*j.in, &generation[s], j.generation)
                                  : handfit::Solve(*j.in, &generation[s], j.generation);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (fit.superseded || generation[s].load() != j.generation)
        return;
    auto r = std::make_shared<Result>();
    r->key = j.key;
    r->generation = j.generation;
    r->mirrored = j.mirrored;
    r->fit = std::move(fit);
    r->in = j.in;
    r->ms = ms;
    std::shared_ptr<const Result> old = std::move(r);
    AcquireSRWLockExclusive(&lock);
    done[s].swap(old);
    ReleaseSRWLockExclusive(&lock);
}

// The thread holds a reference on this module (param), so the code it runs
// stays loaded until it has returned; it gives it back as it exits.
inline DWORD WINAPI Main(void* param) {
    int next = 1;
    while (!stop.load()) {
        WaitForSingleObject(wake, INFINITE);
        while (!stop.load()) {
            Job* j = Take(next);
            if (j == nullptr)
                break;
            Run(*j);
            delete j;
        }
    }
    if (param != nullptr)
        FreeLibraryAndExitThread(static_cast<HMODULE>(param), 0);
    return 0;
}

// Starts the thread when none runs. False when there is none to take a job
// (it could not be made, or an earlier one is still stopping).
inline bool Start() {
    if (thread != nullptr) {
        if (WaitForSingleObject(thread, 0) == WAIT_TIMEOUT)
            return !stop.load();
        CloseHandle(thread);   // an earlier thread has ended
        thread = nullptr;
    }
    if (start_failed)
        return false;
    if (wake == nullptr)
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake == nullptr) {
        start_failed = true;
        diag::Log("handfit: no worker event (error %lu): hands stay plain", GetLastError());
        return false;
    }
    stop.store(false);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&start_failed), &self);
    thread = CreateThread(nullptr, 0, Main, self, CREATE_SUSPENDED, nullptr);
    if (thread == nullptr) {
        if (self != nullptr)
            FreeLibrary(self);
        start_failed = true;
        diag::Log("handfit: no worker thread (error %lu): hands stay plain", GetLastError());
        return false;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(thread);
    return true;
}

// Hands a job over (taking it), superseding the hand's earlier one. False when
// there is no worker; the job is then dropped.
inline bool Post(Job* j) {
    const int s = j->side;
    j->generation = generation[s].fetch_add(1) + 1;
    if (!Start()) {
        delete j;
        return false;
    }
    AcquireSRWLockExclusive(&lock);
    Job* old = pending[s];
    pending[s] = j;
    ReleaseSRWLockExclusive(&lock);
    delete old;
    SetEvent(wake);
    return true;
}

// The hand's newest finished fit, if it is for the newest request.
inline std::shared_ptr<const Result> Collect(int s) {
    std::shared_ptr<const Result> r;
    AcquireSRWLockExclusive(&lock);
    r.swap(done[s]);
    ReleaseSRWLockExclusive(&lock);
    if (r != nullptr && r->generation != generation[s].load())
        r.reset();
    return r;
}

// Drops the hand's waiting job and result and stops its running fit.
inline void Cancel(int s) {
    generation[s].fetch_add(1);
    std::shared_ptr<const Result> r;
    AcquireSRWLockExclusive(&lock);
    Job* j = pending[s];
    pending[s] = nullptr;
    r.swap(done[s]);
    ReleaseSRWLockExclusive(&lock);
    delete j;
}

// Stops the thread (device teardown). A running fit stops at its next step.
inline void Shutdown() {
    if (thread == nullptr)
        return;
    Cancel(0);
    Cancel(1);
    stop.store(true);
    SetEvent(wake);
    if (WaitForSingleObject(thread, SHUTDOWN_WAIT_MS) == WAIT_OBJECT_0) {
        CloseHandle(thread);
        thread = nullptr;
    } else {
        diag::Log("handfit: the worker did not stop within %lu ms; it ends on its own",
                  (unsigned long)SHUTDOWN_WAIT_MS);
    }
}

}  // namespace worker

using ResultPtr = std::shared_ptr<const worker::Result>;

// ------------------------------------------------------------------ the cache
namespace cache {
inline ResultPtr slot[CACHE_SIZE];
inline uint32_t used[CACHE_SIZE];
inline uint32_t tick = 0;

inline ResultPtr Find(const Key& k) {
    for (int i = 0; i < CACHE_SIZE; i++)
        if (slot[i] != nullptr && memcmp(&slot[i]->key, &k, sizeof(Key)) == 0) {
            used[i] = ++tick;
            return slot[i];
        }
    return nullptr;
}
// A key without the weapon's captured geometry: the same item in the same
// hand on the same seat with the same options. A snapshot of one weapon does
// not hash alike from one capture to the next, so this is what says a hold
// has been fitted before.
inline Key HoldOf(Key k) {
    k.weapon_hash = 0;
    k.weapon_draws = 0;
    return k;
}
inline bool SameHoldKey(const Key& a, const Key& b) {
    const Key x = HoldOf(a), y = HoldOf(b);
    return memcmp(&x, &y, sizeof(Key)) == 0;
}
// The fit kept for this hold, whatever snapshot it was made from.
inline ResultPtr FindHold(const Key& k) {
    for (int i = 0; i < CACHE_SIZE; i++)
        if (slot[i] != nullptr && SameHoldKey(slot[i]->key, k)) {
            used[i] = ++tick;
            return slot[i];
        }
    return nullptr;
}
// One fit is kept per hold: a new one takes the place of the hold's earlier
// fit, else an empty slot, else the least recently used.
inline void Put(const ResultPtr& r) {
    int at = -1;
    for (int i = 0; i < CACHE_SIZE && at < 0; i++)
        if (slot[i] != nullptr && SameHoldKey(slot[i]->key, r->key))
            at = i;
    if (at < 0) {
        at = 0;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (slot[i] == nullptr) {
                at = i;
                break;
            }
            if (used[i] < used[at])
                at = i;
        }
    }
    slot[at] = r;
    used[at] = ++tick;
}
inline void Clear() {
    for (int i = 0; i < CACHE_SIZE; i++) {
        slot[i].reset();
        used[i] = 0;
    }
}
inline int Count() {
    int n = 0;
    for (const ResultPtr& r : slot)
        n += r != nullptr ? 1 : 0;
    return n;
}
}  // namespace cache

// ------------------------------------------------------------------ per hand

struct HandState {
    bool have_want = false;   // a fit is asked for ...
    Key want = {};            // ... for this key
    bool waiting = false;     // ... and is with the worker
    ResultPtr target;         // the fit to show (null: the plain hand)
    ResultPtr shown;          // the fit whose pose is drawn now
    double deg[2] = {0, 0};   // the angles drawn now: the index block's, the outer block's
    double slide = 0;         // the thumb's slide drawn now
    bool active = false;      // a reshape is set on the hand
    const char* plain_why = "not started";
    // The role row's fingerprint test, once per captured mesh.
    bool fp_checked = false;
    uint32_t fp_generation = 0;
    int fp_class = -1;
    handfit::adapt::RoleMatch fp = handfit::adapt::RoleMatch::Ok;
    // Counts since hand_fit last took over.
    uint32_t requests = 0, fits = 0, cache_hits = 0, failures = 0;
    uint32_t hold_hits = 0;   // a kept fit shown before the weapon's snapshot came
    int pending_passes = 0;   // passes a held weapon has been without a snapshot
    double last_ms = 0;
    bool last_from_cache = false;
};
inline HandState hs[2];
inline bool holding = false;   // the module owns the reshape (and may hold state)
inline bool worker_missing_logged = false;
// The arrays handed to the substitution: they live as long as the DLL.
inline float moves[2][MAX_W][3];
inline float turns[2][MAX_W][9];
inline double pose_pos[MAX_W * 3];
inline double pose_turn[MAX_W * 9];

// A block's angle in a fit (0 the index block, 1 the outer), without its sign.
inline double TargetDeg(const ResultPtr& r, int which) {
    return r != nullptr && r->fit.fingers.ran && r->fit.blocks[which].present ? std::fabs(r->fit.blocks[which].deg) : 0.0;
}
inline double TargetSlide(const ResultPtr& r) {
    return r != nullptr && r->fit.thumb.present ? r->fit.thumb.search.value : 0.0;
}

// Two fits pose the hand alike: the same rest points, frame, pivots, opening
// ways and thumb shares, so their numbers can be eased into each other.
inline bool SamePose(const handfit::Prepared& a, const handfit::Prepared& b) {
    return a.ok == b.ok && a.mirrored == b.mirrored && a.count == b.count &&
           memcmp(&a.frame, &b.frame, sizeof(a.frame)) == 0 && memcmp(a.block_sign, b.block_sign, sizeof(a.block_sign)) == 0 &&
           memcmp(a.block_pivot, b.block_pivot, sizeof(a.block_pivot)) == 0 && a.rest == b.rest &&
           a.rest_local == b.rest_local && a.roles == b.roles && a.share == b.share;
}

inline void Approach(double& x, double to, double rate) {
    if (std::fabs(to - x) <= rate)
        x = to;
    else
        x += to > x ? rate : -rate;
}

// The plain hand, at once: the reshape dropped and the drawn fit forgotten.
inline void DropShown(int side) {
    HandState& h = hs[side];
    if (h.active)
        hands::IkSetReshape(side, nullptr, nullptr, 0);
    h.active = false;
    h.shown.reset();
    h.deg[0] = h.deg[1] = h.slide = 0;
}

// Nothing to fit: no request, and the hand eases back to plain.
inline void WantNothing(int side, const char* why) {
    HandState& h = hs[side];
    if (h.have_want || h.waiting)
        worker::Cancel(side);
    h.have_want = false;
    h.waiting = false;
    h.target.reset();
    h.plain_why = why;
}

// The handle line the seat lays the weapon on, as the fit's channel: the
// channel's point and the handle's +z turned by its tilt and yaw (the same
// turns the seat matrix makes). mirror: the left hand's, negated in x.
inline void ChannelLine(const eyeheight::ChannelSeat& c, bool mirror, double point[2], double dir[3]) {
    const float ct = cosf(c.tilt), st = sinf(c.tilt), cy = cosf(c.yaw), sy = sinf(c.yaw);
    const float d[3] = {ct * sy, -st, ct * cy};
    point[0] = mirror ? -(double)c.point[0] : (double)c.point[0];
    point[1] = (double)c.point[1];
    dir[0] = mirror ? -(double)d[0] : (double)d[0];
    dir[1] = (double)d[1];
    dir[2] = (double)d[2];
}

inline Key MakeKey(int side, const hands::IkWelded& hw, uint64_t mesh_hash, uint8_t kind, uint32_t item,
                   uint32_t weapon_hash, uint32_t weapon_draws, const handfit::adapt::Placement& at,
                   float weapon_scale, const handfit::Options& o) {
    Key k;
    memset(&k, 0, sizeof(k));
    k.side = (uint32_t)side;
    k.class_id = hw.class_id;
    k.kind = kind;
    k.roles_version = handfit::kRolesVersion;
    k.mesh_points = (uint32_t)hw.mesh->vertex_count;
    k.mesh_tris = (uint32_t)hw.mesh->tri_count;
    k.mesh_hash = mesh_hash;
    k.weapon_hash = weapon_hash;
    k.weapon_draws = weapon_draws;
    k.item = item;
    memcpy(k.weapon, at.weapon, sizeof(k.weapon));
    memcpy(k.seat, at.seat, sizeof(k.seat));
    k.channel[0] = at.channel_point[0];
    k.channel[1] = at.channel_point[1];
    k.channel[2] = at.channel_dir[0];
    k.channel[3] = at.channel_dir[1];
    k.channel[4] = at.channel_dir[2];
    k.world_scale = at.world_scale;
    k.weapon_scale = weapon_scale;
    const double opts[19] = {o.fit_weapon ? 1.0 : 0.0, o.follow_seat ? 1.0 : 0.0, o.keep_tol_cm, o.gap_cm,
                             o.sample_cm, o.block_max_deg, o.block_step_deg, o.block_judge_share,
                             (double)(int)o.block_unclear, o.thumb_max_cm, o.thumb_step_cm, o.thumb_share_from,
                             o.thumb_share_to, o.thumb_judge_share, (double)(int)o.thumb_unclear,
                             o.block_unclear_limit, o.thumb_unclear_limit, o.thumb_sink_cm,
                             o.block_each ? 1.0 : 0.0};
    memcpy(k.options, opts, sizeof(k.options));
    return k;
}

// The fingerprint test of the hand's mesh, logged once per captured mesh when
// the role table does not describe it.
inline handfit::adapt::RoleMatch CheckFingerprint(int side, const hands::IkWelded& hw) {
    HandState& h = hs[side];
    if (h.fp_checked && h.fp_generation == hw.generation && h.fp_class == hw.class_id)
        return h.fp;
    h.fp_checked = true;
    h.fp_generation = hw.generation;
    h.fp_class = hw.class_id;
    h.fp = handfit::adapt::MatchMesh(hw.class_id, side, *hw.mesh);
    if (h.fp != handfit::adapt::RoleMatch::Ok) {
        const handfit::RoleRow* row = handfit::Find(hw.class_id, side);
        const handweld::Mesh& m = *hw.mesh;
        if (row == nullptr)
            diag::Log("handfit: %s hand of class %d %s: no role row; the hand is left as the game draws it",
                      SideName(side), hw.class_id, weapongrip::ClassName(hw.class_id));
        else
            diag::Log("handfit: %s hand of class %d %s (mesh generation %u): %d welded points, %d triangles, "
                      "triangle hash 0x%08X, against the role row's %u / %u / 0x%08X; the hand is left as the "
                      "game draws it",
                      SideName(side), hw.class_id, weapongrip::ClassName(hw.class_id), (unsigned)hw.generation,
                      m.vertex_count, m.tri_count,
                      (unsigned)handfit::TriangleHash(&m.tri[0][0], (size_t)m.tri_count * 3),
                      (unsigned)row->point_count, (unsigned)row->triangle_count, (unsigned)row->triangle_hash);
    }
    return h.fp;
}

inline void LogLanded(int side, const worker::Result& r, bool from_cache) {
    const handfit::Fit& f = r.fit;
    char fingers[128], thumb[96];
    if (f.fingers.ran)
        snprintf(fingers, sizeof fingers, "fingers %.2f deg (index %.2f, outer %.2f; %s, %d inside, %d crossings)",
                 f.fingers.value, std::fabs(f.blocks[0].deg), std::fabs(f.blocks[1].deg),
                 f.fingers.clean ? "clean" : "unclear", f.fingers.bad, f.fingers.cuts);
    else
        snprintf(fingers, sizeof fingers, "fingers not searched");
    if (f.thumb.present)
        snprintf(thumb, sizeof thumb, "thumb %.2f cm (%s, %d inside, %d crossings; brought in %.2f)",
                 f.thumb.search.value, f.thumb.search.clean ? "clean" : "unclear", f.thumb.search.bad,
                 f.thumb.search.cuts, f.thumb.sink_cm);
    else
        snprintf(thumb, sizeof thumb, "no thumb");
    if (from_cache)
        diag::Log("handfit: %s hand, class %d, kind 0x%02X: %s, %s, from the cache", SideName(side),
                  r.key.class_id, (unsigned)r.key.kind, fingers, thumb);
    else
        diag::Log("handfit: %s hand, class %d, kind 0x%02X: %s, %s, %.1f ms on the worker (fit %u)",
                  SideName(side), r.key.class_id, (unsigned)r.key.kind, fingers, thumb, r.ms,
                  (unsigned)hs[side].fits);
}

// Asks for the fit of key (from the cache, or a job for the worker).
inline void Request(int side, const Key& key, const hands::IkWelded& hw, const weaponsnap::HeldGeometry& held,
                    const handfit::adapt::Placement& at, const handfit::Options& options) {
    HandState& h = hs[side];
    h.have_want = true;
    h.want = key;
    h.requests++;
    h.plain_why = nullptr;
    if (ResultPtr hit = cache::Find(key)) {
        worker::Cancel(side);
        h.waiting = false;
        h.target = hit;
        h.cache_hits++;
        h.last_from_cache = true;
        LogLanded(side, *hit, true);
        return;
    }
    auto in = std::make_shared<handfit::Input>();
    const handfit::adapt::RoleMatch m =
        handfit::adapt::MakeInput(hw.class_id, side, handfit::adapt::HandFromMesh(*hw.mesh),
                                  handfit::adapt::WeaponParts(held), at, options, *in);
    if (m != handfit::adapt::RoleMatch::Ok) {
        WantNothing(side, "the role table does not describe this hand");
        return;
    }
    worker::Job* j = new worker::Job();
    j->side = side;
    j->mirrored = side == 0;
    j->key = key;
    j->in = std::move(in);
    if (worker::Post(j)) {
        // The fit shown now stays until this one lands (for another weapon it
        // is the kept fit of this hold, or the last weapon's): the fingers do
        // not close to the plain fist in between.
        h.waiting = true;
    } else {
        h.waiting = false;
        h.target.reset();
        h.plain_why = "no worker thread";
        if (!worker_missing_logged) {
            worker_missing_logged = true;
            diag::Log("handfit: no worker to run the fit on: the hands stay plain");
        }
    }
}

// Why a held gun leaves the hand plain.
inline const char* GunPlainWhy(const weapongrip::HeldSeat& s) {
    switch (s.why) {
    case gunseat::Why::NoLine: return "a gun with no handle line: no gun seat";
    case gunseat::Why::NotThisHand: return "a gun, not drawn in this hand";
    case gunseat::Why::OffGrip: return "a gun held off the hand bone: no gun seat";
    case gunseat::Why::NoChannel: return "a gun, and no fist channel for its seat";
    case gunseat::Why::IkOff: return "a gun, and IK arms off";
    default: return "a gun (gun_seat 0: no gun seat for the hand)";
    }
}

// Passes a held weapon may be without a snapshot before the hand eases back
// to plain (a snapshot normally comes within a few passes of the weapon being
// drawn with its owner idle).
constexpr int PENDING_HOLD_PASSES = 240;

// A weapon is held but its shape is not captured yet (the passes after a
// change of weapon). The fit this item had the last time it was held this way
// is shown at once; with none kept, the fit shown now stays until the new one
// lands. Either way the fingers do not close to the plain fist in between.
// hold: the key without the snapshot's numbers.
inline void Pending(int side, const Key& hold) {
    HandState& h = hs[side];
    if (h.have_want || h.waiting)
        worker::Cancel(side);
    h.have_want = false;
    h.waiting = false;
    if (ResultPtr hit = cache::FindHold(hold)) {
        if (h.target != hit) {
            h.target = hit;
            h.hold_hits++;
            diag::Log("handfit: %s hand, class %d, kind 0x%02X item %06X: the fit kept from the last time it was "
                      "held, shown before the weapon's snapshot",
                      SideName(side), hold.class_id, (unsigned)hold.kind, (unsigned)hold.item);
        }
        h.plain_why = nullptr;
        return;
    }
    if (h.pending_passes < PENDING_HOLD_PASSES)
        h.pending_passes++;
    else
        h.target.reset();
    h.plain_why = h.target != nullptr ? nullptr : "the weapon's shape is not captured yet";
}

// One hand's pass: what to fit, what landed, the easing and the reshape.
// main_seat: the main hand's seat; the left hand takes its mirror. A gun is
// fitted on the hand's gun seat while the hand is moved onto it
// (weapongrip::GunSeatForHand), and otherwise left plain.
inline void StepHand(int side, const D3DMATRIX& main_seat, const eyeheight::ChannelSeat& channel) {
    HandState& h = hs[side];
    const hands::IkWelded hw = hands::Welded(side);
    if (!hw.valid || hw.mesh == nullptr || hw.mesh->vertex_count <= 0 || hw.mesh->vertex_count > MAX_W) {
        WantNothing(side, "no hand mesh");
        DropShown(side);   // the moves are per welded point of a mesh that is gone
        return;
    }
    const uint64_t mesh_hash = MeshHash(*hw.mesh);
    const std::shared_ptr<const weaponsnap::HeldGeometry> held = weaponsnap::Held(side == 1 ? 0 : 1);
    const uint8_t kind = weapongrip::current_kind;
    // This hand's part of the held weapon as captured, or a weapon whose
    // snapshot is still being taken (none published for it yet).
    const bool captured = held != nullptr && !held->draws.empty() && held->kind == kind;
    const bool pending = !captured && weapongrip::current_weapon != nullptr && weaponsnap::Collecting() &&
                         weaponsnap::Held(0) == nullptr;
    const bool gun = (captured || pending) && weapongrip::KindIsGun(kind);
    weapongrip::HeldSeat gun_seat;
    if (gun && vrmod::config.gun_seat == 1)
        gun_seat = weapongrip::GunSeatForHand(side, ikarms::Active());
    if (!pending)
        h.pending_passes = 0;
    if (weapongrip::weapons_hidden) {
        WantNothing(side, "weapons are not drawn on this floor");
    } else if (!captured && !pending) {
        WantNothing(side, "nothing held");
    } else if (side == 0 && pending && !weapongrip::MayDrawLeftHalf()) {
        // A weapon with no off-hand half: the left hand lets go at the change,
        // not when the weapon's snapshot comes.
        WantNothing(side, "the held weapon has no half for this hand");
    } else if (gun && !gun_seat.Moves()) {
        WantNothing(side, GunPlainWhy(gun_seat));
    } else if (CheckFingerprint(side, hw) != handfit::adapt::RoleMatch::Ok) {
        WantNothing(side, "the role table does not describe this hand");
    } else {
        const D3DMATRIX seat = gun ? gun_seat.seat : side == 0 ? weapongrip::MirroredSeat(main_seat) : main_seat;
        const float scale = vrmod::config.weapon_scale;
        handfit::adapt::Placement at;
        const float* m = &seat._11;
        for (int i = 0; i < 16; i++)
            at.seat[i] = (double)m[i];
        handfit::adapt::WeaponMatrix(m, scale, at.weapon);
        at.has_channel = true;
        ChannelLine(channel, side == 0, at.channel_point, at.channel_dir);
        at.world_scale = (double)vrmod::config.world_scale;
        const handfit::Options options = CurrentOptions();
        const uint32_t item = gunseat::ItemCode(weapongrip::current_category, weapongrip::current_variant);
        if (captured) {
            const Key key = MakeKey(side, hw, mesh_hash, kind, item, held->hash, (uint32_t)held->draws.size(), at,
                                    scale, options);
            if (!h.have_want || memcmp(&key, &h.want, sizeof(Key)) != 0)
                Request(side, key, hw, *held, at, options);
        } else {
            Pending(side, MakeKey(side, hw, mesh_hash, kind, item, 0, 0, at, scale, options));
        }
    }

    if (h.waiting) {
        if (ResultPtr r = worker::Collect(side)) {
            h.waiting = false;
            if (!h.have_want || memcmp(&r->key, &h.want, sizeof(Key)) != 0) {
                // not for the request the hand is on (cannot happen: every new
                // request raises the counter); dropped
            } else {
                h.fits++;
                h.last_ms = r->ms;
                h.last_from_cache = false;
                if (r->fit.ok) {
                    cache::Put(r);
                    h.target = r;
                    LogLanded(side, *r, false);
                } else {
                    h.failures++;
                    h.target.reset();
                    h.plain_why = "the fit failed";
                    diag::Log("handfit: %s hand: the fit failed (%s); the hand stays plain", SideName(side),
                              r->fit.error != nullptr ? r->fit.error : "no reason given");
                }
            }
        }
    }


    // The drawn fit belongs to the mesh the hand has now, or it goes at once.
    if (h.shown != nullptr && (h.shown->key.mesh_hash != mesh_hash ||
                               h.shown->key.mesh_points != (uint32_t)hw.mesh->vertex_count))
        DropShown(side);
    // Ease: toward the target's numbers when the two fits pose alike, else
    // back to plain first and then over to the target.
    if (h.shown != nullptr && h.target != nullptr &&
        (h.shown == h.target || SamePose(h.shown->fit.pose, h.target->fit.pose))) {
        h.shown = h.target;
        for (int b = 0; b < 2; b++)
            Approach(h.deg[b], TargetDeg(h.target, b), EASE_DEG_PER_PASS);
        Approach(h.slide, TargetSlide(h.target), EASE_CM_PER_PASS);
    } else if (h.shown != nullptr) {
        for (int b = 0; b < 2; b++)
            Approach(h.deg[b], 0.0, EASE_DEG_PER_PASS);
        Approach(h.slide, 0.0, EASE_CM_PER_PASS);
        if (h.deg[0] == 0.0 && h.deg[1] == 0.0 && h.slide == 0.0)
            h.shown = h.target;   // plain now; the target (if any) eases in from the next pass
    } else if (h.target != nullptr) {
        h.shown = h.target;
        h.deg[0] = h.deg[1] = h.slide = 0;
        for (int b = 0; b < 2; b++)
            Approach(h.deg[b], TargetDeg(h.target, b), EASE_DEG_PER_PASS);
        Approach(h.slide, TargetSlide(h.target), EASE_CM_PER_PASS);
    }

    // At zero and zero the pose is the rest hand bit for bit: nothing to set.
    if (h.shown == nullptr || (h.deg[0] == 0.0 && h.deg[1] == 0.0 && h.slide == 0.0)) {
        if (h.active)
            hands::IkSetReshape(side, nullptr, nullptr, 0);
        h.active = false;
        return;
    }
    const handfit::Fit& f = h.shown->fit;
    const std::vector<double>& rest = h.shown->in->points;   // the hand's own frame, as the mesh holds it
    const int n = f.pose.count;
    if (n != hw.mesh->vertex_count || rest.size() != 3 * (size_t)n) {
        DropShown(side);
        return;
    }
    handfit::PoseBlocks(f.pose, h.deg, h.slide, pose_pos, pose_turn);
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < 3; c++)
            moves[side][i][c] = (float)(pose_pos[3 * i + c] - rest[3 * (size_t)i + c]);
        for (int k = 0; k < 9; k++)
            turns[side][i][k] = (float)pose_turn[9 * i + k];
    }
    hands::IkSetReshape(side, moves[side], nullptr, hw.generation, turns[side]);
    h.active = true;
}

// The plain hands, every request dropped and the cache emptied: what the
// module leaves behind when it hands the reshape back.
inline void ClearAll() {
    for (int side = 0; side < 2; side++) {
        worker::Cancel(side);
        if (hs[side].active)
            hands::IkSetReshape(side, nullptr, nullptr, 0);
        hs[side] = HandState();
    }
    cache::Clear();
    hands::ik_reshape_enabled = false;
    holding = false;
}

// hand_fit on and IK arms active: the fit owns the hands' reshape.
inline bool Owns() { return vrmod::config.hand_fit != 0 && ikarms::Active(); }

// Once per pass, after the weapon module, the hands' capture state and the
// IK arms.
inline void Step() {
    if (!Owns()) {
        if (holding) {
            ClearAll();
            diag::Log("handfit: off (hand_fit %d, IK arms %s): the reshape goes back to the fist closing",
                      vrmod::config.hand_fit, ikarms::Active() ? "on" : "off");
        }
        closefist::Step();
        return;
    }
    if (!holding) {
        // Take over: the closing's reshapes and state are dropped first.
        closefist::ClearAll();
        holding = true;
        diag::Log("handfit: on: the hand is fitted to the held weapon in place of the fist closing "
                  "(fingers %s, thumb %s, when nothing clears)",
                  UnclearText(vrmod::config.hand_fit_fingers_unclear, vrmod::config.hand_fit_fingers_unclear_limit).s,
                  UnclearText(vrmod::config.hand_fit_thumb_unclear, vrmod::config.hand_fit_thumb_unclear_limit).s);
    }
    hands::ik_reshape_enabled = true;
    const D3DMATRIX seat = weapongrip::MeleeSlide();
    const eyeheight::ChannelSeat channel = weapongrip::MeleeCurrentChannel();
    for (int side = 0; side < 2; side++)
        StepHand(side, seat, channel);
}

// Device teardown: the worker stopped and, if the module held the hands, the
// hands left plain. Nothing happens when it never ran.
inline void Shutdown() {
    worker::Shutdown();
    if (holding)
        ClearAll();
}


}  // namespace fithand
