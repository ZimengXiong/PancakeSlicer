#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

#include "admesh/stl.h"
#include <Eigen/Geometry>
#include <boost/filesystem/directory.hpp>
#include "libslic3r/Nonplanar/Report.hpp"
#include "libslic3r/Nonplanar/Rests.hpp"
#include "libslic3r/Nonplanar/StageCache.hpp"
#include "libslic3r/Nonplanar/ShellBase.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintBase.hpp"
#include <math.h>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/BrimEarsPoint.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/enum_bitmask.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Emboss.hpp"
#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/CutSurface.hpp"
#include "libslic3r/EmbossShape.hpp"
#include "libslic3r/TextConfiguration.hpp"
#include "libslic3r/FlushVolCalc.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/CSGMesh/CSGMesh.hpp"
#include "libslic3r/CSGMesh/ModelToCSGMesh.hpp"
#include "libslic3r/CSGMesh/PerformCSGMeshBooleans.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/ClearanceVerifier.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/ToolKinematics.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTowerEstimate.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Nonplanar/AutoOrient.hpp"
#include "libslic3r/Orient.hpp"
#include "libslic3r/Nonplanar/Calibration.hpp"
#include "libslic3r/Nonplanar/ConformalShells.hpp"
#include "libslic3r/Nonplanar/AutoMode.hpp"
#include "libslic3r/Nonplanar/FlowMod.hpp"
#include "libslic3r/Nonplanar/HeadEnvelope.hpp"
#include "libslic3r/Nonplanar/RegionDetect.hpp"
#include "libslic3r/Nonplanar/CurvedTopsSurface.hpp"
#include "libslic3r/Nonplanar/Validate.hpp"
#include "libslic3r/GCode/ClearanceRepair.hpp"
#include "libslic3r/Nonplanar/Multiplanar.hpp"
#include "libslic3r/Nonplanar/NpField.hpp"
#include "libslic3r/Nonplanar/OrientationField.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetCacheFormat.hpp"
#include <cereal/archives/binary.hpp>
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/calib.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/miniz_extension.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <ios>
#include <limits>
#include <cctype>
#include <map>
#include <memory>
#include <iterator>
#include <mutex>
#include <optional>
#include <tuple>
#include <ostream>
#include <stdexcept>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace Slic3r;
namespace fs = boost::filesystem;

void RGB2HSV(float r, float g, float b, float *h, float *s, float *v)
{
    const float cmax = std::max({ r, g, b }), cmin = std::min({ r, g, b }), delta = cmax - cmin;
    if (std::abs(delta) < 0.001f) *h = 0.f;
    else if (cmax == r) *h = 60.f * std::fmod((g - b) / delta, 6.f);
    else if (cmax == g) *h = 60.f * ((b - r) / delta + 2);
    else *h = 60.f * ((r - g) / delta + 4);
    *s = std::abs(cmax) < 0.001f ? 0.f : delta / cmax;
    *v = cmax;
}

namespace {

struct Args
{
    std::string profiles = std::string(PROFILES_DIR);
    std::string vendor   = "Prusa";
    std::string detect_regions;
    std::string plan_tops;
    std::string printer, process, datadir, output, model, config_out, paint, gcode_3mf, json, plate, project_out, head_out, layer_profile, project, save_3mf,
                tip_out, planned_out, verify_gcode, field_file, dump_field, move_times, calib, calib_head, calib_picks, calib_params, moves_out, dump_options, dump_presets,
                preset_cache, stage_cache;
    uint64_t    stage_cache_max_mb = 1024;
    bool        progress = false;
    bool        moves_extended = false;
    bool        allow_mixed_temperatures = false;
    int         calib_fit = -1;
    int         plate_index = 0;
    std::vector<std::string> filaments, colours;
    std::vector<std::pair<std::string, int>> parts;
    std::vector<std::pair<std::string, std::string>> overrides;
    std::vector<CustomGCode::Item> events;
    bool        center_set = false;
    bool        verify     = false;
    bool        explain    = false;
    bool        auto_orient = false;
    bool        orient_only = false;
    bool        orient_scores = false;
    bool        floating   = false;
    std::string edit;
    std::string cut, cut_keep = "both";
    bool        cut_place_lower = false;
    bool        force_planar    = false;
    std::string arrange_only;
    std::string explode_3mf, explode_directory;
    std::string cut_connectors, cut_mode = "planar", cut_dovetail;
    std::string mesh_operation, mesh_input, mesh_output;
    std::vector<std::string> mesh_operands;
    std::string text_font, text_font_name, text_align = "center";
    std::optional<int> text_char_gap;
    bool text_fonts = false, text_set = false, text_use_surface = false, text_surface_outside = false;
    std::string text_surface;
    std::optional<Vec3d> text_at, text_normal, text_up;
    std::optional<double> text_angle;
    std::optional<float> text_size, text_depth, text_line_spacing;
    bool text_bold = false, text_italic = false;
    bool mesh_a_set = false;
    std::optional<int> draco_bits;
    std::optional<double> simplify_ratio;
    std::optional<float> simplify_max_error;
    std::optional<uint32_t> simplify_triangles;
    std::string wipe_tower_only;
    double      arrange_spacing  = -1.;
    bool        arrange_rotation = true;
    bool        arrange_multi_materials = true;
    bool        surface_grid    = false;
    size_t      surface_grid_max_cells = 40000;
    std::string patch_preview;
    double      preview_budget        = 2.;
    size_t      preview_max_triangles = 300000;
    std::string confirm_up;
    std::vector<std::string> argv;
    Vec2d       center     = Vec2d::Zero();
};

struct StepMeshing
{
    double linear_deflection = 0.003;
    double angle_deflection  = 0.5;
    bool   split_compound    = false;
} step_meshing;

void clear_object_modes(Model &model)
{
    for (ModelObject *object : model.objects) {
        object->config.erase("nonplanar_mode");
        for (ModelVolume *volume : object->volumes)
            volume->config.erase("nonplanar_mode");
        for (auto &[range, range_config] : object->layer_config_ranges)
            range_config.erase("nonplanar_mode");
    }
}

Model read_input(const std::string &file)
{
    if (boost::algorithm::iends_with(file, ".step") || boost::algorithm::iends_with(file, ".stp"))
        return Model::read_from_step(file, LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances, nullptr, nullptr, nullptr,
                                     step_meshing.linear_deflection, step_meshing.angle_deflection, step_meshing.split_compound);
    return Model::read_from_file(file, nullptr, nullptr, LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances);
}

[[noreturn]] void usage(const std::string &error)
{
    std::cerr << "error: " << error << "\n"
              << "usage: nonplanar_slice_tool --printer NAME --process NAME --filament NAME [--vendor Prusa]\n"
                 "       --detect-regions OUT.json MODEL [--printer NAME --vendor VENDOR] (no slicing)\n"
                 "       --plan-tops OUT.json: with the print arguments and no -o, the curved tops plan per detected region\n"
                 "       [--profiles DIR] [--datadir DIR] [--bed-type NAME] [--set key=value]... [--center X,Y]\n"
                 "       [--config-out FILE] [--project-out FILE] [--head-out FILE] [--gcode-3mf FILE] [--json FILE]\n"
                 "       [--field-file FIELD.npz] [--dump-field FIELD.npz]\n"
                 "       [--atom TOOLPATH] [--verify] [--explain] [--auto-orient | --orient-only | --orient-scores] [--floating] [--save-3mf PROJECT.3mf]\n"
                 "       [--plate-index N] [--layer-profile adaptive:QUALITY | ramp:H_BOTTOM:H_TOP | range:DEPTH:H | file:PATH] [--calib-head ID]\n"
                 "       -o OUT.gcode (MODEL | PROJECT.3mf | --plate PLATE.json | --project SPEC.json | --calib TEST)\n"
                 "       [--moves-out FILE [--moves-extended]] [--progress] [--calib-params key=value,...] [--preset-cache DIR]\n"
                 "       [--stage-cache DIR [--stage-cache-max-mb N]]\n"
                 "       (plate JSON objects take \"plate\": N; a plate JSON of several plates slices with --plate-index N)\n"
                 "       (plate JSON objects and volumes take \"paint\"; plate JSON objects take \"layer_height_profile\")\n"
                 "       (plate JSON volumes take \"type\": \"seam\" and \"seam_mode\": center|left|right|enforced|blocked|neutral)\n"
                 "       (plate JSON objects take \"ranges\")\n"
                 "       (plate JSON objects take \"brim_ears\": [{\"x\": X, \"y\": Y, \"radius\": R}])\n"
                 "       (plate JSON plates take \"settings\" and \"locked\"; --save-3mf preserves per-plate settings and locks)\n"
                 "       [--surface-grid [--surface-grid-max-cells N]]   (with --json: per-cell top surface error)\n"
                 "       [--force-planar]   (nonplanar_mode off on the plate, objects, volumes and height ranges)\n"
                 "       [--allow-mixed-temperatures]\n"
                 "       nonplanar_slice_tool --explode-3mf IN.3mf OUTDIR   (editable plate.json and volume meshes, no slice)\n"
                 "       [--edit EDITS.json]   (a loaded PROJECT.3mf: add/delete objects and instances, set filaments; --edit takes \"add_volume\"; --edit takes \"set_paint\")\n"
                 "       [--cut-only Z | X,Y,Z,NX,NY,NZ [--cut-keep upper|lower|both] [--cut-place-lower]]\n"
                 "       [--cut-connectors JSON]   (plug/dowel/snap, shape, pos, radius, height/depth, tolerance)\n"
                 "       [--cut-mode planar|dovetail] [--cut-dovetail JSON]\n"
                 "       [--arrange-only OUT.json [--arrange-spacing MM] [--arrange-no-rotation]]   (the desktop's arrange, no slice)\n"
                 "       [--arrange-multi-materials 0|1]   (with --arrange-only; default 1)\n"
                 "       [--wipe-tower-only OUT.json]   (with --plate: per-plate prime tower estimate, no slice)\n"
                 "       [--patch-preview FILE.json [--patch-preview-budget S] [--patch-preview-max-triangles N]]\n"
                 "       [--step-linear-deflection MM] [--step-angle-deflection A] [--step-split-compound]\n"
                 "       text surface: --use-surface --surface TARGET --at X,Y,Z --normal NX,NY,NZ [--up UX,UY,UZ] [--angle DEG] [--surface-outside]\n"
                 "       nonplanar_slice_tool --export-3mf-generic IN.3mf OUT.3mf   (geometry only)\n"
                 "       nonplanar_slice_tool --export-drc IN OUT.drc [--bits N]   (geometry only)\n"
                 "       nonplanar_slice_tool --emboss-svg IN.svg OUT.stl|OUT.3mf --depth MM [--size MM]\n"
                 "       SVG --size scales the larger side; optional --use-surface --surface TARGET --at X,Y,Z --normal X,Y,Z [--up X,Y,Z] [--angle DEG] [--surface-outside]\n"
                 "       nonplanar_slice_tool --emboss-fonts   (bundled font catalog as JSON)\n"
                 "       nonplanar_slice_tool --emboss-text OUT.stl|OUT.3mf --text TEXT [--font FILE | --font-name NAME] --size MM --depth MM\n"
                 "                           [--bold] [--italic] [--line-spacing FACTOR] [--char-gap N] [--align left|center|right]   (desktop emboss, no slice)\n"
                 "       nonplanar_slice_tool --mesh-boolean union|difference|intersection --a A --b B [--b C] OUT.stl|OUT.3mf\n"
                 "       nonplanar_slice_tool --repair-mesh IN OUT.stl|OUT.3mf   (desktop mesh repair, no slice)\n"
                 "       nonplanar_slice_tool --simplify-mesh IN OUT.stl|OUT.3mf [--ratio R | --triangles N] [--max-error E]   (QEM, no slice)\n"
                 "       nonplanar_slice_tool --dump-options FILE | --dump-presets VENDOR FILE [--profiles DIR]\n"
                 "       (--dump-presets filament entries carry filament_id and setting_id)\n"
                 "       nonplanar_slice_tool --load-gcode FILE.gcode   (times the preview's G-code load)\n";
    std::exit(2);
}

std::optional<CalibMode> classic_calib_mode(const std::string &kind);
std::vector<coordf_t> read_layer_profile(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
        usage("--layer-profile file:" + path + ": cannot open the file");
    std::vector<coordf_t> profile;
    std::string           line;
    for (int number = 1; std::getline(in, line); ++number) {
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        std::istringstream fields(line);
        double             z, h;
        std::string        rest;
        if (!(fields >> z >> h) || (fields >> rest) || !std::isfinite(z) || !std::isfinite(h))
            usage("--layer-profile file:" + path + ": line " + std::to_string(number) + " is not a \"z h\" pair: " + line);
        if (h <= 0.)
            usage("--layer-profile file:" + path + ": line " + std::to_string(number) + " has a layer height <= 0");
        if (!profile.empty() && z <= profile[profile.size() - 2])
            usage("--layer-profile file:" + path + ": line " + std::to_string(number) + " does not increase z");
        profile.insert(profile.end(), { z, h });
    }
    if (profile.size() < 4)
        usage("--layer-profile file:" + path + " needs at least two \"z h\" lines");
    return profile;
}

Args parse_args(int argc, char **argv)
{
    Args a;
    a.argv.assign(argv, argv + argc);
    for (int i = 1; i < argc; ++i) {
        const std::string arg  = argv[i];
        auto              next = [&]() -> std::string {
            if (i + 1 >= argc)
                usage(arg + " needs a value");
            return argv[++i];
        };
        if (arg == "--export-3mf-generic" || arg == "--export-drc") {
            if (!a.mesh_operation.empty()) usage("choose one mesh operation");
            a.mesh_operation = arg == "--export-drc" ? "draco" : "generic3mf";
            a.mesh_input = next();
            a.mesh_output = next();
        } else if (arg == "--bits") {
            const std::string value = next();
            size_t used = 0;
            int bits = 0;
            try { bits = std::stoi(value, &used); } catch (const std::exception &) { usage("--bits expects 0 or an integer from 8 to 30"); }
            if (used != value.size() || (bits != 0 && (bits < DRC_BITS_MIN || bits > DRC_BITS_MAX))) usage("--bits expects 0 or an integer from 8 to 30");
            a.draco_bits = bits;
        } else if (arg == "--emboss-svg") {
            if (!a.mesh_operation.empty()) usage("choose one mesh operation");
            a.mesh_operation = "svg";
            a.mesh_input = next();
            a.mesh_output = next();
        } else if (arg == "--emboss-text") {
            if (!a.mesh_operation.empty()) usage("choose one mesh operation");
            a.mesh_operation = "text";
            a.mesh_output = next();
        } else if (arg == "--use-surface") a.text_use_surface = true;
        else if (arg == "--surface-outside") a.text_surface_outside = true;
        else if (arg == "--surface") a.text_surface = next();
        else if (arg == "--at" || arg == "--normal" || arg == "--up") {
            const std::string value = next();
            Vec3d vector;
            char extra;
            if (std::sscanf(value.c_str(), "%lf,%lf,%lf%c", &vector.x(), &vector.y(), &vector.z(), &extra) != 3 || !vector.allFinite())
                usage(arg + " expects three finite comma-separated coordinates");
            if (arg != "--at" && (!std::isfinite(vector.norm()) || vector.norm() < 1e-9)) usage(arg + " needs a nonzero direction");
            if (arg == "--at") a.text_at = vector;
            else if (arg == "--normal") a.text_normal = vector;
            else a.text_up = vector;
        } else if (arg == "--angle") {
            const std::string value = next();
            size_t used = 0;
            double angle = 0.;
            try { angle = std::stod(value, &used); } catch (const std::exception &) { usage("--angle expects finite degrees"); }
            if (used != value.size() || !std::isfinite(angle)) usage("--angle expects finite degrees");
            a.text_angle = angle;
        } else if (arg == "--text") { a.mesh_input = next(); a.text_set = true; }
        else if (arg == "--emboss-fonts") a.text_fonts = true;
        else if (arg == "--font-name") {
            a.text_font_name = next();
            if (a.text_font_name.empty()) usage("--font-name needs a bundled font name");
        }
        else if (arg == "--align") {
            a.text_align = next();
            if (a.text_align != "left" && a.text_align != "center" && a.text_align != "right") usage("--align expects left, center or right");
        } else if (arg == "--char-gap") {
            const std::string value = next();
            size_t used = 0;
            double gap = 0.;
            try { gap = std::stod(value, &used); } catch (const std::exception &) { usage("--char-gap expects finite font units"); }
            if (used != value.size() || !std::isfinite(gap) || std::abs(gap) > 20000.) usage("--char-gap expects font units in [-20000, 20000]");
            a.text_char_gap = int(std::round(gap));
        } else if (arg == "--font") {
            a.text_font = next();
            if (a.text_font.empty()) usage("--font needs a font file");
        }
        else if (arg == "--bold") a.text_bold = true;
        else if (arg == "--italic") a.text_italic = true;
        else if (arg == "--size" || arg == "--depth" || arg == "--line-spacing") {
            const std::string value = next();
            size_t used = 0;
            double number = 0.;
            try { number = std::stod(value, &used); }
            catch (const std::exception &) { usage(arg + " expects a positive finite number"); }
            if (used != value.size() || !std::isfinite(number) || number <= 0. || number > std::numeric_limits<float>::max() || float(number) == 0.f)
                usage(arg + " expects a positive finite number");
            if (arg == "--size") a.text_size = float(number);
            else if (arg == "--depth") a.text_depth = float(number);
            else a.text_line_spacing = float(number);
        } else if (arg == "--mesh-boolean") {
            if (!a.mesh_operation.empty()) usage("choose one mesh operation");
            a.mesh_operation = next();
            if (a.mesh_operation != "union" && a.mesh_operation != "difference" && a.mesh_operation != "intersection")
                usage("--mesh-boolean expects union, difference or intersection");
        } else if (arg == "--a") {
            if (a.mesh_a_set) usage("--a may only be supplied once");
            a.mesh_a_set = true;
            a.mesh_input = next();
        } else if (arg == "--b") {
            if (!a.model.empty()) {
                a.mesh_operands.push_back(std::move(a.model));
                a.model.clear();
            }
            a.mesh_operands.push_back(next());
        } else if (arg == "--repair-mesh" || arg == "--simplify-mesh") {
            if (!a.mesh_operation.empty()) usage("choose one mesh operation");
            a.mesh_operation = arg == "--repair-mesh" ? "repair" : "simplify";
            a.mesh_input = next();
            a.mesh_output = next();
        } else if (arg == "--max-error") {
            const std::string value = next();
            size_t used = 0;
            double error = 0.;
            try { error = std::stod(value, &used); } catch (const std::exception &) { usage("--max-error expects a finite nonnegative QEM bound"); }
            if (used != value.size() || !std::isfinite(error) || error < 0. || error > std::numeric_limits<float>::max())
                usage("--max-error expects a finite nonnegative QEM bound");
            a.simplify_max_error = float(error);
        } else if (arg == "--ratio") {
            const std::string value = next();
            size_t used = 0;
            try { a.simplify_ratio = std::stod(value, &used); }
            catch (const std::exception &) { usage("--ratio expects a number in (0, 1]"); }
            if (used != value.size() || !std::isfinite(*a.simplify_ratio) || *a.simplify_ratio <= 0. || *a.simplify_ratio > 1.)
                usage("--ratio expects a number in (0, 1]");
        } else if (arg == "--triangles") {
            const std::string value = next();
            size_t used = 0;
            unsigned long long count = 0;
            try { count = std::stoull(value, &used); }
            catch (const std::exception &) { usage("--triangles expects a positive 32-bit count"); }
            if (used != value.size() || value.front() == '-' || count == 0 || count > std::numeric_limits<uint32_t>::max())
                usage("--triangles expects a positive 32-bit count");
            a.simplify_triangles = uint32_t(count);
        } else if (arg == "--printer") a.printer = next();
        else if (arg == "--process") a.process = next();
        else if (arg == "--filament") a.filaments.push_back(next());
        else if (arg == "--bed-type") {
            const std::string name = next();
            BedType bed_type;
            if (!ConfigOptionEnum<BedType>::from_string(name, bed_type) || bed_type <= btDefault || bed_type >= btCount)
                usage("--bed-type expects a plate name, e.g. Textured PEI Plate or Cool Plate");
            a.overrides.emplace_back("curr_bed_type", name);
        }
        else if (arg == "--colours") {
            std::string list = next();
            for (size_t pos = 0; pos <= list.size();) {
                const size_t end = std::min(list.find(',', pos), list.size());
                a.colours.push_back(list.substr(pos, end - pos));
                pos = end + 1;
            }
        } else if (arg == "--part") {
            const std::string spec = next();
            const size_t      pos  = spec.rfind(':');
            if (pos == std::string::npos)
                usage("--part expects FILE:EXTRUDER");
            a.parts.emplace_back(spec.substr(0, pos), std::atoi(spec.c_str() + pos + 1));
        } else if (arg == "--paint") a.paint = next();
        else if (arg == "--layer-profile") {
            a.layer_profile = next();
            if (a.layer_profile.rfind("file:", 0) == 0)
                read_layer_profile(a.layer_profile.substr(5));
        } else if (arg == "--vendor") a.vendor = next();
        else if (arg == "--profiles") a.profiles = next();
        else if (arg == "--datadir") a.datadir = next();
        else if (arg == "--config-out") a.config_out = next();
        else if (arg == "--project-out") a.project_out = next();
        else if (arg == "--head-out") a.head_out = next();
        else if (arg == "--gcode-3mf") a.gcode_3mf = next();
        else if (arg == "--preset-cache") a.preset_cache = next();
        else if (arg == "--stage-cache") a.stage_cache = next();
        else if (arg == "--stage-cache-max-mb") {
            const std::string value = next();
            size_t used = 0;
            try { a.stage_cache_max_mb = std::stoull(value, &used); } catch (const std::exception &) { used = 0; }
            if (used != value.size() || a.stage_cache_max_mb == 0)
                usage("--stage-cache-max-mb expects a positive integer");
        }
        else if (arg == "--explain") a.explain = true;
        else if (arg == "--atom") {
            const std::string path = next();
            if (path.empty())
                usage("--atom needs a nonempty toolpath filename");
            a.overrides.emplace_back("nonplanar_atom_toolpath", path);
        }
        else if (arg == "--field-file") {
            a.field_file = fs::absolute(next()).string();
            a.overrides.emplace_back("nonplanar_field_file", a.field_file);
        } else if (arg == "--dump-field") a.dump_field = fs::absolute(next()).string();
        else if (arg == "--json") a.json = next();
        else if (arg == "--moves-out") a.moves_out = next();
        else if (arg == "--moves-extended") a.moves_extended = true;
        else if (arg == "--dump-options") a.dump_options = next();
        else if (arg == "--dump-presets") { a.vendor = next(); a.dump_presets = next(); }
        else if (arg == "--progress") a.progress = true;
        else if (arg == "--move-times") a.move_times = next();
        else if (arg == "--plate-index") {
            const std::string value = next();
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                usage("--plate-index expects a positive 1-based integer");
            try { a.plate_index = std::stoi(value); }
            catch (const std::exception &) { usage("--plate-index is out of range"); }
            if (a.plate_index < 1) usage("--plate-index expects a positive 1-based integer");
        }
        else if (arg == "--plate") a.plate = next();
        else if (arg == "--project") a.project = next();
        else if (arg == "--detect-regions") a.detect_regions = next();
        else if (arg == "--plan-tops") a.plan_tops = next();
        else if (arg == "--save-3mf") a.save_3mf = next();
        else if (arg == "--tip-out") a.tip_out = next();
        else if (arg == "--planned-out") a.planned_out = next();
        else if (arg == "--verify-gcode") a.verify_gcode = next();
        else if (arg == "--calib") a.calib = next();
        else if (arg == "--calib-head") a.calib_head = next();
        else if (arg == "--calib-fit") a.calib_fit = std::atoi(next().c_str());
        else if (arg == "--calib-picks") a.calib_picks = next();
        else if (arg == "--calib-params") a.calib_params = next();
        else if (arg == "--verify") a.verify = true;
        else if (arg == "--auto-orient") a.auto_orient = true;
        else if (arg == "--orient-only") a.auto_orient = a.orient_only = true;
        else if (arg == "--orient-scores") a.orient_scores = true;
        else if (arg == "--floating") a.floating = true;
        else if (arg == "--edit") a.edit = next();
        else if (arg == "--force-planar") a.force_planar = true;
        else if (arg == "--allow-mixed-temperatures") a.allow_mixed_temperatures = true;
        else if (arg == "--surface-grid") a.surface_grid = true;
        else if (arg == "--surface-grid-max-cells") {
            const long n = std::atol(next().c_str());
            if (n < 1) usage("--surface-grid-max-cells expects a positive number");
            a.surface_grid_max_cells = size_t(n);
        }
        else if (arg == "--cut-only") a.cut = next();
        else if (arg == "--cut-connectors") a.cut_connectors = next();
        else if (arg == "--cut-mode") a.cut_mode = next();
        else if (arg == "--cut-dovetail") a.cut_dovetail = next();
        else if (arg == "--arrange-only") a.arrange_only = next();
        else if (arg == "--explode-3mf") { a.explode_3mf = next(); a.explode_directory = next(); }
        else if (arg == "--wipe-tower-only") a.wipe_tower_only = next();
        else if (arg == "--arrange-multi-materials") {
            const std::string value = next();
            if (value != "0" && value != "1") usage("--arrange-multi-materials expects 0 or 1");
            a.arrange_multi_materials = value == "1";
        }
        else if (arg == "--arrange-spacing") {
            const std::string value = next();
            char *end = nullptr;
            a.arrange_spacing = std::strtod(value.c_str(), &end);
            if (value.empty() || *end != '\0' || !(a.arrange_spacing >= 0.))
                usage("--arrange-spacing expects a distance in mm >= 0");
        } else if (arg == "--arrange-no-rotation") a.arrange_rotation = false;
        else if (arg == "--cut-keep") {
            a.cut_keep = next();
            if (a.cut_keep != "upper" && a.cut_keep != "lower" && a.cut_keep != "both")
                usage("--cut-keep expects upper, lower or both");
        } else if (arg == "--cut-place-lower") a.cut_place_lower = true;
        else if (arg == "--patch-preview") a.patch_preview = next();
        else if (arg == "--patch-preview-budget") {
            a.preview_budget = std::atof(next().c_str());
            if (!(a.preview_budget > 0.)) usage("--patch-preview-budget expects seconds > 0");
        } else if (arg == "--patch-preview-max-triangles") {
            const long n = std::atol(next().c_str());
            if (n < 1000) usage("--patch-preview-max-triangles expects at least 1000");
            a.preview_max_triangles = size_t(n);
        } else if (arg == "--step-linear-deflection" || arg == "--step-angle-deflection") {
            const std::string value = next();
            char *end = nullptr;
            const double v = std::strtod(value.c_str(), &end);
            if (value.empty() || *end != '\0' || !(v > 0.))
                usage(arg + " expects a positive number");
            (arg == "--step-linear-deflection" ? step_meshing.linear_deflection : step_meshing.angle_deflection) = v;
        } else if (arg == "--step-split-compound") step_meshing.split_compound = true;
        else if (arg == "--confirm-up") a.confirm_up = next();
        else if (arg == "-o" || arg == "--output") a.output = fs::absolute(next()).string();
        else if (arg == "--set") {
            const std::string kv  = next();
            const size_t      pos = kv.find('=');
            if (pos == std::string::npos)
                usage("--set expects key=value, got " + kv);
            a.overrides.emplace_back(kv.substr(0, pos), kv.substr(pos + 1));
        } else if (arg == "--event") {
            const std::string ev    = next();
            const size_t      colon = ev.find(':');
            CustomGCode::Item item{ 0., CustomGCode::Custom, 1, "", "" };
            const std::string type  = colon == std::string::npos ? "" : ev.substr(colon + 1, ev.find(':', colon + 1) - colon - 1);
            const std::string text  = ev.find(':', colon + 1) == std::string::npos ? "" : ev.substr(ev.find(':', colon + 1) + 1);
            item.print_z            = std::atof(ev.substr(0, colon).c_str());
            if (type == "pause") item.type = CustomGCode::PausePrint;
            else if (type == "template") item.type = CustomGCode::Template;
            else if (type == "colour") { item.type = CustomGCode::ColorChange; item.color = "#FF0000"; }
            else if (type == "change") { item.type = CustomGCode::ToolChange; item.extruder = std::max(1, std::atoi(text.c_str())); }
            else if (type == "custom") {
                item.extra = text;
                for (size_t pos; (pos = item.extra.find("\\n")) != std::string::npos;)
                    item.extra.replace(pos, 2, "\n");
            } else
                usage("--event expects Z:pause, Z:custom:GCODE, Z:template, Z:colour or Z:change:N");
            a.events.emplace_back(item);
        } else if (arg == "--center") {
            const std::string xy = next();
            if (std::sscanf(xy.c_str(), "%lf,%lf", &a.center.x(), &a.center.y()) != 2)
                usage("--center expects X,Y");
            a.center_set = true;
        } else if (! arg.empty() && arg[0] == '-')
            usage("unknown option " + arg);
        else {
            if ((a.mesh_operation == "union" || a.mesh_operation == "difference" || a.mesh_operation == "intersection") && !a.model.empty())
                a.mesh_operands.push_back(std::move(a.model));
            a.model = arg;
        }
    }
    if (!a.field_file.empty() && std::none_of(a.overrides.begin(), a.overrides.end(), [](const auto &o) { return o.first == "nonplanar_mode"; }))
        a.overrides.insert(a.overrides.begin(), { "nonplanar_mode", "field" });
    if (a.cut_mode != "planar" && a.cut_mode != "dovetail") usage("--cut-mode takes planar or dovetail");
    if (a.cut.empty() && (!a.cut_connectors.empty() || a.cut_mode != "planar" || !a.cut_dovetail.empty()))
        usage("cut connector and dovetail options require --cut-only");
    if ((a.mesh_a_set || !a.mesh_operands.empty()) && a.mesh_operation != "union" && a.mesh_operation != "difference" && a.mesh_operation != "intersection")
        usage("--a and --b require --mesh-boolean");
    if (a.text_use_surface) {
        if ((a.mesh_operation != "text" && a.mesh_operation != "svg") || a.text_surface.empty() || !a.text_at || !a.text_normal)
            usage("--use-surface needs --emboss-text or --emboss-svg, --surface TARGET, --at X,Y,Z and --normal X,Y,Z");
    } else if (!a.text_surface.empty() || a.text_at || a.text_normal || a.text_up || a.text_angle || a.text_surface_outside)
        usage("surface placement options need --use-surface");
    if (a.text_fonts) {
        if (!a.mesh_operation.empty() || !a.model.empty() || !a.plate.empty() || !a.project.empty() || !a.explode_3mf.empty()) usage("--emboss-fonts is a standalone operation");
        return a;
    }
    if (a.mesh_operation == "svg" && !a.text_depth) usage("--emboss-svg needs --depth");
    if (a.mesh_operation == "text" && (!a.text_size || !a.text_depth)) usage("--emboss-text needs --size and --depth");
    if (!a.text_font.empty() && !a.text_font_name.empty()) usage("choose --font or --font-name");
    if (a.mesh_operation != "text" && (a.text_set || !a.text_font.empty() || !a.text_font_name.empty() || a.text_char_gap || a.text_align != "center" || a.text_line_spacing || a.text_bold || a.text_italic))
        usage("--font, --size, --depth, --bold, --italic and --line-spacing need --emboss-text");
    if (a.mesh_operation != "text" && a.mesh_operation != "svg" && (a.text_size || a.text_depth)) usage("--size and --depth need --emboss-text or --emboss-svg");
    if (a.draco_bits && a.mesh_operation != "draco") usage("--bits needs --export-drc");
    if (!a.mesh_operation.empty()) {
        if (a.mesh_operation == "union" || a.mesh_operation == "difference" || a.mesh_operation == "intersection") {
            if (!a.mesh_a_set || a.mesh_operands.empty() || a.model.empty()) usage("--mesh-boolean needs --a A --b B OUT");
            a.mesh_output = std::move(a.model);
            a.model.clear();
        }
        if (!a.explode_3mf.empty() || !a.dump_options.empty() || !a.dump_presets.empty() || !a.verify_gcode.empty() || a.orient_scores)
            usage("choose one mesh operation or catalog/project operation");
        if (a.mesh_input.empty() || a.mesh_output.empty() || !a.model.empty() || !a.plate.empty() || !a.project.empty())
            usage("mesh operations need IN and OUT instead of a model, plate or project");
        if (a.mesh_operation == "simplify" && ((a.simplify_ratio && a.simplify_triangles) || (!a.simplify_ratio && !a.simplify_triangles && !a.simplify_max_error)))
            usage("--simplify-mesh needs --ratio, --triangles or --max-error; choose at most one triangle target");
        if (a.mesh_operation != "simplify" && (a.simplify_ratio || a.simplify_triangles || a.simplify_max_error))
            usage("--ratio, --triangles and --max-error need --simplify-mesh");
        return a;
    }
    if (a.simplify_ratio || a.simplify_triangles || a.simplify_max_error) usage("--ratio, --triangles and --max-error need --simplify-mesh");
    if (!a.dump_options.empty() || !a.dump_presets.empty() || !a.verify_gcode.empty() || !a.explode_3mf.empty()) return a;
    if (a.orient_scores) {
        if (a.model.empty()) usage("--orient-scores needs a model");
        return a;
    }
    if (!a.detect_regions.empty()) {
        if (a.model.empty()) usage("--detect-regions OUT.json needs a model");
        return a;
    }
    if (a.printer.empty() || a.process.empty() || a.filaments.empty() ||
        (a.output.empty() && ! a.orient_only && a.plan_tops.empty() && a.confirm_up.empty() && a.save_3mf.empty() && a.patch_preview.empty() && a.arrange_only.empty() && a.wipe_tower_only.empty()) ||
        int(! a.model.empty()) + int(! a.plate.empty()) + int(! a.project.empty()) + int(! a.calib.empty()) != 1)
        usage("--printer, --process, --filament, -o and one of a model, --plate, --project or --calib are required");
    if (a.surface_grid && a.json.empty())
        usage("--surface-grid needs --json");
    if (!a.wipe_tower_only.empty() && a.plate.empty())
        usage("--wipe-tower-only needs --plate");
    if (! a.cut.empty() && a.output.empty())
        usage("--cut-only needs -o PIECES.3mf or -o PIECES.stl");
    if (! a.calib.empty() && ! Nonplanar::Calibration::parse_test(a.calib) && ! classic_calib_mode(a.calib))
        usage("unknown --calib test " + a.calib);
    if (! a.calib_params.empty() && ! classic_calib_mode(a.calib))
        usage("--calib-params needs a classic --calib KIND");
    return a;
}

std::string find_preset_file(const Args &a, const char *kind, const std::string &name)
{
    if (fs::is_regular_file(name))
        return name;
    std::vector<fs::path> dirs = { fs::path(a.profiles) / a.vendor / kind };
    if (std::string(kind) == "filament")
        dirs.push_back(fs::path(a.profiles) / "OrcaFilamentLibrary" / kind);
    for (const fs::path &dir : dirs) {
        if (! fs::is_directory(dir))
            continue;
        for (const fs::directory_entry &entry : fs::recursive_directory_iterator(dir)) {
            if (entry.path().extension() != ".json")
                continue;
            boost::nowide::ifstream in(entry.path().string());
            const nlohmann::json    j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_object() && j.value("name", std::string()) == name)
                return entry.path().string();
        }
    }
    throw std::runtime_error(std::string(kind) + " preset \"" + name + "\" not found under " + dirs.front().string());
}

