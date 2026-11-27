#include "ConformalLayers.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "CurvedTopsSurface.hpp"

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "Multiplanar.hpp"
#include "ConformalShells.hpp"
#include "libslic3r/GCode/ClearanceRepair.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <limits>
#include <array>
#include <map>
#include <set>
#include <iterator>
#include <unordered_map>
#include <sstream>

namespace Slic3r::Nonplanar {

static double smoothstep(double t) { return t * t * (3. - 2. * t); }

static double ramp_rise(double t, double a)
{
    if (a <= 1e-6)
        return t;
    const double p = 1. / (1. - a);
    if (t < a)
        return p * t * t / (2. * a);
    if (t > 1. - a)
        return 1. - p * sqr(1. - t) / (2. * a);
    return p * (t - 0.5 * a);
}

double ConformalLayerField::column_phi(const Column &col, double z) const
{
    if (col.omega <= 0. || z <= col.zb)
        return z;
    const double zt = col.tf;
    if (z < zt) {
        const double W = std::max(zt - col.zb, 1e-9);
        return z + col.omega * (col.L - col.d - zt) * ramp_rise((z - col.zb) / W, col.ease);
    }
    const double f = col.L - col.d + col.d * (z - zt) / std::max(col.T - zt, 1e-9);
    return col.omega * f + (1. - col.omega) * z;
}

double ConformalLayerField::column_z(const Column &col, double level) const
{
    if (col.omega <= 0. || level <= col.zb)
        return level;
    const double zt   = col.tf;
    const double W    = std::max(zt - col.zb, 1e-9);
    const double mw   = col.omega * (col.L - col.d - zt);
    const double phit = zt + mw;
    if (level <= phit) {
        const double r = level - col.zb, a = col.ease;
        double       t;
        if (a <= 1e-6)
            t = r / (W + mw);
        else {
            const double p = 1. / (1. - a), A = mw * p / (2. * a);
            const auto   solve = [&](double rr) { return 2. * rr / (W + std::sqrt(std::max(W * W + 4. * A * rr, 0.))); };
            if (r <= a * W + 0.5 * mw * p * a)
                t = solve(r);
            else if (r <= (1. - a) * W + mw * (1. - 0.5 * p * a))
                t = (r + 0.5 * mw * p * a) / (W + mw * p);
            else
                t = 1. - solve(W + mw - r);
        }
        return col.zb + std::clamp(t, 0., 1.) * W;
    }
    return zt + (level - phit) / (col.omega * col.d / std::max(col.T - zt, 1e-9) + (1. - col.omega));
}

ConformalLayerField::Column ConformalLayerField::column(const Vec2d &xy) const
{
    const double fx = std::clamp((xy.x() - m_origin.x()) / m_pitch, 0., double(m_nx - 1) - 1e-9);
    const double fy = std::clamp((xy.y() - m_origin.y()) / m_pitch, 0., double(m_ny - 1) - 1e-9);
    const size_t i = size_t(fx), j = size_t(fy);
    const double tx = fx - double(i), ty = fy - double(j);
    const Column &a = m_cols[i * m_ny + j], &b = m_cols[(i + 1) * m_ny + j], &c = m_cols[i * m_ny + j + 1],
                 &d = m_cols[(i + 1) * m_ny + j + 1];
    const double wa = (1. - tx) * (1. - ty), wb = tx * (1. - ty), wc = (1. - tx) * ty, wd = tx * ty;
    Column out;
    out.T     = wa * a.T + wb * b.T + wc * c.T + wd * d.T;
    out.c     = wa * a.c + wb * b.c + wc * c.c + wd * d.c;
    out.L     = wa * a.L + wb * b.L + wc * c.L + wd * d.L;
    out.zb    = wa * a.zb + wb * b.zb + wc * c.zb + wd * d.zb;
    out.d     = wa * a.d + wb * b.d + wc * c.d + wd * d.d;
    out.tf    = wa * a.tf + wb * b.tf + wc * c.tf + wd * d.tf;
    out.omega = wa * a.omega + wb * b.omega + wc * c.omega + wd * d.omega;
    out.ease  = wa * a.ease + wb * b.ease + wc * c.ease + wd * d.ease;
    return out;
}

std::vector<double> ConformalLayerField::levels(double first, double top) const
{
    std::vector<double> out;
    const double band_from = std::max(first, m_level_anchor);
    for (size_t k = 0; first + double(k) * m_body_height < band_from - EPSILON; ++k)
        out.push_back(first + double(k) * m_body_height);
    const size_t n = size_t(std::max(1., std::ceil((top - band_from) / m_band_height - 1e-6) + 1.));
    for (size_t k = 0; k < n; ++k)
        out.push_back(band_from + double(k) * m_band_height);
    return out;
}

double ConformalLayerField::lowest_ramp_floor() const
{
    double lo = std::numeric_limits<double>::infinity();
    for (const Column &col : m_cols)
        if (col.omega > 0.)
            lo = std::min(lo, col.zb);
    return lo;
}

double ConformalLayerField::phi(const Vec3d &p) const { return this->column_phi(this->column(p.head<2>()), p.z()); }

double ConformalLayerField::z_at(const Vec2d &xy, double level) const { return this->column_z(this->column(xy), level); }

namespace {

constexpr double Unset = -std::numeric_limits<double>::infinity();

void top_envelope(const indexed_triangle_set &mesh, const Vec2d &origin, double g, size_t nx, size_t ny, std::vector<double> &T,
                  std::vector<double> &c)
{
    T.assign(nx * ny, Unset);
    c.assign(nx * ny, 1.);
    for (const stl_triangle_vertex_indices &f : mesh.indices) {
        const Vec3d a = mesh.vertices[f[0]].cast<double>(), b = mesh.vertices[f[1]].cast<double>(), d = mesh.vertices[f[2]].cast<double>();
        const Vec3d n = (b - a).cross(d - a);
        if (n.norm() < 1e-12 || n.z() <= 1e-3 * n.norm())
            continue;
        const double nz = n.z() / n.norm();
        const double x0 = std::min({ a.x(), b.x(), d.x() }), x1 = std::max({ a.x(), b.x(), d.x() });
        const double y0 = std::min({ a.y(), b.y(), d.y() }), y1 = std::max({ a.y(), b.y(), d.y() });
        const long   i0 = std::max(0L, long(std::ceil((x0 - origin.x()) / g))), i1 = std::min(long(nx) - 1, long(std::floor((x1 - origin.x()) / g)));
        const long   j0 = std::max(0L, long(std::ceil((y0 - origin.y()) / g))), j1 = std::min(long(ny) - 1, long(std::floor((y1 - origin.y()) / g)));
        const double det = (b.x() - a.x()) * (d.y() - a.y()) - (d.x() - a.x()) * (b.y() - a.y());
        if (std::abs(det) < 1e-14)
            continue;
        for (long i = i0; i <= i1; ++i)
            for (long j = j0; j <= j1; ++j) {
                const double px = origin.x() + double(i) * g, py = origin.y() + double(j) * g;
                const double u  = ((px - a.x()) * (d.y() - a.y()) - (d.x() - a.x()) * (py - a.y())) / det;
                const double v  = ((b.x() - a.x()) * (py - a.y()) - (px - a.x()) * (b.y() - a.y())) / det;
                if (u < -1e-9 || v < -1e-9 || u + v > 1. + 1e-9)
                    continue;
                const double z = a.z() + u * (b.z() - a.z()) + v * (d.z() - a.z());
                double      &t = T[size_t(i) * ny + size_t(j)];
                if (z > t)
                    t = z, c[size_t(i) * ny + size_t(j)] = nz;
            }
    }
}

void nearest_source(const std::vector<char> &source, size_t nx, size_t ny, double g, std::vector<size_t> &nearest, std::vector<double> &dist)
{
    nearest.assign(nx * ny, size_t(-1));
    dist.assign(nx * ny, std::numeric_limits<double>::infinity());
    std::deque<size_t> queue;
    for (size_t k = 0; k < nx * ny; ++k)
        if (source[k])
            nearest[k] = k, dist[k] = 0., queue.push_back(k);
    while (!queue.empty()) {
        const size_t k = queue.front();
        queue.pop_front();
        const long i = long(k / ny), j = long(k % ny);
        const size_t s = nearest[k];
        const long   si = long(s / ny), sj = long(s % ny);
        for (long di = -1; di <= 1; ++di)
            for (long dj = -1; dj <= 1; ++dj) {
                const long ii = i + di, jj = j + dj;
                if ((di == 0 && dj == 0) || ii < 0 || jj < 0 || ii >= long(nx) || jj >= long(ny))
                    continue;
                const size_t q = size_t(ii) * ny + size_t(jj);
                const double dd = g * std::hypot(double(ii - si), double(jj - sj));
                if (dd + 1e-9 < dist[q]) {
                    dist[q] = dd, nearest[q] = s;
                    queue.push_back(q);
                }
            }
    }
}

}

std::shared_ptr<const ConformalLayerField> build_conformal_layer_field(const indexed_triangle_set &mesh, const ConformalLayersParams &P)
{
    if (mesh.vertices.empty())
        return nullptr;
    BoundingBoxf3 box;
    for (const stl_vertex &v : mesh.vertices)
        box.merge(v.cast<double>());
    const double pad = 3.;
    const Vec2d  size(box.size().x() + 2. * pad, box.size().y() + 2. * pad);
    const double g  = std::max(P.pitch, std::sqrt(size.x() * size.y() / 4e6));
    auto         F  = std::make_shared<ConformalLayerField>();
    F->m_origin     = Vec2d(box.min.x() - pad, box.min.y() - pad);
    F->m_pitch      = g;
    F->m_nx         = size_t(std::ceil(size.x() / g)) + 1;
    F->m_ny         = size_t(std::ceil(size.y() / g)) + 1;
    const size_t nx = F->m_nx, ny = F->m_ny, N = nx * ny;
    const double h = P.layer_height, K = double(std::max(1, P.shells));
    F->m_shell_depth    = K * h;
    const double body   = P.body_height > 0. ? P.body_height : h;
    const double ident  = P.first_layer + double(std::max(1, P.flat_layers) - 1) * body;
    const double anchor = P.level_anchor > 0. ? P.level_anchor : P.first_layer;
    const double base   = std::max(ident, anchor);
    F->m_level_anchor = anchor, F->m_band_height = h, F->m_body_height = body;
    F->m_identity_below = ident;
    const double tan_max = std::tan(P.max_angle * M_PI / 180.);
    const double cos_max = std::cos(P.max_angle * M_PI / 180.);

    std::vector<double> T, c;
    top_envelope(mesh, F->m_origin, g, nx, ny, T, c);
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 1; i + 1 < nx; ++i)
            for (size_t j = 1; j + 1 < ny; ++j) {
                const size_t k = i * ny + j;
                if (T[k] != Unset)
                    continue;
                const size_t nb[4] = { k - ny, k + ny, k - 1, k + 1 };
                int          set   = 0;
                double       hi    = Unset, cc = 1.;
                for (size_t q : nb)
                    if (T[q] != Unset) {
                        ++set;
                        if (T[q] > hi)
                            hi = T[q], cc = c[q];
                    }
                if (set >= 3)
                    T[k] = hi, c[k] = cc;
            }

