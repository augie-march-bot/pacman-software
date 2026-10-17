#ifndef __RXTX_H_
#define __RXTX_H_

#include<stdbool.h>
#include "hw_access.h"

#ifdef __cplusplus
extern "C" {
#endif

// RX/TX REGISTERS:

#define SCOPE_TX       0x0000
#define SCOPE_RX       0x4000
#define UART_GLOBAL    0x3F00
#define UART_BROADCAST 0x3B00

#define C_ADDR_RX_UART_STATUS       0x00
#define C_ADDR_RX_UART_CONFIG       0x04
#define C_ADDR_RX_UART_CHAN         0x08

// RX UART config fields (C_ADDR_RX_UART_CONFIG; firmware rx_chan.vhd):
#define RX_CONFIG_PERIOD_SHIFT 16          // [23:16] clocks per bit
#define RX_CONFIG_PERIOD_MASK  0x00FF0000
#define RX_CONFIG_FLAGS_SHIFT  14          // [15:14] bit 14: start a byte despite vote dissent
#define RX_CONFIG_FLAGS_MASK   0x0000C000
#define RX_CONFIG_MODE_SHIFT   12          // [13:12] sampling, see RX_MODE_*
#define RX_CONFIG_MODE_MASK    0x00003000
#define RX_CONFIG_INPUT_SHIFT  8           // [11:8]  input select, see RX_INPUT_*
#define RX_CONFIG_INPUT_MASK   0x00000F00
#define RX_CONFIG_PHASE_SHIFT  0           // [7:0]   sample point
#define RX_CONFIG_PHASE_MASK   0x000000FF

#define RX_CONFIG(period, flags, mode, input, phase) \
  ( ((period) << RX_CONFIG_PERIOD_SHIFT) | ((flags) << RX_CONFIG_FLAGS_SHIFT) | ((mode) << RX_CONFIG_MODE_SHIFT) \
  | ((input) << RX_CONFIG_INPUT_SHIFT) | ((phase) << RX_CONFIG_PHASE_SHIFT) )

#define RX_MODE_OFF           0x0          // no sampling
#define RX_MODE_EDGE          0x1          // sample phase relative to the detected falling edge
#define RX_MODE_FIXED         0x2          // fixed sample phase (no edge detection)

#define RX_INPUT_OFF          0x0
#define RX_INPUT_PIN          0x1          // PISO from the pin
#define RX_INPUT_LOOPBACK     0x2          // own TX (POSI), inside the firmware
#define RX_INPUT_PATTERN      0x4          // test pattern generator
#define RX_INPUT_EMUL         0x8          // ASIC emulation

// RX buffer config fields (C_ADDR_RX_BUFFER_CONFIG = 0xMMMMTTTT; firmware rx_buffer.vhd), 0 = no condition:
#define RX_BUFFER_MAX_WORDS_SHIFT 16          // [31:16] max 192-bit words per DMA packet
#define RX_BUFFER_MAX_WORDS_MASK  0xFFFF0000
#define RX_BUFFER_TIMEOUT_SHIFT   0           // [15:0]  packet timeout, in sweeps of 512 clocks
#define RX_BUFFER_TIMEOUT_MASK    0x0000FFFF

#define RX_BUFFER_CONFIG(max_words, timeout) \
  ( ((max_words) << RX_BUFFER_MAX_WORDS_SHIFT) | ((timeout) << RX_BUFFER_TIMEOUT_SHIFT) )

// DAQ setting (pacman-server):  0x260 words (0x3900 bytes) per packet, timeout 0xC4 sweeps (~1 ms)
#define RX_BUFFER_CONFIG_DAQ  RX_BUFFER_CONFIG(0x260, 0xC4)

// operating default:  UART input, 10 clocks per bit, sampled 4 clocks after the falling edge
#define RX_CONFIG_DEFAULT     RX_CONFIG(0x0A, 0, RX_MODE_EDGE, RX_INPUT_PIN, 4)

#define C_ADDR_RX_UART_STARTS       0x20
#define C_ADDR_RX_UART_BEATS        0x24
#define C_ADDR_RX_UART_UPDATES      0x28
#define C_ADDR_RX_UART_LOST         0x2C

#define C_ADDR_RX_UART_FRAME_ERR    0x30
#define C_ADDR_RX_UART_NOISE        0x34


#define C_ADDR_RX_LOOK_SELECT       0xA0
#define C_ADDR_RX_LOOK_UA           0xA4
#define C_ADDR_RX_LOOK_UB           0xA8

#define C_ADDR_RX_BUFFER_STATUS     0xB0
#define C_ADDR_RX_BUFFER_CONFIG     0xB4
#define C_ADDR_RX_BUFFER_ENABLES    0xB8
#define C_ADDR_RX_PACMAN            0xBC

#define C_ADDR_RX_FIFO_CNT          0xF0
#define C_ADDR_RX_FIFO_MAX          0xF4
#define C_ADDR_RX_ZERO_CNTS         0xF8
#define C_ADDR_RX_HEARTBEAT_CONFIG  0xC0
#define C_ADDR_RX_ROLLOVER_CONFIG   0xC4
#define C_ADDR_RX_WORD_TYPE_LUT     0xC8
#define C_ADDR_RX_HEADER_A          0xD0
#define C_ADDR_RX_HEADER_B          0xD4
#define C_ADDR_RX_HEADER_C          0xD8
#define C_ADDR_RX_HEADER_D          0xDC
#define C_ADDR_RX_EOP_HEADER        0xE0

#define C_ADDR_TX_UART_STATUS   0x00
#define C_ADDR_TX_UART_CONFIG   0x04
#define C_ADDR_TX_UART_STARTS   0x20
#define C_ADDR_TX_UART_BEATS    0x24

#define C_ADDR_TX_LOOK_SELECT   0xA0
#define C_ADDR_TX_LOOK_UA       0xA4
#define C_ADDR_TX_LOOK_UB       0xA8
#define C_ADDR_TX_BUFFER_STATUS 0xB0
#define C_ADDR_TX_ZERO_CNTS     0xF8

#define C_ADDR_RX_PATTERN_A      0x80
#define C_ADDR_RX_PATTERN_B      0x84
#define C_ADDR_RX_PATTERN_DELAY  0x88
#define C_ADDR_RX_PATTERN_CONFIG 0x8C
#define C_ADDR_RX_PATTERN_STATUS 0x90
#define C_ADDR_RX_PATTERN_STARTS 0x94
#define C_ADDR_RX_PATTERN_STOPS  0x98

// this is reserved in system-user.dtsi and located within the HP AXI interface for DMA (0x00000000 - 0x3FFFFFFF):
#define DMA_BUFFER_BASEADDR  0x20000000
#define DMA_BUFFER_SIZE      0x10000000  // 256 MB

#define TX_BD_BASEADDR       0x21000000
#define RX_BD_BASEADDR       0x22000000

// 40 uarts x 64 bits + 1-64 bit header => 328 bits = 0x148
// Note: DMA driver will alighn buffer *spacing* to 0x150
#define TX_PACKET_BYTES 0x148
#define TX_HEADER_BYTES 8
#define TX_PAYLOAD_U32_WORDS ((TX_PACKET_BYTES - TX_HEADER_BYTES)/4)
#define TX_HEADER_U32_WORDS  (TX_HEADER_BYTES/4)

// Buffer sizes:
// 1 single UART        (1+1)*24    =  48  = 0x30   <-- size MMMM=0x0001
// 40 UART              (40+1)*24   = 984  = 0x3D8  <-- typical test pattern size
// 40 UART + 3 Extra    (40+3+1)*24 = 1056 = 0x420  <-- max for CCCC=0x0001
//#define RX_BUF_BYTES 0x800
// Enough for single cycles, max (40 uarts + header + 3 T/S/HB) * 16 bytes = 0x2c0 bytes
// Each uart rx takes 10 cycles, so for 10 cycles, the maximum buffer size is:
//    (40 + 1 + 10*3)*16 = 0x470 (1136) bytes
// So the buffer size below is enough for more than 140 cycles (0x8C) which you should see in settings
// Note:  when switching to 64 bit timestamps, each cycle will take three times as long, and so this becomes:
//    (3*40 + 1 + 30*3)*24 = 5064 (0x13c8)
// and the buffer size below is enough for 32 (0x20)  cycles (about 1/4 of 0x8C)
// More directly, that is large enough for 682 words (0x2aa)
#define RX_BUF_BYTES 0x4000

#define RX_TRAILER_BYTES 24
#define RX_WORD_BYTES    24


#define TX_BATCH_NEXTDESC_ADDR       0x20000000
#define RX_BATCH_NEXTDESC_ADDR       0x20000004

// pacman-server hooks:
void init_rxtx(void);
void init_tx_descriptor_ring_mode(int ring_size);
void init_rx_descriptor_ring_mode(int ring_size);

void rx_set_input(unsigned chan, unsigned input);   // RX_INPUT_*
void rx_set_config_all(hw_u32_t config);            // broadcast to all RX UARTs
hw_val_t rx_get_buffer_config(void);
void rx_set_buffer_config(hw_val_t config);
void rx_disable_uart(unsigned chan);
void rx_enable_uart(unsigned chan);
bool rx_uart_is_enabled(unsigned chan);

unsigned rx_pending(void);

// menu hooks:
// per-UART status, read in one call:
typedef struct {
  hw_val_t status;
  hw_val_t config;
  hw_val_t chan;
  hw_val_t starts;
  hw_val_t beats;
  hw_val_t updates;
  hw_val_t lost;
  hw_val_t frame_err;
  hw_val_t noise;
} rx_uart_status_t;

typedef struct {
  hw_val_t status;
  hw_val_t config;
  hw_val_t starts;
  hw_val_t beats;
} tx_uart_status_t;

void get_rx_uart_status(unsigned chan, rx_uart_status_t * us);
void get_tx_uart_status(unsigned chan, tx_uart_status_t * us);

void read_tx_status(void);
void read_tx_look(void);
void toggle_tx_config(void);
void toggle_tx_mask(void);

void read_rx_status(void);
void read_rx_look(void);
void toggle_rx_buffer_config(void);
void toggle_rx_buffer_enables(void);
void toggle_rx_test_patterns(void);
void toggle_rx_test_pattern_delay(void);

void zero_rxtx_counts(void);

void init_rxtx_descriptor_ring_mode(int ring_size);
unsigned rxtx_ring_size(void);   // 0: no ring set up yet in this process
void show_rxtx_bds(void);
void show_rxtx_head_tail(void);

void clear_rxtx_ioc(void);
void show_tx_buffer(void);
void show_rx_buffer(void);
void show_rx_transferred(void);

void single_tx(void);
void single_rx(void);

void batch_tx(void);
void batch_rx(void);


void benchmark_tx(void);
void benchmark_rxtx_loopback(void);

#ifdef __cplusplus
}
#endif

#endif // __RXTX_H_
