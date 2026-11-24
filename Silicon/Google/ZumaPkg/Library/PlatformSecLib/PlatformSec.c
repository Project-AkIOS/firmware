/**
  Google Tensor G3 (zuma): what the firmware sets up before the DXE phase, as Google's bootloader left the SoC.

  - the watchdogs of both CPU clusters: the bootloader leaves one counting for the kernel (bring-up: left on as the
    failsafe, see PlatformInitialize)
  - the log: the ramoops pmsg zone is emptied (the serial library writes the firmware's output there)
  - DECON0's trigger: the panel runs in DSI command mode and the bootloader leaves DECON0's hardware trigger masked
    after its splash (TRIG_CON 0x3070); unmasking it (0x3061) makes DECON0 send the framebuffer at every TE again, so
    what the firmware draws reaches the panel. Only that exact value is changed.

  Copyright (c) 2011-2012, ARM Limited. All rights reserved.
  Copyright (c) 2026, AkIOS contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/IoLib.h>
#include <Library/PlatformSecLib.h>
#include <Library/MemoryMapHelperLib.h>

#include <Pstore.h>

#include "PlatformRegisters.h"

//
// The firmware's log starts empty at every boot (each module's serial library only appends to it).
//
STATIC
VOID
ResetPstoreLog (
  VOID
  )
{
  volatile PSTORE_ZONE  *Zone = (volatile PSTORE_ZONE *)(UINTN)PSTORE_PMSG_BASE;

  Zone->Start     = 0;
  Zone->Size      = 0;
  Zone->Signature = PSTORE_SIGNATURE;
}

STATIC
VOID
StopWatchdog (
  IN CONST CHAR8  *Name
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  Region;

  if (!EFI_ERROR (LocateMemoryRegionByName ((CHAR8 *)Name, &Region))) {
    MmioAnd32 (Region.Address + WDT_WTCON, ~(UINT32)(WTCON_ENABLE | WTCON_RESET));
  }
}

STATIC
VOID
UnmaskDisplayTrigger (
  VOID
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  Decon;

  if (EFI_ERROR (LocateMemoryRegionByName ("DECON0", &Decon))) {
    return;
  }

  if (MmioRead32 (Decon.Address + DECON_TRIG_CON) == DECON_TRIG_MASKED) {
    MmioWrite32 (Decon.Address + DECON_TRIG_CON, DECON_TRIG_HW_UNMASKED);
  }
}

VOID
PlatformInitialize (
  VOID
  )
{
  ResetPstoreLog ();
  //
  // (bring-up) the watchdog the bootloader leaves counting stays on: it restarts the phone about two minutes after
  // the hand-over, into Android, whenever the firmware does not get further -- the failsafe until the firmware has
  // its own. The watchdogs are stopped once XNU is started by a firmware that knows it got there.
  //
  if (FALSE) {
    StopWatchdog ("Watchdog CL0");
    StopWatchdog ("Watchdog CL1");
  }
  UnmaskDisplayTrigger ();
}
