// ocraddon_internal.h —— 内部头（仅插件源码使用，不暴露给宿主）
#pragma once
#include <string>
#include <vector>
#include <functional>

// 避免 Windows.h 的 min/max 宏与 std 冲突
#ifndef NOMINMAX
#define NOMINMAX
#endif

// 前向声明，避免 capture/view 也依赖 tesseract 头文件
namespace tesseract { class TessBaseAPI; }

// 单个识别出的文字块。坐标基于截图图像像素，原点左上，x 向右，y 向下
struct OcrWord {
    int x = 0, y = 0, w = 0, h = 0;
    std::string text; // UTF-8
};

// 截图结果像素格式约定（贯穿 capture / view / ocr_tesseract / test_main）：
//   - 通道顺序：RGB，每像素 3 字节，行优先（top-down，第 0 行是图像顶部）
//   - 全部生产者与消费者必须严格遵守；需要 BGRA/BGR 的地方单独转换，不在
//     这里存储。
struct CaptureImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
};

// 换行处理模式（软件“换行设置”预留接口，暂未接 UI）。
enum class NewlineMode {
    Keep,    // 保持换行：识别出的行结构原样保留（行内拼接，行间换行）
    Smart,  // 智能换行：按续行信号合并/保留（textjoin::JoinLines）—— 当前默认
    Delete, // 删除换行：去掉所有换行（= 旧行为，仅相邻英文词间补空格）
};

// 把选中词（已按阅读顺序、含坐标）格式化为最终文本。
// 这是“换行设置”的统一入口：切换 mode 即得三种处理结果。
std::string FormatSelection(const std::vector<OcrWord>& words, NewlineMode mode);

// 交互式截图：返回用户框选区域（含自动吸附窗口）。取消/空选时 width==0
CaptureImage CaptureRegionInteractive();

// 离线识别：输入 RGB 图像，输出文字块列表（已按 上->下、左->右 排序）
std::vector<OcrWord> OcrImage(const CaptureImage& img);

// 结果视图（用户确认的交互）：
//   - 无边框原图窗口（默认 1:1，居中，图大于屏幕可滚动；Ctrl+滚轮缩放）
//   - 窗口立即显示，OCR 在后台线程进行；完成前拖选只有虚线框、无高亮
//   - 平时不显示任何识别框；左键拖选命中的文字按“行”合并成连续等高色带，
//     行带内背景像素替换为 #d0d5dd，文字像素保持原图（不变形）
//   - 右键弹出菜单【复制文字 / 关闭】，左键点“复制文字”写入剪贴板
//     （复制时去除回车/空格，仅相邻词都是英文或数字时保留一个空格），
//     提示后自动关闭；ESC 也可关闭
// 内部按值接管 img（移动语义），调用方无需保证其生命周期。
void ShowResultAndCopy(CaptureImage img);

// 结果视图的“换行模式”全局开关：Smart=智能换行（默认），Keep=保留原始换行。
// 宿主模式每次触发截图前由 ocraddon_set_newline_smart 更新。
void SetLineMode(NewlineMode mode);

// 结果视图的“退出方式”全局开关：0=复制文字后直接退出（默认），1=复制后 ESC 退出。
// 宿主模式每次触发截图前由 ocraddon_set_exit_mode 更新。
void SetExitMode(int exitMode);

// 把截到的图片以 CF_DIB 写入剪贴板（RGB top-down 输入，转成 bottom-up BGR）。
// 供“直接截图”模式使用；成功返回 true。
bool CopyImageToClipboard(const CaptureImage& img);

// OCR 引擎（Tesseract 后端），生命周期由 ocr_tesseract.cpp 内部持有
bool OcrInit(const wchar_t* tessdata, const wchar_t* lang);
void OcrShutdown();

// 后台 OCR：内部先复制图像数据，在工作线程识别，完成后在工作线程上下文回调
void OcrImageAsync(const CaptureImage& img,
                   std::function<void(std::vector<OcrWord>)> onDone);

// 等待后台 OCR 结束（重新截图前 / 程序退出前调用）
void WaitAsyncOcr();
