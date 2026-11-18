#pragma once
#include "libslic3r/Config.hpp"
#include "libslic3r/Nonplanar/HeadEnvelope.hpp"
#include "libslic3r/Nonplanar/Validate.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <memory>
#include <cstdio>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <set>
#include <fstream>
#include <tuple>
#include <initializer_list>
#include <ios>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace {
void write_catalog(const std::string &path, const nlohmann::json &catalog)
{
    boost::nowide::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << catalog.dump(2) << '\n';
    out.close();
    if (!out) throw std::runtime_error("cannot write catalog " + path);
}

std::string option_type_name(ConfigOptionType type)
{
    switch (type) {
    case coNone: return "none";
    case coFloat: return "float"; case coFloats: return "floats";
    case coInt: return "int"; case coInts: return "ints";
    case coString: return "string"; case coStrings: return "strings";
    case coPercent: return "percent"; case coPercents: return "percents";
    case coFloatOrPercent: return "float_or_percent"; case coFloatsOrPercents: return "floats_or_percents";
    case coPoint: return "point"; case coPoints: return "points"; case coPoint3: return "point3";
    case coBool: return "bool"; case coBools: return "bools";
    case coEnum: return "enum"; case coEnums: return "enums";
    case coPointsGroups: return "points_groups"; case coIntsGroups: return "ints_groups";
    default: return "unknown";
    }
}
void dump_options(const std::string &path)
{
    const std::set<std::string> printer(Preset::printer_options().begin(), Preset::printer_options().end()),
        filament(Preset::filament_options().begin(), Preset::filament_options().end());
    nlohmann::json options = nlohmann::json::array();
    for (const auto &[key, def] : print_config_def.options) {
        options.push_back({{"key", key}, {"type", option_type_name(def.type)}, {"type_id", int(def.type)},
            {"label", def.label}, {"full_label", def.full_label}, {"tooltip", def.tooltip},
            {"category", def.category}, {"sidetext", def.sidetext}, {"enum_values", def.enum_values},
            {"enum_labels", def.enum_labels}, {"min", def.min}, {"max", def.max}, {"mode", int(def.mode)},
            {"default", def.default_value ? def.default_value->serialize() : ""},
            {"nullable", def.nullable}, {"readonly", def.readonly}, {"cli", def.cli},
            {"profile", printer.count(key) ? "machine" : filament.count(key) ? "filament" : "process"}});
        if (key == "nonplanar_curved_max_angle") {
            nlohmann::json by_mode = nlohmann::json::object();
            for (const auto &[name, value] : ConfigOptionEnum<NonplanarMode>::get_enum_values())
                if (const double limit = Nonplanar::curved_max_angle_limit(NonplanarMode(value)); limit > 0.)
                    by_mode[name] = limit;
            options.back()["max_by_mode"] = by_mode;
        }
    }
    write_catalog(path, {{"schema", "orca.options.v1"}, {"options", options}});
}

nlohmann::json read_catalog_json(const fs::path &path)
{
    boost::nowide::ifstream in(path.string());
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return nlohmann::json::parse(in);
}

constexpr const char *CATALOG_SCHEMA = "pancake.catalog.v1";

bool catalog_metadata_key(const std::string &key)
{
    return key == "inherits" || boost::algorithm::starts_with(key, "compatible_") || boost::algorithm::ends_with(key, "_settings_id");
}

template<class Json> void apply_record_values(DynamicPrintConfig &config, const Json &values)
{
    for (const auto &[key, value] : values.items()) {
        ConfigOption *option = config.optptr(key, true);
        if (option == nullptr || !option->deserialize(value.template get<std::string>()))
            throw std::runtime_error("catalog value of " + key + " is invalid");
    }
}

std::map<std::string, std::string> variant_projection(const DynamicPrintConfig &base, const std::vector<std::string> &variants)
{
    std::map<std::string, std::string> out;
    const auto *named = base.option<ConfigOptionStrings>("filament_extruder_variant");
    if (named == nullptr || named->values == variants)
        return out;
    for (const std::string &key : filament_options_with_variant) {
        const auto *from = dynamic_cast<const ConfigOptionVectorBase *>(base.option(key));
        if (key == "filament_extruder_variant" || from == nullptr || from->empty())
            continue;
        const std::vector<std::string> entries = from->vserialize();
        const bool uniform = std::all_of(entries.begin(), entries.end(), [&](const std::string &e) { return e == entries.front(); });
        std::unique_ptr<ConfigOption> projected(from->clone());
        auto *to = dynamic_cast<ConfigOptionVectorBase *>(projected.get());
        to->resize(variants.size(), nullptr);
        bool ok = true;
        for (size_t j = 0; j < variants.size() && ok; ++j) {
            const auto it = std::find(named->values.begin(), named->values.end(), variants[j]);
            const size_t i = size_t(it - named->values.begin());
            if (it != named->values.end() && i < from->size())
                to->set_at(from, j, i);
            else if (uniform)
                to->set_at(from, j, 0);
            else
                ok = false;
        }
        if (ok)
            out[key] = projected->serialize();
    }
    return out;
}

