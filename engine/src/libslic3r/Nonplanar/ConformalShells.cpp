#include "ConformalShells.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Nonplanar/FlowMod.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include <math.h>

#include "AdaptiveBands.hpp"
#include "AutoMode.hpp"
#include "BeadModel.hpp"
#include "ConformalIroning.hpp"
#include "HeadEnvelope.hpp"
#include "HeightField.hpp"
#include "LayerSchedule.hpp"
#include "PatchTools.hpp"
#include "Repair.hpp"
#include "Report.hpp"
#include "Seams.hpp"
#include "ShellPaths.hpp"
#include "ShellBase.hpp"
#include "SpiralTop.hpp"
#include "StrengthRoads.hpp"
#include "SurfaceClip.hpp"

#include "libslic3r/Arachne/WallToolPaths.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLA/IndexedMesh.hpp"
#include "libslic3r/ShortestPath.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_reduce.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cfloat>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r::Nonplanar {

namespace {

constexpr float Inf = std::numeric_limits<float>::infinity();

struct ShellSettings
{
    int                  region_id = -1;
    int                  shells = 0;
    int                  top_shells = 0;
    int                  sparse = 0;
    double               sparse_density = 0.;
    double               solid_angle = 0.;
    double               bottom_keep = 0.;
    double               shell_height = 0.;
    double               max_slope = 0.;
    double               max_error = 0.;
    double               min_area = 0.;
    double               pitch = 0.2;
    double               max_patch_height = 0.;
    bool                 banded = false;
    bool                 adaptive = false;
};

ShellSettings shell_settings(const PrintObject &object, size_t region_id)
{
    ShellSettings s;
    const PrintRegionConfig &cfg = object.printing_region(region_id).config();
    if (!top_shells_enabled(cfg, object.config()))
        return s;
    s.region_id        = int(region_id);
    s.shells           = cfg.nonplanar_top_shells.value;
    s.shell_height     = cfg.nonplanar_shell_layer_height.value > 0. ? cfg.nonplanar_shell_layer_height.value :
                                                                        automatic_shell_height(object);
    s.max_slope        = Geometry::deg2rad(cfg.nonplanar_max_slope.value);
    s.max_error        = cfg.nonplanar_max_surface_error.value;
    s.min_area         = cfg.nonplanar_min_patch_area.value;
    s.pitch            = std::max(0.05, object.config().nonplanar_surface_resolution.value);
    s.max_patch_height = effective_patch_height_cap(object);
    s.banded           = effective_fallback(cfg, object.config()) == NonplanarFallback::None;
    if (object.config().nonplanar_mode.value == NonplanarMode::Auto) {
        const AutoObjectSettings a = auto_object_settings(object, object.printing_region(region_id), s.shell_height);
        s.shells           = a.clip_shells;
        s.max_slope        = 0.;
        s.max_error        = a.max_error;
        s.min_area         = a.min_area;
        s.max_patch_height = a.max_patch_height;
    } else if (s.shells < 0)
        s.shells = AutoMinShells;
    if (s.banded)
        s.shells = std::max(s.shells, AutoMinShells);
    s.adaptive         = s.banded && cfg.nonplanar_adaptive_bands.value;
    s.top_shells       = s.shells;
    if (s.banded && infill_depth(cfg, object) > 0.) {
        const int top_solid = planar_top_layers(cfg, s.shell_height);
        s.sparse_density    = std::min(1., 0.01 * cfg.sparse_infill_density.value);
        if (s.sparse_density > 0.)
            s.sparse = int(std::lround(infill_depth(cfg, object) / s.shell_height));
        s.shells      = std::max(s.shells + (s.sparse > 0 ? 1 : 0), top_solid) + s.sparse;
        s.solid_angle = Geometry::deg2rad(cfg.infill_direction.value);
        s.bottom_keep = std::max(cfg.bottom_shell_layers.value * object.config().layer_height.value, cfg.bottom_shell_thickness.value);
    }
    return s;
}

ExPolygons candidate_top_area(const PrintObject &object, const std::vector<size_t> &region_ids)
{
    const auto &layers = object.layers();
    std::vector<Polygons> tops(layers.size());
    for (size_t i = 0; i < layers.size(); ++i)
        for (const LayerRegion *layerm : layers[i]->regions())
            if (std::any_of(region_ids.begin(), region_ids.end(),
                            [&](size_t id) { return &layerm->region() == &object.printing_region(id); }))
                for (const Surface &surface : layerm->slices.surfaces)
                    if (surface.surface_type == stTop)
                        polygons_append(tops[i], to_polygons(surface.expolygon));
    const float seam   = float(scale_(0.01));
    size_t      points = 0;
    for (const Polygons &layer : tops)
        points += count_points(layer);
    if (points < TreeUnionPoints) {
        Polygons all;
        for (Polygons &layer : tops)
            polygons_append(all, std::move(layer));
        return offset_ex(union_ex(offset(all, seam)), -seam);
    }
    return offset_ex(tbb::parallel_reduce(
        tbb::blocked_range<size_t>(0, tops.size(), 8), ExPolygons(),
        [&](const tbb::blocked_range<size_t> &range, ExPolygons acc) {
            for (size_t i = range.begin(); i < range.end(); ++i)
                if (!tops[i].empty())
                    acc = union_ex(acc, union_ex(offset(tops[i], seam)));
            return acc;
        },
        [](const ExPolygons &a, const ExPolygons &b) { return union_ex(a, b); }), -seam);
}

constexpr size_t ContourLoops = 8;
constexpr double FlatSlope = 0.0175;
constexpr int ContourRefine = 4;
constexpr double LengthwiseMaxLines = 4.;
constexpr double LengthwiseMinAspect = 4.;

void split_by_colour(ConformalResult &result, std::vector<HeightField> &planar_tops, std::vector<HeightField> &stack_layers,
                     std::vector<AdaptiveStack> &stacks, std::vector<ExPolygons> &shell_areas,
                     std::vector<ExPolygons> &fill_areas, std::vector<Point> &seams, std::vector<Polyline> &seam_lines,
                     const TopColours &colours, const std::function<int(const Vec3d &)> &colour)
{
    std::vector<HeightField> tops, depths;
    std::vector<AdaptiveStack> new_stacks;
    std::vector<ExPolygons>  areas, fills;
    std::vector<Point>       new_seams;
    std::vector<Polyline>    lines;
    const std::vector<std::pair<size_t, size_t>> sources = split_by_colour(result, colours, colour);
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto [idx, c] = sources[i];
        const ExPolygons own = colour ? union_ex(colours.areas[c], result.patches[i].footprint) : colours.areas[c];
        tops.emplace_back(planar_tops[idx]);
        depths.emplace_back(stack_layers[idx]);
        if (!stacks.empty())
            new_stacks.emplace_back(stacks[idx]);
        areas.emplace_back(intersection_ex(shell_areas[idx], own));
        fills.emplace_back(intersection_ex(fill_areas[idx], own));
        new_seams.emplace_back(seams[idx]);
        lines.emplace_back(seam_lines[idx]);
    }
    planar_tops = std::move(tops);
    stack_layers = std::move(depths);
    stacks      = std::move(new_stacks);
    shell_areas = std::move(areas);
    fill_areas  = std::move(fills);
    seams       = std::move(new_seams);
    seam_lines  = std::move(lines);
}

Polygons width_cover(const WidthRoads &roads)
{
    Polygons out;
    for (size_t i = 0; i < roads.polylines.size(); ++i) {
        const Points             &pts = roads.polylines[i].points;
        const std::vector<float> &w   = roads.widths[i];
        for (size_t a = 0; a + 1 < pts.size();) {
            size_t b     = a + 1;
            float  w_min = std::min(w[a], w[b]);
            while (b + 1 < pts.size() && std::abs(w[b + 1] - w[a]) < 0.1f * w[a])
                w_min = std::min(w_min, w[++b]);
            Polygons piece = offset(Polyline(Points(pts.begin() + a, pts.begin() + b + 1)), float(scale_(0.5 * w_min)), jtRound, scale_(0.01),
                                    etOpenRound);
            for (Polygon &poly : piece)
                out.emplace_back(std::move(poly));
            a = b;
        }
    }
    return out;
}

Arachne::WallToolPathsParams arachne_params(const Flow &flow)
{
    Arachne::WallToolPathsParams params;
    params.min_bead_width                   = float(0.5 * flow.width());
    params.min_feature_size                 = float(0.25 * flow.nozzle_diameter());
    params.wall_transition_length           = float(3. * flow.nozzle_diameter());
    params.wall_transition_angle            = 10.f;
    params.wall_transition_filter_deviation = float(flow.spacing());
    params.wall_distribution_count          = 1;
    return params;
}

struct ArachneLoops
{
    Polygons                                       outline;
    std::unique_ptr<Arachne::WallToolPaths>          generator;
    const std::vector<Arachne::VariableWidthLines> *paths = nullptr;
    const Polygons                               *inner = nullptr;
};

std::unique_ptr<ArachneLoops> arachne_paths(const ExPolygon &piece, const Flow &flow, double layer_height, size_t max_loops)
{
    const float eps     = float(scale_(0.005));
    auto out = std::make_unique<ArachneLoops>();
    Polygons &outline = out->outline;
    outline = offset(offset_ex(expolygons_simplify({ piece }, scale_(0.01)), -eps), eps);
    if (outline.empty())
        return nullptr;
    const coord_t     bead  = coord_t(scale_(flow.spacing()));
    const BoundingBox bb    = get_extents(outline);
    const size_t      loops = max_loops > 0 ? max_loops : size_t(std::max(bb.size().x(), bb.size().y()) / std::max<coord_t>(bead, 1)) + 1;
    out->generator = std::make_unique<Arachne::WallToolPaths>(outline, bead, bead, loops, 0, layer_height, arachne_params(flow));
    out->paths = &out->generator->getToolPaths();
    if (max_loops > 0)
        out->inner = &out->generator->getInnerContour();
    return out;
}

class ArachneCache
{
    using Key = std::tuple<float, float, float, double, size_t, std::vector<Points>>;
    struct Entry {
        std::once_flag                ready;
        std::unique_ptr<ArachneLoops> paths;
    };
    std::mutex                             m_mutex;
    std::map<Key, std::shared_ptr<Entry>>    m_entries;

public:
    std::shared_ptr<const ArachneLoops> get(const ExPolygon &piece, const Flow &flow, double height, size_t max_loops)
    {
        std::vector<Points> shape{ piece.contour.points };
        for (const Polygon &hole : piece.holes)
            shape.emplace_back(hole.points);
        Key key(flow.width(), flow.spacing(), flow.nozzle_diameter(), height, max_loops, std::move(shape));
        std::shared_ptr<Entry> entry;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_entries.find(key);
            if (it != m_entries.end())
                entry = it->second;
            else {
                entry = std::make_shared<Entry>();
                m_entries.emplace(std::move(key), entry);
            }
        }
        std::call_once(entry->ready, [&] { entry->paths = arachne_paths(piece, flow, height, max_loops); });
        return entry->paths ? std::shared_ptr<const ArachneLoops>(entry, entry->paths.get()) : nullptr;
    }
};

void arachne_roads(const ExPolygon &piece, const ArachneLoops *paths, const Flow &flow, double layer_height, size_t max_loops, WidthRoads &out,
                   Polygons *inner = nullptr, const Point *seam = nullptr)
{
    if (paths == nullptr)
        return;
    const Arachne::WallToolPathsParams params = arachne_params(flow);

    std::vector<const Arachne::ExtrusionLine *> lines;
    for (const Arachne::VariableWidthLines &inset : *paths->paths)
        for (const Arachne::ExtrusionLine &line : inset)
            if (line.junctions.size() >= 2)
                lines.emplace_back(&line);
    std::stable_sort(lines.begin(), lines.end(), [](const auto *a, const auto *b) { return a->inset_idx > b->inset_idx; });
    const float      to_width = float(flow.width() - flow.spacing());
    const float      min_spacing = std::max(params.min_feature_size, float(layer_height));
    const double     link_max = 2. * flow.spacing();
    const ExPolygons link_area{ piece };
    std::optional<ExPolygons> link_grown;
    std::optional<Point> last;
    while (!lines.empty()) {
        const std::optional<Point> near = last ? last : seam != nullptr ? std::optional<Point>(*seam) : std::nullopt;
        size_t best = 0;
        double best_d = std::numeric_limits<double>::max();
        for (size_t i = 0; i < lines.size() && lines[i]->inset_idx == lines.front()->inset_idx; ++i) {
            const auto &js = lines[i]->junctions;
            double      d  = 0.;
            if (near) {
                d = std::min((js.front().p - *near).cast<double>().squaredNorm(), (js.back().p - *near).cast<double>().squaredNorm());
                if (lines[i]->is_closed)
                    for (size_t j = 0; j + 1 < js.size(); ++j)
                        d = std::min(d, Line(js[j].p, js[j + 1].p).distance_to_squared(*near));
            }
            if (d < best_d) {
                best_d = d;
                best   = i;
            }
        }
        const Arachne::ExtrusionLine &line = *lines[best];
        lines.erase(lines.begin() + best);
        Polyline           pl;
        std::vector<float> w;
        for (const Arachne::ExtrusionJunction &j : line.junctions) {
            pl.points.emplace_back(j.p);
            w.emplace_back(std::max(min_spacing, float(unscale<double>(j.w))) + to_width);
        }
        if (line.is_closed) {
            auto foot_of = [&pl](const Point &q, size_t &start, Point &foot) {
                double best_seg = std::numeric_limits<double>::max();
                for (size_t j = 0; j + 1 < pl.points.size(); ++j) {
                    Point        f;
                    const double d = Line(pl.points[j], pl.points[j + 1]).distance_to_squared(q, &f);
                    if (d < best_seg) {
                        best_seg = d;
                        start    = j + 1;
                        foot     = f;
                    }
                }
            };
            size_t start = 0;
            Point  foot;
            if (seam != nullptr)
                foot_of(*seam, start, foot);
            if (last && (seam == nullptr || (foot - *last).cast<double>().norm() > scale_(link_max)))
                foot_of(*last, start, foot);
            if (start > 0) {
                const double seg = (pl.points[start] - pl.points[start - 1]).cast<double>().norm();
                if (const double t = seg > 0. ? (foot - pl.points[start - 1]).cast<double>().norm() / seg : 0.; t > 0.01 && t < 0.99) {
                    pl.points.insert(pl.points.begin() + start, foot);
                    w.insert(w.begin() + start, float(w[start - 1] + t * (w[start] - w[start - 1])));
                } else if (t <= 0.01)
                    --start;
            }
            pl.points.pop_back();
            w.pop_back();
            start %= pl.points.size();
            std::rotate(pl.points.begin(), pl.points.begin() + start, pl.points.end());
            std::rotate(w.begin(), w.begin() + start, w.end());
            pl.points.emplace_back(pl.points.front());
            w.emplace_back(w.front());
            double cut = scale_(flow.spacing());
            while (pl.points.size() > 2 && cut > 0.) {
                const double seg = (pl.points.back() - pl.points[pl.points.size() - 2]).cast<double>().norm();
                if (seg > cut) {
                    const Vec2d a = pl.points[pl.points.size() - 2].cast<double>(), b = pl.points.back().cast<double>();
                    pl.points.back() = (b + (a - b) * (cut / seg)).cast<coord_t>();
                    break;
                }
                cut -= seg;
                pl.points.pop_back();
                w.pop_back();
            }
            {
                Vec2d c = Vec2d::Zero();
                for (size_t j = 0; j < pl.points.size(); ++j)
                    c += unscale(pl.points[j]);
                c /= double(pl.points.size());
                double r = 0.;
                for (const Point &q : pl.points)
                    r = std::max(r, (unscale(q) - c).norm());
                bool summit = r < 1.25 * flow.width() && !(last && (unscale(*last) - c).norm() < r);
                for (size_t i = 0; summit && i < lines.size(); ++i)
                    for (const Arachne::ExtrusionJunction &j : lines[i]->junctions)
                        summit = summit && (unscale(j.p) - c).norm() >= r;
                if (summit) {
                    const Vec2d  s0 = unscale(pl.points.front()) - c;
                    const double a0 = std::atan2(s0.y(), s0.x());
                    const double rc = std::max(r - flow.width(), 0.5 * r);
                    Points       pre{ Point::new_scale(c.x(), c.y()) };
                    for (int k = 0; k <= 6; ++k) {
                        const double a = a0 + 2. * PI * k / 6.;
                        pre.emplace_back(Point::new_scale(c.x() + rc * std::cos(a), c.y() + rc * std::sin(a)));
                    }
                    pl.points.insert(pl.points.begin(), pre.begin(), pre.end());
                    w.insert(w.begin(), pre.size(), float(flow.width()));
                }
            }
        } else if (near && (pl.points.back() - *near).cast<double>().squaredNorm() < (pl.points.front() - *near).cast<double>().squaredNorm()) {
            pl.reverse();
            std::reverse(w.begin(), w.end());
        }
        if (last && !out.polylines.empty() && (pl.points.front() - *last).cast<double>().norm() <= scale_(link_max) &&
            diff_pl(Polylines{ Polyline(*last, pl.points.front()) },
                    link_grown ? *link_grown : link_grown.emplace(offset_ex(link_area, float(scale_(0.25 * flow.spacing()))))).empty()) {
            append(out.polylines.back().points, std::move(pl.points));
            append(out.widths.back(), std::move(w));
        } else {
            out.polylines.emplace_back(std::move(pl));
            out.widths.emplace_back(std::move(w));
        }
        last = out.polylines.back().points.back();
    }
    if (inner != nullptr && max_loops > 0)
        append(*inner, *paths->inner);
}

void arachne_roads(const ExPolygon &piece, const Flow &flow, double layer_height, size_t max_loops, WidthRoads &out, Polygons *inner = nullptr,
                   const Point *seam = nullptr, ArachneCache *cache = nullptr)
{
    std::shared_ptr<const ArachneLoops> loops;
    if (cache != nullptr)
        loops = cache->get(piece, flow, layer_height, max_loops);
    else
        loops = arachne_paths(piece, flow, layer_height, max_loops);
    arachne_roads(piece, loops.get(), flow, layer_height, max_loops, out, inner, seam);
}

HeightField contour_samples(const HeightField &surface)
{
    HeightField fine(BoundingBoxf(surface.origin(), surface.node(surface.nx() - 1, surface.ny() - 1)), surface.pitch() / ContourRefine);
    for (int iy = 0; iy < fine.ny(); ++iy)
        for (int ix = 0; ix < fine.nx(); ++ix)
            if (const float s = surface.sample(fine.node(ix, iy)); !std::isnan(s) && !spans_wall(surface, fine.node(ix, iy)))
                fine.at(ix, iy) = s;
    return fine;
}

constexpr double FlatMiddleSlope = 0.035;
constexpr double FlatBlendSlope = 0.0175;
constexpr double BlendFlatShare = 0.3;
constexpr double BlendCoreRoads = 6.;
constexpr double FlatGroundWidth = 4.;

static bool flat_middle(const HeightField &local)
{
    double sum = 0.;
    size_t n   = 0;
    for (int iy = 0; iy < local.ny(); ++iy)
        for (int ix = 0; ix < local.nx(); ++ix)
            if (!std::isnan(local.at(ix, iy)))
                if (const Vec2d g = local.gradient(local.node(ix, iy)); g.allFinite())
                    sum += g.squaredNorm(), ++n;
    return n > 0 && sum < double(n) * FlatMiddleSlope * FlatMiddleSlope;
}

