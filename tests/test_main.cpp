// Host-side unit tests for the portable parts of RedFactionVRoid.
// usage: rfv_tests <dir containing test_v1.vrm, test_v0.vrm, expected.json>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>

#include "../src/core/ini.hpp"
#include "../src/core/json.hpp"
#include "../src/core/math.hpp"
#include "../src/game/binding.hpp"
#include "../src/game/pointer_path.hpp"
#include "../src/vrm/avatar.hpp"
#include "../src/vrm/expression.hpp"
#include "../src/vrm/spring.hpp"
#include "../src/vrm/model.hpp"

using namespace rfv;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) { ++g_failures; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define CHECK_NEAR(a, b, eps)                                                                              \
    do {                                                                                                   \
        ++g_checks;                                                                                        \
        double _a = (a), _b = (b);                                                                         \
        if (!(std::fabs(_a - _b) <= (eps))) {                                                              \
            ++g_failures;                                                                                  \
            std::printf("  FAIL %s:%d: %s = %g, expected %g\n", __FILE__, __LINE__, #a, _a, _b);           \
        }                                                                                                  \
    } while (0)

static std::string g_dir;

static void testJson() {
    auto v = json::parse(R"({"a":[1,2.5,-3e2],"b":{"c":"x\u00e9\n"},"t":true,"n":null})");
    CHECK(v["a"].size() == 3);
    CHECK_NEAR(v["a"][2].num(), -300, 1e-9);
    CHECK(v["b"]["c"].str() == "x\xC3\xA9\n");
    CHECK(v["t"].boolean());
    CHECK(v["n"].isNull());
    CHECK(v["missing"]["deep"].isNull());
    bool threw = false;
    try { json::parse("{\"a\":}"); } catch (...) { threw = true; }
    CHECK(threw);
}

static void testMath() {
    Quat q = Quat::axisAngle({0, 1, 0}, kPi / 2);
    Vec3 r = q.rotate({1, 0, 0});
    CHECK_NEAR(r.x, 0, 1e-5); CHECK_NEAR(r.z, -1, 1e-5);
    Quat f = fromTo({1, 0, 0}, {0, 0, 1});
    Vec3 r2 = f.rotate({1, 0, 0});
    CHECK_NEAR(r2.z, 1, 1e-5);
    Quat opp = fromTo({0, 1, 0}, {0, -1, 0});
    CHECK_NEAR(opp.rotate({0, 1, 0}).y, -1, 1e-5);
    Mat4 m = Mat4::fromTRS({1, 2, 3}, q, {2, 2, 2});
    Mat4 inv = m.inverse();
    Vec3 p = inv.transformPoint(m.transformPoint({0.3f, -0.7f, 5}));
    CHECK_NEAR(p.x, 0.3, 1e-4); CHECK_NEAR(p.y, -0.7, 1e-4); CHECK_NEAR(p.z, 5, 1e-4);
    Vec3 t; Quat dq; Vec3 s;
    m.decompose(t, dq, s);
    CHECK_NEAR(s.x, 2, 1e-5); CHECK_NEAR(std::fabs(dot(dq, q)), 1, 1e-5);
    // alignFrames maps both direction and secondary axis.
    Quat a = alignFrames({0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0});
    Vec3 d1 = a.rotate({0, 1, 0}), d2 = a.rotate({1, 0, 0});
    CHECK_NEAR(d1.z, 1, 1e-5); CHECK_NEAR(d2.y, 1, 1e-5);
}

static void testIni() {
    Ini ini;
    ini.loadString("; comment\n[Sec]\nKey = value ; trailing\nNum=0x10\nPath=\"C:\\a;b\"\nB=yes\n[Other]\nx=1\n");
    CHECK(ini.get("sec", "key") == "value");
    CHECK(ini.getInt("Sec", "Num", 0) == 16);
    CHECK(ini.get("Sec", "Path") == "C:\\a;b");
    CHECK(ini.getBool("Sec", "B", false));
    CHECK(ini.get("Sec", "missing", "d") == "d");
    ini.set("New", "k", "v");
    CHECK(ini.get("new", "K") == "v");
}

// Fake process memory for pointer-path and binding tests.
struct FakeMemory : game::MemoryAccess {
    std::map<uintptr_t, uint8_t> bytes;
    std::vector<uint8_t> code;  // "module" image for signature scans
    uintptr_t codeBase = 0x140000000;

    void put(uintptr_t a, const void* d, size_t n) {
        for (size_t i = 0; i < n; ++i) bytes[a + i] = static_cast<const uint8_t*>(d)[i];
    }
    template <typename T> void putv(uintptr_t a, T v) { put(a, &v, sizeof(T)); }
    bool read(uintptr_t a, void* out, size_t n) override {
        for (size_t i = 0; i < n; ++i) {
            if (a + i >= codeBase && a + i < codeBase + code.size()) { static_cast<uint8_t*>(out)[i] = code[a + i - codeBase]; continue; }
            auto it = bytes.find(a + i);
            if (it == bytes.end()) return false;
            static_cast<uint8_t*>(out)[i] = it->second;
        }
        return true;
    }
    bool write(uintptr_t a, const void* d, size_t n) override { put(a, d, n); return true; }
    uintptr_t moduleBase(const std::string& name) override { return toLower(name) == "rfg.exe" ? codeBase : 0; }
    uintptr_t findPattern(const std::vector<int>& pat) override {
        for (size_t i = 0; i + pat.size() <= code.size(); ++i) {
            bool ok = true;
            for (size_t k = 0; k < pat.size() && ok; ++k) ok = pat[k] < 0 || code[i + k] == pat[k];
            if (ok) return codeBase + i;
        }
        return 0;
    }
};

static void testPointerPath() {
    FakeMemory mem;
    mem.code.assign(0x100, 0x90);
    // mov rax, [rip+disp] at 0x20: 48 8B 05 <disp32>, target = 0x20+3+4+disp
    uint8_t ins[] = {0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0};
    int32_t disp = 0x1000 - (0x20 + 7);
    std::memcpy(ins + 3, &disp, 4);
    std::memcpy(mem.code.data() + 0x20, ins, sizeof(ins));
    mem.putv<uint64_t>(mem.codeBase + 0x1000, 0x5000);  // global -> object
    mem.putv<uint64_t>(0x5000 + 0x18, 0x9000);           // object->field -> bones

    uintptr_t out = 0;
    auto p1 = game::PointerPath::parse("[[rfg.exe+0x1000]+0x18]+0x30");
    CHECK(p1.resolve(mem, out) && out == 0x9030);
    auto p2 = game::PointerPath::parse("[[sig(\"48 8B 05 ?? ?? ?? ?? 48 85 C0\", 3)]+0x18]");
    CHECK(p2.resolve(mem, out) && out == 0x9000);
    auto p3 = game::PointerPath::parse("sig(\"48 8B 05\")+2");
    CHECK(p3.resolve(mem, out) && out == mem.codeBase + 0x22);
    auto p4 = game::PointerPath::parse("[0x1234]");
    CHECK(!p4.resolve(mem, out));
    bool threw = false;
    try { game::PointerPath::parse("[rfg.exe+1A2B"); } catch (...) { threw = true; }
    CHECK(threw);
}

static json::Value expectedJson(const std::string& file) {
    std::ifstream in(g_dir + "/expected.json");
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str())[file];
}

static std::map<std::string, double> expectedFor(const std::string& file) {
    auto v = expectedJson(file);
    std::map<std::string, double> m;
    for (auto& kv : v.members())
        if (kv.second.isNumber()) m[kv.first] = kv.second.num();
    return m;
}

// Skinned-vertex bounds of the current pose.
static void poseBounds(const vrm::Avatar& av, Vec3& mn, Vec3& mx) {
    const auto& m = av.model();
    mn = {1e9f, 1e9f, 1e9f};
    mx = {-1e9f, -1e9f, -1e9f};
    std::vector<Mat4> pal;
    for (const auto& node : m.nodes) {
        if (node.mesh < 0 || node.skin < 0) continue;
        av.skinPalette(node.skin, pal);
        for (const auto& p : m.meshes[node.mesh].primitives)
            for (size_t v = 0; v < p.positions.size(); ++v) {
                Vec3 pos;
                for (int k = 0; k < 4; ++k) pos += pal[p.joints[v * 4 + k]].transformPoint(p.positions[v]) * p.weights[v * 4 + k];
                mn = vmin(mn, pos);
                mx = vmax(mx, pos);
            }
    }
}

static const char* kAllBones[] = {
    "hips", "spine", "chest", "upperChest", "neck", "head",
    "leftShoulder", "leftUpperArm", "leftLowerArm", "leftHand", "leftMiddleProximal",
    "rightShoulder", "rightUpperArm", "rightLowerArm", "rightHand", "rightMiddleProximal",
    "leftUpperLeg", "leftLowerLeg", "leftFoot", "leftToes", "rightUpperLeg", "rightLowerLeg", "rightFoot", "rightToes"};

static void testAvatar(const std::string& file, bool v1) {
    std::printf("  avatar %s\n", file.c_str());
    vrm::Model model = vrm::loadFile(g_dir + "/" + file);
    auto exp = expectedFor(file);
    CHECK(model.version == (v1 ? vrm::VrmVersion::V1 : vrm::VrmVersion::V0));
    CHECK(static_cast<int>(model.humanoid.size()) == static_cast<int>(exp["humanBones"]));
    CHECK(model.meta.title == (v1 ? "Test Avatar 1.0" : "Test Avatar 0.x"));
    CHECK(model.images.size() == 1 && model.images[0].mimeType == "image/png" && model.images[0].data.size() > 8);
    CHECK(model.materials.size() == 1 && model.materials[0].isMToon);
    CHECK_NEAR(model.materials[0].shadeColor[0], 0.5, 1e-5);
    if (!v1) CHECK(model.materials[0].doubleSided);  // _CullMode = 0

    vrm::Avatar av(model);
    CHECK_NEAR(av.rest().height, exp["height"], 1e-4);
    CHECK_NEAR(av.rest().headY, exp["headY"], 1e-4);
    CHECK_NEAR(av.rest().ankleY, exp["ankleY"], 1e-4);

    // Height normalisation: whole model becomes exactly the target height, feet on the floor.
    const float masonHeight = 1.83f;
    av.setScale(av.scaleForHeight(masonHeight));
    av.setRestPose({5, 0, -2}, 0.7f, true);
    Vec3 mn, mx;
    poseBounds(av, mn, mx);
    CHECK_NEAR(mx.y - mn.y, masonHeight, 0.08);  // A-pose lowers the arms, top/bottom unchanged
    CHECK_NEAR(mn.y, 0.0, 1e-3);
    av.setRestPose({0, 0, 0}, 0, false);
    poseBounds(av, mn, mx);
    CHECK_NEAR(mx.y - mn.y, masonHeight, 1e-3);

    // Identity retarget: feeding the avatar's own rest joints back must reproduce them.
    vrm::SourcePose src;
    src.valid = true;
    for (const char* b : kAllBones) src.joints[b] = av.bonePosition(b);
    std::map<std::string, Vec3> before = src.joints;
    CHECK(av.applySource(src));
    for (const char* b : kAllBones) {
        Vec3 d = av.bonePosition(b) - before[b];
        CHECK_NEAR(length(d), 0, 2e-3);
    }

    // Moved + rotated source (character turned 90 degrees, walked, arm and knee bent).
    Quat turn = Quat::axisAngle({0, 1, 0}, kPi / 2);
    Vec3 offset{10, 3, -4};
    vrm::SourcePose s2;
    s2.valid = true;
    for (const char* b : kAllBones) s2.joints[b] = turn.rotate(before[b]) + offset;
    // Bend the left elbow 90 degrees: forearm and hand point forward from the elbow.
    Vec3 fwd = turn.rotate(v1 ? Vec3{0, 0, 1} : Vec3{0, 0, -1});
    Vec3 elbow = s2.joints["leftLowerArm"];
    float fore = length(before["leftHand"] - before["leftLowerArm"]);
    s2.joints["leftHand"] = elbow + fwd * fore;
    s2.joints["leftMiddleProximal"] = s2.joints["leftHand"] + fwd * 0.08f * av.scale();
    CHECK(av.applySource(s2));
    auto dirOf = [&](const char* a, const char* b) { return normalize(av.bonePosition(b) - av.bonePosition(a)); };
    auto srcDir = [&](const char* a, const char* b) { return normalize(s2.joints[b] - s2.joints[a]); };
    for (auto pr : std::vector<std::pair<const char*, const char*>>{
             {"hips", "spine"}, {"spine", "chest"}, {"neck", "head"}, {"leftUpperArm", "leftLowerArm"},
             {"leftLowerArm", "leftHand"}, {"rightLowerArm", "rightHand"}, {"leftUpperLeg", "leftLowerLeg"},
             {"rightLowerLeg", "rightFoot"}, {"leftFoot", "leftToes"}}) {
        CHECK_NEAR(dot(dirOf(pr.first, pr.second), srcDir(pr.first, pr.second)), 1.0, 1e-3);
    }
    CHECK_NEAR(length(av.bonePosition("hips") - s2.joints["hips"]), 0, 2e-3);
    CHECK_NEAR(length(av.bonePosition("leftHand") - s2.joints["leftHand"]), 0, 2e-3);
    // Facing direction follows the source: avatar's left side ends up where the source's is.
    Vec3 avLeft = av.bonePosition("leftUpperLeg") - av.bonePosition("rightUpperLeg");
    Vec3 srcLeft = s2.joints["leftUpperLeg"] - s2.joints["rightUpperLeg"];
    CHECK_NEAR(dot(normalize(avLeft), normalize(srcLeft)), 1.0, 1e-3);
}

static void testCrossRigRetarget() {
    // A VRM 0.x avatar driven by a VRM 1.0-oriented skeleton of a different size,
    // like Mason driving an arbitrary avatar.
    vrm::Model m0 = vrm::loadFile(g_dir + "/test_v0.vrm");
    vrm::Model m1 = vrm::loadFile(g_dir + "/test_v1.vrm");
    vrm::Avatar a0(m0), a1(m1);
    a1.setScale(1.3f);
    a1.setRestPose({0, 0, 0}, 0, true);
    vrm::SourcePose src;
    src.valid = true;
    for (const char* b : kAllBones) src.joints[b] = a1.bonePosition(b);
    // Scale target by joint span (like measuring Mason in-game).
    float span = src.joints["head"].y - std::min(src.joints["leftFoot"].y, src.joints["rightFoot"].y);
    a0.setScale(a0.scaleForJointSpan(span));
    CHECK_NEAR(a0.scale(), 1.3, 1e-3);
    CHECK(a0.applySource(src));
    // VRM 0.x avatar faces -Z natively; after retargeting it must face +Z like the source.
    Vec3 l0 = a0.bonePosition("leftUpperLeg") - a0.bonePosition("rightUpperLeg");
    CHECK(l0.x > 0.1f);
    CHECK_NEAR(dot(normalize(a0.bonePosition("leftHand") - a0.bonePosition("leftLowerArm")),
                   normalize(src.joints["leftHand"] - src.joints["leftLowerArm"])), 1.0, 1e-3);
    // Feet grounded to the source's feet.
    CHECK_NEAR(std::min(a0.bonePosition("leftFoot").y, a0.bonePosition("rightFoot").y),
               std::min(src.joints["leftFoot"].y, src.joints["rightFoot"].y), 1e-3);
}

static void testBinding() {
    // Simulated game: Volition matrix43 bones, left-handed world, player facing +Z (game space).
    FakeMemory mem;  // no code image: the module range is plain data here
    const uintptr_t bonesAddr = 0x20000;
    mem.putv<uint64_t>(mem.codeBase + 0x8, 0x10000);   // player pointer global
    mem.putv<uint64_t>(0x10000 + 0x40, bonesAddr);     // player->bones
    struct B { const char* name; Vec3 p; };
    std::vector<B> bones = {{"hips", {0, 1, 0}}, {"spine", {0, 1.2f, 0}}, {"head", {0, 1.7f, 0.02f}},
                            {"lUpLeg", {-0.1f, 0.95f, 0}}, {"rUpLeg", {0.1f, 0.95f, 0}}};
    // NOTE: in a left-handed game world facing +Z, the character's left is -X.
    for (size_t i = 0; i < bones.size(); ++i) {
        float m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, bones[i].p.x, bones[i].p.y, bones[i].p.z};
        mem.put(bonesAddr + i * 48, m, sizeof(m));
    }
    const char* names[] = {"pelvis", "spine1", "head", "l_thigh", "r_thigh"};
    for (size_t i = 0; i < 5; ++i) {
        uintptr_t s = 0x30000 + i * 32;
        mem.put(s, names[i], std::strlen(names[i]) + 1);
        mem.putv<uint64_t>(0x31000 + i * 8, s);
    }
    Ini ini;
    ini.loadString(
        "[GameSkeleton]\nBoneArray=[[rfg.exe+0x8]+0x40]\nBoneCount=5\nBoneNameArray=0x31000\n"
        "[BoneMap]\nhips=0\nspine=spine1\nhead=head\nleftUpperLeg=l_thigh\nrightUpperLeg=r_thigh\n"
        "[GameHide]\nWrite1=0x50000 | u8 | 0\nUnrelated=ignored\n"
        "[GamePause]\nFlag=0x60000\nPausedValue=1\n");
    std::string errors;
    game::GameBinding gb;
    gb.load(ini, errors);
    CHECK(errors.empty());
    if (!errors.empty()) std::printf("%s", errors.c_str());
    vrm::SourcePose sp;
    CHECK(gb.samplePose(mem, sp));
    CHECK(sp.joints.size() == 5);
    CHECK_NEAR(sp.joints["head"].z, -0.02, 1e-6);  // LH -> RH flips Z
    CHECK(sp.hasHeadFrame);
    CHECK_NEAR(sp.headForward.z, -1, 1e-6);
    // In avatar (RH) space the character must face -Z with its left at -X: consistent chirality.
    Vec3 left = sp.joints["leftUpperLeg"] - sp.joints["rightUpperLeg"];
    Vec3 up{0, 1, 0};
    Vec3 fwd = cross(normalize(left), up);
    CHECK_NEAR(fwd.z, -1, 1e-5);

    mem.putv<uint8_t>(0x50000, 1);
    gb.applyHide(mem, true);
    uint8_t v = 9;
    mem.get(0x50000, v);
    CHECK(v == 0);
    gb.applyHide(mem, false);
    mem.get(0x50000, v);
    CHECK(v == 1);

    mem.putv<uint8_t>(0x60000, 1);
    CHECK(gb.readPaused(mem) == 1);
    mem.putv<uint8_t>(0x60000, 0);
    CHECK(gb.readPaused(mem) == 0);

    // Optional health and sun values.
    {
        FakeMemory m2;
        m2.putv<float>(0x70000, 87.5f);
        float sun[3] = {0, 2, 2};       // towards the sun, game space (not normalised)
        float col[3] = {2.0f, 1.0f, 0.5f};
        m2.put(0x71000, sun, sizeof(sun));
        m2.put(0x72000, col, sizeof(col));
        Ini i2;
        i2.loadString("[GameState]\nHealth=0x70000\n[GameLighting]\nSunDirection=0x71000\nSunColor=0x72000\n");
        std::string e2;
        game::GameBinding b2;
        b2.load(i2, e2);
        CHECK(e2.empty());
        CHECK_NEAR(b2.readHealth(m2), 87.5, 1e-6);
        Vec3 d, c;
        CHECK(b2.readSun(m2, d, c));
        CHECK_NEAR(d.y, 0.70710678, 1e-5);
        CHECK_NEAR(d.z, -0.70710678, 1e-5);  // LH -> RH
        CHECK_NEAR(c.x, 1.0, 1e-6);
        CHECK_NEAR(c.z, 0.25, 1e-6);
    }

    // Avatar -> game conversion flips Z back.
    Vec3 g = gb.avatarToGame().transformPoint(sp.joints["head"]);
    CHECK_NEAR(g.z, 0.02, 1e-6);
}


