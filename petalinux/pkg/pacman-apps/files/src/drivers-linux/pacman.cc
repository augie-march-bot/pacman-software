#ifndef SIMULATED_PACMAN

#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#include "version.hh"
#include "addr_conf.hh"
#include "pacman.hh"
#include "tx_buffer.hh"

// new common drivers:
#include "hw_access.h"
#include "dma.h"
#include "rxtx.h"
#include "iic.h"
#include "atc.h"
#include "global.h"

static unsigned count_rx = 0;
static unsigned max_rx_pending = 0;

unsigned pacman_packet_count_rx(int clear){
  unsigned tmp = count_rx;
  if (clear)
    count_rx = 0;
  return tmp;
}

unsigned pacman_max_rx_pending(int clear){
  unsigned tmp = max_rx_pending;
  if (clear)
    max_rx_pending = 0;
  return tmp;
}

volatile uint32_t * G_PACMAN_AXIL = NULL;


int pacman_init(int verbose){
  // initialize axi-lite
  if (verbose){
    printf("INFO:  Initializing PACMAN AXI-Lite interface.\n");
  }

  axil_platform_init();
  timing_platform_init();

  global_status_t gs;
  get_global_status(&gs);

  if (verbose){
    printf("INFO:  Running pacman-server version %d.%d.%d\n",
	   PACMAN_SERVER_MAJOR_VERSION, PACMAN_SERVER_MINOR_VERSION, PACMAN_SERVER_PATCH_VERSION);
    printf("INFO:  Running pacman firmware version %u.%u.%u\n",
	   (unsigned) gs.firmware_major, (unsigned) gs.firmware_minor, (unsigned) gs.firmware_patch);
  }

  // initialize I2C, for the hardware version the firmware was built for:
  if (verbose){
    printf("INFO:  Initializing PACMAN I2C interface for PACMAN %uv%u\n",
	   (unsigned) gs.hardware_major, (unsigned) gs.hardware_minor);
  }
  iic_platform_init();
  iic_set_hw_version(gs.hardware_major, gs.hardware_minor, gs.hardware_patch);

  // DEFAULT parameters
  if (verbose){
    printf("INFO:  Setting minimum DMA packet size to 0x3900 and packet timeout to 1 ms\n");
    printf("INFO:  Enabling Trigger, Sync, and Heartbeat words in the RX unit.\n");
  }
  rx_set_buffer_config(RX_BUFFER_CONFIG_DAQ);
  axil_write_register(0x7FB8, 0x0003);

  if (verbose){
    printf("INFO:  Configuring RX UARTs. \n");
  }
  rx_set_config_all(RX_CONFIG_DEFAULT);

  if (verbose){
    printf("INFO:  Limiting TX bandwidth\n");
  }
  axil_write_register(0x3B04, 0x00C60006);

  if (verbose){
    printf("INFO:  Setting ASIC Timing and Control unit to default configuration. \n");
  }
  set_atc_default_config();


  return EXIT_SUCCESS;
}

int pacman_init_tx(int verbose, int skip_reset){
  init_rxtx();

  // reset of S2MM halts MM2S in DMA SG mode, so cmdserver (TX) handles both resets:
  dma_reset_tx(DMA_TIMEOUT);
  dma_reset_rx(DMA_TIMEOUT);

  init_tx_descriptor_ring_mode(512);
  return EXIT_SUCCESS;
}

int pacman_init_rx(int verbose, int skip_reset){
  init_rxtx();

  printf("INFO:  Waiting for TX server to initialize first.\r\n");
  // give pacman_cmdserver a head start:
  usleep(100000);

  // confirm it is running:
  unsigned timeout = dma_wait_tx_run(100000);
  if (timeout == 0){
    printf("ERROR:  RX is not running.  Due to single DMA core, must initialize TX (pacman_cmdserver) before starting RX (pacman_dataserver\r\n");
    exit(0);
  }

  printf("INFO:  Initializing RX descriptor ring.\r\n");
  init_rx_descriptor_ring_mode(512);
  return EXIT_SUCCESS;
}

void pacman_poll_rx(pacman_word_t * buffer, unsigned * index, unsigned max){
  const unsigned rx_trailer_bytes = 24; // current firmware, each DMA RX packet has a 192-bit trailer
  hw_addr_t nxta;

  // track maximum of rx buffers pending:
  unsigned pending = rx_pending();
  if (pending > max_rx_pending)
    max_rx_pending = pending;

  while (1) {
    // check that there is enough space for a maximially filled DMA buffer:
    const unsigned max_words_per_bd = (RX_BUF_BYTES - rx_trailer_bytes) / 24;
    if ((max - *index) < max_words_per_bd)
      return;

    if (!dma_next_available_rx_bd(&nxta))
      return;

    unsigned xbytes = dma_poll_bd_transferred(nxta);
    hw_ptr_t rx_buf = dma_get_buffer(nxta);

    assert(xbytes > rx_trailer_bytes);
    assert((xbytes - rx_trailer_bytes) % 24 == 0);
    unsigned packets = (xbytes - rx_trailer_bytes) / 24;
    count_rx += packets;
    memcpy(&buffer[*index], (void*)rx_buf, xbytes - rx_trailer_bytes);
    *index += packets;
    dma_add_rx_bd(nxta);
    dma_rx_batch();
  }
}

int pacman_poll_tx(){
  const unsigned frags = TX_BUFFER_BYTES/4; // number of u32 fragments per buffer
  static uint32_t output[frags];

  const unsigned batch_size = 100;
  unsigned batch_count = 0;
  hw_addr_t nxta;

  while((batch_count < batch_size) && dma_next_available_tx_bd(&nxta)){
    if (tx_buffer_out(output)==1){
      tx_buffer_print_output(output);
      hw_ptr_t tx_buf = dma_get_buffer(nxta);
      for (unsigned i=0; i<frags; i++)
	tx_buf[i] = output[i];
      dma_add_tx_bd(nxta);
      batch_count++;
    } else {
      break;
    }
  }
  if (batch_count > 0){
    printf("INFO:  sending %d TX buffers \r\n", batch_count);
    dma_tx_batch();
  }
  return EXIT_SUCCESS;
}


#endif
