#include "StageCache.hpp"
#include "Sha256.hpp"

#include "Repair.hpp"
#include "libslic3r/GCode/ClearanceRepair.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
extern char **environ;
#endif

namespace Slic3r::Nonplanar {

namespace fs = boost::filesystem;

namespace {

std::mutex  store_mutex;
std::string store_dir;
uint64_t    store_max_bytes = uint64_t(1) << 30;

constexpr const char *Magic = "np-stage-cache 1";

fs::path entry_path(const std::string &dir, const std::string &key, const char *kind) { return fs::path(dir) / (key + "." + kind); }

void evict(const std::string &dir, uint64_t max_bytes)
{
    boost::system::error_code ec;
    std::vector<std::tuple<std::time_t, uint64_t, fs::path>> entries;
    uint64_t total = 0;
    const std::time_t now = std::time(nullptr);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!fs::is_regular_file(it->status()))
            continue;
        const fs::path    path  = it->path();
        const std::time_t mtime = fs::last_write_time(path, ec);
        if (ec) { ec.clear(); continue; }
        if (path.filename().string().find(".tmp.") != std::string::npos) {
            if (now - mtime > 3600)
                fs::remove(path, ec);
            ec.clear();
            continue;
        }
        const uint64_t size = fs::file_size(path, ec);
        if (ec) { ec.clear(); continue; }
        entries.emplace_back(mtime, size, path);
        total += size;
    }
    std::sort(entries.begin(), entries.end());
    for (const auto &[mtime, size, path] : entries) {
        if (total <= max_bytes)
            break;
        fs::remove(path, ec);
        ec.clear();
        total -= size;
    }
}


struct SettledRepairs
{
    RepairHints              hints;
    std::set<int>            planar_surfaces;
    bool                     fallback_off = false;
    std::vector<std::string> access_warnings;
    std::vector<CodedWarning> guard_warnings;
};

class Writer
{
public:
    template<class T> void put(const T &v) { out.append(reinterpret_cast<const char *>(&v), sizeof(v)); }
    void put_count(size_t n) { put(uint64_t(n)); }
    void put_string(const std::string &v) { put_count(v.size()); out += v; }
    void put_disc(const RepairHints::Disc &d) { put(d.p); put(d.radius); put(d.z); }
    std::string out;
};

class Reader
{
public:
    explicit Reader(const std::string &in) : in(in) {}
    template<class T> void get(T &v)
    {
        if (!ok || in.size() - at < sizeof(v)) { ok = false; return; }
        std::memcpy(&v, in.data() + at, sizeof(v));
        at += sizeof(v);
    }
    size_t count(size_t each)
    {
        uint64_t n = 0;
        get(n);
        if (ok && n > (in.size() - at) / std::max<size_t>(each, 1)) ok = false;
        return ok ? size_t(n) : 0;
    }
    void get_string(std::string &v)
    {
        const size_t n = count(1);
        if (ok) { v.assign(in, at, n); at += n; }
    }
    void get_disc(RepairHints::Disc &d) { get(d.p); get(d.radius); get(d.z); }
    bool done() const { return ok && at == in.size(); }
    const std::string &in;
    size_t at = 0;
    bool   ok = true;
};

constexpr size_t DiscBytes = sizeof(Vec2d) + 2 * sizeof(double);

std::string settled_bytes(const SettledRepairs &r)
{
    Writer w;
    const RepairHints &h = r.hints;
    w.put_count(h.surface_caps.size());
    for (const RepairHints::SurfaceCap &c : h.surface_caps) { w.put(c.p); w.put(c.radius); w.put(c.z); }
    for (const std::vector<RepairHints::Disc> *discs : { &h.ironing_drops, &h.uniform_beads, &h.upright, &h.planar, &h.ordered_roads }) {
        w.put_count(discs->size());
        for (const RepairHints::Disc &d : *discs) w.put_disc(d);
    }
    w.put_count(h.order_bands.size());
    for (const RepairHints::OrderBand &b : h.order_bands) { w.put(b.p); w.put(b.height); }
    w.put_count(h.ordered_road_passes.size());
    for (const auto &[id, passes] : h.ordered_road_passes) { w.put(id); w.put(passes); }
    w.put_count(h.ordered_surfaces.size());
    for (int id : h.ordered_surfaces) w.put(id);
    w.put(h.travel_lift);
    w.put_count(h.travel_layers.size());
    for (size_t l : h.travel_layers) w.put(uint64_t(l));
    w.put(uint8_t(h.curved_finish));
    w.put_count(r.planar_surfaces.size());
    for (int id : r.planar_surfaces) w.put(id);
    w.put(uint8_t(r.fallback_off));
    w.put_count(r.access_warnings.size());
    for (const std::string &warning : r.access_warnings) w.put_string(warning);
    w.put_count(r.guard_warnings.size());
    for (const CodedWarning &warning : r.guard_warnings) {
        w.put_string(warning.code.code);
        w.put_string(warning.code.object);
        w.put_string(warning.code.params);
        w.put_string(warning.message);
    }
    return std::move(w.out);
}

