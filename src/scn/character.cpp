// Character piece assembly, ported 1:1 from C:\S4Web2\engine\scn_character.js.
#include "scn/character.h"
#include <cstring>
#include <unordered_map>

namespace scn {

std::vector<float> ApplyMatrix(const std::vector<float>& pos, const Mat4& M) {
    std::vector<float> out(pos.size());
    for (size_t i = 0; i + 2 < pos.size(); i += 3) {
        float x = pos[i], y = pos[i+1], z = pos[i+2];
        out[i]   = x*M[0]+y*M[4]+z*M[8]+M[12];
        out[i+1] = x*M[1]+y*M[5]+z*M[9]+M[13];
        out[i+2] = x*M[2]+y*M[6]+z*M[10]+M[14];
    }
    return out;
}

// Many weapon-owned bones (Bone001, Dummy001...) have as parent the NAME OF A
// MODEL in the same scene (the blade hangs off the haft), not another bone -
// MakeNodeResolver handles that for rigid pieces, but the bone hierarchy itself
// doesn't know about models. Anchoring such a bone as an identity root loses the
// parent model's matrix and misplaces the piece ("weapon not fully assembled").
// A bone is only admitted if its parent chain ends at: another resolvable
// weapon-owned bone, a real skeleton bone, or a name that is NOT any model in
// the scene (a placeholder like "BONESYSTEM" -> root in its own local space,
// which for those cases IS the correct position, see Frill).
static bool BoneParentResolves(const Bone& bone, const Scene& scene,
                                const std::unordered_map<std::string, Mat4>& skelByName, int depth = 0) {
    if (depth > 16) return false;
    const std::string& p = bone.parent;
    if (p.empty()) return true;
    if (skelByName.count(p)) return true;
    for (const Model& m : scene.models) if (m.name == p) return false;
    const Bone* parentBone = nullptr;
    for (const Bone& b : scene.bones) if (b.name == p) { parentBone = &b; break; }
    if (!parentBone) return true;
    return BoneParentResolves(*parentBone, scene, skelByName, depth + 1);
}

// Parts with their own bones (skirts with Frill chains, etc.) don't come in the
// base skeleton: resolved separately (with sway, see skin.cpp) and appended to
// the bone table so SkinModelW can find their names too.
static std::vector<float> SkinAgainstScene(const Model& m, const Scene& skel, const std::vector<Mat4>& skelWorld,
                                            const Scene& scene, const Pose& pose) {
    // Almost every part ships a duplicate copy of the skeleton's Bip01 bones
    // (leftover of the .scn format); adding them as-is would shadow the real
    // skeleton's names with unresolved matrices -> broken character. Only NEW
    // bones (Frill and similar) get added; everything else always uses `skel`.
    std::unordered_map<std::string, Mat4> skelByName;
    for (size_t i = 0; i < skel.bones.size(); ++i) skelByName[skel.bones[i].name] = skelWorld[i];

    std::vector<Bone> extraBones;
    for (const Bone& b : scene.bones)
        if (!skelByName.count(b.name) && BoneParentResolves(b, scene, skelByName))
            extraBones.push_back(b);

    if (extraBones.empty()) return SkinModelW(m, skel, skelWorld);

    Scene extraScene; extraScene.bones = extraBones;
    float swayTime = float(pose.tickA) / 1000.f;
    // Weapon-owned bones (internal dummies, Frill chains...) that hang off a REAL
    // skeleton bone (hand, etc) anchor there via externalParents; the ones that
    // don't (Frill hung off the part's own "BONESYSTEM", not a real bone) stay
    // root in their own local space, which is already where they belong.
    std::vector<Mat4> partWorld = ResolveBoneWorlds(extraScene, pose, &swayTime, &skelByName);

    Scene combined; combined.bones = skel.bones;
    combined.bones.insert(combined.bones.end(), extraBones.begin(), extraBones.end());
    std::vector<Mat4> combinedWorld = skelWorld;
    combinedWorld.insert(combinedWorld.end(), partWorld.begin(), partWorld.end());
    return SkinModelW(m, combined, combinedWorld);
}

void GatherScene(const Scene& skel, const std::vector<Mat4>& skelWorld, const Scene& scene,
                  const Pose& pose, std::vector<Piece>& pieces) {
    // A model's parent can be a BONE or ANOTHER MODEL in the scene; the resolver
    // covers both (previously bones-only -> weapon pieces floated off).
    NodeResolver worldOf = MakeNodeResolver(scene, pose, false);
    for (const Model& m : scene.models) {
        if (m.positions.empty() || m.indices.empty()) continue;
        std::vector<float> positions;
        if (!m.skin.empty()) {
            positions = SkinAgainstScene(m, skel, skelWorld, scene, pose);   // skinned mesh -> bones place the verts
        } else {
            // rigid (face/hair): world = model.matrix * dummyLocal(part) * headWorld(skel)
            const Mat4* dummy = m.parent.empty() ? nullptr : worldOf(m.parent);
            Mat4 head; bool haveHead = BoneWorldByName(skel, "Bip01 Head", pose, head);
            Mat4 mmat; std::memcpy(mmat.data(), m.matrix, sizeof(mmat));
            Mat4 world;
            if (dummy && haveHead) world = Mat4Mul(Mat4Mul(mmat, *dummy), head);
            else if (dummy)        world = Mat4Mul(mmat, *dummy);
            else {
                // Parent isn't in the piece's own scene: might be a bone of the
                // SKELETON; if not, hang off the head (loose hair/face).
                const Mat4* hueso = m.parent.empty() ? nullptr : BoneWorldByNameW(skel, m.parent, skelWorld);
                if (hueso) world = Mat4Mul(mmat, *hueso);
                else if (haveHead) world = Mat4Mul(mmat, head);
                else world = SampleNode(m, pose.tickA);
            }
            positions = ApplyMatrix(m.positions, world);
        }
        Piece piece;
        piece.name = m.name; piece.positions = std::move(positions); piece.indices = m.indices;
        piece.uvs = m.uvs; piece.texture = m.texture; piece.renderFlag = m.renderFlag;
        pieces.push_back(std::move(piece));
    }
}

void GatherAccessory(const Scene& skel, const std::vector<Mat4>& skelWorld, const Scene& scene,
                      const std::string& targetBone, const Pose& pose, std::vector<Piece>& pieces) {
    NodeResolver worldOf = MakeNodeResolver(scene, pose, false);
    const Mat4* boneW = BoneWorldByNameW(skel, targetBone, skelWorld);
    if (!boneW) boneW = BoneWorldByNameW(skel, "Bip01 Head", skelWorld);
    for (const Model& m : scene.models) {
        if (m.positions.empty() || m.indices.empty()) continue;
        std::vector<float> positions;
        if (!m.skin.empty()) {
            positions = SkinModelW(m, skel, skelWorld);
        } else {
            const Mat4* dummy = m.parent.empty() ? nullptr : worldOf(m.parent);
            Mat4 mmat; std::memcpy(mmat.data(), m.matrix, sizeof(mmat));
            Mat4 local = dummy ? Mat4Mul(mmat, *dummy) : mmat;
            positions = ApplyMatrix(m.positions, boneW ? Mat4Mul(local, *boneW) : local);
        }
        Piece piece;
        piece.name = m.name; piece.positions = std::move(positions); piece.indices = m.indices;
        piece.uvs = m.uvs; piece.texture = m.texture; piece.renderFlag = m.renderFlag;
        pieces.push_back(std::move(piece));
    }
}

std::vector<Piece> AssembleCharacter(const Scene& skel, const std::vector<Scene>& parts, const Pose& pose) {
    // 1:1 with GatherCharacterPieces: skeleton bones resolved ONCE per frame (not
    // per-model), then every part is gathered against that.
    std::vector<Mat4> skelWorld = ResolveBoneWorlds(skel, pose);
    std::vector<Piece> pieces;
    for (const Scene& part : parts) GatherScene(skel, skelWorld, part, pose, pieces);
    return pieces;
}

}  // namespace scn
