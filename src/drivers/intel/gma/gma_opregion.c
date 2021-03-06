/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2017 Patrick Rudolph <siro@das-labor.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <types.h>
#include <string.h>
#include <cbfs.h>
#include <cbmem.h>
#include <console/console.h>
#include <arch/acpi.h>
#include <device/pci.h>
#include <device/pci_ids.h>
#include "intel_bios.h"
#include "gma_opregion.h"

static void *get_intel_vbios(void)
{
	/* This should probably be looking at CBFS or we should always
	 * deploy the VBIOS on Intel systems, even if we don't run it
	 * in coreboot (e.g. SeaBIOS only scenarios).
	 */
	u8 *vbios = (u8 *)0xc0000;

	optionrom_header_t *oprom = (optionrom_header_t *)vbios;
	optionrom_pcir_t *pcir = (optionrom_pcir_t *)(vbios +
						oprom->pcir_offset);

	printk(BIOS_DEBUG, "GET_VBIOS: %x %x %x %x %x\n",
		oprom->signature, pcir->vendor, pcir->classcode[0],
		pcir->classcode[1], pcir->classcode[2]);


	if ((oprom->signature == OPROM_SIGNATURE) &&
		(pcir->vendor == PCI_VENDOR_ID_INTEL) &&
		(pcir->classcode[0] == 0x00) &&
		(pcir->classcode[1] == 0x00) &&
		(pcir->classcode[2] == 0x03))
		return (void *)vbios;

	return NULL;
}

static enum cb_err init_opregion_vbt_vbios(igd_opregion_t *opregion)
{
	void *vbios;
	vbios = get_intel_vbios();
	if (!vbios) {
		printk(BIOS_DEBUG, "VBIOS not found.\n");
		return CB_ERR;
	}

	printk(BIOS_DEBUG, " ... VBIOS found at %p\n", vbios);
	optionrom_header_t *oprom = (optionrom_header_t *)vbios;
	optionrom_vbt_t *vbt = (optionrom_vbt_t *)(vbios +
						oprom->vbt_offset);

	if (read32(vbt->hdr_signature) != VBT_SIGNATURE) {
		printk(BIOS_DEBUG, "VBT not found!\n");
		return CB_ERR;
	}

	memcpy(opregion->header.vbios_version, vbt->coreblock_biosbuild, 4);
	memcpy(opregion->vbt.gvd1, vbt, vbt->hdr_vbt_size < 7168 ?
						vbt->hdr_vbt_size : 7168);

	return CB_SUCCESS;
}

/* Reading VBT table from flash */
static enum cb_err init_opregion_vbt_cbfs(igd_opregion_t *opregion)
{
	size_t vbt_size;
	union {
		const optionrom_vbt_t *data;
		uint32_t *signature;
	} vbt;

	/* Locate the vbt file in cbfs */
	vbt.data = cbfs_boot_map_with_leak("vbt.bin", CBFS_TYPE_RAW, &vbt_size);
	if (!vbt.data) {
		printk(BIOS_ERR, "vbt.bin not found in CBFS!\n");
		return CB_ERR;
	}
	printk(BIOS_DEBUG, "VBT found at %p\n", vbt.data);

	/* Validate the vbt file */
	if (*vbt.signature != VBT_SIGNATURE) {
		printk(BIOS_ERR, "Invalid signature in VBT data file (vbt.bin)!\n");
		return CB_ERR;
	}

	/* Inject GOP driver version */
#if CONFIG_SET_GOP_DRIVER_VERSION
	memcpy(opregion->header.dver, STR16(CONFIG_GOP_DRIVER_VERSION),
		2 * strlen(CONFIG_GOP_DRIVER_VERSION));
#else
	memcpy(opregion->header.vbios_version, vbt.data->coreblock_biosbuild,
		ARRAY_SIZE(vbt.data->coreblock_biosbuild));
#endif
	memcpy(opregion->vbt.gvd1, vbt.data, vbt.data->hdr_vbt_size <
		sizeof(opregion->vbt.gvd1) ? vbt.data->hdr_vbt_size :
		sizeof(opregion->vbt.gvd1));

	return CB_SUCCESS;
}

/* Initialize IGD OpRegion, called from ACPI code and OS drivers */
enum cb_err init_igd_opregion(igd_opregion_t *opregion)
{
	enum cb_err ret;

	memset((void *)opregion, 0, sizeof(igd_opregion_t));

	/* If IGD is disabled, exit here */
	if (pci_read_config16(dev_find_slot(0, PCI_DEVFN(0x2, 0)),
			 PCI_VENDOR_ID) == 0xFFFF)
		return CB_ERR;

	memcpy(&opregion->header.signature, IGD_OPREGION_SIGNATURE,
		sizeof(opregion->header.signature));

	/* 8kb */
	opregion->header.size = sizeof(igd_opregion_t) / 1024;
	opregion->header.version = (IGD_OPREGION_VERSION << 24);

	// FIXME We just assume we're mobile for now
	opregion->header.mailboxes = MAILBOXES_MOBILE;

	//From Intel OpRegion reference doc
	opregion->header.pcon = 279;

	// Initialize Mailbox 1
	opregion->mailbox1.clid = 1;

	// Initialize Mailbox 3
	opregion->mailbox3.bclp = IGD_BACKLIGHT_BRIGHTNESS;
	opregion->mailbox3.pfit = IGD_FIELD_VALID | IGD_PFIT_STRETCH;
	opregion->mailbox3.pcft = 0; // should be (IMON << 1) & 0x3e
	opregion->mailbox3.cblv = IGD_FIELD_VALID | IGD_INITIAL_BRIGHTNESS;
	opregion->mailbox3.bclm[0] = IGD_WORD_FIELD_VALID + 0x0000;
	opregion->mailbox3.bclm[1] = IGD_WORD_FIELD_VALID + 0x0a19;
	opregion->mailbox3.bclm[2] = IGD_WORD_FIELD_VALID + 0x1433;
	opregion->mailbox3.bclm[3] = IGD_WORD_FIELD_VALID + 0x1e4c;
	opregion->mailbox3.bclm[4] = IGD_WORD_FIELD_VALID + 0x2866;
	opregion->mailbox3.bclm[5] = IGD_WORD_FIELD_VALID + 0x327f;
	opregion->mailbox3.bclm[6] = IGD_WORD_FIELD_VALID + 0x3c99;
	opregion->mailbox3.bclm[7] = IGD_WORD_FIELD_VALID + 0x46b2;
	opregion->mailbox3.bclm[8] = IGD_WORD_FIELD_VALID + 0x50cc;
	opregion->mailbox3.bclm[9] = IGD_WORD_FIELD_VALID + 0x5ae5;
	opregion->mailbox3.bclm[10] = IGD_WORD_FIELD_VALID + 0x64ff;

	if (IS_ENABLED(CONFIG_ADD_GMA_VBT_DATA_FILE))
		ret = init_opregion_vbt_cbfs(opregion);
	else
		ret = init_opregion_vbt_vbios(opregion);

	if (ret != CB_SUCCESS)
		return ret;

	/* Write ASLS PCI register and prepare SWSCI register. */
	intel_gma_opregion_register((uintptr_t)opregion);

	return CB_SUCCESS;
}
