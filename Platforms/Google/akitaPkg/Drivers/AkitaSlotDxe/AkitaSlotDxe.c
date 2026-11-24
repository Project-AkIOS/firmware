/**
  The Pixel 8a's A/B slot state (the devinfo partition, LUN 3).

  devinfo starts with "DEVI"; at offset 0x30 it holds one 32-bit word for each slot (a, then b): bits 7:0 the tries
  left, then one bit each for unbootable (8), successful (9), active (10) and fastboot-ok (11). Google's bootloader
  starts the active slot and counts its tries down while it is not marked successful; Android marks it once it is
  up. AkIOS is what runs from that slot now, so the firmware marks it: only the successful bit of the active slot,
  only when it is not set yet, nothing else of the block (it also holds the phone's serial numbers and addresses:
  never logged, never copied).

  Copyright (c) 2026, AkIOS contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>

#define DEVINFO_SLOTS       0x30
#define SLOT_TRIES          0x000000FFu
#define SLOT_UNBOOTABLE     (1u << 8)
#define SLOT_SUCCESSFUL     (1u << 9)
#define SLOT_ACTIVE         (1u << 10)

STATIC VOID     *mRegistration;
STATIC BOOLEAN  mSlotDone;

STATIC
VOID
MarkSlot (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo
  )
{
  UINT8       *Block;
  UINT8       *Check;
  UINTN       Size = BlockIo->Media->BlockSize;
  UINT32      Slot[2];
  UINTN       Active;
  EFI_STATUS  Status;

  Block = AllocateAlignedPages (EFI_SIZE_TO_PAGES (Size), 64);
  Check = AllocateAlignedPages (EFI_SIZE_TO_PAGES (Size), 64);
  if ((Block == NULL) || (Check == NULL) || (Size < 0x40)) {
    return;
  }

  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, Size, Block);
  if (EFI_ERROR (Status) || (CompareMem (Block, "DEVI", 4) != 0)) {
    DEBUG ((DEBUG_ERROR, "AkitaSlot: devinfo unreadable or not \"DEVI\" (%r): left alone\n", Status));
    return;
  }

  CopyMem (Slot, Block + DEVINFO_SLOTS, sizeof (Slot));
  DEBUG ((DEBUG_WARN, "AkitaSlot: slot a 0x%08x, slot b 0x%08x\n", Slot[0], Slot[1]));
  if (((Slot[0] & SLOT_ACTIVE) != 0) == ((Slot[1] & SLOT_ACTIVE) != 0)) {
    DEBUG ((DEBUG_ERROR, "AkitaSlot: not exactly one slot active: left alone\n"));
    return;
  }

  Active = (Slot[0] & SLOT_ACTIVE) ? 0 : 1;
  if (Slot[Active] & SLOT_SUCCESSFUL) {
    DEBUG ((DEBUG_WARN, "AkitaSlot: slot %c is active and marked successful already: nothing written\n", 'a' + (CHAR8)Active));
    return;
  }

  Slot[Active] |= SLOT_SUCCESSFUL;
  CopyMem (Block + DEVINFO_SLOTS + Active * 4, &Slot[Active], 4);
  Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, 0, Size, Block);
  BlockIo->FlushBlocks (BlockIo);
  if (!EFI_ERROR (Status)) {
    Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, Size, Check);
  }

  DEBUG ((DEBUG_WARN, "AkitaSlot: slot %c marked successful (0x%08x): %r, %a\n", 'a' + (CHAR8)Active, Slot[Active], Status,
    (!EFI_ERROR (Status) && (CompareMem (Block, Check, Size) == 0)) ? "read back the same" : "READ BACK DIFFERS"));
}

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
    if (EFI_ERROR (gBS->LocateHandle (ByRegisterNotify, NULL, mRegistration, &Size, &Handle))) {
      return;
    }

    if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiPartitionInfoProtocolGuid, (VOID **)&Info)) ||
        (Info->Type != PARTITION_TYPE_GPT) ||
        EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo)))
    {
      continue;
    }

    if (!mSlotDone && (StrCmp (Info->Info.Gpt.PartitionName, L"devinfo") == 0)) {
      mSlotDone = TRUE;
      MarkSlot (BlockIo);
    }
  }
}

EFI_STATUS
EFIAPI
AkitaSlotDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EfiCreateProtocolNotifyEvent (&gEfiPartitionInfoProtocolGuid, TPL_CALLBACK, OnPartition, NULL, &mRegistration);
  return EFI_SUCCESS;
}
