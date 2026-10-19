// Small vector / quaternion / matrix library.
// Conventions: column vectors (p' = M * p), Mat4 stored column-major (same as glTF).
#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>

namespace rfv {

constexpr float kPi = 3.14159265358979323846f;

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v, const Vec3& fallback = {0, 1, 0}) {
    float l = length(v);
    return l > 1e-8f ? v / l : fallback;
}
inline Vec3 vmin(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 vmax(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    Quat operator*(const Quat& b) const {
        return {w * b.x + x * b.w + y * b.z - z * b.y,
                w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w,
                w * b.w - x * b.x - y * b.y - z * b.z};
    }
    Quat conjugate() const { return {-x, -y, -z, w}; }
    Vec3 rotate(const Vec3& v) const {
        // v' = q v q*, optimised form
        Vec3 u{x, y, z};
        Vec3 t = cross(u, v) * 2.0f;
        return v + t * w + cross(u, t);
    }
    static Quat axisAngle(const Vec3& axis, float angle) {
        Vec3 a = normalize(axis);
        float s = std::sin(angle * 0.5f);
        return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
    }
};

inline Quat normalize(const Quat& q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-12f) return {};
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}
inline float dot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

inline Quat slerp(Quat a, Quat b, float t) {
    float d = dot(a, b);
    if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f) {
        return normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    }
    float th = std::acos(d);
    float s = std::sin(th);
    float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

// Shortest rotation taking unit vector a onto unit vector b.
inline Quat fromTo(const Vec3& a_, const Vec3& b_) {
    Vec3 a = normalize(a_), b = normalize(b_);
    float d = dot(a, b);
    if (d >= 1.0f - 1e-6f) return {};
    if (d <= -1.0f + 1e-6f) {
        Vec3 axis = cross(Vec3{1, 0, 0}, a);
        if (length(axis) < 1e-4f) axis = cross(Vec3{0, 1, 0}, a);
        return Quat::axisAngle(axis, kPi);
    }
    Vec3 c = cross(a, b);
    return normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

// Rotation from orthonormal basis columns (x, y, z axes).
inline Quat fromBasis(const Vec3& X, const Vec3& Y, const Vec3& Z) {
    float m00 = X.x, m01 = Y.x, m02 = Z.x;
    float m10 = X.y, m11 = Y.y, m12 = Z.y;
    float m20 = X.z, m21 = Y.z, m22 = Z.z;
    float tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return normalize(q);
}

// Rotation that maps (dirA, upA) frame onto (dirB, upB) frame. The "up" vectors only
// need to be roughly perpendicular to the directions; they are orthogonalised here.
inline Quat alignFrames(const Vec3& dirA, const Vec3& upA, const Vec3& dirB, const Vec3& upB) {
    auto basis = [](const Vec3& d, const Vec3& u, Vec3& X, Vec3& Y, Vec3& Z) {
        Y = normalize(d);
        Z = normalize(cross(Y, u), Vec3{0, 0, 1});
        X = cross(Y, Z);
    };
    Vec3 ax, ay, az, bx, by, bz;
    basis(dirA, upA, ax, ay, az);
    basis(dirB, upB, bx, by, bz);
    Quat qa = fromBasis(ax, ay, az);
    Quat qb = fromBasis(bx, by, bz);
    return normalize(qb * qa.conjugate());
}

struct Mat4 {
    float m[16]; // column-major: m[col*4 + row]
    Mat4() { identity(); }
    void identity() {
        std::memset(m, 0, sizeof(m));
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += at(rr, k) * b.at(k, c);
                r.at(rr, c) = s;
            }
        return r;
    }
    Vec3 transformPoint(const Vec3& p) const {
        return {at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3),
                at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3),
                at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3)};
    }
    Vec3 transformDir(const Vec3& p) const {
        return {at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z,
                at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z,
                at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z};
    }
    Vec3 translation() const { return {at(0, 3), at(1, 3), at(2, 3)}; }
    Vec3 column(int c) const { return {at(0, c), at(1, c), at(2, c)}; }

    static Mat4 fromTRS(const Vec3& t, const Quat& q, const Vec3& s) {
        Mat4 r;
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        r.at(0, 0) = (1 - 2 * (yy + zz)) * s.x; r.at(0, 1) = (2 * (xy - wz)) * s.y;     r.at(0, 2) = (2 * (xz + wy)) * s.z;
        r.at(1, 0) = (2 * (xy + wz)) * s.x;     r.at(1, 1) = (1 - 2 * (xx + zz)) * s.y; r.at(1, 2) = (2 * (yz - wx)) * s.z;
        r.at(2, 0) = (2 * (xz - wy)) * s.x;     r.at(2, 1) = (2 * (yz + wx)) * s.y;     r.at(2, 2) = (1 - 2 * (xx + yy)) * s.z;
        r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z;
        return r;
    }
    static Mat4 translate(const Vec3& t) { return fromTRS(t, {}, {1, 1, 1}); }
    static Mat4 scale(float s) { return fromTRS({}, {}, {s, s, s}); }
    static Mat4 scale(const Vec3& s) { return fromTRS({}, {}, s); }

    // Decompose (assumes no shear).
    void decompose(Vec3& t, Quat& q, Vec3& s) const {
        t = translation();
        Vec3 c0 = column(0), c1 = column(1), c2 = column(2);
        s = {length(c0), length(c1), length(c2)};
        if (dot(cross(c0, c1), c2) < 0) s.x = -s.x;
        q = fromBasis(c0 / (s.x != 0 ? s.x : 1), c1 / (s.y != 0 ? s.y : 1), c2 / (s.z != 0 ? s.z : 1));
    }

    Mat4 inverse() const {
        // General 4x4 inverse (cofactor expansion).
        const float* a = m;
        float inv[16];
        inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        Mat4 r;
        if (std::fabs(det) < 1e-20f) return r;
        float id = 1.0f / det;
        for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * id;
        return r;
    }

    Mat4 transposed() const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) r.at(i, j) = at(j, i);
        return r;
    }

    // Left-handed (D3D) look-to view matrix: camera looks down +Z in view space.
    static Mat4 viewLH(const Vec3& eye, const Vec3& right, const Vec3& up, const Vec3& fwd) {
        Mat4 r;
        r.at(0, 0) = right.x; r.at(0, 1) = right.y; r.at(0, 2) = right.z; r.at(0, 3) = -dot(right, eye);
        r.at(1, 0) = up.x;    r.at(1, 1) = up.y;    r.at(1, 2) = up.z;    r.at(1, 3) = -dot(up, eye);
        r.at(2, 0) = fwd.x;   r.at(2, 1) = fwd.y;   r.at(2, 2) = fwd.z;   r.at(2, 3) = -dot(fwd, eye);
        return r;
    }
    // Left-handed perspective, depth 0..1 (or reversed 1..0).
    static Mat4 perspectiveLH(float fovY, float aspect, float zn, float zf, bool reversed = false) {
        Mat4 r;
        std::memset(r.m, 0, sizeof(r.m));
        float h = 1.0f / std::tan(fovY * 0.5f);
        r.at(0, 0) = h / aspect;
        r.at(1, 1) = h;
        if (!reversed) {
            r.at(2, 2) = zf / (zf - zn);
            r.at(2, 3) = -zn * zf / (zf - zn);
        } else {
            r.at(2, 2) = zn / (zn - zf);
            r.at(2, 3) = -zn * zf / (zn - zf);
        }
        r.at(3, 2) = 1.0f;
        return r;
    }
};

} // namespace rfv
