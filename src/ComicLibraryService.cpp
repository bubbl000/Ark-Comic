#include "ComicLibraryService.h"
#include "ComicStorageService.h"
#include "FileUtil.h"
#include "JsonUtil.h"
#include "Utf.h"
#include <sqlite3.h>
#include <algorithm>
#include <cctype>
#include <map>

namespace ark {

static std::string Lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

ComicLibraryService::ComicLibraryService(const std::string& libraryInfoPath) {
    LibraryPath = libraryInfoPath;
    CreateDirRecursive(W(libraryInfoPath));
    DbPath = libraryInfoPath + "\\library.db";
    CleanupStaleWalFiles(DbPath);
    OpenDb(DbPath);
    EnsureSchema();
    MigrateSchema();
}

ComicLibraryService::~ComicLibraryService() {
    CheckpointWal();
    if (db_) sqlite3_close_v2(db_);
}

void ComicLibraryService::SwitchLibrary(const std::string& libraryInfoPath) {
    CheckpointWal();
    if (db_) sqlite3_close_v2(db_);
    db_ = nullptr;
    LibraryPath = libraryInfoPath;
    CreateDirRecursive(W(libraryInfoPath));
    DbPath = libraryInfoPath + "\\library.db";
    CleanupStaleWalFiles(DbPath);
    OpenDb(DbPath);
    EnsureSchema();
    MigrateSchema();
}

void ComicLibraryService::OpenDb(const std::string& dbPath) {
    int rc = sqlite3_open_v2(dbPath.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        db_ = nullptr;
        return;
    }
    sqlite3_busy_timeout(db_, 3000);
    // WAL 模式
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
}

int ComicLibraryService::Exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc;
}

void ComicLibraryService::EnsureSchema() {
    Exec(db_,
        "CREATE TABLE IF NOT EXISTS Comics ("
        " Id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " Title TEXT NOT NULL,"
        " Author TEXT NOT NULL DEFAULT '',"
        " FilePath TEXT NOT NULL,"
        " CoverPath TEXT,"
        " PageCount INTEGER NOT NULL DEFAULT 0,"
        " CurrentPage INTEGER NOT NULL DEFAULT 0,"
        " FolderIds TEXT NOT NULL DEFAULT '[]',"
        " FolderOrders TEXT NOT NULL DEFAULT '{}',"
        " FolderNames TEXT NOT NULL DEFAULT '未分类',"
        " SourceUrl TEXT,"
        " Notes TEXT,"
        " Status TEXT NOT NULL,"
        " CreatedAt TEXT NOT NULL DEFAULT (datetime('now')),"
        " LastReadAt TEXT,"
        " Tags TEXT NOT NULL DEFAULT '[]');");
    Exec(db_, "CREATE INDEX IF NOT EXISTS IX_Comics_Status ON Comics(Status);");
    Exec(db_,
        "CREATE TABLE IF NOT EXISTS Folders ("
        " Id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " Name TEXT NOT NULL,"
        " ParentId INTEGER NOT NULL DEFAULT 0,"
        " SortOrder INTEGER NOT NULL DEFAULT 0);");
    Exec(db_, "CREATE INDEX IF NOT EXISTS IX_Folders_ParentId ON Folders(ParentId);");
    Exec(db_,
        "CREATE TABLE IF NOT EXISTS Tags ("
        " Id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " Name TEXT NOT NULL UNIQUE,"
        " Count INTEGER NOT NULL DEFAULT 0);");
}

void ComicLibraryService::MigrateSchema() {
    // 兼容旧 schema：补齐单个文件夹列 -> 多对多列
    sqlite3_stmt* st = nullptr;
    bool hasFolderIds = false, hasFolderOrders = false, hasFolderNames = false, hasAuthor = false;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info('Comics');", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* col = (const char*)sqlite3_column_text(st, 1);
            if (!col) continue;
            std::string c = col;
            if (c == "FolderIds") hasFolderIds = true;
            if (c == "FolderOrders") hasFolderOrders = true;
            if (c == "FolderNames") hasFolderNames = true;
            if (c == "Author") hasAuthor = true;
        }
    }
    sqlite3_finalize(st);
    if (!hasFolderIds) Exec(db_, "ALTER TABLE Comics ADD COLUMN FolderIds TEXT NOT NULL DEFAULT '[]';");
    if (!hasFolderOrders) Exec(db_, "ALTER TABLE Comics ADD COLUMN FolderOrders TEXT NOT NULL DEFAULT '{}';");
    if (!hasFolderNames) Exec(db_, "ALTER TABLE Comics ADD COLUMN FolderNames TEXT NOT NULL DEFAULT '未分类';");
    if (!hasAuthor) Exec(db_, "ALTER TABLE Comics ADD COLUMN Author TEXT NOT NULL DEFAULT '';");
}

