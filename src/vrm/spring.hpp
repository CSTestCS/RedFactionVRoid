// VRM spring bones: secondary motion for hair, skirts, ears, accessories.
// Implements the standard VRM algorithm (as in UniVRM): Verlet integration of each bone's
// tail with stiffness (return to rest direction), gravity and drag, a fixed bone length,
// and sphere / capsule colliders attached to the body.
#pragma once
#include <vector>

#include "avatar.hpp"

namespace rfv::vrm {

class SpringSystem {
public:
    explicit SpringSystem(Avatar& avatar);
    bool empty() const { return joints_.empty(); }
    // Forget all motion; tails restart at the current pose.
    void reset();
    // Advance the simulation by `dt` seconds (fixed 60 Hz sub-steps) and write the
    // resulting rotations into the avatar's current pose. Call after posing the avatar.
    void update(float dt);
    // Extra wind / external force in avatar space (m/s^2-ish, like gravity).
    void setExternalForce(const Vec3& f) { external_ = f; }

    // Current tail position of joint i (for tests / debugging).
    Vec3 tail(size_t i) const { return state_[i].current; }
    size_t jointCount() const { return joints_.size(); }

private:
    struct State {
        Vec3 current, prev;
        bool init = false;
    };
    struct Joint {
        const SpringJoint* def;
        Vec3 tailLocal;  // tail position in the node's local space (unscaled rest)
        std::vector<int> colliders;
    };
    Avatar& avatar_;
    std::vector<Joint> joints_;
    std::vector<State> state_;
    Vec3 external_{};
    float accumulator_ = 0;
    Vec3 lastRootPos_{};
    bool haveRoot_ = false;

    void step(float dt);
};

} // namespace rfv::vrm
