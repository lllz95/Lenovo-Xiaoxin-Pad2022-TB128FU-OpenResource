/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 Southchip Semiconductor Technology(Shanghai) Co., Ltd.
 */
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/hardware_info.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include "sc89601a_charger.h"

#undef dev_dbg
#undef dev_info
#define dev_dbg dev_err
#define dev_info dev_err

static DEFINE_MUTEX(sc89601a_i2c_lock);

static int sc89601a_read_byte(struct sc89601a_device *sc_chg, u8 *data, u8 reg)
{
	int ret;

	mutex_lock(&sc89601a_i2c_lock);
	ret = i2c_smbus_read_byte_data(sc_chg->client, reg);
	if (ret < 0) {
		dev_err(sc_chg->dev, "failed to read 0x%.2x\n", reg);
		mutex_unlock(&sc89601a_i2c_lock);
		return ret;
	}

	*data = (u8)ret;
	mutex_unlock(&sc89601a_i2c_lock);

	return 0;
}

static int sc89601a_write_byte(struct sc89601a_device *sc_chg, u8 reg, u8 data)
{
	int ret;

	mutex_lock(&sc89601a_i2c_lock);
	ret = i2c_smbus_write_byte_data(sc_chg->client, reg, data);
	mutex_unlock(&sc89601a_i2c_lock);

	return ret;
}

static int sc89601a_update_bits(struct sc89601a_device *sc_chg,
													u8 reg, u8 mask, u8 data)
{
	int ret;
	u8 tmp;

	ret = sc89601a_read_byte(sc_chg, &tmp, reg);

	if (ret)
		return ret;

	tmp &= ~mask;
	tmp |= data & mask;

	return sc89601a_write_byte(sc_chg, reg, tmp);
}

static int sc89601a_enter_ship_mode(struct sc89601a_device *sc_chg)
{
	int ret = 0;
	u8 val;
	
	val = SC89601A_BATFET_OFF << SC89601A_BATFET_DIS_SHIFT;
	ret = sc89601a_update_bits(sc_chg, SC89601A_REG_09, SC89601A_BATFET_DIS_MASK, val);
	return ret;
}

static int sc89601a_chg_enable(struct sc89601a_device *sc_chg, bool en)
{
	gpio_direction_output(sc_chg->chg_en_gpio, en);
	msleep(5);

	return 0;
}

#if 0
static int sc89601a_set_otg_volt(struct sc89601a_device *sc_chg, int volt)
{
	u8 val = 0;

	if (volt < SC89601A_BOOSTV_BASE)
		volt = SC89601A_BOOSTV_BASE;
	if (volt > SC89601A_BOOSTV_BASE + (SC89601A_BOOSTV_MASK >> SC89601A_BOOSTV_SHIFT) * SC89601A_BOOSTV_LSB)
		volt = SC89601A_BOOSTV_BASE + (SC89601A_BOOSTV_MASK >> SC89601A_BOOSTV_SHIFT) * SC89601A_BOOSTV_LSB;

	val = ((volt - SC89601A_BOOSTV_BASE) / SC89601A_BOOSTV_LSB) << SC89601A_BOOSTV_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_0A, SC89601A_BOOSTV_MASK, val);
}

static int sc89601a_set_otg_current(struct sc89601a_device *sc_chg, int curr)
{
	u8 val;
	u8 temp;

	if (curr < 1200) {
		temp = SC89601A_BOOST_LIM_500MA;
	} else {
		temp = SC89601A_BOOST_LIM_1200MA;
	}

	val = temp << SC89601A_BOOST_LIM_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_0A, SC89601A_BOOST_LIM_MASK, val);
}
#endif

static int sc89601a_enable_otg(struct sc89601a_device *sc_chg)
{
	u8 val;

	val = SC89601A_OTG_ENABLE << SC89601A_OTG_CONFIG_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_03,
							SC89601A_OTG_CONFIG_MASK, val);
}

static int sc89601a_disable_otg(struct sc89601a_device *sc_chg)
{
	u8 val;

	val = SC89601A_OTG_DISABLE << SC89601A_OTG_CONFIG_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_03,
							SC89601A_OTG_CONFIG_MASK, val);
}

