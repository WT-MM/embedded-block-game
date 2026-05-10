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
#define COMMAND_GIVE_MAX_COUNT 4096
#define COMMAND_ITEMS_MAX_PAGE 999
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    char text[COMMAND_TOKEN_MAX];
    bool truncated;
} CommandToken;

typedef struct {
    char text[COMMAND_TOKEN_MAX];
    int start;
    int end;
    bool truncated;
} CommandCompleteToken;

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

    if (*text == '\0')
        return false;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno == ERANGE || *end != '\0' ||
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

    if (token->text[0] == '\0')
        return false;

    errno = 0;
    value = strtof(token->text, &end);
    if (errno == ERANGE || *end != '\0' ||
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
    { "redstone_dust", BLOCK_REDSTONE_WIRE_UNCONNECTED },
    { "redstone_wire_unconnected", BLOCK_REDSTONE_WIRE_UNCONNECTED },
    { "redstone_unconnected", BLOCK_REDSTONE_WIRE_UNCONNECTED },
    { "redstone_wire_off", BLOCK_REDSTONE_WIRE_OFF },
    { "redstone_connected_off", BLOCK_REDSTONE_WIRE_OFF },
    { "redstone_wire_on", BLOCK_REDSTONE_WIRE_ON },
    { "redstone_connected_on", BLOCK_REDSTONE_WIRE_ON },
    { "redstone_torch_off", BLOCK_REDSTONE_TORCH_OFF },
    { "redstone_torch_on", BLOCK_REDSTONE_TORCH_ON },
    { "repeater", BLOCK_REPEATER_OFF },
    { "repeater_off", BLOCK_REPEATER_OFF },
    { "repeater_on", BLOCK_REPEATER_ON },
    { "repeater_north_off", BLOCK_REPEATER_OFF },
    { "repeater_north_on", BLOCK_REPEATER_ON },
    { "repeater_east_off", BLOCK_REPEATER_EAST_OFF },
    { "repeater_south_off", BLOCK_REPEATER_SOUTH_OFF },
    { "repeater_west_off", BLOCK_REPEATER_WEST_OFF },
    { "repeater_east_on", BLOCK_REPEATER_EAST_ON },
    { "repeater_south_on", BLOCK_REPEATER_SOUTH_ON },
    { "repeater_west_on", BLOCK_REPEATER_WEST_ON },
    { "comparator", BLOCK_COMPARATOR_OFF },
    { "comparator_off", BLOCK_COMPARATOR_OFF },
    { "comparator_on", BLOCK_COMPARATOR_ON },
    { "comparator_north_off", BLOCK_COMPARATOR_OFF },
    { "comparator_north_on", BLOCK_COMPARATOR_ON },
    { "comparator_east_off", BLOCK_COMPARATOR_EAST_OFF },
    { "comparator_south_off", BLOCK_COMPARATOR_SOUTH_OFF },
    { "comparator_west_off", BLOCK_COMPARATOR_WEST_OFF },
    { "comparator_east_on", BLOCK_COMPARATOR_EAST_ON },
    { "comparator_south_on", BLOCK_COMPARATOR_SOUTH_ON },
    { "comparator_west_on", BLOCK_COMPARATOR_WEST_ON },
    { "lamp_off", BLOCK_LAMP_OFF },
    { "lamp_on", BLOCK_LAMP },
    { "button", BLOCK_BUTTON },
    { "lever", BLOCK_LEVER_OFF },
    { "lever_off", BLOCK_LEVER_OFF },
    { "lever_on", BLOCK_LEVER_ON },
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
    { "furnace", BLOCK_FURNACE },
    { "torch", BLOCK_TORCH },
};

static bool parse_block_value(const CommandToken *token, BlockID *out)
{
    const char *text;
    long id;

    text = token->text;
    if (strncmp(text, "minecraft:", 10) == 0)
        text += 10;

    if (parse_long_text(text, BLOCK_AIR, NUM_BLOCK_TYPES - 1, &id)) {
        *out = (BlockID)id;
        return true;
    }

    for (size_t i = 0; i < ARRAY_COUNT(COMMAND_BLOCK_NAMES); i++) {
        if (block_token_name_equals(text, COMMAND_BLOCK_NAMES[i].name)) {
            *out = COMMAND_BLOCK_NAMES[i].block;
            return true;
        }
    }

    return false;
}

