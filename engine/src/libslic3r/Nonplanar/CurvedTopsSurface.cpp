// Curved tops port of fich-l / OrcaSlicer-Nonplanar v0.1 (AGPL-3.0).
#include "CurvedTopsSurface.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "Repair.hpp"
#include "Validate.hpp"
#include "libslic3r/GCode/ClearanceRepair.hpp"
#include "HeadEnvelope.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include <tbb/parallel_for.h>
#include <algorithm>
#include <map>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <functional>
#include <limits>
#include <list>
#include <mutex>
#include <set>
#include <utility>

#include <cstdio>
#include <boost/functional/hash.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

NonplanarSurface::NonplanarSurface(std::map<int, NonplanarFacet> &_mesh)
{
    this->mesh = _mesh;
    this->calculate_stats();
}

bool
NonplanarSurface::operator==(const NonplanarSurface& other) const {
   return (stats.min.x == other.stats.min.x &&
           stats.min.y == other.stats.min.y &&
           stats.min.z == other.stats.min.z &&
           stats.max.x == other.stats.max.x &&
           stats.max.y == other.stats.max.y &&
           stats.max.z == other.stats.max.z);
}

void
NonplanarSurface::calculate_stats()
{
    this->stats.min.x = 10000000;
    this->stats.min.y = 10000000;
    this->stats.min.z = 10000000;
    this->stats.max.x = -10000000;
    this->stats.max.y = -10000000;
    this->stats.max.z = -10000000;
    for(auto& facet : this->mesh) {
        this->stats.min.x = std::min(this->stats.min.x, facet.second.stats.min.x);
        this->stats.min.y = std::min(this->stats.min.y, facet.second.stats.min.y);
        this->stats.min.z = std::min(this->stats.min.z, facet.second.stats.min.z);
        this->stats.max.x = std::max(this->stats.max.x, facet.second.stats.max.x);
        this->stats.max.y = std::max(this->stats.max.y, facet.second.stats.max.y);
        this->stats.max.z = std::max(this->stats.max.z, facet.second.stats.max.z);
    }
}

void
NonplanarSurface::translate(float x, float y, float z)
{
    for(auto& facet : this->mesh) {
        facet.second.translate(x, y, z);
    }

    this->stats.min.x += x;
    this->stats.min.y += y;
    this->stats.min.z += z;
    this->stats.max.x += x;
    this->stats.max.y += y;
    this->stats.max.z += z;
}

void
NonplanarSurface::scale(float factor)
{
    float versor[3];
    versor[0] = factor;
    versor[1] = factor;
    versor[2] = factor;
    this->scale(versor);
}

void
NonplanarSurface::scale(float versor[3])
{
    for(auto& facet : this->mesh) {
        facet.second.scale(versor);
    }

    this->stats.min.x *= versor[0];
    this->stats.min.y *= versor[1];
    this->stats.min.z *= versor[2];
    this->stats.max.x *= versor[0];
    this->stats.max.y *= versor[1];
    this->stats.max.z *= versor[2];
}

void
NonplanarSurface::rotate_z(float angle) {
  double radian_angle = (angle / 180.0) * 3.1415927;
  double c = cos(radian_angle);
  double s = sin(radian_angle);

  for(auto& facet : this->mesh) {
    for(int j = 0; j < 3; j++) {
        double xold = facet.second.vertex[j].x;
        double yold = facet.second.vertex[j].y;
        facet.second.vertex[j].x = c * xold - s * yold;
        facet.second.vertex[j].y = s * xold + c * yold;
    }
    facet.second.calculate_stats();
  }

  this->calculate_stats();
}

