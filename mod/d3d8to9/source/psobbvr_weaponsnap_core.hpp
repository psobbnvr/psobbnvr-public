// The held weapon's geometry, recorded as the game draws it: the pure part.
//
// No game or Windows dependencies. The held model's vertex lists go to the device as triangle strips
// whose index list is stitched (an index repeated back to back joins two
// strips), and each stitched run starts on either parity. The runs are split
// at the stitches and each is turned to face along its vertex normals by a
// vote over its triangles; zero-area triangles are dropped.
//
// A strip sent without an index list (the vertex stream is the model's index
// list already expanded) has its stitches as repeated vertex records, so
// NumberStream gives identical records one number before the runs are split.
// After the orientation, MakeConsistent winds each connected piece of a draw
// one way across its shared edges and turns the piece outward.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace weaponsnap {

// One recorded draw of the held weapon.
struct HeldDraw {
    uint8_t part = 0;           // the matrix number inside the weapon's bracket
    uint8_t path = 0;           // 1 = DrawIndexedPrimitiveUP, 2 = DrawPrimitiveUP
    uint8_t has_normals = 0;    // the vertex format carried normals
    uint32_t first_vertex = 0;  // into HeldGeometry::positions / normals (vertices, not floats)
    uint32_t vertex_count = 0;
    uint32_t first_index = 0;   // into HeldGeometry::indices
    uint32_t index_count = 0;   // a multiple of 3
    uint32_t flipped = 0;       // triangles the consistency pass turned after the strip vote
};

// One hand's weapon, immutable once published. Positions and normals are in
// the weapon's model space with every node transform applied (the part
// matrix times the inverse of the weapon's seat root), unscaled; indices are
// local to their draw.
struct HeldGeometry {
    int side = 0;               // 0 main hand, 1 off-hand
    uint32_t generation = 0;    // the snapshot counter at publication
    uint32_t hash = 0;          // of the geometry, quantised to 1e-4 model units
    uint8_t category = 0;       // the weapon's item class
    uint8_t kind = 0;           // its base weapon kind
    uint32_t draws_indexed = 0;  // draws that came through the indexed path
    uint32_t draws_plain = 0;    // draws that came through the plain strip path
    uint32_t flipped = 0;        // triangles the consistency pass turned, all draws
    float box_min[3] = {0, 0, 0};
    float box_max[3] = {0, 0, 0};
    std::vector<HeldDraw> draws;
    std::vector<float> positions;  // 3 per vertex
    std::vector<float> normals;    // 3 per vertex (zero where the format has none)
    std::vector<uint16_t> indices;
    uint32_t TriangleCount() const { return (uint32_t)(indices.size() / 3); }
    uint32_t VertexCount() const { return (uint32_t)(positions.size() / 3); }
};

