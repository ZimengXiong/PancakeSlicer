#include "Calibration.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Nonplanar/HeadEnvelope.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/ClearanceVerifier.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolKinematics.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/format.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <utility>

namespace Slic3r::Nonplanar::Calibration {

namespace {

constexpr double Inf = std::numeric_limits<double>::infinity();

constexpr double PadHeight = 1.0;
constexpr double PadFlange = 1.0;
constexpr double ConeApexRadius = 1.5;
constexpr double PatchLength = 5., PatchWidth = 7., PatchCrown = 0.25;
constexpr double DomeRadius = 5., DomeHeight = 3., SaddleHalf = 5., SaddleAmplitude = 1.;
constexpr double FitMargin = 0.2;
constexpr double TouchMargin = 0.05;
constexpr double LayoutGap = 3., BedMargin = 5.;

indexed_triangle_set grid_solid(double x0, double x1, double y0, double y1, int nx, int ny, const std::function<double(double, double)> &z)
{
    indexed_triangle_set its;
    auto top = [nx](int i, int j) { return j * (nx + 1) + i; };
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i) {
            const double x = x0 + (x1 - x0) * i / nx, y = y0 + (y1 - y0) * j / ny;
            its.vertices.emplace_back(float(x), float(y), float(z(x, y)));
        }
    const int base = int(its.vertices.size());
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i)
            its.vertices.emplace_back(its.vertices[size_t(top(i, j))].x(), its.vertices[size_t(top(i, j))].y(), 0.f);
    auto bottom = [&](int i, int j) { return base + top(i, j); };
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            its.indices.emplace_back(top(i, j), top(i + 1, j), top(i + 1, j + 1));
            its.indices.emplace_back(top(i, j), top(i + 1, j + 1), top(i, j + 1));
            its.indices.emplace_back(bottom(i, j), bottom(i + 1, j + 1), bottom(i + 1, j));
            its.indices.emplace_back(bottom(i, j), bottom(i, j + 1), bottom(i + 1, j + 1));
        }
    std::vector<std::pair<int, int>> ring;
    for (int i = 0; i < nx; ++i) ring.emplace_back(i, 0);
    for (int j = 0; j < ny; ++j) ring.emplace_back(nx, j);
    for (int i = nx; i > 0; --i) ring.emplace_back(i, ny);
    for (int j = ny; j > 0; --j) ring.emplace_back(0, j);
    for (size_t k = 0; k < ring.size(); ++k) {
        const auto [ia, ja] = ring[k];
        const auto [ib, jb] = ring[(k + 1) % ring.size()];
        its.indices.emplace_back(top(ia, ja), bottom(ia, ja), bottom(ib, jb));
        its.indices.emplace_back(top(ia, ja), bottom(ib, jb), top(ib, jb));
    }
    return its;
}

indexed_triangle_set disk_solid(double radius, int rings, int sectors, const std::function<double(double)> &z)
{
    indexed_triangle_set its;
    its.vertices.emplace_back(0.f, 0.f, float(z(0.)));
    for (int i = 1; i <= rings; ++i) {
        const double r = radius * i / rings;
        for (int k = 0; k < sectors; ++k) {
            const double a = 2. * PI * k / sectors;
            its.vertices.emplace_back(float(r * std::cos(a)), float(r * std::sin(a)), float(z(r)));
        }
    }
    auto top = [sectors](int i, int k) { return i == 0 ? 0 : 1 + (i - 1) * sectors + (k % sectors); };
    const int bottom_center = int(its.vertices.size());
    its.vertices.emplace_back(0.f, 0.f, 0.f);
    const int bottom_ring = int(its.vertices.size());
    for (int k = 0; k < sectors; ++k)
        its.vertices.emplace_back(its.vertices[size_t(top(rings, k))].x(), its.vertices[size_t(top(rings, k))].y(), 0.f);
    for (int k = 0; k < sectors; ++k) {
        its.indices.emplace_back(0, top(1, k), top(1, k + 1));
        for (int i = 1; i < rings; ++i) {
            its.indices.emplace_back(top(i, k), top(i + 1, k), top(i + 1, k + 1));
            its.indices.emplace_back(top(i, k), top(i + 1, k + 1), top(i, k + 1));
        }
        const int b0 = bottom_ring + k, b1 = bottom_ring + (k + 1) % sectors;
        its.indices.emplace_back(top(rings, k), b0, b1);
        its.indices.emplace_back(top(rings, k), b1, top(rings, k + 1));
        its.indices.emplace_back(bottom_center, b1, b0);
    }
    return its;
}

