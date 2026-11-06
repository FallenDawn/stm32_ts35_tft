/*
 * JZ-TS35 V2.1 / STM32F207VCT6 minimal runtime diagnostic firmware.
 *
 * Purpose:
 *   - Verify reset vector + startup + SystemInit + main execution.
 *   - Keep SWD pins PA13/PA14 untouched.
 *   - Expose stable, named volatile symbols for SWD memory inspection.
 *   - Capture Cortex-M3 fault stack and SCB fault status.
 *
 * This target deliberately does not initialize the TFT, SPI, touch controller,
 * external W25Q32, SD card, UART, or any GPIO.
 */

#include "stm32f2xx.h"

#define DIAG_MAGIC              0x4A5A4447UL /* "JZDG" */
#define DIAG_VERSION            0x00010001UL
#define DIAG_STATUS_BOOT        0x424F4F54UL /* "BOOT" */
#define DIAG_STATUS_RUNNING     0x52554E31UL /* "RUN1" */
#define DIAG_STATUS_FAULT       0x4641554CUL /* "FAUL" */
#define DIAG_STATUS_SYSTICK_ERR 0x53595445UL /* "SYTE" */
#define DIAG_FAULT_MAGIC        0x464C5444UL /* "FLTD" */

typedef struct
{
    volatile uint32_t magic;
    volatile uint32_t version;
    volatile uint32_t status;
    volatile uint32_t boot_count;

    volatile uint32_t clock_hz;
    volatile uint32_t vector_table;
    volatile uint32_t reset_flags;
    volatile uint32_t rcc_cr;
    volatile uint32_t rcc_cfgr;
    volatile uint32_t flash_acr;

    volatile uint32_t uptime_ms;
    volatile uint32_t heartbeat;
    volatile uint32_t loop_counter;
    volatile uint32_t last_heartbeat_ms;
    volatile uint32_t systick_load;
    volatile uint32_t systick_control;
    volatile uint32_t systick_config_result;

    volatile uint32_t fault_magic;
    volatile uint32_t fault_type;
    volatile uint32_t fault_r0;
    volatile uint32_t fault_r1;
    volatile uint32_t fault_r2;
    volatile uint32_t fault_r3;
    volatile uint32_t fault_r12;
    volatile uint32_t fault_lr;
    volatile uint32_t fault_pc;
    volatile uint32_t fault_xpsr;
    volatile uint32_t fault_cfsr;
    volatile uint32_t fault_hfsr;
    volatile uint32_t fault_dfsr;
    volatile uint32_t fault_mmfar;
    volatile uint32_t fault_bfar;
    volatile uint32_t fault_loop;

    /* Stage 1: bounded HSE oscillator check; SYSCLK remains on HSI. */
    volatile uint32_t hse_result;          /* 0=pending, 1=ready, 2=timeout */
    volatile uint32_t hse_poll_count;
    volatile uint32_t hse_rcc_cr;

    /* Stage 2A: test PLL lock from known 16 MHz HSI; SYSCLK stays on HSI. */
    volatile uint32_t pll_result;       /* 0=pending, 1=ready, 2=timeout */
    volatile uint32_t pll_poll_count;
    volatile uint32_t pll_rcc_cr;
    volatile uint32_t pll_rcc_pllcfgr;
    volatile uint32_t pll_rcc_cfgr;
    volatile uint32_t pll_flash_acr;
    volatile uint32_t pll_expected_vco_hz;
    volatile uint32_t pll_expected_sysclk_hz;

    /* Stage 2B: bounded SYSCLK switch to the already-locked 96 MHz PLL. */
    volatile uint32_t switch_result;       /* 0=pending, 1=PLL, 2=PLL timeout/HSI restored, 3=fallback unconfirmed, 4=PLL test skipped */
    volatile uint32_t switch_poll_count;
    volatile uint32_t switch_rcc_cr;
    volatile uint32_t switch_rcc_cfgr;
    volatile uint32_t switch_flash_acr;
    volatile uint32_t switch_systick_load;
    volatile uint32_t switch_sysclk_hz;

    /* Stage 2A.5: validate safe pre-switch bus/Flash setup; SYSCLK stays HSI. */
    volatile uint32_t preflight_result;       /* 0=pending, 1=verified, 2=readback mismatch, 3=PLL not ready */
    volatile uint32_t preflight_rcc_cr;
    volatile uint32_t preflight_rcc_cfgr;
    volatile uint32_t preflight_flash_acr;
    volatile uint32_t preflight_systick_load;
    volatile uint32_t preflight_sysclk_hz;
} DiagnosticState;

