#pragma once
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/uuid/detail/sha1.hpp>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace Slic3r;
using Json = nlohmann::json;

Json context_settings(const ConfigBase &config)
{
    Json settings = Json::object();
    for (const std::string &key : config.keys())
        settings[key] = config.opt_serialize(key);
    return settings;
}

std::string context_hash(const Json &settings)
{
    const std::string bytes = settings.dump();
    boost::uuids::detail::sha1 hash;
    hash.process_bytes(bytes.data(), bytes.size());
    boost::uuids::detail::sha1::digest_type digest;
    hash.get_digest(digest);
    std::ostringstream out;
    for (const auto word : digest)
        out << std::hex << std::setfill('0') << std::setw(int(2 * sizeof(word))) << static_cast<unsigned long>(word);
    return out.str();
}

Json context_transform(const Transform3d &transform)
{
    Json rows = Json::array();
    for (int r = 0; r < 4; ++r) {
        Json row = Json::array();
        for (int c = 0; c < 4; ++c) row.push_back(transform.matrix()(r, c));
        rows.push_back(std::move(row));
    }
    return rows;
}

using ContextInstances = std::map<ObjectID, Json>;
ContextInstances context_instances(const std::string &path, const Model &model, const std::vector<SavedInstance> &saved)
{
    std::map<int, std::vector<const ModelObject *>> candidates;
    for (const SavedInstance &s : saved)
        for (const ModelObject *object : model.objects)
            if (s.identify_id > 0 && s.instance_id >= 0 && size_t(s.instance_id) < object->instances.size() &&
                object->instances[size_t(s.instance_id)]->loaded_id == size_t(s.identify_id))
                candidates[s.object_id].push_back(object);
    std::map<int, const ModelObject *> objects;
    for (auto &[saved_id, matched] : candidates) {
        std::sort(matched.begin(), matched.end());
        if (std::unique(matched.begin(), matched.end()) == matched.begin() + 1) objects[saved_id] = matched.front();
    }
    if (objects.size() < model.objects.size()) {
        mz_zip_archive archive{};
        if (!open_zip_reader(&archive, path)) throw std::runtime_error("cannot read project membership: " + path);
        ScopeGuard close([&] { close_zip_reader(&archive); });
        boost::property_tree::ptree tree;
        size_t size = 0;
        if (void *data = mz_zip_reader_extract_file_to_heap(&archive, "3D/3dmodel.model", &size, 0)) {
            std::istringstream in(std::string(static_cast<const char *>(data), size));
            mz_free(data);
            boost::property_tree::read_xml(in, tree);
        }
        const auto build = tree.get_child_optional("model.build");
        std::vector<int> order;
        std::map<int, size_t> counts;
        if (build)
            for (const auto &item : *build)
                if (item.first == "item") {
                    const int id = item.second.get<int>("<xmlattr>.objectid");
                    if (!counts.count(id)) order.push_back(id);
                    ++counts[id];
                }
        bool matches = order.size() == model.objects.size();
        for (size_t i = 0; matches && i < order.size(); ++i)
            matches = counts[order[i]] == model.objects[i]->instances.size() &&
                      (!objects.count(order[i]) || objects.at(order[i]) == model.objects[i]);
        if (matches)
            for (size_t i = 0; i < order.size(); ++i) objects[order[i]] = model.objects[i];
    }
    ContextInstances result;
    const auto entry = [](int object_id, int instance_id, const ModelInstance *instance, Json plate_index, Json plate_name) {
        return Json{{"saved_object_id", object_id}, {"saved_instance_id", instance_id},
                    {"source_plate_index", std::move(plate_index)}, {"source_plate_name", std::move(plate_name)},
                    {"saved_transform", context_transform(instance->get_matrix())}};
    };
    for (const auto &[saved_id, object] : objects)
        for (size_t i = 0; i < object->instances.size(); ++i)
            result[object->instances[i]->id()] = entry(saved_id, int(i), object->instances[i], nullptr, nullptr);
    for (const SavedInstance &s : saved) {
        const auto object = objects.find(s.object_id);
        if (object == objects.end() || s.instance_id < 0 || size_t(s.instance_id) >= object->second->instances.size()) continue;
        const ModelInstance *instance = object->second->instances[size_t(s.instance_id)];
        result[instance->id()] = entry(s.object_id, s.instance_id, instance, s.plate, s.plate_name);
    }
    return result;
}

