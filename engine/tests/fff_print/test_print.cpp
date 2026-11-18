#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/catch_message.hpp>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"
#include <cstddef>
#include "libslic3r/Surface.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <catch2/catch_all.hpp>

#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Support/TreeModelVolumes.hpp"
#include "libslic3r/Support/TreeSupportCommon.hpp"
#include "libslic3r/Support/BeltFloorContext.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Polyline.hpp"
#include <limits>
#include <cmath>
#include <map>
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/LifecycleEvents.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <cstdlib>
#include <sstream>
#include <limits>
#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/SurfaceCollection.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("Timelapse g-code is emitted once per layer for Bambu and non-Bambu printers", "[Print][Regression]")
{
    struct PrinterCase {
        std::string name;
        std::string structure;
        bool        is_bbl;
    };
    const PrinterCase printer = GENERATE(from_range(std::vector<PrinterCase>{
        { "non-BBL undefined", "undefine", false },
        { "non-BBL CoreXY",    "corexy",   false },
        { "non-BBL i3",        "i3",       false },
        { "non-BBL H-Bot",     "hbot",     false },
        { "non-BBL Delta",     "delta",    false },
        { "Bambu CoreXY",      "corexy",   true },
        { "Bambu i3",          "i3",       true },
    }));
    INFO("printer: " << printer.name);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "initial_layer_print_height", 0.2 },
        { "layer_change_gcode",          ";TEST_LAYER_CHANGE" },
        { "layer_height",                0.2 },
        { "printer_structure",           printer.structure },
        { "spiral_mode",                 false },
        { "time_lapse_gcode",            "TIMELAPSE_TAKE_FRAME" },
    });
    Print print;
    print.is_BBL_printer() = printer.is_bbl;
    Model model;
    init_print({ cube(20) }, print, model, config);
    const std::string gcode = Slic3r::Test::gcode(print);

    const auto count = [&gcode](std::string_view token) {
        size_t occurrences = 0;
        size_t pos = 0;
        while ((pos = gcode.find(token, pos)) != std::string::npos) {
            ++occurrences;
            pos += token.size();
        }
        return occurrences;
    };

    const size_t layer_changes = count("\n;TEST_LAYER_CHANGE\n");
    REQUIRE(layer_changes > 0);
    CHECK(count("\nTIMELAPSE_TAKE_FRAME\n") == layer_changes);
}

SCENARIO("Changing the number of solid shell layers does not make all surfaces internal", "[Print]") {
    GIVEN("sliced 20mm cube and config with top_shell_layers = 2 and bottom_shell_layers = 1") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
		config.set_deserialize_strict({
            { "top_shell_layers",           2 },
            { "bottom_shell_layers",        1 },
            { "layer_height",               0.25 },
            { "initial_layer_print_height", 0.25 }
			});
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({cube(20)}, print, model, config);
		auto test_is_solid_infill = [&print](size_t obj_id, size_t layer_id) {
		    const Layer &layer = *(print.objects().at(obj_id)->get_layer((int)layer_id));
		    for (const LayerRegion *region : layer.regions()) {
		        for (const Surface &surface : region->fill_surfaces.surfaces)
		            CHECK(surface.is_solid());
		    }
		};
        print.process();
        test_is_solid_infill(0,  0);
        test_is_solid_infill(0, 79);
        test_is_solid_infill(0, 78);
        WHEN("Model is re-sliced with top_shell_layers == 3") {
			config.set("top_shell_layers", 3);
			print.apply(model, config);
            print.process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(0, 79);
                test_is_solid_infill(0, 78);
                test_is_solid_infill(0, 77);
            }
        }
    }
}

namespace {

void build_cubes(Slic3r::Model& model, Slic3r::Print& print,
                 DynamicPrintConfig config, int n, bool overlap)
{
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));

    for (int i = 0; i < n; ++i) {
        ModelObject* object = model.add_object();
        object->add_volume(cube(20));
        ModelInstance* inst = object->add_instance();
        inst->set_offset(Vec3d(overlap ? 0.0 : i * 60.0, 0.0, 0.0));
    }
    for (ModelObject* mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
}

StringObjectException validate_cubes(const DynamicPrintConfig& config,
                                     std::vector<StringObjectException>& warnings,
                                     int n = 1, bool overlap = false)
{
    Slic3r::Model model;
    Slic3r::Print print;
    build_cubes(model, print, config, n, overlap);
    return print.validate(&warnings);
}

size_t count_opt_key(const std::vector<StringObjectException>& warnings, const std::string& key)
{
    return std::count_if(warnings.begin(), warnings.end(),
        [&](const StringObjectException& w) { return w.opt_key == key; });
}

void trigger_acceleration_warning(DynamicPrintConfig& c)
{
    c.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats{ 100. });
    c.set_key_value("default_acceleration", new ConfigOptionFloatsNullable{ 100000. });
}

void trigger_jerk_warning(DynamicPrintConfig& c)
{
    c.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats{ 0. });
    c.set_key_value("machine_max_jerk_x", new ConfigOptionFloats{ 1. });
    c.set_key_value("machine_max_jerk_y", new ConfigOptionFloats{ 1. });
    c.set_key_value("default_jerk", new ConfigOptionFloatsNullable{ 9999. });
}

void trigger_precise_wall_warning(DynamicPrintConfig& c)
{
    c.set_key_value("precise_outer_wall", new ConfigOptionBool(true));
    c.set_key_value("wall_sequence", new ConfigOptionEnum<WallSequence>(WallSequence::OuterInner));
}

}
namespace {

ModelObject* add_named_cube(Model& model, const std::string& name)
{
    ModelObject* obj = model.add_object();
    obj->name = name;
    obj->add_volume(make_cube(20.0, 20.0, 20.0));
    obj->add_instance();
    obj->ensure_on_bed();
    return obj;
}

std::string resolved_output_name(Model& model, const std::string& format, const std::string& filename_base = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filename_format", new ConfigOptionString(format));

    Print print;
    for (ModelObject* obj : model.objects)
        print.auto_assign_extruders(obj);
    print.apply(model, config);
    return print.output_filename(filename_base);
}

struct ScopedLifecycleHook
{
    explicit ScopedLifecycleHook(LifecycleHookFn hook) { set_lifecycle_hook_fn(std::move(hook)); }
    ~ScopedLifecycleHook() { set_lifecycle_hook_fn(nullptr); }
};

}