void contour_fill(const HeightField &surface, const ExPolygons &area, double drop, double strip_height, double strip_offset, const Flow &flow, double layer_height,
                  double angle, bool monotonic, double z, WidthRoads &roads, std::vector<Polylines> &lines, const Point *seam = nullptr,
                  const HeightField *samples = nullptr, ArachneCache *cache = nullptr,
                  double flat_angle = std::numeric_limits<double>::quiet_NaN())
{
    const HeightField owned = samples == nullptr ? contour_samples(surface) : HeightField();
    const HeightField &fine = samples == nullptr ? owned : *samples;
    RunGroups strips;
    for (int iy = 0; iy < fine.ny(); ++iy)
        for (int ix = 0; ix < fine.nx(); ++ix)
            if (const float s = fine.at(ix, iy); !std::isnan(s))
                strips.add(int(std::floor((s - drop - strip_offset) / strip_height)), ix, iy);
    ExPolygons flat_ground;
    if (!std::isnan(flat_angle)) {
        Mask flat(surface.data().size(), 0);
        for (int iy = 0; iy < surface.ny(); ++iy)
            for (int ix = 0; ix < surface.nx(); ++ix)
                if (!std::isnan(surface.at(ix, iy)))
                    if (const Vec2d g = surface.gradient(surface.node(ix, iy)); g.allFinite() && g.norm() < FlatMiddleSlope)
                        flat[size_t(iy) * surface.nx() + ix] = 1;
        flat_ground = intersection_ex(opening_ex(mask_to_expolygons(flat, surface), float(scale_(0.5 * FlatGroundWidth))), area);
    }
    const ExPolygons  sloped  = flat_ground.empty() ? ExPolygons() : diff_ex(area, flat_ground);
    const ExPolygons &stripped = flat_ground.empty() ? area : sloped;
    ExPolygons pieces;
    for (const auto &[strip, runs] : strips.groups())
        append(pieces, expolygons_simplify(intersection_ex(stripped, runs_to_expolygons(runs, fine)), scale_(0.5 * fine.pitch())));
    if (!flat_ground.empty()) {
        const ExPolygons lined = intersection_ex(offset_ex(flat_ground, float(scale_(0.25 * flow.spacing()))), area);
        if (Polylines pl = fill_solid(lined, monotonic ? ipMonotonic : ipRectilinear, flow.spacing(), flat_angle, monotonic, flow, z); !pl.empty())
            append(lines, link_paths(chain_polylines(std::move(pl)), offset_ex(flat_ground, float(scale_(0.5 * flow.spacing()))),
                                     flow.spacing(), MaxGlide));
    }
    std::vector<std::shared_ptr<const ArachneLoops>> paths(pieces.size());
    std::vector<std::vector<Polylines>>        middles(pieces.size());
    std::vector<size_t>                        max_loops(pieces.size(), ContourLoops);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, pieces.size(), 1), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            const ExPolygon &piece = pieces[i];
            if (cache != nullptr)
                paths[i] = cache->get(piece, flow, layer_height, ContourLoops);
            else
                paths[i] = arachne_paths(piece, flow, layer_height, ContourLoops);
            if (!paths[i])
                continue;
            const ExPolygons middle = intersection_ex(union_ex(*paths[i]->inner), ExPolygons{ piece });
            if (middle.empty())
                continue;
            double a = angle;
            bool   ringed = false;
            {
                const BoundingBox bb  = get_extents(middle);
                const Vec2d       lo  = (unscale(bb.min) - surface.origin()) / surface.pitch();
                const Vec2d       hi  = (unscale(bb.max) - surface.origin()) / surface.pitch();
                const int         ix0 = std::clamp(int(std::floor(lo.x())), 0, surface.nx() - 1), iy0 = std::clamp(int(std::floor(lo.y())), 0, surface.ny() - 1);
                const int         ix1 = std::clamp(int(std::ceil(hi.x())), 0, surface.nx() - 1), iy1 = std::clamp(int(std::ceil(hi.y())), 0, surface.ny() - 1);
                HeightField       local  = crop(surface, ix0, iy0, ix1, iy1);
                const Mask        inside = rasterize(middle, local);
                for (size_t n = 0; n < inside.size(); ++n)
                    if (!inside[n])
                        local.data()[n] = HeightField::NoData;
                if (std::any_of(middle.begin(), middle.end(), [](const ExPolygon &e) { return !e.holes.empty(); })) {
                    double slope = 0.;
                    size_t n     = 0;
                    for (int iy = 0; iy < local.ny(); ++iy)
                        for (int ix = 0; ix < local.nx(); ++ix)
                            if (const Vec2d g = local.gradient(local.node(ix, iy)); g.allFinite())
                                slope += g.norm(), ++n;
                    double unused = angle;
                    ringed = n > 0 && (slope / double(n) < FlatSlope || !contour_line_angle(local, unused));
                }
                if (!ringed) {
                    if (std::isnan(flat_angle) || !flat_middle(local))
                        contour_line_angle(local, a, 0.5);
                    else
                        a = flat_angle;
                }
            }
            if (ringed) {
                paths[i]     = arachne_paths(piece, flow, layer_height, 0);
                max_loops[i] = 0;
                continue;
            }
            const ExPolygons lined = intersection_ex(offset_ex(middle, float(scale_(0.25 * flow.spacing()))), ExPolygons{ piece });
            Polylines pl = fill_solid(lined, monotonic ? ipMonotonic : ipRectilinear, flow.spacing(), a, monotonic, flow, z);
            pl.erase(std::remove_if(pl.begin(), pl.end(), [&middle](const Polyline &l) { return intersection_pl(Polylines{ l }, middle).empty(); }),
                     pl.end());
            if (!pl.empty())
                middles[i] = link_paths(chain_polylines(std::move(pl)), offset_ex(middle, float(scale_(0.5 * flow.spacing()))), flow.spacing(),
                                        MaxGlide);
        }
    });
    for (size_t i = 0; i < pieces.size(); ++i) {
        arachne_roads(pieces[i], paths[i].get(), flow, layer_height, max_loops[i], roads, nullptr, seam);
        append(lines, std::move(middles[i]));
    }
}

ExPolygons lengthwise_strips(const ExPolygons &area, double line_width)
{
    ExPolygons out;
    for (ExPolygon &narrow : diff_ex(area, opening_ex(area, float(scale_(0.5 * LengthwiseMaxLines * line_width))))) {
        const double a    = narrow.area() * SCALING_FACTOR * SCALING_FACTOR;
        const double p    = narrow.contour.length() * SCALING_FACTOR;
        const double disc = 0.0625 * p * p - a;
        if (disc <= 0.)
            continue;
        const double l = 0.25 * p + std::sqrt(disc), w = std::max(0.25 * p - std::sqrt(disc), line_width);
        if (l >= LengthwiseMinAspect * w)
            out.emplace_back(std::move(narrow));
    }
    return out;
}

Polylines sparse_strip_fill(const HeightField &surface, const ExPolygons &area, double drop, double strip_height, double spacing,
                            const Point &start, double shift = 0.)
{
    RunGroups strips;
    for (size_t n = 0; n < surface.data().size(); ++n)
        if (const float s = surface.data()[n]; !std::isnan(s))
            strips.add(int(std::floor((s - drop) / strip_height)), int(n % surface.nx()), int(n / surface.nx()));
    Polylines out;
    for (const auto &[strip, runs] : strips.groups())
        for (const ExPolygon &piece : intersection_ex(area, runs_to_expolygons(runs, surface)))
            for (double inset = 0.5 * spacing;; inset += inset == 0.5 * spacing ? spacing - shift : spacing) {
                const Polygons rings = to_polygons(offset_ex(ExPolygons{ piece }, -float(scale_(inset))));
                if (rings.empty())
                    break;
                for (const Polygon &ring : rings)
                    if (ring.size() >= 3)
                        out.emplace_back(ring.split_at_index(ring.closest_point_index(start)));
            }
    return out;
}

bool ct1_climb_requested()
{
    static const bool on = [] {
        const char *v = research_switch("NP_CT1_CLIMB");
        return v != nullptr && *v != 0 && *v != '0';
    }();
    return on;
}

bool ct1_strips_climb()
{
    static const bool on = [] {
        const char *v = research_switch("NP_CT1_CLIMB");
        return v != nullptr && std::atoi(v) >= 2;
    }();
    return on;
}

double ct1_rim_mm()
{
    static const double rim = [] {
        const char *v = research_switch("NP_CT1_RIM");
        return v != nullptr ? std::max(0., std::atof(v)) : 0.;
    }();
    return rim;
}

double ct1_base_gap_mm()
{
    static const double gap = [] {
        const char *v = research_switch("NP_CT1_BASE_GAP");
        return v != nullptr ? std::max(0., std::atof(v)) : 0.;
    }();
    return gap;
}

double mean_height(const HeightField &surface, const Polyline &pl)
{
    double sum = 0.;
    int    n   = 0;
    for (const Point &p : pl.points)
        if (const float s = surface.sample(unscale(p)); !std::isnan(s))
            sum += s, ++n;
    return n > 0 ? sum / n : std::numeric_limits<double>::infinity();
}

std::vector<Polylines> climbing_lines(const HeightField &surface, const ExPolygons &area, const Flow &flow, double z, const Point &start)
{
    Vec2d g = Vec2d::Zero();
    for (int iy = 0; iy < surface.ny(); ++iy)
        for (int ix = 0; ix < surface.nx(); ++ix)
            if (!std::isnan(surface.at(ix, iy)))
                if (const Vec2d d = surface.gradient(surface.node(ix, iy)); d.allFinite())
                    g += d;
    ShellFill along;
    along.angle   = g.squaredNorm() > 0. ? std::atan2(g.y(), g.x()) + 0.5 * PI : 0.;
    along.outline = false;
    static const bool longwise = [] {
        const char *v = research_switch("NP_CT1_LONG");
        return v != nullptr && *v != 0 && *v != '0';
    }();
    if (longwise) {
        Vec2d  mean = Vec2d::Zero();
        size_t n    = 0;
        for (int iy = 0; iy < surface.ny(); ++iy)
            for (int ix = 0; ix < surface.nx(); ++ix)
                if (!std::isnan(surface.at(ix, iy)))
                    mean += surface.node(ix, iy), ++n;
        if (n > 2) {
            mean /= double(n);
            double sxx = 0., sxy = 0., syy = 0.;
            for (int iy = 0; iy < surface.ny(); ++iy)
                for (int ix = 0; ix < surface.nx(); ++ix)
                    if (!std::isnan(surface.at(ix, iy))) {
                        const Vec2d d = surface.node(ix, iy) - mean;
                        sxx += d.x() * d.x(), sxy += d.x() * d.y(), syy += d.y() * d.y();
                    }
            const double tr = sxx + syy, det = sxx * syy - sxy * sxy, disc = std::sqrt(std::max(0., 0.25 * tr * tr - det));
            const double l1 = 0.5 * tr + disc, l2 = 0.5 * tr - disc;
            if (l2 > 0. && l1 > 2.5 * 2.5 * l2)
                along.angle = 0.5 * std::atan2(2. * sxy, sxx - syy);
        }
    }
    static const bool with_outline = [] {
        const char *v = research_switch("NP_CT1_OUTLINE");
        return v != nullptr && *v != 0 && *v != '0';
    }();
    const ExPolygons inner = with_outline ? offset_ex(area, -float(scale_(flow.spacing()))) : area;
    static const bool snake = [] {
        const char *v = research_switch("NP_CT1_SNAKE");
        return v != nullptr && *v != 0 && *v != '0';
    }();
    std::vector<Polylines> out;
    if (snake) {
        Point low_start = start;
        float low       = std::numeric_limits<float>::infinity();
        for (const ExPolygon &e : inner)
            for (const Point &q : e.contour.points)
                if (const float h = surface.sample(unscale(q)); !std::isnan(h) && h < low)
                    low = h, low_start = q;
        out = shell_polylines(inner, along, ipRectilinear, flow.spacing(), false, flow, z, low_start, MaxGlide);
    }
    Polylines lines;
    if (!snake)
        for (Polylines &chain : shell_polylines(inner, along, ipRectilinear, flow.spacing(), false, flow, z, start, 0.))
            for (size_t i = 0; i < chain.size(); i += 2)
                lines.push_back(std::move(chain[i]));
    if (ct1_strips_climb()) {
        Polylines pieces;
        for (Polyline &pl : lines) {
            std::vector<float> h(pl.size());
            for (size_t i = 0; i < pl.size(); ++i)
                h[i] = surface.sample(unscale(pl.points[i]));
            size_t from = 0;
            int    dir  = 0;
            for (size_t i = 1; i < pl.size(); ++i) {
                if (std::isnan(h[i]) || std::isnan(h[i - 1]))
                    continue;
                const int d = h[i] > h[i - 1] + 0.01f ? 1 : h[i] < h[i - 1] - 0.01f ? -1 : 0;
                if (d != 0 && dir != 0 && d != dir) {
                    pieces.emplace_back(Points(pl.points.begin() + from, pl.points.begin() + i));
                    from = i - 1;
                }
                if (d != 0)
                    dir = d;
            }
            pieces.emplace_back(Points(pl.points.begin() + from, pl.points.end()));
        }
        lines = std::move(pieces);
    }
    std::vector<std::pair<double, Polyline>> keyed;
    keyed.reserve(lines.size());
    for (Polyline &pl : lines) {
        if (pl.points.size() < 2)
            continue;
        const float a = surface.sample(unscale(pl.first_point())), b = surface.sample(unscale(pl.last_point()));
        if (!std::isnan(a) && !std::isnan(b) && b < a)
            pl.reverse();
        const double h = mean_height(surface, pl);
        keyed.emplace_back(h, std::move(pl));
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto &l, const auto &r) { return l.first < r.first; });
    out.reserve(out.size() + keyed.size());
    for (auto &[h, pl] : keyed)
        out.push_back({ std::move(pl) });
    static const bool outline_first = [] {
        const char *v = research_switch("NP_CT1_OUTLINE_FIRST");
        return v != nullptr && *v != 0 && *v != '0';
    }();
    const size_t n_lines = out.size();
    if (with_outline)
        for (const ExPolygon &e : offset_ex(area, -float(scale_(0.5 * flow.spacing()))))
            for (const Polygon &loop : to_polygons(e)) {
                if (loop.size() < 3)
                    continue;
                size_t lowest = 0;
                float  low    = std::numeric_limits<float>::infinity();
                for (size_t i = 0; i < loop.size(); ++i)
                    if (const float h = surface.sample(unscale(loop.points[i])); !std::isnan(h) && h < low)
                        low = h, lowest = i;
                Polyline ring = loop.split_at_index(int(lowest));
                if (outline_first) {
                    size_t top  = 0;
                    float  high = -std::numeric_limits<float>::infinity();
                    for (size_t i = 0; i < ring.size(); ++i)
                        if (const float h = surface.sample(unscale(ring.points[i])); !std::isnan(h) && h > high)
                            high = h, top = i;
                    if (top > 0 && top + 1 < ring.size()) {
                        Polyline up(Points(ring.points.begin(), ring.points.begin() + top + 1));
                        Polyline down(Points(ring.points.begin() + top, ring.points.end()));
                        down.reverse();
                        out.push_back({ std::move(up) });
                        out.push_back({ std::move(down) });
                        continue;
                    }
                }
                out.push_back({ std::move(ring) });
            }
    if (with_outline && outline_first)
        std::rotate(out.begin(), out.begin() + ptrdiff_t(n_lines), out.end());
    return out;
}

}
bool outline_is_contour(const ConformalPatch &patch, double tolerance)
{
    const double pitch = patch.surface.pitch();
    float        lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    float        zmin = std::numeric_limits<float>::max(), zmax = std::numeric_limits<float>::lowest();
    for (const float s : patch.surface.data())
        if (!std::isnan(s)) {
            zmin = std::min(zmin, s);
            zmax = std::max(zmax, s);
        }
    ExPolygons inset = offset_ex(patch.footprint, -float(scale_(std::max(2. * pitch, 1.))));
    if (inset.empty())
        inset = offset_ex(patch.footprint, -float(scale_(2. * pitch)));
    for (const ExPolygon &expoly : inset) {
        const Points &pts = expoly.contour.points;
        for (size_t i = 0; i < pts.size(); ++i) {
            const Vec2d a = unscale(pts[i]), b = unscale(pts[(i + 1) % pts.size()]);
            const int   n = std::max(1, int(std::ceil((b - a).norm() / pitch)));
            for (int j = 0; j < n; ++j)
                if (const float s = patch.surface.sample(a + (b - a) * (double(j) / n)); !std::isnan(s)) {
                    lo = std::min(lo, s);
                    hi = std::max(hi, s);
                }
        }
    }
    return hi >= lo && hi - lo <= std::max(tolerance, 0.1 * double(zmax - zmin));
}

bool is_round(const ExPolygons &footprint)
{
    double a = 0., p = 0.;
    for (const ExPolygon &expoly : footprint) {
        a += expoly.area();
        for (const Polygon &poly : to_polygons(expoly))
            p += poly.length();
    }
    return p > 0. && 4. * PI * a / (p * p) >= 0.5;
}

