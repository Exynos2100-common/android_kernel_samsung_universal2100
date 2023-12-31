// SPDX-License-Identifier: GPL-2.0
/*
 * xhci-plat.c - xHCI host controller driver platform Bus Glue.
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com
 * Author: Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 *
 * A lot of code borrowed from the Linux xHCI driver.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/usb/phy.h>
#include <linux/slab.h>
#include <linux/phy/phy.h>
#include <linux/acpi.h>
#include <linux/usb/of.h>
#ifdef CONFIG_SND_EXYNOS_USB_AUDIO
#include <linux/usb/exynos_usb_audio.h>
#endif

#include "../core/phy.h"
#include "xhci.h"
#include "xhci-plat.h"
#include "xhci-mvebu.h"
#include "xhci-rcar.h"

#define PORTSC_OFFSET		0x430

static struct hc_driver __read_mostly xhci_plat_hc_driver;

static int xhci_plat_setup(struct usb_hcd *hcd);
static int xhci_plat_start(struct usb_hcd *hcd);
static int is_rewa_enabled;
#if defined(CONFIG_USB_PORT_POWER_OPTIMIZATION)
extern void __iomem *usb3_portsc;
extern u32 pp_set_delayed;
extern int port_off_done;
extern u32 portsc_control_priority;
extern spinlock_t xhcioff_lock;
extern void xhci_portsc_power_off(void __iomem *portsc, u32 on, u32 prt);
#endif

static const struct xhci_driver_overrides xhci_plat_overrides __initconst = {
	.extra_priv_size = sizeof(struct xhci_plat_priv),
	.reset = xhci_plat_setup,
	.start = xhci_plat_start,
};

static void xhci_priv_plat_start(struct usb_hcd *hcd)
{
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);

	if (priv->plat_start)
		priv->plat_start(hcd);
}

static int xhci_priv_plat_setup(struct usb_hcd *hcd)
{
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);

	if (!priv->plat_setup)
		return 0;

	return priv->plat_setup(hcd);
}

static int xhci_priv_init_quirk(struct usb_hcd *hcd)
{
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);

	if (!priv->init_quirk)
		return 0;

	return priv->init_quirk(hcd);
}

static int xhci_priv_resume_quirk(struct usb_hcd *hcd)
{
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);

	if (!priv->resume_quirk)
		return 0;

	return priv->resume_quirk(hcd);
}

static void xhci_plat_quirks(struct device *dev, struct xhci_hcd *xhci)
{
	struct xhci_plat_priv *priv = xhci_to_priv(xhci);

	/*
	 * As of now platform drivers don't provide MSI support so we ensure
	 * here that the generic code does not try to make a pci_dev from our
	 * dev struct in order to setup MSI
	 */
	xhci->quirks |= XHCI_PLAT | priv->quirks;
}

/* called during probe() after chip reset completes */
static int xhci_plat_setup(struct usb_hcd *hcd)
{
	int ret;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);

	ret = xhci_priv_init_quirk(hcd);
	if (ret)
		return ret;

	ret = xhci_gen_setup(hcd, xhci_plat_quirks);

	/*
	 * DWC3 WORKAROUND: xhci reset clears PHY CR port settings,
	 * so USB3.0 PHY should be tuned again.
	 */
	if (hcd == xhci->main_hcd) {
	if (xhci->phy_usb2)
		exynos_usbdrd_phy_tune(xhci->phy_usb2, OTG_STATE_A_HOST);
	} else {
		if (xhci->phy_usb3)
			exynos_usbdrd_phy_tune(xhci->phy_usb3, OTG_STATE_A_HOST);
	}

	return ret;
}

static int xhci_plat_start(struct usb_hcd *hcd)
{
	xhci_priv_plat_start(hcd);
	return xhci_run(hcd);
}

static ssize_t
xhci_plat_show_ss_compliance(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	u32			reg;
	void __iomem *reg_base;

	reg_base = hcd->regs;
	reg = readl(reg_base + PORTSC_OFFSET);

	return snprintf(buf, PAGE_SIZE, "0x%x\n", reg);
}