std::vector<std::string> tuning_variants(const DynamicPrintConfig &base, const DynamicPrintConfig &tuning)
{
    const auto *own = tuning.option<ConfigOptionStrings>("filament_extruder_variant");
    if (own != nullptr)
        return own->values;
    const auto *named = base.option<ConfigOptionStrings>("filament_extruder_variant");
    return named ? named->values : std::vector<std::string>{};
}

fs::path catalog_file(const std::string &catalog, const std::string &kind) { return fs::path(catalog) / (kind + ".jsonl"); }

nlohmann::json catalog_product(const std::string &catalog, const std::string &kind, const std::string &id)
{
    const fs::path          file = catalog_file(catalog, kind);
    boost::nowide::ifstream in(file.string(), std::ios::binary);
    std::string             line;
    if (!in || !std::getline(in, line) || nlohmann::json::parse(line).value("schema", "") != CATALOG_SCHEMA)
        throw std::runtime_error("no " + std::string(CATALOG_SCHEMA) + " catalog at " + file.string());
    const std::string prefix = "{\"id\":" + nlohmann::json(id).dump() + ",";
    while (std::getline(in, line))
        if (line.compare(0, prefix.size(), prefix) == 0)
            return nlohmann::json::parse(line);
    throw std::runtime_error(kind + " \"" + id + "\" is not in the catalog");
}

std::pair<nlohmann::json, nlohmann::json> catalog_variant(const std::string &catalog, const std::string &variant)
{
    const fs::path          file = catalog_file(catalog, "printers");
    boost::nowide::ifstream in(file.string(), std::ios::binary);
    const std::string       needle = "{\"id\":" + nlohmann::json(variant).dump() + ",";
    std::string             line;
    std::getline(in, line);
    while (std::getline(in, line))
        if (line.find(needle) != std::string::npos) {
            nlohmann::json printer = nlohmann::json::parse(line);
            for (const nlohmann::json &v : printer.at("variants"))
                if (v.at("id") == variant)
                    return {std::move(printer), v};
        }
    throw std::runtime_error("printer variant \"" + variant + "\" is not in the catalog");
}

namespace flatten {
const std::vector<std::string> source_metadata = {"compatible_printers", "compatible_printers_condition", "compatible_prints", "compatible_prints_condition"};

struct Source
{
    std::string vendor, version, name, file;
    std::vector<std::pair<std::string, std::string>> chain;
    nlohmann::ordered_json values;
    nlohmann::ordered_json json;
    std::vector<std::string> fits, named;
    bool library = false;
};

nlohmann::ordered_json serialize(const DynamicPrintConfig &config)
{
    nlohmann::ordered_json values = nlohmann::ordered_json::object();
    for (const auto &key : config.keys())
        if (!catalog_metadata_key(key)) values[key] = config.opt_serialize(key);
    return values;
}

std::string product_id(const std::string &name)
{
    std::string id = name.substr(0, name.find(" @"));
    boost::algorithm::trim(id);
    return id;
}

std::string nozzle_text(const DynamicPrintConfig &config)
{
    std::ostringstream out;
    out << config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0);
    return out.str();
}

struct Catalog
{
    fs::path profiles;
    std::map<std::tuple<std::string, Preset::Type, std::string>, nlohmann::ordered_json> bases;
    std::vector<Source> machines, processes, filaments;
    std::map<std::string, nlohmann::ordered_json> models;
    std::map<std::string, std::string> model_vendor, vendor_names;
    std::map<std::string, size_t> rules;
    size_t pairs = 0, shadowed = 0;
    std::vector<std::string> shadows, splits;
    std::map<std::pair<Preset::Type, std::string>, std::string> product_of;
    std::map<std::string, std::set<std::string>> print_fits;
    std::vector<std::string> repaired_default_print;
    std::vector<std::string> first_bases;
    std::vector<std::string> unbranded;
    std::map<Preset::Type, size_t> tuning_keys;
    std::map<std::pair<std::string, std::string>, std::string> folds;
    std::map<std::pair<std::string, std::string>, size_t> folded;