namespace {

struct PlanarFace
{
    ExPolygons area;
    double     fall = 0.;
};
constexpr double FaceMinWidth = 4.;
constexpr double FaceMinArea  = 25.;
std::vector<PlanarFace> planar_faces(const HeightField &surface, const ExPolygons &area)
{
    const int          nx = surface.nx(), ny = surface.ny();
    const size_t       n  = size_t(nx) * ny;
    std::vector<Vec2d> grad(n, Vec2d(NAN, NAN));
    for (int iy = 0; iy < ny; ++iy)
        for (int ix = 0; ix < nx; ++ix)
            if (!std::isnan(surface.at(ix, iy)))
                grad[size_t(iy) * nx + ix] = surface.gradient(surface.node(ix, iy));
    const double            spread = std::tan(Geometry::deg2rad(1.)), rise = std::tan(Geometry::deg2rad(3.));
    std::vector<uint8_t>    seen(n, 0);
    std::vector<size_t>     queue;
    std::vector<PlanarFace> out;
    for (size_t seed = 0; seed < n; ++seed) {
        if (seen[seed] || !grad[seed].allFinite() || grad[seed].norm() < rise)
            continue;
        const Vec2d g0  = grad[seed];
        Vec2d       sum = Vec2d::Zero();
        queue.assign(1, seed);
        seen[seed] = 1;
        for (size_t q = 0; q < queue.size(); ++q) {
            const size_t i = queue[q];
            sum += grad[i];
            const int ix = int(i % size_t(nx)), iy = int(i / size_t(nx));
            static constexpr int steps[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (const auto &d : steps) {
                const int jx = ix + d[0], jy = iy + d[1];
                if (jx < 0 || jy < 0 || jx >= nx || jy >= ny)
                    continue;
                const size_t j = size_t(jy) * nx + jx;
                if (!seen[j] && grad[j].allFinite() && (grad[j] - g0).norm() <= spread) {
                    seen[j] = 1;
                    queue.emplace_back(j);
                }
            }
        }
        if (double(queue.size()) * sqr(surface.pitch()) < FaceMinArea)
            continue;
        Mask mask(n, 0);
        for (const size_t i : queue)
            mask[i] = 1;
        ExPolygons face = intersection_ex(opening_ex(mask_to_expolygons(mask, surface), float(scale_(0.5 * FaceMinWidth))), area);
        double     a    = 0.;
        for (const ExPolygon &e : face)
            a += e.area() * SCALING_FACTOR * SCALING_FACTOR;
        if (a >= FaceMinArea)
            out.push_back({ std::move(face), std::atan2(sum.y(), sum.x()) });
    }
    return out;
}

constexpr double SparseWallSlope = 45.;

class SteepTops
{
public:
    static constexpr int MaxOuter = 3;
    static constexpr int Azimuths = 24;

    SteepTops(const HeadEnvelope &head, double shell_height, double band_height, double max_bead, double cap)
        : m_cap(cap), m_shell_height(shell_height), m_max_bead(max_bead)
    {
        HeadEnvelope body = head;
        body.boxes.clear();
        std::vector<std::pair<double, double>> nozzle;
        for (const HeadBox &box : head.boxes)
            if (box.nozzle && !box.infinite_x && !box.infinite_y && box.min.z() < band_height)
                nozzle.emplace_back(box.min.z(), std::max({ -box.min.x(), box.max.x(), -box.min.y(), box.max.y() }));
            else if (box.min.z() - (box.nozzle ? 0. : head.margin) < band_height)
                body.boxes.push_back(box);
        for (int m = 1; m <= MaxOuter; ++m) {
            const double gap = (m + 1) * shell_height;
            double       round = std::numeric_limits<double>::infinity();
            for (const auto &[floor, radius] : nozzle)
                if (radius > 0.)
                    round = std::min(round, (gap + floor) / radius);
            for (int a = 0; a < Azimuths; ++a)
                m_tan[size_t(m - 1)][size_t(a)] = std::min(round, std::tan(body.max_slope(2. * PI * a / Azimuths, gap)));
        }
    }

    double cap() const { return m_cap; }

    int outer_shells(const Vec2d &g) const
    {
        const double slope = g.norm();
        if (!(slope > m_cap))
            return 1;
        const double a   = std::atan2(g.y(), g.x()) / (2. * PI) * Azimuths;
        const int    lo  = int(std::floor(a));
        const auto   bin = [](int i) { return size_t(((i % Azimuths) + Azimuths) % Azimuths); };
        const double cos = 1. / std::sqrt(1. + slope * slope);
        for (int m = 1; m <= MaxOuter && m * m_shell_height * cos <= m_max_bead; ++m)
            if (const auto &lim = m_tan[size_t(m - 1)]; slope <= std::min(lim[bin(lo)], lim[bin(lo + 1)]))
                return m;
        return 0;
    }

private:
    double                                                    m_cap, m_shell_height, m_max_bead;
    std::array<std::array<double, Azimuths>, MaxOuter>        m_tan {};
};

constexpr int SteepSectors = 12;
constexpr double SteepLineWidth = 1.;

std::vector<PlanarFace> steep_sectors(const HeightField &surface, const ExPolygons &area, double cap)
{
    const size_t       n = surface.data().size();
    std::vector<Mask>  masks(SteepSectors);
    std::vector<Vec2d> sums(SteepSectors, Vec2d::Zero());
    for (int iy = 0; iy < surface.ny(); ++iy)
        for (int ix = 0; ix < surface.nx(); ++ix)
            if (const Vec2d g = surface.gradient(surface.node(ix, iy)); g.allFinite() && g.norm() > cap) {
                const int s = int(std::lround(std::atan2(g.y(), g.x()) / (2. * PI) * SteepSectors) % SteepSectors + SteepSectors) % SteepSectors;
                masks[size_t(s)].resize(n, 0);
                masks[size_t(s)][size_t(iy) * surface.nx() + ix] = 1;
                sums[size_t(s)] += g;
            }
    std::vector<PlanarFace> out;
    for (int s = 0; s < SteepSectors; ++s)
        if (!masks[size_t(s)].empty())
            if (ExPolygons a = intersection_ex(mask_to_expolygons(masks[size_t(s)], surface), area); !a.empty())
                out.push_back({ std::move(a), std::atan2(sums[size_t(s)].y(), sums[size_t(s)].x()) });
    return out;
}

constexpr double SteepMinWidth = 1.;

ExPolygons steep_area(const HeightField &surface, double cap)
{
    Mask mask(surface.data().size(), 0);
    bool any = false;
    for (int iy = 0; iy < surface.ny(); ++iy)
        for (int ix = 0; ix < surface.nx(); ++ix)
            if (!std::isnan(surface.at(ix, iy)) && surface.gradient(surface.node(ix, iy)).norm() > cap)
                any = mask[size_t(iy) * surface.nx() + ix] = 1;
    return any ? opening_ex(mask_to_expolygons(mask, surface), float(scale_(0.5 * SteepMinWidth))) : ExPolygons();
}

HeightField steep_left_out(const HeightField &zones, double shell_height)
{
    HeightField out = zones;
    const size_t n = out.data().size();
    for (int m = SteepTops::MaxOuter; m >= 2; --m) {
        Mask at_least(n, 0);
        bool any = false;
        for (size_t i = 0; i < n; ++i)
            any |= at_least[i] = !std::isnan(out.data()[i]) && out.data()[i] >= float(m);
        if (!any)
            continue;
        const Mask inner = rasterize(offset_ex(mask_to_expolygons(at_least, out), -float(scale_(m * shell_height))), out);
        for (size_t i = 0; i < n; ++i)
            if (at_least[i] && !inner[i])
                out.data()[i] = float(m - 1);
    }
    return out;
}

HeightField steep_outer_shells(const HeightField &surface, const SteepTops &steep, const ExPolygons &area, int shells)
{
    HeightField out  = surface;
    const Mask  wide = rasterize(area, surface);
    for (int iy = 0; iy < surface.ny(); ++iy)
        for (int ix = 0; ix < surface.nx(); ++ix)
            if (float &m = out.at(ix, iy); !std::isnan(m)) {
                const Vec2d g = surface.gradient(surface.node(ix, iy));
                m = float(wide[size_t(iy) * surface.nx() + ix] && g.allFinite() ? std::clamp(steep.outer_shells(g), 1, std::max(1, shells - 1)) : 1);
            }
    return out;
}

constexpr double PlugCrossSlope = 8.;
constexpr double PlugLineAngle = 10.;
constexpr double PlugCrossArea = 400.;

constexpr double BandedTopAspect = 1.5;
constexpr double BandedTopMinShare = 0.75;

double banded_outer_width(double line_width, double nozzle, double height, double fraction = BandedTopWidth)
{
    const double w = std::min(line_width, fraction * nozzle);
    return std::min(w, std::max(BandedTopMinShare * fraction * nozzle, BandedTopAspect * height));
}

struct BandedFill
{
    bool       strips       = false;
    const HeightField *contour = nullptr;
    ArachneCache *arachne = nullptr;
    double     strip_height = 0.;
    double     strip_offset = 0.;
    double     flat_angle = std::numeric_limits<double>::quiet_NaN();
    ExPolygons lengthwise;
    const HeightField *layers = nullptr;
    int                sparse = 0;
    int                hidden = 0;
    double             density = 1.;
    double             angle = 0.;
    bool               bridge_first = false;
    const AdaptiveStack *adaptive = nullptr;
    double               outer_height = 0.;
    bool               walls = false;
    double             sparse_angle = std::numeric_limits<double>::quiet_NaN();
    Polygons voids;
    const SteepTops *steep = nullptr;
};

ExPolygons hidden_layer_area(const HeightField &layers, int above, const ExPolygons &area)
{
    Mask mask(layers.data().size(), 0);
    for (size_t n = 0; n < mask.size(); ++n)
        mask[n] = !std::isnan(layers.data()[n]) && above < int(layers.data()[n]);
    const float r = float(scale_(layers.pitch()));
    ExPolygons  cells = offset_ex(offset_ex(offset_ex(mask_to_expolygons(mask, layers), r), -2.f * r), r);
    return intersection_ex(area, expolygons_simplify(cells, scale_(0.25 * layers.pitch())));
}

ExPolygons plumb_wall_band(const PrintObject &object, const HeightField &S, const ExPolygons &candidate, double wall_width, double depth,
                           double void_depth, bool any_shape)
{
    constexpr double Plumb      = 2.;
    constexpr double MaxFitArea = 2200.;
    constexpr double Edge       = 0.1;
    constexpr double Lean       = 0.25;
    const auto layers = object.layers();
    std::vector<double> layer_z(layers.size());
    for (size_t i = 0; i < layers.size(); ++i)
        layer_z[i] = layers[i]->print_z;
    const Mask cand = rasterize(candidate, S);
    float zlo = Inf, zhi = -Inf;
    for (size_t n = 0; n < cand.size(); ++n)
        if (cand[n] && !std::isnan(S.data()[n]))
            zlo = std::min(zlo, S.data()[n]), zhi = std::max(zhi, S.data()[n]);
    if (zhi < zlo)
        return {};
    const float             w = float(scale_(wall_width));
    std::vector<ExPolygons> rings(layers.size()), edges(layers.size()), voids(layers.size()), band(layers.size()), ribs(layers.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i)
            if (const double z = layers[i]->print_z; z > zlo - std::max(depth, void_depth) - 2. * Plumb && z < zhi + EPSILON) {
                const ExPolygons &slices = layers[i]->lslices;
                Polygons          bores;
                ExPolygons        solid_below;
                for (const ExPolygon &island : slices) {
                    const Point inside = island.contour.points.front();
                    const bool  nested = std::any_of(slices.begin(), slices.end(), [&](const ExPolygon &other) {
                        return &other != &island &&
                               std::any_of(other.holes.begin(), other.holes.end(), [&](const Polygon &h) { return h.contains(inside); });
                    });
                    const bool rib = nested && opening_ex(ExPolygons{ ExPolygon(island.contour) }, 1.5f * w).empty();
                    if (rib || (nested && (any_shape || fit_round(island.contour)) && std::abs(island.contour.area()) * SCALING_FACTOR * SCALING_FACTOR <= MaxFitArea)) {
                        if (rib)
                            append(ribs[i], diff_ex(ExPolygon(island.contour), offset_ex(ExPolygon(island.contour), -w)));
                        append(rings[i], diff_ex(ExPolygon(island.contour), offset_ex(ExPolygon(island.contour), -w)));
                        append(edges[i], diff_ex(ExPolygon(island.contour), offset_ex(ExPolygon(island.contour), -float(scale_(Edge)))));
                    }
                    ExPolygons solid;
                    for (Polygon hole : island.holes)
                        if ((any_shape || fit_round(hole)) && std::abs(hole.area()) * SCALING_FACTOR * SCALING_FACTOR <= MaxFitArea) {
                            hole.make_counter_clockwise();
                            if (solid.empty())
                                solid = opening_ex(ExPolygons{ island }, 1.5f * w);
                            const ExPolygons near = offset_ex(ExPolygon(hole), 2.f * w);
                            bool             fit  = !intersection_ex(solid, near).empty();
                            if (!fit) {
                                if (solid_below.empty())
                                    if (const auto b = std::upper_bound(layer_z.begin(), layer_z.end(), z - Plumb + EPSILON); b != layer_z.begin())
                                        solid_below = opening_ex(layers[size_t(b - layer_z.begin()) - 1]->lslices, 1.5f * w);
                                fit = !intersection_ex(solid_below, near).empty();
                            }
                            if (fit) {
                                append(rings[i], intersection_ex(diff_ex(offset(hole, w), Polygons{ hole }), ExPolygons{ island }));
                                append(edges[i], intersection_ex(diff_ex(offset(hole, float(scale_(Edge))), Polygons{ hole }), ExPolygons{ island }));
                                bores.emplace_back(std::move(hole));
                            }
                        }
                }
                if (!bores.empty())
                    voids[i] = diff_ex(bores, slices);
            }
    });
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            const double z = layers[i]->print_z;
            if ((rings[i].empty() || z < zlo - depth - EPSILON) && (voids[i].empty() || z < zlo - void_depth - EPSILON))
                continue;
            const auto below = std::upper_bound(layer_z.begin(), layer_z.end(), z - Plumb + EPSILON);
            if (below == layer_z.begin())
                continue;
            const Mask on = rasterize(rings[i], S), over = rasterize(voids[i], S);
            Mask       reach(on.size(), 0), reach_void(on.size(), 0);
            bool       any = false, any_void = false;
            for (size_t n = 0; n < on.size(); ++n)
                if (const float s = S.data()[n]; cand[n] && !std::isnan(s)) {
                    if (on[n] && s - z < depth + EPSILON && s - z > -layers[i]->height - EPSILON)
                        reach[n] = 1, any = true;
                    if (over[n] && s - z < void_depth + EPSILON && s - z > -0.5 * layers[i]->height + EPSILON)
                        reach_void[n] = 1, any_void = true;
                }
            const size_t b    = size_t(below - layer_z.begin()) - 1;
            const float  cell = float(scale_(S.pitch()));
            if (any_void) {
                ExPolygons lower = rings[b];
                append(lower, voids[b]);
                if (const ExPolygons plumb = intersection_ex(voids[i], offset_ex(lower, float(scale_(Lean)))); !plumb.empty())
                    band[i] = intersection_ex(plumb, offset_ex(mask_to_expolygons(reach_void, S), cell));
            }
            if (!any)
                continue;
            const ExPolygons plumb = intersection_ex(edges[i], offset_ex(edges[b], float(scale_(Lean))));
            const ExPolygons local = intersection_ex(edges[i], edges[i - 1]);
            ExPolygons       walls, rib_walls;
            for (const ExPolygon &piece : rings[i])
                if (!intersection_ex(ExPolygons{ piece }, ribs[i]).empty())
                    rib_walls.emplace_back(piece);
                else if (!intersection_ex(ExPolygons{ piece }, plumb).empty() && !intersection_ex(ExPolygons{ piece }, local).empty())
                    walls.emplace_back(piece);
            if (!walls.empty())
                walls = intersection_ex(walls, offset_ex(intersection_ex(edges[i], offset_ex(edges[b], float(scale_(ThreadDepth)))),
                                                         w + float(scale_(Edge))));
            append(walls, std::move(rib_walls));
            if (!walls.empty())
                append(band[i], intersection_ex(walls, offset_ex(mask_to_expolygons(reach, S), cell)));
        }
    });
    ExPolygons out;
    for (ExPolygons &b : band)
        append(out, std::move(b));
    return out.empty() ? out : intersection_ex(union_ex(out), candidate);
}

float nearest(const HeightField &field, const Vec2d &p)
{
    const Vec2d o  = (p - field.origin()) / field.pitch();
    const int   ix = int(std::lround(o.x())), iy = int(std::lround(o.y()));
    return ix < 0 || iy < 0 || ix >= field.nx() || iy >= field.ny() ? HeightField::NoData : field.at(ix, iy);
}

