#ifndef slic3r_GCode_hpp_
#define slic3r_GCode_hpp_

#include "ExtrusionEntity.hpp"
#include "Polygon.hpp"
#include "Config.hpp"
#include "Print.hpp"
#include "libslic3r.h"
#include "GCodeWriter.hpp"
#include "GCode/BeltKinematics.hpp"
#include "Layer.hpp"
#include "Nonplanar/CurvedTopsSchedule.hpp"
#include "Point.hpp"
#include "PlaceholderParser.hpp"
#include "PrintConfig.hpp"
#include "GCode/AvoidCrossingPerimeters.hpp"
#include "GCode/CoolingBuffer.hpp"
#include "GCode/FanMover.hpp"
#include "GCode/RetractWhenCrossingPerimeters.hpp"
#include "GCode/SpiralVase.hpp"
#include "GCode/ConformalCooling.hpp"
#include "GCode/ConformalEmit.hpp"
#include "GCode/ConformalTravel.hpp"
#include "GCode/ClearanceVerifier.hpp"
#include "Nonplanar/HeadEnvelope.hpp"
#include "Nonplanar/HeightField.hpp"
#include "Nonplanar/Wave.hpp"
#include "Nonplanar/ShellSeams.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/SeamPlacer.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "GCode/ThumbnailData.hpp"
#include "libslic3r/ObjectID.hpp"
#include "GCode/ExtrusionProcessor.hpp"

#include "GCode/PressureEqualizer.hpp"
#include "GCode/SmallAreaInfillFlowCompensator.hpp"
#include "GCode/AdaptivePAProcessor.hpp"

#include "GCode/TimelapsePosPicker.hpp"
#include "libslic3r_version.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <cstdio>
#include <array>
#include <cstdlib>
#include <memory>
#include <map>
#include <unordered_map>
#include <unordered_map>
#include <optional>
#include <set>
#include <string>
#include <cfloat>
#include <vector>
#include <utility>
#include <cmath>
#include "BoundingBox.hpp"
#include "Polyline.hpp"
#include "BeltBrim.hpp"

namespace Slic3r { class ExtrusionEntityCollection; }
namespace Slic3r::Nonplanar { struct ConformalPatch; }

namespace Slic3r {

class GCode;
struct WipeInwardSupport;

namespace CustomGCode{ struct Item; }
struct PrintInstance;
class ConstPrintObjectPtrsAdaptor;

class OozePrevention {
public:
    bool enable;

    OozePrevention() : enable(false) {}
    std::string pre_toolchange(GCode &gcodegen);
    std::string post_toolchange(GCode &gcodegen);

private:
    int _get_temp(const GCode &gcodegen) const;
};

class Wipe {
public:
    bool enable;
    Polyline path;

    struct RetractionValues{
        double retraction_length_before_wipe = 0.;
        double retraction_length_during_wipe = 0.;
        double retraction_length_after_wipe  = 0.;
    };

    Wipe() : enable(false) {}
    bool has_path() const { return !this->path.points.empty(); }
    void reset_path() { this->path = Polyline(); }
    std::string wipe(GCode &gcodegen, double length, bool toolchange = false, bool is_last = false);

    RetractionValues calculateWipeRetractionLengths(GCode& gcodegen, bool toolchange);
    void update_path(const ExtrusionPaths &paths, bool reverse = false);
};

class WipeTowerIntegration {
public:
    WipeTowerIntegration(
        const PrintConfig                                           &print_config,
        const int                                                    plate_idx,
        const Vec3d                                                  plate_origin,
        const std::vector<WipeTower::ToolChangeResult>              &priming,
        const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
        const WipeTower::ToolChangeResult                           &final_purge,
        const std::vector<unsigned int>                             &slice_used_filaments) :
        m_left( 0.f),
        m_right(float( print_config.prime_tower_width.value)),
        m_wipe_tower_pos(float(print_config.wipe_tower_x.get_at(plate_idx)), float(print_config.wipe_tower_y.get_at(plate_idx))),
        m_wipe_tower_rotation(float(print_config.wipe_tower_rotation_angle)),
        m_priming(priming),
        m_tool_changes(tool_changes),
        m_final_purge(final_purge),
        m_layer_idx(-1),
        m_tool_change_idx(0),
        m_plate_origin(plate_origin),
        m_single_extruder_multi_material(print_config.single_extruder_multi_material),
        m_enable_timelapse_print(print_config.timelapse_type.value == TimelapseType::tlSmooth),
        m_enable_wrapping_detection(print_config.enable_wrapping_detection && (print_config.wrapping_exclude_area.values.size() > 2) && (slice_used_filaments.size() <= 1)),
        m_is_first_print(true),
        m_print_config(&print_config),
        m_last_wipe_tower_print_z(print_config.z_offset.value),
        m_sparse_layers_skipped(wipe_tower_sparse_layers_skipped(print_config)),
        m_sparse_layers_combined(wipe_tower_sparse_layers_combined(print_config))
    {
        if (m_sparse_layers_skipped)
            m_compacted_tower_z = compute_compacted_wipe_tower_z(tool_changes, float(print_config.z_offset.value));
        m_extruder_offsets.resize(print_config.filament_map.size(), print_config.extruder_offset.get_at(print_config.master_extruder_id.value - 1));
        const auto& filament_map = print_config.filament_map.values;
        for (size_t idx = 0; idx < filament_map.size(); ++idx)
            m_extruder_offsets[idx] = print_config.extruder_offset.get_at(filament_map[idx] - 1);
    }

