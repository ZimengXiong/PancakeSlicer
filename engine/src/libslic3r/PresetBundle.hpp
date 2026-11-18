#ifndef slic3r_PresetBundle_hpp_
#define slic3r_PresetBundle_hpp_

#include "Config.hpp"
#include "Preset.hpp"
#include "PresetCacheFormat.hpp"
#include "AppConfig.hpp"
#include "PrintConfig.hpp"
#include "PublishSettings.hpp"
#include "Semver.hpp"
#include "enum_bitmask.hpp"

#include <climits>
#include <functional>
#include <cstddef>
#include <memory>
#include <map>
#include <set>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <optional>
#include <array>
#include <atomic>
#include <boost/filesystem/path.hpp>
#include <unordered_set>
#include <vector>
#include <utility>

#define DEFAULT_USER_FOLDER_NAME "default"
#define BUNDLE_STRUCTURE_JSON_NAME "bundle_structure.json"

#define VALIDATE_PRESETS_SUCCESS                0
#define VALIDATE_PRESETS_PRINTER_NOT_FOUND      1
#define VALIDATE_PRESETS_FILAMENTS_NOT_FOUND    2
#define VALIDATE_PRESETS_MODIFIED_GCODES        3


enum class VendorType {
    Unknown = 0,
    Klipper,
    Marlin,
    Marlin_BBL,
    Klipper_Qidi
};
namespace Slic3r {

struct AMSMapInfo
{
    std::string ams_id{""};
    std::string slot_id{""};
};
struct AMSComboInfo
{
    std::vector<std::string>              ams_filament_colors;
    std::vector<std::vector<std::string>> ams_multi_color_filment;
    std::vector<std::string>              ams_filament_presets;
    std::vector<std::string>              ams_names;
    void  clear() {
        ams_filament_colors.clear();
        ams_multi_color_filment.clear();
        ams_filament_presets.clear();
        ams_names.clear();
    }
    bool empty() {
        return ams_names.empty();
    }
};
struct MergeFilamentInfo {
    std::vector<std::vector<int>> merges;
    bool  is_empty() { return merges.empty();}
};


struct FilamentBaseInfo
{
    std::string filament_name;
    std::string filament_id;
    std::string filament_type;
    std::string vendor;
    int nozzle_temp_range_low{ 220 };
    int nozzle_temp_range_high{ 220 };
    int temperature_vitrification = INT_MAX;
    bool is_support{ false };
    bool is_system{ true };
    int  filament_printable = 3;

    int get_extruder_compatibility(int extruder_id) const {
        constexpr int bits_per_extruder  = 3;
        constexpr int extruder_mask      = (1 << bits_per_extruder) - 1;
        constexpr int max_extruder_count = 32 / bits_per_extruder;

        if (extruder_id < 0 || extruder_id >= max_extruder_count)
            return 0;
        return (m_filament_extruder_compatibility >> (bits_per_extruder * extruder_id)) & extruder_mask;
    }

    void set_filament_extruder_compatibility(int value) { m_filament_extruder_compatibility = value; }
    int  get_filament_extruder_compatibility() const    { return m_filament_extruder_compatibility; }

private:
    int  m_filament_extruder_compatibility = 0;
};

enum BundleType{
    Default = 0,
    Local,
    Subscribed,
};

struct BundleMetadata
{
    std::string                     id;
    std::string                     name;
    std::string                     version;
    std::string                     description;
    std::string                     author;
    long long                       imported_time{0};
    long long                       updated_time{0};

    BundleType                      bundle_type{Default};
    std::string                     path;

    std::vector<std::string>        print_presets;
    std::vector<std::string>        filament_presets;
    std::vector<std::string>        printer_presets;

    bool                            is_subscribed{false};
    bool                            update_available{false};
    bool                            not_found{false};
    bool                            unauthorized{false};

    bool load_from_json(const std::string& path);
    bool save_to_json(const std::string& path) const;
};

struct PresetBundleMetadata
{
    std::unordered_map<std::string, BundleMetadata> m_bundles;
    std::shared_mutex RWMtx;
    std::atomic<bool> pauseReads{false};

    void PauseRead()
    {
        pauseReads.store(true);
    }

    void UnpauseRead()
    {
        pauseReads.store(false);
    }

