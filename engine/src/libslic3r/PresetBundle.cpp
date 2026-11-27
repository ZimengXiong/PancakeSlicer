#include <atomic>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/optional/optional.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <optional>
#include <exception>
#include <memory>
#include <initializer_list>
#include <map>
#include <iterator>
#include <functional>
#include <limits>
#include <sstream>

#include "PresetBundle.hpp"

#include "Config.hpp"
#include "Exception.hpp"
#include "AppConfig.hpp"
#include "ParallelResolve.hpp"
#include "Preset.hpp"
#include "PresetCacheFormat.hpp"
#include "PrintConfig.hpp"
#include "PublishSettings.hpp"
#include "FilamentMixer.hpp"
#include "Semver.hpp"
#include "libslic3r.h"
#include "I18N.hpp"
#include "Utils.hpp"
#include "InstanceLock.hpp"
#include "LocalesUtils.hpp"
#include "Model.hpp"
#include "TriangleSelector.hpp"
#include "libslic3r_version.h"

#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <set>
#include <fstream>
#include <string>
#include <tuple>
#include <stdexcept>
#include <unordered_map>
#include <system_error>
#include <unordered_set>
#include <boost/filesystem.hpp>
#include <boost/algorithm/clamp.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/locale.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <miniz/miniz.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_group.h>
#include <tbb/partitioner.h>
#include <vector>
#include <utility>

namespace fs = boost::filesystem;

#define L(s) Slic3r::I18N::translate(s)

using json = nlohmann::json;

namespace Slic3r {

static std::vector<std::string> s_project_options {
    "flush_volumes_vector",
    "flush_volumes_matrix",
    "filament_colour",
    "filament_colour_type",
    "filament_multi_colour",
    "wipe_tower_x",
    "wipe_tower_y",
    "curr_bed_type",
    "flush_multiplier",
    "flush_multiplier_fast",
    "prime_volume_mode",
    "nozzle_volume_type",
    "filament_map_mode",
    "filament_map",
    "filament_volume_map",
    "filament_nozzle_map",
    "has_filament_switcher",
    "enable_filament_dynamic_map",
    "filament_is_mixed",
    "filament_mixed_components",
    "filament_mixed_sublayer_ratios",
    "filament_mixed_gradient",
    "filament_mixed_gradient_range",
    "filament_mixed_gradient_curve",
    "filament_mixed_gradient_per_part"
};

static std::vector<std::string> s_project_options_published{"wipe_tower_x", "wipe_tower_y", "wipe_tower_rotation_angle"};

const char *PresetBundle::ORCA_DEFAULT_BUNDLE = "Custom";
const char *PresetBundle::ORCA_DEFAULT_PRINTER_MODEL = "MyKlipper 0.4 nozzle";
const char *PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT = "0.4";
const char *PresetBundle::ORCA_DEFAULT_FILAMENT = "Generic PLA @System";
const char *PresetBundle::ORCA_FILAMENT_LIBRARY = "OrcaFilamentLibrary";
const char *PresetBundle::ORCA_DEFAULT_FILAMENT_PLACEHOLDER = "Default Filament";

DynamicPrintConfig PresetBundle::construct_full_config(
    Preset& in_printer_preset,
    Preset& in_print_preset,
    const DynamicPrintConfig& project_config,
    std::vector<Preset>& in_filament_presets,
    bool apply_extruder,
    std::optional<std::vector<int>> filament_maps_new,
    std::optional<std::vector<int>> filament_volume_maps_new)
{
    DynamicPrintConfig &printer_config = in_printer_preset.config;
    DynamicPrintConfig &print_config   = in_print_preset.config;

    DynamicPrintConfig out;
    out.apply(FullPrintConfig::defaults());
    out.apply(printer_config);
    out.apply(print_config);
    out.apply(project_config);
    out.apply(in_filament_presets[0].config);

    size_t num_filaments = in_filament_presets.size();

    std::vector<int> filament_maps = out.option<ConfigOptionInts>("filament_map")->values;
    std::vector<int> filament_volume_maps(num_filaments, (int)nvtStandard);

    ConfigOptionInts* filament_volume_map_opt = out.option<ConfigOptionInts>("filament_volume_map");
    if (filament_maps_new.has_value())
        filament_maps = *filament_maps_new;
    if (filament_volume_maps_new.has_value())
        filament_volume_maps = *filament_volume_maps_new;
    else if (filament_volume_map_opt && filament_volume_map_opt->values.size() == num_filaments)
        filament_volume_maps = filament_volume_map_opt->values;

    if (filament_maps.size() != num_filaments) {
        filament_maps.resize(num_filaments, 1);
    }
    if (filament_volume_maps.size() != num_filaments) {
        filament_volume_maps.resize(num_filaments, nvtStandard);
    }

    auto *extruder_diameter = dynamic_cast<const ConfigOptionFloats *>(out.option("nozzle_diameter"));
    std::vector<std::string> compatible_printers_condition;
    std::vector<std::string> compatible_prints_condition;
    std::vector<std::string> inherits;
    std::vector<std::string> filament_ids;
    std::vector<std::string> print_compatible_printers;
    std::vector<std::string> different_settings;
    std::string              different_print_settings, different_printer_settings;
    compatible_printers_condition.emplace_back(in_print_preset.compatible_printers_condition());

    const ConfigOptionStrings *compatible_printers = print_config.option<ConfigOptionStrings>("compatible_printers", false);
    if (compatible_printers) print_compatible_printers = compatible_printers->values;
    std::string print_inherits = in_print_preset.inherits();
    inherits.emplace_back(print_inherits);

    std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
    int extruder_count = 1, extruder_volume_type_count = 1;
    bool different_extruder = false;
    if (apply_extruder) {
        different_extruder = out.support_different_extruders(extruder_count);
        extruder_volume_type_count = out.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        if ((extruder_count > 1) || different_extruder) {
            out.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, printer_options_with_variant_1, "printer_extruder_id", "printer_extruder_variant");
            out.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, printer_options_with_variant_2, "printer_extruder_id", "printer_extruder_variant", 2);
            out.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, print_options_with_variant, "print_extruder_id", "print_extruder_variant");
        }
    }

    if (num_filaments <= 1) {
        DynamicPrintConfig filament_config = in_filament_presets[0].config;
        if (apply_extruder && ((extruder_count > 1) || different_extruder || filament_config.has_multi_variant_filament()))
            filament_config.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, filament_options_with_variant, "", "filament_extruder_variant", 1, filament_maps[0], (NozzleVolumeType)filament_volume_maps[0]);
        out.apply(filament_config);
        compatible_printers_condition.emplace_back(in_filament_presets[0].compatible_printers_condition());
        compatible_prints_condition.emplace_back(in_filament_presets[0].compatible_prints_condition());
        std::string filament_inherits = in_filament_presets[0].inherits();
        inherits.emplace_back(filament_inherits);
        filament_ids.emplace_back(in_filament_presets[0].filament_id);

        std::vector<int> &filament_self_indice = out.option<ConfigOptionInts>("filament_self_index", true)->values;
        int               index_size           = out.option<ConfigOptionStrings>("filament_extruder_variant")->size();
        filament_self_indice.resize(index_size, 1);
    } else {
        std::vector<const DynamicPrintConfig *> filament_configs;
        std::vector<const Preset *>             filament_presets;
        for (const Preset & preset : in_filament_presets) {
            filament_presets.emplace_back(&preset);
            filament_configs.emplace_back(&(preset.config));
        }

        std::vector<DynamicPrintConfig> filament_temp_configs;
        filament_temp_configs.resize(num_filaments);
        for (size_t i = 0; i < num_filaments; ++i) {
            filament_temp_configs[i] = *(filament_configs[i]);
            if (apply_extruder && ((extruder_count > 1) || different_extruder || filament_temp_configs[i].has_multi_variant_filament()))
                filament_temp_configs[i].update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, filament_options_with_variant, "", "filament_extruder_variant", 1, filament_maps[i], (NozzleVolumeType)filament_volume_maps[i]);
        }

        std::vector<int> filament_variant_count(num_filaments, 1);
        for (const t_config_option_key &key : in_filament_presets[0].config.keys()) {
            if (key == "compatible_prints" || key == "compatible_printers") continue;
            ConfigOption *opt_dst = out.option(key, false);
            if (opt_dst->is_scalar()) {
                const ConfigOption *opt_src = filament_temp_configs.front().option(key);
                if (opt_src != nullptr) opt_dst->set(opt_src);
            } else {
                ConfigOptionVectorBase *opt_vec_dst = static_cast<ConfigOptionVectorBase *>(opt_dst);
                {
                    if (apply_extruder) {
                        std::vector<const ConfigOption *> filament_opts(num_filaments, nullptr);
                        for (size_t i = 0; i < filament_opts.size(); ++i) filament_opts[i] = filament_temp_configs[i].option(key);
                        opt_vec_dst->set(filament_opts);
                    } else {
                        for (size_t i = 0; i < num_filaments; ++i) {
                            const ConfigOptionVectorBase *filament_option = static_cast<const ConfigOptionVectorBase *>(filament_temp_configs[i].option(key));
                            if (i == 0)
                                opt_vec_dst->set(filament_option);
                            else
                                opt_vec_dst->append(filament_option);

                            if (key == "filament_extruder_variant") filament_variant_count[i] = filament_option->size();
                        }
                    }
                }
            }
        }

        if (!apply_extruder) {
            std::vector<int> &filament_self_indice = out.option<ConfigOptionInts>("filament_self_index", true)->values;
            int               index_size           = out.option<ConfigOptionStrings>("filament_extruder_variant")->size();
            filament_self_indice.resize(index_size, 1);
            int k = 0;
            for (size_t i = 0; i < num_filaments; i++) {
                for (size_t j = 0; j < filament_variant_count[i]; j++) { filament_self_indice[k++] = i + 1; }
            }
        }
    }

    out.erase("compatible_prints");
    out.erase("compatible_prints_condition");
    out.erase("compatible_printers");
    out.erase("compatible_printers_condition");
    out.erase("inherits");
    out.erase("different_settings_to_system");

    static const char *keys[] = {"support_filament", "support_interface_filament"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        std::string key = std::string(keys[i]);
        auto       *opt = dynamic_cast<ConfigOptionInt *>(out.option(key, false));
        assert(opt != nullptr);
        opt->value = boost::algorithm::clamp<int>(opt->value, 0, int(num_filaments));
    }

    std::vector<std::string> filamnet_preset_names;
    for (auto preset : in_filament_presets) {
        filamnet_preset_names.emplace_back(preset.name);
    }
    out.option<ConfigOptionString>("print_settings_id", true)->value      = in_print_preset.name;
    out.option<ConfigOptionStrings>("filament_settings_id", true)->values = filamnet_preset_names;
    out.option<ConfigOptionString>("printer_settings_id", true)->value    = in_printer_preset.name;
    out.option<ConfigOptionStrings>("filament_ids", true)->values         = filament_ids;
    out.option<ConfigOptionInts>("filament_map", true)->values            = filament_maps;
    out.option<ConfigOptionInts>("filament_volume_map", true)->values     = filament_volume_maps;

    auto add_if_some_non_empty = [&out](std::vector<std::string> &&values, const std::string &key) {
        bool nonempty = false;
        for (const std::string &v : values)
            if (!v.empty()) {
                nonempty = true;
                break;
            }
        if (nonempty) out.set_key_value(key, new ConfigOptionStrings(std::move(values)));
    };
    add_if_some_non_empty(std::move(compatible_printers_condition), "compatible_machine_expression_group");
    add_if_some_non_empty(std::move(compatible_prints_condition), "compatible_process_expression_group");
    add_if_some_non_empty(std::move(inherits), "inherits_group");
    add_if_some_non_empty(std::move(print_compatible_printers), "print_compatible_printers");

    out.option<ConfigOptionEnumGeneric>("printer_technology", true)->value = ptFFF;
    return out;
}

std::string PresetBundle::find_preset_vendor(const std::string &preset_name, Preset::Type type)
{
    fs::path system_dir = fs::path(Slic3r::resources_dir()) / PRESET_PROFILES_DIR;
    if (!fs::exists(system_dir) || !fs::is_directory(system_dir)) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << " Resources profiles directory does not exist: " << system_dir.string();
        return "";
    }

    const char* preset_list_key = nullptr;
    if (type == Preset::Type::TYPE_PRINT)
        preset_list_key = BBL_JSON_KEY_PROCESS_LIST;
    else if (type == Preset::Type::TYPE_FILAMENT)
        preset_list_key = BBL_JSON_KEY_FILAMENT_LIST;
    else if (type == Preset::Type::TYPE_PRINTER)
        preset_list_key = BBL_JSON_KEY_MACHINE_LIST;
    else {
        return "";
    }

    for (const std::string& vendor_name : vendor_names_in(system_dir)) {
        const fs::path vendor_json = system_dir / (vendor_name + ".json");
        if (! fs::exists(vendor_json)) {
            if (VendorCacheFile::carries_preset((system_dir / (vendor_name + ".opc")).string(), vendor_name, type, preset_name)) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Found preset " << preset_name
                                        << " in vendor cache " << vendor_name;
                return vendor_name;
            }
            continue;
        }
        const std::string vendor_file = vendor_json.string();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Checking vendor: " << vendor_file;

        try {
            boost::nowide::ifstream ifs(vendor_file);
            json j;
            ifs >> j;

            if (!j.contains(preset_list_key))
                continue;

            auto& preset_list = j[preset_list_key];
            if (!preset_list.is_array())
                continue;

            for (auto& preset_entry : preset_list) {
                if (!preset_entry.is_object())
                    continue;

                std::string p_name;
                if (preset_entry.contains(BBL_JSON_KEY_NAME) && preset_entry[BBL_JSON_KEY_NAME].is_string())
                    p_name = preset_entry[BBL_JSON_KEY_NAME].get<std::string>();

                if (p_name != preset_name)
                    continue;

                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Found preset " << p_name
                                            << " in vendor bundle " << vendor_name;
                
                return vendor_name;
            }
        }
        catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to find vendor name for " << preset_name << ": " << e.what();
            return "";
        }
    }

    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Could not find vendor for preset " << preset_name;
    return "";
}

PresetBundle::PresetBundle()
    : prints(Preset::TYPE_PRINT, Preset::print_options(), static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    , filaments(Preset::TYPE_FILAMENT, Preset::filament_options(), static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), ORCA_DEFAULT_FILAMENT_PLACEHOLDER)
    , sla_materials(Preset::TYPE_SLA_MATERIAL, Preset::sla_material_options(), static_cast<const SLAMaterialConfig &>(SLAFullPrintConfig::defaults()))
    , sla_prints(Preset::TYPE_SLA_PRINT, Preset::sla_print_options(), static_cast<const SLAPrintObjectConfig &>(SLAFullPrintConfig::defaults()))
    , printers(Preset::TYPE_PRINTER, Preset::printer_options(), static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), "Default Printer")
    , physical_printers(PhysicalPrinter::printer_options())
{
    this->prints.default_preset().config.optptr("print_settings_id", true);
    this->prints.default_preset().compatible_printers_condition();
    this->prints.default_preset().inherits();

    this->filaments.default_preset().config.option<ConfigOptionStrings>("filament_settings_id", true)->values = {""};
    this->filaments.default_preset().compatible_printers_condition();
    this->filaments.default_preset().inherits();
    {
        auto& default_config = this->filaments.default_preset().config;
        for(const std::string& opt_key : default_config.keys()){
            ConfigOption* opt = default_config.optptr(opt_key, false);
            bool is_override_key = is_filament_extruder_override_key(opt_key);
            if(!is_override_key || !opt->nullable()) 
                continue;
            opt->deserialize("nil",ForwardCompatibilitySubstitutionRule::Disable);
        }
    }

    this->sla_materials.default_preset().config.optptr("sla_material_settings_id", true);
    this->sla_materials.default_preset().compatible_printers_condition();
    this->sla_materials.default_preset().inherits();

    this->sla_prints.default_preset().config.optptr("sla_print_settings_id", true);
    this->sla_prints.default_preset().config.opt_string("filename_format", true) = "[input_filename_base].sl1";
    this->sla_prints.default_preset().compatible_printers_condition();
    this->sla_prints.default_preset().inherits();

    for (size_t i = 0; i < 1; ++i) {
        Preset &preset = this->printers.default_preset(i);
        for (const char *key : {"printer_settings_id", "printer_model", "printer_variant", "thumbnails"}) preset.config.optptr(key, true);
            preset.config.optptr("default_print_profile", true);
            preset.config.option<ConfigOptionStrings>("default_filament_profile", true);
        preset.inherits();
    }

    this->prints.select_preset(0);
    this->sla_prints.select_preset(0);
    this->filaments.select_preset(0);
    this->sla_materials.select_preset(0);
    this->printers.select_preset(0);

    this->project_config.apply_only(FullPrintConfig::defaults(), s_project_options);
}

bool PresetBundle::resolve_preset_config(DynamicPrintConfig &config, Preset::Type type, const std::string &source_file,
                                         ForwardCompatibilitySubstitutionRule compatibility_rule, std::string &error,
                                         bool allow_source_manifest)
{
    return resolve_preset_config(config, type, source_file, compatibility_rule, error, allow_source_manifest, nullptr);
}

bool PresetBundle::resolve_preset_config_type(DynamicPrintConfig &config, Preset::Type &type, const std::string &source_file,
                                              ForwardCompatibilitySubstitutionRule compatibility_rule, std::string &error,
                                              bool allow_source_manifest)
{
    return resolve_preset_config_type(config, type, source_file, compatibility_rule, error, allow_source_manifest, nullptr);
}

bool PresetBundle::resolve_system_preset(DynamicPrintConfig &config, Preset::Type type, const std::string &name,
                                         ForwardCompatibilitySubstitutionRule compatibility_rule, std::string &error)
{
    return resolve_system_preset(config, type, name, compatibility_rule, error, nullptr);
}

bool PresetBundle::resolve_preset_config(DynamicPrintConfig &config, Preset::Type type,
                                         const std::string &source_file,
                                         ForwardCompatibilitySubstitutionRule compatibility_rule,
                                         std::string &error, bool allow_source_manifest, std::string *filament_id)
{
    if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSystemSilent)
        compatibility_rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
    else if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem)
        compatibility_rule = ForwardCompatibilitySubstitutionRule::Disable;

    auto collection_for_type = [](const PresetBundle &bundle, Preset::Type preset_type) -> const PresetCollection * {
        switch (preset_type) {
        case Preset::TYPE_PRINT:    return &bundle.prints;
        case Preset::TYPE_FILAMENT: return &bundle.filaments;
        case Preset::TYPE_PRINTER:  return &bundle.printers;
        default:                    return nullptr;
        }
    };

    const PresetCollection *collection = collection_for_type(*this, type);
    if (collection == nullptr) {
        error = "Unsupported preset type";
        return false;
    }

    const boost::filesystem::path source_path = boost::filesystem::absolute(source_file).lexically_normal();
    auto find_loaded = [&](const PresetBundle &bundle) -> const Preset * {
        const PresetCollection *loaded_collection = collection_for_type(bundle, type);
        const Preset *resolved = nullptr;
        for (const Preset &preset : loaded_collection->get_presets()) {
            if (preset.file.empty())
                continue;

            boost::system::error_code ec;
            const bool same_file = boost::filesystem::equivalent(source_path, boost::filesystem::path(preset.file), ec);
            if (ec || !same_file)
                continue;
            if (resolved != nullptr) {
                error = "Preset identity is ambiguous";
                return nullptr;
            }
            resolved = &preset;
        }
        return resolved;
    };

    if (const Preset *resolved = find_loaded(*this)) {
        config = resolved->config;
        if (filament_id)
            *filament_id = resolved->filament_id;
        error.clear();
        return true;
    }
    if (error == "Preset identity is ambiguous")
        return false;
    if (!allow_source_manifest) {
        error = "Preset was not found in the loaded bundle";
        return false;
    }

    for (boost::filesystem::path vendor_dir = source_path.parent_path(); !vendor_dir.empty(); vendor_dir = vendor_dir.parent_path()) {
        const std::string vendor_id = vendor_dir.filename().string();
        if (vendor_id.empty())
            continue;
        const boost::filesystem::path root_dir = vendor_dir.parent_path();
        const boost::filesystem::path manifest = root_dir / (vendor_id + ".json");
        if (!boost::filesystem::is_regular_file(manifest))
            continue;
        const boost::filesystem::path manifest_relative = source_path.lexically_relative(vendor_dir);
        if (manifest_relative.empty() || *manifest_relative.begin() == "..")
            continue;

        try {
            const PresetBundle *loaded = load_source_vendor(root_dir, vendor_id, compatibility_rule, error);
            if (loaded == nullptr)
                return false;

            const Preset *resolved = find_loaded(*loaded);
            if (resolved == nullptr) {
                if (error.empty())
                    error = "Source file is not an instantiated preset in its vendor manifest";
                return false;
            }
            config = resolved->config;
            if (filament_id)
                *filament_id = resolved->filament_id;
            error.clear();
            return true;
        } catch (const std::exception &ex) {
            error = ex.what();
            return false;
        }
    }

    error = "Preset was not found in the loaded bundle";
    return false;
}

const PresetBundle *PresetBundle::load_source_vendor(const boost::filesystem::path &root_dir,
                                                    const std::string &vendor_id,
                                                    ForwardCompatibilitySubstitutionRule compatibility_rule,
                                                    std::string &error, bool allow_cache)
{
    auto key = std::make_tuple(root_dir.string(), vendor_id, compatibility_rule, allow_cache);
    if (auto it = m_source_vendor_bundles.find(key); it != m_source_vendor_bundles.end())
        return it->second.get();

    const std::string   library_file       = std::string(ORCA_FILAMENT_LIBRARY);
    const bool          library_json       = boost::filesystem::is_regular_file(root_dir / (library_file + ".json"));
    const bool          library_cache_only = !library_json && boost::filesystem::is_regular_file(root_dir / (library_file + ".opc"));
    const PresetBundle *library            = nullptr;
    if (vendor_id != ORCA_FILAMENT_LIBRARY && (library_json || library_cache_only)) {
        library = load_source_vendor(root_dir, ORCA_FILAMENT_LIBRARY, compatibility_rule, error, allow_cache || library_cache_only);
        if (library == nullptr) {
            error = "OrcaFilamentLibrary contains invalid presets";
            return nullptr;
        }
    }

    auto bundle = std::make_unique<PresetBundle>();
    bundle->m_preserve_vendor_source_paths = true;
    bundle->load_vendor_configs_from_json(root_dir.string(), vendor_id, LoadSystem, compatibility_rule, library, allow_cache);
    if (bundle->error_count() != 0) {
        error = "Vendor bundle contains invalid presets";
        return nullptr;
    }
    return m_source_vendor_bundles.emplace(std::move(key), std::move(bundle)).first->second.get();
}

bool PresetBundle::resolve_preset_config_type(DynamicPrintConfig &config, Preset::Type &type,
                                              const std::string &source_file,
                                              ForwardCompatibilitySubstitutionRule compatibility_rule,
                                              std::string &error, bool allow_source_manifest, std::string *filament_id)
{
    std::optional<std::pair<Preset::Type, DynamicPrintConfig>> resolved;
    for (Preset::Type candidate_type : types_list(ptFFF)) {
        DynamicPrintConfig candidate_config(config);
        std::string        candidate_error;
        if (!resolve_preset_config(candidate_config, candidate_type, source_file, compatibility_rule,
                                   candidate_error, allow_source_manifest)) {
            if (candidate_error == "Preset identity is ambiguous") {
                error = std::move(candidate_error);
                return false;
            }
            continue;
        }
        if (resolved) {
            error = "Preset type is ambiguous";
            return false;
        }
        resolved.emplace(candidate_type, std::move(candidate_config));
    }

    if (!resolved) {
        error = "Preset type could not be resolved";
        return false;
    }

    if (filament_id && !resolve_preset_config(config, resolved->first, source_file, compatibility_rule, error, allow_source_manifest, filament_id))
        return false;
    type   = resolved->first;
    config = std::move(resolved->second);
    error.clear();
    return true;
}

bool PresetBundle::resolve_system_preset(DynamicPrintConfig &config, Preset::Type type, const std::string &name,
                                         ForwardCompatibilitySubstitutionRule compatibility_rule, std::string &error, std::string *filament_id)
{
    const std::string vendor_id = find_preset_vendor(name, type);
    if (vendor_id.empty()) {
        error = "No vendor lists the preset";
        return false;
    }
    auto installed = [&vendor_id](const fs::path &root) {
        return fs::is_regular_file(root / (vendor_id + ".json")) || fs::is_regular_file(root / (vendor_id + ".opc"));
    };
    fs::path root_dir = fs::path(data_dir()) / PRESET_SYSTEM_DIR;
    if (!installed(root_dir))
        root_dir = fs::path(resources_dir()) / PRESET_PROFILES_DIR;
    const bool cache_only = !fs::is_regular_file(root_dir / (vendor_id + ".json"));

    try {
        const PresetBundle *vendor = load_source_vendor(root_dir, vendor_id, compatibility_rule, error, cache_only);
        if (vendor == nullptr)
            return false;
        const PresetCollection &collection = type == Preset::TYPE_PRINTER ? vendor->printers :
                                             type == Preset::TYPE_PRINT   ? vendor->prints : vendor->filaments;
        const Preset *preset = collection.find_preset(name, false);
        if (preset == nullptr) {
            error = "Preset was not found in its vendor bundle";
            return false;
        }
        config = preset->config;
        if (filament_id)
            *filament_id = preset->filament_id;
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    }
    error.clear();
    return true;
}

PresetBundle::PresetBundle(const PresetBundle &rhs)
{
    *this = rhs;
}

PresetBundle& PresetBundle::operator=(const PresetBundle &rhs)
{
    prints              = rhs.prints;
    sla_prints          = rhs.sla_prints;
    filaments           = rhs.filaments;
    sla_materials       = rhs.sla_materials;
    printers            = rhs.printers;
    physical_printers   = rhs.physical_printers;

    filament_presets    = rhs.filament_presets;
    project_config      = rhs.project_config;
    vendors             = rhs.vendors;
    obsolete_presets    = rhs.obsolete_presets;
    m_errors    = rhs.m_errors;

    prints       .update_vendor_ptrs_after_copy(this->vendors);
    sla_prints   .update_vendor_ptrs_after_copy(this->vendors);
    filaments    .update_vendor_ptrs_after_copy(this->vendors);
    sla_materials.update_vendor_ptrs_after_copy(this->vendors);
    printers     .update_vendor_ptrs_after_copy(this->vendors);

    return *this;
}

void PresetBundle::reset(bool delete_files)
{
    this->vendors.clear();
    this->prints       .reset(delete_files);
    this->sla_prints   .reset(delete_files);
    this->filaments    .reset(delete_files);
    this->sla_materials.reset(delete_files);
    this->printers     .reset(delete_files);
    if (this->filament_presets.empty())
        this->filament_presets.emplace_back(this->filaments.get_selected_preset_name());
    this->obsolete_presets.prints.clear();
    this->obsolete_presets.sla_prints.clear();
    this->obsolete_presets.filaments.clear();
    this->obsolete_presets.sla_materials.clear();
    this->obsolete_presets.printers.clear();
}

void PresetBundle::setup_directories()
{
    boost::filesystem::path data_dir = boost::filesystem::path(Slic3r::data_dir());
    std::initializer_list<boost::filesystem::path> paths = {
        data_dir,
        data_dir / "ota",
		data_dir / PRESET_SYSTEM_DIR,
        data_dir / PRESET_USER_DIR,
    };
    for (const boost::filesystem::path &path : paths) {
		boost::filesystem::path subdir = path;
        subdir.make_preferred();
        if (! boost::filesystem::is_directory(subdir) &&
            ! boost::filesystem::create_directory(subdir)) {
            if (boost::filesystem::is_directory(subdir)) {
                BOOST_LOG_TRIVIAL(warning) << boost::format("creating directory %1% failed, maybe created by other instance, go on!")%subdir.string();
            }
            else
                throw Slic3r::RuntimeError(std::string("Unable to create directory ") + subdir.string());
        }
    }
}

static void copy_dir(const boost::filesystem::path& from_dir, const boost::filesystem::path& to_dir)
{
    if(!boost::filesystem::is_directory(from_dir))
        return;
    if (!boost::filesystem::is_directory(to_dir))
        boost::filesystem::create_directory(to_dir);
    for (auto& dir_entry : boost::filesystem::directory_iterator(from_dir)) {
        if (!boost::filesystem::is_directory(dir_entry.path())) {
            std::string em;
            CopyFileResult cfr = copy_file(dir_entry.path().string(), (to_dir / dir_entry.path().filename()).string(), em, false);
            if (cfr != SUCCESS) {
                BOOST_LOG_TRIVIAL(error) << "Error when copying files from " << from_dir << " to " << to_dir << ": " << em;
            }
        } else {
            copy_dir(dir_entry.path(), to_dir / dir_entry.path().filename());
        }
    }
}

void PresetBundle::copy_files(const std::string& from)
{
    boost::filesystem::path data_dir = boost::filesystem::path(Slic3r::data_dir());
    boost::filesystem::path from_data_dir = boost::filesystem::path(from);
    std::initializer_list<boost::filesystem::path> from_dirs= {
        from_data_dir / PRESET_PRINT_NAME,
        from_data_dir / PRESET_FILAMENT_NAME,
        from_data_dir / PRESET_PRINTER_NAME
    };
    for (const boost::filesystem::path& from_dir : from_dirs) {
        copy_dir(from_dir, data_dir /"old"/from_dir.filename());
    }
}

PresetsConfigSubstitutions PresetBundle::load_presets(AppConfig &config, ForwardCompatibilitySubstitutionRule substitution_rule,
                                                      const PresetPreferences& preferred_selection ,
                                                      std::string *errors, bool read_only)
{
    PresetsConfigSubstitutions substitutions;
    std::string errors_cummulative;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" enter, substitution_rule %1%, preferred printer_model_id %2%")%substitution_rule%preferred_selection.printer_model_id;
    const auto startup_t0 = std::chrono::steady_clock::now();

    std::tie(substitutions, errors_cummulative) = this->load_system_presets_from_json(substitution_rule, !read_only);
    if (errors != nullptr)
        *errors = errors_cummulative;

    std::string dir_user_presets = config.get("preset_folder");
    if (dir_user_presets.empty()) {
        load_user_presets(DEFAULT_USER_FOLDER_NAME, substitution_rule, read_only);
    } else {
        load_user_presets(dir_user_presets, substitution_rule, read_only);
    }
    if (errors != nullptr && errors->empty() && m_errors != 0)
        *errors = "Preset loading reported " + std::to_string(m_errors) + " error(s)";

    if (!validation_mode)
        this->normalize_compatible_presets();

    this->update_multi_material_filament_presets();
    this->update_compatible(PresetSelectCompatibleType::Never);

    this->load_selections(config, preferred_selection);

    set_calibrate_printer("");

    {
        const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startup_t0).count();
        BOOST_LOG_TRIVIAL(info) << "PresetBundle: all presets loaded in " << total_ms << " ms";
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" finished, returned substitutions %1%")%substitutions.size();
    return substitutions;
}

Preset* PresetBundle::get_preset_differed_for_save(Preset& preset)
{
    PresetCollection* preset_collection;

    switch(preset.type) {
        case Preset::TYPE_PRINT:
            preset_collection = &(this->prints);
            break;
        case Preset::TYPE_PRINTER:
            preset_collection = &(this->printers);
            break;
        case Preset::TYPE_FILAMENT:
            preset_collection = &(this->filaments);
            break;
        default:
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" invalid type %1%, return directly")%preset.type;
            return nullptr;
    }

    return preset_collection->get_preset_differed_for_save(preset);
}

int PresetBundle::get_differed_values_to_update(Preset& preset, std::map<std::string, std::string>& key_values)
{
    PresetCollection* preset_collection;

    switch(preset.type) {
        case Preset::TYPE_PRINT:
            preset_collection = &(this->prints);
            break;
        case Preset::TYPE_PRINTER:
            preset_collection = &(this->printers);
            break;
        case Preset::TYPE_FILAMENT:
            preset_collection = &(this->filaments);
            break;
        default:
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" invalid type %1%, return directly")%preset.type;
            return -1;
    }

    return preset_collection->get_differed_values_to_update(preset, key_values);
}

Semver PresetBundle::get_vendor_profile_version(std::string vendor_name)
{
    Semver result_ver;

    auto vendor_profile = vendors.find(vendor_name);
    if (vendor_profile != vendors.end()) {
        result_ver = vendor_profile->second.config_version;
    }

    return result_ver;
}

VendorType PresetBundle::get_current_vendor_type()
{
    auto        t      = VendorType::Unknown;
    auto        config = &printers.get_edited_preset().config;
    const auto* printer_model = config->opt<ConfigOptionString>("printer_model");
    if (printer_model == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": printer_model is "
                                   << (config->has("printer_model") ? "not a string" : "missing")
                                   << ", vendor type is Unknown";
        return t;
    }

    std::string vendor_name;
    for (const auto& vendor_profile : vendors) {
        for (const auto& vendor_model : vendor_profile.second.models) {
            if (vendor_model.name == printer_model->value) {
                vendor_name = vendor_profile.first;
                break;
            }
        }
        if (!vendor_name.empty())
            break;
    }
    if (!vendor_name.empty())
    {
        if(vendor_name.compare("BBL") == 0)
            t = VendorType::Marlin_BBL;
        
        if(vendor_name.compare("Qidi") == 0)
            t = VendorType::Klipper_Qidi;
    }
    return t;
}

bool PresetBundle::use_bbl_network()
{
    const auto cfg             = printers.get_edited_preset().config;
    const bool use_bbl_network = is_bbl_vendor() && !cfg.opt_bool("bbl_use_printhost");
    return use_bbl_network;
}

bool PresetBundle::use_bbl_device_tab() {
    if (!is_bbl_vendor()) {
        return false;
    }

    if (use_bbl_network()) {
        return true;
    }

    const auto cfg = printers.get_edited_preset().config;
    return cfg.opt_string("print_host_webui").empty();
}

bool PresetBundle::backup_user_folder() const
{
    const std::string backup_folderpath = data_dir() + "/" + (boost::format("user_backup-v%1%") % SoftFever_VERSION).str();

    if (boost::filesystem::exists(boost::filesystem::path(backup_folderpath)))
        return false;

    BOOST_LOG_TRIVIAL(info) << "Backing up user folder to: " << backup_folderpath;
    try {
        boost::filesystem::copy(data_dir() + "/" + PRESET_USER_DIR, backup_folderpath, boost::filesystem::copy_options::recursive);
        BOOST_LOG_TRIVIAL(info) << "User folder backup completed successfully";
        return true;
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "Exception during user folder backup: " << ex.what();
        if (boost::filesystem::exists(boost::filesystem::path(backup_folderpath)))
            boost::filesystem::remove_all(boost::filesystem::path(backup_folderpath));
        return false;
    }
}

std::optional<FilamentBaseInfo> PresetBundle::get_filament_by_filament_id(const std::string& filament_id, const std::string& printer_name) const
{
    if (filament_id.empty())
        return std::nullopt;

    for (auto iter = filaments.begin(); iter != filaments.end(); ++iter) {
        const Preset& filament_preset = *iter;
        const auto& config = filament_preset.config;
        if (filament_preset.filament_id == filament_id) {
            FilamentBaseInfo info;
            info.filament_id = filament_id;
            info.is_system = filament_preset.is_system;
            info.filament_name = filament_preset.alias;
            if (config.has("filament_is_support"))
                info.is_support = config.option<ConfigOptionBools>("filament_is_support")->values[0];
            if (config.has("filament_type"))
                info.filament_type = config.option<ConfigOptionStrings>("filament_type")->values[0];
            if (config.has("filament_vendor"))
                info.vendor = config.option<ConfigOptionStrings>("filament_vendor")->values[0];
            if (config.has("nozzle_temperature_range_high"))
                info.nozzle_temp_range_high = config.option<ConfigOptionInts>("nozzle_temperature_range_high")->values[0];
            if (config.has("nozzle_temperature_range_low"))
                info.nozzle_temp_range_low = config.option<ConfigOptionInts>("nozzle_temperature_range_low")->values[0];
            if(config.has("temperature_vitrification"))
                info.temperature_vitrification = config.option<ConfigOptionInts>("temperature_vitrification")->values[0];

            if (!printer_name.empty()) {
                std::vector<std::string> compatible_printers = config.option<ConfigOptionStrings>("compatible_printers")->values;
                auto iter = std::find(compatible_printers.begin(), compatible_printers.end(), printer_name);
                if (iter != compatible_printers.end() && config.has("filament_printable")) {
                    info.filament_printable = config.option<ConfigOptionInts>("filament_printable")->values[0];
                    if (config.has("filament_extruder_compatibility"))
                        info.set_filament_extruder_compatibility(config.option<ConfigOptionInts>("filament_extruder_compatibility")->values[0]);
                    return info;
                }
            }
            else {
                return info;
            }
        }
    }
    return std::nullopt;
}

PresetsConfigSubstitutions PresetBundle::load_project_embedded_presets(std::vector<Preset*> project_presets, ForwardCompatibilitySubstitutionRule substitution_rule)
{
    PresetsConfigSubstitutions substitutions;
    std::string errors_cummulative;

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, substitution_rule %1%, preset toltal count %2%")%substitution_rule% project_presets.size();
    try {
        this->prints.load_project_embedded_presets(project_presets, PRESET_PRINT_NAME, substitutions, substitution_rule);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }
    try {
        this->filaments.load_project_embedded_presets(project_presets, PRESET_FILAMENT_NAME, substitutions, substitution_rule);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }
    try {
        this->printers.load_project_embedded_presets(project_presets, PRESET_PRINTER_NAME, substitutions, substitution_rule);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }

    this->normalize_compatible_presets();
    if (! errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, returned substitutions %1%")%substitutions.size();
    return substitutions;
}

std::vector<Preset*> PresetBundle::get_current_project_embedded_presets()
{
    std::vector<Preset*> project_presets;

    project_presets = this->prints.get_project_embedded_presets();

    auto filament_presets = this->filaments.get_project_embedded_presets();
    if (!filament_presets.empty())
        std::copy(filament_presets.begin(), filament_presets.end(), std::back_inserter(project_presets));
    auto printer_presets = this->printers.get_project_embedded_presets();
    if (!printer_presets.empty())
        std::copy(printer_presets.begin(), printer_presets.end(), std::back_inserter(project_presets));

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, returned project_presets count %1%")%project_presets.size();
    return project_presets;
}

void PresetBundle::reset_project_embedded_presets()
{
    std::string prefer_printer;
    Preset& current_printer = this->printers.get_selected_preset();
    ConfigOption* inherits = current_printer.config.option("inherits");
    if (inherits) {
        prefer_printer = dynamic_cast<ConfigOptionString *>(inherits)->value;
    }
    bool printer_reselect = this->printers.reset_project_embedded_presets();
    bool filament_reselect = this->filaments.reset_project_embedded_presets();
    bool print_reselect = this->prints.reset_project_embedded_presets();

    if (printer_reselect) {
        if (!prefer_printer.empty())
           this->printers.select_preset_by_name(prefer_printer, true);
        else
           this->printers.select_preset(this->printers.first_visible_idx());

        this->update_compatible(PresetSelectCompatibleType::Never);
    }
    else if (filament_reselect || print_reselect) {
        this->update_compatible(PresetSelectCompatibleType::Never);
    }

    for (size_t i = 0; i < filament_presets.size(); ++ i)
    {
        Preset* selected_filament = this->filaments.find_preset(filament_presets[i], false);
        if (!selected_filament) {
            Preset& current_printer = this->printers.get_selected_preset();
            const std::vector<std::string> &prefered_filament_profiles = current_printer.config.option<ConfigOptionStrings>("default_filament_profile")->values;
            const std::string prefered_filament_profile = prefered_filament_profiles.empty() ? std::string() : prefered_filament_profiles.front();
            if (!prefered_filament_profile.empty()) {
                const Preset* preferred_preset = this->filaments.find_preset(prefered_filament_profile, false);
                if (preferred_preset && preferred_preset->is_visible) {
                    filament_presets[i] = prefered_filament_profile;
                } else {
                    filament_presets[i] = this->filaments.first_visible().name;
                }
            } else
                filament_presets[i] = this->filaments.first_visible().name;
        }
    }
}

std::string PresetBundle::get_texture_for_printer_model(std::string model_name)
{
    std::string texture_name, vendor_name, out;

    for (auto vendor_profile: this->vendors)
    {
        for (auto vendor_model: vendor_profile.second.models)
        {
            if (vendor_model.name == model_name || vendor_model.id == model_name)
            {
                texture_name = vendor_model.bed_texture;
                vendor_name = vendor_profile.first;
                break;
            }
        }
    }

    if (!texture_name.empty())
    {
        out = Slic3r::data_dir() + "/vendor/" + vendor_name + "/" + texture_name;
        if (!boost::filesystem::exists(boost::filesystem::path(out)))
            out = Slic3r::resources_dir() + "/profiles/" + vendor_name + "/" + texture_name;
    }

    return out;
}

std::string PresetBundle::get_stl_model_for_printer_model(std::string model_name)
{
    std::string stl_name, vendor_name, out;

    for (auto vendor_profile: this->vendors)
    {
        for (auto vendor_model: vendor_profile.second.models)
        {
            if (vendor_model.name == model_name)
            {
                stl_name = vendor_model.bed_model;
                vendor_name = vendor_profile.first;
                break;
            }
        }
    }

    if (!stl_name.empty())
    {
        out = Slic3r::data_dir() + "/vendor/" + vendor_name + "/" + stl_name;
        if (!boost::filesystem::exists(boost::filesystem::path(out)))
            out = Slic3r::resources_dir() + "/profiles/" + vendor_name + "/" + stl_name;
    }

    return out;
}

std::string PresetBundle::get_hotend_model_for_printer_model(std::string model_name)
{
    std::string hotend_stl, vendor_name, out;

    for (auto vendor_profile: this->vendors)
    {
        for (auto vendor_model: vendor_profile.second.models)
        {
            if (vendor_model.name == model_name)
            {
                hotend_stl = vendor_model.hotend_model;
                vendor_name = vendor_profile.first;
                break;
            }
        }
    }

    if (!hotend_stl.empty())
    {
        out = Slic3r::data_dir() + "/vendor/" + vendor_name + "/" + hotend_stl;
        if (!boost::filesystem::exists(boost::filesystem::path(out)))
            out = Slic3r::resources_dir() + "/profiles/" + vendor_name + "/" + hotend_stl;
    }

    if (out.empty() ||!boost::filesystem::exists(boost::filesystem::path(out)))
        out = Slic3r::resources_dir() + "/profiles/hotend.stl";

    return out;
}

PresetsConfigSubstitutions PresetBundle::load_user_presets(std::string user, ForwardCompatibilitySubstitutionRule substitution_rule, bool read_only)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " entry and user is: " << user;
    PresetsConfigSubstitutions substitutions;
    std::string errors_cummulative;

    fs::path user_folder(data_dir() + "/" + PRESET_USER_DIR);
    if (!fs::exists(user_folder)) {
        if (read_only)
            return substitutions;
        fs::create_directory(user_folder);
    }

    std::string dir_user_presets = data_dir() + "/" + PRESET_USER_DIR + "/" + user;
    fs::path    folder(user_folder / user);
    if (!fs::exists(folder)) {
        if (read_only)
            return substitutions;
        fs::create_directory(folder);
    }

    bundles.WriteLock();
    bundles.m_bundles.clear();
    bundles.WriteUnlock();

    const auto user_load_t0 = std::chrono::steady_clock::now();

    auto load_bundle_metadata = [read_only](const fs::path &metadata_file, BundleMetadata &metadata) {
        InstanceLock instance_lock(user_presets_lock_path(read_only));
        return metadata.load_from_json(metadata_file.string());
    };

    fs::path local_dir(folder / PRESET_LOCAL_DIR);
    if (fs::exists(local_dir)) {
        dir_user_presets_local = local_dir;
        for (auto& entry : fs::directory_iterator(local_dir)) {
            if (!fs::is_directory(entry.path())) continue;

            std::string bundle_dir = entry.path().string();

            fs::path metadata_file = entry.path() / PRESET_BUNDLE_METADATA;
            if (!fs::exists(metadata_file)) continue;

            BundleMetadata metadata;
            if (!load_bundle_metadata(metadata_file, metadata)) continue;
            metadata.print_presets.clear();
            metadata.filament_presets.clear();
            metadata.printer_presets.clear();

            this->prints.load_presets(bundle_dir, PRESET_PRINT_NAME, substitutions, substitution_rule, [&](Preset& preset) {
                metadata.print_presets.push_back(preset.name);
            }, PresetOrigin(PresetOrigin::Kind::LocalBundle, metadata.id), read_only);
            this->filaments.load_presets(bundle_dir, PRESET_FILAMENT_NAME, substitutions, substitution_rule, [&](Preset& preset) {
                metadata.filament_presets.push_back(preset.name);
            }, PresetOrigin(PresetOrigin::Kind::LocalBundle, metadata.id), read_only);
            this->printers.load_presets(bundle_dir, PRESET_PRINTER_NAME, substitutions, substitution_rule, [&](Preset& preset) {
                metadata.printer_presets.push_back(preset.name);
            }, PresetOrigin(PresetOrigin::Kind::LocalBundle, metadata.id), read_only);
            metadata.bundle_type = BundleType::Local;
            metadata.path = metadata_file.string();

            bundles.WriteLock();
            bundles.m_bundles[metadata.id] = metadata;
            bundles.WriteUnlock();
        }
    }

    fs::path subscribed_dir(folder / PRESET_SUBSCRIBED_DIR);
    if (fs::exists(subscribed_dir)) {
        for (auto& entry : fs::directory_iterator(subscribed_dir)) {
            if (!fs::is_directory(entry.path())) continue;

            std::string bundle_dir = entry.path().string();

            fs::path metadata_file = entry.path() / PRESET_BUNDLE_METADATA;
            if (!fs::exists(metadata_file)) continue;

            BundleMetadata metadata;
            if (!load_bundle_metadata(metadata_file, metadata)) continue;
            metadata.print_presets.clear();
            metadata.filament_presets.clear();
            metadata.printer_presets.clear();
            metadata.is_subscribed = true;

            this->prints.load_presets(bundle_dir, PRESET_PRINT_NAME, substitutions, substitution_rule, [&](Preset& preset) {
                metadata.print_presets.push_back(preset.name);
            }, PresetOrigin(PresetOrigin::Kind::SubscribedBundle, metadata.id), read_only);
            this->filaments.load_presets(bundle_dir, PRESET_FILAMENT_NAME, substitutions, substitution_rule, [&](Preset& preset) {
                metadata.filament_presets.push_back(preset.name);
            }, PresetOrigin(PresetOrigin::Kind::SubscribedBundle, metadata.id), read_only);
            this->printers.load_presets(bundle_dir, PRESET_PRINTER_NAME, substitutions, substitution_rule, [&](Preset& preset) {
                metadata.printer_presets.push_back(preset.name);
            }, PresetOrigin(PresetOrigin::Kind::SubscribedBundle, metadata.id), read_only);

            metadata.bundle_type = BundleType::Subscribed;
            metadata.path = metadata_file.string();
            metadata.update_available = false;

            bundles.WriteLock();
            bundles.m_bundles[metadata.id] = metadata;
            bundles.WriteUnlock();
        }
    }

    {
        const auto json_t0 = std::chrono::steady_clock::now();
        try {
            std::string sel = prints.get_selected_preset().name;
            this->prints.load_presets(dir_user_presets, PRESET_PRINT_NAME, substitutions, substitution_rule,
                                      nullptr, PresetOrigin(), read_only);
            prints.select_preset_by_name(sel, false);
        } catch (const std::runtime_error& err) { errors_cummulative += err.what(); }
        try {
            std::string sel = filaments.get_selected_preset().name;
            this->filaments.load_presets(dir_user_presets, PRESET_FILAMENT_NAME, substitutions, substitution_rule,
                                         nullptr, PresetOrigin(), read_only);
            filaments.select_preset_by_name(sel, false);
        } catch (const std::runtime_error& err) { errors_cummulative += err.what(); }
        try {
            std::string sel = printers.get_selected_preset().name;
            this->printers.load_presets(dir_user_presets, PRESET_PRINTER_NAME, substitutions, substitution_rule,
                                        nullptr, PresetOrigin(), read_only);
            printers.select_preset_by_name(sel, false);
        } catch (const std::runtime_error& err) { errors_cummulative += err.what(); }
        if (!errors_cummulative.empty()) throw Slic3r::RuntimeError(errors_cummulative);

        const auto json_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - json_t0).count();
        BOOST_LOG_TRIVIAL(info) << "PresetBundle: user presets loaded from JSON in " << json_ms << " ms";
    }

    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - user_load_t0).count();
        BOOST_LOG_TRIVIAL(info) << "PresetBundle: user + bundle presets loaded in " << ms << " ms";
    }

    this->update_multi_material_filament_presets();
    this->update_compatible(PresetSelectCompatibleType::Never);
    set_calibrate_printer("");

    return PresetsConfigSubstitutions();
}

PresetsConfigSubstitutions PresetBundle::load_user_presets(AppConfig &                                                config,
                                                           std::map<std::string, std::map<std::string, std::string>> &my_presets,
                                                           ForwardCompatibilitySubstitutionRule                       substitution_rule)
{
    PresetsConfigSubstitutions substitutions;
    std::string errors_cummulative;
    bool process_added = false, filament_added = false, machine_added = false;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" enter, substitution_rule %1%, preset toltal count %2%")%substitution_rule%my_presets.size();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" print's selected_idx %1%, selected_name %2%") %prints.get_selected_idx() %prints.get_selected_preset_name();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" filament's selected_idx %1%, selected_name %2%") %filaments.get_selected_idx() %filaments.get_selected_preset_name();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" printers's selected_idx %1%, selected_name %2%") %printers.get_selected_idx() %printers.get_selected_preset_name();

    remove_users_preset(config, &my_presets);

    std::map<std::string, std::map<std::string, std::string>>::iterator it;
    for (int pass = 0; pass < 2; ++pass)
    for (it = my_presets.begin(); it != my_presets.end(); it++) {
        std::string name = it->first;
        std::map<std::string, std::string>& value_map = it->second;
        std::map<std::string, std::string>::iterator inherits_iter = value_map.find(BBL_JSON_KEY_INHERITS);
        if ((pass == 1) == (inherits_iter == value_map.end() || inherits_iter->second.empty()))
            continue;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " start load from cloud: " << name;
        std::map<std::string, std::string>::iterator type_iter = value_map.find(BBL_JSON_KEY_TYPE);
        if (type_iter == value_map.end()) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(" can not find type for setting %1%")%name;
            continue;
        }
        try {
            PresetCollection *preset_collection = nullptr;
            if (type_iter->second == PRESET_IOT_PRINT_TYPE) {
                preset_collection = &(this->prints);
                process_added |= preset_collection->load_user_preset(name, value_map, substitutions, substitution_rule, PresetOrigin(PresetOrigin::Kind::User));
            }
            else if (type_iter->second == PRESET_IOT_FILAMENT_TYPE) {
                preset_collection = &(this->filaments);
                filament_added |= preset_collection->load_user_preset(name, value_map, substitutions, substitution_rule, PresetOrigin(PresetOrigin::Kind::User));
            }
            else if (type_iter->second == PRESET_IOT_PRINTER_TYPE) {
                preset_collection = &(this->printers);
                machine_added |= preset_collection->load_user_preset(name, value_map, substitutions, substitution_rule, PresetOrigin(PresetOrigin::Kind::User));
            }
            else {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("invalid type %1% for setting %2%") %type_iter->second %name;
                continue;
            }
        }
        catch (const std::runtime_error& err) {
            errors_cummulative += err.what();
        }
    }
    this->normalize_compatible_presets();

    this->update_multi_material_filament_presets();
    this->update_compatible(PresetSelectCompatibleType::Never);

    set_calibrate_printer("");

    if (! errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, process_added %1%, filament_added %2%, machine_added %3%")%process_added %filament_added %machine_added;
    return substitutions;
}

bool PresetBundle::apply_vendor_config(
    const std::map<std::string, std::map<std::string, std::set<std::string>>>& new_vendors,
    const std::map<std::string, std::string>& new_filaments,
    AppConfig* app_config,
    bool overwrite,
    const std::string& preferred_printer_model,
    const std::string& preferred_printer_variant,
    const std::string& preferred_filament)
{

    const auto old_vendors = app_config->vendors();
    const auto old_filaments = app_config->has_section(AppConfig::SECTION_FILAMENTS)
        ? app_config->get_section(AppConfig::SECTION_FILAMENTS)
        : std::map<std::string, std::string>();

    std::vector<std::string> install_bundles;
    for (const auto &it : new_vendors) {
        if (it.second.size() > 0) {
            if (!is_vendor_installed(it.first)) {
                install_bundles.emplace_back(it.first);
            }
        }
    }

    if (!install_bundles.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Installing " << install_bundles.size() << " vendor bundles from resources";
        if (!Slic3r::install_vendor_bundles_from_resources(install_bundles)) {
            BOOST_LOG_TRIVIAL(error) << "Failed to install vendor bundles";
            return false;
        }
    } else {
        BOOST_LOG_TRIVIAL(info) << "No bundles need to be installed from resource directory";
    }

    static const std::string system_suffix              = " @System";
    auto                     it_default                 = new_vendors.find(PresetBundle::ORCA_DEFAULT_BUNDLE);
    bool                     has_default_bundle_printer = it_default != new_vendors.end() && !it_default->second.empty();

    bool has_vendor_printer = false;
    for (const auto& [vendor, models] : new_vendors) {
        if (vendor != PresetBundle::ORCA_DEFAULT_BUNDLE && !models.empty()) {
            has_vendor_printer = true;
            break;
        }
    }

    std::map<std::string, std::string> supplemented_filaments;
    for (const auto& [name, value] : new_filaments) {
        if (name.size() > system_suffix.size() &&
            name.compare(name.size() - system_suffix.size(), system_suffix.size(), system_suffix) == 0) {
            std::string short_name = name.substr(0, name.size() - system_suffix.size());

            if (has_vendor_printer) {
                bool has_vendor_filament = false;
                for (const auto& [vendor, models] : new_vendors) {
                    if (vendor != PresetBundle::ORCA_DEFAULT_BUNDLE) {
                        auto vendor_it = this->vendors.find(vendor);
                        if (vendor_it != this->vendors.end()) {
                            for (auto f : vendor_it->second.default_filaments) {
                                BOOST_LOG_TRIVIAL(info) << " checking if vendor filament " << f << " matches " << short_name << "(" << name << ")";
                                if (f.find(short_name) != std::string::npos) {
                                    BOOST_LOG_TRIVIAL(info) << name << " has filament from vendor: " << vendor;
                                    has_vendor_filament = true;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }

                if (has_vendor_filament) {
                    supplemented_filaments[short_name] = value;
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Replacing @System filament: '" << name << "' -> '" << short_name << "'";
                    if (has_default_bundle_printer) {
                        supplemented_filaments[name] = value;
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Also keeping '" << name << "' for default bundle printers";
                    }
                    continue;
                }
            }
        }
        supplemented_filaments[name] = value;
    }

    if (overwrite) {
        app_config->set_section(AppConfig::SECTION_FILAMENTS, supplemented_filaments);
        app_config->set_vendors(new_vendors);
    }
    else {
        std::map<std::string, std::string> merged_filaments = old_filaments;
        for (const auto& [name, value] : supplemented_filaments) {
            merged_filaments[name] = value;
        }
        app_config->set_section(AppConfig::SECTION_FILAMENTS, merged_filaments);

        std::map<std::string, std::map<std::string, std::set<std::string>>> merged_vendors = old_vendors;
        for (const auto& [vendor, models] : new_vendors) {
            auto& vendor_entry = merged_vendors[vendor];
            for (const auto& [model, variants] : models) {
                auto& model_entry = vendor_entry[model];
                model_entry.insert(variants.begin(), variants.end());
            }
        }
        app_config->set_vendors(merged_vendors);
    }

    this->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::Enable,
        {preferred_printer_model, preferred_printer_variant, preferred_filament, std::string()});

    if (!supplemented_filaments.empty()) {
        bool active_filament_selected = supplemented_filaments.count(this->filament_presets.front()) > 0;
        if (!active_filament_selected) {
            for (const auto& [filament_name, _] : supplemented_filaments) {
                const Preset* preset = this->filaments.find_preset(filament_name);
                if (preset && preset->is_visible && preset->is_compatible) {
                    this->filaments.select_preset_by_name(filament_name, true);
                    this->filament_presets.front() = this->filaments.get_selected_preset_name();
                    break;
                }
            }
        }
    }

    this->export_selections(*app_config);
    return true;
}

PresetsConfigSubstitutions PresetBundle::import_presets(std::vector<std::string> &              files,
                                                        std::function<int(std::string const &)> override_confirm,
                                                        ForwardCompatibilitySubstitutionRule    rule,
                                                        AppConfig&                            config)
{
    bundles.PauseRead();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " entry";
    PresetsConfigSubstitutions substitutions;
    int overwrite = 0;
    std::vector<std::string>   result;
    std::string                user_id = config.get("preset_folder");
    if (user_id.empty())
        user_id = DEFAULT_USER_FOLDER_NAME;
    this->update_user_presets_directory(user_id);
    for (auto &file : files) {
        if (Slic3r::is_json_file(file)) {
            import_json_presets(substitutions, file, override_confirm, rule, overwrite, result);
        }
        if (boost::iends_with(file, ".orca_printer") || boost::iends_with(file, ".orca_bundle") || boost::iends_with(file, ".orca_filament") || boost::iends_with(file, ".zip")) {
            boost::system::error_code ec;
            fs::path user_folder(data_dir() + "/" + PRESET_USER_DIR);
            if (!fs::exists(user_folder)) fs::create_directory(user_folder, ec);
            if (ec) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " create directory failed: " << ec.message();
            fs::path configs_folder(user_folder / user_id);
            if (!fs::exists(configs_folder)) fs::create_directory(configs_folder, ec);
            if (ec) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " create directory failed: " << ec.message();
            static std::atomic<unsigned> import_counter{0};
            fs::path temp_folder(fs::path(data_dir()) / "cache" / ("import." + std::to_string(get_current_pid()) + "." + std::to_string(import_counter++)));
            std::string user_default_temp_dir = temp_folder.make_preferred().string();
            fs::create_directories(temp_folder, ec);
            if (ec) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " create directory failed: " << ec.message();

            file = boost::filesystem::path(file).make_preferred().string();
            mz_zip_archive zip_archive;
            mz_zip_zero_struct(&zip_archive);
            mz_bool status;

            FILE *zipFile = boost::nowide::fopen(file.c_str(), "rb");
            status        = mz_zip_reader_init_cfile(&zip_archive, zipFile, 0, MZ_ZIP_FLAG_CASE_SENSITIVE | MZ_ZIP_FLAG_IGNORE_PATH);
            if (MZ_FALSE == status) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Failed to initialize reader ZIP archive";
                if (zipFile != nullptr)
                    std::fclose(zipFile);
                fs::remove_all(temp_folder, ec);
                return substitutions;
            } else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Success to initialize reader ZIP archive";
            }

            bool has_bundle_structure = false;
            BundleMetadata metadata;
            fs::path metadata_path = temp_folder / BUNDLE_STRUCTURE_JSON_NAME;
            status = mz_zip_reader_extract_file_to_file(&zip_archive, BUNDLE_STRUCTURE_JSON_NAME, encode_path(metadata_path.string().c_str()).c_str(), MZ_ZIP_FLAG_CASE_SENSITIVE);
            if (status) {
                if (metadata.load_from_json(metadata_path.string())) {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Found bundle_id: " << metadata.id << " from " << BUNDLE_STRUCTURE_JSON_NAME;
                    has_bundle_structure = true;
                }
            }

            if (has_bundle_structure && metadata.id.empty()) {
                boost::uuids::uuid uuid = boost::uuids::random_generator()();
                metadata.id = to_string(uuid);
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " bundle_id was empty, so generating a UUID: " << metadata.id;
            }
            if (has_bundle_structure && !is_path_within_root(metadata.id, user_folder / user_id / PRESET_LOCAL_DIR)) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " bundle id escapes the bundle directory, not importing: " << metadata.id;
                fclose(zipFile);
                fs::remove_all(temp_folder, ec);
                continue;
            }

            fs::path bundle_base_dir;
            if (has_bundle_structure) {
                bundle_base_dir = user_folder / user_id / PRESET_LOCAL_DIR / metadata.id;
                if (!fs::exists(bundle_base_dir))
                    fs::create_directories(bundle_base_dir, ec);
                if (ec)
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to create bundle directory: " << bundle_base_dir.string() << " error: " << ec.message();
            } else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " No bundle_structure.json found, importing presets into the user preset directory";
            }

            int num_files = mz_zip_reader_get_num_files(&zip_archive);
            for (int i = 0; i < num_files; i++) {
                mz_zip_archive_file_stat file_stat;
                status = mz_zip_reader_file_stat(&zip_archive, i, &file_stat);
                if (status) {
                    std::string file_name = file_stat.m_filename;
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " From zip file: " << file << ". Read file name: " << file_stat.m_filename;
                    size_t index = file_name.find_last_of("/\\");
                    if (std::string::npos != index) {
                        file_name = file_name.substr(index + 1);
                    }
                    if (BUNDLE_STRUCTURE_JSON_NAME == file_name) continue;
                    if (!is_path_within_root(file_name, temp_folder)) {
                        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " zip entry escapes the temp directory, skipping: " << file_stat.m_filename;
                        continue;
                    }
                    std::string target_file_path = boost::filesystem::path(temp_folder / file_name).make_preferred().string();

                    status = mz_zip_reader_extract_to_file(&zip_archive, i, encode_path(target_file_path.c_str()).c_str(), MZ_ZIP_FLAG_CASE_SENSITIVE);
                    if (MZ_FALSE == status) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Failed to open target file: " << target_file_path;
                    } else {
                        bool is_success = import_json_presets(substitutions, target_file_path, override_confirm, rule, overwrite, result,
                                                              has_bundle_structure ? bundle_base_dir.string() : std::string());
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " import target file: " << target_file_path << " import result" << is_success;
                    }
                }
            }

            if (metadata.imported_time == 0) {
                metadata.imported_time = std::time(nullptr);
            }

            if (has_bundle_structure) {
                fs::path metadata_save_path = bundle_base_dir / PRESET_BUNDLE_METADATA;
                if (metadata.save_to_json(metadata_save_path.string())) {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Saved bundle metadata to: " << metadata_save_path.string();

                    metadata.bundle_type = BundleType::Local;
                    metadata.path = metadata_save_path.string();
                    

                    bundles.WriteLock();
                    bundles.m_bundles[metadata.id] = metadata;
                    bundles.WriteUnlock();
                } else {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to save bundle metadata to: " << metadata_save_path.string();
                }
            }

            fclose(zipFile);
            if (fs::exists(temp_folder)) fs::remove_all(temp_folder, ec);
            if (ec) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " remove directory failed: " << ec.message();
        }
    }
    bundles.UnpauseRead();
    files = result;
    return substitutions;
}

bool PresetBundle::import_json_presets(PresetsConfigSubstitutions &            substitutions,
                                       std::string &                           file,
                                       std::function<int(std::string const &)> override_confirm,
                                       ForwardCompatibilitySubstitutionRule    rule,
                                       int &                                   overwrite,
                                       std::vector<std::string> &              result,
                                       const std::string &                     bundle_dir)
{
    try {
        DynamicPrintConfig config;
        std::map<std::string, std::string> key_values;
        std::string                        reason;
        ConfigSubstitutions                config_substitutions = config.load_from_json(file, rule, key_values, reason);
        std::string                        name                 = key_values[BBL_JSON_KEY_NAME];
        std::string                        version_str          = key_values[BBL_JSON_KEY_VERSION];
        boost::optional<Semver>            version              = Semver::parse(version_str);
        if (!version) return false;

        std::string type_subdir;
        PresetCollection *collection = nullptr;
        if (config.has("printer_settings_id")) {
            collection = &printers;
            type_subdir = PRESET_PRINTER_NAME;
        }
        else if (config.has("print_settings_id")) {
            collection = &prints;
            type_subdir = PRESET_PRINT_NAME;
        }
        else if (config.has("filament_settings_id")) {
            collection = &filaments;
            type_subdir = PRESET_FILAMENT_NAME;
        }
        if (collection == nullptr) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Preset type is unknown, not loading: " << name;
            return false;
        }
        if (!is_path_within_root(name, fs::path(collection->m_dir_path))) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Preset name escapes the preset directory, not loading: " << name;
            return false;
        }
        const PresetOrigin load_origin = detect_origin_from_path(boost::filesystem::path(bundle_dir));
        const std::string      preset_name = get_preset_canonical_name(name, load_origin);

        if (overwrite == 0) overwrite = 1;
        if (auto p = collection->find_preset(preset_name, false)) {
            if (p->is_default || p->is_system) {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Preset already present and is system preset, not loading: " << preset_name;
                return false;
            }
            if (overwrite != 2 && overwrite != 3) overwrite = override_confirm(preset_name);
        }
        if (overwrite == 0 || overwrite == 2) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Preset already present, not loading: " << preset_name;
            return false;
        }

        DynamicPrintConfig new_config;
        Preset *           inherit_preset  = nullptr;
        ConfigOption *     inherits_config = config.option(BBL_JSON_KEY_INHERITS);
        std::string        inherits_value;
        if (inherits_config) {
            ConfigOptionString *option_str = dynamic_cast<ConfigOptionString *>(inherits_config);
            inherits_value                 = option_str->value;
            inherit_preset                 = collection->find_preset2(inherits_value, true);
            Preset::normalize_inherits(config, inherit_preset);
            if (inherit_preset)
                inherits_value = inherit_preset->name;
        }
        if (inherit_preset) {
            new_config = inherit_preset->config;
            new_config.apply(std::move(config));
        } else {
            auto inherits_config2 = dynamic_cast<ConfigOptionString *>(inherits_config);
            if (inherits_config2 && !inherits_config2->value.empty()) {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", can not find inherit preset for user preset %1%, just skip") % name;
                return false;
            }
            const Preset &default_preset = collection->default_preset_for(config);
            new_config                   = default_preset.config;
            new_config.apply(std::move(config));
            extend_default_config_length(new_config, true, default_preset.config);
        }

        Preset &preset     = collection->load_preset(collection->path_from_name(name, inherit_preset == nullptr), preset_name, std::move(new_config), false);
        preset.bundle_id = load_origin.bundle_id;
        if (key_values.find(BBL_JSON_KEY_FILAMENT_ID) != key_values.end())
            preset.filament_id = key_values[BBL_JSON_KEY_FILAMENT_ID];
        preset.is_external = true;
        preset.version     = *version;
        inherit_preset     = collection->find_preset(inherits_value, false, true);
        if (inherit_preset) preset.base_id = inherit_preset->setting_id;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << preset.name << " have filament_id: " << preset.filament_id << " and base_id: " << preset.base_id;
        Preset::normalize(preset.config);
        const Preset &default_preset = collection->default_preset_for(new_config);
        std::string   incorrect_keys = Preset::remove_invalid_keys(preset.config, default_preset.config);
        if (!incorrect_keys.empty()) {
            ++m_errors;
            BOOST_LOG_TRIVIAL(error) << "Error in a preset file: The preset \"" << preset.file
                                     << "\" contains the following incorrect keys: " << incorrect_keys << ", which were removed";
        }
        if (!config_substitutions.empty())
            substitutions.push_back({name, collection->type(), PresetConfigSubstitutions::Source::UserFile, file, std::move(config_substitutions)});
        collection->set_custom_preset_alias(preset);

        if (!bundle_dir.empty()) {
            if (!save_preset_to_bundle_dir(preset, collection, load_origin.bundle_id, type_subdir, bundle_dir)) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to save preset " << preset_name << " to bundle directory";
                return false;
            }
        } else {
            preset.save(inherit_preset ? &inherit_preset->config : nullptr);
        }

        result.push_back(file);
    } catch (const std::ifstream::failure &err) {
        ++m_errors;
        BOOST_LOG_TRIVIAL(error) << boost::format("The config cannot be loaded: %1%. Reason: %2%") % file % err.what();
    } catch (const std::runtime_error &err) {
        ++m_errors;
        BOOST_LOG_TRIVIAL(error) << boost::format("Failed importing config file: %1%. Reason: %2%") % file % err.what();
    }
    return true;
}

void PresetBundle::save_user_presets(AppConfig& config, std::map<std::string, std::string>& need_to_delete_list)
{
    std::string user_sub_folder = DEFAULT_USER_FOLDER_NAME;
    if (!config.get("preset_folder").empty())
        user_sub_folder = config.get("preset_folder");
    const std::string dir_user_presets = data_dir() + "/" + PRESET_USER_DIR + "/"+ user_sub_folder;

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, save to %1%")%dir_user_presets;

    fs::path user_folder(data_dir() + "/" + PRESET_USER_DIR);
    if (!fs::exists(user_folder))
        fs::create_directory(user_folder);

    fs::path folder(dir_user_presets);
    if (!fs::exists(folder))
        fs::create_directory(folder);

    this->prints.save_user_presets(dir_user_presets, PRESET_PRINT_NAME, need_to_delete_list);
    this->filaments.save_user_presets(dir_user_presets, PRESET_FILAMENT_NAME, need_to_delete_list);
    this->printers.save_user_presets(dir_user_presets, PRESET_PRINTER_NAME, need_to_delete_list);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished");
}

void PresetBundle::check_and_fix_user_presets_syncinfo(const std::string& user_id)
{
    auto process_collection = [&user_id](PresetCollection& collection) {
        collection.lock();
        for (auto& preset : collection) {
            if (preset.is_user()) {
                collection.check_and_fix_syncinfo(preset, user_id);
            }
        }
        collection.unlock();
    };
    process_collection(this->prints);
    process_collection(this->filaments);
    process_collection(this->printers);
}

PresetsConfigSubstitutions PresetBundle::update_subscribed_presets(
    AppConfig& config,
    const std::map<std::string, std::map<std::string, std::string>>& bundle_presets,
    const BundleMetadata& remote_metadata,
    ForwardCompatibilitySubstitutionRule substitution_rule)
{
    PresetsConfigSubstitutions substitutions;
    std::string errors_cumulative;
    bool process_added = false, filament_added = false, machine_added = false;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " enter, substitution_rule " << substitution_rule << ", bundle_id: " << remote_metadata.id << ", preset count: " << bundle_presets.size();

    BundleMetadata merged_metadata;
    auto existing_it = bundles.m_bundles.find(remote_metadata.id);
    if (existing_it != bundles.m_bundles.end()) {
        merged_metadata = existing_it->second;
    } else {
        merged_metadata.imported_time = std::time(nullptr);
    }

    merged_metadata.id = remote_metadata.id;
    merged_metadata.name = remote_metadata.name;
    merged_metadata.version = remote_metadata.version;
    merged_metadata.description = remote_metadata.description;
    merged_metadata.author = remote_metadata.author;
    merged_metadata.updated_time = remote_metadata.updated_time;
    merged_metadata.bundle_type = BundleType::Subscribed;
    merged_metadata.is_subscribed = true;
    merged_metadata.update_available = false;
    merged_metadata.unauthorized = false;

    const PresetOrigin subscribed_origin(PresetOrigin::Kind::SubscribedBundle, remote_metadata.id);

    std::unordered_set<std::string> remote_prints;
    std::unordered_set<std::string> remote_filaments;
    std::unordered_set<std::string> remote_printers;

    for (const auto& [preset_name, value_map] : bundle_presets) {
        auto type_iter = value_map.find(BBL_JSON_KEY_TYPE);
        if (type_iter == value_map.end()) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " cannot find type for preset " << preset_name;
            continue;
        }

        const std::string subscribed_name = get_preset_canonical_name(preset_name, subscribed_origin);
        if (type_iter->second == PRESET_IOT_PRINT_TYPE)
            remote_prints.insert(subscribed_name);
        else if (type_iter->second == PRESET_IOT_FILAMENT_TYPE)
            remote_filaments.insert(subscribed_name);
        else if (type_iter->second == PRESET_IOT_PRINTER_TYPE)
            remote_printers.insert(subscribed_name);
    }

    auto remove_obsolete_bundle_presets =
        [&](PresetCollection& collection, const std::unordered_set<std::string>& remote_names, const char* type_name) -> int {
        int removed_count = 0;
        std::vector<std::string> to_delete;

        for (const Preset& preset : collection.get_presets()) {
            if (!preset.is_from_bundle() || preset.bundle_id != remote_metadata.id)
                continue;

            if (remote_names.find(preset.name) == remote_names.end())
                to_delete.push_back(preset.name);
        }

        for (const std::string& preset_name : to_delete) {
            if (collection.delete_preset(preset_name, true)) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": " << type_name << " preset '" << preset_name << "' no longer in remote bundle, deleted";
                ++removed_count;
            } else {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": failed to delete obsolete " << type_name << " preset '" << preset_name << "'";
            }
        }

        return removed_count;
    };

    int total_removed = 0;
    total_removed += remove_obsolete_bundle_presets(this->prints, remote_prints, "print");
    total_removed += remove_obsolete_bundle_presets(this->filaments, remote_filaments, "filament");
    total_removed += remove_obsolete_bundle_presets(this->printers, remote_printers, "printer");

    if (total_removed > 0) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": deleted " << total_removed << " obsolete presets from bundle " << remote_metadata.id;
    }

    std::string user_id = config.get("preset_folder");
    if (user_id.empty()) user_id = DEFAULT_USER_FOLDER_NAME;

    boost::filesystem::path user_folder(Slic3r::data_dir() + "/" + PRESET_USER_DIR);
    boost::filesystem::path subscribed_base(user_folder / user_id / PRESET_SUBSCRIBED_DIR);

    boost::system::error_code ec;
    if (!boost::filesystem::exists(subscribed_base))
        boost::filesystem::create_directories(subscribed_base, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " failed to create subscribed directory: " << subscribed_base.string() << " error: " << ec.message();
        return substitutions;
    }

    dir_user_presets_subscribed = subscribed_base;

    boost::filesystem::path bundle_dir(subscribed_base / remote_metadata.id);
    if (!boost::filesystem::exists(bundle_dir))
        boost::filesystem::create_directories(bundle_dir, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " failed to create bundle directory: " << bundle_dir.string() << " error: " << ec.message();
        return substitutions;
    }

    merged_metadata.print_presets.clear();
    merged_metadata.filament_presets.clear();
    merged_metadata.printer_presets.clear();

    for (const auto& preset_entry : bundle_presets) {
        const std::string& preset_name = preset_entry.first;
        const std::string subscribed_name = get_preset_canonical_name(preset_name, subscribed_origin);
        std::map<std::string, std::string> value_map = preset_entry.second;

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " importing preset: " << preset_name << " from bundle: " << remote_metadata.id;

        auto type_iter = value_map.find(BBL_JSON_KEY_TYPE);
        if (type_iter == value_map.end()) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " cannot find type for preset " << preset_name;
            continue;
        }

        auto inherits_iter = value_map.find(BBL_JSON_KEY_INHERITS);
        if (inherits_iter != value_map.end() && !inherits_iter->second.empty() && bundle_presets.find(inherits_iter->second) != bundle_presets.end())
            inherits_iter->second = get_preset_canonical_name(inherits_iter->second, subscribed_origin);

        try {
            PresetCollection* preset_collection = nullptr;
            std::string type_subdir;
            bool preset_added = false;

            if (type_iter->second == PRESET_IOT_PRINT_TYPE) {
                preset_collection = &(this->prints);
                type_subdir = PRESET_PRINT_NAME;
                preset_added = preset_collection->load_user_preset(preset_name, value_map, substitutions, substitution_rule, subscribed_origin);
                process_added |= preset_added;
            }
            else if (type_iter->second == PRESET_IOT_FILAMENT_TYPE) {
                preset_collection = &(this->filaments);
                type_subdir = PRESET_FILAMENT_NAME;
                preset_added = preset_collection->load_user_preset(preset_name, value_map, substitutions, substitution_rule, subscribed_origin);
                filament_added |= preset_added;
            }
            else if (type_iter->second == PRESET_IOT_PRINTER_TYPE) {
                preset_collection = &(this->printers);
                type_subdir = PRESET_PRINTER_NAME;
                preset_added = preset_collection->load_user_preset(preset_name, value_map, substitutions, substitution_rule, subscribed_origin);
                machine_added |= preset_added;
            }
            else {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " invalid type " << type_iter->second << " for preset " << preset_name;
                continue;
            }

            Preset* preset = preset_collection->find_preset(subscribed_name, false, true);
            if (preset) {
                if (!save_preset_to_bundle_dir(*preset, preset_collection, remote_metadata.id, type_subdir, bundle_dir.string())) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " failed to save preset " << preset_name << " to bundle directory";
                    continue;
                }

                if (type_iter->second == PRESET_IOT_PRINT_TYPE)
                    merged_metadata.print_presets.push_back(preset->name);
                else if (type_iter->second == PRESET_IOT_FILAMENT_TYPE)
                    merged_metadata.filament_presets.push_back(preset->name);
                else if (type_iter->second == PRESET_IOT_PRINTER_TYPE)
                    merged_metadata.printer_presets.push_back(preset->name);
            }
        }
        catch (const std::runtime_error& err) {
            errors_cumulative += err.what();
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " error importing preset " << preset_name << ": " << err.what();
        }
    }

    boost::filesystem::path metadata_save_path = bundle_dir / PRESET_BUNDLE_METADATA;
    merged_metadata.path = metadata_save_path.string();
    bundles.m_bundles[remote_metadata.id] = merged_metadata;

    if (bundles.m_bundles[remote_metadata.id].save_to_json(metadata_save_path.string())) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " saved bundle metadata to: " << metadata_save_path.string();
    } else {                                                                                                                                                                                                                                                              
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " failed to save bundle metadata to: " << metadata_save_path.string();
    }

    this->normalize_compatible_presets();

    this->update_multi_material_filament_presets();
    this->update_compatible(PresetSelectCompatibleType::Never);

    set_calibrate_printer("");

    if (!errors_cumulative.empty())
        throw Slic3r::RuntimeError(errors_cumulative);

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << " finished, process_added " << process_added << ", filament_added " << filament_added << ", machine_added " << machine_added;
    return substitutions;
}

