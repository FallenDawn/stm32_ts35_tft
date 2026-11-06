/*
 * JZ-TS35 V2.1 - Stage 2D.3 orientation / offset diagnostic
 * STM32F207VCT6 + ILI9486L, confirmed FSMC wiring only.
 * Does not touch unknown FPC pins or backlight GPIOs.
 *
 * Portrait address window: X=0..319, Y=0..479.
 * MADCTL 0x08 keeps BGR and clears MV (row/column exchange).
 * Persistent pattern: TOP, BOTTOM, four corner labels, colored edge bars,
 * center crosshair and center label.
 */
#include "stm32f2xx.h"
#include <stdint.h>

#define LCD_CMD_ADDR ((volatile uint16_t *)0x60000000UL)
#define LCD_DAT_ADDR ((volatile uint16_t *)0x60020000UL)
#define LCD_CMD(v)   (*LCD_CMD_ADDR = (uint16_t)(v))
#define LCD_DATA(v)  (*LCD_DAT_ADDR = (uint16_t)(v))

static volatile uint32_t g_ms;
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

enum {
    ST_BOOT       = 0x424F4F54UL,
    ST_FSMC_READY = 0x46534D43UL,
    ST_RESET_DONE = 0x52535444UL,
    ST_LCD_INIT   = 0x4C434449UL,
    ST_PATTERN    = 0x50415454UL
};

#define BLACK  0x0000U
#define WHITE  0xFFFFU
#define RED    0xF800U
#define GREEN  0x07E0U
#define BLUE   0x001FU
#define YELLOW 0xFFE0U
#define CYAN   0x07FFU
#define MAGENTA 0xF81FU

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
static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    lcd_cmd(0x2A);
    lcd_data((uint8_t)(x0 >> 8)); lcd_data((uint8_t)x0);
    lcd_data((uint8_t)(x1 >> 8)); lcd_data((uint8_t)x1);
    lcd_cmd(0x2B);
    lcd_data((uint8_t)(y0 >> 8)); lcd_data((uint8_t)y0);
    lcd_data((uint8_t)(y1 >> 8)); lcd_data((uint8_t)y1);
    lcd_cmd(0x2C);
}
static void fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                      uint16_t color)
{
    uint32_t n, count;
    if (x1 >= 320U) x1 = 319U;
    if (y1 >= 480U) y1 = 479U;
    if (x0 > x1 || y0 > y1) return;
    set_window(x0, y0, x1, y1);
    count = (uint32_t)(x1 - x0 + 1U) * (uint32_t)(y1 - y0 + 1U);
    for (n = 0; n < count; ++n) {
        lcd_data(color);
        ++g_pixels_written;
    }
}

