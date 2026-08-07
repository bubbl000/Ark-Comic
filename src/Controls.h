#pragma once
// 轻量自绘控件：文本框 + 按钮（供对话框使用）
#include <d2d1.h>
#include <string>
#include "Ui.h"
#include "Theme.h"

namespace ark::ui {

// 单行可编辑文本框
struct TextBox {
    D2D1_RECT_F rect{};
    std::wstring text;
    int caret = 0;
    bool focused = false;
    bool readOnly = false;
    D2D1_COLOR_F textColor = theme::TextPrimary();

    void Draw(ID2D1RenderTarget* rt) {
        D2D::RoundedRect(rt, rect, 6, theme::BgCard(), focused ? theme::AccentCyan() : theme::BorderColor(),
                         focused ? 1.5f : 1.0f);
        // 文本
        D2D1_RECT_F tr{ rect.left + 8, rect.top, rect.right - 8, rect.bottom };
        D2D::Text(rt, text, tr, readOnly ? theme::TextSecondary() : textColor, 13,
                  DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // 光标
        if (focused) {
            std::wstring before = text.substr(0, (size_t)caret);
            float cx = 8 + D2D::TextWidth(before, 13);
            float x = rect.left + cx;
            float y = (rect.top + rect.bottom) / 2 - 7;
            ID2D1SolidColorBrush* b = nullptr;
            rt->CreateSolidColorBrush(theme::AccentCyan(), &b);
            if (b) {
                rt->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x, y + 14), b, 1.0f);
                b->Release();
            }
        }
    }

    bool Hit(int x, int y) const {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }
    void PlaceCaret(int x) {
        // 按字符宽度定位光标
        std::wstring s = text;
        // 简单估算：逐字符累加宽度
        float px = 8;
        size_t best = 0;
        for (size_t i = 0; i < s.size(); i++) {
            float w = D2D::TextWidth(s.substr(0, i + 1), 13);
            if (w + 8 <= (float)(x - (int)rect.left)) best = i + 1;
        }
        caret = (int)best;
        if (caret < 0) caret = 0;
        if (caret > (int)text.size()) caret = (int)text.size();
    }
};

// 按钮
struct UiButton {
    D2D1_RECT_F rect{};
    std::wstring label;
    bool accent = false;  // 柠檬绿底黑字
    bool hover = false;
    bool enabled = true;
    bool visible = true;

    void Draw(ID2D1RenderTarget* rt) {
        if (!visible) return;
        D2D1_COLOR_F bg = accent ? theme::AccentCyan() : theme::BgCard();
        if (hover && enabled) {
            if (accent) bg = theme::AccentCyan();
            else bg = theme::BgCardHover();
        }
        D2D::RoundedRect(rt, rect, 6, bg, theme::BorderColor(), accent ? 0 : 1.0f);
        D2D1_COLOR_F fg = accent ? theme::AccentText() : theme::TextPrimary();
        if (!enabled) D2D::Text(rt, label, rect, D2D1::ColorF(0.5f, 0.5f, 0.5f), 13,
                                DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        else D2D::Text(rt, label, rect, fg, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    bool Hit(int x, int y) const {
        return visible && enabled && x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }
};

} // namespace ark::ui