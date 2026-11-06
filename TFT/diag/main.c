/*
 * JZ-TS35 V2.1 - Stage 2D.1 ILI9486L bring-up (wiring-corrected)
 * MCU: STM32F207VCT6
 * LCD: 3.5in 320x480, ILI9486L, 40-pin FPC
 * Interface: FSMC Bank1 NORSRAM1, 16-bit 8080 parallel
 *
 * FPC mapping confirmed by user's continuity measurements:
 *   FPC9  CSX   -> PD7   FSMC_NE1
 *   FPC10 D/CX  -> PD11  FSMC_A16
 *   FPC11 WRX   -> PD5   FSMC_NWE
 *   FPC12 RDX   -> PD4   FSMC_NOE
 *   FPC15 RESX  -> PD13  GPIO output (active low)
 *   FPC17..20 DB0..DB3 -> PD14, PD15, PD0, PD1
 *   FPC21..29 DB4..DB12 -> PE7..PE15
 *   FPC30..32 DB13..DB15 -> PD8..PD10
 *
 * CPU address mapping for a 16-bit FSMC bus with RS on A16:
 *   command: 0x60000000
 *   data:    0x60020000
 *
 * This diagnostic keeps SYSCLK on reset-default HSI (16 MHz).
 * It does not touch the external W25Q32, SD, touch controller or UART.
 * Backlight: JZ-TS35 Q1 switch, active-high PA1, enabled before LCD init.
 */

#include "stm32f2xx.h"
#include <stdint.h>

#define LCD_CMD_ADDR ((volatile uint16_t *)0x60000000UL)
#define LCD_DAT_ADDR ((volatile uint16_t *)0x60020000UL)
#define LCD_CMD(v)   (*LCD_CMD_ADDR = (uint16_t)(v))
#define LCD_DATA(v)  (*LCD_DAT_ADDR = (uint16_t)(v))

static volatile uint32_t g_ms;
static volatile uint32_t g_stage2d_status;
static volatile uint32_t g_pixels_written;
static volatile uint32_t g_lcd_reset_count;

enum {
    ST_BOOT       = 0x424F4F54UL,
    ST_FSMC_READY = 0x46534D43UL,
    ST_RESET_DONE = 0x52535444UL,
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


static void backlight_init(void)
{
    /* JZ-TS35 Stage 2F: PA1 drives Q1 gate, active-high. */
    RCC->AHB1ENR |= (1UL << 0); /* GPIOA clock */
    (void)RCC->AHB1ENR;
    GPIOA->MODER = (GPIOA->MODER & ~(3UL << (1U * 2U))) |
                   (1UL << (1U * 2U)); /* PA1 output */
    GPIOA->OTYPER &= ~(1UL << 1U);
    GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~(3UL << (1U * 2U))) |
                     (2UL << (1U * 2U));
    GPIOA->PUPDR &= ~(3UL << (1U * 2U));
    GPIOA->BSRRL = (uint16_t)(1UL << 1U); /* Backlight ON */
    __DSB();
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
    /* FPC15 -> PD13. Keep RESET low until the FSMC pins and bank are ready. */
    RCC->AHB1ENR |= (1UL << 3);
    (void)RCC->AHB1ENR;

    GPIOD->MODER = (GPIOD->MODER & ~(3UL << (13U * 2U))) |
                   (1UL << (13U * 2U));
    GPIOD->OTYPER &= ~(1UL << 13U);
    GPIOD->OSPEEDR = (GPIOD->OSPEEDR & ~(3UL << (13U * 2U))) |
                     (2UL << (13U * 2U));
    GPIOD->PUPDR &= ~(3UL << (13U * 2U));
    GPIOD->BSRRH = (uint16_t)(1UL << 13U); /* Assert LCD reset low. */
}

static void lcd_hardware_reset(void)
{
    GPIOD->BSRRH = (uint16_t)(1UL << 13U); /* RESX low */
    delay_ms(20U);
    GPIOD->BSRRL = (uint16_t)(1UL << 13U);         /* RESX high */
    delay_ms(120U);
    ++g_lcd_reset_count;
}

static void fsmc_gpio_init(void)
{
    RCC->AHB1ENR |= (1UL << 3) | (1UL << 4); /* GPIOD/GPIOE */
    (void)RCC->AHB1ENR;

    /* FSMC D0..D3: PD14, PD15, PD0, PD1. */
    gpio_af12(GPIOD, 14); gpio_af12(GPIOD, 15);
    gpio_af12(GPIOD, 0);  gpio_af12(GPIOD, 1);

    /* FSMC D4..D12: PE7..PE15. */
    gpio_af12(GPIOE, 7);  gpio_af12(GPIOE, 8);
    gpio_af12(GPIOE, 9);  gpio_af12(GPIOE, 10);
    gpio_af12(GPIOE, 11); gpio_af12(GPIOE, 12);
    gpio_af12(GPIOE, 13); gpio_af12(GPIOE, 14);
    gpio_af12(GPIOE, 15);

    /* FSMC D13..D15: PD8, PD9, PD10. */
    gpio_af12(GPIOD, 8); gpio_af12(GPIOD, 9);
    gpio_af12(GPIOD, 10);

    /* Control: PD4=NOE, PD5=NWE, PD7=NE1, PD11=A16/RS. */
    gpio_af12(GPIOD, 4); gpio_af12(GPIOD, 5);
    gpio_af12(GPIOD, 7); gpio_af12(GPIOD, 11);
}