bool PresetBundle::save_preset_to_bundle_dir(Preset& preset, PresetCollection* collection,
                                              const std::string& bundle_id, const std::string& type_subdir,
                                              const std::string& bundle_base_dir)
{
    if (bundle_base_dir.empty()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " bundle_base_dir is empty, cannot save preset " << preset.name;
        return false;
    }

    std::string original_dir_path = collection->m_dir_path;

    try {
        boost::filesystem::path bundle_dir(bundle_base_dir);
        boost::system::error_code ec;
        if (!boost::filesystem::exists(bundle_dir))
            boost::filesystem::create_directories(bundle_dir, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " failed to create bundle directory: " << bundle_dir.string() << " error: " << ec.message();
            return false;
        }

        boost::filesystem::path type_dir = bundle_dir / type_subdir;
        if (!boost::filesystem::exists(type_dir)) {
            boost::filesystem::create_directories(type_dir, ec);
            if (ec) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " failed to create type directory: " << type_dir.string() << " error: " << ec.message();
                return false;
            }
        }

        preset.bundle_id = bundle_id;

        const std::string preset_filename = boost::filesystem::path(preset.name).filename().string();
        const std::string file_name = boost::iends_with(preset_filename, ".json") ? preset_filename : (preset_filename + ".json");
        preset.file = (type_dir / file_name).make_preferred().string();

        std::string inherits = Preset::inherits(preset.config);
        if (inherits.empty()) {
            preset.save(nullptr);
        } else {
            Preset* parent_preset = collection->find_preset2(inherits, true);
            if (!parent_preset) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " cannot find parent preset for " << preset.name << ", inherits " << inherits;
            } else {
                Preset::normalize_inherits(preset.config, parent_preset);
                if (preset.base_id.empty())
                    preset.base_id = parent_preset->setting_id;
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " saved preset " << preset.name
                                        << " filament_id: " << preset.filament_id
                                        << " base_id: " << preset.base_id
                                        << " bundle: " << bundle_id;
                preset.save(&(parent_preset->config));
            }
        }

        collection->m_dir_path = original_dir_path;
        return true;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " exception saving preset " << preset.name << ": " << e.what();
        collection->m_dir_path = original_dir_path;
        return false;
    }
}

void PresetBundle::update_user_presets_directory(const std::string preset_folder)
{
    const std::string dir_user_presets = data_dir() + "/" + PRESET_USER_DIR + "/"+ preset_folder;

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, update directory to %1%")%dir_user_presets;

    fs::path user_folder(data_dir() + "/" + PRESET_USER_DIR);
    if (!fs::exists(user_folder))
        fs::create_directory(user_folder);

    fs::path folder(dir_user_presets);
    if (!fs::exists(folder))
        fs::create_directory(folder);

    this->prints.update_user_presets_directory(dir_user_presets, PRESET_PRINT_NAME);
    this->filaments.update_user_presets_directory(dir_user_presets, PRESET_FILAMENT_NAME);
    this->printers.update_user_presets_directory(dir_user_presets, PRESET_PRINTER_NAME);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished");
}

void PresetBundle::remove_user_presets_directory(const std::string preset_folder)
{
    const std::string dir_user_presets = data_dir() + "/" + PRESET_USER_DIR + "/" + preset_folder;

    if (preset_folder.empty()) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": preset_folder is empty, no need to remove directory : %1%") % dir_user_presets;
        return;
    }
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, delete directory : %1%") % dir_user_presets;
    boost::system::error_code ec;
    InstanceLock instance_lock(user_presets_lock_path());
    fs::remove_all(fs::path(dir_user_presets), ec);
}

void PresetBundle::update_system_preset_setting_ids(std::map<std::string, std::map<std::string, std::string>>& system_presets)
{
    for (auto iterator: system_presets)
    {
        std::string name = iterator.first;
        std::map<std::string, std::string>& value_map = iterator.second;
        std::map<std::string, std::string>::iterator type_iter = value_map.find(BBL_JSON_KEY_TYPE);
        if (type_iter == value_map.end()) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(" can not find type for setting %1%")%name;
            continue;
        }
        PresetCollection *preset_collection = nullptr;
        if (type_iter->second == PRESET_IOT_PRINTER_TYPE) {
            preset_collection = &(this->printers);
        }
        else if (type_iter->second == PRESET_IOT_PRINTER_TYPE) {
            preset_collection = &(this->printers);
        }
        else if (type_iter->second == PRESET_IOT_PRINTER_TYPE) {
            preset_collection = &(this->printers);
        }
        else {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("invalid type %1% for setting %2%") %type_iter->second %name;
            continue;
        }
        std::string setting_id;
        if (value_map.count(BBL_JSON_KEY_SETTING_ID) > 0)
            setting_id = value_map[BBL_JSON_KEY_SETTING_ID];
        else {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(" can not find setting_id for setting %1%")%name;
            continue;
        }
        Preset* preset = preset_collection->find_preset(name, false, true);
        if (preset) {
            if (!preset->setting_id.empty() && (preset->setting_id.compare(setting_id) != 0)) {
                ++m_errors;
                BOOST_LOG_TRIVIAL(error) << boost::format("name %1%, local setting_id %2% is different with remote id %3%")
                    %preset->name %preset->setting_id %setting_id;
            }
            else if (preset->setting_id.empty())
                preset->setting_id = setting_id;
        }
        else {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("can not find setting %1% in system presets, type %2%") %name %type_iter->second;
            continue;
        }
    }
    return;
}

static std::set<std::string> gcodes_key_set =  {"filament_end_gcode", "filament_start_gcode", "change_filament_gcode", "layer_change_gcode", "machine_end_gcode", "machine_pause_gcode", "machine_start_gcode",
            "template_custom_gcode", "printing_by_object_gcode", "before_layer_change_gcode", "time_lapse_gcode", "wrapping_detection_gcode"};
int PresetBundle::validate_presets(const std::string &file_name, DynamicPrintConfig& config, std::set<std::string>& different_gcodes)
{
    bool    validated = false;
    std::vector<std::string> inherits_values                        = config.option<ConfigOptionStrings>("inherits_group", true)->values;
    std::vector<std::string> filament_preset_name                   = config.option<ConfigOptionStrings>("filament_settings_id", true)->values;
    std::string printer_preset                                      = config.option<ConfigOptionString>("printer_settings_id", true)->value;
    bool has_different_settings_to_system                           = config.option("different_settings_to_system")?true:false;
    std::vector<std::string> different_values;
    int     ret = VALIDATE_PRESETS_SUCCESS;

    if (has_different_settings_to_system)
        different_values = config.option<ConfigOptionStrings>("different_settings_to_system", true)->values;

    size_t filament_count = config.option<ConfigOptionFloats>("filament_diameter")->values.size();
    inherits_values.resize(filament_count + 2, std::string());
    different_values.resize(filament_count + 2, std::string());
    filament_preset_name.resize(filament_count, std::string());

    std::string printer_inherits = inherits_values[filament_count + 1];

    validated = this->printers.validate_preset(printer_preset, printer_inherits);
    if (!validated) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":file_name %1%, found the printer preset not inherit from system") % file_name;
        different_gcodes.emplace(printer_preset);
        ret = VALIDATE_PRESETS_PRINTER_NOT_FOUND;
    }
    for(unsigned int index = 0; index < filament_count; index ++)
    {
        std::string filament_preset = filament_preset_name[index];
        std::string filament_inherits = inherits_values[index+1];

        if (filament_preset.empty())
            continue;

        validated = this->filaments.validate_preset(filament_preset, filament_inherits);
        if (!validated) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":file_name %1%, found the filament %2% preset not inherit from system") % file_name %(index+1);
            different_gcodes.emplace(filament_preset);
            ret = VALIDATE_PRESETS_FILAMENTS_NOT_FOUND;
        }
    }

    if (ret)
    {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":file_name %1%, found self defined presets, count %2%") %file_name %different_gcodes.size();
        return ret;
    }

    for(unsigned int index = 1; index < filament_count; index ++)
    {
        std::string different_settingss = different_values[index];

        std::vector<std::string> different_keys;

        Slic3r::unescape_strings_cstyle(different_settingss, different_keys);

        for (unsigned int j = 0; j < different_keys.size(); j++) {
            if (gcodes_key_set.find(different_keys[j]) != gcodes_key_set.end()) {
                different_gcodes.emplace(different_keys[j]);
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":preset index %1%, different key %2%") %index %different_keys[j];
            }
        }
    }

    if (!different_gcodes.empty())
    {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":file_name %1%, found different gcodes count %2%") %file_name %different_gcodes.size();
        return VALIDATE_PRESETS_MODIFIED_GCODES;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":file_name %1%, validate presets success!") % file_name;

    return VALIDATE_PRESETS_SUCCESS;
}

void PresetBundle::remove_users_preset(AppConfig &config, std::map<std::string, std::map<std::string, std::string>> *my_presets)
{
    auto check_removed = [my_presets](Preset &preset) -> bool {
        if (my_presets == nullptr) return true;
        if (my_presets->find(preset.name) != my_presets->end()) return false;
        if (!preset.sync_info.empty()) return false;
        if (preset.setting_id.empty()) return false;
        if (preset.is_dirty) {
            preset.setting_id.clear();
            return false;
        }
        preset.remove_files(true );
        return true;
    };
    std::string preset_folder_user_id = config.get("preset_folder");
    std::string printer_selected_preset_name = printers.get_selected_preset().name;
    bool need_reset_printer_preset = false;
    for (auto it = printers.begin(); it != printers.end();) {
        if (it->is_user() && it->user_id.compare(preset_folder_user_id) == 0 && check_removed(*it)) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":printers erase %1%, type %2%， user_id %3%") % it->name % Preset::get_type_string(it->type) % it->user_id;
            if (it->name == printer_selected_preset_name)
                need_reset_printer_preset = true;
            it = printers.erase(it);
        }
        else {
            it++;
        }
    }

    if (need_reset_printer_preset) {
        std::string default_printer_model = ORCA_DEFAULT_PRINTER_MODEL;
        std::string default_printer_name;
        for (auto it = printers.begin(); it != printers.end(); it++) {
            if (it->config.has("printer_model")) {
                if (it->config.opt_string("printer_model") == default_printer_model) {
                    default_printer_name = it->name;
                    break;
                }
            }
        }
        printers.select_preset_by_name(default_printer_name, true);
    } else {
        printers.select_preset_by_name(printer_selected_preset_name, false);
    }

    std::string selected_print_name = prints.get_selected_preset().name;
    bool need_reset_print_preset = false;
    for (auto it = prints.begin(); it != prints.end();) {
        if (it->is_user() && it->user_id.compare(preset_folder_user_id) == 0 && check_removed(*it)) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":prints erase %1%, type %2%， user_id %3%")%it->name %Preset::get_type_string(it->type) %it->user_id;
            if (it->name == selected_print_name)
                need_reset_print_preset = true;
            it = prints.erase(it);
        }
        else {
            it++;
        }
    }
    if (need_reset_print_preset && printers.get_selected_preset().config.has("default_print_profile")) {
        std::string default_print_profile_name = printers.get_selected_preset().config.opt_string("default_print_profile");
        prints.select_preset_by_name(default_print_profile_name, true);
    } else {
        prints.select_preset_by_name(selected_print_name, false);
    }

    std::string selected_filament_name = filaments.get_selected_preset().name;
    bool need_reset_filament_preset = false;
    for (auto it = filaments.begin(); it != filaments.end();) {
        if (it->is_user() && it->user_id.compare(preset_folder_user_id) == 0 && check_removed(*it)) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":filaments erase %1%, type %2%， user_id %3%")%it->name %Preset::get_type_string(it->type) %it->user_id;
            if (it->name == selected_filament_name)
                need_reset_filament_preset = true;
            it = filaments.erase(it);
        }
        else {
            it++;
        }
    }
    if (need_reset_filament_preset && printers.get_selected_preset().config.has("default_filament_profile")) {
        const std::vector<std::string>& prefered_filament_profiles = printers.get_selected_preset().config.option<ConfigOptionStrings>("default_filament_profile")->values;
        if (prefered_filament_profiles.size() > 0)
            filaments.select_preset_by_name(prefered_filament_profiles[0], true);
    } else {
        filaments.select_preset_by_name(selected_filament_name, false);
    }

    update_compatible(PresetSelectCompatibleType::Always);

    for (size_t i = 0; i < filament_presets.size(); ++i)
    {
        auto preset = this->filaments.find_preset(filament_presets[i]);
        if (preset == nullptr)
            filament_presets[i] = filaments.get_selected_preset_name();
    }
}

void PresetBundle::clear_printer_hold_aliases()
{
    this->prints.m_printer_hold_alias.clear();
    this->sla_prints.m_printer_hold_alias.clear();
    this->filaments.m_printer_hold_alias.clear();
    this->sla_materials.m_printer_hold_alias.clear();
    this->printers.m_printer_hold_alias.clear();
}

std::pair<PresetsConfigSubstitutions, std::string> PresetBundle::load_system_presets_from_json(
    ForwardCompatibilitySubstitutionRule compatibility_rule, bool write_caches)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, compatibility_rule %1%")%compatibility_rule;
    if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSystemSilent)
        compatibility_rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
    else if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem)
        compatibility_rule = ForwardCompatibilitySubstitutionRule::Disable;

    boost::filesystem::path     dir = (boost::filesystem::path(data_dir()) / PRESET_SYSTEM_DIR).make_preferred();
    if (validation_mode)
        dir = (boost::filesystem::path(data_dir())).make_preferred();

    m_generate_vendor_caches = write_caches && (m_generate_vendor_caches || !validation_mode);

    std::vector<VendorSource> vendors;
    for (const std::string& name : vendor_names_in(dir))
        if (name == ORCA_FILAMENT_LIBRARY || !(validation_mode && !vendor_to_validate.empty() && name != vendor_to_validate))
            vendors.push_back({ name, dir });
    auto result = this->load_vendors(vendors, compatibility_rule, true);

	this->update_system_maps();

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, errors_cummulative %1%")%result.second;
    return result;
}

std::pair<PresetsConfigSubstitutions, std::string> PresetBundle::load_vendors(const std::vector<VendorSource>& vendors,
    ForwardCompatibilitySubstitutionRule compatibility_rule, bool allow_cache, const std::atomic<bool>* cancel,
    std::vector<std::string>* failed)
{
    const auto load_t0  = std::chrono::steady_clock::now();
    auto       canceled = [cancel] { return cancel != nullptr && cancel->load(); };

    PresetsConfigSubstitutions  substitutions;
    std::string                 errors_cummulative;
    bool first = true;
    const VendorSource*              orca_lib = nullptr;
    std::vector<const VendorSource*> other_vendors;
    other_vendors.reserve(vendors.size());
    for (const VendorSource& vendor : vendors) {
        if (vendor.name == ORCA_FILAMENT_LIBRARY)
            orca_lib = &vendor;
        else
            other_vendors.push_back(&vendor);
    }

    struct VendorLoad
    {
        const VendorSource*           source { nullptr };
        std::unique_ptr<PresetBundle> bundle;
        VendorRead                    read;
        PresetsConfigSubstitutions    substitutions;
        std::string                   error;
        std::atomic<int>              ready { 0 };
    };
    std::vector<VendorLoad> loads(other_vendors.size());
    for (size_t i = 0; i < other_vendors.size(); ++i)
        loads[i].source = other_vendors[i];

    std::vector<size_t> by_cost(loads.size());
    std::iota(by_cost.begin(), by_cost.end(), size_t(0));
    const bool reads_caches = allow_cache && ! validation_mode;
    std::vector<std::pair<bool, uintmax_t>> vendor_costs(loads.size());
    for (size_t i = 0; i < loads.size(); ++i) {
        const VendorSource&           vendor  = *loads[i].source;
        const boost::filesystem::path cache   = vendor.dir / (vendor.name + ".opc");
        const boost::filesystem::path profile = vendor.dir / (vendor.name + ".json");
        boost::system::error_code ec, profile_ec;
        const uintmax_t cache_size = reads_caches ? boost::filesystem::file_size(cache, ec) : 0;
        if (reads_caches && ! ec) {
            const std::time_t profile_time = boost::filesystem::last_write_time(profile, profile_ec);
            if (profile_ec || profile_time <= boost::filesystem::last_write_time(cache, ec)) {
                vendor_costs[i] = { false, cache_size };
                continue;
            }
        }
        const uintmax_t profile_size = boost::filesystem::file_size(profile, profile_ec);
        vendor_costs[i] = { true, profile_ec ? 0 : profile_size };
    }
    std::stable_sort(by_cost.begin(), by_cost.end(),
        [&](size_t a, size_t b) { return vendor_costs[a] > vendor_costs[b]; });

    auto install = [&](VendorLoad& load) {
        if (load.bundle && ! canceled()) {
            try {
                load.substitutions = load.bundle->install_vendor_read(std::move(load.read), this).first;
            } catch (const std::runtime_error &err) {
                load.error = err.what();
                load.bundle.reset();
            }
        } else
            load.bundle.reset();
        load.read = VendorRead();
    };
    tbb::task_group group;
    group.run([&] {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, loads.size(), 1),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t k = range.begin(); k < range.end(); ++k) {
                    VendorLoad& load = loads[by_cost[k]];
                    if (! canceled()) {
                        try {
                            auto bundle = std::make_unique<PresetBundle>();
                            bundle->set_is_validation_mode(validation_mode);
                            bundle->set_generate_vendor_caches(m_generate_vendor_caches);
                            load.read   = bundle->read_vendor(load.source->dir.string(), load.source->name,
                                                              PresetBundle::LoadSystem, compatibility_rule, allow_cache);
                            load.bundle = std::move(bundle);
                        } catch (const std::runtime_error &err) {
                            load.error = err.what();
                        }
                    }
                    if (++ load.ready == 2)
                        install(load);
                }
            }, tbb::simple_partitioner());
    });
    if (orca_lib != nullptr && ! canceled()) {
        try {
            this->clear_printer_hold_aliases();
            this->m_errors = 0;
            append(substitutions, this->load_vendor_configs_from_json(
                orca_lib->dir.string(), orca_lib->name, PresetBundle::LoadSystem, compatibility_rule, nullptr, allow_cache).first);
            first = false;
        } catch (const std::runtime_error &err) {
            if (validation_mode)
                throw err;
            errors_cummulative += err.what();
            errors_cummulative += "\n";
            if (failed != nullptr)
                failed->push_back(orca_lib->name);
        }
    }
    for (size_t k : by_cost)
        if (++ loads[k].ready == 2)
            group.run([&install, &load = loads[k]] { install(load); });
    group.wait();

    std::vector<PresetBundle*> bundles;
    for (VendorLoad& load : loads)
        if (load.bundle)
            bundles.push_back(load.bundle.get());
    const std::vector<std::vector<std::string>> duplicates = this->merge_presets(bundles);
    size_t merged = 0;
    for (VendorLoad& load : loads) {
        if (! load.error.empty()) {
            if (validation_mode)
                throw std::runtime_error(load.error);
            errors_cummulative += load.error;
            errors_cummulative += "\n";
            if (failed != nullptr)
                failed->push_back(load.source->name);
            continue;
        }
        if (! load.bundle)
            continue;

        const std::string&              vendor_name       = load.source->name;
        const std::vector<std::string>& vendor_duplicates = duplicates[merged ++];
        append(substitutions, std::move(load.substitutions));
        first = false;
        if (!vendor_duplicates.empty()) {
            errors_cummulative += "Found duplicated settings in vendor " + vendor_name + "'s json file lists: ";
            for (size_t k = 0; k < vendor_duplicates.size(); ++k) {
                if (k > 0)
                    errors_cummulative += ", ";
                errors_cummulative += vendor_duplicates[k];
                ++m_errors;
                BOOST_LOG_TRIVIAL(error) << "Found duplicated preset: " + vendor_duplicates[k] + " in vendor: " + vendor_name + ": ";
            }
        }
    }

    if (first) {
		this->reset(false);
	}

    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - load_t0).count();
    BOOST_LOG_TRIVIAL(info) << "PresetBundle: " << vendors.size() << " vendor(s) loaded in " << load_ms << " ms";
    return std::make_pair(std::move(substitutions), errors_cummulative);
}

std::pair<PresetsConfigSubstitutions, std::string> PresetBundle::load_system_models_from_json(ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, compatibility_rule %1%") % compatibility_rule;
    if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSystemSilent)
        compatibility_rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
    else if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem)
        compatibility_rule = ForwardCompatibilitySubstitutionRule::Disable;

    boost::filesystem::path    dir = (boost::filesystem::path(resources_dir()) / "profiles").make_preferred();
    PresetsConfigSubstitutions substitutions;
    std::string                errors_cummulative;
    for (const std::string &vendor_name : vendor_names_in(dir)) {
        try {
            append(substitutions, load_vendor_configs_from_json(dir.string(), vendor_name, PresetBundle::LoadVendorOnly, compatibility_rule).first);
        } catch (const std::runtime_error &err) {
            errors_cummulative += err.what();
            errors_cummulative += "\n";
        }
    }

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, errors_cummulative %1%") % errors_cummulative;
    return std::make_pair(std::move(substitutions), errors_cummulative);
}

std::pair<PresetsConfigSubstitutions, std::string> PresetBundle::load_system_filaments_json(ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, compatibility_rule %1%") % compatibility_rule;
    if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSystemSilent)
        compatibility_rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
    else if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem)
        compatibility_rule = ForwardCompatibilitySubstitutionRule::Disable;

    boost::filesystem::path    dir = (boost::filesystem::path(resources_dir()) / "profiles").make_preferred();
    PresetsConfigSubstitutions substitutions;
    std::string                errors_cummulative;
    bool                       first = true;
    std::vector<std::string> vendor_names;
    for (const std::string &vendor_name : vendor_names_in(dir))
        vendor_names.insert(vendor_name == ORCA_FILAMENT_LIBRARY ? vendor_names.begin() : vendor_names.end(), vendor_name);
    for (const std::string &vendor_name : vendor_names) {
        try {
            if (first) {
                append(substitutions, this->load_vendor_configs_from_json(dir.string(), vendor_name, PresetBundle::LoadSystem | PresetBundle::LoadFilamentOnly, compatibility_rule).first);
                first = false;
            } else {
                PresetBundle other;
                append(substitutions, other.load_vendor_configs_from_json(dir.string(), vendor_name, PresetBundle::LoadSystem | PresetBundle::LoadFilamentOnly, compatibility_rule, this).first);
                std::vector<std::string> duplicates = std::move(this->merge_presets({ &other }).front());
                if (!duplicates.empty()) {
                    errors_cummulative += "Found duplicated settings in vendor " + vendor_name + "'s json file lists: ";
                    for (size_t i = 0; i < duplicates.size(); ++i) {
                        if (i > 0) errors_cummulative += ", ";
                        errors_cummulative += duplicates[i];
                    }
                }
            }
        } catch (const std::runtime_error &err) {
            errors_cummulative += err.what();
            errors_cummulative += "\n";
        }
    }

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, errors_cummulative %1%") % errors_cummulative;
    return std::make_pair(std::move(substitutions), errors_cummulative);
}

VendorProfile PresetBundle::get_custom_vendor_models() const
{
    VendorProfile vendor;
    vendor.name = PRESET_CUSTOM_VENDOR;
    vendor.id   = PRESET_CUSTOM_VENDOR;
    for (auto &preset : printers.get_presets()) {
        if (preset.is_system) continue;
        if (printers.get_preset_base(preset) != &preset) continue;
        if (preset.is_default) continue;
        auto model = preset.config.opt_string("printer_model");
        auto variant = preset.config.opt_string("printer_variant");
        auto iter_model = std::find_if(vendor.models.begin(), vendor.models.end(), [model](VendorProfile::PrinterModel &m) {
            return m.name == model;
        });
        if (iter_model == vendor.models.end()) {
            iter_model = vendor.models.emplace(vendor.models.end(), VendorProfile::PrinterModel{});
            iter_model->id       = model;
            iter_model->name     = model;
            iter_model->variants = {VendorProfile::PrinterVariant(variant)};
        } else {
            iter_model->variants.push_back(VendorProfile::PrinterVariant(variant));
        }
    }
    return vendor;
}

std::vector<std::vector<std::string>> PresetBundle::merge_presets(const std::vector<PresetBundle*> &others)
{
    for (PresetBundle *other : others)
        this->vendors.insert(other->vendors.begin(), other->vendors.end());
    std::vector<std::vector<std::string>> duplicates(others.size());
    auto merge = [&](auto collection) {
        std::vector<PresetCollection*> other_collections;
        for (PresetBundle *other : others)
            other_collections.push_back(&(other->*collection));
        std::vector<std::vector<std::string>> collection_duplicates = (this->*collection).merge_presets(other_collections, this->vendors);
        for (size_t i = 0; i < others.size(); ++ i)
            append(duplicates[i], std::move(collection_duplicates[i]));
    };
    merge(&PresetBundle::prints);
    merge(&PresetBundle::sla_prints);
    merge(&PresetBundle::filaments);
    merge(&PresetBundle::sla_materials);
    merge(&PresetBundle::printers);
    for (PresetBundle *other : others) {
        append(this->obsolete_presets.prints,        std::move(other->obsolete_presets.prints));
        append(this->obsolete_presets.sla_prints,    std::move(other->obsolete_presets.sla_prints));
        append(this->obsolete_presets.filaments,     std::move(other->obsolete_presets.filaments));
        append(this->obsolete_presets.sla_materials, std::move(other->obsolete_presets.sla_materials));
        append(this->obsolete_presets.printers,      std::move(other->obsolete_presets.printers));
        m_errors += other->m_errors;
    }
    return duplicates;
}

void PresetBundle::update_system_maps()
{
    this->prints 	   .update_map_system_profile_renamed();
    this->sla_prints   .update_map_system_profile_renamed();
    this->filaments    .update_map_system_profile_renamed();
    this->sla_materials.update_map_system_profile_renamed();
    this->printers     .update_map_system_profile_renamed();

    this->prints       .update_map_alias_to_profile_name();
    this->sla_prints   .update_map_alias_to_profile_name();
    this->filaments    .update_map_alias_to_profile_name();
    this->sla_materials.update_map_alias_to_profile_name();
    this->printers     .update_map_alias_to_profile_name();

    this->filaments.update_library_profile_excluded_from();
}

static inline std::string remove_ini_suffix(const std::string &name)
{
    std::string out = name;
    if (boost::iends_with(out, ".ini"))
        out.erase(out.end() - 4, out.end());
    return out;
}

void PresetBundle::load_installed_printers(const AppConfig &config)
{
	this->update_system_maps();
    for (auto &preset : printers)
        preset.set_visible_from_appconfig(config);
}

const std::string& PresetBundle::get_preset_name_by_alias( const Preset::Type& preset_type, const std::string& alias) const
{
    if (preset_type == Preset::TYPE_INVALID)
        return alias;

    const PresetCollection& presets = preset_type == Preset::TYPE_PRINT     ? prints :
                                      preset_type == Preset::TYPE_SLA_PRINT ? sla_prints :
                                      preset_type == Preset::TYPE_FILAMENT  ? filaments :
                                      preset_type == Preset::TYPE_PRINTER   ? printers :
                                      sla_materials;

    return presets.get_preset_name_by_alias(alias);
}

const int PresetBundle::get_required_hrc_by_filament_type(const std::string& filament_type) const
{
    static std::unordered_map<std::string, int>filament_type_to_hrc;
    if (filament_type_to_hrc.empty()) {
        for (auto iter = filaments.m_presets.begin(); iter != filaments.m_presets.end(); iter++) {
            if (iter->vendor && iter->vendor->id == "BBL") {
                if (iter->config.has("filament_type") && iter->config.has("required_nozzle_HRC")) {
                    auto type = iter->config.opt_string("filament_type", 0);
                    auto hrc = iter->config.opt_int("required_nozzle_HRC", 0);
                    filament_type_to_hrc[type] = hrc;
                }
            }
        }
    }
    auto iter = filament_type_to_hrc.find(filament_type);
    if (iter != filament_type_to_hrc.end())
        return iter->second;
    else
        return 0;
}

void PresetBundle::save_changes_for_preset(const std::string& new_name, Preset::Type type,
                                           const std::vector<std::string>& unselected_options, bool save_to_project)
{
    PresetCollection& presets = type == Preset::TYPE_PRINT          ? prints :
                                type == Preset::TYPE_SLA_PRINT      ? sla_prints :
                                type == Preset::TYPE_FILAMENT       ? filaments :
                                type == Preset::TYPE_SLA_MATERIAL   ? sla_materials : printers;

    if (!unselected_options.empty()) {
        presets.get_edited_preset().config.apply_only(presets.get_selected_preset().config, unselected_options);
    }

    presets.save_current_preset(new_name, false, save_to_project);
    update_compatible(PresetSelectCompatibleType::Never);

    if (type == Preset::TYPE_FILAMENT) {
        set_filament_preset(0, filaments.get_selected_preset_name());
    }
}

void PresetBundle::load_installed_filaments(AppConfig &config)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": enter, printer size %1%")%printers.size();
        std::unordered_set<const Preset*> compatible_filaments;
        for (const Preset &printer : printers)
            if (printer.is_visible && printer.printer_technology() == ptFFF && printer.vendor && (!printer.vendor->models.empty())) {
                bool add_default_materials = true;
                if (config.has_section(AppConfig::SECTION_FILAMENTS))
                {
                    const std::map<std::string, std::string>& installed_filament = config.get_section(AppConfig::SECTION_FILAMENTS);
                    for (auto filament_iter : installed_filament)
                    {
                        Preset* filament = filaments.find_preset(filament_iter.first, false, true);
                        if (filament && is_compatible_with_printer(PresetWithVendorProfile(*filament, filament->vendor), PresetWithVendorProfile(printer, printer.vendor)))
                        {

                            add_default_materials = false;
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": printer %1% vendor %2% already has default filament %3%")%printer.name %printer.vendor %filament_iter.first;
                            break;
                        }
                    }
                }

                if (!add_default_materials)
                    continue;

                const VendorProfile::PrinterModel *printer_model = PresetUtils::system_printer_model(printer);
                if (!printer_model) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": can not find printer_model for printer %1%")%printer.name;
                    continue;
                }
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": printer %1% vendor %2% don't have filament visible, will add %3% default filaments")%printer.name %printer.vendor %printer_model->default_materials.size();
                for (auto default_filament: printer_model->default_materials)
                {
                    Preset* filament = filaments.find_preset(default_filament, false, true);
                    if (filament && filament->is_system)
                        compatible_filaments.insert(filament);
                }
            }
        for (const auto &filament: compatible_filaments) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": set filament %1% to visible by default")%filament->name;
            config.set(AppConfig::SECTION_FILAMENTS, filament->name, "true");
        }

    for (auto &preset : filaments)
        preset.set_visible_from_appconfig(config);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": exit.");
}

void PresetBundle::load_installed_sla_materials(AppConfig &config)
{
    if (! config.has_section(AppConfig::SECTION_MATERIALS)) {
        std::unordered_set<const Preset*> comp_sla_materials;
        for (const Preset &printer : printers)
            if (printer.is_visible && printer.printer_technology() == ptSLA) {
				const PresetWithVendorProfile printer_with_vendor_profile = printers.get_preset_with_vendor_profile(printer);
				for (const Preset &material : sla_materials)
					if (material.is_system && is_compatible_with_printer(sla_materials.get_preset_with_vendor_profile(material), printer_with_vendor_profile))
						comp_sla_materials.insert(&material);
			}
		for (const auto &material: comp_sla_materials)
            config.set(AppConfig::SECTION_MATERIALS, material->name, "true");
    }

    for (auto &preset : sla_materials)
        preset.set_visible_from_appconfig(config);
}

static void load_mixed_filament_settings(DynamicPrintConfig &project_config, AppConfig &config,
                                         const std::string &printer_name, size_t n_filaments,
                                         bool fallback_to_global)
{
    auto raw_value = [&](const char *key, bool &found) -> std::string {
        if (config.has_printer_setting(printer_name, key)) {
            found = true;
            return config.get_printer_setting(printer_name, key);
        }
        if (fallback_to_global && config.has("presets", key)) {
            found = true;
            return config.get("presets", key);
        }
        found = false;
        return std::string{};
    };
    std::vector<std::string> parts;
    auto load_bools = [&](const char *key) {
        auto &vals = project_config.option<ConfigOptionBools>(key)->values;
        vals.clear();
        bool found = false;
        const std::string s = raw_value(key, found);
        if (found && !s.empty()) {
            boost::algorithm::split(parts, s, boost::algorithm::is_any_of(","));
            for (const auto &p : parts) vals.push_back(p == "1");
        }
        vals.resize(n_filaments, false);
    };
    auto load_strings = [&](const char *key) {
        auto &vals = project_config.option<ConfigOptionStrings>(key)->values;
        vals.clear();
        bool found = false;
        const std::string s = raw_value(key, found);
        if (found && !s.empty()) {
            boost::algorithm::split(parts, s, boost::algorithm::is_any_of("|"));
            vals = parts;
        }
        vals.resize(n_filaments, std::string{});
    };

    load_bools("filament_is_mixed");
    load_strings("filament_mixed_components");
    load_strings("filament_mixed_sublayer_ratios");
    load_bools("filament_mixed_gradient");
    load_strings("filament_mixed_gradient_range");
    load_bools("filament_mixed_gradient_per_part");

    {
        auto &vals = project_config.option<ConfigOptionStrings>("filament_mixed_gradient_curve")->values;
        vals.clear();
        bool found = false;
        const std::string s = raw_value("filament_mixed_gradient_curve", found);
        if (found && !s.empty()) {
            std::vector<std::string> curves;
            if (unescape_strings_cstyle(s, curves))
                vals = std::move(curves);
        }
        vals.resize(n_filaments, std::string{});
        Slic3r::sanitize_mixed_gradient_curve_array(vals);
    }
}

void PresetBundle::update_selections(AppConfig &config)
{
    std::string initial_printer_profile_name    = printers.get_selected_preset_name();
    std::string initial_print_profile_name        = config.get_printer_setting(initial_printer_profile_name, PRESET_PRINT_NAME);
    std::string initial_filament_profile_name     = config.get_printer_setting(initial_printer_profile_name, PRESET_FILAMENT_NAME);

    prints.select_preset_by_name_strict(initial_print_profile_name);
    filaments.select_preset_by_name_strict(initial_filament_profile_name);

    this->filament_presets = { filaments.get_selected_preset_name() };
    for (unsigned int i = 1; i < 1000; ++ i) {
        char name[64];
        sprintf(name, "filament_%02u", i);
        auto f_name = config.get_printer_setting(initial_printer_profile_name, name);
        if (f_name.empty())
            break;
        this->filament_presets.emplace_back(remove_ini_suffix(f_name));
    }

    update_filament_count();

    std::vector<std::string> filament_colors;
    auto f_colors = config.get_printer_setting(initial_printer_profile_name, "filament_colors");
    if (!f_colors.empty()) {
        boost::algorithm::split(filament_colors, f_colors, boost::algorithm::is_any_of(","));
    }
    filament_colors.resize(filament_presets.size(), "#26A69A");
    project_config.option<ConfigOptionStrings>("filament_colour")->values = filament_colors;

    std::vector<std::string> multi_filament_colors;
    if (config.has_printer_setting(initial_printer_profile_name, "filament_multi_colors")) {
        boost::algorithm::split(multi_filament_colors, config.get_printer_setting(initial_printer_profile_name, "filament_multi_colors"), boost::algorithm::is_any_of(","));
    }
    if (multi_filament_colors.size() == 0) project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = filament_colors;
    else project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = multi_filament_colors;

    std::vector<std::string> filament_color_types;
    if (config.has_printer_setting(initial_printer_profile_name, "filament_color_types")) {
        boost::algorithm::split(filament_color_types, config.get_printer_setting(initial_printer_profile_name, "filament_color_types"), boost::algorithm::is_any_of(","));
    }
    filament_color_types.resize(filament_presets.size(), "1");
    project_config.option<ConfigOptionStrings>("filament_colour_type")->values = filament_color_types;

    std::vector<int> filament_maps(filament_colors.size(), 1);
    project_config.option<ConfigOptionInts>("filament_map")->values = filament_maps;

    std::vector<int> filament_nozzle_maps(filament_colors.size(), 0);
    project_config.option<ConfigOptionInts>("filament_nozzle_map")->values = filament_nozzle_maps;

    std::vector<int> filament_volume_maps(filament_colors.size(), static_cast<int>(NozzleVolumeType::nvtStandard));
    project_config.option<ConfigOptionInts>("filament_volume_map")->values = filament_volume_maps;

    std::vector<std::string> extruder_ams_count_str;
    if (config.has_printer_setting(initial_printer_profile_name, "extruder_ams_count")) {
        boost::algorithm::split(extruder_ams_count_str, config.get_printer_setting(initial_printer_profile_name, "extruder_ams_count"), boost::algorithm::is_any_of(","));
    }
    this->extruder_ams_counts = get_extruder_ams_count(extruder_ams_count_str);

    std::vector<std::string> matrix;
    if (config.has_printer_setting(initial_printer_profile_name, "flush_volumes_matrix")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_volumes_matrix"), boost::algorithm::is_any_of("|"));
        auto flush_volumes_matrix = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = std::vector<double>(flush_volumes_matrix.begin(), flush_volumes_matrix.end());
    }
    if (config.has_printer_setting(initial_printer_profile_name, "flush_volumes_vector")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_volumes_vector"), boost::algorithm::is_any_of("|"));
        auto flush_volumes_vector = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_volumes_vector")->values = std::vector<double>(flush_volumes_vector.begin(), flush_volumes_vector.end());
    }
    if (config.has_printer_setting(initial_printer_profile_name, "flush_multiplier")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_multiplier"), boost::algorithm::is_any_of("|"));
        auto flush_multipliers = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_multiplier")->values = std::vector<double>(flush_multipliers.begin(), flush_multipliers.end());
    }
    load_mixed_filament_settings(project_config, config, initial_printer_profile_name, filament_presets.size(), false);

    this->update_compatible(PresetSelectCompatibleType::Always);
    this->update_multi_material_filament_presets();

    std::string first_visible_filament_name;
    for (auto & fp : filament_presets) {
        if (auto it = filaments.find_preset_internal(fp); fp == ORCA_DEFAULT_FILAMENT_PLACEHOLDER || it == filaments.end() || !it->is_visible || !it->is_compatible) {
            if (first_visible_filament_name.empty())
                first_visible_filament_name = filaments.first_compatible().name;
            fp = first_visible_filament_name;
        }
    }

}

