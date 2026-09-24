// Bone/skinning engine for .scn scenes, ported 1:1 from
// C:\S4Web2\engine\scn_skin.js. Row-major, row-vector matrices (D3D9 LH),
// v' = v * M.
#pragma once
#include "scn/scn.h"
#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace scn {

using Mat4 = std::array<float, 16>;

Mat4 Mat4Mul(const Mat4& a, const Mat4& b);
Mat4 Mat4InvAffine(const Mat4& m);
Mat4 BuildTRS(const float T[3], const float q[4], const float S[3]);

// A cross-faded pose: blend clipA(tickA) -> clipB(tickB) by `blend` (0=A, 1=B).
struct Pose {
    std::string clipA, clipB;
    uint32_t tickA = 0, tickB = 0;
    float blend = 0.f;
    bool loopA = true, loopB = true;
};
using AnimPose = Pose;   // legacy name (pre-1:1-port scenes.cpp)

// Animated node matrix (row-major) at `tick`, relative to bind (identity at tick 0).
Mat4 SampleNode(const Model& m, uint32_t tick);
// Same, writing into a caller-owned float[16] (legacy call shape).
void SampleNode(const Model& m, uint32_t tick, float out16[16]);

// Local animated matrix of a MODEL (not a bone) sampled by clip name; falls back
// to m.matrix (bind, un-animated) when the clip doesn't exist on this model -
// no fallback to anims[0]/"BASE" (breaks the plasma sword, per scn_skin.js).
Mat4 SampleModelLocal(const Model& m, const std::string& clipName, uint32_t tick, bool loop = true);

// True for the leg bones (Bip01 L/R Thigh/Calf/Foot/Toe0 and their Nub tips) -
// the ones the real client drives from its own separate lowerPartStateSet
// while the rest of the body (spine/arms/head/weapon dummies) follows the
// main pose. Named-based, matches the S4 bip rig used by every character.
bool IsLowerBodyBone(const std::string& boneName);

// world[i] = local[i] * parentWorld up the hierarchy.
// swayTime (seconds, optional): procedural sine sway on *_Frill_Bone_NN bones
// (frozen bind-pose costume chains with no animation keys of their own).
// externalParents (optional): name -> already-resolved world matrix, for bones
// whose parent isn't in `sc` but exists elsewhere (weapon dummies hung off the
// base skeleton's hand bone).
// lowerPose (optional): if given, IsLowerBodyBone() bones sample THIS pose
// instead of `pose` - legs keep walking/standing on their own real clip while
// the rest of the body plays an attack, matching the real engine's separate
// upper/lower body state machines instead of one clip driving everything.
std::vector<Mat4> ResolveBoneWorlds(const Scene& sc, const Pose& pose,
                                     const float* swayTime = nullptr,
                                     const std::unordered_map<std::string, Mat4>* externalParents = nullptr,
                                     const Pose* lowerPose = nullptr);

bool BoneWorldByName(const Scene& sc, const std::string& boneName, const Pose& pose, Mat4& out);
// Same, writing into a caller-owned float[16] (legacy call shape).
bool BoneWorldByName(const Scene& sc, const std::string& boneName, const Pose& pose, float out16[16]);
const Mat4* BoneWorldByNameW(const Scene& sc, const std::string& boneName, const std::vector<Mat4>& world);

// Linear-blend skin `m` against `sc`'s skeleton posed by `pose` (or a precomputed
// `world[]`). Returns flat x,y,z,... positions; unmatched vertices keep bind pose.
std::vector<float> SkinModel(const Model& m, const Scene& sc, const Pose& pose);
std::vector<float> SkinModelW(const Model& m, const Scene& sc, const std::vector<Mat4>& world);
// Same, appending Vec3 (legacy call shape).
void SkinModel(const Model& m, const Scene& sc, const Pose& pose, std::vector<Vec3>& out);
// Same, with a separate lower-body pose (see ResolveBoneWorlds).
void SkinModel(const Model& m, const Scene& sc, const Pose& pose, const Pose* lowerPose, std::vector<Vec3>& out);

// Inverse of a weapon's mount dummy, for placing its pieces relative to it
// (world = piece.matrix * WeaponMountInverse * handBoneWorld). If the dummy's
// own translation falls OUTSIDE the weapon's vertex bounding box, its
// translation lives in some other space (dragonmachinegun: dummy at z=+81,
// weapon spans z=[-184,49]) - only its ROTATION is usable then (translation
// zeroed). world.html's own weapon-attach logic, not part of scn_skin.js.
void WeaponMountInverse(const Scene& sc, const Bone& mount, float invOut[16]);

// Resolves each weapon MODEL's world matrix in the weapon's own scene space
// (same frame as the mount dummy), picking per-piece between FLAT (m.matrix
// already absolute) and CHAINED (m.matrix relative to its parent MODEL) by
// whichever interpretation lands the piece's vertex centroid closer to its
// already-resolved parent's centroid. world.html's own weapon-attach logic,
// not part of scn_skin.js.
void ResolveWeaponPieceWorlds(const Scene& sc, std::vector<std::array<float,16>>& out);

// Resolves the world matrix of a node that may be a bone OR a model (parents are
// not always bones - a scythe blade hangs off its haft model, a sword effect
// hangs off its blade model). animateModels: apply model.anims (weapons only;
// the character body itself is sampled via bind/SkinModel).
using NodeResolver = std::function<const Mat4*(const std::string& name)>;
NodeResolver MakeNodeResolver(const Scene& scene, const Pose& pose, bool animateModels);

}  // namespace scn
