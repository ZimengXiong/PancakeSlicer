#include "Wave.hpp"
#include "RegionDetect.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/SurfaceCollection.hpp"
#include "libslic3r/libslic3r.h"
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <functional>
#include <ios>
#include <string>
#include <cmath>
#include <limits>
#include <utility>

namespace Slic3r::Nonplanar {

bool wave_enabled(const PrintObject &object)
{
    return object.config().nonplanar_wave.value != NonplanarWave::Off &&
           object.print()->config().nonplanar_kinematics.value == RotaryLayout::None;
}

bool wave_enabled(const Print &print)
{
    return std::any_of(print.objects().begin(), print.objects().end(), [](const PrintObject *object) { return wave_enabled(*object); });
}

void write_wave_verification(const std::string &path, size_t grazes, size_t hits, const std::vector<WaveResult> &results)
{
    boost::nowide::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    const std::string grazes_prefix = "; nonplanar_grazes = ";
    const std::string hits_prefix = "; nonplanar_hits = ";
    const std::string result_prefix = "; nonplanar_wave_result = ";
    const std::string error = "Could not update non-planar verification in the G-code header.";

    auto pad_slot = [&error](std::string value, size_t width) {
        if (value.size() > width)
            throw Slic3r::RuntimeError(error);
        value.resize(width, ' ');
        return value;
    };

    const std::string pending_grazes = grazes_prefix + pad_slot(std::to_string(std::hash<std::string>{}(path)), 20);
    std::string line;
    bool in_header = false;
    auto pos = file.tellg();
    while (std::getline(file, line)) {
        if (line == "; HEADER_BLOCK_START")
            in_header = true;
        else if (line == "; HEADER_BLOCK_END")
            in_header = false;
        else if (in_header && line == pending_grazes)
            break;
        pos = file.tellg();
    }
    if (!file)
        throw Slic3r::RuntimeError(results.empty() ? "Missing curved tops clearance header." : error);

    std::vector<std::pair<std::streampos, std::string>> patches;
    patches.emplace_back(pos + std::streamoff(grazes_prefix.size()), pad_slot(std::to_string(grazes), 20));
    pos = file.tellg();
    if (!std::getline(file, line) || line != hits_prefix + pad_slot("0", 20))
        throw Slic3r::RuntimeError(error);
    patches.emplace_back(pos + std::streamoff(hits_prefix.size()), pad_slot(std::to_string(hits), 20));

    for (const WaveResult &result : results) {
        const size_t object = result.object;
        const std::string pending = "{\"object\":" + std::to_string(object) + "}";
        pos = file.tellg();
        if (!std::getline(file, line) || line != result_prefix + pad_slot(pending, 768))
            throw Slic3r::RuntimeError(error);

        const bool active = result.displaced_length > 0.;
        const std::string mode = ConfigOptionEnum<NonplanarWave>(result.mode).serialize();
        nlohmann::json value = {{"object", object}, {"mode", mode}, {"effective_mode", active ? mode : "off"},
            {"effective_amplitude", active ? result.amplitude : 0.}, {"status", active ? "applied" : "planar"},
            {"fallback_reason", active ? "" : result.fallback_reason}, {"displaced_points", result.displaced_points},
            {"displaced_length_mm", result.displaced_length}, {"max_bead_height", result.max_bead_height}};
        patches.emplace_back(pos + std::streamoff(result_prefix.size()), pad_slot(value.dump(), 768));
    }

    while (std::getline(file, line) && line != "; HEADER_BLOCK_END")
        if (line == "; HEADER_BLOCK_START" || line.rfind(result_prefix, 0) == 0)
            throw Slic3r::RuntimeError(error);
    if (!file)
        throw Slic3r::RuntimeError(error);

    for (const auto &[offset, value] : patches) {
        file.seekp(offset);
        file.write(value.data(), std::streamsize(value.size()));
    }
    file.close();
    if (!file)
        throw Slic3r::RuntimeError(error);
}

WaveField::WaveField(const PrintObject &object)
    : m_mode(object.config().nonplanar_wave.value), m_pattern(object.config().nonplanar_wave_pattern.value),
      m_amplitude(std::max(0., object.config().nonplanar_wave_amplitude.value)),
      m_wavelength(std::max(2., object.config().nonplanar_wave_wavelength.value)),
      m_direction(object.config().nonplanar_wave_direction.value * M_PI / 180.),
      m_base_z(0.), m_ramp_height(1.), m_centre(Vec2d::Zero())
{
    BoundingBox bounds;
    double min_height = std::numeric_limits<double>::max();
    int min_shells = std::numeric_limits<int>::max();
    for (const Layer *layer : object.layers()) {
        Slice slice { layer->print_z, layer->height, {}, {} };
        min_height = std::min(min_height, layer->height);
        for (const ExPolygon &island : layer->lslices)
            bounds.merge(get_extents(island));
        for (const LayerRegion *region : layer->regions()) {
            const int shells = region->region().config().top_shell_layers.value;
            for (const Surface &surface : region->slices.surfaces) {
                if (surface.is_top() && shells > 0) {
                    for (ExPolygon &allowed : nonplanar_paint_mask(object, m_slices.size(), { surface.expolygon }))
                        slice.tops.push_back({ allowed, get_extents(allowed), allowed.lines(), shells });
                    min_shells = std::min(min_shells, shells);
                    m_max_shells = std::max(m_max_shells, shells);
                }
            }
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.is_solid())
                    slice.solids.push_back(surface.expolygon);
        }
        m_slices.push_back(std::move(slice));
    }
    if (m_slices.empty())
        return;
    m_centre = unscale(bounds.center());
    if (m_mode != NonplanarWave::Top) {
        for (const SupportLayer *layer : object.support_layers()) {
            Points points;
            layer->support_fills.collect_points(points);
            bounds.merge(points);
        }
        bounds.offset(scale_(2.1));
        const Point &lo = bounds.min, &hi = bounds.max;
        ExPolygons canvas { ExPolygon(Polygon({ lo, Point(hi.x(), lo.y()), hi, Point(lo.x(), hi.y()) })) };
        for (ExPolygon &allowed : nonplanar_paint_mask(object, 0, canvas))
            m_paint.push_back({ allowed, get_extents(allowed), allowed.lines(), 0 });
    }
    m_base_z = m_slices[std::min(size_t(2), m_slices.size() - 1)].z;
    if (m_mode == NonplanarWave::Top && min_shells != std::numeric_limits<int>::max())
        m_amplitude = std::min({ m_amplitude, 0.5 * min_height, 0.5 * min_height * min_shells });
    if (m_mode == NonplanarWave::Infill)
        m_amplitude = std::min(m_amplitude, 0.5 * min_height);
    double hop = 0.3;
    for (double z : object.print()->config().z_hop.values)
        hop = std::max(hop, z);
    const double ceiling = object.print()->config().printable_height.value;
    if (ceiling > 0. && m_slices.back().z + m_amplitude + hop > ceiling + EPSILON)
        throw Slic3r::SlicingError("Non-planar wave crests and travel lift exceed the printer's printable height.");
    m_ramp_height = std::max(min_height, std::ceil(4. * m_amplitude / min_height) * min_height);
}

