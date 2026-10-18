// pacman_fwcheck:  the firmware requirements (pacman-firmware ghdl/agents/requirements.txt, the "sim+hw" ones),
// checked in the installed firmware on the Trenz module:  no tiles, no external signals, no data.
//
// One source for Linux (-D_LINUX, /dev/mem) and bare metal (standalone BSP), on the pacman-apps drivers.
//
// Output:  one line per check, "PASS|FAIL <requirement>  <what>";  then one line per requirement (PASS, FAIL, or
// NONE when nothing could be checked) and a final VERDICT line.  Exit code (Linux):  0 PASS, 1 FAIL, 3 refused.
//
// Safety:  the read-only checks always run.  The others write registers and send data through the TX -> RX
// loopback inside the firmware;  they run only with J5 out, tile power and every tile off, and (Linux) no pacman
// server running.  Every register written is restored at the end, except the counters (RX, TX and timing unit:
// zeroed or advanced), the timestamp (restarted by the SYNC check), and the DMA rings (reset).
//
// Written by augie (AI agent).

#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "hw_access.h"
#include "global.h"
#include "rxtx.h"
#include "rxtx_menu.h"
#include "dma.h"
#include "atc.h"
#include "jumper.h"

#ifdef _LINUX
  #include <time.h>
  #include <dirent.h>
  #include <signal.h>
#else
  #include "xtime_l.h"
#endif

#define EXIT_PASS     0
#define EXIT_FAIL     1
#define EXIT_INCOMPLETE 2
#define EXIT_REFUSED  3

#define NUART  40
#define UNDEF  0xEEEEEEEEu        // what the bridge returns for a read nobody acknowledges
#define RX_COMMON (SCOPE_RX + UART_GLOBAL)
#define TX_COMMON (SCOPE_TX + UART_GLOBAL)
#define RX_UART(u, reg) (SCOPE_RX + ((u) << 8) + (reg))
#define TX_UART(u, reg) (SCOPE_TX + ((u) << 8) + (reg))
#define ATC(reg)  (C_SCOPE_ATC + (reg))
#define GLB(reg)  (SCOPE_GLOBAL + (reg))

// ------------------------------------------------------------------------------------------------------------
// requirements and results

typedef struct { const char * id; const char * text; unsigned checks, fails; } req_t;

static req_t REQS[] = {
  {"REG-1.1", "each unit acknowledges its own defined registers"},
  {"REG-1.2", "undefined addresses, other scopes, read-only and broadcast reads:  no acknowledge, no change"},
  {"REG-1.3", "an unacknowledged request completes after the timeout;  the bus keeps working"},
  {"REG-2.1", "every read/write register reads back every value written"},
  {"REG-2.2", "a write changes only the register addressed"},
  {"REG-2.3", "a broadcast write sets the register of all 40 UARTs"},
  {"GLB-2.1", "STATUS reads 0x0000CAFE"},
  {"GLB-2.2", "the version registers identify the firmware"},
  {"TX-1.1",  "the channels of the mask send their own word;  the others stay idle"},
  {"TX-1.2",  "all 40 channels can send at once"},
  {"TX-1.3",  "back-to-back packets are sent in order, no word dropped or repeated"},
  {"TX-2.1",  "a frame is start bit, 64 data bits, stop bit:  66 bit periods"},
  {"TX-3.1",  "STARTS and BEATS count frames and hand-overs;  ZERO_CNTS clears them"},
  {"TX-3.2",  "LOOK shows the word last loaded"},
  {"RX-1.1",  "a word comes out as header, timestamp, data, then the trailer"},
  {"RX-1.2",  "words on several channels at once:  channel order, one packet"},
  {"RX-1.3",  "every data bit arrives as sent"},
  {"RX-1.4",  "the header carries word type, PACMAN id, channel id"},
  {"RX-2.4",  "MODE 0 turns a channel off;  INPUT selects the line"},
  {"RX-3.2",  "BUFFER_CONFIG's maximum closes a packet"},
  {"RX-3.3",  "BUFFER_CONFIG's timeout lets later words join the open packet"},
  {"RX-4.1",  "heartbeat words every HEARTBEAT_CONFIG clocks"},
  {"RX-4.3",  "a word for each marker 2 and 3 pulse"},
  {"RX-5.1",  "a channel on the test pattern receives the pattern word"},
  {"RX-6.1",  "the RX counters count their events;  ZERO_CNTS clears them"},
  {"RX-6.2",  "LOOK shows the last word received"},
  {"ATC-1.2", "the timestamp counts UART periods, and reaches the RX unit"},
  {"ATC-1.3", "marker 0 (SYNC) restarts the timestamp"},
  {"ATC-2.2", "a stimulus reaches the markers of its destination"},
  {"ATC-5.1", "the counters latch edges;  START, STOP, CLEAR act on all"},
};
#define NREQ (sizeof(REQS) / sizeof(REQS[0]))

static req_t * G_REQ = NULL;
static int G_FAILS = 0;
static int G_VERBOSE = 0;

static void req(const char * id){
  G_REQ = NULL;
  for (unsigned i = 0; i < NREQ; i++)
    if (strcmp(REQS[i].id, id) == 0) G_REQ = &REQS[i];
  if (!G_REQ) printf("INTERNAL:  unknown requirement %s\r\n", id);
}

// one check:  counted under the current requirement;  printed when it fails (all with -v)
static int check(int ok, const char * fmt, ...){
  char msg[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  if (G_REQ){
    G_REQ->checks++;
    if (!ok) G_REQ->fails++;
  }
  if (!ok) G_FAILS++;
  if (!ok || G_VERBOSE)
    printf("%s %-8s %s\r\n", ok ? "PASS" : "FAIL", G_REQ ? G_REQ->id : "?", msg);
  return ok;
}

static int check_eq(hw_val_t got, hw_val_t want, const char * what){
  return check(got == want, "%s:  0x%08x (expected 0x%08x)", what, (unsigned) got, (unsigned) want);
}

// ------------------------------------------------------------------------------------------------------------
// registers, time

static hw_val_t rd(hw_addr_t a)              { return axil_read_register(a); }
static void     wr(hw_addr_t a, hw_val_t v)  { axil_write_register(a, v); }

static uint64_t now_us(void){
#ifdef _LINUX
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t) t.tv_sec * 1000000u + t.tv_nsec / 1000;
#else
  XTime t;
  XTime_GetTime(&t);
  return (uint64_t) t / (COUNTS_PER_SECOND / 1000000);
#endif
}

