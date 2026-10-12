// Data-driven actor animation engine, ported 1:1 from
// C:\S4Web2\engine\scn_anim.js (AnimController + Crossfader).
#pragma once
#include "anim_states.h"
#include "scn/skin.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace anim {

class LuaStates;   // actorstates.h

// The actor state machine itself lives in src/game/actor_fsm.h (rebuilt from
// the beta client's own tables); this file only plays the animation for
// whatever state it reports.

struct AnimCtx {
    float inX = 0.f, inZ = 0.f;        // move dir, camera-relative: A=-x D=+x, W=+z S=-z
    float motionX = 0.f, motionZ = 0.f; // world velocity used by the 2008 9-way selector
    float headingYaw = 0.f;              // actor heading, radians
    float velY = 0.f;
    bool  grounded = true;
    bool  moving = false;
    float speed = 0.f;
    float refSpeed = 300.f;
    bool  jumpMoving = false;
    int   dodgeDir = -1;               // 0 front / 1 back / 2 left / 3 right / -1 none
    // CDodgeState timers (game_tempos.x7 dodge_dodge_time / dodge_delay, ms).
    // The client stretches the dodge clips to exactly these - see kDodge*Ph.
    float dodgeSlideMs = 225.f, dodgeDelayMs = 375.f;
    // jump_time (game_tempos.x7 idx 30): CJumpState's rise timer, the
    // duration ApplyAnim fits the rise clips (00001 / 00004) to.
    float jumpTimeMs = 700.f;
    int   weakCombo = 0;               // which SwordAtkWeakState_<X> row to play (combo chain index)
    bool  ironBoots = false;           // Iron Boots worn: ActorState_FastRun's IRON_BOOTS row (IB068)
    float syncWindowMs = 0.f;
    float blendMin = 0.f, blendMax = 1e9f;
    std::function<float(const std::string&)> durOf;   // authored clip duration in native ticks, or 0
};

class AnimController {
public:
    // defs = {NORMAL,RUN,SIT,JUMP,BOUNDJUMP,FALL,DODGE,FASTRUN} parsed from the
    // real actorstates_*.lua (scn/actorstates.h). Additive: base table first,
    // then weapon overrides on top.
    void LoadDefs(const std::unordered_map<std::string, std::vector<SAnimParam>>& defs);

    scn::Pose Update(ActorState state, const AnimCtx& ctx, float dtMs, float clockScale = 1.f);

    ActorState state() const { return state_; }
    bool actionBusy() const { return actionBusy_; }
    // Invalidates the cached clip index so the next Update() re-picks the clip
    // for the current state even if state/gate didn't change - needed after
    // swapping weapons mid-run/walk, where the direction index stays the same
    // but now points at a different weapon's clip table.
    void ForceRefresh() { paramIdx_ = -1; }

    // Replay the current state from its first phase - called on every
    // game::ActorFsm state entry. In the client OnEnter calls
    // SetAnimation(..., RESET_TRUE), which rewinds the clip to tick 0 even
    // when the same state is entered again (ChangeState has no same-state
    // guard); seqState_/paramIdx_ alone would not notice that re-entry.
    void Restart() { restart_ = true; }

    // SetAnim blendIn / blendOut (native ticks) of the clip now playing - the
    // scene-level fader needs them when an attack takes over or hands back.
    int CurBlendInTicks() const  { return curBlendIn_; }
    int CurBlendOutTicks() const { return prevBlendOut_; }

    // Legacy entry point (pre-1:1-port scenes.cpp): loads BaseParams + WeaponDefs
    // (actorstates.h) from an already-parsed LuaStates, plus the weapon's
    // weak/strong attack rows into STATE_USE_WEAPON1_WEAK/STRONG (combat isn't
    // part of scn_anim.js's AnimController - see anim_states.h). Returns how many
    // states/attacks ended up covered.
    // normalAs / runAs: the item's own weapon_lua.x7 rows when they name another
    // weapon's NormalState_/RunState_ (Rapier: SpyDagger idle, Katana run,
    // PlasmaSword attacks) - those two state tables come from that suffix.
    int LoadFromLua(const LuaStates& lua, const char* weapon, const char* normalAs = nullptr, const char* runAs = nullptr);
    // Rows loaded for state `s` (0 = not covered) - scenes.cpp uses this to know
    // whether STATE_USE_WEAPON1_WEAK/STRONG have any attack data for this weapon.
    int LuaRowCount(ActorState s) const;
    // The SetAnim rows loaded for a state (nullptr if none) - scenes.cpp reads
    // RUN's UPPER (0..7) / LOWER (8..15) halves to drive torso and legs apart.
    const std::vector<SAnimParam>* StateRows(ActorState s) const { return StateDef(s); }