struct SourcePlate
{
    int                plate_index = 0;
    std::string        name;
    bool               locked = false;
    DynamicPrintConfig config;
    std::string        thumbnail_file, no_light_thumbnail_file, top_file, pick_file;
};

std::vector<std::unique_ptr<PlateData>> saved_plates(const Model &model, const std::vector<SourcePlate> &sources,
                                                    const ContextInstances &membership)
{
    std::vector<std::unique_ptr<PlateData>> plates;
    for (const SourcePlate &source : sources) {
        auto &plate        = plates.emplace_back(std::make_unique<PlateData>());
        plate->plate_index = source.plate_index;
        plate->plate_name  = source.name;
        plate->locked      = source.locked;
        plate->thumbnail_file          = source.thumbnail_file;
        plate->no_light_thumbnail_file = source.no_light_thumbnail_file;
        plate->top_file                = source.top_file;
        plate->pick_file               = source.pick_file;
        plate->config      = source.config;
    }
    bool resolved = !plates.empty();
    for (size_t i = 0; resolved && i < model.objects.size(); ++i)
        for (size_t j = 0; resolved && j < model.objects[i]->instances.size(); ++j) {
            const auto saved = membership.find(model.objects[i]->instances[j]->id());
            const auto plate = saved == membership.end() || !saved->second.at("source_plate_index").is_number_integer() ? plates.end() :
                std::find_if(plates.begin(), plates.end(), [index = saved->second.at("source_plate_index").get<int>()](const auto &plate) {
                    return plate->plate_index + 1 == index;
                });
            resolved = plate != plates.end();
            if (resolved)
                (*plate)->objects_and_instances.emplace_back(int(i), int(j));
        }
    if (resolved)
        return plates;
    plates.clear();
    auto &plate        = plates.emplace_back(std::make_unique<PlateData>());
    plate->plate_index = 0;
    for (size_t i = 0; i < model.objects.size(); ++i)
        for (size_t j = 0; j < model.objects[i]->instances.size(); ++j)
            plate->objects_and_instances.emplace_back(int(i), int(j));
    return plates;
}

Vec3d context_plate_origin(const DynamicPrintConfig &config, const PlateDataPtrs &plates, int plate_index)
{
    const auto *bed = config.option<ConfigOptionPoints>("printable_area");
    if (!bed || bed->values.empty())
        throw std::runtime_error("selected project has no printable area");
    BoundingBoxf bounds;
    for (const Vec2d &point : bed->values) bounds.merge(point);
    int count = 0;
    for (const PlateData *item : plates)
        if (item) count = std::max(count, item->plate_index + 1);
    const int columns = int(std::ceil(std::sqrt(double(count))));
    return Vec3d((plate_index - 1) % columns * bounds.size().x() * 1.2, -(plate_index - 1) / columns * bounds.size().y() * 1.2, 0.);
}

void select_context_plate(Model &model, DynamicPrintConfig &config, PlateDataPtrs &plates,
                          const ContextInstances &saved, int plate_index)
{
    auto selected = std::find_if(plates.begin(), plates.end(), [plate_index](const PlateData *plate) {
        return plate && plate->plate_index + 1 == plate_index;
    });
    if (selected == plates.end())
        throw std::runtime_error("--plate-index does not name a plate in this project");
    PlateData *plate = *selected;
    const Vec3d origin = context_plate_origin(config, plates, plate_index);
    size_t kept = 0;
    for (size_t o = model.objects.size(); o-- > 0;) {
        ModelObject *object = model.objects[o];
        for (size_t i = object->instances.size(); i-- > 0;) {
            ModelInstance *instance = object->instances[i];
            const auto it = saved.find(instance->id());
            if (it == saved.end())
                throw std::runtime_error("cannot resolve saved plate membership for " + object->name);
            if (it->second.at("source_plate_index") != plate_index)
                object->delete_instance(i);
            else {
                instance->set_offset(instance->get_offset() - origin);
                ++kept;
            }
        }
        if (object->instances.empty()) model.delete_object(o);
        else object->invalidate_bounding_box();
    }
    if (!kept) throw std::runtime_error("selected project plate has no objects");
    DynamicPrintConfig plate_config = plate->config;
    const auto *bed_type = plate_config.option<ConfigOptionEnum<BedType>>("curr_bed_type");
    if (bed_type && bed_type->value == btDefault) plate_config.erase("curr_bed_type");
    config.apply(plate_config);
    for (const std::string key : {"wipe_tower_x", "wipe_tower_y"})
        if (auto *positions = config.option<ConfigOptionFloats>(key); positions && !positions->values.empty())
            positions->values = {positions->get_at(size_t(plate_index - 1))};
    const auto events = model.plates_custom_gcodes.find(plate_index - 1);
    CustomGCode::Info selected_events;
    if (events != model.plates_custom_gcodes.end()) selected_events = events->second;
    model.plates_custom_gcodes.clear();
    model.plates_custom_gcodes[0] = std::move(selected_events);
    model.curr_plate_index = 0;
    *selected = nullptr;
    release_PlateData_list(plates);
    plates = {plate};
}

Json context_presets(const DynamicPrintConfig &config, bool project)
{
    const auto preset = [&](const std::string &name, const std::vector<std::string> &keys, int slot = -1) {
        DynamicPrintConfig settings;
        settings.apply_only(config, keys, true);
        Json values = context_settings(settings);
        if (slot >= 0)
            for (const std::string &key : settings.keys())
                if (const auto *option = dynamic_cast<const ConfigOptionVectorBase *>(settings.option(key))) {
                    const auto items = option->vserialize();
                    if (!items.empty()) values[key] = items[size_t(slot) < items.size() ? size_t(slot) : 0];
                }
        return Json{{"name", name.empty() ? Json(nullptr) : Json(name)}, {"source", project ? "project-embedded" : "catalog"},
                    {"settings_sha1", context_hash(values)}};
    };
    const auto id = [&config](const char *key) {
        const auto *option = config.option<ConfigOptionString>(key);
        return option != nullptr ? option->value : std::string();
    };
    Json result = {{"printer", preset(id("printer_settings_id"), Preset::printer_options())},
                   {"process", preset(id("print_settings_id"), Preset::print_options())},
                   {"filaments", Json::array()}};
    const auto *names = config.option<ConfigOptionStrings>("filament_settings_id");
    const auto *diameters = config.option<ConfigOptionFloats>("filament_diameter");
    const size_t slots = diameters != nullptr ? diameters->values.size() : 0;
    for (size_t i = 0; i < slots; ++i) {
        Json filament = preset(names && i < names->values.size() ? names->values[i] : std::string(),
                               Preset::filament_options(), int(i));
        filament["slot"] = i + 1;
        result["filaments"].push_back(std::move(filament));
    }
    return result;
}

void slice_model_context(Json &record, const Print &print, const DynamicPrintConfig &config,
                         const ContextInstances &saved)
{
    Json &context = record["model_context"];
    context["effective_settings_sha1"] = context_hash(context_settings(config));
    context["objects"] = Json::array();
    size_t index = 0;
    for (const PrintObject *object : print.objects()) {
        const ModelObject &model = *object->model_object();
        Json entry = {{"record_index", index++}, {"name", model.name}, {"saved_object_id", nullptr}, {"settings", context_settings(model.config.get())},
                      {"effective_settings", context_settings(object->config())},
                      {"layer_height_profile", model.layer_height_profile.get()},
                      {"volumes", Json::array()}, {"height_ranges", Json::array()},
                      {"regions", Json::array()}, {"instances", Json::array()}};
        for (const ModelVolume *volume : model.volumes)
            entry["volumes"].push_back({{"name", volume->name}, {"type", ModelVolume::type_to_string(volume->type())},
                                         {"settings", context_settings(volume->config.get())},
                                         {"transform", context_transform(volume->get_matrix())}});
        for (const auto &[range, settings] : model.layer_config_ranges)
            entry["height_ranges"].push_back({{"z_mm", {range.first, range.second}},
                                              {"settings", context_settings(settings.get())}});
        for (size_t r = 0; r < object->num_printing_regions(); ++r)
            entry["regions"].push_back({{"id", object->printing_region(r).print_region_id()},
                                        {"settings", context_settings(object->printing_region(r).config())}});
        for (const PrintInstance &instance : object->instances()) {
            const auto it = saved.find(instance.model_instance->id());
            Json item = it == saved.end() ? Json{{"saved_object_id", nullptr}, {"saved_instance_id", nullptr}} : it->second;
            if (it != saved.end()) entry["saved_object_id"] = it->second.at("saved_object_id");
            item["record_index"] = instance.id;
            item["transform"] = context_transform(instance.model_instance->get_matrix());
            entry["instances"].push_back(std::move(item));
        }
        context["objects"].push_back(std::move(entry));
    }
}
}