static int sc89601a_enable_charger(struct sc89601a_device *sc_chg)
{
	u8 val;
	
	val = SC89601A_CHG_ENABLE << SC89601A_CHG_CONFIG_SHIFT;
	return sc89601a_update_bits(sc_chg, SC89601A_REG_03, SC89601A_CHG_CONFIG_MASK, val);
}

static int sc89601a_disable_charger(struct sc89601a_device *sc_chg)
{
	u8 val;
	
	val = SC89601A_CHG_DISABLE << SC89601A_CHG_CONFIG_SHIFT;
	return sc89601a_update_bits(sc_chg, SC89601A_REG_03, SC89601A_CHG_CONFIG_MASK, val);
}

static int sc89601a_set_otg(struct sc89601a_device *sc_chg, int enable)
{
	int ret;

	if (enable) {
		ret = sc89601a_disable_charger(sc_chg);
		ret = sc89601a_enable_otg(sc_chg);
		if (ret < 0) {
			dev_err(sc_chg->dev, "Failed to enable otg-%d\n", ret);
			return ret;
		}
	} else {
		ret = sc89601a_disable_otg(sc_chg);
		ret = sc89601a_enable_charger(sc_chg);
		if (ret < 0) {
			dev_err(sc_chg->dev, "Failed to disable otg-%d\n", ret);
		}
	}

	return ret;
}

static int sc89601a_disable_bit_4(struct sc89601a_device *sc_chg)
{
	u8 val;

	val = SC89601A_BIT_4_DISABLE << SC89601A_BIT_4_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_02,
							SC89601A_BIT_4_MASK, val);
}

static int sc89601a_set_chg_enable(struct sc89601a_device *sc_chg, int enable)
{
	int ret =0;

	dev_dbg(sc_chg->dev, "set slave charge: enable: %d\n", enable);
	if (enable) {
		sc89601a_chg_enable(sc_chg, false);
		sc89601a_enable_charger(sc_chg);
	} else {
		sc89601a_chg_enable(sc_chg, true);
		sc89601a_disable_charger(sc_chg);
	}

	return ret;
}

static int sc89601a_set_charge_current(struct sc89601a_device *sc_chg, int curr)
{
	u8 ichg;
	u8 val;

	dev_dbg(sc_chg->dev, "set slave charge: chg current: %d\n", curr);
	ichg = (curr - SC89601A_ICHG_BASE)/SC89601A_ICHG_LSB;
	val= ichg << SC89601A_ICHG_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_04, SC89601A_ICHG_MASK, val);
}

static int sc89601a_set_term_current(struct sc89601a_device *sc_chg, int curr)
{
	u8 iterm;
	u8 val;

	dev_dbg(sc_chg->dev, "set slave charge: term current: %d\n", curr);
	iterm = (curr - SC89601A_ITERM_BASE) / SC89601A_ITERM_LSB;
	val = iterm << SC89601A_ITERM_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_05, SC89601A_ITERM_MASK, val);
}

static int sc89601a_set_prechg_current(struct sc89601a_device *sc_chg, int curr)
{
	u8 iprechg;
	u8 val;

	iprechg = (curr - SC89601A_IPRECHG_BASE) / SC89601A_IPRECHG_LSB;
	val = iprechg << SC89601A_IPRECHG_SHIFT;
	
	return sc89601a_update_bits(sc_chg, SC89601A_REG_05, SC89601A_IPRECHG_MASK, val);
}

static int sc89601a_set_input_current_limit(struct sc89601a_device *sc_chg, int curr)
{
	u8 val;
	u8 input_curr;

	dev_dbg(sc_chg->dev, "set slave charge: input current: %d\n", curr);
	input_curr = (curr - SC89601A_IINLIM_BASE) / SC89601A_IINLIM_LSB;
	val = input_curr << SC89601A_IINLIM_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_00, SC89601A_IINLIM_MASK, val);
}

static int sc89601a_set_vac_ovp(struct sc89601a_device *sc_chg, int ovp)
{
	return 0;
}

static int sc89601a_set_chargevoltage(struct sc89601a_device *sc_chg, int volt)
{
	u8 val;
	u8 vchg;

	dev_dbg(sc_chg->dev, "set slave charge: voltage: %d\n", volt);
	vchg = (u8)((volt -SC89601A_VREG_BASE)/SC89601A_VREG_LSB);
	val = vchg << SC89601A_VREG_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_06, SC89601A_VREG_MASK, val);
}

