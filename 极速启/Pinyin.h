#ifndef BONNY_PINYIN_H
#define BONNY_PINYIN_H

#include <string>

namespace JisuQi {

// Returns true when 'query' (ASCII letters, e.g. L"wx") matches the pinyin
// initials of the CJK characters in 'text' (e.g. 微信 -> "wx"). ASCII
// characters in 'text' contribute their own lowercased letter.
// Uses the active IME's reverse-conversion dictionary (imm32); when no
// Chinese IME is installed the pinyin part simply never matches.
bool PinyinInitialsMatch(const std::wstring& text, const std::wstring& query);

// Launcher name matcher: case-insensitive substring match OR pinyin
// initials match. Empty query matches everything.
bool NameMatch(const std::wstring& name, const std::wstring& query);

} // namespace JisuQi

#endif // BONNY_PINYIN_H
