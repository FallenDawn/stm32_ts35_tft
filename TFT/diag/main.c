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

/* Active-low P-MOS high-side buzzer switch:
 * PA8 drives the P-MOS gate through R3. For a conventional P-channel
 * high-side stage, PA8 LOW turns the buzzer ON; PA8 HIGH turns it OFF.
 */
static void buzzer_init(void)
{
    RCC->AHB1ENR |= (1UL << 0); /* GPIOA clock */
    (void)RCC->AHB1ENR;
    GPIOA->BSRRH = (uint16_t)(1UL << 8U); /* preload low before output */
    GPIOA->MODER = (GPIOA->MODER & ~(3UL << 16U)) | (1UL << 16U);
    GPIOA->OTYPER &= ~(1UL << 8U);
    GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~(3UL << 16U)) | (1UL << 16U);
    GPIOA->PUPDR &= ~(3UL << 16U);
    GPIOA->BSRRL = (uint16_t)(1UL << 8U); /* OFF: P-MOS gate high */
    __DSB();
}

static void buzzer_click(void)
{
    /* Hold the active buzzer on longer for a more prominent confirmation.
     * Firmware cannot raise its supply voltage or physical sound-pressure level.
     */
    GPIOA->BSRRH = (uint16_t)(1UL << 8U); /* ON */
    delay_ms(250U);
    GPIOA->BSRRL = (uint16_t)(1UL << 8U); /* OFF */
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
    lcd_write_cmd_data(0x36, 0x28); /* Landscape 480x320: MV=1, BGR=1 */
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

/*
 * W25Q32 SPI2-pin GPIO bit-bang diagnostic.
 * Confirmed board wiring: PB12=/CS, PB13=SCK, PB14=MISO, PB15=MOSI.
 * SPI mode 0, MSB first. JEDEC ID command 0x9F; W25Q32JV expected EF 40 16.
 */
static void w25q32_gpio_init(void)
{
    RCC->AHB1ENR |= (1UL << 1); /* GPIOB clock */
    (void)RCC->AHB1ENR;

    /* PB12 /CS, PB13 SCK, PB15 MOSI outputs; PB14 MISO input. */
    GPIOB->MODER &= ~((3UL << 24U) | (3UL << 26U) |
                      (3UL << 28U) | (3UL << 30U));
    GPIOB->MODER |=  ((1UL << 24U) | (1UL << 26U) | (1UL << 30U));
    GPIOB->OTYPER &= ~((1UL << 12U) | (1UL << 13U) |
                       (1UL << 15U));
    GPIOB->OSPEEDR |= ((3UL << 24U) | (3UL << 26U) |
                       (3UL << 28U) | (3UL << 30U));
    GPIOB->PUPDR &= ~((3UL << 24U) | (3UL << 26U) |
                      (3UL << 28U) | (3UL << 30U));

    GPIOB->BSRRL = (uint16_t)((1UL << 12U) | (1UL << 15U));
    GPIOB->BSRRH = (uint16_t)(1UL << 13U); /* SCK idle low */
    __DSB();
}

static uint8_t w25q32_spi_transfer(uint8_t tx)
{
    uint8_t rx = 0U;
    uint32_t bit;

    for (bit = 0U; bit < 8U; ++bit) {
        if ((tx & 0x80U) != 0U) GPIOB->BSRRL = (uint16_t)(1UL << 15U);
        else                    GPIOB->BSRRH = (uint16_t)(1UL << 15U);
        __NOP(); __NOP();

        GPIOB->BSRRL = (uint16_t)(1UL << 13U); /* rising edge */
        __NOP(); __NOP();
        rx = (uint8_t)((rx << 1U) |
             ((GPIOB->IDR & (1UL << 14U)) != 0U ? 1U : 0U));
        GPIOB->BSRRH = (uint16_t)(1UL << 13U); /* falling edge */
        __NOP(); __NOP();
        tx <<= 1U;
    }
    return rx;
}

static void w25q32_read_jedec_id(uint8_t id[3])
{
    GPIOB->BSRRH = (uint16_t)(1UL << 12U); /* CS low */
    (void)w25q32_spi_transfer(0x9FU);
    id[0] = w25q32_spi_transfer(0xFFU);
    id[1] = w25q32_spi_transfer(0xFFU);
    id[2] = w25q32_spi_transfer(0xFFU);
    GPIOB->BSRRL = (uint16_t)(1UL << 12U); /* CS high */
}

static void uart_send_text(const char *s)
{
    while (*s != '\0') {
        while ((USART3->SR & USART_SR_TXE) == 0U) __NOP();
        USART3->DR = (uint16_t)(uint8_t)*s++;
    }
    while ((USART3->SR & USART_SR_TC) == 0U) __NOP();
}

static void uart_send_hex_byte(uint8_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    while ((USART3->SR & USART_SR_TXE) == 0U) __NOP();
    USART3->DR = (uint16_t)hex[(value >> 4U) & 0x0FU];
    while ((USART3->SR & USART_SR_TXE) == 0U) __NOP();
    USART3->DR = (uint16_t)hex[value & 0x0FU];
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    /* CASET/PASET use four separate 8-bit parameters, each on D[7:0]. */
    LCD_CMD(0x2A);
    LCD_DATA((uint8_t)(x0 >> 8)); LCD_DATA((uint8_t)x0);
    LCD_DATA((uint8_t)(x1 >> 8)); LCD_DATA((uint8_t)x1);
    LCD_CMD(0x2B);
    LCD_DATA((uint8_t)(y0 >> 8)); LCD_DATA((uint8_t)y0);
    LCD_DATA((uint8_t)(y1 >> 8)); LCD_DATA((uint8_t)y1);
    LCD_CMD(0x2C);
}

static void lcd_fill(uint16_t color)
{
    uint32_t i;
    lcd_set_window(0U, 0U, 479U, 319U);
    for (i = 0U; i < (480UL * 320UL); ++i) {
        LCD_DATA(color);
        ++g_pixels_written;
    }
}


/*
 * W25Q32 full-chip read-only dump engine.
 * SRAM staging buffer: 0x20010000..0x20017FFF (32 KiB).
 * SWD mailbox: 0x2001F000 (8 words).
 * Host sets [1]=1, [2]=offset, [3]=length (1..32768), then polls [4].
 * Firmware sets [4]=1 while busy, then [4]=2 when buffer is ready.
 * Host reads staging buffer and sets [1]=0 before issuing next chunk.
 */
#define W25Q_DUMP_BUFFER ((volatile uint8_t *)0x20010000UL)
#define W25Q_DUMP_MAILBOX ((volatile uint32_t *)0x2001F000UL)
#define W25Q_CAPACITY 0x00400000UL
#define W25Q_CHUNK_MAX 32768UL

static void w25q32_read_bytes(uint32_t address, volatile uint8_t *dst, uint32_t length)
{
    uint32_t i;
    GPIOB->BSRRH = (uint16_t)(1UL << 12U);
    (void)w25q32_spi_transfer(0x03U);
    (void)w25q32_spi_transfer((uint8_t)(address >> 16U));
    (void)w25q32_spi_transfer((uint8_t)(address >> 8U));
    (void)w25q32_spi_transfer((uint8_t)address);
    for (i = 0U; i < length; ++i) dst[i] = w25q32_spi_transfer(0xFFU);
    GPIOB->BSRRL = (uint16_t)(1UL << 12U);
}

static void w25q32_dump_service(void)
{
    volatile uint32_t *m = W25Q_DUMP_MAILBOX;
    uint32_t command = m[1];
    if (command == 0U) {
        m[4] = 0U;
        __DSB();
        return;
    }
    if (command != 1U || m[4] == 1U || m[4] == 2U) return;

    {
        uint32_t offset = m[2];
        uint32_t length = m[3];
        m[4] = 1U; /* BUSY */
        m[5] = offset;
        m[6] = 0U;
        __DSB();

        if (length == 0U || length > W25Q_CHUNK_MAX ||
            offset >= W25Q_CAPACITY || length > (W25Q_CAPACITY - offset)) {
            m[4] = 3U; /* invalid request */
        } else {
            w25q32_read_bytes(offset, W25Q_DUMP_BUFFER, length);
            m[6] = length;
            m[4] = 2U; /* READY */
        }
        __DSB();
    }
}


static void lcd_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    uint32_t i, count;
    if (x1 >= 480U) x1 = 479U;
    if (y1 >= 320U) y1 = 319U;
    if (x0 > x1 || y0 > y1) return;
    lcd_set_window(x0, y0, x1, y1);
    count = ((uint32_t)x1 - x0 + 1U) * ((uint32_t)y1 - y0 + 1U);
    for (i = 0U; i < count; ++i) LCD_DATA(color);
}
static void draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color)
{
    if (w) lcd_fill_rect(x, y, (uint16_t)(x + w - 1U), y, color);
}
static void draw_static_hud(void)
{
    uint16_t y;
    lcd_fill(0x0841);
    lcd_fill_rect(5U,5U,314U,474U,0x18E3);
    lcd_fill_rect(7U,7U,312U,472U,0x0841);
    lcd_fill_rect(14U,16U,305U,62U,0x10A3);
    lcd_fill_rect(14U,70U,305U,420U,0x0000);
    lcd_fill_rect(14U,428U,305U,463U,0x10A3);
    lcd_fill_rect(20U,22U,25U,56U,0x07FF);
    draw_hline(35U,28U,245U,0x7DFF);
    draw_hline(35U,36U,190U,0x39E7);
    draw_hline(35U,44U,220U,0x39E7);
    draw_hline(35U,52U,150U,0x39E7);
    for (y=90U; y<410U; y=(uint16_t)(y+32U)) draw_hline(22U,y,276U,0x10A3);
    for (y=40U; y<300U; y=(uint16_t)(y+40U)) lcd_fill_rect(y,82U,y,401U,0x0862);
    lcd_fill_rect(22U,438U,92U,443U,0x39E7);
    lcd_fill_rect(22U,450U,285U,453U,0x2104);
    lcd_fill_rect(22U,450U,105U,453U,0x07E0);
    lcd_fill_rect(22U,458U,285U,459U,0x39E7);
}