// the low 32 bits of the timestamp, in UART periods
static hw_val_t timestamp(void) { return rd(ATC(C_ADDR_ATC_TIMESTAMP)); }

// saved registers, restored at the end
#define MAX_SAVED 160
static struct { hw_addr_t a; hw_val_t v; } G_SAVED[MAX_SAVED];
static unsigned G_NSAVED = 0;

static void save(hw_addr_t a){
  if (G_NSAVED < MAX_SAVED){
    G_SAVED[G_NSAVED].a = a;
    G_SAVED[G_NSAVED].v = rd(a);
    G_NSAVED++;
  }
}

static void restore_all(void){
  for (unsigned i = G_NSAVED; i-- > 0; )
    wr(G_SAVED[i].a, G_SAVED[i].v);
}

#ifdef _LINUX
// interrupted (Ctrl-C, kill, the deadline):  restore the registers before leaving
static void on_signal(int sig){
  restore_all();
  static const char m[] = "\r\nINTERRUPTED:  registers restored\r\n";
  if (write(1, m, sizeof(m) - 1) < 0) {}
  _exit(sig == SIGALRM ? EXIT_FAIL : EXIT_REFUSED);
}
#endif

static void save_all(void){
  static const hw_addr_t glb[] = {C_ADDR_GLOBAL_SCRATCH_A, C_ADDR_GLOBAL_SCRATCH_B};
  static const hw_addr_t rxc[] = {C_ADDR_RX_BUFFER_CONFIG, C_ADDR_RX_BUFFER_ENABLES, C_ADDR_RX_PACMAN,
                                  C_ADDR_RX_WORD_TYPE_LUT, C_ADDR_RX_HEARTBEAT_CONFIG, C_ADDR_RX_ROLLOVER_CONFIG,
                                  C_ADDR_RX_HEADER_A, C_ADDR_RX_HEADER_B, C_ADDR_RX_HEADER_C, C_ADDR_RX_HEADER_D,
                                  C_ADDR_RX_EOP_HEADER, C_ADDR_RX_LOOK_SELECT, C_ADDR_RX_PATTERN_A,
                                  C_ADDR_RX_PATTERN_B, C_ADDR_RX_PATTERN_DELAY, C_ADDR_RX_PATTERN_CONFIG};
  static const hw_addr_t atc[] = {C_ADDR_ATC_DST_LEMO_A, C_ADDR_ATC_DST_LEMO_B, C_ADDR_ATC_DST_POKE_A,
                                  C_ADDR_ATC_DST_POKE_B, C_ADDR_ATC_DST_POKE_C, C_ADDR_ATC_DST_POKE_D,
                                  C_ADDR_ATC_DST_LOGIC_A, C_ADDR_ATC_DST_LOGIC_B};
  for (unsigned i = 0; i < sizeof(glb)/sizeof(glb[0]); i++) save(GLB(glb[i]));
  for (unsigned i = 0; i < sizeof(rxc)/sizeof(rxc[0]); i++) save(RX_COMMON + rxc[i]);
  for (unsigned i = 0; i < sizeof(atc)/sizeof(atc[0]); i++) save(ATC(atc[i]));
  save(TX_COMMON + C_ADDR_TX_LOOK_SELECT);
  for (unsigned u = 0; u < NUART; u++){
    save(RX_UART(u, C_ADDR_RX_UART_CONFIG));
    save(RX_UART(u, C_ADDR_RX_UART_CHAN));
    save(TX_UART(u, C_ADDR_TX_UART_CONFIG));
  }
}

// ------------------------------------------------------------------------------------------------------------
// the stream:  DMA packets out (TX), RX words and trailers in

typedef struct { hw_val_t header, ts, data_lo, data_hi; int packet; } word_t;
typedef struct { hw_val_t eop, count; unsigned words; } trailer_t;

#define MAX_WORDS   2048
#define MAX_PACKETS 512
static word_t    G_W[MAX_WORDS];
static trailer_t G_T[MAX_PACKETS];
static unsigned  G_NW, G_NP, G_BAD;

static uint64_t data64(const word_t * w) { return ((uint64_t) w->data_hi << 32) | w->data_lo; }
static unsigned chan_of(const word_t * w) { return (w->header >> 16) & 0xFFFF; }

// queue one DMA packet;  nothing moves until start_tx()
static void queue_packet(uint64_t mask, const uint64_t words[NUART]){
  hw_addr_t bd = 0;
  for (unsigned tries = 0; !dma_next_available_tx_bd(&bd); tries++){
    if (tries > 100000){ printf("ERROR:  no TX buffer free\r\n"); return; }
  }
  hw_ptr_t b = dma_get_buffer(bd);
  b[0] = (hw_u32_t) mask;
  b[1] = (hw_u32_t) (mask >> 32);
  for (unsigned u = 0; u < NUART; u++){
    b[TX_HEADER_U32_WORDS + 2*u]     = (hw_u32_t) words[u];
    b[TX_HEADER_U32_WORDS + 2*u + 1] = (hw_u32_t) (words[u] >> 32);
  }
  HW_FLUSH_DCACHE(b, TX_PACKET_BYTES);
  dma_add_tx_bd(bd);
}

static void start_tx(void) { dma_tx_batch(); }

static void send_packet(uint64_t mask, const uint64_t words[NUART]){
  queue_packet(mask, words);
  start_tx();
}

// collect every RX packet arriving within wait_us (and until quiet for quiet_us) into G_W / G_T
static void collect(unsigned wait_us, unsigned quiet_us){
  G_NW = G_NP = G_BAD = 0;
  uint64_t start = now_us(), last = start;
  while (1){
    hw_addr_t bd = 0;
    int any = 0;
    while (dma_next_available_rx_bd(&bd)){
      any = 1;
      unsigned n = dma_poll_bd_transferred(bd);
      hw_ptr_t b = dma_get_buffer(bd);
      HW_INVALIDATE_DCACHE(b, n);
      if (n < RX_TRAILER_BYTES || (n - RX_TRAILER_BYTES) % RX_WORD_BYTES){
        G_BAD++;
      } else {
        unsigned nw = (n - RX_TRAILER_BYTES) / RX_WORD_BYTES;
        for (unsigned i = 0; i < nw && G_NW < MAX_WORDS; i++){
          hw_ptr_t p = b + i * (RX_WORD_BYTES / 4);
          word_t * w = &G_W[G_NW++];
          w->header = p[0];  w->ts = p[2];  w->data_lo = p[4];  w->data_hi = p[5];  w->packet = G_NP;
        }
        if (G_NP < MAX_PACKETS){
          hw_ptr_t t = b + nw * (RX_WORD_BYTES / 4);
          G_T[G_NP].eop = t[0];  G_T[G_NP].count = t[2];  G_T[G_NP].words = nw;
          G_NP++;
        }
      }
      dma_add_rx_bd(bd);
      dma_rx_batch();
    }
    uint64_t t = now_us();
    if (any) last = t;
    if (t - start >= wait_us && t - last >= quiet_us) break;
  }
}

