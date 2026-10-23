// ocraddon.h —— 对外 C 接口（合并进你的 exe 时只暴露这三个函数）
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 OCR 引擎
//   tessdata_path : 存放 chi_sim.traineddata / eng.traineddata 的目录（含 "tessdata" 或本就是 tessdata 目录）
//   lang          : 语言，如 L"chi_sim+eng"
// 返回 0 成功，非 0 失败
int ocraddon_init(const wchar_t* tessdata_path, const wchar_t* lang);

// 完整流程：弹截图界面（自动吸附/手动框选）-> 离线识别 -> 原图选词复制
// 返回 0 正常；用户取消也返回 0
int ocraddon_capture_and_copy(void);

// 直接截图：弹截图界面 -> 把截到的图片静默写入剪贴板（无 OCR、无结果窗口）
// 返回 0 正常；用户取消也返回 0
int ocraddon_capture_direct(void);

// 设置“换行模式”：smart 非 0 = 智能换行（JoinLines 合并碎行为段落，默认）；
// smart 为 0 = 保留识别出的原始换行。每次截图触发前由宿主按最新设置调用。
void ocraddon_set_newline_smart(int smart);

// 设置“退出方式”：exitMode 为 1 = 复制文字后按 ESC 退出（保持窗口打开）；
// 为 0（默认）= 复制文字后直接退出（约 1 秒自动关闭）。每次触发前由宿主调用。
void ocraddon_set_exit_mode(int exitMode);

// 释放资源
void ocraddon_shutdown(void);

#ifdef __cplusplus
}
#endif
