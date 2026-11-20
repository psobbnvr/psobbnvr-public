#pragma once

// The hand fitted to what it holds, keeping the game's own shape: the two
// finger blocks (the index finger; the three outer fingers as one piece) turn
// open about their knuckle lines by one shared angle, and the thumb slides
// forward out of the palm, each the least that leaves its faces clear of the
// held weapon. Every other point of the hand stays where the game put it.
//
// Pure maths: standard library only, double precision, no globals, no
// logging, no Direct3D types. Solve is re-entrant (all state lives in the
// call), so it can run on a worker thread while the game renders.
//
// Frames and units:
// - The grip frame: the hand's grip bone, game units; +x out of the palm, +y
//   toward the wrist, +z the thumb side. Hand points, the channel line and the
//   fitted points are in it.
// - Matrices: 16 numbers, row-vector convention (p' = p x M), translation in
//   elements 12-14.
// - The channel frame (the fit's own, in cm): f out of the palm (+x made square
//   to the handle line), h toward the wrist, s along the handle line. Pivots,
//   axes and the thumb's slide are in it. cm per unit = 1 / (0.01 * world
//   scale).
//
// The arithmetic is fixed so the same inputs give the same angle, slide and
// counts bit for bit: sums in a set order, the angle and the slide made by
// repeated addition, and fdlibm's sin, cos and atan2 below in place of the
// runtime's.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "psobbvr_handfit_roles.hpp"

namespace handfit {

// ------------------------------------------------------------------ fdlibm
// sin, cos and atan2 that give the same bits everywhere (the MSVC runtime's
// differ in the last bit on a few percent of arguments, enough to move a test
// point across the weapon's surface). Adapted from fdlibm:
//
//   Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//   Developed at SunPro, a Sun Microsystems, Inc. business.
//   Permission to use, copy, modify, and distribute this software is freely
//   granted, provided that this notice is preserved.
//
// Arguments beyond 2^19 * pi / 2 (never met here) fall back to the runtime.
namespace fdlibm {

inline int32_t HiWord(double x) {
    uint64_t u;
    std::memcpy(&u, &x, 8);
    return (int32_t)(u >> 32);
}
inline uint32_t LoWord(double x) {
    uint64_t u;
    std::memcpy(&u, &x, 8);
    return (uint32_t)u;
}
inline double FromWords(uint32_t hi, uint32_t lo) {
    const uint64_t u = ((uint64_t)hi << 32) | lo;
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

inline double KernelSin(double x, double y, int iy) {
    const double half = 5.00000000000000000000e-01, S1 = -1.66666666666666324348e-01,
                 S2 = 8.33333333332248946124e-03, S3 = -1.98412698298579493134e-04,
                 S4 = 2.75573137070700676789e-06, S5 = -2.50507602534068634195e-08,
                 S6 = 1.58969099521155010221e-10;
    const int32_t ix = HiWord(x) & 0x7fffffff;
    if (ix < 0x3e400000 && (int)x == 0)
        return x;
    const double z = x * x;
    const double v = z * x;
    const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0)
        return x + v * (S1 + z * r);
    return x - ((z * (half * y - v * r) - y) - v * S1);
}

inline double KernelCos(double x, double y) {
    const double one = 1.0, C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                 C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                 C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    const int32_t ix = HiWord(x) & 0x7fffffff;
    if (ix < 0x3e400000 && (int)x == 0)
        return one;
    const double z = x * x;
    const double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ix < 0x3fd33333)
        return one - (0.5 * z - (z * r - x * y));
    const double qx = ix > 0x3fe90000 ? 0.28125 : FromWords((uint32_t)(ix - 0x00200000), 0);
    const double hz = 0.5 * z - qx;
    const double a = one - qx;
    return a - (hz - (z * r - x * y));
}

constexpr int kRemOutOfRange = 0x40000000;

// x reduced by a multiple n of pi / 2 into y[0] + y[1]; returns n.
inline int RemPio2(double x, double* y) {
    const double half = 0.5, invpio2 = 6.36619772367581382433e-01, pio2_1 = 1.57079632673412561417e+00,
                 pio2_1t = 6.07710050650619224932e-11, pio2_2 = 6.07710050630396597660e-11,
                 pio2_2t = 2.02226624879595063154e-21, pio2_3 = 2.02226624871116645580e-21,
                 pio2_3t = 8.47842766036889956997e-32;
    const int32_t hx = HiWord(x);
    const int32_t ix = hx & 0x7fffffff;
    if (ix <= 0x3fe921fb) {
        y[0] = x;
        y[1] = 0;
        return 0;
    }
    if (ix < 0x4002d97c) {
        double z;
        if (hx > 0) {
            z = x - pio2_1;
            if (ix != 0x3ff921fb) {
                y[0] = z - pio2_1t;
                y[1] = (z - y[0]) - pio2_1t;
            } else {
                z -= pio2_2;
                y[0] = z - pio2_2t;
                y[1] = (z - y[0]) - pio2_2t;
            }
            return 1;
        }
        z = x + pio2_1;
        if (ix != 0x3ff921fb) {
            y[0] = z + pio2_1t;
            y[1] = (z - y[0]) + pio2_1t;
        } else {
            z += pio2_2;
            y[0] = z + pio2_2t;
            y[1] = (z - y[0]) + pio2_2t;
        }
        return -1;
    }
    if (ix > 0x413921fb)
        return kRemOutOfRange;
    // Medium arguments. fdlibm skips the exponent check below for most n (a
    // quick path); the check alone gives the same result, as the refinement
    // only triggers where the quick path is not taken.
    double t = std::fabs(x);
    const int n = (int)(t * invpio2 + half);
    const double fn = (double)n;
    double r = t - fn * pio2_1;
    double w = fn * pio2_1t;
    const int32_t j = ix >> 20;
    y[0] = r - w;
    int32_t i = j - ((HiWord(y[0]) >> 20) & 0x7ff);
    if (i > 16) {
        t = r;
        w = fn * pio2_2;
        r = t - w;
        w = fn * pio2_2t - ((t - r) - w);
        y[0] = r - w;
        i = j - ((HiWord(y[0]) >> 20) & 0x7ff);
        if (i > 49) {
            t = r;
            w = fn * pio2_3;
            r = t - w;
            w = fn * pio2_3t - ((t - r) - w);
            y[0] = r - w;
        }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) {
        y[0] = -y[0];
        y[1] = -y[1];
        return -n;
    }
    return n;
}

inline double Sin(double x) {
    const int32_t ix = HiWord(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb)
        return KernelSin(x, 0.0, 0);
    if (ix >= 0x7ff00000)
        return x - x;
    double y[2];
    const int n = RemPio2(x, y);
    if (n == kRemOutOfRange)
        return std::sin(x);
    switch (n & 3) {
    case 0: return KernelSin(y[0], y[1], 1);
    case 1: return KernelCos(y[0], y[1]);
    case 2: return -KernelSin(y[0], y[1], 1);
    default: return -KernelCos(y[0], y[1]);
    }
}

inline double Cos(double x) {
    const int32_t ix = HiWord(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb)
        return KernelCos(x, 0.0);
    if (ix >= 0x7ff00000)
        return x - x;
    double y[2];
    const int n = RemPio2(x, y);
    if (n == kRemOutOfRange)
        return std::cos(x);
    switch (n & 3) {
    case 0: return KernelCos(y[0], y[1]);
    case 1: return -KernelSin(y[0], y[1], 1);
    case 2: return -KernelCos(y[0], y[1]);
    default: return KernelSin(y[0], y[1], 1);
    }
}

inline double Atan(double x) {
    static const double atanhi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01,
                                    9.82793723247329054082e-01, 1.57079632679489655800e+00};
    static const double atanlo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17,
                                    1.39033110312309984516e-17, 6.12323399573676603587e-17};
    static const double aT[] = {3.33333333333329318027e-01, -1.99999999998764832476e-01,
                                1.42857142725034663711e-01, -1.11111104054623557880e-01,
                                9.09088713343650656196e-02, -7.69187620504482999495e-02,
                                6.66107313738753120669e-02, -5.83357013379057348645e-02,
                                4.97687799461593236017e-02, -3.65315727442169155270e-02,
                                1.62858201153657823623e-02};
    const double one = 1.0;
    const int32_t hx = HiWord(x);
    const int32_t ix = hx & 0x7fffffff;
    int id;
    if (ix >= 0x44100000) {
        if (ix > 0x7ff00000 || (ix == 0x7ff00000 && LoWord(x) != 0))
            return x + x;
        return hx > 0 ? atanhi[3] + atanlo[3] : -atanhi[3] - atanlo[3];
    }
    if (ix < 0x3fdc0000) {
        if (ix < 0x3e400000)
            return x;
        id = -1;
    } else {
        x = std::fabs(x);
        if (ix < 0x3ff30000) {
            if (ix < 0x3fe60000) {
                id = 0;
                x = (2.0 * x - one) / (2.0 + x);
            } else {
                id = 1;
                x = (x - one) / (x + one);
            }
        } else if (ix < 0x40038000) {
            id = 2;
            x = (x - 1.5) / (one + 1.5 * x);
        } else {
            id = 3;
            x = -1.0 / x;
        }
    }
    double z = x * x;
    const double w = z * z;
    const double s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    const double s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0)
        return x - x * (s1 + s2);
    z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return hx < 0 ? -z : z;
}