void
NonplanarSurface::debug_output()
{
    std::cout << "Facets(" << this->mesh.size() << "): (min:X:" << this->stats.min.x << " Y:" << this->stats.min.y << " Z:" << this->stats.min.z <<
                           " max:X:" << this->stats.max.x << " Y:" << this->stats.max.y << " Z:" << this->stats.max.z << ")" <<
                           "Height " << this->stats.max.z - this->stats.min.z << std::endl;
    for(auto& facet : this->mesh) {
        std::cout << "triangle: (" << facet.first << ")(" << facet.second.marked << ") ";
        std::cout << " (" << (180*std::acos(facet.second.normal.z))/3.14159265 << "°)";

        std::cout << " | V0:";
        std::cout << " X:"<< facet.second.vertex[0].x;
        std::cout << " Y:"<< facet.second.vertex[0].y;
        std::cout << " Z:"<< facet.second.vertex[0].z;

        std::cout << " | V1:";
        std::cout << " X:"<< facet.second.vertex[1].x;
        std::cout << " Y:"<< facet.second.vertex[1].y;
        std::cout << " Z:"<< facet.second.vertex[1].z;

        std::cout << " | V2:";
        std::cout << " X:"<< facet.second.vertex[2].x;
        std::cout << " Y:"<< facet.second.vertex[2].y;
        std::cout << " Z:"<< facet.second.vertex[2].z;

        std::cout << " | Normal:";
        std::cout << " X:"<< facet.second.normal.x;
        std::cout << " Y:"<< facet.second.normal.y;
        std::cout << " Z:"<< facet.second.normal.z;

        std::cout << " | Neighbors:";
        std::cout << " 0:"<< facet.second.neighbor[0];
        std::cout << " 1:"<< facet.second.neighbor[1];
        std::cout << " 2:"<< facet.second.neighbor[2];
        std::cout << std::endl;
    }
}

NonplanarSurfaces
NonplanarSurface::group_surfaces()
{
    NonplanarSurfaces out;
    std::set<int> seen;
    for (const auto &[seed, facet] : mesh) {
        if (!seen.insert(seed).second) continue;
        NonplanarSurface surface;
        std::vector<int> pending{seed};
        while (!pending.empty()) {
            const int id = pending.back();
            pending.pop_back();
            const NonplanarFacet &current = mesh.at(id);
            surface.mesh.emplace(id, current);
            for (int neighbour : current.neighbor)
                if (mesh.count(neighbour) && seen.insert(neighbour).second) pending.push_back(neighbour);
        }
        surface.calculate_stats();
        out.emplace_back(std::move(surface));
    }
    return out;
}

void
NonplanarSurface::mark_neighbor_surfaces(int id)
{
    if(this->mesh.find(id) == this->mesh.end() || this->mesh[id].marked == true) return;
    this->mesh[id].marked = true;
    for(int j = 0; j < 3; j++) {
        this->mark_neighbor_surfaces(this->mesh[id].neighbor[j]);
    }
}

bool
NonplanarSurface::check_max_printing_height(float height)
{
    if ((this->stats.max.z - this->stats.min.z) > height ) {
        BOOST_LOG_TRIVIAL(trace) << "Surface removed: printheight too heigh (" << (this->stats.max.z - this->stats.min.z) << " mm)";
        return true;
    } else {
        return false;
    }
}

bool
NonplanarSurface::check_surface_area(float min_area)
{
    float area = 0.0f;
    for(auto& facet : this->mesh) {
        area += facet.second.calculate_surface_area();
    }
    if (area < min_area) {
        BOOST_LOG_TRIVIAL(trace) << "Surface removed: area too small (" << area << " mm²)";
        return true;
    } else {
        return false;
    }
}

void
NonplanarSurface::check_printable_surfaces(float max_angle)
{
}

ExPolygons
NonplanarSurface::horizontal_projection() const
{
    Polygons pp;
    pp.reserve(this->mesh.size());
    Points key;
    key.reserve(3 * this->mesh.size());
    size_t hash = this->mesh.size();
    for(auto& facet : this->mesh) {
        Polygon p;
        p.points.resize(3);
        p.points[0] = Point(scale_(facet.second.vertex[0].x), scale_(facet.second.vertex[0].y));
        p.points[1] = Point(scale_(facet.second.vertex[1].x), scale_(facet.second.vertex[1].y));
        p.points[2] = Point(scale_(facet.second.vertex[2].x), scale_(facet.second.vertex[2].y));
        p.make_counter_clockwise();
        for (const Point &pt : p.points) {
            key.push_back(pt);
            boost::hash_combine(hash, pt.x());
            boost::hash_combine(hash, pt.y());
        }
        pp.push_back(p);
    }

    struct Entry { size_t hash; Points key; ExPolygons projection; };
    static std::mutex       mutex;
    static std::list<Entry> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = cache.begin(); it != cache.end(); ++ it)
            if (it->hash == hash && it->key == key) {
                cache.splice(cache.begin(), cache, it);
                return cache.front().projection;
            }
    }
    constexpr size_t chunk = 2048;
    std::vector<Polygons> merged((pp.size() + chunk - 1) / chunk);
    tbb::parallel_for(size_t(0), merged.size(), [&](size_t c) {
        const Polygons part(pp.begin() + c * chunk, pp.begin() + std::min(pp.size(), (c + 1) * chunk));
        merged[c] = union_(offset(part, scale_(0.01)));
    });
    Polygons outlines;
    for (Polygons &m : merged)
        append(outlines, std::move(m));
    ExPolygons projection = union_ex(outlines);
    std::lock_guard<std::mutex> lock(mutex);
    cache.push_front({ hash, std::move(key), projection });
    if (cache.size() > 64)
        cache.pop_back();
    return projection;
}

