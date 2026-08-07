#pragma once
// 管理器主窗口：标题栏 + 左栏（搜索/快捷导航/文件夹树）+ 中栏（卡片墙/列表/标签管理/分页/导入）
#include "WindowBase.h"
#include "Controls.h"
#include "Models.h"
#include "TrayIcon.h"
#include <string>
#include <vector>
#include <set>

struct IDropTarget;  // 保留前向声明（未使用，避免破坏包含顺序）

namespace ark {
class ComicLibraryService;
}

namespace ark::ui {

// 快捷导航类型（与 C# QuickNavType 一致）
enum class QuickNav { All, Uncategorized, Untagged, PendingSort, TagManager, Reading, Completed, Trash };

class MainWindow : public WindowBase {
public:
    MainWindow();
    ~MainWindow();
    void Run();

protected:
    void OnPaint(ID2D1RenderTarget* rt, int w, int h) override;
    void OnLButtonDown(int x, int y) override;
    void OnLButtonUp(int x, int y) override;
    void OnDoubleClick(int x, int y) override;
    void OnRButtonUp(int x, int y) override;
    void OnMouseMove(int x, int y) override;
    void OnMouseLeave() override;
    void OnKeyDown(UINT vk) override;
    void OnChar(wchar_t ch) override;
    void OnResize(int w, int h) override;
    void OnTimer(UINT id) override;
    void OnMouseWheel(int delta) override;
    void OnDropFiles(HDROP drop) override;
    void OnActivate() override;
    void OnReaderClosed();   // 阅读器关闭后异步刷新进度
    void DrawTitleBarContent(ID2D1RenderTarget* rt, int w, int h) override;
    bool OnCloseRequested() override;                              // 托盘：隐藏而非退出
    void OnAppMessage(UINT msg, WPARAM wParam, LPARAM lParam) override; // 托盘通知

private:
    struct TreeNode {
        int id = 0;
        int parentId = 0;
        std::wstring name;
        int depth = 0;
        bool expanded = false;
        bool hasChildren = false;
        D2D1_RECT_F rect{};
    };

    // ---- 数据 ----
    void LoadLibrary();
    void ReloadFolders();
    void RefreshFiltered();
    std::vector<ComicModel> FilterComics();
    void CollectGuiIds(int folderId, std::set<int>& out) const;
    bool ComicInFolder(const ComicModel& c, const std::set<int>& ids) const;

    // ---- 左栏 ----
    void OpenLibrarySwitcher(); // 汉堡按钮：下拉显示已加载的资源库，点击切换
    void OnSearchChanged();
    void StartSidebarResize(int x);
    void UpdateSidebarResize(int x);
    void EndSidebarResize();

    // ---- 文件夹树 ----
    void RebuildTree();
    void LayoutTree();
    int HitTreeNode(int x, int y) const;
    void ToggleNode(int index);
    void ShowFolderContextMenu(int x, int y, int folderId);
    void DoAddRootFolder();
    void DoAddSubfolder();
    void DoRenameFolder();
    void DoDeleteFolder();
    void DoMoveFolderUp();
    void DoMoveFolderDown();
    void DoMoveFolderToRoot();
    void DoMoveFolder(int sourceId, int targetId);

    // ---- 中栏绘制 ----
    bool IsTagManager() const { return nav_ == QuickNav::TagManager; }
    bool InTagFilter() const { return !tagFilter_.empty(); }
    bool PaginationVisible() const;
    void DrawHeader(ID2D1RenderTarget* rt);
    void DrawCardView(ID2D1RenderTarget* rt);
    void DrawListView(ID2D1RenderTarget* rt);
    void DrawTagManager(ID2D1RenderTarget* rt);
    void DrawPagination(ID2D1RenderTarget* rt);
    void DrawRubberBand(ID2D1RenderTarget* rt);
    void DrawToast(ID2D1RenderTarget* rt);
    void DrawCover(ID2D1RenderTarget* rt, const ComicModel& c, const D2D1_RECT_F& r, float radius);
    void DrawImportProgress(ID2D1RenderTarget* rt);  // 批量导入进度条

    // ---- 中栏几何 ----
    int ContentLeft() const { return sidebarWidth_ + 4; }
    int ContentRight() const { return Width() - 4 - detailWidth_; }
    // 中栏头部紧贴标题栏（无顶部间距），内容区紧随头部
    int HeaderTop() const { return TitleBarHeight; }
    int HeaderBottom() const { return TitleBarHeight + 46; }
    int ContentTop() const { return TitleBarHeight + 48; }
    int ContentBottom() const { return PaginationVisible() ? Height() - 46 : Height() - 8; }
    void ComputeHeaderRects();
    void LayoutCards();
    void LayoutListRows();
    int HitCard(int x, int y) const;
    int HitListRow(int x, int y) const;

