// text_join.h —— 把 OCR 出来的“按行拆分的碎文本”重组成段落。
// 设计原则：默认保留换行，只在明确是“续行”时才合并；拿不准就保留。
// 规则（详见 JoinLines 注释）：
//   合并条件（满足任一）：
//     1) 上一行末尾是续行标点 ，、；：——…（【《“‘
//     2) 下一行开头是 ，。！？、；：）】」』”’
//     3) 上一行无句末标点（普通折行）
//     4) 上一行以句末标点结尾，但长度接近满行（>=0.7L）=> 刚好折到句号后
//   其余一律保留（空行、缩进、编号/项目符号、短句、拿不准）。
// 合并时中文直接拼；仅当边界两字符都是 ASCII 字母/数字时补一个空格。
#pragma once
#include <string>
#include <vector>
#include <algorithm>

namespace textjoin {

inline std::string FirstCp(const std::string& s) {
    if (s.empty()) return {};
    unsigned char c = (unsigned char)s[0];
    int n = 1;
    if (c < 0x80) n = 1;
    else if ((c >> 5) == 0x6) n = 2;
    else if ((c >> 4) == 0xe) n = 3;
    else if ((c >> 3) == 0x1e) n = 4;
    if (n > (int)s.size()) n = (int)s.size();
    return s.substr(0, n);
}

inline std::string LastCp(const std::string& s) {
    if (s.empty()) return {};
    size_t i = s.size();
    // 跳过多字节字符的后续字节；停止时 s[i-1] 正是最后一个码点的首字节
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    return (i > 0) ? s.substr(i - 1) : std::string{};
}

inline int CodepointCount(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

inline bool InSet(const std::string& cp, const std::vector<std::string>& set) {
    return std::find(set.begin(), set.end(), cp) != set.end();
}

inline bool EndsWithAny(const std::string& s, const std::vector<std::string>& set) {
    if (s.empty()) return false;
    return InSet(LastCp(s), set);
}
inline bool StartsWithAny(const std::string& s, const std::vector<std::string>& set) {
    if (s.empty()) return false;
    return InSet(FirstCp(s), set);
}

inline bool IsSpaceCp(const std::string& cp) {
    if (cp.size() == 1) {
        unsigned char c = (unsigned char)cp[0];
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }
    if (cp == "\xE3\x80\x80") return true; // 全角空格 U+3000
    return false;
}

inline bool IsEmptyLine(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        int n = 1;
        if (c < 0x80) n = 1;
        else if ((c >> 5) == 0x6) n = 2;
        else if ((c >> 4) == 0xe) n = 3;
        else if ((c >> 3) == 0x1e) n = 4;
        if (!IsSpaceCp(s.substr(i, n))) return false;
        i += n;
    }
    return true;
}

inline bool HasIndent(const std::string& s) {
    if (s.empty()) return false;
    std::string f = FirstCp(s);
    return f == " " || f == "\t" || f == "\xE3\x80\x80";
}

inline bool IsAlnumAscii(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
inline bool LastAsciiAlnum(const std::string& s) {
    return !s.empty() && IsAlnumAscii((unsigned char)s.back());
}
inline bool FirstAsciiAlnum(const std::string& s) {
    return !s.empty() && IsAlnumAscii((unsigned char)s.front());
}

// 下一行是否以“编号/项目符号”开头（启发式，可按需扩展）
inline bool IsNumberedOrBullet(const std::string& s) {
    if (s.empty()) return false;
    std::string f = FirstCp(s);
    static const std::vector<std::string> bullets = {
        "\xE2\x80\xA2", "\xE2\x80\xA3", "\xE2\x97\xA6",
        "\xE2\x96\xAA", "\xE2\x96\xB8", "\xC2\xB7", "\xE2\x80\x94"
    }; // • ‣ ◦ ▪ ▸ · —
    if (InSet(f, bullets)) return true;
    // "-" / "*" 后跟空白 => 列表项
    if ((f == "-" || f == "*") && s.size() >= 2 &&
        (s[1] == ' ' || s[1] == '\t')) return true;
    // 阿拉伯数字 + 分隔符
    size_t i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
    if (i > 0 && i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.' || c == '、' || c == ')' || c == '\xEF' /* ）/】等可能，下面精确判断 */) {
            // 精确：）是 EF BC 89，] 是 5D
            if (c == ')') return true;
            if (c == '\xEF') { // 可能是 ）EF BC 89 或 】（全角）EF BC 8D
                if (s.size() > i + 2 &&
                    (unsigned char)s[i+1] == 0xBC &&
                    ((unsigned char)s[i+2] == 0x89 || (unsigned char)s[i+2] == 0x8D))
                    return true;
            }
        }
    }
    // （数字）
    if (f == "\xEF\xBC\x88") { // 全角（
        size_t j = 3;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
        if (j < s.size() && (unsigned char)s[j] == 0xEF && (unsigned char)s[j+1] == 0xBC &&
            (unsigned char)s[j+2] == 0x89) return true; // 全角）
    }
    // 中文数字 + 分隔符
    static const std::vector<std::string> cnNum = {
        "一","二","三","四","五","六","七","八","九","十","百","千","零"
    };
    size_t k = 0;
    while (k < s.size()) {
        std::string cp = FirstCp(s.substr(k));
        if (InSet(cp, cnNum)) k += cp.size(); else break;
    }
    if (k > 0 && k < s.size()) {
        std::string after = FirstCp(s.substr(k));
        if (after == "." || after == "、" || after == ")" ||
            after == "\xEF\xBC\x89") return true; // ）全角
    }
    // 单个 ASCII 字母 + 点
    if (f.size() == 1) {
        unsigned char c = (unsigned char)f[0];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            if (s.size() >= 2 && s[1] == '.') return true;
        }
    }
    return false;
}

