#!/bin/sh
# Explicit two-step power-off: desktop icon -> xmessage confirm -> poweroff.
# README ground rule: power changes happen only when the human asks.
if [ "$(xmessage -center -print -buttons "Shut Down",Cancel:default "Shut the box down now?" 2>/dev/null)" = "Shut Down" ]; then
    /sbin/poweroff
fi
