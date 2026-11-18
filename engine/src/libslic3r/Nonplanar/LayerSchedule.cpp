#include "LayerSchedule.hpp"
#include "libslic3r/Nonplanar/Research.hpp"

#include <math.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include "Multiplanar.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::Nonplanar {

namespace {

constexpr double Bin = 0.005;
constexpr double Ramp = 0.1;
constexpr double LandingLayers = 4.;
constexpr double TopMinNz = 0.342;
constexpr double SubLayerMaxNz = 0.906;
constexpr double SubLayerMin   = 0.12;
constexpr double FieldLayerSlopeMax = 55.;

bool closed_mesh(const ModelObject &model)
{
    size_t open_edges = 0, facets = 0;
    for (const ModelVolume *volume : model.volumes)
        if (volume->is_model_part()) {
            open_edges += size_t(std::max(0, volume->mesh().stats().open_edges));
            facets += volume->mesh().facets_count();
        }
    return open_edges <= facets / 100;
}

std::mutex                            g_field_mutex;
std::map<const PrintObject *, bool>   g_field;

std::vector<double> record(const PrintObject &object, std::vector<double> &&profile)
{
    std::lock_guard<std::mutex> lock(g_field_mutex);
    g_field[&object] = !profile.empty();
    return std::move(profile);
}

}

double field_layer_slope_max() { return std::tan(FieldLayerSlopeMax * M_PI / 180.); }

bool field_layers(const PrintObject &object)
{
    if (!field_layers_considered(object))
        return false;
    std::lock_guard<std::mutex> lock(g_field_mutex);
    const auto it = g_field.find(&object);
    return it != g_field.end() && it->second;
}

bool field_layers_considered(const PrintObject &object, bool planar_pass)
{
    const PrintObjectConfig &ocfg  = object.config();
    const PrintConfig       &pcfg  = object.print()->config();
    const ModelObject       &model = *object.model_object();
    static const bool uniform = [] {
        const char *v = research_switch("NP_AUTO_UNIFORM");
        return v != nullptr && *v != 0 && *v != '0';
    }();
    if (uniform)
        return false;
    return (planar_pass || ocfg.nonplanar_mode.value == NonplanarMode::Auto) && pcfg.nonplanar_kinematics.value == RotaryLayout::None &&
           ocfg.raft_layers.value == 0 &&
           !(pcfg.enable_prime_tower.value && pcfg.filament_diameter.size() > 1) &&
           !(ocfg.enable_support.value && is_tree(ocfg.support_type.value)) && !pcfg.spiral_mode.value &&
           ocfg.nonplanar_deform.value != NonplanarDeform::On && model.layer_height_profile.empty() &&
           model.layer_config_ranges.empty() && closed_mesh(model);
}

LayerScheduleParams layer_schedule_params(const PrintObject &object)
{
    const PrintConfig       &pcfg = object.print()->config();
    const SlicingParameters &sp   = object.slicing_parameters();
    const double h      = object.config().layer_height.value;
    const double nozzle = pcfg.nozzle_diameter.values.empty() ? 0.4 : pcfg.nozzle_diameter.values.front();
    LayerScheduleParams p;
    p.height = h;
    p.fine        = std::min(h, std::max(0.6 * h, 0.3 * nozzle));
    p.stair       = 0.5 * p.fine;
    p.min_height  = std::min(p.fine, std::max(sp.min_layer_height, 0.2 * nozzle));
    return p;
}

namespace {

std::vector<double> layer_profile(const PrintObject &object)
{
    const SlicingParameters  &sp = object.slicing_parameters();
    const LayerScheduleParams p  = layer_schedule_params(object);
    if (p.fine >= p.height - EPSILON && p.stair / p.fine >= 1. - EPSILON)
        return {};
    const double top   = sp.object_print_z_uncompensated_height();
    const double first = sp.first_object_layer_height;
    if (top <= first + p.height)
        return {};

    const indexed_triangle_set its = slicing_frame_mesh(object);
    float z_min = std::numeric_limits<float>::max();
    for (const stl_vertex &v : its.vertices)
        z_min = std::min(z_min, v.z());
    const size_t bins = size_t(std::ceil(top / Bin)) + 2;
    auto bin_of = [&](double z) { return size_t(std::clamp(z / Bin, 0., double(bins - 1))); };

    struct Face { double z0, z1, n_z; bool top; };
    std::vector<Face>   faces;
    const double nozzle           = object.print()->config().nozzle_diameter.values.empty() ? 0.4 : object.print()->config().nozzle_diameter.values.front();
    const double process_cover_nz = 1. / std::hypot(1., std::min(p.height, sp.first_object_layer_height) / (0.625 * nozzle));
    std::vector<double> flats;
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const Vec3d a = its.vertices[f[0]].cast<double>(), b = its.vertices[f[1]].cast<double>(), c = its.vertices[f[2]].cast<double>();
        const Vec3d cross = (b - a).cross(c - a);
        if (cross.norm() <= 1e-12)
            continue;
        const Vec3d  n  = cross.normalized();
        const double z0 = std::min({ a.z(), b.z(), c.z() }) - z_min;
        const double z1 = std::max({ a.z(), b.z(), c.z() }) - z_min;
        if (std::abs(n.z()) > 0.999) {
            if (z1 - z0 < 1e-3)
                flats.push_back(0.5 * (z0 + z1));
            continue;
        }
        if (n.z() >= process_cover_nz)
            continue;
        faces.push_back({ z0, z1, n.z() < TopMinNz ? std::abs(n.z()) : 0., n.z() >= TopMinNz });
    }
    std::sort(flats.begin(), flats.end());
    flats.erase(std::unique(flats.begin(), flats.end(), [](double a, double b) { return b - a < 1e-3; }), flats.end());

