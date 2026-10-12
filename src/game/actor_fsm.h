// Actor locomotion state machine, reconstructed from the 2008 beta client
// (IDA port 5076, full C++ symbols). Replaces the hand-written
// anim::ActorStateMachine, whose transitions were inferred from feel.
//
// Three pieces, each read out of the binary:
//
//  * Common::CStateMachine::ChangeState (sub_81DA20): the only gate is the
//    incoming state's CanEnter(); then OnLeave(old), previous=old,
//    current=new, OnEnter(new). No same-state guard, nothing deferred.
//
//  * Transitions are DATA, not code: every Game::GameSetting::Init*State
//    (called per state by the builder sub_8339E0) fills that state's
//    map<input, IInputAction> with SetStateAtInput (vtbl+48 -> plain
//    ChangeState) and SetInputAction (vtbl+56, usually a
//    CMainSubStateInputAction: "enter `main` if its CanEnter() passes, else
//    `sub`"). The base CActorState::Input (sub_8AC4C0) looks the input up and
//    runs the action; inputs a state has no entry for are simply ignored.
//
//  * A few states override Input / have their own exits (STANDBY, FALL,
//    JUMP landing, DODGE end) - see actor_fsm.cpp.
#pragma once
#include "anim/anim_states.h"
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace game {

using anim::ActorState;

// GetExecutingContext() on the actor, as CJumpState/CStandbyState read it.
enum AirState : int { AIR_GROUND = 0, AIR_RISING = 1, AIR_FALLING = 2 };

struct FsmCtx {
    uint32_t nowMs = 0;
    AirState air = AIR_GROUND;
    bool dodgeMoving = false;      // movement still running the dodge slide/decel
    bool wallJumpMoving = false;   // movement still running the wall-jump delay/flight
};

// The game_tempos.x7 values the state classes read (GAMETEMPO_FREE block).
struct FsmTempo {
    uint32_t jumpLandDelayMs     = 300;   // jump_landdelay      (idx 32) - JUMP and FALL landing
    uint32_t wallJumpLandingMs   = 200;   // walljump_landing_time (idx 14)
    uint32_t dodgeCooldownMs     = 400;   // CDodgeState+180, set by InitDodge*State (sub_4B5A90(400))
};

class ActorFsm {
public:
    using EnterFn = std::function<void(ActorState now, ActorState prev)>;

    ActorFsm();

    ActorState current() const  { return current_; }
    ActorState previous() const { return previous_; }
    void SetOnEnter(EnterFn fn) { onEnter_ = std::move(fn); }
    FsmTempo& tempo() { return tempo_; }

    // CStateMachine::Input -> current state's Input (overrides, then table).
    void Input(int inputId, const FsmCtx& ctx);
    // CStateMachine::ChangeState. Returns false when CanEnter() refuses.
    bool ChangeState(ActorState next, const FsmCtx& ctx);
    // Per-frame state Tick: the exits that are timers / landings, not inputs.
    void Tick(const FsmCtx& ctx);

    // Attack states hand control back to their stored STANDBY when the swing
    // ends (Init*Attack*: vtbl+80(GetState(STANDBY), ...)). The swing timing
    // itself is still scenes.cpp's, so it reports the end here.
    void EndAttack(const FsmCtx& ctx);
    static bool IsAttack(ActorState s);

    // Where the weapon goes when a state is entered. The base OnEnter every
    // actor state runs (sub_7EB7D0) asks the state (vtbl+148) and then calls
    // CWeapon::Draw (sub_71E0B0: link=0 pieces onto their attackAttach node,
    // e.g. R_Hand_Dummy) or CWeapon::Holster (sub_71DFB0: onto idleAttach,
    // e.g. Spine_Dummy / Gun_Dummy). Values read per class from the beta.
    enum WeaponPose { WEAPON_KEEP = 0, WEAPON_DRAW = 1, WEAPON_HOLSTER = 2 };
    static WeaponPose WeaponPoseFor(ActorState s);

    // Rows the equipped weapon's own sub-state adds on top of a state's base
    // table (the client's per-weapon Init<Weapon>* settings, reached through
    // CSelectByWeaponState). Replaces whatever rows were set for `s` before.
    void SetWeaponRows(ActorState s, const std::vector<std::pair<int, ActorState>>& rows);

private:
    struct Action {
        ActorState main = anim::STATE_NORMAL;
        ActorState sub  = anim::STATE_NORMAL;
        bool hasSub = false;     // CMainSubStateInputAction
        bool mainSub = false;
    };
    void At(ActorState s, int input, ActorState target);                    // SetStateAtInput
    void MainSub(ActorState s, int input, ActorState main);                 // MainSub(main | none)
    void MainSub(ActorState s, int input, ActorState main, ActorState sub); // MainSub(main | sub)
    bool CanEnter(ActorState s, const FsmCtx& ctx) const;
    void BaseInput(int inputId, const FsmCtx& ctx);                         // CActorState::Input

    std::unordered_map<int, std::unordered_map<int, Action>> table_;
    std::unordered_map<int, std::unordered_map<int, Action>> weaponRows_;
    ActorState current_  = anim::STATE_NORMAL;
    ActorState previous_ = anim::STATE_NORMAL;
    EnterFn onEnter_;
    FsmTempo tempo_;

    // per-state runtime (fields of the C++ state objects)
    uint32_t lastDodgeStartMs_ = 0;   // dword_A53D70, written in CDodgeState::OnEnter
    bool     dodgeEverStarted_ = false;
    bool     seenAirborne_ = false;   // JUMP/FALL/BOUNDJUMP: left the ground since entering
    bool     landing_ = false;        // landing timer running
    uint32_t landEndMs_ = 0;
    bool     seenActionMove_ = false; // DODGE/BOUNDJUMP: movement started
};

} // namespace game
