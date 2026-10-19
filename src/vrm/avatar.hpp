// Posable VRM avatar: rest-pose analysis, height normalisation and humanoid retargeting.
//
// All retargeting happens in a right-handed, Y-up, metre-based "avatar world" space
// (glTF's native convention). The caller converts game-space joint positions into this
// space (see game/binding.cpp) and converts the resulting matrices back for rendering.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "model.hpp"

namespace rfv::vrm {

// A pose sampled from the game character (Mason), in avatar world space.
struct SourcePose {
    bool valid = false;
    std::map<std::string, Vec3> joints;  // humanoid bone name -> joint world position
    bool hasHeadFrame = false;           // optional head orientation
    Vec3 headForward{0, 0, 1};
    Vec3 headUp{0, 1, 0};

    bool has(const std::string& b) const { return joints.count(b) != 0; }
    const Vec3& at(const std::string& b) const { return joints.at(b); }
};

struct RestMetrics {
    float minY = 0, maxY = 0;  // skinned mesh bounds in rest pose (unscaled)
    float height = 0;          // maxY - minY
    float headY = 0;           // head joint height
    float ankleY = 0;          // average ankle (foot joint) height
    float hipsY = 0;
};

class Avatar {
public:
    explicit Avatar(const Model& model);

    const Model& model() const { return model_; }
    const RestMetrics& rest() const { return rest_; }
    // Direction the character faces in its rest pose (VRM 1.0: +Z, VRM 0.x: -Z).
    const Vec3& restForward() const { return restForward_; }

    // Uniform scale applied to the whole avatar (1 = authored size).
    void setScale(float s) { scale_ = s; }
    float scale() const { return scale_; }
    // Scale so the avatar's total height (feet to top of head/hair) equals `targetHeight` metres.
    float scaleForHeight(float targetHeight) const;
    // Scale so the avatar's head-joint-above-ankle distance equals the source's.
    float scaleForJointSpan(float sourceHeadAboveAnkle) const;

    // Static poses (used by the menu preview). Feet rest on y = 0 at `position`.
    void setRestPose(const Vec3& position, float yawRadians, bool aPose);

    // Retarget a live game pose onto the avatar. Returns false if the pose lacks required joints.
    bool applySource(const SourcePose& src, bool groundFeet = true);

    // Node world matrices of the current pose (avatar world space, scale included).
    const std::vector<Mat4>& world() const { return world_; }
    // Skinning matrices for a skin: world(joint) * inverseBind(joint).
    void skinPalette(int skin, std::vector<Mat4>& out) const;
    // World position of a humanoid bone in the current pose (or origin if missing).
    Vec3 bonePosition(const std::string& bone) const;

private:
    const Model& model_;
    float scale_ = 1.0f;
    RestMetrics rest_;

    std::vector<Vec3> restT_;
    std::vector<Quat> restR_;
    std::vector<Vec3> restS_;
    std::vector<Mat4> restWorld_;   // unscaled, identity root
    std::vector<Quat> restWorldR_;  // world rotation of each node in rest pose
    std::vector<int> order_;        // nodes in parent-before-child order

    // current pose
    std::vector<Vec3> curT_;
    std::vector<Quat> curR_;
    std::vector<Mat4> world_;
    std::vector<Quat> worldR_;
    Mat4 root_;
    Quat rootRot_;

    Vec3 restForward_{0, 0, 1};  // character facing direction in rest pose
    Vec3 restLeft_{1, 0, 0};

    void resetToRest();
    void updateWorld();
    void updateWorldSubtree(int node);
    Vec3 restPos(int node) const { return restWorld_[node].translation(); }
    int hb(const char* name) const { return model_.humanBone(name); }
    // Rotate bone `node` (in world space) so its direction towards `childRest` points at `targetDir`,
    // optionally keeping a pole (secondary axis) aligned.
    void aimBone(int node, int childNode, const Vec3& targetDir, const Vec3* restPoleWorld, const Vec3* targetPole);
    void setWorldRotation(int node, const Quat& worldRot);
    void computeRestMetrics();
};

} // namespace rfv::vrm
