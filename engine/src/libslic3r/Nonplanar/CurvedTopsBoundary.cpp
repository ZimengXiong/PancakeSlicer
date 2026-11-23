#include "CurvedTopsBoundary.hpp"
#include "CurvedTopsSurface.hpp"
#include "CurvedTopsOrder.hpp"
#include "CurvedTopsSchedule.hpp"
#include "BeadModel.hpp"
#include "Repair.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/libslic3r.h"
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iterator>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <utility>
#include <vector>

namespace Slic3r::Nonplanar {
namespace {
void visit(const ExtrusionEntity &entity, const std::function<void(const ExtrusionPath &)> &fn)
{
    if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(&entity))
        for (const auto *e : c->entities) visit(*e, fn);
    else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
        for (const auto &p : loop->paths) fn(p);
    else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity))
        for (const auto &p : multi->paths) fn(p);
    else if (const auto *p = dynamic_cast<const ExtrusionPath *>(&entity)) fn(*p);
}
bool top_of(const ExtrusionPath &p, int id)
{
    return p.nonplanar_source && p.distance_to_top >= 0.f && p.distance_to_top < .001f && p.source_surface_id == id;
}
void erase_top(ExtrusionEntityCollection &collection, int id)
{
    auto &es = collection.entities;
    es.erase(std::remove_if(es.begin(), es.end(), [id](ExtrusionEntity *e) {
        if (auto *c = dynamic_cast<ExtrusionEntityCollection *>(e)) {
            erase_top(*c, id);
            if (!c->entities.empty()) return false;
        } else {
            bool all = true;
            visit(*e, [&](const ExtrusionPath &p) { all &= top_of(p, id); });
            if (!all) return false;
        }
        delete e;
        return true;
    }), es.end());
}
struct Cap {
    Polygons area;
    LayerRegion *home = nullptr;
    ExtrusionPaths paths;
};
bool same_projected_point(const Point3 &a, const Point3 &b)
{
    const Vec2d delta = a.to_point().cast<double>() - b.to_point().cast<double>();
    const double xy_tolerance = scale_(.000002);
    return delta.squaredNorm() <= xy_tolerance * xy_tolerance && std::abs(a.z() - b.z()) <= scale_(.003);
}
Polyline sampled(const Polygon &polygon)
{
    Points points;
    for (size_t i = 0; i < polygon.points.size(); ++i) {
        const Point &a = polygon.points[i], &b = polygon.points[(i + 1) % polygon.points.size()];
        const int n = std::max(1, int(std::ceil(unscale<double>((b - a).cast<double>().norm()) / .2)));
        for (int k = 0; k < n; ++k)
            points.emplace_back(Vec2d(a.cast<double>() + double(k) / n * (b - a).cast<double>()));
    }
    if (!points.empty()) points.push_back(points.front());
    return Polyline(std::move(points));
}
Vec2d tangent(const Point3 &p, const Polygons &rings, double &distance_mm)
{
    const Vec2d xy = unscale(p).head<2>();
    double best = std::numeric_limits<double>::infinity();
    Vec2d out = Vec2d::Zero();
    for (const auto &ring : rings)
        for (size_t i = 0; i < ring.points.size(); ++i) {
            const Vec2d a = unscale(ring.points[i]), d = unscale(ring.points[(i + 1) % ring.points.size()]) - a;
            if (d.squaredNorm() == 0.) continue;
            const double t = std::clamp((xy - a).dot(d) / d.squaredNorm(), 0., 1.);
            const double distance = (xy - a - t * d).squaredNorm();
            if (distance < best) { best = distance; out = d.normalized(); }
        }
    distance_mm = std::sqrt(best);
    return out;
}
void blend_rim(ExtrusionPath &path, const PrintObject &object)
{
    const auto original = path.polyline.points;
    if (original.size() < 4) return;
    std::vector<double> arc(original.size(), 0.);
    for (size_t i = 1; i < original.size(); ++i)
        arc[i] = arc[i - 1] + (unscale(original[i]).head<2>() - unscale(original[i - 1]).head<2>()).norm();
    if (arc.back() < .001) return;
    const auto &layers = object.layers();
    const auto level_at = [&](double z) {
        auto layer = std::upper_bound(layers.begin(), layers.end(), z,
            [](double height, const Layer *l) { return height < l->slice_z; });
        return layer == layers.begin() ? layers.front()->print_z : (*std::prev(layer))->print_z;
    };
    std::vector<std::pair<double, double>> events{{0., level_at(unscale<double>(original.front().z()))}};
    for (size_t i = 1; i < original.size(); ++i) {
        const double a = unscale<double>(original[i - 1].z()), b = unscale<double>(original[i].z());
        events.emplace_back(arc[i - 1], level_at(a + (b > a ? 1e-7 : b < a ? -1e-7 : 0.)));
        if (a == b) continue;
        auto layer = std::lower_bound(layers.begin(), layers.end(), std::min(a, b),
            [](const Layer *l, double z) { return l->slice_z < z; });
        for (; layer != layers.end() && (*layer)->slice_z <= std::max(a, b); ++layer) {
            const double z = (*layer)->slice_z;
            const double t = (z - a) / (b - a);
            if (t <= 0. || t >= 1.) continue;
            events.emplace_back(arc[i - 1] + t * (arc[i] - arc[i - 1]), level_at(z + (b > a ? 1e-7 : -1e-7)));
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<double> integral(events.size(), 0.);
    for (size_t i = 1; i < events.size(); ++i)
        integral[i] = integral[i - 1] + (events[i].first - events[i - 1].first) * events[i - 1].second;
    const double total = integral.back() + (arc.back() - events.back().first) * events.back().second;
    const auto integrated = [&](double s) {
        const double turns = std::floor(s / arc.back());
        s -= turns * arc.back();
        auto it = std::upper_bound(events.begin(), events.end(), s,
            [](double value, const auto &event) { return value < event.first; });
        const size_t i = size_t(std::prev(it) - events.begin());
        return turns * total + integral[i] + (s - events[i].first) * events[i].second;
    };
    for (size_t i = 0; i + 1 < original.size(); ++i) {
        const double native_z = .5 * (integrated(arc[i] + 1.) - integrated(arc[i] - 1.));
        path.polyline.points[i].z() = scale_(.75 * unscale<double>(original[i].z()) + .25 * native_z);
    }
    path.polyline.points.back() = path.polyline.points.front();
}

ExtrusionPaths flow_pieces(const ExtrusionPath &p, const Points3 &points, const std::vector<double> &nominal, bool simplify = true)
{
    ExtrusionPaths out;
    Polyline3 simplified(points);
    if (simplify) simplified.simplify(scale_(.001));
    std::set<size_t> keep{0, points.size() - 1};
    size_t cursor = 0;
    for (const Point3 &q : simplified.points) {
        while (cursor + 1 < points.size() && points[cursor] != q) ++cursor;
        keep.insert(cursor);
    }
    if (!simplify)
        for (size_t i = 0; i < points.size(); ++i) keep.insert(i);
    double minimum = unscale<double>(points.front().z()) - nominal.front(), maximum = minimum;
    for (size_t i = 1; i < points.size(); ++i) {
        const double delta = unscale<double>(points[i].z()) - nominal[i];
        if (std::max(maximum, delta) - std::min(minimum, delta) > .25 * p.height) {
            keep.insert(i - 1);
            minimum = unscale<double>(points[i - 1].z()) - nominal[i - 1]; maximum = minimum;
        }
        minimum = std::min(minimum, delta); maximum = std::max(maximum, delta);
    }
    size_t previous = 0;
    for (size_t i : keep) {
        if (i == 0) continue;
        double volume = 0.;
        for (size_t k = previous + 1; k <= i; ++k) {
            const double length = (unscale(points[k]) - unscale(points[k - 1])).head<2>().norm();
            volume += length * bead_area(p.width, p.height + .5 * (unscale<double>(points[k - 1].z()) + unscale<double>(points[k].z()) - nominal[k - 1] - nominal[k]));
        }
        const double length = (unscale(points[i]) - unscale(points[previous])).head<2>().norm();
        if (length < 1e-9) {
            if (!out.empty()) out.back().polyline.points.back() = points[i];
            previous = i;
            continue;
        }
        ExtrusionPath piece(Polyline3(Points3{points[previous], points[i]}), p);
        const double area = volume / length;
        piece.height = float(bead_height_for_area(p.width, area));
        piece.mm3_per_mm *= area / bead_area(p.width, p.height);
        out.push_back(std::move(piece));
        previous = i;
    }
    if (!out.empty() && points.front() == points.back())
        out.front().polyline.points.front() = out.back().polyline.points.back();
    return out;
}

bool lift_rim(ExtrusionPath &path, const NonplanarClearanceModel &head)
{
    auto &points = path.polyline.points;
    const size_t n = points.size() - 1;
    if (n < 3 || points.front() != points.back()) return false;
    struct Segment { Vec2d center, radius; double arc; };
    std::vector<Segment> segments;
    std::vector<double> z;
    double length = 0., low = std::numeric_limits<double>::infinity(), high = -low;
    for (size_t i = 0; i < n; ++i) {
        const Vec3d a = unscale(points[i]), b = unscale(points[i + 1]);
        const double step = (b - a).norm();
        segments.push_back({.5 * (a + b).head<2>(), .5 * (b - a).head<2>().cwiseAbs(), length + .5 * step});
        length += step;
        z.push_back(a.z());
        low = std::min(low, a.z()); high = std::max(high, a.z());
    }
    const auto original = z;
    const double lift_limit = .5 * path.height, span = high - low + lift_limit;
    const double grow = .5 * path.width + head.bead_margin + head.safety_mm;
    double reach = grow + .4;
    for (const auto &r : head.nozzle)
        if (r.bottom - head.safety_mm <= span) reach = std::max(reach, r.radius + grow + .4);
    for (const auto &b : head.boxes)
        if (b.bottom - head.safety_mm <= span)
            reach = std::max({reach, std::abs(b.x_min) + grow + .4, std::abs(b.x_max) + grow + .4,
                             std::abs(b.y_min) + grow + .4, std::abs(b.y_max) + grow + .4});
    const auto key = [reach](const Vec2d &p) { return std::make_pair(int(std::floor(p.x() / reach)), int(std::floor(p.y() / reach))); };
    std::map<std::pair<int, int>, std::vector<size_t>> grid;
    for (size_t i = 0; i < n; ++i) grid[key(segments[i].center)].push_back(i);
    std::vector<std::vector<std::pair<size_t, double>>> after(n);
    const double contact_run = std::max(1., 2. * double(path.width));
    for (size_t i = 0; i < n; ++i) {
        const size_t next = (i + 1) % n;
        const double grade = .2 * (unscale(points[i + 1]) - unscale(points[i])).head<2>().norm();
        after[i].emplace_back(next, grade); after[next].emplace_back(i, grade);
        const auto [cx, cy] = key(segments[i].center);
        for (int x = cx - 1; x <= cx + 1; ++x)
            for (int y = cy - 1; y <= cy + 1; ++y) {
                const auto found = grid.find({x, y});
                if (found == grid.end()) continue;
                for (size_t j : found->second) {
                    const double arc = segments[i].arc - segments[j].arc;
                    if (j >= i || arc <= contact_run || length - arc <= contact_run) continue;
                    const Vec2d d = segments[j].center - segments[i].center;
                    const Vec2d r = segments[j].radius + segments[i].radius;
                    const double dx = std::max(0., std::abs(d.x()) - r.x()), dy = std::max(0., std::abs(d.y()) - r.y());
                    double floor = std::numeric_limits<double>::infinity();
                    for (const auto &ring : head.nozzle)
                        if (dx * dx + dy * dy < (ring.radius + grow) * (ring.radius + grow)) floor = std::min(floor, ring.bottom);
                    for (const auto &box : head.boxes)
                        if (d.x() + r.x() > box.x_min - grow && d.x() - r.x() < box.x_max + grow &&
                            d.y() + r.y() > box.y_min - grow && d.y() - r.y() < box.y_max + grow) floor = std::min(floor, box.bottom);
                    const double allowance = floor - head.safety_mm - .003;
                    if (allowance > span) continue;
                    for (size_t material : {j, (j + 1) % n})
                        for (size_t tool : {i, next}) after[material].emplace_back(tool, allowance);
                }
            }
    }
    std::priority_queue<std::pair<double, size_t>> pending;
    for (size_t i = 0; i < n; ++i) pending.emplace(z[i], i);
    while (!pending.empty()) {
        const auto [height, i] = pending.top(); pending.pop();
        if (height < z[i]) continue;
        for (const auto &[j, allowance] : after[i])
            if (height - allowance > z[j] + 1e-10) {
                z[j] = height - allowance;
                if (z[j] - original[j] > lift_limit) return false;
                pending.emplace(z[j], j);
            }
    }
    for (size_t i = 0; i < n; ++i) points[i].z() = scale_(z[i]);
    points.back() = points.front();
    return true;
}

bool ramp_inset(ExtrusionEntityCollection &inset, const ExtrusionEntityCollection &rim,
                const NonplanarClearanceModel &head, double max_surface_error)
{
    struct Bead { Vec3d a, b; double width; };
    std::vector<Bead> beads;
    double low = std::numeric_limits<double>::infinity(), high = -low, half_width = 0.;
    visit(rim, [&](const ExtrusionPath &p) {
        for (size_t i = 1; i < p.polyline.points.size(); ++i) {
            const Vec3d a = unscale(p.polyline.points[i - 1]), b = unscale(p.polyline.points[i]);
            beads.push_back({a, b, p.width});
            low = std::min({low, a.z(), b.z()}); high = std::max({high, a.z(), b.z()});
            half_width = std::max(half_width, .5 * p.width);
        }
    });
    double reach = half_width + head.bead_margin + head.safety_mm + .4;
    for (const auto &r : head.nozzle)
        if (r.bottom - head.safety_mm <= high - low + .2) reach = std::max(reach, r.radius + half_width + .4);
    for (const auto &b : head.boxes)
        if (b.bottom - head.safety_mm <= high - low + .2)
            reach = std::max({reach, std::abs(b.x_min) + half_width + .4, std::abs(b.x_max) + half_width + .4,
                             std::abs(b.y_min) + half_width + .4, std::abs(b.y_max) + half_width + .4});
    const auto key = [reach](const Vec2d &p) { return std::make_pair(int(std::floor(p.x() / reach)), int(std::floor(p.y() / reach))); };
    std::map<std::pair<int, int>, std::vector<size_t>> grid;
    for (size_t i = 0; i < beads.size(); ++i) grid[key(.5 * (beads[i].a + beads[i].b).head<2>())].push_back(i);
    const auto near_rim = [&](const Vec2d &point) {
        const auto [cx, cy] = key(point);
        for (int x = cx - 1; x <= cx + 1; ++x)
            for (int y = cy - 1; y <= cy + 1; ++y)
                if (grid.find({x, y}) != grid.end()) return true;
        return false;
    };
    const auto blend_height = [&](const Vec3d &point, double width) {
        double nearest = std::numeric_limits<double>::infinity(), height = point.z();
        const auto [cx, cy] = key(point.head<2>());
        for (int x = cx - 1; x <= cx + 1; ++x)
            for (int y = cy - 1; y <= cy + 1; ++y) {
                const auto found = grid.find({x, y});
                if (found == grid.end()) continue;
                for (size_t i : found->second) {
                    const Bead &bead = beads[i];
                    const Vec2d d = (bead.b - bead.a).head<2>();
                    if (d.squaredNorm() == 0.) continue;
                    const double t = std::clamp((point - bead.a).head<2>().dot(d) / d.squaredNorm(), 0., 1.);
                    const double distance = ((point - bead.a).head<2>() - t * d).norm();
                    if (distance < nearest) {
                        nearest = distance;
                        const double z = bead.a.z() + t * (bead.b.z() - bead.a.z());
                        const double weight = std::clamp((distance - .5 * width) / width, 0., 1.);
                        height = z + weight * (point.z() - z);
                    }
                }
            }
        return height;
    };
    const auto required = [&](const Vec3d &point) {
        double z = point.z();
        const auto [cx, cy] = key(point.head<2>());
        for (int x = cx - 1; x <= cx + 1; ++x)
            for (int y = cy - 1; y <= cy + 1; ++y) {
                const auto found = grid.find({x, y});
                if (found == grid.end()) continue;
                for (size_t i : found->second) {
                    const Bead &bead = beads[i];
                    const Vec2d d = .5 * (bead.a + bead.b).head<2>() - point.head<2>();
                    const Vec2d r = .5 * (bead.b - bead.a).head<2>().cwiseAbs();
                    const double dx = std::max(0., std::abs(d.x()) - r.x()), dy = std::max(0., std::abs(d.y()) - r.y());
                    const double grow = .5 * bead.width + head.bead_margin + head.safety_mm;
                    double floor = std::numeric_limits<double>::infinity();
                    for (const auto &ring : head.nozzle)
                        if (dx * dx + dy * dy < (ring.radius + grow) * (ring.radius + grow)) floor = std::min(floor, ring.bottom);
                    for (const auto &box : head.boxes)
                        if (d.x() + r.x() > box.x_min - grow && d.x() - r.x() < box.x_max + grow &&
                            d.y() + r.y() > box.y_min - grow && d.y() - r.y() < box.y_max + grow) floor = std::min(floor, box.bottom);
                    z = std::max(z, std::max(bead.a.z(), bead.b.z()) - floor + head.safety_mm + .003);
                }
            }
        return z;
    };
    ExtrusionEntityCollection out;
    for (const ExtrusionEntity *entity : inset.entities) {
        const auto &p = *static_cast<const ExtrusionPath *>(entity);
        Points3 points;
        std::vector<double> nominal;
        for (size_t i = 1; i < p.polyline.points.size(); ++i) {
            const Vec3d a = unscale(p.polyline.points[i - 1]), b = unscale(p.polyline.points[i]);
            const int count = std::max(1, int(std::ceil((b - a).head<2>().norm() / .2)));
            for (int k = i == 1 ? 0 : 1; k <= count; ++k) {
                Vec3d q = a + (b - a) * (double(k) / count);
                const double original_z = unscale<double>(Point3::new_scale(q).z());
                q.z() = blend_height(q, p.width);
                const double height = required(q);
                if (k > 0 && k < count && std::abs(height - original_z) < 1e-6 && !near_rim(q.head<2>())) continue;
                points.emplace_back(Point3::new_scale(q));
                nominal.push_back(original_z);
            }
        }
        if (points.size() < 2) { out.entities.push_back(entity->clone()); continue; }
        std::vector<double> z;
        std::priority_queue<std::pair<double, size_t>> pending;
        for (size_t i = 0; i < points.size(); ++i) { z.push_back(required(unscale(points[i]))); pending.emplace(z.back(), i); }
        const bool closed = points.front() == points.back();
        while (!pending.empty()) {
            const auto [height, i] = pending.top(); pending.pop();
            if (height < z[i]) continue;
            for (size_t j : {i == 0 ? i : i - 1, std::min(i + 1, points.size() - 1),
                             closed && i == 0 ? points.size() - 1 : closed && i + 1 == points.size() ? size_t(0) : i}) {
                const double allowance = .2 * (unscale(points[j]) - unscale(points[i])).head<2>().norm();
                if (height - allowance > z[j] + 1e-10) { z[j] = height - allowance; pending.emplace(z[j], j); }
            }
        }
        bool changed = false;
        for (size_t i = 0; i < points.size(); ++i) {
            if (z[i] - nominal[i] > p.height || nominal[i] - z[i] > .5 * p.height) {
                BOOST_LOG_TRIVIAL(info) << "Curved top inset ramp " << p.source_surface_id << " path " << p.source_path_id
                    << " exceeds bead-height range: delta " << z[i] - nominal[i] << " height " << p.height;
                return false;
            }
            if (std::abs(z[i] - nominal[i]) > max_surface_error)
                return false;
            changed |= std::abs(z[i] - nominal[i]) > 1e-6;
            points[i].z() = scale_(z[i]);
        }
        if (!changed) { out.entities.push_back(entity->clone()); continue; }
        for (auto &piece : flow_pieces(p, points, nominal))
            out.entities.push_back(new ExtrusionPath(std::move(piece)));
    }
    inset.clear();
    inset.entities.swap(out.entities);
    return true;
}
}

ExPolygons smooth_curved_top_footprint(const ExPolygons &area, double max_inset_mm)
{
    ExPolygons out;
    for (const ExPolygon &part : area) {
        const double perimeter = part.contour.length();
        const double pitch = perimeter / std::max(1., std::ceil(unscale<double>(perimeter) / .1));
        Points points = part.contour.equally_spaced_points(pitch);
        if (points.size() > 1 && (points.front() - points.back()).cast<double>().norm() < scale_(.01)) points.pop_back();
        if (points.size() < 8) { out.push_back(part); continue; }
        const Points original = points;
        const int n = int(points.size());
        const int radius = std::clamp(int(std::lround(scale_(max_inset_mm) / pitch)), 1, n / 4);
        for (int pass = 0; pass < 2; ++pass) {
            Points filtered;
            for (int i = 0; i < n; ++i) {
                Vec2d sum = Vec2d::Zero();
                for (int k = -radius; k <= radius; ++k) sum += points[size_t((i + k + n) % n)].cast<double>();
                filtered.emplace_back(Vec2d(sum / double(2 * radius + 1)));
            }
            points.swap(filtered);
        }
        const Vec2d center = part.contour.centroid().cast<double>();
        double displacement = 0., reach = 0.;
        for (size_t i = 0; i < points.size(); ++i) {
            displacement = std::max(displacement, (points[i] - original[i]).cast<double>().norm());
            reach = std::max(reach, (points[i].cast<double>() - center).norm());
        }
        const double remaining = scale_(max_inset_mm) - displacement;
        if (!(remaining > 0. && reach > 0.)) {
            BOOST_LOG_TRIVIAL(info) << "Curved top footprint: unchanged, filter displacement " << unscale<double>(displacement)
                << " mm exceeds trim " << max_inset_mm << " mm; points " << n;
            out.push_back(part); continue;
        }
        const auto fitted = [&](double factor) {
            Points p;
            for (const Point &point : points) p.emplace_back(Vec2d(center + factor * (point.cast<double>() - center)));
            return ExPolygon(Polygon(std::move(p)));
        };
        const auto inside = [&](double factor) {
            return diff_ex(ExPolygons{fitted(factor)}, ExPolygons{ExPolygon(part.contour)}, ApplySafetyOffset::No).empty();
        };
        double low = std::max(.75, 1. - remaining / reach), high = 1.;
        if (!inside(low)) {
            BOOST_LOG_TRIVIAL(info) << "Curved top footprint: unchanged, no contained fit at factor " << low
                << "; filter displacement " << unscale<double>(displacement) << " mm, reach " << unscale<double>(reach) << " mm";
            out.push_back(part); continue;
        }
        if (inside(high)) low = high;
        else for (int step = 0; step < 20; ++step) {
            const double mid = .5 * (low + high);
            if (inside(mid)) low = mid; else high = mid;
        }
        BOOST_LOG_TRIVIAL(info) << "Curved top footprint: smoothed, factor " << low << "; filter displacement "
            << unscale<double>(displacement) << " mm, reach " << unscale<double>(reach) << " mm";
        append(out, intersection_ex(ExPolygons{fitted(low)}, ExPolygons{part}, ApplySafetyOffset::No));
    }
    return out;
}

void finish_curved_top_boundaries(const PrintObject &object, const std::vector<Layer *> &shells)
{
    if (const auto *hints = object.nonplanar_repair(); hints != nullptr &&
        hints->curved_finish != RepairHints::CurvedFinish::Blended)
        return;
    std::map<std::pair<int, size_t>, Cap> caps;
    for (Layer *layer : shells)
        for (size_t r = 0; r < layer->region_count(); ++r) {
            LayerRegion *region = layer->get_region(int(r));
            for (const Surface &s : region->slices.surfaces)
                if (s.is_nonplanar() && s.distance_to_top >= 0.f && s.distance_to_top < .001f) {
                    Cap &cap = caps[{s.source_surface_id, r}];
                    polygons_append(cap.area, to_polygons(s.expolygon));
                    if (!cap.home || cap.home->layer()->id() < layer->id()) cap.home = region;
                }
            const auto collect = [&](const ExtrusionPath &p) {
                if (p.nonplanar_source && !p.continuous_top && p.distance_to_top >= 0.f && p.distance_to_top < .001f)
                    caps[{p.source_surface_id, r}].paths.push_back(p);
            };
            visit(region->perimeters, collect);
            visit(region->fills, collect);
        }
    for (auto &[key, cap] : caps) {
        const auto id = key.first;
        const auto r = key.second;
        if (!cap.home || cap.paths.empty()) continue;
        double low = std::numeric_limits<double>::infinity(), high = -low;
        for (const auto &p : cap.paths)
            for (const auto &q : p.polyline.points) {
                low = std::min(low, unscale<double>(q.z()));
                high = std::max(high, unscale<double>(q.z()));
            }
        if (high - low < .03) continue;
        bool mixed = false;
        std::function<void(const ExtrusionEntity &)> check = [&](const ExtrusionEntity &entity) {
            if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
                for (const auto *e : c->entities) check(*e);
            } else {
                bool any = false, all = true;
                visit(entity, [&](const ExtrusionPath &p) { const bool target = top_of(p, id); any |= target; all &= target; });
                mixed |= any && !all;
            }
        };
        for (const Layer *layer : shells) {
            check(layer->get_region(int(r))->perimeters);
            check(layer->get_region(int(r))->fills);
        }
        if (mixed) continue;
        const auto &surfaces = object.nonplanar_surfaces();
        const auto surface = std::find_if(surfaces.begin(), surfaces.end(), [id](const auto &s) { return s.stable_id == id; });
        if (surface == surfaces.end() || surface->stats.max.z - surface->stats.min.z < .03) continue;
        const Flow flow = cap.home->flow(frExternalPerimeter);
        const double inset = .5 * flow.width();
        const ExPolygons area = union_ex(cap.area);
        const Vec2d cap_size = unscale(get_extents(area).size());
        const int transition_rings = std::max(2, int(.04 * cap_size.minCoeff() / flow.spacing()));
        const ExPolygons inside = offset_ex(area, -scale_(inset + transition_rings * flow.spacing() + .3 * flow.width()));
        if (inside.empty()) continue;
        const Polygons inner = to_polygons(offset_ex(area, -scale_(inset + transition_rings * flow.spacing())));
        struct BoundaryLayer : Layer {
            explicit BoundaryLayer(Layer &source) : Layer(source.id(), source.object(), source.height, source.print_z, source.slice_z) {}
        } temporary(*cap.home->layer());
        LayerRegion *region = temporary.add_region(&cap.home->region());
        region->append_nonplanar_surface(*surface);
        auto fills = std::make_unique<ExtrusionEntityCollection>();
        for (const auto &source : cap.paths)
            for (const auto &piece : inside)
                for (const auto &xy_line : intersection_pl(Polylines{source.polyline.to_polyline()}, piece)) {
                    Polyline3 line(xy_line);
                    if (line.points.size() < 2) continue;
                    const bool closed = line.points.front() == line.points.back();
                    for (bool end : {false, true}) {
                        if (closed) break;
                        const size_t a = end ? line.points.size() - 1 : 0, b = end ? a - 1 : 1;
                        double distance_mm;
                        Vec2d t = tangent(line.points[a], inner, distance_mm);
                        if (distance_mm > flow.width()) continue;
                        const Vec2d direction = unscale(Vec3crd(line.points[end ? a : b] - line.points[end ? b : a])).head<2>();
                        if (t.dot(direction) < 0.) t = -t;
                        const Vec2d xy = unscale(line.points[a]).head<2>() + (end ? .1 : -.1) * t;
                        const Point3 p(coord_t(scale_(xy.x())), coord_t(scale_(xy.y())), coord_t(0));
                        if (end) line.points.push_back(p); else line.points.insert(line.points.begin(), p);
                    }
                    fills->entities.push_back(new ExtrusionPath(std::move(line), source));
                }
        for (int ring = 1; ring <= transition_rings; ++ring)
            for (const Polygon &polygon : to_polygons(offset_ex(area, -scale_(inset + ring * flow.spacing())))) {
                ExtrusionPath p(erTopSolidInfill, flow.mm3_per_mm(), flow.width(), flow.height());
                p.polyline = Polyline3(sampled(polygon));
                p.nonplanar_source = true; p.distance_to_top = 0.f; p.source_surface_id = id;
                p.source_layer_id = cap.home->layer()->id();
                p.source_path_id = next_nonplanar_path_id();
                fills->entities.push_back(new ExtrusionPath(std::move(p)));
            }
        if (fills->entities.empty()) continue;
        region->fills.entities.push_back(fills.release());
        {
            auto loop = std::make_unique<ExtrusionEntityCollection>();
            loop->no_sort = true;
            for (const Polygon &polygon : to_polygons(offset_ex(area, -scale_(inset)))) {
                ExtrusionPath p(erExternalPerimeter, flow.mm3_per_mm(), flow.width(), flow.height());
                p.polyline = Polyline3(sampled(polygon));
                p.nonplanar_source = true; p.distance_to_top = 0.f; p.source_surface_id = id;
                p.source_layer_id = cap.home->layer()->id();
                p.source_path_id = next_nonplanar_path_id();
                p.set_reverse();
                loop->entities.push_back(new ExtrusionPath(std::move(p)));
            }
            region->fills.entities.push_back(loop.release());
        }
        region->project_nonplanar_surfaces();
        bool safe = true;
        const double max_surface_error = cap.home->region().config().nonplanar_max_surface_error.value;
        const auto head = NonplanarClearanceModel::for_object(object, flow.height());
        for (size_t i = 1; i < region->fills.entities.size(); ++i) {
            auto *collection = static_cast<ExtrusionEntityCollection *>(region->fills.entities[i]);
            ExtrusionPaths loops;
            for (const auto *e : collection->entities)
                visit(*e, [&](const ExtrusionPath &p) {
                    if (!loops.empty() && loops.back().source_path_id == p.source_path_id &&
                        same_projected_point(loops.back().polyline.points.back(), p.polyline.points.front()) &&
                        loops.back().polyline.points.front() != loops.back().polyline.points.back())
                        loops.back().polyline.points.insert(loops.back().polyline.points.end(), std::next(p.polyline.points.begin()), p.polyline.points.end());
                    else loops.push_back(p);
                });
            for (const auto *e : collection->entities) delete e;
            collection->entities.clear();
            safe &= !loops.empty();
            for (auto &p : loops) {
                if (same_projected_point(p.polyline.points.front(), p.polyline.points.back()))
                    p.polyline.points.back() = p.polyline.points.front();
                const auto mesh_points = p.polyline.points;
                blend_rim(p, object);
                const bool closed = p.polyline.points.size() > 3 && p.polyline.points.front() == p.polyline.points.back();
                const bool clear = closed && lift_rim(p, head) && continuous_curved_top_clear(p, head);
                if (!clear)
                    BOOST_LOG_TRIVIAL(info) << "Curved top boundary " << id << " ring " << i
                        << ": points " << p.polyline.points.size() << " closed " << closed << " clear " << clear
                        << " gap " << unscale(Vec3crd(p.polyline.points.back() - p.polyline.points.front())).norm();
                safe &= clear;
                if (!clear)
                    continue;
                if (!std::equal(mesh_points.begin(), mesh_points.end(), p.polyline.points.begin(), p.polyline.points.end(),
                    [max_surface_error](const Point3 &mesh, const Point3 &finished) {
                        return std::abs(unscale<double>(finished.z() - mesh.z())) <= max_surface_error;
                    })) {
                    BOOST_LOG_TRIVIAL(info) << "Curved top boundary " << id << ": rim exceeds surface error; retaining original roads";
                    safe = false;
                    continue;
                }
                std::vector<double> nominal;
                for (const Point3 &q : mesh_points) nominal.push_back(unscale<double>(q.z()));
                auto connected = std::make_unique<ExtrusionMultiPath>();
                connected->set_reverse();
                connected->paths = flow_pieces(p, p.polyline.points, nominal);
                const auto clear_chain = [&] {
                    ExtrusionPath joined(p);
                    joined.polyline.points.clear();
                    for (const auto &piece : connected->paths) {
                        if (joined.polyline.points.empty()) joined.polyline.points.push_back(piece.polyline.points.front());
                        joined.polyline.points.push_back(piece.polyline.points.back());
                    }
                    if (joined.polyline.points.size() <= 3 || joined.polyline.points.front() != joined.polyline.points.back())
                        BOOST_LOG_TRIVIAL(info) << "Curved top boundary chain " << id << ": closure lost in flow pieces";
                    return joined.polyline.points.size() > 3 && joined.polyline.points.front() == joined.polyline.points.back() &&
                           continuous_curved_top_clear(joined, head);
                };
                if (!clear_chain()) connected->paths = flow_pieces(p, p.polyline.points, nominal, false);
                safe &= clear_chain();
                collection->entities.push_back(connected.release());
            }
        }
        const bool ring_clear = safe;
        if (safe)
            safe = ramp_inset(*static_cast<ExtrusionEntityCollection *>(region->fills.entities.front()),
                              *static_cast<ExtrusionEntityCollection *>(region->fills.entities[1]), head, max_surface_error);
        if (!safe) {
            BOOST_LOG_TRIVIAL(info) << "Curved top boundary " << id
                << (ring_clear ? ": inset transition exceeds its bead-height range or surface error; retaining original roads" :
                                 ": closed road fails its same-run access check; retaining original roads");
            continue;
        }
        for (Layer *layer : shells) {
            erase_top(layer->get_region(int(r))->perimeters, id);
            erase_top(layer->get_region(int(r))->fills, id);
        }
        cap.home->fills.append(std::move(region->fills.entities));
        BOOST_LOG_TRIVIAL(info) << "Curved top boundary " << id << ": one closed finish road, closed transition band and tangent core";
    }
}
}
