#!/bin/bash
# long-running: detect box return as Alpine(.126) or Armbian(.199)
for i in $(seq 1 200); do
  ts=$(date +%H:%M:%S)
  a=$(ssh -o ConnectTimeout=2 -o StrictHostKeyChecking=no root@192.0.2.199 'uname -r' 2>/dev/null)
  b=$(ssh -o ConnectTimeout=2 -o StrictHostKeyChecking=no root@192.0.2.126 'uname -r' 2>/dev/null)
  if [ -n "$a" ]; then echo "$ts >>> ARMBIAN UP on .199 kernel=$a"; fi
  if [ -n "$b" ]; then echo "$ts >>> BOX UP on .126 kernel=$b"; fi
  sleep 5
done
echo "return-watcher done"
