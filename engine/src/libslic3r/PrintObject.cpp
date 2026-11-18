#include "Nonplanar/CurvedTopsSurface.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "Nonplanar/CurvedTopsContinuous.hpp"
#include <cfloat>
#include "ExPolygon.hpp"
#include "Config.hpp"
#include "Exception.hpp"
#include "Line.hpp"
#include "Flow.hpp"
#include "Model.hpp"
#include "ObjectID.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "Print.hpp"
#include "BeltTransform.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ConnectedBodies.hpp"
#include "Geometry.hpp"
#include "I18N.hpp"
#include "Layer.hpp"
#include "MeshBoolean.hpp"
#include "Nonplanar/HeadEnvelope.hpp"
#include "Nonplanar/RegionMask.hpp"
#include "Nonplanar/RegionDetect.hpp"
#include "Nonplanar/NativePrint.hpp"
#include "Nonplanar/AutoMode.hpp"
#include "GCode/ClearanceRepair.hpp"
#include "Nonplanar/CurvedTopsAccess.hpp"
#include "Nonplanar/CurvedTopsSchedule.hpp"
#include "Nonplanar/OneShotField.hpp"
#include "Nonplanar/NpField.hpp"
#include "Nonplanar/ConformalBottom.hpp"
#include "Nonplanar/ConformalIroning.hpp"
#include "Nonplanar/AtomToolpath.hpp"
#include "Nonplanar/ConformalShells.hpp"
#include "Nonplanar/PatchTools.hpp"
#include "Nonplanar/PlanarClip.hpp"
#include "Nonplanar/Repair.hpp"
#include "Nonplanar/CurvedTopsScaffold.hpp"
#include "Nonplanar/CurvedTopsBoundary.hpp"
#include "Nonplanar/Multiplanar.hpp"
#include "Nonplanar/OrientationField.hpp"
#include "Nonplanar/RotaryRouter.hpp"
#include "Nonplanar/ConformalSupport.hpp"
#include "Nonplanar/MultiplanarConfirm.hpp"
#include "Nonplanar/Rests.hpp"
#include "Nonplanar/ConformalVolume.hpp"
#include "Nonplanar/SubLayerWalls.hpp"
#include "Nonplanar/TopTexture.hpp"
#include "Nonplanar/CurvedBridge.hpp"
#include "Nonplanar/CurvedOverhang.hpp"
#include "Nonplanar/DeformSlice.hpp"
#include "Nonplanar/PatchTools.hpp"
#include "Nonplanar/ShellBase.hpp"
#include "Nonplanar/SpiralTop.hpp"
#include "MutablePolygon.hpp"
#include "PrintBase.hpp"
#include "PrintConfig.hpp"
#include "SLA/IndexedMesh.hpp"
#include "Support/SupportMaterial.hpp"
#include "Support/SupportCommon.hpp"
#include "Support/SupportSpotsGenerator.hpp"
#include "Support/TreeSupport.hpp"
#include "Surface.hpp"
#include "Slicing.hpp"
#include "Tesselate.hpp"
#include "TriangleMeshSlicer.hpp"
#include "Utils.hpp"
#include "Fill/FillAdaptive.hpp"
#include "Fill/Fill.hpp"
#include "Fill/FillLightning.hpp"
#include "Fill/FillTpmsAdaptive.hpp"
#include "format.hpp"
#include "AABBTreeIndirect.hpp"
#include "AABBTreeLines.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <Shiny/ShinyMacros.h>
#include <boost/date_time/posix_time/ptime.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <float.h>
#include <functional>
#include <ios>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <limits>
#include <map>
#include <math.h>
#include <mutex>
#include <set>
#include <optional>
#include <ratio>
#include <string>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/concurrent_vector.h>
#include <oneapi/tbb/parallel_for.h>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>

#include <boost/log/trivial.hpp>
#include <Eigen/Core>

#include <tbb/parallel_for.h>
#include <tbb/spin_mutex.h>
#include <tbb/concurrent_unordered_set.h>

#include <Shiny/Shiny.h>
#include <vector>
#include <tbb/concurrent_unordered_map.h>
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Fill/FillBase.hpp"
#include "Fill/Lightning/Generator.hpp"
#include "SurfaceCollection.hpp"
#include "TriangleMesh.hpp"
#include "BeltBrim.hpp"
#include "libslic3r/WaveOverhangs/WaveOverhangs.hpp"

namespace Slic3r { enum class EnforcerBlockerType : int8_t; }

using namespace std::literals;

#define L(s) Slic3r::I18N::translate(s)

#ifdef PRINT_OBJECT_TIMING
    #define PRINT_OBJECT_TIME_LIMIT_DEFAULT 50
    #include <boost/current_function.hpp>
    #include "Timer.hpp"
    #define PRINT_OBJECT_TIME_LIMIT_SECONDS(limit) Timing::TimeLimitAlarm time_limit_alarm(uint64_t(limit) * 1000000000l, BOOST_CURRENT_FUNCTION)
    #define PRINT_OBJECT_TIME_LIMIT_MILLIS(limit) Timing::TimeLimitAlarm time_limit_alarm(uint64_t(limit) * 1000000l, BOOST_CURRENT_FUNCTION)
#else
    #define PRINT_OBJECT_TIME_LIMIT_SECONDS(limit) do {} while(false)
    #define PRINT_OBJECT_TIME_LIMIT_MILLIS(limit) do {} while(false)
#endif

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
#define SLIC3R_DEBUG
#endif

#ifdef SLIC3R_DEBUG
    #undef NDEBUG
    #define DEBUG
    #define _DEBUG
    #include "SVG.hpp"
    #undef assert
    #include <cassert>
#endif

namespace Slic3r {

PrintObject::PrintObject(Print* print, ModelObject* model_object, const Transform3d& trafo, PrintInstances&& instances) :
    PrintObjectBaseWithState(print, model_object),
    m_trafo(trafo),
    m_tree_support_preview_cache(nullptr),
    m_id(model_object->id().id)
{
    m_nonplanar_region_cache = std::make_shared<Nonplanar::RegionMaskCache>();
    BoundingBoxf3  bbox        = model_object->raw_bounding_box();
    Vec3d 		   bbox_center = bbox.center();

	double z_diff = Geometry::rotation_diff_z(model_object->instances.front()->get_rotation(), instances.front().model_instance->get_rotation());
	if (std::abs(z_diff) > EPSILON) {
		auto z_rot  = Eigen::AngleAxisd(z_diff, Vec3d::UnitZ());
		bbox 		= bbox.transformed(Transform3d(z_rot));
		bbox_center = (z_rot * bbox_center).eval();
	}

    m_center_offset = Point::new_scale(bbox_center.x(), bbox_center.y());
    m_size = (bbox.size() * (1. / SCALING_FACTOR)).cast<coord_t>();
    m_max_z = scaled(model_object->instance_bounding_box(0).max(2));

    this->set_instances(std::move(instances));
}

PrintObject::~PrintObject()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, m_shared_object %2%")%this%m_shared_object;
    if (m_shared_regions && -- m_shared_regions->m_ref_cnt == 0) delete m_shared_regions;
    clear_layers();
    clear_support_layers();
    Nonplanar::forget_grounded_route(*this);
}

PrintBase::ApplyStatus PrintObject::set_instances(PrintInstances &&instances)
{
    for (PrintInstance &i : instances)
    	i.shift += m_center_offset;
    PrintBase::ApplyStatus status = PrintBase::APPLY_STATUS_UNCHANGED;
    bool equal_length = instances.size() == m_instances.size();
    bool equal = equal_length && std::equal(instances.begin(), instances.end(), m_instances.begin(),
    	[](const PrintInstance& lhs, const PrintInstance& rhs) { return lhs.model_instance == rhs.model_instance && lhs.shift == rhs.shift; });
    if (! equal) {
        status = PrintBase::APPLY_STATUS_CHANGED;
        if (m_print->invalidate_steps({ psSkirtBrim, psGCodeExport }) ||
            (! equal_length && m_print->invalidate_step(psWipeTower)))
            status = PrintBase::APPLY_STATUS_INVALIDATED;
        m_instances = std::move(instances);
	    for (PrintInstance &i : m_instances)
	    	i.print_object = this;
    }
    return status;
}

std::vector<std::reference_wrapper<const PrintRegion>> PrintObject::all_regions() const
{
    std::vector<std::reference_wrapper<const PrintRegion>> out;
    if(!m_shared_regions)
        return out;
        
    out.reserve(m_shared_regions->all_regions.size());
    for (const std::unique_ptr<Slic3r::PrintRegion> &region : m_shared_regions->all_regions)
        out.emplace_back(*region.get());
    return out;
}

Polygons create_polyholes(const Point center, const coord_t radius, const coord_t nozzle_diameter, bool multiple, int max_edges)
{
    size_t nb_edges = (int)std::min(max_edges, std::max(3, (int)std::round(4.0 * unscaled(radius) * 0.4 / unscaled(nozzle_diameter))));
    int nb_polyhole = 1;
    float rotation = 0;
    if (multiple) {
        nb_polyhole = 5;
        rotation = 2 * float(PI) / (nb_edges * nb_polyhole);
    }
    Polygons list;
    for (int i_poly = 0; i_poly < nb_polyhole; i_poly++)
        list.emplace_back();
    for (int i_poly = 0; i_poly < nb_polyhole; i_poly++) {
        Polygon& pts = (((i_poly % 2) == 0) ? list[i_poly / 2] : list[(nb_polyhole + 1) / 2 + i_poly / 2]);
        const float new_radius = radius / float(std::cos(PI / nb_edges));
        for (size_t i_edge = 0; i_edge < nb_edges; ++i_edge) {
            float angle = rotation * i_poly + (float(PI) * 2 * (float)i_edge) / nb_edges;
            pts.points.emplace_back(center.x() + new_radius * cos(angle), center.y() + new_radius * sin(angle));
        }
        pts.make_clockwise();
    }
    return list;
}

void PrintObject::_transform_hole_to_polyholes()
{
    std::vector<std::vector<std::pair<std::tuple<Point, float, int, coord_t, bool, int>, Polygon*>>> layerid2center;
    for (size_t i = 0; i < this->m_layers.size(); i++) layerid2center.emplace_back();
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, m_layers.size()),
        [this, &layerid2center](const tbb::blocked_range<size_t>& range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            m_print->throw_if_canceled();
            Layer* layer = m_layers[layer_idx];
            for (size_t region_idx = 0; region_idx < layer->m_regions.size(); ++region_idx)
            {
                if (layer->m_regions[region_idx]->region().config().hole_to_polyhole) {
                    for (Surface& surf : layer->m_regions[region_idx]->slices.surfaces) {
                        for (Polygon& hole : surf.expolygon.holes) {
                            if (hole.convex_points(PI).empty() && hole.points.size() > 8) {
                                Point center = hole.centroid();
                                double diameter_min = std::numeric_limits<float>::max(), diameter_max = 0;
                                double diameter_sum = 0;
                                for (int i = 0; i < hole.points.size(); ++i) {
                                    double dist = hole.points[i].distance_to(center);
                                    diameter_min = std::min(diameter_min, dist);
                                    diameter_max = std::max(diameter_max, dist);
                                    diameter_sum += dist;
                                }
                                double diameter_line_min = std::numeric_limits<float>::max(), diameter_line_max = 0;
                                Lines hole_lines = hole.lines();
                                for (Line l : hole_lines) {
                                    Point midline = (l.a + l.b) / 2;
                                    double dist = center.distance_to(midline);
                                    diameter_line_min = std::min(diameter_line_min, dist);
                                    diameter_line_max = std::max(diameter_line_max, dist);
                                }


                                coord_t max_variation = std::max(SCALED_EPSILON, scale_(this->m_layers[layer_idx]->m_regions[region_idx]->region().config().hole_to_polyhole_threshold.get_abs_value(unscaled(diameter_sum / hole.points.size()))));
                                bool twist = this->m_layers[layer_idx]->m_regions[region_idx]->region().config().hole_to_polyhole_twisted.value;
                                int max_edges = this->m_layers[layer_idx]->m_regions[region_idx]->region().config().hole_to_polyhole_max_edges.value;
                                if (diameter_max - diameter_min < max_variation * 2 && diameter_line_max - diameter_line_min < max_variation * 2) {
                                    layerid2center[layer_idx].emplace_back(
                                        std::tuple<Point, float, int, coord_t, bool, int>{center, diameter_max, layer->m_regions[region_idx]->region().config().outer_wall_filament_id.value, max_variation, twist, max_edges}, & hole);
                                }
                            }
                        }
                    }
                }
            }
        }
    });
    std::map<std::tuple<Point, float, int, coord_t, bool, int>, std::vector<std::pair<Polygon*, int>>> id2layerz2hole;

    const size_t min_nb_layers = 2;
    for (size_t layer_idx = 0; layer_idx < this->m_layers.size(); ++layer_idx) {
        for (size_t hole_idx = 0; hole_idx < layerid2center[layer_idx].size(); ++hole_idx) {
            std::tuple<Point, float, int, coord_t, bool, int>& id = layerid2center[layer_idx][hole_idx].first;
            float max_z = layers()[layer_idx]->print_z;
            std::vector<std::pair<Polygon*, int>> holes;
            holes.emplace_back(layerid2center[layer_idx][hole_idx].second, layer_idx);
            for (size_t search_layer_idx = layer_idx + 1; search_layer_idx < this->m_layers.size(); ++search_layer_idx) {
                if (layers()[search_layer_idx]->print_z - layers()[search_layer_idx]->height - max_z > EPSILON) break;
                for (size_t search_hole_idx = 0; search_hole_idx < layerid2center[search_layer_idx].size(); ++search_hole_idx) {
                    std::tuple<Point, float, int, coord_t, bool, int>& search_id = layerid2center[search_layer_idx][search_hole_idx].first;
                    if (std::get<2>(id) == std::get<2>(search_id)
                        && std::get<0>(id).distance_to(std::get<0>(search_id)) < std::get<3>(id)
                        && std::abs(std::get<1>(id) - std::get<1>(search_id)) < std::get<3>(id)
                        ) {
                        max_z = layers()[search_layer_idx]->print_z;
                        holes.emplace_back(layerid2center[search_layer_idx][search_hole_idx].second, search_layer_idx);
                        layerid2center[search_layer_idx].erase(layerid2center[search_layer_idx].begin() + search_hole_idx);
                        search_hole_idx--;
                        break;
                    }
                }
            }
            if (holes.size() >= min_nb_layers || (holes.size() == 1 && holes[0].second == 0)) {
                id2layerz2hole.emplace(std::move(id), std::move(holes));
            }
        }
    }
    for (auto entry : id2layerz2hole) {
        Polygons polyholes = create_polyholes(std::get<0>(entry.first), std::get<1>(entry.first), scale_(print()->config().nozzle_diameter.get_at(std::get<2>(entry.first) - 1)), std::get<4>(entry.first), std::get<5>(entry.first));
        for (auto& poly_to_replace : entry.second) {
            Polygon polyhole = polyholes[poly_to_replace.second % polyholes.size()];
            for (ExPolygon& explo_slice : m_layers[poly_to_replace.second]->lslices) {
                for (Polygon& poly_slice : explo_slice.holes) {
                    if (poly_slice.points == poly_to_replace.first->points) {
                        poly_slice.points = polyhole.points;
                    }
                }
            }
            poly_to_replace.first->points = polyhole.points;
        }
    }
}

std::vector<std::set<int>> PrintObject::detect_extruder_geometric_unprintables() const
{
    int extruder_size = m_print->config().nozzle_diameter.size();
    if(extruder_size == 1)
        return std::vector<std::set<int>>(1, std::set<int>());

    std::vector<std::set<int>> geometric_unprintables(extruder_size);

    std::vector<double> printable_height_per_extruder = m_print->config().extruder_printable_height.values;
    assert(printable_height_per_extruder.size() == extruder_size);

    for (size_t extruder_id = 0; extruder_id < printable_height_per_extruder.size(); ++extruder_id) {
        double printable_height = printable_height_per_extruder[extruder_id];
        for (size_t layer_idx = 0; layer_idx < m_layers.size(); ++layer_idx) {
            auto layer = m_layers[layer_idx];
            if (layer->print_z <= printable_height)
                continue;
            for (auto layerm : layer->regions()) {
                auto region = layerm->region();
                int outer_wall_filament_id = region.config().outer_wall_filament_id;
                int inner_wall_filament_id = region.config().inner_wall_filament_id;
                int internal_solid_filament_id = region.config().internal_solid_filament_id;
                int top_surface_filament_id = region.config().top_surface_filament_id;
                int bottom_surface_filament_id = region.config().bottom_surface_filament_id;
                int sparse_infill_filament_id = region.config().sparse_infill_filament_id;

                if (!layerm->fills.entities.empty()) {
                    if (internal_solid_filament_id > 0)
                        geometric_unprintables[extruder_id].insert(internal_solid_filament_id - 1);
                    if (top_surface_filament_id > 0)
                        geometric_unprintables[extruder_id].insert(top_surface_filament_id - 1);
                    if (bottom_surface_filament_id > 0)
                        geometric_unprintables[extruder_id].insert(bottom_surface_filament_id - 1);
                    if (sparse_infill_filament_id > 0)
                        geometric_unprintables[extruder_id].insert(sparse_infill_filament_id - 1);
                }
                if (!layerm->perimeters.entities.empty()) {
                    if (outer_wall_filament_id > 0)
                        geometric_unprintables[extruder_id].insert(outer_wall_filament_id - 1);
                    if (inner_wall_filament_id > 0)
                        geometric_unprintables[extruder_id].insert(inner_wall_filament_id - 1);
                }
            }
        }
    }

    std::vector<tbb::concurrent_unordered_set<int>> tbb_geometric_unprintables(extruder_size);

    std::vector<Polygons> unprintable_area_in_obj_coord =  m_print->get_extruder_unprintable_polygons();
    std::vector<BoundingBox> unprintable_area_bbox;

    for (auto& polys : unprintable_area_in_obj_coord) {
        for (auto& poly : polys) {
            poly.translate(-m_instances.front().shift_without_plate_offset());
        }
        unprintable_area_bbox.emplace_back(get_extents(polys));
    }

    tbb::parallel_for(tbb::blocked_range<int>(0, m_layers.size()),
        [this, &tbb_geometric_unprintables, &unprintable_area_in_obj_coord, &unprintable_area_bbox](const tbb::blocked_range<int>& range) {
            for (int j = range.begin(); j < range.end(); ++j) {
                auto layer = m_layers[j];
                for (auto layerm : layer->regions()) {
                    const auto& region = layerm->region();
                    int outer_wall_filament_id = region.config().outer_wall_filament_id;
                    int inner_wall_filament_id = region.config().inner_wall_filament_id;
                    int internal_solid_filament_id = region.config().internal_solid_filament_id;
                    int top_surface_filament_id = region.config().top_surface_filament_id;
                    int bottom_surface_filament_id = region.config().bottom_surface_filament_id;
                    int sparse_infill_filament_id = region.config().sparse_infill_filament_id;
                    std::optional<ExPolygons> fill_expolys;
                    BoundingBox fill_bbox;
                    std::optional<ExPolygons> wall_expolys;
                    BoundingBox wall_bbox;

                    for (size_t idx = 0; idx < unprintable_area_in_obj_coord.size(); ++idx) {
                        bool do_infill_filament_detect = (internal_solid_filament_id > 0 && tbb_geometric_unprintables[idx].count(internal_solid_filament_id - 1) == 0) ||
                            (top_surface_filament_id > 0 && tbb_geometric_unprintables[idx].count(top_surface_filament_id - 1) == 0) ||
                            (bottom_surface_filament_id > 0 && tbb_geometric_unprintables[idx].count(bottom_surface_filament_id - 1) == 0) ||
                            (sparse_infill_filament_id > 0 && tbb_geometric_unprintables[idx].count(sparse_infill_filament_id-1) == 0);

                        bool infill_unprintable = !layerm->fills.entities.empty() &&
                            ((internal_solid_filament_id > 0 && tbb_geometric_unprintables[idx].count(internal_solid_filament_id - 1) > 0) ||
                                (top_surface_filament_id > 0 && tbb_geometric_unprintables[idx].count(top_surface_filament_id - 1) > 0) ||
                                (bottom_surface_filament_id > 0 && tbb_geometric_unprintables[idx].count(bottom_surface_filament_id - 1) > 0) ||
                                (sparse_infill_filament_id > 0 && tbb_geometric_unprintables[idx].count(sparse_infill_filament_id - 1) > 0));

                        if (!layerm->fills.entities.empty() && do_infill_filament_detect) {
                            if (!fill_expolys) {
                                fill_expolys = layerm->fill_expolygons;
                                fill_bbox = get_extents(*fill_expolys);
                            }
                            if (fill_bbox.overlap(unprintable_area_bbox[idx]) &&
                                !intersection(*fill_expolys, unprintable_area_in_obj_coord[idx]).empty()) {
                                if (internal_solid_filament_id > 0)
                                    tbb_geometric_unprintables[idx].insert(internal_solid_filament_id - 1);
                                if (top_surface_filament_id > 0)
                                    tbb_geometric_unprintables[idx].insert(top_surface_filament_id - 1);
                                if (bottom_surface_filament_id > 0)
                                    tbb_geometric_unprintables[idx].insert(bottom_surface_filament_id - 1);
                                if (sparse_infill_filament_id > 0)
                                    tbb_geometric_unprintables[idx].insert(sparse_infill_filament_id - 1);
                                infill_unprintable = true;
                            }
                        }

                        bool do_outer_wall_filament_detect = outer_wall_filament_id > 0 && tbb_geometric_unprintables[idx].count(outer_wall_filament_id - 1) == 0;
                        bool do_inner_wall_filament_detect = inner_wall_filament_id > 0 && tbb_geometric_unprintables[idx].count(inner_wall_filament_id - 1) == 0;
                        if (!layerm->perimeters.entities.empty() && (do_outer_wall_filament_detect || do_inner_wall_filament_detect)) {
                            if (infill_unprintable) {
                                if (outer_wall_filament_id > 0)
                                    tbb_geometric_unprintables[idx].insert(outer_wall_filament_id - 1);
                                if (inner_wall_filament_id > 0)
                                    tbb_geometric_unprintables[idx].insert(inner_wall_filament_id - 1);
                                continue;
                            }

                            if (!wall_expolys) {
                                if (!fill_expolys) {
                                    fill_expolys = layerm->fill_expolygons;
                                    fill_bbox = get_extents(*fill_expolys);
                                }
                                wall_expolys = diff_ex(layerm->raw_slices, *fill_expolys);
                                wall_bbox = get_extents(*wall_expolys);
                            }

                            if (wall_bbox.overlap(unprintable_area_bbox[idx]) &&
                                !intersection(*wall_expolys, unprintable_area_in_obj_coord[idx]).empty()) {
                                if (outer_wall_filament_id > 0)
                                    tbb_geometric_unprintables[idx].insert(outer_wall_filament_id - 1);
                                if (inner_wall_filament_id > 0)
                                    tbb_geometric_unprintables[idx].insert(inner_wall_filament_id - 1);
                            }
                        }
                    }
                }
            }
        });

    for (size_t idx = 0; idx < extruder_size; ++idx) {
        geometric_unprintables[idx].insert(tbb_geometric_unprintables[idx].begin(), tbb_geometric_unprintables[idx].end());
    }

    return geometric_unprintables;
}

void PrintObject::make_perimeters()
{
    this->slice();

    if (! this->set_started(posPerimeters))
        return;

    m_print->set_status(15, L("Generating walls"));
    BOOST_LOG_TRIVIAL(info) << "Generating walls..." << log_memory_info();

    if (m_typed_slices) {
        for (Layer *layer : m_layers) {
            layer->restore_untyped_slices();
            m_print->throw_if_canceled();
        }
        m_typed_slices = false;
    }

    for (Layer *layer : m_layers) {
        layer->wave_overhang_floor_polygons.clear();
        layer->wave_overhang_covered_polygons.clear();
    }

    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        const PrintRegion &region = this->printing_region(region_id);
            continue;

        BOOST_LOG_TRIVIAL(debug) << "Generating extra perimeters for region " << region_id << " in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size() - 1),
            [this, &region, region_id](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    LayerRegion &layerm                     = *m_layers[layer_idx]->get_region(region_id);
                    const LayerRegion &upper_layerm         = *m_layers[layer_idx+1]->get_region(region_id);
                    const Polygons upper_layerm_polygons    = to_polygons(upper_layerm.slices.surfaces);
                    const double total_loop_length      = total_length(upper_layerm_polygons);
                    const coord_t perimeter_spacing     = layerm.flow(frPerimeter).scaled_spacing();
                    const Flow ext_perimeter_flow       = layerm.flow(frExternalPerimeter);
                    const coord_t ext_perimeter_width   = ext_perimeter_flow.scaled_width();
                    const coord_t ext_perimeter_spacing = ext_perimeter_flow.scaled_spacing();

                    for (Surface &slice : layerm.slices.surfaces) {
                        for (;;) {
                            const coord_t perimeters_thickness = ext_perimeter_width/2 + ext_perimeter_spacing/2
                                + (region.config().wall_loops-1 + slice.extra_perimeters) * perimeter_spacing;
                            const coord_t critical_area_depth = coord_t(perimeter_spacing * 1.5);
                            const Polygons critical_area = diff(
                                offset(slice.expolygon, float(- perimeters_thickness)),
                                offset(slice.expolygon, float(- perimeters_thickness - critical_area_depth))
                            );
                            const Polylines intersection = intersection_pl(to_polylines(upper_layerm_polygons), critical_area);
                            if (total_length(intersection) <=  total_loop_length*0.3)
                                break;
                            ++ slice.extra_perimeters;
                        }
                        #ifdef DEBUG
                            if (slice.extra_perimeters > 0)
                                printf("  adding %d more perimeter(s) at layer %zu\n", slice.extra_perimeters, layer_idx);
                        #endif
                    }
                }
            });
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Generating extra perimeters for region " << region_id << " in parallel - end";
    }

    BOOST_LOG_TRIVIAL(debug) << "Generating perimeters in parallel - start";
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, m_layers.size()),
        [this](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                m_print->throw_if_canceled();
                m_layers[layer_idx]->make_perimeters();
            }
        }
    );
    m_print->throw_if_canceled();
    BOOST_LOG_TRIVIAL(debug) << "Generating perimeters in parallel - end";

    this->set_done(posPerimeters);
}

void PrintObject::prepare_infill()
{
    if (! this->set_started(posPrepareInfill))
        return;
    m_print->set_status(25, L("Generating infill regions"));
    if (m_typed_slices && !curved_tops_mode(m_config.nonplanar_mode.value)) {
        for (Layer* layer : m_layers) {
            layer->restore_untyped_slices_no_extra_perimeters();
            m_print->throw_if_canceled();
        }
    }

    this->detect_surfaces_type();
    m_print->throw_if_canceled();

    BOOST_LOG_TRIVIAL(info) << "Preparing fill surfaces..." << log_memory_info();
    for (auto *layer : m_layers)
        for (auto *region : layer->m_regions) {
            region->prepare_fill_surfaces();
            m_print->throw_if_canceled();
        }


    this->discover_vertical_shells();
    m_print->throw_if_canceled();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (const Layer *layer : m_layers) {
            LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("3_discover_vertical_shells-final");
            layerm->export_region_fill_surfaces_to_svg_debug("3_discover_vertical_shells-final");
        }
    }
#endif
    this->discover_horizontal_shells();
    m_print->throw_if_canceled();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (const Layer *layer : m_layers) {
            LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("5_discover_horizontal_shells-final");
            layerm->export_region_fill_surfaces_to_svg_debug("5_discover_horizontal_shells-final");
        }
    }
#endif
    this->process_external_surfaces();
    m_print->throw_if_canceled();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (const Layer *layer : m_layers) {
            LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("7_process_external_surfaces-final");
            layerm->export_region_fill_surfaces_to_svg_debug("7_process_external_surfaces-final");
        }
    }
#endif
    this->clip_fill_surfaces();
    m_print->throw_if_canceled();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (const Layer *layer : m_layers) {
            LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("8_clip_surfaces-final");
            layerm->export_region_fill_surfaces_to_svg_debug("8_clip_surfaces-final");
        }
    }
#endif
    bool needs_separated_components = false;
    for (size_t i = 0; i < this->num_printing_regions(); ++ i) {
        const PrintRegionConfig &rc = this->printing_region(i).config();
        if (rc.separated_infills || rc.center_of_surface_pattern == CenterOfSurfacePattern::Each_Model ||
            (rc.sparse_infill_density > 0 && is_octree_infill_pattern(rc.sparse_infill_pattern))) {
            needs_separated_components = true;
            break;
        }
    }
    if (needs_separated_components) {
        int                parts      = 0;
        const ModelVolume *first_part = nullptr;
        for (const ModelVolume *v : this->model_object()->volumes)
            if (v->is_model_part()) { ++ parts; first_part = v; }
        if (parts <= 1 && ! (first_part != nullptr && first_part->is_splittable()))
            needs_separated_components = false;
    }
    m_separated_body_bboxes.clear();
    for (Layer *layer : m_layers)
        layer->lslices_separated_component_ids.clear();
    if (needs_separated_components) {
        std::vector<const ExPolygons *> islands;
        islands.reserve(m_layers.size());
        for (const Layer *layer : m_layers)
            islands.emplace_back(&layer->lslices);
        size_t                           bodies = 0;
        std::vector<std::vector<size_t>> ids    = connected_bodies(islands, bodies, [this]() { m_print->throw_if_canceled(); });
        m_separated_body_bboxes.assign(bodies, BoundingBox());
        for (size_t i = 0; i < m_layers.size(); ++ i) {
            Layer *layer = m_layers[i];
            for (size_t a = 0; a < layer->lslices.size(); ++ a)
                m_separated_body_bboxes[ids[i][a]].merge(layer->lslices_bboxes[a]);
            layer->lslices_separated_component_ids = std::move(ids[i]);
        }
    }

    this->bridge_over_infill();
    m_print->throw_if_canceled();

    this->apply_wave_overhang_floor_layer_authority();
    m_print->throw_if_canceled();

    this->apply_wave_overhang_bridge_suppression();
    m_print->throw_if_canceled();

    this->tag_wave_overhang_perimeters();
    m_print->throw_if_canceled();

    this->combine_infill();
    m_print->throw_if_canceled();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (const Layer *layer : m_layers) {
            LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("9_prepare_infill-final");
            layerm->export_region_fill_surfaces_to_svg_debug("9_prepare_infill-final");
        }
    }
    for (const Layer *layer : m_layers) {
        layer->export_region_slices_to_svg_debug("9_prepare_infill-final");
        layer->export_region_fill_surfaces_to_svg_debug("9_prepare_infill-final");
    }
#endif

    this->set_done(posPrepareInfill);
}

static void find_zaa_flat_tops(const PrintObject &object, const LayerPtrs &layers)
{
    bool mid = object.curved_fallback_zaa();
    for (size_t region_id = 0; region_id < object.num_printing_regions(); ++region_id) {
        const PrintRegionConfig &config = object.printing_region(region_id).config();
        mid |= config.zaa_enabled && config.zaa_mid_layer_slice;
    }
    for (Layer *layer : layers)
        layer->zaa_flat_tops.clear();
    if (!mid || layers.empty())
        return;
    double             ground = 0.;
    const TriangleMesh mesh   = object.object_frame_mesh(ground);
    const float        z0     = mesh.bounding_box().min.z();
    std::vector<Polygons> faces(layers.size());
    for (const stl_triangle_vertex_indices &f : mesh.its.indices) {
        const Vec3f &a = mesh.its.vertices[f[0]], &b = mesh.its.vertices[f[1]], &c = mesh.its.vertices[f[2]];
        const Vec3f  n = (b - a).cross(c - a);
        if (!(n.norm() > 0.f) || n.z() < 0.9999f * n.norm())
            continue;
        const double z  = (a.z() + b.z() + c.z()) / 3. - z0;
        const auto   it = std::upper_bound(layers.begin(), layers.end(), z, [](double z, const Layer *l) { return z < l->slice_z; });
        if (it == layers.begin())
            continue;
        faces[size_t(it - layers.begin()) - 1].emplace_back(Points{ Point::new_scale(a.x(), a.y()), Point::new_scale(b.x(), b.y()),
                                                                    Point::new_scale(c.x(), c.y()) });
    }
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t k = range.begin(); k < range.end(); ++k)
            if (!faces[k].empty())
                layers[k]->zaa_flat_tops = offset_ex(union_(faces[k]), float(scale_(0.05)));
    });
}

void PrintObject::infill()
{
    this->prepare_infill();

    if (this->set_started(posInfill)) {
        Nonplanar::prepare_vase_closure(*this);
        find_zaa_flat_tops(*this, m_layers);
        m_print->set_status(35, L("Generating infill toolpath"));

        BOOST_LOG_TRIVIAL(debug) << "Filling layers in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    m_layers[layer_idx]->make_fills(&m_adaptive_fill_octrees, this->m_lightning_generator.get());
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Filling layers in parallel - end";
        this->set_done(posInfill);
    }
}

void PrintObject::ironing()
{
    if (this->set_started(posIroning)) {
        BOOST_LOG_TRIVIAL(debug) << "Ironing in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    m_layers[layer_idx]->make_ironing();
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Ironing in parallel - end";
        this->set_done(posIroning);
    }
}

