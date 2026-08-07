#pragma once
// 漫画阅读器窗口：单页 / 双页 / 下拉三种模式 + 工具栏 + 缩略图 + 底部进度条
#include "WindowBase.h"
#include "Models.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>

namespace ark {
class ComicLibraryService;
}

namespace ark::ui {

// 阅读模式
enum class ReaderMode { Single, Double, Vertical };

class ReaderWindow : public WindowBase {
public:
    ReaderWindow();
    ~ReaderWindow();
    // 打开漫画并从上次进度页继续（service 由主窗口持有，用于保存进度）
    void OpenComic(ComicLibraryService* service, const ComicModel& comic);
    // 关闭回调（窗口销毁后触发，用于主窗口刷新阅读进度）
    void SetOnClosed(std::function<void()> cb) { onClosed_ = std::move(cb); }

protected:
    void OnPaint(ID2D1RenderTarget* rt, int w, int h) override;
    void OnLButtonDown(int x, int y) override;
    void OnLButtonUp(int x, int y) override;
    void OnMouseMove(int x, int y) override;
    void OnKeyDown(UINT vk) override;
    void OnResize(int w, int h) override;
    void OnMouseWheel(int delta) override;
    void OnTimer(UINT id) override;
    void DrawTitleBarContent(ID2D1RenderTarget* rt, int w, int h) override;
    bool OnCloseRequested() override;
    void OnDestroy() override;

private:
    // ---- 布局 ----
    int ToolbarTop() const   { return TitleBarHeight; }
    int ToolbarBottom() const { return TitleBarHeight + 44; }
    int ViewTop() const      { return ToolbarBottom(); }
    int ThumbH() const       { return (showThumbs_ && thumbsVisible_) ? 158 : 0; }
    int ViewBottom() const   { return Height() - 44; } // 视图区固定，缩略图作为浮层叠加（淡入淡出不挤压图片）
    int ThumbTop() const     { return Height() - 44 - ThumbH(); }
    int ThumbBottom() const  { return Height() - 44; }
    int BarTop() const       { return Height() - 44; }
    int BarBottom() const    { return Height(); }

    // ---- 数据 / 翻页 ----
    void LoadPages();            // 后台线程：提页列表 + 页尺寸
    void SetCurrent(int idx);    // 边界限制 + 保存进度
    void PrevPage();
    void NextPage();
    void SaveProgress();
    void GotoPage(int idx);      // 跳页（不保存进度，用于滑条拖动）
    void ResetView();
    void FitHeight();
    void SetMode(ReaderMode m);
    void VerticalScrollToCurrent();  // 下拉模式定位到当前页
    void CenterThumbOnCurrent();     // 缩略图横向滚动让当前页居中（绿色指示框固定中央）
    int VerticalContentH() const;    // 下拉模式所有页面总高度
    int VerticalMaxScroll() const;   // 下拉模式最大滚动值
    void LayoutVScroll();            // 计算竖向滚动条轨道与滑块几何
    bool HitVScroll(int x, int y) const;  // 命中竖向滚动条滑块

    // ---- 解码 ----
    void WorkerLoop();           // 后台解码线程
    void UpdateDecodeSet();
    ID2D1Bitmap* BitmapFor(int index, ID2D1RenderTarget* rt);
    void DrawImageIn(ID2D1RenderTarget* rt, int index, const D2D1_RECT_F& dst);

    // ---- 缩略图缓存 ----
    // 独立于 decoded_ 的缩略图缓存：打开漫画时后台一次性生成所有页缩略图
    // 采样时直接缩到最终显示尺寸（等比 80×110 内），阅读器关闭前不淘汰
    void ThumbGenLoop();
    // 取缩略图位图（绑定当前 rt），通过 w/h 输出实际像素尺寸用于等比适配绘制
    ID2D1Bitmap* ThumbBitmapFor(int index, ID2D1RenderTarget* rt, int& w, int& h);

    // ---- 绘制 ----
    void DrawToolbar(ID2D1RenderTarget* rt);
    void DrawView(ID2D1RenderTarget* rt);
    void DrawSinglePage(ID2D1RenderTarget* rt);
    void DrawDoublePage(ID2D1RenderTarget* rt);
    void DrawVertical(ID2D1RenderTarget* rt);
    void DrawThumbnails(ID2D1RenderTarget* rt);
    void DrawProgressBar(ID2D1RenderTarget* rt);
    void DrawFloatButtons(ID2D1RenderTarget* rt);
    void DrawGridPanel(ID2D1RenderTarget* rt);   // 全页缩略图侧边栏（顶部网格按钮触发）

    // ---- 几何 / 命中 ----
    void ComputeRects();
    int HitToolbarButton(int x, int y) const;
    void LayoutThumbs();
    int HitThumb(int x, int y) const;
    bool OnSlider(int x, int y) const;
    void UpdateSlider(int x);
    void UpdateScaleFromToolSlider(int x);
    void LayoutGrid();                          // 计算侧边栏网格缩略图布局
    int HitGrid(int x, int y) const;            // 命中侧边栏缩略图（返回页号，-1 未命中）
    bool OnGridScroll(int x, int y) const;      // 命中侧边栏滚动条

    // ---- 工具栏按钮 id ----
    enum { kBtnSingle = 1, kBtnDrop, kBtnThumb, kBtnWheel, kBtnFit, kBtnGrid };

