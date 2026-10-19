#include "avatar.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace rfv::vrm {

using namespace bones;

Avatar::Avatar(const Model& model) : model_(model) {
    size_t n = model_.nodes.size();
    restT_.resize(n);
    restR_.resize(n);
    restS_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        restT_[i] = model_.nodes[i].translation;
        restR_[i] = model_.nodes[i].rotation;
        restS_[i] = model_.nodes[i].scale;
    }

    // Parent-before-child ordering (robust to odd node orders in the file).
    std::vector<char> visited(n, 0);
    std::function<void(int)> visit = [&](int idx) {
        if (visited[idx]) return;
        visited[idx] = 1;
        order_.push_back(idx);
        for (int c : model_.nodes[idx].children) visit(c);
    };
    for (size_t i = 0; i < n; ++i)
        if (model_.nodes[i].parent < 0) visit(static_cast<int>(i));
    for (size_t i = 0; i < n; ++i) visit(static_cast<int>(i));  // cycles / orphans

    world_.resize(n);
    worldR_.resize(n);
    morphWeights_.resize(model_.meshes.size());
    for (size_t m = 0; m < model_.meshes.size(); ++m) {
        size_t targets = 0;
        for (const auto& p : model_.meshes[m].primitives) targets = std::max(targets, p.targets.size());
        morphWeights_[m].assign(targets, 0.0f);
        for (size_t t = 0; t < targets && t < model_.meshes[m].defaultWeights.size(); ++t)
            morphWeights_[m][t] = model_.meshes[m].defaultWeights[t];
    }
    resetToRest();
    root_ = Mat4();
    rootRot_ = Quat{};
    updateWorld();
    restWorld_ = world_;
    restWorldR_ = worldR_;

    // Character orientation in rest pose.
    int lu = hb(LeftUpperLeg), ru = hb(RightUpperLeg);
    if (lu < 0 || ru < 0) { lu = hb(LeftUpperArm); ru = hb(RightUpperArm); }
    if (lu >= 0 && ru >= 0) {
        Vec3 left = restPos(lu) - restPos(ru);
        left.y = 0;
        restLeft_ = normalize(left, {1, 0, 0});
        restForward_ = normalize(cross(restLeft_, Vec3{0, 1, 0}), {0, 0, 1});
    } else if (model_.version == VrmVersion::V0) {
        restForward_ = {0, 0, -1};
        restLeft_ = {-1, 0, 0};
    }
    computeRestMetrics();
}

void Avatar::resetToRest() {
    curT_ = restT_;
    curR_ = restR_;
}

void Avatar::updateWorld() {
    for (int idx : order_) {
        const Node& node = model_.nodes[idx];
        Mat4 local = Mat4::fromTRS(curT_[idx], curR_[idx], restS_[idx]);
        if (node.parent < 0) {
            world_[idx] = root_ * local;
            worldR_[idx] = normalize(rootRot_ * curR_[idx]);
        } else {
            world_[idx] = world_[node.parent] * local;
            worldR_[idx] = normalize(worldR_[node.parent] * curR_[idx]);
        }
    }
}

void Avatar::updateWorldSubtree(int idx) {
    const Node& node = model_.nodes[idx];
    Mat4 local = Mat4::fromTRS(curT_[idx], curR_[idx], restS_[idx]);
    if (node.parent < 0) {
        world_[idx] = root_ * local;
        worldR_[idx] = normalize(rootRot_ * curR_[idx]);
    } else {
        world_[idx] = world_[node.parent] * local;
        worldR_[idx] = normalize(worldR_[node.parent] * curR_[idx]);
    }
    for (int c : node.children) updateWorldSubtree(c);
}

