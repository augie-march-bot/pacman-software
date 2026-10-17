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
#include <time.h>
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
#include "iic_menu.h"
#include "atc.h"

#define EXIT_PASS        0
#define EXIT_FAIL        1
#define EXIT_INCOMPLETE  2
#define EXIT_USAGE       3   // also: refused (J5 installed)

#define LOOPBACK_BITS_DEFAULT    25600000   // internal: 10000 packets of 40 UARTs; pins: per tile

static int G_NPASS = 0, G_NFAIL = 0, G_NSKIP = 0;
static bool G_SIM_IIC = false;            // --sim_i2c
static bool G_SKIP_INTERACTIVE = false;   // --skip_interactive
static unsigned G_TILE_MASK = 0x3FF;      // --tile_mask:  bit t = tile t+1 (as GLOBAL_ENABLE_TILES)

static bool tile_in_mask(int t){
  return (G_TILE_MASK >> t) & 1;
}

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
  char build[48];
  get_build_string(gs->build_flags, build, sizeof(build));
  result("INFO", "firmware_build", "%s", build);
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

// operator answers other than a result:  end of input, --skip_interactive, or "s" (skipped by the operator)
#define ANSWER_NONE        -1
#define ANSWER_INTERACTIVE -2
#define ANSWER_SKIPPED     -3

static const char * no_answer(int answer){
  if (answer == ANSWER_INTERACTIVE) return "--skip_interactive";
  if (answer == ANSWER_SKIPPED)     return "skipped by operator";
  return "no operator answer";
}

// prep stage before an operator step:  say what is coming, wait for ready.  1 ready, or ANSWER_* ("s" skips the
// step:  the action is not run and nothing is recorded)
static int operator_ready(const char * prep){
  char line[64];
  if (G_SKIP_INTERACTIVE) return ANSWER_INTERACTIVE;
  while (1){
    printf("OPERATOR:  Next:  %s  Ready? [y = go, s = skip] ", prep);
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL){
      printf("\n");
      return ANSWER_NONE;
    }
    if (!isatty(fileno(stdin)))
      printf("%s", line);   // echo a scripted answer
    if (line[0] == 'y' || line[0] == 'Y') return 1;
    if (line[0] == 's' || line[0] == 'S') return ANSWER_SKIPPED;
  }
}

// ask the operator a yes/no question:  1 yes, 0 no, or ANSWER_* ("s" skips:  nothing recorded)
static int operator_confirms(const char * question){
  char line[64];
  if (G_SKIP_INTERACTIVE) return ANSWER_INTERACTIVE;
  while (1){
    printf("OPERATOR:  %s [y/n, s = skip] ", question);
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL){
      printf("\n");
      return ANSWER_NONE;
    }
    if (!isatty(fileno(stdin)))
      printf("%s", line);   // echo a scripted answer
    if (line[0] == 'y' || line[0] == 'Y') return 1;
    if (line[0] == 'n' || line[0] == 'N') return 0;
    if (line[0] == 's' || line[0] == 'S') return ANSWER_SKIPPED;
  }
}

// ask the operator for a DMM reading in mV:  1 answered, or ANSWER_* ("s" skips:  no value is recorded)
static int operator_reading_mv(const char * question, int * mv){
  char line[64];
  if (G_SKIP_INTERACTIVE) return ANSWER_INTERACTIVE;
  while (1){
    printf("OPERATOR:  %s [mV, s = skip] ", question);
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL){
      printf("\n");
      return ANSWER_NONE;
    }
    if (!isatty(fileno(stdin)))
      printf("%s", line);   // echo a scripted answer
    if (line[0] == 's' || line[0] == 'S') return ANSWER_SKIPPED;
    char * end;
    long v = strtol(line, &end, 10);
    if (end != line){
      *mv = (int) v;
      return 1;
    }
  }
}

static void blink_check(const char * name, const char * prep, const char * question, void (*blink)(void)){
  int answer = operator_ready(prep);
  if (answer < 0){
    result("SKIP", name, no_answer(answer));
    return;
  }
  blink();
  answer = operator_confirms(question);
  if (answer < 0)
    result("SKIP", name, no_answer(answer));
  else
    result(answer ? "PASS" : "FAIL", name, answer ? "confirmed by operator" : "not seen by operator");
}

