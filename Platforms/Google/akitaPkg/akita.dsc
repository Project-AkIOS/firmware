##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#  Copyright (c) 2026, AkIOS contributors.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = akita
  PLATFORM_GUID                  = 5E1D0B7A-3C24-4F69-8A1E-7D2C9B04F318
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/akitaPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = akitaPkg/akita.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0


!include ZumaPkg/ZumaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xC4400000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Google"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Pixel 8a"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"akita"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Pixel_8a_akita"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Pixel 8a"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2400
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|akitaPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # Storage Libraries (EDK2's SCSI bus and disk drivers, over the UFS pass-through)
  #
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

[Components]
  #
  # ACPI Tables
  #
  #akita/AcpiTables.inf

  #
  # Storage
  #
  ZumaPkg/Drivers/ZumaUfsDxe/ZumaUfsDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf

