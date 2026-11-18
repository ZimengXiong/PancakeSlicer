#include "FieldOrder.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include <cstdlib>

#include "Nonplanar/HeadEnvelope.hpp"
#include "ExtrusionEntity.hpp"
#include "ExPolygon.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "OrientationField.hpp"
#include "ClipperUtils.hpp"

#include "Point.hpp"
#include "Polyline.hpp"
#include "libslic3r.h"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/GCode/ToolKinematics.hpp"

#include <Eigen/Geometry>
#include <boost/log/trivial.hpp>
#include <functional>
#include <math.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r::Nonplanar {

Matrix3d field_head_frame(const ToolKinematics *kinematics, const Vec3d &tip, const Vec3d &axis)
{
    if (kinematics != nullptr && kinematics->rotary_count() > 0) {
        JointPose joints;
        if (kinematics->inverse(ToolPose{ tip, axis }, nullptr, joints))
            return kinematics->head_frame(joints);
    }
    return Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), axis).toRotationMatrix().transpose();
}

std::vector<Vec3d> head_samples(const HeadEnvelope &head, double step)
{
    std::vector<Vec3d> pts;
    for (const HeadBox &box : head.boxes) {
        if (box.infinite_x || box.infinite_y)
            continue;
        if (box.nozzle && box.max.z() <= head.tip_allowance.height)
            continue;
        const double  grow = box.nozzle ? 0. : head.lateral_margin;
        const Vec3d   lo(box.min.x() - grow, box.min.y() - grow,
                         box.nozzle ? std::max(box.min.z(), head.tip_allowance.height) : box.min.z() - head.margin);
        const Vec3d   hi(box.max.x() + grow, box.max.y() + grow, box.max.z());
        const double  pitch = box.nozzle ? step : step * (box.min.z() < 15. ? 2. : 4.);
        const Vec3i32 cnt = ((hi - lo) / pitch).array().ceil().cast<int32_t>().cwiseMax(1);
        for (int c = 0; c <= cnt.z(); ++c)
            for (int b = 0; b <= cnt.y(); ++b)
                for (int a = 0; a <= cnt.x(); ++a)
                    if (a == 0 || b == 0 || c == 0 || a == cnt.x() || b == cnt.y() || c == cnt.z())
                        pts.emplace_back(lo + Vec3d(double(a) / cnt.x() * (hi.x() - lo.x()), double(b) / cnt.y() * (hi.y() - lo.y()),
                                                    double(c) / cnt.z() * (hi.z() - lo.z())));
    }
    std::sort(pts.begin(), pts.end(), [](const Vec3d &a, const Vec3d &b) { return a.z() < b.z(); });
    return pts;
}

namespace {

constexpr double Raster        = 0.25;
constexpr double NozzleContact = 0.;

struct Fragment
{
    size_t road;
    size_t i0, i1;
    int    piece;
};

struct Piece
{
    size_t           layer = 0;
    Vec2d            center { Vec2d::Zero() };
    std::vector<int> fragments;
    size_t           level = 0;
    std::vector<int> after;
    int              before = 0;
};

ExtrusionPathConformal *sub_path(const ExtrusionPathConformal &src, size_t i0, size_t i1)
{
    Polyline3 polyline;
    polyline.points.assign(src.polyline.points.begin() + i0, src.polyline.points.begin() + i1 + 1);
    std::vector<float> heights(src.bead_height.begin() + i0, src.bead_height.begin() + i1 + 1);
    std::vector<Vec3f> axes;
    if (!src.tool_axis.empty())
        axes.assign(src.tool_axis.begin() + i0, src.tool_axis.begin() + i1 + 1);
    auto *out = new ExtrusionPathConformal(std::move(polyline), src, std::move(heights), std::move(axes));
    if (!src.bead_width.empty())
        out->bead_width.assign(src.bead_width.begin() + i0, src.bead_width.begin() + i1 + 1);
    out->native_road_id = src.native_road_id;
    out->patch_id = src.patch_id;
    out->shell    = src.shell;
    return out;
}

}

ExtrusionPathConformal *field_sub_path(const ExtrusionPathConformal &src, size_t i0, size_t i1) { return sub_path(src, i0, i1); }

