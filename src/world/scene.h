// IScene - a game state (login, lobby, room, in-game, ...).
//
// Maps the scene-object vtable used by World (FUN_012928b0 / FUN_01292bb0):
//   +0x18 OnEnter   +0x1c OnLeave   +0x20 OnUpdate   +0x24 OnRender
//   +0x2c GetInputMode  (feeds FUN_014242c0 - the input/clip mode for the scene)
#pragma once
struct IRenderer;
struct World;

struct IScene {
    World* world = nullptr;   // set by World on transition; use to ChangeScene
    virtual ~IScene() = default;
    virtual void OnEnter()  {}   // vtable +0x18
    virtual void OnLeave()  {}   // vtable +0x1c
    virtual void OnUpdate() {}   // vtable +0x20
    virtual void OnRender(IRenderer*) {}   // vtable +0x24
    virtual int  GetInputMode() { return 0; }   // vtable +0x2c
};
