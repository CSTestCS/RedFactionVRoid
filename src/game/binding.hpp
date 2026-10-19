// Reads Mason's skeleton, the camera and the pause state out of game memory, as described
// by the [Game*] sections of RFVRoid.ini. Nothing here is hard-coded to a game build: every
// address is a PointerPath so it can be updated for new patches without recompiling.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "../core/ini.hpp"
#include "../core/math.hpp"
#include "../vrm/avatar.hpp"
#include "pointer_path.hpp"

namespace rfv::game {

// How a 3D transform is laid out in memory.
enum class MatrixFormat {
    Matrix43,   // 12 floats: X axis, Y axis, Z axis, translation (Volition "matrix43")
    Float4x4,   // 16 floats: X,0  Y,0  Z,0  T,1   (D3D row-major / GL column-major)
    Float3x4,   // 12 floats: 3 rows of a column-vector matrix (translation in .w)
    Float4x4T,  // 16 floats: transposed Float4x4
};
bool parseMatrixFormat(const std::string& s, MatrixFormat& out);
int matrixFormatSize(MatrixFormat f);
Mat4 decodeMatrix(const float* f, MatrixFormat fmt);

struct CameraState {
    Mat4 view;   // game space -> view
    Mat4 proj;
    Vec3 position;
    float fovY = 1.2f;
};

class GameBinding {
public:
    // Parses configuration. Problems are appended to `errors` (one per line); the binding
    // stays usable for whatever parts are valid.
    void load(const Ini& ini, std::string& errors);

    bool hasSkeleton() const { return !boneArray_.empty(); }
    bool hasCamera() const { return !camera_.empty(); }
    bool hasPauseFlag() const { return !pauseFlag_.empty(); }
    bool hasHideWrites() const { return !hide_.empty(); }
    float unitsToMeters() const { return unitsToMeters_; }

    // Raw bone transforms in game world space.
    bool readBones(MemoryAccess& mem, std::vector<Mat4>& out);
    // Bone names, if a name table is configured.
    std::vector<std::string> readBoneNames(MemoryAccess& mem);
    // Mason's pose converted into VRM avatar space (right-handed, metres).
    bool samplePose(MemoryAccess& mem, vrm::SourcePose& out);
    bool readCamera(MemoryAccess& mem, float aspect, CameraState& out);
    // -1 = unknown / not configured, 0 = running, 1 = paused.
    int readPaused(MemoryAccess& mem);
    // Applies (hide=true) or restores (hide=false) the configured memory writes that hide Mason.
    void applyHide(MemoryAccess& mem, bool hide);

    // Converts avatar space (RH metres) to game world space.
    Mat4 avatarToGame() const;
    Vec3 gameToAvatar(const Vec3& p) const;
    Vec3 gameDirToAvatar(const Vec3& d) const;
    bool flipsHandedness() const { return leftHanded_; }

    const std::map<std::string, std::string>& boneMap() const { return boneMap_; }

private:
    PointerPath boneArray_;
    int boneStride_ = 48;
    int boneCount_ = 0;
    MatrixFormat boneFormat_ = MatrixFormat::Matrix43;
    bool bonesModelSpace_ = false;
    PointerPath playerTransform_;
    MatrixFormat playerFormat_ = MatrixFormat::Matrix43;
    PointerPath boneNames_;
    int boneNameStride_ = 8;
    std::map<std::string, std::string> boneMap_;  // humanoid bone -> index or name
    std::map<std::string, int> resolvedMap_;
    bool mapResolved_ = false;
    int headForwardAxis_ = 2, headUpAxis_ = 1;   // 0..2 = X/Y/Z
    float headForwardSign_ = 1, headUpSign_ = 1;
    bool leftHanded_ = true;
    float unitsToMeters_ = 1.0f;

    PointerPath camera_;
    MatrixFormat cameraFormat_ = MatrixFormat::Matrix43;
    PointerPath fov_;
    bool fovDegrees_ = true;
    bool fovHorizontal_ = false;
    float fovDefault_ = 70.0f;
    float zNear_ = 0.1f, zFar_ = 2000.0f;
    bool reversedZ_ = false;

    PointerPath pauseFlag_;
    int pauseSize_ = 1;
    long long pausedValue_ = 1;

    struct HideWrite {
        PointerPath path;
        int size = 1;
        bool isFloat = false;
        long long value = 0;
        float fvalue = 0;
        bool saved = false;
        uint8_t original[8] = {};
        uintptr_t savedAddr = 0;
    };
    std::vector<HideWrite> hide_;
    bool hidden_ = false;

    bool resolveBoneMap(MemoryAccess& mem);
};

} // namespace rfv::game
