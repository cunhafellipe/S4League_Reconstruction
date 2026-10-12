// See actor_input.h. Each unit below is the Update (vtable slot 8) / Check
// (slot 9) pair of the matching Common::InputUnit class in the beta client.
#include "actor_input.h"

namespace game {
namespace {

constexpr uint32_t kForever = 10000000;   // the beta's "no window" value

// CKeyDownInput (sub_89E9F0 / sub_89E940): fires on the press edge.
class KeyDown : public InputUnit {
public:
    KeyDown(int id, int vk) : InputUnit(id), vk_(vk) {}
    void Update(const KeyDownFn& key, uint32_t) override { prev_ = now_; now_ = key(vk_); }
    bool Check(uint32_t) override { return now_ && !prev_; }
private:
    int vk_; bool now_ = false, prev_ = false;
};

// CKeyUpInput (sub_89E0B0 / sub_89DFD0): fires once on release, if the key
// was held no longer than maxHeld and pressed more than minSinceDown ago.
class KeyUp : public InputUnit {
public:
    KeyUp(int id, int vk, uint32_t maxHeld, uint32_t minSinceDown)
        : InputUnit(id), vk_(vk), maxHeld_(maxHeld), minSince_(minSinceDown) {}
    void Update(const KeyDownFn& key, uint32_t nowMs) override {
        prev_ = now_; now_ = key(vk_);
        if (now_ && !prev_) downT_ = nowMs;
        else if (!now_ && prev_) upT_ = nowMs;
    }
    bool Check(uint32_t nowMs) override {
        if (upT_ <= downT_ || upT_ - downT_ > maxHeld_ || nowMs - downT_ <= minSince_) return false;
        downT_ = upT_;   // consumed: this[12] = this[13]
        return true;
    }
private:
    int vk_; uint32_t maxHeld_, minSince_;
    bool now_ = false, prev_ = false;
    uint32_t downT_ = 0, upT_ = 0;
};

// CKeyDownTwiceInput (sub_89E790 / sub_89E6E0) - the FASTRUN double tap.
// On a press edge: if more than `window` passed since the anchoring press the
// press becomes the new anchor, otherwise the unit fires. It fires on the
// frame of the second press itself; nothing waits for the window to expire.
// The anchor is NOT moved on a fire, so a third tap still inside the window
// fires again - that is the client's behaviour, kept as-is.
class KeyDownTwice : public InputUnit {
public:
    KeyDownTwice(int id, int vk, uint32_t window) : InputUnit(id), vk_(vk), window_(window) {}
    void Update(const KeyDownFn& key, uint32_t nowMs) override {
        prev_ = now_; now_ = key(vk_);
        if (now_ && !prev_) {
            if (nowMs - anchor_ > window_) anchor_ = nowMs;
            else fired_ = true;
        }
    }
    bool Check(uint32_t) override { if (!fired_) return false; fired_ = false; return true; }
private:
    int vk_; uint32_t window_;
    bool now_ = false, prev_ = false, fired_ = false;
    uint32_t anchor_ = 0;
};

// CKeyPressingWithDownInput (sub_89E330 / sub_89E260): `hold` is down (and
// went down less than `window` ago) and `down` has a press edge this frame.
// This is how A/D + Space dodges and W/S + Space jumps.
class KeyPressingWithDown : public InputUnit {
public:
    KeyPressingWithDown(int id, int vkHold, int vkDown, uint32_t window)
        : InputUnit(id), hold_(vkHold), down_(vkDown), window_(window) {}
    void Update(const KeyDownFn& key, uint32_t nowMs) override {
        downPrev_ = downNow_; downNow_ = key(down_);
        holdPrev_ = holdNow_; holdNow_ = key(hold_);
        if (holdNow_ && !holdPrev_) holdT_ = nowMs;
    }
    bool Check(uint32_t nowMs) override {
        return holdNow_ && nowMs - holdT_ < window_ && downNow_ && !downPrev_;
    }
private:
    int hold_, down_; uint32_t window_;
    bool downNow_ = false, downPrev_ = false, holdNow_ = false, holdPrev_ = false;
    uint32_t holdT_ = 0;
};

// CDirectionInput (sub_81D730): fires every frame the four keys add up to a
// non-zero direction (A+D alone cancels out and does not fire).
class Direction : public InputUnit {
public:
    Direction(int id, int left, int fwd, int right, int back)
        : InputUnit(id), l_(left), f_(fwd), r_(right), b_(back) {}
    void Update(const KeyDownFn& key, uint32_t) override {
        dx_ = (key(r_) ? 1 : 0) - (key(l_) ? 1 : 0);
        dz_ = (key(f_) ? 1 : 0) - (key(b_) ? 1 : 0);
    }
    bool Check(uint32_t) override { return dx_ != 0 || dz_ != 0; }
private:
    int l_, f_, r_, b_; int dx_ = 0, dz_ = 0;
};

// CInputAlways (unknown_libname_417: return 1). Registered last, so it only
// gets through when nothing exclusive fired before it - in practice: STOP
// whenever no direction is held.
class Always : public InputUnit {
public:
    explicit Always(int id) : InputUnit(id) {}
    void Update(const KeyDownFn&, uint32_t) override {}
    bool Check(uint32_t) override { return true; }
};

} // namespace

std::vector<int> UserInput::Poll(const KeyDownFn& key, uint32_t nowMs) {
    std::vector<int> fired;
    for (auto& u : units_) u->Update(key, nowMs);
    for (auto& u : units_) {
        if (!u->Check(nowMs)) continue;
        fired.push_back(u->id());
        if (u->exclusive()) break;
    }
    return fired;
}

void UserInput::BindDefaultLocomotion(int vkLeft, int vkFwd, int vkRight, int vkBack,
                                      int vkJump, int vkSit) {
    // Same order as sub_8C37A0; the weapon/skill/weapon-swap units that sit
    // between these in the client are left to scenes.cpp for now.
    Add(std::make_unique<KeyPressingWithDown>(IN_JUMP,        vkFwd,   vkJump, kForever));
    Add(std::make_unique<KeyPressingWithDown>(IN_JUMP,        vkBack,  vkJump, kForever));
    Add(std::make_unique<KeyPressingWithDown>(IN_DODGE_LEFT,  vkLeft,  vkJump, kForever));
    Add(std::make_unique<KeyPressingWithDown>(IN_DODGE_RIGHT, vkRight, vkJump, kForever));
    Add(std::make_unique<KeyDown>(IN_JUMP, vkJump));
    Add(std::make_unique<KeyDown>(IN_SIT, vkSit))->SetExclusive(false);
    Add(std::make_unique<KeyUp>(IN_SIT_CANCEL, vkSit, kForever, 0));
    Add(std::make_unique<KeyDownTwice>(IN_FASTRUN, vkFwd, 500));
    Add(std::make_unique<KeyUp>(IN_FASTRUN_END, vkFwd, kForever, 0));
    Add(std::make_unique<Direction>(IN_MOVE, vkLeft, vkFwd, vkRight, vkBack));
    Add(std::make_unique<Always>(IN_STOP));
}

} // namespace game
