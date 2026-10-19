// RedFactionVRoid installer: finds Red Faction Guerrilla Re-Mars-tered in the Steam
// libraries and copies the embedded mod DLL (dxgi.dll) next to rfg.exe.
//
//   RFVRoid_Installer.exe                    interactive
//   RFVRoid_Installer.exe /install [dir]     silent install
//   RFVRoid_Installer.exe /uninstall [dir]   silent uninstall
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <cstdio>
#include <string>
#include <vector>

#include "resource.h"

namespace {

const wchar_t* kTitle = L"RedFactionVRoid Installer";
const char kMarker[] = "RFVRoid-dxgi-proxy";  // present in our dxgi.dll
constexpr int kAppId = 667720;                // Red Faction Guerrilla Re-MARS-tered

bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool dirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    std::wstring s(buf);
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    return s;
}

std::string readText(const std::wstring& path) {
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// Extracts all values of "key" "value" pairs from a Valve KeyValues (.vdf / .acf) file.
std::vector<std::string> vdfValues(const std::string& text, const std::string& key) {
    std::vector<std::string> out;
    std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        pos += needle.size();
        size_t q1 = text.find('"', pos);
        size_t nl = text.find('\n', pos);
        if (q1 == std::string::npos || (nl != std::string::npos && q1 > nl)) continue;
        std::string v;
        size_t i = q1 + 1;
        for (; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;  // \\ -> \ .
            v += text[i];
        }
        out.push_back(v);
        pos = i;
    }
    return out;
}

std::vector<std::wstring> steamLibraries() {
    std::vector<std::wstring> libs;
    std::wstring steam = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) steam = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) return libs;
    libs.push_back(steam);
    std::string vdf = readText(steam + L"\\steamapps\\libraryfolders.vdf");
    for (const auto& p : vdfValues(vdf, "path")) {
        std::wstring w = widen(p);
        bool dup = false;
        for (auto& l : libs)
            if (CompareStringOrdinal(l.c_str(), -1, w.c_str(), -1, TRUE) == CSTR_EQUAL) dup = true;
        if (!dup) libs.push_back(w);
    }
    return libs;
}

std::wstring findGame() {
    for (const auto& lib : steamLibraries()) {
        std::wstring apps = lib + L"\\steamapps";
        // Preferred: the app manifest tells us the install folder.
        std::string acf = readText(apps + L"\\appmanifest_" + std::to_wstring(kAppId) + L".acf");
        auto dirs = vdfValues(acf, "installdir");
        if (!dirs.empty()) {
            std::wstring d = apps + L"\\common\\" + widen(dirs[0]);
            if (fileExists(d + L"\\rfg.exe")) return d;
        }
        // Fallback: any folder in steamapps\common that contains rfg.exe.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((apps + L"\\common\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring d = apps + L"\\common\\" + fd.cFileName;
            if (fileExists(d + L"\\rfg.exe")) { FindClose(h); return d; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return {};
}

std::wstring pickFolder() {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Select the Red Faction Guerrilla Re-MARS-tered folder (the one with rfg.exe)");
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

bool isOurDll(const std::wstring& path) {
    std::string data = readText(path);
    return !data.empty() && data.find(kMarker) != std::string::npos;
}

bool gameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, L"rfg.exe") == 0) { found = true; break; }
    CloseHandle(snap);
    return found;
}

bool writePayload(const std::wstring& dest, std::wstring& error) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PAYLOAD_DLL), RT_RCDATA);
    HGLOBAL g = res ? LoadResource(nullptr, res) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!data || !size) { error = L"The installer is damaged (payload missing)."; return false; }
    std::wstring tmp = dest + L".rfvroid_tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = L"Cannot write to the game folder (try running as administrator)."; return false; }
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, nullptr) && written == size;
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        error = L"Cannot replace " + dest + L" (is the game still running?)";
        return false;
    }
    return true;
}

