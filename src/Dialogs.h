#pragma once
// 通用对话框：文本输入 / 下拉菜单
#include "WindowBase.h"
#include "Controls.h"
#include <string>
#include <vector>

namespace ark::ui {

// 文本输入对话框（新建/重命名文件夹等）。确认后通过 Text 取结果。
class InputDialog : public WindowBase {
public:
    InputDialog(HWND owner, const std::wstring& title, const std::wstring& label,
                const std::wstring& initial = L"");
    std::wstring Text;
    bool Run();

protected:
    void OnPaint(ID2D1RenderTarget* rt, int w, int h) override;
    void OnLButtonDown(int x, int y) override;
    void OnMouseMove(int x, int y) override;
    void OnKeyDown(UINT vk) override;
    void OnChar(wchar_t ch) override;

private:
    void TryConfirm();
    HWND owner_;
    TextBox box_;
    UiButton okBtn_, cancelBtn_;
    std::wstring label_;
    bool confirmed_ = false;
};

// 简单下拉菜单（左栏汉堡菜单）。选中返回该项 tag，关闭/点空白返回 -1。
class PopupMenu : public WindowBase {
public:
    struct Item { std::wstring label; int tag; };
    PopupMenu(int x, int y, std::vector<Item> items);
    int Run();

protected:
    void OnPaint(ID2D1RenderTarget* rt, int w, int h) override;
    void OnLButtonDown(int x, int y) override;
    void OnMouseMove(int x, int y) override;
    void OnMouseLeave() override;
    void OnKeyDown(UINT vk) override;
    void OnChar(wchar_t ch) override {}

private:
    std::vector<Item> items_;
    std::vector<D2D1_RECT_F> itemRects_;
    int hover_ = -1;
    int result_ = -1;
    int posX_ = 0;
    int posY_ = 0;
};

} // namespace ark::ui