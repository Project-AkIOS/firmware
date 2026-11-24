/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * readl/writel for Samsung's UFS CAL (ufs-cal-if.c, built as for the kernel), on EDK2's IoLib (which fences around
 * each access, as Linux's readl/writel do).
 *
 * Copyright (c) 2026, AkIOS contributors.
 */
#ifndef _ZUMA_UFS_CAL_LINUX_IO_H_
#define _ZUMA_UFS_CAL_LINUX_IO_H_

#include <linux/types.h>
#include <Library/IoLib.h>

#define readl(Address)         MmioRead32 ((UINTN)(Address))
#define writel(Value, Address) MmioWrite32 ((UINTN)(Address), (Value))

#endif
