#pragma once
#include <string>
#include <vector>

namespace rfv::win {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

std::wstring gameDirectoryW();           // folder containing rfg.exe
std::string gameDirectory();             // UTF-8
std::string vrmDirectory();              // <game>\vrm (created on demand)
std::string configPath();                // <game>\RFVRoid.ini
std::string statePath();                 // <game>\RFVRoid_state.ini
std::string logPath();                   // <game>\RFVRoid.log
bool fileExists(const std::string& utf8);
std::vector<std::string> listVrmFiles(); // full UTF-8 paths, sorted by name
std::string fileStem(const std::string& path);

} // namespace rfv::win