TEST_CASE("Slicing lifecycle events identify the model", "[Print][LifecycleEvents]")
{
    struct ObservedEvent {
        LifecycleEvent event;
        std::string id;
        std::string name;
    };
    std::vector<ObservedEvent> events;
    ScopedLifecycleHook hook([&](LifecycleEvent event, const LifecycleEventContext& ctx) {
        events.push_back({ event, ctx.id, ctx.name });
    });

    Print print;
    Model model;
    ModelInfo info;
    info.model_name = "Lifecycle test model";
    model.model_info = std::make_shared<ModelInfo>(std::move(info));
    init_print({cube(20)}, print, model);

    print.process();
    ScopedTemporaryFile temp(".gcode");
    print.export_gcode(temp.string(), nullptr, nullptr);
    GCodeProcessorResult result;
    print.export_gcode_from_previous_file(temp.string(), &result);

    const std::string expected_id = std::to_string(print.model().id().id);
    const std::vector<LifecycleEvent> expected_events = {
        LifecycleEvent::SliceStarted,
        LifecycleEvent::SliceGeometryFinished,
        LifecycleEvent::GCodeExportStarted,
        LifecycleEvent::GCodeExportFinished,
        LifecycleEvent::GCodeExportStarted,
        LifecycleEvent::GCodeExportFinished,
    };
    REQUIRE(events.size() == expected_events.size());
    for (size_t i = 0; i < expected_events.size(); ++i) {
        CHECK(events[i].event == expected_events[i]);
        CHECK(events[i].id == expected_id);
        CHECK(events[i].name == "Lifecycle test model");
    }
}

TEST_CASE("Slicing lifecycle event name is empty without model metadata", "[Print][LifecycleEvents]")
{
    std::string event_id;
    std::string event_name = "unset";
    ScopedLifecycleHook hook([&](LifecycleEvent event, const LifecycleEventContext& ctx) {
        if (event == LifecycleEvent::SliceStarted) {
            event_id = ctx.id;
            event_name = ctx.name;
        }
    });

    Print print;
    Model model;
    init_print({cube(20)}, print, model);
    print.process();

    CHECK(event_id == std::to_string(print.model().id().id));
    CHECK(event_name.empty());
}

TEST_CASE("Output filenames with numeric statistics fail before slicing finishes", "[Print][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filename_format", new ConfigOptionString("{int(total_weight*10) / 10.0}"));

    Print print;
    Model model;
    init_print({cube(20)}, print, model, config);

    CHECK_THROWS_AS(print.output_filename(), PlaceholderParserError);
}

TEST_CASE("Print: {first_object_name} names the first printable object on the plate", "[Print]")
{
    Model model;

    SECTION("uses the object's name") {
        add_named_cube(model, "WidgetPart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "WidgetPart.gcode");
    }

    SECTION("picks the first when several objects are printable") {
        add_named_cube(model, "FirstPart");
        add_named_cube(model, "SecondPart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "FirstPart.gcode");
    }

    SECTION("skips objects outside the print volume (e.g. on another plate)") {
        add_named_cube(model, "OtherPlatePart")->instances.front()->print_volume_state = ModelInstancePVS_Fully_Outside;
        add_named_cube(model, "OnPlatePart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "OnPlatePart.gcode");
    }

    SECTION("is empty when the object has no name") {
        add_named_cube(model, "");
        CHECK(resolved_output_name(model, "part_{first_object_name}") == "part_.gcode");
    }
}

TEST_CASE("Print: {first_object_name} is not replaced by the saved-project file name", "[Print]")
{
    Model model;
    add_named_cube(model, "WidgetPart");
    CHECK(resolved_output_name(model, "{first_object_name}", "SavedProject") == "WidgetPart.gcode");
}

TEST_CASE("Print::validate stacks independent warnings", "[Print][validate]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    trigger_precise_wall_warning(config);
    trigger_acceleration_warning(config);

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings);

    CHECK(err.string.empty());
    CHECK(warnings.size() >= 2);
    CHECK(count_opt_key(warnings, "precise_outer_wall") == 1);
    for (const auto& w : warnings)
        CHECK(w.is_warning);
}

TEST_CASE("Print::validate coalesces motion-ability warnings into one", "[Print][validate]")
{
    DynamicPrintConfig accel_only = DynamicPrintConfig::full_print_config();
    trigger_acceleration_warning(accel_only);
    std::vector<StringObjectException> w_accel;
    CHECK(validate_cubes(accel_only, w_accel).string.empty());

    DynamicPrintConfig accel_and_jerk = DynamicPrintConfig::full_print_config();
    trigger_acceleration_warning(accel_and_jerk);
    trigger_jerk_warning(accel_and_jerk);
    std::vector<StringObjectException> w_both;
    CHECK(validate_cubes(accel_and_jerk, w_both).string.empty());

    CHECK(w_accel.size() >= 1);
    CHECK(w_both.size() == w_accel.size());
}

TEST_CASE("Print::validate reports the clumping-detection warning", "[Print][validate]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("enable_wrapping_detection", new ConfigOptionBool(true));

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings);

    CHECK(err.string.empty());
    CHECK(count_opt_key(warnings, "enable_prime_tower") == 1);
}

TEST_CASE("Print::validate concatenates layered-clearance collisions into one warning", "[Print][validate]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings, 3, true);

    CHECK(err.string.empty());
    auto is_layered = [](const StringObjectException& w) {
        return w.type == STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT; };
    REQUIRE(std::count_if(warnings.begin(), warnings.end(), is_layered) == 1);
    auto it = std::find_if(warnings.begin(), warnings.end(), is_layered);
    CHECK(it->string.find('\n') != std::string::npos);
}

TEST_CASE("Print::validate tolerates a null warnings pointer", "[Print][validate]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    trigger_precise_wall_warning(config);
    trigger_acceleration_warning(config);

    Slic3r::Model model;
    Slic3r::Print print;
    build_cubes(model, print, config, 1, false);

    StringObjectException err = print.validate();
    CHECK(err.string.empty());
}

TEST_CASE("Purge tower selection keeps ordinary printers on the classic path", "[Print][PurgeTower][Regression]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "belt_printer",            0 },
        { "enable_prime_tower",      1 },
        { "enable_belt_purge_tower", 1 }
    });
    config.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(TimelapseType::tlSmooth));

    Model model;
    Print print;
    build_cubes(model, print, config, 1, false);

    CHECK(print.has_wipe_tower());
    CHECK_FALSE(print.has_belt_purge_tower());
}