void translate(indexed_triangle_set &its, const Vec3f &d)
{
    for (stl_vertex &v : its.vertices)
        v += d;
}

struct Footprint
{
    Vec2d  size;
    double relief;
};

BoundingBoxf bed_box(const DynamicPrintConfig &config)
{
    BoundingBoxf bed;
    if (const auto *area = config.option<ConfigOptionPoints>("printable_area"))
        for (const Vec2d &p : area->values)
            bed.merge(p);
    if (!bed.defined || bed.size().x() <= 0. || bed.size().y() <= 0.)
        throw RuntimeError(_u8L("Non-planar calibration: the printer has no printable area."));
    return bed;
}

std::vector<Vec2d> grid_layout(const std::vector<Footprint> &shapes, int cols, const HeadEnvelope &head, double gap, const BoundingBoxf &bed)
{
    const int n    = int(shapes.size());
    const int rows = (n + cols - 1) / cols;
    double    w = 0., h = 0., max_relief = 0.;
    for (const Footprint &s : shapes) {
        w          = std::max(w, s.size.x());
        h          = std::max(h, s.size.y());
        max_relief = std::max(max_relief, s.relief);
    }
    const std::vector<LowBox> boxes = low_boxes(head, gap, max_relief);
    const double room_x = bed.size().x() - 2. * BedMargin, room_y = bed.size().y() - 2. * BedMargin;
    auto place = [&](double px, double py) {
        std::vector<Vec2d> out;
        for (int i = 0; i < n; ++i)
            out.emplace_back(bed.center().x() + px * ((i % cols) - 0.5 * (cols - 1)), bed.center().y() + py * ((i / cols) - 0.5 * (rows - 1)));
        return out;
    };
    auto clean = [&](const std::vector<Vec2d> &at) {
        std::vector<BoundingBoxf> rect;
        for (int i = 0; i < n; ++i)
            rect.emplace_back(at[size_t(i)] - 0.5 * shapes[size_t(i)].size, at[size_t(i)] + 0.5 * shapes[size_t(i)].size);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (i != j && conflicts(boxes, rect[size_t(i)], shapes[size_t(i)].relief, rect[size_t(j)], shapes[size_t(j)].relief))
                    return false;
        return true;
    };
    double best = Inf;
    std::vector<Vec2d> out;
    for (double px = w + LayoutGap; (cols - 1) * px + w <= room_x + EPSILON; px += 1.) {
        for (double py = h + LayoutGap; (rows - 1) * py + h <= room_y + EPSILON && px * py < best; py += 1.)
            if (std::vector<Vec2d> at = place(px, py); clean(at)) {
                best = px * py;
                out  = std::move(at);
                break;
            }
        if (cols == 1)
            break;
    }
    if (out.empty())
        throw RuntimeError(_u8L("Non-planar calibration: the test plate does not fit this printer's bed with the selected head's clearance."));
    return out;
}

std::vector<Vec2d> best_layout(const std::vector<Footprint> &shapes, std::initializer_list<int> cols, const HeadEnvelope &head, double gap,
                               const BoundingBoxf &bed)
{
    std::vector<Vec2d> best;
    double             best_area = Inf;
    for (int c : cols)
        try {
            std::vector<Vec2d> at = grid_layout(shapes, c, head, gap, bed);
            BoundingBoxf       box;
            for (size_t i = 0; i < at.size(); ++i) {
                box.merge(at[i] - 0.5 * shapes[i].size);
                box.merge(at[i] + 0.5 * shapes[i].size);
            }
            if (box.size().x() * box.size().y() < best_area) {
                best_area = box.size().x() * box.size().y();
                best      = std::move(at);
            }
        } catch (const RuntimeError &) {
        }
    if (best.empty())
        throw RuntimeError(_u8L("Non-planar calibration: the test plate does not fit this printer's bed with the selected head's clearance."));
    return best;
}

double cone_foot_radius(const ClearanceStep &step) { return step.relief / std::tan(Geometry::deg2rad(step.slope_deg)) + 0.5 * ConeApexRadius; }

double cone_height(const ClearanceStep &step, double r)
{
    const double t = std::tan(Geometry::deg2rad(step.slope_deg)), foot = cone_foot_radius(step);
    if (r >= ConeApexRadius)
        return std::max(0., (foot - r) * t);
    return (foot - ConeApexRadius) * t + t * (ConeApexRadius * ConeApexRadius - r * r) / (2. * ConeApexRadius);
}

double layer_height(const DynamicPrintConfig &config)
{
    const auto *opt = config.option<ConfigOptionFloat>("layer_height");
    return opt != nullptr && opt->value > 0. ? opt->value : 0.2;
}


std::string format_number(double value, int decimals)
{
    return (boost::format("%." + std::to_string(decimals) + "f") % value).str();
}

}

std::string test_id(Test test)
{
    switch (test) {
    case Test::Clearance: return "nonplanar-clearance";
    case Test::TiltFlow: return "nonplanar-tilt-flow";
    case Test::CurvedTop: return "nonplanar-curved-top";
    case Test::Verifier: return "nonplanar-verifier";
    }
    return {};
}

std::optional<Test> parse_test(const std::string &id)
{
    for (Test test : { Test::Clearance, Test::TiltFlow, Test::CurvedTop, Test::Verifier })
        if (id == test_id(test))
            return test;
    return std::nullopt;
}

const std::vector<ClearanceStep> &clearance_steps()
{
    static const std::vector<ClearanceStep> steps{ { 12., 0.8 }, { 15., 1.2 }, { 18., 1.8 }, { 21., 2.6 },
                                                   { 24., 3.6 }, { 27., 4.6 }, { 30., 5.8 }, { 32., 7.2 } };
    return steps;
}

double step_clearance(const HeadEnvelope &head, const ClearanceStep &step, double gap, std::string *component)
{
    const double foot = cone_foot_radius(step);
    auto cone = [&](double r) { return cone_height(step, r); };
    double worst = Inf;
    for (int ring = 0; ring <= 8; ++ring) {
        const double rho = foot * ring / 8., tip = cone(rho);
        for (int k = 0; k < 72; ++k) {
            const double phi = 2. * PI * k / 72.;
            const Vec2d  axis(rho * std::cos(phi), rho * std::sin(phi));
            for (const HeadBox &box : head.boxes) {
                const double grow = box.nozzle ? 0. : head.lateral_margin;
                const Vec2d  lo(box.infinite_x ? -Inf : box.min.x() - grow, box.infinite_y ? -Inf : box.min.y() - grow);
                const Vec2d  hi(box.infinite_x ? Inf : box.max.x() + grow, box.infinite_y ? Inf : box.max.y() + grow);
                const Vec2d  nearest(std::clamp(axis.x(), lo.x(), hi.x()), std::clamp(axis.y(), lo.y(), hi.y()));
                double       z = box.min.z();
                if (box.nozzle) {
                    const double half_x = std::max(std::abs(box.min.x()), std::abs(box.max.x()));
                    const double half_y = std::max(std::abs(box.min.y()), std::abs(box.max.y()));
                    if (std::hypot(half_x, half_y) <= head.tip_allowance.radius)
                        z = std::max(z, head.tip_allowance.height);
                    if (z >= box.max.z())
                        continue;
                } else
                    z -= head.margin;
                if (const double c = gap + z - (cone((nearest - axis).norm()) - tip); c < worst) {
                    worst = c;
                    if (component != nullptr)
                        *component = box.name;
                }
            }
        }
    }
    return worst;
}

