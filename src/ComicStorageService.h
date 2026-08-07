#pragma once
// 漫画 .info 文件夹存储服务：负责漫画元数据/标签/封面读写与源文件移入
#include "Models.h"
#include <string>

namespace ark {

class ComicStorageService {
public:
    static constexpr const wchar_t* ComicSuffix = L".info";

    // 创建（或获取已存在）漫画 .info 文件夹，路径：资源库.info / 漫画标题.info
    static std::string EnsureComicInfoFolder(const ComicModel& comic);

    // 获取漫画 .info 文件夹路径（UTF-8，可能不存在）
    static std::string GetComicInfoPath(const ComicModel& comic);

    // 写入 metadata.json 与 tags.json
    static void SaveComicData(const ComicModel& comic);

    // 从 .info 文件夹读取漫画（失败返回 false）
    static bool LoadComic(const std::string& infoDir, ComicModel& out);

    // 加载当前资源库下所有漫画
    static std::vector<ComicModel> LoadAllComics();

    // 把源文件/文件夹移动到 .info 内，并更新 FilePath/CoverPath
    static void MoveSourceIntoInfo(ComicModel& comic, const std::string& sourcePath, bool copySource = false);

    // 仅复制封面到 images/（不移动源文件）
    static void CacheCoverOnly(ComicModel& comic);

    // 删除漫画 .info 文件夹
    static void DeleteComicInfo(const ComicModel& comic);

    // 替换漫画封面：复制新图到 .info/images/cover.ext，更新 CoverPath 并写 metadata.json
    static void ReplaceCover(ComicModel& comic, const std::string& imageFilePath);

    // 当前本地时间 ISO 字符串：yyyy-MM-ddTHH:mm:ss
    static std::string NowIso();
};

} // namespace ark