// Game states (Game::GameState::*), the concrete IScene implementations.
// Reversed names: IntroState -> TitleState_ConnectAuthServer -> ... (login flow).
#pragma once
#include "world/scene.h"
#include "net/proud_client.h"
#include "gui/xui.h"
#include "scn/scn.h"
#include "anim/anim_controller.h"
#include "anim/actorstates.h"
#include "lua/lua_weapon_fire.h"
#include "world/movement.h"
#include "seq/seq.h"
#include "seq/seq_player.h"
#include <vector>
#include <string>
#include <array>
#include <optional>

// One <intro> block from _eu_intro.x7 (a logo animation).
struct IntroBlock {
    int playTime = 1500, frameTime = 60;
    std::vector<std::string> frames;   // texture filenames (basename)
};

// Boot intro: plays every <intro> block in _eu_intro.x7 (Aeria, NEOWIZ, ...).
struct IntroState : IScene {
    std::vector<IntroBlock> blocks;
    IScene*  next = nullptr;
    unsigned start = 0;
    size_t   blockIdx = 0;
    void OnEnter()  override;
    void OnUpdate() override;
    void OnRender(IRenderer*) override;
    void OnLeave()  override;
};

// Title screen: title_back00.png bg + title_ani.png + "PRESS ANY KEY TO START".
// Click -> (TODO auth LoginToken) -> server select.
struct TitleState : IScene {
    IScene* next = nullptr;
    scn::Scene character;       // title_cha00.scn (13 meshes of title_ani.png)
    unsigned animStart = 0;     // GetTickCount() at enter; drives .scn animation
    void OnEnter()  override;
    void OnUpdate() override;
    void OnRender(IRenderer*) override;
};

// Title / login screen (1login.xui). "Press any key" -> next scene.
struct LoginState : IScene {
    gui::Widget ui;
    IScene* next = nullptr;   // where "press any key" goes (server select)
    void OnEnter()  override;
    void OnUpdate() override;
    void OnRender(IRenderer*) override;
};

// Server select (4server.xui).
struct ServerSelectState : IScene {
    gui::Widget ui;
    int selected = 0;                 // highlighted server row
    scn::Scene character3d;           // male_bip.scn = skeleton+anims (left panel preview)
    std::vector<scn::Scene> bodyParts; // default male body/leg/hand/foot/face/hair meshes
    int animIdx = 0;                  // current clip in character3d.animNames ('+' cycles)
    unsigned animStart = 0;           // GetTickCount() when the current clip started
    IScene* world3d = nullptr;        // F1 -> enter the 3D world (map + movement)
    void OnEnter()  override;
    void OnUpdate() override;         // button clicks (Refresh / row select)
    void OnRender(IRenderer*) override;
};

// In-game 3D world: loads a background map (model/background/*.scn per the
// bginfo [STATIC] list) + the male_bip character, third-person camera, WASD
// movement. First step toward the playable lobby/room.
// One world-space collision triangle, precomputed once at map load (the map is
// static) so collide-and-slide never re-transforms per frame. n = unit normal,
// bmin/bmax = AABB for broad-phase culling.
struct CollTri { float v0[3], v1[3], v2[3], n[3], bmin[3], bmax[3]; };

// One map as the client defines it: Resources/MapInfo/bginfo-*.ini, sections
// [MAPINFO] name=, [SKY] object_1= and [STATIC] object_N= (the .scn to load).
struct BgInfo {
    std::string ini;                    // file name (bginfo-square.ini)
    std::string name;                   // [MAPINFO] name=
    std::string sky;                    // [SKY] object_1
    std::vector<std::string> statics;   // [STATIC] object_N, in order
    std::vector<std::string> dynamics;  // [DYNAMIC] object_N (68 de 100 mapas traen)
    std::vector<std::string> bgm;       // [BGM] bgm_N playlist
};

