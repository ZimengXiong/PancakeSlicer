#include "HeadEnvelope.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include <boost/filesystem/directory.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/format.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <ios>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::Nonplanar {

namespace {

constexpr double Inf         = std::numeric_limits<double>::infinity();
constexpr char   SchemaV1[]  = "orca.head_envelope.v1";

std::pair<Vec2d, Vec2d> footprint(const HeadBox &box, double lateral_margin)
{
    const double grow = box.nozzle ? 0. : lateral_margin;
    return { Vec2d(box.min.x() - grow, box.min.y() - grow), Vec2d(box.max.x() + grow, box.max.y() + grow) };
}

bool band_x_extent(const std::vector<Vec2d> &poly, double ya, double yb, double &xmin, double &xmax)
{
    xmin = Inf;
    xmax = -Inf;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Vec2d &a = poly[i], &b = poly[(i + 1) % poly.size()];
        if (a.y() >= ya && a.y() <= yb) {
            xmin = std::min(xmin, a.x());
            xmax = std::max(xmax, a.x());
        }
        for (double y : { ya, yb })
            if ((a.y() - y) * (b.y() - y) < 0.) {
                const double x = a.x() + (b.x() - a.x()) * (y - a.y()) / (b.y() - a.y());
                xmin = std::min(xmin, x);
                xmax = std::max(xmax, x);
            }
    }
    return xmin <= xmax;
}

std::vector<std::pair<Vec2d, Vec2d>> rect_cover(const std::vector<Vec2d> &poly)
{
    double ymin = Inf, ymax = -Inf;
    for (const Vec2d &p : poly) {
        ymin = std::min(ymin, p.y());
        ymax = std::max(ymax, p.y());
    }
    std::set<double> cuts;
    for (const Vec2d &p : poly)
        cuts.insert(p.y());
    for (int i = 0; i <= 24; ++i)
        cuts.insert(ymin + (ymax - ymin) * i / 24.);
    const std::vector<double> ys(cuts.begin(), cuts.end());

    std::vector<std::pair<Vec2d, Vec2d>> best;
    double                               best_area = Inf;
    for (size_t i = 0; i < ys.size(); ++i)
        for (size_t j = i; j < ys.size(); ++j) {
            std::vector<std::pair<Vec2d, Vec2d>> rects;
            double                               area = 0.;
            for (const auto &[ya, yb] : { std::make_pair(ymin, ys[i]), std::make_pair(ys[i], ys[j]), std::make_pair(ys[j], ymax) }) {
                double x0, x1;
                if ((yb <= ya && ymax > ymin) || !band_x_extent(poly, ya, yb, x0, x1))
                    continue;
                rects.emplace_back(Vec2d(x0, ya), Vec2d(x1, yb));
                area += (x1 - x0) * (yb - ya);
            }
            if (!rects.empty() && area < best_area - 1e-12) {
                best_area = area;
                best      = std::move(rects);
            }
        }
    return best;
}

std::string field_path(const std::string &parent, const std::string &key) { return parent.empty() ? key : parent + "." + key; }

[[noreturn]] void bad_field(const std::string &path, const std::string &what)
{
    throw RuntimeError("Head envelope JSON: field \"" + path + "\" " + what);
}

double get_number(const nlohmann::json &obj, const std::string &key, const std::string &parent, const double *def)
{
    const auto it = obj.find(key);
    if (it == obj.end()) {
        if (def == nullptr)
            bad_field(field_path(parent, key), "is missing");
        return *def;
    }
    if (!it->is_number())
        bad_field(field_path(parent, key), "must be a number");
    const double v = it->get<double>();
    if (!std::isfinite(v))
        bad_field(field_path(parent, key), "must be finite");
    return v;
}

double get_number(const nlohmann::json &obj, const std::string &key, const std::string &parent, double def)
{
    return get_number(obj, key, parent, &def);
}

double require_number(const nlohmann::json &obj, const std::string &key, const std::string &parent)
{
    return get_number(obj, key, parent, nullptr);
}

