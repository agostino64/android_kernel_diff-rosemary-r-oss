/*
 * Copyright (C) 2017 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/module.h>
#include <linux/usb/usb_phy_generic.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#include "musb_core.h"
#include "mtk_musb.h"
#include "musbhsdma.h"
#include "usb20.h"

#include <mt-plat/mtk_boot_common.h>
#include <mt-plat/charger_type.h>

#ifdef FPGA_PLATFORM
#include <linux/i2c.h>
#include "phy-mtk-fpga.h"
#endif

#ifdef CONFIG_MTK_MUSB_QMU_SUPPORT
#include "musb_qmu.h"
#endif

#ifdef CONFIG_MTK_USB2JTAG_SUPPORT
#include <mt-plat/mtk_usb2jtag.h>
#endif

#ifndef FPGA_PLATFORM
#include <linux/arm-smccc.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
static void usb_dpidle_request(int mode)
{
	struct arm_smccc_res res;
	int op;

	switch (mode) {
	case USB_DPIDLE_SUSPEND:
		op = MTK_USB_SMC_INFRA_REQUEST;
		break;
	case USB_DPIDLE_RESUME:
		op = MTK_USB_SMC_INFRA_RELEASE;
		break;
	default:
		return;
	}

	DBG(0, "operatio = %d\n", op);
	arm_smccc_smc(MTK_SIP_USB_CONTROL, op, 0, 0, 0, 0, 0, 0, &res);
}
#endif

#ifdef CONFIG_USB_MTK_OTG
static struct regmap *pericfg;

static void mt_usb_wakeup(struct musb *musb, bool enable)
{
	u32 tmp;
	bool is_con = musb->port1_status & USB_PORT_STAT_CONNECTION;

	if (IS_ERR_OR_NULL(pericfg)) {
		DBG(0, "init fail");
		return;
	}

	DBG(0, "connection=%d\n", is_con);

	if (enable) {
		regmap_read(pericfg, USB_WAKEUP_DEC_CON1, &tmp);
		tmp |= USB1_CDDEBOUNCE(0x8) | USB1_CDEN;
		regmap_write(pericfg, USB_WAKEUP_DEC_CON1, tmp);

		tmp = musb_readw(musb->mregs, RESREG);
		if (is_con)
			tmp &= ~HSTPWRDWN_OPT;
		else
			tmp |= HSTPWRDWN_OPT;
		musb_writew(musb->mregs, RESREG, tmp);
	} else {
		regmap_read(pericfg, USB_WAKEUP_DEC_CON1, &tmp);
		tmp &= ~(USB1_CDEN | USB1_CDDEBOUNCE(0xf));
		regmap_write(pericfg, USB_WAKEUP_DEC_CON1, tmp);

		tmp = musb_readw(musb->mregs, RESREG);
		tmp &= ~HSTPWRDWN_OPT;
		musb_writew(musb->mregs, RESREG, tmp);
	}
}

static int mt_usb_wakeup_init(struct musb *musb)
{
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL,
					"mediatek,mt6781-usb20");
	if (!node) {
		DBG(0, "map node failed\n");
		return -ENODEV;
	}

	pericfg = syscon_regmap_lookup_by_phandle(node,
					"pericfg");
	if (IS_ERR(pericfg)) {
		DBG(0, "fail to get pericfg regs\n");
		return PTR_ERR(pericfg);
	}

	return 0;
}
#endif


static u32 cable_mode = CABLE_MODE_NORMAL;
#ifndef FPGA_PLATFORM
struct clk *musb_clk;
struct clk *musb_clk_top_sel;
struct clk *musb_clk_univpll5_d2;
static struct regulator *reg_vusb;
static struct regulator *reg_vio18;
static struct regulator *reg_va12;
#endif

void __iomem *usb_phy_base;

#ifdef CONFIG_MTK_UART_USB_SWITCH
static u32 port_mode = PORT_MODE_USB;
#define AP_GPIO_COMPATIBLE_NAME "mediatek,gpio"
void __iomem *ap_gpio_base;
#endif

/*EP Fifo Config*/
static struct musb_fifo_cfg fifo_cfg[] __initdata = {
	{.hw_ep_num = 1, .style = MUSB_FIFO_TX, .maxpacket = 512,
		.ep_mode = EP_BULK, .mode = MUSB_BUF_DOUBLE},
	{.hw_ep_num = 1, .style = MUSB_FIFO_RX, .maxpacket = 512,
		.ep_mode = EP_BULK, .mode = MUSB_BUF_DOUBLE},
	{.hw_ep_num = 2, .style = MUSB_FIFO_TX, .maxpacket = 512,
		.ep_mode = EP_BULK, .mode = MUSB_BUF_DOUBLE},
	{.hw_ep_num = 2, .style = MUSB_FIFO_RX, .maxpacket = 512,
		.ep_mode = EP_BULK, .mode = MUSB_BUF_DOUBLE},
	{.hw_ep_num = 3, .style = MUSB_FIFO_TX, .maxpacket = 512,
		.ep_mode = EP_BULK, .mode = MUSB_BUF_DOUBLE},
	{.hw_ep_num = 3, .style = MUSB_FIFO_RX, .maxpacket = 512,
		.ep_mode = EP_BULK, .mode = MUSB_BUF_DOUBLE},
	{.hw_ep_num = 4, .style = MUSB_FIFO_TX, .maxpacket = 512,
		.ep_mode = EP_BULK, .mode = MUSB_BUF_DOUBLE},
	{.hw_ep_num = 4, .style = MUSB_FIFO_RX, .maxpacket = 512,
		.ep_mode = EP_BULK, .mode = MUSB_BUF_DOUBLE},
	{.hw_ep_num = 5, .style = MUSB_FIFO_TX, .maxpacket = 512,
		.ep_mode = EP_INT, .mode = MUSB_BUF_SINGLE},
	{.hw_ep_num = 5, .style = MUSB_FIFO_RX, .maxpacket = 512,
		.ep_mode = EP_INT, .mode = MUSB_BUF_SINGLE},
	{.hw_ep_num = 6, .style = MUSB_FIFO_TX, .maxpacket = 512,
		.ep_mode = EP_INT, .mode = MUSB_BUF_SINGLE},
	{.hw_ep_num = 6, .style = MUSB_FIFO_RX, .maxpacket = 512,
		.ep_mode = EP_INT, .mode = MUSB_BUF_SINGLE},
	{.hw_ep_num = 7, .style = MUSB_FIFO_TX, .maxpacket = 512,
		.ep_mode = EP_BULK, .mode = MUSB_BUF_SINGLE},
	{.hw_ep_num = 7, .style = MUSB_FIFO_RX, .maxpacket = 512,
		.ep_mode = EP_BULK, .mode = MUSB_BUF_SINGLE},
	{.hw_ep_num = 8, .style = MUSB_FIFO_TX, .maxpacket = 512,
		.ep_mode = EP_ISO, .mode = MUSB_BUF_DOUBLE},
	{.hw_ep_num = 8, .style = MUSB_FIFO_RX, .maxpacket = 512,
		.ep_mode = EP_ISO, .mode = MUSB_BUF_DOUBLE},
};


/*=======================================================================*/
/* USB GADGET                                                     */
/*=======================================================================*/
static const struct of_device_id apusb_of_ids[] = {
	{.compatible = "mediatek,mt6781-usb20",},
	{},
};

MODULE_DEVICE_TABLE(of, apusb_of_ids);

static struct delayed_work idle_work;

void do_idle_work(struct work_struct *data)
{
	struct musb *musb = mtk_musb;
	unsigned long flags;
	u8 devctl;
	enum usb_otg_state old_state;

	usb_prepare_clock(true);

	spin_lock_irqsave(&musb->lock, flags);
	old_state = musb->xceiv->otg->state;
	if (musb->is_active) {
		DBG(0,
			"%s active, igonre do_idle\n",
			otg_state_string(musb->xceiv->otg->state));
		goto exit;
	}

	switch (musb->xceiv->otg->state) {
	case OTG_STATE_B_PERIPHERAL:
	case OTG_STATE_A_WAIT_BCON:
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		if (devctl & MUSB_DEVCTL_BDEVICE) {
			musb->xceiv->otg->state = OTG_STATE_B_IDLE;
			MUSB_DEV_MODE(musb);
		} else {
			musb->xceiv->otg->state = OTG_STATE_A_IDLE;
			MUSB_HST_MODE(musb);
		}
		break;
	case OTG_STATE_A_HOST:
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		if (devctl & MUSB_DEVCTL_BDEVICE)
			musb->xceiv->otg->state = OTG_STATE_B_IDLE;
		else
			musb->xceiv->otg->state = OTG_STATE_A_WAIT_BCON;
		break;
	default:
		break;
	}
	DBG(0, "otg_state %s to %s, is_active<%d>\n",
			otg_state_string(old_state),
			otg_state_string(musb->xceiv->otg->state),
			musb->is_active);
exit:
	spin_unlock_irqrestore(&musb->lock, flags);

	usb_prepare_clock(false);
}

static struct timer_list musb_idle_timer;

static void musb_do_idle(unsigned long _musb)
{
	struct musb *musb = (void *)_musb;

	queue_delayed_work(musb->st_wq, &idle_work, 0);
}