void ComicLibraryService::CleanupStaleWalFiles(const std::string& dbPath) {
    sqlite3* tmp = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &tmp, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK) {
        sqlite3_exec(tmp, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
        sqlite3_exec(tmp, "PRAGMA journal_mode=DELETE;", nullptr, nullptr, nullptr);
        sqlite3_close_v2(tmp);
    }
}

void ComicLibraryService::CheckpointWal() {
    if (db_) sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
}

// ==================== 读取一行 Comic ====================
static bool ReadComicRow(sqlite3_stmt* st, ComicModel& c) {
    c.id = sqlite3_column_int(st, 0);
    c.title = (const char*)sqlite3_column_text(st, 1);
    c.author = sqlite3_column_type(st, 2) != SQLITE_NULL ? (const char*)sqlite3_column_text(st, 2) : "";
    c.filePath = sqlite3_column_type(st, 3) != SQLITE_NULL ? (const char*)sqlite3_column_text(st, 3) : "";
    c.coverPath = sqlite3_column_type(st, 4) != SQLITE_NULL ? (const char*)sqlite3_column_text(st, 4) : "";
    c.pageCount = sqlite3_column_int(st, 5);
    c.currentPage = sqlite3_column_int(st, 6);
    if (sqlite3_column_type(st, 7) != SQLITE_NULL) {
        std::string s = (const char*)sqlite3_column_text(st, 7);
        try { Json j = Json::parse(s); if (j.is_array()) { c.folderIds.clear(); for (auto& v : j) c.folderIds.push_back(v.get<int>()); } } catch (...) {}
    }
    if (sqlite3_column_type(st, 8) != SQLITE_NULL) {
        std::string s = (const char*)sqlite3_column_text(st, 8);
        try { Json j = Json::parse(s); if (j.is_object()) { c.folderOrders.clear(); for (auto& [k, v] : j.items()) c.folderOrders[std::stoi(k)] = v.get<int>(); } } catch (...) {}
    }
    c.folderNames = sqlite3_column_type(st, 9) != SQLITE_NULL ? (const char*)sqlite3_column_text(st, 9) : "未分类";
    c.sourceUrl = sqlite3_column_type(st, 10) != SQLITE_NULL ? (const char*)sqlite3_column_text(st, 10) : "";
    c.notes = sqlite3_column_type(st, 11) != SQLITE_NULL ? (const char*)sqlite3_column_text(st, 11) : "";
    // Status 兼容：INTEGER（C# EF 默认）或 TEXT
    if (sqlite3_column_type(st, 12) == SQLITE_INTEGER) {
        c.status = (ComicStatus)sqlite3_column_int(st, 12);
    } else {
        c.status = StatusFromString(sqlite3_column_type(st, 12) != SQLITE_NULL ? (const char*)sqlite3_column_text(st, 12) : "Unread");
    }
    c.createdAt = sqlite3_column_type(st, 13) != SQLITE_NULL ? (const char*)sqlite3_column_text(st, 13) : "";
    c.lastReadAt = sqlite3_column_type(st, 14) != SQLITE_NULL ? (const char*)sqlite3_column_text(st, 14) : "";
    if (sqlite3_column_type(st, 15) != SQLITE_NULL) {
        std::string s = (const char*)sqlite3_column_text(st, 15);
        try { Json j = Json::parse(s); if (j.is_array()) { c.tags.clear(); for (auto& v : j) c.tags.push_back(v.get<std::string>()); } } catch (...) {}
    }
    return true;
}

std::vector<ComicModel> ComicLibraryService::GetAllComics() {
    std::vector<ComicModel> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT Id,Title,Author,FilePath,CoverPath,PageCount,CurrentPage,"
        "FolderIds,FolderOrders,FolderNames,SourceUrl,Notes,Status,CreatedAt,LastReadAt,Tags FROM Comics "
        "ORDER BY CreatedAt DESC;", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            ComicModel c;
            if (ReadComicRow(st, c)) out.push_back(c);
        }
    }
    sqlite3_finalize(st);
    // 清理孤儿记录：DB 存在但 .info 文件夹已被删除
    std::vector<ComicModel> kept;
    kept.reserve(out.size());
    for (auto& c : out) {
        if (DirExistsW(W(ComicInfoPath(c.title)))) kept.push_back(c);
        else DeleteComic(c.id);
    }
    return kept;
}

