#!/bin/sh
# pacman_lnxcheck:  embedded-Linux feature checkout of the Trenz module (TE0720), with standard tools only:
# CPU and memory, Zynq XADC (die temperature, supplies), Ethernet, QSPI flash, eMMC, microSD.  Runs on a PACMAN or
# on a bare Trenz.  The PACMAN-specific checkout is pacman_hwcheck.
#
# Output (as pacman_hwcheck):  one line per item, "<VERDICT> <name>---- <value>", then a final "VERDICT" line.
# Exit code:  0 PASS, 1 FAIL, 2 INCOMPLETE (something skipped), 3 usage.
# Read-only, except --sd_write (a test file on the microSD card's first partition, removed afterwards).

SERVER=""            # TFTP server for the network test (default:  the default gateway)
TFTP_FILE=system.bit # file fetched from it (twice:  integrity; busybox tftp is lockstep, ~10 Mb/s)
DO_TFTP=1
SD_WRITE=0
SD_WRITE_MB=16

NOMINAL_CPUS=2
MEM_MIN_MB=900       # TE0720:  1 GB DDR3
MEM_MAX_MB=1100
TEMP_MIN_C=5
TEMP_MAX_C=85
SUPPLY_PCT=5         # XADC supplies, +/- percent of nominal
PING_COUNT=100       # 56-byte pings
PING_BIG_COUNT=1000  # 1472-byte pings (full frames, back to back)
EMMC_READ_MB=64

WORK=/tmp/pacman_lnxcheck.$$
NPASS=0; NFAIL=0; NSKIP=0

usage(){
  cat <<EOF
usage:  pacman_lnxcheck [options]
  --server IP       TFTP server for the network test (default:  the default gateway)
  --tftp_file F     file fetched twice from the server, timed and compared (default:  $TFTP_FILE)
  --no_tftp         skip the TFTP transfer (link, ping and counters still run)
  --sd_write        also write a ${SD_WRITE_MB} MB test file to the microSD card, read it back, remove it
  --help            this message
Without --sd_write nothing is written.  Exit code:  0 PASS, 1 FAIL, 2 INCOMPLETE, 3 usage.
EOF
}

# result VERDICT NAME VALUE:  VERDICT is PASS, FAIL, SKIP or INFO
result(){
  local name="$2" n dashes   # local: callers loop over $name
  n=$((24 - ${#name})); [ $n -lt 2 ] && n=2
  dashes=$(printf '%*s' $n '' | tr ' ' '-')
  printf '%-4s  %s%s %s\n' "$1" "$name" "$dashes" "$3"
  case "$1" in
    PASS) NPASS=$((NPASS+1)) ;;
    FAIL) NFAIL=$((NFAIL+1)) ;;
    SKIP) NSKIP=$((NSKIP+1)) ;;
  esac
}

# check NAME OK VALUE:  PASS if OK is 1, else FAIL
check(){
  if [ "$2" = 1 ]; then result PASS "$1" "$3"; else result FAIL "$1" "$3"; fi
}

# within V NOMINAL PCT:  1 if V is within PCT percent of NOMINAL (integers)
within(){
  [ $((100*$1)) -ge $(($2*(100-$3))) ] && [ $((100*$1)) -le $(($2*(100+$3))) ] && echo 1 || echo 0
}

# seconds since boot, in hundredths
now_cs(){
  awk '{printf "%d", $1*100}' /proc/uptime
}

drop_caches(){
  sync; echo 3 > /proc/sys/vm/drop_caches
}

# *** SYSTEM ***

system(){
  result INFO linux_kernel "$(uname -r)"
  cpus=$(grep -c '^processor' /proc/cpuinfo)
  check cpus $([ "$cpus" = $NOMINAL_CPUS ] && echo 1 || echo 0) "$cpus (expected $NOMINAL_CPUS)"
  mem=$(awk '/^MemTotal/ {printf "%d", $2/1024}' /proc/meminfo)
  check memory $([ $mem -ge $MEM_MIN_MB ] && [ $mem -le $MEM_MAX_MB ] && echo 1 || echo 0) \
        "$mem MB (expected $MEM_MIN_MB-$MEM_MAX_MB)"
}

