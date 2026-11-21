#include "ModelArrange.hpp"

#include <cstddef>
#include <algorithm>
#include <iterator>
#include <libslic3r/Model.hpp>
#include <libslic3r/Geometry/ConvexHull.hpp>
#include <libslic3r/Print.hpp>
#include "Arrange.hpp"
#include <utility>
#include "Config.hpp"
#include <set>
#include <string>
#include "Point.hpp"
#include "PrintConfig.hpp"
#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "Polygon.hpp"

#include <limits>
#include <numeric>

namespace Slic3r {

namespace {

bool arrange_curved_mode(NonplanarMode mode)
{
    return curved_tops_mode(mode) || mode == NonplanarMode::ConformalLayers;
}

struct ArrangeReliefSettings
{
    NonplanarMode mode;
    double        max_angle;
    double        min_area;
};

double instance_nonplanar_relief(ModelInstance *instance, const DynamicPrintConfig &config)
{
    const auto *collision = config.option<ConfigOptionEnum<NonplanarCollisionCheck>>("nonplanar_collision_check");
    if (collision == nullptr || collision->value == NonplanarCollisionCheck::Off)
        return 0.;

    const ModelObject *object = instance->get_object();
    DynamicPrintConfig effective = config;
    effective.apply(object->config.get(), true);
    if (effective.opt_enum<RotaryLayout>("nonplanar_kinematics") != RotaryLayout::None || effective.opt_bool("belt_printer"))
        return 0.;
    const bool native_support = effective.opt_bool("enable_support") && effective.opt_enum<SupportMaterialStyle>("support_style") == smsNative;
    const NonplanarMode automatic = native_support ? NonplanarMode::Off : NonplanarMode::NonplanarTop;
    ArrangeReliefSettings base { effective.opt_enum<NonplanarMode>("nonplanar_mode"),
                                 effective.opt_float("nonplanar_curved_max_angle"), effective.opt_float("nonplanar_curved_min_area") };
    if (base.mode == NonplanarMode::Auto)
        base.mode = automatic;

    auto apply_range = [](ArrangeReliefSettings &settings, const DynamicPrintConfig &config, NonplanarMode automatic) {
        if (const auto *mode = config.option<ConfigOptionEnum<NonplanarMode>>("nonplanar_mode")) {
            settings.mode = mode->value;
            if (settings.mode == NonplanarMode::Auto)
                settings.mode = automatic;
        }
        if (const auto *angle = config.option<ConfigOptionFloat>("nonplanar_curved_max_angle"))
            settings.max_angle = angle->value;
        if (const auto *area = config.option<ConfigOptionFloat>("nonplanar_curved_min_area"))
            settings.min_area = area->value;
    };
    auto at = [&](double z) {
        ArrangeReliefSettings out = base;
        for (const auto &[range, config] : object->layer_config_ranges)
            if (z >= range.first && z < range.second)
                apply_range(out, config.get(), automatic);
        return out;
    };
    auto relevant = [&](NonplanarMode mode) { return arrange_curved_mode(mode == NonplanarMode::Auto ? automatic : mode); };
    if (!relevant(base.mode)) {
        bool any = false;
        for (const auto &[range, config] : object->layer_config_ranges) {
            ArrangeReliefSettings settings = base;
            apply_range(settings, config.get(), automatic);
            any |= relevant(settings.mode);
        }
        if (!any)
            return 0.;
    }

    struct Facet { stl_triangle_vertex_indices vertices; double low, high, area, min_area; };
    double relief = 0.;
    const Transform3d matrix = instance->get_matrix();
    for (const ModelVolume *volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        const Transform3d transform = matrix * volume->get_matrix();
        const double winding = transform.linear().determinant() < 0. ? -1. : 1.;
        const indexed_triangle_set &mesh = volume->mesh().its;
        std::vector<Facet> facets;
        for (const stl_triangle_vertex_indices &face : mesh.indices) {
            const Vec3d a = transform * mesh.vertices[face[0]].cast<double>();
            const Vec3d b = transform * mesh.vertices[face[1]].cast<double>();
            const Vec3d c = transform * mesh.vertices[face[2]].cast<double>();
            const Vec3d normal = (b - a).cross(c - a) * winding;
            const double length = normal.norm();
            if (length <= 0. || normal.z() <= 0.)
                continue;
            const double nz = normal.z() / length;
            const double low = std::min({ a.z(), b.z(), c.z() }), high = std::max({ a.z(), b.z(), c.z() });
            std::vector<double> cuts { low, high };
            for (const auto &[range, config] : object->layer_config_ranges)
                for (double z : { range.first, range.second })
                    if (z > low && z < high)
                        cuts.push_back(z);
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            bool eligible = false;
            double min_area = std::numeric_limits<double>::infinity();
            auto accept = [&](double z) {
                const ArrangeReliefSettings settings = at(z);
                if (!relevant(settings.mode))
                    return;
                const double angle = settings.mode == NonplanarMode::ConformalLayers ? std::clamp(settings.max_angle, 1., 45.) :
                                                                                       std::clamp(settings.max_angle, 0., 45.);
                if (nz + 1e-9 < std::cos(Geometry::deg2rad(angle)))
                    return;
                eligible = true;
                min_area = std::min(min_area, std::max(0., settings.min_area));
            };
            accept(cuts.front());
            for (size_t i = 1; i < cuts.size() && !eligible; ++i) {
                accept(cuts[i]);
                if (!eligible && cuts[i] > cuts[i - 1])
                    accept(0.5 * (cuts[i - 1] + cuts[i]));
            }
            if (eligible)
                facets.push_back({ face, low, high, 0.5 * length, min_area });
        }

        std::vector<size_t> parent(facets.size());
        std::iota(parent.begin(), parent.end(), 0);
        auto root = [&](size_t i) { while (parent[i] != i) i = parent[i]; return i; };
        auto join = [&](size_t a, size_t b) { a = root(a), b = root(b); if (a != b) parent[b] = a; };
        std::map<std::pair<int, int>, size_t> edges;
        for (size_t i = 0; i < facets.size(); ++i)
            for (int k = 0; k < 3; ++k) {
                const int a = facets[i].vertices[k], b = facets[i].vertices[(k + 1) % 3];
                auto [it, inserted] = edges.emplace(std::minmax(a, b), i);
                if (inserted)
                    it->second = i;
                else
                    join(i, it->second);
            }
        std::vector<double> low(facets.size(), std::numeric_limits<double>::infinity()), high(facets.size(), -std::numeric_limits<double>::infinity());
        std::vector<double> area(facets.size()), min_area(facets.size(), std::numeric_limits<double>::infinity());
        for (size_t i = 0; i < facets.size(); ++i) {
            const size_t r = root(i);
            low[r] = std::min(low[r], facets[i].low);
            high[r] = std::max(high[r], facets[i].high);
            area[r] += facets[i].area;
            min_area[r] = std::min(min_area[r], facets[i].min_area);
        }
        for (size_t i = 0; i < facets.size(); ++i)
            if (parent[i] == i && area[i] + EPSILON >= min_area[i] && high[i] - low[i] >= 0.03)
                relief = std::max(relief, high[i] - low[i]);
    }
    return relief;
}

}

arrangement::ArrangePolygons get_arrange_polys(const Model &model, ModelInstancePtrs &instances)
{
    size_t count = 0;
    for (auto obj : model.objects) count += obj->instances.size();

    ArrangePolygons input;
    input.reserve(count);
    instances.clear(); instances.reserve(count);
    ArrangePolygon ap;
    for (ModelObject *mo : model.objects)
        for (ModelInstance *minst : mo->instances) {
            minst->get_arrange_polygon(&ap);
            ap.bed_idx = 0;
            input.emplace_back(ap);
            instances.emplace_back(minst);
        }

    return input;
}

bool apply_arrange_polys(ArrangePolygons &input, ModelInstancePtrs &instances, VirtualBedFn vfn)
{
    bool ret = true;

    for(size_t i = 0; i < input.size(); ++i) {
        if (input[i].bed_idx != 0) { ret = false; if (vfn) vfn(input[i]); }
        if (input[i].bed_idx >= 0)
            instances[i]->apply_arrange_result(input[i].translation.cast<double>(),
                                               input[i].rotation);
    }

    return ret;
}

Slic3r::arrangement::ArrangePolygon get_arrange_poly(const Model &model)
{
    ArrangePolygon ap;
    Points &apts = ap.poly.contour.points;
    for (const ModelObject *mo : model.objects)
        for (const ModelInstance *minst : mo->instances) {
            ArrangePolygon obj_ap;
            minst->get_arrange_polygon(&obj_ap);
            ap.poly.contour.rotate(obj_ap.rotation);
            ap.poly.contour.translate(obj_ap.translation.x(), obj_ap.translation.y());
            const Points &pts = obj_ap.poly.contour.points;
            std::copy(pts.begin(), pts.end(), std::back_inserter(apts));
        }

    apts = std::move(Geometry::convex_hull(apts).points);
    return ap;
}

void duplicate(Model &model, Slic3r::arrangement::ArrangePolygons &copies, VirtualBedFn vfn)
{
    for (ModelObject *o : model.objects) {
        ModelInstancePtrs instances = o->instances;
        o->instances.clear();
        for (const ModelInstance *i : instances) {
            for (arrangement::ArrangePolygon &ap : copies) {
                if (ap.bed_idx != 0) vfn(ap);
                ModelInstance *instance = o->add_instance(*i);
                Vec2d pos = unscale(ap.translation);
                instance->set_offset(instance->get_offset() + to_3d(pos, 0.));
            }
        }
        o->invalidate_bounding_box();
    }
}

void duplicate_objects(Model &model, size_t copies_num)
{
    for (ModelObject *o : model.objects) {
        ModelInstancePtrs instances = o->instances;
        for (const ModelInstance *i : instances)
            for (size_t k = 2; k <= copies_num; ++ k)
                o->add_instance(*i);
    }
}

template<class T>
arrangement::ArrangePolygon get_arrange_poly(T obj, const Slic3r::DynamicPrintConfig& config)
{
    ArrangePolygon ap = obj.get_arrange_polygon(config);
    ap.bed_idx = 0;
    ap.setter = [obj](const ArrangePolygon& p) {
        if (p.is_arranged()) {
            Vec2d t = p.translation.cast<double>();
            T{ obj }.apply_arrange_result(t, p.rotation, p.itemid);
        }
    };

    return ap;
}

template<>
arrangement::ArrangePolygon get_arrange_poly(ModelInstance* inst, const Slic3r::DynamicPrintConfig& config)
{
    return get_arrange_poly(PtrWrapper{ inst },config);
}

ArrangePolygon get_instance_arrange_poly(ModelInstance* instance, const Slic3r::DynamicPrintConfig& config)
{
    ArrangePolygon ap = get_arrange_poly(PtrWrapper{ instance }, config);

    if (config.has("curr_bed_type")) {
        ap.bed_temp = 0;
        ap.first_bed_temp = 0;
        BedType curr_bed_type = config.opt_enum<BedType>("curr_bed_type");

        const ConfigOptionInts* bed_opt = config.option<ConfigOptionInts>(get_bed_temp_key(curr_bed_type));
        if (bed_opt != nullptr)
            ap.bed_temp = bed_opt->get_at(ap.extrude_ids.front()-1);

        const ConfigOptionInts* bed_opt_1st_layer = config.option<ConfigOptionInts>(get_bed_temp_1st_layer_key(curr_bed_type));
        if (bed_opt_1st_layer != nullptr)
            ap.first_bed_temp = bed_opt_1st_layer->get_at(ap.extrude_ids.front()-1);
    }

    if (config.has("nozzle_temperature"))
        ap.print_temp = config.opt_int("nozzle_temperature", ap.extrude_ids.front() - 1);
    if (config.has("nozzle_temperature_initial_layer"))
        ap.first_print_temp = config.opt_int("nozzle_temperature_initial_layer", ap.extrude_ids.front() - 1);

    if (config.has("temperature_vitrification")) {
        ap.vitrify_temp = config.opt_int("temperature_vitrification", ap.extrude_ids.front() - 1);
    }

    auto* filament_types_opt = dynamic_cast<const ConfigOptionStrings*>(config.option("filament_type"));
    if (filament_types_opt) {
        std::set<int> filament_temp_types;
        for (auto i : ap.extrude_ids) {
            std::string type_str = filament_types_opt->get_at(i-1);
            int temp_type = Print::get_filament_temp_type(type_str);
            filament_temp_types.insert(temp_type);
        }
        ap.filament_temp_type = Print::get_compatible_filament_type(filament_temp_types);
    }

    auto obj = instance->get_object();

    ap.brim_width = 1.0;
    auto supp_type_ptr = obj->get_config_value<ConfigOptionBool>(config, "enable_support");
    auto support_type_ptr = obj->get_config_value<ConfigOptionEnum<SupportType>>(config, "support_type");
    auto support_type = support_type_ptr->value;
    auto enable_support = supp_type_ptr->getBool();
    int support_int = support_type_ptr->getInt();

    if (enable_support && (support_type == stNormalAuto || support_type == stNormal))
        ap.brim_width = 6.0;
    else if (enable_support) {
        ap.brim_width = 24.0;
        ap.has_tree_support = true;
    }

    auto size = obj->instance_convex_hull_bounding_box(instance).size();
    ap.height = size.z();
    ap.nonplanar_relief = instance_nonplanar_relief(instance, config);
    ap.name = obj->name;
    return ap;
}

}