bool install(const std::wstring& dir, std::wstring& message) {
    std::wstring dxgi = dir + L"\\dxgi.dll", chain = dir + L"\\dxgi_chain.dll";
    if (fileExists(dxgi) && !isOurDll(dxgi)) {
        // Another dxgi.dll mod (e.g. ReShade): keep it working by chaining to it.
        if (fileExists(chain)) {
            message = L"Both dxgi.dll and dxgi_chain.dll already exist and neither is RedFactionVRoid.\n"
                      L"Please remove one of them and run the installer again.";
            return false;
        }
        if (!MoveFileW(dxgi.c_str(), chain.c_str())) { message = L"Could not rename the existing dxgi.dll."; return false; }
    }
    std::wstring err;
    if (!writePayload(dxgi, err)) { message = err; return false; }
    CreateDirectoryW((dir + L"\\vrm").c_str(), nullptr);
    message = L"RedFactionVRoid is installed in:\n" + dir +
              L"\n\n1. Copy your .vrm avatar files into the \"vrm\" folder there.\n"
              L"2. Start the game, pause, and choose VRM SKIN (or press Y / V while paused, or F7 anywhere).\n\n"
              L"Settings: RFVRoid.ini   Log: RFVRoid.log";
    if (fileExists(chain)) message += L"\n\nYour previous dxgi.dll was kept as dxgi_chain.dll and is still loaded.";
    return true;
}

bool uninstall(const std::wstring& dir, std::wstring& message) {
    std::wstring dxgi = dir + L"\\dxgi.dll", chain = dir + L"\\dxgi_chain.dll";
    if (!fileExists(dxgi) || !isOurDll(dxgi)) { message = L"RedFactionVRoid is not installed in:\n" + dir; return false; }
    if (!DeleteFileW(dxgi.c_str())) { message = L"Could not delete dxgi.dll (is the game running?)"; return false; }
    if (fileExists(chain)) MoveFileW(chain.c_str(), dxgi.c_str());
    message = L"RedFactionVRoid was removed.\nYour vrm folder and RFVRoid.ini were left in place.";
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode = argc > 1 ? argv[1] : L"";
    std::wstring dir = argc > 2 ? argv[2] : L"";
    bool silent = _wcsicmp(mode.c_str(), L"/install") == 0 || _wcsicmp(mode.c_str(), L"/uninstall") == 0;

    if (dir.empty()) dir = findGame();
    if (dir.empty() && !silent) {
        MessageBoxW(nullptr,
                    L"Red Faction Guerrilla Re-MARS-tered was not found in your Steam libraries.\n"
                    L"Please select the game folder (the one that contains rfg.exe).",
                    kTitle, MB_OK | MB_ICONINFORMATION);
        dir = pickFolder();
    }
    if (dir.empty() || !fileExists(dir + L"\\rfg.exe")) {
        if (!silent) MessageBoxW(nullptr, L"rfg.exe was not found in the selected folder. Nothing was changed.", kTitle, MB_OK | MB_ICONERROR);
        return 2;
    }

    if (gameRunning()) {
        if (silent) return 3;
        MessageBoxW(nullptr, L"Please close Red Faction Guerrilla first, then run the installer again.", kTitle, MB_OK | MB_ICONWARNING);
        return 3;
    }

    std::wstring message;
    bool ok = false;
    if (_wcsicmp(mode.c_str(), L"/install") == 0) ok = install(dir, message);
    else if (_wcsicmp(mode.c_str(), L"/uninstall") == 0) ok = uninstall(dir, message);
    else {
        bool installed = fileExists(dir + L"\\dxgi.dll") && isOurDll(dir + L"\\dxgi.dll");
        int choice;
        if (installed) {
            choice = MessageBoxW(nullptr,
                                 (L"RedFactionVRoid is already installed in:\n" + dir +
                                  L"\n\nYes = update / reinstall\nNo = uninstall\nCancel = do nothing").c_str(),
                                 kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (choice == IDCANCEL) return 0;
            ok = choice == IDYES ? install(dir, message) : uninstall(dir, message);
        } else {
            choice = MessageBoxW(nullptr, (L"Install RedFactionVRoid (VRM skins for Mason) into:\n" + dir + L"\n\nContinue?").c_str(), kTitle,
                                 MB_OKCANCEL | MB_ICONQUESTION);
            if (choice != IDOK) return 0;
            ok = install(dir, message);
        }
        MessageBoxW(nullptr, message.c_str(), kTitle, MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        if (ok && !installed) ShellExecuteW(nullptr, L"explore", (dir + L"\\vrm").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    LocalFree(argv);
    CoUninitialize();
    return ok ? 0 : 1;
}
