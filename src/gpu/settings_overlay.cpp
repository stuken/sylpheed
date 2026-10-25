// sylpheed - ReXGlue Recompiled Project

#include "gpu/settings_overlay.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/settings_overlay.h>

namespace sylpheed::gpu {

namespace {

struct Choice {
  const char *label;
  const char *value;
};

constexpr float kLabelWidth = 230.0f;
constexpr float kWidgetWidth = 150.0f;

// Toggled only by the bind. A plain pointer, so no static destructor touches
// the ImGui drawer after the app has torn it down: sylpheed ends the process
// without unwinding (core/shutdown.cpp).
SettingsOverlay *g_overlay = nullptr;

bool RequiresRestart(const rex::cvar::FlagEntry *info) {
  return info && info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart;
}

// A double cvar reads back in its own formatting ("1.000000"), so a choice
// also matches by value.
bool SameValue(const std::string &current, const char *value) {
  if (current == value)
    return true;
  char *end_a = nullptr;
  char *end_b = nullptr;
  const double a = std::strtod(current.c_str(), &end_a);
  const double b = std::strtod(value, &end_b);
  return end_a != current.c_str() && *end_a == '\0' && end_b != value &&
         *end_b == '\0' && a == b;
}

// The label column: plain words, with the cvar's own description and name on
// hover, and "(restart)" after the widget for options read only at start-up.
void Label(const char *label, const rex::cvar::FlagEntry *info) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (info && ImGui::IsItemHovered())
    ImGui::SetTooltip("%s\n[%s]", info->description.c_str(),
                      info->name.c_str());
  ImGui::SameLine(kLabelWidth);
  ImGui::SetNextItemWidth(kWidgetWidth);
}

void RestartNote(const rex::cvar::FlagEntry *info) {
  if (!RequiresRestart(info))
    return;
  ImGui::SameLine();
  ImGui::TextDisabled("(restart)");
}

void Checkbox(const char *label, const char *cvar) {
  const auto *info = rex::cvar::GetFlagInfo(cvar);
  if (!info)
    return;
  ImGui::PushID(cvar);
  Label(label, info);
  bool v = rex::cvar::GetFlagByName(cvar) == "true";
  if (ImGui::Checkbox("##v", &v))
    rex::cvar::SetFlagByName(cvar, v ? "true" : "false");
  RestartNote(info);
  ImGui::PopID();
}

// A checkbox over an integer cvar that takes `off` or `on`.
void IntCheckbox(const char *label, const char *cvar, int off, int on) {
  const auto *info = rex::cvar::GetFlagInfo(cvar);
  if (!info)
    return;
  ImGui::PushID(cvar);
  Label(label, info);
  bool v = rex::cvar::GetFlagByName(cvar) == std::to_string(on);
  if (ImGui::Checkbox("##v", &v))
    rex::cvar::SetFlagByName(cvar, std::to_string(v ? on : off));
  RestartNote(info);
  ImGui::PopID();
}

// A value outside the list (from the config or the command line) shows as
// itself until another is picked.
void Combo(const char *label, const char *cvar,
           std::initializer_list<Choice> choices) {
  const auto *info = rex::cvar::GetFlagInfo(cvar);
  if (!info)
    return;
  ImGui::PushID(cvar);
  Label(label, info);
  const std::string current = rex::cvar::GetFlagByName(cvar);
  auto it = std::find_if(choices.begin(), choices.end(), [&](const Choice &c) {
    return SameValue(current, c.value);
  });
  const char *preview = it != choices.end() ? it->label : current.c_str();
  if (ImGui::BeginCombo("##v", preview)) {
    for (const Choice &c : choices) {
      const bool selected = &c == it;
      if (ImGui::Selectable(c.label, selected))
        rex::cvar::SetFlagByName(cvar, c.value);
      if (selected)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  RestartNote(info);
  ImGui::PopID();
}

void Section(const char *title) {
  ImGui::Spacing();
  ImGui::SeparatorText(title);
}

} // namespace

SettingsOverlay::SettingsOverlay(rex::ui::ImGuiDrawer *imgui_drawer,
                                 std::filesystem::path config_path)
    : rex::ui::ImGuiDialog(imgui_drawer), config_path_(std::move(config_path)) {}

SettingsOverlay::~SettingsOverlay() = default;

void SettingsOverlay::InstallBind(rex::ui::ImGuiDrawer *imgui_drawer,
                                  std::filesystem::path config_path) {
  // A bind can't be registered twice under one name (its key cvar is
  // registered once), so the menu gets its own bind on the SDK one's key, and
  // the SDK one is emptied so the keybinds page shows no conflict.
  std::string key = rex::cvar::GetFlagByName("bind_settings");
  if (key.empty())
    key = "F4";
  rex::ui::UnregisterBind("bind_settings");
  rex::cvar::SetFlagByName("bind_settings", "");
  rex::ui::RegisterBind(
      "bind_sylpheed_settings", key, "Toggle settings menu",
      [imgui_drawer, config_path = std::move(config_path)] {
        if (g_overlay) {
          delete g_overlay;
          g_overlay = nullptr;
        } else {
          g_overlay = new SettingsOverlay(imgui_drawer, config_path);
        }
      });
}

void SettingsOverlay::OnDraw(ImGuiIO & /*io*/) {
  ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.9f);
  if (!ImGui::Begin("Settings##sylpheed", nullptr,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  Section("Display");
  Checkbox("Fullscreen", "fullscreen");

  Section("Frame rate");
  Combo("Frame rate limit", "sylpheed_fps_limit",
        {{"30", "30"},
         {"60", "60"},
         {"120", "120"},
         {"144", "144"},
         {"Unlimited", "0"}});
  Checkbox("Vertical sync", "sylpheed_vsync");

  Section("Resolution");
  // Output::ResolutionScale caps the base scale at 4.
  Combo("Resolution scale", "sylpheed_resolution_scale",
        {{"Auto (window size)", "0"},
         {"1x (720p)", "1"},
         {"2x (1440p)", "2"},
         {"3x (2160p)", "3"},
         {"4x (2880p)", "4"}});
  IntCheckbox("Supersampling (2x scale)", "sylpheed_supersampling", 1, 2);

  Section("Image quality");
  Combo("Anisotropic filtering", "sylpheed_anisotropy",
        {{"Game default", "default"},
         {"2x", "2"},
         {"4x", "4"},
         {"8x", "8"},
         {"16x", "16"}});
  Checkbox("Smooth glow upscaling", "sylpheed_scaled_post_filters");
  Checkbox("16-bit output", "sylpheed_output_16bit");
  Checkbox("Console display correction", "sylpheed_display_correction");

  Section("Detail");
  Combo("Model detail distance", "sylpheed_lod_scale",
        {{"1x (console)", "1"},
         {"1.5x", "1.5"},
         {"2x", "2"},
         {"4x", "4"},
         {"Always highest", "0"}});

  ImGui::Spacing();
  ImGui::Separator();
  if (!rex::cvar::GetPendingRestartFlags().empty())
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                       "Restart the game to apply the changes marked (restart).");
  if (ImGui::Button("Save"))
    rex::cvar::SaveConfig(config_path_);
  ImGui::SameLine();
  if (ImGui::Button(all_settings_ ? "Hide all settings" : "All settings...")) {
    if (all_settings_)
      all_settings_.reset();
    else
      all_settings_ = std::make_unique<rex::ui::SettingsDialog>(
          imgui_drawer(), config_path_);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", config_path_.filename().string().c_str());

  ImGui::End();
}

} // namespace sylpheed::gpu