HeadEnvelope calibrated_head(const DynamicPrintConfig &edited, const DynamicPrintConfig &saved)
{
    PrintConfig printer;
    printer.apply(edited, true);
    if (printer.nonplanar_head.value == ProbeHeadId) {
        PrintConfig selected;
        selected.apply(saved, true);
        printer.nonplanar_head          = selected.nonplanar_head;
        printer.nonplanar_head_snapshot = selected.nonplanar_head_snapshot;
    }
    return resolve_head_envelope(printer);
}

int predicted_first_touch(const HeadEnvelope &head, double gap)
{
    const std::vector<ClearanceStep> &steps = clearance_steps();
    for (size_t k = 0; k < steps.size(); ++k)
        if (step_clearance(head, steps[k], gap) < 0.)
            return int(k) + 1;
    return int(steps.size()) + 1;
}

ClearanceFit fit_clearance(const HeadEnvelope &base, int last_clean, double gap, const std::string &id, const std::string &name)
{
    const std::vector<ClearanceStep> &steps = clearance_steps();
    if (last_clean < 0 || last_clean > int(steps.size()))
        throw RuntimeError((boost::format(_u8L("The last clean cone must be between 0 and %1%.")) % steps.size()).str());
    HeadEnvelope shroud = base;
    shroud.boxes.erase(std::remove_if(shroud.boxes.begin(), shroud.boxes.end(),
                                      [](const HeadBox &b) { return b.nozzle || b.infinite_x || b.infinite_y; }),
                       shroud.boxes.end());
    double lowest = Inf;
    for (const HeadBox &box : shroud.boxes)
        lowest = std::min(lowest, box.min.z());
    if (shroud.boxes.empty())
        throw RuntimeError(_u8L("The head has no body above the nozzle to calibrate."));
    auto needed = [&](int step) { return -step_clearance(shroud, steps[size_t(step - 1)], gap); };
    double shift;
    if (last_clean == 0)
        shift = needed(1) - std::max(gap, 0.1);
    else {
        shift = -Inf;
        for (int k = 1; k <= last_clean; ++k)
            shift = std::max(shift, needed(k));
    }
    shift += FitMargin;
    if (last_clean < int(steps.size()))
        shift = std::min(shift, needed(last_clean + 1) - TouchMargin);
    shift = std::max(shift, -lowest);

    ClearanceFit out;
    out.head = base;
    for (HeadBox &box : out.head.boxes)
        if (!box.nozzle && !box.infinite_x && !box.infinite_y) {
            box.min.z() += shift;
            box.max.z() += shift;
        }
    if (last_clean > 0) {
        const double floor = steps[size_t(last_clean - 1)].relief + out.head.margin - gap;
        for (HeadBox &box : out.head.boxes)
            if ((box.infinite_x || box.infinite_y) && box.min.z() < floor) {
                box.max.z() += floor - box.min.z();
                box.min.z() = floor;
            }
    }
    out.shift        = shift;
    out.head.id      = id;
    out.head.name    = name;
    out.head.calibrated = true;
    const std::string step_text = last_clean == 0 ? std::string("none") :
        (boost::format("%1% (%2% deg, %3% mm)") % last_clean % steps[size_t(last_clean - 1)].slope_deg % steps[size_t(last_clean - 1)].relief).str();
    out.head.provenance = base.provenance + (base.provenance.empty() ? "" : " ") + "Calibrated from " + base.id +
                          " with the non-planar clearance plate at layer height " + format_number(gap, 2) + " mm: last clean step " +
                          step_text + "; body moved " + format_number(shift, 2) + " mm.";
    out.slope_cap_deg = Inf;
    for (double slope : out.head.slope_map(72, gap))
        out.slope_cap_deg = std::min(out.slope_cap_deg, Geometry::rad2deg(slope));
    const double steep = steps.back().slope_deg;
    double lo = 0., hi = 100.;
    if (step_clearance(out.head, { steep, hi }, gap) >= 0.)
        lo = hi;
    for (int i = 0; i < 40 && hi - lo > 1e-3; ++i) {
        const double mid = 0.5 * (lo + hi);
        (step_clearance(out.head, { steep, mid }, gap) >= 0. ? lo : hi) = mid;
    }
    out.band_height  = lo;
    out.gantry_floor = out.head.gantry_clearance();
    out.head = load_head_envelope(save_head_envelope(out.head));
    return out;
}

