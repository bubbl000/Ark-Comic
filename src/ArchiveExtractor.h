#pragma once
// 漫画页提取：文件夹直接列图；CBZ/ZIP 用 miniz 解压到缓存后列图（自然排序，UTF-8 中文名）
#include <string>
#include <vector>

namespace ark {

// 图片扩展名白名单（小写，不含点）
inline bool IsImageFile(const std::string& name) {
    static const char* kExts[] = { ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".tif", ".tiff" };
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e;
    for (size_t i = dot; i < name.size(); i++) e += (char)tolower((unsigned char)name[i]);
    for (auto* x : kExts) if (e == x) return true;
    return false;
}

// 压缩包扩展名（小写）
inline bool IsArchivePath(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e;
    for (size_t i = dot; i < path.size(); i++) e += (char)tolower((unsigned char)path[i]);
    return e == ".cbz" || e == ".zip";
}

class ArchiveExtractor {
public:
    // 返回漫画所有页面图片路径（已自然排序）。失败/无图返回空。
    static std::vector<std::string> GetPages(const std::string& path);

    // 页数（调 GetPages）
    static int GetPageCount(const std::string& path);

private:
    // 图片文件夹：递归列出所有图片（自然排序）
    static std::vector<std::string> ListDirImages(const std::string& dir);
    // CBZ/ZIP：解压到缓存目录后列图
    static std::vector<std::string> ExtractArchive(const std::string& archive);
    // 缓存目录：%LOCALAPPDATA%\ArkComic\cbz_cache\<文件名>_<大小>_<修改时间>（源文件变化自动失效）
    static std::wstring CacheDirFor(const std::string& archive);
};

} // namespace ark