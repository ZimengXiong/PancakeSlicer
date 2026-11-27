// Curved tops: local access order within one fich-l conformal shell (AGPL-3.0).
#include "CurvedTopsOrder.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <iterator>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {

struct Piece {
    ExtrusionPath path;
    Vec2d center;
    double rx, ry, low, high;
};

Piece piece_of(ExtrusionPath path)
{
    const Vec3d a = unscale(path.polyline.points.front()), b = unscale(path.polyline.points.back());
    if (a.z() > b.z()) path.reverse();
    return {std::move(path), 0.5 * (a.head<2>() + b.head<2>()),
            0.5 * std::abs(a.x() - b.x()), 0.5 * std::abs(a.y() - b.y()),
            std::min(a.z(), b.z()), std::max(a.z(), b.z())};
}

std::vector<Piece> split_paths(const std::vector<ExtrusionPath> &paths, double xy_step = 1., double z_step = .04)
{
    std::vector<Piece> out;
    for (const ExtrusionPath &path : paths)
        for (size_t i = 1; i < path.polyline.points.size(); ++i) {
            const Point3 &a = path.polyline.points[i - 1], &b = path.polyline.points[i];
            const Vec3d d = unscale(Vec3crd(b - a));
            const int n = std::max({1, int(std::ceil(d.head<2>().norm() / xy_step)), int(std::ceil(std::abs(d.z()) / z_step))});
            Point3 from = a;
            for (int k = 1; k <= n; ++k) {
                const Point3 to = k == n ? b : Point3(Vec3crd((a.cast<double>() + double(k) / n * (b - a).cast<double>()).array().round().cast<coord_t>()));
                if (from != to) out.push_back(piece_of(ExtrusionPath(Polyline3(Points3{from, to}), path)));
                from = to;
            }
        }
    return out;
}

bool obstructs(const Piece &material, const Piece &tool, const NonplanarClearanceModel &head, bool flow_height = false)
{
    const double dz = material.high - tool.low;
    const double grow = 0.5 * material.path.width + head.bead_margin + head.safety_mm;
    const Vec2d delta = material.center - tool.center;
    const double rx = material.rx + tool.rx, ry = material.ry + tool.ry;
    const double x = std::max(0., std::abs(delta.x()) - rx), y = std::max(0., std::abs(delta.y()) - ry);
    for (const auto &ring : head.nozzle) {
        const double bottom = flow_height && ring.bottom == head.contact_allowance_mm ?
            std::min(.1, .5 * double(tool.path.height)) : ring.bottom;
        if (dz > bottom - head.safety_mm && x * x + y * y < std::pow(ring.radius + grow, 2)) return true;
    }
    for (const auto &box : head.boxes)
        if (dz > box.bottom - head.safety_mm && delta.x() + rx > box.x_min - grow &&
            delta.x() - rx < box.x_max + grow && delta.y() + ry > box.y_min - grow && delta.y() - ry < box.y_max + grow)
            return true;
    return false;
}

using Graph = std::vector<std::vector<size_t>>;
template<typename Visit>
void visit_reachable_pairs(const std::vector<Piece> &pieces, const NonplanarClearanceModel &head, bool flow_height, Visit &&visit)
{
    if (pieces.empty()) return;
    double low = pieces.front().low, high = pieces.front().high, reach = 0.;
    for (const auto &p : pieces) low = std::min(low, p.low), high = std::max(high, p.high);
    double contact = head.contact_allowance_mm;
    if (flow_height)
        for (const auto &piece : pieces) contact = std::min(contact, .5 * double(piece.path.height));
    for (const auto &r : head.nozzle) {
        const double bottom = flow_height && r.bottom == head.contact_allowance_mm ? contact : r.bottom;
        if (high - low > bottom - head.safety_mm) reach = std::max(reach, r.radius);
    }
    for (const auto &b : head.boxes)
        if (high - low > b.bottom - head.safety_mm)
            reach = std::max({reach, std::abs(b.x_min), std::abs(b.x_max), std::abs(b.y_min), std::abs(b.y_max)});
    if (reach == 0.) return;
    double half_width = 0.;
    for (const auto &p : pieces) half_width = std::max(half_width, 0.5 * double(p.path.width));
    reach += 1. + half_width + head.bead_margin + head.safety_mm;
    const double cell = std::max(1., reach);
    std::map<std::pair<int,int>, std::vector<size_t>> grid;
    auto key = [cell](const Vec2d &p) { return std::make_pair(int(std::floor(p.x() / cell)), int(std::floor(p.y() / cell))); };
    for (size_t i = 0; i < pieces.size(); ++i) grid[key(pieces[i].center)].push_back(i);
    for (size_t i = 0; i < pieces.size(); ++i) {
        const auto [cx,cy] = key(pieces[i].center);
        for (int x = cx - 1; x <= cx + 1; ++x)
            for (int y = cy - 1; y <= cy + 1; ++y) {
                const auto found = grid.find({x,y});
                if (found == grid.end()) continue;
                for (size_t j : found->second)
                    if (i != j && visit(i, j)) return;
            }
    }
}