static void fsmc_init(void)
{
    volatile uint32_t * const FSMC_BCR1  = (volatile uint32_t *)0xA0000000UL;
    volatile uint32_t * const FSMC_BTR1  = (volatile uint32_t *)0xA0000004UL;
    volatile uint32_t * const FSMC_BWTR1 = (volatile uint32_t *)0xA0000104UL;

    RCC->AHB3ENR |= 1UL;
    (void)RCC->AHB3ENR;

    *FSMC_BCR1 = 0UL;
    *FSMC_BTR1 = (15UL << 8);  /* Conservative asynchronous read timing. */
    *FSMC_BWTR1 = (7UL << 8);  /* Conservative asynchronous write timing. */
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
    delay_ms(120U);
    LCD_CMD(0x11); /* Sleep out */
    delay_ms(120U);

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
    lcd_write_cmd_data(0x36, 0x28); /* Preserve previous orientation/BGR setting */
    LCD_CMD(0x29);                 /* Display on */
    delay_ms(150U);
}

/*
 * Printer UART diagnostic: USART3_TX on PB10 (AF7).
 * Reset-default clock tree: HSI 16 MHz, APB1 = 16 MHz.
 * 115200 baud, 8 data bits, no parity, 1 stop bit.
 * Only sends a Marlin-compatible comment line; it is not a G-code command.
 */
static void printer_uart_init(void)
{
    RCC->AHB1ENR |= (1UL << 1);  /* GPIOB clock */
    RCC->APB1ENR |= (1UL << 18); /* USART3 clock */
    (void)RCC->APB1ENR;

    /* PB10 = USART3_TX, alternate function 7. */
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << (10U * 2U))) |
                   (2UL << (10U * 2U));
    GPIOB->OTYPER &= ~(1UL << 10U);
    GPIOB->OSPEEDR = (GPIOB->OSPEEDR & ~(3UL << (10U * 2U))) |
                     (2UL << (10U * 2U));
    GPIOB->PUPDR &= ~(3UL << (10U * 2U));
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(15UL << 8U)) | (7UL << 8U);

    USART3->CR1 = 0U;
    USART3->CR2 = 0U;
    USART3->CR3 = 0U;
    USART3->BRR = 139U; /* 16 MHz / 115200, rounded */
    USART3->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void printer_uart_send_test_line(void)
{
    static const char message[] = ";JZ-TS35 USART3 TX TEST\r\n";
    uint32_t i;
    for (i = 0U; i < sizeof(message) - 1U; ++i) {
        while ((USART3->SR & USART_SR_TXE) == 0U) __NOP();
        USART3->DR = (uint16_t)message[i];
    }
    while ((USART3->SR & USART_SR_TC) == 0U) __NOP();
}

static void lcd_fill(uint16_t color)
{
    uint32_t i;

    LCD_CMD(0x2A);
    LCD_DATA(0x0000); LCD_DATA(0x0000);
    LCD_DATA(0x0001); LCD_DATA(0x003F); /* X: 0..319 */

    LCD_CMD(0x2B);
    LCD_DATA(0x0000); LCD_DATA(0x0000);
    LCD_DATA(0x0001); LCD_DATA(0x00DF); /* Y: 0..479 */

    LCD_CMD(0x2C);
    for (i = 0; i < (320UL * 480UL); ++i) {
        LCD_DATA(color);
        ++g_pixels_written;
    }
}

int main(void)
{
    g_stage2d_status = ST_BOOT;
    backlight_init(); /* Turn on backlight before touching the LCD bus. */

    if (SysTick_Config(HSI_VALUE / 1000UL) != 0UL) {
        for (;;) __NOP();
    }

    lcd_reset_gpio_init();
    fsmc_gpio_init();
    fsmc_init();
    g_stage2d_status = ST_FSMC_READY;

    lcd_hardware_reset();
    g_stage2d_status = ST_RESET_DONE;

    lcd_init();
    g_stage2d_status = ST_LCD_INIT;

    /* Red -> green -> blue -> black, one second per screen. */
    lcd_fill(0xF800);
    delay_ms(1000U);
    lcd_fill(0x07E0);
    delay_ms(1000U);
    lcd_fill(0x001F);
    delay_ms(1000U);
    lcd_fill(0x0000);

    g_stage2d_status = ST_FILL_DONE;

    /* Add only printer TX after the already-verified LCD sequence. */
    printer_uart_init();
    for (;;) {
        printer_uart_send_test_line();
        delay_ms(1000U);
    }
}
