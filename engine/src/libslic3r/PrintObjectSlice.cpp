#include <algorithm>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <limits>

#include <cstddef>
#include <functional>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <string>
#include <math.h>
#include <cmath>
#include <iterator>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>
#include <vector>
#include <utility>

#include "Nonplanar/CurvedTopsSurface.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ElephantFootCompensation.hpp"
#include "ExPolygon.hpp"
#include "Exception.hpp"
#include "Flow.hpp"
#include "I18N.hpp"
#include "Layer.hpp"
#include "Model.hpp"
#include "GCode/ClearanceRepair.hpp"
#include "Nonplanar/HeadEnvelope.hpp"
#include "Nonplanar/NativePrint.hpp"
#include "Nonplanar/Wave.hpp"
#include "Nonplanar/ConformalShells.hpp"
#include "Nonplanar/FieldOrder.hpp"
#include "Nonplanar/AutoMode.hpp"
#include "Nonplanar/HeightField.hpp"
#include "Nonplanar/PatchTools.hpp"
#include "Nonplanar/LayerSchedule.hpp"
#include "Nonplanar/Multiplanar.hpp"
#include "Nonplanar/Slant.hpp"
#include "Nonplanar/MultiplanarConfirm.hpp"
#include "Nonplanar/NpField.hpp"
#include "Nonplanar/OneShotField.hpp"
#include "Nonplanar/OrientationField.hpp"
#include "MultiMaterialSegmentation.hpp"
#include "ObjectID.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "Print.hpp"
#include "BeltTransform.hpp"
#include "BeltSliceStrategy.hpp"
#include "Geometry.hpp"
#include "PrintConfig.hpp"
#include "PrintBase.hpp"
#include "ShortestPath.hpp"
#include "AABBMesh.hpp"
#include "libslic3r.h"
#include "TriangleMeshSlicer.hpp"
#include "TriangleMesh.hpp"
#include "Slicing.hpp"
#include "Surface.hpp"
#include "Utils.hpp"
#include "libslic3r/Feature/Interlocking/InterlockingGenerator.hpp"
#include "Config.hpp"
#include "SurfaceCollection.hpp"

#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

bool PrintObject::clip_multipart_objects = true;
bool PrintObject::infill_only_where_needed = false;

static coordf_t compute_slice_z(PrintObject* print_object, size_t i_layer, coordf_t lo, coordf_t hi)
{
    bool zaa_active   = false;
    coordf_t z_offset = 0.0;

    size_t num_regions = print_object->num_printing_regions();
    for (size_t rid = 0; rid < num_regions; ++rid) {
        const auto& rcfg = print_object->printing_region(rid).config();
        if (((rcfg.zaa_enabled && rcfg.zaa_mid_layer_slice) || print_object->curved_fallback_zaa()) && !print_object->auto_z_contouring())
            continue;
        if (rcfg.zaa_enabled || print_object->auto_z_contouring()) {
            const double min_z = rcfg.zaa_enabled ? rcfg.zaa_min_z.value : PrintRegionConfig::defaults().zaa_min_z.value;
            if (!zaa_active || min_z < z_offset)
                z_offset = min_z;
            zaa_active = true;
        }
    }

    if (!zaa_active || i_layer == 0) {
        return 0.5 * (lo + hi);
    }

    coordf_t slice_z = lo + z_offset;
    if ((slice_z < lo && !is_approx(slice_z, lo)) || (slice_z > hi && !is_approx(slice_z, hi))) {
        throw RuntimeError("Bad min Z value");
    }
    return slice_z;
}

LayerPtrs new_layers(
    PrintObject                 *print_object,
    const std::vector<coordf_t> &object_layers)
{
    LayerPtrs out;
    out.reserve(object_layers.size());
    auto     id   = int(print_object->slicing_parameters().raft_layers());
    coordf_t zmin = print_object->slicing_parameters().object_print_z_min;
    Layer   *prev = nullptr;
    for (size_t i_layer = 0; i_layer < object_layers.size(); i_layer += 2) {
        coordf_t lo = object_layers[i_layer];
        coordf_t hi = object_layers[i_layer + 1];
        coordf_t slice_z = compute_slice_z(print_object, i_layer, lo, hi);

        Layer *layer = new Layer(id ++, print_object, hi - lo, hi + zmin, slice_z);
        out.emplace_back(layer);
        if (prev != nullptr) {
            prev->upper_layer = layer;
            layer->lower_layer = prev;
        }
        prev = layer;
    }
    return out;
}

static std::vector<ExPolygons> slice_volume(
    const ModelVolume             &volume,
    const std::vector<float>      &zs,
    const MeshSlicingParamsEx     &params,
    const std::function<void()>   &throw_on_cancel_callback)
{
    std::vector<ExPolygons> layers;
    if (! zs.empty()) {
        indexed_triangle_set its = volume.mesh().its;
        if (its.indices.size() > 0) {
            MeshSlicingParamsEx params2 { params };
            params2.trafo = params2.trafo * volume.get_matrix();
            if (params2.trafo.rotation().determinant() < 0.)
                its_flip_triangles(its);
            layers = slice_mesh_ex(its, zs, params2, throw_on_cancel_callback);
            throw_on_cancel_callback();
        }
    }
    return layers;
}

static std::vector<ExPolygons> slice_volume(
    const ModelVolume                           &volume,
    const std::vector<float>                    &z,
    const std::vector<t_layer_height_range>     &ranges,
    const MeshSlicingParamsEx                   &params,
    const std::function<void()>                 &throw_on_cancel_callback)
{
    std::vector<ExPolygons> out;
    if (! z.empty() && ! ranges.empty()) {
        if (ranges.size() == 1 && z.front() >= ranges.front().first && z.back() < ranges.front().second) {
            out = slice_volume(volume, z, params, throw_on_cancel_callback);
        } else {
            std::vector<float>                     z_filtered;
            std::vector<std::pair<size_t, size_t>> n_filtered;
            z_filtered.reserve(z.size());
            n_filtered.reserve(2 * ranges.size());
            size_t i = 0;
            for (const t_layer_height_range &range : ranges) {
                for (; i < z.size() && z[i] < range.first; ++ i) ;
                size_t first = i;
                for (; i < z.size() && z[i] < range.second; ++ i)
                    z_filtered.emplace_back(z[i]);
                if (i > first)
                    n_filtered.emplace_back(std::make_pair(first, i));
            }
            if (! n_filtered.empty()) {
                std::vector<ExPolygons> layers = slice_volume(volume, z_filtered, params, throw_on_cancel_callback);
                out.assign(z.size(), ExPolygons());
                i = 0;
                for (const std::pair<size_t, size_t> &span : n_filtered)
                    for (size_t j = span.first; j < span.second; ++ j)
                        out[j] = std::move(layers[i ++]);
            }
        }
    }
    return out;
}
static inline bool model_volume_needs_slicing(const ModelVolume &mv)
{
    ModelVolumeType type = mv.type();
    return type == ModelVolumeType::MODEL_PART || type == ModelVolumeType::NEGATIVE_VOLUME || type == ModelVolumeType::PARAMETER_MODIFIER;
}

static std::vector<VolumeSlices> slice_volumes_inner(
    const PrintConfig                                        &print_config,
    const PrintObjectConfig                                  &print_object_config,
    const Transform3d                                        &object_trafo,
    ModelVolumePtrs                                           model_volumes,
    const std::vector<PrintObjectRegions::LayerRangeRegions> &layer_ranges,
    const std::vector<float>                                 &zs,
    const std::function<void()>                              &throw_on_cancel_callback,
    double                                                   *out_belt_min_z = nullptr)
{
    model_volumes_sort_by_id(model_volumes);

    std::vector<VolumeSlices> out;
    out.reserve(model_volumes.size());

    std::vector<t_layer_height_range> slicing_ranges;
    if (layer_ranges.size() > 1)
        slicing_ranges.reserve(layer_ranges.size());

    MeshSlicingParamsEx params_base;
    params_base.closing_radius = print_object_config.slice_closing_radius.value;
    params_base.extra_offset   = 0;
    params_base.trafo          = object_trafo;
    BeltSliceStrategy::apply_preslice_transforms(params_base.trafo, print_config, model_volumes, out_belt_min_z);
    params_base.resolution = print_config.resolution <= 0.001 ? 0.0f : 0.0025;
    switch (print_object_config.slicing_mode.value) {
    case SlicingMode::Regular:    params_base.mode = MeshSlicingParams::SlicingMode::Regular; break;
    case SlicingMode::EvenOdd:    params_base.mode = MeshSlicingParams::SlicingMode::EvenOdd; break;
    case SlicingMode::CloseHoles: params_base.mode = MeshSlicingParams::SlicingMode::Positive; break;
    }

    params_base.mode_below     = params_base.mode;

    const size_t num_extruders = print_config.filament_diameter.size();
    const bool   is_mm_painted = num_extruders > 1 && std::any_of(model_volumes.cbegin(), model_volumes.cend(), [](const ModelVolume *mv) { return mv->is_mm_painted(); });
    const auto   extra_offset = 0.f;

    for (const ModelVolume *model_volume : model_volumes)
        if (model_volume_needs_slicing(*model_volume)) {
            MeshSlicingParamsEx params { params_base };
            if (! model_volume->is_negative_volume())
                params.extra_offset = extra_offset;
            if (layer_ranges.size() == 1) {
                if (const PrintObjectRegions::LayerRangeRegions &layer_range = layer_ranges.front(); layer_range.has_volume(model_volume->id())) {
                    if (model_volume->is_model_part() && print_config.spiral_mode) {
                        auto it = std::find_if(layer_range.volume_regions.begin(), layer_range.volume_regions.end(),
                            [model_volume](const auto &slice){ return model_volume == slice.model_volume; });
                        params.mode = MeshSlicingParams::SlicingMode::PositiveLargestContour;
                        const PrintRegionConfig &region_config = it->region->config();
                        params.slicing_mode_normal_below_layer = size_t(region_config.bottom_shell_layers.value);
                        for (; params.slicing_mode_normal_below_layer < zs.size() && zs[params.slicing_mode_normal_below_layer] < region_config.bottom_shell_thickness - EPSILON;
                            ++ params.slicing_mode_normal_below_layer);
                    }
                    out.push_back({
                        model_volume->id(),
                        slice_volume(*model_volume, zs, params, throw_on_cancel_callback)
                    });
                }
            } else {
                assert(! print_config.spiral_mode);
                slicing_ranges.clear();
                for (const PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges)
                    if (layer_range.has_volume(model_volume->id()))
                        slicing_ranges.emplace_back(layer_range.layer_height_range);
                if (! slicing_ranges.empty())
                    out.push_back({
                        model_volume->id(),
                        slice_volume(*model_volume, zs, slicing_ranges, params, throw_on_cancel_callback)
                    });
            }
            if (! out.empty() && out.back().slices.empty())
                out.pop_back();
        }

    return out;
}

static inline VolumeSlices& volume_slices_find_by_id(std::vector<VolumeSlices> &volume_slices, const ObjectID id)
{
    auto it = lower_bound_by_predicate(volume_slices.begin(), volume_slices.end(), [id](const VolumeSlices &vs) { return vs.volume_id < id; });
    assert(it != volume_slices.end() && it->volume_id == id);
    return *it;
}

static inline bool overlap_in_xy(const PrintObjectRegions::BoundingBox &l, const PrintObjectRegions::BoundingBox &r)
{
    return ! (l.max().x() < r.min().x() || l.min().x() > r.max().x() ||
              l.max().y() < r.min().y() || l.min().y() > r.max().y());
}

static std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator layer_range_first(const std::vector<PrintObjectRegions::LayerRangeRegions> &layer_ranges, double z)
{
    auto  it = lower_bound_by_predicate(layer_ranges.begin(), layer_ranges.end(),
        [z](const PrintObjectRegions::LayerRangeRegions &lr) {
            return lr.layer_height_range.second < z && abs(lr.layer_height_range.second - z) > EPSILON;
        });
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z <= it->layer_height_range.second);
    if (z == it->layer_height_range.second)
        if (auto it_next = it; ++ it_next != layer_ranges.end() && it_next->layer_height_range.first == z)
            it = it_next;
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z <= it->layer_height_range.second);
    return it;
}

