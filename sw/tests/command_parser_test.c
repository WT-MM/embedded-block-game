#include <stdio.h>

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
    if (expect_fill("/blocks set ~-1 ~ ~1 ~1 ~2 ~3 diamond-block",
                    BLOCK_DIAMOND_BLOCK, true))
        return 1;

    printf("command_parser_test: ok\n");
    return 0;
}