static void blink_leds(){
  init_led();
  blink_check("led_trenz_red", "watch the red LED on the Trenz module (blinks for 2 s).",
              "Did the red LED on the Trenz module blink?", blink_red_led);
  blink_check("led_pacman", "watch PACMAN LED-0 to LED-3 (each blinks for 2 s, one after the other).",
              "Did PACMAN LED-0 to LED-3 blink, one after the other?", blink_pacman_leds);
}

// *** LOOPBACK ***

static void loopback_counters(const char * name, uint64_t uart_mask){
  rx_uart_status_t us;
  unsigned frame = 0, noise = 0, lost = 0;
  for (unsigned u = 0; u < 40; u++){
    if (!((uart_mask >> u) & 1)) continue;
    get_rx_uart_status(u, &us);
    frame += us.frame_err;
    noise += us.noise;
    lost  += us.lost;
  }
  result("INFO", name, "frame errors %u, noise %u, lost %u", frame, noise, lost);
}

// internal:  all 40 UARTs at once, inside the firmware
static void loopback_internal(unsigned long ber_bits){
  rxtx_check_t r;
  unsigned packets = (ber_bits + 40*64 - 1) / (40*64);

  zero_rxtx_counts();
  rx_set_config_all(RX_TEST_LOOPBACK);
  check_rxtx_loopback(packets, 1, RXTX_ALL_UARTS, &r);
  rx_set_config_all(RX_CONFIG_DEFAULT);

  bool words_ok = (r.words_sent > 0) && (r.words_rcvd == r.words_sent) && !r.tx_timeout && !r.bad_packets;
  result(words_ok ? "PASS" : "FAIL", "loopback_words", "%u of %u received (tx packets %u%s, bad packets %u)",
         r.words_rcvd, r.words_sent, r.tx_packets, r.tx_timeout ? ", TX TIMEOUT" : "", r.bad_packets);
  result((r.bit_errors == 0 && r.words_rcvd > 0) ? "PASS" : "FAIL", "loopback_bit_errors", "%u in %llu bits (%u words)",
         r.bit_errors, 64ULL * r.words_rcvd, r.words_bad);
  loopback_counters("loopback_rx_counters", RXTX_ALL_UARTS);
  if (r.stale_words || r.other_words)
    result("INFO", "loopback_other", "stale words %u, other words %u", r.stale_words, r.other_words);
  result("SKIP", "loopback_pins", "--internal_loopback: pins and breakout card not tested");
}

// through the pins and the breakout card (TX N -> RX N), one tile at a time:  only that tile's enable,
// only its four RX UARTs on (tile t+1 = UARTs 4t+1 to 4t+4), every other RX UART off by config.
// A word on any other UART is a stray word.
static void loopback_pins(unsigned long ber_bits){
  rxtx_check_t r;
  char name[32];
  unsigned packets = (ber_bits + 4*64 - 1) / (4*64);
  unsigned stray = 0;

  global_status_t gs;
  get_global_status(&gs);
  hw_val_t enables = gs.enables;

  for (int t = 0; t < 10; t++){
    snprintf(name, sizeof(name), "tile_%02d_loopback", t+1);
    if (!tile_in_mask(t)){
      result("SKIP", name, "not in --tile_mask 0x%03x", G_TILE_MASK);
      continue;
    }
    uint64_t uarts = 0xFULL << (4*t);
    set_global_enables(1u << t);
    rx_set_config_all(RX_TEST_PIN);
    for (unsigned u = 0; u < 40; u++)
      if (!((uarts >> u) & 1)) rx_disable_uart(u);
    usleep(10000);
    zero_rxtx_counts();
    check_rxtx_loopback(packets, 1 + t, uarts, &r);

    bool words_ok = (r.words_sent > 0) && (r.words_rcvd == r.words_sent) && !r.tx_timeout && !r.bad_packets;
    unsigned u0 = 4*t;
    result(words_ok ? "PASS" : "FAIL", name, "%u of %u received (UARTs %u-%u: %u %u %u %u%s)",
           r.words_rcvd, r.words_sent, u0+1, u0+4, r.rcvd[u0], r.rcvd[u0+1], r.rcvd[u0+2], r.rcvd[u0+3],
           r.tx_timeout ? ", TX TIMEOUT" : "");
    snprintf(name, sizeof(name), "tile_%02d_bit_errors", t+1);
    result((r.bit_errors == 0 && r.words_rcvd > 0) ? "PASS" : "FAIL", name, "%u in %llu bits (%u words)",
           r.bit_errors, 64ULL * r.words_rcvd, r.words_bad);
    snprintf(name, sizeof(name), "tile_%02d_rx_counters", t+1);
    loopback_counters(name, uarts);
    if (r.stray_words){
      char list[128] = "";
      for (unsigned u = 0; u < 40; u++){
        if (((uarts >> u) & 1) || !r.rcvd[u]) continue;
        size_t n = strlen(list);
        snprintf(list + n, sizeof(list) - n, " UART %u: %u", u+1, r.rcvd[u]);
      }
      snprintf(name, sizeof(name), "tile_%02d_stray", t+1);
      result("FAIL", name, "%u words on disabled UARTs:%s", r.stray_words, list);
      stray += r.stray_words;
    }
  }
  rx_set_config_all(RX_CONFIG_DEFAULT);
  set_global_enables(enables);
  if (G_TILE_MASK)
    result(stray ? "FAIL" : "PASS", "loopback_stray", "%u words on disabled UARTs", stray);
}

