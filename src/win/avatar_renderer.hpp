// GPU skinning + MToon-style toon shading of a VRM avatar with Direct3D 11.
#pragma once
#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/math.hpp"
#include "../vrm/avatar.hpp"
#include "../vrm/expression.hpp"
#include "../vrm/spring.hpp"
#include "image_decode.hpp"

namespace rfv::win {

// Everything produced on the loader thread for one avatar.
struct LoadedAvatar {
    std::string path;
    std::unique_ptr<vrm::Model> model;
    std::unique_ptr<vrm::Avatar> avatar;
    std::unique_ptr<vrm::SpringSystem> springs;            // hair / skirt physics
    std::unique_ptr<vrm::ExpressionController> expressions; // face
    std::vector<DecodedImage> images;  // parallel to model->images (empty entries on failure)
    std::vector<std::string> warnings;
};

struct AvatarDrawParams {
    Mat4 viewProj;                       // game view-projection
    Mat4 avatarToWorld;                  // avatar space -> the space viewProj expects
    Vec3 lightDir{0.3f, 0.8f, 0.5f};     // avatar space, towards the light
    float lightColor[3] = {1.0f, 0.95f, 0.9f};
    float ambient[3] = {0.25f, 0.22f, 0.22f};
    D3D11_VIEWPORT viewport{};
    enum class Depth { Own, GameStandard, GameReversed } depth = Depth::Own;

    // Scene-adaptive lighting (see captureScene). Disabled for the menu preview.
    bool sceneLighting = false;
    float normalToView[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // avatar normal -> view space
    float sceneCenterUv[2] = {0.5f, 0.5f};  // where the character is on screen
    float sceneRadiusPx = 200;              // how far around him to look for light
    float exposure = 2.0f, minBright = 0.22f, maxBright = 1.25f, saturation = 0.45f;
};

class AvatarRenderer {
public:
    ~AvatarRenderer() { shutdown(); }
    bool init(ID3D11Device* device, std::string& error);
    void shutdown();

    // Creates GPU resources for `src` (render thread). Replaces any previous avatar.
    bool upload(const LoadedAvatar& src, std::string& error);
    void unload();
    bool hasAvatar() const { return uploaded_; }

    // Draws the avatar in its current pose into `rtv`. `gameDepth` is used for
    // Depth::Game* modes; otherwise an internal depth buffer sized to `rtv` is used.
    void draw(ID3D11DeviceContext* ctx, const vrm::Avatar& avatar, const AvatarDrawParams& p,
              ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* gameDepth);

    // Copies the frame the game just rendered (before the avatar and our UI are drawn) and
    // builds its mip chain, so the shader can estimate the light around the character.
    bool captureScene(ID3D11DeviceContext* ctx, ID3D11Texture2D* backBuffer);

    void releaseSizeDependent();

private:
    struct GpuPrimitive {
        ID3D11Buffer* vb = nullptr;
        ID3D11Buffer* ib = nullptr;
        UINT indexCount = 0;
        int material = -1;
        // Face morphs: primitives with morph targets keep a CPU copy and a dynamic buffer.
        const vrm::Primitive* src = nullptr;
        int mesh = -1;
        std::vector<uint8_t> baseVerts;
        std::vector<float> lastWeights;
    };
    struct GpuMeshNode {
        int node = -1;
        int skin = -1;
        UINT boneOffset = 0;
        std::vector<GpuPrimitive> prims;
    };

    ID3D11Device* device_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    ID3D11Buffer* frameCb_ = nullptr;
    ID3D11Buffer* drawCb_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11RasterizerState* rsCull_ = nullptr;
    ID3D11RasterizerState* rsNoCull_ = nullptr;
    ID3D11BlendState* bsOpaque_ = nullptr;
    ID3D11BlendState* bsAlpha_ = nullptr;
    ID3D11DepthStencilState* dsWrite_[3] = {};    // per depth mode
    ID3D11DepthStencilState* dsNoWrite_[3] = {};
    ID3D11ShaderResourceView* whiteTex_ = nullptr;

    ID3D11Texture2D* sceneTex_ = nullptr;
    ID3D11ShaderResourceView* sceneSrv_ = nullptr;
    ID3D11SamplerState* sceneSampler_ = nullptr;
    UINT sceneW_ = 0, sceneH_ = 0;
    DXGI_FORMAT sceneFmt_ = DXGI_FORMAT_UNKNOWN;
    bool sceneUnsupported_ = false;
    std::vector<Vec3> morphPos_, morphNrm_;
    std::vector<uint8_t> morphVerts_;
    void updateMorphs(ID3D11DeviceContext* ctx, GpuPrimitive& gp, const std::vector<float>& weights);

    ID3D11Texture2D* depthTex_ = nullptr;
    ID3D11DepthStencilView* depthDsv_ = nullptr;
    UINT depthW_ = 0, depthH_ = 0;

    bool uploaded_ = false;
    const vrm::Model* model_ = nullptr;
    std::vector<ID3D11ShaderResourceView*> textures_;  // per glTF image
    std::vector<GpuMeshNode> meshNodes_;
    UINT totalBones_ = 0;
    ID3D11Texture2D* boneTex_ = nullptr;  // bone matrices (see shader)
    ID3D11ShaderResourceView* boneSrv_ = nullptr;
    std::vector<Mat4> palette_;
    std::vector<Mat4> boneScratch_;

    bool ensureDepth(UINT w, UINT h);
    ID3D11ShaderResourceView* textureFor(int textureIndex) const;
};

} // namespace rfv::win
