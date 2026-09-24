#include "seq_player.h"
#include <algorithm>
#include <cmath>

namespace seq {

Key SampleKeys(const std::vector<Key>& keys, float u) {
    if (keys.empty()) { Key k; k.sizeX=1; k.sizeY=1; k.r=1; k.g=1; k.b=1; k.a=1; k.rotate=0; return k; }
    if (keys.size() == 1) return keys[0];

    float first = keys.front().frame, last = keys.back().frame;
    float span = (last - first) != 0.f ? (last - first) : 1.f;
    float f = first + u * span;
    size_t i = 0;
    while (i < keys.size() - 2 && keys[i+1].frame < f) ++i;
    const Key& A = keys[i];
    const Key& B = (i + 1 < keys.size()) ? keys[i+1] : A;
    float d = (B.frame - A.frame) != 0.f ? (B.frame - A.frame) : 1.f;
    float t = (std::min)(1.f, (std::max)(0.f, (f - A.frame) / d));
    auto mix = [t](float x, float y) { return x + (y - x) * t; };

    Key out;
    out.frame = f;
    out.sizeX = mix(A.sizeX, B.sizeX); out.sizeY = mix(A.sizeY, B.sizeY);
    out.r = mix(A.r, B.r); out.g = mix(A.g, B.g); out.b = mix(A.b, B.b); out.a = mix(A.a, B.a);
    out.rotate = mix(A.rotate, B.rotate);
    return out;
}

// Small, fast, and good enough for scattering particles inside a cone - not
// meant to be cryptographically anything, just deterministic per seed.
namespace {
struct Xorshift32 {
    uint32_t s;
    explicit Xorshift32(uint32_t seed) : s(seed ? seed : 1) {}
    float Next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (float)s / 4294967296.f;
    }
};

constexpr float kPi = 3.14159265358979323846f;

// Picks a random direction inside a cone of half-angle thetaDeg around axis
// (ax,ay,az). The cone's own "roll" around the axis is irrelevant here since
// phi is drawn uniformly over the full circle, so any orthonormal basis around
// the axis works - it doesn't need to match a particular convention.
void ConeDirection(float ax, float ay, float az, float thetaDeg, Xorshift32& rnd, float out[3]) {
    float len = std::sqrt(ax*ax + ay*ay + az*az);
    float vx, vy, vz;
    if (len < 1e-9f) { vx = 0.f; vy = 1.f; vz = 0.f; }
    else { vx = ax/len; vy = ay/len; vz = az/len; }

    float theta = (std::min)(180.f, std::fabs(thetaDeg)) * kPi / 180.f;
    if (theta <= 0.f) { out[0]=vx; out[1]=vy; out[2]=vz; return; }

    float cosT = std::cos(theta);
    float z = cosT + rnd.Next() * (1.f - cosT);
    float phi = rnd.Next() * 2.f * kPi;
    float s = std::sqrt((std::max)(0.f, 1.f - z*z));

    float rx, ry, rz;
    if (std::fabs(vz) < 0.999f) { rx = -vy; ry = vx; rz = 0.f; }   // cross((0,0,1), v)
    else { rx = 0.f; ry = -vz; rz = vy; }                          // cross((1,0,0), v)
    float rl = std::sqrt(rx*rx + ry*ry + rz*rz); rx/=rl; ry/=rl; rz/=rl;
    float ux = vy*rz - vz*ry, uy = vz*rx - vx*rz, uz = vx*ry - vy*rx;   // up = v x right

    float cx = s*std::cos(phi), cy = s*std::sin(phi);
    out[0] = rx*cx + ux*cy + vx*z;
    out[1] = ry*cx + uy*cy + vy*z;
    out[2] = rz*cx + uz*cy + vz*z;
}
}  // namespace

ParticlePlayer::ParticlePlayer(const Node& node, float scale, uint32_t seed) : scale_(scale) {
    emitter_ = node.emitters.empty() ? Emitter{} : node.emitters[0];
    keys_ = node.keys;
    beginTime_ = node.beginTime;
    endTime_ = node.endTime;
    additive_ = node.blendType == 1;

    float lifetimeMs = emitter_.lifetime ? (float)emitter_.lifetime : (node.endTime ? (float)node.endTime : 1000.f);
    lifetime_ = (std::max)(1.f, lifetimeMs);
    float perSec = (std::max)(1.f, emitter_.perSecond ? (float)emitter_.perSecond : 30.f);
    interval_ = 1000.f / perSec;
    count_ = (std::min)(600, (std::max)(8, (int)std::ceil((lifetime_ / 1000.f) * perSec)));
    cycle_ = count_ * interval_;

    Xorshift32 rnd(seed);
    float w0 = (std::min)(emitter_.minWidth, emitter_.maxWidth), w1 = (std::max)(emitter_.minWidth, emitter_.maxWidth);
    float h0 = (std::min)(emitter_.minHeight, emitter_.maxHeight), h1 = (std::max)(emitter_.minHeight, emitter_.maxHeight);

    dirX_.resize(count_); dirY_.resize(count_); dirZ_.resize(count_);
    spawnX_.resize(count_); spawnY_.resize(count_); spawnZ_.resize(count_);
    for (int i = 0; i < count_; ++i) {
        float d[3];
        ConeDirection(emitter_.coneDir.x, emitter_.coneDir.y, emitter_.coneDir.z, emitter_.coneTheta, rnd, d);
        dirX_[i] = d[0]; dirY_[i] = d[1]; dirZ_[i] = d[2];

        float rad = w0 + (w1 - w0) * std::sqrt(rnd.Next());
        float ang = rnd.Next() * 2.f * kPi;
        spawnX_[i] = std::cos(ang) * rad;
        spawnY_[i] = h0 + (h1 - h0) * rnd.Next();
        spawnZ_[i] = std::sin(ang) * rad;
    }
}

void ParticlePlayer::Update(float tMs, std::vector<Particle>& out) const {
    out.clear();
    bool active = tMs >= (float)beginTime_ && (endTime_ == 0 || tMs <= (float)endTime_);
    if (!active) return;

    float local = tMs - (float)beginTime_;
    float ox = emitter_.origin.x, oy = emitter_.origin.y, oz = emitter_.origin.z;
    float gx = emitter_.gravity.x, gy = emitter_.gravity.y, gz = emitter_.gravity.z;
    float speed = emitter_.speed;

    out.reserve(count_);
    for (int i = 0; i < count_; ++i) {
        float age = std::fmod(local - i * interval_, cycle_);
        if (age < 0.f) age += cycle_;
        if (age > lifetime_) continue;

        float u = age / lifetime_;
        float t = age / 1000.f;
        float px = (ox + spawnX_[i] + dirX_[i]*speed*t + 0.5f*gx*t*t) * scale_;
        float py = (oy + spawnY_[i] + dirY_[i]*speed*t + 0.5f*gy*t*t) * scale_;
        float pz = (oz + spawnZ_[i] + dirZ_[i]*speed*t + 0.5f*gz*t*t) * scale_;
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) continue;

        Key k = SampleKeys(keys_, u);
        float a = std::isfinite(k.a) ? k.a : 1.f;
        if (a <= 0.01f) continue;

        Particle p;
        p.x = px; p.y = py; p.z = pz;
        p.r = k.r; p.g = k.g; p.b = k.b; p.a = a;
        p.size = (std::max)(0.01f, (std::fabs(k.sizeX) + std::fabs(k.sizeY)) * 0.5f) * scale_;
        out.push_back(p);
    }
}

}  // namespace seq
