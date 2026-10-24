// ocr_tesseract.cpp —— OCR 后端：Tesseract 5（离线，自带文字块坐标）
#include "ocraddon_internal.h"
#include <windows.h>   // WideCharToMultiByte / CP_UTF8
#include <tesseract/baseapi.h>
#include <memory>      // std::unique_ptr
#include <thread>      // 后台 OCR
#include <functional>
#include <algorithm>
#include <mutex>              // 工作线程任务槽
#include <condition_variable>

// OCR 引擎：用 unique_ptr + 自定义删除器管理生命周期，避免全局裸指针与重复
// 初始化导致的泄漏。删除器负责 End() + delete（与旧 OcrShutdown 等价）。
static auto g_ocrDeleter = [](tesseract::TessBaseAPI* p) {
    if (p) { p->End(); delete p; }
};
static std::unique_ptr<tesseract::TessBaseAPI, decltype(g_ocrDeleter)>
    g_ocr{ nullptr, g_ocrDeleter };

// 进入非 ASCII 路径分支时切到 tessdata 所在目录，退出时无条件还原工作目录，
// 即便 Init 抛异常也能还原（RAII）。
struct CwdGuard {
    std::wstring prev;
    bool active = false;
    ~CwdGuard() { if (active) SetCurrentDirectoryW(prev.c_str()); }
};

static std::string W2U8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.pop_back();
    return s;
}

// 判断字符串是否纯 ASCII（无中文/非 ASCII 字符）
static bool IsAllAscii(const wchar_t* s) {
    for (; s && *s; ++s) if (*s > 127) return false;
    return true;
}

// 解析为绝对路径（相对路径按当前目录展开）
static std::wstring AbsPath(const wchar_t* p) {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetFullPathNameW(p, MAX_PATH, buf, nullptr);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(p ? p : L"");
}

bool OcrInit(const wchar_t* tessdata, const wchar_t* lang) {
    if (g_ocr) return true; // 已初始化，幂等返回，避免重复 new 造成泄漏
    std::string lg = W2U8(lang);
    auto* api = new tesseract::TessBaseAPI();
    int r;
    if (!tessdata || !*tessdata) {
        // 空路径：使用 Tesseract 默认搜索（当前目录）
        r = api->Init(nullptr, lg.c_str());
    } else {
        std::wstring abs = AbsPath(tessdata);
        if (IsAllAscii(abs.c_str())) {
            // 纯 ASCII 路径：直接以绝对路径加载（最快，无副作用）
            std::string dp = W2U8(abs.c_str());
            r = api->Init(dp.c_str(), lg.c_str());
        } else {
            // 含中文/非 ASCII：用宽字符 API 切到该目录，再以空 datapath 让
            // Tesseract 按相对文件名（纯 ASCII 的 xxx.traineddata）读取。
            // 不依赖 8.3 短名（现代 SSD 常禁用），也不依赖任何临时目录。
            CwdGuard cwd;
            wchar_t saved[MAX_PATH] = {0};
            DWORD n = GetCurrentDirectoryW(MAX_PATH, saved);
            if (n > 0 && n < MAX_PATH) { cwd.prev = std::wstring(saved, n); }
            if (SetCurrentDirectoryW(abs.c_str())) cwd.active = true;
            r = api->Init(nullptr, lg.c_str());
            // cwd 析构时自动还原工作目录
        }
    }
    if (r != 0) { delete api; return false; }
    g_ocr.reset(api);
    return true;
}

void WaitAsyncOcr(); // 前置声明：等待后台 OCR 线程结束

void OcrShutdown() {
    WaitAsyncOcr(); // 先等后台 OCR 结束，再销毁引擎，避免线程还在用 g_ocr
    g_ocr.reset();  // 删除器会调用 End() + delete
}

// 识别前的小字号放大参数（提升 LSTM 准确率的关键，纯预处理、不换框架）。
//   - kOcrUpscale：把整图放大倍数；Tesseract LSTM 在“字符高度 ~30~50px”时最稳，
//     而截图常见 12~14px 字体只有十几像素，放大后小字识别率明显提升。
//   - kOcrPsm：截图多为 UI 稀疏文字，PSM_SPARSE_TEXT 比 PSM_AUTO 更贴合；
//     若遇到密集多列排版识别变差，把它改回 tesseract::PSM_AUTO 即可。
constexpr int kOcrUpscale = 2;
constexpr tesseract::PageSegMode kOcrPsm = tesseract::PSM_SPARSE_TEXT;

// 双线性上采样（RGB 24bpp, top-down）。无新依赖，手写即可。
static std::vector<unsigned char> UpscaleRGB(const std::vector<unsigned char>& src,
                                             int w, int h, int up) {
    if (up <= 1) return src;
    int nw = w * up, nh = h * up;
    std::vector<unsigned char> dst((size_t)nw * nh * 3);
    for (int y = 0; y < nh; y++) {
        float fy = (y + 0.5f) / up - 0.5f;
        int y0 = std::max(0, std::min(h - 1, (int)fy));
        int y1 = std::max(0, std::min(h - 1, y0 + 1));
        float ty = fy - (int)fy;
        for (int x = 0; x < nw; x++) {
            float fx = (x + 0.5f) / up - 0.5f;
            int x0 = std::max(0, std::min(w - 1, (int)fx));
            int x1 = std::max(0, std::min(w - 1, x0 + 1));
            float tx = fx - (int)fx;
            for (int c = 0; c < 3; c++) {
                int v00 = src[((size_t)y0 * w + x0) * 3 + c];
                int v01 = src[((size_t)y0 * w + x1) * 3 + c];
                int v10 = src[((size_t)y1 * w + x0) * 3 + c];
                int v11 = src[((size_t)y1 * w + x1) * 3 + c];
                int top = v00 + (int)((v01 - v00) * tx);
                int bot = v10 + (int)((v11 - v10) * tx);
                dst[((size_t)y * nw + x) * 3 + c] =
                    (unsigned char)(top + (int)((bot - top) * ty));
            }
        }
    }
    return dst;
}

