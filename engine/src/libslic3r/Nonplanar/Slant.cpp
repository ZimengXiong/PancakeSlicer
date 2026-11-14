#include "Slant.hpp"
#include "HeadEnvelope.hpp"
#include "Multiplanar.hpp"
#include "libslic3r/GCode/ClearanceRepair.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace Slic3r::Nonplanar {

namespace {
constexpr double CutBand   = 1e-4;
constexpr double CutMinGap = 0.1;
}

SlantField::SlantField(const SlantShape &shape) : m_s(shape)
{
    m_k0  = (m_s.start - m_s.first) / m_s.h;
    m_dt  = m_s.tilt / std::max(1, m_s.morph);
    m_low = m_s.h - 0.5 * m_dt * m_s.width;
    if (m_s.gap_hi > m_s.gap_lo)
        m_low = std::max(m_s.gap_lo, std::min(m_low, m_s.gap_hi - m_dt * m_s.width));
    if (this->cut())
        m_kmax = int(std::floor((m_s.start + m_s.tilt * m_s.width - m_s.first) / m_s.h + 1e-9)) + 1;
}

double SlantField::s_of(const Vec2d &xy) const { return std::clamp(m_s.u_max - m_s.u.dot(xy), 0., m_s.width); }

double SlantField::plane(double s) const { return std::max(m_s.first, m_s.start + m_s.tilt * s); }

double SlantField::z_of(double k, double s) const
{
    if (this->cut()) {
        const double plane = this->plane(s);
        const double fr    = std::floor((plane - m_s.first) / m_s.h + 1e-9);
        const double flat  = m_s.first + fr * m_s.h;
        if (k <= fr)
            return m_s.first + k * m_s.h;
        if (k <= fr + 1.)
            return flat + (k - fr) * (plane - flat);
        if (k <= m_kmax)
            return plane + CutBand * (k - fr - 1.);
        return plane + CutBand * (m_kmax - fr - 1.) + (k - m_kmax) * m_s.h;
    }
    if (k <= m_k0)
        return m_s.first + k * m_s.h;
    const double j = std::min(k - m_k0, double(m_s.morph));
    const double z = m_s.start + j * (m_low + m_dt * s);
    return k - m_k0 <= m_s.morph ? z : z + (k - m_k0 - m_s.morph) * m_s.h;
}

double SlantField::phi(const Vec3d &p) const
{
    const double s = s_of(p.head<2>()), z = p.z();
    double       k;
    if (this->cut()) {
        const double plane = this->plane(s);
        const double fr    = std::floor((plane - m_s.first) / m_s.h + 1e-9);
        const double flat  = m_s.first + fr * m_s.h, band = CutBand * (m_kmax - fr - 1.);
        if (z <= flat)
            k = (z - m_s.first) / m_s.h;
        else if (z <= plane)
            k = fr + (z - flat) / (plane - flat);
        else if (z <= plane + band)
            k = fr + 1. + (z - plane) / CutBand;
        else
            k = m_kmax + (z - plane - band) / m_s.h;
        return m_s.first + k * m_s.h;
    }
    if (z <= m_s.start)
        k = (z - m_s.first) / m_s.h;
    else if (const double top = z_of(m_k0 + m_s.morph, s); z <= top)
        k = m_k0 + (z - m_s.start) / (m_low + m_dt * s);
    else
        k = m_k0 + m_s.morph + (z - top) / m_s.h;
    return m_s.first + k * m_s.h;
}

std::optional<double> SlantField::cut_limit(int k) const
{
    if (!this->cut() || k < 1 || k > m_kmax || m_s.tilt <= 0.)
        return std::nullopt;
    const double s_k = (m_s.first + (k - 1) * m_s.h + CutMinGap * m_s.h - m_s.start) / m_s.tilt;
    if (s_k <= 0.)
        return std::nullopt;
    return m_s.u_max - s_k;
}

double SlantField::z_at(const Vec2d &xy, double level) const { return z_of((level - m_s.first) / m_s.h, s_of(xy)); }

