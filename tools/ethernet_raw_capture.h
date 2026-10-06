/* Native macOS BPF records, not a portable capture-file format. */
#ifndef ETHERNET_RAW_CAPTURE_H
#define ETHERNET_RAW_CAPTURE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <net/bpf.h>

#include "../examples/ethernet-raw/raw_protocol.h"

/* False means malformed records. matched is true only for a complete reply
 * matching the outstanding request; even records after a match are checked. */
bool raw_capture_match(const uint8_t *buffer, size_t length,
                       const uint8_t host[6], const raw_message_t *expected,
                       bool *matched);

/* Exact reply filter: length plus all 60 bytes, including DA/SA, EtherType,
 * opcode, sequence, nonce and full 46-byte payload. No unrelated capture. */
#define RAW_FILTER_INSNS 34u
void raw_reply_filter(struct bpf_insn out[RAW_FILTER_INSNS],
                      const uint8_t reply[RAW_FRAME_BYTES]);
#endif