std::string nonplanar_location(double z, size_t layer_id)
{
    char buf[80];
    std::snprintf(buf, sizeof(buf), " at height %.2f mm (layer %zu)", z, layer_id + 1);
    return buf;
}

double nonplanar_suggested_angle(double angle)
{
    return angle > 10. + EPSILON ? 10. : std::max(1., std::floor(angle) - 2.);
}

void throw_nonplanar_too_steep(double z, size_t layer_id, double angle, size_t object_id, const std::string &detail)
{
    BOOST_LOG_TRIVIAL(error) << "Nonplanar surface too steep" << nonplanar_location(z, layer_id) << ": " << detail;
    char buf[400];
    std::snprintf(buf, sizeof(buf), "Nonplanar slicing failed%s: a nonplanar line would have to climb more than %g° there "
        "(a sharp edge or step in the surface). Lower \"Maximum nonplanar angle\" to %g° or less and slice again.",
        nonplanar_location(z, layer_id).c_str(), std::max(12., angle), nonplanar_suggested_angle(angle));
    throw Slic3r::SlicingError(buf, object_id);
}

void throw_nonplanar_internal(const std::string &where, const std::string &detail, size_t object_id)
{
    BOOST_LOG_TRIVIAL(error) << "Nonplanar internal check failed" << where << ": " << detail;
    throw Slic3r::SlicingError("Nonplanar slicing stopped" + where + ": an internal check failed (" + detail + "). "
        "This is a bug, not a setting problem: please report it with the 3MF file. "
        "To slice now, turn off nonplanar layers for this object.", object_id);
}

bool curved_cone_allowance(NonplanarMode mode)
{
    return curved_tops_mode(mode) || mode == NonplanarMode::SmoothMorph || mode == NonplanarMode::ConformalLayers || mode == NonplanarMode::Slant;
}

double curved_cone_tolerance(NonplanarMode mode)
{
    if (!curved_cone_allowance(mode))
        return 0.05;
    double tolerance = 0.13;
    if (const char *value = research_switch("NP_CONTACT_CONE_TOL"); value != nullptr)
        tolerance = std::atof(value);
    return std::clamp(tolerance, 0., 0.13);
}

namespace {
std::mutex                                                      rejections_mutex;
std::map<const PrintObject *, CurvedSurfaceRejections>          rejections_of;
}

void note_curved_surface_rejections(const PrintObject &object, const CurvedSurfaceRejections &rejections)
{
    std::lock_guard<std::mutex> lock(rejections_mutex);
    rejections_of[&object] = rejections;
}

static double upward_area_between(const PrintObject &object, double low, double high)
{
    if (!(high > low))
        return 0.;
    const double cos_low = std::cos(low * M_PI / 180.), cos_high = std::cos(high * M_PI / 180.);
    double area = 0.;
    for (const ModelVolume *volume : object.model_object()->volumes) {
        if (!volume->is_model_part())
            continue;
        const Transform3d m = object.trafo_centered() * volume->get_matrix();
        const indexed_triangle_set &its = volume->mesh().its;
        for (const stl_triangle_vertex_indices &f : its.indices) {
            const Vec3d a = m * its.vertices[f[0]].cast<double>(), b = m * its.vertices[f[1]].cast<double>(), c = m * its.vertices[f[2]].cast<double>();
            const Vec3d n = (b - a).cross(c - a);
            const double length = n.norm();
            if (length <= 0. || n.z() <= 0.)
                continue;
            const double nz = n.z() / length;
            if (nz < cos_low && nz >= cos_high)
                area += 0.5 * n.z();
        }
    }
    return area;
}

