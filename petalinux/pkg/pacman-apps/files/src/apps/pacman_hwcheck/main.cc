// pacman_hwcheck:  PACMAN hardware checkout.
//
// Run sets (by function) and modifiers are selected on the command line;
// see usage() and /agents/augie/pacman/hwcheck/options.md.
//
// Output:  one line per item, "<VERDICT> <name>---- <value>", then a final
// "VERDICT" line.  Exit code:  0 PASS, 1 FAIL, 2 INCOMPLETE, 3 usage or refused.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "hw_access.h"
#include "global.h"
#include "iic.h"
#include "jumper.h"
#include "led.h"
#include "rxtx.h"
#include "dma.h"
#include "rxtx_menu.h"

#define EXIT_PASS        0
#define EXIT_FAIL        1
#define EXIT_INCOMPLETE  2
#define EXIT_USAGE       3   // also: refused (J5 installed)

#define LOOPBACK_BITS_PER_PACKET (40*64)
#define LOOPBACK_BITS_DEFAULT    25600000   // 10000 packets

static int G_NPASS = 0, G_NFAIL = 0, G_NSKIP = 0;
static bool G_SIM_IIC = false;   // --sim_i2c

// one result line:  verdict is PASS, FAIL, SKIP or INFO
static void result(const char * verdict, const char * name, const char * fmt, ...){
  char value[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(value, sizeof(value), fmt, args);
  va_end(args);

  char dashes[32];
  int n = 24 - (int) strlen(name);
  if (n < 2) n = 2;
  memset(dashes, '-', n);
  dashes[n] = '\0';
  printf("%-4s  %s%s %s\n", verdict, name, dashes, value);

  if (strcmp(verdict, "PASS") == 0) G_NPASS++;
  if (strcmp(verdict, "FAIL") == 0) G_NFAIL++;
  if (strcmp(verdict, "SKIP") == 0) G_NSKIP++;
}

// *** READ ONLY STATUS REPORT ***

static void read_only_jumper(){
  int j5 = jumper_installed();
  if (j5 == JUMPER_UNREADABLE)
    result("FAIL", "j5_jumper", "unreadable (MIO%d)", JUMPER_J5_MIO);
  else
    result("INFO", "j5_jumper", (j5 == JUMPER_IN) ? "installed" : "not installed");
}

static void read_only_global(global_status_t * gs){
  char date[32], hash[32];
  get_synthesis_date_string(date, sizeof(date));
  get_git_hash_string(hash, sizeof(hash));

  // Zipline firmware is 3.X; hwcheck v1 is for hardware 1v5:
  result((gs->firmware_major == 3) ? "PASS" : "FAIL", "firmware_version", "%u.%u.%u",
         (unsigned) gs->firmware_major, (unsigned) gs->firmware_minor, (unsigned) gs->firmware_patch);
  result((gs->hardware_major == 1 && gs->hardware_minor == 5) ? "PASS" : "FAIL", "hardware_version", "%u.%u.%u",
         (unsigned) gs->hardware_major, (unsigned) gs->hardware_minor, (unsigned) gs->hardware_patch);
  result("INFO", "synthesis_date", "%s", date);
  result("INFO", "git_hash", "%s", hash);
  result("INFO", "vivado_version", "%u.%u", (unsigned) gs->vivado_major, (unsigned) gs->vivado_minor);
  result("INFO", "enables", "0x%08x", (unsigned) gs->enables);
  result("INFO", "status", "0x%08x", (unsigned) gs->status);
}

static void read_only_dma_status(const char * name, hw_val_t sr){
  const hw_val_t errors = DMASR_DMA_INT_ERR | DMASR_DMA_SEC_ERR | DMASR_DMA_DEC_ERR
                        | DMASR_SG_INT_ERR  | DMASR_SG_SEC_ERR  | DMASR_SG_DEC_ERR;
  result((sr & errors) ? "FAIL" : "INFO", name, "0x%08x%s%s", (unsigned) sr,
         (sr & DMASR_HALTED) ? " halted" : "", (sr & errors) ? " ERROR" : "");
}

static void read_only_rxtx(){
  unsigned enabled = 0, frame = 0, noise = 0, lost = 0;
  rx_uart_status_t us;
  for (unsigned i = 0; i < 40; i++){
    get_rx_uart_status(i, &us);
    if (rx_uart_is_enabled(i)) enabled++;
    frame += us.frame_err;
    noise += us.noise;
    lost  += us.lost;
  }
  result("INFO", "rx_uarts_enabled", "%u of 40", enabled);
  result("INFO", "rx_frame_errors", "%u", frame);
  result("INFO", "rx_noise", "%u", noise);
  result("INFO", "rx_lost", "%u", lost);

  dma_platform_init();
  read_only_dma_status("dma_tx_status", dma_get_tx_status());
  read_only_dma_status("dma_rx_status", dma_get_rx_status());
}

static void read_only_linux(){
  struct utsname u;
  if (uname(&u) == 0)
    result("INFO", "linux_kernel", "%s", u.release);

  struct ifaddrs * ifs;
  const char * ip = "none";
  char buf[INET_ADDRSTRLEN];
  if (getifaddrs(&ifs) == 0){
    for (struct ifaddrs * i = ifs; i != NULL; i = i->ifa_next){
      if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, "eth0") == 0){
        inet_ntop(AF_INET, &((struct sockaddr_in *) i->ifa_addr)->sin_addr, buf, sizeof(buf));
        ip = buf;
      }
    }
    freeifaddrs(ifs);
  }
  result("INFO", "eth0_address", "%s", ip);
}

