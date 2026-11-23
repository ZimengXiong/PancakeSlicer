// Curved tops port of fich-l / OrcaSlicer-Nonplanar v0.1 (AGPL-3.0).
#include "CurvedTopsSchedule.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "CurvedTopsAccess.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/ConformalTravel.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/libslic3r.h"
#include "CurvedTopsSurface.hpp"
#include "HeadEnvelope.hpp"
#include <nlohmann/json.hpp>
#include "StageCache.hpp"
#include <map>
#include <memory>
#include "libslic3r/Print.hpp"

#include <boost/functional/hash.hpp>
#include <boost/log/trivial.hpp>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

static Nonplanar::HeadEnvelope planning_head(const PrintConfig &config)
{
    Nonplanar::HeadEnvelope head = Nonplanar::resolve_full_head_envelope(config);
    head.boxes.erase(std::remove_if(head.boxes.begin(), head.boxes.end(), [](const Nonplanar::HeadBox &box) { return box.duct; }), head.boxes.end());
    return head;
}

NonplanarClearanceModel NonplanarClearanceModel::from_print(const PrintConfig &config, double height)
{
    NonplanarClearanceModel out;
    const auto head = planning_head(config);
    if (head.empty()) throw Slic3r::SlicingError("Curved tops requires a printer head envelope.");
    out.tip_radius_mm = head.tip_flat_radius;
    out.contact_tolerance = head.contact_tolerance;
    out.contact_allowance_mm = std::min(0.1, 0.5 * height);
    std::map<double, double> radius;
    for (const auto &box : head.boxes) {
        if (box.nozzle) {
            const double floor = box.min.z() <= 0.001 ? out.contact_allowance_mm : box.min.z() + 0.05;
            const double r = std::max({std::abs(box.min.x()), std::abs(box.max.x()), std::abs(box.min.y()), std::abs(box.max.y())});
            radius[floor] = std::max(radius[floor], r);
        } else {
            constexpr double extent = 1000.;
            out.boxes.push_back({box.min.z() - head.margin,
                box.infinite_x ? -extent : box.min.x() - head.lateral_margin,
                box.infinite_x ? extent : box.max.x() + head.lateral_margin,
                box.infinite_y ? -extent : box.min.y() - head.lateral_margin,
                box.infinite_y ? extent : box.max.y() + head.lateral_margin});
        }
    }
    for (const auto &[bottom, r] : radius) out.nozzle.push_back({bottom, r});
    if (out.nozzle.empty())
        out.nozzle.push_back({out.contact_allowance_mm, Nonplanar::nozzle_land(head).boxes.front().max.x()});
    return out;
}

NonplanarClearanceModel NonplanarClearanceModel::for_object(const PrintObject &object, double height)
{
    std::string warning;
    Nonplanar::resolve_head_envelope(object.print()->config(), &warning);
    if (!warning.empty())
        throw Slic3r::SlicingError(object.model_object()->name + ": Curved tops needs this printer's print head model to plan head access. " + warning);
    return from_print(object.print()->config(), height);
}

const NonplanarDeferral::PathPlan* NonplanarDeferral::find(size_t layer_id, size_t source_path_id,
                                                           const Polyline3 &polyline3, bool &reversed) const
{
    if (layer_id >= layers.size() || polyline3.points.size() < 2)
        return nullptr;
    const Polyline polyline = polyline3.to_polyline();
    const auto it = layers[layer_id].find(source_path_id);
    if (it == layers[layer_id].end())
        return nullptr;
    auto close_to = [](const Point &a, const Point &b) { return (a - b).cast<double>().norm() <= 2.; };
    const Points &pts = polyline.points;
    for (const PathPlan &plan : it->second) {
        if (close_to(plan.first, pts.front()) && close_to(plan.second, pts[1]) &&
            close_to(plan.last, pts.back()) && close_to(plan.before_last, pts[pts.size() - 2])) {
            reversed = false;
            return &plan;
        }
        if (close_to(plan.first, pts.back()) && close_to(plan.second, pts[pts.size() - 2]) &&
            close_to(plan.last, pts.front()) && close_to(plan.before_last, pts[1])) {
            reversed = true;
            return &plan;
        }
    }
    return nullptr;
}

namespace {

struct Sample {
    double x, y, z;
    double half_width;
    int    layer;
    int    slot;
    size_t path_id;
    int    path;
    double s;
    int    group { -1 };
};

void collect_paths(const ExtrusionEntity *entity, std::vector<std::pair<const ExtrusionPath*, const ExtrusionEntity*>> &out)
{
    if (auto *path = dynamic_cast<const ExtrusionPath*>(entity))
        out.emplace_back(path, entity);
    else if (auto *loop = dynamic_cast<const ExtrusionLoop*>(entity))
        for (const ExtrusionPath &p : loop->paths) out.emplace_back(&p, entity);
    else if (auto *multi = dynamic_cast<const ExtrusionMultiPath*>(entity))
        for (const ExtrusionPath &p : multi->paths) out.emplace_back(&p, entity);
    else if (auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(entity))
        for (const ExtrusionEntity *child : collection->entities) collect_paths(child, out);
}

void sample_path(const ExtrusionPath &path, double planar_z, int layer, int slot, double step,
                 int index, std::vector<Sample> &out)
{
    const Points3 &pts = path.polyline.points;
    auto z_of = [planar_z, &path](const Point3 &p) { return path.nonplanar_source ? unscale<double>(p.z()) : planar_z; };
    const double hw = 0.5 * double(path.width);
    double walked = 0.;
    for (size_t i = 0; i < pts.size(); ++ i) {
        const double x1 = unscale<double>(pts[i].x()), y1 = unscale<double>(pts[i].y()), z1 = z_of(pts[i]);
        if (i == 0) {
            out.push_back({ x1, y1, z1, hw, layer, slot, path.source_path_id, index, 0. });
            continue;
        }
        const double x0 = unscale<double>(pts[i-1].x()), y0 = unscale<double>(pts[i-1].y()), z0 = z_of(pts[i-1]);
        const double length = std::hypot(x1 - x0, y1 - y0);
        const double scaled_length = (pts[i].to_point() - pts[i-1].to_point()).cast<double>().norm();
        const int n = std::max(1, int(std::ceil(length / step)));
        for (int k = 1; k <= n; ++ k) {
            const double t = double(k) / n;
            out.push_back({ x0 + t * (x1 - x0), y0 + t * (y1 - y0), z0 + t * (z1 - z0), hw, layer, slot,
                            path.source_path_id, index, walked + t * scaled_length });
        }
        walked += scaled_length;
    }
}

class Grid {
public:
    Grid(double cell, double x_min, double y_min, double x_max, double y_max) :
        m_cell(cell), m_x0(index(x_min)), m_y0(index(y_min)),
        m_nx(std::max(1, index(x_max) - m_x0 + 1)), m_ny(std::max(1, index(y_max) - m_y0 + 1)),
        m_cells(size_t(m_nx) * size_t(m_ny)) {}
    void insert(int idx, double x, double y) { m_cells[slot(index(x), index(y))].push_back(idx); }
    template<typename F> void visit(double x, double y, double radius, F &&f) const {
        const int ix0 = std::max(index(x - radius), m_x0), ix1 = std::min(index(x + radius), m_x0 + m_nx - 1);
        const int iy0 = std::max(index(y - radius), m_y0), iy1 = std::min(index(y + radius), m_y0 + m_ny - 1);
        for (int ix = ix0; ix <= ix1; ++ ix)
            for (int iy = iy0; iy <= iy1; ++ iy)
                for (int idx : m_cells[slot(ix, iy)]) f(idx);
    }
    template<typename Z> void sort_cells(Z &&z_of) {
        for (std::vector<int> &cell : m_cells)
            std::stable_sort(cell.begin(), cell.end(), [&z_of](int a, int b) { return z_of(a) < z_of(b); });
    }
    template<typename Z, typename F> void visit_z(double x, double y, double radius, double z_lo, double z_hi, Z &&z_of, F &&f) const {
        const int ix0 = std::max(index(x - radius), m_x0), ix1 = std::min(index(x + radius), m_x0 + m_nx - 1);
        const int iy0 = std::max(index(y - radius), m_y0), iy1 = std::min(index(y + radius), m_y0 + m_ny - 1);
        for (int ix = ix0; ix <= ix1; ++ ix)
            for (int iy = iy0; iy <= iy1; ++ iy) {
                const std::vector<int> &cell = m_cells[slot(ix, iy)];
                auto it = std::upper_bound(cell.begin(), cell.end(), z_lo, [&z_of](double z, int idx) { return z < z_of(idx); });
                for (; it != cell.end() && z_of(*it) < z_hi; ++ it)
                    f(*it);
            }
    }
    template<typename F> void visit_cells(double x, double y, double radius, F &&f) const {
        const int ix0 = std::max(index(x - radius), m_x0), ix1 = std::min(index(x + radius), m_x0 + m_nx - 1);
        const int iy0 = std::max(index(y - radius), m_y0), iy1 = std::min(index(y + radius), m_y0 + m_ny - 1);
        for (int ix = ix0; ix <= ix1; ++ ix)
            for (int iy = iy0; iy <= iy1; ++ iy)
                f(slot(ix, iy));
    }
    int    index(double v) const { return int(std::floor(v / m_cell)); }
    int    cell_slot(int ix, int iy) const {
        return ix < m_x0 || iy < m_y0 || ix >= m_x0 + m_nx || iy >= m_y0 + m_ny ? -1 : int(slot(ix, iy));
    }
    size_t cell_count() const { return m_cells.size(); }
    int    x0() const { return m_x0; }
    int    y0() const { return m_y0; }
    int    x1() const { return m_x0 + m_nx - 1; }
    int    y1() const { return m_y0 + m_ny - 1; }
private:
    size_t slot(int ix, int iy) const { return size_t(ix - m_x0) * size_t(m_ny) + size_t(iy - m_y0); }
    double m_cell;
    int    m_x0, m_y0, m_nx, m_ny;
    std::vector<std::vector<int>> m_cells;
};

std::string describe(const Sample &s, const char *what)
{
    std::ostringstream out;
    out.precision(4);
    out << what << " at X" << std::fixed << s.x << " Y" << s.y << " Z" << s.z << " (layer " << s.layer << ")";
    return out.str();
}

}
struct UnitKeyHash
{
    template<class T> static void mix(size_t &h, const T &v) { h ^= std::hash<T>{}(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); }
    template<class... Ts> size_t operator()(const std::tuple<Ts...> &t) const
    {
        size_t h = 0;
        std::apply([&h](const Ts &...v) { (mix(h, v), ...); }, t);
        return h;
    }
};

size_t nonplanar_plan_fingerprint(const PrintObject &object)
{
    size_t hash = 0;
    boost::hash_combine(hash, int(object.config().nonplanar_mode.value));
    boost::hash_combine(hash, object.config().nonplanar_curved_max_angle.value);
    boost::hash_combine(hash, object.config().layer_height.value);
    for (const Layer *layer : object.layers()) {
        boost::hash_combine(hash, layer->id());
        boost::hash_combine(hash, layer->print_z);
        for (const LayerRegion *layerm : layer->regions()) {
            std::vector<std::pair<const ExtrusionPath*, const ExtrusionEntity*>> paths;
            for (const ExtrusionEntity *entity : layerm->perimeters.entities) collect_paths(entity, paths);
            boost::hash_combine(hash, paths.size());
            for (const ExtrusionEntity *entity : layerm->fills.entities) collect_paths(entity, paths);
            boost::hash_combine(hash, paths.size());
            const ExtrusionEntity *unit = nullptr;
            for (const auto &[path, entity] : paths) {
                boost::hash_combine(hash, entity != unit);
                unit = entity;
                boost::hash_combine(hash, int(path->role()));
                boost::hash_combine(hash, path->width);
                boost::hash_combine(hash, path->nonplanar_source);
                boost::hash_combine(hash, path->distance_to_top);
                boost::hash_combine(hash, path->source_surface_id);
                boost::hash_combine(hash, path->polyline.points.size());
                for (const Point3 &p : path->polyline.points) {
                    boost::hash_combine(hash, p.x());
                    boost::hash_combine(hash, p.y());
                    boost::hash_combine(hash, p.z());
                }
            }
        }
    }
    return hash;
}

