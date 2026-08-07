#include "ReaderWindow.h"
#include "Ui.h"
#include "Theme.h"
#include "ArchiveExtractor.h"
#include "ComicLibraryService.h"
#include "FileUtil.h"
#include "Utf.h"
#include "ActivityLog.h"
#include "I18n.h"
#include "decoders/ImageCodecs.h"   // JPEG 硬解 + PNG 解码统一入口
#include <wincodec.h>
#include <d2d1_1.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace ark::ui {

// 缩略图自动隐藏定时器 ID（鼠标离开底部触发区 0.7s 后收回缩略图面板）
static constexpr UINT kThumbHideTimer = 1001;
// 缩略图淡入淡出动画定时器 ID（~16ms/帧，约 130ms 完成过渡）
static constexpr UINT kThumbAnimTimer = 1002;

// ---- 整页解码（后台线程调用）：JPEG 走 NVJPEG 硬解→libjpeg-turbo→WIC，其他走 WIC ----
// 输出 32bpp PBGRA 像素，直接喂给 D2D CreateBitmap（ALPHA_MODE_PREMULTIPLIED）
static bool DecodePagePixels(const std::wstring& path, std::vector<unsigned char>& px, int& w, int& h) {
    return ark::decoders::DecodeImageFull(path, px, w, h);
}

// 只读图片尺寸（用 WIC 快速读头，不解码像素）
static bool GetImageSize(const std::wstring& path, int& w, int& h) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, (void**)&wic)))
        return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    bool ok = false;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame))) {
        UINT uw = 0, uh = 0;
        frame->GetSize(&uw, &uh);
        w = (int)uw; h = (int)uh;
        ok = (uw > 0 && uh > 0);
    }
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (wic) wic->Release();
    return ok;
}

// 缩略图目标尺寸上限（与显示框 80×110 对齐，等比缩放后 ≤ 此尺寸）
static constexpr int kThumbW = 80, kThumbH = 110;

// 解码并一步缩到缩略图最终尺寸
// JPEG 走 turbojpeg 1/2^n DCT 降采样（极快，不解码全图），PNG/其他走 WIC BitmapScaler Fant
// 输出 32bpp PBGRA 像素；w/h 为实际像素尺寸（≤ kThumbW × kThumbH）
static bool DecodeThumbPixels(const std::wstring& path, std::vector<unsigned char>& px, int& w, int& h) {
    return ark::decoders::DecodeImageThumb(path, kThumbW, kThumbH, px, w, h);
}

ReaderWindow::ReaderWindow() : WindowBase(1300, 900, i18n::Tr(L"漫画阅读器", L"Comic Reader"), true) {
    SetResizable(true);
    SetMinSize(700, 500);
    SetTitleButtons(true, true);
}

ReaderWindow::~ReaderWindow() {
    decStop_ = true;
    decCv_.notify_all();
    if (decThread_.joinable()) decThread_.join();
    thumbGenStop_ = true;
    if (thumbThread_.joinable()) thumbThread_.join();
    for (auto& kv : bitmaps_) if (kv.second) kv.second->Release();
    bitmaps_.clear();
    for (auto& kv : thumbBmps_) if (kv.second) kv.second->Release();
    thumbBmps_.clear();
}

void ReaderWindow::OpenComic(ComicLibraryService* service, const ComicModel& comic) {
    service_ = service;
    comic_ = comic;
    SetTitle(W(comic_.title));
    // 性能遥测：记录当前打开漫画标题（快照用）
    {
        std::lock_guard<std::mutex> lk(PerfState::Mutex());
        PerfState::Current().comicTitle = W(comic_.title);
    }
    ActivityLog::Instance().Log(L"加载", L"打开漫画: " + W(comic_.title));
    loading_ = true;
    loaded_ = false;
    decThread_ = std::thread([this] { WorkerLoop(); });
    thumbThread_ = std::thread([this] { ThumbGenLoop(); });  // 顺序生成所有页缩略图
}

// ---- 后台线程：先提页列表+尺寸，再进入解码循环 ----
void ReaderWindow::WorkerLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ActivityLog::SetThreadName("decode");  // 线程角色名（JSONL thread_name 字段）
    // 阶段1：提页 + 尺寸
    {
        PerfScope ps(L"解码", "LoadPages");  // 性能遥测：提页列表+尺寸耗时
        std::vector<std::string> pages = ArchiveExtractor::GetPages(comic_.filePath);
        std::vector<int> pw, ph;
        pw.reserve(pages.size()); ph.reserve(pages.size());
        for (auto& p : pages) {
            int w = 0, h = 0;
            if (!GetImageSize(W(p), w, h)) { w = 0; h = 0; }
            pw.push_back(w); ph.push_back(h);
        }
        {
            std::lock_guard<std::mutex> lk(decMutex_);
            pages_ = std::move(pages);
            pw_ = std::move(pw);
            ph_ = std::move(ph);
            current_ = comic_.currentPage;
            if ((int)pages_.size() > 0) current_ = std::clamp(current_, 0, (int)pages_.size() - 1);
            else current_ = 0;
            loaded_ = true;
            loading_ = false;
        }
        ps.SetExtra({Perf::N("pages", (double)pages_.size())});
        // 性能遥测：快照记录总页数
        {
            std::lock_guard<std::mutex> lk(PerfState::Mutex());
            PerfState::Current().totalPages = (int)pages_.size();
            PerfState::Current().currentPage = current_;
        }
        ActivityLog::Instance().Log(L"加载", L"共 " + std::to_wstring(pages_.size()) + L" 页");
        UpdateDecodeSet();
        Invalidate();
    }
    // 阶段2：解码循环
    std::vector<int> local;
    while (!decStop_) {
        {
            std::unique_lock<std::mutex> lk(decMutex_);
            decCv_.wait(lk, [this] { return decStop_ || !decQueue_.empty(); });
            if (decStop_) break;
            local.swap(decQueue_);
        }
        for (int idx : local) {
            if (decStop_ || idx < 0 || idx >= (int)pages_.size()) continue;
            std::vector<unsigned char> px;
            int w = 0, h = 0;
            // 提取文件名用于状态显示（取 pages_ 路径的 basename）
            std::wstring fname = W(pages_[idx]);
            size_t pos = fname.find_last_of(L"\\/");
            if (pos != std::wstring::npos) fname = fname.substr(pos + 1);
            {
                std::lock_guard<std::mutex> lk(actMutex_);
                decodeActivity_ = i18n::Tr(L"预解码:开始: ", L"Predecode: start: ") + fname;
            }
            Invalidate();
            // 性能遥测：单页解码耗时（超阈值自动记为 stall 带调用栈）
            {
                PerfScope ps(L"解码", "DecodePage");
                std::wstring path = W(pages_[idx]);
                ps.SetExtra({Perf::S("file", ActivityFmt::NarrowUtf8(fname))});
                if (!DecodePagePixels(path, px, w, h)) {
                    ActivityLog::Instance().Log(L"解码", L"失败: " + fname);
                    std::lock_guard<std::mutex> lk(actMutex_);
                    decodeActivity_ = i18n::Tr(L"预解码:失败: ", L"Predecode: failed: ") + fname;
                    continue;
                }
            }
            std::lock_guard<std::mutex> lk(decMutex_);
            decoded_[idx] = Decoded{ std::move(px), w, h, ++tick_ };
            // 超出上限淘汰最久未用
            long long total = 0;
            for (auto& kv : decoded_) total += (long long)kv.second.w * kv.second.h;
            int evicted = 0;
            while (total > kMaxDecodedPixels && decoded_.size() > 2) {
                auto it = std::min_element(decoded_.begin(), decoded_.end(),
                                           [](const auto& a, const auto& b) { return a.second.last < b.second.last; });
                total -= (long long)it->second.w * it->second.h;
                decoded_.erase(it);
                evicted++;
            }
            if (evicted > 0) {
                ActivityLog::Instance().Log(L"缓存", L"淘汰 " + std::to_wstring(evicted) + L" 条");
                ActivityLog::Instance().LogTimed(L"缓存", "Evict", (double)evicted,
                    {Perf::N("evicted", (double)evicted), Perf::N("cache_entries", (double)decoded_.size())});
            }
            // 性能遥测：快照记录缓存条目数与内存估算
            {
                std::lock_guard<std::mutex> lk2(PerfState::Mutex());
                PerfState::Current().cacheEntries = decoded_.size();
                PerfState::Current().cacheMemMB = (double)total / (1024.0 * 1024.0);
            }
            {
                std::lock_guard<std::mutex> lk2(actMutex_);
                decodeActivity_ = i18n::Tr(L"预解码:完成: ", L"Predecode: done: ") + fname + L" " + std::to_wstring(w) + L"x" + std::to_wstring(h);
            }
        }
        local.clear();
        Invalidate();
    }
}