static void testExpressions(const std::string& file, bool v1) {
    std::printf("  expressions %s\n", file.c_str());
    vrm::Model model = vrm::loadFile(g_dir + "/" + file);
    auto exp = expectedFor(file);
    CHECK(model.meshes.size() == 1 && model.meshes[0].primitives[0].targets.size() == 2);
    int blink = model.findExpression("blink"), happy = model.findExpression("happy"), smug = model.findExpression(v1 ? "smug" : "Smug");  // 0.x keeps custom names as authored
    CHECK(blink >= 0 && happy >= 0 && smug >= 0);  // VRM 0.x "joy" is renamed to "happy"
    if (happy < 0 || smug < 0 || blink < 0) return;
    CHECK(model.expressions[happy].preset && !model.expressions[smug].preset);
    CHECK_NEAR(model.expressions[smug].binds[0].weight, 0.5, 1e-6);  // VRM 0.x weights are 0..100
    CHECK(model.expressions[happy].overrideBlink == (v1 ? vrm::OverrideMode::Block : vrm::OverrideMode::Blend));

    // Morph application moves the head-top vertex.
    int top = static_cast<int>(exp["headTopVertex"]);
    const auto& prim = model.meshes[0].primitives[0];
    std::vector<Vec3> pos, nrm;
    CHECK(vrm::applyMorphs(prim, {1.0f, 0.0f}, pos, nrm));
    CHECK_NEAR(pos[top].y - prim.positions[top].y, -0.05, 1e-6);
    CHECK(!vrm::applyMorphs(prim, {0.0f, 0.0f}, pos, nrm));

    vrm::Avatar av(model);
    auto& w = av.morphWeights();
    CHECK(w.size() == 1 && w[0].size() == 2);
    vrm::ExpressionController ec(model, 7);
    auto sel = ec.selectable();
    CHECK(sel.size() == 3 && sel[0] == "neutral" && sel[1] == "happy" && sel[2] == (v1 ? "smug" : "Smug"));

    // Automatic blinking over 20 seconds: a plausible number of full blinks.
    int blinks = 0;
    bool closed = false;
    for (int f = 0; f < 1200; ++f) {
        ec.update(1.0f / 60, w);
        bool c = w[0][0] > 0.99f;
        if (c && !closed) ++blinks;
        closed = c;
    }
    CHECK(blinks >= 3 && blinks <= 14);

    // Resting expression fades in; while it is on, blinking is suppressed (block) or reduced (blend).
    ec.setBase("happy");
    for (int f = 0; f < 30; ++f) ec.update(1.0f / 60, w);
    CHECK_NEAR(ec.weightOf("happy"), 1.0, 1e-4);
    CHECK_NEAR(w[0][1], 1.0, 1e-4);
    float maxBlink = 0;
    for (int f = 0; f < 600; ++f) { ec.update(1.0f / 60, w); maxBlink = std::max(maxBlink, w[0][0]); }
    CHECK(maxBlink < 1e-4f);

    // A reaction overrides the resting face, then hands back to it.
    const std::string smugName = v1 ? "smug" : "Smug";
    ec.trigger(smugName, 0.5f);
    for (int f = 0; f < 15; ++f) ec.update(1.0f / 60, w);
    CHECK(ec.weightOf(smugName) > 0.95f);
    CHECK(ec.weightOf("happy") < 0.05f);
    for (int f = 0; f < 120; ++f) ec.update(1.0f / 60, w);
    CHECK_NEAR(ec.weightOf(smugName), 0.0, 1e-4);
    CHECK_NEAR(ec.weightOf("happy"), 1.0, 1e-4);
    ec.setBase("neutral");
    for (int f = 0; f < 30; ++f) ec.update(1.0f / 60, w);
    CHECK_NEAR(w[0][1], 0.0, 1e-4);
}