TEST_CASE("Belt purge planning requires its managed purge object", "[Print][PurgeTower][Regression]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "belt_printer",             1 },
        { "enable_belt_purge_tower", 1 }
    });

    Model model;
    Print print;
    build_cubes(model, print, config, 1, false);
    CHECK_FALSE(print.has_belt_purge_tower());

    model.objects.front()->config.set_key_value("belt_purge_tower_object", new ConfigOptionBool(true));
    print.apply(model, config);
    CHECK(print.has_belt_purge_tower());
    CHECK_FALSE(print.has_wipe_tower());
}

TEST_CASE("Belt purge tower enabled without a tower object warns", "[Print][PurgeTower][belt]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "belt_printer",            1 },
        { "enable_belt_purge_tower", 1 },
        { "layer_change_gcode",      "G92 E0\n" }
    });
    auto purge_warnings = [](Print &print) {
        std::vector<StringObjectException> warnings;
        print.validate(&warnings);
        return std::count_if(warnings.begin(), warnings.end(), [](const StringObjectException &w) {
            return w.opt_key == "enable_belt_purge_tower";
        });
    };

    Model model;
    Print print;
    build_cubes(model, print, config, 2, false);
    model.objects[1]->config.set_key_value("extruder", new ConfigOptionInt(2));
    print.apply(model, config);
    REQUIRE(print.extruders().size() > 1);
    CHECK(purge_warnings(print) == 1);

    model.objects.front()->config.set_key_value("belt_purge_tower_object", new ConfigOptionBool(true));
    print.apply(model, config);
    CHECK(purge_warnings(print) == 0);
}

TEST_CASE("Belt purge rejects multiple managed purge objects", "[Print][PurgeTower][Regression]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "belt_printer",             1 },
        { "enable_belt_purge_tower", 1 }
    });

    Model model;
    Print print;
    build_cubes(model, print, config, 2, false);
    for (ModelObject *object : model.objects)
        object->config.set_key_value("belt_purge_tower_object", new ConfigOptionBool(true));
    print.apply(model, config);

    CHECK_FALSE(print.validate().string.empty());
}

TEST_CASE("A default slice emits perimeter, infill, and skirt", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "z_hop",                      0 }
    });
    CHECK(role_passes(gcode, "perimeter") > 0);
    CHECK(role_passes(gcode, "infill")    > 0);
    CHECK(role_passes(gcode, "skirt")     > 0);
    CHECK_THAT(max_z(gcode), Catch::Matchers::WithinAbs(20.0, 1e-4));
}

TEST_CASE("G-code lists the resolved extrusion-width settings", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, { { "initial_layer_line_width", 0 } });
    CHECK(gcode.find("; external perimeters extrusion width") != std::string::npos);
    CHECK(gcode.find("; perimeters extrusion width")          != std::string::npos);
    CHECK(gcode.find("; infill extrusion width")              != std::string::npos);
    CHECK(gcode.find("; solid infill extrusion width")        != std::string::npos);
    CHECK(gcode.find("; top infill extrusion width")          != std::string::npos);
    CHECK(gcode.find("; support material extrusion width")    == std::string::npos);
    CHECK(gcode.find("; first layer extrusion width")         == std::string::npos);
    CHECK(gcode.find("; layer_height")                        != std::string::npos);
    CHECK(gcode.find("; sparse_infill_density")               != std::string::npos);

    const std::string with_support = slice({ cube(20) }, {
        { "initial_layer_line_width", 0 }, { "enable_support", true }, { "raft_layers", 3 },
    });
    CHECK(with_support.find("; support material extrusion width") != std::string::npos);

    const std::string with_first_layer = slice({ cube(20) }, { { "initial_layer_line_width", "0.5" } });
    CHECK(with_first_layer.find("; first layer extrusion width") != std::string::npos);
}

TEST_CASE("gcode_skip_config_block omits the resolved-settings comment block", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "gcode_skip_config_block", true },
        { "gcode_comments",         true },
    });
    CHECK(gcode.find("; CONFIG_BLOCK_START")     == std::string::npos);
    CHECK(gcode.find("; CONFIG_BLOCK_END")       == std::string::npos);
    CHECK(gcode.find("; layer_height =")         == std::string::npos);
    CHECK(gcode.find("; fill_density =")         == std::string::npos);
    CHECK(gcode.find("; HEADER_BLOCK_START")     != std::string::npos);
    CHECK(gcode.find("; EXECUTABLE_BLOCK_START") != std::string::npos);
}

TEST_CASE("The estimated printing time comment stays near the end of the file", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, {});
    const size_t config_block_end = gcode.find("; CONFIG_BLOCK_END");
    const size_t filament_stats   = gcode.find("; filament used [mm]");
    const size_t time_comment     = gcode.find("estimated printing time");
    REQUIRE(config_block_end != std::string::npos);
    REQUIRE(filament_stats != std::string::npos);
    REQUIRE(time_comment != std::string::npos);
    CHECK(filament_stats > config_block_end);
    CHECK(time_comment > filament_stats);

    const size_t line_start = gcode.rfind('\n', time_comment) + 1;
    const size_t trailing_lines = std::count(gcode.begin() + line_start, gcode.end(), '\n');
    CHECK(trailing_lines <= 5);
}