static void drain(void) { collect(2000, 2000); }

// the words received on one RX channel id
static unsigned words_on(unsigned chan_id, const word_t * out[], unsigned max){
  unsigned n = 0;
  for (unsigned i = 0; i < G_NW; i++)
    if (chan_of(&G_W[i]) == chan_id && n < max) out[n++] = &G_W[i];
  return n;
}

// a recognisable word:  UART u, packet k
static uint64_t word(unsigned u, unsigned k){
  return 0xF0C0000000000000ULL | ((uint64_t) k << 32) | ((uint64_t) u << 16) | 0xA5A5;
}

static void words_for(unsigned k, uint64_t w[NUART]) { for (unsigned u = 0; u < NUART; u++) w[u] = word(u, k); }

// RX:  every UART off except those of mask, on the loopback;  channel ids UART + 1;  PACMAN id
#define PACMAN_ID 0x5A
static void rx_loopback(uint64_t mask){
  for (unsigned u = 0; u < NUART; u++){
    wr(RX_UART(u, C_ADDR_RX_UART_CONFIG), ((mask >> u) & 1) ? RX_TEST_LOOPBACK
                                                            : RX_CONFIG(0x0A, 0, RX_MODE_EDGE, RX_INPUT_OFF, 4));
    wr(RX_UART(u, C_ADDR_RX_UART_CHAN), u + 1);
  }
  wr(RX_COMMON + C_ADDR_RX_PACMAN, PACMAN_ID);
  wr(RX_COMMON + C_ADDR_RX_WORD_TYPE_LUT, 0x44444444);
  wr(RX_COMMON + C_ADDR_RX_BUFFER_ENABLES, 0);
  wr(RX_COMMON + C_ADDR_RX_BUFFER_CONFIG, RX_BUFFER_CONFIG(0, 1));
}

static hw_val_t data_header(unsigned type, unsigned u) { return ((u + 1) << 16) | (PACMAN_ID << 8) | type; }

// ------------------------------------------------------------------------------------------------------------
// read-only checks

static void check_identity(void){
  global_status_t gs;
  char date[32], hash[32], build[48];
  get_global_status(&gs);
  get_synthesis_date_string(date, sizeof(date));
  get_git_hash_string(hash, sizeof(hash));
  get_build_string(gs.build_flags, build, sizeof(build));
  printf("INFO     firmware %u.%u.%u for hardware %u.%u.%u, built %s, git %s, %s, Vivado %u.%u\r\n",
         (unsigned) gs.firmware_major, (unsigned) gs.firmware_minor, (unsigned) gs.firmware_patch,
         (unsigned) gs.hardware_major, (unsigned) gs.hardware_minor, (unsigned) gs.hardware_patch,
         date, hash, build, (unsigned) gs.vivado_major, (unsigned) gs.vivado_minor);

  req("GLB-2.1");
  check_eq(gs.status, 0x0000CAFE, "STATUS");

  req("GLB-2.2");
  check(gs.firmware_major == 3, "firmware major 3 (zipline):  %u", (unsigned) gs.firmware_major);
  check(gs.hardware_major == 1 && gs.hardware_minor == 5, "hardware 1.5:  %u.%u",
        (unsigned) gs.hardware_major, (unsigned) gs.hardware_minor);
  check(gs.synthesis_date > 1700000000u && gs.synthesis_date < 2500000000u, "synthesis date plausible:  %u",
        (unsigned) gs.synthesis_date);
  int ascii = 1;
  hw_val_t h[2] = {gs.git_hash_upper, gs.git_hash_lower};
  for (int i = 0; i < 2; i++)
    for (int b = 0; b < 4; b++){
      unsigned c = (h[i] >> (8 * b)) & 0xFF;
      if (c < 0x20 || c > 0x7E) ascii = 0;
    }
  check(ascii, "git hash is ASCII:  0x%08x 0x%08x", (unsigned) h[0], (unsigned) h[1]);
  check((gs.build_flags & ~GLOBAL_BUILD_FLAGS_MASK) == 0, "build flags:  only bits 1-0:  0x%08x",
        (unsigned) gs.build_flags);
  check(gs.vivado_major >= 2018 && gs.vivado_major < 2100, "Vivado major plausible:  %u", (unsigned) gs.vivado_major);
}

static void check_bus_read_only(void){
  req("REG-1.1");
  check(rd(GLB(C_ADDR_GLOBAL_FIRMWARE_MAJOR)) != UNDEF, "global answers (FIRMWARE_MAJOR)");
  check(rd(TX_UART(0, C_ADDR_TX_UART_CONFIG)) != UNDEF, "TX answers (UART_CONFIG 0)");
  check(rd(RX_UART(0, C_ADDR_RX_UART_CONFIG)) != UNDEF, "RX answers (UART_CONFIG 0)");
  check(rd(RX_COMMON + C_ADDR_RX_FIFO_CNT) != UNDEF, "RX answers (FIFO_CNT)");
  check(rd(ATC(C_ADDR_ATC_CONFIG_UART)) != UNDEF, "timing unit answers (CONFIG_UART)");

  req("REG-1.2");
  static const struct { hw_addr_t a; const char * what; } undef[] = {
    {0xF004, "undefined global register"}, {0xFFFC, "undefined global register"},
    {0x3FFC, "undefined TX register"},     {0x7FFC, "undefined RX register"},
    {0xEFFC, "undefined timing register"}, {0x9000, "reserved scope 0x9"},
    {0x3B04, "TX broadcast read"},         {0x7B04, "RX broadcast read"},
    {ATC(C_ADDR_ATC_POKE_A), "write-only POKE_A read"},
  };
  for (unsigned i = 0; i < sizeof(undef)/sizeof(undef[0]); i++)
    check_eq(rd(undef[i].a), UNDEF, undef[i].what);

  req("REG-1.3");
  check_eq(rd(GLB(C_ADDR_GLOBAL_STATUS)), 0x0000CAFE, "bus works after the timeouts (STATUS)");
  for (int i = 0; i < 20; i++) rd(0x9000 + 4 * i);
  check_eq(rd(GLB(C_ADDR_GLOBAL_STATUS)), 0x0000CAFE, "bus works after 20 more timeouts");
}

