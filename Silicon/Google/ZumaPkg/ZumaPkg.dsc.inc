##
#  Google Tensor G3 (zuma): what every zuma phone's firmware shares.
#
#  Copyright (c) 2026, AkIOS contributors.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  USE_PHYSICAL_TIMER                         = 1
  # (the erratum patches Windows' loader for a 25.9 MHz clock: not ours. The firmware's own timer takes
  #  PcdTimerFreqOverwrite below: the bootloader leaves CNTFRQ_EL0 unset, and only EL3 can set it.)
  HAS_CNTFRQ_EL0_EMPTY_ERRATA                = 0

!include SamsungPkg/SamsungPkg.dsc.inc

[BuildOptions]
  *_*_AARCH64_CC_FLAGS = -march=armv9-a+crypto

[PcdsFixedAtBuild]
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x20414d555a474f47
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x0003

  # 9 cores: 4 Cortex-A510, 4 Cortex-A715, 1 Cortex-X3
  gArmPlatformTokenSpaceGuid.PcdCoreCount|9
  gArmPlatformTokenSpaceGuid.PcdClusterCount|3

  # GIC v3 (interrupt-controller@10400000)
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x10400000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x10440000

  # the arch timer's rate (the phone's tree: clock-frequency 0x1770000)
  gSiliciumPkgTokenSpaceGuid.PcdTimerFreqOverwrite|24576000

  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Google Tensor G3"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"zuma"

[LibraryClasses]
  PlatformSecLib|ZumaPkg/Library/PlatformSecLib/PlatformSecLib.inf
  # serial output: the ramoops pmsg zone (the log Android shows after a restart), then the screen
  SerialPortLib|ZumaPkg/Library/PstoreSerialPortLib/PstoreSerialPortLib.inf
