#!/bin/bash
# Monitor headless mainline boot: does the box reappear as Armbian (.199) or return as Alpine (.126)?
# Detect by ICMP, TCP/22, and a successful SSH uname. Also watch the box MAC in the neighbor table.
BOXMAC=02:aa:bb:cc:dd:01
for i in $(seq 1 90); do
  ts=$(date +%H:%M:%S)
  # who answers on the wire for the box MAC right now?
  nb=$(ip neigh 2>/dev/null | grep -i "$BOXMAC" | head -1 | awk '{print $1}')
  # Armbian candidate .199
  a_p=$(ping -c1 -W1 192.0.2.199 >/dev/null 2>&1 && echo ping || echo -)
  a_t=$(timeout 2 bash -c 'echo>/dev/tcp/192.0.2.199/22' 2>/dev/null && echo tcp || echo -)
  a_s=$(ssh -o ConnectTimeout=2 -o StrictHostKeyChecking=no -o BatchMode=yes root@192.0.2.199 'uname -r' 2>/dev/null)
  # Alpine .126
  b_s=$(ssh -o ConnectTimeout=2 -o StrictHostKeyChecking=no -o BatchMode=yes root@192.0.2.126 'uname -r' 2>/dev/null)
  echo "$ts t=$i | .199 ping=$a_p tcp=$a_t ssh='${a_s}' | .126 ssh='${b_s}' | nb=$nb"
  if [ -n "$a_s" ]; then echo ">>> ARMBIAN UP (kernel $a_s) — boot SUCCESS"; break; fi
  sleep 4
done
echo "monitor done"