fs::path preset_cache_file(const Args &a, const std::string &key)
{
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : key)
        hash = (hash ^ c) * 1099511628211ull;
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(hash));
    return fs::path(a.preset_cache) / name;
}

bool read_cached_preset(const fs::path &file, const std::string &key, DynamicPrintConfig &config, std::string &filament_id)
{
    boost::nowide::ifstream in(file.string(), std::ios::binary);
    if (!in)
        return false;
    try {
        cereal::BinaryInputArchive ar(in);
        std::string stored_key, stored_id;
        ar(stored_key);
        if (stored_key != key)
            return false;
        ar(stored_id);
        CacheDictionary dict;
        dict.load(ar);
        DynamicPrintConfig stored;
        load_config(ar, stored, dict);
        config      = std::move(stored);
        filament_id = std::move(stored_id);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

void write_cached_preset(const fs::path &file, const std::string &key, const DynamicPrintConfig &config, const std::string &filament_id)
{
    boost::system::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.parent_path() / fs::unique_path(file.filename().string() + ".%%%%%%%%.tmp");
    try {
        {
            boost::nowide::ofstream out(tmp.string(), std::ios::binary);
            cereal::BinaryOutputArchive ar(out);
            CacheDictionary dict;
            dict.collect(config);
            ar(key, filament_id);
            dict.save(ar);
            save_config(ar, config, dict);
        }
        DynamicPrintConfig back;
        std::string        back_id;
        if (read_cached_preset(tmp, key, back, back_id) && back == config && back_id == filament_id)
            fs::rename(tmp, file, ec);
    } catch (const std::exception &) {
    }
    fs::remove(tmp, ec);
}

DynamicPrintConfig resolve(PresetBundle &bundle, const Args &a, const char *kind, Preset::Type type, const std::string &name)
{
    DynamicPrintConfig config;
    std::string        error, filament_id, preset_name = name;
    if (fs::path(name).extension() == ".json" && fs::is_regular_file(name)) {
        std::map<std::string, std::string> key_values;
        config.load_from_json(name, ForwardCompatibilitySubstitutionRule::EnableSilent, key_values, error);
        if (! error.empty())
            throw std::runtime_error("cannot load " + name + ": " + error);
        preset_name = key_values[BBL_JSON_KEY_NAME];
        if (preset_name.empty())
            preset_name = fs::path(name).stem().string();
        filament_id = key_values[BBL_JSON_KEY_FILAMENT_ID];
    } else {
        const std::string key   = a.vendor + "\n" + kind + "\n" + std::to_string(int(type)) + "\n" + name;
        const fs::path    entry = a.preset_cache.empty() ? fs::path() : preset_cache_file(a, key);
        if (entry.empty() || !read_cached_preset(entry, key, config, filament_id)) {
            const std::string file = find_preset_file(a, kind, name);
            if (!bundle.resolve_preset_config(config, type, file, ForwardCompatibilitySubstitutionRule::EnableSilent, error, true, &filament_id))
                throw std::runtime_error("cannot resolve " + file + ": " + error);
            if (!entry.empty())
                write_cached_preset(entry, key, config, filament_id);
        }
    }
    if (type == Preset::TYPE_PRINTER)
        config.set_key_value("printer_settings_id", new ConfigOptionString(preset_name));
    else if (type == Preset::TYPE_PRINT)
        config.set_key_value("print_settings_id", new ConfigOptionString(preset_name));
    else if (type == Preset::TYPE_FILAMENT) {
        config.set_key_value("filament_settings_id", new ConfigOptionStrings{preset_name});
        if (!filament_id.empty())
            config.set_key_value("filament_ids", new ConfigOptionStrings{filament_id});
    }
    if (type != Preset::TYPE_PRINTER)
        config.erase("printer_settings_id");
    if (type != Preset::TYPE_PRINT)
        config.erase("print_settings_id");
    if (type != Preset::TYPE_FILAMENT)
        config.erase("filament_settings_id");
    return config;
}

Vec3d emitted_tip(const GCodeProcessorResult &result, size_t i, bool rotary)
{
    return !rotary && result.has_joints() ? result.joints[i].xyz : Vec3d(result.moves[i].position.cast<double>());
}

struct WallFragmentation
{
    size_t runs = 0;
    double wall_mm = 0., near_end_mm = 0.;
};
WallFragmentation wall_fragmentation(const GCodeProcessorResult &result, bool rotary)
{
    WallFragmentation out;
    double run_mm = 0.;
    bool   in_run = false;
    Vec3d  last   = Vec3d::Zero();
    auto   close  = [&]() {
        if (in_run)
            out.near_end_mm += std::min(run_mm, 2.), ++out.runs;
        in_run = false, run_mm = 0.;
    };
    size_t prev = 0;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto &m = result.moves[i];
        if (m.internal_only)
            continue;
        const Vec3d a = emitted_tip(result, prev, rotary), b = emitted_tip(result, i, rotary);
        prev          = i;
        const bool  wall = m.type == EMoveType::Extrude && m.delta_extruder > 0.f && is_perimeter(m.extrusion_role);
        if (!wall || (in_run && (a - last).norm() > 1e-3)) {
            close();
            if (!wall)
                continue;
        }
        const double l = (b - a).norm();
        out.wall_mm += l, run_mm += l, in_run = true, last = b;
    }
    close();
    return out;
}

std::pair<double, double> max_vz(const GCodeProcessorResult &result, size_t *actual_line = nullptr, bool rotary = false)
{
    double commanded = 0., actual = 0.;
    size_t previous = 0;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto &m = result.moves[i];
        if (m.internal_only)
            continue;
        const Vec3d d = emitted_tip(result, i, rotary) - emitted_tip(result, previous, rotary);
        const size_t begin = previous + 1;
        previous = i;
        const double l = d.norm();
        if (l < 1e-6 || (m.type != EMoveType::Extrude && m.type != EMoveType::Travel))
            continue;
        const double z_fraction = std::abs(d.z()) / l;
        commanded = std::max(commanded, z_fraction * m.feedrate);
        for (size_t j = begin; j <= i; ++j) {
            const auto &sample = result.moves[j];
            if (sample.gcode_id != m.gcode_id)
                continue;
            if (const double v = z_fraction * sample.actual_feedrate; v > actual) {
                actual = v;
                if (actual_line)
                    *actual_line = m.gcode_id;
            }
        }
    }
    return { commanded, actual };
}

indexed_triangle_set plate_mesh(const Model &model)
{
    indexed_triangle_set out;
    for (const ModelObject *object : model.objects)
        for (const ModelInstance *instance : object->instances) {
            indexed_triangle_set its = object->raw_indexed_triangle_set();
            its_transform(its, instance->get_matrix(), true);
            its_merge(out, its);
        }
    return out;
}

std::string vendor_json_value(const std::vector<fs::path> &dirs, std::string name, const std::string &key)
{
    for (int depth = 0; depth < 8 && ! name.empty(); ++depth) {
        std::string inherits;
        for (const fs::path &dir : dirs) {
            if (! fs::is_directory(dir))
                continue;
            for (const fs::directory_entry &entry : fs::recursive_directory_iterator(dir)) {
                if (entry.path().extension() != ".json")
                    continue;
                boost::nowide::ifstream in(entry.path().string());
                const nlohmann::json    j = nlohmann::json::parse(in, nullptr, false);
                if (! j.is_object() || j.value("name", std::string()) != name)
                    continue;
                if (j.contains(key) && j[key].is_string())
                    return j[key].get<std::string>();
                inherits = j.value("inherits", std::string());
                break;
            }
            if (! inherits.empty())
                break;
        }
        name = inherits;
    }
    return {};
}

void write_gcode_3mf(const Args &args, Model &model, Print &print, DynamicPrintConfig &config, GCodeProcessorResult &result,
                     const PlateData *source_plate)
{
    auto plate                     = source_plate ? std::make_unique<PlateData>(*source_plate) : std::make_unique<PlateData>();
    plate->objects_and_instances.clear();
    if (!source_plate)
        plate->config.apply_only(config, {"filament_map_mode", "filament_map", "filament_volume_map"});
    else if (!plate->config.has("filament_map_mode"))
        plate->config.apply_only(config, {"filament_map_mode"});
    if (const auto *mode = plate->config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode"); mode && is_auto_filament_map_mode(mode->value)) {
        plate->config.option<ConfigOptionInts>("filament_map", true)->values        = print.get_filament_maps();
        plate->config.option<ConfigOptionInts>("filament_volume_map", true)->values = print.get_filament_volume_maps();
    }
    if (const auto *map = plate->config.option<ConfigOptionInts>("filament_map")) {
        plate->filament_maps = map->values;
        config.option<ConfigOptionInts>("filament_map", true)->values = map->values;
    }
    if (const auto *volumes = plate->config.option<ConfigOptionInts>("filament_volume_map"))
        config.option<ConfigOptionInts>("filament_volume_map", true)->values = volumes->values;
    plate->plate_index             = 0;
    for (size_t i = 0; i < model.objects.size(); ++i)
        for (size_t j = 0; j < model.objects[i]->instances.size(); ++j)
            plate->objects_and_instances.emplace_back(int(i), int(j));
    plate->gcode_file              = args.output;
    plate->is_sliced_valid         = true;
    plate->toolpath_outside        = result.toolpath_outside;
    plate->timelapse_warning_code  = result.timelapse_warning_code;
    plate->limit_filament_maps     = result.limit_filament_maps;
    plate->layer_filaments         = result.layer_filaments;
    plate->filament_change_sequence = result.filament_change_sequence;
    plate->nozzle_change_sequence  = result.nozzle_change_sequence;
    plate->optimal_assignment     = result.optimal_assignment;
    plate->is_label_object_enabled = result.label_object_enabled;
    plate->is_support_used         = print.is_support_used();
    plate->printer_model_id = vendor_json_value({ fs::path(args.profiles) / args.vendor / "machine" }, config.opt_string("printer_model"), "model_id");
    plate->nozzle_diameters = config.option("nozzle_diameter")->serialize();
    plate->gcode_prediction = std::to_string(int(result.print_statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)].time));
    plate->first_layer_time = std::to_string(result.initial_layer_time);
    plate->nonplanar_report.clear();
    if (!result.nonplanar_report.empty())
        plate->nonplanar_report = result.nonplanar_report.to_json();
    plate->slice_filaments_info.clear();
    plate->mixed_filaments_info.clear();
    plate->nozzle_group_result.reset();
    plate->parse_filament_info(&result);
    nlohmann::json bambu_ids;
    {
        boost::nowide::ifstream in((fs::path(resources_dir()) / "printers" / "bambu_filament_ids.json").string());
        bambu_ids = nlohmann::json::parse(in, nullptr, false);
    }
    for (FilamentInfo &info : plate->slice_filaments_info) {
        std::string display_type;
        info.type        = config.get_filament_type(display_type, int(info.id));
        info.color       = config.option<ConfigOptionStrings>("filament_colour")->get_at(info.id);
        const auto *ids = config.option<ConfigOptionStrings>("filament_ids");
        info.filament_id = ids && info.id < ids->values.size() ? ids->values[info.id] : std::string();
        if (info.filament_id.empty())
            info.filament_id = vendor_json_value({ fs::path(args.profiles) / args.vendor / "filament",
                                               fs::path(args.profiles) / "OrcaFilamentLibrary" / "filament" },
                                             args.filaments[std::min(size_t(info.id), args.filaments.size() - 1)], "filament_id");
        if (bambu_ids.is_object() && bambu_ids["filaments"].contains(info.filament_id))
            info.filament_id = bambu_ids["filaments"][info.filament_id].value("bambu_id", info.filament_id);
    }
    char weight[32];
    std::snprintf(weight, sizeof(weight), "%.2f", print.print_statistics().total_weight);
    plate->gcode_weight = weight;

    const indexed_triangle_set mesh     = plate_mesh(model);
    const std::string &colour = config.option<ConfigOptionStrings>("filament_colour")->get_at(0);
    ThumbnailData plate_png = render_thumbnail(mesh, 512, 512, false, colour), top_png = render_thumbnail(mesh, 512, 512, true, colour);
    ThumbnailData no_light_png = plate_png, pick_png = top_png;
    if (!plate->thumbnail_file.empty() && fs::exists(plate->thumbnail_file))
        plate_png.reset();
    else
        plate->plate_thumbnail.load_from(plate_png);
    if (!plate->no_light_thumbnail_file.empty() && fs::exists(plate->no_light_thumbnail_file))
        no_light_png.reset();
    if (!plate->top_file.empty() && fs::exists(plate->top_file))
        top_png.reset();
    if (!plate->pick_file.empty() && fs::exists(plate->pick_file))
        pick_png.reset();

    PlateBBoxData bboxes;
    bboxes.is_seq_print     = config.opt_enum<PrintSequence>("print_sequence") == PrintSequence::ByObject;
    bboxes.first_extruder   = print.get_tool_ordering().first_extruder();
    bboxes.nozzle_diameter  = float(config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0));
    bboxes.bed_type         = bed_type_to_gcode_string(config.opt_enum<BedType>("curr_bed_type"));
    bboxes.first_layer_time = result.initial_layer_time;
    BoundingBoxf all;
    for (PrintObject *object : print.objects()) {
        BBoxData   data;
        const auto b = unscaled(object->get_first_layer_bbox(data.area, data.layer_height, data.name));
        all.merge(BoundingBoxf(b.min, b.max));
        data.area *= float(SCALING_FACTOR * SCALING_FACTOR);
        data.id   = int(object->id().id);
        data.bbox = { b.min.x(), b.min.y(), b.max.x(), b.max.y() };
        bboxes.bbox_objs.push_back(data);
    }
    bboxes.bbox_all = { all.min.x(), all.min.y(), all.max.x(), all.max.y() };
    for (const FilamentInfo &info : plate->slice_filaments_info) {
        bboxes.filament_ids.push_back(info.id);
        bboxes.filament_colors.push_back(info.color);
    }

    if (auto *printers = config.option<ConfigOptionStrings>("print_compatible_printers", true); printers->values.empty())
        printers->values = { config.opt_string("printer_settings_id") };

    model.get_backup_path();
    StoreParams params;
    params.path                    = args.gcode_3mf;
    params.model                   = &model;
    params.plate_data_list         = { plate.get() };
    params.export_plate_idx        = 0;
    params.config                  = &config;
    params.thumbnail_data          = { &plate_png };
    params.no_light_thumbnail_data = { &no_light_png };
    params.top_thumbnail_data      = { &top_png };
    params.pick_thumbnail_data     = { &pick_png };
    params.id_bboxes               = { &bboxes };
    params.strategy = SaveStrategy::Silence | SaveStrategy::WithGcode | SaveStrategy::SplitModel | SaveStrategy::ShareMesh |
                      SaveStrategy::SkipModel;
    if (source_plate)
        params.strategy = params.strategy | SaveStrategy::UseLoadedId;
    if (! store_bbs_3mf(params))
        throw std::runtime_error("cannot write " + args.gcode_3mf);
}

void apply_filaments(PresetBundle &bundle, const Args &a, DynamicPrintConfig &config)
{
    const size_t n = a.filaments.size();
    std::vector<DynamicPrintConfig> slots;
    for (const std::string &name : a.filaments)
        slots.emplace_back(resolve(bundle, a, "filament", Preset::TYPE_FILAMENT, name));
    config.apply(slots.front());
    if (n == 1 && a.colours.empty())
        return;
    for (const std::string &key : slots.front().keys())
        if (auto *dst = dynamic_cast<ConfigOptionVectorBase *>(config.optptr(key))) {
            const auto *first = dynamic_cast<const ConfigOptionVectorBase *>(slots.front().optptr(key));
            if (first == nullptr || first->empty())
                continue;
            dst->resize(n, first);
            for (size_t i = 0; i < n; ++i)
                if (const ConfigOption *src = slots[i].optptr(key))
                    dst->set_at(src, i, 0);
        }
    static const char *defaults[] = { "#F2F2F2", "#D02020", "#2050D0", "#20A040", "#E0C020", "#202020", "#E07020", "#9040C0" };
    std::vector<std::string> colours;
    for (size_t i = 0; i < n; ++i)
        colours.push_back(i < a.colours.size() ? a.colours[i] : defaults[i % 8]);
    config.option<ConfigOptionStrings>("filament_colour", true)->values = colours;
    if (auto *map = config.option<ConfigOptionInts>("filament_map", true))
        map->values.assign(n, 1);
    auto rgb = [](const std::string &hex, unsigned char out[3]) {
        for (int c = 0; c < 3; ++c)
            out[c] = (unsigned char) std::strtol(hex.substr(1 + 2 * c, 2).c_str(), nullptr, 16);
    };
    const auto *volume  = config.option<ConfigOptionFloats>("nozzle_volume");
    const auto *dataset = config.option<ConfigOptionInts>("nozzle_flush_dataset");
    const int   min_vol = volume != nullptr && !volume->values.empty() ? int(volume->values.front()) : 0;
    FlushVolCalculator calculator(min_vol, g_max_flush_volume, dataset != nullptr && !dataset->values.empty() ? dataset->values.front() : 0);
    std::vector<double> &matrix = config.option<ConfigOptionFloats>("flush_volumes_matrix", true)->values;
    matrix.assign(n * n, 0.);
    for (size_t from = 0; from < n; ++from)
        for (size_t to = 0; to < n; ++to)
            if (from != to) {
                unsigned char f[3], t[3];
                rgb(colours[from], f);
                rgb(colours[to], t);
                matrix[from * n + to] = calculator.calc_flush_vol(255, f[0], f[1], f[2], 255, t[0], t[1], t[2]);
            }
    config.option<ConfigOptionFloats>("flush_multiplier", true)->values.resize(1, 1.);
    config.option<ConfigOptionFloats>("flush_volumes_vector", true)->values.assign(2 * n, 140.);
}

void apply_multi_material(const Args &a, Model &model)
{
    ModelObject *object = model.objects.front();
    if (!a.paint.empty()) {
        ModelVolume            *volume = object->volumes.front();
        boost::nowide::ifstream in(a.paint);
        std::vector<int>        ids;
        for (int id; in >> id;)
            ids.push_back(id);
        if (ids.size() != volume->mesh().its.indices.size())
            throw std::runtime_error("--paint has " + std::to_string(ids.size()) + " entries, the mesh " +
                                     std::to_string(volume->mesh().its.indices.size()) + " triangles");
        TriangleSelector selector(volume->mesh());
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] > 0)
                selector.set_facet(int(i), EnforcerBlockerType(ids[i]));
        volume->mmu_segmentation_facets.set(selector);
    }
    for (const auto &[file, extruder] : a.parts) {
        Model part = read_input(file);
        if (part.objects.empty())
            throw std::runtime_error("no objects in " + file);
        const ModelVolume *src    = part.objects.front()->volumes.front();
        ModelVolume       *volume = object->add_volume(src->mesh());
        volume->set_transformation(src->get_transformation());
        volume->config.set("extruder", extruder);
    }
    if (!a.parts.empty())
        object->volumes.front()->config.set("extruder", 1);
}

void place_wipe_tower(DynamicPrintConfig &config, const Model &model, size_t filaments)
{
    double x = 165., y = 250.;
    if (const auto *structure = config.option<ConfigOptionEnum<PrinterStructure>>("printer_structure");
        structure != nullptr && structure->value == PrinterStructure::psI3)
        x = 0.;
    std::vector<unsigned int> ids(filaments);
    for (size_t i = 0; i < filaments; ++i)
        ids[i] = unsigned(i);
    double height = 0.;
    for (const ModelObject *object : model.objects)
        height = std::max(height, object->bounding_box_exact().size().z());
    const WipeTowerFootprint fp = estimate_wipe_tower_footprint(config, resolve_wipe_tower_type(config), ids,
                                                                config.opt_float("layer_height"), height);
    const BoundingBoxf bed(config.opt<ConfigOptionPoints>("printable_area")->values);
    const double       margin = WIPE_TOWER_AUTO_MARGIN + fp.brim_width;
    x = std::clamp(x, bed.min.x() + margin, std::max(bed.min.x() + margin, bed.max.x() - fp.width - margin));
    y = std::clamp(y, bed.min.y() + margin, std::max(bed.min.y() + margin, bed.max.y() - fp.depth - margin));
    Polygons outline{ Slic3r::Polygon::new_scale(config.opt<ConfigOptionPoints>("printable_area")->values) };
    const Vec2f move = WipeTower::move_box_inside_polygon(BoundingBox(Point::new_scale(x, y), Point::new_scale(x + fp.width, y + fp.depth)),
                                                          outline, scaled<coord_t>(margin));
    config.option<ConfigOptionFloats>("wipe_tower_x", true)->values = { x + move.x() };
    config.option<ConfigOptionFloats>("wipe_tower_y", true)->values = { y + move.y() };
}

