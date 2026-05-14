#include <stdio.h>
#include <string.h>

#include "command_parser.h"

static int check_failed(const char *message)
{
    fprintf(stderr, "command_parser_test: %s\n", message);
    return 1;
}

static int expect_status(const char *line, GameCommandParseStatus expected)
{
    GameCommandParseResult result;
    GameCommandParseStatus actual = game_command_parse(line, &result);

    if (actual != expected) {
        fprintf(stderr,
                "command_parser_test: %s -> %s, expected %s",
                line ? line : "(null)",
                game_command_parse_status_name(actual),
                game_command_parse_status_name(expected));
        if (result.error[0])
            fprintf(stderr, " (%s)", result.error);
        fputc('\n', stderr);
        return 1;
    }

    return 0;
}

static int expect_time(const char *line, GameCommandTimeValue expected)
{
    GameCommandParseResult result;

    if (game_command_parse(line, &result) != GAME_COMMAND_PARSE_OK)
        return check_failed("time command did not parse");
    if (result.ast.kind != GAME_COMMAND_KIND_TIME ||
        result.ast.action != GAME_COMMAND_ACTION_SET ||
        result.ast.value.time != expected)
        return check_failed("time command AST mismatch");

    return 0;
}

static int expect_gamemode(const char *line,
                           GameCommandGameModeValue expected)
{
    GameCommandParseResult result;

    if (game_command_parse(line, &result) != GAME_COMMAND_PARSE_OK)
        return check_failed("gamemode command did not parse");
    if (result.ast.kind != GAME_COMMAND_KIND_GAMEMODE ||
        result.ast.action != GAME_COMMAND_ACTION_SET ||
        result.ast.value.gamemode != expected)
        return check_failed("gamemode command AST mismatch");

    return 0;
}

static int expect_kind(const char *line, GameCommandKind expected)
{
    GameCommandParseResult result;

    if (game_command_parse(line, &result) != GAME_COMMAND_PARSE_OK)
        return check_failed("command did not parse");
    if (result.ast.kind != expected)
        return check_failed("command kind mismatch");

    return 0;
}

static int expect_physics(const char *line,
                          GameCommandPhysicsProperty expected_property,
                          float expected_value)
{
    GameCommandParseResult result;

    if (game_command_parse(line, &result) != GAME_COMMAND_PARSE_OK)
        return check_failed("physics command did not parse");
    if (result.ast.kind != GAME_COMMAND_KIND_PHYSICS ||
        result.ast.action != GAME_COMMAND_ACTION_SET ||
        result.ast.value.physics.property != expected_property)
        return check_failed("physics command AST mismatch");
    if (result.ast.value.physics.value < expected_value - 0.001f ||
        result.ast.value.physics.value > expected_value + 0.001f)
        return check_failed("physics command value mismatch");

    return 0;
}

static int expect_setblock(const char *line, BlockID expected_block)
{
    GameCommandParseResult result;

    if (game_command_parse(line, &result) != GAME_COMMAND_PARSE_OK)
        return check_failed("setblock command did not parse");
    if (result.ast.kind != GAME_COMMAND_KIND_SETBLOCK ||
        result.ast.action != GAME_COMMAND_ACTION_SET ||
        result.ast.value.setblock.block != expected_block)
        return check_failed("setblock command AST mismatch");

    return 0;
}

static int expect_fill(const char *line, BlockID expected_block,
                       bool expected_relative)
{
    GameCommandParseResult result;

    if (game_command_parse(line, &result) != GAME_COMMAND_PARSE_OK)
        return check_failed("fill command did not parse");
    if (result.ast.kind != GAME_COMMAND_KIND_FILL ||
        result.ast.action != GAME_COMMAND_ACTION_SET ||
        result.ast.value.fill.block != expected_block ||
        result.ast.value.fill.x1.relative != expected_relative)
        return check_failed("fill command AST mismatch");

    return 0;
}

