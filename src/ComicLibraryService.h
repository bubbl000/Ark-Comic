#pragma once
// 漫画库业务服务 + SQLite 索引（library.db，位于资源库 .info 内）
// 参考 C# 版 ComicLibraryService.cs + AppDbContext.cs
#include "Models.h"
#include <string>
#include <vector>
#include <memory>

struct sqlite3;

namespace ark {

class ComicLibraryService {
public:
    static constexpr const wchar_t* DbFileName = L"library.db";

    explicit ComicLibraryService(const std::string& libraryInfoPath);
    ~ComicLibraryService();

    // 切换到另一资源库（调用后需重新 Load）
    void SwitchLibrary(const std::string& libraryInfoPath);

    std::string LibraryPath;
    std::string DbPath;

    // ---- 漫画 ----
    std::vector<ComicModel> GetAllComics();          // 清理孤儿记录后返回
    bool GetComicById(int id, ComicModel& out);
    bool ImportComic(const std::string& path, ComicModel& out); // 已存在返回 false
    bool ImportExistingComic(const ComicModel& comic);
    void UpdateComic(const ComicModel& comic);
    void DeleteComic(int id);
    void TrashComic(int id);
    void RestoreComic(int id);
    void SaveReadingProgress(int id, int currentPage, int pageCount);

    // ---- 自愈重建 ----
    void RebuildFromInfoIfEmpty();

    // ---- 文件夹 ----
    std::vector<FolderModel> BuildFolderTree();
    FolderModel AddRootFolder(const std::string& name);
    FolderModel AddSubfolder(int parentId, const std::string& name);
    void RenameFolder(int id, const std::string& newName);
    void DeleteFolder(int id);
    void MoveFolder(int sourceId, int targetId);        // 移动为 targetId 的子文件夹
    void MoveFolderToRoot(int sourceId);
    void PersistFolderOrder(const std::vector<int>& siblingIds); // 按顺序重写 SortOrder
    bool IsDescendantOf(int candidateId, int ancestorId);        // candidate 是否为 ancestor 的后代
    int GetFolderParent(int id);
    std::vector<int> GetSiblingIds(int folderId);                // 同父兄弟 id（按 SortOrder）

    // ---- 标签 ----
    std::vector<TagModel> GetAllTags();
    void SyncTagCounts();

    // 判断是否为可导入的漫画源（合法扩展名：文件夹/压缩包/图片）
    static bool IsValidComicSource(const std::string& path);

    // ---- 关闭时 checkpoint WAL ----
    void CheckpointWal();

private:
    sqlite3* db_ = nullptr;

    void OpenDb(const std::string& dbPath);
    void EnsureSchema();
    void MigrateSchema();
    static void CleanupStaleWalFiles(const std::string& dbPath);

    // 底层辅助
    static int Exec(sqlite3* db, const char* sql);
    static int CountPages(const std::string& path);
    static std::string FindCover(const std::string& path);
    std::string ComicInfoPath(const std::string& title);
    static std::string SanitizeTitle(const std::string& title);
};

} // namespace ark