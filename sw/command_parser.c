#include "command_parser.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMMAND_TOKEN_MAX 32
#define COMMAND_MAX_TOKENS 10

typedef struct {
    char text[COMMAND_TOKEN_MAX];
    bool truncated;
} CommandToken;

static void command_result_init(GameCommandParseResult *result)
{
    memset(result, 0, sizeof(*result));
    result->status = GAME_COMMAND_PARSE_NOT_COMMAND;
}

static void command_set_error(GameCommandParseResult *result,
                              GameCommandParseStatus status,
                              const char *fmt, ...)
{
    va_list ap;

    result->status = status;
    va_start(ap, fmt);
    vsnprintf(result->error, sizeof(result->error), fmt, ap);
    va_end(ap);
}

static bool token_equals(const CommandToken *token, const char *text)
{
    return strcmp(token->text, text) == 0;
}

static bool parse_long_text(const char *text, long min_value,
                            long max_value, long *out)
{
    char *end = NULL;
    long value;

    if (!text || *text == '\0')
        return false;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' ||
        value < min_value || value > max_value)
        return false;

    if (out)
        *out = value;
    return true;
}

static bool parse_float_token(const CommandToken *token, float *out)
{
    char *end = NULL;
    float value;

    if (!token || token->text[0] == '\0')
        return false;

    errno = 0;
    value = strtof(token->text, &end);
    if (errno == ERANGE || !end || *end != '\0' ||
        value != value || value < -1000000.0f || value > 1000000.0f)
        return false;

    if (out)
        *out = value;
    return true;
}

static int tokenize_command(const char *line,
                            CommandToken tokens[COMMAND_MAX_TOKENS],
                            GameCommandParseResult *result)
{
    int count = 0;
    const unsigned char *p = (const unsigned char *)line;

    while (*p && isspace(*p))
        p++;
    if (*p == '\0')
        return 0;
    if (*p != '/')
        return 0;

    while (*p) {
        int len = 0;

        while (*p && isspace(*p))
            p++;
        if (*p == '\0')
            break;
        if (count >= COMMAND_MAX_TOKENS) {
            command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                              "too many command words");
            return -1;
        }

        memset(&tokens[count], 0, sizeof(tokens[count]));
        while (*p && !isspace(*p)) {
            if (len < COMMAND_TOKEN_MAX - 1) {
                tokens[count].text[len++] =
                    (char)tolower((unsigned char)*p);
            } else {
                tokens[count].truncated = true;
            }
            p++;
        }
        tokens[count].text[len] = '\0';
        if (tokens[count].truncated) {
            command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                              "command word too long");
            return -1;
        }
        count++;
    }

    return count;
}

static bool parse_time_value(const CommandToken *token,
                             GameCommandTimeValue *out)
{
    if (token_equals(token, "day")) {
        *out = GAME_COMMAND_TIME_DAY;
        return true;
    }
    if (token_equals(token, "night")) {
        *out = GAME_COMMAND_TIME_NIGHT;
        return true;
    }

    return false;
}

static bool parse_gamemode_value(const CommandToken *token,
                                 GameCommandGameModeValue *out)
{
    if (token_equals(token, "0") || token_equals(token, "survival")) {
        *out = GAME_COMMAND_GAMEMODE_SURVIVAL;
        return true;
    }
    if (token_equals(token, "1") || token_equals(token, "creative")) {
        *out = GAME_COMMAND_GAMEMODE_CREATIVE;
        return true;
    }
    if (token_equals(token, "2") || token_equals(token, "spectator")) {
        *out = GAME_COMMAND_GAMEMODE_SPECTATOR;
        return true;
    }

    return false;
}