void PrintObject::nonplanar_shells()
{
    if (!this->set_started(posNonplanar))
        return;
    if (curved_tops_mode(m_config.nonplanar_mode.value))
        this->project_nonplanar_surfaces();
    const bool imported = !m_config.nonplanar_atom_toolpath.value.empty();
    bool       enabled  = false;
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id)
        enabled |= Nonplanar::top_shells_enabled(this->printing_region(region_id).config(), this->config()) ||
                   Nonplanar::bottom_shells_enabled(this->printing_region(region_id).config(), this->config()) ||
                   Nonplanar::full_conformal_thickness(this->printing_region(region_id).config(), *this) > 0. ||
                   Nonplanar::overhangs_enabled(this->printing_region(region_id).config(), this->config()) ||
                   Nonplanar::bridges_enabled(this->printing_region(region_id).config(), this->config());
    enabled &= !imported && !m_auto_z_contouring;
    auto record_apply_inputs = [this]() {
        m_conformal->layer_changes_filament = Nonplanar::layer_changes_filament(*this);
        m_conformal->event_cuts             = Nonplanar::event_cuts(*this);
        m_conformal->tool_changes           = Nonplanar::multi_as_single_tool_changes(*this);
    };
    if (!imported) {
        auto routed = std::make_unique<Nonplanar::ConformalResult>();
        if (Nonplanar::build_rotary_routes(*this, *routed, Nonplanar::RotaryRoute::Stage::Replace)) {
            m_conformal = std::move(routed);
            record_apply_inputs();
            m_print->throw_if_canceled();
            this->set_done(posNonplanar);
            return;
        }
    }
    if (m_native_field && m_config.nonplanar_atom_toolpath.value.empty()) {
        m_print->set_status(40, L("Slicing native curved layers"));
        for (Layer *layer : m_layers) {
            layer->conformal_patches_before.clear();
            layer->conformal_patches_after.clear();
        }
        m_conformal = std::make_unique<Nonplanar::ConformalResult>();
        Nonplanar::build_native_patch(*this, m_native_field, *m_conformal);
        record_apply_inputs();
        m_print->throw_if_canceled();
        this->set_done(posNonplanar);
        return;
    }
    if (m_field && m_config.nonplanar_atom_toolpath.value.empty()) {
        m_print->set_status(40, L("Slicing rotary curved layers"));
        for (Layer *layer : m_layers) {
            layer->conformal_patches_before.clear();
            layer->conformal_patches_after.clear();
        }
        m_conformal = std::make_unique<Nonplanar::ConformalResult>();
        Nonplanar::build_field_patch(*this, m_field, *m_conformal);
        record_apply_inputs();
        m_print->throw_if_canceled();
        this->set_done(posNonplanar);
        return;
    }
    if (m_multiplanar && m_config.nonplanar_atom_toolpath.value.empty()) {
        m_print->set_status(40, L("Slicing multiplanar chunks"));
        for (Layer *layer : m_layers) {
            layer->conformal_patches_before.clear();
            layer->conformal_patches_after.clear();
        }
        m_conformal = std::make_unique<Nonplanar::ConformalResult>();
        Nonplanar::build_multiplanar_chunks(*this, *m_multiplanar, *m_conformal);
        record_apply_inputs();
        m_print->throw_if_canceled();
        this->set_done(posNonplanar);
        return;
    }
    std::string deform_refused;
    if (Nonplanar::deform_slice_requested(*this)) {
        m_print->set_status(40, L("Slicing curved layers"));
        auto        conformal = std::make_unique<Nonplanar::ConformalResult>();
        std::string why;
        if (Nonplanar::build_deform_slice(*this, *conformal, &why)) {
            if (!why.empty())
                this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "smooth_morph_limited", warning_object(this->model_object()->name) }, this->model_object()->name + ": " + why + ".");
            m_conformal = std::move(conformal);
            record_apply_inputs();
            m_print->throw_if_canceled();
            this->set_done(posNonplanar);
            return;
        }
        if (m_config.nonplanar_mode.value == NonplanarMode::SmoothMorph)
            throw Slic3r::SlicingError(Slic3r::format(L("%1%: Smooth morph could not slice this part (%2%)."),
                                                    this->model_object()->name, why));
        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "curved_shells_only", warning_object(this->model_object()->name) },
                                      Slic3r::format(L("%1%: could not curve the whole part (%2%); curving the top shells only."),
                                             this->model_object()->name, why));
        deform_refused = this->config().nonplanar_deform.serialize() + ": " + why;
    }
    const bool open_vase = m_print->config().spiral_mode.value && !Nonplanar::vase_with_conformal_roof(*this);
    std::unique_ptr<Nonplanar::ConformalResult> lid;
    if (enabled && m_print->config().spiral_mode.value && !open_vase) {
        lid = std::make_unique<Nonplanar::ConformalResult>();
        Nonplanar::build_vase_lid(*this, *lid);
    }
    if (lid && !lid->patches.empty()) {
        m_conformal = std::move(lid);
    } else if (enabled && !open_vase) {
        m_print->set_status(40, L("Generating curved top shells"));
        m_conformal = std::make_unique<Nonplanar::ConformalResult>(Nonplanar::build_conformal_shells(*this));
        Nonplanar::trim_walls_above_shells(*this, *m_conformal);
        for (Layer *layer : m_layers)
            layer->conformal_patches_before.clear();
        if (Nonplanar::support_free(m_config)) {
            Nonplanar::build_curved_bridges(*this, *m_conformal);
            Nonplanar::build_curved_overhangs(*this, *m_conformal);
            Nonplanar::build_conformal_bottom(*this, *m_conformal);
        } else {
            Nonplanar::build_conformal_bottom(*this, *m_conformal);
            Nonplanar::build_curved_bridges(*this, *m_conformal);
            if (!(m_config.enable_support.value && Nonplanar::conformal_support_configured(*this)))
                Nonplanar::build_curved_overhangs(*this, *m_conformal);
        }
        Nonplanar::build_conformal_ironing(*this, *m_conformal);
        Nonplanar::build_conformal_volume(*this, *m_conformal);
        Nonplanar::measure_shell_bases(*this, *m_conformal);
        Nonplanar::group_hosts_by_filament(*this, *m_conformal);
        Nonplanar::host_before_events(*this, *m_conformal);
        if (Nonplanar::deform_slice_considered(*this)) {
            m_print->set_status(40, L("Slicing curved layers"));
            Nonplanar::choose_deform_slice(*this, *m_conformal);
        }
        m_conformal->mesh.reset();
    } else if (m_conformal) {
        for (Layer *layer : m_layers) {
            layer->conformal_patches_before.clear();
            layer->conformal_patches_after.clear();
            layer->conformal_covered = false;
        }
        m_conformal.reset();
    }
    if (imported) {
        m_conformal = std::make_unique<Nonplanar::ConformalResult>();
        m_print->set_status(40, L("Importing the Atomizer toolpath"));
        for (const std::string &warning : Nonplanar::apply_atom_toolpath(*this, *m_conformal))
            this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "atom_toolpath_note", warning_object(this->model_object()->name) }, warning);
    }
    if (!imported && Nonplanar::rotary_route_appends(*this)) {
        const bool fresh = !m_conformal;
        if (fresh)
            m_conformal = std::make_unique<Nonplanar::ConformalResult>();
        Nonplanar::build_rotary_routes(*this, *m_conformal, Nonplanar::RotaryRoute::Stage::Append);
        if (fresh && m_conformal->patches.empty())
            m_conformal.reset();
    }
    if (m_conformal) {
        m_conformal->layer_source_refused = deform_refused;
        record_apply_inputs();
    }
    m_print->throw_if_canceled();
    m_auto_planar_pass = m_config.nonplanar_mode.value == NonplanarMode::Auto && !imported && !m_auto_z_contouring &&
                         !m_print->config().spiral_mode.value && (!m_conformal || m_conformal->patches.empty());
    if (m_auto_planar_pass) {
        m_auto_planar_exact    = true;
        m_auto_floating_planar = true;
        m_nonplanar_repair.reset();
        this->note_auto_fallback(_u8L("no curved surface to print"));
    }
    this->set_done(posNonplanar);
}

TriangleMesh PrintObject::object_frame_mesh(double &ground_offset) const
{
    TriangleMesh mesh = m_model_object->raw_mesh();
    TriangleMesh negative;
    for (const ModelVolume *volume : m_model_object->volumes)
        if (volume->is_negative_volume()) {
            TriangleMesh vm = volume->mesh();
            vm.transform(volume->get_matrix());
            negative.merge(vm);
        }
    if (! negative.empty()) {
        try {
            MeshBoolean::cgal::minus(mesh, negative);
        } catch (const std::exception &e) {
            std::vector<TriangleMesh> parts;
            MeshBoolean::mcut::make_boolean(mesh, negative, parts, "A_NOT_B");
            TriangleMesh cut;
            for (const TriangleMesh &part : parts)
                cut.merge(part);
            BOOST_LOG_TRIVIAL(warning) << "Non-planar: negative volumes of " << m_model_object->name << " cut with mcut (" << e.what() << ")";
            if (! cut.empty())
                mesh = std::move(cut);
        }
    }
    const ModelInstance *inst = m_instances.front().model_instance;
    Geometry::Transformation trans = inst->get_transformation();
    trans.set_offset(Vec3d(-unscale<double>(m_center_offset.x()), -unscale<double>(m_center_offset.y()), 0));
    mesh.transform(trans.get_matrix());
    ground_offset = -m_model_object->min_z();
    return mesh;
}

bool PrintObject::has_conformal_support() const
{
    const Nonplanar::ConformalResult &conformal = this->conformal();
    return std::any_of(conformal.patches.begin(), conformal.patches.end(), [](const Nonplanar::ConformalPatch &patch) {
               return patch.from_support;
           });
}

void PrintObject::note_auto_fallback(const std::string &why)
{
    m_auto_plain_reason = why;
    constexpr const char *Note = " (fell back to planar control)";
    if (m_oneshot && m_oneshot->offer == Nonplanar::OneShotState::Offer::Auto && m_oneshot->decision.find(Note) == std::string::npos)
        m_oneshot->decision += Note;
}

double PrintObject::wall_band(double over) const
{
    const double top_edge = over > 0. && !m_layers.empty() ? m_layers.back()->print_z - 3. * m_config.layer_height.value
                                                             : std::numeric_limits<double>::max();
    double weighted = 0., wall = 0., below = 0.;
    for (const Layer *layer : m_layers) {
        double length = 0.;
        for (const LayerRegion *layerm : layer->regions()) {
            std::vector<const ExtrusionPath *> paths;
            Nonplanar::collect_paths(&layerm->perimeters, paths);
            for (const ExtrusionPath *path : paths)
                if (path->role() == erExternalPerimeter)
                    length += unscale<double>(path->polyline.length());
        }
        if (length <= 0.)
            continue;
        if (below > 0. && layer->height > 0. && layer->print_z <= top_edge) {
            weighted += std::max(0., std::abs(layer->height - below) / layer->height - over) * length;
            wall += length;
        }
        below = layer->height;
    }
    return wall > 0. ? weighted / wall : 0.;
}

Nonplanar::OneShotState& PrintObject::oneshot_state()
{
    if (!m_oneshot)
        m_oneshot = std::make_shared<Nonplanar::OneShotState>();
    return *m_oneshot;
}

const Nonplanar::ConformalResult& PrintObject::conformal() const
{
    static const Nonplanar::ConformalResult empty;
    if (!m_conformal && m_shared_object != nullptr)
        return m_shared_object->conformal();
    return m_conformal ? *m_conformal : empty;
}

static bool contours_paths(const PrintObject &object, const Print &print)
{
    const bool open_vase = print.config().spiral_mode.value && !Nonplanar::vase_with_conformal_roof(object);
    for (size_t region_id = 0; region_id < object.num_printing_regions(); region_id++) {
        const auto &config = object.printing_region(region_id).config();
        if (config.zaa_enabled || object.curved_fallback_zaa() || (!open_vase && Nonplanar::top_shells_enabled(config, object.config()) &&
                                   Nonplanar::effective_fallback(config, object.config()) != NonplanarFallback::Planar))
            return true;
    }
    return false;
}

bool PrintObject::need_z_contouring() const
{
    if (contours_paths(*this, *m_print) || Nonplanar::has_top_texture(*this))
        return true;
    const NonplanarMode mode = m_config.nonplanar_mode.value;
    if (mode == NonplanarMode::Off || curved_tops_mode(mode))
        for (size_t region_id = 0; region_id < this->num_printing_regions(); region_id++)
            if (this->printing_region(region_id).config().nonplanar_sublayer_walls.value)
                return true;
    return false;
}

void PrintObject::contour_z()
{
    if (!this->set_started(posContouring)) {
        return;
    }

    m_print->set_status(40, L("Z contouring"));
    BOOST_LOG_TRIVIAL(debug) << "Contouring in parallel - start";

    if (contours_paths(*this, *m_print)) {
    double           ground_offset = 0.;
    TriangleMesh     mesh          = this->object_frame_mesh(ground_offset);
    sla::IndexedMesh imesh(mesh);
    imesh.ground_level_offset(ground_offset);
    imesh.enable_column_cache();

    std::mutex mtx;
    size_t completed = 0;
    tbb::parallel_for(
        tbb::blocked_range<size_t>(1, m_layers.size()),
        [&, this](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); layer_idx++) {
                m_print->throw_if_canceled();
                m_layers[layer_idx]->make_contour_z(imesh);

                std::scoped_lock lock(mtx);
                completed++;
                std::string msg = (boost::format("Z contoured layer %d/%d (%d%%)") % (completed) % m_layers.size() % int(double(completed) / m_layers.size() * 100)).str();
                m_print->set_status(40, msg);
            }
        }
    );
    }
    m_print->throw_if_canceled();
    Nonplanar::texture_tops(*this);
    if (const std::string why = Nonplanar::split_sublayer_walls(*this); !why.empty())
        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "sublayer_walls_skipped", warning_object(this->model_object()->name) }, this->model_object()->name + ": " + why + ".");
    BOOST_LOG_TRIVIAL(debug) << "Contouring in parallel - end";

    this->set_done(posContouring);
}

void PrintObject::clear_overhangs_for_lift()
{
    if (!m_shared_object) {
        for (Layer* l : m_layers)
            l->loverhangs.clear();
    }
}

static const float g_min_overhang_percent_for_lift = 0.3f;

void PrintObject::detect_overhangs_for_lift()
{
    if (this->set_started(posDetectOverhangsForLift)) {
        const double nozzle_diameter = m_print->config().nozzle_diameter.get_at(0);
        const coordf_t line_width = this->config().get_abs_value("line_width", nozzle_diameter);

        const float min_overlap = line_width * g_min_overhang_percent_for_lift;
        size_t num_layers = this->layer_count();
        size_t num_raft_layers = m_slicing_params.raft_layers();

        m_print->set_status(71, L("Detect overhangs for auto-lift"));

        this->clear_overhangs_for_lift();

        tbb::spin_mutex layer_storage_mutex;
        tbb::parallel_for(tbb::blocked_range<size_t>(num_raft_layers + 1, num_layers),
            [this, min_overlap, line_width](const tbb::blocked_range<size_t>& range)
            {
                for (size_t layer_id = range.begin(); layer_id < range.end(); ++layer_id) {
                    Layer& layer = *m_layers[layer_id];
                    Layer& lower_layer = *layer.lower_layer;

                    ExPolygons overhangs = diff_ex(layer.lslices, offset_ex(lower_layer.lslices, scale_(min_overlap)));
                    layer.loverhangs = offset2_ex(overhangs, -0.1f * scale_(line_width), 0.1f * scale_(line_width));
                    layer.loverhangs_bbox = get_extents(layer.loverhangs);
                }
            });

        this->set_done(posDetectOverhangsForLift);
    }
}

class NonplanarSupportView
{
public:
    explicit NonplanarSupportView(const LayerPtrs &layers)
    {
        for (Layer *layer : layers) {
            if (layer->lslices_before_nonplanar.empty() && layer->region_slices_before_nonplanar.empty())
                continue;
            Saved saved { layer, layer->lslices, layer->lslices_bboxes, {} };
            for (size_t region_id = 0; region_id < layer->regions().size(); ++ region_id) {
                LayerRegion *layerm = layer->get_region(int(region_id));
                saved.region_slices.push_back(layerm->slices);
                if (region_id >= layer->region_slices_before_nonplanar.size())
                    continue;
                Surfaces planar;
                for (const Surface &surface : layerm->slices.surfaces)
                    if (! surface.is_nonplanar())
                        planar.push_back(surface);
                ExPolygons moved_away = diff_ex(layer->region_slices_before_nonplanar[region_id], to_polygons(planar));
                layerm->slices.clear();
                layerm->slices.append(std::move(planar));
                layerm->slices.append(std::move(moved_away), stInternal);
            }
            layer->lslices = layer->lslices_before_nonplanar;
            layer->lslices_bboxes.clear();
            for (const ExPolygon &expoly : layer->lslices)
                layer->lslices_bboxes.emplace_back(get_extents(expoly));
            m_saved.push_back(std::move(saved));
        }
    }
    ~NonplanarSupportView()
    {
        for (Saved &saved : m_saved) {
            saved.layer->lslices        = std::move(saved.lslices);
            saved.layer->lslices_bboxes = std::move(saved.lslices_bboxes);
            for (size_t region_id = 0; region_id < saved.region_slices.size(); ++ region_id)
                saved.layer->get_region(int(region_id))->slices = std::move(saved.region_slices[region_id]);
        }
    }

private:
    struct Saved {
        Layer                         *layer;
        ExPolygons                     lslices;
        std::vector<BoundingBox>       lslices_bboxes;
        std::vector<SurfaceCollection> region_slices;
    };
    std::vector<Saved> m_saved;
};

bool PrintObject::oneshot_support() const { return Nonplanar::oneshot_requested(*this) || m_field_zonly; }

SupportMaterialStyle PrintObject::tree_default_style() const
{
    if (m_config.nonplanar_mode.value != NonplanarMode::Off || !m_model_object->has_custom_layering())
        return smsTreeOrganic;
    std::vector<coordf_t> profile;
    update_layer_height_profile(*m_model_object, this->slicing_parameters(), profile);
    return check_object_layers_fixed(this->slicing_parameters(), profile) ? smsTreeOrganic : smsTreeSlim;
}

void PrintObject::generate_support_material(bool with_belt_brim)
{
    if (this->set_started(posSupportMaterial)) {
        this->clear_support_layers();
        NonplanarSupportView nonplanar_support_view(m_layers);
        auto apply_auto_support = [this](bool critical_only, SupportMaterialStyle style) {
            if (!m_auto_support_applied) {
                m_auto_support_type     = m_config.support_type.value;
                m_auto_support_style    = m_config.support_style.value;
                m_auto_support_critical = m_config.support_critical_regions_only.value;
            }
            m_auto_support_applied                       = true;
            m_config.enable_support.value                = true;
            m_config.support_type.value                  = stTreeAuto;
            m_config.support_critical_regions_only.value = critical_only;
            m_config.support_style.value                 = style;
            m_slicing_params.valid = false;
            this->update_slicing_parameters();
        };
        if (m_auto_support && !m_auto_support_applied && !m_config.enable_support.value)
            apply_auto_support(true, smsTreeHybrid);

        if (m_oneshot_offer_support) {
            m_config.support_type.value  = m_oneshot_offer_type;
            m_config.support_style.value = m_oneshot_offer_style;
            m_oneshot_offer_support      = false;
        }
        if (const Nonplanar::OneShotState *state = m_oneshot.get();
            state != nullptr && state->offer != Nonplanar::OneShotState::Offer::Body && m_config.enable_support.value &&
            Nonplanar::oneshot_requested(*this)) {
            m_oneshot_offer_support      = true;
            m_oneshot_offer_type         = m_config.support_type.value;
            m_oneshot_offer_style        = m_config.support_style.value;
            const bool normal            = state->offer == Nonplanar::OneShotState::Offer::NormalSupport;
            m_config.support_type.value  = normal ? stNormalAuto : stTreeAuto;
            m_config.support_style.value = normal ? smsDefault : smsTreeSlim;
            m_slicing_params.valid       = false;
            this->update_slicing_parameters();
        }

        if(!has_support() && !m_print->get_no_check_flag()) {
            m_print->set_status(50, L("Checking support necessity"));
            typedef std::chrono::high_resolution_clock clock_;
            typedef std::chrono::duration<double, std::ratio<1> > second_;
            std::chrono::time_point<clock_> t0{ clock_::now() };

            SupportNecessaryType sntype = this->is_support_necessary();

            double duration{ std::chrono::duration_cast<second_>(clock_::now() - t0).count() };
            BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(0) << "is_support_necessary takes " << duration << " secs.";

            if (sntype != NoNeedSupp) {
                std::map<SupportNecessaryType, std::string> reasons = {
                    {SharpTail,L("floating regions")},
                    {Cantilever,L("floating cantilever")},
                    {LargeOverhang,L("large overhangs")} };
                std::string warning_message = Slic3r::format(L("Looks like %s has %s. Try rotating it or turning on supports."),
                    this->model_object()->name, reasons[sntype]);
                this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ sntype == SharpTail ? "floating_regions" : sntype == Cantilever ? "floating_cantilever" : "large_overhangs", warning_object(this->model_object()->name) }, warning_message, PrintStateBase::SlicingNeedSupportOn);
            }

#if 0
            for (const Layer *layer : m_layers)
                if (layer->empty())
                    throw Slic3r::SlicingError("Levitating objects cannot be printed without supports.");
#endif
        }

        if ((this->has_support() && m_layers.size() > 1) || (this->has_raft() && !m_layers.empty())) {
            m_print->set_status(50, L("Generating support"));

            std::vector<ExPolygons> grounded = m_multiplanar ? Nonplanar::grounded_route_slices(*this) : std::vector<ExPolygons>();
            const std::vector<ExPolygons> upper = Nonplanar::grounded_route_upper(*this);
            for (size_t i = 0; i + 1 < std::min({ grounded.size(), upper.size(), m_layers.size() }); ++i)
                if (const ExPolygons chunks = diff_ex(upper[i], m_layers[i + 1]->lslices); !chunks.empty())
                    grounded[i] = union_ex(grounded[i], chunks);
            const bool              plate_only = m_config.support_on_build_plate_only.value;
            auto swap_grounded = [this, &grounded]() {
                for (size_t i = 0; i < std::min(grounded.size(), m_layers.size()); ++i) {
                    std::swap(m_layers[i]->lslices, grounded[i]);
                    m_layers[i]->lslices_bboxes.clear();
                    for (const ExPolygon &expoly : m_layers[i]->lslices)
                        m_layers[i]->lslices_bboxes.emplace_back(get_extents(expoly));
                }
            };
            if (!grounded.empty()) {
                swap_grounded();
                m_config.support_on_build_plate_only.value = true;
            }
            try {
                this->_generate_support_material();
            } catch (...) {
                if (!grounded.empty()) {
                    swap_grounded();
                    m_config.support_on_build_plate_only.value = plate_only;
                }
                throw;
            }
            if (!grounded.empty()) {
                swap_grounded();
                m_config.support_on_build_plate_only.value = plate_only;
            }
            if (m_multiplanar)
                Nonplanar::clear_route_support(*this, *m_multiplanar);
            if (m_auto_support_applied) {
                double volume = 0.;
                for (const SupportLayer *layer : m_support_layers)
                    volume += layer->support_fills.total_volume();
                std::string message;
                if (volume <= 0.) {
                    this->clear_support_layers();
                    m_config.enable_support.value = false;
                    m_config.support_type.value   = m_auto_support_type;
                    m_config.support_style.value  = m_auto_support_style;
                    m_config.support_critical_regions_only.value = m_auto_support_critical;
                    m_auto_support_applied        = false;
                    m_slicing_params.valid        = false;
                    this->update_slicing_parameters();
                    if (m_auto_support_need != NoNeedSupp) {
                        message = Slic3r::format(L("Looks like %s has %s. Try rotating it or turning on supports."),
                                                 this->model_object()->name,
                                                 m_auto_support_need == SharpTail ? L("floating regions") : L("floating cantilever"));
                        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ m_auto_support_need == SharpTail ? "floating_regions" : "floating_cantilever", warning_object(this->model_object()->name) }, message, PrintStateBase::SlicingNeedSupportOn);
                    }
                } else {
                    message = (boost::format(_u8L("Support added: %1% would float without it (%2$.0f mm³).")) %
                               this->model_object()->name % volume).str();
                    this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "support_added", warning_object(this->model_object()->name), nlohmann::json{ { "volume_mm3", volume } }.dump() }, message);
                }
                m_auto_support_volume = m_auto_support_applied ? volume : 0.;
                BOOST_LOG_TRIVIAL(warning) << "Auto support " << this->model_object()->name << ": " << volume << " mm3 kept "
                                           << m_auto_support_applied;
            }
        }
        if ((this->has_support() && m_layers.size() > 1) || (this->has_raft() && !m_layers.empty())) {
            Nonplanar::contour_support_interface(*this);
            if (Nonplanar::conformal_support_wanted(*this)) {
                const bool fresh = !m_conformal;
                if (fresh)
                    m_conformal = std::make_unique<Nonplanar::ConformalResult>();
                Nonplanar::build_conformal_support(*this, *m_conformal);
                if (fresh && m_conformal->patches.empty())
                    m_conformal.reset();
            }
            m_print->throw_if_canceled();
        }
        m_auto_band_demoted = false;
        m_auto_kept_warning.clear();
        if (!m_auto_floating_planar && !m_auto_planar_exact && (m_config.nonplanar_mode.value == NonplanarMode::Auto || m_auto_planar) &&
            m_print->config().nonplanar_kinematics.value == RotaryLayout::None && !Nonplanar::rotary_auto_measurement() &&
            !m_print->config().spiral_mode.value) {
            const double band   = this->wall_band();
            static const bool uniform = [] {
                const char *v = research_switch("NP_AUTO_UNIFORM");
                return v != nullptr && *v != 0 && *v != '0';
            }();
            m_auto_band_demoted = band >= WallBand && !uniform;
            BOOST_LOG_TRIVIAL(warning) << "Auto wall band " << this->model_object()->name << ": " << band
                                       << (m_auto_band_demoted ? ", printed planar exactly" : ", kept");
        }
        m_auto_floating_demoted = false;
        m_auto_curved_floating  = -1.;
        if (!m_auto_floating_planar && m_config.nonplanar_mode.value == NonplanarMode::Auto && m_conformal &&
            !m_conformal->patches.empty() && !Nonplanar::rotary_auto_measurement() && !m_print->config().spiral_mode.value) {
            const Nonplanar::Floating curved = Nonplanar::floating_road(*this, m_conformal.get());
            m_auto_curved_floating           = curved.part;
            if (Nonplanar::floats_past_bar(curved, {})) {
                const bool                user_support = m_config.enable_support.value && !m_auto_support_applied;
                const Nonplanar::Floating planar       = Nonplanar::planar_control_floating(*this, user_support);
                m_print->throw_if_canceled();
                m_auto_floating_demoted = planar.part >= 0. && Nonplanar::floats_past_bar(curved, planar);
                if (!user_support)
                    if (const std::string why = Nonplanar::may_need_support(this->model_object()->name, planar); !why.empty())
                        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "may_need_support", warning_object(this->model_object()->name) }, why);
                BOOST_LOG_TRIVIAL(warning) << "Auto floating " << this->model_object()->name << ": " << curved.part
                                           << " mm of part road in air (support " << curved.support << ") against the planar control's "
                                           << planar.part << " + " << planar.support << " (" << planar.air << " into the air)"
                                           << (m_auto_floating_demoted ? ", demoted" : ", kept");
            }
        }
        if (m_config.nonplanar_mode.value == NonplanarMode::Field && m_config.nonplanar_atom_toolpath.value.empty()) {
            constexpr double MinFieldOverhangArea = 20.;
            const bool planned = std::any_of(m_support_layers.begin(), m_support_layers.end(),
                                             [](const SupportLayer *layer) { return !layer->support_fills.empty(); });
            double residual = -1., planar = -1.;
            if (m_field)
                residual = m_field->stats().overhang_area, planar = m_field->stats().planar_overhang_area;
            else if (!m_config.nonplanar_field_file.value.empty()) {
                for (const auto &[key, value] : Nonplanar::load_npfield(m_config.nonplanar_field_file.value).terms) {
                    if (key == "stat_overhang_area_mm2")
                        residual = value;
                    else if (key == "stat_planar_overhang_area_mm2")
                        planar = value;
                }
            }
            const double added  = residual >= 0. && planar >= 0. ? residual - planar : 0.;
            const bool   floats = m_planar_floating_mm >= 0. && m_field_floating_mm >= 0. &&
                                m_field_floating_mm > m_planar_floating_mm * 1.05 + 2.;
            BOOST_LOG_TRIVIAL(info) << "Field path " << this->model_object()->name << ": overhang " << residual << " mm2 (planar "
                                    << planar << "), floating wall " << m_field_floating_mm << " mm (planar " << m_planar_floating_mm
                                    << "), Orca's check " << int(m_auto_support_need);
            if (!planned && (m_auto_support_need != NoNeedSupp || added > MinFieldOverhangArea || floats))
                throw Slic3r::SlicingError(Slic3r::format(
                    _u8L("The field path can't print the support %1% needs (%2%; %3$.0f mm2 of overhang against planar's %4$.0f, "
                         "%5$.0f mm of floating wall against planar's %6$.0f): print it one-shot or in planar layers."),
                    this->model_object()->name,
                    m_auto_support_need == SharpTail     ? _u8L("floating regions") :
                    m_auto_support_need == Cantilever    ? _u8L("floating cantilever") :
                    m_auto_support_need == LargeOverhang ? _u8L("large overhangs") :
                    floats                               ? _u8L("it floats more than planar") :
                                                           _u8L("overhang beyond planar's"),
                    std::max(residual, 0.), std::max(planar, 0.), m_field_floating_mm, m_planar_floating_mm));
        }
        m_belt_brim_pending = true;
        if (with_belt_brim)
            this->generate_belt_brim();
        this->set_done(posSupportMaterial);
    }
}

void PrintObject::generate_belt_brim()
{
    if (! m_belt_brim_pending)
        return;
    make_belt_brim(*this);
    m_print->throw_if_canceled();
    m_belt_brim_pending = false;
}

void PrintObject::estimate_curled_extrusions()
{
    if (this->set_started(posEstimateCurledExtrusions)) {
        const auto any_region_enables = [this](ConfigOptionBoolsNullable PrintRegionConfig::*option) {
            return std::any_of(this->print()->m_print_regions.begin(), this->print()->m_print_regions.end(),
                               [option](const PrintRegion* region) { return any_enabled(region->config().*option); });
        };
        if (any_region_enables(&PrintRegionConfig::enable_overhang_speed) &&
            any_region_enables(&PrintRegionConfig::slowdown_for_curled_perimeters)) {

            float support_flow_width = support_material_flow(this, this->config().layer_height).width();
            SupportSpotsGenerator::Params params{this->print()->m_config.filament_type.values,
                                                 this->config().raft_layers.getInt(), this->config().brim_type.value,
                                                 float(this->config().brim_width.getFloat())};
            SupportSpotsGenerator::estimate_malformations(this->layers(), params);
            m_print->throw_if_canceled();
        } else {
            for (Layer *layer : m_layers)
                layer->curled_lines.clear();
        }
    }
}

void PrintObject::plan_wide_tops()
{
    m_wide_plan.reset();
    if (!curved_tops_mode(m_config.nonplanar_mode.value) || m_config.nonplanar_curved_max_angle.value <= 12. + EPSILON)
        return;
    const bool simplified = this->is_step_done(posSimplifyPath) && this->is_step_done(posSimplifyInfill);
    if (! simplified && (this->is_step_done(posSimplifyPath) || this->is_step_done(posSimplifyInfill)))
        return;
    struct Originals {
        std::vector<std::pair<LayerRegion*, std::pair<ExtrusionEntityCollection, ExtrusionEntityCollection>>> regions;
        ~Originals() {
            for (auto &[region, paths] : regions) {
                region->perimeters = std::move(paths.first);
                region->fills      = std::move(paths.second);
            }
        }
    } originals;
    if (! simplified) {
        for (Layer *layer : m_layers)
            for (LayerRegion *region : layer->regions()) {
                originals.regions.push_back({ region, { std::move(region->perimeters), std::move(region->fills) } });
                region->perimeters = originals.regions.back().second.first;
                region->fills      = originals.regions.back().second.second;
            }
        tbb::parallel_for(tbb::blocked_range<size_t>(0, m_layers.size()), [this](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++ i) {
                m_print->throw_if_canceled();
                m_layers[i]->simplify_wall_extrusion_path();
                m_layers[i]->simplify_infill_extrusion_path();
            }
        });
    }
    try {
        const size_t fingerprint = nonplanar_plan_fingerprint(*this);
        auto plan = std::make_shared<const NonplanarDeferral>(
            plan_nonplanar_deferral_reusing(*this, NonplanarClearanceModel::for_object(*this, m_config.layer_height.value), fingerprint));
        m_wide_plan_fingerprint = fingerprint;
        m_wide_plan = std::move(plan);
    } catch (const CurvedTopsAccessError &error) {
        BOOST_LOG_TRIVIAL(warning) << this->model_object()->name << ": no collision-free deposition order for " << error.surfaces.size()
                                   << " curved top surfaces; the export prints them planar";
        m_wide_plan.reset();
    }
}

