#include "ConformalEmit.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "ClearanceVerifier.hpp"

#include "libslic3r/Nonplanar/ShellSeams.hpp"

#include "libslic3r/Circle.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolKinematics.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/Nonplanar/TiltBudget.hpp"
#include "libslic3r/libslic3r.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace Slic3r::Nonplanar {

namespace {
constexpr float TagStep = 0.002f;
}

std::vector<ConformalPoint> conformal_points(const ExtrusionPathConformal &path, const ToWriterFrame &to_writer)
{
    const Points3              &pts = path.polyline.points;
    std::vector<ConformalPoint> out(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        out[i].pose.tip     = to_writer(pts[i]);
        out[i].pose.axis    = i < path.tool_axis.size() ? Vec3d(path.tool_axis[i].cast<double>().normalized()) : Vec3d::UnitZ();
        out[i].bead_height  = i < path.bead_height.size() ? path.bead_height[i] : path.height;
        out[i].width        = path.bead_width.size() == pts.size() ? path.bead_width[i] : 0.f;
        if (out[i].bead_height <= 0.f && i > 0 && i + 1 < pts.size())
            out[i].pose.tip += GlideLift * out[i].pose.axis;
    }
    return out;
}

namespace {

double printed_length(const Vec3d &a, const Vec3d &b)
{
    return (Vec3d((b * 1000.).array().round() / 1000.) - Vec3d((a * 1000.).array().round() / 1000.)).norm();
}

double axis_limited_speed(const Vec3d &a, const Vec3d &b, double v, const MotionLimits &limits)
{
    const Vec3d  d = (Vec3d(GCodeFormatter::quantize_xyzf(b.x()), GCodeFormatter::quantize_xyzf(b.y()), GCodeFormatter::quantize_xyzf(b.z())) -
                      Vec3d(GCodeFormatter::quantize_xyzf(a.x()), GCodeFormatter::quantize_xyzf(a.y()), GCodeFormatter::quantize_xyzf(a.z())));
    const double l = d.norm();
    if (l < EPSILON)
        return v;
    if (limits.max_v_z > 0. && std::abs(d.z()) > EPSILON)
        v = std::min(v, limits.max_v_z * l / std::abs(d.z()));
    const double xy = std::max(std::abs(d.x()), std::abs(d.y()));
    if (limits.max_v_xy > 0. && xy > EPSILON)
        v = std::min(v, limits.max_v_xy * l / xy);
    return v;
}

}

ConformalPoint interpolate(const ConformalPoint &a, const ConformalPoint &b, double t)
{
    ConformalPoint p;
    p.pose.tip    = a.pose.tip + t * (b.pose.tip - a.pose.tip);
    p.pose.axis   = (a.pose.axis + t * (b.pose.axis - a.pose.axis)).normalized();
    p.bead_height = a.bead_height + float(t) * (b.bead_height - a.bead_height);
    p.vertical_height = a.vertical_height + float(t) * (b.vertical_height - a.vertical_height);
    p.width       = a.width + float(t) * (b.width - a.width);
    return p;
}

double planner_segment_rate(double configured, GCodeFlavor flavor, bool bbl_printer)
{
    if (configured > 0.)
        return configured;
    if (flavor == gcfKlipper)
        return 400.;
    if (flavor == gcfRepRapFirmware)
        return 250.;
    if (bbl_printer)
        return 200.;
    if (flavor == gcfMarlinFirmware)
        return 120.;
    return 100.;
}

constexpr double ConditionWidthTolerance = 0.02;

void condition_path(std::vector<ConformalPoint> &points, const ConditionParams &params)
{
    const size_t n = points.size();
    if (n < 3)
        return;

    auto chord_ok = [&](size_t i, size_t j) {
        const Vec3d  a = points[i].pose.tip, b = points[j].pose.tip;
        const Vec3d  d = b - a;
        const double len = d.norm();
        const bool extruded = points[i].bead_height > 0.f;
        for (size_t k = i + 1; k <= j; ++k)
            if ((points[k].bead_height > 0.f) != extruded)
                return false;
        if (std::abs(d.z()) > 1e-4 && len > params.max_length)
            return false;
        if (std::acos(std::clamp(points[i].pose.axis.dot(points[j].pose.axis), -1., 1.)) > params.axis_tolerance)
            return false;
        const bool   loose = len < params.target_length;
        const double tol   = loose ? params.min_tolerance : params.tolerance;
        const double ztol  = extruded ? (loose ? params.min_z_tolerance : params.z_tolerance) : params.glide_z_tolerance;
        const Vec2d  dxy   = d.head<2>();
        const double l2    = dxy.squaredNorm();
        for (size_t k = i + 1; k < j; ++k) {
            const Vec3d &p = points[k].pose.tip;
            const double t = l2 > 0. ? std::clamp((p - a).head<2>().dot(dxy) / l2, 0., 1.) : 0.;
            const Vec3d  q = a + t * d;
            if ((p - q).head<2>().norm() > tol || std::abs(p.z() - q.z()) > ztol)
                return false;
            const double bead = (1. - t) * points[i].bead_height + t * points[j].bead_height;
            if (std::abs(points[k].bead_height - bead) > params.bead_tolerance)
                return false;
            const Vec3d axis = ((1. - t) * points[i].pose.axis + t * points[j].pose.axis).normalized();
            if (std::acos(std::clamp(axis.dot(points[k].pose.axis), -1., 1.)) > params.axis_tolerance)
                return false;
            if (std::abs(double(points[k].width) - ((1. - t) * double(points[i].width) + t * double(points[j].width))) > ConditionWidthTolerance)
                return false;
        }
        return true;
    };
    std::vector<ConformalPoint> out;
    out.reserve(n);
    out.push_back(points.front());
    for (size_t i = 0; i + 1 < n;) {
        size_t j = i + 1;
        while (j + 1 < n && chord_ok(i, j + 1))
            ++j;
        out.push_back(points[j]);
        i = j;
    }
    points.swap(out);
}

