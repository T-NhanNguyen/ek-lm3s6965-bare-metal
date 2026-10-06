#include "ethernet_raw_capture.h"
#include <stddef.h>
#include <string.h>

bool raw_capture_match(const uint8_t *buffer, size_t length,
                       const uint8_t host[6], const raw_message_t *expected,
                       bool *matched)
{
    *matched = false;
    bool found = false;
    size_t offset = 0u;
    while (offset < length)
    {
        const size_t remaining = length - offset;
        struct bpf_hdr header;
        if (remaining < sizeof header) { return false; }
        memcpy(&header, buffer + offset, sizeof header);
        /* Darwin Ethernet bh_hdrlen may be 18, while sizeof bpf_hdr is 20:
         * trailing C struct padding is not a required on-wire header field.
         * The memcpy above may copy two frame bytes into that padding. */
        const size_t fields = offsetof(struct bpf_hdr, bh_hdrlen) +
            sizeof header.bh_hdrlen;
        const size_t hlen = header.bh_hdrlen;
        const size_t caplen = header.bh_caplen;
        if (hlen < fields || hlen > remaining ||
            caplen > remaining - hlen || caplen > header.bh_datalen)
        {
            return false;
        }
        const size_t record = hlen + caplen;
        if (record > SIZE_MAX - (BPF_ALIGNMENT - 1u)) { return false; }
        const size_t aligned = BPF_WORDALIGN(record);
        /* The last record need not include its inter-record alignment pad. */
        if (aligned > remaining && record != remaining) { return false; }
        raw_message_t message;
        if (caplen == RAW_FRAME_BYTES &&
            header.bh_datalen == RAW_FRAME_BYTES &&
            raw_parse_reply(buffer + offset + hlen, caplen, host, &message) &&
            message.sequence == expected->sequence &&
            memcmp(message.nonce, expected->nonce, RAW_NONCE_BYTES) == 0)
        {
            found = true;
        }
        offset += aligned <= remaining ? aligned : record;
    }
    *matched = found;
    return true;
}

void raw_reply_filter(struct bpf_insn out[RAW_FILTER_INSNS],
                      const uint8_t reply[RAW_FRAME_BYTES])
{
    out[0] = (struct bpf_insn)BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0);
    out[1] = (struct bpf_insn)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                     RAW_FRAME_BYTES, 0, 31);
    for (size_t i = 0u; i < RAW_FRAME_BYTES / 4u; ++i)
    {
        const size_t p = i * 4u;
        const uint32_t word = ((uint32_t)reply[p] << 24) |
            ((uint32_t)reply[p + 1u] << 16) |
            ((uint32_t)reply[p + 2u] << 8) | reply[p + 3u];
        const size_t instruction = 2u + i * 2u;
        out[instruction] = (struct bpf_insn)BPF_STMT(
            BPF_LD | BPF_W | BPF_ABS, (uint32_t)p);
        out[instruction + 1u] = (struct bpf_insn)BPF_JUMP(
            BPF_JMP | BPF_JEQ | BPF_K, word, 0,
            (uint8_t)(31u - instruction));
    }
    out[32] = (struct bpf_insn)BPF_STMT(BPF_RET | BPF_K, RAW_FRAME_BYTES);
    out[33] = (struct bpf_insn)BPF_STMT(BPF_RET | BPF_K, 0);
}
