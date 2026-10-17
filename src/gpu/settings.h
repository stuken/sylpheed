/**
 * @file    gpu/settings.h
 * @brief   Graphics settings.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <rex/types.h>

namespace sylpheed::gpu {

class Settings {
public:
  static Settings &Get();

  // Adopts the current cvar values, then registers a change callback per
  // setting so console, config file and launch argument writes reach here.
  // Called once from SylpheedApp::OnPostInitLogging, the first hook after
  // rex::cvar::LoadConfig has run.
  void Init();

  // Re-reads every setting from cvar storage. rex::cvar::ResetToDefault and
  // ResetAllToDefaults write the storage without firing a change callback, so
  // anything that calls them calls this after.
  void AdoptCvars();

  // 0 follows the game's sampler states; 2, 4, 8 or 16 forces that level.
  i32 Anisotropy() const { return anisotropy_; }
  bool SetAnisotropy(i32 v);

  bool Vsync() const { return vsync_; }
  bool SetVsync(bool v);

  // Restart-bound: read once when the device and its targets are built.
  bool GeometryGPUUpload() const { return geometryGPUUpload_; }
  bool DRED() const { return dred_; }
  i32 SuperSampling() const { return superSampling_; }
  bool SetSuperSampling(i32 v);

private:
  Settings() = default;
  Settings(const Settings &) = delete;
  Settings &operator=(const Settings &) = delete;

  // Each pulls its own setting from cvar storage and nothing else. AdoptCvars
  // calls every one. Each setting's registered change callback calls only its
  // own, so one setting changing never re-reads the others.
  void AdoptAnisotropy();
  void AdoptVsync();
  void AdoptGeometryGPUUpload();
  void AdoptDRED();
  void AdoptSuperSampling();

  // The cvar defaults, so a reader that runs before Init sees the same values.
  i32 anisotropy_ = 0;
  i32 superSampling_ = 1;
  bool vsync_ = true;
  bool geometryGPUUpload_ = true;
  bool dred_ = true;
};

} // namespace sylpheed::gpu
