// A hand's mesh welded from the body draws it arrives as, and the copy of one
// of those draws with welded vertices moved.
//
// The game draws a hand as a handful of small triangle strips (or lists) whose
// vertices repeat: stitching repeats a vertex inside a strip, and the strips
// of one surface share seam positions. Welding gives every distinct position
// (rounded to WELD_STEP) one index, so a shape change is one move per welded
// vertex, applied to every draw vertex that maps to it.
//
// Positions here are in a frame of the caller's choice (the hand's grip
// point); the caller moves the draws' vertices into it. Nothing in this
// header touches the game or Direct3D.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace handweld {

constexpr int MAX_DRAWS = 24;      // biggest hand band seen: 15 draws
constexpr int MAX_VERTS = 200;     // biggest hand draw seen: 39 vertices
constexpr int MAX_STRIDE = 48;     // the body's vertex format is 36 bytes
constexpr int MAX_WELDED = 160;    // biggest welded hand seen: 36 vertices
constexpr int MAX_TRIS = 320;      // biggest welded hand seen: 68 triangles
constexpr double WELD_STEP = 1e-4; // positions this close are one vertex

// Direct3D primitive type values (D3DPT_*).
constexpr int PT_TRIANGLELIST = 4;
constexpr int PT_TRIANGLESTRIP = 5;
constexpr int PT_TRIANGLEFAN = 6;

// One draw as the builder takes it: its vertices' positions, already in the
// welding frame (verts x 3 floats).
struct DrawIn {
    int type;
    unsigned prims, verts;
    const float* pos;
};

struct Draw {
    int type;
    unsigned prims, verts;
    uint16_t map[MAX_VERTS];   // welded index of each draw vertex
};

// Zero-initialized on purpose (no member initializers): the mesh lives in
// the DLL's uninitialized data.
struct Mesh {
    int draw_count;
    Draw draws[MAX_DRAWS];
    int vertex_count;
    float pos[MAX_WELDED][3];  // the first draw vertex that made each
    int tri_count;
    uint16_t tri[MAX_TRIS][3]; // wound as the game's draws wind them
};

// Calls f(a, b, c) for each triangle of a draw by vertex number: a list's
// own, a strip's with every odd one swapped to keep the winding, a fan's.
// False when the vertex count cannot hold the primitive count.
template <typename F>
inline bool ForEachTriangle(int type, unsigned prims, unsigned verts, F f) {
    if (type == PT_TRIANGLELIST) {
        if (verts < prims * 3)
            return false;
        for (unsigned i = 0; i < prims; i++)
            f(i * 3, i * 3 + 1, i * 3 + 2);
    } else if (type == PT_TRIANGLESTRIP) {
        if (verts < prims + 2)
            return false;
        for (unsigned i = 0; i < prims; i++) {
            if (i & 1)
                f(i + 1, i, i + 2);
            else
                f(i, i + 1, i + 2);
        }
    } else if (type == PT_TRIANGLEFAN) {
        if (verts < prims + 2)
            return false;
        for (unsigned i = 0; i < prims; i++)
            f(0u, i + 1, i + 2);
    } else {
        return false;
    }
    return true;
}