// 主函数：输入按行拆分，输出重组后的字符串。
inline std::string JoinLines(const std::vector<std::string>& lines) {
    // 标点集合（UTF-8）
    static const std::vector<std::string> kCont = {
        "，","、","；","：","—","…","（","【","《","“","‘"
    }; // 续行标点（含破折号、省略号、开括号/开引号）
    static const std::vector<std::string> kNext = {
        "，","。","！","？","、","；","：","）","】","」","』","”","’"
    }; // 下一行开头 => 上一行没说完
    static const std::vector<std::string> kSent = {
        "。","！","？","…"
    }; // 句末标点
    constexpr int kFullLineMin = 24; // 只有中位数达到“像样的满行宽度”才启用规则4，
                                     // 否则短文档里规则4会误合并短句。

    // 估计满行宽度 L：非空行码点数的中位数
    std::vector<int> lens;
    for (const auto& l : lines) if (!IsEmptyLine(l)) lens.push_back(CodepointCount(l));
    int L = 0;
    if (!lens.empty()) {
        std::sort(lens.begin(), lens.end());
        L = lens[lens.size() / 2];
    }

    auto ShouldJoin = [&](const std::string& prev, const std::string& next) -> bool {
        // —— 最高优先级门槛：这些情形一律保留换行（规则1/2 也被它挡住）——
        if (IsEmptyLine(prev) || IsEmptyLine(next)) return false; // 空行
        if (HasIndent(next)) return false;                        // 下一行缩进
        if (IsNumberedOrBullet(next)) return false;               // 下一行编号/项目符号
        // 规则1：上一行末尾是续行标点
        if (EndsWithAny(prev, kCont)) return true;
        // 规则2：下一行开头是续行/句末标点
        if (StartsWithAny(next, kNext)) return true;
        // 规则3：上一行无句末标点（普通折行）
        if (!EndsWithAny(prev, kSent)) return true;
        // 规则4：上一行以句末标点结尾，但接近满行（仅当页面本身有满行时生效）
        if (L >= kFullLineMin && CodepointCount(prev) >= (int)(0.7 * L)) return true;
        return false;
    };

    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        out += lines[i];
        if (i + 1 < lines.size()) {
            const std::string& prev = lines[i];
            const std::string& next = lines[i + 1];
            if (ShouldJoin(prev, next)) {
                if (LastAsciiAlnum(prev) && FirstAsciiAlnum(next)) out += ' ';
                // 否则直接拼接（中文不插空格）
            } else {
                out += '\n';
            }
        }
    }
    return out;
}

} // namespace textjoin
