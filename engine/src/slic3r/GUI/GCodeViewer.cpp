#include "libslic3r/libslic3r.h"
#include "GCodeViewer.hpp"
#include "NotificationManager.hpp"
#include "libslic3r/Nonplanar/HeadBoxLabel.hpp"
#include "libslic3r/Nonplanar/HeadEnvelope.hpp"
#include "libslic3r/Nonplanar/Report.hpp"
#include "libvgcode/include/ClearanceColor.hpp"
#include "libvgcode/include/NonplanarColor.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/IMEXHelpers.hpp"
#include "libslic3r/IMEXZones.hpp"

#include "GUI_App.hpp"
#include "Shortcuts.hpp"
#include "Plater.hpp"
#include "Camera.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "GUI.hpp"
#include "GLCanvas3D.hpp"
#include "FilamentGroupPopup.hpp"
#include "GLToolbar.hpp"
#include "libslic3r/BeltTransform.hpp"
#include "libslic3r/GCode/MachineFrameTransform.hpp"
#include "MsgDialog.hpp"
#include <boost/container_hash/hash.hpp>
#include "slic3r/GUI/MeshUtils.hpp"
#include <set>
#include <string>
#include "libvgcode/include/Types.hpp"
#include <vector>
#include <utility>
#include <cstdio>
#include "libslic3r/Technologies.hpp"
#include <imgui.h>
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoUtils.hpp"
#include "libslic3r/Point.hpp"
#include <math.h>
#include "libvgcode/include/Viewer.hpp"
#include "libvgcode/include/PathVertex.hpp"
#include <cstddef>
#include <cstring>
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include <cassert>
#include <cstdint>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/constants.hpp>
#include "slic3r/GUI/IMSlider.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include <exception>
#include <iterator>
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libvgcode/include/GCodeInputData.hpp"
#include "libvgcode/include/ColorRange.hpp"
#include <optional>
#include "libslic3r_version.h"
#include <wx/app.h>
#include <wx/busycursor.h>
#include "libslic3r/Slicing.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "libslic3r/Color.hpp"
#include <map>
#include <wx/event.h>
#include <wx/string.h>
#include <wx/slider.h>
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/Event.hpp"
#include <string_view>
#include <functional>
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/ExtrusionEntity.hpp"

#define IMGUI_DEFINE_MATH_OPERATORS

#include <imgui/imgui_internal.h>

#include <glad/gl.h>
#include <boost/functional/hash.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <wx/progdlg.h>
#include <wx/numformatter.h>
#include <wx/utils.h>

#include <array>
#include <algorithm>
#include <limits>
#include <sstream>
#include <cmath>
#include <chrono>
#include <Eigen/Geometry>
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/ObjectID.hpp"
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"
#include "slic3r/GUI/PartPlate.hpp"

using namespace std::string_view_literals;

namespace Slic3r { class PrintBase; }


namespace Slic3r {
namespace GUI {

static std::string get_view_type_string(libvgcode::EViewType view_type)
{
    if (view_type == libvgcode::EViewType::Clearance)
        return _u8L("Clearance");
    if (view_type == libvgcode::EViewType::Nonplanar)
        return _u8L("Non-planar");
    if (view_type == libvgcode::EViewType::Summary)
        return _u8L("Summary");
    else if (view_type == libvgcode::EViewType::FeatureType)
        return _u8L("Line Type");
    else if (view_type == libvgcode::EViewType::Height)
        return _u8L("Layer Height");
    else if (view_type == libvgcode::EViewType::Width)
        return _u8L("Line Width");
    else if (view_type == libvgcode::EViewType::Speed)
        return _u8L("Speed");
    else if (view_type == libvgcode::EViewType::ActualSpeed)
        return _u8L("Actual Speed");
    else if (view_type == libvgcode::EViewType::Acceleration)
        return _u8L("Acceleration");
    else if (view_type == libvgcode::EViewType::Jerk)
        return _u8L("Jerk");
    else if (view_type == libvgcode::EViewType::FanSpeed)
        return _u8L("Fan Speed");
    else if (view_type == libvgcode::EViewType::Temperature)
        return _u8L("Temperature");
    else if (view_type == libvgcode::EViewType::VolumetricFlowRate)
        return _u8L("Flow");
    else if (view_type == libvgcode::EViewType::ActualVolumetricFlowRate)
        return _u8L("Actual Flow");
    else if (view_type == libvgcode::EViewType::Tool)
        return _u8L("Tool");
    else if (view_type == libvgcode::EViewType::ColorPrint)
        return _u8L("Filament");
    else if (view_type == libvgcode::EViewType::LayerTimeLinear)
        return _u8L("Layer Time");
    else if (view_type == libvgcode::EViewType::LayerTimeLogarithmic)
        return _u8L("Layer Time (log)");
    else if (view_type == libvgcode::EViewType::PressureAdvance)
        return _u8L("Pressure Advance");
    return "";
}

static const std::vector<std::pair<std::string, libvgcode::EViewType>>& view_type_config_map()
{
    static const std::vector<std::pair<std::string, libvgcode::EViewType>> map = {
        { "summary",                      libvgcode::EViewType::Summary },
        { "feature_type",                 libvgcode::EViewType::FeatureType },
        { "color_print",                  libvgcode::EViewType::ColorPrint },
        { "speed",                        libvgcode::EViewType::Speed },
        { "actual_speed",                 libvgcode::EViewType::ActualSpeed },
        { "acceleration",                 libvgcode::EViewType::Acceleration },
        { "jerk",                         libvgcode::EViewType::Jerk },
        { "clearance",                    libvgcode::EViewType::Clearance },
        { "nonplanar",                    libvgcode::EViewType::Nonplanar },
        { "height",                       libvgcode::EViewType::Height },
        { "width",                        libvgcode::EViewType::Width },
        { "volumetric_flow_rate",         libvgcode::EViewType::VolumetricFlowRate },
        { "actual_volumetric_flow_rate",  libvgcode::EViewType::ActualVolumetricFlowRate },
        { "layer_time_linear",            libvgcode::EViewType::LayerTimeLinear },
        { "layer_time_logarithmic",       libvgcode::EViewType::LayerTimeLogarithmic },
        { "fan_speed",                    libvgcode::EViewType::FanSpeed },
        { "temperature",                  libvgcode::EViewType::Temperature },
        { "pressure_advance",             libvgcode::EViewType::PressureAdvance },
    };
    return map;
}

static int find_close_layer_idx(const std::vector<double> &zs, double &z, double eps)
{
    if (zs.empty()) return -1;
    auto it_h = std::lower_bound(zs.begin(), zs.end(), z);
    if (it_h == zs.end()) {
        auto it_l = it_h;
        --it_l;
        if (z - *it_l < eps) return int(zs.size() - 1);
    } else if (it_h == zs.begin()) {
        if (*it_h - z < eps) return 0;
    } else {
        auto it_l = it_h;
        --it_l;
        double dist_l = z - *it_l;
        double dist_h = *it_h - z;
        if (std::min(dist_l, dist_h) < eps) { return (dist_l < dist_h) ? int(it_l - zs.begin()) : int(it_h - zs.begin()); }
    }
    return -1;
}

static std::string format_compact_weight(double value_in_grams, bool imperial_units)
{
    char buffer[64];
    if (imperial_units) {
        ::sprintf(buffer, "%.2f oz", value_in_grams / GizmoObjectManipulation::oz_to_g);
        return buffer;
    }

    const double abs_value = value_in_grams < 0.0 ? -value_in_grams : value_in_grams;
    const char* unit = "g";
    double scaled_value = abs_value;
    if (scaled_value >= 1000000.0) {
        scaled_value /= 1000000.0;
        unit = "t";
    } else if (scaled_value >= 1000.0) {
        scaled_value /= 1000.0;
        unit = "kg";
    }

    ::sprintf(buffer, "%s%.2f%s", value_in_grams < 0.0 ? "-" : "", scaled_value, unit);
    return buffer;
}

#if ENABLE_ACTUAL_SPEED_DEBUG
int GCodeViewer::SequentialView::ActualSpeedImguiWidget::plot(const char* label, const std::array<float, 2>& frame_size)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return -1;

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiID id = window->GetID(label);

    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    ImVec2 internal_frame_size(frame_size[0], frame_size[1]);
    internal_frame_size = ImGui::CalcItemSize(internal_frame_size, ImGui::CalcItemWidth(), label_size.y + style.FramePadding.y * 2.0f);

    const ImRect frame_bb(window->DC.CursorPos, window->DC.CursorPos + internal_frame_size);
    const ImRect inner_bb(frame_bb.Min + style.FramePadding, frame_bb.Max - style.FramePadding);
    const ImRect total_bb(frame_bb.Min, frame_bb.Max + ImVec2(label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f, 0));
    ImGui::ItemSize(total_bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(total_bb, 0, &frame_bb))
        return -1;

    const bool hovered = ImGui::ItemHoverable(frame_bb, id);

    ImGui::RenderFrame(frame_bb.Min, frame_bb.Max, ImGui::GetColorU32(ImGuiCol_FrameBg), true, style.FrameRounding);

    static const int values_count_min = 2;
    const int values_count = static_cast<int>(data.size());
    int idx_hovered = -1;

    const ImVec2 offset(10.0f, 0.0f);

    const float size_y = y_range.second - y_range.first;
    const float size_x = data.back().pos - data.front().pos;
    if (size_x > 0.0f && values_count >= values_count_min) {
        const float inv_scale_y = (size_y == 0.0f) ? 0.0f : 1.0f / size_y;
        const float inv_scale_x = 1.0f / size_x;
        const float x0 = data.front().pos;
        const float y0 = y_range.first;

        const ImU32 grid_main_color = ImGui::GetColorU32(ImVec4(0.5f, 0.5f, 0.5f, 0.5f));
        const ImU32 grid_secondary_color = ImGui::GetColorU32(ImVec4(0.0f, 150.f / 255.0f, 136.0f / 255.f, 0.5f));
        for (const auto& [level, color] : levels) {
            const float y = 1.0f - ImSaturate((level - y_range.first) * inv_scale_y);
            window->DrawList->AddLine(ImLerp(inner_bb.Min, ImVec2(inner_bb.Min.x + offset.x, inner_bb.Max.y), ImVec2(0.1f, y)),
                ImLerp(inner_bb.Min, ImVec2(inner_bb.Min.x + offset.x, inner_bb.Max.y), ImVec2(0.9f, y)), ImGuiWrapper::to_ImU32(color), 3.0f);
        }

        for (int n = 0; n < values_count - 1; ++n) {
            const float x = ImSaturate((data[n].pos - x0) * inv_scale_x);
            window->DrawList->AddLine(ImLerp(inner_bb.Min + offset, inner_bb.Max, ImVec2(x, 0.0f)),
                ImLerp(inner_bb.Min + offset, inner_bb.Max, ImVec2(x, 1.0f)), data[n].internal ? grid_secondary_color : grid_main_color);
        }
        window->DrawList->AddLine(ImLerp(inner_bb.Min + offset, inner_bb.Max, ImVec2(1.0f, 0.0f)),
            ImLerp(inner_bb.Min + offset, inner_bb.Max, ImVec2(1.0f, 1.0f)), grid_main_color);

        const ImU32 col_base = ImGui::GetColorU32(ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
        const ImU32 col_hovered = ImGui::GetColorU32(ImVec4(0.0f, 150.f / 255.0f, 136.0f / 255.f, 1.0f));
        for (int n = 0; n < values_count - 1; ++n) {
            const ImVec2 tp1(ImSaturate((data[n].pos - x0) * inv_scale_x), 1.0f - ImSaturate((data[n].speed - y0) * inv_scale_y));
            const ImVec2 tp2(ImSaturate((data[n + 1].pos - x0) * inv_scale_x), 1.0f - ImSaturate((data[n + 1].speed - y0) * inv_scale_y));
            if (hovered && inner_bb.Contains(io.MousePos)) {
                const float t = ImClamp((io.MousePos.x - inner_bb.Min.x - offset.x) / (inner_bb.Max.x - inner_bb.Min.x - offset.x), 0.0f, 0.9999f);
                if (tp1.x < t && t < tp2.x)
                    idx_hovered = n;
            }
            window->DrawList->AddLine(ImLerp(inner_bb.Min + offset, inner_bb.Max, tp1), ImLerp(inner_bb.Min + offset, inner_bb.Max, tp2),
                idx_hovered == n ? col_hovered : col_base, 2.0f);
        }
    }

    if (label_size.x > 0.0f)
        ImGui::RenderText(ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x, inner_bb.Min.y), label);
    return idx_hovered;
}
#endif

void GCodeViewer::SequentialView::Marker::init(std::string filename)
{
    if (filename.empty()) {
        m_model.init_from(stilized_arrow(16, 1.5f, 3.0f, 0.8f, 3.0f));
    } else {
        m_model.init_from_file(filename);
    }
    m_model.set_color(default_color());
}

void GCodeViewer::SequentialView::Marker::render(int canvas_width, int canvas_height, const libvgcode::EViewType& view_type)
{
    if (!m_visible)
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    shader->start_using();
    shader->set_uniform("emission_factor", 0.0f);

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d& view_matrix = camera.get_view_matrix();
    float scale_factor = m_scale_factor;
    const Transform3d model_matrix = (Geometry::translation_transform((m_world_position + m_model_z_offset * Vec3f::UnitZ()).cast<double>()) *
        Geometry::translation_transform(scale_factor * m_model.get_bounding_box().size().z() * Vec3d::UnitZ()) * Geometry::rotation_transform({ M_PI, 0.0, 0.0 })) *
        Geometry::scale_transform(scale_factor);
    shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    shader->set_uniform("view_normal_matrix", view_normal_matrix);

    m_model.render();

    shader->stop_using();

    glsafe(::glDisable(GL_BLEND));
}

static std::string to_string(libvgcode::EMoveType type)
{
    switch (type)
    {
    case libvgcode::EMoveType::Noop:        { return _u8L("Noop"); }
    case libvgcode::EMoveType::Retract:     { return _u8L("Retract"); }
    case libvgcode::EMoveType::Unretract:   { return _u8L("Unretract"); }
    case libvgcode::EMoveType::Seam:        { return _u8L("Seam"); }
    case libvgcode::EMoveType::ToolChange:  { return _u8L("Tool Change"); }
    case libvgcode::EMoveType::ColorChange: { return _u8L("Color Change"); }
    case libvgcode::EMoveType::PausePrint:  { return _u8L("Pause Print"); }
    case libvgcode::EMoveType::CustomGCode: { return _u8L("Custom G-code"); }
    case libvgcode::EMoveType::Travel:      { return _u8L("Travel"); }
    case libvgcode::EMoveType::Wipe:        { return _u8L("Wipe"); }
    case libvgcode::EMoveType::Extrude:     { return _u8L("Extrude"); }
    default:                                { return _u8L("Unknown"); }
    }
}

static std::string to_string(libvgcode::EGCodeExtrusionRole role)
{
    switch (role)
    {
    case libvgcode::EGCodeExtrusionRole::None:                     { return _u8L("Unknown"); }
    case libvgcode::EGCodeExtrusionRole::Perimeter:                { return _u8L("Inner wall"); }
    case libvgcode::EGCodeExtrusionRole::ExternalPerimeter:        { return _u8L("Outer wall"); }
    case libvgcode::EGCodeExtrusionRole::OverhangPerimeter:        { return _u8L("Overhang wall"); }
    case libvgcode::EGCodeExtrusionRole::InternalInfill:           { return _u8L("Sparse infill"); }
    case libvgcode::EGCodeExtrusionRole::SolidInfill:              { return _u8L("Internal solid infill"); }
    case libvgcode::EGCodeExtrusionRole::TopSolidInfill:           { return _u8L("Top surface"); }
    case libvgcode::EGCodeExtrusionRole::Ironing:                  { return _u8L("Ironing"); }
    case libvgcode::EGCodeExtrusionRole::BridgeInfill:             { return _u8L("Bridge"); }
    case libvgcode::EGCodeExtrusionRole::GapFill:                  { return _u8L("Gap infill"); }
    case libvgcode::EGCodeExtrusionRole::Skirt:                    { return _u8L("Skirt"); }
    case libvgcode::EGCodeExtrusionRole::SupportMaterial:          { return _u8L("Support"); }
    case libvgcode::EGCodeExtrusionRole::SupportMaterialInterface: { return _u8L("Support interface"); }
    case libvgcode::EGCodeExtrusionRole::WipeTower:                { return _u8L("Prime tower"); }
    case libvgcode::EGCodeExtrusionRole::Custom:                   { return _u8L("Custom"); }
    case libvgcode::EGCodeExtrusionRole::BottomSurface:            { return _u8L("Bottom surface"); }
    case libvgcode::EGCodeExtrusionRole::InternalBridgeInfill:     { return _u8L("Internal bridge"); }
    case libvgcode::EGCodeExtrusionRole::Brim:                     { return _u8L("Brim"); }
    case libvgcode::EGCodeExtrusionRole::SupportTransition:        { return _u8L("Support transition"); }
    case libvgcode::EGCodeExtrusionRole::Mixed:                    { return _u8L("Mixed"); }
    default:                                                       { return _u8L("Unknown"); }
    }
}

void GCodeViewer::SequentialView::Marker::render_position_window(const libvgcode::Viewer* viewer, int canvas_width, int canvas_height, const libvgcode::EViewType& view_type)
{
    static bool properties_shown = false;
    
    const std::string NA_TXT = _u8L("N/A");
    const char* NA_CSTR = NA_TXT.c_str();

    if (viewer != nullptr) {
        ImGuiWrapper& imgui = *wxGetApp().imgui();
        imgui.set_next_window_pos(0.5f * static_cast<float>(canvas_width), static_cast<float>(canvas_height), ImGuiCond_Always, 0.5f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGuiWrapper::COL_BUTTON_BACKGROUND);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGuiWrapper::COL_BUTTON_ACTIVE);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGuiWrapper::COL_BUTTON_HOVERED);
        ImGui::PushStyleColor(ImGuiCol_Border   , ImVec4(.0f,.0f,.0f,.0f));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.f,1.f,1.f,.6f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * m_scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding , ImVec2(10.f, 10.f) * m_scale);
        ImGui::SetNextWindowBgAlpha(0.8f);

        libvgcode::PathVertex vertex = viewer->get_current_vertex();
        size_t vertex_id = viewer->get_current_vertex_id();
        if (view_type != libvgcode::EViewType::FeatureType && vertex.type == libvgcode::EMoveType::Seam) {
            const libvgcode::Interval& visible_range = viewer->get_view_visible_range();
            if (visible_range[1] > 0) {
                vertex_id = static_cast<size_t>(visible_range[1]) - 1;
                vertex = viewer->get_vertex_at(vertex_id);
            }
        }
        const bool is_extrusion = vertex.is_extrusion();

        const ImGuiStyle& style = ImGui::GetStyle();

        char detail_buf[1024];
        switch (view_type) {
            case libvgcode::EViewType::FeatureType:
                if (is_extrusion)
                    strcpy(detail_buf, to_string(vertex.role).c_str());
                else if (vertex.type != libvgcode::EMoveType::Noop)
                    strcpy(detail_buf, to_string(vertex.type).c_str());
                else
                    strcpy(detail_buf, NA_CSTR);
                break;
            case libvgcode::EViewType::Clearance:
                if (std::isnan(vertex.clearance))
                    snprintf(detail_buf, sizeof(detail_buf), "%s", _u8L("Not checked").c_str());
                else if (std::isinf(vertex.clearance))
                    snprintf(detail_buf, sizeof(detail_buf), "%s", _u8L("Nothing within reach").c_str());
                else
                    snprintf(detail_buf, sizeof(detail_buf), "%s", into_u8(format_wxstr(_L("Clearance: %1% mm"),
                        wxString::Format("%.3f", vertex.clearance))).c_str());
                break;
            case libvgcode::EViewType::Nonplanar:
                if (is_extrusion && vertex.is_nonplanar())
                    snprintf(detail_buf, sizeof(detail_buf), "%s", into_u8(format_wxstr(_L("Patch %1%, shell %2%, band %3%: bead %4% mm, slope %5%°"),
                        vertex.nonplanar_patch, int(vertex.nonplanar_shell) + 1,
                        vertex.nonplanar_band < 0 ? _L("none") : wxString::Format("%d", vertex.nonplanar_band + 1),
                        wxString::Format("%.2f", vertex.height), wxString::Format("%.0f", vertex.slope_deg()))).c_str());
                else
                    snprintf(detail_buf, sizeof(detail_buf), "%s", is_extrusion ? _u8L("Planar").c_str() : NA_CSTR);
                break;
            case libvgcode::EViewType::Height:
                if (is_extrusion)
                    sprintf(detail_buf, "%s%.2f", _u8L("Height: ").c_str(), vertex.height);
                else
                    sprintf(detail_buf, "%s%s", _u8L("Height: ").c_str(), NA_CSTR);
                break;
            case libvgcode::EViewType::Width:
                if (is_extrusion)
                    sprintf(detail_buf, "%s%.2f", _u8L("Width: ").c_str(), vertex.width);
                else
                    sprintf(detail_buf, "%s%s", _u8L("Width: ").c_str(), NA_CSTR);
                break;
            case libvgcode::EViewType::VolumetricFlowRate:
                if (is_extrusion)
                    sprintf(detail_buf, "%s%.2f", _u8L("Flow: ").c_str(), vertex.volumetric_rate());
                else
                    sprintf(detail_buf, "%s%s", _u8L("Flow: ").c_str(), NA_CSTR);
                break;
            case libvgcode::EViewType::FanSpeed:
                sprintf(detail_buf, "%s%.0f", _u8L("Fan: ").c_str(), vertex.fan_speed);
                break;
            case libvgcode::EViewType::Temperature:
                sprintf(detail_buf, "%s%.0f", _u8L("Temperature: ").c_str(), vertex.temperature);
                break;
            case libvgcode::EViewType::LayerTimeLinear:
            case libvgcode::EViewType::LayerTimeLogarithmic:
                sprintf(detail_buf, "%s%.1f", _u8L("Layer Time: ").c_str(), vertex.layer_duration);
                break;
            case libvgcode::EViewType::Tool:
                sprintf(detail_buf, "%s%d", _u8L("Tool: ").c_str(), vertex.extruder_id + 1);
                break;
            case libvgcode::EViewType::ColorPrint:
                sprintf(detail_buf, "%s%d", _u8L("Color: ").c_str(), vertex.color_id + 1);
                break;
            case libvgcode::EViewType::Acceleration:
                sprintf(detail_buf, "%s%.0f", _u8L("Acceleration: ").c_str(), vertex.acceleration);
                break;
            case libvgcode::EViewType::Jerk:
                sprintf(detail_buf, "%s%.1f", _u8L("Jerk: ").c_str(), vertex.jerk);
                break;
            case libvgcode::EViewType::PressureAdvance:
                sprintf(detail_buf, "%s%.4f", _u8L("PA: ").c_str(), vertex.pressure_advance);
                break;
            default:
                detail_buf[0] = '\0';
                break;
        }

        std::vector<std::pair<std::string, std::string>> properties_rows;
        float table_content_w = 0.0f;
        if (properties_shown) {
            float label_w = 0.0f;
            float value_w = 0.0f;
            properties_rows.reserve(14);
            auto add_row = [&properties_rows, &label_w, &value_w](std::string label, std::string value) {
                 label_w = std::max(label_w, ImGui::CalcTextSize(label.c_str()).x);
                 value_w = std::max(value_w, ImGui::CalcTextSize(value.c_str()).x);
                 properties_rows.emplace_back(std::move(label), std::move(value));
            };
            char buff[1024];
            add_row(_u8L("Type"), _u8L(to_string(vertex.type)));
            add_row(_u8L("Line Type"), is_extrusion ? _u8L(to_string(vertex.role)) : NA_TXT);
            if (is_extrusion) sprintf(buff, ("%.3f " + _u8L("mm")).c_str(), vertex.width); else strcpy(buff, NA_CSTR);
            add_row(_u8L("Width"), buff);
            if (is_extrusion) sprintf(buff, ("%.3f " + _u8L("mm")).c_str(), vertex.height); else strcpy(buff, NA_CSTR);
            add_row(_u8L("Height"), buff);
            if (vertex_id > 0 && (is_extrusion || vertex.is_travel() || vertex.is_wipe())) {
                const size_t vertices_count = viewer->get_vertices_count();
                size_t first_id = vertex_id;
                while (first_id > 0 && viewer->get_vertex_at(first_id - 1).gcode_id == vertex.gcode_id)
                    --first_id;
                size_t last_id = vertex_id;
                while (last_id + 1 < vertices_count && viewer->get_vertex_at(last_id + 1).gcode_id == vertex.gcode_id)
                    ++last_id;
                float length = 0.0f;
                for (size_t i = std::max<size_t>(first_id, 1); i <= last_id; ++i) {
                    length += (libvgcode::convert(viewer->get_vertex_at(i).position) -
                               libvgcode::convert(viewer->get_vertex_at(i - 1).position)).norm();
                }
                sprintf(buff, ("%.3f " + _u8L("mm")).c_str(), length);
            }
            else
                strcpy(buff, NA_CSTR);
            add_row(_u8L("Length"), buff);
            sprintf(buff, "%d", vertex.layer_id + 1);
            add_row(_u8L("Layer"), buff);
            sprintf(buff, ("%.1f " + _u8L("mm/s")).c_str(), vertex.feedrate);
            add_row(_u8L("Speed"), buff);
            sprintf(buff, ("%.0f " + _u8L("mm/s²")).c_str(), vertex.acceleration);
            add_row(_u8L("Acceleration"), buff);
            sprintf(buff, ("%.1f " + _u8L("mm/s")).c_str(), vertex.jerk);
            add_row(_u8L("Jerk"), buff);
            if (is_extrusion) sprintf(buff, ("%.3f " + _u8L("mm³/s")).c_str(), vertex.volumetric_rate()); else strcpy(buff, NA_CSTR);
            add_row(_u8L("Flow rate"), buff);
            sprintf(buff, "%.0f %%", vertex.fan_speed);
            add_row(_u8L("Fan speed"), buff);
            sprintf(buff, ("%.0f " + _u8L("\u2103" )).c_str(), vertex.temperature);
            add_row(_u8L("Temperature"), buff);
            sprintf(buff, "%.4f", vertex.pressure_advance);
            add_row(_u8L("Pressure Advance"), buff);
            const float estimated_time = viewer->get_estimated_time_at(vertex_id);
            sprintf(buff, "%s (%.3fs)", get_time_dhms(estimated_time).c_str(), vertex.times[static_cast<size_t>(viewer->get_time_mode())]);
            add_row(_u8L("Time"), buff);

            table_content_w = 2 * std::max(label_w, value_w) + style.ItemInnerSpacing.x;
        }

        const float axes_spacing_x = ImGui::CalcTextSize("sp").x;
        const float axes_width     = ImGui::CalcTextSize("X 999.999").x;
        const float axes_content_w = 3.0f * axes_width + 2.0f * axes_spacing_x;
        const float speed_width    = ImGui::CalcTextSize((_u8L("Speed: ") + "9999  ").c_str()).x;
        const float detail_width   = ImGui::CalcTextSize(detail_buf).x;
        const float info_content_w = speed_width + detail_width;
        const float info_group_w   = std::max(axes_content_w, info_content_w);
        const float fold_button_w  = 24.0f * m_scale;
        const float bottom_row_w   = fold_button_w + style.ItemSpacing.x + info_group_w;
        float speed_profile_row_w  = 0.0f;
#if ENABLE_ACTUAL_SPEED_DEBUG
        const float table_btn_width = std::max(ImGui::CalcTextSize(_u8L("Hide").c_str()).x, ImGui::CalcTextSize(_u8L("Show").c_str()).x);
        const float btn_padding_x = 8.0f * m_scale;

        if (properties_shown) {
            speed_profile_row_w = table_btn_width + btn_padding_x * 2.0f + style.ItemSpacing.x + ImGui::CalcTextSize(_u8L("Actual speed profile").c_str()).x;
        }
#endif
        const float properties_w = std::max(table_content_w, speed_profile_row_w);
        const float content_w    = std::max(bottom_row_w, properties_w);
        const float window_w     = std::ceil(content_w + 2.0f * style.WindowPadding.x);
        const float text_h         = ImGui::GetTextLineHeight();
        const float item_spacing_y = style.ItemSpacing.y;
        const float cell_pad_y     = style.CellPadding.y;
        const float window_pad_h   = 2.0f * style.WindowPadding.y;
        const float show_button_h  = text_h + 2.0f * 3.0f * m_scale;
        const float main_row_h     = 2.0f * text_h + item_spacing_y;
        const float properties_h   = static_cast<float>(properties_rows.size()) * (text_h + 2.0f * cell_pad_y) +  2.0f * cell_pad_y + 1.0f + item_spacing_y
                                    + item_spacing_y + show_button_h
                                    + item_spacing_y + 1.0f + style.WindowPadding.y;
        const float folded_window_h   = std::ceil(window_pad_h + main_row_h);
        const float unfolded_window_h = std::ceil(folded_window_h + properties_h);
        const float window_h = properties_shown ? unfolded_window_h : folded_window_h;

        ImGui::SetNextWindowSize(ImVec2(window_w, window_h), ImGuiCond_Always);

        imgui.begin(std::string("ToolPosition"), ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        ImGui::AlignTextToFramePadding();

        if (properties_shown) {
            auto append_table_row = [](const std::string& label, const std::string& value) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORCA, label);
                ImGui::TableSetColumnIndex(1);
                ImGuiWrapper::text(value);
            };

            if (ImGui::BeginTable("Properties", 2)) {
                for (const auto& row : properties_rows)
                    append_table_row(row.first, row.second);

                ImGui::EndTable();
            }