    void ReadLock()
    {
        RWMtx.lock_shared();
    }
    void ReadUnlock()
    {
        RWMtx.unlock_shared();
    }

    void WriteLock()
    {
        RWMtx.lock();
    }

    void WriteUnlock()
    {
        RWMtx.unlock();
    }
};

struct PublishedConfig
{
    bool                        published = false;
    std::vector<std::string>    published_keys;
    std::vector<PublishedMaterialEntry> material_keys;
    std::vector<std::string>    skipped_keys;
    std::vector<std::string>    material_replacements;
    std::map<int, int>          mixed_slot_relocations;
};

class PresetBundle
{
public:
    bool load_vendor_cache(const std::string& cache_path, const std::string& expected_vendor_name,
                          const Semver& expected_vendor_version, const PresetBundle* base_bundle = nullptr);

    void set_generate_vendor_caches(bool enable) { m_generate_vendor_caches = enable; }

    static DynamicPrintConfig construct_full_config(Preset                         &in_printer_preset,
                                                    Preset                         &in_print_preset,
                                                    const DynamicPrintConfig       &project_config,
                                                    std::vector<Preset>            &in_filament_presets,
                                                    bool                            apply_extruder,
                                                    std::optional<std::vector<int>> filament_maps_new,
                                                    std::optional<std::vector<int>> filament_volume_maps_new = std::nullopt);

    static std::string find_preset_vendor(const std::string& preset_name, Preset::Type type);
    static std::set<std::string> project_different_keys(const std::string &different_settings);
    static void convert_filament_preset_name(const std::string& machine_name, std::string& filament_name);

    PresetBundle();
    PresetBundle(const PresetBundle &rhs);
    PresetBundle& operator=(const PresetBundle &rhs);

    void            reset(bool delete_files);

    void            setup_directories();
    void            copy_files(const std::string& from);

    struct PresetPreferences {
        std::string printer_model_id;
        std::string printer_variant;
        std::string filament;
        std::string sla_material;
    };

    PresetsConfigSubstitutions load_presets(AppConfig &config, ForwardCompatibilitySubstitutionRule rule,
                                            const PresetPreferences& preferred_selection = PresetPreferences(),
                                            std::string *errors = nullptr, bool read_only = false);

    bool resolve_preset_config(DynamicPrintConfig &config, Preset::Type type,
                               const std::string &source_file,
                               ForwardCompatibilitySubstitutionRule compatibility_rule,
                               std::string &error, bool allow_source_manifest = true);
    bool resolve_preset_config_type(DynamicPrintConfig &config, Preset::Type &type,
                                    const std::string &source_file,
                                    ForwardCompatibilitySubstitutionRule compatibility_rule,
                                    std::string &error, bool allow_source_manifest = true);
    bool resolve_system_preset(DynamicPrintConfig &config, Preset::Type type, const std::string &name,
                               ForwardCompatibilitySubstitutionRule compatibility_rule, std::string &error);

    bool resolve_preset_config(DynamicPrintConfig &config, Preset::Type type, const std::string &source_file,
                               ForwardCompatibilitySubstitutionRule compatibility_rule, std::string &error,
                               bool allow_source_manifest, std::string *filament_id);
    bool resolve_preset_config_type(DynamicPrintConfig &config, Preset::Type &type, const std::string &source_file,
                                    ForwardCompatibilitySubstitutionRule compatibility_rule, std::string &error,
                                    bool allow_source_manifest, std::string *filament_id);
    bool resolve_system_preset(DynamicPrintConfig &config, Preset::Type type, const std::string &name,
                               ForwardCompatibilitySubstitutionRule compatibility_rule, std::string &error,
                               std::string *filament_id);

    void     load_selections(AppConfig &config, const PresetPreferences& preferred_selection = PresetPreferences());

    PresetsConfigSubstitutions load_user_presets(std::string user, ForwardCompatibilitySubstitutionRule rule, bool read_only = false);
    PresetsConfigSubstitutions load_user_presets(AppConfig &config, std::map<std::string, std::map<std::string, std::string>>& my_presets, ForwardCompatibilitySubstitutionRule rule);
    PresetsConfigSubstitutions update_subscribed_presets(AppConfig& config,
                                                         const std::map<std::string, std::map<std::string, std::string>>& bundle_presets,
                                                         const BundleMetadata& remote_metadata,
                                                         ForwardCompatibilitySubstitutionRule rule);

