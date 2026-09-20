# Persistent root route (applied 2026-09-19)

Recovery gives uid 0 + permissive SELinux (env bootargs carry `selinux=permissive`)
and `/system` is plain ext4 with no dm-verity, so `/system/etc/prop.default`
(= `/default.prop`, read by init on every boot, outranks build.prop) is the
lowest-risk persistence point.

Appended:

    ro.secure=0            # adbd stops requiring RSA auth
    ro.adb.secure=0
    service.adb.root=1     # ro.debuggable=1 -> adbd keeps uid 0, seclabel u:r:su:s0
    service.adb.tcp.port=5555
    persist.sys.usb.config=adb   # init.usb.rc: on boot -> setprop sys.usb.config -> start adbd

Original saved as /system/etc/prop.default.orig and stick copy
`prop_default_orig.txt`; patched copy as `prop_default_patched.txt`.
Revert = restore the .orig file from recovery.

## Verified facts
- recovery shell: `uid=0(root) gid=0(root) context=u:r:recovery:s0`
- /system,/vendor,/product,/cus_config: ext4 rw-capable, no verity; WRITE-OK tested
- /system/xbin/su = setuid root gid shell (AOSP test su); /system/bin/adbd present
- no /system/etc/*.rc touches sys.usb.config, so nothing resets it
- build: p291_iptv-userdebug, test-keys, Android 9 / 20231226, fingerprint R1200-C
- flash backups on stick + in backup/: boot (16MiB), recovery (24MiB), param (1MiB), misc (256KiB), env (32KiB)

## Fallback if adbd does not come up
`tools/mkboot.py` rebuilds boot.img byte-compatibly (round-trip verified); patch
`init.rc` in backup/ramdisk with an `on property:sys.boot_completed=1` trigger and
dd the image to /dev/block/boot from the recovery bind shell.

# Linux on internal eMMC (applied 2026-09-19, USB stick no longer required)

The Alpine rootfs is copied onto the eMMC **cache** partition and the ramfs init
(`tools/tvinit.c` -> `tvrd/init`) mounts root from `/dev/cache` first. The USB
stick image is now only a rescue fallback + `tvinit.log` target. The boot lever
is still the recovery partition (mmcblk0p6); it is NOT the power-on default, so
Android still boots normally unless we ask for recovery.

Daily flow:
- Power-on / plain reboot -> Android (misc is zeroed by tvinit on every recovery
  boot, so the box never gets stuck in Linux).
- Linux -> from Android: `adb shell su 0 reboot recovery` (U-Boot only honours
  Android's misc write; a hand-written boot-recovery block is ignored).
- Root shell once up: `nc 192.0.2.126 2323` (passwordless root, Alpine
  busybox has no telnetd; rc.local runs `nc -lk -p 2323 -e /bin/sh`).

Verified: recovery boot -> `mount` shows `/dev/cache on / type ext4`, up in <10s,
eth0 192.0.2.126/24 + SLAAC v6. Android system/boot/data partitions untouched.

## How it was flashed (repeatable)
1. `dd if=/media/usb/rootfs.img of=/dev/cache bs=1M conv=fsync` (from the running
   Alpine; /dev/cache = mmcblk0p3 is not mounted by Android at runtime).
2. rebuild init: cross-compile `tools/tvinit`, `cp` to `tvrd/init`.
3. `python3 tools/mkboot.py stage/tvboot.img tvrd stage/tvboot.new.img`
   (orig = the clean prior image; mkboot refuses the stock dump's trailing junk).
4. header-preserve: take stock header page from `backup/recovery_backup.img`
   `[:page_size]` (page_size @offset36) over the new body, then patch
   `ramdisk_size` @offset16 to the new gzip length. Result differs from stock
   header only at bytes 16-18 (verified).
5. from Linux/Android shell: `dd if=tvboot.img of=/dev/recovery bs=1M
   conv=fsync; sync`, readback `dd if=/dev/recovery bs=2048 count=5337 | md5sum`.
   Persistence holds because `/system/etc/install-recovery.sh` is an `exit 0`
   stub and `recovery-from-boot.p` is renamed, so Android never reverts p6.

## Amlogic eMMC partition map (mmcblk0 = 7634944 KB / 8 GB "8GTF4R")
Amlogic uses a non-standard table: fdisk/blkid cannot read it. Names come from
kernel DT nodes surfaced as /dev/<name> (minor N == mmcblk0pN).
- p3  /dev/cache   1146880 KB (~1.1 GB)  -> NOW OUR LINUX ROOTFS (was Android cache)
- p6  /dev/recovery 24576 KB  (25165824 B)-> OUR BOOT LEVER (flashed tvboot.img)
- p7  misc         256 KiB                -> reboot-mode token (tvinit zeroes it)
- p11 /dev/boot    16 MiB                  -> stock Android boot (untouched)
- p21 /dev/data    ~3.5 GB                 -> Android userdata (untouched)
Full ordered dump of /proc/partitions + node minors lives in the session log.

## Restore to a stock-only box (undo)
- Recovery: `dd if=backup/recovery_backup.img of=/dev/block/recovery bs=1M; sync`
  (from Android `su 0`; the partition is 24 MiB, image 25 MiB dump - use bs=1M and
  count that fits, or dd the first 24 MiB). Restores stock recovery.
- /dev/cache: it is Android's cache partition; wipe it back (`dd if=/dev/zero
  of=/dev/cache bs=1M count=128`) or let Android reformat on next cache access.
- install-recovery.sh: restore the original (kept at
  /data/local/tmp/install-recovery.sh.orig) and rename recovery-from-boot.p back
  if you want Android's auto-restore re-enabled.
- Nothing in system/boot/data was modified beyond install-recovery.sh + the
  prop.default persistence patch documented at the top of this file.

# Android deleted — Linux-only box (applied 2026-09-19)

The user authorised deleting Android entirely and reclaiming its space. Done.
Stock Android is now GONE from the payload partitions (no full system/vendor/
data backup exists, so it is unrecoverable except via a vendor burn package).
The boot chain (mmcblk0boot0/boot1, U-Boot env, param) is untouched.

## Default boot (no recovery trick)
Read from the live U-Boot env (p4): `bootcmd=run storeboot` and
`storeboot=if imgread kernel boot ${loadaddr}; then bootm ${loadaddr}; fi; run update;`
so a normal power-on simply reads the **boot** partition (p11) and boots it —
no AVB/verification on this path. We therefore wrote the SAME Linux boot image
(kernel + tvinit ramdisk + dtb, an Android v1 boot image) to /dev/boot. On power-
on: storeboot -> imgread boot -> bootm -> tvinit -> switch_root. If p11 ever
fails, storeboot falls through to `update` -> `recovery_from_flash` (p6, same
image), so it cannot lock up. Verified: plain `reboot` (misc empty) comes up in
Alpine with cmdline lacking `recovery_part`, root=/dev/data.

## Root moved to a big partition + Android space reclaimed
- Built Alpine onto /dev/data (p21, was 3.6 GB Android /data): mkfs.ext4, then
  copied the running root's real dirs (`cp -a /bin /etc /sbin /usr /lib /var
  /root /opt ...`; busybox `cp -ax /` does NOT recurse as needed). /dev/cache
  (p3, 1.1 GB) is kept as a tvinit fallback root.
- tvinit `mount_emmc_root()` now tries /dev/data -> /dev/cache (-> USB), rebuilt
  and flashed to BOTH /dev/boot (p11) and /dev/recovery (p6).
- Wiped/reformatted the rest of the Android payload as Linux ext4 and fstab-
  mounted them (see stage/fstab): cache->/home(1.1G), system->/srv(1.2G),
  cus_config->/opt(0.5G); vendor/odm/product mkfs'd (Android erased, spare).
  Usable Linux now ~6 GB across / + /home + /srv + /opt.
- Alpine has no mke2fs by default: `apk add e2fsprogs-extra` (needs clock first,
  see below). stage/fstab + stage/rc.local mirror the on-box config.

## Clock gotcha (why apk failed)
No RTC battery -> clock resets to 2020 each boot -> TLS cert "verify failed" ->
`apk` can't fetch indexes. rc.local now sets a floor `date -u -s @1789776000`
(2026-09-19) early + best-effort router NTP in the background (WAN UDP/123 may be
blocked). Verified apk installs (dropbear, screen) work on a fresh boot.

## Access / state after conversion
- Power-on -> Alpine (Linux-only, STOCK kernel 4.9.113). No Android, no adbd.
- Root shell: dropbear SSH, key-auth only -> `ssh root@192.0.2.126` (tcp/22).
  nc:2323 is retired. Host keys auto-generated in rc.local; VM pubkey in
  /root/.ssh/authorized_keys. Alpine dropbear has no sftp-server, so ship files
  with `ssh root@box 'cat > /path'`.
- Serial console: ttyS0 115200 (getty via inittab).
- Partition lever images: p6 recovery and p11 boot both hold tvboot.img (94e85dcd).
- Restore to bootable Android requires a vendor burn package (we keep only
  boot/recovery/env/param/bootloader dumps in backup/, plus /root/boot_stock_p11.img
  was on the old cache root which is now wiped; backup/boot_backup.img on the VM
  still holds the stock boot image).

## Display / HDMI — why headless, and the mainline route
The box boots fine and serves SSH, but the monitor shows nothing under the
STOCK 4.9.113 kernel. Root-caused completely:
- Amlogic HDMI scans the OSD planes; the boot logo lives in reserved
  `linux,meson-fb` 0x3f800000..0x40000000 (8 MiB, top of RAM), drawn on OSD
  plane 1 (`/sys/module/fb/parameters/osd_logo_index=1`).
- Stock kernel: `# CONFIG_FRAMEBUFFER_CONSOLE is not set`, `CONFIG_DEVMEM=n`
  (open /dev/mem -> ENXIO), `/dev/fb0`=OSD0 (NOT routed to HDMI out),
  `/dev/fb1`=32x32 cursor. So no userspace path (X fbdev on fb0, mmap fills,
  FBIOPAN page-flips, mode reconfig) can put pixels on HDMI. Headless is a
  kernel limitation, not config. (X on fb0 sets osd[0] enable but stays black.)
- Logo can be REMOVED: `echo 255 > .../osd_logo_index` + `echo 1080p60hz >
  /sys/class/display/mode` -> black screen. DANGER: `echo 1 >
  /sys/class/graphics/fb1/osd_clear` and/or mmap past fb1 -> kernel hang (only
  a power cycle recovered; there is no nc fallback anymore). Avoid.

Decision (user): switch to Mainline/Armbian kernel to get real HDMI (meson-drm).

### GXLX2 (S905L3) HDMI on mainline — verified state (ophub issues)
- Vanilla upstream `meson_dw_hdmi` lacks the GXLX2 glue -> 6.1.x fails:
  `meson-dw-hdmi c883a000.hdmi-tx: Unsupported HDMI controller (0d0d:0d:0d)`.
- The 2026-07-18 patchset "drm/meson: add HDMI support for GXLX2"
  (zinan@mieulab.com) adds it; ophub's 6.18.y carries it -> meson-drm now
  brings up HDMI on s905l3.
- Known caveat (ophub #3650, open): intermittent boot hang at
  `meson-drm d0100000.vpu: Queued 2 outputs on vpu`. Root cause: GXLX2 HDMI
  uses DIRECT-register mapping at 0xda800000, but driver's runtime
  `soc_major_id` detection is unreliable and falls back to the legacy
  0xc883a000 indirect window -> HW hang. FIX (ophub commit 93c07a9, in the
  GXLX2 dts e.g. meson-gxl-s905l2-x7-5g.dts):
      &hdmi_tx {
          compatible = "amlogic,meson-g12a-dw-hdmi";   // force direct map
          reg = <0x0 0xda800000 0x0 0x10000>;
      };
  It reportedly REGRESSES between builds (reporter 2026-09-05), so we must
  verify the shipped dtb still has it and re-patch if not.

### Staged plan (safe, eMMC boot path untouched until proven)
1. Board = gxlx2_p291; generic mainline match: meson-gxl-s905l2-x7-5g.dtb
   (fallback meson-gxl-s905l3b-m302a.dtb, meson-gxl-s905x-p212.dtb).
   117 GiB HIKSEMI USB stick is already in the box's USB3 (dwc3) port as sda.
   All its recon/backup artifacts are preserved in backup/stick_artifacts/.
2. Flash ophub Armbian trunk (kernel 6.18.52) .img to the stick (stream from
   VM -> `ssh box 'dd of=/dev/sda bs=4M'`), eMMC boot0/boot path NOT written.
3. Set the USB boot env (aml_autoscript chain) and reboot into USB ONLY.
   Headless Alpine on eMMC is the guaranteed fallback (pull stick -> Alpine).
4. Before booting, decompile the chosen dtb; ensure hdmi_tx has the
   g12a-dw-hdmi compatible + 0xda800000 reg; re-patch with dtc if missing.
5. Confirm on the MONITOR (HDMI picture) and re-verify eth0/SSH from the new
   stack. Only after HDMI+LAN are proven, decide eMMC strategy (mainline
   `armbian-install`/dds u-boot.bin.sd.bin to boot0, OR keep Alpine on eMMC and
   swap just the mainline Image+dtb+modules). Keep backup/ images.

### USB-boot trigger (Amlogic env — NO fw_setenv)
Amlogic env on /dev/env (8 MiB part) is NON-standard: magic
`\xe6\x8aYB` + NUL-separated k=v, NO CRC -> u-boot-tools fw_printenv/
fw_setenv cannot read it. Use tools/mkenv.py instead (proven in task #8).
The stock `bootcmd=run storeboot`; `storeboot` boots eMMC p11 and only on
failure runs `update`. `update` = `run usb_burning; run sdc_burning; ...;
usb start 0; fatload usb 0 ${loadaddr} aml_autoscript; autoscr; ...;
recovery_from_flash`. So to boot USB once:
  dd if=/dev/env of=env_live.bin bs=1 count=32768
  python3 tools/mkenv.py env_live.bin env_new.bin "bootcmd=run update"
  dd if=env_new.bin of=/dev/env bs=1 count=32768 conv=fsync; reboot
Revert: dd the saved original back, or set bootcmd=run storeboot. Not fatal:
U-Boot compiled default is storeboot and update's tail reaches p6 (Alpine).
backup/stick_artifacts + backup/ hold env_raw.bin / env_trace.bin (full env).

### Image choice (verified against the actual .img)
Vanilla Armbian "trunk_resolute" img = single ext4, boot.scr needs MAINLINE
u-boot (booti/fdt/part uuid), ships NO aml_autoscript/u-boot.ext and NO gxlx2
dtb in its pool -> won't boot on stock Amlogic u-boot. Use ophub's PER-BOARD
"amlogic_s905l3*" images instead: built from ophub linux-6.18.y (has GXLX2
dw-hdmi glue + commit 93c07a9 dtb fix) and carry the aml_autoscript+u-boot.ext
wrapper + a large dtb pool. Chosen: amlogic_s905l3b-m302a 6.18.52 (today's
build, the #3650-validated config); override FDT to x7-5g/p212 from the pool
as needed.

## Session update 2026-09-19/20 — mainline abandoned, env restored, WiFi recon

### USB-boot trigger CONFIRMED working (end-to-end)
`tools/reboot_mode.c` compiled natively on box with gcc:
`reboot(MAGIC1, MAGIC2, LINUX_REBOOT_CMD_RESTART2 /*0xA1B2C3D4*/, "update")`
latches Amlogic update-mode -> u-boot runs `update` -> `aml_autoscript` ->
saveenv -> chainload u-boot.ext -> mainline. This proves the whole USB-boot
lever that env-header decoding never could. Binary staged at box /root/reboot_mode.

### Mainline kernel result: HANGS (route abandoned by user)
ophub 6.18.52 hangs on S905L3/p291 BEFORE HDMI-out or eth0 (black + offline).
Tried: dtb swap x7-5g<->m302a-v2, removing forced video= mode; anti-hang dw-hdmi
compatible fix present in shipped dtb but insufficient. No serial console to
debug. User: "i give up" -> do NOT retry USB-boot without explicit request.

### Boot order RESTORED to pristine (done)
`dd if=/root/env_restore.bin of=/dev/env bs=32768 conv=fsync`.
Verified header back to pristine 9e99dec5 (sysfs byte-swapped display c5de999e)
and bootcmd=run storeboot. Box now boots straight to Alpine eMMC. No reboot needed.

### WiFi = Broadcom bcmdhd on SDIO; NOT usable under stock Alpine (verdict)
Hardware: DT `wifi` node compatible "amlogic,aml_wifi", power_on_pin=GPIO486,
interrupt_pin=498, on SDIO host d0070000.sdio (driver meson-aml-mmc; SEPARATE from
eMMC host d0074000). bcmdhd is BUILT INTO the 4.9.113 BSP kernel (dmesg
`bcmdhd_init_wlan_mem`), so no .ko needed. Blockers, all real:
  1) No SDIO card enumerates at boot -> chip's WL_REG_ON never asserted by default
     (Android HAL powers it via the `wifi_power` chardev major 244 / dhd_priv ioctl;
     Alpine ships none of that userspace).
  2) GPIO486 is owned by aml_wifi pinctrl -> sysfs export refused at runtime.
  3) Runtime unbind/bind of the SDIO host to force a rescan OOPSes meson_mmc_remove
     (BSP driver has no clean hot-remove). Only resets on reboot.
  4) /lib/firmware is EMPTY (Android vendor payload wiped in task #13) -> no
     fw_*.bin / nvram for bcmdhd even if the chip enumerated.
Recovery of (4) is possible: full Android images on hand (fw/S905L_6.0.1_TWRP.img,
fw/20191219-R3300L-*.img) contain the vendor firmware; exact R1200-C zip in
ssrf/pkg is metadata-only. The exact R1200-C BCM chip/AP model still TBD.
Bottom line: WiFi would need firmware extraction from the stock image PLUS a
boot-time (not sysfs) WL_REG_ON power path — high effort, low odds, same class of
BSP bring-up pain as the mainline HDMI hang. Recommend USB Ethernet (eth0 works)
or a USB WiFi dongle instead unless the user wants to invest.

NOTE: WiFi SDIO host currently left unbound from the oops experiment (harmless;
restores on next reboot). eMMC root, eth0, SD host all healthy.