int verifier_first_touch(const Plate &plate, const GCodeProcessorResult &result, const HeadEnvelope &head,
                         const ToolKinematics &kinematics, ClearanceReport *report)
{
    const std::vector<ClearanceStep> &steps = clearance_steps();
    ClearanceParams params;
    params.head = head;
    ClearanceReport run = verify_clearance(result, params, kinematics);
    int first = int(steps.size()) + 1;
    for (size_t i = 0; i < run.per_move.size() && i < result.moves.size(); ++i) {
        const auto &move = result.moves[i];
        if (!(run.per_move[i] < 0.f) || move.type != EMoveType::Extrude)
            continue;
        const Vec2d p(move.position.x(), move.position.y());
        for (size_t k = 0; k < plate.objects.size() && k < steps.size(); ++k)
            if ((p - plate.objects[k].center).norm() <= cone_foot_radius(steps[k]) + PadFlange + 0.5)
                first = std::min(first, int(k) + 1);
    }
    if (report != nullptr)
        *report = std::move(run);
    return first;
}

const std::vector<double> &tilt_slopes()
{
    static const std::vector<double> slopes{ 0., 10., 20., 30., 40. };
    return slopes;
}

const std::vector<double> &tilt_multipliers()
{
    static const std::vector<double> multipliers{ 0.90, 0.95, 1.00, 1.05, 1.10 };
    return multipliers;
}

std::vector<double> tilt_flow_result(const std::vector<double> &current, const std::vector<int> &picks)
{
    const std::vector<double> &slopes = tilt_slopes();
    if (picks.size() != slopes.size())
        throw RuntimeError(_u8L("Pick one patch per slope."));
    std::vector<double> out(slopes.size(), 1.);
    for (size_t i = 0; i < slopes.size(); ++i) {
        if (picks[i] < 0 || picks[i] >= int(tilt_multipliers().size()))
            throw RuntimeError(_u8L("Pick one of the printed patches for every slope."));
        const double base = current.size() == slopes.size() ? current[i] : 1.;
        out[i] = std::round(std::clamp(base * tilt_multipliers()[size_t(picks[i])], 0.5, 1.5) * 1000.) / 1000.;
    }
    return out;
}

const std::vector<double> &top_speed_factors()
{
    static const std::vector<double> factors{ 60., 80., 100. };
    return factors;
}

const std::vector<double> &top_fan_boosts()
{
    static const std::vector<double> boosts{ 0., 50., 100. };
    return boosts;
}

DynamicPrintConfig curved_top_result(int speed, int fan)
{
    if (speed < 0 || speed >= int(top_speed_factors().size()) || fan < 0 || fan >= int(top_fan_boosts().size()))
        throw RuntimeError(_u8L("Pick one of the printed tiles."));
    DynamicPrintConfig out;
    out.set_key_value("nonplanar_top_speed_factor", new ConfigOptionPercent(top_speed_factors()[size_t(speed)]));
    out.set_key_value("nonplanar_top_fan_boost", new ConfigOptionPercent(top_fan_boosts()[size_t(fan)]));
    return out;
}