inline double Atan2(double y, double x) {
    const double tiny = 1.0e-300, pi_o_4 = 7.8539816339744827900E-01, pi_o_2 = 1.5707963267948965580E+00,
                 pi = 3.1415926535897931160E+00, pi_lo = 1.2246467991473531772E-16;
    if (std::isnan(x) || std::isnan(y))
        return x + y;
    const int32_t hx = HiWord(x), ix = hx & 0x7fffffff;
    const uint32_t lx = LoWord(x);
    const int32_t hy = HiWord(y), iy = hy & 0x7fffffff;
    const uint32_t ly = LoWord(y);
    if (hx == 0x3ff00000 && lx == 0)
        return Atan(y);
    int m = ((hy >> 31) & 1) | ((hx >> 30) & 2);   // 2 * sign(x) + sign(y)
    if (iy == 0 && ly == 0) {
        switch (m) {
        case 0:
        case 1: return y;
        case 2: return pi + tiny;
        default: return -pi - tiny;
        }
    }
    if (ix == 0 && lx == 0)
        return hy < 0 ? -pi_o_2 - tiny : pi_o_2 + tiny;
    if (ix == 0x7ff00000) {
        if (iy == 0x7ff00000) {
            switch (m) {
            case 0: return pi_o_4 + tiny;
            case 1: return -pi_o_4 - tiny;
            case 2: return 3.0 * pi_o_4 + tiny;
            default: return -3.0 * pi_o_4 - tiny;
            }
        }
        switch (m) {
        case 0: return 0.0;
        case 1: return -0.0;
        case 2: return pi + tiny;
        default: return -pi - tiny;
        }
    }
    if (iy == 0x7ff00000)
        return hy < 0 ? -pi_o_2 - tiny : pi_o_2 + tiny;
    const int32_t k = (iy - ix) >> 20;
    double z;
    if (k > 60) {
        z = pi_o_2 + 0.5 * pi_lo;
        m &= 1;
    } else if (hx < 0 && k < -60) {
        z = 0.0;
    } else {
        z = Atan(std::fabs(y / x));
    }
    switch (m) {
    case 0: return z;
    case 1: return -z;
    case 2: return pi - (z - pi_lo);
    default: return (z - pi_lo) - pi;
    }
}

}  // namespace fdlibm

// ------------------------------------------------------------------ inputs

// Where no angle (no slide) leaves the faces clean: the least bad one, or
// none (the game's own fingers / thumb).
enum class Unclear : uint8_t { Least = 0, Rest = 1 };

// The fit's settings; the unclear rules in particular are the caller's choice.
struct Options {
    bool fit_weapon = true;            // false: hold nothing (fingers as the game has them)
    bool follow_seat = false;          // true: the frame from the seat matrix, not the channel line
    double keep_tol_cm = 0.5;          // the tolerance the roles were made with (not read here)
    double gap_cm = 0.15;              // the gap fingers and thumb keep to the weapon
    double sample_cm = 0.4;            // spacing of the test points laid over each face
    double block_max_deg = 25;         // the fingers' angle is tried from 0 to this ...
    double block_step_deg = 1;         // ... in these steps
    double block_judge_share = 0.85;   // a finger face's test point counts where it takes this share of the turn
    Unclear block_unclear = Unclear::Rest;
    double block_unclear_limit = 1;    // Least: the least bad angle only among angles up to this share of block_max_deg
    bool block_each = false;           // true: after the shared angle, each block closes back to its own least angle
    double thumb_max_cm = 3;         // the thumb's slide is tried from 0 to this ...
    double thumb_step_cm = 0.15;       // ... in these steps
    double thumb_share_from = 0.3;     // a thumb point this far along the thumb stays put ...
    double thumb_share_to = 0.7;       // ... one this far along takes the whole slide (smoothstep between)
    double thumb_judge_share = 0.6;    // a thumb face's test point counts where it takes this share of the slide
    Unclear thumb_unclear = Unclear::Least;
    double thumb_unclear_limit = 1;    // Least: the least bad slide only among slides up to this share of thumb_max_cm
    double thumb_sink_cm = 0;          // a thumb whose slide clears the weapon is brought back in by this much, not past slide 0
};

// One draw: points (x y z) and triangles (three point indices).
struct Part {
    std::vector<double> positions;
    std::vector<uint32_t> tris;
};

struct Input {
    std::vector<double> points;      // welded hand, grip frame, 3 per point
    std::vector<uint32_t> tris;      // welded triangles, 3 indices each
    std::vector<Role> roles;         // one per point
    bool has_channel = false;        // the handle line below is given
    double channel_point[2] = {0, 0};      // where it crosses the grip frame's z = 0 plane (x, y)
    double channel_dir[3] = {0, 0, 1};     // its direction (need not be unit)
    double seat_matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // for the frame without a channel line
    std::vector<Part> weapon;        // the weapon's draws in model space (none: nothing held)
    double weapon_matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // places them in the grip frame
    double world_scale = 9;          // game units per metre (0.01 * world_scale units per cm)
    Options options;
    bool trace = false;              // fill Fit::finger_trace and thumb_trace
    // Called after each search step with the number of steps done (tests).
    void (*step_hook)(void* user, int steps_done) = nullptr;
    void* step_user = nullptr;
};

// ------------------------------------------------------------------ results

struct Frame {
    double origin[3] = {0, 0, 0};
    double ef[3] = {1, 0, 0}, eh[3] = {0, 1, 0}, es[3] = {0, 0, 1};
    double cm_per_unit = 1;
};

// One search's answer: the step value (degrees or cm, as stepped: 0, then the
// step added repeatedly), the points inside at it, the crossings beyond the
// first step's, and whether that step was clean (false: the unclear rule chose).
struct Search {
    bool ran = false;
    double value = 0;
    int bad = 0, cuts = 0;
    bool clean = false;
};

struct TraceStep {
    double step = 0;
    int cuts = 0;
    int inside = -1;   // -1: not tested (the step had crossings), else 0 / 1
    int bad = -1;      // second pass's count, -1: not counted
    int cap = -1;      // the cap that count stopped at, -1: none
};
struct Trace {
    bool ran = false;
    int cuts0 = 0;     // the first step's own crossings
    std::vector<TraceStep> steps;
};

