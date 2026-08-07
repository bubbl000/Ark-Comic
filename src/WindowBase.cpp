#include "WindowBase.h"
#include "Ui.h"
#include "Theme.h"
#include "ActivityLog.h"
#include "../resources/resource.h"
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>

namespace ark::ui {

static const wchar_t* kWndClassName = L"ArkComicRoundedWindow";

WindowBase::WindowBase(int width, int height, const std::wstring& title, bool showClose)
    : width_(width), height_(height), title_(title), showClose_(showClose) {}

WindowBase::~WindowBase() {
    if (rt_) { rt_->Release(); rt_ = nullptr; }
    if (hwnd_) DestroyWindow(hwnd_);
}

bool WindowBase::Create(HWND owner, bool show) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // CS_DROPSHADOW：窗口阴影（与 DWM 圆角搭配，替代 SetWindowRgn 硬裁剪）
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS | CS_DROPSHADOW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP));   // 应用图标（resources/app.ico）
        wc.hIconSm = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP)); // 小图标（标题栏/任务栏）
        wc.lpszClassName = kWndClassName;
        RegisterClassExW(&wc);
        registered = true;
    }

    DWORD ex = resizable_ ? 0 : WS_EX_TOOLWINDOW;
    // 不用 WS_THICKFRAME：它会触发 Win11 DWM 绘制系统边框（失焦时变白/浅色）。
    // 缩放由 WM_NCHITTEST 返回的调整代码处理，阴影由类样式 CS_DROPSHADOW 保留，
    // 边框由 Paint() 自绘深色。同样保留最小化/最大化（无标题栏时仅影响任务栏行为）。
    DWORD style = resizable_ ? (WS_POPUP | WS_MINIMIZEBOX | WS_MAXIMIZEBOX) : WS_POPUP;
    HWND h = CreateWindowExW(ex, kWndClassName, title_.c_str(), style,
                             CW_USEDEFAULT, CW_USEDEFAULT, width_, height_,
                             owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!h) { hwnd_ = nullptr; return false; }
    hwnd_ = h;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)this);
    // Win11 DWM 原生圆角（系统平滑抗锯齿，替代 SetWindowRgn 的 GDI 硬锯齿）
    // DWMWA_WINDOW_CORNER_PREFERENCE=33, DWMWCP_ROUND=2；旧 SDK 无定义时用数值兜底
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
    int cornerPref = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    UpdateRegion();
    if (owner) {
        // 居中到 owner
        RECT o, r;
        GetWindowRect(owner, &o);
        GetWindowRect(h, &r);
        int w = r.right - r.left, hh = r.bottom - r.top;
        int x = o.left + ((o.right - o.left) - w) / 2;
        int y = o.top + ((o.bottom - o.top) - hh) / 2;
        SetWindowPos(h, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    } else if (resizable_) {
        // 主窗口启动居中到屏幕
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        int x = (sw - width_) / 2, y = (sh - height_) / 2;
        if (x < 0) x = 0; if (y < 0) y = 0;
        SetWindowPos(h, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (show) ShowWindow(h, SW_SHOW);
    // 允许接收文件拖拽（导入漫画）
    DragAcceptFiles(h, TRUE);
    return true;
}

int WindowBase::RunModal() {
    // 模态：禁用 owner 窗口，防止弹窗期间主窗口可点击、重复弹窗；窗口销毁后恢复
    HWND owner = GetWindow(hwnd_, GW_OWNER);
    if (owner) EnableWindow(owner, FALSE);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!hwnd_) break;
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        // 弹窗关闭后把主窗口带回前台：绕过前台锁，避免落到其他程序之下
        if (IsIconic(owner)) ShowWindow(owner, SW_RESTORE);
        DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        DWORD cur = GetCurrentThreadId();
        if (fg && fg != cur) AttachThreadInput(fg, cur, TRUE);
        SetForegroundWindow(owner);
        if (fg && fg != cur) AttachThreadInput(fg, cur, FALSE);
        SetWindowPos(owner, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    return 0;
}

void WindowBase::Close() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void WindowBase::Reposition(int x, int y) {
    if (hwnd_) SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void WindowBase::Invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void WindowBase::ToggleMaximize() {
    if (!hwnd_) return;
    if (maximized_) ShowWindow(hwnd_, SW_RESTORE);
    else ShowWindow(hwnd_, SW_MAXIMIZE);
}

void WindowBase::Minimize() {
    if (hwnd_) ShowWindow(hwnd_, SW_MINIMIZE);
}

void WindowBase::UpdateRegion() {
    // 圆角由 Win11 DWM 处理（DwmSetWindowAttribute DWMWCP_ROUND），
    // 不再使用 SetWindowRgn——GDI 区域边缘是硬锯齿且与 D2D 圆角半径不一致。
    // 最大化时 DWM 自动切直角，无需处理。
    (void)hwnd_;
}

bool WindowBase::HitCloseButton(int x, int y) const {
    return showClose_ && showTitleBar_ && x >= width_ - 36 && x < width_ && y >= 0 && y < TitleBarHeight;
}
bool WindowBase::HitMaxButton(int x, int y) const {
    return showMax_ && showTitleBar_ && x >= width_ - 72 && x < width_ - 36 && y >= 0 && y < TitleBarHeight;
}
bool WindowBase::HitMinButton(int x, int y) const {
    return showMin_ && showTitleBar_ && x >= width_ - 108 && x < width_ - 72 && y >= 0 && y < TitleBarHeight;
}

void WindowBase::DrawTitleBarContent(ID2D1RenderTarget* rt, int w, int h) {
    // 标题文本居中于高度 TitleBarHeight（不能用整窗高 h，否则对角线居中会落到内容区）
    D2D1_RECT_F tr{ 14, 0, (float)w - 116, (float)TitleBarHeight };
    D2D::Text(rt, title_, tr, theme::TextPrimary(), 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

void WindowBase::Paint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);
    D2D1_SIZE_U pixelSize{ (UINT32)width_, (UINT32)height_ };
    // 复用渲染目标：CreateHwndRenderTarget 开销极大（涉及 D3D 设备/交换链），
    // 每次 Paint 重建会导致拖拽时每个 MOUSEMOVE 都卡顿，UI 跟不上光标。
    if (!rt_) {
        ID2D1Factory* f = D2D::Factory();
        if (!f || FAILED(f->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                D2D1::HwndRenderTargetProperties(hwnd_, pixelSize), &rt_))) {
            EndPaint(hwnd_, &ps);
            return;
        }
        // 显式开启逐图元抗锯齿（PER_PRIMITIVE），保证圆角矩形边缘平滑无锯齿
        rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    } else {
        // 尺寸变化时同步（WM_SIZE 已 Resize，这里兜底防漂移）
        D2D1_SIZE_U cur = rt_->GetPixelSize();
        if (cur.width != pixelSize.width || cur.height != pixelSize.height)
            rt_->Resize(pixelSize);
    }
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    // 窗口背景（圆角）+ 边框；最大化时画直角（DWM 最大化无圆角）
    float radius = maximized_ ? 0 : 12;
    D2D1_RECT_F full{ 0.5f, 0.5f, (float)width_ - 0.5f, (float)height_ - 0.5f };
    D2D::RoundedRect(rt_, full, radius, theme::BgMain(), maximized_ ? theme::BgMain() : theme::WindowBorder(), 1.0f);
    // 标题栏
    if (showTitleBar_) {
        D2D1_RECT_F tb{ 0, 0, (float)width_, (float)TitleBarHeight };
        D2D::RoundedRect(rt_, tb, radius, theme::BgTitlebar());
        // 关闭按钮
        if (showClose_) {
            D2D1_RECT_F cb{ (float)(width_ - 36), 0, (float)width_, (float)TitleBarHeight };
            if (hoveringClose_) D2D::RoundedRect(rt_, cb, 8, theme::CloseHover());
            D2D::Icon(rt_, L"\xE8BB", cb, hoveringClose_ ? D2D1::ColorF(1, 1, 1) : theme::TextSecondary(), 10);
        }
        // 最大化/还原
        if (showMax_) {
            D2D1_RECT_F rb{ (float)(width_ - 72), 0, (float)(width_ - 36), (float)TitleBarHeight };
            if (hoveringMax_) D2D::RoundedRect(rt_, rb, 8, theme::BgCardHover());
            D2D::Icon(rt_, maximized_ ? L"\xE923" : L"\xE922", rb, theme::TextSecondary(), 10);
        }
        // 最小化
        if (showMin_) {
            D2D1_RECT_F mb{ (float)(width_ - 108), 0, (float)(width_ - 72), (float)TitleBarHeight };
            if (hoveringMin_) D2D::RoundedRect(rt_, mb, 8, theme::BgCardHover());
            D2D::Icon(rt_, L"\xE921", mb, theme::TextSecondary(), 10);
        }
        // 左侧内容
        DrawTitleBarContent(rt_, width_, height_);
    }
    // 派生类内容
    OnPaint(rt_, width_, height_);

    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // 渲染目标失效：释放并在下次 Paint 重建，同时清空封面位图缓存
        rt_->Release(); rt_ = nullptr;
        D2D::ClearBitmapCache();
        Invalidate();
    }
    EndPaint(hwnd_, &ps);
}

LRESULT CALLBACK WindowBase::WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    WindowBase* self = (WindowBase*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        auto cs = (CREATESTRUCT*)l;
        self = (WindowBase*)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (self) {
        if (!self->hwnd_) self->hwnd_ = h;
        return self->HandleMessage(msg, w, l);
    }
    return DefWindowProcW(h, msg, w, l);
}

LRESULT WindowBase::HandleMessage(UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_ACTIVE) OnActivate();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCCALCSIZE:
            // 去掉系统边框的非客户区，让客户区铺满整窗（自绘边框/缩放）
            return 0;
        case WM_NCHITTEST: {
            if (maximized_) return HTCLIENT;
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            POINT pt{ x, y };
            ScreenToClient(hwnd_, &pt);
            int cx = pt.x, cy = pt.y;
            // 边缘缩放仅可缩放窗口
            if (resizable_) {
                const int edge = 6;
                bool left = cx < edge, right = cx >= width_ - edge;
                bool top = cy < edge, bottom = cy >= height_ - edge;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }
            // 标题栏拖动（所有窗口生效，包括对话框/设置弹窗）
            if (showTitleBar_ && cy < TitleBarHeight) {
                if (HitCloseButton(cx, cy) || HitMaxButton(cx, cy) || HitMinButton(cx, cy)) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)l;
            if (minW_ > 0) mmi->ptMinTrackSize.x = minW_;
            if (minH_ > 0) mmi->ptMinTrackSize.y = minH_;
            if (resizable_) {
                RECT wa;
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
                mmi->ptMaxPosition.x = wa.left;
                mmi->ptMaxPosition.y = wa.top;
                mmi->ptMaxSize.x = wa.right - wa.left;
                mmi->ptMaxSize.y = wa.bottom - wa.top;
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            SetCapture(hwnd_);
            if (HitCloseButton(x, y)) { hoveringClose_ = true; Invalidate(); return 0; }
            if (HitMaxButton(x, y)) { hoveringMax_ = true; Invalidate(); return 0; }
            if (HitMinButton(x, y)) { hoveringMin_ = true; Invalidate(); return 0; }
            OnLButtonDown(x, y);
            return 0;
        }
        case WM_LBUTTONUP: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            ReleaseCapture();
            if (HitCloseButton(x, y)) { if (OnCloseRequested()) Close(); return 0; }
            if (HitMaxButton(x, y)) { ToggleMaximize(); return 0; }
            if (HitMinButton(x, y)) { Minimize(); return 0; }
            OnLButtonUp(x, y);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            if (showTitleBar_ && y < TitleBarHeight && resizable_) {
                if (!(HitCloseButton(x, y) || HitMaxButton(x, y) || HitMinButton(x, y))) {
                    ToggleMaximize();
                    return 0;
                }
            }
            OnDoubleClick(x, y);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            OnRButtonDown(x, y);
            return 0;
        }
        case WM_RBUTTONUP: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            OnRButtonUp(x, y);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            bool c = HitCloseButton(x, y), mi = HitMinButton(x, y), ma = HitMaxButton(x, y);
            if (c != hoveringClose_ || mi != hoveringMin_ || ma != hoveringMax_) {
                hoveringClose_ = c; hoveringMin_ = mi; hoveringMax_ = ma;
                Invalidate();
            }
            OnMouseMove(x, y);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hoveringClose_ || hoveringMin_ || hoveringMax_) {
                hoveringClose_ = hoveringMin_ = hoveringMax_ = false;
                Invalidate();
            }
            OnMouseLeave();
            return 0;
        case WM_KEYDOWN:
            // F12 切换活动日志窗口显示/隐藏（任意窗口聚焦均可）
            if ((UINT)w == VK_F12) {
                ActivityLog::Instance().Toggle();
                return 0;
            }
            OnKeyDown((UINT)w);
            return 0;
        case WM_CHAR:
            OnChar((wchar_t)w);
            return 0;
        case WM_TIMER:
            OnTimer((UINT)w);
            return 0;
        case WM_MOUSEWHEEL:
            OnMouseWheel((short)HIWORD(w));
            return 0;
        case WM_DROPFILES:
            OnDropFiles((HDROP)w);
            return 0;
        case WM_SIZE: {
            int wpx = LOWORD(l), hpx = HIWORD(l);
            width_ = wpx; height_ = hpx;
            bool max = (w == SIZE_MAXIMIZED);
            if (max != maximized_) { maximized_ = max; UpdateRegion(); }
            if (rt_) rt_->Resize(D2D1::SizeU((UINT32)wpx, (UINT32)hpx));
            OnResize(wpx, hpx);
            Invalidate();
            return 0;
        }
        case WM_CLOSE:
            if (OnCloseRequested()) Close();
            return 0;
        case WM_DESTROY:
            OnDestroy();
            return 0;
        case WM_NCDESTROY: {
            // 先交系统清理窗口，再按需释放对象（此处 delete 后不再访问 this）
            LRESULT res = DefWindowProcW(hwnd_, msg, w, l);
            if (selfDelete_) {
                SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
                delete this;
            }
            return res;
        }
    }
    if (msg >= WM_APP) { OnAppMessage(msg, w, l); return 0; }
    return DefWindowProcW(hwnd_, msg, w, l);
}

} // namespace ark::ui