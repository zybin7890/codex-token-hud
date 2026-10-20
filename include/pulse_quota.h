#ifndef PULSE_QUOTA_H
#define PULSE_QUOTA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PULSE_MAX_POOLS 16

typedef struct PulseWindow {
    int present;
    double used_percent;
    /* -1 means the server did not provide this value. */
    int64_t duration_mins;
    int64_t resets_at;
} PulseWindow;

typedef struct PulsePool {
    char id[128];
    char name[128];
    char plan[64];
    PulseWindow primary;
    PulseWindow secondary;
    int has_credits;
    int unlimited_credits;
    char credit_balance[64];
    /* Backend permission, independent of percentage: -1 unknown, 0 no, 1 yes. */
    int ordinary_usage_allowed;
} PulsePool;

typedef struct PulseSnapshot {
    PulsePool pools[PULSE_MAX_POOLS];
    size_t pool_count;
    /* Pool count or text exceeded its bounded storage. */
    int truncated;
} PulseSnapshot;

typedef struct PulseTokenUsage {
    int present;
    uint64_t input_tokens;
    uint64_t cached_input_tokens;
    uint64_t cache_write_input_tokens;
    uint64_t output_tokens;
    uint64_t reasoning_output_tokens;
    uint64_t total_tokens;
} PulseTokenUsage;

typedef struct PulseUsage {
    PulseTokenUsage total;
    PulseTokenUsage last;
    /* -1 means no context-window value was provided. */
    int64_t model_context_window;
    char timestamp[64];
} PulseUsage;

/* Accepts a complete app-server JSON-RPC response, up to 1 MiB.
 * Missing/null windows remain unavailable. The output is cleared on failure.
 * A nonempty rateLimitsByLimitId map takes precedence over rateLimits.
 * Error text never copies the server's potentially private error message.
 */
int pulse_parse_response(const char *json, size_t length, PulseSnapshot *out,
                         char *error, size_t error_capacity);

/* Validates the complete message; accepts only a plain int64 numeric id. */
int pulse_response_id(const char *json, size_t length, int64_t *out);

/* Complete JSON object containing result and no error member. */
int pulse_response_ok(const char *json, size_t length);

/* Reads only event_msg / token_count snapshots, never message text.
 * 1: usable info object; 0: other event or absent/null info;
 * -1: malformed JSON, invalid types/counts, or inconsistent token totals.
 * Missing required fields make the corresponding usage group unavailable.
 * Timestamp text is bounded and truncated on a UTF-8 character boundary.
 */
int pulse_parse_usage_event(const char *json, size_t length, PulseUsage *out);

/* -1 means unavailable; other values are clamped to [0,100]. */
double pulse_remaining(const PulseWindow *window);

/* 0: >50%, 1: 20..50%, 2: 0..<20%, 3: unavailable. */
int pulse_health(double remaining);

#ifdef __cplusplus
}
#endif

#endif