static void testSprings(const std::string& file, bool v1) {
    std::printf("  springs %s\n", file.c_str());
    vrm::Model model = vrm::loadFile(g_dir + "/" + file);
    auto exp = expectedJson(file);
    CHECK(static_cast<int>(model.springJoints.size()) == exp["springJoints"].integer());
    CHECK(model.colliders.size() == 1 && model.colliderGroups.size() == 1);
    vrm::Avatar av(model);
    av.setScale(1.2f);
    vrm::SpringSystem springs(av);
    CHECK(springs.jointCount() == model.springJoints.size());

    int head = model.humanBone("head");
    int hair1 = -1;
    for (size_t i = 0; i < model.nodes.size(); ++i)
        if (model.nodes[i].name == "hair1") hair1 = static_cast<int>(i);
    auto restTail = [&](size_t j) { return springs.tail(j); };

    // 1) Standing still: the ponytail settles near its rest shape and keeps its length.
    av.setRestPose({0, 0, 0}, 0, false);
    springs.update(1.0f / 60);
    Vec3 tail0 = restTail(0);
    float len0 = length(tail0 - av.nodeWorld(hair1).translation());
    for (int f = 0; f < 180; ++f) { av.setRestPose({0, 0, 0}, 0, false); springs.update(1.0f / 60); }
    CHECK(length(restTail(0) - tail0) < 0.05f);
    CHECK_NEAR(length(restTail(0) - av.nodeWorld(hair1).translation()), len0, 1e-4);

    // 2) Running sideways: the hair lags behind the head, then swings back when stopping.
    float x = 0;
    for (int f = 0; f < 20; ++f) {
        x += 0.08f;  // ~4.8 m/s
        av.setRestPose({x, 0, 0}, 0, false);
        springs.update(1.0f / 60);
    }
    float lag = restTail(1).x - av.nodeWorld(hair1).translation().x;
    CHECK(lag < -0.01f);
    for (int f = 0; f < 300; ++f) { av.setRestPose({x, 0, 0}, 0, false); springs.update(1.0f / 60); }
    float settled = restTail(1).x - av.nodeWorld(hair1).translation().x;
    CHECK(std::fabs(settled) < 0.02f);

    // 3) A strong force pushing the hair into the face: it must slide around the head
    //    collider, never through it.
    Vec3 fwd = av.restForward();
    springs.setExternalForce(fwd * 30.0f);
    const Mat4& hw = av.nodeWorld(head);
    float minClear = 1e9f;
    for (int f = 0; f < 240; ++f) {
        av.setRestPose({x, 0, 0}, 0, false);
        springs.update(1.0f / 60);
        Vec3 center = av.nodeWorld(head).transformPoint(model.colliders[0].offset);
        float r = (model.colliders[0].radius + 0.02f) * av.scale();
        for (size_t j = 0; j < springs.jointCount(); ++j) minClear = std::min(minClear, length(springs.tail(j) - center) - r);
    }
    (void)hw;
    if (minClear <= -1e-3f) std::printf("  minClear = %f\n", minClear);
    CHECK(minClear > -1e-3f);
    // The force did move the hair forward (relative to rest).
    Vec3 rel = springs.tail(springs.jointCount() - 1) - av.nodeWorld(hair1).translation();
    CHECK(dot(rel, fwd) > -0.05f);

    // 4) A teleport (respawn) resets instead of flinging the hair across the map.
    springs.setExternalForce({});
    av.setRestPose({x + 50, 0, 0}, 0, false);
    springs.update(1.0f / 60);
    CHECK(length(springs.tail(0) - av.nodeWorld(hair1).translation()) < 0.5f);
    (void)v1;
}

int main(int argc, char** argv) {
    g_dir = argc > 1 ? argv[1] : ".";
    std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"json", testJson},
        {"math", testMath},
        {"ini", testIni},
        {"pointer_path", testPointerPath},
        {"avatar_v1", [] { testAvatar("test_v1.vrm", true); }},
        {"avatar_v0", [] { testAvatar("test_v0.vrm", false); }},
        {"cross_rig", testCrossRigRetarget},
        {"binding", testBinding},
        {"expressions_v1", [] { testExpressions("test_v1.vrm", true); }},
        {"expressions_v0", [] { testExpressions("test_v0.vrm", false); }},
        {"springs_v1", [] { testSprings("test_v1.vrm", true); }},
        {"springs_v0", [] { testSprings("test_v0.vrm", false); }},
    };
    for (auto& t : tests) {
        std::printf("[%s]\n", t.first);
        try {
            t.second();
        } catch (const std::exception& e) {
            ++g_failures;
            std::printf("  EXCEPTION: %s\n", e.what());
        }
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
