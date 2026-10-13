// Addresses and layouts for th06nc.exe ver 1.03 (link timestamp 2026-09-07).
// Everything is an RVA from the exe's image base; see re/symbols.txt for how
// each one was identified.
#pragma once
#include <stdint.h>

#define TH06NC_TIMESTAMP  0x6A9E3A91u
#define TH06NC_IMAGE_SIZE 0xC6B000u

// --- functions ---------------------------------------------------------------
#define RVA_GetInput                 0x12BE0
#define RVA_EclManager_RunEcl        0x22060
#define RVA_Player_RegisterChain     0x67DC0
#define RVA_Player_AddedCallback     0x68160
#define RVA_Player_OnUpdate          0x68820
#define RVA_Player_CalcDamageToEnemy 0x69D00
#define RVA_Player_OnDrawHighPrio    0x6A130
#define RVA_Player_OnDrawLowPrio     0x6A210
#define RVA_Player_OnDraw3           0x6A430
#define RVA_Player_BombOverlap       0x6A8C0
#define RVA_Player_CalcKillBox       0x6A980
#define RVA_Player_CalcLaserHitbox   0x6ABA0

#define RVA_AnmManager_LoadAnm       0x20B0 // (unused, int slot, const char *path, int index_offset) -> 0 ok

// --- patch sites -------------------------------------------------------------
// `call rel32` instructions
#define SITE_GetInput          0x79D32 // Supervisor_OnUpdate
#define SITE_RegisterChain     0x3B1B1 // GameManager_AddedCallback
#define SITE_RunEcl            0x377AE // EnemyManager_OnUpdate, once per enemy
#define SITE_KillBox_Enemy     0x37A6C // EnemyManager_OnUpdate, enemy body
#define SITE_DamageToEnemy     0x37AB0 // EnemyManager_OnUpdate
#define SITE_BombOverlap       0x1113B // BulletManager_OnUpdate
#define SITE_KillBox_Bullet    0x11418 // BulletManager_OnUpdate
#define SITE_LaserHitbox       0x11883 // BulletManager_OnUpdate
// `lea reg, [rip+rel32]` instructions in Player_RegisterChain that load the
// chain callbacks
#define SITE_Lea_OnUpdate       0x67E1E
#define SITE_Lea_OnDrawHighPrio 0x67E6C
#define SITE_Lea_OnDrawLowPrio  0x67EBA
#define SITE_Lea_OnDraw3        0x67F08

// --- globals -----------------------------------------------------------------
#define RVA_g_Player          0x4FF3A0
#define RVA_g_CurFrameInput   0xA6EC60 // u32
#define RVA_g_LastFrameInput  0xA6EC64 // u32
#define RVA_g_SupervisorState 0xC21D9C // u32, the state the supervisor is switching to
#define RVA_g_SupervisorCurState 0xC21D98 // u32; 1 = main menu, 2 = in game
#define RVA_g_CfgLives        0xC21DB4 // u8, starting lives option (th06.cfg +0x10)
#define RVA_g_CfgBombs        0xC21DB5 // u8, starting bombs option
#define RVA_g_RngCount        0xA6EC40 // u32, numbers drawn so far
#define RVA_g_RngSeed         0xA6EC44 // u16
#define RVA_g_Stage           0x4F1E84 // i32
#define RVA_g_Difficulty      0x4F27C0 // i32
// DxLib's "a second copy may start" setting (SetDoubleStartValidFlag). At 0,
// its window setup finds the first copy's window and makes DxLib_Init fail.
// (Its "keep running while inactive" setting, 0x562F5C, the game sets itself.)
#define RVA_g_AllowSecondInstance 0x561AF4 // i32
// u8[256], built by GetInput: key code (DirectInput DIK numbering) -> index
// into the keyboard state buffer. Identity except for layout remapping. The
// game binds two layouts to P1: arrows + Z/X/LShift and WASD + J/K/L(/Space).
#define RVA_g_KeyTable        0xC21E60
#define RVA_g_IsInReplay      0x4F278C // u8
// Player_AddedCallback picks speeds, fire and bomb functions from these two;
// Player_OnUpdate reads them again (Marisa B cannot fire during her bomb).
#define RVA_g_Character       0x4F1E80 // u8: 0 Reimu, 1 Marisa
#define RVA_g_ShotType        0x4F1E81 // u8: 0 = A, 1 = B
#define RVA_g_Score           0x4F2798 // u64
#define RVA_g_GrazeInStage    0x4FF0CC // i32, capped at 99999
#define RVA_g_GrazeTotal      0x4FF0D0 // i32, capped at 999999
#define RVA_g_GuiFlags        0xA6EC18 // u32
#define RVA_GrazeMargin       0x30CECC // const float 20.0
#define RVA_g_Power           0x4F1E88 // u16
#define RVA_g_Lives           0x4FF0F0 // i8
#define RVA_g_Bombs           0x4FF0F1 // i8