bool settled_from_bytes(const std::string &in, SettledRepairs &r)
{
    Reader rd(in);
    RepairHints &h = r.hints;
    h.surface_caps.resize(rd.count(sizeof(Vec2d) + 2 * sizeof(double)));
    for (RepairHints::SurfaceCap &c : h.surface_caps) { rd.get(c.p); rd.get(c.radius); rd.get(c.z); }
    for (std::vector<RepairHints::Disc> *discs : { &h.ironing_drops, &h.uniform_beads, &h.upright, &h.planar, &h.ordered_roads }) {
        discs->resize(rd.count(DiscBytes));
        for (RepairHints::Disc &d : *discs) rd.get_disc(d);
    }
    h.order_bands.resize(rd.count(sizeof(Vec2d) + sizeof(double)));
    for (RepairHints::OrderBand &b : h.order_bands) { rd.get(b.p); rd.get(b.height); }
    for (size_t i = 0, n = rd.count(2 * sizeof(int)); i < n; ++i) {
        int id = 0, passes = 0;
        rd.get(id); rd.get(passes);
        h.ordered_road_passes[id] = passes;
    }
    for (size_t i = 0, n = rd.count(sizeof(int)); i < n; ++i) { int id = 0; rd.get(id); h.ordered_surfaces.insert(id); }
    rd.get(h.travel_lift);
    h.travel_layers.resize(rd.count(sizeof(uint64_t)));
    for (size_t &l : h.travel_layers) { uint64_t v = 0; rd.get(v); l = size_t(v); }
    uint8_t finish = 0, off = 0;
    rd.get(finish);
    h.curved_finish = RepairHints::CurvedFinish(std::min<uint8_t>(finish, uint8_t(RepairHints::CurvedFinish::NativeMesh)));
    for (size_t i = 0, n = rd.count(sizeof(int)); i < n; ++i) { int id = 0; rd.get(id); r.planar_surfaces.insert(id); }
    rd.get(off);
    r.fallback_off = off != 0;
    r.access_warnings.resize(rd.count(sizeof(uint64_t)));
    for (std::string &warning : r.access_warnings) rd.get_string(warning);
    r.guard_warnings.resize(rd.count(sizeof(uint64_t)));
    for (CodedWarning &warning : r.guard_warnings) {
        rd.get_string(warning.code.code);
        rd.get_string(warning.code.object);
        rd.get_string(warning.code.params);
        rd.get_string(warning.message);
    }
    return rd.done();
}

std::mutex                                         settled_mutex;
struct ExportNote { bool converged = false; std::map<size_t, std::vector<std::string>> access_warnings; std::vector<CodedWarning> guard_warnings; };
std::map<const Print *, ExportNote>                export_notes;
std::map<size_t, std::vector<std::string>>         primed_access_warnings;
std::map<size_t, std::vector<CodedWarning>>        primed_guard_warnings;

}

void set_stage_cache(const std::string &dir, uint64_t max_bytes)
{
    const std::lock_guard<std::mutex> lock(store_mutex);
    store_dir       = dir;
    store_max_bytes = max_bytes;
    if (!dir.empty()) {
        boost::system::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(warning) << "Stage cache: cannot create " << dir << " (" << ec.message() << "); not caching";
            store_dir.clear();
        }
    }
}