#if ENABLE_ACTUAL_SPEED_DEBUG
            bool actual_speed_exist = vertex.is_extrusion() || vertex.is_travel() || vertex.is_wipe();
            static float cached_table_wnd_width = 0.0f;
            static float cached_table_wnd_scale = 0.0f;
            ImGui::Spacing();

            static bool table_shown = false;
            imgui.push_confirm_button_style();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding  , 3.f * m_scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding   , ImVec2(btn_padding_x, 3.f * m_scale));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(.5f, .5f));
            if (imgui.button((table_shown ? _L("Hide") : _L("Show")).c_str(), ImVec2(table_btn_width + btn_padding_x * 2.f, 0.f), actual_speed_exist))
                table_shown = !table_shown;
            ImGui::PopStyleVar(3);
            imgui.pop_confirm_button_style();
            ImGui::SameLine();
            ImGuiWrapper::text(_u8L("Actual speed profile").c_str());

            if (actual_speed_exist && table_shown) {
                const float plot_height = 135.f * m_scale;

                if (cached_table_wnd_scale != m_scale) {

                    const float cell_pad_x = 9.f * m_scale;
                    const float wnd_pad_x = 2.0f * style.WindowPadding.x;
                    const float table_border_x = 6.0f * m_scale;

                    const float col0_w = ImGui::CalcTextSize((_u8L("Position") + " (" + _u8L("mm") + ")").c_str()).x;
                    const float col1_w = ImGui::CalcTextSize((_u8L("Speed") + " (" + _u8L("mm/s") + ")").c_str()).x;

                    const float content_w = std::ceil(col0_w + col1_w + 4.0f * cell_pad_x + table_border_x + wnd_pad_x);
                    const float aspect_w = std::ceil(plot_height * (16.0f / 9.0f));
                    cached_table_wnd_width = std::max(content_w, aspect_w);
                    cached_table_wnd_scale = m_scale;
                }

                const float cell_pad_y  = 1.f * m_scale;
                const float row_height  = ImGui::GetTextLineHeight() + 2.f * cell_pad_y;
                const float rows        = static_cast<float>(m_actual_speed_imgui_widget.data.size());
                const float table_h     = row_height * (1.0f + rows) + 2.f * m_scale;
                const float target_h    = std::ceil(plot_height + table_h + 2.f * style.WindowPadding.y);
                const float min_h       = ImGui::GetWindowHeight();
                const float max_h       = static_cast<float>(canvas_height) - 8.f * m_scale;
                const float final_h     = std::clamp(target_h, min_h, max_h);
                const bool  needs_scroll = target_h > max_h;
                const float table_view_h = std::max(1.0f, final_h - plot_height - 2.f * style.WindowPadding.y);

                imgui.set_next_window_pos(ImGui::GetWindowPos().x - 5.f * m_scale , static_cast<float>(canvas_height), ImGuiCond_Always, 1.0f, 1.0f);
                ImGui::SetNextWindowSize(ImVec2(cached_table_wnd_width, final_h), ImGuiCond_Always);
                imgui.begin(std::string("ToolPositionTableWnd"), ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(9.f, 1.f) * m_scale);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));

                ImVec4 scroll_col    = ImVec4(0.77f, 0.77f, 0.77f, m_is_dark ? .6f : 1.0f);
                ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.f, 0.f, 0.f, 0.f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, scroll_col);
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, scroll_col);
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, scroll_col);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered , style.Colors[ImGuiCol_TableHeaderBg]);

                const int hover_id = m_actual_speed_imgui_widget.plot("##ActualSpeedProfile", { -1.f, plot_height});
                const ImGuiTableFlags table_flags = ImGuiTableFlags_Borders | (needs_scroll ? ImGuiTableFlags_ScrollY : 0);
                if (ImGui::BeginTable("ToolPositionTable", 2, table_flags, ImVec2(0.0f, needs_scroll ? table_view_h : 0.0f))) {
                    char buff[1024];
                    if (needs_scroll)
                        ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn((_u8L("Position") + " (" + _u8L("mm")   + ")").c_str());
                    ImGui::TableSetupColumn((_u8L("Speed")    + " (" + _u8L("mm/s") + ")").c_str());
                    ImGui::TableHeadersRow();
                    int counter = 0;
                    for (const ActualSpeedImguiWidget::Item& item : m_actual_speed_imgui_widget.data) {
                        const bool highlight = hover_id >= 0 && (counter == hover_id || counter == hover_id + 1);
                        ImGui::TableNextRow();
                        const ImU32 row_bg_color = ImGui::GetColorU32(item.internal ? ImVec4(0.0f, 150.f / 255.0f, 136.0f / 255.f, 0.15f) : ImVec4(0.2f, 0.2f, 0.2f, 0.25f));
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, row_bg_color);
                        ImGui::TableSetColumnIndex(0);
                        sprintf(buff, "%.3f", item.pos);
                        imgui.text_colored(highlight ? ImGuiWrapper::COL_ORCA : ImGuiWrapper::to_ImVec4(ColorRGBA::WHITE()), buff);
                        ImGui::TableSetColumnIndex(1);
                        sprintf(buff, "%.1f", item.speed);
                        imgui.text_colored(highlight ? ImGuiWrapper::COL_ORCA : ImGuiWrapper::to_ImVec4(ColorRGBA::WHITE()), buff);
                        ++counter;
                    }

                    ImGui::EndTable();
                }
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(5);
                imgui.end();
            }

#endif

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Dummy({0, style.WindowPadding.y});
        }

        float draw_area_height = ImGui::GetTextLineHeight() * 2.f + style.ItemSpacing.y;
        ImGui::Dummy({10.f, draw_area_height});

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding  , 3.f * m_scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding   , ImVec2(2.f, 2.f) * m_scale);
        ImGui::PushStyleColor(ImGuiCol_Button            , ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered     , ImVec4(84 / 255.f, 84 / 255.f, 90 / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive      , ImVec4(84 / 255.f, 84 / 255.f, 90 / 255.f, 1.f));
         
        const float main_wnd_height = ImGui::GetWindowHeight();
        const float draw_start_y = main_wnd_height - draw_area_height - style.WindowPadding.y;

        ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, draw_start_y));

        const wchar_t foldIcon = properties_shown ? ImGui::UnfoldButtonIcon : ImGui::FoldButtonIcon;
        if (imgui.glyph_button(foldIcon, ImVec2(16.f, 16.f) * m_scale)) {
            properties_shown = !properties_shown;
            static float main_wnd_height_temp = ImGui::GetWindowHeight();
            static float first_click = true;
            if (first_click || main_wnd_height_temp != main_wnd_height) {
                main_wnd_height_temp = main_wnd_height;
                first_click = false;
                imgui.set_requires_extra_frame();
            }
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);

        ImGui::SetCursorPos(ImVec2(style.WindowPadding.x + style.ItemSpacing.x + 24.f * m_scale, draw_start_y - 1.f * m_scale));

        ImGui::BeginGroup();
        auto pos       =vertex.position;
        int  max_value = std::round(std::max(std::max(pos[0], pos[1]),pos[2]));
        auto precision = max_value > 9999 ? "%.1f" : max_value > 999 ? "%.2f" : "%.3f";

        char xBuf[32];
        ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::X()),"X ");
        ImGui::SameLine(0,0);
        sprintf(xBuf, precision, pos[0]);
        ImGui::Text("%s", xBuf);

        char yBuf[32];
        ImGui::SameLine(axes_width + axes_spacing_x);
        ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Y()),"Y ");
        ImGui::SameLine(0,0);
        sprintf(yBuf, precision, pos[1]);
        ImGui::Text("%s", yBuf);

        char zBuf[32];
        ImGui::SameLine(2.0f * (axes_width + axes_spacing_x));
        ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Z()),"Z ");
        ImGui::SameLine(0,0);
        sprintf(zBuf, precision, pos[2]);
        ImGui::Text("%s", zBuf);

        ImGui::SameLine(3.0f * axes_width + 2.0f * axes_spacing_x);
        ImGui::Dummy({0,0});

        char spdBuf[128];
        sprintf(spdBuf, "%s%.0f ", _u8L("Speed: ").c_str(), vertex.feedrate);
        ImGuiWrapper::text(spdBuf);
        
        if (detail_buf[0] != '\0') {
            ImGui::SameLine(speed_width);
            ImGuiWrapper::text(detail_buf);
        }
        ImGui::EndGroup();

        imgui.end();
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(2);
    }
    else {
        ImGuiWrapper& imgui = *wxGetApp().imgui();
        imgui.set_next_window_pos(0.5f * static_cast<float>(canvas_width), static_cast<float>(canvas_height), ImGuiCond_Always, 0.5f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border   , ImVec4(.0f,.0f,.0f,.0f));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.f,1.f,1.f,.6f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * m_scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding , ImVec2(10.f, 10.f) * m_scale);
        ImGui::SetNextWindowBgAlpha(0.8f);
        imgui.begin(std::string("ToolPosition"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

        const Vec3f pos = m_world_position + m_world_offset + m_z_offset * Vec3f::UnitZ();

        int  max_value = std::round(std::max(std::max(pos.x(), pos.y()),pos.z()));
        auto precision = max_value > 9999 ? "%.1f" : max_value > 999 ? "%.2f" : "%.3f";
        const float axes_width = ImGui::CalcTextSize("X 999.999sp").x;

        char xBuf[32];
        ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::X()),"X ");
        ImGui::SameLine(0,0);
        sprintf(xBuf, precision, pos.x());
        ImGui::Text("%s", xBuf);

        char yBuf[32]; 
        ImGui::SameLine(axes_width);
        ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Y()),"Y ");
        ImGui::SameLine(0,0);
        sprintf(yBuf, precision, pos.y());
        ImGui::Text("%s", yBuf);

        char zBuf[32];
        ImGui::SameLine(axes_width * 2);
        ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Z()),"Z ");
        ImGui::SameLine(0,0);
        sprintf(zBuf, precision, pos.z());
        ImGui::Text("%s", zBuf);

        ImGui::SameLine(axes_width * 3);
        ImGui::Dummy({0,0});

        imgui.end();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
}

void GCodeViewer::SequentialView::GCodeWindow::load_gcode(const std::string& filename, const std::vector<size_t> &lines_ends)
{
    assert(! m_file.is_open());
    if (m_file.is_open())
        return;

    m_filename   = filename;
    m_lines_ends = lines_ends;

    m_selected_line_id = 0;
    m_last_lines_size = 0;

    try
    {
        m_file.open(boost::filesystem::path(m_filename));
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": mapping file " << m_filename;
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << "Unable to map file " << m_filename << ". Cannot show G-code window.";
        reset();
    }
}

static size_t utf8_offset(const std::string& str, size_t count)
{
    const char* const begin = str.c_str();
    const char* const end   = begin + str.size();
    const char*       pos   = begin;
    for (size_t i = 0; i < count && pos < end; ++i) {
        unsigned int codepoint = 0;
        pos += ImTextCharFromUtf8(&codepoint, pos, end);
    }
    return pos - begin;
}

void GCodeViewer::SequentialView::GCodeWindow::render(float top, float bottom, float right, uint64_t curr_line_id) const
{
    auto update_lines = [this](uint64_t start_id, uint64_t end_id) {
        std::vector<Line> ret;
        ret.reserve(end_id - start_id + 1);
        const size_t file_size = m_file.size();
        for (uint64_t id = start_id; id <= end_id; ++id) {
            const size_t start = id == 1 ? 0 : std::min(m_lines_ends[id - 2], file_size);
            const size_t end   = std::min(m_lines_ends[id - 1], file_size);
            const size_t original_len = end > start ? end - start : 0;
            const size_t len          = std::min(original_len, (size_t) 55 * 4);
            std::string  gline(m_file.data() + start, len);

            if (len < original_len || utf8_offset(gline, 55) < gline.size())
                gline = gline.substr(0, utf8_offset(gline, 52)) + "...";

            std::string command, parameters, comment;
            const size_t comment_start = gline.find(';');
            if (comment_start == std::string::npos)
                command = gline;
            else {
                command = gline.substr(0, comment_start);
                comment = gline.substr(comment_start);
            }

            if (!command.empty()) {
                std::vector<std::string> tokens;
                boost::split(tokens, command, boost::is_any_of(" "), boost::token_compress_on);
                command = tokens.front();
                if (tokens.size() > 1) {
                    for (size_t i = 1; i < tokens.size(); ++i) {
                        parameters += " " + tokens[i];
                    }
                }
            }
            ret.push_back({command, parameters, comment});
        }
        return ret;
    };

    static const ImVec4 LINE_NUMBER_COLOR    = ImGuiWrapper::COL_ORANGE_LIGHT;
    static const ImVec4 SELECTION_RECT_COLOR = ImGuiWrapper::COL_ORANGE_DARK;
    static const ImVec4 COMMAND_COLOR        = {0.8f, 0.8f, 0.0f, 1.0f};
    static const ImVec4 PARAMETERS_COLOR     = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const ImVec4 COMMENT_COLOR        = { 0.7f, 0.7f, 0.7f, 1.0f };

    if (!wxGetApp().show_gcode_window() || m_filename.empty() || m_lines_ends.empty() || curr_line_id == 0)
        return;

    const float wnd_height = bottom - top;

    const float text_height = ImGui::CalcTextSize("0").y;
    const ImGuiStyle& style = ImGui::GetStyle();
    const uint64_t lines_count = static_cast<uint64_t>((wnd_height - 2.0f * style.WindowPadding.y + style.ItemSpacing.y) / (text_height + style.ItemSpacing.y));

    if (lines_count == 0)
        return;

    const uint64_t half_lines_count = lines_count / 2;
    uint64_t start_id = (curr_line_id >= half_lines_count) ? curr_line_id - half_lines_count : 0;
    uint64_t end_id = start_id + lines_count - 1;
    if (end_id >= static_cast<uint64_t>(m_lines_ends.size())) {
        end_id = static_cast<uint64_t>(m_lines_ends.size()) - 1;
        start_id = end_id - lines_count + 1;
    }

    if (m_selected_line_id != curr_line_id || m_last_lines_size != end_id - start_id + 1) {
        try
        {
            *const_cast<std::vector<Line>*>(&m_lines) = update_lines(start_id, end_id);
        }
        catch (...)
        {
            BOOST_LOG_TRIVIAL(error) << "Error while loading from file " << m_filename << ". Cannot show G-code window.";
            return;
        }
        *const_cast<uint64_t*>(&m_selected_line_id) = curr_line_id;
        *const_cast<size_t*>(&m_last_lines_size) = m_lines.size();
    }

    const float id_width = ImGui::CalcTextSize(std::to_string(end_id).c_str()).x;

    float required_width = 0.0f;
    {
        float max_text_width = id_width;
        for (const Line& line : m_lines) {
            float w = 0.0f;

            if (!line.command.empty())
                w += ImGui::CalcTextSize(line.command.c_str()).x;

            if (!line.parameters.empty())
                w += ImGui::CalcTextSize(line.parameters.c_str()).x;

            if (!line.comment.empty())
                w += ImGui::CalcTextSize(line.comment.c_str()).x;

            w += 3 * ImGui::GetStyle().ItemSpacing.x;

            if (w > max_text_width)
                max_text_width = w;
        }

        required_width = id_width + max_text_width;
    }

    ImGuiWrapper& imgui = *wxGetApp().imgui();

    imgui.set_next_window_pos(right, top + 6 * m_scale, ImGuiCond_Always, 1.0f, 0.0f);
    ImGui::SetNextWindowSize(ImVec2(required_width, wnd_height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * m_scale);
    ImGui::SetNextWindowBgAlpha(0.8f);
    imgui.begin(std::string("G-code"), ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

    const float f_lines_count = static_cast<float>(lines_count);
    ImGui::SetCursorPosY(0.5f * (wnd_height - f_lines_count * text_height - (f_lines_count - 1.0f) * style.ItemSpacing.y));

    for (uint64_t id = start_id; id <= end_id; ++id) {
        const Line& line = m_lines[id - start_id];

        if (id == curr_line_id) {
            const float pos_y = ImGui::GetCursorScreenPos().y;
            const float pos_x = ImGui::GetCursorScreenPos().x;
            const float half_ItemSpacing_y = 0.5f * style.ItemSpacing.y;
            const float half_ItemSpacing_x = 0.5f * style.ItemSpacing.x;
            ImGui::GetWindowDrawList()->AddRect({ pos_x - half_ItemSpacing_x, pos_y - half_ItemSpacing_y },
                { right - half_ItemSpacing_x, pos_y + text_height + half_ItemSpacing_y },
                ImGui::GetColorU32(SELECTION_RECT_COLOR));
        }

        const std::string id_str = std::to_string(id);
        ImGui::Dummy({ id_width - ImGui::CalcTextSize(id_str.c_str()).x, text_height });
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, LINE_NUMBER_COLOR);
        imgui.text(id_str);
        ImGui::PopStyleColor();

        if (!line.command.empty() || !line.comment.empty())
            ImGui::SameLine();

        if (!line.command.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, COMMAND_COLOR);
            imgui.text(line.command);
            ImGui::PopStyleColor();
        }

        if (!line.parameters.empty()) {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, PARAMETERS_COLOR);
            imgui.text(line.parameters);
            ImGui::PopStyleColor();
        }

        if (!line.comment.empty()) {
            if (!line.command.empty())
                ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, COMMENT_COLOR);
            imgui.text(line.comment);
            ImGui::PopStyleColor();
        }
    }

    imgui.end();
    ImGui::PopStyleVar();
}

void GCodeViewer::SequentialView::GCodeWindow::stop_mapping_file()
{
    if (m_file.is_open()) {
        m_file.close();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": finished mapping file " << m_filename;
    }
}
void GCodeViewer::SequentialView::render_marker(const bool has_render_path, int canvas_width, int canvas_height, const libvgcode::EViewType& view_type)
{
    if (has_render_path && m_show_marker) {
        marker.render(canvas_width, canvas_height, view_type);
        for (auto& sec : m_imex_secondary_markers)
            sec.render(canvas_width, canvas_height, view_type);
    }
}

void GCodeViewer::SequentialView::render_overlay(const bool has_render_path, float legend_height, const libvgcode::Viewer* viewer, uint32_t gcode_id, int canvas_width, int canvas_height, int right_margin, const libvgcode::EViewType& view_type)
{
    if (has_render_path && m_show_marker)
        marker.render_position_window(viewer, canvas_width, canvas_height, view_type);

#if 0
    if (wxGetApp().is_editor())
        bottom -= wxGetApp().plater()->get_view_toolbar().get_height();
#endif
    if (has_render_path)
        gcode_window.render(legend_height + 2, std::max(10.f, (float)canvas_height - 40), (float)canvas_width - (float)right_margin, gcode_id);
}

GCodeViewer::GCodeViewer()
{
    m_moves_slider  = new IMSlider(0, 0, 0, 100, wxSL_HORIZONTAL);
    m_layers_slider = new IMSlider(0, 0, 0, 100, wxSL_VERTICAL);
}

GCodeViewer::~GCodeViewer()
{
    reset();
    if (m_moves_slider) {
        delete m_moves_slider;
        m_moves_slider = nullptr;
    }
    if (m_layers_slider) {
        delete m_layers_slider;
        m_layers_slider = nullptr;
    }
}

void GCodeViewer::init(ConfigOptionMode mode, PresetBundle* preset_bundle)
{
    if (m_gl_data_initialized)
        return;

    std::string filename;
    if (preset_bundle != nullptr) {
        const Preset* curr = &preset_bundle->printers.get_selected_preset();
        if (curr->is_system)
            filename = PresetUtils::system_printer_hotend_model(*curr);
        else {
            auto *printer_model = curr->config.opt<ConfigOptionString>("printer_model");
            if (printer_model != nullptr && ! printer_model->value.empty()) {
                filename = preset_bundle->get_hotend_model_for_printer_model(printer_model->value);
            }

            if (filename.empty()) {
                filename = preset_bundle->get_hotend_model_for_printer_model(PresetBundle::ORCA_DEFAULT_PRINTER_MODEL);
            }
        }
    }

    m_marker_filename = filename;
    m_sequential_view.marker.init(filename);

    std::array<int, 2> point_sizes;
    ::glGetIntegerv(GL_ALIASED_POINT_SIZE_RANGE, point_sizes.data());
    m_detected_point_sizes = { static_cast<float>(point_sizes[0]), static_cast<float>(point_sizes[1]) };

    m_user_mode = mode;
    update_by_mode(m_user_mode);

    m_layers_slider->init_texture();

    m_gl_data_initialized = true;

    try
    {
        m_viewer.init(reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        glcheck();
    }
    catch (const std::exception& e)
    {
        MessageDialog msg_dlg(wxGetApp().plater(), e.what(), _L("Error"), wxICON_ERROR | wxOK);
        msg_dlg.ShowModal();
    }

    m_nozzle_nums = preset_bundle ? preset_bundle->get_printer_extruder_count() : 1;
    apply_default_view_type();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": finished");
}

void GCodeViewer::on_change_color_mode(bool is_dark) {
    m_is_dark = is_dark;
    m_sequential_view.marker.on_change_color_mode(m_is_dark);
    m_sequential_view.gcode_window.on_change_color_mode(m_is_dark);
}

void GCodeViewer::set_scale(float scale)
{
    if(m_scale != scale)m_scale = scale;
    if (m_sequential_view.m_scale != scale) {
        m_sequential_view.m_scale = scale;
        m_sequential_view.marker.m_scale = scale;
        m_sequential_view.gcode_window.m_scale = scale;
    }
}

static const std::array<const char*, size_t(libvgcode::ENonplanarColor::COUNT)> nonplanar_color_names{ "shell", "band", "bead_height", "slope" };

std::string GCodeViewer::view_type_to_config_name(libvgcode::EViewType type)
{
    for (const auto& [name, value] : view_type_config_map()) {
        if (value == type)
            return name;
    }
    return std::string();
}

bool GCodeViewer::view_type_from_config_name(const std::string& name, libvgcode::EViewType& type)
{
    for (const auto& [config_name, value] : view_type_config_map()) {
        if (config_name == name) {
            type = value;
            return true;
        }
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> GCodeViewer::default_view_type_choices()
{
    std::vector<std::pair<std::string, std::string>> choices = {
        { "auto", _u8L("Automatic") },
        { "last", _u8L("Last used") },
    };
    for (const auto& [name, type] : view_type_config_map())
        choices.push_back({ name, get_view_type_string(type) });
    return choices;
}

void GCodeViewer::select_view_type(libvgcode::EViewType type)
{
    auto it = std::find(view_type_items.begin(), view_type_items.end(), type);
    m_view_type_sel = (it != view_type_items.end()) ? static_cast<int>(std::distance(view_type_items.begin(), it)) : 0;
    set_view_type(type);
}

void GCodeViewer::apply_default_view_type()
{
    const std::string nonplanar_color = wxGetApp().app_config->get("preview_nonplanar_color");
    for (size_t i = 0; i < nonplanar_color_names.size(); ++i)
        if (nonplanar_color == nonplanar_color_names[i])
            m_viewer.set_nonplanar_color(libvgcode::ENonplanarColor(i));

    const std::string preference = wxGetApp().app_config->get("preview_default_view_type");

    std::string key = preference;
    libvgcode::EViewType type = libvgcode::EViewType::FeatureType;
    if (preference == "last") {
        if (!view_type_from_config_name(wxGetApp().app_config->get("preview_last_view_type"), type))
            type = libvgcode::EViewType::FeatureType;
    }
    else if (!view_type_from_config_name(preference, type)) {
        const bool multi_material = m_viewer.get_used_extruders_count() > 1;
        type = multi_material ? libvgcode::EViewType::ColorPrint : libvgcode::EViewType::FeatureType;
        key = multi_material ? "auto_multi_material" : "auto_single_material";
    }

    if (m_applied_default_view_type_key == key)
        return;

    m_applied_default_view_type_key = key;
    select_view_type(type);
}

void GCodeViewer::update_by_mode(ConfigOptionMode mode)
{
    view_type_items.clear();
    view_type_items_str.clear();
    options_items.clear();

    view_type_items.push_back(libvgcode::EViewType::Summary);
    view_type_items.push_back(libvgcode::EViewType::FeatureType);
    view_type_items.push_back(libvgcode::EViewType::ColorPrint);
    view_type_items.push_back(libvgcode::EViewType::Speed);
    view_type_items.push_back(libvgcode::EViewType::ActualSpeed);
    view_type_items.push_back(libvgcode::EViewType::Acceleration);
    view_type_items.push_back(libvgcode::EViewType::Jerk);
    view_type_items.push_back(libvgcode::EViewType::Clearance);
    view_type_items.push_back(libvgcode::EViewType::Nonplanar);
    view_type_items.push_back(libvgcode::EViewType::Height);
    view_type_items.push_back(libvgcode::EViewType::Width);
    view_type_items.push_back(libvgcode::EViewType::VolumetricFlowRate);
    view_type_items.push_back(libvgcode::EViewType::ActualVolumetricFlowRate);
    view_type_items.push_back(libvgcode::EViewType::LayerTimeLinear);
    view_type_items.push_back(libvgcode::EViewType::LayerTimeLogarithmic);
    view_type_items.push_back(libvgcode::EViewType::FanSpeed);
    view_type_items.push_back(libvgcode::EViewType::Temperature);
    view_type_items.push_back(libvgcode::EViewType::PressureAdvance);

    for (int i = 0; i < view_type_items.size(); i++) {
        view_type_items_str.push_back(get_view_type_string(view_type_items[i]));
    }

    view_type_items.push_back(libvgcode::EViewType::Tool);

    options_items.push_back(EMoveType::Travel);
    options_items.push_back(EMoveType::Retract);
    options_items.push_back(EMoveType::Unretract);
    options_items.push_back(EMoveType::Wipe);
    options_items.push_back(EMoveType::Seam);
}

std::vector<int> GCodeViewer::get_plater_extruder()
{
    return m_plater_extruder;
}

static Transform3d compute_belt_back_transform(const PrintConfig& cfg)
{
    if (!cfg.belt_printer.value)
        return Transform3d::Identity();

    MachineFrameTransform mft;
    mft.init_from_config(cfg);
    const Transform3d mf_inv = mft.is_active() ? Transform3d(mft.transform().inverse())
                                               : Transform3d::Identity();

    Transform3d ar = Transform3d::Identity();
    const int rr[3] = { int(cfg.gcode_remap_x.value), int(cfg.gcode_remap_y.value), int(cfg.gcode_remap_z.value) };
    if (rr[0] != 0 || rr[1] != 1 || rr[2] != 2) {
        BoundingBoxf bbox_bed(cfg.printable_area.values);
        const Vec3d vmax(bbox_bed.max.x(), bbox_bed.max.y(), cfg.printable_height.value);
        Matrix3d M = Matrix3d::Zero();
        Vec3d    t = Vec3d::Zero();
        for (int i = 0; i < 3; ++i) {
            const int axis = rr[i] % 3;
            if (rr[i] < 3)      M(i, axis) =  1.0;
            else if (rr[i] < 6) M(i, axis) = -1.0;
            else              { M(i, axis) = -1.0; t[i] = vmax[axis]; }
        }
        ar.linear()      = M;
        ar.translation() = t;
    }
    const Transform3d ar_inv = ar.inverse();

    return ar_inv * mf_inv;
}

void GCodeViewer::load_as_gcode(const GCodeProcessorResult& gcode_result, const Print& print, const std::vector<std::string>& str_tool_colors,
                const std::vector<std::string>& str_color_print_colors, const BuildVolume& build_volume,
                const std::vector<BoundingBoxf3>& exclude_bounding_box, ConfigOptionMode mode, bool only_gcode)
{
    m_loaded_as_preview = false;

    m_belt_view_enabled = gcode_result.belt_tilt_angle > 0.f;
    m_belt_angle_deg    = gcode_result.belt_tilt_angle;

    const bool current_top_layer_only = m_viewer.is_top_layer_only_view_range();
    const bool required_top_layer_only = get_app_config()->get_bool("seq_top_layer_only");
    if (current_top_layer_only != required_top_layer_only)
        m_viewer.toggle_top_layer_only_view_range();

    m_viewer.set_dim_previous_layers(get_app_config()->get_bool("preview_dim_previous_layers"));
    m_viewer.set_dim_previous_layers_brightness(0.01f * std::stoi(get_app_config()->get("preview_dim_previous_layers_brightness")));

    const bool same_belt_view = !m_belt_view_enabled || m_last_belt_show_designed == m_belt_show_designed;
    if (m_last_result_id == gcode_result.id && wxGetApp().is_editor() && same_belt_view) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": the same id %1%, return directly, result %2% ") % m_last_result_id % (&gcode_result);

        libvgcode::Palette tools_colors;
        tools_colors.reserve(str_tool_colors.size());
        for (const std::string& color : str_tool_colors) {
            tools_colors.emplace_back(libvgcode::convert(color));
        }
        m_viewer.set_tool_colors(tools_colors);

        libvgcode::Palette color_print_colors;
        const std::vector<std::string>& str_colors = str_color_print_colors.empty() ? str_tool_colors : str_color_print_colors;
        color_print_colors.reserve(str_colors.size());
        for (const std::string& color : str_colors) {
            color_print_colors.emplace_back(libvgcode::convert(color));
        }
        m_viewer.set_color_print_colors(color_print_colors);
        return;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": gcode result %1%, new id %2%, gcode file %3% ") % (&gcode_result) % m_last_result_id % gcode_result.filename;

    reset();
    wxGetApp().plater()->sidebar().set_clearance_warning({});
    wxGetApp().plater()->get_notification_manager()->close_notification_of_type(NotificationType::ClearanceWarning);
    wxGetApp().plater()->sidebar().set_nonplanar_summary(nullptr);
    wxGetApp().plater()->get_notification_manager()->close_notification_of_type(NotificationType::NonplanarWarning);

    wxGetApp().plater()->suppress_background_process(true);
    gcode_result.lock();
    if (gcode_result.moves.size() == 0) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": gcode result reset before, return directly!");
        gcode_result.unlock();
        wxGetApp().plater()->schedule_background_process();
        return;
    }
    m_plate_mass    = gcode_result.plate_mass;
    m_object_masses = gcode_result.object_masses;
    m_body_masses   = gcode_result.body_masses;
    m_support_masses = gcode_result.support_masses;

    const bool is_belt = m_belt_view_enabled && print.config().belt_printer.value;
    Transform3d belt_inv = (is_belt && m_belt_show_designed)
        ? compute_belt_back_transform(print.config()) : Transform3d::Identity();
    if (is_belt && m_belt_show_designed && gcode_result.belt_z_origin != 0.0f)
        belt_inv = belt_inv * Transform3d(Eigen::Translation3d(Vec3d(0.0, 0.0, -double(gcode_result.belt_z_origin))));
    const bool apply_belt = is_belt && m_belt_show_designed
        && !belt_inv.matrix().isApprox(Transform3d::Identity().matrix());
    if (apply_belt) {
        BoundingBoxf3 model_bb;
        for (const PrintObject* po : print.objects())
            for (const ModelInstance* mi : po->model_object()->instances)
                model_bb.merge(po->model_object()->instance_bounding_box(*mi));
        constexpr double ANCHOR_CLIP_MARGIN_MM   = 10.0;
        constexpr double ANCHOR_CLIP_MIN_KEEP    = 0.5;
        BoundingBoxf3 tp_bb_clip, tp_bb_full;
        size_t n_filtered = 0, n_clip = 0;
        const double y_lo = model_bb.defined ? model_bb.min.y() - ANCHOR_CLIP_MARGIN_MM : -1e30;
        const double y_hi = model_bb.defined ? model_bb.max.y() + ANCHOR_CLIP_MARGIN_MM :  1e30;
        auto is_object_body = [](ExtrusionRole r) {
            return r != erSupportMaterial && r != erSupportMaterialInterface
                && r != erSupportTransition
                && r != erSkirt && r != erBrim && r != erWipeTower;
        };
        auto accumulate = [&](bool body_only) {
            tp_bb_clip = BoundingBoxf3();
            tp_bb_full = BoundingBoxf3();
            n_filtered = 0;
            n_clip     = 0;
            for (const GCodeProcessorResult::MoveVertex& mv : gcode_result.moves)
                if (mv.type == EMoveType::Extrude && mv.layer_id >= 1
                    && (!body_only || is_object_body(mv.extrusion_role))) {
                    ++n_filtered;
                    const Vec3d p = belt_inv * mv.position.cast<double>();
                    tp_bb_full.merge(p);
                    if (p.y() >= y_lo && p.y() <= y_hi) {
                        tp_bb_clip.merge(p);
                        ++n_clip;
                    }
                }
        };
        accumulate( true);
        if (n_filtered == 0)
            accumulate( false);
        const BoundingBoxf3& tp_bb =
            (tp_bb_clip.defined && n_filtered > 0 &&
             double(n_clip) >= ANCHOR_CLIP_MIN_KEEP * double(n_filtered)) ? tp_bb_clip : tp_bb_full;
        if (model_bb.defined && tp_bb.defined) {
            const Vec3d d = model_bb.min - tp_bb.min;
            belt_inv = Transform3d(Eigen::Translation3d(d)) * belt_inv;
        }
    }
    libvgcode::GCodeInputData data = libvgcode::convert(gcode_result, str_tool_colors, str_color_print_colors, m_viewer,
        apply_belt ? &belt_inv : nullptr);

    m_nonplanar_shells = 0;
    m_nonplanar_bands.clear();
    for (const libvgcode::PathVertex &v : data.vertices)
        if (v.is_extrusion() && v.is_nonplanar()) {
            m_nonplanar_shells = std::max(m_nonplanar_shells, int(v.nonplanar_shell) + 1);
            m_nonplanar_bands.emplace_back(v.nonplanar_band);
        }
    sort_remove_duplicates(m_nonplanar_bands);
    load_nonplanar_report(gcode_result, data);

    const auto &report = gcode_result.clearance_report;
    if (report.ran && gcode_result.clearance.size() == gcode_result.moves.size()) {
        m_clearance_report.ran = true;
        m_clearance_report.hit_moves = report.hit_moves;
        m_clearance_report.min_clearance = report.min_clearance;
        m_clearance_report.worst = report.worst;
        m_clearance_report.omissions = report.omissions;
        const auto head = Nonplanar::resolve_head_envelope(print.config());
        m_clearance_margin = static_cast<float>(head.margin);
        double floor = std::numeric_limits<double>::infinity();
        for (const auto &box : head.boxes)
            if (!box.nozzle && box.min.z() > 0.)
                floor = std::min(floor, box.min.z());
        m_clearance_head_floor = std::isfinite(floor) ? static_cast<float>(floor) : std::max(1.f, m_clearance_margin);
        data.clearance_margin = m_clearance_margin;
        data.clearance_head_floor = m_clearance_head_floor;
        m_clearance_move_vertices.assign(gcode_result.moves.size(), std::numeric_limits<uint32_t>::max());
        for (size_t i = 0; i < data.vertices.size(); ++i)
            if (data.vertices[i].source_move_id < m_clearance_move_vertices.size())
                m_clearance_move_vertices[data.vertices[i].source_move_id] = static_cast<uint32_t>(i);
        if (report.hit_moves != 0 && !report.worst.empty()) {
            const auto &hit = report.worst.front();
            m_clearance_warning = into_u8(format_wxstr(
                _L("Moves that would hit printed material: %1% (worst clearance: %2% mm, %3%, layer %4%, line %5%)."),
                report.hit_moves, wxString::Format("%.3f", report.min_clearance), from_u8(Nonplanar::head_box_label(hit.component)), hit.layer + 1,
                hit.gcode_line));
            wxGetApp().plater()->sidebar().set_clearance_warning(m_clearance_warning, hit.move_id, gcode_result.id);
            wxGetApp().plater()->get_notification_manager()->push_notification(NotificationType::ClearanceWarning,
                NotificationManager::NotificationLevel::WarningNotificationLevel, m_clearance_warning,
                _u8L("Jump to move"), [move_id = hit.move_id, result_id = gcode_result.id](wxEvtHandler*) {
                    wxGetApp().CallAfter([move_id, result_id] {
                        if (!wxTheApp || wxGetApp().is_closing() || !wxGetApp().plater())
                            return;
                        auto *plater = wxGetApp().plater();
                        plater->select_view_3D("Preview");
                        if (auto *canvas = plater->get_preview_canvas3D())
                            canvas->get_gcode_viewer().jump_to_clearance_move(move_id, result_id);
                    });
                    return true;
                });
        }
    }

    m_viewer.reset_default_extrusion_roles_colors();
    m_viewer.load(std::move(data));

    m_belt_layer_zs.clear();
    if (is_belt || gcode_result.nonplanar) {
        unsigned int src_layer_id = std::numeric_limits<unsigned int>::max();
        for (size_t i = 1; i < gcode_result.moves.size(); ++ i) {
            const GCodeProcessorResult::MoveVertex &mv = gcode_result.moves[i];
            const GCodeProcessorResult::MoveState &state = gcode_result.state(mv);
            double z = state.print_z;
            if (z <= 0. && !is_belt && mv.type == EMoveType::Extrude && state.nonplanar_patch_shell == 0xFF)
                z = mv.position.z();
            if (mv.layer_id != src_layer_id) {
                src_layer_id = mv.layer_id;
                m_belt_layer_zs.emplace_back(z);
            } else
                m_belt_layer_zs.back() = std::max(m_belt_layer_zs.back(), z);
        }
        if (m_belt_layer_zs.size() != m_viewer.get_layers_count() ||
            (!is_belt && std::adjacent_find(m_belt_layer_zs.begin(), m_belt_layer_zs.end(), std::greater_equal<double>()) != m_belt_layer_zs.end()))
            m_belt_layer_zs.clear();
    }

    const libvgcode::AABox bbox = wxGetApp().is_gcode_viewer() ?
        m_viewer.get_bounding_box() :
        m_viewer.get_extrusion_bounding_box({
            libvgcode::EGCodeExtrusionRole::Perimeter, libvgcode::EGCodeExtrusionRole::ExternalPerimeter, libvgcode::EGCodeExtrusionRole::OverhangPerimeter,
            libvgcode::EGCodeExtrusionRole::InternalInfill, libvgcode::EGCodeExtrusionRole::SolidInfill, libvgcode::EGCodeExtrusionRole::TopSolidInfill,
            libvgcode::EGCodeExtrusionRole::Ironing, libvgcode::EGCodeExtrusionRole::BridgeInfill, libvgcode::EGCodeExtrusionRole::GapFill,
            libvgcode::EGCodeExtrusionRole::Skirt, libvgcode::EGCodeExtrusionRole::SupportMaterial, libvgcode::EGCodeExtrusionRole::SupportMaterialInterface,
            libvgcode::EGCodeExtrusionRole::WipeTower,
            libvgcode::EGCodeExtrusionRole::BottomSurface, libvgcode::EGCodeExtrusionRole::InternalBridgeInfill, libvgcode::EGCodeExtrusionRole::Brim,
            libvgcode::EGCodeExtrusionRole::SupportTransition, libvgcode::EGCodeExtrusionRole::Mixed
            });
    m_paths_bounding_box = BoundingBoxf3(libvgcode::convert(bbox[0]).cast<double>(), libvgcode::convert(bbox[1]).cast<double>());
    m_max_bounding_box = m_paths_bounding_box;

    if (wxGetApp().is_editor()) {
        if (is_belt) {
            BoundingBoxf3 bed = wxGetApp().plater()->build_volume().bounding_volume();
            bed.offset(1.);
            m_contained_in_bed = !m_paths_bounding_box.defined || (bed.contains(m_paths_bounding_box.min) && bed.contains(m_paths_bounding_box.max));
        } else
            m_contained_in_bed = wxGetApp().plater()->build_volume().all_paths_inside(gcode_result, m_paths_bounding_box);
    }

    m_extruders_count = gcode_result.filaments_count;

    m_last_result_id = gcode_result.id;
    m_last_belt_show_designed = m_belt_show_designed;
    m_gcode_result = &gcode_result;
    m_move_type_counts.fill(0);
    for (auto& move_type_times : m_move_type_times)
        move_type_times.fill(0.0f);
    m_move_type_distances = gcode_result.move_type_distances;
    for (const GCodeProcessorResult::MoveVertex& move : gcode_result.moves) {
        if (move.internal_only)
            continue;

        const size_t move_type = static_cast<size_t>(move.type);
        if (move_type < m_move_type_counts.size()) {
            ++m_move_type_counts[move_type];
            for (size_t mode = 0; mode < move.time.size(); ++mode)
                m_move_type_times[move_type][mode] += move.time[mode];
        }
    }
    m_only_gcode_in_preview = only_gcode;

    m_sequential_view.gcode_window.load_gcode(gcode_result.filename, gcode_result.lines_ends);

    if (m_only_gcode_in_preview)
        m_custom_gcode_per_print_z = gcode_result.custom_gcode_per_print_z;

    m_max_print_height = gcode_result.printable_height;
    m_machine_frame_transform_active = gcode_result.machine_frame_transform_active;
    m_z_offset = gcode_result.z_offset;


    apply_default_view_type();

    m_nozzle_nums = print.config().option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    if (!print.is_BBL_printer()) m_nozzle_nums = 1;
    std::vector<int>         filament_maps = print.get_filament_maps();
    std::vector<std::string> color_opt     = print.config().option<ConfigOptionStrings>("filament_colour")->values;
    std::vector<std::string> type_opt      = print.config().option<ConfigOptionStrings>("filament_type")->values;
    std::vector<unsigned char> support_filament_opt = print.config().option<ConfigOptionBools>("filament_is_support")->values;
    for (auto extruder_id : m_viewer.get_used_extruders_ids()) {
        if (filament_maps[extruder_id] == 1) {
            m_left_extruder_filament.push_back({type_opt[extruder_id], color_opt[extruder_id], extruder_id, (bool)(support_filament_opt[extruder_id])});
        } else {
            m_right_extruder_filament.push_back({type_opt[extruder_id], color_opt[extruder_id], extruder_id, (bool)(support_filament_opt[extruder_id])});
        }
    }

    m_settings_ids = gcode_result.settings_ids;
    m_filament_diameters = gcode_result.filament_diameters;
    m_filament_densities = gcode_result.filament_densities;
    m_sequential_view.m_show_marker = false;

    if (m_only_gcode_in_preview) {
        Pointfs printable_area;
        Pointfs bed_exclude_area = Pointfs();
        Pointfs wrapping_exclude_area = Pointfs();
        std::vector<Pointfs> extruder_areas;
        std::vector<double> extruder_heights;
        std::string texture;
        std::string model;

        if (!gcode_result.printable_area.empty()) {
            printable_area = gcode_result.printable_area;
            const auto bundle = wxGetApp().preset_bundle;
            if (bundle != nullptr && !m_settings_ids.printer.empty()) {
                const Preset* preset = bundle->printers.find_preset(m_settings_ids.printer);
                if (preset != nullptr) {
                    model = PresetUtils::system_printer_bed_model(*preset);
                    texture = PresetUtils::system_printer_bed_texture(*preset);
                }
            }

            if (!gcode_result.bed_exclude_area.empty())
                bed_exclude_area = gcode_result.bed_exclude_area;

            if (!gcode_result.wrapping_exclude_area.empty())
                wrapping_exclude_area = gcode_result.wrapping_exclude_area;

            if (!gcode_result.extruder_areas.empty())
                extruder_areas = gcode_result.extruder_areas;
            if (!gcode_result.extruder_heights.empty())
                extruder_heights = gcode_result.extruder_heights;

            wxGetApp().plater()->set_bed_shape(printable_area, bed_exclude_area, wrapping_exclude_area, gcode_result.printable_height, extruder_areas, extruder_heights, texture, model, gcode_result.printable_area.empty());
        }
    }

    m_print_statistics = gcode_result.print_statistics;

    PrintEstimatedStatistics::ETimeMode time_mode = convert(m_viewer.get_time_mode());
    if (m_viewer.get_time_mode() != libvgcode::ETimeMode::Normal) {
        const float time = m_print_statistics.modes[static_cast<size_t>(time_mode)].time;
        if (time == 0.0f ||
            short_time(get_time_dhms(time)) == short_time(get_time_dhms(m_print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time)))
            m_viewer.set_time_mode(libvgcode::convert(PrintEstimatedStatistics::ETimeMode::Normal));
    }

    bool only_gcode_3mf = false;
    PartPlate* current_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    bool current_has_print_instances = current_plate->has_printable_instances();
    if (current_plate->is_slice_result_valid() && wxGetApp().model().objects.empty() && !current_has_print_instances)
        only_gcode_3mf = true;
    m_layers_slider->set_menu_enable(!(only_gcode || only_gcode_3mf));
    m_layers_slider->set_as_dirty();
    m_moves_slider->set_as_dirty();

    m_conflict_result = gcode_result.conflict_result;
    if (m_conflict_result) { m_conflict_result.value().layer = m_viewer.get_layer_id_at(static_cast<float>(m_conflict_result.value()._height)); }

    m_gcode_check_result = gcode_result.gcode_check_result;

    filament_printable_reuslt = gcode_result.filament_printable_reuslt;
    gcode_result.unlock();
    wxGetApp().plater()->schedule_background_process();
}