struct WorldState : IScene {
    std::vector<BgInfo> maps;           // every bginfo-*.ini found (ESC map picker)
    int   mapIdx  = -1;                 // currently loaded entry in `maps`
    bool  mapMenu = false;              // ESC overlay open
    int   menuSel = 0;                  // highlighted row
    int   menuTop = 0;                  // first visible row (scroll)
    int   bgmIndex = 0;
    bool  animMenu = false;             // F3: ajuste de animacion en vivo (como el menu del cliente JS)
    int   animSel = 0;                  // fila seleccionada del menu de animacion
    // F4: selector de armas. Una entrada por NormalState_<X> del lua, emparejada
    // a un .scn de model/weapon por coincidencia de nombre (igual que el cliente JS).
    struct WeaponEntry { std::string states, scn; };
    std::vector<WeaponEntry> weaponList;
    bool  wpnMenu = false;
    int   wpnSel = 0, wpnTop = 0;
    void  BuildWeaponList();
    void  EquipEntry(int index);
    std::vector<scn::Scene> map;        // static map meshes (visible geometry)
    std::vector<CollTri> collTris;      // precomputed world-space collision triangles
    scn::Scene character;               // male_bip skeleton + anims
    std::vector<scn::Scene> bodyParts;  // default male body/leg/hand/foot/face/hair
    float charPos[3] = { 0, 0, 0 };     // character world position
    float charYaw = 0.f;                // character facing = camera yaw (S4: heading follows aim)
    float camYaw = 0.f;                 // camera look yaw (mouse + arrows)
    float camPitch = 0.25f;             // camera pitch (radians)
    float camDist = 500.f;              // third-person distance behind (client depthoffset=500)
    float camEye[3] = { 0, 0, 0 };      // smoothed camera position (spring-damper follow)
    float camVel[3] = { 0, 0, 0 };      // camera velocity (SmoothDamp state)
    float curFov = 60.f;                // current FOV (lerps 60 idle -> 72 running)
    bool  camInit = false;
    bool  cursorHidden = false;         // hide the Windows cursor while in-world (crosshair instead)
    float velY = 0.f;                   // vertical velocity (jump/gravity)
    float velX = 0.f, velZ = 0.f;       // horizontal velocity (smoothed accel/decel)
    float footOffset = 11.f;            // model-space feet offset ([ / ] to tune on floor)
    float collRadius = 53.f;            // horizontal collision-ellipsoid radius (- / = tune); vertical = *1.6 (S4 100:160)
    bool  grounded = true;
    world::MovementController moveCtrl;   // ported 1:1 from C:\S4Web2\world.html (moveXZ/hitWall/jump/dodge)
    unsigned lastTick = 0;              // for frame-independent dt
    float lastDt = 0.016f;              // dt from last OnUpdate (for camera smoothing)
    bool  spaceWasDown = false;         // Space edge (dodge/jump on fresh press)
    float dodgeVelX = 0.f, dodgeVelZ = 0.f, dodgeTimeLeft = 0.f;   // world::MovementController dodge state
    float wallJumpDelayLeft = 0.f, wallJumpTimeLeft = 0.f;
    float wallJumpVelX = 0.f, wallJumpVelZ = 0.f;
    float moveWeightEase = 1.f;   // world::MovementController's eased weaponWeightMul, persisted across frames
    float moveVelX = 0.f, moveVelZ = 0.f;   // world::MovementController's eased horizontal velocity, persisted across frames
    unsigned dodgeUntil = 0;            // dodge VELOCITY burst window (dodge_dodge_time 0.225s)
    unsigned dodgeClipUntil = 0;        // dodge CLIP plays fully until here (= clip duration); jump cancels
    std::string dodgeClip;              // current dodge animation clip
    unsigned char prevWASD = 0;         // previous WASD state (double-tap edge detect)
    unsigned char lastTapKey = 0;       // which direction was last tapped (same-key double-tap)
    unsigned lastTapTime = 0;           // last direction fresh-press (double-tap window)
    int pendingDodgeDir = 0;             // -1=A, +1=D, remembered until Space
    unsigned pendingDodgeTime = 0;       // A/D -> Space is accepted for 1 second
    bool  running = false;              // double-tapped -> run (walk otherwise)
    unsigned bounceUntil = 0;           // wall-bounce clip active until this tick
    unsigned airLockUntil = 0;          // after a wall-jump: keep the back+up momentum (no air-control) until here
    unsigned landUntil = 0;             // LANDING clip (00003) window after touching down from a real fall
    bool  bounding = false;             // jumped while moving forward = WALK-BOUND (00066->00067 sequence)
    unsigned jumpTakeoff = 0;           // tick the current jump/bound started (for the takeoff-clip window)
    std::string curClip = "00074";      // idle=00074 run=00008 sit=00065 jump=00001/02/03
    unsigned animStart = 0;
    anim::AnimController animCtrl;       // data-driven animation engine (gates + timers + blend)
    // --- arma equipada -----------------------------------------------------
    // luaStates queda vivo aca: los SAnimParam que el controller usa apuntan a
    // strings que viven adentro suyo.
    anim::LuaStates luaStates;
    scn::Scene  weapon;                 // .scn del arma (vacio = desarmado)
    std::string weaponStates;           // sufijo de NormalState_/RunState_ ("PlasmaSword")
    std::string weaponBone = "R_Hand_Dummy";   // dummy del bip al que cuelga (ver IsAttachDummyName)
    float weaponInvMount[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};   // inverso del dummy raiz del arma
    std::optional<int> weaponTypeId;    // WEAPONTYPE_* real (weapon_attach.x7), para el bone por-pieza
    // World (weapon-scene-space) de cada modelo del arma, resuelto UNA vez al
    // equipar (flat vs encadenado por pieza, ver scn::ResolveWeaponPieceWorlds).
    std::vector<std::array<float,16>> weaponPieceWorlds;
    // A CounterSword (y otras armas dobles) son dos archivos SCN distintos:
    // sceneIndex 0 = pieza derecha, sceneIndex 1 = pieza izquierda.
    struct WeaponRenderPart {
        scn::Scene scene;
        float invMount[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::vector<std::array<float,16>> pieceWorlds;
        std::string attackBone;
        std::string idleBone;
        std::string idleBoneOverride;
        int sceneIndex = 0;
    };
    std::vector<WeaponRenderPart> weaponParts;
    std::vector<std::string> weaponAdjustBones;
    struct WeaponBoneOverride { std::string weapon; int part = 0; std::string bone; };
    std::vector<WeaponBoneOverride> weaponBoneOverrides;
    bool boneEditMode = false;
    unsigned boneEditPopupUntil = 0;
    bool  weaponOk = false;
    int   weaponSlot = 0;               // 1/2/3 cambian de slot
    int      weakCombo = 0;             // current row in the weak-attack combo chain
    unsigned lastWeakAt = 0;            // last weak click (600ms combo window)
    // One weapon swing at a time: a single clip held for its wind-up + authored
    // duration + recovery delay, all read straight from the weapon's Lua data.
    anim::WeaponAnim weaponAnim;        // resolved in EquipWeaponFile for the current weapon
    // Real per-swing STATE:SetMoveSpeedRate() values, pulled once at equip time
    // by actually running the weapon's own Weapon_*.lua (see EquipWeaponFile).
    // Stay at 1.0 (no penalty) for any attack whose real Lua function name
    // isn't found under the StandardXStandAttack_Weak/Strong/Strong1 convention.
    float atkMoveSpeedRateWeak = 1.f, atkMoveSpeedRateStrong = 1.f, atkMoveSpeedRateStrong1 = 1.f;
    // STATE:SetHitStop(a,b)'s first value (ms) - how long the WHOLE SIM
    // near-freezes on this swing's impact frame, applied unconditionally (no
    // hit detection against other actors yet, so every swing gets its own
    // weapon's real hit-stop rather than none at all).
    float atkHitStopWeak = 0.f, atkHitStopStrong = 0.f, atkHitStopStrong1 = 0.f;
    unsigned hitStopUntil = 0;
    const anim::ActorAnim* atkParam = nullptr;
    bool  atkStrongMode = false;
    bool  atkHitDone = false;
    unsigned atkStart = 0;
    unsigned atkCooldownUntil = 0;
    // Real client bounces every action (attack/stun/jump/dodge/dash) through
    // STATE_WAIT then STATE_STANDBY before locomotion resumes - modeled as a
    // brief fastrun lockout so holding a move key through the end of a swing
    // or a weapon2 dash can't skate straight into a full sprint.
    unsigned fastrunLockoutUntil = 0;
    // Root the character in place (no move, no jump) for at least this long
    // from the moment a swing starts - a real attack commits you, it doesn't
    // let you freely walk/jump through it. Legs still play their own
    // lowerWeak/lowerStrong stance clip during this window (see curLowerPose),
    // just without actually translating the character.
    unsigned attackLockUntil = 0;
    // Whatever pose was actually playing the instant a swing/weapon2 started -
    // blended into the new clip over atkBlendMs instead of hard-cutting to it,
    // same idea as AnimController's own state-to-state crossfade.
    scn::Pose atkBlendFrom;
    static constexpr float atkBlendMs = 90.f;
    // WALK/RUN come straight from game_tempos.x7's real values.
    float moveSpeedMul = 1.f;
    // Left mouse button: a quick tap swings the weak combo (fired on release),
    // holding past the threshold swings strong instead (fired while still held).
    bool  lmbWasDown = false, mouseHeld = false, attackFired = false;
    unsigned mouseDownAt = 0;
    // Right mouse button: held pose (guard/dash/whatever the weapon's own Lua
    // data drives it as), no cooldown - it just tracks the button state.
    bool  rmbWasDown = false, rightHeld = false;
    unsigned rightHeldAt = 0, lastWeapon2At = 0;
    int weapon2Phase = 0;            // current row in weaponAnim.atkWeapon2's phase sequence
    unsigned weapon2PhaseStart = 0;  // when the current phase started (its own clock, RESET_TRUE)
    // Set on RMB press, cleared once the held/guard phase is reached AND the
    // button is no longer down - the wind-up phase(s) before that always play
    // out in full even if RMB is released early.
    bool weapon2Active = false;
    bool weapon2IsJump = false;     // committed aerial attack, not a held ground guard
    bool weapon2DropApplied = false;
    bool weapon2MoveBoxValid = false;
    float weapon2MoveBoxX = 0.f, weapon2MoveBoxZ = 0.f;
    std::string weapon2MoveBoxClip;
    luaw::WeaponFireState weapon2Lua;
    luaw::WeaponFireState jumpAttackLua;

