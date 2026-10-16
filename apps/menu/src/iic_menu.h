#ifndef IIC_MENU_H
#define IIC_MENU_H

#include "hw_access.h"
#include "iic.h"

#ifdef __cplusplus
extern "C" {
#endif

// IV curve of one tile:  both DACs swept together, 0x0000 to 0xF000 in steps of 0x1000,
// left at 0 afterwards.  The enables are not touched.
#define IIC_IV_POINTS 16

typedef struct {
  hw_val_t dac_dn;
  hw_val_t vdda_mv, idda_ma;
  hw_val_t vddd_mv, iddd_ma;
} iic_iv_point_t;

void iic_get_iv_curve(hw_u32_t itile, iic_iv_point_t iv[IIC_IV_POINTS]);

// menu interface to ASIC driver:
void iic_menu();

// menu options:
void iic_show_status();
void iic_toggle_hardware_version();
void iic_toggle_power();
void iic_monitor_power();
void iic_toggle_test_dac();
void iic_record_iv_curves();

#ifdef __cplusplus
}
#endif

#endif // ASIC_MENU_H
