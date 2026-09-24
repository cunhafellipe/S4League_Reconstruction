#include "seq/seq.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <cctype>

namespace seq {

struct Reader {
    const uint8_t* p; size_t n, o = 0; bool bad = false;
    Reader(const std::string& b) : p((const uint8_t*)b.data()), n(b.size()) {}
    bool need(size_t k) { if (o + k > n) { bad = true; return false; } return true; }
    uint8_t  U8()   { if (!need(1)) return 0; return p[o++]; }
    bool     Bool() { return U8() != 0; }
    uint32_t U32()  { if (!need(4)) return 0; uint32_t v = p[o] | (p[o+1]<<8) | (p[o+2]<<16) | ((uint32_t)p[o+3]<<24); o += 4; return v; }
    float    F32()  { uint32_t v = U32(); float f; std::memcpy(&f, &v, 4); return f; }
    // Look at the next f32 without consuming it (real files mark the start of
    // several optional blocks with a 0.1 sentinel - peeking is how the client's
    // own reader, and this port, tells "field present" from "block already here").
    float PeekF32() const { if (o + 4 > n) return 0.f; uint32_t v = p[o] | (p[o+1]<<8) | (p[o+2]<<16) | ((uint32_t)p[o+3]<<24); float f; std::memcpy(&f, &v, 4); return f; }
    size_t Remaining() const { return o <= n ? n - o : 0; }
    Vec3 V3() { Vec3 v; v.x = F32(); v.y = F32(); v.z = F32(); return v; }
    std::string Str() { std::string s; while (o < n && p[o] != 0) s.push_back((char)p[o++]); if (o < n) ++o; return s; }
};

static bool IsIdentChar(uint8_t c) { return std::isalnum(c) || c == '_'; }

// Scans the raw byte stream for "CAct" + [A-Za-z0-9_]+ immediately followed by
// a NUL - the same fuzzy record-boundary heuristic as findRecords() in seq.js,
// since the container header gives no per-node byte offsets.
static std::vector<std::pair<size_t, std::string>> FindRecords(const std::string& buf) {
    std::vector<std::pair<size_t, std::string>> out;
    const size_t n = buf.size();
    size_t i = 0;
    while (i + 4 <= n) {
        if (buf.compare(i, 4, "CAct") == 0) {
            size_t j = i + 4;
            while (j < n && IsIdentChar((uint8_t)buf[j])) ++j;
            if (j > i + 4 && j < n && buf[j] == '\0') {
                out.emplace_back(i, buf.substr(i, j - i));
                i = j + 1;
                continue;
            }
        }
        ++i;
    }
    return out;
}

// Full field-accurate parse of a CActParticle/CActParticle_IF body (spec
// verified against real .seq files: 99.6% exact byte consumption). Glow/haze
// were added to the format later, so their presence is detected by peeking for
// the 0.1 sentinel that marks the next block (header4) instead of always
// reading them.
static void ReadParticle(const std::string& body, Node& node) {
    Reader r(body);
    node.version = r.F32(); node.a = r.U32(); node.b = r.U32(); node.nodeName = r.Str();
    node.beginTime = r.U32(); node.endTime = r.U32();
    float header3 = r.F32(); r.F32();   // header3 marker, structureMod1 (unused)
    node.renderType = r.U32(); node.textureAnimType = r.U32(); node.blendType = r.U32();
    r.U32();                            // lifetimeVar
    r.F32(); r.F32(); r.F32();          // emitterSpeedVar, sizeVarX, sizeVarY
    r.F32(); r.F32(); r.F32(); r.F32(); // colorVarC/M/Y/A
    r.F32();                            // rotateVar
    r.Bool(); r.U32(); r.U8(); r.U32(); // attachTarget, maxParticles, scaleFlags, textureAnimDelay
    r.Bool(); r.U32(); r.F32();         // fogState, cullRadius, sizeVar
    r.Bool(); r.Bool(); r.Bool(); r.Bool();   // sizeLock, colorLock, alphaLock, rotateLock
    r.U8(); r.U8(); r.U8(); r.U8();     // minimumTimeDelta + 3 pad bytes

    const bool atHeader4 = r.Remaining() >= 4 && std::fabs(r.PeekF32() - 0.1f) < 1e-6f;
    if (!atHeader4) { r.Bool(); r.Bool(); }   // glow, haze (older files only)
    float header4 = r.F32();

    r.U32();          // emitterTime
    r.Bool();         // keyLerp
    uint32_t nEmit = r.U32();
    node.emitters.reserve(nEmit);
    for (uint32_t k = 0; k < nEmit && !r.bad; ++k) {
        Emitter e;
        e.frame = r.U32(); e.continuance = r.Bool(); e.origin = r.V3(); e.coneDir = r.V3();
        e.coneTheta = r.F32(); e.speed = r.F32(); e.emitterType = r.U32();
        e.minWidth = r.F32(); e.maxWidth = r.F32(); e.minHeight = r.F32(); e.maxHeight = r.F32();
        e.gravity = r.V3(); e.attractive = r.F32();
        e.lifetime = r.U32(); e.perSecond = r.U32(); e.textureKey = r.U32();
        node.emitters.push_back(e);
    }
    float header5 = r.F32();

    uint32_t nKey = r.U32();
    node.keys.reserve(nKey);
    for (uint32_t k = 0; k < nKey && !r.bad; ++k) {
        Key key;
        key.frame = r.F32(); key.sizeX = r.F32(); key.sizeY = r.F32();
        key.r = r.F32(); key.g = r.F32(); key.b = r.F32(); key.a = r.F32(); key.rotate = r.F32();
        node.keys.push_back(key);
    }
    float header6 = r.F32();

    uint32_t nTex = r.U32();
    node.textures.reserve(nTex);
    for (uint32_t t = 0; t < nTex && !r.bad; ++t) {
        TexRef tex;
        tex.path = r.Str();
        size_t slash = tex.path.find_last_of("/\\");
        tex.file = (slash == std::string::npos) ? tex.path : tex.path.substr(slash + 1);
        for (int i = 0; i < 8; ++i) tex.uv[i] = r.F32();
        node.textures.push_back(std::move(tex));
    }

    auto m = [](float v, float exp) { return std::fabs(v - exp) < 1e-6f; };
    const bool markersOk = m(header3, 0.1f) && m(header4, 0.1f) && m(header5, 0.1f) && m(header6, 0.2f);
    node.isParticle = true;
    node.ok = !r.bad && markersOk && r.Remaining() == 0;
}

// Finds a plausible file path embedded anywhere in a node's raw bytes: a run
// of printable ASCII ending in a known asset extension, NUL-terminated. Used
// for node types whose exact struct isn't pinned down (NodeLoader), since the
// path string is there regardless of what comes before/after it.
static std::string FindEmbeddedAsset(const std::string& body, const char* ext) {
    size_t extLen = std::strlen(ext);
    for (size_t end = extLen; end <= body.size(); ++end) {
        if (end < body.size() && body[end] != '\0') continue;
        if (end < extLen) continue;
        bool matches = true;
        for (size_t i = 0; i < extLen; ++i)
            if (tolower((unsigned char)body[end - extLen + i]) != ext[i]) { matches = false; break; }
        if (!matches) continue;
        size_t start = end - extLen;
        while (start > 0 && (unsigned char)body[start - 1] >= 0x20 && (unsigned char)body[start - 1] < 0x7f) --start;
        if (end - start > 3) return body.substr(start, end - start);
    }
    return "";
}

static void ParseNode(const std::string& body, Node& node) {
    Reader r(body);
    node.version = r.F32(); node.a = r.U32(); node.b = r.U32(); node.nodeName = r.Str();
    if (node.className == "CActParticle" || node.className == "CActParticle_IF")
        ReadParticle(body, node);
    else if (node.className == "CActNodeLoader" || node.className == "CActNodeLoader_IF")
        node.modelFile = FindEmbeddedAsset(body, ".scn");
}

Sequence Parse(const char* path) {
    Sequence sc;
    std::ifstream f(path, std::ios::binary);
    if (!f) { sc.error = "open failed"; return sc; }
    std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Reader r(buf);

    sc.version  = r.F32();
    sc.name     = r.Str();
    sc.flag     = r.U8();
    sc.duration = r.U32();
    sc.nodeCount = r.U32();

    auto recs = FindRecords(buf);
    sc.nodes.reserve(recs.size());
    for (size_t i = 0; i < recs.size(); ++i) {
        size_t bs = recs[i].first + recs[i].second.size() + 1;   // past "ClassName\0"
        size_t be = (i + 1 < recs.size()) ? recs[i + 1].first : buf.size();
        Node node; node.className = recs[i].second;
        ParseNode(buf.substr(bs, be - bs), node);
        sc.nodes.push_back(std::move(node));
    }
    sc.ok = (sc.nodeCount == sc.nodes.size()) && std::fabs(sc.version - 0.1f) < 1e-6f;
    return sc;
}

}  // namespace seq
