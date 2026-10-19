#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rfv::log {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
}

void open(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    g_file = _wfopen(w.c_str(), L"w");
#else
    g_file = std::fopen(path.c_str(), "w");
#endif
}

void write(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
    if (g_file) {
        std::fprintf(g_file, "[%s] %s\n", ts, buf);
        std::fflush(g_file);
    }
#ifdef _WIN32
    OutputDebugStringA("[RFVRoid] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
#endif
}

void close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

} // namespace rfv::log
