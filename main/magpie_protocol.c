#include "magpie_protocol.h"

#include <string.h>

static bool decimal(char character)
{
    return character >= '0' && character <= '9';
}

static bool parse_number(const char **cursor, char delimiter, uint64_t *result)
{
    const char *next = *cursor;
    if (!decimal(*next)) {
        return false;
    }

    uint64_t value = 0;
    do {
        unsigned digit = (unsigned)(*next++ - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    } while (decimal(*next));

    if (*next != delimiter) {
        return false;
    }
    *cursor = next + 1;
    *result = value;
    return true;
}

static bool valid_time(const char *time)
{
    return strlen(time) >= 6 && decimal(time[0]) && decimal(time[1]) && time[2] == ':' &&
           decimal(time[3]) && decimal(time[4]) && time[5] == ',' &&
           (time[0] - '0') * 10 + time[1] - '0' < 24 && time[3] <= '5';
}

static bool apply_frame(const char *line, magpie_link_t *link, uint64_t now_ms)
{
    const char ok_prefix[] = "MPU1,OK,";
    bool source_error = strcmp(line, "MPU1,ERR") == 0;
    if (!source_error) {
        if (strncmp(line, ok_prefix, sizeof(ok_prefix) - 1) != 0) {
            return false;
        }
        const char *time = line + sizeof(ok_prefix) - 1;
        if (!valid_time(time)) {
            return false;
        }

        magpie_usage_t sample = {0};
        memcpy(sample.synced_at, time, 5);
        const char *next = time + 6;
        if (!parse_number(&next, ',', &sample.today_total) ||
            !parse_number(&next, ',', &sample.today_input) ||
            !parse_number(&next, ',', &sample.today_output) ||
            !parse_number(&next, ',', &sample.today_cache) ||
            !parse_number(&next, ',', &sample.last_7_days) ||
            !parse_number(&next, '\0', &sample.last_30_days)) {
            return false;
        }
        link->usage = sample;
        link->has_usage = true;
    }

    link->source_error = source_error;
    link->last_received_ms = now_ms;
    link->has_received = true;
    return true;
}

void magpie_stream_reset(magpie_stream_t *stream)
{
    stream->length = 0;
    stream->discarding = false;
}

magpie_reply_t magpie_stream_receive(magpie_stream_t *stream, magpie_link_t *link,
                                     uint8_t byte, uint64_t now_ms)
{
    if (byte != '\n') {
        if (byte == '\0' || stream->length >= sizeof(stream->line) - 1) {
            stream->discarding = true;
        }
        if (!stream->discarding) {
            stream->line[stream->length++] = (char)byte;
        }
        return MAGPIE_REPLY_NONE;
    }

    magpie_reply_t reply = MAGPIE_REPLY_NONE;
    if (!stream->discarding && stream->length > 0) {
        if (stream->line[stream->length - 1] == '\r') {
            --stream->length;
        }
        stream->line[stream->length] = '\0';
        if (strcmp(stream->line, "MPU1,PING") == 0) {
            reply = MAGPIE_REPLY_PONG;
        } else if (apply_frame(stream->line, link, now_ms)) {
            reply = MAGPIE_REPLY_ACK;
        }
    }
    magpie_stream_reset(stream);
    return reply;
}

magpie_status_t magpie_status(const magpie_link_t *link, uint64_t now_ms)
{
    if (!link->has_received) {
        return MAGPIE_WAITING;
    }
    if (now_ms - link->last_received_ms > MAGPIE_STALE_MS) {
        return MAGPIE_STALE;
    }
    return link->source_error ? MAGPIE_PAUSED : MAGPIE_LIVE;
}

void magpie_format_tokens(uint64_t value, char formatted[MAGPIE_TOKEN_TEXT_CAPACITY])
{
    char *end = formatted + MAGPIE_TOKEN_TEXT_CAPACITY - 1;
    char *start = end;
    *end = '\0';
    unsigned digits = 0;
    do {
        if (digits == 3) {
            *--start = ',';
            digits = 0;
        }
        *--start = (char)('0' + value % 10);
        value /= 10;
        ++digits;
    } while (value != 0);
    memmove(formatted, start, (size_t)(end - start) + 1);
}
