#include "Pinyin.h"
#include <windows.h>
#include <imm.h>
#include <cwctype>
#include <vector>

namespace JisuQi {

namespace {

// ---------------------------------------------------------------------------
// 首字母来源 1：GB2312 一级字库边界表（主路径）。
// 一级字库（3755 个常用汉字）按拼音排序存放，因此每个拼音字母区间有一个
// 固定的 GB 码边界。查表无需任何输入法，离线/英文键盘下也 100% 生效，
// 覆盖绝大多数启动项名称（微信、需求、极速启等常用字全部命中）。
// ---------------------------------------------------------------------------

// 各字母区间的起始 GB 码（0xB0A1 起，至 0xD4D1 止为一级字库范围）。
// 注意：i/u/v 三个字母没有一级汉字，表中跳过。
struct GbRange { wchar_t letter; UINT16 code; };
const GbRange kGbBounds[] = {
    {L'a', 0xB0A1}, {L'b', 0xB0C5}, {L'c', 0xB2C1}, {L'd', 0xB4EE},
    {L'e', 0xB6EA}, {L'f', 0xB7A2}, {L'g', 0xB8C1}, {L'h', 0xB9FE},
    {L'j', 0xBBF7}, {L'k', 0xBFA6}, {L'l', 0xC0AC}, {L'm', 0xC2E8},
    {L'n', 0xC4C3}, {L'o', 0xC5B6}, {L'p', 0xC5BE}, {L'q', 0xC6DA},
    {L'r', 0xC8BB}, {L's', 0xC8F6}, {L't', 0xCBFA}, {L'w', 0xCDDA},
    {L'x', 0xCEF4}, {L'y', 0xD1B9}, {L'z', 0xD4D1},
};
const UINT16 kGbLevel1First = 0xB0A1;
const UINT16 kGbLevel1Last  = 0xD7F9; // 一级字库最后一区(55区)的结束码

// 返回小写首字母，非一级字库字符返回 0。
wchar_t GbLevel1Initial(wchar_t ch) {
    BYTE gb[2] = {0, 0};
    // 单个汉字 -> GB2312 两个字节；失败或不足 2 字节说明不在一级字库。
    int n = WideCharToMultiByte(936, 0, &ch, 1,
                                reinterpret_cast<LPSTR>(gb), 2,
                                nullptr, nullptr);
    if (n != 2) return 0;
    UINT16 code = static_cast<UINT16>((gb[0] << 8) | gb[1]);
    if (code < kGbLevel1First || code > kGbLevel1Last) return 0;
    // 线性扫边界表：落在 [bounds[i], bounds[i+1]) 区间即字母 i。
    for (int i = 0; i < static_cast<int>(std::size(kGbBounds)); ++i) {
        UINT16 lo = kGbBounds[i].code;
        UINT16 hi = (i + 1 < static_cast<int>(std::size(kGbBounds)))
                        ? kGbBounds[i + 1].code : UINT16(kGbLevel1Last + 1);
        if (code >= lo && code < hi) return kGbBounds[i].letter;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 首字母来源 2：输入法反查（兜底路径），覆盖二级字库生僻字（如"哔"）。
// 旧实现只用"当前激活"的键盘布局——只要用户切到英文键盘，反查永远失败，
// 这就是"输入 wx 搜不到微信"的根因。现在枚举系统已安装的全部中文布局，
// 逐个尝试，不再依赖当前激活状态。
// ---------------------------------------------------------------------------

// 取一个中文 IME 的 HKL；找不到返回 0。
HKL FindChineseHkl() {
    UINT n = GetKeyboardLayoutList(0, nullptr);
    if (n == 0 || n > 64) return 0;           // 上限保护，避免异常数量
    std::vector<HKL> layouts(n);
    n = GetKeyboardLayoutList(n, layouts.data());
    for (UINT i = 0; i < n; ++i) {
        WORD lang = LOWORD(layouts[i]);
        WORD prim = PRIMARYLANGID(lang);
        if (prim == 0x0004) return layouts[i]; // LANG_CHINESE
    }
    return 0;
}

// 用指定布局反查单个汉字的拼音首字母；失败返回 0。
wchar_t ImeInitial(wchar_t ch, HKL kl, HIMC imc) {
    std::wstring src(1, ch);
    LONG sz = ImmGetConversionListW(kl, imc, src.c_str(), nullptr, 0,
                                    GCL_REVERSECONVERSION);
    if (sz <= 0) return 0;
    std::vector<BYTE> buf(static_cast<size_t>(sz), 0);
    CANDIDATELIST* list = reinterpret_cast<CANDIDATELIST*>(buf.data());
    LONG sz2 = ImmGetConversionListW(kl, imc, src.c_str(), list, sz,
                                     GCL_REVERSECONVERSION);
    if (sz2 <= 0 || list->dwCount == 0) return 0;
    // 越界校验：dwOffset[0] 必须落在缓冲区内才可读取候选串。
    DWORD off = list->dwOffset[0];
    if (off >= buf.size()) return 0;
    const wchar_t* cand = reinterpret_cast<const wchar_t*>(buf.data() + off);
    // 缓冲区内逐字符确认以 '\0' 结尾，防止读到未初始化数据。
    for (size_t i = off; i < buf.size(); i += 2) {
        wchar_t c = *reinterpret_cast<const wchar_t*>(buf.data() + i);
        if (c == L'\0') break;
        if (i + 2 >= buf.size()) return 0;    // 未见到终止符 -> 放弃
    }
    if (!*cand) return 0;
    wchar_t initial = static_cast<wchar_t>(towlower(cand[0]));
    return (initial >= L'a' && initial <= L'z') ? initial : 0;
}

std::wstring LowerAlnum(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s)
        if (iswalnum(c)) out.push_back(static_cast<wchar_t>(towlower(c)));
    return out;
}

// 单个汉字的首字母：GB2312 一级字库查表优先，输入法反查兜底。
wchar_t CharInitial(wchar_t ch) {
    wchar_t r = GbLevel1Initial(ch);
    if (r) return r;
    HKL kl = FindChineseHkl();
    if (!kl) return 0;
    HIMC imc = ImmCreateContext();
    r = ImeInitial(ch, kl, imc);
    ImmDestroyContext(imc);
    return r;
}

} // namespace

bool PinyinInitialsMatch(const std::wstring& text, const std::wstring& query) {
    std::wstring q = LowerAlnum(query);
    if (q.empty()) return false;

    std::wstring initials;
    for (wchar_t ch : text) {
        if (ch >= 0x4E00 && ch <= 0x9FFF) {
            wchar_t r = CharInitial(ch);
            if (r) initials.push_back(r);
        } else if (iswalnum(ch)) {
            initials.push_back(static_cast<wchar_t>(towlower(ch)));
        }
    }

    if (initials.empty()) return false;
    return initials.find(q) != std::wstring::npos;
}

bool NameMatch(const std::wstring& name, const std::wstring& query) {
    std::wstring q = LowerAlnum(query);
    if (q.empty()) return true;
    std::wstring n = LowerAlnum(name);
    if (n.find(q) != std::wstring::npos) return true;
    return PinyinInitialsMatch(name, query);
}

} // namespace JisuQi
