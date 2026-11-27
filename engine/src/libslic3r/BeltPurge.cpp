#include "Print.hpp"
#include "PrintConfig.hpp"
#include "Exception.hpp"
#include "GCode/ToolOrdering.hpp"
#include "Layer.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "LocalesUtils.hpp"
#include "libslic3r.h"
#include "BeltBrim.hpp"
#include "PrintBase.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <boost/log/trivial.hpp>
#include <cassert>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace Slic3r {

bool Print::has_belt_purge_tower() const
{
    if (!(m_config.belt_printer.value
          && m_config.enable_belt_purge_tower.value
          && !m_config.spiral_mode.value
          && m_config.filament_diameter.values.size() > 1))
        return false;

    return std::any_of(m_objects.begin(), m_objects.end(), [](const PrintObject *object) {
        return object->config().belt_purge_tower_object.value;
    });
}

void Print::_align_belt_purge_layers()
{
    PrintObject *prism = nullptr;
    for (PrintObject *po : m_objects)
        if (po->config().belt_purge_tower_object.value && !po->layers().empty()) {
            prism = po;
            break;
        }
    if (prism == nullptr || prism->layers().empty())
        return;

    const double h = prism->config().layer_height.value;
    if (h <= EPSILON)
        return;

    auto grid_offset = [h](const PrintObject *po) -> double {
        if (po->layers().empty())
            return 0.;
        const double z = po->layers().front()->print_z;
        return z - std::floor(z / h) * h;
    };

    const PrintObject *ref     = nullptr;
    double             ref_top = -std::numeric_limits<double>::max();
    for (const PrintObject *po : m_objects) {
        if (po->config().belt_purge_tower_object.value || po->layers().empty())
            continue;
        const double top = po->layers().back()->print_z;
        if (top > ref_top) {
            ref_top = top;
            ref     = po;
        }
    }
    if (ref == nullptr)
        return;

    const double ref_offset = grid_offset(ref);

    for (PrintObject *po : m_objects) {
        if (po->layers().empty())
            continue;
        double delta = ref_offset - grid_offset(po);
        if (delta > 0.5 * h)
            delta -= h;
        else if (delta <= -0.5 * h)
            delta += h;
        po->belt_shift_layer_grid(delta);
    }
}

void Print::_plan_belt_purge()
{
    m_wipe_tower_data.clear();

    for (PrintObject *po : m_objects)
        if (po->config().belt_purge_tower_object.value)
            po->belt_undo_purge_plan();

    this->_align_belt_purge_layers();

    const unsigned int number_of_extruders = (unsigned int) m_config.filament_colour.values.size();

    m_wipe_tower_data.tool_ordering = ToolOrdering(*this, (unsigned int) -1, false);
    m_wipe_tower_data.tool_ordering.sort_and_build_data(*this, (unsigned int) -1, false);

    if (m_wipe_tower_data.tool_ordering.empty() || m_wipe_tower_data.tool_ordering.last_extruder() == unsigned(-1))
        throw Slic3r::SlicingError("The print is empty. The model is not printable with current print settings.");

    {
        bool         any_change = false;
        unsigned int cur        = m_wipe_tower_data.tool_ordering.first_extruder();
        for (const auto &lt : m_wipe_tower_data.tool_ordering.layer_tools())
            for (const unsigned int e : lt.extruders)
                if (e != cur) { any_change = true; cur = e; }
        if (! any_change)
            return;
    }

    this->throw_if_canceled();

    std::vector<float> flush_matrix(cast<float>(
        get_flush_volumes_matrix(m_config.flush_volumes_matrix.values, 0, m_config.nozzle_diameter.values.size(), number_of_extruders)));
    std::vector<std::vector<float>> wipe_volumes;
    for (unsigned int i = 0; i < number_of_extruders; ++i)
        wipe_volumes.push_back(std::vector<float>(flush_matrix.begin() + i * number_of_extruders,
                                                  flush_matrix.begin() + (i + 1) * number_of_extruders));
    const bool  use_flush_matrix = m_config.purge_in_prime_tower && m_config.single_extruder_multi_material;
    const float flush_multiplier = (float) m_config.flush_multiplier.get_at(0);

    {
        double obj_top_z = -1.;
        for (const PrintObject *po : m_objects) {
            if (po->config().belt_purge_tower_object.value)
                continue;
            if (!po->layers().empty())
                obj_top_z = std::max(obj_top_z, po->layers().back()->print_z);
            if (!po->support_layers().empty())
                obj_top_z = std::max(obj_top_z, po->support_layers().back()->print_z);
        }

        double       last_tc_z   = -1.;
        unsigned int cur_ext     = m_wipe_tower_data.tool_ordering.first_extruder();
        for (const auto &lt : m_wipe_tower_data.tool_ordering.layer_tools()) {
            if (obj_top_z >= 0. && lt.print_z > obj_top_z + EPSILON)
                break;
            for (const unsigned int e : lt.extruders)
                if (e != cur_ext) { last_tc_z = lt.print_z; cur_ext = e; }
        }
        if (last_tc_z >= 0.)
            for (PrintObject *po : m_objects)
                if (po->config().belt_purge_tower_object.value && !po->layers().empty()) {
                    po->belt_truncate_layers_above(last_tc_z);
                    break;
                }
    }

    PrintObject *prism_po = nullptr;
    for (PrintObject *po : m_objects)
        if (po->config().belt_purge_tower_object.value && !po->layers().empty()) { prism_po = po; break; }

    float  total_leftover      = 0.f;
    float  worst_layer_leftover = 0.f;
    double worst_layer_z       = 0.;

    unsigned int current_extruder_id = m_wipe_tower_data.tool_ordering.first_extruder();
    for (auto &layer_tools : m_wipe_tower_data.tool_ordering.layer_tools()) {
        float layer_leftover = 0.f;
        for (const unsigned int extruder_id : layer_tools.extruders) {
            if (extruder_id == current_extruder_id)
                continue;
            float volume_to_wipe = use_flush_matrix ?
                wipe_volumes[current_extruder_id][extruder_id] * flush_multiplier :
                (float) m_config.prime_volume;
            float leftover = layer_tools.wiping_extrusions().mark_wiping_extrusions(*this, current_extruder_id, extruder_id,
                                                                                    volume_to_wipe);
            layer_leftover += leftover;
            current_extruder_id = extruder_id;
        }

        if (prism_po != nullptr) {
            const auto &we = layer_tools.wiping_extrusions();
            prism_po->belt_drop_unclaimed_fills(
                prism_po->get_layer_at_printz(layer_tools.print_z, EPSILON),
                [&we, prism_po](const ExtrusionEntity *e) { return we.is_entity_overridden(e, prism_po, 0); });
        }

        layer_tools.wiping_extrusions().ensure_perimeters_infills_order(*this);
        if (layer_leftover > 0.f) {
            total_leftover += layer_leftover;
            if (layer_leftover > worst_layer_leftover) {
                worst_layer_leftover = layer_leftover;
                worst_layer_z        = layer_tools.print_z;
            }
        }
        this->throw_if_canceled();
    }

    if (total_leftover > 1.f) {
        this->active_step_add_warning(
            PrintStateBase::WarningLevel::CRITICAL, WarningCode{ "belt_purge_overflow" },
            Slic3r::format(_u8L("The belt purge tower cannot absorb the full purge volume: %1% mm³ in total could not "
                                "be purged (worst layer: %2% mm³ at height %3%). The print may show color bleeding. "
                                "Increase the belt purge tower width, or reduce flushing volumes."),
                           int(std::ceil(total_leftover)), int(std::ceil(worst_layer_leftover)),
                           Slic3r::float_to_string_decimal_point(worst_layer_z, 2)));
    }
}

