/* SSD1329 panel initialization and windowed pixel transport. */
#include "oled.h"

#include <stddef.h>
#include <stdint.h>

#include "gpio.h"
#include "memory_map.h"
#include "ssi.h"
#include "system_control.h"

#define OLED_SSI_PIN_MASK ((1u << 2) | (1u << 3) | (1u << 5))
#define OLED_POWER_PIN_MASK (1u << 6)
#define OLED_DC_PIN_MASK (1u << 7)

enum
{
    OLED_BITRATE_HZ = 1000000,
    OLED_COLUMNS_PER_ROW = 64,
    OLED_ROW_COUNT = 96,
    /* Provisional bright polarity; change to 0x0u to invert the artwork. */
    OLED_IMAGE_ONE_LEVEL = 0xFu,
    OLED_FRAME_BYTES = OLED_COLUMNS_PER_ROW * OLED_ROW_COUNT
};

/* Command parameters also use D/C low, as does the recovered init sequence. */
static void oled_write_command(uint8_t byte)
{
    ssi0_wait_idle();
    REGISTER32(GPIO_PORT_C_BASE_ADDRESS +
               (OLED_DC_PIN_MASK << GPIO_ADDRESS_MASK_SHIFT)) = 0u;
    ssi0_write_byte(byte);
}

/* Wait before each D/C write so no in-flight byte can change its framing. */
static void oled_write_data(uint8_t byte)
{
    ssi0_wait_idle();
    REGISTER32(GPIO_PORT_C_BASE_ADDRESS +
               (OLED_DC_PIN_MASK << GPIO_ADDRESS_MASK_SHIFT)) = OLED_DC_PIN_MASK;
    ssi0_write_byte(byte);
}

/* Bounds are inclusive; columns address pairs of pixels, rows single pixels. */
static void oled_set_window(uint8_t column_start, uint8_t column_end,
                            uint8_t row_start, uint8_t row_end)
{
    oled_write_command(0x15u);
    oled_write_command(column_start);
    oled_write_command(column_end);
    oled_write_command(0x75u);
    oled_write_command(row_start);
    oled_write_command(row_end);
    oled_write_command(0xA0u);
    oled_write_command(0x52u); /* Horizontal address increment. */
}

/* Enable panel power and transmit the exact recovery sequence from docs/oled.md. */
void oled_initialize(uint32_t system_clock_hz)
{
    static const uint8_t init_sequence[] =
    {
        0xFDu, 0x12u, 0xAEu, 0x94u, 0x00u, 0xA8u, 0x5Fu,
        0x81u, 0xB7u, 0x82u, 0x3Fu, 0xA0u, 0x52u, 0xA1u, 0x00u,
        0xA2u, 0x00u, 0xA4u, 0xB1u, 0x11u, 0xB2u, 0x23u, 0xB3u, 0xE2u,
        0xB8u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x08u, 0x0Au,
        0x0Cu, 0x0Eu, 0x10u, 0x13u, 0x16u, 0x1Au, 0x1Eu,
        0xBBu, 0x01u, 0xBCu, 0x3Fu, 0xAFu
    };

    system_control_enable_peripheral_clock(SYSTEM_CONTROL_RCGC2_OFFSET,
                                           RCGC2_GPIOA_BIT | RCGC2_GPIOC_BIT);
    /* Gate writes are read back by system control; allow three system clocks. */
    __asm__ volatile ("nop\n\tnop\n\tnop");

    gpio_select_alternate_function(GPIO_PORT_A_BASE_ADDRESS, OLED_SSI_PIN_MASK);
    gpio_enable_digital_function(GPIO_PORT_A_BASE_ADDRESS, OLED_SSI_PIN_MASK);
    gpio_configure_output(GPIO_PORT_C_BASE_ADDRESS,
                          OLED_POWER_PIN_MASK | OLED_DC_PIN_MASK);
    REGISTER32(GPIO_PORT_C_BASE_ADDRESS +
               (OLED_POWER_PIN_MASK << GPIO_ADDRESS_MASK_SHIFT)) =
        OLED_POWER_PIN_MASK;

    ssi0_initialize(system_clock_hz, OLED_BITRATE_HZ);
    for (size_t index = 0u; index < sizeof(init_sequence); index++)
    {
        oled_write_command(init_sequence[index]);
    }
    ssi0_wait_idle();
}

/* Stream a full-width 1 bpp image at row zero, then black bottom rows. */
void oled_draw_image(const uint8_t *image, uint16_t image_width,
                     uint16_t image_height, uint8_t bytes_per_row)
{
    if (image == NULL || image_width != OLED_COLUMNS_PER_ROW * 2u ||
        image_height > OLED_ROW_COUNT || bytes_per_row < image_width / 8u)
    {
        return;
    }

    oled_set_window(0u, OLED_COLUMNS_PER_ROW - 1u, 0u, OLED_ROW_COUNT - 1u);
    for (uint16_t row = 0u; row < image_height; row++)
    {
        for (uint16_t column = 0u; column < image_width / 8u; column++)
        {
            uint8_t source = image[(size_t)row * bytes_per_row + column];
            for (uint8_t pair = 0u; pair < 4u; pair++)
            {
                const uint8_t left = (source & 0x80u) != 0u ?
                    OLED_IMAGE_ONE_LEVEL : (OLED_IMAGE_ONE_LEVEL ^ 0xFu);
                const uint8_t right = (source & 0x40u) != 0u ?
                    OLED_IMAGE_ONE_LEVEL : (OLED_IMAGE_ONE_LEVEL ^ 0xFu);
                oled_write_data((uint8_t)((left << 4) | right));
                source = (uint8_t)(source << 2);
            }
        }
    }
    for (uint16_t row = image_height; row < OLED_ROW_COUNT; row++)
    {
        for (uint8_t column = 0u; column < OLED_COLUMNS_PER_ROW; column++)
        {
            oled_write_data(0x00u);
        }
    }
    ssi0_wait_idle();
}

/* Stream black pixel pairs directly; no MCU framebuffer is needed. */
void oled_clear_screen(void)
{
    oled_set_window(0u, OLED_COLUMNS_PER_ROW - 1u, 0u, OLED_ROW_COUNT - 1u);
    for (size_t index = 0u; index < OLED_FRAME_BYTES; index++)
    {
        oled_write_data(0x00u);
    }
    ssi0_wait_idle();
}
