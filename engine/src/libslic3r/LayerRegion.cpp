#include "Flow.hpp"
#include "Config.hpp"
#include "ExPolygon.hpp"
#include "ExtrusionEntity.hpp"
#include "Exception.hpp"
#include "Layer.hpp"
#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "Line.hpp"
#include "PerimeterGenerator.hpp"
#include "Nonplanar/OneShotField.hpp"
#include "GCode/ClearanceRepair.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "Surface.hpp"
#include "BoundingBox.hpp"
#include "SVG.hpp"
#include "Algorithm/RegionExpansion.hpp"
#include "libslic3r.h"
#include "Utils.hpp"

#include <cmath>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <iterator>
#include <stdexcept>
#include <math.h>
#include <cassert>
#include <string>
#include <map>
#include <memory>
#include <unordered_map>
#include <cstdint>

#include <boost/log/trivial.hpp>
#include <boost/algorithm/clamp.hpp>
#include <utility>
#include <vector>
#include "ExtrusionEntityCollection.hpp"
#include "MultiMaterialSegmentation.hpp"
#include "SurfaceCollection.hpp"
#include "WaveOverhangs/WaveOverhangs.hpp"

namespace Slic3r {

class NonplanarProjectionIndex {
public:
    struct Ref {
        size_t                si;
        int                   facet_id;
        const NonplanarFacet *facet;
        bool operator<(const Ref &rhs) const { return si < rhs.si || (si == rhs.si && facet_id < rhs.facet_id); }
        bool operator==(const Ref &rhs) const { return si == rhs.si && facet_id == rhs.facet_id; }
    };
    explicit NonplanarProjectionIndex(const NonplanarSurfaces &surfaces)
    {
        for (size_t si = 0; si < surfaces.size(); ++si)
            for (const auto &entry : surfaces[si].mesh) {
                const auto &v = entry.second.vertex;
                const double xmin = std::min({double(v[0].x), double(v[1].x), double(v[2].x)});
                const double xmax = std::max({double(v[0].x), double(v[1].x), double(v[2].x)});
                const double ymin = std::min({double(v[0].y), double(v[1].y), double(v[2].y)});
                const double ymax = std::max({double(v[0].y), double(v[1].y), double(v[2].y)});
                for (int ix = cell(xmin); ix <= cell(xmax); ++ix)
                    for (int iy = cell(ymin); iy <= cell(ymax); ++iy)
                        m_cells[key(ix, iy)].push_back({ si, entry.first, &entry.second });
            }
    }