/* XPT2046 wiring measured directly by the user on JZ-TS35 V2.1:
 * Pin 11 PENIRQ -> PB3; pin 12 DOUT/MISO -> PB9;
 * pin 14 DIN/MOSI -> PB6; pin 15 CS -> PB8; pin 16 DCLK/SCK -> PB5.
 * Pins 2..5 connect to FPC 1..4 (analog panel electrodes).
 * SPI mode 0, software bit-bang. LCD/FSMC remains unchanged.
 */
#define TOUCH_RECORDS ((volatile uint32_t *)0x20010000UL)
#define TOUCH_MAILBOX ((volatile uint32_t *)0x2001F000UL)
#define TOUCH_RECORD_CAPACITY 256UL
#define TOUCH_MAGIC 0x54504348UL

static void touch_gpio_init(void)
{
    RCC->AHB1ENR |= (1UL << 1); /* GPIOB clock */
    (void)RCC->AHB1ENR;

    /* PB5=SCK, PB6=MOSI, PB8=CS outputs; PB9=MISO and PB3=PENIRQ inputs. */
    GPIOB->MODER &= ~((3UL << 6U) | (3UL << 10U) | (3UL << 12U) |
                      (3UL << 16U) | (3UL << 18U));
    GPIOB->MODER |=  (1UL << 10U) | (1UL << 12U) | (1UL << 16U);
    GPIOB->OTYPER &= ~((1UL << 5U) | (1UL << 6U) | (1UL << 8U));
    GPIOB->OSPEEDR |= (3UL << 10U) | (3UL << 12U) | (3UL << 16U);
    GPIOB->PUPDR &= ~((3UL << 6U) | (3UL << 10U) | (3UL << 12U) |
                      (3UL << 16U) | (3UL << 18U));

    /* PENIRQ PB3 active-low input with pull-up; MISO PB9 floating input. */
    GPIOB->PUPDR |= (1UL << 6U);

    /* PB3 is GPIO input; SWD on PA13/PA14 remains untouched. */
    GPIOB->BSRRL = (uint16_t)(1UL << 8U); /* CS high */
    GPIOB->BSRRH = (uint16_t)((1UL << 5U) | (1UL << 6U)); /* SCK/MOSI low */
    __DSB();
}