typedef struct {
    const char *name;
    ItemID item;
} CommandItemName;

static const CommandItemName COMMAND_ITEM_NAMES[] = {
    { "stick", ITEM_STICK },
    { "sticks", ITEM_STICK },
    { "apple", ITEM_APPLE },
    { "apples", ITEM_APPLE },
    { "red_mushroom", ITEM_RED_MUSHROOM },
    { "brown_mushroom", ITEM_BROWN_MUSHROOM },
    { "bowl", ITEM_BOWL },
    { "bowls", ITEM_BOWL },
    { "mushroom_stew", ITEM_MUSHROOM_STEW },
    { "stew", ITEM_MUSHROOM_STEW },
    { "coal", ITEM_COAL },
    { "iron_ingot", ITEM_IRON_INGOT },
    { "gold_ingot", ITEM_GOLD_INGOT },
    { "diamond", ITEM_DIAMOND },
    { "bucket", ITEM_BUCKET },
    { "buckets", ITEM_BUCKET },
    { "water_bucket", ITEM_WATER_BUCKET },
    { "lava_bucket", ITEM_LAVA_BUCKET },
    { "wood_pickaxe", ITEM_WOOD_PICKAXE },
    { "wooden_pickaxe", ITEM_WOOD_PICKAXE },
    { "stone_pickaxe", ITEM_STONE_PICKAXE },
    { "iron_pickaxe", ITEM_IRON_PICKAXE },
    { "gold_pickaxe", ITEM_GOLD_PICKAXE },
    { "diamond_pickaxe", ITEM_DIAMOND_PICKAXE },
    { "wood_axe", ITEM_WOOD_AXE },
    { "wooden_axe", ITEM_WOOD_AXE },
    { "stone_axe", ITEM_STONE_AXE },
    { "iron_axe", ITEM_IRON_AXE },
    { "gold_axe", ITEM_GOLD_AXE },
    { "diamond_axe", ITEM_DIAMOND_AXE },
};

static bool parse_item_value(const CommandToken *token, ItemID *out)
{
    BlockID block;
    const char *text;
    long id;

    text = token->text;
    if (strncmp(text, "minecraft:", 10) == 0)
        text += 10;

    if (parse_long_text(text, 1, NUM_ITEM_TYPES - 1, &id)) {
        *out = (ItemID)id;
        return true;
    }

    if (parse_block_value(token, &block) && block != BLOCK_AIR) {
        *out = (ItemID)block;
        return true;
    }

    for (size_t i = 0; i < ARRAY_COUNT(COMMAND_ITEM_NAMES); i++) {
        if (block_token_name_equals(text, COMMAND_ITEM_NAMES[i].name)) {
            *out = COMMAND_ITEM_NAMES[i].item;
            return true;
        }
    }

    return false;
}

static bool block_name_is_first_for_list(size_t index)
{
    BlockID block = COMMAND_BLOCK_NAMES[index].block;

    if (block == BLOCK_AIR)
        return false;
    for (size_t i = 0; i < index; i++) {
        if (COMMAND_BLOCK_NAMES[i].block == block)
            return false;
    }
    return true;
}

static bool item_name_conflicts_with_block(const char *name)
{
    for (size_t i = 0; i < ARRAY_COUNT(COMMAND_BLOCK_NAMES); i++) {
        if (COMMAND_BLOCK_NAMES[i].block != BLOCK_AIR &&
            block_token_name_equals(name, COMMAND_BLOCK_NAMES[i].name))
            return true;
    }
    return false;
}

static bool item_name_is_first_for_list(size_t index)
{
    ItemID item = COMMAND_ITEM_NAMES[index].item;
    const char *name = COMMAND_ITEM_NAMES[index].name;

    if (item == ITEM_NONE || item_name_conflicts_with_block(name))
        return false;
    for (size_t i = 0; i < index; i++) {
        if (COMMAND_ITEM_NAMES[i].item == item)
            return false;
    }
    return true;
}

