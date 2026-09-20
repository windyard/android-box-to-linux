#!/bin/sh
# Phased HDMI/OSD diagnostic. Each phase lasts ~6s and is announced with a
# timestamp so the on-screen reaction can be matched to the phase number.
# Everything here is in-RAM and reverts on reboot.
H=/sys/class/amhdmitx/amhdmitx0
LOG=/root/hdmiprobe.log
: > $LOG
say() { echo "PHASE $1: $2  [$(date '+%H:%M:%S')]" | tee -a $LOG; echo "hdmiprobe $1: $2" > /dev/kmsg; }
p() { say "$1" "$2"; sleep 6; }

p 0 "baseline: doing nothing (what is on screen now = reference)"
echo 1 > $H/vid_mute 2>>$LOG
p 1 "HDMI TX video MUTE (expect: screen goes BLACK -> TX is driven by Linux)"
echo 0 > $H/vid_mute 2>>$LOG
p 2 "HDMI TX unmute (expect: the splash picture COMES BACK)"
echo 1 > /sys/class/graphics/fb1/blank 2>>$LOG
p 3 "blank OSD1 / logo layer (expect: splash DISAPPEARS -> splash was on fb1)"
/root/fbtest 6 5 2>>$LOG
say 4 "fb0 filled SOLID BLUE + fb1 still blanked (expect: all blue -> fb0 scanout live)"
echo 0 > /sys/class/graphics/fb1/blank 2>>$LOG
echo 2 > /sys/module/fb/parameters/osd_logo_index 2>>$LOG
echo 1080p60hz > /sys/class/display/mode 2>>$LOG
p 5 "logo index moved off osd1, mode re-applied (expect: splash gone or frozen)"
/root/fbtest 8 0 2>>$LOG
say 6 "fb0 three colour bands B-G-R (final)"
echo 1080p60hz > /sys/class/display/mode 2>>$LOG
echo "END [$(date '+%H:%M:%S')]" | tee -a $LOG
