/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2008-2009 coresystems GmbH
 * Copyright (C) 2014 Google Inc.
 * Copyright (C) 2015 Intel Corporation.
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

#include <arch/hlt.h>
#include <arch/io.h>
#include <console/console.h>
#include <cpu/x86/cache.h>
#include <cpu/x86/smm.h>
#include <device/pci_def.h>
#include <elog.h>
#include <intelblocks/fast_spi.h>
#include <intelblocks/pcr.h>
#include <delay.h>
#include <device/pci_def.h>
#include <elog.h>
#include <pc80/mc146818rtc.h>
#include <spi-generic.h>
#include <soc/iomap.h>
#include <soc/lpc.h>
#include <soc/nvs.h>
#include <soc/pci_devs.h>
#include <soc/pch.h>
#include <soc/pcr_ids.h>
#include <soc/pm.h>
#include <soc/pmc.h>
#include <soc/smm.h>
#include <types.h>

/* IO Trap PCRs */
/* Trap status Register */
#define PCR_PSTH_TRPST	0x1E00
/* Trapped cycle */
#define PCR_PSTH_TRPC	0x1E10
/* Trapped write data */
#define PCR_PSTH_TRPD	0x1E18

static u8 smm_initialized = 0;

/*
 * GNVS needs to be updated by an 0xEA PM Trap (B2) after it has been located
 * by coreboot.
 */
static global_nvs_t *gnvs;
global_nvs_t *smm_get_gnvs(void)
{
	return gnvs;
}

int southbridge_io_trap_handler(int smif)
{
	switch (smif) {
	case 0x32:
		printk(BIOS_DEBUG, "OS Init\n");
		/*
		 * gnvs->smif:
		 * - On success, the IO Trap Handler returns 0
		 * - On failure, the IO Trap Handler returns a value != 0
		 */
		gnvs->smif = 0;
		return 1; /* IO trap handled */
	}

	/* Not handled */
	return 0;
}

/* Set the EOS bit */
void southbridge_smi_set_eos(void)
{
	enable_smi(EOS);
}

static void busmaster_disable_on_bus(int bus)
{
	int slot, func;
	unsigned int val;
	unsigned char hdr;

	for (slot = 0; slot < 0x20; slot++) {
		for (func = 0; func < 8; func++) {
			u32 reg32;
			device_t dev = PCI_DEV(bus, slot, func);

			val = pci_read_config32(dev, PCI_VENDOR_ID);

			if (val == 0xffffffff || val == 0x00000000 ||
			    val == 0x0000ffff || val == 0xffff0000)
				continue;

			/* Disable Bus Mastering for this one device */
			reg32 = pci_read_config32(dev, PCI_COMMAND);
			reg32 &= ~PCI_COMMAND_MASTER;
			pci_write_config32(dev, PCI_COMMAND, reg32);

			/* If this is a bridge, then follow it. */
			hdr = pci_read_config8(dev, PCI_HEADER_TYPE);
			hdr &= 0x7f;
			if (hdr == PCI_HEADER_TYPE_BRIDGE ||
			    hdr == PCI_HEADER_TYPE_CARDBUS) {
				unsigned int buses;
				buses = pci_read_config32(dev, PCI_PRIMARY_BUS);
				busmaster_disable_on_bus((buses >> 8) & 0xff);
			}
		}
	}
}


