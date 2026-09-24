// Game states implementation. Bodies are stubs until each GameState is reversed.
#include "scenes/scenes.h"
#include "gui/gui_render.h"
#include "render/irenderer.h"
#include "world/world.h"
#include "input/input.h"
#include "net/config.h"
#include "scn/scn.h"
#include "data/weapon_attach.h"
#include "lua/lua_weapon_fire.h"
#include "app/dbg.h"
#include "audio/audio.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <cctype>

static std::string Basename(const std::string& p) {
    auto s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

static bool CharacterHasClip(const scn::Scene& scene, const std::string& clip) {
    return !clip.empty() && std::find(scene.animNames.begin(), scene.animNames.end(), clip) != scene.animNames.end();
}

static void NormalizeCharacterClip(const scn::Scene& scene, std::string& clip, const char* fallback = nullptr) {
    if (clip.empty()) return;
    if (CharacterHasClip(scene, clip)) return;
    static std::string lastLogged;
    if (clip.size() > 2 && clip.compare(clip.size() - 2, 2, "_1") == 0) {
        const std::string base = clip.substr(0, clip.size() - 2);
        if (CharacterHasClip(scene, base)) {
            const std::string key = clip + "->" + base;
            if (lastLogged != key) {
                lastLogged = key;
                DbgLog("[anim] clip '%s' no existe en male_bip.scn -> usando '%s'", clip.c_str(), base.c_str());
            }
            clip = base;
            return;
        }
    }
    if (fallback && CharacterHasClip(scene, fallback)) {
        const std::string key = clip + "->" + fallback;
        if (lastLogged != key) {
            lastLogged = key;
            DbgLog("[anim] clip '%s' no existe en male_bip.scn -> standby fallback '%s'", clip.c_str(), fallback);
        }
        clip = fallback;
    }
}

static bool WeaponHasClip(const scn::Scene& scene, const std::string& clip) {
    return !clip.empty() && std::find(scene.animNames.begin(), scene.animNames.end(), clip) != scene.animNames.end();
}

// Character clips and weapon clips are separate namespaces. The original
// actorstates files explicitly give weapons their own idle/run/attack clips;
// only use the character clip when this particular SCN does not provide one.
static scn::Pose MakeWeaponPose(const scn::Scene& scene, const anim::WeaponAnim& wa,
                                const scn::Pose& characterPose, anim::ActorState state,
                                bool moving, float inX, float inZ,
                                const anim::ActorAnim* attack, bool weapon2Active) {
    scn::Pose out = characterPose;
    auto use = [&](const std::string& clip) {
        if (clip.empty() || !WeaponHasClip(scene, clip)) return false;
        out.clipA = clip; out.clipB.clear(); out.blend = 0.f;
        return true;
    };

    if ((attack || weapon2Active) && use(characterPose.clipA)) return out;
    if (attack && use(attack->clip)) return out;

    if ((state == anim::STATE_RUN || state == anim::STATE_FASTRUN) && moving) {
        const auto it = wa.run.find(anim::RunDir(inX, inZ));
        if (it != wa.run.end() && use(it->second.clip)) return out;
    }
    if (state == anim::STATE_NORMAL || state == anim::STATE_IDLE ||
        state == anim::STATE_STANDBY || state == anim::STATE_WAIT) {
        if (wa.idle && use(wa.idle->clip)) return out;
    }
    if (use(characterPose.clipA)) {
        if (!characterPose.clipB.empty() && WeaponHasClip(scene, characterPose.clipB)) return characterPose;
        return out;
    }
    if (wa.idle && use(wa.idle->clip)) return out;
    return scn::Pose{}; // bind pose is safer than applying a foreign clip name
}
// Parse _eu_intro.x7 -> list of <intro> blocks (play_time, frame_time, frames).
static std::vector<IntroBlock> LoadIntro(const char* path) {
    std::vector<IntroBlock> out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t i = 0;
    while ((i = s.find("<intro", i)) != std::string::npos) {
        size_t end = s.find("</intro>", i); if (end == std::string::npos) end = s.size();
        IntroBlock b;
        auto attr = [&](const char* k, int def) {
            size_t a = s.find(k, i); if (a == std::string::npos || a > end) return def;
            a = s.find('"', a); if (a == std::string::npos) return def;
            size_t e = s.find('"', a + 1);
            return std::atoi(s.substr(a + 1, e - a - 1).c_str());
        };
        b.playTime = attr("play_time=", 1500);
        b.frameTime = attr("frame_time=", 60);
        for (size_t p = i; (p = s.find("textrue=", p)) != std::string::npos && p < end; ) {
            size_t a = s.find('"', p), e = s.find('"', a + 1);
            b.frames.push_back(Basename(s.substr(a + 1, e - a - 1)));
            p = e + 1;
        }
        if (!b.frames.empty()) out.push_back(b);
        i = end + 8;
    }
    return out;
}

void IntroState::OnEnter() {
    blocks = LoadIntro("resources/resources/intro/_eu_intro.x7");
    start = GetTickCount(); blockIdx = 0;
    DbgLog("[intro] blocks=%zu", blocks.size());
}
void IntroState::OnUpdate() {
    if ((input::g.anyKey || input::g.click) && world && next) { world->ChangeScene(next); return; }
    if (blocks.empty()) { if (world && next) world->ChangeScene(next); return; }
    if (GetTickCount() - start >= (unsigned)blocks[blockIdx].playTime) {
        if (++blockIdx >= blocks.size()) { if (world && next) world->ChangeScene(next); }
        else start = GetTickCount();
    }
}
void IntroState::OnRender(IRenderer* r) {
    if (!r || blocks.empty() || blockIdx >= blocks.size()) return;
    const IntroBlock& b = blocks[blockIdx];
    int fi = b.frameTime > 0 ? (int)((GetTickCount() - start) / b.frameTime) : 0;
    if (fi >= (int)b.frames.size()) fi = (int)b.frames.size() - 1;   // hold last frame
    int t = r->LoadTexture(b.frames[fi].c_str());
    if (t < 0) return;
    int tw = 0, th = 0; r->GetTextureSize(t, tw, th);
    if (tw <= 0) { tw = 512; th = 384; }
    r->DrawImage((r->width - tw) / 2, (r->height - th) / 2, tw, th, t, 0, 0, 1, 1);  // centered, native size
}
void IntroState::OnLeave()  {}

void TitleState::OnEnter() {
    character = scn::Parse("resources/resources/model/interface/title_cha00.scn");
    animStart = GetTickCount();
    DbgLog("[scn] title_cha00 ok=%d models=%zu", (int)character.ok, character.models.size());
    for (size_t i = 0; i < character.models.size() && i < 3; ++i) {
        const scn::Model& m = character.models[i];
        DbgLog("[scn] model %s anims=%zu", m.name.c_str(), m.anims.size());
        for (const scn::Anim& a : m.anims)
            DbgLog("[scn]   anim '%s' dur=%u trans=%zu rot=%zu scale=%zu",
                   a.name.c_str(), a.duration, a.trans.size(), a.rot.size(), a.scale.size());
    }
}
void TitleState::OnUpdate() {
    if ((input::g.anyKey || input::g.click) && world && next) {
        // Connect to the auth server (ProudNet). CTitleState_ConnectAuthServer.
        std::string ip = "127.0.0.1"; int port = 28002;
        cfg::ServerEndpoint(ip, port);
        bool ok = proud::g_client.Connect(ip.c_str(), (uint16_t)port);
        DbgLog("[flow] Title -> connect %s:%d ok=%d -> ServerSelect", ip.c_str(), port, (int)ok);
        // ponytail: handshake continues in g_client.Poll() (Engine). Full auth
        // needs the RSA session-key exchange (stub in proud_client). Move to the
        // server list regardless so the flow is navigable.
        world->ChangeScene(next);
    }
}
// Render a parsed .scn as a textured 3D character. Auto-frames the mesh with a
// perspective camera so it's always on screen; the exact camera (and skinning +
// animation) get pulled from the reference next.
// ponytail: node matrices skipped (world=identity), matching the old 2D path;
// wire m.matrix once the .scn matrix convention (row/col-vector) is confirmed
// against the binary, else the character comes out transposed.
// Transform a model-space point by a row-major (row-vector) node matrix.
static void XformPt(const float* m, const scn::Vec3& v, float o[3]) {
    o[0] = v.x*m[0] + v.y*m[4] + v.z*m[8]  + m[12];
    o[1] = v.x*m[1] + v.y*m[5] + v.z*m[9]  + m[13];
    o[2] = v.x*m[2] + v.y*m[6] + v.z*m[10] + m[14];
}
// row-major row-vector 4x4 multiply: C = A * B (apply A, then B).
static void Mat4MulS(const float* A, const float* B, float* C) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            C[r*4+c] = A[r*4+0]*B[0*4+c] + A[r*4+1]*B[1*4+c] + A[r*4+2]*B[2*4+c] + A[r*4+3]*B[3*4+c];
}

// Floor height directly under (sx,sz): same downward query as world.html. The
// visible floor is the authoritative support surface; oct_* remains the wall
// hull, otherwise ledges become artificially solid and never release FALL.
static bool FloorYAt(const std::vector<scn::Scene>& map, float sx, float sz, float ceiling, float& outY) {
    float best = -1e9f; bool found = false;
    for (const scn::Scene& sc : map)
        for (const scn::Model& m : sc.models) {
            if (m.name.rfind("oct_", 0) == 0) continue;
            for (const scn::Face& f : m.faces) {
                if (f.a >= m.vertices.size() || f.b >= m.vertices.size() || f.c >= m.vertices.size()) continue;
                float w0[3], w1[3], w2[3];
                XformPt(m.matrix, m.vertices[f.a], w0);
                XformPt(m.matrix, m.vertices[f.b], w1);
                XformPt(m.matrix, m.vertices[f.c], w2);
                float ux=w1[0]-w0[0], uy=w1[1]-w0[1], uz=w1[2]-w0[2];
                float vx=w2[0]-w0[0], vy=w2[1]-w0[1], vz=w2[2]-w0[2];
                float ny = uz*vx - ux*vz;
                float nx = uy*vz - uz*vy, nz = ux*vy - uy*vx;
                if (std::fabs(ny) <= std::fabs(nx) || std::fabs(ny) <= std::fabs(nz)) continue;
                float d = (w1[2]-w2[2])*(w0[0]-w2[0]) + (w2[0]-w1[0])*(w0[2]-w2[2]);
                if (std::fabs(d) < 1e-4f) continue;
                float a = ((w1[2]-w2[2])*(sx-w2[0]) + (w2[0]-w1[0])*(sz-w2[2])) / d;
                float b = ((w2[2]-w0[2])*(sx-w2[0]) + (w0[0]-w2[0])*(sz-w2[2])) / d;
                float c = 1.f - a - b;
                if (a < -0.01f || b < -0.01f || c < -0.01f) continue;
                float y = a*w0[1] + b*w1[1] + c*w2[1];
                if (y <= ceiling && y > best) { best = y; found = true; }
            }
        }
    outY = best; return found;
}

// Critically-damped smooth follow (Unity-style SmoothDamp), one axis. Gives the
// S4 "camera trails/banks into the turn" feel without overshoot jitter.
static float SmoothDamp(float cur, float target, float& vel, float smoothTime, float dt) {
    if (smoothTime < 1e-4f) smoothTime = 1e-4f;
    float omega = 2.f / smoothTime;
    float x = omega * dt;
    float exp = 1.f / (1.f + x + 0.48f*x*x + 0.235f*x*x*x);
    float change = cur - target;
    float temp = (vel + omega*change) * dt;
    vel = (vel - omega*temp) * exp;
    return target + (change + temp) * exp;
}

// Closest point on triangle (a,b,c) to p (Ericson, Real-Time Collision Detection).
static void ClosestPtTri(const float p[3], const float a[3], const float b[3], const float c[3], float out[3]) {
    auto sub = [](const float* u, const float* v, float* o){ o[0]=u[0]-v[0]; o[1]=u[1]-v[1]; o[2]=u[2]-v[2]; };
    auto dot = [](const float* u, const float* v){ return u[0]*v[0]+u[1]*v[1]+u[2]*v[2]; };
    float ab[3],ac[3],ap[3]; sub(b,a,ab); sub(c,a,ac); sub(p,a,ap);
    float d1=dot(ab,ap), d2=dot(ac,ap);
    if (d1<=0 && d2<=0) { out[0]=a[0];out[1]=a[1];out[2]=a[2]; return; }
    float bp[3]; sub(p,b,bp); float d3=dot(ab,bp), d4=dot(ac,bp);
    if (d3>=0 && d4<=d3) { out[0]=b[0];out[1]=b[1];out[2]=b[2]; return; }
    float vc=d1*d4-d3*d2;
    if (vc<=0 && d1>=0 && d3<=0) { float v=d1/(d1-d3); for(int i=0;i<3;++i) out[i]=a[i]+v*ab[i]; return; }
    float cp[3]; sub(p,c,cp); float d5=dot(ab,cp), d6=dot(ac,cp);
    if (d6>=0 && d5<=d6) { out[0]=c[0];out[1]=c[1];out[2]=c[2]; return; }
    float vb=d5*d2-d1*d6;
    if (vb<=0 && d2>=0 && d6<=0) { float w=d2/(d2-d6); for(int i=0;i<3;++i) out[i]=a[i]+w*ac[i]; return; }
    float va=d3*d6-d5*d4;
    if (va<=0 && (d4-d3)>=0 && (d5-d6)>=0) { float w=(d4-d3)/((d4-d3)+(d5-d6)); for(int i=0;i<3;++i) out[i]=b[i]+w*(c[i]-b[i]); return; }
    float denom=1.f/(va+vb+vc), v=vb*denom, w=vc*denom;
    for(int i=0;i<3;++i) out[i]=a[i]+ab[i]*v+ac[i]*w;
}

// Collide-and-slide (S4 FUN_01251FE0, Kasper-Fauerby): resolve sphere (center `c`,
// radius r) against the precomputed world triangles. Push out of each penetration
// by (pen + skin) along the contact normal and project `vel` (a rate vector) onto
// each contact plane so the sphere SLIDES instead of sticking. Sets *grounded on a
// floor-ish (up) normal. Skin = 0.015*r (DAT_0256B1B8 veryCloseDistance). 5 iters.
static void ResolveEllipsoid(const std::vector<CollTri>& tris, float c[3], const float eR[3], float vel[3]) {
    const float skin = 0.015f;                      // DAT_0256b1b8 (unit-sphere space, r==1)
    const float rmax = (eR[0] > eR[1] ? eR[0] : eR[1]) + 1.f;
    float ic[3] = { c[0]/eR[0], c[1]/eR[1], c[2]/eR[2] };   // into ellipsoid space
    for (int it = 0; it < 5; ++it) {                // maxRecursion 5
        bool anyHit = false;
        for (const CollTri& t : tris) {
            if (c[0]+rmax < t.bmin[0] || c[0]-rmax > t.bmax[0] ||
                c[1]+rmax < t.bmin[1] || c[1]-rmax > t.bmax[1] ||
                c[2]+rmax < t.bmin[2] || c[2]-rmax > t.bmax[2]) continue;   // world-space AABB broad-phase
            float a[3]={t.v0[0]/eR[0],t.v0[1]/eR[1],t.v0[2]/eR[2]};
            float b[3]={t.v1[0]/eR[0],t.v1[1]/eR[1],t.v1[2]/eR[2]};
            float g[3]={t.v2[0]/eR[0],t.v2[1]/eR[1],t.v2[2]/eR[2]};
            float cp[3]; ClosestPtTri(ic, a, b, g, cp);
            float dx=ic[0]-cp[0], dy=ic[1]-cp[1], dz=ic[2]-cp[2], l2=dx*dx+dy*dy+dz*dz;
            if (l2 >= 1.f || l2 < 1e-10f) continue;
            float l=std::sqrt(l2), nx=dx/l, ny=dy/l, nz=dz/l, pen=1.f-l;
            ic[0]+=nx*(pen+skin); ic[1]+=ny*(pen+skin); ic[2]+=nz*(pen+skin);
            // slide the (world-space) velocity along the world contact normal
            float wn[3]={nx/eR[0], ny/eR[1], nz/eR[2]};
            float wl=std::sqrt(wn[0]*wn[0]+wn[1]*wn[1]+wn[2]*wn[2]);
            if (wl > 1e-8f && vel) { wn[0]/=wl; wn[1]/=wl; wn[2]/=wl;
                float vn = vel[0]*wn[0]+vel[1]*wn[1]+vel[2]*wn[2];
                if (vn < 0.f) { vel[0]-=vn*wn[0]; vel[1]-=vn*wn[1]; vel[2]-=vn*wn[2]; } }
            anyHit = true;
        }
        if (!anyHit) break;
    }
    c[0]=ic[0]*eR[0]; c[1]=ic[1]*eR[1]; c[2]=ic[2]*eR[2];   // back to world
}

