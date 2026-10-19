#include "binding.hpp"

#include <cstring>
#include <stdexcept>

namespace rfv::game {

bool parseMatrixFormat(const std::string& s, MatrixFormat& out) {
    std::string l = toLower(trim(s));
    if (l == "matrix43") out = MatrixFormat::Matrix43;
    else if (l == "float4x4") out = MatrixFormat::Float4x4;
    else if (l == "float3x4") out = MatrixFormat::Float3x4;
    else if (l == "float4x4t") out = MatrixFormat::Float4x4T;
    else return false;
    return true;
}

int matrixFormatSize(MatrixFormat f) {
    return (f == MatrixFormat::Float4x4 || f == MatrixFormat::Float4x4T) ? 64 : 48;
}

Mat4 decodeMatrix(const float* f, MatrixFormat fmt) {
    Mat4 m;
    auto setCol = [&](int c, float x, float y, float z) { m.at(0, c) = x; m.at(1, c) = y; m.at(2, c) = z; };
    switch (fmt) {
    case MatrixFormat::Matrix43:
        for (int c = 0; c < 4; ++c) setCol(c, f[c * 3], f[c * 3 + 1], f[c * 3 + 2]);
        break;
    case MatrixFormat::Float4x4:
        for (int c = 0; c < 4; ++c) setCol(c, f[c * 4], f[c * 4 + 1], f[c * 4 + 2]);
        break;
    case MatrixFormat::Float3x4:
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) m.at(r, c) = f[r * 4 + c];
        break;
    case MatrixFormat::Float4x4T:
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) m.at(r, c) = f[r * 4 + c];
        break;
    }
    return m;
}

namespace {

PointerPath parsePath(const Ini& ini, const char* sec, const char* key, std::string& errors) {
    std::string text = ini.get(sec, key);
    if (text.empty()) return {};
    try {
        return PointerPath::parse(text);
    } catch (const std::exception& e) {
        errors += std::string("[") + sec + "] " + key + ": " + e.what() + "\n";
        return {};
    }
}

bool parseAxis(const std::string& s, int& axis, float& sign) {
    std::string l = toLower(trim(s));
    if (l.size() == 1) l = "+" + l;
    if (l.size() != 2 || (l[0] != '+' && l[0] != '-')) return false;
    sign = l[0] == '-' ? -1.0f : 1.0f;
    if (l[1] < 'x' || l[1] > 'z') return false;
    axis = l[1] - 'x';
    return true;
}

} // namespace

