// CPU-side representation of a VRM (glTF 2.0 binary) avatar.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../core/math.hpp"

namespace rfv::vrm {

struct Node {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    Vec3 translation{0, 0, 0};
    Quat rotation{};
    Vec3 scale{1, 1, 1};
    int mesh = -1;
    int skin = -1;
};

struct Primitive {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;      // may be empty -> computed on load
    std::vector<float> uvs;         // 2 per vertex (may be empty)
    std::vector<uint16_t> joints;   // 4 per vertex (may be empty if unskinned)
    std::vector<float> weights;     // 4 per vertex
    std::vector<uint32_t> indices;  // triangle list
    int material = -1;
};

struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
};

struct Skin {
    std::vector<int> joints;           // node indices
    std::vector<Mat4> inverseBind;     // one per joint
};

struct Image {
    std::string name;
    std::string mimeType;
    std::vector<uint8_t> data;  // encoded PNG / JPEG bytes
};

struct Texture {
    int image = -1;
    bool nearest = false;
    bool clampU = false, clampV = false;
};

enum class AlphaMode { Opaque, Mask, Blend };

struct Material {
    std::string name;
    float baseColor[4] = {1, 1, 1, 1};
    int baseColorTexture = -1;  // texture index
    float shadeColor[3] = {0.75f, 0.75f, 0.8f};
    int shadeTexture = -1;
    float emissive[3] = {0, 0, 0};
    int emissiveTexture = -1;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;
    bool isMToon = false;
    int renderQueueOffset = 0;  // for sorting transparent materials
};

enum class VrmVersion { None, V0, V1 };

struct Meta {
    std::string title;
    std::string author;
    std::string version;
};

// VRM humanoid bone names (VRM 1.0 spelling; 0.x names are identical for the bones we use).
namespace bones {
constexpr const char* Hips = "hips";
constexpr const char* Spine = "spine";
constexpr const char* Chest = "chest";
constexpr const char* UpperChest = "upperChest";
constexpr const char* Neck = "neck";
constexpr const char* Head = "head";
constexpr const char* LeftShoulder = "leftShoulder";
constexpr const char* LeftUpperArm = "leftUpperArm";
constexpr const char* LeftLowerArm = "leftLowerArm";
constexpr const char* LeftHand = "leftHand";
constexpr const char* RightShoulder = "rightShoulder";
constexpr const char* RightUpperArm = "rightUpperArm";
constexpr const char* RightLowerArm = "rightLowerArm";
constexpr const char* RightHand = "rightHand";
constexpr const char* LeftUpperLeg = "leftUpperLeg";
constexpr const char* LeftLowerLeg = "leftLowerLeg";
constexpr const char* LeftFoot = "leftFoot";
constexpr const char* LeftToes = "leftToes";
constexpr const char* RightUpperLeg = "rightUpperLeg";
constexpr const char* RightLowerLeg = "rightLowerLeg";
constexpr const char* RightFoot = "rightFoot";
constexpr const char* RightToes = "rightToes";
constexpr const char* LeftMiddleProximal = "leftMiddleProximal";
constexpr const char* RightMiddleProximal = "rightMiddleProximal";
} // namespace bones

struct Model {
    VrmVersion version = VrmVersion::None;
    Meta meta;
    std::vector<Node> nodes;
    std::vector<int> roots;
    std::vector<Mesh> meshes;
    std::vector<Skin> skins;
    std::vector<Image> images;
    std::vector<Texture> textures;
    std::vector<Material> materials;
    std::map<std::string, int> humanoid;  // humanoid bone name -> node index

    int humanBone(const std::string& name) const {
        auto it = humanoid.find(name);
        return it == humanoid.end() ? -1 : it->second;
    }
};

// Loads a .vrm / .glb file from memory. Throws std::runtime_error on failure.
Model loadGlb(const uint8_t* data, size_t size);
Model loadFile(const std::string& utf8Path);

} // namespace rfv::vrm