static bool parse_physics_property(const CommandToken *token,
                                   GameCommandPhysicsProperty *out)
{
    if (token_equals(token, "gravity") || token_equals(token, "g")) {
        *out = GAME_COMMAND_PHYSICS_GRAVITY;
        return true;
    }
    if (token_equals(token, "speed") ||
        token_equals(token, "player_speed") ||
        token_equals(token, "walk_speed") ||
        token_equals(token, "max_speed")) {
        *out = GAME_COMMAND_PHYSICS_PLAYER_SPEED;
        return true;
    }
    if (token_equals(token, "sprint") ||
        token_equals(token, "sprint_multiplier")) {
        *out = GAME_COMMAND_PHYSICS_SPRINT_MULTIPLIER;
        return true;
    }
    if (token_equals(token, "jump") ||
        token_equals(token, "jump_velocity") ||
        token_equals(token, "jump_speed")) {
        *out = GAME_COMMAND_PHYSICS_JUMP_VELOCITY;
        return true;
    }
    if (token_equals(token, "jump_height") ||
        token_equals(token, "jumpheight")) {
        *out = GAME_COMMAND_PHYSICS_JUMP_HEIGHT;
        return true;
    }
    if (token_equals(token, "fly") ||
        token_equals(token, "fly_speed")) {
        *out = GAME_COMMAND_PHYSICS_FLY_SPEED;
        return true;
    }

    return false;
}

static bool parse_coord_value(const CommandToken *token, GameCommandCoord *out)
{
    const char *text;
    long value = 0;

    if (!token || !out)
        return false;

    text = token->text;
    out->relative = false;
    out->value = 0;

    if (text[0] == '~') {
        out->relative = true;
        if (text[1] == '\0')
            return true;
        text++;
    }

    if (!parse_long_text(text, INT_MIN, INT_MAX, &value))
        return false;

    out->value = (int)value;
    return true;
}

static bool block_token_name_equals(const char *token, const char *name)
{
    while (*token && *name) {
        char tc = (*token == '-') ? '_' : *token;
        if (tc != *name)
            return false;
        token++;
        name++;
    }

    return *token == '\0' && *name == '\0';
}

typedef struct {
    const char *name;
    BlockID block;
} CommandBlockName;

static const CommandBlockName COMMAND_BLOCK_NAMES[] = {
    { "air", BLOCK_AIR },
    { "grass", BLOCK_GRASS },
    { "grass_block", BLOCK_GRASS },
    { "wood", BLOCK_WOOD },
    { "log", BLOCK_WOOD },
    { "oak_log", BLOCK_WOOD },
    { "dirt", BLOCK_DIRT },
    { "stone", BLOCK_STONE },
    { "glass", BLOCK_GLASS },
    { "lamp", BLOCK_LAMP },
    { "planks", BLOCK_PLANKS },
    { "oak_planks", BLOCK_PLANKS },
    { "leaves", BLOCK_LEAVES },
    { "oak_leaves", BLOCK_LEAVES },
    { "water", BLOCK_WATER },
    { "water_flow", BLOCK_WATER_FLOW },
    { "flowing_water", BLOCK_WATER_FLOW },
    { "sand", BLOCK_SAND },
    { "gravel", BLOCK_GRAVEL },
    { "cobblestone", BLOCK_COBBLESTONE },
    { "cobble", BLOCK_COBBLESTONE },
    { "bricks", BLOCK_BRICKS },
    { "brick", BLOCK_BRICKS },
    { "obsidian", BLOCK_OBSIDIAN },
    { "sandstone", BLOCK_SANDSTONE },
    { "clay", BLOCK_CLAY },
    { "redstone_block", BLOCK_REDSTONE_BLOCK },
    { "redstone", BLOCK_REDSTONE_BLOCK },
    { "lava", BLOCK_LAVA },
    { "lava_flow", BLOCK_LAVA_FLOW },
    { "flowing_lava", BLOCK_LAVA_FLOW },
    { "coal_ore", BLOCK_COAL_ORE },
    { "iron_ore", BLOCK_IRON_ORE },
    { "gold_ore", BLOCK_GOLD_ORE },
    { "diamond_ore", BLOCK_DIAMOND_ORE },
    { "redstone_ore", BLOCK_REDSTONE_ORE },
    { "gold_block", BLOCK_GOLD_BLOCK },
    { "diamond_block", BLOCK_DIAMOND_BLOCK },
    { "red_flower", BLOCK_RED_FLOWER },
    { "rose", BLOCK_RED_FLOWER },
    { "yellow_flower", BLOCK_YELLOW_FLOWER },
    { "dandelion", BLOCK_YELLOW_FLOWER },
    { "crafting_table", BLOCK_CRAFTING_TABLE },
    { "door", BLOCK_DOOR },
    { "cactus", BLOCK_CACTUS },
    { "red_mushroom", BLOCK_RED_MUSHROOM },
    { "brown_mushroom", BLOCK_BROWN_MUSHROOM },
};