    PresetsConfigSubstitutions import_presets(std::vector<std::string>& files,
                                              std::function<int(std::string const&)> override_confirm,
                                              ForwardCompatibilitySubstitutionRule rule,
                                              AppConfig& config);

    bool import_json_presets(PresetsConfigSubstitutions& substitutions,
                             std::string& file,
                             std::function<int(std::string const&)> override_confirm,
                             ForwardCompatibilitySubstitutionRule rule,
                             int& overwrite,
                             std::vector<std::string>& result,
                             const std::string& bundle_dir = "");
                             
    void save_user_presets(AppConfig& config, std::map<std::string, std::string>& need_to_delete_list);
    void check_and_fix_user_presets_syncinfo(const std::string& user_id);
    void remove_users_preset(AppConfig &config, std::map<std::string, std::map<std::string, std::string>> * my_presets = nullptr);
    void update_user_presets_directory(const std::string preset_folder);
    void remove_user_presets_directory(const std::string preset_folder);
    void update_system_preset_setting_ids(std::map<std::string, std::map<std::string, std::string>>& system_presets);

    bool apply_vendor_config(
        const std::map<std::string, std::map<std::string, std::set<std::string>>>& new_vendors,
        const std::map<std::string, std::string>& new_filaments,
        AppConfig* app_config,
        bool overwrite = true,
        const std::string& preferred_printer_model = std::string(),
        const std::string& preferred_printer_variant = std::string(),
        const std::string& preferred_filament = std::string());

    int validate_presets(const std::string &file_name, DynamicPrintConfig& config, std::set<std::string>& different_gcodes);

    Preset* get_preset_differed_for_save(Preset& preset);
    int get_differed_values_to_update(Preset& preset, std::map<std::string, std::string>& key_values);


    Semver get_vendor_profile_version(std::string vendor_name);

    std::optional<FilamentBaseInfo> get_filament_by_filament_id(const std::string& filament_id, const std::string& printer_name = std::string()) const;

    VendorType get_current_vendor_type();
    bool is_bbl_vendor() { return get_current_vendor_type() == VendorType::Marlin_BBL; }

    bool use_bbl_network();
    bool use_bbl_device_tab();

    bool backup_user_folder() const;

    PresetsConfigSubstitutions load_project_embedded_presets(std::vector<Preset*> project_presets, ForwardCompatibilitySubstitutionRule substitution_rule);
    std::vector<Preset*> get_current_project_embedded_presets();
    void reset_project_embedded_presets();

    std::string get_texture_for_printer_model(std::string model_name);
    std::string get_stl_model_for_printer_model(std::string model_name);
    std::string get_hotend_model_for_printer_model(std::string model_name);

    void            export_selections(AppConfig &config);

    void            set_num_filaments(unsigned int n, std::string new_col = "");
    void         update_num_filaments(unsigned int to_del_flament_id);

    void get_ams_cobox_infos(AMSComboInfo &combox_info);
    unsigned int sync_ams_list(std::vector<std::pair<DynamicPrintConfig *,std::string>> &unknowns, bool use_map, std::map<int, AMSMapInfo> &maps, bool enable_append, MergeFilamentInfo &merge_info, bool color_only = false);
    bool is_the_only_edited_filament(unsigned int filament_index);

    void reset_default_nozzle_volume_type();

    std::vector<int> get_used_tpu_filaments(const std::vector<int> &used_filaments);
    void           update_selections(AppConfig &config);
    void set_calibrate_printer(std::string name);

    void set_is_validation_mode(bool mode) { validation_mode = mode; }
    void set_vendor_to_validate(std::string vendor) { vendor_to_validate = vendor; }

    std::vector<std::vector<DynamicPrintConfig>> get_extruder_filament_info() const;