    std::string group_of(const Source &s, Preset::Type type)
    {
        const std::string id = product_id(s.name);
        if (type != Preset::TYPE_FILAMENT) return id;
        const auto fold = folds.find({s.vendor, id});
        if (fold == folds.end()) return id;
        ++folded[fold->first];
        return fold->second;
    }

    nlohmann::ordered_json read(const fs::path &path)
    {
        boost::nowide::ifstream in(path.string());
        if (!in) throw std::runtime_error("cannot read " + path.string());
        return nlohmann::ordered_json::parse(in);
    }

    void load_vendor(const std::string &vendor, const PresetBundle *library_bundle)
    {
        const auto rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
        const bool is_library = library_bundle == nullptr;
        PresetBundle bundle, resolver;
        bundle.load_vendor_configs_from_json(profiles.string(), vendor, PresetBundle::LoadSystem | PresetBundle::KeepBases, rule,
                                             library_bundle, false);
        if (bundle.error_count()) throw std::runtime_error("invalid vendor preset bundle " + vendor);
        const auto manifest = read(profiles / (vendor + ".json"));
        const std::string version = manifest.value("version", "");
        std::map<std::string, std::pair<std::string, std::string>> files;
        for (const char *list : {"machine_list", "process_list", "filament_list"})
            if (manifest.contains(list)) for (const auto &entry : manifest.at(list)) {
                const std::string sub_path = entry.at("sub_path").get<std::string>();
                const auto source = read(profiles / vendor / sub_path);
                files[source.at("name").get<std::string>()] = {vendor + "/" + sub_path, source.value("inherits", "")};
            }
        if (manifest.contains("machine_model_list")) for (const auto &entry : manifest.at("machine_model_list")) {
            auto model = read(profiles / vendor / entry.at("sub_path").get<std::string>());
            const std::string name = model.at("name").get<std::string>();
            if (models.count(name)) throw std::runtime_error("printer model " + name + " is in two vendors");
            models[name] = std::move(model);
            model_vendor[name] = vendor;
        }
        vendor_names[vendor] = manifest.value("name", vendor);
        for (const auto &[key, config] : bundle.base_configs) {
            DynamicPrintConfig normalized = config;
            Preset::normalize(normalized);
            bases[{vendor, key.first, key.second}] = serialize(normalized);
        }
        auto owner = [&](const std::string &name, Preset::Type type) {
            if (files.count(name) || bases.count({vendor, type, name})) return vendor;
            return std::string(PresetBundle::ORCA_FILAMENT_LIBRARY);
        };
        for (const auto &[kind, type, collection, out] : std::initializer_list<std::tuple<const char *, Preset::Type, const PresetCollection *, std::vector<Source> *>>{
                {"machine", Preset::TYPE_PRINTER, &bundle.printers, &machines}, {"process", Preset::TYPE_PRINT, &bundle.prints, &processes},
                {"filament", Preset::TYPE_FILAMENT, &bundle.filaments, &filaments}}) {
            for (const Preset &preset : collection->get_presets()) {
                if (preset.is_default) continue;
                const auto file = files.find(preset.name);
                if (file == files.end()) throw std::runtime_error(vendor + ": no source file for " + preset.name);
                DynamicPrintConfig resolved;
                std::string        error, filament_id;
                if (!resolver.resolve_preset_config(resolved, type, (profiles / file->second.first).string(),
                                                    rule, error, true, &filament_id))
                    throw std::runtime_error("cannot resolve " + file->second.first + ": " + error);
                if (!(resolved == preset.config) || filament_id != preset.filament_id)
                    throw std::runtime_error(vendor + ": " + preset.name + " resolves differently from the vendor load");
                Source src;
                src.vendor = vendor; src.version = version; src.name = preset.name; src.file = file->second.first; src.library = is_library;
                src.values = serialize(resolved);
                DynamicPrintConfig back;
                apply_record_values(back, src.values);
                for (const auto &[key, value] : src.values.items())
                    if (!back.has(key) || back.opt_serialize(key) != value.get<std::string>())
                        throw std::runtime_error(vendor + ": " + preset.name + " does not round-trip " + key);
                nlohmann::ordered_json chain = nlohmann::ordered_json::array();
                std::set<std::string> seen;
                for (std::string parent = file->second.second; !parent.empty();) {
                    if (!seen.insert(parent).second) throw std::runtime_error("cyclic preset chain " + parent);
                    const std::string from = owner(parent, type);
                    src.chain.emplace_back(from, parent);
                    chain.push_back(parent);
                    if (from == vendor) {
                        const auto it = files.find(parent);
                        if (it == files.end()) throw std::runtime_error("missing preset parent " + parent);
                        parent = it->second.second;
                    } else {
                        const auto source = library_raw.find(parent);
                        if (source == library_raw.end()) throw std::runtime_error("missing library parent " + parent);
                        parent = source->second;
                    }
                }
                src.json = {{"name", preset.name}, {"vendor", vendor}, {"version", version}, {"file", src.file}, {"inherits", chain}};
                for (const std::string &key : source_metadata)
                    if (resolved.has(key)) src.json[key] = resolved.opt_serialize(key);
                if (type == Preset::TYPE_PRINTER) {
                    PrintConfig printer;
                    printer.apply(preset.config, true);
                    src.json["native_support_capable"] = Nonplanar::native_support_capable(printer);
                }
                if (type == Preset::TYPE_FILAMENT) {
                    src.json["filament_id"] = preset.filament_id;
                    src.json["setting_id"]  = preset.setting_id;
                }
                if (type != Preset::TYPE_PRINTER && !is_library) {
                    const PresetWithVendorProfile self = collection->get_preset_with_vendor_profile(preset);
                    const auto *named = preset.config.option<ConfigOptionStrings>("compatible_printers");
                    for (const Preset &printer : bundle.printers.get_presets())
                        if (!printer.is_default && is_compatible_with_printer(self, bundle.printers.get_preset_with_vendor_profile(printer))) {
                            src.fits.push_back(printer.name);
                            if (named && std::find(named->values.begin(), named->values.end(), printer.name) != named->values.end())
                                src.named.push_back(printer.name);
                        }
                }
                if (is_library) library_raw.emplace(preset.name, file->second.second);
                out->push_back(std::move(src));
            }
        }
        if (is_library)
            for (const auto &[name, entry] : files) library_raw.emplace(name, entry.second);
    }
    std::map<std::string, std::string> library_raw;
    nlohmann::ordered_json base_of(const std::string &id, Preset::Type type, const std::vector<const Source *> &sources, nlohmann::ordered_json &from)
    {
        for (const Source *s : sources)
            if (s->library && s->name == id + " @System") {
                from = {{"rule", "system"}, {"vendor", s->vendor}, {"name", s->name}};
                ++rules["system"];
                return s->values;
            }
        std::set<std::pair<std::string, std::string>> own;
        for (const Source *s : sources)
            for (const auto &a : s->chain)
                if (a.second == id + " @base") own.insert(a);
        if (own.size() == 1) {
            const auto &a = *own.begin();
            from = {{"rule", "base"}, {"vendor", a.first}, {"name", a.second}};
            ++rules["base"];
            return ancestor(a, type, sources);
        }
        std::vector<const Source *> tunings;
        for (const Source *s : sources)
            if (!s->library) tunings.push_back(s);
        if (tunings.empty()) tunings = sources;
        auto lineage = [](const Source *s) {
            std::vector<std::pair<std::string, std::string>> l = {{s->vendor, s->name}};
            l.insert(l.end(), s->chain.begin(), s->chain.end());
            return l;
        };
        for (const auto &a : lineage(tunings.front())) {
            bool shared = true;
            for (const Source *s : tunings) {
                const auto l = lineage(s);
                shared = shared && std::find(l.begin(), l.end(), a) != l.end();
            }
            if (shared) {
                from = {{"rule", "ancestor"}, {"vendor", a.first}, {"name", a.second}};
                ++rules["ancestor"];
                return ancestor(a, type, sources);
            }
        }
        const Source *first = tunings.front();
        from = {{"rule", "first"}, {"vendor", first->vendor}, {"name", first->name}};
        ++rules["first"];
        first_bases.push_back(id + " (" + first->vendor + "/" + first->name + ")");
        return first->values;
    }
    nlohmann::ordered_json ancestor(const std::pair<std::string, std::string> &a, Preset::Type type, const std::vector<const Source *> &sources)
    {
        for (const Source *s : sources)
            if (s->vendor == a.first && s->name == a.second) return s->values;
        for (const std::vector<Source> *all : {&machines, &processes, &filaments})
            for (const Source &s : *all)
                if (s.vendor == a.first && s.name == a.second) return s.values;
        const auto it = bases.find({a.first, type, a.second});
        if (it == bases.end()) throw std::runtime_error("unresolved base " + a.first + "/" + a.second);
        return it->second;
    }

