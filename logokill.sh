#!/bin/sh
# Paint osd0 continuously, THEN try to remove the boot-logo layer, watching
# what the TV does at each step. All in-RAM; reverts on reboot.
L=/root/logokill.log
: > $L
st() { echo "[$(date +%H:%M:%S)] $1" >> $L; echo "logokill: $1" > /dev/kmsg; }
echo 0 > /sys/class/display/bist 2>>$L
st "bist off; starting continuous BANDS paint on osd0"
/root/fbtest 60 0 >>$L 2>&1 &
PID=$!
sleep 3
st "t+3: painting; screen state A (bands if osd0 scanout works, else logo/black)"
sleep 4
echo 1 > /sys/class/graphics/fb1/blank 2>>$L
st "t+7: echo 1 > fb1/blank  (state B)"
sleep 5
echo "osd1,0,0,0,0" > /sys/class/display/axis 2>>$L
echo "osd2,0,0,0,0" > /sys/class/display/axis 2>>$L
echo 3 > /sys/module/fb/parameters/osd_logo_index 2>>$L
st "t+12: osd1+osd2 windows zeroed, logo index=3 (state C)"
sleep 6
echo 0 > /sys/class/graphics/fb1/blank 2>>$L
st "t+18: fb1 unblanked again (state D - should restore whatever fb1 held)"
sleep 5
echo 1 > /sys/class/graphics/fb1/blank 2>>$L
echo "osd0,0,0,1919,1079" > /sys/class/display/axis 2>>$L
st "t+23: fb1 blank + osd0 fullscreen axis (state E)"
sleep 12
st "t+35: fb0 osd_status=$(cat /sys/class/graphics/fb0/osd_status) window=$(cat /sys/class/graphics/fb0/window_axis) fb1=$(cat /sys/class/graphics/fb1/osd_status)"
st "t+35: bist=$(cat /sys/class/display/bist) mode=$(cat /sys/class/display/mode)"
sleep 28
st "done"
