#include "avatar_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../core/log.hpp"
#include "../vrm/expression.hpp"
#include "d3d_hooks.hpp"
#include "shader_compiler.hpp"

namespace rfv::win {

namespace {

const char* kShader = R"(
cbuffer Frame : register(b0) {
    float4x4 ViewProj;
    float4 LightDir;
    float4 LightColor;
    float4 Ambient;
    float4 NV0, NV1, NV2;   // rows: avatar-space normal -> view space
    float4 EnvUv;           // xy = screen uv of the character, zw = sampling radius in uv
    float4 EnvParams;       // x = scene lighting on, y = exposure, z = min, w = max brightness
    float4 EnvParams2;      // x = saturation, y = mip for directional light, z = mip for average
};
Texture2D SceneTex : register(t2);   // copy of the frame the game rendered (with mips)
SamplerState SceneSamp : register(s1);
cbuffer Draw : register(b1) {
    float4x4 World;
    float4 BaseColor;
    float4 ShadeColor;   // w = unlit
    float4 Emissive;     // w = alpha cutoff (< 0: none)
    uint BoneOffset;
    uint Skinned;
    uint HasTex;
    uint HasShadeTex;
};
// Bone matrices as 4 float4 columns per bone, 256 bones per texture row. A texture is the
// most portable choice (Wine/Proton's HLSL compiler lacks structured and typed buffers).
Texture2D<float4> Bones : register(t0);
Texture2D BaseTex : register(t0);
Texture2D ShadeTex : register(t1);
SamplerState Samp : register(s0);

struct VSIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv : TEXCOORD0;
    uint4 joints : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
};
struct PSIn {
    float4 pos : SV_Position;
    float3 nrm : NORMAL;
    float2 uv : TEXCOORD0;
};

float4 boneTexel(uint bone, uint k) {
    uint b = BoneOffset + bone;
    return Bones.Load(int3((b % 256) * 4 + k, b / 256, 0));
}

float4 boneColumn(uint4 j, float4 w, uint k) {
    return boneTexel(j.x, k) * w.x + boneTexel(j.y, k) * w.y + boneTexel(j.z, k) * w.z + boneTexel(j.w, k) * w.w;
}

PSIn vsMain(VSIn v) {
    float4 c0, c1, c2, c3;
    if (Skinned != 0) {
        c0 = boneColumn(v.joints, v.weights, 0);
        c1 = boneColumn(v.joints, v.weights, 1);
        c2 = boneColumn(v.joints, v.weights, 2);
        c3 = boneColumn(v.joints, v.weights, 3);
    } else {
        c0 = float4(World._11, World._21, World._31, World._41);
        c1 = float4(World._12, World._22, World._32, World._42);
        c2 = float4(World._13, World._23, World._33, World._43);
        c3 = float4(World._14, World._24, World._34, World._44);
    }
    PSIn o;
    float4 wp = c0 * v.pos.x + c1 * v.pos.y + c2 * v.pos.z + c3;
    o.pos = mul(ViewProj, wp);
    o.nrm = (c0 * v.nrm.x + c1 * v.nrm.y + c2 * v.nrm.z).xyz;
    o.uv = v.uv;
    return o;
}

static const float3 LUMA = float3(0.299, 0.587, 0.114);

// Light arriving from the direction a (view-space) normal faces, estimated from the
// game's own rendered image around the character: what is above him lights him from
// above, a red wall to his left tints his left side, a dark interior darkens him.
float3 sceneLight(float3 nAvatar) {
    float3 nv = normalize(float3(dot(NV0.xyz, nAvatar), dot(NV1.xyz, nAvatar), dot(NV2.xyz, nAvatar)));
    float2 uv = EnvUv.xy + float2(nv.x, -nv.y) * EnvUv.zw;
    float3 e = SceneTex.SampleLevel(SceneSamp, uv, EnvParams2.y).rgb;
    float3 avg = SceneTex.SampleLevel(SceneSamp, EnvUv.xy, EnvParams2.z).rgb;
    float le = dot(e, LUMA), la = dot(avg, LUMA);
    float3 hue = lerp(float3(1, 1, 1), e / max(le, 1e-3), EnvParams2.x);
    float bright = clamp(lerp(la, le, 0.5) * EnvParams.y, EnvParams.z, EnvParams.w);
    return hue * bright;
}

float4 psMain(PSIn i, bool front : SV_IsFrontFace) : SV_Target {
    float4 tex = HasTex != 0 ? BaseTex.Sample(Samp, i.uv) : float4(1, 1, 1, 1);
    float4 col = BaseColor * tex;
    if (Emissive.w >= 0) clip(col.a - Emissive.w);
    float3 n = normalize(i.nrm) * (front ? 1.0 : -1.0);
    bool scene = EnvParams.x > 0.5;
    float3 env = scene ? sceneLight(n) : float3(1, 1, 1);
    float3 rgb;
    if (ShadeColor.w > 0.5) {
        rgb = col.rgb * (scene ? env : float3(1, 1, 1));
    } else {
        float ndl = dot(n, normalize(LightDir.xyz));
        float lit = smoothstep(-0.1, 0.15, ndl);
        float3 shade = ShadeColor.rgb * (HasShadeTex != 0 ? ShadeTex.Sample(Samp, i.uv).rgb : tex.rgb);
        if (scene)
            rgb = lerp(shade, col.rgb, lit) * env * LightColor.rgb;
        else
            rgb = lerp(shade, col.rgb, lit) * LightColor.rgb + col.rgb * Ambient.rgb;
    }
    rgb += Emissive.rgb;
    return float4(saturate(rgb), col.a);
}
)";