static bool parse_block_value(const CommandToken *token, BlockID *out)
{
    const char *text;
    long id;

    if (!token || !out)
        return false;

    text = token->text;
    if (strncmp(text, "minecraft:", 10) == 0)
        text += 10;

    if (parse_long_text(text, BLOCK_AIR, NUM_BLOCK_TYPES - 1, &id)) {
        *out = (BlockID)id;
        return true;
    }

    for (size_t i = 0; i < sizeof(COMMAND_BLOCK_NAMES) /
                            sizeof(COMMAND_BLOCK_NAMES[0]); i++) {
        if (block_token_name_equals(text, COMMAND_BLOCK_NAMES[i].name)) {
            *out = COMMAND_BLOCK_NAMES[i].block;
            return true;
        }
    }

    return false;
}

static GameCommandParseStatus parse_time_command(
    const CommandToken tokens[COMMAND_MAX_TOKENS],
    int count,
    GameCommandParseResult *result)
{
    int value_index = 1;
    GameCommandTimeValue value = GAME_COMMAND_TIME_NONE;

    if (count == 1 || (count == 2 && token_equals(&tokens[1], "set"))) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                          "usage: /time set <day|night>");
        return result->status;
    }
    if (count == 3 && token_equals(&tokens[1], "set")) {
        value_index = 2;
    } else if (count != 2) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                          "usage: /time set <day|night>");
        return result->status;
    }

    if (!parse_time_value(&tokens[value_index], &value)) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_VALUE,
                          "time must be day or night");
        return result->status;
    }

    result->status = GAME_COMMAND_PARSE_OK;
    result->ast.kind = GAME_COMMAND_KIND_TIME;
    result->ast.action = GAME_COMMAND_ACTION_SET;
    result->ast.value.time = value;
    return result->status;
}

static GameCommandParseStatus parse_gamemode_command(
    const CommandToken tokens[COMMAND_MAX_TOKENS],
    int count,
    GameCommandParseResult *result)
{
    int value_index = 1;
    GameCommandGameModeValue value = GAME_COMMAND_GAMEMODE_NONE;

    if (count == 1 || (count == 2 && token_equals(&tokens[1], "set"))) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                          "usage: /gamemode set <survival|creative|spectator>");
        return result->status;
    }
    if (count == 3 && token_equals(&tokens[1], "set")) {
        value_index = 2;
    } else if (count != 2) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                          "usage: /gamemode set <survival|creative|spectator>");
        return result->status;
    }

    if (!parse_gamemode_value(&tokens[value_index], &value)) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_VALUE,
                          "gamemode must be survival, creative, or spectator");
        return result->status;
    }

    result->status = GAME_COMMAND_PARSE_OK;
    result->ast.kind = GAME_COMMAND_KIND_GAMEMODE;
    result->ast.action = GAME_COMMAND_ACTION_SET;
    result->ast.value.gamemode = value;
    return result->status;
}

static GameCommandParseStatus parse_physics_command(
    const CommandToken tokens[COMMAND_MAX_TOKENS],
    int count,
    GameCommandParseResult *result)
{
    int property_index = 1;
    int value_index = 2;
    GameCommandPhysicsProperty property = GAME_COMMAND_PHYSICS_NONE;
    float value = 0.0f;

    if (count == 2 && token_equals(&tokens[1], "reset")) {
        result->status = GAME_COMMAND_PARSE_OK;
        result->ast.kind = GAME_COMMAND_KIND_PHYSICS;
        result->ast.action = GAME_COMMAND_ACTION_RESET;
        return result->status;
    }

    if (count == 4 && token_equals(&tokens[1], "set")) {
        property_index = 2;
        value_index = 3;
    } else if (count != 3) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                          "usage: /physics set <property> <value>");
        return result->status;
    }

    if (!parse_physics_property(&tokens[property_index], &property)) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_VALUE,
                          "unknown physics property");
        return result->status;
    }
    if (!parse_float_token(&tokens[value_index], &value)) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_VALUE,
                          "physics value must be a number");
        return result->status;
    }

    result->status = GAME_COMMAND_PARSE_OK;
    result->ast.kind = GAME_COMMAND_KIND_PHYSICS;
    result->ast.action = GAME_COMMAND_ACTION_SET;
    result->ast.value.physics.property = property;
    result->ast.value.physics.value = value;
    return result->status;
}