TEST_CASE("Custom G-code placeholders are substituted", "[Print]")
{
    CHECK(slice({ cube(20) }, { { "machine_start_gcode", "; Extruder [current_extruder]" } })
              .find("; Extruder 0") != std::string::npos);

    const std::string end_gcode = slice({ cube(20) }, {
        { "machine_end_gcode",          "; Layer_num [layer_num]\n; Layer_z [layer_z]" },
        { "layer_height",               0.1 },
        { "initial_layer_print_height", 0.1 },
    });
    CHECK(end_gcode.find("; Layer_num 199") != std::string::npos);
    CHECK(end_gcode.find("; Layer_z 20")    != std::string::npos);

    CHECK(slice_two_cubes_arranged({
                    { "print_sequence",           "by object" },
                    { "printing_by_object_gcode", "; between-object-gcode" },
                })
              .find("; between-object-gcode") != std::string::npos);

    const std::string per_layer = slice_two_cubes_arranged({
        { "print_sequence",             "by object" },
        { "layer_change_gcode",         ";Layer:[layer_num] ([layer_z] mm)" },
        { "layer_height",               0.1 },
        { "initial_layer_print_height", 0.1 },
    });
    CHECK(per_layer.find(";Layer:199 ") != std::string::npos);
    CHECK(per_layer.find(";Layer:399 ") != std::string::npos);
}

TEST_CASE("export_gcode writes G-code without a result pointer", "[Print][export_gcode]")
{
    Print print;
    Model model;
    Slic3r::Test::init_print({cube(20)}, print, model);
    print.process();

    SECTION("non-BBL printer") {}
    SECTION("BBL printer") { print.is_BBL_printer() = true; }

    ScopedTemporaryFile temp(".gcode");
    REQUIRE_NOTHROW(print.export_gcode(temp.string(), nullptr, nullptr));

    std::ifstream in(temp.string());
    const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    REQUIRE_FALSE(gcode.empty());
}

TEST_CASE("Exporting a sliced print again gives the same G-code", "[Print][export_gcode][Regression]")
{
    const int instances = GENERATE(1, 3);
    CAPTURE(instances);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    TestMesh           mesh   = TestMesh::ipadstand;
    SECTION("infill reversed by chaining") { config.set_deserialize_strict({{"sparse_infill_pattern", "gyroid"}}); }
    SECTION("support reversed by chaining") {
        mesh = TestMesh::overhang;
        config.set_deserialize_strict({{"enable_support", true}, {"support_interface_pattern", "concentric"}});
    }
    Print print;
    Model model;
    Slic3r::Test::init_print({Slic3r::Test::mesh(mesh)}, print, model, config, nullptr, true, instances);

    const auto export_without_timestamp = [&print]() {
        std::string gcode = Slic3r::Test::gcode(print);
        const size_t line = gcode.find("; generated by ");
        REQUIRE(line != std::string::npos);
        gcode.erase(line, gcode.find('\n', line) - line);
        const size_t provenance = gcode.find("; pancakeslicer_build = ");
        if (provenance != std::string::npos)
            gcode.erase(provenance, gcode.find('\n', provenance) - provenance + 1);
        return gcode;
    };
    const std::string first  = export_without_timestamp();
    const std::string second = export_without_timestamp();

    const size_t diff       = std::mismatch(first.begin(), first.end(), second.begin(), second.end()).first - first.begin();
    const size_t line_start = diff == 0 ? 0 : first.rfind('\n', diff - 1) + 1;
    INFO("first export:  " << first.substr(line_start, first.find('\n', diff) - line_start));
    INFO("second export: " << second.substr(line_start, second.find('\n', diff) - line_start));
    CHECK(diff == first.size());
    CHECK(first.size() == second.size());
}

TEST_CASE("Sequential printing follows model order", "[Print]")
{
    const std::string gcode = Slic3r::Test::slice({ cube(20), Slic3r::make_cube(20, 20, 10) }, {
        { "print_sequence",             "by object" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "z_hop",                      0 }
    });

    double first_object_peak_z = 0.0;
    double running_peak        = 0.0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (first_object_peak_z != 0.0 || !line.extruding(self)) return;
        if (running_peak > 1.0 && self.z() < 1.0)
            first_object_peak_z = running_peak;
        else
            running_peak = std::max(running_peak, static_cast<double>(self.z()));
    });

    REQUIRE_THAT(first_object_peak_z, Catch::Matchers::WithinAbs(20.0, 0.3));
}

TEST_CASE("Sequential printing publishes the nozzle group result", "[Print][MultiNozzle]")
{
    SECTION("process() publishes the result") {
        Print print;
        Model model;
        place_two_cubes_apart(60.0, { { "print_sequence", "by object" } }, print, model);
        print.process();
        REQUIRE(print.get_layered_nozzle_group_result() != nullptr);
    }

    SECTION("start g-code can index the per-nozzle diameter table") {
        const std::string gcode = slice_two_cubes_arranged({
            { "print_sequence",      "by object" },
            { "machine_start_gcode", "{if nozzle_diameter_at_nozzle_id[0] > 0}; SEQ-ND-OK\n{endif}" },
        });
        CHECK(gcode.find("; SEQ-ND-OK") != std::string::npos);
    }
}

TEST_CASE("Belt printers never start a scarf seam below the layer", "[Print][belt][Seam]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "belt_printer",               1 },
        { "belt_slice_rotation",        "x" },
        { "belt_slice_rotation_angle",  45 },
        { "gcode_remap_x",              "rev_x" },
        { "gcode_remap_y",              "pos_z" },
        { "gcode_remap_z",              "pos_y" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops",                0 },
        { "top_shell_layers",           0 },
        { "bottom_shell_layers",        1 },
        { "wall_loops",                 2 },
        { "seam_position",              "back" },
        { "seam_slope_type",            "external" },
        { "seam_slope_inner_walls",     1 },
        { "seam_slope_start_height",    0 },
        { "z_hop",                      0 },
        { "machine_start_gcode",        "T[initial_tool]\n" },
        { "layer_change_gcode",         "G92 E0\n" },
    });
    const std::string gcode = slice({ cube(20) }, config);
    REQUIRE(! gcode.empty());

    double last_z = std::numeric_limits<double>::lowest();
    double worst_backstep = 0.;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (! line.cmd_is("G1") || ! line.has_z())
            return;
        const double z = line.z();
        if (last_z != std::numeric_limits<double>::lowest())
            worst_backstep = std::max(worst_backstep, last_z - z);
        last_z = z;
    });
    CHECK(worst_backstep < 0.2);
}

