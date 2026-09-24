// .scn parser, ported 1:1 from C:\S4Web2\engine\scn_geometry.js.
#include "scn/scn.h"
#include <cstring>
#include <algorithm>
#include <fstream>

namespace scn {

static const uint32_t SCENE_MAGIC       = 0x6278D57A;
static const uint32_t MODEL_MAGIC       = 0x081098F8;
static const uint32_t BOX_MAGIC         = 0x25ADF0D1;
static const uint32_t BONE_MAGIC        = 0x6D411AD1;
static const uint32_t LIGHT_MAGIC       = 0xC3E8BE62;
static const uint32_t BONE_SYSTEM_MAGIC = 0x5E74333F;
static const uint32_t LINE_MAGIC        = 0xADEE38A2;
static const uint32_t VERSION_2         = 1045220557;   // 0x3E4CCCCD
static const size_t   PADDING           = 1024;

struct Reader {
    const uint8_t* b; size_t n, o = 0; bool bad = false;
    Reader(const uint8_t* buf, size_t len) : b(buf), n(len) {}
    bool need(size_t k) { if (o + k > n) { bad = true; return false; } return true; }
    uint8_t  U8()  { if (!need(1)) return 0; return b[o++]; }
    uint16_t U16() { if (!need(2)) return 0; uint16_t v = uint16_t(b[o] | (b[o+1] << 8)); o += 2; return v; }
    uint32_t U32() { if (!need(4)) return 0; uint32_t v = b[o] | (b[o+1]<<8) | (b[o+2]<<16) | (uint32_t(b[o+3])<<24); o += 4; return v; }
    float    F32() { if (!need(4)) return 0.f; float v; std::memcpy(&v, b + o, 4); o += 4; return v; }
    void     V3(float out[3]) { out[0]=F32(); out[1]=F32(); out[2]=F32(); }
    void     Skip(size_t k) { o += k; if (o > n) bad = true; }
    std::string Str() { std::string s; while (o < n && b[o] != 0) s.push_back(char(b[o++])); if (o < n) ++o; return s; }
    std::string PaddedStr() { size_t s0 = o; std::string s = Str(); o = s0 + PADDING; if (o > n) bad = true; return s; }
    void Mat16(float out[16]) { for (int i = 0; i < 16; ++i) out[i] = F32(); }
};

struct Base { std::string name, parent; float matrix[16]; uint32_t mend; };

static void ReadTransformKeys(Reader& r, std::vector<VecKey>& trans, std::vector<QuatKey>& rot, std::vector<VecKey>& scale) {
    uint32_t k = r.U32();
    for (uint32_t j = 0; j < k && !r.bad; ++j) { VecKey vk; vk.t = r.U32(); r.V3(vk.v); trans.push_back(vk); }
    k = r.U32();
    for (uint32_t j = 0; j < k && !r.bad; ++j) { QuatKey qk; qk.t = r.U32(); qk.q[0]=r.F32(); qk.q[1]=r.F32(); qk.q[2]=r.F32(); qk.q[3]=r.F32(); rot.push_back(qk); }
    k = r.U32();
    for (uint32_t j = 0; j < k && !r.bad; ++j) { VecKey vk; vk.t = r.U32(); r.V3(vk.v); scale.push_back(vk); }
}

static void SkipVis(Reader& r) { uint32_t c = r.U32(); for (uint32_t k = 0; k < c && !r.bad; ++k) r.Skip(8); }

static std::vector<Anim> ReadModelAnims(Reader& r) {
    std::vector<Anim> anims;
    uint32_t c = r.U32();
    for (uint32_t i = 0; i < c && !r.bad; ++i) {
        Anim a; a.name = r.Str(); a.duration = r.U32();
        if (r.U8()) {
            a.hasTransform = true;
            r.V3(a.initT);
            a.initR[0]=r.F32(); a.initR[1]=r.F32(); a.initR[2]=r.F32(); a.initR[3]=r.F32();
            r.V3(a.initS);
            ReadTransformKeys(r, a.trans, a.rot, a.scale);
        }
        SkipVis(r);
        uint32_t mc = r.U32();   // morph keys (unused)
        for (uint32_t j = 0; j < mc && !r.bad; ++j) {
            r.U32();
            uint32_t vc = r.U32(); for (uint32_t v = 0; v < vc && !r.bad; ++v) r.Skip(16);
            uint32_t uc = r.U32(); for (uint32_t v = 0; v < uc && !r.bad; ++v) r.Skip(12);
        }
        anims.push_back(std::move(a));
    }
    return anims;
}

static std::vector<Anim> ReadBoneAnims(Reader& r, bool versionFlag) {
    std::vector<Anim> anims;
    uint32_t c = r.U32();
    for (uint32_t i = 0; i < c && !r.bad; ++i) {
        std::string name = r.Str();
        if (versionFlag) { std::string copy = r.Str(); if (!copy.empty()) continue; }
        Anim a; a.name = name; a.duration = r.U32();
        if (r.U8()) {
            a.hasTransform = true;
            r.V3(a.initT);
            a.initR[0]=r.F32(); a.initR[1]=r.F32(); a.initR[2]=r.F32(); a.initR[3]=r.F32();
            r.V3(a.initS);
            ReadTransformKeys(r, a.trans, a.rot, a.scale);
        }
        SkipVis(r);
        anims.push_back(std::move(a));
    }
    return anims;
}

static Base ReadBase(Reader& r) {
    Base b;
    b.name = r.Str();
    b.parent = r.Str();
    r.U32();               // matrix_start
    r.Mat16(b.matrix);
    b.mend = r.U32();      // matrix_end
    return b;
}

static Model ParseModel(Reader& r, const Base& base) {
    Model m;
    m.name = base.name; m.parent = base.parent;
    std::memcpy(m.matrix, base.matrix, sizeof(m.matrix));
    m.renderFlag = r.U32();
    uint32_t texVersion = r.U32();
    uint32_t extraUv = 0;
    if (texVersion >= VERSION_2) extraUv = r.U32();
    uint32_t itemCount = r.U32();
    m.texItems.reserve(itemCount);
    for (uint32_t i = 0; i < itemCount && !r.bad; ++i) {
        TexItem ti;
        ti.main = r.PaddedStr();
        if (texVersion >= VERSION_2) ti.side = r.PaddedStr();
        ti.faceStart = r.U32();
        ti.faceCount = r.U32();
        if (i == 0) { m.texture = ti.main; m.sideTexture = ti.side; }
        m.texItems.push_back(std::move(ti));
    }

    uint32_t vc = r.U32();
    m.positions.resize(size_t(vc) * 3);
    m.vertices.resize(vc);
    for (uint32_t i = 0; i < vc && !r.bad; ++i) {
        float x=r.F32(), y=r.F32(), z=r.F32();
        m.positions[i*3]=x; m.positions[i*3+1]=y; m.positions[i*3+2]=z;
        m.vertices[i] = { x, y, z };
    }

    uint32_t fc = r.U32();
    m.indices.resize(size_t(fc) * 3);
    m.faces.resize(fc);
    for (uint32_t i = 0; i < fc && !r.bad; ++i) {
        uint16_t a=r.U16(), b=r.U16(), c=r.U16();
        m.indices[i*3]=a; m.indices[i*3+1]=b; m.indices[i*3+2]=c;
        m.faces[i] = { a, b, c };
    }

    uint32_t nc = r.U32(); for (uint32_t i = 0; i < nc && !r.bad; ++i) r.Skip(12);   // normals (unused)

    uint32_t uc = r.U32();
    m.uvs.resize(size_t(uc) * 2);
    m.uv1.resize(uc);
    for (uint32_t i = 0; i < uc && !r.bad; ++i) {
        float u=r.F32(), v=r.F32();
        m.uvs[i*2]=u; m.uvs[i*2+1]=v;
        m.uv1[i] = { u, v };
    }
    if (extraUv == 1) {
        m.uv2.resize(uc);
        for (uint32_t i = 0; i < uc && !r.bad; ++i) m.uv2[i] = { r.F32(), r.F32() };
    }

    uint32_t tc = r.U32(); for (uint32_t i = 0; i < tc && !r.bad; ++i) r.Skip(12);   // tangents (unused)

    uint32_t wb = r.U32();
    m.skin.reserve(wb);
    for (uint32_t i = 0; i < wb && !r.bad; ++i) {
        SkinBone sb; sb.name = r.Str(); r.Mat16(sb.bind);
        uint32_t wc = r.U32(); sb.weights.reserve(wc);
        for (uint32_t k = 0; k < wc && !r.bad; ++k) { BoneWeight bw; bw.vertex = r.U32(); bw.weight = r.F32(); sb.weights.push_back(bw); }
        m.skin.push_back(std::move(sb));
    }

    m.anims = ReadModelAnims(r);
    return m;
}

Scene ParseScn(const uint8_t* data, size_t size) {
    Scene sc;
    Reader r(data, size);

    sc.version = r.U32();
    uint32_t v = r.U32(); int guard = 0;
    while (v != SCENE_MAGIC && !r.bad && guard++ < 64) v = r.U32();
    if (v != SCENE_MAGIC) { sc.error = "no SCENE magic"; return sc; }

    r.Str(); r.Str(); r.U32(); r.Skip(16 * 4);
    uint32_t matrixEnd = r.U32();
    uint32_t chunkCount = r.U32();
    if (matrixEnd >= VERSION_2) r.Str();

    for (uint32_t i = 0; i < chunkCount && !r.bad; ++i) {
        uint32_t type = r.U32();
        Base base = ReadBase(r);
        if (type == MODEL_MAGIC) {
            sc.models.push_back(ParseModel(r, base));
        } else if (type == BONE_MAGIC) {
            Bone b; b.name = base.name; b.parent = base.parent;
            std::memcpy(b.matrix, base.matrix, sizeof(b.matrix));
            b.anims = ReadBoneAnims(r, base.mend >= VERSION_2);
            sc.bones.push_back(std::move(b));
        } else if (type == BONE_SYSTEM_MAGIC) {
            // BONESYSTEM is a real chunk with its own matrix (BaseChunk-only, no extra
            // fields) - added as a bone so parented chains (e.g. Frill) have a node to
            // anchor to, matching scn_geometry.js.
            Bone b; b.name = base.name; b.parent = base.parent;
            std::memcpy(b.matrix, base.matrix, sizeof(b.matrix));
            sc.bones.push_back(std::move(b));
        } else if (type == BOX_MAGIC) {
            // boxVer(u32) + localCenter(vec3) + axes(3x3 floats) + halfExtents(vec3) = 64
            // bytes total (scn_geometry.js skips all of it; captured here as scene-data
            // extra for spawn markers, see Box in scn.h).
            Box bx; bx.name = base.name; bx.parent = base.parent;
            std::memcpy(bx.matrix, base.matrix, sizeof(bx.matrix));
            r.Skip(4 + 3*4 + 9*4);
            float hx=r.F32(), hy=r.F32(), hz=r.F32();
            bx.halfExtents = { hx, hy, hz };
            sc.boxes.push_back(std::move(bx));
        } else if (type == LIGHT_MAGIC) {
            r.Skip(2*4 + 7*3*4);
        } else if (type == LINE_MAGIC) {
            uint32_t c = r.U32(); r.Skip(size_t(c) * 2 * 3 * 4);
        } else {
            break;
        }
    }

    for (const Bone& b : sc.bones)
        for (const Anim& a : b.anims)
            if (a.hasTransform && std::find(sc.animNames.begin(), sc.animNames.end(), a.name) == sc.animNames.end())
                sc.animNames.push_back(a.name);

    for (const Model& m : sc.models)
        if (!m.positions.empty() && !m.indices.empty())
            sc.meshes.push_back(&m);

    sc.ok = !sc.models.empty() || !sc.bones.empty();
    return sc;
}

Scene Parse(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { Scene sc; sc.error = "open failed"; return sc; }
    std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Scene sc = ParseScn((const uint8_t*)buf.data(), buf.size());
    std::string p = path;
    size_t s = p.find_last_of("/\\");
    sc.dir = (s == std::string::npos) ? "" : p.substr(0, s);
    return sc;
}

}  // namespace scn