void PresetBundle::load_selections(AppConfig &config, const PresetPreferences& preferred_selection )
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": enter, preferred printer_model_id %1%")%preferred_selection.printer_model_id;
	this->load_installed_printers(config);

    this->load_installed_filaments(config);
    this->load_installed_sla_materials(config);

	std::string initial_printer_profile_name      = remove_ini_suffix(config.get("presets", PRESET_PRINTER_NAME));

    const Preset *initial_printer = printers.find_preset(initial_printer_profile_name);
    const Preset *preferred_printer = printers.find_system_preset_by_model_and_variant(preferred_selection.printer_model_id, preferred_selection.printer_variant);
    printers.select_preset_by_name(preferred_printer ? preferred_printer->name : initial_printer_profile_name, true);
    Preset &selected_printer = printers.get_edited_preset();
    if (selected_printer.printer_technology() == ptFFF) {
        BedType bed_type = selected_printer.get_default_bed_type(this);
        const std::string saved_bed_type = config.get_printer_setting(selected_printer.name, "curr_bed_type");
        const int saved_bed_type_value = atoi(saved_bed_type.c_str());
        if (saved_bed_type_value > btDefault && saved_bed_type_value < btCount)
            bed_type = static_cast<BedType>(saved_bed_type_value);
        project_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(bed_type));
        config.set("curr_bed_type", std::to_string(static_cast<int>(bed_type)));
    }
    CNumericLocalesSetter locales_setter;

    std::string initial_print_profile_name        = config.get_printer_setting(initial_printer_profile_name, PRESET_PRINT_NAME);
    std::string initial_filament_profile_name     = config.get_printer_setting(initial_printer_profile_name, PRESET_FILAMENT_NAME);

    if (preferred_printer)
    {
        const std::string& prefered_print_profile = preferred_printer->config.opt_string("default_print_profile");
        if ((!initial_print_profile_name.compare("Default Setting")) && (prefered_print_profile.size() > 0))
            initial_print_profile_name = prefered_print_profile;

        const std::vector<std::string>& prefered_filament_profiles = preferred_printer->config.option<ConfigOptionStrings>("default_filament_profile")->values;
        if ((!initial_filament_profile_name.compare(ORCA_DEFAULT_FILAMENT_PLACEHOLDER)) && (prefered_filament_profiles.size() > 0)) {
            const Preset* preferred_preset = this->filaments.find_preset(prefered_filament_profiles[0], false);
            if (preferred_preset && preferred_preset->is_visible) {
                initial_filament_profile_name = prefered_filament_profiles[0];
            }
        }
    }

    prints.select_preset_by_name_strict(initial_print_profile_name);
    filaments.select_preset_by_name_strict(initial_filament_profile_name);
    this->filament_presets = { filaments.get_selected_preset_name() };
    for (unsigned int i = 1; i < 1000; ++ i) {
        char name[64];
        sprintf(name, "filament_%02u", i);
        auto f_name = config.get_printer_setting(initial_printer_profile_name, name);
        if (f_name.empty())
            break;
        this->filament_presets.emplace_back(remove_ini_suffix(f_name));
    }

    update_filament_count();

    std::vector<std::string> filament_colors;
    auto f_colors = config.get_printer_setting(initial_printer_profile_name, "filament_colors");
    if (!f_colors.empty()) {
        boost::algorithm::split(filament_colors, f_colors, boost::algorithm::is_any_of(","));
    }
    filament_colors.resize(filament_presets.size(), "#26A69A");
    project_config.option<ConfigOptionStrings>("filament_colour")->values = filament_colors;

    std::vector<std::string> multi_filament_colors;
    if (config.has_printer_setting(initial_printer_profile_name, "filament_multi_colors")) {
        boost::algorithm::split(multi_filament_colors, config.get_printer_setting(initial_printer_profile_name, "filament_multi_colors"), boost::algorithm::is_any_of(","));
    }
    if (multi_filament_colors.size() == 0) project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = filament_colors;
    else project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = multi_filament_colors;

    std::vector<std::string> filament_color_types;
    if (config.has_printer_setting(initial_printer_profile_name, "filament_color_types")) {
        boost::algorithm::split(filament_color_types, config.get_printer_setting(initial_printer_profile_name, "filament_color_types"), boost::algorithm::is_any_of(","));
    }
    filament_color_types.resize(filament_presets.size(), "1");
    project_config.option<ConfigOptionStrings>("filament_colour_type")->values = filament_color_types;

    std::vector<int> filament_maps(filament_colors.size(), 1);
    project_config.option<ConfigOptionInts>("filament_map")->values = filament_maps;

    std::vector<int> filament_nozzle_maps(filament_colors.size(), 0);
    project_config.option<ConfigOptionInts>("filament_nozzle_map")->values = filament_nozzle_maps;

    std::vector<int> filament_volume_maps(filament_colors.size(), static_cast<int>(NozzleVolumeType::nvtStandard));
    project_config.option<ConfigOptionInts>("filament_volume_map")->values = filament_volume_maps;

    std::vector<std::string> extruder_ams_count_str;
    if (config.has_printer_setting(initial_printer_profile_name, "extruder_ams_count")) {
        boost::algorithm::split(extruder_ams_count_str, config.get_printer_setting(initial_printer_profile_name, "extruder_ams_count"), boost::algorithm::is_any_of(","));
    }
    this->extruder_ams_counts = get_extruder_ams_count(extruder_ams_count_str);

    std::vector<std::string> matrix;
    if (config.has_printer_setting(initial_printer_profile_name, "flush_volumes_matrix")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_volumes_matrix"), boost::algorithm::is_any_of("|"));
        auto flush_volumes_matrix = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = std::vector<double>(flush_volumes_matrix.begin(), flush_volumes_matrix.end());
    }
    if (config.has_printer_setting(initial_printer_profile_name, "flush_volumes_vector")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_volumes_vector"), boost::algorithm::is_any_of("|"));
        auto flush_volumes_vector = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_volumes_vector")->values = std::vector<double>(flush_volumes_vector.begin(), flush_volumes_vector.end());
    }
    if (config.has_printer_setting(initial_printer_profile_name, "flush_multiplier")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_multiplier"), boost::algorithm::is_any_of("|"));
        auto flush_multipliers = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_multiplier")->values = std::vector<double>(flush_multipliers.begin(), flush_multipliers.end());
    }
    load_mixed_filament_settings(project_config, config, initial_printer_profile_name, filament_presets.size(), true);

    this->update_compatible(PresetSelectCompatibleType::Always);
    this->update_multi_material_filament_presets();

    if (initial_printer != nullptr && (preferred_printer == nullptr || initial_printer == preferred_printer)) {
        auto printer_technology = printers.get_selected_preset().printer_technology();
        if (printer_technology == ptFFF && ! preferred_selection.filament.empty()) {
            std::string preferred_preset_name = get_preset_name_by_alias(Preset::Type::TYPE_FILAMENT, preferred_selection.filament);
            if (auto it = filaments.find_preset_internal(preferred_preset_name);
                it != filaments.end() && (it->name == preferred_preset_name ) && it->is_visible && it->is_compatible) {
                filaments.select_preset_by_name_strict(preferred_preset_name);
                this->filament_presets.front() = filaments.get_selected_preset_name();
            }
        } else if (printer_technology == ptSLA && ! preferred_selection.sla_material.empty()) {
            std::string preferred_preset_name = get_preset_name_by_alias(Preset::Type::TYPE_SLA_MATERIAL, preferred_selection.sla_material);
            if (auto it = sla_materials.find_preset_internal(preferred_preset_name);
                it != sla_materials.end() && it->is_visible && it->is_compatible)
                sla_materials.select_preset_by_name_strict(preferred_preset_name);
        }
    }

    std::string first_visible_filament_name;
    for (auto & fp : filament_presets) {
        if (auto it = filaments.find_preset_internal(fp); fp == ORCA_DEFAULT_FILAMENT_PLACEHOLDER || it == filaments.end() || !it->is_visible || !it->is_compatible) {
            if (first_visible_filament_name.empty())
                first_visible_filament_name = filaments.first_compatible().name;
            fp = first_visible_filament_name;
        }
    }

    const Preset& current_printer = printers.get_selected_preset();
    const Preset* base_printer = printers.get_preset_base(current_printer);
    bool use_default_nozzle_volume_type = true;
    if (base_printer) {
        std::string prev_nozzle_volume_type = config.get_nozzle_volume_types_from_config(base_printer->name);
        if (!prev_nozzle_volume_type.empty()) {
            ConfigOptionEnumsGeneric* nozzle_volume_type_option = project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
            if (nozzle_volume_type_option->deserialize(prev_nozzle_volume_type)) {
                use_default_nozzle_volume_type = false;
            }
        }
    }

    if (use_default_nozzle_volume_type) {
        project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values = current_printer.config.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type")->values;
    } else {
        auto& opt = project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values;
        const auto& opt_default = current_printer.config.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type")->values;
        while (opt.size() < opt_default.size()) {
            opt.emplace_back(opt_default[opt.size()]);
        }
    }

    std::string initial_physical_printer_name = remove_ini_suffix(config.get("presets", "physical_printer"));

    if (!initial_physical_printer_name.empty())
        physical_printers.select_printer(initial_physical_printer_name);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": finished, preferred printer_model_id %1%")%preferred_selection.printer_model_id;
}

void PresetBundle::export_selections(AppConfig &config)
{
	assert(this->printers.get_edited_preset().printer_technology() != ptFFF || filament_presets.size() >= 1);
    config.clear_section("presets");
    auto printer_name = printers.get_selected_preset_name();
    config.set("presets", PRESET_PRINTER_NAME, printer_name);

    if (printer_name == "Default Printer") {
        config.clear_printer_settings("Default Printer");
        return;
    }

    config.clear_printer_settings(printer_name);
    config.set_printer_setting(printer_name, PRESET_PRINTER_NAME, printer_name);
    config.set_printer_setting(printer_name, PRESET_PRINT_NAME, prints.get_selected_preset_name());
    config.set_printer_setting(printer_name, PRESET_FILAMENT_NAME,     filament_presets.front());
    config.set_printer_setting(printer_name, "curr_bed_type", config.get("curr_bed_type"));
    for (unsigned i = 1; i < filament_presets.size(); ++i) {
        char name[64];
        assert(!filament_presets[i].empty());
        sprintf(name, "filament_%02u", i);
        config.set_printer_setting(printer_name, name, filament_presets[i]);
    }
    CNumericLocalesSetter locales_setter;
    std::string           filament_colors = boost::algorithm::join(project_config.option<ConfigOptionStrings>("filament_colour")->values, ",");
    config.set_printer_setting(printer_name, "filament_colors", filament_colors);

    std::string           filament_multi_colors = boost::algorithm::join(project_config.option<ConfigOptionStrings>("filament_multi_colour")->values, ",");
    config.set_printer_setting(printer_name, "filament_multi_colors", filament_multi_colors);

    std::string           filament_color_types = boost::algorithm::join(project_config.option<ConfigOptionStrings>("filament_colour_type")->values, ",");
    config.set_printer_setting(printer_name, "filament_color_types", filament_color_types);

    std::string           extruder_ams_count_str = boost::algorithm::join(save_extruder_ams_count_to_string(this->extruder_ams_counts), ",");
    config.set_printer_setting(printer_name, "extruder_ams_count", extruder_ams_count_str);

    std::string flush_volumes_matrix = boost::algorithm::join(project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values |
                                                             boost::adaptors::transformed(static_cast<std::string (*)(double)>(std::to_string)),
                                                         "|");
    config.set_printer_setting(printer_name, "flush_volumes_matrix", flush_volumes_matrix);
    std::string flush_volumes_vector = boost::algorithm::join(project_config.option<ConfigOptionFloats>("flush_volumes_vector")->values |
                                                             boost::adaptors::transformed(static_cast<std::string (*)(double)>(std::to_string)),
                                                         "|");
    config.set_printer_setting(printer_name, "flush_volumes_vector", flush_volumes_vector);


    std::string flush_multiplier_str = boost::algorithm::join(project_config.option<ConfigOptionFloats>("flush_multiplier")->values |
                                                                  boost::adaptors::transformed(static_cast<std::string (*)(double)>(std::to_string)),
                                                              "|");
    config.set_printer_setting(printer_name, "flush_multiplier", flush_multiplier_str);

    auto join_bools = [](const std::vector<unsigned char> &vals) {
        std::string s;
        for (size_t i = 0; i < vals.size(); ++i) {
            if (i > 0) s += ",";
            s += (vals[i] ? "1" : "0");
        }
        return s;
    };
    if (auto *opt = project_config.option<ConfigOptionBools>("filament_is_mixed"))
        config.set_printer_setting(printer_name, "filament_is_mixed", join_bools(opt->values));
    if (auto *opt = project_config.option<ConfigOptionStrings>("filament_mixed_components"))
        config.set_printer_setting(printer_name, "filament_mixed_components", boost::algorithm::join(opt->values, "|"));
    if (auto *opt = project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios"))
        config.set_printer_setting(printer_name, "filament_mixed_sublayer_ratios", boost::algorithm::join(opt->values, "|"));
    if (auto *opt = project_config.option<ConfigOptionBools>("filament_mixed_gradient"))
        config.set_printer_setting(printer_name, "filament_mixed_gradient", join_bools(opt->values));
    if (auto *opt = project_config.option<ConfigOptionStrings>("filament_mixed_gradient_range"))
        config.set_printer_setting(printer_name, "filament_mixed_gradient_range", boost::algorithm::join(opt->values, "|"));
    if (auto *opt = project_config.option<ConfigOptionStrings>("filament_mixed_gradient_curve"))
        config.set_printer_setting(printer_name, "filament_mixed_gradient_curve", escape_strings_cstyle(opt->values));
    if (auto *opt = project_config.option<ConfigOptionBools>("filament_mixed_gradient_per_part"))
        config.set_printer_setting(printer_name, "filament_mixed_gradient_per_part", join_bools(opt->values));

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": printer %1%, print %2%, filaments[0] %3% ")%printers.get_selected_preset_name() % prints.get_selected_preset_name() %filament_presets[0];
}

void PresetBundle::set_num_filaments(unsigned int n, std::string new_color)
{
    unsigned old_filament_count = this->filament_presets.size();
    if (n > old_filament_count && old_filament_count != 0)
        filament_presets.resize(n, filament_presets.back());
    else {
        filament_presets.resize(n);
    }
    ConfigOptionStrings* filament_color = project_config.option<ConfigOptionStrings>("filament_colour");
    ConfigOptionStrings *filament_multi_color = project_config.option<ConfigOptionStrings>("filament_multi_colour");
    ConfigOptionStrings* filament_color_type = project_config.option<ConfigOptionStrings>("filament_colour_type");
    ConfigOptionInts* filament_map = project_config.option<ConfigOptionInts>("filament_map");
    ConfigOptionInts* filament_nozzle_map = project_config.option<ConfigOptionInts>("filament_nozzle_map");
    ConfigOptionInts* filament_volume_map = project_config.option<ConfigOptionInts>("filament_volume_map");

    const size_t old_slot_count = filament_color->values.size();

    filament_color->resize(n);
    filament_multi_color->values.resize(n);
    for (size_t i = 0; i < n; i++) {
        filament_multi_color->values[i] = filament_color->values[i];
    }
    filament_color_type->resize(n);
    filament_map->values.resize(n, 1);
    filament_nozzle_map->values.resize(n, 0);
    filament_volume_map->values.resize(n, static_cast<int>(NozzleVolumeType::nvtStandard));
    ams_multi_color_filment.resize(n);

    resize_mixed_filament_metadata(project_config, old_slot_count, n);

    if (!new_color.empty()) {
        for (size_t i = old_slot_count; i < n; i++) {
            filament_color->values[i] = new_color;
            filament_multi_color->values[i] = new_color;
            filament_color_type->values[i]  = "1";
        }
    }

    update_multi_material_filament_presets();
}

void PresetBundle::update_num_filaments(unsigned int to_del_flament_id)
{
    unsigned old_filament_count = this->filament_presets.size();
    assert(to_del_flament_id < old_filament_count);
    filament_presets.erase(filament_presets.begin() + to_del_flament_id);

    {
        Preset& edited_preset = filaments.get_edited_preset();
        bool    edited_preset_deleted = true;
        for (std::string filament_preset_name : filament_presets) {
            if (filament_preset_name == edited_preset.name) {
                edited_preset_deleted = false;
            }
        }
        if (edited_preset_deleted) {
            filaments.select_preset_by_name(filament_presets.front(), false);
        }
    }

    ConfigOptionStrings *filament_color = project_config.option<ConfigOptionStrings>("filament_colour");
    ConfigOptionStrings *filament_multi_color = project_config.option<ConfigOptionStrings>("filament_multi_colour");
    ConfigOptionStrings *filament_color_type  = project_config.option<ConfigOptionStrings>("filament_colour_type");
    ConfigOptionInts* filament_map = project_config.option<ConfigOptionInts>("filament_map");
    ConfigOptionInts* filament_nozzle_map = project_config.option<ConfigOptionInts>("filament_nozzle_map");
    ConfigOptionInts* filament_volume_map = project_config.option<ConfigOptionInts>("filament_volume_map");
    if (filament_color->values.size() > to_del_flament_id) {
        filament_color->values.erase(filament_color->values.begin() + to_del_flament_id);
        if (filament_map->values.size() > to_del_flament_id) {
            filament_map->values.erase(filament_map->values.begin() + to_del_flament_id);
        }
        if (filament_nozzle_map->values.size() > to_del_flament_id) {
            filament_nozzle_map->values.erase(filament_nozzle_map->values.begin() + to_del_flament_id);
        }
        if (filament_volume_map->values.size() > to_del_flament_id) {
            filament_volume_map->values.erase(filament_volume_map->values.begin() + to_del_flament_id);
        }
    }
    else {
        filament_color->values.resize(to_del_flament_id);
        filament_map->values.resize(to_del_flament_id, 1);
        filament_nozzle_map->values.resize(to_del_flament_id, 0);
        filament_volume_map->values.resize(to_del_flament_id, static_cast<int>(NozzleVolumeType::nvtStandard));
    }

    auto erase_or_resize = [to_del_flament_id](auto& container) {
        if (container.size() > to_del_flament_id) {
            container.erase(container.begin() + to_del_flament_id);
        } else {
            container.resize(to_del_flament_id);
        }
    };

    erase_or_resize(filament_multi_color->values);
    erase_or_resize(filament_color_type->values);
    erase_or_resize(ams_multi_color_filment);

    {
        auto *is_mixed_opt = project_config.option<ConfigOptionBools>("filament_is_mixed");
        auto *comp_opt     = project_config.option<ConfigOptionStrings>("filament_mixed_components");
        if (is_mixed_opt && comp_opt) {
            bool del_is_physical = (to_del_flament_id >= is_mixed_opt->values.size()
                                    || !is_mixed_opt->values[to_del_flament_id]);
            if (del_is_physical)
                remap_mixed_components_on_delete(is_mixed_opt->values, comp_opt->values,
                                                 to_del_flament_id + 1);
        }
        if (is_mixed_opt)
            erase_or_resize(is_mixed_opt->values);
        if (comp_opt)
            erase_or_resize(comp_opt->values);
    }
    if (auto *opt = project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios"))
        erase_or_resize(opt->values);
    if (auto *opt = project_config.option<ConfigOptionBools>("filament_mixed_gradient"))
        erase_or_resize(opt->values);
    if (auto *opt = project_config.option<ConfigOptionStrings>("filament_mixed_gradient_range"))
        erase_or_resize(opt->values);
    if (auto *opt = project_config.option<ConfigOptionStrings>("filament_mixed_gradient_curve"))
        erase_or_resize(opt->values);
    if (auto *opt = project_config.option<ConfigOptionBools>("filament_mixed_gradient_per_part"))
        erase_or_resize(opt->values);

    update_multi_material_filament_presets(to_del_flament_id);
}

bool PresetBundle::is_mixed_filament(size_t idx) const
{
    auto *opt = project_config.option<ConfigOptionBools>("filament_is_mixed");
    return opt && idx < opt->values.size() && opt->values[idx];
}

size_t PresetBundle::num_mixed_filaments() const
{
    const auto *opt = project_config.option<ConfigOptionBools>("filament_is_mixed");
    if (opt == nullptr)
        return 0;
    const size_t slot_count = project_config.option<ConfigOptionStrings>("filament_colour")->values.size();
    return size_t(std::count(opt->values.begin(), opt->values.begin() + std::min(slot_count, opt->values.size()), true));
}

size_t PresetBundle::num_physical_filaments() const
{
    const auto *colors = project_config.option<ConfigOptionStrings>("filament_colour");
    return colors->values.size() - num_mixed_filaments();
}

std::vector<size_t> PresetBundle::physical_filament_config_indices() const
{
    std::vector<size_t> indices;
    for (size_t i = 0; i < filament_presets.size(); ++i)
        if (!is_mixed_filament(i))
            indices.push_back(i);
    return indices;
}


static void warn_ambiguous_filament_id_match(const PresetCollection &filaments, PresetCollection::ConstIterator match, const std::string &filament_id)
{
    if (match == filaments.end())
        return;
    std::string others;
    for (auto it = std::next(match); it != filaments.end(); ++it)
        if (it->is_compatible && filaments.get_preset_base(*it) == &*it && it->filament_id == filament_id)
            others += (others.empty() ? "\"" : ", \"") + it->name + "\"";
    if (!others.empty())
        BOOST_LOG_TRIVIAL(warning) << "Ambiguous AMS filament match: filament_id \"" << filament_id
                                   << "\" matches multiple presets compatible with the selected printer; picked \"" << match->name
                                   << "\", also matches " << others;
}

void PresetBundle::get_ams_cobox_infos(AMSComboInfo& combox_info)
{
    combox_info.clear();
    for (auto &entry : filament_ams_list) {
        auto &ams                  = entry.second;
        auto  filament_id          = ams.opt_string("filament_id", 0u);
        auto  filament_color       = ams.opt_string("filament_colour", 0u);
        auto  ams_name             = ams.opt_string("tray_name", 0u);
        auto  filament_changed     = !ams.has("filament_changed") || ams.opt_bool("filament_changed");
        auto  filament_multi_color = ams.opt<ConfigOptionStrings>("filament_multi_colour")->values;
        if (filament_id.empty()) {
            continue;
        }
        if (!filament_changed && this->filament_presets.size() > combox_info.ams_filament_presets.size()) {
            combox_info.ams_filament_presets.push_back(this->filament_presets[combox_info.ams_filament_presets.size()]);
            combox_info.ams_filament_colors.push_back(filament_color);
            combox_info.ams_multi_color_filment.push_back(filament_multi_color);
            combox_info.ams_names.push_back(ams_name);
            continue;
        }
        auto iter = std::find_if(filaments.begin(), filaments.end(),
                                 [this, &filament_id](auto &f) { return f.is_compatible && filaments.get_preset_base(f) == &f && f.filament_id == filament_id; });
        warn_ambiguous_filament_id_match(filaments, iter, filament_id);
        if (iter == filaments.end()) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": filament_id %1% not found or system or compatible") % filament_id;
            auto filament_type = ams.opt_string("filament_type", 0u);
            if (!filament_type.empty()) {
                filament_type = "Generic " + filament_type;
                iter          = std::find_if(filaments.begin(), filaments.end(),
                                    [&filament_type](auto &f) { return f.is_compatible && f.is_system && boost::algorithm::starts_with(f.name, filament_type); });
            }
            if (iter == filaments.end()) {
                if (combox_info.ams_filament_presets.size() < this->filament_presets.size()) {
                    combox_info.ams_filament_presets.push_back(this->filament_presets[combox_info.ams_filament_presets.size()]);
                    combox_info.ams_filament_colors.push_back(filament_color);
                    combox_info.ams_multi_color_filment.push_back(filament_multi_color);
                    combox_info.ams_names.push_back(ams_name);
                    continue;
                }
                iter = std::find_if(filaments.begin(), filaments.end(), [](auto &f) { return f.is_compatible && f.is_system; });
                if (iter == filaments.end())
                    continue;
            }
            filament_id = iter->filament_id;
        }
        combox_info.ams_filament_presets.push_back(iter->name);
        combox_info.ams_filament_colors.push_back(filament_color);
        combox_info.ams_multi_color_filment.push_back(filament_multi_color);
        combox_info.ams_names.push_back(ams_name);
    }
}

unsigned int PresetBundle::sync_ams_list(std::vector<std::pair<DynamicPrintConfig *,std::string>> &unknowns, bool use_map, std::map<int, AMSMapInfo> &maps, bool enable_append, MergeFilamentInfo &merge_info, bool color_only)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "use_map:" << use_map << " enable_append:" << enable_append;
    std::vector<std::string> ams_filament_presets;
    std::vector<std::string> ams_filament_colors;
    std::vector<std::string> ams_filament_color_types;
    std::vector<AMSMapInfo>  ams_array_maps;
    ams_multi_color_filment.clear();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": filament_ams_list size: %1%") % filament_ams_list.size();
    struct AmsInfo
    {
        bool valid{false};
        bool is_map{false};
        bool is_placeholder{false};
        std::string filament_color  = "";
        std::string filament_color_type = "";
        std::string filament_preset = "";
        std::vector<std::string> mutli_filament_color;
    };
    auto is_double_extruder = get_printer_extruder_count() == 2;
    std::vector<AmsInfo> ams_infos;
    int                  index = 0;
    for (auto &entry : filament_ams_list) {
        auto & ams = entry.second;
        auto filament_id = ams.opt_string("filament_id", 0u);
        auto filament_color = ams.opt_string("filament_colour", 0u);
        auto filament_color_type = ams.opt_string("filament_colour_type", 0u);
        auto filament_changed = !ams.has("filament_changed") || ams.opt_bool("filament_changed");
        auto filament_multi_color = ams.opt<ConfigOptionStrings>("filament_multi_colour")->values;
        auto ams_id     = ams.opt_string("ams_id", 0u);
        auto slot_id    = ams.opt_string("slot_id", 0u);
        auto is_placeholder = ams.has("filament_slot_placeholder") && ams.opt_bool("filament_slot_placeholder", 0u);
        ams_infos.push_back({filament_id.empty() ? false : true, false, is_placeholder, filament_color});
        AMSMapInfo temp = {ams_id, slot_id};
        ams_array_maps.push_back(temp);
        index++;
        if (filament_id.empty()) {
            if (use_map) {
                for (int j = maps.size() - 1; j >= 0; j--) {
                    if (maps[j].slot_id == slot_id && maps[j].ams_id == ams_id) {
                        maps.erase(j);
                    }
                }
                ams_filament_presets.push_back("Generic PLA");
                auto default_unknown_color = "#CECECE";
                ams_filament_colors.push_back(default_unknown_color);
                ams_filament_color_types.push_back("1");
                if (filament_multi_color.size() == 0) {
                    filament_multi_color.push_back(default_unknown_color);
                }
                ams_multi_color_filment.push_back(filament_multi_color);
            } else if (is_placeholder) {
                ams_filament_presets.push_back("");
                ams_filament_colors.push_back("");
                ams_filament_color_types.push_back("");
                ams_multi_color_filment.push_back({});
            }
            continue;
        }
        if (!filament_changed && this->filament_presets.size() > ams_filament_presets.size()) {
            ams_filament_presets.push_back(this->filament_presets[ams_filament_presets.size()]);
            ams_filament_colors.push_back(filament_color);
            ams_filament_color_types.push_back(filament_color_type);
            ams_multi_color_filment.push_back(filament_multi_color);
            continue;
        }
        bool has_type = false;
        auto filament_type = ams.opt_string("filament_type", 0u);
        auto iter = std::find_if(filaments.begin(), filaments.end(), [this, &filament_id, &has_type, filament_type](auto &f) {
            has_type |= f.config.opt_string("filament_type", 0u) == filament_type;
            return f.is_compatible && filaments.get_preset_base(f) == &f && f.filament_id == filament_id; });
        warn_ambiguous_filament_id_match(filaments, iter, filament_id);
        if (iter == filaments.end()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": filament_id %1% not found or system or compatible") % filament_id;
            if (!filament_type.empty()) {
                auto original_type = filament_type;
                filament_type = "Generic " + filament_type;
                iter = std::find_if(filaments.begin(), filaments.end(), [&filament_type](auto &f) {
                    return f.is_compatible && f.is_system
                        && boost::algorithm::starts_with(f.name, filament_type);
                });
                if (iter == filaments.end()) {
                    auto upper_type = boost::to_upper_copy(original_type);
                    auto contains_word = [](const std::string& haystack, const std::string& needle) {
                        auto pos = haystack.find(needle);
                        while (pos != std::string::npos) {
                            bool start_ok = (pos == 0 || !std::isalnum(static_cast<unsigned char>(haystack[pos - 1])));
                            bool end_ok   = (pos + needle.size() >= haystack.size() ||
                                             !std::isalnum(static_cast<unsigned char>(haystack[pos + needle.size()])));
                            if (start_ok && end_ok)
                                return true;
                            pos = haystack.find(needle, pos + 1);
                        }
                        return false;
                    };
                    size_t best_len = 0;
                    for (auto it = filaments.begin(); it != filaments.end(); ++it) {
                        if (!it->is_compatible || !it->is_system || !boost::algorithm::starts_with(it->name, "Generic "))
                            continue;
                        auto preset_type = boost::to_upper_copy(it->config.opt_string("filament_type", 0u));
                        if (preset_type.size() > best_len && contains_word(upper_type, preset_type)) {
                            iter = it;
                            best_len = preset_type.size();
                            filament_type = "Generic " + it->config.opt_string("filament_type", 0u);
                        }
                    }
                }
            }
            if (iter == filaments.end()) {
                if (ams_filament_presets.size() < this->filament_presets.size()) {
                    ams_filament_presets.push_back(this->filament_presets[ams_filament_presets.size()]);
                    ams_filament_colors.push_back(filament_color);
                    ams_filament_color_types.push_back(filament_color_type);
                    ams_multi_color_filment.push_back(filament_multi_color);
                    unknowns.emplace_back(&ams, has_type ? L("The filament may not be compatible with the current machine settings. Generic filament presets will be used.") :
                                                           L("The filament model is unknown. Still using the previous filament preset."));
                    continue;
                }
                iter = std::find_if(filaments.begin(), filaments.end(), [](auto &f) {
                    return f.is_compatible && f.is_system
                        && boost::algorithm::starts_with(f.name, "Generic ");
                });
                if (iter == filaments.end())
                    iter = std::find_if(filaments.begin(), filaments.end(), [](auto &f) {
                        return f.is_compatible && f.is_system;
                    });
                if (iter == filaments.end())
                    continue;
            }
            unknowns.emplace_back(&ams, boost::algorithm::starts_with(iter->name, filament_type) ?
                                            (has_type ? L("The filament may not be compatible with the current machine settings. Generic filament presets will be used.") :
                                                        L("The filament model is unknown. Generic filament presets will be used.")) :
                                            (has_type ? L("The filament may not be compatible with the current machine settings. A random filament preset will be used.") :
                                                        L("The filament model is unknown. A random filament preset will be used.")));
            filament_id = iter->filament_id;
        }
        ams_filament_presets.push_back(iter->name);
        ams_filament_colors.push_back(filament_color);
        ams_filament_color_types.push_back(filament_color_type);
        ams_multi_color_filment.push_back(filament_multi_color);
    }
    if (ams_filament_presets.empty())
        return 0;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "get filament_colour and from config";
    ConfigOptionStrings *filament_color = project_config.option<ConfigOptionStrings>("filament_colour");
    ConfigOptionStrings *filament_color_type = project_config.option<ConfigOptionStrings>("filament_colour_type");
    ConfigOptionInts *   filament_map = project_config.option<ConfigOptionInts>("filament_map");
    ConfigOptionInts *   filament_volume_map = project_config.option<ConfigOptionInts>("filament_volume_map");
    struct MixedSlotSnapshot {
        std::string preset;
        std::string color;
        std::string color_type;
        std::string mixed_components;
        std::string mixed_sublayer_ratios;
        bool        mixed_gradient = false;
        std::string mixed_gradient_range;
        std::string mixed_gradient_curve;
        bool        mixed_gradient_per_part = false;
    };
    std::vector<MixedSlotSnapshot> mixed_snapshots;
    auto* is_mixed_opt         = project_config.option<ConfigOptionBools>("filament_is_mixed");
    auto* mixed_comp_opt       = project_config.option<ConfigOptionStrings>("filament_mixed_components");
    auto* mixed_ratios_opt     = project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
    auto* mixed_gradient_opt   = project_config.option<ConfigOptionBools>("filament_mixed_gradient");
    auto* mixed_grad_range_opt = project_config.option<ConfigOptionStrings>("filament_mixed_gradient_range");
    auto* mixed_grad_curve_opt = project_config.option<ConfigOptionStrings>("filament_mixed_gradient_curve");
    auto* mixed_per_part_opt   = project_config.option<ConfigOptionBools>("filament_mixed_gradient_per_part");
    if (is_mixed_opt) {
        for (size_t i = 0; i < is_mixed_opt->values.size() && i < this->filament_presets.size(); ++i) {
            if (!is_mixed_opt->values[i])
                continue;
            MixedSlotSnapshot snap;
            snap.preset     = this->filament_presets[i];
            snap.color      = (i < filament_color->values.size())      ? filament_color->values[i]      : "";
            snap.color_type = (i < filament_color_type->values.size()) ? filament_color_type->values[i] : "";
            if (mixed_comp_opt     && i < mixed_comp_opt->values.size())     snap.mixed_components      = mixed_comp_opt->values[i];
            if (mixed_ratios_opt   && i < mixed_ratios_opt->values.size())   snap.mixed_sublayer_ratios = mixed_ratios_opt->values[i];
            if (mixed_gradient_opt && i < mixed_gradient_opt->values.size()) snap.mixed_gradient        = mixed_gradient_opt->values[i];
            if (mixed_grad_range_opt && i < mixed_grad_range_opt->values.size()) snap.mixed_gradient_range = mixed_grad_range_opt->values[i];
            if (mixed_grad_curve_opt && i < mixed_grad_curve_opt->values.size()) snap.mixed_gradient_curve = mixed_grad_curve_opt->values[i];
            if (mixed_per_part_opt && i < mixed_per_part_opt->values.size()) snap.mixed_gradient_per_part = mixed_per_part_opt->values[i];
            mixed_snapshots.push_back(snap);
        }
        if (!mixed_snapshots.empty()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": stripping " << mixed_snapshots.size() << " mixed filament slot(s) before AMS sync";
            size_t phys_count = this->filament_presets.size() - mixed_snapshots.size();
            this->filament_presets.resize(phys_count);
            filament_color->values.resize(phys_count);
            filament_color_type->values.resize(phys_count);
            filament_map->values.resize(phys_count, 1);
            is_mixed_opt->values.resize(phys_count);
            if (mixed_comp_opt)       mixed_comp_opt->values.resize(phys_count);
            if (mixed_ratios_opt)     mixed_ratios_opt->values.resize(phys_count);
            if (mixed_gradient_opt)   mixed_gradient_opt->values.resize(phys_count);
            if (mixed_grad_range_opt) mixed_grad_range_opt->values.resize(phys_count);
            if (mixed_grad_curve_opt) mixed_grad_curve_opt->values.resize(phys_count);
            if (mixed_per_part_opt)   mixed_per_part_opt->values.resize(phys_count);
        }
    }

    if (color_only) {
        auto get_map_index = [&ams_infos](const std::vector<AMSMapInfo> &infos, const AMSMapInfo &temp) {
            for (int i = 0; i < infos.size(); i++) {
                if (infos[i].slot_id == temp.slot_id && infos[i].ams_id == temp.ams_id) {
                    ams_infos[i].is_map = true;
                    return i;
                }
            }
            return -1;
        };

        auto exist_colors = filament_color->values;
        std::vector<std::vector<std::string>> exist_multi_color_filment(exist_colors.size());
        for (size_t i = 0; i < exist_colors.size(); i++) {
            exist_multi_color_filment[i] = {exist_colors[i]};
        }

        ConfigOptionStrings *project_multi_color = project_config.option<ConfigOptionStrings>("filament_multi_colour");
        if (project_multi_color) {
            for (size_t i = 0; i < std::min(exist_multi_color_filment.size(), project_multi_color->values.size()); i++) {
                std::vector<std::string> colors = split_string(project_multi_color->values[i], ' ');
                if (!colors.empty()) {
                    exist_multi_color_filment[i] = colors;
                }
            }
        }

        bool mapped_any = false;
        if (use_map && !maps.empty()) {
            for (size_t i = 0; i < exist_colors.size(); i++) {
                if (maps.find(i) == maps.end()) {
                    continue;
                }
                int valid_index = get_map_index(ams_array_maps, maps[i]);
                if (valid_index >= 0 && valid_index < int(ams_filament_colors.size()) && !ams_filament_colors[valid_index].empty()) {
                    exist_colors[i] = ams_filament_colors[valid_index];
                    mapped_any = true;
                    if (valid_index < int(ams_multi_color_filment.size()) && !ams_multi_color_filment[valid_index].empty()) {
                        exist_multi_color_filment[i] = ams_multi_color_filment[valid_index];
                    } else {
                        exist_multi_color_filment[i] = {ams_filament_colors[valid_index]};
                    }
                }
            }
        }
        if (!use_map || maps.empty() || !mapped_any) {
            size_t sync_count = std::min(exist_colors.size(), ams_filament_colors.size());
            for (size_t i = 0; i < sync_count; i++) {
                if (ams_filament_colors[i].empty()) {
                    continue;
                }
                exist_colors[i] = ams_filament_colors[i];
                if (i < ams_multi_color_filment.size() && !ams_multi_color_filment[i].empty()) {
                    exist_multi_color_filment[i] = ams_multi_color_filment[i];
                } else {
                    exist_multi_color_filment[i] = {ams_filament_colors[i]};
                }
            }
        }

        filament_color->values = exist_colors;
        ams_multi_color_filment = exist_multi_color_filment;
        merge_info.merges.clear();
    } else if (use_map) {
        auto check_has_merge_info = [](std::map<int, AMSMapInfo> &maps, MergeFilamentInfo &merge_info, int exist_colors_size) {
            std::set<int> done;
            for (auto it_i = maps.begin(); it_i != maps.end(); ++it_i) {
                std::vector<int> same_ams;
                same_ams.emplace_back(it_i->first);
                for (auto it_j = std::next(it_i); it_j != maps.end(); ++it_j) {
                    if (done.find(it_j->first) != done.end()) {
                        continue;
                    }
                    if (it_i->second.slot_id == "" || it_i->second.ams_id == ""){
                        continue;
                    }
                    if (it_i->second.slot_id == it_j->second.slot_id && it_i->second.ams_id == it_j->second.ams_id) {
                        same_ams.emplace_back(it_j->first);
                        done.insert(it_j->first);
                    }
                }
                if (same_ams.size() > 1) {
                    merge_info.merges.emplace_back(same_ams);
                }
            }
        };
        check_has_merge_info(maps, merge_info,filament_color->values.size());
        auto get_map_index = [&ams_infos](const std::vector<AMSMapInfo> &infos, const AMSMapInfo &temp) {
            for (int i = 0; i < infos.size(); i++) {
                if (infos[i].slot_id == temp.slot_id && infos[i].ams_id == temp.ams_id) {
                    ams_infos[i].is_map = true;
                    return i;
                }
            }
            return -1;
        };
        std::vector<AmsInfo> need_append_colors;
        auto exist_colors = filament_color->values;
        auto exist_color_types = filament_color_type->values;
        auto exist_filament_presets = this->filament_presets;
        std::vector<std::vector<std::string>> exist_multi_color_filment;
        exist_multi_color_filment.resize(exist_colors.size());
        for (int i = 0; i < exist_colors.size(); i++) {
            exist_multi_color_filment[i] = {exist_colors[i]};
        }
        for (size_t i = 0; i < exist_colors.size(); i++) {
            if (maps.find(i) != maps.end()) {
                auto valid_index = get_map_index(ams_array_maps, maps[i]);
                if (valid_index >= 0 && valid_index < ams_filament_presets.size()) {
                    exist_colors[i]           = ams_filament_colors[valid_index];
                    exist_color_types[i]      = ams_filament_color_types[valid_index];
                    exist_filament_presets[i] = ams_filament_presets[valid_index];
                    exist_multi_color_filment[i] = ams_multi_color_filment[valid_index];
                } else {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "check error: array bound (mapping exist)";
                }
            }
        }
        for (size_t i = 0; i < ams_infos.size(); i++) {
            if (ams_infos[i].valid) {
                if (i >= ams_filament_presets.size()) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "check error: array bound (check append)";
                    continue;
                }
                ams_infos[i].filament_preset = ams_filament_presets[i];
                ams_infos[i].mutli_filament_color = ams_multi_color_filment[i];
                if (!ams_infos[i].is_map) {
                    need_append_colors.emplace_back(ams_infos[i]);
                    ams_filament_colors[i]     = "";
                    ams_filament_color_types[i] = "";
                    ams_filament_presets[i]    = "";
                    ams_multi_color_filment[i] = std::vector<std::string>();
                }
            }
            else {
                ams_filament_colors[i]     = "";
                ams_filament_color_types[i] = "";
                ams_filament_presets[i]    = "";
                ams_multi_color_filment[i] = std::vector<std::string>();
            }
        }
        ams_filament_colors.erase(std::remove_if(ams_filament_colors.begin(), ams_filament_colors.end(), [](std::string &value) { return value.empty(); }),
                                  ams_filament_colors.end());
        ams_filament_color_types.erase(std::remove_if(ams_filament_color_types.begin(), ams_filament_color_types.end(), [](std::string &value) { return value.empty(); }),
                                       ams_filament_color_types.end());
        ams_filament_presets.erase(std::remove_if(ams_filament_presets.begin(), ams_filament_presets.end(), [](std::string &value) { return value.empty(); }),
                                   ams_filament_presets.end());
        ams_multi_color_filment.erase(std::remove_if(ams_multi_color_filment.begin(), ams_multi_color_filment.end(),
                                                     [](std::vector<std::string> &value) { return value.empty(); }),
                                      ams_multi_color_filment.end());
        if (need_append_colors.size() > 0 && enable_append) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "need_append_colors.size() > 0 && enable_append";
            auto get_idx_in_array = [](std::vector<std::string> &presets, std::vector<std::string> &colors, const std::string &preset, const std::string &color) -> int {
                for (size_t i = 0; i < presets.size(); i++) {
                    if (presets[i] == preset && colors[i] == color) {
                        return i;
                    }
                }
                return -1;
            };
            for (size_t i = 0; i < need_append_colors.size(); i++){
                if (exist_filament_presets.size() >= MAXIMUM_AMS_SYNC_FILAMENT_NUMBER){
                    break;
                }
                auto idx = get_idx_in_array(exist_filament_presets, exist_colors, need_append_colors[i].filament_preset, need_append_colors[i].filament_color);
                if (idx >= 0) {
                    continue;
                }
                exist_filament_presets.push_back(need_append_colors[i].filament_preset);
                exist_colors.push_back(need_append_colors[i].filament_color);
                exist_color_types.push_back(need_append_colors[i].filament_color_type);
                exist_multi_color_filment.push_back(need_append_colors[i].mutli_filament_color);
            }
        }
        filament_color->values = exist_colors;
        filament_color_type->values = exist_color_types;
        ams_multi_color_filment = exist_multi_color_filment;
        this->filament_presets = exist_filament_presets;
        filament_map->values.resize(exist_filament_presets.size(), 1);
        filament_volume_map->values.resize(exist_filament_presets.size(), static_cast<int>(NozzleVolumeType::nvtStandard));
    }
    else {
        bool has_placeholders = std::any_of(ams_infos.begin(), ams_infos.end(),
                                             [](const AmsInfo& a) { return a.is_placeholder; });
        if (has_placeholders) {
            auto exist_colors       = filament_color->values;
            auto exist_color_types  = filament_color_type->values;
            auto exist_presets      = this->filament_presets;

            size_t tray_count = ams_filament_presets.size();
            size_t total      = std::max(tray_count, exist_presets.size());

            std::vector<std::string> result_colors;
            std::vector<std::string> result_color_types;
            std::vector<std::string> result_presets;
            std::vector<std::vector<std::string>> result_multi_colors;

            for (size_t i = 0; i < total; i++) {
                bool is_loaded = (i < ams_infos.size() && ams_infos[i].valid);

                if (is_loaded) {
                    result_colors.push_back(ams_filament_colors[i]);
                    result_color_types.push_back(ams_filament_color_types[i]);
                    result_presets.push_back(ams_filament_presets[i]);
                    result_multi_colors.push_back(
                        i < ams_multi_color_filment.size() ? ams_multi_color_filment[i]
                                                           : std::vector<std::string>{ams_filament_colors[i]});
                } else if (i < exist_presets.size()) {
                    result_colors.push_back(exist_colors[i]);
                    result_color_types.push_back(exist_color_types[i]);
                    result_presets.push_back(exist_presets[i]);
                    result_multi_colors.push_back({exist_colors[i]});
                } else {
                    auto it = std::find_if(filaments.begin(), filaments.end(), [](const Preset &f) {
                        return f.is_compatible && f.is_system
                            && boost::algorithm::starts_with(f.name, "Generic ");
                    });
                    std::string fallback_name = (it != filaments.end()) ? it->name : filaments.first_visible().name;
                    result_colors.push_back("#CECECE");
                    result_color_types.push_back("1");
                    result_presets.push_back(fallback_name);
                    result_multi_colors.push_back({"#CECECE"});
                }
            }

            filament_color->values      = result_colors;
            filament_color_type->values = result_color_types;
            this->filament_presets      = result_presets;
            ams_multi_color_filment     = result_multi_colors;
            filament_map->values.resize(total, 1);
            filament_volume_map->values.resize(total, static_cast<int>(NozzleVolumeType::nvtStandard));
        } else {
            filament_color->values = ams_filament_colors;
            filament_color_type->values = ams_filament_color_types;
            this->filament_presets = ams_filament_presets;
            filament_map->values.resize(ams_filament_colors.size(), 1);
            filament_volume_map->values.resize(ams_filament_colors.size(), static_cast<int>(NozzleVolumeType::nvtStandard));
        }

        auto& print_config = this->prints.get_edited_preset().config;
        auto  support_filament_opt = print_config.option<ConfigOptionInt>("support_filament");
        auto support_interface_filament_opt = print_config.option<ConfigOptionInt>("support_interface_filament");
        if (support_filament_opt->value > filament_color_type->values.size())
            support_filament_opt->value = 0;

        if (support_interface_filament_opt->value > filament_color_type->values.size())
            support_interface_filament_opt->value = 0;
    }
    if (!mixed_snapshots.empty()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": re-appending " << mixed_snapshots.size() << " mixed filament slot(s) after AMS sync";
        size_t new_phys_count = this->filament_presets.size();
        if (is_mixed_opt)         is_mixed_opt->values.resize(new_phys_count, (unsigned char)false);
        if (mixed_comp_opt)       mixed_comp_opt->values.resize(new_phys_count);
        if (mixed_ratios_opt)     mixed_ratios_opt->values.resize(new_phys_count);
        if (mixed_gradient_opt)   mixed_gradient_opt->values.resize(new_phys_count, (unsigned char)false);
        if (mixed_grad_range_opt) mixed_grad_range_opt->values.resize(new_phys_count);
        if (mixed_grad_curve_opt) mixed_grad_curve_opt->values.resize(new_phys_count);
        if (mixed_per_part_opt)   mixed_per_part_opt->values.resize(new_phys_count, (unsigned char)false);

        for (auto& snap : mixed_snapshots) {
            this->filament_presets.push_back(snap.preset);
            filament_color->values.push_back(snap.color);
            filament_color_type->values.push_back(snap.color_type);
            ams_multi_color_filment.push_back({snap.color});
            filament_map->values.push_back(1);
            if (is_mixed_opt)         is_mixed_opt->values.push_back((unsigned char)true);
            if (mixed_comp_opt)       mixed_comp_opt->values.push_back(snap.mixed_components);
            if (mixed_ratios_opt)     mixed_ratios_opt->values.push_back(snap.mixed_sublayer_ratios);
            if (mixed_gradient_opt)   mixed_gradient_opt->values.push_back((unsigned char)snap.mixed_gradient);
            if (mixed_grad_range_opt) mixed_grad_range_opt->values.push_back(snap.mixed_gradient_range);
            if (mixed_grad_curve_opt) mixed_grad_curve_opt->values.push_back(snap.mixed_gradient_curve);
            if (mixed_per_part_opt)   mixed_per_part_opt->values.push_back((unsigned char)snap.mixed_gradient_per_part);
        }
    }

    update_filament_multi_color();
    update_multi_material_filament_presets();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "finish sync ams list";
    return this->filament_presets.size();
}