# *** XADC ***

# supply NAME NOMINAL_MV
xadc_supply(){
  d=/sys/bus/iio/devices/iio:device0
  for f in $d/in_voltage*_${1}_raw; do
    [ -r "$f" ] || { result FAIL "xadc_$1" "not found"; return; }
    mv=$(awk -v r=$(cat $f) -v s=$(cat ${f%_raw}_scale) 'BEGIN {printf "%d", r*s + 0.5}')
    check "xadc_$1" $(within $mv $2 $SUPPLY_PCT) "$mv mV (nominal $2 +/- $SUPPLY_PCT%)"
  done
}

xadc(){
  d=/sys/bus/iio/devices/iio:device0
  if [ "$(cat $d/name 2>/dev/null)" != xadc ]; then
    result FAIL xadc "no XADC (iio:device0)"
    return
  fi
  t=$(awk -v r=$(cat $d/in_temp0_raw) -v o=$(cat $d/in_temp0_offset) -v s=$(cat $d/in_temp0_scale) \
      'BEGIN {printf "%d", (r+o)*s/1000 + 0.5}')
  check xadc_temperature $([ $t -ge $TEMP_MIN_C ] && [ $t -le $TEMP_MAX_C ] && echo 1 || echo 0) \
        "$t C (expected $TEMP_MIN_C-$TEMP_MAX_C)"
  xadc_supply vccint  1000
  xadc_supply vccaux  1800
  xadc_supply vccbram 1000
  xadc_supply vccpint 1000
  xadc_supply vccpaux 1800
  xadc_supply vccoddr 1500   # DDR3
}

# *** ETHERNET ***

# sum of the error counters in ethtool -S
eth_errors(){
  ethtool -S eth0 2>/dev/null | awk '/(error|errors|underrun|collision|jabbers|undersized|oversize|overrun):/ {s += $2}
                                     END {print s+0}'
}

ethernet(){
  if [ ! -d /sys/class/net/eth0 ]; then
    result FAIL eth0 "no eth0"
    return
  fi
  carrier=$(cat /sys/class/net/eth0/carrier 2>/dev/null)
  speed=$(cat /sys/class/net/eth0/speed 2>/dev/null)
  duplex=$(cat /sys/class/net/eth0/duplex 2>/dev/null)
  check eth_link $([ "$carrier" = 1 ] && [ "$speed" = 1000 ] && [ "$duplex" = full ] && echo 1 || echo 0) \
        "carrier ${carrier:-?}, ${speed:-?} Mb/s, ${duplex:-?} (expected 1000, full)"

  # the MAC address from the Trenz CPLD:  globally administered, not zero
  mac=$(cat /sys/class/net/eth0/address)
  first=$((0x${mac%%:*}))
  check eth_mac $([ "$mac" != 00:00:00:00:00:00 ] && [ $((first & 3)) = 0 ] && echo 1 || echo 0) \
        "$mac (globally administered, unicast)"

  ip=$(ip -4 addr show eth0 | awk '/inet / {print $2; exit}')
  check eth_address $([ -n "$ip" ] && echo 1 || echo 0) "${ip:-none}"

  [ -n "$SERVER" ] || SERVER=$(ip route | awk '/^default/ {print $3; exit}')
  if [ -z "$SERVER" ]; then
    result FAIL eth_server "no default gateway: use --server IP"
    return
  fi
  result INFO eth_server "$SERVER"

  err0=$(eth_errors)
  for size in 56 1472; do
    count=$PING_COUNT; [ $size = 1472 ] && count=$PING_BIG_COUNT
    loss=$(ping -c $count -A -s $size $SERVER 2>&1 | awk -F, '/packet loss/ {print $3}' | tr -dc '0-9')
    [ -n "$loss" ] || loss=100
    check eth_ping_$size $([ "$loss" = 0 ] && echo 1 || echo 0) "$count x $size bytes, $loss% lost"
  done

  if [ $DO_TFTP = 1 ]; then
    sums=""
    for k in 1 2; do
      rm -f $WORK/tftp
      t0=$(now_cs)
      if ! tftp -g -r $TFTP_FILE -l $WORK/tftp $SERVER 2>/dev/null; then
        result FAIL eth_tftp "$TFTP_FILE from $SERVER: transfer failed"
        rm -f $WORK/tftp
        sums=""
        break
      fi
      t1=$(now_cs)
      bytes=$(wc -c < $WORK/tftp)
      cs=$((t1 - t0)); [ $cs -lt 1 ] && cs=1
      result INFO eth_tftp_$k "$TFTP_FILE: $bytes bytes in $((cs/100)).$(printf '%02d' $((cs%100))) s, $((bytes*8/cs/10000)) Mb/s (TFTP lockstep, not link speed)"
      sums="$sums $(md5sum < $WORK/tftp | cut -c1-32)"
    done
    rm -f $WORK/tftp
    if [ -n "$sums" ]; then
      set -- $sums
      check eth_tftp_integrity $([ "$1" = "$2" ] && echo 1 || echo 0) "md5 $1, $2"
    fi
  else
    result SKIP eth_tftp "--no_tftp"
  fi

  err1=$(eth_errors)
  check eth_errors $([ "$err1" = "$err0" ] && echo 1 || echo 0) "$((err1 - err0)) new (ethtool -S error counters)"
}