static void touch_spi_delay(void)
{
    __NOP(); __NOP(); __NOP(); __NOP();
}

static uint8_t touch_spi_transfer(uint8_t tx)
{
    uint8_t rx = 0U;
    uint32_t bit;
    for (bit = 0U; bit < 8U; ++bit) {
        if ((tx & 0x80U) != 0U) GPIOB->BSRRL = (uint16_t)(1UL << 6U);
        else                    GPIOB->BSRRH = (uint16_t)(1UL << 6U);
        touch_spi_delay();
        GPIOB->BSRRL = (uint16_t)(1UL << 5U); /* rising edge: sample */
        touch_spi_delay();
        rx = (uint8_t)((rx << 1U) |
             ((GPIOB->IDR & (1UL << 9U)) != 0U ? 1U : 0U));
        GPIOB->BSRRH = (uint16_t)(1UL << 5U); /* falling edge */
        touch_spi_delay();
        tx <<= 1U;
    }
    return rx;
}

static uint16_t touch_read_adc(uint8_t command)
{
    uint16_t value;
    GPIOB->BSRRH = (uint16_t)(1UL << 8U); /* CS low */
    (void)touch_spi_transfer(command);
    value = (uint16_t)touch_spi_transfer(0xFFU) << 8U;
    value |= touch_spi_transfer(0xFFU);
    GPIOB->BSRRL = (uint16_t)(1UL << 8U); /* CS high */
    /* XPT2046: discard the leading null bit and 3 trailing clock bits. */
    return (uint16_t)((value >> 3U) & 0x0FFFU);
}

static uint8_t touch_is_pressed(void)
{
    return (GPIOB->IDR & (1UL << 3U)) == 0U;
}

static void lcd_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color)
{
    if (x < 480U && h != 0U) lcd_fill_rect(x, y, x, (uint16_t)(y+h-1U), color);
}