static void mt_usb_try_idle(struct musb *musb, unsigned long timeout)
{
	unsigned long default_timeout = jiffies + msecs_to_jiffies(3);
	static unsigned long last_timer;

	DBG(0, "skip %s\n", __func__);
	return;

	if (timeout == 0)
		timeout = default_timeout;

	/* Never idle if active, or when VBUS timeout is not set as host */
	if (musb->is_active || ((musb->a_wait_bcon == 0)
				&& (musb->xceiv->otg->state
				== OTG_STATE_A_WAIT_BCON))) {
		DBG(0, "%s active, deleting timer\n",
			otg_state_string(musb->xceiv->otg->state));
		del_timer(&musb_idle_timer);
		last_timer = jiffies;
		return;
	}

	if (time_after(last_timer, timeout)) {
		if (!timer_pending(&musb_idle_timer))
			last_timer = timeout;
		else {
			DBG(0, "Longer idle timer already pending, ignoring\n");
			return;
		}
	}
	last_timer = timeout;

	DBG(0, "%s inactive, for idle timer for %lu ms\n",
	    otg_state_string(musb->xceiv->otg->state),
	    (unsigned long)jiffies_to_msecs(timeout - jiffies));
	mod_timer(&musb_idle_timer, timeout);
}

static int real_enable = 0, real_disable;
static int virt_enable = 0, virt_disable;
static void mt_usb_enable(struct musb *musb)
{
	unsigned long flags;
	#ifdef CONFIG_MTK_UART_USB_SWITCH
	static int is_check;
	#endif

	virt_enable++;
	DBG(0, "begin <%d,%d>,<%d,%d,%d,%d>\n",
			mtk_usb_power, musb->power,
			virt_enable, virt_disable,
			real_enable, real_disable);
	if (musb->power == true)
		return;

	/* clock alredy prepare before enter here */
	usb_enable_clock(true);

	mdelay(10);
	#ifdef CONFIG_MTK_UART_USB_SWITCH
	if (!is_check) {
		usb_phy_check_in_uart_mode();
	    is_check = 1;
	}
	#endif

	flags = musb_readl(musb->mregs, USB_L1INTM);
	usb_phy_recover();

	/* update musb->power & mtk_usb_power in the same time */
	musb->power = true;
	mtk_usb_power = true;
	real_enable++;
	if (in_interrupt()) {
		DBG(0, "in interrupt !!!!!!!!!!!!!!!\n");
		DBG(0, "in interrupt !!!!!!!!!!!!!!!\n");
		DBG(0, "in interrupt !!!!!!!!!!!!!!!\n");
	}
	DBG(0, "end, <%d,%d,%d,%d>\n",
		virt_enable, virt_disable,
		real_enable, real_disable);
	musb_writel(mtk_musb->mregs, USB_L1INTM, flags);
}

static void mt_usb_disable(struct musb *musb)
{
	virt_disable++;

	DBG(0, "begin, <%d,%d>,<%d,%d,%d,%d>\n",
		mtk_usb_power, musb->power,
		virt_enable, virt_disable,
	    real_enable, real_disable);
	if (musb->power == false)
		return;

	usb_phy_savecurrent();

	usb_enable_clock(false);
	/* clock will unprepare when leave here */

	real_disable++;
	DBG(0, "end, <%d,%d,%d,%d>\n",
		virt_enable, virt_disable,
		real_enable, real_disable);

	/* update musb->power & mtk_usb_power in the same time */
	musb->power = 0;
	mtk_usb_power = false;
}

/* ================================ */
/* connect and disconnect functions */
/* ================================ */
bool mt_usb_is_device(void)
{
	DBG(4, "called\n");

	if (!mtk_musb) {
		DBG(0, "mtk_musb is NULL\n");
		/* don't do charger detection when usb is not ready */
		return false;
	}
	DBG(4, "is_host=%d\n", mtk_musb->is_host);

#ifdef CONFIG_MTK_UART_USB_SWITCH
	if (in_uart_mode) {
		DBG(0, "in UART Mode\n");
		return false;
	}
#endif
#ifdef CONFIG_USB_MTK_OTG
	return !usb20_check_vbus_on();
#else
	return true;
#endif
}

/* to avoid build error due to PMIC module not ready */
#ifndef CONFIG_MTK_CHARGER
#define BYPASS_PMIC_LINKAGE
#endif

#ifdef BYPASS_PMIC_LINKAGE
static struct delayed_work disconnect_check_work;
static bool musb_hal_is_vbus_exist(void);
void do_disconnect_check_work(struct work_struct *data)
{
	bool vbus_exist = false;
	unsigned long flags = 0;
	struct musb *musb = mtk_musb;

	msleep(200);

	vbus_exist = musb_hal_is_vbus_exist();
	DBG(1, "vbus_exist:<%d>\n", vbus_exist);
	if (vbus_exist)
		return;

	spin_lock_irqsave(&mtk_musb->lock, flags);
	DBG(1, "speed <%d>\n", musb->g.speed);
	/* notify gadget driver, g.speed judge is very important */
	if (!musb->is_host && musb->g.speed != USB_SPEED_UNKNOWN) {
		DBG(0, "musb->gadget_driver:%p\n", musb->gadget_driver);
		if (musb->gadget_driver && musb->gadget_driver->disconnect) {
			DBG(0, "musb->gadget_driver->disconnect:%p\n",
					musb->gadget_driver->disconnect);
			/* align musb_g_disconnect */
			spin_unlock(&musb->lock);
			musb->gadget_driver->disconnect(&musb->g);
			spin_lock(&musb->lock);

		}
		musb->g.speed = USB_SPEED_UNKNOWN;
	}
	DBG(1, "speed <%d>\n", musb->g.speed);
	spin_unlock_irqrestore(&mtk_musb->lock, flags);
}
void trigger_disconnect_check_work(void)
{
	static int inited;

	if (!inited) {
		INIT_DELAYED_WORK(&disconnect_check_work,
			do_disconnect_check_work);
		inited = 1;
	}
	queue_delayed_work(mtk_musb->st_wq, &disconnect_check_work, 0);
}
#endif

static enum charger_type musb_hal_get_charger_type(void)
{
	enum charger_type chg_type;
#ifdef BYPASS_PMIC_LINKAGE
	DBG(0, "force on");
	chg_type = STANDARD_HOST;
#else
	chg_type = mt_get_charger_type();
#endif
	return chg_type;
}
static bool musb_hal_is_vbus_exist(void)
{
	bool vbus_exist;

#ifdef BYPASS_PMIC_LINKAGE
	DBG(0, "force on");
	vbus_exist = true;
#else
#ifdef CONFIG_POWER_EXT
	vbus_exist = upmu_get_rgs_chrdet();
#else
	vbus_exist = upmu_is_chr_det();
#endif
#endif

	return vbus_exist;
}

DEFINE_MUTEX(cable_connected_lock);
/* be aware this could not be used in non-sleep context */
bool usb_cable_connected(void)
{
	enum charger_type chg_type = CHARGER_UNKNOWN;
	bool connected = false, vbus_exist = false;

	mutex_lock(&cable_connected_lock);

	/* TYPE CHECK */
	chg_type = musb_hal_get_charger_type();

	if (musb_fake_CDP && chg_type == STANDARD_HOST) {
		DBG(0, "fake to type 2\n");
		chg_type = CHARGING_HOST;
	}

	if (chg_type == STANDARD_HOST || chg_type == CHARGING_HOST)
		connected = true;

	/* VBUS CHECK to avoid type miss-judge */
	vbus_exist = musb_hal_is_vbus_exist();

	if (!vbus_exist)
		connected = false;

	DBG(0, "connected=%d vbus_exist=%d type=%d\n",
		connected, vbus_exist, chg_type);

	mutex_unlock(&cable_connected_lock);
	return connected;
}

static bool cmode_effect_on(void)
{
	enum charger_type chg_type = CHARGER_UNKNOWN;
	bool effect = false;

	/* TYPE CHECK */
	chg_type = musb_hal_get_charger_type();

	/* CMODE CHECK */
	if (cable_mode == CABLE_MODE_CHRG_ONLY ||
		(cable_mode == CABLE_MODE_HOST_ONLY &&
			chg_type != CHARGING_HOST))
		effect = true;

	DBG(0, "cable_mode=%d, effect=%d\n", cable_mode, effect);
	return effect;
}

