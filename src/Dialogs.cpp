#include "Dialogs.h"
#include "Ui.h"
#include "Theme.h"
#include <windowsx.h>

namespace ark::ui {

// ==================== 文本输入对话框 ====================
InputDialog::InputDialog(HWND owner, const std::wstring& title, const std::wstring& label,
                         const std::wstring& initial)
    : WindowBase(420, 200, title, true), owner_(owner), label_(label) {
    box_.rect = D2D1::RectF(20, 70, 400, 102);
    box_.text = initial;
    box_.caret = (int)box_.text.size();
    float by = 200 - 24 - 32;
    okBtn_.rect = D2D1::RectF(316, by, 400, by + 32);
    okBtn_.label = L"确定";
    okBtn_.accent = true;
    cancelBtn_.rect = D2D1::RectF(224, by, 308, by + 32);
    cancelBtn_.label = L"取消";
}

bool InputDialog::Run() {
    Create(owner_, true);
    if (Hwnd()) {
        SetForegroundWindow(Hwnd());
        SetFocus(Hwnd());
        box_.focused = true; // 默认聚焦输入框，打开即可直接输入
        Invalidate();
    }
    RunModal();
    return confirmed_;
}

void InputDialog::TryConfirm() {
    Text = box_.text;
    confirmed_ = true;
    Close();
}

void InputDialog::OnPaint(ID2D1RenderTarget* rt, int w, int h) {
    D2D1_RECT_F lab{ 20, 44, 400, 66 };
    D2D::Text(rt, label_, lab, theme::TextSecondary(), 12);
    box_.Draw(rt);
    cancelBtn_.Draw(rt);
    okBtn_.Draw(rt);
}

void InputDialog::OnLButtonDown(int x, int y) {
    if (okBtn_.Hit(x, y)) { TryConfirm(); return; }
    if (cancelBtn_.Hit(x, y)) { Close(); return; }
    if (box_.Hit(x, y)) { box_.focused = true; Invalidate(); }
}

void InputDialog::OnMouseMove(int x, int y) {
    bool o = okBtn_.Hit(x, y); if (o != okBtn_.hover) { okBtn_.hover = o; Invalidate(); }
    bool c = cancelBtn_.Hit(x, y); if (c != cancelBtn_.hover) { cancelBtn_.hover = c; Invalidate(); }
}

void InputDialog::OnKeyDown(UINT vk) {
    if (vk == VK_RETURN) { TryConfirm(); return; }
    if (vk == VK_ESCAPE) { Close(); return; }
    if (box_.focused) {
        if (vk == VK_BACK && !box_.text.empty()) {
            box_.text.pop_back();
            box_.caret = (int)box_.text.size();
            Invalidate();
        }
    }
}

void InputDialog::OnChar(wchar_t ch) {
    if (!box_.focused || ch < 32) return;
    box_.text += ch;
    box_.caret = (int)box_.text.size();
    Invalidate();
}

// ==================== 下拉菜单 ====================
// item.label 为空表示分隔线；选中项返回其 tag，点空白/关闭返回 -1。
PopupMenu::PopupMenu(int x, int y, std::vector<Item> items)
    : WindowBase(220, (int)items.size() * 38 + 8, L"", false), items_(std::move(items)) {
    SetShowTitleBar(false);
    // 计算尺寸与各项 rect（分隔线占用更小高度）
    int ih = 34;      // 普通项高度
    int sepH = 10;    // 分隔线高度
    int ww = 220;
    int yc = 4;
    itemRects_.resize(items_.size());
    for (size_t i = 0; i < items_.size(); i++) {
        bool sep = items_[i].label.empty();
        int th = sep ? sepH : ih;
        itemRects_[i] = D2D1::RectF(4, (float)yc, (float)(ww - 4), (float)(yc + th));
        yc += th;
    }
    yc += 4;
    width_ = ww;
    height_ = yc;
    // 屏幕内适配
    RECT wr{ x, y, x + ww, y + height_ };
    HMONITOR mon = MonitorFromPoint(POINT{ x, y }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
        const RECT& rc = mi.rcWork;
        if (x + ww > rc.right) x = rc.right - ww;
        if (y + height_ > rc.bottom) y = rc.bottom - height_;
        if (x < rc.left) x = rc.left;
        if (y < rc.top) y = rc.top;
    }
    posX_ = x; posY_ = y;
}

int PopupMenu::Run() {
    Create(nullptr, true);
    if (Hwnd()) {
        Reposition(posX_, posY_);
        // 置顶（TOPMOST）+ 激活：任务栏/托盘区是 topmost 性质，自绘菜单必须 TOPMOST 才能显示在其上
        SetWindowPos(Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(Hwnd());
        // 捕获鼠标以接收外部点击关闭
        SetCapture(Hwnd());
        SetFocus(Hwnd());
    }
    RunModal();
    return result_;
}

void PopupMenu::OnPaint(ID2D1RenderTarget* rt, int w, int h) {
    D2D1_RECT_F bg{ 0.5f, 0.5f, (float)w - 0.5f, (float)h - 0.5f };
    D2D::RoundedRect(rt, bg, 8, theme::BgCard(), theme::BorderColor(), 1.0f);
    for (size_t i = 0; i < items_.size(); i++) {
        auto& rc = itemRects_[i];
        if (items_[i].label.empty()) {
            // 分隔线
            float mid = (rc.top + rc.bottom) / 2;
            D2D1_RECT_F line{ rc.left + 12, mid, rc.right - 12, mid + 1 };
            D2D::RoundedRect(rt, line, 0, theme::BorderColor());
            continue;
        }
        if ((int)i == hover_) D2D::RoundedRect(rt, rc, 4, theme::BgCardHover());
        D2D1_RECT_F tr{ rc.left + 12, rc.top, rc.right - 8, rc.bottom };
        D2D::Text(rt, items_[i].label, tr, theme::TextPrimary(), 13, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

void PopupMenu::OnLButtonDown(int x, int y) {
    for (size_t i = 0; i < itemRects_.size(); i++) {
        auto& rc = itemRects_[i];
        if (items_[i].label.empty()) continue; // 分隔线不可点
        if (x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom) {
            result_ = items_[i].tag;
            Close();
            return;
        }
    }
    // 点空白关闭
    result_ = -1;
    Close();
}

void PopupMenu::OnMouseMove(int x, int y) {
    int h = -1;
    for (size_t i = 0; i < itemRects_.size(); i++) {
        if (items_[i].label.empty()) continue;
        auto& rc = itemRects_[i];
        if (x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom) { h = (int)i; break; }
    }
    if (h != hover_) { hover_ = h; Invalidate(); }
}

void PopupMenu::OnMouseLeave() {
    if (hover_ != -1) { hover_ = -1; Invalidate(); }
}

void PopupMenu::OnKeyDown(UINT vk) {
    if (vk == VK_ESCAPE) { result_ = -1; Close(); }
}

} // namespace ark::ui