bool get_bool(const nlohmann::json &obj, const std::string &key, const std::string &parent, bool def)
{
    const auto it = obj.find(key);
    if (it == obj.end())
        return def;
    if (!it->is_boolean())
        bad_field(field_path(parent, key), "must be true or false");
    return it->get<bool>();
}

std::string get_string(const nlohmann::json &obj, const std::string &key, const std::string &parent, bool required)
{
    const auto it = obj.find(key);
    if (it == obj.end()) {
        if (required)
            bad_field(field_path(parent, key), "is missing");
        return {};
    }
    if (!it->is_string())
        bad_field(field_path(parent, key), "must be a string");
    return it->get<std::string>();
}

Vec3d get_vec3(const nlohmann::json &obj, const std::string &key, const std::string &parent)
{
    const auto it = obj.find(key);
    if (it == obj.end())
        bad_field(field_path(parent, key), "is missing");
    if (!it->is_array() || it->size() != 3)
        bad_field(field_path(parent, key), "must be an array of 3 numbers");
    Vec3d v;
    for (int i = 0; i < 3; ++i) {
        if (!(*it)[i].is_number() || !std::isfinite((*it)[i].get<double>()))
            bad_field(field_path(parent, key), "must be an array of 3 finite numbers");
        v[i] = (*it)[i].get<double>();
    }
    return v;
}

void warn_unknown(const nlohmann::json &obj, std::initializer_list<const char *> known, const std::string &parent)
{
    for (auto it = obj.begin(); it != obj.end(); ++it)
        if (std::none_of(known.begin(), known.end(), [&](const char *k) { return it.key() == k; }))
            BOOST_LOG_TRIVIAL(info) << "Head envelope JSON: ignoring unknown field \"" << field_path(parent, it.key()) << "\"";
}

}

double HeadEnvelope::underside_height(const Vec2d &d) const
{
    double best = Inf;
    for (const HeadBox &box : boxes) {
        const auto [lo, hi] = footprint(box, lateral_margin);
        if ((box.infinite_x || (d.x() >= lo.x() && d.x() <= hi.x())) && (box.infinite_y || (d.y() >= lo.y() && d.y() <= hi.y())))
            best = std::min(best, box.min.z());
    }
    return d.x() == 0. && d.y() == 0. ? std::min(best, 0.) : best;
}

double HeadEnvelope::max_slope(double phi, double gap, double reach) const
{
    const Vec2d u(std::cos(phi), std::sin(phi));
    double      tan_min = Inf;
    for (const HeadBox &box : boxes) {
        if (box.infinite_x || box.infinite_y)
            continue;
        const auto [lo, hi] = footprint(box, lateral_margin);
        double pmin = Inf, pmax = -Inf;
        for (const Vec2d &c : { lo, hi, Vec2d(lo.x(), hi.y()), Vec2d(hi.x(), lo.y()) }) {
            pmin = std::min(pmin, c.dot(u));
            pmax = std::max(pmax, c.dot(u));
        }
        if (pmax <= 0. || pmin > reach)
            continue;
        pmax = std::min(pmax, reach);
        const double num = gap + box.min.z() - (box.nozzle ? 0. : margin);
        const double limit = num >= 0. ? num / pmax : pmin > 0. ? num / pmin : -Inf;
        tan_min = std::min(tan_min, limit);
    }
    return tan_min == Inf ? 0.5 * PI : std::atan(std::max(0., tan_min));
}

std::vector<double> HeadEnvelope::slope_map(int n, double gap) const
{
    std::vector<double> out;
    out.reserve(std::max(0, n));
    for (int i = 0; i < n; ++i)
        out.emplace_back(this->max_slope(2. * PI * i / n, gap));
    return out;
}

