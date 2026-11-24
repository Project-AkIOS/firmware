/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Samsung's UFS CAL (ufs-cal-if.c, built as for the kernel) maps its printf to pr_info; it prints nothing, but
 * the name has to exist.
 *
 * Copyright (c) 2026, AkIOS contributors.
 */
#ifndef _ZUMA_UFS_CAL_LINUX_KERNEL_H_
#define _ZUMA_UFS_CAL_LINUX_KERNEL_H_

#include <Library/DebugLib.h>

#define pr_info(...)  DEBUG ((DEBUG_INFO, __VA_ARGS__))

#endif
