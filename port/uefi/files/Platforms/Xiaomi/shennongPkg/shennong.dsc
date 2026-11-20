##
#
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = shennong
  PLATFORM_GUID                  = EA17CDFB-30F1-4CC0-8BB8-7BF51482F746
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/shennongPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = shennongPkg/shennong.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM8650-AA
  # 1 = SM8650-Q-AA
  # 2 = SM8650-AB
  # 3 = SM8650-Q-AB
  # 4 = SM8650-AC
  #
  SOC_TYPE                       = 2   # 14 Pro: SM8650-AB

!include LanaiPkg/LanaiPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA760D000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Xiaomi 14 Pro"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"shennong"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"23116PN5BC"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Xiaomi 14 Pro"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1440
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|3200
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|shennongPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|shennongPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  #
  # ACPI Tables
  #
  #shennong/AcpiTables.inf

[BuildOptions]
  #
  # HackiOS: where the boot shim puts the XNU loader's "XNUP" container (PAYLOAD_BASE in
  # common/devices/shennong.conf, XNU_Payload in MemoryMapLib.c); XnuLoader.h defaults to redwood's.
  #
  *_CLANGPDB_*_CC_FLAGS = -D XNU_PAYLOAD_BASE=0xC7800000ULL