ExtrusionEntityCollection make_shell(const PrintObject &object, const ConformalPatch &patch, const HeightField &top_of_planar,
                                     const ExPolygons &area, int k, const ShellFill &fill, const Point &start, const Point3 *from,
                                     double host_z, double resample, const Polyline &seam_line, const BandedFill *banded = nullptr)
{
    const PrintRegion        &region  = object.printing_region(size_t(patch.region_id));
    const bool                monotonic_top = nonplanar_top_monotonic(region.config());
    const InfillPattern       top_pattern   = monotonic_top ? ipMonotonic : ipRectilinear;
    const bool                outer   = k == patch.shells;
    const bool                sparse  = banded != nullptr && k <= banded->sparse;
    const bool                plug    = banded != nullptr && !sparse && k <= banded->hidden;
    const AdaptiveStack      *adaptive = banded != nullptr ? banded->adaptive : nullptr;
    if (adaptive != nullptr && outer && banded->outer_height <= 0.) {
        ExtrusionEntityCollection out;
        ExPolygons                rest   = area;
        ExPolygons                rest_lengthwise = banded->lengthwise;
        const Flow                nominal = region.flow(object, frTopSolidInfill, patch.shell_height);
        const double              narrow  = banded_outer_width(nominal.width(), nominal.nozzle_diameter(), patch.shell_height);
        for (size_t i = 0; i < adaptive->levels.size() && !(rest.empty() && rest_lengthwise.empty()); ++i) {
            const bool       last = i + 1 == adaptive->levels.size();
            const double     w    = outer_bead_width(adaptive->levels[i], nominal.nozzle_diameter(), narrow);
            const ExPolygons zone = last ? rest : opening_ex(intersection_ex(rest, adaptive->zones[i]), float(scale_(0.5 * w)));
            const ExPolygons strips = last ? rest_lengthwise : intersection_ex(rest_lengthwise, adaptive->zones[i]);
            if (zone.empty() && strips.empty())
                continue;
            rest            = diff_ex(rest, zone);
            rest_lengthwise = diff_ex(rest_lengthwise, strips);
            BandedFill zone_fill   = *banded;
            zone_fill.outer_height = adaptive->levels[i];
            zone_fill.lengthwise   = strips;
            ExtrusionEntityCollection part = make_shell(object, patch, top_of_planar,
                                                        intersection_ex(offset_ex(zone, float(scale_(0.5 * w))), area), k, fill,
                                                        start, nullptr, host_z, resample, seam_line, &zone_fill);
            out.append(std::move(part.entities));
            part.entities.clear();
        }
        out.no_sort = true;
        return out;
    }
    const double              height  = adaptive != nullptr && outer ? banded->outer_height : patch.shell_height;
    Flow                      flow    = region.flow(object, outer ? frTopSolidInfill : sparse ? frInfill : frSolidInfill, height);
    if (banded != nullptr && outer) {
        const float narrow = float(banded_outer_width(flow.width(), flow.nozzle_diameter(), height));
        flow = flow.with_width(adaptive != nullptr ? float(outer_bead_width(height, flow.nozzle_diameter(), narrow)) : narrow);
    } else if (banded != nullptr && adaptive == nullptr && k + 1 == patch.shells && !sparse && !plug && fill.spiral) {
        flow = flow.with_width(float(banded_outer_width(flow.width(), flow.nozzle_diameter(), height)));
    }
    ShellFill rings = fill;
    if (banded != nullptr && k + 1 == patch.shells && fill.spiral && !sparse && !plug && adaptive == nullptr) {
        const Flow   top   = region.flow(object, frTopSolidInfill, height);
        const Flow   outer = top.with_width(float(banded_outer_width(top.width(), top.nozzle_diameter(), height)));
        const double s     = outer.spacing();
        const float  pitch = float(std::clamp(std::round(flow.spacing() / s), 1., 2.) * s);
        flow               = flow.width() + pitch - flow.spacing() >= flow.height() ? flow.with_spacing(pitch) :
                                 flow.with_width(Flow::rounded_rectangle_extrusion_width_from_spacing(pitch, flow.height()));
        rings.ring_spacing = s;
        rings.first_ring   = int(std::ceil((flow.width() - outer.width()) / (2. * s) - 1e-6));
    }
    const bool                bridge  = plug && banded->bridge_first && k == banded->sparse + 1;
    const ExtrusionRole       role    = outer ? erTopSolidInfill : sparse ? erInternalInfill : bridge ? erInternalBridgeInfill : erSolidInfill;
    const double drop = adaptive != nullptr ? adaptive->nominal_drop(k) : (patch.shells - k) * patch.shell_height;
    const bool   surface_spaced = region.config().nonplanar_surface_spacing.value && !fill.spiral && !(banded != nullptr && banded->strips) &&
                                !sparse && !plug;
    std::vector<Polylines> linked;
    HelixRamp              ramp;
    WidthRoads             roads;
    ExPolygons whole = banded == nullptr || banded->lengthwise.empty() ? area : union_ex(area, banded->lengthwise);
    Polylines  wall;
    const Flow wall_flow = region.flow(object, frPerimeter, patch.shell_height);
    if (plug && banded->walls && banded->layers != nullptr) {
        const ExPolygons layer_area = hidden_layer_area(*banded->layers, patch.shells - k, whole);
        for (const ExPolygon &expoly : offset_ex(layer_area, -0.5f * float(scale_(wall_flow.spacing()))))
            for (const Polygon &poly : to_polygons(expoly))
                if (poly.size() >= 3) {
                    wall.emplace_back(poly.split_at_index(int(poly.closest_point_index(start))));
                    wall.back().points.emplace_back(wall.back().points.front());
                }
        const double     overlap = 0.01 * region.config().infill_wall_overlap.value * wall_flow.width();
        const ExPolygons strip   = diff_ex(layer_area, offset_ex(layer_area, -float(scale_(wall_flow.spacing() - overlap))));
        whole = diff_ex(whole, intersection_ex(strip, hidden_layer_area(*banded->layers, patch.shells - k + 1, whole)));
    }
    const auto off_voids = [&](const ExPolygons &fill_area, double spacing) {
        return banded == nullptr || banded->voids.empty() ? fill_area :
                                                            diff_ex(fill_area, offset(banded->voids, float(scale_(0.45 * spacing))));
    };
    const SteepTops  *steep = banded != nullptr && adaptive == nullptr ? banded->steep : nullptr;
    const ExPolygons  steep_faces = steep != nullptr ? steep_area(patch.surface, steep->cap()) : ExPolygons();
    const HeightField steep_m     = steep != nullptr ? steep_outer_shells(patch.surface, *steep, steep_faces, patch.shells) : HeightField();
    const HeightField steep_skip  = steep != nullptr ? steep_left_out(steep_m, patch.shell_height) : HeightField();
    const Mask steep_pairs = steep != nullptr && !steep_faces.empty() ?
                                 rasterize(offset_ex(steep_faces, -float(scale_(2. * patch.shell_height))), patch.surface) : Mask();
    std::vector<std::pair<std::vector<Polylines>, Flow>> thick;
    ExPolygons rest = area, fall_lines;
    const bool climb_strips = ct1_strips_climb() && banded != nullptr && !sparse && !plug && !banded->lengthwise.empty();
    if (climb_strips)
        rest = union_ex(area, banded->lengthwise);
    if (banded != nullptr && outer && !sparse && !plug && adaptive == nullptr && !ct1_strips_climb()) {
        std::vector<PlanarFace> falls;
        if (!fill.spiral)
            falls = planar_faces(patch.surface, area);
        if (steep != nullptr) {
            ExPolygons faces;
            for (const PlanarFace &face : falls)
                append(faces, face.area);
            append(falls, steep_sectors(patch.surface, intersection_ex(diff_ex(area, faces), steep_faces), steep->cap()));
        }
        std::array<ExPolygons, SteepTops::MaxOuter> zones;
        if (steep != nullptr) {
            std::array<Mask, SteepTops::MaxOuter> masks;
            for (int iy = 0; iy < steep_m.ny(); ++iy)
                for (int ix = 0; ix < steep_m.nx(); ++ix)
                    if (const float m = steep_m.at(ix, iy); !std::isnan(m) && patch.surface.gradient(steep_m.node(ix, iy)).norm() > steep->cap()) {
                        Mask &mask = masks[size_t(m) - 1];
                        mask.resize(steep_m.data().size(), 0);
                        mask[size_t(iy) * steep_m.nx() + ix] = 1;
                    }
            for (size_t m = 0; m < masks.size(); ++m)
                if (!masks[m].empty())
                    zones[m] = mask_to_expolygons(masks[m], steep_m);
        }
        for (const PlanarFace &face : falls) {
            ShellFill down;
            down.angle   = face.fall;
            down.outline = false;
            ExPolygons single = face.area, part;
            double     tallest = 0.;
            for (int m = 1; m <= SteepTops::MaxOuter; ++m)
                if (ExPolygons zone = intersection_ex(face.area, zones[size_t(m - 1)]); !zone.empty()) {
                    append(part, std::move(zone));
                    tallest = m * patch.shell_height;
                }
            if (!part.empty()) {
                part   = union_ex(part);
                single = diff_ex(single, part);
                const double w          = std::max({ double(flow.width()), SteepLineWidth * flow.nozzle_diameter(), tallest });
                const Flow   thick_flow = flow.with_width(float(w)).with_height(float(patch.shell_height));
                thick.push_back({ shell_polylines(off_voids(part, thick_flow.spacing()), down, top_pattern, thick_flow.spacing(), monotonic_top, thick_flow,
                                                  host_z, start, MaxGlide, nullptr, &seam_line), thick_flow });
            }
            append(linked, shell_polylines(off_voids(single, flow.spacing()), down, top_pattern, flow.spacing(), monotonic_top, flow, host_z,
                                           start, MaxGlide, nullptr, &seam_line));
            rest = diff_ex(rest, face.area);
            append(fall_lines, face.area);
        }
    }
    if (plug && k == banded->sparse + 1 && banded->sparse > 0) {
        ShellFill cross;
        cross.angle   = banded->angle + ((banded->hidden - k) % 2 == 0 ? 0.5 * PI : 0.);
        cross.outline = false;
        linked        = shell_polylines(off_voids(whole, flow.spacing()), cross, ipRectilinear, flow.spacing(), false, flow, host_z, start, MaxGlide, nullptr, &seam_line);
    } else if (sparse || plug) {
        const double spacing = sparse ? flow.spacing() / banded->density : flow.spacing();
        ExPolygons gentle;
        if (plug && k == banded->hidden && Slic3r::area(whole) * SCALING_FACTOR * SCALING_FACTOR < PlugCrossArea)
            gentle = whole;
        else if (plug && k == banded->hidden) {
            Mask mask(patch.surface.data().size(), 0);
            for (int iy = 0; iy < patch.surface.ny(); ++iy)
                for (int ix = 0; ix < patch.surface.nx(); ++ix)
                    if (const Vec2d g = patch.surface.gradient(patch.surface.node(ix, iy)); g.norm() <= std::tan(Geometry::deg2rad(PlugCrossSlope)))
                        mask[size_t(iy) * size_t(patch.surface.nx()) + size_t(ix)] = 1;
            gentle = opening_ex(intersection_ex(whole, mask_to_expolygons(mask, patch.surface)), float(scale_(flow.spacing())));
        }
        Point           open      = start;
        const Polyline *open_line = &seam_line;
        if (plug) {
            open      = Point(2 * get_extents(whole).center() - start);
            open_line = nullptr;
        }
        const ExPolygons steep = gentle.empty() ? whole : diff_ex(whole, gentle);
        const double shift = plug ? 0.5 * spacing : 0.;
        if (banded->strips)
            for (Polyline &pl : sparse_strip_fill(patch.surface, steep, drop, banded->strip_height, spacing, open, 0.5 * shift))
                linked.push_back({ std::move(pl) });
        else {
            const ExPolygons plan  = fill.spiral && shift > 0. ? offset_ex(whole, -float(scale_(shift))) : whole;
            ShellFill        along = fill;
            if (plug && !fill.spiral)
                along.angle += Geometry::deg2rad((banded->hidden - k) % 2 == 0 ? PlugLineAngle : -PlugLineAngle);
            else if (sparse && !std::isnan(banded->sparse_angle))
                along.angle = banded->sparse_angle;
            std::vector<Polylines> chains = shell_polylines(fill.spiral ? plan : off_voids(plan, spacing), along, ipRectilinear, spacing, false, flow, host_z, open, MaxGlide, nullptr, open_line);
            if (gentle.empty())
                linked = std::move(chains);
            else
                for (const Polylines &chain : chains)
                    for (Polyline &pl : intersection_pl(chain, steep))
                        linked.push_back({ std::move(pl) });
        }
        if (!gentle.empty()) {
            ShellFill cross;
            cross.angle   = (fill.spiral || banded->strips ? Geometry::deg2rad(region.config().infill_direction.value) : fill.angle) + 0.5 * PI;
            cross.outline = false;
            append(linked, shell_polylines(off_voids(gentle, spacing), cross, ipRectilinear, spacing, false, flow, host_z, open, MaxGlide, nullptr, open_line));
        }
    } else if (banded != nullptr && banded->strips && !(ct1_strips_climb() && !rest.empty() && !sparse && !plug)) {
        ExPolygons contours = outer ? expolygons_simplify(rest, scale_(0.5 * patch.surface.pitch())) : rest;
        if (!fall_lines.empty())
            contours = diff_ex(contours, fall_lines);
        contour_fill(patch.surface, contours, drop,
                     banded->strip_height, banded->strip_offset, flow, height, fill.angle, outer && monotonic_top, host_z, roads, linked,
                     outer ? &start : nullptr, banded->contour, banded->arachne,
                     std::isnan(banded->flat_angle) || outer ? banded->flat_angle : banded->flat_angle + 0.5 * PI);
    } else if (!rest.empty() && ct1_climb_requested() && !sparse && !plug)
        append(linked, climbing_lines(patch.surface.get(), off_voids(rest, flow.spacing()), flow, host_z, start));
    else if (!rest.empty())
        append(linked, shell_polylines(fill.spiral ? rest : off_voids(rest, flow.spacing()), rings, outer ? top_pattern : ipRectilinear, flow.spacing(), outer && monotonic_top, flow,
                                       host_z, start, MaxGlide, surface_spaced ? &patch.surface.get() : nullptr, &seam_line, &ramp));
    if (research_switch("NP_CT1_TRACE"))
        BOOST_LOG_TRIVIAL(warning) << "ct1 shell patch " << patch.id << " k " << k << " outer " << outer << " sparse " << sparse << " plug " << plug
                                   << " banded " << (banded != nullptr) << " strips " << (banded != nullptr && banded->strips) << " spiral " << fill.spiral
                                   << " rest " << rest.size() << " falls " << fall_lines.size() << " linked " << linked.size();
    if (banded != nullptr && !sparse && !plug && !climb_strips)
        for (const ExPolygon &strip : banded->lengthwise)
            arachne_roads(strip, flow, height, 0, roads, nullptr, outer ? &start : nullptr, banded->arachne);
    std::vector<Polylines> pinholes;
    WidthRoads covered = roads;
    for (const auto &[lines, thick_flow] : thick)
        for (const Polylines &chain : lines)
            for (size_t i = 0; i < chain.size(); i += 2) {
                covered.polylines.emplace_back(chain[i]);
                covered.widths.emplace_back(chain[i].size(), thick_flow.width());
            }
    if (banded != nullptr && !sparse &&
        cover_has_gaps(offset_ex(whole, -float(scale_(0.5 * flow.spacing()))), linked, flow.width(), covered.polylines, covered.widths,
                       flow.spacing())) {
        Polygons cover = width_cover(covered);
        append(cover, linked_cover(linked, flow.width()));
        for (Polyline &pl : pinhole_roads(whole, std::move(cover), flow.width(), flow.spacing()))
            pinholes.push_back({ std::move(pl) });
    }

    const ExtrusionPath proto(role, flow.mm3_per_mm(), float(flow.width()), float(height));
    Mask narrow_shell1;
    if (banded != nullptr && banded->layers == nullptr && adaptive == nullptr && k == 1)
        narrow_shell1 = rasterize(diff_ex(whole, opening_ex(whole, float(scale_(0.5 * LengthwiseMaxLines * flow.width())))), patch.surface);
    auto narrow_floor = [&](const Vec2d &p) {
        if (narrow_shell1.empty())
            return false;
        const Vec2d o  = (p - patch.surface.origin()) / patch.surface.pitch();
        const int   ix = int(std::lround(o.x())), iy = int(std::lround(o.y()));
        return ix >= 0 && iy >= 0 && ix < patch.surface.nx() && iy < patch.surface.ny() && narrow_shell1[size_t(iy) * patch.surface.nx() + ix] != 0;
    };
    auto at = [&](const Vec2d &p, double &z, float &bead) {
        const float s = patch.surface.sample(p);
        if (std::isnan(s) || (banded != nullptr && (sparse ? spans_slope(patch.surface, p, SparseWallSlope) : spans_wall(patch.surface, p))))
            return false;
        z    = s - drop;
        bead = float(patch.shell_height);
        if (steep != nullptr) {
            const float m = nearest(steep_skip, p);
            if (const int above = patch.shells - k; m > 1.f && above < int(m)) {
                if (above > 0)
                    return false;
                bead = m * float(patch.shell_height);
            } else if (above > 0 && !steep_pairs.empty()) {
                const Vec2d o  = (p - patch.surface.origin()) / patch.surface.pitch();
                const int   ix = int(std::lround(o.x())), iy = int(std::lround(o.y()));
                if (ix >= 0 && iy >= 0 && ix < patch.surface.nx() && iy < patch.surface.ny() &&
                    steep_pairs[size_t(iy) * patch.surface.nx() + ix]) {
                    const int j = above - std::max(1, int(m));
                    if (j % 2 == 1 && k > 1)
                        return false;
                    if (j % 2 == 0 && k > 2)
                        bead = 2.f * float(patch.shell_height);
                }
            }
        }
        if (adaptive != nullptr) {
            double d, b;
            bool   deepest;
            if (!adaptive->layer(k, p, d, b, deepest))
                return false;
            z    = s - d;
            bead = float(b);
            if (deepest) {
                const float floor_z = banded->layers != nullptr ? nearest(top_of_planar, p) : top_of_planar.sample(p);
                if (std::isnan(floor_z))
                    return false;
                bead = float(z - floor_z);
            }
        } else if (banded != nullptr && banded->layers != nullptr) {
            const float layers = nearest(*banded->layers, p);
            const int   above  = patch.shells - k;
            if (std::isnan(layers) || above >= int(layers))
                return false;
            if (above == int(layers) - 1) {
                const float floor_z = nearest(top_of_planar, p);
                if (!std::isfinite(floor_z))
                    return false;
                bead = float(z - floor_z);
            }
        } else if (k == 1) {
            const float floor_z = narrow_floor(p) ? nearest(top_of_planar, p) : top_of_planar.sample(p);
            if (!std::isfinite(floor_z))
                return false;
            bead = float(z - floor_z);
        }
        if (surface_spaced)
            bead = surface_spaced_bead(bead, flow.width(), patch.surface.gradient(p), fill.angle);
        return true;
    };
    ExtrusionEntityCollection out;
    if (!wall.empty()) {
        auto wall_at = [&](const Vec2d &p, double &z, float &bead) {
            const float layers = nearest(*banded->layers, p);
            return !std::isnan(layers) && int(layers) - 1 > patch.shells - k && at(p, z, bead);
        };
        std::vector<Polylines> rings;
        for (Polyline &pl : wall)
            rings.push_back({ std::move(pl) });
        lift_linked(rings, ExtrusionPath(erPerimeter, wall_flow.mm3_per_mm(), float(wall_flow.width()), float(patch.shell_height)), wall_at,
                    host_z, resample, patch.id, k - 1, out.entities);
    }
    lift_polylines(roads.polylines, proto, at, host_z, resample, patch.id, k - 1, out.entities, &roads.widths);
    for (auto &[lines, thick_flow] : thick)
        lift_linked(lines, ExtrusionPath(role, thick_flow.mm3_per_mm(), float(thick_flow.width()), float(thick_flow.height())), at, host_z,
                    resample, patch.id, k - 1, out.entities);
    const size_t helix_from = out.entities.size();
    lift_linked(linked, proto, at, host_z, resample, patch.id, k - 1, out.entities);
    if (out.entities.size() == helix_from + 1)
        ramp_helix_flow(*static_cast<ExtrusionPathConformal *>(out.entities.back()), ramp, fill.outward);
    else if (ramp.end > ramp.begin && !fill.outward) {
        double before = 0.;
        Point  last;
        for (size_t i = helix_from; i < out.entities.size(); ++i) {
            auto &piece = *static_cast<ExtrusionPathConformal *>(out.entities[i]);
            if (i > helix_from)
                before += unscale(Point(piece.first_point() - last)).norm();
            ramp_helix_flow(piece, HelixRamp{ ramp.begin - before, ramp.end - before }, false);
            before += unscale<double>(piece.polyline.to_polyline().length());
            last = piece.last_point();
        }
    }
    lift_linked(pinholes, proto, at, host_z, resample, patch.id, k - 1, out.entities);
    if (from != nullptr && !out.entities.empty())
        prepend_glide(*static_cast<ExtrusionPathConformal *>(out.entities.front()), *from,
                      offset_ex(area, float(scale_(0.25 * flow.spacing()))), at, host_z, resample, MaxGlide);
    out.no_sort = true;
    return out;
}

void taper_top(ConformalPatch &patch)
{
    if (patch.shell_paths.empty())
        return;
    Polylines                       roads, others;
    std::vector<std::vector<float>> widths;
    std::vector<std::pair<ExtrusionPathConformal *, size_t>> slots;
    float  to_width = 0.f;
    double spacing  = 0.;
    for (ExtrusionEntity *ee : patch.shell_paths.back().entities) {
        auto          &path = *static_cast<ExtrusionPathConformal *>(ee);
        const Points3 &pts  = path.polyline.points;
        for (size_t i = 0; i + 1 < pts.size();) {
            if (path.bead_height[i] <= 0.f || path.bead_height[i + 1] <= 0.f) {
                ++i;
                continue;
            }
            size_t j = i + 1;
            while (j + 1 < pts.size() && path.bead_height[j + 1] > 0.f)
                ++j;
            Polyline run;
            for (size_t k = i; k <= j; ++k)
                run.points.emplace_back(pts[k].x(), pts[k].y());
            if (path.bead_width.size() != pts.size())
                others.emplace_back(std::move(run));
            else {
                roads.emplace_back(std::move(run));
                widths.emplace_back(path.bead_width.begin() + i, path.bead_width.begin() + j + 1);
                slots.emplace_back(&path, i);
                to_width = float(path.height * (1. - 0.25 * PI));
                spacing  = path.width - to_width;
            }
            i = j;
        }
    }
    if (roads.empty())
        return;
    taper_converging(roads, widths, others, to_width, spacing);
    for (size_t r = 0; r < roads.size(); ++r)
        std::copy(widths[r].begin(), widths[r].end(), slots[r].first->bead_width.begin() + slots[r].second);
}

}

void contour_roads(const HeightField &surface, const ExPolygons &area, double drop, double strip_height, double strip_offset, const Flow &flow,
                   double layer_height, double angle, bool monotonic, double z, WidthRoads &roads, std::vector<Polylines> &lines)
{
    contour_fill(surface, area, drop, strip_height, strip_offset, flow, layer_height, angle, monotonic, z, roads, lines);
    if (cover_has_gaps(offset_ex(area, -float(scale_(0.5 * flow.spacing()))), lines, flow.width(), roads.polylines, roads.widths, flow.spacing())) {
        Polygons cover = width_cover(roads);
        append(cover, linked_cover(lines, flow.width()));
        for (Polyline &pl : pinhole_roads(area, std::move(cover), flow.width(), flow.spacing()))
            lines.push_back({ std::move(pl) });
    }
}

double low_plane_top_radius(const PrintObject &object, double layer_height)
{
    if (object.auto_z_contouring())
        return 0.;
    const double min_z = PrintRegionConfig::defaults().zaa_min_z.value;
    if (0.5 * layer_height <= min_z + EPSILON)
        return 0.;
    double radius = 0.;
    for (size_t region_id = 0; region_id < object.num_printing_regions(); ++region_id) {
        const PrintRegion       &region = object.printing_region(region_id);
        const PrintRegionConfig &cfg    = region.config();
        if (cfg.zaa_enabled)
            return 0.;
        if (!top_shells_enabled(cfg, object.config()) || effective_fallback(cfg, object.config()) != NonplanarFallback::None)
            continue;
        const double h_c = cfg.nonplanar_shell_layer_height.value > 0. ? cfg.nonplanar_shell_layer_height.value : automatic_shell_height(object);
        const Flow   top = region.flow(object, frTopSolidInfill, h_c);
        const double w   = banded_outer_width(top.width(), top.nozzle_diameter(), h_c, BandedCapWidth);
        radius = std::max(radius, 0.5 * (0.5 * layer_height - min_z) * w / std::max(h_c, layer_height));
    }
    return radius;
}

double low_plane_rim(const PrintObject &object)
{
    double rim = 0.;
    for (size_t region_id = 0; region_id < object.num_printing_regions(); ++region_id) {
        const PrintRegion &region = object.printing_region(region_id);
        const double       h      = object.config().layer_height.value;
        rim = std::max(rim, std::max(0, region.config().wall_loops.value) * double(region.flow(object, frPerimeter, h).width()) +
                                double(region.flow(object, frTopSolidInfill, h).width()));
    }
    return rim;
}

HeightField sample_top_surface(const sla::IndexedMesh &mesh, double ray_z, double z_offset, const HeightField &like,
                               const std::function<void()> &throw_if_canceled,
                               const std::function<bool(const Vec2d &, double)> &solid_below)
{
    HeightField S = like;
    tbb::parallel_for(tbb::blocked_range<int>(0, S.ny()), [&](const tbb::blocked_range<int> &range) {
        for (int iy = range.begin(); iy < range.end(); ++iy) {
            if (throw_if_canceled)
                throw_if_canceled();
            for (int ix = 0; ix < S.nx(); ++ix) {
                const Vec2d p = S.node(ix, iy);
                S.at(ix, iy) = HeightField::NoData;
                for (const Vec2d &q : { p, Vec2d(p + Vec2d(1e-4, 1.7e-4)) }) {
                    const auto hit = mesh.query_ray_hit(Vec3d(q.x(), q.y(), ray_z), Vec3d(0., 0., -1.));
                    const double nz = hit.is_hit() ? hit.normal().normalized().z() : 0.;
                    if (nz > 0.05 || (nz < -0.05 && solid_below &&
                                      solid_below(p, hit.position().z() - mesh.ground_level() + z_offset))) {
                        const Vec3d n = hit.normal();
                        const double z = hit.position().z() + (n.x() * (q.x() - p.x()) + n.y() * (q.y() - p.y())) / n.z();
                        const float  s = float(z - mesh.ground_level() + z_offset);
                        if (!(S.at(ix, iy) >= s))
                            S.at(ix, iy) = s;
                    }
                }
            }
        }
    });
    return S;
}