// Welds the draws. Triangles with a repeated corner (a strip's stitches) are
// dropped. False on a draw it cannot read or when a limit is passed; the
// mesh is then unusable.
inline bool Build(Mesh& out, const DrawIn* in, int n) {
    memset(&out, 0, sizeof(out));
    if (n <= 0 || n > MAX_DRAWS)
        return false;
    static int32_t keys[MAX_WELDED][3];
    for (int d = 0; d < n; d++) {
        const DrawIn& s = in[d];
        if (s.verts == 0 || s.verts > (unsigned)MAX_VERTS || s.pos == nullptr)
            return false;
        Draw& o = out.draws[d];
        o.type = s.type;
        o.prims = s.prims;
        o.verts = s.verts;
        for (unsigned v = 0; v < s.verts; v++) {
            const float* p = s.pos + v * 3;
            int32_t k[3];
            for (int a = 0; a < 3; a++)
                k[a] = (int32_t)floor((double)p[a] / WELD_STEP + 0.5);
            int found = -1;
            for (int w = 0; w < out.vertex_count; w++) {
                if (keys[w][0] == k[0] && keys[w][1] == k[1] && keys[w][2] == k[2]) {
                    found = w;
                    break;
                }
            }
            if (found < 0) {
                if (out.vertex_count >= MAX_WELDED)
                    return false;
                found = out.vertex_count++;
                memcpy(keys[found], k, sizeof(k));
                memcpy(out.pos[found], p, sizeof(float) * 3);
            }
            o.map[v] = (uint16_t)found;
        }
        bool room = true;
        const bool ok = ForEachTriangle(s.type, s.prims, s.verts, [&](unsigned a, unsigned b, unsigned c) {
            const uint16_t wa = o.map[a], wb = o.map[b], wc = o.map[c];
            if (wa == wb || wb == wc || wa == wc)
                return;
            if (out.tri_count >= MAX_TRIS) {
                room = false;
                return;
            }
            out.tri[out.tri_count][0] = wa;
            out.tri[out.tri_count][1] = wb;
            out.tri[out.tri_count][2] = wc;
            out.tri_count++;
        });
        if (!ok || !room)
            return false;
    }
    out.draw_count = n;
    return true;
}

// The draw's vertex data (data, stride, verts: the game's buffer, never
// written to) copied into out with each vertex's position moved by the move
// of the welded vertex it maps to, then rotated by the 3x3 rot (row-vector
// convention: moved = move x rot). rot takes the welding frame's directions
// into the draw's own space. A move that is exactly zero leaves its vertex's
// bytes alone, so no moves reproduce the input bit for bit. When normals is
// not null and the vertex format carries a normal (has_normal: it follows
// the position), a MOVED vertex's normal is replaced by that welded normal
// rotated by rot (an unmoved vertex keeps the game's own); the rest of the
// vertex (diffuse, texture coordinates) is never touched. out holds at
// least verts * stride bytes. SubstituteTurned below is the variant that
// turns the game's own normals instead of replacing them.
inline void Substitute(const Draw& draw, const uint8_t* data, unsigned stride, bool has_normal,
                       const float (*moves)[3], const float (*normals)[3], const float rot[9],
                       uint8_t* out) {
    memcpy(out, data, (size_t)draw.verts * stride);
    for (unsigned v = 0; v < draw.verts; v++) {
        const unsigned w = draw.map[v];
        uint8_t* vert = out + (size_t)v * stride;
        const float* d = moves[w];
        const bool moved = d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f;
        if (moved) {
            float p[3];
            memcpy(p, vert, sizeof(p));
            p[0] += d[0] * rot[0] + d[1] * rot[3] + d[2] * rot[6];
            p[1] += d[0] * rot[1] + d[1] * rot[4] + d[2] * rot[7];
            p[2] += d[0] * rot[2] + d[1] * rot[5] + d[2] * rot[8];
            memcpy(vert, p, sizeof(p));
        }
        if (moved && normals != nullptr && has_normal) {
            const float* n = normals[w];
            const float r[3] = {n[0] * rot[0] + n[1] * rot[3] + n[2] * rot[6],
                                n[0] * rot[1] + n[1] * rot[4] + n[2] * rot[7],
                                n[0] * rot[2] + n[1] * rot[5] + n[2] * rot[8]};
            memcpy(vert + 12, r, sizeof(r));
        }
    }
}

// A turn that leaves a vertex's normal alone: exactly the identity, or all
// zeros (so zero-initialized turn storage reads as "not turned").
inline bool IsNoTurn(const float t[9]) {
    bool zero = true, identity = true;
    for (int i = 0; i < 9; i++) {
        if (t[i] != 0.0f)
            zero = false;
        if (t[i] != ((i % 4 == 0) ? 1.0f : 0.0f))
            identity = false;
    }
    return zero || identity;
}

