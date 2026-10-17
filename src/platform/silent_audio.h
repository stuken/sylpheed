// sylpheed - ReXGlue Recompiled Project
//
// An audio backend whose drivers accept every frame and play nothing. The
// SDK's NopAudioSystem refuses to create drivers, and so does SDL when the
// session has no output device (e.g. a disconnected Remote Desktop), and games
// commonly treat a failed XAudioRegisterRenderDriverClient as fatal.
// AudioSystem's worker already paces client callbacks by wall clock and uses
// the semaphore only as back-pressure, so handing the slot straight back is
// enough to keep the guest's audio thread running at its usual rate.

#pragma once

#include <memory>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>

namespace sylpheed::platform {

using rex::X_STATUS;

class SilentAudioDriver final : public rex::audio::AudioDriver {
 public:
  SilentAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore) {}

  void SubmitFrame(uint32_t) override {
    if (semaphore_)
      semaphore_->Release(1, nullptr);
  }
  void SetVolume(float) override {}
  void Pause() override {}
  void Resume() override {}

 private:
  rex::thread::Semaphore* semaphore_;
};

class SilentAudioSystem final : public rex::audio::AudioSystem {
 public:
  explicit SilentAudioSystem(rex::runtime::FunctionDispatcher* dispatcher)
      : AudioSystem(dispatcher) {}

  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* dispatcher) {
    return std::make_unique<SilentAudioSystem>(dispatcher);
  }

 protected:
  rex::X_STATUS CreateDriver(size_t, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override {
    *out_driver = new SilentAudioDriver(memory(), semaphore);
    return X_STATUS_SUCCESS;
  }
  void DestroyDriver(rex::audio::AudioDriver* driver) override { delete driver; }
};

} // namespace sylpheed::platform