static void check_timestamp_rate(void){
  req("ATC-1.2");
  hw_val_t period = rd(ATC(C_ADDR_ATC_CONFIG_UART));
  if (!check(period > 0 && period < 1000, "CONFIG_UART sensible:  %u", (unsigned) period)) return;
  uint64_t t0 = now_us();
  hw_val_t s0 = timestamp();
  while (now_us() - t0 < 20000) ;
  hw_val_t s1 = timestamp();
  uint64_t t1 = now_us();
  double expect = (double) (t1 - t0) * 100.0 / period;      // 100 clocks per us
  double got = (double) (hw_val_t) (s1 - s0);
  check(got > 0.95 * expect - 200 && got < 1.05 * expect + 200,
        "timestamp advances %.0f in %u us (expected %.0f at CONFIG_UART %u)",
        got, (unsigned) (t1 - t0), expect, (unsigned) period);
}

// ------------------------------------------------------------------------------------------------------------
// register checks (writes)

static void check_bus_writes(void){
  static const hw_val_t P[] = {0xFFFFFFFF, 0xAAAAAAAA, 0x55555555, 0x12345678, 0x80000001, 0x00000000};
  static const struct { hw_addr_t a; hw_val_t mask; const char * what; } RW[] = {
    {GLB(C_ADDR_GLOBAL_SCRATCH_A),                0xFFFFFFFF, "global SCRATCH_A"},
    {GLB(C_ADDR_GLOBAL_SCRATCH_B),                0xFFFFFFFF, "global SCRATCH_B"},
    {RX_COMMON + C_ADDR_RX_HEADER_D,              0xFFFFFFFF, "RX HEADER_D"},
    {RX_COMMON + C_ADDR_RX_ROLLOVER_CONFIG,       0xFFFFFFFF, "RX ROLLOVER_CONFIG"},
    {RX_COMMON + C_ADDR_RX_PATTERN_A,             0xFFFFFFFF, "RX PATTERN_A"},
    {RX_UART(39, C_ADDR_RX_UART_CHAN),            0x0000FFFF, "RX UART_CHAN 39"},
    {TX_COMMON + C_ADDR_TX_LOOK_SELECT,           0x0000003F, "TX LOOK_SELECT"},
    {ATC(C_ADDR_ATC_DST_LOGIC_A),                 0xFFFFFFFF, "timing DST_LOGIC_A"},
    {ATC(C_ADDR_ATC_DST_LOGIC_B),                 0xFFFFFFFF, "timing DST_LOGIC_B"},
  };
  const unsigned N = sizeof(RW) / sizeof(RW[0]);

  req("REG-2.1");
  for (unsigned r = 0; r < N; r++)
    for (unsigned p = 0; p < sizeof(P)/sizeof(P[0]); p++){
      wr(RW[r].a, P[p]);
      char what[64];
      snprintf(what, sizeof(what), "%s read back", RW[r].what);
      check_eq(rd(RW[r].a), P[p] & RW[r].mask, what);
    }

  req("REG-2.2");
  for (unsigned r = 0; r < N; r++) wr(RW[r].a, (0x0100 + r * 0x0101) & RW[r].mask);
  for (unsigned r = 0; r < N; r++){
    wr(RW[r].a, 0xFFFFFFFF & RW[r].mask);
    for (unsigned o = 0; o < N; o++)
      if (o != r) check_eq(rd(RW[o].a), (0x0100 + o * 0x0101) & RW[o].mask, RW[o].what);
    wr(RW[r].a, (0x0100 + r * 0x0101) & RW[r].mask);
  }

  req("REG-1.2");
  hw_val_t a = rd(GLB(C_ADDR_GLOBAL_SCRATCH_A));
  wr(GLB(C_ADDR_GLOBAL_FIRMWARE_MAJOR), 0x5A5A5A5A);
  check_eq(rd(GLB(C_ADDR_GLOBAL_FIRMWARE_MAJOR)), 3, "FIRMWARE_MAJOR unchanged by a write");
  wr(GLB(C_ADDR_GLOBAL_STATUS), 0x5A5A5A5A);
  check_eq(rd(GLB(C_ADDR_GLOBAL_STATUS)), 0x0000CAFE, "STATUS unchanged by a write");
  wr(0xF004, 0x5A5A5A5A);
  wr(0x9020, 0x5A5A5A5A);
  wr(0xE020, 0x5A5A5A5A);
  check_eq(rd(GLB(C_ADDR_GLOBAL_SCRATCH_A)), a, "SCRATCH_A unchanged by undefined and other-scope writes");

  req("REG-2.3");
  wr(SCOPE_RX + UART_BROADCAST + C_ADDR_RX_UART_CONFIG, RX_CONFIG(0x0A, 0, RX_MODE_EDGE, RX_INPUT_OFF, 5));
  for (unsigned u = 0; u < NUART; u++)
    check_eq(rd(RX_UART(u, C_ADDR_RX_UART_CONFIG)), RX_CONFIG(0x0A, 0, RX_MODE_EDGE, RX_INPUT_OFF, 5),
             "RX broadcast UART_CONFIG");
  hw_val_t tx0 = rd(TX_UART(0, C_ADDR_TX_UART_CONFIG));
  wr(SCOPE_TX + UART_BROADCAST + C_ADDR_TX_UART_CONFIG, 0x00000005);
  for (unsigned u = 0; u < NUART; u++)
    check_eq(rd(TX_UART(u, C_ADDR_TX_UART_CONFIG)), 0x00000005, "TX broadcast UART_CONFIG");
  wr(SCOPE_TX + UART_BROADCAST + C_ADDR_TX_UART_CONFIG, tx0);
}

// ------------------------------------------------------------------------------------------------------------
// data path:  TX -> loopback -> RX -> DMA