/* 16x16 CJK direction labels: 上、下、左、右. */
static const uint16_t direction_glyphs[4][16] = {
    /* 上 */
    {0x0000,0x0000,0x0000,0x0000,0x0380,0x0380,0x0380,0x0380,
     0x0380,0x03FC,0x03FC,0x0380,0x0380,0x0380,0x0380,0x0380},
    /* 下 */
    {0x0000,0x0000,0x0000,0x0000,0x0000,0x7FFE,0x7FFE,0x0180,
     0x0180,0x0180,0x01E0,0x01F8,0x01BC,0x018C,0x0180,0x0180},
    /* 左 */
    {0x0000,0x0000,0x0000,0x0000,0x0300,0x0600,0x0600,0x7FFE,
     0x7FFE,0x0600,0x0C00,0x0FFE,0x1BFE,0x1860,0x1860,0x3060},
    /* 右 */
    {0x0000,0x0000,0x0000,0x0000,0x0300,0x0300,0x0300,0x7FFE,
     0x7FFE,0x0600,0x0C00,0x0C00,0x1FFC,0x3FFC,0x7C0C,0x6C0C}
};

static void draw_direction_glyph(uint16_t x, uint16_t y, uint8_t glyph,
                                 uint16_t color)
{
    uint16_t row, col;
    const uint16_t scale = 2U;
    if (glyph > 3U) return;
    for (row = 0U; row < 16U; ++row) {
        for (col = 0U; col < 16U; ++col) {
            if ((direction_glyphs[glyph][row] & (0x8000U >> col)) != 0U)
                lcd_fill_rect((uint16_t)(x + col * scale),
                              (uint16_t)(y + row * scale),
                              (uint16_t)(x + col * scale + scale - 1U),
                              (uint16_t)(y + row * scale + scale - 1U), color);
        }
    }
}

static void draw_target(uint16_t x, uint16_t y, uint16_t color)
{
    lcd_fill_rect((uint16_t)(x-12U), y, (uint16_t)(x+12U), y, color);
    lcd_fill_rect(x, (uint16_t)(y-12U), x, (uint16_t)(y+12U), color);
    lcd_fill_rect((uint16_t)(x-3U), (uint16_t)(y-3U),
                  (uint16_t)(x+3U), (uint16_t)(y+3U), color);
}

/* Compact 5x7 ASCII font for TOP / LEFT / RIGHT labels. */
static const uint8_t ascii_5x7[10][5] = {
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x01,0x01,0x7F,0x01,0x01}  /* T */
};

static uint8_t ascii_glyph_index(char ch)
{
    switch (ch) {
        case 'E': return 0U; case 'F': return 1U; case 'G': return 2U;
        case 'H': return 3U; case 'I': return 4U; case 'L': return 5U;
        case 'O': return 6U; case 'P': return 7U; case 'R': return 8U;
        case 'T': return 9U; default: return 0xFFU;
    }
}

static void draw_ascii_label(uint16_t x, uint16_t y, const char *s, uint16_t color)
{
    const uint16_t scale = 3U;
    while (*s != '\0') {
        uint8_t index = ascii_glyph_index(*s++);
        uint8_t col, row;
        if (index == 0xFFU) { x = (uint16_t)(x + 4U * scale); continue; }
        for (col = 0U; col < 5U; ++col) {
            for (row = 0U; row < 7U; ++row) {
                if ((ascii_5x7[index][col] & (1U << row)) != 0U)
                    lcd_fill_rect((uint16_t)(x + col * scale),
                                  (uint16_t)(y + row * scale),
                                  (uint16_t)(x + col * scale + scale - 1U),
                                  (uint16_t)(y + row * scale + scale - 1U), color);
            }
        }
        x = (uint16_t)(x + 6U * scale);
    }
}