    const double     MinRamp = P.min_ramp;
    constexpr double Gamma   = 0.25;
    constexpr double ThinRatio = 0.3;
    const double     ThickRatio = P.max_thickness > 0. ? std::min(1.7, P.max_thickness / h) : 1.7;
    const double s_lo = 1. / ThickRatio - 1., s_hi = 1. / ThinRatio - 1.;
    constexpr double MinWidth = 1.;
    std::vector<double> D(N, 0.);
    std::vector<char>   follow(N, 0);
    std::vector<char>   low(N, 0);
    std::vector<char>   unmet(N, 0);
    for (size_t k = 0; k < N; ++k) {
        if (T[k] == Unset || c[k] < cos_max - 1e-9)
            continue;
        const double kk = std::min(K, std::floor((T[k] - base - MinRamp) * c[k] / h + 1e-9));
        if (kk < 1.) {
            low[k] = T[k] > ident + 1e-6;
            continue;
        }
        D[k] = kk * h, follow[k] = 1;
    }
    {
        std::vector<char> outside(N);
        for (size_t k = 0; k < N; ++k)
            outside[k] = !follow[k];
        std::vector<size_t> nearest;
        std::vector<double> dist;
        nearest_source(outside, nx, ny, g, nearest, dist);
        std::vector<char> core(N);
        for (size_t k = 0; k < N; ++k)
            core[k] = follow[k] && dist[k] > 0.5 * MinWidth;
        nearest_source(core, nx, ny, g, nearest, dist);
        for (size_t k = 0; k < N; ++k)
            if (follow[k] && dist[k] > 0.5 * MinWidth + g)
                follow[k] = 0;
    }
    auto label = [&](std::vector<int> &comp, std::vector<std::vector<size_t>> &comps) {
        comp.assign(N, -1);
        comps.clear();
        for (size_t k0 = 0; k0 < N; ++k0) {
            if (!follow[k0] || comp[k0] >= 0)
                continue;
            comps.emplace_back();
            std::vector<size_t> stack { k0 };
            comp[k0] = int(comps.size()) - 1;
            while (!stack.empty()) {
                const size_t k = stack.back();
                stack.pop_back();
                comps.back().push_back(k);
                const long i = long(k / ny), j = long(k % ny);
                for (long di = -1; di <= 1; ++di)
                    for (long dj = -1; dj <= 1; ++dj) {
                        const long ii = i + di, jj = j + dj;
                        if (ii < 0 || jj < 0 || ii >= long(nx) || jj >= long(ny))
                            continue;
                        const size_t q = size_t(ii) * ny + size_t(jj);
                        if (follow[q] && comp[q] < 0 && std::abs(T[q] - T[k]) <= g * std::hypot(double(di), double(dj)) * tan_max + 0.05)
                            comp[q] = comp[k], stack.push_back(q);
                    }
            }
        }
    };
    if (!P.struck.empty()) {
        constexpr double Reach = 4., Grow = 2., GrowMax = 8.;
        size_t     kept_flat = 0;
        double     z_max     = 0.;
        for (const Vec3d &p : P.struck) {
            std::set<int> rounds;
            for (const Vec3d &e : P.struck)
                if (e.z() < p.z() && (e.head<2>() - p.head<2>()).squaredNorm() < sqr(Reach))
                    rounds.insert(int(e.z()));
            const double reach = Reach + std::min(Grow * std::max(0., double(rounds.size()) - 2.), GrowMax);
            const long r = long(std::ceil(reach / g));
            const long i0 = long(std::floor((p.x() - F->m_origin.x()) / g)), j0 = long(std::floor((p.y() - F->m_origin.y()) / g));
            for (long i = std::max(0L, i0 - r); i <= std::min(long(nx) - 1, i0 + r); ++i)
                for (long j = std::max(0L, j0 - r); j <= std::min(long(ny) - 1, j0 + r); ++j)
                    if (const size_t q = size_t(i) * ny + size_t(j); follow[q] && sqr(double(i - i0)) + sqr(double(j - j0)) <= sqr(double(r)))
                        follow[q] = 0, ++kept_flat, z_max = std::max(z_max, T[q]);
        }
        if (kept_flat > 0) {
            ConformalTop top;
            top.area_mm2 = double(kept_flat) * g * g, top.z_max = z_max;
            top.fallback = "the print head met its curved layers there";
            F->tops.push_back(top);
        }
    }
    std::vector<int>                 comp;
    std::vector<std::vector<size_t>> comps;
    label(comp, comps);
    std::vector<double> floor_z(N, ident);
    const auto floors = [&]() {
        std::fill(floor_z.begin(), floor_z.end(), base);
        for (const std::vector<size_t> &cells : comps) {
            double tmin = std::numeric_limits<double>::infinity(), tmax = -tmin, shell_floor = tmin;
            for (size_t k : cells)
                tmin = std::min(tmin, T[k]), tmax = std::max(tmax, T[k]), shell_floor = std::min(shell_floor, T[k] - D[k] / c[k]);
            const double fz = std::max(base, shell_floor - std::max(1., (tmax - tmin) / 4.));
            for (size_t k : cells)
                floor_z[k] = fz;
        }
    };
    floors();
    std::vector<double> L(N, 0.), zb(N, 0.);
    const auto level_of = [&](double z) { return anchor + std::round((z - anchor) / h) * h; };
    const auto bounds = [&](size_t k) {
        const double wa = T[k] - D[k] / c[k] - floor_z[k], base = T[k] - D[k] * (1. / c[k] - 1.);
        return std::make_pair(base + s_lo * wa, base + s_hi * wa);
    };
    auto envelopes = [&](const std::vector<size_t> &cells, int ci, std::vector<double> &up, std::vector<double> &dn) {
        for (size_t k : cells) {
            const auto [lo, hi] = bounds(k);
            up[k] = hi, dn[k] = lo;
        }
        for (int pass = 0; pass < 200; ++pass) {
            bool changed = false;
            for (int dir = 0; dir < 2; ++dir)
                for (size_t idx = 0; idx < cells.size(); ++idx) {
                    const size_t k = cells[dir == 0 ? idx : cells.size() - 1 - idx];
                    const long   i = long(k / ny), j = long(k % ny);
                    for (long di = -1; di <= 1; ++di)
                        for (long dj = -1; dj <= 1; ++dj) {
                            const long ii = i + di, jj = j + dj;
                            if ((di == 0 && dj == 0) || ii < 0 || jj < 0 || ii >= long(nx) || jj >= long(ny))
                                continue;
                            const size_t q = size_t(ii) * ny + size_t(jj);
                            if (comp[q] != ci)
                                continue;
                            const double step = Gamma * g * std::hypot(double(di), double(dj));
                            if (up[q] + step < up[k] - 1e-9)
                                up[k] = up[q] + step, changed = true;
                            if (dn[q] - step > dn[k] + 1e-9)
                                dn[k] = dn[q] - step, changed = true;
                        }
                }
            if (!changed)
                break;
        }
    };
    std::vector<double> up(N), dn(N);
    const auto target_of = [&](const std::vector<size_t> &cells) {
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (size_t k : cells)
            lo = std::min(lo, T[k]), hi = std::max(hi, T[k]);
        const double balanced = level_of(0.5 * (lo + hi));
        std::map<long, size_t> votes;
        for (size_t k : cells) {
            const double d0 = D[k];
            double       a = std::numeric_limits<double>::infinity(), b = -a;
            for (double kk = std::max(1., d0 / h); kk >= 1.; kk -= 1.) {
                D[k] = kk * h;
                const auto [l, u] = bounds(k);
                a = std::min(a, l), b = std::max(b, u);
            }
            D[k] = d0;
            for (long n = long(std::ceil((a - anchor) / h - 1e-9)); n <= long(std::floor((b - anchor) / h + 1e-9)); ++n)
                ++votes[n];
        }
        double best = balanced;
        size_t most = 0;
        for (const auto &[n, v] : votes) {
            const double lv = anchor + double(n) * h;
            if (v > most || (v == most && std::abs(lv - balanced) < std::abs(best - balanced)))
                most = v, best = lv;
        }
        return best;
    };
    for (const std::vector<size_t> &cells : comps) {
        const double target = target_of(cells);
        for (size_t k : cells) {
            const double kmax = D[k] / h;
            for (double kk = kmax; kk >= 1.; kk -= 1.) {
                D[k] = kk * h;
                const auto [lo, hi] = bounds(k);
                if (target >= lo && target <= hi)
                    break;
            }
            if (const auto [lo, hi] = bounds(k); target < lo || target > hi)
                D[k] = h;
        }
    }
    for (int round = 0; round < 4; ++round) {
        size_t dropped = 0;
        for (size_t ci = 0; ci < comps.size(); ++ci) {
            envelopes(comps[ci], int(ci), up, dn);
            for (size_t k : comps[ci])
                if (dn[k] > up[k] + 1e-6) {
                    const auto [lo, hi] = bounds(k);
                    if (hi < dn[k] + 1e-6 || lo > up[k] - 1e-6)
                        follow[k] = 0, unmet[k] = 1, ++dropped;
                }
        }
        if (dropped == 0)
            break;
        label(comp, comps);
        floors();
    }
    std::vector<double> edge_dist;
    {
        std::vector<char> off(N);
        for (size_t k = 0; k < N; ++k)
            off[k] = !follow[k];
        std::vector<size_t> by;
        nearest_source(off, nx, ny, g, by, edge_dist);
    }
    constexpr double WallBand = 1.5;
    for (size_t ci = 0; ci < comps.size(); ++ci) {
        const std::vector<size_t> &cells = comps[ci];
        ConformalTop               top;
        top.area_mm2 = double(cells.size()) * g * g;
        top.z_min = std::numeric_limits<double>::infinity(), top.z_max = -top.z_min;
        for (size_t k : cells)
            top.z_min = std::min(top.z_min, T[k]), top.z_max = std::max(top.z_max, T[k]);
        if (top.area_mm2 < P.min_area) {
            top.fallback = "smaller than " + float_to_string_decimal_point(P.min_area) + " mm2";
            for (size_t k : cells)
                follow[k] = 0;
            F->tops.push_back(top);
            continue;
        }
        const double target = target_of(cells);
        top.level = target;
        envelopes(cells, int(ci), up, dn);
        for (size_t k : cells) {
            L[k] = dn[k] <= up[k] ? std::clamp(target, dn[k], up[k]) : 0.5 * (dn[k] + up[k]);
            const auto [lo, hi] = bounds(k);
            L[k] = std::clamp(L[k], lo, hi);
            const double full = std::max(T[k] - D[k] / c[k] - floor_z[k], 1e-3);
            const double m    = L[k] - (T[k] - D[k] * (1. / c[k] - 1.));
            const double need = std::min(full, std::max(MinRamp, 1.6 * (m >= 0. ? m / s_hi : m / s_lo)));
            const double rho  = WallBand + (full - need) / tan_max;
            const double W    = need + (full - need) * smoothstep(std::clamp((edge_dist[k] - WallBand) / (rho - WallBand + 1e-9), 0., 1.));
            zb[k]             = T[k] - D[k] / c[k] - W;
            top.ramp       = std::max(top.ramp, W);
            if (std::abs(L[k] - target) > 0.5 * h)
                top.stepped_mm2 += g * g;
        }
        F->tops.push_back(top);
    }
    {
        double low_area = 0., low_z = 0.;
        for (size_t k = 0; k < N; ++k)
            if (low[k] && !follow[k])
                low_area += g * g, low_z = std::max(low_z, T[k]);
        if (low_area > 0.) {
            ConformalTop top;
            top.area_mm2 = low_area, top.z_max = low_z;
            top.fallback = "too close to the flat base layers for a shell and a ramp";
            F->tops.push_back(top);
        }
        double unmet_area = 0., unmet_z = 0.;
        for (size_t k = 0; k < N; ++k)
            if (unmet[k] && !follow[k])
                unmet_area += g * g, unmet_z = std::max(unmet_z, T[k]);
        if (unmet_area > 0.) {
            ConformalTop top;
            top.area_mm2 = unmet_area, top.z_max = unmet_z;
            top.fallback = "its top's level could not reach it within the layers' thickness bounds";
            F->tops.push_back(top);
        }
    }
    std::vector<char> core(follow);
    std::vector<int>  top_of(N, -1);
    for (size_t k = 0; k < N; ++k)
        if (follow[k])
            top_of[k] = comp[k];
    {
        std::vector<size_t> nearest;
        std::vector<double> dist;
        nearest_source(follow, nx, ny, g, nearest, dist);
        for (size_t k = 0; k < N; ++k)
            if (!core[k] && T[k] != Unset && c[k] >= cos_max - 1e-9 && dist[k] <= 4. * g && nearest[k] != size_t(-1)) {
                const size_t s = nearest[k];
                const double dk = D[s];
                if (T[k] - dk / c[k] > zb[s] + 0.05 && std::abs(T[k] - T[s]) <= dist[k] * tan_max + 0.05)
                    core[k] = 1, D[k] = dk, L[k] = L[s], top_of[k] = comp[s],
                    zb[k] = std::min(std::max(base, std::min(zb[s], T[k] - dk / c[k] - 0.05)), T[k] - dk / c[k] - 0.01);
            }
    }
    std::vector<size_t> nearest;
    std::vector<double> dist;
    nearest_source(core, nx, ny, g, nearest, dist);
    F->m_cols.resize(N);
    for (size_t k = 0; k < N; ++k) {
        ConformalLayerField::Column &col = F->m_cols[k];
        if (nearest[k] == size_t(-1)) {
            col.omega = 0.;
            continue;
        }
        const size_t s = nearest[k];
        if (core[k]) {
            col.T = T[k], col.c = std::max(c[k], 0.5), col.L = L[k], col.zb = zb[k], col.d = D[k], col.tf = T[k] - D[k] / col.c, col.omega = 1.;
            continue;
        }
        col.T = T[s], col.c = std::max(c[s], 0.5), col.L = L[s], col.zb = zb[s], col.d = D[s], col.tf = T[s] - D[s] / col.c;
        const double rho = std::clamp(std::abs(L[s] - T[s]) / tan_max + 1., 1.5, 25.);
        col.omega        = smoothstep(std::clamp(1. - dist[k] / rho, 0., 1.));
    }
    {
        constexpr double StandClear = 1.;
        std::vector<char> standing(N, 0), higher(N, 0);
        bool any = false, any_higher = false;
        for (size_t k = 0; k < N; ++k)
            if (const size_t s = nearest[k]; !core[k] && T[k] != Unset && s != size_t(-1) &&
                (T[k] > T[s] + StandClear || (T[k] < T[s] - StandClear && T[k] > zb[s])))
                standing[k] = 1, any = true;
        for (size_t k = 0; k < N; ++k) {
            if (!core[k])
                continue;
            const long i = long(k / ny), j = long(k % ny);
            for (long di = -1; di <= 1; ++di)
                for (long dj = -1; dj <= 1; ++dj) {
                    const long ii = i + di, jj = j + dj;
                    if (ii < 0 || jj < 0 || ii >= long(nx) || jj >= long(ny))
                        continue;
                    if (const size_t q = size_t(ii) * ny + size_t(jj); core[q] && top_of[q] != top_of[k] && T[q] > T[k] + StandClear)
                        higher[q] = 1, any_higher = true;
                }
        }
        const auto fade_toward = [&](const std::vector<char> &source, bool lower_only) {
            std::vector<size_t> by;
            std::vector<double> away;
            nearest_source(source, nx, ny, g, by, away);
            for (size_t k = 0; k < N; ++k) {
                ConformalLayerField::Column &col = F->m_cols[k];
                if (col.omega > 0. && by[k] != size_t(-1) && (!lower_only || col.T < T[by[k]] - StandClear)) {
                    const double rho = std::clamp(std::abs(col.L - col.T) / tan_max + 1., 1.5, 25.);
                    col.omega        = std::min(col.omega, smoothstep(std::clamp(away[k] / rho, 0., 1.)));
                }
            }
        };
        if (any)
            fade_toward(standing, false);
        if (any_higher)
            fade_toward(higher, true);
    }
    int n_tops = 0;
    for (size_t k = 0; k < N; ++k)
        if (core[k])
            n_tops = std::max(n_tops, top_of[k] + 1);
    std::vector<std::array<long, 4>> top_box(size_t(n_tops), { long(nx), -1L, long(ny), -1L });
    for (size_t k = 0; k < N; ++k)
        if (core[k]) {
            auto &bx = top_box[size_t(top_of[k])];
            const long i = long(k / ny), j = long(k % ny);
            bx[0] = std::min(bx[0], i), bx[1] = std::max(bx[1], i), bx[2] = std::min(bx[2], j), bx[3] = std::max(bx[3], j);
        }
    const auto blur_core = [&](double ConformalLayerField::Column::*field, double sigma) {
        const long r = long(std::ceil(2. * sigma / g));
        std::vector<double> w(size_t(2 * r + 1));
        for (long d = -r; d <= r; ++d)
            w[size_t(d + r)] = std::exp(-0.5 * sqr(double(d) * g / sigma));
        std::vector<double> out(N, 0.), sum(N), wsum(N), tmp(N), wtmp(N);
        for (int t = 0; t < n_tops; ++t) {
            const auto &bx = top_box[size_t(t)];
            if (bx[1] < bx[0])
                continue;
            const long i0 = std::max(0L, bx[0] - r), i1 = std::min(long(nx) - 1, bx[1] + r);
            const long j0 = std::max(0L, bx[2] - r), j1 = std::min(long(ny) - 1, bx[3] + r);
            for (long i = i0; i <= i1; ++i)
                for (long j = j0; j <= j1; ++j) {
                    const size_t k = size_t(i) * ny + size_t(j);
                    const bool   own = core[k] && top_of[k] == t;
                    sum[k] = own ? F->m_cols[k].*field : 0., wsum[k] = own ? 1. : 0.;
                }
            for (int axis = 0; axis < 2; ++axis) {
                for (long i = i0; i <= i1; ++i)
                    for (long j = j0; j <= j1; ++j) {
                        double a = 0., b = 0.;
                        for (long d = -r; d <= r; ++d) {
                            const long ii = i + (axis == 0 ? d : 0), jj = j + (axis == 1 ? d : 0);
                            if (ii < i0 || jj < j0 || ii > i1 || jj > j1)
                                continue;
                            const size_t q = size_t(ii) * ny + size_t(jj);
                            a += w[size_t(d + r)] * sum[q], b += w[size_t(d + r)] * wsum[q];
                        }
                        tmp[size_t(i) * ny + size_t(j)] = a, wtmp[size_t(i) * ny + size_t(j)] = b;
                    }
                for (long i = i0; i <= i1; ++i)
                    for (long j = j0; j <= j1; ++j) {
                        const size_t k = size_t(i) * ny + size_t(j);
                        sum[k] = tmp[k], wsum[k] = wtmp[k];
                    }
            }
            for (long i = i0; i <= i1; ++i)
                for (long j = j0; j <= j1; ++j)
                    if (const size_t k = size_t(i) * ny + size_t(j); core[k] && top_of[k] == t && wsum[k] > 0.)
                        out[k] = sum[k] / wsum[k];
        }
        return out;
    };
    {
        const auto ramp_ok = [&](const ConformalLayerField::Column &col, double d) {
            const double W = col.T - d / col.c - col.zb, M = col.L - d - col.zb;
            return M > 1e-3 && W >= ThinRatio * M - 1e-9 && W <= ThickRatio * M + 1e-9;
        };
        const std::vector<double> d = blur_core(&ConformalLayerField::Column::d, 2.);
        for (size_t k = 0; k < N; ++k)
            if (core[k]) {
                ConformalLayerField::Column &col = F->m_cols[k];
                const double d0 = col.d;
                for (double t : { 1., 0.75, 0.5, 0.25 })
                    if (ramp_ok(col, d0 + t * (d[k] - d0))) {
                        col.d = d0 + t * (d[k] - d0);
                        break;
                    }
                col.tf = col.T - col.d / col.c;
            }
    }
    if (P.round_sigma > 0.) {
        const std::vector<double> tf = blur_core(&ConformalLayerField::Column::tf, P.round_sigma);
        for (size_t k = 0; k < N; ++k)
            if (core[k]) {
                ConformalLayerField::Column &col = F->m_cols[k];
                double lo = col.zb + 0.05, hi = col.T - 0.6 * col.d / col.c;
                if (P.max_thickness > 0.) {
                    const double r = P.max_thickness / h;
                    lo = std::max(lo, col.T - r * col.d / col.c);
                    hi = std::min(hi, col.zb + r * std::max(col.L - col.d - col.zb, 0.));
                }
                col.tf = lo <= hi ? std::clamp(tf[k], lo, hi) : col.T - col.d / col.c;
            }
    }
    const auto joins = [&](size_t a, size_t b) {
        const size_t sa = nearest[a], sb = nearest[b];
        return top_of[sa] == top_of[sb] || (std::abs(L[sa] - L[sb]) <= 0.5 * h && std::abs(T[sa] - T[sb]) <= 1.);
    };
    {
        std::vector<char> seam(N, 0);
        bool              any = false;
        for (size_t k = 0; k < N; ++k) {
            if (nearest[k] == size_t(-1) || core[k])
                continue;
            const long i = long(k / ny), j = long(k % ny);
            for (long di = -1; di <= 1 && !seam[k]; ++di)
                for (long dj = -1; dj <= 1; ++dj) {
                    const long ii = i + di, jj = j + dj;
                    if (ii < 0 || jj < 0 || ii >= long(nx) || jj >= long(ny))
                        continue;
                    if (const size_t q = size_t(ii) * ny + size_t(jj); nearest[q] != size_t(-1) && !joins(k, q)) {
                        seam[k] = 1, any = true;
                        break;
                    }
                }
        }
        if (any) {
            std::vector<size_t> by;
            std::vector<double> away;
            nearest_source(seam, nx, ny, g, by, away);
            for (size_t k = 0; k < N; ++k) {
                ConformalLayerField::Column &col = F->m_cols[k];
                if (!core[k] && col.omega > 0. && by[k] != size_t(-1)) {
                    const double rho = std::clamp(std::abs(col.L - col.T) / tan_max + 1., 1.5, 25.);
                    col.omega        = std::min(col.omega, smoothstep(std::clamp(away[k] / rho, 0., 1.)));
                }
            }
        }
    }
    {
        std::vector<size_t> free;
        for (size_t k = 0; k < N; ++k)
            if (!core[k] && F->m_cols[k].omega > 0.)
                free.push_back(k);
        for (int it = 0; it < 80 && !free.empty(); ++it)
            for (size_t k : free) {
                const long i = long(k / ny), j = long(k % ny);
                ConformalLayerField::Column sum;
                sum.T = sum.c = sum.L = sum.zb = sum.d = sum.tf = 0.;
                int n = 0;
                for (long di = -1; di <= 1; ++di)
                    for (long dj = -1; dj <= 1; ++dj) {
                        const long ii = i + di, jj = j + dj;
                        if ((di == 0 && dj == 0) || ii < 0 || jj < 0 || ii >= long(nx) || jj >= long(ny))
                            continue;
                        const size_t q = size_t(ii) * ny + size_t(jj);
                        if (nearest[q] == size_t(-1) || !joins(k, q))
                            continue;
                        const ConformalLayerField::Column &o = F->m_cols[q];
                        sum.T += o.T, sum.c += o.c, sum.L += o.L, sum.zb += o.zb, sum.d += o.d, sum.tf += o.tf, ++n;
                    }
                if (n == 0)
                    continue;
                ConformalLayerField::Column &col = F->m_cols[k];
                col.T = sum.T / n, col.c = sum.c / n, col.L = sum.L / n, col.zb = sum.zb / n, col.d = sum.d / n, col.tf = sum.tf / n;
            }
    }
    for (ConformalLayerField::Column &col : F->m_cols)
        if (col.omega > 0.) {
            const double rr   = (col.L - col.d - col.tf) / std::max(col.tf - col.zb, 1e-9);
            const double pmax = rr > 1e-9 ? s_hi / rr : rr < -1e-9 ? s_lo / rr : 1e9;
            col.ease          = std::clamp(1. - 1. / std::max(pmax, 1.), 0., 0.25);
        }
    if (const char *dump = research_switch("NP_CONFORMAL_DUMP"); dump != nullptr && *dump != 0) {
        FILE *f = std::fopen(dump, "w");
        if (f) {
            std::fprintf(f, "# x y T c L zb omega shell_depth=%g identity=%g\n", F->m_shell_depth, ident);
            for (size_t k = 0; k < N; ++k) {
                const auto &col = F->m_cols[k];
                std::fprintf(f, "%.3f %.3f %.4f %.4f %.4f %.4f %.4f %.4f\n", F->m_origin.x() + double(k / ny) * g, F->m_origin.y() + double(k % ny) * g,
                             col.T, col.c, col.L, col.zb, col.omega, col.d);
            }
            std::fclose(f);
        }
    }
    size_t followed = 0;
    for (const ConformalTop &t : F->tops)
        followed += t.fallback.empty();
    if (followed == 0)
        return F->tops.empty() ? nullptr : F;
    return F;
}

