#include "pulse_quota.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
static unsigned checks;

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    ++failures; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
} } while (0)

static PulseSnapshot snapshot;
static char error[160];

static int parse(const char *json)
{
    return pulse_parse_response(json, strlen(json), &snapshot, error, sizeof(error));
}

static void windows_and_buckets(void)
{
    CHECK(parse("{\"id\":2,\"result\":{\"rateLimits\":{\"limitId\":\"legacy\",\"primary\":{\"usedPercent\":80}},\"rateLimitsByLimitId\":{\"codex\":{\"limitName\":\"Codex\",\"planType\":\"plus\",\"primary\":{\"usedPercent\":12.5,\"windowDurationMins\":300,\"resetsAt\":1900000000},\"secondary\":{\"usedPercent\":75,\"windowDurationMins\":10080,\"resetsAt\":1900000001},\"credits\":{\"hasCredits\":true,\"unlimited\":false,\"balance\":\"12.30\"}},\"fast\":{\"primary\":null}},\"extra\":[true,{\"nested\":\"usedPercent\"}]}}"));
    CHECK(snapshot.pool_count == 2);
    CHECK(!strcmp(snapshot.pools[0].id, "codex"));
    CHECK(!strcmp(snapshot.pools[0].plan, "plus"));
    CHECK(snapshot.pools[0].primary.present);
    CHECK(snapshot.pools[0].primary.used_percent == 12.5);
    CHECK(snapshot.pools[0].primary.duration_mins == 300);
    CHECK(snapshot.pools[0].primary.resets_at == INT64_C(1900000000));
    CHECK(pulse_remaining(&snapshot.pools[0].primary) == 87.5);
    CHECK(pulse_remaining(&snapshot.pools[0].secondary) == 25);
    CHECK(snapshot.pools[0].has_credits && !snapshot.pools[0].unlimited_credits);
    CHECK(!strcmp(snapshot.pools[0].credit_balance, "12.30"));
    CHECK(pulse_remaining(&snapshot.pools[1].primary) == -1);
    CHECK(snapshot.pools[1].primary.duration_mins == -1);
    CHECK(snapshot.pools[1].secondary.resets_at == -1);
    CHECK(snapshot.pools[0].ordinary_usage_allowed == -1);
    CHECK(parse("{\"result\":{\"ordinaryUsageAllowed\":false,\"rateLimits\":{\"primary\":{\"usedPercent\":0}}}}"));
    CHECK(snapshot.pools[0].ordinary_usage_allowed == 0);
    CHECK(pulse_remaining(&snapshot.pools[0].primary) == 100);
    CHECK(parse("{\"result\":{\"ordinaryUsageAllowed\":true,\"rateLimitsByLimitId\":{\"a\":{},\"b\":null}}}"));
    CHECK(snapshot.pools[0].ordinary_usage_allowed == 1 && snapshot.pools[1].ordinary_usage_allowed == 1);

    CHECK(parse("{\"result\":{\"rateLimitsByLimitId\":{},\"rateLimits\":{\"limitId\":\"legacy\",\"primary\":{\"usedPercent\":125}}}}"));
    CHECK(snapshot.pool_count == 1 && !strcmp(snapshot.pools[0].id, "legacy"));
    CHECK(pulse_remaining(&snapshot.pools[0].primary) == 0);
    CHECK(parse("{\"result\":{\"rateLimitsByLimitId\":null,\"rateLimits\":{\"primary\":{\"usedPercent\":-2e0},\"secondary\":{}}}}"));
    CHECK(pulse_remaining(&snapshot.pools[0].primary) == 100);
    CHECK(pulse_remaining(&snapshot.pools[0].secondary) == -1);
    CHECK(parse("{\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":null,\"resetsAt\":null}}}}"));
    CHECK(!snapshot.pools[0].primary.present && snapshot.pools[0].primary.resets_at == -1);
    CHECK(parse("{\"result\":{\"rateLimits\":null}}"));
    CHECK(snapshot.pool_count == 0);
    CHECK(parse("{\"result\":{\"rateLimitsByLimitId\":{\"empty\":null},\"rateLimits\":{\"primary\":{\"usedPercent\":7}}}}"));
    CHECK(snapshot.pool_count == 1 && !snapshot.pools[0].primary.present);
    CHECK(parse("{\"result\":{\"rateLimits\":{}}}"));
    CHECK(snapshot.pool_count == 1 && pulse_remaining(&snapshot.pools[0].primary) == -1);
}