    static nlohmann::ordered_json diff(const nlohmann::ordered_json &base, const nlohmann::ordered_json &values)
    {
        nlohmann::ordered_json out = nlohmann::ordered_json::object();
        for (const auto &[key, value] : values.items())
            if (!base.contains(key) || base[key] != value) out[key] = value;
        for (const auto &[key, value] : base.items())
            if (!values.contains(key)) throw std::runtime_error("a source lacks the base key " + key);
        return out;
    }

    std::vector<nlohmann::ordered_json> products(std::vector<Source> &all, Preset::Type type)
    {
        std::map<std::string, std::vector<const Source *>> groups;
        for (const Source &s : all) groups[group_of(s, type)].push_back(&s);
        for (auto &[id, sources] : groups)
            std::sort(sources.begin(), sources.end(), [](const Source *a, const Source *b) { return a->name < b->name; });
        auto owners = [](const std::vector<const Source *> &sources) {
            std::map<std::string, const Source *> owner;
            for (const Source *s : sources)
                for (const std::string &printer : s->fits) {
                    const Source *&cur = owner[printer];
                    const bool named = std::find(s->named.begin(), s->named.end(), printer) != s->named.end();
                    const bool cur_named = cur && std::find(cur->named.begin(), cur->named.end(), printer) != cur->named.end();
                    if (cur == nullptr || (named && !cur_named)) cur = s;
                }
            return owner;
        };
        for (bool split = true; split;) {
            split = false;
            for (auto &[id, sources] : groups) {
                const auto owner = owners(sources);
                for (const Source *s : sources) {
                    for (const std::string &printer : s->fits) {
                        const Source *o = owner.at(printer);
                        if (o == s || o->values == s->values) continue;
                        const Source *moved = s->name != id ? s : o;
                        sources.erase(std::find(sources.begin(), sources.end(), moved));
                        groups[moved->name] = {moved};
                        splits.push_back(moved->name + " (from " + id + ": differs from " + (moved == s ? o : s)->name + " on " + printer + ")");
                        split = true;
                        break;
                    }
                    if (split) break;
                }
                if (split) break;
            }
        }
        std::vector<nlohmann::ordered_json> out;
        for (auto &[id, sources] : groups) {
            nlohmann::ordered_json from;
            const nlohmann::ordered_json base = base_of(id, type, sources, from);
            const auto owner = owners(sources);
            for (const Source *s : sources) product_of[{type, s->name}] = id;
            nlohmann::ordered_json product = {{"id", id}};
            if (type == Preset::TYPE_FILAMENT) {
                DynamicPrintConfig b;
                apply_record_values(b, base);
                std::string display;
                std::string brand = b.has("filament_vendor") ? b.option<ConfigOptionStrings>("filament_vendor")->get_at(0) : "";
                if (brand.empty() || brand == "(Undefined)") {
                    const Source *first = sources.front();
                    brand = first->library ? id.substr(0, id.find(' ')) : vendor_names.at(first->vendor);
                    unbranded.push_back(id + " -> " + brand);
                }
                product["brand"] = brand;
                product["type"]  = b.has("filament_type") ? b.option<ConfigOptionStrings>("filament_type")->get_at(0) : "";
            }
            std::set<std::string> aliases;
            if (type == Preset::TYPE_FILAMENT)
                for (const auto &[from, into] : folds)
                    if (into == id) aliases.insert(from.second);
            if (!aliases.empty()) product["aliases"] = aliases;
            product["base"] = base;
            product["base_from"] = from;
            nlohmann::ordered_json tunings = nlohmann::ordered_json::array();
            for (const Source *s : sources) {
                nlohmann::ordered_json fits = nlohmann::ordered_json::array();
                for (const std::string &printer : s->fits) {
                    ++pairs;
                    if (owner.at(printer) == s) {
                        fits.push_back(printer);
                        if (type == Preset::TYPE_PRINT) print_fits[id].insert(printer);
                    }
                    else if (owner.at(printer)->values == s->values) { ++shadowed; shadows.push_back(s->name + " on " + printer + " (" + owner.at(printer)->name + ")"); }
                    else throw std::runtime_error(s->name + " on " + printer + " would take other values");
                }
                nlohmann::ordered_json values = diff(base, s->values);
                nlohmann::ordered_json resolved = base;
                if (type == Preset::TYPE_FILAMENT) {
                    DynamicPrintConfig b, t;
                    apply_record_values(b, base);
                    apply_record_values(t, values);
                    for (const auto &[key, value] : variant_projection(b, tuning_variants(b, t))) {
                        if (values.contains(key) && values[key] == value) values.erase(key);
                        if (!values.contains(key)) resolved[key] = value;
                    }
                }
                resolved.update(values);
                if (resolved != s->values) throw std::runtime_error(s->name + " does not resolve to its own values");
                tuning_keys[type] += values.size();
                tunings.push_back(nlohmann::ordered_json{{"fits", fits}, {"values", values}, {"source", s->json}});
            }
            product["tunings"] = tunings;
            out.push_back(std::move(product));
        }
        return out;
    }

