#pragma once
// 应用配置管理：持久化到 %LocalAppData%\ArkComic\config.json（与 C# 版 AppConfig 一致）
#include "Models.h"
#include <string>
#include <vector>

namespace ark {

class AppConfig {
public:
    static AppConfig& Instance(); // 懒加载单例

    std::string themeColor = "#CBE93A";
    bool closeToTray = true;
    std::string libraryRootPath;
    std::string libraryName;
    bool migrated = false;
    std::vector<LibraryEntry> libraries;
    std::string currentLibraryPath;
    std::string language = "zh-CN";

    void Save(); // 写回磁盘
    void Reload(); // 重新从磁盘加载

    // 配置文件路径（%LocalAppData%\ArkComic\config.json）
    static std::wstring ConfigFilePath();
    static std::wstring ConfigDir();

private:
    AppConfig() = default;
    void LoadFromJson();
};

} // namespace ark