#ifndef slic3r_ToolOrdering_hpp_
#define slic3r_ToolOrdering_hpp_

#include "../libslic3r.h"

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <functional>
#include <map>
#include <tuple>
#include <set>
#include <unordered_map>
#include <string>
#include <utility>

#include <boost/container/small_vector.hpp>
#include <vector>
#include "../FilamentMixer.hpp"
#include "../MultiNozzleUtils.hpp"
#include "../ObjectID.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r { class ExtrusionEntity; }
namespace Slic3r { class ExtrusionEntityCollection; }
namespace Slic3r { enum ExtrusionRole : uint8_t; }

namespace Slic3r {

class Print;
class PrintObject;
class LayerTools;
namespace CustomGCode { struct Item; }
class PrintRegion;

class WipingExtrusions
{
public:
    bool is_anything_overridden() const {
        return something_overridden;
    }

    typedef boost::container::small_vector<int32_t, 3> ExtruderPerCopy;

    const ExtruderPerCopy* get_extruder_overrides(const ExtrusionEntity* entity, const PrintObject* object, int correct_extruder_id, size_t num_of_copies);
    int get_support_extruder_overrides(const PrintObject* object);
    int get_support_interface_extruder_overrides(const PrintObject* object);

    float mark_wiping_extrusions(const Print& print, unsigned int old_extruder, unsigned int new_extruder, float volume_to_wipe);

    void ensure_perimeters_infills_order(const Print& print);

    bool is_overriddable(const ExtrusionEntityCollection& ee, const PrintConfig& print_config, const PrintObject& object, const PrintRegion& region) const;
    bool is_overriddable_and_mark(const ExtrusionEntityCollection& ee, const PrintConfig& print_config, const PrintObject& object, const PrintRegion& region) {
    	bool out = this->is_overriddable(ee, print_config, object, region);
    	this->something_overridable |= out;
    	return out;
    }

    bool is_support_overriddable(const ExtrusionRole role, const PrintObject& object) const;
    bool is_support_overriddable_and_mark(const ExtrusionRole role, const PrintObject& object) {
        bool out = this->is_support_overriddable(role, object);
        this->something_overridable |= out;
        return out;
    }

    bool is_support_overridden(const PrintObject* object) const {
        return support_map.find(object) != support_map.end();
    }

    bool is_support_interface_overridden(const PrintObject* object) const {
        return support_intf_map.find(object) != support_intf_map.end();
    }

    void set_layer_tools_ptr(const LayerTools* lt) { m_layer_tools = lt; }

    bool is_entity_overridden(const ExtrusionEntity* entity, const PrintObject *object, size_t copy_id) const {
        auto it = entity_map.find(std::make_tuple(entity, object));
        return it != entity_map.end() && copy_id < it->second.size() && it->second[copy_id] != -1;
    }

private:

    int first_nonsoluble_extruder_on_layer(const PrintConfig& print_config) const;
    int last_nonsoluble_extruder_on_layer(const PrintConfig& print_config) const;

    void set_extruder_override(const ExtrusionEntity* entity, const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies);
    void set_support_extruder_override(const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies);
    void set_support_interface_extruder_override(const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies);

    std::map<std::tuple<const ExtrusionEntity*, const PrintObject *>, ExtruderPerCopy> entity_map;
    std::map<const PrintObject*, int> support_map;
    std::map<const PrintObject*, int> support_intf_map;
    bool something_overridable = false;
    bool something_overridden = false;
    const LayerTools* m_layer_tools = nullptr;
};


struct FilamentChangeStats
{
    int filament_flush_weight{0};
    int flush_filament_change_count{0};
    int filament_change_count{0};

    void clear(){
        filament_flush_weight = 0;
        filament_change_count = 0;
        flush_filament_change_count = 0;
    }

    FilamentChangeStats& operator+=(const FilamentChangeStats& other) {
        this->filament_flush_weight += other.filament_flush_weight;
        this->filament_change_count += other.filament_change_count;
        this->flush_filament_change_count += other.flush_filament_change_count;
        return *this;
    }

    FilamentChangeStats operator+(const FilamentChangeStats& other){
        FilamentChangeStats ret;
        ret.filament_flush_weight = this->filament_flush_weight + other.filament_flush_weight;
        ret.filament_change_count = this->filament_change_count + other.filament_change_count;
        ret.flush_filament_change_count = this->flush_filament_change_count + other.flush_filament_change_count;
        return ret;
    }

};


class LayerTools
{
public:
    LayerTools(const coordf_t z) : print_z(z) {}

    bool operator< (const LayerTools &rhs) const { return print_z < rhs.print_z; }
    bool operator==(const LayerTools &rhs) const { return print_z == rhs.print_z; }

    bool is_extruder_order(unsigned int a, unsigned int b) const;
    bool has_extruder(unsigned int extruder) const { return std::find(this->extruders.begin(), this->extruders.end(), extruder) != this->extruders.end(); }