static ssize_t
xhci_platg_store_ss_compliance(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	int		value;
	u32			reg;
	void __iomem *reg_base;

	if (sscanf(buf, "%d", &value) != 1)
		return -EINVAL;

	reg_base = hcd->regs;

	if (value == 1) {
		/* PORTSC PLS is set to 10, LWS to 1 */
		reg = readl(reg_base + PORTSC_OFFSET);
		reg &= ~((0xF << 5) | (1 << 16));
		reg |= (10 << 5) | (1 << 16);
		writel(reg, reg_base + PORTSC_OFFSET);
		pr_info("SS host compliance enabled portsc 0x%x\n", reg);
	} else
		pr_info("Only 1 is allowed for input value\n");

	return n;
}

static DEVICE_ATTR(ss_compliance, 0640,
	xhci_plat_show_ss_compliance, xhci_platg_store_ss_compliance);

static struct attribute *exynos_xhci_attributes[] = {
	&dev_attr_ss_compliance.attr,
	NULL
};

static const struct attribute_group xhci_plat_attr_group = {
	.attrs = exynos_xhci_attributes,
};
#ifdef CONFIG_OF
static const struct xhci_plat_priv xhci_plat_marvell_armada = {
	.init_quirk = xhci_mvebu_mbus_init_quirk,
};

static const struct xhci_plat_priv xhci_plat_marvell_armada3700 = {
	.plat_setup = xhci_mvebu_a3700_plat_setup,
	.init_quirk = xhci_mvebu_a3700_init_quirk,
};

static const struct xhci_plat_priv xhci_plat_renesas_rcar_gen2 = {
	SET_XHCI_PLAT_PRIV_FOR_RCAR(XHCI_RCAR_FIRMWARE_NAME_V1)
};

static const struct xhci_plat_priv xhci_plat_renesas_rcar_gen3 = {
	SET_XHCI_PLAT_PRIV_FOR_RCAR(XHCI_RCAR_FIRMWARE_NAME_V3)
};

static const struct of_device_id usb_xhci_of_match[] = {
	{
		.compatible = "generic-xhci",
	}, {
		.compatible = "xhci-platform",
	}, {
		.compatible = "marvell,armada-375-xhci",
		.data = &xhci_plat_marvell_armada,
	}, {
		.compatible = "marvell,armada-380-xhci",
		.data = &xhci_plat_marvell_armada,
	}, {
		.compatible = "marvell,armada3700-xhci",
		.data = &xhci_plat_marvell_armada3700,
	}, {
		.compatible = "renesas,xhci-r8a7790",
		.data = &xhci_plat_renesas_rcar_gen2,
	}, {
		.compatible = "renesas,xhci-r8a7791",
		.data = &xhci_plat_renesas_rcar_gen2,
	}, {
		.compatible = "renesas,xhci-r8a7793",
		.data = &xhci_plat_renesas_rcar_gen2,
	}, {
		.compatible = "renesas,xhci-r8a7795",
		.data = &xhci_plat_renesas_rcar_gen3,
	}, {
		.compatible = "renesas,xhci-r8a7796",
		.data = &xhci_plat_renesas_rcar_gen3,
	}, {
		.compatible = "renesas,rcar-gen2-xhci",
		.data = &xhci_plat_renesas_rcar_gen2,
	}, {
		.compatible = "renesas,rcar-gen3-xhci",
		.data = &xhci_plat_renesas_rcar_gen3,
	},
	{},
};
MODULE_DEVICE_TABLE(of, usb_xhci_of_match);
#endif

static void xhci_pm_runtime_init(struct device *dev)
{
    dev->power.runtime_status = RPM_SUSPENDED;
    dev->power.idle_notification = false;

    dev->power.disable_depth = 1;
    atomic_set(&dev->power.usage_count, 0);

    dev->power.runtime_error = 0;

    atomic_set(&dev->power.child_count, 0);
    pm_suspend_ignore_children(dev, false);
    dev->power.runtime_auto = true;

    dev->power.request_pending = false;
    dev->power.request = RPM_REQ_NONE;
    dev->power.deferred_resume = false;
    dev->power.accounting_timestamp = jiffies;

    dev->power.timer_expires = 0;
    init_waitqueue_head(&dev->power.wait_queue);
}

