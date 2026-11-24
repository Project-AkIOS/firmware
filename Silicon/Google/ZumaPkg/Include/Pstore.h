/**
  The phone's ramoops region (ramoops_mem@fd3ff000: console 2 MiB, then pmsg 2 MiB, no dump records): the firmware
  writes its log into the pmsg zone, which Android shows after the next start as /sys/fs/pstore/pmsg-ramoops-0. A zone
  starts with Linux's persistent RAM header: signature "DBGC", the write position, the bytes held; the text follows
  as a ring.

  Copyright (c) 2026, AkIOS contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _ZUMA_PSTORE_H_
#define _ZUMA_PSTORE_H_

#include <Base.h>

#define PSTORE_PMSG_BASE  0xFD5FF000ULL
#define PSTORE_PMSG_SIZE  0x00200000ULL
#define PSTORE_SIGNATURE  0x43474244u     // "DBGC"

typedef struct {
  UINT32  Signature;
  UINT32  Start;          // next byte written, as an offset into Data
  UINT32  Size;           // bytes held (up to the zone less this header)
  UINT8   Data[];
} PSTORE_ZONE;

#define PSTORE_DATA_SIZE  ((UINT32)(PSTORE_PMSG_SIZE - sizeof (PSTORE_ZONE)))

#endif