namespace {

void validate_reading(const SlopeStarReading &reading)
{
    const auto &date = reading.date;
    if (date.size() != 10 || date[4] != '-' || date[7] != '-' ||
        date.substr(0, 4).find_first_not_of("0123456789") != std::string::npos ||
        date.substr(5, 2).find_first_not_of("0123456789") != std::string::npos ||
        date.substr(8, 2).find_first_not_of("0123456789") != std::string::npos)
        throw RuntimeError("Slope star: date must be YYYY-MM-DD");
    const int year = std::stoi(date.substr(0, 4)), month = std::stoi(date.substr(5, 2)), day = std::stoi(date.substr(8, 2));
    const int days[] = {31, 28 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year == 0 || month < 1 || month > 12 || day < 1 || day > days[month - 1])
        throw RuntimeError("Slope star: date must be a valid calendar date");
    if (reading.samples.size() < 3)
        throw RuntimeError("Slope star: report at least three azimuths around the head");
    std::set<double> azimuths;
    for (const SlopeStarSample &sample : reading.samples) {
        if (!std::isfinite(sample.azimuth) || sample.azimuth < 0. || sample.azimuth >= 2. * PI ||
            !azimuths.insert(sample.azimuth).second)
            throw RuntimeError("Slope star: azimuths must be unique and in [0, 360) degrees");
        if (!std::isfinite(sample.max_clean_slope) || sample.max_clean_slope < 0. || sample.max_clean_slope >= 0.5 * PI)
            throw RuntimeError("Slope star: clean slopes must be in [0, 90) degrees");
    }
}

double box_projection(const HeadBox &box, double grow, const Vec2d &u)
{
    const auto [lo, hi] = footprint(box, grow);
    return (u.x() >= 0. ? hi.x() : lo.x()) * u.x() + (u.y() >= 0. ? hi.y() : lo.y()) * u.y();
}

}

HeadEnvelope HeadEnvelope::from_slope_star(const HeadEnvelope &base, const SlopeStarReading &reading, float layer_height)
{
    validate_reading(reading);
    if (!std::isfinite(layer_height) || layer_height <= 0. || base.empty())
        throw RuntimeError("Slope star: select a nonempty base head and a positive layer height");
    HeadEnvelope out = base;
    for (HeadBox &box : out.boxes) {
        if (box.infinite_x || box.infinite_y)
            continue;
        double floor = box.min.z();
        for (const SlopeStarSample &sample : reading.samples) {
            const Vec2d u(std::cos(sample.azimuth), std::sin(sample.azimuth));
            const double projection = box_projection(box, out.lateral_margin, u);
            if (projection > 0.)
                floor = std::max(floor, std::tan(sample.max_clean_slope) * projection - layer_height + (box.nozzle ? 0. : out.margin));
        }
        box.min.z() = floor;
        box.max.z() = std::max(box.max.z(), floor);
    }
    for (const SlopeStarSample &sample : reading.samples) {
        if (out.max_slope(sample.azimuth, layer_height) <= sample.max_clean_slope)
            continue;
        const Vec2d u(std::cos(sample.azimuth), std::sin(sample.azimuth));
        HeadBox *binding = nullptr;
        double best = Inf, projection = 0.;
        for (HeadBox &box : out.boxes) {
            if (box.infinite_x || box.infinite_y)
                continue;
            const double p = box_projection(box, out.lateral_margin, u);
            if (p <= 0.)
                continue;
            const double limit = (layer_height + box.min.z() - (box.nozzle ? 0. : out.margin)) / p;
            if (limit < best) {
                best = limit;
                binding = &box;
                projection = p;
            }
        }
        if (binding == nullptr)
            throw RuntimeError("Slope star: base head has no finite component toward a reported azimuth");
        binding->min.z() = std::tan(sample.max_clean_slope) * projection - layer_height +
                           (binding->nozzle ? 0. : out.margin) - 1e-10;
    }
    out.calibrated = true;
    out.provenance = "slope star " + reading.date + ", base " + base.id;
    return out;
}

double HeadEnvelope::gantry_clearance() const
{
    double z = Inf;
    for (const HeadBox &box : boxes)
        if (box.infinite_x || box.infinite_y)
            z = std::min(z, box.min.z());
    return z;
}

double HeadEnvelope::reach() const
{
    double r = 0.;
    for (const HeadBox &box : boxes)
        if (!box.infinite_x && !box.infinite_y) {
            const auto [lo, hi] = footprint(box, lateral_margin);
            r = std::max(r, std::hypot(std::max(std::abs(lo.x()), std::abs(hi.x())), std::max(std::abs(lo.y()), std::abs(hi.y()))));
        }
    return r;
}