bool ComicLibraryService::GetComicById(int id, ComicModel& out) {
    sqlite3_stmt* st = nullptr;
    bool found = false;
    if (sqlite3_prepare_v2(db_, "SELECT Id,Title,Author,FilePath,CoverPath,PageCount,CurrentPage,"
        "FolderIds,FolderOrders,FolderNames,SourceUrl,Notes,Status,CreatedAt,LastReadAt,Tags FROM Comics WHERE Id=?;",
        -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) { ReadComicRow(st, out); found = true; }
    }
    sqlite3_finalize(st);
    return found;
}

bool ComicLibraryService::ImportComic(const std::string& path, ComicModel& out) {
    if (!IsValidComicSource(path)) return false;
    // 避免重复导入（同 FilePath）
    sqlite3_stmt* st = nullptr;
    bool dup = false;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM Comics WHERE FilePath=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) dup = sqlite3_column_int(st, 0) > 0;
    }
    sqlite3_finalize(st);
    if (dup) return false;

    std::string title = U8(GetFileNameNoExtW(W(path)));
    if (DirExistsW(W(path))) title = U8(GetFileNameW(W(path)));

    ComicModel c;
    c.title = title;
    c.filePath = path;
    c.coverPath = FindCover(path);
    c.pageCount = CountPages(path);
    c.createdAt = ComicStorageService::NowIso();
    if (ImportExistingComic(c)) { out = c; return true; }
    return false;
}

bool ComicLibraryService::ImportExistingComic(const ComicModel& comic) {
    ComicModel c = comic;
    c.id = 0;
    // 同 FilePath 已存在则跳过
    sqlite3_stmt* st = nullptr;
    bool dup = false;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM Comics WHERE FilePath=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, c.filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) dup = sqlite3_column_int(st, 0) > 0;
    }
    sqlite3_finalize(st);
    if (dup) return false;

    Json fid = c.folderIds;
    Json fod = Json::object();
    for (auto& kv : c.folderOrders) fod[std::to_string(kv.first)] = kv.second;
    Json tags = c.tags;

    if (sqlite3_prepare_v2(db_, "INSERT INTO Comics (Title,Author,FilePath,CoverPath,PageCount,CurrentPage,"
        "FolderIds,FolderOrders,FolderNames,SourceUrl,Notes,Status,CreatedAt,LastReadAt,Tags) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);", -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_finalize(st); return false;
    }
    sqlite3_bind_text(st, 1, c.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, c.author.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, c.filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, c.coverPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, c.pageCount);
    sqlite3_bind_int(st, 6, c.currentPage);
    sqlite3_bind_text(st, 7, fid.dump().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, fod.dump().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, c.folderNames.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, c.sourceUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 11, c.notes.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 12, StatusToString(c.status), -1, SQLITE_TRANSIENT);
    std::string created = c.createdAt.empty() ? ComicStorageService::NowIso() : c.createdAt;
    sqlite3_bind_text(st, 13, created.c_str(), -1, SQLITE_TRANSIENT);
    if (c.lastReadAt.empty()) sqlite3_bind_null(st, 14);
    else sqlite3_bind_text(st, 14, c.lastReadAt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 15, tags.dump().c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return false;
    c.id = (int)sqlite3_last_insert_rowid(db_);
    const_cast<ComicModel&>(comic).id = c.id;
    return true;
}

void ComicLibraryService::UpdateComic(const ComicModel& comic) {
    Json fid = comic.folderIds;
    Json fod = Json::object();
    for (auto& kv : comic.folderOrders) fod[std::to_string(kv.first)] = kv.second;
    Json tags = comic.tags;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE Comics SET Title=?,Author=?,FilePath=?,CoverPath=?,PageCount=?,CurrentPage=?,"
        "FolderIds=?,FolderOrders=?,FolderNames=?,SourceUrl=?,Notes=?,Status=?,CreatedAt=?,LastReadAt=?,Tags=? WHERE Id=?;",
        -1, &st, nullptr) != SQLITE_OK) { sqlite3_finalize(st); return; }
    sqlite3_bind_text(st, 1, comic.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, comic.author.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, comic.filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, comic.coverPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, comic.pageCount);
    sqlite3_bind_int(st, 6, comic.currentPage);
    sqlite3_bind_text(st, 7, fid.dump().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, fod.dump().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, comic.folderNames.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, comic.sourceUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 11, comic.notes.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 12, StatusToString(comic.status), -1, SQLITE_TRANSIENT);
    std::string created = comic.createdAt.empty() ? ComicStorageService::NowIso() : comic.createdAt;
    sqlite3_bind_text(st, 13, created.c_str(), -1, SQLITE_TRANSIENT);
    if (comic.lastReadAt.empty()) sqlite3_bind_null(st, 14);
    else sqlite3_bind_text(st, 14, comic.lastReadAt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 15, tags.dump().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 16, comic.id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void ComicLibraryService::DeleteComic(int id) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM Comics WHERE Id=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    SyncTagCounts();
}