void PresetBundle::update_filament_multi_color()
{
    std::vector<std::string> exsit_multi_colors;
    for (auto &fil_item : ams_multi_color_filment){
        if (fil_item.empty()) break;
        if (fil_item.size() == 1)
            exsit_multi_colors.push_back(fil_item[0]);
        else {
            std::string colors = "";
            for (auto &color : fil_item){
                   colors += color + " ";
            }
            colors.erase(colors.size() - 1);
            exsit_multi_colors.push_back(colors);
        }
    }
    ConfigOptionStrings *filament_multi_colour = project_config.option<ConfigOptionStrings>("filament_multi_colour");
    filament_multi_colour->resize(exsit_multi_colors.size());
    filament_multi_colour->values = exsit_multi_colors;
}

std::vector<int> PresetBundle::get_used_tpu_filaments(const std::vector<int> &used_filaments)
{
    std::vector<int> tpu_filaments;
    for (size_t i = 0; i < this->filament_presets.size(); ++i) {
        auto iter = std::find(used_filaments.begin(), used_filaments.end(), i + 1);
        if (iter == used_filaments.end()) continue;

        std::string filament_name = this->filament_presets[i];
        for (int f_index = 0; f_index < this->filaments.size(); f_index++) {
            PresetCollection *filament_presets = &this->filaments;
            Preset           *preset           = &filament_presets->preset(f_index);
            int               size             = this->filaments.size();
            if (preset && filament_name.compare(preset->name) == 0) {
                std::string display_filament_type;
                std::string filament_type = preset->config.get_filament_type(display_filament_type);
                if (display_filament_type == "TPU") {
                    tpu_filaments.push_back(i);
                }
            }
        }
    }
    return tpu_filaments;
}

void PresetBundle::set_calibrate_printer(std::string name)
{
    if (name.empty()) {
        calibrate_filaments.clear();
        return;
    }
    if (!name.empty())
        calibrate_printer = printers.find_preset(name);
    const Preset &                printer_preset = calibrate_printer ? *calibrate_printer : printers.get_edited_preset();
    const PresetWithVendorProfile active_printer = printers.get_preset_with_vendor_profile(printer_preset);
    DynamicPrintConfig            config;
    config.set_key_value("printer_preset", new ConfigOptionString(active_printer.preset.name));
    const ConfigOption *opt = active_printer.preset.config.option("nozzle_diameter");
    if (opt) config.set_key_value("num_extruders", new ConfigOptionInt((int) static_cast<const ConfigOptionFloats *>(opt)->values.size()));
    calibrate_filaments.clear();
    for (size_t i = filaments.num_default_presets(); i < filaments.size(); ++i) {
        const Preset &                preset                          = filaments.m_presets[i];
        const PresetWithVendorProfile this_preset_with_vendor_profile = filaments.get_preset_with_vendor_profile(preset);
        bool                          is_compatible                   = is_compatible_with_printer(this_preset_with_vendor_profile, active_printer, &config);
        if (is_compatible) calibrate_filaments.insert(&preset);
    }
}

std::vector<std::vector<DynamicPrintConfig>> PresetBundle::get_extruder_filament_info() const
{
    std::vector<std::vector<DynamicPrintConfig>> filament_infos;
    int extruder_nums = get_printer_extruder_count();
    if (extruder_nums > 1) {
        filament_infos.resize(extruder_nums, std::vector<DynamicPrintConfig>());
        for (auto ams_item : filament_ams_list) {
            if (ams_item.first & 0x10000) {
                filament_infos[1].push_back(ams_item.second);
            } else {
                filament_infos[0].push_back(ams_item.second);
            }
        }
    }
    return filament_infos;
}

std::set<std::string> PresetBundle::get_printer_names_by_printer_type_and_nozzle(const std::string &printer_type, std::string nozzle_diameter_str, bool system_only)
{
    std::set<std::string> printer_names;
    if ("0.0" == nozzle_diameter_str || nozzle_diameter_str.empty()) {
        nozzle_diameter_str = "0.4";
    }
    std::ostringstream    stream;

    for (auto printer_it = this->printers.begin(); printer_it != this->printers.end(); printer_it++) {
        if (system_only && !printer_it->is_system) continue;

        ConfigOption *      printer_model_opt = printer_it->config.option("printer_model");
        ConfigOptionString *printer_model_str = dynamic_cast<ConfigOptionString *>(printer_model_opt);
        if (!printer_model_str) continue;

        if (printer_model_str->value != printer_type) continue;

        if (printer_it->name.find(nozzle_diameter_str) != std::string::npos) printer_names.insert(printer_it->name);
    }

    assert(printer_names.size() == 1);

    for (auto& printer_name : printer_names) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << " printer name: " << printer_name;
    }

    return printer_names;
}

std::vector<Preset *> PresetBundle::get_filament_presets_for_machine(const std::string &printer_type,
                                                                    const std::string &nozzle_diameter_str,
                                                                    bool               include_user_presets)
{
    const std::set<std::string> printer_names = get_printer_names_by_printer_type_and_nozzle(printer_type, nozzle_diameter_str);
    const Preset *printer = printer_names.empty() ? nullptr : printers.find_preset(*printer_names.begin());
    if (printer == nullptr)
        return {};

    const PresetWithVendorProfile active_printer = printers.get_preset_with_vendor_profile(*printer);
    DynamicPrintConfig printer_config;
    printer_config.set_key_value("printer_preset", new ConfigOptionString(printer->name));
    if (const ConfigOption *opt = printer->config.option("nozzle_diameter"))
        printer_config.set_key_value("num_extruders", new ConfigOptionInt((int) static_cast<const ConfigOptionFloats *>(opt)->values.size()));

    std::vector<Preset *> compatible;
    for (Preset &preset : filaments) {
        if (filaments.get_preset_base(preset) != &preset || (!preset.is_system && !include_user_presets))
            continue;
        if (is_compatible_with_printer(filaments.get_preset_with_vendor_profile(preset), active_printer, &printer_config))
            compatible.push_back(&preset);
    }
    return compatible;
}

int PresetBundle::get_filament_variant_index(const DynamicPrintConfig &filament_config, const DynamicPrintConfig &printer_config,
                                             int extruder_id, NozzleVolumeType nozzle_volume_type)
{
    const auto        *extruder_types = printer_config.option<ConfigOptionEnumsGeneric>("extruder_type");
    const ExtruderType extruder_type  = extruder_types && !extruder_types->empty() ? ExtruderType(extruder_types->get_at(extruder_id)) : etDirectDrive;
    return std::max(0, filament_config.get_index_for_extruder(1, "", extruder_type, nozzle_volume_type, "filament_extruder_variant"));
}

bool PresetBundle::check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(
    const std::string &printer_type, std::string& nozzle_diameter_str, std::string &setting_id, std::string &tag_uid, std::string &nozzle_temp_min, std::string &nozzle_temp_max, std::string& preset_setting_id,
    int extruder_id, NozzleVolumeType nozzle_volume_type)
{
    bool is_equation = true;

    std::map<std::string, std::vector<Preset const *>> filament_list = filaments.get_filament_presets();
    std::set<std::string> printer_names       = get_printer_names_by_printer_type_and_nozzle(printer_type, nozzle_diameter_str);

    auto filament_iter = filament_list.find(setting_id);
    if (filament_iter == filament_list.end())
        return is_equation;

    for (const Preset *preset : filament_iter->second) {
        if (tag_uid == "0" || (tag_uid.size() == 16 && tag_uid.substr(12, 2) == "01")) continue;
        if (preset && !preset->is_user()) continue;
        ConfigOption *       printer_opt  = const_cast<Preset *>(preset)->config.option("compatible_printers");
        ConfigOptionStrings *printer_strs = dynamic_cast<ConfigOptionStrings *>(printer_opt);
        bool                 compared = false;
        for (const std::string &printer_str : printer_strs->values) {
            if (printer_names.find(printer_str) != printer_names.end()) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << "nozzle temp matching: preset name: " << preset->name << " printer name: " << printer_str;
                if (!compared) {
                    compared                        = true;
                    const Preset *printer           = printers.find_preset(printer_str);
                    const int     variant_index     = printer ? get_filament_variant_index(preset->config, printer->config, extruder_id, nozzle_volume_type) : 0;
                    bool          min_temp_equation = false, max_temp_equation = false;
                    int           min_nozzle_temp = std::stoi(nozzle_temp_min);
                    int           max_nozzle_temp = std::stoi(nozzle_temp_max);
                    ConfigOption *opt_min         = const_cast<Preset *>(preset)->config.option("nozzle_temperature_range_low");
                    if (opt_min) {
                        ConfigOptionInts *opt_min_ints = dynamic_cast<ConfigOptionInts *>(opt_min);
                        min_nozzle_temp                = opt_min_ints->get_at(variant_index);
                        if (std::to_string(min_nozzle_temp) == nozzle_temp_min)
                            min_temp_equation = true;
                        else {
                            BOOST_LOG_TRIVIAL(info) << "tray min temp: " << nozzle_temp_min << " preset min temp: " << min_nozzle_temp;
                            nozzle_temp_min = std::to_string(min_nozzle_temp);
                        }
                    }
                    ConfigOption *opt_max = const_cast<Preset *>(preset)->config.option("nozzle_temperature_range_high");
                    if (opt_max) {
                        ConfigOptionInts *opt_max_ints = dynamic_cast<ConfigOptionInts *>(opt_max);
                        max_nozzle_temp                = opt_max_ints->get_at(variant_index);
                        if (std::to_string(max_nozzle_temp) == nozzle_temp_max)
                            max_temp_equation = true;
                        else {
                            BOOST_LOG_TRIVIAL(info) << "tray max temp: " << nozzle_temp_max << " preset min temp: " << max_nozzle_temp;
                            nozzle_temp_max = std::to_string(max_nozzle_temp);
                        }
                    }
                    if (min_temp_equation && max_temp_equation) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << "Determine if the temperature has changed: no changed";
                    } else {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << "Determine if the temperature has changed: has changed";
                        preset_setting_id = preset->setting_id;
                        is_equation = false;
                    }
                } else {
                    assert(false);
                }
            }
        }
    }
    return is_equation;
}

Preset *PresetBundle::get_similar_printer_preset(std::string printer_model, std::string printer_variant)
{
    if (printer_model.empty())
        printer_model = printers.get_selected_preset().config.opt_string("printer_model");
    if (printer_model.empty())
        return nullptr;
    auto printer_variant_old = printers.get_selected_preset().config.opt_string("printer_variant");
    std::map<std::string, Preset*> printer_presets;
    for (auto &preset : printers.m_presets) {
        if (printer_variant.empty() && !preset.is_system)
            continue;
        if (preset.config.opt_string("printer_model") == printer_model)
            printer_presets.insert({preset.name, &preset});
    }
    if (printer_presets.empty())
        return nullptr;
    auto prefer_printer = printers.get_selected_preset().alias;

    if (!printer_variant.empty())
        boost::replace_all(prefer_printer, printer_variant_old, printer_variant);
    else if (auto n = prefer_printer.find(printer_variant_old); n != std::string::npos)
        prefer_printer = printer_model + " " + printer_variant_old + prefer_printer.substr(n + printer_variant_old.length());
    if (auto iter = printer_presets.find(prefer_printer); iter != printer_presets.end()) {
        return iter->second;
    }
    if (printer_variant.empty())
        printer_variant = printer_variant_old;
    for (auto& preset : printer_presets) {
        if (preset.second->config.opt_string("printer_variant") == printer_variant)
            return preset.second;
    }
    return printer_presets.begin()->second;
}

bool PresetBundle::is_the_only_edited_filament(unsigned int filament_index)
{
    unsigned n = this->filament_presets.size();
    if (filament_index >= n)
        return false;

    std::string name = this->filament_presets[filament_index];
    Preset& edited_preset = this->filaments.get_edited_preset();
    if (edited_preset.name != name)
        return false;

    unsigned index = 0;
    while (index < n)
    {
        if (index == filament_index) {
            index ++;
            continue;
        }
        std::string filament_preset = this->filament_presets[index];
        if (edited_preset.name == filament_preset)
            return false;
        else
            index ++;
    }
    return true;
}

void PresetBundle::reset_default_nozzle_volume_type()
{
    Preset& current_printer = this->printers.get_edited_preset();
    this->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values = current_printer.config.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type")->values;
}

int PresetBundle::get_printer_extruder_count() const
{
    const Preset& printer_preset = this->printers.get_edited_preset();

    const auto* nozzle_diameter = printer_preset.config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzle_diameter == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": nozzle_diameter is missing, using 1 extruder";
        return 1;
    }
    if (nozzle_diameter->values.empty()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": nozzle_diameter is empty, using 1 extruder";
        return 1;
    }

    int count = int(nozzle_diameter->values.size());

    return count;
}

void PresetBundle::update_filament_count()
{
    if (printers.get_edited_preset().printer_technology() != ptFFF)
        return;
    const size_t num_extruders = static_cast<size_t>(get_printer_extruder_count());
    if (filament_presets.size() >= num_extruders)
        return;
    filament_presets.resize(num_extruders, filament_presets.empty()
        ? filaments.first_visible().name
        : filament_presets.back());
}

bool PresetBundle::support_different_extruders() const
{
    const Preset& printer_preset = this->printers.get_edited_preset();
    int extruder_count;
    bool supported = printer_preset.config.support_different_extruders(extruder_count);

    return supported;
}

std::vector<int> PresetBundle::get_default_nozzle_volume_types_for_filaments(std::vector<int>& f_maps)
{
    std::vector<int> result;
    int filament_count = f_maps.size();
    result.resize(filament_count, static_cast<int>(NozzleVolumeType::nvtStandard));

    auto opt_nozzle_volume_type = dynamic_cast<const ConfigOptionEnumsGeneric*>(this->project_config.option("nozzle_volume_type"));
    for (int index = 0; index < filament_count; index++)
    {
        if (opt_nozzle_volume_type && opt_nozzle_volume_type->values.size() > (f_maps[index] - 1))
            result[index] = opt_nozzle_volume_type->values[f_maps[index] - 1];
    }

    return result;
}

DynamicPrintConfig PresetBundle::full_config(bool apply_extruder, std::optional<std::vector<int>>filament_maps, std::optional<std::vector<int>> filament_volume_maps) const
{
    return (this->printers.get_edited_preset().printer_technology() == ptFFF) ?
        this->full_fff_config(apply_extruder, filament_maps, filament_volume_maps) :
        this->full_sla_config();
}

DynamicPrintConfig PresetBundle::full_config_secure(std::optional<std::vector<int>>filament_maps) const
{
    DynamicPrintConfig config = this->full_fff_config(false, filament_maps);
    config.erase("print_host");
    config.erase("print_host_webui");
    config.erase("printhost_apikey");
    config.erase("printhost_cafile");    
    config.erase("printhost_user");    
    config.erase("printhost_password");    
    config.erase("printhost_port");
    return config;
}

std::vector<std::vector<std::vector<float>>> PresetBundle::get_full_flush_matrix(bool with_multiplier) const
{
    auto full_config = this->full_config();
    int extruder_nums = full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    std::vector<double> flush_volume_value = full_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    int filament_nums = full_config.option<ConfigOptionStrings>("filament_type")->values.size();

    std::vector<std::vector<std::vector<float>>> matrix;
    for (size_t extruder_id = 0; extruder_id < extruder_nums; ++extruder_id) {
        std::vector<float>              flush_matrix(cast<float>(get_flush_volumes_matrix(flush_volume_value, extruder_id, extruder_nums, filament_nums)));
        std::vector<std::vector<float>> wipe_volumes;
        for (unsigned int i = 0; i < filament_nums; ++i)
            wipe_volumes.push_back(std::vector<float>(flush_matrix.begin() + i * filament_nums, flush_matrix.begin() + (i + 1) * filament_nums));

        matrix.emplace_back(wipe_volumes);
    }

    if (with_multiplier) {
        auto* mode_opt = project_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode");
        const bool use_fast = mode_opt && mode_opt->value == PrimeVolumeMode::pvmFast;
        auto* mult_opt = project_config.option<ConfigOptionFloats>(use_fast ? "flush_multiplier_fast" : "flush_multiplier");
        auto flush_multiplies = mult_opt ? mult_opt->values : project_config.option<ConfigOptionFloats>("flush_multiplier")->values;
        flush_multiplies.resize(extruder_nums, 1);
        for (size_t extruder_id = 0; extruder_id < extruder_nums; ++extruder_id) {
            for (auto& vec : matrix[extruder_id]) {
                for (auto& v : vec)
                    v *= flush_multiplies[extruder_id];
            }
        }
    }

    return matrix;
}

const std::set<std::string> ignore_settings_list ={
    "inherits",
    "print_settings_id", "filament_settings_id", "printer_settings_id"
};

std::set<std::string> PresetBundle::project_different_keys(const std::string &different_settings)
{
    std::vector<std::string> keys;
    Slic3r::unescape_strings_cstyle(different_settings, keys);
    std::set<std::string> keys_set(keys.begin(), keys.end());
    keys_set.insert(ignore_settings_list.begin(), ignore_settings_list.end());
    return keys_set;
}

DynamicPrintConfig PresetBundle::full_fff_config(bool apply_extruder, std::optional<std::vector<int>> filament_maps_new, std::optional<std::vector<int>> filament_volume_maps_new) const
{
    DynamicPrintConfig out;
    out.apply(FullPrintConfig::defaults());
    out.apply(this->prints.get_edited_preset().config);
	out.apply(this->filaments.default_preset().config);
	out.apply(this->printers.get_edited_preset().config);
    out.apply(this->project_config);

    size_t  num_filaments = this->filament_presets.size();

    std::vector<int> filament_maps = out.option<ConfigOptionInts>("filament_map")->values;
    std::vector<int> filament_volume_maps(num_filaments, (int)nvtStandard);

    ConfigOptionInts* filament_volume_map_opt = out.option<ConfigOptionInts>("filament_volume_map");
    if (filament_maps_new.has_value())
        filament_maps = *filament_maps_new;
    if (filament_volume_maps_new.has_value()) {
        filament_volume_maps = *filament_volume_maps_new;
        out.option<ConfigOptionInts>("filament_volume_map", true)->values = filament_volume_maps;
    }
    else if (filament_volume_map_opt && filament_volume_map_opt->values.size() == num_filaments)
        filament_volume_maps = filament_volume_map_opt->values;
    if (filament_maps.size() != num_filaments) {
        filament_maps.resize(num_filaments, 1);
    }
    else {
        assert(filament_maps.size() == num_filaments);
    }
    if (filament_volume_maps.size() != num_filaments) {
        filament_volume_maps.resize(num_filaments, nvtStandard);
    }

    auto* extruder_diameter = dynamic_cast<const ConfigOptionFloats*>(out.option("nozzle_diameter"));
    std::vector<std::string> compatible_printers_condition;
    std::vector<std::string> compatible_prints_condition;
    std::vector<std::string> inherits;
    std::vector<std::string> filament_ids;
    std::vector<std::string> print_compatible_printers;
    std::vector<std::string> different_settings;
    std::string different_print_settings, different_printer_settings;
    compatible_printers_condition.emplace_back(this->prints.get_edited_preset().compatible_printers_condition());

    const ConfigOptionStrings* compatible_printers =  (const_cast<PresetBundle*>(this))->prints.get_edited_preset().config.option<ConfigOptionStrings>("compatible_printers", false);
    if (compatible_printers)
        print_compatible_printers = compatible_printers->values;
    std::string print_inherits = this->prints.get_edited_preset().inherits();
    inherits                     .emplace_back(print_inherits);
    const Preset* print_parent_preset =  this->prints.get_selected_preset_parent();
    if (print_parent_preset) {
        std::vector<std::string> dirty_options = this->prints.dirty_options_without_option_list(&(this->prints.get_edited_preset()), print_parent_preset, ignore_settings_list, false);
        if (!dirty_options.empty()) {
            different_print_settings = Slic3r::escape_strings_cstyle(dirty_options);
        }
    }
    different_settings.emplace_back(different_print_settings);

    std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
    int extruder_count = 1, extruder_volume_type_count = 1;
    bool different_extruder = false;
    if (apply_extruder) {
        different_extruder = out.support_different_extruders(extruder_count);
        extruder_volume_type_count = out.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        if ((extruder_count > 1) || different_extruder) {
            out.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, printer_options_with_variant_1, "printer_extruder_id", "printer_extruder_variant");
            out.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, printer_options_with_variant_2, "printer_extruder_id", "printer_extruder_variant", 2);
            out.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, print_options_with_variant, "print_extruder_id", "print_extruder_variant");
        }
    }

    if (num_filaments <= 1) {
        DynamicPrintConfig filament_config = this->filaments.get_edited_preset().config;
        if (apply_extruder && ((extruder_count > 1) || different_extruder || filament_config.has_multi_variant_filament()))
            filament_config.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, filament_options_with_variant, "", "filament_extruder_variant", 1, filament_maps[0], (NozzleVolumeType)filament_volume_maps[0]);
        out.apply(filament_config);
        compatible_printers_condition.emplace_back(this->filaments.get_edited_preset().compatible_printers_condition());
        compatible_prints_condition  .emplace_back(this->filaments.get_edited_preset().compatible_prints_condition());
        std::string current_preset_name = this->filament_presets[0];
        const Preset* preset = this->filaments.find_preset(current_preset_name, true);
        std::string filament_inherits = preset->inherits();
        inherits                     .emplace_back(filament_inherits);
        filament_ids.emplace_back(this->filaments.get_edited_preset().filament_id);

        std::string different_filament_settings;
        const Preset* filament_parent_preset =  this->filaments.get_selected_preset_parent();
        if (filament_parent_preset) {
            std::vector<std::string> dirty_options = this->filaments.dirty_options_without_option_list(&(this->filaments.get_edited_preset()), filament_parent_preset, ignore_settings_list, false);
            if (!dirty_options.empty()) {
                different_filament_settings = Slic3r::escape_strings_cstyle(dirty_options);
            }
        }

        different_settings.emplace_back(different_filament_settings);

        std::vector<int>& filament_self_indice = out.option<ConfigOptionInts>("filament_self_index", true)->values;
        int index_size = out.option<ConfigOptionStrings>("filament_extruder_variant")->size();
        filament_self_indice.resize(index_size, 1);
    } else {
        std::vector<const DynamicPrintConfig*> filament_configs;
        std::vector<const Preset*> filament_presets;
        for (const std::string& filament_preset_name : this->filament_presets) {
            const Preset* preset = this->filaments.find_preset(filament_preset_name, true);
            filament_presets.emplace_back(preset);
            filament_configs.emplace_back(&(preset->config));
        }
        while (filament_configs.size() < num_filaments) {
            const Preset* preset = &this->filaments.first_visible();
            filament_presets.emplace_back(preset);
            filament_configs.emplace_back(&(preset->config));
        }
        for (int index = 0; index < num_filaments; index++) {
            const DynamicPrintConfig *cfg = filament_configs[index];
            const Preset *preset = filament_presets[index];
            DynamicPrintConfig &cfg_rw = *const_cast<DynamicPrintConfig*>(cfg);
            compatible_printers_condition.emplace_back(Preset::compatible_printers_condition(cfg_rw));
            compatible_prints_condition  .emplace_back(Preset::compatible_prints_condition(cfg_rw));

            std::string filament_inherits = Preset::inherits(cfg_rw);
            inherits                     .emplace_back(filament_inherits);
            filament_ids.emplace_back(preset->filament_id);
            std::string different_filament_settings;

            const Preset* filament_parent_preset = nullptr;
            if (preset->is_system || preset->is_default) {
                bool is_selected = this->filaments.get_selected_preset_name() == preset->name;
                if (is_selected) {
                    filament_parent_preset = const_cast<PresetBundle*>(this)->filaments.find_preset(preset->name, false, true);
                }
                else {
                    filament_parent_preset = preset;
                }
            }
            else if (!filament_inherits.empty())
                filament_parent_preset =  const_cast<PresetBundle*>(this)->filaments.find_preset(filament_inherits, false, true);

            if (filament_parent_preset) {
                std::vector<std::string> dirty_options = cfg_rw.diff(filament_parent_preset->config);
                if (!dirty_options.empty()) {
                    auto iter = dirty_options.begin();
                    while (iter != dirty_options.end()) {
                        if (ignore_settings_list.find(*iter) != ignore_settings_list.end()) {
                            iter = dirty_options.erase(iter);
                        }
                        else {
                            ++iter;
                        }
                    }
                    different_filament_settings = Slic3r::escape_strings_cstyle(dirty_options);
                }
            }

            different_settings.emplace_back(different_filament_settings);
        }

        std::vector<DynamicPrintConfig> filament_temp_configs;
        filament_temp_configs.resize(num_filaments);
        for (size_t i = 0; i < num_filaments; ++i) {
            filament_temp_configs[i] = *(filament_configs[i]);
            if (apply_extruder && ((extruder_count > 1) || different_extruder || filament_temp_configs[i].has_multi_variant_filament()))
                filament_temp_configs[i].update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, filament_options_with_variant, "", "filament_extruder_variant", 1, filament_maps[i], (NozzleVolumeType)filament_volume_maps[i]);
        }

        std::vector<int> filament_variant_count(num_filaments, 1);
        for (const t_config_option_key &key : this->filaments.default_preset().config.keys()) {
			if (key == "compatible_prints" || key == "compatible_printers")
				continue;
            ConfigOption *opt_dst = out.option(key, false);
            if (opt_dst->is_scalar()) {
                const ConfigOption *opt_src = filament_temp_configs.front().option(key);
                if (opt_src != nullptr)
                    opt_dst->set(opt_src);
            } else {
                ConfigOptionVectorBase* opt_vec_dst = static_cast<ConfigOptionVectorBase*>(opt_dst);
                {
                    if (apply_extruder) {
                        std::vector<const ConfigOption*> filament_opts(num_filaments, nullptr);
                        for (size_t i = 0; i < filament_opts.size(); ++i)
                            filament_opts[i] = filament_temp_configs[i].option(key);
                        opt_vec_dst->set(filament_opts);
                    }
                    else {
                        for (size_t i = 0; i < num_filaments; ++i) {
                            const ConfigOptionVectorBase* filament_option = static_cast<const ConfigOptionVectorBase*>(filament_temp_configs[i].option(key));
                            if (i == 0)
                                opt_vec_dst->set(filament_option);
                            else
                                opt_vec_dst->append(filament_option);

                            if (key == "filament_extruder_variant")
                                filament_variant_count[i] = filament_option->size();
                        }
                    }
                }
            }
        }

        if (!apply_extruder) {
            std::vector<int>& filament_self_indice = out.option<ConfigOptionInts>("filament_self_index", true)->values;
            int index_size = out.option<ConfigOptionStrings>("filament_extruder_variant")->size();
            filament_self_indice.resize(index_size, 1);
            int k = 0;
            for (size_t i = 0; i < num_filaments; i++) {
                for (size_t j = 0; j < filament_variant_count[i]; j++) {
                    filament_self_indice[k++] = i + 1;
                }
            }
        }
    }

    std::string printer_inherits = this->printers.get_edited_preset().inherits();
    inherits                     .emplace_back(printer_inherits);
    const Preset* printer_parent_preset =  this->printers.get_selected_preset_parent();
    if (printer_parent_preset) {
        std::vector<std::string> dirty_options = this->printers.dirty_options_without_option_list(&(this->printers.get_edited_preset()), printer_parent_preset, ignore_settings_list, false);
        if (!dirty_options.empty()) {
            different_printer_settings = Slic3r::escape_strings_cstyle(dirty_options);
        }
    }
    different_settings.emplace_back(different_printer_settings);

    out.erase("compatible_prints");
    out.erase("compatible_prints_condition");
    out.erase("compatible_printers");
    out.erase("compatible_printers_condition");
    out.erase("inherits");
    out.erase("different_settings_to_system");

    static const char* keys[] = {"support_filament", "support_interface_filament", "wipe_tower_filament"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++ i) {
        std::string key = std::string(keys[i]);
        auto *opt = dynamic_cast<ConfigOptionInt*>(out.option(key, false));
        assert(opt != nullptr);
        opt->value = boost::algorithm::clamp<int>(opt->value, 0, int(num_filaments));
    }

    static const char* keys_with_default[] = {
        "outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
        "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id"
    };
    for (size_t i = 0; i < sizeof(keys_with_default) / sizeof(keys_with_default[0]); ++ i) {
        std::string key = std::string(keys_with_default[i]);
        auto *opt = dynamic_cast<ConfigOptionInt*>(out.option(key, false));
        assert(opt != nullptr);
        if(opt->value < 0 || opt->value > int(num_filaments))
            opt->value = 0;
    }
    out.option<ConfigOptionString >("print_settings_id",    true)->value  = this->prints.get_selected_preset_name();
    out.option<ConfigOptionStrings>("filament_settings_id", true)->values = this->filament_presets;
    out.option<ConfigOptionString >("printer_settings_id",  true)->value  = this->printers.get_selected_preset_name();
    out.option<ConfigOptionStrings>("filament_ids", true)->values = filament_ids;
    out.option<ConfigOptionInts>("filament_map", true)->values = filament_maps;
    auto add_if_some_non_empty = [&out](std::vector<std::string> &&values, const std::string &key) {
        bool nonempty = false;
        for (const std::string &v : values)
            if (! v.empty()) {
                nonempty = true;
                break;
            }
        if (nonempty)
            out.set_key_value(key, new ConfigOptionStrings(std::move(values)));
    };
    add_if_some_non_empty(std::move(compatible_printers_condition), "compatible_machine_expression_group");
    add_if_some_non_empty(std::move(compatible_prints_condition),   "compatible_process_expression_group");
    add_if_some_non_empty(std::move(inherits),                      "inherits_group");
    add_if_some_non_empty(std::move(different_settings),            "different_settings_to_system");
    add_if_some_non_empty(std::move(print_compatible_printers),     "print_compatible_printers");
    out.option<ConfigOptionStrings>("extruder_ams_count", true)->values   = save_extruder_ams_count_to_string(this->extruder_ams_counts);

	out.option<ConfigOptionEnumGeneric>("printer_technology", true)->value = ptFFF;
    return out;
}

DynamicPrintConfig PresetBundle::full_sla_config() const
{
    DynamicPrintConfig out;
    out.apply(SLAFullPrintConfig::defaults());
    out.apply(this->sla_prints.get_edited_preset().config);
    out.apply(this->sla_materials.get_edited_preset().config);
    out.apply(this->printers.get_edited_preset().config);
    std::vector<std::string> compatible_printers_condition;
	std::vector<std::string> compatible_prints_condition;
    std::vector<std::string> inherits;
    compatible_printers_condition.emplace_back(this->sla_prints.get_edited_preset().compatible_printers_condition());
	inherits					 .emplace_back(this->sla_prints.get_edited_preset().inherits());
    compatible_printers_condition.emplace_back(this->sla_materials.get_edited_preset().compatible_printers_condition());
	compatible_prints_condition  .emplace_back(this->sla_materials.get_edited_preset().compatible_prints_condition());
    inherits                     .emplace_back(this->sla_materials.get_edited_preset().inherits());
    inherits                     .emplace_back(this->printers.get_edited_preset().inherits());

    out.erase("compatible_printers");
    out.erase("compatible_printers_condition");
    out.erase("inherits");

    out.option<ConfigOptionString >("sla_print_settings_id",    true)->value  = this->sla_prints.get_selected_preset_name();
    out.option<ConfigOptionString >("sla_material_settings_id", true)->value  = this->sla_materials.get_selected_preset_name();
    out.option<ConfigOptionString >("printer_settings_id",      true)->value  = this->printers.get_selected_preset_name();

    auto add_if_some_non_empty = [&out](std::vector<std::string> &&values, const std::string &key) {
        bool nonempty = false;
        for (const std::string &v : values)
            if (! v.empty()) {
                nonempty = true;
                break;
            }
        if (nonempty)
            out.set_key_value(key, new ConfigOptionStrings(std::move(values)));
    };
    add_if_some_non_empty(std::move(compatible_printers_condition), "compatible_machine_expression_group");
    add_if_some_non_empty(std::move(compatible_prints_condition),   "compatible_process_expression_group");
    add_if_some_non_empty(std::move(inherits),                      "inherits_group");

	out.option<ConfigOptionEnumGeneric>("printer_technology", true)->value = ptSLA;
	return out;
}

ConfigSubstitutions PresetBundle::load_config_file(const std::string &path, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
	if (is_gcode_file(path)) {
		DynamicPrintConfig config;
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, gcodefile %1%, compatibility_rule %2%")%path %compatibility_rule;
		config.apply(FullPrintConfig::defaults());
        ConfigSubstitutions config_substitutions = config.load_from_gcode_file(path, compatibility_rule);
        Preset::normalize(config);
		load_config_file_config(path, true, std::move(config));
		return config_substitutions;
	}

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" can not load config file %1% not from gcode")%path ;
    throw Slic3r::RuntimeError(std::string("Unknown configuration file: ") + path);
    
    return ConfigSubstitutions{};
}