    std::string prime(GCode &gcodegen);
    void next_layer() { ++ m_layer_idx; m_tool_change_idx = 0; }
    std::string tool_change(GCode &gcodegen, int extruder_id, bool finish_layer);
    bool is_empty_wipe_tower_gcode(GCode &gcodegen, int extruder_id, bool finish_layer);
    std::string finalize(GCode &gcodegen);
    std::vector<float> used_filament_length() const;

    bool is_first_print() const { return m_is_first_print;}
    void set_is_first_print(bool is) { m_is_first_print = is; }

    bool enable_timelapse_print() const { return m_enable_timelapse_print; }
    void set_wipe_tower_depth(float depth) { m_wipe_tower_depth = depth; }
    void set_wipe_tower_bbx(const BoundingBoxf & bbx) { m_wipe_tower_bbx = bbx; }
    void set_rib_offset(const Vec2f &rib_offset) { m_rib_offset = rib_offset; }

private:
    WipeTowerIntegration& operator=(const WipeTowerIntegration&);
    std::string prepare_nonplanar_visit(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr,
                                       const Point &start, double &z) const;
    std::string append_tcr(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z = -1.) const;
    void record_nonplanar_material(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, double z) const;
    std::string tower_height_tag(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, const std::string &tcr_gcode) const;
    Polyline generate_path_to_wipe_tower(const Point &start_pos, const Point &end_pos, const BoundingBox &avoid_polygon, const Polygons &bed_polygons) const;
    std::string append_tcr2(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z = -1.) const;
    std::string travel_to_tower_gap(GCode &gcodegen, const Point &route_start, const Point &start_wipe_pos) const;
    Vec2f transform_wt2_pt(const Vec2f &pt) const;
    Polygons shared_printable_area(GCode &gcodegen) const;

    std::string post_process_wipe_tower_moves(const WipeTower::ToolChangeResult& tcr, const Vec2f& translation, float angle) const;
    const float                                                  m_left;
    const float                                                  m_right;
    const Vec2f                                                  m_wipe_tower_pos;
    const float                                                  m_wipe_tower_rotation;
    std::vector<Vec2d>                                           m_extruder_offsets;

    const std::vector<WipeTower::ToolChangeResult>              &m_priming;
    const std::vector<std::vector<WipeTower::ToolChangeResult>> &m_tool_changes;
    const WipeTower::ToolChangeResult                           &m_final_purge;
    int                                                          m_layer_idx;
    int                                                          m_tool_change_idx;
    double                                                       m_last_wipe_tower_print_z;

    Vec3d                                                        m_plate_origin;
    bool                                                         m_single_extruder_multi_material;
    bool                                                         m_enable_timelapse_print;
    bool                                                         m_enable_wrapping_detection;
    bool                                                         m_is_first_print;
    const PrintConfig *                                          m_print_config;
    float                                                        m_wipe_tower_depth;
    BoundingBoxf                                                 m_wipe_tower_bbx;
    Vec2f                                                        m_rib_offset{Vec2f(0, 0)};
    const bool                                                   m_sparse_layers_skipped;
    const bool                                                   m_sparse_layers_combined;
    std::vector<float>                                           m_compacted_tower_z;
};

class ColorPrintColors
{
    static const std::vector<std::string> Colors;
public:
    static const std::vector<std::string>& get() { return Colors; }
};

struct LayerResult {
    std::string gcode;
    size_t      layer_id;
    bool        spiral_vase_enable { false };
    bool        cooling_buffer_flush { false };
    bool        nop_layer_result { false };

    static LayerResult make_nop_layer_result() { return {"", std::numeric_limits<size_t>::max(), false, false, true}; }
};

class GCode {

public:
    GCode() :
    	m_origin(Vec2d::Zero()),
        m_enable_loop_clipping(true),
        m_resonance_avoidance(true),
        m_enable_cooling_markers(false),
        m_enable_extrusion_role_markers(false),
        m_last_processor_extrusion_role(erNone),
        m_layer_count(0),
        m_layer_index(-1),
        m_layer(nullptr),
        m_object_layer_over_raft(false),
        m_last_pos_defined(false),
        m_last_extrusion_role(erNone),
        m_last_width(0.0f),
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_last_mm3_per_mm(0.0),
#endif
        m_brim_done(false),
        m_second_layer_things_done(false),
        m_silent_time_estimator_enabled(false),
        m_last_obj_copy(nullptr, Point(std::numeric_limits<coord_t>::max(), std::numeric_limits<coord_t>::max())),
        m_toolchange_count(0),
        m_nominal_z(0.)
        {}
    virtual ~GCode();

public:
    void            do_export(Print* print, const char* path, GCodeProcessorResult* result = nullptr, ThumbnailsGeneratorCallback thumbnail_cb = nullptr,
                              bool retain_raw_positions = false);
    void            export_layer_filaments(GCodeProcessorResult* result);
    void set_gcode_offset(double x, double y) { m_writer.set_xy_offset(x, y); m_processor.set_xy_offset(x, y);}
    void set_gcode_offset_with_imex_shift(double x, double y, double imex_x, double imex_y) {
        m_writer.set_xy_offset(x + imex_x, y + imex_y);
        m_processor.set_xy_offset(x, y);
    }