namespace {

struct FittedMove
{
    size_t begin = 0, end = 0;
    bool   arc = false, ccw = false;
    Vec2d  center = Vec2d::Zero();
};

std::vector<FittedMove> single_moves(size_t segments)
{
    std::vector<FittedMove> moves(segments);
    for (size_t i = 0; i < segments; ++i) {
        moves[i].begin = i;
        moves[i].end   = i + 1;
    }
    return moves;
}

double swept_angle(const Vec2d &a, const Vec2d &b, const Vec2d &c, bool ccw)
{
    const Vec2d u = a - c, w = b - c;
    double      angle = std::atan2(u.x() * w.y() - u.y() * w.x(), u.dot(w));
    if (!ccw)
        angle = -angle;
    return angle <= 0. ? angle + 2. * PI : angle;
}

double helix_length(const Vec3d &a, const Vec3d &b, const Vec2d &center, bool ccw)
{
    const double arc = (a.head<2>() - center).norm() * swept_angle(a.head<2>(), b.head<2>(), center, ccw);
    return std::sqrt(arc * arc + (b.z() - a.z()) * (b.z() - a.z()));
}

double helix_limited_speed(const Vec3d &a, const Vec3d &b, double length, double v, const MotionLimits &limits)
{
    const double dz = GCodeFormatter::quantize_xyzf(b.z()) - GCodeFormatter::quantize_xyzf(a.z());
    if (length < EPSILON)
        return v;
    if (limits.max_v_z > 0. && std::abs(dz) > EPSILON)
        v = std::min(v, limits.max_v_z * length / std::abs(dz));
    const double xy = std::sqrt(std::max(0., length * length - dz * dz));
    if (limits.max_v_xy > 0. && xy > EPSILON)
        v = std::min(v, limits.max_v_xy * length / xy);
    return v;
}

std::vector<FittedMove> fit_moves(const std::vector<ConformalPoint> &points, const std::vector<double> &volume,
                                  const std::vector<double> &prime, const std::vector<double> &lift, const std::vector<double> &speed,
                                  double tolerance, const ConditionParams &condition)
{
    const size_t n = points.size() - 1;
    enum Kind { Road, Glide, Other };
    std::vector<Kind>   kind(n);
    std::vector<double> length(n);
    for (size_t k = 0; k < n; ++k) {
        const ConformalPoint &a = points[k], &b = points[k + 1];
        length[k] = (b.pose.tip - a.pose.tip).norm();
        kind[k]   = length[k] < ConformalMinSegment * 0.1 || prime[k] > 0. || lift[k] != 0. || lift[k + 1] != 0. ? Other :
                    a.bead_height > 0.f && b.bead_height > 0.f           ? Road :
                    a.bead_height <= 0.f && b.bead_height <= 0.f         ? Glide : Other;
    }
    auto joins = [&](size_t first, size_t k) {
        if (kind[k] != kind[first] || kind[k] == Other || speed[k] != speed[first])
            return false;
        if (kind[k] == Glide)
            return true;
        const double height  = 0.5 * double(points[first].bead_height + points[first + 1].bead_height);
        const double density = volume[first] / length[first];
        return std::abs(volume[k] / length[k] - density) <= density * condition.bead_tolerance / height;
    };
    auto z_tolerance = [&](size_t first) { return kind[first] == Glide ? condition.glide_z_tolerance : condition.z_tolerance; };
    auto line_fits = [&](size_t i, size_t j) {
        const Vec3d  a = points[i].pose.tip, d = points[j].pose.tip - a;
        const double l2 = d.head<2>().squaredNorm();
        if (l2 <= EPSILON)
            return false;
        double last = 0.;
        for (size_t k = i + 1; k < j; ++k) {
            const Vec3d &p = points[k].pose.tip;
            const double t = (p - a).head<2>().dot(d.head<2>()) / l2;
            const Vec3d  q = a + t * d;
            if (t < last || t > 1. || (p - q).head<2>().norm() > tolerance || std::abs(p.z() - q.z()) > z_tolerance(i))
                return false;
            last = t;
        }
        return true;
    };
    Points xy;
    auto arc_fits = [&](size_t i, size_t j, FittedMove &move) {
        xy.clear();
        double polyline = 0.;
        for (size_t k = i; k <= j; ++k) {
            xy.emplace_back(Point::new_scale(points[k].pose.tip.x(), points[k].pose.tip.y()));
            if (k > i)
                polyline += (xy.back() - xy[xy.size() - 2]).cast<double>().norm();
        }
        ArcSegment arc;
        if (!ArcSegment::try_create_arc(xy, arc, polyline, DEFAULT_SCALED_MAX_RADIUS, scaled<double>(tolerance)) || arc.angle_radians > PI)
            return false;
        const Vec2d  c   = unscaled(arc.center);
        const bool   ccw = arc.direction == ArcDirection::Arc_Dir_CCW;
        const Vec3d &a = points[i].pose.tip, &b = points[j].pose.tip;
        const double total = swept_angle(a.head<2>(), b.head<2>(), c, ccw);
        double       last  = 0.;
        for (size_t k = i + 1; k < j; ++k) {
            const Vec3d &p = points[k].pose.tip;
            const double t = swept_angle(a.head<2>(), p.head<2>(), c, ccw) / total;
            if (t < last || t > 1. || std::abs(p.z() - (a.z() + t * (b.z() - a.z()))) > z_tolerance(i))
                return false;
            last = t;
        }
        move.arc    = true;
        move.ccw    = ccw;
        move.center = c;
        return true;
    };

    std::vector<FittedMove> moves;
    for (size_t i = 0; i < n;) {
        auto       open = [&](size_t k) { return k < n && joins(i, k); };
        FittedMove line{ i, i + 1 };
        while (open(line.end) && line_fits(i, line.end + 1))
            ++line.end;
        FittedMove arc{ i, i + 1 }, candidate{ i, i + 2 };
        for (; open(candidate.end - 1) && arc_fits(i, candidate.end, candidate); ++candidate.end)
            arc = candidate;
        moves.emplace_back(arc.end > line.end ? arc : line);
        i = moves.back().end;
    }
    return moves;
}

}

