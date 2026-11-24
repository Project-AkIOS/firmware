/**
  zuma's UFS host: the vendor steps around the standard UFSHCI bring-up (controller enable, link startup, power mode
  change), with Samsung's PHY calibration (CAL). Declarations only: the library behind them (ZumaPkg/Library/
  ZumaUfsCalLib) is GPL-2.0-only, kept apart from the BSD driver that calls it.

  The order, as Google's Linux driver has it:
    ZumaUfsHostInit
    HCE 0, ZumaUfsHostPreHce, HCE 1 (and wait), ZumaUfsHostPostHce
    ZumaUfsHostPreLink, DME_LINKSTARTUP, ZumaUfsHostPostLink
    (NOP OUT, fDeviceInit)
    ZumaUfsHostPrePmc, the PA_* attributes and PA_PWRMODE, ZumaUfsHostPostPmc

  Copyright (c) 2026, AkIOS contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _ZUMA_UFS_CAL_LIB_H_
#define _ZUMA_UFS_CAL_LIB_H_

#include <Uefi.h>

typedef struct {
  UINTN     Std;          // the standard UFSHCI registers
  UINTN     Hci;          // Samsung's vendor HCI registers
  UINTN     Unipro;
  UINTN     Ufsp;         // the UFS protector
  UINTN     Pma;          // the PHY
  UINTN     Cport;
  UINT32    MclkRate;     // UniPro's mclk, Hz
  UINT8     Board;        // the tree's brd-for-cal
  UINT8     EvtVer;       // the tree's evt-ver (0 when it has none)
  UINT8     Gear;         // the tree's ufs,pmd-attr-gear: the HS gear the host is meant to reach
} ZUMA_UFS_HOST_CONFIG;

//
// Power modes (PA_PWRMODE) and HS series
//
#define ZUMA_UFS_FAST_MODE      1
#define ZUMA_UFS_SLOW_MODE      2
#define ZUMA_UFS_FASTAUTO_MODE  4
#define ZUMA_UFS_SLOWAUTO_MODE  5
#define ZUMA_UFS_HS_SERIES_A    1
#define ZUMA_UFS_HS_SERIES_B    2

EFI_STATUS
EFIAPI
ZumaUfsHostInit (
  IN CONST ZUMA_UFS_HOST_CONFIG  *Config
  );

/**
  Before HCE is set: the host's logic reset (UniPro and link), its vendor configuration, and the device's reset pulse.
**/
EFI_STATUS
EFIAPI
ZumaUfsHostPreHce (
  VOID
  );

/**
  Once HCE reads 1: clock control, and the host's available lanes.
**/
EFI_STATUS
EFIAPI
ZumaUfsHostPostHce (
  VOID
  );

/**
  Before DME_LINKSTARTUP: error reporting and the CAL's PHY and UniPro setup.
**/
EFI_STATUS
EFIAPI
ZumaUfsHostPreLink (
  VOID
  );

/**
  After a successful DME_LINKSTARTUP: the lanes the link connected, and the CAL's post-link setup.
**/
EFI_STATUS
EFIAPI
ZumaUfsHostPostLink (
  OUT UINT8  *ConnectedLanes OPTIONAL,
  OUT UINT8  *MaxHsGear OPTIONAL
  );

/**
  Before the power mode change (before the PA_* attributes are set).
**/
EFI_STATUS
EFIAPI
ZumaUfsHostPrePmc (
  IN UINT8  Mode,
  IN UINT8  Gear,
  IN UINT8  Lanes,
  IN UINT8  HsSeries
  );

/**
  After the power mode change completed.
**/
EFI_STATUS
EFIAPI
ZumaUfsHostPostPmc (
  VOID
  );

#endif