static void loopback(bool internal, unsigned long ber_bits){
  init_rxtx();
  init_rxtx_descriptor_ring_mode(128);   // once per run; every check below uses this ring
  if (internal)
    loopback_internal(ber_bits);
  else
    loopback_pins(ber_bits);
}

// *** POWER ***

static const char *   BOARD_RAIL[3]       = {"3V6", "3V3", "3V0"};
static const unsigned BOARD_NOMINAL_MV[3] = {3600, 3300, 3000};
#define BOARD_TOLERANCE_PCT 5

// tile rails (first limits, from the PACMAN 1v5 bench 2026-10-16; no ASICs):
#define TILE_OFF_MAX_MV       50   // a rail switched off, or another tile's rail while one tile is driven
#define TILE_SWING_MIN_MV   1000   // rise of a rail over the DAC sweep
#define TILE_MONOTONIC_MV     30   // largest allowed drop from one sweep point to the next
#define TILE_LOAD_MIN_MA       5   // current at the top of the sweep:  a load is present
#define TILE_GAIN_MV         157   // rail gain per 0x1000 DAC codes, above the regulator floor (0x3000-0xF000)
#define TILE_GAIN_PCT          5   // tolerance on the gain
#define TILE_GAIN_FIRST        3   // first sweep point of the gain fit (0x3000)

static bool within_pct(int v, int ref, int pct){
  return (100*v >= ref*(100-pct)) && (100*v <= ref*(100+pct));
}

static void set_all_tile_dacs(hw_val_t dn){
  for (int t = 0; t < 10; t++){
    iic_set_vdda_dn(t, dn);
    iic_set_vddd_dn(t, dn);
  }
}

// largest rail voltage over the tiles, except tile skip (-1: none); *which = its tile
static unsigned max_tile_mv(int skip, int * which){
  unsigned vmax = 0;
  *which = -1;
  for (int t = 0; t < 10; t++){
    if (t == skip) continue;
    unsigned a = iic_mon_vdda_mv(t), d = iic_mon_vddd_mv(t);
    if (a > vmax) { vmax = a; *which = t; }
    if (d > vmax) { vmax = d; *which = t; }
  }
  return vmax;
}