static std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator layer_range_next(
    const std::vector<PrintObjectRegions::LayerRangeRegions>            &layer_ranges,
    std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator   it,
    double                                                               z)
{
    for (; it->layer_height_range.second <= z + EPSILON; ++ it)
        assert(it != layer_ranges.end());
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z < it->layer_height_range.second);
    return it;
}

static std::vector<std::vector<ExPolygons>> slices_to_regions(
    const PrintConfig                                        &print_config,
    const PrintObject                                        &print_object,
    ModelVolumePtrs                                           model_volumes,
    const PrintObjectRegions                                 &print_object_regions,
    const std::vector<float>                                 &zs,
    std::vector<VolumeSlices>                               &&volume_slices,
    const bool                                                clip_multipart_objects,
    const std::function<void()>                              &throw_on_cancel_callback)
{
    model_volumes_sort_by_id(model_volumes);

    std::vector<std::vector<ExPolygons>> slices_by_region(print_object_regions.all_regions.size(), std::vector<ExPolygons>(zs.size(), ExPolygons()));

    std::vector<std::pair<size_t, float>> zs_complex;
    {
        size_t z_idx = 0;
        for (const PrintObjectRegions::LayerRangeRegions &layer_range : print_object_regions.layer_ranges) {
            for (; z_idx < zs.size() && zs[z_idx] < layer_range.layer_height_range.first; ++ z_idx) ;
            if (layer_range.volume_regions.empty()) {
            } else if (layer_range.volume_regions.size() == 1) {
                const ModelVolume *model_volume = layer_range.volume_regions.front().model_volume;
                assert(model_volume != nullptr);
                if (model_volume->is_model_part()) {
                    VolumeSlices &slices_src = volume_slices_find_by_id(volume_slices, model_volume->id());
                    auto         &slices_dst = slices_by_region[layer_range.volume_regions.front().region->print_object_region_id()];
                    for (; z_idx < zs.size() && zs[z_idx] < layer_range.layer_height_range.second; ++ z_idx)
                        slices_dst[z_idx] = std::move(slices_src.slices[z_idx]);
                }
            } else {
                zs_complex.reserve(zs.size());
                const bool bbox_z_in_layer_frame = !(print_config.belt_printer.value &&
                    BeltTransformPipeline::has_rotation(print_config));
                int num_model_parts = 0;
                for (const PrintObjectRegions::VolumeRegion &vr : layer_range.volume_regions)
                    if (vr.model_volume->is_model_part())
                        ++num_model_parts;
                const bool force_complex_for_belt = !bbox_z_in_layer_frame && num_model_parts > 1;
                for (; z_idx < zs.size() && zs[z_idx] < layer_range.layer_height_range.second; ++ z_idx) {
                    float z                          = zs[z_idx];
                    if (force_complex_for_belt) {
                        zs_complex.push_back({ z_idx, z });
                        continue;
                    }
                    int   idx_first_printable_region = -1;
                    bool  complex                    = false;
                    std::vector<int> printable_region_ids;
                    for (int idx_region = 0; idx_region < int(layer_range.volume_regions.size()); ++ idx_region) {
                        const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[idx_region];
                        if (!bbox_z_in_layer_frame || (region.bbox->min().z() <= z && region.bbox->max().z() >= z)) {
                            if (region.model_volume->is_model_part())
                                printable_region_ids.push_back(idx_region);

                            if (idx_first_printable_region == -1 && region.model_volume->is_model_part()) {
                                idx_first_printable_region = idx_region;
                            }
                            else if (idx_first_printable_region != -1) {
                                for (int idx_region2 = idx_first_printable_region; idx_region2 < idx_region; ++ idx_region2) {
                                    const PrintObjectRegions::VolumeRegion &region2 = layer_range.volume_regions[idx_region2];
                                    const bool region2_in_z = !bbox_z_in_layer_frame
                                        || (region2.bbox->min().z() <= z && region2.bbox->max().z() >= z);
                                    if (region2_in_z && overlap_in_xy(*region.bbox, *region2.bbox)) {
                                        complex = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (complex)
                        zs_complex.push_back({ z_idx, z });
                    else if (idx_first_printable_region >= 0) {
                        for (int printable_region_id : printable_region_ids) {
                            const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[printable_region_id];
                            append(slices_by_region[region.region->print_object_region_id()][z_idx], std::move(volume_slices_find_by_id(volume_slices, region.model_volume->id()).slices[z_idx]));
                        }
                    }
                }
            }
            throw_on_cancel_callback();
        }
    }

    if (! zs_complex.empty()) {
        std::vector<std::vector<VolumeSlices*>> layer_ranges_regions_to_slices(print_object_regions.layer_ranges.size(), std::vector<VolumeSlices*>());
        for (const PrintObjectRegions::LayerRangeRegions &layer_range : print_object_regions.layer_ranges) {
            std::vector<VolumeSlices*> &layer_range_regions_to_slices = layer_ranges_regions_to_slices[&layer_range - print_object_regions.layer_ranges.data()];
            layer_range_regions_to_slices.reserve(layer_range.volume_regions.size());
            for (const PrintObjectRegions::VolumeRegion &region : layer_range.volume_regions)
                layer_range_regions_to_slices.push_back(&volume_slices_find_by_id(volume_slices, region.model_volume->id()));
        }
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, zs_complex.size()),
            [&slices_by_region, &print_object_regions, &zs_complex, &layer_ranges_regions_to_slices, clip_multipart_objects, &throw_on_cancel_callback]
                (const tbb::blocked_range<size_t> &range) {
                float z              = zs_complex[range.begin()].second;
                auto  it_layer_range = layer_range_first(print_object_regions.layer_ranges, z);
                struct RegionSlice {
                    ExPolygons  expolygons;
                    int         region_id;
                    ObjectID    volume_id;
                    bool operator<(const RegionSlice &rhs) const {
                        bool this_empty = this->region_id < 0 || this->expolygons.empty();
                        bool rhs_empty  = rhs.region_id < 0 || rhs.expolygons.empty();
                        return ! this_empty && (rhs_empty || (this->region_id < rhs.region_id || (this->region_id == rhs.region_id && volume_id < rhs.volume_id)));
                    }
                };

                auto trim_overlap = [](ExPolygons& expolys_a, ExPolygons& expolys_b) {
                    ExPolygons trimming_a;
                    ExPolygons trimming_b;

                    for (ExPolygon& expoly_a : expolys_a) {
                        BoundingBox bbox_a = get_extents(expoly_a);
                        ExPolygons expolys_new;
                        for (ExPolygon& expoly_b : expolys_b) {
                            BoundingBox bbox_b = get_extents(expoly_b);
                            if (!bbox_a.overlap(bbox_b))
                                continue;

                            ExPolygons temp = intersection_ex(expoly_b, expoly_a, ApplySafetyOffset::Yes);
                            if (temp.empty())
                                continue;

                            if (expoly_a.contour.length() > expoly_b.contour.length())
                                trimming_a.insert(trimming_a.end(), temp.begin(), temp.end());
                            else
                                trimming_b.insert(trimming_b.end(), temp.begin(), temp.end());
                        }
                    }

                    expolys_a = diff_ex(expolys_a, trimming_a);
                    expolys_b = diff_ex(expolys_b, trimming_b);
                };

                std::vector<RegionSlice> temp_slices;
                for (size_t zs_complex_idx = range.begin(); zs_complex_idx < range.end(); ++ zs_complex_idx) {
                    auto [z_idx, z] = zs_complex[zs_complex_idx];
                    it_layer_range = layer_range_next(print_object_regions.layer_ranges, it_layer_range, z);
                    const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;
                    {
                        std::vector<VolumeSlices*> &layer_range_regions_to_slices = layer_ranges_regions_to_slices[it_layer_range - print_object_regions.layer_ranges.begin()];
                        temp_slices.clear();
                        temp_slices.reserve(layer_range.volume_regions.size());
                        for (VolumeSlices* &slices : layer_range_regions_to_slices) {
                            const PrintObjectRegions::VolumeRegion &volume_region = layer_range.volume_regions[&slices - layer_range_regions_to_slices.data()];
                            temp_slices.push_back({ std::move(slices->slices[z_idx]), volume_region.region ? volume_region.region->print_object_region_id() : -1, volume_region.model_volume->id() });
                        }
                    }
                    for (int idx_region = 0; idx_region < int(layer_range.volume_regions.size()); ++ idx_region)
                        if (! temp_slices[idx_region].expolygons.empty()) {
                            const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[idx_region];
                            if (region.model_volume->is_modifier()) {
                                assert(region.parent > -1);
                                bool next_region_same_modifier = idx_region + 1 < int(temp_slices.size()) && layer_range.volume_regions[idx_region + 1].model_volume == region.model_volume;
                                RegionSlice &parent_slice = temp_slices[region.parent];
                                RegionSlice &this_slice   = temp_slices[idx_region];
                                ExPolygons   source       = std::move(this_slice.expolygons);
                                if (parent_slice.expolygons.empty()) {
                                    this_slice  .expolygons.clear();
                                } else {
                                    this_slice  .expolygons = intersection_ex(parent_slice.expolygons, source);
                                    parent_slice.expolygons = diff_ex        (parent_slice.expolygons, source);
                                }
                                if (next_region_same_modifier)
                                    temp_slices[idx_region + 1].expolygons = std::move(source);
                            } else if ((region.model_volume->is_model_part() && clip_multipart_objects) || region.model_volume->is_negative_volume()) {
                                for (int idx_region2 = 0; idx_region2 < idx_region; ++ idx_region2)
                                    if (! temp_slices[idx_region2].expolygons.empty()) {
#if 1
                                        if (const PrintObjectRegions::VolumeRegion& region2 = layer_range.volume_regions[idx_region2];
                                            !region2.model_volume->is_negative_volume() && overlap_in_xy(*region.bbox, *region2.bbox))
                                            temp_slices[idx_region2].expolygons = diff_ex(temp_slices[idx_region2].expolygons, temp_slices[idx_region].expolygons);
#else
                                        const PrintObjectRegions::VolumeRegion& region2 = layer_range.volume_regions[idx_region2];
                                        if (!region2.model_volume->is_negative_volume() && overlap_in_xy(*region.bbox, *region2.bbox))
                                            if (!region.model_volume->is_negative_volume())
                                                trim_overlap(temp_slices[idx_region2].expolygons, temp_slices[idx_region].expolygons);
                                            else
                                                temp_slices[idx_region2].expolygons = diff_ex(temp_slices[idx_region2].expolygons, temp_slices[idx_region].expolygons);
#endif
                                    }
                            }
                        }
                    std::sort(temp_slices.begin(), temp_slices.end());
                    temp_slices.erase(std::find_if(temp_slices.begin(), temp_slices.end(), [](const auto &slice) { return slice.region_id == -1 || slice.expolygons.empty(); }), temp_slices.end());
                    for (int i = 0; i < int(temp_slices.size());) {
                        int j = i;
                        bool merged = false;
                        ExPolygons &expolygons = temp_slices[i].expolygons;
                        for (++ j; j < int(temp_slices.size()) && temp_slices[i].region_id == temp_slices[j].region_id; ++ j)
                            if (ExPolygons &expolygons2 = temp_slices[j].expolygons; ! expolygons2.empty()) {
                                if (expolygons.empty()) {
                                    expolygons = std::move(expolygons2);
                                } else {
                                    append(expolygons, std::move(expolygons2));
                                    merged = true;
                                }
                            }
                        if (merged && clip_multipart_objects)
                            expolygons = closing_ex(expolygons, float(scale_(EPSILON)));
                        slices_by_region[temp_slices[i].region_id][z_idx] = std::move(expolygons);
                        i = j;
                    }
                    throw_on_cancel_callback();
                }
            });
    }

    return slices_by_region;
}

bool doesVolumeIntersect(VolumeSlices& vs1, VolumeSlices& vs2)
{
    if (vs1.volume_id == vs2.volume_id) return true;
    if (vs1.slices.size() != vs2.slices.size()) return false;

    auto& vs1s = vs1.slices;
    auto& vs2s = vs2.slices;
    bool is_intersect = false;

    tbb::parallel_for(tbb::blocked_range<int>(0, vs1s.size()),
        [&vs1s, &vs2s, &is_intersect](const tbb::blocked_range<int>& range) {
            for (auto i = range.begin(); i != range.end(); ++i) {
                if (vs1s[i].empty()) continue;

                if (overlaps(vs1s[i], vs2s[i])) {
                    is_intersect = true;
                    break;
                }
                if (i + 1 != vs2s.size() && overlaps(vs1s[i], vs2s[i + 1])) {
                    is_intersect = true;
                    break;
                }
                if (i - 1 >= 0 && overlaps(vs1s[i], vs2s[i - 1])) {
                    is_intersect = true;
                    break;
                }
            }
        });
    return is_intersect;
}

bool groupingVolumes(std::vector<VolumeSlices> objSliceByVolume, std::vector<groupedVolumeSlices>& groups, double resolution, int firstLayerReplacedBy)
{
    std::vector<int> groupIndex(objSliceByVolume.size(), -1);
    double offsetValue = 0.05 / SCALING_FACTOR;

    std::vector<std::vector<int>> osvIndex;
    for (int i = 0; i != objSliceByVolume.size(); ++i) {
        for (int j = 0; j != objSliceByVolume[i].slices.size(); ++j) {
            osvIndex.push_back({ i,j });
        }
    }

    tbb::parallel_for(tbb::blocked_range<int>(0, osvIndex.size()),
        [&osvIndex, &objSliceByVolume, &resolution](const tbb::blocked_range<int>& range) {
            for (auto k = range.begin(); k != range.end(); ++k) {
                for (ExPolygon& poly_ex : objSliceByVolume[osvIndex[k][0]].slices[osvIndex[k][1]])
                    poly_ex.douglas_peucker(resolution);
            }
        });

    tbb::parallel_for(tbb::blocked_range<int>(0, osvIndex.size()),
        [&osvIndex, &objSliceByVolume,&offsetValue](const tbb::blocked_range<int>& range) {
            for (auto k = range.begin(); k != range.end(); ++k) {
                objSliceByVolume[osvIndex[k][0]].slices[osvIndex[k][1]] = offset_ex(objSliceByVolume[osvIndex[k][0]].slices[osvIndex[k][1]], offsetValue);
            }
        });

    for (int i = 0; i != objSliceByVolume.size(); ++i) {
        if (groupIndex[i] < 0) {
            groupIndex[i] = i;
        }
        for (int j = i + 1; j != objSliceByVolume.size(); ++j) {
            if (doesVolumeIntersect(objSliceByVolume[i], objSliceByVolume[j])) {
                if (groupIndex[j] < 0) groupIndex[j] = groupIndex[i];
                if (groupIndex[j] != groupIndex[i]) {
                    int retain = std::min(groupIndex[i], groupIndex[j]);
                    int cover = std::max(groupIndex[i], groupIndex[j]);
                    for (int k = 0; k != objSliceByVolume.size(); ++k) {
                        if (groupIndex[k] == cover) groupIndex[k] = retain;
                    }
                }
            }

        }
    }

    std::vector<int> groupVector{};
    for (int gi : groupIndex) {
        bool exist = false;
        for (int gv : groupVector) {
            if (gv == gi) {
                exist = true;
                break;
            }
        }
        if (!exist) groupVector.push_back(gi);
    }

    groups.clear();

    for (int gv : groupVector) {
        groupedVolumeSlices gvs;
        gvs.groupId = gv;
        for (int i = 0; i != objSliceByVolume.size(); ++i) {
            if (groupIndex[i] == gv) {
                gvs.volume_ids.push_back(objSliceByVolume[i].volume_id);
                append(gvs.slices, objSliceByVolume[i].slices[firstLayerReplacedBy]);
            }
        }

        gvs.slices = offset_ex(union_ex(gvs.slices), -offsetValue);
        for (ExPolygon& poly_ex : gvs.slices)
            poly_ex.douglas_peucker(resolution);

        groups.push_back(gvs);
    }
    return true;
}

std::vector<VolumeSlices> findPartVolumes(const std::vector<VolumeSlices>& objSliceByVolume, ModelVolumePtrs model_volumes) {
    std::vector<VolumeSlices> outPut;
    for (const auto& vs : objSliceByVolume) {
        for (const auto& mv : model_volumes) {
            if (vs.volume_id == mv->id() && mv->is_model_part()) outPut.push_back(vs);
        }
    }
    return outPut;
}

void applyNegtiveVolumes(ModelVolumePtrs model_volumes, const std::vector<VolumeSlices>& objSliceByVolume, std::vector<groupedVolumeSlices>& groups, double resolution) {
    ExPolygons negTotal;
    for (const auto& vs : objSliceByVolume) {
        for (const auto& mv : model_volumes) {
            if (vs.volume_id == mv->id() && mv->is_negative_volume()) {
                if (vs.slices.size() > 0) {
                    append(negTotal, vs.slices.front());
                }
            }
        }
    }

    for (auto& g : groups) {
        g.slices = diff_ex(g.slices, negTotal);
        for (ExPolygon& poly_ex : g.slices)
            poly_ex.douglas_peucker(resolution);
    }
}

void reGroupingLayerPolygons(std::vector<groupedVolumeSlices>& gvss, ExPolygons &eps, double resolution)
{
    std::vector<int> epsIndex;
    epsIndex.resize(eps.size(), -1);

    auto gvssc = gvss;
    auto epsc = eps;

    for (ExPolygon& poly_ex : epsc)
        poly_ex.douglas_peucker(resolution);

    for (int i = 0; i != gvssc.size(); ++i) {
        for (ExPolygon& poly_ex : gvssc[i].slices)
            poly_ex.douglas_peucker(resolution);
    }

    tbb::parallel_for(tbb::blocked_range<int>(0, epsc.size()),
        [&epsc, &gvssc, &epsIndex](const tbb::blocked_range<int>& range) {
            for (auto ie = range.begin(); ie != range.end(); ++ie) {
                if (epsc[ie].area() <= 0)
                    continue;

                double minArea = epsc[ie].area();
                for (int iv = 0; iv != gvssc.size(); iv++) {
                    auto clipedExPolys = diff_ex(epsc[ie], gvssc[iv].slices);
                    double area = 0;
                    for (const auto& ce : clipedExPolys) {
                        area += ce.area();
                    }
                    if (area < minArea) {
                        minArea = area;
                        epsIndex[ie] = iv;
                    }
                }
            }
        });

    for (int iv = 0; iv != gvss.size(); iv++)
        gvss[iv].slices.clear();

    for (int ie = 0; ie != eps.size(); ie++) {
        if (epsIndex[ie] >= 0)
            gvss[epsIndex[ie]].slices.push_back(eps[ie]);
    }
}

void groupingVolumesForBrim(PrintObject* object, LayerPtrs& layers, int firstLayerReplacedBy)
{
    const auto           scaled_resolution = scaled<double>(object->print()->config().resolution.value);
    auto partsObjSliceByVolume = findPartVolumes(object->firstLayerObjSliceMod(), object->model_object()->volumes);
    groupingVolumes(partsObjSliceByVolume, object->firstLayerObjGroupsMod(), scaled_resolution, firstLayerReplacedBy);
    applyNegtiveVolumes(object->model_object()->volumes, object->firstLayerObjSliceMod(), object->firstLayerObjGroupsMod(), scaled_resolution);

    reGroupingLayerPolygons(object->firstLayerObjGroupsMod(), layers.front()->lslices, scaled_resolution);
}

static double floating_seed_area(const PrintObject &object)
{
    constexpr double MaxSeedDrop = 1.;
    double           largest     = 0.;
    for (size_t i = 1; i < object.layer_count(); ++i) {
        const Layer *layer  = object.get_layer(int(i));
        const double bottom = layer->print_z - layer->height;
        ExPolygons   below;
        for (size_t j = i; j-- > 0 && object.get_layer(int(j))->print_z >= bottom - MaxSeedDrop;)
            append(below, object.get_layer(int(j))->lslices);
        const BoundingBox box_below = get_extents(below);
        for (const ExPolygon &island : layer->lslices)
            if (!get_extents(island).overlap(box_below) || intersection_ex(ExPolygons{ island }, below).empty())
                largest = std::max(largest, island.area() * SCALING_FACTOR * SCALING_FACTOR);
    }
    return largest;
}

static std::pair<double, double> curved_top_band(PrintObject &object, const SlicingParameters &params, std::vector<coordf_t> &profile)
{
    constexpr double BandHeight = 0.2;
    const PrintObjectConfig &config = object.config();
    if (!curved_tops_mode(config.nonplanar_mode.value))
        return {};
    const std::vector<coordf_t> layers = generate_object_layers(params, profile, config.precise_z_height.value);
    double coarsest = 0.;
    for (size_t i = 2; i + 1 < layers.size(); i += 2)
        coarsest = std::max(coarsest, layers[i + 1] - layers[i]);
    if (coarsest <= BandHeight + EPSILON)
        return {};
    const double cos_low = std::cos(Geometry::deg2rad(1.)), cos_high = std::cos(Geometry::deg2rad(config.nonplanar_curved_max_angle.value));
    const indexed_triangle_set mesh = Nonplanar::slicing_frame_mesh(object);
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
    for (const stl_triangle_vertex_indices &f : mesh.indices) {
        const Vec3d a = mesh.vertices[f[0]].cast<double>(), b = mesh.vertices[f[1]].cast<double>(), c = mesh.vertices[f[2]].cast<double>();
        const Vec3d n = (b - a).cross(c - a);
        const double length = n.norm();
        if (length < 0.02 || n.z() <= 0.)
            continue;
        const double nz = n.z() / length;
        if (nz >= cos_low || nz < cos_high)
            continue;
        lo = std::min({ lo, a.z(), b.z(), c.z() });
        hi = std::max({ hi, a.z(), b.z(), c.z() });
    }
    if (lo > hi)
        return {};
    const int shells = curved_top_layers(object);
    lo -= (shells + 1) * coarsest;
    std::vector<double> banded;
    bool changed = false;
    for (size_t i = 0; i + 1 < layers.size(); i += 2) {
        double h = layers[i + 1] - layers[i];
        if (i > 0 && layers[i + 1] >= lo && layers[i] <= hi && h > BandHeight + EPSILON) {
            h = BandHeight;
            changed = true;
        }
        banded.push_back(layers[i]);
        banded.push_back(h);
    }
    if (!changed)
        return {};
    profile = smooth_height_profile(banded, params, HeightProfileSmoothingParams(5, true));
    BOOST_LOG_TRIVIAL(info) << object.model_object()->name << ": curved top band " << lo << "-" << hi << " mm at " << BandHeight << " mm layers";
    return { std::max(0., lo), hi };
}

void PrintObject::slice()
{
    if (! this->set_started(posSlice))
        return;
    const NonplanarMode previous_mode = m_config.nonplanar_mode.value;
    if (m_auto_planar_pass) {
        m_config.nonplanar_mode.value = NonplanarMode::Off;
        m_auto_planar_top_acceleration = m_config.top_surface_acceleration.values;
        for (size_t i = 0; i < m_config.top_surface_acceleration.values.size() && !m_auto_planar_exact; ++i)
            if (const double fallback = m_config.nonplanar_fallback_top_acceleration.get_at(i);
                fallback > 0. && m_config.top_surface_acceleration.values[i] == m_config.nonplanar_raised_top_acceleration.get_at(i))
                m_config.top_surface_acceleration.values[i] = fallback;
        m_auto_planar_pass            = false;
        m_auto_planar                 = true;
    } else if (m_auto_planar) {
        m_auto_floating_planar        = false;
        m_auto_plain_reason.clear();
        m_auto_planar_exact           = false;
        m_config.nonplanar_mode.value = NonplanarMode::Auto;
        m_config.top_surface_acceleration.values = m_auto_planar_top_acceleration;
        m_auto_planar                 = false;
    }
    m_requested_nonplanar_mode = m_auto_planar ? NonplanarMode::Auto : m_config.nonplanar_mode.value;
    if (m_config.nonplanar_mode.value == NonplanarMode::Off && !m_auto_floating_planar && Nonplanar::auto_rotary_requested(*this))
        m_config.nonplanar_mode.value = NonplanarMode::Auto;
    if (m_auto_support_applied) {
        m_config.enable_support.value = false;
        m_config.support_type.value   = m_auto_support_type;
        m_config.support_style.value  = m_auto_support_style;
        m_config.support_critical_regions_only.value = m_auto_support_critical;
        m_auto_support_applied        = false;
    }
    m_auto_support = false;
    m_auto_support_need = NoNeedSupp;
    if (const std::string why = Nonplanar::auto_rotary_planar(*this); !why.empty()) {
        m_config.nonplanar_mode.value = NonplanarMode::Off;
        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "rotary_planar", warning_object(this->model_object()->name) }, why);
    }
    if (Nonplanar::wave_enabled(*this))
        m_config.nonplanar_mode.value = NonplanarMode::Off;
    bool oneshot = !Nonplanar::wave_enabled(*this) && Nonplanar::oneshot_requested(*this);
    const int raft_before = m_config.raft_layers.value;
    if (m_oneshot && m_oneshot->plinth_applied) {
        m_config.raft_layers.value                = m_oneshot->plinth_user_layers;
        m_config.raft_expansion.value             = m_oneshot->plinth_user_expansion;
        m_config.raft_first_layer_expansion.value = m_oneshot->plinth_user_first_expansion;
        m_oneshot->plinth_applied                 = false;
    }
    if (oneshot)
        Nonplanar::solve_oneshot(*this);
    if (oneshot && m_oneshot->offer != Nonplanar::OneShotState::Offer::Auto) {
        m_config.nonplanar_mode.value = this->oneshot_state().field_path() ? NonplanarMode::OneShot : NonplanarMode::Off;
        if (this->oneshot_state().field_path() && m_config.raft_layers.value > 0)
            throw Slic3r::SlicingError(L("Rotary curved layers cannot be printed on a raft."));
        if (Nonplanar::OneShotState &state = this->oneshot_state();
            state.route != nullptr && state.plinth_layers > 0 && state.offer != Nonplanar::OneShotState::Offer::Control) {
            state.plinth_user_layers          = m_config.raft_layers.value;
            state.plinth_user_expansion       = m_config.raft_expansion.value;
            state.plinth_user_first_expansion = m_config.raft_first_layer_expansion.value;
            state.plinth_applied              = true;
            for (const auto &[key, value] : Nonplanar::plinth_overrides(state.plinth_layers))
                m_config.set_deserialize_strict(key, value);
        }
    } else if (m_oneshot && m_oneshot->offer == Nonplanar::OneShotState::Offer::Planar) {
        m_config.nonplanar_mode.value = NonplanarMode::Off;
    } else if (m_oneshot && m_oneshot->offer == Nonplanar::OneShotState::Offer::Auto && !m_auto_planar) {
        oneshot = false;
        m_config.nonplanar_mode.value = NonplanarMode::Auto;
    }
    std::shared_ptr<const Nonplanar::NpField> zonly_field;
    m_field_zonly = false;
    if (m_config.nonplanar_mode.value == NonplanarMode::Field && !m_config.nonplanar_field_file.value.empty() &&
        m_config.nonplanar_atom_toolpath.value.empty()) {
        BoundingBoxf3 box;
        for (const stl_vertex &v : Nonplanar::slicing_frame_mesh(*this).vertices)
            box.merge(v.cast<double>());
        try {
            auto f = std::make_shared<const Nonplanar::NpField>(
                Nonplanar::load_npfield_for(m_config.nonplanar_field_file.value, box, Nonplanar::plate_offset(*this)));
            if (Nonplanar::in_plane_spread(*f) < Nonplanar::OneShotState::ZOnlySpread) {
                zonly_field                   = std::move(f);
                m_field_zonly                 = true;
                m_config.nonplanar_mode.value = NonplanarMode::Off;
            }
        } catch (const RuntimeError &ex) {
            throw Slic3r::SlicingError(ex.what());
        }
    }
    if (m_config.nonplanar_mode.value != previous_mode || m_config.raft_layers.value != raft_before) {
        m_slicing_params.valid = false;
        this->update_slicing_parameters();
    }
    m_print->set_status(5, L("Slicing mesh"), PrintBase::SlicingStatus::RELOAD_SCENE);
    std::vector<coordf_t> layer_height_profile;
    this->update_layer_height_profile(*this->model_object(), m_slicing_params, layer_height_profile);
    if (oneshot && this->oneshot_state().field() && this->oneshot_state().z_only)
        layer_height_profile = Nonplanar::layer_profile(*this->oneshot_state().solved, m_slicing_params.first_object_layer_height,
                                                        m_slicing_params.object_print_z_height(), m_slicing_params.min_layer_height,
                                                        m_slicing_params.max_layer_height);
    else if (zonly_field) {
        const int threshold = m_config.support_threshold_angle.value > 0 ? m_config.support_threshold_angle.value : 30;
        layer_height_profile = Nonplanar::layer_profile(
            *zonly_field, m_slicing_params.first_object_layer_height, m_slicing_params.object_print_z_height(),
            m_slicing_params.min_layer_height, m_slicing_params.max_layer_height,
            Nonplanar::supported_underside_bands(Nonplanar::slicing_frame_mesh(*this), threshold, 2. * m_config.layer_height.value),
            m_config.layer_height.value);
    }
    else if (Nonplanar::field_layers_considered(*this, m_auto_planar && !m_auto_planar_exact))
        if (std::vector<coordf_t> field = Nonplanar::field_layer_profile(*this); !field.empty())
            layer_height_profile = std::move(field);
    m_curved_band = curved_top_band(*this, m_slicing_params, layer_height_profile);
    m_print->throw_if_canceled();
    m_typed_slices = false;
    m_belt_min_z              = 0.;
    m_belt_global_z_offset    = 0.;
    this->clear_layers();
    const auto object_layers = generate_object_layers(m_slicing_params, layer_height_profile, m_config.precise_z_height.value);
    m_auto_z_contouring = Nonplanar::auto_z_contouring(*this, object_layers);
    m_layers = new_layers(this, object_layers);
    this->slice_volumes();
    m_print->throw_if_canceled();
    if (const RepairedMeshErrors repaired = this->model_object()->get_object_stl_stats().repaired_errors; repaired.repaired()) {
        const std::string message = (boost::format(_u8L("%1%: mesh repaired on load: %2%.")) % this->model_object()->name % repaired.summary()).str();
        BOOST_LOG_TRIVIAL(warning) << message;
        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "mesh_repaired", warning_object(this->model_object()->name) }, message);
    }

    if (std::abs(m_slicing_params.belt_floor_shear_factor) > EPSILON) {
        double z_shift_val = (m_belt_min_z < 0.) ? -m_belt_min_z : 0.;
        m_slicing_params.belt_floor_z_shift = z_shift_val;
    }

    this->find_nonplanar_surfaces();
    m_print->throw_if_canceled();
    int firstLayerReplacedBy = 0;

