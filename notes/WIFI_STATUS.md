# R1200-C WiFi — definitive status (2026-09-20, autonomous overnight pass)

## Bottom line
Built-in WiFi CANNOT be brought up on the current Alpine setup, and it is not a
firmware/power problem — the deployed kernel has NO WiFi NIC driver at all.

## Evidence
* Running kernel: 4.9.113 #1 SMP PREEMPT, armv7l (32-bit), Amlogic "aml-s905l3-androidp"
  BSP build (oops path string: /home/sdb1/fanyp/AML/aml-s905l3-androidp/common).
* /lib/modules/4.9.113 = EMPTY. 0 .ko files anywhere on the box. No build tree, no
  linux-headers. -> No module can be loaded, and none can be built against CRCs.
* /proc/kallsyms:
    - cfg80211/mac80211 core present (~2019 syms) = wireless stack only.
    - WiFi NIC drivers built in: NONE. Checked brcmfmac, b43, mwifiex, rsi, wilc,
      wl12xx, rtl8xxxu/rt2x00usb/mt7601u/ath9k_htc -> 0 real probe functions.
      (The "b43/8189/brcm" earlier hits were hex-address substrings; `grep -c " b43"` = 0.)
    - Amlogic WiFi glue IS present: set_wifi_power / set_usb_wifi_power /
      wifi_power_ioctl (/dev/wifi_power, major 244) and bcmdhd_init_wlan_mem /
      bcmdhd_mem_prealloc. This is only the POWER + static-DMA-buffer helper that a
      bcmdhd/dhd net-driver would call — the net-driver itself is absent.
* WiFi hardware: DT node `wifi` compatible "amlogic,aml_wifi", on SDIO host
  d0070000.sdio (driver meson-aml-mmc, SEPARATE from eMMC host d0074000).
  power_on_pin=GPIO486, interrupt_pin=498, non-removable. Chip family = Broadcom
  (bcmdhd/AP-series) most likely; exact chip unknown (never enumerated).

## Why the stock images don't save us
* fw/r3300l & fw/s905l system partitions DO carry /lib/dhd.ko (+ 8723bs/8189es/...),
  and /etc/wifi/<chip>/ firmware (fw_bcm43455c0_ag.bin etc., nvram_*.txt).
  Extracted to fw/r3300l/wifi_fw/ for reference.
* BUT those dhd.ko are ELF aarch64 (class 2 / e_machine 0xb7), vermagic 3.14.29.
  The box kernel is armv7l 4.9.113. 32-bit kernel CANNOT load a 64-bit module
  (ELF arch is hard-rejected; `--force` cannot cross the architecture). Dead.
* Box's OWN matching armv7 4.9.113 dhd.ko lived on the Android /system partition,
  which was wiped in task #13. boot_backup.img + recovery_backup.img ramdisks contain
  NO modules. No surviving Android fs on eMMC (p3/p18 read-only mounts all failed).

## What WOULD work (all are bigger, awake-user decisions)
1. MAINLINE / Armbian kernel — HAS brcmfmac; would drive the Broadcom SDIO chip AND
   fix HDMI. This is the same route the user ABANDONED because 6.18 hangs on
   gxlx2_p291 before display/net. Fixing that hang needs a SERIAL CONSOLE (USB-TTL),
   which the user said they do not have. WiFi comes along with it once it boots.
2. A custom 4.9.113 armv7l build WITH bcmdhd/brcmfmac compiled in — needs the exact
   Amlogic SDK source + config + Module.symvers to keep ABI/CRC compatible. Not
   available; building+--force-loading unattended is exactly the hard-hang risk the
   "auto recover" requirement forbids.
3. USB WiFi dongle — will NOT work on this kernel either (no in-tree USB WiFi driver
   compiled in, no loadable modules). Would also require a kernel with USB WiFi support.