static int sc89601a_get_charging_status(struct sc89601a_device *sc_chg)
{
	u8 val = 0;
	int ret;

	ret = sc89601a_read_byte(sc_chg, &val, SC89601A_REG_0B);
	if (ret < 0) {
		dev_err(sc_chg->dev, "Failed to read register 0x08:%d\n", ret);
		return ret;
	}

	val &= SC89601A_CHRG_STAT_MASK;
	val >>= SC89601A_CHRG_STAT_SHIFT;

	return val;
}
EXPORT_SYMBOL_GPL(sc89601a_get_charging_status);


static int sc89601a_enable_term(struct sc89601a_device *sc_chg, bool enable)
{
	u8 val;

	if (enable) {
		val = SC89601A_TERM_ENABLE << SC89601A_EN_TERM_SHIFT;
	} else {
		val = SC89601A_TERM_DISABLE << SC89601A_EN_TERM_SHIFT;
	}

	return sc89601a_update_bits(sc_chg, SC89601A_REG_07, SC89601A_EN_TERM_MASK, val);
}

static int sc89601a_disable_watchdog_timer(struct sc89601a_device *sc_chg)
{
	u8 val;

	val = SC89601A_WDT_DISABLE << SC89601A_WDT_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_07, SC89601A_WDT_MASK, val);
}

static int sc89601a_reset_chip(struct sc89601a_device *sc_chg)
{
	u8 val;

	val = SC89601A_RESET << SC89601A_RESET_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_14, SC89601A_RESET_MASK, val);
}

#if 0
int sc89601a_get_hiz_mode(struct sc89601a_device *sc_chg, u8 *state)
{
	u8 val;
	int ret;

	ret = sc89601a_read_byte(sc_chg, &val, SC89601A_REG_00);
	if (ret)
		return ret;

	*state = (val & SC89601A_ENHIZ_MASK) >> SC89601A_ENHIZ_SHIFT;

	return 0;
}
#endif

static int sc89601a_enter_hiz_mode(struct sc89601a_device *sc_chg)
{
	u8 val;

	val = SC89601A_HIZ_ENABLE << SC89601A_ENHIZ_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_00, SC89601A_ENHIZ_MASK, val);

}

static int sc89601a_exit_hiz_mode(struct sc89601a_device *sc_chg)
{

	u8 val;

	val = SC89601A_HIZ_DISABLE << SC89601A_ENHIZ_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_00, SC89601A_ENHIZ_MASK, val);

}

static int sc89601a_disable_iinlimit(struct sc89601a_device *sc_chg)
{
	u8 val;

	val = SC89601A_ILIM_DISABLE << SC89601A_ENILIM_SHIFT;

	return sc89601a_update_bits(sc_chg, SC89601A_REG_00, SC89601A_ENILIM_MASK, val);

}

static int sc89601a_set_input_suspend(struct sc89601a_device *sc_chg, int suspend)
{

	int  ret = 0;

	dev_dbg(sc_chg->dev, "set input suspend: %d\n", suspend);
	if (suspend) {
		sc89601a_enter_hiz_mode(sc_chg);
	} else {
		sc89601a_exit_hiz_mode(sc_chg);
	}

	return ret;
}