/* Global, non-static symbol: locate it in the ELF with arm-none-eabi-nm. */
volatile DiagnosticState g_diag __attribute__((used, aligned(4)));

/*
 * Diagnostic-only clock setup:
 * Keep the STM32F207 on its reset-default 16 MHz HSI clock.
 * Do not start HSE, PLL or PLLI2S here. The board oscillator and PLL
 * configuration are not yet verified, and this test does not need them.
 * The startup assembly calls SystemInit() before main().
 */
void SystemInit(void)
{
    SCB->VTOR = 0x08000000UL;
    __DSB();
    __ISB();
}

static volatile uint32_t g_tick_ms;

void SysTick_Handler(void)
{
    ++g_tick_ms;
    g_diag.uptime_ms = g_tick_ms;
}

static void Fault_Capture(uint32_t *stack_frame, uint32_t fault_type)
{
    g_diag.fault_magic = DIAG_FAULT_MAGIC;
    g_diag.fault_type = fault_type;

    if (stack_frame != 0)
    {
        g_diag.fault_r0   = stack_frame[0];
        g_diag.fault_r1   = stack_frame[1];
        g_diag.fault_r2   = stack_frame[2];
        g_diag.fault_r3   = stack_frame[3];
        g_diag.fault_r12  = stack_frame[4];
        g_diag.fault_lr   = stack_frame[5];
        g_diag.fault_pc   = stack_frame[6];
        g_diag.fault_xpsr = stack_frame[7];
    }

    g_diag.fault_cfsr  = SCB->CFSR;
    g_diag.fault_hfsr  = SCB->HFSR;
    g_diag.fault_dfsr  = SCB->DFSR;
    g_diag.fault_mmfar = SCB->MMFAR;
    g_diag.fault_bfar  = SCB->BFAR;
    g_diag.status = DIAG_STATUS_FAULT;

    for (;;)
    {
        ++g_diag.fault_loop;
        __NOP();
    }
}

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

#define DEFINE_FAULT_HANDLER(handler_name, fault_code)                 \
    void handler_name(void) __attribute__((naked));                    \
    void handler_name(void)                                            \
    {                                                                  \
        __asm volatile (                                               \
            "tst lr, #4          \n"                                   \
            "ite eq              \n"                                   \
            "mrseq r0, msp        \n"                                   \
            "mrsne r0, psp        \n"                                   \
            "movs r1, #" STRINGIFY(fault_code) "\n"                   \
            "b Fault_Capture      \n");                                \
    }

DEFINE_FAULT_HANDLER(HardFault_Handler, 1)
DEFINE_FAULT_HANDLER(MemManage_Handler, 2)
DEFINE_FAULT_HANDLER(BusFault_Handler, 3)
DEFINE_FAULT_HANDLER(UsageFault_Handler, 4)
DEFINE_FAULT_HANDLER(NMI_Handler, 5)