    std::vector<std::string> no_default_print;
    std::vector<nlohmann::ordered_json> printers()
    {

        std::map<std::string, std::vector<const Source *>> groups;
        for (const Source &s : machines) {
            DynamicPrintConfig c;
            apply_record_values(c, s.values);
            groups[c.opt_string("printer_model")].push_back(&s);
        }
        std::vector<nlohmann::ordered_json> out;
        for (auto &[id, sources] : groups) {
            std::sort(sources.begin(), sources.end(), [](const Source *a, const Source *b) { return a->name < b->name; });
            nlohmann::ordered_json from;
            const nlohmann::ordered_json base = base_of(id, Preset::TYPE_PRINTER, sources, from);
            const std::string vendor = model_vendor.count(id) ? model_vendor[id] : sources.front()->vendor;
            nlohmann::ordered_json printer = {{"id", id}, {"vendor", vendor}, {"vendor_name", vendor_names[vendor]},
                {"model", models.count(id) ? models[id] : nlohmann::ordered_json::object()}, {"base", base}, {"base_from", from}};
            std::set<std::string> sizes;
            nlohmann::ordered_json variants = nlohmann::ordered_json::array();
            for (const Source *s : sources) {
                DynamicPrintConfig c;
                apply_record_values(c, s->values);
                const std::string nozzle = nozzle_text(c);
                const nlohmann::ordered_json values = diff(base, s->values);
                nlohmann::ordered_json resolved = base;
                resolved.update(values);
                if (resolved != s->values) throw std::runtime_error(s->name + " does not resolve to its own values");
                std::string default_print = c.opt_string("default_print_profile");
                default_print = default_print.substr(0, default_print.find(';'));
                nlohmann::ordered_json variant = {{"id", s->name}, {"nozzle", nozzle}, {"default", sizes.insert(nozzle).second}};
                if (const auto print = product_of.find({Preset::TYPE_PRINT, default_print}); print != product_of.end())
                    variant["defaultPrint"] = print->second;
                else if (const auto fits = print_fits.find(product_id(default_print)); fits != print_fits.end() && fits->second.count(s->name)) {
                    variant["defaultPrint"] = fits->first;
                    repaired_default_print.push_back(s->name + " (names " + default_print + ", takes " + fits->first + ")");
                } else
                    no_default_print.push_back(s->name + (default_print.empty() ? "" : " (names " + default_print + ")"));
                const auto *diameters = c.option<ConfigOptionFloats>("nozzle_diameter");
                variant["extruders"] = diameters->size();
                nlohmann::ordered_json nozzles = nlohmann::ordered_json::array();
                for (const double d : diameters->values) {
                    std::ostringstream text;
                    text << d;
                    nozzles.push_back({{"diameter", text.str()}});
                }
                variant["nozzles"] = nozzles;
                variant["values"] = values;
                variant["source"] = s->json;
                variants.push_back(std::move(variant));
            }
            printer["variants"] = variants;
            out.push_back(std::move(printer));
        }
        return out;
    }
};