std::vector<LowBox> low_boxes(const HeadEnvelope &head, double gap, double max_relief)
{
    std::vector<LowBox> out;
    for (const HeadBox &box : head.boxes) {
        if (box.nozzle)
            continue;
        const double floor = gap + box.min.z() - head.margin;
        if (floor >= max_relief)
            continue;
        const double grow = head.lateral_margin;
        out.push_back({ Vec2d(box.min.x() - grow, box.min.y() - grow), Vec2d(box.max.x() + grow, box.max.y() + grow), floor,
                        box.infinite_x, box.infinite_y });
    }
    return out;
}

bool conflicts(const std::vector<LowBox> &boxes, const BoundingBoxf &a, double relief_a, const BoundingBoxf &b, double relief_b)
{
    const double stand = std::min(relief_a, relief_b);
    for (const LowBox &box : boxes) {
        if (box.floor >= stand)
            continue;
        const bool x = box.infinite_x || (a.min.x() + box.min.x() < b.max.x() && a.max.x() + box.max.x() > b.min.x());
        const bool y = box.infinite_y || (a.min.y() + box.min.y() < b.max.y() && a.max.y() + box.max.y() > b.min.y());
        if (x && y)
            return true;
    }
    return false;
}

bool operator==(const HeadBox &lhs, const HeadBox &rhs)
{
    return lhs.name == rhs.name && lhs.min == rhs.min && lhs.max == rhs.max && lhs.infinite_x == rhs.infinite_x &&
           lhs.infinite_y == rhs.infinite_y && lhs.nozzle == rhs.nozzle && lhs.block == rhs.block && lhs.duct == rhs.duct;
}

bool operator==(const TipAllowance &lhs, const TipAllowance &rhs) { return lhs.radius == rhs.radius && lhs.height == rhs.height; }

bool operator==(const HeadEnvelope &lhs, const HeadEnvelope &rhs)
{
    return lhs.id == rhs.id && lhs.name == rhs.name && lhs.boxes == rhs.boxes && lhs.tip_flat_radius == rhs.tip_flat_radius &&
           lhs.tip_allowance == rhs.tip_allowance && lhs.margin == rhs.margin && lhs.lateral_margin == rhs.lateral_margin &&
           lhs.calibrated == rhs.calibrated && lhs.provenance == rhs.provenance && lhs.extensions == rhs.extensions;
}

HeadEnvelope envelope_from_side_profile(const std::vector<SideProfileRow> &rows, double top,
                                        double gantry_height, double gantry_ymin, double gantry_ymax)
{
    std::vector<SideProfileRow> sorted = rows;
    std::stable_sort(sorted.begin(), sorted.end(), [](const SideProfileRow &a, const SideProfileRow &b) { return a.height < b.height; });
    HeadEnvelope out;
    for (size_t i = 0; i < sorted.size(); ++i) {
        const SideProfileRow &row = sorted[i];
        HeadBox               box;
        box.name = "profile_" + std::to_string(i);
        box.min  = Vec3d(-row.xneg, -row.yneg, row.height);
        box.max  = Vec3d(row.xpos, row.ypos, std::max(top, row.height));
        out.boxes.emplace_back(std::move(box));
    }
    if (std::isfinite(gantry_height) && gantry_height > 0.) {
        HeadBox gantry;
        gantry.name       = "x_gantry";
        gantry.infinite_x = true;
        gantry.min        = Vec3d(0., std::min(gantry_ymin, gantry_ymax), gantry_height);
        gantry.max        = Vec3d(0., std::max(gantry_ymin, gantry_ymax), std::max(top, gantry_height + 1.));
        out.boxes.emplace_back(std::move(gantry));
    }
    return out;
}

