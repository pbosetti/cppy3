#include <cppy3/utils.hpp>

#include <type_traits>

namespace cppy3
{
  namespace
  {
    constexpr char32_t kReplacementChar = 0xFFFD;

    // Decodes one UTF-8 codepoint starting at text[i], advances i past it.
    // Malformed/truncated sequences decode to U+FFFD and advance by one byte.
    char32_t decodeUtf8(const std::string &text, size_t &i)
    {
      const unsigned char b0 = static_cast<unsigned char>(text[i]);
      size_t extra;
      char32_t cp;

      if (b0 < 0x80) { cp = b0; extra = 0; }
      else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; extra = 1; }
      else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; extra = 2; }
      else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; extra = 3; }
      else { i += 1; return kReplacementChar; }

      if (i + extra >= text.size()) { i += 1; return kReplacementChar; }

      for (size_t k = 1; k <= extra; k++)
      {
        const unsigned char bk = static_cast<unsigned char>(text[i + k]);
        if ((bk & 0xC0) != 0x80) { i += 1; return kReplacementChar; }
        cp = (cp << 6) | (bk & 0x3F);
      }
      i += extra + 1;
      return cp;
    }

    // Appends the UTF-8 encoding of `cp` to `out`.
    void encodeUtf8(char32_t cp, std::string &out)
    {
      if (cp <= 0x7F)
      {
        out.push_back(static_cast<char>(cp));
      }
      else if (cp <= 0x7FF)
      {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
      else if (cp <= 0xFFFF)
      {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
      else
      {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
    }
  }

  std::wstring UTF8ToWide(const std::string &text)
  {
    std::wstring result;
    result.reserve(text.size());

    size_t i = 0;
    while (i < text.size())
    {
      const char32_t cp = decodeUtf8(text, i);

      if constexpr (sizeof(wchar_t) == 2)
      {
        // UTF-16: codepoints above the BMP need a surrogate pair.
        if (cp > 0xFFFF)
        {
          const char32_t v = cp - 0x10000;
          result.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
          result.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
          continue;
        }
      }
      result.push_back(static_cast<wchar_t>(cp));
    }
    return result;
  }

  std::string WideToUTF8(const std::wstring &text)
  {
    using UWChar = std::make_unsigned_t<wchar_t>;
    std::string result;
    result.reserve(text.size());

    size_t i = 0;
    while (i < text.size())
    {
      char32_t cp = static_cast<char32_t>(static_cast<UWChar>(text[i]));

      if constexpr (sizeof(wchar_t) == 2)
      {
        // UTF-16: combine a high/low surrogate pair into one codepoint.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size())
        {
          const char32_t lo = static_cast<char32_t>(static_cast<UWChar>(text[i + 1]));
          if (lo >= 0xDC00 && lo <= 0xDFFF)
          {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 1;
          }
        }
      }
      i += 1;
      encodeUtf8(cp, result);
    }
    return result;
  }
}