#if 0
    std::string warning = fix_slicing_errors(this, m_layers, [this](){ m_print->throw_if_canceled(); }, firstLayerReplacedBy);
    m_print->throw_if_canceled();
#endif

    this->_transform_hole_to_polyholes();

    groupingVolumesForBrim(this, m_layers, firstLayerReplacedBy);

    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, m_layers.size()),
        [this](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                m_print->throw_if_canceled();
                Layer &layer = *m_layers[layer_idx];
                layer.lslices_bboxes.clear();
                layer.lslices_bboxes.reserve(layer.lslices.size());
                for (const ExPolygon &expoly : layer.lslices)
                	layer.lslices_bboxes.emplace_back(get_extents(expoly));
                layer.backup_untyped_slices();
            }
        });
    if (m_layers.empty())
        throw Slic3r::SlicingError(L("No layers were detected. You might want to repair your STL file(s) or check their size or thickness and retry.\n"),
                                   "no_printable_layers", warning_object(this->model_object()->name));

    {
        const auto &pcfg = this->print()->config();
        BOOST_LOG_TRIVIAL(trace) << "Belt global check: belt_printer=" << pcfg.belt_printer.value
            << " belt_slice_rotation=" << int(pcfg.belt_slice_rotation.value)
            << " object=" << this->model_object()->name;
        if (pcfg.belt_printer.value) {

            Point inst_shift = this->instances().empty() ? Point(0, 0)
                : this->instances().front().shift - this->center_offset();
            BOOST_LOG_TRIVIAL(trace) << "Belt global: object " << this->model_object()->name
                << " instances=" << this->instances().size()
                << " shift=(" << unscale<double>(inst_shift.x()) << ", " << unscale<double>(inst_shift.y()) << ")";

            double belt_z_shift = std::min(m_belt_min_z, 0.);
            double global_z_offset = belt_z_shift;

            {
                Transform3d T_fwd = BeltTransformPipeline::build_forward_transform(pcfg);
                Vec3d c_off(unscale<double>(m_center_offset.x()),
                            unscale<double>(m_center_offset.y()),
                            0.);
                double centering_z_corr = (T_fwd.linear() * c_off).z();
                global_z_offset += centering_z_corr;
            }

            {
                Transform3d T = BeltTransformPipeline::build_forward_transform(pcfg);
                Vec3d d(unscale<double>(inst_shift.x()), unscale<double>(inst_shift.y()), 0.);
                Vec3d c = T.linear() * d - d;
                global_z_offset += c.z();

                BOOST_LOG_TRIVIAL(trace) << "Belt preslice_global: correction=("
                    << c.x() << ", " << c.y() << ", " << c.z() << ")"
                    << " belt_z_shift=" << belt_z_shift << " (m_belt_min_z=" << m_belt_min_z << ")";
            }

            BOOST_LOG_TRIVIAL(trace) << "Belt global: z_offset=" << global_z_offset
                << " (" << this->print()->objects().size() << " objects on the plate)";
            m_belt_global_z_offset = global_z_offset;
            if (std::abs(global_z_offset) > EPSILON) {
                for (Layer *layer : m_layers)
                    layer->print_z += global_z_offset;
                m_slicing_params.belt_floor_z_shift += global_z_offset;
            }
            if (!m_layers.empty()) {
                BOOST_LOG_TRIVIAL(trace) << "Belt global: first_layer_z=" << m_layers.front()->print_z
                    << " last_layer_z=" << m_layers.back()->print_z
                    << " num_layers=" << m_layers.size()
                    << " center_offset=(" << unscale<double>(m_center_offset.x())
                    << ", " << unscale<double>(m_center_offset.y()) << ")";
            }

            m_belt_floor_z_shift_cached      = m_slicing_params.belt_floor_z_shift;
            m_belt_floor_z_shift_cache_valid = true;
        }
    }

    const bool oneshot_support = oneshot || m_field_zonly;
    if (!m_config.enable_support.value && m_config.nonplanar_auto_support.value && !m_layers.empty() && !Nonplanar::rotary_auto_measurement() &&
        (m_config.nonplanar_mode.value == NonplanarMode::Auto || m_auto_planar || Nonplanar::auto_rotary_requested(*this) || oneshot_support)) {
        m_auto_support_need = this->is_support_necessary();
        constexpr double MinFloatingSeed = 30.;
        const double     seed            = m_auto_support_need == SharpTail ? floating_seed_area(*this) : 0.;
        m_auto_support = !m_auto_floating_planar && seed >= MinFloatingSeed;
        BOOST_LOG_TRIVIAL(info) << "Support guard " << this->model_object()->name << ": Orca's check " << int(m_auto_support_need)
                                << ", largest region starting in the air " << seed << " mm2" << (m_auto_support ? ", supported" : "");
    } else if (oneshot_support && !m_layers.empty())
        m_auto_support_need = this->is_support_necessary();

    this->set_done(posSlice);
}