double emit_conformal_extrusion(GCodeWriter &writer, const std::vector<ConformalPoint> &points_in, double width,
                                const ConformalEmitParams &params, float &last_height, std::string &out,
                                const DepositFn &deposit)
{
    auto unwritten = [&writer, &params]() {
        if (params.prime > 0. && writer.filament() != nullptr)
            writer.filament()->give_back_retraction(params.prime);
        return 0.;
    };
    if (points_in.size() < 2)
        return unwritten();
    std::vector<ConformalPoint> conditioned;
    if (params.condition.target_length > 0.) {
        conditioned = points_in;
        condition_path(conditioned, params.condition);
    }
    const std::vector<ConformalPoint> &merged = params.condition.target_length > 0. ? conditioned : points_in;
    const bool rotary = writer.tool_kinematics().rotary_count() > 0;
    std::vector<double>                flow;
    std::vector<ConformalPoint>        shaped;
    const bool shape = params.ramp_length > 0. || params.taper_length > 0. || params.coast_length > 0.;
    if (shape)
        shaped = shape_runs(merged, { params.ramp_length, params.taper_length, params.coast_length, params.open_end, params.scored },
                            params.condition.target_length, flow);
    const std::vector<ConformalPoint> &staged = shape ? shaped : merged;

    constexpr double            StubLength = 0.01;
    std::vector<ConformalPoint> points;
    if (params.preserve_points)
        points = staged;
    else {
        std::vector<double>         stub_flow;
        points.reserve(staged.size());
        points.emplace_back(staged.front());
        for (size_t i = 1; i < staged.size(); ++i) {
            const bool last = i + 1 == staged.size();
            if ((staged[i].pose.tip - points.back().pose.tip).norm() >= StubLength) {
                points.emplace_back(staged[i]);
                if (!flow.empty())
                    stub_flow.emplace_back(flow[i - 1]);
            } else if (points.back().bead_height <= 0.f && staged[i].bead_height > 0.f) {
                points.back() = staged[i];
            } else if (points.size() > 1 &&
                       (last || (staged[i].bead_height > 0.f) != (staged[i + 1].bead_height > 0.f))) {
                points.back() = staged[i];
                if (!flow.empty())
                    stub_flow.back() = std::min(stub_flow.back(), flow[i - 1]);
            }
        }
        if (points.size() < 2) {
            points.emplace_back(staged.back());
            if (!flow.empty())
                stub_flow.emplace_back(flow.back());
        }
        flow.swap(stub_flow);

        {
            std::vector<ConformalPoint> moves{ points.front() };
            std::vector<double>         move_flow;
            for (size_t i = 0; i + 1 < points.size(); ++i) {
                const ConformalPoint &a = moves.back();
                const ConformalPoint &b = points[i + 1];
                const double          l = (b.pose.tip - a.pose.tip).norm();
                if (l < EPSILON || (l < ConformalMinSegment && i + 2 < points.size() && (points[i + 2].bead_height > 0.f) == (b.bead_height > 0.f) &&
                                    (a.bead_height > 0.f) == (b.bead_height > 0.f)))
                    continue;
                if (!flow.empty())
                    move_flow.emplace_back(flow[i]);
                moves.emplace_back(b);
            }
            if (moves.size() < 2)
                return unwritten();
            points.swap(moves);
            flow.swap(move_flow);
        }

    }

    std::vector<Vec3d> tips(points.size());
    for (size_t i = 0; i < points.size(); ++i)
        tips[i] = (points[i].pose.tip * 1000.).array().round() / 1000.;
    auto glide = [&points](size_t i) { return points[i].bead_height <= 0.f && points[i + 1].bead_height <= 0.f; };
    std::vector<double> nominal(points.size() - 1, params.nominal_speed);
    std::vector<bool>   glides(points.size() - 1, false);
    for (size_t i = 0; i < nominal.size(); ++i)
        if (glide(i)) {
            nominal[i] = std::max(params.nominal_speed, params.glide_speed);
            glides[i]  = true;
        }

    std::vector<double> prime(points.size() - 1, 0.);
    auto primes = [&](size_t i) {
        return points[i].bead_height > 0.f && points[i + 1].bead_height > 0.f && (points[i + 1].pose.tip - points[i].pose.tip).norm() >= 0.05;
    };
    if (params.prime > 0.) {
        double road = 0.;
        for (size_t i = 0; i + 1 < points.size(); ++i)
            if (primes(i))
                road += (points[i + 1].pose.tip - points[i].pose.tip).norm();
        const double length = writer.get_gcode_flavor() == gcfKlipper ? 0. : std::min(road, std::max(params.prime_length, EPSILON));
        if (length <= EPSILON) {
            if (params.prime_speed > 0.) {
                if (rotary)
                    writer.set_pose_speed(params.prime_speed * 60.);
                else
                    writer.set_speed(out, params.prime_speed * 60.);
            }
            writer.extrude_to_pose(out, points.front().pose, params.prime, "prime");
        }
        else {
            double done = 0.;
            for (size_t i = 0; i + 1 < points.size() && done < length; ++i)
                if (primes(i)) {
                    const double run = std::min((points[i + 1].pose.tip - points[i].pose.tip).norm(), length - done);
                    prime[i] = params.prime * run / length;
                    done += run;
                }
        }
    }

    std::vector<double> seg_volume(points.size() - 1, 0.), seg_width(points.size() - 1, width);
    std::vector<double> lift(points.size(), 0.);
    double coast_floor = -1., coast_dz = 0.;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const ConformalPoint &a = points[i], &b = points[i + 1];
        if ((b.pose.tip - a.pose.tip).norm() < EPSILON)
            continue;
        if (a.width > 0.f && b.width > 0.f)
            seg_width[i] = 0.5 * (double(a.width) + double(b.width));
        const Vec3d axis = (a.pose.axis + b.pose.axis).normalized();
        double     &v    = seg_volume[i];
        v = a.bead_height <= 0.f || b.bead_height <= 0.f ? 0. :
            (a.vertical_height > 0.f && b.vertical_height > 0.f ?
                 segment_volume(a.pose.tip, b.pose.tip, seg_width[i], a.vertical_height, b.vertical_height) :
                 segment_volume(a.pose.tip, b.pose.tip, seg_width[i], a.bead_height, b.bead_height, axis)) *
                (flow.empty() ? 1. : flow[i]);
        if (!rotary && params.clearance && v == 0. && prime[i] == 0. && a.bead_height > 0.f && b.bead_height > 0.f) {
            const Vec3d up = Vec3d::UnitZ();
            if (coast_floor == -1.) {
                coast_floor = std::min(params.margin, params.clearance(a.pose.tip, up)) - 1e-3;
                coast_dz    = 0.;
            }
            const Vec3d from  = a.pose.tip + Vec3d(0., 0., coast_dz);
            const int   steps = std::max(1, int(std::ceil((b.pose.tip - a.pose.tip).norm() / 0.25)));
            for (int pass = 0; pass < 4; ++pass) {
                const Vec3d to   = b.pose.tip + Vec3d(0., 0., coast_dz);
                double      need = 0.;
                for (int k = 1; k <= steps; ++k) {
                    const double t = double(k) / steps;
                    need = std::max(need, (coast_floor - params.clearance(from + t * (to - from), up)) / t);
                }
                if (need <= 0.)
                    break;
                coast_dz += std::min(need + 0.005, 1.);
            }
            lift[i + 1] = coast_dz;
        } else
            coast_floor = -1., coast_dz = 0.;
    }

    const std::vector<double> caps   = params.speed_caps ? params.speed_caps(points) : std::vector<double>();
    std::vector<double> speeds = limit_feedrates(tips, nominal, params.limits, 0.05, caps.empty() ? nullptr : &caps, &glides);
    std::vector<FittedMove> moves = single_moves(points.size() - 1);
    auto move_length = [&points](const FittedMove &m) {
        if (!m.arc)
            return (points[m.end].pose.tip - points[m.begin].pose.tip).norm();
        return helix_length(points[m.begin].pose.tip, points[m.end].pose.tip, m.center, m.ccw);
    };
    if (params.arc_tolerance > 0. && !rotary) {
        moves = fit_moves(points, seg_volume, prime, lift, speeds, params.arc_tolerance, params.condition);
        std::vector<double> block_length(points.size() - 1);
        for (const FittedMove &m : moves)
            std::fill(block_length.begin() + m.begin, block_length.begin() + m.end, move_length(m));
        speeds = limit_feedrates(tips, nominal, params.limits, 0.05, caps.empty() ? nullptr : &caps, &glides, &block_length);
    }
    if (params.speed_scale < 1.)
        for (size_t i = 0; i < speeds.size(); ++i)
            if (!glides[i])
                speeds[i] *= params.speed_scale;

    const std::vector<std::string> tags = params.segment_tags ? params.segment_tags(points, speeds) : std::vector<std::string>();
    if (!tags.empty()) {
        std::vector<FittedMove> split;
        split.reserve(moves.size());
        for (const FittedMove &m : moves) {
            size_t begin = m.begin;
            for (size_t k = m.begin + 1; k <= m.end; ++k)
                if (k == m.end || !tags[k].empty()) {
                    FittedMove piece = m;
                    piece.begin      = begin;
                    piece.end        = k;
                    piece.arc        = m.arc && k - begin > 1;
                    split.emplace_back(piece);
                    begin = k;
                }
        }
        moves.swap(split);
    }

    double volume = 0.;
    char   buf[64];
    float last_vertical_height = -1.f;
    bool   speed_block_open = ! params.speed_marker.empty();
    for (const FittedMove &m : moves) {
        const size_t          i = m.begin;
        const ConformalPoint &a = points[i];
        const ConformalPoint &b = points[m.end];
        const bool            single = m.end == i + 1;
        if (i < tags.size())
            out += tags[i];
        if (single && (b.pose.tip - a.pose.tip).norm() < EPSILON)
            continue;
        if (prime[i] == 0. && (a.bead_height <= 0.f || b.bead_height <= 0.f || (!flow.empty() && flow[i] == 0.)) &&
            m.end + 1 < points.size() && (b.pose.tip - writer.get_position()).norm() < StubLength)
            continue;
        double speed = speeds[i];
        float  height = 0.5f * (a.bead_height + b.bead_height);
        double mean_width = seg_width[i], v = seg_volume[i];
        if (!single) {
            double length = 0., h = 0., w = 0.;
            v = 0.;
            for (size_t k = i; k < m.end; ++k) {
                const double l = (points[k + 1].pose.tip - points[k].pose.tip).norm();
                length += l;
                h += l * 0.5 * (points[k].bead_height + points[k + 1].bead_height);
                w += l * seg_width[k];
                v += seg_volume[k];
                speed = std::min(speed, speeds[k]);
                if (params.max_volume_per_second > 0. && seg_volume[k] > 0.)
                    speed = std::min(speed, params.max_volume_per_second * l / seg_volume[k]);
            }
            height     = float(h / length);
            mean_width = w / length;
        }
        if (params.last_width != nullptr && std::abs(float(mean_width) - *params.last_width) > TagStep) {
            *params.last_width = float(mean_width);
            std::snprintf(buf, sizeof(buf), ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width).c_str(), *params.last_width);
            out += buf;
        }
        if (std::abs(height - last_height) > TagStep) {
            last_height = height;
            std::snprintf(buf, sizeof(buf), ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str(), last_height);
            out += buf;
        }
        const float vertical_height = a.vertical_height > 0.f && b.vertical_height > 0.f ?
                                          0.5f * (a.vertical_height + b.vertical_height) : 0.f;
        if (rotary && std::abs(vertical_height - last_vertical_height) > TagStep) {
            last_vertical_height = vertical_height;
            std::snprintf(buf, sizeof(buf), ";_NONPLANAR_VERTICAL_HEIGHT:%g\n", vertical_height);
            out += buf;
        }
        if (params.pressure_advance != nullptr && !glide(i))
            params.pressure_advance->update(height, out);
        volume += v;
        if (single && params.max_volume_per_second > 0. && v > 0.)
            speed = std::min(speed, params.max_volume_per_second * (b.pose.tip - a.pose.tip).norm() / v);
        const double dE = v * params.e_per_mm3 + prime[i];
        if (prime[i] > 0. && params.prime_speed > 0.)
            speed = std::min(speed, params.prime_speed * printed_length(a.pose.tip, b.pose.tip) / dE);
        if (params.limits.segment_rate > 0.) {
            double length = move_length(m);
            if (params.preserve_points && !rotary)
                if (const double printed = printed_length(a.pose.tip, b.pose.tip); printed > EPSILON)
                    length = std::min(length, printed);
            speed = std::min(speed, params.limits.segment_rate * length);
        }
        const double F = (rotary ? speed :
                          m.arc  ? helix_limited_speed(a.pose.tip, b.pose.tip, move_length(m), speed, params.limits) :
                                   axis_limited_speed(a.pose.tip, b.pose.tip, speed, params.limits)) * 60.;
        if (std::abs(writer.get_current_speed() - F) > EPSILON) {
            if (rotary)
                writer.set_pose_speed(F);
            else {
                const std::string &marker = glide(i) ? std::string() : params.speed_marker;
                if (marker.empty() && speed_block_open)
                    out += ";_EXTRUDE_END\n";
                speed_block_open = ! marker.empty();
                writer.set_speed(out, F, "", marker);
            }
        }
        if (m.arc)
            writer.extrude_helix_to_xyz(out, b.pose.tip, m.center, dE, m.ccw, params.comment);
        else {
            ToolPose to_pose = b.pose;
            to_pose.tip.z() += lift[m.end];
            writer.extrude_to_pose(out, to_pose, dE, params.comment);
        }
        if (deposit)
            for (size_t k = i; k < m.end; ++k)
                if (seg_volume[k] > 0.) {
                    const ConformalPoint &p = points[k], &q = points[k + 1];
                    const Vec3d side = p.pose.axis.cross(q.pose.tip - p.pose.tip);
                    const Vec3d lift = side.norm() > EPSILON ? Vec3d(0., 0., 0.5 * seg_width[k] * std::abs(side.z()) / side.norm()) : Vec3d::Zero();
                    deposit(p.pose.tip + lift, q.pose.tip + lift, seg_width[k]);
                }
    }
    return volume;
}

