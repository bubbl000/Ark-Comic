#include "MainWindow.h"
#include "Ui.h"
#include "Theme.h"
#include "Dialogs.h"
#include "MsgBox.h"
#include "LibraryManager.h"
#include "AppConfig.h"
#include "ComicLibraryService.h"
#include "ComicStorageService.h"
#include "SettingsWindow.h"
#include "ReaderWindow.h"
#include "Models.h"
#include "FileUtil.h"
#include "PickFolder.h"
#include "Utf.h"
#include "ActivityLog.h"
#include "I18n.h"
#include <windowsx.h>
#include <shellapi.h>
#include <d2d1helper.h>
#include <algorithm>
#include <cwctype>
#include <functional>

namespace ark::ui {

static std::wstring WLower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = (wchar_t)towlower(c);
    return r;
}

MainWindow::MainWindow() : WindowBase(1400, 900, L"Ark Comic", true) {
    SetResizable(true);
    SetMinSize(1000, 650);
    SetTitleButtons(true, true);
    LoadLibrary();
}

MainWindow::~MainWindow() {
    if (service_) delete service_;
}

void MainWindow::Run() {
    Create(nullptr, true);
    tray_.Add(Hwnd(), kTrayMsg);
    RunModal();
    tray_.Remove();
}

// ==================== 数据加载 ====================
void MainWindow::LoadLibrary() {
    std::string lib = LibraryManager::CurrentLibraryPath();
    if (lib.empty()) return;
    // 性能遥测：资源库扫描耗时（超阈值自动记为 stall）
    PerfScope ps(L"资源库", "LoadLibrary");
    if (service_) { delete service_; service_ = nullptr; }
    service_ = new ComicLibraryService(lib);
    service_->RebuildFromInfoIfEmpty();
    comics_ = service_->GetAllComics();
    ReloadFolders();
    ReloadTags();
    RefreshFiltered();
    // 性能遥测：快照记录资源库路径 + 漫画数量
    {
        std::lock_guard<std::mutex> lk(PerfState::Mutex());
        PerfState::Current().libraryPath = W(lib);
    }
    ps.SetExtra({Perf::N("comics", (double)comics_.size()),
                 Perf::S("library", ActivityFmt::NarrowUtf8(W(lib)))});
    ActivityLog::Instance().Log(L"资源库", L"加载完成: " + W(lib) + L"，共 " + std::to_wstring(comics_.size()) + L" 本漫画");
}

void MainWindow::ReloadFolders() {
    folders_.clear();
    if (service_) folders_ = service_->BuildFolderTree();
    RebuildTree();
}

void MainWindow::RebuildTree() {
    int sel = -1;
    if (selectedNode_ >= 0 && selectedNode_ < (int)nodes_.size()) sel = nodes_[selectedNode_].id;
    nodes_.clear();
    std::function<void(const FolderModel&, int)> walk = [&](const FolderModel& f, int depth) {
        TreeNode n;
        n.id = f.id; n.parentId = f.parentId;
        n.name = W(f.name);
        n.depth = depth;
        n.hasChildren = !f.children.empty();
        n.expanded = expanded_.count(f.id) > 0;
        nodes_.push_back(n);
        if (n.expanded) for (auto& ch : f.children) walk(ch, depth + 1);
    };
    for (auto& r : folders_) walk(r, 0);
    selectedNode_ = -1;
    for (size_t i = 0; i < nodes_.size(); i++)
        if (nodes_[i].id == sel) { selectedNode_ = (int)i; break; }
    LayoutTree();
}

void MainWindow::LayoutTree() {
    int rowH = 30;
    int top = TreeTop();
    for (size_t i = 0; i < nodes_.size(); i++) {
        int y = top + treeScroll_ + (int)i * rowH;
        nodes_[i].rect = D2D1::RectF(12, (float)y, (float)(sidebarWidth_ - 12), (float)(y + rowH));
    }
}

int MainWindow::HitTreeNode(int x, int y) const {
    if (x < 0 || x > sidebarWidth_ || y < TreeTop() || y > TreeBottom()) return -1;
    for (size_t i = 0; i < nodes_.size(); i++) {
        auto& r = nodes_[i].rect;
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return (int)i;
    }
    return -1;
}

void MainWindow::ToggleNode(int index) {
    if (index < 0 || index >= (int)nodes_.size()) return;
    int id = nodes_[index].id;
    if (expanded_.count(id)) expanded_.erase(id);
    else expanded_.insert(id);
    RebuildTree();
    Invalidate();
}

// ==================== 筛选 ====================
void MainWindow::CollectGuiIds(int folderId, std::set<int>& out) const {
    std::vector<std::pair<int, int>> flat;
    std::function<void(const std::vector<FolderModel>&)> flatten = [&](const std::vector<FolderModel>& list) {
        for (auto& f : list) { flat.push_back({ f.id, f.parentId }); flatten(f.children); }
    };
    flatten(folders_);
    out.insert(folderId);
    std::vector<int> frontier = { folderId };
    while (!frontier.empty()) {
        std::vector<int> next;
        for (auto& pr : flat)
            if (std::find(frontier.begin(), frontier.end(), pr.second) != frontier.end())
                if (out.insert(pr.first).second) next.push_back(pr.first);
        frontier = std::move(next);
    }
}

bool MainWindow::ComicInFolder(const ComicModel& c, const std::set<int>& ids) const {
    for (int f : c.folderIds) if (ids.count(f)) return true;
    return false;
}

std::vector<ComicModel> MainWindow::FilterComics() {
    std::vector<ComicModel> out;
    // 标签管理视图（未进入标签筛选）→ 显示标签云，不列出漫画
    if (IsTagManager() && !InTagFilter()) return out;
    std::set<int> ids;
    if (selectedFolderId_ > 0) CollectGuiIds(selectedFolderId_, ids);
    std::wstring low = WLower(searchText_);
    for (auto& c : comics_) {
        bool keep;
        if (selectedFolderId_ > 0) {
            keep = c.status != ComicStatus::Deleted && ComicInFolder(c, ids);
        } else if (InTagFilter()) {
            keep = c.status != ComicStatus::Deleted;
            if (keep) {
                bool has = false;
                for (auto& t : c.tags) if (t == tagFilter_) { has = true; break; }
                keep = has;
            }
        } else {
            switch (nav_) {
                case QuickNav::All: keep = c.status != ComicStatus::Deleted; break;
                case QuickNav::Uncategorized: keep = c.folderIds.empty() && c.status != ComicStatus::Deleted; break;
                case QuickNav::Untagged: keep = c.tags.empty() && c.status != ComicStatus::Deleted; break;
                case QuickNav::PendingSort: keep = c.folderIds.empty() && c.tags.empty() && c.status != ComicStatus::Deleted; break;
                case QuickNav::Reading: keep = c.status == ComicStatus::Reading; break;
                case QuickNav::Completed: keep = c.status == ComicStatus::Completed; break;
                case QuickNav::Trash: keep = c.status == ComicStatus::Deleted; break;
                default: keep = false;
            }
        }
        if (!keep) continue;
        if (!low.empty()) {
            bool hit = WLower(W(c.title)).find(low) != std::wstring::npos;
            if (!hit)
                for (auto& t : c.tags)
                    if (WLower(W(t)).find(low) != std::wstring::npos) { hit = true; break; }
            if (!hit) continue;
        }
        out.push_back(c);
    }
    return out;
}

void MainWindow::ApplyPagination() {
    if (IsTagManager() && !InTagFilter()) { pageComics_.clear(); totalPages_ = 1; page_ = 1; return; }
    totalPages_ = (int)filtered_.size() / pageSize_ + (((int)filtered_.size() % pageSize_) ? 1 : 0);
    if (totalPages_ < 1) totalPages_ = 1;
    if (page_ > totalPages_) page_ = totalPages_;
    if (page_ < 1) page_ = 1;
    int start = (page_ - 1) * pageSize_;
    pageComics_.clear();
    for (size_t i = start; i < filtered_.size() && i < (size_t)(start + pageSize_); i++)
        pageComics_.push_back(filtered_[i]);
}

void MainWindow::SetPage(int p) {
    if (p < 1) p = 1;
    if (p > totalPages_) p = totalPages_;
    if (p == page_) return;
    page_ = p;
    cardScroll_ = 0; listScroll_ = 0;
    ApplyPagination();
    LayoutCards(); LayoutListRows();
    Invalidate();
}

void MainWindow::RefreshFiltered() {
    filtered_ = FilterComics();
    ApplyPagination();
    ComputeHeaderRects();
    LayoutCards();
    LayoutListRows();
    LayoutTagChips();
    Invalidate();
}

// ==================== 布局几何 ====================
D2D1_RECT_F MainWindow::SearchBoxRect() const {
    return D2D1::RectF(12, 88, (float)(sidebarWidth_ - 12), 120);
}
D2D1_RECT_F MainWindow::NavRect(int i) const {
    int top = 128 + i * 31;
    return D2D1::RectF(12, (float)top, (float)(sidebarWidth_ - 12), (float)(top + 30));
}
D2D1_RECT_F MainWindow::TreeHeaderRect() const {
    return D2D1::RectF(16, 236, (float)(sidebarWidth_ - 16), 264);
}

bool MainWindow::PaginationVisible() const {
    if (IsTagManager() && !InTagFilter()) return false;
    return totalPages_ > 1;
}

void MainWindow::ComputeHeaderRects() {
    float hr = (float)ContentRight() - 16;
    float hb = (float)HeaderTop(), ht = (float)HeaderBottom();
    float bTop = hb + 9, bBot = ht - 9;
    settingsRect_ = D2D1::RectF(hr - 32, bTop, hr, bBot); hr -= 32 + 8;
    langRect_ = D2D1::RectF(hr - 56, bTop, hr, bBot); hr -= 56 + 12;
    sliderRect_ = D2D1::RectF(hr - 120, hb + 11, hr, hb + 37); hr -= 120 + 12;
    listViewRect_ = D2D1::RectF(hr - 32, bTop, hr, bBot); hr -= 32 + 4;
    cardViewRect_ = D2D1::RectF(hr - 32, bTop, hr, bBot); hr -= 32 + 8;
    backRect_ = D2D1::RectF((float)ContentLeft() + 16, bTop, (float)ContentLeft() + 42, bBot);
    // 滑条圆点
    float sw = sliderRect_.right - sliderRect_.left;
    float t = (cardSize_ - 120) / 160.0f;
    sliderThumbRect_ = D2D1::RectF(sliderRect_.left + t * (sw - 8), sliderRect_.top,
                                   sliderRect_.left + t * (sw - 8) + 8, sliderRect_.bottom);
}

void MainWindow::LayoutCards() {
    cardRects_.clear();
    int cw = cardSize_, ch = (int)(cardSize_ * 1.4f);
    int cellW = cw + 12, cellH = ch + 12;
    int usable = (ContentRight() - ContentLeft()) - 20;
    int perRow = usable / cellW; if (perRow < 1) perRow = 1;
    int top = ContentTop() + 10 - cardScroll_;
    for (size_t i = 0; i < pageComics_.size(); i++) {
        int col = (int)i % perRow, row = (int)i / perRow;
        int x = ContentLeft() + 10 + col * cellW;
        int y = top + row * cellH;
        cardRects_.push_back(D2D1::RectF((float)x, (float)y, (float)(x + cw), (float)(y + ch)));
    }
}

void MainWindow::LayoutListRows() {
    listRowRects_.clear();
    int rowH = 56;
    int top = ContentTop() + 36 - listScroll_;
    for (size_t i = 0; i < pageComics_.size(); i++) {
        int y = top + (int)i * rowH;
        listRowRects_.push_back(D2D1::RectF((float)ContentLeft(), (float)y, (float)ContentRight(), (float)(y + rowH)));
    }
}

int MainWindow::HitCard(int x, int y) const {
    // 内容可视区之外不命中（避免滚动上移的卡片穿透到工具栏/标题栏区域）
    if (y < ContentTop() || y > ContentBottom()) return -1;
    for (size_t i = 0; i < cardRects_.size(); i++) {
        auto& r = cardRects_[i];
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return (int)i;
    }
    return -1;
}

int MainWindow::HitListRow(int x, int y) const {
    if (y < ContentTop() || y > ContentBottom()) return -1;
    for (size_t i = 0; i < listRowRects_.size(); i++) {
        auto& r = listRowRects_[i];
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return (int)i;
    }
    return -1;
}