static void southbridge_smi_sleep(void)
{
	u8 reg8;
	u32 reg32;
	u8 slp_typ;
	u8 s5pwr = CONFIG_MAINBOARD_POWER_ON_AFTER_POWER_FAIL;

	/* save and recover RTC port values */
	u8 tmp70, tmp72;
	tmp70 = inb(0x70);
	tmp72 = inb(0x72);
	get_option(&s5pwr, "power_on_after_fail");
	outb(tmp70, 0x70);
	outb(tmp72, 0x72);

	/* First, disable further SMIs */
	disable_smi(SLP_SMI_EN);

	/* Figure out SLP_TYP */
	reg32 = inl(ACPI_BASE_ADDRESS + PM1_CNT);
	printk(BIOS_SPEW, "SMI#: SLP = 0x%08x\n", reg32);
	slp_typ = acpi_sleep_from_pm1(reg32);

	/* Do any mainboard sleep handling */
	mainboard_smi_sleep(slp_typ);

	if (IS_ENABLED(CONFIG_ELOG_GSMI))
		/* Log S3, S4, and S5 entry */
		if (slp_typ >= ACPI_S3)
			elog_add_event_byte(ELOG_TYPE_ACPI_ENTER, slp_typ);

	/* Clear pending GPE events */
	clear_gpe_status();

	/* Next, do the deed. */
	switch (slp_typ) {
	case ACPI_S0:
		printk(BIOS_DEBUG, "SMI#: Entering S0 (On)\n");
		break;
	case ACPI_S1:
		printk(BIOS_DEBUG, "SMI#: Entering S1 (Assert STPCLK#)\n");
		break;
	case ACPI_S3:
		printk(BIOS_DEBUG, "SMI#: Entering S3 (Suspend-To-RAM)\n");

		/* Invalidate the cache before going to S3 */
		wbinvd();
		break;
	case ACPI_S5:
		printk(BIOS_DEBUG, "SMI#: Entering S5 (Soft Power off)\n");
		/*TODO: cmos_layout.bin need to verify; cause wrong CMOS setup*/
		s5pwr = MAINBOARD_POWER_ON;
		/* Disable all GPE */
		disable_all_gpe();

		/*
		 * Always set the flag in case CMOS was changed on runtime. For
		 * "KEEP", switch to "OFF" - KEEP is software emulated
		 */
		reg8 = pci_read_config8(PCH_DEV_PMC, GEN_PMCON_B);
		if (s5pwr == MAINBOARD_POWER_ON)
			reg8 &= ~1;
		else
			reg8 |= 1;
		pci_write_config8(PCH_DEV_PMC, GEN_PMCON_B, reg8);

		/* also iterates over all bridges on bus 0 */
		busmaster_disable_on_bus(0);
		break;
	default:
		printk(BIOS_DEBUG, "SMI#: ERROR: SLP_TYP reserved\n");
		break;
	}

	/*
	 * Write back to the SLP register to cause the originally intended
	 * event again. We need to set BIT13 (SLP_EN) though to make the
	 * sleep happen.
	 */
	enable_pm1_control(SLP_EN);

	/* Make sure to stop executing code here for S3/S4/S5 */
	if (slp_typ >= ACPI_S3)
		hlt();

	/*
	 * In most sleep states, the code flow of this function ends at
	 * the line above. However, if we entered sleep state S1 and wake
	 * up again, we will continue to execute code in this function.
	 */
	reg32 = inl(ACPI_BASE_ADDRESS + PM1_CNT);
	if (reg32 & SCI_EN) {
		/* The OS is not an ACPI OS, so we set the state to S0 */
		disable_pm1_control(SLP_EN | SLP_TYP);
	}
}

/*
 * Look for Synchronous IO SMI and use save state from that
 * core in case we are not running on the same core that
 * initiated the IO transaction.
 */
static em64t101_smm_state_save_area_t *smi_apmc_find_state_save(u8 cmd)
{
	em64t101_smm_state_save_area_t *state;
	int node;

	/* Check all nodes looking for the one that issued the IO */
	for (node = 0; node < CONFIG_MAX_CPUS; node++) {
		state = smm_get_save_state(node);

		/* Check for Synchronous IO (bit0==1) */
		if (!(state->io_misc_info & (1 << 0)))
			continue;

		/* Make sure it was a write (bit4==0) */
		if (state->io_misc_info & (1 << 4))
			continue;

		/* Check for APMC IO port */
		if (((state->io_misc_info >> 16) & 0xff) != APM_CNT)
			continue;

		/* Check AX against the requested command */
		if ((state->rax & 0xff) != cmd)
			continue;

		return state;
	}

	return NULL;
}

static void southbridge_smi_gsmi(void)
{
#if IS_ENABLED(CONFIG_ELOG_GSMI)
	u32 *ret, *param;
	u8 sub_command;
	em64t101_smm_state_save_area_t *io_smi =
		smi_apmc_find_state_save(ELOG_GSMI_APM_CNT);

	if (!io_smi)
		return;

	/* Command and return value in EAX */
	ret = (u32 *)&io_smi->rax;
	sub_command = (u8)(*ret >> 8);

	/* Parameter buffer in EBX */
	param = (u32 *)&io_smi->rbx;

	/* drivers/elog/gsmi.c */
	*ret = gsmi_exec(sub_command, param);
#endif
}