struct BlockFit {
    bool present = false;
    int sign = 0;               // +1 / -1 opening way; 0 when nothing is held
    double deg = 0;             // sign x the fingers' angle
    double pivot[3] = {0, 0, 0};  // knuckle point, channel frame cm (mean of the kept points joined to the block)
    int tip = -1;               // the block's point with the largest f on the game's hand
};

struct ThumbFit {
    bool present = false;
    Search search;              // value: the slide, cm (after the sink; the counts are the search's, before it)
    double sink_cm = 0;         // how far a clear thumb was brought back in
    double pivot[3] = {0, 0, 0};  // channel frame cm
    double axis[3] = {0, 0, 0};   // unit, pivot to tip
    int tip = -1;
    double reach = 0;           // cm
};

// Counts of the inside test, for diagnosing last-bit differences: tests
// made, tests the bounded sum could not settle (the full
// winding sum decided), and of those, the ones within 1e-9 of the 0.5
// threshold, where a last-bit difference could flip the answer.
struct Stats {
    long long inside_tests = 0;
    long long full_sums = 0;
    long long knife_edges = 0;
};

// Everything Pose needs: the frame, the rest points, each block's pivot and
// opening way, each thumb point's share of the slide.
struct Prepared {
    bool ok = false;
    bool mirrored = false;               // results go back through the x mirror
    Frame frame;
    int count = 0;
    std::vector<double> rest;            // the hand's points as solved (grip frame; the mirrored problem's when mirrored)
    std::vector<double> rest_local;      // the same in the channel frame (cm)
    std::vector<Role> roles;
    int block_sign[2] = {0, 0};          // index, outer; 0: does not turn
    double block_pivot[2][3] = {{0, 0, 0}, {0, 0, 0}};
    std::vector<double> share;           // per point: its share of the thumb's slide
};

struct Fit {
    bool ok = false;
    const char* error = nullptr;
    bool superseded = false;   // stopped early: the generation changed
    bool held = false;         // a weapon was fitted to
    Frame frame;
    Search fingers;            // value: the shared angle, degrees, unsigned
    BlockFit blocks[2];        // index, outer
    ThumbFit thumb;
    std::vector<double> points;  // every fitted point, grip frame
    Prepared pose;
    Trace finger_trace, thumb_trace;
    Stats stats;
    int steps = 0;             // search steps evaluated
};

// ------------------------------------------------------------------ internals
namespace detail {

constexpr double kPi = 3.141592653589793;

struct V3 {
    double v[3];
    double operator[](int i) const { return v[i]; }
    double& operator[](int i) { return v[i]; }
};
inline V3 Sub(const V3& a, const V3& b) { return {{a[0] - b[0], a[1] - b[1], a[2] - b[2]}}; }
inline V3 Add(const V3& a, const V3& b) { return {{a[0] + b[0], a[1] + b[1], a[2] + b[2]}}; }
inline V3 Scl(const V3& a, double s) { return {{a[0] * s, a[1] * s, a[2] * s}}; }
inline double Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline V3 Cross(const V3& a, const V3& b) {
    return {{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}};
}
// hypot of two values the way Hypot3 computes three.
inline double Hypot2(double x, double y) {
    const double a[2] = {std::fabs(x), std::fabs(y)};
    const double m = a[0] > a[1] ? a[0] : a[1];
    if (m == 0.0)
        return 0.0;
    double sum = 0.0, compensation = 0.0;
    for (double v : a) {
        const double n = v / m;
        const double summand = n * n - compensation;
        const double preliminary = sum + summand;
        compensation = (preliminary - sum) - summand;
        sum = preliminary;
    }
    return std::sqrt(sum) * m;
}
inline double Len(const V3& a) { return Hypot3(a[0], a[1], a[2]); }
inline V3 Unit(const V3& a) {
    const double l = Len(a);
    return l > 0 ? V3{{a[0] / l, a[1] / l, a[2] / l}} : V3{{0, 0, 0}};
}
// min / max with -0 below +0
inline double Min(double a, double b) {
    if (a < b) return a;
    if (b < a) return b;
    return std::signbit(a) ? a : b;
}
inline double Max(double a, double b) {
    if (a > b) return a;
    if (b > a) return b;
    return std::signbit(a) ? b : a;
}
inline double Min3(double a, double b, double c) { return Min(Min(a, b), c); }
inline double Max3(double a, double b, double c) { return Max(Max(a, b), c); }

inline V3 ToLocal(const Frame& F, const V3& p) {
    const V3 o{{F.origin[0], F.origin[1], F.origin[2]}};
    const V3 ef{{F.ef[0], F.ef[1], F.ef[2]}}, eh{{F.eh[0], F.eh[1], F.eh[2]}}, es{{F.es[0], F.es[1], F.es[2]}};
    const V3 d = Sub(p, o);
    return {{Dot(d, ef) * F.cm_per_unit, Dot(d, eh) * F.cm_per_unit, Dot(d, es) * F.cm_per_unit}};
}
inline V3 ToGrip(const Frame& F, const V3& q) {
    const double inv = 1 / F.cm_per_unit;
    V3 g;
    for (int i = 0; i < 3; i++)
        g[i] = F.origin[i] + ((F.ef[i] * q[0] + F.eh[i] * q[1]) + F.es[i] * q[2]) * inv;
    return g;
}

inline Frame MakeFrame(const Input& in) {
    Frame F;
    V3 dir, origin;
    if (in.options.follow_seat || !in.has_channel) {
        const double* M = in.seat_matrix;
        const double l = Hypot3(M[8], M[9], M[10]);
        dir = {{M[8] / l, M[9] / l, M[10] / l}};
        origin = {{M[12], M[13], M[14]}};
    } else {
        dir = Unit(V3{{in.channel_dir[0], in.channel_dir[1], in.channel_dir[2]}});
        origin = {{in.channel_point[0], in.channel_point[1], 0}};
    }
    const V3 ef = Unit(V3{{1 - dir[0] * dir[0], 0.0 - dir[1] * dir[0], 0.0 - dir[2] * dir[0]}});
    const V3 eh = Cross(dir, ef);
    const double ws = (in.world_scale == 0 || std::isnan(in.world_scale)) ? 9 : in.world_scale;
    for (int i = 0; i < 3; i++) {
        F.origin[i] = origin[i];
        F.ef[i] = ef[i];
        F.eh[i] = eh[i];
        F.es[i] = dir[i];
    }
    F.cm_per_unit = 1 / (0.01 * ws);
    return F;
}

// ---- the weapon: one surface per draw, its triangles in runs of two with a
// ball and an area bound per run, so far runs are bounded, not summed
constexpr int kGroupTris = 2;
struct Group {
    int t0, t1;
    double c[3];
    double r, area;
};
struct Surface {
    int n = 0;
    std::vector<double> a, ab, ac;
    double lo[3], hi[3];
    std::vector<Group> groups;
};

inline Surface MakeSurface(const std::vector<double>& pos, const std::vector<uint32_t>& tris) {
    Surface s;
    const size_t n = tris.size() / 3;
    s.a.resize(n * 3);
    s.ab.resize(n * 3);
    s.ac.resize(n * 3);
    int used = 0;
    for (size_t t = 0; t < n; t++) {
        const size_t i0 = 3 * (size_t)tris[3 * t], i1 = 3 * (size_t)tris[3 * t + 1], i2 = 3 * (size_t)tris[3 * t + 2];
        const double e1[3] = {pos[i1] - pos[i0], pos[i1 + 1] - pos[i0 + 1], pos[i1 + 2] - pos[i0 + 2]};
        const double e2[3] = {pos[i2] - pos[i0], pos[i2 + 1] - pos[i0 + 1], pos[i2 + 2] - pos[i0 + 2]};
        const double l = Hypot3(e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]);
        if (!(l > 1e-12))
            continue;
        const size_t o = 3 * (size_t)used++;
        for (int c = 0; c < 3; c++) {
            s.a[o + c] = pos[i0 + c];
            s.ab[o + c] = e1[c];
            s.ac[o + c] = e2[c];
        }
    }
    s.n = used;
    s.a.resize((size_t)used * 3);
    s.ab.resize((size_t)used * 3);
    s.ac.resize((size_t)used * 3);
    const double inf = std::numeric_limits<double>::infinity();
    for (int c = 0; c < 3; c++) {
        s.lo[c] = inf;
        s.hi[c] = -inf;
    }
    for (int t = 0; t < used; t++)
        for (int c = 0; c < 3; c++) {
            const double p0 = s.a[3 * t + c], p1 = p0 + s.ab[3 * t + c], p2 = p0 + s.ac[3 * t + c];
            s.lo[c] = Min(Min(Min(s.lo[c], p0), p1), p2);
            s.hi[c] = Max(Max(Max(s.hi[c], p0), p1), p2);
        }
    for (int t0 = 0; t0 < used; t0 += kGroupTris) {
        const int t1 = used < t0 + kGroupTris ? used : t0 + kGroupTris;
        V3 pts[3 * kGroupTris];
        int np = 0;
        double area = 0;
        for (int t = t0; t < t1; t++) {
            const size_t i = 3 * (size_t)t;
            const V3 a{{s.a[i], s.a[i + 1], s.a[i + 2]}}, ab{{s.ab[i], s.ab[i + 1], s.ab[i + 2]}},
                ac{{s.ac[i], s.ac[i + 1], s.ac[i + 2]}};
            pts[np++] = a;
            pts[np++] = Add(a, ab);
            pts[np++] = Add(a, ac);
            area += Len(Cross(ab, ac)) / 2;
        }
        V3 lo{{inf, inf, inf}}, hi{{-inf, -inf, -inf}};
        for (int k = 0; k < 3; k++)
            for (int q = 0; q < np; q++) {
                lo[k] = Min(lo[k], pts[q][k]);
                hi[k] = Max(hi[k], pts[q][k]);
            }
        const V3 c = Scl(Add(lo, hi), 0.5);
        double r = 0;
        for (int q = 0; q < np; q++)
            r = Max(r, Len(Sub(pts[q], c)));
        Group g;
        g.t0 = t0;
        g.t1 = t1;
        g.c[0] = c[0];
        g.c[1] = c[1];
        g.c[2] = c[2];
        g.r = r * (1 + 1e-9) + 1e-12;
        g.area = area * (1 + 1e-9) + 1e-12;
        s.groups.push_back(g);
    }
    return s;
}