HeightField sample_underside(const sla::IndexedMesh &mesh, double z_offset, const HeightField &S, const std::function<void()> &throw_if_canceled)
{
    HeightField U = S;
    tbb::parallel_for(tbb::blocked_range<int>(0, U.ny()), [&](const tbb::blocked_range<int> &range) {
        for (int iy = range.begin(); iy < range.end(); ++iy) {
            throw_if_canceled();
            for (int ix = 0; ix < U.nx(); ++ix) {
                const float s = S.at(ix, iy);
                if (std::isnan(s))
                    continue;
                const Vec2d p   = U.node(ix, iy);
                const double z = vertical_ray_hit(mesh, p, s - z_offset + mesh.ground_level() - 0.01, -1., true);
                U.at(ix, iy)    = float(std::isnan(z) ? z_offset : z - mesh.ground_level() + z_offset);
            }
        }
    });
    return U;
}

HeightField undersides_within(const HeightField &U, const HeightField &S, const Mask &closed, double reach)
{
    const int    r  = int(std::ceil(reach / U.pitch()));
    const double r2 = reach * reach / (U.pitch() * U.pitch());
    HeightField  out = U;
    tbb::parallel_for(tbb::blocked_range<int>(0, U.ny()), [&](const tbb::blocked_range<int> &range) {
        for (int iy = range.begin(); iy < range.end(); ++iy)
            for (int ix = 0; ix < U.nx(); ++ix) {
                const float s = S.at(ix, iy);
                float      &u = out.at(ix, iy);
                if (std::isnan(s) || std::isnan(u))
                    continue;
                for (int jy = std::max(0, iy - r); jy <= std::min(U.ny() - 1, iy + r); ++jy)
                    for (int jx = std::max(0, ix - r); jx <= std::min(U.nx() - 1, ix + r); ++jx)
                        if (const float v = U.at(jx, jy), d2 = float((jx - ix) * (jx - ix) + (jy - iy) * (jy - iy));
                            closed[size_t(jy) * U.nx() + jx] && d2 <= r2 && v < s && v - U.at(ix, iy) > 5.67f * std::sqrt(d2) * float(U.pitch()) && v > u)
                            u = v;
            }
    });
    return out;
}

