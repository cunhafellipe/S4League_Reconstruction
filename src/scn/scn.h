// .scn model/scene parser, ported 1:1 from C:\S4Web2\engine\scn_geometry.js.
// Pure binary parsing only - bone/animation sampling and skinning live in
// scn/skin.h (ported from scn_skin.js).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace scn {

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Vec2 { float u = 0, v = 0; };
struct Face { uint16_t a = 0, b = 0, c = 0; };

struct VecKey  { uint32_t t; float v[3]; };
struct QuatKey { uint32_t t; float q[4]; };
struct VisKey  { uint32_t t; float v; };   // visibility (opacity) 0..1 at tick t

struct Anim {
    std::string name;
    uint32_t duration = 0;
    bool hasTransform = false;
    float initT[3] = {0,0,0};
    float initR[4] = {0,0,0,1};   // quaternion x,y,z,w
    float initS[3] = {1,1,1};
    std::vector<VecKey>  trans, scale;
    std::vector<QuatKey> rot;
    std::vector<VisKey> vis;   // opacity track (effect meshes fade with it); empty = always 1
};

struct BoneWeight { uint32_t vertex; float weight; };
struct SkinBone {
    std::string name;
    float bind[16];
    std::vector<BoneWeight> weights;
};

enum RenderFlag : uint32_t {
    RF_NO_LIGHT = 1, RF_TRANSPARENT = 2, RF_CUTOUT = 4, RF_NO_CULLING = 8,
    RF_NO_DEPTH_WRITE = 64,
};

struct TexItem {
    std::string main;        // main texture
    std::string side;        // side texture (lightmap when extraUv)
    uint32_t faceStart = 0;
    uint32_t faceCount = 0;
};

struct Model {
    std::string name;
    std::string parent;
    float matrix[16];
    uint32_t renderFlag = 0;
    std::string texture;
    std::string sideTexture;
    std::vector<TexItem> texItems;
    std::vector<float> positions;   // flat x,y,z,...
    std::vector<uint16_t> indices;  // flat triangle indices
    std::vector<float> uvs;         // flat u,v,... (main UVs)
    std::vector<SkinBone> skin;
    std::vector<Anim> anims;

    // Struct-of-Vec3/Face view of positions/indices, for callers that want named
    // fields instead of flat arrays (renderer/collision code). Filled alongside
    // the flat arrays during parsing - same data, two shapes.
    std::vector<Vec3> vertices;
    std::vector<Face> faces;
    std::vector<Vec2> uv1;   // main UVs
    std::vector<Vec2> uv2;   // lightmap UVs (empty when no extraUv)
};

struct Bone {
    std::string name, parent;
    float matrix[16];
    std::vector<Anim> anims;
};

// A BOX chunk (CBoxNode): oriented box helper. In shipped maps these are the
// spawn markers (alpha_spawn_pos_*, beta_spawn_pos_*); world placement comes
// from `matrix` (the chunk's own localCenter/axes are identity in shipped data).
// Not part of scn_geometry.js (which skips BOX_MAGIC bytes without capturing
// them - S4Web2's own spawn logic just raycasts the map center instead), kept
// here as a scene-data extra since spawn-marker placement is independent of
// the bones/animation/movement engine itself.
struct Box {
    std::string name, parent;
    float matrix[16];
    Vec3 halfExtents;
};

struct Scene {
    uint32_t version = 0;
    std::string dir;    // folder the .scn was loaded from (texture base) - set by Parse()
    std::vector<Model> models;
    std::vector<Bone> bones;
    std::vector<Box> boxes;
    std::vector<std::string> animNames;
    std::vector<const Model*> meshes;   // models with geometry (positions+indices non-empty)
    // Costume node parts only: skeleton bone the scene hangs from
    // (item.x7 to_node_parent_nodeN). Empty = Bip01 Head.
    std::string attachBone;
    // Pets: the scene has its own skeleton and clips (alice.scn: its own
    // Bip01..., 00074 idle). Skinned/posed with itself, not the character.
    bool ownSkeleton = false;
    // item.x7 to_node_animation_partN="1": the node plays the character's
    // clip names itself, in character space (not carried by its bone).
    bool animPart = false;
    // Costume slot this part was loaded for (0 hair .. 7 pet), -1 otherwise.
    int costumeSlot = -1;
    bool ok = false;
    std::string error;
};

// Parses raw .scn bytes already loaded into memory (caller owns decoding/decryption).
Scene ParseScn(const uint8_t* data, size_t size);

// Convenience wrapper: reads `path` from disk and parses it, also setting
// Scene::dir to path's containing folder (texture/asset base for this scene).
Scene Parse(const char* path);

}  // namespace scn