    const double budget = std::ceil((top - first) / p.fine);
    bool sublayer = false;
    for (size_t r = 0; r < object.num_printing_regions(); ++r)
        sublayer |= object.printing_region(r).config().nonplanar_sublayer_walls.value;
    std::vector<float>  limit(bins);
    std::vector<double> heights;
    for (double stair = p.stair;; stair = std::min(p.fine, 1.15 * stair)) {
        std::fill(limit.begin(), limit.end(), float(p.height));
        for (const Face &f : faces) {
            float h_face = float(std::clamp(stair / std::max(f.n_z, 1e-6), p.min_height, p.fine));
            if (sublayer && !f.top && f.n_z < SubLayerMaxNz)
                if (const double twice = std::min(p.height, 2. * stair / std::max(f.n_z, 1e-6)); twice >= SubLayerMin - EPSILON)
                    h_face = float(twice);
            for (size_t i = bin_of(f.z0), e = bin_of(f.z1); i <= e; ++i)
                limit[i] = std::min(limit[i], h_face);
        }
        for (size_t i = 1; i < bins; ++i)
            limit[i] = std::min(limit[i], limit[i - 1] + float(Ramp * Bin));
        for (size_t i = bins - 1; i-- > 0;)
            limit[i] = std::min(limit[i], limit[i + 1] + float(Ramp * Bin));
        auto window_min = [&](double z0, double z1) {
            float lo = float(p.height);
            for (size_t i = bin_of(z0 + 1e-4), e = bin_of(z1 - 1e-4); i <= e; ++i)
                lo = std::min(lo, limit[i]);
            return double(lo);
        };
        heights.clear();
        for (double z = first; z + EPSILON < top;) {
            double h = p.height;
            for (double lo = window_min(z, z + h); lo < h - EPSILON; lo = window_min(z, z + h))
                h = lo;
            if (auto flat = std::upper_bound(flats.begin(), flats.end(), z + 0.5 * p.min_height); flat != flats.end() &&
                *flat - z <= LandingLayers * h + EPSILON) {
                const double n = std::max(1., std::ceil((*flat - z) / h - 1e-6));
                if ((*flat - z) / n >= p.min_height - EPSILON)
                    h = (*flat - z) / n;
            }
            h = std::max(h, p.min_height);
            if (z + h > top - 0.5 * p.min_height)
                h = std::max(top - z, p.min_height);
            heights.push_back(h);
            z += h;
        }
        if (double(heights.size()) <= budget || stair >= p.fine - EPSILON)
            break;
    }
    if (std::all_of(heights.begin(), heights.end(), [&](double h) { return std::abs(h - p.height) < 1e-4; }))
        return {};
    if (double(heights.size()) > 1.10 * budget) {
        BOOST_LOG_TRIVIAL(info) << "Whole-part field " << object.model_object()->name << ": declined, " << heights.size()
                                << " layers over the " << budget << " of uniform fine layers";
        return {};
    }

    std::vector<double> profile{ 0., first };
    if (sp.first_object_layer_height_fixed())
        profile.insert(profile.end(), { first, first });
    double z = first;
    for (size_t i = 0; i < heights.size();) {
        size_t j = i;
        double z_end = z;
        while (j < heights.size() && std::abs(heights[j] - heights[i]) < 1e-6)
            z_end += heights[j++];
        profile.insert(profile.end(), { z, heights[i], z_end - 0.25 * p.min_height, heights[i] });
        z = z_end;
        i = j;
    }
    size_t fine_layers = 0;
    for (double h : heights)
        fine_layers += h < p.height - 1e-4;
    BOOST_LOG_TRIVIAL(info) << "Whole-part field " << object.model_object()->name << ": " << heights.size() << " layers, "
                            << fine_layers << " finer than " << p.height << " mm";
    return profile;
}

}

std::vector<double> field_layer_profile(const PrintObject &object) { return record(object, layer_profile(object)); }

}