// ==================== 绘制 ====================
void MainWindow::DrawTitleBarContent(ID2D1RenderTarget* rt, int w, int h) {
    D2D1_RECT_F ic{ 14, 0, 38, (float)TitleBarHeight };
    D2D::Icon(rt, L"\xE737", ic, theme::AccentCyan(), 14);
    D2D1_RECT_F name{ 40, 0, 160, (float)TitleBarHeight };
    D2D::Text(rt, L"Ark Comic", name, theme::TextPrimary(), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD,
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_RECT_F brand{ 130, 0, 260, (float)TitleBarHeight };
    D2D::Text(rt, i18n::Tr(L"漫画管理器", L"Comic Manager"), brand, theme::TextSecondary(), 12, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void MainWindow::OnPaint(ID2D1RenderTarget* rt, int w, int h) {
    // 左侧栏背景
    D2D1_RECT_F sidebar{ 0, (float)TitleBarHeight, (float)sidebarWidth_, (float)h };
    D2D::RoundedRect(rt, sidebar, 0, theme::BgSidebar());

    // 区域标题：汉堡 + 资源库
    hamburgerRect_ = D2D1::RectF(20, (float)TitleBarHeight + 16, 48, (float)TitleBarHeight + 44);
    if (hamburgerHover_) D2D::RoundedRect(rt, hamburgerRect_, 6, theme::BgCardHover());
    D2D::Icon(rt, L"\xE700", hamburgerRect_, theme::TextPrimary(), 16);
    D2D1_RECT_F titleRect{ 56, (float)TitleBarHeight + 14, (float)sidebarWidth_ - 12, (float)TitleBarHeight + 46 };
    D2D::Text(rt, i18n::Tr(L"资源库", L"Library"), titleRect, theme::TextPrimary(), 15, DWRITE_FONT_WEIGHT_SEMI_BOLD,
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // 搜索框
    auto srect = SearchBoxRect();
    D2D::RoundedRect(rt, srect, 6, D2D1::ColorF(1, 1, 1, 0.06f), theme::BorderColor(), 1.0f);
    D2D1_RECT_F srchIc{ srect.left + 8, srect.top, srect.left + 26, srect.bottom };
    D2D::Icon(rt, L"\xE721", srchIc, theme::TextSecondary(), 12);
    D2D1_RECT_F srchTxt{ srect.left + 30, srect.top, srect.right - 8, srect.bottom };
    if (searchText_.empty() && !searchFocused_)
        D2D::Text(rt, i18n::Tr(L"搜索漫画", L"Search comics"), srchTxt, theme::TextSecondary(), 13, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    else
        D2D::Text(rt, searchText_, srchTxt, theme::TextPrimary(), 13, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // 快捷导航（全部 / 标签管理 / 回收站）
    struct NavDef { const wchar_t* icon; QuickNav nav; };
    static const NavDef navs[3] = {
        { L"\xE80F", QuickNav::All },
        { L"\xE8EC", QuickNav::TagManager },
        { L"\xE74D", QuickNav::Trash },
    };
    bool folderSelected = selectedFolderId_ > 0;
    for (int i = 0; i < 3; i++) {
        auto r = NavRect(i);
        bool sel = !folderSelected && nav_ == navs[i].nav;
        bool hv = navHover_ == i;
        D2D1_COLOR_F bg = sel ? theme::BgMain() : (hv ? theme::BgCardHover() : theme::BgSidebar());
        D2D::RoundedRect(rt, r, 6, bg);
        D2D1_COLOR_F fc = sel ? theme::AccentCyan() : theme::TextPrimary();
        D2D1_RECT_F ic{ r.left + 10, r.top, r.left + 26, r.bottom };
        D2D::Icon(rt, navs[i].icon, ic, fc, 13);
        std::wstring navText;
        switch (navs[i].nav) {
            case QuickNav::All: navText = i18n::Tr(L"全部", L"All"); break;
            case QuickNav::TagManager: navText = i18n::Tr(L"标签管理", L"Tags"); break;
            case QuickNav::Trash: navText = i18n::Tr(L"回收站", L"Trash"); break;
            default: break;
        }
        D2D1_RECT_F tr{ r.left + 32, r.top, r.right - 8, r.bottom };
        D2D::Text(rt, navText, tr, fc, 12, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // 分隔线
    D2D1_RECT_F sep{ 16, 228, (float)(sidebarWidth_ - 16), 229 };
    D2D::RoundedRect(rt, sep, 0, theme::BorderColor());

    // 树标题行
    auto th = TreeHeaderRect();
    D2D1_RECT_F thTxt{ th.left, th.top, th.right - 30, th.bottom };
    D2D::Text(rt, i18n::Tr(L"虚拟文件夹", L"Virtual Folders"), thTxt, theme::TextSecondary(), 12, DWRITE_FONT_WEIGHT_SEMI_BOLD,
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_RECT_F newBtn{ th.right - 24, th.top + 4, th.right, th.top + 28 };
    D2D::Icon(rt, L"\xE710", newBtn, theme::TextSecondary(), 12);

    // 文件夹树
    for (size_t i = 0; i < nodes_.size(); i++) {
        auto& n = nodes_[i];
        auto& r = n.rect;
        if (r.bottom < TreeTop() || r.top > TreeBottom()) continue;
        bool sel = ((int)i == selectedNode_);
        bool hv = ((int)i == hoverNode_);
        bool dropHv = (cardDragging_ && (int)i == dragHoverNode_);
        if (dropHv) D2D::RoundedRect(rt, r, 6, theme::AccentCyan());
        else if (sel) D2D::RoundedRect(rt, r, 6, theme::BgMain());
        else if (hv) D2D::RoundedRect(rt, r, 6, theme::BgCardHover());
        int indent = 8 + n.depth * 16;
        D2D1_RECT_F arrow{ r.left + indent, r.top, r.left + indent + 16, r.bottom };
        D2D::Icon(rt, n.expanded ? L"\xE70D" : L"\xE76C", arrow, theme::TextSecondary(), 10);
        D2D1_RECT_F fic{ r.left + indent + 18, r.top, r.left + indent + 36, r.bottom };
        D2D::Icon(rt, L"\xE8B7", fic, theme::TextSecondary(), 14);
        D2D1_RECT_F tr{ r.left + indent + 38, r.top, r.right - 8, r.bottom };
        D2D::Text(rt, n.name, tr, dropHv ? theme::AccentText() : (sel ? theme::AccentCyan() : theme::TextPrimary()), 13,
                  DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // ---- 中栏 ----
    if (!service_) {
        // 无资源库引导
        float cx = (ContentLeft() + ContentRight()) / 2.0f;
        D2D1_RECT_F ic{ cx - 40, (float)(Height() / 2 - 70), cx + 40, (float)(Height() / 2 - 22) };
        D2D::Icon(rt, L"\xE8B9", ic, D2D1::ColorF(0x3A, 0x40, 0x50, 1.0f), 40);
        D2D1_RECT_F tc{ (float)ContentLeft() + 20, (float)(Height() / 2 - 16), (float)ContentRight() - 20, (float)(Height() / 2 + 8) };
        D2D::Text(rt, i18n::Tr(L"请通过左栏资源库菜单创建或打开资源库", L"Create or open a library via the Library menu in the left sidebar"), tc, theme::TextSecondary(), 13,
                  DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else if (IsTagManager() && !InTagFilter()) {
        DrawTagManager(rt);
    } else if (filtered_.empty()) {
        // 空结果提示
        float cx = (ContentLeft() + ContentRight()) / 2.0f;
        D2D1_RECT_F ic{ cx - 40, (float)(Height() / 2 - 70), cx + 40, (float)(Height() / 2 - 22) };
        D2D::Icon(rt, L"\xE8B9", ic, D2D1::ColorF(0x3A, 0x40, 0x50, 1.0f), 40);
        D2D1_RECT_F tc{ (float)ContentLeft() + 20, (float)(Height() / 2 - 16), (float)ContentRight() - 20, (float)(Height() / 2 + 8) };
        D2D::Text(rt, i18n::Tr(L"没有找到漫画", L"No comics found"), tc, theme::TextSecondary(), 13, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else if (cardView_) DrawCardView(rt);
    else DrawListView(rt);
    // 顶部工具栏在卡片之后绘制，保证卡片滚动时层级在工具栏之下
    DrawHeader(rt);
    DrawPagination(rt);
    DrawRubberBand(rt);
    DrawDetailPanel(rt);
    DrawDragCard(rt);   // 拖拽中的浮动漫画卡片（最上层）
    DrawImportProgress(rt);  // 批量导入进度条（最上层）
    DrawToast(rt);
}

void MainWindow::DrawCover(ID2D1RenderTarget* rt, const ComicModel& c, const D2D1_RECT_F& r, float radius) {
    // 圆角裁剪：用圆角矩形几何裁剪图层
    ID2D1Layer* layer = nullptr;
    ID2D1RoundedRectangleGeometry* geo = nullptr;
    rt->CreateLayer(&layer);
    D2D::Factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &geo);
    if (layer && geo) {
        rt->PushLayer(D2D1::LayerParameters(r, geo), layer);
        ID2D1Bitmap* bmp = D2D::Bitmap(rt, W(c.coverPath));
        if (bmp) {
            // UniformToFill 填满裁切
            D2D1_SIZE_F bs = bmp->GetSize();
            float scale = std::max(r.right - r.left, r.bottom - r.top) / std::max(bs.width, bs.height);
            if (bs.width == 0 || bs.height == 0) scale = 1;
            float bw = bs.width * scale, bh = bs.height * scale;
            float ox = r.left + ((r.right - r.left) - bw) / 2;
            float oy = r.top + ((r.bottom - r.top) - bh) / 2;
            rt->DrawBitmap(bmp, D2D1::RectF(ox, oy, ox + bw, oy + bh));
        } else {
            // 无封面占位：半透明遮罩 + 图标
            D2D::RoundedRect(rt, r, radius, D2D1::ColorF(0, 0, 0, 0.133f));
            D2D1_RECT_F ic{ r.left, r.top + (r.bottom - r.top) * 0.3f, r.right, r.top + (r.bottom - r.top) * 0.6f };
            D2D::Icon(rt, L"\xE8B9", ic, D2D1::ColorF(0x4A, 0x55, 0x68, 1.0f), 28);
            D2D1_RECT_F tc{ r.left, r.top + (r.bottom - r.top) * 0.6f, r.right, r.bottom };
            D2D::Text(rt, i18n::Tr(L"无封面", L"No Cover"), tc, D2D1::ColorF(0x4A, 0x55, 0x68, 1.0f), 11,
                      DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        rt->PopLayer();
        geo->Release();
        layer->Release();
    }
}

// 拖拽动画：鼠标上方浮动一张半透明漫画卡片，跟随鼠标移动
void MainWindow::DrawDragCard(ID2D1RenderTarget* rt) {
    if (!cardDragging_ || !dragCardCached_) return;
    float w = 120, h = 168;
    D2D1_RECT_F card{ (float)(dragPos_.x - w / 2), (float)(dragPos_.y - h / 2),
                      (float)(dragPos_.x + w / 2), (float)(dragPos_.y + h / 2) };
    // 提高可辨识度的外框/投影
    D2D::RoundedRect(rt, D2D1::RectF(card.left - 4, card.top - 4, card.right + 4, card.bottom + 4), 10, theme::BgCard());
    DrawCover(rt, dragCard_, card, 8);
    // 提示文字
    D2D1_RECT_F tr{ card.left, card.bottom + 6, card.right, card.bottom + 26 };
    D2D::Text(rt, i18n::Tr(L"拖到文件夹加入", L"Drag into a folder"), tr, theme::TextPrimary(), 12, DWRITE_FONT_WEIGHT_SEMI_BOLD,
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void MainWindow::DrawHeader(ID2D1RenderTarget* rt) {
    D2D1_RECT_F header{ (float)ContentLeft(), (float)HeaderTop(), (float)ContentRight(), (float)HeaderBottom() };
    D2D::RoundedRect(rt, header, 0, theme::BgSidebar());

    // 视图标题 + 数量
    std::wstring vt;
    if (InTagFilter()) vt = i18n::Tr(L"标签：", L"Tag: ") + W(tagFilter_);
    else if (selectedFolderId_ > 0) {
        for (auto& n : nodes_) if (n.id == selectedFolderId_) { vt = n.name; break; }
        if (vt.empty()) vt = i18n::Tr(L"文件夹", L"Folder");
    } else {
        switch (nav_) {
            case QuickNav::All: vt = i18n::Tr(L"全部漫画", L"All Comics"); break;
            case QuickNav::Uncategorized: vt = i18n::Tr(L"未分类", L"Uncategorized"); break;
            case QuickNav::Untagged: vt = i18n::Tr(L"未标签", L"Untagged"); break;
            case QuickNav::PendingSort: vt = i18n::Tr(L"待分类", L"Unsorted"); break;
            case QuickNav::TagManager: vt = i18n::Tr(L"标签管理", L"Tag Manager"); break;
            case QuickNav::Reading: vt = i18n::Tr(L"阅读中", L"Reading"); break;
            case QuickNav::Completed: vt = i18n::Tr(L"已读完", L"Completed"); break;
            case QuickNav::Trash: vt = i18n::Tr(L"回收站", L"Trash"); break;
            default: vt = i18n::Tr(L"全部漫画", L"All Comics");
        }
    }
    float tx = (float)ContentLeft() + 16;
    if (InTagFilter()) {
        bool hv = hoverHeaderBtn_ == 4;
        if (hv) D2D::RoundedRect(rt, backRect_, 6, theme::BgCardHover());
        D2D::Icon(rt, L"\xE72B", backRect_, theme::TextPrimary(), 12);
        tx = backRect_.right + 8;
    }
    D2D1_RECT_F title{ tx + 4, (float)HeaderTop(), tx + 320, (float)HeaderBottom() };
    D2D::Text(rt, vt, title, theme::TextPrimary(), 15, DWRITE_FONT_WEIGHT_SEMI_BOLD,
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    wchar_t cnt[48];
    swprintf(cnt, 48, i18n::Current() == i18n::Lang::En ? L"· %zu" : L"· %zu 本", filtered_.size());
    D2D1_RECT_F cntr{ tx + 4 + D2D::TextWidth(vt, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 8, (float)HeaderTop(),
                      tx + 420, (float)HeaderBottom() };
    D2D::Text(rt, cnt, cntr, theme::TextSecondary(), 12, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // 右侧按钮组（标签管理未筛选时隐藏卡片/列表/滑条/语言，设置按钮始终显示）
    bool tagBrowse = IsTagManager() && !InTagFilter();
    if (!tagBrowse) {
        // 卡片视图
        bool cardSel = cardView_;
        D2D::RoundedRect(rt, cardViewRect_, 6, cardSel ? theme::BgMain() : (hoverHeaderBtn_ == 0 ? theme::BgCardHover() : theme::BgSidebar()));
        D2D::Icon(rt, L"\xE80A", cardViewRect_, cardSel ? theme::AccentCyan() : theme::TextSecondary(), 13);
        // 列表视图
        D2D::RoundedRect(rt, listViewRect_, 6, !cardSel ? theme::BgMain() : (hoverHeaderBtn_ == 1 ? theme::BgCardHover() : theme::BgSidebar()));
        D2D::Icon(rt, L"\xE8FD", listViewRect_, !cardSel ? theme::AccentCyan() : theme::TextSecondary(), 13);
        // 滑条（细轨道 + 居中短滑块）
        D2D1_RECT_F track{ sliderRect_.left, sliderRect_.top + 10, sliderRect_.right, sliderRect_.bottom - 10 };
        D2D::RoundedRect(rt, track, 3, D2D1::ColorF(1, 1, 1, 0.08f));
        float th = (sliderRect_.bottom - sliderRect_.top - 16) / 2;
        D2D1_RECT_F thumb{ sliderThumbRect_.left, sliderRect_.top + th, sliderThumbRect_.right, sliderRect_.bottom - th };
        D2D::RoundedRect(rt, thumb, 4, theme::AccentCyan());
        // 语言按钮
        D2D::RoundedRect(rt, langRect_, 6, hoverHeaderBtn_ == 2 ? theme::BgCardHover() : theme::BgSidebar());
        D2D::Text(rt, i18n::Tr(L"中/EN", L"EN/中"), langRect_, theme::TextPrimary(), 12, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // 设置按钮（悬停高亮）
    D2D::RoundedRect(rt, settingsRect_, 6, hoverHeaderBtn_ == 3 ? theme::BgCardHover() : theme::BgCard());
    D2D::Icon(rt, L"\xE713", settingsRect_, hoverHeaderBtn_ == 3 ? theme::AccentCyan() : theme::TextSecondary(), 14);
}

void MainWindow::DrawCardView(ID2D1RenderTarget* rt) {
    // 裁剪到内容可视区，避免卡片向上滚动时盖住顶部工具栏/标题栏
    rt->PushAxisAlignedClip(D2D1::RectF((float)ContentLeft(), (float)ContentTop(),
                                        (float)ContentRight(), (float)ContentBottom()),
                            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < pageComics_.size(); i++) {
        auto& r = cardRects_[i];
        if (r.bottom < ContentTop() || r.top > ContentBottom()) continue;
        auto& c = pageComics_[i];
        bool sel = IsSelected(c.id);
        bool hv = ((int)i == hoverCard_);
        D2D1_RECT_F card{ r.left - 0.5f, r.top - 0.5f, r.right + 0.5f, r.bottom + 0.5f };
        D2D::RoundedRect(rt, card, 8, theme::BgCard(),
                         sel ? theme::AccentCyan() : (hv ? theme::AccentCyan() : theme::BorderColor()),
                         sel ? 2.0f : (hv ? 1.5f : 1.0f));
        // 封面
        D2D1_RECT_F cover = r;
        DrawCover(rt, c, cover, 8);
        // 底部渐变 + 标题 + 进度条
        float gradH = 56;
        D2D1_RECT_F grad{ r.left, r.bottom - gradH, r.right, r.bottom };
        // 底部黑色遮罩：仅底部两角圆角与卡片圆角一致（PathGeometry 自定义底部双圆角）
        {
            const float rad = 8.0f;
            ID2D1PathGeometry* geo = nullptr;
            if (SUCCEEDED(D2D::Factory()->CreatePathGeometry(&geo))) {
                ID2D1GeometrySink* sink = nullptr;
                if (SUCCEEDED(geo->Open(&sink))) {
                    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
                    sink->BeginFigure(D2D1::Point2F(grad.left, grad.top), D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(grad.right, grad.top));                        // 上边（直角）
                    sink->AddLine(D2D1::Point2F(grad.right, grad.bottom - rad));               // 右边
                    // 右下圆角：正上→正左 用 CLOCKWISE
                    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(grad.right - rad, grad.bottom),
                                                  D2D1::SizeF(rad, rad), 0.0f,
                                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->AddLine(D2D1::Point2F(grad.left + rad, grad.bottom));                // 底边
                    // 左下圆角：与右下角同方向（CLOCKWISE），外凸左下
                    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(grad.left, grad.bottom - rad),
                                                  D2D1::SizeF(rad, rad), 0.0f,
                                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->AddLine(D2D1::Point2F(grad.left, grad.top));                         // 左边
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    sink->Release();
                    ID2D1SolidColorBrush* brush = nullptr;
                    if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.88f), &brush))) {
                        rt->FillGeometry(geo, brush);
                        brush->Release();
                    }
                }
                geo->Release();
            }
        }
        D2D1_RECT_F ttr{ r.left + 8, r.bottom - 34, r.right - 8, r.bottom - 16 };
        D2D::Text(rt, W(c.title), ttr, D2D1::ColorF(1, 1, 1), 12, DWRITE_FONT_WEIGHT_MEDIUM,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // 进度条（加粗到 6px 提升可见性；浮点百分比避免整数截断为 0 不绘制）
        float pctF = c.pageCount > 0 ? (c.currentPage + 1) * 100.0f / c.pageCount : 0.0f;
        D2D1_RECT_F track{ r.left + 8, r.bottom - 12, r.right - 8, r.bottom - 6 };
        D2D::RoundedRect(rt, track, 2.0f, D2D1::ColorF(1, 1, 1, 0.20f));
        if (pctF > 0) {
            float wpx = (track.right - track.left) * pctF / 100.0f;
            if (wpx < 6) wpx = 6;   // 最小宽度保证绿色可见
            D2D1_RECT_F fill{ track.left, track.top, track.left + wpx, track.bottom };
            D2D::RoundedRect(rt, fill, 2.0f, theme::AccentCyan());
        }
        // 状态徽章
        if (c.status == ComicStatus::Reading || c.status == ComicStatus::Completed) {
            std::wstring badge = c.status == ComicStatus::Reading ? i18n::Tr(L"读中", L"Reading") : i18n::Tr(L"读完", L"Completed");
            float bw = D2D::TextWidth(badge, 9) + 10;
            D2D1_RECT_F bd{ r.right - 6 - bw, r.top + 6, r.right - 6, r.top + 6 + 18 };
            D2D::RoundedRect(rt, bd, 3, D2D1::ColorF(0, 0, 0, 0.80f));
            D2D::Text(rt, badge, bd, D2D1::ColorF(1, 1, 1), 9, DWRITE_FONT_WEIGHT_NORMAL,
                      DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    rt->PopAxisAlignedClip();
}

void MainWindow::DrawListView(ID2D1RenderTarget* rt) {
    // 裁剪到内容可视区，避免行向上滚动时盖住顶部工具栏/标题栏
    rt->PushAxisAlignedClip(D2D1::RectF((float)ContentLeft(), (float)ContentTop(),
                                        (float)ContentRight(), (float)ContentBottom()),
                            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // 列头
    D2D1_RECT_F head{ (float)ContentLeft(), (float)ContentTop(), (float)ContentRight(), (float)(ContentTop() + 36) };
    D2D::RoundedRect(rt, head, 0, theme::BgCard());
    float cl = (float)ContentLeft() + 20;
    float cw = (float)(ContentRight() - ContentLeft());
    D2D1_RECT_F c1{ cl + 40, head.top, cl + 40 + (cw - 40 - 120 - 80 - 60), head.bottom };
    D2D::Text(rt, i18n::Tr(L"标题", L"Title"), c1, theme::TextSecondary(), 11);
    D2D1_RECT_F c2{ c1.right, head.top, c1.right + 120, head.bottom };
    D2D::Text(rt, i18n::Tr(L"文件夹", L"Folder"), c2, theme::TextSecondary(), 11);
    D2D1_RECT_F c3{ c2.right, head.top, c2.right + 80, head.bottom };
    D2D::Text(rt, i18n::Tr(L"进度", L"Progress"), c3, theme::TextSecondary(), 11);
    D2D1_RECT_F c4{ c3.right, head.top, c3.right + 60, head.bottom };
    D2D::Text(rt, L"%", c4, theme::TextSecondary(), 11, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // 行
        for (size_t i = 0; i < pageComics_.size(); i++) {
        auto& r = listRowRects_[i];
        if (r.bottom < ContentTop() || r.top > ContentBottom()) continue;
        auto& c = pageComics_[i];
        bool sel = IsSelected(c.id);
        bool hv = ((int)i == hoverListRow_);
        if (sel) D2D::RoundedRect(rt, r, 0, theme::BgMain());
        else if (hv) D2D::RoundedRect(rt, r, 0, theme::BgCardHover());
        // 封面 32x44
        float ry = r.top + (r.bottom - r.top - 44) / 2;
        DrawCover(rt, c, D2D1::RectF(r.left + 20, ry, r.left + 52, ry + 44), 4);
        // 标题 / 文件夹 / 页码 / 百分比
        D2D1_RECT_F t1{ r.left + 60, r.top, c2.left, r.bottom };
        D2D::Text(rt, W(c.title), t1, theme::TextPrimary(), 13, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F t2{ c2.left, r.top, c2.right, r.bottom };
        D2D::Text(rt, W(c.folderNames), t2, theme::TextSecondary(), 12, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        wchar_t pg[48];
        swprintf(pg, 48, L"%d/%d", c.currentPage, c.pageCount);
        D2D1_RECT_F t3{ c3.left, r.top, c3.right, r.bottom };
        D2D::Text(rt, pg, t3, theme::TextSecondary(), 12);
        wchar_t pp[32];
        swprintf(pp, 32, L"%d%%", c.progressPercent());
        D2D1_RECT_F t4{ c4.left, r.top, c4.right, r.bottom };
        D2D::Text(rt, pp, t4, theme::AccentCyan(), 12, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    rt->PopAxisAlignedClip();
}

void MainWindow::DrawTagManager(ID2D1RenderTarget* rt) {
    D2D1_RECT_F ti{ (float)ContentLeft() + 16, (float)ContentTop() + 8, (float)ContentRight() - 16, (float)ContentTop() + 32 };
    D2D::Text(rt, i18n::Tr(L"标签管理", L"Tag Manager"), ti, theme::TextPrimary(), 15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    D2D1_RECT_F hint{ (float)ContentLeft() + 16, (float)ContentTop() + 32, (float)ContentRight() - 16, (float)ContentTop() + 48 };
    D2D::Text(rt, i18n::Tr(L"点击标签查看该标签下的漫画，右键管理", L"Click a tag to view its comics; right-click to manage"), hint, theme::TextSecondary(), 12);
    for (size_t i = 0; i < tags_.size(); i++) {
        auto& r = tagChipRects_[i];
        if (r.top < ContentTop() || r.top > ContentBottom()) continue;
        // 悬停高亮
        if ((int)i == tagHover_)
            D2D::RoundedRect(rt, r, 10, theme::BgCardHover(), theme::AccentCyan(), 1.0f);
        else
            D2D::RoundedRect(rt, r, 10, theme::MutedCyan(), theme::MutedCyan(), 1.0f);
        std::wstring name = W(tags_[i].name);
        wchar_t cnt[32]; swprintf(cnt, 32, L"%d", tags_[i].count);
        float countW = D2D::TextWidth(cnt, 10);
        // 名称（左对齐），右侧给数量留空间
        D2D1_RECT_F nameR{ r.left + 10, r.top, r.right - 10 - countW - 8, r.bottom };
        D2D::Text(rt, name, nameR, theme::AccentCyan(), 11);
        // 数量（右对齐）
        D2D1_RECT_F ctr{ r.right - 10 - countW, r.top, r.right - 10, r.bottom };
        D2D::Text(rt, cnt, ctr, theme::TextSecondary(), 10);
    }
}

void MainWindow::DrawPagination(ID2D1RenderTarget* rt) {
    if (!PaginationVisible()) return;
    D2D1_RECT_F bar{ (float)ContentLeft(), (float)(Height() - 46), (float)ContentRight(), (float)(Height() - 4) };
    D2D::RoundedRect(rt, bar, 0, theme::BgSidebar(), theme::BorderColor(), 1.0f);
    // 居中
    float cx = (ContentLeft() + ContentRight()) / 2.0f;
    float bTop = bar.top + (46 - 28) / 2, bBot = bar.top + (46 - 28) / 2 + 28;
    // 上一页
    D2D1_RECT_F prev{ cx - 96, bTop, cx - 68, bBot };
    bool prevEn = page_ > 1;
    D2D1_COLOR_F pbg = (hoverPageBtn_ == 0 && prevEn) ? theme::AccentCyan() : theme::BgCard();
    D2D::RoundedRect(rt, prev, 4, pbg, theme::BorderColor(), 1.0f);
    D2D::Icon(rt, L"\xE76B", prev, prevEn ? (hoverPageBtn_ == 0 ? theme::AccentText() : theme::TextPrimary()) : D2D1::ColorF(0.5f, 0.5f, 0.5f), 12);
    // 页码
    wchar_t pg[32]; swprintf(pg, 32, L"%d / %d", page_, totalPages_);
    D2D1_RECT_F pgt{ cx - 68, bTop, cx + 8, bBot };
    D2D::Text(rt, pg, pgt, theme::TextSecondary(), 12, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // 跳页输入框
    pageBox_ = D2D1::RectF(cx + 12, bTop + 1, cx + 56, bBot - 1);
    D2D::RoundedRect(rt, pageBox_, 4, theme::BgCard(), pageJumpFocused_ ? theme::AccentCyan() : theme::BorderColor(), pageJumpFocused_ ? 1.5f : 1.0f);
    D2D::Text(rt, pageJumpText_, pageBox_, theme::TextPrimary(), 12, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // 确认
    D2D1_RECT_F ok{ cx + 60, bTop, cx + 104, bBot };
    D2D::RoundedRect(rt, ok, 4, (hoverPageBtn_ == 1) ? theme::AccentCyan() : theme::BgCard(), theme::BorderColor(), 1.0f);
    D2D::Text(rt, i18n::Tr(L"确认", L"OK"), ok, (hoverPageBtn_ == 1) ? theme::AccentText() : theme::TextPrimary(), 12,
              DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // 下一页
    D2D1_RECT_F next{ cx + 104, bTop, cx + 132, bBot };
    bool nextEn = page_ < totalPages_;
    D2D1_COLOR_F nbg = (hoverPageBtn_ == 2 && nextEn) ? theme::AccentCyan() : theme::BgCard();
    D2D::RoundedRect(rt, next, 4, nbg, theme::BorderColor(), 1.0f);
    D2D::Icon(rt, L"\xE76C", next, nextEn ? (hoverPageBtn_ == 2 ? theme::AccentText() : theme::TextPrimary()) : D2D1::ColorF(0.5f, 0.5f, 0.5f), 12);
}

void MainWindow::DrawRubberBand(ID2D1RenderTarget* rt) {
    if (!rubber_) return;
    float l = (float)std::min(ruStart_.x, ruEnd_.x);
    float t = (float)std::min(ruStart_.y, ruEnd_.y);
    float r = (float)std::max(ruStart_.x, ruEnd_.x);
    float b = (float)std::max(ruStart_.y, ruEnd_.y);
    D2D1_RECT_F rr{ l, t, r, b };
    D2D1_COLOR_F fill = theme::AccentCyan();
    fill.a = 0.15f;
    D2D::RoundedRect(rt, rr, 2, fill, theme::AccentCyan(), 1.0f);
}

void MainWindow::DrawImportProgress(ID2D1RenderTarget* rt) {
    if (!importing_) return;
    float w = 320, h = 100;
    float cx = (ContentLeft() + ContentRight()) / 2.0f;
    float cy = (ContentTop() + ContentBottom()) / 2.0f;
    D2D1_RECT_F panel{ cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2 };
    D2D::RoundedRect(rt, panel, 10, D2D1::ColorF(0.05f, 0.05f, 0.05f, 0.92f));
    D2D::Text(rt, i18n::Tr(L"正在导入漫画...", L"Importing comics..."), D2D1::RectF(panel.left + 16, panel.top + 12, panel.right - 16, panel.top + 38),
              theme::TextPrimary(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD,
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // 进度条
    D2D1_RECT_F bar{ panel.left + 16, panel.top + 50, panel.right - 16, panel.top + 58 };
    D2D::RoundedRect(rt, bar, 3, D2D1::ColorF(1, 1, 1, 0.18f));
    float pct = (float)importProgress_ / 100.0f;
    if (pct > 0) {
        D2D1_RECT_F fill{ bar.left, bar.top, bar.left + (bar.right - bar.left) * pct, bar.bottom };
        D2D::RoundedRect(rt, fill, 3, theme::AccentCyan());
    }
    std::wstring imp = (i18n::Current() == i18n::Lang::En)
        ? L"Completed " + std::to_wstring(importProgress_) + L"% · Imported " + std::to_wstring(importOk_)
        : L"已完成 " + std::to_wstring(importProgress_) + L"% · 已导入 " + std::to_wstring(importOk_) + L" 本";
    D2D::Text(rt, imp,
              D2D1::RectF(panel.left + 16, panel.top + 66, panel.right - 16, panel.top + 86),
              theme::TextSecondary(), 12, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void MainWindow::DrawToast(ID2D1RenderTarget* rt) {
    if (!toastVisible_ || toast_.empty()) return;
    float tw = D2D::TextWidth(toast_, 13) + 32;
    float tx = (ContentLeft() + ContentRight()) / 2.0f - tw / 2;
    float ty = (float)Height() - 40 - 34;
    D2D1_RECT_F bg{ tx, ty, tx + tw, ty + 34 };
    D2D::RoundedRect(rt, bg, 8, theme::BgCard(), theme::AccentCyan(), 1.0f);
    D2D::Text(rt, toast_, bg, theme::TextPrimary(), 13, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

// ==================== 交互 ====================
void MainWindow::OnLButtonDown(int x, int y) {
    SetCapture(Hwnd());
    // 侧栏拖拽
    if (y > TitleBarHeight && x >= sidebarWidth_ - 2 && x <= sidebarWidth_ + 2) {
        StartSidebarResize(x);
        return;
    }
    // 汉堡
    if (x >= hamburgerRect_.left && x <= hamburgerRect_.right && y >= hamburgerRect_.top && y <= hamburgerRect_.bottom) {
        OpenLibrarySwitcher();
        return;
    }
    // 搜索框聚焦
    auto srect = SearchBoxRect();
    searchFocused_ = (x >= srect.left && x <= srect.right && y >= srect.top && y <= srect.bottom);
    if (searchFocused_) SetFocus(Hwnd());
    // 快捷导航
    for (int i = 0; i < 3; i++) {
        auto r = NavRect(i);
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
            navHover_ = i;
            QuickNav nav = (i == 0) ? QuickNav::All : (i == 1) ? QuickNav::TagManager : QuickNav::Trash;
            selectedFolderId_ = -1;
            selectedNode_ = -1;
            tagFilter_.clear();
            page_ = 1;
            ClearSelection();
            nav_ = nav;
            RefreshFiltered();
            return;
        }
    }
    // 虚拟文件夹标题行右侧 "+" 按钮：新建根文件夹（与右键菜单一致）
    auto thR = TreeHeaderRect();
    if (x >= thR.right - 24 && x <= thR.right && y >= thR.top + 4 && y <= thR.top + 28) {
        DoAddRootFolder();
        return;
    }
    // 树节点
    int fi = HitTreeNode(x, y);
    if (fi >= 0) {
        auto& n = nodes_[fi];
        int indent = 8 + n.depth * 16;
        if (n.hasChildren && x >= n.rect.left + indent && x <= n.rect.left + indent + 16) {
            ToggleNode(fi);
            return;
        }
        selectedNode_ = fi;
        selectedFolderId_ = n.id;
        nav_ = QuickNav::All;
        tagFilter_.clear();
        page_ = 1;
        ClearSelection();
        RefreshFiltered();
        return;
    }
    // 树空白点击清空选择
    if (x < sidebarWidth_ && y > TreeTop()) {
        selectedNode_ = -1;
        selectedFolderId_ = -1;
        RefreshFiltered();
        return;
    }

    // ---- 右栏详情面板 ----
    if (x >= DetailLeft()) {
        // 拖拽左边缘调整宽度
        if (x >= DetailLeft() - 2 && x <= DetailLeft() + 2) {
            detailResizing_ = true;
            detailResizeStartX_ = x;
            detailResizeStartW_ = detailWidth_;
            return;
        }
        if (!detailValid_) return;
        // 标题/作者文本框
        if (dTitle_.Hit(x, y)) {
            dTitle_.focused = true; dAuthor_.focused = false; dNotesFocused_ = false;
            dEditField_ = "title"; dTitle_.PlaceCaret(x); SetFocus(Hwnd()); Invalidate(); return;
        }
        if (dAuthor_.Hit(x, y)) {
            dAuthor_.focused = true; dTitle_.focused = false; dNotesFocused_ = false;
            dEditField_ = "author"; dAuthor_.PlaceCaret(x); SetFocus(Hwnd()); Invalidate(); return;
        }
        // 备注
        if (x >= dNotesRect_.left && x <= dNotesRect_.right && y >= dNotesRect_.top && y <= dNotesRect_.bottom) {
            dNotesFocused_ = true; dTitle_.focused = dAuthor_.focused = false;
            dEditField_ = "notes"; dNotesCaret_ = NotesHitCaret(x, y); SetFocus(Hwnd()); Invalidate(); return;
        }
        // 添加标签
        if (x >= dAddTagRect_.left && x <= dAddTagRect_.right && y >= dAddTagRect_.top && y <= dAddTagRect_.bottom) {
            DoAddDetailTag(); return;
        }
        // 打开阅读
        if (x >= dOpenRect_.left && x <= dOpenRect_.right && y >= dOpenRect_.top && y <= dOpenRect_.bottom) {
            if (detailValid_) {
                auto* reader = new ReaderWindow();
                HWND mainHwnd = Hwnd();
                reader->SetOnClosed([mainHwnd] { PostMessageW(mainHwnd, WM_APP + 2, 0, 0); });
                reader->OpenComic(service_, detail_);
                reader->Create(nullptr, true);
            }
            return;
        }
        // 点击其他处：失焦提交
        if (!dEditField_.empty()) SaveDetailField();
        return;
    }

    // ---- 中栏 ----
    if (x < ContentLeft()) return;
    // 头部（设置/列表切换/语言/滑条等）优先响应，不受资源库是否存在影响
    if (y >= HeaderTop() && y <= HeaderBottom()) { OnHeaderClick(x, y); return; }
    // 无资源库：点击中栏内容区无动作（仅显示引导文字，创建入口在左栏「资源库」菜单）
    if (!service_) return;
    // 分页栏
    if (PaginationVisible() && y > Height() - 46) {
        if (x >= pageBox_.left && x <= pageBox_.right) { pageJumpFocused_ = true; SetFocus(Hwnd()); return; }
        pageJumpFocused_ = false;
        // 上一页/下一页/确认
        float cx = (ContentLeft() + ContentRight()) / 2.0f;
        float bTop = (float)(Height() - 46) + (46 - 28) / 2;
        if (x >= cx - 96 && x <= cx - 68) { PrevPage(); return; }
        if (x >= cx + 60 && x <= cx + 104) { DoJumpPage(); return; }
        if (x >= cx + 104 && x <= cx + 132) { NextPage(); return; }
        return;
    }
    // 标签管理视图
    if (IsTagManager() && !InTagFilter()) {
        int tagIdx = HitTagChip(x, y);
        if (tagIdx >= 0) EnterTagFilter(tags_[tagIdx].name);
        return;
    }
    // 卡片 / 列表选择
    int idx = cardView_ ? HitCard(x, y) : HitListRow(x, y);
    if (idx >= 0) {
        anchorIndex_ = idx;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        int id = pageComics_[idx].id;
        if (ctrl) ToggleSelected(id);
        else if (shift && anchorIndex_ >= 0) {
            // 从锚点到点击项区间全选（保留原选择）
            int a = std::min(anchorIndex_, idx), b = std::max(anchorIndex_, idx);
            for (int k = a; k <= b; k++) {
                int kk = pageComics_[k].id;
                if (std::find(selectedIds_.begin(), selectedIds_.end(), kk) == selectedIds_.end())
                    selectedIds_.push_back(kk);
            }
        } else SelectOnly(id);
        // 记录拖拽起点，移动超过阈值后进入拖拽模式
        dragArmed_ = true;
        dragStart_ = { x, y };
        Invalidate();
    } else {
        // 空白：清空选择 + 开始橡皮筋框选
        ClearSelection();
        rubber_ = true;
        ruStart_ = { x, y }; ruEnd_ = { x, y };
        Invalidate();
    }
}

void MainWindow::OnLButtonUp(int x, int y) {
    // 拖拽释放：命中左栏文件夹则把选中漫画加入该文件夹
    if (cardDragging_) {
        cardDragging_ = false; dragArmed_ = false;
        if (dragHoverNode_ >= 0) DoDropToFolder(dragHoverNode_);
        dragHoverNode_ = -1;
        ReleaseCapture();
        Invalidate();
        return;
    }
    dragArmed_ = false;
    if (resizing_) { EndSidebarResize(); ReleaseCapture(); }
    if (detailResizing_) { EndDetailResize(); ReleaseCapture(); }
    if (rubber_) { rubber_ = false; Invalidate(); }
    if (sliderDragging_) { sliderDragging_ = false; ReleaseCapture(); }
    ReleaseCapture();
}

void MainWindow::OnDoubleClick(int x, int y) {
    // 仅允许在内容可视区双击，排除上方工具栏区域（避免穿透到其下方滚动上移的卡片）
    if (x < ContentLeft() || y < ContentTop() || y > ContentBottom()) return;
    int idx = cardView_ ? HitCard(x, y) : HitListRow(x, y);
    if (idx >= 0) DoOpenReader(idx);
}

void MainWindow::OnRButtonUp(int x, int y) {
    // 左栏文件夹
    if (x <= sidebarWidth_) {
        int fi = HitTreeNode(x, y);
        int target;
        if (fi >= 0) target = nodes_[fi].id;
        else if (y > TreeTop()) target = 0;
        else return;
        ShowFolderContextMenu(x, y, target);
        return;
    }
    // 右栏详情：右键标签 chip → 重命名/删除
    if (x >= DetailLeft()) {
        if (detailValid_) {
            for (size_t i = 0; i < dTagRects_.size(); i++) {
                if (x >= dTagRects_[i].left && x <= dTagRects_[i].right && y >= dTagRects_[i].top && y <= dTagRects_[i].bottom) {
                    ShowDetailTagMenu(x, y, detail_.tags[i]); return;
                }
            }
        }
        return;
    }
    // 中栏
    // 标签管理视图：右键 chip → 重命名/删除（标签位于内容区，不受下方工具栏拦截影响）
    if (IsTagManager() && !InTagFilter()) {
        if (x >= ContentLeft()) {
            int ti = HitTagChip(x, y);
            if (ti >= 0) ShowTagContextMenu(x, y, ti);
        }
        return;
    }
    // 排除工具栏区域，避免穿透到其下方滚动上移的卡片
    if (x < ContentLeft() || y < HeaderTop() || y > HeaderBottom()) return;
    int idx = cardView_ ? HitCard(x, y) : HitListRow(x, y);
    if (idx >= 0) {
        // 若未选中该本则单选之
        int id = pageComics_[idx].id;
        if (!IsSelected(id)) SelectOnly(id);
        ShowComicContextMenu(x, y, idx);
    } else if (nav_ == QuickNav::Trash) {
        // 回收站空白处右键：清空回收站
        POINT pt{ x, y };
        if (Hwnd()) ClientToScreen(Hwnd(), &pt);
        std::vector<PopupMenu::Item> items = { { L"清空回收站", 10 } };
        PopupMenu menu(pt.x, pt.y, std::move(items));
        if (menu.Run() == 10) DoEmptyTrash();
    }
}

void MainWindow::OnMouseMove(int x, int y) {
    if (resizing_) { UpdateSidebarResize(x); return; }
    if (detailResizing_) { UpdateDetailResize(x); return; }
    if (sliderDragging_) {
        // 更新卡片尺寸
        float sw = sliderRect_.right - sliderRect_.left;
        float t = (x - sliderRect_.left) / sw;
        if (t < 0) t = 0; if (t > 1) t = 1;
        int v = 120 + (int)(t * 160 + 0.5f);
        v = ((v + 5) / 10) * 10;
        if (v < 120) v = 120; if (v > 280) v = 280;
        if (v != cardSize_) { cardSize_ = v; ComputeHeaderRects(); LayoutCards(); Invalidate(); }
        return;
    }
    // 卡片拖拽到左栏文件夹：越过阈值后进入拖拽，跟踪目标文件夹高亮
    if (dragArmed_ && !cardDragging_) {
        if (abs(x - dragStart_.x) > 4 || abs(y - dragStart_.y) > 4) {
            cardDragging_ = true;
            // 缓存第一张选中漫画作为浮动卡片
            dragCardCached_ = !selectedIds_.empty() && service_ && service_->GetComicById(selectedIds_[0], dragCard_);
        }
    }
    if (cardDragging_) {
        dragPos_ = { x, y };
        int dn = (x <= sidebarWidth_) ? HitTreeNode(x, y) : -1;
        if (dn != dragHoverNode_) { dragHoverNode_ = dn; }
        Invalidate();
        return;
    }
    // 左栏悬停
    bool hb = x >= hamburgerRect_.left && x <= hamburgerRect_.right && y >= hamburgerRect_.top && y <= hamburgerRect_.bottom;
    if (hb != hamburgerHover_) { hamburgerHover_ = hb; Invalidate(); }
    int nhov = -1;
    for (int i = 0; i < 3; i++) {
        auto r = NavRect(i);
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) { nhov = i; break; }
    }
    if (nhov != navHover_) { navHover_ = nhov; Invalidate(); }
    int hn = HitTreeNode(x, y);
    if (hn != hoverNode_) { hoverNode_ = hn; Invalidate(); }

    // 中栏悬停
    int hh = -1;
    if (y >= HeaderTop() && y <= HeaderBottom()) {
        if (InTagFilter() && x >= backRect_.left && x <= backRect_.right) hh = 4;
        else if (x >= cardViewRect_.left && x <= cardViewRect_.right) hh = 0;
        else if (x >= listViewRect_.left && x <= listViewRect_.right) hh = 1;
        else if (x >= langRect_.left && x <= langRect_.right) hh = 2;
        else if (x >= settingsRect_.left && x <= settingsRect_.right) hh = 3;
    }
    if (hh != hoverHeaderBtn_) { hoverHeaderBtn_ = hh; Invalidate(); }
    int hc = -1, hl = -1;
    // 工具栏区域不参与卡片/列表悬停，避免穿透到其下方滚动上移的卡片
    if (!(IsTagManager() && !InTagFilter()) && y >= ContentTop()) {
        if (cardView_) hc = HitCard(x, y); else hl = HitListRow(x, y);
    }
    if (hc != hoverCard_) { hoverCard_ = hc; Invalidate(); }
    if (hl != hoverListRow_) { hoverListRow_ = hl; Invalidate(); }
    // 标签管理 chip 悬停
    int th = (IsTagManager() && !InTagFilter()) ? HitTagChip(x, y) : -1;
    if (th != tagHover_) { tagHover_ = th; Invalidate(); }
    RefreshPageJumpHover(x, y);

    // 橡皮筋框选
    if (rubber_) {
        ruEnd_ = { x, y };
        // 重算选中
        std::vector<int> newSel;
        float l = (float)std::min((int)ruStart_.x, x), t = (float)std::min((int)ruStart_.y, y);
        float r = (float)std::max((int)ruStart_.x, x), b = (float)std::max((int)ruStart_.y, y);
        for (size_t i = 0; i < cardRects_.size(); i++) {
            auto& cr = cardRects_[i];
            if (cr.left < r && cr.right > l && cr.top < b && cr.bottom > t) {
                int id = pageComics_[i].id;
                if (std::find(newSel.begin(), newSel.end(), id) == newSel.end()) newSel.push_back(id);
            }
        }
        selectedIds_ = newSel;
        Invalidate();
    }

    // 右栏详情悬停
    if (x >= DetailLeft()) {
        bool coverHv = detailValid_ && x >= dCoverRect_.left && x <= dCoverRect_.right &&
                       y >= dCoverRect_.top && y <= dCoverRect_.bottom;
        int dh = -1;
        if (detailValid_) {
            if (coverHv) dh = 0;
            else if (x >= dAddTagRect_.left && x <= dAddTagRect_.right && y >= dAddTagRect_.top && y <= dAddTagRect_.bottom) dh = 1;
            else if (x >= dOpenRect_.left && x <= dOpenRect_.right && y >= dOpenRect_.top && y <= dOpenRect_.bottom) dh = 2;
        }
        int dht = -1;
        if (detailValid_)
            for (size_t i = 0; i < dTagRects_.size(); i++)
                if (x >= dTagRects_[i].left && x <= dTagRects_[i].right && y >= dTagRects_[i].top && y <= dTagRects_[i].bottom) { dht = (int)i; break; }
        if (dh != detailHover_ || dht != detailHoverTag_ || coverHv != dCoverHover_) {
            detailHover_ = dh; detailHoverTag_ = dht; dCoverHover_ = coverHv; Invalidate();
        }
    } else if (detailHover_ != -1 || detailHoverTag_ != -1 || dCoverHover_) {
        detailHover_ = -1; detailHoverTag_ = -1; dCoverHover_ = false; Invalidate();
    }
}

void MainWindow::OnMouseLeave() {
    if (hamburgerHover_ || navHover_ != -1 || hoverNode_ != -1 || hoverCard_ != -1 ||
        hoverListRow_ != -1 || hoverHeaderBtn_ != -1 || hoverPageBtn_ != -1 || tagHover_ != -1 ||
        detailHover_ != -1 || detailHoverTag_ != -1 || dCoverHover_) {
        hamburgerHover_ = false; navHover_ = -1; hoverNode_ = -1; hoverCard_ = -1;
        hoverListRow_ = -1; hoverHeaderBtn_ = -1; hoverPageBtn_ = -1; tagHover_ = -1;
        detailHover_ = -1; detailHoverTag_ = -1; dCoverHover_ = false;
        Invalidate();
    }
}

void MainWindow::OnKeyDown(UINT vk) {
    if (!dEditField_.empty()) {
        if (vk == VK_BACK) {
            if (dEditField_ == "title" && dTitle_.caret > 0) { dTitle_.text.erase((size_t)dTitle_.caret - 1, 1); dTitle_.caret--; }
            else if (dEditField_ == "author" && dAuthor_.caret > 0) { dAuthor_.text.erase((size_t)dAuthor_.caret - 1, 1); dAuthor_.caret--; }
            else if (dEditField_ == "notes" && dNotesCaret_ > 0) { dNotes_.erase((size_t)dNotesCaret_ - 1, 1); dNotesCaret_--; }
            Invalidate();
        } else if (vk == VK_RETURN && dEditField_ == "notes") {
            dNotes_.insert((size_t)dNotesCaret_, 1, L'\n'); dNotesCaret_++; Invalidate();
        } else if (vk == VK_ESCAPE) {
            SaveDetailField();
        } else if (vk == VK_LEFT) {
            if (dEditField_ == "notes" && dNotesCaret_ > 0) { dNotesCaret_--; Invalidate(); }
        } else if (vk == VK_RIGHT) {
            if (dEditField_ == "notes" && dNotesCaret_ < (int)dNotes_.size()) { dNotesCaret_++; Invalidate(); }
        }
        return;
    }
    if (searchFocused_) {
        if (vk == VK_BACK && !searchText_.empty()) {
            searchText_.pop_back();
            OnSearchChanged();
            Invalidate();
        } else if (vk == VK_ESCAPE) {
            searchFocused_ = false;
            Invalidate();
        }
        return;
    }
    if (pageJumpFocused_) {
        if (vk == VK_RETURN) { DoJumpPage(); return; }
        if (vk == VK_BACK && !pageJumpText_.empty()) {
            pageJumpText_.pop_back();
            Invalidate();
        } else if (vk == VK_ESCAPE) {
            pageJumpFocused_ = false;
            Invalidate();
        }
        return;
    }
    if (vk == VK_ESCAPE) { ClearSelection(); Invalidate(); }
}

void MainWindow::OnChar(wchar_t ch) {
    if (!dEditField_.empty()) {
        if (ch < 32) return;
        if (dEditField_ == "title") { dTitle_.text.insert((size_t)dTitle_.caret, 1, ch); dTitle_.caret++; }
        else if (dEditField_ == "author") { dAuthor_.text.insert((size_t)dAuthor_.caret, 1, ch); dAuthor_.caret++; }
        else if (dEditField_ == "notes") { dNotes_.insert((size_t)dNotesCaret_, 1, ch); dNotesCaret_++; }
        Invalidate();
        return;
    }
    if (searchFocused_) {
        if (ch < 32) return;
        searchText_ += ch;
        OnSearchChanged();
        Invalidate();
        return;
    }
    if (pageJumpFocused_) {
        if (ch < L'0' || ch > L'9') return;
        if (pageJumpText_.size() < 4) pageJumpText_ += ch;
        Invalidate();
    }
}

void MainWindow::OnResize(int w, int h) {
    if (sidebarWidth_ > w - 300) sidebarWidth_ = w - 300;
    if (sidebarWidth_ < 200) sidebarWidth_ = 200;
    if (detailWidth_ > w - sidebarWidth_ - 8 - 200) detailWidth_ = w - sidebarWidth_ - 8 - 200;
    if (detailWidth_ < 260) detailWidth_ = 260;
    LayoutTree();
    ComputeHeaderRects();
    LayoutCards();
    LayoutListRows();
    LayoutTagChips();
    LayoutDetail();
}

// 搜索防抖：标记脏并重启 300ms 定时器
void MainWindow::OnSearchChanged() {
    searchDirty_ = true;
    KillTimerEx(kSearchTimer);
    StartTimer(kSearchTimer, 300);
}

void MainWindow::OnTimer(UINT id) {
    if (id == kSearchTimer && searchDirty_) {
        searchDirty_ = false;
        page_ = 1;
        RefreshFiltered();
    } else if (id == kToastTimer) {
        KillTimerEx(kToastTimer);
        toastVisible_ = false;
        Invalidate();
    }
}

void MainWindow::OnMouseWheel(int delta) {
    int steps = delta / WHEEL_DELTA;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(Hwnd(), &pt);
    int mx = pt.x, my = pt.y;
    // 判断鼠标所在区域：右栏详情 / 左栏树 / 中栏
    if (mx >= DetailLeft()) {
        // 右栏详情滚动
        detailScroll_ -= steps * 30;
        int visible = (Height() - 52) - (DetailTop() + 16);
        int maxScroll = detailContentH_ - visible;
        if (maxScroll < 0) maxScroll = 0;
        if (detailScroll_ < 0) detailScroll_ = 0;
        if (detailScroll_ > maxScroll) detailScroll_ = maxScroll;
        LayoutDetail();
        Invalidate();
        return;
    }
    if (mx < sidebarWidth_) {
        // 左栏树滚动
        treeScroll_ -= steps * 30;
        int maxScroll = (int)nodes_.size() * 30 - (TreeBottom() - TreeTop());
        if (maxScroll < 0) maxScroll = 0;
        if (treeScroll_ < 0) treeScroll_ = 0;
        if (treeScroll_ > maxScroll) treeScroll_ = maxScroll;
        LayoutTree();
    } else if (!(IsTagManager() && !InTagFilter())) {
        if (cardView_) {
            cardScroll_ -= steps * 40;
            // 最大滚动按“行数×行高”计算（原按卡片数当行数，范围被高估，
            // 导致滚过内容后还有大片空白、底部卡片滚不到位）
            int cw = cardSize_, ch = (int)(cardSize_ * 1.4f) + 12;
            int usable = (ContentRight() - ContentLeft()) - 20;
            int perRow = usable / (cw + 12); if (perRow < 1) perRow = 1;
            int rows = ((int)pageComics_.size() + perRow - 1) / perRow;
            int contentH = 10 + rows * (ch + 12);
            int maxScroll = contentH - (ContentBottom() - ContentTop());
            if (maxScroll < 0) maxScroll = 0;
            if (cardScroll_ < 0) cardScroll_ = 0;
            if (cardScroll_ > maxScroll) cardScroll_ = maxScroll;
            LayoutCards();
        } else {
            listScroll_ -= steps * 40;
            int maxScroll = (int)listRowRects_.size() * 56 - (ContentBottom() - ContentTop());
            if (maxScroll < 0) maxScroll = 0;
            if (listScroll_ < 0) listScroll_ = 0;
            if (listScroll_ > maxScroll) listScroll_ = maxScroll;
            LayoutListRows();
        }
    } else {
        tagScroll_ -= steps * 40;
        if (tagScroll_ < 0) tagScroll_ = 0;
    }
    Invalidate();
}

void MainWindow::OnDropFiles(HDROP drop) {
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> paths;
    for (UINT i = 0; i < n; i++) {
        wchar_t buf[MAX_PATH];
        UINT len = DragQueryFileW(drop, i, buf, MAX_PATH);
        if (len > 0) paths.push_back(buf);
    }
    DragFinish(drop);
    if (paths.empty()) return;
    // 拖拽单张图片到详情封面 → 替换封面
    if (detailValid_ && paths.size() == 1) {
        std::wstring ext = GetExtensionW(paths[0]);
        bool img = ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".gif" ||
                   ext == L".bmp" || ext == L".webp";
        if (img) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(Hwnd(), &pt);
            if (pt.x >= dCoverRect_.left && pt.x <= dCoverRect_.right && pt.y >= dCoverRect_.top && pt.y <= dCoverRect_.bottom) {
                ComicStorageService::ReplaceCover(detail_, U8(paths[0]));
                CommitDetail();
                RefreshFromDetail();
                ShowToast(i18n::Tr(L"封面已更新", L"Cover updated"));
                return;
            }
        }
    }
    // 归属：当前文件夹筛选视图 → 导入到该文件夹；否则根
    int targetFolder = selectedFolderId_ > 0 ? selectedFolderId_ : 0;
    ImportPaths(paths, targetFolder);
}

// ==================== 侧栏宽度 ====================
void MainWindow::StartSidebarResize(int x) {
    resizing_ = true;
    resizeStartX_ = x;
    resizeStartW_ = sidebarWidth_;
}
void MainWindow::UpdateSidebarResize(int x) {
    int nw = resizeStartW_ + (x - resizeStartX_);
    if (nw < 200) nw = 200;
    if (nw > 420) nw = 420;
    if (nw != sidebarWidth_) { sidebarWidth_ = nw; LayoutTree(); ComputeHeaderRects(); LayoutCards(); LayoutListRows(); Invalidate(); }
}
void MainWindow::EndSidebarResize() {
    resizing_ = false;
}

// 右栏宽度缩放：向左拖变宽
void MainWindow::UpdateDetailResize(int x) {
    int nw = detailResizeStartW_ + (detailResizeStartX_ - x);
    if (nw < 260) nw = 260;
    if (nw > 440) nw = 440;
    if (nw != detailWidth_) {
        detailWidth_ = nw;
        LayoutDetail();
        ComputeHeaderRects();
        LayoutCards();
        LayoutListRows();
        Invalidate();
    }
}
void MainWindow::EndDetailResize() {
    detailResizing_ = false;
}

// ==================== 资源库菜单 ====================
void MainWindow::OpenLibrarySwitcher() {
    // 汉堡按钮：下拉显示已加载的资源库，点击切换当前资源库
    POINT pt{ (int)hamburgerRect_.left, (int)hamburgerRect_.bottom };
    if (Hwnd()) ClientToScreen(Hwnd(), &pt);
    auto& cfg = AppConfig::Instance();
    const std::string& cur = cfg.currentLibraryPath;
    std::vector<PopupMenu::Item> items;
    for (size_t i = 0; i < cfg.libraries.size(); i++) {
        std::wstring label = W(cfg.libraries[i].name);
        if (_stricmp(cfg.libraries[i].path.c_str(), cur.c_str()) == 0) label = L"✓ " + label;
        items.push_back({ label, (int)i });
    }
    if (items.empty()) items.push_back({ i18n::Tr(L"暂无资源库（请在设置中创建）", L"No libraries yet (create one in Settings)"), -1 });
    PopupMenu menu(pt.x, pt.y + 2, std::move(items));
    int tag = menu.Run();
    if (tag >= 0 && tag < (int)cfg.libraries.size()) {
        LibraryManager::SwitchTo(cfg.libraries[tag].path);
        LoadLibrary();
    }
}

// ==================== 文件夹操作 ====================
void MainWindow::ShowFolderContextMenu(int x, int y, int folderId) {
    contextFolderId_ = folderId;
    std::vector<PopupMenu::Item> items;
    if (folderId > 0) {
        items = {
            { i18n::Tr(L"新建子文件夹", L"New Subfolder"), 1 }, { i18n::Tr(L"重命名", L"Rename"), 2 }, { i18n::Tr(L"删除", L"Delete"), 3 },
            { L"", 0 }, { i18n::Tr(L"上移", L"Move Up"), 4 }, { i18n::Tr(L"下移", L"Move Down"), 5 }, { L"", 0 }, { i18n::Tr(L"移动到根目录", L"Move to Root"), 6 },
        };
    } else {
        items = { { i18n::Tr(L"新建文件夹", L"New Folder"), 7 } };
    }
    POINT pt{ x, y };
    if (Hwnd()) ClientToScreen(Hwnd(), &pt);
    PopupMenu menu(pt.x, pt.y, std::move(items));
    int tag = menu.Run();
    switch (tag) {
        case 1: DoAddSubfolder(); break;
        case 2: DoRenameFolder(); break;
        case 3: DoDeleteFolder(); break;
        case 4: DoMoveFolderUp(); break;
        case 5: DoMoveFolderDown(); break;
        case 6: DoMoveFolderToRoot(); break;
        case 7: DoAddRootFolder(); break;
    }
}

void MainWindow::DoAddRootFolder() {
    if (!service_) { ShowMsgBox(Hwnd(), i18n::Tr(L"请先创建或打开资源库。", L"Please create or open a library first."), i18n::Tr(L"提示", L"Notice")); return; }
    InputDialog dlg(Hwnd(), i18n::Tr(L"新建根文件夹", L"New Root Folder"), i18n::Tr(L"请输入文件夹名称：", L"Enter folder name:"));
    if (!dlg.Run()) return;
    std::wstring name = dlg.Text;
    if (name.empty()) return;
    service_->AddRootFolder(U8(name));
    ReloadFolders();
}

void MainWindow::DoAddSubfolder() {
    if (contextFolderId_ <= 0) return;
    InputDialog dlg(Hwnd(), i18n::Tr(L"新建子文件夹", L"New Subfolder"), i18n::Tr(L"请输入子文件夹名称：", L"Enter subfolder name:"));
    if (!dlg.Run()) return;
    std::wstring name = dlg.Text;
    if (name.empty()) return;
    service_->AddSubfolder(contextFolderId_, U8(name));
    expanded_.insert(contextFolderId_);
    ReloadFolders();
}

void MainWindow::DoRenameFolder() {
    if (contextFolderId_ <= 0) return;
    std::wstring cur;
    for (auto& n : nodes_) if (n.id == contextFolderId_) { cur = n.name; break; }
    InputDialog dlg(Hwnd(), i18n::Tr(L"重命名文件夹", L"Rename Folder"), i18n::Tr(L"新名称：", L"New name:"), cur);
    if (!dlg.Run()) return;
    std::wstring name = dlg.Text;
    if (name.empty() || name == cur) return;
    service_->RenameFolder(contextFolderId_, U8(name));
    ReloadFolders();
}

void MainWindow::DoDeleteFolder() {
    if (contextFolderId_ <= 0) return;
    std::wstring name;
    for (auto& n : nodes_) if (n.id == contextFolderId_) { name = n.name; break; }
    std::wstring msg = (i18n::Current() == i18n::Lang::En)
        ? L"Delete folder \"" + name + L"\"?\nIts comics become uncategorized; subfolders are deleted too."
        : L"确定删除文件夹「" + name + L"」？\n其中的漫画将变为未分类，子文件夹一并删除。";
    if (!ShowMsgBox(Hwnd(), msg, i18n::Tr(L"确认删除", L"Confirm Delete"), true)) return;
    service_->DeleteFolder(contextFolderId_);
    if (selectedFolderId_ == contextFolderId_) { selectedFolderId_ = -1; selectedNode_ = -1; }
    ReloadFolders();
    RefreshFiltered();
}

void MainWindow::DoMoveFolderUp() {
    if (contextFolderId_ <= 0) return;
    auto sibs = service_->GetSiblingIds(contextFolderId_);
    auto it = std::find(sibs.begin(), sibs.end(), contextFolderId_);
    if (it == sibs.end() || it == sibs.begin()) return;
    std::iter_swap(it, it - 1);
    service_->PersistFolderOrder(sibs);
    ReloadFolders();
}

void MainWindow::DoMoveFolderDown() {
    if (contextFolderId_ <= 0) return;
    auto sibs = service_->GetSiblingIds(contextFolderId_);
    auto it = std::find(sibs.begin(), sibs.end(), contextFolderId_);
    if (it == sibs.end() || it + 1 == sibs.end()) return;
    std::iter_swap(it, it + 1);
    service_->PersistFolderOrder(sibs);
    ReloadFolders();
}

void MainWindow::DoMoveFolderToRoot() {
    if (contextFolderId_ <= 0) return;
    service_->MoveFolderToRoot(contextFolderId_);
    ReloadFolders();
}

void MainWindow::DoMoveFolder(int sourceId, int targetId) {
    if (sourceId <= 0 || targetId <= 0 || sourceId == targetId) return;
    if (service_ && service_->IsDescendantOf(targetId, sourceId)) return; // 防止循环
    if (service_) service_->MoveFolder(sourceId, targetId);
    expanded_.insert(targetId);
    ReloadFolders();
}

// ==================== 中栏交互 ====================
void MainWindow::OnHeaderClick(int x, int y) {
    if (InTagFilter() && x >= backRect_.left && x <= backRect_.right) {
        LeaveTagFilter();
        return;
    }
    if (x >= cardViewRect_.left && x <= cardViewRect_.right && !cardView_) { cardView_ = true; ComputeHeaderRects(); Invalidate(); return; }
    if (x >= listViewRect_.left && x <= listViewRect_.right && cardView_) { cardView_ = false; Invalidate(); return; }
    if (x >= langRect_.left && x <= langRect_.right) { i18n::Toggle(); Invalidate(); return; }
    if (x >= settingsRect_.left && x <= settingsRect_.right) { OpenSettingsWindow(); return; }
    if (x >= sliderRect_.left && x <= sliderRect_.right) {
        sliderDragging_ = true;
        OnMouseMove(x, y);
    }
}

// 打开设置窗口（模态，居中主窗口；模态保证不会多开）
void MainWindow::OpenSettingsWindow() {
    SettingsWindow dlg(Hwnd());
    dlg.Run();
    // 设置期间新增/移除了资源库时，重载当前资源库
    if (dlg.librariesChanged()) LoadLibrary();
    Invalidate();
}

// 关闭请求：托盘模式隐藏窗口继续运行；退出模式真正退出
bool MainWindow::OnCloseRequested() {
    if (AppConfig::Instance().closeToTray) {
        ShowWindow(Hwnd(), SW_HIDE);
        return false;
    }
    tray_.Remove();
    return true;
}

void MainWindow::ShowMainFromTray() {
    ShowWindow(Hwnd(), SW_SHOW);
    SetForegroundWindow(Hwnd());
}

void MainWindow::ShowTrayMenu() {
    POINT pt; GetCursorPos(&pt);
    PopupMenu menu(pt.x, pt.y, { { i18n::Tr(L"显示主窗口", L"Show Main Window"), 0 }, { i18n::Tr(L"退出", L"Exit"), 1 } });
    int r = menu.Run();
    if (r == 0) ShowMainFromTray();
    else if (r == 1) { tray_.Remove(); Close(); }
}

// 托盘通知（左键恢复；右键菜单）
void MainWindow::OnAppMessage(UINT msg, WPARAM w, LPARAM l) {
    // 阅读器关闭后异步通知：重读进度并刷新
    if (msg == WM_APP + 2) { OnReaderClosed(); return; }
    if (msg != kTrayMsg) return;
    // wParam=图标ID，lParam=事件（鼠标消息）
    if (l == WM_LBUTTONDOWN || l == WM_LBUTTONDBLCLK) ShowMainFromTray();
    else if (l == WM_RBUTTONUP) ShowTrayMenu();
}

void MainWindow::RefreshPageJumpHover(int x, int y) {
    if (!PaginationVisible()) { if (hoverPageBtn_ != -1) { hoverPageBtn_ = -1; Invalidate(); } return; }
    int hv = -1;
    if (y > Height() - 46) {
        float cx = (ContentLeft() + ContentRight()) / 2.0f;
        float bTop = (float)(Height() - 46) + (46 - 28) / 2;
        if (x >= cx - 96 && x <= cx - 68) hv = 0;
        else if (x >= cx + 60 && x <= cx + 104) hv = 1;
        else if (x >= cx + 104 && x <= cx + 132) hv = 2;
    }
    if (hv != hoverPageBtn_) { hoverPageBtn_ = hv; Invalidate(); }
}

void MainWindow::DoOpenReader(int index) {
    if (index < 0 || index >= (int)pageComics_.size()) return;
    // 独立阅读器窗口（可多开），关闭时自释放；关闭后异步通知主窗口刷新进度
    auto* reader = new ReaderWindow();
    HWND mainHwnd = Hwnd();
    reader->SetOnClosed([mainHwnd] { PostMessageW(mainHwnd, WM_APP + 2, 0, 0); });
    reader->OpenComic(service_, pageComics_[index]);
    reader->Create(nullptr, true);
}

// 从阅读器切回主窗口：重读最新阅读进度/状态并刷新详情与当前页卡片
void MainWindow::OnActivate() {
    if (!service_) return;
    if (detailValid_ && detailId_ > 0) {
        ComicModel fresh;
        if (service_->GetComicById(detailId_, fresh)) {
            detail_.currentPage = fresh.currentPage;
            detail_.status = fresh.status;
        }
    }
    for (auto& c : pageComics_) {
        ComicModel f;
        if (service_->GetComicById(c.id, f)) { c.currentPage = f.currentPage; c.status = f.status; }
    }
    Invalidate();
}

// 阅读器关闭后异步刷新：重读全量漫画数据（进度/状态），刷新中栏与详情面板
void MainWindow::OnReaderClosed() {
    if (!service_) return;
    comics_ = service_->GetAllComics();   // 重读最新进度/状态
    // 同步当前页显示的漫画副本
    for (auto& pc : pageComics_) {
        for (auto& c : comics_) {
            if (c.id == pc.id) { pc.currentPage = c.currentPage; pc.status = c.status; break; }
        }
    }
    // 同步详情面板
    if (detailValid_ && detailId_ > 0) {
        for (auto& c : comics_) {
            if (c.id == detailId_) { detail_.currentPage = c.currentPage; detail_.status = c.status; break; }
        }
    }
    Invalidate();
}

void MainWindow::ClearSelection() {
    selectedIds_.clear();
    anchorIndex_ = -1;
    ClearDetail();
}
bool MainWindow::IsSelected(int id) const {
    return std::find(selectedIds_.begin(), selectedIds_.end(), id) != selectedIds_.end();
}
void MainWindow::SelectOnly(int id) {
    selectedIds_.clear();
    selectedIds_.push_back(id);
    SetDetail(id);
}
void MainWindow::ToggleSelected(int id) {
    auto it = std::find(selectedIds_.begin(), selectedIds_.end(), id);
    if (it == selectedIds_.end()) { selectedIds_.push_back(id); SetDetail(id); }
    else selectedIds_.erase(it);
}

void MainWindow::OpenInExplorer(const ComicModel& c) {
    std::wstring fp = W(c.filePath);
    if (fp.empty()) return;
    // 打开文件所在文件夹
    size_t pos = fp.find_last_of(L"\\/");
    std::wstring dir = (pos == std::wstring::npos) ? fp : fp.substr(0, pos);
    if (!DirExistsW(dir)) return;
    ShellExecuteW(Hwnd(), L"open", L"explorer.exe", (L"/select,\"" + fp + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

void MainWindow::DoDeleteSelected() {
    if (selectedIds_.empty()) return;
    std::wstring n = std::to_wstring(selectedIds_.size());
    if (nav_ == QuickNav::Trash) {
        std::wstring msg = (i18n::Current() == i18n::Lang::En)
            ? L"Permanently delete the selected " + n + L" comic(s)?\nTheir .info folders will be deleted."
            : L"确定彻底删除选中的 " + n + L" 本漫画吗？\n其 .info 文件夹将被删除。";
        if (!ShowMsgBox(Hwnd(), msg, i18n::Tr(L"确认删除", L"Confirm Delete"), true)) return;
        for (int id : selectedIds_) {
            ComicModel c;
            if (service_ && service_->GetComicById(id, c)) ComicStorageService::DeleteComicInfo(c);
            if (service_) service_->DeleteComic(id);
        }
        ShowToast(i18n::Current() == i18n::Lang::En
                  ? L"Permanently deleted " + n + L" comic(s)"
                  : L"已彻底删除 " + n + L" 本漫画");
    } else {
        std::wstring msg = (i18n::Current() == i18n::Lang::En)
            ? L"Delete the selected " + n + L" comic(s)?\n(Moved to Trash; restorable there)"
            : L"确定删除选中的 " + n + L" 本漫画吗？\n（放入回收站，可在回收站恢复）";
        if (!ShowMsgBox(Hwnd(), msg, i18n::Tr(L"确认删除", L"Confirm Delete"), true)) return;
        for (int id : selectedIds_) if (service_) service_->TrashComic(id);
        ShowToast(i18n::Current() == i18n::Lang::En
                  ? L"Deleted " + n + L" comic(s)"
                  : L"已删除 " + n + L" 本漫画");
    }
    ClearSelection();
    comics_ = service_->GetAllComics();
    ReloadTags();
    RefreshFiltered();
}

void MainWindow::DoRestoreSelected() {
    if (selectedIds_.empty()) return;
    for (int id : selectedIds_) if (service_) service_->RestoreComic(id);
    ShowToast(i18n::Current() == i18n::Lang::En
              ? L"Restored " + std::to_wstring(selectedIds_.size()) + L" comic(s)"
              : L"已恢复 " + std::to_wstring(selectedIds_.size()) + L" 本漫画");
    ClearSelection();
    comics_ = service_->GetAllComics();
    ReloadTags();
    RefreshFiltered();
}

void MainWindow::DoExportSelected() {
    if (selectedIds_.empty()) return;
    std::wstring dest;
    std::wstring dlgTitle = i18n::Tr(L"选择导出目标文件夹", L"Select export destination folder");
    if (!PickFolder(Hwnd(), dlgTitle.c_str(), dest)) return;

    int ok = 0, skip = 0;
    for (int id : selectedIds_) {
        ComicModel c;
        if (!service_ || !service_->GetComicById(id, c)) { skip++; continue; }
        std::wstring src = W(c.filePath);
        if (src.empty() || !PathExistsW(src)) { skip++; continue; }
        // 目标路径：同名文件加 (1)/(2) 序号避免覆盖
        size_t pos = src.find_last_of(L"\\/");
        std::wstring name = (pos == std::wstring::npos) ? src : src.substr(pos + 1);
        std::wstring target = dest + L"\\" + name;
        if (PathExistsW(target)) {
            std::wstring stem = name, ext;
            size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos) { stem = name.substr(0, dot); ext = name.substr(dot); }
            int seq = 1;
            while (true) {
                target = dest + L"\\" + stem + L" (" + std::to_wstring(seq) + L")" + ext;
                if (!PathExistsW(target)) break;
                seq++;
            }
        }
        if (CopyFileW(src.c_str(), target.c_str(), TRUE)) ok++;
        else skip++;
    }
    std::wstring msg = (i18n::Current() == i18n::Lang::En)
        ? L"Exported " + std::to_wstring(ok) + L" comic(s)" + (skip ? (L", " + std::to_wstring(skip) + L" skipped") : L"")
        : L"已导出 " + std::to_wstring(ok) + L" 本漫画" +
          (skip ? (L"，" + std::to_wstring(skip) + L" 本跳过") : L"");
    ShowToast(msg);
}

void MainWindow::DoEmptyTrash() {
    std::wstring msg = (i18n::Current() == i18n::Lang::En)
        ? L"Empty the Trash?\nThe .info folders of all deleted comics will be permanently deleted."
        : L"确定清空回收站吗？\n所有已删除漫画的 .info 文件夹将被彻底删除。";
    if (!ShowMsgBox(Hwnd(), msg, i18n::Tr(L"清空回收站", L"Empty Trash"), true)) return;
    int n = 0;
    for (auto& c : comics_) {
        if (c.status == ComicStatus::Deleted) {
            ComicStorageService::DeleteComicInfo(c);
            if (service_) service_->DeleteComic(c.id);
            n++;
        }
    }
    ShowToast(i18n::Current() == i18n::Lang::En
              ? L"Trash emptied (" + std::to_wstring(n) + L" comic(s))"
              : L"回收站已清空（" + std::to_wstring(n) + L" 本）");
    comics_ = service_->GetAllComics();
    ReloadTags();
    RefreshFiltered();
}

void MainWindow::ShowComicContextMenu(int x, int y, int index) {
    POINT pt{ x, y };
    if (Hwnd()) ClientToScreen(Hwnd(), &pt);
    bool multi = selectedIds_.size() > 1;   // 多选：不显示"打开阅读/打开位置"
    std::vector<PopupMenu::Item> items;
    if (!multi) {
        items.push_back({ i18n::Tr(L"打开阅读", L"Open Reader"), 1 });
        items.push_back({ i18n::Tr(L"打开文件所在位置", L"Open File Location"), 2 });
        items.push_back({ L"", 0 });
    }
    items.push_back({ i18n::Tr(L"导出漫画", L"Export Comics"), 5 });
    items.push_back({ L"", 0 });
    if (nav_ == QuickNav::Trash) {
        items.push_back({ i18n::Tr(L"恢复漫画", L"Restore"), 3 });
        items.push_back({ i18n::Tr(L"删除", L"Delete"), 4 });
    } else {
        items.push_back({ i18n::Tr(L"删除", L"Delete"), 4 });
    }
    PopupMenu menu(pt.x, pt.y, std::move(items));
    int tag = menu.Run();
    switch (tag) {
        case 1: DoOpenReader(index); break;
        case 2: if (index >= 0 && index < (int)pageComics_.size()) OpenInExplorer(pageComics_[index]); break;
        case 3: DoRestoreSelected(); break;
        case 4: DoDeleteSelected(); break;
        case 5: DoExportSelected(); break;
    }
}

void MainWindow::ToggleView() {
    cardView_ = !cardView_;
    ComputeHeaderRects();
    Invalidate();
}

// ==================== 标签管理 ====================
void MainWindow::ReloadTags() {
    tags_.clear();
    if (service_) tags_ = service_->GetAllTags();
    LayoutTagChips();
}

void MainWindow::LayoutTagChips() {
    tagChipRects_.clear();
    // 起始 y 需低于提示文字（ContentTop+32..48），避免标签盖住提示
    int x = ContentLeft() + 16, y = ContentTop() + 56 + tagScroll_;
    int maxW = ContentRight() - ContentLeft() - 32;
    for (size_t i = 0; i < tags_.size(); i++) {
        std::wstring name = W(tags_[i].name);
        wchar_t cnt[32]; swprintf(cnt, 32, L"%d", tags_[i].count);
        float countW = D2D::TextWidth(cnt, 10);
        // 宽度 = 左内边距10 + 名称 + 间隔8 + 数量 + 右内边距10
        float w = 10 + D2D::TextWidth(name, 11) + 8 + countW + 10;
        if (x + w > ContentLeft() + 16 + maxW && x > ContentLeft() + 16) { x = ContentLeft() + 16; y += 34; }
        tagChipRects_.push_back(D2D1::RectF((float)x, (float)y, (float)(x + w), (float)(y + 26)));
        x += (int)w + 4;
    }
}

int MainWindow::HitTagChip(int x, int y) {
    for (size_t i = 0; i < tagChipRects_.size(); i++) {
        auto& r = tagChipRects_[i];
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return (int)i;
    }
    return -1;
}

void MainWindow::EnterTagFilter(const std::string& tag) {
    tagFilter_ = tag;
    page_ = 1;
    ClearSelection();
    RefreshFiltered();
}

void MainWindow::LeaveTagFilter() {
    tagFilter_.clear();
    page_ = 1;
    ClearSelection();
    RefreshFiltered();
}

void MainWindow::ShowTagContextMenu(int x, int y, int tagIndex) {
    POINT pt{ x, y };
    if (Hwnd()) ClientToScreen(Hwnd(), &pt);
    std::vector<PopupMenu::Item> items = { { i18n::Tr(L"重命名", L"Rename"), 1 }, { i18n::Tr(L"删除", L"Delete"), 2 } };
    PopupMenu menu(pt.x, pt.y, std::move(items));
    switch (menu.Run()) {
        case 1: DoRenameTag(tagIndex); break;
        case 2: DoDeleteTag(tagIndex); break;
    }
}

void MainWindow::DoRenameTag(int tagIndex) {
    if (tagIndex < 0 || tagIndex >= (int)tags_.size()) return;
    std::wstring oldName = W(tags_[tagIndex].name);
    InputDialog dlg(Hwnd(), i18n::Tr(L"重命名标签", L"Rename Tag"), i18n::Tr(L"请输入新标签名：", L"Enter new tag name:"), oldName);
    if (!dlg.Run()) return;
    std::wstring newName = dlg.Text;
    if (newName.empty() || newName == oldName) return;
    RenameTag(U8(oldName), U8(newName));
}

// 跨所有漫画重命名标签并重建 Tags 表
void MainWindow::RenameTag(const std::string& oldName, const std::string& newName) {
    if (oldName == newName) return;
    if (service_) {
        for (auto& c : service_->GetAllComics()) {
            bool changed = false;
            for (auto& t : c.tags) if (t == oldName) { t = newName; changed = true; }
            if (changed) service_->UpdateComic(c);
        }
        service_->SyncTagCounts();
    }
    if (tagFilter_ == oldName) tagFilter_ = newName; // 处于该标签筛选时同步更新标题
    if (detailValid_ && detail_.id == detailId_) {
        for (auto& t : detail_.tags) if (t == oldName) t = newName;
    }
    ReloadTags();
    Invalidate();
}

void MainWindow::DeleteTag(const std::string& tag) {
    if (service_) {
        for (auto& c : service_->GetAllComics()) {
            for (auto it = c.tags.begin(); it != c.tags.end();) {
                if (*it == tag) it = c.tags.erase(it); else ++it;
            }
            service_->UpdateComic(c);
        }
        service_->SyncTagCounts();
    }
    if (tagFilter_ == tag) tagFilter_.clear();
    if (detailValid_ && detail_.id == detailId_) {
        detail_.tags.erase(std::remove(detail_.tags.begin(), detail_.tags.end(), tag), detail_.tags.end());
    }
    ReloadTags();
    RefreshFiltered();
}

void MainWindow::DoDeleteTag(int tagIndex) {
    if (tagIndex < 0 || tagIndex >= (int)tags_.size()) return;
    std::string tag = tags_[tagIndex].name;
    std::wstring msg = (i18n::Current() == i18n::Lang::En)
        ? L"Delete tag \"" + W(tag) + L"\"?\nThis tag will be removed from all comics."
        : L"确定删除标签「" + W(tag) + L"」吗？\n所有漫画中的该标签将被移除。";
    if (!ShowMsgBox(Hwnd(), msg, i18n::Tr(L"确认删除", L"Confirm Delete"), true)) return;
    DeleteTag(tag);
}

// ==================== 分页 ====================
void MainWindow::DoJumpPage() {
    if (pageJumpText_.empty()) return;
    int p = _wtoi(pageJumpText_.c_str());
    pageJumpText_.clear();
    pageJumpFocused_ = false;
    SetPage(p);
}

// ==================== 导入 / Toast ====================
void MainWindow::ImportPaths(const std::vector<std::wstring>& paths, int targetFolderId) {
    if (!service_) { ShowToast(i18n::Tr(L"请先创建或打开资源库", L"Please create or open a library first")); return; }
    if (importing_) { ShowToast(i18n::Tr(L"正在导入，请稍候", L"Importing, please wait")); return; }
    importing_ = true;
    importProgress_ = 0; importOk_ = 0;
    int ok = 0, dup = 0, fail = 0;
    int total = (int)paths.size();
    for (int idx = 0; idx < total; idx++) {
        std::string path = U8(paths[idx]);
        if (service_->IsValidComicSource(path)) {
            ComicModel c;
            if (service_->ImportComic(path, c)) {
                // 复制到仓库（保留原文件），而非剪切
                ComicStorageService::MoveSourceIntoInfo(c, path, true);
                // 归属文件夹
                if (targetFolderId > 0) {
                    c.folderIds.push_back(targetFolderId);
                    c.folderOrders[targetFolderId] = (int)c.folderIds.size();
                    std::string fname;
                    for (auto& n : nodes_) if (n.id == targetFolderId) { fname = U8(n.name); break; }
                    if (!fname.empty()) c.folderNames = fname;
                }
                service_->UpdateComic(c);
                ok++;
            } else dup++;
        } else fail++;
        // 更新进度并同步重绘进度条（不进入消息泵，避免导入期间可交互）
        importProgress_ = (int)((idx + 1) * 100.0f / total);
        importOk_ = ok;
        Invalidate();
        UpdateWindow(Hwnd());
    }
    importing_ = false;
    comics_ = service_->GetAllComics();
    ReloadTags();
    RefreshFiltered();
    if (ok > 0 && fail == 0 && dup == 0)
        ShowToast(i18n::Current() == i18n::Lang::En
                  ? L"Successfully imported " + std::to_wstring(ok) + L" comic(s)"
                  : L"成功导入 " + std::to_wstring(ok) + L" 本漫画");
    else if (ok > 0)
        ShowToast(i18n::Current() == i18n::Lang::En
                  ? L"Imported " + std::to_wstring(ok) + L", " + std::to_wstring(dup) + L" existing, " + std::to_wstring(fail) + L" failed"
                  : L"成功导入 " + std::to_wstring(ok) + L" 本，" + std::to_wstring(dup) + L" 本已存在，" + std::to_wstring(fail) + L" 本失败");
    else if (dup > 0 && fail == 0)
        ShowToast(i18n::Tr(L"未能导入任何漫画（已存在）", L"No comics imported (already exist)"));
    else
        ShowToast(i18n::Tr(L"导入失败：格式不支持或已存在", L"Import failed: unsupported format or already exists"));
}

void MainWindow::ShowToast(const std::wstring& msg) {
    toast_ = msg;
    toastVisible_ = true;
    KillTimerEx(kToastTimer);
    StartTimer(kToastTimer, 2500);
    Invalidate();
}

// 拖拽卡片到左栏文件夹：把选中漫画加入该文件夹（允许多文件夹归属，去重）
void MainWindow::DoDropToFolder(int nodeIndex) {
    if (!service_ || nodeIndex < 0 || nodeIndex >= (int)nodes_.size()) return;
    int target = nodes_[nodeIndex].id;
    if (target <= 0) return;
    std::wstring fname;
    for (auto& nn : nodes_) if (nn.id == target) { fname = nn.name; break; }
    int n = 0;
    for (int id : selectedIds_) {
        ComicModel c;
        if (!service_->GetComicById(id, c)) continue;
        if (std::find(c.folderIds.begin(), c.folderIds.end(), target) != c.folderIds.end()) continue;
        c.folderIds.push_back(target);
        c.folderOrders[target] = (int)c.folderIds.size();
        if (!fname.empty()) c.folderNames = U8(fname);
        service_->UpdateComic(c);
        n++;
    }
    if (n == 0) return;
    comics_ = service_->GetAllComics();
    ReloadFolders();
    RefreshFiltered();
    ShowToast(i18n::Current() == i18n::Lang::En
              ? L"Added comics to \"" + fname + L"\""
              : L"已将漫画加入「" + fname + L"」");
}

// ==================== 右栏详情面板 ====================
void MainWindow::SetDetail(int comicId) {
    ComicModel c;
    if (!service_ || !service_->GetComicById(comicId, c)) { ClearDetail(); return; }
    detail_ = c;
    detailId_ = comicId;
    detailValid_ = true;
    dTitle_.text = W(c.title); dTitle_.caret = (int)dTitle_.text.size(); dTitle_.focused = false;
    dAuthor_.text = W(c.author); dAuthor_.caret = (int)dAuthor_.text.size(); dAuthor_.focused = false;
    dNotes_ = W(c.notes); dNotesCaret_ = (int)dNotes_.size(); dNotesFocused_ = false;
    dEditField_.clear();
    detailScroll_ = 0;
    LayoutDetail();
    Invalidate();
}

void MainWindow::ClearDetail() {
    detailValid_ = false;
    detailId_ = -1;
    dEditField_.clear();
    Invalidate();
}

// 将详情工作副本写回 DB（+ 安全时写 metadata/tags.json）
void MainWindow::CommitDetail() {
    if (!service_ || !detailValid_) return;
    service_->UpdateComic(detail_);
    service_->SyncTagCounts(); // 标签增删后同步 Tags 表，标签管理才能看到最新标签
    std::string infoPath = ComicStorageService::GetComicInfoPath(detail_);
    if (DirExistsW(W(infoPath))) ComicStorageService::SaveComicData(detail_);
}

// 失焦提交当前正编辑字段
void MainWindow::SaveDetailField() {
    if (!detailValid_) return;
    if (dEditField_ == "title") detail_.title = U8(dTitle_.text);
    else if (dEditField_ == "author") detail_.author = U8(dAuthor_.text);
    else if (dEditField_ == "notes") detail_.notes = U8(dNotes_);
    dTitle_.focused = dAuthor_.focused = dNotesFocused_ = false;
    dEditField_.clear();
    CommitDetail();
    RefreshFromDetail();
}

// 提交后刷新中栏与标签云（详情本身保持）
void MainWindow::RefreshFromDetail() {
    if (!service_) return;
    comics_ = service_->GetAllComics();
    ReloadTags();
    RefreshFiltered();
}

void MainWindow::LayoutDetail() {
    int DL = DetailLeft(), DR = DetailRight();
    int pw = DR - DL;
    dTagRects_.clear();
    // 底部操作栏
    dOpenRect_ = D2D1::RectF((float)DL + 16, (float)Height() - 52, (float)DR - 16, (float)Height() - 16);
    // 内容区（可滚动）
    int y = DetailTop() + 16 - detailScroll_;
    // 大封面 200x280 居中
    float cw = 200, ch = 280;
    dCoverRect_ = D2D1::RectF(DL + (pw - cw) / 2.0f, (float)y, DL + (pw - cw) / 2.0f + cw, (float)(y + (int)ch));
    y += 280 + 16;
    // 标题
    dTitleRect_ = D2D1::RectF(DL + 20, (float)y, DR - 20, (float)(y + 34)); y += 34 + 14;
    // 作者
    dAuthorLabelRect_ = D2D1::RectF(DL + 20, (float)y, DR - 20, (float)(y + 16));
    dAuthorRect_ = D2D1::RectF(DL + 20, (float)(y + 18), DR - 20, (float)(y + 18 + 34)); y += 18 + 34 + 14;
    // 所属文件夹
    dFolderRect_ = D2D1::RectF(DL + 20, (float)y, DR - 20, (float)(y + 34)); y += 34 + 10;
    // 创建时间
    dCreatedRect_ = D2D1::RectF(DL + 20, (float)y, DR - 20, (float)(y + 34)); y += 34 + 10;
    // 阅读进度 + 状态（两列）
    dProgRect_ = D2D1::RectF(DL + 20, (float)y, DL + (pw) / 2, (float)(y + 34));
    dStatusRect_ = D2D1::RectF(DL + pw / 2, (float)y, DR - 20, (float)(y + 34)); y += 34 + 14;
    // 标签区：标签标题 + 添加按钮
    dTagLabelRect_ = D2D1::RectF(DL + 20, (float)y, DR - 20, (float)(y + 22)); y += 22 + 6;
    dAddTagRect_ = D2D1::RectF((float)DR - 42, dTagLabelRect_.top, (float)DR - 20, dTagLabelRect_.top + 22);
    // 标签 chip（流式换行）
    int cx = DL + 20, cy = y;
    int maxX = DR - 20;
    for (size_t i = 0; i < detail_.tags.size(); i++) {
        std::wstring name = W(detail_.tags[i]);
        float w = 16 + D2D::TextWidth(name, 11) + 4;
        if (cx + (int)w > maxX && cx > DL + 20) { cx = DL + 20; cy += 26 + 4; }
        dTagRects_.push_back(D2D1::RectF((float)cx, (float)cy, cx + (int)w, (float)(cy + 26)));
        cx += (int)w + 4;
    }
    // 备注区：上方还需绘制"备注"标签（高于备注框 24px）
    // 有标签：紧跟最后一行标签 chip 下方留 48px；无标签：在标签标题下方加大间距，避免与备注挤在一起
    if (!detail_.tags.empty()) y = cy + 26 + 48; else y += 70;
    // 备注
    dNotesRect_ = D2D1::RectF(DL + 20, (float)y, DR - 20, (float)(y + 100)); y += 100 + 14;
    detailContentH_ = y - (DetailTop() + 16 - detailScroll_);
}

void MainWindow::DrawDetailPanel(ID2D1RenderTarget* rt) {
    // 面板背景
    D2D1_RECT_F panel{ (float)DetailLeft(), (float)DetailTop(), (float)DetailRight(), (float)DetailBottom() };
    D2D::RoundedRect(rt, panel, 0, theme::BgSidebar());
    // 标题
    D2D1_RECT_F title{ (float)DetailLeft() + 20, (float)TitleBarHeight + 16, (float)DetailRight() - 16, (float)TitleBarHeight + 38 };
    D2D::Text(rt, i18n::Tr(L"漫画详情", L"Comic Details"), title, theme::TextPrimary(), 15, DWRITE_FONT_WEIGHT_SEMI_BOLD,
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // 分隔线
    D2D::RoundedRect(rt, D2D1::RectF((float)DetailLeft(), (float)TitleBarHeight + 44, (float)DetailRight(), (float)TitleBarHeight + 45),
                     0, theme::BorderColor());

    if (!detailValid_) { DrawDetailEmpty(rt); return; }

    // 内容区裁剪：防止滚动内容压住底部"打开阅读"按钮造成文本被遮挡
    D2D1_RECT_F clipRect{ (float)DetailLeft(), (float)(TitleBarHeight + 48), (float)DetailRight(), (float)(Height() - 52) };
    rt->PushLayer(D2D1::LayerParameters(clipRect), nullptr);

    // 大封面（悬停遮罩 + 拖拽提示）
    DrawCover(rt, detail_, dCoverRect_, 8);
    if (dCoverHover_) {
        D2D::RoundedRect(rt, dCoverRect_, 8, D2D1::ColorF(0, 0, 0, 0.667f));
        D2D1_RECT_F ic{ dCoverRect_.left, dCoverRect_.top + (dCoverRect_.bottom - dCoverRect_.top) * 0.38f,
                        dCoverRect_.right, dCoverRect_.top + (dCoverRect_.bottom - dCoverRect_.top) * 0.62f };
        D2D::Icon(rt, L"\xE748", ic, D2D1::ColorF(1, 1, 1), 28);
        D2D1_RECT_F tc{ dCoverRect_.left, dCoverRect_.top + (dCoverRect_.bottom - dCoverRect_.top) * 0.62f,
                        dCoverRect_.right, dCoverRect_.top + (dCoverRect_.bottom - dCoverRect_.top) * 0.78f };
        D2D::Text(rt, i18n::Tr(L"拖拽图片替换封面", L"Drag an image to replace cover"), tc, D2D1::ColorF(1, 1, 1), 12, DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // 标题（可编辑）
    dTitle_.rect = dTitleRect_;
    dTitle_.Draw(rt);
    // 作者
    D2D::Text(rt, i18n::Tr(L"作者", L"Author"), dAuthorLabelRect_, theme::TextSecondary(), 11);
    dAuthor_.rect = dAuthorRect_;
    dAuthor_.Draw(rt);
    // 所属文件夹
    D2D::Text(rt, i18n::Tr(L"所属文件夹", L"Folder"), D2D1::RectF(dFolderRect_.left, dFolderRect_.top, dFolderRect_.right, dFolderRect_.top + 16),
              theme::TextSecondary(), 11);
    D2D::Text(rt, W(detail_.folderNames), D2D1::RectF(dFolderRect_.left, dFolderRect_.top + 16, dFolderRect_.right, dFolderRect_.bottom),
              theme::TextPrimary(), 12);
    // 创建时间
    D2D::Text(rt, i18n::Tr(L"创建时间", L"Created"), D2D1::RectF(dCreatedRect_.left, dCreatedRect_.top, dCreatedRect_.right, dCreatedRect_.top + 16),
              theme::TextSecondary(), 11);
    D2D::Text(rt, W(detail_.createdAt), D2D1::RectF(dCreatedRect_.left, dCreatedRect_.top + 16, dCreatedRect_.right, dCreatedRect_.bottom),
              theme::TextPrimary(), 12);
    // 阅读进度 + 状态
    D2D::Text(rt, i18n::Tr(L"阅读进度", L"Progress"), D2D1::RectF(dProgRect_.left, dProgRect_.top, dProgRect_.right, dProgRect_.top + 16),
              theme::TextSecondary(), 11);
    wchar_t prog[48]; swprintf(prog, 48, L"%d / %d", detail_.currentPage + 1, detail_.pageCount);
    D2D::Text(rt, prog, D2D1::RectF(dProgRect_.left, dProgRect_.top + 16, dProgRect_.right, dProgRect_.bottom),
              theme::TextPrimary(), 12);
    D2D::Text(rt, i18n::Tr(L"状态", L"Status"), D2D1::RectF(dStatusRect_.left, dStatusRect_.top, dStatusRect_.right, dStatusRect_.top + 16),
              theme::TextSecondary(), 11);
    std::wstring st = i18n::Tr(L"未读", L"Unread");
    switch (detail_.status) {
        case ComicStatus::Reading: st = i18n::Tr(L"阅读中", L"Reading"); break;
        case ComicStatus::Completed: st = i18n::Tr(L"已读完", L"Completed"); break;
        case ComicStatus::OnHold: st = i18n::Tr(L"搁置", L"On Hold"); break;
        default: st = i18n::Tr(L"未读", L"Unread");
    }
    D2D::Text(rt, st, D2D1::RectF(dStatusRect_.left, dStatusRect_.top + 16, dStatusRect_.right, dStatusRect_.bottom),
              theme::AccentCyan(), 12);
    // 标签区
    D2D::Text(rt, i18n::Tr(L"标签", L"Tags"), D2D1::RectF(dTagLabelRect_.left, dTagLabelRect_.top, dTagLabelRect_.right - 30, dTagLabelRect_.bottom),
              theme::TextSecondary(), 11);
    bool addHv = detailHover_ == 1;
    if (addHv) D2D::RoundedRect(rt, dAddTagRect_, 4, theme::BgCardHover());
    D2D::Icon(rt, L"\xE710", dAddTagRect_, addHv ? theme::AccentCyan() : theme::TextSecondary(), 12);
    for (size_t i = 0; i < dTagRects_.size(); i++) {
        D2D::RoundedRect(rt, dTagRects_[i], 10, theme::MutedCyan(), theme::MutedCyan(), 1.0f);
        D2D1_RECT_F tr{ dTagRects_[i].left + 8, dTagRects_[i].top, dTagRects_[i].right - 4, dTagRects_[i].bottom };
        D2D::Text(rt, W(detail_.tags[i]), tr, theme::AccentCyan(), 11);
    }
    // 备注
    D2D1_RECT_F nl{ dNotesRect_.left, dNotesRect_.top - 24, dNotesRect_.right, dNotesRect_.top - 4 };
    D2D::Text(rt, i18n::Tr(L"备注", L"Notes"), nl, theme::TextSecondary(), 11);
    D2D::RoundedRect(rt, dNotesRect_, 6, theme::BgCard(), dNotesFocused_ ? theme::AccentCyan() : theme::BorderColor(),
                     dNotesFocused_ ? 1.5f : 1.0f);
    // 备注文本（多行换行）
    dNotesDisplay_ = NotesLines(dNotesRect_.right - dNotesRect_.left - 16);
    int lineH = 20;
    int ty = (int)dNotesRect_.top + 6;
    // 计算光标所在行（聚焦时）
    int caretLine = -1, caretCol = -1;
    if (dNotesFocused_) {
        int remaining = dNotesCaret_;
        for (size_t i = 0; i < dNotesDisplay_.size(); i++) {
            int len = (int)dNotesDisplay_[i].size();
            if (remaining <= len) { caretLine = (int)i; caretCol = remaining; break; }
            remaining -= len + 1; // +1 为逻辑换行符
        }
        if (caretLine < 0) { caretLine = (int)dNotesDisplay_.size() - 1; caretCol = (int)(caretLine >= 0 ? dNotesDisplay_.back().size() : 0); }
    }
    for (size_t i = 0; i < dNotesDisplay_.size(); i++) {
        D2D1_RECT_F lr{ dNotesRect_.left + 8, (float)ty, dNotesRect_.right - 8, (float)(ty + lineH) };
        D2D::Text(rt, dNotesDisplay_[i], lr, theme::TextPrimary(), 12);
        if (dNotesFocused_ && (int)i == caretLine) {
            std::wstring before = dNotesDisplay_[i].substr(0, (size_t)(caretCol < 0 ? 0 : caretCol));
            float cx = dNotesRect_.left + 8 + D2D::TextWidth(before, 12);
            ID2D1SolidColorBrush* b = nullptr;
            rt->CreateSolidColorBrush(theme::AccentCyan(), &b);
            if (b) {
                rt->DrawLine(D2D1::Point2F(cx, (float)ty), D2D1::Point2F(cx, (float)(ty + lineH)), b, 1.0f);
                b->Release();
            }
        }
        ty += lineH;
    }
    rt->PopLayer();
    // 底部"打开阅读"按钮
    bool openHv = detailHover_ == 2;
    D2D::RoundedRect(rt, dOpenRect_, 6, openHv ? theme::AccentCyan() : theme::BgCard(),
                     theme::BorderColor(), openHv ? 0 : 1.0f);
    D2D::Text(rt, i18n::Tr(L"打开阅读", L"Open Reader"), dOpenRect_, openHv ? theme::AccentText() : theme::TextPrimary(), 13,
              DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void MainWindow::DrawDetailEmpty(ID2D1RenderTarget* rt) {
    D2D1_RECT_F ic{ (float)DetailLeft(), (float)(Height() / 2 - 60), (float)DetailRight(), (float)(Height() / 2 - 12) };
    D2D::Icon(rt, L"\xE8B9", ic, D2D1::ColorF(0x3A, 0x40, 0x50, 1.0f), 48);
    D2D1_RECT_F tc{ (float)DetailLeft() + 10, (float)(Height() / 2 - 6), (float)DetailRight() - 10, (float)(Height() / 2 + 18) };
    D2D::Text(rt, i18n::Tr(L"选择一本漫画查看详情", L"Select a comic to view details"), tc, theme::TextSecondary(), 13, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

// 备注文本按宽度换行（保留逻辑换行）
std::vector<std::wstring> MainWindow::NotesLines(float maxW) const {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (size_t i = 0; i < dNotes_.size(); i++) {
        wchar_t ch = dNotes_[i];
        if (ch == L'\n') { out.push_back(cur); cur.clear(); continue; }
        if (!cur.empty() && D2D::TextWidth(cur + std::wstring(1, ch), 12) > maxW) { out.push_back(cur); cur.clear(); }
        cur += ch;
    }
    out.push_back(cur);
    return out;
}

// 备注点击 → 逻辑光标位置
int MainWindow::NotesHitCaret(int x, int y) const {
    int lineH = 20;
    int relY = y - (int)dNotesRect_.top;
    int line = relY / lineH;
    std::vector<std::wstring> lines = NotesLines(dNotesRect_.right - dNotesRect_.left - 16);
    if (line < 0) line = 0;
    if (line >= (int)lines.size()) return (int)dNotes_.size();
    // 累计到该显示行的逻辑偏移
    int idx = 0;
    for (int i = 0; i < line; i++) {
        idx += (int)lines[i].size();
        if (i + 1 < (int)lines.size()) idx += 1; // 逻辑换行符
    }
    // 行内按字符宽度定位
    int relX = x - (int)dNotesRect_.left - 8;
    const std::wstring& ln = lines[line];
    for (size_t k = 0; k < ln.size(); k++) {
        if (D2D::TextWidth(ln.substr(0, k + 1), 12) > relX) return idx + (int)k;
    }
    return idx + (int)ln.size();
}

// 添加标签（逗号分隔多个）
void MainWindow::DoAddDetailTag() {
    if (!detailValid_) return;
    InputDialog dlg(Hwnd(), i18n::Tr(L"添加标签", L"Add Tags"), i18n::Tr(L"请输入标签（多个用逗号分隔）：", L"Enter tags (separate multiple with commas):"));
    if (!dlg.Run()) return;
    std::wstring input = dlg.Text;
    if (input.empty()) return;
    bool added = false;
    size_t pos = 0;
    while (pos <= input.size()) {
        size_t comma = input.find(L',', pos);
        std::wstring piece = (comma == std::wstring::npos) ? input.substr(pos) : input.substr(pos, comma - pos);
        pos = (comma == std::wstring::npos) ? input.size() + 1 : comma + 1;
        // 去首尾空白
        size_t b = 0, e = piece.size();
        while (b < e && iswspace(piece[b])) b++;
        while (e > b && iswspace(piece[e - 1])) e--;
        piece = piece.substr(b, e - b);
        if (piece.empty()) continue;
        std::string tag = U8(piece);
        if (std::find(detail_.tags.begin(), detail_.tags.end(), tag) == detail_.tags.end()) {
            detail_.tags.push_back(tag);
            added = true;
        }
    }
    if (!added) { ShowToast(i18n::Tr(L"未添加标签（已存在或为空）", L"No tags added (already exist or empty)")); return; }
    CommitDetail();
    RefreshFromDetail();
    LayoutDetail();
}

// 详情标签右键：重命名 / 删除（跨所有漫画）
void MainWindow::ShowDetailTagMenu(int x, int y, const std::string& tag) {
    POINT pt{ x, y };
    if (Hwnd()) ClientToScreen(Hwnd(), &pt);
    std::vector<PopupMenu::Item> items = { { i18n::Tr(L"重命名", L"Rename"), 1 }, { i18n::Tr(L"删除", L"Delete"), 2 } };
    PopupMenu menu(pt.x, pt.y, std::move(items));
    switch (menu.Run()) {
        case 1: {
            InputDialog dlg(Hwnd(), i18n::Tr(L"重命名标签", L"Rename Tag"), i18n::Tr(L"请输入新标签名：", L"Enter new tag name:"), W(tag));
            if (!dlg.Run() || dlg.Text.empty()) return;
            std::string neu = U8(dlg.Text);
            if (neu == tag) return;
            RenameTag(tag, neu);
            LayoutDetail();
            break;
        }
        case 2: {
            std::wstring msg = (i18n::Current() == i18n::Lang::En)
                ? L"Delete tag \"" + W(tag) + L"\"?\nThis tag will be removed from all comics."
                : L"确定删除标签「" + W(tag) + L"」吗？\n所有漫画中的该标签将被移除。";
            if (ShowMsgBox(Hwnd(), msg, i18n::Tr(L"确认删除", L"Confirm Delete"), true))
                DeleteTag(tag);
            LayoutDetail();
            break;
        }
    }
}

} // namespace ark::ui