    // ---- 右栏详情面板 ----
    int DetailLeft() const  { return Width() - 4 - detailWidth_; }
    int DetailRight() const { return Width() - 4; }
    int DetailTop() const   { return TitleBarHeight; }
    int DetailBottom() const { return Height() - 4; }
    void LayoutDetail();
    void DrawDetailPanel(ID2D1RenderTarget* rt);
    void DrawDetailEmpty(ID2D1RenderTarget* rt);
    void SetDetail(int comicId);
    void ClearDetail();
    void CommitDetail();                        // 把详情编辑写回 DB + metadata.json
    void SaveDetailField();                     // 失焦提交当前编辑字段
    void DoAddDetailTag();                      // 添加标签（逗号分隔多个）
    void ShowDetailTagMenu(int x, int y, const std::string& tag);
    void RefreshFromDetail();                   // 提交后刷新中栏/标签云
    void UpdateDetailResize(int x);             // 右栏宽度拖拽缩放
    void EndDetailResize();
    std::vector<std::wstring> NotesLines(float maxW) const; // 备注包装行
    int NotesHitCaret(int x, int y) const;      // 备注点击定位光标

    // ---- 中栏交互 ----
    void OnHeaderClick(int x, int y);
    void ShowComicContextMenu(int x, int y, int index);
    void DoOpenReader(int index);
    void DoDeleteSelected();
    void DoRestoreSelected();
    void DoEmptyTrash();
    void DoExportSelected();   // 导出选中漫画到指定文件夹（复制源文件）
    void OpenInExplorer(const ComicModel& c);
    void ToggleView();
    void ClearSelection();
    bool IsSelected(int id) const;
    void SelectOnly(int id);
    void ToggleSelected(int id);
    void RefreshPageJumpHover(int x, int y);
    void OpenSettingsWindow();   // 打开设置窗口
    void DoDropToFolder(int nodeIndex);  // 拖拽卡片到文件夹归类
    void DrawDragCard(ID2D1RenderTarget* rt); // 拖拽中的浮动卡片动画

private:
    // 批量导入进度状态
    bool importing_ = false;
    int importProgress_ = 0;  // 0-100
    int importOk_ = 0;

    // ---- 托盘 ----
    void ShowMainFromTray();     // 从托盘恢复主窗口
    void ShowTrayMenu();         // 托盘右键菜单
    static constexpr UINT kTrayMsg = WM_APP + 1;

    // ---- 标签管理 ----
    void ReloadTags();
    void EnterTagFilter(const std::string& tag);
    void LeaveTagFilter();
    void LayoutTagChips();
    int HitTagChip(int x, int y);
    void ShowTagContextMenu(int x, int y, int tagIndex); // 右键 chip：重命名/删除
    void DoRenameTag(int tagIndex);
    void DoDeleteTag(int tagIndex);
    void RenameTag(const std::string& oldName, const std::string& newName); // 跨所有漫画重命名标签
    void DeleteTag(const std::string& tag);                                 // 跨所有漫画删除标签
    std::vector<D2D1_RECT_F> tagChipRects_;
    int tagHover_ = -1;  // 标签管理 chip 悬停索引（-1=无）

    // ---- 分页 ----
    void ApplyPagination();
    void SetPage(int p);
    void PrevPage() { SetPage(page_ - 1); }
    void NextPage() { SetPage(page_ + 1); }
    void DoJumpPage();

    // ---- 导入 / Toast ----
    void ImportPaths(const std::vector<std::wstring>& paths, int targetFolderId);
    void ShowToast(const std::wstring& msg);

    // ---- 布局几何（左栏） ----
    int SidebarWidth() const { return sidebarWidth_; }
    int TreeTop() const { return 268; }
    int TreeBottom() const { return Height() - 40; }
    D2D1_RECT_F SearchBoxRect() const;
    D2D1_RECT_F NavRect(int i) const;
    D2D1_RECT_F TreeHeaderRect() const;

    // 数据
    ComicLibraryService* service_ = nullptr;
    std::vector<ComicModel> comics_;
    std::vector<FolderModel> folders_;
    std::vector<ComicModel> filtered_;
    std::vector<ComicModel> pageComics_;

    // 筛选状态
    QuickNav nav_ = QuickNav::All;
    std::wstring searchText_;
    int selectedFolderId_ = -1;
    std::string tagFilter_;   // 非空=标签筛选视图