static void draw_touch_screen(void)
{
    uint16_t p;
    /* ILI9486L native GRAM: 320x480. MADCTL.MV=1 exposes 480x320 landscape. */
    lcd_fill(0x0000U);

    /* Edge-to-edge perimeter, logical x=0..479 and y=0..319. */
    lcd_fill_rect(0U, 0U, 479U, 1U, 0xFFFFU);
    lcd_fill_rect(0U, 318U, 479U, 319U, 0xFFFFU);
    lcd_fill_rect(0U, 0U, 1U, 319U, 0xFFFFU);
    lcd_fill_rect(478U, 0U, 479U, 319U, 0xFFFFU);

    /* Inner edge guide. */
    lcd_fill_rect(10U, 10U, 469U, 10U, 0x39E7U);
    lcd_fill_rect(10U, 309U, 469U, 309U, 0x39E7U);
    lcd_fill_rect(10U, 10U, 10U, 309U, 0x39E7U);
    lcd_fill_rect(469U, 10U, 469U, 309U, 0x39E7U);

    /* Grid covers the entire active area. */
    for (p=40U; p<480U; p=(uint16_t)(p+40U))
        lcd_fill_rect(p, 2U, (uint16_t)(p+1U), 317U, 0x1082U);
    for (p=40U; p<320U; p=(uint16_t)(p+40U))
        lcd_fill_rect(2U, p, 477U, (uint16_t)(p+1U), 0x1082U);
    lcd_fill_rect(239U, 2U, 240U, 317U, 0x2104U);
    lcd_fill_rect(2U, 159U, 477U, 160U, 0x2104U);

    /* Direction targets with explicit Chinese labels. */
    draw_target(240U, 34U, 0xF800U);   /* TOP */
    draw_target(240U, 278U, 0x001FU);  /* BOTTOM */
    draw_target(42U, 160U, 0x07E0U);   /* LEFT */
    draw_target(437U, 160U, 0x07FFU);  /* RIGHT */
    draw_target(240U, 160U, 0xFFE0U);  /* CENTER */

    draw_direction_glyph(224U, 8U, 0U, 0xFFFFU);    /* 上, 32x32 */
    draw_direction_glyph(224U, 280U, 1U, 0xFFFFU);  /* 下, 32x32 */
    draw_direction_glyph(8U, 144U, 2U, 0xFFFFU);    /* 左, 32x32 */
    draw_direction_glyph(440U, 144U, 3U, 0xFFFFU);  /* 右, 32x32 */

    /* Large, fixed-size English direction labels. */
    draw_ascii_label(210U, 48U, "TOP", 0xFFFFU);
    draw_ascii_label(14U, 184U, "LEFT", 0xFFFFU);
    draw_ascii_label(390U, 184U, "RIGHT", 0xFFFFU);

    /* Corner witnesses. */
    draw_target(14U, 14U, 0xFFFFU);
    draw_target(465U, 14U, 0xFFFFU);
    draw_target(14U, 305U, 0xFFFFU);
    draw_target(465U, 305U, 0xFFFFU);
}

static void record_touch(uint16_t x, uint16_t y, uint16_t z1, uint16_t z2,
                         uint32_t pressed)
{
    volatile uint32_t *m = TOUCH_MAILBOX;
    uint32_t index = m[3];
    uint32_t total = m[2] + 1U;
    m[2] = total;

    if (index < TOUCH_RECORD_CAPACITY) {
        volatile uint32_t *r = &TOUCH_RECORDS[index * 6U];
        r[0] = x;
        r[1] = y;
        r[2] = z1;
        r[3] = z2;
        r[4] = g_ms;
        r[5] = pressed;
        m[3] = index + 1U;
    }
    m[4] = x; m[5] = y; m[6] = z1; m[7] = z2;
    __DSB();
}

static void run_touch_diagnostic(void)
{
    uint16_t x, y, z1, z2;
    uint8_t was_pressed = 0U, pressed;
    uint32_t last_sample = 0U;
    uint32_t dot_started_ms = 0U;
    uint8_t dot_visible = 0U;
    uint16_t dot_x = 0U, dot_y = 0U;
    volatile uint32_t *m = TOUCH_MAILBOX;

    touch_gpio_init();
    buzzer_init();
    m[0] = TOUCH_MAGIC;
    m[1] = 1U; /* running */
    m[2] = 0U; /* total samples */
    m[3] = 0U; /* stored records */
    m[4] = 0U; m[5] = 0U; m[6] = 0U; m[7] = 0U;
    m[8] = 0U; /* press transitions */
    m[9] = 0U; /* release transitions */
    m[10] = 0x28U; /* MADCTL: landscape MV+BGR */
    m[11] = 480U;
    m[12] = 320U;
    m[13] = 0U; m[14] = 0U; m[15] = 0U; m[16] = 0U; m[17] = 0U;
    m[18] = 0U; m[19] = 0U;
    __DSB();

    draw_touch_screen();

    for (;;) {
        if ((uint32_t)(g_ms - last_sample) >= 25U) {
            last_sample = g_ms;

            /* Poll ADC continuously, even when PENIRQ is not asserted.
             * This separates a bad PENIRQ wire/polarity from an SPI failure. */
            x  = touch_read_adc(0xD0U); /* X */
            y  = touch_read_adc(0x90U); /* Y */
            z1 = touch_read_adc(0xB0U); /* pressure Z1 */
            z2 = touch_read_adc(0xC0U); /* pressure Z2 */
            pressed = (uint8_t)(touch_is_pressed() ||
                                (z1 > 40U && z2 < 4000U));

            m[13] = (GPIOB->IDR >> 3U) & 1U; /* PB3/PENIRQ raw: 0=asserted */
            m[14] = (GPIOB->IDR >> 9U) & 1U; /* PB9/MISO raw */
            m[15] = (GPIOB->IDR >> 5U) & 1U; /* PB5/SCK level */
            m[16] = (GPIOB->IDR >> 6U) & 1U; /* PB6/MOSI level */
            m[17] = (GPIOB->IDR >> 8U) & 1U; /* PB8/CS level */
            record_touch(x, y, z1, z2, pressed);

            if (pressed && !was_pressed) {
                ++m[8];
                buzzer_click();

                /* Map the XPT2046 panel axes onto landscape LCD coordinates.
                 * The observed bottom-corner taps show the raw axes are swapped:
                 * LCD X follows inverted raw Y; LCD Y follows raw X.
                 */
                dot_x = (uint16_t)(((4095UL - (uint32_t)y) * 479UL) / 4095UL);
                dot_y = (uint16_t)(((uint32_t)x * 319UL) / 4095UL);
                if (dot_x < 5U) dot_x = 5U;
                if (dot_x > 474U) dot_x = 474U;
                if (dot_y < 5U) dot_y = 5U;
                if (dot_y > 314U) dot_y = 314U;
                lcd_fill_rect((uint16_t)(dot_x - 5U), (uint16_t)(dot_y - 5U),
                              (uint16_t)(dot_x + 5U), (uint16_t)(dot_y + 5U),
                              0xFFE0U);
                dot_started_ms = g_ms;
                dot_visible = 1U;
            } else if (!pressed && was_pressed) {
                ++m[9];
            }
            was_pressed = pressed;
            __DSB();

            /* One marker per tap; redraw the clean test pattern after 10 s.
             * This avoids leaving touch trails while making each tap visible. */
            if (dot_visible && (uint32_t)(g_ms - dot_started_ms) >= 10000UL) {
                draw_touch_screen();
                dot_visible = 0U;
            }
        }
        __WFI();
    }
}