    std::vector<Ref> query(double xmin, double ymin, double xmax, double ymax) const
    {
        std::vector<Ref> refs;
        for (int ix = cell(xmin); ix <= cell(xmax); ++ix)
            for (int iy = cell(ymin); iy <= cell(ymax); ++iy) {
                const auto it = m_cells.find(key(ix, iy));
                if (it != m_cells.end())
                    refs.insert(refs.end(), it->second.begin(), it->second.end());
            }
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
        return refs;
    }

private:
    static int cell(double value) { return int(std::floor(value / 0.5)); }
    static uint64_t key(int x, int y) {
        return (uint64_t(uint32_t(x)) << 32) | uint32_t(y);
    }
    std::unordered_map<uint64_t, std::vector<Ref>> m_cells;
};

static void mark_nonplanar_provenance(ExtrusionEntity *entity, const Surface &surface, size_t layer_id)
{
    if (auto *path = dynamic_cast<ExtrusionPath*>(entity)) {
        path->nonplanar_source = surface.is_nonplanar();
        path->distance_to_top = surface.is_nonplanar() ? surface.distance_to_top : -1.f;
        path->source_layer_id = surface.source_layer_id != size_t(-1) ? surface.source_layer_id : layer_id;
        path->source_surface_id = surface.source_surface_id;
        if (path->source_path_id == 0)
            path->source_path_id = next_nonplanar_path_id();
    } else if (auto *loop = dynamic_cast<ExtrusionLoop*>(entity)) {
        for (ExtrusionPath &path : loop->paths)
            mark_nonplanar_provenance(&path, surface, layer_id);
    } else if (auto *multi = dynamic_cast<ExtrusionMultiPath*>(entity)) {
        for (ExtrusionPath &path : multi->paths)
            mark_nonplanar_provenance(&path, surface, layer_id);
    } else if (auto *collection = dynamic_cast<ExtrusionEntityCollection*>(entity)) {
        for (ExtrusionEntity *child : collection->entities)
            mark_nonplanar_provenance(child, surface, layer_id);
    }
}



Flow LayerRegion::flow(FlowRole role) const
{
    return this->flow(role, m_layer->height);
}

Flow LayerRegion::flow(FlowRole role, double layer_height) const
{
    return m_region->flow(*m_layer->object(), role, layer_height, m_layer->id() == 0);
}

Flow LayerRegion::bridging_flow(FlowRole role, bool thick_bridge) const
{
    const PrintRegion       &region         = this->region();
    const PrintRegionConfig &region_config  = region.config();
    const PrintObject       &print_object   = *this->layer()->object();
    Flow bridge_flow;
    auto nozzle_diameter = float(nozzle_diameter_for_filament(print_object.print()->config(), region.extruder(role), print_object.print()->is_BBL_printer()));
    const ConfigOptionFloatOrPercent& bridge_width_opt = region_config.bridge_line_width;
    const double                      bridge_width      = bridge_width_opt.get_abs_value(nozzle_diameter);
    const bool                        has_bridge_width  = bridge_width > 0.;
    const double                      bridge_flow_ratio = region_config.bridge_flow;

    if (thick_bridge) {
        float thread_diameter = has_bridge_width ? float(bridge_width) : nozzle_diameter;
        if (bridge_flow_ratio > 0.)
            thread_diameter *= float(sqrt(bridge_flow_ratio));
        bridge_flow = Flow::bridging_flow(thread_diameter, nozzle_diameter);
    } else {
        Flow base_flow = this->flow(role);
        if (has_bridge_width)
            base_flow = Flow(float(bridge_width), base_flow.height(), nozzle_diameter);
        bridge_flow = base_flow.with_flow_ratio(bridge_flow_ratio);
    }
    return bridge_flow;

}

void LayerRegion::slices_to_fill_surfaces_clipped()
{
    std::array<SurfacesPtr, size_t(stCount)> by_surface;
    for (Surface &surface : this->slices.surfaces)
        if (! surface.is_nonplanar())
            by_surface[size_t(surface.surface_type)].emplace_back(&surface);
    this->fill_surfaces.surfaces.clear();
    for (size_t surface_type = 0; surface_type < size_t(stCount); ++ surface_type) {
        const SurfacesPtr &this_surfaces = by_surface[surface_type];
        if (! this_surfaces.empty())
            this->fill_surfaces.append(intersection_ex(this_surfaces, this->fill_expolygons), SurfaceType(surface_type));
    }
    for (const Surface &surface : this->slices.surfaces)
        if (surface.is_nonplanar())
            this->fill_surfaces.append(intersection_ex(surface.expolygon, this->fill_expolygons), surface);
}

void LayerRegion::make_perimeters(const SurfaceCollection &slices, const LayerRegionPtrs &compatible_regions, SurfaceCollection* fill_surfaces, ExPolygons* fill_no_overlap)
{
    this->perimeters.clear();
    this->thin_fills.clear();
    m_nonplanar_perimeters_projected = false;

    SurfaceCollection planar;
    Surfaces          nonplanar;
    for (const Surface &surface : slices.surfaces)
        (surface.is_nonplanar() ? nonplanar : planar.surfaces).push_back(surface);
    if (! nonplanar.empty()) {
        const Surface planar_template(stInternal);
        auto mark_new = [this](ExtrusionEntityCollection &collection, size_t begin, const Surface &surface) {
            for (size_t i = begin; i < collection.entities.size(); ++ i)
                mark_nonplanar_provenance(collection.entities[i], surface, this->layer()->id());
        };
        size_t perimeters_begin = this->perimeters.entities.size(), thin_begin = this->thin_fills.entities.size();
        if (! planar.empty())
            this->make_perimeters_generator(planar, compatible_regions, fill_surfaces, fill_no_overlap, nullptr, true);
        mark_new(this->perimeters, perimeters_begin, planar_template);
        mark_new(this->thin_fills, thin_begin, planar_template);
        for (const Surface &surface : nonplanar) {
            SurfaceCollection one;
            one.surfaces.push_back(surface);
            PrintRegionConfig shell_config = this->region().config();
            if (surface.distance_to_top >= 0.f && surface.distance_to_top < 1e-4f)
                shell_config.wall_loops.value = std::min(shell_config.wall_loops.value, 1);
            else
                shell_config.wall_loops.value = 0;
            perimeters_begin = this->perimeters.entities.size();
            thin_begin       = this->thin_fills.entities.size();
            this->make_perimeters_generator(one, compatible_regions, fill_surfaces, fill_no_overlap, &shell_config, false);
            mark_new(this->perimeters, perimeters_begin, surface);
            mark_new(this->thin_fills, thin_begin, surface);
        }
        return;
    }
    this->make_perimeters_generator(slices, compatible_regions, fill_surfaces, fill_no_overlap, nullptr, true);
    const Surface planar_template(stInternal);
    for (ExtrusionEntity *entity : this->perimeters.entities)
        mark_nonplanar_provenance(entity, planar_template, this->layer()->id());
    for (ExtrusionEntity *entity : this->thin_fills.entities)
        mark_nonplanar_provenance(entity, planar_template, this->layer()->id());
}

void LayerRegion::make_perimeters_generator(const SurfaceCollection &slices, const LayerRegionPtrs &compatible_regions, SurfaceCollection* fill_surfaces, ExPolygons* fill_no_overlap,
                                            const PrintRegionConfig *config_override, bool planar_neighbours)
{

    const PrintConfig       &print_config  = this->layer()->object()->print()->config();
    const PrintRegionConfig &base_config   = config_override ? *config_override : this->region().config();
    const bool               drop_overhang = !planar_neighbours && base_config.detect_overhang_wall.value;
    PrintRegionConfig        no_overhang_config;
    if (drop_overhang) {
        no_overhang_config = base_config;
        no_overhang_config.detect_overhang_wall.value = false;
    }
    const PrintRegionConfig &region_config = drop_overhang ? no_overhang_config : base_config;
    const PrintObjectConfig& object_config = this->layer()->object()->config();
    bool spiral_mode = print_config.spiral_mode &&
        (this->layer()->id() >= size_t(region_config.bottom_shell_layers.value) &&
         this->layer()->print_z >= region_config.bottom_shell_thickness - EPSILON);

    double model_rotation_rad = 0.0;
    if (region_config.align_infill_direction_to_model) {
        auto m = this->layer()->object()->trafo().matrix();
        model_rotation_rad = std::atan2((double)m(1, 0), (double)m(0, 0));
    }

    PerimeterGenerator g(
        &slices,
        &compatible_regions,
        this->layer()->height,
        this->layer()->slice_z,
        this->flow(frPerimeter),
        &region_config,
        &this->layer()->object()->config(),
        &print_config,
        spiral_mode,
        model_rotation_rad,
        
        &this->perimeters,
        &this->thin_fills,
        fill_surfaces,
        fill_no_overlap
    );
    
    g.wave_overhangs = WaveOverhangs::enabled(region_config, *this->layer()->object());
    if (planar_neighbours && this->layer()->upper_layer != NULL)
        g.upper_slices = &this->layer()->upper_layer->lslices;

    int region_id = this->region().print_object_region_id();
    if (planar_neighbours && this->layer()->upper_layer != NULL)
        g.upper_slices_same_region = &this->layer()->upper_layer->get_region(region_id)->slices;

    g.layer_id              = (int)this->layer()->id();
    if (const PrintObject *object = this->layer()->object();
        Nonplanar::oneshot_requested(*object) && !(object->oneshot_state() && object->oneshot_state()->field_path()) &&
        object_config.layer_height.value > 0.)
        g.top_width_scale = std::min(1., this->layer()->height / object_config.layer_height.value);
    g.ext_perimeter_flow    = this->flow(frExternalPerimeter);
    g.overhang_flow         = this->bridging_flow(frPerimeter, object_config.thick_bridges);
    g.solid_infill_flow     = this->flow(frSolidInfill);

    ExPolygons lower_slices;
    if (this->layer()->lower_layer != nullptr) {
        lower_slices   = g.printable_slices(this->layer()->lower_layer->lslices);
        g.lower_slices = &lower_slices;
    }

    if (this->layer()->object()->config().wall_generator.value == PerimeterGeneratorType::Arachne && !spiral_mode)
        g.process_arachne();
    else
        g.process_classic();

    if (! g.out_wave_overhang_floor_polygons.empty())
        append(this->layer()->wave_overhang_floor_polygons, std::move(g.out_wave_overhang_floor_polygons));
    if (! g.out_wave_overhang_covered_polygons.empty())
        append(this->layer()->wave_overhang_covered_polygons, std::move(g.out_wave_overhang_covered_polygons));
}

#if 1

static ExPolygons fill_surfaces_extract_expolygons(Surfaces &surfaces, std::initializer_list<SurfaceType> surface_types, double &thickness)
{
    size_t cnt = 0;
    for (const Surface &surface : surfaces)
        if (std::find(surface_types.begin(), surface_types.end(), surface.surface_type) != surface_types.end()) {
            ++cnt;
            thickness = surface.thickness;
        }
    if (cnt == 0)
        return {};

    ExPolygons out;
    out.reserve(cnt);
    for (Surface &surface : surfaces)
        if (std::find(surface_types.begin(), surface_types.end(), surface.surface_type) != surface_types.end())
            out.emplace_back(std::move(surface.expolygon));
    return out;
}

struct ExpansionZone
{
    ExPolygons                           expolygons;
    Algorithm::RegionExpansionParameters parameters;
    bool                                 expanded_into = false;
};

struct Bridge {
    ExPolygon expolygon;
    uint32_t group_id;
    std::vector<Algorithm::RegionExpansionEx>::const_iterator bridge_expansion_begin;
    std::optional<double> angle{std::nullopt};
};

uint32_t group_id(std::vector<Bridge> &bridges, uint32_t src_id) {
    uint32_t group_id = bridges[src_id].group_id;
    while (group_id != src_id) {
        src_id = group_id;
        group_id = bridges[src_id].group_id;
    }
    bridges[src_id].group_id = group_id;
    return group_id;
};

std::vector<Bridge> get_grouped_bridges(
    ExPolygons&& bridge_expolygons,
    const std::vector<Algorithm::RegionExpansionEx>& bridge_expansions
) {
    using namespace Algorithm;

    std::vector<Bridge> result;
    {
        result.reserve(bridge_expansions.size());
        uint32_t group_id = 0;
        for (ExPolygon& expolygon : bridge_expolygons)
            result.push_back({ std::move(expolygon), group_id ++, bridge_expansions.end() });
    }


    for (auto expansion_iterator = bridge_expansions.begin(); expansion_iterator != bridge_expansions.end();) {
        auto boundary_region_begin = expansion_iterator;
        auto boundary_region_end = std::find_if(
            next(expansion_iterator),
            bridge_expansions.end(),
            [&](const RegionExpansionEx& expansion){
                return expansion.boundary_id != expansion_iterator->boundary_id;
            }
        );

        std::vector<BoundingBox> bounding_boxes;
        bounding_boxes.reserve(std::distance(boundary_region_begin, boundary_region_end));
        std::transform(
            boundary_region_begin,
            boundary_region_end,
            std::back_inserter(bounding_boxes),
            [](const RegionExpansionEx& expansion){
                return get_extents(expansion.expolygon.contour);
            }
        );

        for (;expansion_iterator != boundary_region_end; ++expansion_iterator) {
            auto candidate_iterator = std::next(expansion_iterator);
            for (;candidate_iterator != boundary_region_end; ++candidate_iterator) {
                const BoundingBox& current_bounding_box{
                    bounding_boxes[expansion_iterator - boundary_region_begin]
                };
                const BoundingBox& candidate_bounding_box{
                    bounding_boxes[candidate_iterator - boundary_region_begin]
                };
                if (
                    expansion_iterator->src_id != candidate_iterator->src_id
                    && current_bounding_box.overlap(candidate_bounding_box)
                    && !intersection(expansion_iterator->expolygon.contour, candidate_iterator->expolygon.contour).empty()
                ) {
                    uint32_t id  = group_id(result, expansion_iterator->src_id);
                    uint32_t id2 = group_id(result, candidate_iterator->src_id);
                    if (id < id2)
                        result[id2].group_id = id;
                    else
                        result[id].group_id = id2;
                }
            }
        }
    }
    return result;
}

void detect_bridge_directions(
    const Algorithm::WaveSeeds& bridge_anchors,
    std::vector<Bridge>& bridges,
    const std::vector<ExpansionZone>& expansion_zones
) {
    if (expansion_zones.empty()) {
        throw std::runtime_error("At least one expansion zone must exist!");
    }
    auto it_bridge_anchor = bridge_anchors.begin();
    for (uint32_t bridge_id = 0; bridge_id < uint32_t(bridges.size()); ++ bridge_id) {
        Bridge &bridge = bridges[bridge_id];
        Polygons anchor_areas;
        int32_t last_anchor_id = -1;
        for (; it_bridge_anchor != bridge_anchors.end() && it_bridge_anchor->src == bridge_id; ++ it_bridge_anchor) {
            if (last_anchor_id != int(it_bridge_anchor->boundary)) {
                last_anchor_id = int(it_bridge_anchor->boundary);

                unsigned start_index{};
                unsigned end_index{};
                for (const ExpansionZone& expansion_zone: expansion_zones) {
                    end_index += expansion_zone.expolygons.size();
                    if (last_anchor_id < static_cast<int64_t>(end_index)) {
                        append(anchor_areas, to_polygons(expansion_zone.expolygons[last_anchor_id - start_index]));
                        break;
                    }
                    start_index += expansion_zone.expolygons.size();
                }
            }
        }
        Lines lines{to_lines(diff_pl(to_polylines(bridge.expolygon), expand(anchor_areas, float(SCALED_EPSILON))))};
        auto [bridging_dir, unsupported_dist] = detect_bridging_direction(lines, to_polygons(bridge.expolygon));
        bridge.angle = M_PI + std::atan2(bridging_dir.y(), bridging_dir.x());

        if constexpr (false) {
            coordf_t    stroke_width = scale_(0.06);
            BoundingBox bbox         = get_extents(anchor_areas);
            bbox.merge(get_extents(bridge.expolygon));
            bbox.offset(scale_(1.));
            ::Slic3r::SVG
                svg(debug_out_path(("bridge" + std::to_string(*bridge.angle) + "_" ).c_str()),
                bbox);
            svg.draw(bridge.expolygon, "cyan");
            svg.draw(lines, "green", stroke_width);
            svg.draw(anchor_areas, "red");
        }
    }
}

Surfaces merge_bridges(
    std::vector<Bridge>& bridges,
    const std::vector<Algorithm::RegionExpansionEx>& bridge_expansions,
    const float closing_radius
) {
    for (auto it = bridge_expansions.begin(); it != bridge_expansions.end(); ) {
        bridges[it->src_id].bridge_expansion_begin = it;
        uint32_t src_id = it->src_id;
        for (++ it; it != bridge_expansions.end() && it->src_id == src_id; ++ it) ;
    }

    Surfaces result;
    for (uint32_t bridge_id = 0; bridge_id < uint32_t(bridges.size()); ++ bridge_id) {
        if (group_id(bridges, bridge_id) == bridge_id) {
            Polygons acc;
            for (uint32_t bridge_id2 = bridge_id; bridge_id2 < uint32_t(bridges.size()); ++ bridge_id2)
                if (group_id(bridges, bridge_id2) == bridge_id) {
                    append(acc, to_polygons(std::move(bridges[bridge_id2].expolygon)));
                    auto it_bridge_expansion = bridges[bridge_id2].bridge_expansion_begin;
                    assert(it_bridge_expansion == bridge_expansions.end() || it_bridge_expansion->src_id == bridge_id2);
                    for (; it_bridge_expansion != bridge_expansions.end() && it_bridge_expansion->src_id == bridge_id2; ++ it_bridge_expansion)
                        append(acc, to_polygons(it_bridge_expansion->expolygon));
                }
            if (!bridges[bridge_id].angle) {
                assert(false && "Bridge angle must be pre-calculated!");
            }
            Surface templ{ stBottomBridge, {} };
            templ.bridge_angle = bridges[bridge_id].angle ? *bridges[bridge_id].angle : -1;
            ExPolygons final = closing_ex(acc, closing_radius);
            for (ExPolygon &ex : final)
                result.emplace_back(templ, std::move(ex));
        }
    }
    return result;
}

struct ExpansionResult {
    Algorithm::WaveSeeds anchors;
    std::vector<Algorithm::RegionExpansionEx> expansions;
};

ExpansionResult expand_expolygons(
    const ExPolygons& expolygons,
    std::vector<ExpansionZone>& expansion_zones
) {
    using namespace Algorithm;
    WaveSeeds bridge_anchors;
    std::vector<RegionExpansionEx> bridge_expansions;

    unsigned processed_bridges_count = 0;
    for (ExpansionZone& expansion_zone : expansion_zones) {
        WaveSeeds seeds{wave_seeds(
            expolygons,
            expansion_zone.expolygons,
            expansion_zone.parameters.tiny_expansion,
            true
        )};
        std::vector<RegionExpansionEx> expansions{propagate_waves_ex(
            seeds,
            expansion_zone.expolygons,
            expansion_zone.parameters
        )};

        for (WaveSeed &seed : seeds)
            seed.boundary += processed_bridges_count;
        for (RegionExpansionEx &expansion : expansions)
            expansion.boundary_id += processed_bridges_count;

        expansion_zone.expanded_into = ! expansions.empty();

        append(bridge_anchors, std::move(seeds));
        append(bridge_expansions, std::move(expansions));

        processed_bridges_count += expansion_zone.expolygons.size();
    }
    return {bridge_anchors, bridge_expansions};
}

Surfaces expand_bridges_detect_orientations(
    Surfaces &surfaces,
    std::vector<ExpansionZone>& expansion_zones,
    const float closing_radius
)
{

    double thickness;
    ExPolygons bridge_expolygons = fill_surfaces_extract_expolygons(surfaces, {stBottomBridge}, thickness);
    if (bridge_expolygons.empty())
        return {};

    ExpansionResult expansion_result{expand_expolygons(
        bridge_expolygons,
        expansion_zones
    )};

    std::vector<Bridge> bridges{get_grouped_bridges(
        std::move(bridge_expolygons),
        expansion_result.expansions
    )};
    bridge_expolygons.clear();

    std::sort(expansion_result.anchors.begin(), expansion_result.anchors.end(), Algorithm::lower_by_src_and_boundary);
    detect_bridge_directions(expansion_result.anchors, bridges, expansion_zones);

    std::sort(expansion_result.expansions.begin(), expansion_result.expansions.end(), [](auto &l, auto &r) {
        return l.src_id < r.src_id || (l.src_id == r.src_id && l.boundary_id < r.boundary_id);
    });
    Surfaces out{merge_bridges(bridges, expansion_result.expansions, closing_radius)};

    for (ExpansionZone& expansion_zone : expansion_zones)
        if (expansion_zone.expanded_into)
            expansion_zone.expolygons = diff_ex(expansion_zone.expolygons, out);
    return out;
}

Surfaces expand_merge_surfaces(
    Surfaces &surfaces,
    SurfaceType surface_type,
    std::vector<ExpansionZone>& expansion_zones,
    const float closing_radius,
    const double bridge_angle = -1
)
{
    using namespace Slic3r::Algorithm;

    double thickness;
    ExPolygons src = fill_surfaces_extract_expolygons(surfaces, {surface_type}, thickness);
    if (src.empty())
        return {};

    unsigned processed_expolygons_count = 0;
    std::vector<RegionExpansion> expansions;
    for (ExpansionZone& expansion_zone : expansion_zones) {
        std::vector<RegionExpansion> zone_expansions = propagate_waves(src, expansion_zone.expolygons, expansion_zone.parameters);
        expansion_zone.expanded_into = !zone_expansions.empty();

        for (RegionExpansion &expansion : zone_expansions)
            expansion.boundary_id += processed_expolygons_count;

        processed_expolygons_count += expansion_zone.expolygons.size();
        append(expansions, std::move(zone_expansions));
    }

    std::vector<ExPolygon> expanded = merge_expansions_into_expolygons(std::move(src), std::move(expansions));
    expanded = closing_ex(expanded, closing_radius);
    for (ExpansionZone& expansion_zone : expansion_zones)
        if (expansion_zone.expanded_into)
            expansion_zone.expolygons = diff_ex(expansion_zone.expolygons, expanded);

    Surface templ{ surface_type, {} };
    templ.bridge_angle = bridge_angle;
    Surfaces out;
    out.reserve(expanded.size());
    for (auto &expoly : expanded)
        out.emplace_back(templ, std::move(expoly));
    return out;
}

void LayerRegion::process_external_surfaces(const Layer *lower_layer, const Polygons *lower_layer_covered)
{
    using namespace Slic3r::Algorithm;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-initial");
#endif
    float shell_width = 0;
    float expansion_min = 0;
    if (int num_perimeters = this->region().config().wall_loops; num_perimeters > 0) {
        Flow external_perimeter_flow = this->flow(frExternalPerimeter);
        Flow perimeter_flow          = this->flow(frPerimeter);
        shell_width  = 0.5f * external_perimeter_flow.scaled_width() + external_perimeter_flow.scaled_spacing();
        shell_width += perimeter_flow.scaled_spacing() * (num_perimeters - 1);
        expansion_min = perimeter_flow.scaled_spacing();
    } else {
        shell_width   = float(SCALED_EPSILON);
        expansion_min = float(SCALED_EPSILON);;
    }

    float                           expansion_top           = shell_width * sqrt(2.);
    float                           expansion_bottom        = expansion_top;
    float                           expansion_bottom_bridge = expansion_top;
    const float                     expansion_step          = scaled<float>(0.1);
    static constexpr const size_t   max_nr_expansion_steps  = 5;
    const float closing_radius = 0.55f * 0.65f * 1.05f * this->flow(frSolidInfill).scaled_spacing();

    Surfaces nonplanar_shells;
    for (Surface &surface : this->fill_surfaces.surfaces)
        if (surface.is_nonplanar())
            nonplanar_shells.emplace_back(std::move(surface));
    this->fill_surfaces.surfaces.erase(
        std::remove_if(this->fill_surfaces.surfaces.begin(), this->fill_surfaces.surfaces.end(),
            [](const Surface &surface) { return surface.is_nonplanar(); }),
        this->fill_surfaces.surfaces.end());

    double     layer_thickness;
    ExPolygons shells = union_ex(fill_surfaces_extract_expolygons(this->fill_surfaces.surfaces, { stInternalSolid }, layer_thickness));
    ExPolygons sparse = union_ex(fill_surfaces_extract_expolygons(this->fill_surfaces.surfaces, {stInternal}, layer_thickness));
    ExPolygons top_expolygons = union_ex(fill_surfaces_extract_expolygons(this->fill_surfaces.surfaces, {stTop}, layer_thickness));
    const auto expansion_params_into_sparse_infill = RegionExpansionParameters::build(expansion_min, expansion_step, max_nr_expansion_steps);
    const auto expansion_params_into_solid_infill  = RegionExpansionParameters::build(expansion_bottom_bridge, expansion_step, max_nr_expansion_steps);

    std::vector<ExpansionZone> expansion_zones{
        ExpansionZone{std::move(shells), expansion_params_into_solid_infill},
        ExpansionZone{std::move(sparse), expansion_params_into_sparse_infill},
        ExpansionZone{std::move(top_expolygons), expansion_params_into_solid_infill},
    };

    SurfaceCollection bridges;
    {
        BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges. layer" << this->layer()->print_z;
        const auto  &region_config    = this->region().config();
        const double custom_angle_deg = region_config.bridge_angle.value;
        const bool   relative_angle   = region_config.relative_bridge_angle.value;
        const double custom_angle_rad = Geometry::deg2rad(custom_angle_deg);

        double align_offset_rad = 0.0;
        if (region_config.align_infill_direction_to_model) {
            auto m = this->layer()->object()->trafo().matrix();
            align_offset_rad = std::atan2((double)m(1, 0), (double)m(0, 0));
        }

        bridges.surfaces = (custom_angle_deg > 0.0 && !relative_angle) ?
            expand_merge_surfaces(this->fill_surfaces.surfaces, stBottomBridge, expansion_zones, closing_radius, custom_angle_rad + align_offset_rad) :
            expand_bridges_detect_orientations(this->fill_surfaces.surfaces, expansion_zones, closing_radius);
        if (custom_angle_deg > 0.0 && relative_angle) {
            for (Surface &bridge_surface : bridges.surfaces) {
                if (bridge_surface.bridge_angle >= 0)
                    bridge_surface.bridge_angle += custom_angle_rad;
            }
        }
        BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges - done";
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        {
            static int iRun = 0;
            bridges.export_to_svg(debug_out_path("bridges-after-grouping-%d.svg", iRun++).c_str(), true);
        }
#endif
    }

    this->fill_surfaces.remove_types({stTop});
    {
        Surface top_templ(stTop, {});
        top_templ.thickness = layer_thickness;
        this->fill_surfaces.append(std::move(expansion_zones.back().expolygons), top_templ);
    }

    expansion_zones.pop_back();

    expansion_zones.at(0).parameters = RegionExpansionParameters::build(expansion_bottom, expansion_step, max_nr_expansion_steps);
    Surfaces bottoms = expand_merge_surfaces(this->fill_surfaces.surfaces, stBottom, expansion_zones, closing_radius);

    expansion_zones.at(0).parameters = RegionExpansionParameters::build(expansion_top, expansion_step, max_nr_expansion_steps);
    Surfaces tops = expand_merge_surfaces(this->fill_surfaces.surfaces, stTop, expansion_zones, closing_radius);

    if (!this->layer()->object()->print()->config().spiral_mode && this->region().config().sparse_infill_density.value > 0) {
        double min_area = scale_(scale_(this->region().config().minimum_sparse_infill_area.value));
        ExPolygons small_regions{};
        expansion_zones[1].expolygons.erase(std::remove_if(expansion_zones[1].expolygons.begin(), expansion_zones[1].expolygons.end(), [min_area, &small_regions](ExPolygon& ex_polygon) {
            if (ex_polygon.area() <= min_area) {
                small_regions.push_back(ex_polygon);
                return true;
            }
            return false;
        }), expansion_zones[1].expolygons.end());

        if (!small_regions.empty()) {
            expansion_zones[0].expolygons = union_ex(expansion_zones[0].expolygons, small_regions);
        }
    }

    this->fill_surfaces.clear();
    unsigned zones_expolygons_count = 0;
    for (const ExpansionZone& zone : expansion_zones)
        zones_expolygons_count += zone.expolygons.size();
    reserve_more(this->fill_surfaces.surfaces, zones_expolygons_count + bridges.size() + bottoms.size() + tops.size() + nonplanar_shells.size());
    {
        Surface solid_templ(stInternalSolid, {});
        solid_templ.thickness = layer_thickness;
        this->fill_surfaces.append(std::move(expansion_zones[0].expolygons), solid_templ);
    }
    {
        Surface sparse_templ(stInternal, {});
        sparse_templ.thickness = layer_thickness;
        this->fill_surfaces.append(std::move(expansion_zones[1].expolygons), sparse_templ);
    }
    this->fill_surfaces.append(std::move(bridges.surfaces));
    this->fill_surfaces.append(std::move(bottoms));
    this->fill_surfaces.append(std::move(tops));
    this->fill_surfaces.append(std::move(nonplanar_shells));

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-final");
#endif
}
#else

#define EXTERNAL_SURFACES_OFFSET_PARAMETERS jtSquare, 0.

void LayerRegion::process_external_surfaces(const Layer *lower_layer, const Polygons *lower_layer_covered)
{
    const bool      has_infill = this->region().config().sparse_infill_density.value > 0.;
    auto nozzle_diameter = this->region().nozzle_dmr_avg(this->layer()->object()->print()->config());
    const float margin = float(scale_(EXTERNAL_INFILL_MARGIN));
    const float bridge_margin = std::min(float(scale_(BRIDGE_INFILL_MARGIN)), float(scale_(nozzle_diameter * BRIDGE_INFILL_MARGIN / 0.4)));

    const PrintObjectConfig& object_config = this->layer()->object()->config();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("3_process_external_surfaces-initial");
#endif
    Surfaces                    bottom;
    Surfaces                    bridges;
    Surfaces                    top;
    Surfaces                    internal;
    Polygons                    fill_boundaries = to_polygons(this->fill_expolygons);
    Polygons  					lower_layer_covered_tmp;

    {
        Polygons voids;

        double max_grid_area = -1;
    if (planar_neighbours && this->layer()->lower_layer != nullptr)
            max_grid_area = this->layer()->lower_layer->get_sparse_infill_max_void_area();
        for (const Surface &surface : this->fill_surfaces.surfaces) {
            if (surface.is_top()) {
                if (max_grid_area < 0 || surface.expolygon.area() < max_grid_area)
                    surfaces_append(top, offset_ex(surface.expolygon, margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS), surface);
                else
                    surfaces_append(top, intersection_ex(offset(surface.expolygon.contour, margin / 3.0, EXTERNAL_SURFACES_OFFSET_PARAMETERS),
                                                         offset_ex(surface.expolygon, margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS)), surface);
            } else if (surface.surface_type == stBottom || (surface.surface_type == stBottomBridge && lower_layer == nullptr)) {
                surfaces_append(bottom, offset_ex(surface.expolygon, margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS), surface);
            } else if (surface.surface_type == stBottomBridge) {
                if (! surface.empty())
                    bridges.emplace_back(surface);
            }
            if (surface.is_internal()) {
            	assert(surface.surface_type == stInternal || surface.surface_type == stInternalSolid);
            	if (! has_infill && lower_layer != nullptr)
            		polygons_append(voids, surface.expolygon);
            	internal.emplace_back(std::move(surface));
            }
        }
        if (! has_infill && lower_layer != nullptr && ! voids.empty()) {
            if (lower_layer_covered == nullptr) {
            	lower_layer_covered = &lower_layer_covered_tmp;
            	lower_layer_covered_tmp = to_polygons(lower_layer->lslices);
            }
            if (! lower_layer_covered->empty())
            	voids = diff(voids, *lower_layer_covered);
            fill_boundaries = diff(fill_boundaries, voids);
        }
    }

#if 0
    {
        static int iRun = 0;
        bridges.export_to_svg(debug_out_path("bridges-before-grouping-%d.svg", iRun ++), true);
    }
#endif

    if (bridges.empty())
    {
        fill_boundaries = union_safety_offset(fill_boundaries);
    } else
    {
        ExPolygons               fill_boundaries_ex = union_safety_offset_ex(fill_boundaries);
        std::vector<Polygons>    bridges_grown;
        std::vector<BoundingBox> bridge_bboxes;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        {
            static int iRun = 0;
            SVG svg(debug_out_path("3_process_external_surfaces-fill_regions-%d.svg", iRun ++).c_str(), get_extents(fill_boundaries_ex));
            svg.draw(fill_boundaries_ex);
            svg.draw_outline(fill_boundaries_ex, "black", "blue", scale_(0.05)); 
            svg.Close();
        }

#endif
 
        {
            std::vector<BoundingBox> fill_boundaries_ex_bboxes = get_extents_vector(fill_boundaries_ex);
            bridges_grown.reserve(bridges.size());
            bridge_bboxes.reserve(bridges.size());
            for (size_t i = 0; i < bridges.size(); ++ i) {
                const Point pt = bridges[i].expolygon.contour.points.front();
                int idx_island = -1;
                for (int j = 0; j < int(fill_boundaries_ex.size()); ++ j)
                    if (fill_boundaries_ex_bboxes[j].contains(pt) && 
                        fill_boundaries_ex[j].contains(pt)) {
                        idx_island = j;
                        break;
                    }
                Polygons polys = offset2({ bridges[i].expolygon }, -scale_(nozzle_diameter * 0.1), bridge_margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                if (idx_island == -1) {
				    BOOST_LOG_TRIVIAL(trace) << "Bridge did not fall into the source region!";
                } else {
                    polys = intersection(polys, fill_boundaries_ex[idx_island]);
                }
                bridge_bboxes.push_back(get_extents(polys));
                bridges_grown.push_back(std::move(polys));
            }
        }

        std::vector<size_t> bridge_group(bridges.size(), (size_t)-1);
        size_t n_groups = 0; 
        for (size_t i = 0; i < bridges.size(); ++ i) {
            size_t group_id = (bridge_group[i] == size_t(-1)) ? (n_groups ++) : bridge_group[i];
            bridge_group[i] = group_id;
            for (size_t j = i + 1; j < bridges.size(); ++ j) {
                if (! bridge_bboxes[i].overlap(bridge_bboxes[j]))
                    continue;
                if (intersection(bridges_grown[i], bridges_grown[j]).empty())
                    continue;
                if (bridge_group[j] != size_t(-1)) {
                    size_t group_id_new = bridge_group[j];
                    for (size_t k = 0; k < j; ++ k)
                        if (bridge_group[k] == group_id)
                            bridge_group[k] = group_id_new;
                    group_id = group_id_new;
                }
                bridge_group[j] = group_id;
            }
        }

        {
			BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges. layer" << this->layer()->print_z << ", bridge groups: " << n_groups;
            for (size_t group_id = 0; group_id < n_groups; ++ group_id) {
                size_t n_bridges_merged = 0;
                size_t idx_last = (size_t)-1;
                for (size_t i = 0; i < bridges.size(); ++ i) {
                    if (bridge_group[i] == group_id) {
                        ++ n_bridges_merged;
                        idx_last = i;
                    }
                }
                if (n_bridges_merged == 0)
                    continue;
                ExPolygons  initial;
                Polygons    grown;
                for (size_t i = 0; i < bridges.size(); ++ i) {
                    if (bridge_group[i] != group_id)
                        continue;
                    initial.push_back(std::move(bridges[i].expolygon));
                    polygons_append(grown, bridges_grown[i]);
                }
                const auto &region_config   = this->region().config();
                const double custom_angle_deg = region_config.bridge_angle.value;
                const bool   relative_angle   = region_config.relative_bridge_angle.value;
                const double custom_angle_rad = Geometry::deg2rad(custom_angle_deg);

                double align_offset_rad = 0.0;
                if (region_config.align_infill_direction_to_model) {
                    auto m = this->layer()->object()->trafo().matrix();
                    align_offset_rad = std::atan2((double)m(1, 0), (double)m(0, 0));
                }

                if (custom_angle_deg > 0.0 && !relative_angle) {
                    bridges[idx_last].bridge_angle = custom_angle_rad + align_offset_rad;
                } else {
                    auto [bridging_dir, unsupported_dist] = detect_bridging_direction(to_polygons(initial), to_polygons(lower_layer->lslices));
                    bridges[idx_last].bridge_angle = PI + std::atan2(bridging_dir.y(), bridging_dir.x());
                    if (custom_angle_deg > 0.0 && relative_angle)
                        bridges[idx_last].bridge_angle += custom_angle_rad;
                }

                surfaces_append(bottom, union_safety_offset_ex(grown), bridges[idx_last]);
            }

            fill_boundaries = to_polygons(fill_boundaries_ex);
			BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges - done";
		}

    #if 0
        {
            static int iRun = 0;
            bridges.export_to_svg(debug_out_path("bridges-after-grouping-%d.svg", iRun ++), true);
        }
    #endif
    }

    Surfaces new_surfaces;
    {
        surfaces_append(top, std::move(bottom));
        Polygons bottom_polygons = to_polygons(bottom);
        for (size_t i = 0; i < top.size(); ++ i) {
            Surface &s1 = top[i];
            if (s1.empty())
                continue;
            Polygons polys;
            polygons_append(polys, to_polygons(std::move(s1)));
            for (size_t j = i + 1; j < top.size(); ++ j) {
                Surface &s2 = top[j];
                if (! s2.empty() && surfaces_could_merge(s1, s2)) {
                    polygons_append(polys, to_polygons(std::move(s2)));
                    s2.clear();
                }
            }
            if (s1.is_top())
                polys = diff(polys, bottom_polygons);
            surfaces_append(
                new_surfaces,
                intersection_ex(polys, fill_boundaries),
                s1);
        }
    }
    
    Polygons new_polygons = to_polygons(new_surfaces);
    for (size_t i = 0; i < internal.size(); ++ i) {
        Surface &s1 = internal[i];
        if (s1.empty())
            continue;
        Polygons polys;
        polygons_append(polys, to_polygons(std::move(s1)));
        for (size_t j = i + 1; j < internal.size(); ++ j) {
            Surface &s2 = internal[j];
            if (! s2.empty() && surfaces_could_merge(s1, s2)) {
                polygons_append(polys, to_polygons(std::move(s2)));
                s2.clear();
            }
        }
        ExPolygons new_expolys = diff_ex(polys, new_polygons);
        polygons_append(new_polygons, to_polygons(new_expolys));
        surfaces_append(new_surfaces, std::move(new_expolys), s1);
    }
    
    this->fill_surfaces.surfaces = std::move(new_surfaces);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("3_process_external_surfaces-final");
#endif
}
#endif

void LayerRegion::prepare_fill_surfaces()
{
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-initial");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-initial");
#endif
    
    bool spiral_mode = this->layer()->object()->print()->config().spiral_mode;

    if (! spiral_mode && this->region().config().top_shell_layers == 0) {
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.is_top())
                surface.surface_type = PrintObject::infill_only_where_needed ? stInternalVoid : stInternal;
    }
    if (this->region().config().bottom_shell_layers == 0) {
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.is_bottom())
                surface.surface_type = stInternal;
    }

    if (!spiral_mode && fabs(this->region().config().sparse_infill_density.value - 100.) < EPSILON) {
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.surface_type == stInternal)
                surface.surface_type = stInternalSolid;
    }

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-final");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-final");
#endif
}