std::string layer_profile_mismatch(const std::vector<coordf_t> &profile, const SlicingParameters &sp)
{
    const double height = sp.object_print_z_uncompensated_max - sp.object_print_z_min;
    std::ostringstream why;
    why.precision(6);
    if (std::abs(profile[profile.size() - 2] - height) > 1e-3)
        why << "its last z " << profile[profile.size() - 2] << " is not the object's top " << height << " (within 0.001 mm)";
    else if (profile[1] != sp.first_object_layer_height)
        why << "its first layer height " << profile[1] << " is not the first layer height " << sp.first_object_layer_height;
    return why.str();
}

std::vector<ObjectID> plate_layer_profiles;

void check_plate_layer_profiles(const Print &print)
{
    for (const PrintObject *po : print.objects()) {
        if (std::find(plate_layer_profiles.begin(), plate_layer_profiles.end(), po->model_object()->id()) == plate_layer_profiles.end())
            continue;
        if (const std::string why = layer_profile_mismatch(po->model_object()->layer_height_profile.get(), po->slicing_parameters()); !why.empty())
            throw std::runtime_error("plate object " + po->model_object()->name + ": layer_height_profile: " + why);
    }
}

void apply_layer_profile(const Args &args, Model &model, Print &print, const DynamicPrintConfig &config)
{
    if (args.layer_profile.empty())
        return;
    const size_t colon = args.layer_profile.find(':');
    const std::string kind = args.layer_profile.substr(0, colon);
    double a = 0., b = 0.;
    const int values = colon == std::string::npos ? 0 : std::sscanf(args.layer_profile.c_str() + colon + 1, "%lf:%lf", &a, &b);
    std::vector<coordf_t> given;
    if (kind == "file")
        given = read_layer_profile(args.layer_profile.substr(colon + 1));
    else if (!((kind == "adaptive" && values >= 1) || ((kind == "ramp" || kind == "range") && values == 2)))
        usage("--layer-profile expects adaptive:Q, ramp:H0:H1, range:D:H or file:PATH");
    for (ModelObject *object : model.objects) {
        const PrintObject *print_object = nullptr;
        for (const PrintObject *po : print.objects())
            if (po->model_object()->id() == object->id())
                print_object = po;
        if (print_object == nullptr)
            continue;
        const SlicingParameters &sp  = print_object->slicing_parameters();
        const double             top = sp.object_print_z_uncompensated_height();
        if (kind == "file") {
            if (const std::string why = layer_profile_mismatch(given, sp); !why.empty())
                usage("--layer-profile " + args.layer_profile + ": " + why);
            object->layer_height_profile.set(given);
        } else if (kind == "adaptive")
            object->layer_height_profile.set(layer_height_profile_adaptive(sp, *object, float(a)));
        else if (kind == "ramp")
            object->layer_height_profile.set(std::vector<coordf_t>{ 0., sp.first_object_layer_height, sp.first_object_layer_height, a, top, b });
        else {
            ModelConfig range;
            range.set("layer_height", b);
            object->layer_config_ranges[{ std::max(0., top - a), top }] = range;
        }
    }
    print.apply(model, config);
}

struct CurvedTopsPlan {
    std::map<std::pair<int, int>, std::string> facets;
    double max_angle = 0.;
    bool   head_unknown = false;
    std::string access_by;
    std::pair<double, double> band;
};

CurvedTopsPlan curved_tops_plan(const PrintObject &object)
{
    CurvedTopsPlan plan;
    plan.max_angle    = object.config().nonplanar_curved_max_angle.value;
    plan.head_unknown = curved_tops_stats(object).planar_limit == "head_unknown";
    plan.band         = object.curved_band();
    plan.access_by    = Nonplanar::clearance_override_active(object.print()->config()) ? "nozzle" : "head";
    for (const CurvedSurfaceDrop &drop : object.curved_dropped_surfaces())
        for (int facet : drop.facets)
            plan.facets.emplace(std::make_pair(drop.volume, facet), drop.reason);
    for (const NonplanarSurface &surface : object.nonplanar_surfaces())
        for (const auto &facet : surface.mesh)
            plan.facets[{ surface.volume, facet.first }] = "curved";
    return plan;
}

std::vector<std::optional<CurvedTopsPlan>> plan_curved_tops(Print &print, const Model &model, const DynamicPrintConfig &config)
{
    print.apply(model, config);
    print.plan_curved_tops();
    std::vector<std::optional<CurvedTopsPlan>> plans(model.objects.size());
    for (size_t i = 0; i < model.objects.size(); ++i)
        for (const PrintObject *object : print.objects())
            if (object->model_object()->id() == model.objects[i]->id())
                plans[i] = curved_tops_plan(*object);
    return plans;
}

nlohmann::json region_plan(const Nonplanar::Region &region, int volume, const std::vector<double> &areas, const std::vector<double> &slopes,
                           const CurvedTopsPlan &plan, double asked_angle)
{
    double curved = 0., total = 0.;
    std::map<std::string, double> flat;
    for (int facet : region.facets) {
        total += areas[facet];
        std::string why = "other";
        if (plan.head_unknown)
            why = "head_unknown";
        else if (const auto it = plan.facets.find({ volume, facet }); it != plan.facets.end())
            why = it->second;
        else if (slopes[facet] > plan.max_angle)
            why = "angle";
        (why == "curved" ? curved : flat[why]) += areas[facet];
    }
    const double share = total > 0. ? curved / total : 0.;
    nlohmann::json out = { { "curved", share >= 0.5 }, { "curved_share", share }, { "planar_limit", nullptr }, { "reachable_slope_deg", nullptr } };
    const auto limit = std::max_element(flat.begin(), flat.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    if (limit != flat.end() && total - curved >= 0.01 * total) {
        out["planar_limit"] = limit->first;
        if (limit->first == "access" || limit->first == "angle")
            out["reachable_slope_deg"] = plan.max_angle;
        if (limit->first == "access")
            out["access_by"] = plan.access_by;
    }
    return out;
}

void write_plan_tops(const Args &args, Model &model, const DynamicPrintConfig &given, Print &print)
{
    DynamicPrintConfig config = given;
    if (!curved_tops_mode(config.opt_enum<NonplanarMode>("nonplanar_mode")))
        config.set_key_value("nonplanar_mode", new ConfigOptionEnum<NonplanarMode>(NonplanarMode::NonplanarTop));
    const NonplanarMode mode        = config.opt_enum<NonplanarMode>("nonplanar_mode");
    const double        asked_angle = config.opt_float("nonplanar_curved_max_angle");
    DynamicPrintConfig  duct        = config;
    duct.set_key_value("nonplanar_ignore_duct", new ConfigOptionBool(true));
    duct.set_key_value("nonplanar_curved_max_angle", new ConfigOptionFloat(std::max(asked_angle, Nonplanar::curved_max_angle_limit(mode))));

    auto t0 = std::chrono::steady_clock::now();
    const auto asked = plan_curved_tops(print, model, config);
    const auto head  = Nonplanar::resolve_head_envelope(print.config());
    const double asked_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    t0 = std::chrono::steady_clock::now();
    Print duct_print;
    duct_print.is_BBL_printer() = print.is_BBL_printer();
    const auto ducted = plan_curved_tops(duct_print, model, duct);
    const double duct_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    nlohmann::json report = { { "schema", "orca.nonplanar.plan_tops.v1" }, { "label", "plan" }, { "head", head.id }, { "model", args.model },
                              { "mode", config.opt_serialize("nonplanar_mode") }, { "max_angle_deg", asked_angle },
                              { "ignore_duct", config.opt_bool("nonplanar_ignore_duct") },
                              { "ignore_duct_pass", { { "max_angle_deg", duct.opt_float("nonplanar_curved_max_angle") } } },
                              { "seconds", { { "plan", asked_s }, { "ignore_duct_pass", duct_s } } },
                              { "not_planned", nlohmann::json::array({ "clearance repair", "time and quality guards" }) },
                              { "objects", nlohmann::json::array() }, { "regions", nlohmann::json::array() } };
    for (size_t oi = 0; oi < model.objects.size(); ++oi) {
        if (!asked[oi] || !ducted[oi])
            continue;
        const ModelObject &object = *model.objects[oi];
        nlohmann::json jo = { { "object", oi }, { "name", object.name }, { "max_angle_deg", asked[oi]->max_angle },
                              { "ignore_duct_max_angle_deg", ducted[oi]->max_angle } };
        if (asked[oi]->band.second > asked[oi]->band.first)
            jo["curved_band"] = { { "from", asked[oi]->band.first }, { "to", asked[oi]->band.second }, { "layer_height", 0.2 } };
        report["objects"].push_back(std::move(jo));
        const Transform3d instance = object.instances.empty() ? Transform3d::Identity() : object.instances.front()->get_matrix();
        for (size_t vi = 0; vi < object.volumes.size(); ++vi) {
            const ModelVolume &volume = *object.volumes[vi];
            if (!volume.is_model_part())
                continue;
            const Transform3d          transform = instance * volume.get_matrix();
            const indexed_triangle_set &its      = volume.mesh().its;
            std::vector<double>        areas(its.indices.size()), slopes(its.indices.size());
            for (size_t f = 0; f < its.indices.size(); ++f) {
                const Vec3d a = transform * its.vertices[its.indices[f](0)].cast<double>(), b = transform * its.vertices[its.indices[f](1)].cast<double>(),
                            c = transform * its.vertices[its.indices[f](2)].cast<double>();
                const Vec3d n = (b - a).cross(c - a);
                areas[f]      = 0.5 * n.norm();
                slopes[f]     = n.norm() > 0. ? Geometry::rad2deg(std::acos(std::clamp(std::abs(n.z()) / n.norm(), 0., 1.))) : 90.;
            }
            for (const Nonplanar::Region &r : Nonplanar::detect_nonplanar_regions(volume, transform, head)) {
                nlohmann::json jr = { { "object", oi }, { "volume", vi }, { "kind", Nonplanar::region_kind_name(r.kind) }, { "area", r.area },
                                      { "slope", { r.min_slope, r.max_slope } }, { "z_range", { r.min_z, r.max_z } },
                                      { "centroid", { r.centroid.x(), r.centroid.y(), r.centroid.z() } } };
                jr.update(region_plan(r, int(vi), areas, slopes, *asked[oi], asked_angle));
                jr["ignore_duct"] = region_plan(r, int(vi), areas, slopes, *ducted[oi], duct.opt_float("nonplanar_curved_max_angle"));
                jr["facets"]      = r.facets;
                report["regions"].push_back(std::move(jr));
            }
        }
    }
    boost::nowide::ofstream out(args.plan_tops, std::ios::binary);
    out << report.dump(2) << "\n";
    if (!out)
        throw std::runtime_error("cannot write " + args.plan_tops);
    std::cout << "plan_tops regions " << report["regions"].size() << " seconds " << asked_s + duct_s << "\n";
}

void place(const Args &args, Model &model, DynamicPrintConfig &config, bool saved = false)
{
    if (args.plate.empty() && args.calib.empty() && (!saved || args.center_set)) {
        Vec2d center = args.center;
        if (! args.center_set) {
            const BoundingBoxf bed(config.opt<ConfigOptionPoints>("printable_area")->values);
            center = bed.center();
        }
        if (!saved)
            for (ModelObject *object : model.objects)
                object->ensure_on_bed();
        model.center_instances_around_point(center);
    }
    if (!saved && std::none_of(args.overrides.begin(), args.overrides.end(), [](const auto &kv) { return kv.first.rfind("wipe_tower_", 0) == 0; }))
        place_wipe_tower(config, model, args.filaments.size());
}

void rotate_model(Model &model, const Matrix3d &rotation)
{
    const Eigen::AngleAxisd aa(rotation);
    if (std::abs(aa.angle()) < 1e-9)
        return;
    for (ModelObject *object : model.objects)
        object->rotate(aa.angle(), aa.axis());
}

struct OrientSlice
{
    bool        ok = false;
    double      time_s = 0., footprint = 0., support = 0.;
    size_t      hits = 0;
    std::string error;
};

OrientSlice slice_orientation(const Args &args, const Model &source, const Matrix3d &rotation, const DynamicPrintConfig &config_in,
                              const fs::path &dir)
{
    OrientSlice out;
    try {
        Model              model(source);
        DynamicPrintConfig config = config_in;
        rotate_model(model, rotation);
        place(args, model, config);
        Print print;
        print.is_BBL_printer() = config.opt_string("printer_model").rfind("Bambu Lab", 0) == 0;
        for (ModelObject *object : model.objects)
            print.auto_assign_extruders(object);
        print.apply(model, config);
        Model::setBedPolygon(config, print.config());
        if (args.allow_mixed_temperatures)
            print.set_check_multi_filaments_compatibility(false);
        if (const StringObjectException err = print.validate(); ! err.string.empty())
            throw std::runtime_error("validate: " + err.string);
        print.process();
        GCodeProcessorResult result;
        const fs::path       gcode = dir / fs::unique_path("orient_%%%%%%%%.gcode");
        print.export_gcode(gcode.string(), &result, nullptr);
        fs::remove(gcode);
        out.time_s = result.print_statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)].time;
        out.hits   = result.clearance_report.ran ? result.clearance_report.hit_moves : 0;
        for (const auto &[extruder, volume] : result.print_statistics.support_volumes_per_extruder)
            out.support += volume;
        for (const PrintObject *object : print.objects())
            for (const Nonplanar::ConformalPatch &patch : object->conformal().patches)
                if (patch.kind == Nonplanar::PatchKind::Top)
                    out.footprint += area(patch.footprint) * SCALING_FACTOR * SCALING_FACTOR;
        out.ok = true;
    } catch (const std::exception &e) {
        out.error = e.what();
    }
    return out;
}

OrientSlice slice_orientation_child(const Args &args, const Vec3d &up, const fs::path &dir)
{
    const auto quote = [](const std::string &v) {
#ifdef _WIN32
        return "\"" + v + "\"";
#else
        std::string q = "'";
        for (char c : v)
            q += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return q + "'";
#endif
    };
    const fs::path out = dir / fs::unique_path("orient_%%%%%%%%.txt");
    std::string    cmd;
    for (size_t i = 0; i < args.argv.size(); ++i)
        if (args.argv[i] == "--json" || args.argv[i] == "--project-out" || args.argv[i] == "--head-out")
            ++i;
        else
            cmd += quote(args.argv[i]) + " ";
    std::ostringstream u;
    u.precision(17);
    u << up.x() << "," << up.y() << "," << up.z();
    cmd += "--confirm-up " + quote(u.str()) + " > " + quote(out.string());
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    const int   rc = std::system(cmd.c_str());
    OrientSlice sl;
    boost::nowide::ifstream in(out.string());
    for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        std::string        key;
        ls >> key;
        if (key == "orient_slice")
            ls >> sl.ok >> sl.time_s >> sl.footprint >> sl.support >> sl.hits;
        else if (key == "orient_slice_error")
            std::getline(ls, sl.error);
    }
    in.close();
    fs::remove(out);
    if (! sl.ok && sl.error.empty())
        sl.error = "slice exited with status " + std::to_string(rc);
    return sl;
}

void print_stock_orient_scores(const Args &args)
{
    double angle = 30;
    for (const auto &[key, value] : args.overrides)
        if (key == "support_threshold_angle") angle = std::stod(value);
    const Model model = read_input(args.model);
    for (size_t o = 0; o < model.objects.size(); ++o)
        for (const orientation::CandidateScore &c : orientation::score_candidates(model.objects[o]->mesh(), angle)) {
            Vec3d  axis;
            double turn;
            Geometry::rotation_from_two_vectors(c.up, {0, 0, 1}, axis, turn);
            const nlohmann::json line = {{"object", o}, {"name", model.objects[o]->name},
                                         {"up", {c.up.x(), c.up.y(), c.up.z()}}, {"axis", {axis.x(), axis.y(), axis.z()}},
                                         {"angle_deg", turn * 180. / PI}, {"original", c.original}, {"best", c.best},
                                         {"unprintability", c.unprintability}, {"support_area", c.support_area},
                                         {"float_area", c.float_area}, {"float_volume", c.float_volume}, {"bottom_area", c.bottom_area},
                                         {"size", {c.size.x(), c.size.y(), c.size.z()}}};
            std::cout << line.dump() << "\n";
        }
}

void print_orient_score(const char *tag, const Nonplanar::OrientScore &s)
{
    std::cout << tag << " up " << s.up.x() << "," << s.up.y() << "," << s.up.z() << " cost " << s.cost << " time_s " << s.time_s
              << " defect " << s.defect << " curved_share " << s.curved_share << " flat " << s.flat_top << " curved " << s.curved_top << " steep " << s.steep_top
              << " support_area " << s.support_area << " support_volume " << s.support_volume << " bed " << s.bed
              << " height " << s.height << " stable " << s.stable << "\n";
}

Matrix3d choose_orientation(const Args &args, const Model &model, const DynamicPrintConfig &config, const fs::path &dir)
{
    if (model.objects.size() != 1) {
        std::cout << "auto_orient skipped: " << model.objects.size() << " objects\n";
        return Matrix3d::Identity();
    }
    const Nonplanar::OrientParams params = Nonplanar::orient_params(config);
    const auto                    t0     = std::chrono::steady_clock::now();
    const TriangleMesh            mesh   = model.objects.front()->raw_mesh();
    const Nonplanar::OrientResult res    = Nonplanar::auto_orient(mesh.its, params);
    std::cout << "auto_orient_mesh_s " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << "\n";
    print_orient_score("auto_orient_original", res.original);
    for (const Nonplanar::OrientScore &s : res.ranked)
        print_orient_score("auto_orient_candidate", s);
    Nonplanar::OrientScore chosen = res.original;
    std::vector<Nonplanar::OrientScore> trial{ res.original };
    std::vector<Nonplanar::OrientScore> pool;
    if (! res.ranked.empty())
        pool.push_back(res.ranked.front());
    std::vector<Nonplanar::OrientScore> axes;
    for (const Vec3d &a : std::initializer_list<Vec3d>{ Vec3d::UnitX(), -Vec3d::UnitX(), Vec3d::UnitY(), -Vec3d::UnitY(), -Vec3d::UnitZ() })
        axes.push_back(Nonplanar::score_orientation(mesh.its, a, params));
    std::sort(axes.begin(), axes.end(), [](const auto &l, const auto &r) { return l.cost < r.cost; });
    pool.insert(pool.end(), axes.begin(), axes.end());
    pool.insert(pool.end(), res.ranked.begin(), res.ranked.end());
    for (const Nonplanar::OrientScore &s : pool)
        if (trial.size() < 6 && s.stable && (! res.original.stable || s.height <= 1.25 * res.original.height) &&
            std::none_of(trial.begin(), trial.end(), [&](const auto &t) { return t.up.dot(s.up) > std::cos(2. * M_PI / 180.); }))
            trial.push_back(s);
    if (trial.size() > 1) {
        for (Nonplanar::OrientScore &s : trial) {
            const OrientSlice sl = slice_orientation_child(args, s.up, dir);
            std::cout << "auto_orient_slice up " << s.up.x() << "," << s.up.y() << "," << s.up.z() << " ok " << sl.ok << " time_s "
                      << sl.time_s << " footprint " << sl.footprint << " support " << sl.support << " hits " << sl.hits
                      << (sl.error.empty() ? "" : " error " + sl.error) << "\n";
            if (! sl.ok || sl.hits > 0) {
                s.stable = false;
                s.cost   = std::numeric_limits<double>::max();
                continue;
            }
            s.measured        = true;
            s.time_s          = sl.time_s;
            s.printed_support = sl.support;
            s.curved_share    = s.curved_top > 0. ? std::min(1., sl.footprint / s.curved_top) : 1.;
            Nonplanar::rescore(s);
        }
        const Nonplanar::OrientScore &original = trial.front();
        for (size_t i = 1; i < trial.size(); ++i)
            if (trial[i].cost < std::numeric_limits<double>::max() && Nonplanar::replaces(trial[i], original) &&
                (chosen.up == res.original.up || trial[i].cost < chosen.cost))
                chosen = trial[i];
    }
    const Matrix3d r = Nonplanar::rotation_to_z(chosen.up);
    print_orient_score("auto_orient_chosen", chosen);
    std::cout << "auto_orient_rotated " << (chosen.up == res.original.up ? 0 : 1) << "\n"
              << "auto_orient_rotation";
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            std::cout << " " << r(i, j);
    std::cout << "\n";
    return r;
}

Geometry::Transformation plate_transformation(const nlohmann::json &value)
{
    if (value.is_object()) {
        const auto vector = [&value](const char *key, const Vec3d &fallback) {
            if (!value.contains(key)) return fallback;
            const auto &v = value.at(key);
            if (!v.is_array() || v.size() != 3) throw std::runtime_error(std::string("transform ") + key + " needs three numbers");
            return Vec3d(v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>());
        };
        Geometry::Transformation transform;
        transform.set_offset(vector("offset", Vec3d::Zero()));
        transform.set_rotation(vector("rotation", Vec3d::Zero()));
        transform.set_scaling_factor(vector("scale", Vec3d::Ones()));
        transform.set_mirror(vector("mirror", Vec3d::Ones()));
        return transform;
    }
    const bool flat = value.is_array() && value.size() == 16, rows = value.is_array() && value.size() == 4;
    if (!flat && !rows) throw std::runtime_error("a transform is a row-major 4x4 matrix or offset/rotation/scale/mirror components");
    Transform3d matrix = Transform3d::Identity();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            matrix.matrix()(r, c) = (flat ? value.at(r * 4 + c) : value.at(r).at(c)).get<double>();
    return Geometry::Transformation(matrix);
}

void add_placed_instances(ModelObject *object, const nlohmann::json &entry, const Vec3d &origin = Vec3d::Zero())
{
    const nlohmann::json places = entry.contains("instances") ? entry.at("instances") : nlohmann::json::array({ entry });
    for (const nlohmann::json &place : places) {
        ModelInstance *instance = object->add_instance();
        if (place.contains("transform")) {
            Geometry::Transformation transform = plate_transformation(place.at("transform"));
            transform.set_offset(transform.get_offset() + origin);
            if (place.contains("rot") && place.at("rot").get<double>() != Geometry::rad2deg(transform.get_rotation().z())) {
                Vec3d rotation = transform.get_rotation();
                rotation.z() = Geometry::deg2rad(place.at("rot").get<double>());
                transform.set_rotation(rotation);
            }
            instance->set_transformation(transform);
            instance->auto_drop = false;
        } else
            instance->set_rotation(Vec3d(0., 0., Geometry::deg2rad(place.value("rot", 0.))));
        instance->printable = place.value("printable", true);
        if (place.contains("identify_id")) {
            instance->loaded_id = place.at("identify_id").get<size_t>();
            instance->use_loaded_id_for_label = true;
        }
        instance->arrange_order = place.value("arrange_order", 0);
    }
    object->ensure_on_bed();
    const size_t first = object->instances.size() - places.size();
    for (size_t i = 0; i < places.size(); ++i) {
        const BoundingBoxf3 box = object->instance_bounding_box(first + i, !places[i].contains("transform"));
        object->instances[first + i]->set_offset(object->instances[first + i]->get_offset() +
                                                 Vec3d(places[i].contains("x") ? origin.x() + places[i].at("x").get<double>() - box.center().x() : 0.,
                                                       places[i].contains("y") ? origin.y() + places[i].at("y").get<double>() - box.center().y() : 0.,
                                                       places[i].contains("z") ? places[i].at("z").get<double>() - box.min.z() : 0.));
        object->instances[first + i]->auto_drop = places[i].value("auto_drop", true);
    }
}

void apply_plate_paint(ModelVolume &volume, const nlohmann::json &paint, const std::string &where)
{
    const int triangles = int(volume.mesh().its.indices.size());
    for (const auto &[kind, map] : paint.items()) {
        FacetsAnnotation &facets = kind == "support" ? volume.supported_facets :
                                   kind == "seam"    ? volume.seam_facets :
                                   kind == "fuzzy"   ? volume.fuzzy_skin_facets :
                                   kind == "nonplanar" ? volume.nonplanar_facets :
                                   kind == "color"   ? volume.mmu_segmentation_facets :
                                                       throw std::runtime_error(where + ": unknown paint " + kind);
        if (!map.is_object())
            throw std::runtime_error(where + ": paint " + kind + " is a map of triangle index to paint string");
        std::map<int, std::string> painted;
        for (const auto &[key, value] : map.items()) {
            size_t end = 0;
            const int triangle = key.empty() || !std::isdigit((unsigned char) key.front()) ? -1 : std::stoi(key, &end);
            if (end != key.size() || triangle < 0 || triangle >= triangles)
                throw std::runtime_error(where + ": paint " + kind + " triangle " + key + " is not one of the volume's " +
                                         std::to_string(triangles) + " triangles");
            const std::string code = value.is_string() ? value.get<std::string>() : std::string();
            if (code.empty() || code.find_first_not_of("0123456789ABCDEF") != std::string::npos)
                throw std::runtime_error(where + ": paint " + kind + " triangle " + key + " is not a paint string (uppercase hex)");
            painted.emplace(triangle, code);
        }
        facets.reset();
        facets.reserve(int(painted.size()));
        for (const auto &[triangle, code] : painted) {
            facets.set_triangle_from_string(triangle, code);
            if (facets.get_triangle_as_string(triangle) != code)
                throw std::runtime_error(where + ": paint " + kind + " triangle " + std::to_string(triangle) + " has a malformed paint string");
        }
        facets.shrink_to_fit();
    }
}

struct PlateLayout
{
    int                                                     count = 0;
    std::map<int, std::pair<std::string, DynamicPrintConfig>> plates;
    std::map<int, bool>                                     plate_locked;
    std::map<ObjectID, int>                                 instance_plate;
    std::optional<DynamicPrintConfig>                       settings;
    bool                                                   project_settings = false;
    bool                                                   implicit_plate_settings = true;
};

struct PlateFiles
{
    fs::path                                      dir;
    bool                                          cache = true;
    std::map<std::string, std::unique_ptr<Model>> loaded;

    const Model &operator()(const std::string &name)
    {
        fs::path file = name;
        if (file.is_relative())
            file = dir / file;
        auto it = cache ? loaded.find(file.string()) : loaded.end();
        if (it == loaded.end())
            it = loaded.insert_or_assign(file.string(), cache ? std::make_unique<Model>(read_input(file.string())) :
                                                                std::unique_ptr<Model>(new Model(read_input(file.string())))).first;
        if (it->second->objects.empty())
            throw std::runtime_error("no objects in " + file.string());
        return *it->second;
    }
};

void apply_plate_settings(ModelConfig &config, const nlohmann::json &settings)
{
    ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Disable);
    for (const auto &[key, value] : settings.items())
        config.set_deserialize(key, value.is_string() ? value.get<std::string>() : value.dump(), ctx);
}

