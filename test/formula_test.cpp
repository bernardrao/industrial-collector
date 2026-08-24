// test/formula_test.cpp — 地址公式求值器
//
// 判据不只是"算得对"，还有三件同等重要的事：
//   · 错误信息要能让现场人员改对公式（位置 + 可用变量），不是笼统的"语法错误"
//   · 溢出/除零必须报错，不能回绕成一个看似合法的地址 —— 那会让采集器去读
//     一个风马牛不相及的寄存器，而且没有任何征兆
//   · 小数必须拒绝，不能悄悄截断

#include "formula.h"

#include <cstdio>
#include <map>
#include <string>

using namespace industrial;

static int g_fail = 0;
#define CHECK(c,m) do{ if(c) printf("  ✓ %s\n",m); \
                       else {printf("  ✗ FAIL %s\n",m);++g_fail;} }while(0)

using Vars = std::map<std::string,int64_t>;

// 期望成功且等于 want
static void ok(const std::string& e, const Vars& v, int64_t want) {
    std::string err;
    auto r = evalFormula(e, v, err);
    char msg[256];
    if (!r) {
        snprintf(msg, sizeof msg, "\"%s\" 应得 %lld，却报错: %s",
                 e.c_str(), (long long)want, err.c_str());
        printf("  ✗ FAIL %s\n", msg); ++g_fail; return;
    }
    if (*r != want) {
        snprintf(msg, sizeof msg, "\"%s\" 应得 %lld，实得 %lld",
                 e.c_str(), (long long)want, (long long)*r);
        printf("  ✗ FAIL %s\n", msg); ++g_fail; return;
    }
    snprintf(msg, sizeof msg, "%-38s = %lld", e.c_str(), (long long)want);
    printf("  ✓ %s\n", msg);
}

// 期望失败，且错误信息里含 needle（确保报的是那个原因，不是碰巧别的错）
static void bad(const std::string& e, const Vars& v, const char* needle) {
    std::string err;
    auto r = evalFormula(e, v, err);
    char msg[320];
    if (r) {
        snprintf(msg, sizeof msg, "\"%s\" 应当报错，却算出了 %lld",
                 e.c_str(), (long long)*r);
        printf("  ✗ FAIL %s\n", msg); ++g_fail; return;
    }
    if (err.find(needle) == std::string::npos) {
        snprintf(msg, sizeof msg, "\"%s\" 报错原因应含 \"%s\"，实为: %s",
                 e.c_str(), needle, err.c_str());
        printf("  ✗ FAIL %s\n", msg); ++g_fail; return;
    }
    snprintf(msg, sizeof msg, "%-30s 被拒绝（%s）", e.c_str(), needle);
    printf("  ✓ %s\n", msg);
}