std::vector<OcrWord> OcrImage(const CaptureImage& img) {
    std::vector<OcrWord> res;
    if (!g_ocr || img.rgb.empty()) return res;

    // 预处理：小字号截图整体放大以提升识别率；但**大图不再 2x**——
    // 放大后像素数按平方增长，LSTM 在数百万像素上会慢到数秒（连续截图会卡）。
    // 规则：放大后总像素不超过约 250 万，超出则降为 1x；放大倍数同时用于把词框坐标缩回原图。
    int up = kOcrUpscale;
    {
        const long long px = (long long)img.width * img.height;
        while (up > 1 && px * (long long)up * up > 2500000LL) --up;
    }
    std::vector<unsigned char> rgbCopy = UpscaleRGB(img.rgb, img.width, img.height, up);
    int ow = img.width * up, oh = img.height * up;
    g_ocr->SetImage(rgbCopy.data(), ow, oh, 3, ow * 3);
    g_ocr->SetSourceResolution(300);          // 提升小字号识别率
    g_ocr->SetPageSegMode(kOcrPsm);
    if (g_ocr->Recognize(nullptr) != 0) return res;

    tesseract::ResultIterator* ri = g_ocr->GetIterator();
    if (!ri) return res;
    const int inv = up; // 盒坐标缩回原图
    do {
        char* txt = ri->GetUTF8Text(tesseract::RIL_WORD);
        int l, t, r, b;
        if (ri->BoundingBox(tesseract::RIL_WORD, &l, &t, &r, &b) && txt && *txt) {
            OcrWord w;
            w.x = (l + inv / 2) / inv; w.y = (t + inv / 2) / inv;
            w.w = ((r - l) + inv / 2) / inv; w.h = ((b - t) + inv / 2) / inv;
            w.text = txt;
            res.push_back(std::move(w));
        }
        delete[] txt;
    } while (ri->Next(tesseract::RIL_WORD));
    delete ri;

    // 阅读顺序：按“行”分组（用中心 y 的容差，容忍同一行内 1~3px 抖动），
    // 行内再按 left 排序。避免严格按像素 y 排序把同一行拆成多行。
    const int rowTol = 8; // 像素容差，约等于半行高
    std::stable_sort(res.begin(), res.end(), [rowTol](const OcrWord& a, const OcrWord& b) {
        int ca = a.y + a.h / 2, cb = b.y + b.h / 2;
        if (std::abs(ca - cb) > rowTol) return ca < cb; // 不同行
        return a.x < b.x;                               // 同行按左到右
    });
    return res;
}

// ------------------------------------------------------------------
// 后台 OCR：常驻工作线程 + 「最新优先」任务槽。
//   - 绝不在 UI 线程上 join 上一轮 OCR（旧实现那样会让连续截图卡到上一轮识别结束）；
//   - 新任务覆盖尚未开始的旧任务；正在跑的那一轮无法中断，跑完其结果由调用方按代次丢弃；
//   - 单一引擎，工作线程串行使用，无需额外锁。
// ------------------------------------------------------------------
struct OcrJob {
    CaptureImage img;
    std::function<void(std::vector<OcrWord>)> cb;
};
static std::mutex              g_jobMu;
static std::condition_variable g_jobCv;
static std::shared_ptr<OcrJob> g_pending;
static bool                    g_stop = false;
static std::thread             g_worker;
static std::once_flag          g_workerOnce;

static void OcrWorkerLoop() {
    for (;;) {
        std::shared_ptr<OcrJob> job;
        {
            std::unique_lock<std::mutex> lk(g_jobMu);
            g_jobCv.wait(lk, [] { return g_stop || g_pending != nullptr; });
            if (g_stop && !g_pending) return;
            job = std::move(g_pending);
            g_pending.reset();
        }
        if (job) {
            std::vector<OcrWord> words = OcrImage(job->img);
            if (job->cb) job->cb(std::move(words));
        }
    }
}

void OcrImageAsync(const CaptureImage& img,
                   std::function<void(std::vector<OcrWord>)> onDone) {
    std::call_once(g_workerOnce, [] { g_worker = std::thread(OcrWorkerLoop); });
    auto job = std::make_shared<OcrJob>();
    job->img = img;               // 复制一份像素，线程生命周期独立于调用方
    job->cb  = std::move(onDone);
    {
        std::lock_guard<std::mutex> lk(g_jobMu);
        g_pending = std::move(job);   // 覆盖尚未开始的旧任务（最新优先）
    }
    g_jobCv.notify_one();
}

void WaitAsyncOcr() {
    // 仅用于程序退出：停止工作线程并等待当前任务结束。
    {
        std::lock_guard<std::mutex> lk(g_jobMu);
        g_stop = true;
        g_pending.reset();
    }
    g_jobCv.notify_all();
    if (g_worker.joinable()) g_worker.join();
}