TriangleMesh read_plate_mesh(const std::string &path)
{
    mz_zip_archive archive{};
    if (!open_zip_reader(&archive, path)) throw std::runtime_error("cannot read volume mesh " + path);
    ScopeGuard close([&] { close_zip_reader(&archive); });
    size_t size = 0;
    void *data = mz_zip_reader_extract_file_to_heap(&archive, "3D/3dmodel.model", &size, 0);
    if (!data) throw std::runtime_error("volume mesh has no 3D/3dmodel.model: " + path);
    ScopeGuard release([&] { mz_free(data); });
    std::istringstream input(std::string(static_cast<const char *>(data), size));
    boost::property_tree::ptree tree;
    boost::property_tree::read_xml(input, tree);
    for (const auto &[tag, object] : tree.get_child("model.resources")) {
        const auto mesh = object.get_child_optional("mesh");
        if (tag != "object" || !mesh) continue;
        indexed_triangle_set its;
        for (const auto &[tag, vertex] : mesh->get_child("vertices"))
            if (tag == "vertex") {
                Vec3f v(vertex.get<float>("<xmlattr>.x"), vertex.get<float>("<xmlattr>.y"), vertex.get<float>("<xmlattr>.z"));
                if (!v.allFinite()) throw std::runtime_error("non-finite volume mesh vertex: " + path);
                its.vertices.push_back(v);
            }
        for (const auto &[tag, triangle] : mesh->get_child("triangles"))
            if (tag == "triangle") {
                Vec3i32 face(triangle.get<int>("<xmlattr>.v1"), triangle.get<int>("<xmlattr>.v2"), triangle.get<int>("<xmlattr>.v3"));
                for (int index : face)
                    if (index < 0 || size_t(index) >= its.vertices.size()) throw std::runtime_error("invalid volume mesh vertex index: " + path);
                its.indices.push_back(face);
                FaceProperty property;
                property.from_string(triangle.get<std::string>("<xmlattr>.face_property", ""));
                its.properties.push_back(property);
            }
        return TriangleMesh(std::move(its));
    }
    throw std::runtime_error("volume 3MF has no mesh: " + path);
}

void write_plate_mesh(const std::string &path, const TriangleMesh &mesh)
{
    Model model;
    model.get_backup_path();
    ModelObject *object = model.add_object();
    object->add_volume(TriangleMesh(mesh), ModelVolumeType::MODEL_PART, false);
    object->add_instance();
    StoreParams params;
    params.path = path;
    params.model = &model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    params.config = &config;
    params.strategy = SaveStrategy::Silence | SaveStrategy::MinimalPublished;
    if (!store_bbs_3mf(params)) throw std::runtime_error("cannot write volume mesh " + path);
}

class MeshDiagnosticOutput
{
public:
    MeshDiagnosticOutput()
    {
        std::cout.flush();
        std::fflush(stdout);
        m_saved = duplicate(output_fd());
        if (m_saved < 0) throw std::runtime_error("Unable to save stdout for mesh diagnostics");
        if (redirect(error_fd(), output_fd()) < 0) {
            close_fd(m_saved);
            throw std::runtime_error("Unable to redirect mesh diagnostics to stderr");
        }
    }
    ~MeshDiagnosticOutput()
    {
        std::cout.flush();
        std::fflush(stdout);
        redirect(m_saved, output_fd());
        close_fd(m_saved);
    }
    MeshDiagnosticOutput(const MeshDiagnosticOutput &) = delete;
    MeshDiagnosticOutput &operator=(const MeshDiagnosticOutput &) = delete;
private:
#ifdef _WIN32
    static int output_fd() { return _fileno(stdout); }
    static int error_fd() { return _fileno(stderr); }
    static int duplicate(int fd) { return _dup(fd); }
    static int redirect(int from, int to) { return _dup2(from, to); }
    static void close_fd(int fd) { _close(fd); }
#else
    static int output_fd() { return fileno(stdout); }
    static int error_fd() { return fileno(stderr); }
    static int duplicate(int fd) { return dup(fd); }
    static int redirect(int from, int to) { return dup2(from, to); }
    static void close_fd(int fd) { close(fd); }
#endif
    int m_saved = -1;
};

TriangleMesh read_operation_mesh(const std::string &path)
{
    Model source = read_input(path);
    if (source.objects.size() != 1 || source.objects.front()->volumes.size() != 1 || source.objects.front()->instances.size() > 1)
        throw std::runtime_error("mesh operation needs one object with one volume and at most one instance");
    const ModelObject *object = source.objects.front();
    const ModelVolume *volume = object->volumes.front();
    TriangleMesh mesh = volume->mesh();
    mesh.transform(object->instances.empty() ? volume->get_matrix() : object->instances.front()->get_matrix() * volume->get_matrix(), true);
    if (mesh.empty()) throw std::runtime_error("mesh operation needs a nonempty mesh");
    return mesh;
}

void mesh_only(const Args &args)
{
    std::unique_ptr<MeshDiagnosticOutput> diagnostics;
    if (args.mesh_operation != "repair" && args.mesh_operation != "simplify")
        diagnostics = std::make_unique<MeshDiagnosticOutput>();
    const std::string extension = fs::path(args.mesh_output).extension().string();
    if (!boost::algorithm::iequals(extension, ".stl") && !boost::algorithm::iequals(extension, ".3mf"))
        throw std::runtime_error("mesh output must be .stl or .3mf");
    TriangleMesh mesh = read_operation_mesh(args.mesh_input);
    const size_t before = mesh.its.indices.size();
    const RepairedMeshErrors imported = mesh.stats().repaired_errors;
    nlohmann::json result = {{"operation", args.mesh_operation}, {"input", args.mesh_input}, {"output", args.mesh_output},
                             {"triangles_before", before}};
    if (args.mesh_operation == "repair") {
        const size_t open_before = its_num_open_edges(mesh.its);
        if (open_before != 0) {
            std::string error;
            if (!MeshBoolean::cgal::repair(mesh, nullptr, &error))
                throw std::runtime_error(error.empty() ? "mesh repair failed" : error);
        }
        const size_t open_after = its_num_open_edges(mesh.its);
        result["repair_counts"] = {{"degenerate_facets", imported.degenerate_facets}, {"edges_fixed", imported.edges_fixed},
            {"facets_removed", imported.facets_removed}, {"facets_reversed", imported.facets_reversed},
            {"backwards_edges", imported.backwards_edges}, {"duplicate_facets", imported.duplicate_facets},
            {"nonmanifold_edges", imported.nonmanifold_edges}, {"holes_filled", imported.holes_filled}, {"facets_added", imported.facets_added}};
        result["open_edges_before_cgal"] = open_before;
        result["open_edges_after"] = open_after;
        result["boundary_edges_closed"] = open_before >= open_after ? open_before - open_after : 0;
    } else if (args.mesh_operation == "simplify") {
        const uint32_t target = args.simplify_triangles ? uint32_t(std::min<size_t>(*args.simplify_triangles, before)) :
            args.simplify_ratio ? uint32_t(std::max(1., std::floor(double(before) * *args.simplify_ratio))) : 0;
        indexed_triangle_set its = std::move(mesh.its);
        float max_error = args.simplify_max_error.value_or(std::numeric_limits<float>::max());
        its_quadric_edge_collapse(its, target, &max_error);
        mesh = TriangleMesh(std::move(its));
        result["triangles_requested"] = target;
        if (args.simplify_max_error) {
            result["max_error"] = *args.simplify_max_error;
            result["last_collapse_error"] = max_error;
        }
    } else {
        auto solid_error = [](const TriangleMesh &candidate) -> std::string {
            if (its_num_open_edges(candidate.its) != 0) return "mesh has open or nonmanifold edges";
            try {
                auto solid = MeshBoolean::cgal::triangle_mesh_to_cgal(candidate);
                if (MeshBoolean::cgal::does_self_intersect(*solid)) return "mesh self-intersects";
                if (!MeshBoolean::cgal::does_bound_a_volume(*solid)) return "mesh does not bound a volume";
            } catch (const std::exception &e) { return e.what(); }
            return {};
        };
        result["inputs"] = nlohmann::json::array({args.mesh_input});
        result["method"] = "cgal";
        result["best_effort"] = false;
        result["preflight_reasons"] = nlohmann::json::array();
        const std::string mcut_operation = args.mesh_operation == "union" ? "UNION" :
            args.mesh_operation == "difference" ? "A_NOT_B" : "INTERSECTION";
        for (const std::string &path : args.mesh_operands) {
            TriangleMesh operand = read_operation_mesh(path);
            const std::string source_error = solid_error(mesh), operand_error = solid_error(operand);
            TriangleMesh output;
            bool fallback = !source_error.empty() || !operand_error.empty();
            if (!source_error.empty()) result["preflight_reasons"].push_back({{"input", "accumulated result"}, {"reason", source_error}});
            if (!operand_error.empty()) result["preflight_reasons"].push_back({{"input", path}, {"reason", operand_error}});
            if (!fallback) {
                try {
                    output = mesh;
                    if (args.mesh_operation == "union") MeshBoolean::cgal::plus(output, operand);
                    else if (args.mesh_operation == "difference") MeshBoolean::cgal::minus(output, operand);
                    else MeshBoolean::cgal::intersect(output, operand);
                } catch (const std::exception &e) {
                    result["preflight_reasons"].push_back({{"input", path}, {"reason", e.what()}});
                    fallback = true;
                }
            }
            if (fallback) {
                std::vector<TriangleMesh> meshes;
                MeshBoolean::mcut::make_boolean(mesh, operand, meshes, mcut_operation);
                output = TriangleMesh();
                for (const TriangleMesh &part : meshes) output.merge(part);
                result["method"] = "mcut";
                result["best_effort"] = true;
            }
            if (output.empty()) throw std::runtime_error("Unable to perform boolean operation on selected parts: empty result");
            mesh = std::move(output);
            result["inputs"].push_back(path);
        }
        std::map<std::pair<int, int>, size_t> edges;
        for (const Vec3i32 &face : mesh.its.indices)
            for (int i = 0; i < 3; ++i) {
                const int a = face[i], b = face[(i + 1) % 3];
                ++edges[{std::min(a, b), std::max(a, b)}];
            }
        result["volume_mm3"] = its_mass_properties(mesh.its).volume;
        result["closed"] = std::all_of(edges.begin(), edges.end(), [](const auto &edge) { return edge.second == 2; });
        result["manifold"] = std::all_of(edges.begin(), edges.end(), [](const auto &edge) { return edge.second <= 2; });
    }
    if (mesh.empty()) throw std::runtime_error("mesh operation produced an empty mesh");
    result["triangles_after"] = mesh.its.indices.size();
    if (boost::algorithm::iequals(extension, ".3mf")) write_plate_mesh(args.mesh_output, mesh);
    else if (!store_stl(args.mesh_output.c_str(), &mesh, true)) throw std::runtime_error("cannot write mesh " + args.mesh_output);
    diagnostics.reset();
    std::cout << result.dump() << "\n";
}

nlohmann::json emboss_fonts(const Args &args)
{
    nlohmann::json fonts = nlohmann::json::array();
    const fs::path directory = fs::path(args.profiles).parent_path() / "fonts";
    if (!fs::is_directory(directory)) throw std::runtime_error("emboss fonts directory is missing: " + directory.string());
    std::vector<fs::path> files;
    for (const fs::directory_entry &entry : fs::directory_iterator(directory))
        if (fs::is_regular_file(entry.path()) && boost::algorithm::iequals(entry.path().extension().string(), ".ttf")) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const fs::path &file : files) {
        std::string name = file.stem().string();
        std::replace(name.begin(), name.end(), '_', ' ');
        std::replace(name.begin(), name.end(), '-', ' ');
        fonts.push_back({{"name", name}, {"id", file.stem().string()}, {"file", file.string()}});
    }
    return fonts;
}

bool emboss_font_tables_valid(const std::vector<unsigned char> &data)
{
    auto u16 = [&](size_t at) { return (unsigned(data[at]) << 8) | unsigned(data[at + 1]); };
    auto u32 = [&](size_t at) { return (uint32_t(u16(at)) << 16) | u16(at + 2); };
    auto sfnt = [&](size_t offset) {
        if (offset > data.size() || data.size() - offset < 12) return false;
        const uint32_t tag = u32(offset);
        if (tag != 0x00010000 && tag != 0x4f54544f && tag != 0x74727565 && tag != 0x74797031) return false;
        const size_t count = u16(offset + 4);
        if (count == 0 || count > (data.size() - offset - 12) / 16) return false;
        for (size_t i = 0; i < count; ++i) {
            const size_t table = offset + 12 + i * 16;
            const size_t start = u32(table + 8), size = u32(table + 12);
            if (start > data.size() || size > data.size() - start) return false;
        }
        return true;
    };
    if (data.size() < 12) return false;
    if (u32(0) != 0x74746366) return sfnt(0);
    const size_t count = u32(8);
    if (count == 0 || count > (data.size() - 12) / 4) return false;
    for (size_t i = 0; i < count; ++i)
        if (!sfnt(u32(12 + 4 * i))) return false;
    return true;
}

TriangleMesh text_on_surface(const Args &args, const ExPolygons &polygons, double scale)
{
    const Vec3d normal = args.text_normal->normalized();
    Vec3d up = args.text_up.value_or(Vec3d::UnitZ());
    up -= normal * up.dot(normal);
    if (!args.text_up && up.norm() < 1e-9) {
        up = Vec3d::UnitY();
        up -= normal * up.dot(normal);
    }
    if (up.norm() < 1e-9) throw std::runtime_error("--up must not be parallel to --normal");
    up.normalize();
    Transform3d frame = Transform3d::Identity();
    frame.linear().col(0) = up.cross(normal);
    frame.linear().col(1) = up;
    frame.linear().col(2) = normal;
    frame.translation() = *args.text_at;
    frame.rotate(Eigen::AngleAxisd(args.text_angle.value_or(0.) * M_PI / 180., Vec3d::UnitZ()));
    TriangleMesh target = read_operation_mesh(args.text_surface);
    target.transform(frame.inverse());
    const BoundingBoxf3 box = target.bounding_box();
    constexpr double margin = 0.3;
    const double min_z = std::min(0., box.min.z()) - margin;
    const double max_z = std::max(0., box.max.z()) + margin;
    const double span = max_z - min_z;
    Transform3d projection_frame = Transform3d::Identity();
    const BoundingBox outline_box = get_extents(polygons);
    const Vec2d center = (outline_box.min.cast<double>() + outline_box.max.cast<double>()) * (scale * 0.5);
    projection_frame.translate(Vec3d(-center.x(), -center.y(), min_z));
    projection_frame.scale(scale);
    Emboss::OrthoProject projection(projection_frame, Vec3d(0., 0., span));
    SurfaceCut cut = cut_surface(polygons, {target.its}, projection, float(-min_z / span));
    if (cut.empty()) throw std::runtime_error("text projection found no target surface");
    constexpr float safe_surface_offset = 0.015f;
    double back_from_front = -*args.text_depth;
    if (args.text_surface_outside) {
        Transform3d front = Transform3d::Identity();
        front.translate(Vec3d(0., 0., *args.text_depth));
        its_transform(cut, front);
        back_from_front -= safe_surface_offset;
    }
    Emboss::OrthoProject3d depth(Vec3d(0., 0., back_from_front));
    TriangleMesh mesh(cut2model(cut, depth));
    if (mesh.empty()) throw std::runtime_error("surface text produced an empty mesh");
    mesh.transform(frame);
    return mesh;
}

void text_mesh(const Args &args)
{
    auto diagnostics = std::make_unique<MeshDiagnosticOutput>();
    const std::string extension = fs::path(args.mesh_output).extension().string();
    if (!boost::algorithm::iequals(extension, ".stl") && !boost::algorithm::iequals(extension, ".3mf"))
        throw std::runtime_error("mesh output must be .stl or .3mf");
    const fs::path fonts = fs::path(args.profiles).parent_path() / "fonts";
    fs::path font = args.text_font.empty() ? fonts / "HarmonyOS_Sans_SC_Regular.ttf" : fs::path(args.text_font);
    if (!fs::exists(font) && font.parent_path().empty()) font = fonts / font;
    if (!args.text_font_name.empty()) {
        bool found = false;
        for (const auto &available : emboss_fonts(args))
            if (boost::algorithm::iequals(args.text_font_name, available.at("name").get<std::string>()) ||
                boost::algorithm::iequals(args.text_font_name, available.at("id").get<std::string>())) {
                font = available.at("file").get<std::string>();
                found = true;
                break;
            }
        if (!found) throw std::runtime_error("unknown bundled emboss font " + args.text_font_name);
    }
    bool synthetic_bold = args.text_bold && !boost::algorithm::iends_with(font.stem().string(), "bold");
    if (args.text_bold) {
        const std::string filename = font.filename().string();
        std::string bold_name = filename;
        for (const std::string &regular : {std::string("_Regular"), std::string("-Regular")}) {
            const size_t at = bold_name.find(regular);
            if (at != std::string::npos) bold_name.replace(at, regular.size(), regular.front() + std::string("Bold"));
        }
        if (filename == "Sarabun-Medium.ttf") bold_name = "Sarabun-SemiBold.ttf";
        const fs::path bold_font = font.parent_path() / bold_name;
        if (bold_name != filename && fs::exists(bold_font)) { font = bold_font; synthetic_bold = false; }
    }
    boost::nowide::ifstream font_input(font.string(), std::ios::binary);
    if (!font_input) throw std::runtime_error("cannot read emboss font " + font.string());
    auto bytes = std::make_unique<std::vector<unsigned char>>(std::istreambuf_iterator<char>(font_input), std::istreambuf_iterator<char>());
    if (font_input.bad() || !emboss_font_tables_valid(*bytes)) throw std::runtime_error("invalid emboss font " + font.string());
    auto file = Emboss::create_font_file(std::move(bytes));
    if (!file || file->infos.empty()) throw std::runtime_error("cannot load emboss font " + font.string());
    Emboss::FontFileWithCache cache(std::move(file));
    const float size = args.text_size.value_or(10.f), depth = args.text_depth.value_or(1.f);
    const float spacing = args.text_line_spacing.value_or(1.f);
    FontProp prop(size);
    prop.char_gap = args.text_char_gap;
    if (args.text_align == "left") prop.align.first = FontProp::HorizontalAlign::left;
    else if (args.text_align == "right") prop.align.first = FontProp::HorizontalAlign::right;
    if (synthetic_bold) prop.boldness = 20.f;
    if (args.text_italic && !Emboss::is_italic(*cache.font_file, 0)) prop.skew = 0.2f;
    const auto &info = Emboss::get_font_info(*cache.font_file, prop);
    const double line_height = double(info.ascent) - info.descent + info.linegap;
    const double gap = std::round(line_height * (double(spacing) - 1.));
    if (gap < std::numeric_limits<int>::min() || gap > std::numeric_limits<int>::max() ||
        line_height + gap > double(std::numeric_limits<int>::max()) / 1000.)
        throw std::runtime_error("line spacing exceeds the font coordinate range");
    prop.line_gap = int(gap);
    const double scale = Emboss::get_text_shape_scale(prop, *cache.font_file);
    if (!std::isfinite(scale) || scale <= 0.) throw std::runtime_error("invalid emboss font scale");
    HealedExPolygons shape = Emboss::text2shapes(cache, args.mesh_input.c_str(), prop);
    if (shape.expolygons.empty()) throw std::runtime_error("text has no drawable glyph outlines in this font");
    Emboss::ProjectScale projection(std::make_unique<Emboss::ProjectZ>(depth / scale), scale);
    TriangleMesh mesh(Emboss::polygons2model(shape.expolygons, projection));
    if (mesh.empty()) throw std::runtime_error("text produced an empty mesh");
    const BoundingBoxf3 uncentered = mesh.bounding_box();
    mesh.translate(-uncentered.center().x(), -uncentered.center().y(), 0.);
    const BoundingBoxf3 glyph_box = mesh.bounding_box();
    if (args.text_use_surface) mesh = text_on_surface(args, shape.expolygons, scale);
    if (boost::algorithm::iequals(extension, ".3mf")) write_plate_mesh(args.mesh_output, mesh);
    else if (!store_stl(args.mesh_output.c_str(), &mesh, true)) throw std::runtime_error("cannot write mesh " + args.mesh_output);
    const BoundingBoxf3 box = mesh.bounding_box();
    nlohmann::json result = {{"operation", "text"}, {"text", args.mesh_input}, {"font", font.string()},
        {"glyph_outline_bbox", {{glyph_box.min.x(), glyph_box.min.y()}, {glyph_box.max.x(), glyph_box.max.y()}}},
        {"use_surface", args.text_use_surface},
        {"align", args.text_align}, {"char_gap", args.text_char_gap.value_or(0)},
        {"output", args.mesh_output}, {"size_mm", size}, {"depth_mm", depth}, {"bold", args.text_bold},
        {"italic", args.text_italic}, {"line_spacing", spacing}, {"triangles_after", mesh.its.indices.size()},
        {"bbox", {{box.min.x(), box.min.y(), box.min.z()}, {box.max.x(), box.max.y(), box.max.z()}}}};
    diagnostics.reset();
    std::cout << result.dump() << "\n";
}

void svg_mesh(const Args &args)
{
    auto diagnostics = std::make_unique<MeshDiagnosticOutput>();
    const std::string extension = fs::path(args.mesh_output).extension().string();
    if (!boost::algorithm::iequals(extension, ".stl") && !boost::algorithm::iequals(extension, ".3mf"))
        throw std::runtime_error("mesh output must be .stl or .3mf");
    EmbossShape shape;
    shape.svg_file.emplace();
    shape.svg_file->path = args.mesh_input;
    if (!init_image(*shape.svg_file)) throw std::runtime_error("cannot load emboss SVG " + args.mesh_input);
    boost::property_tree::ptree document;
    std::istringstream xml(*shape.svg_file->file_data);
    boost::property_tree::read_xml(xml, document, boost::property_tree::xml_parser::no_comments);
    auto local_name = [](const std::string &name) {
        const size_t colon = name.find_last_of(':');
        return name.substr(colon == std::string::npos ? 0 : colon + 1);
    };
    if (document.size() != 1 || local_name(document.begin()->first) != "svg")
        throw std::runtime_error("input is not an SVG document");
    std::set<std::string> warnings;
    std::vector<const boost::property_tree::ptree *> nodes{&document};
    while (!nodes.empty()) {
        const auto *node = nodes.back();
        nodes.pop_back();
        for (const auto &entry : *node) {
            if (entry.first == "<xmlattr>") continue;
            const std::string name = local_name(entry.first);
            if (name == "image") warnings.insert("SVG raster images are ignored");
            else if (name == "linearGradient" || name == "radialGradient")
                warnings.insert("SVG gradient colors are ignored; outlines are retained");
            else if (name == "text") warnings.insert("SVG text is ignored; convert it to paths");
            nodes.push_back(&entry.second);
        }
    }
    NSVGLineParams params{0.01 / SCALING_FACTOR / SCALING_FACTOR};
    shape.shapes_with_ids = create_shape_with_ids(*shape.svg_file->image, params);
    ExPolygons polygons = union_with_delta(shape, Emboss::UNION_DELTA, Emboss::UNION_MAX_ITERATIN);
    if (polygons.empty()) throw std::runtime_error("SVG has no drawable outlines");
    const BoundingBox outline = get_extents(polygons);
    double scale = shape.scale;
    if (args.text_size) {
        const coord_t side = std::max(outline.size().x(), outline.size().y());
        if (side <= 0) throw std::runtime_error("SVG has no drawable extent");
        scale = *args.text_size / double(side);
    }
    const double depth = *args.text_depth;
    Emboss::ProjectScale projection(std::make_unique<Emboss::ProjectZ>(depth / scale), scale);
    TriangleMesh mesh(Emboss::polygons2model(polygons, projection));
    if (mesh.empty()) throw std::runtime_error("SVG produced an empty mesh");
    const BoundingBoxf3 flat_box = mesh.bounding_box();
    mesh.translate(-flat_box.center().x(), -flat_box.center().y(), 0.);
    const BoundingBoxf3 shape_box = mesh.bounding_box();
    if (args.text_use_surface) mesh = text_on_surface(args, polygons, scale);
    const bool closed = its_num_open_edges(mesh.its) == 0;
    if (!closed) warnings.insert("SVG produced a mesh with open edges");
    if (boost::algorithm::iequals(extension, ".3mf")) write_plate_mesh(args.mesh_output, mesh);
    else if (!store_stl(args.mesh_output.c_str(), &mesh, true)) throw std::runtime_error("cannot write mesh " + args.mesh_output);
    const BoundingBoxf3 box = mesh.bounding_box();
    nlohmann::json result = {{"operation", "svg"}, {"input", args.mesh_input}, {"output", args.mesh_output},
        {"depth_mm", depth}, {"triangles_after", mesh.its.indices.size()}, {"closed", closed}, {"warnings", warnings},
        {"use_surface", args.text_use_surface},
        {"shape_bbox", {{shape_box.min.x(), shape_box.min.y()}, {shape_box.max.x(), shape_box.max.y()}}},
        {"bbox", {{box.min.x(), box.min.y(), box.min.z()}, {box.max.x(), box.max.y(), box.max.z()}}}};
    diagnostics.reset();
    std::cout << result.dump() << "\n";
}

void export_mesh(const Args &args)
{
    auto diagnostics = std::make_unique<MeshDiagnosticOutput>();
    Model source = read_input(args.mesh_input);
    if (source.objects.empty()) throw std::runtime_error("mesh export needs nonempty geometry");
    const std::string extension = fs::path(args.mesh_output).extension().string();
    nlohmann::json result = {{"operation", args.mesh_operation}, {"input", args.mesh_input}, {"output", args.mesh_output},
        {"objects", source.objects.size()}};
    if (args.mesh_operation == "generic3mf") {
        if (!boost::algorithm::iequals(extension, ".3mf")) throw std::runtime_error("generic 3MF output must be .3mf");
        if (!store_3mf(args.mesh_output.c_str(), &source, nullptr, false)) throw std::runtime_error("cannot write generic 3MF " + args.mesh_output);
        size_t triangles = 0;
        for (const ModelObject *object : source.objects)
            for (const ModelVolume *volume : object->volumes) triangles += volume->mesh().its.indices.size();
        result["triangles_after"] = triangles;
    } else {
        if (!boost::algorithm::iequals(extension, ".drc")) throw std::runtime_error("Draco output must be .drc");
        TriangleMesh mesh;
        result["best_effort"] = false;
        result["fallback_reasons"] = nlohmann::json::array();
        for (const ModelObject *object : source.objects) {
            std::vector<csg::CSGPart> parts;
            csg::model_to_csgmesh(*object, Transform3d::Identity(), std::back_inserter(parts),
                csg::mpartsPositive | csg::mpartsNegative | csg::mpartsDoSplits);
            const auto input = range(parts);
            TriangleMesh solid;
            std::string reason;
            if (csg::is_all_positive(input)) solid = TriangleMesh(csg::csgmesh_merge_positive_parts(input));
            else {
                const auto check = csg::check_csgmesh_booleans(input);
                if (std::get<0>(check) != csg::BooleanFailReason::OK) {
                    static const std::map<csg::BooleanFailReason, std::string> reasons = {
                        {csg::BooleanFailReason::MeshEmpty, "empty mesh"},
                        {csg::BooleanFailReason::NotBoundAVolume, "mesh does not bound a volume"},
                        {csg::BooleanFailReason::SelfIntersect, "self-intersecting mesh"},
                        {csg::BooleanFailReason::NoIntersection, "meshes do not intersect"}};
                    reason = reasons.at(std::get<0>(check)) + ": " + std::get<1>(check);
                }
                else {
                    try { solid = MeshBoolean::cgal::cgal_to_triangle_mesh(*csg::perform_csgmesh_booleans(input)); }
                    catch (const std::exception &e) { reason = e.what(); }
                }
                if (solid.empty()) {
                    solid = TriangleMesh(csg::csgmesh_merge_positive_parts(input));
                    result["best_effort"] = true;
                    result["fallback_reasons"].push_back({{"object", object->name}, {"reason", reason.empty() ? "boolean produced no geometry; exporting positive parts" : reason}});
                }
            }
            for (const ModelInstance *instance : object->instances) {
                TriangleMesh placed = solid;
                placed.transform(instance->get_matrix(), true);
                mesh.merge(placed);
            }
        }
        if (mesh.empty()) throw std::runtime_error("Draco export needs nonempty part geometry");
        if (!store_drc(args.mesh_output.c_str(), &mesh, args.draco_bits.value_or(DRC_BITS_DEFAULT)))
            throw std::runtime_error("cannot write Draco mesh " + args.mesh_output);
        result["triangles_after"] = mesh.its.indices.size();
        result["bits"] = args.draco_bits.value_or(DRC_BITS_DEFAULT);
    }
    diagnostics.reset();
    std::cout << result.dump() << "\n";
}