static ConformalResult build_region_shells(PrintObject &object, const std::vector<size_t> &region_ids, int patch_offset,
                                           std::shared_ptr<ObjectMesh> &mesh, const ExPolygons *whole_tops,
                                           const ExPolygons &clearance)
{
    const size_t    region_id = region_ids.front();
    ConformalResult result;
    result.mesh = mesh;
    const auto      t_start = std::chrono::steady_clock::now();
    LayerPtrs      &layers  = object.layers();
    result.planar_exclusion.assign(layers.size(), ExPolygons());
    result.shells_refused.assign(layers.size(), ExPolygons());

    const ShellSettings settings = shell_settings(object, region_id);
    if (settings.shells <= 0 || layers.empty())
        return result;

    ExPolygons candidate = candidate_top_area(object, region_ids);
    if (!clearance.empty())
        candidate = diff_ex(candidate, clearance);
    if (candidate.empty())
        return result;

    const HeadEnvelope head = resolve_head_envelope(object.print()->config());

    const double      pitch = settings.pitch;
    Polygons          silhouette_src;
    const HeightField grid    = object_grid(object, pitch, &silhouette_src);
    const size_t      n_nodes = grid.data().size();

    auto cancel = [&object]() { throw_if_canceled(object); };
    HeightField S, U;
    Mask        closed_void;
    ExPolygons  whole;
    {
        const ObjectMesh &mesh = object_mesh(object, result);
        S = sample_top_surface(mesh.imesh, mesh.mesh.bounding_box().max.z() + 1., object.slicing_parameters().object_print_z_min, grid, cancel,
                               material_below(object));
        if (settings.shells > settings.top_shells) {
            const double z_offset = object.slicing_parameters().object_print_z_min;
            U = sample_underside(mesh.imesh, z_offset, S, cancel);
            closed_void.assign(U.data().size(), 0);
            tbb::parallel_for(tbb::blocked_range<int>(0, U.ny()), [&](const tbb::blocked_range<int> &range) {
                for (int iy = range.begin(); iy < range.end(); ++iy)
                    for (int ix = 0; ix < U.nx(); ++ix)
                        if (const float u = U.at(ix, iy); !std::isnan(u) && u > z_offset + EPSILON)
                            closed_void[size_t(iy) * U.nx() + ix] =
                                !std::isnan(vertical_ray_hit(mesh.imesh, U.node(ix, iy), u - z_offset + mesh.imesh.ground_level() - 0.01, -1.));
            });
        }
        if (const double noise = smooth_scan_noise(S, cancel); noise > 0.)
            BOOST_LOG_TRIVIAL(info) << "Non-planar: " << object.model_object()->name << " top surface noise " << noise << " mm";
    }
    {
        Mask smooth(n_nodes, 0);
        for (int iy = 0; iy < grid.ny(); ++iy)
            for (int ix = 0; ix < grid.nx(); ++ix) {
                const Vec2d  p = grid.node(ix, iy);
                const double h = 0.5 * pitch;
                smooth[size_t(iy) * grid.nx() + ix] = !std::isnan(S.at(ix, iy)) && !spans_wall(S, p + Vec2d(-h, -h)) &&
                                                      !spans_wall(S, p + Vec2d(h, -h)) && !spans_wall(S, p + Vec2d(-h, h)) &&
                                                      !spans_wall(S, p + Vec2d(h, h));
            }
        const float slit = float(scale_(0.5 * bead_scale(object.print()->config())));
        candidate        = union_ex(candidate, intersection_ex(diff_ex(closing_ex(to_polygons(candidate), slit), candidate),
                                                                mask_to_expolygons(smooth, grid)));
        if (whole_tops != nullptr) {
            Polygons all = to_polygons(*whole_tops);
            polygons_append(all, to_polygons(candidate));
            whole = closing_ex(all, slit);
        }
    }
    const Mask        cand_mask = rasterize(candidate, grid);
    throw_if_canceled(object);

    const PrintRegion &region       = object.printing_region(size_t(settings.region_id));
    const double       line_width   = region.flow(object, frTopSolidInfill, settings.shell_height).width();
    const ExPolygons   silhouette   = union_ex(silhouette_src);
    const double       wall_width   = std::max(0, region.config().wall_loops.value) *
                                region.flow(object, frPerimeter, settings.shell_height).width();
    if (settings.shells > settings.top_shells)
        U = undersides_within(U, S, closed_void, wall_width);
    ExPolygons         band_cand;
    static const bool no_band = [] {
        const char *v = research_switch("NP_CT1_NOBAND");
        return v != nullptr && *v != 0 && *v != '0';
    }();
    if (wall_width > 0. && !no_band) {
        band_cand = diff_ex(candidate, offset_ex(silhouette, float(-scale_(wall_width))));
        const int  deepest = object.config().nonplanar_mode.value == NonplanarMode::Auto ?
                                 std::max(settings.shells, planar_top_layers(region.config(), settings.shell_height)) : settings.shells;
        ExPolygons more    = diff_ex(plumb_wall_band(object, S, candidate, wall_width, settings.shells * settings.shell_height,
                                                     deepest * settings.shell_height, field_layers(object)), band_cand);
        more.erase(std::remove_if(more.begin(), more.end(), [](const ExPolygon &e) { return e.area() * SCALING_FACTOR * SCALING_FACTOR < 0.01; }),
                   more.end());
        if (!more.empty())
            band_cand = union_ex(band_cand, more);
    }
    const Mask         band_mask    = rasterize(band_cand, grid);
    Polygons voids;
    for (const ExPolygon &island : candidate)
        for (const Polygon &hole : island.holes)
            if (const Point c = hole.centroid(); std::isnan(S.sample(unscaled(c))) && hole.contains(c))
                voids.emplace_back(hole).make_counter_clockwise();
    const double       h_c          = settings.shell_height;
    const double       h_min        = 0.5 * h_c;
    const double       h_max        = 0.8 * region.flow(object, settings.shells == 1 ? frTopSolidInfill : frSolidInfill, h_c).nozzle_diameter();
    if (h_c > h_max) {
        PatchDiagnostics diag;
        diag.fallback_area   = area(candidate) * SCALING_FACTOR * SCALING_FACTOR;
        diag.fallback_reason = "shell_height";
        result.diagnostics.emplace_back(std::move(diag));
        return result;
    }
    const double       first_top    = layers.front()->print_z;
    std::vector<double> layer_z(layers.size());
    for (size_t i = 0; i < layers.size(); ++i)
        layer_z[i] = layers[i]->print_z;

    HeightField S0 = S;
    for (size_t i = 0; i < n_nodes; ++i)
        if (!cand_mask[i])
            S0.data()[i] = HeightField::NoData;
    if (settings.max_slope > 0.)
        S0 = slope_cap_minorant(S0, settings.max_slope);
    Mask mask(n_nodes, 0);
    for (size_t i = 0; i < n_nodes; ++i)
        mask[i] = cand_mask[i] && !std::isnan(S0.data()[i]);
    enum DropReason : uint8_t { Kept, DropNearBed, DropSteep, DropSteepStrip, DropFlatGround, DropFlatSliver, DropNarrow, DropPlanar, DropContact };
    static constexpr const char *drop_reason_tag[] = { "outline", "near_bed", "steep", "steep_strip", "flat_ground", "flat_sliver", "narrow", "planar_patch",
                                                       "contact" };
    std::vector<uint8_t> dropped(n_nodes, Kept);
    std::optional<SteepTops> steep_tops;
    auto drop = [&](size_t i, DropReason why) {
        mask[i]    = 0;
        dropped[i] = why;
    };
    if (const RepairHints *hints = object.nonplanar_repair(); hints != nullptr && !hints->planar.empty()) {
        const std::vector<uint8_t> off = planar_disc_mask(*hints, grid);
        for (size_t i = 0; i < n_nodes; ++i)
            if (off[i] && mask[i])
                drop(i, DropContact);
    }
    if (settings.banded && object.config().nonplanar_mode.value == NonplanarMode::Auto) {
        const Flow top_flow = region.flow(object, frTopSolidInfill, h_c);
        const double outer_w = banded_outer_width(line_width, top_flow.nozzle_diameter(), h_c, BandedCapWidth);
        Mask face(S.data().size(), 0);
        for (size_t i = 0; i < face.size(); ++i)
            face[i] = std::isfinite(S.data()[i]);
        const Mask planar = auto_planar_nodes(S, face, object.config().layer_height.value, line_width,
                                              std::max(h_c, object.config().layer_height.value) / outer_w);
        for (size_t i = 0; i < n_nodes; ++i)
            if (planar[i] && mask[i])
                drop(i, DropPlanar);
    }

    const int min_shells = settings.shells > settings.top_shells ? AutoMinShells : settings.top_shells;
    std::vector<float> bed_margin(n_nodes, NAN), steep_margin(n_nodes, NAN), flat_margin(n_nodes, NAN);
    if (settings.banded)
        for (size_t i = 0; i < n_nodes; ++i)
            if (mask[i]) {
                const double margin = S0.data()[i] - min_shells * h_c - h_min - first_top;
                bed_margin[i]       = float(margin);
                if (margin < 0.)
                    drop(i, DropNearBed);
            }
    if (settings.banded) {
        const double outer_w   = banded_outer_width(line_width, region.flow(object, frTopSolidInfill, h_c).nozzle_diameter(), h_c, BandedCapWidth);
        const PrintConfig &print_config = object.print()->config();
        const double tilt      = print_config.nonplanar_kinematics.value == RotaryLayout::None ? 0. :
                                     std::clamp(Geometry::deg2rad(print_config.nonplanar_max_tilt.value), 0., 0.5 * M_PI);
        const double max_slope = h_c / outer_w;
        auto local_height = [&layers](double z) {
            const auto it = std::lower_bound(layers.begin(), layers.end(), z, [](const Layer *layer, double v) { return layer->print_z < v; });
            return it == layers.end() ? layers.back()->height : (*it)->height;
        };
        const bool   field          = field_layers(object);
        const double field_band_max = field && tilt == 0. ? field_layer_slope_max() : 0.;
        if (tilt == 0.)
            steep_tops.emplace(head, h_c, head_band_height(head), h_max, max_slope);
        Mask                steep(n_nodes, 0), admitted(n_nodes, 0), tilted(n_nodes, 0);
        std::vector<double> slope(n_nodes, 0.);
        for (int iy = 0; iy < grid.ny(); ++iy)
            for (int ix = 0; ix < grid.nx(); ++ix)
                if (const size_t i = size_t(iy) * grid.nx() + ix; mask[i])
                    if (const Vec2d g = S0.gradient(grid.node(ix, iy)); g.allFinite()) {
                        const double eff = tilt > 0. ? std::tan(std::max(0., std::atan(g.norm()) - tilt)) : g.norm();
                        const double cap = std::max(max_slope, local_height(S0.at(ix, iy)) / outer_w);
                        steep_margin[i]  = float(cap - eff);
                        tilted[i] = tilt > 0. && eff <= cap && g.norm() > cap;
                        if (eff > cap) {
                            steep[i] = 1, slope[i] = eff;
                            admitted[i] = steep_tops && (!field || eff > std::max(max_slope, field_band_max)) &&
                                          steep_tops->outer_shells(g) > 0;
                        }
                    }
        if (tilt > 0.) {
            const Mask wide = rasterize(opening_ex(mask_to_expolygons(tilted, grid), float(scale_(BlendCoreRoads * line_width))), grid);
            for (int iy = 0; iy < grid.ny(); ++iy)
                for (int ix = 0; ix < grid.nx(); ++ix)
                    if (const size_t i = size_t(iy) * grid.nx() + ix; tilted[i] && !wide[i]) {
                        const double g   = S0.gradient(grid.node(ix, iy)).norm();
                        const double cap = std::max(max_slope, local_height(S0.at(ix, iy)) / outer_w);
                        steep[i] = 1, slope[i] = g, steep_margin[i] = float(cap - g);
                    }
        }
        if (steep_tops) {
            const Mask wide = rasterize(opening_ex(mask_to_expolygons(admitted, grid), float(scale_(0.5 * SteepMinWidth))), grid);
            for (size_t i = 0; i < n_nodes; ++i)
                if (admitted[i] && wide[i])
                    steep[i] = 0;
        }
        const Mask steep_wide = rasterize(opening_ex(mask_to_expolygons(steep, grid), float(scale_(line_width))), grid);
        for (size_t i = 0; i < n_nodes; ++i)
            if (steep[i] && steep_wide[i])
                drop(i, DropSteep);
        constexpr double FlatGround = 2.;
        Mask             flat(n_nodes, 0);
        for (int iy = 0; iy < grid.ny(); ++iy)
            for (int ix = 0; ix < grid.nx(); ++ix)
                if (const size_t i = size_t(iy) * grid.nx() + ix; mask[i])
                    if (const Vec2d g = S0.gradient(grid.node(ix, iy)); g.allFinite()) {
                        flat_margin[i] = float(g.norm() - FlatSlope);
                        flat[i]        = g.norm() < FlatSlope;
                    }
        const double min_z = region.config().zaa_enabled ? region.config().zaa_min_z.value : PrintRegionConfig::defaults().zaa_min_z.value;
        ExPolygons   flat_area;
        for (ExPolygon &ground : opening_ex(mask_to_expolygons(flat, grid), float(scale_(FlatGround)))) {
            const Mask on = rasterize(ExPolygons{ ground }, grid);
            double     lo = DBL_MAX, hi = -DBL_MAX;
            size_t     sample = n_nodes;
            for (size_t i = 0; i < n_nodes; ++i)
                if (on[i] && flat[i]) {
                    lo = std::min(lo, double(S0.data()[i])), hi = std::max(hi, double(S0.data()[i]));
                    sample = i;
                }
            if (sample == n_nodes)
                continue;
            {
                const Mask around = rasterize(offset_ex(ExPolygons{ ground }, float(scale_(FlatGround))), grid);
                size_t     ring = 0, shells = 0;
                for (size_t i = 0; i < n_nodes; ++i)
                    if (around[i] && !on[i]) {
                        ++ring;
                        shells += mask[i];
                    }
                if (ring > 0 && double(shells) >= 0.9 * double(ring))
                    continue;
            }
            const Point at = Point::new_scale(grid.node(int(sample % size_t(grid.nx())), int(sample / size_t(grid.nx()))));
            const Layer *host = nullptr;
            for (auto it = layers.rbegin(); it != layers.rend() && host == nullptr; ++it) {
                const double plane = (*it)->print_z - 0.5 * (*it)->height;
                if (plane > hi + EPSILON)
                    continue;
                if (plane < lo - EPSILON ||
                    std::any_of((*it)->lslices.begin(), (*it)->lslices.end(), [&at](const ExPolygon &e) { return e.contains(at); }))
                    host = *it;
            }
            if (host != nullptr && hi - host->print_z <= min_z + EPSILON && lo - host->print_z >= min_z - host->height - EPSILON)
                flat_area.emplace_back(std::move(ground));
        }
        const Mask       flat_planar = rasterize(flat_area, grid);
        for (size_t i = 0; i < n_nodes; ++i)
            if (flat_planar[i] && flat[i])
                drop(i, DropFlatGround);
        {
            constexpr double     StripSlope = 1.213;
            std::vector<uint8_t> seen(n_nodes, 0);
            std::vector<size_t>  strip, stack;
            for (size_t seed = 0; seed < n_nodes; ++seed) {
                if (seen[seed] || !steep[seed] || !mask[seed])
                    continue;
                strip.clear();
                stack.assign(1, seed);
                seen[seed]   = 1;
                while (!stack.empty()) {
                    const size_t i = stack.back();
                    stack.pop_back();
                    strip.push_back(i);
                    const int ix = int(i % size_t(grid.nx())), iy = int(i / size_t(grid.nx()));
                    for (const auto &[dx, dy] : { std::pair{ 1, 0 }, std::pair{ -1, 0 }, std::pair{ 0, 1 }, std::pair{ 0, -1 } })
                        if (const int jx = ix + dx, jy = iy + dy; jx >= 0 && jy >= 0 && jx < grid.nx() && jy < grid.ny())
                            if (const size_t j = size_t(jy) * grid.nx() + jx; !seen[j] && steep[j] && mask[j])
                                seen[j] = 1, stack.push_back(j);
                }
                std::vector<double> slopes;
                for (const size_t i : strip)
                    slopes.push_back(slope[i]);
                auto p90 = slopes.begin() + ptrdiff_t(0.9 * double(slopes.size() - 1));
                std::nth_element(slopes.begin(), p90, slopes.end());
                if (*p90 > StripSlope)
                    for (const size_t i : strip)
                        drop(i, DropSteepStrip);
            }
        }
        if (!flat_area.empty()) {
            const Mask near_flat = rasterize(offset_ex(flat_area, float(scale_(3. * line_width))), grid);
            const Mask wide      = rasterize(opening_ex(mask_to_expolygons(mask, grid), float(scale_(1.5 * line_width))), grid);
            for (size_t i = 0; i < n_nodes; ++i)
                if (mask[i] && near_flat[i] && !wide[i])
                    drop(i, DropFlatSliver);
        }
    }
    if (!settings.banded)
        drop_unsafe_nodes(S0, head, h_c, settings.max_error, mask);
    if (!settings.banded) {
        int                    n_comp = 0;
        const std::vector<int> label  = label_components(mask, grid.nx(), grid.ny(), n_comp);
        std::vector<int>       ix0(size_t(n_comp), grid.nx()), iy0(size_t(n_comp), grid.ny()), ix1(size_t(n_comp), -1), iy1(size_t(n_comp), -1);
        for (int iy = 0; iy < grid.ny(); ++iy)
            for (int ix = 0; ix < grid.nx(); ++ix)
                if (const int c = label[size_t(iy) * grid.nx() + ix]; c >= 0) {
                    ix0[size_t(c)] = std::min(ix0[size_t(c)], ix), iy0[size_t(c)] = std::min(iy0[size_t(c)], iy);
                    ix1[size_t(c)] = std::max(ix1[size_t(c)], ix), iy1[size_t(c)] = std::max(iy1[size_t(c)], iy);
                }
        std::vector<std::vector<size_t>> low(static_cast<size_t>(n_comp));
        tbb::parallel_for(tbb::blocked_range<int>(0, n_comp), [&](const tbb::blocked_range<int> &range) {
            for (int c = range.begin(); c < range.end(); ++c) {
                const size_t k = size_t(c);
                HeightField  own = crop(S0, ix0[k], iy0[k], ix1[k], iy1[k]);
                for (int iy = 0; iy < own.ny(); ++iy)
                    for (int ix = 0; ix < own.nx(); ++ix)
                        if (label[size_t(iy0[k] + iy) * grid.nx() + ix0[k] + ix] != c)
                            own.at(ix, iy) = HeightField::NoData;
                const HeightField safe  = min_safe_tip_height(own, head, own, true);
                const HeightField lower = head_lipschitz_minorant(own, head, 8, cancel);
                size_t            high  = 0;
                for (int iy = 0; iy < own.ny(); ++iy)
                    for (int ix = 0; ix < own.nx(); ++ix)
                        if (const float z = own.at(ix, iy); !std::isnan(z)) {
                            high += !(z - lower.at(ix, iy) <= settings.max_error);
                            if (z < safe.at(ix, iy) - settings.max_error)
                                low[k].emplace_back(size_t(iy0[k] + iy) * grid.nx() + ix0[k] + ix);
                        }
                if (low[k].size() >= high)
                    low[k].clear();
            }
        });
        for (const std::vector<size_t> &l : low)
            for (size_t i : l)
                mask[i] = 0;
    }

    HeightField cap, S1u;
    if (const RepairHints *hints = object.nonplanar_repair(); hints != nullptr && !hints->surface_caps.empty())
        cap = surface_cap_field(*hints, grid), S1u = grid;

    HeightField S1        = settings.banded ? S0 : grid;
    bool        converged = settings.banded;
    if (settings.banded && !cap.empty()) {
        S1u = S0;
        S1 = apply_surface_cap(S0, cap);
    }
    for (int round = 0; round < 16 && !converged; ++round) {
        int                    n_comp = 0;
        const std::vector<int> label  = label_components(mask, grid.nx(), grid.ny(), n_comp);
        struct Extent { int ix0, iy0, ix1, iy1; float zcap; };
        std::vector<Extent> ext(size_t(n_comp), Extent{ grid.nx(), grid.ny(), -1, -1, -Inf });
        for (int iy = 0; iy < grid.ny(); ++iy)
            for (int ix = 0; ix < grid.nx(); ++ix)
                if (const int c = label[size_t(iy) * grid.nx() + ix]; c >= 0) {
                    Extent &e = ext[size_t(c)];
                    e.ix0 = std::min(e.ix0, ix), e.iy0 = std::min(e.iy0, iy), e.ix1 = std::max(e.ix1, ix), e.iy1 = std::max(e.iy1, iy);
                }
        auto component_field = [&](const HeightField &src, int c) {
            const Extent &e   = ext[size_t(c)];
            HeightField   out = crop(src, e.ix0, e.iy0, e.ix1, e.iy1);
            for (int iy = 0; iy < out.ny(); ++iy)
                for (int ix = 0; ix < out.nx(); ++ix)
                    if (label[size_t(e.iy0 + iy) * grid.nx() + e.ix0 + ix] != c)
                        out.at(ix, iy) = HeightField::NoData;
            return out;
        };
        std::fill(S1.data().begin(), S1.data().end(), HeightField::NoData);
        tbb::parallel_for(tbb::blocked_range<int>(0, n_comp), [&](const tbb::blocked_range<int> &range) {
            for (int c = range.begin(); c < range.end(); ++c) {
                const Extent     &e  = ext[size_t(c)];
                const HeightField s0 = component_field(S0, c);
                const HeightField s1 = head_lipschitz_minorant(cap.empty() ? s0 : apply_surface_cap(s0, component_field(cap, c)), head, 8, cancel);
                const HeightField s1u = cap.empty() ? HeightField() : head_lipschitz_minorant(s0, head, 8, cancel);
                for (int iy = 0; iy < s1.ny(); ++iy)
                    for (int ix = 0; ix < s1.nx(); ++ix)
                        if (label[size_t(e.iy0 + iy) * grid.nx() + e.ix0 + ix] == c) {
                            S1.at(e.ix0 + ix, e.iy0 + iy) = s1.at(ix, iy);
                            if (!cap.empty())
                                S1u.at(e.ix0 + ix, e.iy0 + iy) = s1u.at(ix, iy);
                        }
            }
        });
        bool changed = false;
        for (size_t i = 0; i < n_nodes; ++i) {
            if (!mask[i])
                continue;
            const float s1 = S1.data()[i];
            if (std::isnan(s1) || S.data()[i] - (cap.empty() ? s1 : S1u.data()[i]) > settings.max_error ||
                s1 - settings.shells * h_c - h_min < first_top ||
                planar_transition_layer(layer_z, s1 - (settings.shells - 1) * h_c, h_c, h_min, h_max, PatchKind::Top) < 0) {
                mask[i] = 0;
                changed = true;
            } else
                ext[size_t(label[i])].zcap = std::max(ext[size_t(label[i])].zcap, s1);
        }
        std::vector<std::vector<size_t>> drops(static_cast<size_t>(n_comp));
        auto host_print_z = [&layers](float z) {
            const auto it = std::lower_bound(layers.begin(), layers.end(), double(z) - EPSILON,
                                             [](const Layer *layer, double v) { return layer->print_z < v; });
            return float(it == layers.end() ? layers.back()->print_z : (*it)->print_z);
        };
        tbb::parallel_for(tbb::blocked_range<int>(0, n_comp), [&](const tbb::blocked_range<int> &range) {
            for (int c = range.begin(); c < range.end(); ++c) {
                const Extent &e = ext[size_t(c)];
                if (e.zcap == -Inf)
                    continue;
                const float host_z = host_print_z(e.zcap);
                const double lowest_shell = (settings.shells - 1) * h_c;
                const HeightField like = component_field(S1, c);
                float like_min = Inf;
                for (float z : like.data())
                    if (!std::isnan(z))
                        like_min = std::min(like_min, z);
                HeadEnvelope near_head = head;
                near_head.boxes.clear();
                bool all_x = false, all_y = false;
                double rx = 0., ry = 0.;
                for (const HeadBox &box : head.boxes)
                    if (box.min.z() - head.margin <= host_z - like_min + lowest_shell) {
                        const double lx = std::max(std::abs(box.min.x()), std::abs(box.max.x())) + head.lateral_margin;
                        const double ly = std::max(std::abs(box.min.y()), std::abs(box.max.y())) + head.lateral_margin;
                        box.infinite_x ? void(all_x = true) : void(rx = std::max(rx, lx));
                        box.infinite_y ? void(all_y = true) : void(ry = std::max(ry, ly));
                        near_head.boxes.emplace_back(box);
                    }
                if (near_head.boxes.empty())
                    continue;
                const int wnx = int(std::ceil(rx / pitch)) + 2, wny = int(std::ceil(ry / pitch)) + 2;
                const int wx0 = all_x ? 0 : std::max(0, e.ix0 - wnx), wx1 = all_x ? grid.nx() - 1 : std::min(grid.nx() - 1, e.ix1 + wnx);
                const int wy0 = all_y ? 0 : std::max(0, e.iy0 - wny), wy1 = all_y ? grid.ny() - 1 : std::min(grid.ny() - 1, e.iy1 + wny);
                HeightField H = crop(grid, wx0, wy0, wx1, wy1);
                for (int iy = wy0; iy <= wy1; ++iy)
                    for (int ix = wx0; ix <= wx1; ++ix)
                        if (const size_t i = size_t(iy) * grid.nx() + ix;
                            (label[i] != c || !mask[i] || band_mask[i]) && !std::isnan(S.data()[i]))
                            H.at(ix - wx0, iy - wy0) = std::min(S.data()[i], host_z);
                const HeightField z_safe = min_safe_tip_height(H, near_head, like, true);
                for (int iy = 0; iy < like.ny(); ++iy)
                    for (int ix = 0; ix < like.nx(); ++ix) {
                        const size_t i = size_t(e.iy0 + iy) * grid.nx() + e.ix0 + ix;
                        if (label[i] == c && mask[i] && !band_mask[i] && like.at(ix, iy) - lowest_shell < z_safe.at(ix, iy))
                            drops[size_t(c)].emplace_back(i);
                    }
            }
        });
        for (const std::vector<size_t> &d : drops)
            for (size_t i : d) {
                mask[i] = 0;
                changed = true;
            }
        throw_if_canceled(object);
        converged = !changed;
    }
    if (!converged) {
        BOOST_LOG_TRIVIAL(warning) << "Non-planar: head clipping did not converge, no curved shells for " << object.model_object()->name;
        std::fill(mask.begin(), mask.end(), 0);
    }

    auto footprint_cells = [&]() {
        constexpr float MinCross   = 0.05f;
        constexpr float CreaseStep = 0.2f;
        const int       nx = grid.nx(), ny = grid.ny();
        auto margin = [&](uint8_t why, size_t i) {
            return why == DropNearBed                             ? bed_margin[i] :
                   why == DropSteep || why == DropSteepStrip      ? steep_margin[i] :
                   why == DropFlatGround                          ? flat_margin[i] :
                                                                    NAN;
        };
        auto neighbours = [&](size_t i, auto &&fn) {
            const int ix = int(i % size_t(nx)), iy = int(i / size_t(nx));
            for (const auto &[dx, dy] : { std::pair{ 1, 0 }, std::pair{ -1, 0 }, std::pair{ 0, 1 }, std::pair{ 0, -1 } })
                if (const int jx = ix + dx, jy = iy + dy; jx >= 0 && jy >= 0 && jx < nx && jy < ny)
                    fn(size_t(jy) * nx + jx);
        };
        auto distance = [&](uint8_t why, size_t i) {
            const float m = margin(why, i);
            if (std::isnan(m))
                return 1.f;
            float step = 0.f;
            neighbours(i, [&](size_t j) {
                if (const float n = margin(why, j); !std::isnan(n) && (m < 0.f) != (n < 0.f))
                    step = std::max(step, std::abs(m - n));
            });
            const float slope_step = why == DropNearBed ? step / float(pitch) : step;
            return step > 0.f && slope_step <= CreaseStep ? std::clamp(std::abs(m) / step, MinCross, 1.f) : 1.f;
        };
        HeightField field = grid;
        for (size_t i = 0; i < n_nodes; ++i) {
            float &v = field.data()[i];
            v        = HeightField::NoData;
            if (!cand_mask[i])
                continue;
            if (!mask[i]) {
                if (dropped[i] != Kept)
                    v = -distance(dropped[i], i);
                continue;
            }
            v = 1.f;
            neighbours(i, [&](size_t j) {
                if (!mask[j] && dropped[j] != Kept) {
                    const float m = margin(dropped[j], i);
                    v = std::min(v, std::isnan(m) || m < 0.f ? distance(dropped[j], j) : distance(dropped[j], i));
                }
            });
        }
        HeightField mirrored = field;
        for (size_t j = 0; j < n_nodes; ++j)
            if (!cand_mask[j]) {
                float v = 0.f;
                neighbours(j, [&](size_t i) {
                    if (const float u = field.data()[i]; u > 0.f)
                        v = std::max(v, u);
                });
                if (v > 0.f)
                    mirrored.data()[j] = -v;
            }
        return positive_region(mirrored);
    };
    const float narrow     = float(scale_(0.5 * line_width));
    ExPolygons  components = opening_ex(mask_to_expolygons(mask, grid), narrow);
    if (settings.banded)
        components = intersection_ex(opening_ex(footprint_cells(), narrow), offset_ex(components, float(scale_(pitch))));
    if (settings.banded) {
        const Mask opened = rasterize(components, grid);
        for (size_t i = 0; i < n_nodes; ++i)
            if (mask[i] && !opened[i])
                dropped[i] = DropNarrow;
    }
    components            = settings.banded ? intersection_ex(components, candidate) :
                                              intersection_ex(offset_ex(expolygons_simplify(components, scale_(pitch)), -float(scale_(pitch))),
                                                              candidate);
    if (const double rim = ct1_rim_mm(); rim > 0.)
        components = opening_ex(offset_ex(components, -float(scale_(rim))), narrow);
    if (whole_tops != nullptr && settings.banded)
        if (const ExPolygons other = diff_ex(whole, candidate); !other.empty())
            components = union_ex(components, intersection_ex(intersection_ex(offset_ex(components, float(scale_(1.5 * pitch))), candidate),
                                                              offset_ex(other, float(scale_(1.5 * pitch)))));
    {
        std::vector<BoundingBox> island_bboxes;
        for (const ExPolygon &island : candidate)
            island_bboxes.emplace_back(get_extents(island.contour));
        const bool rotary = settings.banded && object.print()->config().nonplanar_kinematics.value != RotaryLayout::None;
        ExPolygons flat_ground;
        if (rotary) {
            Mask flat(n_nodes, 0);
            for (int iy = 0; iy < grid.ny(); ++iy)
                for (int ix = 0; ix < grid.nx(); ++ix)
                    if (const Vec2d g = S0.gradient(grid.node(ix, iy)); g.allFinite() && g.norm() < FlatBlendSlope)
                        flat[size_t(iy) * grid.nx() + ix] = 1;
            flat_ground = mask_to_expolygons(flat, grid);
        }
        std::vector<BoundingBox> flat_bboxes;
        for (const ExPolygon &f : flat_ground)
            flat_bboxes.emplace_back(get_extents(f.contour));
        const auto sloped_core = [&](const ExPolygon &c, double &sloped) {
            const BoundingBox bbox = get_extents(c.contour);
            ExPolygons        near;
            for (size_t k = 0; k < flat_ground.size(); ++k)
                if (flat_bboxes[k].overlap(bbox))
                    near.emplace_back(flat_ground[k]);
            const ExPolygons part = near.empty() ? ExPolygons{ c } : diff_ex(ExPolygons{ c }, near);
            sloped = area(part) * SCALING_FACTOR * SCALING_FACTOR;
            return sloped > 0. && area(opening_ex(part, float(scale_(BlendCoreRoads * line_width)))) >= 0.5 * area(part);
        };
        ExPolygons kept;
        size_t     refused = 0;
        for (ExPolygon &c : components) {
            const double a      = c.area() * SCALING_FACTOR * SCALING_FACTOR;
            double       sloped = 0.;
            const bool   core   = !rotary || sloped_core(c, sloped);
            const bool   round  = rotary && sloped <= BlendFlatShare * a && a >= settings.min_area && is_round(ExPolygons{ c });
            bool         keep   = rotary ? round || (core && sloped >= settings.min_area) : settings.banded || a >= settings.min_area;
            for (size_t k = 0; !keep && core && a >= 1. && k < candidate.size(); ++k)
                if (island_bboxes[k].contains(c.contour.points.front()) && candidate[k].contains(c.contour.points.front()))
                    keep = a >= 0.25 * candidate[k].area() * SCALING_FACTOR * SCALING_FACTOR;
            if (keep) {
                kept.emplace_back(std::move(c));
                continue;
            }
            ++refused;
            if (rotary) {
                const Mask on = rasterize(ExPolygons{ c }, grid);
                float      lo = Inf, hi = -Inf;
                for (size_t i = 0; i < n_nodes; ++i)
                    if (on[i] && !std::isnan(S0.data()[i]))
                        lo = std::min(lo, S0.data()[i]), hi = std::max(hi, S0.data()[i]);
                const ExPolygons grown = intersection_ex(offset_ex(ExPolygons{ c }, float(scale_(wall_width + pitch))), candidate);
                for (size_t l = 0; l < layers.size() && lo <= hi; ++l)
                    if (layers[l]->print_z >= lo - layers[l]->height && layers[l]->print_z - layers[l]->height <= hi)
                        append(result.shells_refused[l], grown);
            }
        }
        if (rotary)
            for (ExPolygons &r : result.shells_refused)
                if (r.size() > 1)
                    r = union_ex(r);
        if (refused > 0) {
            const Mask on = rasterize(kept, grid);
            for (size_t i = 0; i < n_nodes; ++i)
                if (mask[i] && !on[i])
                    mask[i] = 0, dropped[i] = DropNarrow;
            BOOST_LOG_TRIVIAL(info) << "shells refused: " << refused << " of " << refused + kept.size()
                                    << " components cannot blend (sloped part under the minimum area or twelve roads across), planar there";
        }
        components = std::move(kept);
    }

    const RepairHints *repair = object.nonplanar_repair();
    if (!settings.banded && (settings.max_patch_height > 0. || (repair != nullptr && !repair->order_bands.empty()))) {
        ExPolygons bands;
        for (const ExPolygon &component : components) {
            double max_height = settings.max_patch_height > 0. ? settings.max_patch_height : Inf;
            if (repair != nullptr)
                for (const RepairHints::OrderBand &b : repair->order_bands)
                    if (component.contains(Point::new_scale(b.p.x(), b.p.y())))
                        max_height = std::min(max_height, b.height);
            const Mask pmask = rasterize({ component }, grid);
            float zlo = Inf, zhi = -Inf;
            for (size_t i = 0; i < n_nodes; ++i)
                if (pmask[i] && !std::isnan(S1.data()[i])) {
                    zlo = std::min(zlo, S1.data()[i]);
                    zhi = std::max(zhi, S1.data()[i]);
                }
            if (zhi - zlo <= max_height) {
                bands.emplace_back(component);
                continue;
            }
            const int count = int(std::ceil((zhi - zlo) / max_height));
            const double band_height = (zhi - zlo) / count;
            for (int band = 0; band < count; ++band) {
                Mask selected(n_nodes, 0);
                for (size_t i = 0; i < n_nodes; ++i)
                    if (pmask[i] && !std::isnan(S1.data()[i])) {
                        const int owner = std::min(count - 1, int((S1.data()[i] - zlo) / band_height));
                        selected[i] = owner == band;
                    }
                append(bands, intersection_ex(mask_to_expolygons(selected, grid), ExPolygons{ component }));
            }
        }
        components = std::move(bands);
        auto highest = [&](const ExPolygon &component) {
            const Mask pmask = rasterize({ component }, grid);
            float z = -Inf;
            for (size_t i = 0; i < n_nodes; ++i)
                if (pmask[i] && !std::isnan(S1.data()[i]))
                    z = std::max(z, S1.data()[i]);
            return z;
        };
        std::stable_sort(components.begin(), components.end(), [&](const ExPolygon &a, const ExPolygon &b) {
            return highest(a) < highest(b);
        });
    }

    std::vector<HeightField> planar_tops;
    std::vector<HeightField> stack_layers;
    std::vector<AdaptiveStack> adaptive_stacks;
    const double             nozzle       = region.flow(object, frTopSolidInfill, h_c).nozzle_diameter();
    std::vector<ExPolygons>  shell_areas;
    std::vector<std::pair<ExPolygon, const char *>> skipped;
    const int                solid_layers = settings.shells - settings.sparse;
    std::vector<Mask> planar_solid(layers.size());
    auto solid_at = [&](size_t l, size_t n) {
        if (planar_solid[l].empty()) {
            ExPolygons solid;
            for (const LayerRegion *layerm : layers[l]->regions())
                for (const Surface &surface : layerm->fill_surfaces.surfaces)
                    if (surface.surface_type != stInternal && surface.surface_type != stInternalVoid)
                        solid.emplace_back(surface.expolygon);
            planar_solid[l] = rasterize(solid, grid);
        }
        return planar_solid[l][n] != 0;
    };
    const double             top_thickness = std::max(region.config().top_shell_layers.value * object.config().layer_height.value,
                                                      region.config().top_shell_thickness.value);
    for (ExPolygon &component : components) {
        const Mask pmask = rasterize({ component }, grid);
        int        ix0 = grid.nx(), iy0 = grid.ny(), ix1 = -1, iy1 = -1;
        float      zlo = Inf, zhi = -Inf;
        for (int iy = 0; iy < grid.ny(); ++iy)
            for (int ix = 0; ix < grid.nx(); ++ix)
                if (const size_t i = size_t(iy) * grid.nx() + ix; pmask[i] && !std::isnan(S1.data()[i])) {
                    ix0 = std::min(ix0, ix), iy0 = std::min(iy0, iy), ix1 = std::max(ix1, ix), iy1 = std::max(iy1, iy);
                    zlo = std::min(zlo, S1.data()[i]), zhi = std::max(zhi, S1.data()[i]);
                }
        if (ix1 < 0 || zhi - zlo < 0.01) {
            skipped.emplace_back(component, "flat");
            continue;
        }
        const double nozzle_d     = object.print()->config().nozzle_diameter.get_at(0);
        const bool   fine_process = object.config().nonplanar_mode.value == NonplanarMode::Auto &&
                                  object.print()->config().nonplanar_kinematics.value == RotaryLayout::None && h_c <= 0.3 * nozzle_d + EPSILON;
        if (field_layers(object) || fine_process) {
            std::vector<float> slopes;
            for (int iy = iy0; iy <= iy1; ++iy)
                for (int ix = ix0; ix <= ix1; ++ix)
                    if (const size_t i = size_t(iy) * grid.nx() + ix; pmask[i] && !std::isnan(S1.data()[i]))
                        if (const Vec2d g = S1.gradient(grid.node(ix, iy)); g.allFinite())
                            slopes.emplace_back(float(g.norm()));
            if (!slopes.empty()) {
                auto p90 = slopes.begin() + ptrdiff_t(0.9 * double(slopes.size() - 1));
                std::nth_element(slopes.begin(), p90, slopes.end());
                if (field_layers(object) && *p90 < std::tan(Geometry::deg2rad(1.))) {
                    skipped.emplace_back(component, "flat");
                    continue;
                }
                if (fine_process && *p90 > h_c / banded_outer_width(line_width, nozzle_d, h_c, BandedCapWidth)) {
                    skipped.emplace_back(component, "steep");
                    continue;
                }
            }
        }
        const int pad = 3;
        ix0 = std::max(0, ix0 - pad), iy0 = std::max(0, iy0 - pad);
        ix1 = std::min(grid.nx() - 1, ix1 + pad), iy1 = std::min(grid.ny() - 1, iy1 + pad);

        ConformalPatch patch;
        patch.id           = patch_offset + int(result.patches.size());
        patch.kind         = PatchKind::Top;
        patch.footprint    = { component };
        patch.wall_band    = intersection_ex(patch.footprint, band_cand);
        patch.shells       = settings.shells;
        patch.shell_height = h_c;
        patch.region_id    = settings.region_id;
        patch.surface      = crop(S1, ix0, iy0, ix1, iy1);
        for (int iy = 0; iy < patch.surface.ny(); ++iy)
            for (int ix = 0; ix < patch.surface.nx(); ++ix)
                if (!pmask[size_t(iy0 + iy) * grid.nx() + ix0 + ix])
                    patch.surface.edit().at(ix, iy) = HeightField::NoData;
        {
            const ExPolygons steep_faces = steep_tops ? steep_area(patch.surface, steep_tops->cap()) : ExPolygons();
            choose_patch_settings(object, head, settings.max_error, first_top, patch, &steep_faces);
        }
        if (settings.shells > settings.top_shells)
            patch.shells = settings.shells;
        if (settings.shells > settings.top_shells)
            patch.sparse = settings.sparse;

        PatchDiagnostics diag;
        diag.patch_id   = patch.id;
        double err_sq   = 0.;
        size_t err_n    = 0;
        HeightField top = patch.surface;
        patch.base      = patch.surface;
        patch.z_min     = Inf;
        patch.z_max     = -Inf;
        for (int iy = 0; iy < patch.surface.ny(); ++iy)
            for (int ix = 0; ix < patch.surface.nx(); ++ix) {
                const float s1 = patch.surface.at(ix, iy);
                if (std::isnan(s1))
                    continue;
                const double b = s1 - patch.shells * h_c;
                patch.base.edit().at(ix, iy) = float(b);
                patch.z_max    = std::max(patch.z_max, double(s1));
                const double e = S.at(ix0 + ix, iy0 + iy) - s1;
                if (!cap.empty())
                    if (const double lowered = S1u.at(ix0 + ix, iy0 + iy) - s1; lowered > 1e-4) {
                        ++diag.repaired_nodes;
                        diag.repair_lowering = std::max(diag.repair_lowering, lowered);
                    }
                diag.max_surface_error = std::max(diag.max_surface_error, e);
                err_sq += e * e;
                ++err_n;
                const Vec2d g = patch.surface.gradient(patch.surface.node(ix, iy));
                if (!std::isnan(g.x()) && !std::isnan(g.y()))
                    diag.max_slope_deg = std::max(diag.max_slope_deg, Geometry::rad2deg(std::atan(g.norm())));
            }
        diag.rms_surface_error = err_n > 0 ? std::sqrt(err_sq / err_n) : 0.;
        pad_lowest(patch.surface.edit(), pad);
        pad_lowest(patch.base.edit(), pad);
        ExPolygons  shell_area = diff_ex(patch.footprint, patch.wall_band);
        {
            BoundingBox near = get_extents(shell_area);
            near.offset(scale_(1. + 4. * pitch));
            Polygons material;
            for (const Layer *layer : layers)
                if (layer->print_z > zlo - patch.shells * h_c - EPSILON && layer->print_z - layer->height < zhi + EPSILON)
                    polygons_append(material, ClipperUtils::clip_clipper_polygons_with_subject_bbox(layer->lslices, near));
            const Polygons solid = union_(material);
            ExPolygons gaps = diff_ex(intersection_ex(shell_area, closing(solid, float(scale_(2. * pitch)))), solid);
            gaps.erase(std::remove_if(gaps.begin(), gaps.end(), [](const ExPolygon &g) { return g.area() * SCALING_FACTOR * SCALING_FACTOR < 0.01; }),
                       gaps.end());
            if (!gaps.empty())
                shell_area = diff_ex(shell_area, gaps);
        }
        Mask        stacks     = rasterize(offset_ex(shell_area, -float(scale_(line_width))), top);
        Mask        at_wall(stacks.size(), 0);
        {
            const int nx = top.nx(), ny = top.ny(), reach = int(std::ceil(line_width / top.pitch()));
            for (int iy = 0; iy + 1 < ny; ++iy)
                for (int ix = 0; ix + 1 < nx; ++ix)
                    if (spans_wall(S, top.node(ix, iy) + Vec2d(0.5 * top.pitch(), 0.5 * top.pitch()))) {
                        for (int jy = iy; jy <= iy + 1; ++jy)
                            for (int jx = ix; jx <= ix + 1; ++jx)
                                at_wall[size_t(jy) * nx + jx] = 1;
                        for (int jy = std::max(0, iy - reach); jy <= std::min(ny - 1, iy + 1 + reach); ++jy)
                            for (int jx = std::max(0, ix - reach); jx <= std::min(nx - 1, ix + 1 + reach); ++jx)
                                stacks[size_t(jy) * nx + jx] = 0;
                    }
        }
        bool        printable = true;
        HeightField count     = top;
        const int   n_top     = settings.shells > settings.top_shells ? settings.top_shells : patch.shells;
        std::optional<AdaptiveStack> stack;
        if (settings.adaptive)
            stack = adaptive_stack(top, HeightField(), n_top * h_c, h_c, patch.shells - n_top, nozzle,
                                   banded_outer_width(line_width, nozzle, h_c), settings.max_error);
        if (stack && repair != nullptr)
            for (const RepairHints::Disc &d : repair->uniform_beads)
                for (size_t i = 0; i < stack->outer.data().size(); ++i)
                    if (!std::isnan(stack->outer.data()[i]) &&
                        (stack->outer.node(int(i % size_t(stack->outer.nx())), int(i / size_t(stack->outer.nx()))) - d.p).norm() <= d.radius)
                        stack->make_uniform(i);
        auto thick_layer_transition = [&](double face) {
            const auto above = std::partition_point(layer_z.begin(), layer_z.end(), [&](double z) { return face - z >= h_min; });
            return above == layer_z.begin() || above == layer_z.end() ? -1 : int(above - layer_z.begin()) - 1;
        };
        const bool fine_layers = field_layers(object);
        for (size_t i = 0; i < top.data().size(); ++i) {
            const float s = patch.surface.data()[i];
            if (std::isnan(s))
                continue;
            int layer = -1, m = stacks[i] ? patch.shells : n_top;
            for (; m >= (settings.shells > settings.top_shells ? std::min(n_top, min_shells) : n_top); --m) {
                if (m > n_top) {
                    const float u = U.at(ix0 + int(i % size_t(top.nx())), iy0 + int(i / size_t(top.nx())));
                    if (std::isnan(u) || s - m * h_c - h_min < u + (m > solid_layers ? settings.bottom_keep : h_c))
                        continue;
                    if (const int patch_solid = patch.shells - patch.sparse; m > patch_solid) {
                        const size_t n    = size_t(iy0 + int(i / size_t(top.nx()))) * size_t(grid.nx()) + size_t(ix0 + int(i % size_t(top.nx())));
                        const double zlo  = s - m * h_c, zhi = s - patch_solid * h_c;
                        bool         over = false;
                        for (size_t l = size_t(std::lower_bound(layer_z.begin(), layer_z.end(), zlo) - layer_z.begin());
                             l < layers.size() && layer_z[l] - 0.5 * layers[l]->height < zhi && !over; ++l)
                            over = solid_at(l, n);
                        if (over)
                            continue;
                    }
                }
                if (stack && m == n_top && !std::isnan(stack->outer.data()[i])) {
                    const double h0   = stack->outer.data()[i];
                    const int    n    = int(stack->count.data()[i]);
                    const double bead = n > 1 ? stack->inner_height(h0, n) : h0;
                    if ((layer = planar_transition_layer(layer_z, s - m * h_c + bead, bead, h_min, h_max, PatchKind::Top, fine_layers)) >= 0)
                        break;
                    stack->make_uniform(i);
                }
                if ((layer = planar_transition_layer(layer_z, s - (m - 1) * h_c - ct1_base_gap_mm(), h_c, h_min, h_max, PatchKind::Top, fine_layers)) >= 0)
                    break;
            }
            if (layer < 0) {
                m     = n_top;
                layer = thick_layer_transition(s - (m - 1) * h_c);
            }
            if (layer < 0) {
                if (std::isnan(top.data()[i]))
                    continue;
                printable = false;
                break;
            }
            count.data()[i] = float(m);
            top.data()[i]   = float(layer_z[size_t(layer)]);
            patch.z_min = std::min(patch.z_min, layer_z[size_t(layer)]);
        }
        pad_lowest(count, pad);
        pad_lowest(top, pad);
        if (settings.banded)
            for (size_t i = 0; i < at_wall.size(); ++i)
                if (at_wall[i] && !std::isnan(top.data()[i]))
                    top.data()[i] = Inf;
        if (!printable) {
            skipped.emplace_back(component, "no_transition_layer");
            continue;
        }
        if (stack) {
            if (patch.shells > n_top)
                stack->layers = count;
            stack->update_zones();
            patch.shells = stack->shells();
            adaptive_stacks.emplace_back(std::move(*stack));
        }

        patch.host_layer = size_t(std::lower_bound(layer_z.begin(), layer_z.end(), patch.z_max - EPSILON) - layer_z.begin());
        if (patch.host_layer >= layers.size())
            patch.host_layer = layers.size() - 1;

        planar_tops.emplace_back(std::move(top));
        stack_layers.emplace_back(std::move(count));
        shell_areas.emplace_back(std::move(shell_area));
        result.diagnostics.emplace_back(std::move(diag));
        result.patches.emplace_back(std::move(patch));
    }

    for (const ExPolygon &cand : candidate) {
        double     covered = 0.;
        int        owner   = -1;
        const auto cand_bb = get_extents(cand);
        ExPolygons covering;
        for (const ConformalPatch &patch : result.patches)
            if (get_extents(patch.footprint).overlap(cand_bb))
                if (const double a = area(intersection_ex(patch.footprint, ExPolygons{ cand })); a > 0.) {
                    covered += a;
                    expolygons_append(covering, patch.footprint);
                    if (owner < 0)
                        owner = patch.id;
                }
        const double fallback = std::max(0., cand.area() - covered) * SCALING_FACTOR * SCALING_FACTOR;
        if (settings.banded && fallback > 0.) {
            Polygons uncovered = diff(ExPolygons{ cand }, covering);
            for (const auto &[component, reason] : skipped)
                if (get_extents(component).overlap(cand_bb)) {
                    ExPolygons part = intersection_ex(ExPolygons{ component }, uncovered);
                    if (const double a = area(part) * SCALING_FACTOR * SCALING_FACTOR; a > 0.) {
                        PatchDiagnostics diag;
                        diag.fallback_area   = a;
                        diag.fallback_reason = reason;
                        if (fallback_hatched(reason))
                            record_fallback_shape(diag, std::move(part), S);
                        result.diagnostics.emplace_back(std::move(diag));
                    }
                    uncovered = diff(uncovered, ExPolygons{ component });
                }
            const double            rest = area(uncovered) * SCALING_FACTOR * SCALING_FACTOR;
            std::array<double, std::size(drop_reason_tag)> by_reason{};
            const Mask              umask = rasterize(uncovered, grid);
            for (size_t i = 0; i < n_nodes; ++i)
                if (umask[i])
                    by_reason[dropped[i]] += pitch * pitch;
            const double nodes = std::accumulate(by_reason.begin() + 1, by_reason.end(), 0.);
            const double scale = nodes > rest ? rest / nodes : 1.;
            by_reason[Kept]    = std::max(0., rest - nodes * scale);
            for (size_t r = 0; r < by_reason.size(); ++r)
                if (by_reason[r] * (r == Kept ? 1. : scale) > 1e-3) {
                    PatchDiagnostics diag;
                    diag.fallback_area   = by_reason[r] * (r == Kept ? 1. : scale);
                    diag.fallback_reason = drop_reason_tag[r];
                    if (r != Kept && fallback_hatched(diag.fallback_reason)) {
                        Mask nodes_mask(n_nodes, 0);
                        for (size_t i = 0; i < n_nodes; ++i)
                            nodes_mask[i] = umask[i] && dropped[i] == r;
                        record_fallback_shape(diag, mask_to_expolygons(nodes_mask, grid), S);
                    }
                    result.diagnostics.emplace_back(std::move(diag));
                }
        } else if (owner >= 0) {
            PatchDiagnostics &diag = result.diagnostics[size_t(owner - patch_offset)];
            diag.fallback_area += fallback;
            diag.fallback_reason = "edge";
            if (fallback > 0.) {
                ExPolygons left = diff_ex(ExPolygons{ cand }, covering);
                expolygons_append(left, std::move(diag.fallback_shape));
                diag.fallback_hatch.clear();
                record_fallback_shape(diag, union_ex(left), S);
            }
        } else if (fallback > 0.) {
            PatchDiagnostics diag;
            diag.fallback_area = fallback;
            double best = 0.;
            diag.fallback_reason = settings.banded ? "edge" : "head";
            for (const auto &[component, reason] : skipped)
                if (const double a = area(intersection_ex(ExPolygons{ component }, ExPolygons{ cand })); a > best) {
                    best                 = a;
                    diag.fallback_reason = reason;
                }
            if (fallback_hatched(diag.fallback_reason))
                record_fallback_shape(diag, ExPolygons{ cand }, S);
            result.diagnostics.emplace_back(std::move(diag));
        }
    }
    if (result.patches.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Non-planar: no curved top patch in " << object.model_object()->name;
        return result;
    }

    const double material_max_patch_height = settings.banded ?
        multi_material_patch_height(object, head, settings.max_patch_height) : settings.max_patch_height;
    double band_max_patch_height = material_max_patch_height;
    if (settings.banded && repair != nullptr)
        for (const RepairHints::OrderBand &band : repair->order_bands)
            band_max_patch_height = std::min(band_max_patch_height > 0. ? band_max_patch_height : Inf, band.height);
    if (const double rotary = settings.banded ? rotary_band_reach(object, head, h_c) : 0.; rotary > 0.)
        band_max_patch_height = band_max_patch_height > 0. ? std::min(band_max_patch_height, rotary) : rotary;
    double band_pitch = 0., band_tolerance = 0.;
    const bool rotary_shells = settings.banded && object.print()->config().nonplanar_kinematics.value != RotaryLayout::None;
    if (settings.banded)
    {
        double thinnest = h_c;
        for (const AdaptiveStack &st : adaptive_stacks)
            thinnest = std::min(thinnest, st.min_height());
        band_schedule(head, thinnest, band_max_patch_height, band_pitch, band_tolerance);
    }

    const bool          strength = strength_roads_enabled(region.config(), object.config());
    const StrengthField strength_field = strength ? StrengthField(object) : StrengthField();

    const SeamPaint       paint = seam_paint(object);
    std::vector<Point>    seams(result.patches.size());
    std::vector<Polyline> seam_lines(result.patches.size());
    std::vector<ExPolygons> fill_areas(result.patches.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, result.patches.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t idx = range.begin(); idx < range.end(); ++idx) {
            ConformalPatch &patch = result.patches[idx];
            const double    host_z = settings.banded ? 0. : layers[patch.host_layer]->print_z;
            const double overlap = 0.01 * region.config().top_bottom_infill_wall_overlap.value * line_width;
            const ExPolygons area = intersection_ex(offset_ex(shell_areas[idx], float(scale_(overlap))), patch.footprint);
            std::vector<ShellFill> fills = plan_shell_fills(patch, region.config(), line_width);
            const bool helix = spiral_top_enabled(region.config(), object.config()) && helix_on_surface(patch.surface, area);
            if (!fills.empty())
                fills.back().helix = fills.back().spiral && helix;
            std::vector<ShellFill> banded_fills = fills;
            ArachneCache          arachne_cache;
            BandedFill             banded;
            banded.arachne      = &arachne_cache;
            banded.voids        = voids;
            banded.strip_height = band_pitch;
            banded.strip_offset = 0.5 * band_tolerance;
            if (rotary_shells)
                banded.flat_angle = settings.solid_angle;
            banded.adaptive     = settings.adaptive ? &adaptive_stacks[idx] : nullptr;
            banded.steep        = steep_tops ? &*steep_tops : nullptr;
            const NonplanarTopPattern pattern    = top_pattern(patch, region.config());
            const bool                user_rings = settings.banded && pattern == NonplanarTopPattern::Concentric;
            if (settings.banded && (pattern == NonplanarTopPattern::Contour || user_rings)) {
                double angle = 0.;
                for (size_t k = 0; k < banded_fills.size(); ++k)
                    banded_fills[k] = ShellFill();
                if (!user_rings && contour_line_angle(patch.surface, angle, 0.9)) {
                    for (ShellFill &f : banded_fills) {
                        f.angle = angle;
                        f.outline = false;
                    }
                } else if (is_round(patch.footprint) && outline_is_contour(patch, h_c) &&
                           !(steep_tops && !steep_area(patch.surface, steep_tops->cap()).empty()))
                    for (size_t k = 0; k < banded_fills.size(); ++k) {
                        banded_fills[k].spiral = true;
                        banded_fills[k].outward = (banded_fills.size() - 1 - k) % 2 == 1;
                        banded_fills[k].helix   = k + 1 == banded_fills.size() && helix;
                    }
                else {
                    banded.strips = true;
                    for (size_t k = 0; k < banded_fills.size(); ++k)
                        banded_fills[k].angle = fills[k].angle;
                }
                if (!banded_fills.front().spiral)
                    banded.lengthwise = lengthwise_strips(area, line_width);
            } else if (settings.banded) {
                for (ShellFill &f : banded_fills)
                    f.outline = false;
                banded.lengthwise = lengthwise_strips(area, line_width);
            }
            if (settings.shells > settings.top_shells) {
                banded.layers  = &stack_layers[idx];
                banded.sparse  = patch.sparse;
                patch.bridged_base  = patch.sparse > 0 && settings.sparse_density > 0. &&
                                     past_bridging_limit(region.flow(object, frInfill, h_c).spacing() / settings.sparse_density, h_c);
                banded.bridge_first = patch.bridged_base;
                banded.hidden  = settings.shells - settings.top_shells;
                banded.density = settings.sparse_density;
                const ShellFill &outermost = settings.banded ? banded_fills.back() : fills.back();
                banded.angle = outermost.spiral || banded.strips ? settings.solid_angle : outermost.angle;
                if (strength) {
                    banded.walls = true;
                    double     principal = 0.;
                    const ExPolygons hidden_area = hidden_layer_area(stack_layers[idx], settings.top_shells, area);
                    const bool directed = !hidden_area.empty() && strength_field.direction(patch.surface, hidden_area, principal);
                    if (!outermost.spiral && !banded.strips && banded.sparse > 0) {
                        double sparse = outermost.angle;
                        if (directed && std::abs(std::remainder(principal - sparse, PI)) > PI / 12. &&
                            strength_field.aligned_share(patch.surface, hidden_area, principal) >
                                strength_field.aligned_share(patch.surface, hidden_area, sparse) + 0.05 &&
                            level_enough(patch.surface, hidden_area, principal, sparse))
                            banded.sparse_angle = sparse = principal;
                        banded.angle = sparse + ((banded.hidden - banded.sparse - 1) % 2 == 0 ? 0. : 0.5 * PI);
                    } else if (directed)
                        banded.angle = principal;
                }
            }
            const HeightField contour = banded.strips ? contour_samples(patch.surface) : HeightField();
            banded.contour = banded.strips ? &contour : nullptr;
            const ExPolygons fill_area = banded.lengthwise.empty() ? area : diff_ex(area, banded.lengthwise);
            const SeamPosition          seam_position = object.config().seam_position.value;
            patch.shell_paths.reserve(patch.shells);
            const Point                 seam          = shell_seam(area, patch.surface, seam_position, unsigned(patch.id), &paint);
            seams[idx]                                = seam;
            seam_lines[idx]                           = seam_line(patch.surface, seam, area);
            fill_areas[idx]                           = area;
            if (settings.banded) {
                std::vector<ExtrusionEntityCollection> shells(size_t(patch.shells));
                auto make_shells = [&](int up_to) {
                    tbb::parallel_for(tbb::blocked_range<int>(0, up_to, 1), [&](const tbb::blocked_range<int> &range) {
                        for (int k = range.begin() + 1; k <= range.end(); ++k) {
                            throw_if_canceled(object);
                            shells[size_t(k - 1)] = make_shell(object, patch, planar_tops[idx], fill_area, k, banded_fills[size_t(k - 1)], seam,
                                                               nullptr, host_z, pitch, seam_lines[idx], &banded);
                        }
                    });
                };
                make_shells(patch.shells);
                if (patch.sparse > 0 && banded.layers != nullptr && !settings.adaptive) {
                    const double reach = region.flow(object, frInfill, h_c).spacing() / settings.sparse_density;
                    const int    solid = patch.shells - patch.sparse;
                    HeightField &count = stack_layers[idx], &floor = planar_tops[idx];
                    size_t       lowered = 0;
                    for (const size_t i : unsupported_nodes(shells[size_t(patch.sparse - 1)], count, solid, reach)) {
                        const float s = patch.surface.data()[i];
                        if (std::isnan(s) || !std::isfinite(floor.data()[i]))
                            continue;
                        if (const int layer = planar_transition_layer(layer_z, s - (solid - 1) * h_c, h_c, h_min, h_max, PatchKind::Top); layer >= 0) {
                            count.data()[i] = float(solid);
                            floor.data()[i] = float(layer_z[size_t(layer)]);
                            ++lowered;
                        }
                    }
                    if (lowered > 0)
                        make_shells(patch.sparse + 1);
                }
                for (ExtrusionEntityCollection &shell : shells)
                    patch.shell_paths.emplace_back(std::move(shell));
            } else {
                std::optional<Point3> from;
                for (int k = 1; k <= patch.shells; ++k) {
                    throw_if_canceled(object);
                    const ShellFill &fill  = fills[size_t(k - 1)];
                    const Point      start = fill.spiral || !from ? seam : from->to_point();
                    patch.shell_paths.emplace_back(make_shell(object, patch, planar_tops[idx], fill_area, k, fill, start, from ? &*from : nullptr,
                                                              host_z, pitch, seam_lines[idx]));
                    const ExtrusionEntitiesPtr &entities = patch.shell_paths.back().entities;
                    from.reset();
                    if (!entities.empty())
                        from = static_cast<const ExtrusionPathConformal *>(entities.back())->polyline.points.back();
                }
            }
            if (settings.banded && ironing_enabled(patch, region.config()))
                patch.ironing = make_conformal_ironing(object, patch, area, seam, seam_lines[idx], host_z, true, spans_wall);
        }
    });

    if (const TopColours colours = top_colours(object, candidate); colours.areas.size() > 1) {
        std::function<int(const Vec3d &)> colour;
        if (settings.banded && settings.shells > settings.top_shells) {
            const double hidden = (settings.top_shells - 0.5) * settings.shell_height;
            colour = [planar = planar_colour(object, colours, candidate, 0.2), &S1, &colours, hidden](const Vec3d &p) {
                const float s = S1.sample(p.head<2>());
                return !std::isnan(s) && s - p.z() > hidden ? planar(p) : colours.at(p.head<2>());
            };
        }
        split_by_colour(result, planar_tops, stack_layers, adaptive_stacks, shell_areas, fill_areas, seams, seam_lines, colours, colour);
        const int own = region.config().top_surface_filament_id.value;
        for (ConformalPatch &patch : result.patches) {
            if (object.printing_region(size_t(patch.region_id)).config().top_surface_filament_id.value == own)
                patch.region_id = int(region_id);
            patch.id += patch_offset;
            for (ExtrusionEntityCollection &shell : patch.shell_paths)
                for (ExtrusionEntity *entity : shell.entities)
                    static_cast<ExtrusionPathConformal *>(entity)->patch_id = patch.id;
        }
        for (PatchDiagnostics &diag : result.diagnostics)
            if (diag.patch_id >= 0)
                diag.patch_id += patch_offset;
        BOOST_LOG_TRIVIAL(info) << "Non-planar: " << colours.areas.size() << " top colours in " << object.model_object()->name;
    }

    std::vector<ConformalPatch> component_patches;
    std::vector<size_t>         patch_component;
    if (settings.banded) {
        BandSeams band_seams{ fill_areas, seams, seam_lines, line_width, MaxGlide, spans_wall,
                              settings.shells - settings.top_shells, &stack_layers };
        if (settings.adaptive)
            band_seams.drop = [&adaptive_stacks](size_t c, int k, const Vec2d &q, double &drop) {
                double bead;
                bool   deepest;
                return adaptive_stacks[c].layer(k + 1, q, drop, bead, deepest);
            };
        component_patches = split_into_bands(result, S, layer_z, band_pitch, band_tolerance, &band_seams,
                                             layer_changes_filament(object), patch_offset, &patch_component, event_cuts(object));
    }
    std::vector<ConformalPatch> &units = settings.banded ? component_patches : result.patches;
    if (settings.banded)
        tbb::parallel_for(tbb::blocked_range<size_t>(0, result.patches.size()), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++i)
                taper_top(result.patches[i]);
        });

    if (region.config().nonplanar_flow_mod.value) {
        const double  nozzle = region.flow(object, frSolidInfill, h_c).nozzle_diameter();
        FlowModParams params;
        params.land_radius = head.tip_flat_radius;
        params.max_height  = FlowModMaxThickness * nozzle;
        params.smoothing   = FlowModSmoothing * nozzle;
        params.min_normal  = FlowModMinThickness * bead_scale(object.print()->config());
        params.resample    = pitch;
        std::vector<std::vector<size_t>> groups(units.size());
        for (size_t i = 0; i < result.patches.size(); ++i)
            groups[settings.banded ? patch_component[i] : i].emplace_back(i);
        std::vector<FlowModStats> stats(groups.size());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, groups.size()), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t c = range.begin(); c < range.end(); ++c) {
                if (groups[c].empty())
                    continue;
                throw_if_canceled(object);
                const HeightField &surface = result.patches[groups[c].front()].surface;
                std::optional<ShellDeficit> below;
                const int n_shells = settings.adaptive ? result.patches[groups[c].front()].shells : units[c].shells;
                for (int k = 0; k < n_shells; ++k) {
                    const bool   sparse = k < result.patches[groups[c].front()].sparse;
                    ShellDeficit laid(surface);
                    for (size_t idx : groups[c])
                        for (ExtrusionEntity *ee : result.patches[idx].shell_paths[size_t(k)].entities)
                            modulate_flow(*static_cast<ExtrusionPathConformal *>(ee), surface, below && !sparse ? &*below : nullptr,
                                          sparse ? nullptr : &laid, params, stats[c]);
                    if (sparse)
                        below.reset();
                    else
                        below.emplace(std::move(laid));
                }
            }
        });
        for (const FlowModStats &s : stats)
            result.flow_mod.merge(s);
        const FlowModStats &fm = result.flow_mod;
        BOOST_LOG_TRIVIAL(info) << "Non-planar flow: " << fm.points << " points, normal gap " << fm.min_gap << ".." << fm.max_gap
                                << " mm, laid " << fm.min_laid << ".." << fm.max_laid << " mm; clamped " << fm.max_clamped << " thick, "
                                << fm.min_clamped << " thin; " << fm.dragged << " dragged, " << fm.contact << " in tip contact";
    }

    for (const ConformalPatch &patch : result.patches)
        layers[patch.host_layer]->conformal_patches_after.emplace_back(patch.id);
    std::vector<char> printed(units.size(), !settings.banded);
    for (size_t c : patch_component)
        printed[c] = 1;
    for (size_t idx = 0; idx < units.size(); ++idx)
        if (!printed[idx]) {
            PatchDiagnostics diag;
            diag.fallback_area   = area(units[idx].footprint) * SCALING_FACTOR * SCALING_FACTOR;
            diag.fallback_reason = "no_pieces";
            result.diagnostics.emplace_back(std::move(diag));
        }
    for (size_t idx = 0; idx < units.size(); ++idx)
        if (printed[idx])
            for (size_t i = 0; i < layers.size(); ++i)
                if (layer_z[i] > units[idx].z_min - EPSILON && i <= units[idx].host_layer)
                    layers[i]->conformal_covered = true;
    std::vector<ExPolygons> clip_areas = shell_areas;
    if (settings.banded)
        for (ExPolygons &area : clip_areas)
            area = offset_ex(area, -float(scale_(0.5 * line_width)));
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            throw_if_canceled(object);
            ExPolygons excl;
            for (size_t idx = 0; idx < units.size(); ++idx) {
                const ConformalPatch &patch = units[idx];
                if (!printed[idx] || i > patch.host_layer || layer_z[i] <= patch.z_min - EPSILON)
                    continue;
                const HeightField &B = planar_tops[idx];
                Mask               above(B.data().size(), 0);
                bool               any = false;
                const double bottom = layer_z[i] - layers[i]->height;
                for (size_t n = 0; n < above.size(); ++n)
                    if (!std::isnan(B.data()[n]) && layer_z[i] > B.data()[n] + EPSILON &&
                        !(bottom >= patch.surface.data()[n] - EPSILON)) {
                        above[n] = 1;
                        any      = true;
                    }
                if (any)
                    append(excl, intersection_ex(mask_to_expolygons(above, B), clip_areas[idx]));
            }
            if (!excl.empty())
                result.planar_exclusion[i] = union_ex(excl);
        }
    });
    const bool field_seams = field_layers(object);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            const ExPolygons &excl = result.planar_exclusion[i];
            if (excl.empty())
                continue;
            throw_if_canceled(object);
            const BoundingBox excl_bbox = get_extents(excl);
            ExPolygons        under;
            if (field_seams && i > 0)
                under = offset_ex(result.planar_exclusion[i - 1], float(scale_(0.25 * line_width)));
            const ExPolygons *seam = under.empty() ? nullptr : &under;
            for (LayerRegion *layerm : layers[i]->regions()) {
                clip_extrusions(layerm->fills, excl, excl_bbox, seam);
                clip_extrusions(layerm->perimeters, excl, excl_bbox, seam);
            }
        }
    });
    {
        std::vector<ExPolygons> solid_cells(layers.size()), bridge_cells(layers.size());
        for (size_t idx = 0; idx < units.size(); ++idx)
            if (printed[idx])
                closing_cells(object, units[idx], planar_tops[idx], stack_layers[idx], shell_areas[idx],
                              units[idx].shells - units[idx].sparse, top_thickness, layer_z, solid_cells, bridge_cells);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++i)
                if (!solid_cells[i].empty())
                    close_layer(object, i, union_ex(solid_cells[i]), union_ex(bridge_cells[i]));
        });
    }
    for (size_t idx = 0; idx < units.size(); ++idx)
        if (printed[idx]) {
            BaseInput input{ units[idx].surface, planar_tops[idx], stack_layers[idx], shell_areas[idx], {}, units[idx].sparse,
                             units[idx].shells - units[idx].sparse, units[idx].bridged_base, units[idx].shell_height };
            for (size_t i = 0; i < result.patches.size(); ++i)
                if ((settings.banded ? patch_component[i] : i) == idx)
                    input.patches.push_back(result.patches[i].id);
            result.base_inputs.emplace_back(std::move(input));
        }

    BOOST_LOG_TRIVIAL(info) << "Non-planar: " << result.patches.size() << " curved top patch(es) in " << object.model_object()->name
                            << ", " << grid.nx() << "x" << grid.ny() << " nodes, "
                            << std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count() << " s";
    return result;
}