int curved_top_layers(const PrintObject &object)
{
    if (const int set = object.config().nonplanar_curved_top_layers.value; set > 0)
        return set;
    switch (object.config().nonplanar_mode.value) {
    case NonplanarMode::NonplanarTop: return 3;
    default: break;
    }
    if (object.num_printing_regions() == 0)
        throw Slic3r::SlicingError(object.model_object()->name + ": curved top layers asked before the object has regions");
    const PrintRegionConfig &rc = object.printing_region(0).config();
    const double h = std::max(EPSILON, object.config().layer_height.value);
    return std::max(1, std::max(rc.top_shell_layers.value, int(std::ceil(rc.top_shell_thickness.value / h - 1e-6))));
}

CurvedTopsStats curved_tops_stats(const PrintObject &object)
{
    CurvedTopsStats out;
    out.candidates = object.curved_surface_candidates();
    out.upward_area_mm2 = object.curved_upward_area();
    std::map<int, std::pair<double, double>> top_heights;
    std::map<int, double> road_lengths;
    std::function<void(const ExtrusionEntity &)> visit = [&](const ExtrusionEntity &entity) {
        if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
            if (!path->nonplanar_source) return;
            for (const Point3 &point : path->polyline.points) {
                const double z = unscale<double>(point.z());
                out.max_z = std::max(out.max_z, z);
                if (path->distance_to_top >= 0.f && path->distance_to_top < 0.001f) {
                    auto [it, inserted] = top_heights.emplace(path->source_surface_id, std::make_pair(z, z));
                    it->second.first = std::min(it->second.first, z);
                    it->second.second = std::max(it->second.second, z);
                }
            }
            for (size_t i = 1; i < path->polyline.points.size(); ++i) {
                road_lengths[path->source_surface_id] +=
                    (path->polyline.points[i].cast<double>() - path->polyline.points[i - 1].cast<double>()).norm() * SCALING_FACTOR;

            }
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
            for (const auto &path : loop->paths) visit(path);
        } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
            for (const auto &path : multi->paths) visit(path);
        } else if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
            for (const auto *child : collection->entities) visit(*child);
        }
    };
    for (const Layer *layer : object.layers())
        for (const LayerRegion *region : layer->regions()) {
            visit(region->perimeters);
            visit(region->fills);
        }
    std::set<int> used;
    for (const auto &[id, heights] : top_heights)
        if (heights.second - heights.first >= 0.03 - EPSILON) {
            used.insert(id);
            out.extrusion_mm += road_lengths[id];
        }
    out.kept = used.size();
    if (const auto *hints = object.nonplanar_repair())
        for (int id : used) {
            const auto found = hints->curved_top_areas.find(id);
            if (found != hints->curved_top_areas.end()) out.footprint_mm2 += found->second;
        }
    out.area_fraction = out.upward_area_mm2 > 0. ? std::clamp(out.footprint_mm2 / out.upward_area_mm2, 0., 1.) : 0.;
    if (out.kept == 0)
        out.planar_fallback_reason = out.candidates == 0 ? "no nonflat top surface meets the angle, height, area and access limits" :
                                                        "selected surfaces were rejected by head clearance or left no top roads with at least 0.03 mm of height variation";
    const NonplanarMode mode  = object.config().nonplanar_mode.value;
    const double        limit = Nonplanar::curved_max_angle_limit(mode);
    out.max_angle_deg         = object.config().nonplanar_curved_max_angle.value;
    constexpr double    seen  = 1.;
    CurvedSurfaceRejections rejections;
    {
        std::lock_guard<std::mutex> lock(rejections_mutex);
        if (const auto it = rejections_of.find(&object); it != rejections_of.end()) rejections = it->second;
    }
    if (rejections.head_unknown)
        out.planar_limit = "head_unknown";
    else if (out.kept == 0) {
        if (out.candidates > 0 || rejections.collision > 0)
            out.planar_limit = "access";
        else if (upward_area_between(object, out.max_angle_deg, limit) >= seen)
            out.planar_limit = "angle";
        else if (rejections.height > 0)
            out.planar_limit = "height";
        else if (rejections.area > 0)
            out.planar_limit = "area";
    }
    if (out.planar_limit == "access")
        out.access_by = Nonplanar::clearance_override_active(object.print()->config()) ? "nozzle" : "head";
    return out;
}

