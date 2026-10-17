#include "axpcharge.h"

#include "src/system/core/config.h"
#include "src/system/device/sysinfo.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define MP2731_NODE "/sys/class/power_supply/mp2731-charger"

bool axpcharge_applies(void) {
	static int answer = -1;
	if (answer < 0) {
		const sysinfo_model_t *model = sysinfo_model();
		answer = model ? model->pmic_charger : access(MP2731_NODE, F_OK) != 0;
	}
	return answer != 0;
}

#ifdef HOST_BUILD

// The simulator has no PMIC: the switches are shown and saved, nothing more.
void axpcharge_set(bool limit_voltage, bool limit_current) {
	(void)limit_voltage;
	(void)limit_current;
}
void axpcharge_tick(void) {}
bool axpcharge_holding(void) { return false; }
bool axpcharge_reg_read(uint8_t reg, uint8_t *value) {
	(void)reg;
	(void)value;
	return false;
}
bool axpcharge_reg_write(uint8_t reg, uint8_t value) {
	(void)reg;
	(void)value;
	return false;
}

#else

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>

#define AXP_BUS "/dev/i2c-0"
#define AXP_ADDR 0x34

// REG01h[2:0], charging status: 4 is "charge done".
#define AXP_REG_STATUS 0x01
#define AXP_STATUS_DONE 4

// REG62h[4:0], constant charge current: N*25 mA up to 8 (200 mA), then
// 200 + (N-8)*100 mA. 11 is 500 mA.
#define AXP_REG_CURRENT 0x62
#define AXP_CURRENT_MASK 0x1Fu
#define AXP_CURRENT_CAP 11u

// REG64h[2:0], charge target voltage: 1 4.00 V, 2 4.10 V, 3 4.20 V, 4 4.35 V,
// 5 4.40 V. The R1's axp2101.sh loads the driver with
// charge_voltage_limit=4400, which the driver writes here at every boot.
#define AXP_REG_VOLTAGE 0x64
#define AXP_VOLTAGE_MASK 0x07u
#define AXP_VOLTAGE_CAP 3u
#define AXP_VOLTAGE_STOCK 5u

// How often the tick re-asserts the switches, and retries after a failure.
#define REASSERT_MS 5000

// The charge current found before the cap first lowered it, kept in the
// config: after a restart with the cap on, the register holds the cap.
#define CURRENT_KEY "axp_charge_current"

static bool configured;
static bool want_voltage, want_current;
static bool holding;
static long long next_ms;

// The voltage to go back to: the register as first found, unless it already
// held the cap, left there by an earlier run.
static uint8_t voltage_stock = AXP_VOLTAGE_STOCK;
static bool voltage_seen;

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// One SMBus byte transfer. I2C_SLAVE_FORCE because the PMIC's own driver holds
// the address.
static bool xfer(uint8_t reg, uint8_t *value, bool write) {
	int fd = open(AXP_BUS, O_RDWR);
	if (fd < 0) {
		return false;
	}
	bool ok = false;
	if (ioctl(fd, I2C_SLAVE_FORCE, AXP_ADDR) >= 0) {
		union i2c_smbus_data data;
		data.byte = write ? *value : 0;
		struct i2c_smbus_ioctl_data args = {
			.read_write = write ? I2C_SMBUS_WRITE : I2C_SMBUS_READ,
			.command = reg,
			.size = I2C_SMBUS_BYTE_DATA,
			.data = &data,
		};
		if (ioctl(fd, I2C_SMBUS, &args) >= 0) {
			if (!write) {
				*value = data.byte;
			}
			ok = true;
		}
	}
	close(fd);
	return ok;
}

static bool axp_read(uint8_t reg, uint8_t *value) { return xfer(reg, value, false); }
static bool axp_write(uint8_t reg, uint8_t value) { return xfer(reg, &value, true); }