static bool block_name_is_first_for_completion(size_t index)
{
    BlockID block = COMMAND_BLOCK_NAMES[index].block;

    for (size_t i = 0; i < index; i++) {
        if (COMMAND_BLOCK_NAMES[i].block == block)
            return false;
    }
    return true;
}

int game_command_give_name_count(void)
{
    int count = 0;

    for (size_t i = 0; i < ARRAY_COUNT(COMMAND_BLOCK_NAMES); i++) {
        if (block_name_is_first_for_list(i))
            count++;
    }
    for (size_t i = 0; i < ARRAY_COUNT(COMMAND_ITEM_NAMES); i++) {
        if (item_name_is_first_for_list(i))
            count++;
    }

    return count;
}

const char *game_command_give_name_at(int index)
{
    if (index < 0)
        return NULL;

    for (size_t i = 0; i < ARRAY_COUNT(COMMAND_BLOCK_NAMES); i++) {
        if (!block_name_is_first_for_list(i))
            continue;
        if (index == 0)
            return COMMAND_BLOCK_NAMES[i].name;
        index--;
    }
    for (size_t i = 0; i < ARRAY_COUNT(COMMAND_ITEM_NAMES); i++) {
        if (!item_name_is_first_for_list(i))
            continue;
        if (index == 0)
            return COMMAND_ITEM_NAMES[i].name;
        index--;
    }

    return NULL;
}

#define COMMAND_COMPLETE_MATCH_MAX 128

static const char *COMMAND_ROOT_NAMES[] = {
    "time",
    "gamemode",
    "mode",
    "gm",
    "physics",
    "phys",
    "setblock",
    "fill",
    "blocks",
    "block",
    "give",
    "items",
    "itemnames",
    "itemlist",
    "kill",
    "help",
};

static const char *COMMAND_SET_NAME[] = {
    "set",
};

static const char *COMMAND_TIME_NAMES[] = {
    "set",
    "day",
    "night",
};

static const char *COMMAND_TIME_VALUE_NAMES[] = {
    "day",
    "night",
};

static const char *COMMAND_GAMEMODE_NAMES[] = {
    "set",
    "survival",
    "creative",
    "spectator",
};

static const char *COMMAND_GAMEMODE_VALUE_NAMES[] = {
    "survival",
    "creative",
    "spectator",
};

static const char *COMMAND_PHYSICS_ACTION_NAMES[] = {
    "set",
    "reset",
};

static const char *COMMAND_PHYSICS_PROPERTY_NAMES[] = {
    "gravity",
    "player_speed",
    "sprint_multiplier",
    "jump_velocity",
    "jump_height",
    "fly_speed",
};

static const char *COMMAND_GIVE_PREFIX_NAMES[] = {
    "me",
    "player",
    "self",
};

static const char *COMMAND_KILL_TARGET_NAMES[] = {
    "me",
    "self",
};

static int tokenize_completion_line(const char *line,
                                    CommandCompleteToken tokens[COMMAND_MAX_TOKENS])
{
    int count = 0;
    int pos = 0;

    while (line[pos]) {
        int len = 0;

        while (line[pos] && isspace((unsigned char)line[pos]))
            pos++;
        if (!line[pos])
            break;
        if (count >= COMMAND_MAX_TOKENS)
            return -1;

        memset(&tokens[count], 0, sizeof(tokens[count]));
        tokens[count].start = pos;
        while (line[pos] && !isspace((unsigned char)line[pos])) {
            if (len < COMMAND_TOKEN_MAX - 1) {
                tokens[count].text[len++] =
                    (char)tolower((unsigned char)line[pos]);
            } else {
                tokens[count].truncated = true;
            }
            pos++;
        }
        tokens[count].end = pos;
        tokens[count].text[len] = '\0';
        if (tokens[count].truncated)
            return -1;
        count++;
    }

    return count;
}

static bool completion_name_matches_prefix(const char *prefix,
                                           const char *name)
{
    while (*prefix) {
        char pc = (char)tolower((unsigned char)*prefix);
        char nc = *name;

        if (pc == '-')
            pc = '_';
        if (nc == '-')
            nc = '_';
        if (pc != nc)
            return false;

        prefix++;
        name++;
    }

    return true;
}