double LayerRegion::infill_area_threshold() const
{
    double ss = this->flow(frSolidInfill).scaled_spacing();
    return ss*ss;
}

void LayerRegion::trim_surfaces(const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices.surfaces)
        assert(surface.surface_type == stInternal);
#endif
	this->slices.set(intersection_ex(this->slices.surfaces, trimming_polygons), stInternal);
}

void LayerRegion::elephant_foot_compensation_step(const float elephant_foot_compensation_perimeter_step, const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices.surfaces)
        assert(surface.surface_type == stInternal);
#endif
    Polygons tmp = intersection(this->slices.surfaces, trimming_polygons);
    append(tmp, diff(this->slices.surfaces, opening(this->slices.surfaces, elephant_foot_compensation_perimeter_step)));
    this->slices.set(union_ex(tmp), stInternal);
}

void LayerRegion::export_region_slices_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (Surfaces::const_iterator surface = this->slices.surfaces.begin(); surface != this->slices.surfaces.end(); ++surface)
        bbox.merge(get_extents(surface->expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (Surfaces::const_iterator surface = this->slices.surfaces.begin(); surface != this->slices.surfaces.end(); ++surface)
        svg.draw(surface->expolygon, surface_type_to_color_name(surface->surface_type), transparency);
    for (Surfaces::const_iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface)
        svg.draw(surface->expolygon.lines(), surface_type_to_color_name(surface->surface_type));
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

void LayerRegion::export_region_slices_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_slices_to_svg(debug_out_path("LayerRegion-slices-%s-%d.svg", name, idx ++).c_str());
}

void LayerRegion::export_region_fill_surfaces_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (Surfaces::const_iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface)
        bbox.merge(get_extents(surface->expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const Surface &surface : this->fill_surfaces.surfaces) {
        svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
        svg.draw_outline(surface.expolygon, "black", "blue", scale_(0.05)); 
    }
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

void LayerRegion::export_region_fill_surfaces_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_fill_surfaces_to_svg(debug_out_path("LayerRegion-fill_surfaces-%s-%d.svg", name, idx ++).c_str());
}

void LayerRegion::simplify_entity_collection(ExtrusionEntityCollection* entity_collection)
{
    for (size_t i = 0; i < entity_collection->entities.size(); i++) {
        if (ExtrusionEntityCollection* collection = dynamic_cast<ExtrusionEntityCollection*>(entity_collection->entities[i]))
            this->simplify_entity_collection(collection);
        else if (ExtrusionPath* path = dynamic_cast<ExtrusionPath*>(entity_collection->entities[i]))
            this->simplify_path(path);
        else if (ExtrusionMultiPath* multipath = dynamic_cast<ExtrusionMultiPath*>(entity_collection->entities[i]))
            this->simplify_multi_path(multipath);
        else if (ExtrusionLoop* loop = dynamic_cast<ExtrusionLoop*>(entity_collection->entities[i]))
            this->simplify_loop(loop);
        else
            throw Slic3r::InvalidArgument("Invalid extrusion entity supplied to simplify_entity_collection()");
    }
}

void LayerRegion::simplify_path(ExtrusionPath* path)
{
    const auto &print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    if (enable_arc_fitting &&
        !spiral_mode) {
        if (path->role() == erInternalInfill)
            path->simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
        else
            path->simplify_by_fitting_arc(scaled_resolution);
    } else {
        path->simplify(scaled_resolution);
    }
}

void LayerRegion::simplify_multi_path(ExtrusionMultiPath* multipath)
{
    const auto &print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < multipath->paths.size(); ++i) {
        if (enable_arc_fitting &&
            !spiral_mode) {
            if (multipath->paths[i].role() == erInternalInfill)
                multipath->paths[i].simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
            else
                multipath->paths[i].simplify_by_fitting_arc(scaled_resolution);
        } else {
            multipath->paths[i].simplify(scaled_resolution);
        }
    }
}