TEST_CASE("Belt printers refuse an object taller than the gantry clearance", "[Print][belt]")
{
    auto belt_config = [](double printable_height) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "belt_printer",               1 },
            { "belt_slice_rotation",        "x" },
            { "belt_slice_rotation_angle",  45 },
            { "gcode_remap_x",              "rev_x" },
            { "gcode_remap_y",              "pos_z" },
            { "gcode_remap_z",              "pos_y" },
            { "printable_height",           printable_height },
            { "skirt_loops",                0 },
            { "layer_change_gcode",         "G92 E0\n" },
        });
        return config;
    };

    SECTION("a 20 mm cube fits under 50 mm of clearance") {
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, belt_config(50));
        CHECK(print.validate().string.empty());
    }
    SECTION("a 60 mm cube does not") {
        Print print;
        Model model;
        init_print({ cube(60) }, print, model, belt_config(50));
        CHECK(print.validate().string.find("height") != std::string::npos);
    }
}

TEST_CASE("Belt printers keep the part fan off within the band above the belt", "[Print][belt][Cooling]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "belt_printer",                 1 },
        { "belt_slice_rotation",          "x" },
        { "belt_slice_rotation_angle",    45 },
        { "gcode_remap_x",                "rev_x" },
        { "gcode_remap_y",                "pos_z" },
        { "gcode_remap_z",                "pos_y" },
        { "layer_height",                 0.2 },
        { "initial_layer_print_height",   0.2 },
        { "skirt_loops",                  0 },
        { "z_hop",                        0 },
        { "close_fan_the_first_x_layers", 3 },
        { "full_fan_speed_layer",         0 },
        { "fan_min_speed",                100 },
        { "fan_max_speed",                100 },
        { "slow_down_layer_time",         1000 },
        { "fan_cooling_layer_time",       1001 },
        { "reduce_fan_stop_start_freq",   0 },
        { "machine_start_gcode",          "T[initial_tool]\n" },
        { "layer_change_gcode",           "G92 E0\n" },
    });
    const std::string gcode = slice({ cube(20) }, config);
    REQUIRE(! gcode.empty());

    CHECK(gcode.find(";_BELT_BAND") == std::string::npos);

    size_t in_band = 0, in_band_fan_on = 0, clear = 0, clear_fan_off = 0;
    int    layer   = -1;
    bool   fan_on  = false;
    double y       = 0.;
    std::istringstream lines(gcode);
    for (std::string line; std::getline(lines, line); ) {
        if (boost::starts_with(line, ";LAYER_CHANGE")) {
            ++ layer;
        } else if (boost::starts_with(line, "M107")) {
            fan_on = false;
        } else if (boost::starts_with(line, "M106")) {
            const size_t s = line.find('S');
            fan_on = s != std::string::npos && std::atof(line.c_str() + s + 1) > 0.;
        } else if (boost::starts_with(line, "G1 ")) {
            const size_t comment = line.find(';');
            const std::string cmd = line.substr(0, comment);
            const size_t ypos = cmd.find(" Y"), epos = cmd.find(" E");
            if (ypos == std::string::npos)
                continue;
            const double y_new     = std::atof(cmd.c_str() + ypos + 2);
            const bool   extruding = epos != std::string::npos && std::atof(cmd.c_str() + epos + 2) > 0.;
            if (extruding && layer >= 3) {
                if (std::max(y, y_new) < 0.45) {
                    ++ in_band;
                    in_band_fan_on += fan_on;
                } else if (std::min(y, y_new) > 5.) {
                    ++ clear;
                    clear_fan_off += ! fan_on;
                }
            }
            y = y_new;
        }
    }
    CHECK(in_band > 20);
    CHECK(in_band_fan_on == 0);
    CHECK(clear > 20);
    CHECK(clear_fan_off == 0);
}

TEST_CASE("Belt printers slice organic tree supports that reach the belt", "[Print][belt][Support]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "belt_printer",               1 },
        { "belt_slice_rotation",        "x" },
        { "belt_slice_rotation_angle",  45 },
        { "gcode_remap_x",              "rev_x" },
        { "gcode_remap_y",              "pos_z" },
        { "gcode_remap_z",              "pos_y" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops",                0 },
        { "z_hop",                      0 },
        { "enable_support",             1 },
        { "support_type",               "tree(auto)" },
        { "support_style",              "organic" },
        { "machine_start_gcode",        "T[initial_tool]\n" },
        { "layer_change_gcode",         "G92 E0\n" },
    });
    std::string gcode;
    REQUIRE_NOTHROW(gcode = slice({ TestMesh::overhang }, config));
    CHECK(! gcode.empty());
}

TEST_CASE("Slicing errors are reported per object with the object's name", "[Print]")
{
    Print print;
    Model model;
    init_print({Slic3r::Test::cube(20.)}, print, model);
    ModelObject *object = model.objects.front();
    object->name = "floating cube";
    object->instances.front()->set_offset(object->instances.front()->get_offset() + Vec3d(0., 0., 2.));
    print.apply(model, DynamicPrintConfig::full_print_config());
    print.set_status_silent();

    ScopedTemporaryFile temp(".gcode");
    std::string message;
    try {
        print.process();
        print.export_gcode(temp.string(), nullptr, nullptr);
        FAIL("slicing did not report the empty first layer");
    } catch (const SlicingErrors &errors) {
        REQUIRE(errors.errors_.size() == 1);
        message = print.slicing_errors_message(errors);
    }
    CHECK(message.rfind("floating cube: ", 0) == 0);
    CHECK(message.find("empty first layer") != std::string::npos);
}

