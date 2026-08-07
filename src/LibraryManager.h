#pragma once
// 资源库管理器：管理用户添加的资源库（.info 文件夹）列表与当前激活资源库
// 结构：<rootPath> / 名称.info / 漫画名.info / (images, tags.json, metadata.json)
#include <string>
#include <vector>

namespace ark {

class LibraryManager {
public:
    static constexpr const wchar_t* LibrarySuffix = L".info";

    // 当前激活资源库 .info 文件夹完整路径（UTF-8）
    static std::string CurrentLibraryPath();
    static bool HasLibrary();

    // 生成默认资源库名称："资源库N"（N = 根目录下已有"资源库"开头 .info 数量 + 1）
    static std::string GetDefaultLibraryName(const std::string& rootPath);

    // 创建资源库：在 rootPath 下建立 name.info，加入列表并设为当前激活
    static std::string CreateLibrary(const std::string& rootPath, const std::string& libraryName);

    // 选择/加入：若 rootPath 下已存在 .info 资源库则直接加入（打开）第一个；
    // 否则用 libraryName 在 rootPath 下新建资源库。返回当前激活资源库 .info 路径。
    static std::string OpenOrCreateLibrary(const std::string& rootPath, const std::string& libraryName);

    // 打开（切换）已存在的资源库 .info 文件夹，并加入列表
    static std::string OpenLibrary(const std::string& libInfoPath);

    // 从列表中移除（仅移除记录，不删除磁盘文件）；若为当前库则清空当前项
    static void RemoveLibrary(const std::string& libInfoPath);

    // 切换当前激活资源库为列表中已存在的项
    static void SwitchTo(const std::string& libInfoPath);

    // 枚举当前资源库下所有漫画 .info 文件夹路径（UTF-8）
    static std::vector<std::string> EnumerateComicFolders();
};

} // namespace ark