// one I2C monitor reading; a failed transfer is reported as such, not as a value:
static void iic_reading(const char * name, const char * fmt, unsigned a, unsigned b, hw_u32_t status){
  if (status != 0)
    result("FAIL", name, "no response (I2C status 0x%x)", (unsigned) status);
  else
    result("INFO", name, fmt, a, b);
}

static void read_only_iic(){
  const char * board[] = {"3V6", "3V3", "3V0"};
  char name[32];
  unsigned a, b;

  for (int i = 0; i < 3; i++){
    snprintf(name, sizeof(name), "board_%s", board[i]);
    iic_platform_clear_status();
    a = iic_mon_vboard_mv(i);
    b = iic_mon_iboard_ma(i);
    iic_reading(name, "%5u mV %5u mA", a, b, iic_platform_status());
  }
  iic_platform_clear_status();
  a = iic_mon_vboard_mv(3);
  b = iic_mon_probe_dn();
  iic_reading("rtd_probe", "%5u mV 0x%04x DN", a, b, iic_platform_status());

  for (int i = 0; i < 10; i++){
    snprintf(name, sizeof(name), "tile_%02d_vdda", i+1);
    iic_platform_clear_status();
    a = iic_mon_vdda_mv(i);
    b = iic_mon_idda_ma(i);
    iic_reading(name, "%5u mV %5u mA", a, b, iic_platform_status());
    snprintf(name, sizeof(name), "tile_%02d_vddd", i+1);
    iic_platform_clear_status();
    a = iic_mon_vddd_mv(i);
    b = iic_mon_iddd_ma(i);
    iic_reading(name, "%5u mV %5u mA", a, b, iic_platform_status());
  }
}

static void read_only(bool no_iic){
  global_status_t gs;
  get_global_status(&gs);

  read_only_jumper();
  read_only_global(&gs);
  read_only_rxtx();
  read_only_linux();
  if (no_iic)
    result("SKIP", "i2c_monitors", "--no_i2c");
  else
    read_only_iic();
}

// *** BLINK ***

// ask the operator a yes/no question:  1 yes, 0 no, -1 no answer (end of input)
static int operator_confirms(const char * question){
  char line[64];
  while (1){
    printf("OPERATOR:  %s [y/n] ", question);
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL){
      printf("\n");
      return -1;
    }
    if (!isatty(fileno(stdin)))
      printf("%s", line);   // echo a scripted answer
    if (line[0] == 'y' || line[0] == 'Y') return 1;
    if (line[0] == 'n' || line[0] == 'N') return 0;
  }
}

