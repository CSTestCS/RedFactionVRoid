#!/usr/bin/env python3
"""Generate small synthetic humanoid VRM files for automated tests.

Creates a stick-figure avatar (one skinned box per bone segment) in both VRM 1.0
(facing +Z) and VRM 0.x (facing -Z) flavours, plus an expected.json with the values
the C++ tests compare against.

usage: make_test_vrm.py <output-dir>
"""
import json
import os
import struct
import sys
import zlib


def png_1x1(r, g, b, a=255):
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"\x00" + bytes([r, g, b, a])
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


# (name, parent, local offset) in a +Z-facing, +X-is-left layout (VRM 1.0 convention).
SKELETON = [
    ("Root", None, (0, 0, 0)),
    ("hips", "Root", (0, 0.90, 0)),
    ("spine", "hips", (0, 0.10, 0)),
    ("chest", "spine", (0, 0.15, 0)),
    ("upperChest", "chest", (0, 0.10, 0)),
    ("neck", "upperChest", (0, 0.12, 0)),
    ("head", "neck", (0, 0.10, 0)),
    ("leftShoulder", "upperChest", (0.05, 0.08, 0)),
    ("leftUpperArm", "leftShoulder", (0.10, 0, 0)),
    ("leftLowerArm", "leftUpperArm", (0.25, 0, 0)),
    ("leftHand", "leftLowerArm", (0.25, 0, 0)),
    ("leftMiddleProximal", "leftHand", (0.08, 0, 0)),
    ("rightShoulder", "upperChest", (-0.05, 0.08, 0)),
    ("rightUpperArm", "rightShoulder", (-0.10, 0, 0)),
    ("rightLowerArm", "rightUpperArm", (-0.25, 0, 0)),
    ("rightHand", "rightLowerArm", (-0.25, 0, 0)),
    ("rightMiddleProximal", "rightHand", (-0.08, 0, 0)),
    ("leftUpperLeg", "hips", (0.10, -0.05, 0)),
    ("leftLowerLeg", "leftUpperLeg", (0, -0.40, 0)),
    ("leftFoot", "leftLowerLeg", (0, -0.40, 0)),
    ("leftToes", "leftFoot", (0, -0.03, 0.12)),
    ("rightUpperLeg", "hips", (-0.10, -0.05, 0)),
    ("rightLowerLeg", "rightUpperLeg", (0, -0.40, 0)),
    ("rightFoot", "rightLowerLeg", (0, -0.40, 0)),
    ("rightToes", "rightFoot", (0, -0.03, 0.12)),
    # Ponytail behind the head: a spring-bone chain (not humanoid bones).
    ("hair1", "head", (0, 0.20, -0.13)),
    ("hair2", "hair1", (0, -0.12, -0.03)),
    ("hair3", "hair2", (0, -0.12, 0)),
]
NON_HUMAN = {"Root", "hair1", "hair2", "hair3"}
HEAD_COLLIDER = ((0, 0.10, 0), 0.12)  # offset from head joint, radius
HEAD_TOP = 0.25  # head box extends this far above the head joint
HALF = 0.02      # half thickness of bone boxes


def box(a, b, half):
    """8 corners of an axis-aligned box spanning segment a-b, padded by `half`."""
    lo = [min(a[i], b[i]) - half for i in range(3)]
    hi = [max(a[i], b[i]) + half for i in range(3)]
    verts = [(x, y, z) for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])]
    faces = [(0, 1, 3), (0, 3, 2), (4, 6, 7), (4, 7, 5), (0, 4, 5), (0, 5, 1),
             (2, 3, 7), (2, 7, 6), (0, 2, 6), (0, 6, 4), (1, 5, 7), (1, 7, 3)]
    return verts, faces