namespace core {

// Face normal direction (unnormalised) of a triangle.
inline void Face(const double* a, const double* b, const double* c, double out[3]) {
    const double u0 = b[0] - a[0], u1 = b[1] - a[1], u2 = b[2] - a[2];
    const double v0 = c[0] - a[0], v1 = c[1] - a[1], v2 = c[2] - a[2];
    out[0] = u1 * v2 - u2 * v1;
    out[1] = u2 * v0 - u0 * v2;
    out[2] = u0 * v1 - u1 * v0;
}

// The triangles of one index strip, as vertex ids into pos (3 doubles per
// vertex) and nrm (the same, or null when the format has no normals).
// Appends 3 ids per triangle to out. Returns the number of triangles added.
inline size_t OrientedTriangles(const uint32_t* idx, size_t n, const double* pos,
                                const double* nrm, std::vector<uint32_t>& out) {
    const size_t before = out.size();
    size_t start = 0;
    while (start < n) {
        // One run: up to (not including) the next index equal to its
        // predecessor; that index opens the next run.
        size_t end = start + 1;
        while (end < n && idx[end] != idx[end - 1])
            end++;
        const size_t len = end - start;
        const uint32_t* run = idx + start;
        start = end;
        if (len < 3)
            continue;
        std::vector<uint32_t> tris;
        for (size_t k = 0; k + 2 < len; k++) {
            uint32_t a = run[k], b = run[k + 1], c = run[k + 2];
            if (k & 1)
                std::swap(a, b);
            if (a == b || b == c || a == c)
                continue;
            double f[3];
            Face(pos + 3 * a, pos + 3 * b, pos + 3 * c, f);
            if (!(f[0] * f[0] + f[1] * f[1] + f[2] * f[2] > 1e-12))
                continue;
            tris.push_back(a);
            tris.push_back(b);
            tris.push_back(c);
        }
        if (nrm != nullptr) {
            double vote = 0.0;
            for (size_t t = 0; t < tris.size(); t += 3) {
                const uint32_t a = tris[t], b = tris[t + 1], c = tris[t + 2];
                double f[3];
                Face(pos + 3 * a, pos + 3 * b, pos + 3 * c, f);
                double s = 0.0;
                for (int i = 0; i < 3; i++)
                    s += f[i] * (nrm[3 * a + i] + nrm[3 * b + i] + nrm[3 * c + i]);
                vote += s;
            }
            if (vote < 0.0)
                for (size_t t = 0; t < tris.size(); t += 3)
                    std::swap(tris[t], tris[t + 1]);
        }
        out.insert(out.end(), tris.begin(), tris.end());
    }
    return (out.size() - before) / 3;
}

// Numbers the vertex records of a stream sent without indices. Records with
// identical bytes get one number (numbers in first-appearance order, ids has
// one per stream vertex, src the stream position each number first appeared
// at), so a stitch that repeats a vertex back to back reads as the repeated
// index it was in the model's index list.
inline void NumberStream(const uint8_t* vbase, size_t n, unsigned stride, std::vector<uint32_t>& src,
                         std::vector<uint32_t>& ids) {
    src.clear();
    ids.clear();
    std::unordered_map<std::string, uint32_t> seen;
    seen.reserve(n);
    for (size_t k = 0; k < n; k++) {
        const std::string key(reinterpret_cast<const char*>(vbase + k * stride), stride);
        const auto it = seen.find(key);
        if (it != seen.end()) {
            ids.push_back(it->second);
        } else {
            const uint32_t id = (uint32_t)src.size();
            seen.emplace(key, id);
            src.push_back((uint32_t)k);
            ids.push_back(id);
        }
    }
}

// Makes the winding of a draw's triangles (3 vertex ids each, into pos and
// nrm as above) consistent and outward. Vertices are welded by position
// (1e-4) so duplicated vertices with different normals still share edges.
// Every edge used by exactly two triangles must be walked in opposite
// directions by them: the triangles are grouped into connected pieces over
// such edges and a neighbour that disagrees is turned. A piece with no open
// edge is then turned so its signed volume is positive; an open piece by the
// sign of its triangles' faces against their vertex normals (or, with no
// normals, its signed volume about its centroid as if it were closed).
// Returns the number of triangles turned.
inline size_t MakeConsistent(const double* pos, const double* nrm, std::vector<uint32_t>& tris) {
    const size_t nt = tris.size() / 3;
    if (nt == 0)
        return 0;
    std::map<std::array<long long, 3>, uint32_t> weld;
    std::unordered_map<uint32_t, uint32_t> weld_of;  // vertex id -> weld id
    auto weld_id = [&](uint32_t v) {
        const auto f = weld_of.find(v);
        if (f != weld_of.end())
            return f->second;
        const std::array<long long, 3> key = {std::llround(pos[3 * v] * 1e4), std::llround(pos[3 * v + 1] * 1e4),
                                              std::llround(pos[3 * v + 2] * 1e4)};
        const uint32_t id = weld.emplace(key, (uint32_t)weld.size()).first->second;
        weld_of.emplace(v, id);
        return id;
    };
    std::vector<std::array<uint32_t, 3>> w(nt);
    std::vector<char> live(nt, 1);
    // Edge (low, high welded ids) -> users: triangle * 2 + (1 when it walks high to low).
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edges;
    for (size_t t = 0; t < nt; t++) {
        for (int k = 0; k < 3; k++)
            w[t][k] = weld_id(tris[3 * t + k]);
        if (w[t][0] == w[t][1] || w[t][1] == w[t][2] || w[t][0] == w[t][2]) {
            live[t] = 0;
            continue;
        }
        for (int k = 0; k < 3; k++) {
            const uint32_t a = w[t][k], b = w[t][(k + 1) % 3];
            edges[{std::min(a, b), std::max(a, b)}].push_back((uint32_t)(t * 2 + (a > b ? 1 : 0)));
        }
    }
    // Neighbours over edges with exactly two users, and whether the pair
    // walks the edge the same way (then one of them must turn).
    std::vector<std::vector<std::pair<uint32_t, uint8_t>>> adj(nt);
    std::vector<char> open(nt, 0);  // the triangle has an edge without exactly two users
    for (const auto& e : edges) {
        const std::vector<uint32_t>& u = e.second;
        if (u.size() == 2) {
            const uint8_t same = (u[0] & 1) == (u[1] & 1) ? 1 : 0;
            adj[u[0] >> 1].push_back({u[1] >> 1, same});
            adj[u[1] >> 1].push_back({u[0] >> 1, same});
        } else {
            for (uint32_t x : u)
                open[x >> 1] = 1;
        }
    }
    std::vector<char> turn(nt, 0), done(nt, 0);
    size_t turned = 0;
    for (size_t seed = 0; seed < nt; seed++) {
        if (!live[seed] || done[seed])
            continue;
        std::vector<uint32_t> piece = {(uint32_t)seed};
        done[seed] = 1;
        for (size_t head = 0; head < piece.size(); head++) {
            const uint32_t t = piece[head];
            for (const auto& nb : adj[t]) {
                if (done[nb.first])
                    continue;
                done[nb.first] = 1;
                turn[nb.first] = turn[t] ^ nb.second;
                piece.push_back(nb.first);
            }
        }
        // Which way is out for the piece as it now stands.
        bool closed = true;
        double cx = 0, cy = 0, cz = 0;
        for (uint32_t t : piece) {
            if (open[t])
                closed = false;
            for (int k = 0; k < 3; k++) {
                cx += pos[3 * tris[3 * t + k]];
                cy += pos[3 * tris[3 * t + k] + 1];
                cz += pos[3 * tris[3 * t + k] + 2];
            }
        }
        const double inv = 1.0 / (3.0 * (double)piece.size());
        cx *= inv; cy *= inv; cz *= inv;
        double volume = 0, vote = 0;
        for (uint32_t t : piece) {
            const uint32_t a = tris[3 * t], b = tris[3 * t + 1], c = tris[3 * t + 2];
            const double* pa = pos + 3 * a;
            double f[3];
            Face(pa, pos + 3 * b, pos + 3 * c, f);
            const double sign = turn[t] ? -1.0 : 1.0;
            volume += sign * ((pa[0] - cx) * f[0] + (pa[1] - cy) * f[1] + (pa[2] - cz) * f[2]);
            if (nrm != nullptr)
                for (int i = 0; i < 3; i++)
                    vote += sign * f[i] * (nrm[3 * a + i] + nrm[3 * b + i] + nrm[3 * c + i]);
        }
        const double score = (closed || nrm == nullptr) ? volume : vote;
        const bool flip_all = score < 0.0;
        for (uint32_t t : piece) {
            if ((turn[t] != 0) != flip_all) {
                std::swap(tris[3 * t], tris[3 * t + 1]);
                turned++;
            }
        }
    }
    return turned;
}

}  // namespace core
}  // namespace weaponsnap
