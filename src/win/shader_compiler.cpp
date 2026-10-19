#include "shader_compiler.hpp"

#include <d3dcompiler.h>
#include <windows.h>

#include <cstring>

namespace rfv::win {

ID3DBlob* compileShader(const char* source, const char* entry, const char* target, std::string& error) {
    static pD3DCompile compile = [] {
        for (const wchar_t* name : {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"}) {
            if (HMODULE m = LoadLibraryW(name))
                if (auto f = reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile"))) return f;
        }
        return pD3DCompile(nullptr);
    }();
    if (!compile) { error = "d3dcompiler_47.dll not found"; return nullptr; }
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(source, std::strlen(source), "rfvroid", nullptr, nullptr, entry, target,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "compile failed";
        if (errors) errors->Release();
        if (code) code->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    return code;
}

} // namespace rfv::win
