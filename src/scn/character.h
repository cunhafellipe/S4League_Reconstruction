// Character piece assembly, ported 1:1 from C:\S4Web2\engine\scn_character.js.
#pragma once
#include "scn/scn.h"
#include "scn/skin.h"
#include <string>
#include <vector>

namespace scn {

struct Piece {
    std::string name;
    std::vector<float> positions;      // flat x,y,z,... world space
    std::vector<uint16_t> indices;
    std::vector<float> uvs;
    std::string texture;
    std::string dir;
    uint32_t renderFlag = 0;
};

std::vector<float> ApplyMatrix(const std::vector<float>& pos, const Mat4& m);

// skel = skeleton scene (male_bip). parts = body/leg/hand/foot/face/hair scenes.
void GatherScene(const Scene& skel, const std::vector<Mat4>& skelWorld, const Scene& scene,
                  const Pose& pose, std::vector<Piece>& pieces);

// item.x7 to_node_scene_file attachments (glasses, hats, wings, capes, hoods):
// unlike body parts, hangs off a SPECIFIC named bone (targetBone) instead of
// resolving through the part's own parent chain against the skeleton.
void GatherAccessory(const Scene& skel, const std::vector<Mat4>& skelWorld, const Scene& scene,
                      const std::string& targetBone, const Pose& pose, std::vector<Piece>& pieces);

// skel = skeleton scene. parts = body part scenes. Resolves skeleton bones ONCE
// per frame (not per-model) and gathers all parts against it.
std::vector<Piece> AssembleCharacter(const Scene& skel, const std::vector<Scene>& parts, const Pose& pose);

}  // namespace scn
