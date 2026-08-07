#include "LibraryManager.h"
#include "AppConfig.h"
#include "FileUtil.h"
#include <algorithm>
#include <cctype>

namespace ark {

static std::wstring Suffix() { return LibraryManager::LibrarySuffix; }

std::string LibraryManager::CurrentLibraryPath() {
    return AppConfig::Instance().currentLibraryPath;
}

bool LibraryManager::HasLibrary() {
    const auto& p = AppConfig::Instance().currentLibraryPath;
    return !p.empty() && DirExistsW(W(p));
}

std::string LibraryManager::GetDefaultLibraryName(const std::string& rootPath) {
    if (!DirExistsW(W(rootPath))) return "资源库1";
    int max = 0;
    for (auto& d : ListDirectories(W(rootPath), Suffix())) {
        std::wstring name = GetFileNameNoExtW(d);
        std::string u = U8(name);
        const std::string prefix = "资源库";
        if (u.rfind(prefix, 0) == 0) {
            std::string num = u.substr(prefix.size());
            if (!num.empty() && std::all_of(num.begin(), num.end(), [](char c){ return std::isdigit((unsigned char)c); })) {
                max = std::max(max, std::stoi(num));
            }
        }
    }
    return "资源库" + std::to_string(max + 1);
}

static bool PathEqualsIgnoreCase(const std::string& a, const std::string& b) {
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

std::string LibraryManager::CreateLibrary(const std::string& rootPath, const std::string& libraryName) {
    std::string name = libraryName;
    if (name.empty()) name = GetDefaultLibraryName(rootPath);
    // 去掉首尾空白
    size_t b = name.find_first_not_of(" \t\r\n");
    if (b != std::string::npos) name = name.substr(b, name.find_last_not_of(" \t\r\n") - b + 1);

    std::wstring libDir = W(rootPath) + L"\\" + W(name) + Suffix();
    CreateDirRecursive(libDir);

    auto& cfg = AppConfig::Instance();
    cfg.libraryRootPath = rootPath;
    cfg.libraryName = name;

    bool exists = false;
    for (auto& le : cfg.libraries) if (PathEqualsIgnoreCase(le.path, U8(libDir))) { exists = true; break; }
    if (!exists) cfg.libraries.push_back({name, U8(libDir)});
    cfg.currentLibraryPath = U8(libDir);
    cfg.migrated = false;
    cfg.Save();
    return U8(libDir);
}

std::string LibraryManager::OpenOrCreateLibrary(const std::string& rootPath, const std::string& libraryName) {
    // 若根目录下已有 .info 资源库，直接加入第一个（不新建）
    if (DirExistsW(W(rootPath))) {
        auto dirs = ListDirectories(W(rootPath), Suffix());
        if (!dirs.empty()) return OpenLibrary(U8(dirs.front()));
    }
    return CreateLibrary(rootPath, libraryName);
}

std::string LibraryManager::OpenLibrary(const std::string& libInfoPath) {
    if (!DirExistsW(W(libInfoPath))) return std::string();
    std::wstring name = GetFileNameNoExtW(W(libInfoPath));
    auto& cfg = AppConfig::Instance();
    bool exists = false;
    for (auto& le : cfg.libraries) if (PathEqualsIgnoreCase(le.path, libInfoPath)) { exists = true; break; }
    if (!exists) cfg.libraries.push_back({U8(name), libInfoPath});
    // 根目录取 .info 的父目录
    std::wstring root = W(libInfoPath);
    size_t pos = root.find_last_of(L"\\/");
    cfg.libraryRootPath = pos == std::wstring::npos ? std::string() : U8(root.substr(0, pos));
    cfg.libraryName = U8(name);
    cfg.currentLibraryPath = libInfoPath;
    cfg.migrated = false;
    cfg.Save();
    return libInfoPath;
}

void LibraryManager::RemoveLibrary(const std::string& libInfoPath) {
    auto& cfg = AppConfig::Instance();
    for (auto it = cfg.libraries.begin(); it != cfg.libraries.end(); ++it) {
        if (PathEqualsIgnoreCase(it->path, libInfoPath)) {
            cfg.libraries.erase(it);
            break;
        }
    }
    if (PathEqualsIgnoreCase(cfg.currentLibraryPath, libInfoPath)) {
        cfg.currentLibraryPath.clear();
        cfg.libraryRootPath.clear();
        cfg.libraryName.clear();
    }
    cfg.Save();
}

void LibraryManager::SwitchTo(const std::string& libInfoPath) {
    auto& cfg = AppConfig::Instance();
    bool found = false;
    std::string name;
    for (auto& le : cfg.libraries) {
        if (PathEqualsIgnoreCase(le.path, libInfoPath)) { found = true; name = le.name; break; }
    }
    if (!found) return;
    std::wstring root = W(libInfoPath);
    size_t pos = root.find_last_of(L"\\/");
    cfg.libraryRootPath = pos == std::wstring::npos ? std::string() : U8(root.substr(0, pos));
    cfg.libraryName = name;
    cfg.currentLibraryPath = libInfoPath;
    cfg.migrated = false;
    cfg.Save();
}

std::vector<std::string> LibraryManager::EnumerateComicFolders() {
    std::vector<std::string> out;
    const auto& libPath = AppConfig::Instance().currentLibraryPath;
    if (libPath.empty() || !DirExistsW(W(libPath))) return out;
    for (auto& d : ListDirectories(W(libPath), Suffix())) {
        out.push_back(U8(d));
    }
    return out;
}

} // namespace ark