static void finalize(void)
{
	static int finalize_done;

	if (finalize_done) {
		printk(BIOS_DEBUG, "SMM already finalized.\n");
		return;
	}
	finalize_done = 1;

	if (IS_ENABLED(CONFIG_SPI_FLASH_SMM))
		/* Re-init SPI driver to handle locked BAR */
		fast_spi_init();
}

static void southbridge_smi_apmc(void)
{
	u8 reg8;
	em64t101_smm_state_save_area_t *state;

	/* Emulate B2 register as the FADT / Linux expects it */

	reg8 = inb(APM_CNT);
	switch (reg8) {
	case APM_CNT_PST_CONTROL:
		printk(BIOS_DEBUG, "P-state control\n");
		break;
	case APM_CNT_ACPI_DISABLE:
		disable_pm1_control(SCI_EN);
		printk(BIOS_DEBUG, "SMI#: ACPI disabled.\n");
		break;
	case APM_CNT_ACPI_ENABLE:
		enable_pm1_control(SCI_EN);
		printk(BIOS_DEBUG, "SMI#: ACPI enabled.\n");
		break;
	case APM_CNT_FINALIZE:
		finalize();
		break;
	case APM_CNT_GNVS_UPDATE:
		if (smm_initialized) {
			printk(BIOS_DEBUG,
			       "SMI#: SMM structures already initialized!\n");
			return;
		}
		state = smi_apmc_find_state_save(reg8);
		if (state) {
			/* EBX in the state save contains the GNVS pointer */
			gnvs = (global_nvs_t *)((u32)state->rbx);
			smm_initialized = 1;
			printk(BIOS_DEBUG, "SMI#: Setting GNVS to %p\n", gnvs);
		}
		break;
	case ELOG_GSMI_APM_CNT:
		if (IS_ENABLED(CONFIG_ELOG_GSMI))
			southbridge_smi_gsmi();
		break;
	}

	mainboard_smi_apmc(reg8);
}

static void southbridge_smi_pm1(void)
{
	u16 pm1_sts = clear_pm1_status();

	/*
	 * While OSPM is not active, poweroff immediately on a power button
	 * event.
	 */
	if (pm1_sts & PWRBTN_STS) {
		/* power button pressed */
		if (IS_ENABLED(CONFIG_ELOG_GSMI))
			elog_add_event(ELOG_TYPE_POWER_BUTTON);
		disable_pm1_control(-1UL);
		enable_pm1_control(SLP_EN | (SLP_TYP_S5 << 10));
	}
}

static void southbridge_smi_gpe0(void)
{
	clear_gpe_status();
}

void __attribute__((weak))
mainboard_smi_gpi_handler(const struct gpi_status *sts) { }

static void southbridge_smi_gpi(void)
{
	struct gpi_status smi_sts;

	gpi_clear_get_smi_status(&smi_sts);
	mainboard_smi_gpi_handler(&smi_sts);

	/* Clear again after mainboard handler */
	gpi_clear_get_smi_status(&smi_sts);
}

void __attribute__((weak)) mainboard_smi_espi_handler(void) { }
static void southbridge_smi_espi(void)
{
	mainboard_smi_espi_handler();
}

static void southbridge_smi_mc(void)
{
	u32 reg32 = inl(ACPI_BASE_ADDRESS + SMI_EN);

	/* Are microcontroller SMIs enabled? */
	if ((reg32 & MCSMI_EN) == 0)
		return;

	printk(BIOS_DEBUG, "Microcontroller SMI.\n");
}

static void southbridge_smi_tco(void)
{
	u32 tco_sts = clear_tco_status();

	/* Any TCO event? */
	if (!tco_sts)
		return;

	if (tco_sts & (1 << 8)) { /* BIOSWR */
		u8 bios_cntl = pci_read_config16(PCH_DEV_SPI, BIOS_CNTL);

		if (bios_cntl & 1) {
			/*
			 * BWE is RW, so the SMI was caused by a
			 * write to BWE, not by a write to the BIOS
			 *
			 * This is the place where we notice someone
			 * is trying to tinker with the BIOS. We are
			 * trying to be nice and just ignore it. A more
			 * resolute answer would be to power down the
			 * box.
			 */
			printk(BIOS_DEBUG, "Switching back to RO\n");
			pci_write_config32(PCH_DEV_SPI, BIOS_CNTL,
					   (bios_cntl & ~1));
		} /* No else for now? */
	} else if (tco_sts & (1 << 3)) { /* TIMEOUT */
		/* Handle TCO timeout */
		printk(BIOS_DEBUG, "TCO Timeout.\n");
	}
}

