// Saves and restores the D3D11 pipeline state we touch while drawing on top of the game.
#pragma once
#include <d3d11.h>

namespace rfv::win {

struct D3D11StateBackup {
    static constexpr int kSlots = 4;
    UINT scissorCount = 0, viewportCount = 0;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0;
    UINT stencilRef = 0;
    ID3D11DepthStencilState* dss = nullptr;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* psSrv[kSlots] = {};
    ID3D11SamplerState* psSampler[kSlots] = {};
    ID3D11ShaderResourceView* vsSrv[kSlots] = {};
    ID3D11PixelShader* ps = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11ClassInstance* psInst[256] = {};
    ID3D11ClassInstance* vsInst[256] = {};
    ID3D11ClassInstance* gsInst[256] = {};
    UINT psInstCount = 256, vsInstCount = 256, gsInstCount = 256;
    ID3D11Buffer* vsCb[kSlots] = {};
    ID3D11Buffer* psCb[kSlots] = {};
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    ID3D11Buffer* vb = nullptr;
    UINT vbStride = 0, vbOffset = 0;
    ID3D11InputLayout* layout = nullptr;

    void save(ID3D11DeviceContext* c) {
        scissorCount = viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        c->RSGetScissorRects(&scissorCount, scissors);
        c->RSGetViewports(&viewportCount, viewports);
        c->RSGetState(&rs);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&dss, &stencilRef);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        c->PSGetShaderResources(0, kSlots, psSrv);
        c->PSGetSamplers(0, kSlots, psSampler);
        c->VSGetShaderResources(0, kSlots, vsSrv);
        psInstCount = vsInstCount = gsInstCount = 256;
        c->PSGetShader(&ps, psInst, &psInstCount);
        c->VSGetShader(&vs, vsInst, &vsInstCount);
        c->GSGetShader(&gs, gsInst, &gsInstCount);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->VSGetConstantBuffers(0, kSlots, vsCb);
        c->PSGetConstantBuffers(0, kSlots, psCb);
        c->IAGetPrimitiveTopology(&topology);
        c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
        c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
        c->IAGetInputLayout(&layout);
    }

    template <typename T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

    void restore(ID3D11DeviceContext* c) {
        c->RSSetScissorRects(scissorCount, scissors);
        c->RSSetViewports(viewportCount, viewports);
        c->RSSetState(rs); rel(rs);
        c->OMSetBlendState(blend, blendFactor, sampleMask); rel(blend);
        c->OMSetDepthStencilState(dss, stencilRef); rel(dss);
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        for (auto& r : rtvs) rel(r);
        rel(dsv);
        c->PSSetShaderResources(0, kSlots, psSrv);
        for (auto& s : psSrv) rel(s);
        c->PSSetSamplers(0, kSlots, psSampler);
        for (auto& s : psSampler) rel(s);
        c->VSSetShaderResources(0, kSlots, vsSrv);
        for (auto& s : vsSrv) rel(s);
        c->PSSetShader(ps, psInst, psInstCount); rel(ps);
        for (UINT i = 0; i < psInstCount; ++i) rel(psInst[i]);
        c->VSSetShader(vs, vsInst, vsInstCount); rel(vs);
        for (UINT i = 0; i < vsInstCount; ++i) rel(vsInst[i]);
        c->GSSetShader(gs, gsInst, gsInstCount); rel(gs);
        for (UINT i = 0; i < gsInstCount; ++i) rel(gsInst[i]);
        c->HSSetShader(hs, nullptr, 0); rel(hs);
        c->DSSetShader(ds, nullptr, 0); rel(ds);
        c->VSSetConstantBuffers(0, kSlots, vsCb);
        for (auto& b : vsCb) rel(b);
        c->PSSetConstantBuffers(0, kSlots, psCb);
        for (auto& b : psCb) rel(b);
        c->IASetPrimitiveTopology(topology);
        c->IASetIndexBuffer(ib, ibFormat, ibOffset); rel(ib);
        c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset); rel(vb);
        c->IASetInputLayout(layout); rel(layout);
    }
};

} // namespace rfv::win
