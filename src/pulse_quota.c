#include "pulse_quota.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_BYTES (1024u * 1024u)
#define JSON_MAX_TOKENS 32768u
#define JSON_MAX_DEPTH 64u
#define NO_TOKEN ((size_t)-1)
#define BAD_TOKEN ((size_t)-2)

typedef enum JsonKind {
    JSON_OBJECT, JSON_ARRAY, JSON_STRING, JSON_NUMBER,
    JSON_NULL, JSON_TRUE, JSON_FALSE
} JsonKind;

typedef struct JsonToken {
    JsonKind kind;
    size_t start;
    size_t length;
    size_t next;
} JsonToken;

typedef struct JsonParser {
    const char *json;
    size_t length;
    size_t position;
    JsonToken *tokens;
    size_t token_count;
    size_t token_capacity;
    const char *error;
} JsonParser;

static int fail(JsonParser *parser, const char *message)
{
    if (!parser->error) parser->error = message;
    return 0;
}

static void write_error(char *error, size_t capacity, const char *message)
{
    if (!error || !capacity) return;
    size_t length = strlen(message);
    if (length >= capacity) length = capacity - 1;
    memcpy(error, message, length);
    error[length] = '\0';
}

static void whitespace(JsonParser *parser)
{
    while (parser->position < parser->length) {
        char c = parser->json[parser->position];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        ++parser->position;
    }
}

static int digit(char c) { return c >= '0' && c <= '9'; }

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex4(const char *json, size_t available, unsigned *value)
{
    if (available < 4) return 0;
    *value = 0;
    for (size_t i = 0; i < 4; ++i) {
        int h = hex_digit(json[i]);
        if (h < 0) return 0;
        *value = (*value << 4) | (unsigned)h;
    }
    return 1;
}

