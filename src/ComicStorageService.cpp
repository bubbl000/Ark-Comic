#include "ComicStorageService.h"
#include "LibraryManager.h"
#include "ArchiveExtractor.h"
#include "FileUtil.h"
#include "JsonUtil.h"
#include "Utf.h"
#include <ctime>
#include <stdexcept>

namespace ark {

std::string ComicStorageService::NowIso() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string ComicStorageService::GetComicInfoPath(const ComicModel& comic) {
    const auto& lib = LibraryManager::CurrentLibraryPath();
    if (lib.empty()) return std::string();
    return lib + "\\" + SanitizeFolderName(comic.title) + ".info";
}

std::string ComicStorageService::EnsureComicInfoFolder(const ComicModel& comic) {
    const auto& lib = LibraryManager::CurrentLibraryPath();
    if (lib.empty()) throw std::runtime_error("尚未配置资源库。");
    std::wstring infoDir = W(lib) + L"\\" + W(SanitizeFolderName(comic.title)) + L".info";
    CreateDirRecursive(infoDir);
    CreateDirRecursive(infoDir + L"\\images");
    return U8(infoDir);
}

// metadata.json 序列化（键名与 C# 版完全一致）
static Json MetadataToJson(const ComicModel& c) {
    Json j;
    j["Id"] = c.id;
    j["Title"] = c.title;
    j["FilePath"] = c.filePath;
    j["CoverPath"] = c.coverPath;
    j["PageCount"] = c.pageCount;
    j["CurrentPage"] = c.currentPage;
    j["FolderIds"] = c.folderIds;
    j["FolderOrders"] = Json::object();
    for (auto& kv : c.folderOrders) j["FolderOrders"][std::to_string(kv.first)] = kv.second;
    j["FolderNames"] = c.folderNames;
    j["SourceUrl"] = c.sourceUrl;
    j["Notes"] = c.notes;
    j["Status"] = StatusToString(c.status);
    j["CreatedAt"] = c.createdAt.empty() ? ComicStorageService::NowIso() : c.createdAt;
    j["LastReadAt"] = c.lastReadAt.empty() ? Json(nullptr) : Json(c.lastReadAt);
    return j;
}

static bool JsonToComicMetadata(const Json& j, ComicModel& c) {
    if (j.is_null()) return false;
    try {
        c.id = j.value("Id", 0);
        c.title = j.value("Title", "");
        c.author = j.value("Author", "");
        c.filePath = j.value("FilePath", "");
        c.coverPath = j.value("CoverPath", "");
        c.pageCount = j.value("PageCount", 0);
        c.currentPage = j.value("CurrentPage", 0);
        c.folderNames = j.value("FolderNames", "未分类");
        c.sourceUrl = j.value("SourceUrl", "");
        c.notes = j.value("Notes", "");
        c.status = StatusFromString(j.value("Status", std::string("Unread")));
        c.createdAt = j.value("CreatedAt", "");
        c.lastReadAt = j.value("LastReadAt", "");
        if (j.contains("FolderIds") && j["FolderIds"].is_array()) {
            c.folderIds.clear();
            for (auto& v : j["FolderIds"]) c.folderIds.push_back(v.get<int>());
        }
        if (j.contains("FolderOrders") && j["FolderOrders"].is_object()) {
            c.folderOrders.clear();
            for (auto& [k, v] : j["FolderOrders"].items()) {
                c.folderOrders[std::stoi(k)] = v.get<int>();
            }
        }
        if (j.contains("LastReadAt") && j["LastReadAt"].is_null()) c.lastReadAt.clear();
        return true;
    } catch (...) {
        return false;
    }
}

void ComicStorageService::SaveComicData(const ComicModel& comic) {
    std::wstring infoDir = W(EnsureComicInfoFolder(comic));
    WriteJsonFile(infoDir + L"\\metadata.json", MetadataToJson(comic));
    // tags.json —— 标签数组
    Json tj = Json::array();
    for (auto& t : comic.tags) tj.push_back(t);
    WriteJsonFile(infoDir + L"\\tags.json", tj);
}

bool ComicStorageService::LoadComic(const std::string& infoDir, ComicModel& out) {
    std::wstring metaFile = W(infoDir) + L"\\metadata.json";
    if (!FileExistsW(metaFile)) return false;
    Json j;
    if (!ReadJsonFile(metaFile, j)) return false;
    if (!JsonToComicMetadata(j, out)) return false;
    // tags.json
    std::wstring tagsFile = W(infoDir) + L"\\tags.json";
    Json tj;
    if (ReadJsonFile(tagsFile, tj) && tj.is_array()) {
        out.tags.clear();
        for (auto& t : tj) if (t.is_string()) out.tags.push_back(t.get<std::string>());
    }
    return true;
}

std::vector<ComicModel> ComicStorageService::LoadAllComics() {
    std::vector<ComicModel> list;
    for (auto& dir : LibraryManager::EnumerateComicFolders()) {
        ComicModel c;
        if (LoadComic(dir, c)) list.push_back(c);
    }
    return list;
}

static bool IsImageFileW(const std::wstring& path) {
    std::wstring ext = GetExtensionW(path);
    for (auto& c : ext) c = towlower(c);
    return ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".gif" ||
           ext == L".bmp" || ext == L".webp";
}

static void TryCacheCover(ComicModel& comic, const std::wstring& imagesDir) {
    if (comic.coverPath.empty()) return;
    std::wstring cp = W(comic.coverPath);
    if (FileExistsW(cp)) {
        std::wstring ext = GetExtensionW(cp);
        if (ext.empty()) ext = L".jpg";
        std::wstring dest = imagesDir + L"\\cover" + ext;
        if (!FileExistsW(dest)) CopyFileW(cp.c_str(), dest.c_str(), FALSE);
        comic.coverPath = U8(dest);
    }
}

static void CopyCoverToImages(const std::wstring& sourceFolder, const std::wstring& imagesDir, ComicModel& comic) {
    for (auto& f : ListFiles(sourceFolder)) {
        if (IsImageFileW(f)) {
            std::wstring dest = imagesDir + L"\\cover" + GetExtensionW(f);
            if (!FileExistsW(dest)) CopyFileW(f.c_str(), dest.c_str(), FALSE);
            comic.coverPath = U8(dest);
            break;
        }
    }
}

// 压缩包自动封面：提取第一张页面图作为封面缓存（无封面时才生成）
static void GenerateArchiveCover(ComicModel& comic, const std::wstring& imagesDir) {
    if (!comic.coverPath.empty()) return;
    auto pages = ArchiveExtractor::GetPages(comic.filePath);
    if (pages.empty()) return;
    std::wstring first = W(pages[0]);
    if (!FileExistsW(first)) return;
    std::wstring ext = GetExtensionW(first);
    if (ext.empty()) ext = L".jpg";
    std::wstring dest = imagesDir + L"\\cover" + ext;
    if (!FileExistsW(dest)) CopyFileW(first.c_str(), dest.c_str(), FALSE);
    comic.coverPath = U8(dest);
}

void ComicStorageService::MoveSourceIntoInfo(ComicModel& comic, const std::string& sourcePath, bool copySource) {
    std::wstring infoDir = W(EnsureComicInfoFolder(comic));
    std::wstring imagesDir = infoDir + L"\\images";
    std::wstring src = W(sourcePath);

    if (DirExistsW(src)) {
        // 源是文件夹：移动到 source/
        std::wstring destFolder = infoDir + L"\\source";
        if (!DirExistsW(destFolder)) {
            if (copySource) {
                CreateDirRecursive(destFolder);
                for (auto& f : ListFiles(src)) {
                    std::wstring dst = destFolder + L"\\" + GetFileNameW(f);
                    CopyFileW(f.c_str(), dst.c_str(), FALSE);
                }
            } else {
                MoveFileW(src.c_str(), destFolder.c_str());
            }
        }
        CopyCoverToImages(src, imagesDir, comic);
        comic.filePath = U8(destFolder);
    } else if (FileExistsW(src)) {
        // 源是压缩包：移动到 .info 内
        std::wstring destFile = infoDir + L"\\" + GetFileNameW(src);
        if (!FileExistsW(destFile)) {
            if (copySource) CopyFileW(src.c_str(), destFile.c_str(), FALSE);
            else MoveFileW(src.c_str(), destFile.c_str());
        }
        comic.filePath = U8(destFile);
        // 压缩包无现成封面：提取第一页作为封面
        GenerateArchiveCover(comic, imagesDir);
    }
    TryCacheCover(comic, imagesDir);
}

void ComicStorageService::CacheCoverOnly(ComicModel& comic) {
    std::wstring infoDir = W(EnsureComicInfoFolder(comic));
    TryCacheCover(comic, infoDir + L"\\images");
}

void ComicStorageService::DeleteComicInfo(const ComicModel& comic) {
    std::string infoDir = GetComicInfoPath(comic);
    if (!infoDir.empty()) DeleteDirRecursiveW(W(infoDir));
}

void ComicStorageService::ReplaceCover(ComicModel& comic, const std::string& imageFilePath) {
    if (!FileExistsW(W(imageFilePath))) return;
    std::wstring infoDir = W(GetComicInfoPath(comic));
    std::wstring imagesDir = infoDir + L"\\images";
    CreateDirRecursive(imagesDir);
    // 删除旧封面（任意扩展名）
    for (auto& f : ListFiles(imagesDir)) {
        std::wstring name = GetFileNameW(f);
        if (name.rfind(L"cover.", 0) == 0) DeleteFileW(f.c_str());
    }
    std::wstring ext = GetExtensionW(W(imageFilePath));
    if (ext.empty()) ext = L".jpg";
    std::wstring dest = imagesDir + L"\\cover" + ext;
    CopyFileW(W(imageFilePath).c_str(), dest.c_str(), TRUE);
    comic.coverPath = U8(dest);
    SaveComicData(comic);
}

} // namespace ark