// SPDX-License-Identifier: GPL-2.0-only
/*
 * zuma's UFS host: the vendor steps around the standard UFSHCI bring-up, and the calls into Samsung's CAL
 * (ufs-cal-if.c), for the firmware's UFS driver (ZumaUfsDxe) through Library/ZumaUfsCalLib.h.
 *
 * Derived from Google's Linux driver for the Exynos UFS host, drivers/ufs/ufs-exynos.c (Copyright (C) 2013-2014
 * Samsung Electronics Co., Ltd.): its hce_enable_notify, link_startup_notify and pwr_change_notify callbacks
 * (exynos_ufs_init_host, exynos_ufs_config_host, exynos_ufs_dev_hw_reset, ufs_call_cal and the lane and gear
 * reads), and the register names of ufs-vs-regs.h.
 *
 * Copyright (c) 2026, AkIOS contributors.
 */

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/ZumaUfsCalLib.h>

#include <linux/io.h>
#include "ufs-vs-mmio.h"
#include "ufs-cal-if.h"

/* vendor HCI (ufs-vs-regs.h) */
#define HCI_TXPRDT_ENTRY_SIZE        0x00
#define HCI_RXPRDT_ENTRY_SIZE        0x04
#define  PRDT_PREFECT_EN             BIT(31)
#define  PRDT_SET_SIZE(x)            ((x) & 0x1F)
#define HCI_1US_TO_CNT_VAL           0x0C
#define  CNT_VAL_1US_MASK            0x3FF
#define HCI_UTRL_NEXUS_TYPE          0x40
#define HCI_UTMRL_NEXUS_TYPE         0x44
#define HCI_SW_RST                   0x50
#define  UFS_SW_RST_MASK             (BIT(1) | BIT(0))     /* UniPro, link */
#define HCI_DATA_REORDER             0x60
#define HCI_AXIDMA_RWDATA_BURST_LEN  0x6C
#define  BURST_LEN(x)                ((x) << 27 | (x))
#define  WLU_EN                      BIT(31)
#define HCI_GPIO_OUT                 0x70
#define HCI_ERROR_EN_DL_LAYER        0x7C
#define HCI_ERROR_EN_N_LAYER         0x80
#define HCI_ERROR_EN_T_LAYER         0x84
#define HCI_UFSHCI_V2P1_CTRL         0x8C
#define  IA_TICK_SEL                 BIT(16)
#define HCI_CLKSTOP_CTRL             0xB0
#define  CLK_STOP_ALL                (BIT(4) | BIT(2) | BIT(1) | BIT(0))
#define HCI_FORCE_HCS                0xB4
#define  REFCLKOUT_STOP_EN           BIT(11)
#define  MPHY_APBCLK_STOP_EN         BIT(10)
#define  UFSP_DRCG_EN                BIT(8)
#define  REFCLK_STOP_EN              BIT(7)
#define  UNIPRO_PCLK_STOP_EN         BIT(6)
#define  UNIPRO_MCLK_STOP_EN         BIT(5)
#define  HCI_CORECLK_STOP_EN         BIT(4)
#define  CLK_STOP_CTRL_EN_ALL        (UFSP_DRCG_EN | MPHY_APBCLK_STOP_EN | REFCLKOUT_STOP_EN | REFCLK_STOP_EN | \
                                      UNIPRO_PCLK_STOP_EN | UNIPRO_MCLK_STOP_EN)
#define HCI_IOP_ACG_DISABLE          0x100
#define  HCI_IOP_ACG_DISABLE_EN      BIT(0)
#define HCI_CPORT_LOG_CTRL           0x110
#define HCI_CPORT_LOG_CFG            0x114

/* device fatal error reporting: the errors Linux's ufshci.h names for DFES */
#define DFES_ERR_EN                  BIT(31)
#define DFES_DEF_DL_ERRS             (0x20 | 0x2000)       /* RX buffer overflow, PA_INIT */
#define DFES_DEF_N_ERRS              (0x1 | 0x2 | 0x4)
#define DFES_DEF_T_ERRS              (0x1 | 0x2 | 0x4 | 0x10)