// Continuous unit-sphere sweep in ellipsoid space. This is the missing query
// behind FUN_01255e50/FUN_01251fe0: face, edge and vertex TOIs are considered,
// then the earliest contact is returned in world-space normal form.
static bool SweepEllipsoid(const std::vector<CollTri>& tris,
                           float x, float y, float z,
                           float dx, float dy, float dz,
                           float rx, float ry, float rz,
                           float& outT, float& outNx, float& outNy, float& outNz) {
    if (rx <= 1e-4f || ry <= 1e-4f || rz <= 1e-4f) return false;
    const float p[3] = { x / rx, y / ry, z / rz };
    const float v[3] = { dx / rx, dy / ry, dz / rz };
    const float wx0[3] = { x, y, z };
    const float wx1[3] = { x + dx, y + dy, z + dz };
    const float ex = (std::max)(rx, 0.015f), ey = (std::max)(ry, 0.015f), ez = (std::max)(rz, 0.015f);
    float best = 1.f;
    const CollTri* bestTri = nullptr;

    auto dot3 = [](const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };
    auto add3 = [](const float* a, const float* b, float s, float* o) {
        o[0] = a[0] + b[0]*s; o[1] = a[1] + b[1]*s; o[2] = a[2] + b[2]*s;
    };
    auto pointInTri = [&](const float* q, const float* a, const float* b, const float* c, const float* n) {
        float ab[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]};
        float bc[3] = {c[0]-b[0], c[1]-b[1], c[2]-b[2]};
        float ca[3] = {a[0]-c[0], a[1]-c[1], a[2]-c[2]};
        float aq[3] = {q[0]-a[0], q[1]-a[1], q[2]-a[2]};
        float bq[3] = {q[0]-b[0], q[1]-b[1], q[2]-b[2]};
        float cq[3] = {q[0]-c[0], q[1]-c[1], q[2]-c[2]};
        float c0[3] = {ab[1]*aq[2]-ab[2]*aq[1], ab[2]*aq[0]-ab[0]*aq[2], ab[0]*aq[1]-ab[1]*aq[0]};
        float c1[3] = {bc[1]*bq[2]-bc[2]*bq[1], bc[2]*bq[0]-bc[0]*bq[2], bc[0]*bq[1]-bc[1]*bq[0]};
        float c2[3] = {ca[1]*cq[2]-ca[2]*cq[1], ca[2]*cq[0]-ca[0]*cq[2], ca[0]*cq[1]-ca[1]*cq[0]};
        float d0 = dot3(c0, n), d1 = dot3(c1, n), d2 = dot3(c2, n);
        return (d0 >= -1e-4f && d1 >= -1e-4f && d2 >= -1e-4f) ||
               (d0 <=  1e-4f && d1 <=  1e-4f && d2 <=  1e-4f);
    };
    auto consider = [&](float t, const CollTri& tri) {
        if (t >= -1e-5f && t < best) { best = (std::max)(0.f, t); bestTri = &tri; }
    };
    auto solveQuadratic = [&](float a, float b, float c, float lo, float hi, const CollTri& tri) {
        if (std::fabs(a) < 1e-8f) {
            if (std::fabs(b) >= 1e-8f) {
                float t = -c / b;
                if (t >= lo - 1e-5f && t <= hi + 1e-5f) consider(t, tri);
            }
            return;
        }
        float disc = b*b - 4.f*a*c;
        if (disc < 0.f) return;
        float root = std::sqrt((std::max)(0.f, disc));
        float t0 = (-b - root) / (2.f*a), t1 = (-b + root) / (2.f*a);
        if (t0 >= lo - 1e-5f && t0 <= hi + 1e-5f) consider(t0, tri);
        if (t1 >= lo - 1e-5f && t1 <= hi + 1e-5f) consider(t1, tri);
    };

    for (const CollTri& tri : tris) {
        float lo[3] = { (std::min)(wx0[0], wx1[0]) - ex, (std::min)(wx0[1], wx1[1]) - ey, (std::min)(wx0[2], wx1[2]) - ez };
        float hi[3] = { (std::max)(wx0[0], wx1[0]) + ex, (std::max)(wx0[1], wx1[1]) + ey, (std::max)(wx0[2], wx1[2]) + ez };
        if (hi[0] < tri.bmin[0] || lo[0] > tri.bmax[0] ||
            hi[1] < tri.bmin[1] || lo[1] > tri.bmax[1] ||
            hi[2] < tri.bmin[2] || lo[2] > tri.bmax[2]) continue;

        float a[3] = {tri.v0[0]/rx, tri.v0[1]/ry, tri.v0[2]/rz};
        float b[3] = {tri.v1[0]/rx, tri.v1[1]/ry, tri.v1[2]/rz};
        float c[3] = {tri.v2[0]/rx, tri.v2[1]/ry, tri.v2[2]/rz};
        float ab[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]};
        float ac[3] = {c[0]-a[0], c[1]-a[1], c[2]-a[2]};
        float n[3] = {ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]};
        float nl = std::sqrt(dot3(n,n));
        if (nl < 1e-8f) continue;
        n[0]/=nl; n[1]/=nl; n[2]/=nl;

        float cp[3]; ClosestPtTri(p, a, b, c, cp);
        float initial[3] = {p[0]-cp[0], p[1]-cp[1], p[2]-cp[2]};
        // Contact exactly at the skin is valid support, not penetration. The
        // old <= test made a grounded actor re-hit the floor on every XZ move.
        if (dot3(initial, initial) < 1.f - 1e-4f) consider(0.f, tri);

        float planeD = dot3(a, n), signedDist = dot3(p, n) - planeD, ndv = dot3(v, n);
        if (std::fabs(ndv) > 1e-8f) {
            float t0 = (-1.f - signedDist) / ndv, t1 = (1.f - signedDist) / ndv;
            if (t0 > t1) std::swap(t0, t1);
            t0 = (std::max)(0.f, t0); t1 = (std::min)(1.f, t1);
            if (t0 <= t1 && t0 < best) {
                for (float t : {t0, t1}) {
                    float q[3]; add3(p, v, t, q);
                    float d = signedDist + ndv*t;
                    float side = d >= 0.f ? 1.f : -1.f;
                    float contact[3] = {q[0]-n[0]*side, q[1]-n[1]*side, q[2]-n[2]*side};
                    if (pointInTri(contact, a, b, c, n)) consider(t, tri);
                }
            }
        }

        for (const float* q : {a, b, c}) {
            float q0[3] = {p[0]-q[0], p[1]-q[1], p[2]-q[2]};
            solveQuadratic(dot3(v,v), 2.f*dot3(q0,v), dot3(q0,q0)-1.f, 0.f, 1.f, tri);
        }
        const float* edges[3][2] = {{a,b},{b,c},{c,a}};
        for (auto& edge : edges) {
            float e[3] = {edge[1][0]-edge[0][0], edge[1][1]-edge[0][1], edge[1][2]-edge[0][2]};
            float ee = dot3(e,e); if (ee < 1e-8f) continue;
            float q0[3] = {p[0]-edge[0][0], p[1]-edge[0][1], p[2]-edge[0][2]};
            float u0 = dot3(q0,e)/ee, u1 = dot3(v,e)/ee;
            float tlo = 0.f, thi = 1.f;
            if (std::fabs(u1) < 1e-8f) { if (u0 < 0.f || u0 > 1.f) continue; }
            else {
                float ta = (0.f-u0)/u1, tb = (1.f-u0)/u1;
                if (ta > tb) std::swap(ta,tb);
                tlo = (std::max)(tlo, ta); thi = (std::min)(thi, tb);
                if (tlo > thi) continue;
            }
            float perp0[3] = {q0[0]-e[0]*u0, q0[1]-e[1]*u0, q0[2]-e[2]*u0};
            float perpV[3] = {v[0]-e[0]*u1, v[1]-e[1]*u1, v[2]-e[2]*u1};
            solveQuadratic(dot3(perpV,perpV), 2.f*dot3(perp0,perpV), dot3(perp0,perp0)-1.f, tlo, thi, tri);
        }
    }
    if (!bestTri || best >= 1.f) return false;

    float a[3] = {bestTri->v0[0]/rx, bestTri->v0[1]/ry, bestTri->v0[2]/rz};
    float b[3] = {bestTri->v1[0]/rx, bestTri->v1[1]/ry, bestTri->v1[2]/rz};
    float c[3] = {bestTri->v2[0]/rx, bestTri->v2[1]/ry, bestTri->v2[2]/rz};
    float at[3] = {p[0]+v[0]*best, p[1]+v[1]*best, p[2]+v[2]*best};
    float cp[3]; ClosestPtTri(at, a, b, c, cp);
    float ne[3] = {at[0]-cp[0], at[1]-cp[1], at[2]-cp[2]};
    float nl = std::sqrt(dot3(ne,ne));
    if (nl < 1e-6f) {
        float ab[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]};
        float ac[3] = {c[0]-a[0], c[1]-a[1], c[2]-a[2]};
        ne[0]=ab[1]*ac[2]-ab[2]*ac[1]; ne[1]=ab[2]*ac[0]-ab[0]*ac[2]; ne[2]=ab[0]*ac[1]-ab[1]*ac[0];
        nl = std::sqrt(dot3(ne,ne));
    }
    if (nl < 1e-6f) return false;
    ne[0]/=nl; ne[1]/=nl; ne[2]/=nl;
    float nw[3] = {ne[0]/rx, ne[1]/ry, ne[2]/rz};
    float wl = std::sqrt(dot3(nw,nw));
    if (wl < 1e-6f) return false;
    nw[0]/=wl; nw[1]/=wl; nw[2]/=wl;
    if (nw[0]*dx + nw[1]*dy + nw[2]*dz > 0.f) { nw[0]=-nw[0]; nw[1]=-nw[1]; nw[2]=-nw[2]; }
    outT = best; outNx = nw[0]; outNy = nw[1]; outNz = nw[2];
    return true;
}

// Precompute every static map triangle to world space (called once at map load).
// onlyOct: collide against the client's own hull (meshes named oct_*, shipped in
// *_oct.scn) instead of the visible geometry. Per the .scn spec, oct_*/octree* ARE
// the collision meshes; occlusion is a separate file (*_occlusion.scn).
static void BuildCollTris(const std::vector<scn::Scene>& map, std::vector<CollTri>& out, bool onlyOct = false) {
    for (const scn::Scene& sc : map)
        for (const scn::Model& m : sc.models) {
            if ((m.name.rfind("oct_", 0) == 0) != onlyOct) continue;
            // La matriz del modelo SI se aplica, igual que en el cliente JS. Probe
            // a saltearla para la malla oct y rompio la colision: con la matriz
            // puesta, los 16 marcadores de spawn tienen piso debajo (16/16); sin
            // ella, no. Su traslacion (-2200,0,-200 en square) es colocacion real.
            for (const scn::Face& f : m.faces) {
                if (f.a >= m.vertices.size() || f.b >= m.vertices.size() || f.c >= m.vertices.size()) continue;
                CollTri t;
                XformPt(m.matrix, m.vertices[f.a], t.v0);
                XformPt(m.matrix, m.vertices[f.b], t.v1);
                XformPt(m.matrix, m.vertices[f.c], t.v2);
                float ux=t.v1[0]-t.v0[0], uy=t.v1[1]-t.v0[1], uz=t.v1[2]-t.v0[2];
                float vx=t.v2[0]-t.v0[0], vy=t.v2[1]-t.v0[1], vz=t.v2[2]-t.v0[2];
                t.n[0]=uy*vz-uz*vy; t.n[1]=uz*vx-ux*vz; t.n[2]=ux*vy-uy*vx;
                float nl=std::sqrt(t.n[0]*t.n[0]+t.n[1]*t.n[1]+t.n[2]*t.n[2]);
                if (nl > 1e-6f) { t.n[0]/=nl; t.n[1]/=nl; t.n[2]/=nl; }
                for (int k=0;k<3;++k) {
                    t.bmin[k]=(std::min)({t.v0[k],t.v1[k],t.v2[k]});
                    t.bmax[k]=(std::max)({t.v0[k],t.v1[k],t.v2[k]});
                }
                out.push_back(t);
            }
        }
}

// Render an assembled S4 character: the `skel` scene supplies the 81-bone Biped
// skeleton (male_bip.scn = skeleton+anims+default weapon, NO body mesh); each
// `parts` scene (body/leg/hand/foot/face/hair) supplies skinned meshes bound to
// that skeleton by bone name. `tick` samples animation; 0 = bind pose.
// One drawable piece of the assembled character (already skinned/attached).
struct CharPiece { const scn::Model* m; const std::string* dir; std::vector<scn::Vec3> verts; std::array<float,16> world; };

// Skin/attach every mesh of every body part against the shared skeleton at
// `tick`, producing world-space pieces (model space, before camera).
static void GatherCharacterPieces(const scn::Scene& skel, const std::vector<scn::Scene>& parts,
                                  const scn::AnimPose& pose, std::vector<CharPiece>& pieces,
                                  const scn::AnimPose* lowerPose = nullptr) {
    for (const scn::Scene& part : parts) {
        for (const scn::Model& m : part.models) {
            CharPiece pc; pc.m = &m; pc.dir = &part.dir;
            pc.world = {}; pc.world[0]=pc.world[5]=pc.world[10]=pc.world[15]=1.f;
            if (!m.skin.empty()) {
                scn::SkinModel(m, skel, pose, lowerPose, pc.verts);   // skinned mesh -> bones place verts
            } else {
                pc.verts = m.vertices;
                // Rigid part (face/hair): world = model.matrix * dummyLocal * headWorld
                // (Hair_Bone_Dummy is a head-relative offset; head follows the clip).
                float dummyLocal[16], headWorld[16], tmp[16];
                bool haveDummy = !m.parent.empty() && scn::BoneWorldByName(part, m.parent, pose, dummyLocal);
                bool haveHead  = scn::BoneWorldByName(skel, "Bip01 Head", pose, headWorld);
                if (haveDummy && haveHead) { Mat4MulS(m.matrix, dummyLocal, tmp); Mat4MulS(tmp, headWorld, pc.world.data()); }
                else if (haveDummy)        { Mat4MulS(m.matrix, dummyLocal, pc.world.data()); }
                else                       { scn::SampleNode(m, pose.tickA, pc.world.data()); }
            }
            pieces.push_back(std::move(pc));
        }
    }
}

// Draw pre-gathered character pieces. `place16` (or nullptr) is an extra world
// transform applied on top of each piece (character position/rotation in a map).
static void DrawCharPieces(IRenderer* r, const std::vector<CharPiece>& pieces, const float* place16) {
    for (const CharPiece& pc : pieces) {
        const scn::Model& m = *pc.m;
        if (pc.verts.empty()) continue;
        float world[16];
        if (place16) Mat4MulS(pc.world.data(), place16, world);
        else         std::memcpy(world, pc.world.data(), sizeof(world));
        std::string texPath = (pc.dir && !pc.dir->empty()) ? (*pc.dir + "/" + m.texture) : m.texture;
        int tex = r->LoadTexture(texPath.c_str());
        bool hasUv = pc.verts.size() == m.uv1.size();
        std::vector<float> pu(pc.verts.size() * 5);
        for (size_t i = 0; i < pc.verts.size(); ++i) {
            pu[i*5+0]=pc.verts[i].x; pu[i*5+1]=pc.verts[i].y; pu[i*5+2]=pc.verts[i].z;
            pu[i*5+3]= hasUv ? m.uv1[i].u : 0.f;  pu[i*5+4]= hasUv ? m.uv1[i].v : 0.f;
        }
        std::vector<unsigned short> idx; idx.reserve(m.faces.size() * 3);
        for (const scn::Face& f : m.faces) { idx.push_back(f.a); idx.push_back(f.b); idx.push_back(f.c); }
        r->DrawMesh(pu.data(), (int)pc.verts.size(), idx.data(), (int)idx.size(), tex, world, m.renderFlag);
    }
}

// Character PREVIEW (server-select panel): auto-frames the camera to the model.
static void RenderScnCharacter(IRenderer* r, const scn::Scene& skel,
                               const std::vector<scn::Scene>& parts,
                               const std::string& anim, uint32_t tick) {
    if (!r || skel.bones.empty()) return;
    std::vector<CharPiece> pieces;
    scn::AnimPose pose; pose.clipA = anim; pose.tickA = tick;   // preview = single clip
    GatherCharacterPieces(skel, parts, pose, pieces);
    if (pieces.empty()) return;

    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (const CharPiece& pc : pieces)
        for (const scn::Vec3& v : pc.verts) {
            float w[3]; XformPt(pc.world.data(), v, w);
            lo[0]=(std::min)(lo[0],w[0]); hi[0]=(std::max)(hi[0],w[0]);
            lo[1]=(std::min)(lo[1],w[1]); hi[1]=(std::max)(hi[1],w[1]);
            lo[2]=(std::min)(lo[2],w[2]); hi[2]=(std::max)(hi[2],w[2]);
        }
    float c[3]  = { (lo[0]+hi[0])*0.5f, (lo[1]+hi[1])*0.5f, (lo[2]+hi[2])*0.5f };
    float ext   = (std::max)((std::max)(hi[0]-lo[0], hi[1]-lo[1]), hi[2]-lo[2]);
    if (ext <= 0.f) ext = 1.f;
    float dist  = ext * 1.7f;
    float eye[3] = { c[0], c[1], c[2] - dist };   // LH: camera on -Z looking toward +Z
    r->SetCamera3D(eye, c, 45.f, ext * 0.02f, dist * 4.f);
    DrawCharPieces(r, pieces, nullptr);
}

// Draw a static map scene (no skinning): each model by its own node matrix.
static void DrawMapScene(IRenderer* r, const scn::Scene& sc) {
    for (const scn::Model& m : sc.models) {
        if (m.vertices.empty() || m.faces.empty()) continue;
        if (m.name.rfind("oct_", 0) == 0) continue;   // collision octree meshes are invisible
        const size_t nv = m.vertices.size();
        const bool hasUv = nv == m.uv1.size();
        const bool hasUv2 = nv == m.uv2.size();
        // MULTI-TEXTURA: cada texture_item cubre [faceStart, faceStart+faceCount).
        // Se dibuja un lote por item; dibujar todo con la primera textura pintaba
        // mal el 59% de las caras del mapa (y el skybox entero con una sola cara).
        std::vector<scn::TexItem> items = m.texItems;
        if (items.empty()) {   // .scn viejo sin items: un solo rango con la textura del modelo
            scn::TexItem one; one.main = m.texture; one.side = m.sideTexture;
            one.faceStart = 0; one.faceCount = (uint32_t)m.faces.size();
            items.push_back(one);
        }
        std::vector<unsigned short> idx;
        std::vector<float> pu;
        for (const scn::TexItem& it : items) {
            if (it.faceCount == 0 || it.faceStart >= m.faces.size()) continue;
            const size_t end = (std::min)((size_t)it.faceStart + it.faceCount, m.faces.size());
            idx.clear(); idx.reserve((end - it.faceStart) * 3);
            for (size_t fi = it.faceStart; fi < end; ++fi) {
                const scn::Face& f = m.faces[fi];
                idx.push_back(f.a); idx.push_back(f.b); idx.push_back(f.c);
            }
            if (idx.empty()) continue;
            std::string texPath = sc.dir.empty() ? it.main : (sc.dir + "/" + it.main);
            int tex = r->LoadTexture(texPath.c_str());
            const bool hasLM = !it.side.empty() && hasUv2;
            if (hasLM) {   // main texture x baked lightmap (uv2) -> warm lit map
                std::string lmPath = sc.dir.empty() ? it.side : (sc.dir + "/" + it.side);
                int lm = r->LoadTexture(lmPath.c_str());
                pu.assign(nv * 7, 0.f);
                for (size_t i = 0; i < nv; ++i) {
                    pu[i*7+0]=m.vertices[i].x; pu[i*7+1]=m.vertices[i].y; pu[i*7+2]=m.vertices[i].z;
                    pu[i*7+3]= hasUv ? m.uv1[i].u : 0.f; pu[i*7+4]= hasUv ? m.uv1[i].v : 0.f;
                    pu[i*7+5]= m.uv2[i].u; pu[i*7+6]= m.uv2[i].v;
                }
                r->DrawMeshLightmap(pu.data(), (int)nv, idx.data(), (int)idx.size(), tex, lm, m.matrix, m.renderFlag);
            } else {
                pu.assign(nv * 5, 0.f);
                for (size_t i = 0; i < nv; ++i) {
                    pu[i*5+0]=m.vertices[i].x; pu[i*5+1]=m.vertices[i].y; pu[i*5+2]=m.vertices[i].z;
                    pu[i*5+3]= hasUv ? m.uv1[i].u : 0.f;  pu[i*5+4]= hasUv ? m.uv1[i].v : 0.f;
                }
                r->DrawMesh(pu.data(), (int)nv, idx.data(), (int)idx.size(), tex, m.matrix, m.renderFlag);
            }
        }
    }
}

