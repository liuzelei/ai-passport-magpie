#include "magpie_protocol.h"

#include <assert.h>
#include <string.h>

static unsigned feed(magpie_stream_t *stream, magpie_link_t *link,
                     const char *bytes, uint64_t now_ms)
{
    unsigned accepted = 0;
    for (size_t i = 0; bytes[i] != '\0'; ++i) {
        accepted += magpie_stream_receive(stream, link, (uint8_t)bytes[i], now_ms) == MAGPIE_REPLY_ACK;
    }
    return accepted;
}

static void test_status_and_snapshots(void)
{
    magpie_stream_t stream = {0};
    magpie_link_t link = {0};
    assert(magpie_status(&link, 500000) == MAGPIE_WAITING);
    assert(!link.has_usage);

    assert(feed(&stream, &link, "\nMPU1,ERR\n", 500001) == 1);
    assert(magpie_status(&link, 500001) == MAGPIE_PAUSED);
    assert(!link.has_usage);

    assert(feed(&stream, &link, "MPU1,OK,06:09,0,0,0,0", 500004) == 0);
    assert(!link.has_usage);
    assert(feed(&stream, &link, ",7,30\r\n", 500005) == 1);
    assert(link.has_usage);
    assert(magpie_status(&link, 500005) == MAGPIE_LIVE);
    assert(strcmp(link.usage.synced_at, "06:09") == 0);
    assert(link.usage.today_total == 0);
    assert(link.usage.last_7_days == 7);
    assert(link.usage.last_30_days == 30);
    assert(magpie_status(&link, 590005) == MAGPIE_LIVE);
    assert(magpie_status(&link, 590006) == MAGPIE_STALE);

    assert(feed(&stream, &link, "MPU1,ERR\r\n", 590007) == 1);
    assert(magpie_status(&link, 590007) == MAGPIE_PAUSED);
    assert(link.has_usage && link.usage.last_30_days == 30);
    assert(magpie_status(&link, 680008) == MAGPIE_STALE);

    assert(feed(&stream, &link, "MPU1,OK,23:59,400000023,7,4,400000012,500000024,600000025\n", 690000) == 1);
    assert(magpie_status(&link, 690000) == MAGPIE_LIVE);
    assert(strcmp(link.usage.synced_at, "23:59") == 0);
    assert(link.usage.today_total == 400000023);
    assert(link.usage.today_input == 7 && link.usage.today_output == 4);
    assert(link.usage.today_cache == 400000012);
    assert(link.usage.last_7_days == 500000024 && link.usage.last_30_days == 600000025);
}

static void test_full_unsigned_range(void)
{
    magpie_stream_t stream = {0};
    magpie_link_t link = {0};
    const char *maximum_frame = "MPU1,OK,00:00,18446744073709551615,18446744073709551615,"
                                "18446744073709551615,18446744073709551615,"
                                "18446744073709551615,18446744073709551615\r\n";
    assert(feed(&stream, &link, maximum_frame, 11) == 1);
    assert(link.usage.today_total == UINT64_MAX);
    assert(link.usage.today_input == UINT64_MAX);
    assert(link.usage.today_output == UINT64_MAX);
    assert(link.usage.today_cache == UINT64_MAX);
    assert(link.usage.last_7_days == UINT64_MAX);
    assert(link.usage.last_30_days == UINT64_MAX);
}