// Solid angle of triangle t (Van Oosterom and Strackee), times two.
inline double TwiceSolidAngle(const Surface& s, int t, const double g[3]) {
    const size_t i = 3 * (size_t)t;
    const double ax = s.a[i] - g[0], ay = s.a[i + 1] - g[1], az = s.a[i + 2] - g[2];
    const double bx = ax + s.ab[i], by = ay + s.ab[i + 1], bz = az + s.ab[i + 2];
    const double cx = ax + s.ac[i], cy = ay + s.ac[i + 1], cz = az + s.ac[i + 2];
    const double la = Hypot3(ax, ay, az), lb = Hypot3(bx, by, bz), lc = Hypot3(cx, cy, cz);
    const double det = ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx);
    const double den = la * lb * lc + (ax * bx + ay * by + az * bz) * lc + (ax * cx + ay * cy + az * cz) * lb +
                       (bx * cx + by * cy + bz * cz) * la;
    return 2 * fdlibm::Atan2(det, den);
}

// The winding number of a surface around g: +-1 inside a closed surface, 0
// outside, falling off smoothly for an open one.
inline double WindingNumber(const Surface& s, const double g[3]) {
    double total = 0;
    for (int t = 0; t < s.n; t++)
        total += TwiceSolidAngle(s, t, g);
    return total / (4 * kPi);
}

// |winding number| > 0.5, mostly without the full sum: a run of triangles
// whose ball is further away than its radius adds at most area / distance^2
// of solid angle, so only near runs are summed and far ones bounded; where
// the bound cannot settle the answer, the full sum decides.
inline bool WindingOverHalf(const Surface& s, const double g[3], Stats& st) {
    // (near_sum / far_bound, not near / far: windows.h defines those two
    // words as empty macros)
    double near_sum = 0, far_bound = 0;
    for (const Group& k : s.groups) {
        const double gap = Hypot3(g[0] - k.c[0], g[1] - k.c[1], g[2] - k.c[2]) - k.r;
        if (gap > k.r) {
            far_bound += k.area / (gap * gap);
            continue;
        }
        for (int t = k.t0; t < k.t1; t++)
            near_sum += TwiceSolidAngle(s, t, g);
    }
    const double w = std::fabs(near_sum) / (4 * kPi), wb = far_bound / (4 * kPi);
    if (w - wb > 0.5 + 1e-9)
        return true;
    if (w + wb < 0.5 - 1e-9)
        return false;
    st.full_sums++;
    const double full = std::fabs(WindingNumber(s, g));
    if (std::fabs(full - 0.5) < 1e-9)
        st.knife_edges++;
    return full > 0.5;
}

// ---- faces through faces

// Does segment pq pass through the inside of triangle abc (away from its
// edges and the segment's ends by eps)?
inline bool SegHitsTri(const V3& p, const V3& q, const V3& a, const V3& b, const V3& c, double eps = 1e-7) {
    const V3 d = Sub(q, p), e1 = Sub(b, a), e2 = Sub(c, a);
    const V3 h = Cross(d, e2);
    const double det = Dot(e1, h);
    if (std::fabs(det) < 1e-12)
        return false;
    const double f = 1 / det;
    const V3 s = Sub(p, a);
    const double u = f * Dot(s, h);
    if (u <= eps || u >= 1 - eps)
        return false;
    const V3 qv = Cross(s, e1);
    const double v = f * Dot(d, qv);
    if (v <= eps || u + v >= 1 - eps)
        return false;
    const double t = f * Dot(e2, qv);
    return t > eps && t < 1 - eps;
}

// Do two triangles cut through each other? No shared point: any edge of one
// through the other; one shared point: the edges opposite it; a shared edge:
// when folded flat onto each other (normals over 175 degrees apart).
inline bool TrisCross(const std::vector<V3>& P, const uint32_t* A, const uint32_t* B, double cos175) {
    uint32_t shared[3];
    int ns = 0;
    for (int i = 0; i < 3; i++)
        if (A[i] == B[0] || A[i] == B[1] || A[i] == B[2])
            shared[ns++] = A[i];
    if (ns >= 3)
        return false;
    if (ns == 2) {
        const V3 na = Cross(Sub(P[A[1]], P[A[0]]), Sub(P[A[2]], P[A[0]]));
        const V3 nb = Cross(Sub(P[B[1]], P[B[0]]), Sub(P[B[2]], P[B[0]]));
        return Dot(na, nb) < cos175 * Len(na) * Len(nb);
    }
    const V3 pa[3] = {P[A[0]], P[A[1]], P[A[2]]}, pb[3] = {P[B[0]], P[B[1]], P[B[2]]};
    for (int k = 0; k < 3; k++) {
        const double lo = Min3(pa[0][k], pa[1][k], pa[2][k]), hi = Max3(pa[0][k], pa[1][k], pa[2][k]);
        if (hi < Min3(pb[0][k], pb[1][k], pb[2][k]) || lo > Max3(pb[0][k], pb[1][k], pb[2][k]))
            return false;
    }
    auto isShared = [&](uint32_t v) {
        for (int i = 0; i < ns; i++)
            if (shared[i] == v)
                return true;
        return false;
    };
    static const int E[3][2] = {{0, 1}, {1, 2}, {2, 0}};
    for (const auto& e : E)
        if (!isShared(A[e[0]]) && !isShared(A[e[1]]) && SegHitsTri(pa[e[0]], pa[e[1]], pb[0], pb[1], pb[2]))
            return true;
    for (const auto& e : E)
        if (!isShared(B[e[0]]) && !isShared(B[e[1]]) && SegHitsTri(pb[e[0]], pb[e[1]], pa[0], pa[1], pa[2]))
            return true;
    return false;
}

