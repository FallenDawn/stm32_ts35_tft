/*
 * JZ-TS35 V2.1 compatibility shim.
 *
 * PlatformIO's STM32CubeF2 1.9.6 LL FSMC header refers to the historical
 * FSMC_Bank2_3_TypeDef name, while its bundled STM32F207 CMSIS device header
 * exposes the compatible register layout as FSMC_Bank3_TypeDef.
 *
 * This macro is intentionally included before framework headers. It does not
 * change peripheral addresses or register layout.
 */
#ifndef JZ_TS35_STM32F2XX_COMPAT_H
#define JZ_TS35_STM32F2XX_COMPAT_H

#ifndef FSMC_Bank2_3_TypeDef
#define FSMC_Bank2_3_TypeDef FSMC_Bank3_TypeDef
#endif

#endif