std::string stage_cache_key(const PrintObject &object)
{
    {
        const std::lock_guard<std::mutex> lock(store_mutex);
        if (store_dir.empty())
            return {};
    }
    std::string k;
    const auto bytes = [&k](const void *p, size_t n) { k.append(static_cast<const char *>(p), n); };
    const auto line  = [&k](const std::string &s) { k += s; k += '\n'; };
    const auto config = [&line](const char *what, const ConfigBase &c) {
        line(what);
        for (const std::string &key : c.keys())
            if (Print::gcode_only_options().count(key) == 0 && PrintObject::gcode_only_options().count(key) == 0)
                line(key + "=" + c.opt_serialize(key));
    };
    const Print &print = *object.print();
    config("print", print.config());
    config("object", object.config());
    for (size_t i = 0; i < object.num_printing_regions(); ++i)
        config("region", object.printing_region(i).config());
    const ModelObject &model = *object.model_object();
    for (const ModelVolume *volume : model.volumes) {
        line("volume " + std::to_string(int(volume->type())));
        const indexed_triangle_set &its = volume->mesh().its;
        bytes(its.vertices.data(), its.vertices.size() * sizeof(its.vertices.front()));
        bytes(its.indices.data(), its.indices.size() * sizeof(its.indices.front()));
        bytes(volume->get_matrix().data(), 16 * sizeof(double));
        config("volume config", volume->config.get());
        for (const FacetsAnnotation *paint : { &volume->supported_facets, &volume->seam_facets, &volume->mmu_segmentation_facets, &volume->fuzzy_skin_facets }) {
            const TriangleSelector::TriangleSplittingData &data = paint->get_data();
            line("paint " + std::to_string(data.triangles_to_split.size()));
            for (const TriangleSelector::TriangleBitStreamMapping &t : data.triangles_to_split) {
                bytes(&t.triangle_idx, sizeof(t.triangle_idx));
                bytes(&t.bitstream_start_idx, sizeof(t.bitstream_start_idx));
            }
            for (const std::vector<bool> *bits : { &data.bitstream, &data.used_states })
                line(std::string(bits->begin(), bits->end()));
        }
    }
    for (const auto &[range, range_config] : model.layer_config_ranges) {
        bytes(&range.first, sizeof(range.first));
        bytes(&range.second, sizeof(range.second));
        config("range", range_config.get());
    }
    const std::vector<coordf_t> profile = model.layer_height_profile.get();
    line("profile " + std::to_string(profile.size()));
    bytes(profile.data(), profile.size() * sizeof(coordf_t));
    line("placement");
    bytes(object.trafo().matrix().data(), 16 * sizeof(double));
    for (const PrintInstance &instance : object.instances()) {
        bytes(instance.shift.data(), 2 * sizeof(coord_t));
        bytes(instance.model_instance->get_matrix().data(), 16 * sizeof(double));
    }
#ifdef _WIN32
    char **env = _environ;
#else
    char **env = environ;
#endif
    std::vector<std::string> switches;
    for (; env != nullptr && *env != nullptr; ++env) {
        const std::string var(*env);
        if ((var.rfind("NP_", 0) == 0 || var.rfind("NONPLANAR_", 0) == 0) && var.rfind("NP_LOG_LEVEL=", 0) != 0)
            switches.push_back(var);
    }
    std::sort(switches.begin(), switches.end());
    for (const std::string &var : switches)
        line(var);
    return sha256_hex(std::move(k));
}