ModelVolume *add_plate_volume(ModelObject &object, const nlohmann::json &v, PlateFiles &files)
{
    static const std::map<std::string, ModelVolumeType> types = {
        { "part", ModelVolumeType::MODEL_PART },           { "modifier", ModelVolumeType::PARAMETER_MODIFIER },
        { "negative", ModelVolumeType::NEGATIVE_VOLUME },  { "support_enforcer", ModelVolumeType::SUPPORT_ENFORCER },
        { "support_blocker", ModelVolumeType::SUPPORT_BLOCKER },
        { "precise_seam_center", ModelVolumeType::PRECISE_SEAM_CENTER }, { "precise_seam_left", ModelVolumeType::PRECISE_SEAM_LEFT },
        { "precise_seam_right", ModelVolumeType::PRECISE_SEAM_RIGHT }, { "precise_seam_enforced", ModelVolumeType::PRECISE_SEAM_ENFORCED },
        { "precise_seam_blocked", ModelVolumeType::PRECISE_SEAM_BLOCKED }, { "precise_seam_neutral", ModelVolumeType::PRECISE_SEAM_NEUTRAL } };
    const std::string requested_type = v.value("type", "part");
    const std::string type_name = requested_type == "seam" ? "precise_seam_" + v.value("seam_mode", "center") : requested_type;
    const auto type = types.find(type_name);
    if (type == types.end())
        throw std::runtime_error("unknown volume type " + (requested_type == "seam" ? type_name : v.value("type", "")));
    Geometry::Transformation transform;
    if (const nlohmann::json m = v.value("transform", nlohmann::json()); !m.is_null()) {
        transform = plate_transformation(m);
    }
    const bool native_mesh = v.value("mesh_space", "object") == "volume";
    TriangleMesh mesh;
    if (native_mesh) {
        fs::path path = v.at("file").get<std::string>();
        if (path.is_relative()) path = files.dir / path;
        if (boost::algorithm::iequals(path.extension().string(), ".3mf")) mesh = read_plate_mesh(path.string());
        else if (!mesh.ReadSTLFile(path.string().c_str(), false)) throw std::runtime_error("cannot read volume mesh " + path.string());
        if (v.contains("mesh_init_shift")) {
            const auto &s = v.at("mesh_init_shift");
            mesh.set_init_shift(Vec3d(s.at(0).get<double>(), s.at(1).get<double>(), s.at(2).get<double>()));
        }
    } else
        mesh = files(v.at("file").get<std::string>()).objects.front()->raw_mesh();
    ModelVolume *volume = object.add_volume(std::move(mesh), type->second, false);
    volume->set_transformation(transform);
    if (!native_mesh) volume->center_geometry_after_creation();
    volume->name = v.value("name", fs::path(v.at("file").get<std::string>()).stem().string());
    if (const int extruder = v.value("extruder", 0); extruder > 0)
        volume->config.set("extruder", extruder);
    if (v.contains("settings"))
        apply_plate_settings(volume->config, v.at("settings"));
    if (v.contains("paint"))
        apply_plate_paint(*volume, v.at("paint"), "plate object " + object.name + " volume " + volume->name);
    return volume;
}

ModelObject *add_plate_object(Model &model, const nlohmann::json &entry, PlateFiles &files, const DynamicPrintConfig &config, bool native_profile = false)
{
    if (!entry.contains("file") && !entry.contains("volumes"))
        throw std::runtime_error("plate object " + std::to_string(model.objects.size() + 1) + " needs a file or volumes");
    ModelObject *object = entry.contains("file") ? model.add_object(*files(entry.at("file").get<std::string>()).objects.front()) : model.add_object();
    object->name = entry.value("name", fs::path(entry.contains("file") ? entry.at("file").get<std::string>() : "object").stem().string());
    object->clear_instances();
    if (entry.contains("input_file")) object->input_file = entry.at("input_file").get<std::string>();
    if (entry.contains("object_printable")) object->printable = entry.at("object_printable").get<bool>();
    if (entry.contains("origin_translation")) {
        const auto &v = entry.at("origin_translation");
        object->origin_translation = Vec3d(v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>());
    }
    for (const auto &point : entry.value("brim_points", nlohmann::json::array()))
        object->brim_points.emplace_back(point.at(0).get<float>(), point.at(1).get<float>(), point.at(2).get<float>(), point.at(3).get<float>());
    if (entry.contains("brim_ears")) {
        if (!entry.at("brim_ears").is_array())
            throw std::runtime_error("plate object " + object->name + ": brim_ears must be an array");
        object->brim_points.clear();
        const float default_radius = std::clamp(float(config.get_abs_value("initial_layer_line_width",
            config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0)) * 8.0), 0.1f, 100.f);
        for (const auto &point : entry.at("brim_ears")) {
            const float radius = point.value("radius", point.value("head_diameter", 2.f * default_radius) * 0.5f);
            const float x = point.at("x").get<float>(), y = point.at("y").get<float>();
            const float z = point.value("z", 0.f);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(radius) || radius <= 0.f)
                throw std::runtime_error("plate object " + object->name + ": brim_ears need finite coordinates and a positive radius");
            object->brim_points.emplace_back(x, y, z, radius);
        }
    }
    if (const int extruder = entry.value("extruder", 0); extruder > 0)
        object->config.set("extruder", extruder);
    if (entry.contains("settings"))
        apply_plate_settings(object->config, entry.at("settings"));
    for (const nlohmann::json &r : entry.value("ranges", nlohmann::json::array())) {
        ModelConfig &config = object->layer_config_ranges[{ r.at("z")[0].get<double>(), r.at("z")[1].get<double>() }];
        config.set("layer_height", r.value("layer_height", 0.2));
        apply_plate_settings(config, r.value("settings", nlohmann::json::object()));
    }
    if (const nlohmann::json paint = entry.value("paint", nlohmann::json()); !paint.is_null()) {
        const std::string where = "plate object " + object->name;
        const size_t parts = object->volumes.size();
        if (!entry.contains("file") || (paint.is_object() && parts != 1) || (paint.is_array() && paint.size() != parts) ||
            !(paint.is_object() || paint.is_array()))
            throw std::runtime_error(where + ": paint needs a file and is a map for a file of one part, else a list of one map "
                                     "per part (this file has " + std::to_string(parts) + ")");
        for (size_t i = 0; i < parts; ++i)
            if (const nlohmann::json &p = paint.is_array() ? paint.at(i) : paint; !p.is_null())
                apply_plate_paint(*object->volumes[i], p, where + " part " + std::to_string(i + 1));
    }
    for (const nlohmann::json &v : entry.value("volumes", nlohmann::json::array()))
        add_plate_volume(*object, v, files);
    if (entry.contains("volumes")) {
        if (std::none_of(object->volumes.begin(), object->volumes.end(), [](const ModelVolume *v) { return v->is_model_part(); }))
            throw std::runtime_error("plate object " + object->name + " has no part volume");
        if (!entry.value("preserve_volume_order", false)) object->sort_volumes(true);
        object->invalidate_bounding_box();
    }
    if (const nlohmann::json lhp = entry.value("layer_height_profile", nlohmann::json()); !lhp.is_null()) {
        const std::string where = "plate object " + object->name + ": layer_height_profile";
        std::vector<coordf_t> profile;
        for (const nlohmann::json &v : lhp.is_array() ? lhp : throw std::runtime_error(where + " is a list of z, h pairs")) {
            if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number())
                profile.insert(profile.end(), { v[0].get<double>(), v[1].get<double>() });
            else if (v.is_number())
                profile.push_back(v.get<double>());
            else
                throw std::runtime_error(where + " is a list of z, h pairs");
        }
        if (!native_profile && (profile.size() < 4 || profile.size() % 2 != 0))
            throw std::runtime_error(where + " needs at least two z, h pairs");
        for (size_t i = 0; !native_profile && i < profile.size(); i += 2) {
            if (!std::isfinite(profile[i]) || !std::isfinite(profile[i + 1]) || profile[i + 1] <= 0.)
                throw std::runtime_error(where + " has a layer height <= 0 at z " + std::to_string(profile[i]));
            if (i > 0 && profile[i] <= profile[i - 2])
                throw std::runtime_error(where + " does not increase z at " + std::to_string(profile[i]));
        }
        object->layer_height_profile.set(profile);
        if (!native_profile) plate_layer_profiles.push_back(object->id());
    }
    return object;
}

Model load_plate(const std::string &path, PlateLayout &layout, const DynamicPrintConfig &config, bool allow_empty = false)
{
    boost::nowide::ifstream in(path);
    const nlohmann::json    j = nlohmann::json::parse(in);
    Model                   model;
    PlateFiles              files{ fs::path(path).parent_path(), true, {} };
    ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Disable);
    if (j.contains("settings")) {
        layout.settings.emplace();
        for (const auto &[key, value] : j.at("settings").items()) {
            ConfigOption *option = layout.settings->option(key, true);
            if (!option || !value.is_string() || !option->deserialize(value.get<std::string>()))
                throw std::runtime_error("invalid native plate setting: " + key);
        }
        layout.project_settings = j.value("project_settings", true);
        layout.implicit_plate_settings = j.value("implicit_plate_settings", true);
    }
    for (const nlohmann::json &entry : j.at("objects")) {
        ModelObject *object = add_plate_object(model, entry, files, layout.settings ? *layout.settings : config, layout.settings.has_value());
        const size_t first = object->instances.size();
        add_placed_instances(object, entry);
        const nlohmann::json places = entry.contains("instances") ? entry.at("instances") : nlohmann::json::array({ entry });
        for (size_t i = 0; i < places.size(); ++i) {
            const nlohmann::json plate = places[i].contains("plate") ? places[i].at("plate") : entry.value("plate", nlohmann::json());
            if (plate.is_null())
                continue;
            if (!plate.is_number_integer() || plate.get<int>() < 1)
                throw std::runtime_error("plate object " + object->name + ": plate is a 1-based plate number");
            layout.instance_plate[object->instances[first + i]->id()] = plate.get<int>();
            layout.count = std::max(layout.count, plate.get<int>());
        }
    }
    if (model.objects.empty() && !allow_empty)
        throw std::runtime_error("no objects in " + path);
    for (const nlohmann::json &plate : j.value("plates", nlohmann::json::array())) {
        const nlohmann::json index = plate.value("index", nlohmann::json());
        if (!index.is_number_integer() || index.get<int>() < 1)
            throw std::runtime_error("plates: each plate needs a 1-based index");
        auto &[name, config] = layout.plates[index.get<int>()];
        name = plate.value("name", std::string());
        layout.plate_locked[index.get<int>()] = plate.value("locked", false);
        const nlohmann::json settings = plate.value("settings", nlohmann::json::object());
        for (const auto &[key, value] : settings.items())
            config.set_deserialize(key, value.is_string() ? value.get<std::string>() : value.dump(), ctx);
        if (plate.contains("custom_gcode"))
            model.plates_custom_gcodes[index.get<int>() - 1].from_json(plate.at("custom_gcode"));
        layout.count = std::max(layout.count, index.get<int>());
    }
    if (layout.settings) {
        std::set<size_t> labels;
        size_t next_label = 0;
        for (const ModelObject *object : model.objects)
            for (const ModelInstance *instance : object->instances)
                next_label = std::max(next_label, std::max(instance->id().id, instance->loaded_id));
        for (ModelObject *object : model.objects)
            for (ModelInstance *instance : object->instances)
                if (instance->loaded_id > 0 && !labels.insert(instance->loaded_id).second) {
                    instance->loaded_id = ++next_label;
                    labels.insert(instance->loaded_id);
                }
        for (ModelObject *object : model.objects)
            for (ModelInstance *instance : object->instances)
                if (instance->loaded_id == 0 && !labels.insert(instance->id().id).second) {
                    instance->loaded_id = ++next_label;
                    instance->use_loaded_id_for_label = true;
                    labels.insert(instance->loaded_id);
                }
    }
    if (layout.count > 0)
        for (const ModelObject *object : model.objects)
            for (const ModelInstance *instance : object->instances)
                layout.instance_plate.emplace(instance->id(), 1);
    return model;
}

Model load_project(const std::string &path)
{
    boost::nowide::ifstream in(path);
    const nlohmann::json    j   = nlohmann::json::parse(in);
    const fs::path          dir = fs::path(path).parent_path();
    auto resolve_path = [&dir](const std::string &file) { return fs::path(file).is_relative() ? (dir / file).string() : file; };
    auto read_model   = [](const std::string &file) {
        Model m = read_input(file);
        if (m.objects.empty() || m.objects.front()->volumes.empty())
            throw std::runtime_error("no objects in " + file);
        return m;
    };
    ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Disable);
    auto apply_settings = [&ctx](ModelConfig &config, const nlohmann::json &settings) {
        for (const auto &[key, value] : settings.items())
            config.set_deserialize(key, value.is_string() ? value.get<std::string>() : value.dump(), ctx);
    };

    Model        model  = read_model(resolve_path(j.at("object").get<std::string>()));
    ModelObject *object = model.objects.front();
    object->name        = j.value("name", fs::path(j.at("object").get<std::string>()).stem().string());
    ModelVolume *main   = object->volumes.front();
    if (j.contains("settings"))
        apply_settings(object->config, j.at("settings"));
    for (const nlohmann::json &v : j.value("volumes", nlohmann::json::array())) {
        static const std::map<std::string, ModelVolumeType> types = {
            { "part", ModelVolumeType::MODEL_PART },           { "modifier", ModelVolumeType::PARAMETER_MODIFIER },
            { "negative", ModelVolumeType::NEGATIVE_VOLUME },  { "support_enforcer", ModelVolumeType::SUPPORT_ENFORCER },
            { "support_blocker", ModelVolumeType::SUPPORT_BLOCKER } };
        const auto type = types.find(v.value("type", "modifier"));
        if (type == types.end())
            throw std::runtime_error("unknown volume type " + v.value("type", ""));
        Model        src    = read_model(resolve_path(v.at("file").get<std::string>()));
        TriangleMesh mesh   = src.objects.front()->volumes.front()->mesh();
        mesh.transform(src.objects.front()->volumes.front()->get_matrix());
        if (v.contains("offset"))
            mesh.translate(v["offset"][0].get<float>(), v["offset"][1].get<float>(), v["offset"][2].get<float>());
        ModelVolume *volume = object->add_volume(std::move(mesh), type->second, false);
        volume->set_transformation(Geometry::Transformation(main->get_matrix() * Geometry::translation_transform(-main->mesh().get_init_shift())));
        volume->center_geometry_after_creation();
        volume->name = fs::path(v.at("file").get<std::string>()).stem().string();
        if (v.contains("settings"))
            apply_settings(volume->config, v.at("settings"));
    }
    for (const nlohmann::json &r : j.value("ranges", nlohmann::json::array())) {
        ModelConfig &config = object->layer_config_ranges[{ r.at("z")[0].get<double>(), r.at("z")[1].get<double>() }];
        config.set("layer_height", r.value("layer_height", 0.2));
        apply_settings(config, r.value("settings", nlohmann::json::object()));
    }
    const nlohmann::json paint = j.value("paint", nlohmann::json::object());
    for (const auto &[kind, file] : paint.items()) {
        boost::nowide::ifstream pin(resolve_path(file.get<std::string>()));
        std::vector<int>        ids;
        for (int id; pin >> id;)
            ids.push_back(id);
        if (ids.size() != main->mesh().its.indices.size())
            throw std::runtime_error("paint " + kind + " has " + std::to_string(ids.size()) + " entries, the mesh " +
                                     std::to_string(main->mesh().its.indices.size()) + " triangles");
        TriangleSelector selector(main->mesh());
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] > 0)
                selector.set_facet(int(i), EnforcerBlockerType(ids[i]));
        if (kind == "seam") main->seam_facets.set(selector);
        else if (kind == "support") main->supported_facets.set(selector);
        else if (kind == "fuzzy") main->fuzzy_skin_facets.set(selector);
        else if (kind == "nonplanar") main->nonplanar_facets.set(selector);
        else if (kind == "color") main->mmu_segmentation_facets.set(selector);
        else throw std::runtime_error("unknown paint " + kind);
    }
    return model;
}

DynamicPrintConfig project_config(PresetBundle &bundle, const Args &a, const DynamicPrintConfig &config)
{
    DynamicPrintConfig out = config;
    const size_t n = config.option<ConfigOptionStrings>("filament_settings_id")->size();
    std::vector<std::string> lists;
    if (const auto *saved = config.option<ConfigOptionStrings>("different_settings_to_system"))
        lists = saved->values;
    lists.resize(n + 2);
    auto diff_slot = [&](size_t list, const char *kind, Preset::Type type, const std::string &name, const std::string &saved_name,
                   const std::vector<std::string> &keys, size_t slot) {
        const DynamicPrintConfig system = resolve(bundle, a, kind, type, name);
        const std::string system_name = type == Preset::TYPE_PRINTER ? system.opt_string("printer_settings_id") :
                                        type == Preset::TYPE_PRINT   ? system.opt_string("print_settings_id") :
                                                                       system.option<ConfigOptionStrings>("filament_settings_id")->values.front();
        if (system_name != saved_name)
            return;
        std::vector<std::string> changed;
        for (const std::string &key : keys) {
            const ConfigOption *base = system.option(key), *value = config.option(key);
            if (base == nullptr || value == nullptr || key == "inherits" || boost::algorithm::ends_with(key, "_settings_id") ||
                boost::algorithm::starts_with(key, "compatible_"))
                continue;
            const auto *base_vector = dynamic_cast<const ConfigOptionVectorBase *>(base);
            const auto *value_vector = dynamic_cast<const ConfigOptionVectorBase *>(value);
            bool differs = base->serialize() != value->serialize();
            if (type == Preset::TYPE_FILAMENT && n > 1 && base_vector && value_vector && value_vector->size() == n * base_vector->size()) {
                const std::vector<std::string> b = base_vector->vserialize(), v = value_vector->vserialize();
                differs = !std::equal(b.begin(), b.end(), v.begin() + slot * b.size());
            }
            if (differs)
                changed.push_back(key);
        }
        lists[list] = escape_strings_cstyle(changed);
    };
    diff_slot(0, "process", Preset::TYPE_PRINT, a.process, config.opt_string("print_settings_id"), Preset::print_options(), 0);
    for (size_t i = 0; i < std::min(n, a.filaments.size()); ++i)
        diff_slot(i + 1, "filament", Preset::TYPE_FILAMENT, a.filaments[i],
            config.option<ConfigOptionStrings>("filament_settings_id")->values[i], Preset::filament_options(), i);
    diff_slot(n + 1, "machine", Preset::TYPE_PRINTER, a.printer, config.opt_string("printer_settings_id"), Preset::printer_options(), 0);
    out.set_key_value("different_settings_to_system", new ConfigOptionStrings(lists));
    return out;
}

struct SavedInstance
{
    int         plate = 0;
    std::string plate_name;
    int         object_id = 0, instance_id = 0, identify_id = 0;
};

std::vector<SavedInstance> read_saved_instances(const std::string &path)
{
    std::vector<SavedInstance> out;
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (! open_zip_reader(&zip, path))
        return out;
    size_t size = 0;
    void  *data = mz_zip_reader_extract_file_to_heap(&zip, "Metadata/model_settings.config", &size, 0);
    close_zip_reader(&zip);
    if (data == nullptr)
        return out;
    std::istringstream in(std::string(static_cast<const char *>(data), size));
    mz_free(data);
    namespace pt = boost::property_tree;
    pt::ptree tree;
    pt::read_xml(in, tree);
    const auto root = tree.get_child_optional("config");
    if (!root) return out;
    for (const auto &[tag, plate] : *root) {
        if (tag != "plate")
            continue;
        SavedInstance on_plate;
        for (const auto &[key, meta] : plate)
            if (key == "metadata") {
                const std::string name = meta.get("<xmlattr>.key", std::string());
                if (name == "plater_id") on_plate.plate = meta.get("<xmlattr>.value", 0);
                else if (name == "plater_name") on_plate.plate_name = meta.get("<xmlattr>.value", std::string());
            }
        for (const auto &[item, instance] : plate) {
            if (item != "model_instance")
                continue;
            SavedInstance saved = on_plate;
            for (const auto &[key, meta] : instance)
                if (key == "metadata") {
                    const std::string name  = meta.get("<xmlattr>.key", std::string());
                    const int         value = meta.get("<xmlattr>.value", -1);
                    if (name == "object_id") saved.object_id = value;
                    else if (name == "instance_id") saved.instance_id = value;
                    else if (name == "identify_id") saved.identify_id = value;
                }
            out.push_back(saved);
        }
    }
    return out;
}

void restore_loaded_ids(const std::vector<SavedInstance> &instances, Model &model)
{
    std::map<int, std::vector<std::pair<int, int>>> saved;
    for (const SavedInstance &instance : instances)
        if (instance.instance_id >= 0 && instance.identify_id > 0)
            saved[instance.object_id].emplace_back(instance.instance_id, instance.identify_id);
    for (const auto &[id, instances] : saved)
        for (ModelObject *object : model.objects) {
            auto kept = [object](const std::pair<int, int> &i) {
                return size_t(i.first) < object->instances.size() && object->instances[i.first]->loaded_id == size_t(i.second);
            };
            if (std::none_of(instances.begin(), instances.end(), kept))
                continue;
            for (const auto &[index, identify_id] : instances)
                if (size_t(index) < object->instances.size())
                    object->instances[index]->loaded_id = identify_id;
            break;
        }
}

#include "nonplanar_slice_tool_save.ipp"

void save_project(PresetBundle &bundle, const Args &args, Model &model, const DynamicPrintConfig &config, bool loaded_ids,
                  const PlateDataPtrs &plates, const std::string &cut_information = {}, const DynamicPrintConfig *slice_config = nullptr)
{
    model.get_backup_path();
    StoreParams params;
    params.path            = args.save_3mf;
    params.model           = &model;
    params.plate_data_list = plates;
    DynamicPrintConfig saved = project_config(bundle, args, config);
    params.config          = &saved;
    params.strategy        = SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::ShareMesh |
                             (loaded_ids ? SaveStrategy::UseLoadedId : SaveStrategy::Default);
    std::vector<std::pair<ModelObject *, BrimPoints>> original_points;
    ScopeGuard restore_points([&] {
        for (auto &[object, points] : original_points)
            object->brim_points = std::move(points);
    });
    for (ModelObject *object : model.objects)
        if (object->volumes.size() == 1 && !object->brim_points.empty() &&
            !object->volumes.front()->get_offset().isApprox(Vec3d::Zero())) {
            original_points.emplace_back(object, object->brim_points);
            for (BrimPoint &point : object->brim_points)
                point.pos -= object->volumes.front()->get_offset().cast<float>();
        }
    if (! store_bbs_3mf(params))
        throw std::runtime_error("cannot write " + args.save_3mf);
    Args written = args;
    written.model = args.save_3mf;
    save_source_project(written, slice_config ? *slice_config : config, true, cut_information);
}

WipeTowerFootprint model_wipe_tower_footprint(const Model &model, const DynamicPrintConfig &config)
{
    Print print;
    print.is_BBL_printer() = config.opt_string("printer_model").rfind("Bambu Lab", 0) == 0;
    print.apply(model, config);
    const std::vector<unsigned int> ids = print.extruders(true);
    if (ids.empty()) return {};
    double height = 0., layer_height = std::numeric_limits<double>::max();
    for (const ModelObject *object : model.objects) {
        for (size_t i = 0; i < object->instances.size(); ++i)
            height = std::max(height, object->instance_convex_hull_bounding_box(i, true).size().z());
        const ConfigOption *option = object->config.option("layer_height");
        layer_height = std::min(layer_height, option ? option->getFloat() : config.opt_float("layer_height"));
    }
    if (layer_height == std::numeric_limits<double>::max()) layer_height = config.opt_float("layer_height");
    return estimate_wipe_tower_footprint(config, resolve_wipe_tower_type(config), ids, layer_height, height);
}

arrangement::ArrangePolygon model_wipe_tower_polygon(const DynamicPrintConfig &config, const WipeTowerFootprint &fp,
                                                     size_t plate_index, Vec2d &position)
{
    float x = float(config.option<ConfigOptionFloats>("wipe_tower_x")->get_at(plate_index));
    float y = float(config.option<ConfigOptionFloats>("wipe_tower_y")->get_at(plate_index));
    float brim = float(fp.brim_width);
    if (fp.width > 0. && fp.depth > 0.) {
        const BoundingBox outline = get_extents(estimate_wipe_tower_first_layer_outline(config,
            resolve_wipe_tower_type(config), fp.width, fp.depth, fp.height));
        brim += float(std::max({0., unscaled(outline.max.x()) - fp.width, unscaled(outline.max.y()) - fp.depth,
                                -unscaled(outline.min.x()), -unscaled(outline.min.y())}));
    }
    const BoundingBoxf bed(config.option<ConfigOptionPoints>("printable_area")->values);
    const float margin = float(WIPE_TOWER_MARGIN) + brim, comfort = float(WIPE_TOWER_AUTO_MARGIN) + brim;
    for (int axis = 0; axis < 2; ++axis) {
        float &value = axis == 0 ? x : y;
        const float size = float(axis == 0 ? fp.width : fp.depth);
        const float lo = float(bed.min[axis]) + margin;
        const float hi = std::max(lo, float(bed.max[axis]) - size - margin);
        float comfort_lo = float(bed.min[axis]) + comfort, comfort_hi = float(bed.max[axis]) - size - comfort;
        if (comfort_lo > comfort_hi) { comfort_lo = lo; comfort_hi = hi; }
        value = value < lo - 5.f || value > hi + 5.f ? std::clamp(value, comfort_lo, comfort_hi) : std::clamp(value, lo, hi);
    }
    position = Vec2d(x, y);
    arrangement::ArrangePolygon tower;
    tower.poly.contour = Slic3r::Polygon({Point::new_scale(x - brim, y - brim), Point::new_scale(x + fp.width + brim, y - brim),
                                  Point::new_scale(x + fp.width + brim, y + fp.depth + brim), Point::new_scale(x - brim, y + fp.depth + brim)});
    tower.bed_idx = int(plate_index);
    tower.name = "WipeTower";
    tower.is_virt_object = tower.is_wipe_tower = true;
    return tower;
}

void wipe_tower_only(const Args &args, const Model &model, const DynamicPrintConfig &config, const PlateLayout &layout)
{
    nlohmann::json out = {{"schema", "orca.wipe_tower.v1"}, {"plates", nlohmann::json::array()}};
    const int count = std::max(1, layout.count);
    for (int index = 1; index <= count; ++index) {
        if (args.plate_index && args.plate_index != index) continue;
        Model plate = model;
        for (size_t o = plate.objects.size(); o-- > 0;) {
            ModelObject *object = plate.objects[o];
            for (size_t i = object->instances.size(); i-- > 0;) {
                const auto it = layout.instance_plate.find(object->instances[i]->id());
                if ((it == layout.instance_plate.end() ? 1 : it->second) != index) object->delete_instance(i);
            }
            if (object->instances.empty()) plate.delete_object(o);
            else object->invalidate_bounding_box();
        }
        DynamicPrintConfig settings = config;
        if (const auto it = layout.plates.find(index); it != layout.plates.end()) {
            DynamicPrintConfig overrides = it->second.second;
            if (const auto *bed = overrides.option<ConfigOptionEnum<BedType>>("curr_bed_type"); bed && bed->value == btDefault)
                overrides.erase("curr_bed_type");
            settings.apply(overrides);
        }
        for (const auto &[key, value] : args.overrides) settings.set_deserialize_strict(key, value);
        plate.curr_plate_index = 0;
        if (!args.events.empty()) {
            auto &events = plate.plates_custom_gcodes[0];
            events.gcodes = args.events;
            events.mode = CustomGCode::MultiAsSingle;
        }
        const WipeTowerFootprint fp = model_wipe_tower_footprint(plate, settings);
        Vec2d position;
        model_wipe_tower_polygon(settings, fp, size_t(index - 1), position);
        out["plates"].push_back({{"plate", index}, {"x", position.x()}, {"y", position.y()},
            {"width", fp.width}, {"depth", fp.depth}, {"height", fp.height}, {"brim_width", fp.brim_width}});
    }
    boost::nowide::ofstream file(args.wipe_tower_only);
    file << out.dump(1) << "\n";
    if (!file) throw std::runtime_error("cannot write " + args.wipe_tower_only);
}