Graph dependencies(const std::vector<Piece> &pieces, const NonplanarClearanceModel &head, bool flow_height = false)
{
    Graph graph(pieces.size());
    visit_reachable_pairs(pieces, head, flow_height, [&](size_t i, size_t j) {
        if (obstructs(pieces[j], pieces[i], head, flow_height)) graph[i].push_back(j);
        return false;
    });
    return graph;
}

std::set<size_t> cyclic_pieces(const Graph &graph)
{
    std::vector<int> index(graph.size(), -1), low(graph.size());
    std::vector<size_t> stack;
    std::vector<bool> on_stack(graph.size(), false);
    std::set<size_t> cycles;
    int next = 0;
    std::function<void(size_t)> visit = [&](size_t v) {
        index[v] = low[v] = next++;
        stack.push_back(v); on_stack[v] = true;
        for (size_t w : graph[v]) {
            if (index[w] < 0) { visit(w); low[v] = std::min(low[v], low[w]); }
            else if (on_stack[w]) low[v] = std::min(low[v], index[w]);
        }
        if (low[v] != index[v]) return;
        std::vector<size_t> component;
        for (;;) {
            const size_t w = stack.back(); stack.pop_back(); on_stack[w] = false; component.push_back(w);
            if (w == v) break;
        }
        if (component.size() > 1) cycles.insert(component.begin(), component.end());
    };
    for (size_t v = 0; v < graph.size(); ++v) if (index[v] < 0) visit(v);
    return cycles;
}

}

bool continuous_curved_top_clear(const ExtrusionPath &path, const NonplanarClearanceModel &head)
{
    return continuous_curved_top_clear(std::vector<ExtrusionPath>{path}, head);
}

bool continuous_curved_top_clear(const std::vector<ExtrusionPath> &paths, const NonplanarClearanceModel &head)
{
    if (paths.empty()) return false;
    for (size_t i = 0; i < paths.size(); ++i)
        if (paths[i].polyline.points.size() < 2 || (i > 0 && paths[i - 1].last_point3() != paths[i].first_point3()))
            return false;
    const bool flow_height = std::all_of(paths.begin(), paths.end(), [](const ExtrusionPath &path) { return path.continuous_top; });
    const auto pieces = split_paths(paths, .1, .01);
    std::vector<double> arc;
    double walked = 0.;
    for (const auto &piece : pieces) {
        const double length = unscale(Vec3crd(piece.path.polyline.points.back() - piece.path.polyline.points.front())).norm();
        arc.push_back(walked + .5 * length);
        walked += length;
    }
    double contact_run = 1.;
    for (const auto &path : paths) contact_run = std::max(contact_run, 2. * double(path.width));
    const bool closed = paths.front().first_point3() == paths.back().last_point3();
    std::vector<std::vector<Piece>> refined(pieces.size());
    const auto fine = [&](size_t i) -> const std::vector<Piece> & {
        if (refined[i].empty()) refined[i] = split_paths({pieces[i].path}, .01, .001);
        return refined[i];
    };
    bool clear = true;
    visit_reachable_pairs(pieces, head, flow_height, [&](size_t i, size_t j) {
        if (!(j < i && arc[i] - arc[j] > contact_run && (!closed || walked - (arc[i] - arc[j]) > contact_run)) ||
            !obstructs(pieces[j], pieces[i], head, flow_height))
            return false;
        bool blocked = false;
        for (const auto &tool : fine(i)) {
            for (const auto &material : fine(j))
                if (obstructs(material, tool, head, flow_height)) { blocked = true; break; }
            if (blocked) break;
        }
        if (!blocked) return false;
        BOOST_LOG_TRIVIAL(info) << "Curved top boundary access: surface " << paths.front().source_surface_id
            << " path " << paths.front().source_path_id << " tool arc " << arc[i] << " material arc " << arc[j]
            << " length " << walked << " rise " << pieces[j].high - pieces[i].low;
        clear = false;
        return true;
    });
    return clear;
}