static bool token_is_command(const CommandCompleteToken *token,
                             const char *name)
{
    return token->text[0] == '/' &&
           strcmp(token->text + 1, name) == 0;
}

static bool command_is_physics(const CommandCompleteToken *token)
{
    return token_is_command(token, "physics") ||
           token_is_command(token, "phys");
}

static bool command_is_gamemode(const CommandCompleteToken *token)
{
    return token_is_command(token, "gamemode") ||
           token_is_command(token, "mode") ||
           token_is_command(token, "gm");
}

static bool command_is_blocks(const CommandCompleteToken *token)
{
    return token_is_command(token, "blocks") ||
           token_is_command(token, "block");
}

static bool token_is_give_target(const CommandCompleteToken *token)
{
    return strcmp(token->text, "me") == 0 ||
           strcmp(token->text, "self") == 0 ||
           strcmp(token->text, "player") == 0;
}

static int add_completion_match(const char **matches, int count,
                                const char *prefix, const char *candidate)
{
    if (count >= COMMAND_COMPLETE_MATCH_MAX)
        return count;
    if (completion_name_matches_prefix(prefix, candidate))
        matches[count++] = candidate;
    return count;
}

static int add_completion_array_matches(const char **matches, int count,
                                        const char *prefix,
                                        const char *const *names,
                                        size_t name_count)
{
    for (size_t i = 0; i < name_count; i++)
        count = add_completion_match(matches, count, prefix, names[i]);

    return count;
}

static int add_block_completion_matches(const char **matches, int count,
                                        const char *prefix)
{
    for (size_t i = 0; i < ARRAY_COUNT(COMMAND_BLOCK_NAMES); i++) {
        if (block_name_is_first_for_completion(i)) {
            count = add_completion_match(matches, count, prefix,
                                         COMMAND_BLOCK_NAMES[i].name);
        }
    }

    return count;
}

static int add_give_completion_matches(const char **matches, int count,
                                       const char *prefix)
{
    int give_count = game_command_give_name_count();

    for (int i = 0; i < give_count; i++) {
        const char *name = game_command_give_name_at(i);

        if (name)
            count = add_completion_match(matches, count, prefix, name);
    }

    return count;
}

static bool complete_with_matches(const char *line, int line_len,
                                  int target_start, int target_end,
                                  const char *namespace_prefix,
                                  const char **matches, int match_count,
                                  int cycle_index, char *out, int out_size)
{
    char replacement[COMMAND_TOKEN_MAX + 16];
    const char *selected;
    int selected_index;
    int replacement_len;
    int after_len;

    if (match_count <= 0 || !out || out_size <= 0)
        return false;

    if (cycle_index < 0)
        cycle_index = 0;
    selected_index = cycle_index % match_count;
    selected = matches[selected_index];

    if (namespace_prefix && namespace_prefix[0]) {
        replacement_len = snprintf(replacement, sizeof(replacement),
                                   "%s%s", namespace_prefix, selected);
    } else {
        replacement_len = snprintf(replacement, sizeof(replacement),
                                   "%s", selected);
    }
    if (replacement_len < 0 ||
        replacement_len >= (int)sizeof(replacement))
        return false;

    after_len = line_len - target_end;
    if (target_start + replacement_len + after_len >= out_size)
        return false;

    memcpy(out, line, (size_t)target_start);
    memcpy(out + target_start, replacement, (size_t)replacement_len);
    memcpy(out + target_start + replacement_len,
           line + target_end, (size_t)after_len);
    out[target_start + replacement_len + after_len] = '\0';
    return true;
}

