/*
 * JZ-TS35 V2.1 - Stage 2D.2 observable ILI9486L/FSMC diagnostic.
 *
 * This target is independent of the frozen Stage 2A.5, 2B and 2C
 * diagnostics. It does not touch external flash, SD, touch, UART or
 * backlight pins. LCD command/data use the confirmed FSMC mapping:
 * command = 0x60000000, data = 0x60020000 (A16 = D/CX).
 */
#include "stm32f2xx.h"
#include <stdint.h>

#define LCD_CMD_ADDR ((volatile uint16_t *)0x60000000UL)
#define LCD_DAT_ADDR ((volatile uint16_t *)0x60020000UL)

volatile uint32_t g_stage_status;
volatile uint32_t g_lcd_reset_count;
volatile uint32_t g_fsmc_write_count;
volatile uint32_t g_lcd_command_count;
volatile uint32_t g_lcd_data_count;
volatile uint32_t g_pixels_written;
volatile uint32_t g_last_lcd_command;
volatile uint32_t g_last_lcd_data;
volatile uint32_t g_error_code;
volatile uint32_t g_heartbeat;
volatile uint32_t g_loop_counter;
volatile uint32_t g_reset_low_odr;
volatile uint32_t g_reset_high_odr;
volatile uint32_t g_gpio_ahb1enr;
volatile uint32_t g_fsmc_ahb3enr;
volatile uint32_t g_fsmc_bcr1;
volatile uint32_t g_fsmc_btr1;
volatile uint32_t g_fsmc_bwtr1;

enum {
    ST_BOOT       = 0x424F4F54UL,
    ST_GPIO_LOW   = 0x47504C4CUL,
    ST_FSMC_READY = 0x46534D43UL,
    ST_RESET_DONE = 0x52535444UL,
    ST_LCD_INIT   = 0x4C434449UL,
    ST_FILL_RED   = 0x52454431UL,
    ST_FILL_GREEN = 0x47524E31UL,
    ST_FILL_BLUE  = 0x424C5531UL,
    ST_FILL_WHITE = 0x57485431UL,
    ST_FILL_BLACK = 0x424C4B31UL,
    ST_DONE       = 0x444F4E45UL
};

static volatile uint32_t g_ms;

void SystemInit(void)
{
    SCB->VTOR = 0x08000000UL;
    __DSB();
    __ISB();
}

void SysTick_Handler(void)
{
    ++g_ms;
    ++g_heartbeat;
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = g_ms;
    while ((uint32_t)(g_ms - start) < ms) {
        ++g_loop_counter;
        __NOP();
    }
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
        port->AFR[1] = (port->AFR[1] & ~(15UL << s)) | (12UL << s);
    }
}

static void lcd_reset_gpio_init(void)
{
    RCC->AHB1ENR |= (1UL << 3);
    g_gpio_ahb1enr = RCC->AHB1ENR;
    GPIOD->MODER = (GPIOD->MODER & ~(3UL << (13U * 2U))) |
                   (1UL << (13U * 2U));
    GPIOD->OTYPER &= ~(1UL << 13U);
    GPIOD->OSPEEDR = (GPIOD->OSPEEDR & ~(3UL << (13U * 2U))) |
                     (2UL << (13U * 2U));
    GPIOD->PUPDR &= ~(3UL << (13U * 2U));
    GPIOD->BSRRH = (uint16_t)(1UL << 13U);
    g_reset_low_odr = GPIOD->ODR;
    g_stage_status = ST_GPIO_LOW;
}

static void lcd_hardware_reset(void)
{
    GPIOD->BSRRH = (uint16_t)(1UL << 13U);
    g_reset_low_odr = GPIOD->ODR;
    delay_ms(20U);
    GPIOD->BSRRL = (uint16_t)(1UL << 13U);
    g_reset_high_odr = GPIOD->ODR;
    delay_ms(120U);
    ++g_lcd_reset_count;
    g_stage_status = ST_RESET_DONE;
}

