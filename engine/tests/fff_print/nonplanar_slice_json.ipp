#pragma once
#include "git_commit_hash.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/ClearanceVerifier.hpp"
#include "libslic3r/Nonplanar/ConformalLayers.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Nonplanar/ConformalShells.hpp"
#include "libslic3r/Nonplanar/AutoMode.hpp"
#include "libslic3r/Nonplanar/TopSurfaceGrid.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"
#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/uuid/detail/sha1.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using Json = nlohmann::json;

Json print_statistics_json(const Print &print, const GCodeProcessorResult &result)
{
    const PrintStatistics &stats = print.print_statistics();
    Json statistics = {{"total_used_filament_mm", stats.total_used_filament},
                       {"total_extruded_volume_mm3", stats.total_extruded_volume},
                       {"total_weight_g", stats.total_weight}, {"total_cost", stats.total_cost},
                       {"total_wipe_tower_filament_mm", stats.total_wipe_tower_filament},
                       {"total_toolchanges", stats.total_toolchanges}, {"per_extruder", Json::array()}};
    for (const auto &[id, volume] : result.print_statistics.total_volumes_per_extruder) {
        const double diameter = print.config().filament_diameter.get_at(id);
        const double density = print.config().filament_density.get_at(id);
        statistics["per_extruder"].push_back({{"extruder_id", id},
            {"used_filament_mm", volume / (PI * sqr(0.5 * diameter))},
            {"used_filament_g", volume * density * 0.001}});
    }
    return statistics;
}

std::string file_sha1(const std::string &path)
{
    boost::nowide::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot hash " + path);
    boost::uuids::detail::sha1 hash;
    std::array<char, 65536> buffer;
    while (in) {
        in.read(buffer.data(), buffer.size());
        hash.process_bytes(buffer.data(), size_t(in.gcount()));
    }
    if (!in.eof()) throw std::runtime_error("cannot read " + path);
    boost::uuids::detail::sha1::digest_type digest;
    hash.get_digest(digest);
    std::ostringstream out;
    for (const auto word : digest)
        out << std::hex << std::setfill('0') << std::setw(int(2 * sizeof(word))) << static_cast<unsigned long>(word);
    return out.str();
}

std::string first_line(const fs::path &path)
{
    boost::nowide::ifstream in(path.string());
    std::string line;
    std::getline(in, line);
    return line;
}

Json git_sha(const std::string &ref)
{
    const fs::path root = fs::path(PROFILES_DIR).parent_path().parent_path();
    fs::path git = root / ".git";
    if (fs::is_regular_file(git)) {
        const std::string line = first_line(git);
        if (line.rfind("gitdir: ", 0) != 0) return nullptr;
        const fs::path directory(line.substr(8));
        git = directory.is_absolute() ? directory : root / directory;
    }
    fs::path common = git;
    if (fs::exists(git / "commondir")) {
        const fs::path directory(first_line(git / "commondir"));
        common = directory.is_absolute() ? directory : git / directory;
    }
    const auto packed_ref = [&](const std::string &name) {
        boost::nowide::ifstream packed((common / "packed-refs").string());
        std::string line;
        while (std::getline(packed, line))
            if (line.size() > 41 && line.substr(41) == name) return line.substr(0, 40);
        return std::string();
    };
    std::string value = first_line((ref == "HEAD" ? git : common) / ref);
    if (value.rfind("ref: ", 0) == 0) {
        const std::string target = value.substr(5);
        value = first_line(common / target);
        if (value.empty()) value = packed_ref(target);
    } else if (value.empty() && ref != "HEAD") value = packed_ref(ref);
    return value.size() == 40 ? Json(value) : Json(nullptr);
}

template<class T> std::string text_value(const T &value)
{
    std::ostringstream out;
    out.copyfmt(std::cout);
    out << value;
    return out.str();
}

double text_number(double value)
{
    return std::stod(text_value(value));
}