    unsigned int wall_extruder_id(const PrintRegion &region) const;
    unsigned int sparse_infill_filament_id(const PrintRegion &region) const;
    unsigned int internal_solid_filament_id(const PrintRegion &region) const;
	unsigned int extruder(const ExtrusionEntityCollection &extrusions, const PrintRegion &region) const;

    coordf_t 					print_z	= 0.;
    bool 						has_object = false;
    bool						has_support = false;
    std::vector<unsigned int> 	extruders;
    unsigned int 				extruder_override = 0;
    bool                        has_skirt = false;
    bool 						has_wipe_tower = false;
    size_t                      wipe_tower_partitions = 0;
    coordf_t 					wipe_tower_layer_height = 0.;
    const CustomGCode::Item    *custom_gcode = nullptr;

    std::map<unsigned int, unsigned int> mixed_filament_resolution;

    unsigned int resolve_mixed(unsigned int filament_0based) const {
        auto it = mixed_filament_resolution.find(filament_0based);
        return (it != mixed_filament_resolution.end()) ? it->second : filament_0based;
    }

    struct MixedSubLayerGroup {
        unsigned int              mixed_slot_0based;
        std::vector<unsigned int> components_0based;
        std::vector<double>       sub_heights;
        double                    layer_height = 0.;
        bool                      is_gradient = false;
        int                       gradient_first_sorted_idx = 0;

        struct ObjectGradient {
            size_t        total_layers;
            size_t        current_idx;
            double        gradient_start;
            double        gradient_end;
            GradientCurve curve;
        };
        std::map<const PrintObject*, ObjectGradient> per_object_gradient;

        struct VolumeKey {
            const PrintObject* obj;
            ObjectID           volume_id;
            bool operator<(const VolumeKey &o) const {
                if (obj != o.obj) return std::less<const PrintObject*>{}(obj, o.obj);
                return volume_id < o.volume_id;
            }
            bool operator==(const VolumeKey &o) const {
                return obj == o.obj && volume_id == o.volume_id;
            }
        };
        using VolumeGradient = ObjectGradient;
        std::map<VolumeKey, VolumeGradient> per_volume_gradient;
    };
    std::vector<MixedSubLayerGroup> mixed_sub_layer_groups;

    const MixedSubLayerGroup* mixed_group_by_slot(unsigned int slot_id) const {
        for (const auto &g : mixed_sub_layer_groups)
            if (g.mixed_slot_0based == slot_id)
                return &g;
        return nullptr;
    }

    bool is_mixed_slot(unsigned int slot_id) const {
        return mixed_group_by_slot(slot_id) != nullptr;
    }

    WipingExtrusions& wiping_extrusions() {
        m_wiping_extrusions.set_layer_tools_ptr(this);
        return m_wiping_extrusions;
    }

private:
    WipingExtrusions m_wiping_extrusions;
};

class ToolOrdering
{
public:
    enum FilamentChangeMode {
        SingleExt,
        MultiExtBest,
        MultiExtCurr
    };
    ToolOrdering() = default;

    ToolOrdering(const PrintObject &object, unsigned int first_extruder, bool prime_multi_material = false);

    ToolOrdering(const Print& print, unsigned int first_extruder, bool prime_multi_material = false);

    void handle_dontcare_extruder(const std::vector<unsigned int>& first_layer_tool_order);
    void handle_dontcare_extruder(unsigned int first_extruder);

    void sort_and_build_data(const PrintObject &object, unsigned int first_extruder, bool prime_multi_material = false);
    void sort_and_build_data(const Print& print, unsigned int first_extruder, bool prime_multi_material = false);
    void build_sequential_wipe_tower(const Print &print, bool prime_multi_material);

    void    clear() {
        m_layer_tools.clear();
        m_stats_by_single_extruder.clear();
        m_stats_by_multi_extruder_best.clear();
        m_stats_by_multi_extruder_curr.clear();
    }

	void 				assign_custom_gcodes(const Print &print);

    unsigned int   		first_extruder() const { return m_first_printing_extruder; }

    unsigned int   		last_extruder() const { return m_last_printing_extruder; }

    const std::vector<unsigned int>& all_extruders() const { return m_all_printing_extruders; }
    const std::vector<unsigned int>& used_mixed_filaments() const { return m_used_mixed_filaments; }

    const LayerTools&	tools_for_layer(coordf_t print_z) const;
    LayerTools&			tools_for_layer(coordf_t print_z) { return const_cast<LayerTools&>(std::as_const(*this).tools_for_layer(print_z)); }

    const LayerTools&   front()       const { return m_layer_tools.front(); }
    const LayerTools&   back()        const { return m_layer_tools.back(); }
    std::vector<LayerTools>::const_iterator begin() const { return m_layer_tools.begin(); }
    std::vector<LayerTools>::const_iterator end()   const { return m_layer_tools.end(); }
    bool 				empty()       const { return m_layer_tools.empty(); }
    std::vector<LayerTools>& layer_tools() { return m_layer_tools; }
    bool 				has_wipe_tower() const { return ! m_layer_tools.empty() && m_first_printing_extruder != (unsigned int)-1 && m_layer_tools.front().has_wipe_tower; }

