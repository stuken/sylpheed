// sylpheed - ReXGlue Recompiled Project
//
// The F4 settings menu: the few options a player needs (display, frame rate,
// resolution, image quality, model detail) in plain words, over the same
// cvars and config file as the SDK's full settings dialog, which "All
// settings" still opens. Ported from pgr4.

#pragma once

#include <filesystem>
#include <memory>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class SettingsDialog;
} // namespace rex::ui

namespace sylpheed::gpu {

class SettingsOverlay final : public rex::ui::ImGuiDialog {
public:
  SettingsOverlay(rex::ui::ImGuiDrawer *imgui_drawer,
                  std::filesystem::path config_path);
  ~SettingsOverlay() override;

  // Replaces the SDK's F4 bind with one that toggles this menu. Call from
  // OnCreateDialogs, after the SDK has registered its own binds.
  static void InstallBind(rex::ui::ImGuiDrawer *imgui_drawer,
                          std::filesystem::path config_path);

protected:
  void OnDraw(ImGuiIO &io) override;

private:
  std::filesystem::path config_path_;
  std::unique_ptr<rex::ui::SettingsDialog> all_settings_;
};

} // namespace sylpheed::gpu