static int xhci_plat_probe(struct platform_device *pdev)
{
	struct device		*parent = pdev->dev.parent;
	const struct xhci_plat_priv *priv_match;
	const struct hc_driver	*driver;
	struct device		*sysdev, *tmpdev;
	struct xhci_hcd		*xhci;
	struct resource         *res;
	struct usb_hcd		*hcd;
	int			ret;
	int			irq;
	struct xhci_plat_priv	*priv = NULL;


	struct wakeup_source	*main_wakelock, *shared_wakelock;
	int			value;

	dev_info(&pdev->dev, "XHCI PLAT START\n");

	main_wakelock = wakeup_source_register(&pdev->dev, dev_name(&pdev->dev));
	__pm_stay_awake(main_wakelock);

	/* Initialization shared wakelock for SS HCD */
	shared_wakelock = wakeup_source_register(&pdev->dev, dev_name(&pdev->dev));
	__pm_stay_awake(shared_wakelock);

#if defined(CONFIG_USB_PORT_POWER_OPTIMIZATION)
	port_off_done = 0;
	portsc_control_priority = 0;
#endif
	is_rewa_enabled = 0;

	if (usb_disabled())
		return -ENODEV;

	driver = &xhci_plat_hc_driver;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	/*
	 * sysdev must point to a device that is known to the system firmware
	 * or PCI hardware. We handle these three cases here:
	 * 1. xhci_plat comes from firmware
	 * 2. xhci_plat is child of a device from firmware (dwc3-plat)
	 * 3. xhci_plat is grandchild of a pci device (dwc3-pci)
	 */
	for (sysdev = &pdev->dev; sysdev; sysdev = sysdev->parent) {
		if (is_of_node(sysdev->fwnode) ||
			is_acpi_device_node(sysdev->fwnode))
			break;
#ifdef CONFIG_PCI
		else if (sysdev->bus == &pci_bus_type)
			break;
#endif
	}

	if (!sysdev)
		sysdev = &pdev->dev;

	/* Try to set 64-bit DMA first */
	if (WARN_ON(!sysdev->dma_mask))
		/* Platform did not initialize dma_mask */
		ret = dma_coerce_mask_and_coherent(sysdev,
						   DMA_BIT_MASK(64));
	else
		ret = dma_set_mask_and_coherent(sysdev, DMA_BIT_MASK(64));

	/* If seting 64-bit DMA mask fails, fall back to 32-bit DMA mask */
	if (ret) {
		ret = dma_set_mask_and_coherent(sysdev, DMA_BIT_MASK(32));
		if (ret)
			return ret;
	}

	xhci_pm_runtime_init(&pdev->dev);

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);

	hcd = __usb_create_hcd(driver, sysdev, &pdev->dev,
			       dev_name(&pdev->dev), NULL);
	if (!hcd) {
		ret = -ENOMEM;
		goto disable_runtime;
	}
	hcd->skip_phy_initialization = 1;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	hcd->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(hcd->regs)) {
		ret = PTR_ERR(hcd->regs);
		goto put_hcd;
	}

	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);

	usb3_portsc = hcd->regs + PORTSC_OFFSET;
	if (pp_set_delayed) {
		pr_info("port power set delayed\n");
		xhci_portsc_power_off(usb3_portsc, 0, 2);
		pp_set_delayed = 0;
	}

	xhci = hcd_to_xhci(hcd);

	/*
	 * Not all platforms have clks so it is not an error if the
	 * clock do not exist.
	 */
	xhci->reg_clk = devm_clk_get_optional(&pdev->dev, "reg");
	if (IS_ERR(xhci->reg_clk)) {
		ret = PTR_ERR(xhci->reg_clk);
		goto put_hcd;
	}

	ret = clk_prepare_enable(xhci->reg_clk);
	if (ret)
		goto put_hcd;

	xhci->clk = devm_clk_get_optional(&pdev->dev, NULL);
	if (IS_ERR(xhci->clk)) {
		ret = PTR_ERR(xhci->clk);
		goto disable_reg_clk;
	}

	ret = clk_prepare_enable(xhci->clk);
	if (ret)
		goto disable_reg_clk;

	priv_match = of_device_get_match_data(&pdev->dev);
	if (priv_match) {
		priv = hcd_to_xhci_priv(hcd);
		/* Just copy data for now */
		if (priv_match)
			*priv = *priv_match;
	}

	device_set_wakeup_capable(&pdev->dev, true);

	xhci->main_wakelock = main_wakelock;
	xhci->shared_wakelock = shared_wakelock;
	xhci->main_hcd = hcd;
	xhci->shared_hcd = __usb_create_hcd(driver, sysdev, &pdev->dev,
			dev_name(&pdev->dev), hcd);
	if (!xhci->shared_hcd) {
		ret = -ENOMEM;
		goto disable_clk;
	}
	xhci->shared_hcd->skip_phy_initialization = 1;

	/* imod_interval is the interrupt moderation value in nanoseconds. */
	xhci->imod_interval = 40000;

	/* Iterate over all parent nodes for finding quirks */
	for (tmpdev = &pdev->dev; tmpdev; tmpdev = tmpdev->parent) {

		if (device_property_read_bool(tmpdev, "usb2-lpm-disable"))
			xhci->quirks |= XHCI_HW_LPM_DISABLE;

		if (device_property_read_bool(tmpdev, "usb3-lpm-capable"))
			xhci->quirks |= XHCI_LPM_SUPPORT;

		if (device_property_read_bool(tmpdev, "quirk-broken-port-ped"))
			xhci->quirks |= XHCI_BROKEN_PORT_PED;

		device_property_read_u32(tmpdev, "imod-interval-ns",
					 &xhci->imod_interval);
	}

	hcd->usb_phy = devm_usb_get_phy_by_phandle(sysdev, "usb-phy", 0);
	if (IS_ERR(hcd->usb_phy)) {
		ret = PTR_ERR(hcd->usb_phy);
		if (ret == -EPROBE_DEFER)
			goto put_usb3_hcd;
		hcd->usb_phy = NULL;
	} else {
		ret = usb_phy_init(hcd->usb_phy);
		if (ret)
			goto put_usb3_hcd;
	}

	/* Get USB2.0 PHY for main hcd */
	if (parent) {
		xhci->phy_usb2 = devm_phy_get(parent, "usb2-phy");
		if (IS_ERR_OR_NULL(xhci->phy_usb2)) {
			xhci->phy_usb2 = NULL;
			dev_err(&pdev->dev,
				"%s: failed to get phy\n", __func__);
		}
	}

	/* Get USB3.0 PHY to tune the PHY */
	if (parent) {
		xhci->phy_usb3 =
				devm_phy_get(parent, "usb3-phy");
		if (IS_ERR_OR_NULL(xhci->phy_usb3)) {
			xhci->phy_usb3 = NULL;
			dev_err(&pdev->dev,
				"%s: failed to get phy\n", __func__);
		}
	}

	ret = of_property_read_u32(parent->of_node, "xhci_l2_support", &value);
	if (ret == 0 && value == 1)
		xhci->quirks |= XHCI_L2_SUPPORT;
	else {
		dev_err(&pdev->dev,
			"can't get xhci l2 support, error = %d\n", ret);
	}