int main() {
    printf("══ 地址公式求值器 ══\n");

    // ── 1. 算术与优先级 ───────────────────────────────────────────────────
    printf("\n── 算术 ──\n");
    Vars none;
    ok("1", none, 1);
    ok("1+2*3", none, 7);                    // 乘优先于加
    ok("(1+2)*3", none, 9);
    ok("10-3-2", none, 5);                   // 左结合，不是 10-(3-2)=9
    ok("100/7", none, 14);                   // 整数除法
    ok("100%7", none, 2);
    ok("-5+8", none, 3);
    ok("-(3*4)", none, -12);
    ok("2*3*4", none, 24);
    ok("  8  /  2  ", none, 4);              // 空白随意
    ok("0x10", none, 16);                    // CAN ID 惯用十六进制
    ok("0x18FF50E5", none, 419385573LL);   // CAN 扩展帧 ID
    ok("0xff + 1", none, 256);

    // ── 2. 变量：储能站的真实场景 ─────────────────────────────────────────
    // 电芯地址同时取决于"包在簇内的序号"和"电芯在包内的序号"——
    // 这正是当初决定注入各级祖先序号（而不只是 sibling_id）的理由。
    printf("\n── 变量（储能站电芯地址）──\n");
    Vars cell{{"idx_CLST",2},{"idx_PACK",1},{"idx_CELL",7},{"idx",7}};
    ok("1000 + idx_PACK*128 + idx_CELL*2", cell, 1000 + 1*128 + 7*2);
    ok("3000 + idx_PACK*64 + idx_CELL", cell, 3000 + 1*64 + 7);
    ok("idx", cell, 7);                       // idx = 本级序号的别名
    ok("idx_CLST*10000 + idx_PACK*100 + idx_CELL", cell, 2*10000 + 1*100 + 7);

    // 第一个实例（全 0）应落在基址上 —— 0 基编号的直接后果，值得钉死
    Vars first{{"idx_CLST",0},{"idx_PACK",0},{"idx_CELL",0},{"idx",0}};
    ok("1000 + idx_PACK*128 + idx_CELL*2", first, 1000);

    // 24 芯一包时，末位电芯不应越进下一包的地址段
    Vars last{{"idx_PACK",0},{"idx_CELL",23},{"idx",23}};
    ok("1000 + idx_PACK*128 + idx_CELL*2", last, 1046);
    Vars nextPack{{"idx_PACK",1},{"idx_CELL",0},{"idx",0}};
    ok("1000 + idx_PACK*128 + idx_CELL*2", nextPack, 1128);
    CHECK(1046 < 1128, "24 芯（末位 1046）不与下一包基址 1128 重叠");

    // ── 3. 错误必须可诊断 ─────────────────────────────────────────────────
    printf("\n── 错误诊断 ──\n");
    bad("1+", none, "意外结束");
    bad("(1+2", none, "右括号");
    bad("1+2)", none, "多余字符");
    bad("", none, "为空");
    bad("@1", none, "无法识别");        // @ 出现在该有操作数的位置
    bad("1 @ 2", none, "多余字符");     // @ 出现在表达式结束之后
    bad("0.5", none, "小数点");                 // 不能悄悄截断成 0
    bad("1000 + idx_pack", cell, "未知变量");   // 大小写写错是常见笔误
    bad("1/0", none, "除以零");
    bad("1%0", none, "对零取模");
    bad("0x", none, "十六进制");

    // 未知变量要列出可用变量，否则用户无从下手
    {
        std::string err;
        evalFormula("idx_WRONG", cell, err);
        bool listed = err.find("idx_CELL") != std::string::npos &&
                      err.find("idx_PACK") != std::string::npos;
        if (!listed) printf("    实际信息: %s\n", err.c_str());
        CHECK(listed, "未知变量的报错里列出了可用变量名");
    }
    // 报错要指出位置（插入符）
    {
        std::string err;
        evalFormula("1000 + idx_PACK*  @", cell, err);
        CHECK(err.find('^') != std::string::npos, "报错带插入符定位到出错字符");
    }

    // ── 4. 溢出：错的公式不能回绕成合法地址 ───────────────────────────────
    printf("\n── 溢出保护 ──\n");
    Vars big{{"idx",1000000000LL}};
    bad("idx*idx*idx", big, "溢出");
    bad("9223372036854775807 + 1", none, "溢出");
    bad("99999999999999999999", none, "溢出");
    ok("2147483647 + 1", none, 2147483648LL);   // 32 位边界不该出问题

    // ── 5. 仅语法检查（前端即时校验用）────────────────────────────────────
    printf("\n── 语法检查（不校验变量）──\n");
    {
        std::string err;
        CHECK(checkFormulaSyntax("1000 + idx_PACK*128 + idx_ANYTHING", err),
              "未定义的变量在语法检查时放行（此时还不知道实例位置）");
        CHECK(!checkFormulaSyntax("1000 + (idx*2", err), "括号不配对被查出");
        CHECK(!checkFormulaSyntax("1..2", err), "非法数字被查出");
        CHECK(checkFormulaSyntax("a/b", err),
              "语法检查时 b 当 0，除零不算语法错");
    }

    printf("\n════════════════════════════════════\n");
    if (g_fail) { printf("%d 项失败 ✗\n", g_fail); return 1; }
    printf("全部通过 ✓\n");
    return 0;
}
