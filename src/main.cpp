#include "MainWindow.h"
#include "AppConfig.h"
#include "Theme.h"
#include "ActivityLog.h"
#include <windows.h>
#include <psapi.h>

// 性能遥测：每秒聚合进程内存 + 跨窗口快照状态，由 ActivityLog::PollTick 在 WM_TIMER 中调用
static void CollectSnapshot() {
    PROCESS_MEMORY_COUNTERS pmc = {};
    bool hasMem = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    double memWs = hasMem ? (double)pmc.WorkingSetSize / (1024.0 * 1024.0) : 0;
    double memPk = hasMem ? (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0) : 0;

    std::lock_guard<std::mutex> lk(PerfState::Mutex());
    auto& s = PerfState::Current();
    ActivityLog::Instance().LogSnapshot(L"性能", {
        Perf::S("library", ActivityFmt::NarrowUtf8(s.libraryPath)),
        Perf::S("comic", ActivityFmt::NarrowUtf8(s.comicTitle)),
        Perf::N("page", (double)s.currentPage),
        Perf::N("total", (double)s.totalPages),
        Perf::N("cache_entries", (double)s.cacheEntries),
        Perf::N("cache_mb", s.cacheMemMB),
        Perf::N("mem_ws_mb", memWs),
        Perf::N("mem_pk_mb", memPk)
    });
}

// 程序入口（WIN32 GUI 子系统）
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // 应用已保存的主题强调色
    ark::theme::SetAccentHex(ark::AppConfig::Instance().themeColor);
    // 调试遥测：活动日志窗口 + 性能落盘 JSONL（ARK_PERF=1 开启）
    ActivityLog::Instance().Init(GetModuleHandleW(nullptr));
    ActivityLog::SetThreadName("main");  // 主线程角色名（JSONL thread_name 字段）
    ActivityLog::Instance().SetSnapshotCollector(CollectSnapshot);
    ark::ui::MainWindow wnd;
    wnd.Run();
    return 0;
}
