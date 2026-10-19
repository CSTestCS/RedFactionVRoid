// glTF 2.0 binary (.glb / .vrm) loader with VRM 0.x and VRM 1.0 extension support.
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "../core/json.hpp"
#include "model.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace rfv::vrm {
namespace {

using json::Value;

struct BinCtx {
    const Value& doc;
    const uint8_t* bin;
    size_t binSize;
};

uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

int componentCount(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    throw std::runtime_error("unknown accessor type " + type);
}

int componentSize(int ct) {
    switch (ct) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: throw std::runtime_error("unknown component type");
    }
}

double readComponent(const uint8_t* p, int ct, bool normalized) {
    switch (ct) {
    case 5120: { int8_t v; std::memcpy(&v, p, 1); return normalized ? std::max(v / 127.0, -1.0) : v; }
    case 5121: { uint8_t v = *p; return normalized ? v / 255.0 : v; }
    case 5122: { int16_t v; std::memcpy(&v, p, 2); return normalized ? std::max(v / 32767.0, -1.0) : v; }
    case 5123: { uint16_t v; std::memcpy(&v, p, 2); return normalized ? v / 65535.0 : v; }
    case 5125: { uint32_t v; std::memcpy(&v, p, 4); return v; }
    case 5126: { float v; std::memcpy(&v, p, 4); return v; }
    }
    return 0;
}

// Returns a view of a bufferView's bytes inside the GLB BIN chunk.
const uint8_t* bufferViewData(const BinCtx& c, int viewIndex, size_t& length, size_t& stride) {
    const Value& bv = c.doc["bufferViews"][viewIndex];
    if (!bv.isObject()) throw std::runtime_error("bad bufferView index");
    if (bv["buffer"].integer(0) != 0) throw std::runtime_error("external buffers are not supported (only .glb/.vrm)");
    size_t offset = static_cast<size_t>(bv["byteOffset"].num(0));
    length = static_cast<size_t>(bv["byteLength"].num(0));
    stride = static_cast<size_t>(bv["byteStride"].num(0));
    if (offset + length > c.binSize) throw std::runtime_error("bufferView out of range");
    return c.bin + offset;
}

// Reads an accessor as doubles, `comps` values per element.
std::vector<double> readAccessor(const BinCtx& c, int accessorIndex, int& comps) {
    const Value& acc = c.doc["accessors"][accessorIndex];
    if (!acc.isObject()) throw std::runtime_error("bad accessor index");
    comps = componentCount(acc["type"].str());
    int ct = acc["componentType"].integer();
    bool normalized = acc["normalized"].boolean(false);
    size_t count = static_cast<size_t>(acc["count"].num(0));
    int csize = componentSize(ct);
    std::vector<double> out(count * comps, 0.0);

    if (acc.has("bufferView")) {
        size_t len = 0, stride = 0;
        const uint8_t* base = bufferViewData(c, acc["bufferView"].integer(), len, stride);
        size_t accOffset = static_cast<size_t>(acc["byteOffset"].num(0));
        size_t elemSize = static_cast<size_t>(csize) * comps;
        if (stride == 0) stride = elemSize;
        if (count > 0 && accOffset + (count - 1) * stride + elemSize > len) throw std::runtime_error("accessor out of range");
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* e = base + accOffset + i * stride;
            for (int k = 0; k < comps; ++k) out[i * comps + k] = readComponent(e + k * csize, ct, normalized);
        }
    }

    // Sparse accessors (used by some exporters for morph targets; rare for base data).
    const Value& sparse = acc["sparse"];
    if (sparse.isObject()) {
        size_t scount = static_cast<size_t>(sparse["count"].num(0));
        const Value& si = sparse["indices"];
        const Value& sv = sparse["values"];
        size_t ilen, istride, vlen, vstride;
        const uint8_t* ib = bufferViewData(c, si["bufferView"].integer(), ilen, istride);
        const uint8_t* vb = bufferViewData(c, sv["bufferView"].integer(), vlen, vstride);
        ib += static_cast<size_t>(si["byteOffset"].num(0));
        vb += static_cast<size_t>(sv["byteOffset"].num(0));
        int ict = si["componentType"].integer();
        int isz = componentSize(ict);
        for (size_t i = 0; i < scount; ++i) {
            size_t idx = static_cast<size_t>(readComponent(ib + i * isz, ict, false));
            if (idx >= count) continue;
            for (int k = 0; k < comps; ++k)
                out[idx * comps + k] = readComponent(vb + (i * comps + k) * csize, ct, normalized);
        }
    }
    return out;
}