static std::map<std::string, std::map<std::string, std::string>> filament_preset_convert = {
{"Bambu Lab H2D 0.6 nozzle", {{"Bambu PLA Basic @BBL H2D", "Bambu PLA Basic @BBL H2D 0.6 nozzle"},
                              {"Bambu PLA Matte @BBL H2D", "Bambu PLA Matte @BBL H2D 0.6 nozzle"},
                              {"Bambu ABS @BBL H2D", "Bambu ABS @BBL H2D 0.6 nozzle"}}},
{"Bambu Lab H2D 0.8 nozzle", {{"Bambu PETG HF @BBL H2D 0.6 nozzle", "Bambu PETG HF @BBL H2D 0.8 nozzle"},
                              {"Bambu ASA @BBL H2D 0.6 nozzle", "Bambu ASA @BBL H2D 0.8 nozzle"}}}
};

static void apply_mixed_config_relocations(DynamicPrintConfig&                            config,
                                           const std::string&                             key,
                                           const std::vector<std::pair<size_t, size_t>>&  moves)
{
    ConfigOption* opt = config.optptr(key);
    if (opt == nullptr || moves.empty())
        return;
    std::unique_ptr<ConfigOption> snapshot(opt->clone());
    switch (opt->type()) {
    case coBools: {
        auto*       live   = static_cast<ConfigOptionBools*>(opt);
        const auto* frozen = static_cast<const ConfigOptionBools*>(snapshot.get());
        for (const auto& [from, to] : moves) {
            const unsigned char cell = from < frozen->values.size() ? frozen->values[from] : 0;
            if (live->values.size() <= to)
                live->values.resize(to + 1, 0);
            live->values[to] = cell;
        }
        break;
    }
    case coStrings: {
        auto*       live   = static_cast<ConfigOptionStrings*>(opt);
        const auto* frozen = static_cast<const ConfigOptionStrings*>(snapshot.get());
        for (const auto& [from, to] : moves) {
            const std::string cell = from < frozen->values.size() ? frozen->values[from] : std::string();
            if (live->values.size() <= to)
                live->values.resize(to + 1, std::string{});
            live->values[to] = cell;
        }
        break;
    }
    default: break;
    }
}

static void apply_receiver_mix_relocations(DynamicPrintConfig&                            config,
                                           std::vector<std::vector<std::string>>&          ams_multi_color_filment,
                                           const std::vector<std::pair<size_t, size_t>>&  moves)
{
    if (moves.empty())
        return;

    auto move_bools  = [&](const char* key, bool clear_source) {
        ConfigOption* opt = config.optptr(key);
        if (opt == nullptr)
            return;
        auto*             live   = static_cast<ConfigOptionBools*>(opt);
        std::unique_ptr<ConfigOption> snapshot(opt->clone());
        const auto*       frozen  = static_cast<const ConfigOptionBools*>(snapshot.get());
        for (const auto& [from, to] : moves) {
            const bool cell = from < frozen->values.size() ? frozen->values[from] : false;
            if (live->values.size() <= to)
                live->values.resize(to + 1, false);
            live->values[to] = cell;
            if (clear_source && from < live->values.size())
                live->values[from] = false;
        }
    };
    auto move_strings = [&](const char* key, bool clear_source) {
        ConfigOption* opt = config.optptr(key);
        if (opt == nullptr)
            return;
        auto*             live   = static_cast<ConfigOptionStrings*>(opt);
        std::unique_ptr<ConfigOption> snapshot(opt->clone());
        const auto*       frozen  = static_cast<const ConfigOptionStrings*>(snapshot.get());
        for (const auto& [from, to] : moves) {
            const std::string cell = from < frozen->values.size() ? frozen->values[from] : std::string();
            if (live->values.size() <= to)
                live->values.resize(to + 1, std::string{});
            live->values[to] = cell;
            if (clear_source && from < live->values.size())
                live->values[from] = std::string{};
        }
    };
    auto move_ints = [&](const char* key) {
        ConfigOption* opt = config.optptr(key);
        if (opt == nullptr)
            return;
        auto*             live   = static_cast<ConfigOptionInts*>(opt);
        std::unique_ptr<ConfigOption> snapshot(opt->clone());
        const auto*       frozen  = static_cast<const ConfigOptionInts*>(snapshot.get());
        for (const auto& [from, to] : moves) {
            const int cell = from < frozen->values.size() ? frozen->values[from] : 0;
            if (live->values.size() <= to)
                live->values.resize(to + 1, 0);
            live->values[to] = cell;
        }
    };

    move_bools("filament_is_mixed", true);
    move_strings("filament_mixed_components", true);
    move_strings("filament_mixed_sublayer_ratios", true);
    move_bools("filament_mixed_gradient", true);
    move_strings("filament_mixed_gradient_range", true);
    move_strings("filament_mixed_gradient_curve", true);
    move_bools("filament_mixed_gradient_per_part", true);
    move_strings("filament_colour", false);
    move_strings("filament_multi_colour", false);
    move_strings("filament_colour_type", false);
    move_ints("filament_map");
    move_ints("filament_nozzle_map");
    move_ints("filament_volume_map");
    {
        const std::vector<std::vector<std::string>> frozen = ams_multi_color_filment;
        for (const auto& [from, to] : moves) {
            const std::vector<std::string> cell = from < frozen.size() ? frozen[from] : std::vector<std::string>();
            if (ams_multi_color_filment.size() <= to)
                ams_multi_color_filment.resize(to + 1, std::vector<std::string>{});
            ams_multi_color_filment[to] = cell;
        }
    }
}


void PresetBundle::convert_filament_preset_name(const std::string& machine_name, std::string& filament_name)
{
    auto machine_iter = filament_preset_convert.find(machine_name);
    if (machine_iter != filament_preset_convert.end())
    {
        std::map<std::string, std::string>& filament_maps = machine_iter->second;
        auto filament_iter = filament_maps.find(filament_name);
        if (filament_iter != filament_maps.end())
        {
            filament_name = filament_iter->second;
        }
    }
}
void PresetBundle::load_config_file_config(const std::string &name_or_path, bool is_external, DynamicPrintConfig &&config, Semver file_version, bool selected, PublishedConfig *published_config)
{
    PrinterTechnology printer_technology = Preset::printer_technology(config);

    const bool is_published = published_config != nullptr && published_config->published;

    auto clear_compatible_printers = [](DynamicPrintConfig& config){
        ConfigOption *opt_compatible = config.optptr("compatible_printers");
        if (opt_compatible != nullptr) {
            assert(opt_compatible->type() == coStrings);
            if (opt_compatible->type() == coStrings)
                static_cast<ConfigOptionStrings*>(opt_compatible)->values.clear();
        }
    };
    clear_compatible_printers(config);

    config.erase("enable_filament_dynamic_map");

#if 0
    size_t num_extruders = (printer_technology == ptFFF) ?
        std::min(config.option<ConfigOptionFloats>("nozzle_diameter"  )->values.size(),
                 config.option<ConfigOptionFloats>("filament_diameter")->values.size()) :
        1;
#else
    ConfigOptionStrings* filament_colour_option = config.option<ConfigOptionStrings>("filament_colour");
    size_t num_filaments = filament_colour_option?filament_colour_option->size():0;
    if (num_filaments == 0)
        throw Slic3r::RuntimeError(std::string("Invalid configuration file: ") + name_or_path);
#endif

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": , name_or_path %1%, is_external %2%, num_filaments %3%") % name_or_path % is_external % num_filaments;
    std::vector<std::string> compatible_printers_condition_values   = std::move(config.option<ConfigOptionStrings>("compatible_machine_expression_group", true)->values);
    std::vector<std::string> compatible_prints_condition_values     = std::move(config.option<ConfigOptionStrings>("compatible_process_expression_group",   true)->values);
    std::vector<std::string> inherits_values                        = std::move(config.option<ConfigOptionStrings>("inherits_group", true)->values);
    std::vector<std::string> filament_ids                           = std::move(config.option<ConfigOptionStrings>("filament_ids", true)->values);
    std::vector<std::string> print_compatible_printers              = std::move(config.option<ConfigOptionStrings>("print_compatible_printers", true)->values);
    bool has_different_settings_to_system                           = config.option("different_settings_to_system")?true:false;
    std::vector<std::string> different_values                       = std::move(config.option<ConfigOptionStrings>("different_settings_to_system", true)->values);
    std::string &compatible_printers_condition  = Preset::compatible_printers_condition(config);
    std::string &compatible_prints_condition    = Preset::compatible_prints_condition(config);
    std::string &inherits                       = Preset::inherits(config);
    compatible_printers_condition_values.resize(num_filaments + 2, std::string());
    compatible_prints_condition_values.resize(num_filaments, std::string());
    inherits_values.resize(num_filaments + 2, std::string());
    different_values.resize(num_filaments + 2, std::string());
    filament_ids.resize(num_filaments, std::string());
	switch (printer_technology) {
	case ptFFF:
		config.option<ConfigOptionString>("default_print_profile", true);
        config.option<ConfigOptionStrings>("default_filament_profile", true);
		break;
	case ptSLA:
		config.option<ConfigOptionString>("default_sla_print_profile", true);
		config.option<ConfigOptionString>("default_sla_material_profile", true);
		break;
    default: break;
	}

    bool process_multi_extruder = false;
    std::vector<int> filament_variant_index;
    size_t extruder_variant_count;
    ConfigOptionInts* filament_self_index_opt = config.option<ConfigOptionInts>("filament_self_index", true);
    if (filament_self_index_opt->size() < num_filaments) {
        std::vector<int>& filament_self_indice = filament_self_index_opt->values;
        filament_self_indice.resize(num_filaments);
        for (int index = 0; index < num_filaments; index++)
            filament_self_indice[index] = index + 1;
    }
    std::vector<int> filament_self_indice = std::move(filament_self_index_opt->values);
    ConfigOptionStrings* filament_extruder_variant_opt = config.option<ConfigOptionStrings>("filament_extruder_variant");
    if (!filament_extruder_variant_opt || filament_extruder_variant_opt->size() < num_filaments) {
        std::vector<std::string>& filament_extruder_variant = config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values;
        filament_extruder_variant.resize(num_filaments, "Direct Drive Standard");
    }
    if (config.option("extruder_variant_list")) {
        size_t extruder_count = config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
        extruder_variant_count = config.option<ConfigOptionStrings>("filament_extruder_variant", true)->size();
        if ((extruder_variant_count != filament_self_indice.size())
            || (extruder_variant_count < num_filaments)) {
            assert(false);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": invalid config file %1%, can not find suitable filament_extruder_variant or filament_self_index") % name_or_path;
            throw Slic3r::RuntimeError(std::string("Invalid configuration file: ") + name_or_path);
        }
        if (num_filaments != extruder_variant_count) {
            process_multi_extruder = true;
            filament_variant_index.resize(num_filaments, 0);

            size_t cur_filament_id = 1;
            for (size_t index = 0; index < filament_self_indice.size(); index++) {
                if (filament_self_indice[index] == cur_filament_id) {
                    filament_variant_index[cur_filament_id - 1] = index;
                    cur_filament_id++;
                    if (cur_filament_id > num_filaments)
                        break;
                }
            }
        }
    }
    std::vector<std::string> extruder_ams_count = std::move(config.option<ConfigOptionStrings>("extruder_ams_count", true)->values);
    config.erase("extruder_ams_count");
    if (this->extruder_ams_counts.empty())
        this->extruder_ams_counts = get_extruder_ams_count(extruder_ams_count);


    std::string name = is_external ? boost::filesystem::path(name_or_path).filename().string() : name_or_path;

	auto load_preset =
		[&config, &inherits, &inherits_values,
         &compatible_printers_condition, &compatible_printers_condition_values,
         &compatible_prints_condition, &compatible_prints_condition_values,
         is_external, &name, &name_or_path, file_version, selected]
		(PresetCollection &presets, size_t idx, const std::string &key, const std::set<std::string> &different_keys, std::string filament_id) {
		inherits = inherits_values[idx];
		compatible_printers_condition = compatible_printers_condition_values[idx];
        if (idx > 0 && idx - 1 < compatible_prints_condition_values.size())
            compatible_prints_condition = compatible_prints_condition_values[idx - 1];
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": , name %1%, is_external %2%, inherits %3%")%name %is_external %inherits;
		if (is_external)
			presets.load_external_preset(name_or_path, name, config.opt_string(key, true), config, different_keys, PresetCollection::LoadAndSelect::Always, file_version, filament_id);
		else
            presets.load_preset(presets.path_from_name(name, inherits.empty()), name, config, selected, file_version).save(nullptr);
	};

    switch (Preset::printer_technology(config)) {
    case ptFFF:
    {
        if (!is_published) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": load print preset from print_settings_id");
            std::set<std::string> print_different_keys_set = project_different_keys(different_values[0]);
            if (!print_compatible_printers.empty()) {
                ConfigOptionStrings* compatible_printers = config.option<ConfigOptionStrings>("compatible_printers", true);
                compatible_printers->values = print_compatible_printers;
            }

            load_preset(this->prints, 0, "print_settings_id", print_different_keys_set, std::string());

            clear_compatible_printers(config);

            std::set<std::string> printer_different_keys_set = project_different_keys(different_values[num_filaments + 1]);
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": load printer preset from printer_settings_id");
            load_preset(this->printers, num_filaments + 1, "printer_settings_id", printer_different_keys_set, std::string());

            auto old_filament_profile_names = config.option<ConfigOptionStrings>("filament_settings_id", true);
            old_filament_profile_names->values.resize(num_filaments, std::string());

            auto old_machine_profile_name = config.option<ConfigOptionString>("printer_settings_id", true);

            if (num_filaments <= 1) {
                inherits                      = inherits_values[1];
                compatible_printers_condition = compatible_printers_condition_values[1];
    			compatible_prints_condition   = compatible_prints_condition_values.front();
    			Preset                *loaded = nullptr;

                std::set<std::string> filament_different_keys_set = project_different_keys(different_values[1]);

                std::string filament_id = filament_ids[0];
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": load single filament preset from filament_settings_id");
                if (is_external) {
                    if (inherits.empty())
                        convert_filament_preset_name(old_machine_profile_name->value, old_filament_profile_names->values.front());
                    else
                        convert_filament_preset_name(old_machine_profile_name->value, inherits);
                    loaded = this->filaments.load_external_preset(name_or_path, name, old_filament_profile_names->values.front(), config, filament_different_keys_set, PresetCollection::LoadAndSelect::Always, file_version, filament_id).first;
                }
                else {
    				loaded= &this->filaments.load_preset(this->filaments.path_from_name(name, inherits.empty()), name, config, true, file_version);
    				loaded->save(nullptr);
    			}
                this->filament_presets.clear();
    			this->filament_presets.emplace_back(loaded->name);
            } else {
                assert(is_external);
                std::vector<DynamicPrintConfig> configs(num_filaments, this->filaments.default_preset().config);
                for (const t_config_option_key &key : this->filaments.default_preset().config.keys()) {
                    ConfigOption *other_opt = config.option(key);
                    if (other_opt == nullptr)
                        continue;
                    if (other_opt->is_scalar()) {
                        for (size_t i = 0; i < configs.size(); ++ i)
                            configs[i].option(key, false)->set(other_opt);
                    }
                    else if (key != "compatible_printers" && key != "compatible_prints") {
                        for (size_t i = 0; i < configs.size(); ++i) {
                            if (process_multi_extruder && (filament_options_with_variant.find(key) != filament_options_with_variant.end())) {
                                ConfigOptionVectorBase* other_opt_vec = static_cast<ConfigOptionVectorBase*>(other_opt);
                                if (other_opt_vec->size() != extruder_variant_count) {
                                    other_opt_vec->resize(extruder_variant_count);
                                }
                                size_t next_index = (i < (configs.size() - 1)) ? filament_variant_index[i + 1] : extruder_variant_count;
                                static_cast<ConfigOptionVectorBase*>(configs[i].option(key, false))->set(other_opt, filament_variant_index[i], next_index - filament_variant_index[i]);
                            }
                            else
                                static_cast<ConfigOptionVectorBase*>(configs[i].option(key, false))->set_at(other_opt, 0, i);
                        }
                    }
                }
                this->filament_presets = std::vector<std::string>(configs.size());
                bool any_modified = false;
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": load multiple filament preset from filament_settings_id");
                for (int i = (int)configs.size()-1; i >= 0; i--) {
                    DynamicPrintConfig &cfg = configs[i];
                    cfg.opt_string("compatible_printers_condition", true) = compatible_printers_condition_values[i + 1];
                    cfg.opt_string("compatible_prints_condition",   true) = compatible_prints_condition_values[i];
                    cfg.opt_string("inherits", true)                      = inherits_values[i + 1];

                    std::set<std::string> filament_different_keys_set = project_different_keys(different_values[i+1]);

                    std::string filament_id = filament_ids[i];

                    std::string& filament_inherit = cfg.opt_string("inherits", true);
                    if (filament_inherit.empty() && (i < int(old_filament_profile_names->values.size())))
                        convert_filament_preset_name(old_machine_profile_name->value, old_filament_profile_names->values[i]);
                    else
                        convert_filament_preset_name(old_machine_profile_name->value, filament_inherit);
                    auto [loaded, modified] = this->filaments.load_external_preset(name_or_path, name,
                        (i < int(old_filament_profile_names->values.size())) ? old_filament_profile_names->values[i] : "",
                        std::move(cfg),
                        filament_different_keys_set,
                        i == 0 ?
                            PresetCollection::LoadAndSelect::Always :
                        any_modified ?
                            PresetCollection::LoadAndSelect::Never :
                            PresetCollection::LoadAndSelect::OnlyIfModified,
                        file_version,
                        filament_id);
                    any_modified |= modified;
                    this->filament_presets[i] = loaded->name;
                }
            }
        }
        this->project_config.apply_only(config, is_published ? s_project_options_published : s_project_options);
        if (!is_published)
            resize_mixed_filament_metadata(this->project_config, num_filaments, num_filaments);

        break;
    }
    case ptSLA:
    {
        break;
    }
    default:
        break;
    }

	this->update_compatible(PresetSelectCompatibleType::Never);
    this->update_multi_material_filament_presets();

    if (is_published) {
        std::vector<std::string> skipped_keys;
        std::set<std::string> applied_keys;
        bool material_applied = false;
        const std::set<std::string>& structural_keys = publish_structural_keys();
        const std::set<std::string>& printer_allowlist  = publishable_printer_keys();
        const std::vector<std::string>& printer_options = Preset::printer_options();
        const std::set<std::string> printer_option_set(printer_options.begin(), printer_options.end());
        std::set<std::string> contract_excluded_keys;
        auto apply_published = [&](DynamicPrintConfig& target, const std::set<std::string>* allowlist) {
            std::set<std::string> collapsed_bases;
            for (const std::string& key : published_config->published_keys) {
                if (applied_keys.count(key) != 0)
                    continue;
                const std::string base_key = publish_base_key(key);
                if (structural_keys.count(base_key) != 0)
                    continue;
                if (allowlist != nullptr && printer_option_set.count(base_key) != 0 && allowlist->count(base_key) == 0) {
                    contract_excluded_keys.insert(base_key);
                    continue;
                }
                const ConfigOption* src_opt = config.option(base_key);
                if (src_opt == nullptr)
                    continue;
                if (src_opt->is_vector()) {
                    ConfigOption* dst_opt = target.option(base_key);
                    if (dst_opt == nullptr || !dst_opt->is_vector())
                        continue;
                    if (dst_opt->type() != src_opt->type())
                        continue;
                    if (key.size() > base_key.size()) {
                        const std::string suffix = key.substr(base_key.size() + 1);
                        size_t idx               = 0;
                        bool valid               = !suffix.empty();
                        for (const char c : suffix) {
                            if (c < '0' || c > '9') {
                                valid = false;
                                break;
                            }
                            idx = idx * 10 + size_t(c - '0');
                            if (idx > 1000000) {
                                valid = false;
                                break;
                            }
                        }
                        const size_t src_size = static_cast<const ConfigOptionVectorBase*>(src_opt)->size();
                        if (!valid || idx >= src_size)
                            continue;
                        const size_t dst_size = static_cast<const ConfigOptionVectorBase*>(dst_opt)->size();
                        if (dst_size == 1) {
                            if (collapsed_bases.count(base_key) != 0)
                                continue;
                            collapsed_bases.insert(base_key);
                            static_cast<ConfigOptionVectorBase*>(dst_opt)->set_at(src_opt, 0, idx);
                        } else {
                            if (idx >= dst_size)
                                continue;
                            target.apply_only(config, {key}, true);
                        }
                    } else if (static_cast<const ConfigOptionVectorBase*>(src_opt)->size() !=
                               static_cast<const ConfigOptionVectorBase*>(dst_opt)->size()) {
                        continue;
                    } else {
                        target.apply_only(config, {key}, true);
                    }
                    applied_keys.insert(key);
                } else {
                    if (key.find('#') != std::string::npos)
                        continue;
                    const ConfigOption* dst_opt = target.option(base_key);
                    if (dst_opt == nullptr)
                        continue;
                    if (dst_opt->type() != src_opt->type())
                        continue;
                    target.apply_only(config, {key}, true);
                    applied_keys.insert(key);
                }
            }
        };
        apply_published(this->prints.get_edited_preset().config, nullptr);
        apply_published(this->printers.get_edited_preset().config, &printer_allowlist);

        {
            bool has_published_entries = false;
            size_t physical_capacity = size_t(EnforcerBlockerType::ExtruderMax);
            {
                const Preset& receiver_printer = this->printers.get_edited_preset();
                if (receiver_printer.printer_technology() == ptFFF &&
                    !receiver_printer.config.opt_bool("single_extruder_multi_material")) {
                    if (const auto* nozzle_diameter = receiver_printer.config.option<ConfigOptionFloats>("nozzle_diameter");
                        nozzle_diameter != nullptr && !nozzle_diameter->values.empty())
                        physical_capacity = nozzle_diameter->values.size();
                }
            }
            const std::set<std::string>& mixed_definitions = publish_mixed_keys();
            auto is_mixed_definition                      = [&mixed_definitions](const PublishedMaterialEntry& entry) {
                return std::any_of(entry.keys.begin(), entry.keys.end(), [&](const std::string& key) {
                    return mixed_definitions.count(publish_base_key(key)) != 0;
                });
            };
            size_t grow_target = 0;
            for (const PublishedMaterialEntry& entry : published_config->material_keys) {
                has_published_entries = true;
                if (entry.slot < 0)
                    continue;
                if (!is_mixed_definition(entry) && size_t(entry.slot) >= physical_capacity)
                    continue;
                grow_target = std::max(grow_target, size_t(entry.slot) + 1);
            }
            size_t physical_boundary = this->num_physical_filaments();
            for (const PublishedMaterialEntry& entry : published_config->material_keys)
                if (entry.slot >= 0 && !is_mixed_definition(entry) &&
                    (size_t(entry.slot) < this->filament_presets.size() || size_t(entry.slot) < physical_capacity))
                    physical_boundary = std::max(physical_boundary, size_t(entry.slot) + 1);
            size_t next_free_slot      = std::max(physical_boundary, this->filament_presets.size());
            bool   any_mixed_relocated = false;
            std::vector<std::pair<size_t, size_t>> mixed_moves;
            std::vector<std::pair<size_t, size_t>> receiver_mix_moves;
            std::set<size_t>                        swept_mix_slots;
            for (size_t slot = 0; slot < this->filament_presets.size(); ++slot) {
                if (!this->is_mixed_filament(slot))
                    continue;
                if (slot == 0)
                    continue;
                if (slot >= physical_boundary)
                    continue;
                const size_t dest = next_free_slot++;
                swept_mix_slots.insert(slot);
                receiver_mix_moves.emplace_back(slot, dest);
                any_mixed_relocated = true;
                published_config->mixed_slot_relocations.insert_or_assign(int(slot), int(dest));
                published_config->material_replacements.emplace_back("slot " + std::to_string(slot) + " -> slot " +
                                                                      std::to_string(dest) + ": mixed filament");
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": published 3MF relocated receiver mixed filament slot " << slot
                                        << " -> " << dest << " (physical-first rebalance)";
            }
            for (auto entry_it = published_config->material_keys.begin(); entry_it != published_config->material_keys.end();) {
                PublishedMaterialEntry& entry = *entry_it;
                if (entry.slot < 0) {
                    ++entry_it;
                    continue;
                }
                const bool is_payload_mix = is_mixed_definition(entry);
                bool keep_place = false;
                if (is_payload_mix)
                    keep_place = this->is_mixed_filament(size_t(entry.slot)) && swept_mix_slots.count(size_t(entry.slot)) == 0;
                else
                    keep_place = size_t(entry.slot) < this->filament_presets.size() ||
                                 size_t(entry.slot) < physical_capacity;
                if (keep_place) {
                    ++entry_it;
                    continue;
                }
                const std::string material_label = !entry.filament_id.empty()        ? entry.filament_id :
                                                   !entry.publish_type_value.empty() ? entry.publish_type_value :
                                                                                       entry.filament_type;
                if (std::max(size_t(entry.slot), next_free_slot) >= size_t(EnforcerBlockerType::ExtruderMax)) {
                    skipped_keys.emplace_back("material:" + material_label + (is_payload_mix ?
                                                                                  " (mixed filament definition: filament slot limit reached)" :
                                                                                  " (filament slot limit reached)"));
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": published 3MF " << (is_payload_mix ? "mixed filament definition" : "material")
                                               << " from slot " << entry.slot << " could not be placed: all " << next_free_slot
                                               << " slots exhausted";
                    entry_it = published_config->material_keys.erase(entry_it);
                    continue;
                }
                if (!is_payload_mix && physical_capacity < 2) {
                    skipped_keys.emplace_back("material:" + material_label + " (printer supports only " +
                                              std::to_string(physical_capacity) + " filament)");
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": published 3MF material from slot " << entry.slot
                                               << " dropped: printer supports only " << physical_capacity << " filament";
                    entry_it = published_config->material_keys.erase(entry_it);
                    continue;
                }
                const int authored_slot = entry.slot;
                const int dest_slot     = is_payload_mix ? int(std::max(size_t(authored_slot), next_free_slot)) : int(next_free_slot);
                next_free_slot          = size_t(dest_slot) + 1;
                if (is_payload_mix) {
                    if (dest_slot == authored_slot)
                        ++entry_it;
                    else {
                        entry.slot              = dest_slot;
                        any_mixed_relocated     = true;
                        mixed_moves.emplace_back(size_t(authored_slot), size_t(entry.slot));
                        published_config->mixed_slot_relocations.insert_or_assign(authored_slot, entry.slot);
                        published_config->material_replacements.emplace_back("slot " + std::to_string(authored_slot) + " -> slot " +
                                                                             std::to_string(entry.slot) + ": mixed filament");
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": published 3MF relocated mixed filament slot " << authored_slot
                                                << " -> " << entry.slot;
                        ++entry_it;
                    }
                } else {
                    entry.mixed_placeholder = true;
                    any_mixed_relocated     = true;
                    if (dest_slot != authored_slot) {
                        entry.slot = dest_slot;
                        mixed_moves.emplace_back(size_t(authored_slot), size_t(dest_slot));
                        published_config->mixed_slot_relocations.insert_or_assign(authored_slot, dest_slot);
                    }
                    published_config->material_replacements.emplace_back(
                        (dest_slot != authored_slot ?
                             "slot " + std::to_string(authored_slot) + " -> slot " + std::to_string(dest_slot) :
                             "slot " + std::to_string(dest_slot)) +
                        ": unassigned mixed filament (printer supports only " + std::to_string(physical_capacity) + " filaments)");
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": published 3MF material from slot " << authored_slot
                                            << " placed as an unassigned mixed filament at slot " << dest_slot
                                            << " (printer supports only " << physical_capacity << " filaments)";
                    ++entry_it;
                }
            }
            if (!mixed_moves.empty())
                for (const std::string& mixed_key : mixed_definitions)
                    apply_mixed_config_relocations(config, mixed_key, mixed_moves);
            if (has_published_entries) {
                const size_t target_slots = std::max({this->filament_presets.size(), std::min(grow_target, num_filaments),
                                                      any_mixed_relocated ? next_free_slot : size_t(0)});
                std::set<int> published_slots;
                for (const PublishedMaterialEntry& entry : published_config->material_keys)
                    if (entry.slot >= 0)
                        published_slots.insert(entry.slot);
                std::set<int> virtual_gap_slots;
                std::map<int, std::string> exact_name_by_slot;
                for (const PublishedMaterialEntry& entry : published_config->material_keys)
                    if (entry.slot >= 0 && !entry.preset_name.empty() && exact_name_by_slot.count(entry.slot) == 0)
                        if (const Preset* resolved = this->filaments.find_preset2(entry.preset_name, true))
                            exact_name_by_slot.emplace(entry.slot, resolved->name);
                auto resolved_name_for = [&exact_name_by_slot](int slot) -> std::string {
                    const auto it = exact_name_by_slot.find(slot);
                    return it == exact_name_by_slot.end() ? std::string() : it->second;
                };
                const size_t first_candidate = this->filaments.is_default_suppressed() ? this->filaments.num_default_presets() : 0;
                auto candidate_score = [](const Preset& candidate, const PublishedMaterialEntry& entry,
                                          const std::string& resolved_name) -> int {
                    if (!entry.preset_name.empty() && candidate.name == entry.preset_name)
                        return 5;
                    if (!resolved_name.empty() && candidate.name == resolved_name)
                        return 5;
                    if (!entry.preset_name.empty()) {
                        std::string bare_name = entry.preset_name.substr(0, entry.preset_name.find('@'));
                        boost::trim_right(bare_name);
                        if (!bare_name.empty() &&
                            (candidate.alias == entry.preset_name || candidate.name == bare_name || candidate.alias == bare_name))
                            return 4;
                    }
                    if (!entry.setting_id.empty() && candidate.setting_id == entry.setting_id)
                        return 3;
                    const ConfigOptionStrings* types   = candidate.config.opt<ConfigOptionStrings>("filament_type");
                    const ConfigOptionStrings* vendors = candidate.config.opt<ConfigOptionStrings>("filament_vendor");
                    const std::string type             = (types != nullptr && !types->values.empty()) ? types->get_at(0) : std::string();
                    const std::string vendor = (vendors != nullptr && !vendors->values.empty()) ? vendors->get_at(0) : std::string();
                    if (!entry.filament_id.empty() && candidate.filament_id == entry.filament_id)
                        return 2;
                    const std::string required_type = !entry.publish_type_value.empty() ? entry.publish_type_value :
                                                                                          normalize_filament_type(entry.filament_type);
                    if (!required_type.empty() && normalize_filament_type(type) == required_type) {
                        if (!entry.filament_vendor.empty() && vendor == entry.filament_vendor)
                            return 1;
                        return 0;
                    }
                    return -1;
                };
                while (this->filament_presets.size() < target_slots) {
                    const size_t new_slot_idx = this->filament_presets.size();
                    std::string initial_preset;
                    if (new_slot_idx >= physical_capacity && published_slots.count(static_cast<int>(new_slot_idx)) == 0)
                        virtual_gap_slots.insert(static_cast<int>(new_slot_idx));
                    if (published_slots.count(static_cast<int>(new_slot_idx)) != 0) {
                        if (!this->filament_presets.empty())
                            initial_preset = this->filament_presets.back();
                    }
                    if (initial_preset.empty())
                        initial_preset = this->filament_presets.empty() ? this->filaments.first_visible().name :
                                                                          this->filament_presets.back();
                    this->filament_presets.emplace_back(initial_preset);
                }
                auto referenced_elsewhere = [&](const std::string& preset_name, size_t except_slot) {
                    for (size_t s = 0; s < this->filament_presets.size(); ++s)
                        if (s != except_slot && this->filament_presets[s] == preset_name)
                            return true;
                    return false;
                };
                for (size_t slot = 1; slot < this->filament_presets.size(); ++slot) {
                    if (published_slots.count(static_cast<int>(slot)) == 0 || !referenced_elsewhere(this->filament_presets[slot], slot))
                        continue;
                    bool writes_preset_keys = false;
                    for (const PublishedMaterialEntry& entry : published_config->material_keys) {
                        if (entry.slot != static_cast<int>(slot))
                            continue;
                        for (const std::string& key : entry.keys)
                            if (mixed_definitions.count(publish_base_key(key)) == 0) {
                                writes_preset_keys = true;
                                break;
                            }
                        if (writes_preset_keys)
                            break;
                    }
                    if (!writes_preset_keys)
                        continue;
                    std::string replacement;
                    int best_score = -1;
                    for (const PublishedMaterialEntry& entry : published_config->material_keys) {
                        if (entry.slot != static_cast<int>(slot))
                            continue;
                        const std::string resolved_name = resolved_name_for(entry.slot);
                        auto scan                       = [&](bool compatible_only) -> std::pair<int, std::string> {
                            int best_score = -1;
                            std::string best_name;
                            for (size_t i = first_candidate; i < this->filaments.size(); ++i) {
                                const Preset& candidate = this->filaments.preset(i);
                                if (compatible_only && !candidate.is_compatible)
                                    continue;
                                const int score = candidate_score(candidate, entry, resolved_name);
                                if (score < 3 && !candidate.is_visible)
                                    continue;
                                if (referenced_elsewhere(candidate.name, size_t(-1)))
                                    continue;
                                if (score > best_score) {
                                    best_score = score;
                                    best_name  = candidate.name;
                                }
                            }
                            return {best_score, best_name};
                        };
                        const auto [compat_score, compat_name] = scan(true);
                        const auto [any_score, any_name]       = scan(false);
                        if (compat_score >= 0) {
                            best_score  = compat_score;
                            replacement = compat_name;
                        } else if (any_score >= 0) {
                            best_score  = any_score;
                            replacement = any_name;
                        }
                        if (best_score >= 0)
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": published 3MF re-pointed slot " << slot << " to " << replacement
                                                    << " (score " << best_score << ", preset_name \"" << entry.preset_name << "\", type \""
                                                    << entry.publish_type_value << "\")";
                        break;
                    }
                    if (replacement.empty()) {
                        std::string slot_family;
                        for (const PublishedMaterialEntry& entry : published_config->material_keys)
                            if (entry.slot == static_cast<int>(slot)) {
                                slot_family = !entry.publish_type_value.empty() ? entry.publish_type_value :
                                                                                  normalize_filament_type(entry.filament_type);
                                break;
                            }
                        for (size_t i = first_candidate; i < this->filaments.size(); ++i) {
                            const Preset& candidate = this->filaments.preset(i);
                            if (!candidate.is_visible || referenced_elsewhere(candidate.name, size_t(-1)))
                                continue;
                            if (!slot_family.empty()) {
                                const ConfigOptionStrings* types = candidate.config.opt<ConfigOptionStrings>("filament_type");
                                const std::string cand_type      = (types != nullptr && !types->values.empty()) ? types->get_at(0) :
                                                                                                                  std::string();
                                if (normalize_filament_type(cand_type) != slot_family)
                                    continue;
                            }
                            replacement = candidate.name;
                            break;
                        }
                    }
                    if (replacement.empty())
                        continue;
                    const std::string aliased_name = this->filament_presets[slot];
                    this->filament_presets[slot]   = replacement;
                    material_applied               = true;
                    published_config->material_replacements.emplace_back("slot " + std::to_string(slot) + ": " + aliased_name + " -> " +
                                                                         replacement);
                }
                ConfigOptionStrings* proj_colour       = this->project_config.opt<ConfigOptionStrings>("filament_colour");
                ConfigOptionStrings* proj_multi_colour = this->project_config.opt<ConfigOptionStrings>("filament_multi_colour");
                ConfigOptionStrings* proj_colour_type  = this->project_config.opt<ConfigOptionStrings>("filament_colour_type");
                ConfigOptionInts* proj_map             = this->project_config.opt<ConfigOptionInts>("filament_map");
                ConfigOptionInts* proj_nozzle_map      = this->project_config.opt<ConfigOptionInts>("filament_nozzle_map");
                ConfigOptionInts* proj_volume_map      = this->project_config.opt<ConfigOptionInts>("filament_volume_map");
                const size_t old_colour_count          = (proj_colour != nullptr) ? proj_colour->values.size() : 0;
                if (proj_colour && proj_colour->values.size() < target_slots)
                    proj_colour->resize(target_slots);
                if (proj_multi_colour && proj_multi_colour->values.size() < target_slots)
                    proj_multi_colour->values.resize(target_slots);
                if (proj_colour_type && proj_colour_type->values.size() < target_slots)
                    proj_colour_type->values.resize(target_slots);
                if (proj_map && proj_map->values.size() < target_slots)
                    proj_map->values.resize(target_slots, 1);
                if (proj_nozzle_map && proj_nozzle_map->values.size() < target_slots)
                    proj_nozzle_map->values.resize(target_slots, 0);
                if (proj_volume_map && proj_volume_map->values.size() < target_slots)
                    proj_volume_map->values.resize(target_slots, static_cast<int>(NozzleVolumeType::nvtStandard));
                if (auto* opt = this->project_config.opt<ConfigOptionBools>("filament_is_mixed"))
                    if (opt->values.size() < target_slots)
                        opt->values.resize(target_slots, false);
                if (auto* opt = this->project_config.opt<ConfigOptionStrings>("filament_mixed_components"))
                    if (opt->values.size() < target_slots)
                        opt->values.resize(target_slots, std::string{});
                if (auto* opt = this->project_config.opt<ConfigOptionStrings>("filament_mixed_sublayer_ratios"))
                    if (opt->values.size() < target_slots)
                        opt->values.resize(target_slots, std::string{});
                if (auto* opt = this->project_config.opt<ConfigOptionBools>("filament_mixed_gradient"))
                    if (opt->values.size() < target_slots)
                        opt->values.resize(target_slots, false);
                if (auto* opt = this->project_config.opt<ConfigOptionStrings>("filament_mixed_gradient_range"))
                    if (opt->values.size() < target_slots)
                        opt->values.resize(target_slots, std::string{});
                if (auto* opt = this->project_config.opt<ConfigOptionStrings>("filament_mixed_gradient_curve"))
                    if (opt->values.size() < target_slots)
                        opt->values.resize(target_slots, std::string{});
                if (auto* opt = this->project_config.opt<ConfigOptionBools>("filament_mixed_gradient_per_part"))
                    if (opt->values.size() < target_slots)
                        opt->values.resize(target_slots, false);
                if (this->ams_multi_color_filment.size() < target_slots)
                    this->ams_multi_color_filment.resize(target_slots);
                for (size_t slot = old_colour_count; slot < target_slots; ++slot) {
                    std::string seed;
                    for (const PublishedMaterialEntry& entry : published_config->material_keys)
                        if (entry.slot == static_cast<int>(slot) && entry.publish_color && !entry.color.empty()) {
                            seed = entry.color;
                            break;
                        }
                    if (seed.empty()) {
                        if (const Preset* preset = this->filaments.find_preset(this->filament_presets[slot], false)) {
                            const ConfigOptionStrings* colours = preset->config.opt<ConfigOptionStrings>("filament_colour");
                            if (colours != nullptr && !colours->values.empty())
                                seed = colours->values.front();
                        }
                        if (seed.empty()) {
                            if (const ConfigOptionDef* colour_def = print_config_def.get("filament_colour"))
                                if (const auto* default_colours = dynamic_cast<const ConfigOptionStrings*>(colour_def->default_value.get()))
                                    if (!default_colours->values.empty())
                                        seed = default_colours->values.front();
                        }
                    }
                    if (proj_colour && slot < proj_colour->values.size())
                        proj_colour->values[slot] = seed;
                    if (proj_multi_colour && slot < proj_multi_colour->values.size())
                        proj_multi_colour->values[slot] = seed;
                    if (proj_colour_type && slot < proj_colour_type->values.size())
                        proj_colour_type->values[slot] = "1";
                }
                this->update_multi_material_filament_presets();

                auto apply_slot_keys = [&](DynamicPrintConfig& preset_config, const std::vector<std::string>& slot_keys, int author_slot,
                                           const std::string& material_label) {
                    for (const std::string& key : slot_keys) {
                        const std::string base_key = publish_base_key(key);
                        if (structural_keys.count(base_key) != 0)
                            continue;
                        const ConfigOption* src_opt = config.option(base_key);
                        if (src_opt == nullptr || !src_opt->is_vector() || author_slot < 0 ||
                            author_slot >= static_cast<int>(static_cast<const ConfigOptionVectorBase*>(src_opt)->size())) {
                            skipped_keys.emplace_back("material:" + material_label + " (" + key + ")");
                            continue;
                        }
                        ConfigOption* dst_opt = preset_config.option(base_key);
                        if (dst_opt == nullptr || !dst_opt->is_vector() || static_cast<const ConfigOptionVectorBase*>(dst_opt)->empty() ||
                            dst_opt->type() != src_opt->type()) {
                            skipped_keys.emplace_back("material:" + material_label + " (" + key + ")");
                            continue;
                        }
                        static_cast<ConfigOptionVectorBase*>(dst_opt)->set_at(src_opt, 0, author_slot);
                        material_applied = true;
                    }
                };

                const bool edited_survives_load = this->filament_presets.empty() ||
                                                  this->filament_presets.front() == this->filaments.get_edited_preset().name;
                apply_receiver_mix_relocations(this->project_config, this->ams_multi_color_filment, receiver_mix_moves);
                std::set<int> mixed_final_slots;
                for (const PublishedMaterialEntry& mix_entry : published_config->material_keys)
                    if (mix_entry.slot >= 0 && (is_mixed_definition(mix_entry) || mix_entry.mixed_placeholder))
                        mixed_final_slots.insert(mix_entry.slot);
                for (size_t i = 0; i < this->filament_presets.size(); ++i)
                    if (this->is_mixed_filament(i))
                        mixed_final_slots.insert(int(i));
                for (int gap_slot : virtual_gap_slots)
                    mixed_final_slots.insert(gap_slot);
                const size_t mixed_final_slot_count = this->filament_presets.size();
                auto finalize_mixed_placeholder = [&](size_t slot_idx) {
                    if (ConfigOptionBools* is_mixed_opt = this->project_config.opt<ConfigOptionBools>("filament_is_mixed");
                        is_mixed_opt != nullptr && slot_idx < is_mixed_opt->values.size())
                        is_mixed_opt->values[slot_idx] = true;
                    mixed_final_slots.insert(int(slot_idx));
                    material_applied = true;
                };
                std::map<std::string, std::string> published_full_dedup;
                for (const PublishedMaterialEntry& entry : published_config->material_keys) {
                    if (entry.slot < 0 || size_t(entry.slot) >= this->filament_presets.size())
                        continue;
                    const size_t slot = size_t(entry.slot);

                    if (entry.mixed_placeholder) {
                        finalize_mixed_placeholder(slot);
                        continue;
                    }

                    Preset* recv = this->filaments.find_preset(this->filament_presets[slot], false, true);
                    if (recv == nullptr) {
                        if (is_mixed_definition(entry) && !this->is_mixed_filament(slot)) {
                            finalize_mixed_placeholder(slot);
                            published_config->material_replacements.emplace_back("slot " + std::to_string(slot) +
                                                                                 ": mixed filament definition could not be imported");
                        }
                        continue;
                    }

                    const std::string material_label = entry.filament_id.empty() ?
                                                           (entry.publish_type_value.empty() ? entry.filament_type :
                                                                                               entry.publish_type_value) :
                                                           entry.filament_id;

                    if (is_mixed_definition(entry)) {
                        std::string mix_error;
                        if (const ConfigOptionStrings* comp_opt = config.opt<ConfigOptionStrings>("filament_mixed_components");
                            comp_opt != nullptr && entry.slot < static_cast<int>(comp_opt->values.size())) {
                            const std::vector<unsigned int> comps = parse_mixed_components(comp_opt->values[entry.slot]);
                            if (comps.size() < 2)
                                mix_error = "needs at least two components";
                            else
                                for (unsigned int comp : comps)
                                    if (comp < 1 || size_t(comp) > mixed_final_slot_count ||
                                        mixed_final_slots.count(int(comp) - 1) != 0) {
                                        mix_error = "components reference missing slots";
                                        break;
                                    }
                        }
                        if (!mix_error.empty()) {
                            skipped_keys.emplace_back("material:" + material_label + " (mixed filament definition: " + mix_error + ")");
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": published 3MF mixed filament from slot " << entry.slot
                                                       << " rejected: " << mix_error;
                            if (!this->is_mixed_filament(slot)) {
                                finalize_mixed_placeholder(slot);
                                published_config->material_replacements.emplace_back("slot " + std::to_string(slot) +
                                                                                     ": mixed filament definition could not be imported (" +
                                                                                     mix_error + ")");
                            }
                            continue;
                        }
                    }

                    if (entry.full) {
                        std::string dedup_key = entry.setting_id + std::string("\x1f") + entry.preset_name;
                        if (dedup_key == std::string("\x1f"))
                            dedup_key = dedup_key + std::to_string(slot);
                        else if (!entry.filament_id.empty())
                            dedup_key += std::string("\x1f") + entry.filament_id;
                        std::string new_name;
                        const auto dedup_it = published_full_dedup.find(dedup_key);
                        if (dedup_it != published_full_dedup.end()) {
                            new_name = dedup_it->second;
                        } else {
                            DynamicPrintConfig new_cfg = recv != nullptr ? recv->config : this->filaments.default_preset().config;
                            for (const std::string& key : entry.full_keys) {
                                const std::string base_key = publish_base_key(key);
                                if (structural_keys.count(base_key) != 0)
                                    continue;
                                const ConfigOption* src_opt = config.option(base_key);
                                if (src_opt == nullptr || !src_opt->is_vector() || entry.slot < 0 ||
                                    entry.slot >= static_cast<int>(static_cast<const ConfigOptionVectorBase*>(src_opt)->size()))
                                    continue;
                                ConfigOption* dst_opt = new_cfg.option(base_key);
                                if (dst_opt == nullptr || !dst_opt->is_vector() ||
                                    static_cast<const ConfigOptionVectorBase*>(dst_opt)->empty() || dst_opt->type() != src_opt->type())
                                    continue;
                                static_cast<ConfigOptionVectorBase*>(dst_opt)->set_at(src_opt, 0, entry.slot);
                            }
                            if (entry.publish_color && !entry.color.empty()) {
                                if (ConfigOptionStrings* col = new_cfg.opt<ConfigOptionStrings>("filament_colour", true)) {
                                    if (col->values.empty())
                                        col->values.emplace_back();
                                    col->values[0] = entry.color;
                                }
                            }
                            make_publish_universal(new_cfg);
                            std::string base_name = entry.preset_name.empty() ? std::string() :
                                                                                publish_material_base_name(entry.preset_name);
                            if (base_name.empty()) {
                                base_name = !entry.filament_id.empty() ?
                                                entry.filament_id :
                                                (!entry.publish_type_value.empty() ? entry.publish_type_value : entry.filament_type);
                                if (base_name.empty())
                                    base_name = "Published Filament";
                            }
                            new_name = this->filaments.add_detached_preset(base_name, std::move(new_cfg), entry.filament_id);
                            published_full_dedup.emplace(dedup_key, new_name);
                        }
                        const std::string old_name   = this->filament_presets[slot];
                        this->filament_presets[slot] = new_name;
                        material_applied             = true;
                        published_config->material_replacements.emplace_back("slot " + std::to_string(slot) + ": " + old_name + " -> " +
                                                                             new_name);
                        if (entry.publish_color && !entry.color.empty()) {
                            if (ConfigOptionStrings* proj_colour = this->project_config.opt<ConfigOptionStrings>("filament_colour")) {
                                if (slot < proj_colour->values.size())
                                    proj_colour->values[slot] = entry.color;
                            }
                            if (ConfigOptionStrings* proj_multi_colour = this->project_config.opt<ConfigOptionStrings>(
                                    "filament_multi_colour")) {
                                if (slot < proj_multi_colour->values.size())
                                    proj_multi_colour->values[slot] = entry.color;
                            }
                        } else if (proj_colour && new_name != old_name) {
                            std::string seed;
                            if (const Preset* created = this->filaments.find_preset(new_name, false, true)) {
                                if (const ConfigOptionStrings* cols = created->config.opt<ConfigOptionStrings>("filament_colour"))
                                    if (!cols->values.empty())
                                        seed = cols->values.front();
                            }
                            if (seed.empty()) {
                                if (const ConfigOptionDef* colour_def = print_config_def.get("filament_colour"))
                                    if (const auto* defaults = dynamic_cast<const ConfigOptionStrings*>(colour_def->default_value.get()))
                                        if (!defaults->values.empty())
                                            seed = defaults->values.front();
                            }
                            if (!seed.empty()) {
                                if (proj_colour && slot < proj_colour->values.size())
                                    proj_colour->values[slot] = seed;
                                if (proj_multi_colour && slot < proj_multi_colour->values.size())
                                    proj_multi_colour->values[slot] = seed;
                            }
                        }
                        if (proj_colour_type && slot < proj_colour_type->values.size())
                            proj_colour_type->values[slot] = "1";
                        continue;
                    }

                    bool apply_slot = true;
                    const DynamicPrintConfig& effective_config = (edited_survives_load &&
                                                                  recv->name == this->filaments.get_edited_preset().name) ?
                                                                     this->filaments.get_edited_preset().config :
                                                                     recv->config;
                    if (entry.publish_type && !entry.publish_type_value.empty()) {
                        const ConfigOptionStrings* recv_types = effective_config.opt<ConfigOptionStrings>("filament_type");
                        const std::string recv_type = (recv_types != nullptr && !recv_types->values.empty()) ? recv_types->get_at(0) :
                                                                                                               std::string();
                        if (normalize_filament_type(recv_type) != entry.publish_type_value) {
                            auto find_best = [&](bool compatible_only) -> std::pair<int, std::string> {
                                auto scan = [&](bool unreferenced_only) -> std::pair<int, std::string> {
                                    int best_score = -1;
                                    std::string best_name;
                                    const std::string resolved_name = resolved_name_for(entry.slot);
                                    for (size_t i = first_candidate; i < this->filaments.size(); ++i) {
                                        const Preset& candidate = this->filaments.preset(i);
                                        if (compatible_only && !candidate.is_compatible)
                                            continue;
                                        const int score = candidate_score(candidate, entry, resolved_name);
                                        if (score <= best_score)
                                            continue;
                                        if (score < 3 && !candidate.is_visible)
                                            continue;
                                        if (unreferenced_only) {
                                            bool used = false;
                                            for (size_t s = 0; s < this->filament_presets.size(); ++s)
                                                if (s != slot && this->filament_presets[s] == candidate.name) {
                                                    used = true;
                                                    break;
                                                }
                                            if (used)
                                                continue;
                                        }
                                        best_score = score;
                                        best_name  = candidate.name;
                                    }
                                    return {best_score, best_name};
                                };
                                const auto [strict_score, strict_name]   = scan(true);
                                const auto [relaxed_score, relaxed_name] = scan(false);
                                if (relaxed_score > strict_score)
                                    return {relaxed_score, relaxed_name};
                                return {strict_score, strict_name};
                            };
                            const auto [compat_score, compat_name] = find_best(true);
                            const auto [any_score, any_name]       = find_best(false);
                            int score                              = any_score;
                            std::string replacement                = any_name;
                            if (compat_score >= 0) {
                                score       = compat_score;
                                replacement = compat_name;
                            }
                            if (!replacement.empty()) {
                                const std::string old_name   = recv->name;
                                this->filament_presets[slot] = replacement;
                                recv                         = this->filaments.find_preset(replacement, false, true);
                                material_applied             = true;
                                BOOST_LOG_TRIVIAL(info)
                                    << __FUNCTION__ << ": published 3MF slot " << slot << " material " << old_name << " -> " << replacement
                                    << " (score " << score << ", preset_name \"" << entry.preset_name << "\", filament_id \""
                                    << entry.filament_id << "\", setting_id \"" << entry.setting_id << "\", type \""
                                    << entry.publish_type_value << "\")";
                                std::string replacement_line = "slot " + std::to_string(slot) + ": " + old_name + " -> " + replacement;
                                if (score < 2 && (!entry.filament_id.empty() || !entry.filament_vendor.empty()))
                                    replacement_line += " (substitute)";
                                published_config->material_replacements.emplace_back(std::move(replacement_line));
                            } else {
                                for (const std::string& key : entry.keys)
                                    skipped_keys.emplace_back("material:" + material_label + " (" + key + ")");
                                apply_slot = false;
                                if (is_mixed_definition(entry) && !this->is_mixed_filament(slot)) {
                                    finalize_mixed_placeholder(slot);
                                    published_config->material_replacements.emplace_back("slot " + std::to_string(slot) +
                                                                                         ": mixed filament definition could not be imported");
                                }
                            }
                        }
                    }

                    DynamicPrintConfig& write_config = (edited_survives_load && recv->name == this->filaments.get_edited_preset().name) ?
                                                           this->filaments.get_edited_preset().config :
                                                           recv->config;

                    if (entry.publish_color && !entry.color.empty()) {
                        const bool is_mixed_entry = is_mixed_definition(entry);
                        if (!is_mixed_entry && recv != nullptr) {
                            if (ConfigOptionStrings* colour = write_config.opt<ConfigOptionStrings>("filament_colour", true)) {
                                if (colour->values.empty())
                                    colour->values.emplace_back();
                                colour->values[0] = entry.color;
                                material_applied  = true;
                            }
                        }
                        if (ConfigOptionStrings* proj_colour = this->project_config.opt<ConfigOptionStrings>("filament_colour")) {
                            if (slot < proj_colour->values.size())
                                proj_colour->values[slot] = entry.color;
                        }
                        if (ConfigOptionStrings* proj_multi_colour = this->project_config.opt<ConfigOptionStrings>(
                                "filament_multi_colour")) {
                            if (slot < proj_multi_colour->values.size())
                                proj_multi_colour->values[slot] = entry.color;
                        }
                    }

                    if (apply_slot && recv != nullptr) {
                        const std::set<std::string>& mixed_keys = publish_mixed_keys();
                        std::vector<std::string>        preset_keys;
                        for (const std::string& key : entry.keys) {
                            const std::string base_key = publish_base_key(key);
                            if (mixed_keys.count(base_key) != 0) {
                                const ConfigOption* src_opt = config.option(base_key);
                                if (src_opt != nullptr && src_opt->is_vector() && entry.slot >= 0 &&
                                    entry.slot < static_cast<int>(static_cast<const ConfigOptionVectorBase*>(src_opt)->size())) {
                                    if (ConfigOption* dst_opt = this->project_config.option(base_key)) {
                                        if (dst_opt->is_vector() && dst_opt->type() == src_opt->type() &&
                                            slot < static_cast<const ConfigOptionVectorBase*>(dst_opt)->size()) {
                                            static_cast<ConfigOptionVectorBase*>(dst_opt)->set_at(src_opt, slot, entry.slot);
                                            material_applied = true;
                                            continue;
                                        }
                                    }
                                }
                                skipped_keys.emplace_back("material:" + material_label + " (" + key + ")");
                                continue;
                            }
                            preset_keys.emplace_back(key);
                        }
                        apply_slot_keys(write_config, preset_keys, entry.slot, material_label);
                    }
                }
                for (int gap_slot : virtual_gap_slots)
                    if (!this->is_mixed_filament(size_t(gap_slot))) {
                        finalize_mixed_placeholder(size_t(gap_slot));
                        published_config->material_replacements.emplace_back(
                            "slot " + std::to_string(gap_slot) +
                            ": unassigned mixed filament (printer supports only " + std::to_string(physical_capacity) + " filaments)");
                    }
            }
        }

        for (const std::string& key : published_config->published_keys) {
            if (applied_keys.count(key) != 0)
                continue;
            const std::string base_key = publish_base_key(key);
            if (structural_keys.count(base_key) != 0)
                continue;
            if (contract_excluded_keys.count(base_key) != 0)
                continue;
            skipped_keys.emplace_back(key);
        }
        published_config->skipped_keys = std::move(skipped_keys);

        if (material_applied && !this->filament_presets.empty() &&
            this->filament_presets.front() != this->filaments.get_edited_preset().name)
            this->filaments.select_preset_by_name(this->filament_presets.front(), false);
    }


    const std::string physical_printer;
    if (!is_published) {
        if (this->printers.get_edited_preset().is_external || physical_printer.empty()) {
            this->physical_printers.unselect_printer();
        } else {
            PhysicalPrinter *pp = this->physical_printers.find_printer(physical_printer, true);
            if (pp != nullptr && std::find(pp->preset_names.begin(), pp->preset_names.end(), this->printers.get_edited_preset().name) != pp->preset_names.end())
                this->physical_printers.select_printer(pp->name, this->printers.get_edited_preset().name);
            else
                this->physical_printers.unselect_printer();
        }
    }
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": finished");
}

