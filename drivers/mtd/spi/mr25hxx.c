// SPDX-License-Identifier: GPL-2.0+
/*
 * SPI MTD driver for everspin mr25hxx mram storage devices
 *
 * Copyright (C) 2023 Ralph Gruber (ralph.gruber@avl.com)
 */

#include <common.h>
#include <log.h>
#include <spi.h>
#include <dm.h>
#include <errno.h>
#include <linux/mtd/mtd.h>


static bool mram_mtd_registered;
static char* mram_mtd_name = "mram0";
static struct mr25hxx_data* dev_data = NULL;

struct mr25hxx_data {
	unsigned long size;
	unsigned int addr_bytes;
};

/**
 * spi_mram_probe() - Probe for a SPI flash device on a bus
 * @dev: Pointer to udevice
 */
static int spi_mram_probe(struct udevice* dev)
{
	struct dm_spi_slave_platdata* platdata = dev->parent_platdata;
	struct mtd_info* mtd = dev_get_uclass_priv(dev);
	mtd->priv = dev;
	int result;

	if (platdata)
		debug("mr25hxx: platform data cs: %d, mode: %d, freq: %d\n", platdata->cs, platdata->mode, platdata->max_hz);

	/* Claim spi bus */
	result = dm_spi_claim_bus(dev);
	if (result) {
		printf("mr25hxx: failed to claim SPI bus: %d\n", result);
		return log_ret(result);
	}

	/* set device data for this device */
	dev_data = (struct mr25hxx_data*)dev_get_driver_data(dev);
	debug("mr25hxx driver data -> size: %d, addr_bytes: %d\n", dev_data->size, dev_data->addr_bytes);

	/* register device with mtd */
	if (CONFIG_IS_ENABLED(SPI_FLASH_MTD))
    {
		result = spi_mram_mtd_register(mtd);
		debug("mram registered with mtd: %d\n", result);
    }

	return log_ret(result);
}

/**
 * spi_mram_remove() - Remove SPI mram device
 * @dev: Pointer to udevice
 */
static int spi_mram_remove(struct udevice* dev)
{
	if (CONFIG_IS_ENABLED(SPI_FLASH_MTD))
		spi_mram_mtd_unregister(dev);

	/* unset device data for this device */
	dev_data = NULL;

	return log_ret(0);
}

/**
 * spi_mram_read() - Read from MRAM and store to buffer
 *
 * @mtd: Pointer to mtd_info 
 * @offset: Address to read from
 * @len: Length in bytes to read
 * @retlen: Pointer to integer where result is written to
 * @buf: Pointer to buffer where read data is stored
 */
static int spi_mram_read(struct mtd_info* mtd, loff_t offset, size_t len, size_t* retlen, u_char* buf)
{
    uchar cmd_read[1] = {0x03};
	uchar addr[3];
    int result;
	int addr_bits;

	if (dev_data->addr_bytes == 3) {
		addr[0] = (offset >> 16) & 0xff;
		addr[1] = (offset >> 8) & 0xff;
		addr[2] = offset & 0xff;
		addr_bits = 3 * 8;
	}
	else if (dev_data->addr_bytes == 2) {
		addr[0] = (offset >> 8) & 0xff;
		addr[1] = offset & 0xff;
		addr[2] = 0x0;
		addr_bits = 2 * 8;
	} else {
		printf("Number of address bytes not supported: %d", dev_data->addr_bytes);
		return -1;
	}

	/* write the READ instruction code to mr25hxx, bring CS low */
    result = dm_spi_xfer(mtd->priv, 8, &cmd_read, NULL, SPI_XFER_BEGIN);

    /* write the read address to mr25hxx */
    result = dm_spi_xfer(mtd->priv, addr_bits, addr, NULL, 0);

    /* read data from mr25hxx with the specific length and bring CS high */
    result = dm_spi_xfer(mtd->priv, 8 * len, NULL, buf, SPI_XFER_END);
	if (result == 0)
		*retlen = len;

	return log_ret(result);
}

/**
 * spi_mram_write() - Write data from buffer to MRAM
 *
 * @mtd: Pointer to mtd_info
 * @offset: Address to write to
 * @len: Length in bytes to write
 * @retlen: Pointer to integer where result is written to
 * @buf: Pointer to buffer with data that is written to mram
 */