// Writes `target` into the bits of `mask`, keeping the others, and reads it
// back.
static bool set_field(uint8_t reg, uint8_t mask, uint8_t target) {
	uint8_t v;
	if (!axp_read(reg, &v)) {
		return false;
	}
	if ((v & mask) == target) {
		return true;
	}
	if (!axp_write(reg, (uint8_t)((v & ~mask) | target)) || !axp_read(reg, &v)) {
		return false;
	}
	return (v & mask) == target;
}

static int current_ma(unsigned n) { return n <= 8 ? (int)n * 25 : 200 + ((int)n - 8) * 100; }

static bool apply_voltage(void) {
	uint8_t v;
	if (!axp_read(AXP_REG_VOLTAGE, &v)) {
		return false;
	}
	if (!voltage_seen) {
		voltage_seen = true;
		if ((v & AXP_VOLTAGE_MASK) != AXP_VOLTAGE_CAP) {
			voltage_stock = v & AXP_VOLTAGE_MASK;
		}
	}
	return set_field(AXP_REG_VOLTAGE, AXP_VOLTAGE_MASK, want_voltage ? AXP_VOLTAGE_CAP : voltage_stock);
}

// On: a current above the cap is saved, then lowered; one already at or below
// it is left. Off: only a register holding the cap is touched, and only put
// back to a saved value, so a player that never used the switch keeps its own.
static bool apply_current(void) {
	uint8_t v;
	if (!axp_read(AXP_REG_CURRENT, &v)) {
		return false;
	}
	unsigned field = v & AXP_CURRENT_MASK;
	long saved = config_get_int("power", CURRENT_KEY, -1);

	if (want_current) {
		if (field <= AXP_CURRENT_CAP) {
			return true;
		}
		if (saved != (long)field) {
			config_set_int("power", CURRENT_KEY, (long)field);
			config_save();
		}
		return set_field(AXP_REG_CURRENT, AXP_CURRENT_MASK, AXP_CURRENT_CAP);
	}

	if (field != AXP_CURRENT_CAP || saved <= (long)AXP_CURRENT_CAP || saved > (long)AXP_CURRENT_MASK) {
		return true;
	}
	return set_field(AXP_REG_CURRENT, AXP_CURRENT_MASK, (uint8_t)saved);
}

static bool apply(void) {
	bool ok = apply_voltage();
	ok = apply_current() && ok;

	uint8_t status;
	holding = want_voltage && axp_read(AXP_REG_STATUS, &status) && (status & 7u) == AXP_STATUS_DONE;
	return ok;
}

// What the PMIC holds, once, so the log says what the switches started from.
static void log_first_contact(void) {
	static bool done;
	if (done) {
		return;
	}
	done = true;
	uint8_t volt, cur;
	if (!axp_read(AXP_REG_VOLTAGE, &volt) || !axp_read(AXP_REG_CURRENT, &cur)) {
		printf("axpcharge: the PMIC does not answer on %s at 0x%02x\n", AXP_BUS, AXP_ADDR);
		return;
	}
	printf("axpcharge: found REG64h=0x%02x, REG62h=0x%02x (%d mA)\n", volt, cur,
		   current_ma(cur & AXP_CURRENT_MASK));
}

void axpcharge_set(bool limit_voltage, bool limit_current) {
	if (!axpcharge_applies()) {
		return;
	}
	log_first_contact();
	configured = true;
	want_voltage = limit_voltage;
	want_current = limit_current;
	bool ok = apply();
	next_ms = now_ms() + REASSERT_MS;
	printf("axpcharge: charge limit %s, 500 mA limit %s%s\n", want_voltage ? "on" : "off",
		   want_current ? "on" : "off", ok ? "" : " (the PMIC did not take it, retrying)");
}

void axpcharge_tick(void) {
	if (!configured || now_ms() < next_ms) {
		return;
	}
	next_ms = now_ms() + REASSERT_MS;
	apply();
}

bool axpcharge_holding(void) { return holding; }

bool axpcharge_reg_read(uint8_t reg, uint8_t *value) { return axp_read(reg, value); }
bool axpcharge_reg_write(uint8_t reg, uint8_t value) { return axp_write(reg, value); }

#endif /* HOST_BUILD */