    // 数据
    ComicLibraryService* service_ = nullptr;
    ComicModel comic_;
    std::vector<std::string> pages_;   // 页路径
    std::vector<int> pw_, ph_;         // 页原始宽高
    std::atomic<bool> loading_{ false };
    std::atomic<bool> loaded_{ false };
    int current_ = 0;
    ReaderMode mode_ = ReaderMode::Single;
    bool rtl_ = false;

    // 单页视图状态
    float scale_ = 1.0f;
    float panX_ = 0, panY_ = 0;
    bool wheelZoom_ = false;

    // 下拉视图状态
    int verticalScroll_ = 0;

    // 缩略图
    bool showThumbs_ = true;        // 总开关（工具栏按钮）
    bool thumbsVisible_ = false;    // 实际显隐（鼠标到底部自动浮出，离开延时隐藏）
    float thumbAlpha_ = 0.0f;       // 缩略图面板透明度（淡入淡出动画，0~1）
    int thumbAnimDir_ = 0;          // 淡入淡出方向：0停 1淡入 -1淡出
    int thumbScroll_ = 0;
    bool thumbInited_ = false;     // 缩略图是否已初始化居中（加载完成后首帧主线程执行）
    std::vector<D2D1_RECT_F> thumbRects_;

    // 交互
    bool dragging_ = false;
    int dragType_ = 0;   // 0 无 1 平移 2 滑条 3 缩略图横向滚动 4 工具栏缩放滑条 5 竖向滚动条拖拽 6 网格侧边栏滚动条拖拽
    POINT dragStart_{};
    float panStartX_ = 0, panStartY_ = 0;
    int sliderStart_ = 0;
    int vScrollStart_ = 0;   // 竖向滚动条拖拽起始 verticalScroll_
    int hoverBtn_ = -1;
    int hoverThumb_ = -1;
    bool hoverPrev_ = false, hoverNext_ = false, hoverPrevF_ = false, hoverNextF_ = false;
    bool hoverVScroll_ = false;   // 竖向滚动条悬停
    D2D1_RECT_F btnSingle_, btnDrop_, btnThumb_, toolSliderRect_, sliderRect_, thumbRect_, btnWheel_, btnFit_, btnGrid_;
    D2D1_RECT_F prevBtn_, nextBtn_, prevFloat_, nextFloat_;
    D2D1_RECT_F percentRect_;
    D2D1_RECT_F vTrack_, vThumb_;   // 下拉模式右侧竖向滚动条轨道与滑块

    // 全页缩略图侧边栏（顶部网格按钮触发，浮于视图区上方，不遮挡上下工具栏）
    bool gridOpen_ = false;             // 侧边栏是否展开
    int gridScroll_ = 0;                // 网格竖向滚动偏移
    int gridHover_ = -1;                // 悬停的网格项页号
    bool gridHoverBtn_ = false;         // 网格按钮悬停
    bool gridDragScroll_ = false;       // 正在拖拽网格滚动条
    int gridScrollStart_ = 0;           // 拖拽起始滚动值
    float gridMaxScroll_ = 0;           // 网格最大滚动量（LayoutGrid 计算供滚轮/拖拽复用）
    std::vector<D2D1_RECT_F> gridRects_;  // 网格项几何（按页号）
    D2D1_RECT_F gridPanelRect_;         // 侧边栏整体区域
    D2D1_RECT_F gridTrack_, gridSlider_;// 网格滚动条轨道与滑块

    // 解码缓存（像素缓冲 + 绑定当前渲染目标的位图）
    struct Decoded { std::vector<unsigned char> px; int w = 0, h = 0; long long last = 0; };
    std::map<int, Decoded> decoded_;
    std::map<int, ID2D1Bitmap*> bitmaps_;
    ID2D1RenderTarget* bitmapRt_ = nullptr;
    std::mutex decMutex_;
    std::thread decThread_;
    std::condition_variable decCv_;
    std::vector<int> decQueue_;
    std::atomic<bool> decStop_{ false };
    long long tick_ = 0;
    static constexpr long long kMaxDecodedPixels = 60000000; // 解码缓存像素上限（约 6 页满宽）

    // 缩略图缓存（独立于原图解码缓存，常驻不淘汰，阅读器关闭时统一释放）
    struct ThumbImg { std::vector<unsigned char> px; int w = 0, h = 0; };
    std::map<int, ThumbImg> thumbCache_;            // 像素缓冲（按页号）
    std::map<int, ID2D1Bitmap*> thumbBmps_;         // 绑定当前 rt 的位图（按页号）
    ID2D1RenderTarget* thumbBmpRt_ = nullptr;       // 当前绑定的渲染目标
    std::mutex thumbMutex_;                         // 保护 thumbCache_ / thumbBmps_
    std::thread thumbThread_;                       // 缩略图生成线程
    std::atomic<bool> thumbGenStop_{ false };

    // 解码活动状态（跨线程更新，工具栏右侧显示，仿照图片浏览器"预解码:完成:xxx.jpg WxH"）
    std::wstring decodeActivity_;   // 最近一次解码活动文本
    std::mutex actMutex_;           // 保护 decodeActivity_（后台线程写、UI 线程读）

    std::function<void()> onClosed_;  // 窗口销毁回调（主窗口刷新进度用）
};

} // namespace ark::ui