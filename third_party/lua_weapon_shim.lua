-- Shim de la API C++ que el juego real inyecta en cada Weapon_*.lua (ver comentarios de
-- weapon_init.lua: MAKEVECTOR3/MAKECVECTOR3/MAKERAY/CreateCAttack/etc). Reimplementado en Lua
-- puro para correr los .lua REALES del juego sin tocarlos ni reescribir su logica (loops, RNG,
-- rotaciones) a mano - eso es justo lo que se pidio evitar.
--
-- No simula colision/fisica real: los MAKE* devuelven descriptores de forma {kind=...} y
-- CreateCAttack/CreateCExplosionAttack acumulan los parametros reales que el .lua les pasa.
-- AddAttack empuja el resultado a ATTACKS (tabla global que runWeapon() lee al final).

-- Los Weapon_*.lua hacen require "Resources/Script/X.lua" (convencion de rutas del juego, no de
-- Lua estandar) - ya se cargan las constantes a mano antes de correr el arma, asi que no hace falta
-- resolver el require real.
function require(name) end

local V3 = {}
V3.__index = V3

local function toRad(deg) return deg * math.pi / 180.0 end

function V3.new(x, y, z) return setmetatable({ x = x or 0, y = y or 0, z = z or 0 }, V3) end
function V3:GetX() return self.x end
function V3:GetY() return self.y end
function V3:GetZ() return self.z end

-- RotateY/RotateZ: rotacion est�ndar mano-izquierda alrededor del eje, en GRADOS (los .lua
-- reales pasan valores como 15.0/90/45 directamente).
function V3:RotateX(deg)   -- weapon_smg4.lua StandardShockWave, weapon_mg2.lua StandardMg2
  local a = toRad(deg); local c, s = math.cos(a), math.sin(a)
  local y, z = self.y, self.z
  self.y = y * c - z * s
  self.z = y * s + z * c
  return self
end
function V3:RotateY(deg)
  local a = toRad(deg); local c, s = math.cos(a), math.sin(a)
  local x, z = self.x, self.z
  self.x = x * c + z * s
  self.z = -x * s + z * c
  return self
end
function V3:RotateZ(deg)
  local a = toRad(deg); local c, s = math.cos(a), math.sin(a)
  local x, y = self.x, self.y
  self.x = x * c - y * s
  self.y = x * s + y * c
  return self
end
function V3:Multiply(mx, my, mz) self.x = self.x * mx; self.y = self.y * my; self.z = self.z * mz; return self end
function V3:Normalize()
  local len = math.sqrt(self.x*self.x + self.y*self.y + self.z*self.z)
  if len > 1e-9 then self.x = self.x/len; self.y = self.y/len; self.z = self.z/len end
  return self
end

function MAKECVECTOR3(x, y, z) return V3.new(x, y, z) end
function MAKEVECTOR3(x, y, z) return { x = x, y = y, z = z } end
function MAKEVECTOR2(x, y) return { x = x, y = y } end
function MAKEVECTOR2Y0(x, y) return { x = x, y = 0, z = y } end

function MAKERAY(center, dir, length) return { kind = 'ray', center = center, dir = dir, length = length } end
function MAKESPHERE(center, radius) return { kind = 'sphere', center = center, radius = radius } end
function MAKEBOX(center, dir, width, height, length) return { kind = 'box', center = center, dir = dir, width = width, height = height, length = length } end
function MAKECYLINDER(center, radius, length) return { kind = 'cylinder', center = center, radius = radius, length = length } end
function MAKEFAN(center, dir, width, height, length, angle) return { kind = 'fan', center = center, dir = dir, width = width, height = height, length = length, angle = angle } end
function MAKEPOLE(center, dir, width, height, length) return { kind = 'pole', center = center, dir = dir, width = width, height = height, length = length } end
function MAKETHICKRAY(center, dir, thickness, length) return { kind = 'thickray', center = center, dir = dir, thickness = thickness, length = length } end
function MAKECONE(center, dir, radius, angle, bottomAngle) return { kind = 'cone', center = center, dir = dir, radius = radius, angle = angle, bottomAngle = bottomAngle } end