FieldOrder order_field_roads(std::vector<FieldRoad> &roads, const OrientationField &field, const FieldOrderParams &params,
                             const std::function<FieldSupport(const std::vector<Vec3d> &)> &support_of,
                             const std::function<void()> &throw_if_canceled)
{
    FieldOrder   out;
    const size_t layers = params.layer_top.size();
    out.stats.layers    = layers;
    if (roads.empty() || layers == 0)
        return out;
    auto cancel = [&throw_if_canceled]() {
        if (throw_if_canceled)
            throw_if_canceled();
    };
    const size_t field_roads = roads.size();

    Vec2d lo = Vec2d::Constant(std::numeric_limits<double>::max()), hi = -lo;
    auto extend = [&](const FieldRoad &r) {
        for (const Vec3d &q : r.deformed) {
            lo = lo.cwiseMin(q.head<2>());
            hi = hi.cwiseMax(q.head<2>());
        }
    };
    for (const FieldRoad &r : roads) {
        extend(r);
        for (const Vec3d &t : r.tip) {
            lo = lo.cwiseMin(t.head<2>());
            hi = hi.cwiseMax(t.head<2>());
        }
    }
    lo -= Vec2d::Constant(3.);
    hi += Vec2d::Constant(3.);
    const double cell = std::max(1., params.cell);
    const int    ncx = std::max(1, int(std::ceil((hi.x() - lo.x()) / cell))), ncy = std::max(1, int(std::ceil((hi.y() - lo.y()) / cell)));
    const int    nrx = std::max(1, int(std::ceil((hi.x() - lo.x()) / Raster))), nry = std::max(1, int(std::ceil((hi.y() - lo.y()) / Raster)));
    constexpr int        MaxSplit = 1;
    constexpr int        SubIds   = 1 + (1 << MaxSplit) * (1 << MaxSplit);
    std::vector<uint8_t> refine(size_t(ncx) * size_t(ncy), 0);
    auto cell_of = [&](const Vec3d &q) {
        const double fx = (q.x() - lo.x()) / cell, fy = (q.y() - lo.y()) / cell;
        const int    cx = std::clamp(int(fx), 0, ncx - 1), cy = std::clamp(int(fy), 0, ncy - 1);
        const int    c = cy * ncx + cx, sub = 1 << refine[size_t(c)];
        if (sub == 1)
            return c * SubIds;
        const int sx = std::clamp(int((fx - cx) * sub), 0, sub - 1), sy = std::clamp(int((fy - cy) * sub), 0, sub - 1);
        return c * SubIds + 1 + sy * (1 << MaxSplit) + sx;
    };
    auto cell_center = [&](int id) {
        const int c = id / SubIds, s = id % SubIds, sub = 1 << refine[size_t(c)];
        const Vec2d corner = lo + cell * Vec2d(c % ncx, c / ncx);
        if (s == 0)
            return Vec2d(corner + Vec2d::Constant(0.5 * cell));
        return Vec2d(corner + cell / sub * Vec2d((s - 1) % (1 << MaxSplit) + 0.5, (s - 1) / (1 << MaxSplit) + 0.5));
    };
    auto pixel = [&](const Vec3d &q, int &x, int &y) {
        x = std::clamp(int((q.x() - lo.x()) / Raster), 0, nrx - 1);
        y = std::clamp(int((q.y() - lo.y()) / Raster), 0, nry - 1);
    };
    const int64_t ncell = int64_t(ncx) * ncy * SubIds;

    auto point_layer = [&](const FieldRoad &road, size_t i) {
        if (road.layer >= layers)
            return road.layer;
        const size_t k = size_t(std::lower_bound(params.layer_top.begin(), params.layer_top.end(), road.deformed[i].z() - 1e-6) -
                                params.layer_top.begin());
        return std::min(k, road.layer);
    };
    std::vector<Piece>               pieces;
    std::vector<int>                 column_of;
    std::vector<Fragment>            fragments;
    std::unordered_map<int64_t, int> piece_of;
    std::vector<std::vector<int>>    raster;
    std::vector<std::vector<int>>    volume;
    std::vector<std::vector<int>>    reaches;
    std::vector<int>                 order;
    size_t lean_points = 0, lean_blocked = 0;
    for (int pass = 0;; ++pass) {
    out = FieldOrder();
    out.stats.layers = layers;
    roads.resize(field_roads);
    pieces.clear();
    column_of.clear();
    fragments.clear();
    piece_of.clear();
    raster.assign(layers, {});
    volume.assign(layers, {});
    reaches.clear();
    order.clear();
    std::vector<int> cycle_list;
    auto fragment = [&](size_t from, int grow) {
        for (size_t r = from; r < roads.size(); ++r) {
            const std::vector<Vec3d> &q = roads[r].deformed;
            const size_t              n = q.size();
            if (n < 2 || (roads[r].shell && roads[r].layer < layers))
                continue;
            const bool          by_layer = (roads[r].shell || roads[r].by_height) && roads[r].layer < layers;
            std::vector<int>    c(n);
            std::vector<size_t> pl(n, roads[r].layer);
            for (size_t i = 0; i < n; ++i) {
                c[i] = cell_of(q[i]);
                if (by_layer)
                    pl[i] = point_layer(roads[r], i);
            }
            std::vector<std::pair<size_t, size_t>> runs;
            for (size_t s = 0; s < n;) {
                size_t e = s + 1;
                while (e < n && c[e] == c[s] && pl[e] == pl[s])
                    ++e;
                if (!runs.empty() && e - s < 3)
                    runs.back().second = e;
                else
                    runs.emplace_back(s, e);
                s = e;
            }
            if (runs.size() > 1 && runs[0].second - runs[0].first < 3) {
                runs[1].first = 0;
                runs.erase(runs.begin());
            }
            for (size_t j = 0; j < runs.size(); ++j) {
                const size_t i0 = j == 0 ? 0 : runs[j].first - 1, i1 = runs[j].second - 1;
                if (i1 <= i0)
                    continue;
                const size_t layer = pl[runs[j].first];
                const int64_t key = (int64_t(layer) * 2 + int64_t(roads[r].shell)) * ncell + c[runs[j].first];
                auto [it, fresh]  = piece_of.emplace(key, int(pieces.size()));
                if (fresh) {
                    column_of.push_back(c[runs[j].first]);
                    Piece piece;
                    piece.layer  = layer;
                    piece.level  = layer;
                    piece.center = cell_center(c[runs[j].first]);
                    pieces.push_back(std::move(piece));
                }
                const int p = it->second;
                pieces[p].fragments.push_back(int(fragments.size()));
                fragments.push_back({ r, i0, i1, p });
                for (size_t i = i0; i <= i1; ++i)
                    for (int half = 0; half < (i < i1 ? 2 : 1); ++half) {
                        const size_t      k   = point_layer(roads[r], i);
                        std::vector<int> &ras = raster[k];
                        if (ras.empty())
                            ras.assign(size_t(nrx) * size_t(nry), -1);
                        std::vector<int> *vol = k < layers && !roads[r].shell ? &volume[k] : nullptr;
                        if (vol != nullptr && vol->empty())
                            vol->assign(size_t(nrx) * size_t(nry), -1);
                        int x, y;
                        pixel(half ? Vec3d(0.5 * (q[i] + q[i + 1])) : q[i], x, y);
                        for (int b = std::max(0, y - grow); b <= std::min(nry - 1, y + grow); ++b)
                            for (int a = std::max(0, x - grow); a <= std::min(nrx - 1, x + grow); ++a) {
                                ras[size_t(b) * nrx + a] = p;
                                if (vol != nullptr)
                                    (*vol)[size_t(b) * nrx + a] = p;
                            }
                    }
            }
        }
    };
    fragment(0, 0);
    const size_t field_pieces = pieces.size();

    std::vector<std::vector<int>>   supports(pieces.size());
    std::vector<std::vector<Vec3d>> floating(pieces.size());
    auto any_in = [&](const std::vector<int> &ras, int x, int y, double radius, std::vector<int> *found) {
        bool      any = false;
        const int r   = int(std::ceil(radius / Raster));
        const double r2 = (radius / Raster) * (radius / Raster);
        if (ras.empty())
            return false;
        for (int b = std::max(0, y - r); b <= std::min(nry - 1, y + r); ++b)
            for (int a = std::max(0, x - r); a <= std::min(nrx - 1, x + r); ++a)
                if (const int s = ras[size_t(b) * nrx + a]; s >= 0 && double((a - x) * (a - x) + (b - y) * (b - y)) <= r2) {
                    any = true;
                    if (found == nullptr)
                        return true;
                    if (std::find(found->begin(), found->end(), s) == found->end())
                        found->push_back(s);
                }
        return any;
    };
    tbb::parallel_for(tbb::blocked_range<size_t>(0, field_pieces), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t p = range.begin(); p < range.end(); ++p) {
            const Piece &piece = pieces[p];
            for (int fi : piece.fragments) {
                const Fragment  &f     = fragments[fi];
                const FieldRoad &road  = roads[f.road];
                const double     reach = 0.5 * road.path->width + road.path->height;
                const ExtrusionRole role = road.path->role();
                const bool          checked = is_perimeter(role) || role == erBottomSurface || role == erTopSolidInfill || role == erGapFill;
                std::vector<int> found;
                for (size_t i = f.i0 + (f.i0 > 0); i <= f.i1; ++i) {
                    const size_t k = point_layer(road, i);
                    if (k == 0)
                        continue;
                    int x, y;
                    pixel(road.deformed[i], x, y);
                    found.clear();
                    if (!any_in(road.shell ? raster[k - 1] : volume[k - 1], x, y, reach, &found)) {
                        if (checked)
                            floating[p].push_back(road.tip[i]);
                        continue;
                    }
                    const int   c    = cell_of(road.deformed[i]);
                    const auto  same = std::find_if(found.begin(), found.end(), [&](int s) { return column_of[s] == c; });
                    for (int s : found)
                        if ((same == found.end() || s == *same) && std::find(supports[p].begin(), supports[p].end(), s) == supports[p].end())
                            supports[p].push_back(s);
                }
            }
        }
    });
    for (size_t p = 0; p < field_pieces; ++p) {
        out.stats.floating_mm += 0.5 * double(floating[p].size());
        out.floating_points.insert(out.floating_points.end(), floating[p].begin(), floating[p].end());
        for (int sup : supports[p])
            if (pieces[size_t(sup)].layer < pieces[p].layer) {
                pieces[size_t(sup)].after.push_back(int(p));
                ++out.stats.support_edges;
            }
    }
    cancel();

    FieldSupport support;
    if (support_of && !out.floating_points.empty())
        support = support_of(out.floating_points);
    const size_t support_layers = support.top.size();
    out.stats.support_layers    = support_layers;
    if (support_layers > 0) {
        raster.resize(layers + support_layers);
        for (FieldRoad &r : support.roads) {
            r.layer += layers;
            roads.push_back(std::move(r));
        }
        fragment(field_roads, int(std::ceil(1. / Raster)));
        supports.resize(pieces.size());
        for (size_t p = field_pieces; p < pieces.size(); ++p) {
            Piece          &piece = pieces[p];
            const Fragment &f     = fragments[piece.fragments.front()];
            const double phi = field.forward(roads[f.road].tip[(f.i0 + f.i1) / 2]).z();
            piece.level      = size_t(std::lower_bound(params.layer_top.begin(), params.layer_top.end(), phi) - params.layer_top.begin());
            if (piece.layer > layers)
                for (int fi : piece.fragments) {
                    const Fragment  &g    = fragments[fi];
                    const FieldRoad &road = roads[g.road];
                    for (size_t i = g.i0; i <= g.i1; ++i) {
                        int x, y;
                        pixel(road.deformed[i], x, y);
                        any_in(raster[piece.layer - 1], x, y, 0.5, &supports[p]);
                    }
                }
            for (int sup : supports[p]) {
                pieces[size_t(sup)].after.push_back(int(p));
                ++out.stats.support_edges;
            }
        }
        out.stats.support_pieces = pieces.size() - field_pieces;
        for (size_t p = field_pieces; p < pieces.size(); ++p) {
            std::vector<int> above;
            for (int fi : pieces[p].fragments) {
                const Fragment  &f    = fragments[fi];
                const FieldRoad &road = roads[f.road];
                for (size_t i = f.i0; i <= f.i1; i += 2)
                    for (double dz : { 0.3, 0.6, 1.0, 1.5, 2.0, 3.0, 4.0 }) {
                        const Vec3d q = road.tip[i] + Vec3d(0., 0., dz);
                        const Vec3d d = field.forward(q);
                        const auto  it = std::lower_bound(params.layer_top.begin(), params.layer_top.end(), d.z() - 1e-9);
                        if (it == params.layer_top.end())
                            continue;
                        int x, y;
                        pixel(d, x, y);
                        any_in(raster[size_t(it - params.layer_top.begin())], x, y, 0.5, &above);
                    }
            }
            for (int q : above)
                if (q < int(field_pieces) && std::find(pieces[p].after.begin(), pieces[p].after.end(), q) == pieces[p].after.end()) {
                    pieces[p].after.push_back(q);
                    ++out.stats.support_edges;
                }
        }
        for (size_t p = 0; p < field_pieces; ++p) {
            std::vector<int> under;
            for (const Vec3d &q : floating[p]) {
                const auto it = std::upper_bound(support.top.begin(), support.top.end(), q.z() - 0.5 * params.layer_height.front());
                int        x, y;
                pixel(q, x, y);
                for (size_t j = size_t(it - support.top.begin()); j > 0 && support.top[j - 1] > q.z() - 3.; --j)
                    if (any_in(raster[layers + j - 1], x, y, 1., &under))
                        break;
            }
            for (int s : under)
                if (std::find(pieces[s].after.begin(), pieces[s].after.end(), int(p)) == pieces[s].after.end()) {
                    pieces[s].after.push_back(int(p));
                    ++out.stats.support_edges;
                }
        }
    }
    cancel();

    const std::vector<Vec3d> head = head_samples(params.head, 0.75);
    auto layer_of = [&](double phi) -> int {
        const auto it = std::lower_bound(params.layer_top.begin(), params.layer_top.end(), phi - 1e-9);
        if (it == params.layer_top.end())
            return -1;
        const size_t k = size_t(it - params.layer_top.begin());
        return phi > params.layer_top[k] - params.layer_height[k] - 1e-9 ? int(k) : -1;
    };

    const int                         reach_px = int(std::ceil(0.75 / Raster));
    std::vector<std::vector<uint8_t>> near(layers);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers), [&](const tbb::blocked_range<size_t> &range) {
        std::vector<uint8_t> row;
        for (size_t k = range.begin(); k < range.end(); ++k) {
            if (raster[k].empty())
                continue;
            row.assign(size_t(nrx) * size_t(nry), 0);
            for (int y = 0; y < nry; ++y)
                for (int x = 0; x < nrx; ++x)
                    if (raster[k][size_t(y) * nrx + x] >= 0)
                        for (int a = std::max(0, x - reach_px); a <= std::min(nrx - 1, x + reach_px); ++a)
                            row[size_t(y) * nrx + a] = 1;
            near[k].assign(row.size(), 0);
            for (int y = 0; y < nry; ++y)
                for (int x = 0; x < nrx; ++x)
                    if (row[size_t(y) * nrx + x])
                        for (int b = std::max(0, y - reach_px); b <= std::min(nry - 1, y + reach_px); ++b)
                            near[k][size_t(b) * nrx + x] = 1;
        }
    });

    if (pass == 0 && params.lean_max > 0.) {
        const double lean_rate = Geometry::deg2rad(5.);
        auto backward = [&](const Vec3d &tip, const Vec3d &axis, size_t k, int limit) {
            const Matrix3d h = field_head_frame(params.kinematics, tip, axis).transpose();
            int            n = 0;
            for (const Vec3d &s : head) {
                const Vec3d q = tip + h * s;
                if (q.z() < 0.) {
                    if (++n >= limit)
                        return n;
                    continue;
                }
                const Vec3d d  = field.forward(q);
                const int   kq = layer_of(d.z());
                if (kq < 0 || size_t(kq) >= k || raster[size_t(kq)].empty())
                    continue;
                int x, y;
                pixel(d, x, y);
                if (near[size_t(kq)][size_t(y) * nrx + x] && any_in(raster[size_t(kq)], x, y, 0.75, nullptr) && ++n >= limit)
                    return n;
            }
            return n;
        };
        std::vector<double> leans;
        for (int i = 1; i <= 5; ++i)
            leans.push_back(0.2 * i * params.lean_max);
        std::atomic<size_t> leaned { 0 }, blocked { 0 };
        tbb::parallel_for(tbb::blocked_range<size_t>(0, field_roads), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t r = range.begin(); r < range.end(); ++r) {
                FieldRoad &road = roads[r];
                std::vector<Vec3f> &axes = road.path->tool_axis;
                if (axes.size() != road.tip.size())
                    continue;
                const size_t n = axes.size();
                std::vector<Vec3d> turn(n, Vec3d::Zero());
                bool               any = false;
                for (size_t i = 0; i < n; ++i) {
                    const size_t k  = road.shell ? layers : point_layer(road, i);
                    const Vec3d  a0 = axes[i].cast<double>().normalized();
                    if (k == 0 || backward(road.tip[i], a0, k, 1) == 0)
                        continue;
                    const Vec3d  g      = road.shell ? a0 : Vec3d(field.gradient(road.tip[i]).normalized());
                    const double bead_h = i < road.path->bead_height.size() ? road.path->bead_height[i] : road.path->height;
                    const double dip    = std::asin(std::clamp(0.5 * bead_h / std::max(params.head.tip_flat_radius, 1e-3), 0., 1.));
                    const double cap    = std::max(std::min(params.lean_max, dip), std::acos(std::clamp(a0.dot(g), -1., 1.)));
                    const Vec3d e1 = (std::abs(a0.z()) < 0.9 ? Vec3d::UnitZ() : Vec3d::UnitX()).cross(a0).normalized();
                    const Vec3d e2 = a0.cross(e1);
                    Vec3d       best_turn = Vec3d::Zero();
                    int         best_hits = backward(road.tip[i], a0, k, 1 << 30);
                    bool        clear     = false;
                    for (double lean : leans) {
                        for (int s = 0; s < 12 && !clear; ++s) {
                            const double psi  = 2. * M_PI * s / 12.;
                            const Vec3d  side = std::cos(psi) * e1 + std::sin(psi) * e2;
                            const Vec3d  a    = std::cos(lean) * a0 + std::sin(lean) * side;
                            if (std::acos(std::clamp(a.z(), -1., 1.)) > params.max_tilt || std::acos(std::clamp(a.dot(g), -1., 1.)) > cap + 1e-9)
                                continue;
                            const int hits = backward(road.tip[i], a, k, best_hits);
                            if (hits < best_hits) {
                                best_hits = hits;
                                best_turn = lean * a0.cross(side).normalized();
                                clear     = hits == 0;
                            }
                        }
                        if (clear)
                            break;
                    }
                    if (best_turn.squaredNorm() > 0.) {
                        turn[i] = best_turn;
                        any     = true;
                        ++leaned;
                    }
                    if (!clear)
                        ++blocked;
                }
                if (!any)
                    continue;
                std::vector<double> s(n, 0.);
                for (size_t i = 1; i < n; ++i)
                    s[i] = s[i - 1] + (road.tip[i] - road.tip[i - 1]).norm();
                std::vector<Vec3d> ramp(n, Vec3d::Zero());
                for (size_t j = 0; j < n; ++j) {
                    const double m = turn[j].norm();
                    if (m <= 0.)
                        continue;
                    for (size_t i = 0; i < n; ++i) {
                        const double left = m - lean_rate * std::abs(s[i] - s[j]);
                        if (left > ramp[i].norm())
                            ramp[i] = turn[j] * (left / m);
                    }
                }
                const double cos_max = std::cos(params.max_tilt);
                for (size_t i = 0; i < n; ++i)
                    if (const double m = ramp[i].norm(); m > 0.) {
                        Vec3d a = (Eigen::AngleAxisd(m, ramp[i] / m) * axes[i].cast<double>()).normalized();
                        if (a.z() < cos_max && a.head<2>().squaredNorm() > 1e-12) {
                            const Vec2d h = a.head<2>().normalized() * std::sin(params.max_tilt);
                            a         = Vec3d(h.x(), h.y(), cos_max);
                        }
                        axes[i] = a.cast<float>();
                    }
            }
        });
        lean_points  = leaned;
        lean_blocked = blocked;
    }

    reaches.assign(pieces.size(), {});
    std::vector<std::vector<Vec3d>> table(pieces.size());
    std::vector<Vec2d> block(pieces.size(), Vec2d::Zero());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, pieces.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t p = range.begin(); p < range.end(); ++p) {
            std::vector<int> &hit  = reaches[p];
            auto              meet = [&](int q) {
                if (q != int(p) && std::find(hit.begin(), hit.end(), q) == hit.end())
                    hit.push_back(q);
            };
            for (int fi : pieces[p].fragments) {
                const Fragment  &f    = fragments[fi];
                const FieldRoad &road = roads[f.road];
                for (size_t i = f.i0; i <= f.i1; i = (i + 2 <= f.i1 || i == f.i1) ? i + 2 : f.i1) {
                    const Vec3d    tip  = road.tip[i];
                    const Vec3d    axis = road.path->tool_axis.empty() ? Vec3d(Vec3d::UnitZ()) : Vec3d(road.path->tool_axis[i].cast<double>());
                    const Matrix3d h    = field_head_frame(params.kinematics, tip, axis).transpose();
                    for (const Vec3d &s : head) {
                        const Vec3d q = tip + h * s;
                        if (q.z() < 0.) {
                            table[p].push_back(tip);
                            break;
                        }
                        if (support_layers > 0) {
                            const auto it = std::lower_bound(support.top.begin(), support.top.end(), q.z());
                            if (it != support.top.end()) {
                                const size_t j = size_t(it - support.top.begin());
                                if (q.z() > support.top[j] - support.height[j]) {
                                    int x, y;
                                    pixel(q, x, y);
                                    if (const int sp = raster[layers + j][size_t(y) * nrx + x]; sp >= 0)
                                        meet(sp);
                                }
                            }
                        }
                        const Vec3d d = field.forward(q);
                        const int   k = layer_of(d.z());
                        if (k < 0)
                            continue;
                        int x, y;
                        pixel(d, x, y);
                        const std::vector<int> &ras = raster[size_t(k)];
                        if (ras.empty() || !near[size_t(k)][size_t(y) * nrx + x])
                            continue;
                        const int r = reach_px;
                        bool      lower = false;
                        for (int b = std::max(0, y - r); b <= std::min(nry - 1, y + r); ++b)
                            for (int a = std::max(0, x - r); a <= std::min(nrx - 1, x + r); ++a)
                                if (const int m = ras[size_t(b) * nrx + a]; m >= 0)
                                    if (!(s.z() < NozzleContact && p < field_pieces && pieces[m].layer <= pieces[p].layer)) {
                                        meet(m);
                                        lower |= p < field_pieces && size_t(m) < field_pieces && pieces[m].layer < pieces[p].layer;
                                    }
                        if (lower)
                            block[p] += (q - tip).head<2>();
                    }
                }
            }
        }
    });
    for (size_t p = 0; p < pieces.size(); ++p) {
        for (int q : reaches[p])
            if (std::find(pieces[p].after.begin(), pieces[p].after.end(), q) == pieces[p].after.end()) {
                pieces[p].after.push_back(q);
                ++out.stats.head_edges;
            }
        if (!table[p].empty()) {
            out.stats.table_poses += table[p].size();
            out.relax_points.insert(out.relax_points.end(), table[p].begin(), table[p].end());
            out.relax_dirs.resize(out.relax_points.size(), Vec3d::Zero());
        }
    }
    for (Piece &piece : pieces)
        for (int q : piece.after)
            ++pieces[size_t(q)].before;
    out.stats.pieces = pieces.size();
    cancel();

    std::map<size_t, std::vector<int>> ready;
    std::vector<int>                   left(pieces.size());
    for (size_t p = 0; p < pieces.size(); ++p) {
        left[p] = pieces[p].before;
        if (left[p] == 0)
            ready[pieces[p].level].push_back(int(p));
    }
    std::vector<char> done(pieces.size(), 0);
    int               logged_sccs = 0;
    order.reserve(pieces.size());
    Vec2d at   = pieces.front().center;
    auto  emit = [&](int p) {
        done[p] = 1;
        order.push_back(p);
        at = pieces[p].center;
        for (int q : pieces[p].after)
            if (--left[q] == 0)
                ready[pieces[q].level].push_back(q);
    };
    while (order.size() < pieces.size()) {
        if (ready.empty()) {
            std::vector<int> rest;
            for (size_t p = 0; p < pieces.size(); ++p)
                if (!done[p])
                    rest.push_back(int(p));
            std::vector<int>  index(pieces.size(), -1), low(pieces.size(), 0), stack;
            std::vector<char> on(pieces.size(), 0);
            int               counter = 0;
            for (int root : rest) {
                if (index[root] >= 0)
                    continue;
                std::vector<std::pair<int, size_t>> call { { root, 0 } };
                index[root] = low[root] = counter++;
                stack.push_back(root);
                on[root] = 1;
                while (!call.empty()) {
                    const int               v    = call.back().first;
                    const std::vector<int> &next = pieces[v].after;
                    if (call.back().second < next.size()) {
                        const int w = next[call.back().second++];
                        if (done[w])
                            continue;
                        if (index[w] < 0) {
                            index[w] = low[w] = counter++;
                            stack.push_back(w);
                            on[w] = 1;
                            call.emplace_back(w, 0);
                        } else if (on[w])
                            low[v] = std::min(low[v], index[w]);
                        continue;
                    }
                    if (low[v] == index[v]) {
                        std::vector<int> scc;
                        int              w;
                        do {
                            w = stack.back();
                            stack.pop_back();
                            on[w] = 0;
                            scc.push_back(w);
                        } while (w != v);
                        if (scc.size() > 1 && ++logged_sccs <= 40) {
                            std::string line = "field order cycle of " + std::to_string(scc.size()) + ":";
                            for (size_t m = 0; m < std::min<size_t>(scc.size(), 6); ++m) {
                                const int       c = scc[m];
                                const Fragment &f = fragments[pieces[c].fragments.front()];
                                const FieldRoad &rd = roads[f.road];
                                const size_t    mid = (f.i0 + f.i1) / 2;
                                const Vec3d     t = rd.tip[mid];
                                const Vec3f     a = rd.path->tool_axis.empty() ? Vec3f::UnitZ() : rd.path->tool_axis[mid];
                                int head_in = 0;
                                for (int q : reaches[c])
                                    head_in += std::find(scc.begin(), scc.end(), q) != scc.end();
                                char buf[256];
                                snprintf(buf, sizeof(buf), " [p%d L%zu %s%s tip %.1f,%.1f,%.1f axis %.2f,%.2f,%.2f heads-in-scc %d blk %.1f,%.1f]", c,
                                         pieces[c].layer, rd.shell ? "shell " : "", c >= int(field_pieces) ? "support" : ExtrusionEntity::role_to_string(rd.path->role()).c_str(),
                                         t.x(), t.y(), t.z(), a.x(), a.y(), a.z(), head_in, block[size_t(c)].x(), block[size_t(c)].y());
                                line += buf;
                            }
                            BOOST_LOG_TRIVIAL(info) << line;
                            if (logged_sccs <= 6) {
                                std::unordered_map<int, int> prev;
                                std::vector<int>             frontier { scc.front() };
                                std::vector<char>            in_scc(pieces.size(), 0);
                                for (int c : scc)
                                    in_scc[size_t(c)] = 1;
                                prev[scc.front()] = -1;
                                int closing = -1;
                                for (size_t f = 0; f < frontier.size() && closing < 0; ++f)
                                    for (int w : pieces[frontier[f]].after) {
                                        if (!in_scc[size_t(w)])
                                            continue;
                                        if (w == scc.front()) {
                                            closing = frontier[f];
                                            break;
                                        }
                                        if (prev.emplace(w, frontier[f]).second)
                                            frontier.push_back(w);
                                    }
                                std::vector<int> path;
                                for (int c = closing; c >= 0; c = prev[c])
                                    path.push_back(c);
                                std::reverse(path.begin(), path.end());
                                path.push_back(scc.front());
                                std::string cyc = "field order cycle path:";
                                for (size_t m = 0; m + 1 < path.size() && m < 12; ++m) {
                                    const int        c = path[m], d = path[m + 1];
                                    const Fragment  &f = fragments[pieces[c].fragments.front()];
                                    const FieldRoad &rd = roads[f.road];
                                    const Vec3d      t = rd.tip[(f.i0 + f.i1) / 2];
                                    const Vec3f      a = rd.path->tool_axis.empty() ? Vec3f::UnitZ() : rd.path->tool_axis[(f.i0 + f.i1) / 2];
                                    const bool       head = std::find(reaches[c].begin(), reaches[c].end(), d) != reaches[c].end();
                                    char buf[200];
                                    snprintf(buf, sizeof(buf), " p%d(L%zu %s%s %.1f,%.1f,%.1f ax %.2f,%.2f,%.2f) -%c->", c, pieces[c].layer,
                                             rd.shell ? "shell " : "", c >= int(field_pieces) ? "support" : ExtrusionEntity::role_to_string(rd.path->role()).c_str(),
                                             t.x(), t.y(), t.z(), a.x(), a.y(), a.z(), head ? 'H' : 'S');
                                    cyc += buf;
                                }
                                BOOST_LOG_TRIVIAL(info) << cyc << " p" << scc.front();
                            }
                        }
                        if (scc.size() > 1)
                            for (int c : scc) {
                                ++out.stats.cycle_pieces;
                                cycle_list.push_back(c);
                                const Fragment &f = fragments[pieces[c].fragments.front()];
                                if (!roads[f.road].shell) {
                                    out.relax_points.push_back(roads[f.road].tip[(f.i0 + f.i1) / 2]);
                                    const Vec2d u = block[size_t(c)];
                                    out.relax_dirs.push_back(u.norm() > 1e-9 ? Vec3d(u.x(), u.y(), 0.).normalized() : Vec3d::Zero());
                                }
                            }
                    }
                    call.pop_back();
                    if (!call.empty())
                        low[call.back().first] = std::min(low[call.back().first], low[v]);
                }
            }
            std::stable_sort(rest.begin(), rest.end(), [&](int a, int b) { return pieces[a].level < pieces[b].level; });
            for (int p : rest)
                emit(p);
            break;
        }
        auto              it   = ready.begin();
        std::vector<int> &list = it->second;
        size_t            best = 0;
        double            bd   = std::numeric_limits<double>::max();
        for (size_t i = 0; i < list.size(); ++i)
            if (const double d = (pieces[list[i]].center - at).squaredNorm(); d < bd) {
                bd   = d;
                best = i;
            }
        const int p = list[best];
        list[best]  = list.back();
        list.pop_back();
        if (list.empty())
            ready.erase(it);
        emit(p);
    }
    cancel();
    out.stats.split_passes = pass;
    if (cycle_list.empty() || pass >= MaxSplit)
        break;
    bool                 split = false;
    std::vector<uint8_t> bumped(refine.size(), 0);
    for (int c : cycle_list) {
        const size_t cc = size_t(column_of[size_t(c)] / SubIds);
        if (!bumped[cc] && refine[cc] < MaxSplit) {
            ++refine[cc];
            bumped[cc] = 1;
            split      = true;
        }
    }
    if (!split)
        break;
    }

    std::vector<int> group_of(pieces.size(), -1);
    for (size_t s = 0; s < order.size();) {
        size_t e = s + 1;
        while (e < order.size() && pieces[order[e]].layer == pieces[order[s]].layer)
            ++e;
        const int g = int(s);
        for (size_t i = s; i < e; ++i)
            group_of[order[i]] = g;
        bool inner = false;
        for (size_t i = s; i < e && !inner; ++i)
            for (int q : reaches[order[i]])
                if (group_of[q] == g) {
                    inner = true;
                    break;
                }
        std::vector<int> frags;
        for (size_t i = s; i < e; ++i)
            frags.insert(frags.end(), pieces[order[i]].fragments.begin(), pieces[order[i]].fragments.end());
        if (!inner)
            std::stable_sort(frags.begin(), frags.end(), [&](int a, int b) {
                return fragments[a].road != fragments[b].road ? fragments[a].road < fragments[b].road : fragments[a].i0 < fragments[b].i0;
            });
        ExtrusionEntityCollection shell;
        for (size_t i = 0; i < frags.size();) {
            const Fragment &f  = fragments[frags[i]];
            size_t          i1 = f.i1, j = i + 1;
            while (j < frags.size() && fragments[frags[j]].road == f.road && fragments[frags[j]].i0 == i1)
                i1 = fragments[frags[j++]].i1;
            const ExtrusionPathConformal &src = *roads[f.road].path;
            if (f.i0 == 0 && i1 + 1 == src.polyline.points.size())
                shell.entities.push_back(roads[f.road].path.release());
            else
                shell.entities.push_back(sub_path(src, f.i0, i1));
            i = j;
        }
        if (!shell.entities.empty())
            out.groups.push_back(std::move(shell));
        s = e;
    }
    {
        std::map<size_t, ExtrusionEntityCollection> tops;
        for (size_t r = 0; r < field_roads; ++r)
            if (roads[r].shell && roads[r].path && roads[r].tip.size() >= 2) {
                tops[roads[r].layer].entities.push_back(roads[r].path.release());
                ++out.stats.shell_roads;
            }
        for (auto &[layer, group] : tops)
            out.groups.push_back(std::move(group));
    }
    for (FieldRoad &r : roads)
        r.path.reset();
    out.stats.groups         = out.groups.size();
    out.stats.leaned_points  = lean_points;
    out.stats.blocked_points = lean_blocked;
    return out;
}