void LayerRegion::simplify_loop(ExtrusionLoop* loop)
{
    const auto &print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < loop->paths.size(); ++i) {
        if (enable_arc_fitting &&
            !spiral_mode) {
            if (loop->paths[i].role() == erInternalInfill)
                loop->paths[i].simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
            else
                loop->paths[i].simplify_by_fitting_arc(scaled_resolution);
        } else {
            loop->paths[i].simplify(scaled_resolution);
        }
    }
}


void
LayerRegion::append_nonplanar_surface(const NonplanarSurface& surface)
{
    for(auto & s : m_nonplanar_surfaces){
        if (s == surface){
            return;
        }
    }
    m_nonplanar_surfaces.push_back(surface);
}

void LayerRegion::project_nonplanar_extrusion(ExtrusionEntityCollection *collection, const NonplanarProjectionIndex &index)
{
    ExtrusionEntityCollection input, output;
    output.entities.reserve(collection->entities.size());
    input.entities.swap(collection->entities);
    auto append = [&output](std::unique_ptr<ExtrusionEntity> entity) {
        output.entities.push_back(entity.get());
        entity.release();
    };
    for (ExtrusionEntity *&slot : input.entities) {
        std::unique_ptr<ExtrusionEntity> entity(std::exchange(slot, nullptr));
        if (auto *path = dynamic_cast<ExtrusionPath*>(entity.get())) {
            auto pieces = project_nonplanar_path(*path, index);
            if (pieces.size() == 1) {
                *path = std::move(pieces.front());
                append(std::move(entity));
            } else {
                for (auto &piece : pieces) append(std::make_unique<ExtrusionPath>(std::move(piece)));
            }
        } else if (auto *loop = dynamic_cast<ExtrusionLoop*>(entity.get())) {
            ExtrusionPaths projected;
            bool split = false;
            for (const ExtrusionPath &path : loop->paths) {
                auto pieces = project_nonplanar_path(path, index);
                split |= pieces.size() != 1;
                for (auto &piece : pieces) projected.emplace_back(std::move(piece));
            }
            if (split) {
                for (auto &piece : projected) append(std::make_unique<ExtrusionPath>(std::move(piece)));
            } else {
                loop->paths = std::move(projected);
                append(std::move(entity));
            }
        } else if (auto *multi = dynamic_cast<ExtrusionMultiPath*>(entity.get())) {
            ExtrusionPaths projected;
            bool split = false;
            for (const ExtrusionPath &path : multi->paths) {
                auto pieces = project_nonplanar_path(path, index);
                split |= pieces.size() != 1;
                for (auto &piece : pieces) projected.emplace_back(std::move(piece));
            }
            if (split) {
                for (auto &piece : projected) append(std::make_unique<ExtrusionPath>(std::move(piece)));
            } else {
                multi->paths = std::move(projected);
                append(std::move(entity));
            }
        } else if (auto *nested = dynamic_cast<ExtrusionEntityCollection*>(entity.get())) {
            project_nonplanar_extrusion(nested, index);
            append(std::move(entity));
        } else {
            append(std::move(entity));
        }
    }
    collection->entities.swap(output.entities);
}

