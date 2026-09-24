// Bone/skinning engine, ported 1:1 from C:\S4Web2\engine\scn_skin.js.
#include "scn/skin.h"
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>

namespace scn {

static const Mat4 kIdentity = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

Mat4 Mat4Mul(const Mat4& A, const Mat4& B) {
    Mat4 C{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            C[r*4+c] = A[r*4+0]*B[0*4+c] + A[r*4+1]*B[1*4+c] + A[r*4+2]*B[2*4+c] + A[r*4+3]*B[3*4+c];
    return C;
}

Mat4 Mat4InvAffine(const Mat4& m) {
    float a=m[0],b=m[1],c=m[2], d=m[4],e=m[5],f=m[6], g=m[8],h=m[9],i=m[10];
    float C00=e*i-f*h, C01=-(d*i-f*g), C02=d*h-e*g;
    float det = a*C00 + b*C01 + c*C02;
    if (std::fabs(det) < 1e-12f) return m;
    float id = 1.0f/det;
    float n0=C00*id, n1=-(b*i-c*h)*id, n2=(b*f-c*e)*id;
    float n3=C01*id, n4=(a*i-c*g)*id,  n5=-(a*f-c*d)*id;
    float n6=C02*id, n7=-(a*h-b*g)*id, n8=(a*e-b*d)*id;
    float tx=m[12], ty=m[13], tz=m[14];
    Mat4 o{};
    o[0]=n0; o[1]=n1; o[2]=n2; o[3]=0;
    o[4]=n3; o[5]=n4; o[6]=n5; o[7]=0;
    o[8]=n6; o[9]=n7; o[10]=n8; o[11]=0;
    o[12]=-(tx*n0 + ty*n3 + tz*n6);
    o[13]=-(tx*n1 + ty*n4 + tz*n7);
    o[14]=-(tx*n2 + ty*n5 + tz*n8);
    o[15]=1;
    return o;
}

Mat4 BuildTRS(const float T[3], const float q[4], const float S[3]) {
    float x=q[0], y=q[1], z=q[2], w=q[3];
    Mat4 o{};
    o[0]=S[0]*(1-2*(y*y+z*z)); o[1]=S[0]*(2*(x*y+z*w));   o[2]=S[0]*(2*(x*z-y*w));    o[3]=0;
    o[4]=S[1]*(2*(x*y-z*w));   o[5]=S[1]*(1-2*(x*x+z*z)); o[6]=S[1]*(2*(y*z+x*w));    o[7]=0;
    o[8]=S[2]*(2*(x*z+y*w));   o[9]=S[2]*(2*(y*z-x*w));   o[10]=S[2]*(1-2*(x*x+y*y)); o[11]=0;
    o[12]=T[0]; o[13]=T[1]; o[14]=T[2]; o[15]=1;
    return o;
}

static void SampleVec(const std::vector<VecKey>& keys, const float def[3], uint32_t t, float out[3]) {
    if (keys.empty()) { out[0]=def[0]; out[1]=def[1]; out[2]=def[2]; return; }
    if (t <= keys.front().t) { std::memcpy(out, keys.front().v, sizeof(float)*3); return; }
    for (size_t i = 1; i < keys.size(); ++i)
        if (t <= keys[i].t) {
            uint32_t t0 = keys[i-1].t, t1 = keys[i].t;
            float a = t1 > t0 ? float(t - t0) / float(t1 - t0) : 0.f;
            const float* A = keys[i-1].v; const float* B = keys[i].v;
            out[0]=A[0]+(B[0]-A[0])*a; out[1]=A[1]+(B[1]-A[1])*a; out[2]=A[2]+(B[2]-A[2])*a;
            return;
        }
    std::memcpy(out, keys.back().v, sizeof(float)*3);
}

static void SampleQuat(const std::vector<QuatKey>& keys, const float def[4], uint32_t t, float out[4]) {
    if (keys.empty()) { std::memcpy(out, def, sizeof(float)*4); return; }
    if (t <= keys.front().t) { std::memcpy(out, keys.front().q, sizeof(float)*4); return; }
    for (size_t i = 1; i < keys.size(); ++i)
        if (t <= keys[i].t) {
            const QuatKey& A = keys[i-1]; const QuatKey& B = keys[i];
            float a = B.t > A.t ? float(t - A.t) / float(B.t - A.t) : 0.f;
            float d = A.q[0]*B.q[0]+A.q[1]*B.q[1]+A.q[2]*B.q[2]+A.q[3]*B.q[3];
            float s = d < 0 ? -1.f : 1.f;   // shortest arc
            for (int k = 0; k < 4; ++k) out[k] = A.q[k] + (B.q[k]*s - A.q[k])*a;
            float l = std::sqrt(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]+out[3]*out[3]);
            if (l > 1e-6f) { out[0]/=l; out[1]/=l; out[2]/=l; out[3]/=l; }
            return;
        }
    std::memcpy(out, keys.back().q, sizeof(float)*4);
}

static Mat4 ComposeTRS(const Anim& a, uint32_t tick, bool loop = true) {
    uint32_t t = loop && a.duration ? (tick % a.duration) : tick;
    float T[3], S[3], q[4];
    SampleVec(a.trans, a.initT, t, T);
    SampleVec(a.scale, a.initS, t, S);
    SampleQuat(a.rot, a.initR, t, q);
    return BuildTRS(T, q, S);
}

Mat4 SampleNode(const Model& m, uint32_t tick) {
    if (m.anims.empty() || !m.anims[0].hasTransform) return kIdentity;
    const Anim& a = m.anims[0];
    Mat4 bind = BuildTRS(a.initT, a.initR, a.initS);
    Mat4 cur = ComposeTRS(a, tick);
    return Mat4Mul(Mat4InvAffine(bind), cur);
}

static const Anim* FindAnim(const Bone& bone, const std::string& name) {
    for (const Anim& a : bone.anims) if (a.name == name) return &a;
    return nullptr;
}

struct BoneTRS { float T[3], q[4], S[3]; };

static bool SampleBoneTRS(const Bone& bone, const std::string& animName, uint32_t tick, bool loop, BoneTRS& out) {
    const Anim* a = FindAnim(bone, animName);
    if (!a || !a->hasTransform) return false;
    uint32_t t = loop && a->duration ? (tick % a->duration) : tick;
    SampleVec(a->trans, a->initT, t, out.T);
    SampleQuat(a->rot, a->initR, t, out.q);
    SampleVec(a->scale, a->initS, t, out.S);
    return true;
}

// Local transform of the bone, cross-faded at the TRS level (lerp T/S, NLERP
// rot shortest-arc). std::nullopt = no clip/BASE data on this bone at all -
// caller (ResolveBoneWorlds) reconstructs the local from the bone's own
// absolute matrix.
static std::optional<Mat4> SampleBonePose(const Bone& bone, const Pose& p) {
    BoneTRS A{};
    bool okA = SampleBoneTRS(bone, p.clipA, p.tickA, p.loopA, A);
    if (p.blend > 0.001f && !p.clipB.empty() && p.clipB != p.clipA && okA) {
        BoneTRS B{};
        if (SampleBoneTRS(bone, p.clipB, p.tickB, p.loopB, B)) {
            float w = p.blend > 1.f ? 1.f : p.blend;
            float T[3] = { A.T[0]+(B.T[0]-A.T[0])*w, A.T[1]+(B.T[1]-A.T[1])*w, A.T[2]+(B.T[2]-A.T[2])*w };
            float S[3] = { A.S[0]+(B.S[0]-A.S[0])*w, A.S[1]+(B.S[1]-A.S[1])*w, A.S[2]+(B.S[2]-A.S[2])*w };
            float s = (A.q[0]*B.q[0]+A.q[1]*B.q[1]+A.q[2]*B.q[2]+A.q[3]*B.q[3]) < 0 ? -1.f : 1.f;
            float q[4];
            for (int i = 0; i < 4; ++i) q[i] = A.q[i] + (B.q[i]*s - A.q[i])*w;
            float l = std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
            if (l > 1e-6f) { q[0]/=l; q[1]/=l; q[2]/=l; q[3]/=l; }
            return BuildTRS(T, q, S);
        }
    }
    if (okA) return BuildTRS(A.T, A.q, A.S);
    // Clip missing on this bone: fall back to bind local (or the clip as a
    // matrix, if the clip name matches something with a transform track).
    const Anim* a = FindAnim(bone, p.clipA);
    if (a && a->hasTransform) return ComposeTRS(*a, p.tickA, p.loopA);
    // Anchor bones (Sword_Wea_Dummy, R_Hand_Dummy...) usually carry their own
    // "BASE" clip with the correct offset already decomposed - prefer it over
    // reconstructing from the raw matrix (confirmed: skipping this breaks the
    // plasma sword / assault rifle root bone, which depend on it).
    const Anim* base = FindAnim(bone, "BASE");
    if (base && base->hasTransform) return BuildTRS(base->initT, base->initR, base->initS);
    return std::nullopt;   // resolveBoneWorlds reconstructs the real local from bone.matrix (absolute)
}

// Costume bones like the Frill skirt chains ship with zero animation keys
// (frozen bind pose) so without this they never move. Cheap per-chain sine
// sway instead of real cloth physics; swayTime is null for the main skeleton,
// so this never touches normal character animation.
static float BoneSwayAngle(const Bone& bone, float swayTime) {
    size_t pos = bone.name.rfind("_Frill_Bone_");
    if (pos == std::string::npos) return 0.f;
    size_t digitsStart = pos + 12;
    if (digitsStart >= bone.name.size()) return 0.f;
    for (size_t i = digitsStart; i < bone.name.size(); ++i)
        if (!isdigit((unsigned char)bone.name[i])) return 0.f;
    int idx = std::atoi(bone.name.c_str() + digitsStart);
    bool depth = bone.parent.find("_Frill_Bone_") != std::string::npos &&
                 bone.parent.size() >= 12;   // tip link swings more than root link
    float amp = 0.08f + (depth ? 0.10f : 0.f);
    return std::sin(swayTime * 2.2f + idx * 0.9f) * amp;
}
static Mat4 RotZ(float a) {
    float c = std::cos(a), s = std::sin(a);
    return { c,s,0,0, -s,c,0,0, 0,0,1,0, 0,0,0,1 };
}

bool IsLowerBodyBone(const std::string& boneName) {
    return boneName.find(" L Thigh") != std::string::npos || boneName.find(" R Thigh") != std::string::npos ||
           boneName.find(" L Calf") != std::string::npos  || boneName.find(" R Calf") != std::string::npos ||
           boneName.find(" L Foot") != std::string::npos  || boneName.find(" R Foot") != std::string::npos ||
           boneName.find(" L Toe0") != std::string::npos  || boneName.find(" R Toe0") != std::string::npos;
}

std::vector<Mat4> ResolveBoneWorlds(const Scene& sc, const Pose& pose, const float* swayTime,
                                     const std::unordered_map<std::string, Mat4>* externalParents,
                                     const Pose* lowerPose) {
    const size_t nb = sc.bones.size();
    std::vector<Mat4> local(nb), world(nb);
    std::vector<int> parent(nb, -1);
    std::vector<bool> done(nb, false);
    std::vector<const Mat4*> extWorld(nb, nullptr);

    std::unordered_map<std::string, size_t> byName;
    for (size_t i = 0; i < nb; ++i) byName[sc.bones[i].name] = i;
    for (size_t i = 0; i < nb; ++i) {
        const std::string& p = sc.bones[i].parent;
        if (p.empty()) continue;
        auto it = byName.find(p);
        if (it != byName.end()) parent[i] = (int)it->second;
        else if (externalParents) {
            auto eit = externalParents->find(p);
            if (eit != externalParents->end()) extWorld[i] = &eit->second;
        }
    }
    for (size_t i = 0; i < nb; ++i) {
        const Pose& bonePose = (lowerPose && IsLowerBodyBone(sc.bones[i].name)) ? *lowerPose : pose;
        std::optional<Mat4> l = SampleBonePose(sc.bones[i], bonePose);
        if (!l) {
            // No clip for this bone: bone.matrix is ABSOLUTE (not parent-relative).
            // Using it as-is applies the parent's transform a SECOND time downstream
            // and sends the bone flying off - reconstruct the real local as
            // boneMatrix * inverse(parentMatrix).
            if (parent[i] < 0) {
                Mat4 m; std::memcpy(m.data(), sc.bones[i].matrix, sizeof(m));
                local[i] = m;
            } else {
                Mat4 bm; std::memcpy(bm.data(), sc.bones[i].matrix, sizeof(bm));
                Mat4 pm; std::memcpy(pm.data(), sc.bones[parent[i]].matrix, sizeof(pm));
                local[i] = Mat4Mul(bm, Mat4InvAffine(pm));
            }
        } else {
            local[i] = *l;
        }
        if (swayTime) {
            float a = BoneSwayAngle(sc.bones[i], *swayTime);
            if (a != 0.f) local[i] = Mat4Mul(RotZ(a), local[i]);
        }
    }
    for (size_t i = 0; i < nb; ++i)
        if (extWorld[i]) { world[i] = Mat4Mul(local[i], *extWorld[i]); done[i] = true; }

    for (bool progress = true; progress; ) {
        progress = false;
        for (size_t i = 0; i < nb; ++i) {
            if (done[i]) continue;
            if (parent[i] < 0) { world[i] = local[i]; done[i] = true; progress = true; }
            else if (done[parent[i]]) { world[i] = Mat4Mul(local[i], world[parent[i]]); done[i] = true; progress = true; }
        }
    }
    for (size_t i = 0; i < nb; ++i) if (!done[i]) world[i] = local[i];   // cycle guard
    return world;
}

bool BoneWorldByName(const Scene& sc, const std::string& boneName, const Pose& pose, Mat4& out) {
    for (size_t i = 0; i < sc.bones.size(); ++i)
        if (sc.bones[i].name == boneName) {
            std::vector<Mat4> world = ResolveBoneWorlds(sc, pose);
            out = world[i];
            return true;
        }
    return false;
}

const Mat4* BoneWorldByNameW(const Scene& sc, const std::string& boneName, const std::vector<Mat4>& world) {
    for (size_t i = 0; i < sc.bones.size(); ++i)
        if (sc.bones[i].name == boneName) return &world[i];
    return nullptr;
}

std::vector<float> SkinModelW(const Model& m, const Scene& sc, const std::vector<Mat4>& world) {
    std::vector<float> out = m.positions;
    if (m.skin.empty() || sc.bones.empty() || world.empty()) return out;

    std::unordered_map<std::string, size_t> boneIdx;
    for (size_t j = 0; j < sc.bones.size(); ++j) boneIdx[sc.bones[j].name] = j;

    size_t nv = out.size() / 3;
    std::vector<float> acc(out.size(), 0.f), wsum(nv, 0.f);

    for (const SkinBone& sb : m.skin) {
        auto it = boneIdx.find(sb.name);
        Mat4 bind; std::memcpy(bind.data(), sb.bind, sizeof(bind));
        // sb.bind IS ALREADY the inverse-bind (model->bone): skin = invBind * boneWorld. Do NOT invert it.
        Mat4 skin = (it != boneIdx.end()) ? Mat4Mul(bind, world[it->second]) : kIdentity;
        for (const BoneWeight& bw : sb.weights) {
            if (bw.vertex >= nv) continue;
            float vx = m.positions[bw.vertex*3], vy = m.positions[bw.vertex*3+1], vz = m.positions[bw.vertex*3+2];
            float px = vx*skin[0]+vy*skin[4]+vz*skin[8]+skin[12];
            float py = vx*skin[1]+vy*skin[5]+vz*skin[9]+skin[13];
            float pz = vx*skin[2]+vy*skin[6]+vz*skin[10]+skin[14];
            acc[bw.vertex*3] += px*bw.weight; acc[bw.vertex*3+1] += py*bw.weight; acc[bw.vertex*3+2] += pz*bw.weight;
            wsum[bw.vertex] += bw.weight;
        }
    }
    for (size_t i = 0; i < nv; ++i)
        if (wsum[i] > 1e-4f) { out[i*3]=acc[i*3]/wsum[i]; out[i*3+1]=acc[i*3+1]/wsum[i]; out[i*3+2]=acc[i*3+2]/wsum[i]; }
    return out;
}

std::vector<float> SkinModel(const Model& m, const Scene& sc, const Pose& pose) {
    return SkinModelW(m, sc, ResolveBoneWorlds(sc, pose));
}

// Local animated matrix of a MODEL. Animation goes by clip NAME in model.anims
// (F0001 = heavy gun barrels spinning, etc.); anims[0] is usually BASE. Do NOT
// fall back to anims[0]/"BASE" when the requested clip doesn't exist - tried,
// broke the plasma sword (which relied on staying at bind/m.matrix).
Mat4 SampleModelLocal(const Model& m, const std::string& clipName, uint32_t tick, bool loop) {
    if (!m.anims.empty()) {
        const Anim* a = nullptr;
        if (!clipName.empty()) for (const Anim& x : m.anims) if (x.name == clipName) { a = &x; break; }
        if (a && a->hasTransform) return ComposeTRS(*a, tick, loop);
    }
    Mat4 m16; std::memcpy(m16.data(), m.matrix, sizeof(m16));
    return m16;
}

NodeResolver MakeNodeResolver(const Scene& scene, const Pose& pose, bool animateModels) {
    auto boneWorlds = std::make_shared<std::vector<Mat4>>(
        scene.bones.empty() ? std::vector<Mat4>{} : ResolveBoneWorlds(scene, pose));
    auto boneIdx = std::make_shared<std::unordered_map<std::string, size_t>>();
    for (size_t i = 0; i < scene.bones.size(); ++i) (*boneIdx)[scene.bones[i].name] = i;
    auto modelByName = std::make_shared<std::unordered_map<std::string, const Model*>>();
    for (const Model& m : scene.models) (*modelByName)[m.name] = &m;
    uint32_t tick = pose.tickA;
    bool loop = pose.loopA;
    std::string clipName = pose.clipA;
    auto cache = std::make_shared<std::unordered_map<std::string, std::optional<Mat4>>>();

    auto self = std::make_shared<std::function<std::optional<Mat4>(const std::string&, int)>>();
    *self = [=](const std::string& name, int depth) -> std::optional<Mat4> {
        if (name.empty() || depth > 16) return std::nullopt;   // cut cycles
        auto cit = cache->find(name);
        if (cit != cache->end()) return cit->second;
        std::optional<Mat4> w;
        auto bit = boneIdx->find(name);
        if (bit != boneIdx->end() && !boneWorlds->empty()) {
            w = (*boneWorlds)[bit->second];
        } else {
            auto mit = modelByName->find(name);
            if (mit != modelByName->end()) {
                const Model& m = *mit->second;
                Mat4 local = animateModels ? SampleModelLocal(m, clipName, tick, loop) : [&]{ Mat4 m16; std::memcpy(m16.data(), m.matrix, sizeof(m16)); return m16; }();
                std::optional<Mat4> p = m.parent.empty() ? std::nullopt : (*self)(m.parent, depth + 1);
                w = p ? Mat4Mul(local, *p) : local;
            }
        }
        (*cache)[name] = w;
        return w;
    };

    return [self](const std::string& name) -> const Mat4* {
        std::optional<Mat4> r = (*self)(name, 0);
        if (!r) return nullptr;
        // stash to keep the pointer alive past this call - callers use it immediately.
        static thread_local Mat4 slot;
        slot = *r;
        return &slot;
    };
}

// --- legacy call-shape overloads (pre-1:1-port callers) --------------------
void SampleNode(const Model& m, uint32_t tick, float out16[16]) {
    Mat4 r = SampleNode(m, tick);
    std::memcpy(out16, r.data(), sizeof(float) * 16);
}
bool BoneWorldByName(const Scene& sc, const std::string& boneName, const Pose& pose, float out16[16]) {
    Mat4 m;
    if (!BoneWorldByName(sc, boneName, pose, m)) return false;
    std::memcpy(out16, m.data(), sizeof(float) * 16);
    return true;
}
void SkinModel(const Model& m, const Scene& sc, const Pose& pose, std::vector<Vec3>& out) {
    std::vector<float> flat = SkinModel(m, sc, pose);
    out.resize(flat.size() / 3);
    for (size_t i = 0; i < out.size(); ++i) out[i] = { flat[i*3], flat[i*3+1], flat[i*3+2] };
}
void SkinModel(const Model& m, const Scene& sc, const Pose& pose, const Pose* lowerPose, std::vector<Vec3>& out) {
    if (!lowerPose) { SkinModel(m, sc, pose, out); return; }
    std::vector<float> flat = SkinModelW(m, sc, ResolveBoneWorlds(sc, pose, nullptr, nullptr, lowerPose));
    out.resize(flat.size() / 3);
    for (size_t i = 0; i < out.size(); ++i) out[i] = { flat[i*3], flat[i*3+1], flat[i*3+2] };
}

// Weapon exports contain both forms: most child matrices are already absolute,
// while some legacy weapons (notably dragonmachinegun) store a child relative
// to its model parent. Pick the interpretation whose piece remains near the
// resolved parent; this preserves both layouts.
void WeaponMountInverse(const Scene& sc, const Bone& mount, float invOut[16]) {
    float lo[3]={1e9f,1e9f,1e9f}, hi[3]={-1e9f,-1e9f,-1e9f};
    for (const Model& m : sc.models)
        for (const Vec3& v : m.vertices) {
            float w[3] = { v.x*m.matrix[0]+v.y*m.matrix[4]+v.z*m.matrix[8]+m.matrix[12],
                           v.x*m.matrix[1]+v.y*m.matrix[5]+v.z*m.matrix[9]+m.matrix[13],
                           v.x*m.matrix[2]+v.y*m.matrix[6]+v.z*m.matrix[10]+m.matrix[14] };
            for (int k=0;k<3;++k) { lo[k]=(std::min)(lo[k],w[k]); hi[k]=(std::max)(hi[k],w[k]); }
        }
    const float* t = mount.matrix + 12;
    bool inBounds = !sc.models.empty() &&
                    t[0] >= lo[0]-1.f && t[0] <= hi[0]+1.f &&
                    t[1] >= lo[1]-1.f && t[1] <= hi[1]+1.f &&
                    t[2] >= lo[2]-1.f && t[2] <= hi[2]+1.f;
    Mat4 mountM; std::memcpy(mountM.data(), mount.matrix, sizeof(mountM));
    if (inBounds) { Mat4 inv = Mat4InvAffine(mountM); std::memcpy(invOut, inv.data(), sizeof(float)*16); return; }
    Mat4 rot = mountM; rot[12]=rot[13]=rot[14]=0.f;
    Mat4 inv = Mat4InvAffine(rot);
    std::memcpy(invOut, inv.data(), sizeof(float)*16);
}

void ResolveWeaponPieceWorlds(const Scene& sc, std::vector<std::array<float,16>>& out) {
    out.resize(sc.models.size());
    std::unordered_map<std::string, size_t> byName;
    for (size_t i = 0; i < sc.models.size(); ++i) byName[sc.models[i].name] = i;

    auto centroid = [](const Model& m, const Mat4& x) {
        float p[3] = {};
        if (m.vertices.empty()) return std::array<float,3>{x[12], x[13], x[14]};
        for (const Vec3& v : m.vertices) {
            p[0] += v.x*x[0] + v.y*x[4] + v.z*x[8]  + x[12];
            p[1] += v.x*x[1] + v.y*x[5] + v.z*x[9]  + x[13];
            p[2] += v.x*x[2] + v.y*x[6] + v.z*x[10] + x[14];
        }
        const float n = (float)m.vertices.size();
        return std::array<float,3>{p[0]/n, p[1]/n, p[2]/n};
    };
    auto dist2 = [](const std::array<float,3>& a, const std::array<float,3>& b) {
        const float x=a[0]-b[0], y=a[1]-b[1], z=a[2]-b[2];
        return x*x + y*y + z*z;
    };

    for (size_t i = 0; i < sc.models.size(); ++i) {
        const Model& m = sc.models[i];
        Mat4 flat; std::memcpy(flat.data(), m.matrix, sizeof(flat));
        Mat4 chosen = flat;
        auto parent = byName.find(m.parent);
        if (parent != byName.end() && parent->second < i) {
            const Mat4 parentWorld = out[parent->second];
            const Mat4 chained = Mat4Mul(flat, parentWorld);
            const auto parentCenter = centroid(sc.models[parent->second], parentWorld);
            // A relative child should sit near its model parent; an absolute
            // child becomes visibly far away when chained a second time.
            if (dist2(centroid(m, chained), parentCenter) <
                dist2(centroid(m, flat), parentCenter)) chosen = chained;
        }
        out[i] = chosen;
    }
}

}  // namespace scn