void GameBinding::load(const Ini& ini, std::string& errors) {
    // --- Skeleton
    const char* S = "GameSkeleton";
    boneArray_ = parsePath(ini, S, "BoneArray", errors);
    boneStride_ = ini.getInt(S, "BoneStride", 48);
    boneCount_ = ini.getInt(S, "BoneCount", 0);
    if (!parseMatrixFormat(ini.get(S, "BoneFormat", "matrix43"), boneFormat_)) errors += "[GameSkeleton] BoneFormat: unknown format\n";
    bonesModelSpace_ = toLower(ini.get(S, "BoneSpace", "world")) == "model";
    playerTransform_ = parsePath(ini, S, "PlayerTransform", errors);
    if (!parseMatrixFormat(ini.get(S, "PlayerTransformFormat", "matrix43"), playerFormat_))
        errors += "[GameSkeleton] PlayerTransformFormat: unknown format\n";
    if (bonesModelSpace_ && playerTransform_.empty()) errors += "[GameSkeleton] BoneSpace=model requires PlayerTransform\n";
    boneNames_ = parsePath(ini, S, "BoneNameArray", errors);
    boneNameStride_ = ini.getInt(S, "BoneNameStride", 8);
    if (!parseAxis(ini.get(S, "HeadForwardAxis", "+z"), headForwardAxis_, headForwardSign_)) errors += "[GameSkeleton] HeadForwardAxis invalid\n";
    if (!parseAxis(ini.get(S, "HeadUpAxis", "+y"), headUpAxis_, headUpSign_)) errors += "[GameSkeleton] HeadUpAxis invalid\n";
    leftHanded_ = toLower(ini.get(S, "Handedness", "left")) != "right";
    unitsToMeters_ = ini.getFloat(S, "UnitsToMeters", 1.0f);
    if (unitsToMeters_ <= 0) unitsToMeters_ = 1.0f;
    if (boneStride_ < matrixFormatSize(boneFormat_)) errors += "[GameSkeleton] BoneStride is smaller than the matrix size\n";

    boneMap_.clear();
    resolvedMap_.clear();
    mapResolved_ = false;
    for (const auto& kv : ini.section("BoneMap"))
        if (!kv.second.empty()) boneMap_[kv.first] = kv.second;

    // --- Camera
    const char* C = "GameCamera";
    camera_ = parsePath(ini, C, "Transform", errors);
    if (!parseMatrixFormat(ini.get(C, "Format", "matrix43"), cameraFormat_)) errors += "[GameCamera] Format: unknown format\n";
    fov_ = parsePath(ini, C, "Fov", errors);
    fovDegrees_ = toLower(ini.get(C, "FovUnits", "degrees")) != "radians";
    fovHorizontal_ = toLower(ini.get(C, "FovAxis", "vertical")) == "horizontal";
    fovDefault_ = ini.getFloat(C, "FovDefault", 70.0f);
    zNear_ = ini.getFloat(C, "Near", 0.1f);
    zFar_ = ini.getFloat(C, "Far", 2000.0f);
    reversedZ_ = toLower(ini.get("Render", "DepthMode", "none")) == "reversed";

    // --- Pause
    const char* P = "GamePause";
    pauseFlag_ = parsePath(ini, P, "Flag", errors);
    pauseSize_ = ini.getInt(P, "Size", 1);
    if (pauseSize_ != 1 && pauseSize_ != 2 && pauseSize_ != 4) { errors += "[GamePause] Size must be 1, 2 or 4\n"; pauseSize_ = 1; }
    long long pv;
    pausedValue_ = parseInteger(ini.get(P, "PausedValue", "1"), pv) ? pv : 1;

    // --- Hide Mason via memory writes:  WriteN = <path> | <u8|u16|u32|f32> | <value>
    hide_.clear();
    for (const auto& kv : ini.section("GameHide")) {
        if (kv.second.empty() || toLower(kv.first).rfind("write", 0) != 0) continue;
        std::string v = kv.second;
        size_t p1 = v.find('|'), p2 = p1 == std::string::npos ? p1 : v.find('|', p1 + 1);
        if (p2 == std::string::npos) { errors += "[GameHide] " + kv.first + ": expected 'path | type | value'\n"; continue; }
        HideWrite w;
        try {
            w.path = PointerPath::parse(v.substr(0, p1));
        } catch (const std::exception& e) {
            errors += "[GameHide] " + kv.first + ": " + e.what() + "\n";
            continue;
        }
        std::string type = toLower(trim(v.substr(p1 + 1, p2 - p1 - 1)));
        std::string val = trim(v.substr(p2 + 1));
        if (type == "u8") w.size = 1;
        else if (type == "u16") w.size = 2;
        else if (type == "u32") w.size = 4;
        else if (type == "f32") { w.size = 4; w.isFloat = true; }
        else { errors += "[GameHide] " + kv.first + ": type must be u8/u16/u32/f32\n"; continue; }
        if (w.isFloat) w.fvalue = std::strtof(val.c_str(), nullptr);
        else if (!parseInteger(val, w.value)) { errors += "[GameHide] " + kv.first + ": bad value\n"; continue; }
        hide_.push_back(std::move(w));
    }
}