static int spi_mram_write(struct mtd_info* mtd, loff_t offset, size_t len, size_t* retlen, const u_char* buf)
{
    uchar cmd_write_enable[1] = {0x06};
    uchar cmd_write[1] = {0x02};
	uchar addr[3];
    int result;
	int addr_bits;

	if (dev_data->addr_bytes == 3) {
		addr[0] = (offset >> 16) & 0xff;
		addr[1] = (offset >> 8) & 0xff;
		addr[2] = offset & 0xff;
		addr_bits = 3 * 8;
	}
	else if (dev_data->addr_bytes == 2) {
		addr[0] = (offset >> 8) & 0xff;
		addr[1] = offset & 0xff;
		addr[2] = 0x0;
		addr_bits = 2 * 8;
	} else {
		printf("Number of address bytes not supported: %d", dev_data->addr_bytes);
		return -1;
	}

	/* bring CS low, write the WriteEnable(WREN) instruction code to mr25hxx and then bring CS high. */
	result = dm_spi_xfer(mtd->priv, 8, &cmd_write_enable, NULL, SPI_XFER_BEGIN | SPI_XFER_END);

	/* bring CS low, write the WRITE instruction code to mr25hxx */
	result = dm_spi_xfer(mtd->priv, 8, &cmd_write, NULL, SPI_XFER_BEGIN);

	/* write the read address to mr25hxx, bring CS low */
	result = dm_spi_xfer(mtd->priv, addr_bits, addr, NULL, 0);

	/* write data with the specific length to mr25hxx and bring CS high */
	result = dm_spi_xfer(mtd->priv, 8 * len, buf, NULL, SPI_XFER_END);
	if (result == 0)
		*retlen = len;

	return log_ret(result);
}

/**
 * spi_mram_erase() - Erase MRAM
 *
 * @mtd: Pointer to mtd_info
 * @instr: Pointer to erase_info struct containing erase details
 */
static int spi_mram_erase(struct mtd_info* mtd, struct erase_info* instr)
{
	int result;
	int retlen;
	uchar* buffer = kmalloc(instr->len * sizeof(uchar), GFP_KERNEL);
	memset(buffer, 0, sizeof(buffer));

	/* unlike flash, mram cannot be cleared, but written with zeros instead. */
	result = spi_mram_write(mtd, instr->addr, instr->len, &retlen, buffer);

	kfree(buffer);
	return log_ret(result);
}

/**
 * spi_mram_sync() - Sync mram (not used)
 *
 * @mtd: Pointer to mtd_info
 */
static void spi_mram_sync(struct mtd_info* mtd)
{
}

/**
 * spi_mram_mtd_register() - Register the device at MTD
 *
 * @mtd: Pointer to mtd_info
 */
int spi_mram_mtd_register(struct mtd_info* mtd)
{
	int ret;

	if (mram_mtd_registered) {
		ret = del_mtd_device(mtd);
		if (ret)
			return log_ret(ret);

		mram_mtd_registered = false;
	}

	mram_mtd_registered = false;

	mtd->name = mram_mtd_name;
	mtd->type = MTD_RAM;
	mtd->flags = MTD_CAP_RAM;

	mtd->_erase = spi_mram_erase;
	mtd->_read = spi_mram_read;
	mtd->_write = spi_mram_write;
	mtd->_sync = spi_mram_sync;

	mtd->size = dev_data->size;
	mtd->writesize = 1;
	mtd->writebufsize = 265;
	mtd->numeraseregions = 0;
	mtd->erasesize = 1;

	ret = add_mtd_device(mtd);
	if (!ret)
		mram_mtd_registered = true;

	return log_ret(ret);
}

/**
 * spi_mram_mtd_unregister() - Unregister the device at MTD
 * 
 * @dev: Pointer to udevice
 */
void spi_mram_mtd_unregister(struct udevice* dev)
{
	int ret;
	struct mtd_info* mtd = dev_get_uclass_priv(dev);

	if (!mram_mtd_registered)
		return;

	ret = del_mtd_device(mtd);
	if (!ret) 
	{
		mram_mtd_registered = false;
		return;
	}

	/* Setting mtd->priv to NULL is the best we can do. */
	mtd->priv = NULL;
	printf("mr25hxx: failed to unregister MTD %s!", mtd->name);
}


/* support for multiple variants of everspin mram devices */
static struct mr25hxx_data mr25h40_data  = { .size = 0x80000, .addr_bytes = 3 };
static struct mr25hxx_data mr25h10_data  = { .size = 0x20000, .addr_bytes = 3 };
static struct mr25hxx_data mr25h256_data = { .size =  0x8000, .addr_bytes = 2 };
static struct mr25hxx_data mr25h128_data = { .size =  0x4000, .addr_bytes = 2 };

static const struct udevice_id spi_mram_ids[] = {
	{ .compatible = "mr25h40",  .data = (ulong)&mr25h40_data },
	{ .compatible = "mr25h10",  .data = (ulong)&mr25h10_data },
	{ .compatible = "mr25h256", .data = (ulong)&mr25h256_data },
	{ .compatible = "mr25h128", .data = (ulong)&mr25h128_data },
	{ }
};

/* This driver is implemented in the driver model (DM) framework as a UCLASS_MTD driver. 
* All relevant SPI information (bus, cs, max_freq, mode) needs to be provided by the device-tree to u-boot.
* After parsing the device tree and initialization of the drivers, u-boot instanciates a udevice structure 
* for the device. It's parent device is a spi device and holds relevant information about the bus.
* In order to be able to use the device as MTD device it is registered as MTD device. This way the mtd command 
* can be used to read/write the mram.
*/
U_BOOT_DRIVER(mr25hxx) = {
	.name		= "mr25hxx",
	.id		    = UCLASS_MTD,
	.of_match	= spi_mram_ids,
	.probe		= spi_mram_probe,
	.remove		= spi_mram_remove,
};