bool game_command_complete(const char *line, int cycle_index,
                           char *out, int out_size)
{
    CommandCompleteToken tokens[COMMAND_MAX_TOKENS];
    const char *matches[COMMAND_COMPLETE_MATCH_MAX];
    const char *prefix;
    const char *match_prefix;
    const char *namespace_prefix = "";
    int count;
    int context_count;
    int target_start;
    int target_end;
    int line_len;
    int match_count = 0;
    char root_replacements[COMMAND_COMPLETE_MATCH_MAX][COMMAND_TOKEN_MAX];
    bool trailing_space;

    if (!line || !out || out_size <= 0)
        return false;

    line_len = (int)strlen(line);
    count = tokenize_completion_line(line, tokens);
    if (count < 0)
        return false;

    trailing_space = line_len > 0 &&
                     isspace((unsigned char)line[line_len - 1]);
    if (count == 0)
        return false;

    if (trailing_space) {
        prefix = "";
        context_count = count;
        target_start = line_len;
        target_end = line_len;
    } else {
        prefix = tokens[count - 1].text;
        context_count = count - 1;
        target_start = tokens[count - 1].start;
        target_end = tokens[count - 1].end;
    }

    if (context_count == 0) {
        const char *root_prefix = prefix;

        if (root_prefix[0] != '/')
            return false;
        root_prefix++;
        for (size_t i = 0; i < ARRAY_COUNT(COMMAND_ROOT_NAMES); i++) {
            if (completion_name_matches_prefix(root_prefix,
                                               COMMAND_ROOT_NAMES[i])) {
                if (match_count >= COMMAND_COMPLETE_MATCH_MAX)
                    break;
                snprintf(root_replacements[match_count],
                         sizeof(root_replacements[match_count]),
                         "/%s", COMMAND_ROOT_NAMES[i]);
                matches[match_count] = root_replacements[match_count];
                match_count++;
            }
        }
        return complete_with_matches(line, line_len, target_start, target_end,
                                     "", matches, match_count, cycle_index,
                                     out, out_size);
    }

    if (tokens[0].text[0] != '/' || tokens[0].text[1] == '\0')
        return false;

    match_prefix = prefix;
    if (strncmp(match_prefix, "minecraft:", 10) == 0) {
        namespace_prefix = "minecraft:";
        match_prefix += 10;
    }

    if (token_is_command(&tokens[0], "time")) {
        if (context_count == 1) {
            match_count = add_completion_array_matches(
                matches, match_count, match_prefix, COMMAND_TIME_NAMES,
                ARRAY_COUNT(COMMAND_TIME_NAMES));
        } else if (context_count == 2 &&
                   strcmp(tokens[1].text, "set") == 0) {
            match_count = add_completion_array_matches(
                matches, match_count, match_prefix, COMMAND_TIME_VALUE_NAMES,
                ARRAY_COUNT(COMMAND_TIME_VALUE_NAMES));
        }
    } else if (command_is_gamemode(&tokens[0])) {
        if (context_count == 1) {
            match_count = add_completion_array_matches(
                matches, match_count, match_prefix, COMMAND_GAMEMODE_NAMES,
                ARRAY_COUNT(COMMAND_GAMEMODE_NAMES));
        } else if (context_count == 2 &&
                   strcmp(tokens[1].text, "set") == 0) {
            match_count = add_completion_array_matches(
                matches, match_count, match_prefix,
                COMMAND_GAMEMODE_VALUE_NAMES,
                ARRAY_COUNT(COMMAND_GAMEMODE_VALUE_NAMES));
        }
    } else if (command_is_physics(&tokens[0])) {
        if (context_count == 1) {
            match_count = add_completion_array_matches(
                matches, match_count, match_prefix,
                COMMAND_PHYSICS_ACTION_NAMES,
                ARRAY_COUNT(COMMAND_PHYSICS_ACTION_NAMES));
        } else if (context_count == 2 &&
                   strcmp(tokens[1].text, "set") == 0) {
            match_count = add_completion_array_matches(
                matches, match_count, match_prefix,
                COMMAND_PHYSICS_PROPERTY_NAMES,
                ARRAY_COUNT(COMMAND_PHYSICS_PROPERTY_NAMES));
        }
    } else if (token_is_command(&tokens[0], "setblock")) {
        if (context_count == 4)
            match_count = add_block_completion_matches(
                matches, match_count, match_prefix);
    } else if (token_is_command(&tokens[0], "fill")) {
        if (context_count == 7)
            match_count = add_block_completion_matches(
                matches, match_count, match_prefix);
    } else if (command_is_blocks(&tokens[0])) {
        if (context_count == 1) {
            match_count = add_completion_array_matches(
                matches, match_count, match_prefix, COMMAND_SET_NAME,
                ARRAY_COUNT(COMMAND_SET_NAME));
        } else if (context_count >= 2 &&
                   strcmp(tokens[1].text, "set") == 0 &&
                   (context_count == 5 || context_count == 8)) {
            match_count = add_block_completion_matches(
                matches, match_count, match_prefix);
        }
    } else if (token_is_command(&tokens[0], "give")) {
        if (context_count == 1) {
            match_count = add_completion_array_matches(
                matches, match_count, match_prefix,
                COMMAND_GIVE_PREFIX_NAMES,
                ARRAY_COUNT(COMMAND_GIVE_PREFIX_NAMES));
            match_count = add_give_completion_matches(
                matches, match_count, match_prefix);
        } else if (context_count == 2 && token_is_give_target(&tokens[1])) {
            match_count = add_give_completion_matches(
                matches, match_count, match_prefix);
        }
    } else if (token_is_command(&tokens[0], "kill")) {
        if (context_count == 1) {
            match_count = add_completion_array_matches(
                matches, match_count, match_prefix,
                COMMAND_KILL_TARGET_NAMES,
                ARRAY_COUNT(COMMAND_KILL_TARGET_NAMES));
        }
    }

    return complete_with_matches(line, line_len, target_start, target_end,
                                 namespace_prefix, matches, match_count,
                                 cycle_index, out, out_size);
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

static GameCommandParseStatus parse_give_command(
    const CommandToken tokens[COMMAND_MAX_TOKENS],
    int count,
    GameCommandParseResult *result)
{
    int item_index = 1;
    int count_index;
    long parsed_count = 1;
    ItemID item = ITEM_NONE;

    if (count < 2 || count > 4) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                          "usage: /give [me|player] <item> [count]");
        return result->status;
    }

    if (count >= 3 &&
        (token_equals(&tokens[1], "me") ||
         token_equals(&tokens[1], "self") ||
         token_equals(&tokens[1], "player"))) {
        item_index = 2;
    }

    count_index = item_index + 1;
    if (count != count_index && count != count_index + 1) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                          "usage: /give [me|player] <item> [count]");
        return result->status;
    }

    if (!parse_item_value(&tokens[item_index], &item)) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_VALUE,
                          "unknown item");
        return result->status;
    }

    if (count == count_index + 1 &&
        !parse_long_text(tokens[count_index].text, 1,
                         COMMAND_GIVE_MAX_COUNT, &parsed_count)) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_VALUE,
                          "count must be 1..%d", COMMAND_GIVE_MAX_COUNT);
        return result->status;
    }

    result->status = GAME_COMMAND_PARSE_OK;
    result->ast.kind = GAME_COMMAND_KIND_GIVE;
    result->ast.action = GAME_COMMAND_ACTION_SET;
    result->ast.value.give.item = item;
    result->ast.value.give.count = (int)parsed_count;
    return result->status;
}

static GameCommandParseStatus parse_items_command(
    const CommandToken tokens[COMMAND_MAX_TOKENS],
    int count,
    GameCommandParseResult *result)
{
    long page = 1;

    if (count > 2) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_SYNTAX,
                          "usage: /items [page]");
        return result->status;
    }

    if (count == 2 &&
        !parse_long_text(tokens[1].text, 1, COMMAND_ITEMS_MAX_PAGE, &page)) {
        command_set_error(result, GAME_COMMAND_PARSE_BAD_VALUE,
                          "page must be 1..%d", COMMAND_ITEMS_MAX_PAGE);
        return result->status;
    }

    result->status = GAME_COMMAND_PARSE_OK;
    result->ast.kind = GAME_COMMAND_KIND_ITEMS;
    result->ast.action = GAME_COMMAND_ACTION_NONE;
    result->ast.value.items.page = (int)page;
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
    } else if (strcmp(name, "give") == 0) {
        parse_give_command(tokens, count, &result);
    } else if (strcmp(name, "items") == 0 ||
               strcmp(name, "itemnames") == 0 ||
               strcmp(name, "itemlist") == 0) {
        parse_items_command(tokens, count, &result);
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