FieldOrder layer_order_field_roads(std::vector<FieldRoad> &roads, const FieldOrderParams &params)
{
    FieldOrder   out;
    const size_t layers = params.layer_top.size();
    out.groups.resize(layers);
    auto point_layer = [&](const FieldRoad &road, size_t i) {
        const size_t k = size_t(std::lower_bound(params.layer_top.begin(), params.layer_top.end(), road.deformed[i].z() - 1e-6) -
                                params.layer_top.begin());
        return std::min(k, layers - 1);
    };
    static const bool support_first = research_switch("NP_NATIVE_SUPPORT_FIRST") != nullptr;
    std::vector<size_t> moved(roads.size(), std::numeric_limits<size_t>::max());
    std::vector<ExtrusionEntitiesPtr> early(layers);
    if (support_first && layers > 0) {
        constexpr double Cell = 0.5, Reach = 4., Lift = 0.3;
        auto key = [](double x, double y) { return (int64_t(std::floor(x / Cell)) << 32) ^ (int64_t(std::floor(y / Cell)) & 0xffffffff); };
        auto is_support = [](const FieldRoad &r) { return r.path && (r.path->role() == erSupportMaterial || r.path->role() == erSupportMaterialInterface); };
        std::unordered_map<int64_t, std::vector<std::pair<double, size_t>>> part;
        for (const FieldRoad &r : roads)
            if (r.path)
                for (size_t i = 0; i < r.tip.size(); ++i)
                    part[key(r.tip[i].x(), r.tip[i].y())].emplace_back(r.tip[i].z(), point_layer(r, i));
        std::vector<size_t> order;
        for (size_t i = 0; i < roads.size(); ++i)
            if (is_support(roads[i]) && roads[i].tip.size() >= 2)
                order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return point_layer(roads[a], 0) < point_layer(roads[b], 0); });
        std::unordered_map<int64_t, std::vector<std::pair<double, size_t>>> placed_support;
        size_t n_moved = 0;
        for (size_t ri : order) {
            const FieldRoad &r   = roads[ri];
            const size_t     own = point_layer(r, 0);
            size_t           g   = own, floor_g = 0;
            for (size_t i = 0; i < r.tip.size(); ++i) {
                const Vec3d &t = r.tip[i];
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy) {
                        const int64_t k = key(t.x() + dx * Cell, t.y() + dy * Cell);
                        if (auto it = part.find(k); it != part.end())
                            for (const auto &[z, l] : it->second)
                                if (l < g && z > t.z() + Lift && z < t.z() + Reach)
                                    g = l;
                        if (auto it = placed_support.find(k); it != placed_support.end())
                            for (const auto &[z, gs] : it->second)
                                if (z < t.z() - 1e-3 && z > t.z() - 1.)
                                    floor_g = std::max(floor_g, gs);
                    }
            }
            g = std::max(g, floor_g);
            for (const Vec3d &t : r.tip)
                placed_support[key(t.x(), t.y())].emplace_back(t.z(), g);
            if (g < own) {
                moved[ri] = g;
                ++n_moved;
            }
        }
        BOOST_LOG_TRIVIAL(info) << "Native support first: " << n_moved << " of " << order.size() << " support roads moved earlier";
    }
    for (size_t ri = 0; ri < roads.size(); ++ri) {
        FieldRoad   &road = roads[ri];
        const size_t n = road.tip.size();
        if (!road.path || n < 2 || layers == 0)
            continue;
        if (moved[ri] < layers) {
            early[moved[ri]].push_back(road.path.release());
            continue;
        }
        if (params.planar_support && (road.path->role() == erSupportMaterial || road.path->role() == erSupportMaterialInterface)) {
            early[std::min(road.layer, layers - 1)].push_back(road.path.release());
            continue;
        }
        std::vector<std::pair<size_t, size_t>> runs;
        for (size_t s = 0; s < n;) {
            size_t e = s + 1;
            while (e < n && point_layer(road, e) == point_layer(road, s))
                ++e;
            if (!runs.empty() && e - s < 3)
                runs.back().second = e;
            else
                runs.emplace_back(s, e);
            s = e;
        }
        if (runs.size() == 1) {
            out.groups[point_layer(road, 0)].entities.push_back(road.path.release());
            continue;
        }
        for (size_t j = 0; j < runs.size(); ++j) {
            const size_t i0 = j == 0 ? 0 : runs[j].first - 1, i1 = runs[j].second - 1;
            if (i1 > i0)
                out.groups[point_layer(road, runs[j].first)].entities.push_back(sub_path(*road.path, i0, i1));
        }
        road.path.reset();
    }
    for (size_t k = 0; k < layers; ++k)
        if (!early[k].empty())
            out.groups[k].entities.insert(out.groups[k].entities.begin(), early[k].begin(), early[k].end());
    for (size_t k = 0; k < out.groups.size(); ++k)
        if (!out.groups[k].entities.empty())
            out.group_layers.push_back(k);
    out.groups.erase(std::remove_if(out.groups.begin(), out.groups.end(), [](const ExtrusionEntityCollection &g) { return g.entities.empty(); }),
                     out.groups.end());
    Point pos;
    Vec3d pos_axis = Vec3d::UnitZ();
    bool  placed = false;
    for (ExtrusionEntityCollection &g : out.groups) {
        ExtrusionEntitiesPtr chained;
        chained.reserve(g.entities.size());
        for (size_t s = 0; s < g.entities.size();) {
            const ExtrusionRole role = g.entities[s]->role();
            auto z_of = [](const ExtrusionEntity *p) { return static_cast<const ExtrusionPathConformal *>(p)->polyline.points.front().z(); };
            const bool by_z = params.planar_support && (role == erSupportMaterial || role == erSupportMaterialInterface);
            size_t e = s + 1;
            while (e < g.entities.size() && g.entities[e]->role() == role && (!by_z || z_of(g.entities[e]) == z_of(g.entities[s])))
                ++e;
            if (is_perimeter(role) || !placed) {
                chained.insert(chained.end(), g.entities.begin() + s, g.entities.begin() + e);
            } else {
                std::vector<ExtrusionEntity *> left(g.entities.begin() + s, g.entities.begin() + e);
                std::vector<bool>              level(left.size());
                for (size_t i = 0; i < left.size(); ++i) {
                    coord_t lo = std::numeric_limits<coord_t>::max(), hi = std::numeric_limits<coord_t>::lowest();
                    for (const Point3 &p : static_cast<const ExtrusionPathConformal *>(left[i])->polyline.points)
                        lo = std::min(lo, p.z()), hi = std::max(hi, p.z());
                    level[i] = unscale<double>(hi - lo) < 0.1 && role != erTopSolidInfill && role != erIroning;
                }
                static const double order_axis = [] { const char *v = research_switch("NP_NATIVE_ORDER_AXIS"); return v ? std::max(0., std::atof(v)) : 0.; }();
                const auto axis_at = [](const ExtrusionEntity *ee, bool last) {
                    const auto &a = static_cast<const ExtrusionPathConformal *>(ee)->tool_axis;
                    return a.empty() ? Vec3d(Vec3d::UnitZ()) : Vec3d((last ? a.back() : a.front()).cast<double>().normalized());
                };
                const auto cost = [&](double d2, const Vec3d &from, const Vec3d &to) {
                    if (order_axis <= 0.)
                        return d2;
                    const double turn = Geometry::rad2deg(std::acos(std::clamp(from.dot(to), -1., 1.)));
                    return sqr(unscale<double>(std::sqrt(d2)) + order_axis * turn);
                };
                while (!left.empty()) {
                    size_t best = 0;
                    bool   flip = false;
                    double best_d = std::numeric_limits<double>::infinity();
                    const Vec3d &axis = pos_axis;
                    for (size_t i = 0; i < left.size(); ++i) {
                        if (const double d0 = cost((left[i]->first_point() - pos).cast<double>().squaredNorm(), axis, order_axis > 0. ? axis_at(left[i], false) : axis); d0 < best_d)
                            best_d = d0, best = i, flip = false;
                        if (level[i])
                            if (const double d1 = cost((left[i]->last_point() - pos).cast<double>().squaredNorm(), axis, order_axis > 0. ? axis_at(left[i], true) : axis); d1 < best_d)
                                best_d = d1, best = i, flip = true;
                    }
                    if (flip)
                        left[best]->reverse();
                    chained.push_back(left[best]);
                    pos = left[best]->last_point();
                    if (order_axis > 0.)
                        pos_axis = axis_at(left[best], true);
                    left.erase(left.begin() + best);
                    level.erase(level.begin() + best);
                }
            }
            pos    = chained.back()->last_point();
            if (const auto &a = static_cast<const ExtrusionPathConformal *>(chained.back())->tool_axis; !a.empty())
                pos_axis = a.back().cast<double>().normalized();
            placed = true;
            s      = e;
        }
        const double axis_blend = params.axis_blend;
        if (axis_blend > 0.)
            for (size_t i = 1; i < chained.size(); ++i) {
                auto *a = static_cast<ExtrusionPathConformal *>(chained[i - 1]);
                auto *b = static_cast<ExtrusionPathConformal *>(chained[i]);
                if (a->tool_axis.size() != a->polyline.points.size() || b->tool_axis.size() != b->polyline.points.size() || a->tool_axis.empty() ||
                    b->tool_axis.empty() || (b->polyline.points.front() - a->polyline.points.back()).cast<double>().norm() > scale_(3.))
                    continue;
                const Vec3d from = a->tool_axis.back().cast<double>().normalized(), to = b->tool_axis.front().cast<double>().normalized();
                const Vec3d mid  = Eigen::Quaterniond::Identity().slerp(0.5, Eigen::Quaterniond::FromTwoVectors(from, to)) * from;
                auto        turn = [&](ExtrusionPathConformal &p, bool at_end) {
                    const size_t n = p.polyline.points.size();
                    double       s = 0.;
                    for (size_t k = 0; k < n; ++k) {
                        const size_t j = at_end ? n - 1 - k : k;
                        if (k > 0)
                            s += unscale<double>((p.polyline.points[j] - p.polyline.points[at_end ? j + 1 : j - 1]).cast<double>().norm());
                        if (s >= axis_blend)
                            break;
                        const Vec3d ax = p.tool_axis[j].cast<double>().normalized();
                        p.tool_axis[j] = (Eigen::Quaterniond::Identity().slerp(1. - s / axis_blend, Eigen::Quaterniond::FromTwoVectors(ax, mid)) * ax).cast<float>();
                    }
                };
                turn(*a, true);
                turn(*b, false);
            }
        g.entities = std::move(chained);
        g.no_sort  = true;
    }
    out.stats.groups = out.stats.layers = out.groups.size();
    return out;
}

double wall_floating_mm(const std::vector<ExPolygons> &slices, const std::vector<double> &heights, int wall_loops, double width)
{
    std::vector<double> per_layer(slices.size(), 0.);
    tbb::parallel_for(tbb::blocked_range<size_t>(1, std::max<size_t>(1, slices.size())), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++l) {
            if (slices[l].empty())
                continue;
            const ExPolygons below = offset_ex(slices[l - 1], float(scale_(0.5 * width + heights[l])));
            for (int i = 0; i < std::max(1, wall_loops); ++i)
                for (const Polyline &road : diff_pl(to_polylines(offset_ex(slices[l], -float(scale_((i + 0.5) * width)))), below))
                    per_layer[l] += unscale<double>(road.length());
        }
    });
    double mm = 0.;
    for (double v : per_layer)
        mm += v;
    return mm;
}

}