void slice_json_record(Json &record, const Args &args)
{
    record = {{"schema", "orca.nonplanar_slice.v1"}, {"status", "started"},
              {"inputs", {{"printer", args.printer}, {"process", args.process}, {"filaments", args.filaments},
                          {"catalog", args.catalog}, {"overrides", args.overrides}, {"mode", nullptr}, {"deform", nullptr}, {"head", nullptr}, {"atom_toolpath", nullptr},
                          {"source_sha", std::string(GIT_COMMIT_FULL_HASH).empty() ? Json(nullptr) : Json(GIT_COMMIT_FULL_HASH)},
                          {"checkout_sha", git_sha("HEAD")}, {"trunk_sha", git_sha("refs/heads/nonplanar")}}},
              {"results", {{"patches", Json::array()}}}, {"objects", Json::array()},
              {"totals", {{"whole_top_rms_mm", nullptr}, {"zaa_rms_mm", nullptr}, {"planar_rms_mm", nullptr},
                          {"aborts", Json::array()}, {"exceptions", Json::array()}, {"coverage", nullptr}, {"gap_cells", nullptr},
                          {"ironed_area_mm2", nullptr}, {"estimated_time_s", nullptr}, {"cpu_seconds", nullptr},
                          {"gcode_bytes", nullptr}, {"retractions", nullptr}, {"travel_length_mm", nullptr},
                          {"verifier", {{"ran", false}, {"hit_moves", nullptr}, {"machine_hit_moves", nullptr}, {"tolerated_moves", nullptr},
                                        {"first_hits", Json::array()}, {"worst", Json::array()}, {"min_clearance_mm", nullptr}, {"omissions", Json::array()}}}}},
              {"measurement", {{"pitch_mm", 0.25}, {"inset_mm", 2.}, {"wall_slope_deg", 70.}, {"gap_threshold_mm", 1.}}},
              {"tool", Json::object()}, {"text_summary", Json::object()}};
    auto &meshes = record["inputs"]["meshes"];
    meshes = Json::array();
    if (!args.calib.empty())
        record["inputs"]["calib"] = {{"kind", args.calib}, {"params", args.calib_params}};
    if (args.plate.empty()) {
        if (!args.model.empty())
            meshes.push_back({{"file", args.model}, {"sha1", nullptr}});
    } else {
        record["inputs"]["plate"] = {{"file", args.plate}, {"sha1", file_sha1(args.plate)}};
        boost::nowide::ifstream in(args.plate);
        if (!in) throw std::runtime_error("cannot read plate " + args.plate);
        const Json layout = Json::parse(in);
        const fs::path base = fs::path(args.plate).parent_path();
        for (const Json &entry : layout.at("objects")) {
            std::vector<Json> files;
            if (entry.contains("file")) files.push_back(entry);
            for (const Json &volume : entry.value("volumes", Json::array())) files.push_back(volume);
            for (const Json &item : files) {
                fs::path file = item.at("file").get<std::string>();
                if (file.is_relative()) file = base / file;
                Json mesh = {{"file", file.string()}, {"sha1", nullptr}};
                if (item.contains("extruder")) mesh["extruder"] = item.at("extruder");
                meshes.push_back(std::move(mesh));
            }
        }
    }
    for (const auto &part : args.parts)
        meshes.push_back({{"file", part.first}, {"sha1", nullptr}, {"extruder", part.second}});
    for (auto &mesh : meshes) mesh["sha1"] = file_sha1(mesh["file"].get<std::string>());
}

template<class T> std::string base64_values(const std::vector<T> &values)
{
    std::string out(boost::beast::detail::base64::encoded_size(values.size() * sizeof(T)), '\0');
    out.resize(boost::beast::detail::base64::encode(out.data(), values.data(), values.size() * sizeof(T)));
    return out;
}
std::string base64_floats(const std::vector<float> &values) { return base64_values(values); }