void ComicLibraryService::TrashComic(int id) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE Comics SET Status='Deleted' WHERE Id=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    SyncTagCounts();
}

void ComicLibraryService::RestoreComic(int id) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE Comics SET Status='Unread' WHERE Id=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

void ComicLibraryService::SaveReadingProgress(int id, int currentPage, int pageCount) {
    ComicModel c;
    if (!GetComicById(id, c)) return;
    c.currentPage = currentPage;
    if (pageCount > 0) c.pageCount = pageCount;
    c.lastReadAt = ComicStorageService::NowIso();
    if (c.status == ComicStatus::Unread) c.status = ComicStatus::Reading;
    if (c.pageCount > 0 && currentPage >= c.pageCount - 1) c.status = ComicStatus::Completed;
    UpdateComic(c);
}

void ComicLibraryService::RebuildFromInfoIfEmpty() {
    // 检查 Comics 是否为空
    sqlite3_stmt* st = nullptr;
    int count = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM Comics;", -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    if (count > 0) return;
    // 扫描 .info 下所有漫画文件夹的 metadata.json 重建
    for (auto& dir : ListDirectories(W(LibraryPath), L".info")) {
        ComicModel c;
        if (ComicStorageService::LoadComic(U8(dir), c)) {
            ImportExistingComic(c);
        }
    }
}

std::vector<FolderModel> ComicLibraryService::BuildFolderTree() {
    struct Row { int id, parent, sort; std::string name; };
    std::vector<Row> rows;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT Id,Name,ParentId,SortOrder FROM Folders ORDER BY SortOrder,Id;", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            Row r;
            r.id = sqlite3_column_int(st, 0);
            r.name = (const char*)sqlite3_column_text(st, 1);
            r.parent = sqlite3_column_int(st, 2);
            r.sort = sqlite3_column_int(st, 3);
            rows.push_back(r);
        }
    }
    sqlite3_finalize(st);

    std::vector<FolderModel> nodes;
    for (auto& r : rows) nodes.push_back(FolderModel{r.id, r.name, r.parent, r.sort, {}});
    std::vector<FolderModel> roots;
    for (auto& n : nodes) {
        if (n.parentId == 0) roots.push_back(n);
        else {
            for (auto& p : nodes) if (p.id == n.parentId) { p.children.push_back(n); break; }
        }
    }
    return roots;
}

FolderModel ComicLibraryService::AddRootFolder(const std::string& name) {
    int sort = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM Folders;", -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) sort = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    FolderModel f;
    if (sqlite3_prepare_v2(db_, "INSERT INTO Folders (Name,ParentId,SortOrder) VALUES (?,0,?);", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, sort);
        if (sqlite3_step(st) == SQLITE_DONE) f.id = (int)sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(st);
    f.name = name;
    return f;
}

FolderModel ComicLibraryService::AddSubfolder(int parentId, const std::string& name) {
    FolderModel f;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO Folders (Name,ParentId,SortOrder) VALUES (?,?,0);", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, parentId);
        if (sqlite3_step(st) == SQLITE_DONE) f.id = (int)sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(st);
    f.name = name; f.parentId = parentId;
    return f;
}

void ComicLibraryService::RenameFolder(int id, const std::string& newName) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE Folders SET Name=? WHERE Id=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, newName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

void ComicLibraryService::DeleteFolder(int id) {
    // 递归删除所有子文件夹
    std::vector<int> children;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT Id FROM Folders WHERE ParentId=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) children.push_back(sqlite3_column_int(st, 0));
    }
    sqlite3_finalize(st);
    for (int ch : children) DeleteFolder(ch);

    // 删除本文件夹
    if (sqlite3_prepare_v2(db_, "DELETE FROM Folders WHERE Id=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    // 移除所有漫画对该文件夹的归属（FolderIds / FolderOrders / FolderNames）
    auto all = GetAllComics();
    for (auto& c : all) {
        bool changed = false;
        auto it = std::remove(c.folderIds.begin(), c.folderIds.end(), id);
        if (it != c.folderIds.end()) { c.folderIds.erase(it, c.folderIds.end()); changed = true; }
        if (c.folderOrders.erase(id)) changed = true;
        if (changed) UpdateComic(c);
    }
}

std::vector<int> ComicLibraryService::GetSiblingIds(int folderId) {
    int parent = GetFolderParent(folderId);
    std::vector<int> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT Id FROM Folders WHERE ParentId=? ORDER BY SortOrder,Id;",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, parent);
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(sqlite3_column_int(st, 0));
    }
    sqlite3_finalize(st);
    return out;
}