static int sc89601a_get_chg_status(struct sc89601a_device *sc_chg)
{
	int ret;
	u8 charge_status = 0;

	/* Read STATUS registers */
	ret = sc89601a_get_charging_status(sc_chg);
	if (ret) {
		dev_err(sc_chg->dev, "%s: read regs:0x0b fail !\n", __func__);
		return sc_chg->chg_status;
	}
	charge_status = ret;
	switch (charge_status) {
	case SC89601A_NOT_CHARGING:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case SC89601A_PRE_CHARGE:
	case SC89601A_FAST_CHARGING:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case SC89601A_CHARGE_DONE:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_FULL;
		break;
	default:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	return sc_chg->chg_status;
}

static void sc89601a_dump_regs(struct sc89601a_device *sc_chg)
{
	int addr, ret;
	u8 val;

	dev_err(sc_chg->dev, "sc89601a_dump_regs:\n");
	for (addr = 0x0; addr <= 0x14; addr++) {
		ret = sc89601a_read_byte(sc_chg, &val, addr);
		if (ret == 0) {
			dev_err(sc_chg->dev, "Reg[%.2x] = 0x%.2x \n", addr, val);
		}
	}
}

static int sc89601a_detect_device(struct sc89601a_device *sc_chg)
{
	int ret,part_no;
	u8 data;
	u8 val;

	val = SC89601A_CHG_DISABLE << SC89601A_CHG_CONFIG_SHIFT;

	sc89601a_update_bits(sc_chg, SC89601A_REG_03,
 							   SC89601A_CHG_CONFIG_MASK, val);

	ret = sc89601a_read_byte(sc_chg, &data, SC89601A_REG_0B);
	if (ret) {
		return ret;
	}

	part_no = (data & SC89601A_CHRG_STAT_MASK) >> SC89601A_CHRG_STAT_SHIFT;
	if(part_no != 0){
		dev_err(sc_chg->dev, "Not Sc89601 reason: REG_03\n");
		return 1;
	}
	data = 0;
	ret = sc89601a_read_byte(sc_chg, &data, SC89601A_REG_14);
	if (ret) {
		return ret;
	}
	dev_err(sc_chg->dev, "ret=%d,data=%d\n", ret, data);

	sc_chg->part_no = (data & SC89601A_PN_MASK) >> SC89601A_PN_SHIFT;
	sc_chg->revision = 0;

	return ret;
}

static int sc89601a_init_device(struct sc89601a_device *sc_chg)
{
	int ret;

	sc89601a_reset_chip(sc_chg);
	msleep(50);

	/*common initialization*/
	ret = sc89601a_set_term_current(sc_chg, sc_chg->cfg.term_current);
	if (ret < 0) {
		dev_err(sc_chg->dev, "Failed to set termination current:%d\n", ret);
		return ret;
	}

	ret = sc89601a_set_chargevoltage(sc_chg, sc_chg->cfg.charge_voltage);
	if (ret < 0) {
		dev_err(sc_chg->dev, "Failed to set charge voltage:%d\n",  ret);
		return ret;
	}

	ret = sc89601a_set_charge_current(sc_chg, sc_chg->cfg.charge_current);
	if (ret < 0) {
		dev_err(sc_chg->dev, "Failed to set charge current:%d\n", ret);
		return ret;
	}

	ret = sc89601a_set_prechg_current(sc_chg, sc_chg->cfg.prechg_current);
	if (ret < 0) {
		dev_err(sc_chg->dev, "Failed to set charge current:%d\n", ret);
		return ret;
	}

	sc89601a_set_vac_ovp(sc_chg, SC89601A_OVP_10500mV);
	sc89601a_enable_term(sc_chg, false);
	sc89601a_set_otg(sc_chg, false);
	sc89601a_disable_iinlimit(sc_chg);
	sc89601a_disable_bit_4(sc_chg);
	ret = sc89601a_disable_charger(sc_chg);
	if (ret < 0) {
		dev_err(sc_chg->dev, "Failed to disable charger:%d\n", ret);
		return ret;
	}

	sc89601a_set_input_suspend(sc_chg, true);
	sc89601a_disable_watchdog_timer(sc_chg);
	sc89601a_dump_regs(sc_chg);

	return ret;
}

static ssize_t sc89601a_show_registers(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sc89601a_device *sc_chg = dev_get_drvdata(dev);
	u8 addr;
	u8 val;
	u8 tmpbuf[300];
	int len;
	int idx = 0;
	int ret ;

	idx = snprintf(buf, PAGE_SIZE, "%s:\n", "Charger 1");
	for (addr = 0x0; addr <= 0x14; addr++) {
		ret = sc89601a_read_byte(sc_chg, &val, addr);
		if (ret == 0) {
			len = snprintf(tmpbuf, PAGE_SIZE - idx,"Reg[0x%.2x] = 0x%.2x\n", addr, val);
			memcpy(&buf[idx], tmpbuf, len);
			idx += len;
		}
	}

	return idx;
}

static DEVICE_ATTR(registers, S_IRUGO, sc89601a_show_registers, NULL);

static struct attribute *sc89601a_attributes[] = {
	&dev_attr_registers.attr,
	NULL,
};

static const struct attribute_group sc89601a_attr_group = {
	.attrs = sc89601a_attributes,
};

static void sc89601a_monitor_workfunc(struct work_struct *work)
{
	int ret;
	u8 status = 0;
	u8 fault = 0;
	u8 charge_status = 0;
	struct sc89601a_device *sc_chg = container_of(work,
								struct sc89601a_device, monitor_work.work);

	if(sc_chg->resume_completed != true){
		dev_err(sc_chg->dev, "sc89601a_monitor_workfunc suspend cannot work \n");
		schedule_delayed_work(&sc_chg->monitor_work, msecs_to_jiffies(10000));
		return;
	}

	/* Read STATUS and FAULT registers */
    ret = sc89601a_read_byte(sc_chg, &status, SC89601A_REG_0B);
    if (ret) {
        dev_err(sc_chg->dev, "read regs:0x0B fail !\n");
        return;
    }

    ret = sc89601a_read_byte(sc_chg, &fault, SC89601A_REG_0C);
    if (ret) {
        dev_err(sc_chg->dev, "read regs:0x0C fail !\n");
        return;
    }

	sc_chg->vbus_type = (status & SC89601A_VBUS_STAT_MASK) >> SC89601A_VBUS_STAT_SHIFT;
	sc_chg->usb_online = (status & SC89601A_PG_STAT_MASK) >> SC89601A_PG_STAT_SHIFT;
	charge_status = (status & SC89601A_CHRG_STAT_MASK) >> SC89601A_CHRG_STAT_SHIFT;
	switch (charge_status) {
	case SC89601A_NOT_CHARGING:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case SC89601A_PRE_CHARGE:
	case SC89601A_FAST_CHARGING:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case SC89601A_CHARGE_DONE:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_FULL;
		break;
	default:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	sc89601a_dump_regs(sc_chg);
	dev_err(sc_chg->dev, "%s:usb_online: %d, chg_status: %d, chg_fault: 0x%x\n",
							__func__, sc_chg->usb_online, sc_chg->chg_status, fault);

	schedule_delayed_work(&sc_chg->monitor_work, msecs_to_jiffies(10000));
}

static void sc89601a_set_charger_work(struct sc89601a_device *sc_chg, int work)
{

	if (work) {
		cancel_delayed_work_sync(&sc_chg->monitor_work);
		schedule_delayed_work(&sc_chg->monitor_work, 0);
	} else {
		cancel_delayed_work_sync(&sc_chg->monitor_work);
	}
}

static void sc89601a_charger_irq_workfunc(struct work_struct *work)
{
	int ret;
	u8 status = 0;
	u8 chg_fault = 0;
	u8 vindpm_status;
	u8 charge_status = 0;
	struct sc89601a_device *sc_chg = container_of(work,
								struct sc89601a_device, irq_work.work);

	/* Read STATUS and FAULT registers */
	msleep(5);
    ret = sc89601a_read_byte(sc_chg, &status, SC89601A_REG_0B);
    if (ret) {
        dev_err(sc_chg->dev, "read regs:0x0B fail !\n");
        return;
    }

    ret = sc89601a_read_byte(sc_chg, &chg_fault, SC89601A_REG_0C);
    if (ret) {
        dev_err(sc_chg->dev, "read regs:0x0C fail !\n");
        return;
    }

    ret = sc89601a_update_bits(sc_chg, SC89601A_REG_0D, SC89601A_FORCE_VINDPM_MASK,
		SC89601A_FORCE_VINDPM_ENABLE << SC89601A_FORCE_VINDPM_SHIFT);
    
    ret = sc89601a_read_byte(sc_chg, &vindpm_status, SC89601A_REG_0D);
    if (ret) {
        dev_err(sc_chg->dev, "read regs:0x0D fail !\n");
        return;
    }

	sc_chg->vbus_type = (status & SC89601A_VBUS_STAT_MASK) >> SC89601A_VBUS_STAT_SHIFT;
	sc_chg->usb_online = (status & SC89601A_PG_STAT_MASK) >> SC89601A_PG_STAT_SHIFT;
	charge_status = (status & SC89601A_CHRG_STAT_MASK) >> SC89601A_CHRG_STAT_SHIFT;
	switch (charge_status) {
	case SC89601A_NOT_CHARGING:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case SC89601A_PRE_CHARGE:
	case SC89601A_FAST_CHARGING:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case SC89601A_CHARGE_DONE:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_FULL;
		break;
	default:
		sc_chg->chg_status = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	dev_err(sc_chg->dev, "%s:usb_online: %d, chg_status: %d, chg_fault: 0x%x, vindpm_status: 0x%x\n",
							__func__, sc_chg->usb_online,
							sc_chg->chg_status, chg_fault, vindpm_status);
}

static irqreturn_t sc89601a_charger_interrupt(int irq, void *data)
{
	struct sc89601a_device *sc_chg = data;

	schedule_delayed_work(&sc_chg->irq_work, 0);
	return IRQ_HANDLED;
}

/************************
 * USB PSY REGISTRATION *
 ************************/
static enum power_supply_property sc89601a_usb_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VBUS_VOLTAGE,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_REAL_TYPE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_APSD_RERUN,
	POWER_SUPPLY_PROP_DP_DM,
	POWER_SUPPLY_PROP_INPUT_SUSPEND,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_ENABLED,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_UPDATE_NOW,
	POWER_SUPPLY_PROP_USB_OTG,
	POWER_SUPPLY_PROP_SET_SHIP_MODE,
};

static int sc89601a_usb_get_prop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct sc89601a_device *sc_chg = power_supply_get_drvdata(psy);
	int ret = 0;
	val->intval = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = sc89601a_get_chg_status(sc_chg);
		break;

	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = sc_chg->usb_online;
		break;

	case POWER_SUPPLY_PROP_TYPE:
	case POWER_SUPPLY_PROP_REAL_TYPE:
		val->intval = sc_chg->vbus_type;
		break;

	default:
		pr_err("get prop %d is not supported in usb\n", psp);
		ret = -EINVAL;
		break;
	}

	if (ret < 0) {
		dev_err(sc_chg->dev, "Couldn't get prop %d rc = %d\n", psp, ret);
		return -ENODATA;
	}

	return 0;
}


