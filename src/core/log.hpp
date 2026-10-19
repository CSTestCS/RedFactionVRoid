// Thread-safe append-only log file (RFVRoid.log next to the game executable).
#pragma once
#include <string>

namespace rfv::log {

void open(const std::string& utf8Path);
void write(const char* fmt, ...)
#if defined(__MINGW32__)
    __attribute__((format(gnu_printf, 1, 2)))
#elif defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void close();

} // namespace rfv::log

#define RFV_LOG(...) ::rfv::log::write(__VA_ARGS__)