std::vector<HeadBox> boxes_from_n_side_profile(const std::vector<NSideProfileRow> &rows, double top)
{
    std::vector<HeadBox> out;
    for (size_t i = 0; i < rows.size(); ++i) {
        const NSideProfileRow &row = rows[i];
        const size_t           n   = row.reach.size();
        if (n < 3)
            throw RuntimeError("Head envelope: an N-side profile row needs at least 3 reach values");
        std::vector<Vec2d> poly;
        for (size_t k = 0; k < n; ++k) {
            const double a = 2. * PI * double(k) / double(n);
            poly.emplace_back(std::max(0., row.reach[k]) * Vec2d(std::cos(a), std::sin(a)));
        }
        const auto rects = rect_cover(poly);
        for (size_t j = 0; j < rects.size(); ++j) {
            HeadBox box;
            box.name = "profile_" + std::to_string(i) + "_" + std::to_string(j);
            box.min  = Vec3d(rects[j].first.x(), rects[j].first.y(), row.height);
            box.max  = Vec3d(rects[j].second.x(), rects[j].second.y(), std::max(top, row.height));
            out.emplace_back(std::move(box));
        }
    }
    return out;
}

HeadEnvelope load_head_envelope(const std::string &json_text)
{
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::exception &err) {
        throw RuntimeError(std::string("Head envelope JSON is not valid JSON: ") + err.what());
    }
    if (!j.is_object())
        throw RuntimeError("Head envelope JSON: the top level must be an object");
    const std::string schema = get_string(j, "schema", {}, true);
    if (schema != SchemaV1)
        bad_field("schema", "is \"" + schema + "\", expected \"" + SchemaV1 + "\"");
    warn_unknown(j, { "schema", "id", "name", "provenance", "calibrated", "nozzle", "margin", "lateral_margin", "side_profile",
                      "side_profile_top", "boxes", "sides", "table_obstacles", "coverage", "access_model" }, {});

    HeadEnvelope out;
    auto extensions = j;
    for (const char *key : { "schema", "id", "name", "provenance", "calibrated", "nozzle", "margin", "lateral_margin",
                            "side_profile", "side_profile_top", "boxes", "sides" })
        extensions.erase(key);
    if (!extensions.empty())
        out.extensions = extensions.dump();
    out.id = get_string(j, "id", {}, true);
    if (out.id.empty())
        bad_field("id", "must not be empty");
    out.name           = get_string(j, "name", {}, false);
    out.provenance     = get_string(j, "provenance", {}, false);
    out.calibrated     = get_bool(j, "calibrated", {}, false);
    out.margin         = get_number(j, "margin", {}, out.margin);
    out.lateral_margin = get_number(j, "lateral_margin", {}, out.lateral_margin);
    if (out.margin < 0.)
        bad_field("margin", "must not be negative");
    if (out.lateral_margin < 0.)
        bad_field("lateral_margin", "must not be negative");

    if (const auto it = j.find("nozzle"); it != j.end()) {
        if (!it->is_object())
            bad_field("nozzle", "must be an object");
        warn_unknown(*it, { "tip_flat_radius", "allowance_radius", "allowance_height" }, "nozzle");
        out.tip_flat_radius        = get_number(*it, "tip_flat_radius", "nozzle", out.tip_flat_radius);
        out.tip_allowance.radius   = get_number(*it, "allowance_radius", "nozzle", out.tip_allowance.radius);
        out.tip_allowance.height   = get_number(*it, "allowance_height", "nozzle", out.tip_allowance.height);
    }

    if (const auto it = j.find("side_profile"); it != j.end()) {
        if (!it->is_array())
            bad_field("side_profile", "must be an array");
        const double top   = require_number(j, "side_profile_top", {});
        const double sides = get_number(j, "sides", {}, 0.);
        if (sides < 0. || sides != std::floor(sides))
            bad_field("sides", "must be a non-negative integer");
        if (sides == 0.) {
            std::vector<SideProfileRow> rows;
            for (size_t i = 0; i < it->size(); ++i) {
                const std::string p   = "side_profile[" + std::to_string(i) + "]";
                const auto       &row = (*it)[i];
                if (!row.is_object())
                    bad_field(p, "must be an object");
                rows.push_back({ require_number(row, "height", p), require_number(row, "xpos", p), require_number(row, "xneg", p),
                                 require_number(row, "ypos", p), require_number(row, "yneg", p) });
            }
            out.boxes = envelope_from_side_profile(rows, top, Inf, 0., 0.).boxes;
        } else {
            std::vector<NSideProfileRow> rows;
            for (size_t i = 0; i < it->size(); ++i) {
                const std::string p   = "side_profile[" + std::to_string(i) + "]";
                const auto       &row = (*it)[i];
                if (!row.is_object())
                    bad_field(p, "must be an object");
                NSideProfileRow r { require_number(row, "height", p), {} };
                const auto      reach = row.find("reach");
                if (reach == row.end() || !reach->is_array() || reach->size() != size_t(sides))
                    bad_field(p + ".reach", "must be an array of " + std::to_string(int(sides)) + " numbers");
                for (const auto &v : *reach) {
                    if (!v.is_number())
                        bad_field(p + ".reach", "must contain only numbers");
                    r.reach.push_back(v.get<double>());
                }
                rows.emplace_back(std::move(r));
            }
            out.boxes = boxes_from_n_side_profile(rows, top);
        }
    }

    if (const auto it = j.find("boxes"); it != j.end()) {
        if (!it->is_array())
            bad_field("boxes", "must be an array");
        for (size_t i = 0; i < it->size(); ++i) {
            const std::string p = "boxes[" + std::to_string(i) + "]";
            const auto       &b = (*it)[i];
            if (!b.is_object())
                bad_field(p, "must be an object");
            warn_unknown(b, { "name", "min", "max", "infinite_x", "infinite_y", "nozzle", "block", "duct" }, p);
            HeadBox box;
            box.name       = get_string(b, "name", p, false);
            box.min        = get_vec3(b, "min", p);
            box.max        = get_vec3(b, "max", p);
            box.infinite_x = get_bool(b, "infinite_x", p, false);
            box.infinite_y = get_bool(b, "infinite_y", p, false);
            box.nozzle     = get_bool(b, "nozzle", p, false);
            box.block      = get_bool(b, "block", p, false);
            std::string lower = box.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            box.duct       = get_bool(b, "duct", p, !box.nozzle && !box.block &&
                                                       (lower.find("duct") != std::string::npos || lower.find("shroud") != std::string::npos));
            for (int a = 0; a < 3; ++a)
                if (box.min[a] > box.max[a] && !(a == 0 && box.infinite_x) && !(a == 1 && box.infinite_y))
                    bad_field(p + ".min", "must not exceed max");
            out.boxes.emplace_back(std::move(box));
        }
    }
    if (out.boxes.empty())
        bad_field("boxes", "must describe at least one box (directly or through side_profile)");
    return out;
}