static int sc89601a_usb_set_prop(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct sc89601a_device *sc_chg = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_DP_DM:
		sc89601a_set_charger_work(sc_chg, val->intval);
		break;

	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		sc89601a_set_input_suspend(sc_chg, val->intval);
		break;

	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		sc89601a_set_input_current_limit(sc_chg, val->intval);
		break;

	case POWER_SUPPLY_PROP_CHARGE_ENABLED:
		sc89601a_set_chg_enable(sc_chg, val->intval);
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		sc89601a_set_chargevoltage(sc_chg, val->intval);
		break;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
		sc89601a_set_charge_current(sc_chg, val->intval);
		break;

	case POWER_SUPPLY_PROP_SET_SHIP_MODE:
		sc89601a_enter_ship_mode(sc_chg);
		break;

	default:
		dev_err(sc_chg->dev, "set prop: %d is not supported\n", psp);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int sc89601a_usb_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{

	switch (psp) {
	case POWER_SUPPLY_PROP_APSD_RERUN:
	case POWER_SUPPLY_PROP_DP_DM:
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CHARGE_ENABLED:
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_UPDATE_NOW:
	case POWER_SUPPLY_PROP_USB_OTG:
	case POWER_SUPPLY_PROP_SET_SHIP_MODE:
		return 1;

	default:
		break;
	}

	return 0;
}

static const struct power_supply_desc usb_psy_desc = {
	.name = "slave_chg",
	.type = POWER_SUPPLY_TYPE_MAIN,
	.properties = sc89601a_usb_props,
	.num_properties = ARRAY_SIZE(sc89601a_usb_props),
	.get_property = sc89601a_usb_get_prop,
	.set_property = sc89601a_usb_set_prop,
	.property_is_writeable = sc89601a_usb_prop_is_writeable,
};

static int sc89601a_init_usb_psy(struct sc89601a_device *sc_chg)
{
	struct power_supply_config wall_cfg = {};

	wall_cfg.drv_data = sc_chg;
	wall_cfg.of_node = sc_chg->dev->of_node;
	sc_chg->wall_psy = devm_power_supply_register(sc_chg->dev,
						  &usb_psy_desc,
						  &wall_cfg);
	if (IS_ERR(sc_chg->wall_psy)) {
		pr_err("Couldn't register slave chg power supply\n");
		return PTR_ERR(sc_chg->wall_psy);
	}

	return 0;
}

static int sc89601a_parse_dt(struct device *dev, struct sc89601a_device *sc_chg)
{
	int ret;
	struct device_node *np = dev->of_node;

	sc_chg->cfg.enable_auto_dpdm = of_property_read_bool(np, "sgm4151x,enable-auto-dpdm");
	sc_chg->cfg.enable_term = of_property_read_bool(np, "sgm4151x,enable-termination");
	sc_chg->cfg.enable_ico = of_property_read_bool(np, "sgm4151x,enable-ico");
	sc_chg->cfg.use_absolute_vindpm = of_property_read_bool(np, "sgm4151x,use-absolute-vindpm");

	ret = of_property_read_u32(np, "sgm4151x,charge-voltage", &sc_chg->cfg.charge_voltage);
	if (ret) {
		sc_chg->cfg.charge_voltage = DEFAULT_BATT_CV;
	} else {
		pr_err("charge_voltage: %d\n", sc_chg->cfg.charge_voltage);
	}

	ret = of_property_read_u32(np, "sgm4151x,charge-current", &sc_chg->cfg.charge_current);
	if (ret) {
		sc_chg->cfg.charge_current = DEFAULT_CHG_CURR;
	} else {
		pr_err("charge_current: %d\n", sc_chg->cfg.charge_current);
	}

	ret = of_property_read_u32(np, "sgm4151x,term-current", &sc_chg->cfg.term_current);
	if (ret) {
		sc_chg->cfg.term_current = DEFAULT_TERM_CURR;
	} else {
		pr_err("term-curren: %d\n", sc_chg->cfg.term_current);
	}

	ret = of_property_read_u32(np, "sgm4151x,prechg_current", &sc_chg->cfg.prechg_current);
	if (ret) {
		sc_chg->cfg.prechg_current = DEFAULT_PRECHG_CURR;
	} else {
		pr_err("prechg_current: %d\n", sc_chg->cfg.prechg_current);
	}

	sc_chg->chg_en_gpio = of_get_named_gpio(np, "chg-en-gpio", 0);
        if (ret < 0) {
                pr_err("%s no chg_en_gpio info\n", __func__);
	};

	sc_chg->irq_gpio = of_get_named_gpio(np, "intr-gpio", 0);
        if (ret < 0) {
                pr_err("%s get intr_gpio fail !\n", __func__);
        } else {
                pr_err("%s intr_gpio info %d\n", __func__, sc_chg->irq_gpio);
	}

	return 0;
}

static int sc89601a_charger_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	int ret;
	int irqn;
	struct sc89601a_device *sc_chg;

	sc_chg = devm_kzalloc(&client->dev, sizeof(struct sc89601a_device), GFP_KERNEL);
	if (!sc_chg) {
		dev_err(&client->dev, "%s: out of memory\n", __func__);
		return -ENOMEM;
	}

	sc_chg->dev = &client->dev;
	sc_chg->client = client;
	i2c_set_clientdata(client, sc_chg);
	dev_err(sc_chg->dev,"sc89601a_charger_probe into succeed !!! \n");

	sc_chg->resume_completed = true;
	mutex_init(&sc_chg->resume_complete_lock);



	ret = sc89601a_detect_device(sc_chg);
	if (!ret && sc_chg->part_no == SC89601A) {
		dev_err(sc_chg->dev, "charger:sc89601a detected, part_no: %d, revision:%d\n",
							sc_chg->part_no, sc_chg->revision);
	} else {
		dev_err(sc_chg->dev,"sc89601a_detect_device part_no=%d,ret=%d \n",sc_chg->part_no,ret);
		goto err_1;
	}
	
	ret = sc89601a_init_usb_psy(sc_chg);
	if (ret < 0) {
		dev_err(sc_chg->dev, "Couldn't initialize slave charge usb psy: %d\n", ret);
		goto err_1;
	}

	if (client->dev.of_node)
		sc89601a_parse_dt(&client->dev, sc_chg);

	ret = sc89601a_init_device(sc_chg);
	if (ret) {
		dev_err(sc_chg->dev, "device init failure: %d\n", ret);
		goto err_0;
	}
	ret = gpio_request(sc_chg->irq_gpio, "sc89601a irq pin");
	if (ret) {
		dev_err(sc_chg->dev, "%d gpio request failed\n", sc_chg->irq_gpio);
		goto err_0;
	}

	ret = gpio_request(sc_chg->chg_en_gpio, "chg_en_gpio");
	if (ret) {
		dev_err(sc_chg->dev, "%d chg_en_gpio request failed\n",
						sc_chg->chg_en_gpio);
		goto err_0;
	}

	irqn = gpio_to_irq(sc_chg->irq_gpio);
	if (irqn < 0) {
		dev_err(sc_chg->dev, "%d gpio_to_irq failed\n", irqn);
		ret = irqn;
		goto err_0;
	}
	client->irq = irqn;

	INIT_DELAYED_WORK(&sc_chg->irq_work, sc89601a_charger_irq_workfunc);
	INIT_DELAYED_WORK(&sc_chg->monitor_work, sc89601a_monitor_workfunc);
	ret = sysfs_create_group(&sc_chg->dev->kobj, &sc89601a_attr_group);
	if (ret) {
		dev_err(sc_chg->dev, "failed to register sysfs. err: %d\n", ret);
		goto err_irq;
	}

	ret = request_irq(client->irq, sc89601a_charger_interrupt,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						"sc89601a_charger1_irq", sc_chg);
	if (ret) {
		dev_err(sc_chg->dev, "Request IRQ %d failed: %d\n", client->irq, ret);
		goto err_irq;
	} else {
		dev_err(sc_chg->dev, "Request irq = %d\n", client->irq);
	}

	enable_irq_wake(irqn);

	hardwareinfo_set_prop(HARDWARE_SLAVE_CHARGER, "SC89601A_CHARGER");
	
	dev_err(sc_chg->dev,"sc89601a driver probe succeed !!!!!! \n");
	return 0;

err_irq:
	cancel_delayed_work_sync(&sc_chg->irq_work);
	cancel_delayed_work_sync(&sc_chg->monitor_work);
err_1:
err_0:
	mutex_destroy(&sc_chg->resume_complete_lock);

	devm_kfree(&client->dev, sc_chg);
	sc_chg = NULL;
	return -ENODEV;
}

