// SPDX-License-Identifier: GPL-2.0+
/*
 * AMD Pink Sardine ACP PCI Driver
 *
 * Copyright 2022 Advanced Micro Devices, Inc.
 */

#include <linux/pci.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/interrupt.h>
#include <sound/pcm_params.h>
#include <linux/pm_runtime.h>

#include "acp63.h"

static int acp63_power_on(void __iomem *acp_base)
{
	u32 val;
	int timeout;

	val = acp63_readl(acp_base + ACP_PGFSM_STATUS);

	if (!val)
		return val;

	if ((val & ACP_PGFSM_STATUS_MASK) != ACP_POWER_ON_IN_PROGRESS)
		acp63_writel(ACP_PGFSM_CNTL_POWER_ON_MASK, acp_base + ACP_PGFSM_CONTROL);
	timeout = 0;
	while (++timeout < 500) {
		val = acp63_readl(acp_base + ACP_PGFSM_STATUS);
		if (!val)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int acp63_reset(void __iomem *acp_base)
{
	u32 val;
	int timeout;

	acp63_writel(1, acp_base + ACP_SOFT_RESET);
	timeout = 0;
	while (++timeout < 500) {
		val = acp63_readl(acp_base + ACP_SOFT_RESET);
		if (val & ACP_SOFT_RESET_SOFTRESET_AUDDONE_MASK)
			break;
		cpu_relax();
	}
	acp63_writel(0, acp_base + ACP_SOFT_RESET);
	timeout = 0;
	while (++timeout < 500) {
		val = acp63_readl(acp_base + ACP_SOFT_RESET);
		if (!val)
			return 0;
		cpu_relax();
	}
	return -ETIMEDOUT;
}

static void acp63_enable_interrupts(void __iomem *acp_base)
{
	acp63_writel(1, acp_base + ACP_EXTERNAL_INTR_ENB);
}

static void acp63_disable_interrupts(void __iomem *acp_base)
{
	acp63_writel(ACP_EXT_INTR_STAT_CLEAR_MASK, acp_base +
		     ACP_EXTERNAL_INTR_STAT);
	acp63_writel(0, acp_base + ACP_EXTERNAL_INTR_CNTL);
	acp63_writel(0, acp_base + ACP_EXTERNAL_INTR_ENB);
}

static int acp63_init(void __iomem *acp_base, struct device *dev)
{
	int ret;

	ret = acp63_power_on(acp_base);
	if (ret) {
		dev_err(dev, "ACP power on failed\n");
		return ret;
	}
	acp63_writel(0x01, acp_base + ACP_CONTROL);
	ret = acp63_reset(acp_base);
	if (ret) {
		dev_err(dev, "ACP reset failed\n");
		return ret;
	}
	acp63_enable_interrupts(acp_base);
	return 0;
}

static int acp63_deinit(void __iomem *acp_base, struct device *dev)
{
	int ret;

	acp63_disable_interrupts(acp_base);
	ret = acp63_reset(acp_base);
	if (ret) {
		dev_err(dev, "ACP reset failed\n");
		return ret;
	}
	acp63_writel(0, acp_base + ACP_CONTROL);
	return 0;
}

static irqreturn_t acp63_irq_handler(int irq, void *dev_id)
{
	struct acp63_dev_data *adata;
	struct pdm_dev_data *ps_pdm_data;
	u32 val;

	adata = dev_id;
	if (!adata)
		return IRQ_NONE;

	val = acp63_readl(adata->acp63_base + ACP_EXTERNAL_INTR_STAT);
	if (val & BIT(PDM_DMA_STAT)) {
		ps_pdm_data = dev_get_drvdata(&adata->pdev[0]->dev);
		acp63_writel(BIT(PDM_DMA_STAT), adata->acp63_base + ACP_EXTERNAL_INTR_STAT);
		if (ps_pdm_data->capture_stream)
			snd_pcm_period_elapsed(ps_pdm_data->capture_stream);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

static int snd_acp63_probe(struct pci_dev *pci,
			   const struct pci_device_id *pci_id)
{
	struct acp63_dev_data *adata;
	struct platform_device_info pdevinfo[ACP63_DEVS];
	int index, ret;
	int val = 0x00;
	struct acpi_device *adev;
	const union acpi_object *obj;
	u32 addr;
	unsigned int irqflags;

	irqflags = IRQF_SHARED;
	/* Pink Sardine device check */
	switch (pci->revision) {
	case 0x63:
		break;
	default:
		dev_dbg(&pci->dev, "acp63 pci device not found\n");
		return -ENODEV;
	}
	if (pci_enable_device(pci)) {
		dev_err(&pci->dev, "pci_enable_device failed\n");
		return -ENODEV;
	}

	ret = pci_request_regions(pci, "AMD ACP6.2 audio");
	if (ret < 0) {
		dev_err(&pci->dev, "pci_request_regions failed\n");
		goto disable_pci;
	}
	adata = devm_kzalloc(&pci->dev, sizeof(struct acp63_dev_data),
			     GFP_KERNEL);
	if (!adata) {
		ret = -ENOMEM;
		goto release_regions;
	}

	addr = pci_resource_start(pci, 0);
	adata->acp63_base = devm_ioremap(&pci->dev, addr,
					 pci_resource_len(pci, 0));
	if (!adata->acp63_base) {
		ret = -ENOMEM;
		goto release_regions;
	}
	pci_set_master(pci);
	pci_set_drvdata(pci, adata);
	ret = acp63_init(adata->acp63_base, &pci->dev);
	if (ret)
		goto release_regions;
	val = acp63_readl(adata->acp63_base + ACP_PIN_CONFIG);
	switch (val) {
	case ACP_CONFIG_0:
	case ACP_CONFIG_1:
	case ACP_CONFIG_2:
	case ACP_CONFIG_3:
	case ACP_CONFIG_9:
	case ACP_CONFIG_15:
		dev_info(&pci->dev, "Audio Mode %d\n", val);
		break;
	default:

		/* Checking DMIC hardware*/
		adev = acpi_find_child_device(ACPI_COMPANION(&pci->dev), 0x02, 0);

		if (!adev)
			break;

		if (!acpi_dev_get_property(adev, "acp-audio-device-type",
					   ACPI_TYPE_INTEGER, &obj) &&
					   obj->integer.value == 2) {
			adata->res = devm_kzalloc(&pci->dev, sizeof(struct resource), GFP_KERNEL);
			if (!adata->res) {
				ret = -ENOMEM;
				goto de_init;
			}

			adata->res->name = "acp_iomem";
			adata->res->flags = IORESOURCE_MEM;
			adata->res->start = addr;
			adata->res->end = addr + (ACP63_REG_END - ACP63_REG_START);
			adata->acp63_audio_mode = ACP63_PDM_MODE;

			memset(&pdevinfo, 0, sizeof(pdevinfo));
			pdevinfo[0].name = "acp_ps_pdm_dma";
			pdevinfo[0].id = 0;
			pdevinfo[0].parent = &pci->dev;
			pdevinfo[0].num_res = 1;
			pdevinfo[0].res = adata->res;

			pdevinfo[1].name = "dmic-codec";
			pdevinfo[1].id = 0;
			pdevinfo[1].parent = &pci->dev;

			pdevinfo[2].name = "acp_ps_mach";
			pdevinfo[2].id = 0;
			pdevinfo[2].parent = &pci->dev;

			for (index = 0; index < ACP63_DEVS; index++) {
				adata->pdev[index] =
					platform_device_register_full(&pdevinfo[index]);

				if (IS_ERR(adata->pdev[index])) {
					dev_err(&pci->dev,
						"cannot register %s device\n",
						 pdevinfo[index].name);
					ret = PTR_ERR(adata->pdev[index]);
					goto unregister_devs;
				}
				ret = devm_request_irq(&pci->dev, pci->irq, acp63_irq_handler,
						       irqflags, "ACP_PCI_IRQ", adata);
				if (ret) {
					dev_err(&pci->dev, "ACP PCI IRQ request failed\n");
					goto unregister_devs;
				}
			}
		}
		break;
	}
	pm_runtime_set_autosuspend_delay(&pci->dev, ACP_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&pci->dev);
	pm_runtime_put_noidle(&pci->dev);
	pm_runtime_allow(&pci->dev);
	return 0;
unregister_devs:
	for (--index; index >= 0; index--)
		platform_device_unregister(adata->pdev[index]);
de_init:
	if (acp63_deinit(adata->acp63_base, &pci->dev))
		dev_err(&pci->dev, "ACP de-init failed\n");
release_regions:
	pci_release_regions(pci);
disable_pci:
	pci_disable_device(pci);

	return ret;
}

static int __maybe_unused snd_acp63_suspend(struct device *dev)
{
	struct acp63_dev_data *adata;
	int ret;

	adata = dev_get_drvdata(dev);
	ret = acp63_deinit(adata->acp63_base, dev);
	if (ret)
		dev_err(dev, "ACP de-init failed\n");
	return ret;
}

static int __maybe_unused snd_acp63_resume(struct device *dev)
{
	struct acp63_dev_data *adata;
	int ret;

	adata = dev_get_drvdata(dev);
	ret = acp63_init(adata->acp63_base, dev);
	if (ret)
		dev_err(dev, "ACP init failed\n");
	return ret;
}

static const struct dev_pm_ops acp63_pm_ops = {
	SET_RUNTIME_PM_OPS(snd_acp63_suspend, snd_acp63_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(snd_acp63_suspend, snd_acp63_resume)
};

static void snd_acp63_remove(struct pci_dev *pci)
{
	struct acp63_dev_data *adata;
	int ret, index;

	adata = pci_get_drvdata(pci);
	if (adata->acp63_audio_mode == ACP63_PDM_MODE) {
		for (index = 0; index < ACP63_DEVS; index++)
			platform_device_unregister(adata->pdev[index]);
	}
	ret = acp63_deinit(adata->acp63_base, &pci->dev);
	if (ret)
		dev_err(&pci->dev, "ACP de-init failed\n");
	pm_runtime_forbid(&pci->dev);
	pm_runtime_get_noresume(&pci->dev);
	pci_release_regions(pci);
	pci_disable_device(pci);
}

static const struct pci_device_id snd_acp63_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, ACP_DEVICE_ID),
	.class = PCI_CLASS_MULTIMEDIA_OTHER << 8,
	.class_mask = 0xffffff },
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, snd_acp63_ids);

static struct pci_driver ps_acp63_driver  = {
	.name = KBUILD_MODNAME,
	.id_table = snd_acp63_ids,
	.probe = snd_acp63_probe,
	.remove = snd_acp63_remove,
	.driver = {
		.pm = &acp63_pm_ops,
	}
};

module_pci_driver(ps_acp63_driver);

MODULE_AUTHOR("Vijendar.Mukunda@amd.com");
MODULE_AUTHOR("Syed.SabaKareem@amd.com");
MODULE_DESCRIPTION("AMD ACP Pink Sardine PCI driver");
MODULE_LICENSE("GPL v2");
