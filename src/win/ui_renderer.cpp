#include "ui_renderer.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "d3d_hooks.hpp"
#include "paths.hpp"
#include "shader_compiler.hpp"

namespace rfv::win {

namespace {

const char* kUiShader = R"(
cbuffer Screen : register(b0) { float4 Size; };
Texture2D Tex : register(t0);
SamplerState Samp : register(s0);
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };
PSIn vsMain(VSIn v) {
    PSIn o;
    o.pos = float4(v.pos.x * Size.z - 1.0, 1.0 - v.pos.y * Size.w, 0.0, 1.0);
    o.uv = v.uv;
    o.col = v.col;
    return o;
}
float4 psMain(PSIn i) : SV_Target {
    float4 t = Tex.Sample(Samp, i.uv);
    return float4(i.col.rgb * t.rgb, i.col.a * t.a);
}
)";

template <typename T> void rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

constexpr size_t kCacheLimit = 256;

} // namespace

bool UiRenderer::init(ID3D11Device* device, std::string& error) {
    shutdown();
    device_ = device;
    device_->AddRef();
    ID3DBlob* vsb = compileShader(kUiShader, "vsMain", "vs_4_0", error);
    if (!vsb) return false;
    ID3DBlob* psb = compileShader(kUiShader, "psMain", "ps_4_0", error);
    if (!psb) { vsb->Release(); return false; }
    device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device_->CreateInputLayout(el, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout_);
    vsb->Release();
    psb->Release();

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(QuadVertex) * 4;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&bd, nullptr, &vb_);
    bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device_->CreateBuffer(&bd, nullptr, &cb_);

    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&bl, &blend_);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    device_->CreateRasterizerState(&rd, &rs_);
    D3D11_DEPTH_STENCIL_DESC dd{};
    device_->CreateDepthStencilState(&dd, &ds_);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&sd, &sampler_);

    uint32_t white = 0xFFFFFFFF;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{&white, 4, 0};
    ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(device_->CreateTexture2D(&td, &init, &t))) {
        device_->CreateShaderResourceView(t, nullptr, &white_);
        t->Release();
    }
    if (!vs_ || !ps_ || !layout_ || !vb_ || !cb_ || !blend_ || !rs_ || !ds_ || !sampler_ || !white_) {
        error = "failed to create UI resources";
        return false;
    }
    return true;
}

void UiRenderer::shutdown() {
    for (auto& kv : cache_) rel(kv.second.srv);
    cache_.clear();
    lruList_.clear();
    rel(vs_); rel(ps_); rel(layout_); rel(vb_); rel(cb_); rel(blend_); rel(rs_); rel(ds_); rel(sampler_); rel(white_);
    rel(device_);
}

void UiRenderer::setFonts(const std::wstring& menu, const std::wstring& hint, const std::wstring& body) {
    faces_[0] = menu;
    faces_[1] = hint;
    faces_[2] = body;
    for (auto& kv : cache_) rel(kv.second.srv);
    cache_.clear();
    lruList_.clear();
}

void UiRenderer::begin(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, float w, float h) {
    ctx_ = ctx;
    w_ = w;
    h_ = h;
    float size[4] = {w, h, 2.0f / w, 2.0f / h};
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, size, sizeof(size));
        ctx->Unmap(cb_, 0);
    }
    D3D11_VIEWPORT vp{0, 0, w, h, 0, 1};
    draws::setInternal(true);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs_);
    float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(blend_, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(ds_, 0);
    ctx->IASetInputLayout(layout_);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    UINT stride = sizeof(QuadVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb_, &stride, &offset);
    ctx->VSSetShader(vs_, nullptr, 0);
    ctx->PSSetShader(ps_, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cb_);
    ctx->PSSetSamplers(0, 1, &sampler_);
}

void UiRenderer::end() {
    draws::setInternal(false);
    ctx_ = nullptr;
}

void UiRenderer::quad(const float* xs, const float* ys, const float* us, const float* vs, Color c, ID3D11ShaderResourceView* srv) {
    if (!ctx_) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    auto* v = static_cast<QuadVertex*>(m.pData);
    for (int i = 0; i < 4; ++i) v[i] = {xs[i], ys[i], us[i], vs[i], c.r, c.g, c.b, c.a};
    ctx_->Unmap(vb_, 0);
    ctx_->PSSetShaderResources(0, 1, &srv);
    ctx_->Draw(4, 0);
}