// 请求解码当前视图所需页（非满载时入队）
void ReaderWindow::UpdateDecodeSet() {
    if (!loaded_) return;
    std::lock_guard<std::mutex> lk(decMutex_);
    decQueue_.clear();
    // 构造目标集合：单/双页 = 当前附近；下拉 = 可视区页
    std::vector<int> want;
    if (mode_ == ReaderMode::Vertical) {
        int viewH = ViewBottom() - ViewTop();
        float y = -verticalScroll_;
        for (size_t i = 0; i < pages_.size(); i++) {
            int pw = pw_[i] > 0 ? pw_[i] : 800, ph = ph_[i] > 0 ? ph_[i] : 1200;
            float pageW = (float)(Width() - 40) < 1 ? 800 : (float)(Width() - 40);
            float pageH = pageW * ph / pw;
            if (y + pageH >= 0 && y <= viewH) { want.push_back((int)i); if ((int)want.size() > 8) break; }
            y += pageH + 12;
        }
    } else {
        want.push_back(current_);
        if (current_ + 1 < (int)pages_.size()) want.push_back(current_ + 1);
        if (current_ > 0) want.push_back(current_ - 1);
        if (current_ + 2 < (int)pages_.size()) want.push_back(current_ + 2);
    }
    int hit = 0, miss = 0;
    for (int idx : want) {
        if (decoded_.find(idx) == decoded_.end()) { decQueue_.push_back(idx); miss++; }
        else hit++;
    }
    if (miss > 0) {
        // 性能遥测：预解码命中/未命中
        ActivityLog::Instance().LogTimed(L"缓存", "PreDecode", 0,
            {Perf::N("hit", (double)hit), Perf::N("miss", (double)miss),
             Perf::N("hit_rate", (hit + miss) > 0 ? (double)hit / (double)(hit + miss) : 0)});
    }
    decCv_.notify_one();
}

// 从解码缓冲创建/复用渲染位图（绑定当前渲染目标）
ID2D1Bitmap* ReaderWindow::BitmapFor(int index, ID2D1RenderTarget* rt) {
    if (!rt || index < 0) return nullptr;
    std::lock_guard<std::mutex> lk(decMutex_);
    auto it = decoded_.find(index);
    if (it == decoded_.end()) return nullptr;
    auto& d = it->second;
    if (d.w <= 0 || d.h <= 0 || d.px.empty()) return nullptr;
    if (bitmapRt_ != rt) { // 渲染目标重建，作废位图
        for (auto& kv : bitmaps_) if (kv.second) kv.second->Release();
        bitmaps_.clear();
        bitmapRt_ = rt;
    }
    auto bit = bitmaps_.find(index);
    if (bit != bitmaps_.end()) return bit->second;
    ID2D1Bitmap* bmp = nullptr;
    rt->CreateBitmap(D2D1::SizeU((UINT)d.w, (UINT)d.h),
                     d.px.data(), (UINT32)d.w * 4,
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                     &bmp);
    if (bmp) bitmaps_[index] = bmp;
    return bmp;
}

void ReaderWindow::DrawImageIn(ID2D1RenderTarget* rt, int index, const D2D1_RECT_F& dst) {
    ID2D1Bitmap* bmp = BitmapFor(index, rt);
    if (bmp)
        rt->DrawBitmap(bmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    else
        D2D::RoundedRect(rt, dst, 0, D2D1::ColorF(0x1A, 0x1D, 0x24));
}

// 缩略图生成线程：等原图页列表就绪后，顺序生成所有页的缩略图（一步缩到 80×110 等比）
void ReaderWindow::ThumbGenLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ActivityLog::SetThreadName("thumb");  // 线程角色名（JSONL thread_name 字段）
    // 等待页列表就绪（与 WorkerLoop 阶段1 同步）
    while (!thumbGenStop_ && !loaded_) std::this_thread::sleep_for(std::chrono::milliseconds(30));
    if (thumbGenStop_) return;
    // 顺序生成所有页缩略图；当前页附近优先（让用户能更快看到首屏缩略图）
    std::vector<int> order;
    {
        std::lock_guard<std::mutex> lk(decMutex_);
        int total = (int)pages_.size();
        order.reserve(total);
        if (total > 0) {
            order.push_back(current_);
            for (int d = 1; d < total; d++) {
                if (current_ - d >= 0) order.push_back(current_ - d);
                if (current_ + d < total) order.push_back(current_ + d);
            }
        }
    }
    for (int idx : order) {
        if (thumbGenStop_) break;
        std::vector<unsigned char> px;
        int w = 0, h = 0;
        std::wstring path;
        {
            std::lock_guard<std::mutex> lk(decMutex_);
            if (idx < 0 || idx >= (int)pages_.size()) continue;
            path = W(pages_[idx]);
        }
        if (!DecodeThumbPixels(path, px, w, h)) continue;
        {
            std::lock_guard<std::mutex> lk(thumbMutex_);
            thumbCache_[idx] = ThumbImg{ std::move(px), w, h };
        }
        Invalidate();  // 每生成一张就触发重绘，让用户看到渐进填充
    }
}

// 从缩略图缓存创建/复用渲染位图（绑定当前渲染目标）
ID2D1Bitmap* ReaderWindow::ThumbBitmapFor(int index, ID2D1RenderTarget* rt, int& w, int& h) {
    w = 0; h = 0;
    if (!rt || index < 0) return nullptr;
    std::lock_guard<std::mutex> lk(thumbMutex_);
    auto it = thumbCache_.find(index);
    if (it == thumbCache_.end() || it->second.w <= 0 || it->second.px.empty()) return nullptr;
    w = it->second.w; h = it->second.h;
    // 渲染目标重建，作废所有缩略图位图
    if (thumbBmpRt_ != rt) {
        for (auto& kv : thumbBmps_) if (kv.second) kv.second->Release();
        thumbBmps_.clear();
        thumbBmpRt_ = rt;
    }
    auto bit = thumbBmps_.find(index);
    if (bit != thumbBmps_.end()) return bit->second;
    ID2D1Bitmap* bmp = nullptr;
    auto& d = it->second;
    rt->CreateBitmap(D2D1::SizeU((UINT)d.w, (UINT)d.h),
                     d.px.data(), (UINT32)d.w * 4,
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                     &bmp);
    if (bmp) thumbBmps_[index] = bmp;
    return bmp;
}

// ==================== 绘制 ====================
void ReaderWindow::OnPaint(ID2D1RenderTarget* rt, int w, int h) {
    // 内容区背景（从工具栏下方开始，避免覆盖标题栏上的最小化/最大化/关闭按钮）
    // 标题栏及三颗按钮由 WindowBase::Paint 绘制，与 MainWindow 保持一致
    D2D1_RECT_F full{ 0, (float)ToolbarTop(), (float)w, (float)h };
    D2D::RoundedRect(rt, full, 0, theme::BgMain());
    // 加载完成后首帧：让缩略图当前页居中（主线程安全执行）
    if (loaded_ && !thumbInited_) { thumbInited_ = true; CenterThumbOnCurrent(); }
    ComputeRects();
    DrawToolbar(rt);
    DrawView(rt);
    if (thumbsVisible_) DrawThumbnails(rt);
    DrawProgressBar(rt);
    if (mode_ != ReaderMode::Vertical) DrawFloatButtons(rt);
    // 全页缩略图侧边栏最后绘制，覆盖视图区（含缩略图栏），但不覆盖上下工具栏
    if (gridOpen_) DrawGridPanel(rt);
}