void TitleState::OnRender(IRenderer* r) {
    if (!r) return;
    // Both are 1024x1024 composites with alpha -> full-screen (title_back00.scn
    // places them; full-screen is a faithful approximation of that transform).
    int bg = r->LoadTexture("title_back00.png");
    if (bg >= 0) r->DrawImage(0, 0, r->width, r->height, bg, 0, 0, 1, 1);

    // Character assembled from title_cha00.scn meshes (title_ani.png regions).
    // KNOWN-GOOD static 2D placement: the title art is a fixed pose, so the 3D
    // mesh + node/morph animation path scattered the cloth. Kept simple.
    // (The 3D engine SetCamera3D/DrawMesh/SkinModel stays for real lobby
    //  characters - RenderScnCharacter() below is ready when one is loaded.)
    // Smaller than 1:1 + a gentle idle sway of the WHOLE assembled character
    // (safe movement - moving the meshes individually is what scattered them).
    uint32_t tick = animStart ? (uint32_t)(GetTickCount() - animStart) : 0u;
    const float scale = 0.90f;                                       // bigger
    const float offX = 830.f + std::sin(tick * 0.0011f) * 5.f;       // pegged right (back bleeds off edge)
    const float offY = 430.f + std::sin(tick * 0.0016f) * 8.f;
    for (const scn::Model& m : character.models) {
        int tex = r->LoadTexture(m.texture.c_str());
        if (tex < 0 || m.vertices.size() != m.uv1.size()) continue;
        std::vector<float> pu(m.vertices.size() * 4);
        for (size_t i = 0; i < m.vertices.size(); ++i) {
            pu[i*4+0] = offX + m.vertices[i].x * scale;
            pu[i*4+1] = offY - m.vertices[i].y * scale;
            pu[i*4+2] = m.uv1[i].u;
            pu[i*4+3] = m.uv1[i].v;
        }
        std::vector<unsigned short> idx;
        idx.reserve(m.faces.size() * 3);
        for (const scn::Face& f : m.faces) { idx.push_back(f.a); idx.push_back(f.b); idx.push_back(f.c); }
        r->DrawTriangles(pu.data(), (int)m.vertices.size(), idx.data(), (int)idx.size(), tex);
    }

    r->DrawText(r->width / 2 - 240, r->height - 130, 600, 60, "PRESS ANY KEY TO START", 36, 0xFF7CF0F0);
}

static void DumpTree(const gui::Widget& w, int depth, int& total, int& texts) {
    total++;
    if (!w.text.empty()) { texts++; if (texts <= 8) DbgLog("  text[%d] '%s' font=%d @(%d,%d)", depth, w.text.c_str(), w.fontPx, w.left, w.top); }
    for (auto& c : w.children) DumpTree(c, depth + 1, total, texts);
}
void LoginState::OnEnter() {
    ui = gui::LoadXui("resources/resources/xui/ui/1login.xui");
    int total = 0, texts = 0; DumpTree(ui, 0, total, texts);
    DbgLog("[login] xui='%s' kids=%zu totalWidgets=%d textWidgets=%d",
           ui.type.c_str(), ui.children.size(), total, texts);
}
void LoginState::OnUpdate() {
    // "Press any key to start" -> server select.
    if (world && next && (input::g.anyKey || input::g.click)) {
        DbgLog("[flow] Login -> ServerSelect (key/click)");
        world->ChangeScene(next);
    }
}
void LoginState::OnRender(IRenderer* r) {
    if (r && !ui.type.empty()) gui::DrawTree(r, ui);
}

// --- small widget helpers (name lookup / hit-test) ---------------------------
static gui::Widget* FindWidget(gui::Widget& w, const char* name) {
    if (w.name == name) return &w;
    for (auto& c : w.children) if (gui::Widget* f = FindWidget(c, name)) return f;
    return nullptr;
}
static bool Inside(const gui::Widget* w, int x, int y) {
    return w && x >= w->left && x < w->right && y >= w->top && y < w->bottom;
}

static const int kRowTop = 208, kRowH = 32;   // server_list_ListBox grid (4server.xui)

void ServerSelectState::OnEnter() {
    ui = gui::LoadXui("resources/resources/xui/ui/4server.xui");
    // The off-screen row template (server_list_item_GroupBox @ top=612) is a
    // clone source, not a real widget - hide it so it stops rendering as a
    // stray bar at the bottom. Flatten() skips a subtree when its show=false.
    if (gui::Widget* tmpl = FindWidget(ui, "server_list_item_GroupBox")) tmpl->show = false;
    DbgLog("[server] xui='%s' kids=%zu", ui.type.c_str(), ui.children.size());

    // The male character = male_bip.scn skeleton (81 bones + anims) with the
    // default "00_male_*" body meshes skinned to it. male_bip.scn itself holds
    // only the rig + a default weapon; the body comes from these part files.
    const char* base = "resources/resources/model/character/";
    character3d = scn::Parse((std::string(base) + "male_bip.scn").c_str());
    DbgLog("[char] male_bip ok=%d models=%zu bones=%zu clips=%zu", (int)character3d.ok,
           character3d.models.size(), character3d.bones.size(), character3d.animNames.size());
    for (size_t i = 0; i < character3d.animNames.size(); ++i)
        DbgLog("[char]   clip[%zu] '%s'", i, character3d.animNames[i].c_str());
    const char* parts[] = {
        "body/00_male_body.scn", "leg/00_male_leg.scn", "hand/00_male_hand.scn",
        "foot/00_male_foot.scn", "face/00_male_face.scn", "hair/00_male_hair.scn",
    };
    for (const char* p : parts) {
        scn::Scene s = scn::Parse((std::string(base) + p).c_str());
        DbgLog("[char] part '%s' ok=%d models=%zu bones=%zu", p, (int)s.ok, s.models.size(), s.bones.size());
        // Dump this part's own bone chunks + whether each links into male_bip.
        for (const scn::Bone& b : s.bones) {
            bool inSkel = false;
            for (const scn::Bone& sb : character3d.bones) if (sb.name == b.parent) { inSkel = true; break; }
            float it[3] = {0,0,0};
            if (!b.anims.empty()) { it[0]=b.anims[0].initT[0]; it[1]=b.anims[0].initT[1]; it[2]=b.anims[0].initT[2]; }
            DbgLog("[char]   partbone '%s' parent='%s' parentInSkel=%d anims=%zu matT=(%.1f,%.1f,%.1f) animInitT=(%.1f,%.1f,%.1f)",
                   b.name.c_str(), b.parent.c_str(), (int)inSkel, b.anims.size(),
                   b.matrix[12], b.matrix[13], b.matrix[14], it[0], it[1], it[2]);
        }
        for (const scn::Model& m : s.models)
            DbgLog("[char]   partmodel '%s' parent='%s' matT=(%.1f,%.1f,%.1f)", m.name.c_str(), m.parent.c_str(), m.matrix[12], m.matrix[13], m.matrix[14]);
        if (s.ok) bodyParts.push_back(std::move(s));
    }
}
void ServerSelectState::OnUpdate() {
    // '+' cycles the character preview animation clip.
    if (input::g.cycleAnim && !character3d.animNames.empty()) {
        animIdx = (animIdx + 1) % (int)character3d.animNames.size();
        animStart = GetTickCount();
        DbgLog("[char] anim -> [%d] '%s'", animIdx, character3d.animNames[animIdx].c_str());
    }
    if ((GetAsyncKeyState(VK_F1) & 1) && world3d && world) { world->ChangeScene(world3d); return; }
    if (!input::g.click) return;
    int mx = input::g.mx, my = input::g.my;
    int nsrv = (int)proud::g_client.servers.size();

    // Dispatch by the clicked button's .xui name - 1:1 with the original client
    // (FUN_01768700: find button by name -> its click handler). Positions come
    // straight from the xui <global> rects, so no hardcoded coordinates.
    std::string btn = gui::HitButton(ui, mx, my);
    if (btn == "Refresh_Button") {
        if (proud::g_client.IsConnected()) proud::g_client.SendAppRmi(5004, nullptr, 0);
        DbgLog("[server] Refresh -> ServerListReq");
        return;
    }
    if (btn == "QuickJoin_Button") {
        if (selected < nsrv) {
            const proud::ServerInfo& s = proud::g_client.servers[selected];
            DbgLog("[server] Join '%s' -> connecting game %s:%u", s.name.c_str(), s.ip.c_str(), s.port);
            // The game server has its OWN Version GUID (beb92241-8333-4117-ab92-9b4af78c688f),
            // different from the auth server -> send it or it replies VersionMismatch.
            static const uint8_t gameVer[16] = {
                0x41,0x22,0xb9,0xbe, 0x33,0x83, 0x17,0x41,
                0xab,0x92,0x9b,0x4a, 0xf7,0x8c,0x68,0x8f };
            proud::g_client.Disconnect();
            for (int k = 0; k < 16; ++k) proud::g_client.version[k] = gameVer[k];
            proud::g_client.isGameConn = true;               // don't send ServerListReq to the game server
            proud::g_client.Connect(s.ip.c_str(), s.port);   // game-server handshake (reuses core)
        }
        return;
    }
    if (!btn.empty()) { DbgLog("[server] button '%s' clicked", btn.c_str()); return; }

    // Not a button -> select a server list row.
    int row = (my - kRowTop) / kRowH;
    if (mx >= 362 && mx < 991 && row >= 0 && row < nsrv) {
        selected = row;
        DbgLog("[server] selected row %d '%s'", row, proud::g_client.servers[row].name.c_str());
    }
}
void ServerSelectState::OnRender(IRenderer* r) {
    if (!r) return;
    if (!ui.type.empty()) gui::DrawTree(r, ui);
    // Server rows from the auth ServerListAck, on the 4server.xui ListBox grid
    // (362,208)-(991,560); name column 362.., users ~770, row height 32.
    int i = 0;
    for (const proud::ServerInfo& s : proud::g_client.servers) {
        int y = kRowTop + i * kRowH;
        if (i == selected) r->DrawRect(362, y, 629, kRowH, 0x3348A0FF);          // highlight
        r->DrawText(372, y + 6, 388, kRowH, s.name.c_str(), 18, 0xFFFFFFFF);     // Server
        std::string users = std::to_string(s.online) + " / " + std::to_string(s.limit);
        r->DrawText(770, y + 6, 190, kRowH, users.c_str(), 18, 0xFFFFFFFF);       // Connected Users
        ++i;
    }
    // male_bip character preview (skinned) in the left panel, via a 3D viewport.
    if (character3d.ok && !bodyParts.empty()) {
        std::string anim = character3d.animNames.empty() ? std::string()
                         : character3d.animNames[animIdx % (int)character3d.animNames.size()];
        uint32_t tick = animStart ? (uint32_t)(GetTickCount() - animStart) : 0u;
        r->SetViewport(10, 185, 315, 420);
        RenderScnCharacter(r, character3d, bodyParts, anim, tick);
        r->SetViewport(0, 0, 0, 0);                         // reset to full backbuffer
        // Clip name overlay so you can see which animation '+' selected.
        if (!anim.empty())
            r->DrawText(12, 590, 315, 24, ("Anim: " + anim + "  (+ to change)").c_str(), 14, 0xFFB0E0FF);
    }
}

// Directional RUN clip (ActorStates RunState): pick by input dir relative to the
// character's facing. ix=right(+)/left(-), iz=forward(+)/back(-).
static const char* RunClipForDir(float ix, float iz) {
    // DATA-EXACT from actorstates_runstates.lua RunState_WeaponUnused (LOWER set).
    // ix: A=left(-)/D=right(+), iz: W=front(+)/S=back(-). NO L/R swap - the render
    // facing offset (BuildYawTranslate) is what must be client-correct, not the clip.
    bool f = iz > 0.5f, b = iz < -0.5f, l = ix < -0.5f, r = ix > 0.5f;
    if (f && l) return "00009";   // left-front
    if (f && r) return "00011";   // right-front
    if (b && l) return "00014";   // left-back
    if (b && r) return "00015";   // right-back
    if (b)      return "00013";   // back
    if (l)      return "00010";   // left
    if (r)      return "00012";   // right
    return "00008";               // front
}

// Directional SIT/crouch-walk clip. DATA-EXACT from actorstates_gameplay.lua
// ActorState_Sit (also etcweapon.lua: 00057..00064, same order as RUN).
static const char* SitClipForDir(float ix, float iz) {
    bool f = iz > 0.5f, b = iz < -0.5f, l = ix < -0.5f, r = ix > 0.5f;
    if (f && l) return "00058";   // left-front
    if (f && r) return "00060";   // right-front
    if (b && l) return "00063";   // left-back
    if (b && r) return "00064";   // right-back
    if (b)      return "00062";   // back
    if (l)      return "00059";   // left
    if (r)      return "00061";   // right
    return "00057";               // front
}

// Locomotion clips whose playback CADENCE scales with move speed (foot-sync), vs
// idle/jump/dodge which play at a fixed 1x. Client: CActor::UpdateAnimOptimize ->
// SetAnimationSpeed(speed/ref). run 00008-15, fastrun 00068, crouch-walk 00057-64.
static bool IsLocoClip(const std::string& c) {
    return (c >= "00008" && c <= "00015") || c == "00068" || (c >= "00057" && c <= "00064");
}

// Authored duration (ms) of a clip, from the skeleton's bone animations (0 if absent).
static uint32_t ClipDurationMs(const scn::Scene& sc, const std::string& name) {
    for (const scn::Bone& b : sc.bones)
        for (const scn::Anim& a : b.anims)
            if (a.name == name && a.hasTransform && a.duration) return a.duration;
    return 0;
}

// Row-major row-vector: rotate about Y by `yaw` then translate to `pos`.
static void BuildYawTranslate(float yaw, const float pos[3], float o[16]) {
    float c = std::cos(yaw), s = std::sin(yaw);
    o[0]=c; o[1]=0; o[2]=-s; o[3]=0;
    o[4]=0; o[5]=1; o[6]=0;  o[7]=0;
    o[8]=s; o[9]=0; o[10]=c; o[11]=0;
    o[12]=pos[0]; o[13]=pos[1]; o[14]=pos[2]; o[15]=1;
}

// --- armas ------------------------------------------------------------------
// Slots como en el cliente: 3, se cambian con 1/2/3. `states` DEBE ser uno de los
// sufijos NormalState_<X> de los .lua; si no matchea, el arma se ve pero se anima
// como desarmado. Los 50 disponibles salen de LuaStates::Weapons("NormalState_").
struct SWeaponSlot { std::string scn, states; };

// Reads a "scn"/"states" pair out of one {...} object slice - a full JSON
// parser is overkill for a 3-entry fixed-shape config file.
static std::string JsonStringField(const std::string& obj, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t k = obj.find(pat);
    if (k == std::string::npos) return "";
    size_t colon = obj.find(':', k + pat.size());
    if (colon == std::string::npos) return "";
    size_t q1 = obj.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    size_t q2 = obj.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return obj.substr(q1 + 1, q2 - q1 - 1);
}

static int JsonIntField(const std::string& obj, const char* key, int def = -1) {
    std::string pat = std::string("\"") + key + "\"";
    size_t k = obj.find(pat); if (k == std::string::npos) return def;
    size_t colon = obj.find(':', k + pat.size()); if (colon == std::string::npos) return def;
    return std::atoi(obj.c_str() + colon + 1);
}