struct Vertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    uint16_t joints[4];
    float weights[4];
};

struct FrameCB {
    float viewProj[16];
    float lightDir[4];
    float lightColor[4];
    float ambient[4];
    float nv[3][4];
    float envUv[4];
    float envParams[4];
    float envParams2[4];
};

struct DrawCB {
    float world[16];
    float baseColor[4];
    float shadeColor[4];
    float emissive[4];
    uint32_t boneOffset, skinned, hasTex, hasShadeTex;
};

template <typename T> void rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

ID3D11Buffer* makeBuffer(ID3D11Device* dev, const void* data, UINT size, UINT bind) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd{data, 0, 0};
    ID3D11Buffer* b = nullptr;
    dev->CreateBuffer(&bd, &sd, &b);
    return b;
}

ID3D11Buffer* makeDynamicCb(ID3D11Device* dev, UINT size) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* b = nullptr;
    dev->CreateBuffer(&bd, nullptr, &b);
    return b;
}

template <typename T> void updateCb(ID3D11DeviceContext* ctx, ID3D11Buffer* b, const T& data) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &data, sizeof(T));
        ctx->Unmap(b, 0);
    }
}

} // namespace

bool AvatarRenderer::init(ID3D11Device* device, std::string& error) {
    shutdown();
    device_ = device;
    device_->AddRef();

    ID3DBlob* vsb = compileShader(kShader, "vsMain", "vs_5_0", error);
    if (!vsb) return false;
    ID3DBlob* psb = compileShader(kShader, "psMain", "ps_5_0", error);
    if (!psb) { vsb->Release(); return false; }
    device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    D3D11_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, nrm), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, offsetof(Vertex, joints), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, weights), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device_->CreateInputLayout(elems, 5, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout_);
    vsb->Release();
    psb->Release();

    frameCb_ = makeDynamicCb(device_, sizeof(FrameCB));
    drawCb_ = makeDynamicCb(device_, sizeof(DrawCB));

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &sampler_);

    D3D11_SAMPLER_DESC ss{};
    ss.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ss.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&ss, &sceneSampler_);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    // glTF front faces are counter-clockwise; our avatar->game conversion keeps physical
    // handedness, so they stay counter-clockwise on screen.
    rd.FrontCounterClockwise = TRUE;
    rd.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&rd, &rsCull_);
    rd.CullMode = D3D11_CULL_NONE;
    device_->CreateRasterizerState(&rd, &rsNoCull_);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    device_->CreateBlendState(&bd, &bsOpaque_);
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    device_->CreateBlendState(&bd, &bsAlpha_);

    D3D11_COMPARISON_FUNC funcs[3] = {D3D11_COMPARISON_LESS, D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_GREATER_EQUAL};
    for (int i = 0; i < 3; ++i) {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthFunc = funcs[i];
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        device_->CreateDepthStencilState(&dd, &dsWrite_[i]);
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        device_->CreateDepthStencilState(&dd, &dsNoWrite_[i]);
    }

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
        device_->CreateShaderResourceView(t, nullptr, &whiteTex_);
        t->Release();
    }

    if (!vs_ || !ps_ || !layout_ || !frameCb_ || !drawCb_ || !sampler_ || !rsCull_ || !bsOpaque_ || !dsWrite_[0]) {
        error = "failed to create Direct3D resources";
        return false;
    }
    return true;
}