std::vector<double> conformal_speed_caps(const std::vector<ConformalPoint> &points, double width,
                                         const std::function<Vec2d(const Vec2d &)> &gradient, const ToolKinematics &kinematics,
                                         const JointPose &start, const Vec3d &writer_offset, const ConformalSpeedParams &params)
{
    constexpr double    inf = std::numeric_limits<double>::infinity();
    std::vector<double> caps(points.size() < 2 ? 0 : points.size() - 1, inf);
    const bool          rotary = kinematics.rotary_count() > 0;
    JointPose           prev   = start;
    static const bool twice  = research_switch("NP_JOINT_LIMIT_TWICE") != nullptr;
    bool                joints = rotary && twice;
    for (size_t i = 0; i < caps.size(); ++i) {
        const ConformalPoint &a = points[i], &b = points[i + 1];
        const double          len = (b.pose.tip - a.pose.tip).norm();
        if (len < EPSILON)
            continue;
        const Vec3d  axis   = (a.pose.axis + b.pose.axis).normalized();
        const double volume = segment_volume(a.pose.tip, b.pose.tip, width, a.bead_height, b.bead_height, axis);
        if (params.max_volumetric > 0. && volume > 0.)
            caps[i] = std::min(caps[i], params.max_volumetric * len / volume);
        if (params.reference_height > 0. && volume > 0. && gradient) {
            const Vec2d  g      = gradient(0.5 * (a.pose.tip + b.pose.tip).head<2>());
            const Vec3d  normal = Vec3d(-g.x(), -g.y(), 1.).normalized();
            const double normal_thickness = 0.5 * (a.bead_height + b.bead_height) * std::max(0., axis.dot(normal));
            const double thin = params.nominal_speed * std::clamp(normal_thickness / params.reference_height, params.min_ratio, 1.);
            caps[i] = std::min(caps[i], std::max(thin, std::min(params.min_speed, params.nominal_speed)));
        }
        if (joints) {
            JointPose next;
            if (!kinematics.inverse({ b.pose.tip - writer_offset, b.pose.axis.normalized() }, &prev, next)) {
                joints = false;
                continue;
            }
            const std::array<RotaryAxis, 2> &axes = kinematics.rotary_axes();
            for (int k = 0; k < 2; ++k)
                if (const double d = std::abs(next.rot[k] - prev.rot[k]); axes[k].max_speed_deg_s > 0. && d > 1e-9)
                    caps[i] = std::min(caps[i], axes[k].max_speed_deg_s * len / d);
            for (int k = 0; k < 3; ++k)
                if (const double d = std::abs(next.xyz[k] - prev.xyz[k]); params.max_joint_speed[k] > 0. && d > 1e-9)
                    caps[i] = std::min(caps[i], params.max_joint_speed[k] * len / d);
            prev = next;
        }
    }
    return caps;
}

