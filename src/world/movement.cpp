// Ported 1:1 from the moveXZ/hitWall/main-loop movement logic in
// C:\S4Web2\world.html.
#include "movement.h"
#include <algorithm>
#include <cmath>

namespace world {

bool MovementController::HitWall(const HitTestFn& hit, const MoveState& s, float dx, float dz) const {
    float dist = std::sqrt(dx*dx + dz*dz);
    if (dist < 1e-4f) return false;
    float nx = dx / dist, nz = dz / dist;
    float px = -nz, pz = nx;   // perpendicular (body sides)
    // Wider probe (COLL_R + 75) so lateral runs against a wall are detected
    // even when the body is already flush against it - but only for the two
    // upper rays. The lowest ray (just above step height) must only see what
    // is against the body: with the long probe it reached 3-4 stair steps
    // ahead, whose risers already stand above STEP_H, and stopped the player
    // at the foot of a staircase / stepped ramp. Close in, a step up to STEP_H
    // never reaches that ray and the floor snap climbs it; a real wall still
    // blocks through all three rays.
    const float probeFar  = tuning_.COLL_R + 75.f;
    const float probeNear = tuning_.COLL_R + 10.f;
    for (float side : { 0.f, tuning_.COLL_R * 0.85f, -tuning_.COLL_R * 0.85f }) {
        float ox = s.x + px * side, oz = s.z + pz * side;
        if (hit(ox, s.y + tuning_.STEP_H + 5.f, oz, nx, nz, probeNear)) return true;
        for (float oy : { tuning_.STEP_H + 80.f, tuning_.STEP_H + 170.f }) {
            if (hit(ox, s.y + oy, oz, nx, nz, probeFar)) return true;
        }
    }
    return false;
}

// Variant that also returns the XZ face normal of the first hit triangle.
// Used by wall-jump for specular reflection.
static bool HitWallWithNormal(const HitTestFn& hit, const HitNormalFn* hitNorm,
                               const MoveState& s,
                               float dx, float dz,
                               float COLL_R, float STEP_H,
                               float& outNx, float& outNz) {
    float dist = std::sqrt(dx*dx + dz*dz);
    if (dist < 1e-4f) return false;
    float nx = dx / dist, nz = dz / dist;
    float px = -nz, pz = nx;
    float probe = COLL_R + 75.f;
    for (float side : { 0.f, COLL_R * 0.85f, -COLL_R * 0.85f }) {
        float ox = s.x + px * side, oz = s.z + pz * side;
        for (float oy : { STEP_H + 5.f, STEP_H + 80.f, STEP_H + 170.f }) {
            if (hitNorm && (*hitNorm)(ox, s.y + oy, oz, nx, nz, probe, outNx, outNz)) return true;
            if (!hitNorm && hit(ox, s.y + oy, oz, nx, nz, probe)) {
                outNx = -nx; outNz = -nz; return true;
            }
        }
    }
    return false;
}

void MovementController::MoveXZ(const HitTestFn& hit, MoveState& s, float dx, float dz) const {
    if (!HitWall(hit, s, dx, dz)) { s.x += dx; s.z += dz; return; }
    if (dx != 0.f && !HitWall(hit, s, dx, 0.f)) { s.x += dx; return; }
    if (dz != 0.f && !HitWall(hit, s, 0.f, dz)) { s.z += dz; return; }
    // fully blocked -> no movement
}

MoveResult MovementController::Tick(MoveState& s, const MoveInput& in, const HitTestFn& hitWall,
                                     const HitNormalFn* hitNormal,
                                     const FloorFn& floorAt, float dt) const {
    MoveResult out;
    bool anyDir = (in.mvX != 0.f || in.mvZ != 0.f);
    const bool dodgePressed = in.dodgePressed &&
        (in.dodgeLeftHeld != in.dodgeRightHeld);
    const bool dodging    = s.dodgeTimeLeft > 0.f || dodgePressed;
    const bool wallJumping = s.wallJumpDelayLeft > 0.f || s.wallJumpTimeLeft > 0.f;

    // Body yaw follows camera with exponential decay.
    float d = in.camYaw - s.yaw;
    while (d >  3.14159265f) d -= 2.f * 3.14159265f;
    while (d < -3.14159265f) d += 2.f * 3.14159265f;
    s.yaw += d * (1.f - std::exp(-dt * tuning_.charYawFollowRate));

    // Fastrun engages the moment the player asks for it, airborne included.
    // Gating it on s.grounded meant a double-tap started during a jump did
    // nothing until touchdown, so the sprint always arrived a beat late - and
    // it also bled speed mid-air, since the target silently dropped back to
    // WALK while off the ground. The run ANIMATION still cannot play in the
    // air: scenes.cpp keeps the state latched to JUMP/FALL for as long as the
    // action lasts, so this only affects velocity.
    bool fastrun = in.running && in.mvZ > 0.f && !in.crouch;
    out.fastrun = fastrun;

    s.weightEase += (in.weaponWeightMul - s.weightEase) * (1.f - std::exp(-dt * 8.f));

    float moveDirX = 0.f, moveDirZ = 0.f;
    if (anyDir) {
        float mvX = in.mvX, mvZ = in.mvZ;
        float len = std::sqrt(mvX*mvX + mvZ*mvZ); mvX /= len; mvZ /= len;
        float fwdX = std::sin(in.camYaw), fwdZ = std::cos(in.camYaw);
        float rgtX = std::cos(in.camYaw), rgtZ = -std::sin(in.camYaw);
        moveDirX = fwdX*mvZ + rgtX*mvX;
        moveDirZ = fwdZ*mvZ + rgtZ*mvX;
    }

    // Target velocity.
    float targetVX = 0.f, targetVZ = 0.f;
    if (!dodging && !wallJumping && fastrun) {
        float fwdX = std::sin(in.camYaw), fwdZ = std::cos(in.camYaw);
        float speed = tuning_.RUN * s.weightEase * in.speedMul;
        targetVX = fwdX * speed; targetVZ = fwdZ * speed;
    } else if (!dodging && !wallJumping && anyDir) {
        float mvX = in.mvX, mvZ = in.mvZ;
        float len = std::sqrt(mvX*mvX + mvZ*mvZ); mvX /= len; mvZ /= len;
        float dirRate = mvZ > 0.f ? (mvX != 0.f ? (tuning_.dirFront + tuning_.dirSide) / 2.f : tuning_.dirFront)
                      : mvZ < 0.f ? (mvX != 0.f ? (tuning_.dirBack  + tuning_.dirSide) / 2.f : tuning_.dirBack)
                      : tuning_.dirSide;
        float speed = (in.crouch ? tuning_.WALK * tuning_.crouchMul
                                 : tuning_.WALK * dirRate * s.weightEase) * in.speedMul;
        targetVX = moveDirX * speed; targetVZ = moveDirZ * speed;
    }

    // NO acceleration ramp. Game::ActorState::CRunState::Tick (sub_8AA500 in
    // the beta client) recomputes the velocity from scratch every tick and
    // applies it straight away:
    //
    //   dir   = actor->GetMoveDir();            // vtbl[164]
    //   speed = GetBaseSpeed(actor) * dirRate;  // sub_812150 * sub_89EDD0(23|24|25)
    //   ScaleVec(v, speed, dir);
    //   actor->Move(v.x, v.y, v.z, dt_ms * 0.001f);   // vtbl[100]
    //
    // There is no lerp, no exponential decay and no reference to the previous
    // velocity anywhere in that tick - the target speed IS the speed. The
    // easeRate 16/9 ramp that used to be here took ~190 ms to reach the run
    // speed, which is exactly the "it waits instead of running" delay: the
    // double-tap was already detected on the second keypress, the velocity
    // just took another fifth of a second to get there.
    //
    // Applying the target directly is also strictly more framerate-stable
    // than the ease was: position advances as velocity * dt with no per-frame
    // state, so 30 fps and 240 fps integrate the same distance.
    s.velX = targetVX;
    s.velZ = targetVZ;

    if (!dodging && !wallJumping) MoveXZ(hitWall, s, s.velX * dt, s.velZ * dt);
    out.speed  = std::sqrt(s.velX*s.velX + s.velZ*s.velZ);
    out.moving = out.speed > 5.f;

    // --- Jump / Dodge / WallJump triggers ---

    if (dodgePressed) {
        bool dL = in.dodgeLeftHeld && !in.dodgeRightHeld;
        bool dR = in.dodgeRightHeld && !in.dodgeLeftHeld;
        s.dodgeDir = dL ? 2 : 3;
        float rgtX = std::cos(in.camYaw), rgtZ = -std::sin(in.camYaw), sgn = dR ? 1.f : -1.f;
        s.dodgeVX = sgn * rgtX; s.dodgeVZ = sgn * rgtZ;
        s.dodgeTimeLeft = tuning_.dodgeTime;
        s.velX = s.velZ = 0.f;
        // Leaving BOUNDJUMP for DODGE ends the bounce's flight: its
        // horizontal push is CBoundJumpState's own per-frame move, which stops
        // running once the state changes. The dodge sets the direction alone.
        s.wallJumpTimeLeft = 0.f;
        s.wallJumpDelayLeft = 0.f;
        out.startedDodge = true;
    } else if (in.jumpPressed && s.grounded) {
        s.velY = tuning_.JUMP; s.grounded = false; s.didJump = true;
        s.jumpMoving = anyDir && in.mvZ > 0.3f;
        s.jumpHadDir = anyDir;
        out.startedJump = true;
    } else if (in.jumpPressed && !s.grounded && anyDir && s.wallJumpDelayLeft <= 0.f) {
        // Allowed during a previous bounce's flight too (CBoundJumpState::
        // Input chains on Space in phases 2/3); scenes.cpp only lets Space
        // through here from a moving JUMP or a BOUNDJUMP in flight.
        // Wall jump: airborne + Space + moving toward a wall.
        float wnx = 0.f, wnz = 0.f;
        if (HitWallWithNormal(hitWall, hitNormal, s, moveDirX, moveDirZ,
                              tuning_.COLL_R, tuning_.STEP_H, wnx, wnz)) {
            // Specular reflection: reflect movedir off the wall normal.
            float dot = moveDirX * (-wnx) + moveDirZ * (-wnz);
            float refX = moveDirX - 2.f * dot * (-wnx);
            float refZ = moveDirZ - 2.f * dot * (-wnz);
            float rlen = std::sqrt(refX*refX + refZ*refZ);
            if (rlen > 1e-4f) { refX /= rlen; refZ /= rlen; }
            else              { refX = -moveDirX; refZ = -moveDirZ; }
            s.wallJumpVX = refX; s.wallJumpVZ = refZ;
            s.wallJumpDelayLeft = tuning_.wallJumpDelay;
            s.wallJumpTimeLeft  = 0.f;
            s.velX = s.velZ = s.velY = 0.f;
            s.didJump = true; s.jumpMoving = false; s.jumpHadDir = false;
            out.startedWallJump = true;
        }
    }

    // --- Dodge 2-phase movement (matches CDodgeState) ---
    // Phase 0 (elapsed < dodgeSlideTime): constant dodgeSpeed
    // Phase 1 (elapsed >= dodgeSlideTime): decelerate with dodgeDecel
    if (s.dodgeTimeLeft > 0.f) {
        float elapsed = tuning_.dodgeTime - s.dodgeTimeLeft;
        float ds;
        if (elapsed < tuning_.dodgeSlideTime)
            ds = tuning_.dodgeSpeed;
        else
            ds = (std::max)(0.f, tuning_.dodgeSpeed - tuning_.dodgeDecel * (elapsed - tuning_.dodgeSlideTime));
        MoveXZ(hitWall, s, s.dodgeVX * ds * dt, s.dodgeVZ * ds * dt);
        out.speed  = ds;
        out.moving = ds > 5.f;
        s.dodgeTimeLeft -= dt;
        if (s.dodgeTimeLeft <= 0.f) { s.dodgeTimeLeft = 0.f; s.dodgeDir = -1; }
    }

    // --- Wall-jump phases ---
    if (s.wallJumpDelayLeft > 0.f) {
        s.velY = 0.f;
        s.wallJumpDelayLeft -= dt;
        if (s.wallJumpDelayLeft <= 0.f) {
            s.wallJumpDelayLeft = 0.f;
            s.wallJumpTimeLeft  = tuning_.wallJumpTime;
            s.velY = tuning_.wallJumpForce;
            s.yaw  = std::atan2(s.wallJumpVX, s.wallJumpVZ);
        }
    } else if (s.wallJumpTimeLeft > 0.f) {
        // Launch phase (walljump_jump_time): the horizontal part of the
        // client's launch velocity, 0.75 * walljump_jump_force (~2344), not
        // WALK - that is what made the bounce feel short. Gravity handles the
        // vertical part (velY = walljump_jump_force at the end of the delay).
        const float hs = tuning_.wallJumpHorizRate * tuning_.wallJumpForce;
        MoveXZ(hitWall, s, s.wallJumpVX * hs * dt, s.wallJumpVZ * hs * dt);
        out.speed  = hs;
        out.moving = true;
        s.wallJumpTimeLeft = (std::max)(0.f, s.wallJumpTimeLeft - dt);
    }

    // --- Gravity / floor ---
    float fy; bool haveFloor = floorAt(s.x, s.z, s.y + 200.f, fy);
    float feetY = haveFloor ? fy : s.y;
    if (s.grounded) {
        if (!haveFloor || feetY < s.y - (tuning_.STEP_H + 60.f)) {
            s.grounded = false; s.velY = 0.f; s.didJump = false;
        } else {
            float rate = feetY > s.y ? 24.f : 18.f;
            s.y += (feetY - s.y) * (1.f - std::exp(-dt * rate));
            if (std::fabs(feetY - s.y) < 1.5f) s.y = feetY;
        }
    } else {
        const float term = in.maxFallVel < 0.f ? in.maxFallVel : tuning_.MAX_FALL;
        if (in.gravityOff) {
            // Actor physics off: no gravity and no vertical integration - the
            // actor hangs where it is (CS jump dash hangs for SetDrop's wait).
            // velY is left as is; the drop overwrites it when it fires.
        } else if (s.wallJumpDelayLeft <= 0.f) {
            // Exact constant-gravity step, clamped at the terminal speed
            // (split at the instant velY reaches it), so the parabola and
            // every timed air action are the same at any frame rate.
            const float v0 = s.velY, g = tuning_.GRAV;
            if (v0 <= term) {
                s.y += v0 * dt;
            } else {
                const float tHit = (v0 - term) / g;
                if (dt <= tHit) {
                    s.y    += v0 * dt - 0.5f * g * dt * dt;
                    s.velY  = v0 - g * dt;
                } else {
                    s.y    += v0 * tHit - 0.5f * g * tHit * tHit + term * (dt - tHit);
                    s.velY  = term;
                }
            }
        }
        if (haveFloor && s.velY <= 0.f && s.y <= feetY) {
            s.y = feetY; s.velY = 0.f; s.grounded = true; s.didJump = false;
            s.wallJumpDelayLeft = s.wallJumpTimeLeft = 0.f;
        }
    }
    return out;
}

}  // namespace world