static void strings_and_extra_fields(void)
{
    CHECK(parse("{\"result\":{\"rateLimitsByLimitId\":{\"co\\u0064ex\":{\"limitName\":\"A\\\"B\\\\C\\n\\u4e2d\\ud83d\\ude80\",\"primary\":{\"used\\u0050ercent\":2.25,\"ignored\":{\"usedPercent\":99}},\"extra\":null}}}}"));
    CHECK(!strcmp(snapshot.pools[0].id, "codex"));
    CHECK(!strcmp(snapshot.pools[0].name, "A\"B\\C\n\xe4\xb8\xad\xf0\x9f\x9a\x80"));
    CHECK(snapshot.pools[0].primary.used_percent == 2.25);
    CHECK(parse("{\"result\":{\"rateLimits\":{\"limitName\":\"\xe4\xb8\xad\xe6\x96\x87\",\"extra\":0e99999}}}"));
    CHECK(!strcmp(snapshot.pools[0].name, "\xe4\xb8\xad\xe6\x96\x87"));
    CHECK(!parse("{\"result\":{\"rateLimits\":{\"limitName\":\"bad\\u0000label\"}}}"));
}

static void invalid_inputs(void)
{
    const char *cases[] = {
        "", "null", "[]", "{}", "{\"result\":null}",
        "{\"result\":{\"rateLimits\":{}}}extra",
        "{\"result\":{\"rateLimits\":{}},}",
        "{\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":01}}}}",
        "{\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":1.}}}}",
        "{\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":1e}}}}",
        "{\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":1e999}}}}",
        "{\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":NaN}}}}",
        "{\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":\"12\"}}}}",
        "{\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":2,\"resetsAt\":1.5}}}}",
        "{\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":2,\"resetsAt\":9223372036854775808}}}}",
        "{\"result\":{\"rateLimitsByLimitId\":[]}}",
        "{\"result\":{\"rateLimits\":false}}",
        "{\"result\":{\"rateLimits\":{\"primary\":false}}}",
        "{\"result\":{\"rateLimits\":{\"credits\":{\"hasCredits\":1}}}}",
        "{\"result\":{\"rateLimits\":{\"limitName\":4}}}",
        "{\"result\":{\"ordinaryUsageAllowed\":\"true\",\"rateLimits\":{}}}",
        "{\"result\":{\"rateLimits\":{\"limitName\":\"\\q\"}}}",
        "{\"result\":{\"rateLimits\":{\"limitName\":\"\\ud800\"}}}",
        "{\"result\":{\"rateLimits\":{\"limitName\":\"\\udc00\"}}}",
        "{\"result\":{\"rateLimits\":{\"limitName\":\"\xc0\xaf\"}}}",
        "{\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":1,\"usedPercent\":2}}}}",
        "{\"result\":{\"rateLimits\":{\"extra\":1e999}}}",
        "{\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":2}}}",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memset(&snapshot, 0x7f, sizeof(snapshot));
        CHECK(!parse(cases[i]));
        CHECK(snapshot.pool_count == 0);
        CHECK(error[0] != '\0');
    }
    CHECK(!parse("{\"id\":2,\"error\":{\"code\":-32000,\"message\":\"synthetic secret marker\"}}"));
    CHECK(!strstr(error, "synthetic") && !strstr(error, "marker"));
}

static void ids_and_health(void)
{
    int64_t id = 777;
    const char *maximum = "{\"id\":9223372036854775807,\"result\":{}}";
    CHECK(pulse_response_id(maximum, strlen(maximum), &id) && id == INT64_MAX);
    const char *minimum = "{\"id\":-9223372036854775808,\"result\":{}}";
    CHECK(pulse_response_id(minimum, strlen(minimum), &id) && id == INT64_MIN);
    const char *invalid[] = {
        "{\"id\":1.0}", "{\"id\":1e0}", "{\"id\":\"1\"}", "{\"id\":null}",
        "{\"id\":9223372036854775808}", "{\"id\":1,\"id\":2}", "{\"id\":1}x", "{\"method\":\"notification\"}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        id = 777;
        CHECK(!pulse_response_id(invalid[i], strlen(invalid[i]), &id));
        CHECK(id == 777);
    }
    CHECK(pulse_remaining(NULL) == -1);
    CHECK(pulse_health(50.01) == 0);
    CHECK(pulse_health(50) == 1);
    CHECK(pulse_health(20) == 1);
    CHECK(pulse_health(19.99) == 2);
    CHECK(pulse_health(0) == 2);
    CHECK(pulse_health(-1) == 3);
    CHECK(pulse_health(NAN) == 3);
    PulseWindow window = {1, NAN, -1, -1};
    CHECK(pulse_remaining(&window) == -1);
    const char *ok[] = {"{\"result\":{}}", "{\"result\":null}", "{\"result\":42}", "{\"result\":[1,2]}"};
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); ++i) CHECK(pulse_response_ok(ok[i], strlen(ok[i])));
    const char *not_ok[] = {"{}", "[]", "{\"result\":{},\"error\":null}", "{\"result\":{}}x", "{\"result\":{},\"result\":{}}"};
    for (size_t i = 0; i < sizeof(not_ok) / sizeof(not_ok[0]); ++i) CHECK(!pulse_response_ok(not_ok[i], strlen(not_ok[i])));
}