ConformalVariant conformal_variant(const PrintObjectConfig &config, int step)
{
    struct Rung { bool rounded, capped, flat; };
    static const Rung ladder[] = { { true, false, false }, { false, false, false }, { false, true, false }, { true, true, false },
                                   { false, false, true } };
    static_assert(std::size(ladder) == ConformalSteps + 1);
    const Rung      &rung     = ladder[std::clamp(step, 0, int(std::size(ladder)) - 1)];
    const double     rounding = config.nonplanar_conformal_rounding.value;
    ConformalVariant v;
    v.flat        = rung.flat;
    v.round_sigma = rung.flat ? 0. : rung.rounded ? rounding : std::min(2., rounding);
    v.cap_deg     = rung.capped ? config.nonplanar_conformal_fallback_angle.value : 0.;
    return v;
}

std::string conformal_variant_text(const ConformalVariant &v)
{
    if (v.flat)
        return "flat";
    return "shells rounded over " + float_to_string_decimal_point(v.round_sigma) + " mm" +
           (v.cap_deg > 0. ? ", " + float_to_string_decimal_point(v.cap_deg) + " degree cap" : std::string());
}

std::shared_ptr<const ConformalLayerField> conformal_layers_field(const PrintObject &object, std::string *summary)
{
    const PrintObjectConfig &oc = object.config();
    const PrintRegionConfig &rc = object.printing_region(0).config();
    ConformalLayersParams    P;
    P.layer_height = oc.layer_height.value;
    P.first_layer  = object.print()->config().initial_layer_print_height.value;
    const int step = conformal_cap_step(*object.model_object());
    const ConformalVariant variant = conformal_variant(oc, step);
    P.max_angle    = std::clamp(oc.nonplanar_curved_max_angle.value, 1., 45.);
    if (variant.cap_deg > 0.)
        P.max_angle = std::min(P.max_angle, variant.cap_deg);
    P.min_area    = oc.nonplanar_curved_min_area.value;
    P.round_sigma = variant.round_sigma;
    P.max_thickness = 0.75 * object.print()->config().nozzle_diameter.get_at(0);
    P.struck      = conformal_struck_points(*object.model_object());
    if (variant.flat) {
        if (summary)
            *summary = "conformal layers: flat (the export check found the head meeting the curved layers)";
        return nullptr;
    }
    P.shells       = curved_top_layers(object);
    constexpr double FineBand = 0.2;
    const indexed_triangle_set mesh = slicing_frame_mesh(object);
    if (P.layer_height > FineBand + EPSILON) {
        P.body_height  = P.layer_height;
        P.layer_height = FineBand;
    }
    P.min_ramp = std::max(1, oc.nonplanar_conformal_ramp_layers.value) * P.layer_height;
    std::shared_ptr<const ConformalLayerField> field = build_conformal_layer_field(mesh, P);
    if (field && P.body_height > 0.) {
        if (const double floor_min = field->lowest_ramp_floor(); std::isfinite(floor_min)) {
            P.level_anchor = P.first_layer + std::floor((floor_min - P.first_layer) / P.body_height + 1e-9) * P.body_height;
            field          = build_conformal_layer_field(mesh, P);
        }
    }
    std::ostringstream out;
    size_t followed = 0;
    double area     = 0.;
    if (field)
        for (const ConformalTop &t : field->tops)
            if (t.fallback.empty())
                ++followed, area += t.area_mm2;
    out << "conformal layers: " << followed << " tops followed (" << int(std::round(area)) << " mm2), cap " << P.max_angle << " deg, "
        << P.shells << " shells";
    if (field)
        for (const ConformalTop &t : field->tops)
            if (!t.fallback.empty())
                out << "; flat top " << int(std::round(t.area_mm2)) << " mm2 at z " << t.z_max << ": " << t.fallback;
    if (summary)
        *summary = out.str();
    BOOST_LOG_TRIVIAL(info) << object.model_object()->name << ": " << out.str();
    return followed > 0 ? field : nullptr;
}

