#include <stdlib.h>

#include "hw_access.h"
#include "atc.h"
#include "atc_menu.h"

void atc_menu(){
  printf("ASIC timing and control (ATC) signal menu:  \r\n");
  while(1){
    printf("choose an option:\r\n");
    printf("(x) Exit timing menu (f) set ATC default configuration\r\n");
    printf("(r) read ATC registers (n) read ATC counts (t) toggle ATC destinations (s) toggle ATC run \r\n");
    printf("(a) poke A (b) poke B (c) poke C (d) poke D \r\n");
    char input = input_choice();
    printf("INFO: selected %c\r\n", input);

    switch(input){
    case 'x':
      return;
    case 'f':
      set_atc_default_config();
      break;
    case 'r':
      read_atc_registers();
      break;
    case 'n':
      read_atc_counts();
      break;
    case 't':
      toggle_atc_destinations();
      break;
    case 's':
      toggle_atc_run();
      break;
    case 'a':
      send_poke_a(0x3FF);
      break;
    case 'b':
      send_poke_b(0x3FF);
      break;
    case 'c':
      send_poke_c(0x3FF);
      break;
    case 'd':
      send_poke_d(0x3FF);
      break;
    default:
      printf("invalid selection...\r\n");
    }
  }
}