static int utf8_length(const unsigned char *text, size_t available)
{
    if (!available) return 0;
    unsigned c = text[0];
    unsigned count, value, minimum;
    if (c >= 0xc2 && c <= 0xdf) { count = 2; value = c & 0x1f; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { count = 3; value = c & 0x0f; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { count = 4; value = c & 7; minimum = 0x10000; }
    else return 0;
    if (count > available) return 0;
    for (unsigned i = 1; i < count; ++i) {
        if ((text[i] & 0xc0) != 0x80) return 0;
        value = (value << 6) | (text[i] & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
    return (int)count;
}

/* A locale-independent conversion, used only after JSON number syntax checks. */
static int number_double(const char *text, size_t length, double *out)
{
    size_t position = 0;
    int negative = text[0] == '-';
    if (negative) ++position;
    double significant = 0;
    unsigned digits = 0;
    int fraction = 0;
    int64_t scale = 0;
    for (; position < length && text[position] != 'e' && text[position] != 'E'; ++position) {
        if (text[position] == '.') { fraction = 1; continue; }
        unsigned d = (unsigned)(text[position] - '0');
        if (fraction) --scale;
        if (!digits && !d) continue;
        if (digits < 19) {
            significant = significant * 10.0 + d;
            ++digits;
        } else {
            ++scale;
        }
    }
    if (position < length) {
        ++position;
        int exponent_negative = 0;
        if (text[position] == '-' || text[position] == '+') {
            exponent_negative = text[position] == '-';
            ++position;
        }
        int64_t exponent = 0;
        for (; position < length; ++position) {
            if (exponent < 1000000) exponent = exponent * 10 + (text[position] - '0');
        }
        scale += exponent_negative ? -exponent : exponent;
    }
    if (!significant || scale < -400) { *out = negative ? -0.0 : 0.0; return 1; }
    if (scale > 309) return 0;
    if (scale < -308) {
        significant *= 1e-308;
        scale += 308;
    }
    if (scale > 308) {
        significant *= 1e308;
        scale -= 308;
    }
    double value = significant * pow(10.0, (double)scale);
    if (!isfinite(value)) return 0;
    *out = negative ? -value : value;
    return 1;
}

static int number_integer(const char *text, size_t length, int64_t *out)
{
    size_t position = 0;
    int negative = text[0] == '-';
    if (negative) ++position;
    uint64_t value = 0;
    uint64_t maximum = negative ? UINT64_C(9223372036854775808) : UINT64_C(9223372036854775807);
    for (; position < length; ++position) {
        if (!digit(text[position])) return 0;
        unsigned d = (unsigned)(text[position] - '0');
        if (value > (maximum - d) / 10) return 0;
        value = value * 10 + d;
    }
    if (negative && value == UINT64_C(9223372036854775808)) *out = INT64_MIN;
    else *out = negative ? -(int64_t)value : (int64_t)value;
    return 1;
}

static int number_unsigned(const char *text, size_t length, uint64_t *out)
{
    uint64_t value = 0;
    for (size_t position = 0; position < length; ++position) {
        if (!digit(text[position])) return 0;
        unsigned d = (unsigned)(text[position] - '0');
        if (value > (UINT64_MAX - d) / 10) return 0;
        value = value * 10 + d;
    }
    *out = value;
    return 1;
}

static size_t new_token(JsonParser *parser, JsonKind kind, size_t start)
{
    if (parser->token_count == parser->token_capacity) {
        fail(parser, "JSON token capacity exceeded.");
        return NO_TOKEN;
    }
    size_t index = parser->token_count++;
    parser->tokens[index].kind = kind;
    parser->tokens[index].start = start;
    return index;
}

static int parse_string(JsonParser *parser, size_t *index)
{
    ++parser->position; /* opening quote */
    size_t start = parser->position;
    *index = new_token(parser, JSON_STRING, start);
    if (*index == NO_TOKEN) return 0;
    while (parser->position < parser->length) {
        unsigned char c = (unsigned char)parser->json[parser->position++];
        if (c == '"') {
            parser->tokens[*index].length = parser->position - start - 1;
            parser->tokens[*index].next = parser->token_count;
            return 1;
        }
        if (c < 0x20) return fail(parser, "Invalid JSON string.");
        if (c == '\\') {
            if (parser->position == parser->length) return fail(parser, "Incomplete JSON string.");
            c = (unsigned char)parser->json[parser->position++];
            if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't') continue;
            if (c != 'u') return fail(parser, "Invalid JSON escape.");
            unsigned value;
            if (!hex4(parser->json + parser->position, parser->length - parser->position, &value)) return fail(parser, "Invalid Unicode escape.");
            parser->position += 4;
            if (value >= 0xd800 && value <= 0xdbff) {
                if (parser->length - parser->position < 6 || parser->json[parser->position] != '\\' || parser->json[parser->position + 1] != 'u') return fail(parser, "Invalid Unicode surrogate pair.");
                unsigned low;
                if (!hex4(parser->json + parser->position + 2, 4, &low) || low < 0xdc00 || low > 0xdfff) return fail(parser, "Invalid Unicode surrogate pair.");
                parser->position += 6;
            } else if (value >= 0xdc00 && value <= 0xdfff) {
                return fail(parser, "Invalid Unicode surrogate pair.");
            }
        } else if (c >= 0x80) {
            int count = utf8_length((const unsigned char *)parser->json + parser->position - 1, parser->length - parser->position + 1);
            if (!count) return fail(parser, "Invalid UTF-8 in JSON string.");
            parser->position += (size_t)count - 1;
        }
    }
    return fail(parser, "Incomplete JSON string.");
}

static int parse_number(JsonParser *parser, size_t *index)
{
    size_t start = parser->position;
    if (parser->json[parser->position] == '-') ++parser->position;
    if (parser->position == parser->length) return fail(parser, "Incomplete JSON number.");
    if (parser->json[parser->position] == '0') ++parser->position;
    else {
        if (!digit(parser->json[parser->position])) return fail(parser, "Invalid JSON number.");
        while (parser->position < parser->length && digit(parser->json[parser->position])) ++parser->position;
    }
    if (parser->position < parser->length && parser->json[parser->position] == '.') {
        ++parser->position;
        if (parser->position == parser->length || !digit(parser->json[parser->position])) return fail(parser, "Invalid JSON number.");
        while (parser->position < parser->length && digit(parser->json[parser->position])) ++parser->position;
    }
    if (parser->position < parser->length && (parser->json[parser->position] == 'e' || parser->json[parser->position] == 'E')) {
        ++parser->position;
        if (parser->position < parser->length && (parser->json[parser->position] == '+' || parser->json[parser->position] == '-')) ++parser->position;
        if (parser->position == parser->length || !digit(parser->json[parser->position])) return fail(parser, "Invalid JSON exponent.");
        while (parser->position < parser->length && digit(parser->json[parser->position])) ++parser->position;
    }
    double value;
    size_t length = parser->position - start;
    if (!number_double(parser->json + start, length, &value)) return fail(parser, "Non-finite JSON number.");
    *index = new_token(parser, JSON_NUMBER, start);
    if (*index == NO_TOKEN) return 0;
    parser->tokens[*index].length = length;
    parser->tokens[*index].next = parser->token_count;
    return 1;
}

static int parse_value(JsonParser *parser, unsigned depth, size_t *index)
{
    whitespace(parser);
    if (parser->position == parser->length) return fail(parser, "Incomplete JSON value.");
    char c = parser->json[parser->position];
    if (c == '"') return parse_string(parser, index);
    if (c == '-' || digit(c)) return parse_number(parser, index);
    if (c == '{' || c == '[') {
        if (depth >= JSON_MAX_DEPTH) return fail(parser, "JSON nesting limit exceeded.");
        int object = c == '{';
        *index = new_token(parser, object ? JSON_OBJECT : JSON_ARRAY, parser->position++);
        if (*index == NO_TOKEN) return 0;
        whitespace(parser);
        char close = object ? '}' : ']';
        if (parser->position < parser->length && parser->json[parser->position] == close) ++parser->position;
        else {
            for (;;) {
                size_t child;
                if (object) {
                    if (parser->position == parser->length || parser->json[parser->position] != '"') return fail(parser, "JSON object requires a string key.");
                    if (!parse_string(parser, &child)) return 0;
                    whitespace(parser);
                    if (parser->position == parser->length || parser->json[parser->position++] != ':') return fail(parser, "JSON object requires a colon.");
                }
                if (!parse_value(parser, depth + 1, &child)) return 0;
                whitespace(parser);
                if (parser->position == parser->length) return fail(parser, "Incomplete JSON container.");
                c = parser->json[parser->position++];
                if (c == close) break;
                if (c != ',') return fail(parser, "JSON container requires a comma.");
                whitespace(parser);
            }
        }
        parser->tokens[*index].length = parser->position - parser->tokens[*index].start;
        parser->tokens[*index].next = parser->token_count;
        return 1;
    }
    const char *literal;
    JsonKind kind;
    size_t length;
    if (c == 'n') { literal = "null"; kind = JSON_NULL; length = 4; }
    else if (c == 't') { literal = "true"; kind = JSON_TRUE; length = 4; }
    else if (c == 'f') { literal = "false"; kind = JSON_FALSE; length = 5; }
    else return fail(parser, "Invalid JSON value.");
    if (parser->length - parser->position < length || memcmp(parser->json + parser->position, literal, length)) return fail(parser, "Invalid JSON literal.");
    *index = new_token(parser, kind, parser->position);
    if (*index == NO_TOKEN) return 0;
    parser->position += length;
    parser->tokens[*index].length = length;
    parser->tokens[*index].next = parser->token_count;
    return 1;
}

static int parse_json(JsonParser *parser, const char *json, size_t length)
{
    memset(parser, 0, sizeof(*parser));
    if (!json || !length) return fail(parser, "Empty JSON response.");
    if (length > JSON_MAX_BYTES) return fail(parser, "JSON response size limit exceeded.");
    parser->json = json;
    parser->length = length;
    parser->token_capacity = length / 2 + 4;
    if (parser->token_capacity > JSON_MAX_TOKENS) parser->token_capacity = JSON_MAX_TOKENS;
    parser->tokens = (JsonToken *)calloc(parser->token_capacity, sizeof(JsonToken));
    if (!parser->tokens) return fail(parser, "Unable to allocate JSON parser.");
    size_t root;
    if (!parse_value(parser, 0, &root)) return 0;
    whitespace(parser);
    if (parser->position != length) return fail(parser, "Unexpected content after JSON response.");
    if (parser->tokens[0].kind != JSON_OBJECT) return fail(parser, "JSON-RPC response must be an object.");
    return 1;
}

/* Syntax and Unicode were validated by parse_string. One character at a time
 * keeps truncation from splitting either an escaped or a raw UTF-8 character. */
static size_t string_character(const char *text, size_t *position, unsigned char output[4])
{
    unsigned char c = (unsigned char)text[(*position)++];
    if (c != '\\') {
        if (c < 0x80) { output[0] = c; return 1; }
        size_t count = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        output[0] = c;
        for (size_t i = 1; i < count; ++i) output[i] = (unsigned char)text[(*position)++];
        return count;
    }
    c = (unsigned char)text[(*position)++];
    if (c != 'u') {
        if (c == 'b') c = '\b';
        else if (c == 'f') c = '\f';
        else if (c == 'n') c = '\n';
        else if (c == 'r') c = '\r';
        else if (c == 't') c = '\t';
        output[0] = c;
        return 1;
    }
    unsigned value;
    (void)hex4(text + *position, 4, &value);
    *position += 4;
    if (value >= 0xd800 && value <= 0xdbff) {
        unsigned low;
        (void)hex4(text + *position + 2, 4, &low);
        *position += 6;
        value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
    }
    if (value < 0x80) { output[0] = (unsigned char)value; return 1; }
    if (value < 0x800) {
        output[0] = (unsigned char)(0xc0 | (value >> 6));
        output[1] = (unsigned char)(0x80 | (value & 0x3f));
        return 2;
    }
    if (value < 0x10000) {
        output[0] = (unsigned char)(0xe0 | (value >> 12));
        output[1] = (unsigned char)(0x80 | ((value >> 6) & 0x3f));
        output[2] = (unsigned char)(0x80 | (value & 0x3f));
        return 3;
    }
    output[0] = (unsigned char)(0xf0 | (value >> 18));
    output[1] = (unsigned char)(0x80 | ((value >> 12) & 0x3f));
    output[2] = (unsigned char)(0x80 | ((value >> 6) & 0x3f));
    output[3] = (unsigned char)(0x80 | (value & 0x3f));
    return 4;
}

static int string_equals(const JsonParser *parser, size_t index, const char *target)
{
    const JsonToken *token = &parser->tokens[index];
    const char *text = parser->json + token->start;
    size_t position = 0, target_position = 0;
    while (position < token->length) {
        unsigned char bytes[4];
        size_t count = string_character(text, &position, bytes);
        for (size_t i = 0; i < count; ++i) {
            if (!target[target_position] || (unsigned char)target[target_position++] != bytes[i]) return 0;
        }
    }
    return target[target_position] == '\0';
}

static size_t member(JsonParser *parser, size_t object, const char *key)
{
    size_t found = NO_TOKEN;
    for (size_t i = object + 1; i < parser->tokens[object].next;) {
        size_t value = i + 1;
        if (string_equals(parser, i, key)) {
            if (found != NO_TOKEN) {
                fail(parser, "Duplicate quota response field.");
                return BAD_TOKEN;
            }
            found = value;
        }
        i = parser->tokens[value].next;
    }
    return found;
}

static int copy_string(JsonParser *parser, size_t index, char *target, size_t capacity, int *truncated)
{
    if (index == BAD_TOKEN) return 0;
    if (index == NO_TOKEN || parser->tokens[index].kind == JSON_NULL) return 1;
    if (parser->tokens[index].kind != JSON_STRING) return fail(parser, "Quota label must be a string or null.");
    const JsonToken *token = &parser->tokens[index];
    const char *text = parser->json + token->start;
    size_t position = 0, written = 0;
    int full = 0;
    while (position < token->length) {
        unsigned char bytes[4];
        size_t count = string_character(text, &position, bytes);
        if (count == 1 && bytes[0] == 0) return fail(parser, "Quota label contains a NUL character.");
        if (full || count >= capacity - written) { *truncated = 1; full = 1; continue; }
        memcpy(target + written, bytes, count);
        written += count;
    }
    target[written] = '\0';
    return 1;
}

static int optional_integer(JsonParser *parser, size_t object, const char *key, int64_t *out)
{
    size_t token = member(parser, object, key);
    if (token == BAD_TOKEN) return 0;
    if (token == NO_TOKEN || parser->tokens[token].kind == JSON_NULL) return 1;
    if (parser->tokens[token].kind != JSON_NUMBER || !number_integer(parser->json + parser->tokens[token].start, parser->tokens[token].length, out)) return fail(parser, "Quota time must be an int64 or null.");
    return 1;
}

static int parse_window(JsonParser *parser, size_t token, PulseWindow *window)
{
    window->duration_mins = -1;
    window->resets_at = -1;
    if (token == BAD_TOKEN) return 0;
    if (token == NO_TOKEN || parser->tokens[token].kind == JSON_NULL) return 1;
    if (parser->tokens[token].kind != JSON_OBJECT) return fail(parser, "Quota window must be an object or null.");
    size_t used = member(parser, token, "usedPercent");
    if (used == BAD_TOKEN) return 0;
    if (used != NO_TOKEN && parser->tokens[used].kind != JSON_NULL) {
        if (parser->tokens[used].kind != JSON_NUMBER || !number_double(parser->json + parser->tokens[used].start, parser->tokens[used].length, &window->used_percent)) return fail(parser, "Quota percentage must be a finite number.");
        window->present = 1;
    }
    return optional_integer(parser, token, "windowDurationMins", &window->duration_mins) &&
           optional_integer(parser, token, "resetsAt", &window->resets_at);
}

static int optional_boolean(JsonParser *parser, size_t object, const char *key, int *out)
{
    size_t token = member(parser, object, key);
    if (token == BAD_TOKEN) return 0;
    if (token == NO_TOKEN || parser->tokens[token].kind == JSON_NULL) return 1;
    JsonKind kind = parser->tokens[token].kind;
    if (kind != JSON_TRUE && kind != JSON_FALSE) return fail(parser, "Quota credit flag must be a boolean.");
    *out = kind == JSON_TRUE;
    return 1;
}

static int parse_pool(JsonParser *parser, size_t token, size_t map_key, PulsePool *pool, int *truncated)
{
    memset(pool, 0, sizeof(*pool));
    pool->ordinary_usage_allowed = -1;
    pool->primary.duration_mins = pool->secondary.duration_mins = -1;
    pool->primary.resets_at = pool->secondary.resets_at = -1;
    if (map_key != NO_TOKEN && !copy_string(parser, map_key, pool->id, sizeof(pool->id), truncated)) return 0;
    if (parser->tokens[token].kind == JSON_NULL) return 1;
    if (parser->tokens[token].kind != JSON_OBJECT) return fail(parser, "Quota bucket must be an object or null.");
    if (map_key == NO_TOKEN && !copy_string(parser, member(parser, token, "limitId"), pool->id, sizeof(pool->id), truncated)) return 0;
    if (!copy_string(parser, member(parser, token, "limitName"), pool->name, sizeof(pool->name), truncated) ||
        !copy_string(parser, member(parser, token, "planType"), pool->plan, sizeof(pool->plan), truncated) ||
        !parse_window(parser, member(parser, token, "primary"), &pool->primary) ||
        !parse_window(parser, member(parser, token, "secondary"), &pool->secondary)) return 0;
    size_t credits = member(parser, token, "credits");
    if (credits == BAD_TOKEN) return 0;
    if (credits == NO_TOKEN || parser->tokens[credits].kind == JSON_NULL) return 1;
    if (parser->tokens[credits].kind != JSON_OBJECT) return fail(parser, "Quota credits must be an object or null.");
    return optional_boolean(parser, credits, "hasCredits", &pool->has_credits) &&
           optional_boolean(parser, credits, "unlimited", &pool->unlimited_credits) &&
           copy_string(parser, member(parser, credits, "balance"), pool->credit_balance, sizeof(pool->credit_balance), truncated);
}

static int parse_snapshot(JsonParser *parser, PulseSnapshot *out)
{
    size_t error = member(parser, 0, "error");
    if (error == BAD_TOKEN) return 0;
    if (error != NO_TOKEN && parser->tokens[error].kind != JSON_NULL) return fail(parser, "Codex app-server returned an RPC error.");
    size_t result = member(parser, 0, "result");
    if (result == BAD_TOKEN) return 0;
    if (result == NO_TOKEN || parser->tokens[result].kind != JSON_OBJECT) return fail(parser, "Quota response has no result object.");
    int permission = -1;
    if (!optional_boolean(parser, result, "ordinaryUsageAllowed", &permission)) return 0;
    size_t map = member(parser, result, "rateLimitsByLimitId");
    if (map == BAD_TOKEN) return 0;
    if (map != NO_TOKEN && parser->tokens[map].kind != JSON_NULL) {
        if (parser->tokens[map].kind != JSON_OBJECT) return fail(parser, "Quota bucket map must be an object or null.");
        if (parser->tokens[map].next > map + 1) {
            for (size_t i = map + 1; i < parser->tokens[map].next;) {
                size_t value = i + 1;
                PulsePool overflow;
                PulsePool *pool = out->pool_count < PULSE_MAX_POOLS ? &out->pools[out->pool_count] : &overflow;
                if (!parse_pool(parser, value, i, pool, &out->truncated)) return 0;
                pool->ordinary_usage_allowed = permission;
                if (out->pool_count < PULSE_MAX_POOLS) ++out->pool_count;
                else out->truncated = 1;
                i = parser->tokens[value].next;
            }
            return 1;
        }
    }
    size_t legacy = member(parser, result, "rateLimits");
    if (legacy == BAD_TOKEN) return 0;
    if (legacy == NO_TOKEN) return fail(parser, "Quota response contains no quota buckets.");
    if (parser->tokens[legacy].kind == JSON_NULL) return 1;
    if (!parse_pool(parser, legacy, NO_TOKEN, &out->pools[0], &out->truncated)) return 0;
    out->pools[0].ordinary_usage_allowed = permission;
    out->pool_count = 1;
    return 1;
}

int pulse_parse_response(const char *json, size_t length, PulseSnapshot *out,
                         char *error, size_t error_capacity)
{
    if (error && error_capacity) error[0] = '\0';
    if (!out) { write_error(error, error_capacity, "Missing quota output."); return 0; }
    memset(out, 0, sizeof(*out));
    JsonParser parser;
    int success = parse_json(&parser, json, length);
    PulseSnapshot snapshot = {0};
    if (success) success = parse_snapshot(&parser, &snapshot);
    if (success) *out = snapshot;
    else write_error(error, error_capacity, parser.error ? parser.error : "Invalid quota response.");
    free(parser.tokens);
    return success;
}

int pulse_response_id(const char *json, size_t length, int64_t *out)
{
    if (!out) return 0;
    JsonParser parser;
    int success = parse_json(&parser, json, length);
    int64_t id = 0;
    if (success) {
        size_t token = member(&parser, 0, "id");
        success = token != NO_TOKEN && token != BAD_TOKEN && parser.tokens[token].kind == JSON_NUMBER &&
            number_integer(parser.json + parser.tokens[token].start, parser.tokens[token].length, &id);
    }
    if (success) *out = id;
    free(parser.tokens);
    return success;
}

int pulse_response_ok(const char *json, size_t length)
{
    JsonParser parser;
    int success = parse_json(&parser, json, length);
    if (success) {
        size_t result = member(&parser, 0, "result");
        size_t error = member(&parser, 0, "error");
        success = result != NO_TOKEN && result != BAD_TOKEN && error == NO_TOKEN;
    }
    free(parser.tokens);
    return success;
}

static int usage_counter(JsonParser *parser, size_t object, const char *key,
                         uint64_t *out, int *known)
{
    size_t token = member(parser, object, key);
    *known = 0;
    if (token == BAD_TOKEN) return 0;
    if (token == NO_TOKEN) return 1;
    if (parser->tokens[token].kind != JSON_NUMBER ||
        !number_unsigned(parser->json + parser->tokens[token].start,
                         parser->tokens[token].length, out))
        return fail(parser, "Token count must be an unsigned 64-bit integer.");
    *known = 1;
    return 1;
}

static int parse_usage_group(JsonParser *parser, size_t token, PulseTokenUsage *usage)
{
    if (token == BAD_TOKEN) return 0;
    if (token == NO_TOKEN || parser->tokens[token].kind == JSON_NULL) return 1;
    if (parser->tokens[token].kind != JSON_OBJECT) return fail(parser, "Token usage must be an object or null.");
    int input, cached, cache_write, output, reasoning, total;
    if (!usage_counter(parser, token, "input_tokens", &usage->input_tokens, &input) ||
        !usage_counter(parser, token, "cached_input_tokens", &usage->cached_input_tokens, &cached) ||
        !usage_counter(parser, token, "cache_write_input_tokens", &usage->cache_write_input_tokens, &cache_write) ||
        !usage_counter(parser, token, "output_tokens", &usage->output_tokens, &output) ||
        !usage_counter(parser, token, "reasoning_output_tokens", &usage->reasoning_output_tokens, &reasoning) ||
        !usage_counter(parser, token, "total_tokens", &usage->total_tokens, &total)) return 0;
    if (cached && input && usage->cached_input_tokens > usage->input_tokens)
        return fail(parser, "Cached token count exceeds input count.");
    if (reasoning && output && usage->reasoning_output_tokens > usage->output_tokens)
        return fail(parser, "Reasoning token count exceeds output count.");
    if (input && output) {
        if (usage->input_tokens > UINT64_MAX - usage->output_tokens)
            return fail(parser, "Token total overflows uint64.");
        if (total && usage->total_tokens != usage->input_tokens + usage->output_tokens)
            return fail(parser, "Token total is inconsistent.");
    }
    usage->present = input && cached && output && reasoning && total;
    return 1;
}

static int parse_usage(JsonParser *parser, PulseUsage *usage)
{
    size_t type = member(parser, 0, "type");
    if (type == BAD_TOKEN) return -1;
    if (type == NO_TOKEN) return 0;
    if (parser->tokens[type].kind != JSON_STRING) return -1;
    if (!string_equals(parser, type, "event_msg")) return 0;
    size_t payload = member(parser, 0, "payload");
    if (payload == BAD_TOKEN) return -1;
    if (payload == NO_TOKEN || parser->tokens[payload].kind == JSON_NULL) return 0;
    if (parser->tokens[payload].kind != JSON_OBJECT) return -1;
    type = member(parser, payload, "type");
    if (type == BAD_TOKEN) return -1;
    if (type == NO_TOKEN) return 0;
    if (parser->tokens[type].kind != JSON_STRING) return -1;
    if (!string_equals(parser, type, "token_count")) return 0;
    size_t info = member(parser, payload, "info");
    if (info == BAD_TOKEN) return -1;
    if (info == NO_TOKEN || parser->tokens[info].kind == JSON_NULL) return 0;
    if (parser->tokens[info].kind != JSON_OBJECT) return -1;
    if (!parse_usage_group(parser, member(parser, info, "total_token_usage"), &usage->total) ||
        !parse_usage_group(parser, member(parser, info, "last_token_usage"), &usage->last) ||
        !optional_integer(parser, info, "model_context_window", &usage->model_context_window)) return -1;
    if (usage->model_context_window < -1) return -1;
    size_t context = member(parser, info, "model_context_window");
    if (context != NO_TOKEN && context != BAD_TOKEN && parser->tokens[context].kind != JSON_NULL && usage->model_context_window < 0) return -1;
    int truncated = 0;
    if (!copy_string(parser, member(parser, 0, "timestamp"), usage->timestamp, sizeof(usage->timestamp), &truncated)) return -1;
    return 1;
}

int pulse_parse_usage_event(const char *json, size_t length, PulseUsage *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->model_context_window = -1;
    JsonParser parser;
    int success = parse_json(&parser, json, length);
    PulseUsage usage = {0};
    usage.model_context_window = -1;
    int status = success ? parse_usage(&parser, &usage) : -1;
    if (status == 1) *out = usage;
    free(parser.tokens);
    return status;
}

double pulse_remaining(const PulseWindow *window)
{
    if (!window || !window->present || !isfinite(window->used_percent)) return -1;
    if (window->used_percent <= 0) return 100;
    if (window->used_percent >= 100) return 0;
    return 100 - window->used_percent;
}

int pulse_health(double remaining)
{
    if (!isfinite(remaining) || remaining < 0) return 3;
    if (remaining > 50) return 0;
    if (remaining >= 20) return 1;
    return 2;
}
