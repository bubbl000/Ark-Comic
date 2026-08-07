#pragma once
// 简化日志系统（从方舟图片浏览器移植，路径改为 ArkComic）
#include <string>
#include <sstream>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <Windows.h>

class Logger {
public:
    enum Level { L_DBG = 0, L_INFO = 1, L_WARN = 2, L_ERR = 3 };

    static Logger& Instance() {
        static Logger inst;
        return inst;
    }

    void Log(Level level, const char* tag, const std::string& msg) {
        std::lock_guard<std::mutex> lock(_mutex);
        EnsureFile();
        if (!_file.is_open()) return;

        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        struct tm local; localtime_s(&local, &tt);

        char buf[512];
        int n = snprintf(buf, sizeof(buf),
            "[%02d:%02d:%02d.%03lld] [%s] %s: %s\n",
            local.tm_hour, local.tm_min, local.tm_sec, (long long)ms.count(),
            LevelStr(level), tag, msg.c_str());
        OutputDebugStringA(buf);

        _file.write(buf, n);
        _file.flush();
    }

    static std::wstring GetLogDir() {
        wchar_t path[MAX_PATH];
        GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
        return std::wstring(path) + L"\\ArkComic\\logs";
    }

private:
    std::mutex _mutex;
    std::ofstream _file;
    std::wstring _currentDate;

    Logger() = default;
    ~Logger() { if (_file.is_open()) _file.close(); }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static const char* LevelStr(Level l) {
        switch (l) {
        case L_DBG:  return "DEBUG";
        case L_INFO: return "INFO";
        case L_WARN: return "WARN";
        case L_ERR:  return "ERROR";
        default:     return "?";
        }
    }

    void EnsureFile() {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        struct tm local; localtime_s(&local, &tt);

        wchar_t ds[16];
        swprintf(ds, 16, L"%04d-%02d-%02d",
            local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);

        if (_currentDate == ds && _file.is_open()) return;
        _currentDate = ds;
        if (_file.is_open()) _file.close();

        auto dir = GetLogDir();
        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring parent = dir.substr(0, pos);
            CreateDirectoryW(parent.c_str(), nullptr);
        }
        CreateDirectoryW(dir.c_str(), nullptr);
        _file.open(dir + L"\\arkcomic-" + ds + L".log", std::ios::app);
    }
};

#define LOG_DBG(tag, msg)  Logger::Instance().Log(Logger::L_DBG, tag, msg)
#define LOG_INFO(tag, msg) Logger::Instance().Log(Logger::L_INFO, tag, msg)
#define LOG_WARN(tag, msg) Logger::Instance().Log(Logger::L_WARN, tag, msg)
#define LOG_ERR(tag, msg)  Logger::Instance().Log(Logger::L_ERR,  tag, msg)

class LogStream {
public:
    LogStream(Logger::Level l, const char* tag) : _l(l), _tag(tag) {}
    ~LogStream() { Logger::Instance().Log(_l, _tag, _ss.str()); }
    std::ostringstream& S() { return _ss; }
private:
    Logger::Level _l;
    const char* _tag;
    std::ostringstream _ss;
};

#define LOG_DBG_STREAM(tag) LogStream(Logger::L_DBG, tag).S()
#define LOG_INFO_STREAM(tag) LogStream(Logger::L_INFO, tag).S()
#define LOG_WARN_STREAM(tag) LogStream(Logger::L_WARN, tag).S()
#define LOG_ERR_STREAM(tag)  LogStream(Logger::L_ERR,  tag).S()
