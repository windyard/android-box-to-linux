#!/bin/sh
# Sync the clock from public NTP after every boot, then record it in the RTC.
#
# This box has no RTC battery, so /dev/rtc0 comes up at the rc.local floor
# (2026-09-19 00:00) on every cold boot.  That floor is what keeps TLS and apk
# cert checks working; it is NOT the real time and sits ~2 days behind, so file
# mtimes and git commit dates are wrong until something corrects it.
#
# Why a separate script.  rc.local used to fire one 'ntpd -q' inline, before
# eth0 had a lease, at the LAN router.  Two defects: it races DHCP and loses on
# any boot where the lease is slow, and a failed attempt is indistinguishable
# from a successful one (see below).  Retrying fixes the race; verifying fixes
# the ambiguity.
#
# busybox ntpd v1.36.1, as measured on this box:
#   -q   steps the clock, but DAEMONISES, and exits 0 in ~0s against a peer
#        that never answered.  Its exit status carries no information.
#   -w   queries only (-w implies -n, so it stays in the foreground and can be
#        bounded by timeout) and prints 'reply from IP: offset:+0.0016 strat:N'.
#        This is the only trustworthy signal available here.
# So each server is proven reachable with -w before it is trusted to step, and
# the step is then confirmed by a second -w whose offset must be small.
#
# sntp / ntpdate / ntpclient are not installed; chrony or openntpd would give a
# real exit status, but neither is worth a standing daemon on 512 MB.
#
# Safe to run by hand any time.  Exits 0 on verified sync, 1 if it never got
# there.  The log is stamped with seconds-since-boot rather than with the
# clock, because the clock is the thing being fixed.

LOG=/var/log/time-up.log
exec >>"$LOG" 2>&1

# Public servers only.  The first two are reachable from a Chinese ISP network;
# swap in 0.pool.ntp.org if outbound UDP/123 to these is ever blocked.
SERVERS=${SERVERS-"ntp.aliyun.com cn.pool.ntp.org pool.ntp.org"}
TRIES=${TRIES-32}

# -w never exits on its own, so every query costs its full timeout.  8s allows
# for several sample exchanges on a peer that answers in single-digit ms.
QT=${QT-8}

say() { printf '[%5ss] %s\n' "$(cut -d. -f1 /proc/uptime)" "$*"; }

# Last offset reported by a bounded -w query, in signed whole seconds.
# Prints nothing if the peer did not answer.
#
# The '2>&1' on ntpd is load-bearing, not tidiness: busybox ntpd prints its
# 'reply from ... offset:...' lines on STDERR.  Written as 2>/dev/null this
# function silently returns empty against a perfectly healthy peer - measured
# 3/3 vs 3/3 against ntp.aliyun.com.  The outer '2>/dev/null' is what actually
# wants suppressing, namely the shell's 'Terminated' report for the killed
# pipeline, which is normal here because -w has no clean exit.
query() {
    ( timeout "$QT" busybox ntpd -p "$1" -w 2>&1 \
        | grep -o 'offset:[+-][0-9]*' | tail -1 | sed 's/offset://' ) 2>/dev/null
}

# -q daemonises, so it must only ever be started from here, after a peer has
# proven itself, and always followed by a wait for it to finish stepping.
step() {
    ( busybox ntpd -p "$1" -q >/dev/null 2>&1 ) &
    sp=$!
    n=0
    while kill -0 "$sp" 2>/dev/null && [ "$n" -lt 10 ]; do
        sleep 1; n=$((n + 1))
    done
    kill "$sp" 2>/dev/null
    wait "$sp" 2>/dev/null
}

attempt() {
    local srv pre off
    for srv in $SERVERS; do
        pre=$(query "$srv")
        [ -n "$pre" ] || { say "$srv unreachable"; continue; }
        step "$srv"
        off=$(query "$srv")
        if [ -n "$off" ] && [ "${off#-}" -le 2 ]; then
            say "$srv: was ${pre}s, now ${off}s -> $(date -u '+%F %T UTC')"
            return 0
        fi
        say "$srv: stepped but still off (${off:-no reply}s), trying next"
    done
    return 1
}

# A default route implies a lease, and these servers are named, so it also
# implies a resolver worth asking.  An unresolvable name fails and retries.
have_net() { ip route 2>/dev/null | grep -q '^default'; }

say "start floor=$(date -u +%FT%TZ) servers='$SERVERS'"

# Fast polling for the first ~2 minutes to catch the usual lease-in-5s case,
# then slower for ~4 more to cover a WiFi link that is still associating.
i=0
while [ "$i" -lt "$TRIES" ]; do
    if have_net && attempt; then
        # Only ever write a time we believe - writing the floor would re-seed
        # the exact problem this script exists to fix.
        busybox hwclock -w && say "rtc set: $(busybox hwclock -r 2>&1)"
        say "done"
        exit 0
    fi
    i=$((i + 1))
    [ "$i" -lt $((TRIES * 3 / 4)) ] && sleep 5 || sleep 30
done

say "giving up after $TRIES attempts; clock left at $(date -u +%FT%TZ)"
exit 1