std::vector<double> auto_top_angles(const Points &points, std::optional<double> level)
{
    const Polygon hull = Geometry::convex_hull(points);
    if (hull.points.size() < 2)
        return {};
    const auto wrap  = [](double a) { a = std::fmod(a, M_PI); return a < 0. ? a + M_PI : a; };
    const auto width = [&hull](double a) {
        const Vec2d n(-std::sin(a), std::cos(a));
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (const Point &p : hull.points)
            lo = std::min(lo, n.dot(unscale(p))), hi = std::max(hi, n.dot(unscale(p)));
        return hi - lo;
    };
    std::vector<std::pair<double, double>> edges;
    for (size_t i = 0; i < hull.points.size(); ++i) {
        const Vec2d d = unscale(hull.points[(i + 1) % hull.points.size()]) - unscale(hull.points[i]);
        if (d.squaredNorm() > 0.)
            edges.emplace_back(0., wrap(std::atan2(d.y(), d.x())));
    }
    for (auto &[w, a] : edges)
        w = width(a);
    std::stable_sort(edges.begin(), edges.end(), [](const auto &l, const auto &r) { return l.first < r.first; });
    const auto apart = [](double a, double b) { const double d = std::abs(a - b); return std::min(d, M_PI - d) >= M_PI / 18.; };
    std::vector<double> out;
    if (level && !edges.empty() && width(wrap(*level)) <= 1.1 * edges.front().first)
        out.push_back(wrap(*level));
    for (const auto &edge : edges) {
        const double a = edge.second;
        if (int(out.size()) < AutoTopCandidates && std::all_of(out.begin(), out.end(), [&](double b) { return apart(a, b); }))
            out.push_back(a);
    }
    return out;
}

Points surface_outline_points(const NonplanarSurface &surface)
{
    Points points;
    points.reserve(3 * surface.mesh.size());
    for (const auto &[id, facet] : surface.mesh)
        for (const auto &v : facet.vertex)
            points.emplace_back(Point::new_scale(double(v.x), double(v.y)));
    return points;
}

std::optional<double> mesh_auto_top_direction(const indexed_triangle_set &mesh)
{
    Points points;
    for (const stl_triangle_vertex_indices &f : mesh.indices) {
        const Vec3f a = mesh.vertices[f[0]], b = mesh.vertices[f[1]], c = mesh.vertices[f[2]];
        const Vec3f n = (b - a).cross(c - a);
        if (n.z() > 0.5f * n.norm())
            for (const Vec3f &v : { a, b, c })
                points.emplace_back(Point::new_scale(double(v.x()), double(v.y())));
    }
    const std::vector<double> angles = auto_top_angles(points);
    if (angles.empty())
        return std::nullopt;
    return Geometry::rad2deg(angles.front());
}

