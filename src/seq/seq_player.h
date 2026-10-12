// Runtime playback for a parsed .seq particle node (see seq.h for the file
// format itself). Turns one CActParticle node's emitter + keyframes into a
// stream of live particles at a given point in time: this is pure simulation
// math, no graphics API involved - the caller decides how to actually draw
// the returned points (billboards, point sprites, whatever the renderer
// supports).
#pragma once
#include "seq.h"
#include <cstdint>
#include <vector>

namespace seq {

// One particle at a moment in time, ready to hand to a renderer.
struct Particle {
    float x = 0, y = 0, z = 0;
    float r = 1, g = 1, b = 1, a = 1;
    float size = 1;
    float sizeX = 1, sizeY = 1;   // keyframe width / height (streaks: 5 x 300)
    float vx = 0, vy = 0, vz = 0; // velocity, emitter space (renderType 1 stretches along it)
    float rotate = 0;             // keyframe rotation, degrees (in the quad's own plane)
    float age = 0;                // ms since this particle was born
    int   frame = 0;              // index into the node's texture list (texture animation)
};

// Linear interpolation between the two keyframes bracketing u (0..1 over the
// particle's own lifetime). S10 compares age/lifetime directly to key.frame.
Key SampleKeys(const std::vector<Key>& keys, float u);

class ParticlePlayer {
public:
    // scale: uniform world-space scale for a system authored at a different
    // unit size than the scene it's placed in. seed: picks which spawn points/
    // directions this instance uses - keep it fixed per-effect-slot so a
    // recurring effect (e.g. every sword swing) doesn't visibly "reroll" its
    // particle placement each time.
    explicit ParticlePlayer(const Node& node, float scale = 1.f, uint32_t seed = 1);

    // Additive blending (fire/glow/energy effects) vs normal alpha (smoke,
    // dust) - comes straight from the node's own blendType field.
    bool additive() const { return additive_; }
    uint32_t renderType() const { return renderType_; }
    bool attachTarget() const { return attachTarget_; }

    // Total span this system stays active for, in ms (0 = runs forever /
    // caller decides when to stop it).
    uint32_t endTimeMs() const { return endTime_; }

    // Fills `out` with every particle alive at `tMs` (ms since this effect was
    // triggered). Particles fade in/out via their own keyframes; a fixed-size
    // pool of spawn slots is reused on a loop so continuous emitters (rain,
    // muzzle smoke) don't need to allocate over time.
    void Update(float tMs, std::vector<Particle>& out) const;

private:
    Emitter emitter_;
    std::vector<Key> keys_;
    uint32_t beginTime_ = 0, endTime_ = 0;
    float lifetime_ = 1.f, interval_ = 1.f, cycle_ = 1.f, scale_ = 1.f;
    int count_ = 0;
    // Emitter key Continuance = 0 (S10 CParticleSystem FUN_01c8adb0, key
    // +0x10): EmitPerSec particles come out ONCE, all together, then the
    // emitter stops (+0x38). Only Continuance = 1 emits per second.
    bool burst_ = false;
    uint32_t renderType_ = 0;
    uint32_t texAnimType_ = 0, texAnimDelay_ = 0;
    int texCount_ = 1;
    bool attachTarget_ = false;
    bool additive_ = false;
    std::vector<float> lifetimes_, speeds_;
    std::vector<int> textureStarts_;
    std::vector<std::vector<Key>> particleKeys_;
    std::vector<float> dirX_, dirY_, dirZ_;
    std::vector<float> spawnX_, spawnY_, spawnZ_;
};

}  // namespace seq