static void check_one_packet(void){
  const uint64_t mask = (1ULL << 0) | (1ULL << 5) | (1ULL << 39);
  uint64_t w[NUART];
  rx_loopback(0xFFFFFFFFFFULL);
  zero_rxtx_counts();
  drain();
  words_for(1, w);
  hw_val_t ts0 = timestamp();
  send_packet(mask, w);
  collect(2000, 1000);
  hw_val_t ts1 = timestamp();

  req("TX-1.1");
  check(G_NW == 3, "3 words received for a mask of 3 UARTs:  %u", G_NW);
  for (unsigned i = 0; i < G_NW; i++){
    unsigned u = chan_of(&G_W[i]) - 1;
    check(u < NUART && ((mask >> u) & 1), "word from a UART of the mask:  channel id %u", chan_of(&G_W[i]));
    if (u < NUART) check(data64(&G_W[i]) == w[u], "UART %u sent its own word", u);
  }

  req("RX-1.2");
  check(G_NP == 1, "one packet:  %u", G_NP);
  if (G_NW == 3){
    check(chan_of(&G_W[0]) == 1 && chan_of(&G_W[1]) == 6 && chan_of(&G_W[2]) == 40,
          "channel order 0, 5, 39:  ids %u %u %u", chan_of(&G_W[0]), chan_of(&G_W[1]), chan_of(&G_W[2]));
  }

  req("RX-1.1");
  for (unsigned i = 0; i < G_NW; i++){
    unsigned u = chan_of(&G_W[i]) - 1;
    check_eq(G_W[i].header, data_header(0x44, u), "data header");
    check((hw_val_t) (G_W[i].ts - ts0) <= (hw_val_t) (ts1 - ts0), "timestamp 0x%08x within the send window",
          (unsigned) G_W[i].ts);
  }
  check(G_BAD == 0, "no malformed DMA packet:  %u", G_BAD);
  if (G_NP == 1){
    check_eq(G_T[0].eop, (PACMAN_ID << 8) | 0x4C, "trailer:  EOP header with PACMAN id");
    check_eq(G_T[0].count, G_T[0].words, "trailer:  word count");
  }

  req("RX-6.1");
  rx_uart_status_t us;
  get_rx_uart_status(5, &us);
  check(us.starts == 1 && us.updates == 1 && us.beats == 1, "UART 5:  STARTS %u UPDATES %u BEATS %u (1 each)",
        (unsigned) us.starts, (unsigned) us.updates, (unsigned) us.beats);
  check(us.lost == 0 && us.frame_err == 0 && us.noise == 0, "UART 5:  LOST %u FRAME_ERR %u NOISE %u (0)",
        (unsigned) us.lost, (unsigned) us.frame_err, (unsigned) us.noise);
  get_rx_uart_status(4, &us);
  check(us.starts == 0 && us.updates == 0 && us.beats == 0, "UART 4 (not sent):  counters 0");

  req("TX-3.1");
  tx_uart_status_t ts;
  get_tx_uart_status(5, &ts);
  check(ts.starts == 1 && ts.beats == 1, "TX UART 5:  STARTS %u BEATS %u (1 each)", (unsigned) ts.starts,
        (unsigned) ts.beats);
  get_tx_uart_status(4, &ts);
  check(ts.starts == 0 && ts.beats == 0, "TX UART 4 (not in the mask):  0");

  req("RX-6.2");
  wr(RX_COMMON + C_ADDR_RX_LOOK_SELECT, 5);
  check_eq(rd(RX_COMMON + C_ADDR_RX_LOOK_UA), (hw_val_t) w[5], "RX LOOK_UA (UART 5)");
  check_eq(rd(RX_COMMON + C_ADDR_RX_LOOK_UB), (hw_val_t) (w[5] >> 32), "RX LOOK_UB (UART 5)");

  req("TX-3.2");
  wr(TX_COMMON + C_ADDR_TX_LOOK_SELECT, 39);
  check_eq(rd(TX_COMMON + C_ADDR_TX_LOOK_UA), (hw_val_t) w[39], "TX LOOK_UA (UART 39)");
  check_eq(rd(TX_COMMON + C_ADDR_TX_LOOK_UB), (hw_val_t) (w[39] >> 32), "TX LOOK_UB (UART 39)");

  req("RX-6.1");
  zero_rxtx_counts();
  get_rx_uart_status(5, &us);
  check(us.starts == 0 && us.updates == 0 && us.beats == 0, "ZERO_CNTS clears the RX counters");
  req("TX-3.1");
  get_tx_uart_status(5, &ts);
  check(ts.starts == 0 && ts.beats == 0, "ZERO_CNTS clears the TX counters");
}

static void check_all_channels(void){
  uint64_t w[NUART];
  rx_loopback(0xFFFFFFFFFFULL);
  drain();
  words_for(2, w);
  send_packet(0xFFFFFFFFFFULL, w);
  collect(2000, 1000);

  req("TX-1.2");
  check(G_NW == NUART, "40 words for 40 UARTs:  %u", G_NW);
  unsigned good = 0, ordered = 1;
  for (unsigned i = 0; i < G_NW; i++){
    unsigned u = chan_of(&G_W[i]) - 1;
    if (u < NUART && data64(&G_W[i]) == w[u]) good++;
    if (i > 0 && chan_of(&G_W[i]) <= chan_of(&G_W[i-1])) ordered = 0;
  }
  check(good == NUART, "each UART its own word:  %u of 40", good);

  req("RX-1.2");
  check(ordered && G_NP == 1, "40 words in channel order, one packet (%u packets)", G_NP);
}