static int expect_give(const char *line, ItemID expected_item,
                       int expected_count)
{
    GameCommandParseResult result;

    if (game_command_parse(line, &result) != GAME_COMMAND_PARSE_OK)
        return check_failed("give command did not parse");
    if (result.ast.kind != GAME_COMMAND_KIND_GIVE ||
        result.ast.action != GAME_COMMAND_ACTION_SET ||
        result.ast.value.give.item != expected_item ||
        result.ast.value.give.count != expected_count)
        return check_failed("give command AST mismatch");

    return 0;
}

static int expect_items(const char *line, int expected_page)
{
    GameCommandParseResult result;

    if (game_command_parse(line, &result) != GAME_COMMAND_PARSE_OK)
        return check_failed("items command did not parse");
    if (result.ast.kind != GAME_COMMAND_KIND_ITEMS ||
        result.ast.action != GAME_COMMAND_ACTION_NONE ||
        result.ast.value.items.page != expected_page)
        return check_failed("items command AST mismatch");

    return 0;
}

static int expect_completion(const char *line, int cycle_index,
                             const char *expected)
{
    char completed[64];

    if (!game_command_complete(line, cycle_index,
                               completed, sizeof(completed))) {
        fprintf(stderr,
                "command_parser_test: %s did not complete\n",
                line ? line : "(null)");
        return 1;
    }
    if (strcmp(completed, expected) != 0) {
        fprintf(stderr,
                "command_parser_test: %s completed to %s, expected %s\n",
                line ? line : "(null)", completed, expected);
        return 1;
    }

    return 0;
}