void UiRenderer::rect(float x, float y, float w, float h, Color c) {
    float xs[4] = {x, x + w, x, x + w}, ys[4] = {y, y, y + h, y + h};
    float us[4] = {0, 1, 0, 1}, vs[4] = {0, 0, 1, 1};
    quad(xs, ys, us, vs, c, white_);
}

void UiRenderer::frame(float x, float y, float w, float h, float t, Color c) {
    rect(x, y, w, t, c);
    rect(x, y + h - t, w, t, c);
    rect(x, y + t, t, h - 2 * t, c);
    rect(x + w - t, y + t, t, h - 2 * t, c);
}

void UiRenderer::slant(float x, float y, float w, float h, float skew, Color c) {
    float xs[4] = {x + skew, x + w + skew, x, x + w}, ys[4] = {y, y, y + h, y + h};
    float us[4] = {0, 1, 0, 1}, vs[4] = {0, 0, 1, 1};
    quad(xs, ys, us, vs, c, white_);
}

UiRenderer::Glyphs* UiRenderer::glyphs(const std::string& utf8, int px, FontStyle style) {
    if (utf8.empty() || px < 4) return nullptr;
    std::string key = std::to_string(static_cast<int>(style)) + ":" + std::to_string(px) + ":" + utf8;
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        lruList_.splice(lruList_.begin(), lruList_, it->second.lru);
        return &it->second;
    }
    if (cache_.size() >= kCacheLimit) {
        auto victim = cache_.find(lruList_.back());
        if (victim != cache_.end()) { rel(victim->second.srv); cache_.erase(victim); }
        lruList_.pop_back();
    }

    std::wstring text = widen(utf8);
    // Display fonts like Arial Black have no CJK glyphs; Segoe UI has system font linking
    // (Yu Gothic / Meiryo / Malgun ...), so avatar names in any script render.
    bool ascii = true;
    for (unsigned char c : utf8)
        if (c >= 0x80) { ascii = false; break; }
    const wchar_t* face = ascii ? faces_[static_cast<int>(style)].c_str() : L"Segoe UI";
    int weight = style == FontStyle::Menu ? FW_BLACK : (style == FontStyle::Hint ? FW_BOLD : FW_SEMIBOLD);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    int pad = 2;
    int w = sz.cx + pad * 2, h = sz.cy + pad * 2;
    Glyphs g;
    if (w > 0 && h > 0 && w < 8192 && h < 1024) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && bits) {
            HGDIOBJ oldBmp = SelectObject(dc, bmp);
            std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, TRANSPARENT);
            TextOutW(dc, pad, pad, text.c_str(), static_cast<int>(text.size()));
            GdiFlush();
            std::vector<uint32_t> rgba(static_cast<size_t>(w) * h);
            auto* src = static_cast<const uint8_t*>(bits);
            for (size_t i = 0; i < rgba.size(); ++i) {
                uint8_t a = std::max(src[i * 4], std::max(src[i * 4 + 1], src[i * 4 + 2]));
                rgba[i] = 0x00FFFFFFu | (static_cast<uint32_t>(a) << 24);
            }
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{rgba.data(), static_cast<UINT>(w * 4), 0};
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(device_->CreateTexture2D(&td, &init, &tex))) {
                device_->CreateShaderResourceView(tex, nullptr, &g.srv);
                tex->Release();
            }
            SelectObject(dc, oldBmp);
        }
        if (bmp) DeleteObject(bmp);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    if (!g.srv) return nullptr;
    g.w = w;
    g.h = h;
    g.advance = static_cast<float>(sz.cx);
    lruList_.push_front(key);
    g.lru = lruList_.begin();
    return &(cache_[key] = g);
}

float UiRenderer::text(const std::string& utf8, float x, float y, float px, FontStyle style, Color c) {
    Glyphs* g = glyphs(utf8, static_cast<int>(std::lround(px)), style);
    if (!g) return 0;
    float x0 = std::floor(x) - 2, y0 = std::floor(y) - 2;
    float xs[4] = {x0, x0 + g->w, x0, x0 + g->w}, ys[4] = {y0, y0, y0 + g->h, y0 + g->h};
    float us[4] = {0, 1, 0, 1}, vs[4] = {0, 0, 1, 1};
    quad(xs, ys, us, vs, c, g->srv);
    return g->advance;
}

float UiRenderer::measure(const std::string& utf8, float px, FontStyle style) {
    Glyphs* g = glyphs(utf8, static_cast<int>(std::lround(px)), style);
    return g ? g->advance : 0;
}

} // namespace rfv::win
