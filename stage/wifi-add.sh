#!/bin/sh
# Join, switch or forget a WiFi network on the built-in UWE5623.
#
# This replaces the network-manager GUI this box cannot have (README §6.7):
#  - the SDIO driver registers 'sdiohal' once and never unregisters it, so this
#    script MUST NOT reload modules; missing wlan0 means power-cycle (§6.5).
#  - only the 64-hex PMK is ever written to disk, mode 0600. The passphrase is
#    read from a terminal (echo off) or from stdin, is never an argv element
#    (argv leaks through /proc), and is unset before we finish.
#  - eth0 must stay the preferred default route, so the wired SSH lifeline
#    survives a bad passphrase (§3.5, §6.6).
#
# Usage:
#   wifi-add.sh                  interactive: scan, pick, type passphrase
#   wifi-add.sh "SSID"           passphrase from stdin or typed
#   wifi-add.sh list             scan and print networks, change nothing
#   wifi-add.sh forget "SSID"    drop that network and reconnect
#
# Recommended from a workstation (keeps the secret out of shell history on the
# box, and needs no keyboard attached to it):
#   printf '%s' 'the passphrase' | ssh root@192.0.2.126 wifi-add.sh 'MyWiFi24'

CONF=${WPA_CONF-/etc/wpa_supplicant/wpa_supplicant.conf}
PIDF=/run/wpa_supplicant.pid
GW=192.0.2.1
TESTIP=223.5.5.5

die() { printf 'wifi-add: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }

ensure_wlan0() {
    [ -e /sys/class/net/wlan0 ] && return 0
    say "wlan0 does not exist."
    if lsmod 2>/dev/null | grep -q '^uwe5621_bsp_sdio'; then
        die "the SDIO driver is already loaded but produced no interface.
sdiohal cannot be unregistered, so the driver cannot be reloaded this boot.
Recovery is a power cycle (then /usr/local/bin/wifi-up.sh runs from rc.local)."
    else
        say "modules are not loaded yet - running the boot bring-up now."
        /usr/local/bin/wifi-up.sh
        [ -e /sys/class/net/wlan0 ] || die "wifi-up.sh did not produce wlan0; see /var/log/wifi-up.log"
    fi
}

# Scan and emit "signal<TAB>SSID", strongest first, one row per SSID.
# iw separates BSS blocks with blank lines and no terminator, so flush on the
# next "BSS " header and once at EOF. Tabs keep the SSID intact (it may contain
# spaces, '(' or '-') for the caller to display or select from.
scan_list() {
    iw dev wlan0 scan 2>/dev/null | awk '
        function flush() { if (ssid != "") print sig "\t" ssid }
        /^BSS /              { flush(); sig = ""; ssid = "" }
        /^[ \t]*signal:[ \t]/ { sig = $2 }
        /^[ \t]*SSID:[ \t]/   { s = $0; sub(/^[ \t]*SSID:[ \t]*/, "", s); ssid = s }
        END                  { flush() }
    ' | sort -rn | awk -F'\t' '$1 != "" && $2 != "" && !seen[$2]++'
}

show_list() { # numbered human-readable form of scan_list
    awk -F'\t' '{ printf "%d)  %5.0f dBm  %s\n", NR, $1, $2 }'
}

read_passphrase() { # sets $PASS; never echoes it
    if [ -t 0 ]; then
        printf 'Passphrase for %s: ' "$SSID"
        stty -echo 2>/dev/null
        IFS= read -r PASS
        stty echo 2>/dev/null
        printf '\n'
    else
        IFS= read -r PASS
    fi
    [ -n "$PASS" ] || die "empty passphrase"
}

# Rebuild CONF from the old one, minus any block for $SSID, plus the new block.
# Written to a temp file and mv'd, so a crash cannot leave a half-written config.
write_network() { # requires $SSID $PMK
    TMP="${CONF}.new"
    awk -v drop="$SSID" '
        BEGIN { inblk=0; keep=1 }
        /^network=\{/ { inblk=1; keep=1; buf="network={\n"; next }
        inblk {
            buf = buf $0 "\n"
            if ($0 ~ /^\tssid="/) {
                s=$0; sub(/^\tssid="/,"",s); sub(/"[\r\t ]*$/,"",s)
                if (s == drop) keep=0
            }
            if ($0 ~ /^\}/) { if (keep) printf "%s", buf; inblk=0 }
            next
        }
        { print }
    ' "$CONF" > "$TMP" || { rm -f "$TMP"; die "could not rewrite $CONF"; }

    printf 'network={\n\tssid="%s"\n\tpsk=%s\n}\n' "$SSID" "$PMK" >> "$TMP"
    chmod 600 "$TMP"
    mv -f "$TMP" "$CONF" || die "could not install $CONF"
}

drop_network() { # requires $SSID
    TMP="${CONF}.new"
    awk -v drop="$SSID" '
        BEGIN { inblk=0; keep=1; found=0 }
        /^network=\{/ { inblk=1; keep=1; buf="network={\n"; next }
        inblk {
            buf = buf $0 "\n"
            if ($0 ~ /^\tssid="/) {
                s=$0; sub(/^\tssid="/,"",s); sub(/"[\r\t ]*$/,"",s)
                if (s == drop) { keep=0; found=1 }
            }
            if ($0 ~ /^\}/) { if (keep) printf "%s", buf; inblk=0 }
            next
        }
        { print }
        END { exit (found ? 0 : 3) }
    ' "$CONF" > "$TMP"
    st=$?
    if [ $st -ne 0 ]; then rm -f "$TMP"; die "'$SSID' is not in $CONF"; fi
    chmod 600 "$TMP"; mv -f "$TMP" "$CONF"
    say "removed '$SSID' from $CONF"
}

bounce_supplicant() {
    [ -f "$CONF" ] || die "no $CONF"
    if pidof wpa_supplicant >/dev/null; then
        kill $(pidof wpa_supplicant) 2>/dev/null
        sleep 1
    fi
    rm -f "$PIDF"
    # wpa_supplicant on this box has NO -f option and exits 0 after printing
    # usage, which hides a bad invocation completely (§6.7). These args are the
    # proven form.
    wpa_supplicant -B -Dnl80211 -iwlan0 -c "$CONF" -P "$PIDF" \
        || die "wpa_supplicant failed to start"

    i=0
    while [ "$i" -lt 30 ]; do
        iw dev wlan0 link 2>/dev/null | grep -q '^Connected to' && break
        sleep 1; i=$((i+1))
    done
    iw dev wlan0 link 2>/dev/null | grep -q '^Connected to' || {
        say "not associated after ${i}s - wrong passphrase, or AP out of range."
        say "eth0 still carries traffic; nothing else was changed."
        iw dev wlan0 link 2>/dev/null | head -3
        exit 1
    }
    say "connected after ${i}s:"
    iw dev wlan0 link 2>/dev/null | head -4
}

renew_and_verify() {
    udhcpc -i wlan0 -b -n -t 20 -s /usr/share/udhcpc/default.script >/dev/null 2>&1
    # Enforce the routing invariant: wired first, WiFi demoted to metric 308.
    if ! ip route show | grep -q "^default via $GW dev eth0"; then
        ip route add default via $GW dev eth0 metric 0 2>/dev/null
    fi
    ip route show | grep "^default via $GW dev wlan0" | grep -q "metric 308" || {
        ip route del default dev wlan0 2>/dev/null
        ip route add default via $GW dev wlan0 metric 308 2>/dev/null
    }
    say "routes:"; ip route | grep '^default' | sed 's/^/  /'
    if ping -c 3 -W 3 -I wlan0 $TESTIP >/dev/null 2>&1; then
        say "internet via wlan0: OK"
    else
        say "internet via wlan0: FAIL (check the AP's WAN uplink)"
    fi
}

cmd=$1
case $cmd in
    list)
        ensure_wlan0
        say "scanning…"
        scan_list | show_list
        exit 0
        ;;
    forget)
        SSID=$2
        [ -n "$SSID" ] || die "usage: wifi-add.sh forget \"SSID\""
        ensure_wlan0
        drop_network
        bounce_supplicant
        renew_and_verify
        exit 0
        ;;
