#include "seq_player.h"
#include <algorithm>
#include <cmath>

namespace seq {

Key SampleKeys(const std::vector<Key>& keys, float u) {
    if (keys.empty()) { Key k; k.sizeX=1; k.sizeY=1; k.r=1; k.g=1; k.b=1; k.a=1; k.rotate=0; return k; }
    if (keys.size() == 1) return keys[0];

    float f = u; // S10 sub_1C8C850: key times are fractions of lifetime.
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
    float Signed() { const float a = Next(); return a - Next(); }
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
    renderType_ = node.renderType;
    attachTarget_ = node.attachTarget;
    texAnimType_ = node.textureAnimType;
    texAnimDelay_ = node.textureAnimDelay;
    texCount_ = (std::max)(1, (int)node.textures.size());

    if (node.emitters.empty() || node.keys.empty() || !emitter_.lifetime ||
        !emitter_.perSecond || !node.maxParticles) return;
    lifetime_ = (std::max)(1.f, (float)emitter_.lifetime + (float)node.lifetimeVar);
    float perSec = (float)emitter_.perSecond;
    interval_ = 1000.f / perSec;
    // ponytail: bounded simulation pool; lift 600 only with a measured particle budget.
    const int cap = (int)(std::min)(600u, node.maxParticles);
    count_ = (int)(std::min)((float)cap, std::ceil((lifetime_ / 1000.f) * perSec));
    burst_ = !node.emitters.empty() && !emitter_.continuance;
    if (burst_) count_ = (int)(std::min)((uint32_t)cap, emitter_.perSecond);
    cycle_ = (std::max)(lifetime_, count_ * interval_);

    Xorshift32 rnd(seed);
    float w0 = (std::min)(emitter_.minWidth, emitter_.maxWidth), w1 = (std::max)(emitter_.minWidth, emitter_.maxWidth);
    float h0 = (std::min)(emitter_.minHeight, emitter_.maxHeight), h1 = (std::max)(emitter_.minHeight, emitter_.maxHeight);

    dirX_.resize(count_); dirY_.resize(count_); dirZ_.resize(count_);
    spawnX_.resize(count_); spawnY_.resize(count_); spawnZ_.resize(count_);
    lifetimes_.resize(count_); speeds_.resize(count_); textureStarts_.resize(count_);
    particleKeys_.resize(count_);
    for (int i = 0; i < count_; ++i) {
        float d[3];
        ConeDirection(emitter_.coneDir.x, emitter_.coneDir.y, emitter_.coneDir.z, emitter_.coneTheta, rnd, d);
        dirX_[i] = d[0]; dirY_[i] = d[1]; dirZ_[i] = d[2];

        // S10 particle spawn (CParticleSystem sub_1C8B850): r uniform in
        // [minWidth, maxWidth], h uniform in [minHeight, maxHeight], angles
        // uniform in [0, 6.28). emitterType 0 = ellipsoid shell
        // (r sin(phi) cos(theta), h cos(phi), r sin(phi) sin(theta)) - height
        // spans -h..+h around the origin; emitterType 1 = ring at height h.
        float rad = w0 + (w1 - w0) * rnd.Next();
        float hgt = h0 + (h1 - h0) * rnd.Next();
        if (emitter_.emitterType == 1) {
            float th = rnd.Next() * 6.28f;
            spawnX_[i] = std::cos(th) * rad;
            spawnY_[i] = hgt;
            spawnZ_[i] = std::sin(th) * rad;
        } else {
            float th = rnd.Next() * 6.28f, ph = rnd.Next() * 6.28f;
            spawnX_[i] = std::cos(th) * std::sin(ph) * rad;
            spawnY_[i] = std::cos(ph) * hgt;
            spawnZ_[i] = std::sin(th) * std::sin(ph) * rad;
        }
        // S10 sub_1C8B850/sub_1C8C850: triangular (rand-rand) variation,
        // applied to the endpoints, then interpolated; locks retain spawn values.
        lifetimes_[i] = (std::max)(1.f, (float)emitter_.lifetime + rnd.Signed() * node.lifetimeVar);
        speeds_[i] = emitter_.speed + rnd.Signed() * node.emitterSpeedVar;
        textureStarts_[i] = (int32_t)emitter_.textureKey < 0
            ? (std::min)(texCount_ - 1, (int)(rnd.Next() * texCount_))
            : (int)(emitter_.textureKey % (uint32_t)texCount_);
        for (const Key& source : keys_) {
            Key k = source;
            const float size = rnd.Signed() * node.sizeVar;
            k.sizeX = (std::max)(0.f, k.sizeX + size + rnd.Signed() * node.sizeXVar);
            k.sizeY = (std::max)(0.f, k.sizeY + size + rnd.Signed() * node.sizeYVar);
            auto color = [](float x) { return (std::max)(0.f, (std::min)(1.f, x)); };
            k.r = color(k.r + rnd.Signed() * node.colorVar[0]);
            k.g = color(k.g + rnd.Signed() * node.colorVar[1]);
            k.b = color(k.b + rnd.Signed() * node.colorVar[2]);
            k.a = color(k.a + rnd.Signed() * node.alphaVar);
            k.rotate += rnd.Signed() * node.rotateVar;
            if (!particleKeys_[i].empty()) {
                const Key& first = particleKeys_[i].front();
                if (node.sizeLock) { k.sizeX = first.sizeX; k.sizeY = first.sizeY; }
                if (node.colorLock) { k.r = first.r; k.g = first.g; k.b = first.b; }
                if (node.alphaLock) k.a = first.a;
                if (node.rotateLock) k.rotate = first.rotate;
            }
            particleKeys_[i].push_back(k);
        }
    }
}

void ParticlePlayer::Update(float tMs, std::vector<Particle>& out) const {
    out.clear();
    bool active = tMs >= (float)beginTime_ && (endTime_ == 0 || tMs <= (float)endTime_);
    if (!active || count_ == 0 || !std::isfinite(tMs)) return;

    float local = tMs - (float)beginTime_;
    float ox = emitter_.origin.x, oy = emitter_.origin.y, oz = emitter_.origin.z;
    float gx = emitter_.gravity.x, gy = emitter_.gravity.y, gz = emitter_.gravity.z;

    out.reserve(count_);
    for (int i = 0; i < count_; ++i) {
        float age;
        if (burst_) {
            age = local - (float)emitter_.frame;   // one burst at the emitter key's time
            if (age < 0.f) continue;
        } else {
            // Never wrap a negative age: those particles have not been born.
            const float sinceBirth = local - (float)emitter_.frame - (i + 1) * interval_;
            if (sinceBirth < 0.f) continue;
            age = std::fmod(sinceBirth, cycle_);
        }
        if (age >= lifetimes_[i]) continue;

        const float speed = speeds_[i];
        float u = age / lifetimes_[i];
        float t = age / 1000.f;
        float px = (ox + spawnX_[i] + dirX_[i]*speed*t + 0.5f*gx*t*t) * scale_;
        float py = (oy + spawnY_[i] + dirY_[i]*speed*t + 0.5f*gy*t*t) * scale_;
        float pz = (oz + spawnZ_[i] + dirZ_[i]*speed*t + 0.5f*gz*t*t) * scale_;
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) continue;

        Key k = SampleKeys(particleKeys_[i], u);
        float a = std::isfinite(k.a) ? k.a : 1.f;
        if (a <= 0.01f) continue;

        Particle p;
        p.x = px; p.y = py; p.z = pz;
        p.r = k.r; p.g = k.g; p.b = k.b; p.a = a;
        p.size = (std::max)(0.01f, (std::fabs(k.sizeX) + std::fabs(k.sizeY)) * 0.5f) * scale_;
        p.sizeX = std::fabs(k.sizeX) * scale_;
        p.sizeY = std::fabs(k.sizeY) * scale_;
        p.rotate = std::isfinite(k.rotate) ? k.rotate : 0.f;
        p.vx = dirX_[i]*speed + gx*t; p.vy = dirY_[i]*speed + gy*t; p.vz = dirZ_[i]*speed + gz*t;
        p.age = age;
        p.frame = textureStarts_[i];
        // Texture animation (S10 CParticleSystem sub_1C8D1E0): the node's
        // texture list is a frame strip. 1 = frames spread over the
        // particle's life, 2 = next frame every textureAnimDelay ms, 3 = a
        // random frame every textureAnimDelay ms. Each particle starts on its
        // own frame (the client keeps a per-particle offset).
        if (texCount_ > 1) {
            const uint32_t delay = texAnimDelay_ ? texAnimDelay_ : 100;
            const uint32_t step = (uint32_t)(age / (float)delay);
            switch (texAnimType_) {
            case 1: p.frame = (p.frame + (int)((float)texCount_ * u)) % texCount_; break;
            case 2: p.frame = (int)((step + (uint32_t)p.frame) % (uint32_t)texCount_); break;
            case 3: {
                if (step == 0) break; // keep the authored spawn frame until the first delay
                uint32_t h = (uint32_t)i * 2654435761u ^ step * 40503u;
                h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
                p.frame = (int)(h % (uint32_t)texCount_);
                break;
            }
            default: break;
            }
        }
        out.push_back(p);
    }
}

}  // namespace seq