static ConfigurationError failed_loading_error(const std::string& file, const std::string& dir)
{
    return ConfigurationError((boost::format("Failed loading configuration file %1%\nSuggest cleaning the directory %2% firstly") % file % dir).str());
}

void PresetBundle::log_errors(const std::vector<std::string>& errors)
{
    for (const std::string& error : errors) {
        ++m_errors;
        BOOST_LOG_TRIVIAL(error) << error;
    }
}

PresetBundle::PresetInstall PresetBundle::resolve_vendor_preset(const CachedPreset& entry, const VendorInstall& install) const
{
    const std::string&        path = install.path;
    const std::string&        vendor_name = install.vendor_name;
    const PresetBundle*       base_bundle = install.base_bundle;
    const PresetCollection&   presets_collection = *install.presets;
    const VendorProfile&      current_vendor_profile = *install.vendor_profile;
    const std::string&        preset_name = entry.name;
    const DynamicPrintConfig* default_config = nullptr;
    const bool                retain = install.is_from_lib || install.inherited.count(preset_name) != 0;

    PresetInstall out;
    out.filament_id  = entry.filament_id;
    out.renamed_from = entry.renamed_from;

    if (! entry.inherits.empty()) {
        auto it2 = install.config_maps.find(entry.inherits);
        if (it2 != install.config_maps.end())
            default_config = &(it2->second);
        if (default_config == nullptr && base_bundle != nullptr) {
            auto base_it2 = base_bundle->m_config_maps.find(entry.inherits);
            if (base_it2 != base_bundle->m_config_maps.end())
                default_config = &(base_it2->second);
        }
        if (default_config != nullptr) {
            if (out.filament_id.empty() && (presets_collection.type() == Preset::TYPE_FILAMENT)) {
                auto filament_id_map_iter = install.filament_id_maps.find(entry.inherits);
                if (filament_id_map_iter != install.filament_id_maps.end()) {
                    out.filament_id = filament_id_map_iter->second;
                }
                if (out.filament_id.empty() && base_bundle != nullptr) {
                    auto base_filament_id_map_iter = base_bundle->m_filament_id_maps.find(entry.inherits);
                    if (base_filament_id_map_iter != base_bundle->m_filament_id_maps.end()) {
                        out.filament_id = base_filament_id_map_iter->second;
                    }
                }
            }
        }
        else {
            out.errors.push_back(std::string(__FUNCTION__) + ": can not find inherits " + entry.inherits + " for " + preset_name);
            out.reason = "Can not find inherits: " + entry.inherits;
            return out;
        }
    }
    else
        default_config = &presets_collection.default_preset_for(entry.config_src).config;
    out.config = *default_config;
    for (const std::string& name : entry.includes) {
        auto it = install.include_maps.find(name);
        if (it == install.include_maps.end()) {
            out.errors.push_back(std::string(__FUNCTION__) + ": can not find include " + name + " for " + preset_name);
            continue;
        }
        out.config.apply(it->second);
    }
    out.config.apply(entry.config_src);
    if (entry.instantiation == "false" && install.included.count(preset_name) != 0) {
        out.included.emplace();
        out.included->apply_only(out.config, out.config.diff(presets_collection.default_preset_for(out.config).config));
    }
    extend_default_config_length(out.config, true, *default_config);
    std::string incorrect_keys = Preset::remove_invalid_keys(out.config, *default_config, &entry.config_src);
    if (!incorrect_keys.empty())
        out.errors.push_back(std::string(__FUNCTION__) + ": The config " + path + "/" + vendor_name + "/" + entry.sub_path +
                             " contains incorrect keys: " + incorrect_keys + ", which were removed");
    if (entry.instantiation == "false" && "Template" != vendor_name) {
        out.config_only = true;
        if (retain)
            out.retained = std::move(out.config);
        return out;
    }
    if (out.config.has("alias"))
        out.alias = (dynamic_cast<const ConfigOptionString *>(out.config.option("alias")))->value;

    Preset::normalize(out.config);

    if (presets_collection.type() == Preset::TYPE_PRINTER) {
        auto printer_model   = out.config.opt_string("printer_model");
        if (printer_model.empty()) {
            out.errors.push_back("Error in a Vendor Config Bundle \"" + path + "\": The printer preset \"" +
                preset_name + "\" defines no printer model, it will be ignored.");
            out.reason = std::string("can not find printer_model");
            return out;
        }
        auto printer_variant = out.config.opt_string("printer_variant");
        if (printer_variant.empty()) {
            out.errors.push_back("Error in a Vendor Config Bundle \"" + path + "\": The printer preset \"" +
                preset_name + "\" defines no printer variant, it will be ignored.");
            out.reason = std::string("can not find printer_variant");
            return out;
        }
        auto it_model = std::find_if(current_vendor_profile.models.cbegin(), current_vendor_profile.models.cend(),
            [&](const VendorProfile::PrinterModel &m) { return m.id == printer_model; }
        );
        if (it_model == current_vendor_profile.models.end()) {
            out.errors.push_back("Error in a Vendor Config Bundle \"" + path + "\": The printer preset \"" +
                preset_name + "\" defines invalid printer model \"" + printer_model + "\", it will be ignored.");
            out.reason = std::string("can not find printer model in vendor profile");
            return out;
        }
        auto it_variant = it_model->variant(printer_variant);
        if (it_variant == nullptr) {
            out.errors.push_back("Error in a Vendor Config Bundle \"" + path + "\": The printer preset \"" +
                preset_name + "\" defines invalid printer variant \"" + printer_variant + "\", it will be ignored.");
            out.reason = std::string("can not find printer_variant in vendor profile");
            return out;
        }
        if (validation_mode && entry.instantiation == "true") {
            const auto *nd = out.config.option<ConfigOptionFloats>("nozzle_diameter");
            std::set<double> nozzles, variant_nozzles;
            if (nd != nullptr)
                nozzles.insert(nd->values.begin(), nd->values.end());
            std::vector<std::string> variant_tokens;
            boost::algorithm::split(variant_tokens, printer_variant, boost::algorithm::is_any_of("+"));
            bool variant_ok = true;
            for (const std::string &tok : variant_tokens) {
                size_t consumed = 0;
                double d = string_to_double_decimal_point(tok, &consumed);
                if (consumed == 0) { variant_ok = false; break; }
                variant_nozzles.insert(d);
            }
            if (!variant_ok || variant_nozzles != nozzles) {
                out.errors.push_back("Error in a Vendor Config Bundle \"" + path + "\": The printer preset \"" +
                    preset_name + "\" has printer_variant \"" + printer_variant +
                    "\" that does not match its nozzle_diameter \"" + (nd ? nd->serialize() : std::string()) + "\". "
                    "printer_variant must begin with the nozzle diameter, optionally followed by a non-numeric suffix "
                    "(e.g. \"0.4\", \"0.8HF\"); for multi-nozzle printers, join the per-nozzle diameters with \"+\" in "
                    "nozzle order (e.g. \"0.4+0.6\").");
            }
        }
    }

    if (out.alias.empty()) {
        size_t end_pos = preset_name.find_first_of("@");
        if (end_pos != std::string::npos) {
            out.alias = preset_name.substr(0, end_pos);
            if (out.renamed_from.empty())
                out.renamed_from.emplace_back(out.alias + preset_name.substr(end_pos + 1));
            boost::trim_right(out.alias);
        }
    }

    out.file_path = (boost::filesystem::path(data_dir()) / PRESET_SYSTEM_DIR / vendor_name / entry.sub_path).make_preferred().string();
    if (validation_mode)
        out.file_path = (boost::filesystem::path(data_dir()) / vendor_name / entry.sub_path).make_preferred().string();
    if (m_preserve_vendor_source_paths)
        out.file_path = (boost::filesystem::path(path) / vendor_name / entry.sub_path).make_preferred().string();
    if (retain)
        out.retained = out.config;
    return out;
}

std::string PresetBundle::commit_vendor_preset(const CachedPreset& entry, PresetInstall&& resolved,
                                               ConfigSubstitutions&& substitutions, VendorInstall& install)
{
    const std::string&   path = install.path;
    const std::string&   vendor_name = install.vendor_name;
    PresetCollection*    presets_collection = install.presets;
    const VendorProfile* current_vendor_profile = install.vendor_profile;
    const std::string&   preset_name = entry.name;

    log_errors(resolved.errors);
    if (resolved.included)
        install.include_maps.emplace(preset_name, std::move(*resolved.included));
    if (! resolved.reason.empty())
        return resolved.reason;

    if (resolved.config_only) {
        if (resolved.retained)
            install.config_maps.emplace(preset_name, std::move(*resolved.retained));
        if ((presets_collection->type() == Preset::TYPE_FILAMENT) && (!resolved.filament_id.empty()))
            install.filament_id_maps.emplace(preset_name, resolved.filament_id);
        return std::string();
    }

    if (! install.installed_names.insert(preset_name).second) {
        ++m_errors;
        BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" <<
            preset_name << "\" has already been loaded from another Config Bundle.";
        return std::string("duplicated defines");
    }

    Preset &loaded = presets_collection->append_preset(std::move(resolved.file_path), preset_name, std::move(resolved.config));
    if (install.flags.has(LoadConfigBundleAttribute::LoadSystem)) {
        loaded.is_system = true;
        loaded.vendor = current_vendor_profile;
        loaded.version = current_vendor_profile->config_version;
        loaded.description = entry.description;
        loaded.setting_id = entry.setting_id;
        if (loaded.setting_id.empty() && entry.instantiation == "true")
            loaded.setting_id = generate_preset_setting_id(
                vendor_name, Preset::get_type_string(presets_collection->type()), preset_name);
        loaded.filament_id = resolved.filament_id;
        loaded.m_from_orca_filament_lib = install.is_from_lib;
        if (presets_collection->type() == Preset::TYPE_FILAMENT) {
            if (resolved.filament_id.empty() && "Template" != vendor_name) {
                ++m_errors;
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": can not find filament_id for " << preset_name;
                return "Can not find filament_id for " + preset_name;
            }
            else {
                install.filament_id_maps.emplace(preset_name, resolved.filament_id);
            }
        }
    }

    if (resolved.alias.empty())
        loaded.alias = preset_name;
    else {
        loaded.alias = std::move(resolved.alias);
        filaments.set_printer_hold_alias(loaded.alias, loaded);
    }
    loaded.renamed_from = std::move(resolved.renamed_from);
    if (! substitutions.empty())
        install.substitutions->push_back({
            preset_name, presets_collection->type(), PresetConfigSubstitutions::Source::ConfigBundle,
            std::string(), std::move(substitutions) });
    if (resolved.retained)
        install.config_maps.emplace(preset_name, std::move(*resolved.retained));
    ++install.count;
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ", got preset " << loaded.name << ", filament_id " << loaded.filament_id
                             << ", from " << path << "/" << vendor_name << "/" << entry.sub_path;
    return std::string();
}

void PresetBundle::install_vendor_entries(VendorInstall& install, const std::vector<CachedPreset>& entries,
                                          std::vector<EntryParse>* parsed)
{
    assert(parsed == nullptr || parsed->size() == entries.size());
    for (const CachedPreset& entry : entries) {
        if (! entry.inherits.empty())
            install.inherited.insert(entry.inherits);
        install.included.insert(entry.includes.begin(), entry.includes.end());
    }
    for (const Preset& preset : install.presets->m_presets)
        install.installed_names.insert(preset.name);
    install.presets->lock();
    ScopeGuard sort_on_exit([&install] {
        install.presets->sort_presets();
        install.presets->unlock();
    });

    auto resolve = [&](size_t i) { return resolve_vendor_preset(entries[i], install); };
    auto commit  = [&](size_t i, PresetInstall&& resolved) {
        ConfigSubstitutions substitutions;
        if (parsed != nullptr) {
            log_errors((*parsed)[i].errors);
            for (const std::string& warning : (*parsed)[i].warnings)
                BOOST_LOG_TRIVIAL(error) << warning;
            substitutions = std::move((*parsed)[i].substitutions);
        }
        const std::string reason = commit_vendor_preset(entries[i], std::move(resolved), std::move(substitutions), install);
        if (! reason.empty()) {
            ++m_errors;
            const std::string subfile_path = install.path + "/" + install.vendor_name + "/" + entries[i].sub_path;
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": can not install " << entries[i].name << " from " << subfile_path << ": " << reason;
            throw failed_loading_error(subfile_path, install.path);
        }
    };
    std::unordered_set<std::string> run_names;
    size_t run_begin = 0;
    auto install_run = [&](size_t run_end) {
        resolve_then_commit(run_end - run_begin,
            [&](size_t k) { return resolve(run_begin + k); },
            [&](size_t k, PresetInstall&& resolved) { commit(run_begin + k, std::move(resolved)); });
        run_names.clear();
        run_begin = run_end;
    };
    auto in_run = [&](const std::string& name) { return run_names.count(name) != 0; };
    for (size_t i = 0; i < entries.size(); ++ i) {
        const CachedPreset& entry = entries[i];
        if (in_run(entry.inherits) || std::any_of(entry.includes.begin(), entry.includes.end(), in_run))
            install_run(i);
        if (install.inherited.count(entry.name) != 0 || install.included.count(entry.name) != 0)
            run_names.insert(entry.name);
    }
    install_run(entries.size());
}

size_t PresetBundle::install_vendor(const std::string& path, const std::string& vendor_name, const PresetBundle* base_bundle,
                                    LoadConfigBundleAttributes flags, const VendorCacheData& entries,
                                    VendorParse* parsed, bool complete, PresetsConfigSubstitutions& substitutions)
{
    const bool           is_orca_lib    = vendor_name == ORCA_FILAMENT_LIBRARY;
    const VendorProfile* vendor_profile = &this->vendors.at(vendor_name);
    size_t               count          = 0;
    auto install_collection = [&](const std::vector<CachedPreset>& list, std::vector<EntryParse>* list_parsed,
                                  PresetCollection* presets, bool is_from_lib) {
        VendorInstall install { path, vendor_name, vendor_profile, base_bundle, flags, presets, is_from_lib, &substitutions };
        install_vendor_entries(install, list, list_parsed);
        count += install.count;
        if (flags.has(LoadConfigBundleAttribute::KeepBases))
            for (const auto &[name, config] : install.config_maps)
                base_configs.emplace(std::make_pair(presets->type(), name), config);
        return install;
    };
    install_collection(entries.process_entries, parsed ? &parsed->process_entries : nullptr, &this->prints, false);
    VendorInstall filaments = install_collection(entries.filament_entries, parsed ? &parsed->filament_entries : nullptr,
                                                 &this->filaments, is_orca_lib);
    if (is_orca_lib && complete) {
        m_config_maps      = std::move(filaments.config_maps);
        m_filament_id_maps = std::move(filaments.filament_id_maps);
    }
    install_collection(entries.machine_entries, parsed ? &parsed->machine_entries : nullptr, &this->printers, false);
    return count;
}

std::pair<PresetsConfigSubstitutions, size_t> PresetBundle::load_vendor_configs_from_json(
    const std::string &dir, const std::string &vendor_name, LoadConfigBundleAttributes flags,
    ForwardCompatibilitySubstitutionRule compatibility_rule, const PresetBundle* base_bundle, bool allow_cache)
{
    return this->install_vendor_read(this->read_vendor(dir, vendor_name, flags, compatibility_rule, allow_cache), base_bundle);
}

PresetBundle::VendorRead PresetBundle::read_vendor(const std::string& dir, const std::string& vendor_name,
    LoadConfigBundleAttributes flags, ForwardCompatibilitySubstitutionRule compatibility_rule, bool allow_cache)
{
    VendorRead read { dir, vendor_name, flags, compatibility_rule };
    read.errors_at_entry = m_errors;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" enter, path %1%, compatibility_rule %2%")%dir.c_str()%compatibility_rule;
    if (flags.has(LoadConfigBundleAttribute::ResetUserProfile) || flags.has(LoadConfigBundleAttribute::LoadSystem))
        this->reset(flags.has(LoadConfigBundleAttribute::SaveImported));

    const bool vendor_only   = flags.has(LoadConfigBundleAttribute::LoadVendorOnly);
    const bool filament_only = flags.has(LoadConfigBundleAttribute::LoadFilamentOnly);
    read.cacheable = allow_cache && flags.has(LoadConfigBundleAttribute::LoadSystem) && ! filament_only;
    const boost::filesystem::path dir_path(dir);
    const boost::filesystem::path profile     = dir_path / (vendor_name + ".json");
    const bool                    has_profile = boost::filesystem::exists(profile);
    if (allow_cache && ! validation_mode && (read.cacheable || ((vendor_only || filament_only) && ! has_profile))) {
        const Semver version = has_profile ? get_version_from_json(profile.string()) : Semver::inf();
        read.cache_path = (dir_path / (vendor_name + ".opc")).string();
        if (vendor_only) {
            VendorProfile vendor_profile;
            if (VendorCacheFile::load_vendor_profile(read.cache_path, vendor_name, version, vendor_profile)) {
                this->vendors.emplace(vendor_name, std::move(vendor_profile));
                read.vendor_only = true;
                return read;
            }
        } else if (VendorCacheFile::load(read.cache_path, vendor_name, version, read.data)) {
            if (!read.data.has_model_bed_defaults && has_profile) {
                try {
                    PresetBundle metadata;
                    metadata.load_vendor_configs_from_json(dir, vendor_name, LoadConfigBundleAttribute::LoadVendorOnly,
                                                           compatibility_rule, nullptr, false);
                    const auto& models = metadata.vendors.at(vendor_name).models;
                    for (auto& model : read.data.vendors.at(vendor_name).models)
                        for (const auto& source : models)
                            if (source.id == model.id) {
                                model.default_bed_type = source.default_bed_type;
                                break;
                            }
                } catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(warning) << "Cannot read model bed defaults for " << vendor_name << ": " << e.what();
                }
            }
            read.from_cache = true;
            if (filament_only) {
                read.data.process_entries.clear();
                read.data.machine_entries.clear();
                read.data.parse_errors = 0;
            }
            return read;
        }
    }
    this->parse_vendor_json(read);
    return read;
}