    const Vec2d&    origin() const { return m_origin; }
    void            set_origin(const Vec2d &pointf);
    void            set_origin(const coordf_t x, const coordf_t y) { this->set_origin(Vec2d(x, y)); }
    Point           last_pos() const { return m_last_pos.to_point(); }
    Vec2d           point_to_gcode(const Point &point) const;
    Vec3d                    point_to_gcode(const Point3& point) const;
    Point           gcode_to_point(const Vec2d &point) const;
    Vec2d point_to_gcode_quantized(const Point& point) const;
    Vec3d                    point_to_gcode_quantized(const Point3& point) const;
    const FullPrintConfig &config() const { return m_config; }
    const Layer*    layer() const { return m_layer; }
    GCodeWriter&    writer() { return m_writer; }
    const GCodeWriter& writer() const { return m_writer; }
    PlaceholderParser& placeholder_parser() { return m_placeholder_parser_integration.parser; }
    const PlaceholderParser& placeholder_parser() const { return m_placeholder_parser_integration.parser; }
    std::string     placeholder_parser_process(const std::string &name, const std::string &templ, unsigned int current_filament_id, const DynamicConfig *config_override = nullptr);
    bool            enable_cooling_markers() const { return m_enable_cooling_markers; }
    std::string     extrusion_role_to_string_for_parser(const ExtrusionRole &);

    float interpolate_value_across_layers(float start_value, float end_value, float step = 0.0f) const;

    unsigned int    layer_count() const { return m_layer_count; }
    void            set_layer_count(unsigned int value) { m_layer_count = value; }
    void            apply_print_config(const PrintConfig &print_config);

    std::string     travel_to(const Point& point, ExtrusionRole role, std::string comment, double z = DBL_MAX, const ExtrusionPath *native_target = nullptr);
    bool            needs_zmove(const Polyline &travel, double target_z);
    bool            needs_retraction(const Polyline& travel, ExtrusionRole role, LiftType& lift_type);
    std::string     retract(bool toolchange = false, bool is_last_retraction = false, LiftType lift_type = LiftType::NormalLift, bool apply_instantly = false, ExtrusionRole role = erNone);
    std::string     unretract(float extra_retract = 0.f) { return m_writer.unlift() + m_writer.unretract(extra_retract); }
    std::string     set_extruder(unsigned int extruder_id, double print_z, bool by_object=false, int toolchange_temp_override = -1, bool defer_temp_wait = false);
    std::string     set_filament_pressure_advance(unsigned int filament_id, int tool = -1);
    bool is_BBL_Printer();
    WipeTowerType wipe_tower_type();

    std::string set_object_info(Print* print);

    static void append_full_config(const Print& print, std::string& str);

    size_t get_filament_config_index(int filament_id) const;
    size_t get_filament_config_index(int filament_id, size_t layer_id) const;
    size_t get_nozzle_config_index(int filament_id) const;

    struct ConfigIndexCache
    {
        bool   valid{false};
        int    filament_id{0};
        size_t layer_idx{0};
        size_t generation{0};
        size_t index{0};

        template<class Lookup> size_t get(int filament, size_t layer, size_t gen, Lookup &&lookup)
        {
            if (!valid || filament_id != filament || layer_idx != layer || generation != gen)
                *this = {true, filament, layer, gen, size_t(lookup())};
            return index;
        }
    };

    struct LayerToPrint
    {
        LayerToPrint() : object_layer(nullptr), support_layer(nullptr), original_object(nullptr) {}
        const Layer* 		object_layer;
        const SupportLayer* support_layer;
        const PrintObject*  original_object;
        const BeltBrimBand* belt_brim_band { nullptr };
        const Layer* 		layer()   const
        {
            if (object_layer != nullptr)
                return object_layer;

            if (support_layer != nullptr)
                return support_layer;

            return nullptr;
        }

        const PrintObject* 	object()   const
        {
            return (this->layer() != nullptr) ? this->layer()->object() : nullptr;
        }
        coordf_t            print_z() const
        {
            coordf_t sum_z = 0.;
            size_t count = 0;
            if (object_layer != nullptr) {
                sum_z += object_layer->print_z;
                count++;
            }

            if (support_layer != nullptr) {
                sum_z += support_layer->print_z;
                count++;
            }

            if (count == 0 && belt_brim_band != nullptr)
                return belt_brim_band->print_z;

            return sum_z / count;
        }
    };

protected:
    class GCodeOutputStream {
    public:
        GCodeOutputStream(FILE *f, GCodeProcessor &processor) : f(f), m_processor(processor) {}
        ~GCodeOutputStream() { this->close(); }