/* ================= SDIO 4-bit read-only diagnostic =================
 * Build revision: artifact path corrected.
 * User-confirmed socket wiring:
 * PC8 DAT0, PC9 DAT1, PC10 DAT2, PC11 DAT3, PC12 CLK, PD2 CMD.
 * STM32F207 SDIO uses PLLQ=48 MHz. Configure HSI->PLL 96 MHz,
 * PLLQ=48 MHz, SDIO init clock <=400 kHz, then transfer clock 12 MHz.
 * Results: mailbox 0x2001F000; sector-0 buffer 0x20010000 (512 bytes).
 */
#define SDIO_BASE_ADDR 0x40012C00UL
#define SDIO_REG(off) (*(volatile uint32_t *)(SDIO_BASE_ADDR + (off)))
#define SDIO_POWER   SDIO_REG(0x00U)
#define SDIO_CLKCR   SDIO_REG(0x04U)
#define SDIO_ARG     SDIO_REG(0x08U)
#define SDIO_CMD     SDIO_REG(0x0CU)
#define SDIO_RESPCMD SDIO_REG(0x10U)
#define SDIO_RESP1   SDIO_REG(0x14U)
#define SDIO_RESP2   SDIO_REG(0x18U)
#define SDIO_RESP3   SDIO_REG(0x1CU)
#define SDIO_RESP4   SDIO_REG(0x20U)
#define SDIO_DTIMER  SDIO_REG(0x24U)
#define SDIO_DLEN    SDIO_REG(0x28U)
#define SDIO_DCTRL   SDIO_REG(0x2CU)
#define SDIO_DCOUNT  SDIO_REG(0x30U)
#define SDIO_STA     SDIO_REG(0x34U)
#define SDIO_ICR     SDIO_REG(0x38U)
#define SDIO_MASK    SDIO_REG(0x3CU)
#define SDIO_FIFO    SDIO_REG(0x80U)
#define SDIO_STATIC_FLAGS 0x00C007FFUL
#define SDIO_CMD_CCRCFAIL (1UL<<0)
#define SDIO_CMD_DTIMEOUT (1UL<<3)
#define SDIO_CMD_CTIMEOUT (1UL<<2)
#define SDIO_CMD_TXUNDERR (1UL<<4)
#define SDIO_CMD_RXOVERR  (1UL<<5)
#define SDIO_CMD_CMDREND  (1UL<<6)
#define SDIO_CMD_CMDSENT  (1UL<<7)
#define SDIO_DATA_DCRCFAIL (1UL<<1)
#define SDIO_DATA_DTIMEOUT (1UL<<3)
#define SDIO_DATA_DATAEND (1UL<<8)
#define SDIO_DATA_RXDAVL (1UL<<21)
#define SDIO_DATA_RXFIFOHF (1UL<<15)
#define SDIO_DATA_STBITERR (1UL<<9)
#define SDIO_MAILBOX ((volatile uint32_t *)0x2001F000UL)
#define SDIO_SECTOR0 ((volatile uint32_t *)0x20010000UL)

static void sdio_pll_init_48mhz(void)
{
    /*
     * Keep SYSCLK on the reset-default 16 MHz HSI.
     * Only start the main PLL so PLLQ supplies the SDIO 48 MHz clock.
     * Do NOT switch SYSCLK: FSMC timings and SysTick were configured for
     * 16 MHz, and changing HCLK here corrupts subsequent LCD transactions.
     */
    RCC->PLLCFGR = 16UL | (192UL << 6) | (0UL << 16) | (4UL << 24);
    RCC->CR |= (1UL << 24); /* PLLON */
    while ((RCC->CR & (1UL << 25)) == 0U) { /* PLLRDY */ }
}

