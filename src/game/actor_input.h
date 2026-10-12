// Player input -> actor-state input IDs, reconstructed 1:1 from the 2008 beta
// client (IDA port 5076, full C++ symbols).
//
// The beta does not poll "is W held / was Space pressed" inside the gameplay
// code. Game::CUserInput owns an ordered list of Common::InputUnit objects,
// each bound to one input ID and one or two keys. Every frame
// (CUserInput::Update, sub_81E9F0):
//
//   for each unit: unit->Update()                 // sample keys, edges, timers
//   for each unit: if (unit->Check()) {
//                      stateMachine->Input(id);   // dispatch
//                      if (unit->exclusive) break; // IInputUnit+24, default 1
//                  }
//
// so at most one exclusive input reaches the state machine per frame, and the
// registration order (Game::sub_8C37A0) decides which wins.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace game {

// Input IDs as the BINARY numbers them. The Lua S1 ActorStates_Constants.lua
// enum is the same list but one shorter before USE_WEAPON_BEGIN, so every
// value from there on is Lua+1. Verified against both the key bindings
// (sub_8C37A0) and the transition tables (Game::GameSetting::Init*State):
// RELOAD=28, FASTRUN=36, CHANGE_WEAPON_1..3=30..32, SIT=16 line up.
enum InputId : int {
    IN_STOP            = 0,   // CInputAlways - every frame no direction is held
    IN_MOVE            = 2,   // CDirectionInput - every frame WASD is held
    IN_USE_WEAPON1     = 4,
    IN_USE_WEAPON1_WEAK   = 7,   // released within 200 ms
    IN_USE_WEAPON1_STRONG = 8,   // held for 200 ms
    IN_USE_WEAPON2     = 9,
    IN_JUMP            = 13,
    IN_SIT             = 16,
    IN_SIT_CANCEL      = 17,
    IN_SPECIAL_ACTION1 = 18,
    IN_DODGE_LEFT      = 25,
    IN_DODGE_RIGHT     = 26,
    IN_RELOAD          = 28,
    IN_FASTRUN         = 36,
    IN_FASTRUN_END     = 37,
    IN_STUN            = 39,
    IN_BLOW            = 40,
    IN_DAMAGE          = 41,
    IN_DOWN            = 42,
    IN_LEVITATE        = 45,  // left the ground without jumping (physics, not a key)
    IN_LEVITATE_END    = 46,
    IN_NORMAL          = 47,
    IN_SOCIALACTION    = 49,
};

using KeyDownFn = std::function<bool(int vk)>;

// Common::IInputUnit. Time is in milliseconds, same clock as the beta's
// timer (all the windows below are authored in ms).
class InputUnit {
public:
    explicit InputUnit(int id) : id_(id) {}
    virtual ~InputUnit() = default;
    virtual void Update(const KeyDownFn& key, uint32_t nowMs) = 0;
    virtual bool Check(uint32_t nowMs) = 0;
    int  id() const { return id_; }
    bool exclusive() const { return exclusive_; }
    InputUnit* SetExclusive(bool e) { exclusive_ = e; return this; }   // sub_5FE740
private:
    int  id_;
    bool exclusive_ = true;   // IInputUnit ctor (sub_81D560) sets +24 = 1
};

class UserInput {
public:
    InputUnit* Add(std::unique_ptr<InputUnit> u) { units_.push_back(std::move(u)); return units_.back().get(); }
    // Units in registration order; returns the IDs that fired this frame.
    std::vector<int> Poll(const KeyDownFn& key, uint32_t nowMs);
    // The default PC bindings, in the exact order sub_8C37A0 registers them
    // (locomotion subset - weapon/skill keys still go through scenes.cpp).
    void BindDefaultLocomotion(int vkLeft, int vkFwd, int vkRight, int vkBack, int vkJump, int vkSit);
private:
    std::vector<std::unique_ptr<InputUnit>> units_;
};

} // namespace game
