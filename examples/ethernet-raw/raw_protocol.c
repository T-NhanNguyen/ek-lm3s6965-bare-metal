#include "raw_protocol.h"
#include <string.h>

const uint8_t raw_board_mac[6] = {0x02, 0x00, 0x00, 0x69, 0x65, 0x02};
static const uint8_t magic[8] = {'L', 'M', '3', 'S', 'R', 'A', 'W', '1'};

static bool host_valid(const uint8_t *mac)
{
    if (mac == NULL || (mac[0] & 1u) != 0u ||
        memcmp(mac, raw_board_mac, 6u) == 0)
    {
        return false;
    }
    uint8_t nonzero = 0u;
    for (size_t i = 0u; i < 6u; ++i) { nonzero |= mac[i]; }
    return nonzero != 0u;
}

static bool parse(const uint8_t *frame, size_t length, uint8_t opcode,
                  const uint8_t *destination, const uint8_t *source,
                  raw_message_t *out)
{
    if (frame == NULL || length != RAW_FRAME_BYTES ||
        memcmp(frame, destination, 6u) != 0 ||
        memcmp(frame + 6u, source, 6u) != 0 ||
        frame[12] != (RAW_ETHERTYPE >> 8) ||
        frame[13] != (RAW_ETHERTYPE & 0xFFu) ||
        memcmp(frame + 14u, magic, sizeof magic) != 0 ||
        frame[22] != RAW_VERSION || frame[23] != opcode)
    {
        return false;
    }
    for (size_t i = 0u; i < 24u; ++i)
    {
        if (frame[36u + i] != i) { return false; }
    }
    if (out != NULL)
    {
        raw_message_t message;
        message.sequence = ((uint32_t)frame[24] << 24) |
            ((uint32_t)frame[25] << 16) | ((uint32_t)frame[26] << 8) | frame[27];
        memcpy(message.nonce, frame + 28u, RAW_NONCE_BYTES);
        *out = message;
    }
    return true;
}

bool raw_parse_request(const uint8_t *frame, size_t length, raw_message_t *out)
{
    /* Check size before reading any source bytes. */
    return frame != NULL && length == RAW_FRAME_BYTES &&
        host_valid(frame + 6u) &&
        parse(frame, length, RAW_REQUEST, raw_board_mac, frame + 6u, out);
}

bool raw_parse_reply(const uint8_t *frame, size_t length,
                     const uint8_t host_mac[6], raw_message_t *out)
{
    return host_valid(host_mac) &&
        parse(frame, length, RAW_REPLY, host_mac, raw_board_mac, out);
}

bool raw_build_request(uint8_t *out, size_t capacity, const uint8_t host_mac[6],
                       uint32_t sequence, const uint8_t nonce[RAW_NONCE_BYTES])
{
    if (out == NULL || capacity < RAW_FRAME_BYTES || nonce == NULL ||
        !host_valid(host_mac)) { return false; }
    memcpy(out, raw_board_mac, 6u);
    memcpy(out + 6u, host_mac, 6u);
    out[12] = RAW_ETHERTYPE >> 8;
    out[13] = RAW_ETHERTYPE & 0xFFu;
    memcpy(out + 14u, magic, sizeof magic);
    out[22] = RAW_VERSION;
    out[23] = RAW_REQUEST;
    for (size_t i = 0u; i < 4u; ++i)
    {
        out[24u + i] = (uint8_t)(sequence >> (24u - 8u * i));
    }
    memcpy(out + 28u, nonce, RAW_NONCE_BYTES);
    for (size_t i = 0u; i < 24u; ++i) { out[36u + i] = (uint8_t)i; }
    return true;
}

bool raw_build_reply(const uint8_t *request, size_t length,
                     uint8_t *out, size_t capacity)
{
    if (out == NULL || capacity < RAW_FRAME_BYTES ||
        !raw_parse_request(request, length, NULL)) { return false; }
    uint8_t reply[RAW_FRAME_BYTES];
    memcpy(reply, request, sizeof reply);
    memcpy(reply, request + 6u, 6u);
    memcpy(reply + 6u, request, 6u);
    reply[23] = RAW_REPLY;
    memcpy(out, reply, sizeof reply);
    return true;
}
