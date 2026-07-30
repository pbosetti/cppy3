#include <cppy3/utils.hpp>
#include <iostream>

// Bug #5 (plan Part 1): utils.cpp's C++17 branch of WideToUTF8() allocates
// `text.size() * 2 + 1` bytes and then calls std::wcstombs into it. On
// Linux/macOS wchar_t is 4 bytes and holds one full codepoint per element
// (no surrogate pairs), and UTF-8 needs up to 4 bytes per codepoint above
// U+FFFF (astral / emoji characters). 3 astral codepoints need 12 UTF-8
// bytes + a NUL = 13, but the buffer sized `3*2+1 = 7` bytes -- a definite
// heap-buffer-overflow write, caught by ASan.
int main()
{
  // 3 codepoints outside the BMP (each needs 4 UTF-8 bytes): grinning face x3
  const std::wstring emoji = L"\U0001F600\U0001F600\U0001F600";

  std::cout << "bug05: converting " << emoji.size() << " wchar_t codepoints (needs "
            << emoji.size() * 4 << " UTF-8 bytes, buffer sized for " << emoji.size() * 2
            << ")" << std::endl;

  const std::string utf8 = cppy3::WideToUTF8(emoji);

  std::cout << "bug05: got " << utf8.size() << " bytes back (ASan should have already"
            << " reported a heap-buffer-overflow above if the bug is present)" << std::endl;
  return 0;
}
