  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  INF Binaries/shennong/QcomPkg/Drivers/EnvDxe/EnvDxeEnhanced.inf

  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf

  INF Binaries/shennong/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
  INF Binaries/shennong/QcomPkg/Drivers/TzDxe/ScmDxeCompat.inf
  INF Binaries/shennong/QcomPkg/Drivers/TzDxe/TzDxeLA.inf

  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  INF Binaries/shennong/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/TLMMDxe/TLMMDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/PmicDxe/PmicDxeLa.inf

  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf

  INF Binaries/shennong/MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF Binaries/shennong/MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf

  INF FatPkg/EnhancedFatDxe/Fat.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  INF Binaries/shennong/QcomPkg/Drivers/UFSDxe/UFSDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/VcsDxe/VcsDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/SPMIDxe/SPMIDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/ICBDxe/ICBDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/SmemDxe/SmemDxe.inf

  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

  INF Binaries/shennong/QcomPkg/Drivers/RscDxe/RscDxe.inf

  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/shennong/QcomPkg/Drivers/CPRDxe/CPRDxe.inf
!endif
  INF Binaries/shennong/QcomPkg/Drivers/GpiDxe/GpiDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/SPIDxe/SPIDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/I2CDxe/I2CDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/SdccDxe/SdccDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/FeatureEnablerDxe/FeatureEnablerDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/shennong/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf
!else
  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif

  INF Binaries/shennong/QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/ChargerExDxe/ChargerExDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/IPCCDxe/IPCCDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/Smp2pDxe/Smp2pDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/SSRDxe/SSRDxe.inf
  # Crashes
  # INF Binaries/shennong/QcomPkg/Drivers/GLinkDxe/GLinkDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/PmicGlinkDxe/PmicGlinkDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.inf
  INF Binaries/shennong/QcomPkg/Drivers/UsbPwrCtrlDxe/UsbPwrCtrlDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/TsensDxe/TsensDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/LimitsDxe/LimitsDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/XhciDxe/XhciDxe.inf

  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

  INF Binaries/shennong/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/UsbInitDxe/UsbInitDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/UCDxe/UCDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/RNGDxe/RngDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/PILDxe/PILDxe.inf
  INF Binaries/shennong/QcomPkg/Drivers/PILProxyDxe/PILProxyDxe.inf


  # shennong OEM / extra modules not in giulia's list -- not needed to boot, left out:
  #INF Binaries/shennong/MdeModulePkg/Core/Dxe/DxeMain.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/RscRtDxe.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/SCHandlerRtDxe.inf
  #INF Binaries/shennong/ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  #INF Binaries/shennong/MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/VerifiedBootDxe/VerifiedBootDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/PsStateDxe/PsStateDxe.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/VariableDxe/VariableDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  #INF Binaries/shennong/QcomPkg/Drivers/ResetRuntimeDxe/ResetRuntimeDxe.inf
  #INF Binaries/shennong/EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  #INF Binaries/shennong/EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/FontDxe/FontDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/QcomWDogDxe/QcomWDogDxe.inf
  #INF Binaries/shennong/ArmPkg/Drivers/ArmGic/ArmGicDxe.inf
  #INF Binaries/shennong/ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/ProjectInfoDxe/ProjectInfoDxe.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  #INF Binaries/shennong/FatPkg/EnhancedFatDxe/Fat.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/FvSimpleFileSystemDxe/FvSimpleFileSystemDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/QcomBds/QcomBds.inf
  #INF Binaries/shennong/QcomPkg/Application/QcomChargerApp/QcomChargerApp.inf
  #INF Binaries/shennong/QcomPkg/Application/Ebl/Ebl.inf
  #INF Binaries/shennong/QcomPkg/Drivers/SimpleTextInOutSerialDxe/SimpleTextInOutSerial.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  #INF Binaries/shennong/MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/HSUartDxe/HSUartDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/ConfigWordsDxe/ConfigWordsDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/QRKSDxe/QRKSDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/GLinkDxe/GLinkDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/UsbBusDxe/UsbBusDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/UsbKbDxe/UsbKbDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/UsbMassStorageDxe/UsbMassStorageDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/SecRSADxe/SecRSADxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/ASN1X509Dxe/ASN1X509Dxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/SPSSDxe/SPSSDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/FvUtilsDxe/FvUtilsEnhancedDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/RmVmDxe/RmVmDxe.inf
  #INF Binaries/shennong/MiPkg/Protocol/MiToken/MiToken.inf
  #INF Binaries/shennong/QcomPkg/Drivers/ParserDxe/ParserEnhancedDxe.inf
  #INF Binaries/shennong/QcomPkg/Drivers/SerialPortDxe/SerialPortDxe.inf
