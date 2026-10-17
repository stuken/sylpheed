// sylpheed - ReXGlue Recompiled Project

#include "core/encoding.h"

#if defined(_WIN32)
#include "core/windows_lean.h"
#endif

namespace sylpheed {

std::wstring Utf8ToWide(std::string_view utf8) {
#if defined(_WIN32)
  if (utf8.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                    static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0)
    return {};
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      out.data(), n);
  return out;
#else
  return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string WideToUtf8(std::wstring_view wide) {
#if defined(_WIN32)
  if (wide.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                    static_cast<int>(wide.size()), nullptr, 0,
                                    nullptr, nullptr);
  if (n <= 0)
    return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                      out.data(), n, nullptr, nullptr);
  return out;
#else
  std::string out;
  for (wchar_t c : wide)
    out.push_back(static_cast<char>(c));
  return out;
#endif
}

} // namespace sylpheed