        bool is_open() const { return f; }
        bool is_error() const;

        void flush();
        void close();

        void write(const std::string& what) { this->write(what.c_str()); }
        void write(const char* what);

        void writeln(const std::string& what);

        void write_format(const char* format, ...);

    private:
        FILE *f = nullptr;
        GCodeProcessor &m_processor;
    };

    virtual void init_belt_writer(Print &print) {}
    virtual void write_belt_header(GCodeOutputStream &file, const Print &print) {}
    virtual void on_set_origin(const PrintObject *obj, const Point &inst_shift) {}
    virtual bool should_disable_arc_fitting() const
        { return ! m_writer.kinematics().supports_arc_moves(); }

    void            _do_export(Print &print, GCodeOutputStream &file, ThumbnailsGeneratorCallback thumbnail_cb);

    static std::vector<LayerToPrint>        		                   collect_layers_to_print(const PrintObject &object, bool skip_empty_first_layer = false);
    static std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> collect_layers_to_print(const Print &print);

    std::string generate_skirt(const Print &print,
        const ExtrusionEntityCollection &skirt,
        const Point& offset,
        const float skirt_start_angle,
        const LayerTools &layer_tools,
        const Layer& layer,
        unsigned int extruder_id,
        std::vector<coordf_t> &skirt_done);
    std::string generate_object_skirt_group(const Print &print,
        const PrintObject &object,
        size_t instance_id,
        const LayerTools &layer_tools,
        const Layer& layer,
        unsigned int extruder_id);
    std::string generate_object_brim(const Print &print,
        const PrintObject &object,
        size_t instance_id,
        bool first_layer,
        const Layer *object_layer);

    LayerResult process_belt_brim_layer(
        const Print                     &print,
        const std::vector<LayerToPrint> &layers,
        const LayerTools                &layer_tools,
        const bool                       last_layer,
        const size_t                     single_object_instance_idx);

    std::string emit_belt_brim_bands(
        const Print                     &print,
        const std::vector<LayerToPrint> &layers,
        const size_t                     single_object_instance_idx,
        const unsigned int               extruder_id);

    LayerResult process_layer(
        const Print                     &print,
        const std::vector<LayerToPrint> &layers,
        const LayerTools  				&layer_tools,
        const bool                       last_layer,
		const std::vector<const PrintInstance*> *ordering,
        const int                        most_used_extruder,
        const size_t                     single_object_idx = size_t(-1),
        const bool                       prime_extruder = false);
    void process_layers(
        const Print                                                         &print,
        const ToolOrdering                                                  &tool_ordering,
        const std::vector<const PrintInstance*>                             &print_object_instances_ordering,
        const std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>>   &layers_to_print,
        GCodeOutputStream                                                   &output_stream);
    void process_layers(
        const Print                             &print,
        const ToolOrdering                      &tool_ordering,
        std::vector<LayerToPrint>                layers_to_print,
        const size_t                             single_object_idx,
        GCodeOutputStream                       &output_stream,
        const bool                               prime_extruder = false);

    void check_placeholder_parser_failed();
    size_t cur_extruder_index() const;
    size_t get_extruder_id(unsigned int filament_id) const;
    void   update_placeholder_parser_with_variant_params();

    void            set_last_pos(const Point &pos) { m_last_pos = Point3(pos, 0); m_last_pos_defined = true; }
    void            set_last_pos(const Point3 &pos) { m_last_pos = pos; m_last_pos_defined = true; }
    bool            last_pos_defined() const { return m_last_pos_defined; }
    void            set_extruders(const std::vector<unsigned int> &extruder_ids);
    std::string     preamble();
    std::string     change_layer(coordf_t print_z);
    void            mass_load_limited_machine_acceleration(const PrintStatistics &curr_print_statistics, const Print &print,
                                                           double &y_acceleration_limit_res, double &accumulated_mass_res);
    std::string extrude_entity(const ExtrusionEntity&                     entity,
                               const std::string&                         description       = "",
                               double                                     speed             = -1.,
                               const std::vector<const ExtrusionEntity*>& region_perimeters = {},
                               const WipeInwardSupport*                   wipe_support      = nullptr);
    std::string extrude_loop(const ExtrusionLoop&                       loop,
                             const std::string&                         description,
                             double                                     speed             = -1.,
                             const std::vector<const ExtrusionEntity*>& region_perimeters = {},
                             const Point*                               start_point       = nullptr,
                             const WipeInwardSupport*                   wipe_support      = nullptr);
    void        start_at_upper_seam(const Layer &layer, ExtrusionPath &run) const;
    std::string extrude_raised_walls(const Print &print, const std::vector<LayerToPrint> &layers, unsigned int extruder_id,
                                     size_t single_object_instance_idx);
    std::string extrude_multi_path(const ExtrusionMultiPath& multipath, const std::string& description = "", double speed = -1.);
    std::string extrude_path(const ExtrusionPath& path, const std::string& description = "", double speed = -1.);

