/* Pure protocol tests, not a MAC/PHY model or evidence of hardware operation.
 * cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   scripts/ethernet/raw/tests/raw_protocol_test.c examples/ethernet-raw/raw_protocol.c \
 *   -o /tmp/raw_protocol_test && /tmp/raw_protocol_test
 */
#include "../../../../examples/ethernet-raw/raw_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t host[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t nonce[8] = {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
static const uint8_t golden[60] = {
    0x00,0x11,0x22,0x33,0x44,0x55, 0x02,0x00,0x00,0x69,0x65,0x02,
    0x88,0xB5, 'L','M','3','S','R','A','W','1', 1,2,
    0x12,0x34,0x56,0x78, 0xFE,0xDC,0xBA,0x98,0x76,0x54,0x32,0x10,
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23
};

static void rejected(const uint8_t *frame, size_t length)
{
    raw_message_t out = {.sequence = 99u, .nonce = {9}};
    raw_message_t original = out;
    uint8_t reply[60], before[60];
    memset(reply, 0xA5, sizeof reply);
    memcpy(before, reply, sizeof before);
    assert(!raw_parse_request(frame, length, &out));
    assert(out.sequence == original.sequence);
    assert(memcmp(out.nonce, original.nonce, sizeof out.nonce) == 0);
    assert(!raw_build_reply(frame, length, reply, sizeof reply));
    assert(memcmp(reply, before, sizeof reply) == 0);
}

int main(void)
{
    uint8_t frame[64], reply[64], bad[64];
    assert(raw_build_request(frame, sizeof frame, host, 0x12345678u, nonce));
    raw_message_t message;
    assert(raw_parse_request(frame, 60u, &message));
    assert(message.sequence == 0x12345678u);
    assert(memcmp(message.nonce, nonce, 8u) == 0);
    memset(reply, 0xA5, sizeof reply);
    assert(raw_build_reply(frame, 60u, reply, sizeof reply));
    assert(memcmp(reply, golden, 60u) == 0);
    assert(reply[60] == 0xA5);
    assert(raw_parse_reply(reply, 60u, host, &message));
    assert(message.sequence == 0x12345678u);
    assert(memcmp(message.nonce, nonce, 8u) == 0);
    rejected(reply, 60u); /* replies never trigger replies */
    for (size_t i = 0u; i < 60u; ++i)
    {
        /* All strict request bytes. SA and arbitrary seq/nonce tested below. */
        if ((i >= 6u && i < 12u) || (i >= 24u && i < 36u)) { continue; }
        memcpy(bad, frame, 60u);
        bad[i] ^= 0x80u;
        rejected(bad, 60u);
    }
    for (size_t length = 0u; length <= 64u; ++length)
    {
        if (length != 60u) { rejected(frame, length); }
    }
    rejected(NULL, 60u);
    rejected(NULL, 0u);
    /* Source: zero, broadcast, multicast, board itself. */
    const uint8_t sources[][6] = {
        {0,0,0,0,0,0}, {255,255,255,255,255,255},
        {1,2,3,4,5,6}, {2,0,0,0x69,0x65,2}
    };
    for (size_t i = 0u; i < sizeof sources / sizeof sources[0]; ++i)
    {
        memcpy(bad, frame, 60u);
        memcpy(bad + 6u, sources[i], 6u);
        rejected(bad, 60u);
        assert(!raw_build_request(reply, 60u, sources[i], 1u, nonce));
        assert(!raw_parse_reply(golden, 60u, sources[i], NULL));
    }
    assert(!raw_parse_reply(golden, 60u, NULL, NULL));
    assert(!raw_parse_reply(NULL, 60u, host, NULL));
    for (size_t i = 0u; i < 60u; ++i)
    {
        if (i >= 24u && i < 36u) { continue; }
        memcpy(bad, golden, 60u);
        bad[i] ^= 1u;
        assert(!raw_parse_reply(bad, 60u, host, NULL));
    }
    for (size_t length = 0u; length <= 64u; ++length)
    {
        if (length != 60u) { assert(!raw_parse_reply(golden, length, host, NULL)); }
    }
    /* Arbitrary sequence and nonce, including all-zero/all-ones, are legal. */
    for (unsigned value = 0u; value <= 255u; ++value)
    {
        memcpy(bad, frame, 60u);
        memset(bad + 24u, (int)value, 12u);
        assert(raw_parse_request(bad, 60u, NULL));
        assert(raw_build_reply(bad, 60u, reply, 60u));
        assert(memcmp(reply + 24u, bad + 24u, 12u) == 0);
    }
    /* Both alignment-independent and overlapping/in-place reply construction. */
    for (size_t offset = 0u; offset < 4u; ++offset)
    {
        uint8_t storage[68];
        assert(raw_build_request(storage + offset, 60u, host, 0x12345678u, nonce));
        assert(raw_parse_request(storage + offset, 60u, NULL));
        assert(raw_build_reply(storage + offset, 60u, storage + offset + 1u, 60u));
        assert(memcmp(storage + offset + 1u, golden, 60u) == 0);
        assert(raw_parse_reply(storage + offset + 1u, 60u, host, NULL));
    }
    memcpy(bad, frame, 60u);
    assert(raw_build_reply(bad, 60u, bad, 60u));
    assert(memcmp(bad, golden, 60u) == 0);
    memset(reply, 0xA5, sizeof reply);
    for (size_t capacity = 0u; capacity < 60u; ++capacity)
    {
        assert(!raw_build_reply(frame, 60u, reply, capacity));
        assert(!raw_build_request(reply, capacity, host, 1u, nonce));
    }
    assert(!raw_build_request(NULL, 60u, host, 1u, nonce));
    assert(!raw_build_request(reply, 60u, NULL, 1u, nonce));
    assert(!raw_build_request(reply, 60u, host, 1u, NULL));
    assert(!raw_build_reply(frame, 60u, NULL, 60u));
    for (size_t i = 0u; i < sizeof reply; ++i) { assert(reply[i] == 0xA5); }
    /* Actual host MAC need not be the fixture: any nonzero unicast works. */
    const uint8_t alternate[6] = {2,9,8,7,6,5};
    assert(raw_build_request(frame, 60u, alternate, 0u, nonce));
    assert(raw_build_reply(frame, 60u, reply, 60u));
    assert(raw_parse_reply(reply, 60u, alternate, NULL));
    assert(!raw_parse_reply(reply, 60u, host, NULL));
    puts("raw protocol tests passed (pure bytes only)");
    return 0;
}