namespace {

template<class Keep> void trim_path(const ExtrusionPath &path, const Keep &keep, const BoundingBox &zone, ExtrusionEntityCollection &out)
{
    const Points3 &pts = path.polyline.points;
    BoundingBox    extent;
    for (const Point3 &q : pts)
        extent.merge(Point(q.x(), q.y()));
    if (!extent.overlap(zone)) {
        out.append(path);
        return;
    }
    Points3        run;
    bool           cut = false;
    auto flush = [&]() {
        cut = true;
        if (run.size() >= 2) {
            ExtrusionPath piece(path);
            piece.polyline.points = std::move(run);
            out.append(std::move(piece));
        }
        run.clear();
    };
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i > 0) {
            const Vec3d a = pts[i - 1].cast<double>(), b = pts[i].cast<double>();
            BoundingBox seg;
            seg.merge(Point(pts[i - 1].x(), pts[i - 1].y()));
            seg.merge(Point(pts[i].x(), pts[i].y()));
            const int n = seg.overlap(zone) ? std::max(1, int(std::ceil(unscale<double>((b - a).head<2>().norm()) / 0.25))) : 1;
            for (int j = 1; j < n; ++j) {
                const Vec3d  m = a + (b - a) * (double(j) / n);
                const Point3 q(coord_t(m.x()), coord_t(m.y()), coord_t(m.z()));
                if (keep(q))
                    run.push_back(q);
                else
                    flush();
            }
        }
        if (keep(pts[i]))
            run.push_back(pts[i]);
        else
            flush();
    }
    if (!cut) {
        out.append(path);
        return;
    }
    flush();
}

