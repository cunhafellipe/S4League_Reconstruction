// actorstates_*.lua run natively: the real scripts are executed in Lua 5.1
// with a recorder standing in for the client's CActorState bindings
// (state:GetAnimParam(i):SetAnim(...), GetLowerPartStateSet(i):
// GetWalkAnimParam(dir):SetAnim(...), AddSound/AddSoundBySex, AddSequence,
// AddGhostTrail, GetCustomAnimBlender(n):Get<Walk|Jump|Fly><Upper|Lower>
// AnimParam(dir):SetBlendingAnim(...), SetAniNameForHuman). Every state
// function of every file is called once with a fresh recorder; the calls it
// makes - with their real argument values, including calls made through
// helper functions - become its ActorFunc. A method the recorder does not
// know is counted in `unhandled` instead of being dropped silently.
#pragma once
#include "anim/actorstates.h"
#include <map>
#include <string>

namespace luaw {

struct ActorStatesRun {
    int files = 0, functions = 0, failed = 0;
    std::map<std::string, int> unhandled;   // "state:Foo" / "AnimParam:Bar" -> calls
    std::map<std::string, std::string> errors;   // function -> Lua error (state functions only)
};

// false when the scripts could not be loaded at all (caller falls back).
bool RunActorStates(const std::string& scriptDir, anim::ParsedActorStates& out, ActorStatesRun* report = nullptr);

}  // namespace luaw
