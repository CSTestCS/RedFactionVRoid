// Facial expressions: a chosen resting expression, short reactions triggered by game
// events (e.g. getting hurt), and automatic blinking. Produces morph-target weights.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "model.hpp"

namespace rfv::vrm {

class ExpressionController {
public:
    explicit ExpressionController(const Model& model, uint32_t seed = 12345);

    // Expressions a player can choose as the resting face (emotions + custom ones).
    std::vector<std::string> selectable() const;
    bool has(const std::string& name) const { return model_.findExpression(name) >= 0; }

    // Resting expression ("neutral" or "" = none). Fades over ~0.25 s.
    void setBase(const std::string& name, float weight = 1.0f);
    const std::string& base() const { return baseName_; }
    // Temporary expression, e.g. trigger("angry", 0.8f) when hurt. Overrides the base.
    void trigger(const std::string& name, float holdSeconds, float weight = 1.0f);
    void setAutoBlink(bool on) { autoBlink_ = on; }

    // Advances timers and writes weights[mesh][target] (sized like Avatar::morphWeights()).
    void update(float dt, std::vector<std::vector<float>>& weights);

    // Current effective weight of an expression (after fades), for tests.
    float weightOf(const std::string& name) const;

private:
    const Model& model_;
    uint32_t rng_;
    float rand01();

    std::string baseName_;
    float baseTarget_ = 0, baseWeight_ = 0;
    std::string prevBaseName_;
    float prevBaseWeight_ = 0;

    std::string reactName_;
    float reactWeight_ = 0, reactPeak_ = 1, reactHold_ = 0;

    bool autoBlink_ = true;
    float blinkTimer_ = 2.0f;   // seconds until the next blink
    float blinkPhase_ = -1;     // <0: not blinking, else seconds into the blink
    float blinkWeight_ = 0;

    void addExpression(int idx, float w, std::vector<std::vector<float>>& weights) const;
};

// Morphs one primitive: out = base + sum(weight[t] * delta[t]) for positions and normals.
// `weights` is indexed by morph target; zero weights are skipped. Returns false if no
// target is active (outputs then equal the base data).
bool applyMorphs(const Primitive& p, const std::vector<float>& weights, std::vector<Vec3>& outPos, std::vector<Vec3>& outNrm);

} // namespace rfv::vrm