void PrintObject::simplify_extrusion_path()
{
    if (this->set_started(posSimplifyPath)) {
        m_print->set_status(75, L("Optimizing toolpath"));
        BOOST_LOG_TRIVIAL(debug) << "Simplify extrusion path of object in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    m_layers[layer_idx]->simplify_wall_extrusion_path();
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Simplify wall extrusion path of object in parallel - end";
        this->set_done(posSimplifyPath);
    }

    if (this->set_started(posSimplifyInfill)) {
        m_print->set_status(75, L("Optimizing toolpath"));
        BOOST_LOG_TRIVIAL(debug) << "Simplify infill extrusion path of object in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
                    m_print->throw_if_canceled();
                    m_layers[layer_idx]->simplify_infill_extrusion_path();
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Simplify infill extrusion path of object in parallel - end";
        this->set_done(posSimplifyInfill);
    }

    if (this->set_started(posSimplifySupportPath)) {
        m_print->set_status(75, L("Optimizing toolpath"));
        BOOST_LOG_TRIVIAL(debug) << "Simplify extrusion path of support in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_support_layers.size()),
            [this](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    m_support_layers[layer_idx]->simplify_support_extrusion_path();
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Simplify extrusion path of support in parallel - end";
        this->set_done(posSimplifySupportPath);
    }
}

static int separated_body_at(const Layer &layer, const Point &point)
{
    int    body = -1;
    double best = scaled<double>(1.);
    for (size_t i = 0; i < layer.lslices.size() && i < layer.lslices_separated_component_ids.size() && best > 0.; ++ i) {
        BoundingBox bbox = layer.lslices_bboxes[i];
        bbox.offset(coord_t(best));
        if (! bbox.contains(point))
            continue;
        const double dist = layer.lslices[i].contains(point) ? 0. : (layer.lslices[i].point_projection(point) - point).cast<double>().norm();
        if (dist < best) {
            best = dist;
            body = int(layer.lslices_separated_component_ids[i]);
        }
    }
    return body;
}

static std::vector<indexed_triangle_set> split_mesh_by_body(const PrintObject &object, const indexed_triangle_set &mesh, size_t num_bodies)
{
    const Eigen::Matrix3d             to_object = FillAdaptive::transform_to_world().toRotationMatrix();
    const double                      inset     = object.config().layer_height.value;
    std::vector<indexed_triangle_set> bodies(num_bodies);
    for (const indexed_triangle_set &component : its_split(mesh)) {
        std::vector<size_t> votes(num_bodies, 0);
        const size_t        step = std::max<size_t>(1, component.indices.size() / 8);
        for (size_t i = 0; i < component.indices.size(); i += step) {
            const stl_triangle_vertex_indices &tri = component.indices[i];
            const Vec3d a = component.vertices[tri[0]].cast<double>(), b = component.vertices[tri[1]].cast<double>(),
                        d = component.vertices[tri[2]].cast<double>();
            const Vec3d  normal = (b - a).cross(d - a);
            const double area2  = normal.norm();
            const Vec3d  c = to_object * ((a + b + d) / 3. - (area2 > 0. ? Vec3d(normal * (inset / area2)) : Vec3d::Zero()));
            size_t lo = 0, hi = object.layer_count();
            while (lo < hi) {
                const size_t mid = (lo + hi) / 2;
                if (object.get_layer(int(mid))->slice_z < c.z())
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo == object.layer_count() || (lo > 0 && c.z() - object.get_layer(int(lo) - 1)->slice_z < object.get_layer(int(lo))->slice_z - c.z()))
                -- lo;
            if (const int body = separated_body_at(*object.get_layer(int(lo)), Point(scaled<coord_t>(c.x()), scaled<coord_t>(c.y()))); body >= 0)
                ++ votes[body];
        }
        if (const auto best = std::max_element(votes.begin(), votes.end()); *best > 0)
            its_merge(bodies[best - votes.begin()], component);
    }
    return bodies;
}

FillAdaptive::RegionOctrees PrintObject::prepare_adaptive_infill_data(
    const std::vector<std::pair<const Surface *, const Layer *>> &surfaces_w_layer) const
{
    using namespace FillAdaptive;

    const std::vector<double>            line_spacing = adaptive_fill_line_spacing(*this);
    std::vector<std::pair<double, bool>> spacings;
    RegionOctrees                        octrees;
    octrees.region_set.assign(line_spacing.size(), -1);
    for (size_t region_id = 0; region_id < line_spacing.size(); ++ region_id)
        if (line_spacing[region_id] > 0.) {
            const std::pair<double, bool> spacing(line_spacing[region_id], this->printing_region(region_id).config().sparse_infill_pattern == ipSupportCubic);
            const auto                    it = std::find(spacings.begin(), spacings.end(), spacing);
            octrees.region_set[region_id]    = int(it - spacings.begin());
            if (it == spacings.end())
                spacings.push_back(spacing);
        }
    if (spacings.empty() || this->layers().empty())
        return {};
    octrees.sets.resize(spacings.size());

    indexed_triangle_set mesh = this->model_object()->raw_indexed_triangle_set();
    auto to_octree = transform_to_octree().toRotationMatrix();
    Transform3d object_trafo = this->trafo_sliced();
    object_trafo.translation().z() += m_belt_global_z_offset;
    its_transform(mesh, to_octree * object_trafo, true);

    std::vector<std::vector<Vec3d>> overhangs(std::max(surfaces_w_layer.size(), size_t(1)));
    tbb::parallel_for(tbb::blocked_range<int>(0, surfaces_w_layer.size()),
        [this, &to_octree, &overhangs, &surfaces_w_layer](const tbb::blocked_range<int> &range) {
            PRINT_OBJECT_TIME_LIMIT_MILLIS(PRINT_OBJECT_TIME_LIMIT_DEFAULT);
            for (int surface_idx = range.begin(); surface_idx < range.end(); ++surface_idx) {
                std::vector<Vec3d> &out = overhangs[surface_idx];
                m_print->throw_if_canceled();
                append(out, triangulate_expolygon_3d(surfaces_w_layer[surface_idx].first->expolygon,
                                                   float(surfaces_w_layer[surface_idx].second->bottom_z())));
                for (Vec3d &p : out)
                    p = (to_octree * p).eval();
            }
        });

    const size_t                           num_bodies = m_separated_body_bboxes.size();
    std::vector<std::pair<size_t, size_t>> to_build;
    std::vector<indexed_triangle_set>      body_meshes;
    std::vector<std::vector<Vec3d>>        body_overhangs(num_bodies);
    if (num_bodies > 1) {
        body_meshes = split_mesh_by_body(*this, mesh, num_bodies);
        for (size_t i = 0; i < surfaces_w_layer.size(); ++ i)
            if (const int body = separated_body_at(*surfaces_w_layer[i].second, surfaces_w_layer[i].first->expolygon.contour.points.front()); body >= 0)
                append(body_overhangs[body], overhangs[i]);
        std::vector<std::vector<char>> fills(spacings.size(), std::vector<char>(num_bodies + 1, false));
        for (const Layer *layer : m_layers)
            for (size_t region_id = 0; region_id < layer->regions().size() && region_id < octrees.region_set.size(); ++ region_id)
                if (const int set = octrees.region_set[region_id]; set >= 0)
                    for (const Surface &surface : layer->regions()[region_id]->fill_surfaces) {
                        const int body = separated_body_at(*layer, surface.expolygon.contour.points.front());
                        fills[set][body >= 0 && ! body_meshes[body].indices.empty() ? size_t(body) : num_bodies] = true;
                    }
        for (size_t set = 0; set < spacings.size(); ++ set) {
            octrees.sets[set].bodies.resize(num_bodies);
            for (size_t body = 0; body <= num_bodies; ++ body)
                if (fills[set][body])
                    to_build.emplace_back(set, body);
        }
    } else
        for (size_t set = 0; set < spacings.size(); ++ set)
            to_build.emplace_back(set, num_bodies);

    for (size_t i = 1; i < overhangs.size(); ++ i)
        append(overhangs.front(), std::move(overhangs[i]));

    tbb::parallel_for(tbb::blocked_range<size_t>(0, to_build.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++ i) {
            m_print->throw_if_canceled();
            const auto [set, body] = to_build[i];
            const bool object      = body == num_bodies;
            (object ? octrees.sets[set].object : octrees.sets[set].bodies[body]) =
                build_octree(object ? mesh : body_meshes[body], object ? overhangs.front() : body_overhangs[body], spacings[set].first,
                             spacings[set].second);
        }
    });
    return octrees;
}

FillLightning::GeneratorPtr PrintObject::prepare_lightning_infill_data()
{
    bool has_lightning_infill = false;
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id)
        if (const PrintRegionConfig& config = this->printing_region(region_id).config(); config.sparse_infill_density > 0 && config.sparse_infill_pattern == ipLightning) {
            has_lightning_infill = true;
            break;
        }

    return has_lightning_infill ? FillLightning::build_generator(std::as_const(*this), [this]() -> void { this->throw_if_canceled(); }) : FillLightning::GeneratorPtr();
}

TpmsRadialFields PrintObject::prepare_tpms_radial_fields() const
{
    TpmsRadialFields fields;
    std::array<bool, size_t(TpmsAdaptiveMode::Count)> modes{};
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id)
        if (const PrintRegionConfig &config = this->printing_region(region_id).config();
            config.sparse_infill_density > 0 && config.sparse_infill_density < 100 && is_tpms_adaptive_pattern(config.sparse_infill_pattern))
            modes[size_t(config.tpms_adaptive.value)] = true;
    modes[size_t(TpmsAdaptiveMode::Disabled)] = false;
    if (std::find(modes.begin(), modes.end(), true) == modes.end() || m_layers.empty())
        return fields;

    std::vector<TpmsRadialField::Slice> slices;
    slices.reserve(m_layers.size());
    BoundingBox bbox;
    for (const Layer *layer : m_layers) {
        slices.push_back({layer->bottom_z(), layer->print_z, &layer->lslices});
        bbox.merge(get_extents(layer->lslices));
    }
    if (!bbox.defined)
        return fields;
    for (size_t mode = 0; mode < modes.size(); ++mode) {
        if (!modes[mode])
            continue;
        auto field = std::make_unique<TpmsRadialField>(slices, bbox, TpmsAdaptiveMode(mode), [this]() { m_print->throw_if_canceled(); });
        if (!field->empty())
            fields[mode] = std::move(field);
    }
    return fields;
}

void PrintObject::clear_layers()
{
    m_nonplanar_region_cache->clear();
    if (!m_shared_object) {
        for (Layer *l : m_layers)
            delete l;
        m_layers.clear();
        for (Layer *l : m_belt_truncated_layers)
            delete l;
        m_belt_truncated_layers.clear();
        for (const BeltDroppedFill &d : m_belt_dropped_fills)
            delete d.entity;
        m_belt_dropped_fills.clear();
    }
}

Layer* PrintObject::add_layer(int id, coordf_t height, coordf_t print_z, coordf_t slice_z)
{
    m_layers.emplace_back(new Layer(id, this, height, print_z, slice_z));
    return m_layers.back();
}

const SupportLayer* PrintObject::get_support_layer_at_printz(coordf_t print_z, coordf_t epsilon) const
{
    coordf_t limit = print_z - epsilon;
    auto it = Slic3r::lower_bound_by_predicate(m_support_layers.begin(), m_support_layers.end(), [limit](const SupportLayer* layer) { return layer->print_z < limit; });
    return (it == m_support_layers.end() || (*it)->print_z > print_z + epsilon) ? nullptr : *it;
}

SupportLayer* PrintObject::get_support_layer_at_printz(coordf_t print_z, coordf_t epsilon)
{
    return const_cast<SupportLayer*>(std::as_const(*this).get_support_layer_at_printz(print_z, epsilon));
}

void PrintObject::clear_support_layers()
{
    if (!m_shared_object) {
        for (SupportLayer* l : m_support_layers)
            delete l;
        m_support_layers.clear();
        for (auto l : m_layers) {
            l->sharp_tails.clear();
            l->sharp_tails_height.clear();
            l->cantilevers.clear();
        }
    }
    this->clear_belt_brim();
}

bool PrintObject::has_belt_brim() const
{
    if (! m_print->has_tilted_belt())
        return false;
    if (m_config.belt_purge_tower_object.value)
        return false;
    if (m_config.brim_type == btNoBrim)
        return false;
    if (m_config.brim_type == btInnerOnly) {
        if (m_config.brim_width.value <= 0.)
            return false;
    } else if (m_config.brim_width.value <= 0. && m_config.leading_brim_length.value <= 0.
               && m_config.extra_brim_width.value <= 0.) {
        return false;
    }
    return ! this->has_raft();
}

unsigned int PrintObject::belt_brim_filament() const
{
    unsigned int brim_filament = 0;
    for (size_t i = 0; i < this->num_printing_regions(); ++ i) {
        const unsigned int f = this->printing_region(i).config().outer_wall_filament_id.value;
        if (f > 0 && (brim_filament == 0 || f < brim_filament))
            brim_filament = f;
    }
    return brim_filament == 0 ? 1u : brim_filament;
}

void PrintObject::clear_belt_brim()
{
    m_belt_brim_by_layer.clear();
    m_belt_brim_areas_by_layer.clear();
    m_belt_brim_prologue.clear();
}

void PrintObject::set_belt_brim(std::vector<ExtrusionEntityCollection> &&by_layer,
                                std::vector<ExPolygons>                &&areas,
                                std::vector<BeltBrimBand>              &&prologue)
{
    m_belt_brim_by_layer       = std::move(by_layer);
    m_belt_brim_areas_by_layer = std::move(areas);
    m_belt_brim_prologue       = std::move(prologue);
}

std::shared_ptr<TreeSupportData> PrintObject::alloc_tree_support_preview_cache()
{
    if (!m_tree_support_preview_cache) {
        const coordf_t xy_distance = m_config.support_object_xy_distance.value;
        m_tree_support_preview_cache = std::make_shared<TreeSupportData>(*this, xy_distance, g_config_tree_support_collision_resolution);
    }

    return m_tree_support_preview_cache;
}

SupportLayer* PrintObject::add_tree_support_layer(int id, coordf_t height, coordf_t print_z, coordf_t slice_z)
{
    m_support_layers.emplace_back(new SupportLayer(id, 0, this, height, print_z, slice_z));
    m_support_layers.back()->support_type = stInnerTree;
    return m_support_layers.back();
}

SupportLayer* PrintObject::add_support_layer(int id, int interface_id, coordf_t height, coordf_t print_z)
{
    m_support_layers.emplace_back(new SupportLayer(id, interface_id, this, height, print_z, -1));
    return m_support_layers.back();
}

SupportLayerPtrs::iterator PrintObject::insert_support_layer(SupportLayerPtrs::iterator pos, size_t id, size_t interface_id, coordf_t height, coordf_t print_z, coordf_t slice_z)
{
    return m_support_layers.insert(pos, new SupportLayer(id, interface_id, this, height, print_z, slice_z));
}

const std::unordered_set<std::string> &PrintObject::gcode_only_options()
{
    static const std::unordered_set<std::string> keys = {
        "seam_slope_type",
        "seam_slope_conditional",
        "scarf_angle_threshold",
        "scarf_overhang_threshold",
        "scarf_joint_speed",
        "seam_slope_start_height",
        "seam_slope_entire_loop",
        "seam_slope_min_length",
        "seam_slope_steps",
        "seam_slope_inner_walls",
        "support_interface_speed",
        "overhang_1_4_speed",
        "overhang_2_4_speed",
        "overhang_3_4_speed",
        "overhang_4_4_speed",
        "internal_bridge_speed",
        "bed_mesh_min",
        "bed_mesh_max",
        "adaptive_bed_mesh_margin",
        "bed_mesh_probe_distance",
        "print_flow_ratio",
        "first_layer_flow_ratio",
        "top_solid_infill_flow_ratio",
        "set_nonplanar_top_surface_flow_ratio",
        "nonplanar_top_surface_flow_ratio",
        "nonplanar_top_finish_flow",
        "nonplanar_curved_shell_flow",
        "nonplanar_tilt_flow",
        "nonplanar_top_speed_factor",
        "nonplanar_curved_shell_speed",
        "nonplanar_top_fan_boost",
        "bottom_solid_infill_flow_ratio",
        "outer_wall_flow_ratio",
        "inner_wall_flow_ratio",
        "overhang_flow_ratio",
        "sparse_infill_flow_ratio",
        "internal_solid_infill_flow_ratio",
        "gap_fill_flow_ratio",
        "support_flow_ratio",
        "support_interface_flow_ratio",
        "brim_flow_ratio",
        "filament_flow_ratio",
        "scarf_joint_flow_ratio",
        "wipe_inward",
        "wipe_inward_distance",
        "spiral_starting_flow_ratio",
        "spiral_finishing_flow_ratio",
    };
    return keys;
}

bool PrintObject::invalidate_state_by_config_options(
    const ConfigOptionResolver &old_config, const ConfigOptionResolver &new_config, const std::vector<t_config_option_key> &opt_keys)
{
    if (opt_keys.empty())
        return false;

    std::vector<PrintObjectStep> steps;
    bool invalidated = false;
    if (!m_curved_planar_surfaces.empty()) {
        m_curved_planar_surfaces.clear();
        steps.emplace_back(posSlice);
    }
    if (m_curved_fallback_zaa || m_curved_fallback_off) {
        m_curved_fallback_zaa = m_curved_fallback_off = false;
        steps.emplace_back(posPerimeters);
    }
    if (m_auto_support_applied && std::any_of(opt_keys.begin(), opt_keys.end(), [](const t_config_option_key &key) {
            return key == "enable_support" || key == "support_type" || key == "support_style" || key == "support_critical_regions_only";
        })) {
        m_config.enable_support.value                = false;
        m_config.support_type.value                  = m_auto_support_type;
        m_config.support_style.value                 = m_auto_support_style;
        m_config.support_critical_regions_only.value = m_auto_support_critical;
        m_auto_support_applied                       = false;
        steps.emplace_back(posSlice);
    }
    if (m_oneshot_offer_support && std::any_of(opt_keys.begin(), opt_keys.end(), [](const t_config_option_key &key) {
            return key == "enable_support" || key == "support_type" || key == "support_style";
        })) {
        m_config.support_type.value  = m_oneshot_offer_type;
        m_config.support_style.value = m_oneshot_offer_style;
        m_oneshot_offer_support      = false;
        steps.emplace_back(posSlice);
    }
    if (m_auto_planar && std::any_of(opt_keys.begin(), opt_keys.end(), [](const t_config_option_key &key) {
            return key == "nonplanar_mode" || key == "top_surface_acceleration" || key == "nonplanar_fallback_top_acceleration" ||
                   key == "nonplanar_raised_top_acceleration";
        })) {
        m_auto_planar = false;
        steps.emplace_back(posSlice);
    }
    static const std::set<std::string> low_plane_keys = { "nonplanar_top_shells", "nonplanar_fallback", "nonplanar_shell_layer_height",
                                                          "wall_loops", "inner_wall_line_width", "top_surface_line_width" };
    static const std::set<std::string> deform_keys = {
        "seam_position", "outer_wall_speed", "inner_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed",
        "top_surface_speed", "gap_infill_speed", "bridge_speed", "default_acceleration", "outer_wall_acceleration",
        "inner_wall_acceleration", "sparse_infill_acceleration", "internal_solid_infill_acceleration", "top_surface_acceleration",
        "bridge_acceleration", "travel_acceleration" };
    const bool curved = m_config.nonplanar_mode.value != NonplanarMode::Off;
    const bool deform = Nonplanar::deform_slice_requested(*this) || Nonplanar::deform_slice_considered(*this);
    for (const t_config_option_key &opt_key : opt_keys) {
        if (curved && low_plane_keys.count(opt_key) > 0)
            steps.emplace_back(posSlice);
        if (deform && deform_keys.count(opt_key) > 0)
            steps.emplace_back(posPerimeters);
        if (   opt_key == "brim_width"
            || opt_key == "leading_brim_length"
            || opt_key == "extra_brim_width"
            || opt_key == "brim_object_gap"
            || opt_key == "brim_use_efc_outline"
            || opt_key == "brim_type"
            || opt_key == "brim_ears_max_angle"
            || opt_key == "brim_ears_detection_length"
            || opt_key == "brim_ears_outer_only"
            || opt_key == "outer_wall_speed"
            || opt_key == "small_perimeter_speed"
            || opt_key == "small_perimeter_threshold"
            || opt_key == "small_support_perimeter_speed"
            || opt_key == "small_support_perimeter_threshold"
            || opt_key == "sparse_infill_speed"
            || opt_key == "inner_wall_speed"
            || opt_key == "support_speed"
            || opt_key == "internal_solid_infill_speed"
            || opt_key == "top_surface_speed") {
            steps.emplace_back(posSupportMaterial);
            if (opt_key == "brim_type") {
                const auto* old_brim_type = old_config.option<ConfigOptionEnum<BrimType>>(opt_key);
                const auto* new_brim_type = new_config.option<ConfigOptionEnum<BrimType>>(opt_key);
                if (old_brim_type->value == btOuterOnly || new_brim_type->value == btOuterOnly ||
                    old_brim_type->value == btLeadingEdgeOnly || new_brim_type->value == btLeadingEdgeOnly)
                    steps.emplace_back(posPerimeters);
            }
        } else if (
               opt_key == "wall_loops"
            || opt_key == "alternate_extra_wall"
            || opt_key == "top_one_wall_type"
            || opt_key == "min_width_top_surface"
            || opt_key == "only_one_wall_first_layer"
            || opt_key == "extra_perimeters_on_overhangs"
            || opt_key == "detect_overhang_wall"
            || opt_key == "initial_layer_line_width"
            || opt_key == "inner_wall_line_width"
            || opt_key == "infill_wall_overlap"
            || opt_key == "top_bottom_infill_wall_overlap"
            || opt_key == "seam_gap"
            || opt_key == "role_based_wipe_speed"
            || opt_key == "wipe_on_loops"
            || opt_key == "wipe_speed") {
            steps.emplace_back(posPerimeters);
        } else if (
            opt_key == "small_area_infill_flow_compensation_model"
            || opt_key == "nonplanar_mode"
            || opt_key.rfind("nonplanar_wave", 0) == 0
            || opt_key == "nonplanar_region_source"
            || opt_key == "nonplanar_deform"
            || opt_key == "multiplanar_max_planes"
            || opt_key == "multiplanar_overhang_threshold"
            || opt_key == "nonplanar_slant_angle"
            || opt_key == "nonplanar_slant_direction"
            || opt_key == "nonplanar_slant_start_height"
            || opt_key == "nonplanar_slant_morph_layers"
            || opt_key == "nonplanar_atom_toolpath"
            || opt_key == "nonplanar_steep_skin"
            || opt_key == "nonplanar_oneshot_routes" || opt_key == "nonplanar_oneshot_fan_step"
            || opt_key == "nonplanar_field_file") {
            steps.emplace_back(posSlice);
        } else if (opt_key == "gap_infill_speed"
            || opt_key == "filter_out_gap_fill" ) {
            auto is_gap_fill_changed_state_due_to_speed = [&opt_key, &old_config, &new_config]() -> bool {
                if (opt_key == "gap_infill_speed") {
                    const auto *old_gap_fill_speed = old_config.option<ConfigOptionFloatsNullable>(opt_key);
                    const auto *new_gap_fill_speed = new_config.option<ConfigOptionFloatsNullable>(opt_key);
                    assert(old_gap_fill_speed && new_gap_fill_speed);
                    return (old_gap_fill_speed->values.size() != new_gap_fill_speed->values.size())
                        || (old_gap_fill_speed->values != new_gap_fill_speed->values);
                }
                return false;
            };

            if (this->is_mm_painted() && (opt_key == "filter_out_gap_fill" && (opt_key == "gap_infill_speed" && is_gap_fill_changed_state_due_to_speed())))
                steps.emplace_back(posSlice);
            steps.emplace_back(posPerimeters);
        } else if (
               opt_key == "layer_height"
            || opt_key == "adaptive_layer_height_error"
            || opt_key == "overhang_layer_contact"
            || opt_key == "mmu_segmented_region_max_width"
            || opt_key == "mmu_segmented_region_interlocking_depth"
            || opt_key == "raft_layers"
            || opt_key == "raft_contact_distance"
            || opt_key == "slice_closing_radius"
            || opt_key == "slicing_mode"
            || opt_key == "slowdown_for_curled_perimeters"
            || opt_key == "make_overhang_printable"
            || opt_key == "make_overhang_printable_angle"
            || opt_key == "make_overhang_printable_hole_size"
            || opt_key == "interlocking_beam"
            || opt_key == "interlocking_orientation"
            || opt_key == "interlocking_beam_layer_count"
            || opt_key == "interlocking_depth"
            || opt_key == "interlocking_boundary_avoidance"
            || opt_key == "interlocking_beam_width") {
            steps.emplace_back(posSlice);
		} else if (
               opt_key == "elefant_foot_compensation"
            || opt_key == "elefant_foot_compensation_layers"
            || opt_key == "elefant_foot_layers_density"
            || opt_key == "support_top_z_distance"
            || opt_key == "support_bottom_z_distance"
            || opt_key == "xy_hole_compensation"
            || opt_key == "xy_contour_compensation"
            || opt_key == "support_type"
            || opt_key == "bridge_no_support"
            || opt_key == "max_bridge_length"
            || opt_key == "support_interface_top_layers"
            || opt_key == "support_critical_regions_only"
            || opt_key == "nonplanar_auto_support"
            || opt_key == "hole_to_polyhole"
            || opt_key == "hole_to_polyhole_threshold"
            || opt_key == "hole_to_polyhole_twisted"
            || opt_key == "hole_to_polyhole_max_edges"
            ) {
            steps.emplace_back(posSlice);
        } else if (opt_key == "nonplanar_support_interface") {
            steps.emplace_back(posSupportMaterial);
            for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id)
                if (Nonplanar::overhangs_enabled(this->printing_region(region_id).config(), m_config)) {
                    steps.emplace_back(posPerimeters);
                    break;
                }
        } else if (opt_key == "enable_support") {
            steps.emplace_back(posSupportMaterial);
            if (m_auto_support)
                steps.emplace_back(posSlice);
            if (m_config.adaptive_layer_height_error.value > 0. || m_config.overhang_layer_contact.value > 0.)
                steps.emplace_back(posSlice);
            bool clip = m_config.nonplanar_mode.value == NonplanarMode::Auto;
            for (size_t region_id = 0; region_id < this->num_printing_regions() && !clip; ++region_id)
                clip = Nonplanar::overhangs_enabled(this->printing_region(region_id).config(), m_config) ||
                       Nonplanar::bridges_enabled(this->printing_region(region_id).config(), m_config);
            if (clip)
                steps.emplace_back(posPerimeters);
            if (m_config.support_top_z_distance == 0.) {
	            steps.emplace_back(posSlice);
	        }
        } else if (
        	   opt_key == "support_type"
            || opt_key == "support_angle"
            || opt_key == "support_on_build_plate_only"
            || opt_key == "support_critical_regions_only"
            || opt_key == "support_remove_small_overhang"
            || opt_key == "enforce_support_layers"
            || opt_key == "support_filament"
            || opt_key == "support_line_width"
            || opt_key == "support_interface_top_layers"
            || opt_key == "support_interface_bottom_layers"
            || opt_key == "support_interface_pattern"
            || opt_key == "support_interface_loop_pattern"
            || opt_key == "support_interface_filament"
            || opt_key == "support_interface_not_for_body"
            || opt_key == "support_interface_spacing"
            || opt_key == "support_bottom_interface_spacing"
            || opt_key == "support_base_pattern"
            || opt_key == "support_style"
            || opt_key == "native_support_base_pad"
            || opt_key == "native_support_base_pad_min_ratio"
            || opt_key == "support_object_xy_distance"
            || opt_key == "support_object_first_layer_gap"
            || opt_key == "support_base_pattern_spacing"
            || opt_key == "support_expansion"
            || opt_key == "independent_support_layer_height"
            || opt_key == "support_threshold_angle"
            || opt_key == "support_threshold_overlap"
            || opt_key == "support_ironing"
            || opt_key == "support_ironing_pattern"
            || opt_key == "support_ironing_flow"
            || opt_key == "support_ironing_spacing"
            || opt_key == "raft_expansion"
            || opt_key == "raft_first_layer_density"
            || opt_key == "raft_first_layer_expansion"
            || opt_key == "bridge_no_support"
            || opt_key == "max_bridge_length"
            || opt_key == "initial_layer_line_width"
            || opt_key == "tree_support_auto_brim"
            || opt_key == "tree_support_brim_width"
            || opt_key == "tree_support_top_rate"
            || opt_key == "tree_support_branch_distance"
            || opt_key == "tree_support_branch_distance_organic"
            || opt_key == "tree_support_tip_diameter"
            || opt_key == "tree_support_branch_diameter"
            || opt_key == "tree_support_branch_diameter_organic"
            || opt_key == "tree_support_branch_diameter_angle"
            || opt_key == "tree_support_branch_angle"
            || opt_key == "tree_support_branch_angle_organic"
            || opt_key == "tree_support_angle_slow"
            || opt_key == "tree_support_wall_count") {
            steps.emplace_back(posSupportMaterial);
            if ((opt_key == "support_style" || opt_key == "support_threshold_angle") &&
                (m_config.adaptive_layer_height_error.value > 0. || m_config.overhang_layer_contact.value > 0.))
                steps.emplace_back(posSlice);
        } else if (
               opt_key == "bottom_shell_layers"
            || opt_key == "top_shell_layers") {

            steps.emplace_back(posSlice);
#if (0)
            const auto *old_shell_layers = old_config.option<ConfigOptionInt>(opt_key);
            const auto *new_shell_layers = new_config.option<ConfigOptionInt>(opt_key);
            assert(old_shell_layers && new_shell_layers);

            bool value_changed = (old_shell_layers->value == 0 && new_shell_layers->value > 0) ||
                                 (old_shell_layers->value > 0 && new_shell_layers->value == 0);

            if (value_changed && this->object_extruders().size() > 1) {
                steps.emplace_back(posSlice);
            }
            else if (m_print->config().spiral_mode && opt_key == "bottom_shell_layers") {
                steps.emplace_back(posSlice);
            }
#endif
        } else if (
               opt_key == "interface_shells"
            || opt_key == "infill_multiline"
            || opt_key == "infill_combination"
            || opt_key == "infill_combination_max_layer_height"
            || opt_key == "bottom_shell_thickness"
            || opt_key == "top_shell_thickness"
            || opt_key == "top_surface_expansion_margin"
            || opt_key == "top_surface_expansion_direction"
            || opt_key == "minimum_sparse_infill_area"
            || opt_key == "sparse_infill_filament_id"
            || opt_key == "internal_solid_filament_id"
            || opt_key == "top_surface_filament_id"
            || opt_key == "bottom_surface_filament_id"
            || opt_key == "sparse_infill_line_width"
            || opt_key == "skin_infill_line_width"
            || opt_key == "skeleton_infill_line_width"
            || opt_key == "infill_direction"
            || opt_key == "solid_infill_direction"
            || opt_key == "top_layer_direction"
            || opt_key == "bottom_layer_direction"
            || opt_key == "align_infill_direction_to_model" 
            || opt_key == "extra_solid_infills"
            || opt_key == "ensure_vertical_shell_thickness"
            || opt_key == "bridge_angle"
            || opt_key == "internal_bridge_angle"
            || opt_key == "relative_bridge_angle"
            || opt_key == "bridge_line_width"
            || opt_key == "bridge_density"
            || opt_key == "internal_bridge_density") {
            steps.emplace_back(posPrepareInfill);
        } else if (
               opt_key == "top_surface_pattern"
            || opt_key == "bottom_surface_pattern"
            || opt_key == "top_surface_fill_order"
            || opt_key == "bottom_surface_fill_order"
            || opt_key == "internal_solid_infill_pattern"
            || opt_key == "external_fill_link_max_length"
            || opt_key == "infill_anchor"
            || opt_key == "infill_anchor_max"
            || opt_key == "top_surface_line_width"
            || opt_key == "bottom_surface_density"
            || opt_key == "initial_layer_line_width"
            || opt_key == "small_area_infill_flow_compensation"
            || opt_key == "lateral_lattice_angle_1"
            || opt_key == "lateral_lattice_angle_2"
            || opt_key == "infill_overhang_angle"
            || opt_key == "top_texture"
            || opt_key == "top_texture_depth"
            || opt_key == "top_texture_pitch") {
            steps.emplace_back(posInfill);
        } else if (opt_key == "sparse_infill_pattern"
                   || opt_key == "tpms_adaptive"
                   || opt_key == "tpms_interior_density"
                   || opt_key == "tpms_adaptive_gradient"
                   || opt_key == "center_of_surface_pattern"
                   || opt_key == "separated_infills"
                   || opt_key == "sparse_infill_smooth_factor"
                   || opt_key == "symmetric_infill_y_axis"
                   || opt_key == "infill_shift_step"
                   || opt_key == "sparse_infill_rotate_template"
                   || opt_key == "solid_infill_rotate_template"
                   || opt_key == "lightning_overhang_angle"
                   || opt_key == "lightning_prune_angle"
                   || opt_key == "lightning_straightening_angle"
                   || opt_key == "skeleton_infill_density"
                   || opt_key == "skin_infill_density"
                   || opt_key == "infill_lock_depth"
                   || opt_key == "skin_infill_depth") {
            steps.emplace_back(posPrepareInfill);
        } else if (opt_key == "sparse_infill_density") {
            const auto *old_density = old_config.option<ConfigOptionPercent>(opt_key);
            const auto *new_density = new_config.option<ConfigOptionPercent>(opt_key);
            assert(old_density && new_density);
            if (is_approx(old_density->value, 0.) || is_approx(old_density->value, 100.) ||
                is_approx(new_density->value, 0.) || is_approx(new_density->value, 100.))
                steps.emplace_back(posPerimeters);
            steps.emplace_back(posPrepareInfill);
        } else if (opt_key == "top_surface_density") {
            const auto *old_density = old_config.option<ConfigOptionPercent>(opt_key);
            const auto *new_density = new_config.option<ConfigOptionPercent>(opt_key);
            assert(old_density && new_density);
            if (is_approx(old_density->value, 0.) || is_approx(new_density->value, 0.))
                steps.emplace_back(posPerimeters);
            steps.emplace_back(posInfill);
        } else if (opt_key == "top_surface_expansion") {
            const auto *old_expansion = old_config.option<ConfigOptionFloat>(opt_key);
            const auto *new_expansion = new_config.option<ConfigOptionFloat>(opt_key);
            assert(old_expansion && new_expansion);
            if (old_expansion->value <= 0. || new_expansion->value <= 0.)
                steps.emplace_back(posPerimeters);
            steps.emplace_back(posPrepareInfill);
        } else if (opt_key == "internal_solid_infill_line_width") {
            steps.emplace_back(posPerimeters);
            steps.emplace_back(posPrepareInfill);
        } else if (
               opt_key == "outer_wall_line_width"
            || opt_key == "outer_wall_filament_id"
            || opt_key == "inner_wall_filament_id"
            || opt_key == "fuzzy_skin"
            || opt_key == "fuzzy_skin_thickness"
            || opt_key == "fuzzy_skin_point_distance"
            || opt_key == "fuzzy_skin_first_layer"
            || opt_key == "fuzzy_skin_mode"
            || opt_key == "fuzzy_skin_noise_type"
            || opt_key == "fuzzy_skin_scale"
            || opt_key == "fuzzy_skin_octaves"
            || opt_key == "fuzzy_skin_persistence"
            || opt_key == "detect_overhang_wall"
            || opt_key == "unsupported_wall_last"
            || opt_key == "overhang_reverse"
            || opt_key == "overhang_reverse_internal_only"
            || opt_key == "overhang_reverse_threshold"
            || opt_key == "wall_direction"
            || opt_key == "enable_overhang_speed"
            || opt_key == "detect_thin_wall"
            || opt_key == "precise_outer_wall") {
            steps.emplace_back(posPerimeters);
            steps.emplace_back(posSupportMaterial);
        } else if (
               opt_key == "nonplanar_top_shells"
            || opt_key == "nonplanar_bottom_shells"
            || opt_key == "nonplanar_shell_layer_height"
            || opt_key == "nonplanar_max_slope"
            || opt_key == "nonplanar_max_surface_error"
            || opt_key == "nonplanar_min_patch_area"
            || opt_key == "nonplanar_top_pattern"
            || opt_key == "nonplanar_top_order"
            || opt_key == "nonplanar_surface_spacing"
            || opt_key == "nonplanar_spiral_top"
            || opt_key == "nonplanar_sublayer_walls"
            || opt_key == "nonplanar_flow_mod"
            || opt_key == "nonplanar_adaptive_bands"
            || opt_key == "nonplanar_infill_depth"
            || opt_key == "nonplanar_strength_roads"
            || opt_key == "nonplanar_ironing"
            || opt_key == "nonplanar_overhangs"
            || opt_key == "nonplanar_bridges"
            || opt_key == "nonplanar_fallback"
            || opt_key == "nonplanar_full_conformal_max_thickness"
            || opt_key == "nonplanar_surface_resolution"
            || opt_key == "nonplanar_max_patch_height") {
            steps.emplace_back(posPerimeters);
            steps.emplace_back(posSupportMaterial);
        } else if (opt_key == "bridge_flow" || opt_key == "internal_bridge_flow") {
            steps.emplace_back(posPerimeters);
            steps.emplace_back(posInfill);
            steps.emplace_back(posSupportMaterial);
        } else if (
               opt_key == "nonplanar_curved_top_layers"
            || opt_key == "nonplanar_continuous_top"
            || opt_key == "nonplanar_curved_max_angle"
            || opt_key == "nonplanar_curved_min_area"
            || opt_key == "nonplanar_top_direction"
            || opt_key == "nonplanar_morph_layers"
            || opt_key == "nonplanar_conformal_rounding"
            || opt_key == "nonplanar_conformal_fallback_angle"
            || opt_key == "nonplanar_conformal_ramp_layers") {
            steps.emplace_back(posSlice);
        } else if (
                opt_key == "wall_generator"
            || opt_key == "wall_transition_length"
            || opt_key == "wall_transition_filter_deviation"
            || opt_key == "wall_transition_angle"
            || opt_key == "wall_distribution_count"
            || opt_key == "wall_maximum_resolution"
            || opt_key == "wall_maximum_deviation"
            || opt_key == "min_feature_size"
            || opt_key == "min_length_factor"
            || opt_key == "min_bead_width") {
            steps.emplace_back(posSlice);
        } else if (gcode_only_options().count(opt_key) != 0
            || opt_key == "seam_position" || opt_key == "bridge_speed") {
            invalidated |= m_print->invalidate_step(psGCodeExport);
        } else if (
               opt_key == "flush_into_infill"
            || opt_key == "flush_into_objects"
            || opt_key == "flush_into_support"
            || opt_key == "belt_purge_tower_object") {
            invalidated |= m_print->invalidate_step(psWipeTower);
            invalidated |= m_print->invalidate_step(psGCodeExport);
        } else {
            this->invalidate_all_steps();
            invalidated = true;
        }
    }

    if (m_config.nonplanar_mode.value == NonplanarMode::Auto &&
        std::any_of(steps.begin(), steps.end(), [](PrintObjectStep step) { return step > posSlice && step <= posNonplanar; }))
        steps.emplace_back(posSlice);
    sort_remove_duplicates(steps);
    if (!steps.empty() && steps.front() <= posNonplanar) {
        m_nonplanar_repair.reset();
        m_oneshot.reset();
    }
    for (PrintObjectStep step : steps)
        invalidated |= this->invalidate_step(step);
    return invalidated;
}