static void southbridge_smi_periodic(void)
{
	u32 reg32 = inl(ACPI_BASE_ADDRESS + SMI_EN);

	/* Are periodic SMIs enabled? */
	if ((reg32 & PERIODIC_EN) == 0)
		return;

	printk(BIOS_DEBUG, "Periodic SMI.\n");
}

static void southbridge_smi_monitor(void)
{
#define IOTRAP(x) (trap_sts & (1 << x))
	u32 trap_cycle;
	u32 data, mask = 0;
	u8 trap_sts;
	int i;
	/* TRSR - Trap Status Register */
	trap_sts = pcr_read8(PID_PSTH, PCR_PSTH_TRPST);
	/* Clear trap(s) in TRSR */
	pcr_write8(PID_PSTH, PCR_PSTH_TRPST, trap_sts);

	/* TRPC - Trapped cycle */
	trap_cycle = pcr_read32(PID_PSTH, PCR_PSTH_TRPC);
	for (i = 16; i < 20; i++) {
		if (trap_cycle & (1 << i))
			mask |= (0xff << ((i - 16) << 2));
	}


	/* IOTRAP(3) SMI function call */
	if (IOTRAP(3)) {
		if (gnvs && gnvs->smif)
			io_trap_handler(gnvs->smif); /* call function smif */
		return;
	}

	/*
	 * IOTRAP(2) currently unused
	 * IOTRAP(1) currently unused
	 */

	/* IOTRAP(0) SMIC */
	if (IOTRAP(0)) {
		if (!(trap_cycle & (1 << 24))) { /* It's a write */
			printk(BIOS_DEBUG, "SMI1 command\n");
			/* Trapped write data */
			data = pcr_read32(PID_PSTH, PCR_PSTH_TRPD);
			data &= mask;
		}
	}

	printk(BIOS_DEBUG, "  trapped io address = 0x%x\n",
	       trap_cycle & 0xfffc);
	for (i = 0; i < 4; i++)
		if (IOTRAP(i))
			printk(BIOS_DEBUG, "  TRAP = %d\n", i);
	printk(BIOS_DEBUG, "  AHBE = %x\n", (trap_cycle >> 16) & 0xf);
	printk(BIOS_DEBUG, "  MASK = 0x%08x\n", mask);
	printk(BIOS_DEBUG, "  read/write: %s\n",
	       (trap_cycle & (1 << 24)) ? "read" : "write");

	if (!(trap_cycle & (1 << 24))) {
		/* Write Cycle */
		data = pcr_read32(PID_PSTH, PCR_PSTH_TRPD);
		printk(BIOS_DEBUG, "  iotrap written data = 0x%08x\n", data);
	}
#undef IOTRAP
}

typedef void (*smi_handler_t)(void);

static smi_handler_t southbridge_smi[SMI_STS_BITS] = {
	[SMI_ON_SLP_EN_STS_BIT] = southbridge_smi_sleep,
	[APM_STS_BIT] = southbridge_smi_apmc,
	[PM1_STS_BIT] = southbridge_smi_pm1,
	[GPE0_STS_BIT] = southbridge_smi_gpe0,
	[GPIO_STS_BIT] = southbridge_smi_gpi,
	[ESPI_SMI_STS_BIT] = southbridge_smi_espi,
	[MCSMI_STS_BIT] = southbridge_smi_mc,
	[TCO_STS_BIT] = southbridge_smi_tco,
	[PERIODIC_STS_BIT] = southbridge_smi_periodic,
	[MONITOR_STS_BIT] = southbridge_smi_monitor,
};

/*
 * Interrupt handler for SMI#
 */
void southbridge_smi_handler(void)
{
	int i;
	u32 smi_sts;

	/*
	 * We need to clear the SMI status registers, or we won't see what's
	 * happening in the following calls.
	 */
	smi_sts = clear_smi_status();

	/* Call SMI sub handler for each of the status bits */
	for (i = 0; i < ARRAY_SIZE(southbridge_smi); i++) {
		if (smi_sts & (1 << i)) {
			if (southbridge_smi[i]) {
				southbridge_smi[i]();
			} else {
				printk(BIOS_DEBUG,
				    "SMI_STS[%d] occurred, but no handler available.\n",
				    i);
			}
		}
	}
}