static std::vector<WorldState::WeaponBoneOverride> LoadWeaponBoneOverrides() {
    std::ifstream f("weapon_bone_overrides.json", std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();
    std::vector<WorldState::WeaponBoneOverride> out;
    size_t pos = 0;
    while (true) {
        size_t open = text.find('{', pos); if (open == std::string::npos) break;
        size_t close = text.find('}', open); if (close == std::string::npos) break;
        std::string obj = text.substr(open, close - open + 1);
        std::string weapon = JsonStringField(obj, "weapon");
        std::string bone = JsonStringField(obj, "bone");
        int part = JsonIntField(obj, "part");
        if (!weapon.empty() && !bone.empty() && part >= 0)
            out.push_back({ weapon, part, bone });
        pos = close + 1;
    }
    return out;
}

static void SaveWeaponBoneOverrides(const std::vector<WorldState::WeaponBoneOverride>& values) {
    std::ofstream f("weapon_bone_overrides.json", std::ios::trunc);
    if (!f) return;
    f << "{\n  \"version\": 1,\n  \"overrides\": [\n";
    for (size_t i = 0; i < values.size(); ++i) {
        const auto& v = values[i];
        f << "    {\"weapon\":\"" << v.weapon << "\",\"part\":" << v.part
          << ",\"bone\":\"" << v.bone << "\"}" << (i + 1 < values.size() ? "," : "") << "\n";
    }
    f << "  ]\n}\n";
}

static void UpsertWeaponBoneOverride(std::vector<WorldState::WeaponBoneOverride>& values,
                                     const std::string& weapon, int part, const std::string& bone) {
    for (auto& v : values) {
        if (v.weapon == weapon && v.part == part) { v.bone = bone; return; }
    }
    values.push_back({ weapon, part, bone });
}

// weapon_loadout.json next to the exe picks what's in slots 1/2/3 - edit it to
// try a different weapon without recompiling. Missing/empty/unreadable file
// falls back to the 3 known-working defaults.
static std::vector<SWeaponSlot> LoadWeaponSlots() {
    std::vector<SWeaponSlot> defaults = {
        { "crystal_plasma_sword.scn", "PlasmaSword" },
        { "bat.scn",                  "BatSword"    },
        { "railgun.scn",              "StandardRailGun" },
    };
    std::ifstream f("weapon_loadout.json", std::ios::binary);
    if (!f) return defaults;
    std::ostringstream ss; ss << f.rdbuf();
    std::string text = ss.str();
    std::vector<SWeaponSlot> out;
    size_t pos = 0;
    while (true) {
        size_t open = text.find('{', pos);
        if (open == std::string::npos) break;
        size_t close = text.find('}', open);
        if (close == std::string::npos) break;
        std::string obj = text.substr(open, close - open + 1);
        std::string scn = JsonStringField(obj, "scn");
        std::string states = JsonStringField(obj, "states");
        if (!scn.empty() && !states.empty()) out.push_back({ scn, states });
        pos = close + 1;
    }
    return out.empty() ? defaults : out;
}

static const std::vector<SWeaponSlot> kWeaponSlots = LoadWeaponSlots();
static const int kWeaponSlotCount = (int)kWeaponSlots.size();

// Weapon exports use both `foo_r/foo_l`, `r_foo/l_foo` and numbered forms
// such as `demonic_knuckle_r_01`. Return the canonical pair without guessing
// from the weapon type or from the number of models inside the SCN.
static bool SplitWeaponPair(const std::string& stem, std::string& rightStem, std::string& leftStem) {
    if (stem.size() > 2 && (stem[0] == 'r' || stem[0] == 'R' || stem[0] == 'l' || stem[0] == 'L') && stem[1] == '_') {
        const std::string base = stem.substr(2);
        rightStem = "r_" + base;
        leftStem = "l_" + base;
        return true;
    }
    for (size_t i = 0; i + 1 < stem.size(); ++i) {
        if (stem[i] != '_') continue;
        const char side = (char)std::tolower((unsigned char)stem[i + 1]);
        if ((side != 'r' && side != 'l') || (i + 2 < stem.size() && stem[i + 2] != '_')) continue;
        const std::string before = stem.substr(0, i);
        const std::string after = stem.substr(i + 2);
        rightStem = before + "_r" + after;
        leftStem = before + "_l" + after;
        return true;
    }
    return false;
}

// Inversa de una matriz afin row-major (rotacion+escala uniforme + traslacion).
static void Mat4InvAffine(const float* m, float* o) {
    // transpuesta de la 3x3, dividida por la escala al cuadrado de cada eje
    float s0 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
    float s1 = m[4]*m[4] + m[5]*m[5] + m[6]*m[6];
    float s2 = m[8]*m[8] + m[9]*m[9] + m[10]*m[10];
    s0 = (s0 > 1e-12f) ? 1.f/s0 : 0.f;  s1 = (s1 > 1e-12f) ? 1.f/s1 : 0.f;  s2 = (s2 > 1e-12f) ? 1.f/s2 : 0.f;
    o[0]=m[0]*s0; o[1]=m[4]*s1; o[2]=m[8]*s2;  o[3]=0.f;
    o[4]=m[1]*s0; o[5]=m[5]*s1; o[6]=m[9]*s2;  o[7]=0.f;
    o[8]=m[2]*s0; o[9]=m[6]*s1; o[10]=m[10]*s2;o[11]=0.f;
    o[12] = -(m[12]*o[0] + m[13]*o[4] + m[14]*o[8]);
    o[13] = -(m[12]*o[1] + m[13]*o[5] + m[14]*o[9]);
    o[14] = -(m[12]*o[2] + m[13]*o[6] + m[14]*o[10]);
    o[15] = 1.f;
}

static const int   kMapMenuRows = 18;   // visible rows in the ESC map picker
static const char* kMapInfoDir = "resources/resources/mapinfo/";
static const char* kBgDir      = "resources/resources/model/background/";

// Read one bginfo-*.ini. Sections we need: [MAPINFO] name=, [SKY] object_1=,
// [STATIC] object_N=. Lines starting with ';' are commented-out objects.
static bool ReadBgInfo(const std::string& file, BgInfo& out) {
    std::ifstream f((kMapInfoDir + file).c_str());
    if (!f) return false;
    out.ini = file;
    std::string line, section;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        line = line.substr(b);
        if (line[0] == ';') continue;
        if (line[0] == '[') { section = line.substr(1, line.find(']') - 1); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (section == "MAPINFO" && key == "name") out.name = val;
        else if (section == "SKY"    && key.rfind("object_", 0) == 0 && out.sky.empty()) out.sky = val;
        else if (section == "STATIC"  && key.rfind("object_", 0) == 0 && !val.empty()) out.statics.push_back(val);
        else if (section == "DYNAMIC" && key.rfind("object_", 0) == 0 && !val.empty()) out.dynamics.push_back(val);
        else if (section == "BGM" && key.rfind("bgm_", 0) == 0 && val.size() > 4 &&
                 val.substr(val.size() - 4) == ".ogg") out.bgm.push_back(val);
    }
    if (out.name.empty()) out.name = file;
    return !out.statics.empty() || !out.sky.empty();
}

void WorldState::ScanMaps() {
    maps.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((std::string(kMapInfoDir) + "bginfo-*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { DbgLog("[world] no mapinfo dir"); return; }
    do {
        std::string fn = fd.cFileName;
        if (fn.size() < 4 || fn.substr(fn.size() - 4) != ".ini") continue;   // skip .ini.oct / .ini.oct.txt
        BgInfo bi;
        if (ReadBgInfo(fn, bi)) maps.push_back(std::move(bi));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(maps.begin(), maps.end(), [](const BgInfo& a, const BgInfo& b) { return a.ini < b.ini; });
    DbgLog("[world] maps=%zu", maps.size());
}

// (Re)load a map: visible meshes from [SKY]+[STATIC], collision from its oct_*
// hull, spawn from the spawn-marker BOX chunks. Safe to call at runtime.
void WorldState::LoadMap(int index) {
    if (index < 0 || index >= (int)maps.size()) return;
    const BgInfo& bi = maps[index];
    mapIdx = index;
    PlayMapMusic(0);
    map.clear(); collTris.clear();

    std::vector<std::string> files;
    if (!bi.sky.empty()) files.push_back(bi.sky);
    for (const std::string& s : bi.statics)  files.push_back(s);
    for (const std::string& s : bi.dynamics) files.push_back(s);   // 68 de 100 mapas traen [DYNAMIC]

    float lo[3]={1e9f,1e9f,1e9f}, hi[3]={-1e9f,-1e9f,-1e9f};   // main-geometry bbox (excl. sky)
    std::vector<const scn::Box*> spawns;   // point into `map`, which owns the scenes
    for (const std::string& mm : files) {
        // *_occlusion.scn only feeds the client's culling — never drawn, never collided.
        if (mm.find("occlusion") != std::string::npos) continue;
        scn::Scene s = scn::Parse((std::string(kBgDir) + mm).c_str());
        if (!s.ok) { DbgLog("[world] map '%s' FAIL: %s", mm.c_str(), s.error.c_str()); continue; }
        if (mm != bi.sky)   // the sky is huge; it would swallow the spawn-center bbox
            for (const scn::Model& m : s.models) {
                if (m.name.rfind("oct_", 0) == 0) continue;      // hull, not visible extent
                for (const scn::Vec3& v : m.vertices) {
                    lo[0]=(std::min)(lo[0],v.x); hi[0]=(std::max)(hi[0],v.x);
                    lo[1]=(std::min)(lo[1],v.y); hi[1]=(std::max)(hi[1],v.y);
                    lo[2]=(std::min)(lo[2],v.z); hi[2]=(std::max)(hi[2],v.z);
                }
            }
        map.push_back(std::move(s));
    }
    // EVERY scene stays in `map`: a .scn can hold the hull AND visible geometry, and
    // DrawMapScene already skips the oct_* models. Dropping whole scenes here left
    // maps missing their geometry and with no floor to land on.
    for (const scn::Scene& s : map)
        for (const scn::Box& b : s.boxes)
            if (b.name.find("spawn_pos") != std::string::npos) spawns.push_back(&b);

    // Collision: the client's own oct_* hull; visible geometry only if the map ships none.
    BuildCollTris(map, collTris, true);
    if (collTris.empty()) { BuildCollTris(map, collTris); DbgLog("[world] sin oct_* -> colision contra la geometria visible"); }

    // Spawn on a real marker; no markers -> map center, high up (gravity lands us).
    if (lo[0] > hi[0]) { lo[0]=lo[1]=lo[2]=0.f; hi[0]=hi[1]=hi[2]=0.f; }
    charPos[0] = (lo[0]+hi[0])*0.5f; charPos[2] = (lo[2]+hi[2])*0.5f;
    charPos[1] = hi[1] + 100.f;
    if (!spawns.empty()) {
        // El marcador ya esta ~400u sobre el piso (Y=600 con piso en 199), asi que
        // se usa tal cual: sumarle altura extra solo alarga la caida inicial.
        const scn::Box* b = spawns[GetTickCount() % spawns.size()];
        charPos[0] = b->matrix[12]; charPos[1] = b->matrix[13]; charPos[2] = b->matrix[14];
    }
    grounded = false; velY = 0.f; velX = 0.f; velZ = 0.f; camInit = false;
    DbgLog("[world] map '%s' (%s): scenes=%zu collTris=%zu spawns=%zu charPos=(%.0f,%.0f,%.0f)",
           bi.name.c_str(), bi.ini.c_str(), map.size(), collTris.size(), spawns.size(),
           charPos[0], charPos[1], charPos[2]);
}

void WorldState::PlayMapMusic(int index) {
    if (mapIdx < 0 || mapIdx >= (int)maps.size() || maps[mapIdx].bgm.empty()) { audio::StopMusic(); return; }
    const auto& list = maps[mapIdx].bgm;
    bgmIndex = (index % (int)list.size() + (int)list.size()) % (int)list.size();
    audio::PlayMusic(list[bgmIndex]);
    DbgLog("[bgm] map='%s' track=%d/%zu '%s'", maps[mapIdx].name.c_str(), bgmIndex + 1, list.size(), list[bgmIndex].c_str());
}

// Loads a .seq effect and starts it playing at a world position. Silently
// does nothing if the file can't be found/parsed or has no particle nodes -
// missing effects shouldn't ever crash or block an attack from landing.
void WorldState::SpawnSeq(const std::string& relPath, float x, float y, float z, float yaw) {
    if (!seqEffectsEnabled || relPath.empty()) return;
    std::string fullPath = "resources/" + relPath;
    seq::Sequence s = seq::Parse(fullPath.c_str());
    if (!s.ok) {
        DbgLog("[seq] '%s' FAILED to parse: %s (declared nodeCount=%u, nodes found=%zu, version=%.3f)",
               fullPath.c_str(), s.error.c_str(), s.nodeCount, s.nodes.size(), s.version);
        return;
    }

    ActiveEffect e;
    e.x = x; e.y = y; e.z = z; e.yaw = yaw; e.startTick = GetTickCount(); e.duration = s.duration;
    int particleNodes = 0, modelNodes = 0;
    for (const seq::Node& n : s.nodes) {
        if (n.isParticle) {
            ++particleNodes;
            EffectNode en{ seq::ParticlePlayer(n), n.textures.empty() ? std::string() : n.textures[0].file, n.blendType == 1 };
            e.nodes.push_back(std::move(en));
        } else if (!n.modelFile.empty()) {
            scn::Scene model = scn::Parse(("resources/" + n.modelFile).c_str());
            if (model.ok) { ++modelNodes; e.models.push_back({ std::move(model) }); }
        }
    }
    DbgLog("[seq] '%s' ok, %zu nodes, %d particle nodes, %d model nodes, duration=%ums",
           fullPath.c_str(), s.nodes.size(), particleNodes, modelNodes, s.duration);
    if (!e.nodes.empty() || !e.models.empty()) activeEffects.push_back(std::move(e));
}

// Advances and draws every effect still running, dropping the ones that have
// finished (a little past their own duration, so trailing particles don't cut
// off mid-fade).
void WorldState::DrawEffects(IRenderer* r, unsigned tnow) {
    if (!r || !seqEffectsEnabled) return;
    std::vector<seq::Particle> parts;
    for (size_t i = activeEffects.size(); i-- > 0; ) {
        ActiveEffect& e = activeEffects[i];
        unsigned age = tnow - e.startTick;
        if (e.duration > 0 && age > e.duration + 1000) { activeEffects.erase(activeEffects.begin() + i); continue; }
        for (EffectNode& en : e.nodes) {
            en.player.Update((float)age, parts);
            if (parts.empty()) continue;
            std::vector<float> buf(parts.size() * 8);
            for (size_t k = 0; k < parts.size(); ++k) {
                const seq::Particle& p = parts[k];
                float* o = &buf[k*8];
                o[0] = p.x + e.x; o[1] = p.y + e.y; o[2] = p.z + e.z;
                o[3] = p.r; o[4] = p.g; o[5] = p.b; o[6] = p.a; o[7] = p.size;
            }
            int tex = en.texFile.empty() ? -1 : r->LoadTexture(en.texFile.c_str());
            static unsigned lastLog = 0;
            if (tnow - lastLog > 300) {
                lastLog = tnow;
                DbgLog("[seq] drawing %zu particles age=%ums tex='%s'(handle=%d) additive=%d first=(%.0f,%.0f,%.0f) a=%.2f size=%.2f",
                       parts.size(), age, en.texFile.c_str(), tex, (int)en.additive, buf[0], buf[1], buf[2], buf[6], buf[7]);
            }
            r->DrawParticles(buf.data(), (int)parts.size(), tex, en.additive);
        }
        for (EffectModel& em : e.models) {
            float pos[3] = { e.x, e.y, e.z };
            float world[16]; BuildYawTranslate(e.yaw, pos, world);
            for (const scn::Model& m : em.model.models) {
                if (m.vertices.empty() || m.faces.empty()) continue;
                bool hasUv = m.vertices.size() == m.uv1.size();
                std::vector<float> pu(m.vertices.size() * 5);
                for (size_t i = 0; i < m.vertices.size(); ++i) {
                    pu[i*5+0]=m.vertices[i].x; pu[i*5+1]=m.vertices[i].y; pu[i*5+2]=m.vertices[i].z;
                    pu[i*5+3]= hasUv ? m.uv1[i].u : 0.f; pu[i*5+4]= hasUv ? m.uv1[i].v : 0.f;
                }
                std::vector<unsigned short> idx; idx.reserve(m.faces.size() * 3);
                for (const scn::Face& f : m.faces) { idx.push_back(f.a); idx.push_back(f.b); idx.push_back(f.c); }
                std::string texPath = em.model.dir.empty() ? m.texture : (em.model.dir + "/" + m.texture);
                int tex = r->LoadTexture(texPath.c_str());
                r->DrawMesh(pu.data(), (int)m.vertices.size(), idx.data(), (int)idx.size(), tex, world, m.renderFlag);
            }
        }
    }
}

// Real Weapon_<x>.lua file + StandAttack_Weak/Strong/Strong1 function names for
// a weapon's Lua suffix ("PlasmaSword", "BatSword", ...), found by actually
// reading the script directory instead of guessing a naming convention - some
// weapons (bat) ship BOTH a "Standard<Name>..." and an alt-skin "<Other><Name>..."
// set of functions in the same file, so "Standard" is only a tiebreaker, not
// the rule. Weapons using a different scheme entirely (katana's numbered
// Attack1..7, countersword's JumpDash/AfterDash chain, guns) come back empty -
// there's no Weak/Strong swing to time for those.
struct WeaponFireFuncs { std::string file, weak, strong, strong1, weapon2, jump; };
static WeaponFireFuncs FindWeaponFireFuncs(const std::string& scriptDir, const std::string& weaponStates) {
    WeaponFireFuncs out;
    namespace fs = std::filesystem;
    static const std::regex funcRe(R"(function\s+([A-Za-z0-9_]+)\s*\()");
    auto pick = [](const std::vector<std::string>& cands) -> std::string {
        for (const std::string& s : cands) if (s.rfind("Standard", 0) == 0) return s;
        return cands.empty() ? std::string() : cands.front();
    };
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(scriptDir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        std::string lower = name; for (char& c : lower) c = (char)tolower((unsigned char)c);
        if (lower.rfind("weapon_", 0) != 0 || lower == "weapon_init.lua") continue;
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".lua") != 0) continue;

        std::ifstream f(entry.path(), std::ios::binary);
        if (!f) continue;
        std::ostringstream ss; ss << f.rdbuf();
        std::string text = ss.str();

        std::vector<std::string> weakC, strongC, strong1C, weapon2C, jumpC;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), funcRe); it != std::sregex_iterator(); ++it) {
            std::string fn = (*it)[1].str();
            if (fn.find(weaponStates) == std::string::npos) continue;
            if (fn.find("StandAttack_Strong1") != std::string::npos) strong1C.push_back(fn);
            else if (fn.find("StandAttack_Strong") != std::string::npos) strongC.push_back(fn);
            else if (fn.find("StandAttack_Weak") != std::string::npos) weakC.push_back(fn);
            else if (fn == "Standard" + weaponStates + "Attack2" || fn == weaponStates + "Attack2") weapon2C.push_back(fn);
            else if (fn == "Standard" + weaponStates + "JumpAttack" || fn == weaponStates + "JumpAttack") jumpC.push_back(fn);
        }
        if (weakC.empty() && strongC.empty() && strong1C.empty() && weapon2C.empty() && jumpC.empty()) continue;
        out.file = name; out.weak = pick(weakC); out.strong = pick(strongC); out.strong1 = pick(strong1C);
        out.weapon2 = pick(weapon2C); out.jump = pick(jumpC);
        break;
    }
    return out;
}

// Equipa el arma del slot: carga su .scn, resuelve el dummy raiz y le pasa al
// controller los estados NormalState_/RunState_ de ese tipo de arma.
//
// El attach del cliente es RE-PARENT PURO (CAttachItem_ReparentNodeToBone
// 0x144BFD0 = SetParent + AddChild, cero matematica de matriz). Pero las
// matrices de los MODELOS del .scn son el bind ACUMULADO de la cadena, asi que
// para colocar cada pieza:   world = m.matrix * inv(dummyRaiz) * huesoDeLaMano
// NO seguir la cadena de padres sobre los modelos: los transforma dos veces.
void WorldState::EquipWeapon(int slot) {
    if (slot < 0 || slot >= kWeaponSlotCount) {   // fuera de rango = desarmado
        weaponSlot = -1; weaponOk = false; weapon = scn::Scene(); weaponStates.clear();
        weaponPieceWorlds.clear(); weaponParts.clear(); weaponTypeId.reset();
        animCtrl.LoadFromLua(luaStates, "");
        return;
    }
    weaponSlot = slot;
    EquipWeaponFile(kWeaponSlots[slot].scn.c_str(), kWeaponSlots[slot].states.c_str());
}