void AvatarRenderer::shutdown() {
    unload();
    releaseSizeDependent();
    rel(vs_); rel(ps_); rel(layout_); rel(frameCb_); rel(drawCb_); rel(sampler_); rel(sceneSampler_);
    rel(rsCull_); rel(rsNoCull_); rel(bsOpaque_); rel(bsAlpha_); rel(whiteTex_);
    for (int i = 0; i < 3; ++i) { rel(dsWrite_[i]); rel(dsNoWrite_[i]); }
    rel(device_);
}

void AvatarRenderer::releaseSizeDependent() {
    rel(sceneSrv_);
    rel(sceneTex_);
    sceneW_ = sceneH_ = 0;
    rel(depthDsv_);
    rel(depthTex_);
    depthW_ = depthH_ = 0;
}

void AvatarRenderer::unload() {
    for (auto& t : textures_) rel(t);
    textures_.clear();
    for (auto& mn : meshNodes_)
        for (auto& p : mn.prims) { rel(p.vb); rel(p.ib); }
    meshNodes_.clear();
    rel(boneSrv_);
    rel(boneTex_);
    totalBones_ = 0;
    uploaded_ = false;
    model_ = nullptr;
}

bool AvatarRenderer::upload(const LoadedAvatar& src, std::string& error) {
    unload();
    if (!device_ || !src.model) { error = "renderer not initialised"; return false; }
    const vrm::Model& m = *src.model;
    model_ = &m;

    // Textures (with full mip chains).
    textures_.assign(m.images.size(), nullptr);
    for (size_t i = 0; i < m.images.size() && i < src.images.size(); ++i) {
        const DecodedImage& img = src.images[i];
        if (img.rgba.empty()) continue;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = img.width;
        td.Height = img.height;
        td.MipLevels = 0;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) continue;
        ID3D11DeviceContext* ctx = nullptr;
        device_->GetImmediateContext(&ctx);
        ctx->UpdateSubresource(tex, 0, nullptr, img.rgba.data(), img.width * 4, 0);
        if (SUCCEEDED(device_->CreateShaderResourceView(tex, nullptr, &textures_[i]))) ctx->GenerateMips(textures_[i]);
        ctx->Release();
        tex->Release();
    }

    // Geometry.
    for (size_t ni = 0; ni < m.nodes.size(); ++ni) {
        const vrm::Node& node = m.nodes[ni];
        if (node.mesh < 0 || node.mesh >= static_cast<int>(m.meshes.size())) continue;
        GpuMeshNode mn;
        mn.node = static_cast<int>(ni);
        mn.skin = (node.skin >= 0 && node.skin < static_cast<int>(m.skins.size())) ? node.skin : -1;
        if (mn.skin >= 0) {
            mn.boneOffset = totalBones_;
            totalBones_ += static_cast<UINT>(m.skins[mn.skin].joints.size());
        }
        for (const vrm::Primitive& p : m.meshes[node.mesh].primitives) {
            if (p.positions.empty() || p.indices.empty()) continue;
            std::vector<Vertex> verts(p.positions.size());
            size_t jointLimit = mn.skin >= 0 ? m.skins[mn.skin].joints.size() : 0;
            for (size_t v = 0; v < verts.size(); ++v) {
                Vertex& o = verts[v];
                std::memcpy(o.pos, &p.positions[v], 12);
                Vec3 n = v < p.normals.size() ? p.normals[v] : Vec3{0, 1, 0};
                std::memcpy(o.nrm, &n, 12);
                o.uv[0] = p.uvs.size() > v * 2 + 1 ? p.uvs[v * 2] : 0.0f;
                o.uv[1] = p.uvs.size() > v * 2 + 1 ? p.uvs[v * 2 + 1] : 0.0f;
                for (int k = 0; k < 4; ++k) {
                    bool has = !p.joints.empty() && p.joints[v * 4 + k] < jointLimit;
                    o.joints[k] = has ? p.joints[v * 4 + k] : 0;
                    o.weights[k] = has ? p.weights[v * 4 + k] : (k == 0 && p.joints.empty() ? 1.0f : 0.0f);
                }
            }
            GpuPrimitive gp;
            gp.src = &p;
            gp.mesh = node.mesh;
            if (!p.targets.empty()) {
                // Morphable (face): dynamic vertex buffer, rewritten when expression weights change.
                gp.baseVerts.assign(reinterpret_cast<const uint8_t*>(verts.data()),
                                    reinterpret_cast<const uint8_t*>(verts.data() + verts.size()));
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(Vertex));
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                D3D11_SUBRESOURCE_DATA sd{verts.data(), 0, 0};
                device_->CreateBuffer(&bd, &sd, &gp.vb);
            } else {
                gp.vb = makeBuffer(device_, verts.data(), static_cast<UINT>(verts.size() * sizeof(Vertex)), D3D11_BIND_VERTEX_BUFFER);
            }
            gp.ib = makeBuffer(device_, p.indices.data(), static_cast<UINT>(p.indices.size() * 4), D3D11_BIND_INDEX_BUFFER);
            gp.indexCount = static_cast<UINT>(p.indices.size());
            gp.material = p.material;
            if (!gp.vb || !gp.ib) { rel(gp.vb); rel(gp.ib); continue; }
            mn.prims.push_back(gp);
        }
        // An unskinned mesh still needs a valid bone slot for skinned primitives without a skin.
        if (!mn.prims.empty()) meshNodes_.push_back(std::move(mn));
    }

    if (totalBones_ > 0) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 1024;  // 256 bones x 4 columns
        td.Height = (totalBones_ + 255) / 256;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &boneTex_)) ||
            FAILED(device_->CreateShaderResourceView(boneTex_, nullptr, &boneSrv_))) {
            error = "failed to create bone texture";
            unload();
            return false;
        }
    }
    if (meshNodes_.empty()) { error = "avatar has no drawable meshes"; unload(); return false; }
    uploaded_ = true;
    RFV_LOG("Uploaded avatar: %zu mesh nodes, %u bones, %zu textures", meshNodes_.size(), totalBones_, textures_.size());
    return true;
}