void arrange_only(const Args &args, Model &model, const DynamicPrintConfig &config)
{
    arrangement::ArrangePolygons items;
    std::vector<ModelInstance *> instances;
    for (ModelObject *object : model.objects)
        for (ModelInstance *instance : object->instances) {
            arrangement::ArrangePolygon item = get_instance_arrange_poly(instance, config);
            item.itemid = int(items.size());
            items.push_back(std::move(item));
            instances.push_back(instance);
        }
    arrangement::ArrangeParams params;
    params.allow_rotations         = args.arrange_rotation;
    params.allow_multi_materials_on_same_plate = args.arrange_multi_materials;
    params.clearance_height_to_rod = float(config.opt_float("extruder_clearance_height_to_rod"));
    params.clearance_height_to_lid = float(config.opt_float("extruder_clearance_height_to_lid"));
    params.clearance_radius        = float(config.opt_float("extruder_clearance_radius"));
    params.nozzle_height           = float(config.opt_float("nozzle_height"));
    params.printable_height        = float(config.opt_float("printable_height"));
    params.align_center            = config.option<ConfigOptionPoint>("best_object_pos")->value;
    params.is_seq_print            = config.opt_enum<PrintSequence>("print_sequence") == PrintSequence::ByObject;
    params.align_to_y_axis         = config.opt_enum<PrinterStructure>("printer_structure") == PrinterStructure::psI3;
    params.min_obj_distance        = args.arrange_spacing >= 0. ? scaled(args.arrange_spacing) : 0;
    if (params.is_seq_print) {
        params.bed_shrink_x = BED_SHRINK_SEQ_PRINT;
        params.bed_shrink_y = BED_SHRINK_SEQ_PRINT;
    }
    params.progressind = [](unsigned, std::string) {};
    arrangement::update_arrange_params(params, &config, items);
    arrangement::update_selected_items_inflation(items, &config, params);
    arrangement::update_selected_items_axis_align(items, &config, params);
    const Points bed = arrangement::get_shrink_bedpts(&config, params);
    const auto &exclude = config.option<ConfigOptionPoints>("bed_exclude_area")->values;
    for (size_t i = 0; i + 3 < exclude.size(); i += 4) {
        BoundingBoxf box;
        for (size_t k = i; k < i + 4; ++k)
            box.merge(exclude[k]);
        arrangement::ArrangePolygon region;
        region.poly.contour = Slic3r::Polygon({ { scaled(box.min.x()), scaled(box.min.y()) }, { scaled(box.max.x()), scaled(box.min.y()) },
                                        { scaled(box.max.x()), scaled(box.max.y()) }, { scaled(box.min.x()), scaled(box.max.y()) } });
        region.is_virt_object = true;
        region.bed_idx        = 0;
        region.height         = 1;
        region.inflation      = scaled(1.);
        params.excluded_regions.push_back(std::move(region));
    }
    arrangement::ArrangePolygons fixed;
    if (config.opt_bool("enable_prime_tower") && !params.is_seq_print && !params.is_belt) {
        bool need_tower = config.opt_enum<TimelapseType>("timelapse_type") == TimelapseType::tlSmooth;
        std::map<int, std::vector<int>> bed_filaments;
        for (const auto &item : items) {
            need_tower |= item.extrude_ids.size() > 1;
            auto &ids = bed_filaments[item.bed_temp];
            for (int id : item.extrude_ids)
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        if (params.allow_multi_materials_on_same_plate)
            for (const auto &[temperature, ids] : bed_filaments) need_tower |= ids.size() > 1;
        if (need_tower) {
            const WipeTowerFootprint fp = model_wipe_tower_footprint(model, config);
            if (fp.width > 0. && fp.depth > 0.)
                for (size_t index = 0; index < items.size(); ++index) {
                    Vec2d position;
                    fixed.push_back(model_wipe_tower_polygon(config, fp, index, position));
                }
        }
    }
    arrangement::arrange(items, fixed, bed, params);

    nlohmann::json out = { { "schema", "orca.arrange.v1" }, { "objects", nlohmann::json::array() } };
    int plates = 0, unplaced = 0;
    for (size_t o = 0; o < model.objects.size(); ++o)
        out["objects"].push_back({ { "index", o }, { "name", model.objects[o]->name }, { "instances", nlohmann::json::array() } });
    for (const arrangement::ArrangePolygon &item : items) {
        ModelInstance *instance = instances[size_t(item.itemid)];
        ModelObject   *object   = instance->get_object();
        const size_t   o = std::find(model.objects.begin(), model.objects.end(), object) - model.objects.begin();
        const size_t   i = std::find(object->instances.begin(), object->instances.end(), instance) - object->instances.begin();
        nlohmann::json place = { { "index", i } };
        if (item.bed_idx < 0) {
            place["plate"] = nullptr;
            ++unplaced;
        } else {
            instance->apply_arrange_result(item.translation.cast<double>(), item.rotation);
            const BoundingBoxf3 box = object->instance_bounding_box(i);
            place["plate"] = item.bed_idx + 1;
            place["x"]     = box.center().x();
            place["y"]     = box.center().y();
            place["rot"]   = Geometry::rad2deg(item.rotation);
            plates         = std::max(plates, item.bed_idx + 1);
        }
        out["objects"][o]["instances"].push_back(std::move(place));
    }
    for (nlohmann::json &object : out["objects"])
        std::sort(object["instances"].begin(), object["instances"].end(),
                  [](const nlohmann::json &a, const nlohmann::json &b) { return a.at("index") < b.at("index"); });
    out["plates"]   = plates;
    out["unplaced"] = unplaced;
    boost::nowide::ofstream file(args.arrange_only);
    file << out.dump(1) << "\n";
    if (!file)
        throw std::runtime_error("cannot write " + args.arrange_only);
    std::cout << "arranged " << items.size() - unplaced << " plates " << plates << " unplaced " << unplaced << "\n";
}

size_t add_cut_connectors(ModelObject &object, const std::string &spec, const Transform3d &cut_matrix)
{
    if (spec.empty()) return 0;
    const auto connectors = nlohmann::json::parse(spec);
    if (!connectors.is_array()) throw std::runtime_error("--cut-connectors needs a JSON array");
    size_t dowels = 0;
    for (const auto &row : connectors) {
        const auto positive = [&row](const char *key, double fallback) {
            const double value = row.value(key, fallback);
            if (!std::isfinite(value) || value <= 0. || value > std::numeric_limits<float>::max()) throw std::runtime_error(std::string("cut connector ") + key + " must be positive");
            return value;
        };
        const auto tolerance = [&row](const char *key) {
            const double value = row.value(key, row.value("tolerance", 0.1));
            if (!std::isfinite(value) || value < 0. || value > std::numeric_limits<float>::max()) throw std::runtime_error(std::string("cut connector ") + key + " must be nonnegative");
            return float(value);
        };
        static const std::map<std::string, CutConnectorType> types = {{"plug", CutConnectorType::Plug}, {"dowel", CutConnectorType::Dowel}, {"snap", CutConnectorType::Snap}};
        static const std::map<std::string, CutConnectorShape> shapes = {{"triangle", CutConnectorShape::Triangle}, {"square", CutConnectorShape::Square}, {"hexagon", CutConnectorShape::Hexagon}, {"circle", CutConnectorShape::Circle}};
        const auto type = types.find(row.value("type", "plug"));
        const auto shape = shapes.find(row.value("shape", "circle"));
        const std::string style = row.value("style", "prism");
        if (type == types.end() || shape == shapes.end() || (style != "prism" && style != "frustum"))
            throw std::runtime_error("unknown cut connector type, shape or style");
        CutConnector connector;
        connector.attribs = {type->second, style == "prism" ? CutConnectorStyle::Prism : CutConnectorStyle::Frustum, shape->second};
        connector.radius = float(positive("radius", 2.5));
        connector.height = float(positive("height", row.value("depth", 5.)));
        connector.radius_tolerance = tolerance("radius_tolerance");
        connector.height_tolerance = tolerance("height_tolerance");
        connector.z_angle = float(Geometry::deg2rad(row.value("angle", 0.)));
        const auto &pos = row.at("pos");
        if (!pos.is_array() || pos.size() != 2) throw std::runtime_error("cut connector pos needs two cut-plane coordinates");
        const Vec3d local(pos.at(0).get<double>(), pos.at(1).get<double>(), 0.);
        if (!local.allFinite() || !std::isfinite(connector.z_angle)) throw std::runtime_error("cut connector position and angle must be finite");
        connector.pos = cut_matrix * local;
        connector.rotation_m = Geometry::Transformation(cut_matrix).get_rotation_matrix();
        if (connector.attribs.type == CutConnectorType::Dowel) {
            if (connector.attribs.style == CutConnectorStyle::Prism) connector.height *= 2.f;
            if (!std::isfinite(connector.height)) throw std::runtime_error("dowel height is too large");
            ++dowels;
        } else
            connector.pos += connector.rotation_m * Vec3d(0., 0., 0.5 * connector.height);
        const int sectors = shape->second == CutConnectorShape::Triangle ? 3 : shape->second == CutConnectorShape::Square ? 4 : shape->second == CutConnectorShape::Hexagon ? 6 : 360;
        indexed_triangle_set mesh;
        if (connector.attribs.type == CutConnectorType::Snap) {
            const double space = positive("snap_space", 0.3), bulge = positive("snap_bulge", 0.15);
            if (shape->second != CutConnectorShape::Circle || space >= 1. || bulge > space)
                throw std::runtime_error("snap connector needs a circle, snap_space < 1 and snap_bulge <= snap_space");
            mesh = its_make_snap(1., 1., space, bulge);
        } else if (connector.attribs.style == CutConnectorStyle::Prism)
            mesh = its_make_cylinder(1., 1., 2. * M_PI / sectors);
        else if (connector.attribs.type == CutConnectorType::Plug)
            mesh = its_make_frustum(1., 1., 2. * M_PI / sectors);
        else
            mesh = its_make_frustum_dowel(1., 1., sectors);
        ModelVolume *volume = object.add_volume(TriangleMesh(std::move(mesh)), ModelVolumeType::NEGATIVE_VOLUME);
        volume->set_transformation(Geometry::translation_transform(connector.pos) * connector.rotation_m *
            Geometry::rotation_transform(-connector.z_angle * Vec3d::UnitZ()) *
            Geometry::scale_transform(Vec3d(connector.radius, connector.radius, connector.height)));
        volume->cut_info = {connector.attribs.type, connector.radius_tolerance, connector.height_tolerance};
        volume->name = "Connector-" + std::to_string(object.cut_id.connectors_cnt() + 1);
        object.cut_id.increase_connectors_cnt(1);
    }
    return dowels;
}

void clean_dovetail_mesh(TriangleMesh &mesh)
{
    for (Vec3f &vertex : mesh.its.vertices)
        for (int axis = 0; axis < 3; ++axis)
            vertex[axis] = float(std::round(double(vertex[axis]) * 1e6) / 1e6);
    its_merge_vertices(mesh.its);
    its_remove_degenerate_faces(mesh.its);
}

TriangleMesh cut_solid_mesh(const ModelObject &object)
{
    TriangleMesh result;
    for (const ModelVolume *volume : object.volumes)
        if (volume->is_model_part()) {
            TriangleMesh mesh(volume->mesh());
            mesh.transform(volume->get_matrix(), true);
            if (result.empty()) result = std::move(mesh);
            else MeshBoolean::cgal::plus(result, mesh);
        }
    for (const ModelVolume *volume : object.volumes)
        if (volume->is_negative_volume()) {
            TriangleMesh mesh(volume->mesh());
            mesh.transform(volume->get_matrix(), true);
            MeshBoolean::minus(result, mesh);
        }
    result.transform(object.instances.front()->get_matrix(), true);
    its_merge_vertices(result.its);
    its_remove_degenerate_faces(result.its);
    return result;
}

void cut_only(PresetBundle &bundle, const Args &args, Model &model, DynamicPrintConfig &config)
{
    if (model.objects.size() != 1 || model.objects.front()->instances.size() != 1)
        throw std::runtime_error("--cut-only needs a model of one object with one instance, this has " +
                                 std::to_string(model.objects.size()) + " objects");
    double v[6];
    const int n = std::sscanf(args.cut.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    Vec3d point = Vec3d::Zero(), normal = Vec3d::UnitZ();
    if (n == 1)
        point.z() = v[0];
    else if (n == 6) {
        point  = Vec3d(v[0], v[1], v[2]);
        normal = Vec3d(v[3], v[4], v[5]);
    }
    if ((n != 1 && n != 6) || !(normal.norm() > 1e-9))
        usage("--cut-only expects Z or X,Y,Z,NX,NY,NZ with a nonzero normal");
    ModelObject *object = model.objects.front();
    const Transform3d rotation(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), normal.normalized()));
    const Transform3d cut_matrix = Geometry::translation_transform(point - object->instances.front()->get_offset()) * rotation;
    const bool upper = args.cut_keep != "lower", lower = args.cut_keep != "upper";
    const bool dovetail = args.cut_mode == "dovetail";
    const bool connectors = !args.cut_connectors.empty() && !nlohmann::json::parse(args.cut_connectors).empty();
    if ((connectors || dovetail) && (!upper || !lower))
        throw std::runtime_error("connectors and dovetails require --cut-keep both");
    if (dovetail && connectors) throw std::runtime_error("dovetail cuts do not take separate connectors");
    if (!dovetail && !args.cut_dovetail.empty()) throw std::runtime_error("--cut-dovetail requires --cut-mode dovetail");
    if ((connectors || dovetail) && object->cut_id.id().invalid()) object->cut_id.init();
    const size_t dowels = add_cut_connectors(*object, args.cut_connectors, cut_matrix);
    if (connectors || dovetail) object->cut_id.increase_check_sum(1 + dowels);
    const ModelObjectCutAttributes attributes = only_if(upper, ModelObjectCutAttribute::KeepUpper) |
                                                only_if(lower, ModelObjectCutAttribute::KeepLower) |
                                                ModelObjectCutAttribute::PlaceOnCutUpper |
                                                only_if(args.cut_place_lower, ModelObjectCutAttribute::PlaceOnCutLower) |
                                                only_if(dowels > 0, ModelObjectCutAttribute::CreateDowels) |
                                                only_if(!connectors && !dovetail, ModelObjectCutAttribute::InvalidateCutInfo);
    Cut cut(object, 0, cut_matrix, attributes);
    Cut::Groove groove;
    int groove_count = 1;
    float groove_gap = 2.f;
    if (dovetail) {
        const auto spec = args.cut_dovetail.empty() ? nlohmann::json::object() : nlohmann::json::parse(args.cut_dovetail);
        groove.depth = spec.value("depth", 2.f);
        groove.width = spec.value("width", 8.f);
        groove.flaps_angle = float(Geometry::deg2rad(spec.value("flaps_angle", 60.)));
        groove.angle = float(Geometry::deg2rad(spec.value("angle", 0.)));
        groove.depth_tolerance = spec.value("depth_tolerance", spec.value("tolerance", 0.1f));
        groove.width_tolerance = spec.value("width_tolerance", spec.value("tolerance", 0.1f));
        groove_count = spec.value("count", 1);
        groove_gap = spec.value("gap", 2.f);
        if (!(std::isfinite(groove.depth) && groove.depth > 0.f && std::isfinite(groove.width) && groove.width > 0.f &&
              std::isfinite(groove.flaps_angle) && groove.flaps_angle > 0.f && groove.flaps_angle <= float(M_PI / 2.) &&
              std::isfinite(groove.angle) && std::abs(groove.angle) < M_PI / 2. &&
              std::isfinite(groove.depth_tolerance) && groove.depth_tolerance >= 0.f &&
              std::isfinite(groove.width_tolerance) && groove.width_tolerance >= 0.f &&
              std::isfinite(groove_gap) && groove_gap >= 0.f && groove_count >= 1 && groove_count <= 100))
            throw std::runtime_error("invalid dovetail dimensions, angles, tolerances or count");
    }
    const ModelObjectPtrs &pieces = dovetail ? cut.perform_with_groove(groove, rotation, groove_count, groove_gap,
        float(object->instance_bounding_box(0).radius())) : cut.perform_with_plane();
    if (pieces.size() != size_t(upper) + size_t(lower) + dowels)
        throw std::runtime_error("the cut plane does not cross the object");
    std::vector<std::string> names = upper && lower ? std::vector<std::string>{ "upper", "lower" } :
                                                            std::vector<std::string>{ upper ? "upper" : "lower" };
    for (size_t i = 0; i < dowels; ++i) names.push_back("dowel-" + std::to_string(i + 1));
    Model result;
    for (const ModelObject *piece : pieces) {
        ModelObject *added = result.add_object(*piece);
        if (dowels > 0 && result.objects.size() <= 2) {
            const Vec3d shift = normal.normalized() * (result.objects.size() == 1 ? 0.05 : -0.05);
            for (ModelVolume *volume : added->volumes)
                if (volume->is_negative_volume() && volume->cut_info.is_connector &&
                    volume->cut_info.connector_type == CutConnectorType::Dowel)
                    volume->set_offset(volume->get_offset() + shift);
            added->invalidate_bounding_box();
        }
        if (dovetail) {
            for (ModelVolume *volume : added->volumes)
                if (volume->is_model_part()) {
                    TriangleMesh mesh(volume->mesh());
                    MeshBoolean::self_union(mesh);
                    clean_dovetail_mesh(mesh);
                    volume->set_mesh(std::move(mesh));
                    volume->calculate_convex_hull();
                }
            added->invalidate_bounding_box();
        }
        added->ensure_on_bed();
    }
    for (size_t i = 0; i < result.objects.size(); ++i) {
        TriangleMesh mesh = result.objects[i]->mesh();
        for (const ModelVolume *volume : result.objects[i]->volumes)
            if (connectors && volume->cut_info.is_connector) {
                const auto box = volume->mesh().transformed_bounding_box(volume->get_matrix());
                std::cout << "cut_connector " << names[i] << " " << volume->name << " type " << (volume->cut_info.connector_type == CutConnectorType::Plug ? "plug" :
                              volume->cut_info.connector_type == CutConnectorType::Dowel ? "dowel" : "snap")
                          << " negative " << volume->is_negative_volume() << " size_mm " << box.size().transpose()
                          << " radius_tolerance " << volume->cut_info.radius_tolerance << " height_tolerance " << volume->cut_info.height_tolerance << "\n";
            }
        std::cout << "cut_piece " << names[i] << " triangles " << mesh.facets_count() << " open_edges "
                  << its_num_open_edges(mesh.its) << " volume_mm3 " << mesh.volume() << "\n";
    }
    if (boost::algorithm::iends_with(args.output, ".3mf")) {
        double x = 0.;
        for (ModelObject *piece : result.objects) {
            const BoundingBoxf3 box = piece->instance_bounding_box(0);
            piece->instances.front()->set_offset(piece->instances.front()->get_offset() + Vec3d(x - box.min.x(), 0., 0.));
            piece->invalidate_bounding_box();
            x += box.size().x() + 10.;
        }
        place(args, result, config);
        PlateDataPtrs plates;
        auto plate = std::make_unique<PlateData>();
        plate->plate_index = 0;
        for (size_t i = 0; i < result.objects.size(); ++i)
            plate->objects_and_instances.emplace_back(int(i), 0);
        plates.push_back(plate.get());
        Args written = args;
        written.save_3mf = args.output;
        save_project(bundle, written, result, config, false, plates);
        std::cout << "cut_project " << args.output << "\n";
        return;
    }
    std::string stem = args.output;
    if (boost::algorithm::iends_with(stem, ".stl"))
        stem.resize(stem.size() - 4);
    for (size_t i = 0; i < result.objects.size(); ++i) {
        TriangleMesh mesh = connectors ? cut_solid_mesh(*result.objects[i]) : result.objects[i]->mesh();
        if (dovetail) clean_dovetail_mesh(mesh);
        const std::string file = stem + "." + names[i] + ".stl";
        if (!store_stl(file.c_str(), &mesh, true))
            throw std::runtime_error("cannot write " + file);
        std::cout << "cut_file " << names[i] << " " << file << "\n";
    }
}

}

#include "nonplanar_slice_json.ipp"
#include "nonplanar_slice_moves.ipp"
#include "nonplanar_slice_catalog.ipp"
#include "nonplanar_model_context.ipp"
#include "nonplanar_patch_preview.ipp"
#include "nonplanar_project_edit.ipp"

void select_layout_plate(const Args &args, Model &model, DynamicPrintConfig &config, const PlateLayout &layout,
                         std::vector<SourcePlate> &source_plates, ContextInstances &membership,
                         std::optional<Model> &full_model, std::optional<DynamicPrintConfig> &full_config)
{
    PlateDataPtrs plates;
    ScopeGuard release([&plates] { release_PlateData_list(plates); });
    for (int index = 1; index <= layout.count; ++index) {
        SourcePlate &source = source_plates.emplace_back();
        source.plate_index  = index - 1;
        if (const auto it = layout.plate_locked.find(index); it != layout.plate_locked.end())
            source.locked = it->second;
        if (const auto it = layout.plates.find(index); it != layout.plates.end())
            std::tie(source.name, source.config) = it->second;
        auto *plate        = new PlateData;
        plate->plate_index = source.plate_index;
        plate->plate_name  = source.name;
        plate->locked      = source.locked;
        plate->config      = source.config;
        plates.push_back(plate);
    }
    for (ModelObject *object : model.objects) {
        for (ModelInstance *instance : object->instances) {
            const int index = layout.instance_plate.at(instance->id());
            instance->set_offset(instance->get_offset() + context_plate_origin(config, plates, index));
            membership[instance->id()] = {{"saved_object_id", nullptr}, {"saved_instance_id", nullptr}, {"source_plate_index", index},
                                          {"source_plate_name", source_plates[size_t(index - 1)].name}, {"saved_transform", nullptr}};
        }
        object->invalidate_bounding_box();
    }
    const int selected = args.plate_index ? args.plate_index : layout.count == 1 ? 1 : 0;
    if (selected == 0 && !(args.output.empty() && (!args.save_3mf.empty() || !args.arrange_only.empty())))
        throw std::runtime_error("--plate-index is required for a plate JSON of several plates");
    if (selected == 0)
        return;
    if (!args.save_3mf.empty()) {
        full_model  = model;
        full_config = config;
    }
    std::optional<DynamicPrintConfig> implicit_config;
    if (!args.plate_index && !layout.implicit_plate_settings) implicit_config = config;
    select_context_plate(model, config, plates, membership, selected);
    if (implicit_config) {
        config = std::move(*implicit_config);
        const auto *bed = plates.front()->config.option<ConfigOptionEnum<BedType>>("curr_bed_type");
        if (bed && bed->value > btDefault && bed->value < btCount)
            config.apply_only(plates.front()->config, {"curr_bed_type"});
    }
}
DynamicPrintConfig plate_save_config(const Args &args, const Model &model, const DynamicPrintConfig &global,
                                    const std::vector<SourcePlate> &sources, const ContextInstances &membership,
                                    bool project_settings)
{
    DynamicPrintConfig saved = global;
    if ((project_settings && !args.auto_orient) || std::any_of(args.overrides.begin(), args.overrides.end(),
            [](const auto &setting) { return setting.first.rfind("wipe_tower_", 0) == 0; }))
        return saved;
    for (const std::string key : {"wipe_tower_x", "wipe_tower_y"}) {
        auto &values = saved.option<ConfigOptionFloats>(key)->values;
        values.resize(std::max(values.size(), sources.size()), values.empty() ? 0. : values.back());
    }
    for (size_t index = 1; index <= sources.size(); ++index) {
        auto owned = saved_plates(model, sources, membership);
        if (owned.at(index - 1)->objects_and_instances.empty()) continue;
        PlateDataPtrs plates;
        plates.reserve(owned.size());
        for (auto &plate : owned) plates.push_back(plate.release());
        ScopeGuard release([&] { release_PlateData_list(plates); });
        Model selected = model;
        DynamicPrintConfig config = global;
        select_context_plate(selected, config, plates, membership, int(index));
        for (const auto &[key, value] : args.overrides) config.set_deserialize_strict(key, value);
        place(args, selected, config, project_settings && !args.auto_orient);
        for (const std::string key : {"wipe_tower_x", "wipe_tower_y"})
            saved.option<ConfigOptionFloats>(key)->values[index - 1] = config.option<ConfigOptionFloats>(key)->get_at(0);
    }
    return saved;
}

