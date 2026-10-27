// sylpheed - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>

#include "icon.generated.h"

#ifdef _WIN32
#include <timeapi.h>
#endif

#include "platform/silent_audio.h"
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, sylpheed_null_audio);

#ifdef SYLPHEED_NATIVE_GPU
#include <rex/logging.h>

#include "core/shutdown.h"
#include "gpu/device.h"
#include "gpu/imgui_overlay_drawer.h"
#include "gpu/settings.h"
#include "gpu/settings_overlay.h"
#include "platform/guest_abort_log.h"
#endif

class SylpheedApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<SylpheedApp>(new SylpheedApp(ctx, "sylpheed",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
#ifdef SYLPHEED_NATIVE_GPU
      // Null graphics makes Runtime::Setup skip the SDK GPU. The plume
      // renderer comes up in OnPreLaunchModule instead.
      config.graphics = nullptr;
      config.gpu_plugin.clear();
#else
      config.gpu_plugin = "xenos";
#endif
      if (REXCVAR_GET(sylpheed_null_audio))
        config.audio_factory = REX_AUDIO_BACKEND(sylpheed::platform::SilentAudioSystem);
#ifdef _WIN32
      timeBeginPeriod(1);
#endif
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
      const auto base_dir = paths.config_path.parent_path();
      if (paths.game_data_root.empty()) { // Use default assets directory path if one isn't provided!
          if (std::filesystem::is_regular_file(base_dir / "assets/default.xex")) {
              paths.game_data_root = base_dir / "assets";
          }
      }
      paths.user_data_root = base_dir / "user";
      paths.cache_root = base_dir / "cache";
      paths.update_data_root = base_dir / "update";
      config_path_ = paths.config_path;
  }

  void OnPostSetup() override {
    window()->SetIcon(icon::kIconPNG, icon::kIconPNGSize);
    window()->SetTitle("Project Sylpheed");
  }

#ifdef SYLPHEED_NATIVE_GPU
  // Binds to the plume device lazily, once OnPreLaunchModule creates it. The
  // SDK owns and tears this down.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    return std::make_unique<sylpheed::gpu::ImGuiOverlayDrawer>();
  }

  // The first hook after rex::cvar::LoadConfig: the renderer's settings adopt
  // the config file and command line here and follow later cvar writes.
  // Before this they all sat at their compiled defaults.
  void OnPostInitLogging() override { sylpheed::gpu::Settings::Get().Init(); }

  void OnPreLaunchModule() override {
    sylpheed::platform::InstallGuestAbortLogger();
    // Registered here, where the UI thread is pumping: the shutdown sequence
    // has to run on it.
    sylpheed::SetShutdownDispatcher([this](std::function<void()> fn) {
      return app_context().CallInUIThread(std::move(fn));
    });
    sylpheed::SetShutdownUIPump(
        [this] { app_context().ExecutePendingFunctionsFromUIThread(); });

    if (!sylpheed::gpu::Video::CreateHostDevice(window())) {
      REXLOG_ERROR("sylpheed: native renderer init failed");
      app_context().QuitFromUIThread();
      return;
    }
    sylpheed::gpu::Video::SetOverlayDrawHook(
        [this](plume::RenderCommandList* cmd, plume::RenderFramebuffer* fb,
               u32 w, u32 h) {
          if (!imgui_drawer() || sylpheed::IsShuttingDown() ||
              !imgui_drawer()->HasDialogs())
            return;
          app_context().CallInUIThreadSynchronous([this, cmd, fb, w, h] {
            sylpheed::gpu::ReblueUIDrawContext ctx(w, h, cmd, fb);
            imgui_drawer()->Draw(ctx);
          });
        });
  }

  // F1 opens sylpheed's short settings menu; F4 still opens the SDK's full
  // one. Its Quit button is clicked inside the ImGui draw, nested in
  // Present's call to the UI thread, so the shutdown is deferred to the next
  // pump rather than run there.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    sylpheed::gpu::SettingsOverlay::InstallBind(drawer, config_path_, [this] {
      app_context().CallInUIThreadDeferred([] {
        sylpheed::RequestShutdown(sylpheed::ShutdownReason::MenuQuit);
      });
    });
  }

  void OnWindowPixelSizeChanged(u32, u32) override {
    sylpheed::gpu::Video::RequestResize();
  }

  bool OnWindowCloseRequested() override {
    sylpheed::RequestShutdown(sylpheed::ShutdownReason::WindowClose);
    return false;  // unreachable
  }
#endif

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}

 private:
  // Where the settings menus save (OnConfigurePaths).
  std::filesystem::path config_path_;
};