std::vector<double> auto_top_angles(const NonplanarSurface &surface, std::optional<double> level, double spacing)
{
    if (spacing <= 0.)
        throw Slic3r::InvalidArgument("auto_top_angles: line spacing must be positive");
    const double x0 = surface.stats.min.x, y0 = surface.stats.min.y;
    const double pitch = std::max(0.25, std::max(surface.stats.max.x - x0, surface.stats.max.y - y0) / 2000.);
    const int    nx = std::max(1, int(std::ceil((surface.stats.max.x - x0) / pitch)) + 1);
    const int    ny = std::max(1, int(std::ceil((surface.stats.max.y - y0) / pitch)) + 1);
    std::vector<float> z(size_t(nx) * size_t(ny), std::numeric_limits<float>::quiet_NaN());
    for (const auto &[id, f] : surface.mesh) {
        const Vec3d a(f.vertex[0].x, f.vertex[0].y, f.vertex[0].z), b(f.vertex[1].x, f.vertex[1].y, f.vertex[1].z),
                    c(f.vertex[2].x, f.vertex[2].y, f.vertex[2].z);
        const double det = (b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y());
        if (std::abs(det) < 1e-12)
            continue;
        const int i0 = std::max(0, int(std::floor((std::min({ a.x(), b.x(), c.x() }) - x0) / pitch)));
        const int i1 = std::min(nx - 1, int(std::ceil((std::max({ a.x(), b.x(), c.x() }) - x0) / pitch)));
        const int j0 = std::max(0, int(std::floor((std::min({ a.y(), b.y(), c.y() }) - y0) / pitch)));
        const int j1 = std::min(ny - 1, int(std::ceil((std::max({ a.y(), b.y(), c.y() }) - y0) / pitch)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const double px = x0 + i * pitch, py = y0 + j * pitch;
                const double u = ((b.x() - px) * (c.y() - py) - (c.x() - px) * (b.y() - py)) / det;
                const double v = ((c.x() - px) * (a.y() - py) - (a.x() - px) * (c.y() - py)) / det;
                const double w = 1. - u - v;
                if (u < -1e-9 || v < -1e-9 || w < -1e-9)
                    continue;
                float &cell = z[size_t(j) * size_t(nx) + size_t(i)];
                const float h = float(u * a.z() + v * b.z() + w * c.z());
                cell = std::isnan(cell) ? h : std::max(cell, h);
            }
    }
    const auto at = [&](const Vec2d &p) {
        const int i = int(std::lround((p.x() - x0) / pitch)), j = int(std::lround((p.y() - y0) / pitch));
        return i < 0 || j < 0 || i >= nx || j >= ny ? std::numeric_limits<float>::quiet_NaN() : z[size_t(j) * size_t(nx) + size_t(i)];
    };
    const auto cost = [&](double angle) {
        const Vec2d d(std::cos(angle), std::sin(angle)), n(-d.y(), d.x());
        double lo = std::numeric_limits<double>::infinity(), hi = -lo, tlo = lo, thi = -lo;
        for (const Vec2d &corner : { Vec2d(x0, y0), Vec2d(x0 + nx * pitch, y0), Vec2d(x0, y0 + ny * pitch), Vec2d(x0 + nx * pitch, y0 + ny * pitch) }) {
            lo = std::min(lo, n.dot(corner)), hi = std::max(hi, n.dot(corner));
            tlo = std::min(tlo, d.dot(corner)), thi = std::max(thi, d.dot(corner));
        }
        double roads = 0.;
        for (double o = lo + 0.5 * spacing; o < hi; o += spacing) {
            bool in = false;
            for (double t = tlo; t <= thi; t += pitch) {
                const bool on = !std::isnan(at(o * n + t * d));
                roads += on && !in;
                in = on;
            }
        }
        return roads;
    };
    const auto wrap  = [](double a) { a = std::fmod(a, M_PI); return a < 0. ? a + M_PI : a; };
    const auto apart = [](double a, double b) { const double d = std::abs(a - b); return std::min(d, M_PI - d) >= M_PI / 18.; };
    std::vector<double> candidates;
    const auto add = [&](double a) { a = wrap(a); if (std::all_of(candidates.begin(), candidates.end(), [&](double b) { return apart(a, b); })) candidates.push_back(a); };
    if (level) add(*level);
    for (double a : auto_top_angles(surface_outline_points(surface)))
        add(a);
    for (int k = 0; k < 4; ++k)
        add(k * 0.25 * M_PI);
    std::vector<std::pair<double, double>> scored;
    for (double a : candidates)
        scored.emplace_back(cost(a), a);
    std::stable_sort(scored.begin(), scored.end(), [](const auto &l, const auto &r) { return l.first < r.first; });
    if (scored.empty())
        return {};
    if (level)
        if (const auto it = std::find_if(scored.begin(), scored.end(), [&](const auto &e) { return !apart(e.second, wrap(*level)); });
            it != scored.end() && it->first <= 1.03 * scored.front().first)
            std::rotate(scored.begin(), it, it + 1);
    std::vector<double> out;
    for (size_t i = 0; i < scored.size() && int(out.size()) < AutoTopCandidates; ++i)
        out.push_back(scored[i].second);
    return out;
}

}
