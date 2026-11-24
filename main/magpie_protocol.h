#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAGPIE_FRAME_CAPACITY 160
#define MAGPIE_STALE_MS UINT64_C(90000)

/* A uint64_t with thousands separators takes at most 26 characters. */
#define MAGPIE_TOKEN_TEXT_CAPACITY 27

typedef struct {
    char synced_at[6];
    uint64_t today_total; /* Magpie GUI total, including cache reads and writes. */
    uint64_t today_input;
    uint64_t today_output;
    uint64_t today_cache; /* Cache reads plus cache writes. */
    uint64_t last_7_days;
    uint64_t last_30_days;
} magpie_usage_t;

typedef struct {
    char line[MAGPIE_FRAME_CAPACITY];
    size_t length;
    bool discarding;
} magpie_stream_t;

typedef enum {
    MAGPIE_WAITING,
    MAGPIE_LIVE,
    MAGPIE_PAUSED,
    MAGPIE_STALE,
} magpie_status_t;

typedef struct {
    magpie_usage_t usage;
    uint64_t last_received_ms;
    bool has_received;
    bool has_usage;
    bool source_error;
} magpie_link_t;

typedef enum {
    MAGPIE_REPLY_NONE,
    MAGPIE_REPLY_ACK,
    MAGPIE_REPLY_PONG,
} magpie_reply_t;

/* Discard incomplete framing when a transport connection ends. */
void magpie_stream_reset(magpie_stream_t *stream);

/* ACK follows an applied sample/error. PONG confirms the link without changing state. */
magpie_reply_t magpie_stream_receive(magpie_stream_t *stream, magpie_link_t *link,
                                     uint8_t byte, uint64_t now_ms);
magpie_status_t magpie_status(const magpie_link_t *link, uint64_t now_ms);
void magpie_format_tokens(uint64_t value, char formatted[MAGPIE_TOKEN_TEXT_CAPACITY]);