void do_connection_work(struct work_struct *data)
{
	unsigned long flags = 0;
	int usb_clk_state = NO_CHANGE;
	bool usb_on, usb_connected;
	struct mt_usb_work *work =
		container_of(data, struct mt_usb_work, dwork.work);

	DBG(0, "is_host<%d>, power<%d>, ops<%d>\n",
		mtk_musb->is_host, mtk_musb->power, work->ops);

	/* always prepare clock and check if need to unprepater later */
	/* clk_prepare_cnt +1 here*/
	usb_prepare_clock(true);

	/* be aware this could not be used in non-sleep context */
	usb_connected = usb_cable_connected();

	/* additional check operation here */
	if (musb_force_on)
		usb_on = true;
	else if (work->ops == CONNECTION_OPS_CHECK)
		usb_on = usb_connected;
	else
		usb_on = (work->ops ==
			CONNECTION_OPS_CONN ? true : false);

	if (cmode_effect_on())
		usb_on = false;
	/* additional check operation done */

	spin_lock_irqsave(&mtk_musb->lock, flags);

	if (mtk_musb->is_host) {
		DBG(0, "is host, return\n");
		goto exit;
	}

#ifdef CONFIG_MTK_UART_USB_SWITCH
	if (in_uart_mode) {
		DBG(0, "in uart mode, return\n");
		goto exit;
	}
#endif

	if (!mtk_musb->power && (usb_on == true)) {
		/* enable usb */
		if (!mtk_musb->usb_lock->active) {
			__pm_stay_awake(mtk_musb->usb_lock);
			DBG(0, "lock\n");
		} else {
			DBG(0, "already lock\n");
		}

		/* note this already put SOFTCON */
		musb_start(mtk_musb);
		usb_clk_state = OFF_TO_ON;

	} else if (mtk_musb->power && (usb_on == false)) {
		/* disable usb */
		musb_stop(mtk_musb);
		if (mtk_musb->usb_lock->active) {
			DBG(0, "unlock\n");
			__pm_relax(mtk_musb->usb_lock);
		} else {
			DBG(0, "lock not active\n");
		}
		usb_clk_state = ON_TO_OFF;
		mtk_musb->xceiv->otg->state = OTG_STATE_B_IDLE;
	} else
		DBG(0, "do nothing, usb_on:%d, power:%d\n",
				usb_on, mtk_musb->power);
exit:
	spin_unlock_irqrestore(&mtk_musb->lock, flags);

	if (usb_clk_state == ON_TO_OFF) {
		/* clock on -> of: clk_prepare_cnt -2 */
		usb_prepare_clock(false);
		usb_prepare_clock(false);
	} else if (usb_clk_state == NO_CHANGE) {
		/* clock no change : clk_prepare_cnt -1 */
		usb_prepare_clock(false);
	}

	/* free mt_usb_work */
	kfree(work);
}

static void issue_connection_work(int ops)
{
	struct mt_usb_work *work;

	if (!mtk_musb) {
		DBG(0, "mtk_musb = NULL\n");
		return;
	}
	/* create and prepare worker */
	work = kzalloc(sizeof(struct mt_usb_work), GFP_ATOMIC);
	if (!work) {
		DBG(0, "wrap is NULL, directly return\n");
		return;
	}
	work->ops = ops;
	INIT_DELAYED_WORK(&work->dwork, do_connection_work);
	/* issue connection work */
	DBG(0, "issue work, ops<%d>\n", ops);
	queue_delayed_work(mtk_musb->st_wq, &work->dwork, 0);
}

void mt_usb_connect(void)
{
	DBG(0, "[MUSB] USB connect\n");
	issue_connection_work(CONNECTION_OPS_CONN);
}

void mt_usb_disconnect(void)
{
	DBG(0, "[MUSB] USB disconnect\n");
	issue_connection_work(CONNECTION_OPS_DISC);
}

static void mt_usb_reconnect(void)
{
	DBG(0, "[MUSB] USB reconnect\n");
	issue_connection_work(CONNECTION_OPS_CHECK);
}

/* build time force on */
#if defined(CONFIG_FPGA_EARLY_PORTING) ||\
		defined(U3_COMPLIANCE) || defined(FOR_BRING_UP)
#define BYPASS_PMIC_LINKAGE
#endif

void musb_platform_reset(struct musb *musb)
{
	u16 swrst = 0;
	void __iomem *mbase = musb->mregs;
	u8 bit;

	/* clear all DMA enable bit */
	for (bit = 0; bit < MUSB_HSDMA_CHANNELS; bit++)
		musb_writew(mbase,
			MUSB_HSDMA_CHANNEL_OFFSET(bit, MUSB_HSDMA_CONTROL), 0);

	/* set DMA channel 0 burst mode to boost QMU speed */
	musb_writel(musb->mregs, 0x204,
			musb_readl(musb->mregs, 0x204) | 0x600);
#ifdef CONFIG_MTK_MUSB_DRV_36BIT
	/* eanble DMA channel 0 36-BIT support */
	musb_writel(musb->mregs, 0x204,
			musb_readl(musb->mregs, 0x204) | 0x4000);
#endif

	swrst = musb_readw(mbase, MUSB_SWRST);
	swrst |= (MUSB_SWRST_DISUSBRESET | MUSB_SWRST_SWRST);
	musb_writew(mbase, MUSB_SWRST, swrst);
}

bool is_switch_charger(void)
{
#ifdef SWITCH_CHARGER
	return true;
#else
	return false;
#endif
}

void pmic_chrdet_int_en(int is_on)
{
#ifndef FPGA_PLATFORM
#ifdef CONFIG_MTK_PMIC
	DBG(0, "is_on<%d>\n", is_on);
	upmu_interrupt_chrdet_int_en(is_on);
#else
	DBG(0, "FIXME, no upmu_interrupt_chrdet_int_en ???\n");
#endif
#endif
}

void musb_sync_with_bat(struct musb *musb, int usb_state)
{
#ifndef FPGA_PLATFORM

	DBG(1, "BATTERY_SetUSBState, state=%d\n", usb_state);
#ifdef CONFIG_MTK_CHARGER
	BATTERY_SetUSBState(usb_state);
#endif
#endif
}

/*-------------------------------------------------------------------------*/
static irqreturn_t generic_interrupt(int irq, void *__hci)
{
	irqreturn_t retval = IRQ_NONE;
	struct musb *musb = __hci;

	/* musb_read_clear_generic_interrupt */
	musb->int_usb =
	musb_readb(musb->mregs, MUSB_INTRUSB) &
				musb_readb(musb->mregs, MUSB_INTRUSBE);
	musb->int_tx = musb_readw(musb->mregs, MUSB_INTRTX) &
				musb_readw(musb->mregs, MUSB_INTRTXE);
	musb->int_rx = musb_readw(musb->mregs, MUSB_INTRRX) &
				musb_readw(musb->mregs, MUSB_INTRRXE);
#ifdef CONFIG_MTK_MUSB_QMU_SUPPORT
	musb->int_queue = musb_readl(musb->mregs, MUSB_QISAR);
#endif
	/* hw status up to date before W1C */
	mb();
	musb_writew(musb->mregs, MUSB_INTRRX, musb->int_rx);
	musb_writew(musb->mregs, MUSB_INTRTX, musb->int_tx);
	musb_writeb(musb->mregs, MUSB_INTRUSB, musb->int_usb);
#ifdef CONFIG_MTK_MUSB_QMU_SUPPORT
	if (musb->int_queue) {
		musb_writel(musb->mregs, MUSB_QISAR, musb->int_queue);
		musb->int_queue &= ~(musb_readl(musb->mregs, MUSB_QIMR));
	}
#endif
	/* musb_read_clear_generic_interrupt */

#ifdef CONFIG_MTK_MUSB_QMU_SUPPORT
	if (musb->int_usb || musb->int_tx || musb->int_rx || musb->int_queue)
		retval = musb_interrupt(musb);
#else
	if (musb->int_usb || musb->int_tx || musb->int_rx)
		retval = musb_interrupt(musb);
#endif


	return retval;
}

static irqreturn_t mt_usb_interrupt(int irq, void *dev_id)
{
	irqreturn_t tmp_status;
	irqreturn_t status = IRQ_NONE;
	struct musb *musb = (struct musb *)dev_id;
	u32 usb_l1_ints;
	unsigned long flags;

	spin_lock_irqsave(&musb->lock, flags);
	usb_l1_ints = musb_readl(musb->mregs, USB_L1INTS) &
		musb_readl(mtk_musb->mregs, USB_L1INTM);
	DBG(1, "usb interrupt assert %x %x  %x %x %x %x %x\n", usb_l1_ints,
	    musb_readl(mtk_musb->mregs, USB_L1INTM),
	    musb_readb(musb->mregs, MUSB_INTRUSBE),
		musb_readw(musb->mregs, MUSB_INTRTX),
		musb_readw(musb->mregs, MUSB_INTRTXE),
		musb_readw(musb->mregs, MUSB_INTRRX),
		musb_readw(musb->mregs, MUSB_INTRRXE));

	if ((usb_l1_ints & TX_INT_STATUS) || (usb_l1_ints & RX_INT_STATUS)
	    || (usb_l1_ints & USBCOM_INT_STATUS)
#ifdef CONFIG_MTK_MUSB_QMU_SUPPORT
	    || (usb_l1_ints & QINT_STATUS)
#endif
	   ) {
		tmp_status = generic_interrupt(irq, musb);
		if (tmp_status != IRQ_NONE)
			status = tmp_status;
	}
	spin_unlock_irqrestore(&musb->lock, flags);

	/* FIXME, workaround for device_qmu + host_dma */
#if 1
/* #ifndef CONFIG_MTK_MUSB_QMU_SUPPORT */
	if (usb_l1_ints & DMA_INT_STATUS) {
		tmp_status = dma_controller_irq(irq, musb->dma_controller);
		if (tmp_status != IRQ_NONE)
			status = tmp_status;
	}
#endif

	return status;

}

