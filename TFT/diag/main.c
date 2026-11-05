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