static void check_bits(void){
  static const uint64_t W[] = {0x8000000000000001ULL, 0x7FFFFFFFFFFFFFFEULL, 0xAAAAAAAAAAAAAAAAULL,
                               0x5555555555555555ULL, 0xFFFFFFFFFFFFFFFFULL, 0x0000000000000000ULL};
  uint64_t w[NUART] = {0};
  uint64_t mask = 0;
  for (unsigned i = 0; i < 6; i++){ w[10 + i] = W[i]; mask |= 1ULL << (10 + i); }
  rx_loopback(mask);
  drain();
  send_packet(mask, w);
  collect(2000, 1000);

  req("RX-1.3");
  check(G_NW == 6, "6 words:  %u", G_NW);
  for (unsigned i = 0; i < G_NW; i++){
    unsigned u = chan_of(&G_W[i]) - 1;
    uint64_t got = data64(&G_W[i]);
    check(u >= 10 && u < 16 && got == w[u], "UART %u:  0x%08x%08x", u, (unsigned) (got >> 32), (unsigned) got);
  }

  // random data, many packets, every UART:  the pacman-apps loopback check
  rx_loopback(0xFFFFFFFFFFULL);
  drain();
  rxtx_check_t r;
  check_rxtx_loopback(1000, 7, RXTX_ALL_UARTS, &r);
  check(r.words_rcvd == 40000 && r.bit_errors == 0, "random loopback:  %u of 40000 words, %u bit errors",
        r.words_rcvd, r.bit_errors);

  req("TX-1.3");
  check(r.words_rcvd == r.words_sent && r.words_bad == 0 && r.stray_words == 0,
        "1000 packets back to back:  %u sent, %u received, %u wrong, %u stray (in order per UART)",
        r.words_sent, r.words_rcvd, r.words_bad, r.stray_words);
}

static void check_frame_length(void){
  uint64_t w[NUART];
  rx_loopback(1ULL << 20);
  drain();
  // queued together, so each next word waits in the TX buffer:  the frames follow each other directly
  for (unsigned k = 0; k < 4; k++){ words_for(10 + k, w); queue_packet(1ULL << 20, w); }
  start_tx();
  collect(3000, 1000);

  req("TX-2.1");
  const word_t * on[8];
  unsigned n = words_on(21, on, 8);
  check(n == 4, "4 back-to-back words on UART 20:  %u", n);
  hw_val_t bit = rd(ATC(C_ADDR_ATC_CONFIG_BAUD)), uart = rd(ATC(C_ADDR_ATC_CONFIG_UART));
  hw_val_t delay = rd(TX_UART(20, C_ADDR_TX_UART_CONFIG)) >> 16;
  for (unsigned i = 1; i < n; i++){
    // frames of 66 + DELAY bit periods;  the timestamp counts UART periods
    hw_val_t dt = on[i]->ts - on[i-1]->ts;
    hw_val_t want = (66 + delay) * bit / uart;
    check(dt + 1 >= want && dt <= want + 1, "frame period %u UART periods (expected %u)", (unsigned) dt,
          (unsigned) want);
  }
}

static void check_header(void){
  uint64_t w[NUART] = {0};
  const uint64_t mask = 0xFULL << 24;
  for (unsigned i = 0; i < 4; i++) w[24 + i] = 0x1234567800000000ULL | (0x10 + i);
  rx_loopback(mask);
  wr(RX_COMMON + C_ADDR_RX_WORD_TYPE_LUT, 0x44434241);
  wr(RX_UART(25, C_ADDR_RX_UART_CHAN), 0xC0DE);
  drain();
  send_packet(mask, w);
  collect(2000, 1000);

  req("RX-1.4");
  check(G_NW == 4, "4 words:  %u", G_NW);
  for (unsigned i = 0; i < G_NW; i++){
    unsigned type = G_W[i].data_lo & 3;
    unsigned id = chan_of(&G_W[i]);
    unsigned u = (id == 0xC0DE) ? 25 : id - 1;
    hw_val_t want = ((hw_val_t) (u == 25 ? 0xC0DE : u + 1) << 16) | (PACMAN_ID << 8) | (0x41 + type);
    check_eq(G_W[i].header, want, "header (type by data bits 1-0, PACMAN id, channel id)");
  }
  wr(RX_COMMON + C_ADDR_RX_WORD_TYPE_LUT, 0x44444444);
  wr(RX_UART(25, C_ADDR_RX_UART_CHAN), 26);
}

static void check_input_select(void){
  uint64_t w[NUART];
  const uint64_t mask = (1ULL << 30) | (1ULL << 31) | (1ULL << 32);
  rx_loopback(mask);
  wr(RX_UART(31, C_ADDR_RX_UART_CONFIG), RX_CONFIG(0x0A, 0, RX_MODE_OFF, RX_INPUT_LOOPBACK, 4));
  wr(RX_UART(32, C_ADDR_RX_UART_CONFIG), RX_CONFIG(0x0A, 0, RX_MODE_EDGE, RX_INPUT_OFF, 4));
  drain();
  words_for(3, w);
  send_packet(mask, w);
  collect(2000, 1000);

  req("RX-2.4");
  const word_t * on[4];
  check(words_on(31, on, 4) == 1, "UART 30 on the loopback:  its word");
  check(words_on(32, on, 4) == 0, "UART 31 in MODE 0:  nothing");
  check(words_on(33, on, 4) == 0, "UART 32 with INPUT 0 (line idle):  nothing");
}

static void check_packets(void){
  uint64_t w[NUART];
  const uint64_t mask = 0x7ULL << 33;      // UARTs 33-35
  rx_loopback(mask);
  wr(RX_COMMON + C_ADDR_RX_BUFFER_CONFIG, RX_BUFFER_CONFIG(2, 1));
  drain();
  words_for(4, w);
  send_packet(mask, w);
  collect(2000, 1000);

  req("RX-3.2");
  check(G_NP == 2 && G_NW == 3, "3 words, maximum 2:  %u packets, %u words", G_NP, G_NW);
  if (G_NP == 2){
    check(G_T[0].words == 2 && G_T[0].count == 2, "first packet:  2 words (count %u)", (unsigned) G_T[0].count);
    check(G_T[1].words == 1 && G_T[1].count == 1, "second packet:  1 word (count %u)", (unsigned) G_T[1].count);
  }

  // two frames back to back on one UART, 6.6 us apart:  one packet with a timeout of 4 (20.5 us), two with 1 (5.1 us)
  rx_loopback(1ULL << 36);
  req("RX-3.3");
  static const unsigned TIMEOUTS[] = {4, 1};
  for (unsigned t = 0; t < 2; t++){
    unsigned timeout = TIMEOUTS[t];
    wr(RX_COMMON + C_ADDR_RX_BUFFER_CONFIG, RX_BUFFER_CONFIG(0, timeout));
    drain();
    words_for(5, w); queue_packet(1ULL << 36, w);
    words_for(6, w); queue_packet(1ULL << 36, w);
    start_tx();
    collect(2000, 1000);
    unsigned want = (timeout == 4) ? 1 : 2;
    check(G_NW == 2 && G_NP == want, "timeout %u:  %u words in %u packets (expected 2 in %u;  %u UART periods apart)",
          timeout, G_NW, G_NP, want, G_NW == 2 ? (unsigned) (G_W[1].ts - G_W[0].ts) : 0);
  }
  wr(RX_COMMON + C_ADDR_RX_BUFFER_CONFIG, RX_BUFFER_CONFIG(0, 1));
}