void computeNormals(Primitive& p) {
    p.normals.assign(p.positions.size(), Vec3{0, 0, 0});
    for (size_t i = 0; i + 2 < p.indices.size(); i += 3) {
        uint32_t a = p.indices[i], b = p.indices[i + 1], c = p.indices[i + 2];
        if (a >= p.positions.size() || b >= p.positions.size() || c >= p.positions.size()) continue;
        Vec3 n = cross(p.positions[b] - p.positions[a], p.positions[c] - p.positions[a]);
        p.normals[a] += n; p.normals[b] += n; p.normals[c] += n;
    }
    for (auto& n : p.normals) n = normalize(n);
}

Primitive loadPrimitive(const BinCtx& c, const Value& jp) {
    Primitive p;
    int mode = jp["mode"].integer(4);
    if (mode != 4) throw std::runtime_error("non-triangle primitive");
    const Value& attrs = jp["attributes"];
    int comps = 0;

    if (!attrs.has("POSITION")) throw std::runtime_error("primitive without POSITION");
    auto pos = readAccessor(c, attrs["POSITION"].integer(), comps);
    if (comps != 3) throw std::runtime_error("POSITION must be VEC3");
    size_t vcount = pos.size() / 3;
    p.positions.resize(vcount);
    for (size_t i = 0; i < vcount; ++i) p.positions[i] = {float(pos[i * 3]), float(pos[i * 3 + 1]), float(pos[i * 3 + 2])};

    if (attrs.has("NORMAL")) {
        auto n = readAccessor(c, attrs["NORMAL"].integer(), comps);
        if (comps == 3 && n.size() == vcount * 3) {
            p.normals.resize(vcount);
            for (size_t i = 0; i < vcount; ++i) p.normals[i] = {float(n[i * 3]), float(n[i * 3 + 1]), float(n[i * 3 + 2])};
        }
    }
    if (attrs.has("TEXCOORD_0")) {
        auto uv = readAccessor(c, attrs["TEXCOORD_0"].integer(), comps);
        if (comps == 2 && uv.size() == vcount * 2) {
            p.uvs.resize(vcount * 2);
            for (size_t i = 0; i < vcount * 2; ++i) p.uvs[i] = float(uv[i]);
        }
    }
    if (attrs.has("JOINTS_0") && attrs.has("WEIGHTS_0")) {
        auto j = readAccessor(c, attrs["JOINTS_0"].integer(), comps);
        int wcomps = 0;
        auto w = readAccessor(c, attrs["WEIGHTS_0"].integer(), wcomps);
        if (comps == 4 && wcomps == 4 && j.size() == vcount * 4 && w.size() == vcount * 4) {
            p.joints.resize(vcount * 4);
            p.weights.resize(vcount * 4);
            for (size_t i = 0; i < vcount; ++i) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) {
                    p.joints[i * 4 + k] = static_cast<uint16_t>(j[i * 4 + k]);
                    p.weights[i * 4 + k] = float(w[i * 4 + k]);
                    sum += p.weights[i * 4 + k];
                }
                if (sum > 1e-6f)
                    for (int k = 0; k < 4; ++k) p.weights[i * 4 + k] /= sum;
                else
                    p.weights[i * 4] = 1.0f;
            }
        }
    }

    if (jp.has("indices")) {
        auto idx = readAccessor(c, jp["indices"].integer(), comps);
        p.indices.resize(idx.size());
        for (size_t i = 0; i < idx.size(); ++i) p.indices[i] = static_cast<uint32_t>(idx[i]);
    } else {
        p.indices.resize(vcount);
        for (size_t i = 0; i < vcount; ++i) p.indices[i] = static_cast<uint32_t>(i);
    }
    p.indices.resize(p.indices.size() - p.indices.size() % 3);
    for (auto& i : p.indices)
        if (i >= vcount) i = 0;

    if (p.normals.empty()) computeNormals(p);
    p.material = jp["material"].integer(-1);
    return p;
}