void WorldState::EquipWeaponFile(const char* scnName, const char* states) {
    weaponOk = false;
    weapon = scn::Scene();
    weaponParts.clear();
    weaponAdjustBones.clear();
    weaponStates.clear();
    weaponPieceWorlds.clear();
    weaponTypeId.reset();
    std::string requestedFile = scnName ? scnName : "";
    std::string primaryFile = requestedFile;
    std::filesystem::path requestedPath(requestedFile);
    std::string requestedStem = requestedPath.stem().string();
    std::string rightStem, leftStem;
    const bool splitWeapon = SplitWeaponPair(requestedStem, rightStem, leftStem);
    if (splitWeapon) {
        const std::string rightFile = rightStem + ".scn";
        if (std::filesystem::exists(std::filesystem::path("resources/resources/model/weapon") / rightFile))
            primaryFile = rightFile; // sceneIndex 0 is always the right piece.
    }
    weapon = scn::Parse((std::string("resources/resources/model/weapon/") + primaryFile).c_str());
    if (!weapon.ok || weapon.models.empty()) {
        DbgLog("[weapon] '%s' NO carga: %s", scnName, weapon.error.c_str());
        weaponPieceWorlds.clear();
        animCtrl.LoadFromLua(luaStates, "");
        return;
    }
    // Dummy raiz del arma: hueso cuyo padre no es un nodo de la escena (modelo o
    // otro hueso). "BONESYSTEM" es el contenedor del formato .scn, no un dummy de
    // anclaje real, asi que se excluye a si mismo mientras sus hijos SI entran
    // como candidatos - sin esto, BONESYSTEM ganaba como mount en algunas armas
    // y la pieza entera salia mal ubicada.
    auto isNode = [&](const std::string& name) {
        for (const scn::Model& m : weapon.models) if (m.name == name) return true;
        for (const scn::Bone& o : weapon.bones) if (o.name == name) return true;
        return false;
    };
    std::vector<const scn::Bone*> roots;
    for (const scn::Bone& b : weapon.bones)
        if (!isNode(b.parent) && b.name != "BONESYSTEM") roots.push_back(&b);
    for (const scn::Bone& b : weapon.bones)
        if (b.parent == "BONESYSTEM") roots.push_back(&b);

    const scn::Bone* mount = nullptr;
    for (const scn::Bone* b : roots) if (b->name.find("Wea") != std::string::npos || b->name.find("wea") != std::string::npos) { mount = b; break; }
    if (!mount) for (const scn::Bone* b : roots) if (b->name.find("Dummy") != std::string::npos || b->name.find("dummy") != std::string::npos) { mount = b; break; }
    if (!mount && !roots.empty()) mount = roots[0];

    if (mount) scn::WeaponMountInverse(weapon, *mount, weaponInvMount);
    else { float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; std::memcpy(weaponInvMount, id, sizeof(id)); }
    scn::ResolveWeaponPieceWorlds(weapon, weaponPieceWorlds);

    // Punto de anclaje. El cliente lo saca de un id de la tabla de partes del
    // item (vtbl+0x134); sin esa tabla lo deducimos del nombre del dummy raiz.
    const std::string mn = mount ? mount->name : std::string();
    auto has = [&mn](const char* s) { return mn.find(s) != std::string::npos; };
    if (has("Sniper") || has("Gun") || has("gun") || has("Smg") || has("Mg2") || has("Hw") ||
        has("assult") || has("rocket") || has("ShotGun") || has("Revolver") || has("Minegun") ||
        has("AreaRG") || has("sharpshooter") || has("Turret") || has("Chainlightning") || has("Dual"))
        weaponBone = "Gun_Dummy";
    else if (has("Wing")) weaponBone = "Spine_Dummy";
    else                  weaponBone = "R_Hand_Dummy";

    weaponStates = states;
    weaponOk = true;
    int n = animCtrl.LoadFromLua(luaStates, weaponStates.c_str());
    animCtrl.ForceRefresh();   // apply the new weapon's pose immediately, even mid-run/walk
    weaponAnim = anim::ResolveWeaponAnim(luaStates.parsed(), weaponStates, &luaStates.attackMap(), &luaStates.attribMap());
    atkParam = nullptr; weakCombo = 0;   // clear any attack still playing from the previous weapon
    weapon2Active = false; weapon2IsJump = false; weapon2DropApplied = false; weapon2Phase = 0;
    // Real per-piece bone table (weapon_attach.x7) needs the weapon's numeric
    // WEAPONTYPE_* - resolved from its Lua suffix via the same dispatch table
    // used for attacks/combos.
    weaponTypeId = data::TypeFromSuffix("resources", weaponStates, luaStates.attackMap().typeBySuffix);

    // Keep the already-resolved primary piece and, when this is a *_r/*_l
    // weapon, load its companion as sceneIndex 1. The table is per file:
    // CounterSword's second blade belongs to Bip01 L Forearm, not the right hand.
    WeaponRenderPart primary;
    primary.scene = weapon;
    std::memcpy(primary.invMount, weaponInvMount, sizeof(primary.invMount));
    primary.pieceWorlds = weaponPieceWorlds;
    primary.sceneIndex = 0;
    primary.attackBone = weaponTypeId ? data::AttachBone("resources", *weaponTypeId, 0, false) : "";
    if (primary.attackBone.empty()) primary.attackBone = weaponBone;
    primary.idleBone = weaponTypeId ? data::AttachBone("resources", *weaponTypeId, 0, true) : "";
    if (primary.idleBone.empty()) primary.idleBone = primary.attackBone;
    // Counter Sword scene 0 is the small right-hand blade. The original idle
    // entry (Gun_Dummy) describes the whole stored weapon, but applying it to
    // this split SCN moves the blade out of the hand while leaving the left
    // fist assembly behind. Keep this piece on the right hand; scene 1 stays
    // on Bip01 L Forearm.
    if (weaponTypeId && *weaponTypeId == 1 && primary.attackBone == "R_Hand_Dummy")
        primary.idleBone = primary.attackBone;
    weaponParts.push_back(std::move(primary));

    if (splitWeapon) {
        const std::string leftFile = leftStem + ".scn";
        const std::filesystem::path leftPath = std::filesystem::path("resources/resources/model/weapon") / leftFile;
        if (std::filesystem::exists(leftPath) && leftFile != primaryFile) {
            WeaponRenderPart left;
            left.sceneIndex = 1;
            left.scene = scn::Parse(leftPath.string().c_str());
            if (left.scene.ok && !left.scene.models.empty()) {
                auto isNode = [&](const std::string& name) {
                    for (const scn::Model& m : left.scene.models) if (m.name == name) return true;
                    for (const scn::Bone& b : left.scene.bones) if (b.name == name) return true;
                    return false;
                };
                std::vector<const scn::Bone*> roots;
                for (const scn::Bone& b : left.scene.bones)
                    if (!isNode(b.parent) && b.name != "BONESYSTEM") roots.push_back(&b);
                for (const scn::Bone& b : left.scene.bones)
                    if (b.parent == "BONESYSTEM") roots.push_back(&b);
                const scn::Bone* mount = nullptr;
                for (const scn::Bone* b : roots)
                    if (b->name.find("Wea") != std::string::npos || b->name.find("wea") != std::string::npos) { mount = b; break; }
                if (!mount) for (const scn::Bone* b : roots)
                    if (b->name.find("Dummy") != std::string::npos || b->name.find("dummy") != std::string::npos) { mount = b; break; }
                if (!mount && !roots.empty()) mount = roots[0];
                if (mount) scn::WeaponMountInverse(left.scene, *mount, left.invMount);
                scn::ResolveWeaponPieceWorlds(left.scene, left.pieceWorlds);
                left.attackBone = weaponTypeId ? data::AttachBone("resources", *weaponTypeId, 1, false) : "";
                if (left.attackBone.empty()) left.attackBone = "L_Hand_Dummy";
                left.idleBone = weaponTypeId ? data::AttachBone("resources", *weaponTypeId, 1, true) : "";
                if (left.idleBone.empty()) left.idleBone = left.attackBone;
                weaponParts.push_back(std::move(left));
            }
        }
    }

    for (const scn::Bone& b : character.bones) {
        if (b.name.find("Dummy") != std::string::npos ||
            b.name.find("Forearm") != std::string::npos)
            weaponAdjustBones.push_back(b.name);
    }
    for (size_t i = 0; i < weaponParts.size(); ++i) {
        for (const auto& v : weaponBoneOverrides) {
            if (v.weapon == (states ? states : "") && v.part == (int)i) {
                weaponParts[i].idleBoneOverride = v.bone;
                break;
            }
        }
    }

    // Real per-swing move-speed penalty: run the weapon's own Weapon_<name>.lua
    // once against its REAL StandAttack_Weak/Strong/Strong1 function names
    // (FindWeaponFireFuncs reads the script directory, doesn't guess a
    // filename or a "Standard<Name>..." template). Weapons with no Weak/Strong
    // swing at all (guns, katana's numbered combo, countersword's dash chain)
    // come back empty and just keep the 1.0 no-penalty default.
    atkMoveSpeedRateWeak = atkMoveSpeedRateStrong = atkMoveSpeedRateStrong1 = 1.f;
    atkHitStopWeak = atkHitStopStrong = atkHitStopStrong1 = 0.f;
    weapon2Lua = {}; jumpAttackLua = {};
    {
        const std::string scriptDir = "resources/resources/script";
        const std::string shimPath = "third_party/lua_weapon_shim.lua";
        WeaponFireFuncs ff = FindWeaponFireFuncs(scriptDir, weaponStates);
        auto tryFunc = [&](const std::string& fn, float& outRate, float& outHitStop) {
            if (fn.empty()) return;
            luaw::WeaponFireState st;
            luaw::RunWeaponFire(scriptDir.c_str(), shimPath.c_str(), ff.file.c_str(), fn.c_str(), 0, 0, 0, 0, &st);
            outRate = st.moveSpeedRate; outHitStop = st.hitStopA;
        };
        tryFunc(ff.weak, atkMoveSpeedRateWeak, atkHitStopWeak);
        tryFunc(ff.strong, atkMoveSpeedRateStrong, atkHitStopStrong);
        tryFunc(ff.strong1, atkMoveSpeedRateStrong1, atkHitStopStrong1);
        if (!ff.weapon2.empty()) {
            luaw::RunWeaponFire(scriptDir.c_str(), shimPath.c_str(), ff.file.c_str(), ff.weapon2.c_str(), 0, 0, 0, 0, &weapon2Lua);
            DbgLog("[weapon-lua] weapon2 file='%s' fn='%s' loaded=%d moveBoxMultiplier=%.2f initArgs=%zu",
                   ff.file.c_str(), ff.weapon2.c_str(), (int)weapon2Lua.loaded,
                   weapon2Lua.moveBoxMultiplier, weapon2Lua.initArgs.size());
        }
        if (!ff.jump.empty()) {
            luaw::RunWeaponFire(scriptDir.c_str(), shimPath.c_str(), ff.file.c_str(), ff.jump.c_str(), 0, 0, 0, 0, &jumpAttackLua);
            DbgLog("[weapon-lua] jump file='%s' fn='%s' loaded=%d initArgs=%zu dropAfterMs=%.0f drop=(%.0f,%.0f,%.0f)",
                   ff.file.c_str(), ff.jump.c_str(), (int)jumpAttackLua.loaded, jumpAttackLua.initArgs.size(),
                   jumpAttackLua.dropTime, jumpAttackLua.dropX, jumpAttackLua.dropY, jumpAttackLua.dropZ);
        }
        DbgLog("[weapon] fire funcs file='%s' weak='%s'(hitStop=%.0f) strong='%s'(hitStop=%.0f) strong1='%s'(hitStop=%.0f) (empty file = no Weak/Strong swing found for this weapon)",
               ff.file.c_str(), ff.weak.c_str(), atkHitStopWeak, ff.strong.c_str(), atkHitStopStrong, ff.strong1.c_str(), atkHitStopStrong1);
    }

    DbgLog("[weapon] lowerWeak=%zu lowerStrong=%zu lowerStrong1=%zu (0 = this weapon's actorstates have no GetLowerPartStateSet data, or the parser isn't finding it)",
           weaponAnim.lowerWeak.size(), weaponAnim.lowerStrong.size(), weaponAnim.lowerStrong1.size());
    DbgLog("[weapon] '%s' states='%s' mount='%s' -> hueso '%s', %zu modelos, %d estados, atkWeak=%zu atkStrong=%d, weaponType=%d, moveSpeedRate weak=%.2f strong=%.2f",
           scnName, states, mn.c_str(), weaponBone.c_str(), weapon.models.size(), n,
           weaponAnim.atkWeak.size(), (int)(weaponAnim.atkStrong != nullptr), weaponTypeId.value_or(-1),
           atkMoveSpeedRateWeak, atkMoveSpeedRateStrong);
}

// Arma un catalogo: cada NormalState_<X> del lua emparejado con un .scn de
// model/weapon. Mismo criterio que el cliente JS: se parte el sufijo en tokens,
// se descartan los genericos (sword/gun/rifle/...) y se busca un archivo cuyo
// nombre contenga TODOS los tokens; si no, alguno.
void WorldState::BuildWeaponList() {
    weaponList.clear();
    std::vector<std::string> models;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("resources/resources/model/weapon/*.scn", &fd);
    if (h != INVALID_HANDLE_VALUE) { do { models.push_back(fd.cFileName); } while (FindNextFileA(h, &fd)); FindClose(h); }
    if (models.empty()) return;

    // El nombre del estado no es el nombre del skin/modelo. La búsqueda por
    // tokens cargaba la primera variante que coincidía (p.ej. burn_sigmablade
    // para SigmaBlade), dejando correcto el hueso pero incorrecta la pose del
    // arma. Estos son los SCN base que corresponden al estado del actor.
    static const std::vector<std::pair<const char*, const char*>> kBaseModels = {
        { "AIRGUN",              "airgun.scn" },
        { "EARTH_BOMBER",        "earth_bomber.scn" },
        { "HOMING_RIFLE",        "homing_rifle.scn" },
        { "SPARK_RIFLE",         "spark_rifle.scn" },
        { "RESCUE_GUN",          "rescue_gun.scn" },
        { "MG2",                 "gaussrifle_mk2_v01.scn" },
        { "SigmaBlade",          "sigmablade.scn" },
        { "KatanaSword",         "katana_a.scn" },
        { "Katana_A_Sword",      "katana_a.scn" },
        { "DoubleSword",         "116_twinb_weapon_r.scn" },
        { "DoubleSword_Glitch",  "twinblade_glitch_r.scn" },
        { "StandardRailGun",     "railgun.scn" },
        { "MartinStick",         "railgun_stick.scn" },
        { "DeathScythe",         "deathscythe_normal.scn" },
        { "IRONBOOTS",           "ironboots_01.scn" },
        { "RocketLauncher",      "rocket launcher_n.scn" },
        { "VitalShock",          "vital_shock.scn" },
    };
    auto exactModel = [&](const std::string& state) -> std::string {
        for (const auto& p : kBaseModels) {
            if (state == p.first &&
                std::find(models.begin(), models.end(), p.second) != models.end())
                return p.second;
        }
        return {};
    };

    static const char* kGeneric[] = { "sword","gun","rifle","blade","weapon","glitch","unique",
                                      "state","the","a","b","l","r","2","3","4" };
    for (const std::string& suf : luaStates.Weapons("NormalState_")) {
        if (suf == "WeaponUnused" || suf.find("INSTALL") != std::string::npos) continue;
        if (std::string hit = exactModel(suf); !hit.empty()) {
            weaponList.push_back({ suf, std::move(hit) });
            DbgLog("[weapon] catalog exact: states='%s' -> '%s'", suf.c_str(), weaponList.back().scn.c_str());
            continue;
        }
        // sufijo -> tokens en minuscula, cortando tambien en el cambio a mayuscula
        std::vector<std::string> toks; std::string cur;
        for (size_t i = 0; i < suf.size(); ++i) {
            char c = suf[i];
            if (c == '_' || (i && isupper((unsigned char)c) && islower((unsigned char)suf[i-1]))) {
                if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
                if (c == '_') continue;
            }
            cur += (char)tolower((unsigned char)c);
        }
        if (!cur.empty()) toks.push_back(cur);
        std::vector<std::string> keep;
        for (const std::string& t : toks) {
            bool gen = false;
            for (const char* g : kGeneric) if (t == g) { gen = true; break; }
            if (!gen && !t.empty()) keep.push_back(t);
        }
        if (keep.empty()) continue;
        auto contains = [](const std::string& hay, const std::string& needle) {
            std::string lo = hay; for (auto& c : lo) c = (char)tolower((unsigned char)c);
            return lo.find(needle) != std::string::npos;
        };
        std::string hit;
        for (const std::string& f : models) {   // todos los tokens
            bool all = true;
            for (const std::string& t : keep) if (!contains(f, t)) { all = false; break; }
            if (all) { hit = f; break; }
        }
        if (hit.empty())
            for (const std::string& f : models) {   // al menos uno
                for (const std::string& t : keep) if (contains(f, t)) { hit = f; break; }
                if (!hit.empty()) break;
            }
        if (!hit.empty()) weaponList.push_back({ suf, hit });
    }
    std::sort(weaponList.begin(), weaponList.end(),
              [](const WeaponEntry& a, const WeaponEntry& b) { return a.states < b.states; });
    DbgLog("[weapon] catalogo: %zu armas resueltas de %zu estados", weaponList.size(),
           luaStates.Weapons("NormalState_").size());
}

// Equipa una entrada del catalogo (comparte todo con EquipWeapon salvo el origen).
void WorldState::EquipEntry(int index) {
    if (index < 0 || index >= (int)weaponList.size()) return;
    weaponSlot = -1;
    const WeaponEntry& e = weaponList[index];
    EquipWeaponFile(e.scn.c_str(), e.states.c_str());
}

void WorldState::OnEnter() {
    weaponBoneOverrides = LoadWeaponBoneOverrides();
    const char* cbase = "resources/resources/model/character/";
    character = scn::Parse((std::string(cbase) + "male_bip.scn").c_str());
    const char* pnames[] = {
        "body/00_male_body.scn", "leg/00_male_leg.scn", "hand/00_male_hand.scn",
        "foot/00_male_foot.scn", "face/00_male_face.scn", "hair/00_male_hair.scn" };
    for (const char* p : pnames) { scn::Scene s = scn::Parse((std::string(cbase)+p).c_str()); if (s.ok) bodyParts.push_back(std::move(s)); }

    // Estados de animacion desde los .lua del cliente. Lo que el lua no cubra
    // sigue saliendo de las tablas de anim_states.h.
    if (luaStates.Load("resources/resources/script")) {
        int n = animCtrl.LoadFromLua(luaStates, "");   // "" = WeaponUnused
        DbgLog("[anim] lua: %zu funciones, %d estados cubiertos, %zu armas con NormalState_",
               luaStates.size(), n, luaStates.Weapons("NormalState_").size());
    }
    BuildWeaponList();
    EquipWeapon(0);
    ScanMaps();
    int start = 0;
    for (size_t i = 0; i < maps.size(); ++i) if (maps[i].ini == "bginfo-square.ini") { start = (int)i; break; }
    LoadMap(start);
    menuSel = mapIdx;
    lastTick = GetTickCount();
    loaded = character.ok && !bodyParts.empty();
    animStart = GetTickCount();
    // DEBUG: authored duration (ms) of key locomotion clips, to match playback cadence.
    for (const char* cn : { "00008", "00074", "00068", "00057" }) {
        uint32_t dur = 0;
        for (const scn::Bone& b : character.bones) { for (const scn::Anim& a : b.anims)
            if (a.name == cn && a.hasTransform && a.duration) { dur = a.duration; break; } if (dur) break; }
        DbgLog("[anim-dur] clip '%s' duration=%u ms", cn, dur);
    }
}