void explode_3mf(const Args &args)
{
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    En3mfType type;
    PlateDataPtrs plates;
    std::vector<Preset *> presets;
    ScopeGuard release([&] { release_PlateData_list(plates); for (Preset *preset : presets) delete preset; });
    Semver version;
    DynamicPrintConfig project_config;
    Model model;
    model = Model::read_from_archive(args.explode_3mf, &project_config, &substitutions, type,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances, &plates, &presets, &version);
    bool project_settings = !project_config.empty();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply(project_config);
    const DynamicPrintConfig source_layout = config;
    const bool snapshot = load_slice_config(args.explode_3mf, config, project_settings);
    const bool has_settings = snapshot || !project_config.empty();
    if (has_settings) { repair_project_flush(config); complete_project_ranges(model, config); }
    const auto saved = read_saved_instances(args.explode_3mf);
    restore_loaded_ids(saved, model);
    const ContextInstances membership = context_instances(args.explode_3mf, model, saved);
    const fs::path directory(args.explode_directory);
    fs::create_directories(directory);
    const auto vector = [](const Vec3d &v) { return Json::array({v.x(), v.y(), v.z()}); };
    const auto transformation = [](const Geometry::Transformation &t) { return context_transform(t.get_matrix()); };
    Json out = {{"schema", "orca.plate.v1"}, {"objects", Json::array()}, {"plates", Json::array()}};
    if (has_settings) {
        out["settings"] = slice_settings(config);
        out["project_settings"] = project_settings;
        out["implicit_plate_settings"] = false;
    }
    const std::vector<const char *> event_types = {"ColorChange", "PausePrint", "ToolChange", "Template", "Custom", "Unknown"};
    int count = 1;
    for (const PlateData *plate : plates)
        if (plate) count = std::max(count, plate->plate_index + 1);
    for (int index = 1; index <= count; ++index) {
        const auto plate = std::find_if(plates.begin(), plates.end(), [index](const PlateData *p) { return p && p->plate_index + 1 == index; });
        Json row = {{"index", index}, {"name", plate == plates.end() ? "" : (*plate)->plate_name},
                    {"locked", plate != plates.end() && (*plate)->locked},
                    {"settings", plate == plates.end() ? Json::object() : context_settings((*plate)->config)}};
        if (const auto events = model.plates_custom_gcodes.find(index - 1); events != model.plates_custom_gcodes.end()) {
            const auto mode = events->second.mode;
            Json info = {{"mode", mode == CustomGCode::SingleExtruder ? "SingleExtruder" : mode == CustomGCode::MultiAsSingle ?
                "MultiAsSingle" : mode == CustomGCode::MultiExtruder ? "MultiExtruder" : "Undef"}, {"gcodes", Json::array()}};
            for (const auto &item : events->second.gcodes)
                info["gcodes"].push_back({{"type", event_types.at(size_t(item.type))}, {"print_z", item.print_z},
                    {"extruder", item.extruder}, {"color", item.color}, {"extra", item.extra}});
            row["custom_gcode"] = std::move(info);
        }
        out["plates"].push_back(std::move(row));
    }
    for (size_t o = 0; o < model.objects.size(); ++o) {
        const ModelObject *object = model.objects[o];
        Json entry = {{"name", object->name}, {"input_file", object->input_file}, {"object_printable", object->printable},
            {"origin_translation", vector(object->origin_translation)}, {"settings", context_settings(object->config.get())},
            {"preserve_volume_order", true}, {"volumes", Json::array()}, {"instances", Json::array()}, {"ranges", Json::array()}};
        if (const ConfigOption *extruder = object->config.option("extruder"); extruder && extruder->getInt() > 0) {
            entry["extruder"] = extruder->getInt();
            entry["settings"].erase("extruder");
        }
        if (!object->layer_height_profile.get().empty()) entry["layer_height_profile"] = object->layer_height_profile.get();
        for (const BrimPoint &point : object->brim_points) {
            entry["brim_points"].push_back({point.pos.x(), point.pos.y(), point.pos.z(), point.head_front_radius});
            entry["brim_ears"].push_back({{"x", point.pos.x()}, {"y", point.pos.y()}, {"z", point.pos.z()}, {"radius", point.head_front_radius}});
        }
        for (const auto &[range, settings] : object->layer_config_ranges)
            entry["ranges"].push_back({{"z", {range.first, range.second}}, {"settings", context_settings(settings.get())},
                                      {"layer_height", settings.opt_float("layer_height")}});
        for (size_t v = 0; v < object->volumes.size(); ++v) {
            const ModelVolume *volume = object->volumes[v];
            const std::string file = "object-" + std::to_string(o) + "-volume-" + std::to_string(v) + ".3mf";
            write_plate_mesh((directory / file).string(), volume->mesh());
            std::string type = ModelVolume::type_to_string(volume->type());
            if (type == "normal_part") type = "part";
            else if (type == "modifier_part") type = "modifier";
            else if (type == "negative_part") type = "negative";
            Json row = {{"file", file}, {"name", volume->name}, {"type", type}, {"mesh_space", "volume"},
                {"mesh_init_shift", vector(volume->mesh().get_init_shift())}, {"transform", transformation(volume->get_transformation())},
                {"settings", context_settings(volume->config.get())}, {"paint", Json::object()}};
            if (const ConfigOption *extruder = volume->config.option("extruder"); extruder && extruder->getInt() > 0) {
                row["extruder"] = extruder->getInt();
                row["settings"].erase("extruder");
            }
            for (const auto &[kind, facets] : std::vector<std::pair<std::string, const FacetsAnnotation *>>{
                {"support", &volume->supported_facets}, {"seam", &volume->seam_facets}, {"color", &volume->mmu_segmentation_facets},
                {"fuzzy", &volume->fuzzy_skin_facets}, {"nonplanar", &volume->nonplanar_facets}})
                for (const auto &triangle : facets->get_data().triangles_to_split)
                    row["paint"][kind][std::to_string(triangle.triangle_idx)] = facets->get_triangle_as_string(triangle.triangle_idx);
            entry["volumes"].push_back(std::move(row));
        }
        for (size_t i = 0; i < object->instances.size(); ++i) {
            const ModelInstance *instance = object->instances[i];
            int plate = 1;
            if (const auto it = membership.find(instance->id()); it != membership.end() && it->second.at("source_plate_index").is_number_integer())
                plate = it->second.at("source_plate_index").get<int>();
            else
                for (const PlateData *p : plates)
                    if (p && std::find(p->objects_and_instances.begin(), p->objects_and_instances.end(), std::make_pair(int(o), int(i))) != p->objects_and_instances.end())
                        plate = p->plate_index + 1;
            const Vec3d origin = plate > 1 ? context_plate_origin(source_layout, plates, plate) : Vec3d::Zero();
            Geometry::Transformation local = instance->get_transformation();
            local.set_offset(local.get_offset() - origin);
            const BoundingBoxf3 box = object->instance_bounding_box(i);
            entry["instances"].push_back({{"transform", transformation(local)}, {"x", box.center().x() - origin.x()},
                {"y", box.center().y() - origin.y()}, {"z", box.min.z()}, {"rot", Geometry::rad2deg(local.get_rotation().z())},
                {"plate", plate}, {"printable", instance->printable}, {"auto_drop", instance->auto_drop},
                {"arrange_order", instance->arrange_order}, {"identify_id", instance->loaded_id > 0 ? instance->loaded_id : instance->get_labeled_id()}});
        }
        out["objects"].push_back(std::move(entry));
    }
    const fs::path path = directory / "plate.json";
    boost::nowide::ofstream file(path.string());
    file << out.dump(1) << "\n";
    file.close();
    if (!file) throw std::runtime_error("cannot write " + path.string());
    std::cout << "exploded_plate " << path.string() << "\n";
}

#include "nonplanar_calib_classic.ipp"

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--load-gcode") {
        const auto     t0 = std::chrono::steady_clock::now();
        const double   c0 = process_cpu_seconds();
        GCodeProcessor processor;
        processor.process_file(argv[2]);
        const auto &moves = processor.get_result().moves;
        std::cout << "preview_load_s " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << "\n"
                  << "preview_load_cpu_s " << process_cpu_seconds() - c0 << "\n"
                  << "preview_moves " << moves.size() << "\n"
                  << "preview_moves_mb " << double(moves.size() * sizeof(moves.front())) / 1e6 << "\n";
        return 0;
    }
#ifdef _WIN32
    std::unique_ptr<boost::nowide::args> utf8_args;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--emboss-text" || std::string(argv[i]) == "--emboss-fonts" || std::string(argv[i]) == "--emboss-svg" || std::string(argv[i]) == "--export-3mf-generic" || std::string(argv[i]) == "--export-drc") {
            utf8_args = std::make_unique<boost::nowide::args>(argc, argv);
            boost::nowide::nowide_filesystem();
            break;
        }