double WaveField::wave(const Point &p) const
{
    const Vec2d xy = unscale(p);
    const double k = 2. * M_PI / m_wavelength;
    switch (m_pattern) {
    case NonplanarWavePattern::Ripple: return std::sin(k * (xy - m_centre).norm());
    case NonplanarWavePattern::Crosshatch: return std::sin(k * xy.x()) * std::sin(k * xy.y());
    default: return std::sin(k * (xy.x() * std::cos(m_direction) + xy.y() * std::sin(m_direction)));
    }
}

double WaveField::offset(const Point &p, size_t layer) const
{
    if (layer >= m_slices.size() || layer < 3)
        return 0.;
    double paint_taper = m_mode == NonplanarWave::Top ? 1. : 0.;
    if (m_mode != NonplanarWave::Top)
        for (const TopSurface &mask : m_paint)
            if (mask.bounds.contains(p) && mask.polygon.contains(p)) {
                double distance = scale_(2.);
                for (const Line &edge : mask.edges)
                    distance = std::min(distance, edge.distance_to(p));
                paint_taper = std::max(paint_taper, std::min(1., unscale<double>(distance) / 2.));
            }
    if (paint_taper <= 0.)
        return 0.;
    double ramp = 0.;
    if (m_mode == NonplanarWave::Whole) {
        ramp = std::clamp((m_slices[layer].z - m_base_z) / m_ramp_height, 0., 1.);
    } else if (m_mode == NonplanarWave::Top) {
        for (size_t top = layer; top < std::min(m_slices.size(), layer + size_t(m_max_shells)); ++top) {
            bool covered = false;
            for (const TopSurface &surface : m_slices[top].tops) {
                const size_t j = top - layer;
                if (j >= size_t(surface.shells) || !surface.bounds.contains(p) || !surface.polygon.contains(p))
                    continue;
                double d = scale_(4.);
                for (const Line &edge : surface.edges)
                    d = std::min(d, edge.distance_to(p));
                ramp = std::max(ramp, (1. - double(j) / surface.shells) * std::min(1., unscale<double>(d) / 4.));
                covered = true;
            }
            if (covered)
                break;
        }
    } else if (m_mode == NonplanarWave::Infill) {
        ramp = 1.;
        for (int d = -2; d <= 2; ++d) {
            const int other = int(layer) + d;
            if (other < 0 || other >= int(m_slices.size())) {
                ramp = std::min(ramp, 0.5 * std::abs(d));
                continue;
            }
            for (const ExPolygon &solid : m_slices[size_t(other)].solids)
                if (solid.contains(p))
                    ramp = std::min(ramp, 0.5 * std::abs(d));
        }
    }
    if (m_mode == NonplanarWave::Top && m_amplitude > 0.)
        ramp = std::min(ramp, std::max(0., (m_slices[layer].z - m_base_z) / (2. * m_amplitude)));
    return m_amplitude * wave(p) * ramp * paint_taper;
}

