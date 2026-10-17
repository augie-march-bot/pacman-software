#ifndef __GLOBAL_H_
#define __GLOBAL_H_

#ifdef __cplusplus
extern "C" {
#endif

// GLOBAL REGISTERS:

#define SCOPE_GLOBAL 0xF000

#define C_ADDR_GLOBAL_STATUS          0x000
#define C_ADDR_GLOBAL_ENABLES         0x010
#define C_ADDR_GLOBAL_LEDS            0x014
#define C_ADDR_GLOBAL_SCRATCH_A       0x020
#define C_ADDR_GLOBAL_SCRATCH_B       0x024
#define C_ADDR_GLOBAL_FIRMWARE_MAJOR  0xF10
#define C_ADDR_GLOBAL_FIRMWARE_MINOR  0xF14
#define C_ADDR_GLOBAL_FIRMWARE_PATCH  0xF18
#define C_ADDR_GLOBAL_HARDWARE_MAJOR  0xF20
#define C_ADDR_GLOBAL_HARDWARE_MINOR  0xF24
#define C_ADDR_GLOBAL_HARDWARE_PATCH  0xF28
#define C_ADDR_GLOBAL_SYNTHESIS_DATE  0xF30
#define C_ADDR_GLOBAL_GIT_HASH_UPPER  0xF40
#define C_ADDR_GLOBAL_GIT_HASH_LOWER  0xF44
#define C_ADDR_GLOBAL_BUILD_FLAGS     0xF48
#define C_ADDR_GLOBAL_VIVADO_MAJOR    0xF50
#define C_ADDR_GLOBAL_VIVADO_MINOR    0xF54

// build flags (C_ADDR_GLOBAL_BUILD_FLAGS), set at synthesis by version_info.tcl:
#define GLOBAL_BUILD_DIRTY        0x00000001   // vivado/ had local changes
#define GLOBAL_BUILD_OFFICIAL     0x00000002   // official release (BUILD_RELEASE committed); else a candidate release
#define GLOBAL_BUILD_FLAGS_MASK   0x00000003   // other bits set:  firmware without this register

// global enables (C_ADDR_GLOBAL_ENABLES):
#define GLOBAL_ENABLE_TILES       0x000003FF   // bit i: local power and logic for tile i+1
#define GLOBAL_ENABLE_TILE_POWER  0x00010000   // main analog power rails for all tiles

// all global registers, read in one call:
typedef struct {
  hw_val_t status;
  hw_val_t enables;
  hw_val_t leds;
  hw_val_t scratch_a;
  hw_val_t scratch_b;
  hw_val_t firmware_major;
  hw_val_t firmware_minor;
  hw_val_t firmware_patch;
  hw_val_t hardware_major;
  hw_val_t hardware_minor;
  hw_val_t hardware_patch;
  hw_val_t synthesis_date;   // Unix time (UTC)
  hw_val_t git_hash_upper;
  hw_val_t git_hash_lower;
  hw_val_t build_flags;
  hw_val_t vivado_major;
  hw_val_t vivado_minor;
} global_status_t;

void get_global_status(global_status_t * gs);

void get_synthesis_date_string(char * buffer, size_t buffer_size);
void get_git_hash_string(char * buffer, size_t buffer_size);
void get_build_string(hw_val_t build_flags, char * buffer, size_t buffer_size);   // "official", "candidate, dirty", ...

void read_global_status();
void print_global_version();
void toggle_global_scratch();
void set_global_enables(hw_val_t enables);
void toggle_global_enables();

#ifdef __cplusplus
}
#endif

#endif // __GLOBAL_H_