void GCodeViewer::load_as_preview(libvgcode::GCodeInputData&& data)
{
    wxGetApp().plater()->sidebar().set_clearance_warning({});
    wxGetApp().plater()->get_notification_manager()->close_notification_of_type(NotificationType::ClearanceWarning);
    wxGetApp().plater()->sidebar().set_nonplanar_summary(nullptr);
    wxGetApp().plater()->get_notification_manager()->close_notification_of_type(NotificationType::NonplanarWarning);
    m_loaded_as_preview = true;
    m_plate_mass = {};
    m_object_masses.clear();
    m_body_masses.clear();
    m_support_masses.clear();

    m_move_type_counts.fill(0);
    for (auto& move_type_times : m_move_type_times)
        move_type_times.fill(0.0f);
    m_move_type_distances.fill(0.0f);
    const size_t normal_time_mode_idx = static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal);
    for (size_t i = 0; i < data.vertices.size(); ++i) {
        const libvgcode::PathVertex& vertex = data.vertices[i];
        const size_t move_type = static_cast<size_t>(vertex.type);
        if (move_type < m_move_type_counts.size()) {
            ++m_move_type_counts[move_type];
            for (size_t mode = 0; mode < vertex.times.size(); ++mode)
                m_move_type_times[move_type][mode] += vertex.times[mode];
            if (vertex.type == libvgcode::EMoveType::Retract || vertex.type == libvgcode::EMoveType::Unretract) {
                m_move_type_distances[move_type] += std::fabs(vertex.feedrate) * vertex.times[normal_time_mode_idx];
            } else if (i > 0) {
                const float dx = vertex.position[0] - data.vertices[i - 1].position[0];
                const float dy = vertex.position[1] - data.vertices[i - 1].position[1];
                const float dz = vertex.position[2] - data.vertices[i - 1].position[2];
                m_move_type_distances[move_type] += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }
    }

    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::Skirt,                    { 127, 255, 127 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::ExternalPerimeter,        { 255, 255, 0 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::SupportMaterial,          { 127, 255, 127 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::SupportMaterialInterface, { 127, 255, 127 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::InternalInfill,           { 255, 127, 127 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::SolidInfill,              { 255, 127, 127 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::WipeTower,                { 127, 255, 127 });
    m_viewer.load(std::move(data));

    const libvgcode::AABox bbox = m_viewer.get_extrusion_bounding_box();
    const BoundingBoxf3 paths_bounding_box(libvgcode::convert(bbox[0]).cast<double>(), libvgcode::convert(bbox[1]).cast<double>());
    m_contained_in_bed = wxGetApp().plater()->build_volume().all_paths_inside(GCodeProcessorResult(), paths_bounding_box);
}

void GCodeViewer::update_shells_color_by_extruder(const DynamicPrintConfig *config)
{
    if (config != nullptr)
        m_shells.volumes.update_colors_by_extruder(config, false);
}

void GCodeViewer::set_shell_transparency(float alpha) { m_shells.volumes.set_transparency(alpha); }

void GCodeViewer::reset_shell()
{
    m_shells.volumes.clear();
    m_shells.print_id = -1;
    m_shell_bounding_box = BoundingBoxf3();
}

void GCodeViewer::reset()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": current result id %1% ")%m_last_result_id;
    m_last_result_id = -1;
    m_belt_layer_zs.clear();
    m_clearance_report = {};
    m_clearance_move_vertices.clear();
    m_clearance_warning.clear();
    m_nonplanar_report = {};
    m_nonplanar_hatch.clear();
    m_nonplanar_hover_grid = {};
    m_nonplanar_fallback_z.clear();
    m_nonplanar_selected = -1;
    m_only_gcode_in_preview = false;

    m_viewer.reset();

    m_paths_bounding_box = BoundingBoxf3();
    m_max_bounding_box = BoundingBoxf3();
    m_max_print_height = 0.0f;
    m_machine_frame_transform_active = false;
    m_z_offset = 0.0f;
    m_extruders_count = 0;
    m_filament_diameters = std::vector<float>();
    m_filament_densities = std::vector<float>();
    m_move_type_counts.fill(0);
    for (auto& move_type_times : m_move_type_times)
        move_type_times.fill(0.0f);
    m_move_type_distances.fill(0.0f);
    m_print_statistics.reset();
    m_custom_gcode_per_print_z = std::vector<CustomGCode::Item>();
    m_plate_mass = {};
    m_object_masses.clear();
    m_body_masses.clear();
    m_support_masses.clear();
    m_left_extruder_filament.clear();
    m_right_extruder_filament.clear();
    m_sequential_view.gcode_window.reset();
    m_contained_in_bed = true;
}

GCodeViewer::ImexMarkerPlan GCodeViewer::resolve_imex_marker_plan(const DynamicPrintConfig& printer_cfg,
                                                                  const std::string&        mode,
                                                                  const BoundingBoxf&       bed_extents)
{
    ImexMarkerPlan plan;

    if (imex_cfg_bool(printer_cfg, "imex_firmware_managed_zones"))
        return plan;
    if (mode.empty() || mode == kImexPrimaryMode)
        return plan;

    const int tools_per_gantry = std::max(1, imex_cfg_int(printer_cfg, "imex_tools_per_gantry"));
    plan.box_wx = (float)imex_cfg_float(printer_cfg, "imex_nozzle_clearance_x");
    plan.box_wy = (float)imex_cfg_float(printer_cfg, "imex_nozzle_clearance_y");

    int pri_tool = -1;
    std::vector<int> sec_tool_ids;
    std::map<int, ImexRole> sec_tool_roles;
    std::set<int>     sec_aggregated;
    {
        const std::string entry = find_imex_mode(printer_cfg, mode).active_tools;
        pri_tool = imex_primary_tool_for_mode(entry);
        const ImexGantryGrouping grouping =
            group_imex_active_tools_by_gantry(entry, tools_per_gantry);
        auto add_tool = [&](int phys_idx, ImexRole role, bool aggregated) {
            if (phys_idx < 0 || phys_idx == pri_tool) return;
            switch (role) {
            case ImexRole::Copy:
            case ImexRole::Mirror:
                break;
            case ImexRole::Primary:
            case ImexRole::Span:
                return;
            }
            sec_tool_roles[phys_idx] = role;
            sec_tool_ids.push_back(phys_idx);
            if (aggregated) sec_aggregated.insert(phys_idx);
        };
        for (const auto& grp : grouping.groups) {
            if (grp.aggregate && grp.gantry_index != grouping.primary_gantry)
                add_tool(grp.representative_phys, grp.representative_role, true);
            else
                for (const auto& [phys_idx, role] : grp.tools)
                    add_tool(phys_idx, role, false);
        }
    }
    const int sec_count = (int)sec_tool_ids.size();
    if (sec_count == 0)
        return plan;

    const ImexZoneLayout zone_layout =
        compute_imex_zone_layout(printer_cfg, mode, std::string(), bed_extents);

    Vec2d zone0_center = bed_extents.center();
    if (!zone_layout.head_zone_centers.empty()) {
        zone0_center = zone_layout.head_zone_centers.begin()->second;
        for (const auto& head_center : zone_layout.head_zone_centers) {
            zone0_center.x() = std::min(zone0_center.x(), head_center.second.x());
            zone0_center.y() = std::min(zone0_center.y(), head_center.second.y());
        }
    }
    auto zone_center_of = [&](int tid) -> Vec2d {
        auto it = zone_layout.head_zone_centers.find(tid);
        return (it == zone_layout.head_zone_centers.end()) ? zone0_center : it->second;
    };
    const Vec2d pri_center = zone_center_of(pri_tool);

    auto sec_center_of = [&](int tid) -> Vec2d {
        Vec2d c = zone_center_of(tid);
        if (sec_aggregated.count(tid))
            c.x() = pri_center.x();
        return c;
    };

    plan.pri_box_offset_x = (pri_center.x() <= zone0_center.x()) ? 0.0f : -plan.box_wx;
    plan.pri_box_offset_y = -plan.box_wy;
    for (int i = 0; i < sec_count; ++i) {
        const Vec2d sc = sec_center_of(sec_tool_ids[i]);
        if      (sc.x() > pri_center.x()) { plan.pri_box_offset_x = 0.0f;         }
        else if (sc.x() < pri_center.x()) { plan.pri_box_offset_x = -plan.box_wx; }
        if      (sc.y() > pri_center.y()) { plan.pri_box_offset_y = 0.0f;         }
        else if (sc.y() < pri_center.y()) { plan.pri_box_offset_y = -plan.box_wy; }
    }

    plan.pri_head = pri_tool;
    plan.carriages.reserve(sec_count);
    for (int i = 0; i < sec_count; ++i) {
        const int   sec_tool   = sec_tool_ids[i];
        const Vec2d sec_center = sec_center_of(sec_tool);
        const ImexRole sec_role = sec_tool_roles[sec_tool];

        const bool is_mirror    = (sec_role == ImexRole::Mirror);
        const bool cross_gantry = imex_mirror_axis_for(pri_tool, sec_tool,
                                                       tools_per_gantry) == ImexMirrorAxis::Y;

        ImexMarkerPlan::Carriage carriage;
        carriage.phys_head = sec_tool;
        carriage.mirror_x = is_mirror && !cross_gantry;
        carriage.mirror_y = is_mirror &&  cross_gantry;
        carriage.x_term   = carriage.mirror_x ? (float)(sec_center.x() + pri_center.x())
                                              : (float)(sec_center.x() - pri_center.x());
        carriage.y_term   = carriage.mirror_y ? (float)(sec_center.y() + pri_center.y())
                                              : (float)(sec_center.y() - pri_center.y());

        if (is_mirror && !cross_gantry) {
            if      (sec_center.x() > pri_center.x()) carriage.box_offset_x = -plan.box_wx;
            else if (sec_center.x() < pri_center.x()) carriage.box_offset_x = 0.0f;
            else                                      carriage.box_offset_x = plan.pri_box_offset_x;
        } else {
            carriage.box_offset_x = plan.pri_box_offset_x;
        }
        if      (sec_center.y() > pri_center.y()) carriage.box_offset_y = -plan.box_wy;
        else if (sec_center.y() < pri_center.y()) carriage.box_offset_y = 0.0f;
        else                                      carriage.box_offset_y = plan.pri_box_offset_y;

        plan.carriages.push_back(carriage);
    }

    return plan;
}

void GCodeViewer::render_scene(int canvas_width, int canvas_height)
{
    glsafe(::glEnable(GL_DEPTH_TEST));
    render_shells(canvas_width, canvas_height);

    if (m_viewer.get_extrusion_roles_count() == 0)
        return;

    render_toolpaths();
    render_nonplanar_hatch();

    auto current = m_viewer.get_view_visible_range();
    auto endpoints = m_viewer.get_view_full_range();
    m_sequential_view.m_show_marker = m_sequential_view.m_show_marker || (current.back() != endpoints.back() && !m_no_render_path);
    const libvgcode::PathVertex& curr_vertex = m_viewer.get_current_vertex();
    m_sequential_view.marker.set_world_position(libvgcode::convert(curr_vertex.position));
    m_sequential_view.marker.set_z_offset(m_z_offset + 0.5f);

    static const std::array<ColorRGBA, 4> s_carriage_colors = {{
        { 1.000f, 1.000f, 1.000f, 0.65f },
        { 0.941f, 0.894f, 0.259f, 0.65f },
        { 0.835f, 0.369f, 0.000f, 0.65f },
        { 0.800f, 0.475f, 0.655f, 0.65f },
    }};
    struct CarriageDraw { Vec3f pos; ColorRGBA color; float box_offset_x = 0.0f; float box_offset_y = 0.0f; };
    std::vector<CarriageDraw> carriage_box_draws;
    float imex_box_wx = 0.0f, imex_box_wy = 0.0f;
    {
        PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        bool imex_active = false;
        if (preset_bundle && m_sequential_view.m_show_marker) {
            const DynamicPrintConfig& printer_cfg = preset_bundle->printers.get_edited_preset().config;
            auto* is_imex_opt = printer_cfg.opt<ConfigOptionBool>("is_imex");
            if (is_imex_opt && is_imex_opt->value) {
                const DynamicPrintConfig& process_cfg = preset_bundle->prints.get_edited_preset().config;
                auto* mode_opt = process_cfg.opt<ConfigOptionString>("imex_parallel_mode");
                std::string mode = mode_opt ? mode_opt->value : kImexPrimaryMode;
                PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
                PartPlate*     curr_plate = plate_list.get_curr_plate();
                if (curr_plate) {
                    const std::string plate_mode = curr_plate->get_effective_imex_mode();
                    if (plate_mode != kImexPrimaryMode)
                        mode = plate_mode;
                }

                BoundingBoxf bed_extents;
                {
                    const Pointfs& plate_shape = curr_plate ? curr_plate->get_shape() : Pointfs{};
                    if (!plate_shape.empty()) {
                        bed_extents = get_extents(plate_shape);
                    } else {
                        bed_extents = BoundingBoxf(Vec2d(m_paths_bounding_box.min.x(), m_paths_bounding_box.min.y()),
                                                   Vec2d(m_paths_bounding_box.max.x(), m_paths_bounding_box.max.y()));
                    }
                }

                static const std::vector<std::string> s_no_strings;
                auto* names_opt  = printer_cfg.opt<ConfigOptionStrings>("imex_mode_names");
                auto* tools_opt  = printer_cfg.opt<ConfigOptionStrings>("imex_mode_active_tools");
                const std::vector<std::string>& mode_names   = names_opt ? names_opt->values : s_no_strings;
                const std::vector<std::string>& active_tools = tools_opt ? tools_opt->values : s_no_strings;

                ImexMarkerKey::Scalars key;
                key.plate_index      = plate_list.get_curr_plate_index();
                key.gantry_count     = imex_cfg_int(printer_cfg, "imex_gantry_count");
                key.tools_per_gantry = imex_cfg_int(printer_cfg, "imex_tools_per_gantry");
                key.tool_layout      = (int)imex_cfg_enum<ImexToolLayout>(printer_cfg, "imex_tool_layout");
                key.clearance_x      = imex_cfg_float(printer_cfg, "imex_nozzle_clearance_x");
                key.clearance_y      = imex_cfg_float(printer_cfg, "imex_nozzle_clearance_y");
                key.firmware_managed = imex_cfg_bool(printer_cfg, "imex_firmware_managed_zones");
                key.bed_min_x        = bed_extents.min.x();
                key.bed_min_y        = bed_extents.min.y();
                key.bed_max_x        = bed_extents.max.x();
                key.bed_max_y        = bed_extents.max.y();

                if (!(m_imex_marker_key.s == key) || m_imex_marker_key.mode != mode ||
                    m_imex_marker_key.mode_names != mode_names ||
                    m_imex_marker_key.mode_active_tools != active_tools) {
                    m_imex_marker_key = ImexMarkerKey{ key, mode, mode_names, active_tools };
                    m_imex_marker_plan = resolve_imex_marker_plan(printer_cfg, mode, bed_extents);
                    m_sequential_view.m_imex_secondary_markers.clear();
                }

                const ImexMarkerPlan& plan      = m_imex_marker_plan;
                const int             sec_count = (int)plan.carriages.size();

                const bool color_by_filament = m_viewer.get_view_type() == libvgcode::EViewType::ColorPrint;
                std::vector<ColorRGBA> carriage_colors(sec_count + 1);
                if (sec_count > 0) {
                    const ConfigOptionInts pem       = effective_physical_extruder_map(*preset_bundle);
                    const auto             plate_map = curr_plate ? curr_plate->get_imex_head_filament_map()
                                                                  : std::map<int, int>{};
                    std::map<int, ColorRGBA> head_colors;
                    auto color_for = [&](int phys_head, size_t palette_index) {
                        const ColorRGBA fallback = s_carriage_colors[palette_index % s_carriage_colors.size()];
                        if (!color_by_filament || curr_plate == nullptr || phys_head < 0)
                            return fallback;
                        auto hc = head_colors.find(phys_head);
                        if (hc == head_colors.end())
                            hc = head_colors.emplace(phys_head,
                                     curr_plate->get_imex_head_filament_color(phys_head, pem, plate_map)).first;
                        if (hc->second == GLVolume::UNPRINTABLE_COLOR)
                            return fallback;
                        ColorRGBA c = hc->second;
                        c.a(fallback.a());
                        return c;
                    };
                    carriage_colors[0] = color_for(plan.pri_head, 0);
                    for (int i = 0; i < sec_count; ++i)
                        carriage_colors[i + 1] = color_for(plan.carriages[i].phys_head, i + 1);
                }
                if (sec_count > 0) {
                    imex_active = true;
                    imex_box_wx = plan.box_wx;
                    imex_box_wy = plan.box_wy;

                    if ((int)m_sequential_view.m_imex_secondary_markers.size() != sec_count) {
                        m_sequential_view.m_imex_secondary_markers.resize(sec_count);
                        for (int i = 0; i < sec_count; ++i) {
                            m_sequential_view.m_imex_secondary_markers[i].init(m_marker_filename);
                        }
                    }
                    m_sequential_view.marker.set_color(carriage_colors[0]);
                    for (int i = 0; i < sec_count; ++i)
                        m_sequential_view.m_imex_secondary_markers[i].set_color(carriage_colors[i + 1]);

                    const Vec3f prim_pos = libvgcode::convert(curr_vertex.position);
                    carriage_box_draws.push_back({ prim_pos, carriage_colors[0],
                                                   plan.pri_box_offset_x, plan.pri_box_offset_y });

                    for (int i = 0; i < sec_count; ++i) {
                        const ImexMarkerPlan::Carriage& carriage = plan.carriages[i];
                        const Vec3f sec_pos{ carriage.mirror_x ? carriage.x_term - prim_pos.x()
                                                               : prim_pos.x() + carriage.x_term,
                                             carriage.mirror_y ? carriage.y_term - prim_pos.y()
                                                               : prim_pos.y() + carriage.y_term,
                                             prim_pos.z() };
                        m_sequential_view.m_imex_secondary_markers[i].set_world_position(sec_pos);
                        m_sequential_view.m_imex_secondary_markers[i].set_z_offset(m_z_offset + 0.5f);
                        carriage_box_draws.push_back({
                            sec_pos, carriage_colors[i + 1],
                            carriage.box_offset_x, carriage.box_offset_y });
                    }
                }
            }
        }
        if (!imex_active) {
            m_sequential_view.m_imex_secondary_markers.clear();
            m_sequential_view.marker.set_color(SequentialView::Marker::default_color());
        }
    }
    m_sequential_view.render_marker(!m_no_render_path, canvas_width, sequential_view_height(canvas_height), m_viewer.get_view_type());

    const bool show_toolhead_boxes = wxGetApp().app_config->get("show_imex_toolhead_boxes") != "false";
    if (show_toolhead_boxes && !carriage_box_draws.empty() && imex_box_wx > 0.0f && imex_box_wy > 0.0f) {
        if (!m_imex_toolhead_box.is_initialized() ||
            m_imex_box_mesh_wx != imex_box_wx || m_imex_box_mesh_wy != imex_box_wy) {
            const float box_h = std::max(imex_box_wx, imex_box_wy);
            indexed_triangle_set its = its_make_cube((double)imex_box_wx, (double)imex_box_wy, (double)box_h);
            m_imex_toolhead_box.reset();
            m_imex_toolhead_box.init_from(its);
            m_imex_box_mesh_wx = imex_box_wx;
            m_imex_box_mesh_wy = imex_box_wy;
        }

        GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
        if (shader) {
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            shader->start_using();
            shader->set_uniform("emission_factor", 0.0f);
            const Camera& camera = wxGetApp().plater()->get_camera();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            const Transform3d& view_matrix = camera.get_view_matrix();
            for (const auto& draw : carriage_box_draws) {
                const Transform3d model_matrix = Geometry::translation_transform(
                    (draw.pos + Vec3f(draw.box_offset_x, draw.box_offset_y, m_z_offset)).cast<double>());
                shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
                const Matrix3d view_normal_matrix =
                    view_matrix.matrix().block(0, 0, 3, 3) *
                    model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
                shader->set_uniform("view_normal_matrix", view_normal_matrix);
                m_imex_toolhead_box.set_color(draw.color);
                m_imex_toolhead_box.render();
            }
            shader->stop_using();
            glsafe(::glDisable(GL_BLEND));
        }
    }
}

size_t GCodeViewer::shadow_casters_signature() const
{
    size_t hash = m_viewer.get_vertices_count();
    const libvgcode::Interval& visible = m_viewer.get_view_visible_range();
    const libvgcode::Interval& layers  = m_viewer.get_layers_view_range();
    for (size_t value : { size_t(visible[0]), size_t(visible[1]), size_t(layers[0]), size_t(layers[1]) })
        boost::hash_combine(hash, value);
    for (double value : { m_paths_bounding_box.min.x(), m_paths_bounding_box.min.y(), m_paths_bounding_box.min.z(),
                          m_paths_bounding_box.max.x(), m_paths_bounding_box.max.y(), m_paths_bounding_box.max.z() })
        boost::hash_combine(hash, value);
    boost::hash_combine(hash, m_viewer.is_top_layer_only_view_range());
    for (size_t i = 0; i < libvgcode::GCODE_EXTRUSION_ROLES_COUNT; ++i)
        boost::hash_combine(hash, m_viewer.is_extrusion_role_visible(libvgcode::EGCodeExtrusionRole(i)));
    boost::hash_combine(hash, m_viewer.is_option_visible(libvgcode::EOptionType::Travels));
    boost::hash_combine(hash, m_viewer.is_option_visible(libvgcode::EOptionType::Wipes));
    for (float value : m_clipping_plane)
        boost::hash_combine(hash, value);
    return hash;
}

void GCodeViewer::render_shadow_casters(const Transform3d& light_view_matrix, const Transform3d& light_projection_matrix, const Vec3d& light_position)
{
    if (!has_data())
        return;

    m_viewer.render_shadow_casters(
        libvgcode::convert(static_cast<Matrix4f>(light_view_matrix.matrix().cast<float>())),
        libvgcode::convert(static_cast<Matrix4f>(light_projection_matrix.matrix().cast<float>())),
        libvgcode::convert(static_cast<Vec3f>(light_position.cast<float>())));
}

void GCodeViewer::set_shadow_map(int texture_unit, const Transform3d& light_view_projection, float intensity, float texel_size)
{
    m_viewer.set_shadow_map(texture_unit,
        libvgcode::convert(static_cast<Matrix4f>(light_view_projection.matrix().cast<float>())),
        intensity, texel_size);
}

void GCodeViewer::set_light_top_dir(const Vec3d& direction)
{
    m_viewer.set_light_top_dir(libvgcode::convert(static_cast<Vec3f>(direction.cast<float>())));
}

void GCodeViewer::set_tone(float exposure, float saturation)
{
    m_viewer.set_tone(exposure, saturation);
}

void GCodeViewer::set_clipping_plane(const ClippingPlane& plane)
{
    const Vec3f normal = -plane.get_normal().cast<float>();
    m_clipping_plane = plane.is_active() ?
        std::array<float, 4>{ normal.x(), normal.y(), normal.z(), float(plane.get_offset()) } :
        std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
    m_viewer.set_clipping_plane(m_clipping_plane);
}

void GCodeViewer::render_overlay(int canvas_width, int canvas_height, int right_margin)
{
    if (m_viewer.get_extrusion_roles().empty())
        return;

    render_clearance_markers(canvas_width, canvas_height);
    render_nonplanar_explain(canvas_width, canvas_height);

    float legend_height = 0.0f;
    render_legend(legend_height, canvas_width, canvas_height, right_margin);

    if (m_user_mode != wxGetApp().get_mode()) {
        update_by_mode(wxGetApp().get_mode());
        m_user_mode = wxGetApp().get_mode();
    }

    m_sequential_view.render_overlay(!m_no_render_path, legend_height, &m_viewer, m_viewer.get_current_vertex().gcode_id, canvas_width, sequential_view_height(canvas_height), right_margin * m_scale, m_viewer.get_view_type());

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    if (is_legend_shown()) {
        ImGuiWrapper& imgui = *Slic3r::GUI::wxGetApp().imgui();
        const Size cnv_size = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();
        ImGuiPureWrap::set_next_window_pos(static_cast<float>(cnv_size.get_width()), static_cast<float>(cnv_size.get_height()), ImGuiCond_Always, 1.0f, 1.0f);
        ImGuiPureWrap::begin(std::string("LibVGCode Viewer Controller"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);

        ImGuiPureWrap::checkbox("Cog marker fixed screen size", m_cog_marker_fixed_screen_size);
        if (ImGui::BeginTable("Cog", 2)) {

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Cog marker size");
            ImGui::TableSetColumnIndex(1);
            imgui.slider_float("##CogSize", &m_cog_marker_size, 1.0f, 5.0f);

            ImGui::EndTable();
        }

        ImGuiPureWrap::checkbox("Tool marker fixed screen size", m_tool_marker_fixed_screen_size);
        if (ImGui::BeginTable("Tool", 2)) {

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Tool marker size");
            ImGui::TableSetColumnIndex(1);
            imgui.slider_float("##ToolSize", &m_tool_marker_size, 1.0f, 5.0f);

            ImGui::EndTable();
        }

        ImGuiPureWrap::end();
    }
#endif
    render_slider(canvas_width, canvas_height);
}

int GCodeViewer::sequential_view_height(int canvas_height) const
{
    const int bottom_margin = SLIDER_BOTTOM_MARGIN * GCODE_VIEWER_SLIDER_SCALE;
    return canvas_height - bottom_margin * m_scale;
}

#define ENABLE_CALIBRATION_THUMBNAIL_OUTPUT 0
#if ENABLE_CALIBRATION_THUMBNAIL_OUTPUT
static void debug_calibration_output_thumbnail(const ThumbnailData& thumbnail_data)
{
    wxImage image(thumbnail_data.width, thumbnail_data.height);
    image.InitAlpha();

    for (unsigned int r = 0; r < thumbnail_data.height; ++r)
    {
        unsigned int rr = (thumbnail_data.height - 1 - r) * thumbnail_data.width;
        for (unsigned int c = 0; c < thumbnail_data.width; ++c)
        {
            unsigned char* px = (unsigned char*)thumbnail_data.pixels.data() + 4 * (rr + c);
            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
            image.SetAlpha((int)c, (int)r, px[3]);
        }
    }

    image.SaveFile("D:/calibrate.png", wxBITMAP_TYPE_PNG);
}
#endif

bool GCodeViewer::can_export_toolpaths() const
{
    const libvgcode::Interval& visible_range = m_viewer.get_view_visible_range();
    for (size_t i = visible_range[0]; i <= visible_range[1]; ++i) {
        if (m_viewer.get_vertex_at(i).is_extrusion())
            return true;
    }
    return false;
}

bool GCodeViewer::jump_to_clearance_move(size_t move_id, unsigned int result_id)
{
    if (result_id != m_last_result_id || move_id >= m_clearance_move_vertices.size())
        return false;
    const uint32_t id = m_clearance_move_vertices[move_id];
    if (id >= m_viewer.get_vertices_count())
        return false;
    const auto &vertex = m_viewer.get_vertex_at(id);
    select_view_type(libvgcode::EViewType::Clearance);
    if (vertex.is_extrusion() && !m_viewer.is_extrusion_role_visible(vertex.role))
        m_viewer.toggle_extrusion_role_visibility(vertex.role);
    const auto option = libvgcode::move_type_to_option(vertex.type);
    if (option != libvgcode::EOptionType::COUNT && !m_viewer.is_option_visible(option))
        m_viewer.toggle_option_visibility(option);
    m_layers_slider->SetSelectionSpan(0, static_cast<int>(vertex.layer_id));
    set_layers_z_range({ 0, vertex.layer_id });
    update_sequential_view_current(m_viewer.get_view_enabled_range()[0], id);
    update_moves_slider();
    m_layers_slider->set_as_dirty(false);
    m_moves_slider->set_as_dirty(false);
    auto *canvas = wxGetApp().plater()->get_preview_canvas3D();
    canvas->set_volumes_z_range({ m_layers_slider->GetLowerValueD(), m_layers_slider->GetHigherValueD() });
    canvas->set_as_dirty();
    return true;
}

void GCodeViewer::render_clearance_markers(int canvas_width, int canvas_height)
{
    if (m_viewer.get_view_type() != libvgcode::EViewType::Clearance || !m_clearance_report.ran)
        return;
    const auto &camera = wxGetApp().plater()->get_camera();
    const Matrix4d projection = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const auto &range = m_viewer.get_view_visible_range();
    const auto &layers = m_viewer.get_layers_view_range();
    for (const auto &hit : m_clearance_report.worst) {
        if (hit.move_id >= m_clearance_move_vertices.size())
            continue;
        const uint32_t id = m_clearance_move_vertices[hit.move_id];
        if (id < range[0] || id > range[1] || id >= m_viewer.get_vertices_count())
            continue;
        const auto &vertex = m_viewer.get_vertex_at(id);
        if (vertex.layer_id < layers[0] || vertex.layer_id > layers[1])
            continue;
        if (vertex.is_extrusion() && !m_viewer.is_extrusion_role_visible(vertex.role))
            continue;
        const auto option = libvgcode::move_type_to_option(vertex.type);
        if (option != libvgcode::EOptionType::COUNT && !m_viewer.is_option_visible(option))
            continue;
        const Vec4d clip = projection * Vec4d(vertex.position[0], vertex.position[1], vertex.position[2], 1.);
        if (clip.w() <= 0. || std::abs(clip.z()) > clip.w())
            continue;
        const ImVec2 p(float((clip.x() / clip.w() + 1.) * 0.5 * canvas_width),
                       float((1. - clip.y() / clip.w()) * 0.5 * canvas_height));
        if (p.x < 0 || p.x > canvas_width || p.y < 0 || p.y > canvas_height)
            continue;
        ImGui::GetBackgroundDrawList()->AddCircle(p, 6.f * m_scale, IM_COL32(220, 55, 45, 255), 16, 2.f * m_scale);
    }
}

static ColorRGBA nonplanar_fallback_color(const std::string &cls)
{
    return cls == "fallback" ? ColorRGBA(0.96f, 0.51f, 0.12f, 1.f) : cls == "flat" ? ColorRGBA(0.85f, 0.85f, 0.85f, 1.f) :
                                                                     ColorRGBA(0.35f, 0.62f, 0.96f, 1.f);
}

static wxString nonplanar_kind_label(int kind)
{
    const wxString labels[] = { _L("top"), _L("bottom"), _L("thin-part"), _L("overhang"), _L("chunk"), _L("support interface"),
                                _L("bridge"), _L("rotary layer") };
    return kind >= 0 && kind < int(std::size(labels)) ? labels[kind] : from_u8(Nonplanar::patch_kind_name(kind));
}

static wxString nonplanar_pattern_label(const std::string &pattern)
{
    return pattern == "contour" ? _L("Contour") : pattern == "concentric" ? _L("Concentric") : _L("Rectilinear");
}

void GCodeViewer::load_nonplanar_report(const GCodeProcessorResult &gcode_result, const libvgcode::GCodeInputData &data)
{
    m_nonplanar_report = gcode_result.nonplanar_report;
    m_nonplanar_hatch.clear();
    m_nonplanar_hatch_dirty = true;
    m_nonplanar_hover_grid = {};
    m_nonplanar_fallback_z.clear();
    m_nonplanar_selected = -1;
    if (m_nonplanar_report.empty())
        return;
    for (size_t i = 0; i < data.vertices.size(); ++i)
        if (data.vertices[i].is_extrusion() && data.vertices[i].is_nonplanar())
        {
            const size_t layer = data.vertices[i].layer_id;
            if (m_nonplanar_hover_grid.by_layer.size() <= layer)
                m_nonplanar_hover_grid.by_layer.resize(layer + 1);
            m_nonplanar_hover_grid.by_layer[layer].vertices.emplace_back(uint32_t(i));
        }
    for (const Nonplanar::ReportFallback &fallback : m_nonplanar_report.fallbacks) {
        double z = 0.;
        size_t n = 0;
        for (const auto &line : fallback.hatch)
            for (const Vec3f &p : line)
                z += p.z(), ++n;
        m_nonplanar_fallback_z.emplace_back(n > 0 ? float(z / n) : 0.f);
    }
    Plater *plater = wxGetApp().plater();
    plater->sidebar().set_nonplanar_summary(&m_nonplanar_report);
    for (const Nonplanar::ReportObject &object : m_nonplanar_report.objects)
        for (const CodedWarning &warning : object.warnings)
            plater->get_notification_manager()->push_notification(NotificationType::NonplanarWarning,
                NotificationManager::NotificationLevel::WarningNotificationLevel, warning.message);
}

void GCodeViewer::render_nonplanar_hatch()
{
    if (m_nonplanar_report.empty() || !m_nonplanar_show_fallbacks || m_viewer.get_view_type() != libvgcode::EViewType::Nonplanar)
        return;
    if (m_nonplanar_hatch_dirty) {
        m_nonplanar_hatch_dirty = false;
        m_nonplanar_hatch.clear();
        for (const Nonplanar::ReportFallback &fallback : m_nonplanar_report.fallbacks) {
            GLModel::Geometry data;
            data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
            for (const auto &line : fallback.hatch)
                for (size_t k = 0; k + 1 < line.size(); ++k) {
                    data.add_vertex(Vec3f(line[k] + Vec3f(0.f, 0.f, 0.08f)));
                    data.add_vertex(Vec3f(line[k + 1] + Vec3f(0.f, 0.f, 0.08f)));
                    const unsigned int n = static_cast<unsigned int>(data.vertices_count());
                    data.add_line(n - 2, n - 1);
                }
            auto model = std::make_unique<GLModel>();
            if (data.vertices_count() > 0) {
                model->init_from(std::move(data));
                model->set_color(nonplanar_fallback_color(fallback.cls));
            }
            m_nonplanar_hatch.emplace_back(std::move(model));
        }
    }
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    const auto &layers     = m_viewer.get_layers_view_range();
    const bool  all_layers = layers[1] + 1 >= m_viewer.get_layers_count();
    const float top_z      = all_layers ? FLT_MAX : m_viewer.get_layer_z(layers[1]) + 1e-3f;
    const auto &camera     = wxGetApp().plater()->get_camera();
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glEnable(GL_DEPTH_TEST));
    for (size_t i = 0; i < m_nonplanar_hatch.size() && i < m_nonplanar_fallback_z.size(); ++i)
        if (m_nonplanar_hatch[i]->is_initialized() && m_nonplanar_fallback_z[i] <= top_z)
            m_nonplanar_hatch[i]->render();
    shader->stop_using();
}

void GCodeViewer::render_nonplanar_explain(int canvas_width, int canvas_height)
{
    if (m_nonplanar_report.empty() || m_viewer.get_view_type() != libvgcode::EViewType::Nonplanar)
        return;
    const auto    &camera     = wxGetApp().plater()->get_camera();
    const Matrix4d projection = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    auto project = [&](const Vec3d &p, ImVec2 &screen, double &depth) {
        const Vec4d clip = projection * Vec4d(p.x(), p.y(), p.z(), 1.);
        if (clip.w() <= 0. || std::abs(clip.z()) > clip.w())
            return false;
        screen = ImVec2(float((clip.x() / clip.w() + 1.) * 0.5 * canvas_width), float((1. - clip.y() / clip.w()) * 0.5 * canvas_height));
        depth  = clip.z() / clip.w();
        return true;
    };
    const auto &layers     = m_viewer.get_layers_view_range();
    const bool  all_layers = layers[1] + 1 >= m_viewer.get_layers_count();
    const float top_z      = all_layers ? FLT_MAX : m_viewer.get_layer_z(layers[1]) + 1e-3f;

    const ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse || io.MousePos.x < 0.f || io.MousePos.y < 0.f || io.MousePos.x > canvas_width || io.MousePos.y > canvas_height)
        return;
    if (io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2])
        return;
    const auto  &range  = m_viewer.get_view_visible_range();
    const float  radius = 6.f * m_scale;
    NonplanarHoverGrid &grid = m_nonplanar_hover_grid;
    std::vector<bool>   roles(static_cast<size_t>(libvgcode::EGCodeExtrusionRole::COUNT));
    for (size_t r = 0; r < roles.size(); ++r)
        roles[r] = m_viewer.is_extrusion_role_visible(libvgcode::EGCodeExtrusionRole(r));
    const std::array<uint32_t, 2> shown_range  = { uint32_t(range[0]), uint32_t(range[1]) };
    const std::array<uint32_t, 2> shown_layers = { uint32_t(layers[0]), uint32_t(layers[1]) };
    const bool camera_changed = grid.projection != projection || grid.size != std::array<int, 2>{ canvas_width, canvas_height };
    if (camera_changed)
        for (auto &layer : grid.by_layer)
            layer.ready = false;
    if (camera_changed || grid.range != shown_range || grid.layers != shown_layers || grid.roles != roles || grid.cell != 2.f * radius) {
        grid.projection = projection;
        grid.size       = { canvas_width, canvas_height };
        grid.range      = shown_range;
        grid.layers     = shown_layers;
        grid.roles      = roles;
        grid.cell       = 2.f * radius;
        grid.columns    = std::max(1, int(std::ceil(canvas_width / grid.cell)));
        grid.rows       = std::max(1, int(std::ceil(canvas_height / grid.cell)));
        std::vector<NonplanarHoverGrid::Entry> shown;
        for (size_t l = layers[0]; l <= layers[1] && l < grid.by_layer.size(); ++l) {
            auto &layer = grid.by_layer[l];
            if (!layer.ready) {
                layer.projected.clear();
                for (const uint32_t id : layer.vertices) {
                    const auto &vertex = m_viewer.get_vertex_at(id);
                    ImVec2 screen;
                    double depth;
                    if (project(Vec3d(vertex.position[0], vertex.position[1], vertex.position[2]), screen, depth))
                        layer.projected.push_back({ id, screen.x, screen.y, depth });
                }
                layer.ready = true;
            }
            for (const auto &entry : layer.projected)
                if (entry.id >= range[0] && entry.id <= range[1] &&
                    m_viewer.is_extrusion_role_visible(m_viewer.get_vertex_at(entry.id).role) &&
                    entry.x >= -radius && entry.y >= -radius && entry.x <= canvas_width + radius && entry.y <= canvas_height + radius)
                    shown.push_back(entry);
        }
        auto cell_of = [&grid](const NonplanarHoverGrid::Entry &e) {
            return std::clamp(int(std::floor(e.y / grid.cell)), 0, grid.rows - 1) * grid.columns +
                   std::clamp(int(std::floor(e.x / grid.cell)), 0, grid.columns - 1);
        };
        grid.cell_start.assign(size_t(grid.columns * grid.rows) + 1, 0);
        for (const NonplanarHoverGrid::Entry &e : shown)
            ++grid.cell_start[size_t(cell_of(e)) + 1];
        for (size_t c = 1; c < grid.cell_start.size(); ++c)
            grid.cell_start[c] += grid.cell_start[c - 1];
        grid.entries.resize(shown.size());
        std::vector<uint32_t> fill(grid.cell_start.begin(), grid.cell_start.end() - 1);
        for (const NonplanarHoverGrid::Entry &e : shown)
            grid.entries[fill[size_t(cell_of(e))]++] = e;
    }
    const libvgcode::PathVertex *hovered = nullptr;
    double best_depth = DBL_MAX;
    const int column_min = std::max(0, int((io.MousePos.x - radius) / grid.cell)), column_max = std::min(grid.columns - 1, int((io.MousePos.x + radius) / grid.cell));
    const int row_min    = std::max(0, int((io.MousePos.y - radius) / grid.cell)), row_max    = std::min(grid.rows - 1, int((io.MousePos.y + radius) / grid.cell));
    for (int row = row_min; row <= row_max; ++row)
        for (int column = column_min; column <= column_max; ++column) {
            const size_t cell = size_t(row * grid.columns + column);
            for (uint32_t k = grid.cell_start[cell]; k < grid.cell_start[cell + 1]; ++k) {
                const NonplanarHoverGrid::Entry &e = grid.entries[k];
                const float dx = e.x - io.MousePos.x, dy = e.y - io.MousePos.y;
                if (dx * dx + dy * dy <= radius * radius && e.depth < best_depth)
                    best_depth = e.depth, hovered = &m_viewer.get_vertex_at(e.id);
            }
        }
    std::string title, body;
    if (hovered != nullptr) {
        const Point pos = Point::new_scale(hovered->position[0], hovered->position[1]);
        if (const Nonplanar::ReportPatch *patch = m_nonplanar_report.find_patch(hovered->nonplanar_patch, pos); patch != nullptr && patch->top) {
            title = into_u8(format_wxstr(_L("Curved patch %1% of %2%"), patch->id, m_nonplanar_report.objects[patch->object].name));
            body  = patch->summary;
        } else if (patch != nullptr) {
            title = into_u8(format_wxstr(_L("Curved %1% patch %2% of %3%"), nonplanar_kind_label(patch->kind), patch->id,
                                         m_nonplanar_report.objects[patch->object].name));
            body  = into_u8(format_wxstr(_L("%1% shells over %2% mm²."), patch->shells, wxString::Format("%.0f", patch->area)));
        } else {
            title = into_u8(format_wxstr(_L("Curved pass of patch %1%"), hovered->nonplanar_patch));
            body  = _u8L("Printed curved where the head clears it.");
        }
    } else if (m_nonplanar_show_fallbacks) {
        const Matrix4d inverse = projection.inverse();
        const double   x = 2. * io.MousePos.x / canvas_width - 1., y = 1. - 2. * io.MousePos.y / canvas_height;
        const Vec4d    near_point = inverse * Vec4d(x, y, -1., 1.), far_point = inverse * Vec4d(x, y, 1., 1.);
        const Vec3d    eye = near_point.head<3>() / near_point.w();
        const Vec3d    dir = far_point.head<3>() / far_point.w() - eye;
        for (size_t i = 0; i < m_nonplanar_report.fallbacks.size(); ++i) {
            const Nonplanar::ReportFallback &fallback = m_nonplanar_report.fallbacks[i];
            if (fallback.shape.empty() || std::abs(dir.z()) < 1e-6 || m_nonplanar_fallback_z[i] > top_z)
                continue;
            const double t = (m_nonplanar_fallback_z[i] - eye.z()) / dir.z();
            if (t <= 0. || t >= 1.)
                continue;
            const Vec3d hit = eye + t * dir;
            const Point pos = Point::new_scale(hit.x(), hit.y());
            if (std::any_of(fallback.shape.begin(), fallback.shape.end(), [&pos](const ExPolygon &e) { return e.contains(pos); })) {
                title = into_u8(format_wxstr(fallback.cls == "fallback" ? _L("Planar fallback on %1%") : _L("Planar by design on %1%"),
                                             m_nonplanar_report.objects[fallback.object].name));
                body  = into_u8(format_wxstr("%1% mm²: %2%", wxString::Format("%.1f", fallback.area),
                                            _(Nonplanar::fallback_reason_text(fallback.reason))));
                break;
            }
        }
    }
    if (title.empty())
        return;
    static std::string last_text;
    if (title + body != last_text) {
        last_text = title + body;
        wxGetApp().imgui()->set_requires_extra_frame();
    }
    ImGui::SetNextWindowBgAlpha(1.f);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.f);
    ImGui::TextColored(ImGuiWrapper::COL_ORCA, "%s", title.c_str());
    ImGui::TextUnformatted(body.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void GCodeViewer::render_nonplanar_legend()
{
    if (m_nonplanar_report.empty())
        return;
    ImGuiWrapper &imgui = *wxGetApp().imgui();
    ImGui::Separator();
    std::vector<size_t> top;
    for (size_t i = 0; i < m_nonplanar_report.patches.size(); ++i)
        if (m_nonplanar_report.patches[i].top)
            top.emplace_back(i);
    const auto &patches = m_nonplanar_report.patches;
    if (!top.empty()) {
        std::vector<std::string> labels;
        float widest = ImGui::CalcTextSize(into_u8(format_wxstr(_L("Curved top patches (%1%): pick one for details"), top.size())).c_str()).x;
        for (const size_t i : top) {
            const Nonplanar::ReportPatch &p = patches[i];
            labels.emplace_back(into_u8(format_wxstr(p.ironing ? _L("%1%, patch %2%: %3% shells, %4%, ironed") : _L("%1%, patch %2%: %3% shells, %4%"),
                                                     m_nonplanar_report.objects[p.object].name, p.id, p.shells, nonplanar_pattern_label(p.pattern))));
            widest = std::max(widest, ImGui::CalcTextSize(labels.back().c_str()).x);
        }
        const float width = std::min(28.f * ImGui::GetFontSize(), widest + ImGui::GetStyle().ScrollbarSize + 2.f * ImGui::GetStyle().ItemSpacing.x);
        ImGui::TextUnformatted(into_u8(format_wxstr(_L("Curved top patches (%1%): pick one for details"), top.size())).c_str());
        if (m_nonplanar_selected >= 0 && m_nonplanar_selected < int(patches.size())) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextColored(ImGuiWrapper::COL_ORCA, "%s", patches[size_t(m_nonplanar_selected)].summary.c_str());
            ImGui::PopTextWrapPos();
        }
        const float row = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginChild("nonplanar_patches", ImVec2(width, row * float(std::min<size_t>(top.size(), 6)) + 4.f))) {
            for (size_t k = 0; k < top.size(); ++k) {
                const size_t      i     = top[k];
                const std::string label = labels[k] + "##np" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), m_nonplanar_selected == int(i))) {
                    m_nonplanar_selected = m_nonplanar_selected == int(i) ? -1 : int(i);
                    imgui.set_requires_extra_frame();
                }
            }
        }
        ImGui::EndChild();
    }
    struct Row { size_t object; std::string tag; double area; };
    std::vector<Row> rows;
    for (const Nonplanar::ReportFallback &f : m_nonplanar_report.fallbacks) {
        const auto same = [&f](const Row &r) {
            return r.object == f.object && Nonplanar::fallback_reason_text(r.tag) == Nonplanar::fallback_reason_text(f.reason);
        };
        if (auto it = std::find_if(rows.begin(), rows.end(), same); it != rows.end())
            it->area += f.area;
        else
            rows.push_back({ f.object, f.reason, f.area });
    }
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Row &r) { return r.area < 0.05; }), rows.end());
    if (rows.empty())
        return;
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.area > b.area; });
    if (ImGui::Checkbox(_u8L("Hatch planar areas").c_str(), &m_nonplanar_show_fallbacks))
        wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
    if (ImGui::TreeNodeEx(into_u8(format_wxstr(_L("Planar areas (%1%)"), rows.size())).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 24.f * ImGui::GetFontSize());
        const bool named = m_nonplanar_report.objects.size() > 1;
        for (const Row &row : rows) {
            const wxString area = wxString::Format("%.1f", row.area), reason = _(Nonplanar::fallback_reason_text(row.tag));
            const std::string text = into_u8(named ? format_wxstr("%1%: %2% mm², %3%", from_u8(m_nonplanar_report.objects[row.object].name), area, reason) :
                                                     format_wxstr("%1% mm², %2%", area, reason));
            const ColorRGBA color = Nonplanar::fallback_hatched(row.tag) ? nonplanar_fallback_color(Nonplanar::fallback_class(row.tag)) :
                                                                            ColorRGBA::WHITE();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::to_ImVec4(color));
            ImGui::TextWrapped("%s", text.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        ImGui::TreePop();
    }
}