namespace {

constexpr double MinTilt    = 2. * M_PI / 180.;
constexpr double SlopeMargin = 3. * M_PI / 180.;
constexpr int    Directions = 8;
constexpr double StepWeight = 0.5;

double slant_budget(const HeadEnvelope &head, double psi, double width, double h)
{
    const double rise = psi + M_PI, tol = std::max(0., head.contact_tolerance);
    double       tilt = head.max_slope(rise, h + tol, width);
    HeadEnvelope body = head;
    body.boxes.erase(std::remove_if(body.boxes.begin(), body.boxes.end(), [](const HeadBox &b) { return b.nozzle; }), body.boxes.end());
    if (!body.boxes.empty())
        tilt = std::min(tilt, body.max_slope(rise, tol, width));
    if (const double gantry = head.gantry_clearance(); std::isfinite(gantry))
        tilt = std::min(tilt, std::atan(std::max(0., gantry + tol - head.margin) / width));
    return tilt - SlopeMargin;
}

std::string slant_budget_part(const HeadEnvelope &head, double psi, double width, double h)
{
    const double rise = psi + M_PI, tol = std::max(0., head.contact_tolerance);
    double       best = std::numeric_limits<double>::infinity();
    std::string  part;
    for (const HeadBox &box : head.boxes) {
        if (box.infinite_x || box.infinite_y)
            continue;
        HeadEnvelope one = head;
        one.boxes        = { box };
        double tilt      = one.max_slope(rise, h + tol, width);
        if (!box.nozzle)
            tilt = std::min(tilt, one.max_slope(rise, tol, width));
        if (tilt < best)
            best = tilt, part = box.name;
    }
    if (const double gantry = head.gantry_clearance(); std::isfinite(gantry) && std::atan(std::max(0., gantry + tol - head.margin) / width) < best)
        part = "gantry";
    return part;
}

struct Score
{
    double steps = 0., overhang = 0.;
    double total() const { return overhang + StepWeight * steps; }
};

Score score(const indexed_triangle_set &mesh, const LayerField &field, double h, double first, double sin_threshold)
{
    Score out;
    for (const stl_triangle_vertex_indices &t : mesh.indices) {
        const std::array<Vec3d, 3> v { mesh.vertices[t[0]].cast<double>(), mesh.vertices[t[1]].cast<double>(), mesh.vertices[t[2]].cast<double>() };
        const Vec3d  cr   = (v[1] - v[0]).cross(v[2] - v[0]);
        const double area = 0.5 * cr.norm();
        if (area <= 0.)
            continue;
        const Vec3d n = cr.normalized(), centroid = (v[0] + v[1] + v[2]) / 3.;
        const Vec3d g = field.gradient(centroid).normalized();
        const double c = n.dot(g);
        if (c < -sin_threshold) {
            if (std::max({ v[0].z(), v[1].z(), v[2].z() }) > first + h)
                out.overhang += area;
            continue;
        }
        if (c <= 0.)
            continue;
        std::array<double, 7> s;
        for (int i = 0; i < 3; ++i)
            s[i] = (field.phi(v[i]) - first) / h;
        s[3] = (field.phi(0.5 * (v[0] + v[1])) - first) / h;
        s[4] = (field.phi(0.5 * (v[1] + v[2])) - first) / h;
        s[5] = (field.phi(0.5 * (v[2] + v[0])) - first) / h;
        s[6] = (field.phi(centroid) - first) / h;
        const auto [lo, hi] = std::minmax_element(s.begin(), s.begin() + 3);
        double e2 = 1. / 12.;
        if (*hi - *lo < 1.) {
            e2 = 0.;
            for (double x : s)
                e2 += (x - std::round(x)) * (x - std::round(x)) / 7.;
        }
        out.steps += area * c * c * e2 * 12.;
    }
    return out;
}

class FlatField final : public LayerField
{
public:
    double phi(const Vec3d &p) const override { return p.z(); }
    double z_at(const Vec2d &, double level) const override { return level; }
    Vec3d  gradient(const Vec3d &) const override { return Vec3d::UnitZ(); }
};

}