static bool smoothest_tops_head_known(const PrintConfig &config)
{
    std::string warning;
    Nonplanar::resolve_head_envelope(config, &warning);
    return warning.empty();
}

static void note_curved_drop(std::vector<CurvedSurfaceDrop> &drops, const NonplanarSurface &surface, const char *reason)
{
    CurvedSurfaceDrop &dropped = drops.emplace_back();
    dropped.volume = surface.volume;
    dropped.reason = reason;
    for (const auto &facet : surface.mesh)
        dropped.facets.push_back(facet.first);
}

void PrintObject::drop_curved_planar_surfaces()
{
    const auto planar = [this](const NonplanarSurface &surface) { return m_curved_planar_surfaces.count(surface.stable_id) != 0; };
    for (const NonplanarSurface &surface : m_nonplanar_surfaces)
        if (planar(surface))
            note_curved_drop(m_curved_dropped, surface, "access");
    m_nonplanar_surfaces.erase(std::remove_if(m_nonplanar_surfaces.begin(), m_nonplanar_surfaces.end(), planar), m_nonplanar_surfaces.end());
}

void
PrintObject::find_nonplanar_surfaces()
{
    m_curved_dropped.clear();
    if(!curved_tops_mode(m_config.nonplanar_mode.value)) {
        BOOST_LOG_TRIVIAL(debug) << "Find nonplanar surfaces - disabled";
        return;
    }
    if (m_print->config().nonplanar_kinematics.value != RotaryLayout::None || m_print->config().belt_printer.value)
        throw Slic3r::SlicingError("Curved tops (3-axis) requires a fixed print head and a horizontal bed.");
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id)
        if (this->printing_region(region_id).config().zaa_enabled.value)
            throw Slic3r::SlicingError(_u8L("Nonplanar layers and Z contouring cannot be used together. "
                                            "Disable one of them for this object."));

    BOOST_LOG_TRIVIAL(info) << "Find nonplanar surfaces - start";

    if (curved_tops_mode(m_config.nonplanar_mode.value) && !smoothest_tops_head_known(m_print->config())) {
        m_nonplanar_surfaces.clear();
        m_curved_upward_area = 0.;
        CurvedSurfaceRejections rejections;
        rejections.head_unknown = true;
        note_curved_surface_rejections(*this, rejections);
        BOOST_LOG_TRIVIAL(warning) << this->model_object()->name << ": no print head model for this printer; Curved tops prints flat tops";
        return;
    }

    m_nonplanar_surfaces.clear();
    m_curved_upward_area = 0.;
    CurvedSurfaceRejections rejections;

    const ModelVolumePtrs volumes = this->model_object()->volumes;
    for (ModelVolumePtrs::const_iterator it = volumes.begin(); it != volumes.end(); ++ it) {
        if (! (*it)->is_modifier()) {
            const int volume = int(it - volumes.begin());
            TriangleMesh tmesh = (*it)->mesh();
            tmesh.transform(this->trafo_centered() * (*it)->get_matrix(), true);
            AABBMesh mesh(tmesh, true);
            std::map<int, NonplanarFacet> facets;

            for (int face_id = 0; face_id < mesh.indices().size(); ++ face_id) {
                auto &face = mesh.indices(face_id);
                Vec3d normal = mesh.normal_by_face_id(face_id);

                if (normal.z() > 0.) {
                    const its_triangle triangle = its_triangle_vertices(*mesh.get_triangle_mesh(), face_id);
                    const Vec3d a = triangle[0].cast<double>(), b = triangle[1].cast<double>(), c = triangle[2].cast<double>();
                    m_curved_upward_area += 0.5 * std::abs((b - a).cross(c - a).z());
                }
                if (normal.z() >= std::cos(m_config.nonplanar_curved_max_angle.value * 3.14159265/180.0)) {
                    NonplanarFacet new_facet;
                    new_facet.normal.x = normal.x();
                    new_facet.normal.y = normal.y();
                    new_facet.normal.z = normal.z();

                    Vec3i32 neighbor = mesh.face_neighbor_index()[face_id];
                    its_triangle vertex = its_triangle_vertices(*mesh.get_triangle_mesh(), face_id);
                    for (int j=0; j<=2 ;j++) {
                        new_facet.vertex[j].x = vertex[j].x();
                        new_facet.vertex[j].y = vertex[j].y();
                        new_facet.vertex[j].z = vertex[j].z();
                        new_facet.neighbor[j] = neighbor[j];
                    }
                    new_facet.calculate_stats();
                    facets[face_id] = new_facet;
                }
            }

            NonplanarSurface nf = NonplanarSurface(facets);

            NonplanarSurfaces volume_surfaces = nf.group_surfaces();
            for (NonplanarSurface &surface : volume_surfaces)
                surface.volume = volume;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
            for (size_t id = 0; id < volume_surfaces.size(); ++ id) {
                auto& surface = volume_surfaces[id];
                Surfaces surfaces;
                surfaces_append(surfaces, surface.horizontal_projection(), SurfaceType::stTopNonplanar);
                SurfaceCollection c(surfaces);
                c.export_to_svg(debug_out_path("0_find_nonplanar_surface-%d-initial_layer%.2f.svg", id, surface.stats.min.z).c_str(), true);
            }
#endif

            for (NonplanarSurfaces::iterator it = volume_surfaces.begin(); it!=volume_surfaces.end();) {
                if((*it).stats.max.z - (*it).stats.min.z < 0.03f) {
                    BOOST_LOG_TRIVIAL(trace) << "Find nonplanar surfaces - deleted one surface below 0.03 mm height variation";
                    note_curved_drop(m_curved_dropped, *it, "flat");
                    it = volume_surfaces.erase(it);
                } else {
                    it++;
                }
            }

            for (NonplanarSurfaces::iterator it = volume_surfaces.begin(); it!=volume_surfaces.end();) {
                if((*it).check_surface_area(float(m_config.nonplanar_curved_min_area.value))) {
                    BOOST_LOG_TRIVIAL(trace) << "Find nonplanar surfaces - deleted one surface due to print area too small";
                    ++rejections.area;
                    note_curved_drop(m_curved_dropped, *it, "area");
                    it = volume_surfaces.erase(it);
                }else {
                    it++;
                }
            }

            for (NonplanarSurfaces::iterator it = volume_surfaces.begin(); it!=volume_surfaces.end();) {
                if(check_nonplanar_collisions((*it))) {
                    BOOST_LOG_TRIVIAL(trace) << "Find nonplanar surfaces - deleted one surface due to collision";
                    ++rejections.collision;
                    note_curved_drop(m_curved_dropped, *it, "access");
                    it = volume_surfaces.erase(it);
                } else {
                    it++;
                }
            }

            BOOST_LOG_TRIVIAL(info) << "Find nonplanar surfaces - found " << volume_surfaces.size() << " in " << (*it)->name;
            note_curved_surface_rejections(*this, rejections);
            m_nonplanar_surfaces.insert(m_nonplanar_surfaces.end(), volume_surfaces.begin(), volume_surfaces.end());

            for (size_t id = 0; id < m_nonplanar_surfaces.size(); ++ id) {
                auto& surface = m_nonplanar_surfaces[id];
                BOOST_LOG_TRIVIAL(debug) << "Find nonplanar surfaces - surface" << id << " at Z [min=" << surface.stats.min.z << ", max=" << surface.stats.max.z << "]";

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                Surfaces surfaces;
                surfaces_append(surfaces, surface.horizontal_projection(), SurfaceType::stTopNonplanar);
                SurfaceCollection c(surfaces);
                c.export_to_svg(debug_out_path("0_find_nonplanar_surface-%d-final_layer%.2f.svg", id, surface.stats.min.z).c_str(), true);
#endif
            }
        }
    }

    m_curved_candidates = m_nonplanar_surfaces.size();
    for (size_t i = 0; i < m_nonplanar_surfaces.size(); ++i)
        m_nonplanar_surfaces[i].stable_id = int(i);
    this->drop_curved_planar_surfaces();
    if (curved_tops_mode(m_config.nonplanar_mode.value) && m_curved_candidates == 0 && !m_curved_fallback_off &&
        smoothest_tops_head_known(m_print->config())) {
        bool walls = false;
        for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id)
            walls |= this->printing_region(region_id).config().nonplanar_sublayer_walls.value;
        m_curved_fallback_zaa = !walls;
    }
    BOOST_LOG_TRIVIAL(info) << "Find nonplanar surfaces - end";
}