bool stage_cache_read(const std::string &key, const char *kind, std::string &data)
{
    std::string dir;
    {
        const std::lock_guard<std::mutex> lock(store_mutex);
        dir = store_dir;
    }
    if (dir.empty() || key.empty())
        return false;
    const fs::path path = entry_path(dir, key, kind);
    boost::system::error_code ec;
    if (!fs::exists(path, ec))
        return false;
    std::string content;
    {
        boost::nowide::ifstream in(path.string(), std::ios::binary);
        content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const size_t eol = content.find('\n');
    std::string payload = eol == std::string::npos ? std::string() : content.substr(eol + 1);
    std::ostringstream header;
    header << Magic << " " << key << " " << kind << " " << payload.size() << " " << sha256_hex(payload);
    if (eol == std::string::npos || content.compare(0, eol, header.str()) != 0) {
        BOOST_LOG_TRIVIAL(warning) << "Stage cache: " << path.string() << " is unreadable or corrupt; deleted";
        fs::remove(path, ec);
        return false;
    }
    fs::last_write_time(path, std::time(nullptr), ec);
    data = std::move(payload);
    return true;
}

void stage_cache_write(const std::string &key, const char *kind, const std::string &data)
{
    std::string dir;
    uint64_t    max_bytes;
    {
        const std::lock_guard<std::mutex> lock(store_mutex);
        dir       = store_dir;
        max_bytes = store_max_bytes;
    }
    if (dir.empty() || key.empty())
        return;
    const fs::path path = entry_path(dir, key, kind);
    const fs::path tmp  = fs::path(dir) / (path.filename().string() + ".tmp." + std::to_string(std::random_device{}()));
    boost::system::error_code ec;
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        out << Magic << " " << key << " " << kind << " " << data.size() << " " << sha256_hex(data) << "\n" << data;
        out.close();
        if (!out) {
            BOOST_LOG_TRIVIAL(warning) << "Stage cache: cannot write " << tmp.string();
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(warning) << "Stage cache: cannot store " << path.string() << " (" << ec.message() << ")";
        fs::remove(tmp, ec);
        return;
    }
    evict(dir, max_bytes);
}

void stage_cache_prime(Print &print)
{
    for (PrintObject *object : print.m_objects) {
        if (object->is_step_done(posSlice) || !object->m_stage_cache_key.empty())
            continue;
        object->m_stage_cache_key = stage_cache_key(*object);
        std::string bytes;
        if (curved_tops_mode(object->config().nonplanar_mode.value) && !object->m_nonplanar_repair &&
            stage_cache_read(object->m_stage_cache_key, "hints", bytes)) {
            SettledRepairs settled;
            if (!settled_from_bytes(bytes, settled)) {
                BOOST_LOG_TRIVIAL(warning) << "Stage cache: unreadable repair hints for " << object->model_object()->name << "; repairing";
                continue;
            }
            object->m_nonplanar_repair      = std::make_shared<RepairHints>(std::move(settled.hints));
            object->m_curved_planar_surfaces = std::move(settled.planar_surfaces);
            if (settled.fallback_off) {
                object->m_curved_fallback_off = true;
                object->m_curved_fallback_zaa = false;
            }
            std::lock_guard<std::mutex> lock(settled_mutex);
            primed_access_warnings[object->id().id] = std::move(settled.access_warnings);
            primed_guard_warnings[object->id().id]  = std::move(settled.guard_warnings);
            BOOST_LOG_TRIVIAL(info) << object->model_object()->name << ": the stage cache holds the repairs its clearance passes settled on";
        }
    }
}

void stage_cache_converged(const Print &print, bool converged, const std::map<size_t, std::vector<std::string>> &access_warnings)
{
    std::lock_guard<std::mutex> lock(settled_mutex);
    export_notes[&print].converged       = converged;
    export_notes[&print].access_warnings = access_warnings;
}

void stage_cache_note_guard_warnings(const Print &print, const std::vector<CodedWarning> &warnings)
{
    std::lock_guard<std::mutex> lock(settled_mutex);
    export_notes[&print].guard_warnings = warnings;
}

std::vector<CodedWarning> stage_cache_guard_warnings(const Print &print)
{
    std::lock_guard<std::mutex> lock(settled_mutex);
    std::vector<CodedWarning> out;
    for (const PrintObject *object : print.objects())
        if (const auto it = primed_guard_warnings.find(object->id().id); it != primed_guard_warnings.end())
            out.insert(out.end(), it->second.begin(), it->second.end());
    return out;
}

std::vector<std::string> stage_cache_access_warnings(size_t object_id)
{
    std::lock_guard<std::mutex> lock(settled_mutex);
    const auto it = primed_access_warnings.find(object_id);
    return it == primed_access_warnings.end() ? std::vector<std::string>() : it->second;
}

void stage_cache_record(const Print &print)
{
    {
        std::lock_guard<std::mutex> lock(settled_mutex);
        const ExportNote note = export_notes[&print];
        export_notes.erase(&print);
        for (const PrintObject *object : print.objects()) {
            if (!note.converged || object->m_stage_cache_key.empty() || !curved_tops_mode(object->config().nonplanar_mode.value))
                continue;
            SettledRepairs settled;
            if (object->m_nonplanar_repair)
                settled.hints = *object->m_nonplanar_repair;
            settled.hints.curved_scaffold.reset();
            settled.hints.curved_top_areas.clear();
            settled.planar_surfaces = object->m_curved_planar_surfaces;
            settled.fallback_off    = object->m_curved_fallback_off;
            if (const auto it = note.access_warnings.find(object->id().id); it != note.access_warnings.end())
                settled.access_warnings = it->second;
            const std::string name = warning_object(object->model_object()->name);
            for (const CodedWarning &warning : note.guard_warnings)
                if (warning.code.object == name)
                    settled.guard_warnings.push_back(warning);
            const RepairHints &h = settled.hints;
            const bool repaired = !h.surface_caps.empty() || !h.ironing_drops.empty() || !h.order_bands.empty() || !h.uniform_beads.empty() ||
                                  !h.upright.empty() || !h.planar.empty() || !h.ordered_roads.empty() || !h.ordered_surfaces.empty() ||
                                  h.travel_lift > 0. || h.curved_finish != RepairHints::CurvedFinish::Blended ||
                                  !settled.planar_surfaces.empty() || settled.fallback_off;
            std::string cached;
            if (repaired && !stage_cache_read(object->m_stage_cache_key, "hints", cached))
                stage_cache_write(object->m_stage_cache_key, "hints", settled_bytes(settled));
        }
    }
}

}
