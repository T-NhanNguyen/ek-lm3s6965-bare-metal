#include "lm3s6965/boot_watchdog_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
typedef struct { uint32_t gate; unsigned count, mode; struct { char op; uint32_t value; } events[4]; } model_t;
static void event(model_t *m, char op, uint32_t value) {
    CHECK(m != NULL); CHECK(m->count < 4u);
    m->events[m->count].op = op; m->events[m->count++].value = value;
}
static int read_gate(void *context, uint32_t address, uint32_t *value) {
    model_t *m = context;
    CHECK(address == BOOT_WATCHDOG_RCGC0); CHECK(m->count == 0u || m->count == 2u);
    *value = m->gate;
    if (m->mode == 2u && m->count == 2u) *value ^= UINT32_C(0x100);
    event(m, 'R', *value); return 0;
}
static int write_gate(void *context, uint32_t address, uint32_t value) {
    model_t *m = context;
    CHECK(address == BOOT_WATCHDOG_RCGC0); CHECK(m->count == 1u);
    event(m, 'W', value); if (m->mode != 1u) m->gate = value; return 0;
}
static void delay_gate(void *context) {
    model_t *m = context;
    CHECK(m->mode == 0u); CHECK(m->count == 3u);
    CHECK(m->events[2].value == m->events[1].value); event(m, 'D', 0u);
}
int main(int argc, char **argv) {
    const uint32_t initial = UINT32_C(0xa5000101), expected = initial | UINT32_C(8);
    model_t m = {0}; CHECK(argc == 2);
    if (!strcmp(argv[1], "gate-order")) m.mode = 0u;
    else if (!strcmp(argv[1], "gate-ignored")) m.mode = 1u;
    else { CHECK(!strcmp(argv[1], "gate-exact")); m.mode = 2u; }
    m.gate = initial;
    boot_watchdog_io_t io = {&m, read_gate, write_gate};
    const boot_watchdog_io_t before = io;
    const bool ready = boot_watchdog_target_clock_ready(&io, delay_gate);
    CHECK(io.context == before.context); CHECK(io.read_register == before.read_register);
    CHECK(io.write_register == before.write_register); CHECK(ready == (m.mode == 0u));
    CHECK(m.count == (m.mode == 0u ? 4u : 3u));
    CHECK(m.events[0].op == 'R'); CHECK(m.events[0].value == initial);
    CHECK(m.events[1].op == 'W'); CHECK(m.events[1].value == expected);
    CHECK(m.events[2].op == 'R');
    CHECK(m.events[2].value == (m.mode == 1u ? initial : m.mode == 2u ? expected ^ UINT32_C(0x100) : expected));
    CHECK(m.gate == (m.mode == 1u ? initial : expected));
    if (m.mode == 0u) CHECK(m.events[3].op == 'D');
    return 0;
}
