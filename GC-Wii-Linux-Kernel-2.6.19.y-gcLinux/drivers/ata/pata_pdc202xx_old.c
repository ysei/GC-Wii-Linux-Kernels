/*
 * pata_pdc202xx_old.c 	- Promise PDC202xx PATA for new ATA layer
 *			  (C) 2005 Red Hat Inc
 *			  Alan Cox <alan@redhat.com>
 *
 * Based in part on linux/drivers/ide/pci/pdc202xx_old.c
 *
 * First cut with LBA48/ATAPI
 *
 * TODO:
 *	Channel interlock/reset on both required ?
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <scsi/scsi_host.h>
#include <linux/libata.h>

#define DRV_NAME "pata_pdc202xx_old"
#define DRV_VERSION "0.2.1"

/**
 *	pdc2024x_pre_reset		-	probe begin
 *	@ap: ATA port
 *
 *	Set up cable type and use generic probe init
 */

static int pdc2024x_pre_reset(struct ata_port *ap)
{
	ap->cbl = ATA_CBL_PATA40;
	return ata_std_prereset(ap);
}


static void pdc2024x_error_handler(struct ata_port *ap)
{
	ata_bmdma_drive_eh(ap, pdc2024x_pre_reset, ata_std_softreset, NULL, ata_std_postreset);
}


static int pdc2026x_pre_reset(struct ata_port *ap)
{
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	u16 cis;

	pci_read_config_word(pdev, 0x50, &cis);
	if (cis & (1 << (10 + ap->port_no)))
		ap->cbl = ATA_CBL_PATA80;
	else
		ap->cbl = ATA_CBL_PATA40;

	return ata_std_prereset(ap);
}

static void pdc2026x_error_handler(struct ata_port *ap)
{
	ata_bmdma_drive_eh(ap, pdc2026x_pre_reset, ata_std_softreset, NULL, ata_std_postreset);
}

/**
 *	pdc_configure_piomode	-	set chip PIO timing
 *	@ap: ATA interface
 *	@adev: ATA device
 *	@pio: PIO mode
 *
 *	Called to do the PIO mode setup. Our timing registers are shared
 *	so a configure_dmamode call will undo any work we do here and vice
 *	versa
 */

static void pdc_configure_piomode(struct ata_port *ap, struct ata_device *adev, int pio)
{
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	int port = 0x60 + 4 * ap->port_no + 2 * adev->devno;
	static u16 pio_timing[5] = {
		0x0913, 0x050C , 0x0308, 0x0206, 0x0104
	};
	u8 r_ap, r_bp;

	pci_read_config_byte(pdev, port, &r_ap);
	pci_read_config_byte(pdev, port + 1, &r_bp);
	r_ap &= ~0x3F;	/* Preserve ERRDY_EN, SYNC_IN */
	r_bp &= ~0x07;
	r_ap |= (pio_timing[pio] >> 8);
	r_bp |= (pio_timing[pio] & 0xFF);

	if (ata_pio_need_iordy(adev))
		r_ap |= 0x20;	/* IORDY enable */
	if (adev->class == ATA_DEV_ATA)
		r_ap |= 0x10;	/* FIFO enable */
	pci_write_config_byte(pdev, port, r_ap);
	pci_write_config_byte(pdev, port + 1, r_bp);
}

/**
 *	pdc_set_piomode	-	set initial PIO mode data
 *	@ap: ATA interface
 *	@adev: ATA device
 *
 *	Called to do the PIO mode setup. Our timing registers are shared
 *	but we want to set the PIO timing by default.
 */

static void pdc_set_piomode(struct ata_port *ap, struct ata_device *adev)
{
	pdc_configure_piomode(ap, adev, adev->pio_mode - XFER_PIO_0);
}

/**
 *	pdc_configure_dmamode	-	set DMA mode in chip
 *	@ap: ATA interface
 *	@adev: ATA device
 *
 *	Load DMA cycle times into the chip ready for a DMA transfer
 *	to occur.
 */

static void pdc_set_dmamode(struct ata_port *ap, struct ata_device *adev)
{
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	int port = 0x60 + 4 * ap->port_no + 2 * adev->devno;
	static u8 udma_timing[6][2] = {
		{ 0x60, 0x03 },	/* 33 Mhz Clock */
		{ 0x40, 0x02 },
		{ 0x20, 0x01 },
		{ 0x40, 0x02 },	/* 66 Mhz Clock */
		{ 0x20, 0x01 },
		{ 0x20, 0x01 }
	};
	u8 r_bp, r_cp;

	pci_read_config_byte(pdev, port + 1, &r_bp);
	pci_read_config_byte(pdev, port + 2, &r_cp);

	r_bp &= ~0xF0;
	r_cp &= ~0x0F;

	if (adev->dma_mode >= XFER_UDMA_0) {
		int speed = adev->dma_mode - XFER_UDMA_0;
		r_bp |= udma_timing[speed][0];
		r_cp |= udma_timing[speed][1];

	} else {
		int speed = adev->dma_mode - XFER_MW_DMA_0;
		r_bp |= 0x60;
		r_cp |= (5 - speed);
	}
	pci_write_config_byte(pdev, port + 1, r_bp);
	pci_write_config_byte(pdev, port + 2, r_cp);

}