void Avatar::computeRestMetrics() {
    float minY = std::numeric_limits<float>::max(), maxY = -std::numeric_limits<float>::max();
    std::vector<Mat4> palette;
    for (size_t ni = 0; ni < model_.nodes.size(); ++ni) {
        const Node& node = model_.nodes[ni];
        if (node.mesh < 0 || node.mesh >= static_cast<int>(model_.meshes.size())) continue;
        bool skinned = node.skin >= 0 && node.skin < static_cast<int>(model_.skins.size());
        if (skinned) skinPalette(node.skin, palette);
        for (const Primitive& p : model_.meshes[node.mesh].primitives) {
            for (size_t v = 0; v < p.positions.size(); ++v) {
                Vec3 pos;
                if (skinned && !p.joints.empty()) {
                    for (int k = 0; k < 4; ++k) {
                        float w = p.weights[v * 4 + k];
                        uint16_t j = p.joints[v * 4 + k];
                        if (w <= 0 || j >= palette.size()) continue;
                        pos += palette[j].transformPoint(p.positions[v]) * w;
                    }
                } else {
                    pos = world_[ni].transformPoint(p.positions[v]);
                }
                minY = std::min(minY, pos.y);
                maxY = std::max(maxY, pos.y);
            }
        }
    }
    int head = hb(Head), hips = hb(Hips), lf = hb(LeftFoot), rf = hb(RightFoot);
    rest_.headY = head >= 0 ? restPos(head).y : 0;
    rest_.hipsY = hips >= 0 ? restPos(hips).y : 0;
    if (lf >= 0 && rf >= 0) rest_.ankleY = 0.5f * (restPos(lf).y + restPos(rf).y);
    else if (lf >= 0) rest_.ankleY = restPos(lf).y;
    else rest_.ankleY = 0;
    if (minY > maxY) {  // no geometry: fall back to skeleton
        minY = rest_.ankleY;
        maxY = rest_.headY + 0.1f;
    }
    rest_.minY = minY;
    rest_.maxY = maxY;
    rest_.height = std::max(maxY - minY, 0.01f);
}

float Avatar::scaleForHeight(float targetHeight) const {
    if (targetHeight <= 0) return 1.0f;
    return targetHeight / rest_.height;
}

float Avatar::scaleForJointSpan(float sourceHeadAboveAnkle) const {
    float span = rest_.headY - rest_.ankleY;
    if (span <= 0.01f || sourceHeadAboveAnkle <= 0.01f) return 1.0f;
    return sourceHeadAboveAnkle / span;
}

void Avatar::skinPalette(int skin, std::vector<Mat4>& out) const {
    const Skin& s = model_.skins[skin];
    out.resize(s.joints.size());
    for (size_t i = 0; i < s.joints.size(); ++i) {
        int j = s.joints[i];
        out[i] = (j >= 0 && j < static_cast<int>(world_.size())) ? world_[j] * s.inverseBind[i] : Mat4();
    }
}

Vec3 Avatar::bonePosition(const std::string& bone) const {
    int n = model_.humanBone(bone);
    return n >= 0 ? world_[n].translation() : Vec3{};
}

void Avatar::setWorldRotation(int node, const Quat& worldRot) {
    int parent = model_.nodes[node].parent;
    Quat parentR = parent < 0 ? rootRot_ : worldR_[parent];
    curR_[node] = normalize(parentR.conjugate() * worldRot);
    updateWorldSubtree(node);
}

void Avatar::aimBone(int node, int childNode, const Vec3& targetDir, const Vec3* restPoleWorld, const Vec3* targetPole) {
    if (node < 0 || childNode < 0) return;
    if (length(targetDir) < 1e-6f) return;
    Vec3 restDirWorld = restPos(childNode) - restPos(node);
    if (length(restDirWorld) < 1e-6f) return;
    Quat restInv = restWorldR_[node].conjugate();
    Vec3 dirLocal = restInv.rotate(restDirWorld);
    const Quat& T = worldR_[node];
    Vec3 dirCur = T.rotate(dirLocal);
    Quat delta;
    if (restPoleWorld && targetPole && length(*targetPole) > 1e-6f) {
        Vec3 poleCur = T.rotate(restInv.rotate(*restPoleWorld));
        delta = alignFrames(dirCur, poleCur, targetDir, *targetPole);
    } else {
        delta = fromTo(dirCur, targetDir);
    }
    setWorldRotation(node, normalize(delta * T));
}