    // Cross-fade tuning read by Update() when the caller's AnimCtx doesn't set
    // blendMin/blendMax itself (kept for the debug menu in scenes.cpp; S4Web2's
    // own menu instead sets ctx.blendMin/blendMax directly each frame).
    //
    // Default is NO CLAMP on purpose: the blend times are authored per clip in
    // the real ActorStates_GamePlay.lua and the engine honours them as-is.
    // Verified against the beta client (full C++ symbols):
    // Game::Utility::CAnimParam::SetAnim stores the Lua args verbatim
    // (startMs->this+84, blendIn->this+64, blendOut->this+68, loop, rate,
    // reset) with no clamping anywhere, and Crossfader below - the part this
    // file itself marks "1:1 C++" - also runs unclamped.
    // The old 100/250 default silently crushed every authored blend:
    //   ActorState_BoundJump  RISE_B  blendOut=1000 -> 250  (wall jump)
    //   ActorState_BoundJump  FALL/LANDING blendIn=500 -> 250
    //   ActorState_Fall       FALLING blendIn=1000 -> 250
    //   ActorState_Dodge      LEFT    blendOut=700  -> 250
    // which is what read as "no weight, no fluid transition": the long blends
    // the animators authored were being played at a quarter of their length.
    // The F3 menu can still dial a clamp in for experiments.
    float blendFloorMs = 0.f, blendCapMs = 1e9f;
    // multiplies clockScale in Update(). .scn clip durations/ticks are in
    // native tick units (idle=8000, walk=3200, dodge=7200...), not real ms -
    // without this factor EVERY clip (not just locomotion, which also gets
    // speed-warped) plays several times slower than real.
    // Native CAnimParam duration conversion: clipTicks / 4.8 (sub_42AED0).
    float speedScale = 4.8f;

private:
    const std::vector<SAnimParam>* StateDef(ActorState s) const;

    std::unordered_map<int, const std::vector<SAnimParam>*> defs_ = MakeDefaultStateDefs();
    std::unordered_map<int, std::vector<SAnimParam>> ownedDefs_;   // storage for LoadDefs overrides

    ActorState  state_    = STATE_NORMAL;
    int         paramIdx_ = -1;
    std::string clipA_, clipB_;
    float clockA_ = 0.f, clockB_ = 0.f;
    float blendT_ = 1.f, blendDur_ = 0.f;
    float rateA_ = 1.f, rateB_ = 1.f;
    bool  locoA_ = false, locoB_ = false;
    bool  loopA_ = true, loopB_ = true;
    int   prevBlendOut_ = 0;
    int   curBlendIn_ = 0;

    ActorState seqState_ = STATE_NORMAL;
    int   phaseIdx_ = 0;
    float phaseClock_ = 0.f;
    bool  actionBusy_ = false;
    bool  restart_ = false;
};

// Generic 2-clip cross-fader for a stream of SAnimParam (per-weapon attack
// clips from actorstates_*.lua). Independent of AnimController (doesn't touch
// the unarmed path).
class Crossfader {
public:
    void Set(const SAnimParam* param, const std::function<float(const std::string&)>& durOf);
    scn::Pose Update(float dtMs, float clockScale = 1.f);
    bool Done() const { return !loopB_ && durB_ > 0.f && clockB_ >= durB_; }

private:
    std::string clipA_, clipB_;
    float clockA_ = 0.f, clockB_ = 0.f;
    float rateA_ = 1.f, rateB_ = 1.f;
    bool  loopA_ = true, loopB_ = true;
    float durA_ = 0.f, durB_ = 0.f;
    float blendT_ = 1.f, blendDur_ = 0.f;
    int   prevBlendOut_ = 0;
};

// Per-clip playback overrides, editable from a debug menu (parity with
// S4Web2's editor). clipRate[clip] = speed multiplier; clipTrim[clip] = play
// only [start,end) of the clip, looping within that range.
struct ClipTrim { float start, end; };
extern std::unordered_map<std::string, float> g_clipRate;
extern std::unordered_map<std::string, ClipTrim> g_clipTrim;
uint32_t ApplyTrim(const std::string& clip, float tick, bool loop = true);

} // namespace anim