static void bounded_storage(void)
{
    char json[4096];
    size_t position = (size_t)snprintf(json, sizeof(json), "{\"result\":{\"rateLimitsByLimitId\":{");
    for (unsigned i = 0; i < 20; ++i) position += (size_t)snprintf(json + position, sizeof(json) - position, "%s\"pool%u\":{\"primary\":{\"usedPercent\":%u}}", i ? "," : "", i, i);
    (void)snprintf(json + position, sizeof(json) - position, "}}}");
    CHECK(parse(json));
    CHECK(snapshot.pool_count == PULSE_MAX_POOLS && snapshot.truncated);
    CHECK(!strcmp(snapshot.pools[15].id, "pool15"));

    position = (size_t)snprintf(json, sizeof(json), "{\"result\":{\"rateLimits\":{\"limitName\":\"");
    for (unsigned i = 0; i < 60; ++i) { memcpy(json + position, "\xe4\xb8\xad", 3); position += 3; }
    (void)snprintf(json + position, sizeof(json) - position, "\"}}}");
    CHECK(parse(json));
    CHECK(snapshot.truncated && strlen(snapshot.pools[0].name) == 126);
    CHECK((unsigned char)snapshot.pools[0].name[125] == 0xad);

    char tiny_error[2] = {'x', 'x'};
    CHECK(!pulse_parse_response("{}", 2, &snapshot, tiny_error, sizeof(tiny_error)));
    CHECK(tiny_error[0] != '\0' && tiny_error[1] == '\0');
    CHECK(!pulse_parse_response("{}", 2, &snapshot, NULL, 0));
    CHECK(!pulse_parse_response("{}", 2, NULL, NULL, 0));
    CHECK(!pulse_parse_response(NULL, 0, &snapshot, error, sizeof(error)));

    char nested[256];
    position = 0;
    for (unsigned i = 0; i < 65; ++i) nested[position++] = '[';
    nested[position++] = '0';
    for (unsigned i = 0; i < 65; ++i) nested[position++] = ']';
    nested[position] = '\0';
    CHECK(!parse(nested));
    CHECK(strstr(error, "nesting") != NULL);

    size_t huge_length = 1024u * 1024u + 1;
    char *huge = (char *)malloc(huge_length);
    CHECK(huge != NULL);
    if (huge) {
        memset(huge, ' ', huge_length);
        CHECK(!pulse_parse_response(huge, huge_length, &snapshot, error, sizeof(error)));
        CHECK(strstr(error, "size limit") != NULL);
        free(huge);
    }
    size_t token_length = 70000;
    char *many = (char *)malloc(token_length);
    CHECK(many != NULL);
    if (many) {
        position = 0;
        many[position++] = '[';
        for (unsigned i = 0; i < 33000; ++i) {
            if (i) many[position++] = ',';
            many[position++] = '0';
        }
        many[position++] = ']';
        many[position] = '\0';
        CHECK(!parse(many));
        CHECK(strstr(error, "capacity") != NULL);
        free(many);
    }
}

static int parse_usage(const char *json, PulseUsage *usage)
{
    return pulse_parse_usage_event(json, strlen(json), usage);
}