void GCodeViewer::update_sequential_view_current(unsigned int first, unsigned int last)
{
    m_viewer.set_view_visible_range(static_cast<uint32_t>(first), static_cast<uint32_t>(last));
    const libvgcode::Interval& enabled_range = m_viewer.get_view_enabled_range();
    enable_moves_slider(enabled_range[1] > enabled_range[0]);

#if ENABLE_ACTUAL_SPEED_DEBUG
    if (enabled_range[1] != m_viewer.get_view_visible_range()[1]) {
        const libvgcode::PathVertex& curr_vertex = m_viewer.get_current_vertex();
        if (curr_vertex.is_extrusion() || curr_vertex.is_travel() || curr_vertex.is_wipe() ||
            curr_vertex.type == libvgcode::EMoveType::Seam) {
            const libvgcode::ColorRange& color_range = m_viewer.get_color_range(libvgcode::EViewType::ActualSpeed);
            const std::array<float, 2>& interval = color_range.get_range();
            const size_t vertices_count = m_viewer.get_vertices_count();
            std::vector<SequentialView::ActualSpeedImguiWidget::Item> actual_speed_data;
            const size_t curr_id = m_viewer.get_current_vertex_id();
            size_t start_id = curr_id;
            while (start_id > 0) {
                --start_id;
                if (curr_vertex.gcode_id != m_viewer.get_vertex_at(start_id).gcode_id)
                    break;
            }
            size_t end_id = curr_id;
            while (end_id < vertices_count - 1) {
                ++end_id;
                if (curr_vertex.gcode_id != m_viewer.get_vertex_at(end_id).gcode_id)
                    break;
            }

            if (m_viewer.get_vertex_at(end_id - 1).type == libvgcode::convert(EMoveType::Seam))
                --end_id;

            assert(end_id - start_id >= 2);

            float total_len = 0.0f;
            for (size_t i = start_id; i < end_id; ++i) {
                const libvgcode::PathVertex& v = m_viewer.get_vertex_at(i);
                const float len = (i > start_id) ?
                    (libvgcode::convert(v.position) - libvgcode::convert(m_viewer.get_vertex_at(i - 1).position)).norm() : 0.0f;
                total_len += len;
                if (i == start_id || len > EPSILON)
                    actual_speed_data.push_back({ total_len, v.actual_feedrate, v.times[0] == 0.0f });
            }

            std::vector<std::pair<float, ColorRGBA>> levels;
            const std::vector<float> values = color_range.get_values();
            for (float value : values) {
                levels.push_back(std::make_pair(value, libvgcode::convert(color_range.get_color_at(value))));
                levels.back().second.a(0.5f);
            }

            auto sameSpeed = [](float a, float b) {
                return static_cast<int>(std::roundf(a * 10.0f)) == static_cast<int>(std::roundf(b * 10.0f));
            };
            std::vector<SequentialView::ActualSpeedImguiWidget::Item> compressed;
            if (!actual_speed_data.empty()) {
                compressed.push_back(actual_speed_data[0]);
                for (int i = 1; i < (int)actual_speed_data.size(); ++i) {
                    const bool same_as_prev = sameSpeed(actual_speed_data[i].speed, actual_speed_data[i - 1].speed);
                    const bool same_as_next = (i + 1 < (int)actual_speed_data.size()) && sameSpeed(actual_speed_data[i].speed, actual_speed_data[i + 1].speed);
                    if (!same_as_prev) {
                        if (!sameSpeed(compressed.back().speed, actual_speed_data[i - 1].speed))
                            compressed.push_back(actual_speed_data[i - 1]);
                        compressed.push_back(actual_speed_data[i]);
                    } else if (!same_as_next)
                        compressed.push_back(actual_speed_data[i]);
                }
                if (compressed.back().pos != actual_speed_data.back().pos)
                    compressed.push_back(actual_speed_data.back());
            }

            m_sequential_view.marker.set_actual_speed_data(compressed);
            m_sequential_view.marker.set_actual_speed_y_range(std::make_pair(interval[0], interval[1]));
            m_sequential_view.marker.set_actual_speed_levels(levels);
        }
    }
#endif
}

void GCodeViewer::enable_moves_slider(bool enable) const
{
    bool render_as_disabled = !enable;
    if (m_moves_slider != nullptr && m_moves_slider->is_rendering_as_disabled() != render_as_disabled) {
        m_moves_slider->set_render_as_disabled(render_as_disabled);
        m_moves_slider->set_as_dirty();
    }
}

