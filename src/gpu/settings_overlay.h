// sylpheed - ReXGlue Recompiled Project
//
// The F1 settings menu: the few options a player needs (display, frame rate,
// resolution, image quality, model detail) in plain words, over the same
// cvars and config file as the SDK's full settings dialog, which stays on its
// own F4 bind. Ported from pgr4.

#pragma once

#include <filesystem>
#include <functional>

#include <rex/ui/imgui_dialog.h>

namespace sylpheed::gpu {

class SettingsOverlay final : public rex::ui::ImGuiDialog {
public:
  SettingsOverlay(rex::ui::ImGuiDrawer *imgui_drawer,
                  std::filesystem::path config_path,
                  std::function<void()> quit);
  ~SettingsOverlay() override;

  // Registers F1 to toggle this menu; the SDK's full dialog stays on F4.
  // Call from OnCreateDialogs, after the SDK has registered its own binds.
  // `quit` runs from inside the ImGui draw, so it must defer the actual
  // shutdown (ui_drawer.h).
  static void InstallBind(rex::ui::ImGuiDrawer *imgui_drawer,
                          std::filesystem::path config_path,
                          std::function<void()> quit);

protected:
  void OnDraw(ImGuiIO &io) override;

private:
  std::filesystem::path config_path_;
  std::function<void()> quit_;
};

} // namespace sylpheed::gpu