struct SliceGrid : Nonplanar::TopSurfaceGrid
{
    using Nonplanar::TopSurfaceGrid::TopSurfaceGrid;
    Json coarse(size_t max_cells) const
    {
        int f = 1;
        while (size_t((nx + f - 1) / f) * size_t((ny + f - 1) / f) > max_cells) ++f;
        const int cx = (nx + f - 1) / f, cy = (ny + f - 1) / f;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::vector<float> rms(size_t(cx) * cy, nan), mean = rms, top = rms, curved(rms.size(), 0.f), share(rms.size(), 0.f);
        std::vector<uint16_t> owners(owner.empty() ? 0 : rms.size(), 65535);
        for (int Y = 0; Y < cy; ++Y) for (int X = 0; X < cx; ++X) {
            double squared = 0., sum = 0., z = 0.;
            size_t measured = 0, eligible_cells = 0, curved_cells = 0, total = 0;
            std::map<uint16_t, size_t> votes;
            for (int y = Y * f; y < std::min(ny, (Y + 1) * f); ++y) for (int x = X * f; x < std::min(nx, (X + 1) * f); ++x) {
                const size_t k = size_t(y) * nx + x;
                ++total;
                if (!eligible[k]) continue;
                ++eligible_cells;
                z += mesh[k];
                if (!owner.empty()) ++votes[owner[k]];
                const double error = bead[k] - mesh[k];
                if (!std::isfinite(error)) continue;
                ++measured; squared += error * error; sum += error; curved_cells += conformal[k];
            }
            const size_t c = size_t(Y) * cx + X;
            share[c] = float(double(eligible_cells) / total);
            if (eligible_cells) top[c] = float(z / eligible_cells);
            if (measured) {
                rms[c] = float(std::sqrt(squared / measured)); mean[c] = float(sum / measured);
                curved[c] = float(double(curved_cells) / measured);
            }
            if (!votes.empty())
                owners[c] = std::max_element(votes.begin(), votes.end(), [](const auto &a, const auto &b) { return a.second < b.second; })->first;
        }
        Json out = {{"pitch_mm", pitch * f}, {"fine_pitch_mm", pitch}, {"origin", {origin.x(), origin.y()}}, {"nx", cx}, {"ny", cy},
                    {"rms_mm", base64_floats(rms)}, {"mean_mm", base64_floats(mean)}, {"mesh_z_mm", base64_floats(top)},
                    {"curved_fraction", base64_floats(curved)}, {"eligible_fraction", base64_floats(share)}};
        if (!owners.empty()) out["object"] = base64_values(owners);
        return out;
    }
};

class RowContains
{
public:
    explicit RowContains(const ExPolygons &polygons) : m_polygons(polygons) {}
    bool operator()(const Point &p)
    {
        if (!m_valid || p.y() != m_y)
            this->row(p.y());
        for (const std::vector<Ring> &rings : m_rings) {
            if (const int r = crossings(p, rings.front()); r != -1 && r % 2 == 0)
                continue;
            if (std::none_of(rings.begin() + 1, rings.end(), [&p](const Ring &hole) { const int r = crossings(p, hole); return r != -1 && r % 2 == 1; }))
                return true;
        }
        return false;
    }

private:
    struct Ring
    {
        bool                             degenerate = false;
        std::vector<std::pair<Point, Point>> edges;
    };
    static int crossings(const Point &pt, const Ring &ring)
    {
        if (ring.degenerate)
            return 0;
        int result = 0;
        for (const auto &[ip, ipNext] : ring.edges) {
            if (ipNext.y() == pt.y() && ((ipNext.x() == pt.x()) || (ip.y() == pt.y() && ((ipNext.x() > pt.x()) == (ip.x() < pt.x())))))
                return -1;
            if ((ip.y() < pt.y()) != (ipNext.y() < pt.y())) {
                if (ip.x() >= pt.x()) {
                    if (ipNext.x() > pt.x())
                        result = 1 - result;
                    else {
                        int64_t d = int64_t(ip.x() - pt.x()) * int64_t(ipNext.y() - pt.y()) - int64_t(ipNext.x() - pt.x()) * int64_t(ip.y() - pt.y());
                        if (!d) return -1;
                        if ((d > 0) == (ipNext.y() > ip.y())) result = 1 - result;
                    }
                } else if (ipNext.x() > pt.x()) {
                    int64_t d = int64_t(ip.x() - pt.x()) * int64_t(ipNext.y() - pt.y()) - int64_t(ipNext.x() - pt.x()) * int64_t(ip.y() - pt.y());
                    if (!d) return -1;
                    if ((d > 0) == (ipNext.y() > ip.y())) result = 1 - result;
                }
            }
        }
        return result;
    }
    void row(coord_t y)
    {
        m_y     = y;
        m_valid = true;
        m_rings.resize(m_polygons.size());
        for (size_t i = 0; i < m_polygons.size(); ++i) {
            const ExPolygon &expoly = m_polygons[i];
            m_rings[i].resize(1 + expoly.holes.size());
            for (size_t j = 0; j < m_rings[i].size(); ++j) {
                const Points &path = (j == 0 ? expoly.contour : expoly.holes[j - 1]).points;
                Ring         &ring = m_rings[i][j];
                ring.degenerate    = path.size() < 3;
                ring.edges.clear();
                for (size_t k = 0; k < path.size() && !ring.degenerate; ++k) {
                    const Point &ip = path[k], &ipNext = path[(k + 1) % path.size()];
                    if (ipNext.y() == y || ((ip.y() < y) != (ipNext.y() < y)))
                        ring.edges.emplace_back(ip, ipNext);
                }
            }
        }
    }
    const ExPolygons                    &m_polygons;
    coord_t                              m_y     = 0;
    bool                                 m_valid = false;
    std::vector<std::vector<Ring>>       m_rings;
};