void GCodeViewer::update_moves_slider(bool set_to_max)
{
    if (m_gcode_result->moves.empty())
        return;

    const libvgcode::Interval& range = m_viewer.get_view_enabled_range();
    const libvgcode::Interval& visible_range = m_viewer.get_view_visible_range();
    uint32_t last_gcode_id = get_gcode_vertex_at(range[0]).gcode_id;
    uint32_t gcode_id_min = get_gcode_vertex_at(visible_range[0]).gcode_id;
    uint32_t gcode_id_max = get_gcode_vertex_at(visible_range[1]).gcode_id;

    const size_t range_size = range[1] - range[0] + 1;
    std::vector<double> values;
    values.reserve(range_size);
    std::vector<double> alternate_values;
    alternate_values.reserve(range_size);

    std::optional<uint32_t> visible_range_min_id;
    std::optional<uint32_t> visible_range_max_id;
    uint32_t counter = 0;

    for (size_t i = range[0]; i <= range[1]; ++i) {
        const uint32_t gcode_id = get_gcode_vertex_at(i).gcode_id;
        bool skip = false;
        if (i > range[0]) {
            if (last_gcode_id == gcode_id) {
                values.back() = i + 1;
                skip = true;
            }
            else
                last_gcode_id = gcode_id;
        }

        if (!skip) {
            values.emplace_back(i + 1);
            alternate_values.emplace_back(gcode_id);
            if (alternate_values.back() == gcode_id_min)
                visible_range_min_id = counter;
            else if (alternate_values.back() == gcode_id_max)
                visible_range_max_id = counter;
            ++counter;
        }
    }

    const int span_min_id = visible_range_min_id.has_value() ? *visible_range_min_id : 0;
    const int span_max_id = visible_range_max_id.has_value() ? *visible_range_max_id : static_cast<int>(values.size()) - 1;

    bool keep_min = m_moves_slider->GetActiveValue() == m_moves_slider->GetMinValue();

    m_moves_slider->SetSliderValues(values);
    m_moves_slider->SetSliderAlternateValues(alternate_values);
    m_moves_slider->SetMaxValue(static_cast<int>(values.size()) - 1);
    m_moves_slider->SetSelectionSpan(span_min_id, span_max_id);
    if (set_to_max)
        m_moves_slider->SetHigherValue(keep_min ? m_moves_slider->GetMinValue() : m_moves_slider->GetMaxValue());
}

void GCodeViewer::update_layers_slider_mode()
{
    bool one_extruder_printed_model = true;

    int only_extruder = -1;

    if (wxGetApp().filaments_cnt() > 1) {
        const ModelObjectPtrs &objects = wxGetApp().plater()->model().objects;

        if (!objects.empty()) {
            const int extruder = objects[0]->config.has("extruder") ? objects[0]->config.option("extruder")->getInt() : 0;

            auto is_one_extruder_printed_model = [objects, extruder]() {
                for (ModelObject *object : objects) {
                    if (object->config.has("extruder") && object->config.option("extruder")->getInt() != extruder) return false;

                    for (ModelVolume *volume : object->volumes)
                        if ((volume->config.has("extruder") && volume->config.option("extruder")->getInt() != extruder) || !volume->mmu_segmentation_facets.empty()) return false;

                    for (const auto &range : object->layer_config_ranges)
                        if (range.second.has("extruder") && range.second.option("extruder")->getInt() != extruder) return false;
                }
                return true;
            };

            if (is_one_extruder_printed_model())
                only_extruder = extruder;
            else
                one_extruder_printed_model = false;
        }
    }

}

void GCodeViewer::set_layers_z_range(const std::array<unsigned int, 2>& layers_z_range)
{
    m_viewer.set_layers_view_range(static_cast<uint32_t>(layers_z_range[0]), static_cast<uint32_t>(layers_z_range[1]));
    update_moves_slider(true);
}

class ToolpathsObjExporter
{
public:
    explicit ToolpathsObjExporter(const libvgcode::Viewer& viewer)
    : m_viewer(viewer) {
    }

    void export_to(const std::string& filename) {
        CNumericLocalesSetter locales_setter;

        FilePtr f_geo = boost::nowide::fopen(filename.c_str(), "w");
        if (f_geo.f == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "ToolpathsObjExporter: Couldn't open " << filename << " for writing";
            return;
        }

        boost::filesystem::path materials_filename(filename);
        materials_filename.replace_extension("mtl");

        fprintf(f_geo.f, "# G-Code Toolpaths\n");
        fprintf(f_geo.f, "# Generated by %s-%s based on Slic3r\n", SLIC3R_APP_NAME, SoftFever_VERSION);
        fprintf(f_geo.f, "\nmtllib ./%s\n", materials_filename.filename().string().c_str());

        FilePtr f_mat = boost::nowide::fopen(materials_filename.string().c_str(), "w");
        if (f_mat.f == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "ToolpathsObjExporter: Couldn't open " << materials_filename.string() << " for writing";
            return;
        }

        fprintf(f_mat.f, "# G-Code Toolpaths Materials\n");
        fprintf(f_mat.f, "# Generated by %s-%s based on Slic3r\n", SLIC3R_APP_NAME, SoftFever_VERSION);

        libvgcode::Interval visible_range = m_viewer.get_view_visible_range();
        if (m_viewer.is_top_layer_only_view_range())
            visible_range[0] = m_viewer.get_view_full_range()[0];
        for (size_t i = visible_range[0]; i <= visible_range[1]; ++i) {
            const libvgcode::PathVertex& curr = m_viewer.get_vertex_at(i);
            const libvgcode::PathVertex& next = m_viewer.get_vertex_at(i + 1);
            if (!curr.is_extrusion() || !next.is_extrusion())
                continue;

            const libvgcode::PathVertex& nextnext = m_viewer.get_vertex_at(i + 2);
            unsigned char flags = 0;
            if (curr.gcode_id == next.gcode_id)
                flags |= Flag_First;
            if (i + 1 == visible_range[1] || !nextnext.is_extrusion())
                flags |= Flag_Last;
            else
                flags |= Flag_Internal;
            export_segment(*f_geo.f, flags, i, curr, next, nextnext);
        }
        export_materials(*f_mat.f);
    }

private:
    const libvgcode::Viewer& m_viewer;
    size_t m_vertices_count{ 0 };
    std::vector<libvgcode::Color> m_colors;
    static const unsigned char Flag_First    = 0x01;
    static const unsigned char Flag_Last     = 0x02;
    static const unsigned char Flag_Internal = 0x04;
    static const float Cap_Rounding_Factor;

    struct SegmentLocalAxes
    {
        Vec3f forward;
        Vec3f right;
        Vec3f up;
    };

    SegmentLocalAxes segment_local_axes(const Vec3f& v1, const Vec3f& v2) {
        SegmentLocalAxes ret;
        ret.forward = (v2 - v1).normalized();
        ret.right   = ret.forward.cross(Vec3f::UnitZ()).normalized();
        ret.up      = ret.right.cross(ret.forward);
        return ret;
    }

    struct Vertex
    {
        Vec3f position;
        Vec3f normal;
    };

    struct CrossSection
    {
        Vertex right;
        Vertex top;
        Vertex left;
        Vertex bottom;
    };

    CrossSection cross_section(const Vec3f& v, const Vec3f& right, const Vec3f& up, float width, float height) {
        CrossSection ret;
        const Vec3f w_shift = 0.5f * width * right;
        const Vec3f h_shift = 0.5f * height * up;
        ret.right.position  = v + w_shift;
        ret.right.normal    = right;
        ret.top.position    = v + h_shift;
        ret.top.normal      = up;
        ret.left.position   = v - w_shift;
        ret.left.normal     = -right;
        ret.bottom.position = v - h_shift;
        ret.bottom.normal   = -up;
        return ret;
    }

    CrossSection normal_cross_section(const Vec3f& v, const SegmentLocalAxes& axes, float width, float height) {
        return cross_section(v, axes.right, axes.up, width, height);
    }

    enum CornerType : unsigned char
    {
        RightTurn,
        LeftTurn,
        Straight
    };

    CrossSection corner_cross_section(const Vec3f& v, const SegmentLocalAxes& axes1, const SegmentLocalAxes& axes2,
        float width, float height, CornerType& corner_type) {
        if (std::abs(std::abs(axes1.forward.dot(axes2.forward)) - 1.0f) < EPSILON)
            corner_type = CornerType::Straight;
        else if (axes1.up.dot(axes1.forward.cross(axes2.forward)) < 0.0f)
            corner_type = CornerType::RightTurn;
        else
            corner_type = CornerType::LeftTurn;
        const Vec3f right = (0.5f * (axes1.right + axes2.right)).normalized();
        return cross_section(v, right, axes1.up, width, height);
    }

    void export_segment(FILE& f, unsigned char flags, size_t v1_id, const libvgcode::PathVertex& v1, const libvgcode::PathVertex& v2, const libvgcode::PathVertex& v3) {
        const Vec3f v1_pos = libvgcode::convert(v1.position);
        const Vec3f v2_pos = libvgcode::convert(v2.position);
        const Vec3f v3_pos = libvgcode::convert(v3.position);
        const SegmentLocalAxes v1_v2 = segment_local_axes(v1_pos, v2_pos);
        const SegmentLocalAxes v2_v3 = segment_local_axes(v2_pos, v3_pos);

        if ((flags & Flag_First) > 0) {
            const Vertex v0 = { v1_pos - Cap_Rounding_Factor * v1.width * v1_v2.forward, -v1_v2.forward };
            const CrossSection ncs = normal_cross_section(v1_pos, v1_v2, v1.width, v1.height);
            export_vertex(f, v0);
            export_vertex(f, ncs.right);
            export_vertex(f, ncs.top);
            export_vertex(f, ncs.left);
            export_vertex(f, ncs.bottom);
            export_material(f, color_id(v1_id));
            export_triangle(f, vertex_id(0), vertex_id(1), vertex_id(2));
            export_triangle(f, vertex_id(0), vertex_id(2), vertex_id(3));
            export_triangle(f, vertex_id(0), vertex_id(3), vertex_id(4));
            export_triangle(f, vertex_id(0), vertex_id(4), vertex_id(1));
            m_vertices_count += 5;
        }
        if ((flags & Flag_Last) > 0) {
            const Vertex v0 = { v2_pos + Cap_Rounding_Factor * v2.width * v1_v2.forward, v1_v2.forward };
            const CrossSection ncs = normal_cross_section(v2_pos, v1_v2, v2.width, v2.height);
            export_vertex(f, v0);
            export_vertex(f, ncs.right);
            export_vertex(f, ncs.top);
            export_vertex(f, ncs.left);
            export_vertex(f, ncs.bottom);
            export_material(f, color_id(v1_id + 1));
            export_triangle(f, vertex_id(-4), vertex_id(1), vertex_id(2));
            export_triangle(f, vertex_id(-4), vertex_id(2), vertex_id(-3));
            export_triangle(f, vertex_id(-3), vertex_id(2), vertex_id(3));
            export_triangle(f, vertex_id(-3), vertex_id(3), vertex_id(-2));
            export_triangle(f, vertex_id(-2), vertex_id(3), vertex_id(4));
            export_triangle(f, vertex_id(-2), vertex_id(4), vertex_id(-1));
            export_triangle(f, vertex_id(-1), vertex_id(4), vertex_id(1));
            export_triangle(f, vertex_id(-1), vertex_id(1), vertex_id(-4));
            export_triangle(f, vertex_id(0), vertex_id(3), vertex_id(2));
            export_triangle(f, vertex_id(0), vertex_id(2), vertex_id(1));
            export_triangle(f, vertex_id(0), vertex_id(1), vertex_id(4));
            export_triangle(f, vertex_id(0), vertex_id(4), vertex_id(3));
            m_vertices_count += 5;
        }
        else {
            CornerType corner_type;
            const CrossSection ccs   = corner_cross_section(v2_pos, v1_v2, v2_v3, v2.width, v2.height, corner_type);
            const CrossSection ncs12 = normal_cross_section(v2_pos, v1_v2, v2.width, v2.height);
            const CrossSection ncs23 = normal_cross_section(v2_pos, v2_v3, v2.width, v2.height);
            if (corner_type == CornerType::Straight) {
                export_vertex(f, ncs12.right);
                export_vertex(f, ncs12.top);
                export_vertex(f, ncs12.left);
                export_vertex(f, ncs12.bottom);
                export_material(f, color_id(v1_id + 1));
                export_triangle(f, vertex_id(-4), vertex_id(0), vertex_id(1));
                export_triangle(f, vertex_id(-4), vertex_id(1), vertex_id(-3));
                export_triangle(f, vertex_id(-3), vertex_id(1), vertex_id(2));
                export_triangle(f, vertex_id(-3), vertex_id(2), vertex_id(-2));
                export_triangle(f, vertex_id(-2), vertex_id(2), vertex_id(3));
                export_triangle(f, vertex_id(-2), vertex_id(3), vertex_id(-1));
                export_triangle(f, vertex_id(-1), vertex_id(3), vertex_id(0));
                export_triangle(f, vertex_id(-1), vertex_id(0), vertex_id(-4));
                m_vertices_count += 4;
            }
            else if (corner_type == CornerType::RightTurn) {
                export_vertex(f, ncs12.left);
                export_vertex(f, ccs.left);
                export_vertex(f, ccs.right);
                export_vertex(f, ncs12.top);
                export_vertex(f, ncs23.left);
                export_vertex(f, ncs12.bottom);
                export_material(f, color_id(v1_id + 1));
                export_triangle(f, vertex_id(-4), vertex_id(2), vertex_id(3));
                export_triangle(f, vertex_id(-4), vertex_id(3), vertex_id(-3));
                export_triangle(f, vertex_id(-3), vertex_id(3), vertex_id(0));
                export_triangle(f, vertex_id(-3), vertex_id(0), vertex_id(-2));
                export_triangle(f, vertex_id(-2), vertex_id(0), vertex_id(5));
                export_triangle(f, vertex_id(-2), vertex_id(5), vertex_id(-1));
                export_triangle(f, vertex_id(-1), vertex_id(5), vertex_id(2));
                export_triangle(f, vertex_id(-1), vertex_id(2), vertex_id(-4));
                export_triangle(f, vertex_id(1), vertex_id(0), vertex_id(3));
                export_triangle(f, vertex_id(1), vertex_id(3), vertex_id(4));
                export_triangle(f, vertex_id(1), vertex_id(4), vertex_id(5));
                export_triangle(f, vertex_id(1), vertex_id(5), vertex_id(0));
                m_vertices_count += 6;
            }
            else {
                export_vertex(f, ncs12.right);
                export_vertex(f, ccs.right);
                export_vertex(f, ncs23.right);
                export_vertex(f, ncs12.top);
                export_vertex(f, ccs.left);
                export_vertex(f, ncs12.bottom);
                export_material(f, color_id(v1_id + 1));
                export_triangle(f, vertex_id(-4), vertex_id(0), vertex_id(3));
                export_triangle(f, vertex_id(-4), vertex_id(3), vertex_id(-3));
                export_triangle(f, vertex_id(-3), vertex_id(3), vertex_id(4));
                export_triangle(f, vertex_id(-3), vertex_id(4), vertex_id(-2));
                export_triangle(f, vertex_id(-2), vertex_id(4), vertex_id(5));
                export_triangle(f, vertex_id(-2), vertex_id(5), vertex_id(-1));
                export_triangle(f, vertex_id(-1), vertex_id(5), vertex_id(0));
                export_triangle(f, vertex_id(-1), vertex_id(0), vertex_id(-4));
                export_triangle(f, vertex_id(1), vertex_id(2), vertex_id(3));
                export_triangle(f, vertex_id(1), vertex_id(3), vertex_id(0));
                export_triangle(f, vertex_id(1), vertex_id(0), vertex_id(5));
                export_triangle(f, vertex_id(1), vertex_id(5), vertex_id(2));
                m_vertices_count += 6;
            }
        }
    }

    size_t vertex_id(int id) { return static_cast<size_t>(1 + static_cast<int>(m_vertices_count) + id); }

    void export_vertex(FILE& f, const Vertex& v) {
        fprintf(&f, "v %g %g %g\n", v.position.x(), v.position.y(), v.position.z());
        fprintf(&f, "vn %g %g %g\n", v.normal.x(), v.normal.y(), v.normal.z());
    }

    void export_material(FILE& f, size_t material_id) {
        fprintf(&f, "\nusemtl material_%zu\n", material_id + 1);
    }

    void export_triangle(FILE& f, size_t v1, size_t v2, size_t v3) {
        fprintf(&f, "f %zu//%zu %zu//%zu %zu//%zu\n", v1, v1, v2, v2, v3, v3);
    }

    void export_materials(FILE& f) {
        static const float inv_255 = 1.0f / 255.0f;
        size_t materials_counter = 0;
        for (const auto& color : m_colors) {
            fprintf(&f, "\nnewmtl material_%zu\n", ++materials_counter);
            fprintf(&f, "Ka 1 1 1\n");
            fprintf(&f, "Kd %g %g %g\n", static_cast<float>(color[0]) * inv_255,
                                         static_cast<float>(color[1]) * inv_255,
                                         static_cast<float>(color[2]) * inv_255);
            fprintf(&f, "Ks 0 0 0\n");
        }
    }

    size_t color_id(size_t vertex_id) {
        const libvgcode::PathVertex& v = m_viewer.get_vertex_at(vertex_id);
        const size_t top_layer_id = m_viewer.is_top_layer_only_view_range() ? m_viewer.get_layers_view_range()[1] : 0;
        const bool color_top_layer_only = m_viewer.get_view_full_range()[1] != m_viewer.get_view_visible_range()[1];
        const libvgcode::Color color = (color_top_layer_only && v.layer_id < top_layer_id &&
              (!m_viewer.is_spiral_vase_mode() || vertex_id != m_viewer.get_view_enabled_range()[0])) ?
              libvgcode::DUMMY_COLOR : m_viewer.get_vertex_color(v);
        auto color_it = std::find_if(m_colors.begin(), m_colors.end(), [&color](const libvgcode::Color& m) { return m == color; });
        if (color_it == m_colors.end()) {
            m_colors.emplace_back(color);
            color_it = std::prev(m_colors.end());
        }
        return std::distance(m_colors.begin(), color_it);
    }
};

const float ToolpathsObjExporter::Cap_Rounding_Factor = 0.25f;

void GCodeViewer::export_toolpaths_to_obj(const char* filename) const
{
    if (filename == nullptr)
        return;

    if (!has_data())
        return;

    wxBusyCursor busy;
    ToolpathsObjExporter exporter(m_viewer);
    exporter.export_to(filename);
}

void GCodeViewer::load_shells(const Print& print, bool initialized, bool force_previewing)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": initialized=%1%, force_previewing=%2%")%initialized %force_previewing;
    if ((print.id().id == m_shells.print_id)&&(print.get_modified_count() == m_shells.print_modify_count)) {
        if (force_previewing)
            m_shells.previewing = force_previewing;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": already loaded, print=%1% print_id=%2%, print_modify_count=%3%, force_previewing %4%")%(&print) %m_shells.print_id %m_shells.print_modify_count %force_previewing;
        return;
    }

    reset_shell();

    if (print.objects().empty()) {
        return;
    }
    int object_count = 0;
    const ModelObjectPtrs& model_objs = wxGetApp().model().objects;
    for (const PrintObject* obj : print.objects()) {
        const ModelObject* model_obj = obj->model_object();

        int object_idx = -1;
        for (int idx = 0; idx < model_objs.size(); idx++) {
            if (model_objs[idx]->id() == model_obj->id()) {
                object_idx = idx;
                break;
            }
        }

        if (object_idx == -1)
            continue;

        std::vector<int> instance_ids(model_obj->instances.size());
        int instance_index = 0;
        for (int i = 0; i < (int)model_obj->instances.size(); ++i) {
            if (model_obj->instances[i]->is_printable())
                instance_ids[instance_index++] = i;
        }
        instance_ids.resize(instance_index);

        size_t current_volumes_count = m_shells.volumes.volumes.size();
        m_shells.volumes.load_object(model_obj, object_idx, instance_ids, "object", initialized, false);

        const SlicingParameters& slicing_parameters = obj->slicing_parameters();
        if (slicing_parameters.object_print_z_min != 0.0) {
            const Vec3d z_offset = slicing_parameters.object_print_z_min * Vec3d::UnitZ();
            for (size_t i = current_volumes_count; i < m_shells.volumes.volumes.size(); ++i) {
                GLVolume* v = m_shells.volumes.volumes[i];
                auto offset  = v->get_instance_transformation().get_matrix_no_offset().inverse() * z_offset;
                v->set_volume_offset(v->get_volume_offset() + offset);
            }
        }

        object_count++;
    }

    while (true) {
        GLVolumePtrs::iterator it = std::find_if(m_shells.volumes.volumes.begin(), m_shells.volumes.volumes.end(), [](GLVolume* volume) { return volume->is_modifier; });
        if (it != m_shells.volumes.volumes.end()) {
            delete (*it);
            m_shells.volumes.volumes.erase(it);
        }
        else
            break;
    }

    for (GLVolume* volume : m_shells.volumes.volumes) {
        volume->zoom_to_volumes = false;
        volume->color.a(0.5f);
        volume->force_native_color = true;
        volume->set_render_color();
        m_shell_bounding_box.merge(volume->transformed_bounding_box());
    }

    m_shells.print_id = print.id().id;
    m_shells.print_modify_count = print.get_modified_count();
    m_shells.previewing = true;
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": shell loaded, id change to %1%, modify_count %2%, object count %3%, glvolume count %4%")
        % m_shells.print_id % m_shells.print_modify_count % object_count %m_shells.volumes.volumes.size();
}

void GCodeViewer::render_toolpaths()
{
    const Camera& camera = wxGetApp().plater()->get_camera();
    Matrix4f view = camera.get_view_matrix().matrix().cast<float>();
    const libvgcode::Mat4x4 converted_view_matrix = libvgcode::convert(view);
    const libvgcode::Mat4x4 converted_projetion_matrix = libvgcode::convert(static_cast<Matrix4f>(camera.get_projection_matrix().matrix().cast<float>()));
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    m_viewer.set_cog_marker_scale_factor(m_cog_marker_fixed_screen_size ? 10.0f * m_cog_marker_size * camera.get_inv_zoom() : m_cog_marker_size);
    m_viewer.set_tool_marker_scale_factor(m_tool_marker_fixed_screen_size ? 10.0f * m_tool_marker_size * camera.get_inv_zoom() : m_tool_marker_size);
#endif
    m_viewer.render(converted_view_matrix, converted_projetion_matrix);

#if ENABLE_NEW_GCODE_VIEWER_DEBUG
    if (is_legend_shown()) {
        ImGuiWrapper& imgui = *wxGetApp().imgui();
        const Size cnv_size = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();
        imgui.set_next_window_pos(static_cast<float>(cnv_size.get_width()), 0.0f, ImGuiCond_Always, 1.0f, 0.0f);
        imgui.begin(std::string("LibVGCode Viewer Debug"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);

        if (ImGui::BeginTable("Data", 2)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "# vertices");
            ImGui::TableSetColumnIndex(1);
            ImGuiWrapper::text(std::to_string(m_viewer.get_vertices_count()));

            ImGui::Separator();

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "cpu memory");
            ImGui::TableSetColumnIndex(1);
            ImGuiWrapper::text(format_memsize(m_viewer.get_used_cpu_memory()));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "gpu memory");
            ImGui::TableSetColumnIndex(1);
            ImGuiWrapper::text(format_memsize(m_viewer.get_used_gpu_memory()));

            ImGui::Separator();

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "layers range");
            ImGui::TableSetColumnIndex(1);
            const libvgcode::Interval& layers_range = m_viewer.get_layers_view_range();
            ImGuiWrapper::text(std::to_string(layers_range[0]) + " - " + std::to_string(layers_range[1]));

            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "view range (enabled)");
            ImGui::TableSetColumnIndex(1);
            const libvgcode::Interval& enabled_view_range = m_viewer.get_view_enabled_range();
            ImGuiWrapper::text(std::to_string(enabled_view_range[0]) + " - " + std::to_string(enabled_view_range[1]) + " | " +
                std::to_string(m_viewer.get_vertex_at(enabled_view_range[0]).gcode_id) + " - " +
                std::to_string(m_viewer.get_vertex_at(enabled_view_range[1]).gcode_id));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "view range (visible)");
            ImGui::TableSetColumnIndex(1);
            const libvgcode::Interval& visible_view_range = m_viewer.get_view_visible_range();
            ImGuiWrapper::text(std::to_string(visible_view_range[0]) + " - " + std::to_string(visible_view_range[1]) + " | " +
                std::to_string(m_viewer.get_vertex_at(visible_view_range[0]).gcode_id) + " - " +
                std::to_string(m_viewer.get_vertex_at(visible_view_range[1]).gcode_id));

            auto add_range_property_row = [&imgui](const std::string& label, const std::array<float, 2>& range) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, label);
                ImGui::TableSetColumnIndex(1);
                char buf[128];
                sprintf(buf, "%.3f - %.3f", range[0], range[1]);
                ImGuiWrapper::text(buf);
            };

            add_range_property_row("height range", m_viewer.get_color_range(libvgcode::EViewType::Height).get_range());
            add_range_property_row("width range", m_viewer.get_color_range(libvgcode::EViewType::Width).get_range());
            add_range_property_row("speed range", m_viewer.get_color_range(libvgcode::EViewType::Speed).get_range());
            add_range_property_row("acceleration range", m_viewer.get_color_range(libvgcode::EViewType::Acceleration).get_range());
            add_range_property_row("jerk range", m_viewer.get_color_range(libvgcode::EViewType::Jerk).get_range());
            add_range_property_row("fan speed range", m_viewer.get_color_range(libvgcode::EViewType::FanSpeed).get_range());
            add_range_property_row("temperature range", m_viewer.get_color_range(libvgcode::EViewType::Temperature).get_range());
            add_range_property_row("pressure advance range", m_viewer.get_color_range(libvgcode::EViewType::PressureAdvance).get_range());
            add_range_property_row("volumetric rate range", m_viewer.get_color_range(libvgcode::EViewType::VolumetricFlowRate).get_range());
            add_range_property_row("layer time linear range", m_viewer.get_color_range(libvgcode::EViewType::LayerTimeLinear).get_range());
            add_range_property_row("layer time logarithmic range", m_viewer.get_color_range(libvgcode::EViewType::LayerTimeLogarithmic).get_range());

            ImGui::EndTable();
        }

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
        ImGui::Separator();

        if (ImGui::BeginTable("Cog", 2)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Cog marker scale factor");
            ImGui::TableSetColumnIndex(1);
            ImGuiPureWrap::text(std::to_string(get_cog_marker_scale_factor()));

            ImGui::EndTable();
        }

        ImGui::Separator();

        if (ImGui::BeginTable("Tool", 2)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Tool marker scale factor");
            ImGui::TableSetColumnIndex(1);
            ImGuiPureWrap::text(std::to_string(m_viewer.get_tool_marker_scale_factor()));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Tool marker z offset");
            ImGui::TableSetColumnIndex(1);
            float tool_z_offset = m_viewer.get_tool_marker_offset_z();
            if (imgui.slider_float("##ToolZOffset", &tool_z_offset, 0.0f, 1.0f))
                m_viewer.set_tool_marker_offset_z(tool_z_offset);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Tool marker color");
            ImGui::TableSetColumnIndex(1);
            const libvgcode::Color& color = m_viewer.get_tool_marker_color();
            std::array<float, 3> c = { static_cast<float>(color[0]) / 255.0f, static_cast<float>(color[1]) / 255.0f, static_cast<float>(color[2]) / 255.0f };
            if (ImGui::ColorPicker3("##ToolColor", c.data())) {
                m_viewer.set_tool_marker_color({ static_cast<uint8_t>(c[0] * 255.0f),
                                                  static_cast<uint8_t>(c[1] * 255.0f),
                                                  static_cast<uint8_t>(c[2] * 255.0f) });
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Tool marker alpha");
            ImGui::TableSetColumnIndex(1);
            float tool_alpha = m_viewer.get_tool_marker_alpha();
            if (imgui.slider_float("##ToolAlpha", &tool_alpha, 0.25f, 0.75f))
                m_viewer.set_tool_marker_alpha(tool_alpha);

            ImGui::EndTable();
        }
#endif

        ImGui::Separator();
        if (ImGui::BeginTable("Radii", 2)) {

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "Travels radius");
            ImGui::TableSetColumnIndex(1);
            float travels_radius = m_viewer.get_travels_radius();
            ImGui::SetNextItemWidth(200.0f);
            if (imgui.slider_float("##TravelRadius", &travels_radius, 0.05f, 0.5f))
                m_viewer.set_travels_radius(travels_radius);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "Wipes radius");
            ImGui::TableSetColumnIndex(1);
            float wipes_radius = m_viewer.get_wipes_radius();
            ImGui::SetNextItemWidth(200.0f);
            if (imgui.slider_float("##WipesRadius", &wipes_radius, 0.05f, 0.5f))
                m_viewer.set_wipes_radius(wipes_radius);

            ImGui::EndTable();
        }
        imgui.end();
    }
#endif
}

bool GCodeViewer::refine_toolpaths()
{
    const Camera& camera = wxGetApp().plater()->get_camera();
    return m_viewer.refine(libvgcode::convert(static_cast<Matrix4f>(camera.get_view_matrix().matrix().cast<float>())),
                           libvgcode::convert(static_cast<Matrix4f>(camera.get_projection_matrix().matrix().cast<float>())));
}

void GCodeViewer::render_shells(int canvas_width, int canvas_height)
{
    if ((!m_shells.previewing && !m_shells.visible) || m_shells.volumes.empty())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    glsafe(::glDepthMask(GL_FALSE));

    shader->start_using();
    shader->set_uniform("emission_factor", 0.1f);
    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("z_far", camera.get_far_z());
    shader->set_uniform("z_near", camera.get_near_z());
    m_shells.volumes.render(GLVolumeCollection::ERenderType::Transparent, false, camera.get_view_matrix(), camera.get_projection_matrix(), {canvas_width, canvas_height});
    shader->set_uniform("emission_factor", 0.0f);
    shader->stop_using();

    glsafe(::glDepthMask(GL_TRUE));
}