// one rail of a swept tile:  rises steadily over the sweep, into a load; load resistance fitted through 0
static void tile_rail(int t, const char * rail, const unsigned mv[], const unsigned ma[]){
  char name[32];
  unsigned dip = 0;
  double svv = 0, svi = 0;
  for (int k = 0; k < IIC_IV_POINTS; k++){
    if (k > 0 && mv[k] + TILE_MONOTONIC_MV < mv[k-1] && mv[k-1] - mv[k] > dip) dip = mv[k-1] - mv[k];
    svv += (double) mv[k] * mv[k];
    svi += (double) mv[k] * ma[k];
  }
  unsigned swing = (mv[IIC_IV_POINTS-1] > mv[0]) ? mv[IIC_IV_POINTS-1] - mv[0] : 0;
  unsigned top_ma = ma[IIC_IV_POINTS-1];
  int gain = ((int) mv[IIC_IV_POINTS-1] - (int) mv[TILE_GAIN_FIRST]) / (IIC_IV_POINTS-1 - TILE_GAIN_FIRST);
  bool ok = (swing >= TILE_SWING_MIN_MV) && (dip == 0) && (top_ma >= TILE_LOAD_MIN_MA)
         && within_pct(gain, TILE_GAIN_MV, TILE_GAIN_PCT);
  char load[32] = "no load";
  if (svi > 0) snprintf(load, sizeof(load), "load %.1f ohm", svv / svi);
  snprintf(name, sizeof(name), "tile_%02d_%s", t+1, rail);
  result(ok ? "PASS" : "FAIL", name, "%u-%u mV, gain %d mV/0x1000, %u mA at top, %s%s", mv[0], mv[IIC_IV_POINTS-1],
         gain, top_ma, load, dip ? ", NOT MONOTONIC" : "");
}

static void power(){
  char name[32], question[96];
  unsigned monitor_mv[3];

  global_status_t gs;
  get_global_status(&gs);
  hw_val_t enables = gs.enables;

  // everything off:  board rails, and every tile rail near zero
  set_global_enables(0);
  set_all_tile_dacs(0);
  usleep(100000);

  iic_platform_clear_status();
  for (int i = 0; i < 3; i++)
    monitor_mv[i] = iic_mon_vboard_mv(i);
  if (iic_platform_status() != 0){
    result("FAIL", "power", "no response from the power monitors (I2C status 0x%x)", (unsigned) iic_platform_status());
    set_global_enables(enables);
    return;
  }
  for (int i = 0; i < 2; i++){
    snprintf(name, sizeof(name), "board_%s", BOARD_RAIL[i]);
    result(within_pct(monitor_mv[i], BOARD_NOMINAL_MV[i], BOARD_TOLERANCE_PCT) ? "PASS" : "FAIL", name,
           "%u mV (nominal %u +/- %d%%)", monitor_mv[i], BOARD_NOMINAL_MV[i], BOARD_TOLERANCE_PCT);
  }
  result((monitor_mv[2] <= TILE_OFF_MAX_MV) ? "PASS" : "FAIL", "board_3V0_off", "%u mV with tile power off (limit %u)",
         monitor_mv[2], (unsigned) TILE_OFF_MAX_MV);
  int ready = operator_ready("DMM (DC volts) on the board rails 3V6 and 3V3, two readings.");
  for (int i = 0; i < 2; i++){
    int mv;
    snprintf(name, sizeof(name), "dmm_%s", BOARD_RAIL[i]);
    if (ready < 0){
      result("SKIP", name, no_answer(ready));
      continue;
    }
    snprintf(question, sizeof(question), "DMM:  measure the %s rail (test point TBD).", BOARD_RAIL[i]);
    int answer = operator_reading_mv(question, &mv);
    if (answer < 0)
      result("SKIP", name, no_answer(answer));
    else
      result(within_pct(mv, monitor_mv[i], BOARD_TOLERANCE_PCT) ? "PASS" : "FAIL", name,
             "%d mV (monitor %u mV +/- %d%%)", mv, monitor_mv[i], BOARD_TOLERANCE_PCT);
  }
  int which;
  unsigned off_max = max_tile_mv(-1, &which);
  result((off_max <= TILE_OFF_MAX_MV) ? "PASS" : "FAIL", "tiles_off", "max %u mV, all tiles (limit %u)",
         off_max, (unsigned) TILE_OFF_MAX_MV);

  // main tile power:  3V0 comes up
  set_global_enables(GLOBAL_ENABLE_TILE_POWER);
  usleep(100000);
  unsigned v3v0 = iic_mon_vboard_mv(2);
  result(within_pct(v3v0, BOARD_NOMINAL_MV[2], BOARD_TOLERANCE_PCT) ? "PASS" : "FAIL", "board_3V0", "%u mV (nominal %u +/- %d%%)",
         v3v0, BOARD_NOMINAL_MV[2], BOARD_TOLERANCE_PCT);

  // one tile at a time:  main power + that tile's enable, both DACs swept over the range (iic_get_iv_curve);
  // then the tile held at the top while every other tile's rails are read
  for (int t = 0; t < 10; t++){
    snprintf(name, sizeof(name), "tile_%02d_power", t+1);
    if (!tile_in_mask(t)){
      result("SKIP", name, "not in --tile_mask 0x%03x", G_TILE_MASK);
      continue;
    }
    set_global_enables(GLOBAL_ENABLE_TILE_POWER | (1u << t));
    usleep(100000);
    iic_platform_clear_status();
    iic_iv_point_t iv[IIC_IV_POINTS];
    iic_get_iv_curve(t, iv);
    unsigned amv[IIC_IV_POINTS], ama[IIC_IV_POINTS], dmv[IIC_IV_POINTS], dma[IIC_IV_POINTS];
    for (int k = 0; k < IIC_IV_POINTS; k++){
      amv[k] = iv[k].vdda_mv; ama[k] = iv[k].idda_ma;
      dmv[k] = iv[k].vddd_mv; dma[k] = iv[k].iddd_ma;
      printf("DATA  tile_%02d dac 0x%04x  vdda %5u mV %5u mA  vddd %5u mV %5u mA\n", t+1, (unsigned) iv[k].dac_dn,
             amv[k], ama[k], dmv[k], dma[k]);
    }
    iic_set_vdda_dn(t, iv[IIC_IV_POINTS-1].dac_dn);
    iic_set_vddd_dn(t, iv[IIC_IV_POINTS-1].dac_dn);
    usleep(10000);
    unsigned other = max_tile_mv(t, &which);
    iic_set_vdda_dn(t, 0);
    iic_set_vddd_dn(t, 0);
    if (iic_platform_status() != 0){
      result("FAIL", name, "I2C error during the sweep (status 0x%x)", (unsigned) iic_platform_status());
      continue;
    }
    tile_rail(t, "vdda", amv, ama);
    tile_rail(t, "vddd", dmv, dma);
    snprintf(name, sizeof(name), "tile_%02d_isolation", t+1);
    char where[24] = "";
    if (which >= 0) snprintf(where, sizeof(where), " on tile %d", which + 1);
    result((other <= TILE_OFF_MAX_MV) ? "PASS" : "FAIL", name, "other tiles max %u mV%s (limit %u)",
           other, where, (unsigned) TILE_OFF_MAX_MV);
  }

  // back off, enables as found
  set_all_tile_dacs(0);
  set_global_enables(enables);
}