std::string save_head_envelope(const HeadEnvelope &envelope)
{
    nlohmann::ordered_json j = envelope.extensions.empty() ? nlohmann::ordered_json::object() :
                                                            nlohmann::ordered_json::parse(envelope.extensions);
    j["schema"]         = SchemaV1;
    j["id"]             = envelope.id;
    j["name"]           = envelope.name;
    j["provenance"]     = envelope.provenance;
    j["calibrated"]     = envelope.calibrated;
    j["nozzle"]         = { { "tip_flat_radius", envelope.tip_flat_radius },
                            { "allowance_radius", envelope.tip_allowance.radius },
                            { "allowance_height", envelope.tip_allowance.height } };
    j["margin"]         = envelope.margin;
    j["lateral_margin"] = envelope.lateral_margin;
    nlohmann::ordered_json boxes = nlohmann::ordered_json::array();
    for (const HeadBox &box : envelope.boxes) {
        nlohmann::ordered_json b;
        b["name"] = box.name;
        b["min"]  = { box.min.x(), box.min.y(), box.min.z() };
        b["max"]  = { box.max.x(), box.max.y(), box.max.z() };
        if (box.infinite_x)
            b["infinite_x"] = true;
        if (box.infinite_y)
            b["infinite_y"] = true;
        if (box.nozzle)
            b["nozzle"] = true;
        if (box.block)
            b["block"] = true;
        if (box.duct)
            b["duct"] = true;
        boxes.emplace_back(std::move(b));
    }
    j["boxes"] = std::move(boxes);
    return j.dump(2);
}

