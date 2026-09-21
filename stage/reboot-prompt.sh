#!/bin/sh
# Explicit two-step reboot: desktop icon -> xmessage confirm -> reboot.
# README ground rule: reboots happen only when the human asks for one.
if [ "$(xmessage -center -print -buttons Reboot,Cancel:default "Reboot the box now?" 2>/dev/null)" = Reboot ]; then
    /sbin/reboot
fi
