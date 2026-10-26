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

// 本轮截图是否已经在调整态里把结果写进剪贴板了（点过"保存"或"识别文字"）。
// capture.cpp 提供，ocraddon.cpp 用它决定是否还要弹结果窗口。
bool ShotAlreadyCopied();

// 调整态的"识别文字"：对给定选区跑 OCR，并按用户定案的顺序写剪贴板 ——
// **先写图、再写文字，两次独立的剪贴板会话**。
// 这样直接 Ctrl+V 得到的是文字，而 Win+V 历史里"图、字"两条都在。
// 合并成一次会话只会产生一条历史记录，Ctrl+V 拿到什么就由目标应用决定，
// 正好与需求相反，所以这里刻意开两次会话。
bool ShotOcrAndCopy(const CaptureImage& img);

// 「文」工具框选识别的落盘：把已经识别好的文字（UTF-8）与图片按同一次
// "先图后文、两次独立会话"的顺序写进剪贴板。语义与 ShotOcrAndCopy 完全一致，
// 区别只是文字不用再识别一遍（词已经认过、用户还可能只选了其中几个）。
bool ShotCopyImageThenText(const CaptureImage& img, const std::string& utf8);

// 极速启 data\settings.ini 的绝对路径（main.cpp 提供）。用于把“上次保存目录”
// 这类小状态放在同一个 data 目录里，不额外污染其它位置。取不到时返回空串。
const wchar_t* ShotIniPath();

// OCR 引擎（Tesseract 后端），生命周期由 ocr_tesseract.cpp 内部持有
bool OcrInit(const wchar_t* tessdata, const wchar_t* lang);
void OcrShutdown();

// —— 引擎懒加载（宿主模式）——
// 只记参数：启动时调用，不加载引擎（引擎 46~56MB，加载 100~200ms）。
void OcrSetEngineParams(const wchar_t* tessdata, const wchar_t* lang);
// 每次识别完成后是否立刻卸载引擎并把工作集还给系统。
void OcrSetUnloadAfterUse(bool on);
// 预热：把加载任务塞给后台工作线程，趁用户框选时完成。
void OcrPrewarm();
// **同步**确保引擎已加载（阻塞到加载完成，没有则返回 false）。
// OcrImage() 只是"用"引擎，它自己**不会**加载 —— 调整态点"识别文字"走的是
// 这条同步路径：那一刻可能既没预热（mode=1 直接截图模式根本不预热），
// 预热也可能还没做完，直接调 OcrImage 会拿到空结果。
bool OcrEnsureEngine();
// Init 试过且失败（tessdata 缺失等），供宿主提示。
bool OcrInitFailed();
// 取消截图（没有产生识别任务）后卸载已预热的引擎（排队执行，不打断识别）。
void OcrRequestUnload();

// 后台 OCR：内部先复制图像数据，在工作线程识别，完成后在工作线程上下文回调
void OcrImageAsync(const CaptureImage& img,
                   std::function<void(std::vector<OcrWord>)> onDone);

// 等待后台 OCR 结束（重新截图前 / 程序退出前调用）
void WaitAsyncOcr();
