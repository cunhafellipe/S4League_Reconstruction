// See actor_fsm.h. Every table row below is one SetStateAtInput (At) or
// SetInputAction (MainSub) call from the named Game::GameSetting::Init*State
// in the beta client; state and input numbers are the binary's own.
#include "actor_fsm.h"
#include "actor_input.h"

namespace game {

using namespace anim;

ActorFsm::ActorFsm() {
    // InitNormalState (sub_8A3060)
    At     (STATE_NORMAL, IN_MOVE,        STATE_RUN);
    At     (STATE_NORMAL, IN_SIT,         STATE_SIT);
    At     (STATE_NORMAL, IN_LEVITATE,    STATE_FALL);
    At     (STATE_NORMAL, IN_JUMP,        STATE_JUMP);
    MainSub(STATE_NORMAL, IN_DODGE_LEFT,  STATE_DODGE_LEFT,  STATE_JUMP);
    MainSub(STATE_NORMAL, IN_DODGE_RIGHT, STATE_DODGE_RIGHT, STATE_JUMP);
    MainSub(STATE_NORMAL, IN_FASTRUN,     STATE_FASTRUN,     STATE_RUN);

    // InitRunState (sub_8A2CE0). No MOVE row: RUN stays RUN while a direction
    // is held, and STOP (CInputAlways) takes it back to NORMAL.
    At     (STATE_RUN, IN_STOP,        STATE_NORMAL);
    At     (STATE_RUN, IN_SIT,         STATE_SIT);
    At     (STATE_RUN, IN_LEVITATE,    STATE_FALL);
    At     (STATE_RUN, IN_JUMP,        STATE_JUMP);
    MainSub(STATE_RUN, IN_DODGE_LEFT,  STATE_DODGE_LEFT,  STATE_JUMP);
    MainSub(STATE_RUN, IN_DODGE_RIGHT, STATE_DODGE_RIGHT, STATE_JUMP);
    MainSub(STATE_RUN, IN_FASTRUN,     STATE_FASTRUN);           // MainSub(FASTRUN | none)

    // InitSitState (sub_8A29C0)
    At     (STATE_SIT, IN_SIT_CANCEL,  STATE_NORMAL);
    At     (STATE_SIT, IN_LEVITATE,    STATE_FALL);
    At     (STATE_SIT, IN_JUMP,        STATE_JUMP);
    MainSub(STATE_SIT, IN_DODGE_LEFT,  STATE_DODGE_LEFT,  STATE_JUMP);
    MainSub(STATE_SIT, IN_DODGE_RIGHT, STATE_DODGE_RIGHT, STATE_JUMP);
    MainSub(STATE_SIT, IN_FASTRUN,     STATE_FASTRUN,     STATE_RUN);

    // InitJumpState (sub_8A2750). Dodge goes straight in (no MainSub), and the
    // FASTRUN double tap is honoured mid-air: the sprint starts in the air,
    // it does not wait for the landing.
    At     (STATE_JUMP, IN_DODGE_LEFT,  STATE_DODGE_LEFT);
    At     (STATE_JUMP, IN_DODGE_RIGHT, STATE_DODGE_RIGHT);
    MainSub(STATE_JUMP, IN_FASTRUN,     STATE_FASTRUN, STATE_RUN);

    // InitBoundJumpState (sub_8A5B80)
    MainSub(STATE_BOUNDJUMP, IN_DODGE_LEFT,  STATE_DODGE_LEFT,  STATE_JUMP);
    MainSub(STATE_BOUNDJUMP, IN_DODGE_RIGHT, STATE_DODGE_RIGHT, STATE_JUMP);

    // InitFallState (sub_8A2450)
    At     (STATE_FALL, IN_LEVITATE_END, STATE_STANDBY);
    At     (STATE_FALL, IN_SIT_CANCEL,   STATE_STANDBY);
    MainSub(STATE_FALL, IN_DODGE_LEFT,  STATE_DODGE_LEFT,  STATE_JUMP);
    MainSub(STATE_FALL, IN_DODGE_RIGHT, STATE_DODGE_RIGHT, STATE_JUMP);
    MainSub(STATE_FALL, IN_FASTRUN,     STATE_FASTRUN,     STATE_RUN);

    // InitDodgeLeftState / InitDodgeRightState (sub_8A59A0 / sub_8A57C0).
    // No dodge row: pressing dodge again mid-dodge does nothing.
    At(STATE_DODGE_LEFT,  IN_JUMP, STATE_JUMP);
    At(STATE_DODGE_RIGHT, IN_JUMP, STATE_JUMP);
    // NOT in the 2008 beta table: a W double tap during the dodge sprints
    // straight out of it (project decision, matching the live game the
    // client is rebuilt against). Same MainSub(FASTRUN | RUN) as NORMAL/SIT.
    MainSub(STATE_DODGE_LEFT,  IN_FASTRUN, STATE_FASTRUN, STATE_RUN);
    MainSub(STATE_DODGE_RIGHT, IN_FASTRUN, STATE_FASTRUN, STATE_RUN);

    // InitFastRunState (sub_8A1990). No STOP/MOVE rows: only releasing W
    // (FASTRUN_END, a CKeyUpInput on the same key) ends the sprint.
    At     (STATE_FASTRUN, IN_JUMP,        STATE_JUMP);
    MainSub(STATE_FASTRUN, IN_DODGE_LEFT,  STATE_DODGE_LEFT,  STATE_JUMP);
    MainSub(STATE_FASTRUN, IN_DODGE_RIGHT, STATE_DODGE_RIGHT, STATE_JUMP);
    At     (STATE_FASTRUN, IN_FASTRUN_END, STATE_NORMAL);

    // Attack rows. Every state the player acts from carries the same block
    // (Init{Normal,Run,Sit,FastRun,DodgeLeft,DodgeRight}State): a short click
    // (USE_WEAPON1_END_SHORT, 7) -> WEAK, a held click (_END_LONG, 8) ->
    // STRONG, right click (USE_WEAPON2, 9) -> USE_WEAPON2. Airborne states
    // (Init{Jump,Fall,BoundJump}State) send both clicks to USE_WEAPON1_JUMP.
    for (ActorState s : { STATE_NORMAL, STATE_RUN, STATE_SIT, STATE_FASTRUN,
                          STATE_DODGE_LEFT, STATE_DODGE_RIGHT }) {
        At(s, IN_USE_WEAPON1,        STATE_USE_WEAPON1);
        At(s, IN_USE_WEAPON1_WEAK,   STATE_USE_WEAPON1_WEAK);
        At(s, IN_USE_WEAPON1_STRONG, STATE_USE_WEAPON1_STRONG);
        At(s, IN_USE_WEAPON2,        STATE_USE_WEAPON2);
    }
    for (ActorState s : { STATE_JUMP, STATE_FALL, STATE_BOUNDJUMP }) {
        At(s, IN_USE_WEAPON1,        STATE_USE_WEAPON1);
        At(s, IN_USE_WEAPON1_WEAK,   STATE_USE_WEAPON1_JUMP);
        At(s, IN_USE_WEAPON1_STRONG, STATE_USE_WEAPON1_JUMP);
        At(s, IN_USE_WEAPON2,        STATE_USE_WEAPON2);
    }
    // The attack states themselves (InitUseWeapon1*State / InitUseWeapon2State)
    // have no locomotion rows at all - only DAMAGE/COUNTERATTACK_DAMAGE - so
    // moving, jumping and dodging are ignored for the whole swing unless the
    // weapon's own sub-state adds a row (SetWeaponRows).

    // InitStandbyState (sub_8A0FA0) has no locomotion rows; its Input
    // override (CStandbyState, sub_7CB010) does the routing - see Input().
}

ActorFsm::WeaponPose ActorFsm::WeaponPoseFor(ActorState s) {
    switch (s) {
    // 0 (unknown_libname_70 / 0x49F350): CStandbyState, CSkillStealthState,
    // CSkillSensorState - leave the weapon wherever it is.
    case STATE_STANDBY:
        return WEAPON_KEEP;
    // 1 (0x438280): CNormalState, CRunState (and the CSelectByWeaponState
    // wrapping it, 0x7D3580), every attack / shot / guard / dash state.
    case STATE_NORMAL:
    case STATE_RUN:
        return WEAPON_DRAW;
    default:
        if (IsAttack(s)) return WEAPON_DRAW;
        // 2 (0x48B8B0): CJumpState, CFallState, CBoundJumpState, CDodgeState,
        // CFastRunState, CSitState, CWaitState, CIdleState and the
        // damage/down/skill/social states - the weapon goes on the back /
        // thigh. FASTRUN's 00068 clip is the same unarmed sprint for every
        // weapon, which is why the client holsters it there.
        return WEAPON_HOLSTER;
    }
}

bool ActorFsm::IsAttack(ActorState s) {
    return s == STATE_USE_WEAPON1 || s == STATE_USE_WEAPON2 || s == STATE_USE_WEAPON1_WEAK ||
           s == STATE_USE_WEAPON1_STRONG || s == STATE_USE_WEAPON1_JUMP || s == STATE_USE_WEAPON1_STRONG1;
}

void ActorFsm::EndAttack(const FsmCtx& ctx) {
    if (IsAttack(current_)) ChangeState(STATE_STANDBY, ctx);
}

void ActorFsm::SetWeaponRows(ActorState s, const std::vector<std::pair<int, ActorState>>& rows) {
    auto& m = weaponRows_[s];
    m.clear();
    for (const auto& r : rows) { Action a; a.main = r.second; m[r.first] = a; }
}

void ActorFsm::At(ActorState s, int input, ActorState target) {
    Action a; a.main = target;
    table_[s][input] = a;
}
void ActorFsm::MainSub(ActorState s, int input, ActorState main) {
    Action a; a.main = main; a.mainSub = true;
    table_[s][input] = a;
}
void ActorFsm::MainSub(ActorState s, int input, ActorState main, ActorState sub) {
    Action a; a.main = main; a.sub = sub; a.hasSub = true; a.mainSub = true;
    table_[s][input] = a;
}

bool ActorFsm::CanEnter(ActorState s, const FsmCtx& ctx) const {
    switch (s) {
    case STATE_DODGE_LEFT:
    case STATE_DODGE_RIGHT:
        // CDodgeState::CanEnter (sub_7E0920):
        //   actor->CanAct() && actor->MP() >= dodge_required_mp
        //   && this[45] (=400) + lastDodgeStart <= now
        // MP is not modelled yet, so only the cooldown applies.
        return !dodgeEverStarted_ || lastDodgeStartMs_ + tempo_.dodgeCooldownMs <= ctx.nowMs;
    default:
        // FastRun/Dash: actor->CanAct() (and not flying) - always true locally.
        // Everything else uses unknown_libname_43: return 1.
        return true;
    }
}

bool ActorFsm::ChangeState(ActorState next, const FsmCtx& ctx) {
    if (!CanEnter(next, ctx)) return false;
    const ActorState prev = current_;
    previous_ = prev;
    current_  = next;
    seenAirborne_ = false; landing_ = false; seenActionMove_ = false;
    if (next == STATE_DODGE_LEFT || next == STATE_DODGE_RIGHT) {
        lastDodgeStartMs_ = ctx.nowMs;   // CDodgeState::OnEnter (sub_7E1AD0) -> dword_A53D70
        dodgeEverStarted_ = true;
    }
    if (onEnter_) onEnter_(next, prev);
    return true;
}

void ActorFsm::BaseInput(int inputId, const FsmCtx& ctx) {
    // The weapon sub-state sees the input first (CSelectByWeaponState::Input,
    // sub_7D39B0, forwards to it and then to the base table).
    auto ws = weaponRows_.find(current_);
    if (ws != weaponRows_.end()) {
        auto wi = ws->second.find(inputId);
        if (wi != ws->second.end()) { ChangeState(wi->second.main, ctx); return; }
    }
    auto st = table_.find(current_);
    if (st == table_.end()) return;
    auto it = st->second.find(inputId);
    if (it == st->second.end()) return;   // no row: the input is ignored
    const Action a = it->second;          // copy: ChangeState may touch table_ users
    if (!a.mainSub) { ChangeState(a.main, ctx); return; }
    // CMainSubStateInputAction::Execute (sub_7C6040)
    if (CanEnter(a.main, ctx)) { ChangeState(a.main, ctx); return; }
    if (a.hasSub) ChangeState(a.sub, ctx);
}

void ActorFsm::Input(int inputId, const FsmCtx& ctx) {
    switch (current_) {
    case STATE_STANDBY: {
        // CStandbyState::Input (sub_7CB010), targets NORMAL/RUN/JUMP stored by
        // sub_7CAFD0 in InitStandbyState. STANDBY is the one-input pause after
        // an action ends; the very next STOP or MOVE resolves it.
        const bool ground = ctx.air == AIR_GROUND;
        if (inputId == IN_STOP) { ChangeState(ground ? STATE_NORMAL : STATE_JUMP, ctx); return; }
        if (inputId == IN_MOVE) { ChangeState(ground ? STATE_RUN    : STATE_JUMP, ctx); return; }
        break;
    }
    case STATE_FALL:
        // CFallState::Input (sub_8B0A10): an external LEVITATE_END is ignored;
        // FALL only leaves through its own landing timer (Tick).
        if (inputId == IN_LEVITATE_END) return;
        break;
    default:
        break;
    }
    BaseInput(inputId, ctx);
}

void ActorFsm::Tick(const FsmCtx& ctx) {
    switch (current_) {
    case STATE_JUMP:
    case STATE_FALL: {
        // CJumpState phases 5/6 and CFallState phase 1: touching the ground
        // starts the jump_landdelay timer; going airborne again cancels it.
        // When it runs out JUMP changes to its stored STANDBY (+156) and FALL
        // feeds itself LEVITATE_END through the base Input (-> STANDBY).
        if (ctx.air != AIR_GROUND) { seenAirborne_ = true; landing_ = false; break; }
        if (!landing_) { landing_ = true; landEndMs_ = ctx.nowMs + tempo_.jumpLandDelayMs; }
        if (ctx.nowMs >= landEndMs_) {
            if (current_ == STATE_JUMP) ChangeState(STATE_STANDBY, ctx);
            else BaseInput(IN_LEVITATE_END, ctx);
        }
        break;
    }
    case STATE_BOUNDJUMP:
        // CBoundJumpState lands into its stored STANDBY (sub_7E2B00) after
        // walljump_landing_time.
        if (ctx.wallJumpMoving || ctx.air != AIR_GROUND) { landing_ = false; break; }
        if (!landing_) { landing_ = true; landEndMs_ = ctx.nowMs + tempo_.wallJumpLandingMs; }
        if (ctx.nowMs >= landEndMs_) ChangeState(STATE_STANDBY, ctx);
        break;
    case STATE_DODGE_LEFT:
    case STATE_DODGE_RIGHT:
        // CDodgeState::Tick (sub_7E1680): slide, decelerate, then
        // ChangeState(+176 = STANDBY). The slide/decel timing itself is
        // movement.cpp's (dodge_dodge_time / dodge_decel / dodge_delay).
        if (!ctx.dodgeMoving) ChangeState(STATE_STANDBY, ctx);
        break;
    default:
        break;
    }
}

} // namespace game