void readColor(const Value& arr, float* out, int n) {
    if (!arr.isArray()) return;
    for (int i = 0; i < n && i < static_cast<int>(arr.size()); ++i) out[i] = float(arr[i].num(out[i]));
}

void loadMaterials(const BinCtx& c, Model& m) {
    const Value& mats = c.doc["materials"];
    for (size_t i = 0; i < mats.size(); ++i) {
        const Value& jm = mats[i];
        Material mat;
        mat.name = jm["name"].str("");
        const Value& pbr = jm["pbrMetallicRoughness"];
        readColor(pbr["baseColorFactor"], mat.baseColor, 4);
        if (pbr["baseColorTexture"].isObject()) mat.baseColorTexture = pbr["baseColorTexture"]["index"].integer(-1);
        readColor(jm["emissiveFactor"], mat.emissive, 3);
        if (jm["emissiveTexture"].isObject()) mat.emissiveTexture = jm["emissiveTexture"]["index"].integer(-1);
        std::string am = jm["alphaMode"].str("OPAQUE");
        mat.alphaMode = am == "BLEND" ? AlphaMode::Blend : (am == "MASK" ? AlphaMode::Mask : AlphaMode::Opaque);
        mat.alphaCutoff = float(jm["alphaCutoff"].num(0.5));
        mat.doubleSided = jm["doubleSided"].boolean(false);

        const Value& ext = jm["extensions"];
        if (ext.has("KHR_materials_unlit")) mat.unlit = true;
        const Value& mt = ext["VRMC_materials_mtoon"];
        if (mt.isObject()) {
            mat.isMToon = true;
            for (int k = 0; k < 3; ++k) mat.shadeColor[k] = mat.baseColor[k];
            readColor(mt["shadeColorFactor"], mat.shadeColor, 3);
            if (mt["shadeMultiplyTexture"].isObject()) mat.shadeTexture = mt["shadeMultiplyTexture"]["index"].integer(-1);
            mat.renderQueueOffset = mt["renderQueueOffsetNumber"].integer(0);
        }
        m.materials.push_back(mat);
    }

    // VRM 0.x stores MToon parameters in a side table keyed by material name/index.
    const Value& mprops = c.doc["extensions"]["VRM"]["materialProperties"];
    for (size_t i = 0; i < mprops.size(); ++i) {
        const Value& mp = mprops[i];
        Material* mat = nullptr;
        std::string name = mp["name"].str("");
        for (auto& candidate : m.materials)
            if (!name.empty() && candidate.name == name) { mat = &candidate; break; }
        if (!mat && i < m.materials.size()) mat = &m.materials[i];
        if (!mat) continue;
        std::string shader = mp["shader"].str("");
        if (shader.find("MToon") == std::string::npos) continue;
        mat->isMToon = true;
        const Value& vp = mp["vectorProperties"];
        if (vp["_Color"].isArray()) readColor(vp["_Color"], mat->baseColor, 4);
        if (vp["_ShadeColor"].isArray()) readColor(vp["_ShadeColor"], mat->shadeColor, 3);
        const Value& tp = mp["textureProperties"];
        if (tp.has("_MainTex")) mat->baseColorTexture = tp["_MainTex"].integer(mat->baseColorTexture);
        if (tp.has("_ShadeTexture")) mat->shadeTexture = tp["_ShadeTexture"].integer(-1);
        const Value& fp = mp["floatProperties"];
        if (fp.has("_CullMode")) mat->doubleSided = fp["_CullMode"].integer(2) == 0;
        if (fp.has("_Cutoff")) mat->alphaCutoff = float(fp["_Cutoff"].num(mat->alphaCutoff));
        if (fp.has("_BlendMode")) {
            int bm = fp["_BlendMode"].integer(0);
            mat->alphaMode = bm == 0 ? AlphaMode::Opaque : (bm == 1 ? AlphaMode::Mask : AlphaMode::Blend);
        }
        mat->renderQueueOffset = mp["renderQueue"].integer(2000) - 2000;
    }
}

