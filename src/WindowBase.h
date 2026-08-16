#pragma once
#include <windows.h>
#include <shellapi.h>
#include <d2d1.h>
#include <string>

namespace ark::ui {

// 无边框圆角窗口基类。
// 主窗口（可缩放+任务栏+36px标题栏）；对话框（不可缩放，工具窗口）。
class WindowBase {
public:
    WindowBase(int width, int height, const std::wstring& title, bool showClose = true);
    virtual ~WindowBase();
    bool Create(HWND owner, bool show = true);
    int RunModal();
    void Close();
    HWND Hwnd() const { return hwnd_; }
    int Width() const { return width_; }
    int Height() const { return height_; }
    void Reposition(int x, int y);
    void Invalidate();

    // 标题栏高度
    static constexpr int TitleBarHeight = 36;

protected:
    virtual void OnPaint(ID2D1RenderTarget* rt, int w, int h) = 0;
    virtual void OnLButtonDown(int x, int y) {}
    virtual void OnLButtonUp(int x, int y) {}
    virtual void OnDoubleClick(int x, int y) {}
    virtual void OnRButtonDown(int x, int y) {}
    virtual void OnRButtonUp(int x, int y) {}
    virtual void OnMouseMove(int x, int y) {}
    virtual void OnMouseLeave() {}
    virtual void OnKeyDown(UINT vk) {}
    virtual void OnChar(wchar_t ch) {}
    virtual void OnResize(int w, int h) {}
    virtual void OnTimer(UINT id) {}
    virtual void OnMouseWheel(int delta) {}
    // 拖拽释放（WM_DROPFILES，HDROP 句柄）
    virtual void OnDropFiles(HDROP drop) {}
    // 窗口激活（WM_ACTIVATE WA_ACTIVE，如从阅读器等子窗口切回）
    virtual void OnActivate() {}
    // 关闭请求（点关闭按钮 / WM_CLOSE）：返回 true 继续关闭，false 取消（用于托盘隐藏等）
    virtual bool OnCloseRequested() { return true; }
    // 窗口销毁后回调（WM_DESTROY）。若要在关闭后自动释放对象，设 selfDelete_ = true
    virtual void OnDestroy() {}
    // 自定义消息（msg >= WM_APP，如托盘通知）
    virtual void OnAppMessage(UINT msg, WPARAM wParam, LPARAM lParam) {}
    // 子控件通知（WM_COMMAND：EDIT 的 EN_CHANGE 等）
    virtual void OnCommand(int id, int code) {}
    // WM_CTLCOLOREDIT/STATIC：设置子 EDIT 文字/背景色，返回背景画刷（默认深色主题，可覆盖）
    virtual HBRUSH OnEditColor(HDC hdc, HWND edit);
    // 创建深色主题子 EDIT 控件（rect 为客户区坐标）；返回句柄，失败返回 nullptr
    HWND CreateEdit(const D2D1_RECT_F& rect, UINT id, const wchar_t* text = L"");
    // 标题栏左侧内容（默认绘制居中标题文字）
    virtual void DrawTitleBarContent(ID2D1RenderTarget* rt, int w, int h);

    void SetTitle(const std::wstring& t) { title_ = t; }
    const std::wstring& Title() const { return title_; }
    void SetShowTitleBar(bool b) { showTitleBar_ = b; }
    // 显示最小化/最大化按钮（默认仅关闭按钮）
    void SetTitleButtons(bool showMin, bool showMax) { showMin_ = showMin; showMax_ = showMax; }
    // 主窗口可缩放 + 任务栏；对话框默认不可缩放
    void SetResizable(bool b) { resizable_ = b; }
    void SetMinSize(int w, int h) { minW_ = w; minH_ = h; }
    bool IsMaximized() const { return maximized_; }
    void ToggleMaximize();
    void Minimize();
    // 标题栏按钮命中
    bool HitCloseButton(int x, int y) const;
    bool HitMaxButton(int x, int y) const;
    bool HitMinButton(int x, int y) const;
    // 定时器（防抖等）
    void StartTimer(UINT id, UINT ms) { if (hwnd_) SetTimer(hwnd_, id, ms, nullptr); }
    void KillTimerEx(UINT id) { if (hwnd_) KillTimer(hwnd_, id); }

    int width_ = 0;
    int height_ = 0;
    bool selfDelete_ = false;   // WM_NCDESTROY 时自动 delete this

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    void Paint();
    void UpdateRegion();

    HWND hwnd_ = nullptr;
    int minW_ = 0, minH_ = 0;
    std::wstring title_;
    bool showClose_ = true;
    bool showTitleBar_ = true;
    bool showMin_ = false, showMax_ = false;
    bool resizable_ = false;
    bool maximized_ = false;
    bool hoveringClose_ = false, hoveringMin_ = false, hoveringMax_ = false;
    ID2D1HwndRenderTarget* rt_ = nullptr;  // 复用渲染目标，避免每次 Paint 重建导致卡顿
};

} // namespace ark::ui