/* UniPro */
#define UNIP_PA_DBG_OPTION_SUITE_1   0x39A8
#define  DBG_SUITE1_ENABLE           0x90913C1C
#define UNIP_PA_DBG_OPTION_SUITE_2   0x39B4
#define  DBG_SUITE2_ENABLE           0xE01C115F
#define UNIP_PA_AVAILTXDATALENS      0x3080
#define UNIP_PA_AVAILRXDATALENS      0x3100
#define UNIP_PA_ACTIVETXDATALENS     0x3180
#define UNIP_PA_CONNECTEDTXDATALANES 0x3184
#define UNIP_PA_ACTIVERXDATALENS     0x3200
#define UNIP_PA_CONNECTEDRXDATALANES 0x3204
#define UNIP_PA_MAXRXHSGEAR          0x321C

STATIC struct ufs_vs_handle  mHandle;
STATIC struct ufs_cal_param  mParam;
STATIC struct uic_pwr_mode   mPmd;
STATIC UINT8                 mGear;
STATIC BOOLEAN               mCalReady;

STATIC
VOID
CalDelay (
  IN u32  Us
  )
{
  MicroSecondDelay (Us);
}

/*
 * A CAL call with the PHY's APB clock and UniPro's mclk kept running (ufs_call_cal)
 */