/*--FOR INSTANT POWER ON USAGE --*/
static ssize_t mt_usb_show_cmode(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (!dev) {
		DBG(0, "dev is null!!\n");
		return 0;
	}
	return scnprintf(buf, PAGE_SIZE, "%d\n", cable_mode);
}

static ssize_t mt_usb_store_cmode(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int cmode;
	long tmp_val;

	if (!dev) {
		DBG(0, "dev is null!!\n");
		return count;
	/* } else if (1 == sscanf(buf, "%d", &cmode)) { */
	} else if (kstrtol(buf, 10, (long *)&tmp_val) == 0) {
		if (mtk_musb) {
			if (down_interruptible(&mtk_musb->musb_lock))
				DBG(0,
				"USB20: %s: busy, Couldn't get power_clock_lock\n",
				__func__);
		}
		cmode = tmp_val;
		DBG(0, "cmode=%d, cable_mode=%d\n", cmode, cable_mode);
		if (cmode >= CABLE_MODE_MAX)
			cmode = CABLE_MODE_NORMAL;

		if (cable_mode != cmode) {
			cable_mode = cmode;
			mt_usb_reconnect();
			/* let conection work do its job */
			msleep(50);
		}
		if (mtk_musb)
			up(&mtk_musb->musb_lock);
	}
	return count;
}

DEVICE_ATTR(cmode, 0664, mt_usb_show_cmode, mt_usb_store_cmode);

static bool saving_mode;

static ssize_t mt_usb_show_saving_mode(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (!dev) {
		DBG(0, "dev is null!!\n");
		return 0;
	}
	return scnprintf(buf, PAGE_SIZE, "%d\n", saving_mode);
}

static ssize_t mt_usb_store_saving_mode(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int saving;
	long tmp_val;

	if (!dev) {
		DBG(0, "dev is null!!\n");
		return count;
	/* } else if (1 == sscanf(buf, "%d", &saving)) { */
	} else if (kstrtol(buf, 10, (long *)&tmp_val) == 0) {
		saving = tmp_val;
		DBG(0, "old=%d new=%d\n", saving, saving_mode);
		if (saving_mode == (!saving))
			saving_mode = !saving_mode;
	}
	return count;
}

bool is_saving_mode(void)
{
	DBG(0, "%d\n", saving_mode);
	return saving_mode;
}

void usb_dump_debug_register(void)
{
	struct musb *musb = mtk_musb;

	usb_enable_clock(true);

	/* 1:Read 0x11200620; */
	pr_notice("[IPI USB dump]addr: 0x620, value: %x\n",
					musb_readl(musb->mregs, 0x620));

	/* 2: set 0x11200600[5:0]  = 0x23; */
	/* Read 0x11200634; */
	musb_writew(musb->mregs, 0x600, 0x23);
	pr_notice("[IPI USB dump]addr: 0x634, 0x23 value: %x\n",
					musb_readl(musb->mregs, 0x634));

	/* 3: set 0x11200600[5:0]  = 0x24; */
	/* Read 0x11200634; */
	musb_writew(musb->mregs, 0x600, 0x24);
	pr_notice("[IPI USB dump]addr: 0x634, 0x24 value: %x\n",
					musb_readl(musb->mregs, 0x634));

	/* 4:set 0x11200600[5:0]  = 0x25; */
	/* Read 0x11200634; */
	musb_writew(musb->mregs, 0x600, 0x25);
	pr_notice("[IPI USB dump]addr: 0x634, 0x25 value: %x\n",
					musb_readl(musb->mregs, 0x634));

	/* 5:set 0x11200600[5:0]  = 0x26; */
	/* Read 0x11200634; */
	musb_writew(musb->mregs, 0x600, 0x26);
	pr_notice("[IPI USB dump]addr: 0x634, 0x26 value: %x\n",
					musb_readl(musb->mregs, 0x634));

	usb_enable_clock(false);
}

DEVICE_ATTR(saving, 0664, mt_usb_show_saving_mode, mt_usb_store_saving_mode);

#ifdef CONFIG_MTK_UART_USB_SWITCH
static void uart_usb_switch_dump_register(void)
{
	usb_enable_clock(true);

	DBG(0, "[MUSB]addr: 0x68, value: %x\n"
			"[MUSB]addr: 0x6C, value: %x\n"
			"[MUSB]addr: 0x20, value: %x\n"
			"[MUSB]addr: 0x18, value: %x\n",
			USBPHY_READ32(0x68),
			USBPHY_READ32(0x6C),
			USBPHY_READ32(0x20),
			USBPHY_READ32(0x18));

	usb_enable_clock(false);
	DBG(0, "[MUSB]GPIO_SEL=%x\n", GET_GPIO_SEL_VAL(readl(ap_gpio_base)));
}

static ssize_t mt_usb_show_portmode(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	if (!dev) {
		DBG(0, "dev is null!!\n");
		return 0;
	}
	usb_prepare_enable_clock(true);

	if (usb_phy_check_in_uart_mode())
		port_mode = PORT_MODE_UART;
	else
		port_mode = PORT_MODE_USB;

	if (port_mode == PORT_MODE_USB)
		DBG(0, "\nUSB Port mode -> USB\n");
	else if (port_mode == PORT_MODE_UART)
		DBG(0, "\nUSB Port mode -> UART\n");

	uart_usb_switch_dump_register();

	usb_prepare_enable_clock(false);

	return scnprintf(buf, PAGE_SIZE, "%d\n", port_mode);
}

static ssize_t mt_usb_store_portmode(struct device *dev,
					struct device_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned int portmode;

	if (!dev) {
		DBG(0, "dev is null!!\n");
		return count;
	} else if (kstrtouint(buf, 10, &portmode) == 0) {
		usb_prepare_enable_clock(true);
		DBG(0,
		"\nUSB Port mode: current => %d (port_mode), change to => %d (portmode)\n",
		    port_mode, portmode);
		if (portmode >= PORT_MODE_MAX)
			portmode = PORT_MODE_USB;

		if (port_mode != portmode) {
			/* Changing to USB Mode */
			if (portmode == PORT_MODE_USB) {
				DBG(0, "USB Port mode -> USB\n");
				usb_phy_switch_to_usb();
				/* Changing to UART Mode */
			} else if (portmode == PORT_MODE_UART) {
				DBG(0, "USB Port mode -> UART\n");
				usb_phy_switch_to_uart();
			}
			uart_usb_switch_dump_register();
			port_mode = portmode;
		}
		usb_prepare_enable_clock(false);
	}
	return count;
}

DEVICE_ATTR(portmode, 0664, mt_usb_show_portmode, mt_usb_store_portmode);

static ssize_t mt_usb_show_uart_path(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	u32 var;

	if (!dev) {
		DBG(0, "dev is null!!\n");
		return 0;
	}

	var = GET_GPIO_SEL_VAL(readl(ap_gpio_base));
	DBG(0, "[MUSB]GPIO SELECT=%x\n", var);

	return scnprintf(buf, PAGE_SIZE, "%x\n", var);
}

DEVICE_ATTR(uartpath, 0444, mt_usb_show_uart_path, NULL);
#endif

#ifndef FPGA_PLATFORM
static struct device_attribute *mt_usb_attributes[] = {
	&dev_attr_saving,
#ifdef CONFIG_MTK_UART_USB_SWITCH
	&dev_attr_portmode,
	&dev_attr_uartpath,
#endif
	NULL
};

static int init_sysfs(struct device *dev)
{
	struct device_attribute **attr;
	int rc;

	for (attr = mt_usb_attributes; *attr; attr++) {
		rc = device_create_file(dev, *attr);
		if (rc)
			goto out_unreg;
	}
	return 0;

out_unreg:
	for (; attr >= mt_usb_attributes; attr--)
		device_remove_file(dev, *attr);
	return rc;
}
#endif

#ifdef FPGA_PLATFORM
static struct i2c_client *usb_i2c_client;
static const struct i2c_device_id usb_i2c_id[] = { {"mtk-usb", 0}, {} };

void USB_PHY_Write_Register8(u8 var, u8 addr)
{
	char buffer[2];

	buffer[0] = addr;
	buffer[1] = var;
	i2c_master_send(usb_i2c_client, buffer, 2);
}

u8 USB_PHY_Read_Register8(u8 addr)
{
	u8 var;

	i2c_master_send(usb_i2c_client, &addr, 1);
	i2c_master_recv(usb_i2c_client, &var, 1);
	return var;
}

#define U3_PHY_PAGE 0xff

void _u3_write_bank(u32 value)
{
	USB_PHY_Write_Register8((u8)value, (u8)U3_PHY_PAGE);
}

u32 _u3_read_reg(u32 address)
{
	u8 databuffer = 0;

	databuffer = USB_PHY_Read_Register8((u8)address);
	return databuffer;
}

void _u3_write_reg(u32 address, u32 value)
{
	USB_PHY_Write_Register8((u8)value, (u8)address);
}

u32 u3_phy_read_reg32(u32 addr)
{
	u32 bank;
	u32 addr8;
	u32 data;

	bank = (addr >> 16) & 0xff;
	addr8 = addr & 0xff;

	_u3_write_bank(bank);
	data = _u3_read_reg(addr8);
	data |= (_u3_read_reg(addr8 + 1) << 8);
	data |= (_u3_read_reg(addr8 + 2) << 16);
	data |= (_u3_read_reg(addr8 + 3) << 24);
	return data;
}

