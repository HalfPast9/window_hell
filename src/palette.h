// palette.h — visual language, one source of truth (PRD §7.0, LOCKED look).
// All values are the exact locked hex colors, pre-divided to [0,1] floats.
// Tuning any color is a one-line change here.
#ifndef PALETTE_H
#define PALETTE_H

#include "render.h"  // RGBA

static const RGBA PALETTE_VOID          = { 0.039216f, 0.039216f, 0.070588f, 1.0f }; // #0A0A12
static const RGBA PALETTE_PLAYFIELD     = { 0.070588f, 0.070588f, 0.109804f, 1.0f }; // #12121C
static const RGBA PALETTE_BORDER_NORMAL = { 0.309804f, 0.941176f, 1.000000f, 1.0f }; // #4FF0FF
static const RGBA PALETTE_BORDER_DANGER = { 1.000000f, 0.231373f, 0.231373f, 1.0f }; // #FF3B3B
static const RGBA PALETTE_PLAYER        = { 1.0f,       1.0f,       1.0f,      1.0f }; // #FFFFFF
static const RGBA PALETTE_PLAYER_SHOT   = { 0.682353f, 0.960784f, 1.000000f, 1.0f }; // #AEF5FF
static const RGBA PALETTE_ENEMY_BULLET  = { 1.000000f, 0.364706f, 0.635294f, 1.0f }; // #FF5DA2
static const RGBA PALETTE_TRIANGLE      = { 1.000000f, 0.823529f, 0.247059f, 1.0f }; // #FFD23F
static const RGBA PALETTE_CIRCLE        = { 0.356863f, 0.819608f, 1.000000f, 1.0f }; // #5BD1FF
static const RGBA PALETTE_OCTAGON       = { 1.000000f, 0.478431f, 0.776471f, 1.0f }; // #FF7AC6
static const RGBA PALETTE_SPIKER_CORE   = { 1.000000f, 0.364706f, 0.635294f, 1.0f }; // #FF5DA2
static const RGBA PALETTE_SPIKER_POINTS = { 1.0f,       1.0f,       1.0f,      1.0f }; // #FFFFFF
static const RGBA PALETTE_HUD_TEXT      = { 0.486275f, 1.000000f, 0.690196f, 1.0f }; // #7CFFB0

// Ship color picker (COLOR_SELECT, snapshot.h's COLOR_COUNT/player_color).
// Index 0 matches the original single-player PALETTE_PLAYER white, so a
// solo run that never touches the picker still nets the classic look.
// Chosen to stay clear of every enemy/bullet/danger color above.
#define SHIP_COLOR_COUNT 5
static const RGBA PALETTE_SHIP_COLORS[SHIP_COLOR_COUNT] = {
    { 1.000000f, 1.000000f, 1.000000f, 1.0f },  // 0: white  #FFFFFF (classic)
    { 1.000000f, 0.596078f, 0.219608f, 1.0f },  // 1: orange #FF9838
    { 0.298039f, 1.000000f, 0.627451f, 1.0f },  // 2: mint   #4CFFA0
    { 0.725490f, 0.549020f, 1.000000f, 1.0f },  // 3: violet #B98CFF
    { 0.431373f, 0.611765f, 1.000000f, 1.0f },  // 4: peri   #6E9CFF
};

// Enemy-outside-window: tint x 0.30, desaturate (PRD §7.0). Applied at draw
// time in render.c since it's a transform on another entity's color, not a
// fixed value.
#define PALETTE_OUTSIDE_TINT 0.30f

// The boss never stands in the void — its "outside" flag means "windows not
// merged", and it's sitting inside its own lit room. The full void treatment
// (0.30 + desaturate) reads as dead/broken there, so it gets a gentler cue:
// still clearly pink, just cooled off until the rooms merge.
#define PALETTE_BOSS_UNMERGED_TINT 0.55f

#endif // PALETTE_H