void slice_json_results(Json &record, const Args &args, const Print &print, const GCodeProcessorResult &result,
                        const Model &model, const DynamicPrintConfig &config)
{
    Json &tool = record["tool"], &totals = record["totals"];
    size_t line = 0;
    const bool rotary = print.config().nonplanar_kinematics.value != RotaryLayout::None;
    const auto vz = max_vz(result, &line, rotary);
    const auto &normal = result.print_statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)];
    tool["estimated_time_s"] = text_number(normal.time);
    tool["max_vz_commanded"] = text_number(vz.first);
    tool["max_vz_planned"] = text_number(vz.second);
    tool["machine_max_speed_z"] = text_number(config.opt<ConfigOptionFloats>("machine_max_speed_z")->get_at(0));
    const auto &clearance = result.clearance_report;
    tool["clearance_checked"] = int(clearance.ran);
    if (clearance.ran) {
        tool["clearance_hit_moves"] = clearance.hit_moves;
        tool["clearance_min_mm"] = std::isfinite(clearance.min_clearance) ? Json(text_number(clearance.min_clearance)) : Json("inf");
    }
    totals["estimated_time_s"] = normal.time;
    totals["gcode_bytes"] = fs::file_size(args.output);
    totals["retractions"] = 0;
    double travel = 0.;
    Json hits = Json::array();
    size_t first_print = result.moves.size(), last_print = 0;
    for (size_t i = 0; i < result.moves.size(); ++i)
        if (result.moves[i].type == EMoveType::Extrude && result.moves[i].extrusion_role != erCustom && result.moves[i].extrusion_role != erNone) {
            first_print = std::min(first_print, i); last_print = i;
        }
    for (size_t i = 0; i < result.moves.size(); ++i) {
        const auto &m = result.moves[i];
        if (m.type == EMoveType::Retract) totals["retractions"] = totals["retractions"].get<size_t>() + 1;
        if (m.type == EMoveType::Travel && i > 0) travel += (emitted_tip(result, i, rotary) - emitted_tip(result, i - 1, rotary)).norm();
        if (hits.size() < 10 && i < clearance.per_move.size() && clearance.per_move[i] < -1e-5 &&
            !(m.extrusion_role == erCustom && (i < first_print || i > last_print))) {
            const Vec3d tip = emitted_tip(result, i, rotary);
            Json hit = {{"move_id", i}, {"gcode_line", m.gcode_id}, {"tip_mm", {tip.x(), tip.y(), tip.z()}},
                        {"penetration_mm", -clearance.per_move[i]}, {"component", nullptr}};
            for (const auto &worst : clearance.worst) if (worst.move_id == i) {
                hit["component"] = worst.component;
                hit["tip_mm"] = {worst.tip.x(), worst.tip.y(), worst.tip.z()};
            }
            hits.push_back(std::move(hit));
        }
    }
    totals["travel_length_mm"] = travel;
    Json worst = Json::array();
    for (const ClearanceHit &hit : clearance.worst)
        worst.push_back({{"move_id", hit.move_id}, {"gcode_line", hit.gcode_line}, {"layer", hit.layer},
                         {"component", hit.component},
                         {"class", hit.cls == ClearanceHitClass::Cone ? "cone" : hit.cls == ClearanceHitClass::Land ? "land" : "body"},
                         {"penetration_mm", hit.penetration}, {"tip_mm", {hit.tip.x(), hit.tip.y(), hit.tip.z()}}});
    totals["verifier"] = {{"ran", clearance.ran}, {"hit_moves", clearance.hit_moves},
                          {"machine_hit_moves", clearance.machine_hit_moves}, {"tolerated_moves", clearance.tolerated_moves},
                          {"first_hits", hits}, {"worst", std::move(worst)},
                          {"min_clearance_mm", clearance.min_clearance}, {"max_contact_mm", clearance.max_contact},
                          {"duct_graze_moves", clearance.duct_graze_moves}, {"omissions", clearance.omissions}};
    tool["fallback"] = Json::array();
    std::vector<uint16_t> face_owner;
    if (args.surface_grid)
        for (const ModelObject *object : model.objects)
            for (const ModelInstance *instance : object->instances) {
                uint16_t index = 65535;
                for (size_t i = 0; i < print.objects().size(); ++i)
                    for (const PrintInstance &printed : print.objects()[i]->instances())
                        if (printed.model_instance->id() == instance->id()) index = uint16_t(i);
                face_owner.insert(face_owner.end(), object->raw_indexed_triangle_set().indices.size(), index);
            }
    SliceGrid grid(plate_mesh(model), face_owner);
    grid.deposit(result);
    if (args.surface_grid)
        record["surface_grid"] = grid.coarse(args.surface_grid_max_cells);
    const Json quality = grid.measure([](const Vec2d &) { return true; });
    totals["whole_top_rms_mm"] = quality["rms_vs_mesh_mm"];
    totals["coverage"] = quality["coverage"];
    totals["gap_cells"] = quality["gap_cells"];
    totals["ironed_area_mm2"] = quality["ironed_area_mm2"];
    totals["eval_cells"] = quality["eval_cells"];
    totals["finite_cells"] = quality["finite_cells"];
    totals["rms_covered_mm"] = quality["rms_covered_mm"];
    totals["rms_uncovered_mm"] = quality["rms_uncovered_mm"];
    const bool control = std::all_of(print.objects().begin(), print.objects().end(), [](const PrintObject *object) {
        return object->config().nonplanar_mode.value == NonplanarMode::Off;
    });
    if (control)
        totals[config.opt_bool("zaa_enabled") ? "zaa_rms_mm" : "planar_rms_mm"] = totals["whole_top_rms_mm"];
    size_t object_id = 0;
    for (const PrintObject *object : print.objects()) {
        Json entry = {{"id", object_id}, {"name", object->model_object()->name}, {"mode", Nonplanar::conformal_requested(*object) ? std::string("conformal_layers") : object->config().nonplanar_mode.serialize()}, {"deform", object->config().nonplanar_deform.serialize()}, {"instances", Json::array()}, {"patches", Json::array()}};
        if (const auto &c = object->conformal(); !c.layer_source.empty())
            entry["curved_layers"] = {{"source", c.layer_source == "on" ? "column" : c.layer_source}};
        else if (!c.layer_source_refused.empty())
            entry["curved_layers"] = {{"refused", c.layer_source_refused}};
        size_t top = 0, bands = 0;
        double footprint = 0.;
        for (const auto &instance : object->instances())
            entry["instances"].push_back({{"id", instance.id}, {"shift_mm", {unscale<double>(instance.shift.x()), unscale<double>(instance.shift.y())}}});
        const auto in_footprint = [&](const Vec2d &p, std::vector<RowContains> &rows, const BoundingBox &box) {
            for (size_t i = 0; i < object->instances().size(); ++i) {
                const auto &instance = object->instances()[i];
                const Vec2d local = p - Vec2d(unscale<double>(instance.shift.x()), unscale<double>(instance.shift.y()));
                const Point node = Point::new_scale(local);
                if (box.contains(node) && rows[i](node))
                    return true;
            }
            return false;
        };
        for (const auto &patch : object->conformal().patches) {
            const double a = area(patch.footprint) * SCALING_FACTOR * SCALING_FACTOR;
            const BoundingBox box = get_extents(patch.footprint);
            std::vector<RowContains> rows(object->instances().size(), RowContains(patch.footprint));
            Json p = grid.measure([&](const Vec2d &xy) { return in_footprint(xy, rows, box); });
            static const char *kinds[] = {"top", "bottom", "volume", "overhang", "chunk", "support-interface", "bridge", "field"};
            static_assert(std::size(kinds) == size_t(Nonplanar::PatchKind::Field) + 1, "one name per PatchKind");
            p.update({{"id", patch.id}, {"object_id", object_id}, {"kind", kinds[size_t(patch.kind)]},
                      {"area_mm2", a}, {"mode_chosen", patch.kind == Nonplanar::PatchKind::Chunk ? "multiplanar" :
                                                       patch.kind == Nonplanar::PatchKind::Field ? "field" : "conformal"},
                      {"band_count", patch.band < 0 ? 0 : 1}, {"band", patch.band}, {"shells", patch.shells}, {"selection", patch.automatic ? "auto" : "configured"},
                      {"reason_tags", Json::array()}, {"surface_rms_mm", nullptr}});
            for (const auto &diag : object->conformal().diagnostics) if (diag.patch_id == patch.id) {
                p["surface_rms_mm"] = diag.rms_surface_error;
                if (!diag.fallback_reason.empty()) p["reason_tags"].push_back(diag.fallback_reason);
            }
            if (args.explain && patch.kind == Nonplanar::PatchKind::Top)
                for (const Nonplanar::ReportPatch &rp : result.nonplanar_report.patches)
                    if (rp.object == object_id && rp.id == patch.id)
                        p["explain"] = rp.explain;
            entry["patches"].push_back(p);
            record["results"]["patches"].push_back(std::move(p));
            if (patch.kind == Nonplanar::PatchKind::Top) { ++top; bands = std::max(bands, size_t(patch.band + 1)); footprint += a; }
        }
        bool fallback_zaa = config.opt_bool("zaa_enabled");
        for (const PrintRegion &region : object->all_regions())
            fallback_zaa = fallback_zaa || Nonplanar::zaa_fallback(region.config(), object->config());
        for (const auto &diag : object->conformal().diagnostics) if (diag.fallback_area > 0.) {
            const std::string reason = diag.fallback_reason.empty() ? "untagged" : diag.fallback_reason;
            tool["fallback"].push_back({{"reason", reason}, {"area_mm2", text_number(diag.fallback_area)}, {"patch", diag.patch_id}});
            Json fallback = {{"id", diag.patch_id}, {"object_id", object_id}, {"area_mm2", diag.fallback_area},
                             {"mode_chosen", fallback_zaa ? "zaa" : "planar"},
                             {"kind", "fallback"}, {"reason_tags", {reason}}, {"coverage", 0.}, {"rms_vs_mesh_mm", nullptr},
                             {"gap_cells", nullptr}, {"ironed_area_mm2", 0.}, {"band_count", 0}};
            entry["patches"].push_back(fallback);
            record["results"]["patches"].push_back(std::move(fallback));
        }
        tool["nonplanar_top_patches"] = top;
        tool["nonplanar_bands"] = bands;
        tool["nonplanar_top_footprint_mm2"] = text_number(footprint);
        if (print.objects().size() == 1) {
            entry["totals"] = quality;
        } else {
            const indexed_triangle_set its = object->model_object()->raw_indexed_triangle_set();
            indexed_triangle_set object_mesh;
            for (const auto &instance : object->instances()) {
                indexed_triangle_set copy = its;
                its_transform(copy, instance.model_instance->get_matrix(), true);
                its_merge(object_mesh, copy);
            }
            SliceGrid object_grid(object_mesh);
            object_grid.deposit(result);
            entry["totals"] = object_grid.measure([](const Vec2d &) { return true; });
        }
        record["objects"].push_back(std::move(entry));
        ++object_id;
    }
}

void write_slice_json(const std::string &path, Json &record)
{
    record["totals"]["cpu_seconds"] = process_cpu_seconds();
    const fs::path destination(path);
    const fs::path temporary = destination.parent_path() / fs::unique_path(destination.filename().string() + ".%%%%%%.tmp");
    try {
        boost::nowide::ofstream out(temporary.string(), std::ios::binary | std::ios::trunc);
        out << record.dump(2) << '\n';
        out.close();
        if (!out) throw std::runtime_error("cannot write slice JSON " + path);
        fs::rename(temporary, destination);
    } catch (...) {
        boost::system::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}
}