/**
 *	pdc2026x_bmdma_start		-	DMA engine begin
 *	@qc: ATA command
 *
 *	In UDMA3 or higher we have to clock switch for the duration of the
 *	DMA transfer sequence.
 */

static void pdc2026x_bmdma_start(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;
	struct ata_device *adev = qc->dev;
	struct ata_taskfile *tf = &qc->tf;
	int sel66 = ap->port_no ? 0x08: 0x02;

	unsigned long master = ap->host->ports[0]->ioaddr.bmdma_addr;
	unsigned long clock = master + 0x11;
	unsigned long atapi_reg = master + 0x20 + (4 * ap->port_no);

	u32 len;

	/* Check we keep host level locking here */
	if (adev->dma_mode >= XFER_UDMA_2)
		outb(inb(clock) | sel66, clock);
	else
		outb(inb(clock) & ~sel66, clock);

	/* The DMA clocks may have been trashed by a reset. FIXME: make conditional
	   and move to qc_issue ? */
	pdc_set_dmamode(ap, qc->dev);

	/* Cases the state machine will not complete correctly without help */
	if ((tf->flags & ATA_TFLAG_LBA48) ||  tf->protocol == ATA_PROT_ATAPI_DMA)
	{
		if (tf->flags & ATA_TFLAG_LBA48)
			len = qc->nsect * 512;
		else
			len = qc->nbytes;

		if (tf->flags & ATA_TFLAG_WRITE)
			len |= 0x06000000;
		else
			len |= 0x05000000;

		outl(len, atapi_reg);
	}

	/* Activate DMA */
	ata_bmdma_start(qc);
}

/**
 *	pdc2026x_bmdma_end		-	DMA engine stop
 *	@qc: ATA command
 *
 *	After a DMA completes we need to put the clock back to 33MHz for
 *	PIO timings.
 */

static void pdc2026x_bmdma_stop(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;
	struct ata_device *adev = qc->dev;
	struct ata_taskfile *tf = &qc->tf;

	int sel66 = ap->port_no ? 0x08: 0x02;
	/* The clock bits are in the same register for both channels */
	unsigned long master = ap->host->ports[0]->ioaddr.bmdma_addr;
	unsigned long clock = master + 0x11;
	unsigned long atapi_reg = master + 0x20 + (4 * ap->port_no);

	/* Cases the state machine will not complete correctly */
	if (tf->protocol == ATA_PROT_ATAPI_DMA || ( tf->flags & ATA_TFLAG_LBA48)) {
		outl(0, atapi_reg);
		outb(inb(clock) & ~sel66, clock);
	}
	/* Check we keep host level locking here */
	/* Flip back to 33Mhz for PIO */
	if (adev->dma_mode >= XFER_UDMA_2)
		outb(inb(clock) & ~sel66, clock);

	ata_bmdma_stop(qc);
}

/**
 *	pdc2026x_dev_config	-	device setup hook
 *	@ap: ATA port
 *	@adev: newly found device
 *
 *	Perform chip specific early setup. We need to lock the transfer
 *	sizes to 8bit to avoid making the state engine on the 2026x cards
 *	barf.
 */

static void pdc2026x_dev_config(struct ata_port *ap, struct ata_device *adev)
{
	adev->max_sectors = 256;
}

static struct scsi_host_template pdc_sht = {
	.module			= THIS_MODULE,
	.name			= DRV_NAME,
	.ioctl			= ata_scsi_ioctl,
	.queuecommand		= ata_scsi_queuecmd,
	.can_queue		= ATA_DEF_QUEUE,
	.this_id		= ATA_SHT_THIS_ID,
	.sg_tablesize		= LIBATA_MAX_PRD,
	.max_sectors		= ATA_MAX_SECTORS,
	.cmd_per_lun		= ATA_SHT_CMD_PER_LUN,
	.emulated		= ATA_SHT_EMULATED,
	.use_clustering		= ATA_SHT_USE_CLUSTERING,
	.proc_name		= DRV_NAME,
	.dma_boundary		= ATA_DMA_BOUNDARY,
	.slave_configure	= ata_scsi_slave_config,
	.bios_param		= ata_std_bios_param,
};

static struct ata_port_operations pdc2024x_port_ops = {
	.port_disable	= ata_port_disable,
	.set_piomode	= pdc_set_piomode,
	.set_dmamode	= pdc_set_dmamode,
	.mode_filter	= ata_pci_default_filter,
	.tf_load	= ata_tf_load,
	.tf_read	= ata_tf_read,
	.check_status 	= ata_check_status,
	.exec_command	= ata_exec_command,
	.dev_select 	= ata_std_dev_select,

	.freeze		= ata_bmdma_freeze,
	.thaw		= ata_bmdma_thaw,
	.error_handler	= pdc2024x_error_handler,
	.post_internal_cmd = ata_bmdma_post_internal_cmd,