bool PrintObject::invalidate_step(PrintObjectStep step)
{
    if (curved_tops_mode(m_config.nonplanar_mode.value) &&
        step >= posPrepareInfill && step <= posNonplanar && is_step_done(posNonplanarProjection))
        step = posPerimeters;

    if (step == posSlice || step == posPrepareInfill || step == posNonplanar)
        m_nonplanar_region_cache->clear();
    if ((step == posSlice || step == posPerimeters || step == posPrepareInfill || step == posInfill || step == posIroning) &&
        m_nonplanar_repair)
        m_nonplanar_repair->curved_scaffold.reset();
	bool invalidated = Inherited::invalidate_step(step);

    if (step == posPerimeters) {
		invalidated |= this->invalidate_steps({ posNonplanarProjection, posPrepareInfill, posInfill, posIroning, posNonplanar, posContouring, posSimplifyPath, posSimplifyInfill });
        invalidated |= m_print->invalidate_steps({ psSkirtBrim });
    } else if (step == posPrepareInfill) {
        invalidated |= this->invalidate_steps({ posNonplanarProjection, posInfill, posIroning, posNonplanar, posContouring, posSimplifyPath, posSimplifyInfill });
    } else if (step == posInfill) {
        invalidated |= this->invalidate_steps({ posNonplanarProjection, posIroning, posNonplanar, posContouring, posSimplifyInfill });
        invalidated |= m_print->invalidate_steps({ psSkirtBrim });
    } else if (step == posIroning) {
        invalidated |= this->invalidate_steps({ posNonplanar, posContouring });
    } else if (step == posNonplanar) {
        invalidated |= this->invalidate_steps({ posContouring });
    } else if (step == posSlice) {
		invalidated |= this->invalidate_steps({ posNonplanarProjection, posPerimeters, posPrepareInfill, posInfill, posIroning, posNonplanar, posContouring, posSupportMaterial, posSimplifyPath, posSimplifyInfill, posSimplifySupportPath, posDetectOverhangsForLift });
        invalidated |= m_print->invalidate_steps({ psSkirtBrim });
        m_slicing_params.valid = false;
        m_belt_floor_z_shift_cache_valid = false;
    } else if (step == posSupportMaterial) {
        invalidated |= this->invalidate_steps({ posSimplifySupportPath });
        if (this->has_conformal_support() && (this->is_step_done_unguarded(posPerimeters) || this->is_step_started_unguarded(posPerimeters)))
            invalidated |= this->invalidate_step(posPerimeters);
        invalidated |= m_print->invalidate_steps({ psSkirtBrim });
        m_slicing_params.valid = false;
    }

    if (step >= posPerimeters && step <= posNonplanar && (this->has_conformal_support() || Nonplanar::support_free(m_config)))
        invalidated |= this->invalidate_step(posSupportMaterial);
    else if (step >= posPerimeters && step <= posNonplanar)
        for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id)
            if (Nonplanar::bottom_shells_enabled(this->printing_region(region_id).config(), this->config()) ||
                Nonplanar::full_conformal_thickness(this->printing_region(region_id).config(), *this) > 0.) {
                invalidated |= this->invalidate_step(posSupportMaterial);
                break;
            }

    invalidated |= m_print->invalidate_step(psWipeTower);
    invalidated |= m_print->invalidate_step(psGCodeExport);
    return invalidated;
}

bool PrintObject::invalidate_all_steps()
{
    m_nonplanar_region_cache->clear();
    const bool inherited_invalidated = Inherited::invalidate_all_steps();
    const bool print_invalidated     = m_print->invalidate_all_steps();
    bool result = inherited_invalidated || print_invalidated;
	m_slicing_params.valid = false;
	m_belt_floor_z_shift_cache_valid = false;
    m_nonplanar_repair.reset();
    m_oneshot.reset();
	return result;
}

void PrintObject::detect_surfaces_type()
{
    BOOST_LOG_TRIVIAL(info) << "Detecting solid surfaces..." << log_memory_info();

    bool spiral_mode      = this->print()->config().spiral_mode.value && !Nonplanar::vase_with_conformal_roof(*this);
    bool interface_shells = ! spiral_mode && m_config.interface_shells.value;
    size_t num_layers     = spiral_mode ? std::min(size_t(this->printing_region(0).config().bottom_shell_layers), m_layers.size()) : m_layers.size();

    for (Layer *l : m_layers) l->wave_overhang_shadow_polygons.clear();

    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        BOOST_LOG_TRIVIAL(debug) << "Detecting solid surfaces for region " << region_id << " in parallel - start";
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        for (Layer *layer : m_layers)
            layer->m_regions[region_id]->export_region_fill_surfaces_to_svg_debug("1_detect_surfaces_type-initial");
#endif
        std::vector<Surfaces> surfaces_new;
        if (interface_shells)
            surfaces_new.assign(num_layers, Surfaces());

        tbb::parallel_for(
            tbb::blocked_range<size_t>(0,
            	spiral_mode ?
            		((num_layers > 1) ? num_layers - 1 : num_layers) :
            		m_layers.size()),
            [this, region_id, interface_shells, &surfaces_new](const tbb::blocked_range<size_t>& range) {
                bool bottom_is_fully_supported = this->has_support() && m_config.support_top_z_distance.value == 0 && is_auto(m_config.support_type.value);
                if (m_config.support_type.value == stNormalAuto)
                    bottom_is_fully_supported &= !m_config.bridge_no_support.value;
                else if (m_config.support_type.value == stTreeAuto) {
                    bottom_is_fully_supported &= (m_config.support_interface_top_layers.value > 0 && m_config.max_bridge_length.value == 0 && m_config.support_critical_regions_only.value==false);
                }
                SurfaceType surface_type_bottom_other = bottom_is_fully_supported ? stBottom : stBottomBridge;
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    m_print->throw_if_canceled();
                    Layer       *layer  = m_layers[idx_layer];
                    LayerRegion *layerm = layer->m_regions[region_id];
                    Layer       *upper_layer = (idx_layer + 1 < this->layer_count()) ? m_layers[idx_layer + 1] : nullptr;
                    Layer       *lower_layer = (idx_layer > 0) ? m_layers[idx_layer - 1] : nullptr;
                    const float offset = layerm->flow(frExternalPerimeter).scaled_width() / 10.f;

                    ExPolygons     layerm_slices_surfaces = to_expolygons(layerm->slices.surfaces);
                    if (layerm->region().config().counterbore_hole_bridging.value == chbFilled) {
                        layerm_slices_surfaces = union_ex(layerm_slices_surfaces, to_expolygons(layerm->fill_surfaces.surfaces));
                    }

                    Surfaces nonplanar_surfaces;
                    for (const Surface &surface : layerm->slices.surfaces)
                        if (surface.is_nonplanar()) {
                            if (surface.source_surface_id < 0 || surface.distance_to_top < 0)
                                throw_nonplanar_internal(nonplanar_location(layer->print_z, layer->id()),
                                    "shell without target surface or shell offset", this->id().id);
                            nonplanar_surfaces.push_back(surface);
                        }
                    ExPolygons planar_surfaces = nonplanar_surfaces.empty() ? layerm_slices_surfaces :
                        diff_ex(layerm_slices_surfaces, nonplanar_surfaces, ApplySafetyOffset::Yes);

                    Surfaces top;
                    if (upper_layer) {
                        ExPolygons upper_slices = interface_shells ?
                            diff_ex(layerm_slices_surfaces, upper_layer->m_regions[region_id]->slices.surfaces, ApplySafetyOffset::Yes) :
                            diff_ex(layerm_slices_surfaces, upper_layer->lslices, ApplySafetyOffset::Yes);
                        if (nonplanar_surfaces.empty())
                            surfaces_append(top, opening_ex(upper_slices, offset), stTop);
                        else
                            surfaces_append(top, diff_ex(opening_ex(upper_slices, offset), nonplanar_surfaces, ApplySafetyOffset::Yes), stTop);
                    } else if (! nonplanar_surfaces.empty()) {
                        surfaces_append(top, union_ex(planar_surfaces), stTop);
                    } else {
                        top = layerm->slices.surfaces;
                        for (Surface &surface : top)
                            surface.surface_type = stTop;
                    }

                    Surfaces bottom;
                    if (lower_layer) {
#if 0
                        Polygons lower_slices = interface_shells ?
                            to_polygons(lower_layer->get_region(region_id)->slices.surfaces) :
                            to_polygons(lower_layer->slices);
                        surfaces_append(bottom,
                            opening_ex(diff(layerm_slices_surfaces, lower_slices, true), offset),
                            surface_type_bottom_other);
#else
                        surfaces_append(
                            bottom,
                            opening_ex(
                                nonplanar_surfaces.empty() ?
                                    diff_ex(layerm_slices_surfaces, lower_layer->lslices, ApplySafetyOffset::Yes) :
                                    diff_ex(diff_ex(layerm_slices_surfaces, lower_layer->lslices, ApplySafetyOffset::Yes),
                                            nonplanar_surfaces, ApplySafetyOffset::Yes),
                                offset),
                            surface_type_bottom_other);
                        if (interface_shells) {
                            surfaces_append(
                                bottom,
                                opening_ex(
                                    diff_ex(
                                        diff_ex(
                                            intersection(layerm_slices_surfaces, lower_layer->lslices),
                                            lower_layer->m_regions[region_id]->slices.surfaces,
                                            ApplySafetyOffset::Yes),
                                        nonplanar_surfaces,
                                        ApplySafetyOffset::Yes),
                                    offset),
                                stBottom);
                        }
#endif
                    } else {
                        bottom = layerm->slices.surfaces;
                        for (Surface &surface : bottom)
                            surface.surface_type = stBottom;
                    }

                    if (! top.empty() && ! bottom.empty()) {
                        const auto cracks = intersection_ex(top, bottom);
                        if (!cracks.empty()) {
                            if (lower_layer) {
                                const float small_crack_threshold = -layerm->flow(frExternalPerimeter).scaled_width() * 1.5;
                                
                                for (const auto& crack : cracks) {
                                    if (offset_ex(crack, small_crack_threshold).empty()) {
                                        if (std::any_of(bottom.begin(), bottom.end(), [&crack, small_crack_threshold](const Surface& s) {
                                                const auto& se = s.expolygon;
                                                return diff_ex(crack, se, ApplySafetyOffset::Yes).empty()
                                                    && se.area() > crack.area() * 2
                                                    && !offset_ex(diff_ex(se, crack), small_crack_threshold).empty();
                                        })) continue;

                                        Surfaces bot_tmp;
                                        for (auto& b : bottom) {
                                            surfaces_append(bot_tmp, diff_ex(b.expolygon, offset_ex(crack, -small_crack_threshold)), b.surface_type);
                                        }
                                        bottom = std::move(bot_tmp);
                                    }
                                }
                            }

                            Polygons top_polygons = to_polygons(std::move(top));
                            top.clear();
                            surfaces_append(top, diff_ex(top_polygons, bottom), stTop);
                        }
                    }

                    const PrintRegionConfig &region_config  = layerm->region().config();
                    const double             top_expansion  = region_config.top_surface_expansion.value;
                    if (top_expansion > 0. && region_config.top_shell_layers.value > 0 &&
                        region_config.top_surface_density.value > 0. && ! top.empty()) {
                        const double     d = scale_(top_expansion);
                        const ExPolygons T = union_ex(to_expolygons(top));
                        const int    wall_loops = region_config.only_one_wall_top.value ? std::min(region_config.wall_loops.value, 1)
                                                                                        : region_config.wall_loops.value;
                        const double wall_band  = wall_loops <= 0 ? 0. :
                            double(layerm->flow(frExternalPerimeter).scaled_width()) +
                            double(layerm->flow(frPerimeter).scaled_spacing()) * double(wall_loops - 1);
                        const double margin     = scale_(region_config.top_surface_expansion_margin.value);
                        const float  min_top    = float(layerm->flow(frTopSolidInfill).scaled_width());
                        const auto   direction  = region_config.top_surface_expansion_direction.value;

                        ExPolygons grown;
                        for (const ExPolygon &island : union_ex(layerm_slices_surfaces)) {
                            const ExPolygons infill_region = wall_band > 0. ? offset_ex(island, -float(wall_band)) : ExPolygons{ island };
                            const ExPolygons island_top    = intersection_ex(
                                ClipperUtils::clip_clipper_polygons_with_subject_bbox(T, get_extents(island).inflated(SCALED_EPSILON)),
                                infill_region);
                            if (opening_ex(island_top, min_top).empty())
                                continue;
                            ExPolygons expanded = offset_ex(island_top, float(d), jtMiter, 2.);
                            if (direction != TopSurfaceExpansionDirection::InwardAndOutward) {
                                ExPolygons outline;
                                outline.reserve(island_top.size());
                                for (const ExPolygon &ex : island_top)
                                    outline.emplace_back(ex.contour);
                                outline = union_ex(outline);
                                expanded = direction == TopSurfaceExpansionDirection::Inward ?
                                    intersection_ex(expanded, outline) :
                                    diff_ex(expanded, diff_ex(outline, island_top));
                            }
                            const ExPolygons allowed = margin > 0. ? offset_ex(infill_region, -float(margin)) : infill_region;
                            append(grown, intersection_ex(expanded, allowed));
                        }

                        ExPolygons new_top = diff_ex(union_ex(T, grown), to_expolygons(bottom));
                        top.clear();
                        surfaces_append(top, std::move(new_top), stTop);
                    }

        #ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        static int iRun = 0;
                        std::vector<std::pair<Slic3r::ExPolygons, SVG::ExPolygonAttributes>> expolygons_with_attributes;
                        expolygons_with_attributes.emplace_back(std::make_pair(union_ex(top),                           SVG::ExPolygonAttributes("green")));
                        expolygons_with_attributes.emplace_back(std::make_pair(union_ex(bottom),                        SVG::ExPolygonAttributes("brown")));
                        expolygons_with_attributes.emplace_back(std::make_pair(to_expolygons(layerm->slices.surfaces),  SVG::ExPolygonAttributes("black")));
                        SVG::export_expolygons(debug_out_path("1_detect_surfaces_type_%d_region%d-layer_%f.svg", iRun ++, region_id, layer->print_z).c_str(), expolygons_with_attributes);
                    }
        #endif
                    Surfaces &surfaces_out = interface_shells ? surfaces_new[idx_layer] : layerm->slices.surfaces;
                    Surfaces  surfaces_backup;
                    if (! interface_shells) {
                        surfaces_backup = std::move(surfaces_out);
                        surfaces_out.clear();
                    }
                    const ExPolygons& surfaces_prev_expolys = interface_shells ? layerm_slices_surfaces : to_expolygons(surfaces_backup);

                    {
                        Polygons topbottom = to_polygons(top);
                        polygons_append(topbottom, to_polygons(bottom));
                        polygons_append(topbottom, to_polygons(nonplanar_surfaces));
                        surfaces_append(surfaces_out, diff_ex(surfaces_prev_expolys, topbottom), stInternal);
                    }

                    surfaces_append(surfaces_out, std::move(top));
                    surfaces_append(surfaces_out, std::move(bottom));
                    surfaces_append(surfaces_out, std::move(nonplanar_surfaces));

        #ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    layerm->export_region_slices_to_svg_debug("detect_surfaces_type-final");
        #endif
                }
            }
        );
        m_print->throw_if_canceled();

        if (interface_shells) {
            for (size_t idx_layer = 0; idx_layer < num_layers; ++ idx_layer)
                m_layers[idx_layer]->m_regions[region_id]->slices.surfaces = std::move(surfaces_new[idx_layer]);
        }

        if (spiral_mode) {
        	if (num_layers > 1)
	        	m_layers[num_layers - 1]->m_regions[region_id]->slices.set_type(stTop);
	        for (size_t i = num_layers; i < m_layers.size(); ++ i)
	        	m_layers[i]->m_regions[region_id]->slices.set_type(stInternal);
        }
        
        if( (this->config().enable_extra_bridge_layer.value == eblApplyToAll) || (this->config().enable_extra_bridge_layer.value == eblExternalBridgeOnly)){
            const size_t last = (m_layers.empty() ? 0 : m_layers.size() - 1);

            std::vector<Polygons> bridge_polys_per_layer(last);
            tbb::parallel_for(tbb::blocked_range<size_t>(0, last), [this, region_id, &bridge_polys_per_layer](const tbb::blocked_range<size_t> &range) {
                for (size_t i = range.begin(); i < range.end(); ++i) {
                    m_print->throw_if_canceled();
                    const Surfaces &bot_surfs = m_layers[i]->m_regions[region_id]->slices.surfaces;
                    for (const Surface &sbot : bot_surfs) {
                        if (sbot.surface_type == stBottomBridge) {
                            polygons_append(bridge_polys_per_layer[i], to_polygons(sbot));
                        }
                    }
                }
            });

            tbb::parallel_for( tbb::blocked_range<size_t>(0, last), [this, region_id, &bridge_polys_per_layer](const tbb::blocked_range<size_t> &range) {
                for (size_t i = range.begin(); i < range.end(); ++i) {
                    m_print->throw_if_canceled();

                    const Polygons &polygons_bridge = bridge_polys_per_layer[i];

                    if (polygons_bridge.empty())
                        continue;
                    
                    Surfaces &top_surfs = m_layers[i + 1]->m_regions[region_id]->slices.surfaces;
                    Surfaces new_surfaces;
                    new_surfaces.reserve(top_surfs.size());
                    
                    LayerRegion *layerm = m_layers[i]->m_regions[region_id];
                    int number_of_internal_walls = std::max(0, layerm->m_region->config().wall_loops - 1);
                    float        offset_distance = layerm->flow(frExternalPerimeter).scaled_width()
                                                    + ((layerm->flow(frPerimeter).scaled_width()) * number_of_internal_walls);
                    ExPolygons same_layer_top_expanded;
                    {
                        ExPolygons same_layer_top;
                        for (const Surface &s : top_surfs) {
                            if (s.surface_type == stTop)
                                same_layer_top.push_back(s.expolygon);
                        }
                        if (! same_layer_top.empty())
                            same_layer_top_expanded = offset_ex(same_layer_top, offset_distance);
                    }

                    for (Surface &s_up : top_surfs) {
                        if (s_up.surface_type != stInternal) {
                            new_surfaces.push_back(std::move(s_up));
                            continue;
                        }
                        Polygons p_up = to_polygons(s_up);
                        ExPolygons overlap   = intersection_ex(p_up, polygons_bridge , ApplySafetyOffset::Yes);
                        overlap = offset_ex(shrink_ex(overlap, offset_distance), offset_distance);

                        if (! same_layer_top_expanded.empty() && ! overlap.empty())
                            overlap = diff_ex(overlap, same_layer_top_expanded, ApplySafetyOffset::Yes);

                        ExPolygons remainder = diff_ex(p_up, overlap, ApplySafetyOffset::Yes);
                        
                        ExPolygons unified_remainder = union_safety_offset_ex(remainder);
                        for (auto &ex_remainder : unified_remainder) {
                            Surface s(stInternal, ex_remainder);
                            new_surfaces.push_back(std::move(s));
                        }
                        ExPolygons unified_overlap = union_safety_offset_ex(overlap);
                        for (auto &ex_overlap : unified_overlap) {
                            Surface s(stInternalAfterExternalBridge, ex_overlap);
                            new_surfaces.push_back(std::move(s));
                        }
                    }
                    top_surfs = std::move(new_surfaces);
                }
            }
            );
            for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id) {
                tbb::parallel_for( tbb::blocked_range<size_t>(0, m_layers.size()), [this, region_id](const tbb::blocked_range<size_t> &range) {
                    for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++idx_layer) {
                        Surfaces &surfs = m_layers[idx_layer]->m_regions[region_id]->slices.surfaces;
                        for (Surface &s : surfs) {
                            if (s.surface_type == stInternalAfterExternalBridge) {
                                s.surface_type = stBottomBridge;
                            }
                        }
                    }
                }
              );
            }
        }
        {
            const PrintRegionConfig &rconf = this->printing_region(region_id).config();
            const int floor_layers        = rconf.wave_overhang_floor_layers.value;
            const int bottom_shell_layers = rconf.bottom_shell_layers.value;
            const int top_shell_layers    = rconf.top_shell_layers.value;
            const int shadow_up   = std::max(floor_layers, bottom_shell_layers);
            const int shadow_down = top_shell_layers;
            if (WaveOverhangs::enabled(rconf, *this)) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, m_layers.size()),
                    [this, shadow_up, shadow_down](const tbb::blocked_range<size_t> &range) {
                        for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                            m_print->throw_if_canceled();
                            Layer *layer = m_layers[idx_layer];

                            Polygons shadow = std::move(layer->wave_overhang_shadow_polygons);
                            append(shadow, layer->wave_overhang_floor_polygons);
                            for (int k = 1; k <= shadow_up; ++k) {
                                if ((int)idx_layer - k < 0) break;
                                append(shadow, m_layers[idx_layer - k]->wave_overhang_floor_polygons);
                            }
                            for (int k = 1; k <= shadow_down; ++k) {
                                if (idx_layer + k >= m_layers.size()) break;
                                append(shadow, m_layers[idx_layer + k]->wave_overhang_floor_polygons);
                            }
                            if (!shadow.empty())
                                shadow = union_(shadow);
                            layer->wave_overhang_shadow_polygons = std::move(shadow);
                        }
                    });
                m_print->throw_if_canceled();
            }
        }

        BOOST_LOG_TRIVIAL(debug) << "Detecting solid surfaces for region " << region_id << " - clipping in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this, region_id](const tbb::blocked_range<size_t>& range) {
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    m_print->throw_if_canceled();
                    LayerRegion *layerm = m_layers[idx_layer]->m_regions[region_id];
                    layerm->slices_to_fill_surfaces_clipped();
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    layerm->export_region_fill_surfaces_to_svg_debug("1_detect_surfaces_type-final");
#endif
                }
            });
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Detecting solid surfaces for region " << region_id << " - clipping in parallel - end";
    }
    m_typed_slices = true;
}

bool
PrintObject::check_nonplanar_collisions(NonplanarSurface &surface)
{
    Polygons nonplanar_polygon = to_polygons(surface.horizontal_projection());
	for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        Polygons collider;
        for (size_t idx_layer = 0; idx_layer < m_layers.size(); ++ idx_layer) {
            m_print->throw_if_canceled();
            Layer       *layer  = m_layers[idx_layer];
            LayerRegion *layerm = layer->m_regions[region_id];

            if (surface.stats.min.z > layer->slice_z + layer->height) continue;
            if (surface.stats.max.z < layer->slice_z) break;

            BOOST_LOG_TRIVIAL(trace) << "Check nonplanar collisions for region " << region_id << " and layer " << layer->print_z;

            ExPolygons collisions = union_ex(intersection(layerm->slices.surfaces, diff(collider, nonplanar_polygon)));

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                {
                    static int iRun = 0;
                    SVG svg(debug_out_path("Layer-%u-collider-%u_collisions_layer%.2f.svg", layer->id(), iRun++, layer->print_z), get_extents(layerm->slices.surfaces));
                    svg.draw(layerm->slices.surfaces, "blue");
                    svg.draw(collisions, "red", 0.7f);
                    svg.arrows = false;
                    svg.Close();
                }
#endif

            if (!collisions.empty()) {
                double area = 0;
                for (auto& c : collisions){
                    area += c.area();
                }

                if (1.0 < unscale<double>(unscale<double>(area))) {
                    BOOST_LOG_TRIVIAL(trace) << "Surface removed: collision on layer " << layer->print_z << "mm (" << unscale<double>(unscale<double>(area)) << " mm²)";

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                {
                    static int iRun = 0;
                    SVG svg(debug_out_path("Layer-%u-collider-%u_removed_layer%.2f.svg", layer->id(), iRun++, layer->print_z), get_extents(layerm->slices.surfaces));
                    svg.draw(layerm->slices.surfaces, "blue");
                    svg.draw(union_ex(diff(collider,nonplanar_polygon)), "red", 0.7f);
                    svg.draw_outline(collider);
                    svg.arrows = false;
                    svg.Close();
                }
#endif

                    return true;
                }
            }

            if (layer->upper_layer != NULL) {
                Layer* upper_layer = layer->upper_layer;
                LayerRegion *upper_layerm = upper_layer->m_regions[region_id];
                collider = union_(
                                intersection(
                                    diff_ex(layerm->slices.surfaces, upper_layerm->slices.surfaces, ApplySafetyOffset::No),
                                    nonplanar_polygon,
                                    ApplySafetyOffset::No),
                                collider);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                {
                    static int iRun = 0;
                    SVG svg(debug_out_path("Layer-%u-collider-%u_layer%.2f.svg", layer->id(), iRun++, layer->print_z), get_extents(layerm->slices.surfaces));
                    svg.draw_outline(collider, "black", scale_(0.1f));
                    svg.Close();
                }
#endif
            }
        }
    }

    return false;
}

double PrintObject::curved_footprint() const
{
    double total = 0.;
    for (const auto &surface : m_nonplanar_surfaces)
        for (const ExPolygon &polygon : surface.horizontal_projection())
            total += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
    return total;
}

void
PrintObject::detect_nonplanar_surfaces()
{
    if(!curved_tops_mode(m_config.nonplanar_mode.value)) return;

    bool moved_surfaces = false;

    for (Layer *layer : m_layers)
        for (LayerRegion *layerm : layer->m_regions)
            layerm->clear_nonplanar_surfaces();

    for (Layer *layer : m_layers) {
        layer->region_slices_before_nonplanar.clear();
        layer->lslices_before_nonplanar.clear();
        if (layer->id() == 0)
            continue;
        Polygons merged;
        for (LayerRegion *layerm : layer->m_regions) {
            layer->region_slices_before_nonplanar.emplace_back(to_expolygons(layerm->slices.surfaces));
            polygons_append(merged, to_polygons(layerm->slices.surfaces));
        }
        layer->lslices_before_nonplanar = union_safety_offset_ex(merged);
    }

    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(trace) << "detect_nonplanar_surfaces for region " << region_id;
        const PrintRegion &region = this->printing_region(region_id);

        for (const auto& nonplanar_surface: this->nonplanar_surfaces()) {
            ExPolygons raw_projection = nonplanar_surface.horizontal_projection();
            if (const auto *repair = nonplanar_repair(); repair != nullptr) {
                Polygons struck;
                for (const auto &disc : repair->planar) {
                    if (std::isfinite(disc.z) && (disc.z + disc.radius < nonplanar_surface.stats.min.z ||
                        disc.z - disc.radius > nonplanar_surface.stats.max.z))
                        continue;
                    Polygon circle = make_circle(scale_(disc.radius), scale_(0.01));
                    circle.translate(Point::new_scale(disc.p));
                    struck.emplace_back(std::move(circle));
                }
                raw_projection = diff_ex(raw_projection, struck, ApplySafetyOffset::No);
            }
            const float boundary_radius = 2.f * region.flow(*this, frExternalPerimeter,
                m_config.layer_height.value, false).scaled_width();
            const auto *hints = nonplanar_repair();
            const bool native_mesh = curved_tops_mode(m_config.nonplanar_mode.value) &&
                (curved_tops_mode(m_config.nonplanar_mode.value) ||
                 (hints != nullptr && hints->curved_finish == Nonplanar::RepairHints::CurvedFinish::NativeMesh));
            ExPolygons projection = native_mesh ? raw_projection : intersection_ex(raw_projection,
                opening_ex(raw_projection, boundary_radius, jtRound, scale_(0.01)), ApplySafetyOffset::No);
            if (curved_tops_mode(m_config.nonplanar_mode.value) && !native_mesh)
                projection = Nonplanar::smooth_curved_top_footprint(projection, unscale<double>(boundary_radius));
            float distance_to_top = 0.0f;
            const int curved_shells = curved_top_layers(*this);
            size_t previous_home = std::numeric_limits<size_t>::max();
            for (int shell_thickness = 0; curved_shells > shell_thickness; ++shell_thickness){
                for (LayerPtrs::reverse_iterator home_layer_it = this->m_layers.rbegin(); home_layer_it != this->m_layers.rend(); ++home_layer_it){
                    Layer* home_layer        = *home_layer_it;
                    LayerRegion &home_layerm = *home_layer->m_regions[region_id];
                    if (home_layer->slice_z > nonplanar_surface.stats.max.z - distance_to_top || home_layer->id() >= previous_home) continue;
                    previous_home = home_layer->id();

                    for (LayerPtrs::iterator layer_it = this->m_layers.begin(); layer_it != this->m_layers.end(); ++layer_it){
                        Layer* layer        = *layer_it;
                        LayerRegion &layerm = *layer->m_regions[region_id];

                        if (nonplanar_surface.stats.min.z-layer->height-distance_to_top > layer->slice_z) continue;
                        if (home_layer->slice_z < layer->slice_z) break;
                        if (layer->lower_layer == NULL) continue;

                        BOOST_LOG_TRIVIAL(trace) << "detect_nonplanar_surfaces for region " << region_id << " and layer " << layer->print_z;

                        Surfaces layerm_slices_surfaces = layerm.slices.surfaces;
                        SurfaceCollection topNonplanar;
                        if (layer->upper_layer != NULL) {
                            Layer* upper_layer = layer->upper_layer;
                            LayerRegion &upper_layerm = *upper_layer->m_regions[region_id];
                            Surfaces upper_surfaces = upper_layerm.slices.surfaces;
                            topNonplanar.append(
                                intersection_ex(
                                    projection,
                                    union_ex(
                                        diff_ex(
                                            layerm_slices_surfaces,
                                            upper_surfaces,
                                            ApplySafetyOffset::No)),
                                    ApplySafetyOffset::No),
                                (shell_thickness == 0 ? stTopNonplanar : stInternalSolidNonplanar),
                                distance_to_top
                            );

                            SurfaceCollection upper_nonplanar;
                            for (const Surface &s : upper_surfaces){
                                if (s.is_nonplanar() && s.source_surface_id == nonplanar_surface.stable_id &&
                                    s.distance_to_top < distance_to_top) {
                                    upper_nonplanar.surfaces.push_back(s);
                                }
                            }
                            if (upper_nonplanar.size() > 0)
                                topNonplanar.append(
                                    intersection_ex(
                                        projection,
                                        to_expolygons(upper_nonplanar.surfaces),
                                        ApplySafetyOffset::No),
                                    (shell_thickness == 0 ? stTopNonplanar : stInternalSolidNonplanar),
                                    distance_to_top
                                );
                        }
                        else {
                            topNonplanar.append(
                                intersection_ex(
                                    projection,
                                    union_ex(to_expolygons(layerm_slices_surfaces)),
                                    ApplySafetyOffset::No),
                                (shell_thickness == 0 ? stTopNonplanar : stInternalSolidNonplanar),
                                distance_to_top
                            );
                        }

                        if (!topNonplanar.surfaces.empty()) {
                            Surfaces painted;
                            for (const Surface &piece : topNonplanar.surfaces)
                                surfaces_append(painted, Nonplanar::nonplanar_paint_mask(*this, layer->id(),
                                    ExPolygons{piece.expolygon}), piece);
                            topNonplanar.surfaces = std::move(painted);
                        }

                        if (topNonplanar.size() > 0) {
                            Polygons owned;
                            for (const Surface &s : layerm_slices_surfaces)
                                if (s.is_nonplanar())
                                    polygons_append(owned, to_polygons(s.expolygon));
                            if (! owned.empty()) {
                                owned = union_(owned);
                                const BoundingBox owned_box = get_extents(owned);
                                Surfaces unclaimed;
                                double overlap = 0;
                                for (const Surface &piece : topNonplanar.surfaces) {
                                    const BoundingBox piece_box = get_extents(piece.expolygon);
                                    if (! owned_box.overlap(piece_box)) {
                                        unclaimed.push_back(piece);
                                        continue;
                                    }
                                    ExPolygons rest = diff_ex(piece.expolygon,
                                        ClipperUtils::clip_clipper_polygons_with_subject_bbox(owned, piece_box.inflated(SCALED_EPSILON)),
                                        ApplySafetyOffset::No);
                                    overlap += std::max(0., piece.expolygon.area() - area(rest));
                                    surfaces_append(unclaimed, std::move(rest), piece);
                                }
                                if (overlap > 0)
                                    BOOST_LOG_TRIVIAL(warning) << "Nonplanar surface " << nonplanar_surface.stable_id
                                        << " shell offset " << distance_to_top << " overlaps an existing shell in layer "
                                        << layer->id() << " by " << overlap * SCALING_FACTOR * SCALING_FACTOR << " mm2; kept the first owner";
                                topNonplanar.surfaces = std::move(unclaimed);
                            }
                        }

                        if (topNonplanar.size() > 0) {
                            for (Surface &source_piece : topNonplanar.surfaces) {
                                source_piece.source_layer_id = layer->id();
                                source_piece.source_surface_id = nonplanar_surface.stable_id;
                            }

                            BOOST_LOG_TRIVIAL(trace) << "Removing " << topNonplanar.size() << " nonplanar surfaces from layer " << layer->print_z;

                            layerm.remove_nonplanar_slices(topNonplanar);

                            BOOST_LOG_TRIVIAL(trace) << "Adding " << topNonplanar.size() << " nonplanar surfaces to layer " << home_layer->print_z;

                            home_layerm.append_top_nonplanar_slices(topNonplanar);

                            home_layerm.append_nonplanar_surface(nonplanar_surface);

                            moved_surfaces = true;
                        }
                    }

                    distance_to_top += home_layer->height;
                    break;
                }
            }
        }
    }

    if (! moved_surfaces)
        for (Layer *layer : m_layers) {
            layer->region_slices_before_nonplanar.clear();
            layer->lslices_before_nonplanar.clear();
        }

    if(moved_surfaces) {
        for (Layer *layer : m_layers)
            if (layer->id() > 0) {
                for (LayerRegion *layerm : layer->m_regions)
                    layerm->merge_nonplanar_shell_slices();
                layer->make_slices();
            }
        for (Layer *layer : m_layers) {
            layer->lslices_bboxes.clear();
            for (const ExPolygon &expoly : layer->lslices)
                layer->lslices_bboxes.emplace_back(get_extents(expoly));
        }
    }

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (const Layer *layer : m_layers) {
            LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("0_detect_nonplanar_surfaces");
            layerm->export_region_fill_surfaces_to_svg_debug("0_detect_nonplanar_surfaces");
        }
    }