// ask the operator for a DMM reading in mV:  1 answered, -1 no answer (end of input)
static int operator_reading_mv(const char * question, int * mv){
  char line[64];
  while (1){
    printf("OPERATOR:  %s [mV] ", question);
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL){
      printf("\n");
      return -1;
    }
    if (!isatty(fileno(stdin)))
      printf("%s", line);   // echo a scripted answer
    char * end;
    long v = strtol(line, &end, 10);
    if (end != line){
      *mv = (int) v;
      return 1;
    }
  }
}

static void blink_check(const char * name, const char * question, void (*blink)(void)){
  blink();
  int answer = operator_confirms(question);
  if (answer < 0)
    result("SKIP", name, "no operator answer");
  else
    result(answer ? "PASS" : "FAIL", name, answer ? "confirmed by operator" : "not seen by operator");
}

static void blink_leds(){
  init_led();
  blink_check("led_trenz_red", "Did the red LED on the Trenz module blink?", blink_red_led);
  blink_check("led_pacman", "Did PACMAN LED-0 to LED-3 blink, one after the other?", blink_pacman_leds);
}

// *** LOOPBACK ***

static void loopback(bool internal, unsigned packets){
  rxtx_check_t r;
  rx_uart_status_t us;
  unsigned frame = 0, noise = 0, lost = 0;

  init_rxtx();
  zero_rxtx_counts();
  rx_set_config_all(internal ? RX_TEST_LOOPBACK : RX_TEST_PIN);
  check_rxtx_loopback(packets, 1, &r);
  rx_set_config_all(RX_CONFIG_DEFAULT);

  for (unsigned i = 0; i < 40; i++){
    get_rx_uart_status(i, &us);
    frame += us.frame_err;
    noise += us.noise;
    lost  += us.lost;
  }

  bool words_ok = (r.words_sent > 0) && (r.words_rcvd == r.words_sent) && !r.tx_timeout && !r.bad_packets;
  result(words_ok ? "PASS" : "FAIL", "loopback_words", "%u of %u received (tx packets %u%s, bad packets %u)",
         r.words_rcvd, r.words_sent, r.tx_packets, r.tx_timeout ? ", TX TIMEOUT" : "", r.bad_packets);
  result((r.bit_errors == 0 && r.words_rcvd > 0) ? "PASS" : "FAIL", "loopback_bit_errors", "%u in %llu bits (%u words)",
         r.bit_errors, 64ULL * r.words_rcvd, r.words_bad);
  result("INFO", "loopback_rx_counters", "frame errors %u, noise %u, lost %u", frame, noise, lost);
  if (r.stale_words || r.other_words)
    result("INFO", "loopback_other", "stale words %u, other words %u", r.stale_words, r.other_words);
  if (internal)
    result("SKIP", "loopback_pins", "--internal_loopback: pins and breakout card not tested");
}

// *** POWER ***

// limits per hardware:  tile rails off, and on at a test DAC code into the breakout card's loads
typedef struct {
  const char * name;
  bool     tiles_defined;     // false:  tile readings are recorded, not judged
  hw_val_t test_dn;           // DAC code for the "on" test
  hw_val_t off_max_mv;
  hw_val_t on_min_mv, on_max_mv;
  hw_val_t on_min_ma, on_max_ma;
} power_limits_t;

static const power_limits_t LIMITS_SIM = {"the simulated board", true,  0x8000, 50, 1150, 1350, 100, 150};
static const power_limits_t LIMITS_1V5 = {"PACMAN 1v5",          false, 0x8000,  0,    0,    0,   0,   0};  // TBD: from a real board

static const char *   BOARD_RAIL[3]       = {"3V6", "3V3", "3V0"};
static const unsigned BOARD_NOMINAL_MV[3] = {3600, 3300, 3000};
#define BOARD_TOLERANCE_PCT 5

static bool within_pct(int v, int ref, int pct){
  return (100*v >= ref*(100-pct)) && (100*v <= ref*(100+pct));
}