// The copy for a shape change made of rigid turns and slides (whole finger
// blocks turned about a pivot, the thumb slid without turning). Per welded
// vertex w:
//   moves[w]  the vertex's whole displacement in the welding frame, the turn's
//             own swing about its pivot already included; it is applied to
//             the position exactly as Substitute applies it (moved = move x
//             rot), and a move that is exactly zero leaves the position bytes.
//   turns[w]  a 3x3 rotation in the welding frame, row-vector convention
//             (turned = v x turn, rows at [0..2], [3..5], [6..8]); it only
//             turns the normal. Identity or all zeros: not turned.
// A turned vertex's own normal (the bytes the game supplied for this draw
// vertex, in the draw's space) is taken into the welding frame (x rot^T),
// turned, and brought back (x rot): n' = n x rot^T x turn x rot. rot is
// expected to be a rotation; the result is given the original normal's
// length, so a uniform scale in rot cancels and a unit normal stays unit.
// An unturned vertex keeps its normal bytes exactly, moved or not; a vertex
// neither moved nor turned is the game's bit for bit. The normal is read and
// written only when the format carries one after the position (has_normal)
// and the stride holds it. turns may be null (moves only, no normal touched).
inline void SubstituteTurned(const Draw& draw, const uint8_t* data, unsigned stride, bool has_normal,
                             const float (*moves)[3], const float (*turns)[9], const float rot[9],
                             uint8_t* out) {
    memcpy(out, data, (size_t)draw.verts * stride);
    const bool normal_room = has_normal && stride >= 24;
    for (unsigned v = 0; v < draw.verts; v++) {
        const unsigned w = draw.map[v];
        uint8_t* vert = out + (size_t)v * stride;
        const float* d = moves[w];
        if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
            float p[3];
            memcpy(p, vert, sizeof(p));
            p[0] += d[0] * rot[0] + d[1] * rot[3] + d[2] * rot[6];
            p[1] += d[0] * rot[1] + d[1] * rot[4] + d[2] * rot[7];
            p[2] += d[0] * rot[2] + d[1] * rot[5] + d[2] * rot[8];
            memcpy(vert, p, sizeof(p));
        }
        if (turns == nullptr || !normal_room || IsNoTurn(turns[w]))
            continue;
        const float* t = turns[w];
        float n[3];
        memcpy(n, vert + 12, sizeof(n));
        // into the welding frame: x rot^T, i.e. the dot with each row of rot
        const float g[3] = {n[0] * rot[0] + n[1] * rot[1] + n[2] * rot[2],
                            n[0] * rot[3] + n[1] * rot[4] + n[2] * rot[5],
                            n[0] * rot[6] + n[1] * rot[7] + n[2] * rot[8]};
        const float k[3] = {g[0] * t[0] + g[1] * t[3] + g[2] * t[6],
                            g[0] * t[1] + g[1] * t[4] + g[2] * t[7],
                            g[0] * t[2] + g[1] * t[5] + g[2] * t[8]};
        float r[3] = {k[0] * rot[0] + k[1] * rot[3] + k[2] * rot[6],
                      k[0] * rot[1] + k[1] * rot[4] + k[2] * rot[7],
                      k[0] * rot[2] + k[1] * rot[5] + k[2] * rot[8]};
        const double was = sqrt((double)n[0] * n[0] + (double)n[1] * n[1] + (double)n[2] * n[2]);
        const double now = sqrt((double)r[0] * r[0] + (double)r[1] * r[1] + (double)r[2] * r[2]);
        if (!(now > 1e-12) || !(was > 0.0))
            continue;   // a degenerate turn or normal: keep the game's
        const float s = (float)(was / now);
        r[0] *= s;
        r[1] *= s;
        r[2] *= s;
        memcpy(vert + 12, r, sizeof(r));
    }
}

}  // namespace handweld