// The number of crossing pairs between the faces of A and of B.
inline int CrossingsBetween(const std::vector<V3>& P, const std::vector<uint32_t>& A, const std::vector<uint32_t>& B,
                            double cos175) {
    int out = 0;
    for (size_t i = 0; i < A.size(); i += 3)
        for (size_t j = 0; j < B.size(); j += 3) {
            const uint32_t* ta = &A[i];
            const uint32_t* tb = &B[j];
            bool same = true;
            for (int k = 0; k < 3; k++)
                if (ta[k] != tb[0] && ta[k] != tb[1] && ta[k] != tb[2])
                    same = false;
            if (same)
                continue;
            if (TrisCross(P, ta, tb, cos175))
                out++;
        }
    return out;
}

// The distance (cm) from the s axis to a segment, in the (f, h) projection.
inline double SegLineDist(const V3& p, const V3& q) {
    const double d0 = q[0] - p[0], d1 = q[1] - p[1], l2 = d0 * d0 + d1 * d1;
    double t = l2 > 0 ? -(p[0] * d0 + p[1] * d1) / l2 : 0;
    t = Max(0, Min(1, t));
    return Hypot2(p[0] + t * d0, p[1] + t * d1);
}
// The distance from the s axis to a triangle: 0 where the axis pierces it.
inline double LineTriDist(const V3& a, const V3& b, const V3& c) {
    auto area = [](double p0, double p1, double q0, double q1, double r0, double r1) {
        return (q0 - p0) * (r1 - p1) - (q1 - p1) * (r0 - p0);
    };
    const double s1 = area(a[0], a[1], b[0], b[1], 0, 0), s2 = area(b[0], b[1], c[0], c[1], 0, 0),
                 s3 = area(c[0], c[1], a[0], a[1], 0, 0);
    if ((s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0)) {
        if (std::fabs(area(a[0], a[1], b[0], b[1], c[0], c[1])) > 1e-12)
            return 0;
    }
    return Min3(SegLineDist(a, b), SegLineDist(b, c), SegLineDist(c, a));
}

// A list of faces with the face where a test point was last found inside:
// the next count starts there (the count, or the cap it stops at, is the same
// in any order, and the next pose usually still has a point inside there).
struct FaceList {
    std::vector<uint32_t> tris;
    size_t last_in = 0;
};

struct SearchOut {
    double step = 0;
    int bad = 0, cuts = 0;
    bool clean = false;
    bool aborted = false;
    bool has_first = false;
    double first_step = 0;
    int first_bad = 0, first_cuts = 0;
};

constexpr int kNoCap = std::numeric_limits<int>::max();

class Solver {
public:
    Solver(const Input& in, const std::atomic<uint32_t>* superseded, uint32_t generation, Fit& fit)
        : in_(in), o_(in.options), superseded_(superseded), generation_(generation), fit_(fit) {}

    void Run();

private:
    const Input& in_;
    const Options& o_;
    const std::atomic<uint32_t>* superseded_;
    uint32_t generation_;
    Fit& fit_;

    Frame F_;
    int n_ = 0;
    std::vector<V3> L_, restL_;
    std::vector<uint32_t> tris_;
    std::vector<std::vector<uint32_t>> nbr_;
    std::vector<Surface> solid_;
    bool held_ = false;
    double cos175_ = 0;

    bool Stop() {
        if (superseded_ && superseded_->load(std::memory_order_relaxed) != generation_) {
            fit_.superseded = true;
            return true;
        }
        return false;
    }
    void StepDone() {
        fit_.steps++;
        if (in_.step_hook)
            in_.step_hook(in_.step_user, fit_.steps);
    }

    bool InWeapon(const V3& q) {
        const V3 g = ToGrip(F_, q);
        fit_.stats.inside_tests++;
        for (const Surface& s : solid_) {
            if (g[0] < s.lo[0] || g[1] < s.lo[1] || g[2] < s.lo[2] || g[0] > s.hi[0] || g[1] > s.hi[1] || g[2] > s.hi[2])
                continue;
            if (WindingOverHalf(s, g.v, fit_.stats))
                return true;
        }
        return false;
    }

    // How many lattice points of the faces T (sample_cm apart over each face)
    // lie inside the weapon, or would after the nudge toward it (the gap kept);
    // a point is judged where its share of the move is judge or more. Stops at
    // cap.
    template <class ShareOf, class Nudge>
    int SunkOn(FaceList& T, ShareOf shareOf, Nudge nudge, double judge, int cap) {
        int c = 0;
        const size_t N = T.tris.size();
        const size_t start = T.last_in;
        for (size_t k = 0; k < N; k += 3) {
            const size_t t = (start + k) % N;
            const uint32_t va = T.tris[t], vb = T.tris[t + 1], vc = T.tris[t + 2];
            const V3 a = L_[va], b = L_[vb], cc = L_[vc];
            const double longest = Max3(Len(Sub(b, a)), Len(Sub(cc, a)), Len(Sub(cc, b)));
            const double m = Max(1, std::ceil(longest / o_.sample_cm));
            const double sa = shareOf(va), sb = shareOf(vb), sc = shareOf(vc);
            const V3 ba = Sub(b, a), ca = Sub(cc, a);
            for (double i = 0; i <= m; i++)
                for (double j = 0; j <= m - i; j++) {
                    const double u = i / m, w = j / m;
                    if ((1 - u - w) * sa + u * sb + w * sc < judge)
                        continue;
                    const V3 q = Add(a, Add(Scl(ba, u), Scl(ca, w)));
                    if (InWeapon(q) || InWeapon(nudge(q))) {
                        c++;
                        T.last_in = t;
                        if (c >= cap)
                            return c;
                    }
                }
        }
        return c;
    }

    std::vector<uint32_t> TrisTouching(const std::vector<uint8_t>& in) const {
        std::vector<uint32_t> out;
        for (size_t t = 0; t < tris_.size(); t += 3)
            if (in[tris_[t]] || in[tris_[t + 1]] || in[tris_[t + 2]]) {
                out.push_back(tris_[t]);
                out.push_back(tris_[t + 1]);
                out.push_back(tris_[t + 2]);
            }
        return out;
    }
    double TriClear(const std::vector<uint32_t>& list) const {
        double m = std::numeric_limits<double>::infinity();
        for (size_t t = 0; t < list.size(); t += 3)
            m = Min(m, LineTriDist(L_[list[t]], L_[list[t + 1]], L_[list[t + 2]]));
        return m;
    }
    // The kept points joined by an edge to the set, in the order first met.
    std::vector<uint32_t> KeptNeighbours(const std::vector<uint32_t>& members) const {
        std::vector<uint32_t> A;
        for (uint32_t v : members)
            for (uint32_t w : nbr_[v])
                if (in_.roles[w] == Role::Kept) {
                    bool have = false;
                    for (uint32_t x : A)
                        if (x == w) { have = true; break; }
                    if (!have)
                        A.push_back(w);
                }
        return A;
    }
    V3 MeanRest(const std::vector<uint32_t>& A) const {
        V3 m{{0, 0, 0}};
        for (uint32_t v : A) {
            m[0] += restL_[v][0];
            m[1] += restL_[v][1];
            m[2] += restL_[v][2];
        }
        return Scl(m, 1 / (double)A.size());
    }