    double          m_multi_flow_segment_path_average_mm3_per_mm = 0;
    bool            m_multi_flow_segment_path_pa_set = false;
    double          m_last_mm3_mm = 0;
    struct ObjectByExtruder
    {
        ObjectByExtruder() : support(nullptr), support_extrusion_role(erNone) {}
        const ExtrusionEntityCollection  *support;
        ExtrusionRole                     support_extrusion_role;

        struct Island
        {
            struct Region {
                std::vector<const ExtrusionEntity*> perimeters;
                std::vector<const ExtrusionEntity*> infills;

                std::vector<const WipingExtrusions::ExtruderPerCopy*> infills_overrides;
                std::vector<const WipingExtrusions::ExtruderPerCopy*> perimeters_overrides;

	            enum Type {
	            	PERIMETERS,
	            	INFILL,
	            };

                void append(const Type type, const ExtrusionEntityCollection* eec, const WipingExtrusions::ExtruderPerCopy* copy_extruders);
            };


            std::vector<Region> by_region;
            const std::vector<Region>& by_region_per_copy(std::vector<Region> &by_region_per_copy_cache, unsigned int copy, unsigned int extruder, bool wiping_entities = false) const;
        };
        std::vector<Island>         islands;
    };

	struct InstanceToPrint
	{
		InstanceToPrint(ObjectByExtruder &object_by_extruder, size_t layer_id, const PrintObject &print_object, size_t instance_id, size_t label_object_id) :
			object_by_extruder(object_by_extruder), layer_id(layer_id), print_object(print_object), instance_id(instance_id), label_object_id(label_object_id) {}

		ObjectByExtruder		&object_by_extruder;
		const size_t       		 layer_id;
		const PrintObject 		&print_object;
		const size_t			 instance_id;
        const size_t             label_object_id;
	};

	std::vector<InstanceToPrint> sort_print_object_instances(
		std::vector<ObjectByExtruder> 					&objects_by_extruder,
		const std::vector<LayerToPrint> 				&layers,
		const std::vector<const PrintInstance*>     	*ordering,
		const size_t                     				 single_object_instance_idx);

    std::string     extrude_curved_tops(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region);
    std::string     extrude_perimeters(const Print& print, const std::vector<ObjectByExtruder::Island::Region>& by_region, bool is_first_layer, bool is_infill_first, bool unsupported_loops_only = false);
    std::string     extrude_infill(const Print& print, const std::vector<ObjectByExtruder::Island::Region>& by_region, bool ironing);
    std::string     extrude_support(const ExtrusionEntityCollection& support_fills, const ExtrusionRole support_extrusion_role);
    std::string     extrude_conformal_patches(const Print &print, const std::vector<LayerToPrint> &layers, unsigned int extruder_id,
                                              size_t single_object_instance_idx, bool before = false);
    double          tower_layer_z() const;
    std::string     extrude_conformal_path(const ExtrusionPathConformal &path, const std::string &description,
                                           const std::function<Vec2d(const Vec2d &)> &surface_gradient = {},
                                           const ExtrusionPathConformal *next = nullptr,
                                           const std::function<Vec2d(const Vec2d &)> &slope_gradient = {});
    bool            conformal_in_band(const ExtrusionPathConformal *prev, const ExtrusionPathConformal *path) const;
    bool            conformal_short_hop(const ExtrusionPathConformal &path, const ExtrusionPathConformal *next, Vec3d &to);
    bool            conformal_keep_primed(ExtrusionRole from, const ExtrusionPathConformal *next,
                                         const ExtrusionPathConformal *prev = nullptr);
    std::string     extrude_multiplanar_chunk(const Nonplanar::ConformalPatch &patch);
    std::string     extrude_field_patch(const Nonplanar::ConformalPatch &patch);
    std::string     travel_above_nonplanar(const Polyline &travel, const std::string &comment, double z);
    void            emit_conformal_moves(std::string &gcode, const ExtrusionPathConformal &path, double e_per_mm,
                                         double speed, const std::string &speed_marker, const std::string &description);
    std::string     wipe_conformal_path();
    Nonplanar::PressureAdvance conformal_pressure_advance() const;
    std::string     surface_travel(const Vec3d &to, bool from_on_bead, bool to_on_bead, bool in_band, const std::string &comment,
                                   double clearance = -1., const Vec2d &via = Vec2d::Constant(std::numeric_limits<double>::quiet_NaN()));
    std::string     conformal_retract();
    std::function<double(const Vec3d &, const Vec3d &)> nozzle_clearance();
    double          conformal_clearance(bool lift = true);
    bool            conformal_lift_enforced(ExtrusionRole role) const;
    double          nonplanar_repair_lift() const;
    void            ensure_material_columns(const ExtrusionPath &path);
    std::string     rotary_move_safely(const ToolPose *target, const JointPose *seed = nullptr);
    bool            rotary_at_home() const;
    std::string     rotary_hop(const ToolPose *target, const JointPose *seed = nullptr);
    std::string     rotary_local_hop(const ToolPose &target, bool straighten = false);
    void            clamp_table_tilt(std::vector<Nonplanar::ConformalPoint> &points);
    std::string     rotary_turn_on_spot(const ToolPose &target);
    const Nonplanar::HeadEnvelope &nonplanar_head();
    double          nonplanar_hop();
    double          conformal_max_speed_z() const;
    double          conformal_segment_rate() const;
    void            nonplanar_pass_start(const ExPolygons &footprint, bool ironing);
    std::string     nonplanar_pass_hold(const ExtrusionPathConformal *next);
    std::string     nonplanar_patch_end();
    double          nonplanar_travel_z(const Vec2d *from = nullptr, const Vec2d *to = nullptr, bool nozzle_only = false);
    bool            nonplanar_travel_clear(const Polyline &travel, double z, bool short_travel);
    bool            below_conformal_material() const
    { return m_material_columns && m_nonplanar_ceiling > std::min(m_nominal_z, m_writer.get_position().z()) + EPSILON; }