    int                 get_most_used_extruder() const { return most_used_extruder; }

    const MultiNozzleUtils::LayeredNozzleGroupResult &get_layered_nozzle_group_result() const { return m_nozzle_group_result; }

    const MultiNozzleUtils::NozzleStatusRecorder &get_nozzle_status() const { return m_nozzle_status; }
    void set_nozzle_status(const MultiNozzleUtils::NozzleStatusRecorder &status) { m_initial_nozzle_status = status; m_nozzle_status = status; }
    static MultiNozzleUtils::LayeredNozzleGroupResult get_recommended_filament_maps(const std::vector<std::vector<unsigned int>>& layer_filaments, const Print* print,const FilamentMapMode mode, const std::vector<std::set<int>>& physical_unprintables, const std::vector<std::set<int>>& geometric_unprintables, const std::map<int, std::set<NozzleVolumeType>>& unprintable_volumes = {}, const std::unordered_map<int, int>& nozzle_status = {});

    static MultiNozzleUtils::LayeredNozzleGroupResult build_sequential_group_result(
        Print*                                            print,
        std::vector<std::vector<int>>                     nozzle_map_per_layer,
        const std::vector<std::vector<unsigned int>>&     layer_filaments,
        const std::vector<std::vector<unsigned int>>&     layer_sequences,
        const std::vector<unsigned int>&                  used_filaments,
        const std::vector<std::set<int>>&                 physical_unprintables,
        const std::vector<std::set<int>>&                 geometric_unprintables,
        const std::map<int, std::set<NozzleVolumeType>>&  unprintable_volumes);

    FilamentChangeStats get_filament_change_stats(FilamentChangeMode mode);
    void                cal_most_used_extruder(const PrintConfig &config);
    float               cal_max_additional_fan(const PrintConfig &config);
    bool                cal_non_support_filaments(const PrintConfig &config,
                                                  unsigned int &     first_non_support_filament,
                                                  std::vector<int> & initial_non_support_filaments,
                                                  std::vector<int> & initial_filaments);

    bool                has_non_support_filament(const PrintConfig &config);

private:
    void				initialize_layers(std::vector<coordf_t> &zs);
    void 				collect_extruders(const PrintObject &object, const std::vector<std::pair<double, unsigned int>> &per_layer_extruder_switches);
    void 				fill_wipe_tower_partitions(const PrintConfig &config, coordf_t object_bottom_z, coordf_t max_layer_height);
    bool                insert_wipe_tower_extruder();
    void                mark_skirt_layers(const PrintConfig &config, coordf_t max_layer_height);
    void 				collect_extruder_statistics(bool prime_multi_material);
    void                reorder_extruders_for_minimum_flush_volume(bool reorder_first_layer);
    void                resolve_mixed_filaments(const PrintConfig &config);
    void                enforce_mixed_component_order();
    void                enforce_conformal_support_order();
    void                enforce_conformal_material_order();

    std::vector<unsigned int> generate_first_layer_tool_order(const Print& print);
    std::vector<unsigned int> generate_first_layer_tool_order(const PrintObject& object);

    std::vector<LayerTools>    m_layer_tools;
    unsigned int               m_first_printing_extruder = (unsigned int)-1;
    unsigned int               m_last_printing_extruder  = (unsigned int)-1;
    std::vector<unsigned int>  m_all_printing_extruders;
    std::vector<unsigned int>  m_used_mixed_filaments;
    const DynamicPrintConfig*  m_print_full_config = nullptr;
    const PrintConfig*         m_print_config_ptr = nullptr;

    std::map<unsigned int, std::map<const PrintObject*, std::vector<size_t>>> m_mixed_object_layers;

    std::map<const PrintObject*, std::vector<size_t>> m_object_all_layer_indices;

    std::map<unsigned int, std::map<LayerTools::MixedSubLayerGroup::VolumeKey, std::vector<size_t>>> m_gradient_volume_layers;
    const PrintObject*         m_print_object_ptr = nullptr;
    Print*                     m_print;
    bool                       m_sorted = false;

    FilamentChangeStats        m_stats_by_single_extruder;
    FilamentChangeStats        m_stats_by_multi_extruder_curr;
    FilamentChangeStats        m_stats_by_multi_extruder_best;
    MultiNozzleUtils::LayeredNozzleGroupResult m_nozzle_group_result;
    MultiNozzleUtils::NozzleStatusRecorder     m_initial_nozzle_status;
    MultiNozzleUtils::NozzleStatusRecorder     m_nozzle_status;

    int                        most_used_extruder;
};

std::vector<unsigned int> parse_cyclic_order(const std::string& str, unsigned int number_of_extruders);

}

#endif
