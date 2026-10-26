// ocraddon.cpp —— C 接口实现，串联：截图 -> 识别 -> 选词复制
// v2 新增：直接截图（图片进剪贴板）与换行模式开关（供极速启宿主调用）
#include "ocraddon.h"
#include "ocraddon_internal.h"
#include <windows.h>
#include <string>
#include <utility>   // std::move

int ocraddon_init(const wchar_t* tessdata_path, const wchar_t* lang) {
    return OcrInit(tessdata_path, lang) ? 0 : 1;
}

int ocraddon_capture_and_copy(void) {
    CaptureImage img = CaptureRegionInteractive();
    if (img.width == 0) {         // 取消：把预热好的引擎卸掉（否则白占 50MB）
        OcrRequestUnload();
        return 0;
    }
    // 调整态里点过"保存"/"识别文字"（或按了 ESC）：结果已经写进剪贴板，
    // **不能再弹结果窗口**，否则用户会看到"原地保存完，又冒出一个窗口"。
    // 现在这条是主路径；下面的 ShowResultAndCopy 只是兜底，实际走不到。
    if (ShotAlreadyCopied()) {
        OcrRequestUnload();
        return 0;
    }
    ShowResultAndCopy(std::move(img)); // 兜底：仍走旧的结果窗口
    // 结果窗关闭后这里才返回：此时识别早已结束、引擎也已卸载，再压一次工作集，
    // 把结果窗那几张位图占的物理内存也还回去（否则会停在 10MB 左右）。
    OcrRequestUnload();
    return 0;
}

int ocraddon_capture_direct(void) {
    CaptureImage img = CaptureRegionInteractive();
    if (img.width == 0) return 0; // 取消：不碰剪贴板，避免覆盖用户已有内容
    // 调整态退出时已写过剪贴板就别再写一次，否则 Win+V 历史里会多一条重复的图
    if (!ShotAlreadyCopied()) CopyImageToClipboard(img);
    return 0;
}

void ocraddon_set_newline_smart(int smart) {
    SetLineMode(smart ? NewlineMode::Smart : NewlineMode::Keep);
}

void ocraddon_set_exit_mode(int exitMode) {
    SetExitMode(exitMode);
}

void ocraddon_set_engine(const wchar_t* tessdata_path, const wchar_t* lang) {
    OcrSetEngineParams(tessdata_path, lang);
}

void ocraddon_prewarm(void) {
    OcrPrewarm();
}

void ocraddon_set_unload_after_use(int on) {
    OcrSetUnloadAfterUse(on != 0);
}

int ocraddon_init_failed(void) {
    return OcrInitFailed() ? 1 : 0;
}

void ocraddon_unload_if_idle(void) {
    OcrRequestUnload();
}

void ocraddon_shutdown(void) {
    OcrShutdown();
}
