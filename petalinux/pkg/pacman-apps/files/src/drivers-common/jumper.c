#include "hw_access.h"
#include "jumper.h"

// -----------------------------------------------------------
// J5 production jumper
// -----------------------------------------------------------

void init_jumper() {
  gpio_platform_init();
  gpio_platform_configure_pin(JUMPER_J5_MIO, GPIO_DIR_INPUT, 0);
}

int jumper_installed() {
  gpio_platform_clear_status();
  hw_u32_t value = gpio_read_pin(JUMPER_J5_MIO);
  if (gpio_platform_status() != 0)
    return JUMPER_UNREADABLE;
  // the jumper pulls the pin low against the pull-up (to be confirmed on a 1v5 PACMAN):
  return (value == 0) ? JUMPER_IN : JUMPER_OUT;
}