static void check_nonplanar_projected(const ExtrusionEntity *entity, const Layer &layer)
{
    const size_t layer_id = layer.id();
    auto check_path = [&layer, layer_id](const ExtrusionPath &path) {
        if (! path.nonplanar_source)
            return;
        for (const Point3 &p : path.polyline.points)
            if (p.z() <= 0) {
                const PrintObject &object = *layer.object();
                if (curved_tops_mode(object.config().nonplanar_mode.value) && object.config().nonplanar_curved_max_angle.value > 12.)
                    throw Nonplanar::CurvedSurfacesRetry(object.id().id, path.source_surface_id >= 0 ? std::vector<int>{ path.source_surface_id } : std::vector<int>{});
                throw_nonplanar_internal(nonplanar_location(layer.print_z, layer_id),
                    "path " + std::to_string(path.source_path_id) + " was not projected onto its surface", layer.object()->id().id);
            }
    };
    if (auto *path = dynamic_cast<const ExtrusionPath*>(entity))
        check_path(*path);
    else if (auto *loop = dynamic_cast<const ExtrusionLoop*>(entity))
        for (const ExtrusionPath &path : loop->paths) check_path(path);
    else if (auto *multi = dynamic_cast<const ExtrusionMultiPath*>(entity))
        for (const ExtrusionPath &path : multi->paths) check_path(path);
    else if (auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(entity))
        for (const ExtrusionEntity *child : collection->entities) check_nonplanar_projected(child, layer);
}