esac

SSID=$cmd
ensure_wlan0

if [ -z "$SSID" ]; then
    say "scanning…"
    NETS=$(scan_list)
    if [ -n "$NETS" ]; then
        printf '%s\n' "$NETS" | show_list
    else
        say "(scan returned nothing - you can still type a hidden SSID)"
    fi
    printf 'Pick a number, or type an SSID: '
    IFS= read -r ANSWER
    case "$ANSWER" in
        ''|*[!0-9]*) SSID=$ANSWER ;;
        *) SSID=$(printf '%s\n' "$NETS" | awk -F'\t' -v n="$ANSWER" 'NR==n{print $2}') ;;
    esac
    [ -n "$SSID" ] || die "no network selected"
fi

# An SSID carrying quotes or backslashes would break out of its conf block.
case $SSID in
    *'"'*|*'\'*) die "SSID contains a quote or backslash - refusing" ;;
esac
say "joining: $SSID"

read_passphrase
# wpa_passphrase prints both the plaintext (as "#psk=") and the PMK (as "psk=").
# Extract by line prefix, NOT by field equality: the whole "psk=<hex>" is one
# field, and the leading '#' keeps the plaintext line from ever matching.
PMK=$(printf '%s\n' "$PASS" | wpa_passphrase "$SSID" | sed -n 's/^[ \t]*psk=//p' | head -1)
unset PASS
[ ${#PMK} -eq 64 ] || die "wpa_passphrase produced no PMK"

write_network
say "installed PMK for '$SSID' in $CONF (mode 600, no plaintext on disk)"
bounce_supplicant
renew_and_verify
say "done."
