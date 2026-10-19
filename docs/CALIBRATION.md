# Calibrating RedFactionVRoid for your game build

To make the avatar follow Mason, the mod needs four things from `rfg.exe`'s memory. They are
configured in `RFVRoid.ini` and are the same for everyone on the same game build. Once
someone finds them, they can share the `[Game*]`, `[BoneMap]` and `[DrawFilter]` sections.

| Needed for | Setting | Required? |
|---|---|---|
| Hiding Mason's own model | `[DrawFilter] HideDraws` (in-game tool, no memory hunting) | yes |
| Avatar follows Mason | `[GameSkeleton] BoneArray` + `[BoneMap]` | yes |
| Avatar drawn in the right place on screen | `[GameCamera] Transform` (+ `Fov`) | yes |
| Reliable pause-menu detection | `[GamePause] Flag` | optional |

Tools: the mod's own overlays, plus [Cheat Engine](https://www.cheatengine.org/) for
finding addresses. Play offline / single-player while doing this.

## 1. Hide Mason (draw logger)

This step needs no addresses.

1. Load a save, stand still in third person, press **F10**. The draw logger lists every
   skinned draw call from the last frame (index count, start index, calls). The selected one
   is hidden.
2. Step through the list with **PgUp / PgDn** and watch Mason. When a part of him disappears
   (body, head, arms, gear), press **Insert** to hide that draw whenever a VRM skin is
   active. **Delete** removes it again.
3. Repeat for every part of Mason. Also walk away from the camera and repeat: lower detail
   levels (LODs) use different draws.
4. **F10** closes the logger. The list is saved to `RFVRoid_state.ini` (`HideDraws=`). Copy it
   to `[DrawFilter] HideDraws` in `RFVRoid.ini` to share it.

If the list is empty, the game's skinned meshes don't use `BLENDINDICES`. Set
`[DrawFilter] SkinnedOnly=0` to list every draw.

## 2. Mason's skeleton

Volition's engine stores transforms as `matrix43`: 12 floats (right vector, up vector,
forward vector, position). The mod needs the address of the first bone of Mason's bone array
(world space by default, `BoneStride=48`).

1. In Cheat Engine, attach to `rfg.exe`. Find Mason's world position (three floats, X/Y/Z,
   with Y up): search *unknown initial value* (float), move, *changed value*, stand still,
   *unchanged value*, and so on.
2. Browse the memory region around candidates. A bone array looks like repeated 48-byte
   blocks, each ending in a position close to Mason's position (within about 2 m).
3. Use *Pointer scan* on the first bone address, restart the game, and keep a path that
   survives. Write it like `[[rfg.exe+0x1234560]+0x58]+0x120`, where `[x]` means "read the
   pointer stored at x". Byte signatures are more robust across patches:
   `[[sig("48 8B 05 ?? ?? ?? ?? 48 8B 88", 3)]+0x58]`.
4. Put it in `[GameSkeleton] BoneArray`. Set `BoneCount` (e.g. 80) for now.
5. If the bones turn out to be relative to the character, set `BoneSpace=model` and
   `PlayerTransform` to Mason's world transform.

Community projects that reverse-engineered RFG Re-Mars-tered (for example the RFGR Script
Loader, "RSL2") document engine structures such as the player/human object. They are a good
reference for where the character's bone data lives.

## 3. Camera

1. The camera's world transform is also a `matrix43` (right, up, forward, position). Search
   for the camera position: it changes when you orbit the camera without moving Mason.
2. Verify that the 9 floats before it form unit-length, mutually perpendicular vectors.
3. Set `[GameCamera] Transform` to the address of the first float. If the FOV is known, set
   `Fov` (with `FovUnits`/`FovAxis`). Otherwise tune `FovDefault`.

## 4. Map the bones

1. Set `[General] Debug=1` (or press **F9** in game). With skeleton and camera configured,
   every bone is drawn as an orange dot with its index (and name, if `BoneNameArray` is set).
   The overlay also shows whether the skeleton and camera were read successfully.
2. Fill `[BoneMap]` with the index of each joint: hips/pelvis, spine, chest, neck, head,
   shoulders/clavicles, upper arms, forearms, hands, thighs, shins, feet, toes. `hips` and
   `head` are required. Every other entry improves the result.
3. If the head turns the wrong way, change `HeadForwardAxis` / `HeadUpAxis` (e.g. `-z`, `+x`).

## 5. Pause flag (optional)

Without it, the mod infers the pause menu from Esc / Start / A / B presses and from Mason's
skeleton freezing. For exact detection, search Cheat Engine for a byte that is `1` while
paused and `0` while playing. Set `[GamePause] Flag` (and `PausedValue`).

## 6. Depth (optional)

With `DepthMode=none`, the avatar is drawn over the scene. With `standard` or `reversed`,
the mod reuses the game's depth buffer so walls can hide the avatar. This only works when the
camera `Near` / `Far` values match the game's projection. Try both modes and adjust.

## Sharing

Please share working values (with your game version) so they can become the defaults.