void loadTextures(const BinCtx& c, Model& m) {
    const Value& imgs = c.doc["images"];
    for (size_t i = 0; i < imgs.size(); ++i) {
        const Value& ji = imgs[i];
        Image img;
        img.name = ji["name"].str("");
        img.mimeType = ji["mimeType"].str("");
        if (ji.has("bufferView")) {
            size_t len = 0, stride = 0;
            const uint8_t* d = bufferViewData(c, ji["bufferView"].integer(), len, stride);
            img.data.assign(d, d + len);
        }
        m.images.push_back(std::move(img));
    }
    const Value& samplers = c.doc["samplers"];
    const Value& texs = c.doc["textures"];
    for (size_t i = 0; i < texs.size(); ++i) {
        Texture t;
        t.image = texs[i]["source"].integer(-1);
        if (texs[i].has("sampler")) {
            const Value& s = samplers[texs[i]["sampler"].integer()];
            t.nearest = s["magFilter"].integer(9729) == 9728;
            t.clampU = s["wrapS"].integer(10497) == 33071;
            t.clampV = s["wrapT"].integer(10497) == 33071;
        }
        m.textures.push_back(t);
    }
}

void loadHumanoid(const BinCtx& c, Model& m) {
    const Value& ext = c.doc["extensions"];
    if (ext["VRMC_vrm"].isObject()) {
        m.version = VrmVersion::V1;
        const Value& vrm = ext["VRMC_vrm"];
        m.meta.title = vrm["meta"]["name"].str("");
        const Value& authors = vrm["meta"]["authors"];
        if (authors.size() > 0) m.meta.author = authors[0].str("");
        m.meta.version = vrm["meta"]["version"].str("");
        for (auto& kv : vrm["humanoid"]["humanBones"].members()) {
            int node = kv.second["node"].integer(-1);
            if (node >= 0 && node < static_cast<int>(m.nodes.size())) m.humanoid[kv.first] = node;
        }
    } else if (ext["VRM"].isObject()) {
        m.version = VrmVersion::V0;
        const Value& vrm = ext["VRM"];
        m.meta.title = vrm["meta"]["title"].str("");
        m.meta.author = vrm["meta"]["author"].str("");
        m.meta.version = vrm["meta"]["version"].str("");
        const Value& hb = vrm["humanoid"]["humanBones"];
        for (size_t i = 0; i < hb.size(); ++i) {
            std::string bone = hb[i]["bone"].str("");
            int node = hb[i]["node"].integer(-1);
            if (!bone.empty() && node >= 0 && node < static_cast<int>(m.nodes.size())) m.humanoid[bone] = node;
        }
    }
}

} // namespace

