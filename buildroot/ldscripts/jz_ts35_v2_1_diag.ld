/*
 * Standalone diagnostic linker script for JZ-TS35 V2.1.
 * Direct-boot layout: vectors at 0x08000000, reserved settings sector
 * at 0x08004000, application code at 0x08008000.
 */
ENTRY(Reset_Handler)

_estack = 0x20020000;

MEMORY
{
    RAM         (xrw) : ORIGIN = 0x20000000, LENGTH = 64K /* Upper 64 KiB reserved for SWD dump buffer/mailbox/stack. */
    FLASH_VECTOR (rx) : ORIGIN = 0x08000000, LENGTH = 16K
    FLASH_APP    (rx) : ORIGIN = 0x08008000, LENGTH = 256K - 32K
}

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH_VECTOR

    .text :
    {
        . = ALIGN(4);
        *(.text.Reset_Handler)
        *(.text*)
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH_APP

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH_APP

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH_APP

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH_APP

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    . = ALIGN(4);
    _end = .;
    end = .;

    .ARM.attributes 0 :
    {
        *(.ARM.attributes)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }
}
