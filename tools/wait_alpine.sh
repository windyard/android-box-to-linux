#!/bin/bash
# Detect Alpine (BSP 4.9.113) returning on .126 with a working dropbear shell.
deadline=$(( $(date +%s) + 600 ))
echo "watching for Alpine on .126 ... (start $(date +%H:%M:%S))"
while [ $(date +%s) -lt $deadline ]; do
  k=$(ssh -o ConnectTimeout=2 -o BatchMode=yes -o StrictHostKeyChecking=no root@192.0.2.126 'echo $(uname -r) $(hostname)' 2>/dev/null)
  if [ -n "$k" ]; then
    echo "$(date +%H:%M:%S) ALPINE_BACK: kernel/host = '$k'"
    exit 0
  fi
  sleep 4
done
echo "$(date +%H:%M:%S) TIMEOUT: Alpine not back within 10 min"