// *** ANALOG I/O (front-panel MUX to the SMA jacks J1001 P, J14 N) ***

// test DAC setting for the DAC position (codes; the expected reading needs the DAC scaling)
#define ANALOG_IO_DAC_VP 0x1000
#define ANALOG_IO_DAC_VN 0x0000

// front-panel MUX positions:  tiles 1-10 (0-9), DAC (10), off (11)
static void analog_io(){
  char name[32], question[128];

  int ready = operator_ready("DMM (DC volts) across the SMA jacks J1001 (P) and J14 (N), 12 readings "
                             "(MUX on tiles 1-10, DAC, off).");
  for (int pos = 0; pos < 12; pos++){
    if (ready >= 0)
      iic_set_mux_front_panel(pos);

    if (pos < 10){
      snprintf(name, sizeof(name), "analog_io_tile_%02d", pos+1);
      snprintf(question, sizeof(question), "DMM across the SMA jacks J1001 (P) and J14 (N), MUX on TILE %d.", pos+1);
    } else if (pos == 10){
      if (ready >= 0)
        iic_set_test_dac_dn(ANALOG_IO_DAC_VP, ANALOG_IO_DAC_VN);
      snprintf(name, sizeof(name), "analog_io_dac");
      snprintf(question, sizeof(question), "DMM across the SMA jacks J1001 (P) and J14 (N), MUX on the DAC (VP 0x%04x, VN 0x%04x).",
               ANALOG_IO_DAC_VP, ANALOG_IO_DAC_VN);
    } else {
      snprintf(name, sizeof(name), "analog_io_off");
      snprintf(question, sizeof(question), "DMM across the SMA jacks J1001 (P) and J14 (N), MUX off.");
    }
    int mv;
    int answer = (ready < 0) ? ready : operator_reading_mv(question, &mv);
    if (answer < 0)
      result("SKIP", name, no_answer(answer));
    else
      result("INFO", name, "%d mV", mv);
  }
  if (ready >= 0){
    iic_set_mux_front_panel(11);   // off
    iic_set_test_dac_dn(0, 0);
  }

  result("SKIP", "analog_io_limits", "expected SMA readings not defined yet: readings recorded only");
}