template<class Keep> void trim_entity(const ExtrusionEntity &e, const Keep &keep, const BoundingBox &zone, ExtrusionEntityCollection &out)
{
    if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(&e)) {
        ExtrusionEntityCollection sub;
        sub.no_sort = c->no_sort;
        for (const ExtrusionEntity *child : c->entities)
            trim_entity(*child, keep, zone, sub);
        if (!sub.entities.empty())
            out.append(std::move(sub));
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&e)) {
        bool all = true;
        for (const ExtrusionPath &p : loop->paths)
            for (const Point3 &q : p.polyline.points)
                all = all && keep(q);
        if (all)
            out.append(e);
        else
            for (const ExtrusionPath &p : loop->paths)
                trim_path(p, keep, zone, out);
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&e)) {
        for (const ExtrusionPath &p : multi->paths)
            trim_path(p, keep, zone, out);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&e)) {
        trim_path(*path, keep, zone, out);
    } else
        out.append(e);
}

}
void trim_walls_above_shells(PrintObject &object, const ConformalResult &result)
{
    static const bool on = [] {
        const char *v = research_switch("NP_CT1_TRIM_WALLS");
        return v != nullptr && *v != 0 && *v != '0';
    }();
    if (!on)
        return;
    for (const ConformalPatch &patch : result.patches) {
        if (patch.kind != PatchKind::Top || patch.footprint.empty() || patch.base.empty())
            continue;
        const ExPolygons  zone  = offset_ex(patch.footprint, float(scale_(0.5)));
        const BoundingBox bbox  = get_extents(zone);
        const double      slack = 0.5 * patch.shell_height;
        float             low   = std::numeric_limits<float>::infinity();
        for (const float b : patch.base.data())
            if (!std::isnan(b))
                low = std::min(low, b);
        if (!std::isfinite(low))
            continue;
        double layer_z = 0.;
        auto   keep    = [&](const Point3 &q) {
            const Point p(q.x(), q.y());
            if (!bbox.contains(p))
                return true;
            const double z = q.z() != 0 ? unscale<double>(q.z()) : layer_z;
            if (z <= double(low) + slack)
                return true;
            const float b = patch.base.sample(unscale(p));
            if (std::isnan(b) || z <= double(b) + slack)
                return true;
            for (const ExPolygon &e : zone)
                if (e.contains(p))
                    return false;
            return true;
        };
        for (Layer *layer : object.layers()) {
            if (layer->print_z <= double(low) + slack)
                continue;
            layer_z = layer->print_z;
            for (LayerRegion *layerm : layer->regions())
                for (ExtrusionEntityCollection *roads : { &layerm->perimeters, &layerm->fills }) {
                    if (roads->entities.empty())
                        continue;
                    ExtrusionEntityCollection kept;
                    kept.no_sort = roads->no_sort;
                    for (const ExtrusionEntity *e : roads->entities)
                        trim_entity(*e, keep, bbox, kept);
                    roads->swap(kept);
                }
        }
    }
}

ConformalResult build_conformal_shells(PrintObject &object)
{
    ConformalResult result;
    result.planar_exclusion.resize(object.layers().size());
    result.shells_refused.resize(object.layers().size());
    result.clearance       = clearance_zone(object);
    result.clearance_ready = true;
    for (Layer *layer : object.layers()) {
        layer->conformal_patches_after.clear();
        layer->conformal_covered = false;
    }
    std::vector<std::vector<size_t>> groups;
    for (size_t region_id = 0; region_id < object.num_printing_regions(); ++region_id) {
        const PrintRegionConfig &cfg  = object.printing_region(region_id).config();
        auto                     same = std::find_if(groups.begin(), groups.end(), [&](const std::vector<size_t> &group) {
            const t_config_option_keys diff = object.printing_region(group.front()).config().diff(cfg);
            return std::all_of(diff.begin(), diff.end(), [](const std::string &key) { return boost::starts_with(key, "fuzzy_skin"); });
        });
        if (same == groups.end())
            groups.push_back({ region_id });
        else
            same->push_back(region_id);
    }
    ExPolygons all_tops;
    if (groups.size() > 1) {
        std::vector<size_t> all(object.num_printing_regions());
        for (size_t r = 0; r < all.size(); ++r)
            all[r] = r;
        all_tops = candidate_top_area(object, all);
        if (!result.clearance.empty())
            all_tops = diff_ex(all_tops, result.clearance);
    }
    for (const std::vector<size_t> &group : groups) {
        ConformalResult region = build_region_shells(object, group, int(result.patches.size()), result.mesh,
                                                     groups.size() > 1 ? &all_tops : nullptr, result.clearance);
        result.mesh            = region.mesh;
        result.flow_mod.merge(region.flow_mod);
        append(result.base_inputs, std::move(region.base_inputs));
        for (ConformalPatch &patch : region.patches)
            result.patches.emplace_back(std::move(patch));
        for (PatchDiagnostics &diag : region.diagnostics)
            result.diagnostics.emplace_back(std::move(diag));
        for (size_t i = 0; i < result.planar_exclusion.size(); ++i)
            result.planar_exclusion[i] = union_ex(result.planar_exclusion[i], region.planar_exclusion[i]);
        for (size_t i = 0; i < result.shells_refused.size() && i < region.shells_refused.size(); ++i)
            if (!region.shells_refused[i].empty())
                result.shells_refused[i] = union_ex(result.shells_refused[i], region.shells_refused[i]);
    }
    return result;
}

}
