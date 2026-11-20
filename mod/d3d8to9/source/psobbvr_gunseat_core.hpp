#pragma once

// The gun seat: where a gun sits in the closed fist.
//
// A gun's handle is not its model's +z: the grip leans back, and the model's
// origin sits near it, not on it. Per gun kind the handle is a line in model
// space, two points (the middle of the grip's bottom end and of its top end)
// read off the kind's base model. The seat carries the gun so that this line
// lies on the fist channel, the line the closed fist holds a handle along
// (eyeheight::ChannelSeat):
//   1. the handle frame, a rigid motion in model space: the handle's middle to
//      the origin, its direction to +z, the model's x kept as the new x as
//      near as the handle allows;
//   2. the weapon scale about that origin;
//   3. the melee seat's offsets: a roll about the handle (+z), the slide
//      along it, the tilt about x, then the shifts along -y (wrist to
//      fingers) and +x (out of the palm), cm at the world scale;
//   4. the channel: a turn about x by its tilt, about y by its yaw, and the
//      move to its point at z = 0, as weapongrip::MeleeSeatMatrix.
// The mod scales a part's matrix about the weapon root before the seat, so the
// seat in its order is S(1 / s) x frame x S(s) x offsets x channel, which is
//   T(-s * middle) x R x offsets x channel
// with s the weapon scale and R the handle frame's rotation.
//
// Every factor is a rotation or a translation, so the seat is a rigid motion
// (a proper rotation and a translation); the weapon scale reaches it only
// through the translation s * middle.
//
// The seat is used the other way round from the melee seat: the gun, its shot
// ray and everything aimed by it stay on grip tuple x controller (the gun
// frame), and the hand moves to inverse(seat) x gun frame, so that the gun in
// the hand's frame is the seat ([vr] gun_seat 1, IK arms only; Place). Which
// handle line an item takes, or none, goes by its item code (LineKind), since
// a few models are drawn by items of another kind and some models have no
// handle a line can describe.
//
// Pure maths: standard library only, no globals, no Direct3D types. Matrices
// are 16 numbers, row-vector convention (p' = p x M), translation in elements
// 12-14, the same layout as D3DMATRIX. Units are game units; cm = units /
// (0.01 * world scale).

#include <cmath>
#include <cstdint>