int main(void)
{
    uint32_t last_heartbeat = 0;

    /* Record startup state before clearing the reset flags. */
    g_diag.magic = DIAG_MAGIC;
    g_diag.version = DIAG_VERSION;
    g_diag.status = DIAG_STATUS_BOOT;
    g_diag.boot_count = 1;

    g_diag.clock_hz = HSI_VALUE;
    g_diag.vector_table = SCB->VTOR;
    g_diag.reset_flags = RCC->CSR;
    g_diag.rcc_cr = RCC->CR;
    g_diag.rcc_cfgr = RCC->CFGR;
    g_diag.flash_acr = FLASH->ACR;

    /* 1 kHz time base. No peripheral clock or GPIO setup is needed. */
    g_diag.systick_config_result = SysTick_Config(HSI_VALUE / 1000UL);
    g_diag.systick_load = SysTick->LOAD;
    g_diag.systick_control = SysTick->CTRL;

    /* Clear reset flags only after their value has been copied to g_diag. */
    RCC->CSR |= (1UL << 24);

    if (g_diag.systick_config_result != 0UL)
    {
        g_diag.status = DIAG_STATUS_SYSTICK_ERR;
        for (;;)
        {
            ++g_diag.loop_counter;
            __NOP();
        }
    }

    /*
     * Stage 1 only: start the external high-speed oscillator and check HSERDY.
     * Do not change SYSCLK, PLL, bus prescalers, or Flash latency here.
     * A bounded poll guarantees a missing/bad HSE cannot hang the CPU.
     * Keep HSE enabled on success for the later PLL validation stage.
     */
    g_diag.hse_result = 0UL;
    g_diag.hse_poll_count = 0UL;
    RCC->CR |= RCC_CR_HSEON;

    while ((RCC->CR & RCC_CR_HSERDY) == 0UL &&
           g_diag.hse_poll_count < 1000000UL)
    {
        ++g_diag.hse_poll_count;
        __NOP();
    }

    g_diag.hse_rcc_cr = RCC->CR;
    if ((RCC->CR & RCC_CR_HSERDY) != 0UL)
    {
        g_diag.hse_result = 1UL; /* HSE ready */
    }
    else
    {
        g_diag.hse_result = 2UL; /* bounded timeout; remain on HSI */
        RCC->CR &= ~RCC_CR_HSEON;
        g_diag.hse_rcc_cr = RCC->CR;
    }

    /*
     * Stage 2A: validate PLL lock using the known HSI source only.
     * Current RCC_PLLCFGR observed on this board is 0x24003010:
     * PLLM=16, PLLN=192, PLLP=2, PLLSRC=HSI, PLLQ=4.
     * HSI 16 MHz -> PLL input 1 MHz -> VCO 192 MHz -> PLLP output 96 MHz.
     * Do NOT switch SYSCLK: it remains HSI at 16 MHz, so Flash latency and
     * bus prescalers are intentionally unchanged. PLL lock wait is bounded.
     */
    g_diag.pll_result = 0UL;
    g_diag.pll_poll_count = 0UL;
    g_diag.pll_expected_vco_hz = 192000000UL;
    g_diag.pll_expected_sysclk_hz = 96000000UL;

    /* Ensure PLL is disabled before changing PLLCFGR. */
    RCC->CR &= ~RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) != 0UL &&
           g_diag.pll_poll_count < 100000UL)
    {
        ++g_diag.pll_poll_count;
        __NOP();
    }

    if ((RCC->CR & RCC_CR_PLLRDY) == 0UL)
    {
        /* Explicit HSI-sourced configuration; never assume board HSE MHz. */
        RCC->PLLCFGR = (16UL) | (192UL << 6) | (0UL << 16) |
                       (0UL << 22) | (4UL << 24);
        g_diag.pll_rcc_pllcfgr = RCC->PLLCFGR;
        g_diag.pll_poll_count = 0UL;
        RCC->CR |= RCC_CR_PLLON;

        while ((RCC->CR & RCC_CR_PLLRDY) == 0UL &&
               g_diag.pll_poll_count < 1000000UL)
        {
            ++g_diag.pll_poll_count;
            __NOP();
        }

        g_diag.pll_rcc_cr = RCC->CR;
        g_diag.pll_rcc_cfgr = RCC->CFGR;
        g_diag.pll_flash_acr = FLASH->ACR;

        if ((RCC->CR & RCC_CR_PLLRDY) != 0UL)
        {
            g_diag.pll_result = 1UL;
        }
        else
        {
            g_diag.pll_result = 2UL;
            RCC->CR &= ~RCC_CR_PLLON;
        }
    }
    else
    {
        g_diag.pll_result = 3UL; /* PLL failed to stop; config not modified */
        g_diag.pll_rcc_cr = RCC->CR;
    }

    /* Capture Stage 2A result before attempting any clock switch. */
    g_diag.pll_rcc_cr = RCC->CR;
    g_diag.pll_rcc_pllcfgr = RCC->PLLCFGR;
    g_diag.pll_rcc_cfgr = RCC->CFGR;
    g_diag.pll_flash_acr = FLASH->ACR;

    /*
     * Stage 2A.5: validate the clock-switch prerequisites without switching
     * SYSCLK. The target final clock is 96 MHz from HSI-sourced PLL.
     *
     * RM0033 limits: APB1 <= 30 MHz and APB2 <= 60 MHz. Therefore, at
     * 96 MHz use APB1 /4 (24 MHz) and APB2 /2 (48 MHz), not /2 and /1.
     * AHB remains /1. At the current HSI 16 MHz these are conservative
     * bus clocks (APB1=4 MHz, APB2=8 MHz). Flash latency is set to 3 WS
     * for the later 96 MHz step, but the CPU stays on HSI throughout.
     *
     * Crucially, SW[1:0] is never changed here and SysTick remains at 16 MHz.
     */
    g_diag.preflight_result = 0UL;
    g_diag.preflight_sysclk_hz = HSI_VALUE;
    g_diag.preflight_systick_load = SysTick->LOAD;

    if (g_diag.pll_result == 1UL &&
        (RCC->CR & RCC_CR_PLLRDY) != 0UL &&
        (RCC->PLLCFGR & (1UL << 22)) == 0UL)
    {
        /* Set AHB /1, APB1 /4, APB2 /2 while SYSCLK is still HSI. */
        RCC->CFGR = (RCC->CFGR & ~((0xFUL << 4) | (7UL << 10) | (7UL << 13)))
                  | (5UL << 10) | (4UL << 13);
        __DSB();
        __ISB();

        /* Prepare 3 Flash wait states and enable prefetch/I/D caches. */
        FLASH->ACR = (FLASH->ACR & ~0xFUL) | 3UL |
                     (1UL << 8) | (1UL << 9) | (1UL << 10);
        __DSB();
        __ISB();

        g_diag.preflight_rcc_cr = RCC->CR;
        g_diag.preflight_rcc_cfgr = RCC->CFGR;
        g_diag.preflight_flash_acr = FLASH->ACR;
        g_diag.preflight_systick_load = SysTick->LOAD;

        if (((RCC->CFGR >> 2) & 3UL) == 0UL &&
            (RCC->CFGR & 3UL) == 0UL &&
            (RCC->CFGR & ((0xFUL << 4) | (7UL << 10) | (7UL << 13))) ==
                ((5UL << 10) | (4UL << 13)) &&
            (FLASH->ACR & 0xFUL) == 3UL &&
            (FLASH->ACR & ((1UL << 8) | (1UL << 9) | (1UL << 10))) ==
                ((1UL << 8) | (1UL << 9) | (1UL << 10)) &&
            SysTick->LOAD == ((HSI_VALUE / 1000UL) - 1UL))
        {
            g_diag.preflight_result = 1UL; /* prerequisites verified */
        }
        else
        {
            g_diag.preflight_result = 2UL; /* readback mismatch */
        }
    }
    else
    {
        g_diag.preflight_result = 3UL; /* PLL not ready; no SYSCLK change */
        g_diag.preflight_rcc_cr = RCC->CR;
        g_diag.preflight_rcc_cfgr = RCC->CFGR;
        g_diag.preflight_flash_acr = FLASH->ACR;
    }

    /* Final capture: must still be HSI 16 MHz; do not touch SW or SysTick. */
    g_diag.preflight_rcc_cr = RCC->CR;
    g_diag.preflight_rcc_cfgr = RCC->CFGR;
    g_diag.preflight_flash_acr = FLASH->ACR;
    g_diag.preflight_systick_load = SysTick->LOAD;

    g_diag.status = DIAG_STATUS_RUNNING;

    for (;;)
    {
        ++g_diag.loop_counter;

        if ((uint32_t)(g_tick_ms - last_heartbeat) >= 1000UL)
        {
            last_heartbeat += 1000UL;
            g_diag.heartbeat++;
            g_diag.last_heartbeat_ms = g_tick_ms;
        }

        __NOP();
    }
}