void ReaderWindow::DrawToolbar(ID2D1RenderTarget* rt) {
    int top = ToolbarTop(), bot = ToolbarBottom();
    D2D1_RECT_F bg{ 0, (float)top, (float)Width(), (float)bot };
    // 工具栏背景与漫画仓库 MainWindow 顶部 header 统一：BgSidebar #18191c
    D2D::RoundedRect(rt, bg, 0, theme::BgSidebar());
    D2D1_RECT_F line{ 0, (float)bot - 1, (float)Width(), (float)bot };
    D2D::RoundedRect(rt, line, 0, theme::BorderColor());

    auto drawBtn = [&](const D2D1_RECT_F& r, const std::wstring& label, bool active, bool enabled,
                       int hoverId, bool isIcon, const wchar_t* icon) {
        if (!enabled) { D2D::Text(rt, label, r, theme::TextSecondary(), 12, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_TEXT_ALIGNMENT_CENTER); return; }
        bool hv = hoverBtn_ == hoverId;
        D2D1_COLOR_F fill = active ? theme::AccentCyan() : (hv ? theme::BgCardHover() : D2D1::ColorF(0, 0, 0, 0));
        D2D::RoundedRect(rt, r, 6, fill);
        D2D1_COLOR_F col = active ? theme::AccentText() : (hv ? theme::TextPrimary() : theme::TextSecondary());
        if (isIcon) D2D::Icon(rt, icon, r, col, 12);
        else D2D::Text(rt, label, r, col, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    };

    // 单页/双页切换（开关样式）
    D2D1_COLOR_F fill = mode_ == ReaderMode::Single ? theme::AccentCyan() : theme::BgCardHover();
    D2D::RoundedRect(rt, btnSingle_, 6, fill);
    D2D::Icon(rt, mode_ == ReaderMode::Single ? L"\xE70B" : L"\xE70C", btnSingle_,
              mode_ == ReaderMode::Single ? theme::AccentText() : theme::TextSecondary(), 12);
    // 下拉模式
    drawBtn(btnDrop_, i18n::Tr(L"下拉模式", L"Vertical"), mode_ == ReaderMode::Vertical, true, kBtnDrop, false, nullptr);
    // 缩略图
    drawBtn(btnThumb_, i18n::Tr(L"缩略图", L"Thumbnails"), showThumbs_, true, kBtnThumb, false, nullptr);
    // 分隔线
    D2D1_RECT_F sep{ btnThumb_.right + 8, (float)ToolbarTop() + 12, btnThumb_.right + 9, (float)ToolbarBottom() - 12 };
    D2D::RoundedRect(rt, sep, 0, theme::BorderColor());

    // 图片大小（仅单页可用）
    bool single = mode_ == ReaderMode::Single;
    D2D1_RECT_F labelR{ sep.right + 8, (float)ToolbarTop(), sep.right + 62, (float)ToolbarBottom() };
    D2D::Text(rt, i18n::Tr(L"图片大小", L"Image size"), labelR, theme::TextSecondary(), 11);
    // 滑条轨道
    D2D::RoundedRect(rt, toolSliderRect_, 2, theme::BorderColor());
    // 滑条填充
    float ratio = (scale_ - 1.0f) / 4.0f;
    D2D1_RECT_F fillR{ toolSliderRect_.left, toolSliderRect_.top,
                       toolSliderRect_.left + (toolSliderRect_.right - toolSliderRect_.left) * ratio, toolSliderRect_.bottom };
    if (single) D2D::RoundedRect(rt, fillR, 2, theme::AccentCyan());
    // 滑块
    D2D1_RECT_F thumb{ toolSliderRect_.left + (toolSliderRect_.right - toolSliderRect_.left) * ratio - 4,
                      toolSliderRect_.top - 3, toolSliderRect_.left + (toolSliderRect_.right - toolSliderRect_.left) * ratio + 4,
                      toolSliderRect_.bottom + 3 };
    D2D::RoundedRect(rt, thumb, 5, single ? theme::TextPrimary() : theme::TextSecondary());
    // 百分比
    wchar_t pct[16];
    swprintf_s(pct, L"%d%%", (int)lround(scale_ * 100.0f));
    D2D::Text(rt, pct, percentRect_, theme::TextSecondary(), 11);

    // 滚轮放大 / 适应高度
    drawBtn(btnWheel_, i18n::Tr(L"滚轮放大", L"Wheel zoom"), wheelZoom_, single, kBtnWheel, false, nullptr);
    drawBtn(btnFit_, i18n::Tr(L"适应高度", L"Fit height"), false, single, kBtnFit, false, nullptr);

    // 网格总览按钮（最右侧，自绘 4 格不等大网格图标，点击展开全页缩略图侧边栏）
    {
        bool hv = hoverBtn_ == kBtnGrid;
        D2D1_COLOR_F fill = gridOpen_ ? theme::AccentCyan() : (hv ? theme::BgCardHover() : D2D1::ColorF(0, 0, 0, 0));
        D2D::RoundedRect(rt, btnGrid_, 6, fill);
        D2D1_COLOR_F col = gridOpen_ ? theme::AccentText() : (hv ? theme::TextPrimary() : theme::TextSecondary());
        // 图标：4 个不等大矩形边框（SVG 样式），映射 48×48 视框到按钮中央 18×18
        float s = 0.5f;
        float ix = (btnGrid_.left + btnGrid_.right) / 2.0f - 9.0f;
        float iy = (btnGrid_.top + btnGrid_.bottom) / 2.0f - 9.0f;
        auto cell = [&](float x0, float y0, float x1, float y1) {
            D2D1_RECT_F r{ ix + (x0 - 6) * s, iy + (y0 - 6) * s, ix + (x1 - 6) * s, iy + (y1 - 6) * s };
            D2D::RoundedRect(rt, r, 2, D2D1::ColorF(0, 0, 0, 0), col, 1.5f);
        };
        cell(6, 6, 20, 17);    // 左上（小）
        cell(6, 25, 20, 42);   // 左下（大）
        cell(28, 6, 42, 23);   // 右上（大）
        cell(28, 31, 42, 42);  // 右下（小）
    }
}

void ReaderWindow::DrawView(ID2D1RenderTarget* rt) {
    if (loading_) {
        D2D1_RECT_F full{ 0, (float)ViewTop(), (float)Width(), (float)ViewBottom() };
        D2D::RoundedRect(rt, full, 0, D2D1::ColorF(0, 0, 0, 0.8f));
        D2D::Icon(rt, L"\xE712",
                  D2D1::RectF(Width() / 2.0f - 16, (ViewTop() + ViewBottom()) / 2.0f - 40,
                              Width() / 2.0f + 16, (ViewTop() + ViewBottom()) / 2.0f - 8),
                  theme::AccentCyan(), 32);
        D2D::Text(rt, i18n::Tr(L"正在打开漫画...", L"Opening comic..."),
                  D2D1::RectF(0, (ViewTop() + ViewBottom()) / 2.0f, Width(), (ViewTop() + ViewBottom()) / 2.0f + 24),
                  theme::TextSecondary(), 12);
        return;
    }
    if (!loaded_ || pages_.empty()) {
        D2D::Icon(rt, L"\xE8B9",
                  D2D1::RectF(Width() / 2.0f - 28, (ViewTop() + ViewBottom()) / 2.0f - 60,
                              Width() / 2.0f + 28, (ViewTop() + ViewBottom()) / 2.0f - 4),
                  D2D1::ColorF(0x3A, 0x40, 0x50), 56);
        D2D::Text(rt, W(comic_.title),
                  D2D1::RectF(0, (ViewTop() + ViewBottom()) / 2.0f, Width(), (ViewTop() + ViewBottom()) / 2.0f + 24),
                  theme::TextSecondary(), 13);
        D2D::Text(rt, i18n::Tr(L"请从管理器中右键漫画 → 打开阅读", L"Right-click a comic in the manager → Open Reader"),
                  D2D1::RectF(0, (ViewTop() + ViewBottom()) / 2.0f + 26, Width(), (ViewTop() + ViewBottom()) / 2.0f + 44),
                  D2D1::ColorF(0x4A, 0x55, 0x68), 11);
        return;
    }
    // 裁剪到视图区：放大/平移时图片不溢出压住工具栏与底部进度条
    D2D1_RECT_F clip{ 0, (float)ViewTop(), (float)Width(), (float)ViewBottom() };
    rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (mode_ == ReaderMode::Single) DrawSinglePage(rt);
    else if (mode_ == ReaderMode::Double) DrawDoublePage(rt);
    else DrawVertical(rt);
    rt->PopAxisAlignedClip();
}

void ReaderWindow::DrawSinglePage(ID2D1RenderTarget* rt) {
    int vw = Width(), vh = ViewBottom() - ViewTop();
    int pw = pw_[current_] > 0 ? pw_[current_] : 800, ph = ph_[current_] > 0 ? ph_[current_] : 1200;
    float base = std::min((float)vw / pw, (float)vh / ph);
    float dw = pw * base * scale_, dh = ph * base * scale_;
    float cx = vw / 2.0f + panX_, cy = (ViewTop() + ViewBottom()) / 2.0f + panY_;
    D2D1_RECT_F dst{ cx - dw / 2, cy - dh / 2, cx + dw / 2, cy + dh / 2 };
    DrawImageIn(rt, current_, dst);
}

void ReaderWindow::DrawDoublePage(ID2D1RenderTarget* rt) {
    int vw = Width(), vh = ViewBottom() - ViewTop();
    int left = current_, right = current_ + 1;
    if (rtl_) std::swap(left, right); // RTL：左=下一页，右=当前
    auto dims = [&](int idx, int& w, int& h) {
        if (idx < 0 || idx >= (int)pages_.size()) { w = 0; h = 0; return; }
        w = pw_[idx] > 0 ? pw_[idx] : 800; h = ph_[idx] > 0 ? ph_[idx] : 1200;
    };
    int lw, lh, rw, rh;
    dims(left, lw, lh); dims(right, rw, rh);
    // 按高度适配，总宽 = 两页 + 间距
    float scale = (float)vh / std::max({ (float)lh, (float)rh, 1.0f });
    if (left < 0) scale = (float)vh / std::max((float)rh, 1.0f);
    if (right >= (int)pages_.size()) scale = (float)vh / std::max((float)lh, 1.0f);
    float dw1 = lw > 0 ? lw * scale : 0, dh1 = lh * scale;
    float dw2 = rw > 0 ? rw * scale : 0, dh2 = rh * scale;
    float totalW = dw1 + dw2 + 4;
    float x0 = (vw - totalW) / 2.0f;
    float y = (ViewTop() + ViewBottom()) / 2.0f;
    if (left >= 0) DrawImageIn(rt, left, D2D1::RectF(x0, y - dh1 / 2, x0 + dw1, y + dh1 / 2));
    if (right >= 0) DrawImageIn(rt, right, D2D1::RectF(x0 + dw1 + 4, y - dh2 / 2, x0 + dw1 + 4 + dw2, y + dh2 / 2));
}

void ReaderWindow::DrawVertical(ID2D1RenderTarget* rt) {
    int vw = Width(), vh = ViewBottom() - ViewTop();
    float y = ViewTop() - verticalScroll_;
    for (size_t i = 0; i < pages_.size() && y < ViewBottom(); i++) {
        int pw = pw_[i] > 0 ? pw_[i] : 800, ph = ph_[i] > 0 ? ph_[i] : 1200;
        float pageW = (float)(vw - 40);
        float pageH = pageW * ph / pw;
        if (y + pageH >= ViewTop()) {
            float x = (vw - pageW) / 2.0f;
            DrawImageIn(rt, (int)i, D2D1::RectF(x, y, x + pageW, y + pageH));
        }
        y += pageH + 12;
    }
    // 右侧竖向滚动条（轨道 + 滑块）
    LayoutVScroll();
    int maxS = VerticalMaxScroll();
    if (maxS > 0) {
        D2D::RoundedRect(rt, vTrack_, 4, D2D1::ColorF(1, 1, 1, 0.06f));
        D2D1_COLOR_F tc = hoverVScroll_ ? theme::TextPrimary() : theme::TextSecondary();
        D2D::RoundedRect(rt, vThumb_, 4, tc);
    }
    (void)vh;
}

void ReaderWindow::DrawThumbnails(ID2D1RenderTarget* rt) {
    if (thumbAlpha_ <= 0.0f) return;
    // 透明度辅助：把颜色 alpha 乘以当前面板透明度
    auto A = [&](D2D1_COLOR_F c)->D2D1_COLOR_F { c.a *= thumbAlpha_; return c; };
    D2D1_RECT_F bg{ 0, (float)ThumbTop(), (float)Width(), (float)ThumbBottom() };
    D2D::RoundedRect(rt, bg, 0, A(D2D1::ColorF(0x0A0C10u, 0.72f)));  // 深色半透明底
    D2D1_RECT_F l1{ 0, (float)ThumbTop(), (float)Width(), (float)ThumbTop() + 1 };
    D2D::RoundedRect(rt, l1, 0, A(theme::BorderColor()));

    // 缩略图列表：裁剪到面板内，超出左右边界不绘制
    D2D1_RECT_F clip{ 0, (float)ThumbTop(), (float)Width(), (float)ThumbBottom() };
    rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < thumbRects_.size(); i++) {
        const auto& r = thumbRects_[i];
        if (r.right < 0 || r.left > Width()) continue;
        D2D1_RECT_F box = r;
        // 非当前页画灰色边框；当前页由中央绿色框圈住，不另画边框
        if ((int)i != current_)
            D2D::RoundedRect(rt, D2D1::RectF(box.left - 1, box.top - 1, box.right + 1, box.bottom + 1),
                             3, D2D1::ColorF(0, 0, 0, 0), A(theme::BorderColor()), 1.0f);
        D2D::RoundedRect(rt, box, 2, A(D2D1::ColorF(0x1A1D24u)));
        // 从独立缩略图缓存取位图（一步缩到 80×110 等比，未生成时占位）
        int tw = 0, th = 0;
        ID2D1Bitmap* bmp = ThumbBitmapFor((int)i, rt, tw, th);
        if (bmp && tw > 0 && th > 0) {
            // 等比适配到 80×110 框中央（缩略图实际像素已是等比尺寸，1:1 映射质量最佳）
            float bw = box.right - box.left, bh = box.bottom - box.top;
            float s = std::min(bw / tw, bh / th);
            float dw = tw * s, dh = th * s;
            float dx = box.left + (bw - dw) / 2.0f, dy = box.top + (bh - dh) / 2.0f;
            rt->DrawBitmap(bmp, D2D1::RectF(dx, dy, dx + dw, dy + dh),
                           thumbAlpha_, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }
    rt->PopAxisAlignedClip();

    // 固定中央绿色指示框（绘制在缩略图之上：避免被缩略图遮挡）
    float cx = Width() / 2.0f;
    D2D1_RECT_F ind{ cx - 42, (float)(ThumbTop() + 18), cx + 42, (float)(ThumbTop() + 18 + 114) };
    D2D::RoundedRect(rt, ind, 5, D2D1::ColorF(0, 0, 0, 0), A(theme::AccentCyan()), 2.0f);
}

// ============== 全页缩略图侧边栏 ==============
// 顶部网格按钮触发，从右侧展开，浮于视图区上方（含缩略图栏），不遮挡上下工具栏
void ReaderWindow::LayoutGrid() {
    gridRects_.clear();
    if (!loaded_ || pages_.empty()) {
        gridPanelRect_ = D2D1::RectF(0, 0, 0, 0);
        gridTrack_ = gridSlider_ = gridPanelRect_;
        gridMaxScroll_ = 0;
        return;
    }
    // 侧边栏整体区域：右侧 360px 宽，纵向覆盖 ViewTop~ViewBottom（不含上下工具栏）
    float panelW = 360.0f;
    gridPanelRect_ = D2D1::RectF((float)(Width() - panelW), (float)ViewTop(), (float)Width(), (float)ViewBottom());

    // 网格区：去掉 padding 和右侧滚动条
    float pad = 12.0f, scrollW = 8.0f;
    float gridLeft = gridPanelRect_.left + pad;
    float gridRight = gridPanelRect_.right - scrollW - pad;
    float gridTop = gridPanelRect_.top + pad;
    float gridW = gridRight - gridLeft;

    // 每行 3 列，cell 100×140（缩略图 80×110 + 页号文字 20）
    int cols = 3;
    float cellW = 100.0f, cellH = 140.0f;
    float gapX = (gridW - cols * cellW) / (cols + 1);  // 均匀间距
    float gapY = 12.0f;

    int total = (int)pages_.size();
    int rows = (total + cols - 1) / cols;
    float contentH = rows * cellH + (rows + 1) * gapY;
    float viewH = gridPanelRect_.bottom - gridPanelRect_.top - 2 * pad;
    gridMaxScroll_ = std::max(0.0f, contentH - viewH);
    gridScroll_ = (int)std::clamp((float)gridScroll_, 0.0f, gridMaxScroll_);

    for (int i = 0; i < total; i++) {
        int col = i % cols, row = i / cols;
        float x = gridLeft + gapX + col * (cellW + gapX);
        float y = gridTop + gapY + row * (cellH + gapY) - (float)gridScroll_;
        gridRects_.push_back(D2D1::RectF(x, y, x + cellW, y + cellH));
    }

    // 滚动条轨道与滑块
    gridTrack_ = D2D1::RectF(gridPanelRect_.right - scrollW - 2, gridTop,
                             gridPanelRect_.right - 2, gridPanelRect_.bottom - pad);
    if (gridMaxScroll_ > 0) {
        float sliderH = std::max(20.0f, viewH * viewH / contentH);
        float trackH = gridTrack_.bottom - gridTrack_.top;
        float sliderY = gridTrack_.top + (trackH - sliderH) * (float)gridScroll_ / gridMaxScroll_;
        gridSlider_ = D2D1::RectF(gridTrack_.left, sliderY, gridTrack_.right, sliderY + sliderH);
    } else {
        gridSlider_ = gridTrack_;
    }
}

void ReaderWindow::DrawGridPanel(ID2D1RenderTarget* rt) {
    LayoutGrid();
    if (gridRects_.empty()) return;

    // 左侧遮罩：让视图区变暗（点击遮罩区关闭侧边栏）
    D2D1_RECT_F mask{ 0, (float)ViewTop(), gridPanelRect_.left, (float)ViewBottom() };
    D2D::RoundedRect(rt, mask, 0, D2D1::ColorF(0, 0, 0, 0.5f));

    // 侧边栏背景 + 左边框
    D2D::RoundedRect(rt, gridPanelRect_, 0, theme::BgSidebar());
    D2D1_RECT_F lb{ gridPanelRect_.left, gridPanelRect_.top, gridPanelRect_.left + 1, gridPanelRect_.bottom };
    D2D::RoundedRect(rt, lb, 0, theme::BorderColor());

    // 网格缩略图（裁剪到面板内，超出上下边界不绘制）
    D2D1_RECT_F clip{ gridPanelRect_.left, gridPanelRect_.top, gridPanelRect_.right, gridPanelRect_.bottom };
    rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (int i = 0; i < (int)gridRects_.size(); i++) {
        const auto& r = gridRects_[i];
        if (r.bottom < gridPanelRect_.top || r.top > gridPanelRect_.bottom) continue;

        // 当前页高亮（双页模式高亮 current_ 和 current_+1）
        bool isCur = (i == current_) || (mode_ == ReaderMode::Double && i == current_ + 1);
        bool isHv = (i == gridHover_);
        D2D1_COLOR_F border = isCur ? theme::AccentCyan() : (isHv ? theme::TextSecondary() : theme::BorderColor());
        float bw = 1.5f;
        // 缩略图框（80×110，居中在 cell 上半部分；cellW 与 LayoutGrid 保持一致 = 100）
        float cellW = 100.0f, thumbW = 80.0f, thumbH = 110.0f;
        float tx = r.left + (cellW - thumbW) / 2.0f;
        float ty = r.top + 8.0f;
        D2D1_RECT_F thumbBox{ tx, ty, tx + thumbW, ty + thumbH };
        D2D::RoundedRect(rt, D2D1::RectF(thumbBox.left - 1, thumbBox.top - 1, thumbBox.right + 1, thumbBox.bottom + 1),
                         3, D2D1::ColorF(0, 0, 0, 0), border, bw);
        D2D::RoundedRect(rt, thumbBox, 2, theme::BgMain());

        // 绘制缩略图（复用 thumbCache_，1:1 等比适配）
        int pw = 0, ph = 0;
        ID2D1Bitmap* bmp = ThumbBitmapFor(i, rt, pw, ph);
        if (bmp && pw > 0 && ph > 0) {
            float s = std::min(thumbW / pw, thumbH / ph);
            float dw = pw * s, dh = ph * s;
            float dx = tx + (thumbW - dw) / 2.0f, dy = ty + (thumbH - dh) / 2.0f;
            rt->DrawBitmap(bmp, D2D1::RectF(dx, dy, dx + dw, dy + dh),
                           1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }

        // 页号文字（cell 下半部分）
        wchar_t num[16];
        swprintf_s(num, L"%d", i + 1);
        D2D1_RECT_F nr{ r.left, r.bottom - 22, r.right, r.bottom - 4 };
        D2D1_COLOR_F nc = isCur ? theme::AccentCyan() : theme::TextSecondary();
        D2D::Text(rt, num, nr, nc, 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    rt->PopAxisAlignedClip();

    // 滚动条
    if (gridSlider_.bottom - gridSlider_.top < gridTrack_.bottom - gridTrack_.top) {
        D2D::RoundedRect(rt, gridTrack_, 4, theme::BorderColor());
        D2D::RoundedRect(rt, gridSlider_, 4, theme::TextSecondary());
    }
}

int ReaderWindow::HitGrid(int x, int y) const {
    if (!gridOpen_ || gridRects_.empty()) return -1;
    // 不在侧边栏面板内
    if (x < gridPanelRect_.left || x > gridPanelRect_.right ||
        y < gridPanelRect_.top || y > gridPanelRect_.bottom) return -1;
    for (int i = 0; i < (int)gridRects_.size(); i++) {
        const auto& r = gridRects_[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

bool ReaderWindow::OnGridScroll(int x, int y) const {
    if (!gridOpen_) return false;
    return x >= gridTrack_.left - 2 && x <= gridTrack_.right + 2 &&
           y >= gridTrack_.top && y <= gridTrack_.bottom;
}

void ReaderWindow::DrawProgressBar(ID2D1RenderTarget* rt) {
    D2D1_RECT_F bg{ 0, (float)BarTop(), (float)Width(), (float)BarBottom() };
    D2D::RoundedRect(rt, bg, 0, theme::BgTitlebar());
    D2D1_RECT_F l1{ 0, (float)BarTop(), (float)Width(), (float)BarTop() + 1 };
    D2D::RoundedRect(rt, l1, 0, theme::BorderColor());
    int total = (int)pages_.size();
    int pct = total > 0 ? (int)lround((current_ + 1) * 100.0 / total) : 0;
    wchar_t buf[64];
    swprintf_s(buf, L"%d / %d", total > 0 ? current_ + 1 : 0, total);
    // 左：页码
    D2D1_RECT_F left{ 16, (float)BarTop(), 140, (float)BarBottom() };
    D2D::Text(rt, buf, left, theme::TextPrimary(), 12);
    float capW = D2D::TextWidth(buf, 12);
    wchar_t pctBuf[32];
    swprintf_s(pctBuf, L"\xB7 %d%%", pct);
    D2D1_RECT_F lpct{ 16 + capW, (float)BarTop(), 16 + capW + 60, (float)BarBottom() };
    D2D::Text(rt, pctBuf, lpct, theme::AccentCyan(), 12);
    // 中：上一页 + 滑条 + 下一页
    D2D1_COLOR_F pcol = hoverPrev_ ? theme::TextPrimary() : theme::TextSecondary();
    D2D::Icon(rt, L"\xE76B", prevBtn_, pcol, 14);
    D2D::RoundedRect(rt, sliderRect_, 2, theme::BorderColor());
    float ratio = total > 1 ? (float)current_ / (total - 1) : 0;
    D2D1_RECT_F fillR{ sliderRect_.left, sliderRect_.top,
                       sliderRect_.left + (sliderRect_.right - sliderRect_.left) * ratio, sliderRect_.bottom };
    D2D::RoundedRect(rt, fillR, 2, theme::AccentCyan());
    D2D1_RECT_F th{ sliderRect_.left + (sliderRect_.right - sliderRect_.left) * ratio - 5,
                    sliderRect_.top - 4, sliderRect_.left + (sliderRect_.right - sliderRect_.left) * ratio + 5,
                    sliderRect_.bottom + 4 };
    D2D::RoundedRect(rt, th, 6, theme::TextPrimary());
    D2D1_COLOR_F ncol = hoverNext_ ? theme::TextPrimary() : theme::TextSecondary();
    D2D::Icon(rt, L"\xE76C", nextBtn_, ncol, 14);

    // 最右侧：预解码状态文本（仿照图片浏览器"预解码:完成:xxx.jpg WxH"）
    // 位置：下一页按钮右侧 +16 到 窗口右边缘 -12，右对齐
    std::wstring act;
    {
        std::lock_guard<std::mutex> lk(actMutex_);
        act = decodeActivity_;
    }
    if (!act.empty()) {
        D2D1_RECT_F ar{ nextBtn_.right + 16, (float)BarTop(), (float)Width() - 12, (float)BarBottom() };
        D2D::Text(rt, act, ar, theme::TextSecondary(), 11, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

void ReaderWindow::DrawFloatButtons(ID2D1RenderTarget* rt) {
    int cy = (ViewTop() + ViewBottom()) / 2;
    D2D1_COLOR_F c1 = hoverPrevF_ ? theme::TextPrimary() : theme::TextSecondary();
    D2D::Icon(rt, L"\xE76B", prevFloat_, c1, 18);
    D2D1_COLOR_F c2 = hoverNextF_ ? theme::TextPrimary() : theme::TextSecondary();
    D2D::Icon(rt, L"\xE76C", nextFloat_, c2, 18);
}

void ReaderWindow::DrawTitleBarContent(ID2D1RenderTarget* rt, int w, int h) {
    D2D::Icon(rt, L"\xE737", D2D1::RectF(14, 0, 28, (float)TitleBarHeight), theme::AccentCyan(), 12);
    D2D1_RECT_F tr{ 30, 0, (float)w - 116, (float)h };
    D2D::Text(rt, W(comic_.title), tr, theme::TextPrimary(), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

// ==================== 几何 / 命中 ====================
void ReaderWindow::ComputeRects() {
    int ty = ToolbarTop() + 8;
    int bh = 28;
    btnSingle_ = D2D1::RectF(12, (float)ty, 56, (float)(ty + bh));
    btnDrop_ = D2D1::RectF(60, (float)ty, 146, (float)(ty + bh));
    btnThumb_ = D2D1::RectF(150, (float)ty, 236, (float)(ty + bh));
    // 图片大小：分隔线(8) + "图片大小"文本(54) + 间距(10) 后开始滑条，避免滑条压住文本
    float sx = btnThumb_.right + 8 + 54 + 10;
    toolSliderRect_ = D2D1::RectF(sx, (float)(ty + 13), sx + 160, (float)(ty + 15));
    percentRect_ = D2D1::RectF(sx + 168, (float)ty, sx + 208, (float)(ty + bh));
    btnWheel_ = D2D1::RectF(sx + 216, (float)ty, sx + 302, (float)(ty + bh));
    btnFit_ = D2D1::RectF(sx + 306, (float)ty, sx + 392, (float)(ty + bh));
    // 网格总览按钮：顶部工具栏最右侧（4 格网格图标，点击展开全页缩略图侧边栏）
    btnGrid_ = D2D1::RectF((float)(Width() - 44), (float)ty, (float)(Width() - 12), (float)(ty + bh));

    // 底部进度条
    int by = BarTop() + 8, bbh = 28;
    int center = Width() / 2;
    prevBtn_ = D2D1::RectF(center - 190, (float)by, center - 162, (float)(by + bbh));
    sliderRect_ = D2D1::RectF(center - 150, (float)(by + 13), center + 150, (float)(by + 15));
    nextBtn_ = D2D1::RectF(center + 162, (float)by, center + 190, (float)(by + bbh));

    // 悬浮翻页按钮
    int cy = (ViewTop() + ViewBottom()) / 2;
    prevFloat_ = D2D1::RectF(16, (float)(cy - 18), 52, (float)(cy + 18));
    nextFloat_ = D2D1::RectF((float)(Width() - 52), (float)(cy - 18), (float)(Width() - 16), (float)(cy + 18));
    LayoutThumbs();
}

void ReaderWindow::LayoutThumbs() {
    thumbRects_.clear();
    if (!loaded_) return;
    int top = ThumbTop() + 22, h = 110;
    // 列表以当前页为中心展开：当前页中心 = Width()/2 + thumbScroll_（手动拖动偏移）
    // 这样首末页也能精确落入中央绿色框，不受列表边界 clamp 影响
    float firstX = Width() / 2.0f - (float)current_ * 88.0f - 40.0f + (float)thumbScroll_;
    for (size_t i = 0; i < pages_.size(); i++) {
        float x = firstX + (float)i * 88.0f;
        thumbRects_.push_back(D2D1::RectF(x, (float)top, x + 80.0f, (float)(top + h)));
    }
}

int ReaderWindow::HitToolbarButton(int x, int y) const {
    auto hit = [&](const D2D1_RECT_F& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (hit(btnSingle_)) return kBtnSingle;
    if (hit(btnDrop_)) return kBtnDrop;
    if (hit(btnThumb_)) return kBtnThumb;
    if (hit(btnWheel_)) return kBtnWheel;
    if (hit(btnFit_)) return kBtnFit;
    if (hit(btnGrid_)) return kBtnGrid;
    return 0;
}

bool ReaderWindow::OnSlider(int x, int y) const {
    return x >= sliderRect_.left && x <= sliderRect_.right && y >= sliderRect_.top - 6 && y <= sliderRect_.bottom + 6;
}

void ReaderWindow::UpdateSlider(int x) {
    int total = (int)pages_.size();
    if (total <= 0) return;
    float ratio = std::clamp((x - sliderRect_.left) / (sliderRect_.right - sliderRect_.left), 0.0f, 1.0f);
    GotoPage((int)lround(ratio * (total - 1)));
}

void ReaderWindow::UpdateScaleFromToolSlider(int x) {
    float ratio = std::clamp((x - toolSliderRect_.left) / (toolSliderRect_.right - toolSliderRect_.left), 0.0f, 1.0f);
    scale_ = 1.0f + ratio * 4.0f; // 1.0 ~ 5.0
    Invalidate();
}

int ReaderWindow::HitThumb(int x, int y) const {
    for (size_t i = 0; i < thumbRects_.size(); i++)
        if (x >= thumbRects_[i].left && x < thumbRects_[i].right && y >= thumbRects_[i].top && y < thumbRects_[i].bottom)
            return (int)i;
    return -1;
}

// ==================== 交互 ====================
void ReaderWindow::OnLButtonDown(int x, int y) {
    dragging_ = true;
    // 工具栏
    int b = HitToolbarButton(x, y);
    if (b) return;
    // 全页缩略图侧边栏打开时：优先处理侧边栏交互（缩略图跳页/滚动条/遮罩关闭）
    if (gridOpen_) {
        // 点击侧边栏缩略图 → 跳页（双页模式下点击页作为左页 current_）
        int gi = HitGrid(x, y);
        if (gi >= 0) {
            GotoPage(gi);
            return;
        }
        // 命中滚动条 → 拖拽
        if (OnGridScroll(x, y)) {
            dragType_ = 6;
            gridDragScroll_ = true;
            dragStart_ = { x, y };
            gridScrollStart_ = gridScroll_;
            return;
        }
        // 点击遮罩区（侧边栏左侧视图区）→ 关闭侧边栏
        if (x < gridPanelRect_.left) {
            gridOpen_ = false;
            Invalidate();
            return;
        }
        return;  // 侧边栏内空白点击不穿透到下层
    }
    // 工具栏图片大小滑条（仅单页）
    if (mode_ == ReaderMode::Single && x >= toolSliderRect_.left && x <= toolSliderRect_.right &&
        y >= toolSliderRect_.top - 6 && y <= toolSliderRect_.bottom + 6) {
        dragType_ = 4;
        UpdateScaleFromToolSlider(x);
        return;
    }
    // 底部滑条
    if (y >= BarTop() && y < BarBottom()) {
        if (x >= prevBtn_.left && x < prevBtn_.right && y >= prevBtn_.top && y < prevBtn_.bottom) { PrevPage(); return; }
        if (x >= nextBtn_.left && x < nextBtn_.right && y >= nextBtn_.top && y < nextBtn_.bottom) { NextPage(); return; }
        if (OnSlider(x, y)) { dragType_ = 2; UpdateSlider(x); return; }
        return;
    }
    // 缩略图
    if (y >= ThumbTop() && y < ThumbBottom() && showThumbs_) {
        int t = HitThumb(x, y);
        if (t >= 0) { GotoPage(t); return; }  // 点击缩略图跳页
        dragType_ = 3;
        dragStart_ = { x, y };
        sliderStart_ = thumbScroll_;
        return;
    }
    // 悬浮翻页
    if (mode_ != ReaderMode::Vertical) {
        if (x >= prevFloat_.left && x < prevFloat_.right && y >= prevFloat_.top && y < prevFloat_.bottom) { PrevPage(); return; }
        if (x >= nextFloat_.left && x < nextFloat_.right && y >= nextFloat_.top && y < nextFloat_.bottom) { NextPage(); return; }
    }
    // 下拉模式：右侧竖向滚动条拖拽 / 点击轨道翻页
    if (mode_ == ReaderMode::Vertical) {
        LayoutVScroll();   // 确保几何最新（首次点击前可能未绘制）
        if (VerticalMaxScroll() > 0 &&
            x >= vTrack_.left - 4 && x <= vTrack_.right + 4 &&
            y >= vTrack_.top && y <= vTrack_.bottom) {
            if (HitVScroll(x, y)) {
                dragType_ = 5;   // 拖拽滑块
                dragStart_ = { x, y };
                vScrollStart_ = verticalScroll_;
            } else {
                // 点击轨道空白：向上下翻一屏
                int viewH = ViewBottom() - ViewTop();
                int maxS = VerticalMaxScroll();
                verticalScroll_ = std::clamp(verticalScroll_ + (y < vThumb_.top ? -viewH : viewH), 0, maxS);
                UpdateDecodeSet(); Invalidate();
            }
            return;
        }
    }
    // 单页平移
    if (mode_ == ReaderMode::Single) {
        dragType_ = 1;
        dragStart_ = { x, y };
        panStartX_ = panX_; panStartY_ = panY_;
    }
}

void ReaderWindow::OnLButtonUp(int x, int y) {
    if (!dragging_) return;
    dragging_ = false;
    // 工具栏点击
    int b = HitToolbarButton(x, y);
    if (b) {
        switch (b) {
            case kBtnSingle: SetMode(mode_ == ReaderMode::Single ? ReaderMode::Double : ReaderMode::Single); break;
            case kBtnDrop: SetMode(mode_ == ReaderMode::Vertical ? ReaderMode::Single : ReaderMode::Vertical); break;
            case kBtnThumb:
                if (mode_ != ReaderMode::Vertical) {
                    showThumbs_ = !showThumbs_;
                    if (!showThumbs_) {
                        KillTimerEx(kThumbHideTimer);
                        KillTimerEx(kThumbAnimTimer);
                        thumbAnimDir_ = 0; thumbAlpha_ = 0.0f; thumbsVisible_ = false;
                    }
                    Invalidate();
                }
                break;
            case kBtnWheel:
                if (mode_ == ReaderMode::Single) { wheelZoom_ = !wheelZoom_; Invalidate(); }
                break;
            case kBtnFit:
                if (mode_ == ReaderMode::Single) FitHeight();
                break;
            case kBtnGrid:
                gridOpen_ = !gridOpen_;
                Invalidate();
                break;
        }
        return;
    }
    if (dragType_ == 2) { SaveProgress(); dragType_ = 0; return; } // 滑条松手保存
    if (dragType_ == 6) { gridDragScroll_ = false; dragType_ = 0; return; } // 网格滚动条松手
    dragType_ = 0;
}

void ReaderWindow::OnMouseMove(int x, int y) {
    // 缩略图自动显隐：鼠标进入底部缩略图栏/工具栏区域立即显示，离开后延时 0.7s 淡出收回
    if (showThumbs_) {
        // 触发区覆盖缩略图栏完整高度(158) + 底部工具栏，确保鼠标进入栏上部也立即显示
        bool nearBottom = y >= Height() - 44 - 158;
        bool onPanel = thumbsVisible_ && y >= ThumbTop();    // 已显示时面板内保持
        if (nearBottom || onPanel) {
            if (!thumbsVisible_) {
                thumbsVisible_ = true;
                thumbAlpha_ = 1.0f;                           // 立即显示（用户偏好即时响应）
                thumbAnimDir_ = 0;
                Invalidate();                                 // 立即重绘，避免等待下一次移动
            }
            KillTimerEx(kThumbHideTimer);
            StartTimer(kThumbHideTimer, 700);
        }
    }
    // 悬停刷新
    int b = HitToolbarButton(x, y);
    if (b != hoverBtn_) { hoverBtn_ = b; Invalidate(); }
    bool hp = x >= prevBtn_.left && x < prevBtn_.right && y >= prevBtn_.top && y < prevBtn_.bottom;
    bool hn = x >= nextBtn_.left && x < nextBtn_.right && y >= nextBtn_.top && y < nextBtn_.bottom;
    bool hpf = mode_ != ReaderMode::Vertical && x >= prevFloat_.left && x < prevFloat_.right && y >= prevFloat_.top && y < prevFloat_.bottom;
    bool hnf = mode_ != ReaderMode::Vertical && x >= nextFloat_.left && x < nextFloat_.right && y >= nextFloat_.top && y < nextFloat_.bottom;
    if (hp != hoverPrev_ || hn != hoverNext_ || hpf != hoverPrevF_ || hnf != hoverNextF_) {
        hoverPrev_ = hp; hoverNext_ = hn; hoverPrevF_ = hpf; hoverNextF_ = hnf;
        Invalidate();
    }
    int ht = HitThumb(x, y);
    if (ht != hoverThumb_) { hoverThumb_ = ht; Invalidate(); }
    // 竖向滚动条悬停高亮
    if (mode_ == ReaderMode::Vertical) LayoutVScroll();
    bool hv = (mode_ == ReaderMode::Vertical && VerticalMaxScroll() > 0 &&
               x >= vTrack_.left - 4 && x <= vTrack_.right + 4 &&
               y >= vTrack_.top && y <= vTrack_.bottom);
    if (hv != hoverVScroll_) { hoverVScroll_ = hv; Invalidate(); }
    // 网格侧边栏悬停高亮
    if (gridOpen_) {
        int gh = HitGrid(x, y);
        if (gh != gridHover_) { gridHover_ = gh; Invalidate(); }
    } else if (gridHover_ != -1) { gridHover_ = -1; }

    if (!dragging_) return;
    if (dragType_ == 1) { // 平移
        panX_ = panStartX_ + (x - dragStart_.x);
        panY_ = panStartY_ + (y - dragStart_.y);
        Invalidate();
    } else if (dragType_ == 2) { // 滑条（底部/工具栏图片大小共用 sliderRect_，底部优先）
        UpdateSlider(x);
    } else if (dragType_ == 3) { // 缩略图横向滚动（双向自由拖动，切页时自动归位）
        thumbScroll_ = sliderStart_ - (x - dragStart_.x);
        LayoutThumbs();
        Invalidate();
    } else if (dragType_ == 4) { // 工具栏图片大小滑条
        UpdateScaleFromToolSlider(x);
    } else if (dragType_ == 5) { // 竖向滚动条拖拽
        int maxS = VerticalMaxScroll();
        if (maxS > 0) {
            float trackH = vTrack_.bottom - vTrack_.top;
            float thumbH = vThumb_.bottom - vThumb_.top;
            // 鼠标位移映射到滚动量（滑块可移动范围 = trackH - thumbH）
            float dy = (float)(y - dragStart_.y);
            float scrollDelta = dy * (maxS / std::max(1.0f, trackH - thumbH));
            verticalScroll_ = std::clamp(vScrollStart_ + (int)scrollDelta, 0, maxS);
            UpdateDecodeSet(); Invalidate();
        }
    } else if (dragType_ == 6) { // 网格侧边栏滚动条拖拽
        LayoutGrid();
        if (gridMaxScroll_ > 0) {
            float trackH = gridTrack_.bottom - gridTrack_.top;
            float sliderH = gridSlider_.bottom - gridSlider_.top;
            float dy = (float)(y - dragStart_.y);
            float delta = dy * (gridMaxScroll_ / std::max(1.0f, trackH - sliderH));
            gridScroll_ = (int)std::clamp((float)gridScrollStart_ + delta, 0.0f, gridMaxScroll_);
            Invalidate();
        }
    }
}

void ReaderWindow::OnMouseWheel(int delta) {
    if (!loaded_ || pages_.empty()) return;
    // 网格侧边栏打开时：滚轮滚动网格而非翻页
    if (gridOpen_) {
        LayoutGrid();
        int step = 152;  // cellH(140) + gapY(12)，一次滚一行
        gridScroll_ = (int)std::clamp((float)gridScroll_ + (delta > 0 ? -step : step), 0.0f, gridMaxScroll_);
        Invalidate();
        return;
    }
    if (mode_ == ReaderMode::Vertical) {
        int maxScroll = 0;
        float y = 0;
        for (size_t i = 0; i < pages_.size(); i++) {
            int pw = pw_[i] > 0 ? pw_[i] : 800, ph = ph_[i] > 0 ? ph_[i] : 1200;
            float pageW = (float)(Width() - 40);
            y += pageW * ph / pw + 12;
        }
        maxScroll = (int)std::max(0.0f, y - (ViewBottom() - ViewTop()));
        verticalScroll_ = std::clamp(verticalScroll_ + (delta < 0 ? 60 : -60), 0, maxScroll);
        UpdateDecodeSet();
        Invalidate();
        return;
    }
    if (mode_ == ReaderMode::Single && wheelZoom_) {
        // 以鼠标为中心缩放（1.1 倍，1.0~5.0）
        float ratio = 1.1f;
        float newScale = std::clamp(scale_ * (delta > 0 ? ratio : 1.0f / ratio), 1.0f, 5.0f);
        if (newScale == scale_) return;
        // 鼠标相对视口中心偏移
        POINT p; GetCursorPos(&p); ScreenToClient(Hwnd(), &p);
        float dx = p.x - Width() / 2.0f, dy = p.y - (ViewTop() + ViewBottom()) / 2.0f;
        float r = newScale / scale_;
        panX_ = dx * (1.0f - r) + panX_ * r;
        panY_ = dy * (1.0f - r) + panY_ * r;
        scale_ = newScale;
        Invalidate();
        return;
    }
    // 默认翻页
    if (delta > 0) PrevPage(); else NextPage();
}

// 定时器：缩略图隐藏到期 → 启动淡出；动画定时器驱动淡入淡出
void ReaderWindow::OnTimer(UINT id) {
    if (id == kThumbHideTimer) {
        KillTimerEx(kThumbHideTimer);
        if (thumbsVisible_ && thumbAnimDir_ != -1) {
            thumbAnimDir_ = -1;   // 淡出
            StartTimer(kThumbAnimTimer, 16);
        }
    } else if (id == kThumbAnimTimer) {
        const float step = 0.12f;
        if (thumbAnimDir_ == 1) {
            thumbAlpha_ = std::min(1.0f, thumbAlpha_ + step);
            if (thumbAlpha_ >= 1.0f) { thumbAnimDir_ = 0; KillTimerEx(kThumbAnimTimer); }
        } else if (thumbAnimDir_ == -1) {
            thumbAlpha_ = std::max(0.0f, thumbAlpha_ - step);
            if (thumbAlpha_ <= 0.0f) {
                thumbAnimDir_ = 0; KillTimerEx(kThumbAnimTimer);
                thumbsVisible_ = false;   // 完全淡出后置为隐藏
            }
        }
        Invalidate();
    }
}

void ReaderWindow::OnKeyDown(UINT vk) {
    if (!loaded_) return;
    bool fwd = true; // 前进方向
    switch (vk) {
        case VK_LEFT: case VK_PRIOR: fwd = false; break;
        case VK_RIGHT: case VK_NEXT: case VK_SPACE: fwd = true; break;
        case VK_HOME: SetCurrent(0); return;
        case VK_END: SetCurrent((int)pages_.size() - 1); return;
        default: return;
    }
    if (rtl_ && (vk == VK_LEFT || vk == VK_RIGHT)) fwd = !fwd;
    if (fwd) NextPage(); else PrevPage();
}

void ReaderWindow::OnResize(int w, int h) {
    ComputeRects();
    UpdateDecodeSet();
}

bool ReaderWindow::OnCloseRequested() {
    SaveProgress();
    return true;
}

void ReaderWindow::OnDestroy() {
    KillTimerEx(kThumbHideTimer);
    KillTimerEx(kThumbAnimTimer);
    SaveProgress();
    // 性能遥测：清除快照中的漫画上下文（避免残留上一个漫画状态）
    {
        std::lock_guard<std::mutex> lk(PerfState::Mutex());
        auto& s = PerfState::Current();
        s.comicTitle.clear();
        s.currentPage = -1;
        s.totalPages = 0;
        s.cacheEntries = 0;
        s.cacheMemMB = 0;
    }
    if (onClosed_) onClosed_();
    selfDelete_ = true;
}

// ==================== 翻页 / 模式 / 进度 ====================
void ReaderWindow::SetCurrent(int idx) {
    int total = (int)pages_.size();
    if (total == 0) return;
    idx = std::clamp(idx, 0, total - 1);
    if (idx == current_) return;
    current_ = idx;
    // 性能遥测：快照记录当前页码
    {
        std::lock_guard<std::mutex> lk(PerfState::Mutex());
        PerfState::Current().currentPage = current_;
    }
    ActivityLog::Instance().Log(L"翻页", L"跳转到第 " + std::to_wstring(current_ + 1) + L" 页");
    SaveProgress();
    UpdateDecodeSet();
    CenterThumbOnCurrent();   // 缩略图跟随当前页居中
    Invalidate();
}

void ReaderWindow::PrevPage() {
    if (mode_ == ReaderMode::Double) SetCurrent(current_ - 2);
    else SetCurrent(current_ - 1);
}
void ReaderWindow::NextPage() {
    if (mode_ == ReaderMode::Double) SetCurrent(current_ + 2);
    else SetCurrent(current_ + 1);
}

void ReaderWindow::GotoPage(int idx) {
    int total = (int)pages_.size();
    if (total == 0) return;
    current_ = std::clamp(idx, 0, total - 1);
    // 性能遥测：快照记录当前页码
    {
        std::lock_guard<std::mutex> lk(PerfState::Mutex());
        PerfState::Current().currentPage = current_;
    }
    if (mode_ == ReaderMode::Vertical) VerticalScrollToCurrent();  // 下拉模式跳页滚动到对应位置
    UpdateDecodeSet();
    CenterThumbOnCurrent();   // 缩略图跟随当前页居中
    Invalidate();
}

// 保存进度（含状态流转 Unread→Reading→Completed）
void ReaderWindow::SaveProgress() {
    if (!service_ || !loaded_ || pages_.empty()) return;
    int total = (int)pages_.size();
    if (current_ < 0 || current_ >= total) return;
    service_->SaveReadingProgress(comic_.id, current_, total);
    comic_.currentPage = current_;
    comic_.pageCount = total;
    comic_.status = (current_ >= total - 1) ? ComicStatus::Completed : ComicStatus::Reading;
}

void ReaderWindow::ResetView() {
    scale_ = 1.0f; panX_ = 0; panY_ = 0; wheelZoom_ = false;
}
void ReaderWindow::FitHeight() {
    ResetView();
    wheelZoom_ = false;
    Invalidate();
}

void ReaderWindow::SetMode(ReaderMode m) {
    if (m == mode_) return;
    mode_ = m;
    ResetView();
    verticalScroll_ = 0;
    if (mode_ == ReaderMode::Vertical) VerticalScrollToCurrent();
    UpdateDecodeSet();
    Invalidate();
}

// 下拉模式定位到当前页
void ReaderWindow::VerticalScrollToCurrent() {
    float y = 0;
    for (int i = 0; i < current_ && i < (int)pages_.size(); i++) {
        int pw = pw_[i] > 0 ? pw_[i] : 800, ph = ph_[i] > 0 ? ph_[i] : 1200;
        y += (float)(Width() - 40) * ph / pw + 12;
    }
    verticalScroll_ = (int)y;
}

// 缩略图横向滚动让当前页居中（绿色指示框固定面板中央不动）
// 新布局下当前页中心默认就在 Width()/2，重置手动偏移即可
void ReaderWindow::CenterThumbOnCurrent() {
    if (pages_.empty()) return;
    thumbScroll_ = 0;
    LayoutThumbs();
}

// 下拉模式所有页面总高度
int ReaderWindow::VerticalContentH() const {
    float y = 0;
    for (size_t i = 0; i < pages_.size(); i++) {
        int pw = pw_[i] > 0 ? pw_[i] : 800, ph = ph_[i] > 0 ? ph_[i] : 1200;
        y += (float)(Width() - 40) * ph / pw + 12;
    }
    return (int)y;
}

// 下拉模式最大滚动值
int ReaderWindow::VerticalMaxScroll() const {
    int viewH = ViewBottom() - ViewTop();
    return std::max(0, VerticalContentH() - viewH);
}

// 计算竖向滚动条轨道与滑块几何（右侧 8px 宽）
void ReaderWindow::LayoutVScroll() {
    int viewTop = ViewTop(), viewBot = ViewBottom();
    int trackH = viewBot - viewTop - 16;
    vTrack_ = D2D1::RectF((float)(Width() - 12), (float)(viewTop + 8),
                          (float)(Width() - 4), (float)(viewBot - 8));
    int contentH = VerticalContentH();
    int viewH = viewBot - viewTop;
    int maxS = std::max(0, contentH - viewH);
    // 滑块高度按可视区占比，最小 30px
    float thumbH = maxS > 0 ? std::max(30.0f, (float)viewH * trackH / contentH) : (float)trackH;
    float thumbY = vTrack_.top + (maxS > 0 ? (float)verticalScroll_ / maxS * (trackH - thumbH) : 0);
    vThumb_ = D2D1::RectF(vTrack_.left, thumbY, vTrack_.right, thumbY + thumbH);
}

// 命中竖向滚动条滑块
bool ReaderWindow::HitVScroll(int x, int y) const {
    return x >= vThumb_.left - 2 && x <= vThumb_.right + 2 &&
           y >= vThumb_.top && y <= vThumb_.bottom;
}

} // namespace ark::ui