#pragma once
#include <d3d11.h>

#include <string>

namespace rfv::win {

// Compiles HLSL with d3dcompiler_47.dll (loaded at runtime; ships with Windows 10+).
// Returns nullptr and fills `error` on failure. Caller releases the blob.
ID3DBlob* compileShader(const char* source, const char* entry, const char* target, std::string& error);

} // namespace rfv::win