    std::set<std::string> get_printer_names_by_printer_type_and_nozzle(const std::string &printer_type, std::string nozzle_diameter_str, bool system_only = true);
    std::vector<Preset *> get_filament_presets_for_machine(const std::string &printer_type,
                                                           const std::string &nozzle_diameter_str,
                                                           bool               include_user_presets);
    static int            get_filament_variant_index(const DynamicPrintConfig &filament_config,
                                                     const DynamicPrintConfig &printer_config,
                                                     int                       extruder_id,
                                                     NozzleVolumeType          nozzle_volume_type);
    bool                  check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(const std::string &printer_type,
                                                                                               std::string &      nozzle_diameter_str,
                                                                                               std::string &      setting_id,
                                                                                               std::string &      tag_uid,
                                                                                               std::string &      nozzle_temp_min,
                                                                                               std::string &      nozzle_temp_max,
                                                                                               std::string &      preset_setting_id,
                                                                                               int                extruder_id,
                                                                                               NozzleVolumeType   nozzle_volume_type);
    Preset *                    get_similar_printer_preset(std::string printer_model, std::string printer_variant);

    PresetCollection            prints;
    PresetCollection            sla_prints;
    PresetCollection            filaments;
    PresetCollection            sla_materials;
	PresetCollection& 			materials(PrinterTechnology pt)       { return pt == ptFFF ? this->filaments : this->sla_materials; }
	const PresetCollection& 	materials(PrinterTechnology pt) const { return pt == ptFFF ? this->filaments : this->sla_materials; }
    PrinterPresetCollection     printers;
    PhysicalPrinterCollection   physical_printers;
    std::vector<std::string>    filament_presets;
    std::map<int, DynamicPrintConfig> filament_ams_list;
    std::vector<std::vector<std::string>> ams_multi_color_filment;

    std::vector<std::map<int, int>> extruder_ams_counts;

    Preset const * calibrate_printer = nullptr;
    std::set<Preset const *> calibrate_filaments;

    DynamicPrintConfig          project_config;

    VendorMap                   vendors;

    std::map<std::string, DynamicPrintConfig> m_config_maps;
    std::map<std::string, std::string> m_filament_id_maps;
    std::map<std::pair<Preset::Type, std::string>, DynamicPrintConfig> base_configs;

    boost::filesystem::path dir_user_presets_local;
    boost::filesystem::path dir_user_presets_subscribed;
    PresetBundleMetadata bundles;

        struct ObsoletePresets
    {
        std::vector<std::string> prints;
        std::vector<std::string> sla_prints;
        std::vector<std::string> filaments;
        std::vector<std::string> sla_materials;
        std::vector<std::string> printers;
    };
    ObsoletePresets             obsolete_presets;

    bool                        has_defauls_only() const
        { return prints.has_defaults_only() && filaments.has_defaults_only() && printers.has_defaults_only(); }

    DynamicPrintConfig          full_config(bool apply_extruder = true, std::optional<std::vector<int>>filament_maps = std::nullopt, std::optional<std::vector<int>> filament_volume_maps = std::nullopt) const;
    DynamicPrintConfig          full_config_secure(std::optional<std::vector<int>>filament_maps = std::nullopt) const;

    std::vector<int> get_default_nozzle_volume_types_for_filaments(std::vector<int>& f_maps);

    std::vector<std::vector<std::vector<float>>> get_full_flush_matrix(bool with_multiplier = true) const;

    int get_printer_extruder_count() const;
    bool support_different_extruders() const;

    void update_filament_count();

    void                        load_config_from_wizard(const std::string &name, DynamicPrintConfig config, Semver file_version)
        { this->load_config_file_config(name, false, std::move(config), file_version, true); }

    void                        load_config_model(const std::string &name, DynamicPrintConfig config, Semver file_version = Semver(), PublishedConfig *published_config = nullptr)
        { this->load_config_file_config(name, true, std::move(config), file_version, false, published_config); }

    ConfigSubstitutions         load_config_file(const std::string &path, ForwardCompatibilitySubstitutionRule compatibility_rule);

    enum LoadConfigBundleAttribute {
        SaveImported,
        ResetUserProfile,
        LoadSystem,
        LoadVendorOnly,
        LoadFilamentOnly,
        KeepBases,
    };
    using LoadConfigBundleAttributes = enum_bitmask<LoadConfigBundleAttribute>;
    std::pair<PresetsConfigSubstitutions, size_t> load_vendor_configs_from_json(
        const std::string &dir, const std::string &vendor_name, LoadConfigBundleAttributes flags,
        ForwardCompatibilitySubstitutionRule compatibility_rule, const PresetBundle* base_bundle = nullptr,
        bool allow_cache = true);

