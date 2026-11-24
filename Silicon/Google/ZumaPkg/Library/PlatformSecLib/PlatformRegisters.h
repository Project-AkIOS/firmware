/**
  Google Tensor G3 (zuma) registers the platform SEC library touches.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _PLATFORM_REGISTERS_H_
#define _PLATFORM_REGISTERS_H_

//
// Samsung watchdog (watchdog_cl0@10060000, watchdog_cl1@10070000): WTCON, bit 5 the counter, bit 0 the reset output
//
#define WDT_WTCON     0x00
#define WTCON_ENABLE  (1 << 5)
#define WTCON_RESET   (1 << 0)

//
// DECON0 (drmdecon@19470000): TRIG_CON as the bootloader leaves it (the hardware trigger masked) and unmasked
//
#define DECON_TRIG_CON          0x30
#define DECON_TRIG_MASKED       0x3070
#define DECON_TRIG_HW_UNMASKED  0x3061

#endif /* _PLATFORM_REGISTERS_H_ */
