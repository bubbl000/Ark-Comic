#include "AppConfig.h"
#include "JsonUtil.h"
#include "Utf.h"
#include <shlobj.h>
#include <windows.h>
#include <type_traits>

namespace ark {

static std::wstring LocalAppData() {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)))
        return buf;
    return L"C:\\Users\\Public";
}

std::wstring AppConfig::ConfigDir() {
    return LocalAppData() + L"\\ArkComic";
}

std::wstring AppConfig::ConfigFilePath() {
    return ConfigDir() + L"\\config.json";
}

AppConfig& AppConfig::Instance() {
    static AppConfig cfg;
    static bool loaded = false;
    if (!loaded) { cfg.LoadFromJson(); loaded = true; }
    return cfg;
}

void AppConfig::LoadFromJson() {
    Json j;
    if (!ReadJsonFile(ConfigFilePath(), j)) return;
    using nlohmann::json;
    auto get = [&](const char* k, auto& dst) {
        auto it = j.find(k);
        if (it != j.end()) dst = it->get<std::decay_t<decltype(dst)>>();
    };
    get("ThemeColor", themeColor);
    get("CloseToTray", closeToTray);
    get("LibraryRootPath", libraryRootPath);
    get("LibraryName", libraryName);
    get("Migrated", migrated);
    get("CurrentLibraryPath", currentLibraryPath);
    get("Language", language);
    if (j.contains("Libraries")) {
        libraries.clear();
        for (auto& e : j["Libraries"]) {
            LibraryEntry le;
            if (e.contains("Name")) le.name = e["Name"].get<std::string>();
            if (e.contains("Path")) le.path = e["Path"].get<std::string>();
            libraries.push_back(le);
        }
    }
}

void AppConfig::Save() {
    Json j;
    j["ThemeColor"] = themeColor;
    j["CloseToTray"] = closeToTray;
    j["LibraryRootPath"] = libraryRootPath;
    j["LibraryName"] = libraryName;
    j["Migrated"] = migrated;
    j["CurrentLibraryPath"] = currentLibraryPath;
    j["Language"] = language;
    j["Libraries"] = Json::array();
    for (auto& le : libraries) {
        j["Libraries"].push_back(Json{{"Name", le.name}, {"Path", le.path}});
    }
    // 确保目录存在
    CreateDirectoryW(ConfigDir().c_str(), nullptr);
    WriteJsonFile(ConfigFilePath(), j);
}

void AppConfig::Reload() {
    libraries.clear();
    themeColor = "#CBE93A"; closeToTray = true; libraryRootPath.clear(); libraryName.clear();
    migrated = false; currentLibraryPath.clear(); language = "zh-CN";
    LoadFromJson();
}

} // namespace ark