#endif
    m_typed_slices = true;
}

void
PrintObject::project_nonplanar_surfaces()
{
    if (!curved_tops_mode(m_config.nonplanar_mode.value) || is_step_done(posNonplanarProjection))
        return;
    set_started(posNonplanarProjection);

    struct PlanarLayer {
        std::vector<Surfaces> slices;
        ExPolygons outlines;
        std::vector<BoundingBox> boxes;
    };
    std::vector<PlanarLayer> planar(m_layers.size());
    for (size_t i = 0; i < m_layers.size(); ++i) {
        Layer &layer = *m_layers[i];
        planar[i].outlines = layer.lslices;
        planar[i].boxes = layer.lslices_bboxes;
        for (const LayerRegion *region : layer.regions())
            planar[i].slices.push_back(region->slices.surfaces);
    }
    this->detect_nonplanar_surfaces();
    struct ShellLayer : Layer {
        ShellLayer(const Layer &source, PrintObject *object) :
            Layer(source.id(), object, source.height, source.print_z, source.slice_z) {}
    };
    std::vector<std::unique_ptr<ShellLayer>> shells;
    std::vector<std::vector<ExPolygons>> replaced(m_layers.size());
    Polygons patch_polygons;
    std::map<int, Polygons> top_footprints;
    std::map<int, float> top_insets;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        Layer &layer = *m_layers[i];
        auto shell = std::make_unique<ShellLayer>(layer, this);
        for (size_t r = 0; r < layer.region_count(); ++r) {
            LayerRegion &region = *layer.get_region(int(r));
            LayerRegion &target = *shell->add_region(&region.region());
            Surfaces remaining;
            for (const Surface &surface : region.slices.surfaces)
                if (surface.is_nonplanar()) {
                    target.slices.surfaces.push_back(surface);
                    polygons_append(patch_polygons, to_polygons(surface.expolygon));
                    if (surface.distance_to_top >= 0.f && surface.distance_to_top < 0.001f) {
                        polygons_append(top_footprints[surface.source_surface_id], to_polygons(surface.expolygon));
                        top_insets[surface.source_surface_id] = std::max(top_insets[surface.source_surface_id],
                            float(0.5 * region.flow(frExternalPerimeter).scaled_width()));
                    }
                } else
                    remaining.push_back(surface);
            for (const NonplanarSurface &surface : region.nonplanar_surfaces())
                target.append_nonplanar_surface(surface);
            replaced[i].push_back(diff_ex(planar[i].slices[r], remaining, ApplySafetyOffset::No));
            region.slices.surfaces = std::move(planar[i].slices[r]);
        }
        layer.lslices = std::move(planar[i].outlines);
        layer.lslices_bboxes = std::move(planar[i].boxes);
        shell->make_slices();
        shells.emplace_back(std::move(shell));
    }
    tbb::parallel_for(tbb::blocked_range<size_t>(0, shells.size()), [this, &shells](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            m_print->throw_if_canceled();
            Layer &shell = *shells[i];
            if (shell.empty())
                continue;
            shell.make_perimeters();
            for (LayerRegion *region : shell.regions())
                region->slices_to_fill_surfaces_clipped();
            shell.make_fills(&m_adaptive_fill_octrees, m_lightning_generator.get());
            for (LayerRegion *region : shell.regions())
                region->project_nonplanar_surfaces();
        }
    });
    std::vector<Layer *> boundary_layers;
    for (const auto &shell : shells) boundary_layers.push_back(shell.get());
    if (m_config.nonplanar_continuous_top.value)
        Nonplanar::make_continuous_curved_tops(*this, boundary_layers);
    Nonplanar::finish_curved_top_boundaries(*this, boundary_layers);
    const ExPolygons patch_footprint = union_ex(patch_polygons);
    if (!m_nonplanar_repair) m_nonplanar_repair = std::make_shared<Nonplanar::RepairHints>();
    m_nonplanar_repair->curved_top_areas.clear();
    for (const auto &[id, polygons] : top_footprints)
        m_nonplanar_repair->curved_top_areas[id] = area(offset_ex(union_ex(polygons), -top_insets[id])) *
            SCALING_FACTOR * SCALING_FACTOR;
    const std::array<std::uint64_t, 3> generation{{this->step_state_with_timestamp(posPerimeters).timestamp,
        this->step_state_with_timestamp(posInfill).timestamp, this->step_state_with_timestamp(posIroning).timestamp}};
    if (!m_nonplanar_repair->curved_scaffold || m_nonplanar_repair->curved_scaffold->owner != this ||
        m_nonplanar_repair->curved_scaffold->generation != generation) {
        m_nonplanar_repair->curved_scaffold = std::make_shared<Nonplanar::CurvedTopsScaffold>();
        m_nonplanar_repair->curved_scaffold->owner = this;
        m_nonplanar_repair->curved_scaffold->generation = generation;
    }
    std::map<float, ExPolygons> inset_footprints;
    for (size_t i = 0; i < m_layers.size(); ++i)
        for (size_t r = 0; r < m_layers[i]->region_count(); ++r) {
            LayerRegion &region = *m_layers[i]->get_region(int(r));
            const float inset = -float(0.5 * region.flow(frTopSolidInfill).scaled_width());
            auto footprint = inset_footprints.find(inset);
            if (footprint == inset_footprints.end())
                footprint = inset_footprints.emplace(inset, offset_ex(patch_footprint, inset)).first;
            const ExPolygons area = intersection_ex(replaced[i][r], footprint->second, ApplySafetyOffset::No);
            const LayerRegion &source_shell = *shells[i]->get_region(int(r));
            if (!area.empty() || source_shell.has_extrusions())
                m_nonplanar_repair->curved_scaffold->remember(region);
            if (!area.empty()) {
                const BoundingBox box = get_extents(area);
                Nonplanar::clip_planar_remainder(region.perimeters, area, box);
                Nonplanar::clip_planar_remainder(region.fills, area, box);
                Nonplanar::clip_planar_remainder(region.thin_fills, area, box);
            }
            LayerRegion &shell = *shells[i]->get_region(int(r));
            region.perimeters.append(std::move(shell.perimeters.entities));
            region.fills.append(std::move(shell.fills.entities));
        }
    auto &scaffold = *m_nonplanar_repair->curved_scaffold;
    if (scaffold.restored) {
        for (const auto &saved : scaffold.regions) {
            saved.region->simplify_wall_extrusion_entity();
            saved.region->simplify_infill_extrusion_entity();
        }
        scaffold.restored = false;
    }
    set_done(posNonplanarProjection);
}
void PrintObject::apply_wave_overhang_floor_layer_authority()
{
    bool any_wave = false;
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id)
        if (WaveOverhangs::enabled(this->printing_region(region_id).config(), *this)) {
            any_wave = true;
            break;
        }
    if (!any_wave)
        return;

    BOOST_LOG_TRIVIAL(info) << "Applying wave-overhang floor-layer authority...";

    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        const PrintRegionConfig &rconf = this->printing_region(region_id).config();
        if (!WaveOverhangs::enabled(rconf, *this))
            continue;
        const int floor_layers = rconf.wave_overhang_floor_layers.value;

        tbb::parallel_for(tbb::blocked_range<size_t>(0, m_layers.size()),
            [this, region_id, floor_layers](const tbb::blocked_range<size_t> &range) {
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    m_print->throw_if_canceled();
                    Layer *layer = m_layers[idx_layer];
                    if (layer->wave_overhang_shadow_polygons.empty())
                        continue;

                    Polygons own = m_layers[idx_layer]->wave_overhang_floor_polygons;
                    if (! own.empty())
                        own = union_(own);
                    Polygons solid;
                    for (int d = 1; d <= floor_layers && (int) idx_layer - d >= 0; ++d)
                        append(solid, m_layers[idx_layer - d]->wave_overhang_floor_polygons);
                    if (! solid.empty())
                        solid = union_(solid);
                    const Polygons solid_only = solid.empty() ? Polygons{} : diff(solid, own);
                    Polygons covered = own;
                    append(covered, solid);
                    if (! covered.empty())
                        covered = union_(covered);

                    const Polygons &shadow = layer->wave_overhang_shadow_polygons;
                    LayerRegion *layerm = layer->m_regions[region_id];
                    Surfaces &surfs = layerm->fill_surfaces.surfaces;
                    Surfaces new_surfaces;
                    new_surfaces.reserve(surfs.size() + 6);
                    for (Surface &s : surfs) {
                        Polygons   p       = to_polygons(s);
                        ExPolygons inside  = intersection_ex(p, shadow, ApplySafetyOffset::Yes);
                        if (inside.empty()) {
                            new_surfaces.push_back(std::move(s));
                            continue;
                        }
                        const Polygons    inside_p = to_polygons(inside);
                        const SurfaceType orig     = s.surface_type;
                        const bool        is_geometric_face =
                            orig == stTop || orig == stBottom || orig == stBottomBridge;

                        for (auto &ex_out : union_safety_offset_ex(diff_ex(p, inside_p, ApplySafetyOffset::Yes)))
                            new_surfaces.emplace_back(orig, std::move(ex_out));
                        if (! solid_only.empty()) {
                            const SurfaceType t = is_geometric_face ? orig : stInternalSolid;
                            for (auto &ex_in : union_safety_offset_ex(intersection_ex(inside_p, solid_only, ApplySafetyOffset::Yes)))
                                new_surfaces.emplace_back(t, std::move(ex_in));
                        }
                        const SurfaceType t = is_geometric_face ? orig : stInternal;
                        for (auto &ex_in : union_safety_offset_ex(diff_ex(inside_p, covered, ApplySafetyOffset::Yes)))
                            new_surfaces.emplace_back(t, std::move(ex_in));
                    }
                    surfs = std::move(new_surfaces);
                }
            });
        m_print->throw_if_canceled();
    }
}
void PrintObject::apply_wave_overhang_bridge_suppression()
{
    bool any = false;
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        const PrintRegionConfig &rconf = this->printing_region(region_id).config();
        if (WaveOverhangs::enabled(rconf, *this) && rconf.wave_overhangs_instead_of_bridges.value) {
            any = true;
            break;
        }
    }
    if (!any)
        return;

    BOOST_LOG_TRIVIAL(info) << "Applying wave-overhang bridge suppression...";

    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        const PrintRegionConfig &rconf = this->printing_region(region_id).config();
        if (!WaveOverhangs::enabled(rconf, *this) || !rconf.wave_overhangs_instead_of_bridges.value)
            continue;

        tbb::parallel_for(tbb::blocked_range<size_t>(0, m_layers.size()),
            [this, region_id](const tbb::blocked_range<size_t> &range) {
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    m_print->throw_if_canceled();
                    Layer       *layer  = m_layers[idx_layer];
                    LayerRegion *layerm = layer->m_regions[region_id];
                    Surfaces    &surfs  = layerm->fill_surfaces.surfaces;
                    for (Surface &s : surfs) {
                        if (s.surface_type == stBottomBridge || s.surface_type == stInternalBridge) {
                            s.surface_type = stInternalSolid;
                            s.bridge_angle = 0;
                        }
                    }
                }
            });
        m_print->throw_if_canceled();
    }
}
namespace {
static void tag_wave_overhang_perimeter_recursive(ExtrusionEntity *ent, bool floor_layer, int8_t distance)
{
    if (!ent) return;
    auto stamp = [floor_layer, distance](ExtrusionPath &p) {
        if (floor_layer) {
            p.wave_overhang_floor_perimeter = true;
            p.wave_overhang_floor_distance  = distance;
        } else {
            p.wave_overhang_perimeter = true;
        }
    };
    if (auto *path = dynamic_cast<ExtrusionPath*>(ent))         { stamp(*path); }
    else if (auto *loop = dynamic_cast<ExtrusionLoop*>(ent))    { for (ExtrusionPath &p : loop->paths) stamp(p); }
    else if (auto *mp = dynamic_cast<ExtrusionMultiPath*>(ent)) { for (ExtrusionPath &p : mp->paths)   stamp(p); }
    else if (auto *coll = dynamic_cast<ExtrusionEntityCollection*>(ent))
        for (ExtrusionEntity *child : coll->entities) tag_wave_overhang_perimeter_recursive(child, floor_layer, distance);
}
}

void PrintObject::tag_wave_overhang_perimeters()
{
    bool any_wave = false;
    int  max_floor_layers = 0;
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        const PrintRegionConfig &rconf = this->printing_region(region_id).config();
        if (WaveOverhangs::enabled(rconf, *this)) {
            any_wave = true;
            max_floor_layers = std::max(max_floor_layers, rconf.wave_overhang_floor_layers.value);
        }
    }
    if (!any_wave)
        return;

    BOOST_LOG_TRIVIAL(info) << "Tagging wave-overhang perimeters...";

    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_layers.size()),
        [this, max_floor_layers](const tbb::blocked_range<size_t> &range) {
            for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                m_print->throw_if_canceled();
                Layer *layer = m_layers[idx_layer];
                const bool is_wave_layer  = !layer->wave_overhang_floor_polygons.empty();
                const bool in_shadow      = !is_wave_layer && !layer->wave_overhang_shadow_polygons.empty();
                if (!is_wave_layer && !in_shadow)
                    continue;
                int8_t distance = 0;
                if (in_shadow && max_floor_layers > 0) {
                    const int search_limit = std::min<int>(max_floor_layers, 127);
                    for (int k = 1; k <= search_limit; ++k) {
                        if ((int)idx_layer - k < 0) break;
                        if (!m_layers[idx_layer - k]->wave_overhang_floor_polygons.empty()) {
                            distance = static_cast<int8_t>(k);
                            break;
                        }
                    }
                }
                const bool is_floor_layer = in_shadow && distance > 0;
                if (!is_wave_layer && !is_floor_layer)
                    continue;
                for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
                    if (!WaveOverhangs::enabled(this->printing_region(region_id).config(), *this))
                        continue;
                    LayerRegion *layerm = layer->m_regions[region_id];
                    tag_wave_overhang_perimeter_recursive(&layerm->perimeters, is_floor_layer, distance);
                }
            }
        });
}

void PrintObject::process_external_surfaces()
{
    BOOST_LOG_TRIVIAL(info) << "Processing external surfaces..." << log_memory_info();

    std::vector<Polygons> surfaces_covered;
    bool 				  has_voids = false;
	for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id)
		if (this->printing_region(region_id).config().sparse_infill_density == 0) {
			has_voids = true;
			break;
		}
	if (has_voids && m_layers.size() > 1) {
	    std::vector<unsigned char> layer_expansions_and_voids(m_layers.size(), false);
	    for (size_t layer_idx = 1; layer_idx < m_layers.size(); ++ layer_idx) {
	    	const Layer *layer = m_layers[layer_idx];
	    	bool expansions = false;
	    	bool voids      = false;
	    	for (const LayerRegion *layerm : layer->regions()) {
	    		for (const Surface &surface : layerm->fill_surfaces.surfaces) {
	    			if (surface.surface_type == stInternal)
	    				voids = true;
	    			else
	    				expansions = true;
	    			if (voids && expansions) {
	    				layer_expansions_and_voids[layer_idx] = true;
	    				goto end;
	    			}
	    		}
	    	}
		end:;
		}
	    BOOST_LOG_TRIVIAL(debug) << "Collecting surfaces covered with extrusions in parallel - start";
	    surfaces_covered.resize(m_layers.size() - 1, Polygons());
    	auto unsupported_width = - float(scale_(0.3 * EXTERNAL_INFILL_MARGIN));
	    tbb::parallel_for(
	        tbb::blocked_range<size_t>(0, m_layers.size() - 1),
	        [this, &surfaces_covered, &layer_expansions_and_voids, unsupported_width](const tbb::blocked_range<size_t>& range) {
	            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx)
	            	if (layer_expansions_and_voids[layer_idx + 1]) {
		                m_print->throw_if_canceled();
		                Polygons voids;
		                for (const LayerRegion *layerm : m_layers[layer_idx]->regions()) {
		                	if (layerm->region().config().sparse_infill_density.value == 0.)
		                		for (const Surface &surface : layerm->fill_surfaces.surfaces)
		                			polygons_append(voids, offset(surface.expolygon, unsupported_width));
		                }
		                surfaces_covered[layer_idx] = diff(m_layers[layer_idx]->lslices, voids);
	            	}
	        }
	    );
	    m_print->throw_if_canceled();
	    BOOST_LOG_TRIVIAL(debug) << "Collecting surfaces covered with extrusions in parallel - end";
	}

	for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id) {
        BOOST_LOG_TRIVIAL(debug) << "Processing external surfaces for region " << region_id << " in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this, &surfaces_covered, region_id](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    m_layers[layer_idx]->get_region(int(region_id))->process_external_surfaces(
                    	(layer_idx == 0) ? nullptr : m_layers[layer_idx - 1],
                    	(layer_idx == 0 || surfaces_covered.empty() || surfaces_covered[layer_idx - 1].empty()) ? nullptr : &surfaces_covered[layer_idx - 1]);
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Processing external surfaces for region " << region_id << " in parallel - end";
    }
}

void PrintObject::discover_vertical_shells()
{
    PROFILE_FUNC();

    BOOST_LOG_TRIVIAL(info) << "Discovering vertical shells..." << log_memory_info();

    struct DiscoverVerticalShellsCacheEntry
    {
        Polygons    top_surfaces;
        Polygons    bottom_surfaces;
        Polygons    holes;
    };
    bool     spiral_mode      = this->print()->config().spiral_mode.value;
    size_t   num_layers       = spiral_mode ? std::min(size_t(this->printing_region(0).config().bottom_shell_layers), m_layers.size()) : m_layers.size();
    std::vector<DiscoverVerticalShellsCacheEntry> cache_top_botom_regions(num_layers, DiscoverVerticalShellsCacheEntry());
    bool top_bottom_surfaces_all_regions = this->num_printing_regions() > 1 && ! m_config.interface_shells.value;
    static constexpr const float top_bottom_expansion_coeff = 0.05f;
    if (top_bottom_surfaces_all_regions) {
        bool has_extra_layers = false;
        for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id) {
            const PrintRegionConfig &config = this->printing_region(region_id).config();
            if (config.ensure_vertical_shell_thickness.value == evstAll) {
                has_extra_layers = true;
                break;
            }
        }
        if (! has_extra_layers)
            return;
        BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells in parallel - start : cache top / bottom";
        size_t grain_size = std::max(num_layers / 16, size_t(1));
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, num_layers, grain_size),
            [this, &cache_top_botom_regions](const tbb::blocked_range<size_t>& range) {
                const std::initializer_list<SurfaceType> surfaces_bottom { stBottom, stBottomBridge };
                const size_t num_regions = this->num_printing_regions();
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    m_print->throw_if_canceled();
                    const Layer                      &layer = *m_layers[idx_layer];
                    DiscoverVerticalShellsCacheEntry &cache = cache_top_botom_regions[idx_layer];
                    float                             perimeter_offset = 0.f;
                    float                             perimeter_min_spacing = FLT_MAX;
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    static size_t debug_idx = 0;
                    ++ debug_idx;
#endif
                    for (size_t region_id = 0; region_id < num_regions; ++ region_id) {
                        LayerRegion &layerm               = *layer.m_regions[region_id];
                        float        top_bottom_expansion = float(layerm.flow(frSolidInfill).scaled_spacing()) * top_bottom_expansion_coeff;
                        append(cache.top_surfaces, offset(layerm.slices.filter_by_types({ stTop, stTopNonplanar }), top_bottom_expansion));
                        append(cache.bottom_surfaces, offset(layerm.slices.filter_by_types(surfaces_bottom), top_bottom_expansion));
                        if (!layer.wave_overhang_shadow_polygons.empty()) {
                            if (!cache.bottom_surfaces.empty())
                                cache.bottom_surfaces = diff(cache.bottom_surfaces, layer.wave_overhang_shadow_polygons);
                            if (!cache.top_surfaces.empty())
                                cache.top_surfaces    = diff(cache.top_surfaces,    layer.wave_overhang_shadow_polygons);
                        }
                        unsigned int perimeters = 0;
                        for (Surface &s : layerm.slices.surfaces)
                            perimeters = std::max<unsigned int>(perimeters, s.extra_perimeters);
                        perimeters += layerm.region().config().wall_loops.value;
                        if (perimeters > 0) {
                            Flow extflow = layerm.flow(frExternalPerimeter);
                            Flow flow    = layerm.flow(frPerimeter);
                            perimeter_offset = std::max(perimeter_offset,
                                0.5f * float(extflow.scaled_width() + extflow.scaled_spacing()) + (float(perimeters) - 1.f) * flow.scaled_spacing());
                            perimeter_min_spacing = std::min(perimeter_min_spacing, float(std::min(extflow.scaled_spacing(), flow.scaled_spacing())));
                        }
                        polygons_append(cache.holes, to_polygons(layerm.fill_expolygons));
                    }
                    cache.top_surfaces    = union_(cache.top_surfaces);
                    cache.bottom_surfaces = union_(cache.bottom_surfaces);
                    if (perimeter_offset > 0.) {
                        polygons_append(cache.holes, offset2(layer.lslices, 0.3f * perimeter_min_spacing, - perimeter_offset - 0.3f * perimeter_min_spacing));
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                        {
                            Slic3r::SVG svg(debug_out_path("discover_vertical_shells-extra-holes-%d.svg", debug_idx), get_extents(layer.lslices));
                            svg.draw(layer.lslices, "blue");
                            svg.draw(union_ex(cache.holes), "red");
                            svg.draw_outline(union_ex(cache.holes), "black", "blue", scale_(0.05));
                            svg.Close();
                        }
#endif
                    }
                    cache.holes = union_(cache.holes);
                }
            });
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells in parallel - end : cache top / bottom";
    }

    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        const PrintRegion &region = this->printing_region(region_id);
        if (region.config().ensure_vertical_shell_thickness.value != evstAll )
            continue;

        size_t grain_size = std::max(num_layers / 16, size_t(1));

        if (! top_bottom_surfaces_all_regions) {
            BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells for region " << region_id << " in parallel - start : cache top / bottom";
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, num_layers, grain_size),
                [this, region_id, &cache_top_botom_regions](const tbb::blocked_range<size_t>& range) {
                    const std::initializer_list<SurfaceType> surfaces_bottom { stBottom, stBottomBridge };
                    for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                        m_print->throw_if_canceled();
                        Layer       &layer                = *m_layers[idx_layer];
                        LayerRegion &layerm               = *layer.m_regions[region_id];
                        float        top_bottom_expansion = float(layerm.flow(frSolidInfill).scaled_spacing()) * top_bottom_expansion_coeff;
                        auto &cache = cache_top_botom_regions[idx_layer];
                        cache.top_surfaces = offset(layerm.slices.filter_by_types({ stTop, stTopNonplanar }), top_bottom_expansion);
                        cache.bottom_surfaces = offset(layerm.slices.filter_by_types(surfaces_bottom), top_bottom_expansion);
                        if (!layer.wave_overhang_shadow_polygons.empty()) {
                            if (!cache.bottom_surfaces.empty())
                                cache.bottom_surfaces = diff(cache.bottom_surfaces, layer.wave_overhang_shadow_polygons);
                            if (!cache.top_surfaces.empty())
                                cache.top_surfaces    = diff(cache.top_surfaces,    layer.wave_overhang_shadow_polygons);
                        }
                        if (cache.holes.empty()) {
                            for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id)
                                polygons_append(cache.holes, to_polygons(layer.regions()[region_id]->fill_expolygons));
                        }
                    }
                });
            m_print->throw_if_canceled();
            BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells for region " << region_id << " in parallel - end : cache top / bottom";
        }

        BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells for region " << region_id << " in parallel - start : ensure vertical wall thickness";
        grain_size = 1;
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, num_layers, grain_size),
            [this, region_id, &cache_top_botom_regions]
            (const tbb::blocked_range<size_t>& range) {
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    m_print->throw_if_canceled();
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        			static size_t debug_idx = 0;
        			++ debug_idx;
#endif

                    Layer       	        *layer          = m_layers[idx_layer];
                    LayerRegion 	        *layerm         = layer->m_regions[region_id];
                    const PrintRegionConfig &region_config  = layerm->region().config();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    layerm->export_region_slices_to_svg_debug("3_discover_vertical_shells-initial");
                    layerm->export_region_fill_surfaces_to_svg_debug("3_discover_vertical_shells-initial");
#endif

                    Flow         solid_infill_flow   = layerm->flow(frSolidInfill);
                    coord_t      infill_line_spacing = solid_infill_flow.scaled_spacing();
                    Polygons shell;
                    Polygons holes;
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    ExPolygons shell_ex;
#endif
                    float min_perimeter_infill_spacing = float(infill_line_spacing) * 1.05f;
#if 0
                    {
        				Slic3r::SVG svg_cummulative(debug_out_path("discover_vertical_shells-perimeters-before-union-run%d.svg", debug_idx), this->bounding_box());
                        for (int n = (int)idx_layer - n_extra_bottom_layers; n <= (int)idx_layer + n_extra_top_layers; ++ n) {
                            if (n < 0 || n >= (int)m_layers.size())
                                continue;
                            ExPolygons &expolys = m_layers[n]->perimeter_expolygons;
                            for (size_t i = 0; i < expolys.size(); ++ i) {
        						Slic3r::SVG svg(debug_out_path("discover_vertical_shells-perimeters-before-union-run%d-layer%d-expoly%d.svg", debug_idx, n, i), get_extents(expolys[i]));
                                svg.draw(expolys[i]);
                                svg.draw_outline(expolys[i].contour, "black", scale_(0.05));
                                svg.draw_outline(expolys[i].holes, "blue", scale_(0.05));
                                svg.Close();

                                svg_cummulative.draw(expolys[i]);
                                svg_cummulative.draw_outline(expolys[i].contour, "black", scale_(0.05));
                                svg_cummulative.draw_outline(expolys[i].holes, "blue", scale_(0.05));
                            }
                        }
                    }
#endif
			        polygons_append(holes, cache_top_botom_regions[idx_layer].holes);
                    auto combine_holes = [&holes](const Polygons &holes2) {
                        if (holes.empty() || holes2.empty())
                            holes.clear();
                        else
                            holes = intersection(holes, holes2);
                    };
                    auto combine_shells = [&shell](const Polygons &shells2) {
                        if (shell.empty())
                            shell = std::move(shells2);
                        else if (! shells2.empty()) {
                            polygons_append(shell, shells2);
                            shell = union_(shell);
                        }
                    };
                    static constexpr const bool one_more_layer_below_top_bottom_surfaces = false;
			        if (int n_top_layers = region_config.top_shell_layers.value; n_top_layers > 0) {
                        coordf_t print_z = layer->print_z;
                        int i = int(idx_layer) + 1;
                        int itop = int(idx_layer) + n_top_layers;
                        bool at_least_one_top_projected = false;
	                    for (; i < int(cache_top_botom_regions.size()) &&
	                         (i < itop || m_layers[i]->print_z - print_z < region_config.top_shell_thickness - EPSILON);
	                        ++ i) {
                            at_least_one_top_projected = true;
	                        const DiscoverVerticalShellsCacheEntry &cache = cache_top_botom_regions[i];
                            combine_holes(cache.holes);
                            combine_shells(cache.top_surfaces);
	                    }
                        if (!at_least_one_top_projected && i < int(cache_top_botom_regions.size())) {
                            Polygons anchor_area = intersection(expand(cache_top_botom_regions[idx_layer].top_surfaces,
                                                                       layerm->flow(frExternalPerimeter).scaled_spacing()),
                                                                to_polygons(m_layers[i]->lslices));
                            combine_shells(anchor_area);
                        }

                        if (one_more_layer_below_top_bottom_surfaces)
                            if (i < int(cache_top_botom_regions.size()) &&
                                (i <= itop || m_layers[i]->bottom_z() - print_z < region_config.top_shell_thickness - EPSILON))
                                combine_holes(cache_top_botom_regions[i].holes);
	                }
	                if (int n_bottom_layers = region_config.bottom_shell_layers.value; n_bottom_layers > 0) {
                        coordf_t bottom_z = layer->bottom_z();
                        int i = int(idx_layer) - 1;
                        int ibottom = int(idx_layer) - n_bottom_layers;
                        bool at_least_one_bottom_projected = false;
	                    for (; i >= 0 &&
	                         (i > ibottom || bottom_z - m_layers[i]->bottom_z() < region_config.bottom_shell_thickness - EPSILON);
	                        -- i) {
                                at_least_one_bottom_projected = true;
	                        const DiscoverVerticalShellsCacheEntry &cache = cache_top_botom_regions[i];
							combine_holes(cache.holes);
                            combine_shells(cache.bottom_surfaces);
	                    }

                        if (!at_least_one_bottom_projected && i >= 0) {
                            Polygons anchor_area = intersection(expand(cache_top_botom_regions[idx_layer].bottom_surfaces,
                                                                       layerm->flow(frExternalPerimeter).scaled_spacing()),
                                                                to_polygons(m_layers[i]->lslices));
                            combine_shells(anchor_area);
                        }

                        if (one_more_layer_below_top_bottom_surfaces)
                            if (i >= 0 &&
                                (i > ibottom || bottom_z - m_layers[i]->print_z < region_config.bottom_shell_thickness - EPSILON))
                                combine_holes(cache_top_botom_regions[i].holes);
	                }
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
        				Slic3r::SVG svg(debug_out_path("discover_vertical_shells-perimeters-before-union-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(shell);
                        svg.draw_outline(shell, "black", scale_(0.05));
                        svg.Close();
                    }
#endif
#if 0
                    shell = union_(shell, false);
#endif
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    shell_ex = union_safety_offset_ex(shell);
#endif

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-perimeters-after-union-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(shell_ex);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05));
                        svg.Close();
                    }
#endif

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-internal-wshell-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(layerm->fill_surfaces.filter_by_type(stInternal), "yellow", 0.5);
                        svg.draw_outline(layerm->fill_surfaces.filter_by_type(stInternal), "black", "blue", scale_(0.05));
                        svg.draw(shell_ex, "blue", 0.5);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05));
                        svg.Close();
                    }
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-internalvoid-wshell-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(layerm->fill_surfaces.filter_by_type(stInternalVoid), "yellow", 0.5);
                        svg.draw_outline(layerm->fill_surfaces.filter_by_type(stInternalVoid), "black", "blue", scale_(0.05));
                        svg.draw(shell_ex, "blue", 0.5);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05));
                        svg.Close();
                    }
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-internalvoid-wshell-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(layerm->fill_surfaces.filter_by_type(stInternalVoid), "yellow", 0.5);
                        svg.draw_outline(layerm->fill_surfaces.filter_by_type(stInternalVoid), "black", "blue", scale_(0.05));
                        svg.draw(shell_ex, "blue", 0.5);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05));
                        svg.Close();
                    }
