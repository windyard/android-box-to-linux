#!/bin/bash
# watch both potential boot targets; report which OS answers
LOG=/tmp/watch_boot.log
: > $LOG
for i in $(seq 1 150); do
  ts=$(date +%H:%M:%S)
  arm=$(ssh -o ConnectTimeout=2 -o StrictHostKeyChecking=no root@192.0.2.199 'echo K=$(uname -r)' 2>/dev/null | grep -o '6\.18[^ ]*')
  alp=$(ssh -o ConnectTimeout=2 -o StrictHostKeyChecking=no root@192.0.2.126 'echo K=$(uname -r)' 2>/dev/null | grep -o '[0-9]\.[0-9]\.[0-9]*')
  line="$ts arm=${arm:-.} alpine=${alp:-.}"
  echo "$line" >> $LOG
  if [ -n "$arm" ]; then echo "$ts ARMBIAN_UP $arm" >> $LOG; break; fi
  sleep 4
done
echo "watch done" >> $LOG
