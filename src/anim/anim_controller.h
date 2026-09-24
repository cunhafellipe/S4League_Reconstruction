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

// CStateMachine core recovered from the old client:
// CanEnter -> OnLeave(previous) -> save previous/current -> OnEnter(new).
// WAIT/STANDBY are empty Lua states and therefore deliberately keep the last
// pose for their transition frame; NORMAL/RUN then select the real clip.
class ActorStateMachine {
public:
    using CanEnter = std::function<bool(ActorState)>;
    using Callback = std::function<void(ActorState)>;

    ActorState current() const { return current_; }
    ActorState previous() const { return previous_; }

    bool SetCurrentState(ActorState next, const CanEnter& canEnter = {}) {
        if (next == current_ || (canEnter && !canEnter(next))) return false;
        if (onLeave_) onLeave_(current_);
        previous_ = current_;
        current_ = next;
        if (onEnter_) onEnter_(current_);
        return true;
    }

    void SetCallbacks(Callback onEnter, Callback onLeave) {
        onEnter_ = std::move(onEnter);
        onLeave_ = std::move(onLeave);
    }

    // Input-to-state path used by WorldState. Stop from locomotion passes
    // through STANDBY for exactly one update, matching the empty state in the
    // recovered actorstates script before NORMAL is entered.
    void Request(ActorState wanted) {
        if (transitionTarget_ != kNone && (current_ == STATE_WAIT || current_ == STATE_STANDBY)) return;
        const bool locomotion = wanted == STATE_NORMAL || wanted == STATE_RUN ||
                                wanted == STATE_FASTRUN || wanted == STATE_SIT;
        const bool action = current_ == STATE_JUMP || current_ == STATE_FALL ||
                            current_ == STATE_DODGE_LEFT || current_ == STATE_DODGE_RIGHT ||
                            current_ == STATE_BOUNDJUMP;
        if (locomotion && action) {
            SetCurrentState(STATE_WAIT);
            transitionTarget_ = wanted;
            return;
        }
        if (wanted == STATE_NORMAL && (current_ == STATE_RUN || current_ == STATE_FASTRUN)) {
            SetCurrentState(STATE_STANDBY);
            transitionTarget_ = STATE_NORMAL;
            return;
        }
        SetCurrentState(wanted);
        transitionTarget_ = kNone;
    }

    // Action states use the real WAIT -> STANDBY -> locomotion return path.
    void FinishAction(ActorState wanted) {
        if (current_ != STATE_WAIT && current_ != STATE_STANDBY)
            SetCurrentState(STATE_WAIT);
        transitionTarget_ = wanted;
    }

    void EndFrame() {
        if (transitionTarget_ == kNone) return;
        if (current_ == STATE_WAIT) SetCurrentState(STATE_STANDBY);
        else if (current_ == STATE_STANDBY) {
            const ActorState next = transitionTarget_;
            transitionTarget_ = kNone;
            SetCurrentState(next);
        }
    }

private:
    static constexpr ActorState kNone = static_cast<ActorState>(0xff);
    ActorState current_ = STATE_NORMAL;
    ActorState previous_ = STATE_NORMAL;
    ActorState transitionTarget_ = kNone;
    Callback onEnter_;
    Callback onLeave_;
};

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
    int   weakCombo = 0;               // which SwordAtkWeakState_<X> row to play (combo chain index)
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

    // Legacy entry point (pre-1:1-port scenes.cpp): loads BaseParams + WeaponDefs
    // (actorstates.h) from an already-parsed LuaStates, plus the weapon's
    // weak/strong attack rows into STATE_USE_WEAPON1_WEAK/STRONG (combat isn't
    // part of scn_anim.js's AnimController - see anim_states.h). Returns how many
    // states/attacks ended up covered.
    int LoadFromLua(const LuaStates& lua, const char* weapon);
    // Rows loaded for state `s` (0 = not covered) - scenes.cpp uses this to know
    // whether STATE_USE_WEAPON1_WEAK/STRONG have any attack data for this weapon.
    int LuaRowCount(ActorState s) const;

    // Cross-fade tuning read by Update() when the caller's AnimCtx doesn't set
    // blendMin/blendMax itself (kept for the debug menu in scenes.cpp; S4Web2's
    // own menu instead sets ctx.blendMin/blendMax directly each frame).
    float blendFloorMs = 100.f, blendCapMs = 250.f;
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

    ActorState seqState_ = STATE_NORMAL;
    int   phaseIdx_ = 0;
    float phaseClock_ = 0.f;
    bool  actionBusy_ = false;
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