    std::vector<std::string> export_current_configs(const std::string &path, std::function<int(std::string const &)> override_confirm,
        bool include_modify, bool export_system_settings = false);

    void                        set_default_suppressed(bool default_suppressed);

    void                        set_filament_preset(size_t idx, const std::string &name);

    void                        update_multi_material_filament_presets(size_t to_delete_filament_id = size_t(-1));
    bool                        is_mixed_filament(size_t idx) const;
    std::vector<size_t>         physical_filament_config_indices() const;
    size_t                      num_mixed_filaments() const;
    size_t                      num_physical_filaments() const;

    void                        on_extruders_count_changed(int extruder_count);

    void                        update_compatible(PresetSelectCompatibleType select_other_print_if_incompatible, PresetSelectCompatibleType select_other_filament_if_incompatible);
    void                        update_compatible(PresetSelectCompatibleType select_other_if_incompatible) { this->update_compatible(select_other_if_incompatible, select_other_if_incompatible); }

    void                        normalize_compatible_presets();

    void                        load_installed_printers(const AppConfig &config);

    const std::string&          get_preset_name_by_alias(const Preset::Type& preset_type, const std::string& alias) const;

    const int                   get_required_hrc_by_filament_type(const std::string& filament_type) const;
    void                        save_changes_for_preset(const std::string& new_name, Preset::Type type, const std::vector<std::string>& unselected_options, bool save_to_project = false);

    std::pair<PresetsConfigSubstitutions, std::string> load_system_models_from_json(ForwardCompatibilitySubstitutionRule compatibility_rule);
    std::pair<PresetsConfigSubstitutions, std::string> load_system_filaments_json(ForwardCompatibilitySubstitutionRule compatibility_rule);
    VendorProfile                                      get_custom_vendor_models() const;

    static const char *ORCA_DEFAULT_BUNDLE;
	static const char *ORCA_DEFAULT_PRINTER_MODEL;
	static const char *ORCA_DEFAULT_PRINTER_VARIANT;
	static const char *ORCA_DEFAULT_FILAMENT;
    static const char *ORCA_FILAMENT_LIBRARY;
    static const char *ORCA_DEFAULT_FILAMENT_PLACEHOLDER;


    static std::array<Preset::Type, 3>  types_list(PrinterTechnology pt) {
        if (pt == ptFFF)
            return  { Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT };
        return      { Preset::TYPE_PRINTER, Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL };
    }

    bool has_errors(bool check_duplicate_filament_subtypes = false) const;

    int error_count() const { return m_errors; }

    bool check_preset_references() const;

    bool check_printer_default_materials() const;

    struct VendorSource
    {
        std::string             name;
        boost::filesystem::path dir;
    };

    std::pair<PresetsConfigSubstitutions, std::string> load_vendors(const std::vector<VendorSource>& vendors,
        ForwardCompatibilitySubstitutionRule compatibility_rule, bool allow_cache,
        const std::atomic<bool>* cancel = nullptr, std::vector<std::string>* failed = nullptr);

private:
    std::vector<std::vector<std::string>> merge_presets(const std::vector<PresetBundle*> &others);

    struct EntryParse
    {
        ConfigSubstitutions      substitutions;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
    };

    struct VendorParse
    {
        std::vector<EntryParse> process_entries;
        std::vector<EntryParse> filament_entries;
        std::vector<EntryParse> machine_entries;
    };

    struct VendorRead
    {
        std::string                          dir;
        std::string                          vendor_name;
        LoadConfigBundleAttributes           flags;
        ForwardCompatibilitySubstitutionRule compatibility_rule;
        int                                  errors_at_entry { 0 };
        bool                                 cacheable { false };
        bool                                 from_cache { false };
        std::string                          cache_path;
        bool                                 vendor_only { false };
        VendorCacheData                      data;
        VendorParse                          parsed;
        bool                                 will_cache { false };
        std::string                          version;
        std::string                          reason;
        std::string                          failed_subfile;
        const char*                          failed_kind { nullptr };
        std::vector<std::string>             failed_errors;
    };