std::shared_ptr<const LayerField> slant_field(const PrintObject &object, SlantChoice &choice)
{
    choice = SlantChoice{};
    const PrintObjectConfig &config = object.config();
    const PrintConfig       &print  = object.print()->config();
    const indexed_triangle_set mesh = slicing_frame_mesh(object);
    if (mesh.vertices.empty())
        return {};
    const double h      = config.layer_height.value;
    const double first  = print.initial_layer_print_height.value;
    const double sin_th = std::sin(0.5 * M_PI - Geometry::deg2rad(config.multiplanar_overhang_threshold.value));
    const double nozzle = print.nozzle_diameter.get_at(0);
    const double lo_h   = 0.2 * nozzle;
    const double hi_h   = 0.8 * nozzle;
    const double range  = std::max(1e-3, hi_h - lo_h);
    const HeadEnvelope head = resolve_head_envelope(print);
    const bool strict = print.nonplanar_collision_check.value == NonplanarCollisionCheck::Block;
    {
        const Score planar     = score(mesh, FlatField(), h, first, sin_th);
        choice.planar_steps    = planar.steps;
        choice.planar_overhang = planar.overhang;
    }
    const double asked = config.nonplanar_slant_start_height.value;
    const bool   cut   = config.nonplanar_slant_morph_layers.value <= 0;
    const double start = cut && asked < 0. ? first + asked : std::max(first, asked);
    const int    step  = slant_step(*object.model_object());
    const double scale = std::pow(0.75, step);
    if (step >= SlantSteps) {
        std::ostringstream why;
        why << "its tilted layers met the print head in " << slant_step_hits(*object.model_object()) << " moves at every angle tried";
        choice.planar_limit = "access";
        choice.reason       = why.str();
        choice.steps        = choice.planar_steps;
        choice.overhang     = choice.planar_overhang;
        return {};
    }

    struct Option
    {
        SlantShape shape;
        double     budget = 0.;
        Score      s;
    };
    std::vector<Option> options;
    double              best_budget = 0.;
    const double        direction   = config.nonplanar_slant_direction.value;
    const int           n_dir       = direction >= 0. ? 1 : Directions;
    for (int k = 0; k < n_dir; ++k) {
        const double psi = direction >= 0. ? Geometry::deg2rad(direction) : 2. * M_PI * k / Directions;
        Option       o;
        SlantShape  &sh = o.shape;
        sh.u            = Vec2d(std::cos(psi), std::sin(psi));
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (const stl_vertex &v : mesh.vertices) {
            lo = std::min(lo, sh.u.dot(v.head<2>().cast<double>()));
            hi = std::max(hi, sh.u.dot(v.head<2>().cast<double>()));
        }
        sh.u_max    = hi;
        sh.width    = std::max(hi - lo, EPSILON);
        o.budget    = slant_budget(head, psi, sh.width, h);
        best_budget = std::max(best_budget, o.budget);
        const bool   user  = config.nonplanar_slant_angle.value > 0. && (!strict || Geometry::deg2rad(config.nonplanar_slant_angle.value) <= o.budget);
        const double alpha = scale * (user ? Geometry::deg2rad(config.nonplanar_slant_angle.value) : o.budget);
        if (alpha < MinTilt && !(user && alpha > 0.))
            continue;
        sh.tilt  = std::tan(alpha);
        sh.first = first;
        sh.start = cut ? std::max(start, first + h - sh.tilt * sh.width) : start;
        sh.h     = h;
        sh.morph  = config.nonplanar_slant_morph_layers.value <= 0
                        ? 0
                        : std::max(config.nonplanar_slant_morph_layers.value, int(std::ceil(sh.tilt * sh.width / range - 1e-9)));
        sh.gap_lo = lo_h;
        sh.gap_hi = hi_h;
        o.s      = score(mesh, SlantField(sh), h, first, sin_th);
        options.push_back(o);
    }
    if (options.empty()) {
        std::ostringstream why;
        why << "the print head clears no layer tilted more than " << std::lround(Geometry::rad2deg(MinTilt)) << " deg over this part (at most "
            << std::fixed << std::setprecision(1) << Geometry::rad2deg(std::max(0., best_budget)) << " deg)";
        choice.budget_deg   = Geometry::rad2deg(std::max(0., best_budget));
        choice.planar_limit = "access";
        choice.reason       = why.str();
        choice.steps        = choice.planar_steps;
        choice.overhang     = choice.planar_overhang;
        return {};
    }
    if (config.nonplanar_slant_angle.value > 0.) {
        const double asked = Geometry::deg2rad(config.nonplanar_slant_angle.value);
        if (std::any_of(options.begin(), options.end(), [asked](const Option &x) { return x.budget >= asked; }))
            options.erase(std::remove_if(options.begin(), options.end(), [asked](const Option &x) { return x.budget < asked; }), options.end());
        else {
            const double best = std::max_element(options.begin(), options.end(), [](const Option &a, const Option &b) { return a.budget < b.budget; })->budget;
            options.erase(std::remove_if(options.begin(), options.end(), [best](const Option &x) { return x.budget < best - 1e-9; }), options.end());
        }
    }
    const Option &o = *std::min_element(options.begin(), options.end(), [](const Option &a, const Option &b) {
        return a.s.total() < b.s.total() - 1e-9 || (std::abs(a.s.total() - b.s.total()) <= 1e-9 && a.shape.tilt > b.shape.tilt);
    });
    const SlantShape &sh    = o.shape;
    const auto        field = std::make_shared<const SlantField>(sh);
    const double      low   = field->low_gap(), dt = field->dt();
    double            top   = 0.;
    for (const stl_vertex &v : mesh.vertices)
        top = std::max(top, double(v.z()));
    choice.angle_deg     = Geometry::rad2deg(std::atan(sh.tilt));
    choice.direction_deg = Geometry::rad2deg(std::atan2(sh.u.y(), sh.u.x()));
    if (choice.direction_deg < 0.)
        choice.direction_deg += 360.;
    choice.budget_deg    = Geometry::rad2deg(o.budget);
    choice.requested_deg = config.nonplanar_slant_angle.value;
    const std::string part = slant_budget_part(head, std::atan2(sh.u.y(), sh.u.x()), sh.width, h);
    choice.start_mm      = sh.start;
    choice.morph_layers  = sh.morph;
    choice.lowered       = step;
    choice.min_gap_mm    = field->cut() ? CutMinGap * sh.h : low;
    choice.max_gap_mm    = field->cut() ? sh.h : low + dt * sh.width;
    choice.full_tilt_mm  = field->cut() ? sh.start : sh.start + sh.morph * low;
    choice.steps         = o.s.steps;
    choice.overhang      = o.s.overhang;
    {
        std::ostringstream why;
        why << std::fixed << std::setprecision(1);
        if (step > 0) {
            choice.limit = "steps";
            why << "its tilted layers met the print head, so the angle stepped down " << step << (step == 1 ? " time" : " times");
        } else if (choice.angle_deg + 0.05 < choice.requested_deg) {
            choice.limit = "head";
            why << "the print head clears no layer steeper than " << choice.budget_deg << " deg in this direction (" << part << ")";
        } else if (choice.angle_deg > choice.budget_deg + 0.05) {
            choice.limit = "reach";
            why << "steeper than the print head clears in this direction (" << choice.budget_deg << " deg, " << part
                << "), printed as asked because Head clearance check is " << (print.nonplanar_collision_check.value == NonplanarCollisionCheck::Off ? "Off" : "Warn");
        }
        if (choice.full_tilt_mm > top) {
            const double reached = field->cut() ? 0. : Geometry::rad2deg(std::atan(dt * std::max(0., top - sh.start) / (low + 0.5 * dt * sh.width)));
            if (!choice.limit.empty())
                why << "; ";
            if (choice.limit.empty())
                choice.limit = "height";
            why << "the part ends at " << top << " mm, before the full tilt at " << choice.full_tilt_mm << " mm, so its top layers lean "
                << reached << " deg";
        }
        choice.limit_reason = why.str();
    }
    BOOST_LOG_TRIVIAL(info) << "Slant " << object.model_object()->name << ": " << choice.angle_deg << " deg toward " << choice.direction_deg
                            << " deg (head limit " << choice.budget_deg << "), flat to " << choice.start_mm << " mm, " << choice.morph_layers
                            << " morph layers (gaps " << choice.min_gap_mm << ".." << choice.max_gap_mm << " mm), full tilt from "
                            << choice.full_tilt_mm << " mm at the low edge; stair-step " << choice.steps << " mm2 against "
                            << choice.planar_steps << " flat, overhang " << choice.overhang << " against " << choice.planar_overhang;
    return field;
}

}
