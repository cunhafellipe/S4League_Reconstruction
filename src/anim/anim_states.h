// S4 animation state tables, ported 1:1 from C:\S4Web2\engine\scn_anim.js.
// These are FALLBACK values only - AnimController::LoadDefs overrides them with
// the real numbers parsed from actorstates_*.lua (scn/actorstates.h) the moment
// that load succeeds; they only matter if that load fails.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace anim {

enum ActorState {
    // Runtime IDs are the 1-based values used by CActorStateGroup. The Lua
    // file builds them from a zero-based counter but its trailing annotations
    // and actor context use these IDs.
    STATE_WAIT       = 3,
    STATE_STANDBY    = 4,
    STATE_NORMAL     = 5,
    STATE_RUN        = 6,
    STATE_SIT        = 7,
    STATE_JUMP       = 8,
    STATE_BOUNDJUMP  = 9,
    STATE_FALL       = 10,
    STATE_DODGE_LEFT = 11,
    STATE_DODGE_RIGHT= 12,
    STATE_FASTRUN    = 21,
    STATE_IDLE       = 29,
    // Combat states. Numbered as the BETA binary numbers them (the builder
    // sub_8339E0 creates them by index and names them through
    // Game::GameSetting::Init*State). The S1 Lua constants differ here: S1 has
    // no SKILL_SENSOR (34 in the beta) but adds USE_WEAPON2_WEAK/STRONG, which
    // is why its USE_WEAPON1_WEAK/STRONG read 43/44 instead of 42/43.
    STATE_USE_WEAPON1        = 40,
    STATE_USE_WEAPON2        = 41,
    STATE_USE_WEAPON1_WEAK   = 42,
    STATE_USE_WEAPON1_STRONG = 43,
    STATE_USE_WEAPON1_JUMP   = 44,
    STATE_USE_WEAPON1_STRONG1= 45,
};

struct SAnimParam {
    std::string clip;
    int   startMs = 0;
    int   blendInMs = 0;
    int   blendOutMs = 0;
    float rate = 1.f;
    bool  reset = false;
    bool  loop = true;      // real Lua defs may set this false (LOOP_FALSE)
};

inline SAnimParam P(const char* clip, int startMs, int blendInMs, float rate, bool reset) {
    SAnimParam p; p.clip = clip; p.startMs = startMs; p.blendInMs = blendInMs; p.rate = rate; p.reset = reset;
    return p;
}

inline const std::vector<SAnimParam> kNormalParams = { P("00074", 0, 0, 1, false) };

inline const std::vector<SAnimParam> kRunParams = {
    P("00008",0,0,1,false), P("00009",0,0,1,false), P("00010",0,0,1,false), P("00014",0,0,1,false),
    P("00011",0,0,1,false), P("00012",0,0,1,false), P("00015",0,0,1,false), P("00013",0,0,1,false),
    P("00008",0,0,1,false), P("00009",0,0,1,false), P("00010",0,0,1,false), P("00014",0,0,1,false),
    P("00011",0,0,1,false), P("00012",0,0,1,false), P("00015",0,0,1,false), P("00013",0,0,1,false),
};

inline const std::vector<SAnimParam> kSitParams = {
    P("00065",0,0,1,false), P("00057",0,0,1,false), P("00058",0,0,1,false), P("00059",0,0,1,false),
    P("00060",0,0,1,false), P("00061",0,0,1,false), P("00062",0,0,1,false), P("00063",0,0,1,false), P("00064",0,0,1,false),
};

// Corrected against real actorstates_gameplay.lua (ActorState_Jump/BoundJump/Fall/Dodge):
// Fallback rows preserve the principal incoming blend value.
inline const std::vector<SAnimParam> kJumpParams = {
    P("00000",0,500,1,true), P("00001",0,500,1,true), P("00004",600,100,1,true),
    P("00003",500,200,0.8f,true), P("00000",0,100,0.8f,true), P("00002",0,1000,1,true),
};

inline const std::vector<SAnimParam> kBoundJumpParams = {
    P("00066",500,0,1,true), P("00067",500,100,1,true), P("00067_2",500,200,0.8f,true),
    P("00002",0,500,1,true), P("00002",0,500,1,true), P("00003",0,500,1,true), P("00002",0,500,1,true),
};

inline const std::vector<SAnimParam> kFallParams = { P("00002",0,1000,1,true), P("00003",380,200,1,true) };

inline const std::vector<SAnimParam> kDodgeParams = {
    P("00038",0,0,1,true), P("00041",0,0,1,true), P("00005",0,0,1,true),
    P("00056",0,0,1,true), P("00039",0,500,1,true), P("00042",0,500,1,true),
};

inline const std::vector<SAnimParam> kFastRunParams = { P("00068",0,0,1,true), P("IB068",0,0,1,true) };

inline std::unordered_map<int, const std::vector<SAnimParam>*> MakeDefaultStateDefs() {
    return {
        { STATE_STANDBY,    nullptr },
        { STATE_NORMAL,     &kNormalParams },
        { STATE_RUN,        &kRunParams },
        { STATE_SIT,        &kSitParams },
        { STATE_JUMP,       &kJumpParams },
        { STATE_BOUNDJUMP,  &kBoundJumpParams },
        { STATE_FALL,       &kFallParams },
        { STATE_DODGE_LEFT, &kDodgeParams },
        { STATE_DODGE_RIGHT,&kDodgeParams },
        { STATE_FASTRUN,    &kFastRunParams },
        { STATE_IDLE,       nullptr },
    };
}

} // namespace anim
