// .scn parser, ported 1:1 from C:\S4Web2\engine\scn_geometry.js.
#include "scn/scn.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace scn {

static const uint32_t SCENE_MAGIC       = 0x6278D57A;
static const uint32_t MODEL_MAGIC       = 0x081098F8;
static const uint32_t BOX_MAGIC         = 0x25ADF0D1;
static const uint32_t BONE_MAGIC        = 0x6D411AD1;
static const uint32_t LIGHT_MAGIC       = 0xC3E8BE62;
static const uint32_t BONE_SYSTEM_MAGIC = 0x5E74333F;
static const uint32_t LINE_MAGIC        = 0xADEE38A2;
static const uint32_t SPHERE_MAGIC      = 0x4F1C2440;

static bool IsChunkMagic(uint32_t v) {
    return v == MODEL_MAGIC || v == BOX_MAGIC || v == BONE_MAGIC || v == LIGHT_MAGIC ||
           v == BONE_SYSTEM_MAGIC || v == LINE_MAGIC || v == SPHERE_MAGIC;
}
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

static void ReadVis(Reader& r, std::vector<VisKey>& out) {
    uint32_t c = r.U32();
    for (uint32_t k = 0; k < c && !r.bad; ++k) { VisKey vk; vk.t = r.U32(); vk.v = r.F32(); if (k < 4096) out.push_back(vk); }
}

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
        ReadVis(r, a.vis);
        uint32_t mc = r.U32();   // morph (vertex animation) keys
        for (uint32_t j = 0; j < mc && !r.bad; ++j) {
            MorphKey mk; mk.t = r.U32();
            uint32_t vc = r.U32();
            for (uint32_t v = 0; v < vc && !r.bad; ++v) { MorphVert mv; mv.idx = r.U32(); r.V3(mv.p); if (vc < 65536) mk.verts.push_back(mv); }
            uint32_t uc = r.U32();
            for (uint32_t v = 0; v < uc && !r.bad; ++v) { MorphUv mu; mu.idx = r.U32(); mu.u = r.F32(); mu.v = r.F32(); if (uc < 65536) mk.uvs.push_back(mu); }
            if (mc < 4096) a.morph.push_back(std::move(mk));
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
        ReadVis(r, a.vis);
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

// Re-links the parsed nodes the way the client builds the tree
// (CSceneResource::Load sub_583270 -> per node sub_5833D0 -> sub_583540, beta):
// nodes are read in FILE order, the first one (the SCENE header, named after
// the file) becomes the root, and each later
// node looks its parent up with root->FindNode(name) (sub_530F40: pre-order
// DFS, first match) among the nodes already attached. So a duplicated name
// resolves to the copy found first, and a parent that only appears LATER in
// the file is not found. 124_weapon_bat has two Dummy002 and two IK Chain001,
// and its first node IK Chain001 names IK Chain004 (read later) as parent:
// linked by name alone that formed a loop and the bat came out giant.
// A node whose parent the client would not find keeps its original parent
// name: some files (zautumn_umbrella_*, erosion_ripper) name a scene
// root other than their header and would vanish entirely.
// Here: later duplicates get a unique name ("Dummy002#2") and every child's
// parent string points at the node the client would have picked, so the rest
// of the code (name based) sees the client's tree.
struct NodeRef { int kind; size_t idx; };   // 0 model, 1 bone, 2 box
static void LinkLikeClient(Scene& sc, const std::vector<NodeRef>& order, std::string rootName) {
    const size_t n = order.size();
    if (n < 2) return;
    auto nameOf = [&](const NodeRef& r) -> std::string& {
        if (r.kind < 0) return rootName;
        return r.kind == 0 ? sc.models[r.idx].name : r.kind == 1 ? sc.bones[r.idx].name : sc.boxes[r.idx].name; };
    std::string rootParent;
    auto parentOf = [&](const NodeRef& r) -> std::string& {
        if (r.kind < 0) return rootParent;
        return r.kind == 0 ? sc.models[r.idx].parent : r.kind == 1 ? sc.bones[r.idx].parent : sc.boxes[r.idx].parent; };
    bool dup = false, forward = false;
    {
        std::unordered_map<std::string, int> seen;
        for (size_t i = 0; i < n; ++i) if (++seen[nameOf(order[i])] > 1) dup = true;
        std::unordered_map<std::string, size_t> first;
        for (size_t i = 0; i < n; ++i) first.emplace(nameOf(order[i]), i);
        for (size_t i = 0; i < n; ++i) {
            auto it = first.find(parentOf(order[i]));
            if (it != first.end() && it->second >= i) forward = true;   // parent read later (or itself)
        }
    }
    if (!dup && !forward) return;   // the plain name links already match the client

    std::vector<std::string> names(n);
    for (size_t i = 0; i < n; ++i) names[i] = nameOf(order[i]);
    std::vector<std::vector<size_t>> kids(n);
    std::vector<int> par(n, -1);
    std::vector<bool> attached(n, false);
    attached[0] = true;
    for (size_t i = 1; i < n; ++i) {
        const std::string& want = parentOf(order[i]);
        // pre-order DFS from the root over what is attached so far
        int found = -1;
        std::vector<size_t> stack{ 0 };
        while (!stack.empty() && found < 0) {
            const size_t c = stack.back(); stack.pop_back();
            if (names[c] == want) { found = (int)c; break; }
            for (size_t k = kids[c].size(); k-- > 0; ) stack.push_back(kids[c][k]);
        }
        if (found >= 0) { par[i] = found; kids[found].push_back(i); attached[i] = true; }
    }
    // unique names for the later duplicates
    std::unordered_map<std::string, int> count;
    for (size_t i = 0; i < n; ++i) {
        const int c = ++count[names[i]];
        if (c > 1) names[i] += "#" + std::to_string(c);
    }
    for (size_t i = 0; i < n; ++i) nameOf(order[i]) = names[i];
    for (size_t i = 1; i < n; ++i) {
        if (par[i] > 0) { parentOf(order[i]) = names[par[i]]; continue; }   // under the root: keep its own spelling
        if (par[i] == 0) continue;
        // Not found because the parent is only read LATER (the bat's first node
        // IK Chain001 -> IK Chain004): the client cannot link it either; hang
        // it from the root instead of looping back through its own children.
        const std::string& want = parentOf(order[i]);
        for (size_t j = i; j < n; ++j)
            if (nameOf(order[j]) == want || names[j] == want) { parentOf(order[i]) = names[0]; break; }
    }
}

Scene ParseScn(const uint8_t* data, size_t size) {
    Scene sc;
    std::vector<NodeRef> order{ { -1, 0 } };   // [0] = the SCENE header node (root)
    Reader r(data, size);

    sc.version = r.U32();
    uint32_t v = r.U32(); int guard = 0;
    while (v != SCENE_MAGIC && !r.bad && guard++ < 64) v = r.U32();
    if (v != SCENE_MAGIC) { sc.error = "no SCENE magic"; return sc; }

    const std::string sceneName = r.Str(); r.Str(); r.U32(); r.Skip(16 * 4);
    uint32_t matrixEnd = r.U32();
    r.U32();   // chunk count - not trusted, see below
    if (matrixEnd >= VERSION_2) r.Str();

    // Newer .scn files carry extra header data and chunk types this parser
    // does not know, and their chunk count no longer matches what follows -
    // walking exactly chunkCount chunks stopped at the first unknown block
    // (e.g. 0x21000000) with 0 meshes loaded. Same as ItemManager's
    // scn_geometry.js: skip forward to the next known chunk magic and read
    // chunks until the end of the file.
    while (!r.bad) {
        while (r.o + 4 <= r.n && !IsChunkMagic(uint32_t(r.b[r.o]) | uint32_t(r.b[r.o+1]) << 8 |
                                                uint32_t(r.b[r.o+2]) << 16 | uint32_t(r.b[r.o+3]) << 24))
            ++r.o;
        if (r.o + 4 > r.n) break;
        uint32_t type = r.U32();
        Base base = ReadBase(r);
        if (type == MODEL_MAGIC) {
            sc.models.push_back(ParseModel(r, base));
            order.push_back({ 0, sc.models.size() - 1 });
        } else if (type == BONE_MAGIC) {
            Bone b; b.name = base.name; b.parent = base.parent;
            std::memcpy(b.matrix, base.matrix, sizeof(b.matrix));
            b.anims = ReadBoneAnims(r, base.mend >= VERSION_2);
            sc.bones.push_back(std::move(b));
            order.push_back({ 1, sc.bones.size() - 1 });
        } else if (type == BONE_SYSTEM_MAGIC) {
            // BONESYSTEM is a real chunk with its own matrix (BaseChunk-only, no extra
            // fields) - added as a bone so parented chains (e.g. Frill) have a node to
            // anchor to, matching scn_geometry.js.
            Bone b; b.name = base.name; b.parent = base.parent;
            std::memcpy(b.matrix, base.matrix, sizeof(b.matrix));
            sc.bones.push_back(std::move(b));
            order.push_back({ 1, sc.bones.size() - 1 });
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
            order.push_back({ 2, sc.boxes.size() - 1 });
        } else if (type == LIGHT_MAGIC) {
            r.Skip(2*4 + 7*3*4);
        } else if (type == LINE_MAGIC) {
            // CShapeNode line (weapon "Line01" under the blade): the segment a
            // CGhostTrailNode sweeps for the sword trail (Game::CActor::
            // MakeGhostTrailNode looks it up by name).
            Line ln; ln.name = base.name; ln.parent = base.parent;
            std::memcpy(ln.matrix, base.matrix, sizeof(ln.matrix));
            uint32_t c = r.U32();
            if (c > 4096) c = 0;
            ln.pts.resize(size_t(c) * 6);
            for (float& f : ln.pts) f = r.F32();
            sc.lines.push_back(std::move(ln));
        } else if (type == SPHERE_MAGIC) {
            r.Skip(4 + 3 * 4 + 4);
        }
    }

    LinkLikeClient(sc, order, sceneName);

    // Clip names any node of this scene can play. Models count too: in weapon
    // SCNs the clip tracks often sit on the MODEL, not on its Wea_Dummy (the
    // plasma sword's A0018/A0019 - the jump attack's reverse grip and blade
    // extension - live on model "sword"; the dummy only has "SWORD"). Listing
    // bones only made the weapon skip its own clip and stay in its normal
    // grip while the body played the stab - the sword showed up upside down.
    auto addName = [&](const Anim& a) {
        if (a.hasTransform && std::find(sc.animNames.begin(), sc.animNames.end(), a.name) == sc.animNames.end())
            sc.animNames.push_back(a.name);
    };
    for (const Bone& b : sc.bones)   for (const Anim& a : b.anims) addName(a);
    for (const Model& m : sc.models) for (const Anim& a : m.anims) addName(a);

    for (const Model& m : sc.models)
        if (!m.positions.empty() && !m.indices.empty())
            sc.meshes.push_back(&m);

    // Spawn scenes (<map>_spawn.scn) hold only BOX markers; they are valid too.
    sc.ok = !sc.models.empty() || !sc.bones.empty() || !sc.boxes.empty();
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
    // Memory it takes: geometry (kept twice: flat arrays + Vec3/Face views),
    // skins and animation keys.
    size_t geo = 0, dup = 0, anim = 0;
    auto animBytes = [](const Anim& a) {
        size_t b = a.trans.capacity() * sizeof(VecKey) + a.scale.capacity() * sizeof(VecKey) +
                   a.rot.capacity() * sizeof(QuatKey) + a.vis.capacity() * sizeof(VisKey) + sizeof(Anim);
        for (const MorphKey& k : a.morph) b += sizeof(MorphKey) + k.verts.capacity() * sizeof(MorphVert) + k.uvs.capacity() * sizeof(MorphUv);
        return b;
    };
    for (const Model& m : sc.models) {
        geo += m.positions.capacity() * sizeof(float) + m.indices.capacity() * sizeof(uint16_t) + m.uvs.capacity() * sizeof(float);
        dup += m.vertices.capacity() * sizeof(Vec3) + m.faces.capacity() * sizeof(Face) + m.uv1.capacity() * sizeof(Vec2) + m.uv2.capacity() * sizeof(Vec2);
        for (const SkinBone& sb : m.skin) geo += sizeof(SkinBone) + sb.weights.capacity() * sizeof(BoneWeight);
        for (const Anim& a : m.anims) anim += animBytes(a);
    }
    for (const Bone& b : sc.bones) for (const Anim& a : b.anims) anim += animBytes(a);
    char note[160];
    snprintf(note, sizeof(note), "(geometry %.2f MB + its second copy %.2f MB, animation %.2f MB, file %.2f MB)",
             geo / 1048576.0, dup / 1048576.0, anim / 1048576.0, buf.size() / 1048576.0);
    if (sc.ok) sc.memTag.Set(reslog::SCN, path, geo + dup + anim, note);
    return sc;
}

}  // namespace scn