bool conformal_requested(const PrintObject &object)
{
    const auto *set = object.model_object()->config.get().option<ConfigOptionEnum<NonplanarMode>>("nonplanar_mode");
    return (set ? set->value : object.print()->default_object_config().nonplanar_mode.value) == NonplanarMode::ConformalLayers;
}

std::string conformal_planar_limit(const PrintObject &object, std::string &reason)
{
    if (conformal_cap_step(*object.model_object()) >= ConformalSteps) {
        reason = "Careful! Conformal layers would bump the print head in " + std::to_string(conformal_flat_hits(*object.model_object())) +
                 " moves; printed with flat layers";
        return "access";
    }
    if (object.config().nonplanar_mode.value != NonplanarMode::ConformalLayers || !object.conformal().patches.empty())
        return {};
    std::string summary;
    conformal_layers_field(object, &summary);
    const bool base = summary.find("too close to the flat base") != std::string::npos;
    if (!base && summary.find("could not reach") != std::string::npos) {
        reason = "Conformal layers: the top surface height varies too much to fit inside a single layer's thickness limits.";
        return "bounds";
    }
    reason = base ? "Conformal layers: the top surfaces sit too close to the flat base layers to squeeze in a curved shell and transition ramp." :
                    "Conformal layers: none of the top surfaces are big enough to curve (" +
                        float_to_string_decimal_point(object.config().nonplanar_curved_min_area.value) + " mm2)";
    return base ? "base" : "area";
}

