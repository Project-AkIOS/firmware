/**
  The Pixel 8a's (akita) memory map for the firmware: where it lives, what it may use, and the device registers it
  touches. RAM the bootloader, the secure world and the remote processors keep (the phone's /reserved-memory) is left
  out, so the firmware never allocates it: only the free gap 0xC3090000-0xD8100000 of the first bank and the three
  upper banks (0x880000000-0x9FFFFFFFF, 6 GiB) are given to it.

  Copyright (c) 2026, AkIOS contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR: the firmware and its memory (the free gap of the first bank)
  {"UEFI FD",            0xC4000000,  0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI Stack",         0xC4400000,  0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"DXE Heap",           0xC4440000,  0x13BC0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},

  // DDR: the framebuffer the bootloader left the panel reading (1080 x 2400 x 4), and the ramoops region (console,
  // then pmsg: the firmware's log goes into the pmsg zone, which Android shows as /sys/fs/pstore/pmsg-ramoops-0)
  {"Display Reserved",   0xFAC00000,  0x00A00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  // DDR: the bootloader's own memory (abl@f8800000): unused once it has started the firmware, kept out of its memory
  {"ABL",                0xF8800000,  0x01000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED},
  {"Ramoops",            0xFD3FF000,  0x00400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED},

  // DDR: the upper banks, free
  {"RAM Bank 1",         0x880000000, 0x80000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"RAM Bank 2",         0x900000000, 0x80000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"RAM Bank 3",         0x980000000, 0x80000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},

  // Registers
  {"Watchdog CL0",       0x10060000,  0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Watchdog CL1",       0x10070000,  0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICD",               0x10400000,  0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICR",               0x10440000,  0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GPIO ALIVE",         0x10840000,  0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SYSREG UFS",         0x13020000,  0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GPIO HSI2",          0x13060000,  0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"S2MPU HSI2",         0x131F0000,  0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  // ufs@13200000: standard and vendor HCI, the PHY (+0x4000) and CPort (+0x8000); UniPro and the UFS protector (+0x20000)
  {"UFS",                0x13200000,  0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS UniPro",         0x13280000,  0x00030000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PMU",                0x15460000,  0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"DECON0",             0x19470000,  0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