void WorldState::OnUpdate() {
    if (GetAsyncKeyState(VK_F5) & 1) audio::ToggleMusic();
    if (GetAsyncKeyState(VK_F6) & 1) PlayMapMusic(bgmIndex + 1);
    if (GetAsyncKeyState(VK_F7) & 1) PlayMapMusic(bgmIndex - 1);
    if (audio::ConsumeMusicFinished()) PlayMapMusic(bgmIndex + 1);
    // ESC map picker. While it is open the world is frozen and the cursor comes back:
    // Up/Down (or mouse wheel keys) move, Enter loads, ESC closes.
    if (GetAsyncKeyState(VK_ESCAPE) & 1) {
        mapMenu = !mapMenu;
        if (mapMenu) { menuSel = mapIdx < 0 ? 0 : mapIdx; ClipCursor(nullptr);
                       if (cursorHidden) { while (ShowCursor(TRUE) < 0) {} cursorHidden = false; } }
    }
    if (mapMenu) {
        const int rows = (int)maps.size();
        if (rows) {
            if (GetAsyncKeyState(VK_UP)    & 1) menuSel = (menuSel + rows - 1) % rows;
            if (GetAsyncKeyState(VK_DOWN)  & 1) menuSel = (menuSel + 1) % rows;
            if (GetAsyncKeyState(VK_PRIOR) & 1) menuSel = (std::max)(0, menuSel - 10);
            if (GetAsyncKeyState(VK_NEXT)  & 1) menuSel = (std::min)(rows - 1, menuSel + 10);
            if (menuSel < menuTop) menuTop = menuSel;
            if (menuSel >= menuTop + kMapMenuRows) menuTop = menuSel - kMapMenuRows + 1;
            if (GetAsyncKeyState(VK_RETURN) & 1) { LoadMap(menuSel); mapMenu = false; }
        }
        lastTick = GetTickCount();   // don't accumulate dt while paused
        return;
    }

    // F4 = selector de armas (todas las del lua con modelo, como el panel del JS).
    if (GetAsyncKeyState(VK_F4) & 1) { wpnMenu = !wpnMenu; if (wpnMenu) animMenu = false; }
    if (wpnMenu && !weaponList.empty()) {
        const int rows = (int)weaponList.size();
        if (GetAsyncKeyState(VK_UP)    & 1) wpnSel = (wpnSel + rows - 1) % rows;
        if (GetAsyncKeyState(VK_DOWN)  & 1) wpnSel = (wpnSel + 1) % rows;
        if (GetAsyncKeyState(VK_PRIOR) & 1) wpnSel = (std::max)(0, wpnSel - 10);
        if (GetAsyncKeyState(VK_NEXT)  & 1) wpnSel = (std::min)(rows - 1, wpnSel + 10);
        if (wpnSel < wpnTop) wpnTop = wpnSel;
        if (wpnSel >= wpnTop + kMapMenuRows) wpnTop = wpnSel - kMapMenuRows + 1;
        if (GetAsyncKeyState(VK_RETURN) & 1) { EquipEntry(wpnSel); wpnMenu = false; }
        lastTick = GetTickCount();
        return;                       // el mundo queda quieto mientras elegis
    }

    // F3 = menu de animacion en vivo (el equivalente al menu del cliente JS).
    // Flechas arriba/abajo eligen fila, izquierda/derecha ajustan.
    if (GetAsyncKeyState(VK_F3) & 1) animMenu = !animMenu;
    if (animMenu) {
        const int rows = 4;
        if (GetAsyncKeyState(VK_UP)   & 1) animSel = (animSel + rows - 1) % rows;
        if (GetAsyncKeyState(VK_DOWN) & 1) animSel = (animSel + 1) % rows;
        int step = 0;
        if (GetAsyncKeyState(VK_LEFT)  & 1) step = -1;
        if (GetAsyncKeyState(VK_RIGHT) & 1) step = +1;
        if (step) {
            switch (animSel) {
            case 0: animCtrl.blendFloorMs = (std::max)(0.f,  animCtrl.blendFloorMs + step * 20.f); break;
            case 1: animCtrl.blendCapMs   = (std::max)(0.f,  animCtrl.blendCapMs   + step * 50.f); break;
            case 2: animCtrl.speedScale   = (std::max)(0.1f, animCtrl.speedScale   + step * 0.05f); break;
            case 3: animRefSpeed          = (std::max)(50.f, animRefSpeed          + step * 25.f); break;
            }
        }
    }

    // 1/2/3 = slot de arma, 0 = desarmado (recarga los estados del lua del arma).
    if (GetAsyncKeyState('1') & 1) EquipWeapon(0);
    if (GetAsyncKeyState('2') & 1) EquipWeapon(1);
    if (GetAsyncKeyState('3') & 1) EquipWeapon(2);
    if (GetAsyncKeyState('0') & 1) EquipWeapon(-1);

    if (GetAsyncKeyState(VK_F1) & 1) {
        boneEditMode = !boneEditMode;
        boneEditPopupUntil = GetTickCount() + 2500;
        DbgLog("[weapon-tune] bone edit mode %s", boneEditMode ? "enabled" : "disabled");
    }

    // Bone editing is deliberately gated behind F1 so gameplay presses cannot
    // modify weapon_bone_overrides.json accidentally.
    const bool boneLeftPressed = (GetAsyncKeyState('L') & 1) != 0;
    const bool boneRightPressed = (GetAsyncKeyState('R') & 1) != 0;
    int boneStep = 0;
    if (boneEditMode && boneLeftPressed) boneStep = -1;
    if (boneEditMode && boneRightPressed) boneStep = +1;
    if (boneEditMode && weaponOk && boneStep && !weaponParts.empty() && !weaponAdjustBones.empty()) {
        const bool secondary = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const size_t partIndex = secondary && weaponParts.size() > 1 ? 1u : 0u;
        auto& part = weaponParts[partIndex];
        int i = 0;
        const std::string& current = part.idleBoneOverride.empty() ? part.attackBone : part.idleBoneOverride;
        for (int n = 0; n < (int)weaponAdjustBones.size(); ++n)
            if (weaponAdjustBones[n] == current) { i = n; break; }
        i = (i + boneStep + (int)weaponAdjustBones.size()) % (int)weaponAdjustBones.size();
        part.idleBoneOverride = weaponAdjustBones[i];
        UpsertWeaponBoneOverride(weaponBoneOverrides, weaponStates, (int)partIndex, part.idleBoneOverride);
        DbgLog("[weapon-tune] part=%zu mount bone='%s'", partIndex, part.idleBoneOverride.c_str());
    }
    const bool backPressed = (GetAsyncKeyState(VK_BACK) & 1) != 0;
    const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (boneEditMode && weaponOk && backPressed && ctrlDown) {
        weaponBoneOverrides.clear();
        for (auto& part : weaponParts) part.idleBoneOverride.clear();
        SaveWeaponBoneOverrides(weaponBoneOverrides);
        DbgLog("[weapon-tune] ALL standby overrides restored and saved");
    } else if (boneEditMode && weaponOk && backPressed) {
        for (auto& part : weaponParts) part.idleBoneOverride.clear();
        weaponBoneOverrides.erase(std::remove_if(weaponBoneOverrides.begin(), weaponBoneOverrides.end(),
            [&](const WorldState::WeaponBoneOverride& v) { return v.weapon == weaponStates; }), weaponBoneOverrides.end());
        DbgLog("[weapon-tune] standby bones restored to weapon_attach.x7");
    }
    // F10 belongs to x32dbg/x64dbg (Step Over) and pauses the process when a
    // debugger is attached. Use a chord the debugger does not consume.
    const bool saveChordDown = boneEditMode && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                               (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                               (GetAsyncKeyState('S') & 0x8000);
    static bool saveChordWasDown = false;
    if (saveChordDown && !saveChordWasDown) {
        SaveWeaponBoneOverrides(weaponBoneOverrides);
        DbgLog("[weapon-tune] saved %zu overrides to weapon_bone_overrides.json", weaponBoneOverrides.size());
    }
    saveChordWasDown = saveChordDown;

    // Hide + confine the Windows cursor while our window is focused (the centered
    // crosshair in OnRender replaces it). Restore it when the game loses focus.
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0; if (fg) GetWindowThreadProcessId(fg, &fgPid);
    if (fg && fgPid == GetCurrentProcessId()) {
        RECT rc; GetClientRect(fg, &rc);
        POINT tl = { rc.left, rc.top }, br = { rc.right, rc.bottom };
        ClientToScreen(fg, &tl); ClientToScreen(fg, &br);
        RECT clip = { tl.x, tl.y, br.x, br.y }; ClipCursor(&clip);
        if (!cursorHidden) { while (ShowCursor(FALSE) >= 0) {} cursorHidden = true; }
    } else if (cursorHidden) {
        while (ShowCursor(TRUE) < 0) {} cursorHidden = false; ClipCursor(nullptr);
    }

    // Mouse-look from RAW relative deltas (WM_INPUT), like the client's DirectInput8
    // relative axes — no cursor recenter (recentering spun the camera).
    // Raw Input only arrives when our window has focus, so apply the delta
    // unconditionally (the old focus guard was blocking all mouse-look).
    const float sens = 0.0030f;   // radians per mouse count (tunable)
    if (input::g.mouseDX || input::g.mouseDY) {
        static bool once = false;
        if (!once) { once = true; DbgLog("[world] first mouse delta dx=%ld dy=%ld", input::g.mouseDX, input::g.mouseDY); }
        camYaw   += input::g.mouseDX * sens;
        camPitch += input::g.mouseDY * sens;
    }
    input::g.mouseDX = 0; input::g.mouseDY = 0;   // consume every frame regardless
    // Arrow-key camera fallback + zoom.
    if (GetAsyncKeyState(VK_LEFT)  & 0x8000) camYaw -= 0.05f;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) camYaw += 0.05f;
    if (GetAsyncKeyState(VK_UP)    & 0x8000) camPitch -= 0.03f;
    if (GetAsyncKeyState(VK_DOWN)  & 0x8000) camPitch += 0.03f;
    if (camPitch < -0.35f) camPitch = -0.35f;  if (camPitch > 1.2f) camPitch = 1.2f;
    if (GetAsyncKeyState(VK_PRIOR) & 0x8000) camDist *= 0.95f;
    if (GetAsyncKeyState(VK_NEXT)  & 0x8000) camDist *= 1.05f;
    if (camDist < 120.f) camDist = 120.f;  if (camDist > 6000.f) camDist = 6000.f;

    // Frame-independent dt (seconds) from a HIGH-RES timer (QPC). GetTickCount's
    // ~15ms granularity made dt jump 0<->15ms at high fps = stuttery movement.
    static LARGE_INTEGER qfreq = { 0 }, qlast = { 0 };
    if (qfreq.QuadPart == 0) QueryPerformanceFrequency(&qfreq);
    LARGE_INTEGER qnow; QueryPerformanceCounter(&qnow);
    float dt = qlast.QuadPart ? (float)((double)(qnow.QuadPart - qlast.QuadPart) / qfreq.QuadPart) : 0.016f;
    qlast = qnow;
    if (dt < 0.f) dt = 0.f;  if (dt > 0.033f) dt = 0.033f;   // cap hitches (screenshot etc.) -> brief slow-mo, never a tunneling leap
    lastDt = dt;

    bool crouch = (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState('C') & 0x8000);

    // Desired camera-relative horizontal move (forward = camera-forward, XZ plane).
    float mvX = 0, mvZ = 0;
    if (GetAsyncKeyState('W') & 0x8000) mvZ += 1;
    if (GetAsyncKeyState('S') & 0x8000) mvZ -= 1;
    if (GetAsyncKeyState('A') & 0x8000) mvX -= 1;
    if (GetAsyncKeyState('D') & 0x8000) mvX += 1;
    float inX = mvX, inZ = mvZ;   // raw input dir (for the directional clips)
    bool anyDir = (mvX != 0.f || mvZ != 0.f);

    // Double-tap a direction quickly -> FASTRUN (single hold = walk/RUN).
    unsigned char wasd = 0;
    if (inZ > 0) wasd|=1;  if (inZ < 0) wasd|=2;  if (inX < 0) wasd|=4;  if (inX > 0) wasd|=8;
    unsigned char fresh = wasd & ~prevWASD; prevWASD = wasd;
    unsigned tnow = GetTickCount();
    if (pendingDodgeDir != 0 && tnow - pendingDodgeTime > 1000) pendingDodgeDir = 0;
    if (fresh) {   // W double-tap = FASTRUN; only a clean A/D -> Space sequence dodges.
        const bool doubleTap = fresh == lastTapKey && tnow - lastTapTime < 280;
        if (doubleTap && fresh == 1) running = true;
        if (pendingDodgeDir != 0) pendingDodgeDir = 0;
        else if (fresh == 4) { pendingDodgeDir = -1; pendingDodgeTime = tnow; }
        else if (fresh == 8) { pendingDodgeDir =  1; pendingDodgeTime = tnow; }
        lastTapKey = fresh; lastTapTime = tnow;
    }
    if (wasd == 0) running = false;

    // Hit-stop: a brief near-freeze of the whole sim right on a swing's impact
    // frame (STATE:SetHitStop's own first value, real per-weapon data - see
    // EquipWeaponFile). This is what "weight" actually is in an action game;
    // without it every hit reads as the character just waving the weapon
    // through empty air, no matter how good the swing animation itself looks.
    if (tnow < hitStopUntil) dt *= 0.05f;

    bool spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    bool spacePressed = spaceDown && !spaceWasDown;
    spaceWasDown = spaceDown;
    const bool dodgeCommand = spacePressed && pendingDodgeDir != 0 &&
        tnow - pendingDodgeTime <= 1000;

    // Movement/jump/dodge/gravity/floor-snap physics, ported 1:1 from
    // C:\S4Web2\world.html (moveXZ/hitWall). Wall test = ray vs the precomputed
    // world-space collision triangles (collTris), same pattern (3 side offsets x
    // 3 heights above the climbable-step height) as world.html's THREE.Raycaster
    // version, just against our own triangle list instead of THREE meshes.
    world::MoveState ms;
    ms.x = charPos[0]; ms.y = charPos[1]; ms.z = charPos[2]; ms.yaw = charYaw;
    ms.velY = velY; ms.grounded = grounded; ms.didJump = didJump; ms.jumpMoving = jumpMoving;
    ms.dodgeVX = dodgeVelX; ms.dodgeVZ = dodgeVelZ; ms.dodgeTimeLeft = dodgeTimeLeft; ms.dodgeDir = dodgeDir;
    ms.wallJumpDelayLeft = wallJumpDelayLeft; ms.wallJumpTimeLeft = wallJumpTimeLeft;
    ms.wallJumpVX = wallJumpVelX; ms.wallJumpVZ = wallJumpVelZ;
    ms.weightEase = moveWeightEase;
    ms.velX = moveVelX; ms.velZ = moveVelZ;

    // An attack commits you - holding W through a swing used to keep sliding
    // you toward wherever you were pointed, which isn't how a real attack
    // works. Root in place (no move, no jump) for the swing's real duration or
    // 500ms, whichever is longer (attackLockUntil, set in startAtk).
    // Grounded only - jump attacks don't get this: you're already committed to
    // the jump's arc, and some weapons' jump attacks are themselves a forward
    // dash/lunge (built into the attack, not held WASD) that this would
    // otherwise cancel out.
    bool attackLocked = grounded && ((atkParam != nullptr) ||
        (weapon2Active && weapon2IsJump) || (tnow < attackLockUntil));

    world::MoveInput mi;
    mi.mvX = attackLocked ? 0.f : mvX; mi.mvZ = attackLocked ? 0.f : mvZ;
    mi.running = running && tnow >= fastrunLockoutUntil; mi.crouch = crouch;
    mi.jumpPressed = !attackLocked && spacePressed && !dodgeCommand;
    mi.dodgePressed = !attackLocked && dodgeCommand;
    mi.dodgeLeftHeld  = dodgeCommand && pendingDodgeDir < 0;
    mi.dodgeRightHeld = dodgeCommand && pendingDodgeDir > 0;
    if (spacePressed) pendingDodgeDir = 0;
    mi.camYaw = camYaw;
    // Mid-swing move penalty, read straight from the weapon's own
    // STATE:SetMoveSpeedRate() call (EquipWeaponFile) instead of a guess.
    mi.weaponWeightMul = !atkParam ? 1.f
                        : (atkStrongMode ? (atkParam == weaponAnim.atkStrong1 ? atkMoveSpeedRateStrong1 : atkMoveSpeedRateStrong)
                                         : atkMoveSpeedRateWeak);
    mi.speedMul = moveSpeedMul;

    world::HitTestFn hitWall = [this](float ox, float oy, float oz, float dx, float dz, float maxDist) {
        for (const CollTri& t : collTris) {
            float e1[3] = { t.v1[0]-t.v0[0], t.v1[1]-t.v0[1], t.v1[2]-t.v0[2] };
            float e2[3] = { t.v2[0]-t.v0[0], t.v2[1]-t.v0[1], t.v2[2]-t.v0[2] };
            float lo = (std::min)({t.v0[1],t.v1[1],t.v2[1]}), hi = (std::max)({t.v0[1],t.v1[1],t.v2[1]});
            if (oy < lo - 1.f || oy > hi + 1.f) continue;   // ray is horizontal at height oy
            float pvec[3] = { 0.f*e2[2]-dz*e2[1], dz*e2[0]-dx*e2[2], dx*e2[1]-0.f*e2[0] };
            float det = e1[0]*pvec[0]+e1[1]*pvec[1]+e1[2]*pvec[2];
            if (std::fabs(det) < 1e-8f) continue;
            float invDet = 1.f/det;
            float tv[3] = { ox-t.v0[0], oy-t.v0[1], oz-t.v0[2] };
            float u = (tv[0]*pvec[0]+tv[1]*pvec[1]+tv[2]*pvec[2]) * invDet;
            if (u < 0.f || u > 1.f) continue;
            float qv[3] = { tv[1]*e1[2]-tv[2]*e1[1], tv[2]*e1[0]-tv[0]*e1[2], tv[0]*e1[1]-tv[1]*e1[0] };
            float v = (dx*qv[0]+0.f*qv[1]+dz*qv[2]) * invDet;
            if (v < 0.f || u+v > 1.f) continue;
            float tt = (e2[0]*qv[0]+e2[1]*qv[1]+e2[2]*qv[2]) * invDet;
            if (tt > 1e-4f && tt <= maxDist) return true;
        }
        return false;
    };
    world::FloorFn floorAt = [this](float x, float z, float fromY, float& outY) {
        return FloorYAt(map, x, z, fromY, outY);
    };

    // Keep the stable native path: wall rays + floor ray. The experimental
    // continuous ellipsoid sweep caused lateral drift and prevented ledge falls.
    world::MoveResult mr = moveCtrl.Tick(ms, mi, hitWall, floorAt, dt);

    charPos[0] = ms.x; charPos[1] = ms.y; charPos[2] = ms.z; charYaw = ms.yaw;
    velY = ms.velY; grounded = ms.grounded; didJump = ms.didJump; jumpMoving = ms.jumpMoving;
    dodgeVelX = ms.dodgeVX; dodgeVelZ = ms.dodgeVZ; dodgeTimeLeft = ms.dodgeTimeLeft; dodgeDir = ms.dodgeDir;
    wallJumpDelayLeft = ms.wallJumpDelayLeft; wallJumpTimeLeft = ms.wallJumpTimeLeft;
    wallJumpVelX = ms.wallJumpVX; wallJumpVelZ = ms.wallJumpVZ;
    moveWeightEase = ms.weightEase;
    moveVelX = ms.velX; moveVelZ = ms.velZ;
    bool actuallyMoving = mr.moving;
    weaponPoseInX = inX; weaponPoseInZ = inZ; weaponPoseMoving = actuallyMoving;
    float curSpeed = mr.speed;
    auto durOfClip = [this](const std::string& clip) { return (float)ClipDurationMs(character, clip); };

    // CAttackState (0x00896290) disables gravity during SetDrop's timer, then
    // writes the Lua vector as the actor velocity. Preserve that state timing:
    // Plasma = 400 ms then -50000 Y, Bat = 200 ms then -20000 Y.
    if (weapon2Active && weapon2IsJump && jumpAttackLua.loaded && jumpAttackLua.hasDrop) {
        const float elapsed = float(tnow - weapon2PhaseStart);
        if (elapsed < jumpAttackLua.dropTime) {
            velY = 0.f;
        } else if (!weapon2DropApplied) {
            velY = jumpAttackLua.dropY;
            weapon2DropApplied = true;
            DbgLog("[weapon-action] SetDrop fired at %.0fms velocity=(%.0f,%.0f,%.0f)",
                   elapsed, jumpAttackLua.dropX, jumpAttackLua.dropY, jumpAttackLua.dropZ);
        }
    }

    // DATA-DRIVEN ANIMATION. Gameplay picks the high-level ActorState; the
    // AnimController runs each ACTION state (jump/fall/dodge/bound) as a full phase
    // SEQUENCE and reports actionBusy() until it finishes — so scenes.cpp LATCHES the
    // action state and holds it to completion instead of cutting it with a timer.
    anim::AnimCtx ac;
    ac.inX = inX; ac.inZ = inZ; ac.velY = velY; ac.grounded = grounded;
    ac.motionX = ms.velX; ac.motionZ = ms.velZ; ac.headingYaw = charYaw;
    ac.moving   = actuallyMoving;
    ac.speed    = (ms.dodgeTimeLeft > 0.f) ? moveCtrl.tuning().RUN : curSpeed;  // dodge holds run cadence
    ac.refSpeed = animRefSpeed;
    ac.durOf = durOfClip;
    ac.jumpMoving = jumpMoving;              // 00004 move-rise spans the whole flight
    ac.dodgeDir   = dodgeDir;                // which dodge clip (0 front/1 back/2 left/3 right)
    ac.weakCombo  = weakCombo;               // which SwordAtkWeakState_<X> row (combo chain)

    // Start an action the frame it's triggered (airborne, or a fresh dodge); the
    // controller's phase machine + actionBusy() then own its lifetime.
    bool wantAction = !grounded || dodgeDir >= 0;
    if (mr.startedDodge) {
        audio::PlayEffect("Resources/Sound/dodge.ogg", 0.8f);
        inAction = true;
        actionState = (dodgeDir == 3) ? anim::STATE_DODGE_RIGHT : anim::STATE_DODGE_LEFT;
        DbgLog("[movement] DODGE source=0x008A59A0/0x008A57C0 airborne=%d dir=%d duration=%.0fms velocity=(%.3f,%.3f)",
               (int)!grounded, dodgeDir, moveCtrl.tuning().dodgeTime * 1000.f,
               ms.dodgeVX, ms.dodgeVZ);
    }
    if (mr.startedWallJump) {
        inAction = true;
        actionState = anim::STATE_BOUNDJUMP;
        DbgLog("[movement] STATE_BOUNDJUMP source=0x008A5B80 delay=%.3f force=%.0f moveTime=%.3f dir=(%.3f,%.3f)",
               moveCtrl.tuning().wallJumpDelay, moveCtrl.tuning().wallJumpForce,
               moveCtrl.tuning().wallJumpTime, ms.wallJumpVX, ms.wallJumpVZ);
    }
    if (wantAction && !inAction) {
        inAction = true;
        if (dodgeDir >= 0)  actionState = (dodgeDir == 3) ? anim::STATE_DODGE_RIGHT
                                                          : anim::STATE_DODGE_LEFT;
        else                actionState = didJump ? anim::STATE_JUMP : anim::STATE_FALL;
    }
    anim::ActorState wantedState;
    if (inAction) {
        wantedState = actionState;
    } else if (crouch)                                  wantedState = anim::STATE_SIT;
    else if (running && actuallyMoving && inZ > 0.3f)    wantedState = anim::STATE_FASTRUN;
    else if (actuallyMoving)                             wantedState = anim::STATE_RUN;
    else                                                  wantedState = anim::STATE_NORMAL;

    actorStateMachine.Request(wantedState);
    const anim::ActorState st = actorStateMachine.current();

    if ((int)st != lastAudioState) {
        lastAudioState = (int)st;
        if (st == anim::STATE_FASTRUN) {
            auto it = luaStates.parsed().find("ActorState_FastRun");
            if (it != luaStates.parsed().end())
                for (const anim::ActorSound& sound : it->second.sounds)
                    audio::PlayEffect(sound.man, 0.75f, (unsigned)(std::max)(0, sound.start));
        }
    }

    curPose = animCtrl.Update(st, ac, dt * 1000.f);
    actorStateMachine.EndFrame();
    curLowerPoseActive = false;   // set below only while an attack with its own leg data is active

    // Weapon swings run independently of the locomotion/jump/fall/dodge state
    // above and just overwrite curPose when one is active - you can throw a hit
    // mid-air just as well as on the ground.
    auto startAtk = [&](const anim::ActorAnim* param, bool strong) {
        if (!param || tnow < atkCooldownUntil || atkParam || weapon2Active) return;
        atkParam = param; atkStrongMode = strong; atkHitDone = false; atkStart = tnow;
        atkBlendFrom = curPose;   // whatever was playing this frame - blended into the swing below
        float rawClipDur = durOfClip(param->clip);
        float dur = float(param->startMs) + rawClipDur;
        if (dur <= 0.f) dur = 500.f;
        float speedScale = (std::max)(animCtrl.speedScale, 0.1f);
        atkCooldownUntil = tnow + (unsigned)(dur / speedScale) + 200;
        attackLockUntil = tnow + (unsigned)(std::max)(500.f, dur / speedScale);
        DbgLog("[atk] strong=%d clip='%s' startMs=%d rawClipDurTicks=%.0f dur=%.0f speedScale=%.2f -> realMsToFinish=%.0f (0 rawClipDurTicks = clip not found on character skeleton)",
               (int)strong, param->clip.c_str(), param->startMs, rawClipDur, dur, speedScale, dur / speedScale);

        const bool jumpWeak = param == weaponAnim.jumpAtkWeak;
        const anim::ActorSeq* fx = jumpWeak ? weaponAnim.seqJumpAtkWeak : (strong ? weaponAnim.seqStrong : weaponAnim.seqWeak);
        const anim::ActorSound* snd = jumpWeak ? weaponAnim.sndJumpAtkWeak : (strong ? weaponAnim.sndStrong : weaponAnim.sndWeak);
        if (snd) audio::PlayEffect(snd->man, 0.85f, (unsigned)(std::max)(0, snd->start));
        if (fx && !fx->sound.empty()) audio::PlayEffect(fx->sound, 0.85f, (unsigned)(std::max)(0, fx->startMs));
        if (weaponAnim.isGun && !weaponAnim.gunBlastSound.empty()) audio::PlayEffect(weaponAnim.gunBlastSound, 0.9f);
        if (fx) {
            float fx_ = std::sin(charYaw), fz_ = std::cos(charYaw);
            SpawnSeq(fx->seq, charPos[0] + fx_*90.f, charPos[1] + 130.f, charPos[2] + fz_*90.f, charYaw + 3.14159265f);
        }
    };

    auto startWeapon2 = [&](bool aerial) {
        const auto& phases = aerial ? weaponAnim.jumpAtk : weaponAnim.atkWeapon2;
        if (!weaponOk || phases.empty() || atkParam || weapon2Active || tnow < atkCooldownUntil) return;
        weapon2Active = true; weapon2IsJump = aerial;
        weapon2DropApplied = false;
        weapon2MoveBoxValid = false; weapon2MoveBoxClip.clear();
        atkBlendFrom = curPose;
        weapon2Phase = 0; weapon2PhaseStart = tnow; rightHeldAt = tnow;
        const auto* fx = aerial ? weaponAnim.seqJumpAtk : weaponAnim.seqWeapon2;
        const auto* snd = aerial ? weaponAnim.sndJumpAtk : weaponAnim.sndWeapon2;
        if (snd) audio::PlayEffect(snd->man, 0.85f, (unsigned)(std::max)(0, snd->start));
        if (fx && !fx->sound.empty()) audio::PlayEffect(fx->sound, 0.85f, (unsigned)(std::max)(0, fx->startMs));
        if (fx) {
            float fx_ = std::sin(charYaw), fz_ = std::cos(charYaw);
            SpawnSeq(fx->seq, charPos[0] + fx_*90.f, charPos[1] + 130.f, charPos[2] + fz_*90.f, charYaw + 3.14159265f);
        }
        DbgLog("[weapon-action] enter aerial=%d phase=0 clip='%s' rate=%.2f loop=%d",
               (int)aerial, phases[0].clip.c_str(), phases[0].rate, (int)phases[0].loop);
    };

    const unsigned kHoldMs = 220;   // tap vs hold threshold for the left mouse button
    bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool rmbDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    bool wantWeak = false, wantStrong = false;
    const bool repeatWeak = weaponStates.find("SpyDagger") != std::string::npos;
    if (lmbDown && !lmbWasDown) {
        mouseHeld = true; mouseDownAt = tnow; attackFired = false;
        if (repeatWeak) { wantWeak = true; attackFired = true; }
    }
    // Checked BEFORE the release edge clears mouseHeld below, so a release that
    // lands the same frame the hold threshold is crossed still commits to the
    // strong swing instead of firing nothing (was possible to hold past 220ms
    // and let go right on the frame it would've fired, and get no attack at all).
    if (!repeatWeak && mouseHeld && !attackFired && tnow - mouseDownAt > kHoldMs) wantStrong = true;
    if (!lmbDown && lmbWasDown) { if (!repeatWeak && mouseHeld && !attackFired && tnow - mouseDownAt < kHoldMs) wantWeak = true; mouseHeld = false; }
    if (repeatWeak && lmbDown && !atkParam && !weapon2Active && tnow >= atkCooldownUntil) wantWeak = true;
    lmbWasDown = lmbDown;
    if (rmbDown && !rmbWasDown) {
        rightHeld = true;
        startWeapon2(!grounded && !weaponAnim.jumpAtk.empty());
    }
    if (!rmbDown && rmbWasDown) { rightHeld = false; lastWeapon2At = tnow; }
    rmbWasDown = rmbDown;

    if (weaponOk) {
        // Holding past the threshold fires the strong swing while the button is
        // still down - standing still uses the stand variant when the weapon has
        // one, and being airborne swaps in the jump attack.
        const anim::ActorAnim* strongPick = (!actuallyMoving && weaponAnim.atkStrong1) ? weaponAnim.atkStrong1 : weaponAnim.atkStrong;
        if (wantStrong && !atkParam) {
            if (!grounded && !weaponAnim.jumpAtk.empty()) startWeapon2(true);
            else startAtk(strongPick, true);
            attackFired = true;
        }
        if (wantWeak && !atkParam) {
            // A weak hit thrown right after letting go of guard (right click) is a
            // counter instead of the next combo step.
            if (weaponAnim.atkCounter && grounded && tnow - lastWeapon2At < 800) {
                startAtk(weaponAnim.atkCounter, false);
            } else if (grounded) {
                weakCombo = (tnow - lastWeakAt < 600 && !weaponAnim.atkWeak.empty()) ? (weakCombo + 1) % (int)weaponAnim.atkWeak.size() : 0;
                lastWeakAt = tnow;
                startAtk(weaponAnim.atkWeak.empty() ? nullptr : &weaponAnim.atkWeak[weakCombo], false);
            } else {
                const anim::ActorAnim* p = weaponAnim.jumpAtkWeak ? weaponAnim.jumpAtkWeak
                                          : (weaponAnim.atkWeak.empty() ? nullptr : &weaponAnim.atkWeak[0]);
                startAtk(p, false);
            }
        }
        if (atkParam) {
            float startDelay = float(atkParam->startMs);
            float dur = startDelay + durOfClip(atkParam->clip);
            if (dur <= 0.f) dur = 500.f;
            float t = float(tnow - atkStart) * animCtrl.speedScale * atkParam->rate;
            if (!atkHitDone && t >= startDelay) {
                atkHitDone = true;   // TODO: hit-test against peers once networking lands
                float hs = atkStrongMode ? (atkParam == weaponAnim.atkStrong1 ? atkHitStopStrong1 : atkHitStopStrong) : atkHitStopWeak;
                if (hs > 0.f) hitStopUntil = tnow + (unsigned)hs;
            }
            if (t >= dur) { atkParam = nullptr; fastrunLockoutUntil = tnow + 120; }
            else {
                unsigned sinceStartMs = tnow - atkStart;
                curPose.clipA = atkParam->clip; curPose.tickA = (uint32_t)(std::max)(0.f, t - startDelay);
                curPose.loopA = atkParam->loop;
                // Ease in from whatever pose was playing right before the swing
                // started instead of hard-cutting to frame 0 of the attack clip -
                // the cut is what made every swing feel like a snap/glitch.
                if (sinceStartMs < (unsigned)atkBlendMs && !atkBlendFrom.clipA.empty() && atkBlendFrom.clipA != curPose.clipA) {
                    curPose.clipB = curPose.clipA; curPose.tickB = curPose.tickA;
                    curPose.loopB = curPose.loopA;
                    curPose.clipA = atkBlendFrom.clipA; curPose.tickA = atkBlendFrom.tickA;
                    curPose.loopA = atkBlendFrom.loopA;
                    curPose.blend = (std::min)(1.f, float(sinceStartMs) / atkBlendMs);
                } else {
                    curPose.clipB.clear(); curPose.blend = 0.f;
                }
                // Legs: the real client keeps them on their own walk/stand clip
                // (state:GetLowerPartStateSet) instead of freezing into the
                // swing's whole-body pose - look up this attack's own leg data
                // for the actual movement direction and drive it independently.
                if (grounded) {
                    const auto& lowerMap = atkStrongMode
                        ? (atkParam == weaponAnim.atkStrong1 ? weaponAnim.lowerStrong1 : weaponAnim.lowerStrong)
                        : weaponAnim.lowerWeak;
                    std::string dir = actuallyMoving ? anim::RunDir(inX, inZ) : "NORMAL";
                    auto lit = lowerMap.find(dir);
                    if (lit == lowerMap.end() && dir != "NORMAL") lit = lowerMap.find("NORMAL");
                    if (lit != lowerMap.end()) {
                        float ldur = durOfClip(lit->second.clip);
                        float lticks = float(tnow - atkStart) * animCtrl.speedScale;
                        curLowerPose.clipA = lit->second.clip;
                        curLowerPose.tickA = (uint32_t)(ldur > 0.f ? std::fmod(lticks, ldur) : lticks);
                        curLowerPose.clipB.clear(); curLowerPose.blend = 0.f;
                        curLowerPose.loopA = lit->second.loop;
                        curLowerPoseActive = true;
                        static std::string lastLoggedLowerClip;
                        if (lastLoggedLowerClip != lit->second.clip) {
                            lastLoggedLowerClip = lit->second.clip;
                            DbgLog("[lowerpose] dir='%s' clip='%s' durTicks=%.0f", dir.c_str(), lit->second.clip.c_str(), ldur);
                        }
                    } else {
                        static unsigned lastMissLog = 0;
                        if (tnow - lastMissLog > 1000) {
                            lastMissLog = tnow;
                            DbgLog("[lowerpose] MISS dir='%s' lowerMap.size=%zu (no entry for this direction or NORMAL)", dir.c_str(), lowerMap.size());
                        }
                    }
                }
            }
        } else if (weapon2Active) {
            // Wind-up (phase 0) always plays out in full even if RMB is let go
            // early - matches UseWeapon2State_PlasmaSword's real B0000 -> B0001
            // phase order. Only the held/guard phase at the end depends on the
            // button still being down.
            const auto& phases = weapon2IsJump ? weaponAnim.jumpAtk : weaponAnim.atkWeapon2;
            if (weapon2Phase >= (int)phases.size()) weapon2Phase = (int)phases.size() - 1;
            const anim::ActorAnim* p = &phases[weapon2Phase];
            float realDur = durOfClip(p->clip);
            float dur = realDur; if (dur <= 0.f) dur = 500.f;
            float since = float(tnow - weapon2PhaseStart);
            // InitLua(normalMs, delayMs, ...) owns attack-state timing. Native
            // ApplyAnim 0x008AEAE0 fits each clip to that state window:
            // playbackRate = clipDuration * SetAnim.rate / stateWindowMs.
            const luaw::WeaponFireState& actionLua = weapon2IsJump ? jumpAttackLua : weapon2Lua;
            float phaseMs = 0.f;
            if (actionLua.loaded && weapon2Phase < (int)actionLua.initArgs.size())
                phaseMs = actionLua.initArgs[weapon2Phase];
            float ticks = phaseMs > 0.f ? since * dur * p->rate / phaseMs
                                        : since * animCtrl.speedScale * p->rate;
            bool lastPhase = (weapon2Phase == (int)phases.size() - 1);
            // Aerial NORMAL keeps its last key until contact; then DELAY plays
            // the authored recovery. Releasing RMB is not a cancellation event.
            const bool phaseDone = phaseMs > 0.f ? since >= phaseMs : ticks >= dur;
            if (!lastPhase && !p->loop && phaseDone) {
                weapon2Phase++;
                weapon2PhaseStart = tnow;
                p = &phases[weapon2Phase];
                realDur = durOfClip(p->clip);
                dur = realDur; if (dur <= 0.f) dur = 500.f;
                phaseMs = (actionLua.loaded && weapon2Phase < (int)actionLua.initArgs.size())
                    ? actionLua.initArgs[weapon2Phase] : 0.f;
                ticks = 0.f;
                lastPhase = (weapon2Phase == (int)phases.size() - 1);
                DbgLog("[weapon-action] advance aerial=%d phase=%d clip='%s' grounded=%d rate=%.2f",
                       (int)weapon2IsJump, weapon2Phase, p->clip.c_str(), (int)grounded, p->rate);
            }
            const bool lastDone = phaseMs > 0.f ? since >= phaseMs : ticks >= dur;
            if (lastPhase && !p->loop && lastDone) {
                // Plasma B0001 is LOOP_FALSE in ActorStates_PlasmaSword.lua:
                // releasing RMB must not cut the committed stun animation.
                weapon2Active = false;
                DbgLog("[weapon-action] finish aerial=%d clip='%s'", (int)weapon2IsJump, p->clip.c_str());
                fastrunLockoutUntil = tnow + 120;
            } else if (lastPhase && p->loop && !rightHeld && !weapon2IsJump) {
                // A LOOP_TRUE final phase is the held/guard pose; it may end
                // immediately when the button is released.
                weapon2Active = false;
                fastrunLockoutUntil = tnow + 120;
            } else {
                // Respect the clip's OWN loop flag here - forcing the held phase to
                // always loop replayed one-shot guard poses (loop=false, meant to
                // freeze on their last frame) from the start every ~300ms instead.
                float tk = p->loop ? std::fmod(ticks, dur) : (std::min)(ticks, dur);
                curPose.clipA = p->clip; curPose.tickA = (uint32_t)tk;
                curPose.loopA = p->loop;
                unsigned sinceRmbMs = tnow - rightHeldAt;
                if (weapon2Phase == 0 && sinceRmbMs < (unsigned)atkBlendMs && !atkBlendFrom.clipA.empty() && atkBlendFrom.clipA != curPose.clipA) {
                    curPose.clipB = curPose.clipA; curPose.tickB = curPose.tickA;
                    curPose.loopB = curPose.loopA;
                    curPose.clipA = atkBlendFrom.clipA; curPose.tickA = atkBlendFrom.tickA;
                    curPose.loopA = atkBlendFrom.loopA;
                    curPose.blend = (std::min)(1.f, float(sinceRmbMs) / atkBlendMs);
                } else {
                    curPose.clipB.clear(); curPose.blend = 0.f;
                }
                static std::string lastLoggedClip;
                if (lastLoggedClip != p->clip) {
                    lastLoggedClip = p->clip;
                    DbgLog("[weapon2] phase=%d clip='%s' loop=%d skeletonDurationMs=%.0f (0 = the skeleton has no clip by this name)",
                           weapon2Phase, p->clip.c_str(), (int)p->loop, realDur);
                }
            }
        }
    }

    // Native CAttackMovingState::Update (S1 0x0052E8C0) consumes the animated
    // MoveBox delta, rotates it by actor heading and scales it by the weapon Lua
    // multiplier. Plasma Sword Attack2 authors multiplier 2.0; it is not a dodge.
    if (weapon2Active && !weapon2IsJump && weapon2Lua.loaded && weapon2Lua.moveBoxMultiplier != 0.f) {
        const std::string clip = curPose.blend > 0.f ? curPose.clipB : curPose.clipA;
        bool authored = false;
        for (const scn::Bone& b : character.bones) if (b.name == "MoveBox") {
            for (const scn::Anim& a : b.anims) if (a.name == clip && a.hasTransform) { authored = true; break; }
            break;
        }
        scn::Pose rootPose;
        rootPose.clipA = clip;
        rootPose.tickA = curPose.blend > 0.f ? curPose.tickB : curPose.tickA;
        rootPose.loopA = curPose.blend > 0.f ? curPose.loopB : curPose.loopA;
        scn::Mat4 box;
        if (authored && scn::BoneWorldByName(character, "MoveBox", rootPose, box)) {
            if (weapon2MoveBoxValid && weapon2MoveBoxClip == clip) {
                const float lx = (box[12] - weapon2MoveBoxX) * weapon2Lua.moveBoxMultiplier;
                const float lz = (box[14] - weapon2MoveBoxZ) * weapon2Lua.moveBoxMultiplier;
                const float a = charYaw + 3.14159265f, c = std::cos(a), s = std::sin(a);
                moveCtrl.MoveXZ(hitWall, ms, lx*c + lz*s, -lx*s + lz*c);
                charPos[0] = ms.x; charPos[2] = ms.z;
            }
            weapon2MoveBoxX = box[12]; weapon2MoveBoxZ = box[14];
            weapon2MoveBoxClip = clip; weapon2MoveBoxValid = true;
        } else {
            weapon2MoveBoxValid = false; weapon2MoveBoxClip = clip;
        }
    } else {
        weapon2MoveBoxValid = false; weapon2MoveBoxClip.clear();
    }
    // actionBusy() checked AFTER Update() ran for THIS frame's state - before this fix it
    // read last frame's stale value (always false coming from locomotion), so inAction got
    // cleared the SAME frame an action started, before the controller ever saw the new
    // state. jump/fall/dodge got one invisible frame of pose then snapped back to
    // locomotion, even though the physics (position) played out the full action normally.
    if (inAction && grounded && !animCtrl.actionBusy()) {
        inAction = false; dodgeDir = -1;
        // The real client always bounces through STATE_WAIT (empty/no pose of
        // its own) before landing on whatever comes next - its actual job is a
        // brief mandatory settle, not a visual one. Modeled here as a short
        // lockout on re-engaging fastrun right out of an action, so
        // jump/dodge/attack always resolve through a normal-speed beat first
        // instead of an action clip snapping straight into a full sprint.
        fastrunLockoutUntil = tnow + 120;
    }
    // Unique weapon states sometimes use AS000_1/E0018_1 variants absent from
    // the shared skeleton. An invalid clip falls back to bind matrices (T-pose),
    // so use the matching base clip when it exists.
    const char* standbyFallback = (st == anim::STATE_NORMAL || st == anim::STATE_IDLE) ? "00074" : nullptr;
    NormalizeCharacterClip(character, curPose.clipA, standbyFallback);
    NormalizeCharacterClip(character, curPose.clipB, standbyFallback);
    { static unsigned lc = 0;   // DEBUG: state/clip/speed each 0.4s
      if (tnow - lc > 400) { lc = tnow;
        const std::string& cc = curPose.blend > 0.f ? curPose.clipB : curPose.clipA;
        DbgLog("[anim] st=%d clip=%s spd=%.0f ref=%.0f mov=%d grnd=%d tick=%u loop=%d aerial=%d",
               (int)st, cc.c_str(), ac.speed, ac.refSpeed, (int)actuallyMoving, (int)grounded,
               curPose.blend > 0.f ? curPose.tickB : curPose.tickA,
               (int)(curPose.blend > 0.f ? curPose.loopB : curPose.loopA),
               (int)(weapon2Active && weapon2IsJump)); } }
}

