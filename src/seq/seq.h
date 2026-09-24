// .seq parser - S4 League particle-effect timeline format (weapon fire flashes,
// hit sparks, dodge trails...). Ported 1:1 from C:\S4Web2\engine\seq.js, which
// was reverse-engineered from S4Client.exe (CoreLib/BaseLib CRuntimeClass
// records) and validated against 700 real .seq files.
//
// Layout: f32 version(0.1), str name, u8 flag, u32 duration(ms), u32 nodeCount,
// then nodeCount "CActXxx" class records found by scanning the packed
// (unaligned) byte stream for an ASCII "CAct[A-Za-z0-9_]+" run followed by a
// NUL - the header doesn't give per-node offsets, so record boundaries are
// found the same way the JS tool does it. Only CActParticle/CActParticle_IF
// carry the structured emitter+keyframe+texture data SeqPlayer needs to render;
// other classNames (CActNodeLoader, ...) are kept for the node count/debug only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace seq {

struct Vec3 { float x = 0, y = 0, z = 0; };

struct Emitter {
    uint32_t frame = 0;
    bool     continuance = false;
    Vec3     origin, coneDir;
    float    coneTheta = 0, speed = 0;
    uint32_t emitterType = 0;
    float    minWidth = 0, maxWidth = 0, minHeight = 0, maxHeight = 0;
    Vec3     gravity;
    float    attractive = 0;
    uint32_t lifetime = 0, perSecond = 0, textureKey = 0;
};

// One point on the size/color/rotation curve, sampled by frame (0..duration).
struct Key { float frame = 0, sizeX = 1, sizeY = 1, r = 1, g = 1, b = 1, a = 1, rotate = 0; };

struct TexRef { std::string path, file; float uv[8] = {0,0,0,0,0,0,0,0}; };

struct Node {
    std::string className, nodeName;
    float    version = 0;
    uint32_t a = 0, b = 0;
    bool     isParticle = false;   // CActParticle(_IF): the rest of the fields below are valid
    uint32_t beginTime = 0, endTime = 0;
    uint32_t renderType = 0, textureAnimType = 0, blendType = 0;   // blendType 1 = additive
    std::vector<Emitter> emitters;   // real files: 0 or 1
    std::vector<Key>     keys;
    std::vector<TexRef>  textures;
    bool ok = false;                 // structured parse consumed the body cleanly

    // CActNodeLoader(_IF): the .scn this node attaches to the effect, found by
    // scanning the node's own bytes for an embedded ".scn" path rather than a
    // known field offset - the loader's exact struct layout isn't confirmed,
    // but every real file carries the model path as a plain string somewhere
    // in the body.
    std::string modelFile;
};

struct Sequence {
    float    version = 0;
    std::string name;
    uint8_t  flag = 0;
    uint32_t duration = 0;    // ms, timeline loops at this
    uint32_t nodeCount = 0;
    std::vector<Node> nodes;
    bool ok = false;
    std::string error;
};

Sequence Parse(const char* path);

}  // namespace seq