#ifdef CONFIG_SND_EXYNOS_USB_AUDIO
	ret = of_property_read_u32(parent->of_node,
				"xhci_use_uram_for_audio", &value);
	if (ret == 0 && value == 1) {
		/*
		 * Check URAM address. At least the following address should
		 * be defined.(Otherwise, URAM feature will be disabled.)
		 */
		if (EXYNOS_URAM_DCBAA_ADDR == 0x0 ||
				EXYNOS_URAM_ABOX_ERST_SEG_ADDR == 0x0 ||
				EXYNOS_URAM_ABOX_EVT_RING_ADDR == 0x0 ||
				EXYNOS_URAM_DEVICE_CTX_ADDR == 0x0 ||
				EXYNOS_URAM_ISOC_OUT_RING_ADDR == 0x0) {
			dev_info(&pdev->dev,
				"Some URAM addresses are not defiend!\n");
			goto skip_uram;
		}

		dev_info(&pdev->dev, "Support URAM for USB audio.\n");
		xhci->quirks |= XHCI_USE_URAM_FOR_EXYNOS_AUDIO;
		/* Initialization Default Value */
		xhci->exynos_uram_ctx_alloc = false;
		xhci->exynos_uram_isoc_out_alloc = false;
		xhci->exynos_uram_isoc_in_alloc = false;
		xhci->usb_audio_ctx_addr = NULL;
		xhci->usb_audio_isoc_out_addr = NULL;
		xhci->usb_audio_isoc_in_addr = NULL;
	} else {
		dev_err(&pdev->dev, "URAM is not used.\n");
	}
