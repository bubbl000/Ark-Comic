#pragma once
// 数据模型：与 C# 旧版 ArkComic.Core.Models 保持一致
#include <string>
#include <vector>
#include <map>

namespace ark {

enum class ComicStatus { Unread, Reading, Completed, OnHold, Deleted };

inline const char* StatusToString(ComicStatus s) {
    switch (s) {
        case ComicStatus::Unread: return "Unread";
        case ComicStatus::Reading: return "Reading";
        case ComicStatus::Completed: return "Completed";
        case ComicStatus::OnHold: return "OnHold";
        case ComicStatus::Deleted: return "Deleted";
    }
    return "Unread";
}

inline ComicStatus StatusFromString(const std::string& s) {
    if (s == "Reading") return ComicStatus::Reading;
    if (s == "Completed") return ComicStatus::Completed;
    if (s == "OnHold") return ComicStatus::OnHold;
    if (s == "Deleted") return ComicStatus::Deleted;
    return ComicStatus::Unread;
}

// 漫画实体（ComicModel）。metadata.json 键名与 C# 版完全一致（不含 Author）。
struct ComicModel {
    int id = 0;
    std::string title;
    std::string author;              // 仅存 DB，metadata.json 不写
    std::string filePath;
    std::string coverPath;
    int pageCount = 0;
    int currentPage = 0;
    std::vector<int> folderIds;
    std::map<int, int> folderOrders;
    std::string folderNames = "未分类";
    std::string sourceUrl;
    std::string notes;
    ComicStatus status = ComicStatus::Unread;
    std::string createdAt;           // ISO 字符串，如 2026-08-05T15:30:00
    std::string lastReadAt;          // 空串表示 null
    std::vector<std::string> tags;

    // 与阅读器进度条一致：currentPage 为 0 索引，显示时 +1，末页=100%
    int progressPercent() const {
        return pageCount > 0 ? static_cast<int>((currentPage + 1) * 100.0 / pageCount) : 0;
    }
};

// 虚拟文件夹（FolderModel）
struct FolderModel {
    int id = 0;
    std::string name;
    int parentId = 0;
    int sortOrder = 0;
    std::vector<FolderModel> children;
};

// 全局标签（TagModel）
struct TagModel {
    int id = 0;
    std::string name;
    int count = 0;
};

// 资源库记录（LibraryEntry）
struct LibraryEntry {
    std::string name;
    std::string path;
};

} // namespace ark