namespace gunseat {

// The weapon kinds, numbered as the item table's base kinds (0x00 knuckle to
// 0x12 card; guns are 0x06 handgun, 0x07 rifle, 0x08 mechgun, 0x09 shot and
// 0x11 launcher).
constexpr int kKindCount = 0x13;

// One kind's handle line in model space; has is false for a kind with none.
struct Handle {
    bool has;
    double a[3];  // the grip's bottom end
    double b[3];  // the grip's top end
};

// BEGIN gun handle table (generated)
// Generated from the guns' handle lines and the default slide: do not edit
// by hand.
//
// Per weapon kind: whether it has a handle line, its bottom end and its top
// end, in model space (game units). The gun barrel runs along -y.
constexpr Handle kHandles[kKindCount] = {
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x00 knuckle
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x01 saber
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x02 sword
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x03 dagger
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x04 partisan
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x05 slicer
    {true, {0.0, 0.605, -0.78}, {0.0, 0.1, 0.8}},     // 0x06 handgun
    {true, {0.0, 0.83, -0.83}, {0.0, 0.02, 0.75}},    // 0x07 rifle
    {true, {0.04, 0.47, -0.75}, {0.04, 0.22, 0.65}},  // 0x08 mechgun
    {true, {0.0, 0.41, -1.0}, {0.0, -0.45, 1.4}},     // 0x09 shot
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x0A cane
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x0B rod
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x0C wand
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x0D claw
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x0E double saber
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x0F twin sword
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x10 katana
    {true, {0.0, 0.9, -1.55}, {0.0, -0.1, 1.4}},      // 0x11 launcher
    {false, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},        // 0x12 card
};
// The default slide along the handle from the channel's point, toward the
// handle's top end (cm at the world scale).
constexpr double kSlideDefaultCm = 0.0;
// END gun handle table (generated)

// An item drawn with another kind's model, and the kind whose line it takes.
struct LineOverride {
    uint32_t code;
    int kind;
};

// BEGIN gun item table (generated)
// Generated: the gun models without a handle line and the per-item line
// overrides, with every item code drawn with each model. Do not edit by
// hand.
//
// Item codes (ItemCode). Guns drawn with a model no handle line describes: the
// hand stays where it is without the gun seat.
constexpr uint32_t kNoHandleItems[] = {
    0x000608,  // TENSION BLASTER
    0x000708,  // RIANOV 303SNR
    0x000709,  // RIANOV 303SNR model (000708)
    0x00070A,  // RIANOV 303SNR model (000708)
    0x00070B,  // RIANOV 303SNR model (000708)
    0x00070C,  // RIANOV 303SNR model (000708)
    0x00070D,  // RIANOV 303SNR model (000708)
    0x004201,  // MASTER RAVEN
    0x004301,  // LAST SWAN
    0x004700,  // ROCKET PUNCH
    0x004900,  // TWIN PSYCHOGUN
    0x004A00,  // DRILL LAUNCHER
    0x004B01,  // DUAL BIRD
    0x008300,  // TWIN PSYCHOGUN model (004900)
    0x008301,  // TWIN PSYCHOGUN model (004900)
    0x008302,  // TWIN PSYCHOGUN model (004900)
    0x008303,  // TWIN PSYCHOGUN model (004900)
    0x008304,  // TWIN PSYCHOGUN model (004900)
    0x008305,  // TWIN PSYCHOGUN model (004900)
    0x008306,  // TWIN PSYCHOGUN model (004900)
    0x008307,  // TWIN PSYCHOGUN model (004900)
    0x008308,  // TWIN PSYCHOGUN model (004900)
    0x008309,  // TWIN PSYCHOGUN model (004900)
    0x00830A,  // TWIN PSYCHOGUN model (004900)
    0x00830B,  // TWIN PSYCHOGUN model (004900)
    0x00830C,  // TWIN PSYCHOGUN model (004900)
    0x00830D,  // TWIN PSYCHOGUN model (004900)
    0x00830E,  // TWIN PSYCHOGUN model (004900)
    0x00830F,  // TWIN PSYCHOGUN model (004900)
    0x008310,  // TWIN PSYCHOGUN model (004900)
    0x008400,  // ROCKET PUNCH model (004700)
    0x008401,  // ROCKET PUNCH model (004700)
    0x008402,  // ROCKET PUNCH model (004700)
    0x008403,  // ROCKET PUNCH model (004700)
    0x008404,  // ROCKET PUNCH model (004700)
    0x008405,  // ROCKET PUNCH model (004700)
    0x008406,  // ROCKET PUNCH model (004700)
    0x008407,  // ROCKET PUNCH model (004700)
    0x008408,  // ROCKET PUNCH model (004700)
    0x008409,  // ROCKET PUNCH model (004700)
    0x00840A,  // ROCKET PUNCH model (004700)
    0x00840B,  // ROCKET PUNCH model (004700)
    0x00840C,  // ROCKET PUNCH model (004700)
    0x00840D,  // ROCKET PUNCH model (004700)
    0x00840E,  // ROCKET PUNCH model (004700)
    0x00840F,  // ROCKET PUNCH model (004700)
    0x008410,  // ROCKET PUNCH model (004700)
    0x008B03,  // PHONON MASER
    0x00C000,  // CANNON ROUGE
};
// Items drawn with the base model of another kind: the kind whose line they take.
constexpr LineOverride kLineOverrides[] = {
    {0x00E801, 0x08},  // a handgun drawn with the Mechgun model (000800)
};
// END gun item table (generated)

// An item's code as 0x00CCVV: its category and variant (the item's data[1] and
// data[2]; data[0] is 0 for every weapon).
inline uint32_t ItemCode(int category, int variant) {
    return ((uint32_t)(category & 0xFF) << 8) | (uint32_t)(variant & 0xFF);
}

// The kind whose handle line an item of this kind and code is carried on, or
// -1 for none: a kind without a line (not a gun), or a model no line
// describes.
inline int LineKind(uint32_t code, int kind) {
    for (uint32_t c : kNoHandleItems)
        if (c == code)
            return -1;
    for (const LineOverride& o : kLineOverrides)
        if (o.code == code)
            kind = o.kind;
    if (kind < 0 || kind >= kKindCount || !kHandles[kind].has)
        return -1;
    return kind;
}

// The seat's offsets from the channel line: cm along the handle (toward its
// top end), cm along the hand (wrist to fingers), cm out of the palm, degrees
// about the palm axis and degrees about the handle.
struct Amounts {
    double slide_cm;
    double hand_cm;
    double side_cm;
    double pitch_deg;
    double roll_deg;
};
inline Amounts DefaultAmounts() {
    return {kSlideDefaultCm, 0.0, 0.0, 0.0, 0.0};
}

// The channel as the seat takes it: the point (x, y at z = 0) and the turns
// (radians) of eyeheight::ChannelSeat.
struct Channel {
    double point[2];
    double tilt;
    double yaw;
};

// The handle frame of a kind: the line's middle, its unit direction (bottom
// to top), its length, and the frame's axes in model space (z = the
// direction).
struct Frame {
    double mid[3];
    double dir[3];
    double length;
    double x[3], y[3], z[3];
};

inline double Length3(const double v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// False for a kind out of range or without a handle line.
inline bool HandleFrame(int kind, Frame& f) {
    if (kind < 0 || kind >= kKindCount || !kHandles[kind].has)
        return false;
    const Handle& h = kHandles[kind];
    double d[3];
    for (int i = 0; i < 3; i++) {
        d[i] = h.b[i] - h.a[i];
        f.mid[i] = (h.a[i] + h.b[i]) / 2.0;
    }
    f.length = Length3(d);
    if (!(f.length > 0.0))
        return false;
    for (int i = 0; i < 3; i++)
        f.dir[i] = d[i] / f.length;
    const double* ez = f.dir;
    // the model's x made square to the handle
    const double along = ez[0];
    double ex[3] = {1.0 - along * ez[0], -along * ez[1], -along * ez[2]};
    const double lx = Length3(ex);
    if (!(lx > 0.0))
        return false;
    for (int i = 0; i < 3; i++) {
        f.x[i] = ex[i] / lx;
        f.z[i] = ez[i];
    }
    f.y[0] = f.z[1] * f.x[2] - f.z[2] * f.x[1];
    f.y[1] = f.z[2] * f.x[0] - f.z[0] * f.x[2];
    f.y[2] = f.z[0] * f.x[1] - f.z[1] * f.x[0];
    return true;
}

inline void Identity(double m[16]) {
    for (int i = 0; i < 16; i++)
        m[i] = (i % 5 == 0) ? 1.0 : 0.0;
}

// out = a then b (row vectors); out may be a or b.
inline void Multiply(const double a[16], const double b[16], double out[16]) {
    double r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    for (int i = 0; i < 16; i++)
        out[i] = r[i];
}

// The seat of a gun of this kind on the channel: the transform from the
// gun's model space, after the weapon scale about its root, into the hand's
// grip frame. False, with the identity, for a kind without a handle line.
inline bool SeatMatrix(int kind, const Channel& channel, const Amounts& amounts,
                       double world_scale, double weapon_scale, double out[16]) {
    Identity(out);
    Frame f;
    if (!HandleFrame(kind, f))
        return false;
    const double cm = 0.01 * world_scale;
    const double rad = 3.14159265358979323846 / 180.0;
    // T(-s * middle) x R: R's columns are the frame's axes
    double m[16];
    Identity(m);
    for (int i = 0; i < 3; i++) {
        m[4 * i + 0] = f.x[i];
        m[4 * i + 1] = f.y[i];
        m[4 * i + 2] = f.z[i];
    }
    double shift[16];
    Identity(shift);
    for (int i = 0; i < 3; i++)
        shift[12 + i] = -weapon_scale * f.mid[i];
    Multiply(shift, m, m);
    // the offsets: roll, slide, tilt, then the shifts along the hand and out
    // of the palm
    const double roll = amounts.roll_deg * rad, pitch = amounts.pitch_deg * rad;
    double t[16];
    Identity(t);
    t[0] = std::cos(roll); t[1] = std::sin(roll); t[4] = -std::sin(roll); t[5] = std::cos(roll);
    Multiply(m, t, m);
    Identity(t);
    t[14] = amounts.slide_cm * cm;
    Multiply(m, t, m);
    Identity(t);
    t[5] = std::cos(pitch); t[6] = std::sin(pitch); t[9] = -std::sin(pitch); t[10] = std::cos(pitch);
    Multiply(m, t, m);
    Identity(t);
    t[12] = amounts.side_cm * cm;
    t[13] = -amounts.hand_cm * cm;
    Multiply(m, t, m);
    // the channel: about x by its tilt, about y by its yaw, to its point
    Identity(t);
    t[5] = std::cos(channel.tilt); t[6] = std::sin(channel.tilt);
    t[9] = -std::sin(channel.tilt); t[10] = std::cos(channel.tilt);
    Multiply(m, t, m);
    Identity(t);
    t[0] = std::cos(channel.yaw); t[2] = -std::sin(channel.yaw);
    t[8] = std::sin(channel.yaw); t[10] = std::cos(channel.yaw);
    Multiply(m, t, m);
    Identity(t);
    t[12] = channel.point[0];
    t[13] = channel.point[1];
    Multiply(m, t, out);
    return true;
}

// The same in single precision, for a D3DMATRIX (&m._11).
inline bool SeatMatrixF(int kind, const Channel& channel, const Amounts& amounts,
                        double world_scale, double weapon_scale, float out[16]) {
    double m[16];
    const bool ok = SeatMatrix(kind, channel, amounts, world_scale, weapon_scale, m);
    for (int i = 0; i < 16; i++)
        out[i] = (float)m[i];
    return ok;
}

// The left hand's seat from the right hand's: X M X with X = diag(-1, 1, 1).
// The left hand is drawn as the right hand's geometry with x negated, so the
// side shift, the channel's point x and its yaw change sign. Still a rigid
// motion. out must not be in.
inline void Mirrored(const float in[16], float out[16]) {
    for (int i = 0; i < 16; i++)
        out[i] = in[i];
    out[1] = -in[1];
    out[2] = -in[2];
    out[4] = -in[4];
    out[8] = -in[8];
    out[12] = -in[12];
}

// The kind's handle line in the hand's frame through a seat (after the weapon
// scale about the root): a point on it, the grip's bottom end, and its unit
// direction toward the top end. False for a kind without a line.
inline bool HandleLine(int kind, const float seat[16], double weapon_scale, double point[3],
                       double dir[3]) {
    if (kind < 0 || kind >= kKindCount || !kHandles[kind].has)
        return false;
    const Handle& h = kHandles[kind];
    double b[3];
    for (int j = 0; j < 3; j++) {
        point[j] = seat[12 + j];
        b[j] = seat[12 + j];
        for (int i = 0; i < 3; i++) {
            point[j] += h.a[i] * weapon_scale * seat[4 * i + j];
            b[j] += h.b[i] * weapon_scale * seat[4 * i + j];
        }
    }
    double d[3] = {b[0] - point[0], b[1] - point[1], b[2] - point[2]};
    const double l = Length3(d);
    if (!(l > 0.0))
        return false;
    for (int j = 0; j < 3; j++)
        dir[j] = d[j] / l;
    return true;
}

// The inverse of a rigid motion (rotation and translation): the transposed
// rotation and the translation turned back by it. out must not be m.
inline void RigidInverse(const double m[16], double out[16]) {
    Identity(out);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[4 * i + j] = m[4 * j + i];
    for (int j = 0; j < 3; j++)
        out[12 + j] = -(m[12] * m[4 * j] + m[13] * m[4 * j + 1] + m[14] * m[4 * j + 2]);
}

// ---- which hand moves

constexpr int kMechgunKind = 0x08;

// Why the gun seat does or does not move a hand.
enum class Why {
    Moves,        // the hand is moved onto the gun
    NoGun,        // no gun held
    NoLine,       // a gun no handle line describes, or an item of a kind without one
    NotThisHand,  // the left hand, without a mechgun half drawn in it
    OffGrip,      // the gun is held off the hand bone, so not drawn on the gun frame
    NoChannel,    // the character's fist channel is not known
    IkOff,        // IK arms do not pose the hand
    Off,          // [vr] gun_seat is not 1
};

inline const char* WhyText(Why w) {
    switch (w) {
    case Why::Moves: return "the hand is moved onto the gun";
    case Why::NoGun: return "no gun held";
    case Why::NoLine: return "no handle line for this item";
    case Why::NotThisHand: return "no mechgun half drawn in this hand";
    case Why::OffGrip: return "the gun is held off the hand bone";
    case Why::NoChannel: return "no fist channel";
    case Why::IkOff: return "IK arms off";
    case Why::Off: return "gun_seat 0";
    }
    return "?";
}

// What decides it for one hand.
struct HandInput {
    int setting;           // [vr] gun_seat
    bool gun;              // the held item is a gun
    int kind;              // its base kind
    uint32_t code;         // its item code (ItemCode)
    int side;              // 0 left, 1 right
    bool left_half_drawn;  // an off-hand half has been drawn in the left hand lately
    bool channel;          // the character's fist channel is known
    bool ik_arms;          // IK arms pose the hand
    bool on_grip;          // the gun is drawn on this hand's gun frame (seated on the hand bone)
};

// Whether the hand is moved onto the gun, and why not. line_kind: the kind
// whose handle line the item takes, -1 for none.
inline Why Decide(const HandInput& in, int& line_kind) {
    line_kind = -1;
    if (!in.gun)
        return Why::NoGun;
    line_kind = LineKind(in.code, in.kind);
    if (line_kind < 0)
        return Why::NoLine;
    if (in.side == 0 && (line_kind != kMechgunKind || !in.left_half_drawn))
        return Why::NotThisHand;
    if (!in.on_grip)
        return Why::OffGrip;
    if (!in.channel)
        return Why::NoChannel;
    if (!in.ik_arms)
        return Why::IkOff;
    if (in.setting != 1)
        return Why::Off;
    return Why::Moves;
}

// The two frames of one hand, from the gun frame (grip tuple x controller):
// the gun is drawn and aimed on the gun frame whatever happens to the hand;
// the hand (the IK wrist target) is on the gun frame too, or with move_hand at
// inverse(seat) x gun frame, so that seat x hand = gun frame. seat: the gun in
// this hand's frame (Mirrored for the left hand).
struct Placement {
    float gun[16];
    float hand[16];
};
inline Placement Place(bool move_hand, const float gun_frame[16], const float seat[16]) {
    Placement p;
    for (int i = 0; i < 16; i++) {
        p.gun[i] = gun_frame[i];
        p.hand[i] = gun_frame[i];
    }
    if (move_hand) {
        double s[16], inv[16], g[16], h[16];
        for (int i = 0; i < 16; i++) {
            s[i] = seat[i];
            g[i] = gun_frame[i];
        }
        RigidInverse(s, inv);
        Multiply(inv, g, h);
        for (int i = 0; i < 16; i++)
            p.hand[i] = (float)h[i];
    }
    return p;
}

// The shot's ray from the frame a gun is drawn in (origin, unit forward): its
// -y axis (row 1, the muzzle end) from its origin. pitch_trim_deg tilts it
// about the frame's side axis (row 0), positive = toward world down. False,
// with nothing written, for a degenerate frame.
inline bool RayFromFrame(const float m[16], float pitch_trim_deg, float* origin, float* fwd) {
    float f[3] = {-m[4], -m[5], -m[6]};
    float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (len < 1e-6f)
        return false;
    f[0] /= len; f[1] /= len; f[2] /= len;
    if (pitch_trim_deg != 0.0f) {
        // t = side x fwd, signed so positive trim tilts toward world down (-y).
        float ax[3] = {m[0], m[1], m[2]};
        const float al = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        if (al > 1e-6f) {
            ax[0] /= al; ax[1] /= al; ax[2] /= al;
            float t[3] = {ax[1] * f[2] - ax[2] * f[1],
                          ax[2] * f[0] - ax[0] * f[2],
                          ax[0] * f[1] - ax[1] * f[0]};
            if (t[1] > 0.0f) {
                t[0] = -t[0]; t[1] = -t[1]; t[2] = -t[2];
            }
            const float a = pitch_trim_deg * (3.14159265f / 180.0f);
            const float c = cosf(a), s = sinf(a);
            for (int i = 0; i < 3; i++)
                f[i] = f[i] * c + t[i] * s;
        }
    }
    fwd[0] = f[0]; fwd[1] = f[1]; fwd[2] = f[2];
    origin[0] = m[12]; origin[1] = m[13]; origin[2] = m[14];
    return true;
}

}  // namespace gunseat
