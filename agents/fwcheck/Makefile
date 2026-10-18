# pacman_fwcheck, Linux:  one static ARM binary (runs on any of the board's root file systems), built with the
# Vitis cross compiler against the pacman-apps drivers.  Bare metal:  baremetal/install.tcl (xsct).
#
#   make            build/pacman_fwcheck
#   make stage      copy it to felix's TFTP server (/srv/tftp/trenz/dev), for "tftp -g -r dev/pacman_fwcheck"
#   make reqcheck   the requirements in fwcheck.c are exactly the sim+hw ones of requirements.txt

SHELL := /bin/bash
CC   := /tools/Xilinx/Vitis/2023.2/gnu/aarch32/lin/gcc-arm-linux-gnueabi/bin/arm-linux-gnueabihf-gcc
APPS := ../../petalinux/pkg/pacman-apps/files
SRCS := fwcheck.c $(APPS)/src/platform/hw_access.c \
        $(addprefix $(APPS)/src/drivers-common/,global.c rxtx.c rxtx_menu.c dma.c jumper.c asic.c)
CFLAGS := -D_LINUX -O2 -Wall -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
          -I$(APPS)/include -I$(APPS)/src/drivers-common -I$(APPS)/src/platform

build/pacman_fwcheck: $(SRCS) $(wildcard $(APPS)/src/drivers-common/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -static -o $@ $(SRCS)

stage: build/pacman_fwcheck
	cp $< /srv/tftp/trenz/dev/

clean:
	rm -rf build

.PHONY: stage clean reqcheck

# the requirements fwcheck reports must be exactly the "sim+hw" ones of the firmware's requirements.txt
REQS := ../../../pacman-firmware/ghdl/agents/requirements.txt
reqcheck:
	@diff <(grep -E '^[A-Z]+-[0-9]+\.[0-9]+ +sim\+hw' $(REQS) | awk '{print $$1}') \
	      <(grep -oE '^  \{"[A-Z]+-[0-9]+\.[0-9]+"' fwcheck.c | tr -d ' {"') && echo "reqcheck:  fwcheck.c covers the sim+hw requirements"
