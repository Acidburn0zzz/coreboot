/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2016 Siemens AG.
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
#include <console/console.h>
#include <device/pci.h>
#include <device/pci_ids.h>
#include <device/pci_ops.h>
#include <device/pci_def.h>
#include <string.h>
#include <delay.h>
#include <hwilib.h>
#include "nc_fpga.h"

#define FPGA_SET_PARAM(src, dst) \
{ \
	uint32_t var; \
	if (hwilib_get_field(src, (uint8_t *)&var, sizeof(var))) \
		dst = *((typeof(dst) *)var); \
}

static void init_temp_mon (void *base_adr)
{
	uint32_t cc[5], i = 0;
	uint8_t num = 0;
	volatile fan_ctrl_t *ctrl = (fan_ctrl_t *)base_adr;

	/* Program sensor delay first. */
	FPGA_SET_PARAM(FANSensorDelay, ctrl->sensordelay);
	/* Program correction curve for every used sensor. */
	if ((hwilib_get_field(FANSensorNum, &num, 1) != 1) ||
	    (num == 0) || (num > MAX_NUM_SENSORS))
		return;
	for (i = 0; i < num; i ++) {
		if (hwilib_get_field(FANSensorCfg0 + i, (uint8_t *)&cc[0],
		    sizeof(cc)) == sizeof(cc)) {
			ctrl->sensorcfg[cc[0]].rmin = cc[1] & 0xffff;
			ctrl->sensorcfg[cc[0]].rmax = cc[2] & 0xffff;
			ctrl->sensorcfg[cc[0]].nmin = cc[3] & 0xffff;
			ctrl->sensorcfg[cc[0]].nmax = cc[4] & 0xffff;
		}
	}
	ctrl->sensornum = num;
}

static void init_fan_ctrl (void *base_adr)
{
	uint8_t mask = 0, freeze_disable = 0, fan_req = 0;
	volatile fan_ctrl_t *ctrl = (fan_ctrl_t *)base_adr;

	/* Program all needed fields of FAN controller. */
	FPGA_SET_PARAM(FANSensorSelect, ctrl->sensorselect);
	FPGA_SET_PARAM(T_Warn, ctrl->t_warn);
	FPGA_SET_PARAM(T_Crit, ctrl->t_crit);
	FPGA_SET_PARAM(FANSamplingTime, ctrl->samplingtime);
	FPGA_SET_PARAM(FANSetPoint, ctrl->setpoint);
	FPGA_SET_PARAM(FANHystCtrl, ctrl->hystctrl);
	FPGA_SET_PARAM(FANHystVal, ctrl->hystval);
	FPGA_SET_PARAM(FANHystThreshold, ctrl->hystthreshold);
	FPGA_SET_PARAM(FANKp, ctrl->kp);
	FPGA_SET_PARAM(FANKi, ctrl->ki);
	FPGA_SET_PARAM(FANKd, ctrl->kd);
	FPGA_SET_PARAM(FANMaxSpeed, ctrl->fanmax);
	/* Set freeze and FAN configuration. */
	if ((hwilib_get_field(FF_FanReq, &fan_req, 1) == 1) &&
	    (hwilib_get_field(FF_FreezeDis, &freeze_disable, 1) == 1)) {
		if (!fan_req)
			mask = 1;
		else if  (fan_req && !freeze_disable)
			mask = 2;
		else
			mask = 3;
		ctrl->fanmon = mask << 10;
	}
}

/** \brief This function is the driver entry point for the init phase
 *         of the PCI bus allocator. It will initialize all the needed parts
 *         of NC_FPGA.
 * @param  *dev  Pointer to the used PCI device
 * @return void  Nothing is given back
 */
static void nc_fpga_init(struct device *dev)
{
	void *bar0_ptr = NULL;
	uint8_t cmd_reg;
	uint32_t cap = 0;

	/* All we need is mapped to BAR 0, get the address. */
	bar0_ptr = (void *)(pci_read_config32(dev, PCI_BASE_ADDRESS_0) &
				~PCI_BASE_ADDRESS_MEM_ATTR_MASK);
	cmd_reg = pci_read_config8(dev, PCI_COMMAND);
	/* Ensure BAR0 has a valid value. */
	if (!bar0_ptr || !(cmd_reg & PCI_COMMAND_MEMORY))
		return;
	/* Ensure this is really a NC FPGA by checking magic register. */
	if (read32(bar0_ptr + NC_MAGIC_OFFSET) != NC_FPGA_MAGIC)
		return;
	/* Open hwinfo block. */
	if (hwilib_find_blocks("hwinfo.hex") != CB_SUCCESS)
		return;
	/* Set up FAN controller and temperature monitor according to */
	/* capability bits. */
	cap = read32(bar0_ptr + NC_CAP1_OFFSET);
	if (cap & (NC_CAP1_TEMP_MON | NC_CAP1_FAN_CTRL))
		init_temp_mon(bar0_ptr + NC_FANMON_CTRL_OFFSET);
	if (cap & NC_CAP1_FAN_CTRL)
		init_fan_ctrl(bar0_ptr + NC_FANMON_CTRL_OFFSET);
	if (cap & NC_CAP1_DSAVE_NMI_DELAY) {
		uint16_t *dsave_ptr = (uint16_t *)(bar0_ptr + NC_DSAVE_OFFSET);
		FPGA_SET_PARAM(NvramVirtTimeDsaveReset, *dsave_ptr);
	}
	if (cap & NC_CAP1_BL_BRIGHTNESS_CTRL) {
		uint8_t *bl_bn_ptr =
				(uint8_t *)(bar0_ptr + NC_BL_BRIGHTNESS_OFFSET);
		uint8_t *bl_pwm_ptr = (uint8_t *)(bar0_ptr + NC_BL_PWM_OFFSET);
		FPGA_SET_PARAM(BL_Brightness, *bl_bn_ptr);
		FPGA_SET_PARAM(PF_PwmFreq, *bl_pwm_ptr);
	}
}

static struct device_operations nc_fpga_ops  = {
	.read_resources   = pci_dev_read_resources,
	.set_resources    = pci_dev_set_resources,
	.enable_resources = pci_dev_enable_resources,
	.init             = nc_fpga_init,
	.scan_bus         = 0,
	.ops_pci          = 0,
};

static const unsigned short nc_fpga_device_ids[] = { 0x4080, 0x4091, 0 };

static const struct pci_driver nc_fpga_driver __pci_driver = {
	.ops    = &nc_fpga_ops,
	.vendor = PCI_VENDOR_ID_SIEMENS,
	.devices = nc_fpga_device_ids,
};