void PrintObject::belt_shift_layer_grid(double delta)
{
    if (std::abs(delta) < EPSILON)
        return;
    for (Layer *layer : m_layers)
        layer->print_z += delta;
    for (SupportLayer *layer : m_support_layers)
        layer->print_z += delta;
    for (BeltBrimBand &band : m_belt_brim_prologue)
        band.print_z += delta;
    m_slicing_params.belt_floor_z_shift += delta;
    if (m_belt_floor_z_shift_cache_valid)
        m_belt_floor_z_shift_cached += delta;
    m_belt_global_z_offset += delta;
}

size_t PrintObject::belt_truncate_layers_above(coordf_t z)
{
    assert(m_belt_truncated_layers.empty());
    size_t keep = m_layers.size();
    while (keep > 0 && m_layers[keep - 1]->print_z > z + EPSILON)
        --keep;
    if (keep >= m_layers.size())
        return 0;
    const size_t removed = m_layers.size() - keep;
    m_belt_truncated_layers.assign(m_layers.begin() + keep, m_layers.end());
    m_layers.resize(keep);
    if (!m_layers.empty())
        m_layers.back()->upper_layer = nullptr;
    return removed;
}

size_t PrintObject::belt_drop_unclaimed_fills(Layer *layer, const std::function<bool(const ExtrusionEntity*)> &claimed)
{
    if (layer == nullptr)
        return 0;
    size_t dropped = 0;
    for (size_t ri = 0; ri < layer->regions().size(); ++ri) {
        LayerRegion *lr = layer->get_region(ri);
        auto        &ents = lr->fills.entities;
        ExtrusionEntitiesPtr keep;
        keep.reserve(ents.size());
        for (size_t i = 0; i < ents.size(); ++i) {
            if (claimed(ents[i])) {
                keep.emplace_back(ents[i]);
            } else {
                m_belt_dropped_fills.push_back(BeltDroppedFill{ layer, ri, i, ents[i] });
                ++dropped;
            }
        }
        ents = std::move(keep);
    }
    return dropped;
}

void PrintObject::belt_restore_dropped_fills()
{
    if (m_belt_dropped_fills.empty())
        return;
    std::stable_sort(m_belt_dropped_fills.begin(), m_belt_dropped_fills.end(),
                     [](const BeltDroppedFill &a, const BeltDroppedFill &b) {
                         if (a.layer != b.layer)           return a.layer < b.layer;
                         if (a.region_idx != b.region_idx) return a.region_idx < b.region_idx;
                         return a.index < b.index;
                     });
    for (const BeltDroppedFill &d : m_belt_dropped_fills) {
        auto &ents = d.layer->get_region(d.region_idx)->fills.entities;
        ents.insert(ents.begin() + std::min(d.index, ents.size()), d.entity);
    }
    m_belt_dropped_fills.clear();
}

void PrintObject::belt_restore_truncated_layers()
{
    if (m_belt_truncated_layers.empty())
        return;

    m_layers.insert(m_layers.end(), m_belt_truncated_layers.begin(), m_belt_truncated_layers.end());
    m_belt_truncated_layers.clear();
    for (size_t i = 0; i < m_layers.size(); ++i) {
        m_layers[i]->lower_layer = i == 0 ? nullptr : m_layers[i - 1];
        m_layers[i]->upper_layer = i + 1 < m_layers.size() ? m_layers[i + 1] : nullptr;
    }
}

}