bool emit_conformal_wipe(GCodeWriter &writer, const std::vector<ConformalPoint> &points, const ConformalWipeParams &params,
                         std::string &out, Vec3d &end)
{
    end = points.empty() ? writer.get_position() : points.back().pose.tip;
    if (points.size() < 2 || params.distance <= 0. || params.speed <= 0.)
        return false;
    std::vector<ConformalPoint> route{ points.back() };
    double                      length = 0.;
    for (size_t i = points.size() - 1; i > 0 && length < params.distance; --i) {
        const ConformalPoint &a = points[i], &b = points[i - 1];
        if (a.bead_height <= 0.f || b.bead_height <= 0.f)
            break;
        const double l = (b.pose.tip - a.pose.tip).norm();
        if (length + l > params.distance) {
            route.emplace_back(interpolate(a, b, (params.distance - length) / l));
            length = params.distance;
            break;
        }
        route.emplace_back(b);
        length += l;
    }
    const bool                  rotary_route = writer.tool_kinematics().rotary_count() > 0;
    std::vector<ConformalPoint> kept{ route.front() };
    for (size_t i = 1; i < route.size(); ++i) {
        const Vec3d &a = kept.back().pose.tip, &p = route[i].pose.tip;
        if ((p - a).norm() < ConformalMinSegment) {
            if (i + 1 == route.size() && kept.size() > 1)
                kept.back() = route[i];
            continue;
        }
        if (i + 1 < route.size() && ! rotary_route) {
            const Vec3d  c = route[i + 1].pose.tip - a;
            const double t = c.squaredNorm() > 0. ? std::clamp((p - a).dot(c) / c.squaredNorm(), 0., 1.) : 0.;
            if ((a + t * c - p).norm() < 0.01)
                continue;
        }
        kept.emplace_back(route[i]);
    }
    route.swap(kept);
    if (route.size() < 2)
        return false;
    const bool rotary = writer.tool_kinematics().rotary_count() > 0;
    if (params.clearance && !rotary) {
        const Vec3d &tip = writer.get_position();
        if (const double lift = tip.z() - route.front().pose.tip.z();
            lift > EPSILON && (tip.head<2>() - route.front().pose.tip.head<2>()).norm() < 1e-3)
            for (ConformalPoint &p : route)
                p.pose.tip.z() += lift;
        const Vec3d  axis  = Vec3d::UnitZ();
        const double floor = std::min(params.margin, params.clearance(route.front().pose.tip, axis)) - 1e-3;
        for (size_t i = 0; i + 1 < route.size(); ++i) {
            const Vec3d &a = route[i].pose.tip, &b = route[i + 1].pose.tip;
            const int    steps = std::max(1, int(std::ceil((b - a).norm() / 0.25)));
            bool         clear = true;
            for (int k = 1; k <= steps && clear; ++k)
                clear = params.clearance(a + (double(k) / steps) * (b - a), axis) >= floor;
            if (!clear) {
                route.resize(i + 1);
                break;
            }
        }
        if (route.size() < 2)
            return false;
    }
    length = 0.;
    for (size_t i = 0; i + 1 < route.size(); ++i)
        length += rotary ? (route[i + 1].pose.tip - route[i].pose.tip).norm() : printed_length(route[i].pose.tip, route[i + 1].pose.tip);
    if (length < ConformalMinSegment)
        return false;
    out += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start) + "\n";
    for (size_t i = 0; i + 1 < route.size(); ++i) {
        const Vec3d  d = Vec3d((route[i + 1].pose.tip * 1000.).array().round() / 1000.) -
                         Vec3d((route[i].pose.tip * 1000.).array().round() / 1000.);
        const double l = rotary ? (route[i + 1].pose.tip - route[i].pose.tip).norm() : d.norm();
        if (l < EPSILON)
            continue;
        double v = rotary ? params.speed : axis_limited_speed(route[i].pose.tip, route[i + 1].pose.tip, params.speed, params.limits);
        if (rotary && params.limits.max_v_z > 0. && std::abs(d.z()) > EPSILON)
            v = std::min(v, params.limits.max_v_z * l / std::abs(d.z()));
        if (params.max_v_e > 0. && params.retract > EPSILON)
            v = std::min(v, params.max_v_e * length / params.retract);
        if (params.limits.segment_rate > 0.)
            v = std::min(v, l * params.limits.segment_rate);
        const double F = v * 60.;
        if (i == 0 || std::abs(writer.get_current_speed() - F) > EPSILON) {
            if (rotary)
                writer.set_pose_speed(F);
            else
                writer.set_speed(out, F, "", params.speed_marker);
        }
        writer.extrude_to_pose(out, route[i + 1].pose, -params.retract * l / length, "wipe on the surface");
    }
    out += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End) + "\n";
    end = route.back().pose.tip;
    return true;
}