#endif
                    const Polygons polygonsInternal = to_polygons(layerm->fill_surfaces.filter_by_types({ stInternal, stInternalVoid, stInternalSolid, stInternalSolidNonplanar }));
                    const Polygons polygonsInternalNonplanar = to_polygons(layerm->fill_surfaces.filter_by_type(stInternalSolidNonplanar));
                    shell = intersection(shell, polygonsInternal, ApplySafetyOffset::Yes);
                    polygons_append(shell, diff(polygonsInternal, holes));
                    if (shell.empty())
                        continue;

                    polygons_append(shell, to_polygons(layerm->fill_surfaces.filter_by_types({ stInternalSolid, stInternalSolidNonplanar })));

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    Polygons shell_before = shell;
#endif
                    ExPolygons regularized_shell;
                    {
                        const float narrow_ensure_vertical_wall_thickness_region_radius = 0.5f * 0.65f * min_perimeter_infill_spacing;
                        const float narrow_sparse_infill_region_radius                  = 0.5f * 1.2f * min_perimeter_infill_spacing;
                        const float tiny_overlap_radius                                 = 0.2f        * min_perimeter_infill_spacing;
                        regularized_shell = shrink_ex(offset2_ex(union_ex(shell),
                            -narrow_ensure_vertical_wall_thickness_region_radius,
                            narrow_ensure_vertical_wall_thickness_region_radius + narrow_sparse_infill_region_radius, jtSquare),
                            narrow_sparse_infill_region_radius - tiny_overlap_radius, jtSquare);

                        Polygons object_volume;
                        Polygons internal_volume;
                        {
                            Polygons shrinked_bottom_slice = idx_layer > 0 ? to_polygons(m_layers[idx_layer - 1]->lslices) : Polygons{};
                            Polygons shrinked_upper_slice  = (idx_layer + 1) < m_layers.size() ?
                                                                 to_polygons(m_layers[idx_layer + 1]->lslices) :
                                                                 Polygons{};
                            object_volume = intersection(shrinked_bottom_slice, shrinked_upper_slice);
                            internal_volume = closing(polygonsInternal, SCALED_EPSILON);
                        }

                        regularized_shell.erase(std::remove_if(regularized_shell.begin(), regularized_shell.end(),
                                                               [&internal_volume, &min_perimeter_infill_spacing,
                                                                &object_volume](const ExPolygon &p) {
                                                                   return (p.area() < min_perimeter_infill_spacing * scaled(1.5) ||
                                                                           (p.area() < min_perimeter_infill_spacing * scaled(8.0) &&
                                                                            diff(to_polygons(p), object_volume).empty())) &&
                                                                          diff(internal_volume,
                                                                               expand(to_polygons(p), min_perimeter_infill_spacing))
                                                                                  .size() >= internal_volume.size();
                                                               }),
                                                regularized_shell.end());
                    }
                    if (regularized_shell.empty())
                        continue;

                    ExPolygons new_internal_solid = intersection_ex(diff_ex(polygonsInternal, polygonsInternalNonplanar), regularized_shell);
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-regularized-%d.svg", debug_idx), get_extents(shell_before));
                        svg.draw(union_safety_offset_ex(shell_before));
                        svg.draw_outline(union_safety_offset_ex(shell), "black", "blue", scale_(0.05));
                        svg.draw_outline(new_internal_solid, "red", "magenta", scale_(0.05));
                        svg.Close();
                    }
#endif
                    Slic3r::ExPolygons new_internal = diff_ex(layerm->fill_surfaces.filter_by_type(stInternal), regularized_shell);
                    Slic3r::ExPolygons new_internal_void = diff_ex(layerm->fill_surfaces.filter_by_type(stInternalVoid), regularized_shell);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        SVG::export_expolygons(debug_out_path("discover_vertical_shells-new_internal-%d.svg", debug_idx), get_extents(shell), new_internal, "black", "blue", scale_(0.05));
        				SVG::export_expolygons(debug_out_path("discover_vertical_shells-new_internal_void-%d.svg", debug_idx), get_extents(shell), new_internal_void, "black", "blue", scale_(0.05));
        				SVG::export_expolygons(debug_out_path("discover_vertical_shells-new_internal_solid-%d.svg", debug_idx), get_extents(shell), new_internal_solid, "black", "blue", scale_(0.05));
                    }
#endif
                    layerm->fill_surfaces.keep_types({ stTop, stTopNonplanar, stBottom, stBottomBridge, stInternalSolidNonplanar });
                    layerm->fill_surfaces.append(new_internal,       stInternal);
                    layerm->fill_surfaces.append(new_internal_void,  stInternalVoid);
                    layerm->fill_surfaces.append(new_internal_solid, stInternalSolid);
                }
            });
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells for region " << region_id << " in parallel - end";

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
		for (size_t idx_layer = 0; idx_layer < m_layers.size(); ++idx_layer) {
			LayerRegion *layerm = m_layers[idx_layer]->get_region(region_id);
			layerm->export_region_slices_to_svg_debug("3_discover_vertical_shells-final");
			layerm->export_region_fill_surfaces_to_svg_debug("3_discover_vertical_shells-final");
		}
#endif
    }
}
#ifdef DEBUG_BRIDGE_OVER_INFILL
template<typename T> void debug_draw(std::string name, const T& a, const T& b, const T& c, const T& d)
{
        std::vector<std::string> colors = {"red", "green", "blue", "orange"};
    BoundingBox              bbox   = get_extents(a);
    bbox.merge(get_extents(b));
    bbox.merge(get_extents(c));
    bbox.merge(get_extents(d));
    bbox.offset(scale_(1.));
    ::Slic3r::SVG svg(debug_out_path(name.c_str()).c_str(), bbox);   
    svg.draw(a, colors[0], scale_(0.3));
    svg.draw(b, colors[1], scale_(0.23));
    svg.draw(c, colors[2], scale_(0.16));
    svg.draw(d, colors[3], scale_(0.10));
    svg.Close();
}
#endif