void WorldState::OnRender(IRenderer* r) {
    if (!r) return;
    // S4 third-person camera (FUN_01202ad0): the eye is rigidly actor-relative
    // (no position smoothing/lag — direct mouse-look). Only FOV lerps. Look-at =
    // actorPos + heightoffset; eye = lookAt - forward(yaw,pitch)*depth + right*side.
    float cp = std::cos(camPitch);
    float fwd[3]   = { std::sin(camYaw)*cp, -std::sin(camPitch), std::cos(camYaw)*cp };
    float right[3] = { std::cos(camYaw), 0.f, -std::sin(camYaw) };
    const float heightOff = 170.f, sideOff = 45.f;   // client 350/100; tuned for framing
    float at[3]  = { charPos[0], charPos[1] + heightOff, charPos[2] };
    float eye[3] = { at[0] - fwd[0]*camDist + right[0]*sideOff,
                     at[1] - fwd[1]*camDist,
                     at[2] - fwd[2]*camDist + right[2]*sideOff };
    // FOV lerp toward runfov(72) while running, norfov(60) otherwise. alpha = dt_ms*0.01.
    bool fastRun = (animCtrl.state() == anim::STATE_FASTRUN);   // FASTRUN widens FOV (ActorCameraFastRunFov)
    float fovTarget = fastRun ? 72.f : 60.f;
    float aFov = lastDt * 1000.f * 0.01f * 1.75f; if (aFov > 1.f) aFov = 1.f;
    curFov += (fovTarget - curFov) * aFov;
    r->SetCamera3D(eye, at, curFov, 5.f, 300000.f);

    for (const scn::Scene& s : map) DrawMapScene(r, s);

    if (loaded) {
        // curPose is produced each frame by the AnimController (gates + timers + blend).
        // The model's authored forward axis is -Z; the native client applies
        // this fixed correction when placing the assembled character.
        float place[16]; BuildYawTranslate(charYaw + 3.14159265f, charPos, place);
        std::vector<CharPiece> pieces;
        GatherCharacterPieces(character, bodyParts, curPose, pieces, curLowerPoseActive ? &curLowerPose : nullptr);
        DrawCharPieces(r, pieces, place);

        // Arma: cuelga del dummy del bip y sigue la animacion del personaje.
        // world = m.matrix * inv(dummyRaizDelArma) * huesoDelBip * place
        //
        // The official bone table (weapon_attach.x7) is keyed by which FILE this
        // weapon piece came from (0 = right/single .scn, 1 = the left companion),
        // not by model index within one file. Prefix (r_/l_) and suffix (_r/_l)
        // pairs are normalized to the same sceneIndex order.
        for (const WeaponRenderPart& part : weaponParts) {
        // This renderer only draws the currently equipped weapon. idleAttach is
        // the holstered slot (Plasma Sword -> Spine_Dummy), not its locomotion
        // mount; RUN A0005..A0012 animates attackAttach (R_Hand_Dummy).
        const std::string& selectedBone = part.idleBoneOverride.empty() ? part.attackBone : part.idleBoneOverride;
        std::string attachBoneName = selectedBone.empty() ? weaponBone : selectedBone;
        float hand[16];
        if (weaponOk && scn::BoneWorldByName(character, attachBoneName, curPose, hand)) {
            float toHand[16];
            Mat4MulS(part.invMount, hand, toHand);
            // Character and weapon SCNs use separate clip namespaces. Demonic
            // Knuckle, for example, uses KN000/KN001..KN008 while the body uses
            // 00074/00008..00015. Select the weapon's authored state first.
            scn::AnimPose weaponPose = MakeWeaponPose(part.scene, weaponAnim, curPose,
                animCtrl.state(), weaponPoseMoving, weaponPoseInX, weaponPoseInZ,
                atkParam, weapon2Active);
            const bool animateWeapon = !weaponPose.clipA.empty();
            const scn::NodeResolver weaponNodes = animateWeapon
                ? scn::MakeNodeResolver(part.scene, weaponPose, true)
                : scn::NodeResolver{};
            for (size_t mi = 0; mi < part.scene.models.size(); ++mi) {
                const scn::Model& m = part.scene.models[mi];
                if (m.faces.empty()) continue;
                const float* toHandForPiece = toHand;
                // Skinned weapon sub-part (e.g. the counter sword's fist): deform
                // against the WEAPON's OWN bones first, in the weapon's own bind
                // pose (its attack-clip playback isn't driven here, same as idle
                // weapon parts elsewhere), then attach exactly like a rigid piece -
                // skipping this step left the fist floating loose off the hand.
                std::vector<scn::Vec3> skinned;
                const scn::Vec3* verts = &m.vertices[0];
                size_t nv = m.vertices.size();
                float world[16];
                if (!m.skin.empty() && !part.scene.bones.empty()) {
                    scn::SkinModel(m, part.scene, weaponPose, skinned);
                    if (skinned.empty()) continue;
                    verts = skinned.data(); nv = skinned.size();
                    Mat4MulS(toHandForPiece, place, world);   // NOT in-place: aliasing A==C corrupts the row-by-row multiply
                } else {
                    if (m.vertices.empty()) continue;
                    float toMount[16];
                    const float* pieceMat = (mi < part.pieceWorlds.size()) ? part.pieceWorlds[mi].data() : m.matrix;
                    if (animateWeapon) {
                        if (const scn::Mat4* animated = weaponNodes(m.name)) pieceMat = animated->data();
                    }
                    Mat4MulS(pieceMat, toHandForPiece, toMount);
                    Mat4MulS(toMount, place, world);
                }
                std::string texPath = part.scene.dir.empty() ? m.texture : (part.scene.dir + "/" + m.texture);
                int tex = r->LoadTexture(texPath.c_str());
                const bool hasUv = nv == m.uv1.size();
                std::vector<float> pu(nv * 5);
                for (size_t i = 0; i < nv; ++i) {
                    pu[i*5+0]=verts[i].x; pu[i*5+1]=verts[i].y; pu[i*5+2]=verts[i].z;
                    pu[i*5+3]= hasUv ? m.uv1[i].u : 0.f;  pu[i*5+4]= hasUv ? m.uv1[i].v : 0.f;
                }
                std::vector<unsigned short> idx; idx.reserve(m.faces.size() * 3);
                for (const scn::Face& f : m.faces) { idx.push_back(f.a); idx.push_back(f.b); idx.push_back(f.c); }
                r->DrawMesh(pu.data(), (int)nv, idx.data(), (int)idx.size(), tex, world, m.renderFlag);
            }
        }
        }
    }
    DrawEffects(r, GetTickCount());
    r->DrawText(10, 10, 940, 24, "WASD move | Space jump | Mouse attack | ESC maps | F4 weapons | F3 animation | F1 bone editor", 15, 0xFFFFFFFF);
    if (weaponOk) {
        const std::string mount = weaponParts.empty() ? weaponBone :
            (weaponParts[0].idleBoneOverride.empty() ? weaponParts[0].attackBone : weaponParts[0].idleBoneOverride);
        const std::string help = boneEditMode
            ? " [EDITING: L/R primary, Shift+L/R secondary, Backspace reset, Ctrl+Shift+S save, F1 exit]"
            : " [F1 edit bones]";
        r->DrawText(10, 54, 1200, 24, ("Weapon: " + weaponStates + " mount=" + mount + help).c_str(), 14, 0xFFB0E0FF);
    }
    int fps = lastDt > 1e-5f ? (int)(1.f / lastDt) : 0;
    r->DrawText(10, 32, 400, 24, ("FPS: " + std::to_string(fps) + "  (F2 = cap 60/144/off)").c_str(), 15, 0xFF66FF66);
    if (mapIdx >= 0 && mapIdx < (int)maps.size() && !maps[mapIdx].bgm.empty()) {
        const auto& tracks = maps[mapIdx].bgm;
        const std::string title = Basename(tracks[bgmIndex]);
        const int x = (std::max)(10, r->width - 420);
        r->DrawRect(x, 58, 410, 48, 0xB0101824);
        r->DrawText(x + 10, 62, 390, 20, ("BGM " + std::to_string(bgmIndex + 1) + "/" + std::to_string(tracks.size()) + ": " + title).c_str(), 14, 0xFFFFFFFF);
        r->DrawText(x + 10, 82, 390, 20, audio::MusicPaused() ? "F5 Play | F6 Next | F7 Previous" : "F5 Pause | F6 Next | F7 Previous", 13, 0xFF80D8FF);
    }

    // Centered aim crosshair (replaces the hidden Windows cursor).
    int cx = r->width / 2, cy = r->height / 2;
    const uint32_t col = 0xE000FF66;   // translucent green
    r->DrawRect(cx-1, cy-1, 2, 2, col);      // center dot
    r->DrawRect(cx-11, cy, 7, 2, col);       // left arm
    r->DrawRect(cx+5,  cy, 7, 2, col);       // right arm
    r->DrawRect(cx, cy-11, 2, 7, col);       // top arm
    r->DrawRect(cx, cy+5,  2, 7, col);       // bottom arm

    if (GetTickCount() < boneEditPopupUntil) {
        const int w = 700, h = 64, x = (r->width - w) / 2, y = 80;
        r->DrawRect(x, y, w, h, 0xE0101820);
        r->DrawRect(x, y, w, 4, boneEditMode ? 0xFF55DD88 : 0xFFFF9966);
        const char* message = boneEditMode
            ? "Bone edit mode enabled - L/R: primary, Shift+L/R: secondary, F1: exit"
            : "Bone edit mode disabled";
        r->DrawText(x + 18, y + 22, w - 36, 24, message, 16, 0xFFFFFFFF);
    }

    // F4: catalogo de armas.
    if (wpnMenu) {
        const int W = 470, H = kMapMenuRows * 20 + 60;
        const int x = (r->width - W) / 2, y = (r->height - H) / 2;
        r->DrawRect(x, y, W, H, 0xE00A121C);
        r->DrawRect(x, y, W, 26, 0xFF1C4A6E);
        r->DrawText(x + 12, y + 5, W - 24, 20,
                    ("Armas (" + std::to_string(weaponList.size()) + ")   flechas - Enter equipa - F4 cierra").c_str(),
                    14, 0xFFDDEEFF);
        for (int i = 0; i < kMapMenuRows && wpnTop + i < (int)weaponList.size(); ++i) {
            const WeaponEntry& e = weaponList[wpnTop + i];
            const int ry = y + 32 + i * 20;
            if (wpnTop + i == wpnSel)            r->DrawRect(x + 4, ry - 2, W - 8, 20, 0x8033AAFF);
            if (e.states == weaponStates)        r->DrawRect(x + 4, ry - 2, 3,     20, 0xFF66FF88);
            r->DrawText(x + 14,  ry, 210, 20, e.states.c_str(), 14, 0xFFFFFFFF);
            r->DrawText(x + 230, ry, 230, 20, e.scn.c_str(),    12, 0xFF88AACC);
        }
    }

    // F3: ajuste de animacion en vivo. Mismos mandos que el menu del cliente JS:
    // piso y techo del cross-fade, velocidad de reproduccion y cadencia de piernas.
    if (animMenu) {
        const int W = 430, H = 4 * 22 + 52;
        const int x = r->width - W - 16, y = 90;
        r->DrawRect(x, y, W, H, 0xE00A121C);
        r->DrawRect(x, y, W, 24, 0xFF1C4A6E);
        r->DrawText(x + 10, y + 4, W - 20, 20, "Animacion (F3)   flechas: elegir / ajustar", 14, 0xFFDDEEFF);
        char buf[96];
        const char* names[4] = { "Blend minimo", "Blend maximo", "Velocidad clip", "Cadencia piernas" };
        for (int i = 0; i < 4; ++i) {
            const int ry = y + 30 + i * 22;
            if (i == animSel) r->DrawRect(x + 4, ry - 2, W - 8, 21, 0x8033AAFF);
            switch (i) {
            case 0: sprintf_s(buf, "%-18s %.0f ms",  names[i], animCtrl.blendFloorMs); break;
            case 1: sprintf_s(buf, "%-18s %.0f ms",  names[i], animCtrl.blendCapMs);   break;
            case 2: sprintf_s(buf, "%-18s x%.2f",    names[i], animCtrl.speedScale);   break;
            default:sprintf_s(buf, "%-18s %.0f u/s", names[i], animRefSpeed);          break;
            }
            r->DrawText(x + 14, ry, W - 28, 20, buf, 14, 0xFFFFFFFF);
        }
    }

    // ESC map picker: every bginfo-*.ini the client ships, loadable at runtime.
    if (mapMenu) {
        const int W = 520, H = kMapMenuRows * 20 + 66;
        const int x = (r->width - W) / 2, y = (r->height - H) / 2;
        r->DrawRect(x, y, W, H, 0xE00A121C);                       // panel
        r->DrawRect(x, y, W, 26, 0xFF1C4A6E);                      // title bar
        r->DrawText(x + 12, y + 5, W - 24, 20,
                    ("Mapas (" + std::to_string(maps.size()) + ")   Flechas / PgUp / PgDn - Enter carga - ESC cierra").c_str(),
                    14, 0xFFDDEEFF);
        for (int i = 0; i < kMapMenuRows && menuTop + i < (int)maps.size(); ++i) {
            const BgInfo& bi = maps[menuTop + i];
            const int ry = y + 32 + i * 20;
            if (menuTop + i == menuSel)   r->DrawRect(x + 4, ry - 2, W - 8, 20, 0x8033AAFF);
            if (menuTop + i == mapIdx)    r->DrawRect(x + 4, ry - 2, 3,     20, 0xFF66FF88);   // mapa activo
            std::string label = bi.ini.substr(7, bi.ini.size() - 11);   // bginfo-XXX.ini -> XXX
            r->DrawText(x + 14, ry, 300, 20, label.c_str(), 14, 0xFFFFFFFF);
            r->DrawText(x + 320, ry, 190, 20, (std::to_string(bi.statics.size()) + " scn").c_str(), 13, 0xFF88AACC);
        }
    }
}

void TitleState_ConnectAuthServer::OnEnter() {
    // TODO: real host/port from version.ini "SERVER" (WinMain LoadConfig).
    conn.Connect("127.0.0.1", 28002);   // placeholder auth endpoint
}
void TitleState_ConnectAuthServer::OnUpdate() {
    conn.Poll();
    // when conn.IsConnected(): send auth-login RMI, then ChangeScene(Lobby).
}
void TitleState_ConnectAuthServer::OnLeave() { conn.Disconnect(); }