## Auto-recovery / state left behind
* bootcmd restored to pristine `run storeboot` (verified header 9e99dec5). Any reboot
  -> Alpine eMMC. The box ALWAYS self-recovers to Alpine on power-cycle. Untouched:
  /dev/env, boot/recovery/uboot partitions, router (192.0.2.1).
* Overnight experiments left ONLY in-RAM, non-persistent effects: I unbound the WiFi
  SDIO host once (meson_mmc_remove OOPSed; box and SSH survived, only the dead WiFi
  host is unbound), exported GPIO486 high, and left aml_wifi unbound. ALL of these
  reset on the next reboot. Nothing was written to disk beyond notes + local extraction.
* No firmware was pushed to /lib/firmware (inert without any driver).
* Box verified healthy: root on /dev/data (p21 ext4), eth0 192.0.2.126 up, low load.

## Correction 2026-09-20 (session 3): blob roulette regressed; p212+headless is the PROVEN combo
- Switched u-boot.ext to the model_database "canonical" s905l3b blob (u-boot-s905x-s912.bin).
  RESULT: WORSE — box stayed fully offline (MAC 00:22:6d absent), not even eth0. The DB row pairs
  that blob with a different DTB; it must NOT override direct evidence on THIS board.
- Re-read of authoritative note above: the ONLY boot that ever reached networking was
  u-boot.ext=p212 (606670B) + FDT=p291-headless (m302a-v2 derived) -> eth0 UP + DHCP .126,
  but TCP/22 refused. Root cause of the ssh failure was DIAGNOSED as first-boot services
  (amlbootcap/resize-fs) stalling the box before multi-user, so ssh.socket never came up.
  (Also: possible slow OpenSSH host-key gen on low-entropy headless boot.)
- Therefore the never-yet-tried-correctly config is:
      u-boot.ext = u-boot-p212.bin   (REVERT from s905x-s912)
      FDT        = p291-headless     (keep; proven)
      + armbian-firstrun.service MASKED to /dev/null   (already on stick)
      + armbian-resize-filesystem.service MASKED        (already on stick)
      + ssh.socket enabled                               (already in rootfs)
      + (do) PRE-SEED /etc/ssh host keys on rootfs so sshd binds without entropy-wait.
- Recovery invariant: bootcmd=run start_autoscript;run storeboot means a power-cycle WITH THE STICK
  IN re-enters start_autoscript -> re-boots the (hung) u-boot.ext. To recover to Alpine you MUST
  power-cycle with the STICK REMOVED (-> storeboot -> Alpine). Verified unchanged on eMMC/env.

## Session 2026-09-20 — headless mainline boot attempt (PROGRESS: hang fixed at kernel level)
- Built p291_headless.dtb from ophub m302a-v2 with TWO fixes:
  (1) memory 2GB->1GB (0x40000000; board has 1GB — m302a/v2 both wrongly declared 2GB),
  (2) vpu@d0100000 + hdmi-tx@c883a000 set status="disabled" (the prior 6.18 hang was at
      `meson-drm d0100000.vpu` probe, BEFORE eth0; disabling display removes that hang).
  Framebuffer nodes were already disabled. Compiled clean on box (dtc). On stick as
      BOOT/dtb/amlogic/meson-gxl-s905l3b-p291-headless.dtb ; uEnv.txt FDT -> that file
      (old uEnv saved: uEnv.txt.bak_m302a).
- Trigger: `reboot_mode update` RESTART2 latch printed but stalled AFTER reboot notifiers
  (hdmitx avmute + wdt disable in dmesg) WITHOUT completing reset; reboot_mode proc stuck D.
  A subsequent NORMAL `reboot` DID reboot the box.
- Result: box came back with eth0 UP + DHCP at .126 (same hw MAC 02:aa:bb:cc:dd:01), but
  TCP/22 REFUSED and NO open ports (11+ min). => mainline kernel got PAST the old hang to
  networking (headless DTB WORKED at kernel level!) but Armbian userspace never brought up
  sshd (likely stuck in first-boot services: amlbootcap/resize-fs, or ssh.socket not reached).
  NOTE: this is NOT normal Alpine (dropbear keys persist on /data -> it always answers 22 in ~60s).