static std::string gcode_body(const std::string &gcode)
{
    std::string      body;
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line); ) {
        line.erase(std::min(line.size(), line.find(';')));
        while (! line.empty() && line.back() == ' ')
            line.pop_back();
        if (! line.empty())
            body += line + '\n';
    }
    return body;
}

static DynamicPrintConfig belt_test_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "belt_printer",               1 },
        { "belt_slice_rotation",        "x" },
        { "belt_slice_rotation_angle",  45 },
        { "gcode_remap_x",              "rev_x" },
        { "gcode_remap_y",              "pos_z" },
        { "gcode_remap_z",              "pos_y" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops",                0 },
        { "z_hop",                      0 },
        { "machine_start_gcode",        "T[initial_tool]\n" },
        { "layer_change_gcode",         "G92 E0\n" },
    });
    return config;
}

TEST_CASE("Belt-only keys at non-default values leave non-belt G-code unchanged", "[Print][belt][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "z_hop",                      0 },
        { "brim_type",                  "outer_only" },
        { "brim_width",                 4 },
        { "enable_support",             1 },
        { "support_type",               "tree(auto)" },
        { "support_style",              "organic" },
        { "sparse_infill_pattern",      "adaptivecubic" },
        { "machine_start_gcode",        "T[initial_tool]\n" },
        { "layer_change_gcode",         "G92 E0\n" },
    });
    const std::string reference = gcode_body(slice({ TestMesh::overhang }, config));
    REQUIRE(! reference.empty());

    config.set_deserialize_strict({
        { "belt_printer",                0 },
        { "belt_printer_infinite_y",     0 },
        { "belt_slice_rotation",         "y" },
        { "belt_slice_rotation_angle",   30 },
        { "gcode_remap_x",               "rev_x" },
        { "gcode_remap_y",               "pos_z" },
        { "gcode_remap_z",               "pos_y" },
        { "belt_frame_tilt_decouple",    1 },
        { "belt_frame_tilt_angle",       30 },
        { "belt_support_floor_offset",   -5 },
        { "enable_belt_purge_tower",     1 },
        { "belt_purge_tower_width",      10 },
        { "leading_brim_length",         10 },
        { "extra_brim_width",            5 },
    });
    CHECK(gcode_body(slice({ TestMesh::overhang }, config)) == reference);
}

TEST_CASE("Switching a sliced project from belt to non-belt matches a fresh slice", "[Print][belt][Regression]")
{
    DynamicPrintConfig flat = DynamicPrintConfig::full_print_config();
    flat.set_deserialize_strict({
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops",                0 },
        { "z_hop",                      0 },
        { "enable_support",             1 },
        { "support_type",               "tree(auto)" },
        { "support_style",              "organic" },
        { "sparse_infill_pattern",      "adaptivecubic" },
        { "machine_start_gcode",        "T[initial_tool]\n" },
        { "layer_change_gcode",         "G92 E0\n" },
    });
    DynamicPrintConfig belt = belt_test_config();
    belt.set_deserialize_strict({
        { "enable_support",        1 },
        { "support_type",          "tree(auto)" },
        { "support_style",         "organic" },
        { "sparse_infill_pattern", "adaptivecubic" },
    });

    auto fresh_slice = [&](const DynamicPrintConfig &target) {
        Print print;
        Model model;
        init_print({ TestMesh::overhang }, print, model, belt);
        print.apply(model, target);
        const std::string out = gcode(print);
        return gcode_body(out);
    };
    auto resliced = [&](const DynamicPrintConfig &target) {
        Print print;
        Model model;
        init_print({ TestMesh::overhang }, print, model, belt);
        REQUIRE(! gcode(print).empty());
        print.apply(model, target);
        const std::string out = gcode(print);
        return gcode_body(out);
    };
    SECTION("belt printer to a flat-bed printer") {
        CHECK(resliced(flat) == fresh_slice(flat));
    }
    SECTION("belt tilt axis set to None") {
        DynamicPrintConfig untilted = belt;
        untilted.set_deserialize_strict({ { "belt_slice_rotation", "none" } });
        CHECK(resliced(untilted) == fresh_slice(untilted));
    }
}

TEST_CASE("A support-only change on a belt purge print matches a fresh slice", "[Print][belt][PurgeTower][Regression]")
{
    auto make_config = [](bool support) {
        DynamicPrintConfig config = multifilament_config(2, {
            { "belt_printer",               1 },
            { "belt_slice_rotation",        "x" },
            { "belt_slice_rotation_angle",  45 },
            { "gcode_remap_x",              "rev_x" },
            { "gcode_remap_y",              "pos_z" },
            { "gcode_remap_z",              "pos_y" },
            { "layer_height",               0.2 },
            { "initial_layer_print_height", 0.2 },
            { "skirt_loops",                0 },
            { "z_hop",                      0 },
            { "enable_belt_purge_tower",    1 },
            { "machine_start_gcode",        "T[initial_tool]\n" },
            { "layer_change_gcode",         "G92 E0\n" },
        });
        config.set_deserialize_strict({
            { "enable_support", support ? 1 : 0 },
            { "support_type",   "tree(auto)" },
            { "support_style",  "organic" },
        });
        return config;
    };
    const std::vector<std::vector<Slic3r::ConfigBase::SetDeserializeItem>> overrides {
        { { "extruder", 1 } }, { { "extruder", 2 } },
    };
    auto build = [&](Print &print, Model &model, const DynamicPrintConfig &config) {
        init_print(std::vector<TriangleMesh>{ mesh(TestMesh::overhang), cube(20) }, print, model, config, &overrides);
        model.objects.back()->config.set_key_value("belt_purge_tower_object", new ConfigOptionBool(true));
        print.apply(model, config);
        REQUIRE(print.has_belt_purge_tower());
    };

    std::string fresh;
    {
        Print print;
        Model model;
        build(print, model, make_config(true));
        fresh = gcode_body(gcode(print));
    }
    REQUIRE(! fresh.empty());

    Print print;
    Model model;
    build(print, model, make_config(false));
    REQUIRE(! gcode(print).empty());
    print.apply(model, make_config(true));
    CHECK(gcode_body(gcode(print)) == fresh);
}

