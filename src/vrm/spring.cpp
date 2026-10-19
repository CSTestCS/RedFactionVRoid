#include "spring.hpp"

#include <algorithm>
#include <cmath>

namespace rfv::vrm {

namespace {
constexpr float kStep = 1.0f / 60.0f;
constexpr int kMaxSteps = 4;
constexpr float kVirtualTail = 0.07f;  // UniVRM's tail length for leaf bones

Vec3 closestOnSegment(const Vec3& p, const Vec3& a, const Vec3& b) {
    Vec3 ab = b - a;
    float len2 = dot(ab, ab);
    if (len2 < 1e-12f) return a;
    float t = std::clamp(dot(p - a, ab) / len2, 0.0f, 1.0f);
    return a + ab * t;
}
} // namespace

SpringSystem::SpringSystem(Avatar& avatar) : avatar_(avatar) {
    const Model& m = avatar.model();
    for (const SpringJoint& sj : m.springJoints) {
        if (sj.node < 0 || sj.node >= avatar.nodeCount()) continue;
        Joint j{&sj, {}, {}};
        if (sj.tailNode >= 0 && sj.tailNode < avatar.nodeCount()) {
            // Tail position expressed in the joint's local space (works even if the tail is
            // not a direct child).
            Mat4 inv = avatar.restWorld(sj.node).inverse();
            j.tailLocal = inv.transformPoint(avatar.restWorld(sj.tailNode).translation());
        } else {
            // Leaf bone: extend the parent->bone direction by 7 cm.
            int parent = m.nodes[sj.node].parent;
            Vec3 pos = avatar.restWorld(sj.node).translation();
            Vec3 dir = parent >= 0 ? pos - avatar.restWorld(parent).translation() : Vec3{0, -1, 0};
            Vec3 tailWorld = pos + normalize(dir, {0, -1, 0}) * kVirtualTail;
            j.tailLocal = avatar.restWorld(sj.node).inverse().transformPoint(tailWorld);
        }
        if (length(j.tailLocal) < 1e-5f) continue;
        for (int g : sj.colliderGroups)
            for (int c : m.colliderGroups[g])
                if (m.colliders[c].node >= 0 && m.colliders[c].node < avatar.nodeCount()) j.colliders.push_back(c);
        joints_.push_back(std::move(j));
    }
    state_.resize(joints_.size());
}

void SpringSystem::reset() {
    for (auto& s : state_) s.init = false;
    accumulator_ = 0;
}

void SpringSystem::update(float dt) {
    if (joints_.empty()) return;
    // Teleports (respawn, menu preview <-> world) would fling everything: restart instead.
    int hips = avatar_.model().humanBone(bones::Hips);
    if (hips >= 0) {
        Vec3 root = avatar_.nodeWorld(hips).translation();
        if (haveRoot_ && length(root - lastRootPos_) > 2.0f) reset();
        lastRootPos_ = root;
        haveRoot_ = true;
    }
    if (dt > 0.5f) reset();
    accumulator_ = std::min(accumulator_ + std::max(dt, 0.0f), kStep * kMaxSteps);
    int steps = 0;
    while (accumulator_ >= kStep && steps < kMaxSteps) {
        step(kStep);
        accumulator_ -= kStep;
        ++steps;
    }
    if (steps == 0) step(0.0f);  // still apply the current state to this frame's pose
}

void SpringSystem::step(float dt) {
    const Model& m = avatar_.model();
    for (size_t i = 0; i < joints_.size(); ++i) {
        const Joint& j = joints_[i];
        const SpringJoint& d = *j.def;
        State& st = state_[i];
        int node = d.node;

        // Rest orientation of this bone given its parent's current orientation.
        Quat restRot = normalize(avatar_.parentWorldRotation(node) * avatar_.restLocalRotation(node));
        const Mat4& w = avatar_.nodeWorld(node);
        Vec3 head = w.translation();
        // Bone length in world units (includes the avatar's scale).
        float len = length(w.transformDir(j.tailLocal));
        if (len < 1e-6f) continue;
        Vec3 restDir = restRot.rotate(normalize(j.tailLocal));
        float worldScale = len / length(j.tailLocal);

        if (!st.init) {
            st.current = st.prev = head + restDir * len;
            st.init = true;
        }
        Vec3 next = st.current;
        if (dt > 0) {
            Vec3 inertia = (st.current - st.prev) * (1.0f - d.dragForce);
            Vec3 stiffness = restDir * (d.stiffness * dt * worldScale);
            Vec3 external = (normalize(d.gravityDir, {0, -1, 0}) * d.gravityPower + external_) * (dt * worldScale);
            next = st.current + inertia + stiffness + external;
        }
        next = head + normalize(next - head, restDir) * len;

        // Collisions: push the tail out of each collider (sphere or capsule), then restore
        // the bone length. Restoring the length can pull the tail slightly back inside, so a
        // few iterations are used (a single pass, as in UniVRM, lets hair clip a few mm).
        for (int iter = 0; iter < 4 && !j.colliders.empty(); ++iter) {
            bool hit = false;
            for (int ci : j.colliders) {
                const SpringCollider& c = m.colliders[ci];
                const Mat4& cw = avatar_.nodeWorld(c.node);
                float cr = c.radius * length(cw.transformDir({1, 0, 0}));
                float r = d.hitRadius * worldScale + cr;
                Vec3 center = cw.transformPoint(c.offset);
                if (c.capsule) center = closestOnSegment(next, center, cw.transformPoint(c.tail));
                Vec3 delta = next - center;
                if (length(delta) < r - 1e-5f) {
                    // Slightly over-push so the length constraint lands outside.
                    next = center + normalize(delta, restDir) * (r * (1.0f + 0.02f * (iter + 1)));
                    next = head + normalize(next - head, restDir) * len;
                    hit = true;
                }
            }
            if (!hit) break;
        }

        if (dt > 0) {
            st.prev = st.current;
            st.current = next;
        }
        Quat swing = fromTo(restDir, next - head);
        avatar_.setNodeWorldRotation(node, normalize(swing * restRot));
    }
}

} // namespace rfv::vrm