void LayerRegion::project_nonplanar_surfaces()
{
    if (m_nonplanar_surfaces.empty()) return;
    const NonplanarProjectionIndex index(m_nonplanar_surfaces);
    if (! m_nonplanar_perimeters_projected)
        for (ExtrusionEntity *entity : perimeters.entities)
            if (auto *collection = dynamic_cast<ExtrusionEntityCollection*>(entity))
                project_nonplanar_extrusion(collection, index);
    m_nonplanar_perimeters_projected = true;
    for (ExtrusionEntity *entity : fills.entities)
        if (auto *collection = dynamic_cast<ExtrusionEntityCollection*>(entity))
            project_nonplanar_extrusion(collection, index);
    check_nonplanar_projected(&perimeters, *this->layer());
    check_nonplanar_projected(&fills, *this->layer());
}

static bool nonplanar_triangle_height(const NonplanarFacet &facet, double x, double y,
                                      double margin, double &z, double &distance2)
{
    const auto &a = facet.vertex[0], &b = facet.vertex[1], &c = facet.vertex[2];
    const double bx = double(b.x) - a.x, by = double(b.y) - a.y;
    const double cx = double(c.x) - a.x, cy = double(c.y) - a.y;
    const double den = by*cx - bx*cy;
    if (std::abs(den) < 1e-12) return false;
    const double w1 = ((y-a.y)*cx - (x-a.x)*cy) / den;
    const double w2 = (by*(x-a.x) - bx*(y-a.y)) / den;
    const double w0 = 1.0 - w1 - w2;
    if (w0 >= -1e-7 && w1 >= -1e-7 && w2 >= -1e-7) {
        z = a.z + w1*(double(b.z)-a.z) + w2*(double(c.z)-a.z);
        distance2 = 0.0;
        return true;
    }
    distance2 = std::numeric_limits<double>::max();
    const auto *vertices = facet.vertex;
    for (int i = 0; i < 3; ++i) {
        const auto &p = vertices[i], &q = vertices[(i+1)%3];
        const double dx = double(q.x)-p.x, dy = double(q.y)-p.y;
        const double len2 = dx*dx + dy*dy;
        if (len2 < 1e-12) continue;
        const double t = std::max(0.0, std::min(1.0, ((x-p.x)*dx + (y-p.y)*dy)/len2));
        const double ex = x-p.x-t*dx, ey = y-p.y-t*dy;
        const double d2 = ex*ex + ey*ey;
        if (d2 < distance2) {
            distance2 = d2;
            z = p.z + t*(double(q.z)-p.z);
        }
    }
    return distance2 <= margin*margin;
}