- Stick rootfs facts: uses NetworkManager + OpenSSH sshd (ssh.service NOT enabled but
  ssh.socket IS in sockets.target.wants -> socket-activated). root login PermitRootLogin yes,
  PubkeyAuth yes. hostname=armbian. aml-static.nmconnection (600 root) exists (static .199, but
  DHCP won this time). ROOTFS is ext4 mounted rw but root-owned; VM `dev` CANNOT write it and
  sudo needs a password -> fixing ssh/WiFi on the stick needs elevated VM access.
- STATE: box left unreachable-by-me (up, pingable, no shell). Needs a physical power-cycle to
  recover to Alpine. bootcmd may now be `run start_autoscript; run storeboot` (if aml_autoscript
  ran) -> power-cycle WITH STICK REMOVED guarantees storeboot->Alpine. Nothing destructive was
  written to eMMC /dev/env or router; auto-recovery invariant intact.

## Session 2026-09-20 (session 4): verdict on "flash Android instead" + the REAL WiFi blocker

### Bootlogger result = mainline route is dead, not "service-stalled"
No `bootlog.txt` / `bootlog.count` / `bootlog-dmesg.txt` appeared on the stick after the
p212 + headless + bootlogger boot. The logger runs at multi-user, so absence proves mainline
NEVER REACHED USERSPACE. The hang is in u-boot.ext DRAM/board init or very early kernel.
=> Deprioritise mainline (task #34). Without a serial console this is guesswork per power-cycle.

### Firmware search (task #35) ANSWERED: compatible images exist
github.com/ophub/kernel/releases/tag/tools carries many S905L3B Android-9 amlogic images, e.g.
android_tv_m302a_s905l3b (220MB), e900v22e_e900v22d_s905l3b, cm201-1-ys, cm211-1-ys,
cm311-1-ch, ty1608, mgv2000-jl/kl, b863av3.1-m2(s905l3ab). Unbrn path documented for S905L3B:
short the black resistor marked `4R12` -> Amlogic USB Burning Tool (v3.2.0 + driver also on that
release page). NOTE: flashing these = destroying Alpine unless eMMC is imaged first, and none
of them is an R1200-C image (no BestV/p291 ROM found in the clear).

### The actual WiFi blocker, precisely stated (from /proc/config.gz of the RUNNING kernel)
- CONFIG_AMLOGIC_WIFI=y  -> the aml_wifi platform GLUE is BUILT IN. dmesg confirms it probes:
  power_on_pin=486, interrupt_pin=498, irq 67, `bcmdhd_init_wlan_mem prealloc ok`.
- /sys/class/mmc_host has `sdio` (d0070000.sdio) and it is EMPTY -> no SDIO card enumerated,
  because nothing has driven WL_REG_ON yet.
- grep dhd_ /proc/kallsyms = 0 hits; only `bcmdhd_init` (glue). => the Broadcom fullmac driver
  (dhd.ko) is NOT in the kernel. That one missing file is the entire WiFi problem.
- vermagic to match: `4.9.113 SMP preempt mod_unload ARMv7 p2v8` (CONFIG_LOCALVERSION="",
  SMP=y, PREEMPT=y, ARM=y, AEABI=y, OABI off, THUMB2 off).
- CONFIG_MODVERSIONS=y  AND  CONFIG_MODULE_FORCE_LOAD is NOT set
  => insmod rejects any symbol-CRC mismatch with -ENOEXEC. Clean refusal, no oops. So harvesting
  dhd.ko from another S905L3B Android-9 ROM is a SAFE experiment (worst case: "disagrees about
  version of symbol"), and if it loads, worst case is an oops that a power-cycle heals.

## Session 2026-09-20 (session 5): CHIP IDENTIFIED = UniWave/RuiWave UWE5621DS, NOT Broadcom
### Hard evidence (from the props I harvested off the live Android system, backup/harvest/props.txt)
    [vendor.bcm_wifi]: [uwe]                      <- Amlogic SDK "which bcmdhd-compatible chip" flag
    [persist.vendor.bt_vendor]: [libbt-vendor_uwe_share.so]
    [ro.product.chiptype]: [AMLS905L3]
    [rw.vendor.bluetooth.fw.ver]: [8532.2015.02.04]
=> the SDIO combo is UniWave (RuiWave) UWE5621DS, a REBRANDED REALTEK RTL8821CS-class chip.
   Stock driver = uwe5621_bsp_sdio.ko + uwe5621_wifi_sdio.ko (+ uwe5621_bt_sdio.ko), NOT dhd.ko.
   (Confirmed those exact filenames exist in ophub Amlogic images: see E900V22E carve below.)

### Consequences (this invalidates several earlier plans)
- dhd.ko hunting = wrong target. bcmdhd/brcmfmac will never bind this chip.
- MAINLINE WOULD NOT HAVE FIXED WIFI EITHER: mainline has no uwe5621 driver at all (out-of-tree
  Realtek derivative). => The ONLY viable WiFi path is the box's own BSP kernel 4.9.113 + the
  vendor uwe5621 .ko set. Good news: that needs no flashing and no u-boot.
- The "bcmdhd is built in" note in ROOT_PLAN.md:241 is WRONG (verified: grep -c ' dhd_'
  /proc/kallsyms = 0; only `bcmdhd_init` from the aml_wifi glue). cfg80211/mac80211 ARE built in.

### RECOVERED /dev/wifi_power ioctl ABI (brute-forced from userspace with a gcc-compiled prober;
### /dev/mem, /dev/kmem and /proc/kcore are all disabled on this kernel, so static RE was impossible)
    0x00006d01 arg=1  -> "Set usb_sdio wifi power up!"   (GPIO486 hi, sdio_reinit)
    0x00006d02 arg=0  -> "Set usb_sdio wifi power down!" (GPIO486 lo)
    0x00006d03 arg=1  -> "Set sdio wifi power up!"       (also [pci_reinit])
    0x00006d04 arg=0  -> "Set sdio wifi power down!"
    0x00006d05        -> reports "wifi interface dev type: sdio, length = 4"
    (magic 'm'=0x6d, _IO no-size, nr 1..5)   Source: ~/tvbox/wificmd.c, wififind.c
    Powering works: /sys/kernel/debug/gpio shows gpio-486 (sdio_wifi) flipping out lo->hi.
- After power-up the SDIO host does re-tune + `sdio_reset_comm()`. Once it reported
  "sdio: new ultra high speed SDR104 SDIO card at address 8800", clock 200MHz, 4-bit.
  But /sys/bus/sdio/devices/sdio:8800/1 reports vendor=0x0000 device=0x0000 class=0x00
  (modalias sdio:c00v0000d0000) -> the card node is STALE and the real CIS was never read.
  => do not trust an ID read from sysfs; the .ko must drive its own enumeration.

## Session 2026-09-20 (session 6): DRIVER LOADS + CHIP RUNS. Blocker narrowed to one file
### Vermagic oracle (no /dev/mem needed)
Feed the kernel a module whose `vermagic=` in `.modinfo` is binary-patched to nonsense; the loader
refuses and prints the EXPECTED string verbatim. Our kernel expects exactly:
    `4.9.y SMP preempt mod_unload modversions ARMv7 `
(NOT `4.9.113`, and it DOES include `modversions`; no `p2v8` -> the session-4 note above is WRONG.)
### CRC compatibility PROVEN, patching is unnecessary
`tb_detect.ko` (Amlogic 4.9.y armv7 out-of-tree module harvested from a CM311 pack) loaded AND
unloaded cleanly => CONFIG_MODVERSIONS symbol CRCs of neighbouring S905L3B ROMs match our kernel.
So prebuilt `.ko` from other packs can be used verbatim; no kernel rebuild, no vermagic patching.
Tooling: ~/tvbox/karve2.py <img> <outdir> carves complete ET_REL/EM_ARM modules out of
raw ROM blobs (fixes learned: e_type at 16, e_machine at 18, ELF magic is 5 bytes `\x7fELF\x01`,
complete extent must include the section-header table, module name is at +12 of
`.gnu.linkonce.this_module`).
### CHIP CORRECTED: Unisoc/Spreadtrum Marlin, not Realtek
Live chip answered over SDIO:
    chipid: 0x56630001
    Platform Version: MARLIN3E_20A_W21.19.2 / Project Version: uwe5623_marlin3E_ott
=> **UWE5623 / MARLIN3E** (UWE5621 is the family name). Two driver generations exist:
  legacy `uwe5621_bsp_sdio.ko`+`uwe5621_wifi_sdio.ko` (use the kernel's aml_wifi glue, which we HAVE)
  vs new `sprdbt_tty.ko`+`sprdwl_ng.ko` (need built-in wcn/marlin glue, which we LACK).
  The CM201-1-YS donor pack uniquely ships sprdwl_ng *together with* legacy uwe5621_bsp_sdio,
  which supplies the missing symbols. Donor: fw/cm201-1-ys_s905l3b.tar.xz ->
  S905L3-L3B...2024_cm201-1-ys_s905l3b可用.img (path in /tmp/imgpath.txt).
### WHAT WORKS NOW (first functional WiFi bring-up on this box under Alpine)
  /root/uwe5621_bsp_sdio.ko (4,507,984 B) + /root/sprdwl_ng.ko (7,766,664 B) both insmod OK;
  sdio:8800:1 enumerated by the driver's own sdiohal (vendor/device still read 0x0000 — expected,
  sdiohal does private enumeration, no clean CIS needed); WCN firmware `wcnmodem.bin.hex` is
  EMBEDDED in the .ko, parsed as a WCNE image and downloaded; CP booted and answered AT
  (WCN_VER line above); `get_board_ant_num [two_ant]`. No /lib/firmware needed for the CP image.
### THE REMAINING BLOCKER (one missing file) — NOW FIXED ON DISK, untested
  wifi ini path = /vendor/etc/wifi/uwe5621/wifi_56630001_2ant.ini -> open error ->
  LOAD_INI_DATA_FAILED (WIFI_CMD_DOWNLOAD_INI assert) -> "WIFI MAC can't be found wifimac.txt"
  -> sprdwl_init_fw failed -> failed to register netdev(-5) -> no wlan0.
  Donor ROM is NOT ext4/f2fs/erofs/squashfs: it is an Amlogic USP image whose partitions use
  12-byte extent headers {magic=0x0000cac1, nblocks, 0xc+nblocks*4096} followed by
  nblocks*4096 of raw payload; ext2-style dir entries (ino u32, rec_len u16, name_len u16, type u8)
  live in DIR extents. Walk = read header, payload, advance 12+nblocks*4096. Directory extent at
  0x3ec6f6cc lists inodes 0x229..0x232 = the 10 wifi_*.ini; the 10 following FILE extents are those
  files in inode order (2ant/3ant pairs are byte-identical per chipid). Extracted all 10 ->
  ~/tvbox/inis/, installed on the box at /vendor/etc/wifi/uwe5621/ (on Alpine rootfs,
  persists), plus /data/misc/wifi/ created for the MAC file (`/data/misc/wifi/wifimac.txt`).
### HARD CONSTRAINT discovered: uwe5621_bsp_sdio is SINGLE-LOAD PER BOOT
  Its init registers the `sdiohal` sdio_bus driver and its exit never unregisters it. After any
  rmmod, the next insmod fails: "Error: Driver 'sdiohal' is already registered, aborting" ->
  sdio_register_driver -16 -> "wait SDIO rescan card time out" -> "chip power on fail".
  => the whole bring-up gets exactly ONE attempt per boot. Never rmmod bsp; only sprdwl_ng may be
  reloaded, and even that left the chip unpowerable ("SDIO card dump") in this session.
  Bring-up script for a fresh boot: ~/tvbox/up2.sh (logs to /root/up.log on the box).
### INCIDENT: sshd died right after the failed 2nd insmod (ping OK, port 22 refused).
  Suspect the vendor driver's `WARNING: __vunmap Trying to vfree() bad address` / memory abuse,
  or OOM (1 GB). Recovery = physical power cycle (box self-recovers to Alpine; /dev/env untouched).

## NEXT STEP
1. Physical power-cycle (user), then run ~/tvbox/up2.sh ONCE with the ini already in place
   and read /root/up.log — expect DOWNLOAD_INI to succeed, MAC written to /data/misc/wifi/wifimac.txt,
   and `wlan0` registered. Then `iwlist`/`ip link set wlan0 up` + scan.
2. If MAC still not found: seed /data/misc/wifi/wifimac.txt manually (format: the sprdwl CP reads it
   via `sprdwl_get_customize_mac`; alternatively our kernel exports `wifi_get_mac` from the aml_wifi
   glue, which the legacy path uses).
3. Once wlan0 exists: persist the bring-up (a /etc/local.d script that runs up2.sh at boot, since it
   is only valid once per boot) and only then consider Bluetooth (sprdbt_tty.ko, 993,760 B, carved).

## Session 2026-09-20 (session 6b): **BUILT-IN WiFi WORKS** — wlan0 up, scan TX+RX confirmed
After a power cycle (sdiohal is single-load-per-boot) the one-shot sequence
`wificmd 0x6d03 -> insmod uwe5621_bsp_sdio -> insmod sprdwl_ng` with the harvested ini in place:
    WCN: cali_ini_need_download return 1
    wifi ini path = /vendor/etc/wifi/uwe5621/wifi_56630001_2ant.ini      (opens, parses)
    WCN: marlin_write_cali_data finish / marlin download finished and run ok
    sprdwl:fw_ver:0, fw_std:0x13, fw_capa:0x120f4f
    sprdwl:mac_addr:02:aa:bb:cc:dd:01
    unisoc_wifi unisoc_wifi wlan0: mixed HW and IP checksum settings.   <- netdev REGISTERED
Only benign warnings remain (`TLV check failed: type=0, len=0`, the checksum notice, and the
`sprdwl: ctx_id=0 sm_state=4 bssid=<ipv6 mcast>` rx spam).
* `ip link set wlan0 up` -> RUNNING; `/proc/net/wireless` lists wlan0.
* `iw dev wlan0 scan` returns 15 BSSes on 2.4 GHz AND 5 GHz with plausible RSSI (-37..-81 dBm).
  Probe requests are being answered => RF TX and RX both work. Antennas TX 0x2 RX 0x2 (two_ant).
* phy0 supported modes: IBSS, managed, AP, P2P-client/GO/device -> station and hotspot both viable.
* Router identified: gateway 192.0.2.1 == 02:aa:bb:cc:dd:a3 == SSID "MyWiFi24" (ch 2442);
  its 5 GHz radio 02:aa:bb:cc:dd:a4 == "MyWiFi50". The WPA passphrase was never recoverable
  from the box (Android /data wiped in task #13) -> must come from the user.
* Userspace installed: `apk add iw wpa_supplicant` (armhf, Alpine v3.20). Note init is busybox init
  with /etc/rc.local (no openrc runlevels), sshd is **dropbear** started from rc.local.
* PERSISTENCE INSTALLED: /usr/local/bin/wifi-up.sh (power-up ioctl + 2 insmods + wait for wlan0 +
  `ip link set wlan0 up`, then optional wpa_supplicant/udhcpc if
  /etc/wpa_supplicant/wpa_supplicant.conf exists) hooked into /etc/rc.local as a background job
  AFTER dropbear, logging to /root/wifi-boot.log. /etc/rc.local.bak_wifi is the pre-hook copy.

## Session 6c (2026-09-19/20) — WiFi ASSOCIATED + DHCP + INTERNET. Task complete at RF/L3 level
- Chip confirmed again from the running driver: UniWave/RuiWave **UWE5623 (MARLIN3E)**, SDIO,
  wlan0 MAC `02:aa:bb:cc:dd:01`. Bring-up = `/root/wificmd 0x00006d03 1` -> `insmod uwe5621_bsp_sdio.ko`
  -> `insmod sprdwl_ng.ko` (all three live in /root on the Alpine rootfs = p21 `/dev/data`, persistent).
- RF-calibration ini files recovered from the donor USP image are installed at
  `/vendor/etc/wifi/uwe5621/` (wifi_5663000{0,1}_{2,3}ant.ini) — this was the last missing piece.
- **Association works**: `iw dev wlan0 link` -> `Connected to 02:aa:bb:cc:dd:a3`, SSID MyWiFi24,
  2442 MHz, signal -34 dBm, tx bitrate 173 MBit/s VHT-MCS 9 VHT-NSS 2 (so 802.11ac / 2-stream TX
  is genuinely up, not just management frames).
- **DHCP**: `udhcpc -i wlan0 -b -n -t 20` -> lease 192.0.2.125 from 192.0.2.1, 86400 s.
  eth0 keeps .126 and the primary default route (wlan0 default has metric 308), so the wired SSH
  lifeline is untouched.
- **Internet over WiFi verified**: `ping -I wlan0 223.5.5.5` 0% loss; DNS through the router works.
- CREDENTIAL HANDLING (user policy: derive the hash, keep the plaintext off disk):
  `/etc/wpa_supplicant/wpa_supplicant.conf` (0600, rootfs) contains ONLY the 64-hex PMK for
  `network={ ssid="MyWiFi24" ... }`. The passphrase was piped over stdin into `wpa_passphrase`,
  the `#psk=` comment line was filtered out, the value was `unset` and `grep` for the plaintext
  returns 0 in every file. No plaintext (and no PMK) is stored in these notes or in the repo.
  MyWiFi50 (5 GHz, BSSID 02:aa:bb:cc:dd:a4) is deliberately NOT in the config — its passphrase
  was never supplied, so nothing was inferred for it.
- wpa_supplicant v2.10 on this box has **no `-f` option** (that is what silently killed the first
  attempt — busybox dumped the usage text and exited 0). Valid form, now used everywhere:
      wpa_supplicant -B -Dnl80211 -iwlan0 -c/etc/wpa_supplicant/wpa_supplicant.conf -P/run/wpa_supplicant.pid
  and logging goes to `/var/log/wifi-up.log` (not /root/wifi-boot.log as noted above).
- /usr/local/bin/wifi-up.sh rewritten with those flags + a pidfile guard (never start a second
  wpa) + "skip insmod if wlan0 already exists" (respects the single-load-per-boot limit) and it was
  executed end-to-end successfully this session (rc=0, INTERNET via wlan0: OK).
- Still only proven componentwise for the BOOT path: the module-load branch cannot be re-tested
  without a reboot (single-load constraint), so final confirmation lands on the next physical
  power-cycle. Auto-recovery invariant unchanged (bootcmd -> storeboot -> Alpine eMMC).
- Real traffic over the radio: a 1.4 MB HTTP download completes in ~1 s with the default route
  pinned to wlan0 (`default via 192.0.2.1 dev wlan0 metric 50`), i.e. >11 Mbit/s goodput.
  Steady-state routing left exactly as udhcpc/rc.local make it:
      default via 192.0.2.1 dev eth0 src 192.0.2.126   (wired stays preferred = SSH lifeline)
      default via 192.0.2.1 dev wlan0 metric 308
