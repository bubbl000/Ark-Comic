#include "TrayIcon.h"
#include "Theme.h"
#include "../resources/resource.h"
#include <shellapi.h>

namespace ark::ui {

bool TrayIcon::Add(HWND hwnd, UINT msg) {
    hwnd_ = hwnd;
    msg_ = msg;
    // 托盘图标：加载应用 logo（resources/app.ico 的 16x16 渲染）
    icon_ = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP),
                              IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!icon_) icon_ = LoadIconW(nullptr, MAKEINTRESOURCEW(IDI_APPLICATION));  // 兜底：系统默认
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = msg;
    nid.hIcon = icon_;
    wsprintfW(nid.szTip, L"Ark Comic");
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (!added_ && icon_) { DestroyIcon(icon_); icon_ = nullptr; }
    return added_;
}

void TrayIcon::Remove() {
    if (added_ && hwnd_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
    }
    if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
}

} // namespace ark::ui