u32 u3_phy_write_reg32(u32 addr, u32 data)
{
	u32 bank;
	u32 addr8;
	u32 data_0, data_1, data_2, data_3;

	bank = (addr >> 16) & 0xff;
	addr8 = addr & 0xff;
	data_0 = data & 0xff;
	data_1 = (data >> 8) & 0xff;
	data_2 = (data >> 16) & 0xff;
	data_3 = (data >> 24) & 0xff;

	_u3_write_bank(bank);
	_u3_write_reg(addr8, data_0);
	_u3_write_reg(addr8 + 1, data_1);
	_u3_write_reg(addr8 + 2, data_2);
	_u3_write_reg(addr8 + 3, data_3);

	return 0;
}

void u3_phy_write_field32(int addr, int offset, int mask, int value)
{
	u32 cur_value;
	u32 new_value;

	cur_value = u3_phy_read_reg32(addr);
	new_value = (cur_value & (~mask)) | ((value << offset) & mask);

	u3_phy_write_reg32(addr, new_value);
}

u32 u3_phy_write_reg8(u32 addr, u8 data)
{
	u32 bank;
	u32 addr8;

	bank = (addr >> 16) & 0xff;
	addr8 = addr & 0xff;
	_u3_write_bank(bank);
	_u3_write_reg(addr8, data);

	return 0;
}

int phy_init_a60931(struct u3phy_info *info)
{
	/* 0xFC[31:24], Change bank address to 0 */
	//phy_writeb(i2c, 0x60, 0xff, 0x0);
	/* 0x14[14:12],  RG_USB20_HSTX_SRCTRL, set U2 slew rate as 4 */
	u3_phy_write_field32(((u32)&info->u2phy_regs_a->usbphyacr5),
		A60931_RG_USB20_HSTX_SRCTRL_OFST, A60931_RG_USB20_HSTX_SRCTRL, 0x4);

	/* 0x18[23:23],  RG_USB20_BC11_SW_EN, Disable BC 1.1 */
	//phy_writelmsk(i2c, 0x60, 0x18, 23, BIT(23), 0x0);
	u3_phy_write_field32(((u32)&info->u2phy_regs_a->usbphyacr6),
		A60931_RG_USB20_BC11_SW_EN_OFST, A60931_RG_USB20_BC11_SW_EN, 0x0);

	/* 0x68[18:18],  force_suspendm = 0 */
	//phy_writelmsk(i2c, 0x60, 0x68, 18, BIT(18), 0x0);
	u3_phy_write_field32(((u32)&info->u2phy_regs_a->u2phydtm0),
		A60931_FORCE_SUSPENDM_OFST, A60931_FORCE_SUSPENDM, 0x0);

	/* 0xFC[31:24], Change bank address to 0x30 */
	//phy_writeb(i2c, 0x60, 0xff, 0x30);
	/* 0x04[29:29],  RG_VUSB10_ON, SSUSB 1.0V power ON */
	//phy_writelmsk(i2c, 0x60, 0x04, 29, BIT(29), 0x1);
	u3_phy_write_field32(((u32)&info->u3phya_regs_a->reg1),
		A60931_RG_VUSB10_ON_OFST, A60931_RG_VUSB10_ON, 0x1);

	/* 0x04[25:21], RG_SSUSB_XTAL_TOP_RESERVE */
	//phy_writelmsk(i2c, 0x60, 0x04, 21, GENMASK(25, 21), 0x11);
	u3_phy_write_field32(((u32)&info->u3phya_regs_a->reg1),
		A60931_RG_SSUSB_XTAL_TOP_RESERVE_OFST,
		A60931_RG_SSUSB_XTAL_TOP_RESERVE, 0x11);

	/* 0xFC[31:24], Change bank address to 0x40 */
	//phy_writeb(i2c, 0x60, 0xff, 0x40);

	/* 0x38[15:0], DA_SSUSB_PLL_SSC_DELTA1 */
	/* fine tune SSC delta1 to let SSC min average ~0ppm */
	//phy_writelmsk(i2c, 0x60, 0x38, 0, GENMASK(15, 0)<<0, 0x47);
	u3_phy_write_field32(((u32)&info->u3phya_da_regs_a->reg19),
		A60931_RG_SSUSB_PLL_SSC_DELTA1_U3_OFST,
		A60931_RG_SSUSB_PLL_SSC_DELTA1_U3, 0x47);

	/* 0x40[31:16], DA_SSUSB_PLL_SSC_DELTA */
	/* fine tune SSC delta to let SSC min average ~0ppm */
	//phy_writelmsk(i2c, 0x60, 0x40, 16, GENMASK(31, 16), 0x44);
	u3_phy_write_field32(((u32)&info->u3phya_da_regs_a->reg21),
		A60931_RG_SSUSB_PLL_SSC_DELTA_U3_OFST,
		A60931_RG_SSUSB_PLL_SSC_DELTA_U3, 0x44);

	/* 0xFC[31:24], Change bank address to 0x30 */
	//phy_writeb(i2c, 0x60, 0xff, 0x30);
	/* 0x14[15:0],  RG_SSUSB_PLL_SSC_PRD */
	/* fine tune SSC PRD to let SSC freq average 31.5KHz */
	//phy_writelmsk(i2c, 0x60, 0x14, 0, GENMASK(15, 0), 0x190);
	u3_phy_write_field32(((u32)&info->u3phya_regs_a->reg7),
		A60931_RG_SSUSB_PLL_SSC_PRD_OFST, A60931_RG_SSUSB_PLL_SSC_PRD, 0x190);

	/* ToDo: PCIE CODA A60931A_PCIE_GLB_CSR_Description */
#ifdef CONFIG_A60931_PCIE
	/* 0xFC[31:24], Change bank address to 0x70 */
	//phy_writeb(i2c, 0x70, 0xff, 0x70);
	/* 0x88[3:2], Pipe reset, clk driving current */
	phy_writelmsk(i2c, 0x70, 0x88, 2, GENMASK(3, 2), 0x1);
	/* 0x88[5:4], Data lane 0 driving current */
	phy_writelmsk(i2c, 0x70, 0x88, 4, GENMASK(5, 4), 0x1);
	/* 0x88[7:6], Data lane 1 driving current */
	phy_writelmsk(i2c, 0x70, 0x88, 6, GENMASK(7, 6), 0x1);
	/* 0x88[9:8], Data lane 2 driving current */
	phy_writelmsk(i2c, 0x70, 0x88, 8, GENMASK(9, 8), 0x1);
	/* 0x88[11:10], Data lane 3 driving current */
	phy_writelmsk(i2c, 0x70, 0x88, 10, GENMASK(11, 10), 0x1);
	/* 0x9C[4:0],  rg_ssusb_ckphase, PCLK phase 0x00~0x1F */
	phy_writelmsk(i2c, 0x70, 0x9c, 0, GENMASK(4, 0), 0x19);
#endif
	/* Set INTR & TX/RX Impedance */

	/* 0xFC[31:24], Change bank address to 0x30 */
	//phy_writeb(i2c, 0x60, 0xff, 0x30);

	/* 0x00[26:26],  RG_SSUSB_INTR_EN */
	//phy_writelmsk(i2c, 0x60, 0x00, 26, BIT(26), 0x1);
	u3_phy_write_field32(((u32)&info->u3phya_regs_a->reg0),
		A60931_RG_SSUSB_INTR_EN_OFST, A60931_RG_SSUSB_INTR_EN, 0x1);

	/* 0x00[15:10],  RG_SSUSB_IEXT_INTR_CTRL, Set Iext R selection */
	//phy_writelmsk(i2c, 0x60, 0x00, 10, GENMASK(15, 10), 0x26);
	u3_phy_write_field32(((u32)&info->u3phya_regs_a->reg0),
		A60931_RG_SSUSB_IEXT_INTR_CTRL_OFST, A60931_RG_SSUSB_IEXT_INTR_CTRL, 0x26);

	/* 0xFC[31:24], Change bank address to 0x10 */
	//phy_writeb(i2c, 0x60, 0xff, 0x10);

	/* 0x10[31:31],  rg_ssusb_force_tx_impsel,  enable */
	//phy_writelmsk(i2c, 0x60, 0x10, 31, BIT(31), 0x1);
	u3_phy_write_field32(((u32)&info->u3phyd_regs_a->phyd_impcal0),
		A60931_RG_SSUSB_FORCE_TX_IMPSEL_OFST, A60931_RG_SSUSB_FORCE_TX_IMPSEL, 0x1);

	/* 0x10[28:24],  rg_ssusb_tx_impsel, Set TX Impedance */
	//phy_writelmsk(i2c, 0x60, 0x10, 24, GENMASK(28, 24), 0x10);
	u3_phy_write_field32(((u32)&info->u3phyd_regs_a->phyd_impcal0),
		A60931_RG_SSUSB_TX_IMPSEL_OFST, A60931_RG_SSUSB_RX_IMPSEL, 0x10);

	/* 0x14[31:31],  rg_ssusb_force_rx_impsel, enable */
	//phy_writelmsk(i2c, 0x60, 0x14, 31, BIT(31), 0x1);
	u3_phy_write_field32(((u32)&info->u3phyd_regs_a->phyd_impcal1),
		A60931_RG_SSUSB_FORCE_RX_IMPSEL_OFST, A60931_RG_SSUSB_FORCE_RX_IMPSEL, 0x1);

	/* 0x14[28:24],  rg_ssusb_rx_impsel, Set RX Impedance */
	//phy_writelmsk(i2c, 0x60, 0x14, 24, GENMASK(28, 24), 0x10);
	u3_phy_write_field32(((u32)&info->u3phyd_regs_a->phyd_impcal1),
		A60931_RG_SSUSB_RX_IMPSEL_OFST, A60931_RG_SSUSB_RX_IMPSEL, 0x10);

	/* 0xFC[31:24], Change bank address to 0x00 */
	//phy_writeb(i2c, 0x60, 0xff, 0x00);
	/* 0x00[05:05],  RG_USB20_INTR_EN, U2 INTR_EN */
	//phy_writelmsk(i2c, 0x60, 0x00, 5, BIT(5), 0x1);
	u3_phy_write_field32(((u32)&info->u2phy_regs_a->usbphyacr0),
		A60931_RG_USB20_INTR_EN_OFST, A60931_RG_USB20_INTR_EN, 0x1);

	/* 0x04[23:19],  RG_USB20_INTR_CAL, Set Iext R selection */
	//phy_writelmsk(i2c, 0x60, 0x04, 19, GENMASK(23, 19), 0x14);
	u3_phy_write_field32(((u32)&info->u2phy_regs_a->usbphyacr1),
		A60931_RG_USB20_INTR_CAL_OFST, A60931_RG_USB20_INTR_CAL, 0x14);

	return 0;
}

