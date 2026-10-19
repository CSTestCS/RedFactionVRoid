// Minimal immediate-mode 2D drawing on top of the game: solid rectangles, outlines and
// text. Text is rasterised with GDI (full Unicode, so Japanese avatar names render) and
// cached as textures.
#pragma once
#include <d3d11.h>

#include <list>
#include <string>
#include <unordered_map>

namespace rfv::win {

struct Color {
    float r, g, b, a;
    static constexpr Color rgb(int R, int G, int B, float A = 1.0f) { return {R / 255.0f, G / 255.0f, B / 255.0f, A}; }
};

enum class FontStyle { Menu, Hint, Body };

class UiRenderer {
public:
    ~UiRenderer() { shutdown(); }
    bool init(ID3D11Device* device, std::string& error);
    void shutdown();
    void setFonts(const std::wstring& menuFace, const std::wstring& hintFace, const std::wstring& bodyFace);

    // Begin/end a batch targeting `rtv` of size w x h. Draw calls in between.
    void begin(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, float w, float h);
    void end();

    void rect(float x, float y, float w, float h, Color c);
    void frame(float x, float y, float w, float h, float thickness, Color c);
    // Parallelogram slanted like the game's highlight stripes.
    void slant(float x, float y, float w, float h, float skew, Color c);
    // Draws UTF-8 text; returns its width in pixels. (x, y) is the top-left corner.
    float text(const std::string& utf8, float x, float y, float pixelHeight, FontStyle style, Color c);
    float measure(const std::string& utf8, float pixelHeight, FontStyle style);

    float width() const { return w_; }
    float height() const { return h_; }

private:
    struct Glyphs {
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;          // texture size
        float advance = 0;         // text width in pixels
        std::list<std::string>::iterator lru;
    };
    struct QuadVertex { float x, y, u, v; float r, g, b, a; };

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    ID3D11Buffer* vb_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11RasterizerState* rs_ = nullptr;
    ID3D11DepthStencilState* ds_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11ShaderResourceView* white_ = nullptr;
    float w_ = 0, h_ = 0;
    std::wstring faces_[3] = {L"Arial Black", L"Impact", L"Segoe UI"};

    std::unordered_map<std::string, Glyphs> cache_;
    std::list<std::string> lruList_;

    void quad(const float* xs, const float* ys, const float* us, const float* vs, Color c, ID3D11ShaderResourceView* srv);
    Glyphs* glyphs(const std::string& utf8, int pixelHeight, FontStyle style);
};

} // namespace rfv::win