std::vector<ExtrusionPath> order_curved_tops_paths(const std::vector<ExtrusionPath> &paths,
                                                const NonplanarClearanceModel &head, const Point &start)
{
    if (!paths.empty() && std::all_of(paths.begin(), paths.end(), [](const ExtrusionPath &path) { return path.continuous_top; }))
        return paths;
    struct Ends { Point3 first, last; bool connected = true; };
    std::map<size_t, Ends> ends;
    for (const auto &path : paths)
        if (!path.can_reverse() && path.role() == erExternalPerimeter && path.distance_to_top < .001f && !path.polyline.empty()) {
            const auto [it, inserted] = ends.emplace(path.source_path_id, Ends{path.polyline.points.front(), path.polyline.points.back()});
            if (!inserted) {
                it->second.connected &= it->second.last == path.polyline.points.front();
                it->second.last = path.polyline.points.back();
            }
        }
    std::set<size_t> finish_ids;
    for (const auto &[id, chain] : ends)
        if (chain.connected && chain.first == chain.last) finish_ids.insert(id);
    std::vector<ExtrusionPath> interior, boundary;
    for (const auto &path : paths)
        (finish_ids.count(path.source_path_id) ? boundary : interior).push_back(path);
    std::vector<Piece> pieces = split_paths(interior);
    Graph graph;
    for (int pass = 0; ; ++pass) {
        graph = dependencies(pieces, head);
        const auto cycles = cyclic_pieces(graph);
        if (cycles.empty()) break;
        if (pass == 3) throw SlicingError("Curved tops: local skin/perimeter access cycle remains after three piece refinements.");
        std::vector<Piece> refined;
        for (size_t i = 0; i < pieces.size(); ++i) {
            if (cycles.count(i) == 0) { refined.push_back(std::move(pieces[i])); continue; }
            const ExtrusionPath &path = pieces[i].path;
            const Point3 a = path.polyline.points.front(), b = path.polyline.points.back();
            const Point3 mid(Vec3crd((0.5 * (a.cast<double>() + b.cast<double>())).array().round().cast<coord_t>()));
            if (mid == a || mid == b) throw SlicingError("Curved tops: local skin/perimeter access cycle is below coordinate resolution.");
            refined.push_back(piece_of(ExtrusionPath(Polyline3(Points3{a,mid}), path)));
            refined.push_back(piece_of(ExtrusionPath(Polyline3(Points3{mid,b}), path)));
        }
        pieces = std::move(refined);
    }
    std::vector<size_t> incoming(pieces.size(), 0), ready;
    for (const auto &edges : graph) for (size_t v : edges) ++incoming[v];
    for (size_t v = 0; v < pieces.size(); ++v) if (incoming[v] == 0) ready.push_back(v);
    std::vector<ExtrusionPath> ordered;
    ordered.reserve(pieces.size());
    Vec2d from = unscale(start);
    double from_z = 0.;
    size_t previous_source = size_t(-1);
    while (!ready.empty()) {
        auto best = ready.begin();
        double distance = std::numeric_limits<double>::infinity();
        bool reverse = false;
        for (auto it = ready.begin(); it != ready.end(); ++it) {
            const auto &path = pieces[*it].path;
            for (bool reversed : {false, true}) {
                const Vec3d begin = unscale(reversed ? path.polyline.points.back() : path.polyline.points.front());
                const double dz = ordered.empty() ? 0. : begin.z() - from_z;
                const double score = (begin.head<2>() - from).squaredNorm() + dz * dz +
                    (path.source_path_id == previous_source ? 0. : 0.04);
                if (score < distance) distance = score, best = it, reverse = reversed;
            }
        }
        const size_t v = *best; *best = ready.back(); ready.pop_back();
        ExtrusionPath &path = pieces[v].path;
        if (reverse) path.reverse();
        const Vec3d end = unscale(path.polyline.points.back());
        from = end.head<2>(); from_z = end.z(); previous_source = path.source_path_id;
        if (!ordered.empty() && ordered.back().source_path_id == path.source_path_id &&
            ordered.back().height == path.height && ordered.back().mm3_per_mm == path.mm3_per_mm &&
            ordered.back().polyline.points.back() == path.polyline.points.front())
            ordered.back().polyline.points.insert(ordered.back().polyline.points.end(), std::next(path.polyline.points.begin()), path.polyline.points.end());
        else ordered.push_back(std::move(path));
        for (size_t w : graph[v]) if (--incoming[w] == 0) ready.push_back(w);
    }
    ordered.insert(ordered.begin(), std::make_move_iterator(boundary.begin()), std::make_move_iterator(boundary.end()));
    return ordered;
}

}
