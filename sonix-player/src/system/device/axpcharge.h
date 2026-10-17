#ifndef AXPCHARGE_H
#define AXPCHARGE_H

#include <stdbool.h>
#include <stdint.h>

// Charging on a player whose AXP2101 PMIC is also its charger: the R1, which
// has no MP2731. The percentage limit in power.c needs the MP2731's node, so on
// this player there are two switches instead, both written to the PMIC over
// I2C (bus 0, address 0x34):
//
//   * the charge limit: target voltage 4.20 V instead of the stock 4.40 V
//     (REG64h). The charge tapers off and ends on its own at the lower
//     ceiling, around 80%; the charger itself is never switched off.
//   * the current limit: charge current at most 500 mA (REG62h). Never raises
//     a current that is already lower.
//
// Switching either off writes back what was there before. Both are asserted
// again every few seconds from the power tick.

// True on a player charged by the PMIC alone: by the model table when
// system-info.json names one, otherwise by the MP2731's node being absent.
// Decides which controls the Power page shows.
bool axpcharge_applies(void);

// Applies both switches at once. Does nothing where axpcharge_applies() is
// false.
void axpcharge_set(bool limit_voltage, bool limit_current);

// Re-asserts the switches, at most every few seconds. Called from the power
// tick.
void axpcharge_tick(void);

// True while the voltage cap is on and the PMIC reports the charge as done:
// the cable is in and nothing more goes into the battery. As of the last tick.
bool axpcharge_holding(void);

// One register of the PMIC, read or written over the same bus. Both players
// have the AXP2101, whichever of them charges. False when it does not answer,
// and always on the host.
bool axpcharge_reg_read(uint8_t reg, uint8_t *value);
bool axpcharge_reg_write(uint8_t reg, uint8_t value);

#endif /* AXPCHARGE_H */
