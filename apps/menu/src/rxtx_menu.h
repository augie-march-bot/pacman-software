#ifndef __RXTX_MENU_H_
#define __RXTX_MENU_H_

#include <stdint.h>
#include "rxtx.h"

#ifdef __cplusplus
extern "C" {
#endif

// RX test configurations (whole RX UART config words), shared by pacman_menu and pacman_hwcheck:
#define RX_TEST_PIN            RX_CONFIG_DEFAULT                                     // normal: UART input
#define RX_TEST_LOOPBACK       RX_CONFIG(0x0A, 0, RX_MODE_EDGE,  RX_INPUT_LOOPBACK, 4)  // full internal loopback
#define RX_TEST_PATTERN        RX_CONFIG(0x0A, 0, RX_MODE_EDGE,  RX_INPUT_PATTERN,  4)  // test pattern
#define RX_TEST_LOOPBACK_FIXED RX_CONFIG(0x0A, 0, RX_MODE_FIXED, RX_INPUT_LOOPBACK, 1)  // loopback, edge detect disabled
#define RX_TEST_LOOPBACK_NOISE RX_CONFIG(0x0A, 1, RX_MODE_EDGE,  RX_INPUT_LOOPBACK, 8)  // loopback, intentional noise
#define RX_TEST_LOOPBACK_FAIL  RX_CONFIG(0x0A, 1, RX_MODE_EDGE,  RX_INPUT_LOOPBACK, 9)  // loopback, intentional failures

void toggle_rx_config(void);

// Loopback check with random data:  every TX packet carries a pseudo-random 64-bit
// word for each UART in uart_mask (bit u: UART u, 0-39); each received word is compared
// with the word expected next on its UART.  A word from a UART outside the mask is a
// stray word.  Uses the current RX config (pin or internal loopback) and the DMA ring
// already set up (init_rxtx_descriptor_ring_mode); the ring is not touched.
#define RXTX_ALL_UARTS 0xFFFFFFFFFFULL
typedef struct {
  unsigned tx_packets;    // DMA packets sent
  unsigned words_sent;    // UART words sent (one per masked UART per packet)
  unsigned words_rcvd;    // UART data words received and checked
  unsigned words_bad;     // received words with at least one bit error
  unsigned bit_errors;    // total bit errors in received words
  unsigned stale_words;   // words already waiting before the check started (discarded)
  unsigned other_words;   // received words not from a UART (sync, heartbeat, trigger)
  unsigned stray_words;   // received words from a UART outside the mask
  unsigned rcvd[40];      // words received per UART (stray words included)
  unsigned bad_packets;   // RX DMA packets with an invalid size
  unsigned tx_timeout;    // nonzero if TX stalled
} rxtx_check_t;

void check_rxtx_loopback(unsigned tx_packets, hw_u32_t seed, uint64_t uart_mask, rxtx_check_t * r);
void print_rxtx_check(const rxtx_check_t * r);
void menu_check_rxtx_loopback(void);

#ifdef __cplusplus
}
#endif

#endif // __RXTX_MENU_H_