template<typename ThrowOnCancel>
static inline void apply_mm_segmentation(PrintObject &print_object, ThrowOnCancel throw_on_cancel)
{
    std::vector<std::vector<ExPolygons>> segmentation = multi_material_segmentation_by_painting(print_object, throw_on_cancel);
    assert(segmentation.size() == print_object.layer_count());
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, segmentation.size(), std::max(segmentation.size() / 128, size_t(1))),
        [&print_object, &segmentation, throw_on_cancel](const tbb::blocked_range<size_t> &range) {
            const auto  &layer_ranges   = print_object.shared_regions()->layer_ranges;
            double       z              = print_object.get_layer(int(range.begin()))->slice_z;
            auto         it_layer_range = layer_range_first(layer_ranges, z);
            const size_t num_extruders = print_object.print()->config().filament_diameter.size();

            struct ByExtruder {
                ExPolygons  expolygons;
                BoundingBox bbox;
            };

            struct ByRegion {
                ExPolygons expolygons;
                bool       needs_merge { false };
            };

            std::vector<ByExtruder> by_extruder;
            std::vector<ByRegion>   by_region;
            for (size_t layer_id = range.begin(); layer_id < range.end(); ++layer_id) {
                throw_on_cancel();
                Layer &layer = *print_object.get_layer(int(layer_id));
                it_layer_range = layer_range_next(layer_ranges, it_layer_range, layer.slice_z);
                const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;
                by_extruder.assign(num_extruders, ByExtruder());
                by_region.assign(layer.region_count(), ByRegion());
                bool layer_split = false;
                for (size_t extruder_id = 0; extruder_id < num_extruders; ++ extruder_id) {
                    ByExtruder &region = by_extruder[extruder_id];
                    append(region.expolygons, std::move(segmentation[layer_id][extruder_id]));
                    if (! region.expolygons.empty()) {
                        region.bbox = get_extents(region.expolygons);
                        layer_split = true;
                    }
                }

                if (!layer_split)
                    continue;

                auto it_painted_region_begin = layer_range.painted_regions.cbegin();
                for (int parent_layer_region_idx = 0; parent_layer_region_idx < layer.region_count(); ++parent_layer_region_idx) {
                    if (it_painted_region_begin == layer_range.painted_regions.cend())
                        continue;

                    const LayerRegion &parent_layer_region = *layer.get_region(parent_layer_region_idx);
                    const PrintRegion &parent_print_region = parent_layer_region.region();
                    assert(parent_print_region.print_object_region_id() == parent_layer_region_idx);
                    if (parent_layer_region.slices.empty())
                        continue;

                    auto it_first_painted_region = std::find_if(it_painted_region_begin, layer_range.painted_regions.cend(), [&layer_range, &parent_print_region](const auto &painted_region) {
                        return layer_range.volume_regions[painted_region.parent].region->print_object_region_id() == parent_print_region.print_object_region_id();
                    });

                    if (it_first_painted_region == layer_range.painted_regions.cend())
                        continue;

                    assert(&parent_print_region == layer_range.volume_regions[it_first_painted_region->parent].region);

                    it_painted_region_begin = it_first_painted_region;

                    const BoundingBox parent_layer_region_bbox = get_extents(parent_layer_region.slices.surfaces);
                    bool              self_trimmed             = false;
                    int               self_extruder_id         = -1;
                    for (int extruder_id = 1; extruder_id <= int(by_extruder.size()); ++extruder_id) {
                        const ByExtruder &segmented = by_extruder[extruder_id - 1];
                        if (!segmented.bbox.defined || !parent_layer_region_bbox.overlap(segmented.bbox))
                            continue;

                        auto it_target_region = std::find_if(it_painted_region_begin, layer_range.painted_regions.cend(), [extruder_id](const auto &painted_region) {
                            return int(painted_region.extruder_id) >= extruder_id;
                        });

                        assert(it_target_region != layer_range.painted_regions.end());
                        assert(layer_range.volume_regions[it_target_region->parent].region == &parent_print_region && int(it_target_region->extruder_id) == extruder_id);

                        it_painted_region_begin = it_target_region;

                        if (it_target_region->region == &parent_print_region) {
                            self_extruder_id = extruder_id;
                            continue;
                        }

                        int        target_region_id = it_target_region->region->print_object_region_id();
                        ExPolygons stolen           = intersection_ex(parent_layer_region.slices.surfaces, segmented.expolygons);
                        if (!stolen.empty()) {
                            ByRegion &dst = by_region[target_region_id];
                            if (dst.expolygons.empty()) {
                                dst.expolygons = std::move(stolen);
                            } else {
                                append(dst.expolygons, std::move(stolen));
                                dst.needs_merge = true;
                            }
                        }
                    }

                    if (!self_trimmed) {
                        Polygons mine = to_polygons(parent_layer_region.slices.surfaces);
                        for (auto &segmented : by_extruder) {
                            if (&segmented - by_extruder.data() + 1 != self_extruder_id && segmented.bbox.defined && parent_layer_region_bbox.overlap(segmented.bbox)) {
                                mine = diff(mine, segmented.expolygons);
                                if (mine.empty())
                                    break;
                            }
                        }

                        if (!mine.empty()) {
                            mine = opening(union_ex(mine), scaled<float>(5. * EPSILON), scaled<float>(5. * EPSILON));
                        }

                        if (!mine.empty()) {
                            ByRegion &dst = by_region[parent_print_region.print_object_region_id()];
                            if (dst.expolygons.empty()) {
                                dst.expolygons = union_ex(mine);
                            } else {
                                append(dst.expolygons, union_ex(mine));
                                dst.needs_merge = true;
                            }
                        }
                    }
                }

                for (int region_id = 0; region_id < layer.region_count(); ++region_id) {
                    ByRegion &src = by_region[region_id];
                    if (src.needs_merge) {
                        src.expolygons = closing_ex(src.expolygons, scaled<float>(10. * EPSILON));
                    }

                    layer.get_region(region_id)->slices.set(std::move(src.expolygons), stInternal);
                }
            }
        });
}