// *** TIMING INPUTS ***

// Rates on the isolated front-panel inputs, from the ATC input counters (rising edges, debounced, 16 bits,
// saturating):  LEMO A = TRIG_ISO (J18), LEMO B = SYNC_ISO (K18, the PPS input).  Reported as INFO until the
// lab test (function generator on both) sets expected rates.  Clearing also clears the ATC's other counters.
#define TIMING_GATE_US 1000000
#define TIMING_COUNT_MAX 0xFFFF

static double seconds_now(){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static void timing_inputs(){
  static const char * NAME[2] = {"timing_trig_rate", "timing_pps_rate"};
  static const char * NET[2]  = {"TRIG_ISO, LEMO A", "SYNC_ISO, LEMO B"};
  hw_val_t invert = axil_read_register(C_SCOPE_ATC + C_ADDR_ATC_CONFIG_INPUT);

  axil_write_register(C_SCOPE_ATC + C_ADDR_ATC_COUNT_REQ, 0x10);   // clear
  axil_write_register(C_SCOPE_ATC + C_ADDR_ATC_COUNT_REQ, 0x20);   // start
  double t0 = seconds_now();
  usleep(TIMING_GATE_US);
  axil_write_register(C_SCOPE_ATC + C_ADDR_ATC_COUNT_REQ, 0x30);   // stop
  double gate = seconds_now() - t0;

  for (int i = 0; i < 2; i++){
    axil_write_register(C_SCOPE_ATC + C_ADDR_ATC_COUNT_REQ, 0x70 + i);   // read input counter i
    unsigned count = axil_read_register(C_SCOPE_ATC + C_ADDR_ATC_COUNT) & 0xFFFF;
    const char * inv = ((invert >> i) & 1) ? ", input inverted (ATC CONFIG_INPUT)" : "";
    if (count >= TIMING_COUNT_MAX)
      result("INFO", NAME[i], "saturated: >= %u edges in %.3f s (> %.0f Hz) (%s%s)", count, gate, count / gate, NET[i], inv);
    else
      result("INFO", NAME[i], "%.1f Hz (%u edges in %.3f s) (%s%s)", count / gate, count, gate, NET[i], inv);
  }
  axil_write_register(C_SCOPE_ATC + C_ADDR_ATC_COUNT_REQ, 0x20);   // counters run, as after reset
  result("SKIP", "timing_limits", "expected input rates not defined yet (lab test: function generator on both)");
}

// *** ANALOG I/O MODE ***

// --analog_io_mode none|dac|tile2|tile2_100:  exactly the register writes of toggle_mux() in the bare-metal
// app pacman/apps/hwcheck/hwcheck.c (modes 0-3), P chip then N chip.  The MUX stays set.
//   none:       CMDA/CMDB = 0x10 (no connection)
//   dac:        CMDA/CMDB = 11 (DAC input)
//   tile2:      CMDA/CMDB = 1 (TILE 2 analog monitor)
//   tile2_100:  shadow A/B = AB02 + AB14 (TILE 2 with 100 ohm termination), CMDA/CMDB = 0x11 (copy shadow)
typedef struct { hw_u8_t reg, value; } mux_write_t;

static int analog_io_mode(const char * mode){
  static const mux_write_t none[]      = {{0x14, 0x10}, {0x15, 0x10}};
  static const mux_write_t dac[]       = {{0x14, 11},   {0x15, 11}};
  static const mux_write_t tile2[]     = {{0x14, 1},    {0x15, 1}};
  static const mux_write_t tile2_100[] = {{0x10, 0x02}, {0x11, 0x20}, {0x12, 0x02}, {0x13, 0x20},
                                          {0x14, 0x11}, {0x15, 0x11}};
  const mux_write_t * w;
  int n;
  if      (strcmp(mode, "none") == 0)      { w = none;      n = 2; }
  else if (strcmp(mode, "dac") == 0)       { w = dac;       n = 2; }
  else if (strcmp(mode, "tile2") == 0)     { w = tile2;     n = 2; }
  else if (strcmp(mode, "tile2_100") == 0) { w = tile2_100; n = 6; }
  else { printf("ERROR:  --analog_io_mode none|dac|tile2|tile2_100\n"); return -1; }

  hw_u32_t status = 0;
  for (int side = 0; side < 2; side++){        // P (0x4c) then N (0x4d), as toggle_mux()
    for (int k = 0; k < n; k++){
      iic_platform_clear_status();
      iic_write_mux_reg(side, w[k].reg, w[k].value);
      status |= iic_platform_status();
    }
  }
  result(status ? "FAIL" : "INFO", "analog_io_mode", "%s%s", mode,
         status ? ": I2C write failed" : ": set (SMA jacks J1001 P, J14 N)");
  return 0;
}

// *** MAIN ***

static void usage(){
  printf("usage:  pacman_hwcheck <run sets> [modifiers]\n");
  printf("\n");
  printf("run sets (combinable; tests run in a fixed order):\n");
  printf("  --read_only          read-only status report (allowed with J5 installed)\n");
  printf("  --blink              LED blink test (operator confirms)\n");
  printf("  --power              board rails (monitor + DMM); each tile in the mask swept over the DAC range\n");
  printf("                       into the card's loads, one tile at a time, other tiles checked off\n");
  printf("  --loopback           UART loopback, random data, bit errors (breakout card TX N -> RX N), one tile\n");
  printf("                       at a time:  only that tile enabled, other RX UARTs off; stray words fail\n");
  printf("  --timing_inputs      rates on the TRIG (LEMO A) and PPS (LEMO B) inputs, ATC counters, 1 s gate\n");
  printf("  --analog_io          front-panel MUX to the SMA jacks J1001 (P), J14 (N): DMM readings\n");
  printf("  --full_checkout      all of the above, in production order; stops at the first FAIL\n");
  printf("  --analog_io_mode M   set the front-panel MUX as the bare-metal toggle_mux(); the MUX stays set:\n");
  printf("                       M = none, dac, tile2, tile2_100 (tile 2 with 100 ohm termination)\n");
  printf("modifiers:\n");
  printf("  --no_i2c             skip everything on I2C\n");
  printf("  --sim_i2c            use the simulated board for everything on I2C (development)\n");
  printf("  --internal_loopback  with --loopback: loop back inside the firmware (no pins)\n");
  printf("  --ber_bits N         loopback length in bits, per tile (internal: in all) (default %d)\n", LOOPBACK_BITS_DEFAULT);
  printf("  --tile_mask M        tiles connected on the bench (bit t = tile t+1; default 0x3ff); others SKIP\n");
  printf("  --skip_interactive   skip every operator step (LEDs, DMM readings)\n");
  printf("  --continue           with --full_checkout: keep going after a FAIL\n");
  printf("  --help               this message\n");
  printf("\n");
  printf("A run with anything skipped or substituted ends INCOMPLETE, never PASS.\n");
  printf("Operator steps start with a prep prompt (what comes next; y = go).  At any prompt, s skips the step\n");
  printf("(recorded SKIP, no value).\n");
  printf("With J5 installed (production), only --read_only runs.\n");
  printf("Exit code:  0 PASS, 1 FAIL, 2 INCOMPLETE, 3 usage or refused.\n");
}

int main(int argc, char * argv[]){
  enum { OPT_READ_ONLY = 1, OPT_NO_I2C, OPT_SIM_I2C, OPT_BLINK, OPT_POWER, OPT_LOOPBACK, OPT_ANALOG_IO, OPT_TIMING, OPT_FULL,
         OPT_INTERNAL_LOOPBACK, OPT_BER_BITS, OPT_CONTINUE, OPT_ANALOG_IO_MODE, OPT_TILE_MASK, OPT_SKIP_INTERACTIVE,
         OPT_HELP };
  static struct option opts[] = {
    {"read_only",         no_argument, 0, OPT_READ_ONLY},
    {"no_i2c",            no_argument, 0, OPT_NO_I2C},
    {"sim_i2c",           no_argument, 0, OPT_SIM_I2C},
    {"help",              no_argument, 0, OPT_HELP},
    {"blink",             no_argument, 0, OPT_BLINK},
    {"power",             no_argument, 0, OPT_POWER},
    {"loopback",          no_argument, 0, OPT_LOOPBACK},
    {"analog_io",         no_argument, 0, OPT_ANALOG_IO},
    {"timing_inputs",     no_argument, 0, OPT_TIMING},
    {"full_checkout",     no_argument, 0, OPT_FULL},
    {"internal_loopback", no_argument, 0, OPT_INTERNAL_LOOPBACK},
    {"ber_bits",          required_argument, 0, OPT_BER_BITS},
    {"continue",          no_argument, 0, OPT_CONTINUE},
    {"analog_io_mode",    required_argument, 0, OPT_ANALOG_IO_MODE},
    {"tile_mask",         required_argument, 0, OPT_TILE_MASK},
    {"skip_interactive",  no_argument, 0, OPT_SKIP_INTERACTIVE},
    {0, 0, 0, 0}
  };

  bool do_read_only = false, do_blink = false, do_power = false, do_loopback = false, do_analog_io = false;
  bool do_timing = false;
  bool full = false, no_iic = false, internal = false, keep_going = false;
  unsigned long ber_bits = LOOPBACK_BITS_DEFAULT;
  const char * aio_mode = NULL;
  unsigned long tile_mask = 0x3FF;
  int c;
  while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1){
    switch (c){
    case OPT_READ_ONLY: do_read_only = true; break;
    case OPT_NO_I2C:    no_iic = true;       break;
    case OPT_SIM_I2C:   G_SIM_IIC = true;    break;
    case OPT_BLINK:     do_blink = true;     break;
    case OPT_POWER:     do_power = true;     break;
    case OPT_LOOPBACK:  do_loopback = true;  break;
    case OPT_ANALOG_IO: do_analog_io = true; break;
    case OPT_TIMING:    do_timing = true;    break;
    case OPT_FULL:      full = do_read_only = do_blink = do_power = do_loopback = do_timing = do_analog_io = true; break;
    case OPT_INTERNAL_LOOPBACK: internal = true; break;
    case OPT_BER_BITS:  ber_bits = strtoul(optarg, NULL, 0); break;
    case OPT_CONTINUE:  keep_going = true;   break;
    case OPT_ANALOG_IO_MODE: aio_mode = optarg; break;
    case OPT_TILE_MASK: tile_mask = strtoul(optarg, NULL, 0); break;
    case OPT_SKIP_INTERACTIVE: G_SKIP_INTERACTIVE = true; break;
    case OPT_HELP:      usage(); return EXIT_PASS;
    default:
      usage();
      return EXIT_USAGE;
    }
  }
  bool any = do_read_only || do_blink || do_power || do_loopback || do_timing || do_analog_io || aio_mode;
  if (optind < argc || !any || (internal && !do_loopback) || (keep_going && !full) || (no_iic && G_SIM_IIC) || ber_bits == 0
      || tile_mask == 0 || tile_mask > 0x3FF){
    usage();
    return EXIT_USAGE;
  }

  G_TILE_MASK = tile_mask;
  printf("pacman_hwcheck:  PACMAN hardware checkout.  Tile mask 0x%03x%s.\n", G_TILE_MASK,
         G_SKIP_INTERACTIVE ? ", operator steps skipped" : "");
  axil_platform_init();
  init_jumper();

  // J5 = production jumper:  only --read_only runs with it installed
  if ((do_blink || do_power || do_loopback || do_timing || do_analog_io || aio_mode) && jumper_installed() != JUMPER_OUT){
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

  if (aio_mode){
    if (no_iic || G_SIM_IIC){
      printf("ERROR:  --analog_io_mode needs the PACMAN's I2C.\n");
      return EXIT_USAGE;
    }
    if (analog_io_mode(aio_mode) < 0)
      return EXIT_USAGE;
  }

  // production order; a full checkout stops after the first section with a FAIL (unless --continue)
  for (int section = 0; section < 6; section++){
    switch (section){
    case 0: if (do_read_only) read_only(no_iic); break;
    case 1: if (do_blink) blink_leds(); break;
    case 2:
      if (do_power){
        if (no_iic) result("SKIP", "power", "--no_i2c");
        else        power();
      }
      break;
    case 3: if (do_loopback) loopback(internal, ber_bits); break;
    case 4: if (do_timing) timing_inputs(); break;
    case 5:
      if (do_analog_io){
        if (no_iic) result("SKIP", "analog_io", "--no_i2c");
        else        analog_io();
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
