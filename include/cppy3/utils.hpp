#pragma once

#include <string>

namespace cppy3
{
  // Locale-independent UTF-8 <-> platform wchar_t conversion (UTF-16 on
  // Windows, UTF-32 elsewhere -- selected automatically from sizeof(wchar_t)).
  // Never consults the process locale (the previous mbstowcs/wcstombs-based
  // implementation depended on LC_CTYPE and silently mis-converted or
  // overflowed its output buffer under "C" locale or on astral codepoints).
  [[nodiscard]] std::wstring UTF8ToWide(const std::string &text);
  [[nodiscard]] std::string WideToUTF8(const std::wstring &text);
}