int phy_init_a60810(struct u3phy_info *info)
{
	/* BANK 0x00 */
	/* for U2 hS eye diagram */
	u3_phy_write_field32(((u32)
		&info->u2phy_regs_a->usbphyacr1)
		, A60810_RG_USB20_TERM_VREF_SEL_OFST
		, A60810_RG_USB20_TERM_VREF_SEL
		, 0x05);
	/* for U2 hS eye diagram */
	u3_phy_write_field32(((u32)
		&info->u2phy_regs_a->usbphyacr1)
		, A60810_RG_USB20_VRT_VREF_SEL_OFST
		, A60810_RG_USB20_VRT_VREF_SEL
		, 0x05);
	/* for U2 sensititvity */
	u3_phy_write_field32(((u32)
		&info->u2phy_regs_a->usbphyacr6)
		, A60810_RG_USB20_SQTH_OFST
		, A60810_RG_USB20_SQTH
		, 0x04);

	/* BANK 0x10 */
	/* disable ssusb_p3_entry to work around resume from P3 bug */
	u3_phy_write_field32(((u32)
		&info->u3phyd_regs_a->phyd_lfps0)
		, A60810_RG_SSUSB_P3_ENTRY_OFST
		, A60810_RG_SSUSB_P3_ENTRY
		, 0x00);
	/* force disable ssusb_p3_entry to
	 * work around resume from P3 bug
	 */
	u3_phy_write_field32(((u32)
		&info->u3phyd_regs_a->phyd_lfps0)
		, A60810_RG_SSUSB_P3_ENTRY_SEL_OFST
		, A60810_RG_SSUSB_P3_ENTRY_SEL
		, 0x01);

	/* BANK 0x40 */
	/* fine tune SSC delta1 to let SSC min average ~0ppm */
	u3_phy_write_field32(((u32)
		&info->u3phya_da_regs_a->reg19)
		, A60810_RG_SSUSB_PLL_SSC_DELTA1_U3_OFST
		, A60810_RG_SSUSB_PLL_SSC_DELTA1_U3
		, 0x46);
	/* U3PhyWriteField32(((u32)&info.u3phya_da_regs_a->reg19) */
	u3_phy_write_field32(((u32)
		&info->u3phya_da_regs_a->reg21)
		, A60810_RG_SSUSB_PLL_SSC_DELTA1_PE1H_OFST
		, A60810_RG_SSUSB_PLL_SSC_DELTA1_PE1H
		, 0x40);

	/* fine tune SSC delta to let SSC min average ~0ppm */

	/* Fine tune SYSPLL to improve phase noise */
	/* I2C  60    0x08[01:00]	0x03
	 * RW  RG_SSUSB_PLL_BC_U3
	 */
	u3_phy_write_field32(((u32)
		&info->u3phya_da_regs_a->reg4)
		, A60810_RG_SSUSB_PLL_BC_U3_OFST
		, A60810_RG_SSUSB_PLL_BC_U3
		, 0x3);
	/* I2C  60    0x08[12:10]	0x03
	 * RW  RG_SSUSB_PLL_DIVEN_U3
	 */
	u3_phy_write_field32(((u32)
		&info->u3phya_da_regs_a->reg4)
		, A60810_RG_SSUSB_PLL_DIVEN_U3_OFST
		, A60810_RG_SSUSB_PLL_DIVEN_U3
		, 0x3);
	/* I2C  60    0x0C[03:00]	0x01   RW  RG_SSUSB_PLL_IC_U3 */
	u3_phy_write_field32(((u32)
		&info->u3phya_da_regs_a->reg5)
		, A60810_RG_SSUSB_PLL_IC_U3_OFST
		, A60810_RG_SSUSB_PLL_IC_U3
		, 0x1);
	/* I2C  60    0x0C[23:22]	0x01   RW  RG_SSUSB_PLL_BR_U3 */
	u3_phy_write_field32(((u32)
		&info->u3phya_da_regs_a->reg5)
		, A60810_RG_SSUSB_PLL_BR_U3_OFST
		, A60810_RG_SSUSB_PLL_BR_U3
		, 0x1);
	/* I2C  60    0x10[03:00]	0x01
	 * RW  RG_SSUSB_PLL_IR_U3
	 */
	u3_phy_write_field32(((u32)
		&info->u3phya_da_regs_a->reg6)
		, A60810_RG_SSUSB_PLL_IR_U3_OFST
		, A60810_RG_SSUSB_PLL_IR_U3
		, 0x1);
	/* I2C  60    0x14[03:00]	0x0F   RW  RG_SSUSB_PLL_BP_U3 */
	u3_phy_write_field32(((u32)
		&info->u3phya_da_regs_a->reg7)
		, A60810_RG_SSUSB_PLL_BP_U3_OFST
		, A60810_RG_SSUSB_PLL_BP_U3
		, 0x0f);

	/* BANK 0x60 */
	/* force xtal pwd mode enable */
	u3_phy_write_field32(((u32)
		&info->spllc_regs_a->u3d_xtalctl_2)
		, A60810_RG_SSUSB_FORCE_XTAL_PWD_OFST
		, A60810_RG_SSUSB_FORCE_XTAL_PWD
		, 0x1);
	/* force bias pwd mode enable */
	u3_phy_write_field32(((u32)
		&info->spllc_regs_a->u3d_xtalctl_2)
		, A60810_RG_SSUSB_FORCE_BIAS_PWD_OFST
		, A60810_RG_SSUSB_FORCE_BIAS_PWD
		, 0x1);
	/* force xtal pwd mode off to work around xtal drv de */
	u3_phy_write_field32(((u32)
		&info->spllc_regs_a->u3d_xtalctl_2)
		, A60810_RG_SSUSB_XTAL_PWD_OFST
		, A60810_RG_SSUSB_XTAL_PWD
		, 0x0);
	/* force bias pwd mode off to work around xtal drv de */
	u3_phy_write_field32(((u32)
		&info->spllc_regs_a->u3d_xtalctl_2)
		, A60810_RG_SSUSB_BIAS_PWD_OFST
		, A60810_RG_SSUSB_BIAS_PWD
		, 0x0);

	/********* test chip settings ***********/
	/* BANK 0x00 */
	/* slew rate setting */
	u3_phy_write_field32(((u32)
		&info->u2phy_regs_a->usbphyacr5)
		, A60810_RG_USB20_HSTX_SRCTRL_OFST
		, A60810_RG_USB20_HSTX_SRCTRL
		, 0x4);

	/* BANK 0x50 */

	/* PIPE setting  BANK5 */
	/* PIPE drv = 2 */
	u3_phy_write_reg8(((u32)
			&info->sifslv_chip_regs_a->gpio_ctla) + 2, 0x10);
	/* PIPE phase */
	/* U3PhyWriteReg8(((u32)&info.sifslv_chip_regs_a->gpio_ctla)+3, */
	/* 0xdc); */
	u3_phy_write_reg8(((u32)
			&info->sifslv_chip_regs_a->gpio_ctla) + 3, 0x24);

	return 0;
}

