/*
 * JZ-TS35 V2.1 - Stage 2D ILI9486L bring-up
 * MCU: STM32F207VCT6
 * Interface: FSMC Bank1 NORSRAM1, 16-bit 8080 parallel
 *
 * Assumed data bus is the existing repository mapping. Control pins:
 * PD4=NOE, PD5=NWE, PD7=NE1, PE2=A23(RS).
 * This image deliberately keeps SYSCLK on reset-default HSI (16 MHz).
 * It does not touch the external W25Q32, SD, touch controller or UART.
 */

#include "stm32f2xx.h"
#include <stdint.h>

#define LCD_CMD_ADDR ((volatile uint16_t *)0x60FFFFFEUL)
#define LCD_DAT_ADDR ((volatile uint16_t *)0x61000000UL)
#define LCD_CMD(v)   (*LCD_CMD_ADDR = (uint16_t)(v))
#define LCD_DATA(v)  (*LCD_DAT_ADDR = (uint16_t)(v))

static volatile uint32_t g_ms;
static volatile uint32_t g_stage2d_status;
static volatile uint32_t g_pixels_written;

enum {
    ST_BOOT = 0x424F4F54UL,
    ST_FSMC_READY = 0x46534D43UL,
    ST_LCD_INIT = 0x4C434449UL,
    ST_FILL_DONE = 0x444F4E45UL
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
        port->AFR[1] = (port->AFR[1] & ~(15UL << s)) | (12UL << s);
    }
}

static void fsmc_gpio_init(void)
{
    /* GPIO D/E clocks. */
    RCC->AHB1ENR |= (1UL << 3) | (1UL << 4);
    (void)RCC->AHB1ENR;

    /* FSMC D0..D3: PD14, PD15, PD0, PD1. */
    gpio_af12(GPIOD, 14); gpio_af12(GPIOD, 15);
    gpio_af12(GPIOD, 0);  gpio_af12(GPIOD, 1);

    /* FSMC D4..D11: PE7..PE14. */
    gpio_af12(GPIOE, 7);  gpio_af12(GPIOE, 8);
    gpio_af12(GPIOE, 9);  gpio_af12(GPIOE, 10);
    gpio_af12(GPIOE, 11); gpio_af12(GPIOE, 12);
    gpio_af12(GPIOE, 13); gpio_af12(GPIOE, 14);

    /* FSMC D12..D15: PE15, PD8, PD9, PD10. */
    gpio_af12(GPIOE, 15); gpio_af12(GPIOD, 8);
    gpio_af12(GPIOD, 9);  gpio_af12(GPIOD, 10);

    /* Control: PD4=NOE, PD5=NWE, PD7=NE1, PE2=A23/RS. */
    gpio_af12(GPIOD, 4); gpio_af12(GPIOD, 5);
    gpio_af12(GPIOD, 7); gpio_af12(GPIOE, 2);
}

static void fsmc_init(void)
{
    volatile uint32_t * const FSMC_BCR1  = (volatile uint32_t *)0xA0000000UL;
    volatile uint32_t * const FSMC_BTR1  = (volatile uint32_t *)0xA0000004UL;
    volatile uint32_t * const FSMC_BWTR1 = (volatile uint32_t *)0xA0000104UL;

    RCC->AHB3ENR |= 1UL; /* FSMC clock */
    (void)RCC->AHB3ENR;

    /* Disable bank while programming. */
    *FSMC_BCR1 = 0UL;
    /* Async SRAM, 16-bit, write enabled, extended read/write timing. */
    *FSMC_BTR1 = (15UL << 8); /* read DATAST=15 HCLK */
    *FSMC_BWTR1 = (7UL << 8); /* write DATAST=7 HCLK */
    *FSMC_BCR1 = (1UL << 14) | (1UL << 12) | (1UL << 4) | 1UL;
    __DSB();
    __ISB();
}

static void lcd_write_cmd_data(uint8_t cmd, uint8_t data)
{
    LCD_CMD(cmd);
    LCD_DATA(data);
}

static void lcd_init(void)
{
    LCD_CMD(0x01); /* Software reset */
    delay_ms(120);
    LCD_CMD(0x11); /* Sleep out */
    delay_ms(120);

    lcd_write_cmd_data(0x3A, 0x55); /* RGB565 */
    LCD_CMD(0xC0); LCD_DATA(0x0E); LCD_DATA(0x0E);
    LCD_CMD(0xC1); LCD_DATA(0x41); LCD_DATA(0x00);
    lcd_write_cmd_data(0xC2, 0x55);
    LCD_CMD(0xC5);
    LCD_DATA(0x00); LCD_DATA(0x00); LCD_DATA(0x00); LCD_DATA(0x00);

    LCD_CMD(0xE0);
    LCD_DATA(0x0F); LCD_DATA(0x1F); LCD_DATA(0x1C);
    LCD_DATA(0x0C); LCD_DATA(0x0F); LCD_DATA(0x08);
    LCD_DATA(0x48); LCD_DATA(0x98); LCD_DATA(0x37);
    LCD_DATA(0x0A); LCD_DATA(0x13); LCD_DATA(0x04);
    LCD_DATA(0x11); LCD_DATA(0x0D); LCD_DATA(0x00);

    LCD_CMD(0xE1);
    LCD_DATA(0x0F); LCD_DATA(0x32); LCD_DATA(0x2E);
    LCD_DATA(0x0B); LCD_DATA(0x0D); LCD_DATA(0x05);
    LCD_DATA(0x47); LCD_DATA(0x75); LCD_DATA(0x37);
    LCD_DATA(0x06); LCD_DATA(0x10); LCD_DATA(0x03);
    LCD_DATA(0x24); LCD_DATA(0x20); LCD_DATA(0x00);

    lcd_write_cmd_data(0x20, 0x00); /* Inversion off */
    lcd_write_cmd_data(0x36, 0x28); /* Existing driver orientation/BGR */
    LCD_CMD(0x29); /* Display on */
    delay_ms(150);
}

static void lcd_fill(uint16_t color)
{
    uint32_t i;
    LCD_CMD(0x2A);
    LCD_DATA(0x00); LCD_DATA(0x00);
    LCD_DATA(0x01); LCD_DATA(0x3F); /* 0..319 */
    LCD_CMD(0x2B);
    LCD_DATA(0x00); LCD_DATA(0x00);
    LCD_DATA(0x01); LCD_DATA(0xDF); /* 0..479 */
    LCD_CMD(0x2C);
    for (i = 0; i < (320UL * 480UL); ++i) {
        LCD_DATA(color);
        ++g_pixels_written;
    }
}

int main(void)
{
    g_stage2d_status = ST_BOOT;
    if (SysTick_Config(HSI_VALUE / 1000UL) != 0UL) {
        for (;;) __NOP();
    }

    fsmc_gpio_init();
    fsmc_init();
    g_stage2d_status = ST_FSMC_READY;

    lcd_init();
    g_stage2d_status = ST_LCD_INIT;

    /* Red -> green -> blue -> black, one second per screen. */
    lcd_fill(0xF800);
    delay_ms(1000);
    lcd_fill(0x07E0);
    delay_ms(1000);
    lcd_fill(0x001F);
    delay_ms(1000);
    lcd_fill(0x0000);
    g_stage2d_status = ST_FILL_DONE;

    for (;;) __WFI();
}