#endif
    const Args args = parse_args(argc, argv);
    if (!args.stage_cache.empty())
        Slic3r::Nonplanar::set_stage_cache(args.stage_cache, args.stage_cache_max_mb << 20);
    nlohmann::json record;
    if (const char *level = std::getenv("NP_LOG_LEVEL"))
        set_logging_level(unsigned(std::atoi(level)));
    try {
        if (!args.dump_options.empty()) dump_options(args.dump_options);
        if (!args.dump_options.empty() && args.dump_presets.empty()) return 0;
        if (args.orient_scores) { print_stock_orient_scores(args); return 0; }
        if (! args.json.empty()) slice_json_record(record, args);
        auto progress = [&args](const char *stage, int percent) {
            if (args.progress) std::cout << "progress " << stage << " " << percent << std::endl;
        };
        if (args.mesh_operation != "svg") progress("load", 0);
        set_resources_dir((fs::path(args.profiles) / "..").lexically_normal().string());
        const fs::path datadir = args.datadir.empty() ? fs::temp_directory_path() / fs::unique_path("np_slice_%%%%%%%%") :
                                                        fs::path(args.datadir);
        fs::create_directories(datadir);
        set_data_dir(datadir.string());
        set_temporary_dir(datadir.string());
        Nonplanar::HeadLibrary::instance().reload();
        if (!args.detect_regions.empty()) {
            PrintConfig printer_config;
            if (!args.printer.empty()) {
                PresetBundle bundle;
                printer_config.apply(resolve(bundle, args, "machine", Preset::TYPE_PRINTER, args.printer), true);
            } else printer_config.nonplanar_head.value = "bambu-a1-mini";
            for (const auto &[key, value] : args.overrides)
                if (printer_config.has(key)) printer_config.set_deserialize_strict(key, value);
            const auto head = Nonplanar::resolve_head_envelope(printer_config);
            Model model = read_input(args.model);
            nlohmann::json report = {{"schema", "orca.nonplanar.regions.v1"}, {"head", head.id},
                                     {"model", args.model}, {"regions", nlohmann::json::array()}};
            for (size_t oi = 0; oi < model.objects.size(); ++oi) {
                const ModelObject &object = *model.objects[oi];
                const Transform3d instance = object.instances.empty() ? Transform3d::Identity() : object.instances.front()->get_matrix();
                for (size_t vi = 0; vi < object.volumes.size(); ++vi) {
                    const ModelVolume &volume = *object.volumes[vi];
                    if (!volume.is_model_part()) continue;
                    for (const auto &r : Nonplanar::detect_nonplanar_regions(volume, instance * volume.get_matrix(), head))
                        report["regions"].push_back({{"object", oi}, {"volume", vi}, {"kind", Nonplanar::region_kind_name(r.kind)},
                            {"area", r.area}, {"slope", {r.min_slope, r.max_slope}}, {"span", r.height_span}, {"z_range", {r.min_z, r.max_z}},
                            {"centroid", {r.centroid.x(), r.centroid.y(), r.centroid.z()}}, {"facets", r.facets}});
                }
            }
            boost::nowide::ofstream out(args.detect_regions, std::ios::binary);
            out << report.dump(2) << "\n";
            if (!out) throw std::runtime_error("cannot write " + args.detect_regions);
            std::cout << "detected_regions " << report["regions"].size() << "\n";
            return 0;
        }
        if (!args.explode_3mf.empty()) { explode_3mf(args); return 0; }
        if (args.text_fonts) { std::cout << nlohmann::json({{"fonts", emboss_fonts(args)}}).dump() << "\n"; return 0; }
        if (args.mesh_operation == "text") { text_mesh(args); return 0; }
        if (args.mesh_operation == "svg") { svg_mesh(args); return 0; }
        if (args.mesh_operation == "generic3mf" || args.mesh_operation == "draco") { export_mesh(args); return 0; }
        if (!args.mesh_operation.empty()) { mesh_only(args); return 0; }
        if (!args.verify_gcode.empty()) {
            DynamicPrintConfig file_config;
            file_config.load_from_gcode_file(args.verify_gcode, ForwardCompatibilitySubstitutionRule::Enable);
            FullPrintConfig full;
            full.apply(file_config, true);
            GCodeProcessor processor;
            processor.apply_config(full);
            processor.process_file(args.verify_gcode);
            ClearanceParams params;
            params.head        = Nonplanar::resolve_head_envelope(full);
            params.contact_bar = true;
            if (full.nonplanar_mode.value == NonplanarMode::Auto && full.nonplanar_kinematics.value == RotaryLayout::None)
                full.nonplanar_mode.value = NonplanarMode::NonplanarTop;
            params.cone_tolerance = curved_cone_tolerance(full.nonplanar_mode.value);
            GCodeProcessorResult &result = processor.result();
            result.clearance_report = verify_clearance(result, params, *make_tool_kinematics(full));
            const ClearanceReport &report = result.clearance_report;
            if (!args.moves_out.empty())
                write_moves(args.moves_out, result, args.filaments, nlohmann::json::object(), args.moves_extended);
            std::cout << "clearance_hit_moves " << report.hit_moves
                      << "\nclearance_machine_hit_moves " << report.machine_hit_moves
                      << "\nclearance_tolerated_moves " << report.tolerated_moves
                      << "\nclearance_land_strict_moves " << report.land_strict_moves
                      << "\nclearance_min_mm " << report.min_clearance << "\n";
            for (const ClearanceHit &hit : report.worst)
                std::cout << "clearance_hit line " << hit.gcode_line << " move " << hit.move_id << " " << hit.component << " "
                          << hit.penetration << " mm\n";
            std::cout << "clearance_cone_graze_moves " << report.cone_graze_moves << "\n";
            std::cout << "clearance_duct_moves " << report.duct_moves << "\n";
            std::cout << "clearance_hit_classes land " << report.hit_land_moves << " cone " << report.hit_cone_moves << " body "
                      << report.hit_body_moves << " travel " << report.hit_travel_moves << " extrusion " << report.hit_extrusion_moves << "\n";
            if (const char *lines = std::getenv("NP_VERIFY_LINES")) {
                std::map<unsigned, float> least;
                std::stringstream         list(lines);
                for (std::string item; std::getline(list, item, ',');)
                    if (!item.empty())
                        least[unsigned(std::stoul(item))] = std::numeric_limits<float>::infinity();
                for (size_t i = 0; i < result.moves.size() && i < report.per_move.size(); ++i)
                    if (auto it = least.find(result.moves[i].gcode_id); it != least.end())
                        it->second = std::min(it->second, report.per_move[i]);
                for (const auto &[line, clearance] : least)
                    std::cout << "clearance_line " << line << " " << clearance << " mm\n";
            }
            return 0;
        }

        if (!args.dump_presets.empty()) { dump_presets(args); return 0; }
        PresetBundle       bundle;
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.apply(resolve(bundle, args, "machine", Preset::TYPE_PRINTER, args.printer));
        const std::string default_bed = !config.opt_string("default_bed_type").empty() ? config.opt_string("default_bed_type") :
            vendor_json_value({fs::path(args.profiles) / args.vendor / "machine"}, config.opt_string("printer_model"), "default_bed_type");
        if (args.vendor == "BBL" || config.opt_bool("support_multi_bed_types") || !default_bed.empty()) {
            Preset printer(Preset::TYPE_PRINTER, args.printer);
            printer.config = config;
            printer.config.set_key_value("default_bed_type", new ConfigOptionString(default_bed));
            config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(printer.get_default_bed_type(&bundle)));
        }
        config.apply(resolve(bundle, args, "process", Preset::TYPE_PRINT, args.process));
        apply_filaments(bundle, args, config);
        Model model;
        const bool project_input = boost::algorithm::iequals(fs::path(args.model).extension().string(), ".3mf");
        if (args.plate_index && !project_input && args.plate.empty())
            throw std::runtime_error("--plate-index requires a saved .3mf project or a plate JSON");
        if (!args.edit.empty() && !project_input)
            throw std::runtime_error("--edit requires a saved .3mf project");
        bool project_settings = false;
        ContextInstances saved_instances;
        std::unique_ptr<PlateData> source_plate;
        std::vector<SourcePlate> source_plates;
        std::optional<Model> full_model;
        std::optional<DynamicPrintConfig> full_config;
        PlateLayout layout;
        std::vector<ObjectID> source_objects;
        if (project_input) {
            ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
            En3mfType type;
            PlateDataPtrs plates;
            std::vector<Preset *> presets;
            Semver version;
            DynamicPrintConfig project_config;
            model = Model::read_from_archive(args.model, &project_config, &substitutions, type,
                LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances,
                &plates, &presets, &version);
            if (!project_config.empty()) {
                config = DynamicPrintConfig::full_print_config();
                config.apply(project_config);
                project_settings = true;
            }
            std::unique_ptr<ConfigOption> source_area(config.option("printable_area")->clone());
            load_slice_config(args.model, config, project_settings);
            const std::vector<SavedInstance> saved = read_saved_instances(args.model);
            restore_loaded_ids(saved, model);
            for (const ModelObject *object : model.objects)
                source_objects.push_back(object->id());
            if (!args.json.empty() || args.plate_index || !args.save_3mf.empty() || !args.edit.empty())
                saved_instances = context_instances(args.model, model, saved);
            if (!args.edit.empty()) {
                DynamicPrintConfig source_layout = config;
                source_layout.set_key_value("printable_area", source_area->clone());
                edit_project(args.edit, model, source_layout, plates, saved_instances);
            }
            if (!args.json.empty()) {
                auto &sources = record["model_context"]["source_plates"];
                sources = Json::array();
                for (const PlateData *plate : plates)
                    if (plate) sources.push_back({{"index", plate->plate_index + 1}, {"name", plate->plate_name},
                                                   {"settings", context_settings(plate->config)}});
            }
            if (!args.save_3mf.empty()) {
                for (const PlateData *plate : plates)
                    if (plate) source_plates.push_back({plate->plate_index, plate->plate_name, plate->locked, plate->config,
                                                        plate->thumbnail_file, plate->no_light_thumbnail_file, plate->top_file, plate->pick_file});
                if (args.plate_index) {
                    full_model  = model;
                    full_config = config;
                }
            }
            if (args.plate_index) {
                std::unique_ptr<ConfigOption> slice_area(config.option("printable_area")->clone());
                config.set_key_value("printable_area", source_area.release());
                select_context_plate(model, config, plates, saved_instances, args.plate_index);
                config.set_key_value("printable_area", slice_area.release());
                if (!args.json.empty())
                    record["model_context"]["source_plate"] = {{"index", args.plate_index},
                        {"name", plates.front()->plate_name}, {"settings", context_settings(plates.front()->config)}};
            }
            if (plates.size() == 1 && plates.front() != nullptr) {
                const auto *bed_type = plates.front()->config.option<ConfigOptionEnum<BedType>>("curr_bed_type");
                if (bed_type != nullptr && bed_type->value > btDefault && bed_type->value < btCount)
                    config.apply_only(plates.front()->config, {"curr_bed_type"});
            }
            for (ModelObject *object : model.objects)
                for (ModelInstance *instance : object->instances)
                    instance->use_loaded_id_for_label = true;
            if (plates.size() == 1) {
                source_plate.reset(plates.front());
                plates.clear();
                const bool changed_colours = !args.colours.empty() || std::any_of(args.overrides.begin(), args.overrides.end(),
                    [](const auto &setting) { return setting.first == "filament_colour"; });
                if (args.auto_orient || args.center_set || changed_colours || !args.parts.empty() || !args.paint.empty() || !args.edit.empty()) {
                    source_plate->plate_thumbnail.reset();
                    source_plate->thumbnail_file.clear();
                    source_plate->no_light_thumbnail_file.clear();
                    source_plate->top_file.clear();
                    source_plate->pick_file.clear();
                }
            }
            release_PlateData_list(plates);
            for (Preset *preset : presets)
                delete preset;
        } else if (args.calib.empty()) {
            model = ! args.plate.empty()   ? load_plate(args.plate, layout, config, !args.wipe_tower_only.empty()) :
                    ! args.project.empty() ? load_project(args.project) :
                read_input(args.model);
            if (layout.settings) {
                config = *layout.settings;
                project_settings = layout.project_settings;
            }
            if (layout.count > 0) {
                if (args.wipe_tower_only.empty())
                    select_layout_plate(args, model, config, layout, source_plates, saved_instances, full_model, full_config);
                else if (args.plate_index > layout.count)
                    throw std::runtime_error("--plate-index does not name a plate in this plate JSON");
            }
            else if (args.plate_index > 1)
                throw std::runtime_error("--plate-index does not name a plate in this plate JSON");
        }
        if (!args.json.empty())
            record["model_context"]["presets"] = context_presets(config, project_settings);
        for (const auto &[key, value] : args.overrides) {
            config.set_deserialize_strict(key, value);
            if (full_config)
                full_config->set_deserialize_strict(key, value);
        }
        if (project_input || layout.settings) {
            repair_project_flush(config);
            complete_project_ranges(model, config);
            if (full_config) repair_project_flush(*full_config);
            if (full_model) complete_project_ranges(*full_model, full_config ? *full_config : config);
        }
        if (! args.config_out.empty())
            config.save(args.config_out);
        if (! args.head_out.empty()) {
            PrintConfig printer_config;
            printer_config.apply(config, true);
            boost::nowide::ofstream out(args.head_out, std::ios::binary);
            out << Nonplanar::save_head_envelope(Nonplanar::resolve_head_envelope(printer_config));
            if (!out)
                throw std::runtime_error("cannot write " + args.head_out);
        }

        std::optional<Nonplanar::Calibration::Plate> calib_plate;
        std::optional<Calib_Params> classic_params;
        Nonplanar::HeadEnvelope calib_head;
        if (classic_calib_mode(args.calib)) {
            classic_params = classic_calib(args, model, config);
            for (const auto &[key, value] : args.overrides)
                config.set_deserialize_strict(key, value);
            finish_classic_calib(model, config);
        } else if (! args.calib.empty()) {
            PrintConfig printer_config;
            printer_config.apply(config, true);
            for (const auto &[key, value] : args.overrides)
                if (key == "nonplanar_head")
                    printer_config.nonplanar_head.value = value;
            if (! args.calib_head.empty())
                printer_config.nonplanar_head.value = args.calib_head;
            calib_head  = Nonplanar::resolve_head_envelope(printer_config);
            calib_plate = Nonplanar::Calibration::make_plate(*Nonplanar::Calibration::parse_test(args.calib), config, calib_head);
            config.apply(calib_plate->config);
            for (const auto &[key, value] : args.overrides)
                if (key != "nonplanar_head" || ! calib_plate->config.has(key))
                    config.set_deserialize_strict(key, value);
            Nonplanar::Calibration::add_to_model(*calib_plate, model);
        }
        if (model.objects.empty() && (args.wipe_tower_only.empty() || !args.paint.empty() || !args.parts.empty()))
            throw std::runtime_error("no objects in " + args.model);
        if (! args.cut.empty()) {
            cut_only(bundle, args, model, config);
            return 0;
        }
        if (!model.objects.empty()) apply_multi_material(args, model);
        if (!args.wipe_tower_only.empty()) {
            wipe_tower_only(args, model, config, layout);
            return 0;
        }
        if (! args.arrange_only.empty()) {
            arrange_only(args, model, config);
            return 0;
        }
        if (! args.events.empty()) {
            CustomGCode::Info &info = model.plates_custom_gcodes[model.curr_plate_index];
            info.gcodes             = args.events;
            std::sort(info.gcodes.begin(), info.gcodes.end());
            const bool change = std::any_of(info.gcodes.begin(), info.gcodes.end(),
                                            [](const CustomGCode::Item &item) { return item.type == CustomGCode::ToolChange; });
            info.mode = config.option<ConfigOptionFloats>("filament_diameter")->size() == 1 ? CustomGCode::SingleExtruder :
                        change ? CustomGCode::MultiAsSingle : CustomGCode::MultiExtruder;
        }
        if (! args.save_3mf.empty()) {
            if (preserves_project_geometry(args)) {
                save_source_project(args, full_config ? *full_config : config, project_settings);
            } else {
                place(args, model, config, project_settings && !args.auto_orient);
                Model &saved_model = full_model ? *full_model : model;
                const std::vector<std::unique_ptr<PlateData>> plates = saved_plates(saved_model, source_plates, saved_instances);
                PlateDataPtrs plate_list;
                for (const auto &plate : plates)
                    plate_list.push_back(plate.get());
                DynamicPrintConfig stored = full_config ? *full_config : config;
                if (!project_input && layout.count > 0)
                    stored = plate_save_config(args, saved_model, stored, source_plates, saved_instances, project_settings);
                save_project(bundle, args, saved_model, stored, project_input, plate_list,
                             project_input ? carried_cut_information(args.model, source_objects, saved_model) : std::string(),
                             !project_input && layout.count == 1 ? &config : nullptr);
            }
            std::cout << "project " << args.save_3mf << "\n";
            if (args.output.empty())
                return 0;
        }
        if (args.force_planar) {
            config.set_key_value("nonplanar_mode", new ConfigOptionEnum<NonplanarMode>(NonplanarMode::Off));
            clear_object_modes(model);
        }
        if (! args.confirm_up.empty()) {
            Vec3d up;
            if (std::sscanf(args.confirm_up.c_str(), "%lf,%lf,%lf", &up.x(), &up.y(), &up.z()) != 3)
                usage("--confirm-up expects X,Y,Z");
            const OrientSlice sl = slice_orientation(args, model, Nonplanar::rotation_to_z(up), config, datadir);
            std::cout << "orient_slice " << sl.ok << " " << sl.time_s << " " << sl.footprint << " " << sl.support << " " << sl.hits << "\n";
            if (! sl.error.empty())
                std::cout << "orient_slice_error " << sl.error << "\n";
            return 0;
        }
        if (args.auto_orient)
            rotate_model(model, choose_orientation(args, model, config, datadir));
        if (args.orient_only)
            return 0;
        place(args, model, config, project_settings && !args.auto_orient);
        if (!args.patch_preview.empty()) {
            write_patch_preview(args, model, config);
            if (args.output.empty())
                return 0;
        }

        if (!args.project_out.empty()) {
            fs::create_directories(model.get_backup_path());
            PlateData plate;
            plate.plate_index = 0;
            for (size_t i = 0; i < model.objects.size(); ++i)
                for (size_t j = 0; j < model.objects[i]->instances.size(); ++j)
                    plate.objects_and_instances.emplace_back(int(i), int(j));
            StoreParams params;
            params.plate_data_list = { &plate };
            params.path = args.project_out;
            params.model = &model;
            DynamicPrintConfig saved = project_config(bundle, args, config);
            params.config = &saved;
            params.strategy = SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::ShareMesh |
                              (project_input ? SaveStrategy::UseLoadedId : SaveStrategy::Default);
            if (!store_bbs_3mf(params))
                throw std::runtime_error("cannot write " + args.project_out);
        }

        std::mutex progress_mutex;
        int last_percent = 0, export_percent = 0;
        Print print;
        if (classic_params)
            print.set_calib_params(*classic_params);
        print.is_BBL_printer() = config.opt_string("printer_model").rfind("Bambu Lab", 0) == 0;
        for (ModelObject *object : model.objects)
            print.auto_assign_extruders(object);
        print.apply(model, config);
        Model::setBedPolygon(config, print.config());
        check_plate_layer_profiles(print);
        apply_layer_profile(args, model, print, config);
        if (! args.json.empty()) {
            const auto head = Nonplanar::resolve_head_envelope(print.config());
            const std::string file = Nonplanar::HeadLibrary::instance().source_path(head.id);
            record["inputs"]["mode"] = config.opt_serialize("nonplanar_mode");
            record["inputs"]["zaa_enabled"] = config.opt_bool("zaa_enabled");
            record["inputs"]["deform"] = config.opt_serialize("nonplanar_deform");
            const std::string atom = config.opt_string("nonplanar_atom_toolpath");
            record["inputs"]["atom_toolpath"] = atom.empty() ? nlohmann::json(nullptr) :
                nlohmann::json{{"file", atom}, {"sha1", file_sha1(atom)}};
            record["inputs"]["head"] = {{"id", head.id}, {"file", file}, {"sha1", file.empty() ? nlohmann::json(nullptr) : nlohmann::json(file_sha1(file))}};
        }

        std::vector<StringObjectException> warnings;
        if (args.allow_mixed_temperatures)
            print.set_check_multi_filaments_compatibility(false);
        if (const StringObjectException err = print.validate(&warnings); ! err.string.empty())
            throw std::runtime_error("validate: " + err.string);
        for (const StringObjectException &w : warnings)
            std::cerr << "warning: " << w.string << "\n";
        if (!args.plan_tops.empty()) {
            write_plan_tops(args, model, config, print);
            return 0;
        }
        const auto   t_process = std::chrono::steady_clock::now();
        const double c_process = process_cpu_seconds();
        progress("load", 100);
        progress("slice", 0);
        if (args.progress) print.set_status_callback([&](const PrintBase::SlicingStatus &status) {
            std::lock_guard<std::mutex> lock(progress_mutex);
            const int percent = std::clamp(status.percent, 0, 100);
            if (percent > last_percent) { last_percent = percent; progress("slice", percent); }
        });
        try {
            print.process();
        } catch (const SlicingErrors &errors) {
            throw std::runtime_error(print.slicing_errors_message(errors));
        }
        if (args.progress) print.set_status_callback([&](const PrintBase::SlicingStatus &status) {
            std::lock_guard<std::mutex> lock(progress_mutex);
            const int percent = std::min(99, (status.percent - 80) * 5);
            if (percent > 0 && percent > export_percent) { export_percent = percent; progress("export", percent); }
        });
        for (const PrintObject *object : print.objects())
            for (const PrintStateBase::Warning &w : object->step_state_with_warnings(posSupportMaterial).warnings)
                std::cerr << "warning: " << w.message << "\n";
        if (last_percent < 100) progress("slice", 100);
        progress("export", 0);
        const auto   t_export = std::chrono::steady_clock::now();
        const double c_export = process_cpu_seconds();
        GCodeProcessorResult result;
        const indexed_triangle_set mesh = plate_mesh(model);
        std::vector<std::array<double, 7>> planned;
        if (!args.planned_out.empty())
            GCodeWriter::planned_moves = &planned;
        print.export_gcode(args.output, &result, [&mesh, &config](const ThumbnailsParams &params) {
            ThumbnailsList list;
            for (const Vec2d &size : params.sizes)
                list.push_back(render_thumbnail(mesh, unsigned(size.x()), unsigned(size.y()), false,
                    config.option<ConfigOptionStrings>("filament_colour")->get_at(0)));
            return list;
        }, args.verify || !args.json.empty());
        GCodeWriter::planned_moves = nullptr;
        if (args.progress) print.set_status_default();
        for (const PrintStateBase::Warning &w : print.step_state_with_warnings(psGCodeExport).warnings)
            std::cerr << "warning: " << w.message << "\n";
        if (!args.planned_out.empty()) {
            boost::nowide::ofstream out(args.planned_out);
            out << "# x y z b c planned_tip_speed duration\n" << std::setprecision(10);
            for (const std::array<double, 7> &m : planned)
                out << m[0] << ' ' << m[1] << ' ' << m[2] << ' ' << m[3] << ' ' << m[4] << ' ' << m[5] << ' ' << m[6] << '\n';
        }
        const auto   t_done = std::chrono::steady_clock::now();
        const double c_done = process_cpu_seconds();

        progress("export", 100);
        if (!args.json.empty() || !args.moves_out.empty()) {
            const auto statistics = print_statistics_json(print, result);
            if (!args.json.empty()) record["statistics"] = statistics;
            if (!args.moves_out.empty() && !args.moves_extended)
                write_moves(args.moves_out, result, args.filaments, statistics, false);
        }

        if (args.explain)
            for (size_t i = 0; i < print.objects().size(); ++i) {
                std::cout << "explain object " << print.objects()[i]->model_object()->name << "\n";
                for (const Nonplanar::ReportPatch &patch : result.nonplanar_report.patches)
                    if (patch.object == i && patch.top)
                        std::cout << patch.explain << "\n";
            }

        {
            const WallFragmentation frag = wall_fragmentation(result, print.config().nonplanar_kinematics.value != RotaryLayout::None);
            const double            per_mm = frag.wall_mm > 0. ? 2. * double(frag.runs) / frag.wall_mm : 0.;
            const double            share  = frag.wall_mm > 0. ? frag.near_end_mm / frag.wall_mm : 0.;
            std::cout << "wall_fragmentation runs " << frag.runs << " wall_mm " << frag.wall_mm << " ends_per_mm " << per_mm
                      << " share_within_1mm_of_end " << share << "\n";
            if (!args.json.empty())
                record["wall_fragmentation"] = { { "runs", frag.runs }, { "wall_mm", frag.wall_mm }, { "ends_per_mm", per_mm },
                                                 { "share_within_1mm_of_end", share } };
        }
        size_t     vz_line         = 0;
        const auto [vz, vz_actual] = max_vz(result, &vz_line, print.config().nonplanar_kinematics.value != RotaryLayout::None);
        const auto &normal         = result.print_statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)];
        std::cout << "time_process_s " << std::chrono::duration<double>(t_export - t_process).count() << "\n"
                  << "time_export_s " << std::chrono::duration<double>(t_done - t_export).count() << "\n"
                  << "gcode " << args.output << "\n"
                  << "estimated_time_s " << normal.time << "\n"
                  << "max_vz_commanded " << vz << "\n"
                  << "max_vz_planned " << vz_actual << "\n"
                  << "max_vz_planned_line " << vz_line << "\n"
                  << "machine_max_speed_z " << config.opt<ConfigOptionFloats>("machine_max_speed_z")->get_at(0) << "\n"
                  << "cpu_process_s " << c_export - c_process << "\n"
                  << "cpu_export_s " << c_done - c_export << "\n"
                  << "preview_moves " << result.moves.size() << "\n"
                  << "preview_moves_mb " << double(result.moves.size() * sizeof(result.moves.front())) / 1e6 << "\n";
        if (! args.json.empty()) {
            auto &text = record["text_summary"];
            text["time_process_s"] = text_value(std::chrono::duration<double>(t_export - t_process).count());
            text["time_export_s"] = text_value(std::chrono::duration<double>(t_done - t_export).count());
            text["gcode"] = args.output;
            text["estimated_time_s"] = text_value(normal.time);
            text["max_vz_commanded"] = text_value(vz);
            text["max_vz_planned"] = text_value(vz_actual);
            text["max_vz_planned_line"] = text_value(vz_line);
            text["machine_max_speed_z"] = text_value(config.opt<ConfigOptionFloats>("machine_max_speed_z")->get_at(0));
        }
        for (const auto &warning : print.step_state_with_warnings(psSkirtBrim).warnings) {
            std::cout << "print_warning " << warning.message << "\n";
            if (!args.json.empty()) {
                if (!record["text_summary"].contains("print_warning"))
                    record["text_summary"]["print_warning"] = warning.message;
                record["print_warnings"].push_back(warning.message);
            }
        }
        for (const auto &warning : print.step_state_with_warnings(psGCodeExport).warnings)
            std::cout << "export_warning " << warning.message << "\n";
        if (! args.json.empty()) {
            std::string all;
            for (const auto &warning : print.step_state_with_warnings(psGCodeExport).warnings)
                all += (all.empty() ? "" : "\n") + warning.message;
            if (! all.empty())
                record["text_summary"]["export_warning"] = all;
        }
        for (const PrintObject *object : print.objects())
            for (int step = 0; step < int(posCount); ++step)
                for (const auto &warning : object->step_state_with_warnings(PrintObjectStep(step)).warnings) {
                    std::cout << "object_warning " << warning.message << "\n";
                    if (! args.json.empty()) {
                        if (! record["text_summary"].contains("object_warning"))
                            record["text_summary"]["object_warning"] = warning.message;
                        record["object_warnings"].push_back(warning.message);
                    }
                }
        Nonplanar::FlowModStats flow;
        for (const PrintObject *object : print.objects())
            flow.merge(object->conformal().flow_mod);
        if (flow.points > 0)
            std::cout << "flow_points " << flow.points << "\n"
                      << "flow_normal_gap_mm " << flow.min_gap << " " << flow.max_gap << "\n"
                      << "flow_laid_mm " << flow.min_laid << " " << flow.max_laid << "\n"
                      << "flow_clamped thick " << flow.max_clamped << " thin " << flow.min_clamped << " dragged " << flow.dragged
                      << " contact " << flow.contact << "\n";
        if (! args.json.empty() && flow.points > 0) {
            auto &text = record["text_summary"];
            text["flow_points"] = text_value(flow.points);
            text["flow_normal_gap_mm"] = text_value(flow.min_gap) + " " + text_value(flow.max_gap);
            text["flow_laid_mm"] = text_value(flow.min_laid) + " " + text_value(flow.max_laid);
            text["flow_clamped"] = "thick " + text_value(flow.max_clamped) + " thin " + text_value(flow.min_clamped) +
                " dragged " + text_value(flow.dragged) + " contact " + text_value(flow.contact);
        }
        double np_extrude = 0., np_other = 0.;
        std::map<std::string, double> np_kind;
        for (size_t i = 0; i < result.moves.size(); ++i) {
            const auto &m = result.moves[i];
            if (result.state(m).nonplanar_patch_shell == 0xFF)
                continue;
            const double t = m.time[size_t(PrintEstimatedStatistics::ETimeMode::Normal)];
            if (m.type == EMoveType::Extrude) {
                np_extrude += t;
                continue;
            }
            np_other += t;
            const Vec3f d = i > 0 ? Vec3f(m.position - result.moves[i - 1].position) : Vec3f::Zero();
            const bool  xy = std::abs(d.x()) + std::abs(d.y()) > 1e-4f, z = std::abs(d.z()) > 1e-4f;
            np_kind[m.type == EMoveType::Retract || m.type == EMoveType::Unretract ? "retract" : xy && z ? "xyz" : xy ? "xy" : z ? "z" : "other"] += t;
        }
        for (const auto &[kind, t] : np_kind)
            std::cout << "nonplanar_other_kind_s " << t << " " << kind << "\n";
        if (! args.json.empty() && !np_kind.empty())
            record["text_summary"]["nonplanar_other_kind_s"] = text_value(np_kind.begin()->second) + " " + np_kind.begin()->first;
        std::map<std::string, double> role_time;
        for (const auto &m : result.moves)
            role_time[m.type == EMoveType::Extrude ? ExtrusionEntity::role_to_string(m.extrusion_role) : std::string("Travel and other")] +=
                m.time[size_t(PrintEstimatedStatistics::ETimeMode::Normal)];
        for (const auto &[role, t] : role_time)
            std::cout << "role_time_s " << t << " " << role << "\n";
        if (! args.json.empty() && !role_time.empty())
            record["text_summary"]["role_time_s"] = text_value(role_time.begin()->second) + " " + role_time.begin()->first;
        if (!args.move_times.empty()) {
            std::ofstream mt(args.move_times);
            for (const auto &m : result.moves)
                mt << m.gcode_id << ' ' << m.position.x() << ' ' << m.position.y() << ' ' << m.position.z() << ' '
                   << m.time[size_t(PrintEstimatedStatistics::ETimeMode::Normal)] << ' ' << int(result.state(m).nonplanar_patch_shell != 0xFF) << '\n';
        }
        std::map<std::string, double> feature_time;
        for (size_t i = 0; i < result.moves.size(); ++i) {
            const auto &m  = result.moves[i];
            const bool  np = result.state(m).nonplanar_patch_shell != 0xFF;
            std::string kind;
            if (m.type == EMoveType::Extrude)
                kind = ExtrusionEntity::role_to_string(m.extrusion_role);
            else {
                const Vec3f d  = i > 0 ? Vec3f(m.position - result.moves[i - 1].position) : Vec3f::Zero();
                const bool  xy = std::abs(d.x()) + std::abs(d.y()) > 1e-4f, z = std::abs(d.z()) > 1e-4f;
                kind = m.type == EMoveType::Retract || m.type == EMoveType::Unretract ? "retract" :
                       xy && z                                                       ? "move xyz" :
                       xy                                                            ? "move xy" :
                       z                                                             ? "move z" :
                                                                                       "other";
            }
            feature_time[(np ? "np " : "planar ") + kind] += m.time[size_t(PrintEstimatedStatistics::ETimeMode::Normal)];
        }
        for (const auto &[kind, t] : feature_time)
            std::cout << "feature_time_s " << t << " " << kind << "\n";
        std::cout << "nonplanar_extrude_time_s " << np_extrude << "\n"
                  << "nonplanar_other_time_s " << np_other << "\n";
        if (! args.json.empty()) {
            record["text_summary"]["nonplanar_extrude_time_s"] = text_value(np_extrude);
            record["text_summary"]["nonplanar_other_time_s"] = text_value(np_other);
            if (!feature_time.empty())
                record["text_summary"]["feature_time_s"] = text_value(feature_time.begin()->second) + " " + feature_time.begin()->first;
        }
        if (calib_plate) {
            std::cout << "calib_test " << args.calib << "\n" << "calib_objects " << calib_plate->objects.size() << "\n";
            for (const auto &object : calib_plate->objects)
                std::cout << "calib_object " << object.center.x() << " " << object.center.y() << " " << object.name << "\n";
            const auto test = *Nonplanar::Calibration::parse_test(args.calib);
            if (test == Nonplanar::Calibration::Test::Clearance || test == Nonplanar::Calibration::Test::Verifier) {
                const double gap = config.opt_float("layer_height");
                std::cout << "calib_head " << calib_head.id << "\n"
                          << "calib_model_first_touch " << Nonplanar::Calibration::predicted_first_touch(calib_head, gap) << "\n";
                for (const auto &step : Nonplanar::Calibration::clearance_steps()) {
                    std::string component;
                    const double c = Nonplanar::Calibration::step_clearance(calib_head, step, gap, &component);
                    std::cout << "calib_step " << step.slope_deg << " " << step.relief << " clearance " << c << " " << component << "\n";
                }
            }
            if (args.calib_fit >= 0) {
                const double gap = config.opt_float("layer_height");
                const std::string id = calib_head.id + "-calibrated";
                const auto fit = Nonplanar::Calibration::fit_clearance(calib_head, args.calib_fit, gap, id, calib_head.name + " (calibrated)");
                Nonplanar::HeadLibrary::instance().save_user_head(fit.head);
                Nonplanar::HeadLibrary::instance().reload();
                const Nonplanar::HeadEnvelope *loaded = Nonplanar::HeadLibrary::instance().find(id);
                PrintConfig selected;
                selected.apply(config, true);
                selected.nonplanar_head.value = id;
                selected.nonplanar_head_snapshot.value.clear();
                std::cout << "calib_fit_shift_mm " << fit.shift << "\n"
                          << "calib_fit_slope_cap_deg " << fit.slope_cap_deg << "\n"
                          << "calib_fit_band_height_mm " << fit.band_height << "\n"
                          << "calib_fit_gantry_floor_mm " << fit.gantry_floor << "\n"
                          << "calib_fit_model_first_touch " << Nonplanar::Calibration::predicted_first_touch(fit.head, gap) << "\n"
                          << "calib_roundtrip_head " << (loaded != nullptr && *loaded == fit.head && Nonplanar::resolve_head_envelope(selected) == fit.head ? "ok" : "MISMATCH") << "\n"
                          << "calib_roundtrip_head_file " << Nonplanar::HeadLibrary::instance().source_path(id) << "\n";
                if (loaded != nullptr)
                    std::cout << "calib_fit_verifier_first_touch "
                              << Nonplanar::Calibration::verifier_first_touch(*calib_plate, result, *loaded, *make_tool_kinematics(print.config())) << "\n";
            }
            if (! args.calib_picks.empty()) {
                std::vector<int> picks;
                for (size_t pos = 0; pos <= args.calib_picks.size();) {
                    const size_t end = std::min(args.calib_picks.find(',', pos), args.calib_picks.size());
                    picks.push_back(std::atoi(args.calib_picks.substr(pos, end - pos).c_str()));
                    pos = end + 1;
                }
                DynamicPrintConfig written;
                if (test == Nonplanar::Calibration::Test::TiltFlow)
                    written.set_key_value("nonplanar_tilt_flow", new ConfigOptionFloats(Nonplanar::Calibration::tilt_flow_result(
                        config.option<ConfigOptionFloats>("nonplanar_tilt_flow")->values, picks)));
                else if (test == Nonplanar::Calibration::Test::CurvedTop && picks.size() == 2)
                    written = Nonplanar::Calibration::curved_top_result(picks[0], picks[1]);
                DynamicPrintConfig saved = config;
                saved.apply(written);
                const std::string path = (datadir / "calib_roundtrip.json").string();
                saved.save_to_json(path, "calibrated", "User", "2.0.0.0");
                DynamicPrintConfig                 loaded;
                std::map<std::string, std::string> key_values;
                std::string                        reason;
                loaded.load_from_json(path, ForwardCompatibilitySubstitutionRule::EnableSilent, key_values, reason);
                bool same = ! written.empty();
                for (const std::string &key : written.keys()) {
                    std::cout << "calib_written " << key << " = " << written.opt_serialize(key) << "\n";
                    same = same && loaded.has(key) && *loaded.option(key) == *written.option(key);
                }
                std::cout << "calib_roundtrip_config " << (same ? "ok" : "MISMATCH") << "\n";
            }
            if (test == Nonplanar::Calibration::Test::Verifier) {
                ClearanceReport report;
                const int first = Nonplanar::Calibration::verifier_first_touch(*calib_plate, result, calib_head,
                                                                               *make_tool_kinematics(print.config()), &report);
                std::cout << "calib_verifier_first_touch " << first << "\n"
                          << "calib_verifier_hit_moves " << report.hit_moves << "\n";
                std::vector<size_t> hits(calib_plate->objects.size(), 0);
                std::vector<float>  worst(calib_plate->objects.size(), 0.f);
                for (size_t i = 0; i < report.per_move.size() && i < result.moves.size(); ++i) {
                    if (!(report.per_move[i] < 0.f) || result.moves[i].type != EMoveType::Extrude)
                        continue;
                    const Vec2d p(result.moves[i].position.x(), result.moves[i].position.y());
                    size_t best = 0;
                    for (size_t k = 1; k < calib_plate->objects.size(); ++k)
                        if ((p - calib_plate->objects[k].center).norm() < (p - calib_plate->objects[best].center).norm())
                            best = k;
                    ++hits[best];
                    worst[best] = std::min(worst[best], report.per_move[i]);
                }
                for (size_t k = 0; k < hits.size(); ++k)
                    std::cout << "calib_verifier_step " << k + 1 << " hits " << hits[k] << " worst " << worst[k] << "\n";
                for (const ClearanceHit &hit : report.worst)
                    std::cout << "calib_verifier_hit " << hit.component << " " << hit.penetration << " at " << hit.tip.x() << "," << hit.tip.y() << "," << hit.tip.z() << "\n";
            }
        }
        progress("verify", 0);
        if (args.verify) {
            ClearanceParams params;
            params.head               = Nonplanar::resolve_head_envelope(print.config());
            params.contact_bar        = true;
            params.cone_tolerance     = curved_tops_cone_tolerance(print);
            result.clearance_report   = verify_clearance(result, params, *make_tool_kinematics(print.config()));
        }
        if (!args.moves_out.empty() && args.moves_extended)
            write_moves(args.moves_out, result, args.filaments, print_statistics_json(print, result), true);
        if (!args.tip_out.empty()) {
            const auto       kinematics = make_tool_kinematics(print.config());
            const bool       rotary     = kinematics->rotary_count() > 0;
            boost::nowide::ofstream tips(args.tip_out);
            tips << "# ax ay az bx by bz nx ny nz width height part\n";
            for (size_t i = 1; i < result.moves.size(); ++i) {
                const auto &m = result.moves[i];
                if (m.type != EMoveType::Extrude)
                    continue;
                const Vec3f &a    = result.moves[i - 1].position, &b = m.position;
                const Vec3d  axis = rotary && result.has_joints() ? kinematics->forward(result.joints[i]).axis : Vec3d::UnitZ();
                const ExtrusionRole r = m.extrusion_role;
                const bool part = !(r == erSkirt || r == erBrim || r == erWipeTower || r == erCustom || r == erNone ||
                                    r == erSupportMaterial || r == erSupportMaterialInterface || r == erSupportTransition);
                tips << a.x() << ' ' << a.y() << ' ' << a.z() << ' ' << b.x() << ' ' << b.y() << ' ' << b.z() << ' '
                     << axis.x() << ' ' << axis.y() << ' ' << axis.z() << ' ' << m.width << ' ' << m.height << ' ' << int(part) << '\n';
            }
        }
        if (!args.dump_field.empty()) {
            for (size_t i = 0; i < print.objects().size(); ++i) {
                const PrintObject &object = *print.objects()[i];
                Nonplanar::NpField file;
                bool               found = false;
                for (const Nonplanar::ConformalPatch &patch : object.conformal().patches)
                    if (const auto *of = dynamic_cast<const Nonplanar::OrientationField *>(patch.field.get());
                        patch.kind == Nonplanar::PatchKind::Field && of != nullptr) {
                        file  = of->to_npfield();
                        found = true;
                        break;
                    }
                if (!found) {
                    const indexed_triangle_set mesh = Nonplanar::slicing_frame_mesh(object);
                    BoundingBoxf3              box;
                    for (const stl_vertex &v : mesh.vertices)
                        box.merge(v.cast<double>());
                    file = Nonplanar::planar_npfield(box, 1., object.config().layer_height.value, print.config().initial_layer_print_height.value);
                }
                Nonplanar::to_plate_frame(file, Nonplanar::plate_offset(object));
                file.machine = args.printer;
                const std::string path = print.objects().size() == 1 ? args.dump_field :
                                                                       args.dump_field + "." + std::to_string(i) + ".npz";
                Nonplanar::save_npfield(path, file);
                std::cout << "field_dumped " << path << " " << file.solver << " " << file.dims[0] << "x" << file.dims[1] << "x" << file.dims[2] << "\n";
            }
        }
        progress("verify", 100);
        const ClearanceReport &clearance = result.clearance_report;
        std::cout << "clearance_checked " << clearance.ran << "\n";
        if (clearance.ran) {
            std::cout << "clearance_hit_moves " << clearance.hit_moves << "\n"
                      << "clearance_machine_hit_moves " << clearance.machine_hit_moves << "\n"
                      << "clearance_cone_graze_moves " << clearance.cone_graze_moves << "\n"
                      << "clearance_tolerated_moves " << clearance.tolerated_moves << "\n"
                      << "clearance_land_strict_moves " << clearance.land_strict_moves << "\n"
                      << "clearance_min_mm " << clearance.min_clearance << "\n";
            for (const ClearanceHit &hit : clearance.worst)
                std::cout << "clearance_hit line " << hit.gcode_line << " " << hit.component << " " << hit.penetration << " mm\n";
            for (const std::string &omission : clearance.omissions)
                std::cout << "clearance_omission " << omission << "\n";
            std::cout << "clearance_hit_classes land " << clearance.hit_land_moves << " cone " << clearance.hit_cone_moves << " body "
                      << clearance.hit_body_moves << " travel " << clearance.hit_travel_moves << " extrusion " << clearance.hit_extrusion_moves << "\n";
        }
        if (! args.json.empty()) {
            auto &text = record["text_summary"];
            text["clearance_checked"] = text_value(clearance.ran);
            if (clearance.ran) {
                text["clearance_hit_moves"] = text_value(clearance.hit_moves);
                text["clearance_machine_hit_moves"] = text_value(clearance.machine_hit_moves);
                text["clearance_tolerated_moves"]   = text_value(clearance.tolerated_moves);
                text["clearance_cone_graze_moves"] = text_value(clearance.cone_graze_moves);
                text["clearance_body_hit_moves"] = text_value(clearance.hit_body_moves);
                text["clearance_min_mm"] = text_value(clearance.min_clearance);
                if (!clearance.worst.empty()) {
                    const auto &hit = clearance.worst.front();
                    text["clearance_hit"] = "line " + text_value(hit.gcode_line) + " " + hit.component + " " + text_value(hit.penetration) + " mm";
                }
                if (!clearance.omissions.empty()) text["clearance_omission"] = clearance.omissions.front();
            }
            if (!args.gcode_3mf.empty()) text["gcode_3mf"] = args.gcode_3mf;
        }
        const Nonplanar::NonplanarReport report = result.nonplanar_report.objects.empty() ? Nonplanar::make_report(print, result, true) :
                                                                                           result.nonplanar_report;
        for (size_t i = 0; i < report.objects.size(); ++i) {
            const Nonplanar::ReportObject &object = report.objects[i];
            if (print.objects()[i]->auto_z_contouring()) {
                std::cout << "nonplanar_auto_z_contouring " << object.name << "\n";
                if (!args.json.empty())
                    record["auto_z_contouring_objects"].push_back(object.name);
            }
            if (!object.oneshot.empty()) {
                std::cout << "nonplanar_oneshot " << object.oneshot << "\n"
                          << "nonplanar_oneshot_student " << object.oneshot_student << " polish_k " << object.oneshot_polish_k << "\n";
                if (object.oneshot_max_time_ratio_override > 0.)
                    std::cout << "nonplanar_oneshot_max_time_ratio_override " << object.oneshot_max_time_ratio_override << "\n";
                if (!args.json.empty()) {
                    record["oneshot"] = { { "decision", object.oneshot }, { "student", object.oneshot_student },
                                          { "polish_k", object.oneshot_polish_k } };
                    if (object.oneshot_max_time_ratio_override > 0.)
                        record["oneshot"]["max_time_ratio_override"] = object.oneshot_max_time_ratio_override;
                }
            }
            std::cout << "nonplanar_object " << object.name << "\n"
                      << "nonplanar_top_patches " << object.top_patches << "\n"
                      << "nonplanar_bands " << object.bands << "\n"
                      << "nonplanar_top_footprint_mm2 " << object.curved_top << "\n";
            if (! args.json.empty()) {
                auto &text = record["text_summary"];
                if (!text.contains("nonplanar_top_patches")) {
                    text["nonplanar_top_patches"] = text_value(object.top_patches);
                    text["nonplanar_bands"] = text_value(object.bands);
                    text["nonplanar_top_footprint_mm2"] = text_value(object.curved_top);
                }
                for (const Nonplanar::ReportFallback &f : report.fallbacks)
                    if (f.object == i && !text.contains("nonplanar_fallback"))
                        text["nonplanar_fallback"] = f.reason + " " + text_value(f.area) + " patch " + text_value(f.patch);
            }
            for (size_t r = 0; r < object.regions.size(); ++r)
                std::cout << "nonplanar_region " << r << " " << object.regions[r] << "\n";
            for (const Nonplanar::ReportPatch &patch : report.patches)
                if (patch.object == i)
                    std::cout << "nonplanar_patch " << patch.id << " kind " << patch.kind << " region " << patch.region << " band "
                              << patch.band << " shells " << patch.shells << " xy " << patch.center.x() << "," << patch.center.y() << "\n";
            for (const Nonplanar::PatchBase &b : print.objects()[i]->conformal().base) {
                std::cout << "nonplanar_base patches ";
                for (size_t k = 0; k < b.patches.size(); ++k)
                    std::cout << (k ? "," : "") << b.patches[k];
                std::cout << " area " << b.area << " on_csparse " << b.share[0] << " on_sparse " << b.share[1] << " on_solid " << b.share[2]
                          << " on_air " << b.share[3] << " on_underside " << b.share[4] << " span " << b.span << " reach_p50 " << b.reach_p50
                          << " solid_min " << b.solid_min << " solid_p50 " << b.solid_p50 << " slope_p90 " << b.slope_p90 << " sag_rms " << b.sag_rms
                          << " sag_max " << b.sag_max
                          << " solid_layers " << b.solid_layers << " bridged " << b.bridged << "\n";
            }
            for (const Nonplanar::ReportFallback &f : report.fallbacks)
                if (f.object == i)
                    std::cout << "nonplanar_fallback " << f.reason << " " << f.area << " patch " << f.patch << "\n";
        }
        if (! args.json.empty()) {
            progress("json", 0);
            slice_json_results(record, args, print, result, model, config);
            record["nonplanar_report"] = nlohmann::json::parse(report.to_json());
            if (!args.plate.empty()) {
                boost::nowide::ifstream in(args.plate);
                const auto layout = nlohmann::json::parse(in);
                for (size_t i = 0; i < print.objects().size(); ++i) {
                    const ModelObject *object = print.objects()[i]->model_object();
                    const auto it = std::find_if(model.objects.begin(), model.objects.end(),
                        [object](const ModelObject *source) { return source->id() == object->id(); });
                    const auto &entry = layout.at("objects").at(size_t(it - model.objects.begin()));
                    if (entry.contains("settings")) {
                        auto &settings = record["objects"][i]["settings"] = nlohmann::json::object();
                        for (const auto &[key, value] : entry.at("settings").items())
                            settings[key] = object->config.has(key) ? nlohmann::json(object->config.opt_serialize(key)) : nlohmann::json(nullptr);
                    }
                }
            }
            record["totals"]["move_count"] = result.moves.size();
            if (args.floating && !print.objects().empty()) {
                const auto                t_floating = std::chrono::steady_clock::now();
                const Nonplanar::Floating floating   = Nonplanar::floating_road(result, *print.objects().front());
                record["totals"]["floating"] = {{"part_mm", floating.part}, {"air_mm", floating.air},
                                                {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - t_floating).count()}};
            }
            slice_model_context(record, print, config, saved_instances);
            record["status"] = "sliced";
            write_slice_json(args.json, record);
            progress("json", 100);
        }
        if (! args.gcode_3mf.empty()) {
            progress("package", 0);
            write_gcode_3mf(args, model, print, config, result, source_plate.get());
            std::cout << "gcode_3mf " << args.gcode_3mf << "\n";
            progress("package", 100);
        }
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        if (! args.json.empty()) {
            record["status"] = "error";
            record["totals"]["aborts"].push_back(e.what());
            record["totals"]["exceptions"].push_back(e.what());
            try { write_slice_json(args.json, record); }
            catch (const std::exception &json_error) { std::cerr << "error: " << json_error.what() << "\n"; }
        }
        return 1;
    }
    return 0;
}