    void compute_farthest_point(const std::vector<LayerToPrint> &layers, int most_used_extruder,
                                const std::map<std::pair<const SupportLayer *, ExtrusionRole>, unsigned int> &support_filaments);
    std::string     generate_timelapse_gcode(const Print &print, coordf_t print_z, int most_used_extruder,
                                              const std::set<size_t> *layer_object_label_ids,
                                              const std::vector<const PrintObject*> *printed_objects,
                                              bool skip_pos_pick = false);

    LiftType to_lift_type(ZHopType z_hop_types);

    std::set<ObjectInstanceID>      m_objsWithBrim;
    SeamPlacer                          m_seam_placer;

    struct InstanceVisit
    {
        size_t              instance_idx;
        std::vector<size_t> islands;
        bool                first_visit;
    };

    struct IslandOrderNode
    {
        ObjectID object_id;
        size_t   instance_id;
        size_t   island_idx;
        Point    pos;
        bool operator==(const IslandOrderNode &rhs) const {
            return object_id == rhs.object_id && instance_id == rhs.instance_id &&
                   island_idx == rhs.island_idx && pos == rhs.pos;
        }
    };

    struct IslandOrderCacheEntry
    {
        std::vector<IslandOrderNode>         nodes;
        std::vector<std::pair<size_t, bool>> layout;
        std::vector<InstanceVisit>           visits;
    };
    std::map<unsigned int, IslandOrderCacheEntry> m_ordering_cache;

    ExtrusionQualityEstimator m_extrusion_quality_estimator;


    Vec2d                               m_origin;
    FullPrintConfig                     m_config;
    DynamicConfig                       m_calib_config;
    double                              m_scaled_resolution;
    GCodeWriter                         m_writer;

    struct PlaceholderParserIntegration {
        void reset();
        void init(const GCodeWriter &config);
        void update_from_gcodewriter(const GCodeWriter &writer);
        void validate_output_vector_variables();

        PlaceholderParser                   parser;
        PlaceholderParser::ContextData      context;
        std::map<std::string, std::string>  failed_templates;
        DynamicConfig                       output_config;
        ConfigOptionFloats                 *opt_position { nullptr };
        ConfigOptionFloat                  *opt_zhop { nullptr };
        ConfigOptionFloats                 *opt_e_position { nullptr };
        ConfigOptionFloats                 *opt_e_retracted { nullptr };
        ConfigOptionFloats                 *opt_e_restart_extra { nullptr };
        ConfigOptionFloats                 *opt_extruded_volume { nullptr };
        ConfigOptionFloats                 *opt_extruded_weight { nullptr };
        ConfigOptionFloat                  *opt_extruded_volume_total { nullptr };
        ConfigOptionFloat                  *opt_extruded_weight_total { nullptr };
        size_t                              num_extruders;
        std::vector<double>                 position;
        std::vector<double>                 e_position;
        std::vector<double>                 e_retracted;
        std::vector<double>                 e_restart_extra;
    } m_placeholder_parser_integration;

    OozePrevention                      m_ooze_prevention;
    Wipe                                m_wipe;
    AvoidCrossingPerimeters             m_avoid_crossing_perimeters;
    RetractWhenCrossingPerimeters       m_retract_when_crossing_perimeters;
    TimelapsePosPicker                  m_timelapse_pos_picker;

    struct FarthestPointTimelapseContext {
        bool    enabled{false};
        Point   farthest_point;
        Vec2d   farthest_gcode_pos{0, 0};
        int     farthest_extruder_id{0};
        bool    farthest_is_photo_head{false};
        bool    inserted_this_layer{false};
        int     most_used_extruder{0};
        std::set<size_t> layer_object_label_ids;
    };
    FarthestPointTimelapseContext m_farthest_point_timelapse;

    bool                                m_enable_loop_clipping;
    bool                                m_resonance_avoidance; 
    bool                                m_enable_cooling_markers;
    