std::optional<ExtrusionPathConformal> WaveField::lift(const ExtrusionPath &path, const Layer &layer) const
{
    if (m_slices.empty() || path.polyline.points.size() < 2 || m_amplitude <= 0. ||
        path.is_force_no_extrusion() ||
        (m_mode == NonplanarWave::Infill && path.role() != erInternalInfill) ||
        path.role() == erSkirt || path.role() == erBrim || path.role() == erWipeTower || path.role() == erCustom)
        return {};
    const auto found = std::lower_bound(m_slices.begin(), m_slices.end(), layer.print_z - EPSILON,
                                        [](const Slice &slice, double z) { return slice.z < z; });
    const size_t index = std::min(size_t(found - m_slices.begin()), m_slices.size() - 1);
    if (index < 3)
        return {};
    const double step = std::min(m_wavelength / 16., 0.5);
    Points3 points;
    std::vector<float> heights;
    bool lifted = false;
    auto add = [&](const Point &xy) {
        const double z = offset(xy, index);
        const double below = offset(xy, index - 1);
        const double h = layer.height;
        points.push_back(Point3(xy.x(), xy.y(), coord_t(scale_(z))));
        heights.push_back(float(std::clamp(h + z - below, 0.5 * h, 1.5 * h)));
        lifted |= std::abs(z) > EPSILON;
    };
    add(path.polyline.points.front().to_point());
    for (size_t i = 1; i < path.polyline.points.size(); ++i) {
        const Point a = path.polyline.points[i - 1].to_point(), b = path.polyline.points[i].to_point();
        const size_t pieces = std::max(size_t(1), size_t(std::ceil(unscale<double>((b - a).cast<double>().norm()) / step)));
        for (size_t j = 1; j <= pieces; ++j)
            add(Point((a.cast<double>() + (b - a).cast<double>() * (double(j) / pieces)).cast<coord_t>()));
    }
    if (!lifted)
        return {};
    ExtrusionPathConformal out(Polyline3(std::move(points)), path, std::move(heights));
    out.height = float(layer.height);
    out.bead_width.assign(out.polyline.points.size(), path.width);
    out.preserve_points = true;
    return out;
}

}