bool GameBinding::readBones(MemoryAccess& mem, std::vector<Mat4>& out) {
    out.clear();
    uintptr_t base;
    if (!boneArray_.resolve(mem, base)) return false;
    int count = boneCount_;
    if (count <= 0) {
        // Unknown count: read up to the highest numerically mapped index.
        for (const auto& kv : boneMap_) {
            long long idx;
            if (parseInteger(kv.second, idx)) count = std::max<int>(count, static_cast<int>(idx) + 1);
        }
        if (count <= 0) count = 64;
    }
    count = std::min(count, 512);
    std::vector<uint8_t> raw(static_cast<size_t>(count) * boneStride_);
    if (!mem.read(base, raw.data(), raw.size())) return false;

    Mat4 player;
    if (bonesModelSpace_) {
        uintptr_t pa;
        float pf[16];
        if (!playerTransform_.resolve(mem, pa) || !mem.read(pa, pf, matrixFormatSize(playerFormat_))) return false;
        player = decodeMatrix(pf, playerFormat_);
    }
    out.resize(count);
    for (int i = 0; i < count; ++i) {
        float f[16];
        std::memcpy(f, raw.data() + static_cast<size_t>(i) * boneStride_, matrixFormatSize(boneFormat_));
        Mat4 m = decodeMatrix(f, boneFormat_);
        out[i] = bonesModelSpace_ ? player * m : m;
    }
    // Sanity check: reject obviously garbage data (NaNs / astronomically large positions).
    for (const Mat4& m : out) {
        Vec3 t = m.translation();
        if (!(std::fabs(t.x) < 1e7f && std::fabs(t.y) < 1e7f && std::fabs(t.z) < 1e7f)) return false;
    }
    return true;
}

std::vector<std::string> GameBinding::readBoneNames(MemoryAccess& mem) {
    std::vector<std::string> names;
    uintptr_t base;
    if (boneNames_.empty() || !boneNames_.resolve(mem, base)) return names;
    int count = boneCount_ > 0 ? boneCount_ : 128;
    for (int i = 0; i < count; ++i) {
        uint64_t p;
        if (!mem.get(base + static_cast<uintptr_t>(i) * boneNameStride_, p) || p == 0) break;
        // Read up to the terminator one byte at a time so a short string next to an
        // unmapped page still reads correctly (names are only read once).
        std::string s;
        char ch = 0;
        while (s.size() < 63 && mem.get(static_cast<uintptr_t>(p) + s.size(), ch) && ch != 0) s += ch;
        bool printable = !s.empty();
        for (char c : s)
            if (c < 32 || c > 126) printable = false;
        if (!printable) break;
        names.push_back(s);
    }
    return names;
}

bool GameBinding::resolveBoneMap(MemoryAccess& mem) {
    if (mapResolved_) return true;
    std::vector<std::string> names;
    bool needNames = false;
    resolvedMap_.clear();
    for (const auto& kv : boneMap_) {
        long long idx;
        if (parseInteger(kv.second, idx)) resolvedMap_[kv.first] = static_cast<int>(idx);
        else needNames = true;
    }
    if (needNames) {
        names = readBoneNames(mem);
        if (names.empty()) return !resolvedMap_.empty();  // retry name lookup next frame
        for (const auto& kv : boneMap_) {
            long long idx;
            if (parseInteger(kv.second, idx)) continue;
            for (size_t i = 0; i < names.size(); ++i)
                if (toLower(names[i]) == toLower(kv.second)) { resolvedMap_[kv.first] = static_cast<int>(i); break; }
        }
    }
    mapResolved_ = true;
    return !resolvedMap_.empty();
}

Vec3 GameBinding::gameToAvatar(const Vec3& p) const {
    Vec3 r = p * unitsToMeters_;
    if (leftHanded_) r.z = -r.z;
    return r;
}

Vec3 GameBinding::gameDirToAvatar(const Vec3& d) const {
    Vec3 r = d;
    if (leftHanded_) r.z = -r.z;
    return r;
}

Mat4 GameBinding::avatarToGame() const {
    float s = 1.0f / unitsToMeters_;
    return Mat4::scale(Vec3{s, s, leftHanded_ ? -s : s});
}