    // 树 UI
    std::vector<TreeNode> nodes_;
    std::set<int> expanded_;
    int selectedNode_ = -1;
    int hoverNode_ = -1;
    int treeScroll_ = 0;

    // 左栏布局
    int sidebarWidth_ = 260;
    D2D1_RECT_F hamburgerRect_{};
    bool hamburgerHover_ = false;
    TextBox searchBox_;
    bool searchFocused_ = false;
    int navHover_ = -1;
    bool resizing_ = false;
    int resizeStartX_ = 0;
    int resizeStartW_ = 0;

    // 搜索防抖
    static constexpr UINT kSearchTimer = 1;
    bool searchDirty_ = false;

    // 右键菜单目标
    int contextFolderId_ = -1;

    // ---- 中栏状态 ----
    bool cardView_ = true;
    int cardSize_ = 180;
    std::vector<int> selectedIds_;
    int anchorIndex_ = -1;          // Shift 多选锚点
    int page_ = 1;
    int pageSize_ = 60;
    int totalPages_ = 1;
    std::vector<TagModel> tags_;

    // 中栏滚动 / 悬停
    int cardScroll_ = 0, listScroll_ = 0, tagScroll_ = 0;
    int hoverCard_ = -1, hoverListRow_ = -1;
    int hoverHeaderBtn_ = -1;       // 0=卡片 1=列表 2=语言 3=设置
    int hoverPageBtn_ = -1;         // 0=上一页 1=确认 2=下一页
    D2D1_RECT_F pageBox_{};         // 跳页输入框

    // 头部按钮几何
    D2D1_RECT_F backRect_, cardViewRect_, listViewRect_, sliderRect_, langRect_, settingsRect_;
    D2D1_RECT_F sliderThumbRect_;
    bool sliderDragging_ = false;

    // 卡片/列表几何
    std::vector<D2D1_RECT_F> cardRects_;
    std::vector<D2D1_RECT_F> listRowRects_;

    // 橡皮筋框选
    bool rubber_ = false;
    POINT ruStart_{}, ruEnd_{};

    // 卡片拖拽到左栏文件夹（软件内部拖拽）
    bool dragArmed_ = false;     // 按下卡片但尚未越过拖拽阈值
    bool cardDragging_ = false;  // 正在拖拽卡片
    POINT dragStart_{};          // 拖拽起点
    POINT dragPos_{};            // 拖拽实时位置（浮动卡片跟随）
    int dragHoverNode_ = -1;     // 拖拽悬停的目标文件夹节点索引
    bool dragCardCached_ = false; // 拖拽封面已缓存
    ComicModel dragCard_;        // 拖拽中的漫画（第一张选中）

    // 跳页输入
    bool pageJumpFocused_ = false;
    std::wstring pageJumpText_;

    // Toast
    std::wstring toast_;
    std::string toastColorTag_;
    bool toastVisible_ = false;
    static constexpr UINT kToastTimer = 2;

    // ---- 右栏详情面板状态 ----
    int detailWidth_ = 320;
    bool detailResizing_ = false;
    int detailResizeStartX_ = 0, detailResizeStartW_ = 0;
    int detailScroll_ = 0;                 // 详情内容滚动偏移
    ComicModel detail_;                    // 当前详情漫画（工作副本）
    int detailId_ = -1;
    bool detailValid_ = false;
    TextBox dTitle_, dAuthor_;             // 标题/作者单行编辑
    std::wstring dNotes_; int dNotesCaret_ = 0; bool dNotesFocused_ = false;
    std::string dEditField_;               // 正在编辑的字段："" ”title" "author" "notes"
    int detailHover_ = -1;                 // 0=封面 1=添加标签 2=打开阅读
    int detailHoverTag_ = -1;              // 详情标签 chip 悬停
    int detailContentH_ = 0;               // 详情内容总高（滚动用）
    D2D1_RECT_F dCoverRect_{}, dOpenRect_{}, dAddTagRect_{}, dNotesRect_{}, dTitleRect_{}, dAuthorRect_{};
    D2D1_RECT_F dAuthorLabelRect_{}, dFolderRect_{}, dCreatedRect_{}, dProgRect_{}, dStatusRect_{}, dTagLabelRect_{};
    std::vector<D2D1_RECT_F> dTagRects_;   // 详情标签 chip
    // 拖拽封面替换状态
    bool dCoverHover_ = false;
    std::vector<std::wstring> dNotesDisplay_; // 备注包装行（绘制用）

    // 托盘
    TrayIcon tray_;
};

} // namespace ark::ui