void Avatar::setRestPose(const Vec3& position, float yaw, bool aPose) {
    resetToRest();
    rootRot_ = Quat::axisAngle({0, 1, 0}, yaw);
    root_ = Mat4::translate(position + Vec3{0, -rest_.minY * scale_, 0}) * Mat4::fromTRS({}, rootRot_, {scale_, scale_, scale_});
    updateWorld();
    if (aPose) {
        const float c = 0.5f, s = 0.866f;  // arms 60 degrees below horizontal
        Vec3 down{0, -1, 0};
        Vec3 l = rootRot_.rotate(normalize(restLeft_ * c + down * s));
        Vec3 r = rootRot_.rotate(normalize(-restLeft_ * c + down * s));
        aimBone(hb(LeftUpperArm), hb(LeftLowerArm), l, nullptr, nullptr);
        aimBone(hb(RightUpperArm), hb(RightLowerArm), r, nullptr, nullptr);
    }
}

namespace {
// Offset of the middle joint from the line between the outer joints (bend direction).
bool bendPole(const Vec3& a, const Vec3& mid, const Vec3& b, Vec3& pole) {
    Vec3 line = b - a;
    float len = length(line);
    if (len < 1e-4f) return false;
    Vec3 dir = line / len;
    Vec3 proj = a + dir * dot(mid - a, dir);
    Vec3 off = mid - proj;
    if (length(off) < 0.03f * len) return false;  // nearly straight: bend plane undefined
    pole = normalize(off);
    return true;
}
} // namespace

