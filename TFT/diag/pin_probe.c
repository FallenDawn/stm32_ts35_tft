/*
 * JZ-TS35 V2.1 - Stage 2D.2 LCD readback probe
 * Safe scope: uses ONLY the already-confirmed FSMC/LCD wiring.
 * It does NOT drive unknown FPC pins or the LED backlight pins.
 *
 * FPC33=A (backlight anode), FPC34..36=K (cathodes) is a likely
 * family pinout, not permission to drive them from MCU GPIO.
 */
#include "stm32f2xx.h"
#include <stdint.h>

#define LCD_CMD_ADDR ((volatile uint16_t *)0x60000000UL)
#define LCD_DAT_ADDR ((volatile uint16_t *)0x60020000UL)
#define LCD_CMD(v)   (*LCD_CMD_ADDR = (uint16_t)(v))
#define LCD_DATA(v)  (*LCD_DAT_ADDR = (uint16_t)(v))
#define LCD_READ()   (*LCD_DAT_ADDR)

static volatile uint32_t g_ms;
volatile uint32_t g_stage_status;
volatile uint32_t g_lcd_reset_count;
volatile uint32_t g_fsmc_write_count;
volatile uint32_t g_lcd_command_count;
volatile uint32_t g_lcd_data_count;
volatile uint32_t g_lcd_data_read_count;
volatile uint32_t g_pixels_written;
volatile uint32_t g_last_lcd_command;
volatile uint32_t g_last_lcd_data;
volatile uint32_t g_error_code;
volatile uint32_t g_heartbeat;
volatile uint32_t g_loop_counter;

/* Each row: command, then up to eight raw 16-bit bus read samples. */
volatile uint16_t g_probe_results[4][9];

enum {
    ST_BOOT       = 0x424F4F54UL,
    ST_FSMC_READY = 0x46534D43UL,
    ST_RESET_DONE = 0x52535444UL,
    ST_ID_READ    = 0x49445244UL,
    ST_LCD_INIT   = 0x4C434449UL,
    ST_FILL_DONE  = 0x444F4E45UL
};

void SystemInit(void)
{
    SCB->VTOR = 0x08000000UL;
    __DSB();
    __ISB();
}

void SysTick_Handler(void) { ++g_ms; }

static void delay_ms(uint32_t ms)
{
    uint32_t start = g_ms;
    while ((uint32_t)(g_ms - start) < ms) __NOP();
}

static void gpio_af12(GPIO_TypeDef *port, uint32_t pin)
{
    uint32_t shift = pin * 2U;
    port->MODER = (port->MODER & ~(3UL << shift)) | (2UL << shift);
    port->OTYPER &= ~(1UL << pin);
    port->OSPEEDR = (port->OSPEEDR & ~(3UL << shift)) | (3UL << shift);
    port->PUPDR &= ~(3UL << shift);
    if (pin < 8U) {
        port->AFR[0] = (port->AFR[0] & ~(15UL << (pin * 4U))) |
                       (12UL << (pin * 4U));
    } else {
        uint32_t s = (pin - 8U) * 4U;
        port->AFR[1] = (port->AFR[1] & ~(15UL << s)) |
                       (12UL << s);
    }
}

static void lcd_reset_gpio_init(void)
{
    RCC->AHB1ENR |= (1UL << 3);
    (void)RCC->AHB1ENR;
    GPIOD->MODER = (GPIOD->MODER & ~(3UL << 26)) | (1UL << 26);
    GPIOD->OTYPER &= ~(1UL << 13);
    GPIOD->OSPEEDR = (GPIOD->OSPEEDR & ~(3UL << 26)) | (2UL << 26);
    GPIOD->PUPDR &= ~(3UL << 26);
    GPIOD->BSRRH = (uint16_t)(1UL << 13);
}

static void lcd_hardware_reset(void)
{
    GPIOD->BSRRH = (uint16_t)(1UL << 13);
    delay_ms(20U);
    GPIOD->BSRRL = (uint16_t)(1UL << 13);
    delay_ms(120U);
    ++g_lcd_reset_count;
}