-- STATE: solo los metodos que los Weapon_*.lua realmente llaman a nivel de "state" (no de Attack).
STATE = {
  InitLua = function(self, ...) self.lua = { ... } end,
  SetDrop = function(self, time, vector) self.dropTime = time; self.dropVector = vector end,
  SetDelayDrop = function(self, time, vector) self.delayDropTime = time; self.delayDropVector = vector end,
  -- Retain visual commands too; the caller must report unsupported consumers.
  EnableCameraShake = function(self, ...) self.cameraShake = { ... } end,
  SetCameraShakeFactor1 = function(self, ...) self.cameraShakeFactor1 = { ... } end,
  SetCameraShakeFactor2 = function(self, ...) self.cameraShakeFactor2 = { ... } end,
  AddSequence = function(self, index, file, sound, startTime, endTime)
    -- rawget: the lenient metatable hands back a stub function for unknown
    -- fields, so "self.sequences or {}" got that function and table.insert
    -- failed - every swing calling AddSequence before InitLua (plasma jump
    -- attack: its SetDrop/stun) aborted.
    if type(rawget(self, 'sequences')) ~= 'table' then self.sequences = {} end
    table.insert(self.sequences, { index = index, file = file, sound = sound, startTime = startTime, endTime = endTime })
  end,
  SetMoveBoxMultiplier = function(self, v) self.moveBoxMultiplier = v end,
  SetUseMoveBox = function(self, v) self.useMoveBox = v end,
  SetMoveStopWhenDamage = function(self, v) self.moveStopWhenDamage = v end,
  SetMoveSpeedRate = function(self, v) self.moveSpeedRate = v end,
  SetHitStop = function(self, a, b) self.hitStopA = a; self.hitStopB = b end,
  SetStealthCancel = function(self, v) self.stealthCancel = v end,
  SetIncreaseDelayChargeTime = function(self, v) end,
  SetMaxIncreaseDelayTime = function(self, v) end,
  SetEnableMouseSensitivityRotate = function(self, v) end,
  SetNormalMouseSensitivityRotate = function(self, v) end,
  SetMaxAttackCount = function(self, v) end,
  SetStateMinSp = function(self, v) end,
  SetSpendSp = function(self, v) end,
  SetCoolTime = function(self, v) self.coolTime = v end,
}

-- The S10 scripts call more STATE/WEAPON/Attack methods than this shim models
-- (SetCoolTime cut StandardCounterSwordJumpDash off before its SetDrop). An
-- unknown method is a no-op instead of an error, so every script runs to the
-- end and the values that ARE read (InitLua, SetDrop, ...) are always set.
local function noop() end
local lenient = { __index = function(t, k) return noop end }
setmetatable(STATE, lenient)

WEAPON = {
  ApplyRandomDamage = function(self, atk) return atk end,
  GetRange = function(self) return 3000.0 end,   -- melee reach as before (not the 25000 shot ray)
  GetFloat = function(self, index) return 0 end,
}
setmetatable(WEAPON, lenient)
-- ACTORSTATE (the client binds it next to STATE, sub_12BFD40): the original
-- gun scripts call ACTORSTATE:SetGunSpreadType / SetInnerAccuracyVary on it.
ACTORSTATE = setmetatable({}, lenient)

RayDistanceLimit = 25000.0   -- client: dword_256B3A4 (FUN_012bfd40)
-- weapon_init.lua (loaded by the client before every weapon file):
if API_VERSION == nil then API_VERSION = 1 end
ATTACKS = {}

local Attack = {}
Attack.__index = function(t, k) return rawget(Attack, k) or noop end
local function newAttack(explosion)
  return setmetatable({ explosion = explosion, blow = {}, push = {}, stun = {} }, Attack)
end
function CreateCAttack() return newAttack(false) end
function CreateCExplosionAttack() return newAttack(true) end

function Attack:InitAttack1(weaponType, attackAttrib, fireTime, power, additionalPower)
  self.weaponType, self.attackAttrib, self.fireTime, self.power, self.additionalPower =
    weaponType, attackAttrib, fireTime, power, additionalPower
end
function Attack:InitAttack2(shapeOrCollision, a, b, c)
  self.shape = shapeOrCollision; self.a = a; self.b = b; self.c = c
end
function Attack:SetCollisionRangeAdjust(a, b) self.collisionRangeAdjust = { a, b } end
function Attack:SetConsumeAmmo(n) self.consumeAmmo = n end
function Attack:SetGhostCalcCollisionRange(v) self.ghostCalcCollisionRange = v end
function Attack:AddBlowCondition(dist, power) table.insert(self.blow, { dist, power }) end
function Attack:AddPushCondition(t, speed, cond) table.insert(self.push, { t, speed, cond }) end
function Attack:AddStunCondition(minT, maxT, minD, maxD) table.insert(self.stun, { minT, maxT, minD, maxD }) end
function Attack:SetAlwaysCritical(v) self.alwaysCritical = v end
function Attack:SetAttackSuccessCondition(v) self.attackSuccessCondition = v end
function Attack:SetCritical(mult, chance) self.critMult = mult; self.critChance = chance end
function Attack:SetCameraShakeEnable(...) end
function Attack:SetCameraShakeSetFactor1(...) end
function Attack:SetCameraShakeSetFactor2(...) end
function Attack:SetCameraShakeRandomDir(...) end
function Attack:SetCameraShakeWhenSuccess(...) end
function Attack:SetPlaySound(v) self.playSound = v end
-- Shot pattern: one offset per shot, cycling (x, y, z with z = -1 forward).
-- API_VERSION 1 scripts call AddProjectileOffset, 2 AddPatternPoint.
function Attack:AddProjectileOffset(x, y, z)
  if type(rawget(self, 'offsets')) ~= 'table' then self.offsets = {} end
  table.insert(self.offsets, { x = x, y = y, z = z })
end
Attack.AddPatternPoint = Attack.AddProjectileOffset

function AddAttack(list, atk) table.insert(list, atk) end