void deposit_material_path(MaterialColumns &columns, const ExtrusionPath &path, const ToWriterFrame &to_writer,
                           const std::function<double(double)> &z_at_fraction)
{
    const double length = z_at_fraction ? path.polyline.length() * SCALING_FACTOR : 0.;
    double distance = 0.;
    Vec3d previous;
    for (size_t i = 0; i < path.polyline.points.size(); ++i) {
        Vec3d current = to_writer(path.polyline.points[i]);
        if (i != 0) distance += (current.head<2>() - previous.head<2>()).norm();
        if (z_at_fraction) current.z() = z_at_fraction(length > 0. ? distance / length : 0.);
        if (i != 0) {
            const int count = std::max(1, int(std::ceil((current - previous).norm() / 0.25)));
            for (int k = 0; k < count; ++k)
                columns.deposit(previous + double(k) / count * (current - previous),
                                previous + double(k + 1) / count * (current - previous), path.width);
        }
        previous = current;
    }
}

bool densify_conformal_path(const ExtrusionPathConformal &path, double max_segment, ExtrusionPathConformal &out)
{
    const Points3 &pts  = path.polyline.points;
    const double   step = scale_(max_segment);
    bool           long_segment = false;
    for (size_t i = 1; i < pts.size() && ! long_segment; ++i)
        long_segment = (pts[i] - pts[i - 1]).cast<double>().norm() > step;
    if (! long_segment)
        return false;
    out = path;
    Points3            &dense_pts = out.polyline.points;
    std::vector<float> &heights   = out.bead_height;
    std::vector<Vec3f> &axes      = out.tool_axis;
    dense_pts.clear(), heights.clear(), axes.clear();
    for (size_t i = 0; i < pts.size(); ++i) {
        const float h = i < path.bead_height.size() ? path.bead_height[i] : path.height;
        if (i > 0) {
            const int n = int(std::ceil((pts[i] - pts[i - 1]).cast<double>().norm() / step));
            const float previous_height = i - 1 < path.bead_height.size() ? path.bead_height[i - 1] : path.height;
            for (int k = 1; k < n; ++k) {
                const double t = double(k) / n;
                dense_pts.emplace_back((pts[i - 1].cast<double>() + t * (pts[i] - pts[i - 1]).cast<double>()).cast<coord_t>());
                heights.emplace_back(previous_height <= 0.f || h <= 0.f ? 0.f : float((1. - t) * previous_height + t * h));
                if (! path.tool_axis.empty())
                    axes.emplace_back(((1. - t) * path.tool_axis[i - 1] + t * path.tool_axis[i]).normalized());
            }
        }
        dense_pts.emplace_back(pts[i]);
        heights.emplace_back(h);
        if (! path.tool_axis.empty())
            axes.emplace_back(path.tool_axis[i]);
    }
    return true;
}