STATIC
EFI_STATUS
CallCal (
  IN CONST CHAR8  *Name,
  IN cal_if_func  Function
  )
{
  enum ufs_cal_errno  Result;
  u32                 Reg;

  Reg = hci_readl (&mHandle, HCI_FORCE_HCS);
  hci_writel (&mHandle, Reg & ~(UNIPRO_MCLK_STOP_EN | MPHY_APBCLK_STOP_EN), HCI_FORCE_HCS);

  Result = Function (&mParam);

  Reg = hci_readl (&mHandle, HCI_FORCE_HCS);
  hci_writel (&mHandle, Reg | MPHY_APBCLK_STOP_EN, HCI_FORCE_HCS);

  if (Result != UFS_CAL_NO_ERROR) {
    DEBUG ((DEBUG_ERROR, "ZumaUfsCal: %a failed: %d\n", Name, Result));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ZumaUfsHostInit (
  IN CONST ZUMA_UFS_HOST_CONFIG  *Config
  )
{
  if ((Config == NULL) || (Config->MclkRate == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  mHandle.std    = (void *)Config->Std;
  mHandle.hci    = (void *)Config->Hci;
  mHandle.ufsp   = (void *)Config->Ufsp;
  mHandle.unipro = (void *)Config->Unipro;
  mHandle.pma    = (void *)Config->Pma;
  mHandle.cport  = (void *)Config->Cport;
  mHandle.udelay = CalDelay;

  mParam.handle          = &mHandle;
  mParam.mclk_rate       = Config->MclkRate;
  mParam.board           = Config->Board;
  mParam.evt_ver         = Config->EvtVer;
  mParam.tbl             = HOST_EMBD;
  mParam.support_ah8_cal = 0;       /* the tree has no samsung,support-ah8 */
  mGear                  = Config->Gear;

  if (ufs_cal_init (&mParam, 0) != UFS_CAL_NO_ERROR) {
    return EFI_DEVICE_ERROR;
  }

  mCalReady = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ZumaUfsHostPreHce (
  VOID
  )
{
  UINTN  Waited;
  u32    Reg;

  if (!mCalReady) {
    return EFI_NOT_READY;
  }

  /* host reset (exynos_ufs_init_host) */
  hci_writel (&mHandle, UFS_SW_RST_MASK, HCI_SW_RST);
  for (Waited = 0; hci_readl (&mHandle, HCI_SW_RST) & UFS_SW_RST_MASK; Waited++) {
    if (Waited >= 1000) {
      DEBUG ((DEBUG_ERROR, "ZumaUfsCal: host software reset timed out\n"));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  /* configure host (exynos_ufs_config_host): the internal clocks, the 1 us tick, DMA and PRDT settings */
  Reg = hci_readl (&mHandle, HCI_FORCE_HCS);
  hci_writel (&mHandle, Reg & ~HCI_CORECLK_STOP_EN, HCI_FORCE_HCS);

  Reg = hci_readl (&mHandle, HCI_UFSHCI_V2P1_CTRL);
  hci_writel (&mHandle, Reg | IA_TICK_SEL, HCI_UFSHCI_V2P1_CTRL);
  hci_writel (&mHandle, (mParam.mclk_rate / 1000000) & CNT_VAL_1US_MASK, HCI_1US_TO_CNT_VAL);

  hci_writel (&mHandle, 0xA, HCI_DATA_REORDER);
  hci_writel (&mHandle, PRDT_PREFECT_EN | PRDT_SET_SIZE (12), HCI_TXPRDT_ENTRY_SIZE);
  hci_writel (&mHandle, PRDT_SET_SIZE (12), HCI_RXPRDT_ENTRY_SIZE);
  hci_writel (&mHandle, 0xFFFFFFFF, HCI_UTRL_NEXUS_TYPE);
  hci_writel (&mHandle, 0xFFFFFFFF, HCI_UTMRL_NEXUS_TYPE);
  hci_writel (&mHandle, WLU_EN | BURST_LEN (3), HCI_AXIDMA_RWDATA_BURST_LEN);

  Reg = hci_readl (&mHandle, HCI_IOP_ACG_DISABLE);
  hci_writel (&mHandle, Reg & ~HCI_IOP_ACG_DISABLE_EN, HCI_IOP_ACG_DISABLE);

  unipro_writel (&mHandle, DBG_SUITE1_ENABLE, UNIP_PA_DBG_OPTION_SUITE_1);
  unipro_writel (&mHandle, DBG_SUITE2_ENABLE, UNIP_PA_DBG_OPTION_SUITE_2);

  /* device reset (exynos_ufs_dev_hw_reset) */
  hci_writel (&mHandle, 0, HCI_GPIO_OUT);
  MicroSecondDelay (5);
  hci_writel (&mHandle, 1, HCI_GPIO_OUT);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ZumaUfsHostPostHce (
  VOID
  )
{
  u32  Tx;
  u32  Rx;
  u32  Reg;

  /* the clock stop controls on, the internal clocks ungated (exynos_ufs_ctrl_clk, exynos_ufs_gate_clk) */
  Reg = hci_readl (&mHandle, HCI_FORCE_HCS);
  hci_writel (&mHandle, Reg | CLK_STOP_CTRL_EN_ALL, HCI_FORCE_HCS);
  Reg = hci_readl (&mHandle, HCI_CLKSTOP_CTRL);
  hci_writel (&mHandle, Reg & ~CLK_STOP_ALL, HCI_CLKSTOP_CTRL);

  /* the host's available lanes (exynos_ufs_get_available_lane) */
  Tx = unipro_readl (&mHandle, UNIP_PA_AVAILTXDATALENS);
  Rx = unipro_readl (&mHandle, UNIP_PA_AVAILRXDATALENS);
  if ((Tx == 0) || (Rx == 0) || (Tx != Rx)) {
    DEBUG ((DEBUG_ERROR, "ZumaUfsCal: invalid available lanes: rx %u tx %u\n", Rx, Tx));
    return EFI_DEVICE_ERROR;
  }

  mParam.available_lane = (u8)Rx;

  /* the CPort logger running (__thaw_cport_logger) */
  hci_writel (&mHandle, 0x22, HCI_CPORT_LOG_CFG);
  hci_writel (&mHandle, 1, HCI_CPORT_LOG_CTRL);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ZumaUfsHostPreLink (
  VOID
  )
{
  hci_writel (&mHandle, DFES_ERR_EN | DFES_DEF_DL_ERRS, HCI_ERROR_EN_DL_LAYER);
  hci_writel (&mHandle, DFES_ERR_EN | DFES_DEF_N_ERRS, HCI_ERROR_EN_N_LAYER);
  hci_writel (&mHandle, DFES_ERR_EN | DFES_DEF_T_ERRS, HCI_ERROR_EN_T_LAYER);

  /* keep the PHY's context even if the link startup fails */
  unipro_writel (&mHandle, DBG_SUITE1_ENABLE, UNIP_PA_DBG_OPTION_SUITE_1);
  unipro_writel (&mHandle, DBG_SUITE2_ENABLE, UNIP_PA_DBG_OPTION_SUITE_2);

  return CallCal ("pre_link", ufs_cal_pre_link);
}

EFI_STATUS
EFIAPI
ZumaUfsHostPostLink (
  OUT UINT8  *ConnectedLanes OPTIONAL,
  OUT UINT8  *MaxHsGear OPTIONAL
  )
{
  u32  Tx;
  u32  Rx;
  u32  MaxRxHsGear;

  /* what the capability exchange found (exynos_ufs_get_caps_after_link) */
  Tx          = unipro_readl (&mHandle, UNIP_PA_CONNECTEDTXDATALANES);
  Rx          = unipro_readl (&mHandle, UNIP_PA_CONNECTEDRXDATALANES);
  MaxRxHsGear = unipro_readl (&mHandle, UNIP_PA_MAXRXHSGEAR);
  if ((Tx == 0) || (Rx == 0) || (Tx != Rx)) {
    DEBUG ((DEBUG_ERROR, "ZumaUfsCal: asymmetric connected lanes: rx %u tx %u\n", Rx, Tx));
  }

  mParam.connected_tx_lane = (u8)Rx;
  mParam.connected_rx_lane = (u8)Rx;
  mParam.max_gear          = (u8)MIN (MaxRxHsGear, (u32)mGear);
  DEBUG ((DEBUG_WARN, "ZumaUfsCal: connected lanes rx %u tx %u, PA_MaxRxHSGear %u, max gear %u\n", Rx, Tx, MaxRxHsGear,
    mParam.max_gear));

  if (ConnectedLanes != NULL) {
    *ConnectedLanes = (UINT8)Rx;
  }

  if (MaxHsGear != NULL) {
    *MaxHsGear = mParam.max_gear;
  }

  return CallCal ("post_link", ufs_cal_post_link);
}

EFI_STATUS
EFIAPI
ZumaUfsHostPrePmc (
  IN UINT8  Mode,
  IN UINT8  Gear,
  IN UINT8  Lanes,
  IN UINT8  HsSeries
  )
{
  mPmd.mode      = Mode;
  mPmd.gear      = Gear;
  mPmd.lane      = Lanes;
  mPmd.hs_series = HsSeries;
  mParam.pmd     = &mPmd;
  return CallCal ("pre_pmc", ufs_cal_pre_pmc);
}

EFI_STATUS
EFIAPI
ZumaUfsHostPostPmc (
  VOID
  )
{
  u32  Tx;
  u32  Rx;

  /* the lanes the new power mode uses (exynos_ufs_update_active_lanes) */
  Tx = unipro_readl (&mHandle, UNIP_PA_ACTIVETXDATALENS);
  Rx = unipro_readl (&mHandle, UNIP_PA_ACTIVERXDATALENS);
  if ((Tx == 0) || (Rx == 0) || (Tx != Rx)) {
    DEBUG ((DEBUG_ERROR, "ZumaUfsCal: invalid active lanes: rx %u tx %u\n", Rx, Tx));
  }

  mParam.active_tx_lane = (u8)Tx;
  mParam.active_rx_lane = (u8)Rx;
  return CallCal ("post_pmc", ufs_cal_post_pmc);
}
