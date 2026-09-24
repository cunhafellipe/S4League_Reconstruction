// World - the scene/state manager (Engine +0x54).
// Reconstructed from FUN_012928b0 (Update) + FUN_01292bb0 (Present).
#pragma once

struct IScene;
struct IRenderer;

struct World {
    IScene*    current  = nullptr;   // +0x110  active scene
    IScene*    pending  = nullptr;   // +0x114  requested next scene
    int        prevSceneId = 0;      // +0x118
    IRenderer* renderer = nullptr;   // for scene OnRender

    void ChangeScene(IScene* next) { pending = next; }  // request a transition
    void Update();    // FUN_012928b0 : handle transition + current->OnUpdate
    void Present();   // FUN_01292bb0 : current->OnRender + flush layers
};
