#include "image_decode.hpp"

#include <windows.h>
#include <wincodec.h>

#include <cstdio>

namespace rfv::win {

namespace {
template <typename T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
};
}

bool decodeImage(const std::vector<uint8_t>& encoded, DecodedImage& out, std::string& error) {
    char msg[96];
    if (encoded.empty()) { error = "empty image"; return false; }
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { std::snprintf(msg, sizeof(msg), "WIC unavailable (0x%08lx)", (unsigned long)hr); error = msg; return false; }
    ComPtr<IWICStream> stream;
    if (FAILED(hr = factory->CreateStream(&stream)) ||
        FAILED(hr = stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()), static_cast<DWORD>(encoded.size())))) {
        error = "cannot create image stream";
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(hr = factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
        std::snprintf(msg, sizeof(msg), "unsupported image format (0x%08lx)", (unsigned long)hr);
        error = msg;
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(hr = decoder->GetFrame(0, &frame))) { error = "image has no frames"; return false; }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(hr = factory->CreateFormatConverter(&conv)) ||
        FAILED(hr = conv->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        error = "cannot convert image to RGBA";
        return false;
    }
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) { error = "bad image size"; return false; }
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    if (FAILED(hr = conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()))) {
        error = "cannot read image pixels";
        return false;
    }
    return true;
}

} // namespace rfv::win
