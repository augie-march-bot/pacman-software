#ifndef IIC_H
#define IIC_H

// iic.h
//
// I2C features for setting output voltages, monitoring power, and
// configuring MUX.
//
// This driver supports multiple HW versions, as selected via
// iic_set_hw_version (see below).  Unsupported features in legacy
// hardware generally do nothing and return 0, with no error.
//
// The TILE cards are labeled TILE 1- TILE 10 in PACMAN schematics.
// Here we use itile to denote the zero referenced tile index
// (e.g. TILE 1 has itile=0)
//

#include "hw_access.h"

#ifdef __cplusplus
extern "C" {
#endif

// confirm that I2C is up and running using NO-OPs
void check_iic();

// set the PACMAN HW version to use for I2C
void iic_set_hw_version(hw_val_t major, hw_val_t minor, hw_val_t patch);

// use the simulated board (iic_sim.c) instead of hardware:
void iic_set_simulated();

// report IIC version currently in use:
hw_val_t iic_get_hw_version();

// set VDDA and VDDD of channel <itile> to value <val>
void iic_set_vdda_dn(hw_u32_t itile, hw_u32_t val);
void iic_set_vddd_dn(hw_u32_t itile, hw_u32_t val);

// set the differential test DAC:  <vp> on the positive side, <vn> on the negative side
// (PACMAN 1V5:  DAC channel 10 of the VDDA and VDDD DACs; also reachable as itile 10 above)
void iic_set_test_dac_dn(hw_u32_t vp, hw_u32_t vn);

// Get monitored value of VDDA and VDDD for TILE <chan>+1 in mV
hw_u32_t iic_mon_vdda_mv(hw_u32_t itile);
hw_u32_t iic_mon_vddd_mv(hw_u32_t itile);

// Get monitored value of IDDA and IDDD for TILE <chan>+1 in mA
hw_u32_t iic_mon_idda_ma(hw_u32_t itile);
hw_u32_t iic_mon_iddd_ma(hw_u32_t itile);

// Gen monitored value of board voltage for chan <chan>
// chan:  0= 3V6, 1=3V3, 2=3V0, 3=3V3 (Probe)
hw_u32_t iic_mon_vboard_mv(hw_u32_t chan);

// Gen monitored value of board voltage for chan <chan>
// chan:  0= 3V6, 1=3V3, 2=3V0
hw_u32_t iic_mon_iboard_ma(hw_u32_t chan);

// Get monitored value of votage drop across probe in raw counts:
hw_u32_t iic_mon_probe_dn();

// Setup MUX to route chan to front panel: (0=off, 1-10=TILE, 11=DAC)
void iic_set_mux_front_panel(hw_val_t chan);
// raw register write to one front-panel MUX chip, side 0: P, side 1: N (PACMAN 1V5 MAX14661:
// 0x10-0x13 shadow switch registers, 0x14 CMDA (COMA), 0x15 CMDB (COMB))
void iic_write_mux_reg(hw_val_t side, hw_val_t reg, hw_val_t value);

// Setup MUX to route chan to ADC: (0=off, 1-10=TILE, 11=DAC)
void iic_set_mux_adc(hw_val_t chan);


#ifdef __cplusplus
}
#endif

#endif // IIC_H