void GCodeViewer::render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show ) const {
    if (!show)
        return;
    for (auto gcode_result : gcode_result_list) {
        if (gcode_result->moves.size() == 0)
            return;
    }
    ImGuiWrapper& imgui = *wxGetApp().imgui();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0, 10.0 * m_scale));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.42f, 0.42f, 0.42f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(340.f * m_scale * imgui.scaled(1.0f / 15.0f), 0));

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), 0, ImVec2(0.5f, 0.5f));
    ImGui::Begin(_L("Statistics of All Plates").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    std::vector<float> filament_diameters = gcode_result_list.front()->filament_diameters;
    std::vector<float> filament_densities = gcode_result_list.front()->filament_densities;
    std::vector<ColorRGBA> filament_colors;
    decode_colors(wxGetApp().plater()->get_extruder_colors_from_plater_config(gcode_result_list.back()), filament_colors);

    for (int i = 0; i < filament_colors.size(); i++) {
        filament_colors[i] = adjust_color_for_rendering(filament_colors[i]);
    }

    bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
    float window_padding = 4.0f * m_scale;
    const float icon_size = ImGui::GetTextLineHeight() * 0.7;
    std::map<std::string, float> offsets;
    std::map<int, double> model_volume_of_extruders_all_plates;
    std::map<int, double> flushed_volume_of_extruders_all_plates;
    std::map<int, double> wipe_tower_volume_of_extruders_all_plates;
    std::map<int, double> support_volume_of_extruders_all_plates;
    std::vector<double> model_used_filaments_m_all_plates;
    std::vector<double> model_used_filaments_g_all_plates;
    std::vector<double> flushed_filaments_m_all_plates;
    std::vector<double> flushed_filaments_g_all_plates;
    std::vector<double> wipe_tower_used_filaments_m_all_plates;
    std::vector<double> wipe_tower_used_filaments_g_all_plates;
    std::vector<double> support_used_filaments_m_all_plates;
    std::vector<double> support_used_filaments_g_all_plates;
    float total_time_all_plates = 0.0f;
    float total_cost_all_plates = 0.0f;
    bool show_detailed_statistics_page = false;
    struct ColumnData {
        enum {
            Model = 1,
            Flushed = 2,
            WipeTower = 4,
            Support = 1 << 3,
        };
    };
    int displayed_columns = 0;
    auto max_width = [](const std::vector<std::string>& items, const std::string& title, float extra_size = 0.0f) {
        float ret = ImGui::CalcTextSize(title.c_str()).x;
        for (const std::string& item : items) {
            ret = std::max(ret, extra_size + ImGui::CalcTextSize(item.c_str()).x);
        }
        return ret;
    };
    auto calculate_offsets = [max_width, window_padding](const std::vector<std::pair<std::string, std::vector<std::string>>>& title_columns, float extra_size = 0.0f) {
        const ImGuiStyle& style = ImGui::GetStyle();
        std::vector<float> offsets;
        offsets.push_back(max_width(title_columns[0].second, title_columns[0].first, extra_size) + 3.0f * style.ItemSpacing.x + style.WindowPadding.x);
        for (size_t i = 1; i < title_columns.size() - 1; i++)
            offsets.push_back(offsets.back() + max_width(title_columns[i].second, title_columns[i].first) + style.ItemSpacing.x);
        if (title_columns.back().first == _u8L("Display"))
            offsets.back() = ImGui::GetWindowWidth() - ImGui::CalcTextSize(_u8L("Display").c_str()).x - ImGui::GetFrameHeight() / 2 - 2 * window_padding;

        float average_col_width = ImGui::GetWindowWidth() / static_cast<float>(title_columns.size());
        std::vector<float> ret;
        ret.push_back(0);
        for (size_t i = 1; i < title_columns.size(); i++) {
            ret.push_back(std::max(offsets[i - 1], i * average_col_width));
        }

        return ret;
    };
    auto append_item = [icon_size, &imgui, &window_padding, &draw_list, this](const ColorRGBA& color, const std::vector<std::pair<std::string, float>>& columns_offsets)
    {
        ImVec2 pos = ImVec2(ImGui::GetCursorScreenPos().x + window_padding * 3, ImGui::GetCursorScreenPos().y);

        draw_list->AddRectFilled({ pos.x + 1.0f * m_scale, pos.y + 3.0f * m_scale }, { pos.x + icon_size - 1.0f * m_scale, pos.y + icon_size + 1.0f * m_scale },
            ImGuiWrapper::to_ImU32(color));

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(20.0 * m_scale, 6.0 * m_scale));

        ImGui::Dummy({ 0.0, 0.0 });
        ImGui::SameLine();

        {
            float dummy_size = ImGui::GetStyle().ItemSpacing.x + icon_size;
            ImGui::SameLine(dummy_size);
            imgui.text(columns_offsets[0].first);

            for (auto i = 1; i < columns_offsets.size(); i++) {
                ImGui::SameLine(columns_offsets[i].second);
                imgui.text(columns_offsets[i].first);
            }
        }

        ImGui::PopStyleVar(1);
    };
    auto append_headers = [&imgui](const std::vector<std::pair<std::string, float>>& title_offsets) {
        for (size_t i = 0; i < title_offsets.size(); i++) {
            ImGui::SameLine(title_offsets[i].second);
            imgui.bold_text(title_offsets[i].first);
        }
        ImGui::Separator();
    };
    auto get_used_filament_from_volume = [imperial_units, &filament_diameters, &filament_densities](double volume, int extruder_id) {
        double koef = imperial_units ? 1.0 / GizmoObjectManipulation::in_to_mm : 0.001;
        std::pair<double, double> ret = { koef * volume / (PI * sqr(0.5 * filament_diameters[extruder_id])),
                                            volume * filament_densities[extruder_id] * 0.001 };
        return ret;
    };

    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine();
    {
        PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
        for (auto plate : plate_list.get_nonempty_plate_list())
        {
            auto plate_print_statistics = plate->get_slice_result()->print_statistics;
            auto plate_extruders = plate->get_extruders(true);
            auto max_extruders_colors = wxGetApp().plater()->get_extruders_colors().size();
            for (size_t extruder_id : plate_extruders) {
                extruder_id -= 1;
                if (extruder_id >= max_extruders_colors)
                    continue;
                if (plate_print_statistics.model_volumes_per_extruder.find(extruder_id) == plate_print_statistics.model_volumes_per_extruder.end())
                    model_volume_of_extruders_all_plates[extruder_id] += 0;
                else {
                    double model_volume = plate_print_statistics.model_volumes_per_extruder.at(extruder_id);
                    model_volume_of_extruders_all_plates[extruder_id] += model_volume;
                }
                if (plate_print_statistics.flush_per_filament.find(extruder_id) == plate_print_statistics.flush_per_filament.end())
                    flushed_volume_of_extruders_all_plates[extruder_id] += 0;
                else {
                    double flushed_volume = plate_print_statistics.flush_per_filament.at(extruder_id);
                    flushed_volume_of_extruders_all_plates[extruder_id] += flushed_volume;
                }
                if (plate_print_statistics.wipe_tower_volumes_per_extruder.find(extruder_id) == plate_print_statistics.wipe_tower_volumes_per_extruder.end())
                    wipe_tower_volume_of_extruders_all_plates[extruder_id] += 0;
                else {
                    double wipe_tower_volume = plate_print_statistics.wipe_tower_volumes_per_extruder.at(extruder_id);
                    wipe_tower_volume_of_extruders_all_plates[extruder_id] += wipe_tower_volume;
                }
                if (plate_print_statistics.support_volumes_per_extruder.find(extruder_id) == plate_print_statistics.support_volumes_per_extruder.end())
                    support_volume_of_extruders_all_plates[extruder_id] += 0;
                else {
                    double support_volume = plate_print_statistics.support_volumes_per_extruder.at(extruder_id);
                    support_volume_of_extruders_all_plates[extruder_id] += support_volume;
                }
            }
            const PrintEstimatedStatistics::Mode& plate_time_mode = plate_print_statistics.modes[static_cast<size_t>(m_viewer.get_time_mode())];
            total_time_all_plates += plate_time_mode.time;

            Print     *print;
            plate->get_print((PrintBase **) &print, nullptr, nullptr);
            total_cost_all_plates += print->print_statistics().total_cost;
        }

        for (auto it = model_volume_of_extruders_all_plates.begin(); it != model_volume_of_extruders_all_plates.end(); it++) {
            auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(it->second, it->first);
            if (model_used_filament_m != 0.0 || model_used_filament_g != 0.0)
                displayed_columns |= ColumnData::Model;
            model_used_filaments_m_all_plates.push_back(model_used_filament_m);
            model_used_filaments_g_all_plates.push_back(model_used_filament_g);
        }
        for (auto it = flushed_volume_of_extruders_all_plates.begin(); it != flushed_volume_of_extruders_all_plates.end(); it++) {
            auto [flushed_filament_m, flushed_filament_g] = get_used_filament_from_volume(it->second, it->first);
            if (flushed_filament_m != 0.0 || flushed_filament_g != 0.0)
                displayed_columns |= ColumnData::Flushed;
            flushed_filaments_m_all_plates.push_back(flushed_filament_m);
            flushed_filaments_g_all_plates.push_back(flushed_filament_g);
        }
        for (auto it = wipe_tower_volume_of_extruders_all_plates.begin(); it != wipe_tower_volume_of_extruders_all_plates.end(); it++) {
            auto [wipe_tower_filament_m, wipe_tower_filament_g] = get_used_filament_from_volume(it->second, it->first);
            if (wipe_tower_filament_m != 0.0 || wipe_tower_filament_g != 0.0)
                displayed_columns |= ColumnData::WipeTower;
            wipe_tower_used_filaments_m_all_plates.push_back(wipe_tower_filament_m);
            wipe_tower_used_filaments_g_all_plates.push_back(wipe_tower_filament_g);
        }
        for (auto it = support_volume_of_extruders_all_plates.begin(); it != support_volume_of_extruders_all_plates.end(); it++) {
            auto [support_filament_m, support_filament_g] = get_used_filament_from_volume(it->second, it->first);
            if (support_filament_m != 0.0 || support_filament_g != 0.0)
                displayed_columns |= ColumnData::Support;
            support_used_filaments_m_all_plates.push_back(support_filament_m);
            support_used_filaments_g_all_plates.push_back(support_filament_g);
        }

        char buff[64];
        double longest_str = 0.0;
        for (auto i : model_used_filaments_g_all_plates) {
            if (i > longest_str)
                longest_str = i;
        }
        ::sprintf(buff, "%.2f", longest_str);

        std::vector<std::pair<std::string, std::vector<std::string>>> title_columns;
        if (displayed_columns & ColumnData::Model) {
            title_columns.push_back({ _u8L("Filament"), {""} });
            title_columns.push_back({ _u8L("Model"), {buff} });
        }
        if (displayed_columns & ColumnData::Support) {
            title_columns.push_back({ _u8L("Support"), {buff} });
        }
        if (displayed_columns & ColumnData::Flushed) {
            title_columns.push_back({ _u8L("Flushed"), {buff} });
        }
        if (displayed_columns & ColumnData::WipeTower) {
            title_columns.push_back({ _u8L("Tower"), {buff} });
        }
        if ((displayed_columns & ~ColumnData::Model) > 0) {
            title_columns.push_back({ _u8L("Total"), {buff} });
        }
        auto offsets_ = calculate_offsets(title_columns, icon_size);
        std::vector<std::pair<std::string, float>> title_offsets;
        for (int i = 0; i < offsets_.size(); i++) {
            title_offsets.push_back({ title_columns[i].first, offsets_[i] });
            offsets[title_columns[i].first] = offsets_[i];
        }
        append_headers(title_offsets);
    }

    {
        size_t i = 0;
        for (auto it = model_volume_of_extruders_all_plates.begin(); it != model_volume_of_extruders_all_plates.end(); it++) {
            if (i < model_used_filaments_m_all_plates.size() && i < model_used_filaments_g_all_plates.size()) {
                std::vector<std::pair<std::string, float>> columns_offsets;
                columns_offsets.push_back({ std::to_string(it->first + 1), offsets[_u8L("Filament")]});

                char buf[64];
                float column_sum_m = 0.0f;
                float column_sum_g = 0.0f;
                if (displayed_columns & ColumnData::Model) {
                    const std::string weight_text = format_compact_weight(model_used_filaments_g_all_plates[i], imperial_units);
                    if ((displayed_columns & ~ColumnData::Model) > 0)
                        ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", model_used_filaments_m_all_plates[i], weight_text.c_str());
                    else
                        ::sprintf(buf, imperial_units ? "%.2f in    %s" : "%.2f m    %s", model_used_filaments_m_all_plates[i], weight_text.c_str());
                    columns_offsets.push_back({ buf, offsets[_u8L("Model")] });
                    column_sum_m += model_used_filaments_m_all_plates[i];
                    column_sum_g += model_used_filaments_g_all_plates[i];
                }
                if (displayed_columns & ColumnData::Support) {
                    const std::string weight_text = format_compact_weight(support_used_filaments_g_all_plates[i], imperial_units);
                    ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", support_used_filaments_m_all_plates[i], weight_text.c_str());
                    columns_offsets.push_back({ buf, offsets[_u8L("Support")] });
                    column_sum_m += support_used_filaments_m_all_plates[i];
                    column_sum_g += support_used_filaments_g_all_plates[i];
                }
                if (displayed_columns & ColumnData::Flushed) {
                    const std::string weight_text = format_compact_weight(flushed_filaments_g_all_plates[i], imperial_units);
                    ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", flushed_filaments_m_all_plates[i], weight_text.c_str());
                    columns_offsets.push_back({ buf, offsets[_u8L("Flushed")] });
                    column_sum_m += flushed_filaments_m_all_plates[i];
                    column_sum_g += flushed_filaments_g_all_plates[i];
                }
                if (displayed_columns & ColumnData::WipeTower) {
                    const std::string weight_text = format_compact_weight(wipe_tower_used_filaments_g_all_plates[i], imperial_units);
                    ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", wipe_tower_used_filaments_m_all_plates[i], weight_text.c_str());
                    columns_offsets.push_back({ buf, offsets[_u8L("Tower")] });
                    column_sum_m += wipe_tower_used_filaments_m_all_plates[i];
                    column_sum_g += wipe_tower_used_filaments_g_all_plates[i];
                }
                if ((displayed_columns & ~ColumnData::Model) > 0) {
                    const std::string weight_text = format_compact_weight(column_sum_g, imperial_units);
                    ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", column_sum_m, weight_text.c_str());
                    columns_offsets.push_back({ buf, offsets[_u8L("Total")] });
                }

                append_item(filament_colors[it->first], columns_offsets);
            }
            i++;
        }

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.title(_u8L("Total estimation"));

        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(_u8L("Total time") + ":");
        ImGui::SameLine();
        imgui.text(short_time(get_time_dhms(total_time_all_plates)));

        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(_u8L("Total cost") + ":");
        ImGui::SameLine();
        char buf[64];
        ::sprintf(buf, "%.2f", total_cost_all_plates);
        imgui.text(buf);

    }
    ImGui::End();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(3);
    return;
}

void GCodeViewer::render_legend_color_arr_recommen(float window_padding)
{
    ImGuiWrapper &imgui = *wxGetApp().imgui();

    auto link_text = [&](const std::string &label) {
        ImVec2 wiki_part_size = ImGui::CalcTextSize(label.c_str());

        ImColor HyperColor = ImColor(0, 150, 136, 255).Value;
        ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
        imgui.text(label.c_str());
        ImGui::PopStyleColor();

        ImVec2 lineEnd = ImGui::GetItemRectMax();
        lineEnd.y -= 2.0f;
        ImVec2 lineStart = lineEnd;
        lineStart.x      = ImGui::GetItemRectMin().x;
        ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, HyperColor);
        if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                Plater *plater = wxGetApp().plater();
                wxCommandEvent evt(EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG);
                evt.SetEventObject(plater);
                evt.SetInt(1);
                wxPostEvent(plater, evt);
            }
        }
    };

    auto link_text_set_to_optional = [&](const std::string &label) {
        ImVec2 wiki_part_size = ImGui::CalcTextSize(label.c_str());

        ImColor HyperColor = ImColor(0, 150, 136, 255).Value;
        ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
        imgui.text(label.c_str());
        ImGui::PopStyleColor();
        ImVec2 lineEnd = ImGui::GetItemRectMax();
        lineEnd.y -= 2.0f;
        ImVec2 lineStart = lineEnd;
        lineStart.x      = ImGui::GetItemRectMin().x;
        ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, HyperColor);
        if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                MessageDialog msg_dlg(nullptr, _L("Automatically re-slice according to the optimal filament grouping, and the grouping results will be displayed after slicing."), wxEmptyString, wxOK | wxCANCEL);
                if (msg_dlg.ShowModal() == wxID_OK) {
                    PartPlateList &partplate_list = wxGetApp().plater()->get_partplate_list();
                    PartPlate     *plate          = partplate_list.get_curr_plate();
                    plate->set_filament_map_mode(FilamentMapMode::fmmAutoForFlush);
                    Plater        *plater = wxGetApp().plater();
                    wxPostEvent(plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
                }
            }
        }
    };

    auto draw_dash_line = [&](ImDrawList* draw_list, int dash_length = 5, int gap_length = 3) {
        ImVec2 p1 = ImGui::GetCursorScreenPos();
        ImVec2 p2 = ImVec2(p1.x + ImGui::GetContentRegionAvail().x, p1.y);
        for (float i = p1.x; i < p2.x; i += (dash_length + gap_length)) {
            draw_list->AddLine(ImVec2(i, p1.y), ImVec2(i + dash_length, p1.y), ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.6f)));
        }
    };

    auto config            = wxGetApp().plater()->get_partplate_list().get_current_fff_print().config();
    auto stats_by_extruder = wxGetApp().plater()->get_partplate_list().get_current_fff_print().statistics_by_extruder();

    float delta_weight_to_single_ext = stats_by_extruder.stats_by_single_extruder.filament_flush_weight - stats_by_extruder.stats_by_multi_extruder_curr.filament_flush_weight;
    float delta_weight_to_best = stats_by_extruder.stats_by_multi_extruder_curr.filament_flush_weight - stats_by_extruder.stats_by_multi_extruder_best.filament_flush_weight;
    int   delta_change_to_single_ext = stats_by_extruder.stats_by_single_extruder.flush_filament_change_count - stats_by_extruder.stats_by_multi_extruder_curr.flush_filament_change_count;
    int   delta_change_to_best = stats_by_extruder.stats_by_multi_extruder_curr.flush_filament_change_count - stats_by_extruder.stats_by_multi_extruder_best.flush_filament_change_count;

    bool any_less_to_single_ext = delta_weight_to_single_ext > EPSILON || delta_change_to_single_ext > 0;
    bool any_more_to_best = delta_weight_to_best > EPSILON || delta_change_to_best > 0;
    bool all_less_to_single_ext = delta_weight_to_single_ext > EPSILON && delta_change_to_single_ext > 0;
    bool all_more_to_best = delta_weight_to_best > EPSILON && delta_change_to_best > 0;

    auto get_filament_display_type = [](const ExtruderFilament& filament) {
        if (filament.is_support_filament && (filament.type == "PLA" || filament.type == "PA" || filament.type == "ABS"))
            return "Sup." + filament.type;
        return filament.type;
        };


    float line_height          = ImGui::GetFrameHeight();
    float ams_item_height = 0;
    float filament_group_item_align_width = 0;
    {
        float three_words_width    = imgui.calc_text_size("ABC"sv).x;
        const int line_capacity = 4;

        for (const auto& extruder_filaments : {m_left_extruder_filament,m_right_extruder_filament })
        {
            float container_height = 0.f;
            for (size_t idx = 0; idx < extruder_filaments.size(); idx += line_capacity) {
                float text_line_height = 0;
                for (int j = idx; j < extruder_filaments.size() && j < idx + line_capacity; ++j) {
                    auto text_info = imgui.calculate_filament_group_text_size(get_filament_display_type(extruder_filaments[j]));
                    auto text_size = std::get<0>(text_info);
                    filament_group_item_align_width = std::max(filament_group_item_align_width, text_size.x);
                    text_line_height = std::max(text_line_height, text_size.y);
                }
                container_height += (three_words_width * 1.3f + text_line_height );
            }
            container_height += 2 * line_height;
            ams_item_height = std::max(ams_item_height, container_height);
        }
    }

    int tips_count = 8;
    if (any_more_to_best) {
        tips_count = 8;
        if (wxGetApp().app_config->get("language") != "zh_CN")
            tips_count += 1;
    }
    else if (any_less_to_single_ext) {
        tips_count = 6;
        if (wxGetApp().app_config->get("language") != "zh_CN")
            tips_count += 1;
    }
    else
        tips_count = 5;

    float AMS_container_height = ams_item_height + line_height * tips_count + line_height + window_padding * 2.f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(window_padding * 3, 0));

    ImGui::BeginChild("#AMS", ImVec2(0, AMS_container_height), true, ImGuiWindowFlags_AlwaysUseWindowPadding);
    {
        float available_width   = ImGui::GetContentRegionAvail().x;
        float half_width       = available_width * 0.49f;
        float spacing           = 18.0f * m_scale;

        ImGui::Dummy({window_padding, window_padding});
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f,1.0f,1.0f,0.6f));
        imgui.bold_text(_u8L("Filament Grouping"));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Dummy({window_padding, window_padding});

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.05f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(window_padding * 2, window_padding));

        ImDrawList *child_begin_draw_list = ImGui::GetWindowDrawList();
        ImVec2      cursor_pos            = ImGui::GetCursorScreenPos();
        child_begin_draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + half_width, cursor_pos.y + line_height), IM_COL32(255, 255, 255, 10));
        ImGui::BeginChild("#LeftAMS", ImVec2(half_width, ams_item_height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
        {
            imgui.text(_u8L("Left nozzle"));
            ImGui::Dummy({window_padding, window_padding});
            int index = 1;
            for (const auto &extruder_filament : m_left_extruder_filament) {
                imgui.filament_group(get_filament_display_type(extruder_filament), extruder_filament.hex_color.c_str(), extruder_filament.filament_id, filament_group_item_align_width);
                if (index % 4 != 0) { ImGui::SameLine(0, spacing); }
                index++;
            }
            ImGui::EndChild();
        }
        ImGui::SameLine();
        cursor_pos = ImGui::GetCursorScreenPos();
        child_begin_draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + half_width, cursor_pos.y + line_height), IM_COL32(255, 255, 255, 10));
        ImGui::BeginChild("#RightAMS", ImVec2(half_width, ams_item_height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
        {
            imgui.text(_u8L("Right nozzle"));
            ImGui::Dummy({window_padding, window_padding});
            int index = 1;
            for (const auto &extruder_filament : m_right_extruder_filament) {
                imgui.filament_group(get_filament_display_type(extruder_filament), extruder_filament.hex_color.c_str(), extruder_filament.filament_id, filament_group_item_align_width);
                if (index % 4 != 0) { ImGui::SameLine(0, spacing); }
                index++;
            }
            ImGui::EndChild();
        }
        ImGui::PopStyleColor(1);
        ImGui::PopStyleVar(1);

        ImGui::Dummy({window_padding, window_padding});
        imgui.text_wrapped(from_u8(_u8L("Please place filaments on the printer based on grouping result.")), ImGui::GetContentRegionAvail().x);
        ImGui::Dummy({window_padding, window_padding});

        {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_dash_line(draw_list);
        }

        bool is_optimal_group = true;
        float parent_width = ImGui::GetContentRegionAvail().x;
        auto number_format = [](float num) {
            if (num > 1000) {
                std::string number_str = std::to_string(num);
                std::string first_three_digits = number_str.substr(0, 3);
                return std::stoi(first_three_digits);
            }
            return static_cast<int>(num);
        };

        if (any_more_to_best) {
            ImGui::Dummy({window_padding, window_padding});
            is_optimal_group = false;
            ImVec4 orangeColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, orangeColor);
            imgui.text(_u8L("Tips:"));
            imgui.text(_u8L("Current grouping of slice result is not optimal."));
            wxString tip;
            if (delta_weight_to_best >= 0 && delta_change_to_best >= 0)
                tip = from_u8((boost::format(_u8L("Increase %1%g filament and %2% changes compared to optimal grouping."))
                    % number_format(delta_weight_to_best)
                    % delta_change_to_best).str());
            else if (delta_weight_to_best >= 0 && delta_change_to_best < 0)
                tip = from_u8((boost::format(_u8L("Increase %1%g filament and save %2% changes compared to optimal grouping."))
                    % number_format(delta_weight_to_best)
                    % std::abs(delta_change_to_best)).str());
            else if (delta_weight_to_best < 0 && delta_change_to_best >= 0)
                tip = from_u8((boost::format(_u8L("Save %1%g filament and increase %2% changes compared to optimal grouping."))
                    % number_format(std::abs(delta_weight_to_best))
                    % delta_change_to_best).str());

            imgui.text_wrapped(tip, parent_width);
            ImGui::PopStyleColor(1);
        }
        else if (any_less_to_single_ext) {
            ImGui::Dummy({window_padding, window_padding});
            wxString tip;
            if (delta_weight_to_single_ext >= 0 && delta_change_to_single_ext >= 0)
                tip = from_u8((boost::format(_u8L("Save %1%g filament and %2% changes compared to a printer with one nozzle."))
                    % number_format(delta_weight_to_single_ext)
                    % delta_change_to_single_ext).str());
            else if (delta_weight_to_single_ext >= 0 && delta_change_to_single_ext < 0)
                tip = from_u8((boost::format(_u8L("Save %1%g filament and increase %2% changes compared to a printer with one nozzle."))
                    % number_format(delta_weight_to_single_ext)
                    % std::abs(delta_change_to_single_ext)).str());
            else if (delta_weight_to_single_ext < 0 && delta_change_to_single_ext >= 0)
                tip = from_u8((boost::format(_u8L("Increase %1%g filament and save %2% changes compared to a printer with one nozzle."))
                    % number_format(std::abs(delta_weight_to_single_ext))
                    % delta_change_to_single_ext).str());

            imgui.text_wrapped(tip, parent_width);
        }

        ImGui::Dummy({window_padding, window_padding});
        GLGizmoUtils::render_wiki_guide_button(*wxGetApp().plater()->get_current_canvas3D(), m_scale,"https://e.bambulab.com/t?c=mOkvsXkJ9pldGYp9");
        ImGui::SameLine();

        if (!is_optimal_group) {
            link_text_set_to_optional(_u8L("Set to Optimal"));
            ImGui::SameLine();
            ImGui::Dummy({window_padding, window_padding});
            ImGui::SameLine();
        }
        link_text(_u8L("Regroup filament"));

        ImGui::Dummy({window_padding, window_padding});

        ImGui::EndChild();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(1);
}