static int usb_i2c_probe(struct i2c_client *client,
						const struct i2c_device_id *id)
{
	void __iomem *base;
	u32 val = 0;
	/* if i2c probe before musb prob, this would cause KE */
	/* base = (unsigned long)((unsigned long)mtk_musb->xceiv->io_priv); */
	base = usb_phy_base + USB_PHY_OFFSET;
	DBG(0, "[MUSB]%s, start, base:%p\n", __func__, base);

	usb_i2c_client = client;

	/* disable usb mac suspend */
	val = musb_readl(base, 0x68);
	/* DBG(0, "[MUSB]0x68=0x%x\n", val); */

	musb_writel(base, 0x68, (val & ~(0x4 << 16)));

	/* DBG(0, "[MUSB]0x68=0x%x\n"                    */
	/*		"[MUSB]addr: 0xFF, value: %x\n", */
	/*		musb_readl(base, 0x68),          */
	/*		USB_PHY_Read_Register8(0xFF));   */

	USB_PHY_Write_Register8(0x20, 0xFF);

	DBG(0, "[MUSB]version=[%02x %02x %02x %02x]\n",
		USB_PHY_Read_Register8(0xE4),
		USB_PHY_Read_Register8(0xE5),
		USB_PHY_Read_Register8(0xE6),
		USB_PHY_Read_Register8(0xE7));

	if (USB_PHY_Read_Register8(0xE7) == 0xa) {
		static struct u3phy_info info;

		DBG(0, "[MUSB] Phy version is %x\n",
					u3_phy_read_reg32(0x2000e4));

		info.u2phy_regs_a = (struct u2phy_reg_a *)0x0;
		info.u3phyd_regs_a = (struct u3phyd_reg_a *)0x100000;
		info.u3phyd_bank2_regs_a =
					(struct u3phyd_bank2_reg_a *)0x200000;
		info.u3phya_regs_a = (struct u3phya_reg_a *)0x300000;
		info.u3phya_da_regs_a = (struct u3phya_da_reg_a *)0x400000;
		info.sifslv_chip_regs_a = (struct sifslv_chip_reg_a *)0x500000;
		info.spllc_regs_a = (struct spllc_reg_a *)0x600000;
		info.sifslv_fm_regs_a = (struct sifslv_fm_reg_a *)0xf00000;

		if (u3_phy_read_reg32(0x2000e4) == 0xa60810a) {
			/* DBG(0, "[MUSB] PHY A60810 init\n"); */
			phy_init_a60810(&info);
		} else if (u3_phy_read_reg32(0x2000e4) == 0xa60931a) {
			/* DBG(0, "[MUSB] PHY A60931 init\n"); */
			phy_init_a60931(&info);
		}

	} else {
		USB_PHY_Write_Register8(0x00, 0xFF);

		DBG(0, "[MUSB]addr: 0xFF, value: %x\n",
				USB_PHY_Read_Register8(0xFF));

		/* usb phy initial sequence */
		USB_PHY_Write_Register8(0x00, 0xFF);
		USB_PHY_Write_Register8(0x04, 0x61);
		USB_PHY_Write_Register8(0x00, 0x68);
		USB_PHY_Write_Register8(0x00, 0x6a);
		USB_PHY_Write_Register8(0x6e, 0x00);
		USB_PHY_Write_Register8(0x0c, 0x1b);
		USB_PHY_Write_Register8(0x44, 0x08);
		USB_PHY_Write_Register8(0x55, 0x11);
		USB_PHY_Write_Register8(0x68, 0x1a);


		DBG(0, "[MUSB]addr: 0xFF, value: %x\n"
				"[MUSB]addr: 0x61, value: %x\n"
				"[MUSB]addr: 0x68, value: %x\n"
				"[MUSB]addr: 0x6a, value: %x\n"
				"[MUSB]addr: 0x00, value: %x\n"
				"[MUSB]addr: 0x1b, value: %x\n"
				"[MUSB]addr: 0x08, value: %x\n"
				"[MUSB]addr: 0x11, value: %x\n"
				"[MUSB]addr: 0x1a, value: %x\n",
				USB_PHY_Read_Register8(0xFF),
				USB_PHY_Read_Register8(0x61),
				USB_PHY_Read_Register8(0x68),
				USB_PHY_Read_Register8(0x6a),
				USB_PHY_Read_Register8(0x00),
				USB_PHY_Read_Register8(0x1b),
				USB_PHY_Read_Register8(0x08),
				USB_PHY_Read_Register8(0x11),
				USB_PHY_Read_Register8(0x1a));
	}

	DBG(0, "[MUSB]%s, end\n", __func__);
	return 0;

}

static int usb_i2c_remove(struct i2c_client *client)
{
	return 0;
}

static const struct of_device_id usb_of_match[] = {
	{.compatible = "mediatek,mtk-usb"},
	{},
};

struct i2c_driver usb_i2c_driver = {
	.probe = usb_i2c_probe,
	.remove = usb_i2c_remove,
	.driver = {
		.name = "mtk-usb",
		.of_match_table = usb_of_match,
	},
	.id_table = usb_i2c_id,
};

static int add_usb_i2c_driver(void)
{
	DBG(0, "%s\n", __func__);

	if (i2c_add_driver(&usb_i2c_driver) != 0) {
		DBG(0, "[MUSB]usb_i2c_driver initialization failed!!\n");
		return -1;
	}
	DBG(0, "[MUSB]usb_i2c_driver initialization succeed!!\n");
	return 0;
}
#endif				/* End of FPGA_PLATFORM */

static int __init mt_usb_init(struct musb *musb)
{
	int ret;

	DBG(1, "%s\n", __func__);

	usb_phy_generic_register();
	musb->xceiv = usb_get_phy(USB_PHY_TYPE_USB2);

	if (IS_ERR_OR_NULL(musb->xceiv)) {
		DBG(0, "[MUSB] usb_get_phy error!!\n");
		return -EPROBE_DEFER;
	}

	musb->dma_irq = (int)SHARE_IRQ;
	musb->fifo_cfg = fifo_cfg;
	musb->fifo_cfg_size = ARRAY_SIZE(fifo_cfg);
	musb->dyn_fifo = true;
	musb->power = false;
	musb->is_host = false;
	musb->fifo_size = 8 * 1024;
#ifndef FPGA_PLATFORM
	musb->usb_rev6_setting = usb_rev6_setting;
#endif

	musb->usb_lock = wakeup_source_register(NULL, "USB suspend lock");

#ifndef FPGA_PLATFORM
	reg_vusb = regulator_get(musb->controller, "vusb");
	if (!IS_ERR(reg_vusb)) {
#ifdef NEVER
#define	VUSB33_VOL_MIN 3070000
#define	VUSB33_VOL_MAX 3070000
		ret = regulator_set_voltage(reg_vusb,
					VUSB33_VOL_MIN, VUSB33_VOL_MAX);
		if (ret < 0)
			pr_err("regulator set vol failed: %d\n", ret);
		else
			DBG(0, "regulator set vol ok, <%d,%d>\n",
					VUSB33_VOL_MIN, VUSB33_VOL_MAX);
#endif /* NEVER */
		ret = regulator_enable(reg_vusb);
		if (ret < 0) {
			pr_err("regulator_enable vusb failed: %d\n", ret);
			regulator_put(reg_vusb);
		}
	} else
		pr_err("regulator_get vusb failed\n");

	reg_vio18 = regulator_get(musb->controller, "vio18");
	if (!IS_ERR(reg_vio18)) {
		ret = regulator_enable(reg_vio18);
		if (ret < 0) {
			pr_err("regulator_enable vio18 failed: %d\n", ret);
			regulator_put(reg_vio18);
		}
	} else
		pr_err("regulator_get vio18 failed\n");

	reg_va12 = regulator_get(musb->controller, "va12");
	if (!IS_ERR(reg_va12)) {
		ret = regulator_enable(reg_va12);
		if (ret < 0) {
			pr_err("regulator_enable va12 failed: %d\n", ret);
			regulator_put(reg_va12);
		}
	} else
		pr_err("regulator_get va12 failed\n");

#endif

	ret = device_create_file(musb->controller, &dev_attr_cmode);

	/* mt_usb_enable(musb); */

	musb->isr = mt_usb_interrupt;
	musb_writel(musb->mregs,
			MUSB_HSDMA_INTR, 0xff |
			(0xff << DMA_INTR_UNMASK_SET_OFFSET));
	DBG(1, "musb platform init %x\n",
			musb_readl(musb->mregs, MUSB_HSDMA_INTR));

#ifdef CONFIG_MTK_MUSB_QMU_SUPPORT
	/* FIXME, workaround for device_qmu + host_dma */
	musb_writel(musb->mregs,
			USB_L1INTM,
		    TX_INT_STATUS |
		    RX_INT_STATUS |
		    USBCOM_INT_STATUS |
		    DMA_INT_STATUS |
		    QINT_STATUS);
#else
	musb_writel(musb->mregs,
			USB_L1INTM,
		    TX_INT_STATUS |
		    RX_INT_STATUS |
		    USBCOM_INT_STATUS |
		    DMA_INT_STATUS);
#endif

	setup_timer(&musb_idle_timer, musb_do_idle, (unsigned long)musb);

#ifdef CONFIG_USB_MTK_OTG
	mt_usb_otg_init(musb);
	/* enable host suspend mode */
	mt_usb_wakeup_init(musb);
	musb->host_suspend = true;
#endif
	return 0;
}

