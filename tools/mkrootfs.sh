#!/bin/bash
# Build the Alpine armhf rootfs image that the R1200C boots from its USB stick.
#   mkrootfs.sh <root-password>
# Non-privileged: the tree carries no device nodes (devtmpfs fills /dev at boot)
# and PID 1 is busybox init driven by inittab + rc.local (3.20 minirootfs has no
# openrc).
set -eu

VER=3.20
BASE=https://dl-cdn.alpinelinux.org/alpine/v$VER
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TARBALL=$ROOT/fw/alpine-minirootfs-$VER.0-armhf.tar.gz
TREE=$ROOT/work/rootfs
IMG=$ROOT/work/rootfs.img
SIZE_MB=${SIZE_MB:-2048}
BOXIP=192.0.2.126
GW=192.0.2.1

pass=${1:?usage: mkrootfs.sh <root-password>}
[ -f "$TARBALL" ] || curl -fsSLo "$TARBALL" "$BASE/releases/armhf/alpine-minirootfs-$VER.0-armhf.tar.gz"

rm -rf "$TREE" "$IMG"
mkdir -p "$TREE"
tar -C "$TREE" -xzf "$TARBALL" --no-same-owner --exclude=./dev
mkdir -p "$TREE"/dev/pts "$TREE"/proc "$TREE"/sys "$TREE"/tmp/run "$TREE"/var/log \
       "$TREE"/root "$TREE"/mnt/usb "$TREE"/mnt/root
chmod 1777 "$TREE/tmp"

cat > "$TREE/etc/network/interfaces" <<EOF
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
    address $BOXIP
    netmask 255.255.255.0
    gateway $GW
EOF
printf 'nameserver %s\nnameserver 223.5.5.5\n' "$GW" > "$TREE/etc/resolv.conf"
printf 'r1200c\n' > "$TREE/etc/hostname"

cat > "$TREE/etc/fstab" <<'EOF'
devtmpfs /dev  devtmpfs defaults 0 0
proc     /proc proc   defaults 0 0
sysfs    /sys  sysfs  defaults 0 0
tmpfs    /run  tmpfs  nosuid,nodev 0 0
EOF

cat > "$TREE/etc/inittab" <<'EOF'
::sysinit:/bin/mount -a
::sysinit:/bin/mkdir -p /dev/pts /tmp/run
::sysinit:/bin/hostname -F /etc/hostname
::sysinit:/sbin/swapon -a
null::sysinit:/bin/ln -sf /proc/self/mounts /etc/mtab
::sysinit:/etc/rc.local
ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100
tty1::respawn:/sbin/getty 38400 tty1
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
EOF

cat > "$TREE/etc/rc.local" <<'EOF'
#!/bin/sh
# R1200C bring-up: wired net + reachable root shells
ip link set lo up 2>/dev/null
ifup lo 2>/dev/null
ifup eth0 || ip link set eth0 up
# the image is built unprivileged, so everything lands as uid 1000 once
[ -e /root/.ownership-fixed ] || { chown -R 0:0 /; touch /root/.ownership-fixed; }
busybox telnetd -l /bin/sh -p 2323
echo "r1200c linux up: $(ip -4 addr show eth0 2>/dev/null | awk '/inet /{print $2}')" > /dev/kmsg
exit 0
EOF
chmod 755 "$TREE/etc/rc.local"

hash=$(openssl passwd -6 "$pass")
sed -i "s|^root:[^:]*:|root:$hash:|" "$TREE/etc/shadow"
grep -q '^root:\$6\$' "$TREE/etc/shadow" || { echo "shadow rewrite failed"; exit 1; }

dd if=/dev/zero of="$IMG" bs=1M count=0 seek=$SIZE_MB status=none
mke2fs -q -t ext4 -L tvroot -d "$TREE" "$IMG" -E root_owner=0:0 \
    -O ^orphan_file,^fast_commit,^metadata_csum_seed,^64bit
ls -l "$IMG"
echo "tree: $(du -sh "$TREE" | cut -f1), image: ${SIZE_MB} MB sparse, $(du -h "$IMG" | cut -f1) on disk"