    // The step (tried in order) with the fewest cuts, then the fewest points
    // inside, the earliest of equals; a step with neither ends the search. Two
    // passes with the same answer: each step's cuts and, without cuts, whether
    // any point is inside (the first clean step wins); then, when none is
    // clean, the full counts, a count stopping where it can no longer win.
    // first_only: the caller takes the first step whenever none is clean (the
    // unclear rule 'rest'), so the second pass counts only the first step;
    // the answer is the same, the trace's later counts are then missing.
    // up_to: the least bad step is chosen only among the first step and the
    // steps at most up_to (the clean search still tries every step); the
    // second pass does not count the others.
    template <class SetStep, class CutsNow, class BadNow>
    SearchOut LeastBadStep(const std::vector<double>& steps, SetStep setStep, CutsNow cutsNow, BadNow badNow, Trace* trace,
                           bool first_only, double up_to) {
        SearchOut r;
        std::vector<int> cutsAt;
        bool haveCuts0 = false;
        int cuts0 = 0;
        if (trace) {
            trace->ran = true;
            trace->steps.clear();
        }
        for (double step : steps) {
            if (Stop()) { r.aborted = true; return r; }
            setStep(step);
            const int all = cutsNow();
            if (!haveCuts0) {
                haveCuts0 = true;
                cuts0 = all;
                if (trace) trace->cuts0 = all;
            }
            const int cuts = all - cuts0 > 0 ? all - cuts0 : 0;
            cutsAt.push_back(cuts);
            const int inside = cuts ? -1 : (badNow(1) ? 1 : 0);
            if (trace) {
                TraceStep ts;
                ts.step = step;
                ts.cuts = cuts;
                ts.inside = inside;
                trace->steps.push_back(ts);
            }
            StepDone();
            if (!cuts && !inside) {
                r.step = step;
                r.bad = 0;
                r.cuts = 0;
                r.clean = true;
                return r;
            }
        }
        bool haveBest = false;
        double bestStep = 0;
        int bestBad = 0, bestCuts = 0;
        for (size_t i = 0; i < steps.size(); i++) {
            const int cuts = cutsAt[i];
            if (i > 0 && steps[i] > up_to)
                continue;
            if (haveBest && cuts > bestCuts)
                continue;
            if (Stop()) { r.aborted = true; return r; }
            setStep(steps[i]);
            const int cap = haveBest && cuts == bestCuts ? bestBad : kNoCap;
            const int bad = badNow(cap);
            if (trace) {
                trace->steps[i].bad = bad;
                trace->steps[i].cap = cap == kNoCap ? -1 : cap;
            }
            StepDone();
            if (i == 0) {
                r.has_first = true;
                r.first_step = steps[i];
                r.first_bad = bad;
                r.first_cuts = cuts;
            }
            if (!haveBest || cuts < bestCuts || bad < bestBad) {
                haveBest = true;
                bestStep = steps[i];
                bestBad = bad;
                bestCuts = cuts;
            }
            if (first_only)
                break;
        }
        r.step = bestStep;
        r.bad = bestBad;
        r.cuts = bestCuts;
        r.clean = false;
        return r;
    }

    struct BlockPart {
        int which;                     // 0 index, 1 outer
        std::vector<uint32_t> M;
        std::vector<uint8_t> inM;
        FaceList T;
        V3 pivot;
        int tip;
        int sign;
    };
    void TurnTo(const BlockPart& P, double deg) {
        const double a = deg * kPi / 180, c = fdlibm::Cos(a), s = fdlibm::Sin(a);
        for (uint32_t v : P.M) {
            const double x = restL_[v][0] - P.pivot[0], y = restL_[v][1] - P.pivot[1];
            L_[v] = {{P.pivot[0] + x * c - y * s, P.pivot[1] + x * s + y * c, restL_[v][2]}};
        }
    }
};