static GameCommandParseStatus parse_setblock_at(
    const CommandToken tokens[COMMAND_MAX_TOKENS],
    int count,
    int coord_index,
    const char *usage,
    GameCommandParseResult *result)
{
    BlockID block = BLOCK_AIR;

    if (count != coord_index + 4) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                          "usage: %s", usage);
        return result->status;
    }

    if (!parse_coord_value(&tokens[coord_index],
                           &result->ast.value.setblock.x) ||
        !parse_coord_value(&tokens[coord_index + 1],
                           &result->ast.value.setblock.y) ||
        !parse_coord_value(&tokens[coord_index + 2],
                           &result->ast.value.setblock.z)) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_VALUE,
                          "coordinates must be integers or ~relative integers");
        return result->status;
    }

    if (!parse_block_value(&tokens[coord_index + 3], &block)) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_VALUE,
                          "unknown block type");
        return result->status;
    }

    result->status = GAME_COMMAND_PARSE_OK;
    result->ast.kind = GAME_COMMAND_KIND_SETBLOCK;
    result->ast.action = GAME_COMMAND_ACTION_SET;
    result->ast.value.setblock.block = block;
    return result->status;
}

static GameCommandParseStatus parse_fill_at(
    const CommandToken tokens[COMMAND_MAX_TOKENS],
    int count,
    int coord_index,
    const char *usage,
    GameCommandParseResult *result)
{
    BlockID block = BLOCK_AIR;

    if (count != coord_index + 7) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                          "usage: %s", usage);
        return result->status;
    }

    if (!parse_coord_value(&tokens[coord_index],
                           &result->ast.value.fill.x1) ||
        !parse_coord_value(&tokens[coord_index + 1],
                           &result->ast.value.fill.y1) ||
        !parse_coord_value(&tokens[coord_index + 2],
                           &result->ast.value.fill.z1) ||
        !parse_coord_value(&tokens[coord_index + 3],
                           &result->ast.value.fill.x2) ||
        !parse_coord_value(&tokens[coord_index + 4],
                           &result->ast.value.fill.y2) ||
        !parse_coord_value(&tokens[coord_index + 5],
                           &result->ast.value.fill.z2)) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_VALUE,
                          "coordinates must be integers or ~relative integers");
        return result->status;
    }

    if (!parse_block_value(&tokens[coord_index + 6], &block)) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_VALUE,
                          "unknown block type");
        return result->status;
    }

    result->status = GAME_COMMAND_PARSE_OK;
    result->ast.kind = GAME_COMMAND_KIND_FILL;
    result->ast.action = GAME_COMMAND_ACTION_SET;
    result->ast.value.fill.block = block;
    return result->status;
}

static GameCommandParseStatus parse_blocks_command(
    const CommandToken tokens[COMMAND_MAX_TOKENS],
    int count,
    GameCommandParseResult *result)
{
    if (count >= 2 && token_equals(&tokens[1], "set")) {
        if (count == 6) {
            return parse_setblock_at(
                tokens, count, 2,
                "/blocks set <x> <y> <z> <block>",
                result);
        }
        return parse_fill_at(
            tokens, count, 2,
            "/blocks set <x1> <y1> <z1> <x2> <y2> <z2> <block>",
            result);
    }

    command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                      "usage: /blocks set <coords> <block>");
    return result->status;
}

static GameCommandParseStatus parse_kill_command(
    const CommandToken tokens[COMMAND_MAX_TOKENS],
    int count,
    GameCommandParseResult *result)
{
    if (count > 2 ||
        (count == 2 &&
         !token_equals(&tokens[1], "self") &&
         !token_equals(&tokens[1], "me"))) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                          "usage: /kill");
        return result->status;
    }

    result->status = GAME_COMMAND_PARSE_OK;
    result->ast.kind = GAME_COMMAND_KIND_KILL;
    result->ast.action = GAME_COMMAND_ACTION_NONE;
    return result->status;
}