Model loadGlb(const uint8_t* data, size_t size) {
    if (size < 20) throw std::runtime_error("file too small to be a VRM/GLB");
    if (rd32(data) != 0x46546C67) throw std::runtime_error("not a binary glTF file (bad magic)");
    if (rd32(data + 4) != 2) throw std::runtime_error("unsupported glTF version");
    size_t total = std::min<size_t>(rd32(data + 8), size);

    const char* jsonData = nullptr;
    size_t jsonLen = 0;
    const uint8_t* bin = nullptr;
    size_t binLen = 0;
    size_t off = 12;
    while (off + 8 <= total) {
        uint32_t clen = rd32(data + off);
        uint32_t ctype = rd32(data + off + 4);
        off += 8;
        if (off + clen > total) throw std::runtime_error("truncated GLB chunk");
        if (ctype == 0x4E4F534A && !jsonData) { jsonData = reinterpret_cast<const char*>(data + off); jsonLen = clen; }
        else if (ctype == 0x004E4942 && !bin) { bin = data + off; binLen = clen; }
        off += (clen + 3) & ~size_t(3);
    }
    if (!jsonData) throw std::runtime_error("GLB has no JSON chunk");

    json::Value doc = json::parse(jsonData, jsonLen);
    static const uint8_t kEmpty[1] = {0};
    BinCtx c{doc, bin ? bin : kEmpty, bin ? binLen : 0};
    Model m;

    // Nodes.
    const Value& jn = doc["nodes"];
    m.nodes.resize(jn.size());
    for (size_t i = 0; i < jn.size(); ++i) {
        const Value& n = jn[i];
        Node& node = m.nodes[i];
        node.name = n["name"].str("");
        node.mesh = n["mesh"].integer(-1);
        node.skin = n["skin"].integer(-1);
        if (n["matrix"].isArray() && n["matrix"].size() == 16) {
            Mat4 mat;
            for (int k = 0; k < 16; ++k) mat.m[k] = float(n["matrix"][k].num());
            mat.decompose(node.translation, node.rotation, node.scale);
        } else {
            const Value& t = n["translation"];
            const Value& r = n["rotation"];
            const Value& s = n["scale"];
            if (t.size() == 3) node.translation = {float(t[0].num()), float(t[1].num()), float(t[2].num())};
            if (r.size() == 4) node.rotation = normalize(Quat{float(r[0].num()), float(r[1].num()), float(r[2].num()), float(r[3].num(1))});
            if (s.size() == 3) node.scale = {float(s[0].num(1)), float(s[1].num(1)), float(s[2].num(1))};
        }
        const Value& ch = n["children"];
        for (size_t k = 0; k < ch.size(); ++k) {
            int child = ch[k].integer(-1);
            if (child >= 0 && child < static_cast<int>(jn.size())) node.children.push_back(child);
        }
    }
    for (size_t i = 0; i < m.nodes.size(); ++i)
        for (int child : m.nodes[i].children) m.nodes[child].parent = static_cast<int>(i);

    // Root nodes: those of the default scene, or every parentless node.
    int sceneIdx = doc["scene"].integer(0);
    const Value& sceneNodes = doc["scenes"][sceneIdx]["nodes"];
    for (size_t i = 0; i < sceneNodes.size(); ++i) {
        int r = sceneNodes[i].integer(-1);
        if (r >= 0 && r < static_cast<int>(m.nodes.size()) && m.nodes[r].parent < 0) m.roots.push_back(r);
    }
    if (m.roots.empty())
        for (size_t i = 0; i < m.nodes.size(); ++i)
            if (m.nodes[i].parent < 0) m.roots.push_back(static_cast<int>(i));

    // Meshes.
    const Value& jmeshes = doc["meshes"];
    for (size_t i = 0; i < jmeshes.size(); ++i) {
        Mesh mesh;
        mesh.name = jmeshes[i]["name"].str("");
        const Value& prims = jmeshes[i]["primitives"];
        for (size_t k = 0; k < prims.size(); ++k) {
            try {
                mesh.primitives.push_back(loadPrimitive(c, prims[k]));
            } catch (const std::exception&) {
                // Skip primitives we cannot represent (points/lines etc.) instead of failing the whole avatar.
            }
        }
        m.meshes.push_back(std::move(mesh));
    }

    // Skins.
    const Value& jskins = doc["skins"];
    for (size_t i = 0; i < jskins.size(); ++i) {
        Skin skin;
        const Value& joints = jskins[i]["joints"];
        for (size_t k = 0; k < joints.size(); ++k) skin.joints.push_back(joints[k].integer());
        skin.inverseBind.resize(skin.joints.size());
        if (jskins[i].has("inverseBindMatrices")) {
            int comps = 0;
            auto ibm = readAccessor(c, jskins[i]["inverseBindMatrices"].integer(), comps);
            if (comps == 16)
                for (size_t k = 0; k < skin.joints.size() && (k + 1) * 16 <= ibm.size(); ++k)
                    for (int e = 0; e < 16; ++e) skin.inverseBind[k].m[e] = float(ibm[k * 16 + e]);
        }
        m.skins.push_back(std::move(skin));
    }

    loadTextures(c, m);
    loadMaterials(c, m);
    loadHumanoid(c, m);
    if (m.version == VrmVersion::None) throw std::runtime_error("file is a glTF but has no VRM humanoid extension");
    if (m.humanBone(bones::Hips) < 0 || m.humanBone(bones::Head) < 0)
        throw std::runtime_error("VRM humanoid is missing required bones (hips/head)");
    return m;
}

Model loadFile(const std::string& utf8Path) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, wpath.data(), wlen);
    FILE* f = _wfopen(wpath.c_str(), L"rb");
#else
    FILE* f = std::fopen(utf8Path.c_str(), "rb");
#endif
    if (!f) throw std::runtime_error("cannot open " + utf8Path);
    std::vector<uint8_t> buf;
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) buf.insert(buf.end(), chunk, chunk + n);
    std::fclose(f);
    return loadGlb(buf.data(), buf.size());
}

} // namespace rfv::vrm
