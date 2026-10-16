// iic_sim.c
//
// Simulated I2C back end:  a simple PACMAN 1V5 board model for developing
// I2C code without PACMAN hardware.  Used only through the iic.c dispatcher,
// selected explicitly (iic_set_simulated), never by the hardware version register.
//
// Model:  DAC settings are stored; a tile rail is on when tile power (global
// enables bit 16) and the tile's enable are both set, and its voltage is then
// linear in the DAC code; current is voltage over a load resistor (as on the
// breakout card).  Board rails read nominal values.  The numbers are
// placeholders, not the 1V5 transfer functions.

#include "hw_access.h"
#include "global.h"

#define SIM_MAX_TILE          10
#define SIM_FULLSCALE_RAIL_MV 2500   // rail voltage at DAC code 0xFFFF (placeholder)
#define SIM_LOAD_OHM          10     // breakout card load resistor (placeholder)

static hw_val_t sim_vdda_dn[SIM_MAX_TILE+1];   // index SIM_MAX_TILE: test DAC (V+)
static hw_val_t sim_vddd_dn[SIM_MAX_TILE+1];   // index SIM_MAX_TILE: test DAC (V-)

static hw_val_t rail_mv(hw_val_t itile, hw_val_t dn){
  global_status_t gs;
  get_global_status(&gs);
  if (!(gs.enables & GLOBAL_ENABLE_TILE_POWER) || !(gs.enables & (1 << itile)))
    return 0;
  return SIM_FULLSCALE_RAIL_MV * dn / 0xFFFF;
}

void iic_sim_set_vdda_dn(hw_val_t itile, hw_val_t val){ if (itile <= SIM_MAX_TILE) sim_vdda_dn[itile] = val; }
void iic_sim_set_vddd_dn(hw_val_t itile, hw_val_t val){ if (itile <= SIM_MAX_TILE) sim_vddd_dn[itile] = val; }

hw_val_t iic_sim_mon_vdda_mv(hw_val_t itile){ return (itile < SIM_MAX_TILE) ? rail_mv(itile, sim_vdda_dn[itile]) : 0; }
hw_val_t iic_sim_mon_vddd_mv(hw_val_t itile){ return (itile < SIM_MAX_TILE) ? rail_mv(itile, sim_vddd_dn[itile]) : 0; }
hw_val_t iic_sim_mon_idda_ma(hw_val_t itile){ return iic_sim_mon_vdda_mv(itile) / SIM_LOAD_OHM; }
hw_val_t iic_sim_mon_iddd_ma(hw_val_t itile){ return iic_sim_mon_vddd_mv(itile) / SIM_LOAD_OHM; }

hw_val_t iic_sim_mon_vboard_mv(hw_val_t chan){
  const hw_val_t mv[] = {3600, 3300, 3000, 3300};   // 3V6, 3V3, 3V0, RTD probe 3V3
  return (chan < 4) ? mv[chan] : 0;
}

hw_val_t iic_sim_mon_iboard_ma(hw_val_t chan){
  const hw_val_t ma[] = {800, 400, 100};
  return (chan < 3) ? ma[chan] : 0;
}

hw_val_t iic_sim_mon_probe_dn(){ return 0x4000; }

// the MUX has no readback:  nothing to model
void iic_sim_set_mux_front_panel(hw_val_t itile){ (void) itile; }
void iic_sim_set_mux_adc(hw_val_t itile){ (void) itile; }
