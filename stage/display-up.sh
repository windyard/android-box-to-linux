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
# Output mode 1080p60hz (VIC 16, 148.5 MHz).
#
# IMPORTANT CORRECTION (2026-09-21).  Every earlier note in this file and in
# README 7.7.8 claimed the sink drops HPD at 148.5 MHz and that only 74.25 MHz
# modes lock, so 1080p30 was pinned as the "highest safe" mode.  That was
# wrong, and it was wrong for two separate reasons:
#   1. The measurements behind it were 12s and 25s windows.  One HPD blip lasts
#      about a second and the gaps between them run tens of seconds, so a short
#      window is not evidence of a lock - it is evidence of nothing.
#   2. 720p50 and 1080p30 share the SAME 74.25 MHz clock, yet one sampled clean
#      and the other flapped.  A variable that does not vary with the effect
#      cannot be the cause.
# Re-measured today, 30-60s per mode, after the link had been re-established a
# few times: 1080p60 0/30 and 0/60, 1080p25 0/30, 1080i50 0/30, 720p60 0/30,
# 720p50 0/40.  Every mode stable, including the one previously declared
# unusable.  The only flapping seen was in the first minutes after a boot-run
# recipe, independent of mode - i.e. a boot-pass problem, not a clock limit.
# The fix below is therefore unproven and deliberately conservative: if a cold
# boot reintroduces flapping, re-run this script once and re-measure rather
# than falling back to 74.25 MHz, which buys nothing.
# The sink is a Xiaomi "Mi TV" (real EDID, CEA extension, 4K-class) whose
# declared preferred timing is 720p50hz; feeding it 1080p60 is outside that
# preference and works, and 1080p60 also crops less at the panel edges.
MODE=1080p60hz
W=1920
H=1080
X1=$((W - 1))
Y1=$((H - 1))

LOG=/var/log/display-up.log
exec >>"$LOG" 2>&1
echo "=== $(date) display-up start ($MODE ${W}x${H})"

# Boot-window HPD observer (added 2026-09-21).  Every claim about which modes
# lock has been made from samples taken after the fact, which is what produced
# the wrong 148.5 MHz conclusion corrected above.  This records the link from
# the moment the recipe starts through ~3min, so a cold boot is judged on data
# rather than on a short lucky window.  Costs one shell + one sleep per boot.
( hpdl=/var/log/hpd-boot.log
  echo "=== $(date -u +%FT%TZ) uptime=$(cut -d. -f1 /proc/uptime)s" >>"$hpdl"
  z=0; n=0
  while [ "$n" -lt 180 ]; do
      v=$(cat /sys/class/amhdmitx/amhdmitx0/hpd_state 2>/dev/null)
      [ "$v" = 0 ] && { z=$((z + 1)); printf 'DROP at %ss\n' "$(cut -d. -f1 /proc/uptime)" >>"$hpdl"; }
      n=$((n + 1)); sleep 1
  done
  echo "=== window 180s drops=$z" >>"$hpdl" ) &

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
UDEVD_PID=""
if ! pidof udevd >/dev/null; then
    /sbin/udevd --daemon
    UDEVD_PID=$!
    sleep 1
    udevadm trigger >/dev/null 2>&1
fi
# No pidof here: eudev workers are forked from the daemon and share its comm
# *and* its /proc/PID/exe, so both 'pidof udevd' and 'pidof /sbin/udevd' print
# every worker (17 PIDs observed).  The $! from --daemon is the real daemon.

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
echo "=== display-up done: Xorg=$(pidof Xorg) xfce4-session=$(pidof xfce4-session) udevd=${UDEVD_PID:-pre-existing} autosusp=$(cat /sys/module/usbcore/parameters/autosuspend)"
