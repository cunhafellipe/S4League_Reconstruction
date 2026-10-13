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
#include "app/respath.h"
#include "app/crash.h"
#include "lua/lua_script_scan.h"
#include "app/reslog.h"
#include "audio/audio.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <set>
#include "app/focus.h"
// Every key/mouse-button read below goes through FocusedKeyState: with the
// game window in the background, keys typed elsewhere must not move it.
#define GetAsyncKeyState FocusedKeyState
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
                                const anim::ActorAnim* attack, bool weapon2Active, bool holstered = false) {
    scn::Pose out = characterPose;
    // The weapon's default clip: the first one after its base track.
    std::string fallback;
    if (scene.animNames.size() > 1) {
        std::string first = scene.animNames[0];
        for (char& c : first) c = (char)std::tolower((unsigned char)c);
        if (first == "base") fallback = scene.animNames[1];
    }
    const std::string assembledClip = fallback;   // before the family filter below
    // Only a clip of the same family as the one the body plays (letters before
    // the digits: R0003 for a Counter Sword's R* clips). arcane_counter_galaxy
    // ships sword clips only (00068...): its first frame stood the blade up
    // out of the hand instead of along the grip.
    auto family = [](const std::string& c) { size_t k = 0; while (k < c.size() && std::isalpha((unsigned char)c[k])) ++k; return c.substr(0, k); };
    if (!fallback.empty() && !characterPose.clipA.empty() && family(fallback) != family(characterPose.clipA)) fallback.clear();
    out.fallbackClip = fallback;
    auto use = [&](const std::string& clip) {
        if (clip.empty() || !WeaponHasClip(scene, clip)) return false;
        out.clipA = clip; out.clipB.clear(); out.blend = 0.f;
        return true;
    };

    // Holstered (FASTRUN, jump... on Gun_Dummy / Spine_Dummy): RunState_<X>'s
    // WK0xx rows are the WALK with the gun in hand - played on the back they
    // threw assult_rifle off its holster. The body's own clip if the weapon
    // has it, else its bind pose.
    if (holstered) {
        if (use(characterPose.clipA)) return out;
        // BASE of a gun is its exploded layout (skull_handgun: the slide, hammer
        // and trigger pieces fanned out - "bent"). Its own first clip, first
        // frame, is the assembled gun.
        scn::Pose bind;
        bind.fallbackClip = fallback;
        if (!assembledClip.empty()) { bind.clipA = assembledClip; bind.tickA = 0; bind.loopA = false; }
        return bind;
    }
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
    scn::Pose bind;   // bind pose is safer than applying a foreign clip name
    bind.fallbackClip = fallback;
    return bind;
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
    blocks = LoadIntro(RESC("resources/resources/intro/_eu_intro.x7"));
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
    character = scn::Parse(RESC("resources/resources/model/interface/title_cha00.scn"));
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

// A gun holstered on the hip (Gun_Dummy, hung on the left thigh) hangs STRAIGHT:
// barrel down, grip straight back at the player - a "|" seen from behind - and
// stays like that whatever the leg does. The position comes from the dummy, the
// orientation is fixed in the character's space: the weapon's barrel axis (its
// fire dummy seen from the mount) goes to "down", and the side the body hangs
// on (the body piece against the barrel pieces, perpendicular to the barrel)
// goes to "backward" (+Z, the model faces -Z). gHolsterRoll45 turns that side
// about the vertical in 45 degree steps (4 = straight back). R is the rotation
// applied in the mount frame, row-vector form, no translation.
static int gHolsterRoll45 = 4;
static unsigned gHolsterGen = 1;
static bool HolsterBarrelDown(const scn::Scene& weapon, const float* invMount, const scn::Scene& /*skel*/,
                              const std::string& /*bone*/, const std::string& mountName, float R[16]) {
    std::string fire;
    auto low = [](std::string n) { for (char& c : n) c = (char)std::tolower((unsigned char)c); return n; };
    for (const scn::Bone& b : weapon.bones)
        if (low(b.name).find("fire") != std::string::npos && low(b.name).find("dummy") != std::string::npos) { fire = b.name; break; }
    if (fire.empty())
        for (const scn::Model& m : weapon.models)
            if (low(m.name).find("fire") != std::string::npos && low(m.name).find("dummy") != std::string::npos) { fire = m.name; break; }
    if (fire.empty()) return false;
    // The assembled gun: its first clip after BASE, first frame (BASE is the exploded layout).
    scn::Pose bp; bp.clipA = "BASE";
    if (weapon.animNames.size() > 1) {
        std::string first = weapon.animNames[0];
        for (char& c : first) c = (char)std::tolower((unsigned char)c);
        if (first == "base") bp.clipA = weapon.animNames[1];
    }
    const scn::NodeResolver nodes = scn::MakeNodeResolver(weapon, bp, true);
    const scn::Mat4* fwp = nodes(fire);
    if (!fwp) return false;
    const scn::Mat4 fw = *fwp;   // the resolver hands out one shared slot
    float invM[16];
    std::memcpy(invM, invMount, sizeof(invM));
    if (!mountName.empty())
        if (const scn::Mat4* mw = nodes(mountName)) { const scn::Mat4 inv = scn::Mat4InvAffine(*mw); std::memcpy(invM, inv.data(), sizeof(invM)); }
    float fm[16]; Mat4MulS(fw.data(), invM, fm);
    float e1[3] = { fm[12], fm[13], fm[14] };   // barrel axis in the mount frame
    const float al = std::sqrt(e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2]);
    if (al < 1e-3f) return false;
    for (float& v : e1) v /= al;

    // The body piece (most vertices) against the others (the barrel pieces):
    // which way the body hangs from the barrel line, in the mount frame.
    size_t bodyIdx = weapon.models.size(), bodyN = 0;
    for (size_t i = 0; i < weapon.models.size(); ++i)
        if (weapon.models[i].vertices.size() > bodyN && !weapon.models[i].faces.empty()) { bodyN = weapon.models[i].vertices.size(); bodyIdx = i; }
    if (bodyIdx == weapon.models.size()) return false;
    double sb[3] = { 0, 0, 0 }, so[3] = { 0, 0, 0 }; size_t nb = 0, no = 0;
    for (size_t mi = 0; mi < weapon.models.size(); ++mi) {
        const scn::Model& m = weapon.models[mi];
        if (m.vertices.empty() || m.faces.empty()) continue;
        const scn::Mat4* mwp = nodes(m.name);
        if (!mwp) continue;
        const scn::Mat4 mwm = *mwp;
        float toMount[16]; Mat4MulS(mwm.data(), invM, toMount);
        for (size_t vi = 0; vi < m.vertices.size(); vi += 2) {
            float w[3]; XformPt(toMount, m.vertices[vi], w);
            if (mi == bodyIdx) { sb[0] += w[0]; sb[1] += w[1]; sb[2] += w[2]; ++nb; }
            else               { so[0] += w[0]; so[1] += w[1]; so[2] += w[2]; ++no; }
        }
    }
    if (nb == 0) return false;
    float g[3];
    for (int i = 0; i < 3; ++i) g[i] = (float)(sb[i] / nb) - (no ? (float)(so[i] / no) : 0.f);
    if (!no) {}   // single piece: the body's own centroid, seen from the mount origin
    const float along = g[0]*e1[0] + g[1]*e1[1] + g[2]*e1[2];
    for (int i = 0; i < 3; ++i) g[i] -= along * e1[i];
    const float gl = std::sqrt(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
    if (gl < 1.f) return false;   // no side to hang on (symmetric): leave it to the bone
    for (float& v : g) v /= gl;
    const float e2[3] = { g[0], g[1], g[2] };
    const float e3[3] = { e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0] };
    // Targets in the character's space: barrel down, body backward (+Z) turned by the roll step.
    const float ang = (gHolsterRoll45 - 4) * 0.78539816f;
    const float f1[3] = { 0.f, -1.f, 0.f };
    const float f2[3] = { std::sin(ang), 0.f, std::cos(ang) };
    const float f3[3] = { f1[1]*f2[2] - f1[2]*f2[1], f1[2]*f2[0] - f1[0]*f2[2], f1[0]*f2[1] - f1[1]*f2[0] };
    // Row-vector form: p_char = p_mount * R, R = sum over i of e_i^T f_i.
    std::memset(R, 0, sizeof(float) * 16);
    const float* E[3] = { e1, e2, e3 };
    const float* F[3] = { f1, f2, f3 };
    for (int k = 0; k < 3; ++k)
        for (int r = 0; r < 3; ++r)
            for (int q = 0; q < 3; ++q) R[r*4+q] += E[k][r] * F[k][q];
    R[15] = 1.f;
    DbgLog("[weapon] holster fixed: barrel down, body side rolled %d deg (body offset %.0f)", (gHolsterRoll45 - 4) * 45, gl);
    return true;
}

// The holstered gun is bolted to the dummy's bone: the animation moves the bone
// (thigh -> Gun_Dummy) and the weapon goes with it, whatever it does. The only
// correction is the weapon's offset FROM the bone: the fixed rotation that makes
// it straight at the bone's bind pose (HolsterBarrelDown: barrel down, grip
// straight back) is expressed in the bone's own frame, so with the bone at bind
// it is that exact pose and as the bone moves it moves rigidly with it.
// out = fixedRot * inverse(bind rotation) * current rotation, at the bone's position.
static void HolsterWithSwing(const scn::Scene& skel, const float* hand, const float* fixedRot, float out[16]) {
    for (const scn::Bone& b : skel.bones)
        if (b.name == "Gun_Dummy") {
            scn::Mat4 bind;
            std::memcpy(bind.data(), b.matrix, sizeof(float) * 16);
            bind[12] = bind[13] = bind[14] = 0.f;
            const scn::Mat4 invBind = scn::Mat4InvAffine(bind);
            float now[16];
            std::memcpy(now, hand, sizeof(now));
            now[12] = now[13] = now[14] = 0.f;
            float tmp[16];
            Mat4MulS(fixedRot, invBind.data(), tmp);
            Mat4MulS(tmp, now, out);
            out[12] = hand[12]; out[13] = hand[13]; out[14] = hand[14];
            return;
        }
    std::memcpy(out, hand, sizeof(float) * 16);
}

// Skin/attach every mesh of every body part against the shared skeleton at
// `tick`, producing world-space pieces (model space, before camera).
// A skeleton bone's world with the upper/lower split the body is skinned with
// (pelvis, root and legs on lowerPose - skin.cpp IsLowerBodyBone). Head items,
// pets and the weapon hand must use the same chain or they drift off the body.
static bool SkelBoneWorld(const scn::Scene& skel, const std::string& name, const scn::AnimPose& pose,
                          const scn::AnimPose* lowerPose, float out[16]) {
    if (!lowerPose) return scn::BoneWorldByName(skel, name, pose, out);
    for (size_t i = 0; i < skel.bones.size(); ++i)
        if (skel.bones[i].name == name) {
            const std::vector<scn::Mat4> w = scn::ResolveBoneWorlds(skel, pose, nullptr, nullptr, lowerPose);
            std::memcpy(out, w[i].data(), sizeof(float) * 16);
            return true;
        }
    return false;
}

static void GatherCharacterPieces(const scn::Scene& skel, const std::vector<scn::Scene>& parts,
                                  const scn::AnimPose& pose, std::vector<CharPiece>& pieces,
                                  const scn::AnimPose* lowerPose = nullptr, unsigned skipSlots = 0) {
    for (const scn::Scene& part : parts) {
        if (part.costumeSlot >= 0 && ((skipSlots >> part.costumeSlot) & 1u)) continue;
        if (part.ownSkeleton) {
            // Pet: animate it with its own skeleton and clip (the character's
            // clip when it has one - 00074 idle, 00008 run... - else its idle
            // or first clip), then carry it with the bone it hangs from:
            // its scene is authored in character space, so only the bone's
            // motion since the bind pose is applied (like a weapon mount).
            // animation_part=1 pets (116_pet_magpie) play the character's clip
            // names; without that clip, and for every other pet, their own
            // BASE loop (the magpie's 40000-tick flight) on its own clock.
            scn::AnimPose petPose = pose;
            petPose.clipB.clear(); petPose.blend = 0.f; petPose.fromWeight = 0.f;
            auto has = [&part](const std::string& c) { return !c.empty() && std::find(part.animNames.begin(), part.animNames.end(), c) != part.animNames.end(); };
            if (!(part.animPart && has(petPose.clipA))) {
                petPose.clipA = has("BASE") ? std::string("BASE") : has("base") ? std::string("base") : (part.animNames.empty() ? std::string() : part.animNames[0]);
                petPose.tickA = (uint32_t)((double)GetTickCount() * 4.8);
            }
            petPose.loopA = true;
            const std::string boneName = part.attachBone.empty() ? std::string("Bip01 Head") : part.attachBone;
            float bindW[16], nowW[16], delta[16];
            // animation_part=1: authored in character space, animated by itself.
            if (part.animPart) { std::memset(delta, 0, sizeof(delta)); delta[0] = delta[5] = delta[10] = delta[15] = 1.f; }
            else if (scn::BoneWorldByName(skel, boneName, scn::AnimPose{}, bindW) && SkelBoneWorld(skel, boneName, pose, lowerPose, nowW)) {
                scn::Mat4 b; std::memcpy(b.data(), bindW, sizeof(bindW));
                const scn::Mat4 inv = scn::Mat4InvAffine(b);
                Mat4MulS(inv.data(), nowW, delta);
            } else { std::memset(delta, 0, sizeof(delta)); delta[0] = delta[5] = delta[10] = delta[15] = 1.f; }
            const scn::NodeResolver nodes = scn::MakeNodeResolver(part, petPose, true);
            for (const scn::Model& m : part.models) {
                CharPiece pc; pc.m = &m; pc.dir = &part.dir;
                if (!m.skin.empty()) {
                    scn::SkinModel(m, part, petPose, pc.verts);
                    std::memcpy(pc.world.data(), delta, sizeof(delta));
                } else {
                    pc.verts = m.vertices;
                    float local[16];
                    if (const scn::Mat4* w = nodes(m.name)) std::memcpy(local, w->data(), sizeof(local));
                    else std::memcpy(local, m.matrix, sizeof(local));
                    Mat4MulS(local, delta, pc.world.data());
                }
                pieces.push_back(std::move(pc));
            }
            continue;
        }
        for (const scn::Model& m : part.models) {
            CharPiece pc; pc.m = &m; pc.dir = &part.dir;
            pc.world = {}; pc.world[0]=pc.world[5]=pc.world[10]=pc.world[15]=1.f;
            if (!m.skin.empty()) {
                // Skin bones the skeleton lacks are the part's own head dummies
                // (00_female_hair is skinned to Hair_Bone_Dummy, which only the
                // hair SCN defines). Resolve them like the rigid parts below -
                // dummy (head-relative) x Bip01 Head - instead of letting them
                // fall to identity, which left the hair at the actor origin.
                std::unordered_map<std::string, scn::Mat4> extra;
                for (const scn::SkinBone& sb : m.skin) {
                    bool inSkel = false;
                    for (const scn::Bone& b : skel.bones) if (b.name == sb.name) { inSkel = true; break; }
                    if (inSkel) continue;
                    float dummyLocal[16], headWorld[16], w[16];
                    if (scn::BoneWorldByName(part, sb.name, pose, dummyLocal) &&
                        SkelBoneWorld(skel, "Bip01 Head", pose, lowerPose, headWorld)) {
                        Mat4MulS(dummyLocal, headWorld, w);
                        scn::Mat4 mw; std::memcpy(mw.data(), w, sizeof(w));
                        extra[sb.name] = mw;
                    }
                }
                if (extra.empty()) scn::SkinModel(m, skel, pose, lowerPose, pc.verts);   // skinned mesh -> bones place verts
                else               scn::SkinModel(m, skel, pose, lowerPose, extra, pc.verts);
            } else {
                pc.verts = m.vertices;
                // Rigid part (face/hair): world = model.matrix * dummyLocal * headWorld
                // (Hair_Bone_Dummy is a head-relative offset; head follows the clip).
                float dummyLocal[16], headWorld[16], tmp[16];
                bool haveDummy = !m.parent.empty() && scn::BoneWorldByName(part, m.parent, pose, dummyLocal);
                // Node items hang from their item.x7 bone (glasses on Bip01
                // Head, belts on Bip01 Pelvis, wings on Bip01 Spine2...).
                bool haveHead  = SkelBoneWorld(skel, part.attachBone.empty() ? std::string("Bip01 Head") : part.attachBone, pose, lowerPose, headWorld);
                // Both the dummy and the model take their local from the initial
                // transform of their OWN track (11_SKIN_1114, 00_MALE_HAIR,
                // 09_FACE... - names the character clips never use), like the
                // SCN importer animating each object over its parent. In the
                // 00_ parts that equals the static matrix; newer items keep
                // the static matrices in another space (138_male_hair: dummy
                // at y=262 rotated, model x100 at the origin) and only the
                // tracks hold the real offsets (dummy 14.5 over Bip01 Head,
                // model -262.3 under the dummy), so using the matrices put the
                // hair/face far off the head.
                auto ownTrack = [](const std::vector<scn::Anim>& anims, float out[16]) {
                    for (const scn::Anim& a : anims)
                        if (a.hasTransform) {
                            const scn::Mat4 local = scn::BuildTRS(a.initT, a.initR, a.initS);
                            std::memcpy(out, local.data(), sizeof(float) * 16);
                            return true;
                        }
                    return false;
                };
                if (haveDummy)
                    for (const scn::Bone& b : part.bones)
                        if (b.name == m.parent) { ownTrack(b.anims, dummyLocal); break; }
                float modelLocal[16];
                if (!ownTrack(m.anims, modelLocal)) std::memcpy(modelLocal, m.matrix, sizeof(modelLocal));
                if (haveDummy && haveHead) { Mat4MulS(modelLocal, dummyLocal, tmp); Mat4MulS(tmp, headWorld, pc.world.data()); }
                else if (haveDummy)        { Mat4MulS(modelLocal, dummyLocal, pc.world.data()); }
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

// Draw a map scene (no skinning): each model by its own node matrix. Animated
// scenes (210 of 612 background .scn: [DYNAMIC] objects, skies, screens,
// gears, blinking lights) loop their clip: moving nodes, visibility tracks,
// vertex and UV ("uvani") keys, at the clip's 4.8 ticks per ms.
static void DrawMapScene(IRenderer* r, const scn::Scene& sc, unsigned nowMs, const float* skyEye = nullptr,
                         std::unordered_map<const scn::Model*, WorldState::MapMeshCache>* cache = nullptr) {
    scn::Pose pose;
    const bool animated = !sc.animNames.empty();
    if (animated) { pose.clipA = sc.animNames[0]; pose.tickA = (uint32_t)((double)nowMs * 4.8); pose.loopA = true; }
    bool anyMoving = false;
    if (animated)
        for (const scn::Model& m : sc.models)
            for (const scn::Anim& a : m.anims)
                if (!a.trans.empty() || !a.rot.empty() || !a.scale.empty()) { anyMoving = true; break; }
    const scn::NodeResolver nodes = anyMoving ? scn::MakeNodeResolver(sc, pose, true) : scn::NodeResolver{};
    std::vector<scn::Vec3> morphPos;
    std::vector<scn::Vec2> morphUv;
    for (const scn::Model& m : sc.models) {
        if (m.vertices.empty() || m.faces.empty()) continue;
        if (m.name.rfind("oct_", 0) == 0) continue;   // collision octree meshes are invisible
        // Game-rule team areas (station2_sector.scn: sector01_ally/_enemy,
        // map-sized additive volumes): not scenery.
        if (m.name.rfind("sector", 0) == 0) continue;
        const size_t nv = m.vertices.size();
        bool hasUv = nv == m.uv1.size();
        const bool hasUv2 = nv == m.uv2.size();
        // Animation of this mesh, if any.
        const float* world = m.matrix;
        float vis = 1.f;
        const std::vector<scn::Vec3>* verts = &m.vertices;
        const std::vector<scn::Vec2>* uvs = &m.uv1;
        if (animated && !m.anims.empty()) {
            bool moves = false;
            for (const scn::Anim& a : m.anims) if (!a.trans.empty() || !a.rot.empty() || !a.scale.empty()) { moves = true; break; }
            if (moves && nodes) if (const scn::Mat4* nw = nodes(m.name)) world = nw->data();
            vis = (std::max)(0.f, (std::min)(1.f, scn::ModelVisibility(m, pose.clipA, pose.tickA, true)));
            if (vis <= 0.004f) continue;
            if (scn::SampleMorphPositions(m, pose.clipA, pose.tickA, true, morphPos)) verts = &morphPos;
            if (hasUv && scn::SampleMorphUvs(m, pose.clipA, pose.tickA, true, morphUv)) uvs = &morphUv;
        }
        float worldCopy[16]; std::memcpy(worldCopy, world, sizeof(worldCopy));   // the resolver reuses its slot
        if (skyEye) {
            // The sky (sky_bluesky: +-862700, "NoDepthTest NoDepthWrite") is
            // drawn around the eye, behind everything. A dome centred on the
            // eye looks the same at any size, so it is scaled inside the far
            // plane (300000); at its own size the far plane cut it to black.
            const float k = 0.1f;
            for (int i = 0; i < 12; ++i) if ((i & 3) != 3) worldCopy[i] *= k;
            for (int i = 0; i < 3; ++i) worldCopy[12 + i] = worldCopy[12 + i] * k + skyEye[i];
        }
        world = worldCopy;
        r->meshFade = vis;
        const uint32_t drawFlags = m.renderFlag | (skyEye ? (uint32_t)scn::RF_RT_NO_DEPTH_TEST : 0u);
        // Vertices and UVs as in the file (no vertex/UV animation this mesh):
        // its GPU buffers are built once and every material is one draw from
        // them - no per-frame vertex rebuild, copy or texture-path lookup.
        if (cache && verts == &m.vertices && uvs == &m.uv1) {
            WorldState::MapMeshCache& c = (*cache)[&m];
            if (c.mesh == -1) {
                std::vector<scn::TexItem> its = m.texItems;
                if (its.empty()) {
                    scn::TexItem one; one.main = m.texture; one.side = m.sideTexture;
                    one.faceStart = 0; one.faceCount = (uint32_t)m.faces.size();
                    its.push_back(one);
                }
                bool anyLm = false;
                for (const scn::TexItem& it : its) if (!it.side.empty()) anyLm = true;
                const int fpv = (anyLm && hasUv2) ? 7 : 5;
                std::vector<float> vb(nv * fpv, 0.f);
                for (size_t i = 0; i < nv; ++i) {
                    float* o = &vb[i * fpv];
                    o[0] = m.vertices[i].x; o[1] = m.vertices[i].y; o[2] = m.vertices[i].z;
                    if (hasUv) { o[3] = m.uv1[i].u; o[4] = m.uv1[i].v; }
                    if (fpv == 7) { o[5] = m.uv2[i].u; o[6] = m.uv2[i].v; }
                }
                std::vector<unsigned short> ib; ib.reserve(m.faces.size() * 3);
                for (const scn::Face& f : m.faces) { ib.push_back(f.a); ib.push_back(f.b); ib.push_back(f.c); }
                c.mesh = r->CreateStaticMesh(vb.data(), fpv, (int)nv, ib.data(), (int)ib.size());
                if (c.mesh < 0) c.mesh = -2;   // could not be made: keep the per-frame path
                for (const scn::TexItem& it : its) {
                    if (it.faceCount == 0 || it.faceStart >= m.faces.size()) continue;
                    const uint32_t cnt = (uint32_t)((std::min)((size_t)it.faceStart + it.faceCount, m.faces.size()) - it.faceStart);
                    c.ranges.push_back({ it.faceStart, cnt });
                    c.tex.push_back(r->LoadTexture((sc.dir.empty() ? it.main : sc.dir + "/" + it.main).c_str()));
                    c.lm.push_back(fpv == 7 && !it.side.empty() ? r->LoadTexture((sc.dir.empty() ? it.side : sc.dir + "/" + it.side).c_str()) : -1);
                }
            }
            if (c.mesh >= 0) {
                for (size_t k = 0; k < c.ranges.size(); ++k)
                    r->DrawStaticMesh(c.mesh, (int)c.ranges[k].first * 3, (int)c.ranges[k].second, c.tex[k],
                                      vis >= 0.999f ? c.lm[k] : -1, world, drawFlags);   // a fading mesh: no lightmap pass (meshFade)
                continue;
            }
        }
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
            // A fading mesh (visibility < 1) goes through DrawMesh, which applies meshFade.
            const bool hasLM = !it.side.empty() && hasUv2 && vis >= 0.999f;
            const std::vector<scn::Vec3>& vv = *verts;
            const std::vector<scn::Vec2>& uu = *uvs;
            if (hasLM) {   // main texture x baked lightmap (uv2) -> warm lit map
                std::string lmPath = sc.dir.empty() ? it.side : (sc.dir + "/" + it.side);
                int lm = r->LoadTexture(lmPath.c_str());
                pu.assign(nv * 7, 0.f);
                for (size_t i = 0; i < nv; ++i) {
                    pu[i*7+0]=vv[i].x; pu[i*7+1]=vv[i].y; pu[i*7+2]=vv[i].z;
                    pu[i*7+3]= hasUv ? uu[i].u : 0.f; pu[i*7+4]= hasUv ? uu[i].v : 0.f;
                    pu[i*7+5]= m.uv2[i].u; pu[i*7+6]= m.uv2[i].v;
                }
                r->DrawMeshLightmap(pu.data(), (int)nv, idx.data(), (int)idx.size(), tex, lm, world, m.renderFlag | (skyEye ? (uint32_t)scn::RF_RT_NO_DEPTH_TEST : 0u));
            } else {
                pu.assign(nv * 5, 0.f);
                for (size_t i = 0; i < nv; ++i) {
                    pu[i*5+0]=vv[i].x; pu[i*5+1]=vv[i].y; pu[i*5+2]=vv[i].z;
                    pu[i*5+3]= hasUv ? uu[i].u : 0.f;  pu[i*5+4]= hasUv ? uu[i].v : 0.f;
                }
                r->DrawMesh(pu.data(), (int)nv, idx.data(), (int)idx.size(), tex, world, m.renderFlag | (skyEye ? (uint32_t)scn::RF_RT_NO_DEPTH_TEST : 0u));
            }
        }
    }
    r->meshFade = 1.f;
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
    ui = gui::LoadXui(RESC("resources/resources/xui/ui/1login.xui"));
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
    ui = gui::LoadXui(RESC("resources/resources/xui/ui/4server.xui"));
    // The off-screen row template (server_list_item_GroupBox @ top=612) is a
    // clone source, not a real widget - hide it so it stops rendering as a
    // stray bar at the bottom. Flatten() skips a subtree when its show=false.
    if (gui::Widget* tmpl = FindWidget(ui, "server_list_item_GroupBox")) tmpl->show = false;
    DbgLog("[server] xui='%s' kids=%zu", ui.type.c_str(), ui.children.size());

    // The male character = male_bip.scn skeleton (81 bones + anims) with the
    // default "00_male_*" body meshes skinned to it. male_bip.scn itself holds
    // only the rig + a default weapon; the body comes from these part files.
    const char* base = RESC("resources/resources/model/character/");
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

static void BuildYawPitchTranslate(float yaw, float pitch, const float pos[3], float o[16]) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    o[0]  = cy;         o[1]  = 0.f;   o[2]  = -sy;        o[3]  = 0.f;
    o[4]  = sp * sy;    o[5]  = cp;    o[6]  = sp * cy;    o[7]  = 0.f;
    o[8]  = cp * sy;    o[9]  = -sp;   o[10] = cp * cy;    o[11] = 0.f;
    o[12] = pos[0];     o[13] = pos[1]; o[14] = pos[2];    o[15] = 1.f;
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

// config.json next to the exe keeps the 3 equipped weapons ("weapons": slots
// 1/2/3). The F4 menu writes it every time a weapon is equipped in game. The
// old weapon_loadout.json is read once, moved into config.json and deleted.
// Missing/empty/unreadable falls back to the 3 known-working defaults.
static const char* kConfigFile = "config.json";
static const char* kOldLoadoutFile = "weapon_loadout.json";

static std::vector<SWeaponSlot> ParseWeaponSlots(const std::string& text) {
    std::vector<SWeaponSlot> out;
    size_t pos = text.find("\"weapons\"");
    pos = pos == std::string::npos ? 0 : pos;
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
    return out;
}

static std::string ReadTextFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// config.json "online": the mini multiplayer relay (server/runServer.bat).
// A missing section is created with a random id and the Windows user name.
struct OnlineConfig { bool enabled = true; std::string server = "127.0.0.1"; int port = 4100; int id = 0; std::string nickname; };
static OnlineConfig gOnline;
// config.json "resource_mode": "normal" = the vanilla tree (resources/);
// "full" = the extended tree given by "full_dir" (see respath.h), whose
// richest script folder is used. Falls back to normal when it is missing.
static std::string gResourceMode = "normal";
static std::string gFullDir;   // config "full_dir" (optional; found automatically otherwise)
// config.json "music_enabled" (default false: silent) and "music_track"
// ("Bgm/x.ogg", empty = the map's own playlist): ESC > Settings, F5, F6.
static bool gMusicOn = false;
static std::string gMusicTrack;
// config.json "graphics_quality": "low" | "medium" | "high" (ESC menu cycles it).
static int gGraphicsQuality = 2;
static const char* const kQualityNames[3] = { "low", "medium", "high" };
// config.json "resolution": "1280x720" (or "window" = whatever size the
// window has) and "display": "windowed" | "borderless" - ESC > Settings.
static int  gResW = 0, gResH = 0;
static bool gBorderless = false;
// config.json "aspect": "auto" or one of kAspects - filters the resolution list.
static const char* const kAspects[6] = { "auto", "4:3", "5:4", "16:10", "16:9", "21:9" };
static int gAspect = 0;
struct ResOpt { int w, h; };
static const ResOpt kResolutions[] = {
    { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 }, { 1366, 768 }, { 1440, 900 },
    { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1080 }, { 2560, 1440 },
    { 3440, 1440 }, { 3840, 2160 } };
static const char* AspectName(int w, int h) {
    const float a = (float)w / (float)h;
    if (std::fabs(a - 4.f / 3.f) < 0.02f) return "4:3";
    if (std::fabs(a - 5.f / 4.f) < 0.02f) return "5:4";
    if (std::fabs(a - 16.f / 10.f) < 0.02f) return "16:10";
    if (std::fabs(a - 16.f / 9.f) < 0.03f) return "16:9";
    if (a > 2.2f) return "21:9";
    return "";
}
static RECT MonitorRect(HWND h, bool work) {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    return work ? mi.rcWork : mi.rcMonitor;
}
// Resolutions that fit the monitor, of the chosen aspect ratio (all when auto).
static std::vector<ResOpt> ResolutionChoices(HWND h) {
    const RECT m = MonitorRect(h, false);
    std::vector<ResOpt> out;
    for (const ResOpt& r : kResolutions)
        if (r.w <= m.right - m.left && r.h <= m.bottom - m.top &&
            (gAspect == 0 || std::string(AspectName(r.w, r.h)) == kAspects[gAspect])) out.push_back(r);
    return out;
}
// Window style/size from the settings; WM_SIZE then resets the renderer's
// back buffer to the new client size.
static void ApplyDisplayMode() {
    HWND h = GameHwndSlot();
    if (!h) return;
    if (gBorderless) {
        const RECT m = MonitorRect(h, false);
        SetWindowLongPtrA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
        SetWindowPos(h, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        return;
    }
    const DWORD style = 0x06CA0000 | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_VISIBLE;
    SetWindowLongPtrA(h, GWL_STYLE, style);
    if (gResW > 0 && gResH > 0) {
        RECT rc = { 0, 0, gResW, gResH };
        AdjustWindowRect(&rc, style, FALSE);
        const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        const RECT wa = MonitorRect(h, true);
        const int x = wa.left + (std::max)(0, ((int)(wa.right - wa.left) - ww) / 2);
        const int y = wa.top + (std::max)(0, ((int)(wa.bottom - wa.top) - wh) / 2);
        ShowWindow(h, SW_RESTORE);
        SetWindowPos(h, HWND_TOP, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}
static std::string ScriptDir() {
    static std::string chosen;   // the root is fixed for the whole run
    if (!chosen.empty()) return chosen;
    chosen = RES("resources/resources/script");
    if (gResourceMode == "full" && respath::Root() != "resources") {
        // The extended tree may carry several script folders (script, and
        // extended ones next to it): take the one with the most .lua files.
        const std::string base = respath::Root() + "/resources/";
        size_t best = 0;
        WIN32_FIND_DATAA dd;
        HANDLE h = FindFirstFileExA((base + "script*").c_str(), FindExInfoBasic, &dd, FindExSearchLimitToDirectories, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(dd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                const std::string dir = base + dd.cFileName;
                if (GetFileAttributesA((dir + "/actorstates_gameplay.lua").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                size_t n = 0;
                WIN32_FIND_DATAA fd;
                HANDLE f = FindFirstFileA((dir + "/*.lua").c_str(), &fd);
                if (f != INVALID_HANDLE_VALUE) { do ++n; while (FindNextFileA(f, &fd)); FindClose(f); }
                if (n > best) { best = n; chosen = dir; }
            } while (FindNextFileA(h, &dd));
            FindClose(h);
        }
    }
    return chosen;
}
static bool gOnlineLoaded = false;

// config.json "equip": the wardrobe (ESC > Vestuario). One .scn per sex and
// slot ("male_body": "x.scn"); a missing key = the default 00_<sex>_<slot>.scn.
static const char* const kCostumeSlots[8] = { "hair", "face", "body", "leg", "hand", "foot", "acc", "pet" };
struct CostumeConfig { bool female = false; std::map<std::string, std::string> part; };
static CostumeConfig gCostume;

// String value of `key` in a flat JSON object; "" when missing or null.
static std::string JsonStringOrNull(const std::string& obj, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = obj.find(pat); if (k == std::string::npos) return "";
    size_t c = obj.find(':', k + pat.size()); if (c == std::string::npos) return "";
    size_t v = obj.find_first_not_of(" \t\r\n", c + 1);
    if (v == std::string::npos || obj[v] != '"') return "";
    size_t q2 = obj.find('"', v + 1); if (q2 == std::string::npos) return "";
    return obj.substr(v + 1, q2 - v - 1);
}

static void WriteWeaponSlots(const std::vector<SWeaponSlot>& slots) {
    std::ofstream f(kConfigFile, std::ios::trunc);
    if (!f) return;
    f << "{\n    \"weapons\": [\n";
    for (size_t i = 0; i < slots.size(); ++i)
        f << "        { \"scn\": \"" << slots[i].scn << "\", \"states\": \"" << slots[i].states << "\" }"
          << (i + 1 < slots.size() ? ",\n" : "\n");
    f << "    ],\n";
    f << "    \"online\": { \"enabled\": " << (gOnline.enabled ? "true" : "false")
      << ", \"server\": \"" << gOnline.server << "\", \"port\": " << gOnline.port
      << ", \"nickname\": \"" << gOnline.nickname << "\" },\n";
    f << "    \"resource_mode\": \"" << gResourceMode << "\",\n";
    if (!gFullDir.empty()) f << "    \"full_dir\": \"" << gFullDir << "\",\n";
    f << "    \"graphics_quality\": \"" << kQualityNames[gGraphicsQuality] << "\",\n";
    f << "    \"music_enabled\": " << (gMusicOn ? "true" : "false") << ",\n";
    f << "    \"music_track\": \"" << gMusicTrack << "\",\n";
    f << "    \"resolution\": \"" << (gResW > 0 ? std::to_string(gResW) + "x" + std::to_string(gResH) : std::string("window")) << "\",\n";
    f << "    \"display\": \"" << (gBorderless ? "borderless" : "windowed") << "\",\n";
    f << "    \"aspect\": \"" << kAspects[gAspect] << "\",\n";
    f << "    \"equip\": { \"gender\": \"" << (gCostume.female ? "female" : "male") << "\"";
    for (const auto& kv : gCostume.part)
        if (!kv.second.empty()) f << ", \"" << kv.first << "\": \"" << kv.second << "\"";
    f << " }\n}\n";
}

static void LoadOnlineConfig() {
    if (gOnlineLoaded) return;
    gOnlineLoaded = true;
    const std::string text = ReadTextFile(kConfigFile);
    const size_t at = text.find("\"online\"");
    if (at != std::string::npos) {
        const std::string obj = text.substr(at);
        gOnline.enabled = obj.find("\"enabled\": false") == std::string::npos && obj.find("\"enabled\":false") == std::string::npos;
        std::string sv = JsonStringField(obj, "server"); if (!sv.empty()) gOnline.server = sv;
        gOnline.port = JsonIntField(obj, "port", 4100);
        if (gOnline.port <= 0 || gOnline.port > 65534) gOnline.port = 4100;
        gOnline.id = JsonIntField(obj, "id", 0);
        gOnline.nickname = JsonStringField(obj, "nickname");
    }
    { const std::string rm = JsonStringOrNull(text, "resource_mode"); if (rm == "full" || rm == "normal") gResourceMode = rm; }
    { const std::string fd = JsonStringOrNull(text, "full_dir"); if (!fd.empty()) gFullDir = fd; }
    {   // music_enabled: true / false (a bare JSON boolean)
        const size_t k = text.find("\"music_enabled\"");
        if (k != std::string::npos) {
            const size_t c = text.find(':', k);
            const size_t e = text.find_first_of(",\n}", c == std::string::npos ? k : c);
            if (c != std::string::npos && e != std::string::npos) gMusicOn = text.substr(c + 1, e - c - 1).find("true") != std::string::npos;
        }
        gMusicTrack = JsonStringOrNull(text, "music_track");
    }
    { const std::string gq = JsonStringOrNull(text, "graphics_quality");
      for (int q = 0; q < 3; ++q) if (gq == kQualityNames[q]) gGraphicsQuality = q; }
    { const std::string res = JsonStringOrNull(text, "resolution");
      int w = 0, h = 0;
      if (sscanf_s(res.c_str(), "%dx%d", &w, &h) == 2 && w >= 640 && h >= 480) { gResW = w; gResH = h; } }
    gBorderless = JsonStringOrNull(text, "display") == "borderless";
    { const std::string asp = JsonStringOrNull(text, "aspect"); for (int a = 0; a < 6; ++a) if (asp == kAspects[a]) gAspect = a; }
    const size_t eq = text.find("\"equip\"");
    if (eq != std::string::npos) {
        const size_t open = text.find('{', eq), close = open == std::string::npos ? open : text.find('}', open);
        if (close != std::string::npos) {
            const std::string obj = text.substr(open, close - open + 1);
            gCostume.female = JsonStringOrNull(obj, "gender") == "female";
            for (const char* sx : { "male", "female" })
                for (const char* sl : kCostumeSlots) {
                    const std::string key = std::string(sx) + "_" + sl, v = JsonStringOrNull(obj, key.c_str());
                    if (!v.empty()) gCostume.part[key] = v;
                }
        }
    }
    if (gOnline.id <= 0) { srand((unsigned)GetTickCount() ^ (unsigned)GetCurrentProcessId()); gOnline.id = 1000 + rand() % 900000; }
    if (gOnline.nickname.empty()) {
        char user[128] = "player"; DWORD n = sizeof(user); GetUserNameA(user, &n);
        gOnline.nickname = user;
    }
}

static std::vector<SWeaponSlot> LoadWeaponSlots() {
    std::vector<SWeaponSlot> defaults = {
        { "crystal_plasma_sword.scn", "PlasmaSword" },
        { "bat.scn",                  "BatSword"    },
        { "railgun.scn",              "StandardRailGun" },
    };
    std::vector<SWeaponSlot> out = ParseWeaponSlots(ReadTextFile(kConfigFile));
    if (out.empty()) {
        out = ParseWeaponSlots(ReadTextFile(kOldLoadoutFile));
        if (!out.empty()) WriteWeaponSlots(out);
    }
    std::remove(kOldLoadoutFile);
    return out.empty() ? defaults : out;
}

// The 3 equipped weapons (keys 1/2/3). Editable in game from the F4 menu,
// which writes the choice back to config.json.
static std::vector<SWeaponSlot> gLoadout = LoadWeaponSlots();
static const bool gOnlineInit = (LoadOnlineConfig(), WriteWeaponSlots(gLoadout), true);

static void SaveWeaponSlots() { WriteWeaponSlots(gLoadout); }

// Weapon exports use both `foo_r/foo_l`, `r_foo/l_foo` and numbered forms
// such as `demonic_knuckle_r_01`. Return the canonical pair without guessing
// from the weapon type or from the number of models inside the SCN.
// Punto de anclaje. El cliente lo saca de un id de la tabla de partes del
// item (vtbl+0x134); sin esa tabla lo deducimos del nombre del dummy raiz.
static std::string WeaponBoneFromMount(const std::string& mn) {
    auto has = [&mn](const char* s) { return mn.find(s) != std::string::npos; };
    if (has("Sniper") || has("Gun") || has("gun") || has("Smg") || has("Mg2") || has("Hw") ||
        has("assult") || has("rocket") || has("ShotGun") || has("Revolver") || has("Minegun") ||
        has("AreaRG") || has("sharpshooter") || has("Turret") || has("Chainlightning") || has("Dual"))
        return "Gun_Dummy";
    if (has("Wing")) return "Spine_Dummy";
    return "R_Hand_Dummy";
}

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
#define kMapInfoDir RESC("resources/resources/mapinfo/")
#define kBgDir RESC("resources/resources/model/background/")

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
        else if (section == "ADDON_SEQUENCE" && key.rfind("object_", 0) == 0 && !val.empty()) {
            const int n = atoi(key.c_str() + 7);
            const bool dummy = key.size() > 6 && key.compare(key.size() - 6, 6, "_Dummy") == 0;
            auto& slot = out.addonSeqs[n];
            (dummy ? slot.second : slot.first) = val;
        }
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
    reslog::Note("loading map %s", bi.ini.c_str());
    mapIdx = index;
    PlayMapMusic(0);
    ReleaseClones();   // its clones stay with the old map
    map.clear(); collTris.clear(); mapSkyIdx = -1; mapCacheDirty = true;   // static buffers rebuilt on the next frame

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
        if (mm == bi.sky) mapSkyIdx = (int)map.size();
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
    spawnPoints.clear();
    for (const scn::Box* b : spawns) spawnPoints.push_back({ b->matrix[12], b->matrix[13], b->matrix[14] });
    grounded = false; velY = 0.f; velX = 0.f; velZ = 0.f; camInit = false;

    // [ADDON_SEQUENCE]: each .seq loops on its Dummy node of the map scenes.
    activeEffects.erase(std::remove_if(activeEffects.begin(), activeEffects.end(),
                        [](const ActiveEffect& e) { return e.tag == "map"; }), activeEffects.end());
    for (const auto& kv : bi.addonSeqs) {
        const std::string& file = kv.second.first; const std::string& dummy = kv.second.second;
        if (file.empty()) continue;
        const float* M = nullptr;
        for (const scn::Scene& s : map) {
            for (const scn::Bone& b : s.bones)   if (b.name == dummy) { M = b.matrix; break; }
            if (!M) for (const scn::Model& m : s.models) if (m.name == dummy) { M = m.matrix; break; }
            if (!M) for (const scn::Box& b : s.boxes)   if (b.name == dummy) { M = b.matrix; break; }
            if (M) break;
        }
        if (!M && !dummy.empty()) { DbgLog("[world] addon seq '%s': node '%s' not in the map", file.c_str(), dummy.c_str()); continue; }
        static const float kOrigin[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        if (!M) M = kOrigin;
        const size_t before = activeEffects.size();
        SpawnSeq("Resources/Effects/" + file, M[12], M[13], M[14], std::atan2(M[8], M[10]), "map");
        if (activeEffects.size() > before) activeEffects.back().followActor = false;
    }
    DbgLog("[world] map '%s' (%s): scenes=%zu collTris=%zu spawns=%zu charPos=(%.0f,%.0f,%.0f)",
           bi.name.c_str(), bi.ini.c_str(), map.size(), collTris.size(), spawns.size(),
           charPos[0], charPos[1], charPos[2]);
}

// Every .ogg of the Bgm folder, as "Bgm/<file>" (what the maps' [BGM] lists use).
static const std::vector<std::string>& MusicTracks() {
    static std::vector<std::string> tracks;
    static bool built = false;
    if (!built) {
        built = true;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(RESC("resources/resources/bgm/*.ogg"), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do tracks.push_back(std::string("Bgm/") + fd.cFileName); while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::sort(tracks.begin(), tracks.end());
        DbgLog("[bgm] %zu tracks in the Bgm folder", tracks.size());
    }
    return tracks;
}

// Turns the music on with the next / previous track of the folder and saves it.
void WorldState::ChangeMusic(int dir) {
    const std::vector<std::string>& tracks = MusicTracks();
    if (tracks.empty()) return;
    int cur = -1;
    for (size_t i = 0; i < tracks.size(); ++i) {
        std::string a = tracks[i], b = gMusicTrack;
        for (char& c : a) c = (char)std::tolower((unsigned char)c);
        for (char& c : b) c = (char)std::tolower((unsigned char)c);
        if (a == b) { cur = (int)i; break; }
    }
    const int n = (int)tracks.size();
    const int next = cur < 0 ? (dir > 0 ? 0 : n - 1) : ((cur + dir) % n + n) % n;
    gMusicTrack = tracks[next];
    gMusicOn = true;
    PlayMapMusic(0);
    SaveWeaponSlots();
}

void WorldState::PlayMapMusic(int index) {
    if (!gMusicOn) { audio::StopMusic(); return; }
    if (!gMusicTrack.empty()) {   // the track chosen in the menu / config.json
        audio::PlayMusic(gMusicTrack);
        DbgLog("[bgm] chosen track '%s'", gMusicTrack.c_str());
        return;
    }
    if (mapIdx < 0 || mapIdx >= (int)maps.size() || maps[mapIdx].bgm.empty()) { audio::StopMusic(); return; }
    const auto& list = maps[mapIdx].bgm;
    bgmIndex = (index % (int)list.size() + (int)list.size()) % (int)list.size();
    audio::PlayMusic(list[bgmIndex]);
    DbgLog("[bgm] map='%s' track=%d/%zu '%s'", maps[mapIdx].name.c_str(), bgmIndex + 1, list.size(), list[bgmIndex].c_str());
}

// Loads a .seq effect and starts it playing at a world position. Silently
// does nothing if the file can't be found/parsed or has no particle nodes -
// missing effects shouldn't ever crash or block an attack from landing.
void WorldState::SpawnSeq(const std::string& relPath, float x, float y, float z, float yaw, const std::string& tag,
                          float localScale, float playbackRate, float pitch) {
    if (!seqEffectsEnabled || relPath.empty()) return;
    // "Resources/Effects/x.seq" or "Effects/x.seq" (scripts without the
    // Resources/ prefix): both live under resources/resources/.
    std::string fullPath = relPath;
    std::replace(fullPath.begin(), fullPath.end(), '\\', '/');
    std::string lowPath = fullPath;
    for (char& c : lowPath) c = (char)std::tolower((unsigned char)c);
    fullPath = (lowPath.rfind("resources/", 0) == 0 ? RES("resources/") : RES("resources/resources/")) + fullPath;
    seq::Sequence s = seq::Parse(fullPath.c_str());
    if (!s.ok) {
        DbgLog("[seq] '%s' FAILED to parse: %s (declared nodeCount=%u, nodes found=%zu, version=%.3f)",
               fullPath.c_str(), s.error.c_str(), s.nodeCount, s.nodes.size(), s.version);
        return;
    }

    ActiveEffect e;
    e.x = x; e.y = y; e.z = z; e.yaw = yaw; e.pitch = pitch; e.startTick = GetTickCount(); e.duration = s.duration;
    e.localScale = localScale; e.playbackRate = playbackRate;
    e.tag = tag;
    int particleNodes = 0, modelNodes = 0;
    for (const seq::Node& n : s.nodes) {
        if (n.isParticle) {
            if (!n.ok) {
                DbgLog("[seq] '%s': invalid particle node '%s'", fullPath.c_str(), n.nodeName.c_str());
                continue;
            }
            ++particleNodes;
            EffectNode en{ seq::ParticlePlayer(n, localScale), n.textures, n.blendType == 1 };
            e.nodes.push_back(std::move(en));
        } else if (!n.modelFile.empty()) {
            // .seq model paths are rooted at Resources ("\effects\x.scn",
            // "Resources\Effects\x.scn"); the extracted files live under
            // resources/resources/. "resources/" + "\effects\..." never
            // opened, so every effect lost its meshes and only sparks showed.
            std::string rel = n.modelFile;
            for (char& c : rel) if (c == '\\') c = '/';
            while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
            std::string low = rel; for (char& c : low) c = (char)tolower((unsigned char)c);
            // Paths are often the artist's absolute ones (d:/s4/work/client/
            // resources/effects/x.scn, /zinteam/.../resources/effects/x.scn):
            // keep what follows the LAST "resources/". A bare name, or one still
            // not found, lives with the effects.
            const size_t lastRes = low.rfind("resources/");
            if (lastRes != std::string::npos) rel = rel.substr(lastRes + 10);
            if (!std::filesystem::exists(RES("resources/resources/") + rel)) {
                const size_t sl = rel.find_last_of('/');
                rel = "effects/" + (sl == std::string::npos ? rel : rel.substr(sl + 1));
            }
            scn::Scene model = scn::Parse((RES("resources/resources/") + rel).c_str());
            if (model.ok) { ++modelNodes; e.models.push_back({ std::move(model), n.beginTime, n.endTime, n.fadeIn, n.fadeOut, n.loaderLoop, n.animName }); }
            else DbgLog("[seq] model '%s' -> 'resources/resources/%s' failed: %s", n.modelFile.c_str(), rel.c_str(), model.error.c_str());
        }
    }
    DbgLog("[seq] '%s' ok, %zu nodes, %d particle nodes, %d model nodes, duration=%ums",
           fullPath.c_str(), s.nodes.size(), particleNodes, modelNodes, s.duration);
    if (!e.nodes.empty() || !e.models.empty()) activeEffects.push_back(std::move(e));
}

// Advances and draws every effect still running, dropping the ones that have
// finished (a little past their own duration, so trailing particles don't cut
// off mid-fade).
void WorldState::DrawEffects(IRenderer* r, unsigned tnow, const float* previewPlace, const scn::AnimPose* previewPose) {
    if (!r || !seqEffectsEnabled) return;
    const bool preview = previewPlace != nullptr;
    const scn::AnimPose& pose = previewPose ? *previewPose : curPose;
    std::vector<seq::Particle> parts;
    for (size_t i = activeEffects.size(); i-- > 0; ) {
        ActiveEffect& e = activeEffects[i];
        if (preview && e.tag != "costume") continue;   // the preview only shows what the clothes carry
        if (e.followActor && !preview) {
            e.x = charPos[0]; e.y = charPos[1]; e.z = charPos[2];
            e.yaw = charYaw + 3.14159265f;
            e.pitch = 0.f;
        }
        float nodeLocal[16];
        // Weapon effects: the weapon's node as drawn this frame (hand or holster).
        auto weaponNode = [&]() {
            if (e.tag == "costume") return false;
            auto it = weaponNodeChar.find(e.attachNode);
            if (it == weaponNodeChar.end()) return false;
            std::memcpy(nodeLocal, it->second.data(), sizeof(nodeLocal));
            return true;
        };
        const bool onNode = !e.attachNode.empty() && (weaponNode() || CostumeNodeWorld(e.attachNode, nodeLocal, &pose) ||
                            (!e.attachFallback.empty() && scn::BoneWorldByName(character, e.attachFallback, pose, nodeLocal)));
        if (e.tag == "weaponfx" && !onNode) continue;   // weapon not drawn this frame: nothing at the feet
        unsigned age = (unsigned)((float)(tnow - e.startTick) * e.playbackRate);
        if (e.frozen) age = e.frozenAge;   // a clone's copy: the instant it was left
        else if (preview) { if (e.duration > 0) age %= e.duration; }
        else {
            // A state-owned effect (fastrun trail) replays while its state lasts.
            if (!e.tag.empty() && e.duration > 0 && age >= e.duration) { e.startTick = tnow; age = 0; }
            if (e.duration > 0 && age > e.duration + 1000) { activeEffects.erase(activeEffects.begin() + i); continue; }
        }
        if (!e.frozen) e.lastAge = age;
        // Particles live in the effect's own space, like its model nodes: the
        // emitter cones (e.g. (0,1,-1) = forward and up) turn with the actor.
        // Adding only the position sent every spray the same world direction
        // whichever way the swing faced, off from the effect's own meshes.
        float effPos[3] = { e.x, e.y, e.z };
        if (e.offX != 0.f || e.offZ != 0.f) {   // effect-local shift, turned with the effect
            const float c = std::cos(e.yaw), s = std::sin(e.yaw);
            effPos[0] += e.offX * c + e.offZ * s;
            effPos[2] += -e.offX * s + e.offZ * c;
        }
        float effWorld[16]; BuildYawPitchTranslate(e.yaw, e.pitch, effPos, effWorld);
        if (preview) std::memcpy(effWorld, previewPlace, sizeof(effWorld));
        if (e.frozen && !preview) std::memcpy(effWorld, e.fixedWorld.data(), sizeof(effWorld));
        if (onNode) { float w[16]; Mat4MulS(nodeLocal, effWorld, w); std::memcpy(effWorld, w, sizeof(w)); }
        float modelEffectWorld[16]; std::memcpy(modelEffectWorld, effWorld, sizeof(effWorld));
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col)
                modelEffectWorld[row*4 + col] *= e.localScale;
        if (!preview && !e.frozen) {
            std::array<float, 16> m; std::memcpy(m.data(), effWorld, sizeof(effWorld));
            e.history.push_back({ tnow, m });
            while (e.history.size() > 2 && tnow - e.history.front().first > 6000) e.history.erase(e.history.begin());
        }
        auto placementAt = [&e, &effWorld, preview](unsigned t) -> const float* {
            if (preview || e.history.empty()) return effWorld;
            for (size_t h = e.history.size(); h-- > 0; )
                if (e.history[h].first <= t) return e.history[h].second.data();
            return e.history.front().second.data();
        };
        for (EffectNode& en : e.nodes) {
            en.player.Update((float)age, parts);
            if (parts.empty()) continue;
            const bool worldFixed = en.player.attachTarget();
            // One batch per texture frame (texture animation).
            const int frames = (std::max)(1, (int)en.textures.size());
            for (int fr = 0; fr < frames; ++fr) {
            std::vector<float> buf;
            buf.reserve(parts.size() * 12);
            const size_t keepOf10 = r->quality <= 0 ? 4 : r->quality == 1 ? 7 : 10;   // graphics quality: particle density
            for (size_t k = 0; k < parts.size(); ++k) {
                const seq::Particle& p = parts[k];
                if (k % 10 >= keepOf10) continue;
                if (frames > 1 && p.frame != fr) continue;
                buf.resize(buf.size() + 12);
                float* o = &buf[buf.size() - 12];
                const float* W = worldFixed ? placementAt(tnow - (unsigned)p.age) : effWorld;
                o[0] = p.x*W[0] + p.y*W[4] + p.z*W[8]  + W[12];
                o[1] = p.x*W[1] + p.y*W[5] + p.z*W[9]  + W[13];
                o[2] = p.x*W[2] + p.y*W[6] + p.z*W[10] + W[14];
                // The client's quad (S1 sub_536CD0): keyframe sizeX/sizeY are
                // half extents, shaped by the node's RenderType (camera-facing,
                // stretched along the velocity - bullets -, upright, flat),
                // turned by the key's rotation.
                o[3] = p.r; o[4] = p.g; o[5] = p.b; o[6] = p.a; o[7] = p.sizeX; o[8] = p.sizeY;
                if (en.player.renderType() == 1) {
                    o[9]  = p.vx*W[0] + p.vy*W[4] + p.vz*W[8];
                    o[10] = p.vx*W[1] + p.vy*W[5] + p.vz*W[9];
                    o[11] = p.vx*W[2] + p.vy*W[6] + p.vz*W[10];
                } else {
                    o[9] = p.rotate; o[10] = o[11] = 0.f;
                }
            }
            if (buf.empty()) continue;
            const seq::TexRef* texture = en.textures.empty() ? nullptr : &en.textures[fr];
            int tex = !texture || texture->file.empty() ? -1 : r->LoadTexture(texture->file.c_str());
            r->DrawParticles(buf.data(), (int)(buf.size() / 12), tex, en.additive,
                             10 + (int)(std::min)(3u, en.player.renderType()), texture ? texture->uv : nullptr);
            }
        }
        for (EffectModel& em : e.models) {
            // Each effect mesh is placed by its own node (an expanding ring
            // authored at scale 0.009 grows through its clip) and animated
            // like the S4 viewer does: the scene's clip at tick = ms * 4.8
            // (clip length 9600 ticks = the .seq's 2000 ms), held at its end.
            // CActNodeLoader: on from begin to end, fading in over FadeInTime
            // and out over the last FadeOutTime ms (gun_hit: 0-1000, 500/500).
            float fade = 1.f;
            if (em.end > em.begin) {
                // On from begin to end, then fading out over FadeOutTime AFTER end
                // (skill_swordman_battack part1: 0-300 ms, fade 500; its meshes
                // run visibility tracks to ~480 ms, so cutting at 300 clipped it).
                if (age < em.begin || age > em.end + em.fadeOut) continue;
                if (em.fadeIn)  fade = (std::min)(fade, (float)(age - em.begin) / (float)em.fadeIn);
                if (age > em.end && em.fadeOut) fade = (std::min)(fade, 1.f - (float)(age - em.end) / (float)em.fadeOut);
                if (fade <= 0.f) continue;
            }
            scn::Pose effPose;
            effPose.clipA = !em.anim.empty() ? em.anim : em.model.animNames.empty() ? std::string() : em.model.animNames[0];
            effPose.tickA = (uint32_t)((float)(age - (std::min)(age, em.begin)) * 4.8f);
            effPose.loopA = em.loop;
            r->meshFade = fade;
            const scn::NodeResolver effNodes = scn::MakeNodeResolver(em.model, effPose, true);
            for (const scn::Model& m : em.model.models) {
                if (m.vertices.empty() || m.faces.empty()) continue;
                // The mesh's own visibility track (opacity keys; mapeff_gun_
                // ricochet: three hex layers, peak 0.7, each gone in ~170 ms).
                const float vis = (std::max)(0.f, (std::min)(1.f, scn::ModelVisibility(m, effPose.clipA, effPose.tickA, effPose.loopA)));
                if (vis * fade <= 0.004f) continue;
                r->meshFade = fade * vis;
                float world[16];
                if (const scn::Mat4* node = effNodes(m.name)) Mat4MulS(node->data(), modelEffectWorld, world);
                else Mat4MulS(m.matrix, modelEffectWorld, world);
                bool hasUv = m.vertices.size() == m.uv1.size();
                // Vertex animation (the dash's slash grows by stretching its vertices).
                std::vector<scn::Vec3> morphed;
                const bool isMorph = scn::SampleMorphPositions(m, effPose.clipA, effPose.tickA, effPose.loopA, morphed);
                const std::vector<scn::Vec3>& vsrc = isMorph ? morphed : m.vertices;
                std::vector<float> pu(m.vertices.size() * 5);
                for (size_t i = 0; i < m.vertices.size(); ++i) {
                    pu[i*5+0]=vsrc[i].x; pu[i*5+1]=vsrc[i].y; pu[i*5+2]=vsrc[i].z;
                    pu[i*5+3]= hasUv ? m.uv1[i].u : 0.f; pu[i*5+4]= hasUv ? m.uv1[i].v : 0.f;
                }
                std::vector<unsigned short> idx; idx.reserve(m.faces.size() * 3);
                for (const scn::Face& f : m.faces) { idx.push_back(f.a); idx.push_back(f.b); idx.push_back(f.c); }
                std::string texPath = em.model.dir.empty() ? m.texture : (em.model.dir + "/" + m.texture);
                int tex = r->LoadTexture(texPath.c_str());
                // The engine reads render modes from tags in the node NAME
                // ("cc as morph ... nodepthwrite alphablend2", S10 strings
                // alphablend1/alphablend2/nodepthwrite/nocull/billboard).
                std::string lname = m.name; for (char& c : lname) c = (char)tolower((unsigned char)c);
                uint32_t flags = m.renderFlag;
                if (lname.find("alphablend2") != std::string::npos) flags |= 0x40000000u;
                else if (lname.find("alphablend") != std::string::npos) flags |= 2u;
                if (lname.find("nodepthwrite") != std::string::npos) flags |= 64u;
                r->DrawMesh(pu.data(), (int)m.vertices.size(), idx.data(), (int)idx.size(), tex, world, flags);
            }
            r->meshFade = 1.f;
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
struct WeaponFireFuncs {
    std::string file, weak, strong, strong1, weapon2, jump;
    std::map<std::string, std::string> fileOf;   // function -> script file to run it from
    const std::string& FileFor(const std::string& fn) const { auto it = fileOf.find(fn); return it != fileOf.end() ? it->second : file; }
};
// The weapon's swing functions, from every Weapon_*.lua and every script in a
// subfolder: some script trees split the bodies into script/attack/*.lua (Standard
// PlasmaSwordAttack2 - the dash - and the jump attack live in attack/weapon_
// plasmasword_attack.lua; the top-level file only holds skin wrappers like
// ..._Lightsaber_Kylo). Scanning the top level only left the plasma without
// its dash and stun data. A function in a subfolder file runs from the top-
// level weapon file that requires it, so its helpers and constants load too.
// The exact name (Standard<X>StandAttack_Weak...) wins over skin variants.
static WeaponFireFuncs FindWeaponFireFuncs(const std::string& scriptDir, const std::string& weaponStates) {
    WeaponFireFuncs out;
    namespace fs = std::filesystem;
    static const std::regex funcRe(R"(function\s+([A-Za-z0-9_]+)\s*\()");
    static const std::regex reqRe(R"(require\s*\(?\s*"([^"]+)\")");
    std::error_code ec;
    struct Src { std::string rel, text; bool top; std::vector<std::string> fns, reqs; };
    std::vector<Src> files;
    for (auto it = fs::recursive_directory_iterator(scriptDir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const fs::path rp = fs::relative(it->path(), scriptDir, ec);
        std::string rel = rp.generic_string();
        std::string lower = rel; for (char& c : lower) c = (char)tolower((unsigned char)c);
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".lua") != 0) continue;
        const bool top = lower.find('/') == std::string::npos;
        if (top && (lower.rfind("weapon_", 0) != 0 || lower == "weapon_init.lua")) continue;
        std::ifstream f(it->path(), std::ios::binary);
        if (!f) continue;
        std::ostringstream ss; ss << f.rdbuf();
        Src src{ rel, ss.str(), top, {}, {} };
        if (luaw::IsBytecodeFile(it->path().string())) {
            // Compiled script (vanilla resources): what it defines and requires, by running it.
            const luaw::ScriptScan& scan = luaw::ScanScript(it->path().string());
            src.fns = scan.functions; src.reqs = scan.requires_;
        } else {
            for (auto m = std::sregex_iterator(src.text.begin(), src.text.end(), funcRe); m != std::sregex_iterator(); ++m) src.fns.push_back((*m)[1].str());
            for (auto m = std::sregex_iterator(src.text.begin(), src.text.end(), reqRe); m != std::sregex_iterator(); ++m) src.reqs.push_back((*m)[1].str());
        }
        files.push_back(std::move(src));
    }
    // subfolder file -> top-level weapon file that requires it
    std::map<std::string, std::string> requiredBy;
    for (const Src& f : files) {
        if (!f.top) continue;
        for (const std::string& rq : f.reqs) {
            std::string r = rq;
            for (char& c : r) { if (c == '\\') c = '/'; c = (char)tolower((unsigned char)c); }
            for (const char* pre : { "resources/script/", "script/" }) if (r.rfind(pre, 0) == 0) { r = r.substr(strlen(pre)); break; }
            requiredBy.emplace(r, f.rel);
        }
    }
    std::vector<std::string> weakC, strongC, strong1C, weapon2C, jumpC, attack1C;
    for (const Src& f : files) {
        std::string lowRel = f.rel; for (char& c : lowRel) c = (char)tolower((unsigned char)c);
        auto rb = requiredBy.find(lowRel);
        const std::string runFile = f.top ? f.rel : (rb != requiredBy.end() ? rb->second : f.rel);
        for (const std::string& fn : f.fns) {
            if (out.fileOf.count(fn)) continue;
            out.fileOf[fn] = runFile;   // every function: the item rows name any of them
            if (fn.find(weaponStates) == std::string::npos) continue;
            if (fn.find("StandAttack_Strong1") != std::string::npos) strong1C.push_back(fn);
            else if (fn.find("StandAttack_Strong") != std::string::npos) strongC.push_back(fn);
            else if (fn.find("StandAttack_Weak") != std::string::npos) weakC.push_back(fn);
            else if (fn == "Standard" + weaponStates + "Attack2" || fn == weaponStates + "Attack2" ||
                     (fn.find("Attack2_") != std::string::npos && fn.size() > weaponStates.size() &&
                      fn.compare(fn.size() - weaponStates.size(), weaponStates.size(), weaponStates) == 0)) weapon2C.push_back(fn);   // StandardPlasmaSwordAttack2_Lightsaber
            else if (fn == "Standard" + weaponStates + "Attack1" || fn == weaponStates + "Attack1") attack1C.push_back(fn);
            // Air attack function names in the S10 scripts: Standard<X>JumpAttack,
            // Standard<X>JumpDash (counter sword meteor, sigma, breaker, katana A,
            // twin blade - the ones carrying STATE:SetDrop), <X>_JumpAttack and
            // JumpAttackBy<X>.
            else if (fn == "Standard" + weaponStates + "JumpAttack" || fn == weaponStates + "JumpAttack" ||
                     fn == "Standard" + weaponStates + "JumpDash"   || fn == weaponStates + "JumpDash" ||
                     fn == "Standard" + weaponStates + "_JumpAttack" || fn == "JumpAttackBy" + weaponStates) jumpC.push_back(fn);
        }
    }
    // S10 InitSpyDaggerAttack1 is the weak swing (CSwordStandAttack_Weak_
    // Setting) and runs Standard<X>Attack1: weapons whose left click is
    // named that way have no StandAttack_Weak function.
    if (weakC.empty()) weakC = attack1C;
    auto pick = [&](const std::vector<std::string>& cands, const std::string& exact) -> std::string {
        for (const std::string& c : cands) if (c == exact) return c;
        std::string best;
        for (const std::string& c : cands)   // the plain one, not a skin variant (..._Lightsaber_Kylo)
            if (best.empty() || (c.rfind("Standard", 0) == 0) > (best.rfind("Standard", 0) == 0) ||
                ((c.rfind("Standard", 0) == 0) == (best.rfind("Standard", 0) == 0) && c.size() < best.size())) best = c;
        return best;
    };
    const std::string S = "Standard" + weaponStates;
    out.weak = pick(weakC, S + "StandAttack_Weak"); out.strong = pick(strongC, S + "StandAttack_Strong");
    out.strong1 = pick(strong1C, S + "StandAttack_Strong1"); out.weapon2 = pick(weapon2C, S + "Attack2");
    out.jump = pick(jumpC, S + "JumpAttack");
    for (const std::string* fn : { &out.weak, &out.weapon2, &out.strong, &out.jump, &out.strong1 })
        if (!fn->empty()) { out.file = out.fileOf[*fn]; break; }
    // Skin types (Lightsaber: StandardPlasmaSwordStandAttack_Weak_Lightsaber_
    // Green) borrow whatever they lack - dash, jump attack - from the base
    // type their weak swing is named after (PlasmaSword).
    static const std::regex baseRe(R"(^Standard([A-Za-z0-9]+?)StandAttack_)");
    std::smatch bm;
    if (!out.weak.empty() && std::regex_search(out.weak, bm, baseRe) && bm[1].str() != weaponStates &&
        (out.weapon2.empty() || out.jump.empty() || out.strong.empty() || out.strong1.empty())) {
        static int depth = 0;
        if (depth == 0) {
            ++depth;
            const WeaponFireFuncs base = FindWeaponFireFuncs(scriptDir, bm[1].str());
            --depth;
            auto take = [&](std::string& mine, const std::string& theirs) {
                if (mine.empty() && !theirs.empty()) { mine = theirs; out.fileOf[theirs] = base.FileFor(theirs); }
            };
            take(out.weapon2, base.weapon2); take(out.jump, base.jump); take(out.strong, base.strong); take(out.strong1, base.strong1);
        }
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
    if (slot < 0 || slot >= (int)gLoadout.size()) {   // fuera de rango = desarmado
        weaponSlot = -1; weaponOk = false; weapon = scn::Scene(); weaponStates.clear();
        weaponPieceWorlds.clear(); weaponParts.clear(); weaponTypeId.reset();
        activeEffects.erase(std::remove_if(activeEffects.begin(), activeEffects.end(),
                            [](const ActiveEffect& e) { return e.tag == "weaponfx"; }), activeEffects.end());
        animCtrl.LoadFromLua(luaStates, "");
        return;
    }
    weaponSlot = slot;
    EquipWeaponFile(gLoadout[slot].scn.c_str(), gLoadout[slot].states.c_str());
}

// The item that uses this .scn (_eu_weapon.x7, then resources/xbn/weapon.xml;
// first listing wins): its <ability type> and its weapon_lua.x7 rows.
struct SceneItem {
    long key = 0; int type = -1; std::string normal, run, attack, second;   // second = <scene value2>
    std::string gunState;   // left_attack ani=UseWeapon1State_<X>: a gun's own weapon type suffix
    std::string fireFunc;   // left_attack weapon= (the gun's Lua fire function)
    std::string crosshair;  // <resources crosshair_file>
    float rate = 0.f, range = 0.f, power = 0.f; int mag = 0; std::string reloadSound;   // <ability>, <resources reload_sound_file>
    anim::ItemLuaRows rows;   // every weapon_lua.x7 row: state -> weapon= / ani=
    // <sequence node_name_i file_name_i> (weapon file 1) and <sequence2
    // node_name_2_i file_name_2_i> (file 2): effects that live on nodes of
    // the weapon while it is equipped (guillotine's cs_hand_dummy, lightning
    // sword's Effect_Dummy01..03). An entry with no node is not hung anywhere
    // (the sigma's body change/un-change: played by its transform).
    struct NodeSeq { std::string node, file; int part = 0; };
    std::vector<NodeSeq> nodeSeqs;
    std::string otherType, attackSeq, attackOgg;   // <other_effects other_type attack_seq attack_ogg>
};
static const SceneItem* SceneItemOf(const std::string& scnFile) {
    static std::unordered_map<std::string, SceneItem> byScene;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::unordered_map<long, SceneItem> byKey;
        const std::string lua = ReadTextFile(RESC("resources/xml/weapon_lua.x7"));
        for (size_t p = 0; (p = lua.find("<lua_func item_key=\"", p)) != std::string::npos; ) {
            p += 20;
            const long k = strtol(lua.c_str() + p, nullptr, 10);
            const size_t e = lua.find("/>", p);
            const std::string tag = lua.substr(p, e - p);
            p = e;
            auto attr = [&](const char* name) {
                const std::string key = std::string(" ") + name + "=\"";
                const size_t q = tag.find(key);
                return q == std::string::npos ? std::string() : tag.substr(q + key.size(), tag.find('"', q + key.size()) - q - key.size());
            };
            const std::string rowState = attr("state");
            if (!rowState.empty()) {
                anim::ItemLuaRow& row = byKey[k].rows[rowState];
                row.weapon = attr("weapon"); row.ani = attr("ani");
            }
            const size_t a = tag.find("ani=\"");
            if (a == std::string::npos) continue;
            const std::string ani = tag.substr(a + 5, tag.find('"', a + 5) - a - 5);
            auto after = [&](const char* pre) { const size_t q = ani.find(pre); return q == std::string::npos ? std::string() : ani.substr(q + strlen(pre)); };
            SceneItem& it = byKey[k];
            if (tag.find("state=\"normal\"") != std::string::npos) it.normal = after("NormalState_");
            else if (tag.find("state=\"run\"") != std::string::npos) it.run = after("RunState_");
            else if (tag.find("state=\"left_attack\"") != std::string::npos) {
                const size_t w = tag.find("weapon=\"");
                if (w != std::string::npos) it.fireFunc = tag.substr(w + 8, tag.find('"', w + 8) - w - 8);
                it.gunState = after("UseWeapon1State_");
            }
            else if (tag.find("state=\"left_weak\"") != std::string::npos) {
                const std::string rest = after("State_");   // SwordAtkWeakState_PlasmaSword[_Lightsaber_Kylo]
                it.attack = rest.substr(0, rest.find('_'));
            }
        }
        for (const char* path : { RESC("resources/xml/_eu_weapon.x7"), RESC("resources/resources/xbn/weapon.xml") }) {
            const std::string x = ReadTextFile(path);
            for (size_t p = 0; (p = x.find("<weapon item_key=\"", p)) != std::string::npos; ) {
                const long k = strtol(x.c_str() + p + 18, nullptr, 10);
                const size_t e = x.find("</weapon>", p);
                const std::string blk = x.substr(p, e == std::string::npos ? std::string::npos : e - p);
                p = (e == std::string::npos) ? x.size() : e;
                const size_t a = blk.find("ability type=\"");
                const size_t sc = blk.find("value1=\"");
                if (sc == std::string::npos) continue;
                std::string f = blk.substr(sc + 8, blk.find('"', sc + 8) - sc - 8);
                for (char& c : f) c = (char)std::tolower((unsigned char)c);
                if (byScene.count(f)) continue;
                SceneItem si = byKey.count(k) ? byKey[k] : SceneItem{};
                si.key = k;
                si.type = a == std::string::npos ? -1 : atoi(blk.c_str() + a + 14);
                auto num = [&](const char* name) -> float {
                    const std::string key = std::string(name) + "=\"";
                    const size_t q = blk.find(key);
                    return q == std::string::npos ? 0.f : (float)atof(blk.c_str() + q + key.size());
                };
                si.rate = num(" rate_of_fire"); si.range = num(" range"); si.mag = (int)num(" magazine_capacity");
                si.power = num(" power");
                const size_t xh = blk.find("crosshair_file=\"");
                if (xh != std::string::npos) si.crosshair = blk.substr(xh + 16, blk.find('"', xh + 16) - xh - 16);
                const size_t rs = blk.find("reload_sound_file=\"");
                if (rs != std::string::npos) si.reloadSound = blk.substr(rs + 19, blk.find('"', rs + 19) - rs - 19);
                const size_t v2 = blk.find("value2=\"");
                if (v2 != std::string::npos) si.second = blk.substr(v2 + 8, blk.find('"', v2 + 8) - v2 - 8);
                auto tagAttr = [&](const char* tagName, const std::string& name) -> std::string {
                    const size_t t0 = blk.find(std::string("<") + tagName + " ");
                    if (t0 == std::string::npos) return std::string();
                    const size_t t1 = blk.find("/>", t0);
                    const std::string tg = blk.substr(t0, t1 == std::string::npos ? std::string::npos : t1 - t0);
                    const std::string key = " " + name + "=\"";
                    const size_t q = tg.find(key);
                    return q == std::string::npos ? std::string() : tg.substr(q + key.size(), tg.find('"', q + key.size()) - q - key.size());
                };
                for (int i = 1; i <= 16; ++i) {
                    const std::string file = tagAttr("sequence", "file_name_" + std::to_string(i));
                    if (!file.empty()) si.nodeSeqs.push_back({ tagAttr("sequence", "node_name_" + std::to_string(i)), file, 0 });
                    const std::string file2 = tagAttr("sequence2", "file_name_2_" + std::to_string(i));
                    if (!file2.empty()) si.nodeSeqs.push_back({ tagAttr("sequence2", "node_name_2_" + std::to_string(i)), file2, 1 });
                }
                si.otherType = tagAttr("other_effects", "other_type");
                si.attackSeq = tagAttr("other_effects", "attack_seq");
                si.attackOgg = tagAttr("other_effects", "attack_ogg");
                byScene.emplace(f, si);
            }
        }
    }
    std::string f = scnFile;
    for (char& c : f) c = (char)std::tolower((unsigned char)c);
    auto it = byScene.find(f);
    return it == byScene.end() ? nullptr : &it->second;
}

// maingame_ingame.xui (1024x768 layout): where the client draws the crosshair
// and the ammo arc - GroupBox_bullet_boxA/B/C, ten
// stacked bullet_N arc segments each, plus bullet_empty_Image behind them.
struct HudLayout {
    bool loaded = false;
    // CCustomGUI_CrossHair hangs from the full-screen crosshair_GroupBox: the
    // crosshair is the screen centre - which is also where the ammo arc's
    // quarter circle is centred (bullet_empty.dds ink corner (10,9)/90 of the
    // 504,376 64x64 rect -> 511,382).
    float xhairCx = 512.f, xhairCy = 384.f;
    int arcL = 504, arcT = 376, arcR = 568, arcB = 440;
    std::string box[3][10];                          // [A,B,C][index 0 = bullet_10 .. 9 = bullet_1] texture
    std::string empty;
};
static const HudLayout& GetHudLayout() {
    static HudLayout h;
    if (h.loaded) return h;
    h.loaded = true;
    const std::string x = ReadTextFile(RESC("resources/resources/gui/new/maingame_ingame.xui"));
    auto rectOf = [&](size_t at, int& l, int& t, int& r, int& b) {
        const size_t g = x.find("<global ", at); if (g == std::string::npos) return false;
        auto num = [&](const char* k) { const size_t q = x.find(std::string(k) + "=\"", g); return q == std::string::npos ? 0 : atoi(x.c_str() + q + strlen(k) + 2); };
        l = num("left"); t = num("top"); r = num("right"); b = num("bottom"); return true;
    };
    const char* names[3] = { "GroupBox_bullet_boxA", "GroupBox_bullet_boxB", "GroupBox_bullet_boxC" };
    for (int k = 0; k < 3; ++k) {
        const size_t s0 = x.find(std::string("name=\"") + names[k] + "\"");
        if (s0 == std::string::npos) continue;
        const size_t s1 = x.find("<CGUI_GroupBox name=", s0 + 10);
        for (size_t q = s0; (q = x.find("<CGUI_Image name=\"", q)) != std::string::npos && q < s1; ++q) {
            const size_t n0 = q + 18, n1 = x.find('"', n0);
            const std::string nm = x.substr(n0, n1 - n0);
            const size_t tx = x.find("texture=\"", n1);
            if (tx == std::string::npos || tx > s1) continue;
            const std::string tex = x.substr(tx + 9, x.find('"', tx + 9) - tx - 9);
            if (nm == "bullet_empty_Image") { h.empty = tex; rectOf(q, h.arcL, h.arcT, h.arcR, h.arcB); continue; }
            if (nm.rfind("bullet_", 0) == 0) {
                const int idx = 10 - atoi(nm.c_str() + 7);   // bullet_10 -> 0 .. bullet_1 -> 9
                if (idx >= 0 && idx < 10) h.box[k][idx] = tex;
            }
        }
    }
    return h;
}

// Third-person camera: the view centre (crosshair) at head top - the original
// keeps it just over the head (Bip01 HeadNub: male y 279, female 269) - and
// sideoffset 130 over the shoulder.
// Camera pitch limits (rad, + = looking down): 77 degrees up, 69 down. The old
// 20 degrees up kept the eye above the ground; the camera now collides instead.
static const float kPitchUp = -1.35f, kPitchDown = 1.2f;
static const float kCamHeightOffset = 280.f, kCamSideOffset = 130.f;

// weapon_type_reload_info.x7: <weapon_type type="N"><normal reload_period="ms">.
static unsigned ReloadPeriodOfType(int type) {
    static std::unordered_map<int, unsigned> byType;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        const std::string x = ReadTextFile(RESC("resources/xml/weapon_type_reload_info.x7"));
        for (size_t p = 0; (p = x.find("<weapon_type type=\"", p)) != std::string::npos; ) {
            const int t = atoi(x.c_str() + p + 19);
            const size_t e = x.find("</weapon_type>", p);
            const size_t n = x.find("<normal ", p);
            if (n != std::string::npos && n < e) {
                const size_t r = x.find("reload_period=\"", n);
                if (r != std::string::npos && r < e) byType[t] = (unsigned)atoi(x.c_str() + r + 15);
            }
            p = (e == std::string::npos) ? x.size() : e;
        }
    }
    auto it = byType.find(type);
    return it == byType.end() ? 2000u : it->second;
}

// <ability type="N"> of the weapon item whose <scene value1> is this file. -1 when not listed.
static int AbilityTypeOfScene(const std::string& scnFile) {
    const SceneItem* si = SceneItemOf(scnFile);
    return si ? si->type : -1;
}
void WorldState::EquipWeaponFile(const char* scnName, const char* states) {
    mpWeaponScn = scnName ? scnName : "";
    if (mpClient.Connected()) mpClient.SendEquip(mpWeaponScn, states ? states : "");
    weaponOk = false;
    weapon = scn::Scene();
    weaponParts.clear();
    weaponAdjustBones.clear();
    weaponStates.clear();
    weaponPieceWorlds.clear();
    weaponTypeId.reset();
    weaponWornParts.clear();
    std::string requestedFile = scnName ? scnName : "";
    std::string primaryFile = requestedFile;
    std::filesystem::path requestedPath(requestedFile);
    std::string requestedStem = requestedPath.stem().string();
    std::string rightStem, leftStem;
    const bool splitWeapon = SplitWeaponPair(requestedStem, rightStem, leftStem);
    if (splitWeapon) {
        const std::string rightFile = rightStem + ".scn";
        if (std::filesystem::exists(std::filesystem::path(RES("resources/resources/model/weapon")) / rightFile))
            primaryFile = rightFile; // sceneIndex 0 is always the right piece.
    }
    weapon = scn::Parse((std::string(RES("resources/resources/model/weapon/")) + primaryFile).c_str());
    // Iron Boots: the x7 scene (ironboots_01.scn) is not a weapon model - the
    // boots are model/character/foot/<name>_foot.scn, skinned to the
    // character's calves and feet. Worn like a costume part, not held.
    bool wornWeapon = false;
    if (!weapon.ok || weapon.models.empty()) {
        const std::string worn = RES("resources/resources/model/character/foot/") + requestedStem + "_foot.scn";
        if (std::filesystem::exists(worn)) {
            scn::Scene w = scn::Parse(worn.c_str());
            if (w.ok && !w.models.empty()) { weapon = std::move(w); wornWeapon = true; DbgLog("[weapon] '%s' is worn: %s", scnName, worn.c_str()); }
        }
    }
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
    // No root is the weapon dummy (124_weapon_bat: Sword_Wea_Dummy hangs under
    // its IK Chain001 root): the client hangs the weapon by its *_Wea_Dummy.
    if (!mount) for (const scn::Bone& b : weapon.bones) if (b.name.find("Wea_Dummy") != std::string::npos) { mount = &b; break; }
    if (!mount) for (const scn::Bone* b : roots) if (b->name.find("Dummy") != std::string::npos || b->name.find("dummy") != std::string::npos) { mount = b; break; }
    if (!mount && !roots.empty()) mount = roots[0];

    if (mount) scn::WeaponMountInverse(weapon, *mount, weaponInvMount);
    else { float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; std::memcpy(weaponInvMount, id, sizeof(id)); }
    // Counter Sword skins carry the real grip as CS_Right_Dummy. In most it
    // sits on Cs_Wea_Dummy (0-4 units off), but dragonsoul_* and zghost_*
    // put it ~12 units away (-8.9, 7.8 in mount space), which left the
    // dagger beside the hand. Hang the piece by that grip when it is clearly
    // off the mount (pose = the weapon's default clip, as when idle).
    for (const scn::Bone& g : weapon.bones) {
        if (g.name != "CS_Right_Dummy" || !mount) continue;
        scn::Pose dp;
        if (weapon.animNames.size() > 1) {
            std::string first = weapon.animNames[0];
            for (char& c : first) c = (char)std::tolower((unsigned char)c);
            if (first == "base") dp.fallbackClip = weapon.animNames[1];
        }
        const scn::NodeResolver nodes = scn::MakeNodeResolver(weapon, dp, true);
        if (const scn::Mat4* gw = nodes(g.name)) {
            const float* p = gw->data() + 12;
            const float* I = weaponInvMount;
            const float d[3] = { p[0]*I[0] + p[1]*I[4] + p[2]*I[8]  + I[12],
                                 p[0]*I[1] + p[1]*I[5] + p[2]*I[9]  + I[13],
                                 p[0]*I[2] + p[1]*I[6] + p[2]*I[10] + I[14] };
            if (std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]) > 6.f) {
                for (int k = 0; k < 3; ++k) weaponInvMount[12 + k] -= d[k];
                DbgLog("[weapon] '%s' grip CS_Right_Dummy (%.1f %.1f %.1f) off the mount - hanging by it", primaryFile.c_str(), d[0], d[1], d[2]);
            }
        }
        break;
    }
    scn::ResolveWeaponPieceWorlds(weapon, weaponPieceWorlds);

    const std::string mn = mount ? mount->name : std::string();
    weaponBone = WeaponBoneFromMount(mn);

    weaponStates = states;
    // Per-item animation tables (weapon_lua.x7), as the client: the attacks
    // name the weapon type (Rapier / golden_lgndz_sword: SwordAtkWeakState_
    // PlasmaSword), while idle and run may borrow another weapon's
    // (NormalState_SpyDagger, RunState_Katana_A_Sword).
    std::string normalAs, runAs;
    weaponRows.clear();
    weaponItemRange = weaponItemPower = 0.f;
    luaw::SetWeaponRange(0.f);
    weaponAttackSeq.clear(); weaponAttackOgg.clear();
    activeEffects.erase(std::remove_if(activeEffects.begin(), activeEffects.end(),
                        [](const ActiveEffect& e) { return e.tag == "weaponfx"; }), activeEffects.end());
    if (const SceneItem* si = SceneItemOf(requestedFile)) {
        weaponRows = si->rows;
        weaponItemRange = si->range; weaponItemPower = si->power;
        luaw::SetWeaponRange(weaponItemRange);
        // The item's own effects on its weapon nodes, looped while equipped.
        for (const SceneItem::NodeSeq& ns : si->nodeSeqs) {
            if (ns.node.empty()) continue;
            const size_t before = activeEffects.size();
            SpawnSeq("Resources/Effects/" + ns.file, charPos[0], charPos[1], charPos[2], charYaw + 3.14159265f, "weaponfx");
            if (activeEffects.size() > before) {
                activeEffects.back().attachNode = ns.node;
                // Skeleton bones (sigma's Bip01 buff) hang off the body.
                if (ns.node.rfind("Bip01", 0) == 0) activeEffects.back().attachFallback = ns.node;
            }
            DbgLog("[weaponfx] item %ld node '%s' seq '%s' %s", si->key, ns.node.c_str(), ns.file.c_str(),
                   activeEffects.size() > before ? "spawned" : "FAILED");
        }
        if (si->otherType == "counter_sword") { weaponAttackSeq = si->attackSeq; weaponAttackOgg = si->attackOgg; }
        // <sequence> entries with no node: the shape change's own effects - the
        // first on transforming, the second on reverting (sigmablade_body_change
        // / sigmablade_body_un_change).
        weaponChangeOnSeq.clear(); weaponChangeOffSeq.clear();
        for (const SceneItem::NodeSeq& ns : si->nodeSeqs) {
            if (!ns.node.empty()) continue;
            if (weaponChangeOnSeq.empty()) weaponChangeOnSeq = ns.file;
            else if (weaponChangeOffSeq.empty()) weaponChangeOffSeq = ns.file;
        }
        // The item's own rows name its weapon type: left_weak for swords
        // (SwordAtkWeakState_X), left_attack for guns (UseWeapon1State_X).
        // The menu's token guess for the .scn only fills in when they do not.
        // A suffix the dispatch tables do not list takes its WEAPONTYPE from the
        // item's <ability type> (assault / homing / spark rifles, turrets...).
        const int abilityType = si->type;
        const std::string abilityName = abilityType >= 0 ? data::WeaponTypeName(respath::Root(), abilityType) : std::string();
        std::string cand = !si->attack.empty() ? si->attack : !si->gunState.empty() ? si->gunState
                         : (!si->fireFunc.empty() ? si->normal : std::string());
        if (!cand.empty()) {
            luaStates.AddTypeSuffix(cand, abilityName);
            if (luaStates.attackMap().typeBySuffix.count(cand)) weaponStates = cand;
        }
        if (!si->normal.empty() && si->normal != weaponStates) normalAs = si->normal;
        if (!si->run.empty() && si->run != weaponStates) runAs = si->run;
        if (weaponStates != (states ? states : "") || !normalAs.empty() || !runAs.empty())
            DbgLog("[weapon] '%s' item %ld: attacks '%s' idle '%s' run '%s'", scnName, si->key, weaponStates.c_str(),
                   normalAs.empty() ? weaponStates.c_str() : normalAs.c_str(), runAs.empty() ? weaponStates.c_str() : runAs.c_str());
    }
    weaponOk = true;
    // A new weapon starts untransformed (sigma blade shape change).
    anim::SetShapeChange(false); weaponChangePending = false;
    weaponNormalAs = normalAs; weaponRunAs = runAs;
    int n = animCtrl.LoadFromLua(luaStates, weaponStates.c_str(), normalAs.c_str(), runAs.c_str());
    animCtrl.ForceRefresh();   // apply the new weapon's pose immediately, even mid-run/walk
    weaponAnim = anim::ResolveWeaponAnim(luaStates.parsed(), weaponStates, &luaStates.attackMap(), &luaStates.attribMap(),
                                         weaponRows.empty() ? nullptr : &weaponRows);
    ghostTrail = nullptr; ghostSamples.clear();
    atkParam = nullptr; weakCombo = 0; swingAttacks = nullptr;   // clear any attack still playing from the previous weapon
    weapon2Active = false; weapon2IsJump = false; weapon2DropApplied = false; weapon2Phase = 0;
    // Real per-piece bone table (weapon_attach.x7) needs the weapon's numeric
    // WEAPONTYPE_* - resolved from its Lua suffix via the same dispatch table
    // used for attacks/combos.
    weaponTypeId = data::TypeFromSuffix(respath::Root(), weaponStates, luaStates.attackMap().typeBySuffix);
    // Suffix with no ActorState_Normal dispatch row (RESCUE_GUN ->
    // WEAPONTYPE_RESCUEGUN, DemonicKnuckle): the item's own <ability type>
    // (_eu_weapon.x7 / xbn weapon.xml) is the WEAPONTYPE_* index itself.
    if (!weaponTypeId) {
        const int t = AbilityTypeOfScene(requestedFile);
        if (t >= 0) weaponTypeId = t;
    }
    weaponCrosshair.clear();
    if (const SceneItem* si = SceneItemOf(requestedFile)) weaponCrosshair = si->crosshair;
    gunFiring = gunReloading = false; gunNextShotAt = 0; gunMuzzleValid = false;
    gunRate = 0.f; gunMag = gunAmmo = 0; gunRange = 3000.f; gunReloadSound.clear();
    if (weaponAnim.isGun) {
        if (const SceneItem* si = SceneItemOf(requestedFile)) {
            gunRate = si->rate; gunMag = si->mag; if (si->range > 0.f) gunRange = si->range;
            gunReloadSound = si->reloadSound;
        }
        if (gunRate <= 0.f) gunRate = 2.f;
        if (gunMag <= 0) gunMag = 30;
        gunAmmo = gunMag;
        gunReloadMs = weaponTypeId ? ReloadPeriodOfType(*weaponTypeId) : 2000u;
        gunHasOrigin = false; gunAnimMs = 0.f; gunRays.clear(); gunRayGroups = 1; gunRoundOffsets.clear(); gunPendingRounds.clear(); gunShotIndex = 0;
        if (const SceneItem* si = SceneItemOf(requestedFile)) if (!si->fireFunc.empty()) {
            // The function lives in one of the weapon_*.lua files.
            const std::string scriptDir = ScriptDir();
            std::string file;
            WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((scriptDir + "/weapon_*.lua").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    const std::string path = scriptDir + "/" + fd.cFileName;
                    if (luaw::IsBytecodeFile(path)) {
                        const luaw::ScriptScan& scan = luaw::ScanScript(path);
                        if (std::find(scan.functions.begin(), scan.functions.end(), si->fireFunc) != scan.functions.end()) { file = fd.cFileName; break; }
                        continue;
                    }
                    const std::string txt = ReadTextFile(path.c_str());
                    if (txt.find("function " + si->fireFunc + "(") != std::string::npos) { file = fd.cFileName; break; }   // relative: RunWeaponFire prepends scriptDir
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            if (!file.empty()) {
                luaw::WeaponFireState st;
                auto runFire = [&](float oneShot, luaw::WeaponFireState* out) {
                    return luaw::RunWeaponFire(scriptDir.c_str(), "third_party/lua_weapon_shim.lua", file.c_str(), si->fireFunc.c_str(), oneShot, 0, 0, 0, out);
                };
                auto distinctTimes = [](const std::vector<luaw::WeaponAttack>& a) {
                    std::vector<float> t;
                    for (const auto& x : a) if (x.shape.kind == "ray" && std::find(t.begin(), t.end(), x.fireTime) == t.end()) t.push_back(x.fireTime);
                    return t.size();
                };
                // A burst: the attacks carry their own fireTimes (SMG2 3 rounds at
                // 10/65/120 ms) or scale them by OneShotTime (machine gun, turret:
                // (i-1) * OneShotTime). Probe both; the scaled ones are fitted
                // into the state's normal time (InitLua arg 1).
                luaw::WeaponFireState st0;
                auto attacks = runFire(0.f, &st0);
                size_t nRays = 0; for (const auto& x : attacks) if (x.shape.kind == "ray") ++nRays;
                if (nRays > 1 && distinctTimes(attacks) <= 1) {
                    luaw::WeaponFireState stp;
                    const auto probe = runFire(100.f, &stp);
                    if (distinctTimes(probe) > 1) {
                        const float normalMs = st0.initArgs.empty() ? 750.f : st0.initArgs[0];
                        const float oneShot = (std::min)(143.f, (std::max)(30.f, normalMs / (float)nRays));
                        attacks = runFire(oneShot, &st);
                        DbgLog("[gun] burst scaled by OneShotTime: %zu rounds in %.0f ms (normal %.0f)", nRays, oneShot * (nRays - 1), normalMs);
                    } else attacks = runFire(0.f, &st);
                } else attacks = runFire(0.f, &st);
                for (const luaw::WeaponAttack& wa : attacks) {
                    if (wa.shape.kind != "ray" || wa.shape.dirZ > -1e-4f) continue;
                    GunRay gr; gr.x = wa.shape.dirX / -wa.shape.dirZ; gr.y = wa.shape.dirY / -wa.shape.dirZ;
                    gr.fireTime = wa.fireTime;
                    if (wa.push.size() >= 3) { gr.pushMs = wa.push[0]; gr.pushSpeed = wa.push[1]; gr.pushAccel = wa.push[2]; }
                    for (size_t k = 0; k + 2 < wa.offsets.size(); k += 3) {
                        const float z = wa.offsets[k + 2] < -1e-4f ? -wa.offsets[k + 2] : 1.f;
                        gr.off.push_back(wa.offsets[k] / z); gr.off.push_back(wa.offsets[k + 1] / z);
                    }
                    gunRays.push_back(std::move(gr));
                }
                // Attacks sharing a fireTime go out together (revolver 4 rays,
                // shotgun 7 pellets); different fireTimes are a SEQUENCE - the
                // machine gun's 20 spread rays are 20 consecutive shots, one
                // per trigger tick, cycling.
                {
                    std::vector<float> times;
                    for (const GunRay& g : gunRays) if (std::find(times.begin(), times.end(), g.fireTime) == times.end()) times.push_back(g.fireTime);
                    std::sort(times.begin(), times.end());
                    gunRayGroups = (int)(std::max)((size_t)1, times.size());
                    gunRoundOffsets.clear();
                    for (float t : times) gunRoundOffsets.push_back(t - (times.empty() ? 0.f : times.front()));
                    for (GunRay& g : gunRays) g.group = (int)(std::find(times.begin(), times.end(), g.fireTime) - times.begin());
                }
                DbgLog("[gun] ray groups=%d, rays %zu, pattern=%zu push[0]=(%.0fms, %.0f, %.0f)", gunRayGroups, gunRays.size(), gunRays.empty() ? (size_t)0 : gunRays[0].off.size() / 2,
                       gunRays.empty() ? 0.f : gunRays[0].pushMs, gunRays.empty() ? 0.f : gunRays[0].pushSpeed, gunRays.empty() ? 0.f : gunRays[0].pushAccel);
                if (st.loaded && st.initArgs.size() >= 6) {
                    gunHeight = st.initArgs[0]; gunSide0 = st.initArgs[1]; gunSide1 = st.initArgs[2];
                    gunFront = st.initArgs[3]; gunAnimMs = st.initArgs[5];
                    gunHasOrigin = true;
                }
                DbgLog("[gun] fire func '%s' (%s) loaded=%d InitLua=%zu args height=%.0f side=%.0f/%.0f front=%.0f animMs=%.0f",
                       si->fireFunc.c_str(), file.c_str(), (int)st.loaded, st.initArgs.size(), gunHeight, gunSide0, gunSide1, gunFront, gunAnimMs);
                // The attack attribute is the one the fire function names
                // (Attack1:InitAttack1(WEAPONTYPE_SMG3, ATTACKATTRIB_SMG3_GUN, ...)),
                // not the first of its weapon type - the scripts list a skin variant
                // per type (ATTACKATTRIB_SMG3_GUN_PRISM...) with other effects.
                const std::string txt = ReadTextFile((scriptDir + "/" + file).c_str());
                const size_t f0 = txt.find("function " + si->fireFunc + "(");
                const size_t f1 = f0 == std::string::npos ? f0 : txt.find("\nfunction ", f0 + 9);
                const std::string fn = f0 == std::string::npos ? std::string() : txt.substr(f0, f1 == std::string::npos ? std::string::npos : f1 - f0);
                static const std::regex attRe(R"(InitAttack1\s*\(\s*\w+\s*,\s*(ATTACKATTRIB_\w+))");
                std::smatch am;
                if (std::regex_search(fn, am, attRe)) {
                    const auto it = luaStates.attribMap().find(am[1].str());
                    if (it != luaStates.attribMap().end() && (!it->second.gunBlastSeq.empty() || !it->second.gunTracerSeq.empty())) {
                        const anim::AttackAttrib& a = it->second;
                        weaponAnim.gunBlastSeq = a.gunBlastSeq; weaponAnim.gunBlastSound = a.gunBlastSound;
                        if (!a.gunFireBone.empty()) weaponAnim.gunFireBone = a.gunFireBone;
                        weaponAnim.gunWallSeq = a.wallHitSeq; weaponAnim.gunWallSound = a.wallHitSound;
                        weaponAnim.gunHitSeq = a.hitSeq; weaponAnim.gunHitSound = a.hitSound;
                        weaponAnim.gunTracerSeq = a.gunTracerSeq;
                    }
                    DbgLog("[gun] attack attrib '%s' (%s)", am[1].str().c_str(), it != luaStates.attribMap().end() ? "found" : "missing");
                }
            }
        }
        // Tracer beam: the CActLightning nodes of the attribute's tracer .seq.
        gunBeamNodes.clear(); gunBeams.clear(); gunBeamDuration = 0;
        if (!weaponAnim.gunTracerSeq.empty()) {
            std::string p = weaponAnim.gunTracerSeq; std::replace(p.begin(), p.end(), '\\', '/');
            std::string low = p; for (char& c : low) c = (char)std::tolower((unsigned char)c);
            p = (low.rfind("resources/", 0) == 0 ? RES("resources/") : RES("resources/resources/")) + p;
            const seq::Sequence ts = seq::Parse(p.c_str());
            for (const seq::Node& n : ts.nodes)
                if (n.isLightning) gunBeamNodes.push_back({ n.beam, n.beginTime, n.endTime });
            gunBeamDuration = ts.duration;
            DbgLog("[gun] tracer '%s' ok=%d beams=%zu dur=%u%s", p.c_str(), (int)ts.ok, gunBeamNodes.size(), ts.duration,
                   gunBeamNodes.empty() ? "" : (" tex=" + gunBeamNodes[0].beam.texture + " width=" + std::to_string(gunBeamNodes[0].beam.unitWidth)).c_str());
        }
        DbgLog("[gun] '%s' rate=%.2f/s mag=%d range=%.0f reload=%ums blast='%s' wall='%s' fireNode='%s'", scnName, gunRate, gunMag, gunRange,
               gunReloadMs, weaponAnim.gunBlastSeq.c_str(), weaponAnim.gunWallSeq.c_str(), weaponAnim.gunFireBone.c_str());
    }

    // Keep the already-resolved primary piece and, when this is a *_r/*_l
    // weapon, load its companion as sceneIndex 1. The table is per file:
    // CounterSword's second blade belongs to Bip01 L Forearm, not the right hand.
    WeaponRenderPart primary;
    primary.scene = weapon;
    std::memcpy(primary.invMount, weaponInvMount, sizeof(primary.invMount));
    primary.pieceWorlds = weaponPieceWorlds;
    // A Counter Sword LEFT piece worn alone (counter_vr_shield: its root is
    // Counter_Sword_Dummy like every *_l file, no _r companion) takes the left
    // row of weapon_attach.x7, not the right hand.
    const int primaryRow = (!splitWeapon && mount && mount->name == "Counter_Sword_Dummy") ? 1 : 0;
    primary.sceneIndex = primaryRow;
    if (mount) primary.mountName = mount->name;
    primary.attackBone = weaponTypeId ? data::AttachBone(respath::Root(), *weaponTypeId, primaryRow, false) : "";
    if (primary.attackBone.empty()) primary.attackBone = primaryRow ? std::string("L_Hand_Dummy") : weaponBone;
    primary.idleBone = weaponTypeId ? data::AttachBone(respath::Root(), *weaponTypeId, primaryRow, true) : "";
    if (primary.idleBone.empty()) primary.idleBone = primary.attackBone;
    // link=1 pieces go through CWeapon::LoadAttachWeaponModel (sub_71E8C0):
    // every child of the SCN root is hung from the bone with its authored
    // transform - no Wea_Dummy, nothing cancelled. Only link=0 pieces
    // (LoadWeaponModel) hang by their mount dummy.
    if (weaponTypeId && data::AttachLink(respath::Root(), *weaponTypeId, primaryRow) == 1) {
        const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::memcpy(primary.invMount, id, sizeof(id));
        primary.mountName.clear();
    }
    // Weapons rigged on a character bone instead of a *_Wea_Dummy
    // (seal_dag: seal_dag_armature sits exactly on Bip01 R Forearm's bind
    // pose, the claw hand 46 units down that forearm): hang them from that
    // bone, cancelling the root's own place. On R_Hand_Dummy (the dagger row
    // of weapon_attach.x7) the claw came out reversed and off the arm.
    // seal_dag has no root dummy at all (its armature hangs under BONESYSTEM),
    // so look at every top bone of the weapon's own rig.
    // A real dummy mount (vital_shock_Dummy, Counter_Sword_Dummy) keeps the
    // normal path: those sit near the origin, where the rule below matched
    // Bip01 Footsteps and hung the weapon from the floor.
    const scn::Bone* rigRoot = (mount && mount->name.find("Dummy") == std::string::npos &&
                                mount->name.find("dummy") == std::string::npos) ? mount : nullptr;
    if (!mount) for (const scn::Bone& b : weapon.bones) if (b.parent == "BONESYSTEM") { rigRoot = &b; break; }
    // A link=1 row of weapon_attach.x7 already names the bone and hangs the
    // piece as authored (mind_alice, Mind Storm: Bip01 R Forearm) - the rule
    // matched its doll's Bip07 to Bip01 L Toe0 and left it on the floor.
    if (rigRoot && weaponTypeId && data::AttachLink(respath::Root(), *weaponTypeId, primaryRow) == 1) rigRoot = nullptr;
    if (rigRoot) {
        const scn::Bone* mount = rigRoot;
        const float* M = mount->matrix;
        // Nearest bip bone to the rig root (static, absolute bind matrices).
        // The rig is authored on the MALE bind (seal_dag: Bip01 R Forearm at
        // (-60.4, 226.6, 8.4)); the female bip is smaller, so take the
        // closest Bip01 bone within 20 units rather than an exact match. Its
        // rotation never matches either (the rig comes from Blender): its own
        // tracks already put the pieces in the bone's frame.
        const scn::Bone* best = nullptr; float bestD = 20.f;
        for (const scn::Bone& cb : character.bones) {
            if (cb.name.rfind("Bip01", 0) != 0 || cb.name == "Bip01" || cb.name == "Bip01 Footsteps") continue;   // floor/root: never a grip
            const float dx = cb.matrix[12] - M[12], dy = cb.matrix[13] - M[13], dz = cb.matrix[14] - M[14];
            const float d = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (d < bestD) { bestD = d; best = &cb; }
        }
        if (best) {
            scn::Mat4 tw;
            if (scn::TrackWorld(weapon, mount->name, tw)) {
                const scn::Mat4 inv = scn::Mat4InvAffine(tw);
                std::memcpy(primary.invMount, inv.data(), sizeof(primary.invMount));
            }
            primary.attackBone = primary.idleBone = best->name;
            DbgLog("[weapon] '%s' root '%s' sits on the bip bone '%s' (%.1f away) - hung from it", primaryFile.c_str(), mount->name.c_str(), best->name.c_str(), bestD);
        }
    }
    if (wornWeapon) {
        weaponWornParts.push_back(weapon);
        // ...and its leg part: ironboots_01_leg.scn carries the shin armour
        // (without it the costume's bare shins showed between the plates).
        const std::string leg = RES("resources/resources/model/character/leg/") + requestedStem + "_leg.scn";
        if (std::filesystem::exists(leg)) {
            scn::Scene l = scn::Parse(leg.c_str());
            if (l.ok && !l.models.empty()) { l.costumeSlot = 3; weaponWornParts.push_back(std::move(l)); }
        }
        weaponWornParts.front().costumeSlot = 5;
    }
    else weaponParts.push_back(std::move(primary));

    // The item lists its pieces: <scene value1="demonic_cat_knuckle_R.scn"
    // value2="demonic_cat_glove_L.scn"> - the second piece is value2, not
    // always the _r name turned into _l. The name rule stays as the fallback.
    std::string leftName = splitWeapon ? leftStem + ".scn" : std::string();
    if (const SceneItem* si = SceneItemOf(requestedFile))
        if (!si->second.empty() && std::filesystem::exists(std::filesystem::path(RES("resources/resources/model/weapon")) / si->second) &&
            !(weaponTypeId && data::AttachLink(respath::Root(), *weaponTypeId, 1) == 2))   // link=2: sigmablade's _change form, shown only when it transforms
            leftName = si->second;
    if (!leftName.empty()) {
        const std::string leftFile = leftName;
        const std::filesystem::path leftPath = std::filesystem::path(RES("resources/resources/model/weapon")) / leftFile;
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
                left.attackBone = weaponTypeId ? data::AttachBone(respath::Root(), *weaponTypeId, 1, false) : "";
                if (left.attackBone.empty()) left.attackBone = "L_Hand_Dummy";
                left.idleBone = weaponTypeId ? data::AttachBone(respath::Root(), *weaponTypeId, 1, true) : "";
                if (left.idleBone.empty()) left.idleBone = left.attackBone;
                // Fixed piece (counter sword fist on Bip01 L Forearm, ...):
                // LoadAttachWeaponModel - hangs as authored, no mount inverse.
                if (weaponTypeId && data::AttachLink(respath::Root(), *weaponTypeId, 1) == 1) {
                    const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                    std::memcpy(left.invMount, id, sizeof(id));
                }
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
        const std::string scriptDir = ScriptDir();
        const std::string shimPath = "third_party/lua_weapon_shim.lua";
        WeaponFireFuncs ff = FindWeaponFireFuncs(scriptDir, weaponStates);
        // The item's rows name its attack functions (weapon=): the counter
        // sword's StandardCounterSwordAfterDash1 / CounterAttack / JumpDash,
        // the sigma skins' StandardSigmaBladeAfterDash1 for magmaBlade...
        // Name guessing above only fills states the item does not list.
        {
            auto rowWeapon = [&](const char* st) -> std::string {
                auto it = weaponRows.find(st);
                return it != weaponRows.end() && ff.fileOf.count(it->second.weapon) ? it->second.weapon : std::string();
            };
            auto take = [](std::string& dst, const std::string& v) { if (!v.empty()) dst = v; };
            std::string w = rowWeapon("left_weak");
            if (w.empty() && !weaponAnim.isGun) w = rowWeapon("left_attack");   // daggers: StandardSpyDaggerAttack1 (a gun's is its shot)
            take(ff.weak, w);
            take(ff.strong, rowWeapon("left_strong1"));
            take(ff.strong1, rowWeapon("left_strong2"));
            take(ff.weapon2, rowWeapon("right_attack"));
            take(ff.jump, rowWeapon("left_jump"));
            if (!ff.weak.empty()) ff.file = ff.FileFor(ff.weak);
        }
        atkSeqWeak.clear(); atkSeqStrong.clear(); atkSeqStrong1.clear(); pendingSeqs.clear();
        ghostTrail = nullptr; ghostSamples.clear();
        atkMoveBoxWeak = atkMoveBoxStrong = atkMoveBoxStrong1 = 1.f;
        atkNormalMsWeak = atkNormalMsStrong = atkNormalMsStrong1 = 0.f;
        atkDelayMsWeak = atkDelayMsStrong = atkDelayMsStrong1 = 0.f;
        bool lastUseMoveBox = false;
        // The weapon's own range (WEAPON:GetRange) and power (the Power argument)
        // from its item, so every CreateCAttack box/ray/fan has its real size.
        luaw::SetWeaponRange(weaponItemRange);
        atkLuaWeak.clear(); atkLuaStrong.clear(); atkLuaStrong1.clear(); weapon2Attacks.clear(); jumpAttacks.clear();
        std::vector<luaw::WeaponAttack>* attacksOut = nullptr;
        auto tryFunc = [&](const std::string& fn, float& outRate, float& outHitStop,
                           std::vector<luaw::WeaponFireState::Sequence>& outSeqs, float& outMoveBox,
                           float& outNormalMs, float& outDelayMs) {
            if (fn.empty()) return;
            luaw::WeaponFireState st;
            std::vector<luaw::WeaponAttack> atks = luaw::RunWeaponFire(scriptDir.c_str(), shimPath.c_str(), ff.FileFor(fn).c_str(), fn.c_str(), 0, weaponItemPower, 0, 0, &st);
            if (attacksOut) *attacksOut = std::move(atks);
            outRate = st.moveSpeedRate; outHitStop = st.hitStopA;
            outSeqs = st.sequences;
            outMoveBox = st.moveBoxMultiplier;
            if (st.loaded && st.initTimed && st.initArgs.size() >= 2) { outNormalMs = st.initArgs[0]; outDelayMs = st.initArgs[1]; }
            lastUseMoveBox = st.useMoveBox;
        };
        atkUseMoveBoxWeak = atkUseMoveBoxStrong = atkUseMoveBoxStrong1 = false;
        attacksOut = &atkLuaWeak;
        lastUseMoveBox = false; tryFunc(ff.weak, atkMoveSpeedRateWeak, atkHitStopWeak, atkSeqWeak, atkMoveBoxWeak, atkNormalMsWeak, atkDelayMsWeak);
        atkUseMoveBoxWeak = lastUseMoveBox;
        attacksOut = &atkLuaStrong;
        lastUseMoveBox = false; tryFunc(ff.strong, atkMoveSpeedRateStrong, atkHitStopStrong, atkSeqStrong, atkMoveBoxStrong, atkNormalMsStrong, atkDelayMsStrong);
        atkUseMoveBoxStrong = lastUseMoveBox;
        attacksOut = &atkLuaStrong1;
        lastUseMoveBox = false; tryFunc(ff.strong1, atkMoveSpeedRateStrong1, atkHitStopStrong1, atkSeqStrong1, atkMoveBoxStrong1, atkNormalMsStrong1, atkDelayMsStrong1);
        atkUseMoveBoxStrong1 = lastUseMoveBox;
        DbgLog("[weapon-lua] swing moveSpeedRate weak=%.2f strong=%.2f useMoveBox weak=%d strong=%d",
               atkMoveSpeedRateWeak, atkMoveSpeedRateStrong, (int)atkUseMoveBoxWeak, (int)atkUseMoveBoxStrong);
        DbgLog("[weapon-lua] swing timing weak=%.0f+%.0f strong=%.0f+%.0f strong1=%.0f+%.0f ms",
               atkNormalMsWeak, atkDelayMsWeak, atkNormalMsStrong, atkDelayMsStrong, atkNormalMsStrong1, atkDelayMsStrong1);
        // Combo hits 2..5 (left_attack2..5 weapon=: counter sword AfterDash2..4)
        // each run their own function.
        atkLuaCombo.clear();
        for (const char* stName : { "left_attack2", "left_attack3", "left_attack4", "left_attack5" }) {
            auto it = weaponRows.find(stName);
            if (it == weaponRows.end() || !ff.fileOf.count(it->second.weapon)) break;
            atkLuaCombo.push_back(luaw::RunWeaponFire(scriptDir.c_str(), shimPath.c_str(), ff.FileFor(it->second.weapon).c_str(),
                                                      it->second.weapon.c_str(), 0, weaponItemPower, 0, 0, nullptr));
        }
        if (!ff.weapon2.empty()) {
            weapon2Attacks = luaw::RunWeaponFire(scriptDir.c_str(), shimPath.c_str(), ff.FileFor(ff.weapon2).c_str(), ff.weapon2.c_str(), 0, weaponItemPower, 0, 0, &weapon2Lua);
            DbgLog("[weapon-lua] weapon2 file='%s' fn='%s' loaded=%d moveBoxMultiplier=%.2f initArgs=%zu",
                   ff.file.c_str(), ff.weapon2.c_str(), (int)weapon2Lua.loaded,
                   weapon2Lua.moveBoxMultiplier, weapon2Lua.initArgs.size());
        }
        if (!ff.jump.empty()) {
            jumpAttacks = luaw::RunWeaponFire(scriptDir.c_str(), shimPath.c_str(), ff.FileFor(ff.jump).c_str(), ff.jump.c_str(), 0, weaponItemPower, 0, 0, &jumpAttackLua);
            DbgLog("[weapon-lua] jump file='%s' fn='%s' loaded=%d initArgs=%zu dropAfterMs=%.0f drop=(%.0f,%.0f,%.0f)",
                   ff.file.c_str(), ff.jump.c_str(), (int)jumpAttackLua.loaded, jumpAttackLua.initArgs.size(),
                   jumpAttackLua.dropTime, jumpAttackLua.dropX, jumpAttackLua.dropY, jumpAttackLua.dropZ);
        }
        for (const auto* v : { &atkSeqWeak, &atkSeqStrong, &atkSeqStrong1, &weapon2Lua.sequences, &jumpAttackLua.sequences })
            for (const auto& sq : *v)
                DbgLog("[weapon-lua] AddSequence '%s' sound='%s' start=%.0fms end=%.0fms",
                       sq.file.c_str(), sq.sound.c_str(), sq.startMs, sq.endMs);
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

// Nearest hit of a ray against the map's collision triangles (Moller-Trumbore).
static bool RayCollTris(const std::vector<CollTri>& tris, const float o[3], const float d[3], float maxT, float& outT,
                        float* outNormal = nullptr) {
    float best = maxT; bool hit = false;
    for (const CollTri& t : tris) {
        // quick reject: the ray's box against the triangle's box
        float lo[3], hi[3];
        for (int k = 0; k < 3; ++k) { const float e = o[k] + d[k] * maxT; lo[k] = (std::min)(o[k], e); hi[k] = (std::max)(o[k], e); }
        if (hi[0] < t.bmin[0] || lo[0] > t.bmax[0] || hi[1] < t.bmin[1] || lo[1] > t.bmax[1] || hi[2] < t.bmin[2] || lo[2] > t.bmax[2]) continue;
        const float e1[3] = { t.v1[0]-t.v0[0], t.v1[1]-t.v0[1], t.v1[2]-t.v0[2] };
        const float e2[3] = { t.v2[0]-t.v0[0], t.v2[1]-t.v0[1], t.v2[2]-t.v0[2] };
        const float pv[3] = { d[1]*e2[2]-d[2]*e2[1], d[2]*e2[0]-d[0]*e2[2], d[0]*e2[1]-d[1]*e2[0] };
        const float det = e1[0]*pv[0] + e1[1]*pv[1] + e1[2]*pv[2];
        if (std::fabs(det) < 1e-8f) continue;
        const float inv = 1.f / det;
        const float tv[3] = { o[0]-t.v0[0], o[1]-t.v0[1], o[2]-t.v0[2] };
        const float u = (tv[0]*pv[0] + tv[1]*pv[1] + tv[2]*pv[2]) * inv;
        if (u < 0.f || u > 1.f) continue;
        const float qv[3] = { tv[1]*e1[2]-tv[2]*e1[1], tv[2]*e1[0]-tv[0]*e1[2], tv[0]*e1[1]-tv[1]*e1[0] };
        const float v = (d[0]*qv[0] + d[1]*qv[1] + d[2]*qv[2]) * inv;
        if (v < 0.f || u + v > 1.f) continue;
        const float tt = (e2[0]*qv[0] + e2[1]*qv[1] + e2[2]*qv[2]) * inv;
        if (tt > 1e-3f && tt < best) {
            best = tt; hit = true;
            if (outNormal) {
                float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
                const float nl = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
                if (nl > 1e-6f) for (int k = 0; k < 3; ++k) outNormal[k] = n[k] / nl;
            }
        }
    }
    outT = best;
    return hit;
}

// One gun shot: blast sound + the weapon's gun_blast .seq on its fire node,
// then the bullet down the crosshair (camera centre, same eye as OnRender) up
// to the item's range; on the map it plays the weapon's wall-hit .seq/sound
// (SetDamageSequence args 2 and 4: gun_hit.seq, wall_attack.ogg).
void WorldState::HitPeersInCone(float range, float dmg, float knock) {
    if (!mpOnline || !mpClient.Connected() || mapIdx < 0 || mapIdx >= (int)maps.size()) {
        if (mpOnline) DbgLog("[mp] melee: not connected (online=%d connected=%d)", (int)mpOnline, (int)mpClient.Connected());
        return;
    }
    DbgLog("[mp] melee swing: %zu peers known, my map '%s'", mpClient.Peers().size(), maps[mapIdx].name.c_str());
    const float fx = std::sin(charYaw), fz = std::cos(charYaw);
    for (auto& kv : mpClient.Peers()) {
        const mp::Peer& p = kv.second;
        if (!p.hasPos || p.map != maps[mapIdx].name) continue;
        const float dx = p.rx - charPos[0], dz = p.rz - charPos[2], dy = p.ry - charPos[1];
        const float d = std::sqrt(dx*dx + dz*dz);
        if (d > range || std::fabs(dy) > 220.f) {
            DbgLog("[mp]   peer %d '%s' map='%s' out of reach (dist=%.0f dy=%.0f)", p.id, p.nickname.c_str(), p.map.c_str(), d, dy);
            continue;
        }
        if (d > 60.f && (dx*fx + dz*fz) / d < 0.3f) continue;   // within ~70 degrees of where we face
        mpClient.SendHit(p.id, dmg, knock);
        DbgLog("[mp] melee hit -> %d dmg=%.0f dist=%.0f", p.id, dmg, d);
    }
}

// One CreateCAttack shape against a point, in the attacker's space: the
// shape's center/dir are actor-local (y up, -z forward: MAKEVECTOR3(0,0,-1)),
// placed by the actor's position and facing like the body (charYaw + pi).
// The target is a body: its point is tested with a 40-unit radius.
bool WorldState::AttackShapeHits(const luaw::AttackShape& sh, float px, float py, float pz) const {
    const float a = charYaw + 3.14159265f, c = std::cos(a), s = std::sin(a);
    const float wx = px - charPos[0], wy = py - charPos[1], wz = pz - charPos[2];
    // world = local * R (BuildYawPitchTranslate rows) -> local = world * R^T
    const float lx = wx * c - wz * s, ly = wy, lz = wx * s + wz * c;
    const float R = 40.f;
    const float vx = lx - sh.centerX, vy = ly - sh.centerY, vz = lz - sh.centerZ;
    float ux = sh.dirX, uy = sh.dirY, uz = sh.dirZ;
    const float ul = std::sqrt(ux*ux + uy*uy + uz*uz);
    if (ul > 1e-6f) { ux /= ul; uy /= ul; uz /= ul; } else { ux = 0.f; uy = 0.f; uz = -1.f; }
    const std::string& k = sh.kind;
    if (k == "sphere") return std::sqrt(vx*vx + vy*vy + vz*vz) <= sh.radius + R;
    if (k == "ray" || k == "thickray") {
        const float t = (std::max)(0.f, (std::min)(sh.length, vx*ux + vy*uy + vz*uz));
        const float dx = vx - ux*t, dy = vy - uy*t, dz = vz - uz*t;
        return std::sqrt(dx*dx + dy*dy + dz*dz) <= R + (k == "thickray" ? sh.radius : 0.f);
    }
    if (k == "box" || k == "pole") {
        // length along dir from the center, width across, height up.
        const float along = vx*ux + vy*uy + vz*uz;
        if (along < -R || along > sh.length + R) return false;
        float rx = uz, rz = -ux; const float rl = std::sqrt(rx*rx + rz*rz);
        if (rl > 1e-6f) { rx /= rl; rz /= rl; } else { rx = 1.f; rz = 0.f; }
        if (std::fabs(vx*rx + vz*rz) > sh.width * 0.5f + R) return false;
        return std::fabs(vy) <= sh.height * 0.5f + R;
    }
    if (k == "fan" || k == "cone") {
        // A slice on the ground: within length (cone: radius) and +-angle/2 of dir.
        const float reach = k == "cone" ? sh.radius : sh.length;
        const float hd = std::sqrt(vx*vx + vz*vz);
        if (hd > reach + R) return false;
        if (k == "fan" && std::fabs(vy) > sh.height * 0.5f + R) return false;
        if (hd <= R) return true;
        const float hl = std::sqrt(ux*ux + uz*uz);
        if (hl < 1e-6f) return true;
        const float cosA = (vx*ux + vz*uz) / (hd * hl);
        return cosA >= std::cos(sh.angle * 0.5f * 3.14159265f / 180.f);
    }
    if (k == "cylinder") {
        const float hd = std::sqrt(vx*vx + vz*vz);
        return hd <= sh.radius + R && vy >= -R && vy <= sh.length + R;
    }
    return false;
}

// Everything an actor-state function declares for its state, as the client
// plays it on entering: AddSound / AddSoundBySex (the female voice for a female
// character) at their start, every AddSequence (BySex: the female version) at
// its start time, AddInstantEffect on its node of the worn item, the trail.
void WorldState::PlayStateFx(const anim::ActorFunc* fn, unsigned tnow) {
    if (!fn) { StartGhostTrail(nullptr); return; }
    for (const anim::ActorSound& s : fn->sounds)
        if (!s.man.empty()) audio::PlayEffect(female && !s.girl.empty() ? s.girl : s.man, 0.85f, (unsigned)(std::max)(0, s.start));
    if (anim::ShapeChange())   // AddSoundTransBySex: the transformed sigma's sounds
        for (const anim::ActorSound& s : fn->transSounds)
            if (!s.man.empty()) audio::PlayEffect(female && !s.girl.empty() ? s.girl : s.man, 0.85f, (unsigned)(std::max)(0, s.start));
    for (const anim::ActorSeq& q : fn->seqs)
        pendingSeqs.push_back({ tnow + (unsigned)(std::max)(0, q.startMs), (female && !q.seqGirl.empty()) ? q.seqGirl : q.seq, q.sound, q.index });
    for (const anim::ActorInstantEffect& e : fn->instant) {
        PendingSeq ps{ tnow + (unsigned)(std::max)(0, e.startMs), e.seq, e.sound };
        ps.node = e.node;
        pendingSeqs.push_back(ps);
    }
    const anim::ActorGhostTrail* g = nullptr;
    for (const anim::ActorGhostTrail& t : fn->ghosts) if (!t.name.empty()) { g = &t; break; }
    StartGhostTrail(g);
}

// A hit's reaction takes the body: whatever the actor was doing is cut.
void WorldState::StartReaction(int kind, unsigned tnow, unsigned durMs) {
    reactKind = kind; reactPhase = 0; reactStart = reactPhaseStart = tnow;
    reactEnd = tnow + durMs; reactLeftGround = false;
    if (kind != REACT_FLINCH) {
        atkParam = nullptr; weapon2Active = false; swingAttacks = nullptr;
        ghostTrail = nullptr; ghostSamples.clear(); pendingSeqs.clear();
        gunFiring = false;
    }
}

void WorldState::StartSwingAttacks(const std::vector<luaw::WeaponAttack>* atks, unsigned tnow, float baseDmg) {
    swingAttacks = (atks && !atks->empty()) ? atks : nullptr;
    swingStart = tnow;
    swingFired.assign(swingAttacks ? swingAttacks->size() : 0, 0);
    swingBaseDmg = baseDmg;
}

// Fires every attack of the swing at its own fireTime. Attacks sharing a
// fireTime are one hit per target: a critical one (SetAlwaysCritical) wins
// where it lands - and only where the attack it DependOn lands too - else
// the normal one. The target's reaction comes from the conditions of every
// attack of that moment that reached it (AddStunCondition time by distance,
// AddBlowCondition, AddPushCondition).
void WorldState::ProcessSwingAttacks(unsigned tnow) {
    if (!swingAttacks) return;
    const std::vector<luaw::WeaponAttack>& atks = *swingAttacks;
    const float since = float(tnow - swingStart);
    for (size_t i = 0; i < atks.size(); ++i) {
        if (swingFired[i] || since < atks[i].fireTime) continue;
        std::vector<size_t> group;
        for (size_t j = 0; j < atks.size(); ++j)
            if (!swingFired[j] && atks[j].fireTime == atks[i].fireTime) { group.push_back(j); swingFired[j] = 1; }
        // Targets: the other players on this map, and the clone (C key).
        struct Target { int id; float x, y, z; };
        std::vector<Target> targets;
        if (mpOnline && mpClient.Connected() && mapIdx >= 0 && mapIdx < (int)maps.size())
            for (auto& kv : mpClient.Peers()) {
                const mp::Peer& pp = kv.second;
                if (pp.hasPos && pp.map == maps[mapIdx].name) targets.push_back({ pp.id, pp.rx, pp.ry, pp.rz });
            }
        for (size_t ci = 0; ci < clones.size(); ++ci) targets.push_back({ -1 - (int)ci, clones[ci].pos[0], clones[ci].pos[1] + clones[ci].lift, clones[ci].pos[2] });
        bool upperCutLanded = false; float upperDist = 0.f, upperPower = 0.f;
        for (const Target& p : targets) {
            auto shapeHits = [&](const luaw::WeaponAttack& a) {
                for (float h : { 30.f, 100.f, 170.f }) if (AttackShapeHits(a.shape, p.x, p.y + h, p.z)) return true;
                return false;
            };
            const luaw::WeaponAttack* crit = nullptr; const luaw::WeaponAttack* normal = nullptr;
            float stunMs = 0.f, blowDist = 0.f, blowPower = 0.f, knock = 0.f, pushMs = 0.f;
            bool shake = false; std::vector<float> shakeF1;
            const float dx = p.x - charPos[0], dz = p.z - charPos[2];
            const float dist = std::sqrt(dx*dx + dz*dz);
            for (size_t j : group) {
                const luaw::WeaponAttack& a = atks[j];
                if (!shapeHits(a)) continue;
                if (a.dependOn >= 0 && a.dependOn < (int)atks.size() && !shapeHits(atks[a.dependOn])) continue;
                if (a.alwaysCritical) { if (!crit) crit = &a; } else if (!normal) normal = &a;
                for (size_t q = 0; q + 3 < a.stun.size(); q += 4) {
                    // (minMs, maxMs, minDist, maxDist): the closer, the longer.
                    const float minT = a.stun[q], maxT = a.stun[q+1], minD = a.stun[q+2], maxD = a.stun[q+3];
                    const float f = maxD > minD ? (std::max)(0.f, (std::min)(1.f, (dist - minD) / (maxD - minD))) : 0.f;
                    stunMs = (std::max)(stunMs, maxT - (maxT - minT) * f);
                }
                // (dist, power) = launch speed away from the attacker (negative: towards
                // it) and upwards (S10 CBlowCondition::Apply 0x00FD5890).
                for (size_t q = 0; q + 1 < a.blow.size(); q += 2) if (a.blow[q+1] > blowPower || (blowPower == 0.f && a.blow[q] != 0.f)) { blowDist = a.blow[q]; blowPower = a.blow[q+1]; }
                for (size_t q = 0; q + 1 < a.upperCut.size(); q += 2) { upperCutLanded = true; upperDist = a.upperCut[q]; upperPower = (std::max)(upperPower, a.upperCut[q+1]); }
                for (size_t q = 0; q + 2 < a.push.size(); q += 3) if (a.push[q+1] > knock) { knock = a.push[q+1]; pushMs = a.push[q]; }
                if (a.shakeEnable && a.shakeWhenSuccess && !shake) { shake = true; shakeF1 = a.shakeF1; }
            }
            if (!crit && !normal) continue;
            // Damage: the landing attack's own power from its InitAttack1 (the
            // script's formula on the item's power: plasma weak fan 4.5, its
            // critical box 3.4, each dash hit 1.7, counter sword heavy 22.3).
            const luaw::WeaponAttack* landed = crit ? crit : normal;
            const float dmg = landed->power > 0.f ? landed->power : swingBaseDmg;
            if (p.id < 0) CloneHit(clones[-1 - p.id], dmg, stunMs, blowDist, blowPower, tnow);
            else mpClient.SendHit(p.id, dmg, knock, stunMs, blowDist, blowPower, crit != nullptr, pushMs);
            if (shake) StartShake(shakeF1, tnow);
            DbgLog("[mp] attack t=%.0f hit %d dist=%.0f crit=%d stun=%.0f blow=%.0f/%.0f push=%.0f shape=%s",
                   atks[i].fireTime, p.id, dist, (int)(crit != nullptr), stunMs, blowDist, blowPower, knock, (crit ? crit : normal)->shape.kind.c_str());
        }
        // AddUpperCutCondition (S10 CUpperCutCondition::Apply 0x00FDD1F0): a
        // landed hit sends the attacker into STATE_UPPERCUT with speed
        // (facing * dist, power) - the Death Scythe goes up with its target.
        if (upperCutLanded && upperPower > 0.f) {
            StartReaction(REACT_UPPERCUT, tnow, 0);
            reactVX = std::sin(charYaw) * upperDist; reactVZ = std::cos(charYaw) * upperDist;
            velY = upperPower; grounded = false;
            DbgLog("[weapon] uppercut: attacker rises at %.0f (forward %.0f)", upperPower, upperDist);
            return;   // the attack state is over (StartReaction dropped its attacks)
        }
    }
}

// SetCameraShakeSetFactor1(a, b, c): the third is the size of the shake
// (plasma strong 20, dash 50); it decays over 300 ms. (Exact S10 shake curve
// not reversed yet - amplitude from the script, length assumed.)
void WorldState::StartShake(const std::vector<float>& f1, unsigned tnow) {
    const float amp = f1.size() >= 3 ? f1[2] : (f1.empty() ? 20.f : f1.back());
    (void)tnow;   // the render clock: the shake is drawn by OnRender
    shakeStart = GetTickCount(); shakeEnd = shakeStart + 300; shakeAmp = (std::max)(0.f, amp);
}

bool WorldState::PeerAlongRay(const float org[3], const float dir[3], float maxT, int& peerId, float& tHit) {
    if (!mpOnline || !mpClient.Connected() || mapIdx < 0 || mapIdx >= (int)maps.size()) return false;
    bool found = false; tHit = maxT;
    for (auto& kv : mpClient.Peers()) {
        const mp::Peer& p = kv.second;
        if (!p.hasPos || p.map != maps[mapIdx].name) continue;
        // Body = three spheres along the character's height (feet at ry).
        static const float kH[3] = { 60.f, 130.f, 200.f };
        for (float h : kH) {
            const float c[3] = { p.rx - org[0], p.ry + h - org[1], p.rz - org[2] };
            const float tc = c[0]*dir[0] + c[1]*dir[1] + c[2]*dir[2];
            if (tc < 0.f || tc > tHit) continue;
            const float d2 = c[0]*c[0] + c[1]*c[1] + c[2]*c[2] - tc*tc;
            if (d2 > 75.f * 75.f) continue;
            // Entry point on that sphere, not its centre.
            const float tin = tc - std::sqrt((std::max)(0.f, 75.f * 75.f - d2));
            if ((std::max)(0.f, tin) < tHit) { tHit = (std::max)(0.f, tin); peerId = p.id; found = true; }
        }
    }
    return found;
}

void WorldState::FireGunShot(unsigned tnow) {
    if (!weaponAnim.gunBlastSound.empty()) audio::PlayEffect(weaponAnim.gunBlastSound, 0.8f);
    const float viewYaw = camYaw + lookYaw;
    const float viewPitch = (std::max)(kPitchUp, (std::min)(kPitchDown, camPitch + lookPitch));
    const float right[3] = { std::cos(viewYaw), 0.f, -std::sin(viewYaw) };
    const float fh[3] = { std::sin(viewYaw), 0.f, std::cos(viewYaw) };
    float mz[3] = { gunMuzzle[0], gunMuzzle[1], gunMuzzle[2] };
    if (!gunMuzzleValid) {
        const float side = camSideCur >= 0.f ? gunSide0 : gunSide1;
        mz[0] = charPos[0] + right[0]*side + fh[0]*gunFront;
        mz[1] = charPos[1] + (gunHasOrigin ? gunHeight : 260.f);
        mz[2] = charPos[2] + right[2]*side + fh[2]*gunFront;
    }
    // Gun effects stay where they are spawned (muzzle / impact): the default
    // followActor re-places every effect at the actor's feet each frame.
    auto spawnFixed = [this](const std::string& seqFile, float x, float y, float z, float yaw,
                             float scale = 1.f, float rate = 1.f, float pitch = 0.f) {
        const size_t before = activeEffects.size();
        SpawnSeq(seqFile, x, y, z, yaw, {}, scale, rate, pitch);
        if (activeEffects.size() > before) activeEffects.back().followActor = false;
    };
    if (!weaponAnim.gunBlastSeq.empty()) spawnFixed(weaponAnim.gunBlastSeq, mz[0], mz[1], mz[2], viewYaw + 3.14159265f, 1.f, 1.f, -viewPitch);   // forward = local -Z; viewPitch > 0 looks down

    const float cp = std::cos(viewPitch);
    float fwd[3] = { std::sin(viewYaw) * cp, -std::sin(viewPitch), std::cos(viewYaw) * cp };
    const float heightOff = kCamHeightOffset, sideOff = kCamSideOffset * camSideCur;   // same framing as OnRender
    const float at[3] = { charPos[0] + right[0]*sideOff, charPos[1] + heightOff, charPos[2] + right[2]*sideOff };
    const float eyeDist = camDistEff > 0.f ? (std::min)(camDistEff, camDist) : camDist;
    const float eye[3] = { at[0] - fwd[0]*eyeDist, at[1] - fwd[1]*eyeDist, at[2] - fwd[2]*eyeDist };
    // What the crosshair points at (camera ray), then the client's shot: it
    // leaves the actor at InitLua's height/side/front offsets and flies to
    // that point, up to RayDistanceLimit (25000, FUN_012bfd40).
    const float kRayLimit = 25000.f;
    // The shot ray goes through the crosshair pixel (HudLayout: the screen
    // centre, as the client's full-screen crosshair_GroupBox).
    {
        const HudLayout& hud = GetHudLayout();
        const float ndcX = (hud.xhairCx - 512.f) / 512.f, ndcY = (384.f - hud.xhairCy) / 384.f;
        const float tv = std::tan(curFov * 0.5f * 3.14159265f / 180.f);
        const float aspect = scrH > 0 ? (float)scrW / (float)scrH : 4.f / 3.f;
        const float upv[3] = { fwd[1]*right[2] - fwd[2]*right[1], fwd[2]*right[0] - fwd[0]*right[2], fwd[0]*right[1] - fwd[1]*right[0] };
        float d[3];
        for (int k = 0; k < 3; ++k) d[k] = fwd[k] + upv[k] * ndcY * tv + right[k] * ndcX * tv * aspect;
        const float dl0 = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        for (int k = 0; k < 3; ++k) fwd[k] = d[k] / dl0;
    }
    float t = 0.f;
    float target[3];
    if (RayCollTris(collTris, eye, fwd, eyeDist + kRayLimit, t) && t >= eyeDist * 0.5f)
        for (int k = 0; k < 3; ++k) target[k] = eye[k] + fwd[k] * t;
    else
        for (int k = 0; k < 3; ++k) target[k] = eye[k] + fwd[k] * (eyeDist + kRayLimit);
    float org[3] = { mz[0], mz[1], mz[2] };
    DbgLog("[gun] shot %u from %s (%.0f,%.0f,%.0f) actor (%.0f,%.0f,%.0f) node='%s'", gunShotIndex,
           gunMuzzleValid ? "fire node" : "InitLua offsets", org[0], org[1], org[2], charPos[0], charPos[1], charPos[2], weaponAnim.gunFireBone.c_str());
    float dir[3] = { target[0]-org[0], target[1]-org[1], target[2]-org[2] };
    const float dl = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (dl < 1.f) return;
    for (float& c : dir) c /= dl;
    (void)tnow;
    // Every ray of the shot, turned off the aim line by its local x/y (and
    // this shot's pattern offset): a bullet / beam and a wall hit each.
    float ar[3] = { dir[2], 0.f, -dir[0] };
    { const float l = std::sqrt(ar[0]*ar[0] + ar[2]*ar[2]); if (l > 1e-5f) { ar[0] /= l; ar[2] /= l; } else { ar[0] = 1.f; ar[2] = 0.f; } }
    const float au[3] = { dir[1]*ar[2] - dir[2]*ar[1], dir[2]*ar[0] - dir[0]*ar[2], dir[0]*ar[1] - dir[1]*ar[0] };
    // One group of rays per shot: all of them when they share a fireTime,
    // else the group this shot lands on in the sequence.
    std::vector<GunRay> rays;
    for (const GunRay& g : gunRays)
        if (gunRayGroups <= 1 || g.group == (gunForceGroup >= 0 ? gunForceGroup : (int)(gunShotIndex % (unsigned)gunRayGroups))) rays.push_back(g);
    if (rays.empty()) rays.push_back(GunRay{});
    bool anyWall = false;
    mp::Shot netShot;   // what the other players draw of this shot
    netShot.o[0] = mz[0]; netShot.o[1] = mz[1]; netShot.o[2] = mz[2];
    netShot.yaw = viewYaw + 3.14159265f; netShot.pitch = -viewPitch;
    netShot.blast = weaponAnim.gunBlastSeq; netShot.blastSound = weaponAnim.gunBlastSound; netShot.tracer = weaponAnim.gunTracerSeq;
    netShot.wall = weaponAnim.gunWallSeq; netShot.wallSound = weaponAnim.gunWallSound;
    netShot.hit = weaponAnim.gunHitSeq; netShot.hitSound = weaponAnim.gunHitSound;
    for (const GunRay& gr : rays) {
        float lx = gr.x, ly = gr.y;
        if (gr.off.size() >= 2) {
            const size_t k = (gunShotIndex % (gr.off.size() / 2)) * 2;
            lx += gr.off[k]; ly += gr.off[k + 1];
        }
        float d[3] = { dir[0] - ar[0]*lx + au[0]*ly, dir[1] - ar[1]*lx + au[1]*ly, dir[2] - ar[2]*lx + au[2]*ly };
        const float l = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        for (float& c : d) c /= l;
        float wallN[3] = { 0.f, 0.f, 0.f };
        const bool onMap = RayCollTris(collTris, org, d, kRayLimit, t, wallN);
        if (!onMap) t = kRayLimit;
        // Another player in the way stops the bullet there: the attribute's
        // player-hit sequence/sound (SetDamageSequence args 1/3) and the damage.
        int peerId = 0; float peerT = t;
        const bool onPeer = PeerAlongRay(org, d, t, peerId, peerT);
        if (onPeer) t = peerT;
        const float hit[3] = { org[0] + d[0]*t, org[1] + d[1]*t, org[2] + d[2]*t };
        if (!gunBeamNodes.empty()) gunBeams.push_back({ { hit[0], hit[1], hit[2] }, (unsigned)GetTickCount() });   // OnRender's clock
        // No tracer .seq on the attribute (revolver, handgun, shotguns...):
        // separated bullets flying from the muzzle (see OnRender).
        else gunTracers.push_back({ { mz[0], mz[1], mz[2] }, { hit[0], hit[1], hit[2] }, (unsigned)GetTickCount() });
        netShot.ends.push_back({ hit[0], hit[1], hit[2], onPeer ? 2 : (onMap ? 1 : 0) });
        if (onPeer) {
            const float dmg = 10.f / (float)rays.size() * (rays.size() > 1 ? 2.f : 1.f);
            // AddPushCondition (ms, speed, accel) of the ray: the target is pushed.
            mpClient.SendHit(peerId, dmg, gr.pushSpeed, 0.f, 0.f, 0.f, false, gr.pushMs);
            DbgLog("[mp] bullet hit -> %d dmg=%.1f at %.0f", peerId, dmg, t);
            if (!weaponAnim.gunHitSeq.empty()) spawnFixed(weaponAnim.gunHitSeq, hit[0], hit[1], hit[2], viewYaw);
            if (!weaponAnim.gunHitSound.empty()) audio::PlayEffect(weaponAnim.gunHitSound, 0.7f);
            continue;
        }
        if (!onMap) continue;
        anyWall = true;
        // Point-blank on a wall, shooting it head-on: the attack's push lands
        // on the shooter (a wall beside you, hit at a glancing angle, does not).
        const float kPointBlank = 150.f;     // estimate: "pegado a la pared"
        const float kHeadOnCos = 0.8f;       // estimate: within ~37 degrees of the wall's normal
        if (t <= kPointBlank && gr.pushSpeed > 0.f && gr.pushMs > 0.f) {
            float px = -d[0], pz = -d[2]; const float pl = std::sqrt(px*px + pz*pz);
            float nx = wallN[0], nz = wallN[2]; const float nl = std::sqrt(nx*nx + nz*nz);
            if (pl > 1e-4f && nl > 1e-4f) {
                px /= pl; pz /= pl; nx /= nl; nz /= nl;
                if (std::fabs(px*nx + pz*nz) >= kHeadOnCos)   // normal may face either way
                    selfPushes.push_back({ px, pz, gr.pushSpeed, gr.pushAccel, gr.pushMs });
            }
        }
        // Just in front of the wall, back along the shot: at the surface
        // itself half of every flash quad sat behind the wall (cut off).
        const float kWallLift = 40.f;
        if (!weaponAnim.gunWallSeq.empty())
            spawnFixed(weaponAnim.gunWallSeq, hit[0] - d[0]*kWallLift, hit[1] - d[1]*kWallLift, hit[2] - d[2]*kWallLift, viewYaw);
    }
    ++gunShotIndex;
    if (anyWall && !weaponAnim.gunWallSound.empty()) audio::PlayEffect(weaponAnim.gunWallSound, 0.6f);
    if (mpOnline && mpClient.Connected()) mpClient.SendShot(netShot);
}

const std::pair<std::vector<WorldState::BeamNode>, uint32_t>& WorldState::BeamNodesOf(const std::string& seqPath) {
    auto it = beamSeqCache.find(seqPath);
    if (it != beamSeqCache.end()) return it->second;
    std::pair<std::vector<BeamNode>, uint32_t> v{ {}, 0u };
    if (!seqPath.empty()) {
        std::string p = seqPath; std::replace(p.begin(), p.end(), '\\', '/');
        std::string low = p; for (char& c : low) c = (char)std::tolower((unsigned char)c);
        p = (low.rfind("resources/", 0) == 0 ? RES("resources/") : RES("resources/resources/")) + p;
        const seq::Sequence ts = seq::Parse(p.c_str());
        for (const seq::Node& n : ts.nodes) if (n.isLightning) v.first.push_back({ n.beam, n.beginTime, n.endTime });
        v.second = ts.duration;
    }
    return beamSeqCache.emplace(seqPath, std::move(v)).first->second;
}

// Another player's shot, drawn with that weapon's own effects at the points
// that player sent: muzzle blast, bullet / beam to each end, wall or player hit.
void WorldState::PlayPeerShot(const mp::Shot& s) {
    auto spawnFixed = [this](const std::string& seqFile, float x, float y, float z, float yaw, float pitch = 0.f) {
        const size_t before = activeEffects.size();
        SpawnSeq(seqFile, x, y, z, yaw, {}, 1.f, 1.f, pitch);
        if (activeEffects.size() > before) activeEffects.back().followActor = false;
    };
    if (!s.blast.empty()) spawnFixed(s.blast, s.o[0], s.o[1], s.o[2], s.yaw, s.pitch);
    if (!s.blastSound.empty()) audio::PlayEffect(s.blastSound, 0.6f);
    const bool beam = !BeamNodesOf(s.tracer).first.empty();
    bool anyWall = false, anyPlayer = false;
    for (const mp::Shot::End& e : s.ends) {
        if (beam) peerBeams.push_back({ { s.o[0], s.o[1], s.o[2] }, { e.x, e.y, e.z }, (unsigned)GetTickCount(), s.tracer });
        else gunTracers.push_back({ { s.o[0], s.o[1], s.o[2] }, { e.x, e.y, e.z }, (unsigned)GetTickCount() });
        float d[3] = { e.x - s.o[0], e.y - s.o[1], e.z - s.o[2] };
        const float l = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (l > 1e-3f) for (float& c : d) c /= l;
        const float yaw = s.yaw - 3.14159265f;
        if (e.kind == 1 && !s.wall.empty()) { spawnFixed(s.wall, e.x - d[0]*40.f, e.y - d[1]*40.f, e.z - d[2]*40.f, yaw); anyWall = true; }
        if (e.kind == 2 && !s.hit.empty()) { spawnFixed(s.hit, e.x, e.y, e.z, yaw); anyPlayer = true; }
    }
    if (anyWall && !s.wallSound.empty()) audio::PlayEffect(s.wallSound, 0.5f);
    if (anyPlayer && !s.hitSound.empty()) audio::PlayEffect(s.hitSound, 0.6f);
}

// Arma un catalogo: cada NormalState_<X> del lua emparejado con un .scn de
// model/weapon. Mismo criterio que el cliente JS: se parte el sufijo en tokens,
// se descartan los genericos (sword/gun/rifle/...) y se busca un archivo cuyo
// nombre contenga TODOS los tokens; si no, alguno.
void WorldState::BuildWeaponList() {
    weaponList.clear();
    std::vector<std::string> models;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(RESC("resources/resources/model/weapon/*.scn"), &fd);
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
        // Base Counter Sword. The token search picked 114_counter_sword_*
        // (the Mars set skin), whose right blade is centred on its Cs_Wea_
        // Dummy instead of gripped at one end - the blade sat out of the hand.
        { "CounterSword",        "counter_sword_l.scn" },
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

// Skeleton (<sex>_bip.scn: bones + every clip) plus the default 00_<sex>_*
// body meshes skinned to it. F7 swaps between them at runtime.
#define kCharBase RESC("resources/resources/model/character/")

// .scn for one costume slot: the wardrobe choice or the default 00_<sex>_<slot>.
static std::string CostumeFile(bool asFemale, int slot) {
    const std::string sex = asFemale ? "female" : "male";
    auto it = gCostume.part.find(sex + "_" + kCostumeSlots[slot]);
    if (it != gCostume.part.end() && !it->second.empty()) return it->second;
    return slot >= 6 ? std::string() : "00_" + sex + "_" + kCostumeSlots[slot] + ".scn";   // no accessory by default
}

// The skinned parts (body/leg/hand/foot/face/hair) plus the accessory's
// node models. A file that fails to load falls back to the default part so
// the character never loses a slot. '|' separates the files of one item.
static std::vector<scn::Scene> LoadCostumeParts(bool asFemale, const std::string (&files)[8]) {
    const std::string sex = asFemale ? "female" : "male";
    std::vector<scn::Scene> parts;
    for (int i = 0; i < 8; ++i) {
        const std::string dir = std::string(kCharBase) + kCostumeSlots[i] + "/";
        size_t a = 0;
        while (a <= files[i].size()) {
            size_t b = files[i].find('|', a); if (b == std::string::npos) b = files[i].size();
            std::string f = files[i].substr(a, b - a);
            a = b + 1;
            std::string bone;
            if (const size_t at = f.find('@'); at != std::string::npos) { bone = f.substr(at + 1); f.resize(at); }
            bool animPart = false;
            if (!f.empty() && f.back() == '*') { animPart = true; f.pop_back(); }
            if (f.empty()) continue;
            scn::Scene s = scn::Parse((dir + f).c_str());
            s.attachBone = bone;
            s.ownSkeleton = (i == 7);   // pet
            s.animPart = animPart;
            s.costumeSlot = i;
            if (!s.ok && i < 6) {
                const std::string def = "00_" + sex + "_" + kCostumeSlots[i] + ".scn";
                if (f != def) { DbgLog("[char] %s failed - default %s", f.c_str(), def.c_str()); s = scn::Parse((dir + def).c_str()); }
            }
            s.costumeSlot = i;   // also on the default part loaded above
            if (s.ok) parts.push_back(std::move(s));
            else DbgLog("[char] %s%s failed to load", dir.c_str(), f.c_str());
        }
    }
    return parts;
}

static scn::Scene LoadCharacterSkeleton(bool asFemale) {
    const char* cbase = kCharBase;
    const std::string sex = asFemale ? "female" : "male";
    scn::Scene skel = scn::Parse((std::string(cbase) + sex + "_bip.scn").c_str());
    if (!skel.ok) return skel;
    // S10 splits the animation set: bip_<sex>/<sex>_bip_0000.scn is a second
    // copy of the rig carrying the newer weapons' clips (Sigma DC*, Dual Gun
    // DU*, Death Scythe GR*, Iron Boots IB*, Katana A KT*, Railgun NS*,
    // Rocket RL*, ...). Without it 460 of the S10 actorstates rows pointed at
    // clips the skeleton does not have. Merge per bone: tracks the main file
    // lacks are taken from the extension; the main file's own tracks win.
    {
        scn::Scene ext = scn::Parse((std::string(cbase) + "bip_" + sex + "/" + sex + "_bip_0000.scn").c_str());
        size_t added = 0;
        if (ext.ok) {
            std::unordered_map<std::string, scn::Bone*> byName;
            for (scn::Bone& b : skel.bones) byName[b.name] = &b;
            for (const scn::Bone& eb : ext.bones) {
                auto it = byName.find(eb.name);
                if (it == byName.end()) continue;
                scn::Bone& b = *it->second;
                for (const scn::Anim& a : eb.anims) {
                    bool have = false;
                    for (const scn::Anim& own : b.anims) if (own.name == a.name) { have = true; break; }
                    if (!have) { b.anims.push_back(a); ++added; }
                }
            }
            for (const std::string& n : ext.animNames)
                if (std::find(skel.animNames.begin(), skel.animNames.end(), n) == skel.animNames.end())
                    skel.animNames.push_back(n);
        }
        // The merged clips are copies of the extension's tracks: counted on the skeleton.
        if (ext.ok && added) skel.memTag.Set(reslog::SCN, skel.memTag.path + " + " + sex + "_bip_0000 clips",
                                             skel.memTag.bytes + ext.memTag.bytes, "(merged skeleton)");
        DbgLog("[char] %s_bip_0000 ext=%d merged %zu bone tracks, clips now %zu",
               sex.c_str(), (int)ext.ok, added, skel.animNames.size());
    }
    return skel;
}

void WorldState::LoadCharacterBody(bool asFemale) {
    const std::string sex = asFemale ? "female" : "male";
    scn::Scene skel = LoadCharacterSkeleton(asFemale);
    if (!skel.ok) { DbgLog("[char] %s_bip.scn failed to load - keeping the current body", sex.c_str()); return; }
    std::string files[8];
    for (int i = 0; i < 8; ++i) files[i] = CostumeFile(asFemale, i);
    std::vector<scn::Scene> parts = LoadCostumeParts(asFemale, files);
    if (parts.empty()) { DbgLog("[char] no %s body parts - keeping the current body", sex.c_str()); return; }
    character = std::move(skel);
    bodyParts = std::move(parts);
    female = asFemale;
    animCtrl.ForceRefresh();   // clip durations come from the new skeleton
    animCtrl.Restart();
    DbgLog("[char] %s_bip bones=%zu parts=%zu", sex.c_str(), character.bones.size(), bodyParts.size());
    if (gCostume.female != asFemale) { gCostume.female = asFemale; SaveWeaponSlots(); }
    if (mpClient.Connected()) mpClient.SendCostume(CostumeString());
    if (costumeBuilt) SpawnCostumeEffects();   // F7: the other sex's worn effects
}

// Where a node of a worn part is now, in character space: its own-track
// chain inside the part, then the skeleton bone it hangs from (or Bip01 Head
// for the head parts, like GatherCharacterPieces).
bool WorldState::CostumeNodeWorld(const std::string& node, float out[16], const scn::AnimPose* poseIn) const {
    const scn::AnimPose& curPose = poseIn ? *poseIn : this->curPose;
    // Many items hang the effect straight on a skeleton bone (127: Bip01
    // Spine2 / Spine1 / Head).
    if (SkelBoneWorld(character, node, curPose, curLowerPoseActive ? &curLowerPose : nullptr, out)) return true;
    for (const scn::Scene& part : bodyParts) {
        bool found = false;
        for (const scn::Bone& b : part.bones) if (b.name == node) { found = true; break; }
        if (!found) for (const scn::Model& m : part.models) if (m.name == node) { found = true; break; }
        if (!found) continue;
        scn::Mat4 acc{}; acc[0] = acc[5] = acc[10] = acc[15] = 1.f;
        std::string cur = node;
        float skelBone[16];
        for (int depth = 0; depth < 32; ++depth) {
            // Stop at the first ancestor the live skeleton has: skinned parts
            // carry their own copy of the bip (15_1_male_hand: Hand_Master_R_
            // Dummy under its Bip01 R Hand). Walking that static copy up to the
            // root left the effect on the fallback bone instead of the hand.
            if (depth > 0 && SkelBoneWorld(character, cur, curPose, curLowerPoseActive ? &curLowerPose : nullptr, skelBone)) break;
            const std::vector<scn::Anim>* anims = nullptr; const float* M = nullptr; std::string parent;
            for (const scn::Bone& b : part.bones) if (b.name == cur) { anims = &b.anims; M = b.matrix; parent = b.parent; break; }
            if (!anims) for (const scn::Model& m : part.models) if (m.name == cur) { anims = &m.anims; M = m.matrix; parent = m.parent; break; }
            if (!anims) break;   // left the part
            scn::Mat4 local; std::memcpy(local.data(), M, sizeof(float) * 16);
            for (const scn::Anim& a : *anims) if (a.hasTransform) { local = scn::BuildTRS(a.initT, a.initR, a.initS); break; }
            acc = scn::Mat4Mul(acc, local);
            cur = parent;
        }
        float bone[16];
        const bool onSkel = !cur.empty() && SkelBoneWorld(character, cur, curPose, curLowerPoseActive ? &curLowerPose : nullptr, bone);
        if (!onSkel && !SkelBoneWorld(character, part.attachBone.empty() ? std::string("Bip01 Head") : part.attachBone, curPose, curLowerPoseActive ? &curLowerPose : nullptr, bone)) return false;
        float res[16]; Mat4MulS(acc.data(), bone, res);
        std::memcpy(out, res, sizeof(res));
        return true;
    }
    return false;
}

void WorldState::SpawnCostumeEffects() {
    BuildCostumeList();
    activeEffects.erase(std::remove_if(activeEffects.begin(), activeEffects.end(),
                        [](const ActiveEffect& e) { return e.tag == "costume"; }), activeEffects.end());
    for (int slot = 0; slot < 8; ++slot) {
        const std::string worn = CostumeFile(female, slot);
        if (worn.empty()) continue;
        for (const CostumeItem& it : costumeItems[female][slot]) {
            if (it.file != worn) continue;
            for (const auto& sq : it.seqs) {
                const size_t before = activeEffects.size();
                SpawnSeq(sq.second, 0.f, 0.f, 0.f, 0.f, "costume");   // looped while worn
                if (activeEffects.size() > before) {
                    activeEffects.back().attachNode = sq.first;
                    // Node missing from the worn .scn (15_1_male_face has no
                    // Eye_Master_*_Dummy): keep it on the part, not the feet.
                    // Hand_Master_R_Dummy / Foot_Master_L_Dummy are missing from
                    // 15_1_male_hand/foot: use the matching hand/foot bone.
                    const std::string& nn = sq.first;
                    const bool left = nn.find("_L_") != std::string::npos || nn.rfind("L_", 0) == 0 || nn.find(" L ") != std::string::npos;
                    const char* side = left ? "L" : "R";
                    std::string fb;
                    switch (slot) {
                    case 4: fb = std::string("Bip01 ") + side + " Hand"; break;   // hand
                    case 5: fb = std::string("Bip01 ") + side + " Foot"; break;   // foot
                    case 3: fb = "Bip01 Pelvis"; break;                           // leg
                    case 2: fb = "Bip01 Spine1"; break;                           // body
                    default: fb = "Bip01 Head"; break;                            // hair, face, acc
                    }
                    activeEffects.back().attachFallback = fb;
                }
            }
            break;
        }
    }
}

void WorldState::ReloadCostume() {
    std::string files[8];
    for (int i = 0; i < 8; ++i) files[i] = CostumeFile(female, i);
    std::vector<scn::Scene> parts = LoadCostumeParts(female, files);
    if (!parts.empty()) bodyParts = std::move(parts);
    SpawnCostumeEffects();
    SaveWeaponSlots();
    if (mpClient.Connected()) mpClient.SendCostume(CostumeString());
}

std::string WorldState::CostumeString() const {
    std::string s = std::string("{\"gender\":\"") + (female ? "female" : "male") + "\"";
    for (int i = 0; i < 8; ++i) s += std::string(",\"") + kCostumeSlots[i] + "\":\"" + CostumeFile(female, i) + "\"";
    return s + "}";
}

// item.x7 (plain XML in S10): keys 1000000..1069999 are hair, face, top,
// pants, gloves, shoes, accessories (10000 each, same ranges as S4Web2's
// item_db.js); 2000000..2069999 are weapons (icon only). Skinned parts are
// to_part_scene_file; hair/face/accessory items are to_node_scene_fileN
// under a bip bone, which load the same way as the default 00_<sex>_hair.
static std::string NormName(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += (char)std::tolower(c);
    return o;
}

void WorldState::BuildCostumeList() {
    if (costumeBuilt) return;
    costumeBuilt = true;
    const std::string x = ReadTextFile(RESC("resources/xml/item.x7"));
    auto attr = [](const std::string& tag, const std::string& name) -> std::string {
        const std::string pat = " " + name + "=\"";
        size_t p = tag.find(pat); if (p == std::string::npos) return "";
        p += pat.size();
        size_t e = tag.find('"', p); return e == std::string::npos ? "" : tag.substr(p, e - p);
    };
    std::set<std::string> seen[2][8];
    for (int s = 0; s < 2; ++s)
        for (int i = 0; i < 8; ++i) {
            const std::string def = i >= 6 ? std::string() : std::string("00_") + (s ? "female" : "male") + "_" + kCostumeSlots[i] + ".scn";
            costumeItems[s][i].push_back({ i >= 6 ? "None" : "Default", def, "" });
            seen[s][i].insert(def);
        }
    std::map<std::string, std::string> weaponIconByName;
    struct RawWeapon { long key; std::string name, icon; int parent = -1; };
    std::vector<RawWeapon> weaponRaw;
    size_t pos = 0;
    while ((pos = x.find("<item item_key=\"", pos)) != std::string::npos) {
        pos += 16;
        const long key = strtol(x.c_str() + pos, nullptr, 10);
        const size_t end = x.find("</item>", pos);
        if (end == std::string::npos) break;
        const bool weapon = key >= 2000000 && key <= 2069999;
        if (!weapon && (key < 1000000 || key > 1079999)) { pos = end; continue; }
        const std::string body = x.substr(pos, end - pos);
        pos = end;
        const size_t b = body.find("<base "), g = body.find("<graphic ");
        if (g == std::string::npos) continue;
        const std::string baseTag = b == std::string::npos ? std::string() : body.substr(b, body.find('>', b) - b);
        const std::string gTag = body.substr(g, body.find('>', g) - g);
        const std::string name = attr(baseTag, "name"), icon = attr(gTag, "icon_image");
        if (weapon) {
            if (!name.empty() && !icon.empty()) weaponIconByName.emplace(NormName(name), icon);
            const size_t et = body.find("<etc ");
            const std::string parent = et == std::string::npos ? std::string() : attr(body.substr(et, body.find('>', et) - et), "parent_number");
            weaponRaw.push_back({ key, name, icon, parent.empty() ? -1 : atoi(parent.c_str()) });
            continue;
        }
        const int slot = (int)((key - 1000000) / 10000);
        std::string file = attr(gTag, "to_part_scene_file");
        if (file == "-") file.clear();
        if (file.empty() && (slot <= 1 || slot >= 6))
            for (int n = 1; n <= 6; ++n) {
                const std::string f = attr(gTag, "to_node_scene_file" + std::to_string(n));
                if (f.empty() || f == "-") continue;
                // Bone it hangs from, kept with the file ("x.scn@Bip01 Pelvis");
                // the head is the default and is left implicit.
                const std::string bone = attr(gTag, "to_node_parent_node" + std::to_string(n));
                const bool onSkeleton = !bone.empty() && bone != "Bip01 Head" && (bone.rfind("Bip01", 0) == 0 || bone.rfind("Pet View", 0) == 0);
                const bool animPart = attr(gTag, "to_node_animation_part" + std::to_string(n)) == "1";
                file += (file.empty() ? "" : "|") + f + (animPart ? "*" : "") + (onSkeleton ? "@" + bone : std::string());
            }
        if (file.empty()) continue;
        const std::string first = file.substr(0, (std::min)({ file.find('|'), file.find('@'), file.find('*') }));
        const std::string sex = attr(baseTag, "sex");
        std::vector<std::pair<std::string, std::string>> seqs;
        const size_t sq = body.find("<sequence ");
        if (sq != std::string::npos) {
            const std::string sTag = body.substr(sq, body.find('>', sq) - sq);
            for (int n = 1; n <= 8; ++n) {
                const std::string node = attr(sTag, "node_name" + std::to_string(n)), f = attr(sTag, "file_name" + std::to_string(n));
                if (!f.empty() && f != "-") seqs.push_back({ node, f });
            }
        }
        for (int s = 0; s < 2; ++s) {
            if ((sex == "man" && s == 1) || (sex == "woman" && s == 0)) continue;
            if (seen[s][slot].count(file)) {
                // Same .scn listed again (recolors, set versions): keep the
                // first name, but take its effects if the first had none.
                if (!seqs.empty())
                    for (CostumeItem& c : costumeItems[s][slot]) if (c.file == file && c.seqs.empty()) { c.seqs = seqs; break; }
                continue;
            }
            const std::string path = std::string(kCharBase) + kCostumeSlots[slot] + "/" + first;
            if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            seen[s][slot].insert(file);
            costumeItems[s][slot].push_back({ name.empty() ? first : name, file, icon, seqs });
        }
    }
    for (int s = 0; s < 2; ++s)
        for (int i = 0; i < 8; ++i) {
            auto& v = costumeItems[s][i];
            std::sort(v.begin() + 1, v.end(), [](const CostumeItem& a, const CostumeItem& c) { return a.file < c.file; });
        }
    // Weapon icons: the lua state name is the item name without spaces
    // (PlasmaSword = "Plasma Sword"); otherwise the longest item name it starts with.
    for (const WeaponEntry& e : weaponList) {
        const std::string n = NormName(e.states);
        auto it = weaponIconByName.find(n);
        if (it != weaponIconByName.end()) { weaponIcons[e.states] = it->second; continue; }
        size_t best = 0;
        for (const auto& kv : weaponIconByName)
            if (kv.first.size() > best && kv.first.size() >= 3 && (n.rfind(kv.first, 0) == 0 || kv.first.rfind(n, 0) == 0)) {
                best = kv.first.size(); weaponIcons[e.states] = kv.second;
            }
    }
    // ---- All weapons of item.x7 ----
    // states: weapon_lua.x7 (item_key -> ani="NormalState_<states>").
    // model:  _eu_weapon.x7 <scene value1> for the items it lists; otherwise
    //         the .scn named like the icon (icon_breaker_oriental.png ->
    //         breaker_oriental.scn), else the best word match with the icon
    //         (bat_firework -> firework_bat.scn, score >= 0.6), else the base
    //         model of its weapon type (weaponList).
    {
        std::map<long, std::string> statesOf, sceneOf, weakAniOf;
        const std::string lua = ReadTextFile(RESC("resources/xml/weapon_lua.x7"));
        for (size_t p = 0; (p = lua.find("<lua_func item_key=\"", p)) != std::string::npos; ) {
            p += 20;
            const long k = strtol(lua.c_str() + p, nullptr, 10);
            const size_t e = lua.find("/>", p);
            const std::string tag = lua.substr(p, e - p);
            if (tag.find("state=\"left_weak\"") != std::string::npos) {
                const size_t a = tag.find("ani=\"");
                if (a != std::string::npos) { const size_t b2 = tag.find('"', a + 5); weakAniOf[k] = tag.substr(a + 5, b2 - a - 5); }
            }
            if (tag.find("state=\"normal\"") != std::string::npos) {
                const size_t a = tag.find("ani=\"NormalState_");
                if (a != std::string::npos) { const size_t b2 = tag.find('"', a + 17); statesOf[k] = tag.substr(a + 17, b2 - a - 17); }
            }
            p = e;
        }
        const std::string ew = ReadTextFile(RESC("resources/xml/_eu_weapon.x7"));
        for (size_t p = 0; (p = ew.find("<weapon item_key=\"", p)) != std::string::npos; ) {
            p += 18;
            const long k = strtol(ew.c_str() + p, nullptr, 10);
            const size_t e = ew.find("</weapon>", p);
            const size_t sc = ew.find("<scene ", p);
            if (sc != std::string::npos && sc < e) { const std::string v = attr(ew.substr(sc, ew.find("/>", sc) - sc), "value1"); if (!v.empty()) sceneOf[k] = v; }
            p = e;
        }
        std::vector<std::string> models;
        { WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(RESC("resources/resources/model/weapon/*.scn"), &fd);
          if (h != INVALID_HANDLE_VALUE) { do { std::string f = fd.cFileName; for (char& c : f) c = (char)std::tolower((unsigned char)c); models.push_back(f); } while (FindNextFileA(h, &fd)); FindClose(h); } }
        const std::set<std::string> modelSet(models.begin(), models.end());
        auto tokens = [](std::string s) {
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            for (const char* ext : { ".png", ".tga", ".dds", ".scn" }) if (s.size() > 4 && s.compare(s.size() - 4, 4, ext) == 0) s.resize(s.size() - 4);
            static const std::set<std::string> skip = { "icon", "sword", "r", "l", "weapon", "unique", "x", "a", "b", "c" };
            std::set<std::string> t; std::string cur; bool digit = false;
            auto flush = [&] { if (!cur.empty() && !skip.count(cur) && !(digit && cur.size() < 3)) t.insert(cur); cur.clear(); };
            for (char c : s) {
                const bool isD = std::isdigit((unsigned char)c) != 0, isA = std::isalpha((unsigned char)c) != 0;
                if (!isD && !isA) { flush(); continue; }
                if (!cur.empty() && isD != digit) flush();
                digit = isD; cur += c;
            }
            flush();
            return t;
        };
        std::vector<std::set<std::string>> modelTok; for (const std::string& m : models) modelTok.push_back(tokens(m));
        auto baseModel = [&](const std::string& states) -> std::string {
            for (const WeaponEntry& e : weaponList) if (e.states == states) return e.scn;
            return {};
        };
        int exact = 0, byIcon = 0, fuzzy = 0, base = 0, inferred = 0;
        std::set<std::string> statesWithItem;
        // Items with no weapon_lua.x7 row (Snow Fairy, Falling...): parent_number
        // is the weapon type inside its category (melee 1 = PlasmaSword, 2 =
        // CounterSword, 6 = SpyDagger...) - take the states its siblings use.
        std::map<std::pair<int, int>, std::map<std::string, int>> statesByParent;
        for (const RawWeapon& rw : weaponRaw) {
            auto st = statesOf.find(rw.key);
            if (st != statesOf.end() && rw.parent >= 0) statesByParent[{ (int)((rw.key - 2000000) / 10000), rw.parent }][st->second]++;
        }
        for (const RawWeapon& rw : weaponRaw) {
            const int category = (int)((rw.key - 2000000) / 10000);
            std::string states;
            auto st = statesOf.find(rw.key);
            if (st != statesOf.end()) states = st->second;
            else if (rw.parent >= 0) {
                auto sp = statesByParent.find({ category, rw.parent });
                if (sp != statesByParent.end()) {
                    int best = 0;
                    for (const auto& kv : sp->second) if (kv.second > best) { best = kv.second; states = kv.first; }
                    if (!states.empty()) ++inferred;
                }
            }
            if (states.empty()) continue;   // no weapon type to play it with
            // States with no weapon type of their own (Lightsaber: its attack
            // anis are SwordAtkWeakState_PlasmaSword_Lightsaber_Kylo...): play
            // it as the type its attack animation names (PlasmaSword) - its
            // attacks, attach bones and position.
            if (!luaStates.attackMap().typeBySuffix.count(states)) {
                auto wa = weakAniOf.find(rw.key);
                const std::string pre = "SwordAtkWeakState_";
                if (wa != weakAniOf.end() && wa->second.rfind(pre, 0) == 0) {
                    const std::string rest = wa->second.substr(pre.size());
                    const std::string t = rest.substr(0, rest.find('_'));
                    if (luaStates.attackMap().typeBySuffix.count(t)) states = t;
                }
            }
            std::string scn;
            auto sc = sceneOf.find(rw.key);
            if (sc != sceneOf.end()) { std::string l = sc->second; for (char& c : l) c = (char)std::tolower((unsigned char)c); if (modelSet.count(l)) { scn = sc->second; ++exact; } }
            if (scn.empty() && !rw.icon.empty()) {
                std::string b = rw.icon; for (char& c : b) c = (char)std::tolower((unsigned char)c);
                const size_t dot = b.rfind('.'); if (dot != std::string::npos) b.resize(dot);
                if (b.rfind("icon_", 0) == 0) b = b.substr(5);
                std::string noNum = b; { const size_t u = noNum.rfind('_'); if (u != std::string::npos && u + 1 < noNum.size() && std::all_of(noNum.begin() + u + 1, noNum.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; })) noNum.resize(u); }
                for (const std::string& c : { b, noNum }) {
                    for (const char* suf : { "", "_r", "_l" }) if (modelSet.count(c + suf + ".scn")) { scn = c + suf + ".scn"; break; }
                    if (!scn.empty()) { ++byIcon; break; }
                }
                if (scn.empty()) {
                    const std::set<std::string> it = tokens(rw.icon);
                    float best = 0.f;
                    for (size_t m = 0; m < models.size(); ++m) {
                        size_t shared = 0; for (const std::string& t : it) shared += modelTok[m].count(t);
                        if (!shared) continue;
                        const float score = (float)shared / (float)(it.size() + modelTok[m].size() - shared);
                        if (score > best) { best = score; scn = models[m]; }
                    }
                    if (best < 0.6f) scn.clear(); else ++fuzzy;
                }
            }
            if (scn.empty()) { scn = baseModel(states); if (!scn.empty()) ++base; }
            if (scn.empty()) continue;      // nothing to draw it with
            statesWithItem.insert(states);
            weaponItems.push_back({ rw.name.empty() ? states : rw.name, rw.icon, scn, states, category });
        }
        for (const WeaponEntry& e : weaponList)   // weapon types no item names
            if (!statesWithItem.count(e.states)) weaponItems.push_back({ e.states, "", e.scn, e.states, -1 });
        DbgLog("[inventory] weapons %zu of %zu items (x7 model %d, icon name %d, word match %d, base model %d, states from siblings %d)",
               weaponItems.size(), weaponRaw.size(), exact, byIcon, fuzzy, base, inferred);
    }

    DbgLog("[inventory] male %zu/%zu/%zu/%zu/%zu/%zu/%zu female %zu/%zu/%zu/%zu/%zu/%zu/%zu, weapon icons %zu/%zu",
           costumeItems[0][0].size(), costumeItems[0][1].size(), costumeItems[0][2].size(), costumeItems[0][3].size(),
           costumeItems[0][4].size(), costumeItems[0][5].size(), costumeItems[0][6].size(),
           costumeItems[1][0].size(), costumeItems[1][1].size(), costumeItems[1][2].size(), costumeItems[1][3].size(),
           costumeItems[1][4].size(), costumeItems[1][5].size(), costumeItems[1][6].size(),
           weaponIcons.size(), weaponList.size());
}

// ---- Inventory page (ESC > Inventory), laid out like S4's equip window ----
static const int  kBoxCostume[12] = { 0, 1, 6, 7, 2, 3, 4, 5, -1, -1, -1, -1 };   // box -> costume slot (-1 = weapons, skill)
static const char* const kBoxLabel[12] = { "HAIR", "FACE", "ACC", "PET", "TOP", "LEGS", "HANDS", "FEET",
                                           "WEAPON 1", "WEAPON 2", "WEAPON 3", "SKILL" };
// Grid size follows the screen (set by InvLayout every frame).
static int kInvCols = 6, kInvRows = 7;
static const int kInvCell = 80;
static const int kInvGridTop = 74;   // title row + weapon filter tabs above the cells
static const char* const kWeaponFilters[8] = { "ALL", "MELEE", "RIFLE", "HEAVY", "SNIPER", "SENTRY", "THROWN", "SPECIAL" };
// Filter tab rects (weapon boxes only): tab 0 = all, tab k = category k-1.
static WorldState::UiRect InvFilterTab(const WorldState::UiRect& grid, int tab) {
    const int w = (std::min)(84, (grid.w - 34) / 8);
    return { grid.x + 10 + tab * w, grid.y + 40, w - 4, 26 };
}

void WorldState::InvLayout(UiRect boxes[12], UiRect& panel, UiRect& grid) const {
    // Full screen: equip window on the left (~45%), item grid on the right.
    const int M = 16;
    const int PW = (std::max)(420, scrW * 45 / 100), PH = scrH - 2 * M;
    panel = { M, M, PW, PH };
    const int bottomH = (std::min)(78, (std::max)(60, PH / 9));           // weapon row
    const int B = (std::min)(78, (std::max)(56, (PH - 54 - bottomH - 40) / 4 - 12));
    for (int i = 0; i < 4; ++i) {
        boxes[i]     = { panel.x + 20,          panel.y + 54 + i * (B + 12), B, B };
        boxes[4 + i] = { panel.x + PW - 20 - B, panel.y + 54 + i * (B + 12), B, B };
    }
    const int ww = (std::min)(110, (PW - 40 - 3 * 12) / 4);
    for (int j = 0; j < 4; ++j) boxes[8 + j] = { panel.x + 20 + j * (ww + 12), panel.y + PH - bottomH - 30, ww, bottomH };
    grid = { panel.x + PW + M, M, scrW - (panel.x + PW + M) - M, PH };
    kInvCols = (std::max)(3, (grid.w - 34) / kInvCell);
    kInvRows = (std::max)(3, (grid.h - kInvGridTop - 60) / kInvCell);
}

// Middle of the equip window, between the two box columns: the character preview.
static WorldState::UiRect InvPreviewRect(const WorldState::UiRect boxes[12], const WorldState::UiRect& panel) {
    const int x = boxes[0].x + boxes[0].w + 16, y = panel.y + 44;
    return { x, y, boxes[4].x - 16 - x, boxes[8].y - 14 - y };
}

std::vector<int> WorldState::InvWeaponView() const {
    std::vector<int> v;
    for (size_t k = 0; k < weaponItems.size(); ++k)
        if (invWeaponFilter < 0 || weaponItems[k].category == invWeaponFilter) v.push_back((int)k);
    return v;
}

int WorldState::InvGridCount() const {
    if (invBox >= 8 && invBox <= 10) return (int)InvWeaponView().size();
    const int cs = kBoxCostume[invBox];
    return cs < 0 ? 0 : (int)costumeItems[female][cs].size();
}

int WorldState::InvEquippedIndex(int box) const {
    if (box >= 8 && box <= 10) {
        const int j = box - 8;
        if (j >= (int)gLoadout.size()) return -1;
        const std::vector<int> view = InvWeaponView();
        for (size_t k = 0; k < view.size(); ++k)
            if (weaponItems[view[k]].scn == gLoadout[j].scn && weaponItems[view[k]].states == gLoadout[j].states) return (int)k;
        return -1;
    }
    const int cs = kBoxCostume[box];
    if (cs < 0) return -1;
    const std::string cur = CostumeFile(female, cs);
    const auto& v = costumeItems[female][cs];
    for (size_t k = 0; k < v.size(); ++k) if (v[k].file == cur) return (int)k;
    return -1;
}

void WorldState::InvSelectBox(int box) {
    invBox = box;
    // A weapon box shows its weapon in hand on the preview.
    if (box >= 8 && box <= 10 && box - 8 != weaponSlot && box - 8 < (int)gLoadout.size()) EquipWeapon(box - 8);
    const int eq = InvEquippedIndex(box);
    invSel[box] = eq < 0 ? 0 : eq;
    invTop = (std::max)(0, invSel[box] / kInvCols - kInvRows / 2) * kInvCols;
}

void WorldState::InvEquip(int index) {
    if (invBox >= 8 && invBox <= 10) {
        const std::vector<int> view = InvWeaponView();
        if (index < 0 || index >= (int)view.size()) return;
        index = view[index];
        const int j = invBox - 8;
        while ((int)gLoadout.size() < 3) gLoadout.push_back(gLoadout.empty() ? SWeaponSlot{} : gLoadout.back());
        gLoadout[j] = { weaponItems[index].scn, weaponItems[index].states };
        SaveWeaponSlots();
        EquipWeapon(j);
        DbgLog("[inventory] weapon %d = %s: %s (%s)", j + 1, weaponItems[index].name.c_str(), weaponItems[index].states.c_str(), weaponItems[index].scn.c_str());
        return;
    }
    const int cs = kBoxCostume[invBox];
    if (cs < 0) return;
    const auto& list = costumeItems[female][cs];
    if (index < 0 || index >= (int)list.size()) return;
    const std::string key = std::string(female ? "female_" : "male_") + kCostumeSlots[cs];
    if (index == 0) gCostume.part.erase(key); else gCostume.part[key] = list[index].file;
    ReloadCostume();
    DbgLog("[inventory] %s = %s", key.c_str(), list[index].file.c_str());
}

bool WorldState::MouseClient(int& mx, int& my) const {
    HWND hwnd = GetForegroundWindow();
    POINT pt; RECT rc;
    if (!hwnd || !GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &rc)) return false;
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return false;
    mx = pt.x * scrW / cw; my = pt.y * scrH / ch;
    return true;
}

void WorldState::OnEnter() {
    weaponBoneOverrides = LoadWeaponBoneOverrides();
    female = gCostume.female;
    crash::LogMemory("before character");
    LoadCharacterBody(female);
    crash::LogMemory("after character (skeleton + clips + outfit)");

    // Estados de animacion desde los .lua del cliente. Lo que el lua no cubra
    // sigue saliendo de las tablas de anim_states.h.
    DbgLog("[resources] resource_mode=%s scripts=%s", gResourceMode.c_str(), ScriptDir().c_str());
    if (luaStates.Load(ScriptDir().c_str())) {
        int n = animCtrl.LoadFromLua(luaStates, "");   // "" = WeaponUnused
        DbgLog("[anim] lua: %zu funciones, %d estados cubiertos, %zu armas con NormalState_",
               luaStates.size(), n, luaStates.Weapons("NormalState_").size());
    }
    crash::LogMemory("after actorstates lua");
    BuildWeaponList();
    SpawnCostumeEffects();
    EquipWeapon(0);
    crash::LogMemory("after weapon");
    ScanMaps();
    int start = 0;
    // Default map: Station 2 (the square if this resource set lacks it).
    for (size_t i = 0; i < maps.size(); ++i) if (maps[i].ini == "bginfo-square.ini") { start = (int)i; break; }
    for (size_t i = 0; i < maps.size(); ++i) if (maps[i].ini == "bginfo-station-2.ini") { start = (int)i; break; }
    LoadMap(start);
    crash::LogMemory("after map");
    menuSel = mapIdx;
    // The menu opens with the game: online or solo first (ESC brings it back).
    mapMenu = true; menuPage = 0; mainSel = gOnline.enabled ? 0 : 1;
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
    if (!displayApplied) { displayApplied = true; if (gResW > 0 || gBorderless) ApplyDisplayMode(); }
    if (GetAsyncKeyState(VK_F5) & 1) {   // music on / off (saved)
        gMusicOn = !gMusicOn;
        if (gMusicOn) PlayMapMusic(0); else audio::StopMusic();
        SaveWeaponSlots();
    }
    if (GetAsyncKeyState(VK_F6) & 1) {
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        ChangeMusic(shift ? -1 : +1);   // Shift+F6 = previous track
    }
    if (GetAsyncKeyState(VK_F7) & 1) LoadCharacterBody(!female);   // male <-> female bip
    if (audio::ConsumeMusicFinished() && gMusicOn) PlayMapMusic(gMusicTrack.empty() ? bgmIndex + 1 : 0);   // the chosen track repeats
    // ESC menu (also open when the game starts). While it is open the world is
    // frozen and the cursor comes back; the online link keeps running.
    // Main: online / solo / maps / wardrobe / weapons / sex / back / quit.
    // ESC in a sub-page goes back to the main page, in the main page closes.
    auto menuClock = [] { LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
                          return (uint32_t)((double)q.QuadPart * 1000.0 / (double)f.QuadPart); };
    // F4: the inventory on the weapon boxes (same window as the clothes).
    if (GetAsyncKeyState(VK_F4) & 1) {
        if (mapMenu && menuPage == 2) mapMenu = false;
        else { BuildCostumeList(); mapMenu = true; menuPage = 2; InvSelectBox(8 + ((weaponSlot >= 0 && weaponSlot < 3) ? weaponSlot : 0)); }
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 1) {
        if (!mapMenu) { mapMenu = true; menuPage = 0; }
        else if (menuPage != 0) menuPage = 0;
        else mapMenu = false;
    }
    if (mapMenu) {
        ClipCursor(nullptr);
        if (cursorHidden) { while (ShowCursor(TRUE) < 0) {} cursorHidden = false; }
        if (menuPage == 0) {
            const int rows = 9;
            if (GetAsyncKeyState(VK_UP)   & 1) mainSel = (mainSel + rows - 1) % rows;
            if (GetAsyncKeyState(VK_DOWN) & 1) mainSel = (mainSel + 1) % rows;
            bool pick = (GetAsyncKeyState(VK_RETURN) & 1) != 0;
            int mx, my;
            const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, click = lmb && !lmbPrev;
            lmbPrev = lmb;
            if (MouseClient(mx, my)) {   // same rows as the render below
                const int W = 520, H = rows * 30 + 70, x = (scrW - W) / 2, y = (scrH - H) / 2;
                for (int i = 0; i < rows; ++i)
                    if (UiRect{ x + 6, y + 40 + i * 30 - 4, W - 12, 28 }.Hit(mx, my)) { mainSel = i; if (click) pick = true; }
            }
            if (pick) {
                switch (mainSel) {
                case 0: if (mpOnline) mpOnline = false; else { mpOnline = true; mpNextTryMs = 0; mapMenu = false; } break;
                case 1: mpOnline = false; mapMenu = false; break;
                case 2: menuPage = 1; menuSel = mapIdx < 0 ? 0 : mapIdx; break;
                case 3: BuildCostumeList(); menuPage = 2; InvSelectBox(invBox); break;
                case 4: BuildCostumeList(); menuPage = 2; InvSelectBox(8 + ((weaponSlot >= 0 && weaponSlot < 3) ? weaponSlot : 0)); break;
                case 5: LoadCharacterBody(!female); break;
                case 6: menuPage = 3; settingsSel = 0; break;   // Settings: resolution, display, graphics
                case 7: mapMenu = false; break;
                case 8: PostQuitMessage(0); break;
                }
            }
        } else if (menuPage == 3) {
            // Settings: Up/Down row, Left/Right (or Enter / click) changes it.
            const int rows = 7;
            if (GetAsyncKeyState(VK_UP)   & 1) settingsSel = (settingsSel + rows - 1) % rows;
            if (GetAsyncKeyState(VK_DOWN) & 1) settingsSel = (settingsSel + 1) % rows;
            int step = 0;
            if (GetAsyncKeyState(VK_LEFT)   & 1) step = -1;
            if (GetAsyncKeyState(VK_RIGHT)  & 1) step = +1;
            if (GetAsyncKeyState(VK_RETURN) & 1) step = +1;
            int mx, my;
            const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, click = lmb && !lmbPrev;
            lmbPrev = lmb;
            if (MouseClient(mx, my)) {   // same rows as the render
                const int W = 560, H = rows * 34 + 80, x = (scrW - W) / 2, y = (scrH - H) / 2;
                for (int i = 0; i < rows; ++i)
                    if (UiRect{ x + 6, y + 44 + i * 34 - 4, W - 12, 30 }.Hit(mx, my)) { settingsSel = i; if (click) step = +1; }
            }
            if (step) {
                if (settingsSel == 0) {
                    // Aspect ratio: keep the resolution if it already matches,
                    // else the one of that aspect closest in width.
                    gAspect = (gAspect + step + 6) % 6;
                    if (gAspect != 0 && gResW > 0 && std::string(AspectName(gResW, gResH)) != kAspects[gAspect]) {
                        const std::vector<ResOpt> opts = ResolutionChoices(GameHwndSlot());
                        int best = -1;
                        for (size_t k = 0; k < opts.size(); ++k)
                            if (best < 0 || std::abs(opts[k].w - gResW) < std::abs(opts[best].w - gResW)) best = (int)k;
                        if (best >= 0) { gResW = opts[best].w; gResH = opts[best].h; gBorderless = false; ApplyDisplayMode(); }
                    }
                } else if (settingsSel == 1) {
                    // "Window" (free size) then every resolution that fits the monitor.
                    const std::vector<ResOpt> opts = ResolutionChoices(GameHwndSlot());
                    int cur = -1;
                    for (size_t k = 0; k < opts.size(); ++k) if (opts[k].w == gResW && opts[k].h == gResH) cur = (int)k;
                    const int n = (int)opts.size() + 1;
                    int next = ((cur + 1) + step + n) % n - 1;
                    if (next < 0) { gResW = gResH = 0; } else { gResW = opts[next].w; gResH = opts[next].h; }
                    gBorderless = false;
                    ApplyDisplayMode();
                } else if (settingsSel == 2) {
                    gBorderless = !gBorderless;
                    ApplyDisplayMode();
                } else if (settingsSel == 3) {
                    gGraphicsQuality = (gGraphicsQuality + (step > 0 ? 1 : 2)) % 3;
                } else if (settingsSel == 4) {
                    gMusicOn = !gMusicOn;
                    if (gMusicOn) PlayMapMusic(0); else audio::StopMusic();
                } else if (settingsSel == 5) {
                    ChangeMusic(step > 0 ? +1 : -1);
                } else {
                    menuPage = 0;
                }
                SaveWeaponSlots();
            }
        } else if (menuPage == 1) {
            const int rows = (int)maps.size();
            if (rows) {
                if (input::g.wheel) { menuSel = (std::max)(0, (std::min)(rows - 1, menuSel - input::g.wheel * 3)); input::g.wheel = 0; }
                if (GetAsyncKeyState(VK_UP)    & 1) menuSel = (menuSel + rows - 1) % rows;
                if (GetAsyncKeyState(VK_DOWN)  & 1) menuSel = (menuSel + 1) % rows;
                if (GetAsyncKeyState(VK_PRIOR) & 1) menuSel = (std::max)(0, menuSel - 10);
                if (GetAsyncKeyState(VK_NEXT)  & 1) menuSel = (std::min)(rows - 1, menuSel + 10);
                if (menuSel < menuTop) menuTop = menuSel;
                if (menuSel >= menuTop + kMapMenuRows) menuTop = menuSel - kMapMenuRows + 1;
                if (GetAsyncKeyState(VK_RETURN) & 1) { LoadMap(menuSel); mapMenu = false; }
            }
        } else {
            // Inventory: click a box (or Tab), then an item (or arrows + Enter). F7 = male/female.
            UiRect boxes[12], panel, grid; InvLayout(boxes, panel, grid);
            int mx = -1, my = -1;
            const bool haveMouse = MouseClient(mx, my);
            const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, click = lmb && !lmbPrev;
            lmbPrev = lmb;
            if (GetAsyncKeyState(VK_TAB) & 1) { int bx = invBox; bx = (bx + 1) % 11; InvSelectBox(bx); }
            const int n = InvGridCount();
            int& sel = invSel[invBox];
            bool kb = false;
            if (n) {
                if (GetAsyncKeyState(VK_LEFT)  & 1) { sel = (std::max)(0, sel - 1); kb = true; }
                if (GetAsyncKeyState(VK_RIGHT) & 1) { sel = (std::min)(n - 1, sel + 1); kb = true; }
                if (GetAsyncKeyState(VK_UP)    & 1) { sel = (std::max)(0, sel - kInvCols); kb = true; }
                if (GetAsyncKeyState(VK_DOWN)  & 1) { sel = (std::min)(n - 1, sel + kInvCols); kb = true; }
                if (GetAsyncKeyState(VK_PRIOR) & 1) { sel = (std::max)(0, sel - kInvCols * kInvRows); kb = true; }
                if (GetAsyncKeyState(VK_NEXT)  & 1) { sel = (std::min)(n - 1, sel + kInvCols * kInvRows); kb = true; }
                if (GetAsyncKeyState(VK_RETURN) & 1) InvEquip(sel);
            }
            if (kb) {
                if (sel < invTop) invTop = sel / kInvCols * kInvCols;
                if (sel >= invTop + kInvCols * kInvRows) invTop = (sel / kInvCols - kInvRows + 1) * kInvCols;
            }
            // Mouse wheel scrolls the item grid one row per notch.
            if (input::g.wheel) {
                const int totalRows = (n + kInvCols - 1) / kInvCols;
                const int row = (std::max)(0, (std::min)((std::max)(0, totalRows - kInvRows), invTop / kInvCols - input::g.wheel));
                invTop = row * kInvCols;
                input::g.wheel = 0;
            }
            invHover = -1;
            const UiRect pv = InvPreviewRect(boxes, panel);
            if (haveMouse && lmb && (invDragX >= 0 || (click && pv.Hit(mx, my)))) {
                if (invDragX >= 0) invPreviewYaw += (mx - invDragX) * 0.012f;
                invDragX = mx;
            } else invDragX = -1;
            if (haveMouse) {
                for (int bx = 0; bx < 11; ++bx)
                    if (boxes[bx].Hit(mx, my) && click) InvSelectBox(bx);
                if (click && invBox >= 8 && invBox <= 10)
                    for (int t = 0; t < 8; ++t)
                        if (InvFilterTab(grid, t).Hit(mx, my)) { invWeaponFilter = t - 1; InvSelectBox(invBox); }
                for (int i = 0; i < kInvCols * kInvRows && invTop + i < n; ++i) {
                    const UiRect c = { grid.x + 10 + (i % kInvCols) * kInvCell, grid.y + kInvGridTop + (i / kInvCols) * kInvCell, kInvCell - 4, kInvCell - 4 };
                    if (!c.Hit(mx, my)) continue;
                    invHover = invTop + i;
                    if (click) { sel = invTop + i; InvEquip(sel); }
                }
                // Scroll bar: click/drag jumps the list there.
                const UiRect sb = { grid.x + grid.w - 18, grid.y + kInvGridTop, 12, kInvRows * kInvCell };
                const int totalRows = (n + kInvCols - 1) / kInvCols;
                if (lmb && sb.Hit(mx, my) && totalRows > kInvRows) {
                    const int row = (int)((float)(my - sb.y) / sb.h * totalRows) - kInvRows / 2;
                    invTop = (std::max)(0, (std::min)(totalRows - kInvRows, row)) * kInvCols;
                }
            }
            if (GetAsyncKeyState(VK_F7) & 1) { LoadCharacterBody(!female); InvSelectBox(invBox); }
        }
        UpdateOnline(menuClock(), 0.f);
        input::g.wheel = 0;          // notches not used by the open page
        lastTick = GetTickCount();   // don't accumulate dt while paused
        return;
    }
    input::g.wheel = 0;              // in game the wheel does nothing (yet)

    // F4 = selector de armas (todas las del lua con modelo, como el panel del JS).
    if (wpnMenu && !weaponList.empty()) {
        // Loadout editor: 1/2/3 (or Left/Right) pick the slot to fill, Enter
        // puts the highlighted weapon in it, equips it and saves the loadout.
        if (GetAsyncKeyState('1') & 1) wpnTargetSlot = 0;
        if (GetAsyncKeyState('2') & 1) wpnTargetSlot = 1;
        if (GetAsyncKeyState('3') & 1) wpnTargetSlot = 2;
        if (GetAsyncKeyState(VK_LEFT)  & 1) wpnTargetSlot = (wpnTargetSlot + 2) % 3;
        if (GetAsyncKeyState(VK_RIGHT) & 1) wpnTargetSlot = (wpnTargetSlot + 1) % 3;
        const int rows = (int)weaponList.size();
        if (GetAsyncKeyState(VK_UP)    & 1) wpnSel = (wpnSel + rows - 1) % rows;
        if (GetAsyncKeyState(VK_DOWN)  & 1) wpnSel = (wpnSel + 1) % rows;
        if (GetAsyncKeyState(VK_PRIOR) & 1) wpnSel = (std::max)(0, wpnSel - 10);
        if (GetAsyncKeyState(VK_NEXT)  & 1) wpnSel = (std::min)(rows - 1, wpnSel + 10);
        if (wpnSel < wpnTop) wpnTop = wpnSel;
        if (wpnSel >= wpnTop + kMapMenuRows) wpnTop = wpnSel - kMapMenuRows + 1;
        if (GetAsyncKeyState(VK_RETURN) & 1) {
            while ((int)gLoadout.size() < 3) gLoadout.push_back(gLoadout.empty() ? SWeaponSlot{} : gLoadout.back());
            gLoadout[wpnTargetSlot] = { weaponList[wpnSel].scn, weaponList[wpnSel].states };
            SaveWeaponSlots();
            EquipWeapon(wpnTargetSlot);
            DbgLog("[loadout] slot %d = %s (%s)", wpnTargetSlot + 1, weaponList[wpnSel].states.c_str(), weaponList[wpnSel].scn.c_str());
        }
        UpdateOnline(menuClock(), 0.f);
        lastTick = GetTickCount();
        return;                       // el mundo queda quieto mientras elegis
    }

    // 1/2/3 = slot de arma, 0 = desarmado (recarga los estados del lua del arma).
    // The attack states (InitUseWeapon*State) carry no weapon-change row, so a
    // swap pressed mid-swing is ignored - otherwise it cut the stun dive in
    // the air. The key edge is still consumed.
    {
        const bool busy = game::ActorFsm::IsAttack(fsm.current()) || atkParam || weapon2Active;
        const int slotKey = (GetAsyncKeyState('1') & 1) ? 0 : (GetAsyncKeyState('2') & 1) ? 1 :
                            (GetAsyncKeyState('3') & 1) ? 2 : (GetAsyncKeyState('0') & 1) ? -1 : -2;
        if (slotKey != -2 && !busy) EquipWeapon(slotKey);
    }

    // C: leave a clone here, frozen in the pose we have right now (moves it if
    // there is one); V releases it.
    if (GetAsyncKeyState(VK_OEM_6) & 1) { ++gHolsterRoll45; ++gHolsterGen; DbgLog("[weapon] holster roll step %d (%d deg)", gHolsterRoll45 & 7, (gHolsterRoll45 & 7) * 45); }
    if (GetAsyncKeyState(VK_OEM_4) & 1) { --gHolsterRoll45; ++gHolsterGen; DbgLog("[weapon] holster roll step %d (%d deg)", gHolsterRoll45 & 7, (gHolsterRoll45 & 7) * 45); }
    if (GetAsyncKeyState('V') & 1) { if (!clones.empty()) DbgLog("[clone] released %zu", clones.size()); ReleaseClones(); }
    if (GetAsyncKeyState('C') & 1) {
        Clone c;
        std::memcpy(c.pos, charPos, sizeof(c.pos));
        c.yaw = charYaw;
        c.pose = curPose;
        c.lowerActive = curLowerPoseActive; c.lowerPose = curLowerPose;
        c.born = GetTickCount();
        c.ghostSamples = ghostSamples; c.ghostTrail = ghostTrail; c.ghostStart = ghostStart; c.ghostAt = c.born;
        // The effects playing now stay with it, frozen where and when they were.
        const size_t n = activeEffects.size();
        for (size_t i = 0; i < n; ++i) {
            const ActiveEffect& e = activeEffects[i];
            if (e.frozen || e.history.empty()) continue;
            ActiveEffect f = e;
            f.frozen = true; f.frozenAge = e.lastAge; f.fixedWorld = e.history.back().second;
            f.history.clear(); f.followActor = false; f.attachNode.clear(); f.attachFallback.clear();
            f.tag = "clone"; f.offX = f.offZ = 0.f;
            activeEffects.push_back(std::move(f));
        }
        clones.push_back(std::move(c));
        DbgLog("[clone] #%zu at (%.0f %.0f %.0f) pose '%s' tick %u, %zu effects kept", clones.size(), charPos[0], charPos[1], charPos[2],
               curPose.clipA.c_str(), curPose.tickA, activeEffects.size() - n);
    }

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
    const bool freeLook = (GetAsyncKeyState('Q') & 0x8000) != 0;
    if (input::g.mouseDX || input::g.mouseDY) {
        static bool once = false;
        if (!once) { once = true; DbgLog("[world] first mouse delta dx=%ld dy=%ld", input::g.mouseDX, input::g.mouseDY); }
        if (freeLook) {   // Q held: orbit the camera, the character keeps its facing
            lookYaw   += input::g.mouseDX * sens;
            lookPitch += input::g.mouseDY * sens;
        } else {
            camYaw   += input::g.mouseDX * sens;
            camPitch += input::g.mouseDY * sens;
        }
    }
    input::g.mouseDX = 0; input::g.mouseDY = 0;   // consume every frame regardless
    if (!freeLook) {   // released: swing back behind the character (same at any fps)
        const float back = std::exp(-lastDt * 12.f);
        lookYaw *= back; lookPitch *= back;
        if (std::fabs(lookYaw) < 0.001f) lookYaw = 0.f;
        if (std::fabs(lookPitch) < 0.001f) lookPitch = 0.f;
    }
    // Arrow-key camera fallback + zoom.
    if (GetAsyncKeyState(VK_LEFT)  & 0x8000) camYaw -= 0.05f;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) camYaw += 0.05f;
    // Up/Down arrows: zoom the camera in/out toward the character (~2x per
    // second held, the same at any frame rate). Pitch stays on the mouse.
    const float zoomStep = std::exp(lastDt * 0.7f);
    if (GetAsyncKeyState(VK_UP)    & 0x8000) camDist /= zoomStep;
    if (GetAsyncKeyState(VK_DOWN)  & 0x8000) camDist *= zoomStep;
    if (camPitch < kPitchUp) camPitch = kPitchUp;  if (camPitch > kPitchDown) camPitch = kPitchDown;
    // Z: swap the camera to the other shoulder (ActorCameraSideOffset).
    if (GetAsyncKeyState('Z') & 1) camSideTarget = -camSideTarget;
    if (GetAsyncKeyState(VK_PRIOR) & 0x8000) camDist *= 0.95f;
    if (GetAsyncKeyState(VK_NEXT)  & 0x8000) camDist *= 1.05f;
    if (camDist < 100.f) camDist = 100.f;  if (camDist > 1500.f) camDist = 1500.f;

    // Frame-independent dt (seconds) from a HIGH-RES timer (QPC). GetTickCount's
    // ~15ms granularity made dt jump 0<->15ms at high fps = stuttery movement.
    static LARGE_INTEGER qfreq = { 0 }, qlast = { 0 };
    if (qfreq.QuadPart == 0) QueryPerformanceFrequency(&qfreq);
    LARGE_INTEGER qnow; QueryPerformanceCounter(&qnow);
    float dt = qlast.QuadPart ? (float)((double)(qnow.QuadPart - qlast.QuadPart) / qfreq.QuadPart) : 0.016f;
    qlast = qnow;
    if (dt < 0.f) dt = 0.f;  if (dt > 0.033f) dt = 0.033f;   // cap hitches (screenshot etc.) -> brief slow-mo, never a tunneling leap
    lastDt = dt;

    // Desired camera-relative horizontal move (forward = camera-forward, XZ plane).
    float mvX = 0, mvZ = 0;
    if (GetAsyncKeyState('W') & 0x8000) mvZ += 1;
    if (GetAsyncKeyState('S') & 0x8000) mvZ -= 1;
    if (GetAsyncKeyState('A') & 0x8000) mvX -= 1;
    if (GetAsyncKeyState('D') & 0x8000) mvX += 1;
    float inX = mvX, inZ = mvZ;   // raw input dir (for the directional clips)

    // Millisecond clock for input units, state timers AND every gameplay timer
    // below (attack phases, SetDrop, cooldowns, hit-stop, tap/hold), off the
    // same QPC sample as dt. GetTickCount ticks in ~15.6 ms steps, so timed
    // actions landed on a different frame depending on the frame rate.
    const uint32_t nowMs = (uint32_t)((double)qnow.QuadPart * 1000.0 / (double)qfreq.QuadPart);
    unsigned tnow = nowMs;

    // Hit-stop: a brief near-freeze of the whole sim right on a swing's impact
    // frame (STATE:SetHitStop's own first value, real per-weapon data - see
    // EquipWeaponFile). This is what "weight" actually is in an action game;
    // without it every hit reads as the character just waving the weapon
    // through empty air, no matter how good the swing animation itself looks.
    if (tnow < hitStopUntil) dt *= 0.05f;

    // Movement/jump/dodge/gravity/floor-snap physics, ported 1:1 from
    // C:\S4Web2\world.html (moveXZ/hitWall). Wall test = ray vs the precomputed
    // world-space collision triangles (collTris), same pattern (3 side offsets x
    // 3 heights above the climbable-step height) as world.html's THREE.Raycaster
    // version, just against our own triangle list instead of THREE meshes.
    world::MoveState ms;
    ms.x = charPos[0]; ms.y = charPos[1]; ms.z = charPos[2]; ms.yaw = charYaw;
    ms.velY = velY; ms.grounded = grounded; ms.didJump = didJump; ms.jumpMoving = jumpMoving; ms.jumpHadDir = jumpHadDir;
    ms.dodgeVX = dodgeVelX; ms.dodgeVZ = dodgeVelZ; ms.dodgeTimeLeft = dodgeTimeLeft; ms.dodgeDir = dodgeDir;
    ms.wallJumpDelayLeft = wallJumpDelayLeft; ms.wallJumpTimeLeft = wallJumpTimeLeft;
    ms.wallJumpVX = wallJumpVelX; ms.wallJumpVZ = wallJumpVelZ;
    ms.weightEase = moveWeightEase;
    ms.velX = moveVelX; ms.velZ = moveVelZ;

    // An attack moves only through its own animation: every Weapon_*.lua
    // attack declares STATE:SetMoveBoxMultiplier (0 = stand still, 2.0 = the
    // plasma dash), i.e. the swing's MoveBox delta x multiplier - never WASD.
    // That covers right-click actions too: the grounded plasma dash used to
    // add held W/S on top of its MoveBox and went farther or shorter.
    // Grounded only - in the air the jump arc (and SetDrop) owns the motion.
    // Which INPUTS a swing accepts is the state table's call (attack states
    // have no locomotion rows; the counter-sword guard adds its dodge rows).
    // CAttackState::Tick (sub_896290) moves the actor by WASD x base speed x
    // the attack's SetMoveSpeedRate (unset = 0: no WASD at all, e.g. the
    // plasma dash) plus the swing's own MoveBox delta (applied further down).
    // Ground and air alike.
    const bool inAttack = game::ActorFsm::IsAttack(fsm.current()) || atkParam != nullptr || weapon2Active;
    float attackMoveRate = 1.f;
    if (atkParam)
        attackMoveRate = atkStrongMode ? (atkParam == weaponAnim.atkStrong1 ? atkMoveSpeedRateStrong1 : atkMoveSpeedRateStrong)
                                       : atkMoveSpeedRateWeak;
    else if (weapon2Active)
        attackMoveRate = (weapon2IsJump ? jumpAttackLua : weapon2Lua).moveSpeedRate;
    else if (inAttack)
        attackMoveRate = 0.f;
    // In the air an attack stops the travel: no WASD drift (the swing's own
    // MoveBox / SetDrop still move it, e.g. the CS dive).
    if (inAttack && !grounded) attackMoveRate = 0.f;
    // Taking a hit (stun / knocked down / flinch / dead): no control at all.
    const bool reacting = reactKind != REACT_NONE;
    const bool attackLocked = (inAttack && attackMoveRate <= 0.f) || reacting;

    // --- Game::CUserInput -> Common::CStateMachine::Input (src/game) ---
    // Keys become input IDs through the beta's own InputUnit list (W double
    // tap within 500 ms = FASTRUN, A/D held + Space = dodge, ...), and the
    // actor state only changes through the per-state transition tables.
    if (!fsmBound) {
        userInput.BindDefaultLocomotion('A', 'W', 'D', 'S', VK_SPACE, VK_CONTROL);
        fsm.tempo().jumpLandDelayMs   = (uint32_t)(moveCtrl.tuning().jump_landdelay * 1000.f + 0.5f);
        fsm.tempo().wallJumpLandingMs = (uint32_t)(moveCtrl.tuning().wallJumpLandingTime * 1000.f + 0.5f);
        fsm.SetOnEnter([this](anim::ActorState now, anim::ActorState prev) {
            fsmEntered = true;
            // Base OnEnter (sub_7EB7D0): draw or holster the weapon per state.
            switch (game::ActorFsm::WeaponPoseFor(now)) {
            case game::ActorFsm::WEAPON_DRAW:    weaponHolstered = false; break;
            case game::ActorFsm::WEAPON_HOLSTER: weaponHolstered = true;  break;
            default: break;
            }
            if (now == anim::STATE_JUMP && grounded) fsmStartJump = true;   // airborne JUMP (from STANDBY) is just "in the air"
            // CFastRunState::OnEnter (sub_7E02B0): velocity.y <= 0 keeps the
            // fall (horizontal zeroed), velocity.y > 0 zeroes it all - a
            // sprint started mid-jump cancels the rest of the rise instead of
            // carrying the run up through the jump arc.
            if (now == anim::STATE_FASTRUN) fsmKillRise = true;
            // CFastRunState::OnEnter spawns skill_fastrun1.seq on the actor;
            // OnExit (sub_7DFE70 -> sub_7C3D40) removes it.
            if (now == anim::STATE_FASTRUN)
                SpawnSeq("Resources/Effects/skill_fastrun1.seq", charPos[0], charPos[1], charPos[2], charYaw + 3.14159265f, "fastrun");
            else if (prev == anim::STATE_FASTRUN)
                activeEffects.erase(std::remove_if(activeEffects.begin(), activeEffects.end(),
                    [](const ActiveEffect& a) { return a.tag == "fastrun"; }), activeEffects.end());
            // Leaving a dodge early (jump, sprint, attack) ends its slide on
            // the spot; otherwise movement.cpp keeps sliding for the rest of
            // the 600 ms and the new state's motion only starts afterwards.
            const bool wasDodge = prev == anim::STATE_DODGE_LEFT || prev == anim::STATE_DODGE_RIGHT;
            const bool isDodge  = now  == anim::STATE_DODGE_LEFT || now  == anim::STATE_DODGE_RIGHT;
            // The flags cover a change made while input is dispatched (before
            // this frame's movement); the member writes cover one made later
            // in the frame (the attack clicks), which the next frame reads.
            // A ground swing out of a dodge (DODGE rows -> USE_WEAPON1_WEAK /
            // STRONG: the held left click) keeps the rest of the slide - the
            // swing plays while the dodge still carries you. Jump, sprint and
            // the rest still stop it on the spot.
            const bool dodgeToSwing = wasDodge && (now == anim::STATE_USE_WEAPON1_WEAK ||
                                                   now == anim::STATE_USE_WEAPON1_STRONG ||
                                                   now == anim::STATE_USE_WEAPON1_STRONG1);
            if (wasDodge && !isDodge && now != anim::STATE_STANDBY && !dodgeToSwing) {
                fsmCancelDodge = true; dodgeTimeLeft = 0.f; dodgeDir = -1;
            }
            // An attack started in the air cancels what was carrying you:
            // wall-jump flight and dodge slide (not a swing out of a dodge,
            // whose small hop also leaves the ground).
            if (game::ActorFsm::IsAttack(now) && !grounded) {
                fsmCancelWallJump = true; wallJumpTimeLeft = 0.f; wallJumpDelayLeft = 0.f;
                if (!dodgeToSwing) { fsmCancelDodge = true; dodgeTimeLeft = 0.f; dodgeDir = -1; }
            }
            if (now == anim::STATE_DODGE_LEFT || now == anim::STATE_DODGE_RIGHT) {
                fsmStartDodge = (now == anim::STATE_DODGE_LEFT) ? -1 : +1;
                // Sound on the state entering, not on the key: a press the
                // state table refuses (mid-dodge, cooldown) makes no sound,
                // same as the client.
                audio::PlayEffect("Resources/Sound/dodge.ogg", 0.8f);
                // S10 CDodgeState::OnEnter (sub_10A76B0): the dodge's own
                // effect per direction, hung off the actor.
                SpawnSeq(now == anim::STATE_DODGE_LEFT ? "Resources/Effects/Dodge_left.seq"
                                                       : "Resources/Effects/Dodge_right.seq",
                         charPos[0], charPos[1], charPos[2], charYaw + 3.14159265f);
                if (weapon2Active) weapon2Active = false;   // dodge cancels the guard
                gunFiring = false;
                gunFireStart = 0;
                gunLastShotAt = 0;
            }
            DbgLog("[fsm] t=%u %d -> %d grounded=%d", fsmNowMs, (int)prev, (int)now, (int)grounded);
        });
        fsmBound = true;
    }
    // Equipped weapon's own sub-state rows. Counter sword guard
    // (Game::GameSetting::InitCounterSwordGuard): dodge out of the guard,
    // NORMAL input ends it. Other weapons' right-click rows (plasma dash ->
    // STRONG1, bat ...) are not ported yet.
    {
        const bool counter = weaponAnim.atkCounter != nullptr;
        if (counter != fsmCounterRows) {
            if (counter)
                fsm.SetWeaponRows(anim::STATE_USE_WEAPON2, {
                    { game::IN_DODGE_LEFT,  anim::STATE_DODGE_LEFT },
                    { game::IN_DODGE_RIGHT, anim::STATE_DODGE_RIGHT },
                    { game::IN_NORMAL,      anim::STATE_NORMAL } });
            else
                fsm.SetWeaponRows(anim::STATE_USE_WEAPON2, {});
            fsmCounterRows = counter;
        }
    }
    auto airOf = [](bool g, float vy) {
        return g ? game::AIR_GROUND : (vy > 0.f ? game::AIR_RISING : game::AIR_FALLING);
    };
    fsmNowMs = nowMs;
    game::FsmCtx fctx;
    fctx.nowMs = nowMs;
    fctx.air = airOf(grounded, velY);
    fctx.dodgeMoving = dodgeTimeLeft > 0.f;
    fctx.wallJumpMoving = wallJumpDelayLeft > 0.f || wallJumpTimeLeft > 0.f;
    fsmStartJump = false; fsmStartDodge = 0; fsmTryWallJump = false; fsmEntered = false; fsmKillRise = false;
    fsmCancelDodge = false; fsmCancelWallJump = false;
    const auto keyDown = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    for (int id : userInput.Poll(keyDown, nowMs)) {
        // Wall jump (CJumpState::Input, sub_7DC650, on input 13): only a
        // MOVING jump (phases 3/4 - took off with a direction) can bounce,
        // never FALL; CBoundJumpState::Input (sub_7E2B20) allows the chain
        // during its own flight (phases 2/3). The contact test itself - a
        // near-vertical surface (|n.y| < 0.5) within 150 of the actor's
        // height while moving - is movement.cpp's wall probe; BOUNDJUMP is
        // entered below once it confirms the hit.
        if (id == game::IN_JUMP && !grounded &&
            ((fsm.current() == anim::STATE_JUMP && jumpHadDir) ||
             (fsm.current() == anim::STATE_BOUNDJUMP && wallJumpTimeLeft > 0.f)))
            fsmTryWallJump = true;
        if (reacting) continue;   // the reaction owns the actor
        if (id != game::IN_MOVE && id != game::IN_STOP)
            DbgLog("[input] t=%u id=%d state=%d grounded=%d", nowMs, id, (int)fsm.current(), (int)grounded);
        fsm.Input(id, fctx);
    }
    const bool wasGrounded = grounded;
    if (fsmKillRise && ms.velY > 0.f) ms.velY = 0.f;
    if (fsmCancelDodge) { ms.dodgeTimeLeft = 0.f; ms.dodgeDir = -1; }
    if (fsmCancelWallJump) { ms.wallJumpTimeLeft = 0.f; ms.wallJumpDelayLeft = 0.f; }

    world::MoveInput mi;
    mi.mvX = attackLocked ? 0.f : mvX; mi.mvZ = attackLocked ? 0.f : mvZ;
    mi.running = fsm.current() == anim::STATE_FASTRUN;
    mi.crouch  = fsm.current() == anim::STATE_SIT;
    mi.jumpPressed = fsmStartJump || fsmTryWallJump;
    mi.dodgePressed = fsmStartDodge != 0;
    mi.dodgeLeftHeld  = fsmStartDodge < 0;
    mi.dodgeRightHeld = fsmStartDodge > 0;
    mi.camYaw = camYaw;
    // SetDrop's wait: the attack state turns gravity off until the drop fires.
    mi.gravityOff = weapon2Active && weapon2IsJump && jumpAttackLua.loaded && jumpAttackLua.hasDrop &&
                    !weapon2DropApplied && float(tnow - weapon2PhaseStart) < jumpAttackLua.dropTime;
    // Once the drop fired, its Y is the terminal speed (sub_4341B0(drop.y))
    // until the attack state exits and restores max_gravity_velocity.
    mi.maxFallVel = (weapon2Active && weapon2IsJump && weapon2DropApplied && jumpAttackLua.dropY < 0.f)
                    ? jumpAttackLua.dropY : 0.f;
    // Mid-swing move penalty, read straight from the weapon's own
    // STATE:SetMoveSpeedRate() call (EquipWeaponFile) instead of a guess.
    mi.weaponWeightMul = inAttack ? (std::max)(attackMoveRate, 0.f) : 1.f;
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
    // hitWallNormal: returns the XZ face normal of the first hit wall triangle —
    // used by wall-jump so a side-approach rebounds at the correct angle instead
    // of always reflecting backward.
    world::HitNormalFn hitWallNormal = [this](float ox, float oy, float oz,
                                               float dx, float dz, float maxDist,
                                               float& outNx, float& outNz) -> bool {
        float dist = std::sqrt(dx*dx + dz*dz);
        if (dist < 1e-4f) return false;
        float nx_ = dx/dist, nz_ = dz/dist;
        for (const CollTri& t : collTris) {
            float e1[3] = { t.v1[0]-t.v0[0], t.v1[1]-t.v0[1], t.v1[2]-t.v0[2] };
            float e2[3] = { t.v2[0]-t.v0[0], t.v2[1]-t.v0[1], t.v2[2]-t.v0[2] };
            float lo = (std::min)({t.v0[1],t.v1[1],t.v2[1]}), hi = (std::max)({t.v0[1],t.v1[1],t.v2[1]});
            if (oy < lo-1.f || oy > hi+1.f) continue;
            float pvec[3] = { 0.f*e2[2]-dz*e2[1], dz*e2[0]-dx*e2[2], dx*e2[1]-0.f*e2[0] };
            float det = e1[0]*pvec[0]+e1[1]*pvec[1]+e1[2]*pvec[2];
            if (std::fabs(det) < 1e-8f) continue;
            float invDet = 1.f/det;
            float tv[3] = { ox-t.v0[0], oy-t.v0[1], oz-t.v0[2] };
            float u = (tv[0]*pvec[0]+tv[1]*pvec[1]+tv[2]*pvec[2])*invDet;
            if (u < 0.f || u > 1.f) continue;
            float qv[3] = { tv[1]*e1[2]-tv[2]*e1[1], tv[2]*e1[0]-tv[0]*e1[2], tv[0]*e1[1]-tv[1]*e1[0] };
            float v = (dx*qv[0]+0.f*qv[1]+dz*qv[2])*invDet;
            if (v < 0.f || u+v > 1.f) continue;
            float tt = (e2[0]*qv[0]+e2[1]*qv[1]+e2[2]*qv[2])*invDet;
            if (tt > 1e-4f && tt <= maxDist) {
                float fnx = e1[1]*e2[2]-e1[2]*e2[1], fnz = e1[0]*e2[1]-e1[1]*e2[0];
                float fnl = std::sqrt(fnx*fnx+fnz*fnz);
                if (fnl > 1e-6f) { fnx/=fnl; fnz/=fnl; } else { fnx=-nx_; fnz=-nz_; }
                if (fnx*nx_+fnz*nz_ > 0.f) { fnx=-fnx; fnz=-fnz; }
                outNx=fnx; outNz=fnz; return true;
            }
        }
        return false;
    };

    // Keep the stable native path: wall rays + floor ray.
    world::MoveResult mr = moveCtrl.Tick(ms, mi, hitWall, &hitWallNormal, floorAt, dt);


    charPos[0] = ms.x; charPos[1] = ms.y; charPos[2] = ms.z; charYaw = ms.yaw;
    velY = ms.velY; grounded = ms.grounded; didJump = ms.didJump; jumpMoving = ms.jumpMoving; jumpHadDir = ms.jumpHadDir;
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
            // Gravity is off (mi.gravityOff); the velocity is left alone.
        } else if (!weapon2DropApplied) {
            velY = jumpAttackLua.dropY;
            weapon2DropApplied = true;
            // Horizontal part, actor-local (-Z = forward), to world.
            const float a = charYaw + 3.14159265f, c = std::cos(a), s = std::sin(a);
            weapon2DropVelX = jumpAttackLua.dropX * c + jumpAttackLua.dropZ * s;
            weapon2DropVelZ = -jumpAttackLua.dropX * s + jumpAttackLua.dropZ * c;
            DbgLog("[weapon-action] SetDrop fired at %.0fms velocity=(%.0f,%.0f,%.0f)",
                   elapsed, jumpAttackLua.dropX, jumpAttackLua.dropY, jumpAttackLua.dropZ);
        }
        // The drop velocity is the actor's velocity from then on: the
        // horizontal part keeps carrying it until it lands (CS jump attack:
        // forward-down dive; plasma/bat: straight down, X=Z=0).
        if (weapon2DropApplied && !grounded && (weapon2DropVelX != 0.f || weapon2DropVelZ != 0.f)) {
            moveCtrl.MoveXZ(hitWall, ms, weapon2DropVelX * dt, weapon2DropVelZ * dt);
            charPos[0] = ms.x; charPos[2] = ms.z;
        }
    }

    // Self pushes (point-blank wall shots): CPush motion, through MoveXZ so a
    // wall behind still stops it.
    for (size_t i = 0; i < selfPushes.size(); ) {
        SelfPush& sp = selfPushes[i];
        sp.speed += sp.accel * dt;
        sp.msLeft -= dt * 1000.f;
        if (sp.speed <= 0.f || sp.msLeft <= 0.f) { selfPushes.erase(selfPushes.begin() + i); continue; }
        moveCtrl.MoveXZ(hitWall, ms, sp.dirX * sp.speed * dt, sp.dirZ * sp.speed * dt);
        charPos[0] = ms.x; charPos[2] = ms.z;
        ++i;
    }

    // DATA-DRIVEN ANIMATION. Gameplay picks the high-level ActorState; the
    // AnimController runs each ACTION state (jump/fall/dodge/bound) as a full phase
    // SEQUENCE and reports actionBusy() until it finishes — so scenes.cpp LATCHES the
    // action state and holds it to completion instead of cutting it with a timer.
    anim::AnimCtx ac;
    ac.ironBoots = weaponOk && weaponTypeId && *weaponTypeId == 43;   // WEAPONTYPE_IRON_BOOTS
    ac.inX = inX; ac.inZ = inZ; ac.velY = velY; ac.grounded = grounded;
    ac.motionX = ms.velX; ac.motionZ = ms.velZ; ac.headingYaw = charYaw;
    ac.moving   = actuallyMoving;
    ac.speed    = (ms.dodgeTimeLeft > 0.f) ? moveCtrl.tuning().RUN : curSpeed;  // dodge holds run cadence
    ac.refSpeed = animRefSpeed;
    ac.durOf = durOfClip;
    ac.jumpMoving = jumpMoving;              // 00004 move-rise spans the whole flight
    ac.dodgeDir   = dodgeDir;                // which dodge clip (0 front/1 back/2 left/3 right)
    ac.dodgeSlideMs = moveCtrl.tuning().dodgeSlideTime * 1000.f;   // dodge_dodge_time
    ac.dodgeDelayMs = moveCtrl.tuning().dodgeDelay * 1000.f;       // dodge_delay
    ac.jumpTimeMs   = 700.f;                                        // jump_time (game_tempos.x7, all tempo sets)
    ac.weakCombo  = weakCombo;               // which SwordAtkWeakState_<X> row (combo chain)

    // Physics side of the state machine, after this frame's movement:
    //  * a confirmed wall jump enters BOUNDJUMP;
    //  * walking off a ledge is the LEVITATE input (NORMAL/RUN/SIT -> FALL;
    //    states without a row - JUMP, DODGE, FASTRUN - ignore it);
    //  * the state's own Tick runs its timer/landing exits.
    fctx.air = airOf(grounded, velY);
    fctx.dodgeMoving = dodgeTimeLeft > 0.f;
    fctx.wallJumpMoving = wallJumpDelayLeft > 0.f || wallJumpTimeLeft > 0.f;
    if (mr.startedWallJump) {
        fsm.ChangeState(anim::STATE_BOUNDJUMP, fctx);
        DbgLog("[movement] STATE_BOUNDJUMP source=0x008A5B80 delay=%.3f force=%.0f moveTime=%.3f dir=(%.3f,%.3f)",
               moveCtrl.tuning().wallJumpDelay, moveCtrl.tuning().wallJumpForce,
               moveCtrl.tuning().wallJumpTime, ms.wallJumpVX, ms.wallJumpVZ);
    }
    if (mr.startedDodge)
        DbgLog("[movement] DODGE source=0x008A59A0/0x008A57C0 airborne=%d dir=%d duration=%.0fms velocity=(%.3f,%.3f)",
               (int)!grounded, dodgeDir, moveCtrl.tuning().dodgeTime * 1000.f,
               ms.dodgeVX, ms.dodgeVZ);
    if (wasGrounded && !grounded && !mr.startedJump && !mr.startedWallJump)
        fsm.Input(game::IN_LEVITATE, fctx);
    fsm.Tick(fctx);

    // During a swing the body clip is projectoS10's attack playback (curPose is
    // overwritten below), so the AnimController keeps running the last
    // non-attack state underneath - what the swing blends out to when it ends.
    if (!game::ActorFsm::IsAttack(fsm.current())) animBaseState = fsm.current();
    // An attack REPLACES the jump/fall/bound-jump/dodge state in the client:
    // landing out of an air attack never plays the jump's landing (the
    // forward-jump somersault 00000). Left running underneath, the jump
    // sequence reached its landing phase on touchdown and the hand-back
    // faded through the somersault - a roll after every air attack.
    else if (animBaseState == anim::STATE_JUMP || animBaseState == anim::STATE_FALL ||
             animBaseState == anim::STATE_BOUNDJUMP || animBaseState == anim::STATE_DODGE_LEFT ||
             animBaseState == anim::STATE_DODGE_RIGHT)
        animBaseState = anim::STATE_NORMAL;
    const anim::ActorState st = animBaseState;
    // OnEnter replays the state's animation (SetAnimation with RESET), even
    // when the state is entered again.
    if (fsmEntered && !game::ActorFsm::IsAttack(fsm.current())) animCtrl.Restart();

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
    curLowerPoseActive = false;   // set below by RUN's leg half or by an attack with its own leg data

    // RUN is two layers in the client (CRunState sets an UPPER and a LOWER
    // AnimParam per direction). The controller has already picked and blended
    // the LOWER (legs) clip; give the torso the matching UPPER clip at the same
    // tick. Identical halves (swords, bat) change nothing; for the guns this
    // keeps the torso aiming while the legs strafe.
    if (st == anim::STATE_RUN) {
        const auto* rows = animCtrl.StateRows(anim::STATE_RUN);
        if (rows && rows->size() >= 16) {
            auto upperOf = [&](const std::string& lowerClip) -> std::string {
                for (int i = 0; i < 8; ++i)
                    if ((*rows)[i + 8].clip == lowerClip && !(*rows)[i].clip.empty()) return (*rows)[i].clip;
                return lowerClip;
            };
            const std::string ua = upperOf(curPose.clipA);
            const std::string ub = curPose.clipB.empty() ? std::string() : upperOf(curPose.clipB);
            if (ua != curPose.clipA || (!ub.empty() && ub != curPose.clipB)) {
                curLowerPose = curPose;
                curLowerPoseActive = true;
                auto wrap = [&](const std::string& clip, uint32_t tick) -> uint32_t {
                    const float d = durOfClip(clip);
                    return d > 0.f ? (uint32_t)std::fmod((float)tick, d) : tick;
                };
                curPose.clipA = ua; curPose.tickA = wrap(ua, curPose.tickA);
                if (!ub.empty()) { curPose.clipB = ub; curPose.tickB = wrap(ub, curPose.tickB); }
            }
        }
    }

    const bool isDodgeState = st == anim::STATE_DODGE_LEFT || st == anim::STATE_DODGE_RIGHT;
    const unsigned gunPoseMs = gunAnimMs > 0.f ? (unsigned)gunAnimMs : 200u;
    const bool gunFirePoseActive = !isDodgeState && (gunFiring || (gunLastShotAt && tnow - gunLastShotAt < gunPoseMs));

    // Gun fire / reload (UseWeapon1State_<gun>, ReloadState_<gun>): the torso
    // plays the fire blend (AS001 -> AS002 over its blend time) or the reload
    // clip; the legs keep the locomotion clip already picked above.
    if (weaponOk && weaponAnim.isGun && (gunFirePoseActive || gunReloading)) {
        if (!curLowerPoseActive) { curLowerPose = curPose; curLowerPoseActive = true; }
        scn::Pose up = curPose;
        up.clipB.clear(); up.blend = 0.f; up.loopA = true;
        float gunPoseFadeMs = 80.f;
        if (gunReloading && weaponAnim.reload && CharacterHasClip(character, weaponAnim.reload->clip)) {
            up.clipA = weaponAnim.reload->clip; up.loopA = false;
            up.tickA = (uint32_t)((float)(tnow - (gunReloadEnd - gunReloadMs)) * 4.8f);
            gunPoseFadeMs = (float)weaponAnim.reload->blendInMs / 4.8f;
        } else if (gunFirePoseActive) {
            // CCustomAnimBlender::Update (beta sub_723DC0): the aim angle (deg,
            // up +, clamped to +-90 by sub_723A60) picks the half - Upper
            // (A straight up, B level) when > 0, Lower (A straight down, B
            // level) otherwise - and blends toward B by 1 - angle / limit.
            const float viewPitch = (std::max)(kPitchUp, (std::min)(kPitchDown, camPitch + lookPitch));
            const float angle = (std::max)(-90.f, (std::min)(90.f, -viewPitch * 57.2958f));
            const anim::BlendSlot* slot = anim::WeaponFireBlendSlot(luaStates.parsed(), weaponStates);
            const anim::ActorBlend* e = nullptr;
            if (slot) {
                const auto& half = (angle > 0.f || slot->lower.empty()) ? slot->upper : slot->lower;
                // In the air the blender's Jump (rising) / Fly (falling) rows, as the Lua defines them.
                const std::string dir = !grounded ? std::string(velY > 0.f ? "JUMP" : "FLY")
                                      : ac.moving ? anim::RunDir(ac.inX, ac.inZ) : std::string("NORMAL");
                auto it = half.find(dir);
                if (it == half.end()) it = half.find("NORMAL");
                if (it != half.end()) e = &it->second; else if (!half.empty()) e = &half.begin()->second;
            }
            if (e && CharacterHasClip(character, e->clipA)) {
                // The fire clip plays from each shot and holds its last frame.
                const float ms = (float)(tnow - (gunLastShotAt ? gunLastShotAt : gunFireStart));
                // uiAnimPlayTime (InitLua arg 6): the fire clip spans that many ms per shot.
                const float clipTicks = durOfClip(e->clipA);
                const float scale = (gunAnimMs > 0.f && clipTicks > 0.f) ? clipTicks / gunAnimMs : 4.8f;
                up.clipA = e->clipA; up.tickA = (uint32_t)(ms * scale * e->rate); up.loopA = e->loop;
                gunPoseFadeMs = (float)e->blendInMs / 4.8f;
                if (!e->clipB.empty() && CharacterHasClip(character, e->clipB)) {
                    const float bTicks = durOfClip(e->clipB);
                    const float bScale = (gunAnimMs > 0.f && bTicks > 0.f) ? bTicks / gunAnimMs : 4.8f;
                    up.clipB = e->clipB; up.tickB = (uint32_t)(ms * bScale * e->rate); up.loopB = e->loop;
                    up.blend = (std::max)(0.f, (std::min)(1.f, 1.f - angle / (angle > 0.f ? 90.f : -90.f)));
                }
                // Restarted by a shot: blend out of where the previous shot had got to.
                const float sinceShot = (float)(tnow - gunLastShotAt);
                if (gunLastShotAt && gunPrevShotAt && sinceShot < 60.f) {
                    up.fromA = up.clipA; up.fromB = up.clipB;
                    up.fromTickA = (uint32_t)((float)(gunLastShotAt - gunPrevShotAt) * scale * e->rate);
                    up.fromTickB = up.clipB.empty() ? 0u : (uint32_t)((float)(gunLastShotAt - gunPrevShotAt) *
                                   ((gunAnimMs > 0.f && durOfClip(e->clipB) > 0.f) ? durOfClip(e->clipB) / gunAnimMs : 4.8f) * e->rate);
                    up.fromLoopA = up.fromLoopB = false;
                    up.fromBlend = up.blend; up.fromWeight = 1.f - sinceShot / 60.f;
                }
                if (tnow - gunFireStart < 40)
                    DbgLog("[gun] aim pose %s/%s blend=%.2f pitch=%.2f lowerActive=%d", up.clipA.c_str(), up.clipB.c_str(), up.blend, viewPitch, (int)curLowerPoseActive);
            } else if (tnow - gunFireStart < 40) {
                DbgLog("[gun] no aim pose for '%s' (blend %s, clip '%s' on skeleton=%d)", weaponStates.c_str(), e ? "found" : "missing",
                       e ? e->clipA.c_str() : "", e ? (int)CharacterHasClip(character, e->clipA) : 0);
            }
        }
        // ReloadState also owns a lowerPartStateSet. When standing still,
        // NORMAL is the authored waist-down reload clip; when moving,
        // the legs continue walking/running with the locomotion clip.
        if (gunReloading && !ac.moving) {
            auto it = weaponAnim.lowerReload.find("NORMAL");
            if (it != weaponAnim.lowerReload.end() && CharacterHasClip(character, it->second.clip)) {
                const float reloadMs = (float)(tnow - (gunReloadEnd - gunReloadMs));
                curLowerPose.clipA = it->second.clip; curLowerPose.clipB.clear(); curLowerPose.blend = 0.f;
                curLowerPose.loopA = it->second.loop;
                curLowerPose.tickA = (uint32_t)(reloadMs * 4.8f * it->second.rate);
            }
        }
        // Legs while firing: the fire state's lowerPartStateSet (E0008.. by
        // direction while walking, AS002 standing) instead of the plain walk.
        // lowerPartStateSet only has walking rows: in the air the legs keep the
        // jump / fall clip (the blender's Jump/Fly rows drive the torso).
        if (gunFirePoseActive && !gunReloading && grounded) {
            const auto lower = anim::WeaponFireLower(luaStates.parsed(), weaponStates);
            auto it = lower.find(ac.moving ? anim::RunDir(ac.inX, ac.inZ) : std::string("NORMAL"));
            if (it == lower.end()) it = lower.find("NORMAL");
            if (it != lower.end() && CharacterHasClip(character, it->second.clip)) {
                curLowerPose.clipA = it->second.clip; curLowerPose.clipB.clear(); curLowerPose.blend = 0.f;
                curLowerPose.loopA = it->second.loop;
                const float legMs = it->second.reset ? (float)(tnow - gunFireStart) : (float)tnow;
                curLowerPose.tickA = (uint32_t)(legMs * 4.8f * it->second.rate);
            }
            static unsigned lastLegLog = 0;
            if (tnow - lastLegLog > 500) {
                lastLegLog = tnow;
                DbgLog("[gun] legs moving=%d dir=%s rows=%zu -> '%s' (on skeleton=%d) state=%d", (int)ac.moving,
                       ac.moving ? anim::RunDir(ac.inX, ac.inZ).c_str() : "NORMAL", lower.size(),
                       it != lower.end() ? it->second.clip.c_str() : "(none, keeps locomotion)",
                       it != lower.end() ? (int)CharacterHasClip(character, it->second.clip) : 0, (int)st);
            }
        }
        // Fade in from what was on screen (80 ms) so the gun comes up smoothly.
        const float inMs = (float)(tnow - (gunReloading ? gunReloadEnd - gunReloadMs : gunFireStart));
        if (gunPoseFadeMs > 0.f && inMs < gunPoseFadeMs && up.fromWeight <= 0.f) {
            up.fromA = curPose.clipA; up.fromB = curPose.clipB;
            up.fromTickA = curPose.tickA; up.fromTickB = curPose.tickB;
            up.fromLoopA = curPose.loopA; up.fromLoopB = curPose.loopB;
            up.fromBlend = curPose.blend; up.fromWeight = 1.f - inMs / gunPoseFadeMs;
        }
        curPose = up;
        // In the air the fire state has no leg rows of its own: the legs keep a
        // jump pose - the STANDING jump's rise (JUMP slot 1) / fall (slot 5).
        // The moving jump (slot 2) is a forward somersault and must not play
        // while shooting.
        if (gunFirePoseActive && !gunReloading && !grounded) {
            const std::vector<anim::SAnimParam>* rows = animCtrl.StateRows(anim::STATE_JUMP);
            const size_t slot = velY > 0.f ? 1u : 5u;
            if (rows && slot < rows->size() && CharacterHasClip(character, (*rows)[slot].clip)) {
                const anim::SAnimParam& jp = (*rows)[slot];
                curLowerPose.clipA = jp.clip; curLowerPose.clipB.clear(); curLowerPose.blend = 0.f;
                curLowerPose.loopA = jp.loop;
                curLowerPose.tickA = (uint32_t)((float)(tnow - gunFireStart) * 4.8f * jp.rate);
                curLowerPoseActive = true;
            }
        }
    }

    // Weapon swings run independently of the locomotion/jump/fall/dodge state
    // above and just overwrite curPose when one is active - you can throw a hit
    // mid-air just as well as on the ground.
    auto startAtk = [&](const anim::ActorAnim* param, bool strong) {
        if (!param || tnow < atkCooldownUntil || atkParam || weapon2Active || reactKind != REACT_NONE) return;
        atkParam = param; atkStrongMode = strong; atkHitDone = false; atkStart = tnow;
        // This swing's CreateCAttack list (weapon_*.lua): hits by shape and time.
        {
            const std::vector<luaw::WeaponAttack>* list = param == weaponAnim.atkStrong1 ? &atkLuaStrong1
                : (strong || param == weaponAnim.atkCounter) ? &atkLuaStrong : &atkLuaWeak;
            // Combo hit N (N >= 2) of a weapon whose rows give it its own function.
            if (!strong && !weaponAnim.atkWeak.empty() && param > &weaponAnim.atkWeak.front() && param <= &weaponAnim.atkWeak.back()) {
                const size_t hit = (size_t)(param - &weaponAnim.atkWeak.front());
                if (hit - 1 < atkLuaCombo.size() && !atkLuaCombo[hit - 1].empty()) list = &atkLuaCombo[hit - 1];
            }
            StartSwingAttacks(list, tnow, strong ? 15.f : 8.f);
        }
        atkBlendFrom = curPose;   // whatever was playing this frame - blended into the swing below
        float rawClipDur = durOfClip(param->clip);
        float dur = float(param->startMs) + rawClipDur;
        if (dur <= 0.f) dur = 500.f;
        float speedScale = (std::max)(animCtrl.speedScale, 0.1f);
        // Held-click weapons (spy dagger) chain swing after swing; the extra
        // 200 ms gap made the held attack stall between hits.
        const bool heldRepeat = weaponStates.find("SpyDagger") != std::string::npos;
        atkCooldownUntil = tnow + (unsigned)(dur / speedScale) + (heldRepeat ? 0u : 200u);
        attackLockUntil = tnow + (unsigned)(std::max)(500.f, dur / speedScale);
        DbgLog("[atk] strong=%d clip='%s' startMs=%d rawClipDurTicks=%.0f dur=%.0f speedScale=%.2f -> realMsToFinish=%.0f (0 rawClipDurTicks = clip not found on character skeleton)",
               (int)strong, param->clip.c_str(), param->startMs, rawClipDur, dur, speedScale, dur / speedScale);

        const bool jumpWeak = param == weaponAnim.jumpAtkWeak;
        // The state function behind this swing: everything it declares plays.
        const anim::ActorFunc* stateFn = jumpWeak ? weaponAnim.fnJumpWeak
            : param == weaponAnim.atkStrong1 ? weaponAnim.fnStrong1
            : param == weaponAnim.atkCounter ? weaponAnim.fnCounter
            : strong ? weaponAnim.fnStrong : nullptr;
        if (!stateFn && !weaponAnim.atkWeak.empty() && param >= &weaponAnim.atkWeak.front() && param <= &weaponAnim.atkWeak.back()) {
            const size_t hit = (size_t)(param - &weaponAnim.atkWeak.front());
            if (hit < weaponAnim.fnWeakHits.size()) stateFn = weaponAnim.fnWeakHits[hit];
        }
        const anim::ActorSeq* fx = stateFn ? nullptr : jumpWeak ? weaponAnim.seqJumpAtkWeak : (strong ? weaponAnim.seqStrong : weaponAnim.seqWeak);
        const anim::ActorSound* snd = stateFn ? nullptr : jumpWeak ? weaponAnim.sndJumpAtkWeak : (strong ? weaponAnim.sndStrong : weaponAnim.sndWeak);
        if (snd) audio::PlayEffect(female ? snd->girl : snd->man, 0.85f, (unsigned)(std::max)(0, snd->start));
        if (weaponAnim.isGun && !weaponAnim.gunBlastSound.empty()) audio::PlayEffect(weaponAnim.gunBlastSound, 0.9f);
        {
            const auto& seqs = (param == weaponAnim.atkStrong1) ? atkSeqStrong1 : (strong ? atkSeqStrong : atkSeqWeak);
            for (const auto& sq : seqs)
                pendingSeqs.push_back({ tnow + (unsigned)(std::max)(0.f, sq.startMs), sq.file, sq.sound });
        }
        // The state's own AddSequence: .seq + sound `start` ms in (as weapon 2 below).
        if (fx) pendingSeqs.push_back({ tnow + (unsigned)(std::max)(0, fx->startMs), (female && !fx->seqGirl.empty()) ? fx->seqGirl : fx->seq, fx->sound });   // AddSequenceBySex: the female version
        if (stateFn) PlayStateFx(stateFn, tnow);
        // <other_effects other_type="counter_sword" attack_seq/attack_ogg>: the
        // counter sword's slash on its held swing and its counter attack, when the
        // first attack lands - only on a counter sword (item <ability type> 1):
        // Iron Boots items (type 43) carry the same block copied over.
        const bool isCounterSword = weaponTypeId && *weaponTypeId == 1;
        if (isCounterSword && (strong || (param == weaponAnim.atkCounter && weaponAnim.atkCounter)) && !weaponAttackSeq.empty()) {
            float at = 0.f;
            if (swingAttacks && !swingAttacks->empty()) at = swingAttacks->front().fireTime;
            pendingSeqs.push_back({ tnow + (unsigned)at, "Resources/Effects/" + weaponAttackSeq,
                                    weaponAttackOgg.empty() ? std::string() : "Resources/Sound/" + weaponAttackOgg });
        }
        else StartGhostTrail(jumpWeak ? weaponAnim.ghostJumpAtkWeak
                        : param == weaponAnim.atkStrong1 ? weaponAnim.ghostStrong1
                        : param == weaponAnim.atkCounter ? weaponAnim.ghostCounter
                        : strong ? weaponAnim.ghostStrong : weaponAnim.ghostWeak);
    };

    // Checked BEFORE the FSM is told: a refused start used to leave the FSM in
    // USE_WEAPON1_JUMP (44) with nothing playing, so the jump attack "cut" in
    // the air after one frame.
    auto canStartWeapon2 = [&](bool aerial) {
        const auto& phases = aerial ? weaponAnim.jumpAtk : weaponAnim.atkWeapon2;
        const bool ok = weaponOk && !phases.empty() && !atkParam && !weapon2Active && tnow >= atkCooldownUntil && reactKind == REACT_NONE;
        if (!ok) DbgLog("[weapon-action] refused aerial=%d weaponOk=%d phases=%zu atk=%d w2=%d cooldown=%dms", (int)aerial, (int)weaponOk,
                        phases.size(), (int)(atkParam != nullptr), (int)weapon2Active, (int)(atkCooldownUntil > tnow ? atkCooldownUntil - tnow : 0));
        return ok;
    };
    auto startWeapon2 = [&](bool aerial) {
        const auto& phases = aerial ? weaponAnim.jumpAtk : weaponAnim.atkWeapon2;
        if (!weaponOk || phases.empty() || atkParam || weapon2Active || tnow < atkCooldownUntil) return;
        weapon2Active = true; weapon2IsJump = aerial;
        weapon2DropApplied = false; weapon2DropVelX = weapon2DropVelZ = 0.f;
        weapon2MoveBoxValid = false; weapon2MoveBoxClip.clear();
        atkBlendFrom = curPose;
        weapon2Phase = 0; weapon2PhaseStart = tnow; rightHeldAt = tnow; weapon2PeerHitDone = false;
        StartSwingAttacks(aerial ? &jumpAttacks : &weapon2Attacks, tnow, 15.f);
        // STATE:EnableCameraShake(true, whenSuccess, ms): the state's own shake
        // at that time (plasma jump attack: 600 ms, on landing).
        {
            const luaw::WeaponFireState& w = aerial ? jumpAttackLua : weapon2Lua;
            if (w.cameraShake.size() >= 3 && w.cameraShake[0] != 0.f && w.cameraShake[1] == 0.f) {
                shakePendingAt = GetTickCount() + (unsigned)w.cameraShake[2];
                shakePendingAmp = w.cameraShakeF1.size() >= 3 ? w.cameraShakeF1[2] : 30.f;
            }
        }
        for (const auto& sq : (aerial ? jumpAttackLua.sequences : weapon2Lua.sequences))
            pendingSeqs.push_back({ tnow + (unsigned)(std::max)(0.f, sq.startMs), sq.file, sq.sound });
        // The state function's own declarations (sounds, effects, effects on
        // nodes, trail). AddSequence(index, seq, sound, start[, range]): the .seq
        // and its sound start `start` ms in, where the actor is then (plasma dash
        // 500 ms in; counter sword jump attack lands at 400, its effect at 500).
        PlayStateFx(aerial ? weaponAnim.fnJump : weaponAnim.fnWeapon2, tnow);
        // Shape change (sigma blade right click): its own effect now; the weapon
        // switches form once the action is over.
        if (!aerial && weaponAnim.weaponChange) {
            weaponChangePending = true;
            const std::string& fxFile = anim::ShapeChange() ? weaponChangeOffSeq : weaponChangeOnSeq;
            if (!fxFile.empty()) SpawnSeq("Resources/Effects/" + fxFile, charPos[0], charPos[1], charPos[2], charYaw + 3.14159265f);
        }
        DbgLog("[weapon-action] enter aerial=%d phase=0 clip='%s' rate=%.2f loop=%d",
               (int)aerial, phases[0].clip.c_str(), phases[0].rate, (int)phases[0].loop);
    };

    // Tap vs hold threshold for the left button: the client's
    // CKeyUpInput(USE_WEAPON1_END_SHORT, 200) / CKeyPressingInput1(_END_LONG, 200).
    const unsigned kHoldMs = 200;
    // Every swing goes through the state machine first: the current state's
    // table decides whether this click is an attack at all and which attack
    // state it is (WEAK/STRONG on the ground, USE_WEAPON1_JUMP in the air,
    // nothing from STANDBY or from inside another swing). Only a click the
    // table accepts starts projectoS10's swing playback.
    auto fsmAttack = [&](int inputId) {
        const anim::ActorState before = fsm.current();
        fsm.Input(inputId, fctx);
        return fsm.current() != before && game::ActorFsm::IsAttack(fsm.current());
    };
    const bool canSwing = weaponOk && !atkParam && !weapon2Active && tnow >= atkCooldownUntil;
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
        // The right click is the weapon's ground action (plasma dash). Its
        // CAttackState::CanEnter (sub_895870) refuses it in the air when the
        // Lua InitLua's 4th arg (onlyVStateNormal, +345) is true - the plasma
        // dash's is. The jump attack (stun) is the LEFT click.
        const bool onlyGround = weapon2Lua.initArgs.size() >= 4 && weapon2Lua.initArgs[weapon2Lua.initTimed ? 3 : 2] != 0.f;
        bool started = false;
        if (canSwing && !weaponAnim.atkWeapon2.empty() && (grounded || !onlyGround) &&
            fsmAttack(game::IN_USE_WEAPON2)) {
            startWeapon2(false);
            started = true;
        }
        DbgLog("[rmb] t=%u state=%d grounded=%d onlyGround=%d canSwing=%d -> %s",
               tnow, (int)fsm.current(), (int)grounded, (int)onlyGround, (int)canSwing,
               started ? "weapon2" : "ignored");
    }
    if (!rmbDown && rmbWasDown) { rightHeld = false; lastWeapon2At = tnow; }
    rmbWasDown = rmbDown;

    if (weaponOk && weaponAnim.isGun) {
        const anim::ActorState gs = fsm.current();
        if (gunReloading && tnow >= gunReloadEnd) {
            gunReloading = false;
            gunAmmo = gunMag;
        }
        const bool rDown = (GetAsyncKeyState('R') & 0x8000) != 0;
        const bool wantReload = (rDown && !gunRWasDown && gunAmmo < gunMag) || (lmbDown && gunAmmo <= 0);
        gunRWasDown = rDown;
        if (!gunReloading && wantReload) {
            gunReloading = true; gunFiring = false;
            gunReloadEnd = tnow + gunReloadMs;
            if (!gunReloadSound.empty()) audio::PlayEffect("Resources/Sound/" + gunReloadSound, 0.9f);
        }
        const bool canShoot = gs != anim::STATE_FASTRUN && gs != anim::STATE_DODGE_LEFT && gs != anim::STATE_DODGE_RIGHT &&
                              reactKind == REACT_NONE;
        const bool firing = lmbDown && !gunReloading && gunAmmo > 0 && canShoot;
        if (firing && !gunFiring) { gunFireStart = tnow; gunLastShotAt = 0; gunPrevShotAt = 0; }
        gunFiring = firing;
        auto fireRound = [&](unsigned at) {
            FireGunShot(at);
            // Every shot plays the weapon's fire clip again on the body; the
            // pose it was in is blended out over 60 ms (no snap to frame 0).
            gunPrevShotAt = gunLastShotAt;
            gunLastShotAt = at;
            --gunAmmo;
        };
        if (gunFiring && tnow >= gunNextShotAt) {
            const unsigned cycle = (unsigned)(1000.f / (std::max)(gunRate, 0.1f));
            if (gunRayGroups <= 1) {
                fireRound(tnow);
                gunNextShotAt = tnow + cycle;
            } else {
                // A burst: every round at its own fireTime, one after another.
                float span = 0.f;
                for (int g = 0; g < gunRayGroups && g < (int)gunRoundOffsets.size(); ++g) {
                    gunPendingRounds.push_back({ tnow + (unsigned)gunRoundOffsets[g], g });
                    span = (std::max)(span, gunRoundOffsets[g]);
                }
                const float spacing = gunRayGroups > 1 ? span / (float)(gunRayGroups - 1) : 0.f;
                gunNextShotAt = tnow + (std::max)(cycle, (unsigned)(span + spacing));   // no overlap: the next burst follows the last round
            }
        }
        if (!gunFiring) gunPendingRounds.clear();   // letting go of the trigger ends the burst
        for (size_t i = 0; i < gunPendingRounds.size(); ) {
            if (tnow < gunPendingRounds[i].at) { ++i; continue; }
            const int g = gunPendingRounds[i].group;
            gunPendingRounds.erase(gunPendingRounds.begin() + (long)i);
            if (gunAmmo > 0 && !gunReloading) { gunForceGroup = g; fireRound(tnow); gunForceGroup = -1; }
            else gunPendingRounds.clear();
        }
    }
    if (weaponOk && !weaponAnim.isGun) {
        // Holding past the threshold fires the strong swing while the button is
        // still down - standing still uses the stand variant when the weapon has
        // one, and being airborne swaps in the jump attack.
        const anim::ActorAnim* strongPick = (!actuallyMoving && weaponAnim.atkStrong1) ? weaponAnim.atkStrong1 : weaponAnim.atkStrong;
        // Left click in the air (JUMP/FALL/BOUNDJUMP rows -> USE_WEAPON1_JUMP):
        // the weapon's jump attack - plasma: the stun dive - going forward or
        // with no direction held; sideways/backwards the weak jump attack
        // (A0015, S10's SwordJumpAtkWeakState). Held or tapped alike.
        // The stun (jump attack) only comes from a jump taken with no
        // direction (jump in place + left click) or from a wall jump
        // (BOUNDJUMP + left click); any directional jump gets the weak jump
        // attack instead.
        const bool stunDir = fsm.current() == anim::STATE_BOUNDJUMP || !jumpHadDir;
        // Air vs ground swing is decided by the STATE, like the client's
        // tables: JUMP/FALL/BOUNDJUMP send the click to USE_WEAPON1_JUMP, but
        // DODGE sends it to USE_WEAPON1_WEAK/STRONG even while the dodge has
        // you off the ground - a held click out of a dodge is the strong
        // swing, never the stun.
        const anim::ActorState cs = fsm.current();
        const bool airState = cs == anim::STATE_JUMP || cs == anim::STATE_FALL || cs == anim::STATE_BOUNDJUMP;
        // S10: only a HELD left click, standing-jump or forward, is the stun
        // (jump attack); a tap - or a held click sideways/back - is the weak
        // jump attack (A0015). The 2008 beta sends both to the jump attack.
        auto airAttack = [&](int inputId, bool held) {
            if (!canSwing) return;
            // Weapons with no weak jump attack (spy dagger, ...) send every
            // left click in the air to their jump attack, as the client does
            // (USE_WEAPON1_JUMP). Falling back to the first GROUND swing
            // played a standing stab mid-air.
            // Spy dagger: no air attack of its own - a left click in the air is
            // the same alternating SN013/SN014 stab as on the ground.
            if (repeatWeak && !weaponAnim.atkWeak.empty()) {
                if (fsmAttack(inputId)) {
                    weakCombo = (weakCombo + 1) % (int)(std::min)(weaponAnim.atkWeak.size(), (size_t)2);
                    lastWeakAt = tnow;
                    startAtk(&weaponAnim.atkWeak[weakCombo], false);
                }
                return;
            }
            // S10 UseWeapon1_Jump (actorstates_gameplay.lua): STATE_USE_WEAPON1_JUMP
            // loads only SwordJumpAtkState_<weapon> (plasma: the stun); the
            // SwordJumpAtkWeakState_* functions are defined but no table uses
            // them. Any left click in the air is the jump attack.
            (void)held; (void)stunDir;
            if (!weaponAnim.jumpAtk.empty()) {
                if (canStartWeapon2(true) && fsmAttack(inputId)) startWeapon2(true);
                return;
            }
            const anim::ActorAnim* p = weaponAnim.jumpAtkWeak ? weaponAnim.jumpAtkWeak
                                      : (weaponAnim.atkWeak.empty() ? nullptr : &weaponAnim.atkWeak[0]);
            if (p && fsmAttack(inputId)) startAtk(p, false);
        };
        if (wantStrong && !atkParam) {
            if (airState) airAttack(game::IN_USE_WEAPON1_STRONG, true);
            else if (canSwing && strongPick && fsmAttack(game::IN_USE_WEAPON1_STRONG)) startAtk(strongPick, true);
            attackFired = true;
        }
        if (wantWeak && !atkParam && airState) { airAttack(game::IN_USE_WEAPON1_WEAK, false); wantWeak = false; }
        // A click during a combo hit queues the next one (counter sword
        // AfterDash1 -> 2 -> 3 -> 4, plasma FIRST -> SECOND -> THIRD): it
        // starts once this hit's attack time (InitLua normal) is over.
        if (wantWeak && atkParam && !atkStrongMode && !repeatWeak && weaponAnim.atkWeak.size() > 1 &&
            atkParam >= &weaponAnim.atkWeak.front() && atkParam <= &weaponAnim.atkWeak.back()) {
            weakQueued = true; wantWeak = false;
        }
        if (wantWeak && !atkParam && canSwing && fsmAttack(game::IN_USE_WEAPON1_WEAK)) {
            // A weak hit thrown right after letting go of guard (right click) is a
            // counter instead of the next combo step.
            // Air states were handled above (airAttack); here it is a ground
            // or dodge state, so the regular weak combo.
            if (weaponAnim.atkCounter && tnow - lastWeapon2At < 800) {
                startAtk(weaponAnim.atkCounter, false);
            } else {
                // CAttackSpyDaggerState::OnEnter (S10 sub_106BC80) flips
                // NORMAL_FIRST/NORMAL_SECOND on every entry, however long ago
                // the last stab was; the other weapons chain within 600 ms.
                if (repeatWeak && !weaponAnim.atkWeak.empty())
                    weakCombo = (weakCombo + 1) % (int)(std::min)(weaponAnim.atkWeak.size(), (size_t)2);
                else
                    weakCombo = (tnow - lastWeakAt < 600 && !weaponAnim.atkWeak.empty()) ? (weakCombo + 1) % (int)weaponAnim.atkWeak.size() : 0;
                lastWeakAt = tnow;
                startAtk(weaponAnim.atkWeak.empty() ? nullptr : &weaponAnim.atkWeak[weakCombo], false);
            }
        }
        if (atkParam) {
            const bool isS1 = atkStrongMode && atkParam == weaponAnim.atkStrong1;
            const float normalMs = atkStrongMode ? (isS1 ? atkNormalMsStrong1 : atkNormalMsStrong) : atkNormalMsWeak;
            const float delayMs  = atkStrongMode ? (isS1 ? atkDelayMsStrong1 : atkDelayMsStrong) : atkDelayMsWeak;
            const float clipTicks = durOfClip(atkParam->clip);
            const float sinceMs = float(tnow - atkStart);
            float startDelay = float(atkParam->startMs);
            float t, tick; bool ended;
            if (normalMs > 0.f && clipTicks > 0.f) {
                // CAttackState: the state lasts InitLua normal + delay; ApplyAnim
                // (sub_8AEAE0) fits the clip to normalMs: speed = ref * rate /
                // normalMs, ref = SetAnim startMs when set, else the clip's ms.
                const float refMs = atkParam->startMs > 0 ? float(atkParam->startMs) : clipTicks / animCtrl.speedScale;
                t = sinceMs * animCtrl.speedScale * refMs * atkParam->rate / normalMs;
                tick = atkParam->loop ? t : (std::min)(t, clipTicks);
                startDelay = 0.f;
                ended = sinceMs >= normalMs + delayMs;
            } else {
                float dur = startDelay + clipTicks;
                if (dur <= 0.f) dur = 500.f;
                t = sinceMs * animCtrl.speedScale * atkParam->rate;
                tick = (std::max)(0.f, t - startDelay);
                ended = t >= dur;
            }
            if (!atkHitDone && t >= startDelay) {
                atkHitDone = true;
                // Online: whoever stands in front of the swing is hit.
                if (!swingAttacks) HitPeersInCone(atkStrongMode ? 330.f : 280.f, atkStrongMode ? 15.f : 8.f, atkStrongMode ? 900.f : 400.f);
                float hs = atkStrongMode ? (isS1 ? atkHitStopStrong1 : atkHitStopStrong) : atkHitStopWeak;
                if (hs > 0.f) hitStopUntil = tnow + (unsigned)hs;
            }
            // This combo hit and its DELAY row (recovery) after the attack time.
            const bool weakHit = !atkStrongMode && !weaponAnim.atkWeak.empty() &&
                                 atkParam >= &weaponAnim.atkWeak.front() && atkParam <= &weaponAnim.atkWeak.back();
            const size_t hitIdx = weakHit ? (size_t)(atkParam - &weaponAnim.atkWeak.front()) : 0;
            const anim::ActorAnim* delayRow = (weakHit && hitIdx < weaponAnim.atkWeakDelay.size() &&
                                               !weaponAnim.atkWeakDelay[hitIdx].clip.empty()) ? &weaponAnim.atkWeakDelay[hitIdx] : nullptr;
            const bool pastAttack = normalMs > 0.f ? sinceMs >= normalMs : (clipTicks > 0.f && t >= startDelay + clipTicks);
            if (weakHit && weakQueued && pastAttack) {
                // The queued click: next hit now, from where this one is.
                weakQueued = false;
                weakCombo = (int)((hitIdx + 1) % weaponAnim.atkWeak.size());
                lastWeakAt = tnow;
                atkParam = nullptr; atkCooldownUntil = 0;
                startAtk(&weaponAnim.atkWeak[weakCombo], false);
                ended = false; tick = 0.f;
            }
            if (ended) {
                if (weakHit) lastWeakAt = tnow;   // the chain window counts from the end of the hit
                weakQueued = false;
                atkParam = nullptr; fastrunLockoutUntil = tnow + 120;
            }
            else if (delayRow && pastAttack && atkParam && !weakQueued) {
                // Recovery: the DELAY row over the delay time.
                const float dt = (sinceMs - (normalMs > 0.f ? normalMs : 0.f)) * animCtrl.speedScale * delayRow->rate;
                const float dd = durOfClip(delayRow->clip);
                curPose.clipA = delayRow->clip; curPose.tickA = (uint32_t)(dd > 0.f ? (std::min)(dt, dd) : dt);
                curPose.loopA = false;
                curPose.clipB.clear(); curPose.blend = 0.f;
            }
            else {
                curPose.clipA = atkParam->clip; curPose.tickA = (uint32_t)tick;
                curPose.loopA = atkParam->loop;
                // The fade in from the previous pose is the scene-level source
                // fade at the end of Update (blendIn of this row + blendOut of
                // what was playing).
                curPose.clipB.clear(); curPose.blend = 0.f;
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
                        const anim::ActorAnim& lp = lit->second;
                        const float dtMs = atkLegLastMs ? float(tnow - atkLegLastMs) : 0.f;
                        atkLegLastMs = tnow;
                        const bool newSwing = atkLegAttackStart != atkStart;
                        atkLegAttackStart = atkStart;
                        // The leg layer carried over from the previous swing:
                        // the source fade must leave the legs alone.
                        if (newSwing) atkLegCarried = !atkLegClip.empty();
                        // SetAnimation on the leg layer: a different clip, or the
                        // same RESET_TRUE row re-applied by a new swing, fades
                        // over prev blendOut + blendIn ticks; RESET_FALSE keeps
                        // the running clip untouched.
                        if (lp.clip != atkLegClip || (newSwing && lp.reset)) {
                            if (!atkLegClip.empty()) {
                                atkLegPrevClip = atkLegClip; atkLegPrevClock = atkLegClock; atkLegPrevLoop = atkLegLoop;
                                atkLegFadeTicks = float(atkLegBlendOut + lp.blendInMs); atkLegFadeElapsed = 0.f;
                            }
                            if (lp.clip != atkLegClip || lp.reset) atkLegClock = lp.reset ? 0.f : atkLegClock;
                            atkLegClip = lp.clip; atkLegLoop = lp.loop; atkLegBlendOut = lp.blendOutMs;
                        }
                        // Walk legs follow the actual ground speed like the RUN
                        // layer does, so they do not skate at the reduced
                        // swing speed (moveSpeedRate).
                        const float warp = (actuallyMoving && animRefSpeed > 1.f)
                            ? (std::max)(0.05f, (std::min)(curSpeed / animRefSpeed, 8.f)) : 1.f;
                        const float adv = dtMs * animCtrl.speedScale * lp.rate * warp;
                        atkLegClock += adv; atkLegPrevClock += adv; atkLegFadeElapsed += dtMs * animCtrl.speedScale;
                        auto wrapTick = [&](const std::string& c, float t, bool loop) -> uint32_t {
                            const float d = durOfClip(c);
                            if (d <= 0.f) return (uint32_t)t;
                            return (uint32_t)(loop ? std::fmod(t, d) : (std::min)(t, d));
                        };
                        if (atkLegFadeTicks > 0.f && atkLegFadeElapsed < atkLegFadeTicks && !atkLegPrevClip.empty()) {
                            curLowerPose.clipA = atkLegPrevClip; curLowerPose.tickA = wrapTick(atkLegPrevClip, atkLegPrevClock, atkLegPrevLoop);
                            curLowerPose.loopA = atkLegPrevLoop;
                            curLowerPose.clipB = atkLegClip; curLowerPose.tickB = wrapTick(atkLegClip, atkLegClock, atkLegLoop);
                            curLowerPose.loopB = atkLegLoop;
                            curLowerPose.blend = atkLegFadeElapsed / atkLegFadeTicks;
                        } else {
                            curLowerPose.clipA = atkLegClip; curLowerPose.tickA = wrapTick(atkLegClip, atkLegClock, atkLegLoop);
                            curLowerPose.loopA = atkLegLoop;
                            curLowerPose.clipB.clear(); curLowerPose.blend = 0.f;
                        }
                        float ldur = durOfClip(lp.clip);
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
            // Online: the action hits whoever is in front once its wind-up is
            // past (phase 1, or 250 ms into a single-phase action).
            if (!weapon2PeerHitDone && (weapon2Phase >= 1 || tnow - weapon2PhaseStart >= 250)) {
                weapon2PeerHitDone = true;
                if (!swingAttacks) HitPeersInCone(330.f, 15.f, 900.f);
            }
            float realDur = durOfClip(p->clip);
            float dur = realDur; if (dur <= 0.f) dur = 500.f;
            float since = float(tnow - weapon2PhaseStart);
            // InitLua(normalMs, delayMs, ...) owns attack-state timing. Native
            // ApplyAnim 0x008AEAE0 fits each clip to that state window:
            // playbackRate = clipDuration * SetAnim.rate / stateWindowMs.
            const luaw::WeaponFireState& actionLua = weapon2IsJump ? jumpAttackLua : weapon2Lua;
            float phaseMs = 0.f;
            if (actionLua.loaded && actionLua.initTimed && weapon2Phase < (int)actionLua.initArgs.size())
                phaseMs = actionLua.initArgs[weapon2Phase];
            float ticks = phaseMs > 0.f ? since * dur * p->rate / phaseMs
                                        : since * animCtrl.speedScale * p->rate;
            bool lastPhase = (weapon2Phase == (int)phases.size() - 1);
            // Aerial NORMAL keeps its last key until contact; then DELAY plays
            // the authored recovery. Releasing RMB is not a cancellation event.
            bool phaseDone = phaseMs > 0.f ? since >= phaseMs : ticks >= dur;
            // Jump attack: the dive phase (A0018 + SetDrop) holds until the
            // actor is on the ground; only then the impact phase (A0019, the
            // slam / stun) plays. Advancing on the 400 ms timer alone played
            // the slam mid-air - the higher the jump, the earlier.
            if (weapon2IsJump && weapon2Phase == 0 && !grounded) phaseDone = false;
            // A held loop phase that is not the last (counter sword guard:
            // BEGIN R0005 loop, then DELAY R0032) lasts exactly while the
            // right button is down; letting go moves on to the recovery.
            if (!lastPhase && p->loop && !rightHeld && !weapon2IsJump) phaseDone = true;
            if (!lastPhase && (!p->loop || !rightHeld) && phaseDone) {
                weapon2Phase++;
                weapon2PhaseStart = tnow;
                p = &phases[weapon2Phase];
                realDur = durOfClip(p->clip);
                dur = realDur; if (dur <= 0.f) dur = 500.f;
                phaseMs = (actionLua.loaded && actionLua.initTimed && weapon2Phase < (int)actionLua.initArgs.size())
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
                // Phase-to-phase and in/out fades: scene-level source fade.
                curPose.clipB.clear(); curPose.blend = 0.f;
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
    // Same rule for EVERY attack (CAttackState, sub_896290): weak/strong/strong1
    // swings, right-click actions and jump attacks, ground or air, each with
    // its own STATE:SetMoveBoxMultiplier (0 = the swing does not travel).
    float moveBoxMul = 0.f;
    // CAttackSpyDaggerState (S10) moves by the MoveBox only when the swing's
    // Lua calls SetUseMoveBox(true): the left-click stab does not, so the
    // body does not lurch forward with each stab.
    const bool daggerState = weaponStates.find("SpyDagger") != std::string::npos;
    if (atkParam) {
        moveBoxMul = atkStrongMode ? (atkParam == weaponAnim.atkStrong1 ? atkMoveBoxStrong1 : atkMoveBoxStrong) : atkMoveBoxWeak;
        const bool use = atkStrongMode ? (atkParam == weaponAnim.atkStrong1 ? atkUseMoveBoxStrong1 : atkUseMoveBoxStrong) : atkUseMoveBoxWeak;
        if (daggerState && !use) moveBoxMul = 0.f;
    } else if (weapon2Active) {
        const luaw::WeaponFireState& w2 = weapon2IsJump ? jumpAttackLua : weapon2Lua;
        moveBoxMul = w2.loaded ? w2.moveBoxMultiplier : 0.f;
        if (daggerState && w2.loaded && !w2.useMoveBox) moveBoxMul = 0.f;
    }
    if ((atkParam || weapon2Active) && moveBoxMul != 0.f) {
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
                const float lx = (box[12] - weapon2MoveBoxX) * moveBoxMul;
                const float lz = (box[14] - weapon2MoveBoxZ) * moveBoxMul;
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
    // Weapon_*.lua STATE:AddSequence effects: fire each one (the .seq plus
    // its sound) once its startTime is reached; a swing that is cut before
    // then (dodge out of a guard, weapon swap) never plays them.
    // The jump attack's SetDrop lands it before its 500 ms AddSequence (plasma
    // stun): the FSM is already back on the ground state, but the attack
    // (weapon2Active, landing phase) is still running and owns its sequences.
    if (!game::ActorFsm::IsAttack(fsm.current()) && !weapon2Active && !atkParam) pendingSeqs.clear();
    for (size_t i = 0; i < pendingSeqs.size();) {
        if (tnow < pendingSeqs[i].atMs) { ++i; continue; }
        const PendingSeq ps = pendingSeqs[i];
        pendingSeqs.erase(pendingSeqs.begin() + (long)i);
        float fx_ = std::sin(charYaw), fz_ = std::cos(charYaw);
        const size_t beforeFx = activeEffects.size();
        if (!ps.file.empty())
            SpawnSeq(ps.file, charPos[0], charPos[1], charPos[2], charYaw + 3.14159265f);   // .seq is authored from the actor root (feet)
        // AddInstantEffect: on its node of the worn item (Iron Boots R_boo_Dummy*).
        if (!ps.node.empty() && activeEffects.size() > beforeFx) activeEffects.back().attachNode = ps.node;
        // AddSequence index 2 (only the plasma dash: skill_swordman_battack):
        // centred on the actor - its dome is authored 156 ahead of the root,
        // which left the dash running out of it.
        if (ps.index == 2 && activeEffects.size() > beforeFx) {
            ActiveEffect& ne = activeEffects.back();
            for (const EffectModel& em : ne.models) {
                bool done = false;
                for (const scn::Model& m : em.model.models)
                    // Shifted by its own offset once more along the same axis:
                    // the opposite sign moved it further behind (user test).
                    if (!m.vertices.empty()) { ne.offX = m.matrix[12]; ne.offZ = m.matrix[14]; done = true; break; }
                if (done) break;
            }
            // 1.5x (user test: bigger, so it covers the actor; the scale also
            // carries the dome a bit further ahead, 156 -> 234).
            ne.localScale = 1.5f;
        }
        if (!ps.sound.empty()) audio::PlayEffect(ps.sound, 0.9f);
        DbgLog("[weapon-lua] sequence '%s' + '%s' fired", ps.file.c_str(), ps.sound.c_str());
    }

    // Push from a hit (AddPushCondition): speed easing to zero over its time.
    if (pushEnd > pushStart && tnow < pushEnd) {
        const float k = 1.f - float(tnow - pushStart) / float(pushEnd - pushStart);
        moveCtrl.MoveXZ(hitWall, ms, pushVX * k * dt, pushVZ * k * dt);
        charPos[0] = ms.x; charPos[2] = ms.z;
    }

    // Reaction to a hit taken: the body is the reaction's (input is locked,
    // see `reacting`). Clips from actorstates_gameplay.lua by their labels.
    if (reactKind != REACT_NONE) {
        auto row = [&](const char* fn, const char* label) -> const anim::ActorAnim* {
            const anim::ActorFunc* f = luaStates.Func(fn);
            if (!f) return nullptr;
            const std::string want = std::string("ANIMPARAMLIST_") + label;
            for (const anim::ActorAnim& a : f->anims) if (a.label == want && !a.clip.empty()) return &a;
            return nullptr;
        };
        // Plays a row from `since` ms; returns true once a non-looping clip is done.
        auto play = [&](const std::string& clip, float rate, bool loop, float since) {
            const float d = durOfClip(clip);
            float t = since * animCtrl.speedScale * rate;
            const bool done = !loop && d > 0.f && t >= d;
            if (d > 0.f) t = loop ? std::fmod(t, d) : (std::min)(t, d);
            curPose.clipA = clip; curPose.tickA = (uint32_t)t; curPose.loopA = loop;
            curPose.clipB.clear(); curPose.blend = 0.f;
            curLowerPoseActive = false;
            return done;
        };
        auto playRow = [&](const anim::ActorAnim* a, float since) { return a ? play(a->clip, a->rate, a->loop, since) : true; };
        auto nextPhase = [&]() { ++reactPhase; reactPhaseStart = tnow; };
        const float since = float(tnow - reactPhaseStart);
        switch (reactKind) {
        case REACT_FLINCH:
            if (play(reactFlinchClip, 1.f, false, since)) reactKind = REACT_NONE;
            break;
        case REACT_STUN:
            if (reactPhase == 0) { if (playRow(row("ActorState_Stun", "PRECEDING"), since)) nextPhase(); }
            else { playRow(row("ActorState_Stun", "STUN"), since); }
            if (tnow >= reactEnd) reactKind = REACT_NONE;
            break;
        case REACT_BLOW: {
            // In the air: begin, then the middle loop; on landing the end row,
            // then Down and Standup.
            if (reactPhase <= 1) {
                moveCtrl.MoveXZ(hitWall, ms, reactVX * dt, reactVZ * dt);
                charPos[0] = ms.x; charPos[2] = ms.z;
                if (!grounded) reactLeftGround = true;
                if (reactPhase == 0) { if (playRow(row("ActorState_Blow", "BEGIN_NORMAL"), since)) nextPhase(); }
                else playRow(row("ActorState_Blow", "MIDDLE_NORMAL"), since);
                if (reactLeftGround && grounded) { reactPhase = 2; reactPhaseStart = tnow; }
            } else if (reactPhase == 2) {
                if (playRow(row("ActorState_Blow", "END_NORMAL"), since)) { reactKind = REACT_DOWN; reactPhase = 0; reactPhaseStart = tnow; reactEnd = tnow + 800; }
            }
            break;
        }
        case REACT_UPPERCUT:
            // ActorState_UpperCut: NORMAL (GR005) in the air, END (GR006) once
            // landed (S10 CUpperCutState::Update 0x010F3DD0 phase 1 -> 2).
            if (reactPhase == 0) {
                moveCtrl.MoveXZ(hitWall, ms, reactVX * dt, reactVZ * dt);
                charPos[0] = ms.x; charPos[2] = ms.z;
                if (!grounded) reactLeftGround = true;
                playRow(row("ActorState_UpperCut", "NORMAL"), since);
                if (reactLeftGround && grounded) { reactPhase = 1; reactPhaseStart = tnow; }
            } else if (playRow(row("ActorState_UpperCut", "END"), since)) reactKind = REACT_NONE;
            break;
        case REACT_DOWN:
            playRow(row("ActorState_Down", "NORMAL"), since);
            if (tnow >= reactEnd) { reactKind = REACT_STANDUP; reactPhase = 0; reactPhaseStart = tnow; }
            break;
        case REACT_STANDUP:
            if (playRow(row("ActorState_Standup", "NORMAL"), since)) reactKind = REACT_NONE;
            break;
        case REACT_DEATH:
            playRow(row("ActorState_Death", "FALLDOWN_NORMAL_WEAK"), since);
            if (tnow >= reactEnd) {   // back at full health on a spawn point
                reactKind = REACT_NONE;
                myHp = myHpMax;
                if (!spawnPoints.empty()) {
                    const auto& sp = spawnPoints[tnow % spawnPoints.size()];
                    charPos[0] = sp[0]; charPos[1] = sp[1]; charPos[2] = sp[2];
                    velX = velZ = velY = 0.f; grounded = false;
                }
            }
            break;
        default: reactKind = REACT_NONE; break;
        }
    }

    // The shape change is done: the other form, read again from the scripts
    // (every state's SHAPECHNAGE_* rows, the CHANGE_NORMAL idle, the revert pair).
    if (weaponChangePending && !weapon2Active) {
        weaponChangePending = false;
        anim::SetShapeChange(!anim::ShapeChange());
        animCtrl.LoadFromLua(luaStates, weaponStates.c_str(), weaponNormalAs.c_str(), weaponRunAs.c_str());
        weaponAnim = anim::ResolveWeaponAnim(luaStates.parsed(), weaponStates, &luaStates.attackMap(), &luaStates.attribMap(),
                                             weaponRows.empty() ? nullptr : &weaponRows);
        DbgLog("[weapon] shape change -> %s", anim::ShapeChange() ? "transformed" : "normal");
    }

    // The swing's attacks, each at its own fireTime (weapon_*.lua CreateCAttack).
    ProcessSwingAttacks(tnow);
    UpdateClones(tnow, dt);

    // Characters are solid: pushed out of the clones and the other players on
    // this map, the character collision radius (COLL_R) each, respecting walls.
    {
        const float R = 2.f * moveCtrl.tuning().COLL_R;
        float px = 0.f, pz = 0.f; bool any = false;
        auto push = [&](float ox, float oy, float oz) {
            if (std::fabs(oy - charPos[1]) > 170.f) return;   // a body's height: above / below does not block
            float dx = charPos[0] - ox, dz = charPos[2] - oz;
            const float d2 = dx*dx + dz*dz;
            if (d2 >= R*R) return;
            float d = std::sqrt(d2);
            if (d < 1e-3f) { dx = 1.f; dz = 0.f; d = 1.f; }
            const float k = (R - std::sqrt(d2)) / d;
            px += dx * k; pz += dz * k; any = true;
        };
        for (const Clone& c : clones) push(c.pos[0], c.pos[1] + c.lift, c.pos[2]);
        if (mpOnline && mpClient.Connected() && mapIdx >= 0 && mapIdx < (int)maps.size())
            for (auto& kv : mpClient.Peers())
                if (kv.second.hasPos && kv.second.map == maps[mapIdx].name) push(kv.second.rx, kv.second.ry, kv.second.rz);
        if (any) {
            ms.x = charPos[0]; ms.z = charPos[2];
            moveCtrl.MoveXZ(hitWall, ms, px, pz);
            charPos[0] = ms.x; charPos[2] = ms.z;
        }
    }
    if (!atkParam && !weapon2Active) swingAttacks = nullptr;

    // Whatever ended the swing (clip done, weapon swap, guard cancelled), the
    // attack state hands back to STANDBY - the next STOP/MOVE resolves it.
    if (game::ActorFsm::IsAttack(fsm.current()) && !atkParam && !weapon2Active)
        fsm.EndAttack(fctx);

    // Unique weapon states sometimes use AS000_1/E0018_1 variants absent from
    // the shared skeleton. An invalid clip falls back to bind matrices (T-pose),
    // so use the matching base clip when it exists.
    // CAnimParam:SetAniNameForHuman(clip, alt, true): the per-sex clip ("use it if
    // the animation needs adjusting by sex" - the Death Scythe idle GR000 ->
    // GR000_1 for the female character), from every state that declares one.
    if (female) {
        if (!humanAltBuilt) {
            humanAltBuilt = true;
            for (const auto& kv : luaStates.parsed())
                for (const anim::ActorHumanName& h : kv.second.human)
                    if (h.flag && !h.clip.empty() && !h.alt.empty()) humanAlt[h.clip] = h.alt;
        }
        for (std::string* c : { &curPose.clipA, &curPose.clipB }) {
            auto it = humanAlt.find(*c);
            if (it != humanAlt.end() && ClipDurationMs(character, it->second) > 0) *c = it->second;
        }
    }
    const char* standbyFallback = (st == anim::STATE_NORMAL || st == anim::STATE_IDLE) ? "00074" : nullptr;
    NormalizeCharacterClip(character, curPose.clipA, standbyFallback);
    NormalizeCharacterClip(character, curPose.clipB, standbyFallback);
    UpdateOnline(nowMs, dt);

    // The one-frame STANDBY gap between two swings must not restart the legs.
    if (!(atkParam && curLowerPoseActive) && (!atkLegLastMs || tnow - atkLegLastMs > 150)) { atkLegCarried = false; atkLegClip.clear(); atkLegPrevClip.clear(); atkLegLastMs = 0; atkLegFadeTicks = 0.f; }

    // Source fade (see scenes.h srcKey). Every SetAnimation in the client
    // cross-fades from what is on screen; the AnimController does that inside
    // locomotion/jump/dodge, this does it when a swing or a right-click/jump
    // attack phase takes over the body, restarts, or hands back.
    {
        std::string key; int inTicks = 0, outTicks = 0;
        if (atkParam) {
            key = "atk:" + std::to_string((uintptr_t)atkParam) + ":" + std::to_string(atkStart);
            inTicks = atkParam->blendInMs; outTicks = atkParam->blendOutMs;
        } else if (weapon2Active) {
            const auto& phases = weapon2IsJump ? weaponAnim.jumpAtk : weaponAnim.atkWeapon2;
            const int i = phases.empty() ? -1 : (std::min)(weapon2Phase, (int)phases.size() - 1);
            key = "w2:" + std::to_string((int)weapon2IsJump) + ":" + std::to_string(weapon2Phase) + ":" + std::to_string(weapon2PhaseStart);
            if (i >= 0) { inTicks = phases[i].blendInMs; outTicks = phases[i].blendOutMs; }
        } else if (gunReloading) {
            key = "reload:" + std::to_string(gunReloadEnd - gunReloadMs);
            if (weaponAnim.reload) {
                inTicks = weaponAnim.reload->blendInMs;
                outTicks = weaponAnim.reload->blendOutMs;
            }
        } else if (gunFirePoseActive) {
            // The fire state's row is per walking direction (SubMachineGun:
            // D0004 front, D0006 left ... each with its own blendIn/Out): a
            // direction change is a new SetBlendingAnim and cross-fades like
            // any other, torso and legs, with that row's times.
            const std::string dir = !grounded ? std::string(velY > 0.f ? "JUMP" : "FLY")
                                  : ac.moving ? anim::RunDir(ac.inX, ac.inZ) : std::string("NORMAL");
            key = "gun:" + std::to_string(gunFireStart) + ":" + dir;
            if (const anim::BlendSlot* slot = anim::WeaponFireBlendSlot(luaStates.parsed(), weaponStates)) {
                const float vp = (std::max)(kPitchUp, (std::min)(kPitchDown, camPitch + lookPitch));
                const auto& half = (-vp > 0.f || slot->lower.empty()) ? slot->upper : slot->lower;
                const anim::ActorBlend* blend = nullptr;
                auto it = half.find(dir);
                if (it == half.end()) it = half.find("NORMAL");
                if (it != half.end()) blend = &it->second;
                else if (!half.empty()) blend = &half.begin()->second;
                if (blend) { inTicks = blend->blendInMs; outTicks = blend->blendOutMs; }
            }
        } else {
            key = "ctl";
            inTicks = animCtrl.CurBlendInTicks(); outTicks = animCtrl.CurBlendOutTicks();
        }
        if (!srcKey.empty() && key != srcKey && !shownPose.clipA.empty()) {
            const float ticks = float(srcBlendOut + inTicks);
            if (ticks > 0.f) {
                // Interrupting a fade whose outgoing pose still dominates keeps
                // that pose as the source (sub_7324D0 snapshots the mix).
                const float w = srcFadeActive ? 1.f - float(tnow - srcFadeStart) * animCtrl.speedScale / srcFadeTicks : 0.f;
                if (!(srcFadeActive && w > 0.5f)) {
                    srcFadeUp = shownPose;
                    srcFadeLow = shownLowerActive ? shownLower : shownPose;
                }
                // Swing -> swing (with or without the one-frame STANDBY gap in
                // between): the legs are the lowerPartStateSet layer, which
                // carries on by itself; only the torso cross-fades.
                srcFadeLower = !(key.rfind("atk:", 0) == 0 && atkLegCarried);
                srcFadeActive = true; srcFadeStart = tnow; srcFadeTicks = ticks;
                DbgLog("[fade] %s -> %s over %.0f ticks (%.0f ms)", srcKey.c_str(), key.c_str(), ticks, ticks / animCtrl.speedScale);
            }
        }
        srcKey = key; srcBlendOut = outTicks;
        if (srcFadeActive) {
            const float elapsedTicks = float(tnow - srcFadeStart) * animCtrl.speedScale;
            if (elapsedTicks >= srcFadeTicks) srcFadeActive = false;
            else {
                const float w = 1.f - elapsedTicks / srcFadeTicks;
                // The outgoing animation keeps running during the fade
                // (sub_731AC0 advances the shared clock by the same ticks).
                auto apply = [&](scn::Pose& dst, const scn::Pose& src) {
                    dst.fromA = src.clipA; dst.fromB = src.clipB;
                    dst.fromTickA = src.tickA + (uint32_t)elapsedTicks;
                    dst.fromTickB = src.tickB + (uint32_t)elapsedTicks;
                    dst.fromLoopA = src.loopA; dst.fromLoopB = src.loopB;
                    dst.fromBlend = src.blend; dst.fromWeight = w;
                };
                apply(curPose, srcFadeUp);
                if (srcFadeLower) {
                    if (!curLowerPoseActive && srcFadeLow.clipA != srcFadeUp.clipA) {
                        curLowerPose = curPose; curLowerPoseActive = true;
                    }
                    if (curLowerPoseActive) apply(curLowerPose, srcFadeLow);
                }
            }
        }
        shownPose = curPose; shownPose.fromWeight = 0.f;
        shownLowerActive = curLowerPoseActive;
        if (curLowerPoseActive) { shownLower = curLowerPose; shownLower.fromWeight = 0.f; }
    }
    { static unsigned lc = 0;   // DEBUG: state/clip/speed each 0.4s
      if (tnow - lc > 400) { lc = tnow;
        const std::string& cc = curPose.blend > 0.f ? curPose.clipB : curPose.clipA;
        DbgLog("[anim] st=%d clip=%s spd=%.0f ref=%.0f mov=%d grnd=%d tick=%u loop=%d aerial=%d",
               (int)st, cc.c_str(), ac.speed, ac.refSpeed, (int)actuallyMoving, (int)grounded,
               curPose.blend > 0.f ? curPose.tickB : curPose.tickA,
               (int)(curPose.blend > 0.f ? curPose.loopB : curPose.loopA),
               (int)(weapon2Active && weapon2IsJump)); } }
}

// ---- Online (server/mpserver.js) ------------------------------------------
void WorldState::UpdateOnline(unsigned nowMs, float dt) {
    if (!mpOnline) {   // "Jugar solo": close the link once any pending attempt has finished
        if (mpClient.Connected()) { mpClient.Disconnect(); peerLooks.clear(); DbgLog("[mp] offline (solo)"); }
        return;
    }
    if (mapIdx < 0 || mapIdx >= (int)maps.size()) return;
    const std::string mapName = maps[mapIdx].name;
    if (!mpClient.Connected()) {
        if (nowMs < mpNextTryMs) return;
        mpNextTryMs = nowMs + 5000;   // retry every 5 s while the server is down
        // Off the main thread: the probe/handshake can take seconds against a VPS.
        mpClient.ConnectAsync(gOnline.server, gOnline.port, 0 /* server assigns a unique id: two windows on one PC are two players */,
                              gOnline.nickname, mapName, mpWeaponScn, weaponStates, CostumeString());
        return;
    }
    mpClient.Poll(nowMs);
    for (const mp::ChatLine& c : mpClient.TakeChat()) DbgLog("[chat] %s: %s", c.nickname.c_str(), c.text.c_str());
    for (const mp::Shot& s : mpClient.TakeShots()) PlayPeerShot(s);
    for (const mp::Hurt& h : mpClient.TakeHurts()) {
        if (reactKind == REACT_DEATH) continue;
        myHp -= h.dmg;
        hurtFlashUntil = nowMs + 250;
        DbgLog("[mp] hurt %.0f from %d -> hp %.0f stun=%.0f blow=%.0f/%.0f crit=%d", h.dmg, h.fromId, myHp, h.stunMs, h.blowDist, h.blowPower, (int)h.crit);
        // Away from the attacker, on the ground.
        float ax = charPos[0] - h.fromX, az = charPos[2] - h.fromZ;
        const float al = std::sqrt(ax*ax + az*az);
        if (al > 1e-3f) { ax /= al; az /= al; } else { ax = -std::sin(charYaw); az = -std::cos(charYaw); }
        // AddPushCondition: slid away from the attacker at its speed for its time
        // (slowing to a stop), on top of whatever reaction follows.
        if (h.knock > 0.f && h.pushMs > 0.f) {
            pushVX = ax * h.knock; pushVZ = az * h.knock;
            pushStart = nowMs; pushEnd = nowMs + (unsigned)h.pushMs;
        }
        if (myHp <= 0.f) {
            // ActorState_Death, then back at full health on a spawn point.
            StartReaction(REACT_DEATH, nowMs, 2500);
        } else if (h.blowPower > 0.f || h.blowDist != 0.f) {
            // AddBlowCondition(dist, power): knocked off the feet (ActorState_Blow
            // -> Down -> Standup), launched at dist away from the attacker
            // (negative: towards it) and power upwards - S10 CBlowCondition::Apply
            // 0x00FD5890 (same speed units as jump_force 2700).
            reactVX = ax * h.blowDist; reactVZ = az * h.blowDist;
            velY = h.blowPower; grounded = false;
            StartReaction(REACT_BLOW, nowMs, 0);
        } else if (h.stunMs > 0.f) {
            // AddStunCondition: ActorState_Stun for the time the hit decided.
            StartReaction(REACT_STUN, nowMs, (unsigned)h.stunMs);
            if (const anim::ActorFunc* f = luaStates.Func("ActorState_Stun"))
                for (const anim::ActorSeq& q : f->seqs) {
                    SpawnSeq(q.seq, charPos[0], charPos[1], charPos[2], charYaw + 3.14159265f);
                    if (!q.sound.empty()) audio::PlayEffect(q.sound, 0.85f);
                    break;
                }
        } else {
            // A plain hit: the weapon's DamageState_<X> row for the side it came from.
            const float fx = std::sin(charYaw), fz = std::cos(charYaw);
            const float toAx = -ax, toAz = -az;   // towards the attacker
            const float front = toAx * fx + toAz * fz, side = toAx * fz - toAz * fx;
            const char* dir = std::fabs(front) >= std::fabs(side) ? (front >= 0.f ? "FRONT" : "BACK") : (side >= 0.f ? "LEFT" : "RIGHT");
            auto it = weaponAnim.dmg.find(dir);
            if (it == weaponAnim.dmg.end()) it = weaponAnim.dmg.find("FRONT");
            if (it != weaponAnim.dmg.end() && !it->second.clip.empty()) { reactFlinchClip = it->second.clip; StartReaction(REACT_FLINCH, nowMs, 0); }
        }
    }
    if (nowMs >= mpNextSendMs) {
        mpNextSendMs = nowMs + 50;   // ~20/s, like the web client
        const std::string& clip = curPose.blend > 0.5f && !curPose.clipB.empty() ? curPose.clipB : curPose.clipA;
        mpClient.SendState(mapName, charPos[0], charPos[1], charPos[2], charYaw, clip, myHp);
        static unsigned nextWsState = 0;
        if (nowMs >= nextWsState) { nextWsState = nowMs + 500; mpClient.SendStateWs(mapName, charPos[0], charPos[1], charPos[2], charYaw, clip, myHp); }
    }
    // Smooth the others toward their last received state (same at any fps).
    const float a = 1.f - std::exp(-dt * 12.f);
    for (auto& kv : mpClient.Peers()) {
        mp::Peer& p = kv.second;
        if (!p.hasPos) continue;
        p.rx += (p.x - p.rx) * a; p.ry += (p.y - p.ry) * a; p.rz += (p.z - p.rz) * a;
        float dy = p.yaw - p.ryaw;
        while (dy > 3.14159265f) dy -= 6.2831853f;
        while (dy < -3.14159265f) dy += 6.2831853f;
        p.ryaw += dy * a;
        if (p.clip != p.lastClip) { p.lastClip = p.clip; p.animClockMs = 0.f; }
        p.animClockMs += dt * 1000.f;
    }
}

const std::vector<WorldState::WeaponRenderPart>& WorldState::PeerWeaponParts(const std::string& file, const std::string& states) {
    const std::string key = file + "|" + states;
    auto it = peerWeapons.find(key);
    if (it != peerWeapons.end()) return it->second;
    std::vector<WeaponRenderPart> parts;
    const std::optional<int> typeId = file.empty() ? std::nullopt
        : data::TypeFromSuffix(respath::Root(), states, luaStates.attackMap().typeBySuffix);
    auto build = [&](const std::string& f, int row) {
        WeaponRenderPart part;
        part.sceneIndex = row;
        part.scene = scn::Parse((RES("resources/resources/model/weapon/") + f).c_str());
        if (!part.scene.ok || part.scene.models.empty()) return;
        auto isNode = [&](const std::string& name) {
            for (const scn::Model& m : part.scene.models) if (m.name == name) return true;
            for (const scn::Bone& b : part.scene.bones) if (b.name == name) return true;
            return false;
        };
        std::vector<const scn::Bone*> roots;
        for (const scn::Bone& b : part.scene.bones) if (!isNode(b.parent) && b.name != "BONESYSTEM") roots.push_back(&b);
        for (const scn::Bone& b : part.scene.bones) if (b.parent == "BONESYSTEM") roots.push_back(&b);
        const scn::Bone* mount = nullptr;
        for (const scn::Bone* b : roots) if (b->name.find("Wea") != std::string::npos || b->name.find("wea") != std::string::npos) { mount = b; break; }
        if (!mount) for (const scn::Bone& b : part.scene.bones) if (b.name.find("Wea_Dummy") != std::string::npos) { mount = &b; break; }
        if (!mount) for (const scn::Bone* b : roots) if (b->name.find("Dummy") != std::string::npos || b->name.find("dummy") != std::string::npos) { mount = b; break; }
        if (!mount && !roots.empty()) mount = roots[0];
        if (mount) { scn::WeaponMountInverse(part.scene, *mount, part.invMount); part.mountName = mount->name; }
        scn::ResolveWeaponPieceWorlds(part.scene, part.pieceWorlds);
        // Same rules as our own primary piece (EquipWeaponFile): the table's
        // bone, else the one deduced from the mount name (smash: Gun_Dummy);
        // link=1 pieces hang as authored, no mount inverse.
        part.attackBone = typeId ? data::AttachBone(respath::Root(), *typeId, row, false) : "";
        if (part.attackBone.empty()) part.attackBone = row ? std::string("L_Hand_Dummy") : WeaponBoneFromMount(part.mountName);
        // Holstered bone (sprint, jump, dodge, sit): the table's idle row, as ours.
        part.idleBone = typeId ? data::AttachBone(respath::Root(), *typeId, row, true) : "";
        if (part.idleBone.empty()) part.idleBone = part.attackBone;
        if (typeId && data::AttachLink(respath::Root(), *typeId, row) == 1) {
            const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            std::memcpy(part.invMount, id, sizeof(id));
            part.mountName.clear();
        }
        parts.push_back(std::move(part));
    };
    if (!file.empty()) {
        std::string rightStem, leftStem;
        const std::string stem = std::filesystem::path(file).stem().string();
        build(file, 0);
        if (SplitWeaponPair(stem, rightStem, leftStem) && std::filesystem::exists(RES("resources/resources/model/weapon/") + leftStem + ".scn"))
            build(leftStem + ".scn", 1);
    }
    DbgLog("[mp] peer weapon '%s' states='%s' type=%d pieces=%zu bone='%s'", file.c_str(), states.c_str(), typeId ? *typeId : -1,
           parts.size(), parts.empty() ? "" : parts[0].attackBone.c_str());
    return peerWeapons.emplace(key, std::move(parts)).first->second;
}

// The clone's reaction to our hit: a plain hit plays our weapon's
// DamageState FRONT row; a blow launches it (dist away from us, power up),
// it lands on ActorState_Blow END and gets up with ActorState_Standup, then
// goes back to the pose it was left in.
void WorldState::CloneHit(Clone& c, float dmg, float stunMs, float blowDist, float blowPower, unsigned tnow) {
    (void)stunMs;
    if (blowPower > 0.f || blowDist != 0.f) {
        float ax = c.pos[0] - charPos[0], az = c.pos[2] - charPos[2];
        const float al = std::sqrt(ax*ax + az*az);
        if (al > 1e-3f) { ax /= al; az /= al; } else { ax = std::sin(charYaw); az = std::cos(charYaw); }
        c.vx = ax * blowDist; c.vz = az * blowDist; c.vy = blowPower;
        c.react = 2;
    } else if (c.react == 0 || c.react == 1) {
        auto it = weaponAnim.dmg.find("FRONT");
        if (it != weaponAnim.dmg.end() && !it->second.clip.empty()) { c.reactClip = it->second.clip; c.react = 1; }
    }
    c.reactStart = tnow;
    DbgLog("[clone] hit %.1f blow=%.0f/%.0f", dmg, blowDist, blowPower);
}

void WorldState::UpdateClones(unsigned tnow, float dt) {
    for (Clone& c : clones) {
        if (c.react != 2) continue;
        // Ballistic flight under the movement's gravity (GRAV 7500).
        c.pos[0] += c.vx * dt; c.pos[2] += c.vz * dt;
        c.lift += c.vy * dt; c.vy -= moveCtrl.tuning().GRAV * dt;
        if (c.lift <= 0.f && c.vy < 0.f) { c.lift = 0.f; c.vy = 0.f; c.react = 3; c.reactStart = tnow; }
    }
}

// V (and a new map): the clones go, with their frozen effects and GPU meshes.
void WorldState::ReleaseClones(IRenderer* r) {
    for (Clone& c : clones) for (const Clone::Baked& b : c.baked) cloneMeshesToFree.push_back(b.mesh);
    clones.clear();
    activeEffects.erase(std::remove_if(activeEffects.begin(), activeEffects.end(), [](const ActiveEffect& e) { return e.frozen; }),
                        activeEffects.end());
    if (r) { for (int m : cloneMeshesToFree) r->ReleaseStaticMesh(m); cloneMeshesToFree.clear(); }
}

void WorldState::DrawClones(IRenderer* r) {
    if (!loaded) return;
    for (int m : cloneMeshesToFree) r->ReleaseStaticMesh(m);
    cloneMeshesToFree.clear();
    for (Clone& c : clones) DrawClone(r, c);
}

void WorldState::DrawClone(IRenderer* r, Clone& c) {
    const unsigned now = GetTickCount();
    auto row = [&](const char* fn, const char* label) -> std::string {
        if (const anim::ActorFunc* f = luaStates.Func(fn))
            for (const anim::ActorAnim& a : f->anims) if (a.label == std::string("ANIMPARAMLIST_") + label && !a.clip.empty()) return a.clip;
        return std::string();
    };
    // Where it is now: its own position plus the lift of a blow.
    const float pos[3] = { c.pos[0], c.pos[1] + c.lift, c.pos[2] };

    // Reaction pose (flinch / blown / getting up): played live.
    scn::AnimPose pose = c.pose;
    float since = -1.f;
    const float sinceReact = float(now - c.reactStart);
    auto oneShot = [&](const std::string& clip, float t) {
        const float d = (float)ClipDurationMs(character, clip);
        if (clip.empty() || d <= 0.f || t * animCtrl.speedScale >= d) return false;
        pose.clipA = clip; pose.loopA = false; since = t; return true;
    };
    if (c.react == 1 && !oneShot(c.reactClip, sinceReact)) c.react = 0;
    if (c.react == 2) {
        const std::string mid = row("ActorState_Blow", "MIDDLE_NORMAL");
        if (!oneShot(row("ActorState_Blow", "BEGIN_NORMAL"), sinceReact) && !mid.empty()) { pose.clipA = mid; pose.loopA = true; since = sinceReact; }
    }
    if (c.react == 3) {
        const std::string end = row("ActorState_Blow", "END_NORMAL"), up = row("ActorState_Standup", "NORMAL");
        const float endMs = (float)ClipDurationMs(character, end) / animCtrl.speedScale;
        if (!oneShot(end, sinceReact) && !oneShot(up, sinceReact - endMs)) c.react = 0;
    }

    if (c.react == 0 || since < 0.f) {
        // Held pose: built once as GPU meshes at the origin, then only moved.
        if (!c.bakedDone) {
            c.bakedDone = true;
            std::vector<IRenderer::CapturedMesh> cap;
            const float origin[3] = { 0.f, 0.f, 0.f };
            float place0[16]; BuildYawTranslate(c.yaw + 3.14159265f, origin, place0);
            const scn::AnimPose* lower = c.lowerActive ? &c.lowerPose : nullptr;
            std::vector<CharPiece> pieces;
            const unsigned hideSlot = weaponOk ? WornSlotMask() : 0u;
            GatherCharacterPieces(character, bodyParts, c.pose, pieces, lower, hideSlot);
            if (weaponOk) GatherCharacterPieces(character, weaponWornParts, c.pose, pieces, lower);
            r->SetMeshCapture(&cap);
            DrawCharPieces(r, pieces, place0);
            if (weaponOk) DrawWeaponParts(r, c.pose, place0, true, lower);
            r->SetMeshCapture(nullptr);
            for (const IRenderer::CapturedMesh& m : cap) {
                Clone::Baked b;
                b.mesh = r->CreateStaticMesh(m.verts.data(), 5, m.vertCount, m.idx.data(), (int)m.idx.size());
                if (b.mesh < 0) continue;
                b.tris = (int)m.idx.size() / 3; b.tex = m.tex; b.flags = m.flags;
                std::memcpy(b.world, m.world, sizeof(b.world));
                c.baked.push_back(b);
            }
            DbgLog("[clone] baked %zu meshes", c.baked.size());
        }
        float move[16]; BuildYawTranslate(0.f, pos, move);
        for (const Clone::Baked& b : c.baked) {
            float w[16]; Mat4MulS(b.world, move, w);
            r->DrawStaticMesh(b.mesh, 0, b.tris, b.tex, -1, w, b.flags);
        }
    } else {
        // Live: the reaction clip, our body and weapon.
        pose.tickA = (uint32_t)(since * animCtrl.speedScale); pose.clipB.clear(); pose.blend = 0.f;
        float place[16]; BuildYawTranslate(c.yaw + 3.14159265f, pos, place);
        std::vector<CharPiece> pieces;
        const unsigned hideSlot = weaponOk ? WornSlotMask() : 0u;
        GatherCharacterPieces(character, bodyParts, pose, pieces, nullptr, hideSlot);
        if (weaponOk) GatherCharacterPieces(character, weaponWornParts, pose, pieces, nullptr);
        DrawCharPieces(r, pieces, place);
        if (weaponOk) DrawWeaponParts(r, pose, place, true, nullptr);
    }

    // The weapon trail it had, as it was (drawn at the instant it was left).
    if (c.ghostTrail && c.ghostSamples.size() >= 2) {
        std::swap(ghostSamples, c.ghostSamples);
        const anim::ActorGhostTrail* gt = ghostTrail; const unsigned gs = ghostStart;
        ghostTrail = c.ghostTrail; ghostStart = c.ghostStart;
        DrawGhostTrail(r, c.ghostAt);
        c.ghostTrail = ghostTrail;
        ghostTrail = gt; ghostStart = gs;
        std::swap(ghostSamples, c.ghostSamples);
    }
}

void WorldState::DrawPeers(IRenderer* r) {
    if (!mpClient.Connected() || mapIdx < 0 || mapIdx >= (int)maps.size()) return;
    const std::string mapName = maps[mapIdx].name;
    for (auto& kv : mpClient.Peers()) {
        const mp::Peer& p = kv.second;
        if (!p.hasPos || p.map != mapName) continue;
        scn::AnimPose pose;
        pose.clipA = p.clip.empty() ? std::string("00074") : p.clip;
        pose.tickA = (uint32_t)(p.animClockMs * animCtrl.speedScale);
        pose.loopA = true;
        const float pos[3] = { p.rx, p.ry, p.rz };
        float place[16]; BuildYawTranslate(p.ryaw + 3.14159265f, pos, place);
        const scn::Scene* skel = &character;
        const std::vector<scn::Scene>* parts = &bodyParts;
        if (!p.equip.empty()) {
            auto it = peerLooks.find(p.equip);
            if (it == peerLooks.end()) {
                // {"gender":"female","body":"x.scn",...} (web clients may omit gender / send null slots)
                PeerLook look;
                const std::string g = JsonStringOrNull(p.equip, "gender");
                look.female = g.empty() ? p.equip.find("female") != std::string::npos : g == "female";
                std::string files[8];
                for (int i = 0; i < 8; ++i) {
                    files[i] = JsonStringOrNull(p.equip, kCostumeSlots[i]);
                    if (files[i].empty() && i < 6) files[i] = std::string("00_") + (look.female ? "female" : "male") + "_" + kCostumeSlots[i] + ".scn";
                }
                look.parts = LoadCostumeParts(look.female, files);
                DbgLog("[mp] look for %s: %s, %zu parts", p.nickname.c_str(), look.female ? "female" : "male", look.parts.size());
                it = peerLooks.emplace(p.equip, std::move(look)).first;
            }
            const PeerLook& look = it->second;
            if (!look.parts.empty()) {
                if (look.female != female) {
                    if (!peerSkelLoaded[look.female]) { peerSkel[look.female] = LoadCharacterSkeleton(look.female); peerSkelLoaded[look.female] = true; }
                    if (peerSkel[look.female].ok) { skel = &peerSkel[look.female]; parts = &look.parts; }
                } else parts = &look.parts;
            }
        }
        std::vector<CharPiece> pieces;
        GatherCharacterPieces(*skel, *parts, pose, pieces, nullptr);
        DrawCharPieces(r, pieces, place);
        // The states that holster the weapon (ActorFsm::WeaponPoseFor: FASTRUN,
        // JUMP, BOUNDJUMP, FALL, DODGE, SIT) told apart by the clip they play,
        // read from those states' own rows in actorstates_gameplay.lua.
        if (peerHolsterClips.empty())
            for (const char* fn : { "ActorState_Jump", "ActorState_BoundJump", "ActorState_Fall", "ActorState_Dodge",
                                    "ActorState_FastRun", "ActorState_Sit" })
                if (const anim::ActorFunc* f = luaStates.Func(fn))
                    for (const anim::ActorAnim& a : f->anims) if (!a.clip.empty()) peerHolsterClips.insert(a.clip);
        const bool holstered = peerHolsterClips.count(pose.clipA) != 0;
        // Their weapon, rigid pieces: piece * mount inverse * hand (or holster) bone * place.
        for (const WeaponRenderPart& part : PeerWeaponParts(p.weapon, p.weaponStates)) {
            const bool holsterThis = holstered && part.idleBone != part.attackBone;
            float hand[16];
            if (!scn::BoneWorldByName(*skel, holsterThis ? part.idleBone : part.attackBone, pose, hand)) continue;
            float handAligned[16];
            std::memcpy(handAligned, hand, sizeof(handAligned));
            if (holsterThis && part.idleBone == "Gun_Dummy") {
                if (part.holsterState == 0 || part.holsterGen != gHolsterGen) {
                    part.holsterGen = gHolsterGen;
                    part.holsterState = HolsterBarrelDown(part.scene, part.invMount, *skel, part.idleBone, part.mountName, part.holsterRot) ? 1 : 2;
                }
                if (part.holsterState == 1) HolsterWithSwing(*skel, hand, part.holsterRot, handAligned);
            }
            float toHand[16]; Mat4MulS(part.invMount, handAligned, toHand);
            // The weapon plays the clip the peer's body plays when it has it
            // (smash rifle DD000.. place its pieces), else its default clip -
            // MakeWeaponPose, as for our own weapon.
            const scn::AnimPose weaponPose = MakeWeaponPose(part.scene, anim::WeaponAnim{}, pose, anim::STATE_NORMAL, false, 0.f, 0.f, nullptr, false, holsterThis);
            const bool animateWeapon = !weaponPose.clipA.empty() || !weaponPose.fallbackClip.empty() || !part.scene.animNames.empty();
            const scn::NodeResolver weaponNodes = animateWeapon ? scn::MakeNodeResolver(part.scene, weaponPose, true) : scn::NodeResolver{};
            // A mount moved by that clip is cancelled where it is now (DrawWeaponParts).
            if (animateWeapon && !part.mountName.empty()) {
                scn::Mat4 baseW, now;
                const scn::Mat4* nowP = weaponNodes(part.mountName);
                if (nowP) now = *nowP;
                bool haveBase = nowP && scn::TrackWorld(part.scene, part.mountName, baseW, true);
                if (nowP && !haveBase) {
                    scn::Pose bp; bp.clipA = "BASE";
                    if (const scn::Mat4* b = scn::MakeNodeResolver(part.scene, bp, true)(part.mountName)) { baseW = *b; haveBase = true; }
                }
                if (haveBase) {
                    float diff = 0.f;
                    for (int k = 0; k < 15; ++k) diff = (std::max)(diff, std::fabs(now[k] - baseW[k]) / (k >= 12 ? 1.f : 0.02f));
                    if (diff > 1.f) { const scn::Mat4 invNow = scn::Mat4InvAffine(now); Mat4MulS(invNow.data(), handAligned, toHand); }
                }
            }
            for (size_t mi = 0; mi < part.scene.models.size(); ++mi) {
                const scn::Model& m = part.scene.models[mi];
                if (m.faces.empty() || m.vertices.empty() || !m.skin.empty()) continue;
                const float* pieceMat = mi < part.pieceWorlds.size() ? part.pieceWorlds[mi].data() : m.matrix;
                if (animateWeapon) if (const scn::Mat4* a = weaponNodes(m.name)) pieceMat = a->data();
                float toMount[16], world[16];
                Mat4MulS(pieceMat, toHand, toMount);
                Mat4MulS(toMount, place, world);
                const bool hasUv = m.vertices.size() == m.uv1.size();
                std::vector<float> pu(m.vertices.size() * 5);
                for (size_t i = 0; i < m.vertices.size(); ++i) {
                    pu[i*5+0] = m.vertices[i].x; pu[i*5+1] = m.vertices[i].y; pu[i*5+2] = m.vertices[i].z;
                    pu[i*5+3] = hasUv ? m.uv1[i].u : 0.f; pu[i*5+4] = hasUv ? m.uv1[i].v : 0.f;
                }
                std::vector<unsigned short> idx; idx.reserve(m.faces.size() * 3);
                for (const scn::Face& f : m.faces) { idx.push_back(f.a); idx.push_back(f.b); idx.push_back(f.c); }
                const std::string texPath = part.scene.dir.empty() ? m.texture : (part.scene.dir + "/" + m.texture);
                r->DrawMesh(pu.data(), (int)m.vertices.size(), idx.data(), (int)idx.size(), r->LoadTexture(texPath.c_str()), world, m.renderFlag);
            }
        }
    }
}

// The blade trail of state:AddGhostTrail. Values are the beta's CWeapon ->
// Game::CActor::MakeGhostTrailNode(Line01, name, texture, 200, 50.f, 50, 100,
// 0, blend) into CGhostTrailData: LifeTime 200 ms, UnitLength 50, FadeInTime
// 50, FadeOutTime 100; texture gi.tga (the _02 trail gi2.tga, the bat
// bi_atex.tga), additive except the bat's (blend arg 1 -> alpha).
void WorldState::DrawGhostTrail(IRenderer* r, unsigned tnow) {
    const unsigned kLifeMs = 200, kFadeInMs = 50, kFadeOutMs = 100;
    const float kUnitLength = 50.f;
    while (!ghostSamples.empty() && tnow - ghostSamples.front().t > kLifeMs) ghostSamples.erase(ghostSamples.begin());
    if (!r || !ghostTrail || ghostSamples.size() < 2) return;
    // Whole-trail fade: in over FadeInTime from the window start, out over
    // FadeOutTime after its end.
    const float age = (float)(tnow - ghostStart);
    float fade = 1.f;
    if (age < ghostTrail->startMs + (float)kFadeInMs) fade = (std::max)(0.f, (age - ghostTrail->startMs) / (float)kFadeInMs);
    if (age > ghostTrail->endMs) fade = (std::min)(fade, 1.f - (age - ghostTrail->endMs) / (float)kFadeOutMs);
    if (fade <= 0.f) {
        if (age > ghostTrail->endMs + (float)kFadeOutMs) { ghostTrail = nullptr; ghostSamples.clear(); }
        return;
    }
    const std::string& name = ghostTrail->name;
    const bool bat = name.find("bat_sword") != std::string::npos;
    const bool second = name.size() >= 3 && name.compare(name.size() - 3, 3, "_02") == 0;
    const char* tex = bat ? RESC("resources/resources/image/bi_atex.tga")
                    : second ? RESC("resources/resources/image/gi2.tga") : RESC("resources/resources/image/gi.tga");
    // Samples come once per frame; at a fast swing the blade jumps far between
    // them, so the strip is cut into UnitLength pieces along a Catmull-Rom
    // through the samples (both edges), like the node's unit subdivision.
    auto cr = [](const float* p0, const float* p1, const float* p2, const float* p3, float t, float* o) {
        const float t2 = t * t, t3 = t2 * t;
        for (int k = 0; k < 3; ++k)
            o[k] = 0.5f * (2.f*p1[k] + (-p0[k] + p2[k]) * t + (2.f*p0[k] - 5.f*p1[k] + 4.f*p2[k] - p3[k]) * t2 + (-p0[k] + 3.f*p1[k] - 3.f*p2[k] + p3[k]) * t3);
    };
    std::vector<float> v;   // x,y,z,u,v
    const size_t n = ghostSamples.size();
    auto emit = [&](const float* a, const float* b, float sampleAge) {
        const float u = (std::min)(1.f, sampleAge / (float)kLifeMs);
        v.insert(v.end(), { a[0], a[1], a[2], u, 0.f, b[0], b[1], b[2], u, 1.f });
    };
    for (size_t i = n; i-- > 0; ) {   // newest first
        const GhostSample& s1 = ghostSamples[i];
        emit(s1.a, s1.b, (float)(tnow - s1.t));
        if (i == 0) break;
        const GhostSample& s2 = ghostSamples[i - 1];
        const GhostSample& s0 = ghostSamples[i + 1 < n ? i + 1 : i];
        const GhostSample& s3 = ghostSamples[i >= 2 ? i - 2 : i - 1];
        const float dx = s2.b[0] - s1.b[0], dy = s2.b[1] - s1.b[1], dz = s2.b[2] - s1.b[2];
        const int pieces = (std::min)(16, (int)std::ceil(std::sqrt(dx*dx + dy*dy + dz*dz) / kUnitLength));
        for (int k = 1; k < pieces; ++k) {
            const float t = (float)k / (float)pieces;
            float a[3], b[3];
            cr(s0.a, s1.a, s2.a, s3.a, t, a);
            cr(s0.b, s1.b, s2.b, s3.b, t, b);
            emit(a, b, (float)(tnow - s1.t) + t * (float)(s1.t - s2.t));
        }
    }
    const int verts = (int)(v.size() / 5);
    if (verts < 4) return;
    std::vector<unsigned short> idx;
    for (int q = 0; q + 3 < verts; q += 2) {
        idx.insert(idx.end(), { (unsigned short)q, (unsigned short)(q + 1), (unsigned short)(q + 2),
                                (unsigned short)(q + 1), (unsigned short)(q + 3), (unsigned short)(q + 2) });
    }
    r->meshFade = fade;
    const uint32_t flags = scn::RF_NO_CULLING | scn::RF_NO_DEPTH_WRITE | (bat ? scn::RF_TRANSPARENT : 32u);
    r->DrawMesh(v.data(), verts, idx.data(), (int)idx.size(), r->LoadTexture(tex), nullptr, flags);
    r->meshFade = 1.f;
}

// The equipped weapon on the character: every piece hung from its bip bone
// and posed by the weapon's own clip. preview = the inventory character:
// always in hand, idle, no attack.
void WorldState::DrawWeaponParts(IRenderer* r, const scn::AnimPose& pose, const float* place, bool preview, const scn::AnimPose* lowerPose) {
        const unsigned shotPoseMs = gunAnimMs > 0.f ? (unsigned)gunAnimMs : 200u;
        const bool isDodge = !preview && (animCtrl.state() == anim::STATE_DODGE_LEFT || animCtrl.state() == anim::STATE_DODGE_RIGHT);
        const bool shotPoseActive = !isDodge && (gunFiring || (gunLastShotAt && GetTickCount() - gunLastShotAt < shotPoseMs));
        if (!preview) weaponNodeChar.clear();
        // Arma: cuelga del dummy del bip y sigue la animacion del personaje.
        // world = m.matrix * inv(dummyRaizDelArma) * huesoDelBip * place
        //
        // The official bone table (weapon_attach.x7) is keyed by which FILE this
        // weapon piece came from (0 = right/single .scn, 1 = the left companion),
        // not by model index within one file. Prefix (r_/l_) and suffix (_r/_l)
        // pairs are normalized to the same sceneIndex order.
        for (const WeaponRenderPart& part : weaponParts) {
        // The actor states move the weapon between its two nodes (see
        // game::ActorFsm::WeaponPoseFor): in hand for NORMAL/RUN/attacks,
        // holstered (Plasma Sword -> Spine_Dummy, guns -> Gun_Dummy) for
        // FASTRUN/JUMP/FALL/DODGE/SIT... Pieces without an idleAttach (link=1:
        // fists, forearm parts) keep idleBone == attackBone and never move.
        // Holstered states (sprint, jump, dodge, sit) put the piece on its
        // idleAttach (revolver: Gun_Dummy, the left hip). It plays the body's
        // clip when it has one (the plasma's own 00068), else its bind pose.
        const bool holsterThis = weaponHolstered && !preview && part.idleBone != part.attackBone && !shotPoseActive && !gunReloading;
        const std::string& selectedBone = !part.idleBoneOverride.empty() ? part.idleBoneOverride
                                        : (holsterThis ? part.idleBone : part.attackBone);
        std::string attachBoneName = selectedBone.empty() ? weaponBone : selectedBone;
        float hand[16];
        if (weaponOk && SkelBoneWorld(character, attachBoneName, pose, lowerPose, hand)) {
            // Holstered on the thigh: barrel down (see HolsterBarrelDown).
            float handAligned[16];
            std::memcpy(handAligned, hand, sizeof(handAligned));
            if (holsterThis && attachBoneName == "Gun_Dummy") {
                if (part.holsterState == 0 || part.holsterGen != gHolsterGen) {
                    part.holsterGen = gHolsterGen;
                    part.holsterState = HolsterBarrelDown(part.scene, part.invMount, character, attachBoneName, part.mountName, part.holsterRot) ? 1 : 2;
                }
                if (part.holsterState == 1) HolsterWithSwing(character, hand, part.holsterRot, handAligned);
            }
            float toHand[16];
            Mat4MulS(part.invMount, handAligned, toHand);
            // Character and weapon SCNs use separate clip namespaces. Demonic
            // Knuckle, for example, uses KN000/KN001..KN008 while the body uses
            // 00074/00008..00015. Select the weapon's authored state first.
            scn::AnimPose weaponPose = MakeWeaponPose(part.scene, weaponAnim, pose,
                preview ? anim::STATE_NORMAL : animCtrl.state(), !preview && weaponPoseMoving, weaponPoseInX, weaponPoseInZ,
                preview ? nullptr : atkParam, !preview && (weapon2Active || shotPoseActive || gunReloading),
                holsterThis);
            // A file with only BASE (counter_goldenchain_r) still assembles from
            // its own tracks: its static matrices are parent-local, not absolute.
            const bool animateWeapon = !weaponPose.clipA.empty() || !weaponPose.fallbackClip.empty() || !part.scene.animNames.empty();
            const scn::NodeResolver weaponNodes = animateWeapon
                ? scn::MakeNodeResolver(part.scene, weaponPose, true)
                : scn::NodeResolver{};
            // A mount dummy moved by the weapon's clip (deathscythe: Sword_Wea_
            // Dummy is at (-28,127,40) in BASE but at the origin in its GR*
            // clips) must be cancelled where it is NOW - the BASE inverse put
            // the assembled scythe low in the hand, upside down. Only when it
            // really moved; otherwise keep invMount (its special cases).
            if (animateWeapon && !part.mountName.empty()) {
                scn::Mat4 baseW, now;
                const scn::Mat4* nowP = weaponNodes(part.mountName);
                if (nowP) now = *nowP;   // the resolver hands out one shared slot: copy it
                bool haveBase = nowP && scn::TrackWorld(part.scene, part.mountName, baseW, true);
                if (nowP && !haveBase) {
                    // Chain with a trackless node (124_weapon_bat's IK Chain*):
                    // the BASE place from the same resolver the draw uses.
                    scn::Pose bp; bp.clipA = "BASE";
                    if (const scn::Mat4* b = scn::MakeNodeResolver(part.scene, bp, true)(part.mountName)) { baseW = *b; haveBase = true; }
                }
                if (haveBase) {
                    float diff = 0.f;
                    for (int k = 0; k < 15; ++k) diff = (std::max)(diff, std::fabs(now[k] - baseW[k]) / (k >= 12 ? 1.f : 0.02f));
                    if (diff > 1.f) {
                        const scn::Mat4 invNow = scn::Mat4InvAffine(now);
                        Mat4MulS(invNow.data(), handAligned, toHand);
                    }
                }
            }
            // The fire node of the weapon as drawn this frame (Lua
            // SetGunBlastSequence / SetGunTracerAttachNode node): shots, the
            // muzzle blast and beams start here.
            // A holstered gun (hip Gun_Dummy) is not where the next shot leaves
            // from: the first shot after it falls back to the InitLua offsets.
            if (!preview && holsterThis && part.sceneIndex == 0) gunMuzzleValid = false;
            if (!preview && !holsterThis && !weaponAnim.gunFireBone.empty() && part.sceneIndex == 0) {
                std::string fireNode = weaponAnim.gunFireBone;
                auto endsWith = [](const std::string& value, const std::string& suffix) {
                    return value.size() >= suffix.size() &&
                           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
                };
                // Some skins prefix the Lua node name (toygun:
                // Amhandgun_Fire_Dummy vs handgun_Fire_Dummy).
                bool named = false;
                for (const scn::Bone& b : part.scene.bones)
                    if (b.name == fireNode) { named = true; break; }
                if (!named) for (const scn::Model& m : part.scene.models)
                    if (m.name == fireNode) { named = true; break; }
                if (!named) {
                    for (const scn::Bone& b : part.scene.bones)
                        if (endsWith(b.name, fireNode)) { fireNode = b.name; named = true; break; }
                    if (!named) for (const scn::Model& m : part.scene.models)
                        if (endsWith(m.name, fireNode)) { fireNode = m.name; named = true; break; }
                }
                // The Lua node is not in this skin (minegun, rescue gun, gs: their
                // shots come from other scripts): the weapon's own fire node.
                if (!named) {
                    auto isFire = [](std::string n) { for (char& c : n) c = (char)tolower((unsigned char)c); return n.find("fire") != std::string::npos && n.find("dummy") != std::string::npos; };
                    for (const scn::Bone& b : part.scene.bones) if (isFire(b.name)) { fireNode = b.name; named = true; break; }
                    if (!named) for (const scn::Model& m : part.scene.models) if (isFire(m.name)) { fireNode = m.name; break; }
                }
                const scn::Mat4* fw = animateWeapon ? weaponNodes(fireNode) : nullptr;
                float nodeW[16], a[16], w[16];
                bool have = false;
                // Some exports store a node's LOCAL transform as its matrix
                // (handgun, smg_02, revolver_kid, hw_kid, railgun_kid: matrix ==
                // its track's initT, parent = a model piece) - read raw it sits
                // 50-2800 units off the gun. Composing the tracks up the parent
                // chain (TrackWorld) is right for all 191 guns but the 2 whose
                // node rides a skinned bone. Use the clip's position only when
                // the clip really moves the node away from its stored matrix.
                const scn::Bone* fireBone = nullptr;
                for (const scn::Bone& b : part.scene.bones) if (b.name == fireNode) { fireBone = &b; break; }
                bool clipMoves = false;
                if (fw && fireBone) for (int k = 12; k < 15; ++k) if (std::fabs((*fw)[k] - fireBone->matrix[k]) > 0.5f) clipMoves = true;
                scn::Mat4 trackW;
                if (fw && (clipMoves || !fireBone)) { std::memcpy(nodeW, fw->data(), sizeof(nodeW)); have = true; }
                else if (fireBone && scn::TrackWorld(part.scene, fireNode, trackW, true)) { std::memcpy(nodeW, trackW.data(), sizeof(nodeW)); have = true; }
                else if (fw) { std::memcpy(nodeW, fw->data(), sizeof(nodeW)); have = true; }
                else if (fireBone) { std::memcpy(nodeW, fireBone->matrix, sizeof(nodeW)); have = true; }
                if (!have) for (const scn::Model& m : part.scene.models) if (m.name == fireNode) { std::memcpy(nodeW, m.matrix, sizeof(nodeW)); have = true; break; }
                if (have) {
                    Mat4MulS(nodeW, toHand, a);
                    Mat4MulS(a, place, w);
                    gunMuzzle[0] = w[12]; gunMuzzle[1] = w[13]; gunMuzzle[2] = w[14];
                    gunMuzzleValid = true;
                    // Where the drawn gun is vs its fire node (one line per shot pose
                    // change): a muzzle that drifts from the model shows up here.
                    static unsigned lastMzLog = 0;
                    if (shotPoseActive && GetTickCount() - lastMzLog > 300 && !part.scene.models.empty()) {
                        lastMzLog = GetTickCount();
                        const float* pm = part.pieceWorlds.empty() ? part.scene.models[0].matrix : part.pieceWorlds[0].data();
                        const scn::Mat4* am = animateWeapon ? weaponNodes(part.scene.models[0].name) : nullptr;
                        if (am) pm = am->data();
                        float mw[16], mt[16]; Mat4MulS(pm, toHand, mt); Mat4MulS(mt, place, mw);
                        const float dx = w[12]-mw[12], dy = w[13]-mw[13], dz = w[14]-mw[14];
                        DbgLog("[muzzle] bone='%s' hand=(%.0f,%.0f,%.0f) model=(%.0f,%.0f,%.0f) fire=(%.0f,%.0f,%.0f) dist=%.0f clip=%s/%s fw=%d anim=%d",
                               attachBoneName.c_str(), hand[12], hand[13], hand[14], mw[12], mw[13], mw[14], w[12], w[13], w[14],
                               std::sqrt(dx*dx + dy*dy + dz*dz), pose.clipA.c_str(), pose.clipB.c_str(), (int)(fw != nullptr), (int)animateWeapon);
                    }
                }
            }
            // Weapon nodes the item's <sequence> effects hang from, as drawn.
            if (!preview) {
                for (const ActiveEffect& fx : activeEffects) {
                    if (fx.tag == "costume" || fx.attachNode.empty()) continue;   // item <sequence> and AddInstantEffect
                    float nodeW[16]; bool have = false;
                    if (const scn::Mat4* nw = animateWeapon ? weaponNodes(fx.attachNode) : nullptr) { std::memcpy(nodeW, nw->data(), sizeof(nodeW)); have = true; }
                    if (!have) for (size_t mi = 0; mi < part.scene.models.size(); ++mi) if (part.scene.models[mi].name == fx.attachNode) {
                        std::memcpy(nodeW, mi < part.pieceWorlds.size() ? part.pieceWorlds[mi].data() : part.scene.models[mi].matrix, sizeof(nodeW));
                        have = true; break;
                    }
                    if (!have) { scn::Mat4 tw; if (scn::TrackWorld(part.scene, fx.attachNode, tw, true)) { std::memcpy(nodeW, tw.data(), sizeof(nodeW)); have = true; } }
                    if (!have) for (const scn::Bone& b : part.scene.bones) if (b.name == fx.attachNode) { std::memcpy(nodeW, b.matrix, sizeof(nodeW)); have = true; break; }
                    if (have) { std::array<float, 16> c; Mat4MulS(nodeW, toHand, c.data()); weaponNodeChar[fx.attachNode] = c; }
                }
            }
            // Ghost trail: Line01 (local to its parent piece) as the blade is
            // drawn this frame, sampled while the state's AddGhostTrail window
            // is open. The beta's CWeapon makes one trail per weapon file 0.
            if (!preview && ghostTrail && part.sceneIndex == 0 && !part.scene.lines.empty()) {
                const unsigned tnowG = GetTickCount();
                const unsigned ageG = tnowG - ghostStart;
                const scn::Line* ln = nullptr;
                for (const scn::Line& l : part.scene.lines) if (l.name == "Line01" && l.pts.size() >= 6) { ln = &l; break; }
                if (ln && ageG >= (unsigned)ghostTrail->startMs && ageG <= (unsigned)ghostTrail->endMs) {
                    float parentW[16]; bool haveParent = false;
                    if (const scn::Mat4* pw = animateWeapon ? weaponNodes(ln->parent) : nullptr) { std::memcpy(parentW, pw->data(), sizeof(parentW)); haveParent = true; }
                    if (!haveParent) for (size_t mi = 0; mi < part.scene.models.size(); ++mi) if (part.scene.models[mi].name == ln->parent) {
                        std::memcpy(parentW, mi < part.pieceWorlds.size() ? part.pieceWorlds[mi].data() : part.scene.models[mi].matrix, sizeof(parentW));
                        haveParent = true; break;
                    }
                    if (!haveParent) for (const scn::Bone& b : part.scene.bones) if (b.name == ln->parent) { std::memcpy(parentW, b.matrix, sizeof(parentW)); haveParent = true; break; }
                    if (haveParent) {
                        float lw[16], a[16], w[16];
                        Mat4MulS(ln->matrix, parentW, lw);
                        Mat4MulS(lw, toHand, a);
                        Mat4MulS(a, place, w);
                        GhostSample s; s.t = tnowG;
                        for (int e = 0; e < 2; ++e) {
                            const float* p = &ln->pts[e * 3];
                            float* o = e == 0 ? s.a : s.b;
                            o[0] = p[0]*w[0] + p[1]*w[4] + p[2]*w[8]  + w[12];
                            o[1] = p[0]*w[1] + p[1]*w[5] + p[2]*w[9]  + w[13];
                            o[2] = p[0]*w[2] + p[1]*w[6] + p[2]*w[10] + w[14];
                        }
                        ghostSamples.push_back(s);
                    }
                }
            }
            // Weapon bones can hang off a weapon MODEL (counter sword: the claw
            // fingers' Counter_Sword_Skin_Dummy is a child of the gauntlet piece
            // Counter_Sword). Give the bone resolver those pieces as external
            // parents - current (animated) and bind - so the skinned claw is
            // deformed in place on the gauntlet instead of off the origin.
            std::unordered_map<std::string, scn::Mat4> pieceNow, pieceBind;
            for (size_t mi = 0; mi < part.scene.models.size(); ++mi) {
                const scn::Model& pm = part.scene.models[mi];
                scn::Mat4 bindM; std::memcpy(bindM.data(), mi < part.pieceWorlds.size() ? part.pieceWorlds[mi].data() : pm.matrix, sizeof(bindM));
                pieceBind[pm.name] = bindM;
                const scn::Mat4* animated = animateWeapon ? weaponNodes(pm.name) : nullptr;
                pieceNow[pm.name] = animated ? *animated : bindM;
            }
            std::vector<scn::Mat4> weaponBoneWorld;
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
                    if (weaponBoneWorld.empty())
                        weaponBoneWorld = scn::ResolveBoneWorlds(part.scene, weaponPose, nullptr, &pieceNow, nullptr, &pieceBind);
                    const std::vector<float> flat = scn::SkinModelW(m, part.scene, weaponBoneWorld);
                    skinned.resize(flat.size() / 3);
                    for (size_t k = 0; k < skinned.size(); ++k) { skinned[k].x = flat[k*3]; skinned[k].y = flat[k*3+1]; skinned[k].z = flat[k*3+2]; }
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
                // The weapon's own effect meshes (taser plasma's "Plane01 as uvani",
                // "Plan as morph vertexani", blinking glows): their clip loops on the
                // weapon like map and .seq meshes do - visibility, vertex and UV keys.
                std::vector<scn::Vec3> fxPos; std::vector<scn::Vec2> fxUv;
                const scn::Vec2* uvSrc = hasUv ? m.uv1.data() : nullptr;
                float fxVis = 1.f;
                if (m.skin.empty() && !m.anims.empty()) {
                    const uint32_t fxTick = (uint32_t)((double)GetTickCount() * 4.8);
                    const std::string& fxClip = weaponPose.clipA;
                    fxVis = (std::max)(0.f, (std::min)(1.f, scn::ModelVisibility(m, fxClip, fxTick, true)));
                    if (fxVis <= 0.004f) continue;
                    if (scn::SampleMorphPositions(m, fxClip, fxTick, true, fxPos) && fxPos.size() == nv) verts = fxPos.data();
                    if (hasUv && scn::SampleMorphUvs(m, fxClip, fxTick, true, fxUv) && fxUv.size() == nv) uvSrc = fxUv.data();
                }
                std::vector<float> pu(nv * 5);
                for (size_t i = 0; i < nv; ++i) {
                    pu[i*5+0]=verts[i].x; pu[i*5+1]=verts[i].y; pu[i*5+2]=verts[i].z;
                    pu[i*5+3]= uvSrc ? uvSrc[i].u : 0.f;  pu[i*5+4]= uvSrc ? uvSrc[i].v : 0.f;
                }
                std::vector<unsigned short> idx; idx.reserve(m.faces.size() * 3);
                for (const scn::Face& f : m.faces) { idx.push_back(f.a); idx.push_back(f.b); idx.push_back(f.c); }
                // Render states from the material name, as for effect meshes
                // (alphablend2 = additive glow, alphablend1 = blend, nodepthwrite, nocull).
                std::string lname = m.name; for (char& c : lname) c = (char)tolower((unsigned char)c);
                uint32_t flags = m.renderFlag;
                if (lname.find("alphablend2") != std::string::npos) flags |= 0x40000000u;
                else if (lname.find("alphablend1") != std::string::npos) flags |= 2u;
                if (lname.find("nodepthwrite") != std::string::npos) flags |= 64u;
                r->meshFade = fxVis;
                r->DrawMesh(pu.data(), (int)nv, idx.data(), (int)idx.size(), tex, world, flags);
                r->meshFade = 1.f;
            }
        }
        }
}

void WorldState::OnRender(IRenderer* r) {
    if (!r) return;
    if (r->quality != gGraphicsQuality) r->RequestQuality(gGraphicsQuality);   // applied at the next BeginFrame
    scrW = r->width; scrH = r->height;
    // S4 third-person camera (FUN_01202ad0): the eye is rigidly actor-relative
    // (no position smoothing/lag — direct mouse-look). Only FOV lerps. Look-at =
    // actorPos + heightoffset; eye = lookAt - forward(yaw,pitch)*depth + right*side.
    // Q free look adds its own yaw/pitch on top (camera only).
    const float viewYaw = camYaw + lookYaw;
    const float viewPitch = (std::max)(kPitchUp, (std::min)(kPitchDown, camPitch + lookPitch));
    float cp = std::cos(viewPitch);
    float fwd[3]   = { std::sin(viewYaw)*cp, -std::sin(viewPitch), std::cos(viewYaw)*cp };
    float right[3] = { std::cos(viewYaw), 0.f, -std::sin(viewYaw) };
    // Shoulder swap eases like the FOV (S10 camera speed 1.75 per 100 ms),
    // the same at any frame rate.
    camSideCur += (camSideTarget - camSideCur) * (1.f - std::exp(-lastDt * 1000.f * 0.01f * 1.75f));
    // Client camera offsets (heightoffset 350 / sideoffset 130): the view centre
    // - the crosshair - sits above the head with the actor off to one side,
    // and Z swaps the shoulder; the shots fly through that centre.
    const float heightOff = kCamHeightOffset, sideOff = kCamSideOffset * camSideCur;
    // Over-the-shoulder: the look-at point moves sideways WITH the eye, so the
    // view centre (the crosshair) is the straight line the shots follow and
    // the actor sits off to the other side (Z swaps it).
    float at[3]  = { charPos[0] + right[0]*sideOff, charPos[1] + heightOff, charPos[2] + right[2]*sideOff };
    float eye[3] = { at[0] - fwd[0]*camDist, at[1] - fwd[1]*camDist, at[2] - fwd[2]*camDist };
    // Camera collision: looking up swings the eye down behind the actor, into
    // the floor. Walk the eye in along its own line until the map is not in
    // between (kept 25 units off the surface, never closer than 80 to the
    // look-at point). The shot origin uses the same distance (camDistEff).
    {
        const float back[3] = { -fwd[0], -fwd[1], -fwd[2] };
        float hitT = 0.f, d = camDist;
        if (RayCollTris(collTris, at, back, camDist, hitT)) d = (std::max)(80.f, hitT - 25.f);
        // Smooth the pull-in so crossing an edge does not snap the view.
        camDistEff += (d - camDistEff) * (d < camDistEff ? 1.f : (1.f - std::exp(-lastDt * 1000.f * 0.012f)));
        if (camDistEff <= 0.f || camDistEff > camDist) camDistEff = d;
        for (int k = 0; k < 3; ++k) eye[k] = at[k] - fwd[k]*camDistEff;
    }
    // S10 camera (sub_F48780, ActorCameraNormalFov/ActorCameraFastRunFov):
    // norfov 60 (camera+340), FOV speed 1.75 (camera+348) applied as
    // alpha = dt_ms * 0.01 * 1.75 toward the target each frame - near
    // instant (~0.2 s). Run FOV 80 (the beta's CFastRunState::OnEnter value,
    // sub_435BD0(80, ...); the S10 default runfov 72 is overridden by its
    // camera_dev.ini, which this resource set lacks). Exponential form so the
    // change takes the same time at any frame rate.
    const bool fastRun = (animCtrl.state() == anim::STATE_FASTRUN);
    const float fovTarget = fastRun ? 80.f : 60.f;
    const float fovAlpha = 1.f - std::exp(-lastDt * 1000.f * 0.01f * 1.75f);
    curFov += (fovTarget - curFov) * fovAlpha;
    // Camera shake (SetCameraShake*): a landed hit, or the state's own timed
    // one (shakePendingAt). Eye and look-at move together, fading out.
    {
        const unsigned rnow = GetTickCount();
        if (shakePendingAt && rnow >= shakePendingAt) { shakeStart = rnow; shakeEnd = rnow + 300; shakeAmp = shakePendingAmp; shakePendingAt = 0; }
        if (rnow < shakeEnd && shakeAmp > 0.f && shakeEnd > shakeStart) {
            const float k = 1.f - float(rnow - shakeStart) / float(shakeEnd - shakeStart);
            const float ox = std::sin(rnow * 0.09f) * shakeAmp * k, oy = std::cos(rnow * 0.11f) * shakeAmp * k * 0.7f;
            for (float* p : { eye, at }) { p[0] += right[0] * ox; p[1] += oy; p[2] += right[2] * ox; }
        }
    }
    r->SetCamera3D(eye, at, curFov, 5.f, 300000.f);
    std::memcpy(camEyeNow, eye, sizeof(camEyeNow)); std::memcpy(camAtNow, at, sizeof(camAtNow));

    // A new map: drop the previous map's GPU buffers (built again as drawn).
    if (mapCacheDirty) { r->ReleaseStaticMeshes(); mapMeshCache.clear(); mapCacheDirty = false; }
    for (size_t mi = 0; mi < map.size(); ++mi)
        DrawMapScene(r, map[mi], GetTickCount(), (int)mi == mapSkyIdx ? eye : nullptr, &mapMeshCache);

    if (loaded) {
        // curPose is produced each frame by the AnimController (gates + timers + blend).
        // The model's authored forward axis is -Z; the native client applies
        // this fixed correction when placing the assembled character.
        float place[16]; BuildYawTranslate(charYaw + 3.14159265f, charPos, place);
        std::vector<CharPiece> pieces;
        // Worn Iron Boots replace the foot and leg parts: the costume's skin
        // poked through the slimmer ironboots_01.
        const unsigned hideSlot = weaponOk ? WornSlotMask() : 0u;
        GatherCharacterPieces(character, bodyParts, curPose, pieces, curLowerPoseActive ? &curLowerPose : nullptr, hideSlot);
        if (weaponOk) GatherCharacterPieces(character, weaponWornParts, curPose, pieces, curLowerPoseActive ? &curLowerPose : nullptr);
        DrawCharPieces(r, pieces, place);
        DrawPeers(r);
        DrawClones(r);

        DrawWeaponParts(r, curPose, place, false, curLowerPoseActive ? &curLowerPose : nullptr);
    }
    DrawEffects(r, GetTickCount());
    // Memory: what the current map, outfit and weapons no longer draw is let go
    // (textures unused for 30 s - reloaded by name if drawn again; the looks and
    // weapons of players no longer here).
    {
        static unsigned nextTrim = 0;
        const unsigned now = GetTickCount();
        if (now >= nextTrim) {
            nextTrim = now + 10000;
            r->TrimTextures(30000);
            std::set<std::string> lookKeys, weaponKeys;
            if (mpClient.Connected())
                for (auto& kv : mpClient.Peers()) { lookKeys.insert(kv.second.equip); weaponKeys.insert(kv.second.weapon + "|" + kv.second.weaponStates); }
            for (auto it = peerLooks.begin(); it != peerLooks.end(); ) it = lookKeys.count(it->first) ? std::next(it) : peerLooks.erase(it);
            for (auto it = peerWeapons.begin(); it != peerWeapons.end(); ) it = weaponKeys.count(it->first) ? std::next(it) : peerWeapons.erase(it);
            static unsigned nextMemLog = 0;
            if (now >= nextMemLog) { nextMemLog = now + 60000; crash::LogMemory("periodic"); }
        }
    }
    DrawGhostTrail(r, GetTickCount());
    // Bullets of guns without a beam sequence (see below).
    {
        const unsigned now = GetTickCount();
        // Bullets of guns without a tracer .seq (beta CGunTracerNode, sub_730730 /
        // sub_730320 / sub_730530): a white quad, width 4, alpha 0.75, no
        // texture, from the muzzle (10% down the shot) to the target, on for
        // 100 ms; every frame its tail is pulled toward the target by (1 - t^2)
        // of what is left (t = age / 100 ms), so the line collapses on the
        // impact - a short white stub near the target, not a laser. The
        // per-frame product is evaluated at 60 steps/s (as at the client's
        // usual frame rate).
        const float kLifeMs = 100.f, kStepMs = 1000.f / 60.f;
        gunTracers.erase(std::remove_if(gunTracers.begin(), gunTracers.end(), [&](const GunTracer& g) {
            return (float)(now - g.born) >= kLifeMs;
        }), gunTracers.end());
        for (const GunTracer& g : gunTracers) {
            const float age = (float)(now - g.born);
            float left = 1.f;
            for (float tk = kStepMs; tk <= age; tk += kStepMs) { const float x = tk / kLifeMs; left *= (1.f - x * x); }
            float start[3], tail[3];
            // The client starts 10% down the shot; at a far target that is
            // hundreds of units off the gun, so cap it to a short fixed lead.
            const float dl[3] = { g.b[0]-g.a[0], g.b[1]-g.a[1], g.b[2]-g.a[2] };
            const float shotLen = std::sqrt(dl[0]*dl[0] + dl[1]*dl[1] + dl[2]*dl[2]);
            const float lead = shotLen > 1.f ? (std::min)(0.1f, 25.f / shotLen) : 0.f;
            for (int k = 0; k < 3; ++k) start[k] = g.a[k] + dl[k] * lead;
            for (int k = 0; k < 3; ++k) tail[k] = g.b[k] - (g.b[k] - start[k]) * left;
            const float seg[6] = { tail[0], tail[1], tail[2], g.b[0], g.b[1], g.b[2] };
            r->DrawBeam(seg, 2, 4.f, 0xBFFFFFFF, -1, false, 0.f, 1.f);
        }
        // Tracer beams (CLightningNode, S1 sub_63C780/sub_63C930): UnitCount+2
        // points from the gun's fire node to the hit; on while the node is
        // active (begin..end), fading in over FadeIn and out over FadeOut ms;
        // the texture scrolls UVAnimSpeed per second.
        if (!gunBeams.empty() && gunMuzzleValid) {
            uint32_t life = gunBeamDuration;
            for (const BeamNode& bn : gunBeamNodes) life = (std::max)(life, bn.end + bn.beam.fadeOut);
            gunBeams.erase(std::remove_if(gunBeams.begin(), gunBeams.end(), [&](const GunBeam& g) { return now - g.born > life; }), gunBeams.end());
            std::vector<float> pts;
            for (const GunBeam& g : gunBeams) {
                const unsigned age = now - g.born;
                for (const BeamNode& bn : gunBeamNodes) {
                    if (age < bn.begin) continue;
                    float a = 1.f;
                    if (age <= bn.end) { if (bn.beam.fadeIn) a = (std::min)(1.f, (float)(age - bn.begin) / (float)bn.beam.fadeIn); }
                    else a = bn.beam.fadeOut ? 1.f - (float)(age - bn.end) / (float)bn.beam.fadeOut : 0.f;
                    if (a <= 0.f) continue;
                    const int n = (int)bn.beam.unitCount + 2;
                    pts.resize((size_t)n * 3);
                    for (int i = 0; i < n; ++i) {
                        const float t = (float)i / (float)(n - 1);
                        for (int k = 0; k < 3; ++k) pts[i*3+k] = gunMuzzle[k] + (g.b[k] - gunMuzzle[k]) * t;
                    }
                    const float u0 = (float)age * bn.beam.uvAnimSpeed / 1000.f;
                    const int tex = r->LoadTexture(bn.beam.texture.c_str());
                    const uint32_t alpha = (uint32_t)(a * 255.f);
                    r->DrawBeam(pts.data(), n, (float)(std::max)(1u, bn.beam.unitWidth), (alpha << 24) | bn.rgb, tex,
                                bn.beam.blendType == 1, u0, u0 + bn.beam.uvScale);
                }
            }
        }
    }
    if (!peerBeams.empty()) {
        const unsigned now = GetTickCount();
        std::vector<float> pts;
        peerBeams.erase(std::remove_if(peerBeams.begin(), peerBeams.end(), [&](const PeerBeam& g) {
            const auto& bn = BeamNodesOf(g.tracer);
            uint32_t life = bn.second;
            for (const BeamNode& n : bn.first) life = (std::max)(life, n.end + n.beam.fadeOut);
            return now - g.born > life;
        }), peerBeams.end());
        for (const PeerBeam& g : peerBeams) {
            const unsigned age = now - g.born;
            for (const BeamNode& bn : BeamNodesOf(g.tracer).first) {
                if (age < bn.begin) continue;
                float a = 1.f;
                if (age <= bn.end) { if (bn.beam.fadeIn) a = (std::min)(1.f, (float)(age - bn.begin) / (float)bn.beam.fadeIn); }
                else a = bn.beam.fadeOut ? 1.f - (float)(age - bn.end) / (float)bn.beam.fadeOut : 0.f;
                if (a <= 0.f) continue;
                const int n = (int)bn.beam.unitCount + 2;
                pts.resize((size_t)n * 3);
                for (int i = 0; i < n; ++i) {
                    const float t = (float)i / (float)(n - 1);
                    for (int k = 0; k < 3; ++k) pts[i*3+k] = g.a[k] + (g.b[k] - g.a[k]) * t;
                }
                const float u0 = (float)age * bn.beam.uvAnimSpeed / 1000.f;
                const int tex = r->LoadTexture(bn.beam.texture.c_str());
                r->DrawBeam(pts.data(), n, (float)(std::max)(1u, bn.beam.unitWidth), ((uint32_t)(a * 255.f) << 24) | bn.rgb, tex,
                            bn.beam.blendType == 1, u0, u0 + bn.beam.uvScale);
            }
        }
    }
    if (mpOnline && mpClient.Connected() && mapIdx >= 0 && mapIdx < (int)maps.size()) {
        // Every other player's HP over their head, with their name: the same
        // perspective as SetCamera3D (LookAtLH, vertical FOV curFov).
        float zx[3] = { camAtNow[0]-camEyeNow[0], camAtNow[1]-camEyeNow[1], camAtNow[2]-camEyeNow[2] };
        const float zl = std::sqrt(zx[0]*zx[0] + zx[1]*zx[1] + zx[2]*zx[2]);
        if (zl > 1e-4f) {
            for (float& c : zx) c /= zl;
            float xx[3] = { zx[2], 0.f, -zx[0] };   // up x z
            const float xl = std::sqrt(xx[0]*xx[0] + xx[2]*xx[2]);
            if (xl > 1e-4f) { xx[0] /= xl; xx[2] /= xl; }
            const float yy[3] = { zx[1]*xx[2] - zx[2]*xx[1], zx[2]*xx[0] - zx[0]*xx[2], zx[0]*xx[1] - zx[1]*xx[0] };
            const float tv = std::tan(curFov * 0.5f * 3.14159265f / 180.f);
            const float aspect = r->height > 0 ? (float)r->width / (float)r->height : 4.f / 3.f;
            for (auto& kv : mpClient.Peers()) {
                const mp::Peer& p = kv.second;
                if (!p.hasPos || p.map != maps[mapIdx].name) continue;
                const float v[3] = { p.rx - camEyeNow[0], p.ry + 330.f - camEyeNow[1], p.rz - camEyeNow[2] };
                const float vz = v[0]*zx[0] + v[1]*zx[1] + v[2]*zx[2];
                if (vz < 10.f) continue;
                const float sx = (v[0]*xx[0] + v[1]*xx[1] + v[2]*xx[2]) / (vz * tv * aspect);
                const float sy = (v[0]*yy[0] + v[1]*yy[1] + v[2]*yy[2]) / (vz * tv);
                if (std::fabs(sx) > 1.2f || std::fabs(sy) > 1.2f) continue;
                const int px = (int)((sx + 1.f) * 0.5f * r->width), py = (int)((1.f - sy) * 0.5f * r->height);
                const int bw = 64, bh = 6;
                const float f = (std::max)(0.f, (std::min)(1.f, p.hp / 100.f));
                r->DrawRect(px - bw/2 - 1, py - 1, bw + 2, bh + 2, 0xC0000000);
                r->DrawRect(px - bw/2, py, (int)(bw * f), bh, f > 0.5f ? 0xFF3CC85A : f > 0.25f ? 0xFFE0B030 : 0xFFE03030);
                r->DrawText(px - 80, py - 18, 160, 16, (p.nickname + "  " + std::to_string((int)std::ceil((std::max)(0.f, p.hp)))).c_str(), 12, 0xFFFFFFFF, 1);
            }
        }
    }
    if (mpOnline && mpClient.Connected()) {
        // HP bar, bottom centre (starts at 100).
        const int bw = (std::max)(240, r->width / 4), bh = 16, bx = (r->width - bw) / 2, by = r->height - bh - 28;   // bottom centre, like the client's HP
        const float f = (std::max)(0.f, (std::min)(1.f, myHp / myHpMax));
        const bool flash = GetTickCount() < hurtFlashUntil;
        r->DrawRect(bx - 2, by - 2, bw + 4, bh + 4, 0xC0000000);
        r->DrawRect(bx, by, bw, bh, 0xFF3A1010);
        r->DrawRect(bx, by, (int)(bw * f), bh, flash ? 0xFFFFFFFF : (f > 0.5f ? 0xFF3CC85A : f > 0.25f ? 0xFFE0B030 : 0xFFE03030));
        r->DrawText(bx, by - 1, bw, bh + 2, ("HP " + std::to_string((int)std::ceil((std::max)(0.f, myHp))) + " / " + std::to_string((int)myHpMax)).c_str(), 13, 0xFFFFFFFF, 1);
    }
    if (loaded && weaponOk) {
        // The weapon's own crosshair (crosshair_file -> image/weapon/<name>_1.png)
        // at the screen centre (HudLayout), scaled from 1024x768.
        const HudLayout& hud = GetHudLayout();
        const float sy = (float)r->height / 768.f, sx = (float)r->width / 1024.f;
        const int cx = (int)(hud.xhairCx * sx), cy = (int)(hud.xhairCy * sy);
        int xt = -1;
        if (!weaponCrosshair.empty()) xt = r->LoadTexture((RES("resources/resources/image/weapon/") + weaponCrosshair + "_1.png").c_str());
        if (xt >= 0) {
            // 64x64 layout units (the 128 png at half size): measured against the
            // original, the crosshair spans ~0.77 of the arc's radius.
            const int w = (int)(64.f * sy), h = w;
            r->DrawImage(cx - w / 2, cy - h / 2, w, h, xt, 0, 0, 1, 1);
        } else {
            r->DrawRect(cx - 9, cy - 1, 7, 2, 0xE0FFFFFF); r->DrawRect(cx + 3, cy - 1, 7, 2, 0xE0FFFFFF);
            r->DrawRect(cx - 1, cy - 9, 2, 7, 0xE0FFFFFF); r->DrawRect(cx - 1, cy + 3, 2, 7, 0xE0FFFFFF);
        }
        if (weaponAnim.isGun && gunMag > 0) {
            // Ammo arc (FUN_013b2870): bullet_empty behind; box A lights the
            // first N = ammo*10/magazine segments; while reloading boxes B and
            // C, C filling the rest with the reload progress.
            const int al = cx + (int)((hud.arcL - hud.xhairCx) * sy), at = cy + (int)((hud.arcT - hud.xhairCy) * sy);   // kept around the crosshair on wide screens
            const int aw = (int)((hud.arcR - hud.arcL) * sy), ah = (int)((hud.arcB - hud.arcT) * sy);
            auto img = [&](const std::string& tex) {
                if (tex.empty()) return;
                const int t = r->LoadTexture((RES("resources/resources/gui/new/") + tex).c_str());
                if (t >= 0) r->DrawImage(al, at, aw, ah, t, 0, 0, 1, 1);
            };
            img(hud.empty);
            const int n = (std::min)(10, gunAmmo * 10 / gunMag);
            if (!gunReloading) {
                for (int i = 0; i < n; ++i) img(hud.box[0][i]);
            } else {
                for (int i = 0; i < n; ++i) img(hud.box[1][i]);
                const float prog = gunReloadMs ? (std::min)(1.f, (float)(GetTickCount() - (gunReloadEnd - gunReloadMs)) / (float)gunReloadMs) : 1.f;
                const int fill = (int)((float)(10 - (n - 1)) * prog);
                for (int i = n; i < (std::min)(10, n + fill); ++i) img(hud.box[2][i]);
            }
        }
    }
    if (loaded && weaponOk && weaponAnim.isGun) {
        // Ammo counter (the shots go where the crosshair points).
        const std::string ammo = gunReloading ? std::string("Reloading...") : std::to_string(gunAmmo) + " / " + std::to_string(gunMag) + "   (R reload)";
        r->DrawText(r->width - 330, r->height - 44, 320, 30, ammo.c_str(), 22, (gunAmmo > 0 || gunReloading) ? 0xFFFFFFFF : 0xFFFF6060);
    }
    if (weaponOk && boneEditMode) {   // the bone editor's own line (F1)
        const std::string mount = weaponParts.empty() ? weaponBone :
            (weaponParts[0].idleBoneOverride.empty() ? weaponParts[0].attackBone : weaponParts[0].idleBoneOverride);
        const std::string help = boneEditMode
            ? " [EDITING: L/R primary, Shift+L/R secondary, Backspace reset, Ctrl+Shift+S save, F1 exit]"
            : " [F1 edit bones]";
        r->DrawText(10, 32, 1200, 24, ("Weapon: " + weaponStates + " mount=" + mount + help).c_str(), 14, 0xFFB0E0FF);
    }
    int fps = lastDt > 1e-5f ? (int)(1.f / lastDt) : 0;
    if (weaponOk && weaponHolstered)
        r->DrawText(10, 56, 600, 20, ("Holster roll: " + std::to_string((gHolsterRoll45 - 4) * 45) + " deg   ( [ and ] turn the grip side )").c_str(), 14, 0xFFFFCC66);
    r->DrawText(10, 10, 400, 24, ("FPS: " + std::to_string(fps) + "  (F2 = cap 60/144/off)").c_str(), 15, 0xFF66FF66);
    {   // music box (off by default; ESC > Settings, F5, F6)
        const int x = (std::max)(10, r->width - 420);
        r->DrawRect(x, 58, 410, 48, 0xB0101824);
        if (!gMusicOn) {
            r->DrawText(x + 10, 62, 390, 20, "Music: off", 14, 0xFFFFFFFF);
            r->DrawText(x + 10, 82, 390, 20, "F5 on | F6 choose track | ESC > Settings | F7 Sex", 13, 0xFF80D8FF);
        } else {
            std::string title = gMusicTrack;
            if (title.empty() && mapIdx >= 0 && mapIdx < (int)maps.size() && bgmIndex >= 0 && bgmIndex < (int)maps[mapIdx].bgm.size())
                title = maps[mapIdx].bgm[bgmIndex];
            r->DrawText(x + 10, 62, 390, 20, ("Music: " + Basename(title)).c_str(), 14, 0xFFFFFFFF);
            r->DrawText(x + 10, 82, 390, 20, "F5 off | F6 Next | Shift+F6 Prev | F7 Sex", 13, 0xFF80D8FF);
        }
    }

    // Centered aim crosshair (replaces the hidden Windows cursor) - only
    // without a weapon: a weapon draws its own crosshair above.
    if (!(loaded && weaponOk)) {
        const int cx = r->width / 2, cy = r->height / 2;
        const uint32_t col = 0xE000FF66;   // translucent green
        r->DrawRect(cx-1, cy-1, 2, 2, col);      // center dot
        r->DrawRect(cx-11, cy, 7, 2, col);       // left arm
        r->DrawRect(cx+5,  cy, 7, 2, col);       // right arm
        r->DrawRect(cx, cy-11, 2, 7, col);       // top arm
        r->DrawRect(cx, cy+5,  2, 7, col);       // bottom arm
    }

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
        const int W = 520, slotsH = 3 * 22 + 10, H = kMapMenuRows * 20 + 60 + slotsH;
        const int x = (r->width - W) / 2, y = (r->height - H) / 2;
        r->DrawRect(x, y, W, H, 0xE00A121C);
        r->DrawRect(x, y, W, 26, 0xFF1C4A6E);
        r->DrawText(x + 12, y + 5, W - 24, 20,
                    ("Loadout (" + std::to_string(weaponList.size()) + " weapons)  1/2/3 slot - arrows weapon - Enter assigns - F4 closes").c_str(),
                    13, 0xFFDDEEFF);
        // The 3 slots, the one being edited highlighted.
        for (int sIdx = 0; sIdx < 3; ++sIdx) {
            const int sy = y + 32 + sIdx * 22;
            const bool target = sIdx == wpnTargetSlot;
            if (target) r->DrawRect(x + 4, sy - 2, W - 8, 21, 0x80FFAA33);
            const std::string name = sIdx < (int)gLoadout.size() ? gLoadout[sIdx].states + "   " + gLoadout[sIdx].scn : "(empty)";
            r->DrawText(x + 14, sy, W - 28, 20, ("Slot " + std::to_string(sIdx + 1) + (sIdx == weaponSlot ? " (equipped): " : ": ") + name).c_str(),
                        14, target ? 0xFFFFE0A0 : 0xFFB0C4D8);
        }
        const int listY = y + 32 + slotsH;
        for (int i = 0; i < kMapMenuRows && wpnTop + i < (int)weaponList.size(); ++i) {
            const WeaponEntry& e = weaponList[wpnTop + i];
            const int ry = listY + i * 20;
            if (wpnTop + i == wpnSel)            r->DrawRect(x + 4, ry - 2, W - 8, 20, 0x8033AAFF);
            if (e.states == weaponStates)        r->DrawRect(x + 4, ry - 2, 3,     20, 0xFF66FF88);
            r->DrawText(x + 14,  ry, 210, 20, e.states.c_str(), 14, 0xFFFFFFFF);
            r->DrawText(x + 230, ry, 230, 20, e.scn.c_str(),    12, 0xFF88AACC);
        }
    }

    // Online status, always visible while online.
    if (mpOnline) {
        const std::string st = mpClient.Connected()
            ? "Online  (" + std::to_string(mpClient.Peers().size()) + " other players)"
            : std::string("Connecting...");
        r->DrawText(10, 78, 700, 20, st.c_str(), 14, mpClient.Connected() ? 0xFF66FF88 : 0xFFFFCC66);
    }

    // ESC menu, main page.
    if (mapMenu && menuPage == 0) {
        const char* const labelsFixed[9] = { nullptr, "Play solo", "Change map", "Inventory", "Weapons (F4)", nullptr, nullptr, "Back to game", "Quit" };
        const int W = 520, H = 9 * 30 + 70;
        const int x = (r->width - W) / 2, y = (r->height - H) / 2;
        r->DrawRect(x, y, W, H, 0xE00A121C);
        r->DrawRect(x, y, W, 26, 0xFF1C4A6E);
        r->DrawText(x + 12, y + 5, W - 24, 20, ("projectoS10  -  " + gOnline.nickname + "      Arrows/mouse + Enter, ESC back").c_str(), 14, 0xFFDDEEFF);
        for (int i = 0; i < 9; ++i) {
            std::string label;
            if (i == 0)
                label = !mpOnline ? std::string("Play online")
                      : mpClient.Connected() ? "Online - Enter to disconnect"
                      : "Connecting... - Enter to cancel";
            else if (i == 5) label = std::string("Character: ") + (female ? "Female" : "Male") + "  (Enter switches)";
            else if (i == 6) label = "Settings";
            else label = labelsFixed[i];
            const int ry = y + 40 + i * 30;
            if (i == mainSel) r->DrawRect(x + 6, ry - 4, W - 12, 28, 0x8033AAFF);
            r->DrawText(x + 24, ry, W - 48, 22, label.c_str(), 17, i == 0 && mpOnline ? 0xFF66FF88 : 0xFFFFFFFF);
        }
    }

    // ESC menu, inventory page: S4 equip window (boxes around the character,
    // the 3 weapons below) + the item grid of the selected box.
    if (mapMenu && menuPage == 2) {
        UiRect boxes[12], panel, grid; InvLayout(boxes, panel, grid);
        auto frame = [&](const UiRect& q, uint32_t c, int t) {
            r->DrawRect(q.x, q.y, q.w, t, c); r->DrawRect(q.x, q.y + q.h - t, q.w, t, c);
            r->DrawRect(q.x, q.y, t, q.h, c); r->DrawRect(q.x + q.w - t, q.y, t, q.h, c);
        };
        auto icon = [&](const std::string& file, const UiRect& q, int inset) -> bool {
            if (file.empty()) return false;
            const int tex = r->LoadTexture(file.c_str());
            if (tex < 0) return false;
            r->DrawImage(q.x + inset, q.y + inset, q.w - 2 * inset, q.h - 2 * inset, tex, 0.f, 0.f, 1.f, 1.f);
            return true;
        };
        // Equip window: only the frame and the boxes, the middle stays clear for the character.
        r->DrawRect(0, 0, scrW, scrH, 0xFF0B0914);                      // full screen: hide the world
        r->DrawRect(panel.x, panel.y, panel.w, panel.h, 0xFF15111F);
        frame(panel, 0xFF4A3D6E, 2);
        r->DrawText(panel.x, panel.y + 8, panel.w, 24, (std::string("INVENTORY  -  ") + (female ? "FEMALE" : "MALE")).c_str(), 18, 0xFF66E0FF, 1);
        // Character preview: own viewport, black background, front camera,
        // idle loop. Only the character - no map behind it.
        if (loaded) {
            const UiRect pv = InvPreviewRect(boxes, panel);
            r->SetViewport(pv.x, pv.y, pv.w, pv.h);
            r->ClearViewport(0xFF000000);
            // Distance so ~300 units (feet to above the hair; the bip head is
            // at ~262) fill the height, or ~190 the width when the panel is narrow.
            const float tanHalf = std::tan(30.f * 3.14159265f / 180.f), aspect = (float)pv.w / (float)(std::max)(1, pv.h);
            const float dist = (std::max)(300.f / (2.f * tanHalf), 190.f / (2.f * tanHalf * aspect));
            const float pvAt[3] = { 0.f, 140.f, 0.f }, pvEye[3] = { 0.f, 150.f, dist };
            r->SetCamera3D(pvEye, pvAt, 60.f, 5.f, 20000.f);
            scn::AnimPose pose;
            // Weapon boxes: the weapon's standing idle - the NORMAL row the
            // controller loaded from NormalState_<weapon> (hands on its grip),
            // the same clip it plays standing in the map. Clothes boxes keep
            // the plain 00074 standing pose.
            pose.clipA = "00074";
            if (weaponOk && invBox >= 8 && invBox <= 10)
                if (const std::vector<anim::SAnimParam>* rows = animCtrl.StateRows(anim::STATE_NORMAL))
                    for (const anim::SAnimParam& row : *rows)
                        if (!row.clip.empty() && CharacterHasClip(character, row.clip)) { pose.clipA = row.clip; break; }
            pose.tickA = (uint32_t)((GetTickCount() - animStart) * animCtrl.speedScale);
            pose.loopA = true;
            const float origin[3] = { 0.f, 0.f, 0.f };
            float place[16]; BuildYawTranslate(invPreviewYaw + 3.14159265f, origin, place);
            std::vector<CharPiece> pieces;
            GatherCharacterPieces(character, bodyParts, pose, pieces, nullptr,
                                  (weaponOk && invBox >= 8 && invBox <= 10) ? WornSlotMask() : 0u);
            if (weaponOk && invBox >= 8 && invBox <= 10) GatherCharacterPieces(character, weaponWornParts, pose, pieces, nullptr);
            DrawCharPieces(r, pieces, place);
            if (invBox >= 8 && invBox <= 10) DrawWeaponParts(r, pose, place, true);   // weapon boxes only: the weapon in hand
            DrawEffects(r, GetTickCount(), place, &pose);   // worn-costume effects on the preview too
            r->SetViewport(0, 0, 0, 0);
            r->SetCamera3D(eye, at, curFov, 5.f, 300000.f);
        }
        for (int bx = 0; bx < 12; ++bx) {
            const UiRect& q = boxes[bx];
            const bool off = bx == 11;
            r->DrawRect(q.x, q.y, q.w, q.h, off ? 0xC0141020 : 0xE0221C34);
            frame(q, bx == invBox ? 0xFFFFAA33 : 0xFF3A3256, bx == invBox ? 3 : 1);
            bool drawn = false;
            if (bx >= 8 && bx <= 10) {
                const int j = bx - 8;
                if (j < (int)gLoadout.size()) {
                    const int wi = InvEquippedIndex(bx);
                    const std::vector<int> wv = InvWeaponView();
                    if (wi >= 0 && wi < (int)wv.size() && !weaponItems[wv[wi]].icon.empty()) drawn = icon(weaponItems[wv[wi]].icon, q, 6);
                    if (!drawn) { auto it = weaponIcons.find(gLoadout[j].states); drawn = it != weaponIcons.end() && icon(it->second, q, 6); }
                    if (!drawn) r->DrawText(q.x + 2, q.y, q.w - 4, q.h, gLoadout[j].states.c_str(), 11, 0xFFB0C4D8, 1);
                    drawn = true;
                }
            } else if (kBoxCostume[bx] >= 0) {
                const int eq = InvEquippedIndex(bx);
                const auto& v = costumeItems[female][kBoxCostume[bx]];
                if (eq > 0 && eq < (int)v.size()) drawn = icon(v[eq].icon, q, 6);
            }
            if (!drawn) r->DrawText(q.x, q.y, q.w, q.h, kBoxLabel[bx], 13, off ? 0xFF4A4458 : 0xFF8A84A0, 1);
        }
        r->DrawText(panel.x, panel.y + panel.h - 24, panel.w, 20, "Click a slot, then an item  |  Tab / arrows + Enter  |  F7 male/female  |  F4 / ESC back",
                    11, 0xFF8A84A0, 1);

        // Item grid of the selected box.
        const int n = InvGridCount();
        const bool weaponsBox = invBox >= 8 && invBox <= 10;
        const std::vector<int> weaponView = weaponsBox ? InvWeaponView() : std::vector<int>();
        const int eq = InvEquippedIndex(invBox);
        r->DrawRect(grid.x, grid.y, grid.w, grid.h, 0xE0181428);
        frame(grid, 0xFF4A3D6E, 2);
        r->DrawText(grid.x + 12, grid.y + 10, grid.w - 24, 24, (std::string(kBoxLabel[invBox]) + "   (" + std::to_string(n) + " items)").c_str(), 16, 0xFF66E0FF);
        if (weaponsBox)
            for (int t = 0; t < 8; ++t) {
                const UiRect tb = InvFilterTab(grid, t);
                const bool on = invWeaponFilter == t - 1;
                r->DrawRect(tb.x, tb.y, tb.w, tb.h, on ? 0xFFFFAA33 : 0xFF2A2440);
                r->DrawText(tb.x, tb.y, tb.w, tb.h, kWeaponFilters[t], 11, on ? 0xFF000000 : 0xFFB0C4D8, 1);
            }
        for (int i = 0; i < kInvCols * kInvRows && invTop + i < n; ++i) {
            const int idx = invTop + i;
            const UiRect c = { grid.x + 10 + (i % kInvCols) * kInvCell, grid.y + kInvGridTop + (i / kInvCols) * kInvCell, kInvCell - 4, kInvCell - 4 };
            r->DrawRect(c.x, c.y, c.w, c.h, idx == invHover ? 0xFF3A3058 : 0xFF2A2440);
            std::string ic, label;
            if (weaponsBox) { const WeaponItem& w = weaponItems[weaponView[idx]]; ic = w.icon; label = w.name; }
            else { const CostumeItem& it = costumeItems[female][kBoxCostume[invBox]][idx]; ic = it.icon; label = it.name; }
            if (!icon(ic, c, 3)) r->DrawText(c.x + 2, c.y, c.w - 4, c.h, label.c_str(), 10, 0xFFB0C4D8, 1);
            if (idx == eq) frame(c, 0xFF66FF88, 2);
            if (idx == invSel[invBox]) frame(c, 0xFFFFAA33, 2);
        }
        // Scroll bar.
        const int totalRows = (n + kInvCols - 1) / kInvCols;
        const UiRect sb = { grid.x + grid.w - 18, grid.y + kInvGridTop, 12, kInvRows * kInvCell };
        r->DrawRect(sb.x, sb.y, sb.w, sb.h, 0xFF141020);
        if (totalRows > kInvRows) {
            const int th = (std::max)(20, sb.h * kInvRows / totalRows);
            const int ty = sb.y + (sb.h - th) * (invTop / kInvCols) / (std::max)(1, totalRows - kInvRows);
            r->DrawRect(sb.x + 1, ty, sb.w - 2, th, 0xFF6A5C9E);
        }
        // Name of the hovered (or selected) item.
        const int shown = invHover >= 0 ? invHover : invSel[invBox];
        if (shown >= 0 && shown < n) {
            const std::string text = weaponsBox ? weaponItems[weaponView[shown]].name + "   " + weaponItems[weaponView[shown]].scn
                : costumeItems[female][kBoxCostume[invBox]][shown].name + "   " + costumeItems[female][kBoxCostume[invBox]][shown].file;
            r->DrawText(grid.x + 12, grid.y + grid.h - 44, grid.w - 24, 20, text.c_str(), 13, 0xFFFFFFFF);
            r->DrawText(grid.x + 12, grid.y + grid.h - 24, grid.w - 24, 20,
                        (std::to_string(shown + 1) + " / " + std::to_string(n) + (shown == eq ? "   (equipped)" : "")).c_str(), 12, 0xFF88AACC);
        }
    }

    // ESC > Settings.
    if (mapMenu && menuPage == 3) {
        const int rows = 7, W = 560, H = rows * 34 + 80;
        const int x = (r->width - W) / 2, y = (r->height - H) / 2;
        r->DrawRect(x, y, W, H, 0xE00A121C);
        r->DrawRect(x, y, W, 26, 0xFF1C4A6E);
        r->DrawText(x + 12, y + 5, W - 24, 20, "Settings      Up/Down - Left/Right or click changes - ESC back", 14, 0xFFDDEEFF);
        const std::string res = gResW > 0 ? std::to_string(gResW) + " x " + std::to_string(gResH) + "  (" + AspectName(gResW, gResH) + ")"
                                          : "Window (" + std::to_string(r->width) + " x " + std::to_string(r->height) + ", free size)";
        const std::string labels[rows] = {
            std::string("Aspect ratio:   < ") + (gAspect == 0 ? "Auto (all)" : kAspects[gAspect]) + " >",
            "Resolution:   < " + res + " >",
            std::string("Display:   < ") + (gBorderless ? "Borderless fullscreen" : "Windowed") + " >",
            std::string("Graphics:   < ") + (gGraphicsQuality == 0 ? "Low" : gGraphicsQuality == 1 ? "Medium" : "High") + " >",
            std::string("Music:   < ") + (gMusicOn ? "On" : "Off") + " >",
            "Track:   < " + (gMusicTrack.empty() ? std::string("map playlist") : Basename(gMusicTrack)) + " >",
            "Back" };
        for (int i = 0; i < rows; ++i) {
            const int ry = y + 44 + i * 34;
            if (i == settingsSel) r->DrawRect(x + 6, ry - 4, W - 12, 30, 0x8033AAFF);
            r->DrawText(x + 24, ry, W - 48, 24, labels[i].c_str(), 17, 0xFFFFFFFF);
        }
        r->DrawText(x + 12, y + H - 26, W - 24, 20, "Saved to config.json. The window can also be dragged to any size.", 12, 0xFF88AACC);
    }

    // ESC map picker: every bginfo-*.ini the client ships, loadable at runtime.
    if (mapMenu && menuPage == 1) {
        const int W = 520, H = kMapMenuRows * 20 + 66;
        const int x = (r->width - W) / 2, y = (r->height - H) / 2;
        r->DrawRect(x, y, W, H, 0xE00A121C);                       // panel
        r->DrawRect(x, y, W, 26, 0xFF1C4A6E);                      // title bar
        r->DrawText(x + 12, y + 5, W - 24, 20,
                    ("Maps (" + std::to_string(maps.size()) + ")   Arrows / PgUp / PgDn - Enter loads - ESC back").c_str(),
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