bool Avatar::applySource(const SourcePose& src, bool groundFeet) {
    if (!src.valid || !src.has(Hips) || !src.has(Head)) return false;
    int hips = hb(Hips);
    if (hips < 0) return false;

    resetToRest();
    rootRot_ = Quat{};
    root_ = Mat4::scale(scale_);
    updateWorld();

    auto srcHas = [&](const char* b) { return src.has(b) && hb(b) >= 0; };

    // ---- Hips: position + full orientation from pelvis/spine frame.
    auto placeHips = [&](const Vec3& p) {
        int parent = model_.nodes[hips].parent;
        Mat4 parentM = parent < 0 ? root_ : world_[parent];
        curT_[hips] = parentM.inverse().transformPoint(p);
        updateWorldSubtree(hips);
    };
    placeHips(src.at(Hips));

    const char* upTarget = src.has(Spine) ? Spine : (src.has(Chest) ? Chest : (src.has(Neck) ? Neck : Head));
    int upNode = hb(upTarget);
    Vec3 srcUp = src.at(upTarget) - src.at(Hips);
    Vec3 restUp = upNode >= 0 ? restPos(upNode) - restPos(hips) : Vec3{0, 1, 0};
    Vec3 srcSide, restSide;
    bool haveSide = false;
    if (srcHas(LeftUpperLeg) && srcHas(RightUpperLeg)) {
        srcSide = src.at(LeftUpperLeg) - src.at(RightUpperLeg);
        restSide = restPos(hb(LeftUpperLeg)) - restPos(hb(RightUpperLeg));
        haveSide = true;
    } else if (srcHas(LeftUpperArm) && srcHas(RightUpperArm)) {
        srcSide = src.at(LeftUpperArm) - src.at(RightUpperArm);
        restSide = restPos(hb(LeftUpperArm)) - restPos(hb(RightUpperArm));
        haveSide = true;
    }
    Quat hipsDelta = haveSide ? alignFrames(restUp, restSide, srcUp, srcSide) : fromTo(restUp, srcUp);
    setWorldRotation(hips, normalize(hipsDelta * restWorldR_[hips]));
    Vec3 hipsForward = hipsDelta.rotate(restForward_);

    // Torso forward from the shoulder line, when available.
    Vec3 torsoForward = hipsForward;
    {
        const char* lA = srcHas(LeftShoulder) ? LeftShoulder : LeftUpperArm;
        const char* rA = srcHas(RightShoulder) ? RightShoulder : RightUpperArm;
        if (src.has(lA) && src.has(rA)) {
            Vec3 left = src.at(lA) - src.at(rA);
            Vec3 up = src.at(src.has(Neck) ? Neck : Head) - src.at(Hips);
            Vec3 f = cross(normalize(left), normalize(up));
            if (length(f) > 0.2f) torsoForward = normalize(f);
        }
    }

    // ---- Spine chain.
    std::vector<const char*> chain;
    for (const char* b : {Spine, Chest, UpperChest, Neck, Head})
        if (hb(b) >= 0) chain.push_back(b);
    for (size_t i = 0; i + 1 < chain.size(); ++i) {
        const char* bone = chain[i];
        if (!src.has(bone)) continue;
        const char* target = nullptr;
        for (size_t j = i + 1; j < chain.size(); ++j)
            if (src.has(chain[j])) { target = chain[j]; break; }
        if (!target) continue;
        Vec3 dir = src.at(target) - src.at(bone);
        Vec3 fwd = (i == 0) ? hipsForward : torsoForward;
        if (std::string(bone) == Neck && src.hasHeadFrame) fwd = src.headForward;
        aimBone(hb(bone), hb(chain[i + 1]), dir, &restForward_, &fwd);
    }

    // ---- Head orientation.
    if (src.hasHeadFrame && hb(Head) >= 0) {
        Quat d = alignFrames(Vec3{0, 1, 0}, restForward_, src.headUp, src.headForward);
        setWorldRotation(hb(Head), normalize(d * restWorldR_[hb(Head)]));
    }

    // ---- Arms.
    Vec3 back = -restForward_;
    auto arm = [&](const char* shoulder, const char* upper, const char* lower, const char* hand, const char* tip) {
        if (srcHas(shoulder) && srcHas(upper))
            aimBone(hb(shoulder), hb(upper), src.at(upper) - src.at(shoulder), &restForward_, &torsoForward);
        if (srcHas(upper) && srcHas(lower)) {
            Vec3 pole;
            Vec3 targetBack = -torsoForward;
            bool bent = srcHas(hand) && bendPole(src.at(upper), src.at(lower), src.at(hand), pole);
            aimBone(hb(upper), hb(lower), src.at(lower) - src.at(upper), &back, bent ? &pole : &targetBack);
        }
        if (srcHas(lower) && srcHas(hand)) aimBone(hb(lower), hb(hand), src.at(hand) - src.at(lower), nullptr, nullptr);
        if (srcHas(hand) && srcHas(tip)) aimBone(hb(hand), hb(tip), src.at(tip) - src.at(hand), nullptr, nullptr);
    };
    arm(LeftShoulder, LeftUpperArm, LeftLowerArm, LeftHand, LeftMiddleProximal);
    arm(RightShoulder, RightUpperArm, RightLowerArm, RightHand, RightMiddleProximal);

    // ---- Legs.
    auto leg = [&](const char* upper, const char* lower, const char* foot, const char* toes) {
        if (srcHas(upper) && srcHas(lower)) {
            Vec3 pole;
            bool bent = srcHas(foot) && bendPole(src.at(upper), src.at(lower), src.at(foot), pole);
            aimBone(hb(upper), hb(lower), src.at(lower) - src.at(upper), &restForward_, bent ? &pole : &hipsForward);
        }
        if (srcHas(lower) && srcHas(foot)) aimBone(hb(lower), hb(foot), src.at(foot) - src.at(lower), nullptr, nullptr);
        if (srcHas(foot) && srcHas(toes)) aimBone(hb(foot), hb(toes), src.at(toes) - src.at(foot), nullptr, nullptr);
    };
    leg(LeftUpperLeg, LeftLowerLeg, LeftFoot, LeftToes);
    leg(RightUpperLeg, RightLowerLeg, RightFoot, RightToes);

    // ---- Ground the feet: match the lowest avatar ankle to the lowest source ankle.
    if (groundFeet && srcHas(LeftFoot) && srcHas(RightFoot)) {
        float srcLow = std::min(src.at(LeftFoot).y, src.at(RightFoot).y);
        float avLow = std::min(bonePosition(LeftFoot).y, bonePosition(RightFoot).y);
        Vec3 p = world_[hips].translation();
        p.y += srcLow - avLow;
        placeHips(p);
    }
    return true;
}

} // namespace rfv::vrm
