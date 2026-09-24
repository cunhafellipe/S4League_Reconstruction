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
};

// Linear interpolation between the two keyframes bracketing u (0..1 over the
// particle's own lifetime). u is remapped into the keys' own frame range
// first, since keyframe "frame" values aren't guaranteed to span 0..1.
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
    bool additive_ = false;
    std::vector<float> dirX_, dirY_, dirZ_;
    std::vector<float> spawnX_, spawnY_, spawnZ_;
};

}  // namespace seq