static void fsmc_gpio_init(void)
{
    RCC->AHB1ENR |= (1UL << 3) | (1UL << 4);
    (void)RCC->AHB1ENR;
    gpio_af12(GPIOD, 14); gpio_af12(GPIOD, 15);
    gpio_af12(GPIOD, 0);  gpio_af12(GPIOD, 1);
    gpio_af12(GPIOE, 7);  gpio_af12(GPIOE, 8);  gpio_af12(GPIOE, 9);
    gpio_af12(GPIOE, 10); gpio_af12(GPIOE, 11); gpio_af12(GPIOE, 12);
    gpio_af12(GPIOE, 13); gpio_af12(GPIOE, 14); gpio_af12(GPIOE, 15);
    gpio_af12(GPIOD, 8);  gpio_af12(GPIOD, 9);  gpio_af12(GPIOD, 10);
    gpio_af12(GPIOD, 4);  gpio_af12(GPIOD, 5);
    gpio_af12(GPIOD, 7);  gpio_af12(GPIOD, 11);
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
    g_gpio_ahb1enr = RCC->AHB1ENR;
    g_fsmc_ahb3enr = RCC->AHB3ENR;
    g_fsmc_bcr1 = *BCR1;
    g_fsmc_btr1 = *BTR1;
    g_fsmc_bwtr1 = *BWTR1;
    g_stage_status = ST_FSMC_READY;
}

static void lcd_cmd(uint8_t value)
{
    ++g_fsmc_write_count;
    ++g_lcd_command_count;
    g_last_lcd_command = value;
    *LCD_CMD_ADDR = value;
}

static void lcd_data(uint16_t value)
{
    ++g_fsmc_write_count;
    ++g_lcd_data_count;
    g_last_lcd_data = value;
    *LCD_DAT_ADDR = value;
}

static void lcd_cmd_data(uint8_t cmd, uint8_t data)
{
    lcd_cmd(cmd);
    lcd_data(data);
}

static void lcd_init(void)
{
    lcd_cmd(0x01); delay_ms(120U);
    lcd_cmd(0x11); delay_ms(120U);
    lcd_cmd_data(0x3A, 0x55);
    lcd_cmd(0xC0); lcd_data(0x0E); lcd_data(0x0E);
    lcd_cmd(0xC1); lcd_data(0x41); lcd_data(0x00);
    lcd_cmd_data(0xC2, 0x55);
    lcd_cmd(0xC5); lcd_data(0x00); lcd_data(0x00); lcd_data(0x00); lcd_data(0x00);
    lcd_cmd(0xE0);
    lcd_data(0x0F); lcd_data(0x1F); lcd_data(0x1C); lcd_data(0x0C); lcd_data(0x0F);
    lcd_data(0x08); lcd_data(0x48); lcd_data(0x98); lcd_data(0x37); lcd_data(0x0A);
    lcd_data(0x13); lcd_data(0x04); lcd_data(0x11); lcd_data(0x0D); lcd_data(0x00);
    lcd_cmd(0xE1);
    lcd_data(0x0F); lcd_data(0x32); lcd_data(0x2E); lcd_data(0x0B); lcd_data(0x0D);
    lcd_data(0x05); lcd_data(0x47); lcd_data(0x75); lcd_data(0x37); lcd_data(0x06);
    lcd_data(0x10); lcd_data(0x03); lcd_data(0x24); lcd_data(0x20); lcd_data(0x00);
    lcd_cmd_data(0x20, 0x00);
    lcd_cmd_data(0x36, 0x28);
    lcd_cmd(0x29);
    delay_ms(150U);
    g_stage_status = ST_LCD_INIT;
}

static void lcd_fill(uint16_t color, uint32_t stage)
{
    uint32_t i;
    g_stage_status = stage;
    lcd_cmd(0x2A);
    lcd_data(0x0000); lcd_data(0x0000); lcd_data(0x0001); lcd_data(0x003F);
    lcd_cmd(0x2B);
    lcd_data(0x0000); lcd_data(0x0000); lcd_data(0x0001); lcd_data(0x00DF);
    lcd_cmd(0x2C);
    for (i = 0; i < (320UL * 480UL); ++i) {
        lcd_data(color);
        ++g_pixels_written;
    }
}

int main(void)
{
    g_stage_status = ST_BOOT;
    if (SysTick_Config(HSI_VALUE / 1000UL) != 0UL) {
        g_error_code = 1UL;
        for (;;) { ++g_loop_counter; }
    }

    lcd_reset_gpio_init();
    fsmc_gpio_init();
    fsmc_init();
    lcd_hardware_reset();
    lcd_init();

    lcd_fill(0xF800, ST_FILL_RED);   delay_ms(1000U);
    lcd_fill(0x07E0, ST_FILL_GREEN); delay_ms(1000U);
    lcd_fill(0x001F, ST_FILL_BLUE);  delay_ms(1000U);
    lcd_fill(0xFFFF, ST_FILL_WHITE); delay_ms(1000U);
    lcd_fill(0x0000, ST_FILL_BLACK);
    g_stage_status = ST_DONE;

    for (;;) {
        ++g_loop_counter;
        __WFI();
    }
}

