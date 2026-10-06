/* Pure byte protocol: no BSP, allocation, packed structs or alignment assumptions.
 * Chosen local experiment EtherType 0x88B5; no standards/assignment claim.
 * Exactly 60 bytes DA..payload, excluding preamble and FCS (no extra padding):
 * DA[0:6], SA[6:12], type[12:14], "LM3SRAW1"[14:22], version[22],
 * opcode[23], sequence BE[24:28], nonce[28:36], pattern 00..17[36:60].
 * Host chooses a random eight-byte nonce per run and its actual runtime MAC.
 * There is no authentication, replay protection or delivery guarantee.
 */
#ifndef LM3S6965_RAW_PROTOCOL_H
#define LM3S6965_RAW_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAW_FRAME_BYTES 60u
#define RAW_ETHERTYPE 0x88B5u
#define RAW_VERSION 1u
#define RAW_REQUEST 0x01u
#define RAW_REPLY 0x02u
#define RAW_NONCE_BYTES 8u

/* Locally administered unicast, explicitly test-only, not a production MAC. */
extern const uint8_t raw_board_mac[6]; /* 02:00:00:69:65:02 */
typedef struct
{
    uint32_t sequence;
    uint8_t nonce[RAW_NONCE_BYTES];
} raw_message_t;

/* All inputs may be unaligned. Non-NULL pointers must reference their stated
 * lengths (MAC: six bytes, nonce: eight). No retained pointers or side effects.
 * False leaves output untouched. Parse output may be NULL for validation only.
 * Requests require board DA and a nonzero unicast SA distinct from board.
 * Replies require host DA and board SA, with the same host MAC constraints.
 * Every field except sequence/nonce is checked; all sequence/nonce values valid.
 * Caller must match reply sequence/nonce to its outstanding request. */
bool raw_parse_request(const uint8_t *frame, size_t length, raw_message_t *out);
bool raw_parse_reply(const uint8_t *frame, size_t length,
                     const uint8_t host_mac[6], raw_message_t *out);
/* Builders write exactly 60 bytes on success. Capacity >=60 required.
 * Request input MAC/nonce must not overlap output. Reply may be in-place or
 * overlap its input: it validates and snapshots before writing. Reply changes
 * only DA/SA and opcode. Both builders leave output untouched on failure. */
bool raw_build_request(uint8_t *out, size_t capacity, const uint8_t host_mac[6],
                       uint32_t sequence, const uint8_t nonce[RAW_NONCE_BYTES]);
bool raw_build_reply(const uint8_t *request, size_t length,
                     uint8_t *out, size_t capacity);
#endif