static void check_pattern(void){
  rx_loopback(0);
  wr(RX_COMMON + C_ADDR_RX_PATTERN_A, 0xDEADBEEF);
  wr(RX_COMMON + C_ADDR_RX_PATTERN_B, 0xC0FFEE00);
  wr(RX_COMMON + C_ADDR_RX_PATTERN_DELAY, 1000);          // 1000 idle bits:  a word every ~107 us
  wr(RX_UART(37, C_ADDR_RX_UART_CONFIG), RX_TEST_PATTERN);
  hw_val_t starts0 = rd(RX_COMMON + C_ADDR_RX_PATTERN_STARTS);
  drain();
  wr(RX_COMMON + C_ADDR_RX_PATTERN_CONFIG, 0x00000104);   // on, phase 4
  uint64_t t0 = now_us();
  while (now_us() - t0 < 2000) ;
  // stop between frames:  a long delay after the current frame, then off (off mid-frame would cut a word short)
  wr(RX_COMMON + C_ADDR_RX_PATTERN_DELAY, 0xFFFFFFFF);
  t0 = now_us();
  while (now_us() - t0 < 200) ;
  wr(RX_COMMON + C_ADDR_RX_PATTERN_CONFIG, 0x00000000);
  collect(1000, 500);
  hw_val_t starts1 = rd(RX_COMMON + C_ADDR_RX_PATTERN_STARTS);
  wr(RX_UART(37, C_ADDR_RX_UART_CONFIG), RX_CONFIG(0x0A, 0, RX_MODE_EDGE, RX_INPUT_OFF, 4));

  req("RX-5.1");
  unsigned good = 0;
  for (unsigned i = 0; i < G_NW; i++){
    if (chan_of(&G_W[i]) == 38 && data64(&G_W[i]) == 0xC0FFEE00DEADBEEFULL) good++;
    else check(0, "word %u:  channel id %u, data 0x%08x%08x", i, chan_of(&G_W[i]), (unsigned) G_W[i].data_hi,
               (unsigned) G_W[i].data_lo);
  }
  check(G_NW >= 10 && good == G_NW, "UART 37 on the pattern:  %u words, %u of them the pattern word", G_NW, good);
  hw_val_t dn = starts1 - starts0;
  check(dn + 2 >= G_NW && dn <= G_NW + 2, "PATTERN_STARTS counted %u frames (%u words received)", (unsigned) dn,
        G_NW);
  drain();
}

static void check_heartbeat(void){
  rx_loopback(0);
  wr(RX_COMMON + C_ADDR_RX_HEARTBEAT_CONFIG, 100000);      // 1 ms
  drain();
  wr(RX_COMMON + C_ADDR_RX_BUFFER_ENABLES, 0x1);
  collect(5500, 0);
  wr(RX_COMMON + C_ADDR_RX_BUFFER_ENABLES, 0x0);
  hw_val_t header_a = rd(RX_COMMON + C_ADDR_RX_HEADER_A);
  hw_val_t uart = rd(ATC(C_ADDR_ATC_CONFIG_UART));

  req("RX-4.1");
  check(G_NW >= 4 && G_NW <= 7, "heartbeats in 5.5 ms at 1 ms:  %u", G_NW);
  for (unsigned i = 0; i < G_NW; i++){
    check_eq(G_W[i].header, (header_a & 0xFFFF00FF) | (PACMAN_ID << 8), "heartbeat header:  HEADER_A, PACMAN id");
    check(data64(&G_W[i]) == 0, "heartbeat data zero");
    if (i > 0){
      hw_val_t dt = G_W[i].ts - G_W[i-1].ts;
      check(dt + 1 >= 100000 / uart && dt <= 100000 / uart + 1, "heartbeat period %u UART periods (expected %u)",
            (unsigned) dt, (unsigned) (100000 / uart));
    }
  }

  req("ATC-1.2");
  hw_val_t now = timestamp();
  if (G_NW > 0)
    check((hw_val_t) (now - G_W[G_NW-1].ts) < 1000000, "the RX unit's timestamps follow the timing unit's");
  drain();
}

// a poke routed to one marker only (no G or H:  nothing leaves the PL)
#define DST_MARKER(m) (1u << (2 + (m)))

static hw_val_t count(unsigned cmd){
  wr(ATC(C_ADDR_ATC_COUNT_REQ), cmd);
  return rd(ATC(C_ADDR_ATC_COUNT));
}

