/**
  zuma's UFS host controller (ufs@13200000) for the firmware, as an EFI SCSI pass-through: EDK2's SCSI bus and disk
  drivers make a block device of each logical unit on top of it, its partition driver the GPT partitions.

  Google's bootloader reads the boot image from UFS, so when the firmware starts the controller is enabled, the link
  is up (HS gear 4, two lanes) and its transfer request list runs, in the bootloader's memory. The firmware takes it
  over as it is: with nothing in flight the list is stopped, pointed at a list of the firmware's own, and started
  again. No reset, no link startup, no UIC command, no PHY, clock or power setting; the task management list is not
  touched.

  Read only for now: only commands that read (and the ones that ask the device about itself) reach the device; a
  write is refused here, whatever the layers above ask.

  Copyright (c) 2026, AkIOS contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <IndustryStandard/Scsi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/ScsiPassThruExt.h>

#include "Ufshci.h"

#define UFS_LUNS       8                // the logical units offered (the phone has 0-3: user, boot A/B, a small one)
#define IO_ALIGN       64               // the cache line: buffers the controller writes into must not share one
#define DEFAULT_WAIT   (5 * 1000 * 1000)  // microseconds, when a request names no timeout

STATIC UINTN       mBase;
STATIC UTRD        *mList;              // 32 descriptors, 1 KiB aligned; the driver uses slot 0
STATIC UINT8       *mUcd;               // slot 0's command descriptor, 128-byte aligned
STATIC UINT8       mTag;
STATIC UINTN       mDumps;

//
// (bring-up) the vendor state that decides whether the controller takes requests: clock stops, auto-Hibern8,
// its state machine and DMA monitors
//
STATIC
VOID
DumpState (
  IN CONST CHAR8  *When
  )
{
  UINTN  V = mBase + UFS_VS_HCI;

  DEBUG ((DEBUG_WARN, "ZumaUfs: [%a] AHIT 0x%08x CLKSTOP_CTRL 0x%08x FORCE_HCS 0x%08x ACG_DISABLE 0x%08x AH8_STATE 0x%08x\n",
    When, MmioRead32 (mBase + 0x18), MmioRead32 (V + 0xB0), MmioRead32 (V + 0xB4), MmioRead32 (V + 0xFC), MmioRead32 (V + 0x50C)));
  DEBUG ((DEBUG_WARN, "ZumaUfs: [%a] FSM 0x%08x DMA0 0x%08x/0x%08x DMA1 0x%08x DBR_DEBUG 0x%08x/0x%08x DBR 0x%08x HCS 0x%08x IS 0x%08x\n",
    When, MmioRead32 (V + 0xC0), MmioRead32 (V + 0xC8), MmioRead32 (V + 0xCC), MmioRead32 (V + 0xD0), MmioRead32 (V + 0xD8),
    MmioRead32 (V + 0xDC), MmioRead32 (mBase + UFS_UTRLDBR), MmioRead32 (mBase + UFS_HCS), MmioRead32 (mBase + UFS_IS)));
  {
    //
    // the S2MPU in front of HSI2 (s2mpu_s0_hsi2@131f0000), where UFS sits: whether it lets the controller's DMA through
    //
    UINTN   S = 0x131F0000;
    UINT32  Vid;

    DEBUG ((DEBUG_WARN, "ZumaUfs: [%a] S2MPU HSI2: reading VERSION\n", When));
    DEBUG ((DEBUG_WARN, "ZumaUfs: [%a] S2MPU HSI2: VERSION 0x%08x\n", When, MmioRead32 (S + 0x60)));
    DEBUG ((DEBUG_WARN, "ZumaUfs: [%a] S2MPU HSI2: CTRL0 0x%08x\n", When, MmioRead32 (S + 0x0)));
    DEBUG ((DEBUG_WARN, "ZumaUfs: [%a] S2MPU HSI2: PROT_EN_PER_VID 0x%08x STATUS 0x%08x\n", When, MmioRead32 (S + 0x50),
      MmioRead32 (S + 0x68)));
    DEBUG ((DEBUG_WARN, "ZumaUfs: [%a] S2MPU HSI2: FAULT_STATUS 0x%08x\n", When, MmioRead32 (S + 0x2000)));
    (VOID)Vid;
  }
}
STATIC BOOLEAN     mInPlace = FALSE;             // (bring-up test, see TakeOver)
STATIC UINT8       *mBounce;
#define ABL_UCD     0xF9400000ULL
#define ABL_BOUNCE  0xF9500000ULL
#define BOUNCE_SIZE SIZE_4MB

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} ZUMA_UFS_PATH;
#pragma pack()

STATIC ZUMA_UFS_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0x5b6e3a14, 0xc2d8, 0x4f07, { 0x93, 0x1a, 0x6e, 0x04, 0xb2, 0x8c, 0x5d, 0x71 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

//
// What reaches the device: reads and questions. Anything else is refused before it is built.
//
STATIC
BOOLEAN
Permitted (
  IN CONST UINT8  *Cdb
  )
{
  switch (Cdb[0]) {
    case EFI_SCSI_OP_TEST_UNIT_READY:
    case EFI_SCSI_OP_REQUEST_SENSE:
    case EFI_SCSI_OP_INQUIRY:
    case EFI_SCSI_OP_MODE_SEN6:
    case EFI_SCSI_OP_MODE_SEN10:
    case EFI_SCSI_OP_READ_CAPACITY:
    case EFI_SCSI_OP_READ10:
    case EFI_SCSI_OP_READ16:
      return TRUE;
    case EFI_SCSI_OP_READ_CAPACITY16:       // SERVICE ACTION IN(16): only READ CAPACITY(16)
      return (Cdb[1] & 0x1F) == 0x10;
    default:
      return FALSE;
  }
}

STATIC
VOID
Flush (
  IN VOID   *Address,
  IN UINTN  Length
  )
{
  WriteBackDataCacheRange (Address, Length);
}

STATIC
VOID
Discard (
  IN VOID   *Address,
  IN UINTN  Length
  )
{
  InvalidateDataCacheRange (Address, Length);
}

//
// One request through slot 0, waited for. The data buffer must be IO_ALIGN-aligned (the mode says so).
//
STATIC
EFI_STATUS
Transfer (
  IN     UINT8                                       Lun,
  IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet
  )
{
  COMMAND_UPIU   *Command;
  RESPONSE_UPIU  *Response;
  PRDT_ENTRY     *Prdt;
  UTRD           *Utrd;
  UINT8          *Data;
  UINT32         Length;
  UINT32         Left;
  UINTN          Entries;
  BOOLEAN        Read;
  UINT64         Waited;
  UINT64         Limit;
  UINT32         Is;
  UINTN          SenseBytes;
  UINT8          *Caller = NULL;

  if ((Packet->CdbLength == 0) || (Packet->CdbLength > 16) || (Packet->Cdb == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Permitted (Packet->Cdb) || (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_BIDIRECTIONAL)) {
    return EFI_UNSUPPORTED;
  }

  Read   = (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ);
  Length = Read ? Packet->InTransferLength : Packet->OutTransferLength;
  Data   = Read ? Packet->InDataBuffer : Packet->OutDataBuffer;
  if (!Read && (Length != 0)) {
    return EFI_UNSUPPORTED;                // a command that sends data: none is permitted yet
  }

  if ((Length != 0) && ((Data == NULL) || (((UINTN)Data & (IO_ALIGN - 1)) != 0) || ((Length & 3) != 0) ||
                        (Length > PRDT_ENTRIES * PRDT_MAX_BYTES)))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (MmioRead32 (mBase + UFS_UTRLDBR) & 1) {
    return EFI_DEVICE_ERROR;                // slot 0 is busy: something went wrong before
  }

  //
  // The command descriptor
  //
  ZeroMem (mUcd, UCD_SIZE);
  Command                 = (COMMAND_UPIU *)(mUcd + UCD_COMMAND_OFFSET);
  Response                = (RESPONSE_UPIU *)(mUcd + UCD_RESPONSE_OFFSET);
  Prdt                    = (PRDT_ENTRY *)(mUcd + UCD_PRDT_OFFSET);
  Command->Type           = UPIU_COMMAND;
  Command->Flags          = (Length != 0) ? UPIU_FLAG_READ : 0;
  Command->Lun            = Lun;
  Command->TaskTag        = ++mTag;
  Command->ExpectedLength = SwapBytes32 (Length);
  CopyMem (Command->Cdb, Packet->Cdb, Packet->CdbLength);

  if ((mBounce != NULL) && (Length != 0)) {
    if (Length > BOUNCE_SIZE) {
      return EFI_BAD_BUFFER_SIZE;
    }

    Caller = Data;
    Data   = mBounce;
  }

  Entries = 0;
  for (Left = Length; Left > 0; Entries++) {
    UINT32  Chunk = MIN (Left, (UINT32)PRDT_MAX_BYTES);
    UINT64  At    = (UINT64)(UINTN)Data + (Length - Left);

    Prdt[Entries].Address     = (UINT32)At;
    Prdt[Entries].AddressHigh = (UINT32)(At >> 32);
    Prdt[Entries].ByteCount   = Chunk - 1;
    Left                     -= Chunk;
  }

  //
  // Slot 0's descriptor
  //
  Utrd = &mList[0];
  ZeroMem (Utrd, sizeof (*Utrd));
  Utrd->Dw0         = UTRD_CT_UFS | UTRD_INTERRUPT | ((Length != 0) ? UTRD_DD_FROM_DEVICE : UTRD_DD_NONE);
  Utrd->Dw2         = OCS_INVALID;
  Utrd->UcdBase     = (UINT32)(UINTN)mUcd;
  Utrd->UcdBaseHigh = (UINT32)((UINT64)(UINTN)mUcd >> 32);
  Utrd->RespOffset  = UCD_RESPONSE_OFFSET / 4;
  Utrd->RespLength  = 64 / 4;                         // (as the bootloader: room for the sense data)
  Utrd->PrdtOffset  = UCD_PRDT_OFFSET / 4;
  Utrd->PrdtLength  = (UINT16)Entries;

  Flush (mUcd, UCD_SIZE);
  Flush (Utrd, sizeof (*Utrd));
  if (Length != 0) {
    WriteBackInvalidateDataCacheRange (Data, Length);   // no dirty line may land on what the device writes
  }

  MmioWrite32 (mBase + UFS_IS, IS_UTRCS);
  MmioOr32 (mBase + UFS_VS_HCI + VS_UTRL_NEXUS_TYPE, 1);    // slot 0 carries a SCSI command
  MmioWrite32 (mBase + UFS_UTRLDBR, 1);
  if (mDumps < 2) {
    MicroSecondDelay (1000);
    DumpState ("1 ms after the doorbell");
    mDumps++;
  }

  Limit = (Packet->Timeout != 0) ? DivU64x32 (Packet->Timeout, 10) : DEFAULT_WAIT;
  for (Waited = 0; (MmioRead32 (mBase + UFS_UTRLDBR) & 1) && (Waited < Limit); Waited += 10) {
    MicroSecondDelay (10);
  }

  Is = MmioRead32 (mBase + UFS_IS);
  if (MmioRead32 (mBase + UFS_UTRLDBR) & 1) {
    MmioWrite32 (mBase + UFS_UTRLCLR, ~1u);           // take slot 0 back
    DEBUG ((DEBUG_ERROR, "ZumaUfs: LUN %u op 0x%02x: no answer in %lu us (IS 0x%08x)\n", Lun, ((UINT8 *)Packet->Cdb)[0], Limit, Is));
    return EFI_TIMEOUT;
  }

  MmioWrite32 (mBase + UFS_IS, IS_UTRCS);
  Discard (Utrd, sizeof (*Utrd));
  Discard (mUcd, UCD_SIZE);
  if (Length != 0) {
    Discard (Data, Length);
    if (Caller != NULL) {
      CopyMem (Caller, Data, Length);
    }
  }

  if ((Utrd->Dw2 & 0xFF) != OCS_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "ZumaUfs: LUN %u op 0x%02x: OCS 0x%x (IS 0x%08x)\n", Lun, ((UINT8 *)Packet->Cdb)[0], Utrd->Dw2 & 0xFF, Is));
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OTHER;
    return EFI_DEVICE_ERROR;
  }

  Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK;
  Packet->TargetStatus      = Response->Status;
  SenseBytes                = MIN ((UINTN)SwapBytes16 (Response->SenseLength), sizeof (Response->Sense));
  if ((Response->Status != EFI_EXT_SCSI_STATUS_TARGET_GOOD) && (Packet->SenseData != NULL) && (SenseBytes != 0)) {
    SenseBytes = MIN (SenseBytes, (UINTN)Packet->SenseDataLength);
    CopyMem (Packet->SenseData, Response->Sense, SenseBytes);
    Packet->SenseDataLength = (UINT8)SenseBytes;
  } else {
    Packet->SenseDataLength = 0;
  }

  if (Read) {
    Packet->InTransferLength = Length - MIN (Length, SwapBytes32 (Response->ResidualCount));
  }

  return EFI_SUCCESS;
}

//
// EFI_EXT_SCSI_PASS_THRU_PROTOCOL: one target (id 0), its logical units 0..UFS_LUNS-1
//
STATIC
EFI_STATUS
EFIAPI
UfsPassThru (
  IN     EFI_EXT_SCSI_PASS_THRU_PROTOCOL             *This,
  IN     UINT8                                       *Target,
  IN     UINT64                                      Lun,
  IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet,
  IN     EFI_EVENT                                   Event OPTIONAL
  )
{
  UINTN  Index;

  for (Index = 0; Index < TARGET_MAX_BYTES; Index++) {
    if (Target[Index] != 0) {
      return EFI_INVALID_PARAMETER;
    }
  }

  if (Lun >= UFS_LUNS) {
    return EFI_INVALID_PARAMETER;
  }

  return Transfer ((UINT8)Lun, Packet);      // (synchronous: a non-blocking caller gets it done all the same)
}

STATIC
EFI_STATUS
EFIAPI
UfsGetNextTargetLun (
  IN     EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This,
  IN OUT UINT8                            **Target,
  IN OUT UINT64                           *Lun
  )
{
  UINTN  Index;
  BOOLEAN First = TRUE;

  for (Index = 0; Index < TARGET_MAX_BYTES; Index++) {
    if ((*Target)[Index] != 0xFF) {
      First = FALSE;
    }
  }

  if (First) {
    ZeroMem (*Target, TARGET_MAX_BYTES);
    *Lun = 0;
    return EFI_SUCCESS;
  }

  for (Index = 0; Index < TARGET_MAX_BYTES; Index++) {
    if ((*Target)[Index] != 0) {
      return EFI_INVALID_PARAMETER;
    }
  }

  if (*Lun + 1 >= UFS_LUNS) {
    return EFI_NOT_FOUND;
  }

  (*Lun)++;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UfsBuildDevicePath (
  IN     EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This,
  IN     UINT8                            *Target,
  IN     UINT64                           Lun,
  IN OUT EFI_DEVICE_PATH_PROTOCOL         **DevicePath
  )
{
  SCSI_DEVICE_PATH  *Node;

  if ((Target[0] != 0) || (Lun >= UFS_LUNS)) {
    return EFI_NOT_FOUND;
  }

  Node = (SCSI_DEVICE_PATH *)CreateDeviceNode (MESSAGING_DEVICE_PATH, MSG_SCSI_DP, sizeof (SCSI_DEVICE_PATH));
  if (Node == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Node->Pun   = 0;
  Node->Lun   = (UINT16)Lun;
  *DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)Node;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UfsGetTargetLun (
  IN  EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This,
  IN  EFI_DEVICE_PATH_PROTOCOL         *DevicePath,
  OUT UINT8                            **Target,
  OUT UINT64                           *Lun
  )
{
  SCSI_DEVICE_PATH  *Node = (SCSI_DEVICE_PATH *)DevicePath;

  if ((DevicePath == NULL) || (Target == NULL) || (Lun == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((DevicePathType (DevicePath) != MESSAGING_DEVICE_PATH) || (DevicePathSubType (DevicePath) != MSG_SCSI_DP) ||
      (Node->Pun != 0) || (Node->Lun >= UFS_LUNS))
  {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (*Target, TARGET_MAX_BYTES);
  *Lun = Node->Lun;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UfsResetChannel (
  IN EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This
  )
{
  return EFI_UNSUPPORTED;         // never: the link is the bootloader's, and it works
}

STATIC
EFI_STATUS
EFIAPI
UfsResetTargetLun (
  IN EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This,
  IN UINT8                            *Target,
  IN UINT64                           Lun
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
UfsGetNextTarget (
  IN     EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This,
  IN OUT UINT8                            **Target
  )
{
  UINTN  Index;

  for (Index = 0; Index < TARGET_MAX_BYTES; Index++) {
    if ((*Target)[Index] != 0xFF) {
      return EFI_NOT_FOUND;       // target 0 was the only one
    }
  }

  ZeroMem (*Target, TARGET_MAX_BYTES);
  return EFI_SUCCESS;
}

STATIC EFI_EXT_SCSI_PASS_THRU_MODE  mMode = {
  0xFFFFFFFF,                     // the initiator has no id of its own
  EFI_EXT_SCSI_PASS_THRU_ATTRIBUTES_PHYSICAL | EFI_EXT_SCSI_PASS_THRU_ATTRIBUTES_LOGICAL,
  IO_ALIGN
};

STATIC EFI_EXT_SCSI_PASS_THRU_PROTOCOL  mPassThru = {
  &mMode,
  UfsPassThru,
  UfsGetNextTargetLun,
  UfsBuildDevicePath,
  UfsGetTargetLun,
  UfsResetChannel,
  UfsResetTargetLun,
  UfsGetNextTarget
};


//
// ---- bringing the device back ----------------------------------------------------------------------------------
//
// Google's bootloader leaves the host as it used it (enabled, its PHY set up for HS gear 4) but switches the device off
// before it hands over: its supply (gpp0-1 of pinctrl@10840000) low, its reset line (gph5-0 of pinctrl@13060000) a GPIO
// driven low, the controller's own reset output (vendor GPIO_OUT) 0. The device is powered and released again, and the
// link started from the host's side, with the PHY as the bootloader set it: no host reset, no PHY setting.
//
#define GPIO_ALIVE_GPP0     0x10840000ULL   // CON, DAT at +4
#define GPIO_HSI2_GPH5      0x13060000ULL
#define UFS_VCC_PIN         1
#define GPH5_UFS_FUNCTIONS  0x22            // gph5-0 and gph5-1 to their UFS function (2): the reset and the refclk

STATIC
EFI_STATUS
UicCommand (
  IN  UINT32  Command,
  IN  UINT32  Arg1,
  IN  UINT32  Arg2,
  IN  UINT32  Arg3,
  OUT UINT32  *Result OPTIONAL,
  OUT UINT32  *Value OPTIONAL
  )
{
  UINTN  Waited;

  for (Waited = 0; (MmioRead32 (mBase + UFS_HCS) & HCS_UCRDY) == 0; Waited += 10) {
    if (Waited > 100000) {
      return EFI_NOT_READY;
    }

    MicroSecondDelay (10);
  }

  MmioWrite32 (mBase + UFS_IS, IS_UCCS);
  MmioWrite32 (mBase + UFS_UICARG1, Arg1);
  MmioWrite32 (mBase + UFS_UICARG2, Arg2);
  MmioWrite32 (mBase + UFS_UICARG3, Arg3);
  MmioWrite32 (mBase + UFS_UICCMD, Command);
  for (Waited = 0; (MmioRead32 (mBase + UFS_IS) & IS_UCCS) == 0; Waited += 10) {
    if (Waited > 500000) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (10);
  }

  MmioWrite32 (mBase + UFS_IS, IS_UCCS);
  if (Result != NULL) {
    *Result = MmioRead32 (mBase + UFS_UICARG2) & 0xFF;
  }

  if (Value != NULL) {
    *Value = MmioRead32 (mBase + UFS_UICARG3);
  }

  return ((MmioRead32 (mBase + UFS_UICARG2) & 0xFF) == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
DeviceUp (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Result;
  UINTN       Try;

  MmioOr32 (GPIO_ALIVE_GPP0 + 4, 1u << UFS_VCC_PIN);                    // the supply on
  MicroSecondDelay (20000);
  MmioWrite32 (GPIO_HSI2_GPH5, (MmioRead32 (GPIO_HSI2_GPH5) & ~0xFFu) | GPH5_UFS_FUNCTIONS);
  MmioWrite32 (mBase + UFS_VS_HCI + VS_GPIO_OUT, 0);
  MicroSecondDelay (10);
  MmioWrite32 (mBase + UFS_VS_HCI + VS_GPIO_OUT, 1);                    // out of reset
  MicroSecondDelay (1000);

  Status = EFI_DEVICE_ERROR;
  for (Try = 0; Try < 4; Try++) {
    if (Try == 1) {
      //
      // (bring-up) the host's UniPro may be left in Hibern8 by the bootloader: the controller reset (HCE 0 -> 1) brings
      // it back to its initial state; the PHY's registers are a block of their own and keep what the bootloader set
      //
      UINTN  Waited;

      MmioWrite32 (mBase + UFS_HCE, 0);
      for (Waited = 0; (MmioRead32 (mBase + UFS_HCE) & 1) && Waited < 100000; Waited += 10) {
        MicroSecondDelay (10);
      }

      MicroSecondDelay (1000);
      MmioWrite32 (mBase + UFS_HCE, 1);
      for (Waited = 0; !(MmioRead32 (mBase + UFS_HCE) & 1) && Waited < 100000; Waited += 10) {
        MicroSecondDelay (10);
      }

      DEBUG ((DEBUG_WARN, "ZumaUfs: host reset: HCE 0x%x HCS 0x%08x\n", MmioRead32 (mBase + UFS_HCE), MmioRead32 (mBase + UFS_HCS)));
    }

    Result = 0xFF;
    Status = UicCommand (UIC_DME_LINKSTARTUP, 0, 0, 0, &Result, NULL);
    DEBUG ((DEBUG_WARN, "ZumaUfs: link startup %u: %r (result %u), HCS 0x%08x IS 0x%08x\n", Try + 1, Status, Result,
      MmioRead32 (mBase + UFS_HCS), MmioRead32 (mBase + UFS_IS)));
    if (!EFI_ERROR (Status) && (MmioRead32 (mBase + UFS_HCS) & HCS_DP)) {
      break;
    }

    MicroSecondDelay (10000);
  }

  return Status;
}

//
// A NOP or query UPIU through slot 1 (its nexus type cleared: not a SCSI command). RESPONSE gets the 32-byte answer.
//
STATIC
EFI_STATUS
SendUpiu (
  IN  CONST UINT8  *Request,
  OUT UINT8        *Response
  )
{
  UTRD    *Utrd = &mList[1];
  UINT8   *Ucd  = mUcd + ALIGN_VALUE (UCD_SIZE, SIZE_4KB);    // slot 1's command descriptor, past slot 0's
  UINTN   Waited;

  ZeroMem (Ucd, 0x80);
  CopyMem (Ucd, Request, 32);
  ((UINT8 *)Ucd)[3] = ++mTag;
  ZeroMem (Utrd, sizeof (*Utrd));
  Utrd->Dw0         = UTRD_CT_UFS | UTRD_INTERRUPT | UTRD_DD_NONE;
  Utrd->Dw2         = OCS_INVALID;
  Utrd->UcdBase     = (UINT32)(UINTN)Ucd;
  Utrd->UcdBaseHigh = (UINT32)((UINT64)(UINTN)Ucd >> 32);
  Utrd->RespOffset  = 0x20 / 4;
  Utrd->RespLength  = 0x20 / 4;
  Flush (Ucd, 0x80);
  Flush (Utrd, sizeof (*Utrd));

  MmioAnd32 (mBase + UFS_VS_HCI + VS_UTRL_NEXUS_TYPE, ~(UINT32)(1u << 1));
  MmioWrite32 (mBase + UFS_IS, IS_UTRCS);
  MmioWrite32 (mBase + UFS_UTRLDBR, 1u << 1);
  for (Waited = 0; (MmioRead32 (mBase + UFS_UTRLDBR) & (1u << 1)) != 0; Waited += 10) {
    if (Waited > 500000) {
      MmioWrite32 (mBase + UFS_UTRLCLR, ~(1u << 1));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (10);
  }

  MmioWrite32 (mBase + UFS_IS, IS_UTRCS);
  Discard (Utrd, sizeof (*Utrd));
  Discard (Ucd, 0x80);
  CopyMem (Response, Ucd + 0x20, 32);
  return ((Utrd->Dw2 & 0xFF) == OCS_SUCCESS) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
DeviceInit (
  VOID
  )
{
  UINT8       Request[32];
  UINT8       Response[32];
  EFI_STATUS  Status;
  UINTN       Try;

  ZeroMem (Request, sizeof (Request));
  Request[0] = UPIU_NOP_OUT;
  for (Try = 0; Try < 10; Try++) {
    Status = SendUpiu (Request, Response);
    if (!EFI_ERROR (Status) && (Response[0] == UPIU_NOP_IN)) {
      break;
    }

    MicroSecondDelay (1000);
  }

  DEBUG ((DEBUG_WARN, "ZumaUfs: NOP OUT -> %r, answer type 0x%02x (after %u tries)\n", Status, Response[0], Try + 1));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (Request, sizeof (Request));
  Request[0]  = UPIU_QUERY;
  Request[5]  = QUERY_FN_WRITE;
  Request[12] = QUERY_OP_SET_FLAG;
  Request[13] = FLAG_DEVICE_INIT;
  Status      = SendUpiu (Request, Response);
  DEBUG ((DEBUG_WARN, "ZumaUfs: set fDeviceInit -> %r, response 0x%02x\n", Status, Response[6]));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Try = 0; Try < 1000; Try++) {
    ZeroMem (Request, sizeof (Request));
    Request[0]  = UPIU_QUERY;
    Request[5]  = QUERY_FN_READ;
    Request[12] = QUERY_OP_READ_FLAG;
    Request[13] = FLAG_DEVICE_INIT;
    Status      = SendUpiu (Request, Response);
    if (!EFI_ERROR (Status) && (Response[23] == 0)) {
      DEBUG ((DEBUG_WARN, "ZumaUfs: the device is initialised (after %u ms)\n", Try));
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1000);
  }

  DEBUG ((DEBUG_ERROR, "ZumaUfs: fDeviceInit not cleared: %r, flag %u\n", Status, Response[23]));
  return EFI_TIMEOUT;
}

//
// The takeover: the bootloader's list out, the firmware's in, with nothing in flight
//
STATIC
EFI_STATUS
TakeOver (
  VOID
  )
{
  UINT32  Hcs;
  UINTN   Waited;
  UINT64  Old;

  DumpState ("as left");
  DEBUG ((DEBUG_WARN, "ZumaUfs: device side as left: GPIO_OUT (reset, 1 = released) 0x%x; supply gpp0: CON 0x%08x DAT 0x%08x; pins gph5: CON 0x%08x DAT 0x%08x PUD 0x%08x; PMU phy iso 0x%08x; sysreg IOCC 0x%08x\n",
    MmioRead32 (mBase + UFS_VS_HCI + 0x70), MmioRead32 (0x10840000), MmioRead32 (0x10840004), MmioRead32 (0x13060000),
    MmioRead32 (0x13060004), MmioRead32 (0x13060008), MmioRead32 (0x15463EC0), MmioRead32 (0x13020710)));
  {
    //
    // (bring-up) the bootloader's last descriptors and one of its command descriptors: the format this controller took
    //
    UINT32  *L = (UINT32 *)(UINTN)(LShiftU64 (MmioRead32 (mBase + UFS_UTRLBAU), 32) | MmioRead32 (mBase + UFS_UTRLBA));
    UINTN   Slot, W;

    for (Slot = 0; Slot < 32; Slot++) {
      UINT32  *D = L + Slot * 8;

      if ((D[0] | D[4] | D[5]) == 0) {
        continue;
      }

      DEBUG ((DEBUG_WARN, "ZumaUfs: ABL slot %2u: %08x %08x %08x %08x %08x %08x %08x %08x\n", Slot, D[0], D[1], D[2], D[3],
        D[4], D[5], D[6], D[7]));
      if (Slot == 1) {
        UINT32  *U = (UINT32 *)(UINTN)(LShiftU64 (D[5], 32) | (D[4] & ~0x7Fu));

        for (W = 0; W < 0x140 / 4; W += 8) {
          DEBUG ((DEBUG_WARN, "ZumaUfs:   ucd+%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", W * 4, U[W], U[W + 1], U[W + 2],
            U[W + 3], U[W + 4], U[W + 5], U[W + 6], U[W + 7]));
        }
      }
    }

    for (W = 0; W < 0x120; W += 0x10) {
      DEBUG ((DEBUG_WARN, "ZumaUfs: vs_hci+%02x: %08x %08x %08x %08x\n", W, MmioRead32 (mBase + UFS_VS_HCI + W),
        MmioRead32 (mBase + UFS_VS_HCI + W + 4), MmioRead32 (mBase + UFS_VS_HCI + W + 8), MmioRead32 (mBase + UFS_VS_HCI + W + 12)));
    }
  }
  Hcs = MmioRead32 (mBase + UFS_HCS);
  if ((MmioRead32 (mBase + UFS_HCE) & 1) == 0 || (Hcs & (HCS_DP | HCS_UTRLRDY)) != (HCS_DP | HCS_UTRLRDY)) {
    DEBUG ((DEBUG_ERROR, "ZumaUfs: the controller is not as the bootloader should leave it (HCE 0x%x HCS 0x%x): left alone\n",
      MmioRead32 (mBase + UFS_HCE), Hcs));
    return EFI_NOT_READY;
  }

  for (Waited = 0; MmioRead32 (mBase + UFS_UTRLDBR) != 0; Waited += 10) {
    if (Waited > 100000) {
      DEBUG ((DEBUG_ERROR, "ZumaUfs: requests in flight (doorbell 0x%x): left alone\n", MmioRead32 (mBase + UFS_UTRLDBR)));
      return EFI_NOT_READY;
    }

    MicroSecondDelay (10);
  }

  //
  // (bring-up test) the bootloader's list stays where it is; the command descriptor and the bounce buffer go into
  // the bootloader's memory, which the controller's DMA is known to reach
  //
  if (mInPlace) {
    Old    = LShiftU64 (MmioRead32 (mBase + UFS_UTRLBAU), 32) | MmioRead32 (mBase + UFS_UTRLBA);
    mList  = (UTRD *)(UINTN)Old;
    mUcd   = (UINT8 *)(UINTN)ABL_UCD;
    mBounce = (UINT8 *)(UINTN)ABL_BOUNCE;
    DEBUG ((DEBUG_WARN, "ZumaUfs: (test) the bootloader's list kept at 0x%lx, descriptor at 0x%p, bounce at 0x%p\n", Old,
      mUcd, mBounce));
    return EFI_SUCCESS;
  }

  mList = AllocateAlignedPages (1, SIZE_4KB);
  mUcd  = AllocateAlignedPages (EFI_SIZE_TO_PAGES (UCD_SIZE) + 1, SIZE_4KB);    // slot 0's, then slot 1's (4 KiB)
  if ((mList == NULL) || (mUcd == NULL)) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (mList, SIZE_4KB);
  Flush (mList, SIZE_4KB);

  DEBUG ((DEBUG_WARN, "ZumaUfs: nexus types as left: transfer 0x%08x, task management 0x%08x\n",
    MmioRead32 (mBase + UFS_VS_HCI + VS_UTRL_NEXUS_TYPE), MmioRead32 (mBase + UFS_VS_HCI + VS_UTMRL_NEXUS_TYPE)));
  Old = LShiftU64 (MmioRead32 (mBase + UFS_UTRLBAU), 32) | MmioRead32 (mBase + UFS_UTRLBA);
  MmioWrite32 (mBase + UFS_UTRLRSR, 0);
  if (EFI_ERROR (DeviceUp ())) {
    return EFI_DEVICE_ERROR;
  }

  MmioWrite32 (mBase + UFS_UTRLBA, (UINT32)(UINTN)mList);
  MmioWrite32 (mBase + UFS_UTRLBAU, (UINT32)((UINT64)(UINTN)mList >> 32));
  MmioWrite32 (mBase + UFS_UTRLRSR, 1);
  DEBUG ((DEBUG_WARN, "ZumaUfs: transfer list moved from 0x%lx to 0x%p (%u slots, UFSHCI %x.%x)\n", Old, mList,
    (MmioRead32 (mBase + UFS_CAP) & 0x1F) + 1, (MmioRead32 (mBase + UFS_VER) >> 8) & 0xF, (MmioRead32 (mBase + UFS_VER) >> 4) & 0xF));
  return DeviceInit ();
}

EFI_STATUS
EFIAPI
ZumaUfsDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  Ufs;
  EFI_HANDLE                    Handle;
  EFI_STATUS                    Status;

  if (EFI_ERROR (LocateMemoryRegionByName ("UFS", &Ufs))) {
    DEBUG ((DEBUG_ERROR, "ZumaUfs: no UFS region in the memory map\n"));
    return EFI_NOT_FOUND;
  }

  mBase  = Ufs.Address;
  Status = TakeOver ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiExtScsiPassThruProtocolGuid,
                  &mPassThru,
                  NULL
                  );
  DEBUG ((DEBUG_WARN, "ZumaUfs: SCSI pass-through for LUN 0-%u (read only): %r\n", UFS_LUNS - 1, Status));
  return Status;
}