skip_uram:
#endif

	xhci->xhci_alloc = &xhci_pre_alloc;

	hcd->tpl_support = of_usb_host_tpl_support(sysdev->of_node);
	xhci->shared_hcd->tpl_support = hcd->tpl_support;

	if (priv) {
		ret = xhci_priv_plat_setup(hcd);
		if (ret)
			goto disable_usb_phy;
	}

	if ((xhci->quirks & XHCI_SKIP_PHY_INIT) || (priv && (priv->quirks & XHCI_SKIP_PHY_INIT)))
		hcd->skip_phy_initialization = 1;

	ret = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (ret)
		goto disable_usb_phy;

	if (HCC_MAX_PSA(xhci->hcc_params) >= 4)
		xhci->shared_hcd->can_do_streams = 1;

	ret = usb_add_hcd(xhci->shared_hcd, irq, IRQF_SHARED);
	if (ret)
		goto dealloc_usb2_hcd;

#ifdef CONFIG_SND_EXYNOS_USB_AUDIO
	ret = of_property_read_u32(parent->of_node,
				"usb_audio_offloading", &value);
	if (ret == 0 && value == 1) {
		ret = exynos_usb_audio_init(parent, pdev);
		if (ret) {
			dev_err(&pdev->dev, "USB Audio INIT fail\n");
			return ret;
		} else {
			dev_info(&pdev->dev, "USB Audio offloading is supported\n");
		}
	} else {
		dev_err(&pdev->dev, "No usb offloading, err = %d\n",
					ret);
		return ret;
	}

	xhci->out_dma = xhci_data.out_data_dma;
	xhci->out_addr = xhci_data.out_data_addr;
	xhci->in_dma = xhci_data.in_data_dma;
	xhci->in_addr = xhci_data.in_data_addr;
#endif

	ret = sysfs_create_group(&pdev->dev.kobj, &xhci_plat_attr_group);
	if (ret)
		dev_err(&pdev->dev, "failed to create xhci-plat attributes\n");

	device_enable_async_suspend(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	device_set_wakeup_enable(&xhci->main_hcd->self.root_hub->dev, 1);
	device_set_wakeup_enable(&xhci->shared_hcd->self.root_hub->dev, 1);

	/*
	 * Prevent runtime pm from being on as default, users should enable
	 * runtime pm using power/control in sysfs.
	 */
	pm_runtime_forbid(&pdev->dev);

	return 0;


dealloc_usb2_hcd:
	usb_remove_hcd(hcd);

disable_usb_phy:
	usb_phy_shutdown(hcd->usb_phy);

put_usb3_hcd:
	usb_put_hcd(xhci->shared_hcd);

disable_clk:
	clk_disable_unprepare(xhci->clk);

disable_reg_clk:
	clk_disable_unprepare(xhci->reg_clk);

put_hcd:
	usb_put_hcd(hcd);

disable_runtime:
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return ret;
}