/* Small 5x7 uppercase font, sufficient for orientation labels. */
static uint8_t glyph_col(char c, uint8_t col)
{
    static const uint8_t blank[5] = {0,0,0,0,0};
    static const uint8_t letters[][5] = {
        {0x7F,0x08,0x08,0x08,0x7F}, /* A placeholder index is not used */
    };
    (void)letters;
    switch (c) {
    case 'A': { static const uint8_t a[5]={0x7E,0x11,0x11,0x11,0x7E}; return a[col]; }
    case 'B': { static const uint8_t a[5]={0x7F,0x49,0x49,0x49,0x36}; return a[col]; }
    case 'C': { static const uint8_t a[5]={0x3E,0x41,0x41,0x41,0x22}; return a[col]; }
    case 'D': { static const uint8_t a[5]={0x7F,0x41,0x41,0x22,0x1C}; return a[col]; }
    case 'E': { static const uint8_t a[5]={0x7F,0x49,0x49,0x49,0x41}; return a[col]; }
    case 'L': { static const uint8_t a[5]={0x7F,0x40,0x40,0x40,0x40}; return a[col]; }
    case 'M': { static const uint8_t a[5]={0x7F,0x02,0x0C,0x02,0x7F}; return a[col]; }
    case 'N': { static const uint8_t a[5]={0x7F,0x04,0x08,0x10,0x7F}; return a[col]; }
    case 'O': { static const uint8_t a[5]={0x3E,0x41,0x41,0x41,0x3E}; return a[col]; }
    case 'P': { static const uint8_t a[5]={0x7F,0x09,0x09,0x09,0x06}; return a[col]; }
    case 'R': { static const uint8_t a[5]={0x7F,0x09,0x19,0x29,0x46}; return a[col]; }
    case 'T': { static const uint8_t a[5]={0x01,0x01,0x7F,0x01,0x01}; return a[col]; }
    case 'X': { static const uint8_t a[5]={0x63,0x14,0x08,0x14,0x63}; return a[col]; }
    case 'Y': { static const uint8_t a[5]={0x03,0x04,0x78,0x04,0x03}; return a[col]; }
    default: return blank[col];
    }
}
static void draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint8_t scale)
{
    uint8_t cx, cy;
    for (cx=0; cx<5; ++cx) {
        uint8_t bits = glyph_col(c, cx);
        for (cy=0; cy<7; ++cy) {
            if (bits & (1U << cy))
                fill_rect(x + cx*scale, y + cy*scale,
                          x + (cx+1U)*scale - 1U,
                          y + (cy+1U)*scale - 1U, color);
        }
    }
}
static void draw_text(uint16_t x, uint16_t y, const char *s,
                      uint16_t color, uint8_t scale)
{
    while (*s) {
        if (*s != ' ')
            draw_char(x, y, *s, color, scale);
        x = (uint16_t)(x + 6U*scale);
        ++s;
    }
}
static void draw_hline(uint16_t x0,uint16_t x1,uint16_t y,uint16_t c)
{ fill_rect(x0,y,x1,y,c); }
static void draw_vline(uint16_t x,uint16_t y0,uint16_t y1,uint16_t c)
{ fill_rect(x,y0,x,y1,c); }

static void lcd_init(void)
{
    lcd_cmd(0x01); delay_ms(120U);
    lcd_cmd(0x11); delay_ms(120U);
    lcd_cmd(0x3A); lcd_data(0x55);
    lcd_cmd(0x36); lcd_data(0x08); /* BGR=1, MV=0: portrait 320x480 */
    lcd_cmd(0x29); delay_ms(150U);
}

static void draw_pattern(void)
{
    /* White canvas and four uniquely colored physical-edge indicators. */
    fill_rect(0,0,319,479,WHITE);
    fill_rect(0,0,319,13,RED);       /* intended TOP */
    fill_rect(306,14,319,465,GREEN); /* intended RIGHT */
    fill_rect(0,466,319,479,BLUE);   /* intended BOTTOM */
    fill_rect(0,14,13,465,YELLOW);   /* intended LEFT */

    /* Corner tags are deliberately inset from the colored edge bars. */
    draw_text(24,28,"TL",RED,3);
    draw_text(250,28,"TR",RED,3);
    draw_text(24,420,"BL",BLUE,3);
    draw_text(250,420,"BR",BLUE,3);

    draw_text(105,42,"TOP",BLACK,3);
    draw_text(78,432,"BOTTOM",BLACK,2);

    /* Center crosshair and a clear center marker. */
    draw_vline(159,150,329,MAGENTA);
    draw_hline(45,274,239,MAGENTA);
    fill_rect(151,231,167,247,CYAN);
    draw_text(111,260,"CENTER",BLACK,2);

    /* Axis labels and short ticks reveal clipping / scaling. */
    draw_text(20,205,"Y",BLACK,2);
    draw_text(285,205,"Y",BLACK,2);
    draw_text(148,18,"X",BLACK,2);
    draw_text(148,445,"X",BLACK,2);
    draw_vline(30,100,110,BLACK);
    draw_vline(30,230,240,BLACK);
    draw_vline(30,360,370,BLACK);
    draw_vline(289,100,110,BLACK);
    draw_vline(289,230,240,BLACK);
    draw_vline(289,360,370,BLACK);
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
    lcd_init();
    g_stage_status = ST_LCD_INIT;
    draw_pattern();
    g_stage_status = ST_PATTERN;
    for (;;) {
        ++g_loop_counter;
        __WFI();
        ++g_heartbeat;
    }
}
