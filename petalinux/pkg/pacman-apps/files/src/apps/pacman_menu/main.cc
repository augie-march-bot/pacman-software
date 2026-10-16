#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#include "hw_access.h"
#include "global.h"
#include "dma.h"
#include "led.h"
#include "iic.h"
#include "rxtx.h"
#include "rxtx_menu.h"
#include "atc.h"
#include "adc.h"
#include "asic.h"

#include "asic_menu.h"
#include "atc_menu.h"
#include "iic_menu.h"

// *** LED ***

void blink_leds(){
  printf("INFO: starting LED blink test...\r\n");
  blink_red_led();
  blink_pacman_leds();
  printf("INFO: done with LED blink test.\r\n");
}

// *** GLOBAL UNIT ***


void toggle_global_enables_obsolete(){
  unsigned enables[] = {0x00000000, 0x00010000, 0x00010001,  0x000103FF, 0x001103FF};
  static int mode = 0;
  mode = (mode + 1) % 5;
  printf("INFO: setting enables to 0x%08x \r\n", enables[mode]);
  axil_write_register(SCOPE_GLOBAL+C_ADDR_GLOBAL_ENABLES, enables[mode]);
}

void toggle_global_scratch_obsolete(){
  unsigned scra, scrb;
  static int mode = 0;
  mode = (mode + 1) % 3;
  switch(mode){
    case 1:
      scra = 0xAAAAAAAA;
      scrb = 0xBBBBBBBB;
      break;
    case 2:
      scra = 0x12341234;
      scrb = 0x7777FFFF;
      break;
    default:
      scra = 0x0;
      scrb = 0x0;
  }
  printf("INFO: setting scratch a to 0x%08x and scratch b to 0x%08x \r\n", scra, scrb);
  axil_write_register(SCOPE_GLOBAL+C_ADDR_GLOBAL_SCRATCH_A, scra);
  axil_write_register(SCOPE_GLOBAL+C_ADDR_GLOBAL_SCRATCH_B, scrb);
}

// *** MENUS ***

void adc_menu(){
  while(1){
    printf("ADC MENU:  choose an option:\r\n");
    printf("(0) main menu (1) read ADC registers (2) toggle ADC sleep (3) toggle ADC enable (4) toggle DAC \r\n");

    int input;
    if (scanf("%d", &input) != 1){
      printf("ERROR: invalid input.\r\n");
      continue;
    }
    printf("INFO: selected %d\r\n", input);

    switch(input){
    case 0:
      return;
    case 1:
      adc_read_registers();
      break;
    case 2:
      adc_toggle_sleep();
      break;
    case 3:
      adc_toggle_config();
      break;
    case 4:
      iic_toggle_test_dac();
      break;
    default:
      printf("invalid selection...\r\n");
    }
  }
  return;
}





void power_menu(){
  while(1){
    printf("POWER MENU:  choose an option:\r\n");
    printf("(0) main menu (1) toggle enables (2) toggle power (3) monitor power\r\n");
    printf("(4) write IV curves to file (5) check I2C \r\n");

    int input;
    if (scanf("%d", &input) != 1){
      printf("ERROR: invalid input.\r\n");
      continue;
    }
    printf("INFO: selected %d\r\n", input);

    switch(input){
    case 0:
      return;
    case 1:
      toggle_global_enables();
      break;
    case 2:
      iic_toggle_power();
      break;
    case 3:
      iic_monitor_power();
      break;
    case 4:
      iic_record_iv_curves();
      break;
    case 5:
      check_iic();
      break;
    default:
      printf("invalid selection...\r\n");
    }
  }
  return;
}