    bool m_enable_exclude_object;
    std::vector<size_t> m_label_objects_ids;
    std::unordered_map<const PrintInstance*, std::string> m_instance_names;
    const std::string& instance_name(const PrintInstance &instance);
    std::string _encode_label_ids_to_base64(std::vector<size_t> ids);
    std::string object_exclude_start(const PrintInstance &inst);
    std::string object_exclude_end(const PrintInstance &inst);
    std::array<bool, ExtrusionRole::erCount> m_is_role_based_fan_on;
    std::array<int, ExtrusionRole::erCount>  m_role_based_fan_marker_layer;
    bool                                m_enable_extrusion_role_markers;
    ExtrusionRole                       m_last_processor_extrusion_role;
    unsigned int                        m_layer_count;
    int                                 m_layer_index;
    const Layer*                        m_layer;
    bool                                m_object_layer_over_raft;
    ExtrusionRole                       m_last_extrusion_role;
    ExtrusionRole                       m_last_notgapfill_extrusion_role;
    float                               m_last_height{ 0.0f };
    float                               m_last_layer_z{ 0.0f };
    float                               m_max_layer_z{ 0.0f };
    double                              m_nonplanar_ceiling{ 0. };
    std::unique_ptr<MaterialColumns>      m_material_columns;
    std::unique_ptr<TableGuard>           m_table_guard;
    size_t                                m_table_clamped = 0;
    bool                                m_track_nonplanar_material{ false };
    std::map<const PrintObject *, Nonplanar::WaveField> m_wave_fields;
    bool m_conformal_lift { false };
    std::vector<Nonplanar::ConformalPoint> m_conformal_points;
    const Nonplanar::HeightField          *m_conformal_surface{ nullptr };
    double                                 m_conformal_tolerance{ 0. };
    std::function<Vec2d(const Vec2d &)>    m_conformal_gradient;
    int                                    m_conformal_band_patch{ -1 };
    bool                                   m_conformal_may_hang{ false };
    const ExtrusionPathConformal          *m_conformal_prev{ nullptr };
    bool                                   m_conformal_ramp{ false };
    bool                                   m_conformal_open_end{ false };
    bool                                   m_conformal_scored{ false };
    bool                                   m_conformal_next_hold{ false };
    bool                                   m_ordered_plan_layer{ false };
    bool                                   m_conformal_scarf{ false };
    std::map<unsigned int, Nonplanar::PressureAdvance> m_start_gcode_pa;
    Nonplanar::PressureAdvance                         m_machine_start_pa;
    std::unique_ptr<Nonplanar::PressureAdvanceTracker> m_conformal_pa_tracker;
    Nonplanar::SupportSnapshot             m_np_support;
    float                                  m_np_fan_level{ 0.f };
    bool                                   m_conformal_on_bead{ false };
    bool                                   m_conformal_primed{ false };
    double                                 m_conformal_retract{ 0. };
    double                                 m_conformal_prime{ 0. };
    double                                 m_print_clock{ 0. };
    Nonplanar::FreshBeads                  m_fresh_beads;
    Nonplanar::HeadEnvelope                m_nonplanar_head;
    float                               m_last_width{ 0.0f };
    double                              m_last_layer_accumulated_mass{ 0.0 };

#if !defined(NDEBUG)
#undef ORCA_CHECK_GCODE_PLACEHOLDERS
#define ORCA_CHECK_GCODE_PLACEHOLDERS 1
#endif
    
#if ORCA_CHECK_GCODE_PLACEHOLDERS
    std::map<std::string, std::vector<std::string>> m_placeholder_error_messages;
#endif

    Point3                              m_last_pos;
    bool                                m_last_pos_defined;

    NonplanarHeightMap                  m_nonplanar_heights;
    bool                                m_emitting_deferred{ false };
    NonplanarClearanceModel             m_nonplanar_clearance;
    bool                                m_nonplanar_printed_this_layer{ false };
    bool                                m_last_native_curved{ false };
    std::unordered_map<size_t, size_t>  m_path_task_ids;
    struct DeferredExtrusion {
        int                                     slot;
        int                                     base;
        double                                  z;
        size_t                                  source_layer;
        double                                  source_layer_z;
        std::shared_ptr<ExtrusionEntity>        entity;
        std::string                             description;
        double                                  speed;
        const PrintRegion                      *region;
        unsigned int                            extruder;
    };
    std::map<const PrintObject*, NonplanarDeferral>                              m_nonplanar_deferral;
    std::map<std::pair<const PrintObject*, size_t>, std::vector<DeferredExtrusion>> m_deferred_extrusions;
    const NonplanarDeferral            *m_deferral_plan { nullptr };
    std::pair<const PrintObject*, size_t> m_deferral_key { nullptr, 0 };
    size_t                              m_deferral_layer_id { 0 };
    const PrintRegion                  *m_current_region { nullptr };
    bool                                m_force_safe_travel { false };
    bool                                m_force_safe_travel_once { false };
    bool                                m_loop_seam_placed { false };
    bool                                m_slope_compensating { false };
    bool                                defer_planar_material(const ExtrusionEntity &entity, const std::string &description,
                                                              double speed, std::string &gcode, std::vector<ExtrusionPath> *curved_current = nullptr);
    void                                queue_advanced_conformal(const Print &print);
    std::string                         curved_level_marker(const Print &print, double z);
    std::string                         emit_deferred_material(int slot, unsigned int extruder_id);
    std::string                         emit_deferred_before_layer(unsigned int extruder_id);
    std::string                         emit_deferred_after_layer(const PrintObject &print_object, unsigned int extruder_id);