TEST_CASE("Organic tree supports place a support blocker at its own height above a raft", "[Print][Support][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops",                0 },
        { "enable_support",             1 },
        { "support_type",               "tree(auto)" },
        { "support_style",              "organic" },
        { "raft_layers",                3 },
    });
    Print print;
    Model model;
    init_print({ cube(20) }, print, model, config);
    ModelObject *object  = model.objects.front();
    ModelVolume *blocker = object->add_volume(TriangleMesh(its_make_cube(6., 6., 4.)));
    blocker->set_type(ModelVolumeType::SUPPORT_BLOCKER);
    const Vec3d part_offset = object->volumes.front()->get_offset();
    blocker->set_offset(Vec3d(part_offset.x() + 20., part_offset.y(), 10.));
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    const PrintObject &print_object = *print.objects().front();
    const std::vector<Vec2d> bed = { { 0., 0. }, { 200., 0. }, { 200., 200. }, { 0., 200. } };
    const BuildVolume build_volume{ bed, print.config().printable_height.value, {}, {} };
    TreeSupport3D::TreeModelVolumes volumes{ print_object, build_volume, scaled<coord_t>(1.), scaled<coord_t>(0.5), 0, {} };

    const size_t num_raft = TreeSupport3D::TreeSupportSettings(TreeSupport3D::TreeSupportMeshGroupSettings(print_object),
                                                               print_object.slicing_parameters()).raft_layers.size();
    REQUIRE(num_raft >= 3);
    const std::vector<Polygons> blockers = print_object.slice_support_blockers();
    size_t first = 0, last = 0;
    bool   found = false;
    for (size_t i = 0; i < blockers.size(); ++ i)
        if (! blockers[i].empty()) {
            if (! found) { first = i; found = true; }
            last = i;
        }
    REQUIRE(found);
    REQUIRE(last - first > num_raft);
    const Vec3d centre3 = print_object.trafo_sliced() * blocker->get_offset();
    const Point centre  = Point::new_scale(centre3.x(), centre3.y());
    auto collides = [&](size_t tree_layer) {
        for (const Slic3r::Polygon &poly : volumes.getCollision(0, TreeSupport3D::LayerIndex(tree_layer), false))
            if (poly.contains(centre))
                return true;
        return false;
    };
    CHECK(collides(num_raft + first));
    CHECK(collides(num_raft + last));
    CHECK_FALSE(collides(first));
    CHECK_FALSE(collides(first + num_raft - 1));
    CHECK(collides(last + 1));
    CHECK(collides(last + num_raft));
}

TEST_CASE("Organic tree supports keep their distance from the part", "[Print][Support]")
{
    indexed_triangle_set its   = its_make_cube(20., 20., 20.);
    indexed_triangle_set plate = its_make_cube(60., 60., 4.);
    its_translate(its, Vec3f(20.f, 20.f, 0.f));
    its_translate(plate, Vec3f(0.f, 0.f, 16.f));
    its_merge(its, plate);
    TriangleMesh mesh(std::move(its));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops",                0 },
        { "enable_support",             1 },
        { "support_type",               "tree(auto)" },
        { "support_style",              "organic" },
        { "support_threshold_angle",    30 },
    });
    Print print;
    Model model;
    init_print({ mesh }, print, model, config);
    model.objects.front()->instances.front()->set_offset(Vec3d(100., 100., 0.));
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    const PrintObject &object = *print.objects().front();
    INFO("object layers " << object.layers().size() << ", support layers " << object.support_layers().size());
    REQUIRE(! object.support_layers().empty());
    size_t support_layers_with_fills = 0;
    for (const SupportLayer *layer : object.support_layers())
        if (! layer->support_fills.empty())
            ++ support_layers_with_fills;
    INFO("support layers with extrusions " << support_layers_with_fills);
    CHECK(support_layers_with_fills > 20);

    std::map<coord_t, const Layer *> object_layers;
    for (const Layer *layer : object.layers())
        object_layers[scaled<coord_t>(layer->print_z)] = layer;
    auto contains = [](const ExPolygons &expolys, const Point &pt) {
        for (const ExPolygon &ex : expolys)
            if (ex.contains(pt))
                return true;
        return false;
    };
    const float min_gap = scaled<float>(0.2);
    size_t too_close = 0, points = 0, layers_checked = 0, layers_unmatched = 0;
    for (const SupportLayer *layer : object.support_layers()) {
        if (layer->support_fills.empty())
            continue;
        auto it = object_layers.lower_bound(scaled<coord_t>(layer->print_z - EPSILON));
        if (it == object_layers.end()) {
            ++ layers_unmatched;
            continue;
        }
        ++ layers_checked;
        const ExPolygons grown = offset_ex(it->second->lslices, min_gap);
        for (const ExtrusionEntity *entity : layer->support_fills.flatten().entities)
            for (const Slic3r::Polyline &pl : entity->as_polylines())
                for (size_t i = 0; i < pl.points.size(); ++ i) {
                    ++ points;
                    if (contains(grown, pl.points[i]))
                        ++ too_close;
                    if (i + 1 < pl.points.size() && contains(grown, (pl.points[i] + pl.points[i + 1]) / 2))
                        ++ too_close;
                }
    }
    INFO("support layers checked " << layers_checked << " (unmatched " << layers_unmatched << "), support points " << points
         << ", within 0.2 mm of the part " << too_close);
    CHECK(layers_checked > 20);
    CHECK(layers_unmatched == 0);
    REQUIRE(points > 0);
    CHECK(too_close == 0);
}