void rxtx_menu(){
  while(1){
    printf("RXTX MENU:  choose an option:\r\n");
    printf("choose an option:\r\n");
    printf("(0) exit RX/TX Menu \r\n");
    printf("(1) read tx status (2) read rx status (3) read tx look (4) read rx look\r\n");
    printf("(5) toggle tx UART configs (6) toggle rx UART configs (7) zero counts\r\n");
    printf("(8) toggle rx buffer config (9) toggle rx enables (10) toggle tx mask \r\n");
    printf("(11) toggle test patterns (12) toggle delay\r\n");
    printf("...\r\n");
    printf("(20) init descriptor ring mode (21) show BDs (22) show head/tail (23) clear IOC flags \r\n");
    printf("(24) single TX (25) single RX (26) batch TX (27) batch RX \r\n");
    printf("(28) show TX buffer (29) show RX buffer (30) show RX transferred \r\n");
    printf("...\r\n");
    printf("(40) reset TX DMA (41) TX DMA status (42) reset RX DMA (43) RX DMA status (44) long DMA status \r\n");
    printf("(45) benchmark TX (46) benchmark RX/TX loopback (47) check RX/TX loopback (random data) \r\n");

    int input;
    if (scanf("%d", &input) != 1){
      printf("ERROR: invalid input.\r\n");
      continue;
    }
    printf("INFO: selected %d\r\n", input);

    switch(input){
    case 0:
      return;
    case 1:
      read_tx_status();
      break;
    case 2:
      read_rx_status();
      break;
    case 3:
      read_tx_look();
      break;
    case 4:
      read_rx_look();
      break;
    case 5:
      toggle_tx_config();
      break;
    case 6:
      toggle_rx_config();
      break;
    case 7:
      zero_rxtx_counts();
      break;
    case 8:
      toggle_rx_buffer_config();
      break;
    case 9:
      toggle_rx_buffer_enables();
      break;
    case 10:
      toggle_tx_mask();
      break;
    case 11:
      toggle_rx_test_patterns();
      break;
    case 12:
      toggle_rx_test_pattern_delay();
      break;
    case 20:
      init_rxtx_descriptor_ring_mode(512);
      break;
    case 21:
      show_rxtx_bds();
      break;
    case 22:
      show_rxtx_head_tail();
      break;
    case 23:
      clear_rxtx_ioc();
      break;
    case 24:
      single_tx();
      break;
    case 25:
      single_rx();
      break;
    case 26:
      batch_tx();
      break;
    case 27:
      batch_rx();
      break;
    case 28:
      show_tx_buffer();
      break;
    case 29:
      show_rx_buffer();
      break;
    case 30:
      show_rx_transferred();
      break;
    case 40:
      dma_reset_tx(DMA_TIMEOUT);
      break;
    case 41:
      dma_show_tx_status();
      break;
    case 42:
      dma_reset_rx(DMA_TIMEOUT);
      break;
    case 43:
      dma_show_rx_status();
      break;
    case 44:
      dma_show_long_status();
      break;
    case 45:
      benchmark_tx();
      break;
    case 46:
      benchmark_rxtx_loopback();
      break;
    case 47:
      menu_check_rxtx_loopback();
      break;
    default:
      printf("invalid selection...\r\n");
    }
  }
  return;
}

void atc_menu(){
  printf("ASIC timing and control (ATC) signal menu:  \r\n");
  while(1){
    printf("choose an option:\r\n");
    printf("(0) Exit timing menu\r\n");
    printf("(1) read ATC registers (2) read ATC counts (3) toggle ATC destinations \r\n");
    printf("(4) poke C (5) poke D \r\n");

    int input;
    if (scanf("%d", &input) != 1){
      printf("ERROR: invalid input.\r\n");
      continue;
    }
    printf("INFO: selected %d\r\n", input);

    switch(input){
    case 0:
      return;
    case 1:
      read_atc_registers();
      break;
    case 2:
      read_atc_counts();
      break;
    case 3:
      toggle_atc_destinations();
      break;
    case 4:
      send_poke_c(0x3FF);
      break;
    case 5:
      send_poke_d(0x3FF);
      break;
    default:
      printf("invalid selection...\r\n");
    }
  }
}

void main_menu(){
  while(1){
    printf("MAIN MENU:  choose an option:\r\n");
    printf("(1) blink LEDs (2) global registers (3) toggle scratch registers\r\n");
    printf("(4) power menu (5) I2C menu (6) RX/TX menu (7) ATC menu (8) ADC menu \r\n");
    printf("(9) ASIC menu (10) ATC menu \r\n");
    int input;
    if (scanf("%d", &input) != 1){
      printf("ERROR: invalid input.\r\n");
      continue;
    }
    int tmp;
    while ((tmp = getchar()) != '\n' && tmp != EOF){}


    printf("INFO: selected %d\r\n", input);

    switch(input){
    case 1:
      blink_leds();
      break;
    case 2:
      read_global_status();
      break;
    case 3:
      toggle_global_scratch();
      break;
    case 4:
      power_menu();
      break;
    case 5:
      iic_menu();
      break;
    case 6:
      rxtx_menu();
      break;
    case 7:
      atc_menu();
      break;
    case 8:
      adc_menu();
      break;
    case 9:
      asic_menu();
      break;
    case 10:
      atc_menu();
      break;
default:
      printf("invalid selection...\r\n");
    }
  }
  return;
}

int main(){
  printf("pacman_menu:  PACMAN Linux driver access via menu, for diagnostics and hardware checkout.\r\n");
  //printf("Random Max:  0x%x Random Number:  0x%x \r\n", RAND_MAX, rand());

  axil_platform_init();
  print_global_version();
  iic_platform_init();
  bram_platform_init();
  init_rxtx();
  init_led();

  main_menu();
}
