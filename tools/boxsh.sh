#!/bin/bash
# One-shot command batch against the box's recovery bind shell.
# usage: boxsh.sh cmdsfile [waitsec]
set -u
f="${1:?cmds file}"
w="${2:-25}"
{ cat "$f"; sleep "$w"; } | nc 192.0.2.126 5555