void GCodeViewer::render_legend(float &legend_height, int canvas_width, int canvas_height, int right_margin)
{
    if (!is_legend_shown())
        return;

    const Size cnv_size = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();

    ImGuiWrapper& imgui = *wxGetApp().imgui();

    imgui.set_next_window_pos(float(canvas_width - right_margin * m_scale), 4.0f * m_scale, ImGuiCond_Always, 1.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0,0.0));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f,1.0f,1.0f,0.6f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.42f, 0.42f, 0.42f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border, {1, 0, 0, 0});
    ImGui::SetNextWindowBgAlpha(0.8f);
    const float max_height = 0.75f * static_cast<float>(cnv_size.get_height());
    const float child_height = 0.3333f * max_height;
    ImGui::SetNextWindowSizeConstraints({ 0.0f, 0.0f }, { -1.0f, max_height });
    imgui.begin(std::string("Legend"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

    enum class EItemType : unsigned char
    {
        Rect,
        Circle,
        Hexagon,
        Line,
        None
    };

    const PrintEstimatedStatistics::Mode& time_mode = m_print_statistics.modes[static_cast<size_t>(m_viewer.get_time_mode())];
    const libvgcode::EViewType curr_view_type = m_viewer.get_view_type();
    const int curr_view_type_i = static_cast<int>(curr_view_type);
    const size_t current_time_mode = static_cast<size_t>(m_viewer.get_time_mode());
    const float total_estimated_time = time_mode.time > 0.0f ? time_mode.time : m_viewer.get_estimated_time();
    bool show_estimated_time = total_estimated_time > 0.0f && (curr_view_type == libvgcode::EViewType::FeatureType ||
        curr_view_type == libvgcode::EViewType::LayerTimeLinear || curr_view_type == libvgcode::EViewType::LayerTimeLogarithmic ||
        (curr_view_type == libvgcode::EViewType::ColorPrint && !time_mode.custom_gcode_times.empty()));

    const float icon_size = ImGui::GetTextLineHeight() * 0.7;
    const float percent_bar_size = 0;

    bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos_rect = ImGui::GetCursorScreenPos();
    float window_padding = 4.0f * m_scale;

    auto format_compact_count = [](unsigned long long value) {
        static constexpr const char* suffixes[] = { "", "K", "M", "B", "T", "P", "E" };
        constexpr size_t suffix_count = sizeof(suffixes) / sizeof(suffixes[0]);

        if (value < 1000)
            return std::to_string(value);

        size_t suffix_index = 0;
        unsigned long long divisor = 1;
        while (suffix_index + 1 < suffix_count && value / divisor >= 1000) {
            divisor *= 1000;
            ++suffix_index;
        }

        const unsigned long long whole = value / divisor;
        const unsigned long long tenths = (value % divisor) * 10 / divisor;

        std::string ret = std::to_string(whole);
        if (tenths != 0)
            ret += "." + std::to_string(tenths);
        ret += suffixes[suffix_index];
        return ret;
    };

    auto format_percent = [](float percent) {
        if (percent == 0.0f)
            return std::string("0");

        char buffer[64];
        percent > 0.001f ? ::sprintf(buffer, "%.1f", percent * 100.0f) : ::sprintf(buffer, "<0.1");
        return std::string(buffer);
    };

    auto append_item = [icon_size, &imgui, &window_padding, &draw_list, this](
        EItemType type,
        const ColorRGBA& color,
        const std::vector<std::pair<std::string, float>>& columns_offsets,
        bool checkbox = true,
        float checkbox_pos = 0.f,
        bool visible = true,
        std::function<void()> callback = nullptr)
    {
        ImVec2 pos = ImVec2(ImGui::GetCursorScreenPos().x + window_padding * 3, ImGui::GetCursorScreenPos().y);
        switch (type) {
        default:
        case EItemType::Rect: {
            draw_list->AddRectFilled({ pos.x + 1.0f * m_scale, pos.y + 3.0f * m_scale }, { pos.x + icon_size - 1.0f * m_scale, pos.y + icon_size + 1.0f * m_scale },
                                     ImGuiWrapper::to_ImU32(color));
            break;
        }
        case EItemType::Circle: {
            ImVec2 center(0.5f * (pos.x + pos.x + icon_size), 0.5f * (pos.y + pos.y + icon_size + 5.0f));
            draw_list->AddCircleFilled(center, 0.5f * icon_size, ImGuiWrapper::to_ImU32(color), 16);
            break;
        }
        case EItemType::Hexagon: {
            ImVec2 center(0.5f * (pos.x + pos.x + icon_size), 0.5f * (pos.y + pos.y + icon_size + 5.0f));
            draw_list->AddNgonFilled(center, 0.5f * icon_size, ImGuiWrapper::to_ImU32(color), 6);
            break;
        }
        case EItemType::Line: {
            draw_list->AddLine({ pos.x + 1, pos.y + icon_size + 2 }, { pos.x + icon_size - 1, pos.y + 4 }, ImGuiWrapper::to_ImU32(color), 3.0f);
            break;
        }
        case EItemType::None:
            break;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(20.0 * m_scale, 6.0 * m_scale));

        ImGui::Dummy({ 0.0, 0.0 });
        ImGui::SameLine();
        if (callback) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * m_scale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0 * m_scale, 0.0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.00f, 0.68f, 0.26f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.00f, 0.68f, 0.26f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
            float max_height = 0.f;
            for (auto column_offset : columns_offsets) {
                if (ImGui::CalcTextSize(column_offset.first.c_str()).y > max_height)
                    max_height = ImGui::CalcTextSize(column_offset.first.c_str()).y;
            }
            bool b_menu_item = ImGui::BBLMenuItem(("##" + columns_offsets[0].first).c_str(), nullptr, false, true, max_height);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
            if (b_menu_item)
                callback();
            if (checkbox) {
                ImGui::SameLine(checkbox_pos);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0, 0.0));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 0.0));
                ImGui::Text("%s", into_u8(visible ? ImGui::VisibleIcon : ImGui::HiddenIcon).c_str());
                ImGui::PopStyleVar(2);
            }
        }

        {
            if(callback && !checkbox && !visible)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(172 / 255.0f, 172 / 255.0f, 172 / 255.0f, 1.00f));
            float dummy_size = type == EItemType::None ? window_padding * 3 : ImGui::GetStyle().ItemSpacing.x + icon_size;
            ImGui::SameLine(dummy_size);
            imgui.text(columns_offsets[0].first);

            for (auto i = 1; i < columns_offsets.size(); i++) {
                ImGui::SameLine(columns_offsets[i].second);
                imgui.text(columns_offsets[i].first);
            }
            if (callback && !checkbox && !visible)
                ImGui::PopStyleColor(1);
        }

        ImGui::PopStyleVar(1);

    };

    auto append_range = [append_item](const libvgcode::ColorRange& range, unsigned int decimals) {
        auto append_range_item = [append_item, &range](int i, float value, unsigned int decimals) {
            char buf[1024];
            ::sprintf(buf, "%.*f", decimals, value);
            append_item(EItemType::Rect, libvgcode::convert(range.get_palette()[i]), { { buf , 0} });
        };

        std::vector<float> values = range.get_values();
        if (values.size() == 1)
            append_range_item(0, values.front(), decimals);
        else if (values.size() == 2) {
            append_range_item(static_cast<int>(range.get_palette().size()) - 1, values.back(), decimals);
            append_range_item(0, values.front(), decimals);
        }
        else {
            for (int i = static_cast<int>(range.get_palette().size()) - 1; i >= 0; --i) {
                append_range_item(i, values[i], decimals);
            }
        }
    };

    auto append_headers = [&imgui, window_padding, this](const std::vector<std::pair<std::string, float>>& title_offsets) {
        for (size_t i = 0; i < title_offsets.size(); i++) {
            if (title_offsets[i].first == _u8L("Display")) {
                ImGui::SameLine(title_offsets[i].second);
                ImGui::Dummy({16.f * m_scale, 1});
                continue;
            }
            ImGui::SameLine(title_offsets[i].second);
            imgui.bold_text(title_offsets[i].first);
        }
        ImGui::SameLine();
        ImGui::Dummy({window_padding, 1});
        ImGui::Separator();
    };

    auto max_width = [](const std::vector<std::string>& items, const std::string& title, float extra_size = 0.0f) {
        float ret = ImGui::CalcTextSize(title.c_str()).x;
        for (const std::string& item : items) {
            ret = std::max(ret, extra_size + ImGui::CalcTextSize(item.c_str()).x);
        }
        return ret;
    };

    auto calculate_offsets = [max_width, this](const std::vector<std::pair<std::string, std::vector<std::string>>>& title_columns, float extra_size = 0.0f) {
            const ImGuiStyle& style = ImGui::GetStyle();
            std::vector<float> offsets;
            offsets.push_back(max_width(title_columns[0].second, title_columns[0].first, extra_size) + 12.f * m_scale + ImGui::GetTextLineHeight());
            for (size_t i = 1; i < title_columns.size() - 1; i++)
                offsets.push_back(offsets.back() + max_width(title_columns[i].second, title_columns[i].first) + ((title_columns[i].first == _u8L("Display") ? 0 : 12.f) * m_scale));
            if (title_columns.back().first == _u8L("Display") && title_columns.size() > 2)
                offsets[title_columns.size() - 2] -= 3.f;

            float average_col_width = ImGui::GetWindowWidth() / static_cast<float>(title_columns.size());
            std::vector<float> ret;
            ret.push_back(0);
            for (size_t i = 1; i < title_columns.size(); i++) {
                ret.push_back(std::max(offsets[i - 1], i * average_col_width));
            }
            return ret;
    };

    auto color_print_ranges = [this](unsigned char extruder_id, const std::vector<CustomGCode::Item>& custom_gcode_per_print_z) {
        std::vector<std::pair<ColorRGBA, std::pair<double, double>>> ret;
        ret.reserve(custom_gcode_per_print_z.size());

        std::vector<float> zs;
        bool zs_built = false;

        for (const auto& item : custom_gcode_per_print_z) {
            if (extruder_id + 1 != static_cast<unsigned char>(item.extruder))
                continue;

            if (item.type != CustomGCode::ColorChange)
                continue;

            if (!zs_built) {
                zs = m_viewer.get_layers_zs();
                zs_built = true;
            }
            auto lower_b = std::lower_bound(zs.begin(), zs.end(),
                static_cast<float>(item.print_z - epsilon()));
            if (lower_b == zs.end())
                continue;

            const double current_z = *lower_b;
            const double previous_z = (lower_b == zs.begin()) ? 0.0 : *(--lower_b);

            if (ret.empty() || !(ret.back().second.first == previous_z && ret.back().second.second == current_z))
            {
                ColorRGBA color;
                decode_color(item.color, color);
                ret.push_back({ color, { previous_z, current_z } });
            }
        }

        return ret;
    };

    auto upto_label = [](double z) {
        char buf[64];
        ::sprintf(buf, "%.2f", z);
        return format(_u8L("up to %1% mm"), buf);
    };

    auto above_label = [](double z) {
        char buf[64];
        ::sprintf(buf, "%.2f", z);
        return format(_u8L("above %1% mm"), buf);
    };

    auto fromto_label = [](double z1, double z2) {
        char buf1[64];
        ::sprintf(buf1, "%.2f", z1);
        char buf2[64];
        ::sprintf(buf2, "%.2f", z2);
        return format(_u8L("from %1% to %2% mm"), buf1, buf2);
    };

    auto role_time_and_percent = [this, total_estimated_time](libvgcode::EGCodeExtrusionRole role) {
        const float time = m_viewer.get_extrusion_role_estimated_time(role);
        return std::make_pair(time, total_estimated_time > 0.0f ? time / total_estimated_time : 0.0f);
    };

    auto travel_time_and_percent = [this, total_estimated_time]() {
        const float time = m_viewer.get_travels_estimated_time();
        return std::make_pair(time, total_estimated_time > 0.0f ? time / total_estimated_time : 0.0f);
    };

    auto format_distance = [imperial_units](float distance_mm) {
        char buffer[64];
        if (imperial_units) {
            ::sprintf(buffer, "%.2fin", distance_mm / GizmoObjectManipulation::in_to_mm);
        } else if (std::fabs(distance_mm) < 1000.0f) {
            ::sprintf(buffer, "%.0fmm", distance_mm);
        } else {
            ::sprintf(buffer, "%.2fm", distance_mm / 1000.0f);
        }
        return std::string(buffer);
    };

    auto used_filament_per_role = [this, imperial_units](ExtrusionRole role) {
        auto it = m_print_statistics.used_filaments_per_role.find(role);
        if (it == m_print_statistics.used_filaments_per_role.end())
            return std::make_pair(0.0, 0.0);

        double koef        = imperial_units ? GizmoObjectManipulation::in_to_mm / 1000.0 : 1.0;
        double unit_conver = imperial_units ? GizmoObjectManipulation::oz_to_g : 1;
        return std::make_pair(it->second.first / koef, it->second.second / unit_conver);
    };

    auto get_used_filament_from_volume = [this, imperial_units](double volume, int extruder_id) {
        double koef = imperial_units ? 1.0 / GizmoObjectManipulation::in_to_mm : 0.001;
        std::pair<double, double> ret = { koef * volume / (PI * sqr(0.5 * m_filament_diameters[extruder_id])),
                                          volume * m_filament_densities[extruder_id] * 0.001 };
        return ret;
    };

    ImGui::Dummy({ window_padding, window_padding });
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine(window_padding * 2);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(84 / 255.f, 84 / 255.f, 90 / 255.f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(84 / 255.f, 84 / 255.f, 90 / 255.f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding , ImVec2(2.f, 2.f) * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * m_scale);
    const wchar_t foldIcon = m_fold ? ImGui::UnfoldButtonIcon : ImGui::FoldButtonIcon;
    if (imgui.glyph_button(foldIcon, ImVec2(16.f, 16.f) * m_scale)) {
        m_fold = !m_fold;
    }

    ImGui::SameLine();
    const wchar_t gCodeToggle = ImGui::gCodeButtonIcon;
    if (imgui.glyph_button(gCodeToggle, ImVec2(16.f, 16.f) * m_scale)) {
        wxGetApp().toggle_show_gcode_window();
        wxGetApp().plater()->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
    }

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    push_combo_style();

    ImGui::SameLine();
    const char* view_type_value = view_type_items_str[m_view_type_sel].c_str();
    ImGuiComboFlags flags = ImGuiComboFlags_HeightLargest;
    if (ImGui::BBLBeginCombo("", view_type_value, flags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        for (int i = 0; i < view_type_items_str.size(); i++) {
            const bool is_selected = (m_view_type_sel == i);
            if (ImGui::BBLSelectable(view_type_items_str[i].c_str(), is_selected)) {
                m_fold = false;
                m_view_type_sel = i;
                set_view_type(view_type_items[m_view_type_sel]);
                reset_visible(view_type_items[m_view_type_sel]);
                const std::string view_type_name = view_type_to_config_name(view_type_items[m_view_type_sel]);
                if (!view_type_name.empty())
                    wxGetApp().app_config->set("preview_last_view_type", view_type_name);
                update_moves_slider();
            #if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                imgui.set_requires_extra_frame();
            #else
                wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
            #endif
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::PopStyleVar(1);
        ImGui::EndCombo();
    }
    pop_combo_style();
    ImGui::SameLine(0, window_padding);
    float predictable_icon_pos = ImGui::GetCursorPosX() - icon_size - window_padding - ImGui::GetStyle().ItemSpacing.x - 1.f * m_scale;
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::Dummy({ window_padding, window_padding });
    float window_width = ImGui::GetWindowWidth();

    if (m_fold) {
        legend_height = ImGui::GetFrameHeight() + window_padding * 4;
        ImGui::SameLine(window_width);
        ImGui::Dummy({ 0, 0 });
        imgui.end();
        ImGui::PopStyleColor(7);
        ImGui::PopStyleVar(2);
        return;
    }

    std::vector<float> offsets;
    std::vector<std::string> labels;
    std::vector<std::string> times;
    std::string travel_time;
    std::vector<std::string> percents;
    std::vector<std::string> used_filaments_length;
    std::vector<std::string> used_filaments_weight;
    std::string travel_percent;
    std::string travel_distance;
    std::string travel_moves;
    std::vector<double> model_used_filaments_m;
    std::vector<double> model_used_filaments_g;
    double total_model_used_filament_m = 0, total_model_used_filament_g = 0;
    std::vector<double> flushed_filaments_m;
    std::vector<double> flushed_filaments_g;
    double total_flushed_filament_m = 0, total_flushed_filament_g = 0;
    std::vector<double> wipe_tower_used_filaments_m;
    std::vector<double> wipe_tower_used_filaments_g;
    double total_wipe_tower_used_filament_m = 0, total_wipe_tower_used_filament_g = 0;
    std::vector<double> support_used_filaments_m;
    std::vector<double> support_used_filaments_g;
    double total_support_used_filament_m = 0, total_support_used_filament_g = 0;
    struct ColumnData {
        enum {
            Model = 1,
            Flushed = 2,
            WipeTower = 4,
            Support = 1 << 3,
        };
    };
    int displayed_columns = 0;
    std::map<std::string, float> color_print_offsets;
    const PrintStatistics& ps = wxGetApp().plater()->get_partplate_list().get_current_fff_print().print_statistics();
    double koef = imperial_units ? GizmoObjectManipulation::in_to_mm : 1000.0;
    double unit_conver = imperial_units ? GizmoObjectManipulation::oz_to_g : 1;
    std::vector<double> used_filaments_m;
    std::vector<double> used_filaments_g;

    for (size_t extruder_id : m_viewer.get_used_extruders_ids()) {
        if (m_print_statistics.model_volumes_per_extruder.find(extruder_id) == m_print_statistics.model_volumes_per_extruder.end()) {
            model_used_filaments_m.push_back(0.0);
            model_used_filaments_g.push_back(0.0);
        }
        else {
            double volume = m_print_statistics.model_volumes_per_extruder.at(extruder_id);
            auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
            model_used_filaments_m.push_back(model_used_filament_m);
            model_used_filaments_g.push_back(model_used_filament_g);
            total_model_used_filament_m += model_used_filament_m;
            total_model_used_filament_g += model_used_filament_g;
            displayed_columns |= ColumnData::Model;
        }
    }

    for (size_t extruder_id : m_viewer.get_used_extruders_ids()) {
        if (m_print_statistics.wipe_tower_volumes_per_extruder.find(extruder_id) == m_print_statistics.wipe_tower_volumes_per_extruder.end()) {
            wipe_tower_used_filaments_m.push_back(0.0);
            wipe_tower_used_filaments_g.push_back(0.0);
        }
        else {
            double volume = m_print_statistics.wipe_tower_volumes_per_extruder.at(extruder_id);
            auto [wipe_tower_used_filament_m, wipe_tower_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
            wipe_tower_used_filaments_m.push_back(wipe_tower_used_filament_m);
            wipe_tower_used_filaments_g.push_back(wipe_tower_used_filament_g);
            total_wipe_tower_used_filament_m += wipe_tower_used_filament_m;
            total_wipe_tower_used_filament_g += wipe_tower_used_filament_g;
            displayed_columns |= ColumnData::WipeTower;
        }
    }

    for (size_t extruder_id : m_viewer.get_used_extruders_ids()) {
        if (m_print_statistics.flush_per_filament.find(extruder_id) == m_print_statistics.flush_per_filament.end()) {
            flushed_filaments_m.push_back(0.0);
            flushed_filaments_g.push_back(0.0);
        }
        else {
            double volume = m_print_statistics.flush_per_filament.at(extruder_id);
            auto [flushed_filament_m, flushed_filament_g] = get_used_filament_from_volume(volume, extruder_id);
            flushed_filaments_m.push_back(flushed_filament_m);
            flushed_filaments_g.push_back(flushed_filament_g);
            total_flushed_filament_m += flushed_filament_m;
            total_flushed_filament_g += flushed_filament_g;
            displayed_columns |= ColumnData::Flushed;
        }
    }

    for (size_t extruder_id : m_viewer.get_used_extruders_ids()) {
        if (m_print_statistics.support_volumes_per_extruder.find(extruder_id) == m_print_statistics.support_volumes_per_extruder.end()) {
            support_used_filaments_m.push_back(0.0);
            support_used_filaments_g.push_back(0.0);
        }
        else {
            double volume = m_print_statistics.support_volumes_per_extruder.at(extruder_id);
            auto [used_filament_m, used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
            support_used_filaments_m.push_back(used_filament_m);
            support_used_filaments_g.push_back(used_filament_g);
            total_support_used_filament_m += used_filament_m;
            total_support_used_filament_g += used_filament_g;
            displayed_columns |= ColumnData::Support;
        }
    }


    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine();
    switch (curr_view_type)
    {
    case libvgcode::EViewType::FeatureType:
    {
        char buffer[64];
        const std::vector<libvgcode::EGCodeExtrusionRole>& roles = m_viewer.get_extrusion_roles();
        for (libvgcode::EGCodeExtrusionRole role : roles) {
            assert(static_cast<size_t>(role) < libvgcode::GCODE_EXTRUSION_ROLES_COUNT);
            if (static_cast<size_t>(role) < libvgcode::GCODE_EXTRUSION_ROLES_COUNT) {
                labels.push_back(_u8L(ExtrusionEntity::role_to_string(convert(role))));
                auto [time, percent] = role_time_and_percent(role);
                times.push_back((time > 0.0f) ? short_time(get_time_dhms(time)) : "");
                if (percent == 0)
                    ::sprintf(buffer, "0");
                else
                    percent > 0.001 ? ::sprintf(buffer, "%.1f", percent * 100) : ::sprintf(buffer, "<0.1");
                percents.push_back(buffer);

                auto [model_used_filament_m, model_used_filament_g] = used_filament_per_role(convert(role));
                ::sprintf(buffer, imperial_units ? "%.2fin" : "%.2fm", model_used_filament_m);
                used_filaments_length.push_back(buffer);
                used_filaments_weight.push_back(format_compact_weight(model_used_filament_g, imperial_units));
            }
        }

        {
            auto [time, percent] = travel_time_and_percent();
            travel_time = (time > 0.0f) ? short_time(get_time_dhms(time)) : "";
            if (percent == 0)
                ::sprintf(buffer, "0");
            else
                percent > 0.001 ? ::sprintf(buffer, "%.1f", percent * 100) : ::sprintf(buffer, "<0.1");
            travel_percent = buffer;
            percents.push_back(travel_percent);

            travel_distance = format_distance(m_print_statistics.total_travel_distance);
            used_filaments_length.push_back(travel_distance);

            travel_moves = format_compact_count(m_print_statistics.total_travel_moves);
            used_filaments_weight.push_back(travel_moves);
        }

        offsets = calculate_offsets({ {_u8L("Line Type"), labels}, {_u8L("Time"), times}, {"%", percents}, {"", used_filaments_length}, {"", used_filaments_weight}, {_u8L("Display"), {""}}}, icon_size);
        percents.pop_back();
        append_headers({{_u8L("Line Type"), offsets[0]}, {_u8L("Time"), offsets[1]}, {"%", offsets[2]}, {_u8L("Usage"), offsets[3]}, {_u8L("Display"), offsets[5]}});
        break;
    }
    case libvgcode::EViewType::Clearance:      { imgui.title(_u8L("Clearance (mm)")); break; }
    case libvgcode::EViewType::Nonplanar:      { imgui.title(_u8L("Non-planar")); break; }
    case libvgcode::EViewType::Height:         { imgui.title(_u8L("Layer height (mm)")); break; }
    case libvgcode::EViewType::Width:          { imgui.title(_u8L("Line width (mm)")); break; }
    case libvgcode::EViewType::Speed:
    {
        imgui.title(_u8L("Speed (mm/s)"));
        break;
    }
    case libvgcode::EViewType::ActualSpeed:
    {
        imgui.title(_u8L("Actual Speed (mm/s)"));
        break;
    }
    case libvgcode::EViewType::Acceleration:
    {
        imgui.title(_u8L("Acceleration (mm/s²)"));
        break;
    }
    case libvgcode::EViewType::Jerk:
    {
        imgui.title(_u8L("Jerk (mm/s)"));
        break;
    }
    case libvgcode::EViewType::FanSpeed:       { imgui.title(_u8L("Fan speed (%)")); break; }
    case libvgcode::EViewType::Temperature:    { imgui.title(_u8L("Temperature (℃)")); break; }
    case libvgcode::EViewType::PressureAdvance:{ imgui.title(_u8L("Pressure Advance")); break; }
    case libvgcode::EViewType::VolumetricFlowRate:
        { imgui.title(_u8L("Volumetric flow rate (mm³/s)")); break; }
    case libvgcode::EViewType::ActualVolumetricFlowRate:
        { imgui.title(_u8L("Actual volumetric flow rate (mm³/s)")); break; }
    case libvgcode::EViewType::LayerTimeLinear:
        { imgui.title(_u8L("Layer Time")); break; }
    case libvgcode::EViewType::LayerTimeLogarithmic:
        { imgui.title(_u8L("Layer Time (log)")); break; }

    case libvgcode::EViewType::Tool:
    {
        const size_t extruders_count = get_extruders_count();
        used_filaments_m = std::vector<double>(extruders_count, 0.0);
        used_filaments_g = std::vector<double>(extruders_count, 0.0);
        const std::vector<uint8_t>& used_extruders_ids = m_viewer.get_used_extruders_ids();
        for (uint8_t extruder_id : used_extruders_ids) {
            if (m_print_statistics.model_volumes_per_extruder.find(extruder_id) == m_print_statistics.model_volumes_per_extruder.end())
                continue;
            double volume = m_print_statistics.model_volumes_per_extruder.at(extruder_id);

            auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
            model_used_filaments_m.push_back(model_used_filament_m);
            model_used_filaments_g.push_back(model_used_filament_g);
        }

        offsets = calculate_offsets({ { "Extruder NNN", {""}}}, icon_size);
        append_headers({ {_u8L("Filament"), offsets[0]}, {_u8L("Usage"), offsets[1]} });
        break;
    }
    case libvgcode::EViewType::ColorPrint:
    {
        std::vector<std::string> total_filaments;
        char buffer[64];
        const std::string total_weight_text = format_compact_weight(ps.total_weight, imperial_units);
        ::sprintf(buffer, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", ps.total_used_filament / koef, total_weight_text.c_str());
        total_filaments.push_back(buffer);


        std::vector<std::pair<std::string, std::vector<std::string>>> title_columns;
        if (displayed_columns & ColumnData::Model) {
            title_columns.push_back({ _u8L("Filament"), {""} });
            title_columns.push_back({ _u8L("Model"), total_filaments });
        }
        if (displayed_columns & ColumnData::Support) {
            title_columns.push_back({ _u8L("Support"), total_filaments });
        }
        if (displayed_columns & ColumnData::Flushed) {
            title_columns.push_back({ _u8L("Flushed"), total_filaments });
        }
        if (displayed_columns & ColumnData::WipeTower) {
            title_columns.push_back({ _u8L("Tower"), total_filaments });
        }
        if ((displayed_columns & ~ColumnData::Model) > 0) {
            title_columns.push_back({ _u8L("Total"), total_filaments });
        }
        title_columns.push_back({ _u8L("Display"), {""}});
        auto offsets_ = calculate_offsets(title_columns, icon_size);
        std::vector<std::pair<std::string, float>> title_offsets;
        for (int i = 0; i < offsets_.size(); i++) {
            title_offsets.push_back({ title_columns[i].first, offsets_[i] });
            color_print_offsets[title_columns[i].first] = offsets_[i];
        }
        append_headers(title_offsets);

        break;
    }
    default: { break; }
    }

    auto append_option_item = [this, append_item, current_time_mode, total_estimated_time, &format_compact_count, &format_percent, &format_distance](libvgcode::EOptionType type, std::vector<float> offsets) {
        const bool full_layout = offsets.size() > 4;
        auto option_stats = [this, current_time_mode, total_estimated_time, &format_compact_count, &format_percent, &format_distance, full_layout](libvgcode::EOptionType option_type) -> std::array<std::string, 4> {
            libvgcode::EMoveType move_type;
            bool has_move_type = true;
            switch (option_type) {
            case libvgcode::EOptionType::Wipes:         { move_type = libvgcode::EMoveType::Wipe; break; }
            case libvgcode::EOptionType::Retractions:   { move_type = libvgcode::EMoveType::Retract; break; }
            case libvgcode::EOptionType::Unretractions: { move_type = libvgcode::EMoveType::Unretract; break; }
            case libvgcode::EOptionType::Seams:         { move_type = libvgcode::EMoveType::Seam; break; }
            case libvgcode::EOptionType::ToolChanges:   { move_type = libvgcode::EMoveType::ToolChange; break; }
            default:                                    { has_move_type = false; break; }
            }

            if (!has_move_type)
                return { "", "", "", "" };

            const size_t move_type_idx = static_cast<size_t>(move_type);
            float time = m_move_type_times[move_type_idx][current_time_mode];
            if (option_type == libvgcode::EOptionType::ToolChanges) {
                time = m_print_statistics.total_filament_load_time + m_print_statistics.total_filament_unload_time + m_print_statistics.total_tool_change_time;
            }
            const std::string time_text = full_layout && time > 0.0f ? short_time(get_time_dhms(time)) : "";
            const std::string percent_text = full_layout && total_estimated_time > 0.0f ? format_percent(time / total_estimated_time) : "";
            const float seam_distance = m_print_statistics.total_seam_gap_distance + m_print_statistics.total_seam_scarf_distance;
            const float distance = (option_type == libvgcode::EOptionType::Seams && seam_distance > 0.0f) ? seam_distance : m_move_type_distances[move_type_idx];
            const std::string distance_text = full_layout && (option_type == libvgcode::EOptionType::Wipes || option_type == libvgcode::EOptionType::Retractions || option_type == libvgcode::EOptionType::Unretractions || option_type == libvgcode::EOptionType::Seams)
                ? format_distance(distance)
                : "";
            const std::string count_text = full_layout ? format_compact_count(m_move_type_counts[move_type_idx]) : "";

            return { time_text, percent_text, distance_text, count_text };
        };

        auto append_option_item_with_type = [this, offsets, append_item, full_layout](libvgcode::EOptionType type, const ColorRGBA& color, const std::string& label, bool visible,
            const std::string& time_text, const std::string& percent_text, const std::string& distance_text, const std::string& count_text) {
            std::vector<std::pair<std::string, float>> columns_offsets;
            columns_offsets.push_back({ label , offsets[0] });
            if (full_layout && !time_text.empty())
                columns_offsets.push_back({ time_text, offsets[1] });
            if (full_layout && !percent_text.empty())
                columns_offsets.push_back({ percent_text, offsets[2] });
            if (full_layout && !distance_text.empty())
                columns_offsets.push_back({ distance_text, offsets[3] });
            if (full_layout && !count_text.empty())
                columns_offsets.push_back({ count_text, distance_text.empty() ? offsets[3] : offsets[4] });
            append_item(EItemType::Rect, color, columns_offsets, true, offsets.back() , visible, [this, type]() {
                m_viewer.toggle_option_visibility(type);
                update_moves_slider();
                });
        };
        const bool visible = m_viewer.is_option_visible(type);
        if (type == libvgcode::EOptionType::Travels) {
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Travels)), _u8L("Travel"), visible, "", "", "", "");
        }
        else if (type == libvgcode::EOptionType::Seams) {
            const auto option_values = option_stats(type);
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Seams)), _u8L("Seams"), visible,
                option_values[0], option_values[1], option_values[2], option_values[3]);
        }
        else if (type == libvgcode::EOptionType::Retractions) {
            const auto option_values = option_stats(type);
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Retractions)), _u8L("Retract"), visible,
                option_values[0], option_values[1], option_values[2], option_values[3]);
        }
        else if (type == libvgcode::EOptionType::Unretractions) {
            const auto option_values = option_stats(type);
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Unretractions)), _u8L("Unretract"), visible,
                option_values[0], option_values[1], option_values[2], option_values[3]);
        }
        else if (type == libvgcode::EOptionType::ToolChanges) {
            const auto option_values = option_stats(type);
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::ToolChanges)), _u8L("Filament changes"), visible,
                option_values[0], option_values[1], option_values[2], option_values[3]);
        }
        else if (type == libvgcode::EOptionType::Wipes) {
            const auto option_values = option_stats(type);
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Wipes)), _u8L("Wipe"), visible,
                option_values[0], option_values[1], option_values[2], option_values[3]);
        }
    };

    const libvgcode::EViewType new_view_type = curr_view_type;

    switch (new_view_type)
    {
    case libvgcode::EViewType::FeatureType:
    {
        auto roles = m_viewer.get_extrusion_roles();
        for (size_t i = 0; i < roles.size(); ++i) {
            libvgcode::EGCodeExtrusionRole role = roles[i];
            if (role >= libvgcode::EGCodeExtrusionRole::COUNT)
                continue;
            const bool visible = m_viewer.is_extrusion_role_visible(role);
            std::vector<std::pair<std::string, float>> columns_offsets;
            columns_offsets.push_back({ labels[i], offsets[0] });
            columns_offsets.push_back({ times[i], offsets[1] });
            columns_offsets.push_back({percents[i], offsets[2]});
            columns_offsets.push_back({used_filaments_length[i], offsets[3]});
            columns_offsets.push_back({used_filaments_weight[i], offsets[4]});
            append_item(EItemType::Rect, libvgcode::convert(m_viewer.get_extrusion_role_color(role)), columns_offsets,
                true, offsets.back(), visible, [this, role]() {
                    m_viewer.toggle_extrusion_role_visibility(role);
                    update_moves_slider();
                });
        }

        auto options = m_viewer.get_options();
        for(auto item : options) {
            if (item != libvgcode::EOptionType::Travels) {
                append_option_item(item, offsets);
            } else {
                const bool visible = m_viewer.is_option_visible(item);
                std::vector<std::pair<std::string, float>> columns_offsets;
                columns_offsets.push_back({ _u8L("Travel"), offsets[0] });
                columns_offsets.push_back({ travel_time, offsets[1] });
                columns_offsets.push_back({ travel_percent, offsets[2] });
                columns_offsets.push_back({ travel_distance, offsets[3] });
                columns_offsets.push_back({ travel_moves, offsets[4] });
                append_item(EItemType::Rect, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Travels)), columns_offsets, true, offsets.back() , visible, [this, item]() {
                        m_viewer.toggle_option_visibility(item);
                        update_moves_slider();
                    });
            }
        }
        break;
    }
    case libvgcode::EViewType::Clearance: {
        const float indent = window_padding + ImGui::GetStyle().ItemSpacing.x;
        if (!m_clearance_report.ran) {
            ImGui::Indent(indent);
            ImGui::TextUnformatted(_u8L("Not checked").c_str());
            ImGui::Unindent(indent);
            break;
        }
        for (float value : { m_clearance_head_floor, std::min(m_clearance_margin, m_clearance_head_floor), 0.f }) {
            const std::string label = into_u8(wxString::Format("%.3f", value));
            append_item(EItemType::Rect, libvgcode::convert(libvgcode::clearance_color(value, m_clearance_margin, m_clearance_head_floor)), {{label, 0}});
        }
        ImGui::Indent(indent);
        if (!m_clearance_warning.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 28.f * ImGui::GetFontSize());
            ImGui::TextWrapped("%s", m_clearance_warning.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button(_u8L("Jump to move").c_str()))
                jump_to_clearance_move(m_clearance_report.worst.front().move_id, m_last_result_id);
        }
        if (ImGui::TreeNode(_u8L("Verification limits").c_str())) {
            for (const auto &omission : m_clearance_report.omissions)
                ImGui::TextWrapped("%s", omission.c_str());
            ImGui::TreePop();
        }
        ImGui::Unindent(indent);
        break;
    }
    case libvgcode::EViewType::Nonplanar: {
        const float indent = window_padding + ImGui::GetStyle().ItemSpacing.x;
        auto indented = [indent](auto &&draw) {
            ImGui::Indent(indent);
            draw();
            ImGui::Unindent(indent);
        };
        const std::array<std::string, 4> labels{ _u8L("Shell"), _u8L("Band"), _u8L("Bead height"), _u8L("Slope") };
        const libvgcode::ENonplanarColor mode = m_viewer.get_nonplanar_color();
        ImGui::Indent(indent);
        for (size_t i = 0; i < labels.size(); ++i) {
            if (i > 0)
                ImGui::SameLine();
            if (ImGui::RadioButton(labels[i].c_str(), size_t(mode) == i)) {
                m_viewer.set_nonplanar_color(libvgcode::ENonplanarColor(i));
                wxGetApp().app_config->set("preview_nonplanar_color", nonplanar_color_names[i]);
                imgui.set_requires_extra_frame();
            }
        }
        if (m_nonplanar_shells == 0) {
            ImGui::TextUnformatted(_u8L("No curved passes").c_str());
            render_nonplanar_legend();
            ImGui::Unindent(indent);
            break;
        }
        ImGui::Unindent(indent);
        switch (mode) {
        case libvgcode::ENonplanarColor::Shell:
            for (int k = m_nonplanar_shells - 1; k >= 0; --k)
                append_item(EItemType::Rect, libvgcode::convert(m_viewer.get_color_range(libvgcode::EViewType::Nonplanar).get_color_at(float(k))),
                    { { into_u8(format_wxstr(_L("Shell %1%"), k + 1)), 0 } });
            break;
        case libvgcode::ENonplanarColor::Band:
        {
            static constexpr int hues = 7;
            if (m_nonplanar_bands.front() < 0)
                append_item(EItemType::Rect, libvgcode::convert(libvgcode::nonplanar_index_color(-1)), { { _u8L("Unbanded"), 0 } });
            std::vector<std::vector<int>> rows(hues);
            for (auto band = m_nonplanar_bands.rbegin(); band != m_nonplanar_bands.rend() && *band >= 0; ++band)
                rows[size_t(*band % hues)].emplace_back(*band);
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return !a.empty() && (b.empty() || a.front() > b.front()); });
            for (const std::vector<int> &bands : rows) {
                if (bands.empty())
                    break;
                std::string label;
                for (size_t k = 0; k < bands.size() && k < 3; ++k)
                    label += (k > 0 ? ", " : "") + std::to_string(bands[k] + 1);
                if (bands.size() > 3)
                    label += ", …";
                append_item(EItemType::Rect, libvgcode::convert(libvgcode::nonplanar_index_color(bands.front() % hues)),
                    { { into_u8(format_wxstr(bands.size() > 1 ? _L("Bands %1%") : _L("Band %1%"), label)), 0 } });
            }
            break;
        }
        case libvgcode::ENonplanarColor::BeadHeight:
            indented([] { ImGui::TextUnformatted(_u8L("Bead height (mm)").c_str()); });
            append_range(m_viewer.get_color_range(libvgcode::EViewType::Nonplanar), 2);
            break;
        default:
            indented([] { ImGui::TextUnformatted(_u8L("Surface slope (°)").c_str()); });
            append_range(m_viewer.get_color_range(libvgcode::EViewType::Nonplanar), 0);
            break;
        }
        append_item(EItemType::Rect, libvgcode::convert(libvgcode::NONPLANAR_PLANAR_COLOR), { { _u8L("Planar"), 0 } });
        indented([this] { render_nonplanar_legend(); });
        break;
    }
    case libvgcode::EViewType::Height:                   { append_range(m_viewer.get_color_range(libvgcode::EViewType::Height), 2); break; }
    case libvgcode::EViewType::Width:                    { append_range(m_viewer.get_color_range(libvgcode::EViewType::Width), 2); break; }
    case libvgcode::EViewType::Speed:       {
        append_range(m_viewer.get_color_range(libvgcode::EViewType::Speed), 0);
        ImGui::Spacing();
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        offsets = calculate_offsets({ { _u8L("Options"), { _u8L("Travel")}}, { _u8L("Display"), {""}} }, icon_size);
        append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
        const bool travel_visible = m_viewer.is_option_visible(libvgcode::EOptionType::Travels);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 3.0f));
        append_item(EItemType::None, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Travels)), { {_u8L("Travel"), offsets[0] }}, true, predictable_icon_pos , travel_visible, [this]() {
            m_viewer.toggle_option_visibility(libvgcode::EOptionType::Travels);
            update_moves_slider();
            });
        ImGui::PopStyleVar(1);
        break;
    }
    case libvgcode::EViewType::ActualSpeed: {
        append_range(m_viewer.get_color_range(libvgcode::EViewType::ActualSpeed), 0);
        ImGui::Spacing();
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        offsets = calculate_offsets({ { _u8L("Options"), { _u8L("Travel")}}, { _u8L("Display"), {""}} }, icon_size);
        append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
        const bool travel_visible = m_viewer.is_option_visible(libvgcode::EOptionType::Travels);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 3.0f));
        append_item(EItemType::None, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Travels)), { {_u8L("Travel"), offsets[0] }}, true, predictable_icon_pos , travel_visible, [this]() {
            m_viewer.toggle_option_visibility(libvgcode::EOptionType::Travels);
            update_moves_slider();
            });
        ImGui::PopStyleVar(1);
        break;
    }
    case libvgcode::EViewType::Acceleration: {
        append_range(m_viewer.get_color_range(libvgcode::EViewType::Acceleration), 0);
        ImGui::Spacing();
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        offsets = calculate_offsets({ { _u8L("Options"), { _u8L("Travel")}}, { _u8L("Display"), {""}} }, icon_size);
        append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
        const bool travel_visible = m_viewer.is_option_visible(libvgcode::EOptionType::Travels);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 3.0f));
        append_item(EItemType::None, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Travels)), { {_u8L("Travel"), offsets[0] }}, true, predictable_icon_pos , travel_visible, [this]() {
            m_viewer.toggle_option_visibility(libvgcode::EOptionType::Travels);
            update_moves_slider();
            });
        ImGui::PopStyleVar(1);
        break;
    }
    case libvgcode::EViewType::Jerk: {
        append_range(m_viewer.get_color_range(libvgcode::EViewType::Jerk), 1);
        ImGui::Spacing();
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        offsets = calculate_offsets({ { _u8L("Options"), { _u8L("Travel")}}, { _u8L("Display"), {""}} }, icon_size);
        append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
        const bool travel_visible = m_viewer.is_option_visible(libvgcode::EOptionType::Travels);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 3.0f));
        append_item(EItemType::None, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Travels)), { {_u8L("Travel"), offsets[0] }}, true, predictable_icon_pos , travel_visible, [this]() {
            m_viewer.toggle_option_visibility(libvgcode::EOptionType::Travels);
            update_moves_slider();
            });
        ImGui::PopStyleVar(1);
        break;
    }
    case libvgcode::EViewType::FanSpeed:                 { append_range(m_viewer.get_color_range(libvgcode::EViewType::FanSpeed), 0); break; }
    case libvgcode::EViewType::Temperature:              { append_range(m_viewer.get_color_range(libvgcode::EViewType::Temperature), 0); break; }
    case libvgcode::EViewType::PressureAdvance:          { append_range(m_viewer.get_color_range(libvgcode::EViewType::PressureAdvance), 3); break; }
    case libvgcode::EViewType::LayerTimeLinear:          { append_range(m_viewer.get_color_range(libvgcode::EViewType::LayerTimeLinear), true); break; }
    case libvgcode::EViewType::LayerTimeLogarithmic:     { append_range(m_viewer.get_color_range(libvgcode::EViewType::LayerTimeLogarithmic), true); break; }
    case libvgcode::EViewType::VolumetricFlowRate:       { append_range(m_viewer.get_color_range(libvgcode::EViewType::VolumetricFlowRate), 2); break; }
    case libvgcode::EViewType::ActualVolumetricFlowRate: { append_range(m_viewer.get_color_range(libvgcode::EViewType::ActualVolumetricFlowRate), 2); break; }
    case libvgcode::EViewType::Tool:
    {
        char buf[64];
        size_t i = 0;
        const std::vector<uint8_t>& used_extruders_ids = m_viewer.get_used_extruders_ids();
        for (uint8_t extruder_id : used_extruders_ids) {
            const std::string weight_text = format_compact_weight(model_used_filaments_g[i], imperial_units);
            ::sprintf(buf, imperial_units ? "%.2f in    %s" : "%.2f m    %s", model_used_filaments_m[i], weight_text.c_str());
            append_item(EItemType::Rect, libvgcode::convert(m_viewer.get_tool_colors()[extruder_id]), { { _u8L("Extruder") + " " + std::to_string(extruder_id + 1), offsets[0]}, {buf, offsets[1]} });
            i++;
        }
        break;
    }
    case libvgcode::EViewType::Summary:
    {
        char buf[64];
        imgui.text(_u8L("Total") + ":");
        ImGui::SameLine();
        const std::string total_weight_text = format_compact_weight(ps.total_weight, imperial_units);
        ::sprintf(buf, imperial_units ? "%.2f in / %s" : "%.2f m / %s", ps.total_used_filament / koef, total_weight_text.c_str());
        imgui.text(buf);

        ImGui::Dummy({window_padding, window_padding});
        ImGui::SameLine();
        imgui.text(_u8L("Cost") + ":");
        ImGui::SameLine();
        ::sprintf(buf, "%.2f", ps.total_cost);
        imgui.text(buf);

        ImGui::Dummy({window_padding, window_padding});
        ImGui::SameLine();
        imgui.text(_u8L("Total time") + ":");
        ImGui::SameLine();
        imgui.text(short_time(get_time_dhms(time_mode.time)));
        break;
    }
    case libvgcode::EViewType::ColorPrint: {
        const std::vector<CustomGCode::Item>& custom_gcode_per_print_z = wxGetApp().is_editor() ? wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes : m_custom_gcode_per_print_z;
        size_t total_items = 1;

        const bool need_scrollable = static_cast<float>(total_items) * (icon_size + ImGui::GetStyle().ItemSpacing.y) > child_height;

        if (need_scrollable)
            ImGui::BeginChild("color_prints", { -1.0f, child_height }, false);

        size_t i = 0;
        const std::vector<uint8_t>& used_extruders_ids = m_viewer.get_used_extruders_ids();
        auto tool_colors = m_viewer.get_tool_colors();
        for (auto extruder_idx : used_extruders_ids) {
            if (i < model_used_filaments_m.size() && i < model_used_filaments_g.size()) {
                std::vector<std::pair<std::string, float>> columns_offsets;
                columns_offsets.push_back({ std::to_string(extruder_idx + 1), color_print_offsets[_u8L("Filament")]});

                char buf[64];
                float column_sum_m = 0.0f;
                float column_sum_g = 0.0f;
                if (displayed_columns & ColumnData::Model) {
                    const std::string weight_text = format_compact_weight(model_used_filaments_g[i], imperial_units);
                    if ((displayed_columns & ~ColumnData::Model) > 0)
                        ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", model_used_filaments_m[i], weight_text.c_str());
                    else
                        ::sprintf(buf, imperial_units ? "%.2f in    %s" : "%.2f m    %s", model_used_filaments_m[i], weight_text.c_str());
                    columns_offsets.push_back({ buf, color_print_offsets[_u8L("Model")] });
                    column_sum_m += model_used_filaments_m[i];
                    column_sum_g += model_used_filaments_g[i];
                }
                if (displayed_columns & ColumnData::Support) {
                    const std::string weight_text = format_compact_weight(support_used_filaments_g[i], imperial_units);
                    ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", support_used_filaments_m[i], weight_text.c_str());
                    columns_offsets.push_back({ buf, color_print_offsets[_u8L("Support")] });
                    column_sum_m += support_used_filaments_m[i];
                    column_sum_g += support_used_filaments_g[i];
                }
                if (displayed_columns & ColumnData::Flushed) {
                    const std::string weight_text = format_compact_weight(flushed_filaments_g[i], imperial_units);
                    ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", flushed_filaments_m[i], weight_text.c_str());
                    columns_offsets.push_back({ buf, color_print_offsets[_u8L("Flushed")]});
                    column_sum_m += flushed_filaments_m[i];
                    column_sum_g += flushed_filaments_g[i];
                }
                if (displayed_columns & ColumnData::WipeTower) {
                    const std::string weight_text = format_compact_weight(wipe_tower_used_filaments_g[i], imperial_units);
                    ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", wipe_tower_used_filaments_m[i], weight_text.c_str());
                    columns_offsets.push_back({ buf, color_print_offsets[_u8L("Tower")] });
                    column_sum_m += wipe_tower_used_filaments_m[i];
                    column_sum_g += wipe_tower_used_filaments_g[i];
                }
                if ((displayed_columns & ~ColumnData::Model) > 0) {
                    const std::string weight_text = format_compact_weight(column_sum_g, imperial_units);
                    ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", column_sum_m, weight_text.c_str());
                    columns_offsets.push_back({ buf, color_print_offsets[_u8L("Total")] });
                }

                float checkbox_pos = std::max(predictable_icon_pos, color_print_offsets[_u8L("Display")]);
                append_item(EItemType::Rect, libvgcode::convert(tool_colors[extruder_idx]), columns_offsets, false, checkbox_pos , true, []() {});
            }
            i++;
        }

        if (need_scrollable)
            ImGui::EndChild();

        char buf[64];
        if (used_extruders_ids.size() > 1) {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const ImRect separator(ImVec2(window->Pos.x + window_padding * 3, window->DC.CursorPos.y), ImVec2(window->Pos.x + window->Size.x - window_padding * 3, window->DC.CursorPos.y + 1.0f));
            ImGui::ItemSize(ImVec2(0.0f, 0.0f));
            const bool item_visible = ImGui::ItemAdd(separator, 0);
            window->DrawList->AddLine(separator.Min, ImVec2(separator.Max.x, separator.Min.y), ImGui::GetColorU32(ImGuiCol_Separator));

            std::vector<std::pair<std::string, float>> columns_offsets;
            columns_offsets.push_back({ _u8L("Total"), color_print_offsets[_u8L("Filament")]});
            if (displayed_columns & ColumnData::Model) {
                const std::string weight_text = format_compact_weight(total_model_used_filament_g, imperial_units);
                if ((displayed_columns & ~ColumnData::Model) > 0)
                    ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", total_model_used_filament_m, weight_text.c_str());
                else
                    ::sprintf(buf, imperial_units ? "%.2f in    %s" : "%.2f m    %s", total_model_used_filament_m, weight_text.c_str());
                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Model")] });
            }
            if (displayed_columns & ColumnData::Support) {
                const std::string weight_text = format_compact_weight(total_support_used_filament_g, imperial_units);
                ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", total_support_used_filament_m, weight_text.c_str());
                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Support")] });
            }
            if (displayed_columns & ColumnData::Flushed) {
                const std::string weight_text = format_compact_weight(total_flushed_filament_g, imperial_units);
                ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", total_flushed_filament_m, weight_text.c_str());
                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Flushed")] });
            }
            if (displayed_columns & ColumnData::WipeTower) {
                const std::string weight_text = format_compact_weight(total_wipe_tower_used_filament_g, imperial_units);
                ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", total_wipe_tower_used_filament_m, weight_text.c_str());
                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Tower")] });
            }
            if ((displayed_columns & ~ColumnData::Model) > 0) {
                const std::string weight_text = format_compact_weight(total_model_used_filament_g + total_support_used_filament_g + total_flushed_filament_g + total_wipe_tower_used_filament_g, imperial_units);
                ::sprintf(buf, imperial_units ? "%.2f in\n%s" : "%.2f m\n%s", total_model_used_filament_m + total_support_used_filament_m + total_flushed_filament_m + total_wipe_tower_used_filament_m,
                    weight_text.c_str());
                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Total")] });
            }
            append_item(EItemType::None, libvgcode::convert(tool_colors[0]), columns_offsets);
        }

        ImGui::Dummy({window_padding, window_padding});
        ImGui::SameLine();
        imgui.text(_u8L("Filament change times") + ":");
        ImGui::SameLine();
        imgui.text(format_compact_count(m_print_statistics.total_filament_changes));

        ImGui::Dummy({window_padding, window_padding});
        ImGui::SameLine();
        imgui.text(_u8L("Tool changes") + ":");
        ImGui::SameLine();
        imgui.text(format_compact_count(m_print_statistics.total_extruder_changes));

        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(_u8L("Cost")+":");
        ImGui::SameLine();
        ::sprintf(buf, "%.2f", ps.total_cost);
        imgui.text(buf);

        break;
    }
    default: { break; }
    }

    if (new_view_type == libvgcode::EViewType::ColorPrint) {
        using Times = std::pair<float, float>;
        using TimesList = std::vector<std::pair<CustomGCode::Type, Times>>;

        struct PartialTime
        {
            enum class EType : unsigned char
            {
                Print,
                ColorChange,
                Pause
            };
            EType type;
            int extruder_id;
            ColorRGBA color1;
            ColorRGBA color2;
            Times times;
            std::pair<double, double> used_filament {0.0f, 0.0f};
        };
        using PartialTimes = std::vector<PartialTime>;

        auto generate_partial_times = [this, get_used_filament_from_volume](const TimesList& times, const std::vector<double>& used_filaments) {
            PartialTimes items;

            std::vector<CustomGCode::Item> custom_gcode_per_print_z = wxGetApp().is_editor() ? wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes : m_custom_gcode_per_print_z;
            const size_t extruders_count = get_extruders_count();
            std::vector<ColorRGBA> last_color(extruders_count);
            for (size_t i = 0; i < extruders_count; ++i) {
                last_color[i] = libvgcode::convert(m_viewer.get_tool_colors()[i]);
            }
            int last_extruder_id = 1;
            int color_change_idx = 0;
            for (const auto& time_rec : times) {
                switch (time_rec.first)
                {
                case CustomGCode::PausePrint: {
                    auto it = std::find_if(custom_gcode_per_print_z.begin(), custom_gcode_per_print_z.end(), [time_rec](const CustomGCode::Item& item) { return item.type == time_rec.first; });
                    if (it != custom_gcode_per_print_z.end()) {
                        items.push_back({ PartialTime::EType::Print, it->extruder, last_color[it->extruder - 1], ColorRGBA::BLACK(), time_rec.second });
                        items.push_back({ PartialTime::EType::Pause, it->extruder, ColorRGBA::BLACK(), ColorRGBA::BLACK(), time_rec.second });
                        custom_gcode_per_print_z.erase(it);
                    }
                    break;
                }
                case CustomGCode::ColorChange: {
                    auto it = std::find_if(custom_gcode_per_print_z.begin(), custom_gcode_per_print_z.end(), [time_rec](const CustomGCode::Item& item) { return item.type == time_rec.first; });
                    if (it != custom_gcode_per_print_z.end()) {
                        items.push_back({ PartialTime::EType::Print, it->extruder, last_color[it->extruder - 1], ColorRGBA::BLACK(), time_rec.second, get_used_filament_from_volume(used_filaments[color_change_idx++], it->extruder - 1) });
                        ColorRGBA color;
                        decode_color(it->color, color);
                        items.push_back({ PartialTime::EType::ColorChange, it->extruder, last_color[it->extruder - 1], color, time_rec.second });
                        last_color[it->extruder - 1] = color;
                        last_extruder_id = it->extruder;
                        custom_gcode_per_print_z.erase(it);
                    }
                    else
                        items.push_back({ PartialTime::EType::Print, last_extruder_id, last_color[last_extruder_id - 1], ColorRGBA::BLACK(), time_rec.second, get_used_filament_from_volume(used_filaments[color_change_idx++], last_extruder_id - 1) });

                    break;
                }
                default: { break; }
                }
            }

            return items;
        };

        auto append_color_change = [&imgui](const ColorRGBA& color1, const ColorRGBA& color2, const std::array<float, 4>& offsets, const Times& times) {
            imgui.text(_u8L("Color change"));
            ImGui::SameLine();

            float icon_size = ImGui::GetTextLineHeight();
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            pos.x -= 0.5f * ImGui::GetStyle().ItemSpacing.x;

            draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                ImGuiWrapper::to_ImU32(color1));
            pos.x += icon_size;
            draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                ImGuiWrapper::to_ImU32(color2));

            ImGui::SameLine(offsets[0]);
            imgui.text(short_time(get_time_dhms(times.second - times.first)));
        };

        auto append_print = [&imgui, imperial_units](const ColorRGBA& color, const std::array<float, 4>& offsets, const Times& times, std::pair<double, double> used_filament) {
            imgui.text(_u8L_CONTEXT("Print", "Noun"));
            ImGui::SameLine();

            float icon_size = ImGui::GetTextLineHeight();
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            pos.x -= 0.5f * ImGui::GetStyle().ItemSpacing.x;

            draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                ImGuiWrapper::to_ImU32(color));

            ImGui::SameLine(offsets[0]);
            imgui.text(short_time(get_time_dhms(times.second)));
            ImGui::SameLine(offsets[1]);
            imgui.text(short_time(get_time_dhms(times.first)));
            if (used_filament.first > 0.0f) {
                char buffer[64];
                ImGui::SameLine(offsets[2]);
                ::sprintf(buffer, imperial_units ? "%.2f in" : "%.2f m", used_filament.first);
                imgui.text(buffer);

                ImGui::SameLine(offsets[3]);
                imgui.text(format_compact_weight(used_filament.second, imperial_units));
            }
        };

        PartialTimes partial_times = generate_partial_times(time_mode.custom_gcode_times, m_print_statistics.volumes_per_color_change);
        if (!partial_times.empty()) {
            labels.clear();
            times.clear();

            for (const PartialTime& item : partial_times) {
                switch (item.type)
                {
                case PartialTime::EType::Print:       { labels.push_back(_u8L_CONTEXT("Print", "Noun")); break; }
                case PartialTime::EType::Pause:       { labels.push_back(_u8L("Pause")); break; }
                case PartialTime::EType::ColorChange: { labels.push_back(_u8L("Color change")); break; }
                }
                times.push_back(short_time(get_time_dhms(item.times.second)));
            }

            std::string longest_used_filament_string;
            for (const PartialTime& item : partial_times) {
                if (item.used_filament.first > 0.0f) {
                    char buffer[64];
                    ::sprintf(buffer, imperial_units ? "%.2f in" : "%.2f m", item.used_filament.first);
                    if (::strlen(buffer) > longest_used_filament_string.length())
                        longest_used_filament_string = buffer;
                }
            }

        }
    }

    if (m_viewer.is_option_visible(libvgcode::EOptionType::Travels)) {
        switch (m_viewer.get_view_type())
        {
        case libvgcode::EViewType::Speed:
        case libvgcode::EViewType::ActualSpeed:
        case libvgcode::EViewType::Acceleration:
        case libvgcode::EViewType::Jerk:
        case libvgcode::EViewType::Tool:
        case libvgcode::EViewType::ColorPrint: {
            break;
        }
        default: {

            break;
        }
        }
    }

    bool has_settings = false;
    has_settings |= !m_settings_ids.print.empty();
    has_settings |= !m_settings_ids.printer.empty();
    bool has_filament_settings = true;
    has_filament_settings &= !m_settings_ids.filament.empty();
    for (const std::string& fs : m_settings_ids.filament) {
        has_filament_settings &= !fs.empty();
    }
    has_settings |= has_filament_settings;
    bool show_settings = m_only_gcode_in_preview;
    show_settings &= (new_view_type == libvgcode::EViewType::FeatureType || new_view_type == libvgcode::EViewType::Tool);
    show_settings &= has_settings;
    if (show_settings) {
        auto calc_offset = [this]() {
            float ret = 0.0f;
            if (!m_settings_ids.printer.empty())
                ret = std::max(ret, ImGui::CalcTextSize((_u8L("Printer") + std::string(":")).c_str()).x);
            if (!m_settings_ids.print.empty())
                ret = std::max(ret, ImGui::CalcTextSize((_u8L("Print settings") + std::string(":")).c_str()).x);
            if (!m_settings_ids.filament.empty()) {
                for (unsigned char i : m_viewer.get_used_extruders_ids()) {
                    ret = std::max(ret, ImGui::CalcTextSize((_u8L("Filament") + " " + std::to_string(i + 1) + ":").c_str()).x);
                }
            }
            if (ret > 0.0f)
                ret += 2.0f * ImGui::GetStyle().ItemSpacing.x;
            return ret;
        };

        ImGui::Spacing();
        imgui.title(_u8L("Settings"));

        float offset = calc_offset();

        if (!m_settings_ids.printer.empty()) {
            imgui.text(_u8L("Printer") + ":");
            ImGui::SameLine(offset);
            imgui.text(m_settings_ids.printer);
        }
        if (!m_settings_ids.print.empty()) {
            imgui.text(_u8L("Print settings") + ":");
            ImGui::SameLine(offset);
            imgui.text(m_settings_ids.print);
        }
        if (!m_settings_ids.filament.empty()) {
            for (unsigned char i : m_viewer.get_used_extruders_ids()) {
                if (i < static_cast<unsigned char>(m_settings_ids.filament.size()) && !m_settings_ids.filament[i].empty()) {
                    std::string txt = _u8L("Filament");
                    txt += (m_viewer.get_used_extruders_count() == 1) ? ":" : " " + std::to_string(i + 1);
                    imgui.text(txt);
                    ImGui::SameLine(offset);
                    imgui.text(m_settings_ids.filament[i]);
                }
            }
        }
    }
    std::vector<CustomGCode::Item> custom_gcode_per_print_z = wxGetApp().is_editor() ?
                                                                  wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes :
                                                                  m_custom_gcode_per_print_z;
    if (custom_gcode_per_print_z.size() != 0) {
        float max_len = window_padding + 2 * ImGui::GetStyle().ItemSpacing.x;
        ImGui::Spacing();
        std::string cgcode_title_str       = _u8L("Custom G-code");
        std::string cgcode_layer_str       = _u8L("Layer");
        std::string cgcode_time_str        =  _u8L("Time");
        std::string cgcode_pause_str = _u8L("Pause");
        std::string cgcode_template_str= _u8L("Template");
        std::string cgcode_toolchange_str = _u8L("Tool Change");
        std::string cgcode_custom_str = _u8L("Custom");
        std::string cgcode_unknown_str = _u8L("Unknown");

        max_len += std::max(ImGui::CalcTextSize(cgcode_title_str.c_str()).x,
                                              std::max(ImGui::CalcTextSize(cgcode_pause_str.c_str()).x,
                                                       std::max(ImGui::CalcTextSize(cgcode_template_str.c_str()).x,
                                                                std::max(ImGui::CalcTextSize(cgcode_toolchange_str.c_str()).x,
                                                                         std::max(ImGui::CalcTextSize(cgcode_custom_str.c_str()).x,
                                                                                  ImGui::CalcTextSize(cgcode_unknown_str.c_str()).x))))

        );

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
        ImGui::Dummy({window_padding, window_padding});
        ImGui::SameLine();
        imgui.title(cgcode_title_str,true);
        ImGui::SameLine(max_len);
        imgui.title(cgcode_layer_str, true);
        ImGui::SameLine(max_len*1.5);
        imgui.title(cgcode_time_str, false);

        std::vector<double> layer_zs = get_layers_zs();
        const std::vector<float> layer_times = m_viewer.get_layers_estimated_times();

        for (Slic3r::CustomGCode::Item custom_gcode : custom_gcode_per_print_z) {
            ImGui::Dummy({window_padding, window_padding});
            ImGui::SameLine();

            switch (custom_gcode.type) {
            case CustomGCode::PausePrint: imgui.text(cgcode_pause_str); break;
            case CustomGCode::Template: imgui.text(cgcode_template_str); break;
            case CustomGCode::ToolChange: imgui.text(cgcode_toolchange_str); break;
            case CustomGCode::Custom: imgui.text(cgcode_custom_str); break;
            default: imgui.text(cgcode_unknown_str); break;
            }
            ImGui::SameLine(max_len);
            char buf[64];
            int layer = find_close_layer_idx(layer_zs, custom_gcode.print_z, epsilon());
            ::sprintf(buf, "%d", layer + 1);
            imgui.text(buf);
            ImGui::SameLine(max_len * 1.5);

            float custom_gcode_time = 0;
            if (layer > 0)
            {
                for (int i = 0; i < layer; i++) {
                    custom_gcode_time += layer_times[i];
                }
            }
            imgui.text(short_time(get_time_dhms(custom_gcode_time)));

        }
    }


    ImGui::Spacing();
    std::string time_title = m_viewer.get_view_type() == libvgcode::EViewType::FeatureType ? _u8L("Total estimation") : _u8L("Time Estimation");
    auto can_show_mode_button = [this](libvgcode::ETimeMode mode) {
        std::vector<std::string> time_strs;
        for (size_t i = 0; i < m_print_statistics.modes.size(); ++i) {
            if (m_print_statistics.modes[i].time > 0.0f) {
                const std::string time_str = short_time(get_time_dhms(m_print_statistics.modes[i].time));
                const auto it = std::find(time_strs.begin(), time_strs.end(), time_str);
                if (it == time_strs.end())
                    time_strs.emplace_back(time_str);
            }
        }
        return time_strs.size() > 1;
    };

    const libvgcode::ETimeMode time_mode_id = m_viewer.get_time_mode();
    if (can_show_mode_button(time_mode_id)) {
        switch (time_mode_id)
        {
        case libvgcode::ETimeMode::Normal: { time_title += " [" + _u8L("Normal mode") + "]"; break; }
        default: { assert(false); break; }
        }
    }
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine();
    imgui.title(time_title);
    std::string total_filament_str = _u8L("Total Filament");
    std::string model_filament_str = _u8L("Model Filament");
    std::string cost_str = _u8L("Cost");
    std::string prepare_str = _u8L("Prepare time");
    std::string print_str = _u8L("Model printing time");
    std::string total_str = _u8L("Total time");
    float max_len = window_padding + 2 * ImGui::GetStyle().ItemSpacing.x;
    if (m_viewer.get_layers_count() == 0)
        max_len += ImGui::CalcTextSize(total_str.c_str()).x;
    else {
        if (m_viewer.get_view_type() == libvgcode::EViewType::FeatureType)
            max_len += std::max(ImGui::CalcTextSize(cost_str.c_str()).x,
                std::max(ImGui::CalcTextSize(print_str.c_str()).x,
                    std::max(std::max(ImGui::CalcTextSize(prepare_str.c_str()).x, ImGui::CalcTextSize(total_str.c_str()).x),
                        std::max(ImGui::CalcTextSize(total_filament_str.c_str()).x, ImGui::CalcTextSize(model_filament_str.c_str()).x))));
        else
            max_len += std::max(ImGui::CalcTextSize(print_str.c_str()).x,
                (std::max(ImGui::CalcTextSize(prepare_str.c_str()).x, ImGui::CalcTextSize(total_str.c_str()).x)));
    }
    if (m_viewer.get_view_type() == libvgcode::EViewType::FeatureType) {
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(total_filament_str + ":");
        ImGui::SameLine(max_len);
        bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
        char buf[64];
        ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", ps.total_used_filament / koef);
        imgui.text(buf);
        ImGui::SameLine();
        imgui.text("  " + format_compact_weight(ps.total_weight, imperial_units));
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(model_filament_str + ":");
        ImGui::SameLine(max_len);
        auto exlude_m = total_support_used_filament_m + total_flushed_filament_m + total_wipe_tower_used_filament_m;
        auto exlude_g = total_support_used_filament_g + total_flushed_filament_g + total_wipe_tower_used_filament_g;
        ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", ps.total_used_filament / koef - exlude_m);
        imgui.text(buf);
        ImGui::SameLine();
        imgui.text("  " + format_compact_weight(ps.total_weight - exlude_g, imperial_units));
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(cost_str + ":");
        ImGui::SameLine(max_len);
        ::sprintf(buf, "%.2f", ps.total_cost);
        imgui.text(buf);
    }
    if (time_mode.prepare_time != 0.0f) {
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(prepare_str + ":");
        ImGui::SameLine(max_len);
        imgui.text(short_time(get_time_dhms(time_mode.prepare_time)));
    }
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine();
    imgui.text(print_str + ":");
    ImGui::SameLine(max_len);
    imgui.text(short_time(get_time_dhms(time_mode.time - time_mode.prepare_time)));
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine();
    imgui.text(total_str + ":");
    ImGui::SameLine(max_len);
    imgui.text(short_time(get_time_dhms(time_mode.time)));

    auto show_mode_button = [this, &imgui, can_show_mode_button](const std::string& label, libvgcode::ETimeMode mode) {
        if (can_show_mode_button(mode)) {
            if (imgui.button(label)) {
                m_viewer.set_time_mode(mode);
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                imgui.set_requires_extra_frame();
#else
                wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
#endif
            }
        }
    };

    switch (time_mode_id) {
    case libvgcode::ETimeMode::Normal: {
        show_mode_button(_u8L("Show stealth mode"), libvgcode::ETimeMode::Stealth);
        break;
    }
    case libvgcode::ETimeMode::Stealth: {
        show_mode_button(_u8L("Show normal mode"), libvgcode::ETimeMode::Normal);
        break;
    }
    default : { assert(false); break; }
    }

    if (m_viewer.get_view_type() == libvgcode::EViewType::ColorPrint) {
        ImGui::Spacing();
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        offsets = calculate_offsets({ { _u8L("Options"), { ""}}, { _u8L("Display"), {""}} }, icon_size);
        offsets[1] = std::max(predictable_icon_pos, color_print_offsets[_u8L("Display")]);
        append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
        for (auto item : m_viewer.get_options())
            append_option_item(item, offsets);
    }
    ImGui::Dummy({ window_padding, window_padding });
    if (m_nozzle_nums > 1 && (m_viewer.get_view_type() == libvgcode::EViewType::Summary || m_viewer.get_view_type() == libvgcode::EViewType::ColorPrint))
        render_legend_color_arr_recommen(window_padding);


    legend_height = ImGui::GetCurrentWindow()->Size.y;
    imgui.end();
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(2);
}

void GCodeViewer::push_combo_style()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0,8.0));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.0f, 0.0f, 0.0f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.59f, 0.53f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
}
void GCodeViewer::pop_combo_style()
{
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(8);
}

void GCodeViewer::render_slider(int canvas_width, int canvas_height) {
    m_moves_slider->render(canvas_width, canvas_height);
    m_layers_slider->render(canvas_width, canvas_height);
}

}
}
