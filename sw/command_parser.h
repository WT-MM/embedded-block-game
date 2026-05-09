#ifndef COMMAND_PARSER_H
#define COMMAND_PARSER_H

#include <stdbool.h>

#include "inventory.h"

#define GAME_COMMAND_ERROR_MAX 96

typedef enum {
    GAME_COMMAND_PARSE_OK = 0,
    GAME_COMMAND_PARSE_NOT_COMMAND,
    GAME_COMMAND_PARSE_EMPTY,
    GAME_COMMAND_PARSE_UNKNOWN_COMMAND,
    GAME_COMMAND_PARSE_BAD_SYNTAX,
    GAME_COMMAND_PARSE_BAD_VALUE,
} GameCommandParseStatus;

typedef enum {
    GAME_COMMAND_KIND_NONE = 0,
    GAME_COMMAND_KIND_TIME,
    GAME_COMMAND_KIND_GAMEMODE,
    GAME_COMMAND_KIND_KILL,
    GAME_COMMAND_KIND_HELP,
    GAME_COMMAND_KIND_PHYSICS,
    GAME_COMMAND_KIND_SETBLOCK,
    GAME_COMMAND_KIND_FILL,
    GAME_COMMAND_KIND_GIVE,
} GameCommandKind;

typedef enum {
    GAME_COMMAND_ACTION_NONE = 0,
    GAME_COMMAND_ACTION_SET,
    GAME_COMMAND_ACTION_RESET,
} GameCommandAction;

typedef enum {
    GAME_COMMAND_TIME_NONE = 0,
    GAME_COMMAND_TIME_DAY,
    GAME_COMMAND_TIME_NIGHT,
} GameCommandTimeValue;

typedef enum {
    GAME_COMMAND_GAMEMODE_NONE = 0,
    GAME_COMMAND_GAMEMODE_SURVIVAL,
    GAME_COMMAND_GAMEMODE_CREATIVE,
    GAME_COMMAND_GAMEMODE_SPECTATOR,
} GameCommandGameModeValue;

typedef enum {
    GAME_COMMAND_PHYSICS_NONE = 0,
    GAME_COMMAND_PHYSICS_GRAVITY,
    GAME_COMMAND_PHYSICS_PLAYER_SPEED,
    GAME_COMMAND_PHYSICS_SPRINT_MULTIPLIER,
    GAME_COMMAND_PHYSICS_JUMP_VELOCITY,
    GAME_COMMAND_PHYSICS_JUMP_HEIGHT,
    GAME_COMMAND_PHYSICS_FLY_SPEED,
} GameCommandPhysicsProperty;

typedef struct {
    bool relative;
    int value;
} GameCommandCoord;

typedef struct {
    GameCommandKind kind;
    GameCommandAction action;
    union {
        GameCommandTimeValue time;
        GameCommandGameModeValue gamemode;
        struct {
            GameCommandPhysicsProperty property;
            float value;
        } physics;
        struct {
            GameCommandCoord x;
            GameCommandCoord y;
            GameCommandCoord z;
            BlockID block;
        } setblock;
        struct {
            GameCommandCoord x1;
            GameCommandCoord y1;
            GameCommandCoord z1;
            GameCommandCoord x2;
            GameCommandCoord y2;
            GameCommandCoord z2;
            BlockID block;
        } fill;
        struct {
            ItemID item;
            int count;
        } give;
    } value;
} GameCommandAst;

typedef struct {
    GameCommandParseStatus status;
    GameCommandAst ast;
    char error[GAME_COMMAND_ERROR_MAX];
} GameCommandParseResult;

GameCommandParseStatus game_command_parse(const char *line,
                                          GameCommandParseResult *out);

const char *game_command_parse_status_name(GameCommandParseStatus status);
const char *game_command_time_value_name(GameCommandTimeValue value);
const char *game_command_gamemode_value_name(GameCommandGameModeValue value);
const char *game_command_physics_property_name(GameCommandPhysicsProperty value);

#endif
