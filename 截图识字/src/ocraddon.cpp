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
    if (img.width == 0) return 0; // 取消
    ShowResultAndCopy(std::move(img)); // 窗口先显示，OCR 在后台进行
    return 0;
}

int ocraddon_capture_direct(void) {
    CaptureImage img = CaptureRegionInteractive();
    if (img.width == 0) return 0; // 取消：不碰剪贴板，避免覆盖用户已有内容
    CopyImageToClipboard(img);    // 静默复制，无窗口、无提示
    return 0;
}

void ocraddon_set_newline_smart(int smart) {
    SetLineMode(smart ? NewlineMode::Smart : NewlineMode::Keep);
}

void ocraddon_set_exit_mode(int exitMode) {
    SetExitMode(exitMode);
}

void ocraddon_shutdown(void) {
    OcrShutdown();
}