double conformal_band_height(const HeadEnvelope &head, double layer_height)
{
    double lowest = std::numeric_limits<double>::infinity();
    for (const HeadBox &b : head.boxes)
        if (!b.nozzle)
            lowest = std::min(lowest, b.min.z());
    if (!std::isfinite(lowest))
        return std::numeric_limits<double>::infinity();
    return std::max(layer_height, lowest - head.margin - layer_height - 0.4 + head.contact_tolerance);
}

std::string contour_solid_angles(const LayerField &field, const std::vector<ExPolygons> &domains, const std::vector<double> &levels,
                                 double flat_angle, bool sparse, double *dominant)
{
    std::array<double, 18> votes {};
    constexpr double Step = 2., Eps = 0.25, MinSlope = 0.035;
    std::ostringstream out;
    for (size_t k = 0; k < domains.size() && k < levels.size(); ++k) {
        double sxx = 0., sxy = 0., syy = 0., slope = 0.;
        size_t n = 0;
        const BoundingBox bb = get_extents(domains[k]);
        if (bb.defined)
            for (double x = unscale<double>(bb.min.x()) + 0.5 * Step; x < unscale<double>(bb.max.x()); x += Step)
                for (double y = unscale<double>(bb.min.y()) + 0.5 * Step; y < unscale<double>(bb.max.y()); y += Step) {
                    const Point p = Point::new_scale(x, y);
                    if (!std::any_of(domains[k].begin(), domains[k].end(), [&p](const ExPolygon &e) { return e.contains(p); }))
                        continue;
                    const double gx = (field.z_at(Vec2d(x + Eps, y), levels[k]) - field.z_at(Vec2d(x - Eps, y), levels[k])) / (2. * Eps);
                    const double gy = (field.z_at(Vec2d(x, y + Eps), levels[k]) - field.z_at(Vec2d(x, y - Eps), levels[k])) / (2. * Eps);
                    sxx += gx * gx, sxy += gx * gy, syy += gy * gy, slope += std::hypot(gx, gy), ++n;
                }
        double angle = flat_angle + 90. * double(k % 2);
        if (n > 0 && slope / double(n) > MinSlope) {
            const double steepest = 0.5 * std::atan2(2. * sxy, sxx - syy) * 180. / M_PI;
            angle = steepest + 90. + (sparse ? (k % 2 ? 45. : -45.) : 0.);
            const double contour = std::fmod(std::fmod(steepest + 90., 180.) + 180., 180.);
            votes[size_t(contour / 10.) % 18] += slope;
        }
        angle = std::fmod(std::fmod(angle, 180.) + 180., 180.);
        out << (k ? "," : "") << std::lround(angle);
    }
    if (dominant != nullptr) {
        double total = 0.;
        for (double v : votes)
            total += v;
        const size_t best = size_t(std::max_element(votes.begin(), votes.end()) - votes.begin());
        *dominant = total > 0. && votes[best] > 0.7 * total ? 10. * double(best) + 5. : -1.;
    }
    return out.str();
}

