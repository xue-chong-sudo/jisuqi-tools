// text_join_test.cpp —— 验证 JoinLines 的各条规则。
// 用字节级相等断言（UTF-8），不依赖控制台能否显示中文。
#include "text_join.h"
#include <cstdio>
#include <cassert>

static int g_fail = 0;

static void Check(const char* name, const std::vector<std::string>& in,
                  const std::string& expect) {
    std::string got = textjoin::JoinLines(in);
    bool ok = (got == expect);
    if (!ok) g_fail++;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        printf("  expect(%zu)=", expect.size());
        for (unsigned char c : expect) printf("\\x%02X", c);
        printf("\n  got   (%zu)=", got.size());
        for (unsigned char c : got) printf("\\x%02X", c);
        printf("\n");
    }
}

int main() {
    // 规则1：上一行末尾是续行标点“，”
    Check("rule1-cont", {"他说，", "我们去了公园。"},
          "他说，我们去了公园。");

    // 规则2：下一行开头是“，”
    Check("rule2-next-punct", {"他说", "，我们去了。"},
          "他说，我们去了。");

    // 规则3：上一行无句末标点（普通折行）
    Check("rule3-wrap", {"这是一个很长的句子需要折行", "后面的内容继续。"},
          "这是一个很长的句子需要折行后面的内容继续。");

    // 规则4：仅当“上一行”接近满行才合并。
    //   line1(满行。)+line2(短行。) => line1 近满 => 合并
    //   line2(短行。)+line3(满行。) => line2 不近满 => 保留换行
    {
        std::string s1 = "一二三四五六七八九十十一二三四五六七八九十十二三四五六七八九十十一二三四五六七八九十十二三。";
        std::string s2 = "后续的内容继续写在这里。";
        std::string s3 = "一二三四五六七八九十十一二三四五六七八九十十二三四五六七八九十十一二三四五六七八九十十二三。";
        std::vector<std::string> in = {s1, s2, s3};
        std::string expect = s1 + s2 + "\n" + s3;
        Check("rule4-nearfull", in, expect);
    }

    // 保留：空行（两个换行都保留）
    Check("keep-empty", {"第一段。", "", "第二段。"},
          "第一段。\n\n第二段。");

    // 保留：下一行缩进
    Check("keep-indent", {"他说：", "  缩进的内容"},
          "他说：\n  缩进的内容");

    // 保留：下一行项目符号
    Check("keep-bullet", {"买东西：", "• 苹果", "• 香蕉"},
          "买东西：\n• 苹果\n• 香蕉");

    // 保留：短行以句末标点结尾（L 很小，规则4 不触发）
    Check("keep-short-sent", {"好的。", "他来了。"},
          "好的。\n他来了。");

    // 英文空格：两边都 ASCII 字母/数字时补空格
    Check("space-en", {"hello", "world"},
          "hello world");
    Check("space-en-num", {"version", "2.0"},
          "version 2.0");

    // 中文不补空格（边界不是 ASCII 字母/数字）
    Check("no-space-cn-en", {"hello", "世界"},
          "hello世界");
    Check("no-space-en-cn", {"世界", "hello"},
          "世界hello");

    // 规则1 + 空行共存：上一行“，”但下一行空 => 保留（不吞空行）
    Check("cont-then-empty", {"他说，", "", "我们去了。"},
          "他说，\n\n我们去了。");

    printf("\n%s (%d failures)\n",
           g_fail == 0 ? "ALL PASS" : "HAS FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