static void sdio_gpio_init(void)
{
    RCC->AHB1ENR |= (1UL<<2) | (1UL<<3); /* GPIOC, GPIOD */
    RCC->APB2ENR |= (1UL<<11);           /* SDIO clock */
    (void)RCC->APB2ENR;
    /* PC8..PC12 AF12. CMD/DAT lines have pull-ups; CLK no pull. */
    gpio_af12(GPIOC,8); gpio_af12(GPIOC,9); gpio_af12(GPIOC,10);
    gpio_af12(GPIOC,11); gpio_af12(GPIOC,12);
    gpio_af12(GPIOD,2);
    GPIOC->PUPDR = (GPIOC->PUPDR & ~((3UL<<16)|(3UL<<18)|(3UL<<20)|(3UL<<22)|(3UL<<24))) |
                   (1UL<<16)|(1UL<<18)|(1UL<<20)|(1UL<<22);
    GPIOD->PUPDR = (GPIOD->PUPDR & ~(3UL<<4)) | (1UL<<4);
    SDIO_POWER = 0U;
    SDIO_CLKCR = 0U;
    SDIO_MASK = 0U;
    SDIO_ICR = SDIO_STATIC_FLAGS;
    SDIO_POWER = 3U;
    delay_ms(2U);
    /* 48 MHz / (CLKDIV+2), divider 118 => 400 kHz. */
    SDIO_CLKCR = 118U | (1UL<<8); /* clock enable, 1-bit bus initially */
    delay_ms(2U);
}

static uint32_t sdio_command(uint8_t index, uint32_t argument, uint32_t response)
{
    uint32_t timeout = 2000000UL;
    uint32_t sta;
    SDIO_ICR = SDIO_STATIC_FLAGS;
    SDIO_ARG = argument;
    SDIO_CMD = (uint32_t)index | response | (1UL<<10); /* CPSMEN */
    while (timeout-- != 0U) {
        sta = SDIO_STA;
        if (sta & (SDIO_CMD_CTIMEOUT|SDIO_CMD_CCRCFAIL|SDIO_CMD_CMDREND|SDIO_CMD_CMDSENT))
            break;
    }
    sta = SDIO_STA;
    SDIO_MAILBOX[30] = sta;
    if (sta & SDIO_CMD_CTIMEOUT) return 1U;
    if ((sta & SDIO_CMD_CCRCFAIL) && response != 0U && index != 41U) return 2U; /* R3 has no CRC */
    if (response != 0U && !(sta & (SDIO_CMD_CMDREND|SDIO_CMD_CCRCFAIL))) return 3U;
    if (response == 0U && !(sta & SDIO_CMD_CMDSENT)) return 4U;
    return 0U;
}

static uint32_t sdio_app_command(uint16_t rca, uint8_t index, uint32_t arg)
{
    uint32_t e = sdio_command(55U, (uint32_t)rca<<16, 1UL<<6);
    if (e != 0U) return 0x100U | e;
    return sdio_command(index,arg,1UL<<6);
}

static uint32_t sdio_read_block0(void)
{
    uint32_t i, timeout, sta;
    SDIO_ICR = SDIO_STATIC_FLAGS;
    SDIO_DTIMER = 0xFFFFFFFFUL;
    SDIO_DLEN = 512U;
    SDIO_DCTRL = (9UL<<4) | (1UL<<1) | 1UL; /* 512-byte, block, read, enable */
    if (sdio_command(17U,0U,1UL<<6) != 0U) return 0x201U;
    timeout = 10000000UL;
    i = 0U;
    while (i < 128U && timeout-- != 0U) {
        sta = SDIO_STA;
        if (sta & (SDIO_DATA_DCRCFAIL|SDIO_DATA_DTIMEOUT|SDIO_DATA_STBITERR|SDIO_CMD_RXOVERR))
            return 0x202U | ((sta & 0xFFU)<<16);
        if (sta & SDIO_DATA_RXDAVL) {
            SDIO_SECTOR0[i++] = SDIO_FIFO;
        }
    }
    if (i != 128U) return 0x203U;
    timeout = 10000000UL;
    while (timeout-- != 0U) {
        sta = SDIO_STA;
        if (sta & (SDIO_DATA_DCRCFAIL|SDIO_DATA_DTIMEOUT|SDIO_DATA_STBITERR))
            return 0x204U | ((sta & 0xFFU)<<16);
        if (sta & SDIO_DATA_DATAEND) break;
    }
    if ((SDIO_STA & SDIO_DATA_DATAEND) == 0U) return 0x205U;
    SDIO_DCTRL = 0U;
    return 0U;
}

