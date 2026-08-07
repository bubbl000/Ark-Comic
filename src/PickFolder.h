#pragma once
// 系统文件夹选择（IFileOpenDialog + FOS_PICKFOLDERS）
#include <windows.h>
#include <shobjidl.h>
#include <string>

namespace ark::ui {

// 弹出系统文件夹选择框。成功选择返回 true，out 为所选文件夹完整路径。
// title 可为空（用系统默认标题）。
inline bool PickFolder(HWND owner, const wchar_t* title, std::wstring& out) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr)) { CoUninitialize(); return false; }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (title && title[0]) dlg->SetTitle(title);
    bool picked = false;
    hr = dlg->Show(owner);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out = path;
                CoTaskMemFree(path);
                picked = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    CoUninitialize();
    return picked;
}

} // namespace ark::ui