static void fsmc_gpio_init(void)
{
    RCC->AHB1ENR |= (1UL << 3) | (1UL << 4);
    (void)RCC->AHB1ENR;

    gpio_af12(GPIOD,14); gpio_af12(GPIOD,15);
    gpio_af12(GPIOD,0);  gpio_af12(GPIOD,1);
    gpio_af12(GPIOE,7);  gpio_af12(GPIOE,8);
    gpio_af12(GPIOE,9);  gpio_af12(GPIOE,10);
    gpio_af12(GPIOE,11); gpio_af12(GPIOE,12);
    gpio_af12(GPIOE,13); gpio_af12(GPIOE,14);
    gpio_af12(GPIOE,15);
    gpio_af12(GPIOD,8);  gpio_af12(GPIOD,9);
    gpio_af12(GPIOD,10);
    gpio_af12(GPIOD,4);  gpio_af12(GPIOD,5);
    gpio_af12(GPIOD,7);  gpio_af12(GPIOD,11);
}

static void fsmc_init(void)
{
    volatile uint32_t * const BCR1  = (volatile uint32_t *)0xA0000000UL;
    volatile uint32_t * const BTR1  = (volatile uint32_t *)0xA0000004UL;
    volatile uint32_t * const BWTR1 = (volatile uint32_t *)0xA0000104UL;
    RCC->AHB3ENR |= 1UL;
    (void)RCC->AHB3ENR;
    *BCR1 = 0UL;
    *BTR1 = (15UL << 8);
    *BWTR1 = (7UL << 8);
    *BCR1 = (1UL << 14) | (1UL << 12) | (1UL << 4) | 1UL;
    __DSB();
    __ISB();
}

static void lcd_cmd(uint8_t cmd)
{
    LCD_CMD(cmd);
    ++g_fsmc_write_count;
    ++g_lcd_command_count;
    g_last_lcd_command = cmd;
}

static void lcd_data(uint16_t data)
{
    LCD_DATA(data);
    ++g_fsmc_write_count;
    ++g_lcd_data_count;
    g_last_lcd_data = data;
}

static uint16_t lcd_read_data(void)
{
    uint16_t v = LCD_READ();
    ++g_lcd_data_read_count;
    return v;
}

static void probe_register(uint32_t row, uint8_t cmd, uint32_t samples)
{
    uint32_t i;
    g_probe_results[row][0] = cmd;
    lcd_cmd(cmd);
    (void)lcd_read_data(); /* discard one dummy/read-latency cycle */
    for (i = 0; i < samples && i < 8U; ++i) {
        g_probe_results[row][i + 1U] = lcd_read_data();
    }
}

static void lcd_init_minimal(void)
{
    lcd_cmd(0x01);
    delay_ms(120U);
    lcd_cmd(0x11);
    delay_ms(120U);
    lcd_cmd(0x3A); lcd_data(0x55);
    lcd_cmd(0x36); lcd_data(0x28);
    lcd_cmd(0x29);
    delay_ms(150U);
}

static void lcd_fill(uint16_t color)
{
    uint32_t i;
    lcd_cmd(0x2A);
    lcd_data(0x0000); lcd_data(0x0000);
    lcd_data(0x0001); lcd_data(0x003F);
    lcd_cmd(0x2B);
    lcd_data(0x0000); lcd_data(0x0000);
    lcd_data(0x0001); lcd_data(0x00DF);
    lcd_cmd(0x2C);
    for (i = 0; i < 320UL * 480UL; ++i) {
        lcd_data(color);
        ++g_pixels_written;
    }
}

int main(void)
{
    g_stage_status = ST_BOOT;
    if (SysTick_Config(HSI_VALUE / 1000UL) != 0UL) {
        g_error_code = 1UL;
        for (;;) __NOP();
    }

    lcd_reset_gpio_init();
    fsmc_gpio_init();
    fsmc_init();
    g_stage_status = ST_FSMC_READY;

    lcd_hardware_reset();
    g_stage_status = ST_RESET_DONE;

    /* Read-only ID/status probes. Raw 16-bit samples are retained in SRAM. */
    probe_register(0U, 0x04U, 4U); /* Read Display Identification */
    probe_register(1U, 0xD3U, 5U); /* Common ILI9486-family ID read */
    probe_register(2U, 0x09U, 5U); /* Display status */
    probe_register(3U, 0x0AU, 2U); /* Power mode */
    g_stage_status = ST_ID_READ;

    lcd_init_minimal();
    g_stage_status = ST_LCD_INIT;

    lcd_fill(0xF800U); delay_ms(1500U);
    lcd_fill(0x07E0U); delay_ms(1500U);
    lcd_fill(0x001FU); delay_ms(1500U);
    lcd_fill(0xFFFFU); delay_ms(1500U);
    lcd_fill(0x0000U);

    g_stage_status = ST_FILL_DONE;
    for (;;) {
        ++g_loop_counter;
        __WFI();
        ++g_heartbeat;
    }
}
