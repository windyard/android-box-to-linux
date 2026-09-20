#!/bin/bash
# Fire when SSH (22) opens on .126 or .199, identifying dropbear vs OpenSSH.
deadline=$(( $(date +%s) + 420 ))   # 7 minutes
while [ $(date +%s) -lt $deadline ]; do
  for ip in 192.0.2.126 192.0.2.199; do
    banner=$(timeout 2 bash -c "exec 3<>/dev/tcp/$ip/22; head -c 40 <&3" 2>/dev/null | tr -d '\r\n')
    if [ -n "$banner" ]; then
      echo "SSH_UP $ip banner=[$banner] at $(date +%H:%M:%S)"
      exit 0
    fi
  done
  # also note if it becomes Alpine (dropbear) via a working key-auth shell
  k=$(ssh -o ConnectTimeout=2 -o BatchMode=yes -o StrictHostKeyChecking=no root@192.0.2.126 'echo K=$(uname -r)' 2>/dev/null)
  [ -n "$k" ] && { echo "KEYAUTH_OK .126 $k at $(date +%H:%M:%S)"; exit 0; }
  sleep 8
done
echo "TIMEOUT: no SSH on 22 for .126/.199 by $(date +%H:%M:%S)"
