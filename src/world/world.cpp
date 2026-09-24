// World - scene/state manager implementation.
//
// FUN_012928b0  World::Update   (scene transition + tick + subsystems)
// FUN_01292bb0  World::Present   (scene render + layer flush)
//
// ponytail: the original also drives a per-world frame-limiter (+0x1e4) and
// misc subsystems (+0xd8 net?, +0x1ec, +0x144) each tick. Deferred - the scene
// state machine below is the core. Add them when a scene needs them.

#include "world/world.h"
#include "world/scene.h"

void World::Update()
{
    // Scene transition (FUN_012928b0 spine).
    if (pending) {
        if (current == pending) {
            pending = nullptr;                 // already active
        } else {
            if (current) {
                current->OnLeave();            // vtable +0x1c
                current = nullptr;
            }
            current = pending;
            current->world = this;             // scenes reach back to ChangeScene
            current->OnEnter();                // vtable +0x18
            // FUN_014242c0(_108, current->GetInputMode()): apply scene input mode.
            pending = nullptr;
        }
    }

    // Tick the active scene.
    if (current) current->OnUpdate();          // vtable +0x20
}

void World::Present()
{
    if (current) current->OnRender(renderer);  // vtable +0x24
    // FUN_01ce23e0(_fc) / FUN_01ce23e0(_100): flush the two render layers (3D/UI).
}