std::vector<RotaryRun> plan_rotary_runs(const ToolKinematics &kinematics, const std::vector<ConformalPoint> &points,
                                        const JointPose &from, double max_jump_deg, bool reseed)
{
    std::vector<RotaryRun> runs;
    RotaryRun              run;
    run.seed = from;
    run.end  = points.empty() ? 0 : points.size() - 1;
    const auto *rotary = dynamic_cast<const RotaryKinematics *>(&kinematics);
    JointPose   prev;
    if (rotary == nullptr || points.size() < 2 || ! kinematics.inverse(points.front().pose, &from, prev))
        return { run };
    auto other_branch = [rotary](const JointPose &j) {
        const std::array<double, 2> kin = rotary->kinematic_angles(j);
        JointPose                   out = j;
        out.rot[0] = rotary->machine_angle(0, -kin[0]);
        out.rot[1] = rotary->machine_angle(1, kin[1] + M_PI);
        const RotaryAxis &axis = rotary->rotary_axes()[1];
        if (!axis.continuous) {
            const double first = std::ceil((axis.min_deg - out.rot[1]) / 360.);
            const double last = std::floor((axis.max_deg - out.rot[1]) / 360.);
            if (first <= last)
                out.rot[1] += 360. * std::clamp(std::round((j.rot[1] - out.rot[1]) / 360.), first, last);
        }
        return out;
    };
    auto jump = [](const JointPose &a, const JointPose &b) { return std::max(std::abs(a.rot[0] - b.rot[0]), std::abs(a.rot[1] - b.rot[1])); };
    if (reseed && layout_is_table(kinematics.layout())) {
        JointPose other;
        const JointPose seed = other_branch(prev);
        const bool has_other = kinematics.inverse(points.front().pose, &seed, other);
        double best_cost = std::numeric_limits<double>::infinity();
        JointPose best;
        for (const JointPose &start : { prev, has_other ? other : prev }) {
            JointPose p = start;
            double cost = std::abs(start.rot[1] - from.rot[1]);
            bool continuous = true;
            for (size_t i = 1; i < points.size(); ++i) {
                JointPose q;
                if (!kinematics.inverse_for_motion(points[i].pose, p, q) || jump(p, q) > max_jump_deg) {
                    continuous = false;
                    break;
                }
                cost += std::abs(q.rot[1] - p.rot[1]);
                p = q;
            }
            if (continuous && cost < best_cost) {
                best_cost = cost;
                best = start;
            }
        }
        if (std::isfinite(best_cost)) {
            run.seed = jump(best, prev) > 1e-6 ? best : from;
            return { run };
        }
    }
    for (size_t i = 1; i < points.size(); ++i) {
        JointPose next;
        if (! kinematics.inverse_for_motion(points[i].pose, prev, next))
            break;
        if (jump(prev, next) > max_jump_deg) {
            const JointPose seed = other_branch(prev);
            JointPose       start, after;
            if (kinematics.inverse(points[i - 1].pose, &seed, start) && kinematics.inverse_for_motion(points[i].pose, start, after) &&
                jump(start, after) <= max_jump_deg) {
                if (i - 1 == run.begin)
                    run.seed = seed;
                else {
                    run.end = i - 1;
                    runs.emplace_back(run);
                    run.begin = i - 1;
                    run.seed  = seed;
                }
                next = after;
            }
        }
        prev = next;
    }
    run.end = points.size() - 1;
    runs.emplace_back(run);
    return runs;
}