# *** QSPI FLASH ***

qspi(){
  if [ ! -r /proc/mtd ] || [ "$(grep -c '^mtd' /proc/mtd)" = 0 ]; then
    result FAIL qspi "no MTD partitions"
    return
  fi
  for name in qspi-boot qspi-kernel qspi-bootenv bootscr; do
    dev=$(awk -F: -v n="\"$name\"" '$2 ~ n {print $1}' /proc/mtd)
    if [ -z "$dev" ]; then
      result FAIL "qspi_$name" "partition missing"
      continue
    fi
    size=$((0x$(awk -v d=$dev: '$1 == d {print $2}' /proc/mtd)))
    # Read 3 times.  The first QSPI read after boot can return a wrong first byte (seen on the bench Trenz:
    # 0x01 at offset 0 of a blank flash, later reads fine): reported as INFO, not a failure (Michael
    # 2026-10-17).  Reads 2 and 3 must agree.
    dd if=/dev/${dev}ro of=$WORK/q1 bs=64k 2>/dev/null
    dd if=/dev/${dev}ro of=$WORK/q2 bs=64k 2>/dev/null
    dd if=/dev/${dev}ro of=$WORK/q3 bs=64k 2>/dev/null
    s1=$(md5sum < $WORK/q3 | cut -c1-32)
    blank=""; [ "$(tr -d '\377' < $WORK/q3 | wc -c)" = 0 ] && blank=", blank (erased)"
    nfirst=$(cmp -l $WORK/q1 $WORK/q2 | wc -l)
    if [ "$nfirst" != 0 ]; then
      first=$(cmp -l $WORK/q1 $WORK/q2 | awk 'NR == 1 {printf "%d", $1 - 1}')
      result INFO "qspi_${name}_first_read" "first read differed from later reads in $nfirst byte(s), first at offset $first (known first-read artifact, not a failure)"
    fi
    ndiff=$(cmp -l $WORK/q2 $WORK/q3 | wc -l)
    if [ "$ndiff" = 0 ]; then
      result PASS "qspi_$name" "/dev/$dev, $((size/1024)) kB$blank, read 3 times: md5 $s1"
    else
      first=$(cmp -l $WORK/q2 $WORK/q3 | awk 'NR == 1 {printf "%d", $1 - 1}')
      result FAIL "qspi_$name" "/dev/$dev$blank: reads differ in $ndiff byte(s), first at offset $first"
    fi
    rm -f $WORK/q1 $WORK/q2 $WORK/q3
  done
}

# *** eMMC ***