bool AvatarRenderer::ensureDepth(UINT w, UINT h) {
    if (depthDsv_ && depthW_ == w && depthH_ == h) return true;
    releaseSizeDependent();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &depthTex_))) return false;
    if (FAILED(device_->CreateDepthStencilView(depthTex_, nullptr, &depthDsv_))) { rel(depthTex_); return false; }
    depthW_ = w;
    depthH_ = h;
    return true;
}

ID3D11ShaderResourceView* AvatarRenderer::textureFor(int ti) const {
    if (!model_ || ti < 0 || ti >= static_cast<int>(model_->textures.size())) return nullptr;
    int img = model_->textures[ti].image;
    if (img < 0 || img >= static_cast<int>(textures_.size())) return nullptr;
    return textures_[img];
}

bool AvatarRenderer::captureScene(ID3D11DeviceContext* ctx, ID3D11Texture2D* back) {
    if (!device_ || !back || sceneUnsupported_) return false;
    D3D11_TEXTURE2D_DESC bd;
    back->GetDesc(&bd);
    if (bd.SampleDesc.Count != 1) { sceneUnsupported_ = true; return false; }
    if (!sceneTex_ || sceneW_ != bd.Width || sceneH_ != bd.Height || sceneFmt_ != bd.Format) {
        rel(sceneSrv_);
        rel(sceneTex_);
        // Typeless back buffers are viewed as UNORM; mip generation needs a filterable format.
        DXGI_FORMAT view = bd.Format;
        if (view == DXGI_FORMAT_R8G8B8A8_TYPELESS) view = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (view == DXGI_FORMAT_B8G8R8A8_TYPELESS) view = DXGI_FORMAT_B8G8R8A8_UNORM;
        else if (view == DXGI_FORMAT_R10G10B10A2_TYPELESS) view = DXGI_FORMAT_R10G10B10A2_UNORM;
        else if (view == DXGI_FORMAT_R16G16B16A16_TYPELESS) view = DXGI_FORMAT_R16G16B16A16_FLOAT;
        UINT support = 0;
        if (FAILED(device_->CheckFormatSupport(view, &support)) || !(support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) {
            RFV_LOG("Scene lighting unavailable: back-buffer format %d cannot generate mips", static_cast<int>(bd.Format));
            sceneUnsupported_ = true;
            return false;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = bd.Width;
        td.Height = bd.Height;
        td.MipLevels = 0;
        td.ArraySize = 1;
        td.Format = bd.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = view;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = static_cast<UINT>(-1);
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &sceneTex_)) || FAILED(device_->CreateShaderResourceView(sceneTex_, &sv, &sceneSrv_))) {
            rel(sceneTex_);
            sceneUnsupported_ = true;
            RFV_LOG("Scene lighting unavailable: cannot create the scene texture");
            return false;
        }
        sceneW_ = bd.Width;
        sceneH_ = bd.Height;
        sceneFmt_ = bd.Format;
    }
    ctx->CopySubresourceRegion(sceneTex_, 0, 0, 0, 0, back, 0, nullptr);
    ctx->GenerateMips(sceneSrv_);
    return true;
}