bool GameBinding::samplePose(MemoryAccess& mem, vrm::SourcePose& out) {
    out = vrm::SourcePose{};
    if (!hasSkeleton()) return false;
    std::vector<Mat4> bones;
    if (!readBones(mem, bones)) return false;
    if (!resolveBoneMap(mem)) return false;
    for (const auto& kv : resolvedMap_) {
        if (kv.second < 0 || kv.second >= static_cast<int>(bones.size())) continue;
        out.joints[kv.first] = gameToAvatar(bones[kv.second].translation());
    }
    auto head = resolvedMap_.find(vrm::bones::Head);
    if (head != resolvedMap_.end() && head->second >= 0 && head->second < static_cast<int>(bones.size())) {
        const Mat4& h = bones[head->second];
        Vec3 f = normalize(h.column(headForwardAxis_)) * headForwardSign_;
        Vec3 u = normalize(h.column(headUpAxis_)) * headUpSign_;
        if (length(f) > 0.5f && length(u) > 0.5f && std::fabs(dot(f, u)) < 0.5f) {
            out.hasHeadFrame = true;
            out.headForward = gameDirToAvatar(f);
            out.headUp = gameDirToAvatar(u);
        }
    }
    out.valid = out.has(vrm::bones::Hips) && out.has(vrm::bones::Head);
    return out.valid;
}

bool GameBinding::readCamera(MemoryAccess& mem, float aspect, CameraState& out) {
    if (!hasCamera()) return false;
    uintptr_t a;
    float f[16];
    if (!camera_.resolve(mem, a) || !mem.read(a, f, matrixFormatSize(cameraFormat_))) return false;
    Mat4 m = decodeMatrix(f, cameraFormat_);
    Vec3 right = normalize(m.column(0)), up = normalize(m.column(1)), fwd = normalize(m.column(2));
    if (std::fabs(dot(right, up)) > 0.1f || std::fabs(dot(up, fwd)) > 0.1f) return false;  // not a rotation
    out.position = m.translation();
    out.view = Mat4::viewLH(out.position, right, up, fwd);

    float fov = fovDefault_;
    if (!fov_.empty()) {
        uintptr_t fa;
        float v;
        if (fov_.resolve(mem, fa) && mem.get(fa, v) && v > 0 && v < 180) fov = v;
    }
    float rad = fovDegrees_ ? fov * kPi / 180.0f : fov;
    if (fovHorizontal_) rad = 2.0f * std::atan(std::tan(rad * 0.5f) / std::max(aspect, 0.1f));
    out.fovY = rad;
    out.proj = Mat4::perspectiveLH(rad, aspect, zNear_, zFar_, reversedZ_);
    return true;
}

int GameBinding::readPaused(MemoryAccess& mem) {
    if (!hasPauseFlag()) return -1;
    uintptr_t a;
    if (!pauseFlag_.resolve(mem, a)) return -1;
    uint32_t v = 0;
    if (!mem.read(a, &v, pauseSize_)) return -1;
    uint32_t mask = pauseSize_ == 4 ? 0xFFFFFFFFu : ((1u << (pauseSize_ * 8)) - 1);
    return (v & mask) == (static_cast<uint32_t>(pausedValue_) & mask) ? 1 : 0;
}

void GameBinding::applyHide(MemoryAccess& mem, bool hide) {
    for (auto& w : hide_) {
        if (hide) {
            uintptr_t a;
            if (!w.path.resolve(mem, a)) continue;
            if (!w.saved || w.savedAddr != a) {
                // Remember the original only when the target changed (e.g. new player object).
                if (!mem.read(a, w.original, w.size)) continue;
                w.saved = true;
                w.savedAddr = a;
            }
            uint8_t buf[8] = {};
            if (w.isFloat) std::memcpy(buf, &w.fvalue, 4);
            else std::memcpy(buf, &w.value, w.size);
            mem.write(a, buf, w.size);
        } else if (hidden_ && w.saved) {
            uintptr_t a;
            if (w.path.resolve(mem, a) && a == w.savedAddr) mem.write(a, w.original, w.size);
            w.saved = false;
        }
    }
    hidden_ = hide;
}

} // namespace rfv::game