void PrintObject::bridge_over_infill()
{
    BOOST_LOG_TRIVIAL(info) << "Bridge over infill - Start" << log_memory_info();
    struct CandidateSurface
    {
        CandidateSurface(const Surface     *original_surface,
                         int                layer_index,
                         Polygons           new_polys,
                         const LayerRegion *region,
                         double             bridge_angle)
            : original_surface(original_surface)
            , layer_index(layer_index)
            , new_polys(new_polys)
            , region(region)
            , bridge_angle(bridge_angle)
        {}
        const Surface     *original_surface;
        int                layer_index;
        Polygons           new_polys;
        const LayerRegion *region;
        double             bridge_angle;
    };

    std::map<size_t, std::vector<CandidateSurface>> surfaces_by_layer;
    bool has_lightning_infill = false;
    for (size_t i = 0; i < this->num_printing_regions(); i++) {
        if (this->printing_region(i).config().sparse_infill_pattern == ipLightning) {
            has_lightning_infill = true;
            break;
        }
    }

    {
        tbb::concurrent_vector<CandidateSurface> candidate_surfaces;
        tbb::parallel_for(tbb::blocked_range<size_t>(0, this->layers().size()), [po = static_cast<const PrintObject *>(this), &candidate_surfaces](tbb::blocked_range<size_t> r) {
            PRINT_OBJECT_TIME_LIMIT_MILLIS(PRINT_OBJECT_TIME_LIMIT_DEFAULT);
            for (size_t lidx = r.begin(); lidx < r.end(); lidx++) {
                const Layer *layer = po->get_layer(lidx);
                if (layer->lower_layer == nullptr) {
                    continue;
                }
                double spacing = layer->regions().front()->flow(frSolidInfill).scaled_spacing();
                Polygons   unsupported_area;
                Polygons   lower_layer_solids;
                for (const LayerRegion *region : layer->lower_layer->regions()) {
                    Polygons fill_polys = to_polygons(region->fill_expolygons);
                    unsupported_area.insert(unsupported_area.end(), fill_polys.begin(), fill_polys.end());
                    for (const Surface &surface : region->fill_surfaces) {
                        if (surface.surface_type != stInternal || region->region().config().sparse_infill_density.value == 100) {
                            Polygons p = to_polygons(surface.expolygon);
                            lower_layer_solids.insert(lower_layer_solids.end(), p.begin(), p.end());
                        }
                    }
                }
                unsupported_area = closing(unsupported_area, float(SCALED_EPSILON));
                
                double expansion_multiplier = 3;
                if(po->config().dont_filter_internal_bridges.value !=ibfDisabled){
                    expansion_multiplier = 1;
                }
                lower_layer_solids = shrink(lower_layer_solids, 1 * spacing);
                lower_layer_solids = expand(lower_layer_solids, (1 + expansion_multiplier) * spacing);
                unsupported_area   = shrink(unsupported_area, expansion_multiplier * spacing);
                unsupported_area   = diff(unsupported_area, lower_layer_solids);
                
                for (const LayerRegion *region : layer->regions()) {
                    SurfacesPtr region_internal_solids = region->fill_surfaces.filter_by_type(stInternalSolid);
                    for (const Surface *s : region_internal_solids) {
                        Polygons unsupported         = intersection(to_polygons(s->expolygon), unsupported_area);
                        
                        if (po->config().dont_filter_internal_bridges.value == ibfNofilter){
                            unsupported = expand(unsupported, 4 * spacing);
                            candidate_surfaces.push_back(CandidateSurface(s, lidx, unsupported, region, 0));
                        }else{
                            bool     partially_supported = area(unsupported) < area(to_polygons(s->expolygon)) - EPSILON;
                            if (!unsupported.empty() && (!partially_supported || area(unsupported) > 3 * 3 * spacing * spacing)) {
                                Polygons worth_bridging = intersection(to_polygons(s->expolygon), expand(unsupported, 4 * spacing));
                                for (const Polygon& p : diff(to_polygons(s->expolygon), expand(worth_bridging, spacing))) {
                                    double area = p.area();
                                    if (area < spacing * scale_(12.0) && area > spacing * spacing) {
                                        worth_bridging.push_back(p);
                                    }
                                }
                                worth_bridging = intersection(closing(worth_bridging, float(SCALED_EPSILON)), s->expolygon);
                                candidate_surfaces.push_back(CandidateSurface(s, lidx, worth_bridging, region, 0));
                                
#ifdef DEBUG_BRIDGE_OVER_INFILL
                                debug_draw(std::to_string(lidx) + "_candidate_surface_" + std::to_string(area(s->expolygon)),
                                           to_lines(region->layer()->lslices), to_lines(s->expolygon), to_lines(worth_bridging),
                                           to_lines(unsupported_area));
#endif
#ifdef DEBUG_BRIDGE_OVER_INFILL
                                debug_draw(std::to_string(lidx) + "_candidate_processing_" + std::to_string(area(unsupported)),
                                           to_lines(unsupported), to_lines(intersection(to_polygons(s->expolygon), expand(unsupported, 5 * spacing))),
                                           to_lines(diff(to_polygons(s->expolygon), expand(worth_bridging, spacing))),
                                           to_lines(unsupported_area));
#endif
                            }
                        }
                    }
                }
            }
        });

        for (const CandidateSurface &c : candidate_surfaces) {
            surfaces_by_layer[c.layer_index].push_back(c);
        }
    }

    if (has_lightning_infill) {
        std::map<size_t, std::map<const LayerRegion *, SurfaceCollection>> backup_surfaces;
        for (size_t lidx = 0; lidx < this->layer_count(); lidx++) {
            backup_surfaces[lidx] = {};
        }

        std::vector<char> needs_lightning_expansion(this->layers().size(), 0);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, this->layers().size()), [po = this, &surfaces_by_layer,
                                                                                 &needs_lightning_expansion](tbb::blocked_range<size_t> r) {
            PRINT_OBJECT_TIME_LIMIT_MILLIS(PRINT_OBJECT_TIME_LIMIT_DEFAULT);
            for (size_t lidx = r.begin(); lidx < r.end(); lidx++) {
                if (surfaces_by_layer.find(lidx) == surfaces_by_layer.end())
                    continue;
                const Layer *layer = po->get_layer(lidx);
                const Layer *lower_layer = layer->lower_layer;
                if (lower_layer == nullptr)
                    continue;
                for (const LayerRegion *region : lower_layer->regions()) {
                    if (region->region().config().sparse_infill_pattern == ipLightning
                        && ! region->fill_surfaces.filter_by_type(stInternal).empty()) {
                        needs_lightning_expansion[lidx] = 1;
                        break;
                    }
                }
            }
        });

        tbb::parallel_for(tbb::blocked_range<size_t>(0, this->layers().size()), [po = this, &backup_surfaces,
                                                                                 &surfaces_by_layer,
                                                                                 &needs_lightning_expansion](tbb::blocked_range<size_t> r) {
            PRINT_OBJECT_TIME_LIMIT_MILLIS(PRINT_OBJECT_TIME_LIMIT_DEFAULT);
            for (size_t lidx = r.begin(); lidx < r.end(); lidx++) {
                if (! needs_lightning_expansion[lidx])
                    continue;

                Layer *layer = po->get_layer(lidx);

                for (LayerRegion *region : layer->regions()) {
                    backup_surfaces[lidx][region] = std::move(
                        region->fill_surfaces);
                    region->fill_surfaces = backup_surfaces[lidx][region];
                }

                for (LayerRegion *region : layer->regions()) {
                    ExPolygons sparse_infill = to_expolygons(region->fill_surfaces.filter_by_type(stInternal));
                    ExPolygons solid_infill  = to_expolygons(region->fill_surfaces.filter_by_type(stInternalSolid));

                    if (sparse_infill.empty()) {
                        break;
                    }
                    for (const auto &surface : surfaces_by_layer[lidx]) {
                        if (surface.region != region)
                            continue;
                        ExPolygons expansion = intersection_ex(sparse_infill, expand(surface.new_polys, scaled<float>(3.0)));
                        solid_infill.insert(solid_infill.end(), expansion.begin(), expansion.end());
                    }

                    solid_infill  = union_safety_offset_ex(solid_infill);
                    sparse_infill = diff_ex(sparse_infill, solid_infill);

                    region->fill_surfaces.remove_types({stInternalSolid, stInternal});
                    for (const ExPolygon &ep : solid_infill) {
                        region->fill_surfaces.surfaces.emplace_back(stInternalSolid, ep);
                    }
                    for (const ExPolygon &ep : sparse_infill) {
                        region->fill_surfaces.surfaces.emplace_back(stInternal, ep);
                    }
                }
            }
        });

        this->m_lightning_generator = this->prepare_lightning_infill_data();

        for (size_t lidx = 0; lidx < this->layer_count(); lidx++) {
            Layer *layer = this->get_layer(lidx);
            for (LayerRegion *region : layer->regions()) {
                if (backup_surfaces[lidx].find(region) != backup_surfaces[lidx].end()) {
                    region->fill_surfaces = std::move(backup_surfaces[lidx][region]);
                }
            }
        }
    }

    std::map<size_t, Polylines> infill_lines;
    {
        std::vector<std::pair<const Surface *, const Layer *>> surfaces_w_layer;
        for (const auto &pair : surfaces_by_layer) {
            for (const CandidateSurface &c : pair.second) {
                surfaces_w_layer.emplace_back(c.original_surface, c.region->m_layer);
            }
        }

        this->m_adaptive_fill_octrees = this->prepare_adaptive_infill_data(surfaces_w_layer);
        this->m_tpms_radial_fields    = this->prepare_tpms_radial_fields();

        std::vector<size_t> layers_to_generate_infill;
        for (const auto &pair : surfaces_by_layer) {
            assert(pair.first > 0);
            infill_lines[pair.first - 1] = {};
            layers_to_generate_infill.push_back(pair.first - 1);
        }

        tbb::parallel_for(tbb::blocked_range<size_t>(0, layers_to_generate_infill.size()), [po = static_cast<const PrintObject *>(this),
                                                                                            &layers_to_generate_infill,
                                                                                            &infill_lines](tbb::blocked_range<size_t> r) {
            PRINT_OBJECT_TIME_LIMIT_MILLIS(PRINT_OBJECT_TIME_LIMIT_DEFAULT);
            for (size_t job_idx = r.begin(); job_idx < r.end(); job_idx++) {
                size_t lidx = layers_to_generate_infill[job_idx];
                infill_lines.at(
                    lidx) = po->get_layer(lidx)->generate_sparse_infill_polylines_for_anchoring(&po->m_adaptive_fill_octrees,
                                                                                                po->m_lightning_generator.get());
            }
        });
#ifdef DEBUG_BRIDGE_OVER_INFILL
        for (const auto &il : infill_lines) {
            debug_draw(std::to_string(il.first) + "_infill_lines", to_lines(get_layer(il.first)->lslices), to_lines(il.second), {}, {});
        }
#endif
    }

    std::vector<std::vector<size_t>> clustered_layers_for_threads;
    float target_flow_height_factor = 0.9f;
    {
        std::vector<size_t> layers_with_candidates;
        std::map<size_t, Polygons> layer_area_covered_by_candidates;
        for (const auto& pair : surfaces_by_layer) {
            layers_with_candidates.push_back(pair.first);
            layer_area_covered_by_candidates[pair.first] = {};
        }

        tbb::parallel_for(tbb::blocked_range<size_t>(0, layers_with_candidates.size()), [&layers_with_candidates, &surfaces_by_layer,
                                                                                         &layer_area_covered_by_candidates](
                                                                                            tbb::blocked_range<size_t> r) {
            PRINT_OBJECT_TIME_LIMIT_MILLIS(PRINT_OBJECT_TIME_LIMIT_DEFAULT);
            for (size_t job_idx = r.begin(); job_idx < r.end(); job_idx++) {
                size_t lidx = layers_with_candidates[job_idx];
                for (const auto &candidate : surfaces_by_layer.at(lidx)) {
                    Polygon candiate_inflated_aabb = get_extents(candidate.new_polys).inflated(scale_(7)).polygon();
                    layer_area_covered_by_candidates.at(lidx) = union_(layer_area_covered_by_candidates.at(lidx),
                                                                       Polygons{candiate_inflated_aabb});
                }
            }
        });

        for (auto pair : surfaces_by_layer) {
            if (clustered_layers_for_threads.empty() ||
                this->get_layer(clustered_layers_for_threads.back().back())->print_z <
                    this->get_layer(pair.first)->print_z -
                        this->get_layer(pair.first)->regions()[0]->bridging_flow(frSolidInfill, true).height() * target_flow_height_factor -
                        EPSILON ||
                intersection(layer_area_covered_by_candidates[clustered_layers_for_threads.back().back()],
                             layer_area_covered_by_candidates[pair.first])
                    .empty()) {
                clustered_layers_for_threads.push_back({pair.first});
            } else {
                clustered_layers_for_threads.back().push_back(pair.first);
            }
        }

#ifdef DEBUG_BRIDGE_OVER_INFILL
        std::cout << "BRIDGE OVER INFILL CLUSTERED LAYERS FOR SINGLE THREAD" << std::endl;
        for (auto cluster : clustered_layers_for_threads) {
            std::cout << "CLUSTER: ";
            for (auto l : cluster) {
                std::cout << l << "  ";
            }
            std::cout << std::endl;
        }
#endif
    }

    auto gather_areas_w_depth = [target_flow_height_factor](const PrintObject *po, int lidx, float target_flow_height) {
        ExPolygons layers_sparse_infill{};
        ExPolygons not_sparse_infill{};
        double   bottom_z = po->get_layer(lidx)->print_z - target_flow_height * target_flow_height_factor - EPSILON;
        for (int i = int(lidx) - 1; i >= 0; --i) {
            const Layer *layer = po->get_layer(i);
            if (layer->print_z < bottom_z && i < int(lidx) - 1)
                break;

            for (const LayerRegion *region : layer->regions()) {
                bool has_low_density = region->region().config().sparse_infill_density.value < 100;
                for (const Surface &surface : region->fill_surfaces) {
                    if ((surface.surface_type == stInternal && has_low_density) || surface.surface_type == stInternalVoid ) {
                        layers_sparse_infill.push_back(surface.expolygon);
                    } else {
                        not_sparse_infill.push_back(surface.expolygon);
                    }
                }
            }
        }
        layers_sparse_infill = union_ex(layers_sparse_infill);
        layers_sparse_infill = closing_ex(layers_sparse_infill, float(SCALED_EPSILON));
        not_sparse_infill    = union_ex(not_sparse_infill);
        not_sparse_infill    = closing_ex(not_sparse_infill, float(SCALED_EPSILON));
        return diff(layers_sparse_infill, not_sparse_infill);
    };

    auto determine_bridging_angle = [](const Polygons &bridged_area, const Lines &anchors) {
        AABBTreeLines::LinesDistancer<Line> lines_tree(anchors);

        std::map<double, int> counted_directions;
        for (const Polygon &p : bridged_area) {
            double acc_distance = 0;
            for (int point_idx = 0; point_idx < int(p.points.size()) - 1; ++point_idx) {
                Vec2d  start        = p.points[point_idx].cast<double>();
                Vec2d  next         = p.points[point_idx + 1].cast<double>();
                Vec2d  v            = next - start;
                double dist_to_next = v.norm();
                acc_distance += dist_to_next;
                if (acc_distance > scaled(2.0)) {
                    acc_distance = 0.0;
                    v.normalize();
                    int   lines_count = int(std::ceil(dist_to_next / scaled(2.0)));
                    float step_size   = dist_to_next / lines_count;
                    for (int i = 0; i < lines_count; ++i) {
                        Point a                   = (start + v * (i * step_size)).cast<coord_t>();
                        auto [distance, index, p] = lines_tree.distance_from_lines_extra<false>(a);
                        double angle = lines_tree.get_line(index).orientation();
                        if (angle > PI) {
                            angle -= PI;
                        }
                        angle += PI * 0.5;
                        counted_directions[angle]++;
                    }
                }
            }
        }

        std::pair<double, int> best_dir{0, 0};
        for (const auto &dir : counted_directions) {
            int    score_acc          = 0;
            double dir_acc            = 0;
            double window_start_angle = dir.first - PI * 0.1;
            double window_end_angle   = dir.first + PI * 0.1;
            for (auto dirs_window = counted_directions.lower_bound(window_start_angle);
                 dirs_window != counted_directions.upper_bound(window_end_angle); dirs_window++) {
                dir_acc += dirs_window->first * dirs_window->second;
                score_acc += dirs_window->second;
            }
            if (window_start_angle < 0.5 * PI) {
                for (auto dirs_window = counted_directions.lower_bound(1.5 * PI - (0.5 * PI - window_start_angle));
                     dirs_window != counted_directions.end(); dirs_window++) {
                    dir_acc += (dirs_window->first - PI) * dirs_window->second;
                    score_acc += dirs_window->second;
                }
            }
            if (window_start_angle > 1.5 * PI) {
                for (auto dirs_window = counted_directions.begin();
                     dirs_window != counted_directions.upper_bound(window_start_angle - 1.5 * PI); dirs_window++) {
                    dir_acc += (dirs_window->first + PI) * dirs_window->second;
                    score_acc += dirs_window->second;
                }
            }

            if (score_acc > best_dir.second) {
                best_dir = {dir_acc / score_acc, score_acc};
            }
        }
        double bridging_angle = best_dir.first;
        if (bridging_angle == 0) {
            bridging_angle = 0.001;
        }

        return bridging_angle;
    };

    auto construct_anchored_polygon = [](Polygons bridged_area, Lines anchors, const Flow &bridging_flow, double bridging_angle,
                                         coord_t scan_spacing, bool restore_anchors = false) {
        auto lines_rotate = [](Lines &lines, double cos_angle, double sin_angle) {
            for (Line &l : lines) {
                double ax = double(l.a.x());
                double ay = double(l.a.y());
                l.a.x()   = coord_t(round(cos_angle * ax - sin_angle * ay));
                l.a.y()   = coord_t(round(cos_angle * ay + sin_angle * ax));
                double bx = double(l.b.x());
                double by = double(l.b.y());
                l.b.x()   = coord_t(round(cos_angle * bx - sin_angle * by));
                l.b.y()   = coord_t(round(cos_angle * by + sin_angle * bx));
            }
        };

        auto segments_overlap = [](coord_t alow, coord_t ahigh, coord_t blow, coord_t bhigh) {
            return (alow >= blow && alow <= bhigh) || (ahigh >= blow && ahigh <= bhigh) || (blow >= alow && blow <= ahigh) ||
                   (bhigh >= alow && bhigh <= ahigh);
        };

        Polygons expanded_bridged_area{};
        double   aligning_angle = -bridging_angle + PI * 0.5;
        {
            polygons_rotate(bridged_area, aligning_angle);
            lines_rotate(anchors, cos(aligning_angle), sin(aligning_angle));
            BoundingBox bb_x = get_extents(bridged_area);
            BoundingBox bb_y = get_extents(anchors);

            const size_t n_vlines = (bb_x.max.x() - bb_x.min.x() + scan_spacing - 1) / scan_spacing;
            std::vector<Line> vertical_lines(n_vlines);
            for (size_t i = 0; i < n_vlines; i++) {
                coord_t x           = bb_x.min.x() + (i + 0.5) * scan_spacing;
                coord_t y_min       = bb_y.min.y() - bridging_flow.scaled_spacing();
                coord_t y_max       = bb_y.max.y() + bridging_flow.scaled_spacing();
                vertical_lines[i].a = Point{x, y_min};
                vertical_lines[i].b = Point{x, y_max};
            }

            auto anchors_and_walls_tree = AABBTreeLines::LinesDistancer<Line>{std::move(anchors)};
            auto bridged_area_tree      = AABBTreeLines::LinesDistancer<Line>{to_lines(bridged_area)};

            std::vector<std::vector<Line>> polygon_sections(n_vlines);
            for (size_t i = 0; i < n_vlines; i++) {
                auto area_intersections = bridged_area_tree.intersections_with_line<true>(vertical_lines[i]);
                for (int intersection_idx = 0; intersection_idx < int(area_intersections.size()) - 1; intersection_idx++) {
                    if (bridged_area_tree.outside(
                            (area_intersections[intersection_idx].first + area_intersections[intersection_idx + 1].first) / 2) < 0) {
                        polygon_sections[i].emplace_back(area_intersections[intersection_idx].first,
                                                         area_intersections[intersection_idx + 1].first);
                    }
                }
                auto anchors_intersections = anchors_and_walls_tree.intersections_with_line<true>(vertical_lines[i]);

                for (Line &section : polygon_sections[i]) {
                    const coord_t overlap = restore_anchors ? bridging_flow.scaled_width() + SCALED_EPSILON : 0;
                    auto maybe_below_anchor = std::upper_bound(anchors_intersections.rbegin(), anchors_intersections.rend(), section.a + Point{0, overlap},
                                                               [](const Point &a, const std::pair<Point, size_t> &b) {
                                                                   return a.y() > b.first.y();
                                                               });
                    if (maybe_below_anchor != anchors_intersections.rend()) {
                        section.a = maybe_below_anchor->first;
                        section.a.y() -= bridging_flow.scaled_width() * (0.5 + 0.5);
                    }

                    auto maybe_upper_anchor = std::upper_bound(anchors_intersections.begin(), anchors_intersections.end(), section.b - Point{0, overlap},
                                                               [](const Point &a, const std::pair<Point, size_t> &b) {
                                                                   return a.y() < b.first.y();
                                                               });
                    if (maybe_upper_anchor != anchors_intersections.end()) {
                        section.b = maybe_upper_anchor->first;
                        section.b.y() += bridging_flow.scaled_width() * (0.5 + 0.5);
                    }
                }

                for (int section_idx = 0; section_idx < int(polygon_sections[i].size()) - 1; section_idx++) {
                    Line &section_a = polygon_sections[i][section_idx];
                    Line &section_b = polygon_sections[i][section_idx + 1];
                    if (segments_overlap(section_a.a.y(), section_a.b.y(), section_b.a.y(), section_b.b.y())) {
                        section_b.a = section_a.a.y() < section_b.a.y() ? section_a.a : section_b.a;
                        section_b.b = section_a.b.y() < section_b.b.y() ? section_b.b : section_a.b;
                        section_a.a = section_a.b;
                    }
                }

                polygon_sections[i].erase(std::remove_if(polygon_sections[i].begin(), polygon_sections[i].end(),
                                                         [](const Line &s) { return s.a == s.b; }),
                                          polygon_sections[i].end());
                std::sort(polygon_sections[i].begin(), polygon_sections[i].end(),
                          [](const Line &a, const Line &b) {
                              if (a == b) return false;
                              return a.a.y() < b.b.y();
                          });
            }

            struct TracedPoly
            {
                Points lows;
                Points highs;
            };

            std::vector<TracedPoly> current_traced_polys;
            for (const auto &polygon_slice : polygon_sections) {
                std::unordered_set<const Line *> used_segments;
                for (TracedPoly &traced_poly : current_traced_polys) {
                    auto candidates_begin = std::upper_bound(polygon_slice.begin(), polygon_slice.end(), traced_poly.lows.back(),
                                                             [](const Point &low, const Line &seg) { return seg.b.y() > low.y(); });
                    auto candidates_end   = std::upper_bound(polygon_slice.begin(), polygon_slice.end(), traced_poly.highs.back(),
                                                             [](const Point &high, const Line &seg) { return seg.a.y() > high.y(); });

                    bool segment_added = false;
                    for (auto candidate = candidates_begin; candidate != candidates_end && !segment_added; candidate++) {
                        if (used_segments.find(&(*candidate)) != used_segments.end()) {
                            continue;
                        }

                        if ((traced_poly.lows.back() - candidate->a).cast<double>().squaredNorm() <
                            36.0 * double(bridging_flow.scaled_spacing()) * bridging_flow.scaled_spacing()) {
                            traced_poly.lows.push_back(candidate->a);
                        } else {
                            traced_poly.lows.push_back(traced_poly.lows.back() + Point{scan_spacing / 2, 0});
                            traced_poly.lows.push_back(candidate->a - Point{scan_spacing / 2, 0});
                            traced_poly.lows.push_back(candidate->a);
                        }

                        if ((traced_poly.highs.back() - candidate->b).cast<double>().squaredNorm() <
                            36.0 * double(bridging_flow.scaled_spacing()) * bridging_flow.scaled_spacing()) {
                            traced_poly.highs.push_back(candidate->b);
                        } else {
                            traced_poly.highs.push_back(traced_poly.highs.back() + Point{scan_spacing / 2, 0});
                            traced_poly.highs.push_back(candidate->b - Point{scan_spacing / 2, 0});
                            traced_poly.highs.push_back(candidate->b);
                        }
                        segment_added = true;
                        used_segments.insert(&(*candidate));
                    }

                    if (!segment_added) {
                        traced_poly.lows.push_back(traced_poly.lows.back() + Point{scan_spacing / 2, 0});
                        traced_poly.highs.push_back(traced_poly.highs.back() + Point{scan_spacing / 2, 0});
                        Polygon &new_poly = expanded_bridged_area.emplace_back(std::move(traced_poly.lows));
                        new_poly.points.insert(new_poly.points.end(), traced_poly.highs.rbegin(), traced_poly.highs.rend());
                        traced_poly.lows.clear();
                        traced_poly.highs.clear();
                    }
                }

                current_traced_polys.erase(std::remove_if(current_traced_polys.begin(), current_traced_polys.end(),
                                                          [](const TracedPoly &tp) { return tp.lows.empty(); }),
                                           current_traced_polys.end());

                for (const auto &segment : polygon_slice) {
                    if (used_segments.find(&segment) == used_segments.end()) {
                        TracedPoly &new_tp = current_traced_polys.emplace_back();
                        new_tp.lows.push_back(segment.a - Point{scan_spacing / 2, 0});
                        new_tp.lows.push_back(segment.a);
                        new_tp.highs.push_back(segment.b - Point{scan_spacing / 2, 0});
                        new_tp.highs.push_back(segment.b);
                    }
                }
            }

            for (TracedPoly &traced_poly : current_traced_polys) {
                Polygon &new_poly = expanded_bridged_area.emplace_back(std::move(traced_poly.lows));
                new_poly.points.insert(new_poly.points.end(), traced_poly.highs.rbegin(), traced_poly.highs.rend());
            }
            expanded_bridged_area = union_safety_offset(expanded_bridged_area);
        }

        polygons_rotate(expanded_bridged_area, -aligning_angle);
        return expanded_bridged_area;
    };

    tbb::parallel_for(tbb::blocked_range<size_t>(0, clustered_layers_for_threads.size()), [po = static_cast<const PrintObject *>(this),
                                                                                           target_flow_height_factor, &surfaces_by_layer,
                                                                                           &clustered_layers_for_threads,
                                                                                           gather_areas_w_depth, &infill_lines,
                                                                                           determine_bridging_angle,
                                                                                           construct_anchored_polygon](
                                                                                              tbb::blocked_range<size_t> r) {
        PRINT_OBJECT_TIME_LIMIT_MILLIS(PRINT_OBJECT_TIME_LIMIT_DEFAULT);
        for (size_t cluster_idx = r.begin(); cluster_idx < r.end(); cluster_idx++) {
            for (size_t job_idx = 0; job_idx < clustered_layers_for_threads[cluster_idx].size(); job_idx++) {
                size_t       lidx  = clustered_layers_for_threads[cluster_idx][job_idx];
                const Layer *layer = po->get_layer(lidx);
                std::sort(surfaces_by_layer[lidx].begin(), surfaces_by_layer[lidx].end(),
                          [](const CandidateSurface &left, const CandidateSurface &right) {
                              auto a = get_extents(left.new_polys);
                              auto b = get_extents(right.new_polys);

                              if (a.min.x() == b.min.x()) {
                                  return a.min.y() < b.min.y();
                              };
                              return a.min.x() < b.min.x();
                          });
                if (surfaces_by_layer[lidx].size() > 2) {
                    Vec2d origin = get_extents(surfaces_by_layer[lidx].front().new_polys).max.cast<double>();
                    std::stable_sort(surfaces_by_layer[lidx].begin() + 1, surfaces_by_layer[lidx].end(),
                                     [origin](const CandidateSurface &left, const CandidateSurface &right) {
                                         auto a = get_extents(left.new_polys);
                                         auto b = get_extents(right.new_polys);

                                         return (origin - a.min.cast<double>()).squaredNorm() <
                                                (origin - b.min.cast<double>()).squaredNorm();
                                     });
                }

                coordf_t spacing            = surfaces_by_layer[lidx].front().region->bridging_flow(frSolidInfill, true).scaled_spacing();
                coordf_t target_flow_height = surfaces_by_layer[lidx].front().region->bridging_flow(frSolidInfill, true).height() *
                                              target_flow_height_factor;
                Polygons deep_infill_area = gather_areas_w_depth(po, lidx, target_flow_height);

                {
                    Polygons filled_polyons_on_lower_layers;
                    double   bottom_z = layer->print_z - target_flow_height - EPSILON;
                    if (job_idx > 0) {
                        for (int lower_job_idx = job_idx - 1; lower_job_idx >= 0; lower_job_idx--) {
                            size_t       lower_layer_idx = clustered_layers_for_threads[cluster_idx][lower_job_idx];
                            const Layer *lower_layer     = po->get_layer(lower_layer_idx);
                            if (lower_layer->print_z >= bottom_z) {
                                for (const auto &c : surfaces_by_layer[lower_layer_idx]) {
                                    filled_polyons_on_lower_layers.insert(filled_polyons_on_lower_layers.end(), c.new_polys.begin(),
                                                                          c.new_polys.end());
                                }
                            } else {
                                break;
                            }
                        }
                    }
                    deep_infill_area = diff(deep_infill_area, filled_polyons_on_lower_layers);
                }

                deep_infill_area = expand(deep_infill_area, spacing * 1.5);

                Polygons lightning_area;
                Polygons expansion_area;
                Polygons total_fill_area;
                Polygons total_top_area;
                for (const LayerRegion *region : layer->regions()) {
                    Polygons top_polys = to_polygons(region->fill_surfaces.filter_by_types({stTop}));
                    total_top_area.insert(total_top_area.end(), top_polys.begin(), top_polys.end());
                    Polygons internal_polys = to_polygons(region->fill_surfaces.filter_by_types({stInternal, stInternalSolid}));
                    expansion_area.insert(expansion_area.end(), internal_polys.begin(), internal_polys.end());
                    Polygons fill_polys = to_polygons(region->fill_expolygons);
                    total_fill_area.insert(total_fill_area.end(), fill_polys.begin(), fill_polys.end());
                    if (region->region().config().sparse_infill_pattern == ipLightning) {
                        Polygons l = to_polygons(region->fill_surfaces.filter_by_type(stInternal));
                        lightning_area.insert(lightning_area.end(), l.begin(), l.end());
                    }
                }
                total_fill_area   = closing(total_fill_area, float(SCALED_EPSILON));
                expansion_area    = closing(expansion_area, float(SCALED_EPSILON));
                expansion_area    = intersection(expansion_area, deep_infill_area);
                const Polylines anchors = intersection_pl(infill_lines[lidx - 1], shrink(expansion_area, spacing));
                Polygons internal_unsupported_area = shrink(deep_infill_area, spacing * 4.5);

#ifdef DEBUG_BRIDGE_OVER_INFILL
                debug_draw(std::to_string(lidx) + "_" + std::to_string(cluster_idx) + "_" + std::to_string(job_idx) + "_" + "_total_area",
                           to_lines(total_fill_area), to_lines(expansion_area), to_lines(deep_infill_area), to_lines(anchors));
#endif

                std::vector<CandidateSurface> expanded_surfaces;
                expanded_surfaces.reserve(surfaces_by_layer[lidx].size());
                for (const CandidateSurface &candidate : surfaces_by_layer[lidx]) {
                    const auto &region_config = candidate.region->region().config();
                    const bool turning_pattern = region_config.sparse_infill_pattern == ipHilbertCurve ||
                                                 region_config.sparse_infill_pattern == ipOctagramSpiral;
                    const Flow &flow              = candidate.region->bridging_flow(frSolidInfill, true);
                    ExPolygons bridge_components = intersection_ex(expand(candidate.new_polys, flow.scaled_spacing()), deep_infill_area);
                    bridge_components.erase(std::remove_if(bridge_components.begin(), bridge_components.end(),
                                                           [&internal_unsupported_area](const ExPolygon &component) {
                                                               return intersection_ex(component, internal_unsupported_area).empty();
                                                           }),
                                            bridge_components.end());
                    Polygons area_to_be_bridge = to_polygons(std::move(bridge_components));

                    Polygons limiting_area = union_(area_to_be_bridge, expansion_area);

                    if (area_to_be_bridge.empty())
                        continue;

                    Polylines boundary_plines = to_polylines(expand(total_fill_area, 1.3f * flow.scaled_spacing()));
                    {
                        Polylines limiting_plines = to_polylines(expand(limiting_area, 0.3f * flow.scaled_spacing()));
                        boundary_plines.insert(boundary_plines.end(), limiting_plines.begin(), limiting_plines.end());
                    }

#ifdef DEBUG_BRIDGE_OVER_INFILL
                    int r = rand();
                    debug_draw(std::to_string(lidx) + "_" + std::to_string(cluster_idx) + "_" + std::to_string(job_idx) + "_" +
                                   "_anchors_" + std::to_string(r),
                               to_lines(area_to_be_bridge), to_lines(boundary_plines), to_lines(anchors), to_lines(expansion_area));
#endif

                    double bridging_angle = -1.;
                    if (!anchors.empty() && turning_pattern) {
                        for (const LayerRegion *lower_region : layer->lower_layer->regions()) {
                            if (&lower_region->region() != &candidate.region->region() ||
                                intersection(area_to_be_bridge, to_polygons(lower_region->fill_surfaces.filter_by_type(stInternal))).empty())
                                continue;
                            bridging_angle = calculate_infill_rotation_angle(po, layer->lower_layer->id(), region_config.infill_direction.value,
                                                                            region_config.sparse_infill_rotate_template.value) + 0.5 * PI;
                            if (region_config.align_infill_direction_to_model) {
                                const auto &m = po->trafo().matrix();
                                bridging_angle += std::atan2(double(m(1, 0)), double(m(0, 0)));
                            }
                            bridging_angle = std::fmod(bridging_angle, PI);
                            if (bridging_angle < 0.)
                                bridging_angle += PI;
                            break;
                        }
                    }
                    if (bridging_angle < 0.)
                        bridging_angle = determine_bridging_angle(area_to_be_bridge, to_lines(anchors.empty() ? boundary_plines : anchors));
                    
                    if (candidate.region->region().config().internal_bridge_angle.value > 0) {
                        const double custom_angle_rad   = Geometry::deg2rad(region_config.internal_bridge_angle.value);
                        if (region_config.relative_bridge_angle.value)
                            bridging_angle += custom_angle_rad;
                        else {
                            bridging_angle = custom_angle_rad;
                            if (region_config.align_infill_direction_to_model) {
                                auto m = po->trafo().matrix();
                                bridging_angle += std::atan2((double)m(1, 0), (double)m(0, 0));
                            }
                        }
                    }

                    boundary_plines.insert(boundary_plines.end(), anchors.begin(), anchors.end());
                    if (!lightning_area.empty() && !intersection(area_to_be_bridge, lightning_area).empty()) {
                        boundary_plines = intersection_pl(boundary_plines, expand(area_to_be_bridge, scale_(10)));
                    }
                    const coord_t scan_spacing = std::max(coord_t(1), flow.scaled_spacing() / (turning_pattern ? 4 : 1));
                    Polygons bridging_area = construct_anchored_polygon(area_to_be_bridge, to_lines(boundary_plines), flow, bridging_angle, scan_spacing);

                    {
                        bool     reconstruct       = false;
                        Polygons tmp_expanded_area = expand(bridging_area, 3.0f * flow.scaled_spacing());
                        for (const CandidateSurface &s : expanded_surfaces) {
                            if (!intersection(s.new_polys, tmp_expanded_area).empty()) {
                                bridging_angle = s.bridge_angle;
                                reconstruct    = true;
                                break;
                            }
                        }
                        if (reconstruct) {
                            bridging_area = construct_anchored_polygon(area_to_be_bridge, to_lines(boundary_plines), flow, bridging_angle, scan_spacing);
                        }
                    }

                    bridging_area          = opening(bridging_area, flow.scaled_spacing() * 0.75f);
                    bridging_area          = closing(bridging_area, flow.scaled_spacing());
                    if (turning_pattern && !bridging_area.empty()) {
                        bridging_area = union_(bridging_area, construct_anchored_polygon(bridging_area, to_lines(boundary_plines), flow,
                                                                                       bridging_angle, scan_spacing, true));
                    }
                    bridging_area          = intersection(bridging_area, limiting_area);
                    bridging_area          = intersection(bridging_area, total_fill_area);
                    bridging_area          = diff(bridging_area, total_top_area);
                    expansion_area         = diff(expansion_area, bridging_area);

#ifdef DEBUG_BRIDGE_OVER_INFILL
                    debug_draw(std::to_string(lidx) + "_" + std::to_string(cluster_idx) + "_" + std::to_string(job_idx) + "_" + "_expanded_bridging" +  std::to_string(r),
                               to_lines(layer->lslices), to_lines(boundary_plines), to_lines(candidate.new_polys), to_lines(bridging_area));
#endif

                    expanded_surfaces.push_back(CandidateSurface(candidate.original_surface, candidate.layer_index, bridging_area,
                                                                 candidate.region, bridging_angle));
                }
                surfaces_by_layer[lidx].swap(expanded_surfaces);
                expanded_surfaces.clear();
            }
        }
    });

    BOOST_LOG_TRIVIAL(info) << "Bridge over infill - Directions and expanded surfaces computed" << log_memory_info();

    tbb::parallel_for(tbb::blocked_range<size_t>(0, this->layers().size()), [po = this, &surfaces_by_layer](tbb::blocked_range<size_t> r) {
        PRINT_OBJECT_TIME_LIMIT_MILLIS(PRINT_OBJECT_TIME_LIMIT_DEFAULT);
        for (size_t lidx = r.begin(); lidx < r.end(); lidx++) {
            if (surfaces_by_layer.find(lidx) == surfaces_by_layer.end() && surfaces_by_layer.find(lidx + 1) == surfaces_by_layer.end())
                continue;
            Layer *layer = po->get_layer(lidx);

            Polygons cut_from_infill{};
            if (surfaces_by_layer.find(lidx) != surfaces_by_layer.end()) {
                for (const auto &surface : surfaces_by_layer.at(lidx)) {
                    cut_from_infill.insert(cut_from_infill.end(), surface.new_polys.begin(), surface.new_polys.end());
                }
            }

            Polygons additional_ensuring_areas{};
            if (surfaces_by_layer.find(lidx + 1) != surfaces_by_layer.end()) {
                for (const auto &surface : surfaces_by_layer.at(lidx + 1)) {
                    auto additional_area = diff(surface.new_polys,
                                                shrink(surface.new_polys, surface.region->flow(frSolidInfill).scaled_spacing()));
                    additional_ensuring_areas.insert(additional_ensuring_areas.end(), additional_area.begin(), additional_area.end());
                }
            }

            for (LayerRegion *region : layer->regions()) {
                Surfaces new_surfaces;

                Polygons near_perimeters = to_polygons(union_safety_offset_ex(to_polygons(region->fill_surfaces.surfaces)));
                near_perimeters          = diff(near_perimeters, shrink(near_perimeters, region->flow(frSolidInfill).scaled_spacing()));
                ExPolygons additional_ensuring = intersection_ex(additional_ensuring_areas, near_perimeters);

                SurfacesPtr internal_infills = region->fill_surfaces.filter_by_type(stInternal);
                ExPolygons new_internal_infills = diff_ex(internal_infills, cut_from_infill);
                new_internal_infills            = diff_ex(new_internal_infills, additional_ensuring);
                for (const ExPolygon &ep : new_internal_infills) {
                    new_surfaces.emplace_back(stInternal, ep);
                }

                SurfacesPtr internal_solids = region->fill_surfaces.filter_by_type(stInternalSolid);
                if (surfaces_by_layer.find(lidx) != surfaces_by_layer.end()) {
                    for (const CandidateSurface &cs : surfaces_by_layer.at(lidx)) {
                        for (const Surface *surface : internal_solids) {
                            if (cs.original_surface == surface) {
                                Surface tmp{*surface, {}};
                                tmp.surface_type = stInternalBridge;
                                tmp.bridge_angle = cs.bridge_angle;
                                for (const ExPolygon &ep : union_ex(cs.new_polys)) {
                                    new_surfaces.emplace_back(tmp, ep);
                                }
                                break;
                            }
                        }
                    }
                }
                ExPolygons new_internal_solids = to_expolygons(internal_solids);
                new_internal_solids.insert(new_internal_solids.end(), additional_ensuring.begin(), additional_ensuring.end());
                new_internal_solids = diff_ex(new_internal_solids, cut_from_infill);
                new_internal_solids = union_safety_offset_ex(new_internal_solids);
                for (const ExPolygon &ep : new_internal_solids) {
                    new_surfaces.emplace_back(stInternalSolid, ep);
                }
                
#ifdef DEBUG_BRIDGE_OVER_INFILL
                debug_draw("Aensuring_" + std::to_string(reinterpret_cast<uint64_t>(&region)), to_polylines(additional_ensuring),
                           to_polylines(near_perimeters), to_polylines(to_polygons(internal_infills)),
                           to_polylines(to_polygons(internal_solids)));
                debug_draw("Aensuring_" + std::to_string(reinterpret_cast<uint64_t>(&region)) + "_new", to_polylines(additional_ensuring),
                           to_polylines(near_perimeters), to_polylines(to_polygons(new_internal_infills)),
                           to_polylines(to_polygons(new_internal_solids)));
#endif

                region->fill_surfaces.remove_types({stInternalSolid, stInternal});
                region->fill_surfaces.append(new_surfaces);
            }
        }
    });
    
    if ( this->m_config.enable_extra_bridge_layer == eblApplyToAll || this->m_config.enable_extra_bridge_layer == eblInternalBridgeOnly) {
        struct LayerBridgeCache {
            ExPolygons polys;
            double     angle = 0.0;
            float      offset_distance = 0.0f;
            bool       has_bridge = false;
        };
        const size_t last = (this->layers().size() < 2) ? 0 : this->layers().size() - 1;
        std::vector<LayerBridgeCache> caches(this->layers().size());

        tbb::parallel_for( tbb::blocked_range<size_t>(0, last), [this, &caches](const tbb::blocked_range<size_t>& r) {
            for (size_t lidx = r.begin(); lidx < r.end(); ++lidx) {
                Layer* layer = this->get_layer(lidx);
                LayerBridgeCache &cache = caches[lidx];

                if (!layer->regions().empty())
                
                    cache.offset_distance = layer->regions().front()->flow(frSolidInfill).scaled_width();
                for (LayerRegion *region : layer->regions()) {
                    for (const Surface &surf : region->fill_surfaces.surfaces) {
                        if (surf.surface_type == stInternalBridge) {
                            cache.polys.push_back(surf.expolygon);
                            cache.angle = surf.bridge_angle;
                            cache.has_bridge = true;
                        }
                    }
                }

                if (!cache.has_bridge || cache.polys.empty()) {
                    cache.has_bridge = false;
                    continue;
                }

                cache.polys = offset_ex(shrink_ex(cache.polys, cache.offset_distance), cache.offset_distance);
                if (cache.polys.empty())
                    cache.has_bridge = false;
            }
        });

        tbb::parallel_for( tbb::blocked_range<size_t>(0, last), [this, &caches](const tbb::blocked_range<size_t>& r) {
            for (size_t lidx = r.begin(); lidx < r.end(); ++lidx)
            {
                const LayerBridgeCache &cache = caches[lidx];

                if (!cache.has_bridge || cache.polys.empty())
                    continue;

                if (lidx + 1 < this->layers().size()) {
                    Layer* next_layer = this->get_layer(lidx + 1);
                    
                    
                    double bridging_angle_second = cache.angle + M_PI / 2.0;
                    ExPolygons bridging_union = union_safety_offset_ex(cache.polys);
                    const float offset_distance = cache.offset_distance;

                    for (LayerRegion *next_region : next_layer->regions()) {
                        Surfaces next_new_surfaces;
                        Surfaces keep_surfaces;
                        
                        for (const Surface &s : next_region->fill_surfaces.surfaces) {
                            if ( (s.surface_type != stInternal) &&  (s.surface_type != stInternalSolid)) {
                                keep_surfaces.push_back(s);
                            }
                        }
                        
                        SurfacesPtr next_internals = next_region->fill_surfaces.filter_by_types({ stInternal, stInternalSolid });
                        
                        for (const Surface *s : next_internals) {
                            ExPolygons overlap = intersection_ex( s->expolygon, bridging_union, ApplySafetyOffset::Yes );
                            
                            overlap = offset_ex(shrink_ex(overlap, offset_distance), offset_distance);
                            
                            if (!overlap.empty()) {
                                Surface tmp{*s, {}};
                                tmp.surface_type = stSecondInternalBridge;
                                tmp.bridge_angle = bridging_angle_second;
                                
                                for (const ExPolygon &ep : overlap) {
                                    next_new_surfaces.emplace_back(tmp, ep);
                                }
                                
                                ExPolygons leftover = diff_ex(s->expolygon, bridging_union, ApplySafetyOffset::Yes);
                                leftover = offset_ex(shrink_ex(leftover, offset_distance), offset_distance);
                                
                                if (!leftover.empty()) {
                                    ExPolygons unified_leftover = union_safety_offset_ex(leftover);
                                    for (const ExPolygon &ep : unified_leftover) {
                                        Surface leftover_surf{*s, {}};
                                        leftover_surf.surface_type = s->surface_type;
                                        leftover_surf.bridge_angle = s->bridge_angle;
                                        next_new_surfaces.emplace_back(leftover_surf, ep);
                                    }
                                }
                            }
                            else {
                                keep_surfaces.push_back(*s);
                            }
                        }
                        
                        next_region->fill_surfaces.surfaces.clear();
                        next_region->fill_surfaces.append(keep_surfaces);
                        next_region->fill_surfaces.append(next_new_surfaces);
                    }
                }
            }
        });
        for (size_t lidx = 0; lidx < this->layers().size(); ++lidx) {
            Layer* layer = this->get_layer(lidx);
            for (LayerRegion* region : layer->regions()) {
                for (Surface &surf : region->fill_surfaces.surfaces) {
                    if (surf.surface_type == stSecondInternalBridge) {
                        surf.surface_type = stInternalBridge;
                    }
                }
            }
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Bridge over infill - End" << log_memory_info();

}

static void clamp_exturder_to_default(ConfigOptionInt &opt, size_t num_extruders)
{
    if (opt.value > (int)num_extruders)
        opt.value = 1;
}

static void clamp_feature_filament_to_valid(ConfigOptionInt &opt, size_t num_extruders)
{
    if (opt.value <= 0 || opt.value > (int)num_extruders)
        opt.value = 1;
}

static bool auto_single_curved_top(const ModelObject &object)
{
    if (object.instances.empty())
        return false;
    constexpr double cell = 0.5;
    struct Facet { Vec3d a, b, c; double nz; };
    std::vector<Facet> facets;
    BoundingBoxf box;
    const Transform3d instance = object.instances.front()->get_matrix();
    for (const ModelVolume *volume : object.volumes) {
        if (!volume->is_model_part())
            continue;
        const Transform3d m = instance * volume->get_matrix();
        const indexed_triangle_set &its = volume->mesh().its;
        for (const stl_triangle_vertex_indices &f : its.indices) {
            const Vec3d a = m * its.vertices[f[0]].cast<double>(), b = m * its.vertices[f[1]].cast<double>(),
                        c = m * its.vertices[f[2]].cast<double>();
            const Vec3d n = (b - a).cross(c - a);
            if (n.norm() <= 0.)
                continue;
            facets.push_back({ a, b, c, n.z() / n.norm() });
            for (const Vec3d &v : { a, b, c }) box.merge(Vec2d(v.head<2>()));
        }
    }
    if (facets.empty() || !box.defined)
        return false;
    const int nx = int(std::ceil(box.size().x() / cell)) + 1, ny = int(std::ceil(box.size().y() / cell)) + 1;
    if (nx <= 1 || ny <= 1 || size_t(nx) * size_t(ny) > 4000000)
        return false;
    std::vector<uint8_t> footprint(size_t(nx) * ny, 0);
    std::vector<float> top_z(size_t(nx) * ny, -FLT_MAX), low_z(size_t(nx) * ny, FLT_MAX), top_nz(size_t(nx) * ny, 1.f);
    for (const Facet &f : facets) {
        const Vec2d a = f.a.head<2>(), b = f.b.head<2>(), c = f.c.head<2>();
        const double det = (b - a).x() * (c - a).y() - (b - a).y() * (c - a).x();
        if (std::abs(det) < 1e-12)
            continue;
        const int x0 = std::max(0, int((std::min({ a.x(), b.x(), c.x() }) - box.min.x()) / cell)),
                  x1 = std::min(nx - 1, int((std::max({ a.x(), b.x(), c.x() }) - box.min.x()) / cell) + 1),
                  y0 = std::max(0, int((std::min({ a.y(), b.y(), c.y() }) - box.min.y()) / cell)),
                  y1 = std::min(ny - 1, int((std::max({ a.y(), b.y(), c.y() }) - box.min.y()) / cell) + 1);
        for (int iy = y0; iy <= y1; ++iy)
            for (int ix = x0; ix <= x1; ++ix) {
                const Vec2d p = box.min + Vec2d(ix + 0.5, iy + 0.5) * cell;
                const double u = ((p - a).x() * (c - a).y() - (p - a).y() * (c - a).x()) / det,
                             v = ((b - a).x() * (p - a).y() - (b - a).y() * (p - a).x()) / det;
                if (u < 0. || v < 0. || u + v > 1.)
                    continue;
                const size_t i = size_t(iy) * nx + ix;
                footprint[i] = 1;
                if (f.nz <= 0.02)
                    continue;
                const double z = (f.a + u * (f.b - f.a) + v * (f.c - f.a)).z();
                if (z > top_z[i]) { top_z[i] = float(z); top_nz[i] = float(f.nz); }
                low_z[i] = std::min(low_z[i], float(z));
            }
    }
    size_t covered = 0, topped = 0, stacked = 0, curved = 0, steep = 0;
    const double flat_nz = std::cos(1. * M_PI / 180.), steep_nz = std::cos(20. * M_PI / 180.);
    for (size_t i = 0; i < footprint.size(); ++i) {
        covered += footprint[i];
        if (top_z[i] == -FLT_MAX)
            continue;
        ++topped;
        stacked += top_z[i] - low_z[i] > 1.f;
        curved += top_nz[i] < flat_nz && top_nz[i] >= steep_nz;
        steep += top_nz[i] < steep_nz;
    }
    const double tops = double(std::max<size_t>(topped, 1));
    BOOST_LOG_TRIVIAL(info) << "Single curved top: " << object.name << " topped " << topped / double(std::max<size_t>(covered, 1))
                            << " (>= 0.9) stacked " << stacked / tops << " (<= 0.02) steep " << steep / tops << " (<= 0.02) curved "
                            << curved / tops << " (>= 0.6)";
    return covered > 0 && topped >= 0.9 * covered && stacked <= 0.02 * topped && steep <= 0.02 * topped && curved >= 0.6 * topped;
}

PrintObjectConfig PrintObject::object_config_from_model_object(const PrintObjectConfig &default_object_config, const ModelObject &object, size_t num_extruders, std::vector<int>& variant_index,
                                                                 const PrintConfig *printer)
{
    PrintObjectConfig config = default_object_config;
    {
        DynamicPrintConfig src_normalized(object.config.get());
        src_normalized.normalize_fdm();
        update_static_print_config_from_dynamic(config, src_normalized, variant_index, print_options_with_variant, 1);
    }
    clamp_exturder_to_default(config.support_filament,           num_extruders);
    clamp_exturder_to_default(config.support_interface_filament, num_extruders);
    if (config.support_style.value == smsNative && printer != nullptr && !Nonplanar::native_support_capable(*printer))
        config.support_style.value = smsDefault;
    if (config.support_style.value == smsNative && config.enable_support.value && config.nonplanar_mode.value == NonplanarMode::Auto)
        config.nonplanar_mode.value = NonplanarMode::Off;
    static const bool legacy = research_switch("NP_AUTO_LEGACY") != nullptr;
    const bool fixed_head = printer != nullptr && printer->nonplanar_kinematics.value == RotaryLayout::None;
    auto morph = [&]() {
        return !config.enable_support.value && !printer->nonplanar_head.value.empty() && !printer->spiral_mode.value &&
               !printer->belt_printer.value && object.volumes.size() == 1 && object.layer_config_ranges.empty() &&
               !object.is_mm_painted() && !Nonplanar::auto_morph_refused(object) && auto_single_curved_top(object);
    };
    if (config.nonplanar_mode.value == NonplanarMode::CurvedTops)
        throw Slic3r::SlicingError(object.name + ": Fast curved tops is now Curved tops (nonplanar_mode = nonplanar_top) with Curved top "
                                                 "layers 1; the setting was not migrated.");
    if (config.nonplanar_mode.value == NonplanarMode::Auto && !legacy && fixed_head)
        config.nonplanar_mode.value = morph() ? NonplanarMode::SmoothMorph : NonplanarMode::NonplanarTop;
    else if (config.nonplanar_mode.value == NonplanarMode::NonplanarTop && fixed_head && Nonplanar::top_picks_morph() && morph())
        config.nonplanar_mode.value = NonplanarMode::SmoothMorph;
    return config;
}

const std::string                                                    key_extruder { "extruder" };
static constexpr const std::initializer_list<const std::string_view> keys_extruders {
    "sparse_infill_filament_id"sv,
    "internal_solid_filament_id"sv,
    "top_surface_filament_id"sv,
    "bottom_surface_filament_id"sv,
    "outer_wall_filament_id"sv,
    "inner_wall_filament_id"sv
};

struct FeatureFilamentOverrideMask
{
    bool sparse_infill_filament_id = false;
    bool internal_solid_filament_id  = false;
    bool top_surface_filament_id     = false;
    bool bottom_surface_filament_id  = false;
    bool outer_wall_filament_id    = false;
    bool inner_wall_filament_id    = false;
};

static void apply_to_print_region_config(PrintRegionConfig &out, const DynamicPrintConfig &in, FeatureFilamentOverrideMask &feature_overrides, std::vector<int>& variant_index)
{
    auto *opt_extruder = in.opt<ConfigOptionInt>(key_extruder);
    int base_extruder = (opt_extruder != nullptr) ? opt_extruder->value : 0;

    for (auto it = in.cbegin(); it != in.cend(); ++ it)
        if (it->first != key_extruder)
            if (ConfigOption* my_opt = out.option(it->first, false); my_opt != nullptr) {
                if (one_of(it->first, keys_extruders)) {
                    int extruder = static_cast<const ConfigOptionInt*>(it->second.get())->value;
                    if (extruder > 0) {
                        my_opt->setInt(extruder);
                        if (it->first == "sparse_infill_filament_id")
                            feature_overrides.sparse_infill_filament_id = true;
                        else if (it->first == "internal_solid_filament_id")
                            feature_overrides.internal_solid_filament_id = true;
                        else if (it->first == "top_surface_filament_id")
                            feature_overrides.top_surface_filament_id = true;
                        else if (it->first == "bottom_surface_filament_id")
                            feature_overrides.bottom_surface_filament_id = true;
                        else if (it->first == "outer_wall_filament_id")
                            feature_overrides.outer_wall_filament_id = true;
                        else if (it->first == "inner_wall_filament_id")
                            feature_overrides.inner_wall_filament_id = true;
                    } else {
                        if (it->first == "sparse_infill_filament_id")
                            feature_overrides.sparse_infill_filament_id = false;
                        else if (it->first == "internal_solid_filament_id")
                            feature_overrides.internal_solid_filament_id = false;
                        else if (it->first == "top_surface_filament_id")
                            feature_overrides.top_surface_filament_id = false;
                        else if (it->first == "bottom_surface_filament_id")
                            feature_overrides.bottom_surface_filament_id = false;
                        else if (it->first == "outer_wall_filament_id")
                            feature_overrides.outer_wall_filament_id = false;
                        else if (it->first == "inner_wall_filament_id")
                            feature_overrides.inner_wall_filament_id = false;
                    }
                } else {
                    if (*my_opt != *(it->second)) {
                        if (my_opt->is_scalar() || variant_index.empty() || (print_options_with_variant.find(it->first) == print_options_with_variant.end()))
                            my_opt->set(it->second.get());
                        else {
                            ConfigOptionVectorBase* opt_vec_src = static_cast<ConfigOptionVectorBase*>(my_opt);
                            const ConfigOptionVectorBase* opt_vec_dest = static_cast<const ConfigOptionVectorBase*>(it->second.get());
                            set_variant_override(*opt_vec_src, *opt_vec_dest, variant_index);
                        }
                    }
                }
            }

    if (base_extruder > 0) {
        if (!feature_overrides.sparse_infill_filament_id)
            out.sparse_infill_filament_id.value = base_extruder;
        if (!feature_overrides.internal_solid_filament_id)
            out.internal_solid_filament_id.value = base_extruder;
        if (!feature_overrides.top_surface_filament_id)
            out.top_surface_filament_id.value = base_extruder;
        if (!feature_overrides.bottom_surface_filament_id)
            out.bottom_surface_filament_id.value = base_extruder;
        if (!feature_overrides.outer_wall_filament_id)
            out.outer_wall_filament_id.value = base_extruder;
        if (!feature_overrides.inner_wall_filament_id)
            out.inner_wall_filament_id.value = base_extruder;
    }
}

PrintRegionConfig region_config_from_model_volume(const PrintRegionConfig &default_or_parent_region_config, const DynamicPrintConfig *layer_range_config, const ModelVolume &volume, size_t num_extruders, std::vector<int>& variant_index)
{
    PrintRegionConfig config = default_or_parent_region_config;
    FeatureFilamentOverrideMask feature_overrides;

    if (volume.is_model_part()) {
        feature_overrides.sparse_infill_filament_id = (config.sparse_infill_filament_id.value > 0);
        feature_overrides.internal_solid_filament_id  = (config.internal_solid_filament_id.value > 0);
        feature_overrides.top_surface_filament_id     = (config.top_surface_filament_id.value > 0);
        feature_overrides.bottom_surface_filament_id  = (config.bottom_surface_filament_id.value > 0);
        feature_overrides.outer_wall_filament_id    = (config.outer_wall_filament_id.value > 0);
        feature_overrides.inner_wall_filament_id    = (config.inner_wall_filament_id.value > 0);
    }

    if (volume.is_model_part()) {
        apply_to_print_region_config(config, volume.get_object()->config.get(), feature_overrides, variant_index);
    } else {
    }
    apply_to_print_region_config(config, volume.config.get(), feature_overrides, variant_index);
    if (! volume.material_id().empty())
        apply_to_print_region_config(config, volume.material()->config.get(), feature_overrides, variant_index);
    if (layer_range_config != nullptr) {
        assert(volume.is_model_part());
    	apply_to_print_region_config(config, *layer_range_config, feature_overrides, variant_index);
    }
    clamp_feature_filament_to_valid(config.sparse_infill_filament_id, num_extruders);
    clamp_feature_filament_to_valid(config.outer_wall_filament_id, num_extruders);
    clamp_feature_filament_to_valid(config.inner_wall_filament_id, num_extruders);
    clamp_feature_filament_to_valid(config.internal_solid_filament_id, num_extruders);
    clamp_feature_filament_to_valid(config.top_surface_filament_id, num_extruders);
    clamp_feature_filament_to_valid(config.bottom_surface_filament_id, num_extruders);
    if (config.sparse_infill_density.value < 0.00011f)
        config.sparse_infill_density.value = 0;
    else
        config.sparse_infill_density.value = std::min(config.sparse_infill_density.value, 100.);
    if (config.fuzzy_skin.value != FuzzySkinType::None && (config.fuzzy_skin_point_distance.value < 0.01 || config.fuzzy_skin_thickness.value < 0.001))
        config.fuzzy_skin.value = FuzzySkinType::None;
    return config;
}

struct POProfiler
{
    uint32_t duration1;
    uint32_t duration2;
};

void PrintObject::generate_support_preview()
{
    POProfiler profiler;

    boost::posix_time::ptime ts1 = boost::posix_time::microsec_clock::local_time();
    this->slice();
    boost::posix_time::ptime ts2 = boost::posix_time::microsec_clock::local_time();
    profiler.duration1 = (ts2 - ts1).total_milliseconds();

    this->generate_support_material();
    boost::posix_time::ptime ts3 = boost::posix_time::microsec_clock::local_time();
    profiler.duration2 = (ts3 - ts2).total_milliseconds();
}

void PrintObject::update_slicing_parameters()
{
    if (!m_slicing_params.valid) {
          coordf_t object_height = this->model_object()->max_z();
          BeltTransformPipeline::BeltFloorParams belt_floor;
          const auto &pcfg = this->print()->config();
          if (pcfg.belt_printer.value) {
              BoundingBoxf3     bb;
              const Transform3d trafo = this->trafo_centered();
              for (const ModelVolume *v : this->model_object()->volumes)
                  if (v->is_model_part())
                      bb.merge(v->mesh().transformed_bounding_box(trafo * v->get_matrix()));
              auto hr = BeltTransformPipeline::compute_belt_height_and_floor(pcfg, bb, object_height);
              object_height = hr.object_height;
              belt_floor    = hr.floor_params;
          }
          m_slicing_params = SlicingParameters::create_from_config(pcfg, m_config, object_height,
                                                                   this->object_extruders(), this->print()->shrinkage_compensation());
          m_slicing_params.belt_floor_shear_factor = belt_floor.shear_factor;
          m_slicing_params.belt_floor_from_axis    = belt_floor.from_axis;
          m_slicing_params.belt_floor_z_shift     = belt_floor.z_shift;
          if (m_belt_floor_z_shift_cache_valid)
              m_slicing_params.belt_floor_z_shift = m_belt_floor_z_shift_cached;
      }
}

SlicingParameters PrintObject::slicing_parameters(const DynamicPrintConfig &full_config, const ModelObject &model_object, float object_max_z, const Vec3d &object_shrinkage_compensation, std::vector<int> variant_index)
{
	PrintConfig         print_config;
	PrintObjectConfig   object_config;
	PrintRegionConfig   default_region_config;
	print_config.apply(full_config, true);
	object_config.apply(full_config, true);
	default_region_config.apply(full_config, true);
	size_t              filament_extruders = print_config.filament_diameter.size();
	object_config = object_config_from_model_object(object_config, model_object, filament_extruders, variant_index, &print_config);

	std::vector<unsigned int> object_extruders;
	for (const ModelVolume* model_volume : model_object.volumes)
		if (model_volume->is_model_part()) {
			PrintRegion::collect_object_printing_extruders(
				print_config,
				region_config_from_model_volume(default_region_config, nullptr, *model_volume, filament_extruders, variant_index),
                object_config.brim_type != btNoBrim && object_config.brim_width > 0.,
				object_extruders);
			for (const std::pair<const t_layer_height_range, ModelConfig> &range_and_config : model_object.layer_config_ranges)
				if (range_and_config.second.has("outer_wall_filament_id") ||
					range_and_config.second.has("inner_wall_filament_id") ||
					range_and_config.second.has("sparse_infill_filament_id") ||
					range_and_config.second.has("internal_solid_filament_id") ||
					range_and_config.second.has("top_surface_filament_id") ||
					range_and_config.second.has("bottom_surface_filament_id"))
					PrintRegion::collect_object_printing_extruders(
						print_config,
						region_config_from_model_volume(default_region_config, &range_and_config.second.get(), *model_volume, filament_extruders, variant_index),
                        object_config.brim_type != btNoBrim && object_config.brim_width > 0.,
						object_extruders);
		}
    sort_remove_duplicates(object_extruders);

    BeltTransformPipeline::BeltFloorParams belt_floor;
    if (object_max_z <= 0.f) {
        BoundingBoxf3 bb = model_object.raw_bounding_box();
        object_max_z = (float)bb.size().z();
        if (print_config.belt_printer.value) {
            if (! model_object.instances.empty()) {
                bb = model_object.instance_bounding_box(0, false);
                const Vec3d off = model_object.instances.front()->get_offset();
                bb.translate(-off.x(), -off.y(), 0.);
            }
            auto hr = BeltTransformPipeline::compute_belt_height_and_floor(print_config, bb, object_max_z);
            object_max_z = (float)hr.object_height;
            belt_floor   = hr.floor_params;
        }
    }
    SlicingParameters params = SlicingParameters::create_from_config(print_config, object_config, object_max_z, object_extruders, object_shrinkage_compensation);
    params.belt_floor_shear_factor = belt_floor.shear_factor;
    params.belt_floor_from_axis    = belt_floor.from_axis;
    params.belt_floor_z_shift     = belt_floor.z_shift;
    return params;
}

std::vector<unsigned int> PrintObject::object_extruders() const
{
    std::vector<unsigned int> extruders;
    extruders.reserve(this->all_regions().size() * 3);

    for (const PrintRegion &region : this->all_regions())
        region.collect_object_printing_extruders(*this->print(), extruders);

    const ModelObject* mo = this->model_object();
    for (const ModelVolume* mv : mo->volumes) {
        std::vector<int> volume_extruders = mv->get_extruders();
        for (int extruder : volume_extruders) {
            assert(extruder > 0);
            extruders.push_back(extruder - 1);
        }
    }
    sort_remove_duplicates(extruders);
    return extruders;
}

bool PrintObject::update_layer_height_profile(const ModelObject &model_object, const SlicingParameters &slicing_parameters, std::vector<coordf_t> &layer_height_profile)
{
    bool updated = false;

    if (layer_height_profile.empty()) {
        layer_height_profile = std::vector<coordf_t>(model_object.layer_height_profile.get());
        updated = true;
    }

    if (!layer_height_profile.empty() &&
        ((layer_height_profile.size() & 1) != 0 ||
            std::abs(layer_height_profile[layer_height_profile.size() - 2] - slicing_parameters.object_print_z_uncompensated_max + slicing_parameters.object_print_z_min) > 1e-3))
        layer_height_profile.clear();

    if (layer_height_profile.empty() || layer_height_profile[1] != slicing_parameters.first_object_layer_height) {
        layer_height_profile = (slicing_parameters.surface_error > 0. || slicing_parameters.overhang_reach > 0.) &&
                model_object.layer_config_ranges.empty() ?
            layer_height_profile_surface_error(slicing_parameters, model_object) :
            layer_height_profile_from_ranges(slicing_parameters, model_object.layer_config_ranges);
        updated = true;
    }

    return updated;
}
void PrintObject::get_certain_layers(float start, float end, std::vector<LayerPtrs> &out, std::vector<BoundingBox> &boundingbox_objects)
{
    BoundingBox temp;
    LayerPtrs   out_temp;
    for (const auto &layer : layers()) {
        if (layer->print_z < start) continue;

        if (layer->print_z > end + EPSILON) break;
        temp.merge(layer->loverhangs_bbox);
        out_temp.emplace_back(layer);
    }
    boundingbox_objects.emplace_back(std::move(temp));
    out.emplace_back(std::move(out_temp));
};

Points PrintObject::get_instances_shift_without_plate_offset() const
{
    Points out;
    out.reserve(m_instances.size());
    for (const auto& instance : m_instances)
        out.push_back(instance.shift_without_plate_offset());

    return out;
}

void PrintObject::clip_fill_surfaces()
{
    if (! PrintObject::infill_only_where_needed)
        return;
    bool has_infill = false;
    for (size_t i = 0; i < this->num_printing_regions(); ++ i)
        if (this->printing_region(i).config().sparse_infill_density > 0) {
            has_infill = true;
            break;
        }
    if (! has_infill)
        return;

    Polygons upper_internal;
    for (int layer_id = int(m_layers.size()) - 1; layer_id > 0; -- layer_id) {
        Layer *layer       = m_layers[layer_id];
        Layer *lower_layer = m_layers[layer_id - 1];
        Polygons fill_surfaces;
        Polygons overhangs;
        for (const LayerRegion *layerm : layer->m_regions)
            for (const Surface &surface : layerm->fill_surfaces.surfaces) {
                Polygons polygons = to_polygons(surface.expolygon);
                if (surface.is_solid())
                    polygons_append(overhangs, polygons);
                polygons_append(fill_surfaces, std::move(polygons));
            }
        Polygons lower_layer_fill_surfaces;
        Polygons lower_layer_internal_surfaces;
        for (const LayerRegion *layerm : lower_layer->m_regions)
            for (const Surface &surface : layerm->fill_surfaces.surfaces) {
                Polygons polygons = to_polygons(surface.expolygon);
                if (surface.surface_type == stInternal || surface.surface_type == stInternalVoid)
                    polygons_append(lower_layer_internal_surfaces, polygons);
                polygons_append(lower_layer_fill_surfaces, std::move(polygons));
            }
        {
            Polygons perimeters = intersection(diff(layer->lslices, fill_surfaces), lower_layer_fill_surfaces);
            float pw = FLT_MAX;
            for (const LayerRegion *layerm : layer->m_regions)
                pw = std::min(pw, (float)layerm->flow(frPerimeter).scaled_width());
            polygons_append(overhangs, opening(perimeters, pw));
        }
        polygons_append(upper_internal, std::move(overhangs));
        const auto closing_radius = scaled<float>(2.f);
        upper_internal = intersection(
            smooth_outward(
                closing(upper_internal, closing_radius, jtSquare, 0.),
                scaled<coord_t>(0.1)),
            lower_layer_internal_surfaces);
        for (LayerRegion *layerm : lower_layer->m_regions) {
            if (layerm->region().config().sparse_infill_density.value == 0)
                continue;
            Polygons internal;
            for (Surface &surface : layerm->fill_surfaces.surfaces)
                if (surface.surface_type == stInternal || surface.surface_type == stInternalVoid)
                    polygons_append(internal, std::move(surface.expolygon));
            layerm->fill_surfaces.remove_types({ stInternal, stInternalVoid });
            layerm->fill_surfaces.append(intersection_ex(internal, upper_internal, ApplySafetyOffset::Yes), stInternal);
            layerm->fill_surfaces.append(diff_ex        (internal, upper_internal, ApplySafetyOffset::Yes), stInternalVoid);
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
            layerm->export_region_fill_surfaces_to_svg_debug("6_clip_fill_surfaces");
#endif
        }
        m_print->throw_if_canceled();
    }
}

void PrintObject::discover_horizontal_shells()
{
    BOOST_LOG_TRIVIAL(trace) << "discover_horizontal_shells()";

    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (size_t i = 0; i < m_layers.size(); ++ i) {
            m_print->throw_if_canceled();
            Layer 					*layer  = m_layers[i];
            LayerRegion             *layerm = layer->regions()[region_id];
            const PrintRegionConfig &region_config = layerm->region().config();

            if (!region_config.extra_solid_infills.value.empty() &&
                check_layer_id_pattern(region_config.extra_solid_infills.value, i)) {
                for (Surface& surface : layerm->fill_surfaces.surfaces)
                    if (surface.surface_type == stInternal)
                        surface.surface_type = stInternalSolid;
            }

            if (region_config.ensure_vertical_shell_thickness.value == evstAll)
                continue;

            coordf_t print_z  = layer->print_z;
            coordf_t bottom_z = layer->bottom_z();
            for (size_t idx_surface_type = 0; idx_surface_type < 3; ++ idx_surface_type) {
                m_print->throw_if_canceled();
                SurfaceType type = (idx_surface_type == 0) ? stTop : (idx_surface_type == 1) ? stBottom : stBottomBridge;
                int num_solid_layers = (type == stTop) ? region_config.top_shell_layers.value : region_config.bottom_shell_layers.value;
                if (num_solid_layers == 0)
                	continue;
                auto of_type = [type](const Surface &surface) {
                    return surface.surface_type == type || (type == stTop && surface.surface_type == stTopNonplanar);
                };
                Polygons solid;
                for (const Surface &surface : layerm->slices.surfaces)
                    if (of_type(surface))
                        polygons_append(solid, to_polygons(surface.expolygon));
                for (const Surface &surface : layerm->fill_surfaces.surfaces)
                    if (of_type(surface))
                        polygons_append(solid, to_polygons(surface.expolygon));
                if (!layer->wave_overhang_shadow_polygons.empty() && !solid.empty())
                    solid = diff(solid, layer->wave_overhang_shadow_polygons);
                if (solid.empty())
                    continue;
                for (int n = (type == stTop) ? int(i) - 1 : int(i) + 1;
                	(type == stTop) ?
                		(n >= 0                   && (int(i) - n < num_solid_layers ||
                								 	  print_z - m_layers[n]->print_z < region_config.top_shell_thickness.value - EPSILON)) :
                		(n < int(m_layers.size()) && (n - int(i) < num_solid_layers ||
                									  m_layers[n]->bottom_z() - bottom_z < region_config.bottom_shell_thickness.value - EPSILON));
                	(type == stTop) ? -- n : ++ n)
                {
                    LayerRegion *neighbor_layerm = m_layers[n]->regions()[region_id];

                    Polygons new_internal_solid;
                    {
                        Polygons internal;
                        for (const Surface &surface : neighbor_layerm->fill_surfaces.surfaces)
                            if (surface.surface_type == stInternal || surface.surface_type == stInternalSolid)
                                polygons_append(internal, to_polygons(surface.expolygon));
                        new_internal_solid = intersection(solid, internal, ApplySafetyOffset::Yes);
                    }
                    if (new_internal_solid.empty()) {
                        if (region_config.sparse_infill_density.value == 0 || region_config.ensure_vertical_shell_thickness.value == evstCriticalOnly || region_config.ensure_vertical_shell_thickness.value == evstNone) {
                            goto EXTERNAL;
                        } else {
                            continue;
                        }
                    }

                    float factor = 0.0f;
                    if (region_config.sparse_infill_density.value == 0)
                        factor = 1.0f;
                    else if (region_config.ensure_vertical_shell_thickness.value == evstNone)
                        factor = 0.5f;
                    else if (region_config.ensure_vertical_shell_thickness.value == evstCriticalOnly)
                        factor = 0.2f;
                    if (factor > 0.0f) {
                        float    margin     = float(neighbor_layerm->flow(frExternalPerimeter).scaled_width()) * factor;
                        Polygons too_narrow = diff(new_internal_solid,
                                                   opening(new_internal_solid, margin, margin + ClipperSafetyOffset, jtMiter, 5));
                        if (!too_narrow.empty())
                            new_internal_solid = solid = diff(new_internal_solid, too_narrow);
                    }

                    {
                        float margin = (region_config.ensure_vertical_shell_thickness.value != evstNone ? 3.f : 1.0f) * layerm->flow(frSolidInfill).scaled_width();
                        Polygons too_narrow = diff(
                            new_internal_solid,
                            opening(new_internal_solid, margin, margin + ClipperSafetyOffset, jtMiter, 5));
                        if (! too_narrow.empty()) {
                            Polygons internal;
                            for (const Surface &surface : neighbor_layerm->fill_surfaces.surfaces)
                                if (surface.is_internal() && !surface.is_bridge())
                                    polygons_append(internal, to_polygons(surface.expolygon));
                            polygons_append(new_internal_solid,
                                intersection(
                                    expand(too_narrow, +margin),
                                    internal));
                        }
                    }

                    SurfaceCollection backup = std::move(neighbor_layerm->fill_surfaces);
                    Surfaces neighbor_nonplanar;
                    for (const Surface &surface : backup.surfaces)
                        if (surface.is_nonplanar())
                            neighbor_nonplanar.push_back(surface);
                    polygons_append(new_internal_solid, to_polygons(backup.filter_by_type(stInternalSolid)));
                    if (! neighbor_nonplanar.empty())
                        new_internal_solid = diff(new_internal_solid, to_polygons(neighbor_nonplanar));
                    ExPolygons internal_solid = union_ex(new_internal_solid);
                    neighbor_layerm->fill_surfaces.set(internal_solid, stInternalSolid);
                    Polygons polygons_internal = to_polygons(std::move(internal_solid));
                    ExPolygons internal = diff_ex(backup.filter_by_type(stInternal), polygons_internal, ApplySafetyOffset::Yes);
                    neighbor_layerm->fill_surfaces.append(internal, stInternal);
                    polygons_append(polygons_internal, to_polygons(std::move(internal)));
                    backup.keep_types({ stTop, stBottom, stBottomBridge });
                    std::vector<SurfacesPtr> top_bottom_groups;
                    backup.group(&top_bottom_groups);
                    for (SurfacesPtr &group : top_bottom_groups)
                        neighbor_layerm->fill_surfaces.append(
                            diff_ex(group, polygons_internal),
                            *group.front());
                    neighbor_layerm->fill_surfaces.append(std::move(neighbor_nonplanar));
                }
		EXTERNAL:;
            }
        }
    }

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id) {
        for (const Layer *layer : m_layers) {
            const LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("5_discover_horizontal_shells");
            layerm->export_region_fill_surfaces_to_svg_debug("5_discover_horizontal_shells");
        }
    }
