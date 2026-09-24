// Ported 1:1 from C:\S4Web2\engine\scn_anim.js.
#include "anim_controller.h"
#include "actorstates.h"
#include <algorithm>
#include <cmath>

namespace anim {

std::unordered_map<std::string, float> g_clipRate;
std::unordered_map<std::string, ClipTrim> g_clipTrim;

uint32_t ApplyTrim(const std::string& clip, float tick, bool loop) {
    auto it = g_clipTrim.find(clip);
    if (it == g_clipTrim.end() || !(it->second.end - it->second.start > 100.f)) return (uint32_t)tick;
    float range = it->second.end - it->second.start;
    float t = loop ? std::fmod(tick, range) : (std::min)(tick, range);
    if (t < 0.f) t += range;
    return (uint32_t)(it->second.start + t);
}

static float ClipRate(const std::string& clip) {
    auto it = g_clipRate.find(clip);
    return it != g_clipRate.end() ? it->second : 1.f;
}

// 00008-015 (RUN 8-way) and 00057-064 (SIT walk) reuse the SAME leg cycle at
// different speeds, so they need the speed warp. 00068/IB068 (FASTRUN) is a
// DEDICATED clip already authored at its own sprint cadence - warping it (with
// curSpeed=RUN over REFSPEED=WALK, ~1.3x) made the legs spin faster than the
// body advanced ("skating" look).
static bool IsLoco(const std::string& c) {
    return (c >= "00008" && c <= "00015") || (c >= "00057" && c <= "00064");
}
static bool IsAction(ActorState s) {
    return s == STATE_JUMP || s == STATE_FALL || s == STATE_DODGE_LEFT || s == STATE_DODGE_RIGHT || s == STATE_BOUNDJUMP;
}

// Phases (ExitRule: 0 HOLD_LAST, 1 AFTER_MS, 2 TILL_APEX, 3 TILL_LAND)
enum ExitRule { HOLD_LAST = 0, AFTER_MS = 1, TILL_APEX = 2, TILL_LAND = 3 };
struct Phase { int slot; ExitRule rule; float ms; };

static const Phase kJumpMovePh[] = { {2, TILL_LAND, 0}, {4, AFTER_MS, 250} };
static const Phase kJumpStatPh[] = { {1, TILL_APEX, 0}, {5, TILL_LAND, 0}, {3, AFTER_MS, 500} };
static const Phase kFallPh[]     = { {0, TILL_LAND, 0}, {1, AFTER_MS, 500} };
// ActorState_Dodge (Lua) declares 6 params: FRONT/BACK/LEFT/RIGHT + DELAY_FRONT/
// DELAY_BACK (slots 4,5) - FRONT/BACK are a 2-phase roll (clip -> recovery clip
// in slot 4/5); LEFT/RIGHT are a SINGLE phase with blendOut=700 in
// their SetAnim. The third numeric value is blendOut, not an action delay.
static const Phase kDodgeFrontPh[] = { {0, AFTER_MS, 600}, {4, AFTER_MS, 500} };
static const Phase kDodgeBackPh[]  = { {1, AFTER_MS, 600}, {5, AFTER_MS, 500} };
static const Phase kDodgeLeftPh[]  = { {2, AFTER_MS, 1000} };
static const Phase kDodgeRightPh[] = { {3, AFTER_MS, 1000} };
// CBoundJumpState: authored 0.2s wall delay -> rise -> fall -> 0.2s landing.
// A/B are alternatives selected by the native state; this controller uses A.
static const Phase kBoundPh[]      = { {0, AFTER_MS, 200}, {1, TILL_APEX, 0}, {3, TILL_LAND, 0}, {5, AFTER_MS, 200} };

static const Phase* PhaseListFor(ActorState s, const AnimCtx& c, int& count) {
    if (s == STATE_JUMP) { if (c.jumpMoving) { count = 2; return kJumpMovePh; } count = 3; return kJumpStatPh; }
    if (s == STATE_FALL) { count = 2; return kFallPh; }
    if (s == STATE_DODGE_LEFT || s == STATE_DODGE_RIGHT) {
        int d = (c.dodgeDir >= 0 && c.dodgeDir <= 3) ? c.dodgeDir : 0;
        if (d == 0) { count = 2; return kDodgeFrontPh; }
        if (d == 1) { count = 2; return kDodgeBackPh; }
        if (d == 2) { count = 1; return kDodgeLeftPh; }
        count = 1; return kDodgeRightPh;
    }
    if (s == STATE_BOUNDJUMP) { count = 4; return kBoundPh; }
    count = 0; return nullptr;
}

// GATE - active slot from context (client vtbl+0xf8).
static int Gate(ActorState s, const AnimCtx& c) {
    bool f = c.inZ > 0.5f, b = c.inZ < -0.5f, l = c.inX < -0.5f, r = c.inX > 0.5f;
    switch (s) {
        case STATE_NORMAL: case STATE_IDLE: return 0;
        case STATE_FASTRUN: return 0;
        case STATE_RUN:
            if (f && l) return 9; if (f && r) return 12; if (b && l) return 11; if (b && r) return 14;
            if (b) return 15; if (l) return 10; if (r) return 13; return 8;
        case STATE_SIT:
            if (!c.moving) return 0;
            if (f && l) return 2; if (f && r) return 4; if (b && l) return 7; if (b && r) return 8;
            if (b) return 6; if (l) return 3; if (r) return 5; return 1;
        case STATE_JUMP:
            if (c.grounded) return c.jumpMoving ? 4 : 3;
            if (c.jumpMoving) return 2; if (c.velY > 0.f) return 1; return 5;
        case STATE_FALL: return c.grounded ? 1 : 0;
        case STATE_DODGE_LEFT: case STATE_DODGE_RIGHT: return (c.dodgeDir >= 0 && c.dodgeDir <= 3) ? c.dodgeDir : 0;
        case STATE_BOUNDJUMP: if (c.grounded) return 5; if (c.velY > 0.f) return 1; return 3;
        // Combo chain: SwordAtkWeakState_<Arma> carries one SetAnim row per hit;
        // scenes.cpp advances weakCombo on a fresh click within the combo window,
        // otherwise resets to 0 (hit 1). Not part of scn_anim.js's gate (JS runs
        // attacks through Crossfader instead) - see anim_states.h.
        case STATE_USE_WEAPON1_WEAK: return c.weakCombo;
        default: return 0;
    }
}

void AnimController::LoadDefs(const std::unordered_map<std::string, std::vector<SAnimParam>>& defs) {
    static const std::pair<const char*, ActorState> kMap[] = {
        {"NORMAL", STATE_NORMAL}, {"RUN", STATE_RUN}, {"SIT", STATE_SIT}, {"JUMP", STATE_JUMP},
        {"BOUNDJUMP", STATE_BOUNDJUMP}, {"FALL", STATE_FALL}, {"FASTRUN", STATE_FASTRUN},
    };
    for (auto& kv : kMap) {
        auto it = defs.find(kv.first);
        if (it != defs.end() && !it->second.empty()) {
            ownedDefs_[kv.second] = it->second;
            defs_[kv.second] = &ownedDefs_[kv.second];
        }
    }
    auto dodge = defs.find("DODGE");
    if (dodge != defs.end() && !dodge->second.empty()) {
        ownedDefs_[STATE_DODGE_LEFT] = dodge->second;
        ownedDefs_[STATE_DODGE_RIGHT] = dodge->second;
        defs_[STATE_DODGE_LEFT] = &ownedDefs_[STATE_DODGE_LEFT];
        defs_[STATE_DODGE_RIGHT] = &ownedDefs_[STATE_DODGE_RIGHT];
    }
}

const std::vector<SAnimParam>* AnimController::StateDef(ActorState s) const {
    auto it = defs_.find(s);
    return it != defs_.end() ? it->second : nullptr;
}

int AnimController::LoadFromLua(const LuaStates& lua, const char* weapon) {
    if (!lua.loaded()) return 0;
    std::string w = (weapon && *weapon) ? weapon : "WeaponUnused";

    auto defs = BaseParams(lua.parsed());
    auto over = WeaponDefs(lua.parsed(), w);
    for (auto& kv : over) defs[kv.first] = kv.second;
    LoadDefs(defs);

    WeaponAnim wa = ResolveWeaponAnim(lua.parsed(), w, &lua.attackMap(), &lua.attribMap());
    ownedDefs_[STATE_USE_WEAPON1_WEAK] = ToSAnimParams(wa.atkWeak);
    defs_[STATE_USE_WEAPON1_WEAK] = &ownedDefs_[STATE_USE_WEAPON1_WEAK];
    ownedDefs_[STATE_USE_WEAPON1_STRONG] = wa.atkStrong ? ToSAnimParams({ *wa.atkStrong }) : std::vector<SAnimParam>{};
    defs_[STATE_USE_WEAPON1_STRONG] = &ownedDefs_[STATE_USE_WEAPON1_STRONG];

    int covered = 0;
    for (auto& kv : defs) if (!kv.second.empty()) ++covered;
    if (!wa.atkWeak.empty()) ++covered;
    if (wa.atkStrong) ++covered;
    return covered;
}

int AnimController::LuaRowCount(ActorState s) const {
    const std::vector<SAnimParam>* d = StateDef(s);
    return d ? (int)d->size() : 0;
}

scn::Pose AnimController::Update(ActorState state, const AnimCtx& rawCtx, float dtMs, float clockScale) {
    // ctx.blendMin/blendMax default to 0/1e9 (no clamp) when the caller doesn't set
    // them per-frame (S4Web2's own menu does); fall back to the controller's own
    // tunables (blendFloorMs/blendCapMs, edited by scenes.cpp's F3 menu) instead.
    AnimCtx ctx = rawCtx;
    if (ctx.blendMin <= 0.f)   ctx.blendMin = blendFloorMs;
    if (ctx.blendMax >= 1e9f)  ctx.blendMax = blendCapMs;
    clockScale *= speedScale;

    const std::vector<SAnimParam>* def = StateDef(state);
    int idx;

    if (IsAction(state)) {
        int n; const Phase* ph = PhaseListFor(state, ctx, n);
        if (state != seqState_) { seqState_ = state; phaseIdx_ = 0; phaseClock_ = 0.f; }
        phaseClock_ += dtMs;
        // AFTER_MS uses the authored clip duration. SetAnim's numeric fields are
        // startTime/blendIn/blendOut; blendOut must not extend the state lifetime.
        auto phaseMs = [&](const Phase& cur) -> float {
            const SAnimParam* p = (def && cur.slot >= 0 && cur.slot < (int)def->size()) ? &(*def)[cur.slot] : nullptr;
            if (cur.ms > 0.f) return cur.ms;
            if (!p || !ctx.durOf) return cur.ms;
            return (float(p->startMs) + ctx.durOf(p->clip)) / (std::max)(clockScale, 1e-4f);
        };
        while (ph && phaseIdx_ < n - 1) {
            const Phase& cur = ph[phaseIdx_];
            bool done = (cur.rule == AFTER_MS && phaseClock_ >= phaseMs(cur))
                     || (cur.rule == TILL_APEX && ctx.velY <= 0.f)
                     || (cur.rule == TILL_LAND && ctx.grounded);
            if (!done) break;
            ++phaseIdx_; phaseClock_ = 0.f;
        }
        const Phase& cur = ph[phaseIdx_ < n ? phaseIdx_ : n - 1];
        idx = cur.slot;
        if (phaseIdx_ < n - 1) actionBusy_ = true;
        else if (cur.rule == AFTER_MS) actionBusy_ = phaseClock_ < phaseMs(cur);
        else if (cur.rule == TILL_LAND) actionBusy_ = !ctx.grounded;
        else if (cur.rule == TILL_APEX) actionBusy_ = ctx.velY > 0.f;
        else actionBusy_ = true;
    } else {
        idx = Gate(state, ctx);
        seqState_ = STATE_NORMAL; actionBusy_ = false;
    }

    const SAnimParam* p = (def && idx >= 0 && idx < (int)def->size()) ? &(*def)[idx] : nullptr;

    if (p && (state != state_ || idx != paramIdx_)) {
        if (!p->reset && clipB_ == p->clip) {
            state_ = state; paramIdx_ = idx;
        } else {
            float fade = float((std::max)(prevBlendOut_, p->blendInMs));
            clipA_ = clipB_; clockA_ = clockB_; rateA_ = rateB_; locoA_ = locoB_; loopA_ = loopB_;
            clipB_ = p->clip;
            clockB_ = p->reset ? 0.f : clockB_;
            rateB_ = (ctx.syncWindowMs > 1.f && p->startMs > 0) ? float(p->startMs) * p->rate / ctx.syncWindowMs : p->rate;
            locoB_ = IsLoco(clipB_);
            loopB_ = p->loop;
            blendDur_ = (std::min)((std::max)(fade, ctx.blendMin), ctx.blendMax);
            blendT_ = (blendDur_ > 1.f) ? 0.f : 1.f;
            prevBlendOut_ = p->blendOutMs;
            state_ = state; paramIdx_ = idx;
        }
    } else if (!p && clipB_.empty()) {
        clipB_ = "00074"; clockB_ = 0.f; rateB_ = 1.f; locoB_ = false; blendT_ = 1.f;
        state_ = state; paramIdx_ = -1;
    } else if (!p) {
        state_ = state;
    }

    // clockScale (animSpeed) is NOT a locomotion-only slider: .scn durations/ticks
    // are in native tick units that need this factor to become real ms for ANY
    // clip. Gating it to IsLoco only left everything else (jump/dodge/attacks) at
    // raw ticks treated as ms - 4-5x slower than real.
    float warp = (ctx.refSpeed > 1.f) ? (std::max)(0.05f, (std::min)(ctx.speed / ctx.refSpeed, 8.f)) : 1.f;
    clockB_ += dtMs * rateB_ * (locoB_ ? warp : 1.f) * clockScale * ClipRate(clipB_);
    clockA_ += dtMs * rateA_ * (locoA_ ? warp : 1.f) * clockScale * ClipRate(clipA_);
    // Carry LOOP_FALSE through to every bone/model, including costume tracks
    // shorter than the main skeleton's clip. Clamping only this clock is not enough.
    if (!loopB_ && ctx.durOf) { float d = ctx.durOf(clipB_); if (d > 0.f) clockB_ = (std::min)(clockB_, d); }
    if (!loopA_ && ctx.durOf) { float d = ctx.durOf(clipA_); if (d > 0.f) clockA_ = (std::min)(clockA_, d); }
    if (blendDur_ > 1.f) { blendT_ += dtMs / blendDur_; if (blendT_ > 1.f) blendT_ = 1.f; }

    scn::Pose pose;
    if (blendT_ < 1.f && !clipA_.empty() && clipA_ != clipB_) {
        pose.clipA = clipA_; pose.tickA = ApplyTrim(clipA_, clockA_, loopA_); pose.loopA = loopA_;
        pose.clipB = clipB_; pose.tickB = ApplyTrim(clipB_, clockB_, loopB_); pose.loopB = loopB_;
        pose.blend = blendT_;
    } else {
        pose.clipA = clipB_; pose.tickA = ApplyTrim(clipB_, clockB_, loopB_); pose.loopA = loopB_;
        pose.blend = 0.f;
    }
    return pose;
}

void Crossfader::Set(const SAnimParam* param, const std::function<float(const std::string&)>& durOf) {
    if (!param || param->clip == clipB_) return;
    float fade = float(prevBlendOut_);   // cross uses the OUTGOING clip's blendOut (1:1 C++)
    clipA_ = clipB_; clockA_ = clockB_; rateA_ = rateB_; loopA_ = loopB_; durA_ = durB_;
    clipB_ = param->clip; clockB_ = param->reset ? 0.f : clockB_;
    rateB_ = param->rate ? param->rate : 1.f; loopB_ = param->loop;
    durB_ = durOf ? durOf(param->clip) : 0.f;
    blendDur_ = (std::max)(fade, float(param->blendInMs)); blendT_ = blendDur_ > 1.f ? 0.f : 1.f; prevBlendOut_ = param->blendOutMs;
}

scn::Pose Crossfader::Update(float dtMs, float clockScale) {
    clockB_ += dtMs * rateB_ * clockScale * ClipRate(clipB_);
    clockA_ += dtMs * rateA_ * clockScale * ClipRate(clipA_);
    if (!loopB_ && durB_ > 0.f && clockB_ > durB_) clockB_ = durB_;   // one-shot: freeze at the end
    if (!loopA_ && durA_ > 0.f && clockA_ > durA_) clockA_ = durA_;
    if (blendDur_ > 1.f) { blendT_ += dtMs / blendDur_; if (blendT_ > 1.f) blendT_ = 1.f; }
    scn::Pose pose;
    if (blendT_ < 1.f && !clipA_.empty() && clipA_ != clipB_) {
        pose.clipA = clipA_; pose.tickA = ApplyTrim(clipA_, clockA_, loopA_); pose.loopA = loopA_;
        pose.clipB = clipB_; pose.tickB = ApplyTrim(clipB_, clockB_, loopB_); pose.loopB = loopB_;
        pose.blend = blendT_;
    } else {
        pose.clipA = clipB_; pose.tickA = ApplyTrim(clipB_, clockB_, loopB_); pose.loopA = loopB_;
        pose.blend = 0.f;
    }
    return pose;
}

} // namespace anim
