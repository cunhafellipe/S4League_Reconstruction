// Player movement/collision/jump/dodge physics, ported 1:1 from the moveXZ/
// hitWall/main-loop movement logic in C:\S4Web2\world.html.
//
// The actual raycast against map geometry is left to the caller (HitTestFn /
// FloorFn) so this doesn't reinvent projectoS10's own collision tree
// (CollTri/BgInfo in scenes.cpp) - it only ports the QUERY pattern (3 side
// offsets x 3 heights for walls, straight-down for floor) and what to do with
// the results.
#pragma once
#include <functional>

namespace world {

struct MoveTuning {
    // Straight off the live game_tempos.x7 (GAMETEMPO_FREE/GAMETEMPO_CLUB -
    // the only two blocks not commented out; GAMETEMPO_LT5's block above them
    // is dead/legacy data): actor_default_move_speed=1100 (WALK),
    // fastrun_speed_rate=2.0 -> RUN=2200, gravity=-7500, jump_force=2700.
    // Earlier guesses reverse-derived from a live capture were close on
    // gravity/dodge but off on WALK/JUMP/dodge_dodge_time - this replaces them
    // with the exact authored values instead.
    float WALK = 1100.f, RUN = 2200.f, JUMP = 2700.f, GRAV = 7500.f;
    float STEP_H = 110.f;    // step height that gets climbed automatically (wall rays start above it)
    float REFSPEED = 915.f;  // actor_default_animation_move_speed - speed a locomotion clip plays at 1x
    float COLL_R = 55.f;     // character collision radius
    // dodge_speed/dodge_decel/dodge_dodge_time straight off game_tempos.x7 too.
    float dodgeSpeed = 2250.f, dodgeDecel = 4500.f, dodgeTime = 0.4f;
    // GAMETEMPO_FREE common_action: walljump_*.
    float wallJumpForce = 3125.f, wallJumpDelay = 0.2f, wallJumpTime = 0.5f, wallJumpLandingTime = 0.2f;
    float crouchMul = 0.5f;   // sit_speed_rate
    // front_speed_rate/side_speed_rate/back_speed_rate.
    float dirFront = 1.0f, dirSide = 0.95f, dirBack = 0.9f;
    float charYawFollowRate = 14.f;   // body yaw exp-decay toward camera yaw
};

// True if a wall/obstacle blocks the straight-line move (dx,dz) from the
// character's current position (ox,oy,oz already offset by the caller if
// needed) within maxDist. dir is NOT normalized by the caller - implementer
// normalizes internally same as hitWall() in world.html.
using HitTestFn = std::function<bool(float ox, float oy, float oz, float dx, float dz, float maxDist)>;
// Floor height under (x,z), casting down from fromY. Returns false if no hit.
using FloorFn = std::function<bool(float x, float z, float fromY, float& outY)>;

struct MoveState {
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f;             // character body yaw (follows camera with decay)
    float velY = 0.f;
    bool  grounded = false;
    bool  didJump = false;
    bool  jumpMoving = false;
    float dodgeVX = 0.f, dodgeVZ = 0.f, dodgeTimeLeft = 0.f;
    int   dodgeDir = -1;         // 0 front / 1 back / 2 left / 3 right (matches anim::AnimCtx.dodgeDir)
    float wallJumpDelayLeft = 0.f, wallJumpTimeLeft = 0.f;
    float wallJumpVX = 0.f, wallJumpVZ = 0.f;
    // Eased copy of MoveInput.weaponWeightMul (exp decay toward it each tick) -
    // a swing ending was snapping straight back to full RUN speed the same
    // frame, which read as skating/sliding since the legs hadn't caught up to
    // the sudden velocity jump. This smooths both directions: easing INTO the
    // swing's own penalty and back OUT of it once the swing ends.
    float weightEase = 1.f;
    // Actual horizontal velocity, eased toward whatever WALK/RUN/stop target
    // the input wants each tick instead of snapping straight to it - without
    // this, pressing/releasing a move key changed speed instantly, which reads
    // as weightless (no ground friction, no body mass) no matter how good the
    // top-line WALK/RUN numbers are.
    float velX = 0.f, velZ = 0.f;
};

struct MoveInput {
    float mvX = 0.f, mvZ = 0.f;      // raw WASD, -1..1 each axis
    bool  running = false;
    bool  crouch = false;
    bool  jumpPressed = false;       // edge-triggered by caller (space && !spaceWasDown)
    bool  dodgePressed = false;      // A/D was pressed, then Space inside the input window
    bool  dodgeLeftHeld = false, dodgeRightHeld = false;   // remembered A/D direction
    float camYaw = 0.f;
    float weaponWeightMul = 1.f;     // e.g. 0.917 for plasma sword (captured live via x32dbg)
    float speedMul = 1.f;            // external gameplay multiplier; neutral for base locomotion
};

struct MoveResult {
    bool  moving = false;
    bool  fastrun = false;
    float speed = 0.f;               // curSpeed, feeds anim::AnimCtx.speed
    bool  startedDodge = false;      // this tick began a dodge (caller may play sound/seq)
    bool  startedJump = false;       // this tick began a normal jump
    bool  startedWallJump = false;   // this tick entered native STATE_BOUNDJUMP
};

class MovementController {
public:
    explicit MovementController(const MoveTuning& t = {}) : tuning_(t) {}
    const MoveTuning& tuning() const { return tuning_; }

    // Wall raycast pattern: 3 side offsets (center, +-COLL_R*0.85) x 3 heights
    // (STEP_H+5/80/170, always above the auto-climb step) against `hit`.
    bool HitWall(const HitTestFn& hit, const MoveState& s, float dx, float dz) const;
    // Collide-and-slide: full move -> slide X -> slide Z -> blocked (no move).
    void MoveXZ(const HitTestFn& hit, MoveState& s, float dx, float dz) const;

    // Advances one physics tick (dt seconds): body yaw, horizontal movement
    // (walk/run/fastrun), jump/dodge trigger + dodge impulse, gravity + floor
    // snap. Does NOT handle HP/SP, peer collision or void-respawn - those are
    // gameplay-specific and stay with the caller (same as world.html keeps them
    // in the main loop, calling MoveXZ directly for knockback/dodge impulses).
    MoveResult Tick(MoveState& s, const MoveInput& in, const HitTestFn& hitWall,
                    const FloorFn& floorAt, float dt) const;

private:
    MoveTuning tuning_;
};

}  // namespace world