static int xhci_plat_remove(struct platform_device *dev)
{
	struct device	*parent = dev->dev.parent;
	struct usb_hcd	*hcd = platform_get_drvdata(dev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);
	struct clk *clk = xhci->clk;
	struct clk *reg_clk = xhci->reg_clk;
	struct usb_hcd *shared_hcd = xhci->shared_hcd;
	struct usb_device *rhdev = hcd->self.root_hub;
	struct usb_device *srhdev = shared_hcd->self.root_hub;
	struct usb_device *udev;
	int port, need_wait, timeout;
	unsigned long flags;

	dev_info(&dev->dev, "XHCI PLAT REMOVE\n");

	usb3_portsc = NULL;
	pp_set_delayed = 0;

#if defined(CONFIG_USB_HOST_SAMSUNG_FEATURE)
	pr_info("%s\n", __func__);
	/* In order to prevent kernel panic */
	if (!pm_runtime_suspended(&xhci->shared_hcd->self.root_hub->dev)) {
		pr_info("%s, shared_hcd pm_runtime_forbid\n", __func__);
		pm_runtime_forbid(&xhci->shared_hcd->self.root_hub->dev);
	}
	if (!pm_runtime_suspended(&xhci->main_hcd->self.root_hub->dev)) {
		pr_info("%s, main_hcd pm_runtime_forbid\n", __func__);
		pm_runtime_forbid(&xhci->main_hcd->self.root_hub->dev);
	}
#endif

	pm_runtime_get_sync(&dev->dev);
	spin_lock_irqsave(&xhci->lock, flags);
	xhci->xhc_state |= XHCI_STATE_REMOVING;
	xhci->xhci_alloc->offset = 0;

	dev_info(&dev->dev, "WAKE UNLOCK\n");
	__pm_relax(xhci->main_wakelock);
	__pm_relax(xhci->shared_wakelock);
	spin_unlock_irqrestore(&xhci->lock, flags);

	wakeup_source_unregister(xhci->main_wakelock);
	wakeup_source_unregister(xhci->shared_wakelock);

	if (!rhdev || !srhdev)
		goto remove_hcd;

	/* check all ports */
	for (timeout = 0; timeout < XHCI_HUB_EVENT_TIMEOUT; timeout++) {
		need_wait = false;
		usb_hub_for_each_child(rhdev, port, udev) {
			if (udev && udev->devnum != -1)
				need_wait = true;
		}
		if (need_wait == false) {
			usb_hub_for_each_child(srhdev, port, udev) {
				if (udev && udev->devnum != -1)
					need_wait = true;
			}
		}
		if (need_wait == true) {
			usleep_range(20000, 22000);
			timeout += 20;
			xhci_info(xhci, "Waiting USB hub disconnect\n");
		} else {
			xhci_info(xhci,	"device disconnect all done\n");
			break;
		}
	}

remove_hcd:
#ifdef CONFIG_USB_DEBUG_DETAILED_LOG
	dev_info(&dev->dev, "remove hcd (shared)\n");
#endif
	usb_remove_hcd(shared_hcd);
	xhci->shared_hcd = NULL;
	usb_phy_shutdown(hcd->usb_phy);

	/*
	 * In usb_remove_hcd, phy_exit is called if phy is not NULL.
	 * However, in the case that PHY was turn on or off as runtime PM,
	 * PHY sould not exit at this time. So, to prevent the PHY exit,
	 * PHY pointer have to be NULL.
	 */
	if (parent && xhci->phy_usb2)
		xhci->phy_usb2 = NULL;

	if (parent && xhci->phy_usb3)
		xhci->phy_usb3 = NULL;
#ifdef CONFIG_USB_DEBUG_DETAILED_LOG
	dev_info(&dev->dev, "remove hcd (main)\n");
#endif
	usb_remove_hcd(hcd);
	devm_iounmap(&dev->dev, hcd->regs);
	usb_put_hcd(shared_hcd);

	clk_disable_unprepare(clk);
	clk_disable_unprepare(reg_clk);
	usb_put_hcd(hcd);

	pm_runtime_disable(&dev->dev);
	pm_runtime_put_noidle(&dev->dev);
	pm_runtime_set_suspended(&dev->dev);

	return 0;
}

