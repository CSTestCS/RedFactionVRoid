#include "paths.hpp"

#include <windows.h>

#include <algorithm>

namespace rfv::win {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring gameDirectoryW() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::string gameDirectory() { return narrow(gameDirectoryW()); }

std::string vrmDirectory() {
    std::wstring d = gameDirectoryW() + L"\\vrm";
    CreateDirectoryW(d.c_str(), nullptr);
    return narrow(d);
}

std::string configPath() { return gameDirectory() + "\\RFVRoid.ini"; }
std::string statePath() { return gameDirectory() + "\\RFVRoid_state.ini"; }
std::string logPath() { return gameDirectory() + "\\RFVRoid.log"; }

bool fileExists(const std::string& p) {
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::string> listVrmFiles() {
    std::vector<std::string> out;
    std::wstring dir = widen(vrmDirectory());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.vrm").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(narrow(dir + L"\\" + fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return CompareStringOrdinal(widen(a).c_str(), -1, widen(b).c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return out;
}

std::string fileStem(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

} // namespace rfv::win