namespace {

std::string memo_bytes(const NonplanarPlanMemo &memo)
{
    std::string out;
    const auto put = [&out](const auto &v) { out.append(reinterpret_cast<const char *>(&v), sizeof(v)); };
    put(memo.fingerprint);
    put(memo.model);
    put(uint64_t(memo.ids.size()));
    out.append(reinterpret_cast<const char *>(memo.ids.data()), memo.ids.size() * sizeof(uint64_t));
    const NonplanarDeferral &plan = memo.plan;
    for (size_t v : { plan.conformal_samples, plan.planar_samples, plan.deferred_samples, plan.deferred_paths, plan.cut_paths,
                      plan.refined_units, plan.advanced_paths })
        put(uint64_t(v));
    put(uint64_t(plan.layers.size()));
    for (const auto &layer : plan.layers) {
        put(uint64_t(layer.size()));
        for (const auto &[id, plans] : layer) {
            put(uint64_t(id));
            put(uint64_t(plans.size()));
            for (const NonplanarDeferral::PathPlan &p : plans) {
                for (const Point *pt : { &p.first, &p.second, &p.before_last, &p.last })
                    put(*pt);
                put(p.length);
                put(uint64_t(p.breaks.size()));
                for (const auto &[s, slot] : p.breaks) {
                    put(s);
                    put(slot);
                }
            }
        }
    }
    return out;
}

bool memo_from_bytes(const std::string &in, NonplanarPlanMemo &memo)
{
    size_t at = 0;
    bool   ok = true;
    const auto get = [&](auto &v) {
        if (!ok || in.size() - at < sizeof(v)) { ok = false; return; }
        std::memcpy(&v, in.data() + at, sizeof(v));
        at += sizeof(v);
    };
    const auto count = [&](size_t each) {
        uint64_t n = 0;
        get(n);
        if (ok && (each == 0 ? n > in.size() : n > (in.size() - at) / each)) ok = false;
        return ok ? size_t(n) : size_t(0);
    };
    get(memo.fingerprint);
    get(memo.model);
    memo.ids.resize(count(sizeof(uint64_t)));
    for (uint64_t &id : memo.ids) get(id);
    NonplanarDeferral &plan = memo.plan;
    for (size_t *v : { &plan.conformal_samples, &plan.planar_samples, &plan.deferred_samples, &plan.deferred_paths, &plan.cut_paths,
                       &plan.refined_units, &plan.advanced_paths }) {
        uint64_t x = 0;
        get(x);
        *v = size_t(x);
    }
    plan.layers.resize(count(sizeof(uint64_t)));
    for (auto &layer : plan.layers) {
        const size_t ids = count(2 * sizeof(uint64_t));
        for (size_t i = 0; i < ids && ok; ++ i) {
            uint64_t id = 0;
            get(id);
            std::vector<NonplanarDeferral::PathPlan> &plans = layer[size_t(id)];
            plans.resize(count(4 * sizeof(Point) + sizeof(double) + sizeof(uint64_t)));
            for (NonplanarDeferral::PathPlan &p : plans) {
                for (Point *pt : { &p.first, &p.second, &p.before_last, &p.last })
                    get(*pt);
                get(p.length);
                p.breaks.resize(count(sizeof(double) + sizeof(int)));
                for (auto &[s, slot] : p.breaks) {
                    get(s);
                    get(slot);
                }
            }
        }
    }
    return ok && at == in.size();
}

}

NonplanarDeferral plan_nonplanar_deferral_reusing(const PrintObject &object, const NonplanarClearanceModel &model, size_t fingerprint)
{
    size_t key = 0;
    for (double v : { model.tip_radius_mm, model.contact_allowance_mm, model.safety_mm, model.bead_margin, model.sample_step })
        boost::hash_combine(key, v);
    for (const NonplanarClearanceModel::Ring &r : model.nozzle)
        for (double v : { r.bottom, r.radius })
            boost::hash_combine(key, v);
    for (const NonplanarClearanceModel::Box &b : model.boxes)
        for (double v : { b.bottom, b.x_min, b.x_max, b.y_min, b.y_max })
            boost::hash_combine(key, v);
    std::vector<uint64_t> ids;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *layerm : layer->regions()) {
            std::vector<std::pair<const ExtrusionPath*, const ExtrusionEntity*>> paths;
            for (const ExtrusionEntity *entity : layerm->perimeters.entities) collect_paths(entity, paths);
            for (const ExtrusionEntity *entity : layerm->fills.entities) collect_paths(entity, paths);
            for (const auto &path : paths)
                ids.push_back(path.first->source_path_id);
        }
    std::shared_ptr<const NonplanarPlanMemo> &memo = object.m_tops_plan_memo;
    char kind[64];
    std::snprintf(kind, sizeof(kind), "plan.%016llx.%016llx", static_cast<unsigned long long>(fingerprint), static_cast<unsigned long long>(key));
    if (!(memo && memo->fingerprint == fingerprint && memo->model == key)) {
        std::string bytes;
        auto cached = std::make_shared<NonplanarPlanMemo>();
        if (Nonplanar::stage_cache_read(object.m_stage_cache_key, kind, bytes)) {
            if (memo_from_bytes(bytes, *cached) && cached->fingerprint == fingerprint && cached->model == key) {
                BOOST_LOG_TRIVIAL(info) << "Nonplanar deposition order: the stage cache holds the plan of these paths";
                memo = std::move(cached);
            } else
                BOOST_LOG_TRIVIAL(warning) << "Stage cache: an unreadable deposition plan; planning";
        }
    }
    if (memo && memo->fingerprint == fingerprint && memo->model == key && memo->ids.size() == ids.size()) {
        std::map<uint64_t, uint64_t> renumber, inverse;
        bool same = true;
        for (size_t i = 0; i < ids.size() && same; ++ i)
            same = renumber.emplace(memo->ids[i], ids[i]).first->second == ids[i] && inverse.emplace(ids[i], memo->ids[i]).first->second == memo->ids[i];
        if (same) {
            NonplanarDeferral plan = memo->plan;
            for (std::map<size_t, std::vector<NonplanarDeferral::PathPlan>> &layer : plan.layers) {
                std::map<size_t, std::vector<NonplanarDeferral::PathPlan>> renumbered;
                for (auto &[id, plans] : layer) {
                    const auto it = renumber.find(id);
                    if (it == renumber.end()) { same = false; break; }
                    renumbered.emplace(it->second, std::move(plans));
                }
                layer = std::move(renumbered);
            }
            if (same) {
                BOOST_LOG_TRIVIAL(info) << "Nonplanar deposition order: the paths plan as the last export's; its plan reused";
                return plan;
            }
        }
    }
    NonplanarDeferral plan = plan_nonplanar_deferral(object, model);
    memo = std::make_shared<const NonplanarPlanMemo>(NonplanarPlanMemo{ fingerprint, key, std::move(ids), plan });
    Nonplanar::stage_cache_write(object.m_stage_cache_key, kind, memo_bytes(*memo));
    return plan;
}