size_t layer_order_head_conflicts(const std::vector<FieldRoad> &roads, size_t layers, const HeadEnvelope &head)
{
    constexpr double Cell = 1.;
    BoundingBoxf box;
    double       zlo = std::numeric_limits<double>::infinity(), zhi = -zlo;
    for (const FieldRoad &r : roads)
        if (r.path)
            for (const Vec3d &t : r.tip)
                box.merge(t.head<2>()), zlo = std::min(zlo, t.z()), zhi = std::max(zhi, t.z());
    if (!box.defined || layers == 0)
        return 0;
    const size_t nx = size_t(std::ceil(box.size().x() / Cell)) + 1, ny = size_t(std::ceil(box.size().y() / Cell)) + 1;
    const auto   cell = [&](const Vec3d &t) {
        return size_t(std::floor((t.x() - box.min.x()) / Cell)) * ny + size_t(std::floor((t.y() - box.min.y()) / Cell));
    };
    struct Reach { long di, dj; double h; };
    std::vector<Reach> kernel;
    const long         r = long(std::ceil(std::max(box.size().x(), box.size().y()) / Cell));
    for (long di = -r; di <= r; ++di)
        for (long dj = -r; dj <= r; ++dj) {
            const double dx = double(di) * Cell, dy = double(dj) * Cell;
            double       h  = std::numeric_limits<double>::infinity();
            for (const HeadBox &b : head.boxes) {
                if (b.nozzle)
                    continue;
                const double m = head.lateral_margin + 0.5 * Cell;
                if ((b.infinite_x || (dx >= b.min.x() - m && dx <= b.max.x() + m)) && (b.infinite_y || (dy >= b.min.y() - m && dy <= b.max.y() + m)))
                    h = std::min(h, b.min.z() - head.margin);
            }
            if (h < zhi - zlo)
                kernel.push_back({ di, dj, h });
        }
    if (kernel.empty())
        return 0;
    std::vector<std::vector<std::pair<size_t, double>>> lowest(layers);
    std::vector<std::vector<std::pair<size_t, double>>> highest(layers);
    {
        std::vector<std::map<size_t, std::pair<double, double>>> by(layers);
        for (const FieldRoad &rd : roads)
            if (rd.path)
                for (const Vec3d &t : rd.tip) {
                    auto [it, fresh] = by[std::min(rd.layer, layers - 1)].try_emplace(cell(t), t.z(), t.z());
                    if (!fresh)
                        it->second.first = std::min(it->second.first, t.z()), it->second.second = std::max(it->second.second, t.z());
                }
        for (size_t k = 0; k < layers; ++k)
            for (const auto &[c, mm] : by[k])
                lowest[k].emplace_back(c, mm.first), highest[k].emplace_back(c, mm.second);
    }
    std::vector<double> top(nx * ny, -std::numeric_limits<double>::infinity());
    size_t conflicts = 0;
    for (size_t k = 0; k < layers; ++k) {
        for (const auto &[c, z] : highest[k])
            top[c] = std::max(top[c], z);
        for (const auto &[c, z] : lowest[k]) {
            const long i = long(c / ny), j = long(c % ny);
            for (const Reach &q : kernel) {
                const long ii = i + q.di, jj = j + q.dj;
                if (ii < 0 || jj < 0 || ii >= long(nx) || jj >= long(ny))
                    continue;
                if (top[size_t(ii) * ny + size_t(jj)] - q.h > z + head.contact_tolerance) {
                    ++conflicts;
                    break;
                }
            }
        }
    }
    return conflicts;
}