// --- Player ------------------------------------------------------------------
#define PLAYER_SIZE          0xA2B8
#define P_POS                0x7730 // float[3]
#define P_RESPAWN_TIMER      0x773C // i32
#define P_LAST_ENEMY_HIT     0x7740 // float[3], homing target written by EnemyManager
#define P_HIT_RADIUS         0x774C // float
#define P_INVULN_TIMER_PREV  0x7854 // i32, -999 when reset
#define P_INVULN_TIMER       0x7858 // i32
#define P_STATE              0x7898 // u8
#define P_CHAIN_ELEMS        0x78A8 // ChainElem*[4]
#define P_SPRITE_COLOR       0x79B4 // u32 ARGB
#define P_BOMB_IN_USE        0x9EC8 // u8
#define P_GRAB_TOP_LEFT      0xA29C // float[3], item pickup box, refreshed by Player_OnUpdate
#define P_GRAB_BOTTOM_RIGHT  0xA2A8 // float[3]

enum {
    PLAYER_STATE_ALIVE = 0,
    PLAYER_STATE_SPAWNING = 1,
    PLAYER_STATE_DEAD = 2,
    PLAYER_STATE_INVULNERABLE = 3,
};

#define PLAYER_SPAWN_X 192.0f

// --- input bits --------------------------------------------------------------
enum {
    IN_SHOOT = 0x01,
    IN_BOMB = 0x02,
    IN_FOCUS = 0x04,
    IN_MENU = 0x08,
    IN_UP = 0x10,
    IN_DOWN = 0x20,
    IN_LEFT = 0x40,
    IN_RIGHT = 0x80,
    // The bits Player_OnUpdate looks at.
    IN_GAMEPLAY = IN_SHOOT | IN_BOMB | IN_FOCUS | IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT,
};

#define ENEMY_STRIDE 0x10B0

// --- AnmManager (sprite sheets) ------------------------------------------------
// An .anm file is loaded into a slot with an index offset; its sprites and
// scripts land at offset + their id in the manager's tables. A running script
// picks sprites through sprite_offsets[script index], so pointing a range of
// the scripts/sprite_offsets tables at another file re-skins every AnmVm
// created from that range.
#define RVA_g_AnmManager        0xA6E9B0 // AnmManager *
#define MGR_FILES               0x8      // void *[128], loaded file per slot
#define MGR_SPRITE_OFFSETS      0x20F90  // u32[2048], indexed by script index
#define MGR_SCRIPTS             0x23090  // void *[2048]
// Both characters' sheets (player00.anm = Reimu, player01.anm = Marisa) go to
// slot 5 at 0x420, so only one fits. Faces start at 0x4C0.
#define ANM_SLOT_PLAYER         5
#define ANM_OFFSET_PLAYER       0x420
#define ANM_PLAYER_RANGE        0xA0
// Where this mod loads the other character. Slot 100 and indices
// 0x520-0x5BF are unused by the game during a stage.
#define ANM_SLOT_ALT_PLAYER     100
#define ANM_OFFSET_ALT_PLAYER   0x520

// --- enemy bullets (g_BulletManager.bullets) -----------------------------------
#define RVA_g_Bullets      0x3EC2A8
#define BULLET_STRIDE      0x620
#define BULLET_COUNT       640
#define B_POS              0x30  // float[3]
#define B_STATE            0x44  // u16
#define B_SIZE             0x5F4 // float[2] hitbox width, height
#define B_IS_GRAZED        0x618 // u8; the kill box is only tested once this is set
#define BULLET_STATE_FIRED 1

// --- items (g_ItemManager.items) -----------------------------------------------
#define RVA_g_Items        0xBAF0D8
#define ITEM_STRIDE        0x160
#define ITEM_COUNT         1024
#define I_IN_USE           0x00     // u8
#define I_STATE            0x0C     // u8
#define I_POS              0x10     // float[3]
#define ITEM_STATE_HOMING  1        // flying to g_Player
#define ITEM_HALF_SIZE     8.0f
#define RVA_PocY           0x30CF84 // const float 128.0: above this line every item homes in
