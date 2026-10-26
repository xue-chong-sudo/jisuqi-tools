// anno_shift_test.cpp —— Shift 约束（正圆/正方形）的确定性单测
//
// 为什么不靠运行时驱动按 Shift：产品的按键读取在消息循环里，注入按键时遮罩
// 不一定是前台焦点窗，会拿到假失败；而这条约束本身是纯函数
// ConstrainShapeEnd(anchor, cur, square)，直接传 square=true 调用最可靠。
// （产品里 square 由 GetAsyncKeyState(VK_SHIFT) 决定——拖拽过程中 GetKeyState
//   读不到当次按下，这一处已实测踩坑，详见 capture.cpp 拖拽分支的注释。）
#include "../src/capture.cpp"

const wchar_t* ShotIniPath() { return L""; }
bool CopyImageToClipboard(const CaptureImage&) { return false; }
bool ShotOcrAndCopy(const CaptureImage&) { return false; }
bool ShotCopyImageThenText(const CaptureImage&, const std::string&) { return false; }
bool OcrEnsureEngine() { return false; }
std::vector<OcrWord> OcrImage(const CaptureImage&) { return {}; }
void OcrImageAsync(const CaptureImage&, std::function<void(std::vector<OcrWord>)>) {}
bool OcrGetUnloadAfterUse() { return false; }
void OcrSetUnloadAfterUse(bool) {}
void OcrRequestUnload() {}
std::string FormatSelection(const std::vector<OcrWord>&, NewlineMode) { return {}; }

#include <cstdio>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) ++g_fail;
}

static bool Eq(POINT p, int x, int y) { return p.x == x && p.y == y; }

// 约束后的外接框必须走到正方形（宽高相等）
static bool IsSquare(POINT a, POINT b) {
    int dx = b.x - a.x, dy = b.y - a.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx == dy;
}

int main() {
    POINT a{ 100, 200 };

    // ---- 基本：取较大边 ----
    {
        POINT b = ConstrainShapeEnd(a, POINT{ 400, 240 }, true);
        Check(Eq(b, 400, 500), "右下拖 300x40 → 取 300，终点 (400,500)");
        Check(IsSquare(a, b), "约束后是正方形");
    }
    // ---- 反向：左上拖，符号要保住 ----
    {
        POINT b = ConstrainShapeEnd(a, POINT{ 60, 90 }, true);
        Check(Eq(b, -20, -90) || (b.x < a.x && b.y < a.y),
              "左上拖时终点仍在左上象限（符号没被绝对值吃掉）");
        Check(IsSquare(a, b), "反向拖也是正方形");
    }
    // ---- 混合：一正一负 ----
    {
        POINT b = ConstrainShapeEnd(a, POINT{ 40, 500 }, true);
        Check(b.x < a.x && b.y > a.y, "左下列仍是左下象限");
        Check(IsSquare(a, b), "跨象限拖也是正方形");
    }
    // ---- dx==0 时 sign 必须取 +1，不能变成 0 边长 ----
    {
        POINT b = ConstrainShapeEnd(a, POINT{ a.x, 500 }, true);
        Check(b.x == a.x + 300 && b.y == 500, "dx==0、dy=300 → (a.x+300, 500)");
        Check(IsSquare(a, b), "dx==0 也能得到正方形");
    }
    // ---- dy==0 ----
    {
        POINT b = ConstrainShapeEnd(a, POINT{ 500, a.y }, true);
        Check(b.y == a.y + 400 && b.x == 500, "dy==0、dx=400 → (500, a.y+400)");
        Check(IsSquare(a, b), "dy==0 也能得到正方形");
    }
    // ---- square=false 必须原样返回 ----
    {
        POINT b = ConstrainShapeEnd(a, POINT{ 400, 240 }, false);
        Check(Eq(b, 400, 240), "square=false 时原样返回，不改终点");
    }
    // ---- 退化：起点=终点 ----
    {
        POINT b = ConstrainShapeEnd(a, a, true);
        Check(Eq(b, a.x, a.y) || (b.x == a.x && b.y == a.y), "起终点重合时仍是 0 边长（不崩）");
    }
    // ---- 大范围不溢出 ----
    {
        POINT b = ConstrainShapeEnd(POINT{ -5000, -5000 }, POINT{ 5000, -4900 }, true);
        Check(b.x == 5000 && b.y == 5000, "跨虚拟屏负坐标时取 10000 边长");
    }

    printf("\n%s (failures=%d)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