extern u32 otg_is_connect(void);
static int __maybe_unused xhci_plat_suspend(struct device *dev)
{
	struct usb_hcd	*hcd = dev_get_drvdata(dev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);
	int ret;

	pr_info("[%s]\n", __func__);

	/*
	 * xhci_suspend() needs `do_wakeup` to know whether host is allowed
	 * to do wakeup during suspend. Since xhci_plat_suspend is currently
	 * only designed for system suspend, device_may_wakeup() is enough
	 * to dertermine whether host is allowed to do wakeup. Need to
	 * reconsider this when xhci_plat_suspend enlarges its scope, e.g.,
	 * also applies to runtime suspend.
	 */

	ret = xhci_suspend(xhci, device_may_wakeup(dev));
	if (ret)
		return ret;

	if (otg_is_connect() != 1) { /* If it is not OTG_CONNECT_ONLY */
		/* Enable HS ReWA */
		exynos_usbdrd_phy_vendor_set(xhci->phy_usb2, 1, 0);
		/* Enable SS ReWA */
		exynos_usbdrd_phy_vendor_set(xhci->phy_usb3, 1, 0);
		is_rewa_enabled = 1;
	}

	return  ret;
}

static int __maybe_unused xhci_plat_resume(struct device *dev)
{
	struct usb_hcd	*hcd = dev_get_drvdata(dev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);
	int ret;

	pr_info("[%s]\n", __func__);

	ret = xhci_priv_resume_quirk(hcd);
	if (ret)
		return ret;

	if (is_rewa_enabled == 1) {
		/* Disable SS ReWA */
		exynos_usbdrd_phy_vendor_set(xhci->phy_usb3, 1, 1);
		/* Disablee HS ReWA */
		exynos_usbdrd_phy_vendor_set(xhci->phy_usb2, 1, 1);
		exynos_usbdrd_phy_vendor_set(xhci->phy_usb2, 0, 0);
		is_rewa_enabled = 0;
	}

	return xhci_resume(xhci, 0);
}

static int __maybe_unused xhci_plat_runtime_suspend(struct device *dev)
{
	/*
	 *struct usb_hcd  *hcd = dev_get_drvdata(dev);
	 *struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	 *
	 *return xhci_suspend(xhci, true);
	 */

	pr_info("[%s]\n", __func__);
	return 0;
}

static int __maybe_unused xhci_plat_runtime_resume(struct device *dev)
{
	/*
	 *struct usb_hcd  *hcd = dev_get_drvdata(dev);
	 *struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	 *
	 *return xhci_resume(xhci, 0);
	 */

	pr_info("[%s]\n", __func__);
	return 0;
}

static const struct dev_pm_ops xhci_plat_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(xhci_plat_suspend, xhci_plat_resume)

	SET_RUNTIME_PM_OPS(xhci_plat_runtime_suspend,
			   xhci_plat_runtime_resume,
			   NULL)
};

static const struct acpi_device_id usb_xhci_acpi_match[] = {
	/* XHCI-compliant USB Controller */
	{ "PNP0D10", },
	{ }
};
MODULE_DEVICE_TABLE(acpi, usb_xhci_acpi_match);

static struct platform_driver usb_xhci_driver = {
	.probe	= xhci_plat_probe,
	.remove	= xhci_plat_remove,
	.shutdown = usb_hcd_platform_shutdown,
	.driver	= {
		.name = "xhci-hcd",
		.pm = &xhci_plat_pm_ops,
		.of_match_table = of_match_ptr(usb_xhci_of_match),
		.acpi_match_table = ACPI_PTR(usb_xhci_acpi_match),
	},
};
MODULE_ALIAS("platform:xhci-hcd");

static int __init xhci_plat_init(void)
{
	xhci_init_driver(&xhci_plat_hc_driver, &xhci_plat_overrides);
#if defined(CONFIG_USB_PORT_POWER_OPTIMIZATION)
	spin_lock_init(&xhcioff_lock);
#endif
	return platform_driver_register(&usb_xhci_driver);
}
module_init(xhci_plat_init);

static void __exit xhci_plat_exit(void)
{
	platform_driver_unregister(&usb_xhci_driver);
}
module_exit(xhci_plat_exit);

MODULE_DESCRIPTION("xHCI Platform Host Controller Driver");
MODULE_LICENSE("GPL");