template<typename ThrowOnCancel>
void apply_fuzzy_skin_segmentation(PrintObject &print_object, ThrowOnCancel throw_on_cancel)
{
    std::vector<std::vector<ExPolygons>> segmentation = fuzzy_skin_segmentation_by_painting(print_object, throw_on_cancel);
    assert(segmentation.size() == print_object.layer_count());

    struct ByRegion
    {
        ExPolygons expolygons;
        bool       needs_merge { false };
    };

    tbb::parallel_for(tbb::blocked_range<size_t>(0, segmentation.size(), std::max(segmentation.size() / 128, size_t(1))), [&print_object, &segmentation, throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        const auto &layer_ranges   = print_object.shared_regions()->layer_ranges;
        auto        it_layer_range = layer_range_first(layer_ranges, print_object.get_layer(int(range.begin()))->slice_z);

        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            throw_on_cancel();

            Layer &layer = *print_object.get_layer(int(layer_idx));
            it_layer_range = layer_range_next(layer_ranges, it_layer_range, layer.slice_z);
            const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;

            assert(segmentation[layer_idx].size() == 1);
            const ExPolygons &fuzzy_skin_segmentation      = segmentation[layer_idx][0];
            const BoundingBox fuzzy_skin_segmentation_bbox = get_extents(fuzzy_skin_segmentation);
            if (fuzzy_skin_segmentation.empty())
                continue;

            std::vector<ByRegion> by_region(layer.region_count());
            auto                  it_fuzzy_skin_region_begin = layer_range.fuzzy_skin_painted_regions.cbegin();
            for (int parent_layer_region_idx = 0; parent_layer_region_idx < layer.region_count(); ++parent_layer_region_idx) {
                if (it_fuzzy_skin_region_begin == layer_range.fuzzy_skin_painted_regions.cend())
                    continue;

                const LayerRegion &parent_layer_region = *layer.get_region(parent_layer_region_idx);
                const PrintRegion &parent_print_region = parent_layer_region.region();
                assert(parent_print_region.print_object_region_id() == parent_layer_region_idx);
                if (parent_layer_region.slices.empty())
                    continue;

                auto it_fuzzy_skin_region = std::find_if(it_fuzzy_skin_region_begin, layer_range.fuzzy_skin_painted_regions.cend(), [&layer_range, &parent_print_region](const auto &fuzzy_skin_region) {
                    return fuzzy_skin_region.parent_print_object_region_id(layer_range) == parent_print_region.print_object_region_id();
                });

                if (it_fuzzy_skin_region == layer_range.fuzzy_skin_painted_regions.cend())
                    continue;

                assert(it_fuzzy_skin_region->parent_print_object_region(layer_range) == &parent_print_region);

                it_fuzzy_skin_region_begin = std::next(it_fuzzy_skin_region);

                const BoundingBox parent_layer_region_bbox        = get_extents(parent_layer_region.slices.surfaces);
                Polygons          layer_region_remaining_polygons = to_polygons(parent_layer_region.slices.surfaces);
                if (parent_layer_region_bbox.overlap(fuzzy_skin_segmentation_bbox) && it_fuzzy_skin_region->region != &parent_print_region) {
                    const int  target_region_id = it_fuzzy_skin_region->region->print_object_region_id();
                    ExPolygons stolen           = intersection_ex(parent_layer_region.slices.surfaces, fuzzy_skin_segmentation);
                    if (!stolen.empty()) {
                        ByRegion &dst = by_region[target_region_id];
                        if (dst.expolygons.empty()) {
                            dst.expolygons = std::move(stolen);
                        } else {
                            append(dst.expolygons, std::move(stolen));
                            dst.needs_merge = true;
                        }
                    }

                    layer_region_remaining_polygons = diff(layer_region_remaining_polygons, fuzzy_skin_segmentation);

                    if (!layer_region_remaining_polygons.empty()) {
                        layer_region_remaining_polygons = opening(union_ex(layer_region_remaining_polygons), scaled<float>(5. * EPSILON), scaled<float>(5. * EPSILON));
                    }
                }

                if (!layer_region_remaining_polygons.empty()) {
                    ByRegion &dst = by_region[parent_print_region.print_object_region_id()];
                    if (dst.expolygons.empty()) {
                        dst.expolygons = union_ex(layer_region_remaining_polygons);
                    } else {
                        append(dst.expolygons, union_ex(layer_region_remaining_polygons));
                        dst.needs_merge = true;
                    }
                }
            }

            for (int region_id = 0; region_id < layer.region_count(); ++region_id) {
                ByRegion &src = by_region[region_id];
                if (src.needs_merge) {
                    src.expolygons = closing_ex(src.expolygons, scaled<float>(10. * EPSILON));
                }

                layer.get_region(region_id)->slices.set(std::move(src.expolygons), stInternal);
            }
        }
    });
}


