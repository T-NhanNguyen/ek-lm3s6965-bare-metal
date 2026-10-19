#include <stdint.h>
volatile uint32_t *fixture_word(uint32_t address, int writing);
volatile const uint8_t *fixture_byte(uint32_t address);