static void power(const power_limits_t * lim){
  char name[32], question[96];
  unsigned monitor_mv[3];

  // board rails:  monitors against nominal, then the operator's DMM against the monitors
  iic_platform_clear_status();
  for (int i = 0; i < 3; i++)
    monitor_mv[i] = iic_mon_vboard_mv(i);
  if (iic_platform_status() != 0){
    result("FAIL", "power", "no response from the power monitors (I2C status 0x%x)", (unsigned) iic_platform_status());
    return;
  }
  for (int i = 0; i < 3; i++){
    snprintf(name, sizeof(name), "board_%s", BOARD_RAIL[i]);
    result(within_pct(monitor_mv[i], BOARD_NOMINAL_MV[i], BOARD_TOLERANCE_PCT) ? "PASS" : "FAIL", name,
           "%u mV (nominal %u +/- %d%%)", monitor_mv[i], BOARD_NOMINAL_MV[i], BOARD_TOLERANCE_PCT);
  }
  for (int i = 0; i < 3; i++){
    int mv;
    snprintf(name, sizeof(name), "dmm_%s", BOARD_RAIL[i]);
    snprintf(question, sizeof(question), "DMM:  measure the %s rail (test point TBD).", BOARD_RAIL[i]);
    if (operator_reading_mv(question, &mv) < 0)
      result("SKIP", name, "no operator answer");
    else
      result(within_pct(mv, monitor_mv[i], BOARD_TOLERANCE_PCT) ? "PASS" : "FAIL", name,
             "%d mV (monitor %u mV +/- %d%%)", mv, monitor_mv[i], BOARD_TOLERANCE_PCT);
  }

  global_status_t gs;
  get_global_status(&gs);
  hw_val_t enables = gs.enables;

  // tiles off:  every rail near zero
  set_global_enables(0);
  for (int t = 0; t < 10; t++){
    iic_set_vdda_dn(t, 0);
    iic_set_vddd_dn(t, 0);
  }
  usleep(100000);
  unsigned off_max = 0;
  for (int t = 0; t < 10; t++){
    unsigned a = iic_mon_vdda_mv(t), d = iic_mon_vddd_mv(t);
    if (a > off_max) off_max = a;
    if (d > off_max) off_max = d;
  }
  if (lim->tiles_defined)
    result((off_max <= lim->off_max_mv) ? "PASS" : "FAIL", "tiles_off", "max %u mV (limit %u)", off_max, (unsigned) lim->off_max_mv);
  else
    result("INFO", "tiles_off", "max %u mV", off_max);

  // tiles on at the test code:  voltage and current of each rail.  Not without limits:
  // the test code's voltage on that hardware is not known yet.
  if (!lim->tiles_defined){
    set_global_enables(enables);
    result("SKIP", "power_tiles_on", "no tile limits for %s yet: tile rails not powered", lim->name);
    return;
  }
  set_global_enables(GLOBAL_ENABLE_TILE_POWER | GLOBAL_ENABLE_TILES);
  for (int t = 0; t < 10; t++){
    iic_set_vdda_dn(t, lim->test_dn);
    iic_set_vddd_dn(t, lim->test_dn);
  }
  usleep(100000);
  for (int t = 0; t < 10; t++){
    for (int rail = 0; rail < 2; rail++){
      unsigned mv = rail ? iic_mon_vddd_mv(t) : iic_mon_vdda_mv(t);
      unsigned ma = rail ? iic_mon_iddd_ma(t) : iic_mon_idda_ma(t);
      snprintf(name, sizeof(name), "tile_%02d_%s_on", t+1, rail ? "vddd" : "vdda");
      bool ok = (mv >= lim->on_min_mv) && (mv <= lim->on_max_mv) && (ma >= lim->on_min_ma) && (ma <= lim->on_max_ma);
      result(ok ? "PASS" : "FAIL", name, "%u mV %u mA (limits %u-%u mV, %u-%u mA)", mv, ma,
             (unsigned) lim->on_min_mv, (unsigned) lim->on_max_mv, (unsigned) lim->on_min_ma, (unsigned) lim->on_max_ma);
    }
  }

  // back off, enables as found
  for (int t = 0; t < 10; t++){
    iic_set_vdda_dn(t, 0);
    iic_set_vddd_dn(t, 0);
  }
  set_global_enables(enables);
}