    VendorRead read_vendor(const std::string& dir, const std::string& vendor_name, LoadConfigBundleAttributes flags,
                           ForwardCompatibilitySubstitutionRule compatibility_rule, bool allow_cache);

    void parse_vendor_json(VendorRead& read);

    std::pair<PresetsConfigSubstitutions, size_t> install_vendor_read(VendorRead&& read, const PresetBundle* base_bundle);

    bool install_vendor_cache(const std::string& cache_path, const std::string& vendor_name, VendorCacheData&& data,
                              const PresetBundle* base_bundle);

    void log_errors(const std::vector<std::string>& errors);

    struct VendorInstall
    {
        std::string                path;
        std::string                vendor_name;
        const VendorProfile*       vendor_profile;
        const PresetBundle*        base_bundle;
        LoadConfigBundleAttributes flags;
        PresetCollection*          presets;
        bool                       is_from_lib;
        PresetsConfigSubstitutions* substitutions;
        std::set<std::string>      inherited;
        std::set<std::string>      included;
        std::map<std::string, DynamicPrintConfig> config_maps;
        std::map<std::string, DynamicPrintConfig> include_maps;
        std::map<std::string, std::string>        filament_id_maps;
        std::unordered_set<std::string>           installed_names;
        size_t                     count { 0 };
    };

    size_t install_vendor(const std::string& path, const std::string& vendor_name, const PresetBundle* base_bundle,
                          LoadConfigBundleAttributes flags, const VendorCacheData& entries,
                          VendorParse* parsed, bool complete, PresetsConfigSubstitutions& substitutions);

    void install_vendor_entries(VendorInstall& install, const std::vector<CachedPreset>& entries,
                                std::vector<EntryParse>* parsed);

    struct PresetInstall
    {
        DynamicPrintConfig       config;
        std::string              file_path;
        std::string              alias;
        std::string              filament_id;
        std::vector<std::string> renamed_from;
        std::vector<std::string> errors;
        std::optional<DynamicPrintConfig> included;
        std::optional<DynamicPrintConfig> retained;
        bool                     config_only { false };
        std::string              reason;
    };

    PresetInstall resolve_vendor_preset(const CachedPreset& entry, const VendorInstall& install) const;

    std::string commit_vendor_preset(const CachedPreset& entry, PresetInstall&& resolved,
                                     ConfigSubstitutions&& substitutions, VendorInstall& install);

    void clear_printer_hold_aliases();

    bool m_generate_vendor_caches { false };
    bool m_preserve_vendor_source_paths { false };

    std::map<std::tuple<std::string, std::string, ForwardCompatibilitySubstitutionRule, bool>, std::unique_ptr<PresetBundle>>
        m_source_vendor_bundles;

    const PresetBundle *load_source_vendor(const boost::filesystem::path &root_dir,
                                           const std::string &vendor_id,
                                           ForwardCompatibilitySubstitutionRule compatibility_rule,
                                           std::string &error, bool allow_cache = false);

    bool check_duplicate_filament_subtypes() const;

    std::pair<PresetsConfigSubstitutions, std::string> load_system_presets_from_json(ForwardCompatibilitySubstitutionRule compatibility_rule, bool write_caches = true);
    void update_filament_multi_color();
    void 						update_system_maps();

    void                        load_installed_filaments(AppConfig &config);
    void                        load_installed_sla_materials(AppConfig &config);

    void                        load_config_file_config(const std::string &name_or_path, bool is_external, DynamicPrintConfig &&config, Semver file_version = Semver(), bool selected = false, PublishedConfig *published_config = nullptr);

    DynamicPrintConfig          full_fff_config(bool apply_extruder, std::optional<std::vector<int>> filament_maps=std::nullopt, std::optional<std::vector<int>> filament_volume_maps=std::nullopt) const;
    DynamicPrintConfig          full_sla_config() const;

    bool validation_mode = false;
    std::string vendor_to_validate = "";
    int m_errors = 0;

    bool save_preset_to_bundle_dir(Preset& preset, PresetCollection* collection,
                                   const std::string& bundle_id, const std::string& type_subdir,
                                   const std::string& bundle_base_dir);

};

ENABLE_ENUM_BITMASK_OPERATORS(PresetBundle::LoadConfigBundleAttribute)

}

#endif