static void check_markers(void){
  rx_loopback(0);
  wr(ATC(C_ADDR_ATC_DST_POKE_A), DST_MARKER(2));
  wr(ATC(C_ADDR_ATC_DST_POKE_B), DST_MARKER(3));
  wr(RX_COMMON + C_ADDR_RX_HEADER_D, 0x00000058);
  drain();
  wr(RX_COMMON + C_ADDR_RX_BUFFER_ENABLES, 0xC);
  hw_val_t ts0 = timestamp();
  wr(ATC(C_ADDR_ATC_POKE_A), 0);
  wr(ATC(C_ADDR_ATC_POKE_B), 0);
  hw_val_t ts1 = timestamp();
  collect(2000, 1000);
  wr(RX_COMMON + C_ADDR_RX_BUFFER_ENABLES, 0x0);
  hw_val_t header_c = rd(RX_COMMON + C_ADDR_RX_HEADER_C);

  req("RX-4.3");
  check(G_NW == 2, "2 marker words for one poke to marker 2 and one to marker 3:  %u", G_NW);
  if (G_NW == 2){
    check_eq(G_W[0].header, (header_c & 0xFFFF00FF) | (PACMAN_ID << 8), "marker 2 header:  HEADER_C");
    check_eq(G_W[1].header, 0x00000058 | (PACMAN_ID << 8), "marker 3 header:  HEADER_D");
    for (unsigned i = 0; i < 2; i++)
      check((hw_val_t) (G_W[i].ts - ts0) <= (hw_val_t) (ts1 - ts0), "marker timestamp within the pokes");
  }

  req("ATC-2.2");
  check(G_NW == 2, "POKE A reached marker 2, POKE B marker 3 (by their destination registers)");
  wr(ATC(C_ADDR_ATC_DST_POKE_A), 0);
  wr(ATC(C_ADDR_ATC_DST_POKE_B), 0);
  wr(RX_COMMON + C_ADDR_RX_HEADER_D, 0);

  // the counters:  READ I 2 (POKE A), READ M 2;  STOP, START, CLEAR
  req("ATC-5.1");
  wr(ATC(C_ADDR_ATC_DST_POKE_A), DST_MARKER(2));
  count(0x20);                                         // START (the default)
  hw_val_t i0 = count(0x72) & 0xFFFF, m0 = count(0x42) & 0xFFFF;
  for (int k = 0; k < 3; k++) wr(ATC(C_ADDR_ATC_POKE_A), 0);
  hw_val_t i1 = count(0x72) & 0xFFFF, m1 = count(0x42) & 0xFFFF;
  check(i1 - i0 == 3, "READ I 2 (POKE A):  +%u edges for 3 pokes", (unsigned) (i1 - i0));
  check(m1 - m0 == 3, "READ M 2:  +%u edges for 3 marker pulses", (unsigned) (m1 - m0));
  count(0x30);                                         // STOP
  wr(ATC(C_ADDR_ATC_POKE_A), 0);
  hw_val_t i2 = count(0x72) & 0xFFFF;
  check(i2 == i1, "STOP:  no count (%u -> %u)", (unsigned) i1, (unsigned) i2);
  count(0x20);                                         // START
  wr(ATC(C_ADDR_ATC_POKE_A), 0);
  hw_val_t i3 = count(0x72) & 0xFFFF;
  check(i3 == i2 + 1, "START:  counts again (%u -> %u)", (unsigned) i2, (unsigned) i3);
  count(0x10);                                         // CLEAR
  check(count(0x72) == 0 && count(0x42) == 0, "CLEAR:  counters zero");
  wr(ATC(C_ADDR_ATC_DST_POKE_A), 0);
}

static void check_sync(void){
  req("ATC-1.3");
  wr(ATC(C_ADDR_ATC_DST_POKE_C), DST_MARKER(0));
  uint64_t t0 = now_us();
  while (now_us() - t0 < 2000) ;                       // let the timestamp grow past 2 ms
  hw_val_t before = timestamp();
  wr(ATC(C_ADDR_ATC_POKE_C), 0);
  hw_val_t after = timestamp();
  wr(ATC(C_ADDR_ATC_DST_POKE_C), 0);
  check(before > 10000 && after < 1000, "marker 0 restarts the timestamp:  %u -> %u", (unsigned) before,
        (unsigned) after);
}

// ------------------------------------------------------------------------------------------------------------

static int report(void){
  int nfail = 0, nnone = 0;
  printf("\r\n");
  for (unsigned i = 0; i < NREQ; i++){
    const char * s = REQS[i].fails ? "FAIL" : (REQS[i].checks ? "PASS" : "NONE");
    if (REQS[i].fails) nfail++;
    if (!REQS[i].checks) nnone++;
    printf("%s %-8s %4u  %s\r\n", s, REQS[i].id, REQS[i].checks, REQS[i].text);
  }
  const char * verdict = nfail ? "FAIL" : (nnone ? "INCOMPLETE" : "PASS");
  printf("VERDICT %s  (%u requirements:  %d failed, %d not checked;  %d failed checks)\r\n", verdict,
         (unsigned) NREQ, nfail, nnone, G_FAILS);
  return nfail ? EXIT_FAIL : (nnone ? EXIT_INCOMPLETE : EXIT_PASS);
}

#ifdef _LINUX
// a pacman server running (it owns the DMA engine and the RX/TX configuration):  name of the first found, or NULL
static const char * server_running(void){
  static char name[64];
  DIR * d = opendir("/proc");
  struct dirent * e;
  if (!d) return NULL;
  while ((e = readdir(d)) != NULL){
    char path[300];
    snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
    FILE * f = fopen(path, "r");
    if (!f) continue;
    // comm is cut to 15 characters ("pacman_cmdserve", "pacman_dataserv")
    if (fgets(name, sizeof(name), f) && strncmp(name, "pacman_", 7) == 0 && strstr(name, "serv")){
      fclose(f);
      closedir(d);
      name[strcspn(name, "\n")] = 0;
      return name;
    }
    fclose(f);
  }
  closedir(d);
  return NULL;
}
#endif

int main(int argc, char ** argv){
  int read_only = 0;
#ifdef _LINUX
  for (int i = 1; i < argc; i++){
    if      (strcmp(argv[i], "--read_only") == 0) read_only = 1;
    else if (strcmp(argv[i], "-v") == 0)          G_VERBOSE = 1;
    else {
      printf("usage:  pacman_fwcheck [--read_only] [-v]\n"
             "  the firmware requirements (sim+hw) checked on this board;  -v prints every check\n");
      return EXIT_REFUSED;
    }
  }
#else
  (void) argc; (void) argv;
#endif
  printf("pacman_fwcheck:  firmware requirements checked in the installed firmware\r\n");
  axil_platform_init();
  init_jumper();

  check_identity();
  check_bus_read_only();
  check_timestamp_rate();

  const char * why = NULL;
  hw_val_t enables = rd(GLB(C_ADDR_GLOBAL_ENABLES));
  if (read_only)                                    why = "--read_only";
  else if (jumper_installed() != JUMPER_OUT)        why = "J5 installed (or unreadable)";
  else if (enables & (GLOBAL_ENABLE_TILES | GLOBAL_ENABLE_TILE_POWER)) why = "tile power or a tile is enabled";
#ifdef _LINUX
  else if (server_running())                        why = server_running();
#endif
  if (why){
    printf("INFO     read-only checks only:  %s\r\n", why);
    return report();
  }

  save_all();
#ifdef _LINUX
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGALRM, on_signal);
  alarm(300);                       // the whole run takes well under a minute
#endif
  check_bus_writes();
  wr(SCOPE_TX + UART_BROADCAST + C_ADDR_TX_UART_CONFIG, 0x00000006);   // the default:  phase 6, no delay
  init_rxtx();
  init_rxtx_descriptor_ring_mode(64);
  check_one_packet();
  check_all_channels();
  check_bits();
  check_frame_length();
  check_header();
  check_input_select();
  check_packets();
  check_pattern();
  check_heartbeat();
  check_markers();
  check_sync();
  restore_all();
  zero_rxtx_counts();
  return report();
}