// *** LEMO ***

// front-panel MUX positions:  tiles 1-10 (0-9), DAC (10), off (11)
static void lemo(){
  char name[32], question[96];

  for (int pos = 0; pos < 12; pos++){
    iic_set_mux_front_panel(pos);

    if (pos < 10){
      snprintf(name, sizeof(name), "lemo_tile_%02d", pos+1);
      snprintf(question, sizeof(question), "DMM at the front-panel LEMO, MUX on TILE %d.", pos+1);
    } else if (pos == 10){
      snprintf(name, sizeof(name), "lemo_dac");
      snprintf(question, sizeof(question), "DMM at the front-panel LEMO, MUX on the DAC.");
    } else {
      snprintf(name, sizeof(name), "lemo_off");
      snprintf(question, sizeof(question), "DMM at the front-panel LEMO, MUX off.");
    }
    int mv;
    if (operator_reading_mv(question, &mv) < 0)
      result("SKIP", name, "no operator answer");
    else
      result("INFO", name, "%d mV", mv);
  }
  iic_set_mux_front_panel(11);   // off

  result("SKIP", "lemo_limits", "expected LEMO readings not defined yet: readings recorded only");
}

// *** MAIN ***

static void usage(){
  printf("usage:  pacman_hwcheck <run sets> [modifiers]\n");
  printf("\n");
  printf("run sets (combinable; tests run in a fixed order):\n");
  printf("  --read_only          read-only status report (allowed with J5 installed)\n");
  printf("  --blink              LED blink test (operator confirms)\n");
  printf("  --power              board rails (monitor + DMM), tile rails off and on into loads\n");
  printf("  --loopback           UART loopback, random data, bit errors (breakout card TX<->RX)\n");
  printf("  --lemo               front-panel MUX through the LEMO (DMM readings)\n");
  printf("  --full_checkout      all of the above, in production order; stops at the first FAIL\n");
  printf("modifiers:\n");
  printf("  --no_i2c             skip everything on I2C\n");
  printf("  --sim_i2c            use the simulated board for everything on I2C (development)\n");
  printf("  --internal_loopback  with --loopback: loop back inside the firmware (no pins)\n");
  printf("  --ber_bits N         loopback length in bits (default %d)\n", LOOPBACK_BITS_DEFAULT);
  printf("  --continue           with --full_checkout: keep going after a FAIL\n");
  printf("  --help               this message\n");
  printf("\n");
  printf("A run with anything skipped or substituted ends INCOMPLETE, never PASS.\n");
  printf("With J5 installed (production), only --read_only runs.\n");
  printf("Exit code:  0 PASS, 1 FAIL, 2 INCOMPLETE, 3 usage or refused.\n");
}