void PrintObject::slice_volumes()
{
    BOOST_LOG_TRIVIAL(info) << "Slicing volumes..." << log_memory_info();
    const Print *print                      = this->print();
    const auto   throw_on_cancel_callback   = std::function<void()>([print](){ print->throw_if_canceled(); });

    for (Layer* layer : m_layers) {
        while (!layer->m_regions.empty()) {
            if (layer->m_regions.back())
                delete layer->m_regions.back();
            layer->m_regions.pop_back();
        }
        layer->m_regions.reserve(m_shared_regions->all_regions.size());
        for (const std::unique_ptr<PrintRegion> &pr : m_shared_regions->all_regions)
            layer->m_regions.emplace_back(new LayerRegion(layer, pr.get()));
    }

    std::vector<float>                   slice_zs      = zs_from_layers(m_layers);
    std::vector<VolumeSlices> objSliceByVolume;
    if (!slice_zs.empty()) {
        objSliceByVolume = slice_volumes_inner(
            print->config(), this->config(), this->trafo_centered(),
            this->model_object()->volumes, m_shared_regions->layer_ranges, slice_zs, throw_on_cancel_callback,
            &m_belt_min_z);
    }

    firstLayerObjSliceByVolume = objSliceByVolume;

    std::vector<std::vector<ExPolygons>> region_slices =
        slices_to_regions(print->config(), *this, this->model_object()->volumes, *m_shared_regions, slice_zs,
                          std::move(objSliceByVolume), PrintObject::clip_multipart_objects, throw_on_cancel_callback);

    if (!slice_zs.empty()) {
        std::vector<float>  low_zs = slice_zs;
        std::vector<double> radius(m_layers.size(), 0.);
        bool                any    = false;
        for (size_t i = 1; i < m_layers.size(); ++i)
            if ((radius[i] = Nonplanar::low_plane_top_radius(*this, m_layers[i]->height)) > 0.) {
                low_zs[i] = float(m_layers[i]->slice_z - 0.5 * m_layers[i]->height + PrintRegionConfig::defaults().zaa_min_z.value);
                any       = true;
            }
        if (any) {
            std::vector<std::vector<ExPolygons>> low = slices_to_regions(print->config(), *this, this->model_object()->volumes,
                *m_shared_regions, low_zs, slice_volumes_inner(print->config(), this->config(), this->trafo_centered(),
                    this->model_object()->volumes, m_shared_regions->layer_ranges, low_zs, throw_on_cancel_callback),
                PrintObject::clip_multipart_objects, throw_on_cancel_callback);
            Nonplanar::HeightField planar_surface;
            Nonplanar::Mask planar_nodes;
            if (config().nonplanar_mode.value == NonplanarMode::Auto && !print->config().spiral_mode.value) {
                const Nonplanar::ObjectMesh mesh(*this);
                const auto bbox = mesh.mesh.bounding_box();
                const double pitch = std::max(0.05, config().nonplanar_surface_resolution.value);
                const BoundingBoxf bounds((bbox.min.head<2>().array() / pitch).floor().matrix() * pitch - Vec2d::Constant(pitch),
                                          (bbox.max.head<2>().array() / pitch).ceil().matrix() * pitch + Vec2d::Constant(pitch));
                const Nonplanar::HeightField grid(bounds, pitch);
                planar_surface = Nonplanar::sample_top_surface(mesh.imesh, bbox.max.z() + 1.,
                    slicing_parameters().object_print_z_min, grid, throw_on_cancel_callback);
                Nonplanar::Mask candidate(planar_surface.data().size(), 0);
                for (size_t j = 0; j < candidate.size(); ++j)
                    candidate[j] = std::isfinite(planar_surface.data()[j]);
                const double h = config().layer_height.value;
                double width = 0.;
                for (size_t rid = 0; rid < num_printing_regions(); ++rid)
                    width = std::max(width, double(printing_region(rid).flow(*this, frTopSolidInfill, h).width()));
                const double r = Nonplanar::low_plane_top_radius(*this, h);
                if (r > 0.)
                    planar_nodes = Nonplanar::auto_planar_nodes(planar_surface, candidate, h, width,
                        0.5 * (0.5 * h - PrintRegionConfig::defaults().zaa_min_z.value) / r);
            }
            Polygons outline;
            for (const std::vector<ExPolygons> &by_layer : low)
                for (const ExPolygons &slice : by_layer)
                    polygons_append(outline, to_polygons(slice));
            ExPolygons outline_ex;
            if (count_points(outline) < Nonplanar::TreeUnionPoints)
                outline_ex = union_ex(outline);
            else {
                std::vector<ExPolygons> per_layer(m_layers.size());
                for (const std::vector<ExPolygons> &by_layer : low)
                    for (size_t i = 0; i < by_layer.size() && i < per_layer.size(); ++i)
                        expolygons_append(per_layer[i], by_layer[i]);
                outline_ex = tbb::parallel_reduce(
                    tbb::blocked_range<size_t>(0, per_layer.size(), 8), ExPolygons(),
                    [&per_layer](const tbb::blocked_range<size_t> &range, ExPolygons acc) {
                        for (size_t i = range.begin(); i < range.end(); ++i)
                            acc = union_ex(acc, per_layer[i]);
                        return acc;
                    },
                    [](const ExPolygons &a, const ExPolygons &b) { return union_ex(a, b); });
            }
            ExPolygons silhouette;
            for (ExPolygon &island : outline_ex)
                silhouette.emplace_back(std::move(island.contour));
            const ExPolygons rim        = diff_ex(silhouette, offset_ex(silhouette, -float(scale_(Nonplanar::low_plane_rim(*this)))));
            std::vector<Polygons> mid_all;
            if (region_slices.size() > 1) {
                mid_all.assign(m_layers.size(), Polygons());
                for (size_t i = 1; i < m_layers.size(); ++i)
                    if (radius[i] > 0.)
                        for (const std::vector<ExPolygons> &by_layer : region_slices)
                            polygons_append(mid_all[i], to_polygons(by_layer[i]));
            }
            for (size_t region_id = 0; region_id < region_slices.size(); ++region_id)
                tbb::parallel_for(size_t(1), region_slices[region_id].size(), [&](size_t i) {
                    if (radius[i] <= 0.)
                        return;
                    const ExPolygons &below = low[region_id][i];
                    ExPolygons       &mid   = region_slices[region_id][i];
                    ExPolygons        wide  = intersection_ex(opening_ex(mid_all.empty() ? diff_ex(below, mid) : diff_ex(below, mid_all[i]),
                                                                         float(scale_(radius[i]))), below);
                    if (wide.empty())
                        return;
                    const BoundingBox reach = get_extents(wide);
                    ExPolygons        keep  = intersection_ex(wide, rim);
                    if (!planar_nodes.empty()) {
                        Nonplanar::Mask active(planar_nodes.size(), 0);
                        const double margin = planar_surface.pitch();
                        const double lo = m_layers[i]->bottom_z() + PrintRegionConfig::defaults().zaa_min_z.value - margin;
                        const double hi = m_layers[i]->print_z - 0.5 * m_layers[i]->height + margin;
                        for (size_t j = 0; j < active.size(); ++j)
                            active[j] = planar_nodes[j] && planar_surface.data()[j] >= lo && planar_surface.data()[j] <= hi;
                        append(keep, intersection_ex(wide, Nonplanar::mask_to_expolygons(active, planar_surface)));
                    }
                    for (const ExPolygon &island : below)
                        if (get_extents(island).overlap(reach) && !intersection_ex(ExPolygons{ island }, wide).empty() &&
                            (offset_ex(island, -float(scale_(1.))).empty() ||
                             area(intersection_ex(ExPolygons{ island }, mid)) < 0.5 * island.area()))
                            append(keep, intersection_ex(wide, ExPolygons{ island }));
                    wide = union_ex(keep);
                    if (!wide.empty())
                        mid = union_ex(mid, offset_ex(wide, float(scale_(0.01))));
                });
        }
    }
    if (const std::vector<ExPolygons> *native = Nonplanar::NativeSliceOverride::find(this->model_object()->name);
        native != nullptr && region_slices.size() == 1)
        for (size_t layer_id = 0; layer_id < region_slices.front().size(); ++layer_id)
            region_slices.front()[layer_id] = layer_id < native->size() ? (*native)[layer_id] : ExPolygons();
    for (size_t region_id = 0; region_id < region_slices.size(); ++ region_id) {
        std::vector<ExPolygons> &by_layer = region_slices[region_id];
        for (size_t layer_id = 0; layer_id < by_layer.size(); ++ layer_id)
            m_layers[layer_id]->regions()[region_id]->slices.append(std::move(by_layer[layer_id]), stInternal);
    }
    region_slices.clear();
    m_multiplanar = Nonplanar::clip_multiplanar_slices(*this);
    m_planar_floating_mm = m_field_floating_mm = -1.;
    if (m_config.nonplanar_mode.value == NonplanarMode::Field && m_config.nonplanar_atom_toolpath.value.empty() && !m_layers.empty()) {
        m_auto_support_need = this->is_support_necessary();
        std::vector<ExPolygons> slices(m_layers.size());
        std::vector<double>     heights(m_layers.size());
        for (size_t i = 0; i < m_layers.size(); ++i) {
            for (const LayerRegion *region : m_layers[i]->regions())
                append(slices[i], to_expolygons(region->slices.surfaces));
            slices[i]  = union_ex(slices[i]);
            heights[i] = m_layers[i]->height;
        }
        m_planar_floating_mm = Nonplanar::wall_floating_mm(slices, heights, this->printing_region(0).config().wall_loops.value,
                                                           m_config.line_width.get_abs_value(m_print->config().nozzle_diameter.get_at(0)));
    }
    m_native_summary.clear();
    if (!m_multiplanar && m_config.nonplanar_mode.value == NonplanarMode::ConformalLayers)
        Nonplanar::conformal_ladder_slice(*this->model_object());
    m_slant.reset();
    if (m_config.nonplanar_mode.value == NonplanarMode::Slant && !m_multiplanar && m_config.nonplanar_atom_toolpath.value.empty()) {
        Nonplanar::slant_ladder_slice(*this->model_object());
        Nonplanar::SlantChoice choice;
        m_native_field = Nonplanar::slant_field(*this, choice);
        m_slant        = choice;
        if (!choice.planar_limit.empty())
            this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "slant_flat", warning_object(this->model_object()->name) },
                                          (boost::format(_u8L("Slant prints %1% in flat layers: %2%.")) % m_model_object->name % choice.reason).str());
    } else
        m_native_field = !m_multiplanar && (Nonplanar::native_requested() || m_config.nonplanar_mode.value == NonplanarMode::ConformalLayers) ?
                             Nonplanar::native_field(*this) : nullptr;
    m_field        = m_multiplanar || m_native_field ? nullptr : Nonplanar::clip_field_slices(*this);
    if (m_multiplanar && !m_multiplanar->auto_reason.empty())
        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "multiplanar_auto", warning_object(this->model_object()->name) }, m_multiplanar->auto_reason);

    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - removing top empty layers";
    const bool grounded = m_multiplanar != nullptr && !Nonplanar::grounded_route_slices(*this).empty();
    while (! m_layers.empty() && ! grounded) {
        const Layer *layer = m_layers.back();
        if (! layer->empty())
            break;
        delete layer;
        m_layers.pop_back();
    }
    if (! m_layers.empty())
        m_layers.back()->upper_layer = nullptr;
    m_print->throw_if_canceled();

    this->apply_conical_overhang();

    if (const auto& volumes = this->model_object()->volumes;
        m_print->config().filament_diameter.size() > 1 &&
        std::find_if(volumes.begin(), volumes.end(), [](const ModelVolume* v) { return !v->mmu_segmentation_facets.empty(); }) != volumes.end()) {

        if (m_config.xy_hole_compensation.value != 0.f || m_config.xy_contour_compensation.value != 0.f) {
            this->active_step_add_warning(
                PrintStateBase::WarningLevel::CRITICAL, WarningCode{ "xy_compensation_ignored_color", warning_object(this->model_object()->name) },
                L("An object's XY size compensation will not be used because it is also color-painted.\nXY Size "
                  "compensation cannot be combined with color-painting."));
            BOOST_LOG_TRIVIAL(info) << "xy compensation will not work for object " << this->model_object()->name << " for multi filament.";
        }

        BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - MMU segmentation";
        apply_mm_segmentation(*this, [print]() { print->throw_if_canceled(); });
    }

    if (this->model_object()->is_fuzzy_skin_painted()) {
        if (m_config.xy_hole_compensation.value != 0.f || m_config.xy_contour_compensation.value != 0.f) {
            this->active_step_add_warning(
                PrintStateBase::WarningLevel::CRITICAL, WarningCode{ "xy_compensation_ignored_fuzzy", warning_object(this->model_object()->name) },
                _u8L("An object has enabled XY Size compensation which will not be used because it is also fuzzy skin painted.\nXY Size "
                     "compensation cannot be combined with fuzzy skin painting.") +
                    "\n" + (_u8L("Object name")) + ": " + this->model_object()->name);
        }

        BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - Fuzzy skin segmentation";
        apply_fuzzy_skin_segmentation(*this, [print]() { print->throw_if_canceled(); });
    }

    InterlockingGenerator::generate_interlocking_structure(this, [print]() { print->throw_if_canceled(); });
    m_print->throw_if_canceled();

    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - make_slices in parallel - begin";
    {
        const size_t num_extruders = print->config().filament_diameter.size();
        const auto   xy_hole_scaled = (num_extruders > 1 && this->is_mm_painted()) ? scaled<float>(0.f) : scaled<float>(m_config.xy_hole_compensation.value);
        const auto   xy_contour_scaled            = (num_extruders > 1 && this->is_mm_painted()) ? scaled<float>(0.f) : scaled<float>(m_config.xy_contour_compensation.value);
        const float  elephant_foot_compensation_scaled = (m_config.raft_layers == 0) ?
            float(scale_(m_config.elefant_foot_compensation.value)) :
        	0.f;
        std::vector<ExPolygons> lslices_elfoot_uncompensated;
        lslices_elfoot_uncompensated.resize(elephant_foot_compensation_scaled > 0 ? std::min(m_config.elefant_foot_compensation_layers.value, (int)m_layers.size()) : 0);
	    tbb::parallel_for(
	        tbb::blocked_range<size_t>(0, m_layers.size()),
			[this, xy_hole_scaled, xy_contour_scaled, elephant_foot_compensation_scaled, &lslices_elfoot_uncompensated](const tbb::blocked_range<size_t>& range) {
	            for (size_t layer_id = range.begin(); layer_id < range.end(); ++ layer_id) {
	                m_print->throw_if_canceled();
	                Layer *layer = m_layers[layer_id];
	                float elfoot = elephant_foot_compensation_scaled > 0 && layer_id < m_config.elefant_foot_compensation_layers.value ? 
                        elephant_foot_compensation_scaled - (elephant_foot_compensation_scaled / m_config.elefant_foot_compensation_layers.value) * layer_id : 
                        0.f;
	                if (layer->m_regions.size() == 1) {
	                    LayerRegion *layerm = layer->m_regions.front();
                        if (elfoot > 0) {
                            ExPolygons expolygons_to_compensate = to_expolygons(std::move(layerm->slices.surfaces));
                            if (xy_contour_scaled > 0 || xy_hole_scaled > 0) {
                                expolygons_to_compensate = _shrink_contour_holes(std::max(0.f, xy_contour_scaled),
                                                                   std::max(0.f, xy_hole_scaled),
                                                                   expolygons_to_compensate);
                            }
                            if (xy_contour_scaled < 0 || xy_hole_scaled < 0) {
                                expolygons_to_compensate = _shrink_contour_holes(std::min(0.f, xy_contour_scaled),
                                                                   std::min(0.f, xy_hole_scaled),
                                                                   expolygons_to_compensate);
                            }
                            lslices_elfoot_uncompensated[layer_id] = expolygons_to_compensate;
							layerm->slices.set(
								union_ex(
									Slic3r::elephant_foot_compensation(expolygons_to_compensate,
	                            		layerm->flow(frExternalPerimeter), unscale<double>(elfoot))),
								stInternal);
	                    } else {
                            if (xy_contour_scaled != 0.0f || xy_hole_scaled != 0.0f) {
                                ExPolygons expolygons = to_expolygons(std::move(layerm->slices.surfaces));
                                if (xy_contour_scaled > 0 || xy_hole_scaled > 0) {
                                    expolygons = _shrink_contour_holes(std::max(0.f, xy_contour_scaled),
                                                                       std::max(0.f, xy_hole_scaled),
                                                                       expolygons);
                                }
                                if (xy_contour_scaled < 0 || xy_hole_scaled < 0) {
                                    expolygons = _shrink_contour_holes(std::min(0.f, xy_contour_scaled),
                                                                       std::min(0.f, xy_hole_scaled),
                                                                       expolygons);
                                }
                                layerm->slices.set(std::move(expolygons), stInternal);
                            }
	                    }
	                } else {
                        float max_growth = std::max(xy_hole_scaled, xy_contour_scaled);
                        float min_growth = std::min(xy_hole_scaled, xy_contour_scaled);
                        ExPolygons merged_poly_for_holes_growing;
                        if (max_growth > 0) {
                            merged_poly_for_holes_growing = layer->merged(float(SCALED_EPSILON));
                            merged_poly_for_holes_growing = _shrink_contour_holes(std::max(0.f, xy_contour_scaled),
                                                                                  std::max(0.f, xy_hole_scaled),
                                                                                  union_ex(merged_poly_for_holes_growing));

                            Polygons processed;
                            for (size_t region_id = 0; region_id < layer->regions().size(); ++region_id) {
                                ExPolygons slices = to_expolygons(std::move(layer->m_regions[region_id]->slices.surfaces));
                                if (max_growth > 0.f) {
                                    slices = intersection_ex(offset_ex(slices, max_growth), merged_poly_for_holes_growing);
                                }

                                if (region_id > 0)
                                    slices = diff_ex(to_polygons(std::move(slices)), processed);
                                if (region_id + 1 < layer->regions().size())
                                    polygons_append(processed, slices);
                                layer->m_regions[region_id]->slices.set(std::move(slices), stInternal);
                            }
                        }
                        if (min_growth < 0.f || elfoot > 0.f) {
                            ExPolygons trimming;
                            static const float eps = float(scale_(m_config.slice_closing_radius.value) * 1.5);
                            if (elfoot > 0.f) {
                                ExPolygons expolygons_to_compensate = offset_ex(layer->merged(eps), -eps);
                                lslices_elfoot_uncompensated[layer_id] = expolygons_to_compensate;
                                trimming = Slic3r::elephant_foot_compensation(expolygons_to_compensate,
                                    layer->m_regions.front()->flow(frExternalPerimeter), unscale<double>(elfoot));
                            } else {
                                trimming = layer->merged(float(SCALED_EPSILON));
                            }
                            if (min_growth < 0.0f)
                                trimming = _shrink_contour_holes(std::min(0.f, xy_contour_scaled),
                                                                 std::min(0.f, xy_hole_scaled),
                                                                 trimming);
                            for (size_t region_id = 0; region_id < layer->regions().size(); ++region_id) {
                                ExPolygons contour_exp = to_expolygons(std::move(layer->regions()[region_id]->slices.surfaces));

                                layer->regions()[region_id]->slices.set(intersection_ex(contour_exp, to_polygons(trimming)), stInternal);
                            }
                        }
	                }
	                layer->make_slices();
	            }
	        });
	    if (elephant_foot_compensation_scaled > 0.f && ! m_layers.empty()) {
	    	assert(m_layers.front()->id() == 0);
            for (int i = 0; i < lslices_elfoot_uncompensated.size(); i++) {
                ExPolygons &expolygons_uncompensated = lslices_elfoot_uncompensated[i];
                Points ordering_points;
                ordering_points.reserve(expolygons_uncompensated.size());
                for (const ExPolygon &ex : expolygons_uncompensated)
                    ordering_points.push_back(ex.contour.first_point());
                std::vector<Points::size_type> order = chain_points(ordering_points);
                ExPolygons lslices_sorted;
                lslices_sorted.reserve(expolygons_uncompensated.size());
                for (size_t i : order)
                    lslices_sorted.emplace_back(std::move(expolygons_uncompensated[i]));
                m_layers[i]->lslices = std::move(lslices_sorted);
            }
		}
	}

    m_print->throw_if_canceled();
    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - make_slices in parallel - end";
}