static void usage_contract(void)
{
    PulseUsage usage;
    CHECK(parse_usage("{\"timestamp\":\"2026-01-01T01:02:03Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":80,\"cached_input_tokens\":24,\"cache_write_input_tokens\":8,\"output_tokens\":20,\"reasoning_output_tokens\":10,\"total_tokens\":100},\"last_token_usage\":{\"input_tokens\":40,\"cached_input_tokens\":12,\"output_tokens\":5,\"reasoning_output_tokens\":3,\"total_tokens\":45},\"model_context_window\":128000},\"timestamp\":\"ignored\"}}", &usage) == 1);
    CHECK(usage.total.present && usage.last.present);
    CHECK(usage.total.input_tokens == 80 && usage.total.cached_input_tokens == 24);
    CHECK(usage.total.cache_write_input_tokens == 8 && usage.last.cache_write_input_tokens == 0);
    CHECK(usage.total.output_tokens == 20 && usage.total.reasoning_output_tokens == 10 && usage.total.total_tokens == 100);
    CHECK(usage.last.input_tokens == 40 && usage.last.total_tokens == 45);
    CHECK(usage.model_context_window == 128000);
    CHECK(!strcmp(usage.timestamp, "2026-01-01T01:02:03Z"));
    CHECK(parse_usage("{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":1,\"cached_input_tokens\":0,\"output_tokens\":1,\"total_tokens\":2},\"last_token_usage\":null,\"model_context_window\":null}}}", &usage) == 1);
    CHECK(!usage.total.present && !usage.last.present && usage.model_context_window == -1);
    CHECK(usage.timestamp[0] == '\0');
    CHECK(parse_usage("{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":18446744073709551615,\"cached_input_tokens\":18446744073709551615,\"output_tokens\":0,\"reasoning_output_tokens\":0,\"total_tokens\":18446744073709551615}}}}", &usage) == 1);
    CHECK(usage.total.present && usage.total.total_tokens == UINT64_MAX);

    const char *unrelated[] = {
        "{\"type\":\"session_meta\",\"payload\":{\"type\":\"token_count\",\"info\":{}}}",
        "{\"type\":\"event_msg\",\"payload\":{\"type\":\"agent_message\",\"message\":\"token_count\"}}",
        "{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":null}}",
        "{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\"}}",
        "{\"payload\":{\"type\":\"token_count\",\"info\":{}}}"
    };
    for (size_t i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); ++i) {
        CHECK(parse_usage(unrelated[i], &usage) == 0);
        CHECK(!usage.total.present && usage.model_context_window == -1 && usage.timestamp[0] == '\0');
    }
    const char *bad_count[] = {
        "-1", "-0", "1.0", "1e0", "18446744073709551616", "null", "true", "\"3\""
    };
    char fixture[1024];
    for (size_t i = 0; i < sizeof(bad_count) / sizeof(bad_count[0]); ++i) {
        (void)snprintf(fixture, sizeof(fixture), "{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":%s}}}}", bad_count[i]);
        CHECK(parse_usage(fixture, &usage) == -1);
        CHECK(!usage.total.present && usage.model_context_window == -1);
    }
    const char *bad_group[] = {
        "{\"input_tokens\":1,\"cached_input_tokens\":2}",
        "{\"output_tokens\":1,\"reasoning_output_tokens\":2}",
        "{\"input_tokens\":2,\"output_tokens\":3,\"total_tokens\":6}",
        "{\"input_tokens\":18446744073709551615,\"output_tokens\":1,\"total_tokens\":0}",
        "{\"input_tokens\":1,\"input_tokens\":2}",
        "[]"
    };
    for (size_t i = 0; i < sizeof(bad_group) / sizeof(bad_group[0]); ++i) {
        (void)snprintf(fixture, sizeof(fixture), "{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"last_token_usage\":%s}}}", bad_group[i]);
        CHECK(parse_usage(fixture, &usage) == -1);
    }
    CHECK(parse_usage("{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"model_context_window\":-1}}}", &usage) == -1);
    CHECK(parse_usage("{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"model_context_window\":9223372036854775808}}}", &usage) == -1);
    CHECK(parse_usage("{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":true}}", &usage) == -1);
    CHECK(parse_usage("{\"type\":\"event_msg\",\"payload\":{\"type\":2}}", &usage) == -1);
    CHECK(parse_usage("{\"type\":2}", &usage) == -1);
    CHECK(parse_usage("{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{}}", &usage) == -1);
    CHECK(pulse_parse_usage_event(NULL, 0, &usage) == -1);
    CHECK(pulse_parse_usage_event("{}", 2, NULL) == -1);

    size_t position = (size_t)snprintf(fixture, sizeof(fixture), "{\"timestamp\":\"");
    for (unsigned i = 0; i < 30; ++i) { memcpy(fixture + position, "\xe4\xb8\xad", 3); position += 3; }
    (void)snprintf(fixture + position, sizeof(fixture) - position, "\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{}}}");
    CHECK(parse_usage(fixture, &usage) == 1);
    CHECK(strlen(usage.timestamp) == 63 && (unsigned char)usage.timestamp[62] == 0xad);
}

int main(void)
{
    windows_and_buckets();
    strings_and_extra_fields();
    invalid_inputs();
    ids_and_health();
    bounded_storage();
    usage_contract();
    printf("%s: %u checks, %u failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