nlohmann::ordered_json fold_candidates(const std::vector<nlohmann::ordered_json> &filaments, const std::map<std::string, std::string> &vendor_names)
{
    auto lower = [](std::string s) { boost::algorithm::to_lower(s); return s; };
    auto words = [&](const std::string &s) { std::vector<std::string> w; boost::algorithm::split(w, lower(s), boost::is_any_of(" "), boost::token_compress_on); w.erase(std::remove(w.begin(), w.end(), ""), w.end()); return w; };
    auto first = [&](const std::string &s) { const auto w = words(s); return w.empty() ? std::string() : w.front(); };
    auto vendors = [](const nlohmann::ordered_json &p) { std::set<std::string> v; for (const auto &t : p["tunings"]) v.insert(t["source"].value("vendor", "")); return v; };
    auto own = [&](const std::string &vendor, const std::string &brand) {
        if (vendor == PresetBundle::ORCA_FILAMENT_LIBRARY) return true;
        const auto name = vendor_names.count(vendor) ? vendor_names.at(vendor) : vendor;
        return !brand.empty() && (boost::starts_with(lower(brand), first(name)) || boost::starts_with(lower(name), first(brand)));
    };
    auto density = [](const nlohmann::ordered_json &p) {
        const std::string d = p["base"].value("filament_density", "");
        return d.empty() ? -1. : std::atof(d.substr(0, d.find(',')).c_str());
    };
    nlohmann::ordered_json folds = nlohmann::ordered_json::array(), separate = nlohmann::ordered_json::array();
    for (const auto &p : filaments) {
        const auto vs = vendors(p);
        const std::string brand = p.value("brand", ""), type = p.value("type", ""), id = p["id"];
        if (vs.size() != 1 || brand.empty() || lower(brand) == "generic" || own(*vs.begin(), brand)) continue;
        const std::string vendor = *vs.begin();
        const auto mine = words(id);
        std::vector<const nlohmann::ordered_json *> targets;
        bool brand_has_type = false;
        for (const auto &t : filaments) {
            if (&t == &p || t.value("brand", "") != brand || t.value("type", "") != type) continue;
            const auto tv = vendors(t);
            if (std::none_of(tv.begin(), tv.end(), [&](const std::string &v) { return own(v, brand); })) continue;
            brand_has_type = true;
            auto theirs = words(t["id"]);
            bool holds = std::all_of(mine.begin(), mine.end(), [&](const std::string &w) { return std::find(theirs.begin(), theirs.end(), w) != theirs.end(); });
            bool extra_basic = std::all_of(theirs.begin(), theirs.end(), [&](const std::string &w) { return w == "basic" || std::find(mine.begin(), mine.end(), w) != mine.end(); });
            if (holds && extra_basic) targets.push_back(&t);
        }
        nlohmann::ordered_json entry = {{"from", id}, {"vendor", vendor}, {"brand", brand}, {"type", type}};
        if (targets.size() == 1) {
            const auto &t = *targets.front();
            const double a = density(p), b = density(t);
            entry["into"] = t["id"];
            entry["evidence"] = {{"brand", brand}, {"type", type}, {"words", "every word of " + id + " is in " + std::string(t["id"])},
                                 {"tunings", p["tunings"].size()}, {"density", {a, b}}, {"into_base", t["base_from"].value("name", "")}};
            if (a > 0 && b > 0 && std::abs(a - b) <= 0.03) folds.push_back(entry);
            else { entry["reason"] = "densities differ"; separate.push_back(entry); }
        } else {
            entry["reason"] = targets.empty() ? (brand_has_type ? "no product of the brand with the same name" : "the brand has no product of its own of this type")
                                              : "several products of the brand match";
            nlohmann::ordered_json names = nlohmann::ordered_json::array();
            for (const auto *t : targets) names.push_back((*t)["id"]);
            if (!targets.empty()) entry["matches"] = names;
            separate.push_back(entry);
        }
    }
    return {{"folds", folds}, {"separate", separate}};
}