void PrintObject::apply_conical_overhang() {
    BOOST_LOG_TRIVIAL(info) << "Make overhang printable...";

    if (m_layers.empty()) {
        return;
    }
    
    const double conical_overhang_angle = this->config().make_overhang_printable_angle;
    if (conical_overhang_angle == 90.0) {
        return;
    }
    const double angle_radians = conical_overhang_angle * M_PI / 180.;
    const double max_hole_area = this->config().make_overhang_printable_hole_size;
    const double tan_angle = tan(angle_radians);
    BOOST_LOG_TRIVIAL(info) << "angle " << angle_radians << " maxHoleArea " << max_hole_area << " tan_angle "
                            << tan_angle;
    const coordf_t layer_thickness = m_config.layer_height.value;
    const coordf_t max_dist_from_lower_layer = tan_angle * layer_thickness;
    BOOST_LOG_TRIVIAL(info) << "layer_thickness " << layer_thickness << " max_dist_from_lower_layer "
                            << max_dist_from_lower_layer;

    const coordf_t scaled_max_dist_from_lower_layer = -float(scale_(max_dist_from_lower_layer));
    const coordf_t scaled_max_hole_area = float(scale_(scale_(max_hole_area)));


    for (auto i = m_layers.rbegin() + 1; i != m_layers.rend(); ++i) {
        m_print->throw_if_canceled();
        Layer *layer = *i;
        Layer *upper_layer = layer->upper_layer;

        if (upper_layer->empty()) {
          continue;
        }

        if (std::all_of(layer->m_regions.begin(), layer->m_regions.end(),
                        [](const LayerRegion *r) { return  r->slices.empty() || !r->region().config().make_overhang_printable; })) {
            continue;
        }

        auto upper_poly = upper_layer->merged(float(SCALED_EPSILON));
        upper_poly = union_ex(upper_poly);

        auto current_poly = layer->merged(float(SCALED_EPSILON));
        current_poly = union_ex(current_poly);

        if (scaled_max_hole_area > 0.0) {

            for (auto layer_polygon : current_poly) {
                for (auto hole : layer_polygon.holes) {
                    if (std::abs(hole.area()) < scaled_max_hole_area) {
                        ExPolygon hole_poly(hole);
                        auto hole_with_above = intersection_ex(upper_poly, hole_poly);
                        if (!hole_with_above.empty()) {
                            auto hole_difference = xor_ex(hole_with_above, hole_poly);
                            if (hole_difference.empty()) {
                                upper_poly = diff_ex(upper_poly, hole_poly);
                            }
                        }
                    }
                }
            }
        }

        upper_poly = offset_ex(upper_poly, scaled_max_dist_from_lower_layer);

        for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id) {
            if (!upper_layer->m_regions[region_id]->region().config().make_overhang_printable) {
                continue;
            }

            auto p = union_ex(intersection_ex(upper_layer->m_regions[region_id]->slices.surfaces, upper_poly));

            p.erase(std::remove_if(p.begin(), p.end(), [&current_poly](const ExPolygon& ex) {
                return diff_ex(ex, current_poly).empty();
            }), p.end());

            ExPolygons layer_polygons = to_expolygons(layer->m_regions[region_id]->slices.surfaces);
            layer->m_regions[region_id]->slices.set(union_ex(layer_polygons, p), stInternal);

            for (size_t other_region = 0; other_region < this->num_printing_regions(); ++other_region) {
                if (other_region == region_id) {
                    continue;
                }
                ExPolygons s = to_expolygons(layer->m_regions[other_region]->slices.surfaces);
                layer->m_regions[other_region]->slices.set(diff_ex(s, p, ApplySafetyOffset::Yes), stInternal);
            }
        }
    }
}

ExPolygons PrintObject::_shrink_contour_holes(double contour_delta, double hole_delta, const ExPolygons& polys) const
{
    ExPolygons new_ex_polys;
    for (const ExPolygon& ex_poly : polys) {
        Polygons contours;
        Polygons holes;
        for (const Polygon& hole : ex_poly.holes) {
            if (hole_delta != 0) {
                for (Polygon& newHole : offset(hole, -hole_delta)) {
                    newHole.make_counter_clockwise();
                    holes.emplace_back(std::move(newHole));
                }
            } else {
                holes.push_back(hole);
                holes.back().make_counter_clockwise();
            }
        }
        if (contour_delta != 0) {
            Polygons new_contours = offset(ex_poly.contour, contour_delta);
            if (new_contours.size() == 0)
                continue;
            contours.insert(contours.end(), std::make_move_iterator(new_contours.begin()), std::make_move_iterator(new_contours.end()));
        } else {
            contours.push_back(ex_poly.contour);
        }
        ExPolygons temp = diff_ex(union_(contours), union_(holes));
        new_ex_polys.insert(new_ex_polys.end(), std::make_move_iterator(temp.begin()), std::make_move_iterator(temp.end()));
    }
    return union_ex(new_ex_polys);
}

Transform3d PrintObject::trafo_sliced() const
{
    Transform3d trafo = this->trafo_centered();
    BeltSliceStrategy::apply_preslice_transforms(trafo, this->print()->config(), this->model_object()->volumes);
    return trafo;
}

std::vector<Polygons> PrintObject::slice_support_volumes(const ModelVolumeType model_volume_type) const
{
    auto it_volume     = this->model_object()->volumes.begin();
    auto it_volume_end = this->model_object()->volumes.end();
    for (; it_volume != it_volume_end && (*it_volume)->type() != model_volume_type; ++ it_volume) ;
    std::vector<Polygons> slices;
    if (it_volume != it_volume_end) {
        std::vector<float> zs = zs_from_layers(this->layers());
        std::vector<char>  merge_layers;
        bool               merge = false;
        const Print       *print = this->print();
        auto               throw_on_cancel_callback = std::function<void()>([print](){ print->throw_if_canceled(); });
        MeshSlicingParamsEx params;
        params.trafo = this->trafo_sliced();
        for (; it_volume != it_volume_end; ++ it_volume)
            if ((*it_volume)->type() == model_volume_type) {
                std::vector<ExPolygons> slices2 = slice_volume(*(*it_volume), zs, params, throw_on_cancel_callback);
                if (slices.empty()) {
                    slices.reserve(slices2.size());
                    for (ExPolygons &src : slices2)
                        slices.emplace_back(to_polygons(std::move(src)));
                } else if (!slices2.empty()) {
                    if (merge_layers.empty())
                        merge_layers.assign(zs.size(), false);
                    for (size_t i = 0; i < zs.size(); ++ i) {
                        if (slices[i].empty())
                            slices[i] = to_polygons(std::move(slices2[i]));
                        else if (! slices2[i].empty()) {
                            append(slices[i], to_polygons(std::move(slices2[i])));
                            merge_layers[i] = true;
                            merge = true;
                        }
                    }
                }
            }
        if (merge) {
            std::vector<Polygons*> to_merge;
            to_merge.reserve(zs.size());
            for (size_t i = 0; i < zs.size(); ++ i)
                if (merge_layers[i])
                    to_merge.emplace_back(&slices[i]);
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, to_merge.size()),
                [&to_merge](const tbb::blocked_range<size_t> &range) {
                    for (size_t i = range.begin(); i < range.end(); ++ i)
                        *to_merge[i] = union_(*to_merge[i]);
            });
        }
    }
    return slices;
}

std::vector<ExPolygons> PrintObject::slice_single_volume_regions(const ModelVolume* volume) const
{
    if (volume == nullptr)
        return {};
    const std::vector<float> zs = zs_from_layers(this->layers());
    MeshSlicingParamsEx params;
    params.trafo = this->trafo_sliced();
    const Print *print = this->print();
    return slice_volume(*volume, zs, params, [print]() { print->throw_if_canceled(); });
}

}