TEST_CASE("Belt G-code has no layer that prints nothing", "[Print][belt][GCode][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "belt_printer",               1 },
        { "belt_slice_rotation",        "x" },
        { "belt_slice_rotation_angle",  45 },
        { "gcode_remap_x",              "rev_x" },
        { "gcode_remap_y",              "pos_z" },
        { "gcode_remap_z",              "pos_y" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops",                0 },
        { "z_hop",                      0 },
        { "brim_type",                  "outer_only" },
        { "brim_width",                 4 },
        { "machine_start_gcode",        "T[initial_tool]\n" },
        { "layer_change_gcode",         "G92 E0\n" },
    });
    SECTION("by layer")  { config.set_deserialize_strict({{ "print_sequence", "by layer" }}); }
    SECTION("by object") { config.set_deserialize_strict({{ "print_sequence", "by object" }}); }
    Print print;
    Model model;
    TriangleMesh cube_a(its_make_cube(20., 20., 20.));
    TriangleMesh cube_b(its_make_cube(20., 20., 20.));
    init_print({ cube_a, cube_b }, print, model, config);
    model.objects[0]->instances.front()->set_offset(Vec3d(50., 40., 0.));
    model.objects[1]->instances.front()->set_offset(Vec3d(50., 100., 0.));
    print.apply(model, config);
    print.set_status_silent();
    const std::string gc = gcode(print);
    REQUIRE(! gc.empty());

    size_t layers = 0, empty = 0, total_header = 0, total_count = 0;
    bool   extruded = true;
    auto close_layer = [&]() { if (! extruded) ++ empty; };
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(gc, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string &raw = line.raw();
        if (raw.rfind(";LAYER_CHANGE", 0) == 0) {
            close_layer();
            ++ layers;
            extruded = false;
        } else if (raw.rfind("; total layer number: ", 0) == 0) {
            total_header = size_t(std::atoi(raw.c_str() + 22));
        } else if (raw.rfind("; total layers count = ", 0) == 0) {
            total_count = size_t(std::atoi(raw.c_str() + 23));
        } else if (! extruded && line.extruding(self) && line.dist_XY(self) > EPSILON) {
            extruded = true;
        }
    });
    close_layer();
    INFO("layers " << layers << ", header " << total_header << ", count " << total_count
         << ", layers without extrusion " << empty);
    CHECK(layers > 150);
    CHECK(empty == 0);
    CHECK(total_header == layers);
    CHECK(total_count == layers);
}

TEST_CASE("Belt supports reach the belt under a leading overhang", "[Print][belt][Support][Regression]")
{
    const char *support_type  = GENERATE("normal(auto)", "tree(auto)");
    const char *support_style = GENERATE("default", "organic", "tree_hybrid");
    if (std::string(support_type) == "normal(auto)" && std::string(support_style) != "default")
        return;
    DYNAMIC_SECTION(support_type << " / " << support_style) {
        indexed_triangle_set its = its_make_cube(20., 20., 20.);
        indexed_triangle_set fin = its_make_cube(20., 20., 2.);
        Transform3d shear = Transform3d::Identity();
        shear.matrix() << 1., 0., 0.,   0.,
                          0., 1., 0., -20.,
                          0., -1., 1., 38.,
                          0., 0., 0.,   1.;
        its_transform(fin, shear);
        its_merge(its, fin);
        TriangleMesh mesh(std::move(its));

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "belt_printer",               1 },
            { "belt_slice_rotation",        "x" },
            { "belt_slice_rotation_angle",  45 },
            { "gcode_remap_x",              "rev_x" },
            { "gcode_remap_y",              "pos_z" },
            { "gcode_remap_z",              "pos_y" },
            { "layer_height",               0.2 },
            { "initial_layer_print_height", 0.2 },
            { "skirt_loops",                0 },
            { "z_hop",                      0 },
            { "enable_support",             1 },
            { "support_type",               support_type },
            { "support_style",              support_style },
            { "support_threshold_angle",    30 },
            { "machine_start_gcode",        "T[initial_tool]\n" },
            { "layer_change_gcode",         "G92 E0\n" },
        });
        Print print;
        Model model;
        init_print({ mesh }, print, model, config);
        model.objects.front()->instances.front()->set_offset(Vec3d(100., 100., 0.));
        print.apply(model, config);
        print.set_status_silent();
        print.process();

        const PrintObject &object = *print.objects().front();
        REQUIRE(! object.layers().empty());
        {
            double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
            for (const stl_vertex &v : mesh.its.vertices) {
                lo = std::min<double>(lo, v.y() + v.z());
                hi = std::max<double>(hi, v.y() + v.z());
            }
            const double span = (hi - lo) / std::sqrt(2.);
            size_t nonempty = 0;
            for (const Layer *layer : object.layers())
                if (! layer->lslices.empty())
                    ++ nonempty;
            INFO("non-empty object layers " << nonempty << ", slicing span " << span << " mm");
            CHECK(double(nonempty) * 0.2 > span - 0.6);
        }
        BeltFloorContext floor;
        REQUIRE(floor.init(object.slicing_parameters(), print.config()));

        const SupportLayer *lowest = nullptr;
        for (const SupportLayer *layer : object.support_layers())
            if (! layer->support_fills.empty() && (lowest == nullptr || layer->print_z < lowest->print_z))
                lowest = layer;
        REQUIRE(lowest != nullptr);
        double floor_under_lowest = std::numeric_limits<double>::max();
        for (const ExtrusionEntity *entity : lowest->support_fills.flatten().entities)
            for (const Slic3r::Polyline &pl : entity->as_polylines())
                for (const Point &pt : pl.points)
                    floor_under_lowest = std::min(floor_under_lowest, floor.floor_print_z(pt));
        double first_object_z = std::numeric_limits<double>::max();
        for (const Layer *layer : object.layers())
            if (! layer->lslices.empty()) { first_object_z = layer->print_z; break; }
        REQUIRE(first_object_z < std::numeric_limits<double>::max());
        INFO("lowest support z " << lowest->print_z << ", floor under it " << floor_under_lowest
             << ", first object layer " << first_object_z);
        CHECK(lowest->print_z < first_object_z - 5.);
        CHECK(lowest->print_z - floor_under_lowest < 4. * 0.2 + EPSILON);
        CHECK(lowest->print_z - floor_under_lowest > -0.2 - EPSILON);
    }
}
