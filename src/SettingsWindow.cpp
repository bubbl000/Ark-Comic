#include "SettingsWindow.h"
#include "Ui.h"
#include "Theme.h"
#include "AppConfig.h"
#include "LibraryManager.h"
#include "MsgBox.h"
#include "PickFolder.h"
#include "FileUtil.h"
#include "Utf.h"
#include "I18n.h"
#include <cctype>
#include <cstdio>

namespace ark::ui {

SettingsWindow::SettingsWindow(HWND owner)
    : WindowBase(560, 720, i18n::Tr(L"设置", L"Settings"), true), ownerWnd_(owner) {
    presets_[0] = 0xCBE93A; presets_[1] = 0x54D1DB; presets_[2] = 0xE89F3A; presets_[3] = 0xB07CF0;
    presetNames_[0] = i18n::Tr(L"柠檬绿", L"Lemon Green"); presetNames_[1] = i18n::Tr(L"青蓝", L"Cyan");
    presetNames_[2] = i18n::Tr(L"橙色", L"Orange"); presetNames_[3] = i18n::Tr(L"紫色", L"Purple");

    auto& cfg = AppConfig::Instance();
    closeToTray_ = cfg.closeToTray;
    // 匹配当前主题色到预设
    std::string low = cfg.themeColor;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    selectedPreset_ = -1;
    for (int i = 0; i < 4; i++) {
        char buf[8]; sprintf_s(buf, "#%06X", presets_[i]);
        std::string ps = buf;
        for (auto& c : ps) c = (char)tolower((unsigned char)c);
        if (low == ps) { selectedPreset_ = i; break; }
    }
    customBox_.text = W(theme::CurrentAccentHex());
    customBox_.caret = (int)customBox_.text.size();
    SyncRows();
    Layout();
}

void SettingsWindow::Run() {
    // 真正模态：禁用主窗口，防止重复打开设置弹窗
    if (ownerWnd_) EnableWindow(ownerWnd_, FALSE);
    Create(ownerWnd_, true);
    if (Hwnd()) SetFocus(Hwnd());
    RunModal();
    if (ownerWnd_) {
        EnableWindow(ownerWnd_, TRUE);
        SetForegroundWindow(ownerWnd_);
    }
}

void SettingsWindow::SyncRows() {
    rows_.clear();
    const auto& libs = AppConfig::Instance().libraries;
    for (int i = 0; i < 3; i++) {
        LibRow r;
        if (i < (int)libs.size()) {
            r.name.text = W(libs[i].name);
            r.name.caret = (int)r.name.text.size();
            r.name.readOnly = true;   // 已配置库的名称仅展示，不可编辑
            r.pathText = W(libs[i].path);
            r.configured = true;
        } else {
            r.name.readOnly = false;
        }
        rows_.push_back(r);
    }
    focusedRow_ = -1;
}

void SettingsWindow::Layout() {
    int t = TitleBarHeight;
    // 分页标签
    tabGeneral_ = D2D1::RectF(28, (float)(t + 18), 200, (float)(t + 46));
    tabLibrary_ = D2D1::RectF(210, (float)(t + 18), 382, (float)(t + 46));

    // 常规设置
    sepTheme_ = D2D1::RectF(28, (float)(t + 66), 532, (float)(t + 67));
    themeTitle_ = D2D1::RectF(28, (float)(t + 82), 300, (float)(t + 102));
    themeDesc_ = D2D1::RectF(28, (float)(t + 106), 532, (float)(t + 124));
    int sx[4] = { 28, 156, 284, 412 };
    swatchRects_.clear();
    for (int i = 0; i < 4; i++)
        swatchRects_.push_back(D2D1::RectF((float)sx[i], (float)(t + 134), (float)(sx[i] + 20), (float)(t + 154)));
    customLabel_ = D2D1::RectF(28, (float)(t + 172), 100, (float)(t + 198));
    customBox_.rect = D2D1::RectF(100, (float)(t + 172), 180, (float)(t + 198));
    sepClose_ = D2D1::RectF(28, (float)(t + 210), 532, (float)(t + 211));
    closeTitle_ = D2D1::RectF(28, (float)(t + 226), 300, (float)(t + 246));
    closeDesc_ = D2D1::RectF(28, (float)(t + 250), 532, (float)(t + 268));
    radioRects_.clear();
    radioRects_.push_back(D2D1::RectF(28, (float)(t + 282), 48, (float)(t + 302)));
    radioRects_.push_back(D2D1::RectF(28, (float)(t + 314), 48, (float)(t + 334)));
    radioNote_ = D2D1::RectF(258, (float)(t + 282), 532, (float)(t + 302));

    // 资源库设置
    libTitle_ = D2D1::RectF(28, (float)(t + 66), 300, (float)(t + 86));
    libDesc_ = D2D1::RectF(28, (float)(t + 90), 532, (float)(t + 126));
    int ry = t + 150;
    for (int i = 0; i < (int)rows_.size(); i++) {
        int top = ry + i * 52;
        rows_[i].name.rect = D2D1::RectF(28, (float)top, 260, (float)(top + 36));
        rows_[i].btn = D2D1::RectF(272, (float)top, 344, (float)(top + 36));
        rows_[i].path = D2D1::RectF(352, (float)top, 532, (float)(top + 36));
    }

    okRect_ = D2D1::RectF(560 - 28 - 96, (float)(720 - 24 - 32), 560 - 28, (float)(720 - 24));
}

void SettingsWindow::SelectPreset(int i) {
    selectedPreset_ = i;
    customFocused_ = false; customBox_.focused = false;
    theme::SetAccentColor(presets_[i]);
    customBox_.text = W(theme::CurrentAccentHex());
    customBox_.caret = (int)customBox_.text.size();
    Invalidate();
    if (ownerWnd_) InvalidateRect(ownerWnd_, nullptr, FALSE);
}

void SettingsWindow::ApplyCustom() {
    std::string s = U8(customBox_.text);
    if (s.empty()) return;
    if (theme::SetAccentHex(s)) {
        selectedPreset_ = -1;
        Invalidate();
        if (ownerWnd_) InvalidateRect(ownerWnd_, nullptr, FALSE);
    }
}

void SettingsWindow::OnLibButton(int row) {
    if (row < 0 || row >= (int)rows_.size()) return;
    auto& r = rows_[row];
    if (r.configured) {
        // 移除：仅从软件记录中移除，不删除磁盘上的资源库文件夹
        LibraryManager::RemoveLibrary(U8(r.pathText));
        libsChanged_ = true;
    } else {
        // 选择：挑选根文件夹；若其中已有 .info 资源库则直接加入，否则用名称新建
        std::wstring root;
        std::wstring dlgTitle = i18n::Tr(L"选择资源库所在文件夹", L"Select the library folder");
        if (!PickFolder(Hwnd(), dlgTitle.c_str(), root)) return;
        LibraryManager::OpenOrCreateLibrary(U8(root), U8(r.name.text));
        libsChanged_ = true;
    }
    SyncRows();
    Layout();
    Invalidate();
}

void SettingsWindow::TryConfirm() {
    // 常规页校验颜色；资源库页跳过颜色校验（资源库操作已即时保存）
    if (tab_ == 0) {
        std::string s = U8(customBox_.text);
        if (selectedPreset_ == -1 && !theme::SetAccentHex(s)) {
            ShowMsgBox(Hwnd(), i18n::Tr(L"颜色格式应为 #RRGGBB", L"Color format must be #RRGGBB"), i18n::Tr(L"提示", L"Notice"));
            return;
        }
    }
    auto& cfg = AppConfig::Instance();
    cfg.themeColor = theme::CurrentAccentHex();
    cfg.closeToTray = closeToTray_;
    cfg.Save();
    ok_ = true;
    Close();
}

bool SettingsWindow::OnCloseRequested() {
    if (!ok_) {
        // 未点确定：还原为已保存的主题色
        theme::SetAccentHex(AppConfig::Instance().themeColor);
        if (ownerWnd_) InvalidateRect(ownerWnd_, nullptr, FALSE);
    }
    return true;
}

void SettingsWindow::PaintTabs(ID2D1RenderTarget* rt) {
    // 标签文字：选中用强调色加粗，未选次级色
    auto drawTab = [&](int idx, const std::wstring& text, const D2D1_RECT_F& r, bool hover) {
        bool sel = tab_ == idx;
        D2D1_COLOR_F fc = sel ? theme::AccentCyan() : (hover ? theme::TextPrimary() : theme::TextSecondary());
        D2D::Text(rt, text, r, fc, 14,
                  sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (sel) {
            ID2D1SolidColorBrush* b = nullptr;
            rt->CreateSolidColorBrush(theme::AccentCyan(), &b);
            if (b) {
                rt->FillRectangle(D2D1::RectF(r.left, r.bottom + 4, r.right, r.bottom + 6), b);
                b->Release();
            }
        }
    };
    drawTab(0, i18n::Tr(L"常规设置", L"General"), tabGeneral_, hoverTabGeneral_);
    drawTab(1, i18n::Tr(L"资源库设置", L"Library"), tabLibrary_, hoverTabLibrary_);
    // 标签下沿分隔线
    D2D::RoundedRect(rt, D2D1::RectF(28, tabGeneral_.bottom + 10, 532, tabGeneral_.bottom + 11), 0, theme::BorderColor());
}

void SettingsWindow::PaintGeneral(ID2D1RenderTarget* rt) {
    int t = TitleBarHeight;
    // 主题色区
    D2D::RoundedRect(rt, sepTheme_, 0, theme::BorderColor());
    D2D::Text(rt, i18n::Tr(L"主题色", L"Accent Color"), themeTitle_, theme::TextPrimary(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    D2D::Text(rt, i18n::Tr(L"选择软件的主题强调色", L"Choose the app's accent color"), themeDesc_, theme::TextSecondary(), 12);
    for (int i = 0; i < 4; i++) {
        auto& r = swatchRects_[i];
        D2D1_POINT_2F c{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        ID2D1SolidColorBrush* br = nullptr;
        rt->CreateSolidColorBrush(theme::rgb(presets_[i]), &br);
        if (br) { rt->FillEllipse(D2D1::Ellipse(c, 9, 9), br); br->Release(); }
        if (selectedPreset_ == i) {
            rt->CreateSolidColorBrush(theme::TextPrimary(), &br);
            if (br) { rt->DrawEllipse(D2D1::Ellipse(c, 12, 12), br, 1.5f); br->Release(); }
        }
        D2D1_RECT_F nr{ r.right + 8, r.top - 2, r.right + 70, r.bottom + 2 };
        D2D::Text(rt, presetNames_[i], nr, theme::TextSecondary(), 12);
    }
    // 自定义
    D2D::Text(rt, i18n::Tr(L"自定义:", L"Custom:"), customLabel_, theme::TextSecondary(), 12);
    customBox_.focused = customFocused_ || selectedPreset_ == -1;
    customBox_.Draw(rt);

    // 关闭行为
    D2D::RoundedRect(rt, sepClose_, 0, theme::BorderColor());
    D2D::Text(rt, i18n::Tr(L"关闭行为", L"Close Behavior"), closeTitle_, theme::TextPrimary(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    D2D::Text(rt, i18n::Tr(L"选择关闭窗口时的行为", L"Choose what happens when the window is closed"), closeDesc_, theme::TextSecondary(), 12);
    for (int i = 0; i < 2; i++) {
        auto& r = radioRects_[i];
        D2D1_POINT_2F c{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        bool sel = (i == 0) ? closeToTray_ : !closeToTray_;
        ID2D1SolidColorBrush* br = nullptr;
        if (sel) {
            rt->CreateSolidColorBrush(theme::AccentCyan(), &br);
            if (br) { rt->FillEllipse(D2D1::Ellipse(c, 9, 9), br); br->Release(); }
            rt->CreateSolidColorBrush(theme::TextPrimary(), &br);
            if (br) { rt->FillEllipse(D2D1::Ellipse(c, 3.5f, 3.5f), br); br->Release(); }
        } else {
            rt->CreateSolidColorBrush(theme::BorderColor(), &br);
            if (br) { rt->DrawEllipse(D2D1::Ellipse(c, 9, 9), br, 1.5f); br->Release(); }
        }
    }
    D2D::Text(rt, i18n::Tr(L"最小化到系统托盘", L"Minimize to system tray"), D2D1::RectF(60, radioRects_[0].top, 250, radioRects_[0].bottom),
              theme::TextPrimary(), 12);
    D2D::Text(rt, i18n::Tr(L"（推荐，后台保持运行）", L"(Recommended, keeps running in background)"), radioNote_, theme::TextSecondary(), 11);
    D2D::Text(rt, i18n::Tr(L"完全退出程序", L"Exit the app completely"), D2D1::RectF(60, radioRects_[1].top, 400, radioRects_[1].bottom),
              theme::TextPrimary(), 12);
    (void)t;
}

void SettingsWindow::PaintLibrary(ID2D1RenderTarget* rt) {
    D2D::Text(rt, i18n::Tr(L"资源库设置", L"Library Settings"), libTitle_, theme::TextPrimary(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    D2D::Text(rt, i18n::Tr(L"最多可设置 3 个资源库。选择文件夹并填入名称即创建；移除仅从软件移除，不删除文件夹。",
                           L"Up to 3 libraries. Pick a folder and enter a name to create; removing only removes it from the app, not from disk."),
              libDesc_, theme::TextSecondary(), 12);
    for (int i = 0; i < (int)rows_.size(); i++) {
        auto& r = rows_[i];
        // 名称输入框
        r.name.focused = (tab_ == 1 && focusedRow_ == i);
        r.name.Draw(rt);
        // 按钮：未配置=选择，已配置=移除
        D2D1_COLOR_F bg = r.configured ? D2D1::ColorF(0.35f, 0.12f, 0.12f) : theme::AccentCyan();
        if (hoverRowBtn_ == i) {
            if (r.configured) bg = D2D1::ColorF(0.5f, 0.18f, 0.18f);
            else bg = theme::AccentCyan(), bg.a = 0.85f;
        }
        D2D::RoundedRect(rt, r.btn, 6, bg);
        D2D1_COLOR_F fc = r.configured ? D2D1::ColorF(1, 1, 1) : theme::AccentText();
        D2D::Text(rt, r.configured ? i18n::Tr(L"移除", L"Remove") : i18n::Tr(L"选择", L"Select"), r.btn, fc, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // 路径（单行省略）
        std::wstring path = r.configured ? r.pathText : i18n::Tr(L"未创建", L"Not created");
        float avail = r.path.right - r.path.left;
        while (path.size() > 1 && D2D::TextWidth(path, 12) > avail) path.pop_back();
        D2D1_COLOR_F pc = r.configured ? theme::TextSecondary() : theme::TextSecondary();
        D2D::Text(rt, path, r.path, pc, 12, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

void SettingsWindow::OnPaint(ID2D1RenderTarget* rt, int w, int h) {
    PaintTabs(rt);
    if (tab_ == 0) PaintGeneral(rt);
    else PaintLibrary(rt);

    // 确定按钮（悬停 0.85 透明度）
    D2D1_COLOR_F okFill = theme::AccentCyan();
    if (hoverOk_) okFill.a = 0.85f;
    D2D::RoundedRect(rt, okRect_, 6, okFill);
    D2D::Text(rt, i18n::Tr(L"确定", L"OK"), okRect_, theme::AccentText(), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD,
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void SettingsWindow::OnLButtonDown(int x, int y) {
    auto hit = [](const D2D1_RECT_F& r, int px, int py) {
        return px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
    };
    if (hit(okRect_, x, y)) { TryConfirm(); return; }
    // 分页切换
    if (hit(tabGeneral_, x, y)) { tab_ = 0; Layout(); Invalidate(); return; }
    if (hit(tabLibrary_, x, y)) { tab_ = 1; Layout(); Invalidate(); return; }

    if (tab_ == 0) {
        for (int i = 0; i < 4; i++)
            if (hit(swatchRects_[i], x, y)) { SelectPreset(i); return; }
        if (customBox_.Hit(x, y)) { customFocused_ = true; customBox_.focused = true; SetFocus(Hwnd()); Invalidate(); return; }
        for (int i = 0; i < 2; i++) {
            auto& r = radioRects_[i];
            if (y >= r.top && y <= r.bottom && x >= r.left - 8 && x <= r.left + 250) {
                bool want = (i == 0);
                if (closeToTray_ != want) { closeToTray_ = want; Invalidate(); }
                return;
            }
        }
        customFocused_ = false; customBox_.focused = false; Invalidate();
    } else {
        for (int i = 0; i < (int)rows_.size(); i++) {
            auto& r = rows_[i];
            if (hit(r.btn, x, y)) { OnLibButton(i); return; }
            if (!r.configured && r.name.Hit(x, y)) {
                focusedRow_ = i; r.name.focused = true; SetFocus(Hwnd()); Invalidate(); return;
            }
        }
        focusedRow_ = -1;
        for (auto& r : rows_) r.name.focused = false;
        Invalidate();
    }
}

void SettingsWindow::OnMouseMove(int x, int y) {
    auto hit = [](const D2D1_RECT_F& r, int px, int py) {
        return px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
    };
    int hs = -1;
    if (tab_ == 0)
        for (int i = 0; i < 4; i++)
            if (hit(swatchRects_[i], x, y)) hs = i;
    int hb = -1;
    if (tab_ == 1)
        for (int i = 0; i < (int)rows_.size(); i++)
            if (hit(rows_[i].btn, x, y)) hb = i;
    bool ho = hit(okRect_, x, y);
    bool htg = hit(tabGeneral_, x, y);
    bool htl = hit(tabLibrary_, x, y);
    if (hs != hoverSwatch_ || hb != hoverRowBtn_ || ho != hoverOk_ ||
        htg != hoverTabGeneral_ || htl != hoverTabLibrary_) {
        hoverSwatch_ = hs; hoverRowBtn_ = hb; hoverOk_ = ho;
        hoverTabGeneral_ = htg; hoverTabLibrary_ = htl;
        Invalidate();
    }
}

void SettingsWindow::OnMouseLeave() {
    if (hoverSwatch_ != -1 || hoverRowBtn_ != -1 || hoverOk_ || hoverTabGeneral_ || hoverTabLibrary_) {
        hoverSwatch_ = -1; hoverRowBtn_ = -1; hoverOk_ = false;
        hoverTabGeneral_ = false; hoverTabLibrary_ = false;
        Invalidate();
    }
}

void SettingsWindow::OnKeyDown(UINT vk) {
    // 资源库名称输入框
    if (tab_ == 1 && focusedRow_ >= 0 && focusedRow_ < (int)rows_.size()) {
        auto& nb = rows_[focusedRow_].name;
        if (vk == VK_BACK && !nb.text.empty()) {
            nb.text.pop_back();
            nb.caret = (int)nb.text.size();
            Invalidate();
        }
        return;
    }
    if (customFocused_) {
        if (vk == VK_BACK && !customBox_.text.empty()) {
            customBox_.text.pop_back();
            customBox_.caret = (int)customBox_.text.size();
            ApplyCustom();
            Invalidate();
        }
        return;
    }
    if (vk == VK_RETURN) { TryConfirm(); return; }
    if (vk == VK_ESCAPE) { Close(); return; }
}

void SettingsWindow::OnChar(wchar_t ch) {
    if (tab_ == 1 && focusedRow_ >= 0 && focusedRow_ < (int)rows_.size()) {
        auto& nb = rows_[focusedRow_].name;
        if (ch >= 32) {
            nb.text += ch;
            nb.caret = (int)nb.text.size();
            Invalidate();
        }
        return;
    }
    if (!customFocused_ || ch < 32) return;
    customBox_.text += ch;
    customBox_.caret = (int)customBox_.text.size();
    ApplyCustom();
    Invalidate();
}

} // namespace ark::ui