static void test_malformed_frames_do_not_update_and_can_resync(void)
{
    magpie_stream_t stream = {0};
    magpie_link_t link = {0};
    assert(feed(&stream, &link, "MPU1,OK,12:34,3,2,1,4,5,6\n", 10) == 1);
    const char *malformed[] = {
        "W (808) USB console message\n",
        "MPU0,ERR\n", "MPU1,ERR,\n", "MPU1,ERR\r\r\n", "MPU1,ERT\n",
        "MPU1,OK,12:34,3,2,1,4,5\n", "MPU1,OK,12:34,3,2,1,4,5,6,7\n",
        "MPU1,OK,12:34,3,2,1,,5,6\n", "MPU1,OK,12:34,3,2,1,4,5,\n",
        "MPU1,OK,12:34,3,2,1,4,5,18446744073709551616\n",
        "MPU1,OK,12:34,3,2,1,4,5,0a\n", "MPU1,OK,12:34,3,2,1,4,5,-1\n",
        "MPU1,OK,12:34,3,2,1,4,5,+1\n", "MPU1,OK,12:34,3,2,1,4,5, 6\n",
        "MPU1,OK,24:00,3,2,1,4,5,6\n", "MPU1,OK,12:60,3,2,1,4,5,6\n",
        "MPU1,OK,2:34,3,2,1,4,5,6\n", "MPU1,OK,12:34:00,3,2,1,4,5,6\n",
        "MPU1,O\rK,12:34,3,2,1,4,5,6\n",
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        assert(feed(&stream, &link, malformed[i], 20) == 0);
    }
    assert(feed(&stream, &link, "MPU1,ERR", 20) == 0);
    assert(!magpie_stream_receive(&stream, &link, '\0', 21));
    assert(feed(&stream, &link, "\n", 21) == 0);
    for (unsigned i = 0; i <= MAGPIE_FRAME_CAPACITY; ++i) {
        assert(!magpie_stream_receive(&stream, &link, 'x', 21));
    }
    assert(feed(&stream, &link, "MPU1,ERR\n", 21) == 0);
    assert(link.last_received_ms == 10);
    assert(link.usage.last_30_days == 6);
    assert(magpie_status(&link, 90011) == MAGPIE_STALE);
    assert(feed(&stream, &link, "\nMPU1,ERR\nMPU1,OK,21:15,0,0,0,0,0,0\n", 90012) == 2);
    assert(magpie_status(&link, 90012) == MAGPIE_LIVE);
    assert(link.has_usage && link.usage.last_30_days == 0);
}

static void test_probe_does_not_change_state(void)
{
    magpie_stream_t stream = {0};
    magpie_link_t link = {0};
    assert(feed(&stream, &link, "MPU1,PING\r", 1) == 0);
    assert(magpie_stream_receive(&stream, &link, '\n', 1) == MAGPIE_REPLY_PONG);
    assert(!link.has_received && !link.has_usage);
    assert(magpie_status(&link, 1) == MAGPIE_WAITING);

    assert(feed(&stream, &link, "MPU1,OK,14:12,6,3,3,1,6,6\n", 2) == 1);
    assert(feed(&stream, &link, "MPU1,PING", 100000) == 0);
    assert(magpie_stream_receive(&stream, &link, '\n', 100000) == MAGPIE_REPLY_PONG);
    assert(magpie_status(&link, 100000) == MAGPIE_STALE);
    assert(link.last_received_ms == 2 && link.usage.today_total == 6);
}

static void test_disconnect_drops_partial_frame(void)
{
    magpie_stream_t stream = {0};
    magpie_link_t link = {0};
    assert(feed(&stream, &link, "MPU1,OK,08:22,5,3,2,9,12,13\n", 10) == 1);

    /* An ATT write can end inside a field. A new connection cannot finish it. */
    assert(feed(&stream, &link, "MPU1,OK,12:34,99,9,90,", 100) == 0);
    magpie_stream_reset(&stream);
    assert(feed(&stream, &link, "1,2,3\n", 150) == 0);
    assert(link.has_usage && link.usage.today_total == 5 && link.last_received_ms == 10);
    assert(feed(&stream, &link, "\nMPU1,OK,21:15,0,0,0,0,0,0\n", 155) == 1);
    assert(link.usage.today_total == 0 && link.last_received_ms == 155);
}

static void test_token_format(void)
{
    char formatted[MAGPIE_TOKEN_TEXT_CAPACITY];
    magpie_format_tokens(0, formatted);
    assert(strcmp(formatted, "0") == 0);
    magpie_format_tokens(1700, formatted);
    assert(strcmp(formatted, "1,700") == 0);
    magpie_format_tokens(UINT64_MAX, formatted);
    assert(strcmp(formatted, "18,446,744,073,709,551,615") == 0);
}

int main(void)
{
    test_status_and_snapshots();
    test_full_unsigned_range();
    test_malformed_frames_do_not_update_and_can_resync();
    test_probe_does_not_change_state();
    test_disconnect_drops_partial_frame();
    test_token_format();
    return 0;
}
