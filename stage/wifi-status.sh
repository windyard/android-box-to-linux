#!/bin/sh
# WiFi status for the XFCE panel (xfce4-genmon-plugin). README §6.
# Output is Pango markup, so keep it single-line and cheap: genmon re-runs this
# on its own interval, every tick.
#
# Three states, deliberately separated, because on this chip "no wlan0" is NOT
# the same failure as "not associated": the SDIO driver registers sdiohal once
# and can never be reloaded (§6.5), so a missing interface means power-cycle,
# while a missing association is just a wrong/absent passphrase.

WLAN=/sys/class/net/wlan0
CONF=${WPA_CONF-/etc/wpa_supplicant/wpa_supplicant.conf}

RED='#ff6b6b'
ORANGE='#ffb86c'
GREEN='#8fe388'
GREY='#9aa5b1'

emit() { # emit <color> <text>
    printf '<txt><span foreground="%s">%s</span></txt>' "$1" "$2"
}

if [ ! -e "$WLAN" ]; then
    emit "$RED" 'WiFi: no interface (sdio one-shot — power-cycle)'
    exit 0
fi

OPERSTATE=$(cat "$WLAN/operstate" 2>/dev/null)
LINK=$(iw dev wlan0 link 2>/dev/null)

if ! printf '%s' "$LINK" | grep -q '^Connected to'; then
    if printf '%s' "$LINK" | grep -q '^Not connected'; then
        SSID_WANT=$(grep -m1 'ssid=' "$CONF" 2>/dev/null | sed -E 's/.*ssid="(.*)".*/\1/')
        emit "$ORANGE" "WiFi: not connected${SSID_WANT:+ (want $SSID_WANT)} — state $OPERSTATE"
    else
        emit "$ORANGE" 'WiFi: associating…'
    fi
    exit 0
fi

# iw indents every field with a TAB and lower-cases the link's signal line:
#   Connected to 02:aa:bb:cc:dd:a3 (on wlan0)
#   \tSSID: MyWiFi24
#   \tfreq: 2442.0
#   \tsignal: -34 dBm
SSID=$(printf '%s\n' "$LINK" | sed -n 's/^[ \t]*SSID:[ \t]*//p')
FREQ=$(printf '%s\n' "$LINK" | sed -n 's/^[ \t]*freq:[ \t]*//p' | cut -d. -f1)
SIGNAL=$(printf '%s\n' "$LINK" | sed -n 's/^[ \t]*signal:[ \t]*//p' | awk '{print $1; exit}')
IP=$(ip -4 addr show dev wlan0 2>/dev/null | awk '/inet /{sub(/\/.*/,"",$2); print $2; exit}')

# Coarse bars: dBm is negative, so compare against thresholds top-down.
BARS='='
if [ -n "$SIGNAL" ]; then
    [ "$SIGNAL" -le -71 ] || BARS='=='
    [ "$SIGNAL" -le -61 ] || BARS='==='
    [ "$SIGNAL" -le -51 ] || BARS='===='
fi

emit "$GREEN" "WiFi: ${SSID:-?} ${BARS} ${SIGNAL:-?}dBm ${IP:-no-addr} ${FREQ:-?}MHz"