static int mt_usb_exit(struct musb *musb)
{
	del_timer_sync(&musb_idle_timer);
#ifndef FPGA_PLATFORM
	if (reg_vusb)
		regulator_put(reg_vusb);
	if (reg_vio18)
		regulator_put(reg_vio18);
	if (reg_va12)
		regulator_put(reg_va12);
#endif
#ifdef CONFIG_USB_MTK_OTG
	mt_usb_otg_exit(musb);
#endif
	return 0;
}

static void mt_usb_enable_clk(struct musb *musb)
{
	usb_enable_clock(true);
}

static void mt_usb_disable_clk(struct musb *musb)
{
	usb_enable_clock(false);
}

static void mt_usb_prepare_clk(struct musb *musb)
{
	usb_prepare_clock(true);
}

static void mt_usb_unprepare_clk(struct musb *musb)
{
	usb_prepare_clock(false);
}

static const struct musb_platform_ops mt_usb_ops = {
	.init = mt_usb_init,
	.exit = mt_usb_exit,
	/*.set_mode     = mt_usb_set_mode, */
	.try_idle = mt_usb_try_idle,
	.enable = mt_usb_enable,
	.disable = mt_usb_disable,
	/* .set_vbus = mt_usb_set_vbus, */
	.vbus_status = mt_usb_get_vbus_status,
	.enable_clk =  mt_usb_enable_clk,
	.disable_clk =  mt_usb_disable_clk,
	.prepare_clk = mt_usb_prepare_clk,
	.unprepare_clk = mt_usb_unprepare_clk,
#ifdef CONFIG_USB_MTK_OTG
	.enable_wakeup = mt_usb_wakeup,
#endif
};

#ifdef CONFIG_MTK_MUSB_DRV_36BIT
static u64 mt_usb_dmamask = DMA_BIT_MASK(36);
#else
static u64 mt_usb_dmamask = DMA_BIT_MASK(32);
#endif

static int mt_usb_probe(struct platform_device *pdev)
{
	struct musb_hdrc_platform_data *pdata = pdev->dev.platform_data;
	struct platform_device *musb;
	struct mt_usb_glue *glue;
	struct musb_hdrc_config *config;
	struct device_node *np = pdev->dev.of_node;
#ifdef CONFIG_MTK_UART_USB_SWITCH
	struct device_node *ap_gpio_node = NULL;
#endif
	int ret = -ENOMEM;

	glue = kzalloc(sizeof(*glue), GFP_KERNEL);
	if (!glue) {
		/* dev_err(&pdev->dev, "failed to allocate glue context\n"); */
		goto err0;
	}

	musb = platform_device_alloc("musb-hdrc", PLATFORM_DEVID_NONE);
	if (!musb) {
		dev_err(&pdev->dev, "failed to allocate musb device\n");
		goto err1;
	}

	usb_phy_base = of_iomap(np, 1);
	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(&pdev->dev, "failed to allocate musb platform data\n");
		goto err2;
	}

	config = devm_kzalloc(&pdev->dev, sizeof(*config), GFP_KERNEL);
	if (!config) {
		/* dev_notice(&pdev->dev,
		 * "failed to allocate musb hdrc config\n");
		 */
		goto err2;
	}
#ifdef CONFIG_USB_MTK_OTG
	pdata->mode = MUSB_OTG;
#else
	of_property_read_u32(np, "mode", (u32 *) &pdata->mode);
#endif

#ifdef CONFIG_MTK_UART_USB_SWITCH
	ap_gpio_node =
		of_find_compatible_node(NULL, NULL, AP_GPIO_COMPATIBLE_NAME);

	if (ap_gpio_node == NULL) {
		dev_err(&pdev->dev, "USB get ap_gpio_node failed\n");
		if (ap_gpio_base)
			iounmap(ap_gpio_base);
		ap_gpio_base = 0;
	} else {
		ap_gpio_base = of_iomap(ap_gpio_node, 0);
		ap_gpio_base += RG_GPIO_SELECT;
	}
#endif

	of_property_read_u32(np, "num_eps", (u32 *) &config->num_eps);
	config->multipoint = of_property_read_bool(np, "multipoint");

	pdata->config = config;

	musb->dev.parent = &pdev->dev;
	musb->dev.dma_mask = &mt_usb_dmamask;
	musb->dev.coherent_dma_mask = mt_usb_dmamask;

	pdev->dev.dma_mask = &mt_usb_dmamask;
	pdev->dev.coherent_dma_mask = mt_usb_dmamask;
	arch_setup_dma_ops(&musb->dev, 0, mt_usb_dmamask, NULL, 0);

	glue->dev = &pdev->dev;
	glue->musb = musb;

	pdata->platform_ops = &mt_usb_ops;

	/*
	 * Don't use the name from dtsi, like "11200000.usb0".
	 * So modify the device name. And rc can use the same path for
	 * all platform, like "/sys/devices/platform/mt_usb/".
	 */
	ret = device_rename(&pdev->dev, "mt_usb");
	if (ret)
		dev_notice(&pdev->dev, "failed to rename\n");

	/*
	 * fix uaf(use afer free) issue:backup pdev->name,
	 * device_rename will free pdev->name
	 */
	pdev->name = pdev->dev.kobj.name;

	platform_set_drvdata(pdev, glue);

	ret = platform_device_add_resources(musb,
				pdev->resource, pdev->num_resources);
	if (ret) {
		dev_err(&pdev->dev, "failed to add resources\n");
		goto err2;
	}

	ret = platform_device_add_data(musb, pdata, sizeof(*pdata));
	if (ret) {
		dev_err(&pdev->dev, "failed to add platform_data\n");
		goto err2;
	}

	ret = platform_device_add(musb);

	if (ret) {
		dev_err(&pdev->dev, "failed to register musb device\n");
		goto err2;
	}

#ifdef CONFIG_MTK_MUSB_QMU_SUPPORT
	isoc_ep_end_idx = 1;
	isoc_ep_gpd_count = 248; /* 30 ms for HS, at most (30*8 + 1) */

	mtk_host_qmu_force_isoc_restart = 0;
#endif
#ifndef FPGA_PLATFORM
	register_usb_hal_dpidle_request(usb_dpidle_request);
#endif
#ifdef BYPASS_PMIC_LINKAGE
	register_usb_hal_disconnect_check(trigger_disconnect_check_work);
#endif
	INIT_DELAYED_WORK(&idle_work, do_idle_work);

	DBG(0, "keep musb->power & mtk_usb_power in the samae value\n");
	mtk_usb_power = false;

#ifndef FPGA_PLATFORM
	musb_clk = devm_clk_get(&pdev->dev, "usb0");
	if (IS_ERR(musb_clk)) {
		DBG(0, "cannot get musb_clk clock\n");
		goto err2;
	}


	musb_clk_top_sel = devm_clk_get(&pdev->dev, "usb0_clk_top_sel");
	if (IS_ERR(musb_clk_top_sel)) {
		DBG(0, "cannot get musb_clk_top_sel clock\n");
		goto err2;
	}

	musb_clk_univpll5_d2 = devm_clk_get(&pdev->dev, "usb0_clk_univpll5_d2");
	if (IS_ERR(musb_clk_univpll5_d2)) {
		DBG(0, "cannot get musb_clk_univpll5_d2 clock\n");
		goto err2;
	}

#ifdef CONFIG_DEBUG_FS
	if (usb20_phy_init_debugfs()) {
		DBG(0, "usb20_phy_init_debugfs fail!\n");
		goto err2;
	}
#endif

	if (init_sysfs(&pdev->dev)) {
		DBG(0, "failed to init_sysfs\n");
		goto err2;
	}
#endif
	DBG(0, "USB probe done!\n");

#if defined(FPGA_PLATFORM) || defined(FOR_BRING_UP)
	musb_force_on = 1;
#endif

#ifndef FPGA_PLATFORM
	if (get_boot_mode() == META_BOOT) {
		DBG(0, "in special mode %d\n", get_boot_mode());
		musb_force_on = 1;
	}
#endif
	return 0;

err2:
	platform_device_put(musb);
err1:
	kfree(glue);
err0:
	return ret;
}

static int mt_usb_remove(struct platform_device *pdev)
{
	struct mt_usb_glue *glue = platform_get_drvdata(pdev);

	platform_device_unregister(glue->musb);
	kfree(glue);

	return 0;
}

static struct platform_driver mt_usb_driver = {
	.remove = mt_usb_remove,
	.probe = mt_usb_probe,
	.driver = {
		.name = "mt_usb",
		.of_match_table = apusb_of_ids,
	},
};

static int __init usb20_init(void)
{
	int ret;

	DBG(0, "usb20 init\n");

#ifdef CONFIG_MTK_USB2JTAG_SUPPORT
	if (usb2jtag_mode()) {
		pr_notice("[USB2JTAG] in usb2jtag mode, not to initialize usb driver\n");
		return 0;
	}
#endif

	ret = platform_driver_register(&mt_usb_driver);

#ifdef FPGA_PLATFORM
	add_usb_i2c_driver();
#endif

	DBG(0, "usb20 init ret:%d\n", ret);
	return ret;
}
fs_initcall(usb20_init);

static void __exit usb20_exit(void)
{
	platform_driver_unregister(&mt_usb_driver);
}
module_exit(usb20_exit);