inline void Solver::Run() {
    fit_ = Fit();
    const size_t np = in_.points.size() / 3;
    if (in_.points.size() % 3 || in_.tris.size() % 3 || in_.roles.size() != np) {
        fit_.error = "points, triangles and roles do not agree";
        return;
    }
    for (uint32_t t : in_.tris)
        if (t >= np) {
            fit_.error = "a triangle index is past the points";
            return;
        }
    for (const handfit::Part& p : in_.weapon) {
        if (p.positions.size() % 3 || p.tris.size() % 3) {
            fit_.error = "a weapon part is not whole points and triangles";
            return;
        }
        for (uint32_t t : p.tris)
            if (t >= p.positions.size() / 3) {
                fit_.error = "a weapon triangle index is past its part's points";
                return;
            }
    }
    n_ = (int)np;
    F_ = MakeFrame(in_);
    fit_.frame = F_;
    cos175_ = fdlibm::Cos(175 * kPi / 180);
    L_.resize(n_);
    for (int v = 0; v < n_; v++)
        L_[v] = ToLocal(F_, V3{{in_.points[3 * v], in_.points[3 * v + 1], in_.points[3 * v + 2]}});
    restL_ = L_;
    tris_ = in_.tris;
    nbr_.assign(n_, {});
    auto addNbr = [&](uint32_t v, uint32_t w) {
        for (uint32_t x : nbr_[v])
            if (x == w) return;
        nbr_[v].push_back(w);
    };
    for (size_t t = 0; t < tris_.size(); t += 3)
        for (int k = 0; k < 3; k++) {
            addNbr(tris_[t + k], tris_[t + (k + 1) % 3]);
            addNbr(tris_[t + k], tris_[t + (k + 2) % 3]);
        }

    // the weapon placed in the grip frame
    held_ = o_.fit_weapon && !in_.weapon.empty();
    if (held_) {
        const double* T = in_.weapon_matrix;
        for (const handfit::Part& mp : in_.weapon) {
            std::vector<double> pos(mp.positions.size());
            for (size_t v = 0; v < pos.size(); v += 3) {
                const double x = mp.positions[v], y = mp.positions[v + 1], z = mp.positions[v + 2];
                pos[v] = x * T[0] + y * T[4] + z * T[8] + T[12];
                pos[v + 1] = x * T[1] + y * T[5] + z * T[9] + T[13];
                pos[v + 2] = x * T[2] + y * T[6] + z * T[10] + T[14];
            }
            Surface s = MakeSurface(pos, mp.tris);
            if (s.n)
                solid_.push_back(std::move(s));
        }
    }
    fit_.held = held_;

    Prepared& prep = fit_.pose;
    prep.frame = F_;
    prep.count = n_;
    prep.rest = in_.points;
    prep.rest_local.resize(3 * (size_t)n_);
    for (int v = 0; v < n_; v++)
        for (int k = 0; k < 3; k++)
            prep.rest_local[3 * v + k] = restL_[v][k];
    prep.roles = in_.roles;
    prep.share.assign(n_, 0.0);

    // ---- the finger blocks
    std::vector<BlockPart> parts;
    for (int which = 0; which < 2; which++) {
        const Role want = which == 0 ? Role::Index : Role::Outer;
        BlockPart P;
        P.which = which;
        P.inM.assign(n_, 0);
        for (int v = 0; v < n_; v++)
            if (in_.roles[v] == want) {
                P.M.push_back((uint32_t)v);
                P.inM[v] = 1;
            }
        if (P.M.empty())
            continue;
        P.T.tris = TrisTouching(P.inM);
        const std::vector<uint32_t> A = KeptNeighbours(P.M);
        P.pivot = MeanRest(A);
        int tip = -1;
        for (uint32_t v : P.M)
            if (tip < 0 || restL_[v][0] > restL_[tip][0])
                tip = (int)v;
        P.tip = tip;
        P.sign = 0;
        parts.push_back(std::move(P));
    }
    std::vector<uint8_t> inBoth(n_, 0);
    for (const BlockPart& P : parts)
        for (uint32_t v : P.M)
            inBoth[v] = 1;
    const std::vector<uint32_t> bothT = TrisTouching(inBoth);

    if (held_) {
        for (BlockPart& P : parts) {
            std::vector<uint32_t> own;
            for (size_t t = 0; t < P.T.tris.size(); t += 3)
                if (P.inM[P.T.tris[t]] && P.inM[P.T.tris[t + 1]] && P.inM[P.T.tris[t + 2]])
                    for (int k = 0; k < 3; k++)
                        own.push_back(P.T.tris[t + k]);
            const std::vector<uint32_t>& probe = own.empty() ? P.T.tris : own;
            // the way that opens: the one that takes the block's own faces further from the line
            TurnTo(P, 3);
            const double cPlus = TriClear(probe);
            TurnTo(P, -3);
            const double cMinus = TriClear(probe);
            TurnTo(P, 0);
            P.sign = cPlus >= cMinus ? 1 : -1;
        }
        const double gap = o_.gap_cm;
        auto toLine = [gap](const V3& q) {
            double r = Hypot2(q[0], q[1]);
            if (r == 0 || std::isnan(r)) r = 1;
            return V3{{q[0] - gap * q[0] / r, q[1] - gap * q[1] / r, q[2]}};
        };
        std::vector<double> degs;
        for (double deg = 0; deg <= o_.block_max_deg + 1e-9; deg += o_.block_step_deg)
            degs.push_back(deg);
        const double judge = o_.block_judge_share;
        // Least: the least bad angle only up to block_unclear_limit of the
        // limit (the same bound form as the steps'); a clean angle above it still wins
        const double up_to = o_.block_unclear == Unclear::Least ? o_.block_max_deg * o_.block_unclear_limit + 1e-9
                                                                 : std::numeric_limits<double>::infinity();
        SearchOut s = LeastBadStep(
            degs,
            [&](double deg) {
                for (const BlockPart& P : parts) TurnTo(P, P.sign * deg);
            },
            [&]() { return CrossingsBetween(L_, bothT, tris_, cos175_); },
            [&](int cap) {
                int bad = 0;
                for (BlockPart& P : parts) {
                    if (bad >= cap) break;
                    const std::vector<uint8_t>& inM = P.inM;
                    bad += SunkOn(P.T, [&inM](uint32_t v) { return inM[v] ? 1.0 : 0.0; }, toLine, judge, cap - bad);
                }
                return bad;
            },
            in_.trace ? &fit_.finger_trace : nullptr, o_.block_unclear == Unclear::Rest && !in_.trace, up_to);
        if (s.aborted)
            return;
        double deg = s.step;
        int bad = s.bad, cuts = s.cuts;
        if (!s.clean && o_.block_unclear == Unclear::Rest && s.has_first) {
            deg = s.first_step;
            bad = s.first_bad;
            cuts = s.first_cuts;
        }
        fit_.fingers.ran = true;
        fit_.fingers.value = deg;
        fit_.fingers.bad = bad;
        fit_.fingers.cuts = cuts;
        fit_.fingers.clean = s.clean;
        // block_each: each block then closes back to the least angle that
        // suits it alone, the other held where it is (the index first, then
        // the outer block), never past the shared angle. A block whose own
        // search is not clean keeps the shared angle where that one is clean.
        std::vector<double> part_deg(parts.size(), deg);
        if (o_.block_each && parts.size() > 1 && deg > 0) {
            for (const BlockPart& P : parts) TurnTo(P, P.sign * deg);
            std::vector<double> lower;
            for (double d : degs)
                if (d <= deg + 1e-9)
                    lower.push_back(d);
            for (size_t i = 0; i < parts.size(); i++) {
                BlockPart& P = parts[i];
                const std::vector<uint8_t>& inM = P.inM;
                SearchOut own = LeastBadStep(
                    lower, [&](double d) { TurnTo(P, P.sign * d); },
                    [&]() { return CrossingsBetween(L_, bothT, tris_, cos175_); },
                    [&](int cap) {
                        return SunkOn(P.T, [&inM](uint32_t v) { return inM[v] ? 1.0 : 0.0; }, toLine, judge, cap);
                    },
                    nullptr, false, up_to);
                if (own.aborted)
                    return;
                if (own.clean || !s.clean)
                    part_deg[i] = own.step;
                TurnTo(P, P.sign * part_deg[i]);
            }
        }
        for (size_t i = 0; i < parts.size(); i++) {
            const BlockPart& P = parts[i];
            TurnTo(P, P.sign * part_deg[i]);
            BlockFit& B = fit_.blocks[P.which];
            B.sign = P.sign;
            const double sd = P.sign * part_deg[i];
            B.deg = sd == 0 ? 0.0 : sd;
        }
    }
    for (const BlockPart& P : parts) {
        BlockFit& B = fit_.blocks[P.which];
        B.present = true;
        for (int k = 0; k < 3; k++)
            B.pivot[k] = P.pivot[k];
        B.tip = P.tip;
        prep.block_sign[P.which] = held_ ? P.sign : 0;
        for (int k = 0; k < 3; k++)
            prep.block_pivot[P.which][k] = P.pivot[k];
    }

    // ---- the thumb, slid forward
    std::vector<uint32_t> TM;
    std::vector<uint8_t> inTM(n_, 0);
    for (int v = 0; v < n_; v++)
        if (in_.roles[v] == Role::Thumb) {
            TM.push_back((uint32_t)v);
            inTM[v] = 1;
        }
    if (!TM.empty()) {
        if (Stop())
            return;
        const std::vector<uint32_t> A = KeptNeighbours(TM);
        const V3 pivot = MeanRest(A);
        int tipT = -1;
        for (uint32_t v : TM)
            if (tipT < 0 || Len(Sub(restL_[v], pivot)) > Len(Sub(restL_[tipT], pivot)))
                tipT = (int)v;
        const double reach = Len(Sub(restL_[tipT], pivot));
        const V3 ax = Unit(Sub(restL_[tipT], pivot));
        std::vector<double>& share = prep.share;
        for (uint32_t v : TM) {
            const double t = Dot(Sub(restL_[v], pivot), ax) / reach;
            const double x = Max(0, Min(1, (t - o_.thumb_share_from) / Max(1e-6, o_.thumb_share_to - o_.thumb_share_from)));
            share[v] = x * x * (3 - 2 * x);
        }
        FaceList thumbT;
        thumbT.tris = TrisTouching(inTM);
        const double gap = o_.gap_cm;
        auto setShift = [&](double d) {
            for (uint32_t v : TM)
                L_[v] = {{restL_[v][0] + d * share[v], restL_[v][1], restL_[v][2]}};
        };
        std::vector<double> shifts;
        for (double d = 0; d <= o_.thumb_max_cm + 1e-9; d += o_.thumb_step_cm)
            shifts.push_back(d);
        const double judge = o_.thumb_judge_share;
        // Least: the least bad slide only up to thumb_unclear_limit of the limit
        const double up_to = o_.thumb_unclear == Unclear::Least ? o_.thumb_max_cm * o_.thumb_unclear_limit + 1e-9
                                                                 : std::numeric_limits<double>::infinity();
        SearchOut s = LeastBadStep(
            shifts, setShift, [&]() { return CrossingsBetween(L_, thumbT.tris, tris_, cos175_); },
            [&](int cap) {
                if (!held_) return 0;
                return SunkOn(thumbT, [&share](uint32_t v) { return share[v]; },
                              [gap](const V3& q) { return V3{{q[0] - gap, q[1], q[2]}}; }, judge, cap);
            },
            in_.trace ? &fit_.thumb_trace : nullptr, o_.thumb_unclear == Unclear::Rest && !in_.trace, up_to);
        if (s.aborted)
            return;
        double d = s.step;
        int bad = s.bad, cuts = s.cuts;
        if (!s.clean && o_.thumb_unclear == Unclear::Rest && s.has_first) {
            d = s.first_step;
            bad = s.first_bad;
            cuts = s.first_cuts;
        }
        // thumb_sink_cm: a thumb that clears the weapon is brought back in by
        // that much, never past the game's own thumb (slide 0)
        double sink = 0;
        if (s.clean && o_.thumb_sink_cm > 0) {
            const double to = d - o_.thumb_sink_cm > 0 ? d - o_.thumb_sink_cm : 0;
            sink = d - to;
            d = to;
        }
        setShift(d);
        ThumbFit& th = fit_.thumb;
        th.present = true;
        th.search.ran = true;
        th.search.value = d;
        th.sink_cm = sink;
        th.search.bad = bad;
        th.search.cuts = cuts;
        th.search.clean = s.clean;
        for (int k = 0; k < 3; k++) {
            th.pivot[k] = pivot[k];
            th.axis[k] = ax[k];
        }
        th.tip = tipT;
        th.reach = reach;
    }

    fit_.points.resize(3 * (size_t)n_);
    for (int v = 0; v < n_; v++) {
        const V3 g = ToGrip(F_, L_[v]);
        for (int k = 0; k < 3; k++)
            fit_.points[3 * v + k] = g[k];
    }
    prep.ok = true;
    fit_.ok = true;
}

