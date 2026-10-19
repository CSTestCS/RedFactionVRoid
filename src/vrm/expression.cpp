#include "expression.hpp"

#include <algorithm>
#include <cmath>

namespace rfv::vrm {

namespace {
bool isMechanical(const std::string& n) {
    // Visemes, blinks and gaze are driven automatically, not chosen as a mood.
    static const char* skip[] = {"aa", "ih", "ou", "ee", "oh", "blink", "blinkLeft", "blinkRight",
                                 "lookUp", "lookDown", "lookLeft", "lookRight", "neutral"};
    for (const char* s : skip)
        if (n == s) return true;
    return false;
}
float approach(float v, float target, float rate, float dt) {
    if (v < target) return std::min(target, v + rate * dt);
    return std::max(target, v - rate * dt);
}
} // namespace

ExpressionController::ExpressionController(const Model& model, uint32_t seed) : model_(model), rng_(seed ? seed : 1) {
    blinkTimer_ = 1.0f + 3.0f * rand01();
}

float ExpressionController::rand01() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (rng_ & 0xFFFFFF) / float(0x1000000);
}

std::vector<std::string> ExpressionController::selectable() const {
    std::vector<std::string> out{"neutral"};
    for (const char* p : {"happy", "relaxed", "angry", "sad", "surprised"})
        if (has(p)) out.push_back(p);
    for (const auto& e : model_.expressions)
        if (!e.preset && !isMechanical(e.name) && !e.binds.empty()) out.push_back(e.name);
    return out;
}

void ExpressionController::setBase(const std::string& name, float weight) {
    std::string n = (name == "neutral") ? "" : name;
    if (n == baseName_) { baseTarget_ = n.empty() ? 0 : weight; return; }
    prevBaseName_ = baseName_;
    prevBaseWeight_ = baseWeight_;
    baseName_ = has(n) ? n : "";
    baseWeight_ = 0;
    baseTarget_ = baseName_.empty() ? 0 : weight;
}

void ExpressionController::trigger(const std::string& name, float hold, float weight) {
    if (!has(name)) return;
    if (reactName_ != name) reactWeight_ = 0;
    reactName_ = name;
    reactPeak_ = weight;
    reactHold_ = hold;
}

float ExpressionController::weightOf(const std::string& name) const {
    float react = reactName_.empty() ? 0 : reactWeight_;
    float w = 0;
    if (name == baseName_ && !name.empty()) w += baseWeight_ * (1 - react);
    if (name == prevBaseName_ && !name.empty()) w += prevBaseWeight_ * (1 - react);
    if (name == reactName_ && !name.empty()) w += react;
    if (name == "blink") w += blinkWeight_;
    return std::min(w, 1.0f);
}

void ExpressionController::addExpression(int idx, float w, std::vector<std::vector<float>>& weights) const {
    if (idx < 0 || w <= 0) return;
    const Expression& e = model_.expressions[idx];
    if (e.binary) w = w >= 0.5f ? 1.0f : 0.0f;
    for (const auto& b : e.binds) {
        if (b.mesh < 0 || b.mesh >= static_cast<int>(weights.size())) continue;
        auto& mw = weights[b.mesh];
        if (b.morph < 0 || b.morph >= static_cast<int>(mw.size())) continue;
        mw[b.morph] += w * b.weight;
    }
}

void ExpressionController::update(float dt, std::vector<std::vector<float>>& weights) {
    dt = std::clamp(dt, 0.0f, 0.25f);
    // Fades.
    baseWeight_ = approach(baseWeight_, baseTarget_, 4.0f, dt);
    prevBaseWeight_ = approach(prevBaseWeight_, 0.0f, 4.0f, dt);
    if (!reactName_.empty()) {
        if (reactHold_ > 0) {
            reactWeight_ = approach(reactWeight_, reactPeak_, 12.0f, dt);
            reactHold_ -= dt;
        } else {
            reactWeight_ = approach(reactWeight_, 0.0f, 3.0f, dt);
            if (reactWeight_ <= 0) reactName_.clear();
        }
    }

    // Blinking: close 0.06 s, hold 0.04 s, open 0.10 s; every 2-6 s, sometimes twice.
    if (autoBlink_ && has("blink")) {
        if (blinkPhase_ < 0) {
            blinkTimer_ -= dt;
            if (blinkTimer_ <= 0) {
                blinkPhase_ = 0;
                blinkTimer_ = rand01() < 0.15f ? 0.25f : 2.0f + 4.0f * rand01();
            }
            blinkWeight_ = 0;
        } else {
            blinkPhase_ += dt;
            const float c = 0.06f, h = 0.04f, o = 0.10f;
            if (blinkPhase_ < c) blinkWeight_ = blinkPhase_ / c;
            else if (blinkPhase_ < c + h) blinkWeight_ = 1;
            else if (blinkPhase_ < c + h + o) blinkWeight_ = 1 - (blinkPhase_ - c - h) / o;
            else { blinkWeight_ = 0; blinkPhase_ = -1; }
        }
    } else {
        blinkWeight_ = 0;
    }

    // Reset to the meshes' default weights.
    for (size_t m = 0; m < weights.size(); ++m) {
        auto& mw = weights[m];
        const auto& def = model_.meshes[m].defaultWeights;
        for (size_t t = 0; t < mw.size(); ++t) mw[t] = t < def.size() ? def[t] : 0.0f;
    }

    float react = reactName_.empty() ? 0 : reactWeight_;
    struct Active { int idx; float w; };
    std::vector<Active> active;
    auto push = [&](const std::string& n, float w) {
        if (!n.empty() && w > 0.001f) active.push_back({model_.findExpression(n), w});
    };
    push(baseName_, baseWeight_ * (1 - react));
    push(prevBaseName_, prevBaseWeight_ * (1 - react));
    push(reactName_, react);

    // Emotions with overrideBlink suppress (block) or reduce (blend) blinking.
    float blink = blinkWeight_;
    for (const auto& a : active) {
        if (a.idx < 0) continue;
        OverrideMode om = model_.expressions[a.idx].overrideBlink;
        if (om == OverrideMode::Block && a.w > 0) blink = 0;
        else if (om == OverrideMode::Blend) blink *= 1 - a.w;
    }
    for (const auto& a : active) addExpression(a.idx, a.w, weights);
    addExpression(model_.findExpression("blink"), blink, weights);

    for (auto& mw : weights)
        for (auto& w : mw) w = std::clamp(w, 0.0f, 1.0f);
}

bool applyMorphs(const Primitive& p, const std::vector<float>& weights, std::vector<Vec3>& outPos, std::vector<Vec3>& outNrm) {
    outPos = p.positions;
    outNrm = p.normals;
    bool any = false;
    for (size_t t = 0; t < p.targets.size() && t < weights.size(); ++t) {
        float w = weights[t];
        if (std::fabs(w) < 1e-4f) continue;
        const MorphTarget& mt = p.targets[t];
        if (mt.dPos.size() == outPos.size()) {
            any = true;
            for (size_t v = 0; v < outPos.size(); ++v) outPos[v] += mt.dPos[v] * w;
        }
        if (mt.dNrm.size() == outNrm.size())
            for (size_t v = 0; v < outNrm.size(); ++v) outNrm[v] += mt.dNrm[v] * w;
    }
    if (any)
        for (auto& n : outNrm) n = normalize(n);
    return any;
}

} // namespace rfv::vrm