HeadLibrary &HeadLibrary::instance()
{
    static HeadLibrary library;
    return library;
}

void HeadLibrary::load_locked() const
{
    if (m_loaded)
        return;
    m_heads.clear();
    m_sources.clear();
    m_heads.emplace(generic_conservative().id, generic_conservative());
    namespace fs = boost::filesystem;
    for (const std::string &root : { resources_dir(), data_dir() }) {
        if (root.empty())
            continue;
        const fs::path dir = fs::path(root) / "nonplanar" / "heads";
        boost::system::error_code ec;
        if (!fs::is_directory(dir, ec))
            continue;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".json")
                files.emplace_back(it->path());
        std::sort(files.begin(), files.end());
        for (const fs::path &file : files) {
            std::string json_text;
            try {
                boost::nowide::ifstream in(file.string());
                std::stringstream       ss;
                ss << in.rdbuf();
                json_text = ss.str();
                HeadEnvelope head = load_head_envelope(json_text);
                m_sources[head.id] = file.string();
                m_heads[head.id]  = std::move(head);
            } catch (const std::exception &err) {
                const auto json = nlohmann::json::parse(json_text, nullptr, false);
                std::string id = json.is_object() && json.contains("id") && json["id"].is_string() ?
                                     json["id"].get<std::string>() : file.stem().string();
                if (id.empty())
                    id = file.stem().string();
                m_heads.erase(id);
                m_sources.erase(id);
                BOOST_LOG_TRIVIAL(error) << "Skipping non-planar head " << file.string() << ": " << err.what();
            }
        }
    }
    m_loaded = true;
}

const HeadEnvelope *HeadLibrary::find(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    this->load_locked();
    const auto it = m_heads.find(id);
    return it == m_heads.end() ? nullptr : &it->second;
}

std::string HeadLibrary::source_path(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    this->load_locked();
    const auto it = m_sources.find(id);
    return it == m_sources.end() ? std::string() : it->second;
}

std::vector<std::string> HeadLibrary::ids() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    this->load_locked();
    std::vector<std::string> out;
    out.reserve(m_heads.size());
    std::transform(m_heads.begin(), m_heads.end(), std::back_inserter(out), [](const auto &kv) { return kv.first; });
    return out;
}

void HeadLibrary::reload()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_loaded = false;
    this->load_locked();
}