    std::unique_ptr<CoolingBuffer>      m_cooling_buffer;
    std::unique_ptr<SpiralVase>         m_spiral_vase;
    std::unique_ptr<PressureEqualizer>  m_pressure_equalizer;
    
    std::unique_ptr<AdaptivePAProcessor>      m_pa_processor;

    std::string m_imex_parallel_mode;
    std::map<int,int> m_imex_head_filament_map;

    std::unique_ptr<WipeTowerIntegration> m_wipe_tower;

    std::unique_ptr<SmallAreaInfillFlowCompensator> m_small_area_infill_flow_compensator;
    
    std::vector<std::vector<coordf_t>>   m_skirt_group_done;
    bool                                m_brim_done;
    bool                                m_second_layer_things_done;
    std::pair<const PrintObject*, Point> m_last_obj_copy;

    int m_timelapse_warning_code = 0;
    bool m_support_traditional_timelapse = true;

    bool m_silent_time_estimator_enabled;

    Print *m_print{nullptr};

    std::vector<const PrintObject*> m_printed_objects;

    GCodeProcessor m_processor;

    std::unique_ptr<FanMover> m_fan_mover;

    Print* m_curr_print = nullptr;
    unsigned int m_toolchange_count;
    coordf_t m_nominal_z;
    double   m_sub_layer_flow_ratio = 0.0;
    double   m_sub_layer_height     = 0.0;
    bool m_need_change_layer_lift_z = false;
    double m_np_hold_z = 0.;
    int m_start_gcode_filament = -1;
    std::string m_filament_instances_code;

    size_t m_cur_layer_idx{0};
    mutable ConfigIndexCache m_filament_index_cache;
    mutable ConfigIndexCache m_nozzle_index_cache;

    std::optional<coordf_t> m_belt_brim_z;
    const PrintObject *m_belt_floor_object{nullptr};
    struct BeltFloorObjectGuard {
        const PrintObject *&slot;
        BeltFloorObjectGuard(const PrintObject *&s, const PrintObject *o) : slot(s) { slot = o; }
        ~BeltFloorObjectGuard() { slot = nullptr; }
    };

    bool m_belt_in_band{false};

    std::set<unsigned int>                  m_initial_layer_extruders;
    std::vector<std::vector<unsigned int>>  m_sorted_layer_filaments;
    int get_bed_temperature(const int extruder_id, const bool is_first_layer, const BedType bed_type) const;
    int get_highest_bed_temperature(const bool is_first_layer,const Print &print) const;

    void update_layer_related_config(int layer_id);

    double      calc_max_volumetric_speed(const double layer_height, const double line_width, const std::string co_str);
    std::string _extrude(const ExtrusionPath &path, const std::string &path_description = "", double speed = -1);
    ConfigOptionFloatsOrPercents overhang_speed_table(double ref_speed) const;
    bool _needSAFC(const ExtrusionPath &path);
    void print_machine_envelope(GCodeOutputStream& file, Print& print);
    void _print_first_layer_bed_temperature(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait);
    void _print_first_layer_extruder_temperatures(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait);
    bool    on_first_layer() const { return m_layer != nullptr && m_layer->id() == 0 && abs(m_layer->bottom_z()) < EPSILON; }
    bool on_first_layer(const Vec3d &point_slicing_mm) const {
        double h;
        if (this->belt_height_above_floor(point_slicing_mm, h))
            return h <= m_config.initial_layer_print_height.value + EPSILON;
        return on_first_layer();
    }
    int effective_layer_index_for_point(const Vec3d &point_slicing_mm) const {
        double h;
        if (this->belt_height_above_floor(point_slicing_mm, h)) {
            const double lh = this->first_layer_band_mm();
            return h <= 0. ? 0 : int(std::floor(h / lh));
        }
        return on_first_layer() ? 0 : layer_id();
    }

    double first_layer_band_mm() const {
        const double band = m_config.initial_layer_print_height.value;
        return band > 0. ? band : 0.2;
    }

    bool belt_height_above_floor(const Vec3d &point_slicing_mm, double &height_mm) const;
    int  belt_layer_past_first_layer_band(const Layer *object_layer) const;
    int layer_id() const {
        if (m_layer == nullptr)
            return -1;
        return m_layer->id();
    }
    bool                                object_layer_over_raft() const { return m_object_layer_over_raft; }

    friend ObjectByExtruder& object_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>> &by_extruder,
        unsigned int                                           extruder_id,
        size_t                                                 object_idx,
        size_t                                                 num_objects);
    friend std::vector<ObjectByExtruder::Island>& object_islands_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>>  &by_extruder,
        unsigned int                                            extruder_id,
        size_t                                                  object_idx,
        size_t                                                  num_objects,
        size_t                                                  num_islands);

    friend class Wipe;
    friend class WipeTowerIntegration;
    friend class PressureEqualizer;
    friend class Print;
    friend class SmallAreaInfillFlowCompensator;
};

std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print, bool init_order = false);

std::vector<PrecomputedOverhangLayer> precompute_overhang_layers(const std::vector<GCode::LayerToPrint> &layers, bool overhang_fan);

}

#endif