void link_roads(ExtrusionEntitiesPtr &roads, const ExPolygons *area, double fill_reach)
{
    constexpr double LinkMax = 1.5, LinkWalls = 3.5, LinkRise = 0.3;
    for (size_t i = 1; i < roads.size(); ++i) {
        auto *prev = static_cast<ExtrusionPathConformal *>(roads[i - 1]);
        auto *next = static_cast<ExtrusionPathConformal *>(roads[i]);
        const Point3 &a = prev->polyline.points.back(), &b = next->polyline.points.front();
        const Vec3d   d = unscale(b) - unscale(a);
        const bool   fills = prev->role() == erInternalInfill && next->role() == erInternalInfill;
        const double reach = is_perimeter(prev->role()) && is_perimeter(next->role()) ? LinkWalls :
                             fills                                                     ? std::max(LinkMax, fill_reach) :
                                                                                         LinkMax;
        if (d.head<2>().norm() > reach || std::abs(d.z()) > LinkRise || d.norm() < 1e-4)
            continue;
        if (area != nullptr) {
            bool over = true;
            for (double t = 0.15; t < 0.9 && over; t += 0.1) {
                const Point p((a.cast<double>() + t * (b - a).cast<double>()).head<2>().cast<coord_t>());
                over = std::any_of(area->begin(), area->end(), [&p](const ExPolygon &e) { return e.contains(p); });
            }
            if (!over)
                continue;
        }
        next->polyline.points.insert(next->polyline.points.begin(), a);
        if (!next->bead_height.empty())
            next->bead_height.insert(next->bead_height.begin(), next->bead_height.front());
        if (!next->tool_axis.empty())
            next->tool_axis.insert(next->tool_axis.begin(), next->tool_axis.front());
        if (!next->bead_width.empty())
            next->bead_width.insert(next->bead_width.begin(), next->bead_width.front());
    }
}

double band_offset(const std::vector<const std::vector<FieldRoad> *> &sets, double band)
{
    if (!std::isfinite(band) || band <= 0.)
        return 0.;
    double offset = 0.;
    size_t best   = std::numeric_limits<size_t>::max();
    for (int s = 0; s < 24; ++s) {
        const double o    = band * double(s) / 24.;
        size_t       cuts = 0;
        for (const std::vector<FieldRoad> *roads : sets)
            for (const FieldRoad &r : *roads)
                if (r.path)
                    for (size_t i = 1; i < r.tip.size(); ++i)
                        cuts += std::floor((r.tip[i].z() - o) / band) != std::floor((r.tip[i - 1].z() - o) / band);
        if (cuts < best)
            best = cuts, offset = o;
    }
    return offset;
}