void HeadLibrary::save_user_head(const HeadEnvelope &head)
{
    if (head.id.empty() || head.id == "." || head.id == ".." ||
        head.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
        throw RuntimeError("Head id must contain only letters, digits, hyphens and underscores.");
    const std::string json = save_head_envelope(head);
    load_head_envelope(json);
    if (data_dir().empty())
        throw RuntimeError("The user data directory is not available.");
    namespace fs = boost::filesystem;
    const fs::path dir = fs::path(data_dir()) / "nonplanar" / "heads";
    fs::create_directories(dir);
    const fs::path path = dir / (head.id + ".json");
    if (find(head.id) != nullptr || fs::exists(path))
        throw RuntimeError("A head with this id already exists. Choose a new user head id.");
    const fs::path temporary = dir / fs::unique_path("head-%%%%-%%%%.tmp");
    try {
        boost::nowide::ofstream stream(temporary.string());
        stream << json << '\n';
        stream.close();
        if (!stream)
            throw RuntimeError("Could not write the user head file.");
        fs::rename(temporary, path);
    } catch (...) {
        boost::system::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_heads.emplace(head.id, head);
    m_sources.emplace(head.id, path.string());
}

const HeadEnvelope &HeadLibrary::generic_conservative()
{
    static const HeadEnvelope head = [] {
        const double                t = std::tan(15. * PI / 180.);
        std::vector<SideProfileRow> rows;
        for (int i = 1; i <= 40; ++i) {
            const double h = 0.5 * i, r = 0.5 * (i + 1) / t;
            rows.push_back({ h, r, r, r, r });
        }
        HeadEnvelope out = envelope_from_side_profile(rows, 60., 20., -40., 40.);
        HeadBox      land;
        land.name   = "nozzle";
        land.min    = Vec3d(-0.5, -0.5, 0.);
        land.max    = Vec3d(0.5, 0.5, 60.);
        land.nozzle = true;
        out.boxes.insert(out.boxes.begin(), land);
        out.id              = "generic-conservative";
        out.name            = "Generic conservative head";
        out.provenance      = "Built in: 15-degree four-sided stepped pyramid, 20 mm gantry. Not a real head.";
        out.tip_flat_radius = 0.5;
        out.tip_allowance   = { 0.75, 0.1 };
        return out;
    }();
    return head;
}

HeadEnvelope resolve_head_envelope(const PrintConfig &config, std::string *warning)
{
    HeadEnvelope head = resolve_full_head_envelope(config, warning);
    if (config.nonplanar_ignore_duct.value)
        head.boxes.erase(std::remove_if(head.boxes.begin(), head.boxes.end(), [](const HeadBox &box) { return box.duct; }),
                         head.boxes.end());
    return head;
}

bool clearance_override_active(const PrintConfig &config)
{
    if (!config.nonplanar_ignore_duct.value)
        return false;
    const HeadEnvelope head = resolve_full_head_envelope(config);
    return std::any_of(head.boxes.begin(), head.boxes.end(), [](const HeadBox &box) { return box.duct; });
}

static HeadEnvelope resolve_head_shape(const PrintConfig &config, std::string *warning)
{
    if (warning != nullptr)
        warning->clear();
    const std::string &id = config.nonplanar_head.value;
    try {
        if (!config.nonplanar_head_snapshot.value.empty()) {
            const auto snapshot = nlohmann::json::parse(config.nonplanar_head_snapshot.value);
            if (snapshot.at("reference").get<std::string>() == id)
                return load_head_envelope(snapshot.at("envelope").dump());
        }
        if (!id.empty() && boost::filesystem::is_regular_file(boost::filesystem::path(id))) {
            boost::nowide::ifstream in(id, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            return load_head_envelope(text.str());
        }
    } catch (const std::exception &err) {
        BOOST_LOG_TRIVIAL(error) << "Cannot load non-planar head " << id << ": " << err.what();
        if (warning != nullptr)
            *warning = (boost::format(_u8L("Oops! Couldn't load print head profile \"%1%\": %2%")) % id % err.what()).str();
        return HeadLibrary::generic_conservative();
    }
    if (!id.empty())
        if (const HeadEnvelope *head = HeadLibrary::instance().find(id))
            return *head;
    if (warning != nullptr)
        *warning = id.empty() ? _u8L("This printer doesn't have a specific print head profile yet, so we're using the safe generic print head.")
                              : (boost::format(_u8L("Oops! Couldn't find or load print head profile \"%1%\", so we're falling back to the safe generic print head.")) % id).str();
    return HeadLibrary::generic_conservative();
}

HeadEnvelope resolve_full_head_envelope(const PrintConfig &config, std::string *warning)
{
    HeadEnvelope head      = resolve_head_shape(config, warning);
    head.contact_tolerance = std::max(0., config.nonplanar_collision_tolerance.value);
    head.duct_graze_limit  = size_t(std::max(0, config.nonplanar_duct_graze_limit.value));
    return head;
}

bool native_support_capable(const PrintConfig &config)
{
    std::string      warning;
    const HeadEnvelope head = resolve_head_envelope(config, &warning);
    if (!warning.empty() || head.extensions.empty())
        return false;
    try {
        const auto coverage = nlohmann::json::parse(head.extensions).at("coverage");
        const auto &cone = coverage.at("nozzle_cone"), &ducts = coverage.at("duct_limits");
        const double half_angle = cone.at("half_angle_deg").get<double>();
        return cone.at("tip_radius").get<double>() > 0. && half_angle > 0. && half_angle < 90. && cone.at("top_z").get<double>() > 0. &&
               ducts.at("z_min").get<double>() > 0. && ducts.at("half_width_x").get<double>() > 0. &&
               ducts.at("half_width_y").get<double>() > 0.;
    } catch (const std::exception &) {
        return false;
    }
}

}