#endif
}
void PrintObject::combine_infill()
{
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        const PrintRegion &region = this->printing_region(region_id);
        const bool enable_combine_infill = region.config().infill_combination.value ||
                                           (this->config().nonplanar_mode.value == NonplanarMode::Auto &&
                                            m_print->config().nonplanar_kinematics.value == RotaryLayout::None &&
                                            Nonplanar::infill_depth(region.config(), *this) == 0.) ||
                                           (Nonplanar::oneshot_requested(*this) && !this->oneshot_state().field_path() &&
                                            this->oneshot_state().combine_infill());
        if (enable_combine_infill == false || region.config().sparse_infill_density == 0.)
            continue;

        const bool          use_solid_infill = fabs(region.config().sparse_infill_density.value - 100.) < EPSILON;
        const SurfaceType   surface_type     = use_solid_infill ? stInternalSolid : stInternal;
        const InfillPattern infill_pattern   = use_solid_infill ? region.config().internal_solid_infill_pattern :
                                                                  region.config().sparse_infill_pattern;

        double nozzle_diameter = std::min(
            nozzle_diameter_for_filament(this->print()->config(), region.config().sparse_infill_filament_id.value, this->print()->is_BBL_printer()),
            nozzle_diameter_for_filament(this->print()->config(), region.config().internal_solid_filament_id.value, this->print()->is_BBL_printer()));
        
        const double infill_combination_max_layer_height = region.config().infill_combination_max_layer_height.get_abs_value(nozzle_diameter);
        nozzle_diameter = infill_combination_max_layer_height > 0 ? std::min(infill_combination_max_layer_height, nozzle_diameter) : nozzle_diameter;
        
        std::vector<size_t> combine(m_layers.size(), 0);
        {
            double current_height = 0.;
            size_t num_layers = 0;
            for (size_t layer_idx = 0; layer_idx < m_layers.size(); ++ layer_idx) {
                m_print->throw_if_canceled();
                const Layer *layer = m_layers[layer_idx];
                if (layer->id() == 0)
                    continue;
                if (current_height + layer->height >= nozzle_diameter + EPSILON) {
                    combine[layer_idx - 1] = num_layers;
                    current_height = 0.;
                    num_layers = 0;
                }
                current_height += layer->height;
                ++ num_layers;
            }

            combine[m_layers.size() - 1] = num_layers;
        }

        for (size_t layer_idx = 0; layer_idx < m_layers.size(); ++ layer_idx) {
            m_print->throw_if_canceled();
            size_t num_layers = combine[layer_idx];
			if (num_layers <= 1)
                continue;
            std::vector<LayerRegion*> layerms;
            layerms.reserve(num_layers);
			for (size_t i = layer_idx + 1 - num_layers; i <= layer_idx; ++ i)
                layerms.emplace_back(m_layers[i]->regions()[region_id]);
            ExPolygons intersection = to_expolygons(layerms.front()->fill_surfaces.filter_by_type(surface_type));
            for (size_t i = 1; i < layerms.size(); ++ i)
                intersection = intersection_ex(layerms[i]->fill_surfaces.filter_by_type(surface_type), intersection);
            double area_threshold = layerms.front()->infill_area_threshold();
            if (! intersection.empty() && area_threshold > 0.)
                intersection.erase(std::remove_if(intersection.begin(), intersection.end(),
                    [area_threshold](const ExPolygon &expoly) { return expoly.area() <= area_threshold; }),
                    intersection.end());
            if (intersection.empty())
                continue;
            Polygons intersection_with_clearance;
            intersection_with_clearance.reserve(intersection.size());
            float clearance_offset =
                0.5f * layerms.back()->flow(frPerimeter).scaled_width() +
                ((infill_pattern == ipRectilinear   ||
                  infill_pattern == ipMonotonic     ||
                  infill_pattern == ipGrid          ||
                  infill_pattern == ipLateralLattice     ||
                  infill_pattern == ipLine          ||
                  infill_pattern == ipHoneycomb     ||
                  infill_pattern == ipLateralHoneycomb) ? 1.5f : 0.5f) *
                    layerms.back()->flow(frSolidInfill).scaled_width();
            for (ExPolygon &expoly : intersection)
                polygons_append(intersection_with_clearance, offset(expoly, clearance_offset));
            for (LayerRegion *layerm : layerms) {
                Polygons internal = to_polygons(std::move(layerm->fill_surfaces.filter_by_type(surface_type)));
                layerm->fill_surfaces.remove_type(surface_type);
                layerm->fill_surfaces.append(diff_ex(internal, intersection_with_clearance), surface_type);
                if (layerm == layerms.back()) {
                    Surface templ(surface_type, ExPolygon());
                    templ.thickness = 0.;
                    for (LayerRegion *layerm2 : layerms)
                        templ.thickness += layerm2->layer()->height;
                    templ.thickness_layers = (unsigned short)layerms.size();
                    layerm->fill_surfaces.append(intersection, templ);
                } else {
                    layerm->fill_surfaces.append(
                        intersection_ex(internal, intersection_with_clearance),
                        stInternalVoid);
                }
            }
        }
    }
}

void PrintObject::_generate_support_material()
{
    if (Nonplanar::generate_planar_native_support(*this))
        return;
    if (is_tree(m_config.support_type.value)) {
        TreeSupport tree_support(*this, m_slicing_params);
        tree_support.throw_on_cancel = [this]() { this->throw_if_canceled(); };
        tree_support.generate();
    }
    else {
        const double global_z = m_belt_global_z_offset;
        const bool   unshift  = std::abs(global_z) > EPSILON;
        auto shift_object_frame = [this, global_z](double sign) {
            for (Layer *layer : m_layers)
                layer->print_z += sign * global_z;
            m_slicing_params.belt_floor_z_shift += sign * global_z;
        };
        if (unshift)
            shift_object_frame(-1.);
        try {
            PrintObjectSupportMaterial support_material(this, m_slicing_params);
            support_material.generate(*this);
        } catch (...) {
            if (unshift)
                shift_object_frame(1.);
            throw;
        }
        if (unshift) {
            shift_object_frame(1.);
            for (SupportLayer *sl : m_support_layers)
                sl->print_z += global_z;
        }
    }
    if (is_tree(m_config.support_type.value) && std::abs(m_belt_global_z_offset) > EPSILON) {
        auto style = m_config.support_style.value;
        if (style == smsDefault)
            style = tree_default_style();
        if (style == smsTreeOrganic) {
            for (SupportLayer *sl : m_support_layers)
                sl->print_z += m_belt_global_z_offset;
        }
    }

}

#define SUPPORT_SURFACES_OFFSET_PARAMETERS jtSquare, 0.
#define SUPPORT_MATERIAL_MARGIN 1.2
template<typename PolysType>
void PrintObject::remove_bridges_from_contacts(
    const Layer* lower_layer,
    const Layer* current_layer,
    float extrusion_width,
    PolysType* overhang_regions,
    float max_bridge_length,
    bool break_bridge)
{
    float fw = extrusion_width;
    Lines overhang_perimeters = to_lines(*overhang_regions);
    auto layer_regions = current_layer->regions();
    Polygons lower_layer_polygons = to_polygons(lower_layer->lslices);
    const PrintObjectConfig& object_config = current_layer->object()->config();

    Polygons all_bridges;
    for (LayerRegion* layerm : layer_regions)
    {
        Polygons bridges;
        Polygons lower_grown_slices = offset(lower_layer_polygons,
            0.5f * fw, SUPPORT_SURFACES_OFFSET_PARAMETERS);
        Polylines overhang_perimeters = diff_pl(layerm->perimeters.as_polylines(), lower_grown_slices);
        Flow bridge_flow = layerm->bridging_flow(frPerimeter, object_config.thick_bridges);
        float w = float(std::max(bridge_flow.scaled_width(), bridge_flow.scaled_spacing()));
        for (Polyline& polyline : overhang_perimeters)
            if (polyline.is_straight()) {
                polyline.extend_start(fw);
                polyline.extend_end(fw);
                Point pts[2] = { polyline.first_point(), polyline.last_point() };
                bool  supported[2] = { false, false };
                for (size_t i = 0; i < lower_layer->lslices.size() && !(supported[0] && supported[1]); ++i)
                    for (int j = 0; j < 2; ++j)
                        if (!supported[j] && lower_layer->lslices_bboxes[i].contains(pts[j]) && lower_layer->lslices[i].contains(pts[j]))
                            supported[j] = true;
                if (supported[0] && supported[1]) {
                    Polylines lines;
                    if (polyline.length() > max_bridge_length + 10) {
                        if (break_bridge) {
                            float len = polyline.length() / ceil(polyline.length() / max_bridge_length);
                            lines = polyline.equally_spaced_lines(len);
                            for (auto& line : lines) {
                                if (line.is_valid())
                                    line.clip_start(fw);
                                if (line.is_valid())
                                    line.clip_end(fw);
                            }
                        }
                    }
                    else
                        lines.push_back(polyline);
                    polygons_append(bridges, offset(lines, 0.5f * w + 10.f));
                }
            }
        bridges = union_(bridges);

        for (const Surface& surface : layerm->fill_surfaces.surfaces)
            if (surface.surface_type == stBottomBridge && surface.bridge_angle != -1) {
                auto bbox      = get_extents(surface.expolygon);
                auto bbox_size = bbox.size();
                if (bbox_size[0] < max_bridge_length && bbox_size[1] < max_bridge_length)
                    polygons_append(bridges, surface.expolygon);
                else {
                    if (break_bridge) {
                        Polygons holes;
                        coord_t  x0 = bbox.min.x();
                        coord_t  x1 = bbox.max.x();
                        coord_t  y0 = bbox.min.y();
                        coord_t  y1 = bbox.max.y();
                        const int grid_lw = int(w/2);

                        Vec2f bridge_direction{ cos(surface.bridge_angle),sin(surface.bridge_angle) };
                        if (fabs(bridge_direction(0)) > fabs(bridge_direction(1)))
                        {
                            int step = bbox_size(0) / ceil(bbox_size(0) / max_bridge_length);
                            for (int x = x0 + step; x < x1; x += step) {
                                Polygon poly;
                                poly.points = {Point(x - grid_lw, y0), Point(x + grid_lw, y0), Point(x + grid_lw, y1), Point(x - grid_lw, y1)};
                                holes.emplace_back(poly);
                            }
                        } else {
                            int step = bbox_size(1) / ceil(bbox_size(1) / max_bridge_length);
                            for (int y = y0 + step; y < y1; y += step) {
                                Polygon poly;
                                poly.points = {Point(x0, y - grid_lw), Point(x0, y + grid_lw), Point(x1, y + grid_lw), Point(x1, y - grid_lw)};
                                holes.emplace_back(poly);
                            }
                        }
                        auto expoly = diff_ex(surface.expolygon, holes);
                        polygons_append(bridges, expoly);
                    }
                }
            }
        bridges = diff(bridges,
            offset(layerm->unsupported_bridge_edges, scale_(SUPPORT_MATERIAL_MARGIN), SUPPORT_SURFACES_OFFSET_PARAMETERS));
        append(all_bridges, bridges);
    }
    if (typeid(overhang_regions) == typeid(ExPolygons*)) {
        *(ExPolygons*)overhang_regions = diff_ex(*overhang_regions, all_bridges, ApplySafetyOffset::Yes);
    }
    else if (typeid(overhang_regions) == typeid(Polygons*)) {
        *(Polygons*)overhang_regions = diff(*overhang_regions, all_bridges, ApplySafetyOffset::Yes);
    }
}

template void PrintObject::remove_bridges_from_contacts<ExPolygons>(
    const Layer* lower_layer,
    const Layer* current_layer,
    float extrusion_width,
    ExPolygons* overhang_regions,
    float max_bridge_length, bool break_bridge);
template void PrintObject::remove_bridges_from_contacts<Polygons>(
    const Layer* lower_layer,
    const Layer* current_layer,
    float extrusion_width,
    Polygons* overhang_regions,
    float max_bridge_length, bool break_bridge);


SupportNecessaryType PrintObject::is_support_necessary()
{
    const double cantilevel_dist_thresh = scale_(6);

    TreeSupport tree_support(*this, m_slicing_params);
    tree_support.support_type = SupportType::stTreeAuto;
    tree_support.detect_overhangs(true);
    this->clear_support_layers();
    if (tree_support.has_sharp_tails)
        return SharpTail;
    else if (tree_support.has_cantilever && tree_support.max_cantilever_dist > cantilevel_dist_thresh)
        return Cantilever;

    return NoNeedSupp;
}

static void project_triangles_to_slabs(ConstLayerPtrsAdaptor layers, const indexed_triangle_set &custom_facets, const Transform3f &tr, bool seam, std::vector<Polygons> &out)
{
    if (custom_facets.indices.empty())
        return;

    const float tr_det_sign = (tr.matrix().determinant() > 0. ? 1.f : -1.f);

    struct LightPolygon {
        LightPolygon() { pts.reserve(5); }
        LightPolygon(const std::array<Vec2f, 3>& tri) {
            pts.reserve(3);
            pts.emplace_back(scaled<coord_t>(tri.front()));
            pts.emplace_back(scaled<coord_t>(tri[1]));
            pts.emplace_back(scaled<coord_t>(tri.back()));
        }

        Points pts;

        void add(const Vec2f& pt) {
            pts.emplace_back(scaled<coord_t>(pt));
            assert(pts.size() <= 5);
        }
    };

    struct TriangleProjections {
        size_t first_layer_id;
        std::vector<LightPolygon> polygons;
    };

    std::vector<TriangleProjections> projections_of_triangles(custom_facets.indices.size());

    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, custom_facets.indices.size()),
        [&custom_facets, &tr, tr_det_sign, seam, layers, &projections_of_triangles](const tbb::blocked_range<size_t>& range) {
        for (size_t idx = range.begin(); idx < range.end(); ++ idx) {

        std::array<Vec3f, 3> facet;

        for (int i=0; i<3; ++i)
            facet[i] = tr * custom_facets.vertices[custom_facets.indices[idx](i)];

        float z_comp = (facet[1]-facet[0]).cross(facet[2]-facet[0]).z();
        if (! seam && tr_det_sign * z_comp > 0.)
            continue;

        if (seam && z_comp == 0.f)
            facet[0].x() += float(EPSILON);

        std::sort(facet.begin(), facet.end(),
                  [](const Vec3f& pt1, const Vec3f&pt2) {
                      return pt1.z() < pt2.z();
                  });

        std::array<Vec2f, 3> trianglef;
        for (int i=0; i<3; ++i)
            trianglef[i] = to_2d(facet[i]);

        auto it = std::lower_bound(layers.begin(), layers.end(), facet[0].z()+EPSILON,
                      [](const Layer* l1, float z) {
                           return l1->slice_z < z;
                      });

        size_t first_layer_id = projections_of_triangles[idx].first_layer_id = it - layers.begin();
        size_t last_layer_id  = first_layer_id;
        while (last_layer_id + 1 < layers.size()
            && float(layers[last_layer_id]->slice_z) <= facet[2].z())
            ++last_layer_id;

        if (first_layer_id == last_layer_id) {
            float dz = facet[2].z() - facet[0].z();
            assert(dz >= 0);
            bool add_below = dz < float(2. * EPSILON) && first_layer_id > 0 && layers[first_layer_id - 1]->slice_z > facet[0].z() - EPSILON;
            projections_of_triangles[idx].polygons.reserve(add_below ? 2 : 1);
            projections_of_triangles[idx].polygons.emplace_back(trianglef);
            if (add_below) {
                -- projections_of_triangles[idx].first_layer_id;
                projections_of_triangles[idx].polygons.emplace_back(trianglef);
            }
            continue;
        }

        projections_of_triangles[idx].polygons.resize(last_layer_id - first_layer_id + 1);

        Vec2f ta(trianglef[1] - trianglef[0]);
        Vec2f tb(trianglef[2] - trianglef[0]);
        ta *= 1.f/(facet[1].z() - facet[0].z());
        tb *= 1.f/(facet[2].z() - facet[0].z());

        LightPolygon* proj = &projections_of_triangles[idx].polygons[0];
        proj->add(trianglef[0]);

        bool passed_first = false;
        bool stop = false;

        while (it != layers.end()) {
            const float z = float((*it)->slice_z);

            Vec2f a;
            Vec2f b;

            if (z > facet[1].z() && ! passed_first) {
                proj->add(trianglef[1]);
                ta = trianglef[2]-trianglef[1];
                ta *= 1.f/(facet[2].z() - facet[1].z());
                passed_first = true;
            }

            if (z > facet[2].z() || it+1 == layers.end()) {
                proj->add(trianglef[2]);
                stop = true;
            }
            else {
                a = passed_first ? (trianglef[1]+ta*(z-facet[1].z()))
                                 : (trianglef[0]+ta*(z-facet[0].z()));
                b = trianglef[0]+tb*(z-facet[0].z());
                proj->add(a);
                proj->add(b);
            }

           if (stop)
                break;

            ++it;
            ++proj;
            assert(proj <= &projections_of_triangles[idx].polygons.back() );

            proj->add(b);
            proj->add(a);
        }
    }
    });
    out.resize(layers.size());

    for (auto& trg : projections_of_triangles) {
        int layer_id = int(trg.first_layer_id);
        for (LightPolygon &poly : trg.polygons) {
            if (layer_id >= int(out.size()))
                break;
            assert(! poly.pts.empty());

            out[layer_id].emplace_back(std::move(poly.pts));
            ++layer_id;
        }
    }
}

void PrintObject::project_and_append_custom_facets(
        bool seam, EnforcerBlockerType type, std::vector<Polygons>& out, std::vector<std::pair<Vec3f, Vec3f>>* vertical_points) const
{
    const Transform3d object_trafo = this->trafo_sliced();
    for (const ModelVolume* mv : this->model_object()->volumes)
        if (mv->is_model_part()) {
            const indexed_triangle_set custom_facets = seam
                    ? mv->seam_facets.get_facets_strict(*mv, type)
                    : mv->supported_facets.get_facets_strict(*mv, type);
            if (! custom_facets.indices.empty()) {
                if (seam)
                    project_triangles_to_slabs(this->layers(), custom_facets,
                        (object_trafo * mv->get_matrix()).cast<float>(),
                        seam, out);
                else {
                    std::vector<Polygons> projected;
                    slice_mesh_slabs(custom_facets, zs_from_layers(this->layers()), object_trafo * mv->get_matrix(), nullptr, &projected, vertical_points, [](){});
                    assert(! projected.empty());
                    assert(out.empty() || out.size() == projected.size());
                    if (out.empty())
                        out = std::move(projected);
                    else
                        for (size_t i = 0; i < out.size(); ++ i)
                            append(out[i], std::move(projected[i]));
                }
            }
        }
    if (! seam && type == EnforcerBlockerType::BLOCKER && m_conformal &&
        (! m_conformal->self_supported.empty() || ! m_conformal->flat_printed.empty() || ! m_conformal->bridged.empty())) {
        std::vector<Polygons> enforcers = this->slice_support_enforcers();
        this->project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, enforcers);
        out.resize(std::max(out.size(), m_layers.size()));
        for (size_t i = 0; i < out.size(); ++ i) {
            Polygons blocked;
            if (i < m_conformal->self_supported.size())
                polygons_append(blocked, to_polygons(m_conformal->self_supported[i]));
            if (i < m_conformal->flat_printed.size())
                polygons_append(blocked, to_polygons(m_conformal->flat_printed[i]));
            if (i < m_conformal->bridged.size())
                polygons_append(blocked, to_polygons(m_conformal->bridged[i]));
            if (blocked.empty())
                continue;
            polygons_append(out[i], i < enforcers.size() && ! enforcers[i].empty() ?
                                        diff(blocked, expand(enforcers[i], float(scale_(0.3)))) : blocked);
        }
    }
}

const Layer* PrintObject::get_layer_at_printz(coordf_t print_z) const {
    auto it = Slic3r::lower_bound_by_predicate(m_layers.begin(), m_layers.end(), [print_z](const Layer *layer) { return layer->print_z < print_z; });
    return (it == m_layers.end() || (*it)->print_z != print_z) ? nullptr : *it;
}



Layer* PrintObject::get_layer_at_printz(coordf_t print_z) { return const_cast<Layer*>(std::as_const(*this).get_layer_at_printz(print_z)); }



const Layer* PrintObject::get_layer_at_printz(coordf_t print_z, coordf_t epsilon) const {
    coordf_t limit = print_z - epsilon;
    auto it = Slic3r::lower_bound_by_predicate(m_layers.begin(), m_layers.end(), [limit](const Layer *layer) { return layer->print_z < limit; });
    return (it == m_layers.end() || (*it)->print_z > print_z + epsilon) ? nullptr : *it;
}



Layer* PrintObject::get_layer_at_printz(coordf_t print_z, coordf_t epsilon) { return const_cast<Layer*>(std::as_const(*this).get_layer_at_printz(print_z, epsilon)); }

const Layer *PrintObject::get_first_layer_bellow_printz(coordf_t print_z, coordf_t epsilon) const
{
    coordf_t limit = print_z + epsilon;
    auto it = Slic3r::lower_bound_by_predicate(m_layers.begin(), m_layers.end(), [limit](const Layer *layer) { return layer->print_z < limit; });
    return (it == m_layers.begin()) ? nullptr : *(--it);
}
int PrintObject::get_layer_idx_get_printz(coordf_t print_z, coordf_t epsilon) {
    coordf_t limit = print_z + epsilon;
    auto     it    = Slic3r::lower_bound_by_predicate(m_layers.begin(), m_layers.end(), [limit](const Layer *layer) { return layer->print_z < limit; });
    return (it == m_layers.begin()) ? -1 : std::distance(m_layers.begin(), it);
}
const Layer* PrintObject::get_layer_at_bottomz(coordf_t bottom_z, coordf_t epsilon) const {
    coordf_t limit_upper = bottom_z + epsilon;
    coordf_t limit_lower = bottom_z - epsilon;

    for (const Layer* layer : m_layers) {
        if (layer->bottom_z() > limit_lower)
            return layer->bottom_z() < limit_upper ? layer : nullptr;
    }

    return nullptr;
}

Layer* PrintObject::get_layer_at_bottomz(coordf_t bottom_z, coordf_t epsilon) { return const_cast<Layer*>(std::as_const(*this).get_layer_at_bottomz(bottom_z, epsilon)); }


}