    // A single .seq effect currently playing at some world position (a swing's
    // particle burst, a muzzle flash, ...).
    struct EffectNode { seq::ParticlePlayer player; std::string texFile; bool additive = false; };
    // A mesh a node loader hangs off the effect (the actual slash-arc/flash
    // model some effects use alongside their sparks). No transform animation
    // yet - it sits at the effect's spawn point for its whole lifetime.
    struct EffectModel { scn::Scene model; };
    struct ActiveEffect {
        std::vector<EffectNode> nodes;
        std::vector<EffectModel> models;
        uint32_t duration = 0;
        float x = 0, y = 0, z = 0, yaw = 0;
        unsigned startTick = 0;
    };
    std::vector<ActiveEffect> activeEffects;
    bool seqEffectsEnabled = false;   // .seq particle/model effects off for now, still being tuned
    void SpawnSeq(const std::string& relPath, float x, float y, float z, float yaw);
    void DrawEffects(IRenderer* r, unsigned tnow);

    void  EquipWeapon(int slot);
    void  EquipWeaponFile(const char* scnName, const char* states);
    scn::AnimPose curPose;              // pose produced by animCtrl this frame (used by OnRender)
    // Raw locomotion direction kept for the weapon SCN's own upper-body clips
    // (KN001..KN008, gun run poses, etc.). The character controller may be
    // playing a different clip name than the weapon scene.
    float weaponPoseInX = 0.f, weaponPoseInZ = 0.f;
    bool weaponPoseMoving = false;
    // Real per-weapon leg pose (state:GetLowerPartStateSet) while an attack's
    // own upper-body swing is active - legs keep walking/standing on their own
    // clip instead of freezing into the swing's whole-body pose. Set each
    // frame in Update() alongside curPose, nullptr (curLowerPoseActive=false)
    // whenever the current weapon/attack has no lower-body data.
    scn::AnimPose curLowerPose;
    bool curLowerPoseActive = false;
    bool  didJump = false;             // true while airborne FROM a jump (JUMP vs FALL state)
    bool  jumpMoving = false;          // this jump took off while moving -> 00004 move-rise plays the WHOLE arc
    int   dodgeDir = -1;               // active dodge direction (0 front/1 back/2 left/3 right; -1 = none)
    bool  inAction = false;            // an action sequence (jump/fall/dodge) is latched
    anim::ActorState actionState = anim::STATE_NORMAL;  // which action state is latched
    anim::ActorStateMachine actorStateMachine; // CStateMachine-compatible transition core
    int lastAudioState = -1;
    std::string prevClip;               // outgoing clip during a cross-fade
    unsigned prevAnimStart = 0;         // when the outgoing clip started
    unsigned blendStart = 0;            // cross-fade start tick
    float blendDur = 0.2f;              // cross-fade duration (s) for the current transition
    float animClock = 0.f;             // current clip playback phase (ms); speed-scaled for locomotion
    float prevAnimClock = 0.f;         // outgoing clip phase during a cross-fade
    // Leg cadence: rate = moveSpeed/this. Was tuned by ear to 300 against the
    // old WALK=1320 guess; game_tempos.x7's real WALK=1100 changes that ratio,
    // so this needs a quick re-check with , / . rather than trusting the old
    // number verbatim.
    // GAMETEMPO_actor_default_animation_move_speed from game_tempos.x7.
    float animRefSpeed = 915.f;
    bool  loaded = false;
    void ScanMaps();                    // fill `maps` from Resources/MapInfo/bginfo-*.ini
    void LoadMap(int index);            // (re)load map `index`: meshes, collision, spawn
    void PlayMapMusic(int index);
    void OnEnter()  override;
    void OnUpdate() override;
    void OnRender(IRenderer*) override;
};

// After the intro: connect to the auth server over ProudNet.
// Original: "TitleState_ConnectAuthServer::EnterState/LeaveState".
struct TitleState_ConnectAuthServer : IScene {
    proud::Client conn;
    void OnEnter()  override;   // start connect
    void OnUpdate() override;   // pump handshake
    void OnLeave()  override;   // disconnect
};

// Intro -> Title transition: pending the real GameState-machine plumbing.