FieldOrder band_order_roads(std::vector<FieldRoad> &roads, size_t layers, double band, BandOrderStats *stats,
                            const std::vector<std::pair<Vec3d, Vec3d>> *precede, const std::vector<ExPolygons> *domains,
                            double shared_offset)
{
    FieldOrder out;
    if (layers == 0)
        return out;
    std::vector<std::pair<size_t, size_t>> before;
    if (precede != nullptr && !precede->empty()) {
        const auto road_at = [&](const Vec3d &p) {
            size_t best = size_t(-1);
            double bd   = sqr(0.4);
            for (size_t r = 0; r < roads.size(); ++r) {
                const std::vector<Vec3d> &tip = roads[r].tip;
                if (!roads[r].path)
                    continue;
                for (size_t i = 0; i + 1 < tip.size(); ++i) {
                    const Vec3d  ab = tip[i + 1] - tip[i];
                    const double l2 = ab.head<2>().squaredNorm();
                    const double t  = l2 > 0. ? std::clamp((p - tip[i]).head<2>().dot(ab.head<2>()) / l2, 0., 1.) : 0.;
                    const Vec3d  q  = tip[i] + t * ab;
                    if (const double d = (q - p).head<2>().squaredNorm(); d < bd && std::abs(q.z() - p.z()) < 0.12)
                        bd = d, best = r;
                }
            }
            return best;
        };
        for (const auto &[hit, struck] : *precede)
            if (const size_t h = road_at(hit), s = road_at(struck); h != size_t(-1) && s != size_t(-1) && h != s)
                before.emplace_back(h, s);
    }
    std::unordered_map<const ExtrusionEntity *, size_t> road_of;
    const bool   banded = std::isfinite(band) && band > 0.;
    const double offset = !banded ? 0. : std::isfinite(shared_offset) ? shared_offset : band_offset({ &roads }, band);
    std::vector<long> group_bands;
    const auto band_of = [&](double z) { return banded ? long(std::floor((z - offset) / band)) : 0L; };
    std::map<std::pair<long, size_t>, ExtrusionEntitiesPtr> groups;
    size_t cut_roads = 0;
    for (FieldRoad &road : roads) {
        const size_t n = road.tip.size();
        if (!road.path || n < 2)
            continue;
        const size_t k = std::min(road.layer, layers - 1);
        if (banded) {
            const ExtrusionPathConformal &src = *road.path;
            ExtrusionPathConformal dense(src);
            dense.polyline.points.clear(), dense.bead_height.clear(), dense.tool_axis.clear(), dense.bead_width.clear();
            std::vector<Vec3d> tips;
            const auto push = [&](size_t i, size_t j, double t) {
                const Point3 &a = src.polyline.points[i], &b = src.polyline.points[j];
                dense.polyline.points.emplace_back(Point3((a.cast<double>() + t * (b - a).cast<double>()).cast<coord_t>()));
                tips.push_back(road.tip[i] + t * (road.tip[j] - road.tip[i]));
                if (!src.bead_height.empty()) dense.bead_height.push_back(float((1. - t) * src.bead_height[i] + t * src.bead_height[j]));
                if (!src.tool_axis.empty()) dense.tool_axis.push_back(((1.f - float(t)) * src.tool_axis[i] + float(t) * src.tool_axis[j]).normalized());
                if (!src.bead_width.empty()) dense.bead_width.push_back(float((1. - t) * src.bead_width[i] + t * src.bead_width[j]));
            };
            for (size_t i = 0; i < n; ++i) {
                push(i, i, 0.);
                if (i + 1 == n)
                    break;
                const double za = road.tip[i].z(), zb = road.tip[i + 1].z();
                long         ba = band_of(za), bb = band_of(zb);
                for (long b = std::min(ba, bb) + 1; b <= std::max(ba, bb); ++b) {
                    const double zc = offset + double(b) * band;
                    const double t  = (zc - za) / (zb - za);
                    if (t > 1e-6 && t < 1. - 1e-6)
                        push(i, i + 1, t);
                }
            }
            if (tips.size() > n) {
                *road.path = std::move(dense);
                road.tip   = std::move(tips);
            }
        }
        const size_t m = road.tip.size();
        std::vector<std::pair<size_t, size_t>> runs;
        const auto seg_band = [&](size_t i) { return band_of(0.5 * (road.tip[i].z() + road.tip[i + 1].z())); };
        for (size_t s = 0; s + 1 < m;) {
            size_t e = s + 1;
            while (e + 1 < m && seg_band(e) == seg_band(s))
                ++e;
            if (!runs.empty() && e - s < 2)
                runs.back().second = e;
            else
                runs.emplace_back(s, e);
            s = e;
        }
        const size_t ri = size_t(&road - roads.data());
        if (runs.size() <= 1) {
            road_of[road.path.get()] = ri;
            groups[{ m >= 2 ? seg_band(0) : band_of(road.tip.front().z()), k }].push_back(road.path.release());
            continue;
        }
        ++cut_roads;
        for (const auto &[s0, e0] : runs) {
            ExtrusionEntity *piece = field_sub_path(*road.path, s0, e0);
            road_of[piece] = ri;
            groups[{ seg_band(s0), k }].push_back(piece);
        }
        road.path.reset();
    }
    const auto mean_z = [](const ExtrusionEntity *e) {
        const auto &pts = static_cast<const ExtrusionPathConformal *>(e)->polyline.points;
        double      s   = 0.;
        for (const Point3 &p : pts)
            s += unscale<double>(p.z());
        return s / double(std::max<size_t>(1, pts.size()));
    };
    Point3 pos(0, 0, 0);
    bool   placed = false;
    long   last_band = std::numeric_limits<long>::min();
    size_t bands = 0;
    for (auto &[key, entities] : groups) {
        if (key.first != last_band)
            ++bands, last_band = key.first;
        ExtrusionEntitiesPtr outer, inner, solid, rest;
        for (ExtrusionEntity *e : entities)
            (e->role() == erExternalPerimeter ? outer : is_perimeter(e->role()) ? inner : is_solid_infill(e->role()) ? solid : rest).push_back(e);
        const std::array<ExtrusionEntitiesPtr *, 4> sets { &outer, &inner, &solid, &rest };
        std::array<ExtrusionEntitiesPtr *, 4>       chosen = sets;
        if (std::count_if(sets.begin(), sets.end(), [](const ExtrusionEntitiesPtr *p) { return !p->empty(); }) > 1) {
            constexpr double Reach = 0.55, Rise = 0.03;
            const auto key = [](double x, double y) { return (int64_t(std::floor(x / Reach)) << 32) ^ (int64_t(std::floor(y / Reach)) & 0xffffffff); };
            std::array<std::unordered_map<int64_t, std::vector<Vec3d>>, 4> pts;
            for (size_t si = 0; si < 4; ++si)
                for (const ExtrusionEntity *e : *sets[si]) {
                    const auto &pl = static_cast<const ExtrusionPathConformal *>(e)->polyline.points;
                    for (size_t i = 1; i < pl.size(); ++i) {
                        const Vec3d a = unscale(pl[i - 1]), b = unscale(pl[i]);
                        const int   m = std::max(1, int(std::ceil((b - a).head<2>().norm() / 0.25)));
                        for (int j = 0; j <= m; ++j) {
                            const Vec3d q = a + (b - a) * (double(j) / m);
                            pts[si][key(q.x(), q.y())].push_back(q);
                        }
                    }
                }
            const auto conflicts = [&](size_t a, size_t b) {
                size_t n = 0;
                for (const auto &[k, list] : pts[b])
                    for (const Vec3d &q : list) {
                        bool hit = false;
                        for (int dx = -1; dx <= 1 && !hit; ++dx)
                            for (int dy = -1; dy <= 1 && !hit; ++dy)
                                if (auto it = pts[a].find(key(q.x() + dx * Reach, q.y() + dy * Reach)); it != pts[a].end())
                                    for (const Vec3d &p : it->second)
                                        if ((p - q).head<2>().squaredNorm() < Reach * Reach && p.z() > q.z() + Rise) {
                                            hit = true;
                                            break;
                                        }
                        n += hit;
                    }
                return n;
            };
            std::array<std::array<size_t, 4>, 4> c {};
            for (size_t x = 0; x < 4; ++x)
                for (size_t y = 0; y < 4; ++y)
                    if (x != y && !sets[x]->empty() && !sets[y]->empty())
                        c[x][y] = conflicts(x, y);
            std::array<size_t, 4> perm { 0, 1, 2, 3 }, best_perm = perm;
            size_t best = std::numeric_limits<size_t>::max();
            do {
                size_t n = 0;
                for (size_t x = 0; x < 4; ++x)
                    for (size_t y = x + 1; y < 4; ++y)
                        n += c[perm[x]][perm[y]];
                if (n < best)
                    best = n, best_perm = perm;
            } while (std::next_permutation(perm.begin(), perm.end()));
            for (size_t x = 0; x < 4; ++x)
                chosen[x] = sets[best_perm[x]];
        }
        ExtrusionEntityCollection group;
        for (ExtrusionEntitiesPtr *part : chosen) {
            std::vector<std::pair<double, ExtrusionEntity *>> left;
            for (ExtrusionEntity *e : *part)
                left.emplace_back(mean_z(e), e);
            std::stable_sort(left.begin(), left.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
            for (size_t s = 0; s < left.size();) {
                size_t e = s + 1;
                while (e < left.size() && left[e].first < left[s].first + 0.1)
                    ++e;
                std::vector<ExtrusionEntity *> step;
                for (size_t q = s; q < e; ++q)
                    step.push_back(left[q].second);
                while (!step.empty()) {
                    size_t best = 0;
                    bool   flip = false;
                    double bd   = std::numeric_limits<double>::infinity();
                    for (size_t q = 0; q < step.size(); ++q) {
                        const auto  &pts = static_cast<ExtrusionPathConformal *>(step[q])->polyline.points;
                        const double d0  = placed ? (pts.front() - pos).cast<double>().head<2>().squaredNorm() : 0.;
                        if (d0 < bd)
                            bd = d0, best = q, flip = false;
                        if (placed && !is_perimeter(step[q]->role()) && step[q]->can_reverse()) {
                            const double d1 = (pts.back() - pos).cast<double>().head<2>().squaredNorm();
                            if (d1 < bd)
                                bd = d1, best = q, flip = true;
                        }
                    }
                    ExtrusionEntity *pick = step[best];
                    if (flip)
                        pick->reverse();
                    if (!is_perimeter(pick->role()) && pick->can_reverse()) {
                        const auto &pts = static_cast<ExtrusionPathConformal *>(pick)->polyline.points;
                        if (pts.front().z() > pts.back().z() + scale_(0.2))
                            pick->reverse();
                    }
                    pos    = static_cast<ExtrusionPathConformal *>(pick)->polyline.points.back();
                    placed = true;
                    group.entities.push_back(pick);
                    step.erase(step.begin() + std::ptrdiff_t(best));
                }
                s = e;
            }
        }
        for (const auto &[h, s] : before) {
            auto &v = group.entities;
            const auto road_is = [&](size_t r) { return [&road_of, r](const ExtrusionEntity *e) { auto it = road_of.find(e); return it != road_of.end() && it->second == r; }; };
            const auto first_s = std::find_if(v.begin(), v.end(), road_is(s));
            if (first_s == v.end())
                continue;
            ExtrusionEntitiesPtr moved;
            std::copy_if(first_s, v.end(), std::back_inserter(moved), road_is(h));
            if (moved.empty())
                continue;
            v.erase(std::remove_if(first_s, v.end(), road_is(h)), v.end());
            v.insert(std::find_if(v.begin(), v.end(), road_is(s)), moved.begin(), moved.end());
        }
        constexpr double SimplifyTolerance = 0.01;
        for (ExtrusionEntity *e : group.entities)
            static_cast<ExtrusionPathConformal *>(e)->simplify(SimplifyTolerance);
        link_roads(group.entities, domains != nullptr && key.second < domains->size() ? &(*domains)[key.second] : nullptr);
        out.groups.push_back(std::move(group));
        out.group_layers.push_back(key.second);
        group_bands.push_back(key.first);
    }
    if (stats) {
        stats->band        = band;
        stats->offset      = offset;
        stats->bands       = bands;
        stats->groups      = out.groups.size();
        stats->cut_roads   = cut_roads;
        stats->group_bands = std::move(group_bands);
    }
    return out;
}

}
