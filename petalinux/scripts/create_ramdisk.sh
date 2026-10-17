#! /bin/bash

SRC=./src
PKG=./pkg
SPEC=ramdisk
PROJ=$SPEC

# create project in the correct location no matter where the script is run from:
cd "$(dirname "$0")/.."
echo "working directory is:  "
pwd

if [ -d "$PROJ" ]; then
    echo "The petalinux project directory $PROJ already exists."
    echo "*** You must delete or move project directory to start the process from scratch. ***"
    # uncomment return below to require starting from scratch.
    # return 0
    echo "WARNING   Updating an *existing* project without starting from scratch."
else
    echo "Creating new project directory $PROJ"
    petalinux-create -t project -n $PROJ --template zynq
fi

if [ -d "$PROJ/project-spec.prev" ]; then
    echo "Please delete $PROJ/project-spec.prev then retry ..."
    exit 0
fi

# Baseline is the project-spec directory provided by TRENZ for the test board design:
mv $PROJ/project-spec $PROJ/project-spec.prev
cp -r $SRC/trenz/project-spec $PROJ/

# Our changes:
echo "our changes:"
cp -v $SRC/$SPEC/system-user.dtsi  $PROJ/project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi
cp -v $SRC/$SPEC/user-rootfsconfig $PROJ/project-spec/meta-user/conf/
cp -v $SRC/$SPEC/rootfs_config     $PROJ/project-spec/configs/rootfs_config
cp -v $SRC/$SPEC/config            $PROJ/project-spec/configs/config

# TFTP boot script boot.scr.uimg (built next to boot.scr):
mkdir -p $PROJ/project-spec/meta-user/recipes-bsp/u-boot/files
cp -v $SRC/$SPEC/u-boot/u-boot-xlnx-scr.bbappend $PROJ/project-spec/meta-user/recipes-bsp/u-boot/
cp -v $SRC/$SPEC/u-boot/boot.cmd.tftp            $PROJ/project-spec/meta-user/recipes-bsp/u-boot/files/

# add PACMAN software package:
ln -sfn  ../../../../$PKG/pacman-apps $PROJ/project-spec/meta-user/recipes-apps/