void ComicLibraryService::MoveFolder(int sourceId, int targetId) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE Folders SET ParentId=? WHERE Id=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, targetId);
        sqlite3_bind_int(st, 2, sourceId);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

void ComicLibraryService::MoveFolderToRoot(int sourceId) {
    MoveFolder(sourceId, 0);
}

void ComicLibraryService::PersistFolderOrder(const std::vector<int>& siblingIds) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE Folders SET SortOrder=? WHERE Id=?;", -1, &st, nullptr) == SQLITE_OK) {
        for (size_t i = 0; i < siblingIds.size(); i++) {
            sqlite3_reset(st);
            sqlite3_bind_int(st, 1, (int)i);
            sqlite3_bind_int(st, 2, siblingIds[i]);
            sqlite3_step(st);
        }
    }
    sqlite3_finalize(st);
}

int ComicLibraryService::GetFolderParent(int id) {
    int parent = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT ParentId FROM Folders WHERE Id=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) parent = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return parent;
}

bool ComicLibraryService::IsDescendantOf(int candidateId, int ancestorId) {
    int cur = candidateId;
    while (cur != 0) {
        if (cur == ancestorId) return true;
        cur = GetFolderParent(cur);
    }
    return false;
}

std::vector<TagModel> ComicLibraryService::GetAllTags() {
    std::vector<TagModel> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT Id,Name,Count FROM Tags ORDER BY Name;", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            TagModel t;
            t.id = sqlite3_column_int(st, 0);
            t.name = (const char*)sqlite3_column_text(st, 1);
            t.count = sqlite3_column_int(st, 2);
            out.push_back(t);
        }
    }
    sqlite3_finalize(st);
    return out;
}

void ComicLibraryService::SyncTagCounts() {
    // 统计所有非 Deleted 漫画的标签
    std::map<std::string, int> counts;
    for (auto& c : GetAllComics()) {
        if (c.status == ComicStatus::Deleted) continue;
        for (auto& t : c.tags) counts[t]++;
    }
    // 清空并重建 Tags 表
    Exec(db_, "DELETE FROM Tags;");
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO Tags (Name,Count) VALUES (?,?);", -1, &st, nullptr) == SQLITE_OK) {
        for (auto& kv : counts) {
            sqlite3_bind_text(st, 1, kv.first.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, kv.second);
            sqlite3_step(st);
            sqlite3_reset(st);
        }
    }
    sqlite3_finalize(st);
}

bool ComicLibraryService::IsValidComicSource(const std::string& path) {
    std::wstring wp = W(path);
    if (FileExistsW(wp)) {
        std::string ext = Lower(U8(GetExtensionW(wp)));
        return ext == ".cbz" || ext == ".zip";
    }
    if (DirExistsW(wp)) {
        for (auto& f : ListFiles(wp)) {
            std::string ext = Lower(U8(GetExtensionW(f)));
            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" ||
                ext == ".bmp" || ext == ".webp") return true;
        }
    }
    return false;
}

int ComicLibraryService::CountPages(const std::string& path) {
    std::wstring wp = W(path);
    int n = 0;
    if (DirExistsW(wp)) {
        for (auto& f : ListFiles(wp)) {
            std::string ext = Lower(U8(GetExtensionW(f)));
            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" ||
                ext == ".bmp" || ext == ".webp") n++;
        }
    }
    return n;
}

std::string ComicLibraryService::FindCover(const std::string& path) {
    std::wstring wp = W(path);
    if (!DirExistsW(wp)) return std::string();
    for (auto& f : ListFiles(wp)) {
        std::string ext = Lower(U8(GetExtensionW(f)));
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") return U8(f);
    }
    return std::string();
}

std::string ComicLibraryService::SanitizeTitle(const std::string& title) {
    return SanitizeFolderName(title);
}

std::string ComicLibraryService::ComicInfoPath(const std::string& title) {
    return LibraryPath + "\\" + SanitizeTitle(title) + ".info";
}

} // namespace ark