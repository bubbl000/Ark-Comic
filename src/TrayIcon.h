#pragma once
// 系统托盘图标：添加/删除，配合主窗口隐藏实现"最小化到托盘"
#include <windows.h>

namespace ark::ui {

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon() { Remove(); }

    // 添加托盘图标；msg 为自定义托盘消息（通知回调到 hwnd）
    bool Add(HWND hwnd, UINT msg);
    // 删除托盘图标
    void Remove();
    bool Present() const { return added_; }

private:
    HWND hwnd_ = nullptr;
    UINT msg_ = 0;
    bool added_ = false;
    HICON icon_ = nullptr;
};

} // namespace ark::ui