// The 3x3 (row-vector, grip frame) that turns a direction as a block turns
// by deg about the channel frame's s axis: B^T x R x B with B's rows the
// frame's axes and R the turn in the (f, h) plane.
inline void BlockTurn(const Frame& F, double c, double s, double out[9]) {
    const double* B[3] = {F.ef, F.eh, F.es};
    const double R[3][3] = {{c, s, 0}, {-s, c, 0}, {0, 0, 1}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double sum = 0;
            for (int k = 0; k < 3; k++)
                for (int l = 0; l < 3; l++)
                    sum += B[k][i] * R[k][l] * B[l][j];
            out[3 * i + j] = sum;
        }
}

inline void MirrorTriangles(std::vector<uint32_t>& tris) {
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const uint32_t b = tris[t + 1];
        tris[t + 1] = tris[t + 2];
        tris[t + 2] = b;
    }
}

}  // namespace detail

// ------------------------------------------------------------------ the calls

// Runs both searches on the inputs. When superseded is given and its value
// differs from my_generation at a step boundary, returns early with
// Fit::superseded set (the rest of the Fit is then not meaningful).
inline Fit Solve(const Input& in, const std::atomic<uint32_t>* superseded = nullptr, uint32_t my_generation = 0) {
    Fit fit;
    detail::Solver(in, superseded, my_generation, fit).Run();
    return fit;
}

// The fitted points for any fingers' angle (degrees, unsigned, as the
// search gives it) and thumb slide (cm), with no search: cheap enough to run
// every frame while the two numbers ease. positions: 3 per point, grip
// frame. turns (may be null): 9 per point, the 3x3 rotation each point's
// block turned it by, grip frame, row-vector (turned = v x turn), identity
// for points that do not turn. A point that does not move comes back as its
// rest position bit for bit, so its move is exactly zero.
// block_deg: the index block's angle and the outer block's (BlockFit::deg
// without its sign), which differ with Options::block_each.
inline void PoseBlocks(const Prepared& p, const double block_deg[2], double thumb_slide_cm, double* positions,
                       double* turns) {
    using namespace detail;
    const int n = p.count;
    for (int v = 0; v < n; v++) {
        for (int k = 0; k < 3; k++)
            positions[3 * v + k] = p.rest[3 * v + k];
        if (turns)
            for (int k = 0; k < 9; k++)
                turns[9 * v + k] = (k % 4 == 0) ? 1.0 : 0.0;
    }
    if (!p.ok)
        return;
    for (int which = 0; which < 2; which++) {
        const int sign = p.block_sign[which];
        const double deg = sign * block_deg[which];
        if (sign == 0 || deg == 0)
            continue;
        const Role want = which == 0 ? Role::Index : Role::Outer;
        const double a = deg * kPi / 180, c = fdlibm::Cos(a), s = fdlibm::Sin(a);
        double turn[9];
        BlockTurn(p.frame, c, s, turn);
        const double* pv = p.block_pivot[which];
        for (int v = 0; v < n; v++) {
            if (p.roles[v] != want)
                continue;
            const double* r = &p.rest_local[3 * v];
            const double x = r[0] - pv[0], y = r[1] - pv[1];
            const V3 g = ToGrip(p.frame, V3{{pv[0] + x * c - y * s, pv[1] + x * s + y * c, r[2]}});
            for (int k = 0; k < 3; k++)
                positions[3 * v + k] = g[k];
            if (turns)
                for (int k = 0; k < 9; k++)
                    turns[9 * v + k] = turn[k];
        }
    }
    if (thumb_slide_cm != 0)
        for (int v = 0; v < n; v++) {
            if (p.roles[v] != Role::Thumb || p.share[v] == 0)
                continue;
            const double* r = &p.rest_local[3 * v];
            const V3 g = ToGrip(p.frame, V3{{r[0] + thumb_slide_cm * p.share[v], r[1], r[2]}});
            for (int k = 0; k < 3; k++)
                positions[3 * v + k] = g[k];
        }
    if (p.mirrored)
        for (int v = 0; v < n; v++) {
            positions[3 * v] = -positions[3 * v];
            if (turns) {
                double* t = &turns[9 * v];
                t[1] = -t[1];
                t[2] = -t[2];
                t[3] = -t[3];
                t[6] = -t[6];
            }
        }
}

// Both blocks at one angle.
inline void Pose(const Prepared& p, double fingers_deg, double thumb_slide_cm, double* positions, double* turns) {
    const double both[2] = {fingers_deg, fingers_deg};
    PoseBlocks(p, both, thumb_slide_cm, positions, turns);
}

// The inputs mirrored in the grip frame's x (a left hand becomes a right one):
// the hand's points and the channel line negated in x, the seat and weapon
// matrices' x column negated, and every triangle's winding turned back.
inline Input MirrorInput(const Input& in) {
    Input m = in;
    for (size_t i = 0; i < m.points.size(); i += 3)
        m.points[i] = -m.points[i];
    detail::MirrorTriangles(m.tris);
    m.channel_point[0] = -m.channel_point[0];
    m.channel_dir[0] = -m.channel_dir[0];
    for (int r = 0; r < 4; r++) {
        m.seat_matrix[4 * r] = -m.seat_matrix[4 * r];
        m.weapon_matrix[4 * r] = -m.weapon_matrix[4 * r];
    }
    for (Part& p : m.weapon)
        detail::MirrorTriangles(p.tris);
    return m;
}

// The fit for a left hand: its inputs mirrored into a right hand, solved by
// the right-hand rule, and the points brought back through the mirror. The
// angle, slide, counts and the channel-frame values (pivots, axis, frame)
// are the mirrored problem's; points and Pose's output are in the left
// hand's own grip frame.
inline Fit SolveMirrored(const Input& left, const std::atomic<uint32_t>* superseded = nullptr, uint32_t my_generation = 0) {
    Fit fit = Solve(MirrorInput(left), superseded, my_generation);
    for (size_t i = 0; i < fit.points.size(); i += 3)
        fit.points[i] = -fit.points[i];
    fit.pose.mirrored = true;   // Pose works in the mirrored problem and negates x last
    return fit;
}

}  // namespace handfit