void PresetBundle::parse_vendor_json(VendorRead& read)
{
    read = VendorRead { std::move(read.dir), std::move(read.vendor_name), read.flags, read.compatibility_rule,
                        read.errors_at_entry, read.cacheable };
    const std::string&         dir         = read.dir;
    const std::string&         vendor_name = read.vendor_name;
    LoadConfigBundleAttributes flags       = read.flags;

    VendorProfile vendor_profile(vendor_name);
    std::string root_file = dir + "/" + vendor_name + ".json";
    std::vector<std::pair<std::string, std::string>> machine_model_subfiles;
    std::vector<std::pair<std::string, std::string>> process_subfiles;
    std::vector<std::pair<std::string, std::string>> filament_subfiles;
    std::vector<std::pair<std::string, std::string>> machine_subfiles;
    auto get_name_and_subpath = [this](json::iterator& it, std::vector<std::pair<std::string, std::string>>& subfile_map) {
        if (it.value().is_array()) {
            size_t index = 0;
            for (auto iter1 = it.value().begin(); iter1 != it.value().end(); iter1++, index++) {
                if (iter1.value().is_object()) {
                    std::string name, subpath;
                    for (auto iter2 = iter1.value().begin(); iter2 != iter1.value().end(); iter2++) {
                        if (iter2.value().is_string()) {
                            if (boost::iequals(iter2.key(), BBL_JSON_KEY_NAME)) {
                                name = iter2.value();
                            } else if (boost::iequals(iter2.key(), BBL_JSON_KEY_SUB_PATH)) {
                                subpath = iter2.value();
                            }
                        }
                        else {
                            ++m_errors;
                            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": invalid value type for " << iter2.key();
                        }
                    }
                    if (!name.empty() && !subpath.empty())
                        subfile_map.push_back(std::make_pair(name, subpath));
                } else {
                    ++m_errors;
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": invalid type for " << it.key() << "[" << index << "]";
                }
            }
        } else {
            ++m_errors;
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": invalid type for " << it.key();
        }
    };
    try {
        boost::nowide::ifstream ifs(root_file);
        json j;
        ifs >> j;
        for (auto it = j.begin(); it != j.end(); it++) {
            if (boost::iequals(it.key(), BBL_JSON_KEY_VERSION)) {
                std::string version_str = it.value();
                auto config_version = Semver::parse(version_str);
                if (! config_version) {
                    ++m_errors;
                    throw ConfigurationError((boost::format("vendor %1%'s config version: %2% invalid\nSuggest cleaning the directory %3% firstly")
                        % vendor_name % version_str % dir).str());
                } else {
                    vendor_profile.config_version = std::move(*config_version);
                }
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_URL)) {
                vendor_profile.config_update_url = it.value();
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_DESCRIPTION)) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ": parse "<<root_file<<", got description:  " << it.value();
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_NAME)) {
                vendor_profile.name = it.value();
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_MACHINE_MODEL_LIST)) {
                get_name_and_subpath(it, machine_model_subfiles);
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_PROCESS_LIST)) {
                get_name_and_subpath(it, process_subfiles);
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_FILAMENT_LIST)) {
                get_name_and_subpath(it, filament_subfiles);
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_MACHINE_LIST)) {
                get_name_and_subpath(it, machine_subfiles);
            }
        }
    }
    catch(nlohmann::detail::parse_error &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<<root_file<<" got a nlohmann::detail::parse_error, reason = " << err.what();
        throw ConfigurationError((boost::format("Failed loading configuration file %1%: %2%\nSuggest cleaning the directory %3% firstly")
                %root_file %err.what() % dir).str());
    }

    if (flags.has(LoadConfigBundleAttribute::LoadFilamentOnly)) {
        machine_model_subfiles.clear();
        machine_subfiles.clear();
        process_subfiles.clear();
    }

    for (auto& machine_model : machine_model_subfiles)
    {
        std::string subfile = dir + "/" + vendor_name + "/" + machine_model.second;
        VendorProfile::PrinterModel model;
        model.id = machine_model.first;
        try {
            boost::nowide::ifstream ifs(subfile);
            json j;
            ifs >> j;
            for (auto it = j.begin(); it != j.end(); it++) {
                if (boost::iequals(it.key(), BBL_JSON_KEY_VERSION)) {
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_URL)) {
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_NAME)) {
                    model.name = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_MODEL_ID)) {
                    model.model_id = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_NOZZLE_DIAMETER)) {
                    std::string nozzle_diameters = it.value();
                    std::vector<std::string> variants;
                    if (Slic3r::unescape_strings_cstyle(nozzle_diameters, variants)) {
                        for (const std::string &variant_name : variants) {
                            if (model.variant(variant_name) == nullptr)
                                model.variants.emplace_back(VendorProfile::PrinterVariant(variant_name));
                        }
                    } else {
                        ++m_errors;
                        BOOST_LOG_TRIVIAL(error)<< __FUNCTION__ << boost::format(": invalid nozzle_diameters %1% for Vendor %1%") % nozzle_diameters % vendor_name;
                    }
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_PRINTER_TECH)) {
                    if (boost::algorithm::starts_with(it.value(), "SL"))
                        model.technology = ptSLA;
                    else
                        model.technology = ptFFF;
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_FAMILY)) {
                    model.family = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_BED_MODEL)) {
                    model.bed_model = it.value();
                } else if (boost::iequals(it.key(), BBL_JSON_KEY_BOTTOM_TEXTURE_END_NAME)) {
                    model.bottom_texture_end_name = it.value();
                } else if (boost::iequals(it.key(), BBL_JSON_KEY_USE_DOUBLE_EXTRUDER_DEFAULT_TEXTURE)) {
                    model.use_double_extruder_default_texture = it.value();
                } else if (boost::iequals(it.key(), BBL_JSON_KEY_BOTTOM_TEXTURE_RECT)) {
                    model.bottom_texture_rect = it.value();
                } else if (boost::iequals(it.key(), BBL_JSON_KEY_BOTTOM_TEXTURE_RECT_LONGER)) {
                    model.bottom_texture_rect_longer = it.value();
                } else if (boost::iequals(it.key(), BBL_JSON_KEY_MIDDLE_TEXTURE_RECT)) {
                    model.middle_texture_rect = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_IMAGE_BED_TYPE)) {
                    model.image_bed_type = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_BED_TEXTURE)) {
                    model.bed_texture = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_HOTEND_MODEL)) {
                    model.hotend_model = it.value();
                }
                else if (boost::iequals(it.key(), "default_bed_type")) {
                    model.default_bed_type = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_DEFAULT_MATERIALS)) {
                    std::string default_materials_field = it.value();
                    if (Slic3r::unescape_strings_cstyle(default_materials_field, model.default_materials)) {
                    	Slic3r::sort_remove_duplicates(model.default_materials);
                        if (! model.default_materials.empty() && model.default_materials.front().empty())
                            model.default_materials.erase(model.default_materials.begin());
                    } else {
                        ++m_errors;
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": invalid default_materials %1% for Vendor %1%") % default_materials_field % vendor_name;
                    }
                } else if (boost::iequals(it.key(), BBL_JSON_KEY_NOT_SUPPORT_BED_TYPE)) {
                    std::string not_support_bed_type_field = it.value();
                    if (Slic3r::unescape_strings_cstyle(not_support_bed_type_field, model.not_support_bed_types)) {
                        Slic3r::sort_remove_duplicates(model.not_support_bed_types);
                        if (!model.not_support_bed_types.empty() && model.not_support_bed_types.front().empty())
                            model.not_support_bed_types.erase(model.not_support_bed_types.begin());
                    } else {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__
                                                 << boost::format(": invalid not_support_bed_types %1% for Vendor %1%") % not_support_bed_type_field % vendor_name;
                    }
                }
            }
        }
        catch(nlohmann::detail::parse_error &err) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<< subfile <<" got a nlohmann::detail::parse_error, reason = " << err.what();
            throw ConfigurationError((boost::format("Failed loading configuration file %1%: %2%\nSuggest cleaning the directory %3% firstly")
                %subfile %err.what() % dir).str());
        }

        if (! model.id.empty() && ! model.variants.empty())
            vendor_profile.models.push_back(std::move(model));
    }

    this->vendors.emplace(vendor_name, vendor_profile);

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", loaded vendor profile, name %1%, id %2%, version %3%")%vendor_profile.name%vendor_profile.id%vendor_profile.config_version.to_string();

    if (flags.has(LoadConfigBundleAttribute::LoadVendorOnly)) {
        read.vendor_only = true;
        return;
    }

    auto parse_subfile = [&dir, &vendor_name](
        ConfigSubstitutionContext& substitution_context,
        const std::pair<std::string, std::string>& subfile_iter,
        CachedPreset& entry, std::vector<std::string>& errors, std::vector<std::string>& warnings) -> std::string {

        std::string subfile = dir + "/" + vendor_name + "/" + subfile_iter.second;
        std::string reason;
        try {
            std::map<std::string, std::string> key_values;
            substitution_context.substitutions.clear();

            entry.sub_path = subfile_iter.second;
            entry.config_src.load_from_json(subfile, substitution_context, false, key_values, reason);
            if (!reason.empty()) {
                errors.push_back(std::string(__FUNCTION__) + ": load config file " + subfile + " Failed!");
                return reason;
            }
            entry.name        = key_values[BBL_JSON_KEY_NAME];
            entry.description = key_values[BBL_JSON_KEY_DESCRIPTION];
            if (auto it = key_values.find(BBL_JSON_KEY_INSTANTIATION);
                (it == key_values.end() || it->second.empty()) && (entry.name.empty() || entry.name.find("gcode") != std::string::npos)) {
                key_values[BBL_JSON_KEY_INSTANTIATION] = "false";
                if (entry.name.empty())
                    entry.name = subfile_iter.first;
            }
            if(key_values.find(BBL_JSON_KEY_INSTANTIATION) == key_values.end())
                errors.push_back(std::string(__FUNCTION__) + ": Missing instantiation attribute for " + entry.name);
            entry.instantiation = key_values[BBL_JSON_KEY_INSTANTIATION];
            if(entry.instantiation != "false" && entry.instantiation != "true")
                errors.push_back(std::string(__FUNCTION__) + ": Missing instantiation attribute for " + entry.name);
            auto setting_it = key_values.find(BBL_JSON_KEY_SETTING_ID);
            if (setting_it != key_values.end())
                entry.setting_id = setting_it->second;
            auto filament_it = key_values.find(BBL_JSON_KEY_FILAMENT_ID);
            if (filament_it != key_values.end())
                entry.filament_id = filament_it->second;
            auto it1 = key_values.find(BBL_JSON_KEY_INHERITS);
            if (it1 != key_values.end()) {
                entry.inherits = it1->second;
                if (entry.inherits.empty()) {
                    errors.push_back(std::string(__FUNCTION__) + ": can not find inherits " + entry.inherits + " for " + entry.name);
                    reason = "Can not find inherits: " + entry.inherits;
                    return reason;
                }
            }
            if (auto it = key_values.find(BBL_JSON_KEY_INCLUDES); it != key_values.end()) {
                nlohmann::json includes = nlohmann::json::parse(it->second);
                if (!includes.is_array())
                    includes = nlohmann::json::array({std::move(includes)});
                for (const auto& name : includes) {
                    if (name.is_string())
                        entry.includes.push_back(name.get<std::string>());
                    else
                        errors.push_back(std::string(__FUNCTION__) + ": invalid include " + name.dump() + " for " + entry.name);
                }
            }
            if (key_values.find(ORCA_JSON_KEY_RENAMED_FROM) != key_values.end()) {
                if (!unescape_strings_cstyle(key_values[ORCA_JSON_KEY_RENAMED_FROM], entry.renamed_from))
                    warnings.push_back("Error in a Config \"" + dir + "\": The preset \"" + entry.name +
                                       "\" contains invalid \"renamed_from\" key, which is being ignored.");
            }
        }
        catch(nlohmann::detail::parse_error &err) {
            errors.push_back(std::string(__FUNCTION__) + ": parse " + subfile + " got a nlohmann::detail::parse_error, reason = " + err.what());
            reason = std::string("json parse error") + err.what();
            return reason;
        }
        return reason;
    };

    read.will_cache = read.cacheable && m_generate_vendor_caches && vendor_profile.config_version.valid();
    read.version    = vendor_profile.config_version.to_string();
    ConfigSubstitutionContext substitution_context { read.compatibility_rule };
    auto parse_subfiles = [&](const std::vector<std::pair<std::string, std::string>>& subfiles,
                              std::vector<CachedPreset>& entries, std::vector<EntryParse>& entries_parsed, const char* kind) {
        for (const auto& subfile : subfiles) {
            CachedPreset             entry;
            std::vector<std::string> errors, warnings;
            read.reason = parse_subfile(substitution_context, subfile, entry, errors, warnings);
            if (! read.reason.empty()) {
                read.failed_subfile = subfile.second;
                read.failed_kind    = kind;
                read.failed_errors  = std::move(errors);
                return false;
            }
            entries.emplace_back(std::move(entry));
            entries_parsed.push_back({ std::move(substitution_context.substitutions), std::move(errors), std::move(warnings) });
        }
        return true;
    };
    CNumericLocalesSetter locales_setter;
    if (parse_subfiles(process_subfiles, read.data.process_entries, read.parsed.process_entries, "process") &&
        parse_subfiles(filament_subfiles, read.data.filament_entries, read.parsed.filament_entries, "filament"))
        parse_subfiles(machine_subfiles, read.data.machine_entries, read.parsed.machine_entries, "printer");
}

std::pair<PresetsConfigSubstitutions, size_t> PresetBundle::install_vendor_read(VendorRead&& read, const PresetBundle* base_bundle)
{
    const std::string&         dir         = read.dir;
    const std::string&         vendor_name = read.vendor_name;
    PresetsConfigSubstitutions substitutions;
    if (read.from_cache) {
        if (this->install_vendor_cache(read.cache_path, vendor_name, std::move(read.data), base_bundle)) {
            size_t presets_loaded = 0;
            for (const PresetCollection* coll : std::initializer_list<const PresetCollection*>{
                     &this->prints, &this->sla_prints, &this->filaments, &this->sla_materials, &this->printers })
                presets_loaded += coll->m_presets.size() - coll->m_num_default_presets;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", %1% served from its preset cache, %2% presets")%vendor_name%presets_loaded;
            return std::make_pair(std::move(substitutions), presets_loaded);
        }
        this->parse_vendor_json(read);
    }
    if (read.vendor_only)
        return std::make_pair(PresetsConfigSubstitutions{}, 0);

    int parse_errors = 0;
    for (const std::vector<EntryParse>* list : { &read.parsed.process_entries, &read.parsed.filament_entries, &read.parsed.machine_entries })
        for (const EntryParse& entry_parsed : *list)
            parse_errors += int(entry_parsed.errors.size());
    const int    errors_before_install = m_errors;
    const size_t presets_loaded = install_vendor(dir, vendor_name, base_bundle, read.flags, read.data, &read.parsed,
                                                 read.reason.empty(), substitutions);
    const int install_errors = m_errors - errors_before_install - parse_errors;
    if (! read.reason.empty()) {
        log_errors(read.failed_errors);
        ++m_errors;
        std::string subfile_path = dir + "/" + vendor_name + "/" + read.failed_subfile;
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", got error when parse %1% setting from %2%") % read.failed_kind % subfile_path;
        throw failed_loading_error(subfile_path, dir);
    }

    if (read.will_cache) {
        read.data.parse_errors = uint64_t(std::max(0, m_errors - read.errors_at_entry - install_errors));
        read.data.vendors      = this->vendors;
        if (! VendorCacheFile::save((boost::filesystem::path(dir) / (vendor_name + ".opc")).string(), vendor_name,
                                    read.version, read.data))
            BOOST_LOG_TRIVIAL(warning) << "PresetBundle: failed to save vendor cache for " << vendor_name;
    }

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", finished, presets_loaded %1%")%presets_loaded;
    return std::make_pair(std::move(substitutions), presets_loaded);
}

void PresetBundle::on_extruders_count_changed(int extruders_count)
{
    printers.get_edited_preset().set_num_extruders(extruders_count);
    update_multi_material_filament_presets();
    reset_default_nozzle_volume_type();
    extruder_ams_counts.resize(extruders_count);
}

void PresetBundle::update_multi_material_filament_presets(size_t to_delete_filament_id)
{
    if (printers.get_edited_preset().printer_technology() != ptFFF)
        return;

    size_t num_filaments = this->filament_presets.size();

    auto* nozzle_diameter = static_cast<const ConfigOptionFloats*>(printers.get_edited_preset().config.option("nozzle_diameter"));
    size_t num_extruders  = nozzle_diameter->values.size();
    if (num_extruders > num_filaments) {
        for (size_t i = 0; i < std::min(this->filament_presets.size(), num_extruders); ++i)
            this->filament_presets[i] = this->filaments.find_preset(this->filament_presets[i], true)->name;
        this->filament_presets.resize(num_extruders, this->filament_presets.empty() ? this->filaments.first_visible().name :
                                                                                      this->filament_presets.back());
        num_filaments = this->filament_presets.size();
    }
    if (to_delete_filament_id == -1)
        to_delete_filament_id = num_filaments;

    std::vector<double> old_matrix = this->project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    size_t old_nozzle_nums = this->project_config.option<ConfigOptionFloats>("flush_multiplier")->values.size();
    size_t old_number_of_filaments = size_t(sqrt(old_matrix.size() / old_nozzle_nums) + EPSILON);
    size_t nozzle_nums = get_printer_extruder_count();
    if (old_nozzle_nums != nozzle_nums) {
        std::vector<double>& f_multiplier = this->project_config.option<ConfigOptionFloats>("flush_multiplier")->values;
        f_multiplier.resize(nozzle_nums, 1.f);
    }

    if (old_matrix.size() != num_filaments * num_filaments * nozzle_nums) {
        std::vector<double>& filaments = this->project_config.option<ConfigOptionFloats>("flush_volumes_vector")->values;
        while (filaments.size() < 2* num_filaments) {
            filaments.push_back(filaments.size()>1 ? filaments[0] : 140.);
            filaments.push_back(filaments.size()>1 ? filaments[1] : 140.);
        }
        while (filaments.size() > 2* num_filaments) {
            filaments.pop_back();
            filaments.pop_back();
        }

        size_t old_matrix_size = old_number_of_filaments * old_number_of_filaments;
        size_t new_matrix_size = num_filaments * num_filaments;
        std::vector<double> new_matrix(new_matrix_size * nozzle_nums, 0);
        for (unsigned int i = 0; i < num_filaments; ++i)
            for (unsigned int j = 0; j < num_filaments; ++j) {
                if (i < old_number_of_filaments && j < old_number_of_filaments) {
                    unsigned int old_i = i >= to_delete_filament_id ? i + 1 : i;
                    unsigned int old_j = j >= to_delete_filament_id ? j + 1 : j;
                    for (size_t nozzle_id = 0; nozzle_id < nozzle_nums; ++nozzle_id) {
                        if (nozzle_id < old_nozzle_nums) {
                            new_matrix[i * num_filaments + j + new_matrix_size * nozzle_id] = old_matrix[old_i * old_number_of_filaments + old_j + old_matrix_size * nozzle_id];
                        } else {
                            new_matrix[i * num_filaments + j + new_matrix_size * nozzle_id] = (i == j ? 0. : filaments[2 * i] + filaments[2 * j + 1]);
                        }
                    }
                } else {
                    for (size_t nozzle_id = 0; nozzle_id < nozzle_nums; ++nozzle_id) {
                        new_matrix[i * num_filaments + j + new_matrix_size * nozzle_id] = (i == j ? 0. : filaments[2 * i] + filaments[2 * j + 1]);
                    }
                }
            }
        this->project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = new_matrix;
    }
}

static void normalize_compatible_field(Preset &preset, const char *field_key, PresetCollection &target)
{
    auto *opt = preset.config.option<ConfigOptionStrings>(field_key);
    if (opt == nullptr)
        return;
    for (std::string &name : opt->values) {
        if (name.empty())
            continue;
        if (const Preset *resolved = target.find_preset(name, false); resolved != nullptr && resolved->name != name)
            name = resolved->name;
    }
}

void PresetBundle::normalize_compatible_presets()
{
    auto normalize = [this](PresetCollection &holders, PresetCollection *processes) {
        for (Preset &p : holders) {
            normalize_compatible_field(p, "compatible_printers", this->printers);
            if (processes != nullptr)
                normalize_compatible_field(p, "compatible_prints", *processes);
        }
    };
    normalize(this->prints,        nullptr);
    normalize(this->filaments,     &this->prints);
    normalize(this->sla_prints,    nullptr);
    normalize(this->sla_materials, &this->sla_prints);
}

void PresetBundle::update_compatible(PresetSelectCompatibleType select_other_print_if_incompatible, PresetSelectCompatibleType select_other_filament_if_incompatible)
{
    const Preset					&printer_preset					    = this->printers.get_edited_preset();
	const PresetWithVendorProfile    printer_preset_with_vendor_profile = this->printers.get_preset_with_vendor_profile(printer_preset);

    class PreferedProfileMatch
    {
    public:
        PreferedProfileMatch(const std::string &prefered_alias, const std::string &prefered_name) :
            m_prefered_alias(prefered_alias), m_prefered_name(prefered_name) {}

        int operator()(const Preset &preset) const
        {
            return
                preset.is_default || preset.is_external ?
                    0 :
                    ! m_prefered_alias.empty() && m_prefered_alias == preset.alias ?
                        std::numeric_limits<int>::max() :
                        preset.name == m_prefered_name;
        }

    private:
        const std::string  m_prefered_alias;
        const std::string &m_prefered_name;
    };

    class PreferedPrintProfileMatch : public PreferedProfileMatch
    {
    public:
        PreferedPrintProfileMatch(const Preset *preset, const std::string &prefered_name) :
            PreferedProfileMatch(preset ? preset->alias : std::string(), prefered_name), m_prefered_layer_height(preset ? preset->config.opt_float("layer_height") : 0) {}

        int operator()(const Preset &preset) const
        {
            if (preset.is_default || preset.is_external)
                return 0;
            int match_quality = PreferedProfileMatch::operator()(preset);
            if (match_quality < std::numeric_limits<int>::max()) {
                match_quality += 1;
                if (preset.is_visible)
                    match_quality += 1;
                if (m_prefered_layer_height > 0. && std::abs(preset.config.opt_float("layer_height") - m_prefered_layer_height) < 0.0005)
                    match_quality *= 10;
            }
            return match_quality;
        }

    private:
        const double m_prefered_layer_height;
    };

    class PreferedFilamentProfileMatch : public PreferedProfileMatch
    {
    public:
        PreferedFilamentProfileMatch(const Preset *preset, const std::string &prefered_name) :
            PreferedProfileMatch(preset ? preset->alias : std::string(), prefered_name),
            m_prefered_filament_type(preset ? preset->config.opt_string("filament_type", 0) : std::string()) {}

        int operator()(const Preset &preset) const
        {
            if (preset.is_default || preset.is_external)
                return 0;
            int match_quality = PreferedProfileMatch::operator()(preset);
            if (match_quality < std::numeric_limits<int>::max()) {
                match_quality += 1;
                if(preset.is_visible)
                    match_quality += 1;
                if (! m_prefered_filament_type.empty() && m_prefered_filament_type == preset.config.opt_string("filament_type", 0))
                    match_quality *= 10;
            }
            return match_quality;
        }

    private:
        const std::string m_prefered_filament_type;
    };

    class PreferedFilamentsProfileMatch
    {
    public:
        PreferedFilamentsProfileMatch(const Preset *preset, const std::vector<std::string> &prefered_names) :
            m_prefered_alias(preset ? preset->alias : std::string()),
            m_prefered_filament_type(preset ? preset->config.opt_string("filament_type", 0) : std::string("PLA")),
            m_prefered_names(prefered_names)
            {}

        int operator()(const Preset &preset) const
        {
            if (preset.is_default || preset.is_external || !preset.is_visible)
                return 0;
            if (! m_prefered_alias.empty() && m_prefered_alias == preset.alias)
                return std::numeric_limits<int>::max();
            int match_quality = (std::find(m_prefered_names.begin(), m_prefered_names.end(), preset.name) != m_prefered_names.end()) + 1;
            if (! m_prefered_filament_type.empty() && m_prefered_filament_type == preset.config.opt_string("filament_type", 0))
                match_quality *= 10;
            return match_quality;
        }

    private:
        const std::string               m_prefered_alias;
        const std::string               m_prefered_filament_type;
        const std::vector<std::string> &m_prefered_names;
    };

    BOOST_LOG_TRIVIAL(info) << boost::format("update_compatibility for all presets enter, select_other_print_if_incompatible %1%, select_other_filament_if_incompatible %2%")%(int)select_other_print_if_incompatible %(int)select_other_filament_if_incompatible;
	switch (printer_preset.printer_technology()) {
    case ptFFF:
    {
		assert(printer_preset.config.has("default_print_profile"));
		assert(printer_preset.config.has("default_filament_profile"));
        const std::vector<std::string> &prefered_filament_profiles = printer_preset.config.option<ConfigOptionStrings>("default_filament_profile")->values;
        this->prints.update_compatible(printer_preset_with_vendor_profile, nullptr, select_other_print_if_incompatible,
            PreferedPrintProfileMatch(this->prints.get_selected_idx() == size_t(-1) ? nullptr : &this->prints.get_edited_preset(), printer_preset.config.opt_string("default_print_profile")));
        const PresetWithVendorProfile   print_preset_with_vendor_profile = this->prints.get_edited_preset_with_vendor_profile();
        std::vector<char> 				filament_preset_was_compatible(this->filament_presets.size(), false);
        for (size_t idx = 0; idx < this->filament_presets.size(); ++ idx) {
            Preset *preset = this->filaments.find_preset(this->filament_presets[idx], false);
            filament_preset_was_compatible[idx] = preset != nullptr && preset->is_compatible;
        }
        BOOST_LOG_TRIVIAL(info) << boost::format("prefered filaments: size %1%, previous selected %2%") %prefered_filament_profiles.size() % this->filaments.get_selected_idx();
        if (this->filaments.get_selected_idx() != size_t(-1))
        {
            BOOST_LOG_TRIVIAL(info) << boost::format("previous selected filament： %1%") % this->filaments.get_edited_preset().name;
        }
        for (size_t idx = 0; idx < prefered_filament_profiles.size(); ++idx) {
            BOOST_LOG_TRIVIAL(info) << boost::format("prefered filament： %1%") % prefered_filament_profiles[idx];
        }
        this->filaments.update_compatible(printer_preset_with_vendor_profile, &print_preset_with_vendor_profile, select_other_filament_if_incompatible,
            PreferedFilamentsProfileMatch(this->filaments.get_selected_idx() == size_t(-1) ? nullptr : &this->filaments.get_edited_preset(), prefered_filament_profiles));
        if (select_other_filament_if_incompatible != PresetSelectCompatibleType::Never) {
            const std::string prefered_filament_profile = prefered_filament_profiles.empty() ? std::string() : prefered_filament_profiles.front();
            if (this->filament_presets.size() == 1) {
            	if (select_other_filament_if_incompatible == PresetSelectCompatibleType::Always || filament_preset_was_compatible.front())
                	this->filament_presets.front() = this->filaments.get_edited_preset().name;
            } else {
                for (size_t idx = 0; idx < this->filament_presets.size(); ++ idx) {
                    std::string &filament_name = this->filament_presets[idx];
                    Preset      *preset = this->filaments.find_preset(filament_name, false);
                    if (preset == nullptr || (! preset->is_compatible && (select_other_filament_if_incompatible == PresetSelectCompatibleType::Always || filament_preset_was_compatible[idx])))
                        filament_name = this->filaments.first_compatible(
                            PreferedFilamentProfileMatch(preset,
                                (idx < prefered_filament_profiles.size()) ? prefered_filament_profiles[idx] : prefered_filament_profile)).name;
                }
            }
        }
		break;
    }
    case ptSLA:
    {
		assert(printer_preset.config.has("default_sla_print_profile"));
		assert(printer_preset.config.has("default_sla_material_profile"));
		this->sla_prints.update_compatible(printer_preset_with_vendor_profile, nullptr, select_other_print_if_incompatible,
            PreferedPrintProfileMatch(this->sla_prints.get_selected_idx() == size_t(-1) ? nullptr : &this->sla_prints.get_edited_preset(), printer_preset.config.opt_string("default_sla_print_profile")));
        const PresetWithVendorProfile sla_print_preset_with_vendor_profile = this->sla_prints.get_edited_preset_with_vendor_profile();
		this->sla_materials.update_compatible(printer_preset_with_vendor_profile, &sla_print_preset_with_vendor_profile, select_other_filament_if_incompatible,
            PreferedProfileMatch(this->sla_materials.get_selected_idx() == size_t(-1) ? std::string() : this->sla_materials.get_edited_preset().alias, printer_preset.config.opt_string("default_sla_material_profile")));
		break;
	}
    default: break;
    }

    BOOST_LOG_TRIVIAL(info) << boost::format("update_compatibility for all presets exit");
}


std::vector<std::string> PresetBundle::export_current_configs(const std::string &                     path,
                                                              std::function<int(std::string const &)> override_confirm,
                                                              bool                                    include_modify,
                                                              bool                                    export_system_settings)
{
    const Preset &print_preset    = include_modify ? prints.get_edited_preset() : prints.get_selected_preset();
    const Preset &printer_preset  = include_modify ? printers.get_edited_preset() : printers.get_selected_preset();
    std::set<Preset const *> presets { &print_preset, &printer_preset };
    for (auto &f : filament_presets) {
        auto filament_preset = filaments.find_preset(f, include_modify);
        if (filament_preset) presets.insert(filament_preset);
    }

    int overwrite = 0;
    std::vector<std::string> result;
    for (auto preset : presets) {
        if ((preset->is_system  && !export_system_settings) || preset->is_default)
            continue;
        std::string file = path + "/" + preset->name + ".json";
        if (overwrite == 0) overwrite = 1;
        if (boost::filesystem::exists(file) && overwrite < 2) {
            overwrite = override_confirm(preset->name);
            if (overwrite == 0 || overwrite == 2)
                continue;
        }
        preset->config.save_to_json(file, preset->name, "", preset->version.to_string());
        result.push_back(file);
    }
    return result;
}

void PresetBundle::set_filament_preset(size_t idx, const std::string &name)
{
    if (idx >= filament_presets.size()) {
        BOOST_LOG_TRIVIAL(warning) << boost::format("Warning: set_filament_preset out of range %1% - %2%") % idx % filament_presets.size();
        return;
    }
    filament_presets[idx] = Preset::remove_suffix_modified(name);
}

void PresetBundle::set_default_suppressed(bool default_suppressed)
{
    prints.set_default_suppressed(default_suppressed);
    filaments.set_default_suppressed(default_suppressed);
    sla_prints.set_default_suppressed(default_suppressed);
    sla_materials.set_default_suppressed(default_suppressed);
    printers.set_default_suppressed(default_suppressed);
}

bool PresetBundle::has_errors(bool check_duplicate_filament_subtypes) const
{
    if (m_errors != 0 || printers.m_errors != 0 || filaments.m_errors != 0 || prints.m_errors != 0)
        return true;

    bool has_errors = false;
    for (auto& preset : filaments) {
        if (!preset.is_system)
            continue;
        if(preset.vendor->name == PresetBundle::ORCA_FILAMENT_LIBRARY)
            continue;
        auto* compatible_printers = dynamic_cast<const ConfigOptionStrings*>(preset.config.option("compatible_printers"));
        if (compatible_printers == nullptr || compatible_printers->values.empty()) {
            has_errors = true;
            BOOST_LOG_TRIVIAL(error) << "Filament preset \"" << preset.file << "\" is missing compatible_printers setting";
        }
    }

    if (check_duplicate_filament_subtypes && this->check_duplicate_filament_subtypes())
        has_errors = true;

    if (this->check_preset_references())
        has_errors = true;

    if (this->check_printer_default_materials())
        has_errors = true;

    return has_errors;
}

static std::string preset_file_uri(const std::string &file)
{
    std::string path;
    try {
        path = boost::filesystem::canonical(file).generic_string();
    } catch (...) {
        path = file;
    }
    std::string uri = "file://";
    if (path.empty() || path.front() != '/')
        uri += '/';
    for (char c : path) {
        switch (c) {
        case ' ': uri += "%20"; break;
        case '#': uri += "%23"; break;
        case '%': uri += "%25"; break;
        default:  uri += c;
        }
    }
    return uri;
}

bool PresetBundle::check_preset_references() const
{
    bool found = false;

    auto report_ref = [&](const Preset &p, const std::string &name, const PresetCollection &target,
                          const char *verb, const char *noun) {
        const Preset *resolved = target.find_preset(name, false);
        if (resolved == nullptr) {
            found = true;
            BOOST_LOG_TRIVIAL(error) << "Preset \"" << p.name << "\" " << verb << " unknown " << noun << " \"" << name << "\":\n"
                                     << preset_file_uri(p.file);
        } else if (resolved->name != name) {
            found = true;
            BOOST_LOG_TRIVIAL(error) << "Preset \"" << p.name << "\" " << verb << " renamed " << noun << " \"" << name
                                     << "\" (now \"" << resolved->name << "\"):\n" << preset_file_uri(p.file);
        }
    };

    auto check_list = [&](const Preset &p, const char *key, const PresetCollection &target) {
        const auto *opt = p.config.option<ConfigOptionStrings>(key);
        if (opt == nullptr)
            return;
        for (const std::string &name : opt->values)
            if (!name.empty())
                report_ref(p, name, target, "references", key);
    };

    auto check_collection = [&](const PresetCollection &holders, const PresetCollection *processes) {
        for (const Preset &p : holders) {
            if (!p.is_system)
                continue;
            if (const std::string &inh = p.inherits(); !inh.empty())
                report_ref(p, inh, holders, "inherits", "parent");
            check_list(p, "compatible_printers", this->printers);
            if (processes != nullptr)
                check_list(p, "compatible_prints", *processes);
        }
    };

    check_collection(this->printers,      nullptr);
    check_collection(this->prints,        nullptr);
    check_collection(this->filaments,     &this->prints);
    check_collection(this->sla_prints,    nullptr);
    check_collection(this->sla_materials, &this->sla_prints);

    return found;
}

bool PresetBundle::check_printer_default_materials() const
{
    bool found = false;
    std::set<const VendorProfile::PrinterModel *> checked_models;
    std::set<std::string> reported_unknown_profiles;
    for (const Preset &printer : printers) {
        if (!printer.is_system || printer.vendor == nullptr || printer.printer_technology() != ptFFF)
            continue;

        const VendorProfile::PrinterModel *model = PresetUtils::system_printer_model(printer);
        const PresetWithVendorProfile active_printer = printers.get_preset_with_vendor_profile(printer);
        const bool has_default = model != nullptr && std::any_of(model->default_materials.begin(), model->default_materials.end(),
            [&](const std::string &name) {
                const Preset *filament = filaments.find_preset(name, false);
                return filament != nullptr && filament->is_system &&
                       is_compatible_with_printer(filaments.get_preset_with_vendor_profile(*filament), active_printer);
            });
        if (!has_default) {
            found = true;
            BOOST_LOG_TRIVIAL(error) << "Printer preset \"" << printer.name << "\" (vendor \"" << printer.vendor->name
                << "\", model \"" << printer.config.opt_string("printer_model") << "\", variant \""
                << printer.config.opt_string("printer_variant")
                << "\") has no compatible system filament in its model's \"default_materials\". "
                   "Add at least one full filament preset name compatible with this printer variant:\n"
                << preset_file_uri(printer.file);
        }

        if (model != nullptr && checked_models.insert(model).second) {
            for (const std::string &name : model->default_materials) {
                const Preset *filament = filaments.find_preset(name, false);
                if (filament == nullptr || !filament->is_system) {
                    found = true;
                    BOOST_LOG_TRIVIAL(error) << "Printer model \"" << model->name << "\" (vendor \"" << printer.vendor->name
                        << "\") names the unknown system filament \"" << name
                        << "\" in its \"default_materials\":\n" << preset_file_uri(printer.file);
                }
            }
        }

        if (printer.config.has("default_filament_profile")) {
            for (const std::string &name : printer.config.opt<ConfigOptionStrings>("default_filament_profile")->values) {
                if (name.empty())
                    continue;
                const Preset *filament = filaments.find_preset(name, false);
                if ((filament == nullptr || !filament->is_system) && reported_unknown_profiles.insert(name).second) {
                    found = true;
                    BOOST_LOG_TRIVIAL(error) << "Printer preset \"" << printer.name << "\" (vendor \"" << printer.vendor->name
                        << "\", model \"" << printer.config.opt_string("printer_model") << "\", variant \""
                        << printer.config.opt_string("printer_variant")
                        << "\") names the unknown system filament \"" << name
                        << "\" in its \"default_filament_profile\":\n" << preset_file_uri(printer.file);
                }
            }
        }
    }
    return found;
}

bool PresetBundle::check_duplicate_filament_subtypes() const
{
    std::map<std::string, std::vector<const Preset *>> filaments_by_vendor;
    for (const auto &preset : filaments) {
        if (!preset.is_system || preset.filament_id.empty() || preset.vendor == nullptr)
            continue;
        filaments_by_vendor[preset.vendor->name].push_back(&preset);
    }

    const std::vector<const Preset *> no_filaments;
    const auto library_it = filaments_by_vendor.find(ORCA_FILAMENT_LIBRARY);
    const std::vector<const Preset *> &library_filaments = library_it == filaments_by_vendor.end() ? no_filaments : library_it->second;

    bool found_duplicates = false;
    for (const auto &printer : printers) {
        if (!printer.is_system || printer.vendor == nullptr)
            continue;
        auto vendor_it = filaments_by_vendor.find(printer.vendor->name);
        const std::vector<const Preset *> &vendor_filaments = vendor_it == filaments_by_vendor.end() ? no_filaments : vendor_it->second;
        if (vendor_filaments.empty() && library_filaments.empty())
            continue;

        const PresetWithVendorProfile active_printer = printers.get_preset_with_vendor_profile(printer);
        std::map<std::string, std::vector<const Preset *>> by_filament_id;
        for (const Preset *fil : vendor_filaments)
            if (is_compatible_with_printer(filaments.get_preset_with_vendor_profile(*fil), active_printer))
                by_filament_id[fil->filament_id].push_back(fil);
        if (&vendor_filaments != &library_filaments)
            for (const Preset *fil : library_filaments)
                if (is_compatible_with_printer(filaments.get_preset_with_vendor_profile(*fil), active_printer))
                    by_filament_id[fil->filament_id].push_back(fil);

        for (const auto &entry : by_filament_id) {
            if (entry.second.size() < 2)
                continue;
            found_duplicates = true;
            std::string presets;
            for (const Preset *p : entry.second) {
                presets += "\n    - " + p->name;
                if (p->vendor != nullptr && p->vendor->name != printer.vendor->name)
                    presets += " [" + p->vendor->name + "]";
                presets += "\n      " + preset_file_uri(p->file);
            }
            BOOST_LOG_TRIVIAL(error)
                << "Ambiguous AMS filament match: " << entry.second.size()
                << " filament presets share filament_id \"" << entry.first
                << "\" and are all compatible with printer \"" << printer.name
                << "\". When matching an AMS spool the slicer cannot tell them apart and"
                   " silently picks whichever loads first." << presets;
        }
    }

    if (found_duplicates)
        BOOST_LOG_TRIVIAL(error) << "\n========================================\n"
            << "How to fix \"Ambiguous AMS filament match\" errors: make sure only ONE filament"
               " preset with a given filament_id is compatible with each printer. Either"
               "\n    (a) remove the overlapping printer from a preset's \"compatible_printers\""
               " list (e.g. a '@printer' preset over-claiming a nozzle that already has its own"
               " '@printer 0.x nozzle' preset), or"
               "\n    (b) if these are genuinely different materials, give each its own"
               " \"filament_id\" - a common cause is a wrong \"inherits\" pointing at another"
               " material's @base preset.";

    return found_duplicates;
}

bool BundleMetadata::load_from_json(const std::string& path)
{
    try {
        boost::nowide::ifstream ifs(path);
        if (!ifs.good())
            return false;

        json j;
        ifs >> j;

        if (j.contains("id")) this->id = j["id"].get<std::string>();

        if (j.contains("name")) this->name = j["name"].get<std::string>();
        else if (j.contains("bundle_id")) this->name = j["bundle_id"].get<std::string>();

        if (j.contains("version")) this->version = j["version"].get<std::string>();

        if (j.contains("description")) this->description = j["description"].get<std::string>();
        else if (j.contains("bundle_type")) this->description = j["bundle_type"].get<std::string>();

        if (j.contains("author")) this->author = j["author"].get<std::string>();

        if (j.contains("imported_time")) this->imported_time = j["imported_time"].get<long long>();

        if (j.contains("updated_time")) this->updated_time = j["updated_time"].get<long long>();

        if (j.contains("print_presets"))                                                                                                                                                                                                                                      
            this->print_presets = j["print_presets"].get<std::vector<std::string>>();                                                                                                                                                                                         
                                                                                                                                                                                                                                                                                
        if (j.contains("filament_presets"))                                                                                                                                                                                                                                   
            this->filament_presets = j["filament_presets"].get<std::vector<std::string>>();                                                                                                                                                                                   
                                                                                                                                                                                                                                                                                
        if (j.contains("printer_presets"))                                                                                                                                                                                                                                    
            this->printer_presets = j["printer_presets"].get<std::vector<std::string>>();  

        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load bundle metadata from " << path << ": " << e.what();
        return false;
    }
}

bool BundleMetadata::save_to_json(const std::string& path) const
{
    auto strip_prefix = [](const std::vector<std::string>& names) {
                json arr = json::array();
                for (const auto& name : names)
                {
                    arr.push_back(boost::filesystem::path(name).filename().string());
                    std::string test = boost::filesystem::path(name).filename().string();
                }
                return arr;
            };
    try {
        json j;
        j["id"] = this->id;
        j["name"] = this->name;
        j["version"] = this->version;
        j["description"] = this->description;
        j["author"] = this->author;
        j["imported_time"] = this->imported_time;
        j["updated_time"] = this->updated_time;

        j["print_presets"] = strip_prefix(this->print_presets);                                                                                                                                                                                                                             
        j["filament_presets"] = strip_prefix(this->filament_presets);                                                                                                                                                                                                                       
        j["printer_presets"] = strip_prefix(this->printer_presets);

        const std::string content = j.dump(4);
        InstanceLock instance_lock(user_presets_lock_path());
        if (const std::error_code ec = write_file_atomically(path, content)) {
            BOOST_LOG_TRIVIAL(error) << "Failed to save bundle metadata to " << path << ": " << ec.message();
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to save bundle metadata to " << path << ": " << e.what();
        return false;
    }
}

bool PresetBundle::load_vendor_cache(const std::string& cache_path, const std::string& expected_vendor_name,
                                     const Semver& expected_vendor_version, const PresetBundle* base_bundle)
{
    VendorCacheData data;
    if (! VendorCacheFile::load(cache_path, expected_vendor_name, expected_vendor_version, data))
        return false;
    return this->install_vendor_cache(cache_path, expected_vendor_name, std::move(data), base_bundle);
}

bool PresetBundle::install_vendor_cache(const std::string& cache_path, const std::string& vendor_name, VendorCacheData&& data,
                                        const PresetBundle* base_bundle)
{
    const int errors_at_entry = this->m_errors;
    try {
        this->vendors = std::move(data.vendors);

        m_errors += int(data.parse_errors);

        PresetsConfigSubstitutions substitutions;
        install_vendor(boost::filesystem::path(cache_path).parent_path().string(), vendor_name, base_bundle,
                       LoadConfigBundleAttribute::LoadSystem, data, nullptr, true, substitutions);
        return true;
    } catch (const std::exception& e) {
        if (tbb::is_current_task_group_canceling())
            throw;
        BOOST_LOG_TRIVIAL(warning) << "PresetBundle: rejecting vendor cache " << cache_path << ": " << e.what();
        this->reset(false);
        this->vendors.clear();
        this->m_config_maps.clear();
        this->m_filament_id_maps.clear();
        this->m_errors = errors_at_entry;
        this->clear_printer_hold_aliases();
        return false;
    }
}

}
