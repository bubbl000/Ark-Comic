#pragma once
// 设置窗口（560x720）：分页「常规设置」（主题色/关闭行为）与「资源库设置」（最多 3 个资源库）
#include "WindowBase.h"
#include "Controls.h"
#include <string>
#include <vector>

namespace ark::ui {

class SettingsWindow : public WindowBase {
public:
    explicit SettingsWindow(HWND owner);
    void Run(); // 模态运行，点击 OK 保存并关闭
    // 设置期间是否新增/移除了资源库（供主窗口决定是否重载资源库）
    bool librariesChanged() const { return libsChanged_; }

protected:
    void OnPaint(ID2D1RenderTarget* rt, int w, int h) override;
    void OnLButtonDown(int x, int y) override;
    void OnMouseMove(int x, int y) override;
    void OnMouseLeave() override;
    void OnKeyDown(UINT vk) override;
    void OnChar(wchar_t ch) override;
    bool OnCloseRequested() override; // 关闭：还原未保存的主题色

private:
    // 资源库行：名称框 + 选择/移除按钮 + 路径
    struct LibRow {
        TextBox name;
        D2D1_RECT_F btn;
        D2D1_RECT_F path;
        bool configured = false;
        std::wstring pathText;
    };

    void SelectPreset(int i);   // 选中预设色并实时应用
    void ApplyCustom();         // 应用自定义输入色（合法才生效）
    void TryConfirm();          // 确定：保存主题/关闭行为（资源库操作已即时生效）
    void Layout();
    void SyncRows();            // 从 cfg.libraries 重建最多 3 行资源库列表
    void OnLibButton(int row);  // 未配置=选择创建；已配置=从软件移除
    void PaintTabs(ID2D1RenderTarget* rt);
    void PaintGeneral(ID2D1RenderTarget* rt);
    void PaintLibrary(ID2D1RenderTarget* rt);

    HWND ownerWnd_;
    int tab_ = 0;               // 0 常规设置，1 资源库设置
    bool libsChanged_ = false;

    // 预设色（0xCBE93A 等）与名称
    DWORD presets_[4];
    std::wstring presetNames_[4];
    std::vector<D2D1_RECT_F> swatchRects_;
    int selectedPreset_ = -1;   // -1 = 自定义
    TextBox customBox_;
    bool customFocused_ = false;
    bool closeToTray_ = true;
    bool ok_ = false;

    // 资源库行
    std::vector<LibRow> rows_;
    int focusedRow_ = -1;       // 聚焦的名称输入框所在行（-1 无）

    // 几何
    D2D1_RECT_F tabGeneral_, tabLibrary_;
    D2D1_RECT_F sepTheme_, themeTitle_, themeDesc_, customLabel_;
    D2D1_RECT_F sepClose_, closeTitle_, closeDesc_;
    std::vector<D2D1_RECT_F> radioRects_;
    D2D1_RECT_F radioNote_;
    D2D1_RECT_F libTitle_, libDesc_;
    D2D1_RECT_F okRect_;

    int hoverSwatch_ = -1;
    int hoverRowBtn_ = -1;
    bool hoverOk_ = false;
    bool hoverTabGeneral_ = false, hoverTabLibrary_ = false;
};

} // namespace ark::ui