bool alternate_rotary_runs(const ToolKinematics &kinematics, const std::vector<ConformalPoint> &points, const JointPose &from,
                           std::vector<RotaryRun> &runs, double max_jump_deg)
{
    if (runs.size() < 2)
        return false;
    auto jump = [](const JointPose &a, const JointPose &b) { return std::max(std::abs(a.rot[0] - b.rot[0]), std::abs(a.rot[1] - b.rot[1])); };
    std::vector<RotaryRun> out = runs;
    JointPose              prev = from;
    for (size_t k = 0; k < out.size(); ++k) {
        RotaryRun &run = out[k];
        run.reversed   = k % 2 == 1;
        const size_t n = run.end - run.begin + 1;
        for (size_t m = 0; m < n; ++m) {
            JointPose next;
            const ToolPose &pose = points[run.reversed ? run.end - m : run.begin + m].pose;
            if (!(m == 0 ? kinematics.inverse(pose, &prev, next) : kinematics.inverse_for_motion(pose, prev, next)) ||
                jump(prev, next) > max_jump_deg)
                return false;
            if (m == 0)
                run.seed = next;
            prev = next;
        }
    }
    runs = std::move(out);
    return true;
}

namespace {

Vec3d rotate_toward(const Vec3d &from, const Vec3d &to, double max_angle)
{
    const double angle = std::acos(std::clamp(from.dot(to), -1., 1.));
    if (angle <= max_angle || angle <= 0.)
        return to;
    const Vec3d axis = from.cross(to);
    if (axis.norm() < 1e-15)
        return from;
    return Eigen::AngleAxisd(max_angle, axis.normalized()) * from;
}

}

void apply_minimal_tilt(std::vector<ConformalPoint> &points, const std::function<Vec2d(const Vec2d &)> &gradient,
                        const ToolKinematics &kinematics, const MinimalTiltParams &params)
{
    if (points.empty() || kinematics.rotary_count() == 0)
        return;

    const TiltBudget   budget(kinematics, params.slope_limit, params.margin, params.max_tilt, params.max_residual, params.residual_from);
    const size_t       n = points.size();
    std::vector<Vec3d> normals(n), axes(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec2d g = gradient(points[i].pose.tip.head<2>());
        if (!g.allFinite())
            throw RuntimeError("Non-planar rotary path has no finite surface gradient. Repair the model or regenerate its non-planar paths.");
        normals[i]    = Vec3d(-g.x(), -g.y(), 1.).normalized();
        axes[i]       = budget.axis(g);
    }

    std::vector<double> s(n, 0.);
    for (size_t i = 1; i < n; ++i)
        s[i] = s[i - 1] + (points[i].pose.tip - points[i - 1].pose.tip).norm();
    if (params.smoothing > 0.) {
        std::vector<Vec3d> smoothed(n);
        size_t             lo = 0, hi = 0;
        Vec3d              sum = Vec3d::Zero();
        for (size_t i = 0; i < n; ++i) {
            while (hi < n && s[hi] <= s[i] + 0.5 * params.smoothing)
                sum += axes[hi++];
            while (s[lo] < s[i] - 0.5 * params.smoothing)
                sum -= axes[lo++];
            smoothed[i] = sum.normalized();
        }
        axes.swap(smoothed);
    }

    if (params.max_rate > 0.) {
        for (size_t i = 1; i < n; ++i)
            axes[i] = rotate_toward(axes[i - 1], axes[i], params.max_rate * (s[i] - s[i - 1]));
        for (size_t i = n - 1; i > 0; --i)
            axes[i - 1] = rotate_toward(axes[i], axes[i - 1], params.max_rate * (s[i] - s[i - 1]));
    }

    if (params.clearance) {
        auto tilt_of = [](const Vec3d &axis) { return std::acos(std::clamp(axis.z(), -1., 1.)); };
        auto shrink = [&tilt_of](const Vec3d &axis, double tilt) {
            const double full = tilt_of(axis);
            if (full <= tilt)
                return axis;
            return Vec3d(Eigen::Quaterniond::Identity().slerp(tilt / full, Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), axis)) *
                         Vec3d::UnitZ());
        };
        std::vector<Vec3d> cleared(n, Vec3d::UnitZ());
        for (int round = 0; round < 16; ++round) {
            bool changed = false;
            for (size_t i = 0; i < n; ++i) {
                if (axes[i].z() > 1. - EPSILON || axes[i] == cleared[i])
                    continue;
                const Vec3d &tip = points[i].pose.tip;
                if (params.clearance(tip, axes[i]) >= params.clearance_margin) {
                    cleared[i] = axes[i];
                    continue;
                }
                const double floor = params.clearance_margin - 1e-3;
                const Vec3d  full  = axes[i];
                axes[i]            = Vec3d::UnitZ();
                for (int k = 3; k > 0; --k) {
                    const Vec3d axis = shrink(full, 0.25 * k * tilt_of(full));
                    if (params.clearance(tip, axis) >= floor) {
                        axes[i] = axis;
                        break;
                    }
                }
                cleared[i] = axes[i];
                changed    = true;
            }
            if (! changed || params.max_rate <= 0.)
                break;
            for (size_t i = 1; i < n; ++i)
                axes[i] = shrink(axes[i], tilt_of(axes[i - 1]) + params.max_rate * (s[i] - s[i - 1]));
            for (size_t i = n - 1; i > 0; --i)
                axes[i - 1] = shrink(axes[i - 1], tilt_of(axes[i]) + params.max_rate * (s[i] - s[i - 1]));
        }
        for (size_t i = 0; i < n; ++i)
            if (axes[i] != cleared[i])
                axes[i] = Vec3d::UnitZ();
    }

    for (size_t i = 0; i < n; ++i) {
        const double along   = std::min(normals[i].z() / std::max(axes[i].dot(normals[i]), 1e-6), 1. / std::max(axes[i].z(), 1e-6));
        points[i].vertical_height = points[i].bead_height;
        if (points[i].bead_height <= 0.f && i > 0 && i + 1 < n)
            points[i].pose.tip += GlideLift * (axes[i] - points[i].pose.axis);
        points[i].pose.axis  = axes[i];
        points[i].bead_height = float(points[i].bead_height * along);
    }
}

}