GameCommandParseStatus game_command_parse(const char *line,
                                          GameCommandParseResult *out)
{
    GameCommandParseResult result;
    CommandToken tokens[COMMAND_MAX_TOKENS];
    int count;
    const char *name;

    command_result_init(&result);
    memset(tokens, 0, sizeof(tokens));

    if (!line) {
        if (out)
            *out = result;
        return result.status;
    }

    count = tokenize_command(line, tokens, &result);
    if (count < 0) {
        if (out)
            *out = result;
        return result.status;
    }
    if (count == 0) {
        if (out)
            *out = result;
        return result.status;
    }

    name = tokens[0].text + 1;
    if (*name == '\0') {
        command_set_error(&result, GAME_COMMAND_PARSE_EMPTY,
                          "empty command");
    } else if (strcmp(name, "time") == 0) {
        parse_time_command(tokens, count, &result);
    } else if (strcmp(name, "gamemode") == 0 ||
               strcmp(name, "mode") == 0 ||
               strcmp(name, "gm") == 0) {
        parse_gamemode_command(tokens, count, &result);
    } else if (strcmp(name, "physics") == 0 ||
               strcmp(name, "phys") == 0) {
        parse_physics_command(tokens, count, &result);
    } else if (strcmp(name, "setblock") == 0) {
        parse_setblock_at(tokens, count, 1,
                          "/setblock <x> <y> <z> <block>",
                          &result);
    } else if (strcmp(name, "fill") == 0) {
        parse_fill_at(tokens, count, 1,
                      "/fill <x1> <y1> <z1> <x2> <y2> <z2> <block>",
                      &result);
    } else if (strcmp(name, "blocks") == 0 ||
               strcmp(name, "block") == 0) {
        parse_blocks_command(tokens, count, &result);
    } else if (strcmp(name, "kill") == 0) {
        parse_kill_command(tokens, count, &result);
    } else if (strcmp(name, "help") == 0) {
        if (count == 1) {
            result.status = GAME_COMMAND_PARSE_OK;
            result.ast.kind = GAME_COMMAND_KIND_HELP;
        } else {
            command_set_error(&result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                              "usage: /help");
        }
    } else {
        command_set_error(&result, GAME_COMMAND_PARSE_UNKNOWN_COMMAND,
                          "unknown command: /%s", name);
    }

    if (out)
        *out = result;
    return result.status;
}

const char *game_command_parse_status_name(GameCommandParseStatus status)
{
    switch (status) {
    case GAME_COMMAND_PARSE_OK:              return "ok";
    case GAME_COMMAND_PARSE_NOT_COMMAND:     return "not-command";
    case GAME_COMMAND_PARSE_EMPTY:           return "empty";
    case GAME_COMMAND_PARSE_UNKNOWN_COMMAND: return "unknown-command";
    case GAME_COMMAND_PARSE_BAD_SYNTAX:      return "bad-syntax";
    case GAME_COMMAND_PARSE_BAD_VALUE:       return "bad-value";
    default:                                 return "unknown";
    }
}

const char *game_command_time_value_name(GameCommandTimeValue value)
{
    switch (value) {
    case GAME_COMMAND_TIME_DAY:   return "day";
    case GAME_COMMAND_TIME_NIGHT: return "night";
    default:                      return "unknown";
    }
}

const char *game_command_gamemode_value_name(GameCommandGameModeValue value)
{
    switch (value) {
    case GAME_COMMAND_GAMEMODE_SURVIVAL:  return "survival";
    case GAME_COMMAND_GAMEMODE_CREATIVE:  return "creative";
    case GAME_COMMAND_GAMEMODE_SPECTATOR: return "spectator";
    default:                              return "unknown";
    }
}

const char *game_command_physics_property_name(GameCommandPhysicsProperty value)
{
    switch (value) {
    case GAME_COMMAND_PHYSICS_GRAVITY:
        return "gravity";
    case GAME_COMMAND_PHYSICS_PLAYER_SPEED:
        return "player_speed";
    case GAME_COMMAND_PHYSICS_SPRINT_MULTIPLIER:
        return "sprint_multiplier";
    case GAME_COMMAND_PHYSICS_JUMP_VELOCITY:
        return "jump_velocity";
    case GAME_COMMAND_PHYSICS_JUMP_HEIGHT:
        return "jump_height";
    case GAME_COMMAND_PHYSICS_FLY_SPEED:
        return "fly_speed";
    default:
        return "unknown";
    }
}
