// save_dir_probe.cpp —— 直接测产品的 LoadLastSaveDir / StoreLastSaveDir（临时，不入库）
// 为什么单独测：端到端跑下来"默认目录没跟着 lastdir 走"，但驱动自己读同一个 ini 是对的，
// 所以要么产品的读逻辑有问题，要么 ini 路径不是我以为的那个。这里把函数单独拎出来看。
#include "../src/capture.cpp"
#include <cstdio>

// capture.cpp 里这些是外部符号，探针里给空实现
const wchar_t* ShotIniPath() {
    return L"C:\\Users\\Administrator\\Desktop\\极速启\\data\\settings.ini";
}
bool CopyImageToClipboard(const CaptureImage&) { return false; }
bool ShotOcrAndCopy(const CaptureImage&) { return false; }
bool ShotCopyImageThenText(const CaptureImage&, const std::string&) { return false; }
bool OcrEnsureEngine() { return false; }
std::vector<OcrWord> OcrImage(const CaptureImage&) { return std::vector<OcrWord>(); }
std::string FormatSelection(const std::vector<OcrWord>&, NewlineMode) { return std::string(); }

int main() {
    printf("ShotUiIni() = ");
    wprintf(L"%ls\n", ShotUiIni().c_str());
    const std::wstring d = LoadLastSaveDir();
    printf("LoadLastSaveDir() 长度 = %d\n", (int)d.size());
    wprintf(L"LoadLastSaveDir() = [%ls]\n", d.c_str());
    return d.empty() ? 1 : 0;
}