int main(void)
{
    if (expect_status("hello world", GAME_COMMAND_PARSE_NOT_COMMAND))
        return 1;
    if (expect_status("/", GAME_COMMAND_PARSE_EMPTY))
        return 1;
    if (expect_status("/weather set rain",
                      GAME_COMMAND_PARSE_UNKNOWN_COMMAND))
        return 1;
    if (expect_status("/time set noon", GAME_COMMAND_PARSE_BAD_VALUE))
        return 1;
    if (expect_status("/gamemode", GAME_COMMAND_PARSE_BAD_SYNTAX))
        return 1;
    if (expect_status("/kill steve", GAME_COMMAND_PARSE_BAD_SYNTAX))
        return 1;
    if (expect_status("/physics set gravity moon",
                      GAME_COMMAND_PARSE_BAD_VALUE))
        return 1;
    if (expect_status("/fill 0 0 0 1 1 glass",
                      GAME_COMMAND_PARSE_BAD_SYNTAX))
        return 1;
    if (expect_status("/setblock 0 0 0 cheese",
                      GAME_COMMAND_PARSE_BAD_VALUE))
        return 1;
    if (expect_status("/give air", GAME_COMMAND_PARSE_BAD_VALUE))
        return 1;
    if (expect_status("/give coal 0", GAME_COMMAND_PARSE_BAD_VALUE))
        return 1;
    if (expect_status("/items zero", GAME_COMMAND_PARSE_BAD_VALUE))
        return 1;
    if (expect_status("/items 1 extra", GAME_COMMAND_PARSE_BAD_SYNTAX))
        return 1;

    if (expect_time("/time set day", GAME_COMMAND_TIME_DAY))
        return 1;
    if (expect_time("  /TIME night  ", GAME_COMMAND_TIME_NIGHT))
        return 1;
    if (expect_gamemode("/gamemode set creative",
                        GAME_COMMAND_GAMEMODE_CREATIVE))
        return 1;
    if (expect_gamemode("/gm spectator",
                        GAME_COMMAND_GAMEMODE_SPECTATOR))
        return 1;
    if (expect_gamemode("/mode 0",
                        GAME_COMMAND_GAMEMODE_SURVIVAL))
        return 1;
    if (expect_kind("/kill", GAME_COMMAND_KIND_KILL))
        return 1;
    if (expect_kind("/kill self", GAME_COMMAND_KIND_KILL))
        return 1;
    if (expect_kind("/physics reset", GAME_COMMAND_KIND_PHYSICS))
        return 1;
    if (expect_physics("/physics set gravity 12.5",
                       GAME_COMMAND_PHYSICS_GRAVITY, 12.5f))
        return 1;
    if (expect_physics("/phys speed 6",
                       GAME_COMMAND_PHYSICS_PLAYER_SPEED, 6.0f))
        return 1;
    if (expect_physics("/physics set jump_height 2",
                       GAME_COMMAND_PHYSICS_JUMP_HEIGHT, 2.0f))
        return 1;
    if (expect_setblock("/setblock 1 2 3 air", BLOCK_AIR))
        return 1;
    if (expect_setblock("/blocks set ~ ~-1 ~2 minecraft:stone",
                        BLOCK_STONE))
        return 1;
    if (expect_fill("/fill 0 1 2 3 4 5 glass", BLOCK_GLASS, false))
        return 1;
    if (expect_setblock("/setblock 1 2 3 furnace", BLOCK_FURNACE))
        return 1;
    if (expect_setblock("/setblock 1 2 3 torch", BLOCK_TORCH))
        return 1;
    if (expect_setblock("/setblock 1 2 3 redstone_torch_on",
                        BLOCK_REDSTONE_TORCH_ON))
        return 1;
    if (expect_setblock("/setblock 1 2 3 repeater-off", BLOCK_REPEATER_OFF))
        return 1;
    if (expect_setblock("/setblock 1 2 3 comparator-east-on",
                        BLOCK_COMPARATOR_EAST_ON))
        return 1;
    if (expect_setblock("/setblock 1 2 3 button", BLOCK_BUTTON))
        return 1;
    if (expect_setblock("/setblock 1 2 3 pressure_plate",
                        BLOCK_WOOD_PRESSURE_PLATE))
        return 1;
    if (expect_setblock("/setblock 1 2 3 stone_pressure_plate",
                        BLOCK_STONE_PRESSURE_PLATE))
        return 1;
    if (expect_setblock("/setblock 1 2 3 lever", BLOCK_LEVER_OFF))
        return 1;
    if (expect_setblock("/setblock 1 2 3 lever_on", BLOCK_LEVER_ON))
        return 1;
    if (expect_fill("/blocks set ~-1 ~ ~1 ~1 ~2 ~3 diamond-block",
                    BLOCK_DIAMOND_BLOCK, true))
        return 1;
    if (expect_give("/give coal 12", ITEM_COAL, 12))
        return 1;
    if (expect_give("/give me minecraft:stone 64", (ItemID)BLOCK_STONE, 64))
        return 1;
    if (expect_give("/give player mushroom-stew", ITEM_MUSHROOM_STEW, 1))
        return 1;
    if (expect_give("/give lava_bucket", ITEM_LAVA_BUCKET, 1))
        return 1;
    if (expect_give("/give diamond-axe 2", ITEM_DIAMOND_AXE, 2))
        return 1;
    if (expect_items("/items", 1))
        return 1;
    if (expect_items("/itemnames 2", 2))
        return 1;
    if (game_command_give_name_count() <= 0)
        return check_failed("give name list is empty");
    if (!game_command_give_name_at(0))
        return check_failed("give name list first entry missing");
    if (expect_completion("/phy", 0, "/physics"))
        return 1;
    if (expect_completion("/phy", 1, "/phys"))
        return 1;
    if (expect_completion("/physics set ", 0, "/physics set gravity"))
        return 1;
    if (expect_completion("/physics set ", 1, "/physics set player_speed"))
        return 1;
    if (expect_completion("/physics set ", 6, "/physics set gravity"))
        return 1;
    if (expect_completion("/physics set j", 0, "/physics set jump_velocity"))
        return 1;
    if (expect_completion("/physics set j", 1, "/physics set jump_height"))
        return 1;
    if (expect_completion("/time set ", 1, "/time set night"))
        return 1;
    if (expect_completion("/setblock 1 2 3 ", 0, "/setblock 1 2 3 air"))
        return 1;
    if (expect_completion("/blocks set ~ ~ ~ tor", 0,
                          "/blocks set ~ ~ ~ torch"))
        return 1;
    if (expect_completion("/give player diamond_a", 0,
                          "/give player diamond_axe"))
        return 1;

    printf("command_parser_test: ok\n");
    return 0;
}