std::vector<ExtrusionPath> LayerRegion::project_nonplanar_path(const ExtrusionPath &source, const NonplanarProjectionIndex &index)
{
    if (!source.nonplanar_source || source.polyline.points.size() < 2)
        return {source};

    if (source.source_surface_id < 0 || source.distance_to_top < 0)
        throw_nonplanar_internal(nonplanar_location(this->layer()->print_z, this->layer()->id()),
            "path " + std::to_string(source.source_path_id) + " without target surface or shell offset", this->layer()->object()->id().id);
    const double offset = source.distance_to_top;
    const double margin = std::max(0.003, double(source.width) * 0.5 + 0.003);
    auto sample = [&](double x, double y, int &owner, int &facet_id, const NonplanarFacet *&facet, double &height) {
        double best = std::numeric_limits<double>::max();
        owner = -1;
        facet_id = -1;
        facet = nullptr;
        for (const NonplanarProjectionIndex::Ref &candidate : index.query(x - margin, y - margin, x + margin, y + margin)) {
            if (m_nonplanar_surfaces[candidate.si].stable_id != source.source_surface_id)
                continue;
            double z = 0.0, d2 = 0.0;
            if (!nonplanar_triangle_height(*candidate.facet, x, y, margin, z, d2)) continue;
            if (d2 < best) {
                best = d2;
                owner = int(candidate.si);
                facet_id = candidate.facet_id;
                facet = candidate.facet;
                height = z - offset;
                if (d2 == 0.) break;
            }
        }
    };
    auto sample_facet = [&](double x, double y, const NonplanarFacet *facet, double &height) {
        if (facet == nullptr)
            return false;
        double z = 0.0, d2 = 0.0;
        if (!nonplanar_triangle_height(*facet, x, y, margin, z, d2)) return false;
        height = z - offset;
        return true;
    };
    auto point_at = [&](const Point3 &a, const Point3 &b, double t, int owner, const NonplanarFacet *facet, double inside_t) {
        double use_t = t;
        double x = unscale<double>(a.x()) + use_t*unscale<double>(b.x()-a.x());
        double y = unscale<double>(a.y()) + use_t*unscale<double>(b.y()-a.y());
        Point3 p(coord_t(scale_(x)), coord_t(scale_(y)), coord_t(0));
        if (owner >= 0) {
            double h = 0.0;
            if (!sample_facet(x, y, facet, h)) {
                double outside_t = t, inside = inside_t;
                for (int n = 0; n < 32; ++n) {
                    const double candidate = 0.5 * (outside_t + inside);
                    const double cx = unscale<double>(a.x()) + candidate*unscale<double>(b.x()-a.x());
                    const double cy = unscale<double>(a.y()) + candidate*unscale<double>(b.y()-a.y());
                    if (sample_facet(cx, cy, facet, h))
                        inside = candidate;
                    else
                        outside_t = candidate;
                }
                use_t = inside;
                x = unscale<double>(a.x()) + use_t*unscale<double>(b.x()-a.x());
                y = unscale<double>(a.y()) + use_t*unscale<double>(b.y()-a.y());
                p = Point3(coord_t(scale_(x)), coord_t(scale_(y)), coord_t(0));
                if (!sample_facet(x, y, facet, h))
                    throw_nonplanar_internal(nonplanar_location(this->layer()->print_z, this->layer()->id()),
                        "no surface height at the boundary point X" + std::to_string(x) + " Y" + std::to_string(y), this->layer()->object()->id().id);
            }
            p.z() = coord_t(scale_(h));
        }
        return p;
    };

    const double floor_z = this->layer()->object()->layers().front()->print_z;
    std::vector<ExtrusionPath> result;
    ExtrusionPath blank(source);
    blank.polyline.points = Points3();
    ExtrusionPath current(blank);
    int current_owner = -3;
    auto flush = [&]() {
        if (current.polyline.points.size() >= 2)
            result.emplace_back(std::move(current));
        current = blank;
    };

    for (size_t i = 1; i < source.polyline.points.size(); ++i) {
        const Point3 &a = source.polyline.points[i-1], &b = source.polyline.points[i];
        if (a == b) continue;
        const double ax = unscale<double>(a.x()), ay = unscale<double>(a.y());
        const double dx = unscale<double>(b.x()-a.x()), dy = unscale<double>(b.y()-a.y());
        const auto nearby = index.query(std::min(ax, ax + dx) - margin, std::min(ay, ay + dy) - margin,
                                        std::max(ax, ax + dx) + margin, std::max(ay, ay + dy) + margin);
        std::vector<double> ts{0.0, 1.0};
        for (const NonplanarProjectionIndex::Ref &ref : nearby) {
                const NonplanarFacet &f = *ref.facet;
                for (int e = 0; e < 3; ++e) {
                    const auto &v = f.vertex[e], &w = f.vertex[(e+1)%3];
                    const double ex = w.x-v.x, ey = w.y-v.y;
                    const double den = dx*ey-dy*ex;
                    if (std::abs(den) < 1e-12) continue;
                    const double cx = v.x-ax, cy = v.y-ay;
                    const double t = (cx*ey-cy*ex)/den;
                    const double u = (cx*dy-cy*dx)/den;
                    if (t > 1e-8 && t < 1.0-1e-8 && u >= -1e-8 && u <= 1.0+1e-8)
                        ts.push_back(t);
                }
        }
        std::sort(ts.begin(), ts.end());
        ts.erase(std::unique(ts.begin(), ts.end(), [](double u, double v) { return std::abs(u-v) < 1e-7; }), ts.end());
        for (size_t j = 1; j < ts.size(); ++j) {
            const double mid = 0.5*(ts[j-1]+ts[j]);
            int owner = -1, facet_id = -1;
            const NonplanarFacet *facet = nullptr;
            double height = 0.0;
            sample(ax+mid*dx, ay+mid*dy, owner, facet_id, facet, height);
            if (owner < 0)
                throw_nonplanar_internal(nonplanar_location(this->layer()->print_z, this->layer()->id()),
                    "no facet of surface " + std::to_string(source.source_surface_id) + " under X" +
                    std::to_string(ax+mid*dx) + " Y" + std::to_string(ay+mid*dy) +
                    ", shell offset " + std::to_string(source.distance_to_top), this->layer()->object()->id().id);
            Point3 p0 = point_at(a, b, ts[j-1], owner, facet, mid);
            Point3 p1 = point_at(a, b, ts[j], owner, facet, mid);
            if (p0 == p1) continue;
            if (owner >= 0 && std::min(p0.z(), p1.z()) <= scale_(floor_z)) {
                flush();
                current_owner = -3;
                continue;
            }
            if (owner >= 0) {
                const double xy = std::hypot(unscale<double>(p1.x() - p0.x()),
                                             unscale<double>(p1.y() - p0.y()));
                const double dz = std::abs(unscale<double>(p1.z() - p0.z()));
                const double steepest = std::max(12., this->layer()->object()->config().nonplanar_curved_max_angle.value);
                if (dz > std::tan(steepest * M_PI / 180.0) * xy + 0.03)
                    throw_nonplanar_too_steep(unscale<double>(std::max(p0.z(), p1.z())), this->layer()->id(),
                        this->layer()->object()->config().nonplanar_curved_max_angle.value, this->layer()->object()->id().id,
                        "projected segment X" + std::to_string(unscale<double>(p0.x())) + " Y" + std::to_string(unscale<double>(p0.y())) +
                        " -> X" + std::to_string(unscale<double>(p1.x())) + " Y" + std::to_string(unscale<double>(p1.y())) +
                        ", XY " + std::to_string(xy) + " mm, Z " + std::to_string(dz) + " mm, surface " + std::to_string(owner));
            }
            if (owner == current_owner && !current.polyline.points.empty() &&
                current.polyline.points.back().to_point() == p0.to_point() &&
                std::abs(current.polyline.points.back().z() - p0.z()) <= scale_(0.003))
                p0 = current.polyline.points.back();
            if (owner != current_owner || (!current.polyline.points.empty() &&
                current.polyline.points.back() != p0)) {
                flush();
                current_owner = owner;
                current.source_surface_id = owner >= 0 ? m_nonplanar_surfaces[size_t(owner)].stable_id : -1;
                current.nonplanar_source = owner >= 0;
                current.distance_to_top = owner >= 0 ? source.distance_to_top : -1.f;
            }
            if (current.polyline.points.empty()) current.polyline.points.push_back(p0);
            if (current.polyline.points.back() != p1)
                current.polyline.points.push_back(p1);
        }
    }
    flush();
    return result;
}


}
 