	.bmdma_setup 	= ata_bmdma_setup,
	.bmdma_start 	= ata_bmdma_start,
	.bmdma_stop	= ata_bmdma_stop,
	.bmdma_status 	= ata_bmdma_status,

	.qc_prep 	= ata_qc_prep,
	.qc_issue	= ata_qc_issue_prot,
	.data_xfer	= ata_pio_data_xfer,

	.irq_handler	= ata_interrupt,
	.irq_clear	= ata_bmdma_irq_clear,

	.port_start	= ata_port_start,
	.port_stop	= ata_port_stop,
	.host_stop	= ata_host_stop
};

static struct ata_port_operations pdc2026x_port_ops = {
	.port_disable	= ata_port_disable,
	.set_piomode	= pdc_set_piomode,
	.set_dmamode	= pdc_set_dmamode,
	.mode_filter	= ata_pci_default_filter,
	.tf_load	= ata_tf_load,
	.tf_read	= ata_tf_read,
	.check_status 	= ata_check_status,
	.exec_command	= ata_exec_command,
	.dev_select 	= ata_std_dev_select,
	.dev_config	= pdc2026x_dev_config,

	.freeze		= ata_bmdma_freeze,
	.thaw		= ata_bmdma_thaw,
	.error_handler	= pdc2026x_error_handler,
	.post_internal_cmd = ata_bmdma_post_internal_cmd,

	.bmdma_setup 	= ata_bmdma_setup,
	.bmdma_start 	= pdc2026x_bmdma_start,
	.bmdma_stop	= pdc2026x_bmdma_stop,
	.bmdma_status 	= ata_bmdma_status,

	.qc_prep 	= ata_qc_prep,
	.qc_issue	= ata_qc_issue_prot,
	.data_xfer	= ata_pio_data_xfer,

	.irq_handler	= ata_interrupt,
	.irq_clear	= ata_bmdma_irq_clear,

	.port_start	= ata_port_start,
	.port_stop	= ata_port_stop,
	.host_stop	= ata_host_stop
};

static int pdc_init_one(struct pci_dev *dev, const struct pci_device_id *id)
{
	static struct ata_port_info info[3] = {
		{
			.sht = &pdc_sht,
			.flags = ATA_FLAG_SLAVE_POSS | ATA_FLAG_SRST,
			.pio_mask = 0x1f,
			.mwdma_mask = 0x07,
			.udma_mask = ATA_UDMA2,
			.port_ops = &pdc2024x_port_ops
		},
		{
			.sht = &pdc_sht,
			.flags = ATA_FLAG_SLAVE_POSS | ATA_FLAG_SRST,
			.pio_mask = 0x1f,
			.mwdma_mask = 0x07,
			.udma_mask = ATA_UDMA4,
			.port_ops = &pdc2026x_port_ops
		},
		{
			.sht = &pdc_sht,
			.flags = ATA_FLAG_SLAVE_POSS | ATA_FLAG_SRST,
			.pio_mask = 0x1f,
			.mwdma_mask = 0x07,
			.udma_mask = ATA_UDMA5,
			.port_ops = &pdc2026x_port_ops
		}

	};
	static struct ata_port_info *port_info[2];

	port_info[0] = port_info[1] = &info[id->driver_data];

	if (dev->device == PCI_DEVICE_ID_PROMISE_20265) {
		struct pci_dev *bridge = dev->bus->self;
		/* Don't grab anything behind a Promise I2O RAID */
		if (bridge && bridge->vendor == PCI_VENDOR_ID_INTEL) {
			if( bridge->device == PCI_DEVICE_ID_INTEL_I960)
				return -ENODEV;
			if( bridge->device == PCI_DEVICE_ID_INTEL_I960RM)
				return -ENODEV;
		}
	}
	return ata_pci_init_one(dev, port_info, 2);
}

static const struct pci_device_id pdc[] = {
	{ PCI_VDEVICE(PROMISE, PCI_DEVICE_ID_PROMISE_20246), 0 },
	{ PCI_VDEVICE(PROMISE, PCI_DEVICE_ID_PROMISE_20262), 1 },
	{ PCI_VDEVICE(PROMISE, PCI_DEVICE_ID_PROMISE_20263), 1 },
	{ PCI_VDEVICE(PROMISE, PCI_DEVICE_ID_PROMISE_20265), 2 },
	{ PCI_VDEVICE(PROMISE, PCI_DEVICE_ID_PROMISE_20267), 2 },

	{ },
};

static struct pci_driver pdc_pci_driver = {
	.name 		= DRV_NAME,
	.id_table	= pdc,
	.probe 		= pdc_init_one,
	.remove		= ata_pci_remove_one
};

static int __init pdc_init(void)
{
	return pci_register_driver(&pdc_pci_driver);
}

static void __exit pdc_exit(void)
{
	pci_unregister_driver(&pdc_pci_driver);
}

MODULE_AUTHOR("Alan Cox");
MODULE_DESCRIPTION("low-level driver for Promise 2024x and 20262-20267");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, pdc);
MODULE_VERSION(DRV_VERSION);

module_init(pdc_init);
module_exit(pdc_exit);