static void sc89601a_charger_shutdown(struct i2c_client *client)
{
	struct sc89601a_device *sc_chg = i2c_get_clientdata(client);

	dev_info(sc_chg->dev, "%s: shutdown\n", __func__);

	mutex_destroy(&sc_chg->resume_complete_lock);

	sysfs_remove_group(&sc_chg->dev->kobj, &sc89601a_attr_group);
	cancel_delayed_work_sync(&sc_chg->irq_work);
	cancel_delayed_work_sync(&sc_chg->monitor_work);

	free_irq(sc_chg->client->irq, NULL);
}

static int sc89601a_charger_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sc89601a_device *sc_chg = i2c_get_clientdata(client);

	dev_info(sc_chg->dev, "sc89601a_charger_resume enter \n");

	mutex_lock(&sc_chg->resume_complete_lock);
	sc_chg->resume_completed = true;
	mutex_unlock(&sc_chg->resume_complete_lock);

	return 0;
}

static int sc89601a_charger_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sc89601a_device *sc_chg = i2c_get_clientdata(client);

	dev_info(sc_chg->dev,"sc89601a_charger_suspend enter \n");

	mutex_lock(&sc_chg->resume_complete_lock);
	sc_chg->resume_completed = false;
	mutex_unlock(&sc_chg->resume_complete_lock);

	return 0;
}

static const struct dev_pm_ops sc89601a_charger_pm_ops = {
	.resume		= sc89601a_charger_resume,
	.suspend	= sc89601a_charger_suspend,
};

static struct of_device_id sc89601a_charger_match_table[] = {
	{.compatible = "southchip,sc89601a-1",},
	{},
};


static const struct i2c_device_id sc89601a_charger_id[] = {
	{ "sc89601a-1", SC89601A },
	{},
};

MODULE_DEVICE_TABLE(i2c, sc89601a_charger_id);

static struct i2c_driver sc89601a_charger_driver = {
	.driver		= {
		.name	= "sc89601a-1",
		.of_match_table = sc89601a_charger_match_table,
		.pm		= &sc89601a_charger_pm_ops,
	},
	.id_table	= sc89601a_charger_id,

	.probe		= sc89601a_charger_probe,
	.shutdown   = sc89601a_charger_shutdown,
};

module_i2c_driver(sc89601a_charger_driver);

MODULE_DESCRIPTION("SouthChip SC89601A Charger Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aiden-yu <Aiden-yu@southchip.com>");

