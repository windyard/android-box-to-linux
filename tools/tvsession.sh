#!/bin/sh
# Runs in the R1200C ramfs bind shell. Prepares + installs the auto-Alpine boot.
BB=/bin/busybox
$BB mkdir -p /mnt/usb /mnt/root
$BB umount /mnt/usb 2>/dev/null
$BB mount -t vfat -o rw,noatime /dev/sda1 /mnt/usb && echo STICK_OK || echo STICK_FAIL
$BB rm -f /mnt/usb/rootfs.img /mnt/usb/rootfs.img.gz /mnt/usb/unpack.done
$BB wget -q -O /mnt/usb/rootfs.img.gz http://192.0.2.220:8000/work/rootfs.img.gz && echo GZDL_OK
$BB nohup $BB sh -c "$BB gunzip -c /mnt/usb/rootfs.img.gz >/mnt/usb/rootfs.img; echo rc=\$? >/mnt/usb/unpack.done" >/tmp/u.log 2>&1 &
echo UNPACK_LAUNCHED
