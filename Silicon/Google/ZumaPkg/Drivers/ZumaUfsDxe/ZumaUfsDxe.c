/**
  zuma's UFS host controller (ufs@13200000) for the firmware, as an EFI SCSI pass-through: EDK2's SCSI bus and disk
  drivers make a block device of each logical unit on top of it, its partition driver the GPT partitions.

  Google's bootloader leaves the host enabled but switches the device off before it hands over, so the firmware
  brings the whole thing up again, in the order Google's Linux driver does: the device's supply and pins, the host's
  reset and enable (with the vendor steps of ZumaUfsCalLib around it), Samsung's PHY calibration before and after
  the link startup, the device's initialisation (NOP OUT, fDeviceInit), then the power mode change to HS.

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
#include <Library/UefiLib.h>
#include <Library/ZumaUfsCalLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/ScsiPassThruExt.h>

#include "Ufshci.h"

#define UFS_LUNS       8                // the logical units offered (the phone has 0-3: user, boot A/B, a small one)
#define IO_ALIGN       64               // the cache line: buffers the controller writes into must not share one
#define DEFAULT_WAIT   (5 * 1000 * 1000)  // microseconds, when a request names no timeout
#define BOUNCE_SIZE    SIZE_64KB        // for small buffers that are not aligned (SCSI disk's on its stack)

STATIC UINTN       mBase;
STATIC UTRD        *mList;              // 32 descriptors, 1 KiB aligned; the driver uses slot 0
STATIC UINT8       *mUcd;               // slot 0's command descriptor, 128-byte aligned
STATIC UINT8       mTag;
STATIC UINT8       *mBounce;
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
// Writes: only into partitions AkIOS owns, by their GPT names, on the logical unit that holds them. The ranges come
// from EDK2's partition driver (OnPartition); until a partition is known, no write reaches it. Everything else on
// the phone -- Android, the bootloaders, persist, efs, modem, the other slot's images -- cannot be written from here.
//
STATIC CONST CHAR16  *mWritableNames[] = {
  L"devinfo",     // the A/B slot state: the booted slot is marked successful (AkitaSlotDxe)
  L"klog",        // the firmware's and the loader's log
};
STATIC CONST CHAR16  mIosPrefix[] = L"ios_";       // and every iOS volume (ios_system, ios_data, ...)

typedef struct {
  UINT8   Lun;
  UINT64  First;
  UINT64  Last;
} WRITABLE_RANGE;

STATIC WRITABLE_RANGE  mWritable[32];
STATIC UINTN           mWritableCount;

STATIC
BOOLEAN
IsWritableName (
  IN CONST CHAR16  *Name
  )
{
  UINTN  Index;

  if (StrnCmp (Name, mIosPrefix, StrLen (mIosPrefix)) == 0) {
    return TRUE;
  }

  for (Index = 0; Index < ARRAY_SIZE (mWritableNames); Index++) {
    if (StrCmp (Name, mWritableNames[Index]) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

//
// A command that writes the medium: WRITE(10)/(16) inside a writable range; SYNCHRONIZE CACHE writes nothing new.
//
STATIC
BOOLEAN
IsWriteCommand (
  IN CONST UINT8  *Cdb
  )
{
  return (Cdb[0] == EFI_SCSI_OP_WRITE10) || (Cdb[0] == EFI_SCSI_OP_WRITE16) || (Cdb[0] == EFI_SCSI_OP_SYNC_CACHE);
}

STATIC
BOOLEAN
WritePermitted (
  IN UINT8        Lun,
  IN CONST UINT8  *Cdb
  )
{
  UINT64  Lba;
  UINT64  Blocks;
  UINTN   Index;

  if (Cdb[0] == EFI_SCSI_OP_SYNC_CACHE) {
    return TRUE;
  }

  if (Cdb[0] == EFI_SCSI_OP_WRITE10) {
    Lba    = SwapBytes32 (ReadUnaligned32 ((CONST UINT32 *)&Cdb[2]));
    Blocks = SwapBytes16 (ReadUnaligned16 ((CONST UINT16 *)&Cdb[7]));
  } else {
    Lba    = SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)&Cdb[2]));
    Blocks = SwapBytes32 (ReadUnaligned32 ((CONST UINT32 *)&Cdb[10]));
  }

  if ((Blocks == 0) || (Lba + Blocks < Lba)) {
    return FALSE;
  }

  for (Index = 0; Index < mWritableCount; Index++) {
    if ((mWritable[Index].Lun == Lun) && (Lba >= mWritable[Index].First) && (Lba + Blocks - 1 <= mWritable[Index].Last)) {
      return TRUE;
    }
  }

  DEBUG ((DEBUG_ERROR, "ZumaUfs: write REFUSED: LUN %u LBA %lu, %lu blocks: not in a partition AkIOS may write\n", Lun, Lba,
    Blocks));
  return FALSE;
}

//
// What reaches the device: reads, questions, and writes WritePermitted lets through. Anything else is refused.
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
    case EFI_SCSI_OP_WRITE10:           // (with WritePermitted)
    case EFI_SCSI_OP_WRITE16:
    case EFI_SCSI_OP_SYNC_CACHE:
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
// One request through slot 0, waited for. A data buffer that is not IO_ALIGN-aligned, or whose length is not whole
// double words (the PRDT's unit), goes through the bounce buffer.
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
  UINT32         Mapped;
  UINT32         Left;
  UINT8          *Caller;
  UINTN          Entries;
  BOOLEAN        Read;
  UINT64         Waited;
  UINT64         Limit;
  UINT32         Is;
  UINTN          SenseBytes;

  if ((Packet->CdbLength == 0) || (Packet->CdbLength > 16) || (Packet->Cdb == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Permitted (Packet->Cdb) || (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_BIDIRECTIONAL)) {
    return EFI_UNSUPPORTED;
  }

  Read   = (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ);
  Length = Read ? Packet->InTransferLength : Packet->OutTransferLength;
  Data   = Read ? Packet->InDataBuffer : Packet->OutDataBuffer;
  if (IsWriteCommand (Packet->Cdb) ? (Read && (Length != 0)) || !WritePermitted (Lun, Packet->Cdb)
                                   : (!Read && (Length != 0)))
  {
    return EFI_WRITE_PROTECTED;            // data only goes out with a permitted write, and a write sends, never reads
  }

  if ((Length != 0) && ((Data == NULL) || (Length > PRDT_ENTRIES * PRDT_MAX_BYTES))) {
    return EFI_INVALID_PARAMETER;
  }

  Caller = NULL;
  if ((Length != 0) && ((((UINTN)Data & (IO_ALIGN - 1)) != 0) || ((Length & 3) != 0))) {
    if (Length > BOUNCE_SIZE) {
      return EFI_INVALID_PARAMETER;
    }

    Caller = Data;
    Data   = mBounce;
    if (!Read) {
      CopyMem (Data, Caller, Length);
      ZeroMem (Data + Length, ALIGN_VALUE (Length, 4) - Length);
    }
  }

  Mapped = ALIGN_VALUE (Length, 4);

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
  Command->Flags          = (Length == 0) ? 0 : Read ? UPIU_FLAG_READ : UPIU_FLAG_WRITE;
  Command->Lun            = Lun;
  Command->TaskTag        = ++mTag;
  Command->ExpectedLength = SwapBytes32 (Length);
  CopyMem (Command->Cdb, Packet->Cdb, Packet->CdbLength);

  Entries = 0;
  for (Left = Mapped; Left > 0; Entries++) {
    UINT32  Chunk = MIN (Left, (UINT32)PRDT_MAX_BYTES);
    UINT64  At    = (UINT64)(UINTN)Data + (Mapped - Left);

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
  Utrd->Dw0         = UTRD_CT_UFS | UTRD_INTERRUPT | ((Length == 0) ? UTRD_DD_NONE : Read ? UTRD_DD_FROM_DEVICE : UTRD_DD_TO_DEVICE);
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
    if (Read) {
      WriteBackInvalidateDataCacheRange (Data, Mapped);   // no dirty line may land on what the device writes
    } else {
      WriteBackDataCacheRange (Data, Mapped);             // what the device reads is in memory
    }
  }

  MmioWrite32 (mBase + UFS_IS, IS_UTRCS);
  MmioOr32 (mBase + UFS_VS_HCI + VS_UTRL_NEXUS_TYPE, 1);    // slot 0 carries a SCSI command
  MmioWrite32 (mBase + UFS_UTRLDBR, 1);

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
    Discard (Data, Mapped);
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
    //
    // What the device sent: nothing when the command failed, less than asked when it says so (underflow, bit 5:
    // only then is the residual count valid). Only that much reaches the caller; the rest of its buffer is zeroed,
    // never left with what an earlier transfer put in the bounce buffer.
    //
    UINT32  Done = (Response->Status != EFI_EXT_SCSI_STATUS_TARGET_GOOD) ? 0 :
                   (Response->Flags & 0x20) ? Length - MIN (Length, SwapBytes32 (Response->ResidualCount)) : Length;

    if (Length != 0) {
      UINT8  *To = (Caller != NULL) ? Caller : Data;

      if (Caller != NULL) {
        CopyMem (Caller, Data, Done);
      }

      ZeroMem (To + Done, Length - Done);
    }

    Packet->InTransferLength = Done;
  }

  //
  // (bring-up) what each logical unit says about itself
  //
  if (Response->Status == EFI_EXT_SCSI_STATUS_TARGET_GOOD) {
    if ((Command->Cdb[0] == EFI_SCSI_OP_INQUIRY) && ((Command->Cdb[1] & 1) == 0) && (Packet->InTransferLength >= 36)) {
      DEBUG ((DEBUG_WARN, "ZumaUfs: LUN %u INQUIRY: type 0x%02x \"%.8a\" \"%.16a\" rev \"%.4a\"\n", Lun, Data[0] & 0x1F,
        Data + 8, Data + 16, Data + 32));
    } else if ((Command->Cdb[0] == EFI_SCSI_OP_READ_CAPACITY) && (Packet->InTransferLength >= 8)) {
      DEBUG ((DEBUG_WARN, "ZumaUfs: LUN %u READ CAPACITY(10): last LBA 0x%x, block %u\n", Lun, SwapBytes32 (*(UINT32 *)Data),
        SwapBytes32 (*(UINT32 *)(Data + 4))));
    } else if ((Command->Cdb[0] == EFI_SCSI_OP_READ_CAPACITY16) && (Packet->InTransferLength >= 12)) {
      DEBUG ((DEBUG_WARN, "ZumaUfs: LUN %u READ CAPACITY(16): last LBA 0x%lx, block %u\n", Lun, SwapBytes64 (ReadUnaligned64 ((UINT64 *)Data)),
        SwapBytes32 (*(UINT32 *)(Data + 8))));
    }
  } else if ((SenseBytes < 3) || (((Response->Sense[2] & 0xF) != EFI_SCSI_SK_ILLEGAL_REQUEST) &&
                                   ((Response->Sense[2] & 0xF) != EFI_SCSI_SK_UNIT_ATTENTION)))
  {
    //
    // (the expected ones left out: a logical unit that does not exist, MODE SENSE(6) that UFS does not have, the
    // unit attention after the reset)
    //
    DEBUG ((DEBUG_WARN, "ZumaUfs: LUN %u op 0x%02x: status 0x%02x, sense key 0x%x\n", Lun, Command->Cdb[0], Response->Status,
      (SenseBytes >= 3) ? (Response->Sense[2] & 0xF) : 0));
  }

  return EFI_SUCCESS;
}

//
// MODE SENSE(6), which UFS does not have (only MODE SENSE(10)): asked as MODE SENSE(10), the 8-byte header turned
// into the 6-byte one. EDK2's disk driver takes the write protection from it (the device-specific parameter, bit 7).
//
STATIC
EFI_STATUS
ModeSense6 (
  IN     UINT8                                       Lun,
  IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet
  )
{
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  Ten;
  UINT8                                       Cdb[10];
  UINT8                                       *Six = Packet->InDataBuffer;
  UINT8                                       *Data;
  UINT32                                      Asked = Packet->InTransferLength;
  UINT32                                      Got;
  UINT32                                      Length10;
  EFI_STATUS                                  Status;

  Data = AllocateZeroPool (256 + 8);
  if ((Data == NULL) || (Six == NULL) || (Asked < 4)) {
    if (Data != NULL) {
      FreePool (Data);
    }

    return EFI_INVALID_PARAMETER;
  }

  CopyMem (&Ten, Packet, sizeof (Ten));
  ZeroMem (Cdb, sizeof (Cdb));
  Cdb[0] = EFI_SCSI_OP_MODE_SEN10;
  Cdb[1] = ((UINT8 *)Packet->Cdb)[1] & 0x08;      // DBD
  Cdb[2] = ((UINT8 *)Packet->Cdb)[2];             // page control and code
  Cdb[3] = ((UINT8 *)Packet->Cdb)[3];             // subpage
  Cdb[7] = (UINT8)((MIN (Asked, 252u) + 4) >> 8);
  Cdb[8] = (UINT8)(MIN (Asked, 252u) + 4);
  Ten.Cdb              = Cdb;
  Ten.CdbLength        = sizeof (Cdb);
  Ten.InDataBuffer     = Data;
  Ten.InTransferLength = MIN (Asked, 252u) + 4;
  Status = Transfer (Lun, &Ten);
  Packet->HostAdapterStatus = Ten.HostAdapterStatus;
  Packet->TargetStatus      = Ten.TargetStatus;
  Packet->SenseDataLength   = Ten.SenseDataLength;
  ZeroMem (Six, Asked);
  if (EFI_ERROR (Status) || (Ten.TargetStatus != EFI_EXT_SCSI_STATUS_TARGET_GOOD) || (Ten.InTransferLength < 8)) {
    Packet->InTransferLength = 0;
    FreePool (Data);
    return Status;
  }

  Length10 = ((UINT32)Data[0] << 8) | Data[1];          // the bytes after the length field
  Got      = MIN (Ten.InTransferLength, Length10 + 2);
  Six[0]   = (UINT8)MIN (Got - 8 + 3, 255u);            // mode data length: the bytes after this one
  Six[1]   = Data[2];                                    // medium type
  Six[2]   = Data[3];                                    // device-specific parameter (bit 7: write protected)
  Six[3]   = Data[7];                                    // block descriptor length (DBD: none)
  CopyMem (Six + 4, Data + 8, MIN (Got - 8, Asked - 4));
  Packet->InTransferLength = 4 + MIN (Got - 8, Asked - 4);
  DEBUG ((DEBUG_WARN, "ZumaUfs: LUN %u MODE SENSE: device-specific parameter 0x%02x (%a)\n", Lun, Data[3],
    (Data[3] & 0x80) ? "write protected" : "writable"));
  FreePool (Data);
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

  if ((((UINT8 *)Packet->Cdb)[0] == EFI_SCSI_OP_MODE_SEN6) && (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ)) {
    return ModeSense6 ((UINT8)Lun, Packet);
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
  return EFI_UNSUPPORTED;
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
// ---- bring-up ------------------------------------------------------------------------------------------------------
//
// The phone's tree: ufs@13200000's register ranges (standard HCI, vendor HCI, UniPro, UFS protector, PHY, CPort), the
// unipro clock's rate (178 MHz, as Android reports it and as the bootloader's 1 us tick, 0xB2, says), the CAL's board
// (brd-for-cal) and the power mode it is meant to reach (ufs,pmd-attr-lane / -gear, HS series B). It has no evt-ver:
// Linux takes 0 then (and the CAL's evt0 and evt1 tables are the same for zuma).
//
#define UFS_PMA_OFFSET      0x4000          // 0x13204000, in the "UFS" region
#define UFS_CPORT_OFFSET    0x8000          // 0x13208000, in the "UFS" region
#define UFS_UFSP_OFFSET     0x20000         // 0x132a0000, in the "UFS UniPro" region
#define UFS_MCLK_RATE       178000000
#define UFS_CAL_BOARD       1
#define UFS_CAL_EVT_VER     0
#define UFS_PMD_LANES       2
#define UFS_PMD_GEAR        4

//
// The device's supply (gpp0-1 of pinctrl@10840000, the ufs-vcc fixed regulator) and its pins (gph5-0, the reset, and
// gph5-1, the reference clock, of pinctrl@13060000): the bootloader leaves the supply off and the reset a GPIO low
//
#define GPIO_ALIVE_GPP0     0x10840000ULL   // CON, DAT at +4
#define GPIO_HSI2_GPH5      0x13060000ULL
#define UFS_VCC_PIN         1
#define GPH5_UFS_FUNCTIONS  0x22            // gph5-0 and gph5-1 to their UFS function (2)

STATIC UINTN  mUnipro;

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
  UINTN   Waited;
  UINT32  Code;

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
  Code = MmioRead32 (mBase + UFS_UICARG2) & 0xFF;
  if (Result != NULL) {
    *Result = Code;
  }

  if (Value != NULL) {
    *Value = MmioRead32 (mBase + UFS_UICARG3);
  }

  return (Code == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
DmeSet (
  IN UINT16  Attribute,
  IN UINT32  Value
  )
{
  EFI_STATUS  Status;
  UINT32      Result;

  Status = UicCommand (UIC_DME_SET, UIC_ARG_MIB (Attribute), 0, Value, &Result, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ZumaUfs: DME_SET 0x%04x = %u: %r (result %u)\n", Attribute, Value, Status, Result));
  }

  return Status;
}

STATIC
UINT32
DmeGet (
  IN UINT32   Command,            // UIC_DME_GET or UIC_DME_PEER_GET
  IN UINT16   Attribute
  )
{
  UINT32  Value = 0;

  if (EFI_ERROR (UicCommand (Command, UIC_ARG_MIB (Attribute), 0, 0, NULL, &Value))) {
    return 0;
  }

  return Value;
}

//
// The controller's enable (ufshcd_hba_execute_hce): HCE to 0, the vendor steps, HCE to 1 and wait, the vendor steps
//
STATIC
EFI_STATUS
HostEnable (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Waited;

  if (MmioRead32 (mBase + UFS_HCE) & 1) {
    MmioWrite32 (mBase + UFS_HCE, 0);
    for (Waited = 0; MmioRead32 (mBase + UFS_HCE) & 1; Waited += 10) {
      if (Waited > 10000) {
        DEBUG ((DEBUG_ERROR, "ZumaUfs: HCE does not clear\n"));
        return EFI_TIMEOUT;
      }

      MicroSecondDelay (10);
    }
  }

  Status = ZumaUfsHostPreHce ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (mBase + UFS_HCE, 1);
  MicroSecondDelay (1000);
  for (Waited = 0; (MmioRead32 (mBase + UFS_HCE) & 1) == 0; Waited++) {
    if (Waited > 50) {
      DEBUG ((DEBUG_ERROR, "ZumaUfs: the controller does not enable\n"));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1000);
  }

  return ZumaUfsHostPostHce ();
}

//
// The link startup (ufshcd_link_startup): a failed attempt re-enables the controller; a device that was not active
// gets the whole startup twice
//
STATIC
EFI_STATUS
LinkStartup (
  OUT UINT8  *Lanes,
  OUT UINT8  *MaxGear
  )
{
  EFI_STATUS  Status;
  UINT32      Result;
  UINTN       Round;
  UINTN       Try;

  for (Round = 0; Round < 2; Round++) {
    for (Try = 0; Try < 4; Try++) {
      if (Try > 0) {
        Status = HostEnable ();
        if (EFI_ERROR (Status)) {
          return Status;
        }
      }

      Status = ZumaUfsHostPreLink ();
      if (EFI_ERROR (Status)) {
        return Status;
      }

      Result = 0xFF;
      Status = UicCommand (UIC_DME_LINKSTARTUP, 0, 0, 0, &Result, NULL);
      DEBUG ((DEBUG_WARN, "ZumaUfs: link startup (round %u, try %u): %r (result %u), HCS 0x%08x\n", Round + 1, Try + 1,
        Status, Result, MmioRead32 (mBase + UFS_HCS)));
      if (!EFI_ERROR (Status)) {
        if ((MmioRead32 (mBase + UFS_HCS) & HCS_DP) == 0) {
          DEBUG ((DEBUG_ERROR, "ZumaUfs: link up but no device present\n"));
          return EFI_NOT_FOUND;
        }

        break;
      }
    }

    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = ZumaUfsHostPostLink (Lanes, MaxGear);
  MmioRead32 (mBase + UFS_UECPA);         // the line reset of the link startup left an error code here
  return Status;
}

//
// The power mode change (ufshcd_change_power_mode, with the vendor steps around it): FAST mode, HS series B
//
STATIC
EFI_STATUS
PowerModeChange (
  IN UINT8  Gear,
  IN UINT8  Lanes
  )
{
  EFI_STATUS  Status;
  UINTN       Waited;
  UINT32      Hcs;
  UINT32      Mode = ZUMA_UFS_FAST_MODE;

  Status = ZumaUfsHostPrePmc ((UINT8)Mode, Gear, Lanes, ZUMA_UFS_HS_SERIES_B);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (EFI_ERROR (DmeSet (PA_RXGEAR, Gear)) || EFI_ERROR (DmeSet (PA_ACTIVERXDATALANES, Lanes)) ||
      EFI_ERROR (DmeSet (PA_RXTERMINATION, 1)) || EFI_ERROR (DmeSet (PA_TXGEAR, Gear)) ||
      EFI_ERROR (DmeSet (PA_ACTIVETXDATALANES, Lanes)) || EFI_ERROR (DmeSet (PA_TXTERMINATION, 1)) ||
      EFI_ERROR (DmeSet (PA_HSSERIES, ZUMA_UFS_HS_SERIES_B)))
  {
    return EFI_DEVICE_ERROR;
  }

  MmioWrite32 (mBase + UFS_IS, IS_UPMS);
  Status = DmeSet (PA_PWRMODE, (Mode << 4) | Mode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Waited = 0; (MmioRead32 (mBase + UFS_IS) & IS_UPMS) == 0; Waited += 10) {
    if (Waited > 500000) {
      DEBUG ((DEBUG_ERROR, "ZumaUfs: power mode change: no completion\n"));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (10);
  }

  MmioWrite32 (mBase + UFS_IS, IS_UPMS);
  Hcs = MmioRead32 (mBase + UFS_HCS);
  if (HCS_UPMCRS (Hcs) != UPMCRS_PWR_LOCAL) {
    DEBUG ((DEBUG_ERROR, "ZumaUfs: power mode change: UPMCRS %u\n", HCS_UPMCRS (Hcs)));
    return EFI_DEVICE_ERROR;
  }

  return ZumaUfsHostPostPmc ();
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
  ZeroMem (Response, sizeof (Response));
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
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ZumaUfs: set fDeviceInit -> %r, response 0x%02x\n", Status, Response[6]));
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
// Everything from the device's power to a link in PWM gear 1 (the link startup's mode) with the device initialised
// and the transfer list running
//
STATIC
EFI_STATUS
BringUp (
  OUT UINT8  *Lanes,
  OUT UINT8  *MaxGear
  )
{
  EFI_STATUS  Status;

  //
  // the device: held in reset, its supply on, its pins to the controller
  //
  MmioWrite32 (mBase + UFS_VS_HCI + VS_GPIO_OUT, 0);
  MmioOr32 (GPIO_ALIVE_GPP0 + 4, 1u << UFS_VCC_PIN);
  MicroSecondDelay (20000);
  MmioWrite32 (GPIO_HSI2_GPH5, (MmioRead32 (GPIO_HSI2_GPH5) & ~0xFFu) | GPH5_UFS_FUNCTIONS);

  Status = HostEnable ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = LinkStartup (Lanes, MaxGear);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((MmioRead32 (mBase + UFS_HCS) & HCS_READY) != HCS_READY) {
    DEBUG ((DEBUG_ERROR, "ZumaUfs: the controller is not ready for lists: HCS 0x%08x\n", MmioRead32 (mBase + UFS_HCS)));
    return EFI_NOT_READY;
  }

  MmioWrite32 (mBase + UFS_IS, MmioRead32 (mBase + UFS_IS));
  MmioWrite32 (mBase + UFS_UTRLBA, (UINT32)(UINTN)mList);
  MmioWrite32 (mBase + UFS_UTRLBAU, (UINT32)((UINT64)(UINTN)mList >> 32));
  MmioWrite32 (mBase + UFS_UTRLRSR, 1);

  return DeviceInit ();
}

STATIC
EFI_STATUS
Start (
  VOID
  )
{
  ZUMA_UFS_HOST_CONFIG  Config;
  EFI_STATUS            Status;
  UINT8                 Lanes;
  UINT8                 MaxGear;
  UINT8                 PeerGear;
  UINT8                 Gear;

  mList = AllocateAlignedPages (1, SIZE_4KB);
  mUcd  = AllocateAlignedPages (EFI_SIZE_TO_PAGES (UCD_SIZE) + 1, SIZE_4KB);    // slot 0's, then slot 1's (4 KiB)
  mBounce = AllocateAlignedPages (EFI_SIZE_TO_PAGES (BOUNCE_SIZE), SIZE_4KB);
  if ((mList == NULL) || (mUcd == NULL) || (mBounce == NULL)) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (mList, SIZE_4KB);
  Flush (mList, SIZE_4KB);

  Config.Std      = mBase;
  Config.Hci      = mBase + UFS_VS_HCI;
  Config.Unipro   = mUnipro;
  Config.Ufsp     = mUnipro + UFS_UFSP_OFFSET;
  Config.Pma      = mBase + UFS_PMA_OFFSET;
  Config.Cport    = mBase + UFS_CPORT_OFFSET;
  Config.MclkRate = UFS_MCLK_RATE;
  Config.Board    = UFS_CAL_BOARD;
  Config.EvtVer   = UFS_CAL_EVT_VER;
  Config.Gear     = UFS_PMD_GEAR;
  Status          = ZumaUfsHostInit (&Config);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_WARN, "ZumaUfs: CAL: mclk %u Hz, board %u, evt %u (the tree has no evt-ver)\n", Config.MclkRate,
    Config.Board, Config.EvtVer));

  Status = BringUp (&Lanes, &MaxGear);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // HS: the gear both sides can do, at most the tree's; on failure the link is brought up again and left in PWM
  //
  PeerGear = (UINT8)DmeGet (UIC_DME_PEER_GET, PA_MAXRXHSGEAR);
  Gear     = MIN (MaxGear, PeerGear);
  Lanes    = MIN (Lanes, (UINT8)UFS_PMD_LANES);
  if ((Gear == 0) || (Lanes == 0)) {
    DEBUG ((DEBUG_WARN, "ZumaUfs: no HS gear (host %u, device %u): staying in PWM gear 1\n", MaxGear, PeerGear));
    return EFI_SUCCESS;
  }

  Status = PowerModeChange (Gear, Lanes);
  DEBUG ((DEBUG_WARN, "ZumaUfs: power mode change to HS gear %u, %u lanes, series B (device max gear %u): %r\n", Gear,
    Lanes, PeerGear, Status));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "ZumaUfs: bringing the link up again, staying in PWM gear 1\n"));
    return BringUp (&Lanes, &MaxGear);
  }

  return EFI_SUCCESS;
}

//
// Each partition EDK2's partition driver finds: the writable ones are noted (WritePermitted), all are logged
//
STATIC EFI_EVENT  mPartitionEvent;
STATIC VOID       *mPartitionRegistration;

STATIC
VOID
EFIAPI
OnPartition (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_HANDLE                   Handle;
  UINTN                        Size;
  EFI_PARTITION_INFO_PROTOCOL  *Info;
  EFI_BLOCK_IO_PROTOCOL        *BlockIo;

  for ( ; ;) {
    Size = sizeof (Handle);
    if (EFI_ERROR (gBS->LocateHandle (ByRegisterNotify, NULL, mPartitionRegistration, &Size, &Handle))) {
      return;
    }

    if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiPartitionInfoProtocolGuid, (VOID **)&Info))) {
      continue;
    }

    BlockIo = NULL;
    gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if ((Info->Type == PARTITION_TYPE_GPT) && IsWritableName (Info->Info.Gpt.PartitionName) &&
        (mWritableCount < ARRAY_SIZE (mWritable)))
    {
      EFI_DEVICE_PATH_PROTOCOL  *Path;

      if (!EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid, (VOID **)&Path))) {
        for ( ; !IsDevicePathEnd (Path); Path = NextDevicePathNode (Path)) {
          if ((DevicePathType (Path) == MESSAGING_DEVICE_PATH) && (DevicePathSubType (Path) == MSG_SCSI_DP)) {
            mWritable[mWritableCount].Lun   = (UINT8)((SCSI_DEVICE_PATH *)Path)->Lun;
            mWritable[mWritableCount].First = Info->Info.Gpt.StartingLBA;
            mWritable[mWritableCount].Last  = Info->Info.Gpt.EndingLBA;
            DEBUG ((DEBUG_WARN, "ZumaUfs: writable: \"%s\" (LUN %u, LBA %lu-%lu)\n", Info->Info.Gpt.PartitionName,
              mWritable[mWritableCount].Lun, mWritable[mWritableCount].First, mWritable[mWritableCount].Last));
            mWritableCount++;
            break;
          }
        }
      }
    }

    if (Info->Type == PARTITION_TYPE_GPT) {
      DEBUG ((DEBUG_WARN, "ZumaUfs: partition \"%s\": LBA %lu-%lu (%lu blocks of %u)\n", Info->Info.Gpt.PartitionName,
        Info->Info.Gpt.StartingLBA, Info->Info.Gpt.EndingLBA, (BlockIo != NULL) ? BlockIo->Media->LastBlock + 1 : 0,
        (BlockIo != NULL) ? BlockIo->Media->BlockSize : 0));
    } else {
      DEBUG ((DEBUG_WARN, "ZumaUfs: partition of type %u\n", Info->Type));
    }
  }
}

EFI_STATUS
EFIAPI
ZumaUfsDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  Ufs;
  EFI_MEMORY_REGION_DESCRIPTOR  Unipro;
  EFI_HANDLE                    Handle;
  EFI_STATUS                    Status;

  if (EFI_ERROR (LocateMemoryRegionByName ("UFS", &Ufs)) || EFI_ERROR (LocateMemoryRegionByName ("UFS UniPro", &Unipro))) {
    DEBUG ((DEBUG_ERROR, "ZumaUfs: no UFS regions in the memory map\n"));
    return EFI_NOT_FOUND;
  }

  mBase   = Ufs.Address;
  mUnipro = Unipro.Address;
  Status  = Start ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ZumaUfs: bring-up failed: %r\n", Status));
    return Status;
  }

  mPartitionEvent = EfiCreateProtocolNotifyEvent (&gEfiPartitionInfoProtocolGuid, TPL_CALLBACK, OnPartition, NULL,
                      &mPartitionRegistration);

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiExtScsiPassThruProtocolGuid,
                  &mPassThru,
                  NULL
                  );
  DEBUG ((DEBUG_WARN, "ZumaUfs: SCSI pass-through for LUN 0-%u (writes only to AkIOS's partitions): %r\n", UFS_LUNS - 1, Status));
  return Status;
}