static void sdio_draw_start_screen(void)
{
    /* Keep a visible LCD witness while SDIO runs; LCD init alone does not draw. */
    lcd_fill(0x0000U);
    lcd_fill_rect(0U, 0U, 479U, 11U, 0x07FFU);
    lcd_fill_rect(16U, 36U, 463U, 42U, 0xFFFFU);
    lcd_fill_rect(16U, 62U, 350U, 68U, 0x7DFFU);
    lcd_fill_rect(16U, 104U, 463U, 108U, 0x2104U);
    lcd_fill_rect(16U, 104U, 90U, 108U, 0xFFE0U);
    lcd_fill_rect(16U, 280U, 463U, 287U, 0x18E3U);
}

static void sdio_draw_result(uint8_t passed)
{
    lcd_fill_rect(16U, 130U, 463U, 166U, passed ? 0x07E0U : 0xF800U);
    lcd_fill_rect(16U, 190U, 463U, 198U, passed ? 0x07E0U : 0xF800U);
    lcd_fill_rect(16U, 210U, 463U, 218U, passed ? 0x07E0U : 0xF800U);
    lcd_fill_rect(16U, 280U, 463U, 287U, passed ? 0x07E0U : 0xF800U);
}

static void run_sdio_diagnostic(void)
{
    volatile uint32_t *m = SDIO_MAILBOX;
    uint32_t err, i, ocr, rca;
    m[0]=0x5344494FUL; /* SDIO */
    for(i=1U;i<32U;i++) m[i]=0U;
    m[1]=1U; /* running */
    __DSB();
    sdio_draw_start_screen();

    sdio_pll_init_48mhz();
    sdio_gpio_init();

    err=sdio_command(0U,0U,0U); /* CMD0 */
    m[2]=err;
    if(err) goto fail;
    err=sdio_command(8U,0x1AAU,1UL<<6); /* CMD8 */
    m[3]=err; m[4]=SDIO_RESP1;
    /* CMD8 timeout is normal for legacy SDSC cards. */
    if(err!=0U && err!=1U) goto fail;

    ocr=0U;
    for(i=0U;i<1000U;i++) {
        err=sdio_app_command(0U,41U,0x40FF8000UL);
        if(err!=0U) { m[5]=err; goto fail; }
        ocr=SDIO_RESP1;
        if(ocr & (1UL<<31)) break;
        delay_ms(1U);
    }
    m[6]=ocr;
    if((ocr & (1UL<<31))==0U) { err=0x301U; goto fail; }

    err=sdio_command(2U,0U,3UL<<6); /* CMD2, long response CID */
    if(err) goto fail;
    m[7]=SDIO_RESP1; m[8]=SDIO_RESP2; m[9]=SDIO_RESP3; m[11]=SDIO_RESP4;

    err=sdio_command(3U,0U,1UL<<6); /* CMD3, RCA */
    if(err) goto fail;
    rca=SDIO_RESP1>>16;
    m[12]=rca;
    err=sdio_command(9U,rca<<16,3UL<<6); /* CMD9, long response CSD */
    if(err) goto fail;
    m[13]=SDIO_RESP1; m[14]=SDIO_RESP2; m[15]=SDIO_RESP3; m[16]=SDIO_RESP4;

    err=sdio_command(7U,rca<<16,1UL<<6); /* CMD7 select */
    if(err) goto fail;
    err=sdio_app_command((uint16_t)rca,6U,2U); /* ACMD6, 4-bit */
    if(err) goto fail;
    SDIO_CLKCR = (SDIO_CLKCR & ~(3UL<<11)) | (1UL<<11) | 2U | (1UL<<8);
    /* Transfer clock 12 MHz from 48 MHz PLLQ, 4-bit bus. */
    err=sdio_read_block0();
    m[17]=err;
    if(err) goto fail;

    m[18]=SDIO_DCOUNT;
    m[19]=SDIO_SECTOR0[0];
    m[20]=SDIO_SECTOR0[1];
    m[21]=SDIO_SECTOR0[2];
    m[22]=SDIO_SECTOR0[3];
    m[23]=SDIO_SECTOR0[127];
    m[24]=0U;
    for(i=0U;i<128U;i++) m[24]^=SDIO_SECTOR0[i];
    m[1]=2U; /* PASS */
    m[25]=0x50415353UL;
    sdio_draw_result(1U);
    __DSB();
    for(;;) __WFI();

fail:
    m[1]=3U; /* FAIL */
    m[26]=err;
    m[27]=SDIO_STA;
    m[28]=SDIO_CLKCR;
    m[29]=SDIO_POWER;
    sdio_draw_result(0U);
    __DSB();
    for(;;) __WFI();
}


int main(void)
{
    g_stage2d_status = ST_BOOT;
    backlight_init();
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
    run_touch_diagnostic(); /* Isolation build: keep SDIO completely inactive. */
}