Plate make_plate(Test test, const DynamicPrintConfig &config, const HeadEnvelope &head)
{
    const BoundingBoxf bed = bed_box(config);
    const double       gap = layer_height(config);
    Plate              plate;
    std::vector<Footprint> shapes;
    std::vector<int>       cols;
    std::vector<int>       cells;
    switch (test) {
    case Test::Clearance:
    case Test::Verifier: {
        plate.config.set_key_value("nonplanar_head", new ConfigOptionString(ProbeHeadId));
        for (size_t k = 0; k < clearance_steps().size(); ++k) {
            const ClearanceStep &step = clearance_steps()[k];
            const double foot = cone_foot_radius(step), pad = foot + PadFlange;
            const int    rings = std::max(12, int(std::ceil(pad / 0.25)));
            PlateObject  object;
            object.name = (boost::format(_u8L("Cone %1%: %2% deg, %3% mm")) % (k + 1) % step.slope_deg % step.relief).str();
            object.mesh = TriangleMesh(disk_solid(pad, rings, 128, [&](double r) { return PadHeight + cone_height(step, r); }));
            plate.objects.emplace_back(std::move(object));
            shapes.push_back({ Vec2d(2. * pad, 2. * pad), step.relief });
        }
        cols = { 2, 4 };
        break;
    }
    case Test::TiltFlow: {
        const auto *opt = config.option<ConfigOptionFloats>("nonplanar_tilt_flow");
        const std::vector<double> current = opt != nullptr && opt->values.size() == tilt_slopes().size() ? opt->values :
                                                                                                          std::vector<double>(tilt_slopes().size(), 1.);
        for (double slope : tilt_slopes())
            for (double m : tilt_multipliers()) {
                const double t     = std::tan(Geometry::deg2rad(slope));
                const double x0 = -0.5 * PatchWidth - PadFlange, x1 = -x0, y0 = -0.5 * PatchLength - PadFlange, y1 = -y0;
                PlateObject  object;
                object.name = (boost::format(_u8L("Tilt %1% deg, flow %2%")) % slope % format_number(m, 2)).str();
                object.mesh = TriangleMesh(grid_solid(x0, x1, y0, y1, int(std::ceil((x1 - x0) / 0.25)), int(std::ceil((y1 - y0) / 0.25)),
                    [&](double x, double y) {
                        if (std::abs(x) > 0.5 * PatchWidth || std::abs(y) > 0.5 * PatchLength)
                            return PadHeight;
                        const double u = 2. * x / PatchWidth;
                        return PadHeight + (y + 0.5 * PatchLength) * t + PatchCrown * (1. - u * u);
                    }));
                std::vector<double> curve = current;
                for (double &f : curve)
                    f = std::clamp(f * m, 0.5, 1.5);
                object.config.set_key_value("nonplanar_tilt_flow", new ConfigOptionFloats(curve));
                plate.objects.emplace_back(std::move(object));
                shapes.push_back({ Vec2d(x1 - x0, y1 - y0), PatchLength * t + PatchCrown });
            }
        cols = { int(tilt_multipliers().size()) };
        break;
    }
    case Test::CurvedTop: {
        for (size_t fi = 0; fi < top_fan_boosts().size(); ++fi)
            for (size_t si = 0; si < top_speed_factors().size(); ++si) {
                const double fan = top_fan_boosts()[fi], speed = top_speed_factors()[si];
                const double half = SaddleHalf + PadFlange;
                shapes.push_back({ Vec2d(4. * half + 1., 2. * half), std::max(DomeHeight, 2. * SaddleAmplitude) });
                if (fi != 0 && si + 1 != top_speed_factors().size())
                    continue;
                cells.push_back(int(shapes.size()) - 1);
                indexed_triangle_set dome = disk_solid(DomeRadius + PadFlange, 48, 128, [](double r) {
                    if (r >= DomeRadius)
                        return PadHeight;
                    const double R = (DomeRadius * DomeRadius + DomeHeight * DomeHeight) / (2. * DomeHeight);
                    return PadHeight + std::sqrt(R * R - r * r) - (R - DomeHeight);
                });
                translate(dome, Vec3f(float(-half - 0.5), 0.f, 0.f));
                indexed_triangle_set saddle = grid_solid(-half, half, -half, half, int(std::ceil(2. * half / 0.25)), int(std::ceil(2. * half / 0.25)),
                    [](double x, double y) {
                        if (std::abs(x) > SaddleHalf || std::abs(y) > SaddleHalf)
                            return PadHeight;
                        const double u = x / SaddleHalf, v = y / SaddleHalf;
                        const double fade = std::min({ 1., 4. * (1. - std::abs(u)), 4. * (1. - std::abs(v)) });
                        return PadHeight + fade * (SaddleAmplitude + SaddleAmplitude * (u * u - v * v));
                    });
                translate(saddle, Vec3f(float(half + 0.5), 0.f, 0.f));
                its_merge(dome, saddle);
                PlateObject object;
                object.name = (boost::format(_u8L("Top speed %1%, fan boost %2%")) % (format_number(speed, 0) + "%") % (format_number(fan, 0) + "%")).str();
                object.mesh = TriangleMesh(dome);
                object.config.set_key_value("nonplanar_top_speed_factor", new ConfigOptionPercent(speed));
                object.config.set_key_value("nonplanar_top_fan_boost", new ConfigOptionPercent(fan));
                plate.objects.emplace_back(std::move(object));
            }
        plate.config.set_key_value("nonplanar_road_cooling", new ConfigOptionBool(true));
        cols = { int(top_speed_factors().size()) };
        break;
    }
    }
    std::vector<Vec2d> at;
    try {
        at = cols.size() == 1 ? grid_layout(shapes, cols.front(), head, gap, bed) : best_layout(shapes, { cols[0], cols[1] }, head, gap, bed);
    } catch (const RuntimeError &) {
        if (test != Test::TiltFlow)
            throw;
        const size_t           m = tilt_multipliers().size(), n = tilt_slopes().size();
        static const int       column_of[] = { 1, 3, 0, 2, 4 };
        std::vector<Footprint> transposed(shapes.size());
        cells.assign(shapes.size(), 0);
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < m; ++j) {
                const size_t cell = j * n + size_t(column_of[i]);
                transposed[cell]  = shapes[i * m + j];
                cells[i * m + j]  = int(cell);
            }
        at = grid_layout(transposed, int(n), head, gap, bed);
    }
    for (size_t i = 0; i < plate.objects.size(); ++i) {
        PlateObject &object = plate.objects[i];
        const BoundingBoxf3 box = object.mesh.bounding_box();
        object.mesh.translate(-float(box.center().x()), -float(box.center().y()), -float(box.min.z()));
        object.center = at[cells.empty() ? i : size_t(cells[i])];
        object.config.set_key_value("nonplanar_mode", new ConfigOptionEnum<NonplanarMode>(NonplanarMode::Auto));
        object.config.set_key_value("ironing_type", new ConfigOptionEnum<IroningType>(IroningType::NoIroning));
        object.config.set_key_value("nonplanar_ironing", new ConfigOptionBool(false));
    }
    return plate;
}

void add_to_model(const Plate &plate, Model &model)
{
    for (const PlateObject &src : plate.objects) {
        ModelObject *object = model.add_object();
        object->name        = src.name;
        ModelVolume *volume = object->add_volume(TriangleMesh(src.mesh), ModelVolumeType::MODEL_PART, false);
        volume->name        = src.name;
        object->config.assign_config(src.config);
        ModelInstance *instance = object->add_instance();
        instance->set_offset(Vec3d(src.center.x(), src.center.y(), 0.));
        object->invalidate_bounding_box();
    }
}

}