void AvatarRenderer::updateMorphs(ID3D11DeviceContext* ctx, GpuPrimitive& gp, const std::vector<float>& weights) {
    bool changed = weights.size() != gp.lastWeights.size();
    for (size_t i = 0; !changed && i < weights.size(); ++i) changed = std::fabs(weights[i] - gp.lastWeights[i]) > 1e-4f;
    if (!changed) return;
    gp.lastWeights = weights;
    vrm::applyMorphs(*gp.src, weights, morphPos_, morphNrm_);
    morphVerts_ = gp.baseVerts;
    auto* v = reinterpret_cast<Vertex*>(morphVerts_.data());
    size_t n = morphVerts_.size() / sizeof(Vertex);
    for (size_t i = 0; i < n && i < morphPos_.size(); ++i) {
        std::memcpy(v[i].pos, &morphPos_[i], 12);
        if (i < morphNrm_.size()) std::memcpy(v[i].nrm, &morphNrm_[i], 12);
    }
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ctx->Map(gp.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, morphVerts_.data(), morphVerts_.size());
        ctx->Unmap(gp.vb, 0);
    }
}

void AvatarRenderer::draw(ID3D11DeviceContext* ctx, const vrm::Avatar& avatar, const AvatarDrawParams& p,
                          ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* gameDepth) {
    if (!uploaded_ || &avatar.model() != model_) return;

    ID3D11DepthStencilView* dsv = nullptr;
    int depthMode = 0;
    if (p.depth != AvatarDrawParams::Depth::Own && gameDepth) {
        dsv = gameDepth;
        depthMode = p.depth == AvatarDrawParams::Depth::GameReversed ? 2 : 1;
    } else {
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
            D3D11_TEXTURE2D_DESC td;
            tex->GetDesc(&td);
            if (ensureDepth(td.Width, td.Height)) dsv = depthDsv_;
            tex->Release();
        }
        if (res) res->Release();
        if (dsv) ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }

    // Bone palettes for every skinned mesh node.
    if (boneTex_) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(boneTex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            auto* base = static_cast<uint8_t*>(map.pData);
            for (const auto& mn : meshNodes_) {
                if (mn.skin < 0) continue;
                avatar.skinPalette(mn.skin, palette_);
                for (size_t i = 0; i < palette_.size(); ++i) {
                    UINT b = mn.boneOffset + static_cast<UINT>(i);
                    std::memcpy(base + (b / 256) * map.RowPitch + (b % 256) * sizeof(Mat4), palette_[i].m, sizeof(Mat4));
                }
            }
            ctx->Unmap(boneTex_, 0);
        }
    }

    FrameCB fcb{};
    Mat4 vp = p.viewProj * p.avatarToWorld;
    std::memcpy(fcb.viewProj, vp.m, sizeof(fcb.viewProj));
    Vec3 l = normalize(p.lightDir);
    fcb.lightDir[0] = l.x; fcb.lightDir[1] = l.y; fcb.lightDir[2] = l.z;
    for (int i = 0; i < 3; ++i) { fcb.lightColor[i] = p.lightColor[i]; fcb.ambient[i] = p.ambient[i]; }
    bool scene = p.sceneLighting && sceneSrv_;
    if (scene) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) fcb.nv[r][c] = p.normalToView[r][c];
        float rad = std::max(p.sceneRadiusPx, 8.0f);
        fcb.envUv[0] = p.sceneCenterUv[0];
        fcb.envUv[1] = p.sceneCenterUv[1];
        fcb.envUv[2] = rad / sceneW_;
        fcb.envUv[3] = rad / sceneH_;
        fcb.envParams[0] = 1;
        fcb.envParams[1] = p.exposure;
        fcb.envParams[2] = p.minBright;
        fcb.envParams[3] = p.maxBright;
        fcb.envParams2[0] = p.saturation;
        fcb.envParams2[1] = std::log2(std::max(rad * 0.5f, 1.0f));  // a few blurry samples across the area
        fcb.envParams2[2] = std::log2(std::max(rad * 2.0f, 1.0f));  // one average over the whole area
    }
    updateCb(ctx, frameCb_, fcb);

    draws::setInternal(true);
    ctx->OMSetRenderTargets(1, &rtv, dsv);
    ctx->RSSetViewports(1, &p.viewport);
    ctx->IASetInputLayout(layout_);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_, nullptr, 0);
    ctx->PSSetShader(ps_, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer* cbs[2] = {frameCb_, drawCb_};
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetConstantBuffers(0, 2, cbs);
    ctx->VSSetShaderResources(0, 1, &boneSrv_);
    ctx->PSSetSamplers(0, 1, &sampler_);
    ID3D11SamplerState* sceneSamp = sceneSampler_;
    ctx->PSSetSamplers(1, 1, &sceneSamp);
    ID3D11ShaderResourceView* sceneView = scene ? sceneSrv_ : nullptr;
    ctx->PSSetShaderResources(2, 1, &sceneView);

    struct Item { const GpuMeshNode* node; GpuPrimitive* prim; int queue; bool blend; };
    std::vector<Item> items;
    const auto& morphW = avatar.morphWeights();
    for (auto& mn : meshNodes_)
        for (auto& gp : mn.prims) {
            if (!gp.baseVerts.empty() && gp.mesh >= 0 && gp.mesh < static_cast<int>(morphW.size())) updateMorphs(ctx, gp, morphW[gp.mesh]);
            const vrm::Material* mat = (gp.material >= 0 && gp.material < static_cast<int>(model_->materials.size())) ? &model_->materials[gp.material] : nullptr;
            bool blend = mat && mat->alphaMode == vrm::AlphaMode::Blend;
            items.push_back({&mn, &gp, mat ? mat->renderQueueOffset : 0, blend});
        }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.blend != b.blend) return !a.blend;
        return a.queue < b.queue;
    });

    static const vrm::Material kDefaultMat;
    float blendFactor[4] = {0, 0, 0, 0};
    for (const Item& it : items) {
        const vrm::Material& mat = it.prim->material >= 0 && it.prim->material < static_cast<int>(model_->materials.size())
                                       ? model_->materials[it.prim->material] : kDefaultMat;
        DrawCB dcb{};
        Mat4 world = avatar.world()[it.node->node];
        std::memcpy(dcb.world, world.m, sizeof(dcb.world));
        std::memcpy(dcb.baseColor, mat.baseColor, sizeof(dcb.baseColor));
        std::memcpy(dcb.shadeColor, mat.shadeColor, 12);
        dcb.shadeColor[3] = mat.unlit ? 1.0f : 0.0f;
        if (!mat.isMToon && !mat.unlit) {
            // Plain PBR material: approximate with a darker shade of the base colour.
            for (int k = 0; k < 3; ++k) dcb.shadeColor[k] = 0.6f;
        }
        std::memcpy(dcb.emissive, mat.emissive, 12);
        dcb.emissive[3] = mat.alphaMode == vrm::AlphaMode::Mask ? mat.alphaCutoff : (mat.alphaMode == vrm::AlphaMode::Blend ? 0.002f : -1.0f);
        dcb.boneOffset = it.node->boneOffset;
        dcb.skinned = it.node->skin >= 0 ? 1u : 0u;
        ID3D11ShaderResourceView* srvs[2] = {textureFor(mat.baseColorTexture), textureFor(mat.shadeTexture)};
        dcb.hasTex = srvs[0] ? 1u : 0u;
        dcb.hasShadeTex = srvs[1] ? 1u : 0u;
        if (!srvs[0]) srvs[0] = whiteTex_;
        if (!srvs[1]) srvs[1] = whiteTex_;
        updateCb(ctx, drawCb_, dcb);
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->RSSetState(mat.doubleSided ? rsNoCull_ : rsCull_);
        ctx->OMSetBlendState(it.blend ? bsAlpha_ : bsOpaque_, blendFactor, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(it.blend ? dsNoWrite_[depthMode] : dsWrite_[depthMode], 0);
        UINT stride = sizeof(Vertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &it.prim->vb, &stride, &offset);
        ctx->IASetIndexBuffer(it.prim->ib, DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(it.prim->indexCount, 0, 0);
    }
    draws::setInternal(false);
}

} // namespace rfv::win
