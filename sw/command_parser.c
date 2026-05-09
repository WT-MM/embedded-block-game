#include "command_parser.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define COMMAND_TOKEN_MAX 32
#define COMMAND_MAX_TOKENS 5

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
