#!/bin/sh
# Decide which OSD layer can actually carry the UI.
# S1 = bands on fb1 (osd1)   S2 = bands on fb0 (osd0)   S3 = solid blue on fb1
L=/root/probe3.log; A=/sys/class/display/axis
: > $L
st() { echo "[$(date +%H:%M:%S)] $1" >> $L; echo "probe3: $1" > /dev/kmsg; }
echo 1080p60hz > /sys/class/display/mode 2>>$L
echo 0 > /sys/class/graphics/fb1/blank 2>>$L
fbset -fb /dev/fb1 -g 1920 1080 1920 1080 32 2>>$L
st "setup rc=$? fb1 geom now: $(fbset -fb /dev/fb1 2>/dev/null | sed -n 4p)"
echo "osd1,0,0,1919,1079" > $A 2>>$L
echo "osd0,0,0,0,0" > $A 2>>$L
st "S1 begin: bands on fb1/osd1, osd0 window zeroed"
/root/fbtest 10 0 /dev/fb1 >>$L 2>&1
st "S2 begin: bands on fb0/osd0 (osd1 window zeroed, osd0 fullscreen)"
echo "osd1,0,0,0,0" > $A 2>>$L
echo "osd0,0,0,1919,1079" > $A 2>>$L
/root/fbtest 10 0 /dev/fb0 >>$L 2>&1
st "S3 begin: SOLID BLUE on fb1/osd1"
echo "osd0,0,0,0,0" > $A 2>>$L
echo "osd1,0,0,1919,1079" > $A 2>>$L
/root/fbtest 12 5 /dev/fb1 >>$L 2>&1
st "end: leaving blue on fb1"
