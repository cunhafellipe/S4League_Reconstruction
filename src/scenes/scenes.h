// Game states (Game::GameState::*), the concrete IScene implementations.
// Reversed names: IntroState -> TitleState_ConnectAuthServer -> ... (login flow).
#pragma once
#include "world/scene.h"
#include "net/proud_client.h"
#include "gui/xui.h"
#include "scn/scn.h"
#include "seq/seq.h"
#include "anim/anim_controller.h"
#include "anim/actorstates.h"
#include "lua/lua_weapon_fire.h"
#include "world/movement.h"
#include "game/actor_fsm.h"
#include "game/actor_input.h"
#include "seq/seq.h"
#include "seq/seq_player.h"
#include <vector>
#include <string>
#include <array>
#include <map>
#include <unordered_map>
#include <unordered_set>
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
    // [ADDON_SEQUENCE] object_N=x.seq / object_N_Dummy=node: map effects
    // looping on a node of the map's scenes (135 maps).
    std::map<int, std::pair<std::string, std::string>> addonSeqs;   // N -> (seq, dummy)
};

#include "net/mp_client.h"

struct WorldState : IScene {
    std::vector<BgInfo> maps;           // every bginfo-*.ini found (ESC map picker)
    int   mapIdx  = -1;                 // currently loaded entry in `maps`
    bool  mapMenu = false;              // ESC overlay open (also shown when the game starts)
    int   menuPage = 0;                 // 0 = main (online/solo, maps, wardrobe...), 1 = maps, 2 = wardrobe
    int   mainSel = 0;                  // highlighted main-menu row
    int   settingsSel = 0;              // ESC > Settings: highlighted row
    bool  displayApplied = false;       // config.json resolution/display applied once at start
    bool  mpOnline = false;             // chosen in the menu: "Jugar en linea" / "Jugar solo"
    // Wardrobe (like S4Web2's equip panels): every item.x7 costume whose .scn exists,
    // per sex and slot (hair, face, body, leg, hand, foot). Entry 0 = the default 00_<sex>_<slot>.
    // Slots: hair, face, body, leg, hand, foot, acc. file may list several
    // node .scn joined by '|' (accessory bundles), "" = nothing worn.
    // seqs: item.x7 <sequence node_nameN file_nameN> - effects hung from a
    // node of the item's own .scn while it is worn (eye glows, auras...).
    struct CostumeItem { std::string name, file, icon; std::vector<std::pair<std::string, std::string>> seqs; };
    std::vector<CostumeItem> costumeItems[2][8];
    bool  costumeBuilt = false;
    // Inventory page (S4 layout): boxes 0..10 = HAIR FACE ACC PET | TOP LEGS HANDS FEET | WEAPON 1..3.
    int   invBox = 4, invSel[11] = {}, invTop = 0, invHover = -1;
    float invPreviewYaw = 0.f;          // character preview turn (drag with the mouse)
    int   invDragX = -1;
    bool  lmbPrev = false;
    int   scrW = 1280, scrH = 720;      // last render size, for mouse hit tests
    std::map<std::string, std::string> weaponIcons;   // weapon states -> item.x7 icon_image
    // Every weapon item of item.x7 (inventory grid), like the clothes.
    // category: 0 melee 1 rifle 2 heavy 3 sniper 4 sentry 5 thrown 6 special
    // (item_key 2000000 + 10000 * category); -1 = a Lua type with no item.
    struct WeaponItem { std::string name, icon, scn, states; int category = -1; };
    std::vector<WeaponItem> weaponItems;
    int   invWeaponFilter = -1;         // inventory weapon grid: -1 = all, else a category
    std::vector<int> InvWeaponView() const;   // weaponItems indices passing the filter
    struct UiRect { int x = 0, y = 0, w = 0, h = 0;
                    bool Hit(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; } };
    void  InvLayout(UiRect boxes[12], UiRect& panel, UiRect& grid) const;
    int   InvGridCount() const;
    int   InvEquippedIndex(int box) const;
    void  InvEquip(int index);
    void  InvSelectBox(int box);
    bool  MouseClient(int& mx, int& my) const;
    void  BuildCostumeList();
    void  ReloadCostume();              // re-reads the 6 parts after a wardrobe change, saves and sends it
    std::string CostumeString() const;  // {"gender":..,"hair":..,...} sent to the server as 'equip'
    // Other players' looks: parts per distinct equip string, skeleton per sex.
    struct PeerLook { bool female = false; std::vector<scn::Scene> parts; };
    std::map<std::string, PeerLook> peerLooks;
    scn::Scene peerSkel[2];
    bool  peerSkelLoaded[2] = { false, false };
    int   menuSel = 0;                  // highlighted row
    int   menuTop = 0;                  // first visible row (scroll)
    int   bgmIndex = 0;
    // F4: selector de armas. Una entrada por NormalState_<X> del lua, emparejada
    // a un .scn de model/weapon por coincidencia de nombre (igual que el cliente JS).
    struct WeaponEntry { std::string states, scn; };
    std::vector<WeaponEntry> weaponList;
    bool  wpnMenu = false;
    int   wpnSel = 0, wpnTop = 0;
    int   wpnTargetSlot = 0;            // F4 loadout editor: slot (0..2) that Enter fills
    void  BuildWeaponList();
    void  EquipEntry(int index);
    void  LoadCharacterBody(bool asFemale);   // <sex>_bip + 00_<sex>_* parts (F7 toggles)
    bool  female = false;
    std::vector<scn::Scene> map;        // static map meshes (visible geometry)
    int mapSkyIdx = -1;                 // which of `map` is the [SKY] scene (drawn around the eye)
    // Map meshes uploaded once to GPU buffers (rebuilt on the first frame after
    // LoadMap): per model its static mesh and the texture / lightmap per material.
    struct MapMeshCache { int mesh = -1; std::vector<int> tex, lm; std::vector<std::pair<uint32_t, uint32_t>> ranges; };
    std::unordered_map<const scn::Model*, MapMeshCache> mapMeshCache;
    bool mapCacheDirty = true;
    std::vector<CollTri> collTris;      // precomputed world-space collision triangles
    scn::Scene character;               // male_bip skeleton + anims
    std::vector<scn::Scene> bodyParts;  // default male body/leg/hand/foot/face/hair
    float charPos[3] = { 0, 0, 0 };     // character world position
    float charYaw = 0.f;                // character facing = camera yaw (S4: heading follows aim)
    float camYaw = 0.f;                 // camera look yaw (mouse + arrows)
    float camPitch = 0.25f;             // camera pitch (radians)
    float lookYaw = 0.f, lookPitch = 0.f;   // hold Q: free look around the character (camera only), eases back on release
    float camSideCur = 1.f;             // -1 left .. +1 right shoulder, eased toward camSideTarget
    float camSideTarget = 1.f;          // Z toggles the over-the-shoulder side
    float camDist = 500.f;              // third-person distance behind (client depthoffset=500)
    float camDistEff = 0.f;             // actual eye distance this frame (pulled in by map collision)
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
    float dodgeVelX = 0.f, dodgeVelZ = 0.f, dodgeTimeLeft = 0.f;   // world::MovementController dodge state
    float wallJumpDelayLeft = 0.f, wallJumpTimeLeft = 0.f;
    float wallJumpVelX = 0.f, wallJumpVelZ = 0.f;
    float moveWeightEase = 1.f;   // world::MovementController's eased weaponWeightMul, persisted across frames
    float moveVelX = 0.f, moveVelZ = 0.f;   // world::MovementController's eased horizontal velocity, persisted across frames
    unsigned dodgeUntil = 0;            // dodge VELOCITY burst window (dodge_dodge_time 0.225s)
    unsigned dodgeClipUntil = 0;        // dodge CLIP plays fully until here (= clip duration); jump cancels
    std::string dodgeClip;              // current dodge animation clip
    // Game::CUserInput + the actor state machine, reconstructed from the
    // beta client (src/game). They replace the hand-written double-tap /
    // A-then-Space detection and the polled "wanted state" selection.
    game::UserInput userInput;
    game::ActorFsm  fsm;
    bool  fsmBound = false;
    // Set by the FSM's OnEnter for this frame's movement tick.
    bool  fsmStartJump = false;
    int   fsmStartDodge = 0;            // -1 left, +1 right, 0 none
    bool  fsmTryWallJump = false;
    bool  fsmEntered = false;           // a ChangeState happened this frame (anim restart)
    bool  fsmKillRise = false;
    bool  fsmCancelDodge = false;       // left DODGE for another state: stop the dodge slide now
    bool  fsmCancelWallJump = false;    // attack started mid-air: stop the wall-jump flight
    uint32_t fsmNowMs = 0;              // this frame's input/FSM clock (for the [fsm]/[input] log)          // FASTRUN entered while rising: drop the jump's upward speed
    bool  fsmCounterRows = false;
    // CWeapon Draw/Holster state: true = link=0 pieces sit on their
    // idleAttach node (Spine_Dummy / Gun_Dummy) instead of the hand.
    bool  weaponHolstered = false;      // counter-sword guard rows installed on USE_WEAPON2
    anim::ActorState animBaseState = anim::STATE_NORMAL;  // last non-attack state (AnimController input)
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
    anim::ItemLuaRows weaponRows;       // the item's weapon_lua.x7 rows (state -> weapon= / ani=)
    float weaponItemRange = 0.f, weaponItemPower = 0.f;   // _eu_weapon.x7 <ability range / power>
    std::unordered_set<std::string> peerHolsterClips;   // clips of the states that holster the weapon
    std::unordered_map<std::string, std::string> humanAlt;   // SetAniNameForHuman: clip -> female clip
    // Sigma blade shape change: the right click toggles the form when it ends.
    bool weaponChangePending = false;
    std::string weaponNormalAs, weaponRunAs, weaponChangeOnSeq, weaponChangeOffSeq;
    bool humanAltBuilt = false;
    // Each swing's CreateCAttack list from its weapon_*.lua function: hit
    // shape, time, conditions (stun / blow / critical / camera shake).
    std::vector<luaw::WeaponAttack> atkLuaWeak, atkLuaStrong, atkLuaStrong1, weapon2Attacks, jumpAttacks;
    std::vector<std::vector<luaw::WeaponAttack>> atkLuaCombo;   // combo hits 2..5 (left_attack2..5)
    // The swing in progress: its attacks fire at their own fireTime and hit
    // whoever their shape (ray / box / fan / ...) reaches, in actor space.
    const std::vector<luaw::WeaponAttack>* swingAttacks = nullptr;
    unsigned swingStart = 0;
    std::vector<char> swingFired;
    float swingBaseDmg = 0.f;
    void StartSwingAttacks(const std::vector<luaw::WeaponAttack>* atks, unsigned tnow, float baseDmg);
    void ProcessSwingAttacks(unsigned tnow);
    bool AttackShapeHits(const luaw::AttackShape& sh, float px, float py, float pz) const;
    // Reaction to a hit taken (actorstates_gameplay.lua ActorState_Stun /
    // Blow / Down / Standup / Death, DamageState_<weapon>).
    enum ReactKind { REACT_NONE = 0, REACT_FLINCH, REACT_STUN, REACT_BLOW, REACT_DOWN, REACT_STANDUP, REACT_DEATH, REACT_UPPERCUT };
    int reactKind = REACT_NONE, reactPhase = 0;
    unsigned reactStart = 0, reactEnd = 0, reactPhaseStart = 0;
    float reactVX = 0.f, reactVZ = 0.f;
    bool reactLeftGround = false;
    std::string reactFlinchClip;
    void StartReaction(int kind, unsigned tnow, unsigned durMs);
    float pushVX = 0.f, pushVZ = 0.f; unsigned pushStart = 0, pushEnd = 0;   // a hit's AddPushCondition
    void PlayStateFx(const anim::ActorFunc* fn, unsigned tnow);
    // Camera shake (SetCameraShake*: on a landed hit, or the state's own timed one).
    unsigned shakeStart = 0, shakeEnd = 0, shakePendingAt = 0;
    float shakeAmp = 0.f, shakePendingAmp = 0.f;
    void StartShake(const std::vector<float>& f1, unsigned tnow);
    // Nodes of the drawn weapon, character space, this frame (item <sequence>
    // effects hang from them: tag "weaponfx", attachNode = node).
    std::unordered_map<std::string, std::array<float, 16>> weaponNodeChar;
    std::string weaponAttackSeq, weaponAttackOgg;   // item <other_effects attack_seq/attack_ogg> (counter sword)
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
        std::string mountName;          // link=0 mount dummy (empty for link=1 pieces)
        std::vector<std::array<float,16>> pieceWorlds;
        std::string attackBone;
        std::string idleBone;
        std::string idleBoneOverride;
        int sceneIndex = 0;
        // Barrel-down alignment of a gun holstered on the thigh (HolsterBarrelDown):
        // 0 = not computed, 1 = valid, 2 = this weapon has none.
        mutable int holsterState = 0;
        mutable unsigned holsterGen = 0;
        mutable float holsterRot[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    };
    std::vector<WeaponRenderPart> weaponParts;
    // Other players' weapons (mp 'equip' / roster: weapon .scn + its states):
    // the pieces, mount inverse and attach bones built the same way as ours,
    // without the per-skin special cases. Keyed by "file|states".
    std::unordered_map<std::string, std::vector<WeaponRenderPart>> peerWeapons;
    const std::vector<WeaponRenderPart>& PeerWeaponParts(const std::string& file, const std::string& states);
    // Guns (weaponAnim.isGun): hold the left click to fire at the item's
    // rate_of_fire, magazine_capacity rounds, R / empty magazine reloads
    // (weapon_type_reload_info.x7 reload_period).
    float gunRate = 0.f, gunRange = 3000.f;
    int gunMag = 0, gunAmmo = 0;
    unsigned gunReloadMs = 2000, gunNextShotAt = 0, gunReloadEnd = 0, gunFireStart = 0;
    bool gunFiring = false, gunReloading = false, gunRWasDown = false;
    unsigned gunPrevShotAt = 0;              // the shot before - its pose is blended out of on a restart
    unsigned gunLastShotAt = 0;              // game clock (tnow) of the last shot - the fire pose restarts with it
    // The gun's own STATE:InitLua (weapon_lua.x7 left_attack weapon=, e.g.
    // StandardClassicRifle: 260, 40, -40, 250, 0, 666, 1, dry sound):
    // fHeightOffset / fSideOffset0 / fSideOffset1 / fFrontOffset = where the
    // shot leaves the actor, uiAnimPlayTime = the fire clip's length per shot.
    float gunHeight = 0.f, gunSide0 = 0.f, gunSide1 = 0.f, gunFront = 0.f, gunAnimMs = 0.f;
    bool gunHasOrigin = false;
    std::string weaponCrosshair;   // item <resources crosshair_file> (xhair_assaultrifle -> image/weapon/xhair_assaultrifle_1.png)
    std::string gunReloadSound;
    float gunMuzzle[3] = { 0.f, 0.f, 0.f };   // world place of the weapon's fire node, last frame
    bool gunMuzzleValid = false;
    void FireGunShot(unsigned tnow);
    // Ghost trail (state:AddGhostTrail): the weapon's Line01 sampled every
    // frame while the attack is inside [startMs, endMs]; each sample lives
    // the CGhostTrailData LifeTime and the strip is drawn through them.
    struct GhostSample { unsigned t; float a[3], b[3]; };
    std::vector<GhostSample> ghostSamples;
    const anim::ActorGhostTrail* ghostTrail = nullptr;
    unsigned ghostStart = 0;
    // Render-side clock (GetTickCount, like DrawWeaponParts/DrawEffects): the
    // update's tnow is a QPC clock with another origin.
    void StartGhostTrail(const anim::ActorGhostTrail* g) { ghostTrail = g; ghostStart = GetTickCount(); ghostSamples.clear(); }
    // Clones (C key, as many as wanted): copies of us left where we were, our
    // look and weapon, frozen in the pose we had - with the effects that were
    // playing (frozen at that instant) and the weapon trail; targets for our
    // attacks (they react, no damage sent anywhere). V releases them all.
    struct Clone {
        float pos[3] = { 0, 0, 0 }, yaw = 0.f, lift = 0.f, vy = 0.f, vx = 0.f, vz = 0.f;
        unsigned born = 0, reactStart = 0;
        std::string reactClip;
        scn::AnimPose pose, lowerPose; bool lowerActive = false;
        int react = 0;   // 0 held pose, 1 flinch clip, 2 blown (in the air), 3 landing/standup
        // The held pose as GPU meshes (built on the first draw): no skinning per frame.
        struct Baked { int mesh = -1; int tris = 0; int tex = -1; float world[16]; uint32_t flags = 0; };
        std::vector<Baked> baked; bool bakedDone = false;
        // The weapon trail as it was.
        std::vector<GhostSample> ghostSamples; const anim::ActorGhostTrail* ghostTrail = nullptr;
        unsigned ghostStart = 0, ghostAt = 0;
    };
    std::vector<Clone> clones;
    void ReleaseClones(IRenderer* r = nullptr);
    void CloneHit(Clone& c, float dmg, float stunMs, float blowDist, float blowPower, unsigned tnow);
    void UpdateClones(unsigned tnow, float dt);
    void DrawClones(IRenderer* r);
    void DrawClone(IRenderer* r, Clone& c);
    std::vector<int> cloneMeshesToFree;   // GPU meshes of released clones, freed on the next draw
    void DrawGhostTrail(IRenderer* r, unsigned tnow);
    // Online combat: HP (starts at 100), hits sent through the relay server
    // (mpserver 'hit' -> target gets 'hurt'). The server only relays.
    float myHp = 100.f, myHpMax = 100.f;
    float camEyeNow[3] = { 0, 0, 0 }, camAtNow[3] = { 0, 0, 1 };   // this frame's camera (HP bars over heads)
    std::vector<std::array<float, 3>> spawnPoints;   // the map's spawn_pos markers (respawn after dying)
    unsigned hurtFlashUntil = 0;
    void HitPeersInCone(float range, float dmg, float knock);   // melee: in front of the character
    // Bullets: the nearest other player the ray reaches before maxT (the wall).
    bool PeerAlongRay(const float org[3], const float dir[3], float maxT, int& peerId, float& tHit);
    // The gun's rays from its Lua fire function: every Attack's MAKERAY
    // direction (revolver: 4 rays per shot), plus its AddProjectileOffset
    // pattern (smash rifle: one offset per shot, cycling). Local x/y per unit
    // forward, x < 0 = right (script comments: -0.015 "right right").
    struct GunRay { float x = 0.f, y = 0.f; std::vector<float> off; float pushMs = 0.f, pushSpeed = 0.f, pushAccel = 0.f; float fireTime = 0.f; int group = 0; };
    int gunRayGroups = 1;   // distinct fireTimes: 1 = all rays together, N = a burst of N rounds
    std::vector<float> gunRoundOffsets;                       // each round's fireTime, ms from the first
    struct PendingRound { unsigned at; int group; };
    std::vector<PendingRound> gunPendingRounds;               // rounds of the burst still to fire
    int gunForceGroup = -1;                                   // the round FireGunShot is firing now
    // A shot that hits a wall point-blank pushes the shooter back with that
    // attack's AddPushCondition (shotgun 3000 + 7x750, revolver 4x1125), moved
    // like the client's AttackEffect::CPush (beta sub_88BCD0): speed += accel*dt
    // each step, displaced along the push direction, for its time.
    struct SelfPush { float dirX = 0.f, dirZ = 0.f, speed = 0.f, accel = 0.f, msLeft = 0.f; };
    std::vector<SelfPush> selfPushes;
    std::vector<GunRay> gunRays;
    unsigned gunShotIndex = 0;
    struct GunTracer { float a[3], b[3]; unsigned born; };   // bullet streak, muzzle -> impact (no beam .seq)
    // The weapon's tracer .seq (SetGunTracerAttachNode arg 2) CActLightning
    // nodes: a beam from the gun's fire node to where the shot landed.
    struct BeamNode { seq::Node::Beam beam; uint32_t begin = 0, end = 0; uint32_t rgb = 0xFFFFFF; };   // rgb: tint (white for .seq beams)
    std::vector<BeamNode> gunBeamNodes;
    uint32_t gunBeamDuration = 0;
    struct GunBeam { float b[3]; unsigned born; };
    std::vector<GunBeam> gunBeams;
    // Other players' shots (mp 'shot'): their beams start at their muzzle, and
    // use the beam nodes of their own tracer .seq.
    struct PeerBeam { float a[3], b[3]; unsigned born; std::string tracer; };
    std::vector<PeerBeam> peerBeams;
    std::unordered_map<std::string, std::pair<std::vector<BeamNode>, uint32_t>> beamSeqCache;   // tracer .seq -> beam nodes, duration
    const std::pair<std::vector<BeamNode>, uint32_t>& BeamNodesOf(const std::string& seqPath);
    void PlayPeerShot(const mp::Shot& s);
    std::vector<GunTracer> gunTracers;
    std::vector<scn::Scene> weaponWornParts;   // Iron Boots: worn on the feet, skinned like a costume part
    unsigned WornSlotMask() const {   // costume slots the worn weapon replaces
        unsigned m = 0;
        for (const scn::Scene& w : weaponWornParts) if (w.costumeSlot >= 0) m |= 1u << w.costumeSlot;
        return m;
    }
    std::vector<std::string> weaponAdjustBones;
    struct WeaponBoneOverride { std::string weapon; int part = 0; std::string bone; };
    std::vector<WeaponBoneOverride> weaponBoneOverrides;
    bool boneEditMode = false;
    unsigned boneEditPopupUntil = 0;
    bool  weaponOk = false;
    int   weaponSlot = 0;               // 1/2/3 cambian de slot
    int      weakCombo = 0;             // current row in the weak-attack combo chain
    unsigned lastWeakAt = 0;            // last weak click (600ms combo window)
    bool weakQueued = false;            // a click during a combo hit: the next hit follows it
    // One weapon swing at a time: a single clip held for its wind-up + authored
    // duration + recovery delay, all read straight from the weapon's Lua data.
    anim::WeaponAnim weaponAnim;        // resolved in EquipWeaponFile for the current weapon
    // Real per-swing STATE:SetMoveSpeedRate() values, pulled once at equip time
    // by actually running the weapon's own Weapon_*.lua (see EquipWeaponFile).
    // Stay at 1.0 (no penalty) for any attack whose real Lua function name
    // isn't found under the StandardXStandAttack_Weak/Strong/Strong1 convention.
    float atkMoveSpeedRateWeak = 1.f, atkMoveSpeedRateStrong = 1.f, atkMoveSpeedRateStrong1 = 1.f;
    // STATE:SetMoveBoxMultiplier of each swing: the attack's own animated
    // MoveBox delta is scaled by this (CAttackState::Tick, sub_896290).
    float atkMoveBoxWeak = 1.f, atkMoveBoxStrong = 1.f, atkMoveBoxStrong1 = 1.f;
    // STATE:InitLua(normalMs, delayMs, ...) of each swing: the attack state
    // lasts normal + delay, and ApplyAnim fits the clip to normalMs. 0 = the
    // swing's Lua was not found (falls back to the clip's own length).
    float atkNormalMsWeak = 0.f, atkNormalMsStrong = 0.f, atkNormalMsStrong1 = 0.f;
    bool  atkUseMoveBoxWeak = false, atkUseMoveBoxStrong = false, atkUseMoveBoxStrong1 = false;
    float atkDelayMsWeak = 0.f, atkDelayMsStrong = 0.f, atkDelayMsStrong1 = 0.f;
    // Scene-level fade between animation SOURCES (locomotion controller,
    // a swing, each right-click/jump-attack phase, a swing restarted on the
    // same clip). Same rule as CAnimationController1::SetAnimation: the
    // outgoing pose fades over (its blendOut + the new row's blendIn) ticks,
    // ticks = ms * 4.8 (sub_731AC0). A change in the middle of a fade starts
    // from the pose on screen.
    std::string srcKey;
    int srcBlendOut = 0;
    bool srcFadeLower = true;   // false: attack -> attack, the legs keep their own layer
    // Legs during a grounded swing (the state's lowerPartStateSet). Own clock
    // and own cross-fade: the walk rows are RESET_FALSE, so the leg cycle runs
    // on across swings instead of restarting with each one.
    std::string atkLegClip, atkLegPrevClip;
    float atkLegClock = 0.f, atkLegPrevClock = 0.f;
    float atkLegFadeTicks = 0.f, atkLegFadeElapsed = 0.f;
    int atkLegBlendOut = 0;
    bool atkLegLoop = true, atkLegPrevLoop = true;
    unsigned atkLegLastMs = 0, atkLegAttackStart = 0;
    bool atkLegCarried = false;
    bool srcFadeActive = false;
    unsigned srcFadeStart = 0;
    float srcFadeTicks = 0.f;
    scn::Pose srcFadeUp, srcFadeLow;
    scn::Pose shownPose, shownLower;
    bool shownLowerActive = false;
    // SetDrop's horizontal part, rotated to world when the drop fires; carried
    // until landing (the CS jump attack flies forward-down at -6000 Z).
    float weapon2DropVelX = 0.f, weapon2DropVelZ = 0.f;
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
    bool weapon2PeerHitDone = false;     // online: this right-click/jump action already hit-tested the peers
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
    // STATE:AddSequence entries of each swing's Weapon_*.lua function, and the
    // ones scheduled for the swing in progress (effect + its sound, fired
    // startTime ms after the swing starts; dropped if the swing is cut).
    std::vector<luaw::WeaponFireState::Sequence> atkSeqWeak, atkSeqStrong, atkSeqStrong1;
    struct PendingSeq { unsigned atMs; std::string file, sound; int index = 0; std::string node; };   // index: AddSequence's first arg; node: AddInstantEffect
    std::vector<PendingSeq> pendingSeqs;
    luaw::WeaponFireState jumpAttackLua;

