#!/bin/sh
# After rootfs.img is fully unpacked: flash the fixed recovery image, enable
# autoboot, verify, then drop to Android via a plain reboot.
BB=/bin/busybox
$BB cat /mnt/usb/unpack.done; $BB ls -l /mnt/usb/rootfs.img
SZ=$($BB cat /mnt/usb/rootfs.img | $BB wc -c)
$BB wget -q -O /tmp/tvboot.img http://192.0.2.220:8000/stage/tvboot.img && echo BOOTDL
$BB md5sum /tmp/tvboot.img
$BB dd if=/tmp/tvboot.img of=/dev/block/recovery bs=8192 2>/dev/null; $BB sync
$BB dd if=/dev/block/recovery bs=4096 count=2664 2>/dev/null | $BB md5sum
$BB touch /mnt/usb/autoboot; $BB sync
echo FINISHED size=$SZ
$BB echo b > /proc/sysrq-trigger