emmc(){
  if [ ! -b /dev/mmcblk1 ]; then
    result FAIL emmc "no /dev/mmcblk1"
    return
  fi
  mb=$(($(cat /sys/block/mmcblk1/size) / 2048))
  result INFO emmc_size "$mb MB"
  drop_caches
  t0=$(now_cs)
  s1=$(dd if=/dev/mmcblk1 bs=1M count=$EMMC_READ_MB 2>/dev/null | md5sum | cut -c1-32)
  t1=$(now_cs)
  drop_caches
  s2=$(dd if=/dev/mmcblk1 bs=1M count=$EMMC_READ_MB 2>/dev/null | md5sum | cut -c1-32)
  cs=$((t1 - t0)); [ $cs -lt 1 ] && cs=1
  check emmc_read $([ "$s1" = "$s2" ] && echo 1 || echo 0) \
        "first $EMMC_READ_MB MB read twice: md5 $s1, $((EMMC_READ_MB*100/cs)) MB/s"
}

# *** microSD ***

sd(){
  if [ ! -b /dev/mmcblk0 ]; then
    result SKIP sd_card "no card (/dev/mmcblk0)"
    return
  fi
  mb=$(($(cat /sys/block/mmcblk0/size) / 2048))
  parts=$(ls /sys/block/mmcblk0 | grep -c '^mmcblk0p')
  check sd_card $([ $parts -ge 1 ] && echo 1 || echo 0) "$mb MB, $parts partition(s)"
  [ $parts -ge 1 ] || return

  # the first partition, mounted where it is, or read-only here
  mnt=$(awk '$1 == "/dev/mmcblk0p1" {print $2; exit}' /proc/mounts)
  ours=0
  if [ -z "$mnt" ]; then
    mnt=$WORK/sd; mkdir -p $mnt
    if [ $SD_WRITE = 1 ]; then opt=rw; else opt=ro; fi
    if ! mount -o $opt /dev/mmcblk0p1 $mnt 2>/dev/null; then
      result FAIL sd_mount "/dev/mmcblk0p1 does not mount"
      return
    fi
    ours=1
  fi
  if [ -f $mnt/BOOT.BIN ]; then
    result INFO sd_boot_bin "$(wc -c < $mnt/BOOT.BIN) bytes, md5 $(md5sum < $mnt/BOOT.BIN | cut -c1-32)"
  else
    result INFO sd_boot_bin "no BOOT.BIN on the first partition"
  fi

  if [ $SD_WRITE = 1 ]; then
    f=$mnt/pacman_lnxcheck.tmp
    dd if=/dev/urandom of=$WORK/sd_data bs=1M count=$SD_WRITE_MB 2>/dev/null
    want=$(md5sum < $WORK/sd_data | cut -c1-32)
    if cp $WORK/sd_data $f 2>/dev/null; then
      drop_caches
      got=$(md5sum < $f | cut -c1-32)
      rm -f $f; sync
      check sd_write "$([ "$got" = "$want" ] && echo 1 || echo 0)" "$SD_WRITE_MB MB written, read back: md5 $got (expected $want)"
    else
      rm -f $f
      result FAIL sd_write "cannot write to the first partition"
    fi
    rm -f $WORK/sd_data
  fi
  [ $ours = 1 ] && umount $mnt
}

# *** MAIN ***

while [ $# -gt 0 ]; do
  case "$1" in
    --server)     [ $# -ge 2 ] || { usage; exit 3; }; SERVER=$2; shift ;;
    --tftp_file)  [ $# -ge 2 ] || { usage; exit 3; }; TFTP_FILE=$2; shift ;;
    --no_tftp)    DO_TFTP=0 ;;
    --sd_write)   SD_WRITE=1 ;;
    --help)       usage; exit 0 ;;
    *)            usage; exit 3 ;;
  esac
  shift
done

if [ "$(id -u)" != 0 ]; then
  echo "pacman_lnxcheck:  needs root (raw reads of flash, eMMC; drop_caches)."
  exit 3
fi

echo "pacman_lnxcheck:  embedded-Linux checkout of the Trenz module."
mkdir -p $WORK
trap 'rm -rf $WORK' EXIT

system
xadc
ethernet
qspi
emmc
sd

if [ $NFAIL -gt 0 ]; then verdict=FAIL; code=1
elif [ $NSKIP -gt 0 ]; then verdict=INCOMPLETE; code=2
else verdict=PASS; code=0; fi
echo "VERDICT $verdict  (pass $NPASS, fail $NFAIL, skip $NSKIP)"
exit $code