    // A single .seq effect currently playing at some world position (a swing's
    // particle burst, a muzzle flash, ...).
    struct EffectNode { seq::ParticlePlayer player; std::vector<seq::TexRef> textures; bool additive = false; };
    // A mesh a node loader hangs off the effect (the actual slash-arc/flash
    // model some effects use alongside their sparks). No transform animation
    // yet - it sits at the effect's spawn point for its whole lifetime.
    struct EffectModel { scn::Scene model; uint32_t begin = 0, end = 0, fadeIn = 0, fadeOut = 0; bool loop = false; std::string anim; };
    struct ActiveEffect {
        std::vector<EffectNode> nodes;
        std::vector<EffectModel> models;
        uint32_t duration = 0;
        float x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
        float localScale = 1.f, playbackRate = 1.f;
        unsigned startTick = 0;
        // Hung off the actor's scene node (sub_7C50D0(seq, "", actorScene)):
        // follows the actor's position and facing every frame.
        bool followActor = true;
        float offX = 0.f, offZ = 0.f;   // effect-local shift (centres an AddSequence index-2 effect on the actor)
        // Effect owned by a state (fastrun trail): restarted while the state
        // lasts and removed when it ends. Empty = one-shot.
        std::string tag;
        // Costume effect: follows this node of a worn part (item.x7 <sequence>).
        std::string attachNode;
        std::string attachFallback;     // skeleton bone when the node is in neither the part nor the skeleton
        // Recent placements (tick, effect matrix) - an AttachTarget particle
        // is drawn from where the emitter was when it was born.
        std::vector<std::pair<unsigned, std::array<float, 16>>> history;
        // A clone's copy: stays at fixedWorld at frozenAge (never expires).
        bool frozen = false;
        unsigned frozenAge = 0, lastAge = 0;
        std::array<float, 16> fixedWorld = {};
    };
    std::vector<ActiveEffect> activeEffects;
    bool seqEffectsEnabled = true;    // .seq particle/model effects (all 616 S10 .seq files parse)
    void SpawnSeq(const std::string& relPath, float x, float y, float z, float yaw, const std::string& tag = std::string(),
                  float localScale = 1.f, float playbackRate = 1.f, float pitch = 0.f);
    // previewPlace/previewPose: draw only the worn-costume effects on the
    // inventory preview character (no state changes to the live effects).
    void DrawEffects(IRenderer* r, unsigned tnow, const float* previewPlace = nullptr, const scn::AnimPose* previewPose = nullptr);
    void SpawnCostumeEffects();         // (re)starts the worn items' <sequence> effects
    bool CostumeNodeWorld(const std::string& node, float out[16], const scn::AnimPose* pose = nullptr) const;   // character space

    void  EquipWeapon(int slot);
    void  EquipWeaponFile(const char* scnName, const char* states);
    scn::AnimPose curPose;              // pose produced by animCtrl this frame (used by OnRender)
    // Online (server/mpserver.js): config.json "online" {enabled, server, id, nickname}.
    mp::Client mpClient;
    unsigned mpNextTryMs = 0, mpNextSendMs = 0;
    std::string mpWeaponScn;            // .scn of the equipped weapon, sent with equip
    void UpdateOnline(unsigned nowMs, float dt);
    void DrawPeers(IRenderer* r);
    void DrawWeaponParts(IRenderer* r, const scn::AnimPose& pose, const float* place, bool preview, const scn::AnimPose* lowerPose = nullptr);
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
    bool  jumpHadDir = false;          // took off with any direction held (wall-jump eligible)
    int   dodgeDir = -1;               // active dodge direction (0 front/1 back/2 left/3 right; -1 = none)
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
    void ChangeMusic(int dir);
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
