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
    for (float side : { 0.f, tuning_.COLL_R * 0.85f, -tuning_.COLL_R * 0.85f }) {
        float ox = s.x + px * side, oz = s.z + pz * side;
        for (float oy : { tuning_.STEP_H + 5.f, tuning_.STEP_H + 80.f, tuning_.STEP_H + 170.f }) {
            if (hit(ox, s.y + oy, oz, nx, nz, dist + tuning_.COLL_R)) return true;
        }
    }
    return false;
}

void MovementController::MoveXZ(const HitTestFn& hit, MoveState& s, float dx, float dz) const {
    if (!HitWall(hit, s, dx, dz)) { s.x += dx; s.z += dz; return; }
    if (dx != 0.f && !HitWall(hit, s, dx, 0.f)) { s.x += dx; return; }   // slide X
    if (dz != 0.f && !HitWall(hit, s, 0.f, dz)) { s.z += dz; return; }   // slide Z
    // fully blocked -> no movement
}

MoveResult MovementController::Tick(MoveState& s, const MoveInput& in, const HitTestFn& hitWall,
                                     const FloorFn& floorAt, float dt) const {
    MoveResult out;
    bool anyDir = (in.mvX != 0.f || in.mvZ != 0.f);
    const bool dodgePressed = in.dodgePressed &&
        (in.dodgeLeftHeld != in.dodgeRightHeld);
    const bool dodging = s.dodgeTimeLeft > 0.f || dodgePressed;
    const bool wallJumping = s.wallJumpDelayLeft > 0.f || s.wallJumpTimeLeft > 0.f;

    // Body yaw follows camera yaw with exponential decay (the body doesn't snap
    // to input direction, it lags the camera).
    float d = in.camYaw - s.yaw;
    while (d > 3.14159265f) d -= 2.f * 3.14159265f;
    while (d < -3.14159265f) d += 2.f * 3.14159265f;
    s.yaw += d * (1.f - std::exp(-dt * tuning_.charYawFollowRate));

    bool fastrun = in.running && in.mvZ > 0.f && !in.crouch && s.grounded;   // can't start/hold sprint airborne
    out.fastrun = fastrun;

    // Ease toward the swing's real penalty instead of snapping - a swing
    // ending used to hand back full RUN speed the same frame it finished,
    // which read as skating since the legs hadn't caught up to the jump yet.
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

    // Target velocity the input wants right now (zero when idle) - the ACTUAL
    // velocity (s.velX/velZ) eases toward this instead of snapping to it, same
    // exp-decay pattern as weightEase/charYawFollowRate above. Accelerating
    // and, especially, decelerating over a few frames instead of instantly is
    // what actually reads as ground friction/body mass; the top-line WALK/RUN
    // numbers alone don't do it.
    float targetVX = 0.f, targetVZ = 0.f;
    if (!dodging && !wallJumping && fastrun) {
        float fwdX = std::sin(in.camYaw), fwdZ = std::cos(in.camYaw);
        float speed = tuning_.RUN * s.weightEase * in.speedMul;
        targetVX = fwdX * speed; targetVZ = fwdZ * speed;
    } else if (!dodging && !wallJumping && anyDir) {
        float mvX = in.mvX, mvZ = in.mvZ;
        float len = std::sqrt(mvX*mvX + mvZ*mvZ); mvX /= len; mvZ /= len;
        float dirRate = mvZ > 0.f ? (mvX != 0.f ? (tuning_.dirFront + tuning_.dirSide) / 2.f : tuning_.dirFront)
                      : mvZ < 0.f ? (mvX != 0.f ? (tuning_.dirBack + tuning_.dirSide) / 2.f : tuning_.dirBack)
                      : tuning_.dirSide;
        float speed = (in.crouch ? tuning_.WALK * tuning_.crouchMul : tuning_.WALK * dirRate * s.weightEase) * in.speedMul;
        targetVX = moveDirX * speed; targetVZ = moveDirZ * speed;
    }
    // Snappier accelerating into a move than braking out of one - real bodies
    // take longer to shed momentum than to start building it.
    bool accelerating = (targetVX*targetVX + targetVZ*targetVZ) > (s.velX*s.velX + s.velZ*s.velZ);
    float easeRate = accelerating ? 16.f : 9.f;
    s.velX += (targetVX - s.velX) * (1.f - std::exp(-dt * easeRate));
    s.velZ += (targetVZ - s.velZ) * (1.f - std::exp(-dt * easeRate));
    if (s.velX*s.velX + s.velZ*s.velZ < 1.f) { s.velX = 0.f; s.velZ = 0.f; }   // kill infinite-decay crawl
    out.speed = std::sqrt(s.velX*s.velX + s.velZ*s.velZ);
    out.moving = out.speed > 5.f;
    if (!dodging && !wallJumping) MoveXZ(hitWall, s, s.velX * dt, s.velZ * dt);

    if (dodgePressed) {
        bool dL = in.dodgeLeftHeld && !in.dodgeRightHeld, dR = in.dodgeRightHeld && !in.dodgeLeftHeld;
        s.dodgeDir = dL ? 2 : 3;
        float rgtX = std::cos(in.camYaw), rgtZ = -std::sin(in.camYaw), sgn = dR ? 1.f : -1.f;
        s.dodgeVX = sgn * rgtX; s.dodgeVZ = sgn * rgtZ; s.dodgeTimeLeft = tuning_.dodgeTime;
        s.velX = s.velZ = 0.f;   // dodge owns horizontal displacement until its impulse ends
        out.startedDodge = true;
    } else if (in.jumpPressed && s.grounded) {
        s.velY = tuning_.JUMP; s.grounded = false; s.didJump = true;
        s.jumpMoving = anyDir && in.mvZ > 0.3f;
        out.startedJump = true;
    } else if (in.jumpPressed && !s.grounded && anyDir && !wallJumping &&
               HitWall(hitWall, s, moveDirX, moveDirZ)) {
        // Native CBoundJumpState first holds the wall-facing pose, then applies
        // the authored jump impulse and locks its directed movement window.
        s.wallJumpVX = -moveDirX; s.wallJumpVZ = -moveDirZ;
        s.wallJumpDelayLeft = tuning_.wallJumpDelay;
        s.wallJumpTimeLeft = 0.f;
        s.velX = s.velZ = s.velY = 0.f;
        s.didJump = true; s.jumpMoving = false;
        out.startedWallJump = true;
    }

    if (s.dodgeTimeLeft > 0.f) {
        float elapsed = tuning_.dodgeTime - s.dodgeTimeLeft;
        float ds = (std::max)(0.f, tuning_.dodgeSpeed - tuning_.dodgeDecel * elapsed);
        MoveXZ(hitWall, s, s.dodgeVX * ds * dt, s.dodgeVZ * ds * dt);
        out.speed = ds;
        out.moving = ds > 5.f;
        s.dodgeTimeLeft -= dt;
    }

    if (s.wallJumpDelayLeft > 0.f) {
        s.velY = 0.f;
        s.wallJumpDelayLeft -= dt;
        if (s.wallJumpDelayLeft <= 0.f) {
            s.wallJumpDelayLeft = 0.f;
            s.wallJumpTimeLeft = tuning_.wallJumpTime;
            s.velY = tuning_.wallJumpForce;
            s.yaw = std::atan2(s.wallJumpVX, s.wallJumpVZ);
        }
    } else if (s.wallJumpTimeLeft > 0.f) {
        MoveXZ(hitWall, s, s.wallJumpVX * tuning_.WALK * dt,
                           s.wallJumpVZ * tuning_.WALK * dt);
        out.speed = tuning_.WALK;
        out.moving = true;
        s.wallJumpTimeLeft = (std::max)(0.f, s.wallJumpTimeLeft - dt);
    }

    float fy; bool haveFloor = floorAt(s.x, s.z, s.y + 200.f, fy);
    float feetY = haveFloor ? fy : s.y;
    if (s.grounded) {
        if (!haveFloor || feetY < s.y - (tuning_.STEP_H + 60.f)) { s.grounded = false; s.velY = 0.f; s.didJump = false; }
        else {
            float rate = feetY > s.y ? 24.f : 18.f;
            s.y += (feetY - s.y) * (1.f - std::exp(-dt * rate));
            if (std::fabs(feetY - s.y) < 1.5f) s.y = feetY;
        }
    } else {
        if (s.wallJumpDelayLeft <= 0.f) {
            s.velY -= tuning_.GRAV * dt;
            s.y += s.velY * dt;
        }
        if (haveFloor && s.velY <= 0.f && s.y <= feetY) {
            s.y = feetY; s.velY = 0.f; s.grounded = true; s.didJump = false;
            s.wallJumpDelayLeft = s.wallJumpTimeLeft = 0.f;
        }
    }
    return out;
}

}  // namespace world