NonplanarDeferral plan_nonplanar_deferral(const PrintObject &object, const NonplanarClearanceModel &model)
{
    NonplanarDeferral result;
    const PrintBase *print = object.print();
    auto throw_if_canceled = [print]() {
        if (print != nullptr && print->canceled())
            throw CanceledException();
    };
    const auto layers = object.layers();
    size_t layer_slots = 0;
    for (const Layer *layer : layers)
        layer_slots = std::max(layer_slots, layer->id() + 1);
    result.layers.assign(layer_slots, {});

    std::vector<Sample> conformal;
    std::vector<std::tuple<const ExtrusionPath*, const ExtrusionEntity*, int, double>> planar_paths;
    struct Sampled { const ExtrusionPath *path; const ExtrusionEntity *entity; int layer; };
    std::vector<Sampled> sampled;
    for (const Layer *layer : layers) {
        const int lid = int(layer->id());
        for (const LayerRegion *layerm : layer->regions()) {
            std::vector<std::pair<const ExtrusionPath*, const ExtrusionEntity*>> paths;
            for (const ExtrusionEntity *entity : layerm->perimeters.entities) collect_paths(entity, paths);
            for (const ExtrusionEntity *entity : layerm->fills.entities) collect_paths(entity, paths);
            for (const auto &[path, unit] : paths) {
                if (path->role() == erIroning || path->polyline.points.empty())
                    continue;
                if (path->nonplanar_source)
                {
                    sample_path(*path, layer->print_z, lid, 2 * lid, model.sample_step, int(sampled.size()), conformal);
                    sampled.push_back({ path, unit, lid });
                }
                else
                    planar_paths.emplace_back(path, unit, lid, layer->print_z);
            }
        }
    }
    if (conformal.empty())
        return result;

    struct Model : NonplanarClearanceModel { std::vector<Ring> ring_list; };
    Model m_model;
    static_cast<NonplanarClearanceModel&>(m_model) = model;
    m_model.ring_list = model.rings();
    double min_bottom = std::numeric_limits<double>::max();
    double ring_reach = 0., box_reach = 0.;
    for (const NonplanarClearanceModel::Ring &r : m_model.ring_list) {
        min_bottom = std::min(min_bottom, r.bottom);
        ring_reach = std::max(ring_reach, r.radius);
    }
    for (const NonplanarClearanceModel::Box &b : model.boxes) {
        min_bottom = std::min(min_bottom, b.bottom);
        box_reach = std::max({ box_reach, std::abs(b.x_min), std::abs(b.x_max), std::abs(b.y_min), std::abs(b.y_max) });
    }
    const double threshold = min_bottom - model.safety_mm + model.contact_tolerance;
    auto blocks = [&m_model](const Sample &m, const Sample &t, bool tolerant = false) {
        const double dz = m.z - t.z - (tolerant ? m_model.safety_mm + 0.001 : 0.);
        const double grow = m.half_width + m_model.bead_margin + (tolerant ? 0. : m_model.safety_mm + m_model.sample_step);
        const double d = std::hypot(m.x - t.x, m.y - t.y);
        for (const NonplanarClearanceModel::Ring &r : m_model.ring_list)
            if (dz > r.bottom - m_model.safety_mm + m_model.contact_tolerance && d < r.radius + grow)
                return true;
        const double rx = m.x - t.x, ry = m.y - t.y;
        for (const NonplanarClearanceModel::Box &b : m_model.boxes)
            if (dz > b.bottom - m_model.safety_mm + m_model.contact_tolerance &&
                rx > b.x_min - grow && rx < b.x_max + grow && ry > b.y_min - grow && ry < b.y_max + grow)
                return true;
        return false;
    };

    double zmin = std::numeric_limits<double>::max();
    double zmax_conformal = std::numeric_limits<double>::lowest();
    BoundingBoxf bbox;
    for (const Sample &c : conformal) {
        zmin = std::min(zmin, c.z);
        zmax_conformal = std::max(zmax_conformal, c.z);
        bbox.merge(Vec2d(c.x, c.y));
    }
    bbox.offset(box_reach + ring_reach + 1.);

    std::vector<Sample> planar;
    double zmax = zmin;
    for (const auto &[path, unit, lid, z] : planar_paths) {
        throw_if_canceled();
        if (z <= zmin + threshold)
            continue;
        const size_t first = planar.size();
        sample_path(*path, z, lid, 2 * lid, model.sample_step, int(sampled.size()), planar);
        bool inside = false;
        for (size_t i = first; i < planar.size() && ! inside; ++ i)
            inside = bbox.contains(Vec2d(planar[i].x, planar[i].y));
        if (! inside)
            planar.resize(first);
        else {
            sampled.push_back({ path, unit, lid });
            zmax = std::max(zmax, z);
        }
    }
    result.conformal_samples = conformal.size();
    result.planar_samples    = planar.size();

    std::vector<Sample> all = std::move(planar);
    const size_t first_conformal = all.size();
    all.insert(all.end(), conformal.begin(), conformal.end());
    std::vector<int> base(all.size());
    int max_layer = 0;
    double max_half_width = 0.;
    for (size_t i = 0; i < all.size(); ++ i) {
        base[i] = all[i].slot;
        max_half_width = std::max(max_half_width, all[i].half_width);
        max_layer = std::max(max_layer, all[i].layer);
        zmax = std::max(zmax, all[i].z);
    }
    for (const Layer *layer : layers)
        max_layer = std::max(max_layer, int(layer->id()));

    double sx_min = 0., sy_min = 0., sx_max = 0., sy_max = 0.;
    for (size_t i = 0; i < all.size(); ++ i) {
        sx_min = i == 0 ? all[i].x : std::min(sx_min, all[i].x); sx_max = i == 0 ? all[i].x : std::max(sx_max, all[i].x);
        sy_min = i == 0 ? all[i].y : std::min(sy_min, all[i].y); sy_max = i == 0 ? all[i].y : std::max(sy_max, all[i].y);
    }
    constexpr double fine_cell = 0.5;
    Grid grid(fine_cell, sx_min, sy_min, sx_max, sy_max);
    std::vector<double> fine_zmax(grid.cell_count(), -std::numeric_limits<double>::max());
    for (int i = 0; i < int(all.size()); ++i) {
        grid.insert(i, all[i].x, all[i].y);
        const size_t cell = size_t(grid.cell_slot(grid.index(all[i].x), grid.index(all[i].y)));
        fine_zmax[cell] = std::max(fine_zmax[cell], all[i].z);
    }
    auto z_of = [&all](int i) { return all[size_t(i)].z; };
    grid.sort_cells(z_of);
    double block_dz_min = std::numeric_limits<double>::max();
    for (const NonplanarClearanceModel::Ring &r : m_model.ring_list)
        block_dz_min = std::min(block_dz_min, r.bottom - m_model.safety_mm + m_model.contact_tolerance);
    for (const NonplanarClearanceModel::Box &b : m_model.boxes)
        block_dz_min = std::min(block_dz_min, b.bottom - m_model.safety_mm + m_model.contact_tolerance);
    block_dz_min -= 1e-6;

    constexpr double coarse_cell = 2.;
    Grid coarse(coarse_cell, sx_min, sy_min, sx_max, sy_max);
    auto coarse_key = [&coarse](double x, double y) { return coarse.cell_slot(coarse.index(x), coarse.index(y)); };
    std::vector<double> coarse_zmax(coarse.cell_count(), -std::numeric_limits<double>::max());
    std::vector<char>   coarse_used(coarse.cell_count(), 0);
    for (int i = 0; i < int(all.size()); ++ i) {
        coarse.insert(i, all[i].x, all[i].y);
        if (i + 1 == int(all.size()))
            coarse.sort_cells(z_of);
        const int k = coarse_key(all[i].x, all[i].y);
        coarse_zmax[size_t(k)] = coarse_used[size_t(k)] ? std::max(coarse_zmax[size_t(k)], all[i].z) : all[i].z;
        coarse_used[size_t(k)] = 1;
    }
    std::vector<NonplanarClearanceModel::Box> boxes_by_bottom(model.boxes.begin(), model.boxes.end());
    std::stable_sort(boxes_by_bottom.begin(), boxes_by_bottom.end(),
        [](const NonplanarClearanceModel::Box &a, const NonplanarClearanceModel::Box &b) { return a.bottom < b.bottom; });
    const double full_ring_reach = ring_reach + 0.5 + model.sample_step;
    std::vector<double> local_top(grid.cell_count(), -std::numeric_limits<double>::max());
    for (int ix = grid.x0(); ix <= grid.x1(); ++ix)
        for (int iy = grid.y0(); iy <= grid.y1(); ++iy) {
            const size_t cell = size_t(grid.cell_slot(ix, iy));
            if (fine_zmax[cell] == -std::numeric_limits<double>::max()) continue;
            grid.visit_cells((ix + 0.5) * fine_cell, (iy + 0.5) * fine_cell, full_ring_reach + 0.5 * fine_cell + 1e-6,
                [&](size_t k) { local_top[cell] = std::max(local_top[cell], fine_zmax[k]); });
        }
    auto reach = [&](int t, double material_top = std::numeric_limits<double>::max()) {
        const Sample &tool = all[size_t(t)];
        if (std::min(zmax, material_top) - tool.z <= threshold) return 0.;
        const double full = full_ring_reach;
        const size_t cell = size_t(grid.cell_slot(grid.index(tool.x), grid.index(tool.y)));
        const double dz = std::min(local_top[cell], material_top) - tool.z;
        if (!boxes_by_bottom.empty() && dz > boxes_by_bottom.front().bottom - model.safety_mm + model.contact_tolerance) return full;
        const double pad = std::max(0.5 + model.sample_step,
            max_half_width + model.bead_margin + model.safety_mm + model.sample_step);
        double radius = 0.;
        for (const auto &ring : m_model.ring_list)
            if (dz > ring.bottom - model.safety_mm + model.contact_tolerance) radius = std::max(radius, ring.radius + pad);
        return std::min(full, radius);
    };
    double box_x_min = 0., box_x_max = 0., box_y_min = 0., box_y_max = 0.;
    for (const NonplanarClearanceModel::Box &b : model.boxes) {
        box_x_min = std::min(box_x_min, b.x_min); box_x_max = std::max(box_x_max, b.x_max);
        box_y_min = std::min(box_y_min, b.y_min); box_y_max = std::max(box_y_max, b.y_max);
    }
    auto visit_boxes = [&](int t, auto &&f) {
        if (model.boxes.empty())
            return;
        const Sample &s0 = all[t];
        double lowest_floor = std::numeric_limits<double>::max();
        for (const NonplanarClearanceModel::Box &b : model.boxes)
            lowest_floor = std::min(lowest_floor, s0.z + b.bottom - model.safety_mm + model.contact_tolerance);
        if (zmax <= lowest_floor)
            return;
        for (double cx = std::max(std::floor((s0.x + box_x_min - 0.5) / coarse_cell), double(coarse.x0()));
             cx <= std::min(std::floor((s0.x + box_x_max + 0.5) / coarse_cell), double(coarse.x1())); cx += 1.)
            for (double cy = std::max(std::floor((s0.y + box_y_min - 0.5) / coarse_cell), double(coarse.y0()));
                 cy <= std::min(std::floor((s0.y + box_y_max + 0.5) / coarse_cell), double(coarse.y1())); cy += 1.) {
                const double x = (cx + 0.5) * coarse_cell, y = (cy + 0.5) * coarse_cell;
                const int k = coarse_key(x, y);
                if (k < 0 || ! coarse_used[size_t(k)] || coarse_zmax[size_t(k)] <= lowest_floor)
                    continue;
                const double cell_zmax = coarse_zmax[size_t(k)];
                bool covered = false;
                for (const NonplanarClearanceModel::Box &b : boxes_by_bottom) {
                    if (cell_zmax <= s0.z + b.bottom - model.safety_mm + model.contact_tolerance)
                        break;
                    if (cx >= std::floor((s0.x + b.x_min - 0.5) / coarse_cell) && cx <= std::floor((s0.x + b.x_max + 0.5) / coarse_cell) &&
                        cy >= std::floor((s0.y + b.y_min - 0.5) / coarse_cell) && cy <= std::floor((s0.y + b.y_max + 0.5) / coarse_cell)) {
                        covered = true;
                        break;
                    }
                }
                if (covered)
                    coarse.visit_z(x, y, 0., s0.z + block_dz_min, std::numeric_limits<double>::max(), z_of, f);
            }
    };
    auto i_is_conformal = [&](int i) { return size_t(i) >= first_conformal; };

    constexpr double edge_band = 1.5;
    constexpr int    max_level = 4;
    std::vector<int> edge_level(all.size(), 0);
    auto chunk_length = [](int l) { return l <= 1 ? 1e30 : scaled<double>(l == 2 ? 1. : l == 3 ? 0.25 : 0.01); };
    std::vector<int> earliest(all.size());
    constexpr double height_band_width = 0.099;
    std::vector<int> height_band(all.size(), 0);
    bool banded = false, all_height_bands = false;
    auto compute_earliest = [&]() {
        std::vector<std::pair<double, int>> tops;
        for (const Layer *layer : layers)
            tops.emplace_back(layer->print_z, int(layer->id()));
        std::sort(tops.begin(), tops.end());
        for (size_t i = 0; i < all.size(); ++ i) {
            earliest[i] = base[i];
            const ExtrusionPath &path = *sampled[size_t(all[i].path)].path;
            if (!banded || !path.nonplanar_source || (!all_height_bands && path.distance_to_top < 0.001f))
                continue;
            auto it = std::upper_bound(tops.begin(), tops.end(), std::make_pair(all[i].z + 0.03, std::numeric_limits<int>::max()));
            if (it != tops.begin())
                earliest[i] = std::min(base[i], 2 * std::prev(it)->second + 1);
        }
    };
    compute_earliest();
    std::vector<std::pair<size_t, size_t>> spans;
    std::vector<char> path_has_edge(sampled.size(), 0);
    auto make_spans = [&]() {
        spans.clear();
        for (size_t i = 0; i < all.size();) {
            size_t j = i + 1;
            while (j < all.size() && all[j].path == all[i].path && earliest[j] == earliest[i] &&
                   height_band[j] == height_band[i]) ++j;
            spans.emplace_back(i, j);
            i = j;
        }
    };
    make_spans();
    const bool verify_dependents = research_switch("NONPLANAR_PLAN_VERIFY") != nullptr;
    std::vector<int> unit_of(all.size());
    std::vector<double> unit_zmin, unit_zmax;
    std::vector<std::vector<int>> members;
    std::vector<BoundingBoxf3> unit_bounds;
    auto visit_followers = [&](int t, auto &&wanted, auto &&found, bool with_boxes = true) {
        const Sample &s0 = all[size_t(t)];
        auto check = [&](int m) {
            if (m != t && wanted(m) && blocks(all[size_t(m)], s0))
                found(m);
        };
        const double r = reach(t);
        if (r > 0)
            grid.visit_z(s0.x, s0.y, r, s0.z + block_dz_min, std::numeric_limits<double>::max(), z_of, check);
        if (with_boxes)
            visit_boxes(t, check);
        grid.visit_z(s0.x, s0.y, 2. * s0.half_width, s0.z + 0.1 - 1e-6, s0.z + 0.35 + 1e-6, z_of, [&](int r2) {
            const Sample &s1 = all[size_t(r2)];
            if (r2 != t && wanted(r2) && s1.z - s0.z > 0.1 && s1.z - s0.z < 0.35 &&
                std::hypot(s1.x - s0.x, s1.y - s0.y) < 0.5 * (s1.half_width + s0.half_width))
                found(r2);
        });
    };
    std::vector<int> collected_for;
    struct CellGroup { int unit; double zmax; int begin, end; };
    std::vector<std::vector<CellGroup>> cell_groups(coarse.cell_count());
    std::vector<int> cell_samples;
    std::vector<std::vector<CellGroup>> fine_groups(grid.cell_count());
    std::vector<int> fine_samples;

    auto collect_box_dependents = [&](int t, int u, std::vector<int> &out, const std::vector<char> *only, double material_top, std::vector<int> &marker) {
        if (model.boxes.empty())
            return;
        const Sample &s0 = all[size_t(t)];
        double lowest_floor = std::numeric_limits<double>::max();
        for (const NonplanarClearanceModel::Box &b : model.boxes)
            lowest_floor = std::min(lowest_floor, s0.z + b.bottom - model.safety_mm + model.contact_tolerance);
        if (material_top <= lowest_floor)
            return;
        for (double cx = std::max(std::floor((s0.x + box_x_min - 0.5) / coarse_cell), double(coarse.x0()));
             cx <= std::min(std::floor((s0.x + box_x_max + 0.5) / coarse_cell), double(coarse.x1())); cx += 1.)
            for (double cy = std::max(std::floor((s0.y + box_y_min - 0.5) / coarse_cell), double(coarse.y0()));
                 cy <= std::min(std::floor((s0.y + box_y_max + 0.5) / coarse_cell), double(coarse.y1())); cy += 1.) {
                const int key = coarse_key((cx + 0.5) * coarse_cell, (cy + 0.5) * coarse_cell);
                if (key < 0 || cell_groups[size_t(key)].empty() || coarse_zmax[size_t(key)] <= lowest_floor)
                    continue;
                const size_t stamp = unit_zmin.size() + fine_groups.size() + size_t(key);
                if (!verify_dependents && marker[stamp] == u) continue;
                const double unit_floor = unit_zmin[size_t(u)] + boxes_by_bottom.front().bottom - model.safety_mm + model.contact_tolerance;
                bool complete = true;
                for (const CellGroup &g : cell_groups[size_t(key)]) {
                    if (g.unit == u || marker[size_t(g.unit)] == u || g.zmax <= unit_floor ||
                        (only != nullptr && ! (*only)[size_t(g.unit)]))
                        continue;
                    bool covered = false;
                    for (const NonplanarClearanceModel::Box &b : boxes_by_bottom) {
                        if (g.zmax <= s0.z + b.bottom - model.safety_mm + model.contact_tolerance)
                            break;
                        if (cx >= std::floor((s0.x + b.x_min - 0.5) / coarse_cell) && cx <= std::floor((s0.x + b.x_max + 0.5) / coarse_cell) &&
                            cy >= std::floor((s0.y + b.y_min - 0.5) / coarse_cell) && cy <= std::floor((s0.y + b.y_max + 0.5) / coarse_cell)) {
                            covered = true;
                            break;
                        }
                    }
                    if (! covered) { complete = false; continue; }
                    for (int k = g.begin; k < g.end && all[size_t(cell_samples[size_t(k)])].z > s0.z + block_dz_min; ++ k)
                        if (cell_samples[size_t(k)] != t && blocks(all[size_t(cell_samples[size_t(k)])], s0)) {
                            marker[size_t(g.unit)] = u;
                            out.push_back(g.unit);
                            break;
                        }
                    complete &= marker[size_t(g.unit)] == u;
                }
                if (complete && !verify_dependents) marker[stamp] = u;
            }
    };
    auto visit_unit_followers = [&](int t, int unit, auto &&extra, auto &&found) {
        const Sample &s0 = all[size_t(t)];
        BoundingBoxf3 &bounds = unit_bounds[size_t(unit)];
        if (!bounds.defined)
            for (int m : members[size_t(unit)])
                bounds.merge(Vec3d(all[size_t(m)].x, all[size_t(m)].y, all[size_t(m)].z));
        const double dx = std::max({bounds.min.x() - s0.x, s0.x - bounds.max.x(), 0.});
        const double dy = std::max({bounds.min.y() - s0.y, s0.y - bounds.max.y(), 0.});
        const double dz = bounds.max.z() - s0.z;
        const double grow = max_half_width + model.bead_margin + model.safety_mm + model.sample_step;
        bool possible = dz > 0.1 && bounds.min.z() - s0.z < 0.35 && dx * dx + dy * dy < sqr(max_half_width + 1e-6);
        for (const auto &ring : m_model.ring_list)
            possible |= dz > ring.bottom - model.safety_mm + model.contact_tolerance && dx * dx + dy * dy < sqr(ring.radius + grow + 1e-6);
        for (const auto &box : model.boxes)
            possible |= dz > box.bottom - model.safety_mm + model.contact_tolerance &&
                bounds.max.x() > s0.x + box.x_min - grow - 1e-6 && bounds.min.x() < s0.x + box.x_max + grow + 1e-6 &&
                bounds.max.y() > s0.y + box.y_min - grow - 1e-6 && bounds.min.y() < s0.y + box.y_max + grow + 1e-6;
        if (!possible) return;
        auto check = [&](int m) {
            if (m != t && unit_of[size_t(m)] == unit && extra(m) && blocks(all[size_t(m)], s0))
                found(m);
        };
        auto visit_group = [&](const std::vector<CellGroup> &groups, const std::vector<int> &samples,
                               double z_lo, double z_hi, auto &&f) {
            const auto g = std::lower_bound(groups.begin(), groups.end(), unit,
                [](const CellGroup &a, int u) { return a.unit < u; });
            if (g == groups.end() || g->unit != unit || g->zmax <= z_lo) return;
            for (int k = g->begin; k < g->end; ++k) {
                const int m = samples[size_t(k)];
                if (all[size_t(m)].z <= z_lo) break;
                if (all[size_t(m)].z < z_hi) f(m);
            }
        };
        const double r = reach(t);
        if (r > 0.)
            grid.visit_cells(s0.x, s0.y, r, [&](size_t cell) {
                visit_group(fine_groups[cell], fine_samples, s0.z + block_dz_min, std::numeric_limits<double>::max(), check);
            });
        if (! model.boxes.empty()) {
            double lowest_floor = std::numeric_limits<double>::max();
            for (const NonplanarClearanceModel::Box &b : model.boxes)
                lowest_floor = std::min(lowest_floor, s0.z + b.bottom - model.safety_mm + model.contact_tolerance);
            if (zmax > lowest_floor)
                for (double cx = std::max(std::floor((s0.x + box_x_min - 0.5) / coarse_cell), double(coarse.x0()));
                     cx <= std::min(std::floor((s0.x + box_x_max + 0.5) / coarse_cell), double(coarse.x1())); cx += 1.)
                    for (double cy = std::max(std::floor((s0.y + box_y_min - 0.5) / coarse_cell), double(coarse.y0()));
                         cy <= std::min(std::floor((s0.y + box_y_max + 0.5) / coarse_cell), double(coarse.y1())); cy += 1.) {
                        const int k = coarse_key((cx + 0.5) * coarse_cell, (cy + 0.5) * coarse_cell);
                        if (k < 0 || ! coarse_used[size_t(k)] || coarse_zmax[size_t(k)] <= lowest_floor)
                            continue;
                        const double cell_zmax = coarse_zmax[size_t(k)];
                        bool covered = false;
                        for (const NonplanarClearanceModel::Box &b : boxes_by_bottom) {
                            if (cell_zmax <= s0.z + b.bottom - model.safety_mm + model.contact_tolerance)
                                break;
                            if (cx >= std::floor((s0.x + b.x_min - 0.5) / coarse_cell) && cx <= std::floor((s0.x + b.x_max + 0.5) / coarse_cell) &&
                                cy >= std::floor((s0.y + b.y_min - 0.5) / coarse_cell) && cy <= std::floor((s0.y + b.y_max + 0.5) / coarse_cell)) {
                                covered = true;
                                break;
                            }
                        }
                        if (! covered)
                            continue;
                        visit_group(cell_groups[size_t(k)], cell_samples, s0.z + block_dz_min,
                                    std::numeric_limits<double>::max(), check);
                    }
        }
        grid.visit_cells(s0.x, s0.y, 2. * s0.half_width, [&](size_t cell) {
            visit_group(fine_groups[cell], fine_samples, s0.z + 0.1 - 1e-6, s0.z + 0.35 + 1e-6, [&](int m) {
                const Sample &s1 = all[size_t(m)];
                if (m != t && extra(m) && s1.z - s0.z > 0.1 && s1.z - s0.z < 0.35 &&
                    std::hypot(s1.x - s0.x, s1.y - s0.y) < 0.5 * (s1.half_width + s0.half_width))
                    found(m);
            });
        });
    };
    std::vector<int> unit_slot, unit_base;
    std::vector<std::vector<int>> dependents;
    std::vector<char> dependents_known;
    std::vector<std::string> conflicts;
    int conflict_a = -1, conflict_b = -1;
    using Slot = std::pair<int, int>;
    const Slot before_all {-1, -1}, after_all {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
    std::vector<Slot> raw_limit, old_limit;
    bool reset_raw = false;
    std::vector<std::vector<int>> raw, old_raw, old_members;
    std::vector<std::vector<std::pair<int, int>>> unit_runs, old_unit_runs;
    std::vector<char> raw_known, pending, old_known, changed;
    std::vector<int> old_unit_of;
    size_t refined_units = 0;
    std::set<std::pair<int, int>> dropped;
    size_t dropped_edges = 0;
    bool rebuild_units = true;
    const bool wide_cap = curved_tops_mode(object.config().nonplanar_mode.value) &&
        object.config().nonplanar_curved_max_angle.value > 12. + EPSILON;
    constexpr size_t MaxWork = 2000000000;
    size_t work = 0;
    std::vector<std::pair<Slot, size_t>> eligible_by_slot;
    std::vector<char> eligible;
    std::vector<size_t> eligible_units;
    bool   out_of_work = false;
    for (int attempt = 0;; ++ attempt) {
        throw_if_canceled();
        if (rebuild_units || verify_dependents) {
            old_members = std::move(members);
            old_unit_runs = std::move(unit_runs);
            old_raw     = std::move(raw);
            old_known   = std::move(raw_known);
            old_limit = std::move(raw_limit);
            if (reset_raw) { std::fill(old_known.begin(), old_known.end(), 0); reset_raw = false; }
            for (size_t o = 0; o < old_known.size(); ++ o)
                if (pending[o])
                    old_known[o] = 0;
            old_unit_of = unit_of;
            std::unordered_map<std::tuple<int, int, long, int, int>, int, UnitKeyHash> shells;
            std::unordered_map<const void*, int> entities;
            std::unordered_map<std::tuple<long, int, long, int>, int, UnitKeyHash> chunks;
            long edge_run = 0;
            int next = 0;
            std::vector<std::array<int, 3>> runs;
            auto add_run = [&runs](int u, int begin, int end) {
                if (!runs.empty() && runs.back()[0] == u && runs.back()[2] == begin)
                    runs.back()[2] = end;
                else
                    runs.push_back({u, begin, end});
            };
            for (const auto &[first, last] : spans) {
                const Sampled &src = sampled[size_t(all[first].path)];
                if (!path_has_edge[size_t(all[first].path)]) {
                    const int u = src.path->nonplanar_source
                        ? shells.try_emplace(std::make_tuple(src.layer, src.path->source_surface_id,
                            std::lround(src.path->distance_to_top * 1000.f), earliest[first], height_band[first]), next).first->second
                        : entities.try_emplace(src.entity, next).first->second;
                    std::fill(unit_of.begin() + first, unit_of.begin() + last, u);
                    add_run(u, int(first), int(last));
                    if (u == next) ++next;
                    continue;
                }
                for (size_t i = first; i < last; ++i) {
                    const Sampled &src = sampled[size_t(all[i].path)];
                    if (edge_level[i] > 0 && ! (i > 0 && all[i - 1].path == all[i].path && edge_level[i - 1] > 0))
                        ++ edge_run;
                    if (i > 0 && edge_level[i] == 0 && edge_level[i - 1] == 0 &&
                        all[i].path == all[i - 1].path && earliest[i] == earliest[i - 1] && height_band[i] == height_band[i - 1]) {
                        unit_of[i] = unit_of[i - 1];
                        add_run(unit_of[i], int(i), int(i) + 1);
                        continue;
                    }
                    if (edge_level[i] > 0) {
                        const auto key = std::make_tuple(edge_run, edge_level[i], long(all[i].s / chunk_length(edge_level[i])), height_band[i]);
                        unit_of[i] = chunks.try_emplace(key, next).first->second;
                    } else if (src.path->nonplanar_source) {
                        const auto key = std::make_tuple(src.layer, src.path->source_surface_id,
                            std::lround(src.path->distance_to_top * 1000.f), earliest[i], height_band[i]);
                        unit_of[i] = shells.try_emplace(key, next).first->second;
                    } else {
                        unit_of[i] = entities.try_emplace(src.entity, next).first->second;
                    }
                    add_run(unit_of[i], int(i), int(i) + 1);
                    if (unit_of[i] == next)
                        ++ next;
                }
            }
            members.assign(size_t(next), {});
            unit_slot.assign(size_t(next), 0);
            const auto old_zmin = std::move(unit_zmin);
            const auto old_zmax = std::move(unit_zmax);
            unit_zmin.assign(size_t(next), std::numeric_limits<double>::max());
            unit_zmax.assign(size_t(next), -std::numeric_limits<double>::max());
            unit_base.assign(size_t(next), 0);
            changed.assign(size_t(next), 1);
            size_t covered = 0;
            for (const auto &r : runs) covered += size_t(r[2] - r[1]);
            if (covered != unit_of.size() || (!runs.empty() && runs.back()[2] != int(unit_of.size()))) {
                runs.clear();
                for (size_t i = 0; i < unit_of.size(); ++i) add_run(unit_of[i], int(i), int(i) + 1);
            }
            std::vector<size_t> counts(size_t(next), 0), first(size_t(next), 0);
            unit_runs.assign(size_t(next), {});
            for (const auto &[u, begin, end] : runs) {
                if (counts[size_t(u)] == 0) first[size_t(u)] = size_t(begin);
                counts[size_t(u)] += size_t(end - begin);
                unit_runs[size_t(u)].emplace_back(begin, end);
            }
            std::vector<int> new_of(old_members.size(), -1);
            for (int u = 0; u < next; ++u) {
                const int o = old_unit_of[first[size_t(u)]];
                if (attempt > 0 && counts[size_t(u)] == old_members[size_t(o)].size() && unit_runs[size_t(u)] == old_unit_runs[size_t(o)]) {
                    members[size_t(u)] = std::move(old_members[size_t(o)]);
                    unit_zmin[size_t(u)] = old_zmin[size_t(o)];
                    unit_zmax[size_t(u)] = old_zmax[size_t(o)];
                    new_of[size_t(o)] = u;
                    changed[size_t(u)] = 0;
                } else
                    members[size_t(u)].reserve(counts[size_t(u)]);
            }
            for (const auto &[u, begin, end] : runs)
                if (changed[size_t(u)])
                    for (int i = begin; i < end; ++i) members[size_t(u)].push_back(i);
            for (size_t u = 0; u < members.size(); ++u) {
                const size_t last = size_t(members[u].back());
                unit_base[u] = base[last];
                unit_slot[u] = earliest[last];
            }
            dependents.assign(size_t(next), {});
            dependents_known.assign(size_t(next), 0);
            collected_for.assign(size_t(next) + fine_groups.size() + cell_groups.size(), -1);
            raw.assign(size_t(next), {});
            raw_known.assign(size_t(next), 0);
            raw_limit.assign(size_t(next), before_all);
            pending.assign(size_t(next), 0);
            unit_bounds.assign(size_t(next), {});
            for (int u = 0; u < next; ++u)
                if (changed[size_t(u)])
                    for (int t : members[size_t(u)]) {
                        unit_zmin[size_t(u)] = std::min(unit_zmin[size_t(u)], all[size_t(t)].z);
                        unit_zmax[size_t(u)] = std::max(unit_zmax[size_t(u)], all[size_t(t)].z);
                    }
            auto group_cells = [&](const Grid &index, std::vector<std::vector<CellGroup>> &cells, std::vector<int> &samples) {
                auto less = [&](int a, int b) {
                    if (unit_of[size_t(a)] != unit_of[size_t(b)]) return unit_of[size_t(a)] < unit_of[size_t(b)];
                    return all[size_t(a)].z != all[size_t(b)].z ? all[size_t(a)].z > all[size_t(b)].z : a < b;
                };
                if (samples.empty()) {
                    std::vector<std::vector<int>> by_cell(index.cell_count());
                    for (size_t i = 0; i < all.size(); ++i)
                        by_cell[size_t(index.cell_slot(index.index(all[i].x), index.index(all[i].y)))].push_back(int(i));
                    samples.reserve(all.size());
                    for (size_t k = 0; k < cells.size(); ++k) {
                        const int begin = int(samples.size());
                        samples.insert(samples.end(), by_cell[k].begin(), by_cell[k].end());
                        if (begin < int(samples.size()))
                            cells[k].push_back({-1, 0., begin, int(samples.size())});
                    }
                }
                for (auto &groups : cells) {
                    if (groups.empty()) continue;
                    bool rebuild = false;
                    int begin = int(samples.size()), end = 0;
                    for (CellGroup &g : groups) {
                        begin = std::min(begin, g.begin);
                        end = std::max(end, g.end);
                        if (g.unit < 0 || size_t(g.unit) >= new_of.size() || new_of[size_t(g.unit)] < 0)
                            rebuild = true;
                        else
                            g.unit = new_of[size_t(g.unit)];
                    }
                    if (!rebuild) {
                        auto by_unit = [](const CellGroup &a, const CellGroup &b) { return a.unit < b.unit; };
                        if (!std::is_sorted(groups.begin(), groups.end(), by_unit))
                            std::sort(groups.begin(), groups.end(), by_unit);
                        continue;
                    }
                    std::sort(samples.begin() + begin, samples.begin() + end, less);
                    groups.clear();
                    for (int k = begin; k < end; ++k) {
                        const int m = samples[size_t(k)], u = unit_of[size_t(m)];
                        if (!groups.empty() && groups.back().unit == u)
                            groups.back().end = k + 1;
                        else
                            groups.push_back({u, all[size_t(m)].z, k, k + 1});
                    }
                }
            };
            group_cells(coarse, cell_groups, cell_samples);
            group_cells(grid, fine_groups, fine_samples);
            for (int u = 0; u < next; ++ u) {
                if (changed[size_t(u)])
                    continue;
                const int o = old_unit_of[size_t(members[size_t(u)].front())];
                if (! old_known[size_t(o)])
                    continue;
                for (int v : old_raw[size_t(o)]) {
                    if (new_of[size_t(v)] >= 0)
                        raw[size_t(u)].push_back(new_of[size_t(v)]);
                    else
                        pending[size_t(u)] = 1;
                }
                std::sort(raw[size_t(u)].begin(), raw[size_t(u)].end());
                raw_known[size_t(u)] = 1;
                raw_limit[size_t(u)] = old_limit[size_t(o)];
                pending[size_t(u)] = 1;
            }
            eligible_by_slot.clear();
            eligible_by_slot.reserve(members.size());
            eligible.assign(members.size(), 0);
            eligible_units.clear();
            work += members.size();
            for (size_t v = 0; v < members.size(); ++v) {
                ++work;
                eligible_by_slot.emplace_back(Slot{earliest[size_t(members[v].back())], unit_base[v]}, v);
            }
            std::sort(eligible_by_slot.begin(), eligible_by_slot.end(), [&](const auto &a, const auto &b) {
                ++work;
                return a < b;
            });
        } else {
            for (size_t u = 0; u < members.size(); ++u) {
                unit_slot[u] = earliest[size_t(members[u].back())];
                dependents[u].clear();
            }
            std::fill(dependents_known.begin(), dependents_known.end(), 0);
            std::fill(collected_for.begin(), collected_for.end(), -1);
        }
        rebuild_units = false;
        auto collect_samples = [&](int u, const std::vector<char> *only, double material_top, const int *first, const int *last, std::vector<int> &out, std::vector<int> &marker) {
            for (const int *t = first; t != last; ++ t) {
                if (verify_dependents) {
                    visit_followers(*t,
                        [&](int m) { const int v = unit_of[size_t(m)]; return v != u && marker[size_t(v)] != u && (only == nullptr || (*only)[size_t(v)]); },
                        [&](int m) { const int v = unit_of[size_t(m)]; if (marker[size_t(v)] != u) { marker[size_t(v)] = u; out.push_back(v); } },
                        false);
                } else {
                    const Sample &s0 = all[size_t(*t)];
                    auto collect = [&](double radius, double z_lo, double z_hi, bool stacked) {
                        if (radius <= 0.) return;
                        grid.visit_cells(s0.x, s0.y, radius, [&](size_t cell) {
                            const size_t stamp = members.size() + cell;
                            if (marker[stamp] == u) return;
                            bool complete = true;
                            const double floor = unit_zmin[size_t(u)] + std::min(block_dz_min, 0.1 - 1e-6);
                            for (const CellGroup &g : fine_groups[cell]) {
                                if (g.unit == u || marker[size_t(g.unit)] == u || g.zmax <= floor ||
                                    (only != nullptr && !(*only)[size_t(g.unit)]))
                                    continue;
                                for (int k = g.begin; k < g.end; ++k) {
                                    const int m = fine_samples[size_t(k)];
                                    const Sample &s1 = all[size_t(m)];
                                    if (s1.z <= z_lo) break;
                                    if (s1.z >= z_hi) continue;
                                    const bool follows = stacked
                                        ? s1.z - s0.z > 0.1 && s1.z - s0.z < 0.35 &&
                                          std::hypot(s1.x - s0.x, s1.y - s0.y) < 0.5 * (s1.half_width + s0.half_width)
                                        : blocks(s1, s0);
                                    if (follows) {
                                        marker[size_t(g.unit)] = u;
                                        out.push_back(g.unit);
                                        break;
                                    }
                                }
                                complete &= marker[size_t(g.unit)] == u;
                            }
                            if (complete) marker[stamp] = u;
                        });
                    };
                    collect(reach(*t, material_top), s0.z + block_dz_min, std::numeric_limits<double>::max(), false);
                    collect(2. * s0.half_width, s0.z + 0.1 - 1e-6, s0.z + 0.35 + 1e-6, true);
                }
                collect_box_dependents(*t, u, out, only, material_top, marker);
            }
        };
        tbb::enumerable_thread_specific<std::pair<std::vector<int>, std::vector<int>>> workers(
            [&]() { return std::make_pair(std::vector<int>(collected_for.size(), -1), std::vector<int>()); });
        auto collect_dependents = [&](int u, bool only_changed, Slot from, Slot to, std::vector<int> &out) {
            work += eligible_units.size() + members[size_t(u)].size();
            for (size_t v : eligible_units) eligible[v] = 0;
            eligible_units.clear();
            double material_top = -std::numeric_limits<double>::max();
            if (from < to) {
                auto upper = [&](Slot slot) {
                    return std::upper_bound(eligible_by_slot.begin(), eligible_by_slot.end(),
                        std::make_pair(slot, std::numeric_limits<size_t>::max()), [&](const auto &key, const auto &entry) {
                            ++work;
                            return key < entry;
                        });
                };
                const auto last = upper(to);
                for (auto it = upper(from); it != last; ++it) {
                    ++work;
                    const size_t v = it->second;
                    if (int(v) != u && (!only_changed || changed[v])) {
                        eligible[v] = 1;
                        eligible_units.push_back(v);
                        material_top = std::max(material_top, unit_zmax[v]);
                    }
                }
            }
            if (verify_dependents) {
                std::vector<size_t> exhaustive;
                for (size_t v = 0; v < members.size(); ++v) {
                    const Slot key {earliest[size_t(members[v].back())], unit_base[v]};
                    if (int(v) != u && key > from && key <= to && (!only_changed || changed[v]))
                        exhaustive.push_back(v);
                }
                auto indexed = eligible_units;
                std::sort(indexed.begin(), indexed.end());
                if (indexed != exhaustive)
                    throw Slic3r::RuntimeError("Nonplanar deposition order: indexed eligibility differs from a full scan");
            }
            if (material_top == -std::numeric_limits<double>::max()) return;
            for (int v : out) collected_for[size_t(v)] = u;
            std::fill(collected_for.begin() + members.size(), collected_for.end(), -1);
            std::vector<int> affected;
            if (only_changed && !verify_dependents && members[size_t(u)].size() >= 256) {
                auto query = [&](const Vec2d &lo, const Vec2d &hi, double top) {
                    if (top < unit_zmin[size_t(u)]) return;
                    const Vec2d center = 0.5 * (lo + hi);
                    const double radius = 0.5 * (hi - lo).maxCoeff() + 1e-6;
                    grid.visit_z(center.x(), center.y(), radius, unit_zmin[size_t(u)] - 1e-6, top + 1e-6, z_of, [&](int t) {
                        const Sample &q = all[size_t(t)];
                        if (unit_of[size_t(t)] == u && q.x >= lo.x() - 1e-6 && q.x <= hi.x() + 1e-6 &&
                            q.y >= lo.y() - 1e-6 && q.y <= hi.y() + 1e-6)
                            affected.push_back(t);
                    });
                };
                const double grow = max_half_width + model.bead_margin + model.safety_mm + model.sample_step;
                work += eligible_units.size();
                for (size_t v : eligible_units) {
                    if (collected_for[v] == u) continue;
                    BoundingBoxf3 &box = unit_bounds[v];
                    if (!box.defined)
                        for (int m : members[v]) box.merge(Vec3d(all[size_t(m)].x, all[size_t(m)].y, all[size_t(m)].z));
                    for (const auto &ring : m_model.ring_list) {
                        const Vec2d pad = Vec2d::Constant(ring.radius + grow);
                        query(box.min.head<2>() - pad, box.max.head<2>() + pad, box.max.z() - ring.bottom + model.safety_mm);
                    }
                    for (const auto &body : model.boxes)
                        query(box.min.head<2>() - Vec2d(body.x_max + grow, body.y_max + grow),
                              box.max.head<2>() - Vec2d(body.x_min - grow, body.y_min - grow), box.max.z() - body.bottom + model.safety_mm);
                    const Vec2d pad = Vec2d::Constant(max_half_width);
                    query(box.min.head<2>() - pad, box.max.head<2>() + pad, box.max.z() - 0.1);
                }
                std::sort(affected.begin(), affected.end());
                affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
            }
            const std::vector<int> &samples = only_changed && !verify_dependents && members[size_t(u)].size() >= 256
                ? affected : members[size_t(u)];
            if (samples.size() < 256 || verify_dependents) {
                collect_samples(u, &eligible, material_top, samples.data(), samples.data() + samples.size(), out, collected_for);
                return;
            }
            for (auto &w : workers) {
                w.second.clear();
                std::fill(w.first.begin() + members.size(), w.first.end(), -1);
            }
            tbb::parallel_for(tbb::blocked_range<size_t>(0, samples.size(), 64), [&](const tbb::blocked_range<size_t> &range) {
                auto &[marker, found] = workers.local();
                collect_samples(u, &eligible, material_top, samples.data() + range.begin(), samples.data() + range.end(), found, marker);
            });
            for (auto &w : workers)
                for (int v : w.second)
                    if (collected_for[size_t(v)] != u) {
                        collected_for[size_t(v)] = u;
                        out.push_back(v);
                    }
        };
        auto dependents_of = [&](int u) -> const std::vector<int>& {
            const Slot needed = verify_dependents ? after_all : Slot {unit_slot[size_t(u)], unit_base[size_t(u)]};
            if (!dependents_known[size_t(u)] || needed > raw_limit[size_t(u)]) {
                std::vector<int> &list = raw[size_t(u)];
                const bool carried = raw_known[size_t(u)];
                if (!carried) {
                    collect_dependents(u, false, before_all, needed, list);
                    raw_known[size_t(u)] = 1;
                    raw_limit[size_t(u)] = needed;
                } else {
                    if (pending[size_t(u)]) {
                        collect_dependents(u, true, before_all, raw_limit[size_t(u)], list);
                        pending[size_t(u)] = 0;
                    }
                    if (needed > raw_limit[size_t(u)]) {
                        collect_dependents(u, false, raw_limit[size_t(u)], needed, list);
                        raw_limit[size_t(u)] = needed;
                    }
                }
                std::sort(list.begin(), list.end());
                if (carried && verify_dependents) {
                    std::vector<int> saved = collected_for, fresh;
                    std::fill(collected_for.begin(), collected_for.end(), -1);
                    collect_dependents(u, false, before_all, after_all, fresh);
                    std::sort(fresh.begin(), fresh.end());
                    collected_for = std::move(saved);
                    if (fresh != list)
                        throw Slic3r::RuntimeError("Nonplanar deposition order: carried dependents differ from a fresh computation (attempt " +
                                                   std::to_string(attempt + 1) + ", unit " + std::to_string(u) + ")");
                }
                dependents[size_t(u)].clear();
                for (int v : list)
                    if (!dropped.count({members[size_t(u)].front(), members[size_t(v)].front()}))
                        dependents[size_t(u)].push_back(v);
                dependents_known[size_t(u)] = 1;
            }
            return dependents[size_t(u)];
        };
        auto unit_after = [&](int a, int b) {
            return unit_slot[size_t(a)] > unit_slot[size_t(b)] ||
                (unit_slot[size_t(a)] == unit_slot[size_t(b)] && unit_base[size_t(a)] > unit_base[size_t(b)]);
        };
        auto slot_after = [&](int u, int v) {
            const int su = unit_slot[size_t(u)];
            if (su % 2 == 0)
                return su + 1;
            return unit_base[size_t(v)] > unit_base[size_t(u)] ? su : su + 2;
        };

        if (attempt == 0) {
            auto rests_on = [](const Sample &upper, const Sample &lower) {
                return upper.z - lower.z > 0.1 && upper.z - lower.z < 0.35 &&
                    std::hypot(upper.x - lower.x, upper.y - lower.y) < 0.5 * (upper.half_width + lower.half_width);
            };
            const double below_margin = std::min(block_dz_min, 0.1 - 1e-6);
            std::vector<int> tried(all.size(), -1);
            for (size_t T = 0; T < members.size() && conflict_a < 0; ++ T) {
                const int t0 = members[T].front();
                if (! i_is_conformal(t0))
                    continue;
                const ExtrusionPath &tp = *sampled[size_t(all[size_t(t0)].path)].path;
                if (! (tp.nonplanar_source && tp.distance_to_top < 0.001f))
                    continue;
                double top_zmax = -std::numeric_limits<double>::max();
                for (int t : members[T])
                    top_zmax = std::max(top_zmax, all[size_t(t)].z);
                const double p_zmax = top_zmax - below_margin + 1e-6;
                for (int t : members[T]) {
                    if (conflict_a >= 0)
                        break;
                    throw_if_canceled();
                    const Sample &st = all[size_t(t)];
                    auto candidate = [&](int p) {
                        if (conflict_a >= 0 || i_is_conformal(p) || tried[size_t(p)] == int(T))
                            return;
                        tried[size_t(p)] = int(T);
                        const int v = unit_of[size_t(p)];
                        if (unit_after(v, int(T)))
                            return;
                        const Sample &sp = all[size_t(p)];
                        bool back = false;
                        const double rp = reach(p);
                        if (rp > 0)
                            grid.visit_z(sp.x, sp.y, rp, sp.z + block_dz_min, std::numeric_limits<double>::max(), z_of, [&](int m) {
                                back = back || (m != p && unit_of[size_t(m)] == int(T) && blocks(all[size_t(m)], sp, true));
                            });
                        if (! back)
                            grid.visit_z(sp.x, sp.y, 2. * sp.half_width, sp.z + 0.1 - 1e-6, sp.z + 0.35 + 1e-6, z_of, [&](int m) {
                                back = back || (m != p && unit_of[size_t(m)] == int(T) && rests_on(all[size_t(m)], sp));
                            });
                        if (back) {
                            conflict_a = t;
                            conflict_b = p;
                        }
                    };
                    const double r = reach(t);
                    if (r > 0)
                        grid.visit_z(st.x, st.y, r, st.z + block_dz_min, p_zmax, z_of, [&](int p) {
                            if (p != t && tried[size_t(p)] != int(T) && ! i_is_conformal(p) && blocks(all[size_t(p)], st, true))
                                candidate(p);
                        });
                    grid.visit_z(st.x, st.y, 2. * st.half_width, st.z + 0.1 - 1e-6, std::min(st.z + 0.35 + 1e-6, p_zmax), z_of, [&](int p) {
                        if (p != t && tried[size_t(p)] != int(T) && ! i_is_conformal(p) && rests_on(all[size_t(p)], st))
                            candidate(p);
                    });
                }
            }
            if (conflict_a >= 0) {
                conflicts.emplace_back(describe(all[size_t(conflict_b)], "planar material") + " and " +
                    describe(all[size_t(conflict_a)], "top shell") + " strictly obstruct each other (early check)");
                break;
            }
        }

        std::vector<int> parent(members.size(), -1);
        std::vector<int> worklist;
        std::vector<int> cycle;
        for (size_t u = 0; u < members.size(); ++ u)
            if (i_is_conformal(members[u].front()))
                worklist.push_back(int(u));
        size_t visited = 0;
        while (! worklist.empty() && cycle.empty()) {
            if ((++ visited & 255) == 0)
                throw_if_canceled();
            const int u = worklist.back();
            worklist.pop_back();
            for (int v : dependents_of(u)) {
                if (unit_after(v, u))
                    continue;
                for (int w = u, steps = 0; w != -1 && steps < 100000; w = parent[size_t(w)], ++ steps)
                    if (w == v) {
                        for (int c = u; c != v; c = parent[size_t(c)])
                            cycle.push_back(c);
                        cycle.push_back(v);
                        break;
                    }
                if (! cycle.empty())
                    break;
                unit_slot[size_t(v)] = slot_after(u, v);
                parent[size_t(v)] = u;
                worklist.push_back(v);
            }
        }
        work += visited;
        if (cycle.empty())
            break;
        if (! banded) {
            bool lower_shell = false;
            for (int c : cycle) {
                const ExtrusionPath &path = *sampled[size_t(all[size_t(members[size_t(c)].front())].path)].path;
                lower_shell |= path.nonplanar_source && path.distance_to_top > 0.001f;
            }
            all_height_bands = threshold >= 0.1;
            if (lower_shell || all_height_bands) {
                if (all_height_bands)
                    for (size_t i = first_conformal; i < all.size(); ++i)
                        height_band[i] = int(std::floor(all[i].z / height_band_width));
                banded = true;
                reset_raw = true;
                rebuild_units = true;
                compute_earliest();
                make_spans();
                std::fill(path_has_edge.begin(), path_has_edge.end(), 0);
                std::fill(edge_level.begin(), edge_level.end(), 0);
                dropped.clear();
                refined_units = 0;
                dropped_edges = 0;
                BOOST_LOG_TRIVIAL(info) << "Nonplanar deposition order: a cycle requires height bands for "
                    << (all_height_bands ? "all curved shells" : "lower shells");
                continue;
            }
        }
        bool progress = false;
        out_of_work = work > MaxWork;
        const bool exhausted = out_of_work;
        for (size_t k = 0; k < cycle.size() && ! exhausted; ++ k) {
            throw_if_canceled();
            const int c = cycle[k];
            const int i0 = members[size_t(c)].front();
            const Sampled &src = sampled[size_t(all[size_t(i0)].path)];
            if (src.path->nonplanar_source && src.path->distance_to_top < 0.001f)
                continue;
            if (members[size_t(c)].size() <= 1)
                continue;
            bool all_edge = true;
            for (int t : members[size_t(c)])
                all_edge &= edge_level[size_t(t)] > 0;
            if (all_edge) {
                if (edge_level[size_t(i0)] < max_level) {
                    for (int t : members[size_t(c)])
                        ++ edge_level[size_t(t)];
                    progress = true;
                    ++ refined_units;
                    rebuild_units = true;
                }
                continue;
            }
            std::set<int> involved;
            const int leader   = cycle[(k + 1) % cycle.size()];
            const int follower = cycle[(k + cycle.size() - 1) % cycle.size()];
            std::set<int> entering, leaving;
            for (int t : members[size_t(leader)])
                visit_unit_followers(t, c, [](int) { return true; },
                                     [&](int m) { entering.insert(m); });
            for (int t : members[size_t(c)])
                visit_unit_followers(t, follower, [&](int) { return ! leaving.count(t); },
                                     [&](int) { leaving.insert(t); });
            if (entering.empty() || (! leaving.empty() && leaving.size() < entering.size()))
                involved = leaving;
            else
                involved = entering;
            bool marked = false;
            for (int t : members[size_t(c)]) {
                if (edge_level[size_t(t)] > 0)
                    continue;
                for (int m : involved)
                    if (all[size_t(m)].path == all[size_t(t)].path && std::abs(all[size_t(m)].s - all[size_t(t)].s) < scaled<double>(edge_band)) {
                        edge_level[size_t(t)] = 1;
                        path_has_edge[size_t(all[size_t(t)].path)] = 1;
                        marked = true;
                        break;
                    }
            }
            if (marked) {
                progress = true;
                ++ refined_units;
                rebuild_units = true;
            }
        }
        if (! progress && ! exhausted) {
            auto holds_strictly_needed = [&](int u, int v) {
                for (int t : members[size_t(u)])
                    for (int m : members[size_t(v)]) {
                        const Sample &st = all[size_t(t)], &sm = all[size_t(m)];
                        if (blocks(sm, st, true))
                            return true;
                        if (sm.z - st.z > 0.1 && sm.z - st.z < 0.35 &&
                            std::hypot(sm.x - st.x, sm.y - st.y) < 0.5 * (sm.half_width + st.half_width))
                            return true;
                    }
                return false;
            };
            const size_t n = cycle.size();
            for (size_t k = 0; k < n; ++ k) {
                const int follower = cycle[k], leader = cycle[(k + 1) % n];
                if (! holds_strictly_needed(leader, follower) &&
                    dropped.insert({ members[size_t(leader)].front(), members[size_t(follower)].front() }).second) {
                    progress = true;
                    ++ dropped_edges;
                }
            }
        }
        if (! progress) {
            const size_t n = cycle.size();
            for (size_t k = 0; k < n && conflicts.size() < 8; ++ k) {
                const int follower = cycle[k], leader = cycle[(k + 1) % n];
                for (int t : members[size_t(leader)]) {
                    bool found = false;
                    for (int m : members[size_t(follower)]) {
                        const Sample &st = all[size_t(t)], &sm = all[size_t(m)];
                        const bool access = blocks(sm, st, true);
                        const bool rests  = sm.z - st.z > 0.1 && sm.z - st.z < 0.35 &&
                            std::hypot(sm.x - st.x, sm.y - st.y) < 0.5 * (sm.half_width + st.half_width);
                        if (access || rests) {
                            if (conflict_a < 0) {
                                conflict_a = t;
                                conflict_b = m;
                            }
                            conflicts.emplace_back(describe(sm, i_is_conformal(m) ? "conformal shell" : "planar material") +
                                (access ? " obstructs the tool at " : " rests on ") +
                                describe(st, i_is_conformal(t) ? "conformal shell" : "planar material"));
                            found = true;
                            break;
                        }
                    }
                    if (found)
                        break;
                }
            }
        }
        if (! progress) {
            if (conflict_a < 0 && cycle.size() >= 2) {
                conflict_a = members[size_t(cycle[0])].front();
                conflict_b = members[size_t(cycle[1])].front();
            }
            if (conflicts.empty())
            for (int c : cycle)
                if (conflicts.size() < 8)
                    conflicts.emplace_back(describe(all[size_t(members[size_t(c)].front())],
                        i_is_conformal(members[size_t(c)].front()) ? "conformal shell" : "planar material"));
            break;
        }
        BOOST_LOG_TRIVIAL(info) << "Nonplanar deposition order: cycle of " << cycle.size()
            << " units resolved by refining wall loops or dropping marginal access edges (attempt " << attempt + 1 << ")";
    }
    for (size_t i = 0; i < all.size(); ++ i)
        all[i].slot = unit_slot[size_t(unit_of[i])];

    if (const char *debug = research_switch("NONPLANAR_PLAN_DEBUG")) {
        double dx = 0, dy = 0, dr = 0;
        if (std::sscanf(debug, "%lf,%lf,%lf", &dx, &dy, &dr) == 3) {
            for (size_t i = 0; i < all.size(); ++ i)
                if (std::hypot(all[i].x - dx, all[i].y - dy) <= dr)
                    BOOST_LOG_TRIVIAL(error) << "plan sample " << i << (i >= first_conformal ? " conformal" : " planar")
                        << " x=" << all[i].x << " y=" << all[i].y << " z=" << all[i].z << " layer=" << all[i].layer
                        << " base=" << base[i] << " slot=" << all[i].slot << " unit=" << unit_of[i] << " edge=" << edge_level[i]
                        << " path=" << all[i].path_id;
        }
    }

    if (! conflicts.empty()) {
        std::string details = "Nonplanar deposition order: these units must each precede and follow one another: ";
        for (size_t i = 0; i < conflicts.size(); ++ i)
            details += (i ? "; " : "") + conflicts[i];
        BOOST_LOG_TRIVIAL(error) << details;
        auto kind = [&](int i) -> std::string {
            const ExtrusionPath &path = *sampled[size_t(all[size_t(i)].path)].path;
            if (path.nonplanar_source)
                return path.distance_to_top < 0.001f ? "nonplanar top surface" : "nonplanar shell below the top surface";
            return is_perimeter(path.role()) ? "walls" : "infill";
        };
        const double angle = object.config().nonplanar_curved_max_angle.value;
        std::string message = "Nonplanar slicing failed";
        if (conflict_a >= 0) {
            const Sample &a = all[size_t(conflict_a)];
            const std::string ka = kind(conflict_a), kb = kind(conflict_b);
            message += nonplanar_location(a.z, size_t(std::max(0, a.layer))) + ": no print order avoids a hotend collision there. " +
                (ka == kb ? "Oops, print order conflict in " + ka + ": " + ka + " and " + kb : ka + " and " + kb) +
                " all need to go first so the print head can clear!";
        } else
            message += ": no print order avoids a hotend collision.";
        char hint[160];
        std::snprintf(hint, sizeof(hint), " Lower \"Maximum nonplanar angle\" to %g° or less and slice again.", nonplanar_suggested_angle(angle));
        message += hint;
        std::vector<int> surfaces;
        for (int sample : {conflict_a, conflict_b})
            if (sample >= 0) {
                const ExtrusionPath &path = *sampled[size_t(all[size_t(sample)].path)].path;
                if (path.nonplanar_source && path.source_surface_id >= 0 &&
                    std::find(surfaces.begin(), surfaces.end(), path.source_surface_id) == surfaces.end())
                    surfaces.push_back(path.source_surface_id);
            }
        if (out_of_work && ! wide_cap) {
            BOOST_LOG_TRIVIAL(warning) << object.model_object()->name << ": the deposition order search ran past its work budget; its curved tops print planar";
            for (const Sampled &s : sampled)
                if (s.path->nonplanar_source && s.path->source_surface_id >= 0 &&
                    std::find(surfaces.begin(), surfaces.end(), s.path->source_surface_id) == surfaces.end())
                    surfaces.push_back(s.path->source_surface_id);
        }
        if (!surfaces.empty() || wide_cap)
            throw CurvedTopsAccessError(message, object.id().id, std::move(surfaces));
        throw Slic3r::SlicingError(message, object.id().id);
    }

    for (size_t i = 0; i < all.size();) {
        size_t j = i;
        while (j < all.size() && all[j].path == all[i].path)
            ++ j;
        const int regular = 2 * all[i].layer;
        bool moved = false;
        for (size_t k = i; k < j; ++ k)
            moved |= all[k].slot != regular;
        if (moved) {
            bool earlier = false;
            for (size_t k = i; k < j; ++ k)
                earlier |= all[k].slot < regular;
            ++ (earlier ? result.advanced_paths : result.deferred_paths);
            result.deferred_samples += j - i;
            const Points pts = sampled[size_t(all[i].path)].path->polyline.to_polyline().points;
            NonplanarDeferral::PathPlan plan { pts.front(), pts[1], pts[pts.size() - 2], pts.back(), all[j - 1].s, {} };
            plan.breaks.emplace_back(0., all[i].slot);
            for (size_t k = i + 1; k < j; ++ k)
                if (all[k].slot != all[k - 1].slot)
                    plan.breaks.emplace_back(0.5 * (all[k - 1].s + all[k].s), all[k].slot);
            if (plan.breaks.size() > 1)
                ++ result.cut_paths;
            result.layers[size_t(all[i].layer)][all[i].path_id].push_back(std::move(plan));
        }
        i = j;
    }
    result.refined_units = refined_units;
    BOOST_LOG_TRIVIAL(info) << "Nonplanar deposition order: " << result.conformal_samples << " conformal and "
        << result.planar_samples << " planar samples; " << result.deferred_paths << " paths ("
        << result.deferred_samples << " samples) moved, " << result.cut_paths << " planar paths cut in "
        << result.refined_units << " refined wall units, " << dropped_edges << " marginal access edges dropped"
        << (all_height_bands ? ", all shells in height bands" : banded ? ", lower shells in height bands" : ", all shells whole");
    return result;
}

void NonplanarHeightMap::clear()
{
    m_fine.h.clear();
    m_mid.h.clear();
    m_coarse.h.clear();
    m_max_z = -1e9;
}

void NonplanarHeightMap::allocate()
{
    for (auto [grid, cell] : { std::make_pair(&m_fine, 0.1), std::make_pair(&m_mid, 0.5), std::make_pair(&m_coarse, 2.) }) {
        grid->cell = cell;
        grid->n    = int(std::lround(extent / cell));
        grid->h.assign(size_t(grid->n) * size_t(grid->n), -1e9f);
    }
}

bool NonplanarHeightMap::Grid::index(double x, double y, int &ix, int &iy) const
{
    ix = int(std::floor((x - origin) / cell));
    iy = int(std::floor((y - origin) / cell));
    return ix >= 0 && iy >= 0 && ix < n && iy < n;
}

static double distance_to_segment(const Vec2d &p, const Vec2d &a, const Vec2d &b)
{
    const Vec2d  d  = b - a;
    const double l2 = d.squaredNorm();
    const double t  = l2 > 0. ? std::clamp((p - a).dot(d) / l2, 0., 1.) : 0.;
    return (a + t * d - p).norm();
}

static std::vector<std::pair<Vec2d, Vec2d>> pieces(const std::vector<Vec2d> &path, double step)
{
    std::vector<std::pair<Vec2d, Vec2d>> out;
    for (size_t i = 1; i < path.size(); ++ i) {
        const Vec2d a = path[i - 1], b = path[i];
        const int   n = std::max(1, int(std::ceil((b - a).norm() / step)));
        for (int k = 0; k < n; ++ k)
            out.emplace_back(a + (b - a) * (double(k) / n), a + (b - a) * (double(k + 1) / n));
    }
    if (path.size() == 1)
        out.emplace_back(path.front(), path.front());
    return out;
}

void NonplanarHeightMap::deposit(const Vec2d &a, const Vec2d &b, double z, double half_width)
{
    if (m_fine.h.empty())
        this->allocate();
    m_max_z = std::max(m_max_z, z);
    const double reach = half_width + m_fine.cell * std::sqrt(0.5);
    for (const auto &[pa, pb] : pieces({ a, b }, 1.)) {
        int ix0, iy0, ix1, iy1;
        m_fine.index(std::min(pa.x(), pb.x()) - reach, std::min(pa.y(), pb.y()) - reach, ix0, iy0);
        m_fine.index(std::max(pa.x(), pb.x()) + reach, std::max(pa.y(), pb.y()) + reach, ix1, iy1);
        ix0 = std::max(ix0, 0); iy0 = std::max(iy0, 0);
        ix1 = std::min(ix1, m_fine.n - 1); iy1 = std::min(iy1, m_fine.n - 1);
        for (int iy = iy0; iy <= iy1; ++ iy)
            for (int ix = ix0; ix <= ix1; ++ ix) {
                const Vec2d c(origin + (ix + 0.5) * m_fine.cell, origin + (iy + 0.5) * m_fine.cell);
                if (distance_to_segment(c, pa, pb) > reach)
                    continue;
                float &fine = m_fine.h[size_t(iy) * size_t(m_fine.n) + size_t(ix)];
                if (fine >= float(z))
                    continue;
                fine = float(z);
                float &mid = m_mid.h[size_t(iy / 5) * size_t(m_mid.n) + size_t(ix / 5)];
                mid = std::max(mid, float(z));
                float &coarse = m_coarse.h[size_t(iy / 20) * size_t(m_coarse.n) + size_t(ix / 20)];
                coarse = std::max(coarse, float(z));
            }
    }
}

double NonplanarHeightMap::max_in_rect(const Grid &grid, double x0, double y0, double x1, double y1) const
{
    int ix0, iy0, ix1, iy1;
    grid.index(x0, y0, ix0, iy0);
    grid.index(x1, y1, ix1, iy1);
    ix0 = std::max(ix0, 0); iy0 = std::max(iy0, 0);
    ix1 = std::min(ix1, grid.n - 1); iy1 = std::min(iy1, grid.n - 1);
    double out = -1e9;
    for (int iy = iy0; iy <= iy1; ++ iy)
        for (int ix = ix0; ix <= ix1; ++ ix)
            out = std::max(out, double(grid.at(ix, iy)));
    return out;
}

double NonplanarHeightMap::travel_height(const std::vector<Vec2d> &path, const NonplanarClearanceModel &model, double min_z) const
{
    double       z      = min_z;
    const double safety = model.safety_mm;
    if (m_fine.h.empty() || m_max_z + safety <= z)
        return z;
    std::vector<NonplanarClearanceModel::Ring> rings = model.rings();
    rings.front().bottom = 0.;
    const double half_diagonal = m_fine.cell * std::sqrt(0.5);
    const double reach         = rings.back().radius + half_diagonal;
    for (const auto &[a, b] : pieces(path, 2.)) {
        int ix0, iy0, ix1, iy1;
        m_fine.index(std::min(a.x(), b.x()) - reach, std::min(a.y(), b.y()) - reach, ix0, iy0);
        m_fine.index(std::max(a.x(), b.x()) + reach, std::max(a.y(), b.y()) + reach, ix1, iy1);
        ix0 = std::max(ix0, 0); iy0 = std::max(iy0, 0);
        ix1 = std::min(ix1, m_fine.n - 1); iy1 = std::min(iy1, m_fine.n - 1);
        for (int iy = iy0; iy <= iy1; ++ iy)
            for (int ix = ix0; ix <= ix1; ++ ix) {
                const double h = m_fine.at(ix, iy);
                if (h + safety <= z)
                    continue;
                const Vec2d  c(origin + (ix + 0.5) * m_fine.cell, origin + (iy + 0.5) * m_fine.cell);
                const double d = distance_to_segment(c, a, b);
                for (const NonplanarClearanceModel::Ring &ring : rings)
                    if (d <= ring.radius + half_diagonal) {
                        z = std::max(z, h - ring.bottom + safety);
                        break;
                    }
            }
    }
    for (const NonplanarClearanceModel::Box &box : model.boxes) {
        if (m_max_z - box.bottom + safety <= z)
            continue;
        const bool  large = box.x_max - box.x_min > 20.;
        const Grid &grid  = large ? m_coarse : m_mid;
        for (const auto &[a, b] : pieces(path, large ? 4. : 1.)) {
            const double h = this->max_in_rect(grid,
                std::min(a.x(), b.x()) + box.x_min, std::min(a.y(), b.y()) + box.y_min,
                std::max(a.x(), b.x()) + box.x_max, std::max(a.y(), b.y()) + box.y_max);
            z = std::max(z, h - box.bottom + safety);
        }
    }
    return z;
}

bool NonplanarHeightMap::supports_travel(const Vec3d &from, const Vec3d &to, double max_gap) const
{
    if (m_fine.h.empty())
        return false;
    const int steps = std::max(1, int(std::ceil((to - from).head<2>().norm() / m_fine.cell)));
    for (int k = 0; k <= steps; ++k) {
        const Vec3d p = from + (to - from) * (double(k) / steps);
        int ix, iy;
        if (!m_fine.index(p.x(), p.y(), ix, iy) || m_fine.at(ix, iy) < p.z() - max_gap)
            return false;
    }
    return true;
}

double NonplanarHeightMap::travel_lift(const Vec3d &from, const Vec3d &to, const NonplanarClearanceModel &model) const
{
    if (m_fine.h.empty() || m_max_z + model.safety_mm <= std::min(from.z(), to.z()))
        return 0.;
    const Vec2d a = from.head<2>(), b = to.head<2>(), d = b - a;
    const double length2 = d.squaredNorm(), dz = to.z() - from.z();
    if (length2 < EPSILON * EPSILON)
        return this->travel_height({a}, model, std::min(from.z(), to.z())) - std::min(from.z(), to.z());
    double lift = 0.;
    auto height = [&](double lo, double hi) { return from.z() + dz * (dz < 0. ? hi : lo); };
    auto require = [&](double material, double bottom, double tool_z) {
        lift = std::max(lift, material - bottom + model.safety_mm - tool_z);
    };
    const double half_diagonal = m_fine.cell * std::sqrt(0.5);
    const double reach = model.nozzle.back().radius + half_diagonal;
    for (const auto &[pa, pb] : pieces({a, b}, 2.)) {
        int ix0, iy0, ix1, iy1;
        m_fine.index(std::min(pa.x(), pb.x()) - reach, std::min(pa.y(), pb.y()) - reach, ix0, iy0);
        m_fine.index(std::max(pa.x(), pb.x()) + reach, std::max(pa.y(), pb.y()) + reach, ix1, iy1);
        for (int iy = std::max(iy0, 0); iy <= std::min(iy1, m_fine.n - 1); ++iy)
            for (int ix = std::max(ix0, 0); ix <= std::min(ix1, m_fine.n - 1); ++ix) {
                const double h = m_fine.at(ix, iy);
                if (h + model.safety_mm <= std::min(from.z(), to.z()) + lift)
                    continue;
                const Vec2d c(origin + (ix + 0.5) * m_fine.cell, origin + (iy + 0.5) * m_fine.cell);
                const Vec2d delta = c - a;
                const double centre = delta.dot(d) / length2;
                const double perpendicular2 = std::max(0., delta.squaredNorm() - sqr(delta.dot(d)) / length2);
                for (size_t i = 0; i < model.nozzle.size(); ++i) {
                    const auto &ring = model.nozzle[i];
                    const double radius = ring.radius + half_diagonal;
                    if (perpendicular2 > radius * radius)
                        continue;
                    const double span = std::sqrt((radius * radius - perpendicular2) / length2);
                    const double lo = std::max(0., centre - span), hi = std::min(1., centre + span);
                    if (lo <= hi)
                        require(h, i == 0 ? 0. : ring.bottom, height(lo, hi));
                }
            }
    }
    for (const auto &box : model.boxes) {
        if (m_max_z - box.bottom + model.safety_mm <= std::min(from.z(), to.z()) + lift)
            continue;
        const bool large = box.x_max - box.x_min > 20.;
        const Grid &grid = large ? m_coarse : m_mid;
        for (const auto &[pa, pb] : pieces({a, b}, large ? 4. : 1.)) {
            int ix0, iy0, ix1, iy1;
            grid.index(std::min(pa.x(), pb.x()) + box.x_min, std::min(pa.y(), pb.y()) + box.y_min, ix0, iy0);
            grid.index(std::max(pa.x(), pb.x()) + box.x_max, std::max(pa.y(), pb.y()) + box.y_max, ix1, iy1);
            for (int iy = std::max(iy0, 0); iy <= std::min(iy1, grid.n - 1); ++iy)
                for (int ix = std::max(ix0, 0); ix <= std::min(ix1, grid.n - 1); ++ix) {
                    double lo = 0., hi = 1.;
                    const Vec2d cell_min(origin + ix * grid.cell, origin + iy * grid.cell);
                    const Vec2d lower = cell_min - Vec2d(box.x_max, box.y_max);
                    const Vec2d upper = cell_min + Vec2d::Constant(grid.cell) - Vec2d(box.x_min, box.y_min);
                    for (int axis = 0; axis < 2; ++axis) {
                        if (std::abs(d[axis]) < EPSILON) {
                            if (a[axis] < lower[axis] || a[axis] > upper[axis]) hi = -1.;
                        } else {
                            const double t0 = (lower[axis] - a[axis]) / d[axis], t1 = (upper[axis] - a[axis]) / d[axis];
                            lo = std::max(lo, std::min(t0, t1));
                            hi = std::min(hi, std::max(t0, t1));
                        }
                    }
                    if (lo <= hi)
                        require(grid.at(ix, iy), box.bottom, height(lo, hi));
                }
        }
    }
    return lift;
}

}
