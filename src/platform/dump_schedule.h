// sylpheed - ReXGlue Recompiled Project
//
// The sylpheed_dump_frames schedule, shared by the native renderer's frame
// dump and the xenos plugin build's resolve dump: a comma-separated list of
// present frame numbers, or of seconds into the mission with a 'w' suffix
// ("5w" is the first present at least five seconds after the first 3D frame).
// Present counts differ between the two builds; mission time does not, so 'w'
// entries dump the same moment of a scripted run in both.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <rex/types.h>

double SylpheedMissionSeconds(); // platform/input_script.cpp

namespace sylpheed::platform {

class DumpSchedule {
public:
  explicit DumpSchedule(const std::string &text) {
    size_t pos = 0;
    while (pos < text.size()) {
      size_t end = text.find(',', pos);
      if (end == std::string::npos)
        end = text.size();
      std::string entry = text.substr(pos, end - pos);
      pos = end + 1;
      if (entry.empty())
        continue;
      if (entry.back() == 'w') {
        entry.pop_back();
        mission_.push_back({std::stod(entry), entry, false});
      } else {
        frames_.push_back(static_cast<u32>(std::stoul(entry)));
      }
    }
  }

  bool Empty() const { return frames_.empty() && mission_.empty(); }

  // The dump label for present `frame` ("1234" or "w5"), if one is due.
  std::optional<std::string> Due(u32 frame) {
    for (u32 f : frames_)
      if (f == frame)
        return std::to_string(frame);
    if (mission_.empty())
      return std::nullopt;
    const double secs = SylpheedMissionSeconds();
    if (secs < 0.0)
      return std::nullopt;
    for (auto &m : mission_) {
      if (!m.done && secs >= m.seconds) {
        m.done = true;
        return "w" + m.text;
      }
    }
    return std::nullopt;
  }

private:
  struct MissionEntry {
    double seconds;
    std::string text;
    bool done;
  };
  std::vector<u32> frames_;
  std::vector<MissionEntry> mission_;
};

} // namespace sylpheed::platform