def build(version):
    flip = -1.0 if version == 0 else 1.0  # VRM 0.x avatars face -Z (left side is -X)
    names = [s[0] for s in SKELETON]
    idx = {n: i for i, n in enumerate(names)}
    world = {}
    nodes = []
    for name, parent, off in SKELETON:
        off = (off[0] * flip, off[1], off[2] * flip)
        pw = world[parent] if parent else (0, 0, 0)
        world[name] = tuple(pw[i] + off[i] for i in range(3))
        nodes.append({"name": name, "translation": list(off)})
    for name, parent, _ in SKELETON:
        if parent:
            nodes[idx[parent]].setdefault("children", []).append(idx[name])

    # geometry
    positions, joints, weights, indices = [], [], [], []
    segments = [(n, p) for n, p, _ in SKELETON if p and p != "Root"]
    for child, parent in segments:
        v, f = box(world[parent], world[child], HALF)
        base = len(positions)
        positions += v
        joints += [(idx[parent] - 1, 0, 0, 0)] * len(v)  # skin joints exclude Root
        weights += [(1.0, 0, 0, 0)] * len(v)
        indices += [base + i for tri in f for i in tri]
    head = world["head"]
    v, f = box(head, (head[0], head[1] + HEAD_TOP, head[2]), 0.08)
    base = len(positions)
    positions += v
    joints += [(idx["head"] - 1, 0, 0, 0)] * len(v)
    weights += [(1.0, 0, 0, 0)] * len(v)
    indices += [base + i for tri in f for i in tri]

    # Morph targets on the head box: 0 = "blink" (top face down), 1 = "happy" (top face up).
    head_box = list(range(base, base + len(v)))
    top_y = max(positions[i][1] for i in head_box)
    blink_d = [(0, 0, 0)] * len(positions)
    happy_d = [(0, 0, 0)] * len(positions)
    for i in head_box:
        if abs(positions[i][1] - top_y) < 1e-6:
            blink_d[i] = (0, -0.05, 0)
            happy_d[i] = (0, 0.03, 0)

    skin_joints = list(range(1, len(nodes)))
    ibms = []
    for j in skin_joints:
        w = world[names[j]]
        ibms += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -w[0], -w[1], -w[2], 1]

    bin_parts = []
    views = []
    accessors = []

    def add(data, target=None):
        off = sum(len(p) for p in bin_parts)
        pad = (-off) % 4
        if pad:
            bin_parts.append(b"\x00" * pad)
            off += pad
        bin_parts.append(data)
        view = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            view["target"] = target
        views.append(view)
        return len(views) - 1

    def acc(view, ctype, count, typ, **kw):
        a = {"bufferView": view, "componentType": ctype, "count": count, "type": typ}
        a.update(kw)
        accessors.append(a)
        return len(accessors) - 1

    pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
    mn = [min(p[i] for p in positions) for i in range(3)]
    mx = [max(p[i] for p in positions) for i in range(3)]
    a_pos = acc(add(pos_bytes, 34962), 5126, len(positions), "VEC3", min=mn, max=mx)
    a_uv = acc(add(b"".join(struct.pack("<2f", 0.5, 0.5) for _ in positions), 34962), 5126, len(positions), "VEC2")
    a_j = acc(add(b"".join(struct.pack("<4H", *j) for j in joints), 34962), 5123, len(joints), "VEC4")
    a_w = acc(add(b"".join(struct.pack("<4f", *w) for w in weights), 34962), 5126, len(weights), "VEC4")
    a_i = acc(add(b"".join(struct.pack("<H", i) for i in indices), 34963), 5123, len(indices), "SCALAR")
    a_ibm = acc(add(b"".join(struct.pack("<16f", *ibms[k:k + 16]) for k in range(0, len(ibms), 16))), 5126,
                len(skin_joints), "MAT4")
    img_view = add(png_1x1(230, 150, 40))
    targets = []
    for deltas in (blink_d, happy_d):
        tmin = [min(d[k] for d in deltas) for k in range(3)]
        tmax = [max(d[k] for d in deltas) for k in range(3)]
        a = acc(add(b"".join(struct.pack("<3f", *d) for d in deltas), 34962), 5126, len(deltas), "VEC3", min=tmin, max=tmax)
        targets.append({"POSITION": a})

    body = {"name": "Body", "mesh": 0, "skin": 0}
    nodes.append(body)
    mesh_node = len(nodes) - 1

    material = {
        "name": "BodyMat",
        "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "baseColorTexture": {"index": 0}},
        "alphaMode": "OPAQUE",
        "doubleSided": False,
    }
    gltf = {
        "asset": {"version": "2.0", "generator": "RedFactionVRoid test generator"},
        "scene": 0,
        "scenes": [{"nodes": [0, mesh_node]}],
        "nodes": nodes,
        "meshes": [{"name": "Body", "weights": [0, 0], "primitives": [{
            "attributes": {"POSITION": a_pos, "TEXCOORD_0": a_uv, "JOINTS_0": a_j, "WEIGHTS_0": a_w},
            "targets": targets, "indices": a_i, "material": 0}]}],
        "skins": [{"joints": skin_joints, "inverseBindMatrices": a_ibm}],
        "materials": [material],
        "images": [{"name": "tex", "mimeType": "image/png", "bufferView": img_view}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987}],
        "textures": [{"source": 0, "sampler": 0}],
        "bufferViews": views,
        "accessors": accessors,
    }
    human = [n for n in names if n not in NON_HUMAN]
    hair = [idx["hair1"], idx["hair2"], idx["hair3"]]
    coll_off, coll_r = HEAD_COLLIDER
    if version == 1:
        material["extensions"] = {"VRMC_materials_mtoon": {"specVersion": "1.0", "shadeColorFactor": [0.5, 0.3, 0.2]}}
        gltf["extensionsUsed"] = ["VRMC_vrm", "VRMC_materials_mtoon"]
        gltf["extensions"] = {"VRMC_vrm": {
            "specVersion": "1.0",
            "meta": {"name": "Test Avatar 1.0", "authors": ["RedFactionVRoid"], "version": "1",
                     "licenseUrl": "https://vrm.dev/licenses/1.0/"},
            "humanoid": {"humanBones": {n: {"node": idx[n]} for n in human}},
            "expressions": {
                "preset": {
                    "blink": {"morphTargetBinds": [{"node": mesh_node, "index": 0, "weight": 1.0}], "isBinary": False},
                    "happy": {"morphTargetBinds": [{"node": mesh_node, "index": 1, "weight": 1.0}], "overrideBlink": "block"},
                },
                "custom": {"smug": {"morphTargetBinds": [{"node": mesh_node, "index": 1, "weight": 0.5}]}},
            },
        }, "VRMC_springBone": {
            "specVersion": "1.0",
            "colliders": [{"node": idx["head"], "shape": {"sphere": {"offset": list(coll_off), "radius": coll_r}}}],
            "colliderGroups": [{"colliders": [0]}],
            "springs": [{"joints": [{"node": n, "hitRadius": 0.02, "stiffness": 0.5, "gravityPower": 1.0,
                                     "gravityDir": [0, -1, 0], "dragForce": 0.4} for n in hair],
                         "colliderGroups": [0]}],
        }}
        gltf["extensionsUsed"].append("VRMC_springBone")
    else:
        gltf["extensionsUsed"] = ["VRM"]
        gltf["extensions"] = {"VRM": {
            "exporterVersion": "test",
            "specVersion": "0.0",
            "meta": {"title": "Test Avatar 0.x", "author": "RedFactionVRoid", "version": "1"},
            "humanoid": {"humanBones": [{"bone": n, "node": idx[n], "useDefaultValues": True} for n in human]},
            "materialProperties": [{"name": "BodyMat", "shader": "VRM/MToon", "renderQueue": 2000,
                                    "floatProperties": {"_CullMode": 0, "_BlendMode": 0},
                                    "vectorProperties": {"_Color": [1, 1, 1, 1], "_ShadeColor": [0.5, 0.3, 0.2, 1]},
                                    "textureProperties": {"_MainTex": 0}}],
            "blendShapeMaster": {"blendShapeGroups": [
                {"name": "Blink", "presetName": "blink", "binds": [{"mesh": 0, "index": 0, "weight": 100}]},
                {"name": "Joy", "presetName": "joy", "binds": [{"mesh": 0, "index": 1, "weight": 100}]},
                {"name": "Smug", "presetName": "unknown", "binds": [{"mesh": 0, "index": 1, "weight": 50}]},
            ]},
            "secondaryAnimation": {
                "boneGroups": [{"stiffiness": 0.5, "gravityPower": 1.0, "gravityDir": {"x": 0, "y": -1, "z": 0},
                                "dragForce": 0.4, "hitRadius": 0.02, "bones": [hair[0]], "colliderGroups": [0]}],
                "colliderGroups": [{"node": idx["head"], "colliders": [
                    {"offset": {"x": coll_off[0], "y": coll_off[1], "z": coll_off[2]}, "radius": coll_r}]}],
            },
        }}

    bin_data = b"".join(bin_parts)
    bin_data += b"\x00" * ((-len(bin_data)) % 4)
    gltf["buffers"] = [{"byteLength": len(bin_data)}]
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * ((-len(js)) % 4)
    total = 12 + 8 + len(js) + 8 + len(bin_data)
    glb = struct.pack("<III", 0x46546C67, 2, total)
    glb += struct.pack("<II", len(js), 0x4E4F534A) + js
    glb += struct.pack("<II", len(bin_data), 0x004E4942) + bin_data

    expected = {
        "minY": mn[1], "maxY": mx[1], "height": mx[1] - mn[1],
        "headY": world["head"][1], "ankleY": world["leftFoot"][1],
        "humanBones": len(human), "title": "Test Avatar 1.0" if version == 1 else "Test Avatar 0.x",
        "springJoints": 2 if version == 1 else 3,
        "headTopVertex": [i for i in head_box if abs(positions[i][1] - top_y) < 1e-6][0],
        "headCollider": [world["head"][0], world["head"][1] + coll_off[1], world["head"][2]], "headColliderRadius": coll_r,
    }
    return glb, expected


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    exp = {}
    for v, fname in ((1, "test_v1.vrm"), (0, "test_v0.vrm")):
        glb, e = build(v)
        with open(os.path.join(out, fname), "wb") as f:
            f.write(glb)
        exp[fname] = e
    with open(os.path.join(out, "expected.json"), "w") as f:
        json.dump(exp, f, indent=2)


if __name__ == "__main__":
    main()