uint64_t fnv(uint64_t hash, const std::string &text)
{
    for (const unsigned char c : text) hash = (hash ^ c) * 1099511628211ull;
    return hash;
}
}
void flatten_catalog(const Args &args)
{
    flatten::Catalog catalog;
    catalog.profiles = args.profiles;
    const fs::path profiles(args.profiles), out_dir(args.flatten_catalog);
    const fs::path folds_file = profiles.parent_path() / "catalog_folds.json";
    if (args.fold_candidates.empty()) {
        const nlohmann::ordered_json reviewed = catalog.read(folds_file);
        for (const auto &f : reviewed.at("folds"))
            catalog.folds[{f.at("vendor").get<std::string>(), f.at("from").get<std::string>()}] = f.at("into").get<std::string>();
    }
    const auto rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
    PresetBundle library;
    library.load_vendor_configs_from_json(profiles.string(), PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem, rule, nullptr, false);
    if (library.error_count()) throw std::runtime_error("invalid filament library");
    catalog.load_vendor(PresetBundle::ORCA_FILAMENT_LIBRARY, nullptr);
    std::vector<std::string> vendors;
    for (const fs::directory_entry &entry : fs::directory_iterator(profiles))
        if (entry.path().extension() == ".json" && fs::is_directory(profiles / entry.path().stem() / "machine"))
            vendors.push_back(entry.path().stem().string());
    std::sort(vendors.begin(), vendors.end());
    for (const std::string &vendor : vendors)
        catalog.load_vendor(vendor, &library);
    std::map<std::string, std::vector<nlohmann::ordered_json>> files;
    files["prints"]    = catalog.products(catalog.processes, Preset::TYPE_PRINT);
    files["filaments"] = catalog.products(catalog.filaments, Preset::TYPE_FILAMENT);
    if (!args.fold_candidates.empty()) {
        std::ofstream out(args.fold_candidates, std::ios::binary | std::ios::trunc);
        out << flatten::fold_candidates(files["filaments"], catalog.vendor_names).dump(2) << '\n';
        if (!out) throw std::runtime_error("cannot write " + args.fold_candidates);
        std::cout << "fold candidates written to " << args.fold_candidates << "\n";
        return;
    }
    for (const auto &fold : catalog.folds) {
        const auto &from = fold.first;
        const auto &into = fold.second;
        if (!catalog.folded.count(from)) throw std::runtime_error("catalog_folds.json: " + from.first + " has no filament " + from.second);
        if (std::none_of(files["filaments"].begin(), files["filaments"].end(), [&](const auto &p) { return p["id"] == into; }))
            throw std::runtime_error("catalog_folds.json: no product " + into + " to fold " + from.second + " into");
    }
    files["printers"]  = catalog.printers();
    std::map<std::string, std::string> text;
    uint64_t version = 1469598103934665603ull;
    for (const auto &[kind, products] : files) {
        std::string &body = text[kind];
        for (const auto &product : products) body += product.dump() + "\n";
        version = flatten::fnv(version, kind + "\n" + body);
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(version));
    fs::create_directories(out_dir);
    for (const auto &[kind, body] : text) {
        const fs::path file = catalog_file(out_dir.string(), kind), tmp = fs::path(file.string() + ".tmp");
        std::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        out << nlohmann::ordered_json{{"schema", CATALOG_SCHEMA}, {"version", hex}}.dump() << '\n' << body;
        out.close();
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        fs::rename(tmp, file);
    }
    std::cout << "catalog " << hex << ": " << files["printers"].size() << " printers (" << catalog.machines.size() << " variants), "
              << files["prints"].size() << " prints (" << catalog.processes.size() << " presets), " << files["filaments"].size()
              << " filaments (" << catalog.filaments.size() << " presets)\n";
    for (const auto &[rule, n] : catalog.rules) std::cout << "base rule " << rule << ": " << n << "\n";
    for (const std::string &s : catalog.first_bases) std::cout << "  base is its first tuning: " << s << "\n";
    std::cout << "filament products branded by their bundle " << catalog.unbranded.size() << "\n";
    for (const std::string &s : catalog.unbranded) std::cout << "  brand: " << s << "\n";
    std::cout << "accepted (preset, printer) pairs " << catalog.pairs << ": " << catalog.pairs - catalog.shadowed << " resolve to their own preset, "
              << catalog.shadowed << " to another preset of the product with identical values\n";
    std::cout << "tuning values held: prints " << catalog.tuning_keys[Preset::TYPE_PRINT] << ", filaments "
              << catalog.tuning_keys[Preset::TYPE_FILAMENT] << "\n";
    std::cout << "vendor copies folded into the brand's product " << catalog.folds.size() << "\n";
    for (const auto &[from, into] : catalog.folds)
        std::cout << "  fold: " << from.first << " " << from.second << " -> " << into << " (" << catalog.folded[from] << " presets)\n";
    std::cout << "presets split into a product of their own " << catalog.splits.size() << "\n";
    for (const std::string &s : catalog.splits) std::cout << "  split: " << s << "\n";
    std::cout << "printer variants whose default print names no preset, repaired to its product " << catalog.repaired_default_print.size() << "\n";
    for (const std::string &s : catalog.repaired_default_print) std::cout << "  repaired default print: " << s << "\n";
    std::cout << "printer variants without a default print " << catalog.no_default_print.size() << "\n";
    for (const std::string &s : catalog.no_default_print) std::cout << "  no default print: " << s << "\n";
    if (args.verbose_flatten) for (const std::string &s : catalog.shadows) std::cout << "  shadowed " << s << "\n";
}
}
