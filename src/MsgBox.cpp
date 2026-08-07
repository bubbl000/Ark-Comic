#include "MsgBox.h"
#include "WindowBase.h"
#include "Ui.h"
#include "Theme.h"
#include "Controls.h"
#include <dwrite.h>

namespace ark::ui {

class MsgBoxWindow : public WindowBase {
public:
    // yesNo=false：仅"确定"；yesNo=true：确定/取消
    MsgBoxWindow(const std::wstring& text, const std::wstring& title, bool yesNo)
        : WindowBase(420, 170, title, false), text_(text) {
        float bw = 84, bh = 32;
        float by = 170 - 24 - bh;
        float x = 420 - 20 - bw;
        ok_.rect = D2D1::RectF(x, by, x + bw, by + bh);
        ok_.label = L"确定";
        ok_.accent = true;
        if (yesNo) {
            x -= bw + 8;
            cancel_.rect = D2D1::RectF(x, by, x + bw, by + bh);
            cancel_.label = L"取消";
        } else {
            cancel_.visible = false;
        }
    }

protected:
    void OnPaint(ID2D1RenderTarget* rt, int w, int h) override {
        D2D1_RECT_F body{ 20, 46, (float)w - 20, (float)h - 52 };
        D2D::Text(rt, text_, body, theme::TextSecondary(), 12, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        ok_.Draw(rt);
        if (cancel_.visible) cancel_.Draw(rt);
    }

    void OnLButtonDown(int x, int y) override {
        if (ok_.Hit(x, y)) { result_ = true; Close(); return; }
        if (cancel_.Hit(x, y)) { result_ = false; Close(); }
    }
    void OnMouseMove(int x, int y) override {
        bool h = ok_.Hit(x, y); if (h != ok_.hover) { ok_.hover = h; Invalidate(); }
        bool h2 = cancel_.Hit(x, y); if (h2 != cancel_.hover) { cancel_.hover = h2; Invalidate(); }
    }
    void OnKeyDown(UINT vk) override {
        if (vk == VK_RETURN) { result_ = true; Close(); }
        else if (vk == VK_ESCAPE) Close();
    }

public:
    bool result_ = true;
    std::wstring text_;
    UiButton ok_, cancel_;
};

bool ShowMsgBox(HWND owner, const std::wstring& text, const std::wstring& title, bool yesNo) {
    MsgBoxWindow wnd(text, title, yesNo);
    wnd.Create(owner, true);
    wnd.RunModal();
    return wnd.result_;
}

} // namespace ark::ui