int main(int argc, char * argv[]){
  enum { OPT_READ_ONLY = 1, OPT_NO_I2C, OPT_SIM_I2C, OPT_BLINK, OPT_POWER, OPT_LOOPBACK, OPT_LEMO, OPT_FULL,
         OPT_INTERNAL_LOOPBACK, OPT_BER_BITS, OPT_CONTINUE, OPT_HELP };
  static struct option opts[] = {
    {"read_only",         no_argument, 0, OPT_READ_ONLY},
    {"no_i2c",            no_argument, 0, OPT_NO_I2C},
    {"sim_i2c",           no_argument, 0, OPT_SIM_I2C},
    {"help",              no_argument, 0, OPT_HELP},
    {"blink",             no_argument, 0, OPT_BLINK},
    {"power",             no_argument, 0, OPT_POWER},
    {"loopback",          no_argument, 0, OPT_LOOPBACK},
    {"lemo",              no_argument, 0, OPT_LEMO},
    {"full_checkout",     no_argument, 0, OPT_FULL},
    {"internal_loopback", no_argument, 0, OPT_INTERNAL_LOOPBACK},
    {"ber_bits",          required_argument, 0, OPT_BER_BITS},
    {"continue",          no_argument, 0, OPT_CONTINUE},
    {0, 0, 0, 0}
  };

  bool do_read_only = false, do_blink = false, do_power = false, do_loopback = false, do_lemo = false;
  bool full = false, no_iic = false, internal = false, keep_going = false;
  unsigned long ber_bits = LOOPBACK_BITS_DEFAULT;
  int c;
  while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1){
    switch (c){
    case OPT_READ_ONLY: do_read_only = true; break;
    case OPT_NO_I2C:    no_iic = true;       break;
    case OPT_SIM_I2C:   G_SIM_IIC = true;    break;
    case OPT_BLINK:     do_blink = true;     break;
    case OPT_POWER:     do_power = true;     break;
    case OPT_LOOPBACK:  do_loopback = true;  break;
    case OPT_LEMO:      do_lemo = true;      break;
    case OPT_FULL:      full = do_read_only = do_blink = do_power = do_loopback = do_lemo = true; break;
    case OPT_INTERNAL_LOOPBACK: internal = true; break;
    case OPT_BER_BITS:  ber_bits = strtoul(optarg, NULL, 0); break;
    case OPT_CONTINUE:  keep_going = true;   break;
    case OPT_HELP:      usage(); return EXIT_PASS;
    default:
      usage();
      return EXIT_USAGE;
    }
  }
  bool any = do_read_only || do_blink || do_power || do_loopback || do_lemo;
  if (optind < argc || !any || (internal && !do_loopback) || (keep_going && !full) || (no_iic && G_SIM_IIC) || ber_bits == 0){
    usage();
    return EXIT_USAGE;
  }

  printf("pacman_hwcheck:  PACMAN hardware checkout.\n");
  axil_platform_init();
  init_jumper();

  // J5 = production jumper:  only --read_only runs with it installed
  if ((do_blink || do_power || do_loopback || do_lemo) && jumper_installed() != JUMPER_OUT){
    printf("REFUSED:  J5 installed (or unreadable): only --read_only runs on a production board.\n");
    return EXIT_USAGE;
  }

  // I2C:  the PACMAN, the simulated board, or nothing
  global_status_t gs;
  get_global_status(&gs);
  if (G_SIM_IIC){
    iic_set_simulated();
    result("SKIP", "i2c_hardware", "--sim_i2c: simulated board, not the PACMAN");
  } else if (!no_iic){
    iic_platform_init();
    iic_set_hw_version(gs.hardware_major, gs.hardware_minor, gs.hardware_patch);
  }
  const power_limits_t * lim = G_SIM_IIC ? &LIMITS_SIM : &LIMITS_1V5;
  unsigned packets = (ber_bits + LOOPBACK_BITS_PER_PACKET - 1) / LOOPBACK_BITS_PER_PACKET;

  // production order; a full checkout stops after the first section with a FAIL (unless --continue)
  for (int section = 0; section < 5; section++){
    switch (section){
    case 0: if (do_read_only) read_only(no_iic); break;
    case 1: if (do_blink) blink_leds(); break;
    case 2:
      if (do_power){
        if (no_iic) result("SKIP", "power", "--no_i2c");
        else        power(lim);
      }
      break;
    case 3: if (do_loopback) loopback(internal, packets); break;
    case 4:
      if (do_lemo){
        if (no_iic) result("SKIP", "lemo", "--no_i2c");
        else        lemo();
      }
      break;
    }
    if (full && G_NFAIL && !keep_going){
      printf("STOPPED after the first FAIL (--continue to keep going).\n");
      break;
    }
  }

  const char * verdict = G_NFAIL ? "FAIL" : (G_NSKIP ? "INCOMPLETE" : "PASS");
  printf("VERDICT %s  (pass %d, fail %d, skip %d)\n", verdict, G_NPASS, G_NFAIL, G_NSKIP);
  return G_NFAIL ? EXIT_FAIL : (G_NSKIP ? EXIT_INCOMPLETE : EXIT_PASS);
}
