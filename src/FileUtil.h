#pragma once
// 宽字符文件/目录工具（中文路径支持）
#include <string>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include "Utf.h"

namespace ark {

inline std::wstring W(const std::string& u8) { return Utf8ToWide(u8); }
inline std::string U8(const std::wstring& w) { return WideToUtf8(w); }

inline bool DirExistsW(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
inline bool FileExistsW(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
inline bool PathExistsW(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}
inline void CreateDirRecursive(const std::wstring& p) {
    // 逐级创建
    std::wstring cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur += p[i];
        if (p[i] == L'\\' || p[i] == L'/' || i == p.size() - 1) {
            if (!cur.empty() && PathExistsW(cur) == false) {
                CreateDirectoryW(cur.c_str(), nullptr);
            }
        }
    }
}
inline bool DeleteDirRecursiveW(const std::wstring& p) {
    if (!DirExistsW(p)) return false;
    SHFILEOPSTRUCTW so{};
    wchar_t buf[MAX_PATH * 2] = {};
    wcscpy_s(buf, p.c_str());
    buf[wcslen(buf) + 1] = L'\0'; // 双空结尾
    so.wFunc = FO_DELETE;
    so.pFrom = buf;
    so.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    int r = SHFileOperationW(&so);
    return r == 0;
}

// 列出目录下所有子目录（返回完整路径），suffix 为可选过滤后缀（如 L".info"，空则全部）
inline std::vector<std::wstring> ListDirectories(const std::wstring& dir, const std::wstring& suffix = L"") {
    std::vector<std::wstring> out;
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
            std::wstring full = dir + L"\\" + fd.cFileName;
            if (suffix.empty() ||
                (full.size() >= suffix.size() &&
                 _wcsicmp(full.c_str() + full.size() - suffix.size(), suffix.c_str()) == 0)) {
                out.push_back(full);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// 列出目录下所有文件（返回完整路径）
inline std::vector<std::wstring> ListFiles(const std::wstring& dir) {
    std::vector<std::wstring> out;
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            out.push_back(dir + L"\\" + fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// 文件名/扩展名工具
inline std::wstring GetFileNameW(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}
inline std::wstring GetFileNameNoExtW(const std::wstring& p) {
    std::wstring name = GetFileNameW(p);
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}
inline std::wstring GetExtensionW(const std::wstring& p) {
    std::wstring name = GetFileNameW(p);
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? L"" : name.substr(dot);
}

// 清理文件夹名非法字符：\ / : * ? " < > | 全部替换为 _（与 C# SanitizeFolderName 一致）
inline std::string SanitizeFolderName(const std::string& name) {
    std::string s = name;
    if (s.empty()) return "未命名";
    for (auto& c : s) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '\"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    // 去首尾空白
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return "未命名";
    return s.substr(b, e - b + 1);
}

} // namespace ark