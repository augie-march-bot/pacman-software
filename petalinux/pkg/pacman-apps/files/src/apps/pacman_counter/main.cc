#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <zmq.h>
#include <cstring>
#include <cassert>
#include <sys/time.h>
#include <signal.h>

#define REPORT_INTERVAL_MS 10000
#define SOCKET_B_BINDING_SUB "tcp://localhost:5556"
#define MAX_CHANNEL 40

// NOTE: this needs to be synchronized with MAX_PAYLOAD of pacman_dataserver,
// HEADER_BYTES + MAX_PAYLOAD * WORD_BYTES
// = 24 + 24*64000 = 1536024:
#define MAX_BUFFER_SIZE 1536024
uint32_t rx_buffer[MAX_BUFFER_SIZE/4];

volatile bool running = true;
void signal_handler(int) { running = false; }

int main(int argc, char* argv[]){
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  // timers and counters:
  struct timeval start, window_start, now;
  uint32_t packets[MAX_CHANNEL];
  for (int i=0; i<MAX_CHANNEL; i++){
    packets[i]=0;
  }
  uint64_t total_messages = 0;
  uint64_t total_packets = 0;
  uint64_t window_packets = 0;
  uint64_t total_errors = 0;
  uint32_t max_pkts_per_msg = 0;
  uint32_t min_pkts_per_msg = UINT32_MAX;

  int iparam;
  printf("INFO:  Starting ZMQ packet counter utility.\n");
  printf("INFO:  Creating new ZMQ context...\n");
  void* ctx = zmq_ctx_new();

  printf("INFO:  Initializing SUB socket (B) ...\n");
  void * sub = zmq_socket(ctx, ZMQ_SUB);
  iparam = 1000;
  zmq_setsockopt(sub, ZMQ_RCVTIMEO, &iparam, sizeof(iparam));
  zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "", 0);

  if (zmq_connect(sub, SOCKET_B_BINDING_SUB) !=0 ) {
    printf("ERROR:  Failed to connect socket (%s)!\n", SOCKET_B_BINDING_SUB);
    return 1;
  }
  printf("INFO:  ZQM SUB socket (B) connected successfully...\n");


  gettimeofday(&start, NULL);
  gettimeofday(&window_start, NULL);
  while(running){
    gettimeofday(&now, NULL);
    double window_ms     = 1000.0*(now.tv_sec - window_start.tv_sec) + (now.tv_usec - window_start.tv_usec)/1000.0;
    if (window_ms >= REPORT_INTERVAL_MS) {
      double cumulative_ms = 1000.0*(now.tv_sec - start.tv_sec) + (now.tv_usec - start.tv_usec)/1000.0;

      printf("INFO:  --- Report @ %.1f ms (interval: %.1f ms) ---\n", cumulative_ms, window_ms);
      printf("  cumulative packets: %12lu rate: %12.2f / ms\n",  total_packets,  total_packets/cumulative_ms);
      printf("  interval packets: %12lu   rate: %12.2f / ms\n",  window_packets, window_packets/window_ms);
      printf("  packets per message: min: %12u  max: %12u\n", min_pkts_per_msg, max_pkts_per_msg);
      printf("  channel counts:\n");
      for (int i=0; i<MAX_CHANNEL; i++){
	printf("%8d ", packets[i]);
	if (((i+1)%8)==0) printf("\n");
      }
      gettimeofday(&window_start, NULL);
      window_packets = 0;
    }

    zmq_msg_t msg;
    zmq_msg_init(&msg);
    zmq_msg_recv(&msg, sub, 0);
    unsigned size = zmq_msg_size(&msg);
    if (size <= 0){
      zmq_msg_close(&msg);
      //printf("DEBUG:  no message received \n");
      continue;
    }
    if (size > MAX_BUFFER_SIZE){
      printf("ERROR:  unexpectedly large message:  %d ... (MAX: %d) discarding\n", size, MAX_BUFFER_SIZE);
      total_errors++;
      continue;
    }
    memcpy(rx_buffer,zmq_msg_data(&msg), size);
    zmq_msg_close(&msg);

    //printf("DEBUG:  received message of size %d bytes \n", size);
    //printf("DEBUG:  header:  0x %x %x %x\n", rx_buffer[2], rx_buffer[1], rx_buffer[0]);
    uint32_t mtype = rx_buffer[0]&0xFF;
    uint32_t nbytes = rx_buffer[1];
    if (mtype != 0x44){
      printf("NOTE:  message type is not 0x44 (0x%x)... discarding\n", mtype);
      continue;
    }
    if (size != nbytes+24){
      printf("ERROR:  message size (%d bytes) does not match n_bytes in header (%d)\n", size, nbytes);
      continue;
    }
    //printf("DEBUG:  received valid message\n");
    unsigned npkts = (nbytes / 24);
    total_messages++;
    if (npkts > max_pkts_per_msg) max_pkts_per_msg = npkts;
    if (npkts < min_pkts_per_msg) min_pkts_per_msg = npkts;

    for (unsigned i=0; i<npkts; i++){
      uint32_t wtype =  rx_buffer[6+6*i]&0xFF;
      uint32_t chan  = (rx_buffer[6+6*i]>>16)&0xFFFF;
      //printf("DEBUG:  packet %d wtype:  0x%x  chan:  %d \n", i, wtype, chan);

      if (wtype == 0x44){
	if ((chan == 0) || (chan > 40)){
	  printf("ERROR:  invalid channel detected:  %d\n", chan);
	  continue;
	}
	packets[chan-1]++;
	total_packets++;
	window_packets++;
      } else {
	//printf("INFO:  packet: 0x %08x %08x %08x %08x %08x %08x\n",
	//       rx_buffer[6+6*i+5], rx_buffer[6+6*i+4],
	//       rx_buffer[6+6*i+3], rx_buffer[6+6*i+2],
	//       rx_buffer[6+6*i+1], rx_buffer[6+6*i+0]);
      }
    }
  }
  zmq_close(sub);
  zmq_ctx_term(ctx);
  printf("INFO:  Shutting down.\n");
  return 0;

}
