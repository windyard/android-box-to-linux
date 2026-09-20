#!/bin/sh
# One-shot UWE5623/MARLIN3E WiFi bring-up.
# NOTE: uwe5621_bsp_sdio registers the sdiohal bus driver at init and never
# unregisters it, so this script is valid exactly ONCE per boot.
cd /root
: > /root/up.log
exec >>/root/up.log 2>&1
echo "== free =="; free -m; echo "== ini =="; ls -l /vendor/etc/wifi/uwe5621/
mkdir -p /data/misc/wifi
dmesg -c >/dev/null
date
/root/wificmd 0x00006d03 1
sleep 1
insmod /root/uwe5621_bsp_sdio.ko; echo "insmod bsp rc=$?"
sleep 2
insmod /root/sprdwl_ng.ko; echo "insmod sprdwl rc=$?"
sleep 15
echo "== net =="; ls /sys/class/net/
echo "== mac file =="; ls -l /data/misc/wifi/; cat /data/misc/wifi/wifimac.txt 2>/dev/null
echo "== lsmod =="; lsmod | grep -E "sprd|uwe"
echo "== free =="; free -m
echo "== dmesg =="
dmesg | grep -viE "hdmitx|edid|up_phy_addr|^[0-9. ]*[0-9a-f]{8,}"
