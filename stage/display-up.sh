#!/bin/sh
# R1200C OSD/HDMI recipe re-apply, every boot (README §7.3 + §7.7 fix).
# Order is load-bearing:
#   - fbset before mode write; ANY display/mode write re-enables the fb1 logo
#     plane, so blank fb1 AFTER the mode write.
#   - free_scale_axis must be programmed after mode-set; its boot default is
#     the degenerate "0 0 0 0" while free_scale_enable=0x10001, which makes the
#     vertical scaler paint a luminance ramp over ALL content (the "gradient").
#   - the VIU latches the canvas only on FBIOPUT(FORCE)+FBIOPAN: fbfill does
#     that as a side effect; osd_do_hwc completes hardware composition.
#
# Output mode is 1080p30hz, NOT 1080p60hz: the link fails on TMDS *clock rate*,
# not on resolution.  1080p60 and 1080p50 both need 148.5 MHz, which this
# cable/TV combination cannot lock - the sink drops HPD every ~8s and each
# replug re-runs EDID + mode-set, which re-enables the logo plane and discards
# the latched canvas (panel reads "no signal" while hdmi_init/cur_VIC look
# healthy).  Anything at 74.25 MHz locks solid: measured 0 HPD drops/12s at
# 1080p30, 1080p25 and 1080i50.  1080p30 is picked as the highest-quality of
# those (full 1920x1080 progressive, 30Hz refresh).  Go back to 1080p60hz only
# after the cable/input is confirmed good at 148.5 MHz.
MODE=1080p30hz
W=1920
H=1080
X1=$((W - 1))
Y1=$((H - 1))

LOG=/var/log/display-up.log
exec >>"$LOG" 2>&1
echo "=== $(date) display-up start ($MODE ${W}x${H})"

# Take the previous session down FIRST. A live Xorg owns fb0 and restores its
# own var (1920x1080) over our fbset, which leaves the framebuffer and the OSD
# axes disagreeing.  Never pkill -f here - the pattern matches this shell's own
# command line and kills the session (README §5.3).
if pidof Xorg >/dev/null; then
    kill $(pidof xfce4-session) 2>/dev/null
    kill $(pidof Xorg) 2>/dev/null
    sleep 3
fi

fbset -fb /dev/fb0 -g $W $H $W $H 16
echo $MODE > /sys/class/display/mode
echo 1 > /sys/class/graphics/fb1/blank
echo 0 > /sys/class/graphics/fb0/ver_clone
echo "osd0,0,0,$X1,$Y1" > /sys/class/display/axis
echo "0 0 $X1 $Y1" > /sys/class/graphics/fb0/free_scale_axis
echo "0 0 $X1 $Y1" > /sys/class/graphics/fb0/window_axis

/root/fbfill /dev/fb0 000000 1
echo 1 > /sys/class/graphics/fb0/osd_do_hwc

# Cold-boot fixes (2026-09-21, verified same day):
#  - no udev daemon on this box => X hotplugs NOTHING (USB mouse invisible).
#    Start eudev and enumerate current devices before Xorg reads the queue.
#  - usbcore autosuspend defaults to 2s here and the xhci port DROPS the
#    suspended mouse off the bus (disconnect/re-enumerate every ~2s). -1 off.
echo -1 > /sys/module/usbcore/parameters/autosuspend
if ! pidof udevd >/dev/null; then
    /sbin/udevd --daemon
    sleep 1
    udevadm trigger >/dev/null 2>&1
fi

/usr/bin/Xorg :0 vt7 >>/var/log/Xorg.start.log 2>&1 &
sleep 5
# Session: XFCE4 is the default since 2026-09-21 (panel + xfdesktop icons +
# thunar + settings; verified live: 199 MB used with everything loaded).
# HOME must be set explicitly: rc.local runs with no HOME.
# Fallback if XFCE ever misbehaves: openbox-session (still installed), which
# runs ~/.config/openbox/autostart.
HOME=/root DISPLAY=:0 dbus-launch startxfce4 >/dev/null 2>&1 &
sleep 3
# X's own mode-set is non-FORCE: latch ONCE here at the ownership transfer;
# later X updates (taskbar, windows) reach the panel without relatching
# (verified live, §7.7.7). The cursor is a hardware plane, always visible.
/root/fblatch
echo 1 > /sys/class/graphics/fb0/osd_do_hwc
echo "=== display-up done: Xorg=$(pidof Xorg) xfce4-session=$(pidof xfce4-session) udevd=$(pidof udevd) autosusp=$(cat /sys/module/usbcore/parameters/autosuspend)"
