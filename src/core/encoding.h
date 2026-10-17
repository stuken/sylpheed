// sylpheed - ReXGlue Recompiled Project

#pragma once

#include <string>
#include <string_view>

namespace sylpheed {

// UTF-8 to UTF-16 for Win32 wide-character APIs. Empty on failure.
std::wstring Utf8ToWide(std::string_view utf8);
// UTF-16 to UTF-8. Empty on failure.
std::string WideToUtf8(std::wstring_view wide);

} // namespace sylpheed
