# BestV R1200-C → Linux media box

Full field notes for taking a **BestV R1200-C** IPTV set-top box (Amlogic
**S905L3B**, board `gxlx2_p291`, Android 9 "userdebug/test-keys") from a locked,
vendor-managed device to a rootable Linux box with working wired + wireless
network, and the still-unfinished fight to get an HDMI picture out of the stock
BSP kernel.

Every problem we hit is listed with the **root cause we actually confirmed** and
the **fix that landed**, including the dead ends — several conclusions written
into earlier notes were later disproved on this box, and those reversals are the
most valuable part of the record.

**The root chain, in one line** (§2–§3): we faked the vendor OTA upgrade server
(`qhup.bestv.com.cn`, DNS-poisoned for this box only), offered a package signed
with the AOSP **testkey** that matches the box's `otacerts.zip`, and the box's
own upgrade client then downloaded it, wrote the `misc` boot-recovery token and
**rebooted into recovery to install it** — that fake package is what made the
recovery screen appear. No button combo, no serial console, no pre-existing adb.

* Detailed appendices: [`notes/ROOT_PLAN.md`](notes/ROOT_PLAN.md),
  [`notes/WIFI_STATUS.md`](notes/WIFI_STATUS.md)
* Tooling: [`tools/`](tools), [`ssrf/`](ssrf) (OTA interception),
  [`stage/`](stage) (on-target config), [`recon/`](recon) (display probes)

---

## 0. Ground rules that shaped every decision

| Rule | Why |
|---|---|
| The box must **self-recover on power-cycle** | It sits behind a TV; a hang costs a physical unplug. |
| **Never** write `/dev/env`, `bootloader`, `boot0/1`, `param`, or the router | U-Boot env and the bootloader are the only truly unrecoverable objects. |
| Reboots only with explicit user go-ahead | Each failed boot costs a power cycle and ~2 minutes of blindness. |
| No plaintext passphrase stored on disk or in git | Passwords are converted to a PMK and piped on stdin only. |
| eMMC `data`/`cache` were fair game, the boot chain was not | Partition content is re-flashable from Linux; a bricked bootloader is not. |

The invariant that held the whole project together: `bootcmd=run storeboot` was
restored to pristine after every experiment, so removing power always returned
the box to a known Linux.

---

# PART I — ENGLISH

## 1. Target reconnaissance

### 1.1 What the box exposes on the LAN

The box runs an **UPnP/DLNA MediaServer** on **tcp/38520** (CyberLink
`CyberHTTP` server stack) with `AVTransport` and `ConnectionManager` services;
their descriptions were dumped to `ssrf/avt.scpd.xml` and `ssrf/cm.scpd.xml`.

**Problem:** the box is behind a router with client isolation on the management
side and has no ADB enabled, no telnet, no SSH.

**Finding that opened the door:** `AVTransport.SetAVTransportURI` is reachable
**without authentication**, so the box can be told to fetch an arbitrary URL.
That is effectively an SSRF / request-forgery oracle: `ssrf/oracle.py` and
`ssrf/redir.py` (a `302`-redirect chain plus a slow-response endpoint) proved the
device really performs the outbound GET and follows redirects, and the captured
`User-Agent`/`Range` headers fingerprinted the client stack.

**Also:** UDP sweep + SSDP/WS-Discovery probes (`ssrf/scan.py`) mapped the other
services.

### 1.2 Firmware and attack surface

`kitchen/` is **AmlogicKitchen** (upstream), used to unpack Amlogic `pkg`/`img`
containers. Compatible R1200-C firmware was never found in the clear (task #35),
but neighbouring S905L3B Android-9 images were recovered later and turned out to
be the source of both the WiFi driver and the WiFi calibration data (§6).

---

## 2. Making the box talk to us instead of the vendor

### 2.1 Intercept

`ssrf/ota_poison.sh` is the whole interception stack, **scoped to the box's IP
only** so the rest of the LAN (and the router) is untouched:

* `minispoof.py` — hand-built ARP replies to poison the box's default gateway
  (and, separately, the router's entry for the box) — bidirectional, with a
  restore phase that re-ARP-corrects both sides on exit;
* the box also had a **public IPv6** address, so an **NDP spoof** (`ndpspoof.py`)
  was required for the same trick on v6 — the vendor endpoint was reached over
  v6, which is why IPv4-only redirection silently captured nothing;
* `iptables`/`ip6tables` `PREROUTING … -s $BOX/32 -p tcp/udp --dport 53,80
  -j REDIRECT` to our ports;
* `dnsmasq-poison.conf` answers `bestv.com.cn` and `qhup.bestv.com.cn` with the
  attacker VM (`192.0.2.220`) for the box only, everything else forwards to
  the real router — so the box keeps working normally while its upgrade check is
  hijacked;
* `MASQUERADE` + `FORWARD` accept keeps the box online during the MITM, which
  matters because several vendor flows abort if general connectivity breaks.

### 2.2 The captured OTA protocol

From the phone-home capture (`ssrf/ota_requests.log`, `ssrf/boxcap.pcap`): the
upgrade UI (`OttService`) posts to `qhup.bestv.com.cn` and expects this JSON
shape — reproduced exactly by `ssrf/ota_server.py`:

```json
{ "Response": { "Header": { "RC": 0, "RM": "发现新版本" },
  "Body": { "UpgradeMode": 1, "LastedVersion": "BesTV_R1200-C_QHHZ_3.0.2.0",
            "FileHash": "<uppercase MD5>", "CompressType": 2, "SoftName": "...",
            "SoftSize": <bytes>, "SoftCode": "OS2714",
            "FileURL": "http://qhup.bestv.com.cn/pkg/<file>.zip", "Desc": "..." }}}
```

**Problem 1 — the client silently rejected our first offers.**
Root cause: field-set sensitivity. The client only renders the "new version"
dialog for the *long form* response (all fields populated, `\n`-formatted like
the vendor's own server) and needs `RC:0` with `RM` in Chinese. A minimal JSON
with the same semantics produced no UI at all. Fix: byte-level mimicry of the
captured vendor response.

**Problem 2 — the box downloads with HTTP `Range`.**
`ota_server.py` therefore implements real partial-content serving (`206`,
`Content-Range`, and a `200` when the whole file is asked for). Ignoring `Range`
made the download stall at a fixed percentage with no error shown.

Serving `{"RC":0,"RM":"没有升级计划"}` ("no upgrade planned") for the other
endpoints (`upgradeinside`) is used to keep the box quiet when we are not
attacking.

---

## 3. Getting code execution: the signed recovery package

The root chain in one line: **become the vendor's upgrade server → the box
downloads our package → the box's own OTA client writes `misc` and reboots into
recovery → recovery verifies the signature (testkey) and execs our code as
root.** The box only applies a package whose signature verifies against the
certificate(s) in the recovery ramdisk's `res/keys` (and
`/system/etc/security/otacerts.zip`).

### 3.1 The trigger — the recovery screen itself was called by our fake package

This is worth stating first, because it is the answer to "how did you
even get into recovery on a sealed unit?". There is **no recovery button combo,
no serial console, and no adb** on a factory box, and a hand-written `misc`
block does not work (§4.2). The **only** lever that opens recovery is the vendor
upgrade client itself — so we became the upgrade server, and the box walked
itself into recovery to install our package. The chain, step by step:

1. **DNS poison** for `qhup.bestv.com.cn` (§2.1) → the upgrade UI's periodic
   `POST /upgrade/OttService/UpgradeOSV2` (and `UpgradeInsideV2`, `GetMessage`)
   now lands on `ssrf/ota_server.py`. The client tells us who it is in the
   `User-Agent`: `OSVersion/BesTV_R1200-C_QHHZ_3.0.1.4`.
2. **Answer with an offer that is genuinely newer.** `LastedVersion` must sort
   *above* the version the client just reported (`3.0.1.4` → we used
   `3.0.2.0`); otherwise the client sees nothing to install and no dialog appears.
   The rest of the long-form body must be self-consistent: `RC:0`, Chinese `RM`,
   `UpgradeMode:1`, `FileHash` = **uppercase MD5 of the exact bytes we serve**,
   `SoftSize` = the exact byte length, `FileURL` pointing at our own `/pkg/…`
   path. Any inconsistency → the "new version" dialog never renders.
3. The user confirms 升级 on the box UI (or the client auto-downloads); it `GET`s `FileURL` and
   **resumes with `Range`**, so `ota_server.py` must answer real `206` /
   `Content-Range` (§2.2, Problem 2). Repeated `GET /pkg/update.zip` lines from
   `192.0.2.125` in `ssrf/ota_requests.log` are the box pulling the payload —
   `.125` is the box's **WiFi** address (§3.5 explains why the address changes).
4. The client verifies the MD5, then does what `RecoverySystem.installPackage()`
   does: write the **BCB (bootloader message) into `misc` = mmcblk0p7** — the
   64-byte command slot at offset 0 gets `"boot-recovery"`, plus the
   `--update_package=<path>` argument for recovery (upstream behaviour quoted in
   `tools/recovery9.cpp`: *"`get_args()` writes BCB with `boot-recovery` and
   `--update_package=…`"*). Then it reboots.
5. U-Boot sees the token, boots **p6 = `/dev/recovery`**, and stock recovery
   draws the "upgrading" screen (Android logo + red triangle). **That screen is
   the entry point we were after** — it is not a page we had to unlock; it is
   recovery doing its job on a package we supplied.
6. Recovery verifies the zip against `res/keys`, accepts it (§3.2/§3.3), mounts
   nothing from Android, and **`exec`s our `update-binary` as `uid=0`**, context
   `u:r:recovery:s0` (§3.4).

Two consequences that are easy to miss and cost us time:

* Because the BCB was written by the *running system's* own update path, it is
  honoured. That is precisely the privilege we did **not** have writing `misc`
  by hand (§4.2) — so the fake-OTA route was not merely convenient, it was the
  only route to recovery without physical access.
* Recovery is *not* the payload's goal, it is the sandbox: nothing we install
  there survives into Android. Persistence had to be written to `/system` from
  inside recovery (§3.6), and our own Linux eventually replaced p6 entirely (§4).

### 3.2 The box is signed with AOSP test keys

**Problem:** we assumed a vendor release key we did not have.
**Finding:** `ro.build.fingerprint` says `…/test-keys`, and `keys/` (the standard
AOSP `releasekey`/`testkey` PKCS#8 + x509 pair) **actually matches the box's
`otacerts.zip`**. That single fact converts "unforgeable signature" into "we can
sign whatever we want". Everything below is only possible because of it.

### 3.3 Android 9 recovery's whole-file zip-comment PKCS#7

Android 9 `RecoverySystem.verifyPackage()` no longer uses only `META-INF/*.SF`
per-entry signatures; for an OTA it verifies a **PKCS#7 blob stored in the zip
end-of-central-directory comment**, over a digest of the archive *excluding* the
comment bytes.

Two hard lessons:

* **`META-INF/com/android/metadata` must exist and be inside the signed region.**
  Recovery refuses the package without it (commit `4f4a482`). The block our
  accepted packages carried (`ssrf/signzip.py`):

  ```
  pre-device=p211
  pre-build-incremental=20231226
  post-build-incremental=20240101
  post-timestamp=1704067200
  ```

  plus `META-INF/com/google/android/updater-script` = `assert true;` — the
  pre/post ordering has to look like a forward upgrade. Note we declared
  `pre-device=p211` although the board is `p291`, and recovery still accepted
  the package: this build does not enforce that field, so do not waste time on
  it (the `assert true;` updater-script is likewise not a gate).
* **The `SignerInfo` must have no `authenticationAttributes`.** The recovery
  verifier is `asn1_context`-based (`tools/asn1_decoder.cpp`, `tools/verifier9.cpp`
  are the upstream sources we read to get this right) and computes the digest over
  the *content* only. A `pkcs7`/`smime`-signed package built by ordinary OpenSSL
  tooling embeds authAttrs, and the box rejects it with a generic
  `Signature verification failed` — no useful error. Fix: `ssrf/signzip.py`
  (JAR v1 manifest section hashing) plus `tools/mk_pk7.py`, which hand-builds the
  DER `SignerInfo` with **no authAttrs** to faithfully match the verifier.

### 3.4 update-binary may be an ELF

**Problem:** we wanted a shell, not an install. Recovery's `update-binary` is
normally a sh script.
**Finding:** Android 9 recovery accepts an **ELF `update-binary`** (the
`UPDATE_BINARY` is opened from the zip and `exec`'d; signzip path supports the
`UB_FILE` variant). We ship **static musl** binaries — both `aarch64` and
`arm` — because the userspace is Android but the kernel is armv7l `4.9.113`
(`recon.c`, `recon2..6.c`). Confirming an *arm32* `update-binary` executed (commit
`774610b`) is what proved code execution as `uid=0` in recovery.

Recovery protocol detail: `update-binary` must speak the updater protocol
(`ui_print` / `done`) or recovery treats the install as failed, and it must call
`/system/bin/sh` rather than a hard-coded `/bin/sh` (commit `9986e0d`).

### 3.5 The recovery bind shell

`recon5`/`R5` (`ssrf/pkg/updateR5.zip`) is the payload that mattered:

**Problem:** the bind shell never connected back, and no port was listening.
**Root cause:** recovery's `init` leaves **eth0 administratively DOWN** — there
is no `ifup`, no DHCP, nothing. Any connect-back or listen-only payload is dead
on arrival.
**Fix:** the payload configures the interface itself (`ip link set eth0 up` +
static `192.0.2.126/24` + gateway) *before* opening the shell
(commit `4c19d14`). That produced the first root shell on the box:
`nc 192.0.2.126 <port>`, `uid=0(root) context=u:r:recovery:s0`.

**Why the box's IP changes when it drops into recovery (this is not a bug, and
it will lose you the box if you forget it).** Stock Android runs on **WiFi**
(`.125`, from the router's DHCP lease), and that is the address whose OTA
traffic we poison. **Recovery has no WiFi at all** — no wireless driver, no
`wpa_supplicant`, no configuration; the recovery image only ever has wired
`eth0`. So the moment the box reboots into recovery, its WiFi address is gone
and the *only* way to reach it is an **Ethernet cable**, on the address we hand
it there (`.126`, deliberately a different static so it can never be confused
with the Android/WiFi lease). Two operational rules follow:

* **Plug the cable in before confirming the upgrade.** Once the box is in
  recovery with no cable and no WiFi it is completely unreachable, and the only
  way out is a power cycle back into Android.
* Expect the target to move: `192.0.2.125` while watching the OTA happen in
  Android, `192.0.2.126` for anything in recovery. Alpine later kept `.126`
  on `eth0` for exactly this reason — and when the built-in WiFi finally came up
  (§6), `udhcpc` re-acquired **`.125` on `wlan0`**: the same wireless chip, the
  same burned-in MAC (`02:aa:bb:cc:dd:01`), so the router recognised it and
  reissued the lease Android's WiFi had held. (Wired is a different MAC
  entirely: `02:aa:bb:cc:dd:01`, from `ro.boot.mac` in `backup/harvest/props.txt`.)

Operational trap that cost time: `Ncat` on the attacker side rejected the legacy
`-w 300` timeout form and silently killed the reconnect loop — hence the one-shot
batch runner (`tools/boxsh.sh`, `tools/batch_recon*.txt`).

### 3.6 Persistence without touching a bootloader

Recovery gives root but only in recovery. To keep root in Android the choice was
`/system/etc/prop.default`:

* SELinux is **permissive at boot** (`androidboot.selinux=permissive` is in the
  cmdline read from the live env);
* `/system`, `/vendor`, `/product`, `/cus_config` are plain ext4, **no
  dm-verity**, and writable from recovery (verified by an actual write test);
* `/system/xbin/su` is an AOSP **setuid test su**;
* `prop.default` (= `/default.prop`) is read by `init` on every boot and
  outranks `build.prop`.

So we appended (original saved as `prop.default.orig`, patch in
`notes/prop_default_patch.txt`):

```
ro.secure=0
ro.adb.secure=0
service.adb.root=1
service.adb.tcp.port=5555
persist.sys.usb.config=adb
```

→ network ADB as root, no RSA prompt. This is the "keep trying from the network"
foothold, and it is 100 % revertible by restoring one file.

**Problem (later, by design):** this whole Android-side persistence became
irrelevant once the box was converted to Linux-only (§5) — kept in the record
because it is the path that gets you root on a stock unit.

---

## 4. Replacing Android's recovery with our own Linux

### 4.1 Why the U-Boot env was *not* the lever

`tools/mkenv.py` decodes/edits the Amlogic env blob because **standard
`fw_printenv`/`fw_setenv` cannot**: the env has magic `\xe6\x8a\x59\x42`,
NUL-separated `k=v` and **no CRC**.

**Problem:** every hand-written env we flashed was rejected and the env silently
reverted to the factory copy.
**Root cause:** the first 4 bytes of the env block are a **hash over the data**
that is not plain CRC32 (observed values `9e99dec5` pristine / byte-swapped
display `c5de999e`). Brute-forcing the algorithm was not worth it.
**Decision:** treat env as read-only. `upgrade_step=2` left over from an early
experiment is the one env write still on the box (§9).

### 4.2 The trigger that does work: `reboot recovery`

U-Boot's `storeboot` falls through to `update`, whose tail includes
`recovery_from_flash` (partition **p6 = `/dev/recovery`**), gated on the
**`misc` partition's boot-recovery block**. Crucially:

* `reboot recovery` from Android writes that token → it boots p6;
* a **hand-written** `misc` boot-recovery block is **ignored** — only Android's
  own write is honoured. So `stage/misc.bin` is not a trigger. (This is exactly
  the asymmetry §3.1 exploited: the fake-OTA route made the *running system*
  write the token for us, which is why it worked where our own writes did not.)

Therefore: build our own Android-format boot image (kernel + ramfs init + dtb),
flash it to **p6**, and enter it with `adb reboot recovery`.

### 4.3 `tools/mkboot.py` and the Android boot image gotcha

`mkboot.py` rebuilds a byte-compatible Android v1 `boot.img` (round-trip verified,
so we know it is lossless). Two traps:

* the stock `boot`/`recovery` dumps carry **trailing junk** past the declared
  size; `mkboot.py` refuses them — feed it the clean prior image as the base;
* to keep the vendor header byte-identical, copy the stock **header page** from
  `backup/recovery_backup.img` over the new body (`page_size` at offset 36) and
  patch only `ramdisk_size` (offset 16) to the new gzip length. Verified: our
  image differs from stock only at bytes 16–18.

### 4.4 The ramfs init (`tools/tvinit.c` → `tvrd/init`)

Our first-party init, small and single-purpose, and its failure modes were each
worth a bug fix:

| Problem | Root cause | Fix |
|---|---|---|
| Kernel panic `Attempted to kill init!` | Alpine's `/dev` is **empty**; after `switch_root` there was no `/dev/console` | mount `devtmpfs`, `proc`, `sysfs` into the new root **before** `switch_root` |
| Stick rootfs never mounted | `vfat` **rejects** a `rw` data string in that mount form | use `MS_NOATIME` / correct flags (`tvinit.c:stick_mount`) |
| No trace of why a boot failed | logging started after the network stage | open `/mnt/usb/tvinit.log` **first**, so every boot leaves a forensic record |
| eth0 link never locks after a *warm* reboot | the `dwmac` PHY only locks on a cold boot | `net_up()` returns lock status; on failure re-arm the recovery token and reboot, bounded by a `/mnt/usb/phyfail` counter so it cannot loop forever |
| Rootfs choice raced the flaky PHY | ordering | run `try_rootfs()` (hand-off) ahead of the rescue bind shell, gated on link-up |

`switch_root` hand-off is factored into `do_handoff()` and the same source boots
from USB stick, eMMC `cache`, or eMMC `data`.

---

## 5. Linux on the internal eMMC, then Android deleted

### 5.1 Amlogic's non-standard partition table

`mmcblk0` is an 8 GB `HIKSEMI/8GTF4R` eMMC with **21 partitions that `fdisk`,
`blkid` and `lsblk` cannot name**: Amlogic exposes partition names from DT nodes
as `/dev/<name>`, with **minor N == mmcblk0pN**. The map that matters:

| Part | Node | Size | Role after conversion |
|---|---|---|---|
| p3 | `/dev/cache` | 1.1 GB | Linux rootfs **fallback**, later `/home` |
| p4 | `/dev/env` | 8 MB | U-Boot env — **read-only, never written again** |
| p6 | `/dev/recovery` | 24 MB | our `tvboot.img` (the boot lever) |
| p7 | `misc` | 256 KB | reboot-mode token (`tvinit` zeroes it each boot) |
| p11 | `/dev/boot` | 16 MB | was stock Android boot → **now the default Linux boot** |
| p18 | `/dev/system` | 1.2 GB | Android system (erased) → `/srv` |
| p21 | `/dev/data` | 3.6 GB | Android userdata (erased) → **Linux rootfs** |
| — | `boot0/boot1` | — | **untouched** |

### 5.2 Escalation ladder (each step reversible)

1. **USB stick** — `tools/mkrootfs.sh` builds an Alpine armhf rootfs loop image;
   boot from the stick via the p6 path, stick stays the rescue media.
2. **eMMC `cache`** (p3) — `dd if=rootfs.img of=/dev/cache bs=1M conv=fsync`;
   `tvinit` tries `cache` first → **no stick required**.
3. **Default boot** — write the same Linux boot image to **p11 `/dev/boot`**.
   `storeboot` does `imgread kernel boot; bootm`, and **there is no AVB/verified
   boot on that path**. If p11 ever fails it falls through to `update` →
   `recovery_from_flash` → p6 (same image), so it cannot lock up. Verified: a
   plain `reboot` with empty `misc` comes up in Alpine.
4. **Space** — Android deleted (user-authorised). `mkfs.ext4` on the freed
   partitions and mount by UUID (`stage/fstab`): `/dev/data` = root,
   `cache`→`/home`, `system`→`/srv`, `cus_config`→`/opt`, giving ≈6 GB usable.
   `tvinit`'s `mount_emmc_root()` order became **data → cache → USB**.
   `mount -a` before `switch_root`; `switch_root` is the sole source of
   `INIT_VERSION` in our path.

### 5.3 Problems specific to this conversion

* **Android fought back on p6.** `/system/etc/install-recovery.sh` +
  `/system/recovery-from-boot.p` re-flash stock recovery over our image at every
  boot. Neutralised (`install-recovery.sh` → `exit 0` stub, `.p` renamed) —
  original kept at `/data/local/tmp/install-recovery.sh.orig`.
* **`apk` could not install anything.** No RTC battery → clock resets to 2020 on
  every boot → TLS `certificate verify failed`. Fix: `rc.local` sets a floor
  (`date -u -s @1789776000`) early plus best-effort router NTP in the background
  (WAN/UDP-123 may be blocked). Then `apk add e2fsprogs-extra` (no `mke2fs` in
  base Alpine), `dropbear`, `screen`, `iw`, `wpa_supplicant` all work.
* **`cp -ax /` did not copy the rootfs** the way busybox suggests; real dirs had
  to be copied explicitly (`cp -a /bin /etc /sbin /usr /lib /var /root /opt`).
* **scp does not exist.** Alpine dropbear has **no `sftp-server`**
  (`sh: /usr/lib/ssh/sftp-server: not found`). Ship files with
  `ssh root@box 'cat > /path' < localfile` and verify with sha256.
* **Shell trap:** `pkill -f "[f]btest2"` inside the same `sh -c` line that also
  contains `/root/fbtest2` **kills our own ssh session** (silent, no output).
  Use `kill $(pidof name)`.
* **Root shell hardened:** the initial plaintext `busybox nc -lk -p 2323 -e /bin/sh`
  was replaced by **dropbear with password logins disabled** (`-s`), host keys
  auto-generated in `rc.local`, VM key in `/root/.ssh/authorized_keys`
  (commit `2f3f55f`). `tcp/2323` closed.

---

## 6. Built-in WiFi

This took six sessions and most of the wrong assumptions had to be retracted.

### 6.1 Wrong turns, recorded

| Believed | Reality |
|---|---|
| "Broadcom bcmdhd, built into the kernel" | `grep -c ' dhd_' /proc/kallsyms = 0`. Only `bcmdhd_init_wlan_mem` from the Amlogic **glue** exists; cfg80211/mac80211 are built in, **no NIC driver is**. |
| "Harvest a `dhd.ko` from another ROM" | Donor `dhd.ko` files are **ELF aarch64** (`e_machine 0xb7`) with vermagic 3.14.29 — a 32-bit kernel hard-rejects them, `--force` cannot cross architectures. |
| "Mainline/Armbian would fix WiFi *and* HDMI" | The chip is **not** Broadcom; mainline has no driver for it at all. Mainline would have fixed neither. |
| "Realtek RTL8821CS class (UWE5621)" | Live chip answered `chipid 0x56630001`, `MARLIN3E_20A_W21.19.2`, `uwe5623_marlin3E_ott` → **UniWave UWE5623 = Unisoc/Marlin**. Legacy `uwe5621_bsp_sdio.ko` + `sprdwl_ng.ko`, not `dhd.ko`. |
| "vermagic is `4.9.113 … p2v8`" | It is exactly **`4.9.y SMP preempt mod_unload modversions ARMv7`**. |

The chip family was settled from **Android properties harvested off the live
system** (`backup/harvest/props.txt`): `[vendor.bcm_wifi]: [uwe]`,
`[persist.vendor.bt_vendor]: [libbt-vendor_uwe_share.so]`,
`[ro.product.chiptype]: [AMLS905L3]`, `[rw.vendor.bluetooth.fw.ver]: [8532.…]`.

### 6.2 Powering the chip (no `/dev/mem`, no GPIO sysfs)

* `CONFIG_DEVMEM=n` and `/proc/kcore` absent ⇒ **static reverse engineering of
  the vendor driver was impossible**. The ioctl numbers were **brute-forced from
  userspace** with a gcc-compiled prober on the box (`wificmd.c`, `wififind.c`,
  `wifipoke.c`): magic `'m'` (0x6d), `_IO` no-size, nr 1..5.

  ```
  0x6d01 arg=1  "Set usb_sdio wifi power up!"
  0x6d03 arg=1  "Set sdio wifi power up!"   <- the one that works (GPIO486 hi + re-init)
  0x6d05        reports "wifi interface dev type: sdio, length = 4"
  ```

* `GPIO486` is owned by `aml_wifi` pinctrl ⇒ sysfs export is refused; only the
  `wifi_power` chardev (major 244) can drive `WL_REG_ON`.
* **Runtime `unbind`/`bind` of the SDIO host to force a rescan OOPSes
  `meson_mmc_remove`** (the BSP `meson-aml-mmc` driver has no clean hot-remove).
  Never do it; enumeration happens at boot only.
* Do **not** trust SDIO IDs read from sysfs after a power-up: the card node was
  stale (`vendor=0x0000 device=0x0000`) while the vendor driver's private
  `sdiohal` enumeration worked fine.

### 6.3 Getting a matching module (and a vermagic oracle)

`CONFIG_MODVERSIONS=y` with `CONFIG_MODULE_FORCE_LOAD` unset ⇒ the loader cleanly
refuses CRC mismatches with `-ENOEXEC` ("disagrees about version of symbol")
instead of oopsing. That makes harvesting prebuilt `.ko` a **safe experiment**.

**The vermagic oracle trick** (no source, no `/dev/mem` needed): feed the kernel
a module whose `.modinfo vermagic=` is binary-patched to nonsense; the loader
prints the *expected* string verbatim. That is how the exact vermagic in §6.1 was
obtained.

Compatibility was then **proven**, not assumed: `tb_detect.ko`, an unrelated
Amlogic 4.9.y armv7 out-of-tree module from a CM311 pack, loaded **and unloaded
cleanly** ⇒ neighbouring S905L3B ROMs share our kernel's symbol CRCs. No kernel
rebuild, no patching.

To find those `.ko` files: the donor ROMs are **not** ext4/f2fs/erofs/squashfs —
they are Amlogic **USP** images whose partitions are a sequence of extents with
12-byte headers `{magic=0x0000cac1, nblocks, 0xc+nblocks*4096}` followed by
`nblocks*4096` of payload; ext2-style dir entries live inside `DIR` extents.
`tools/unpack_aml.py`, `karve2.py` and `carveko.py` implement that walk and carve
complete `ET_REL`/`EM_ARM` modules out of raw blobs. Hard-won details: `e_type` at
offset 16, `e_machine` at 18, the ELF magic is 5 bytes `\x7fELF\x01`, the extent
must include the section-header table, and the module name sits at `+12` of
`.gnu.linkonce.this_module`.

### 6.4 The last missing file was RF calibration

With `uwe5621_bsp_sdio.ko` (4.5 MB) + `sprdwl_ng.ko` (7.8 MB) both loading, WCN
firmware downloading (it is **embedded in the `.ko`** as `wcnmodem.bin.hex`, no
`/lib/firmware` needed), the CP booting and answering AT, bring-up still failed:

```
wifi ini path = /vendor/etc/wifi/uwe5621/wifi_56630001_2ant.ini  -> open error
LOAD_INI_DATA_FAILED  -> "WIFI MAC can't be found wifimac.txt"
-> sprdwl_init_fw failed -> failed to register netdev (-5) -> no wlan0
```

Fix: extract the ten `wifi_*.ini` calibration files from the donor USP image
(the `2ant`/`3ant` pairs are byte-identical per chipid) and install them at
`/vendor/etc/wifi/uwe5621/` on the Alpine rootfs, plus create
`/data/misc/wifi/` for the MAC file. That was the whole remaining problem.

### 6.5 Hard constraint: one attempt per boot

`uwe5621_bsp_sdio`'s `init` registers the `sdiohal` sdio_bus driver and its
`exit` **never unregisters it**. After any `rmmod`, the next `insmod` fails:

```
Error: Driver 'sdiohal' is already registered, aborting
-> sdio_register_driver -16 -> wait SDIO rescan card time out -> chip power on fail
```

⇒ **never `rmmod` the bsp module**; the bring-up gets exactly one shot per boot.
So `wifi-up.sh` starts with "skip insmod if `wlan0` already exists".

### 6.6 Result

One-shot sequence `/root/wificmd 0x00006d03 1` → `insmod uwe5621_bsp_sdio.ko` →
`insmod sprdwl_ng.ko`:

```
sprdwl: mac_addr 02:aa:bb:cc:dd:01
unisoc_wifi unisoc_wifi wlan0: netdev registered
iw dev wlan0 scan -> 15 BSSes on 2.4 + 5 GHz, RSSI -37..-81 dBm
iw dev wlan0 link -> Connected 02:aa:bb:cc:dd:a3, 2442 MHz, -34 dBm,
                     tx 173 MBit/s VHT-MCS 9 VHT-NSS 2   (real 2-stream 11ac)
udhcpc -> 192.0.2.125   ping -I wlan0 223.5.5.5 0% loss   1.4 MB in ~1 s
#  .125 on wlan0 = the very lease Android's WiFi held (see 3.5); .126 stays eth0
```

Remaining messages are benign (`TLV check failed: type=0, len=0`, the
`mixed HW and IP checksum settings` notice, and `sprdwl` rx-mgmt spam).
Router identified: `192.0.2.1` = `02:aa:bb:cc:dd:a3` = SSID `MyWiFi24`
(2.4 GHz, ch 2442); `02:aa:bb:cc:dd:a4` = `MyWiFi50` (5 GHz).

Steady-state routing keeps **eth0 preferred** so the wired SSH lifeline is never
lost:

```
default via 192.0.2.1 dev eth0  src 192.0.2.126
default via 192.0.2.1 dev wlan0 metric 308
```

### 6.7 wpa_supplicant CLI gotcha that cost a whole cycle

wpa_supplicant v2.10 on this box **has no `-f` option** (that is a different
build's flag). Busybox-style behaviour made the failure invisible: it dumped
usage and **exited 0**, so the script "succeeded" while no supplicant ran. The
working invocation, used everywhere now:

```
wpa_supplicant -B -Dnl80211 -iwlan0 -c/etc/wpa_supplicant/wpa_supplicant.conf \
               -P/run/wpa_supplicant.pid
```

Credential policy in force: `wpa_supplicant.conf` (mode 0600) stores **only the
64-hex PMK**. The passphrase is piped on stdin into `wpa_passphrase`, the
`#psk=` line filtered out, the shell variable `unset`, and `grep` for the
plaintext returns nothing in any file. An SSID whose passphrase was never
supplied is deliberately **not** in the config — nothing is guessed.

---

## 7. The display fight

### 7.1 Mainline / Armbian route — attempted, fixed, then abandoned

* Upstream `meson_dw_hdmi` has **no GXLX2 glue**:
  `meson-dw-hdmi c883a000.hdmi-tx: Unsupported HDMI controller (0d0d:0d:0d)`.
* The 2026-07-18 patchset *"drm/meson: add HDMI support for GXLX2"*
  (zinan@mieulab.com) adds it; **ophub's 6.18.y** carries it.
* Known intermittent hang (ophub issue #3650) at
  `meson-drm d0100000.vpu: Queued 2 outputs on vpu`. Cause: GXLX2 HDMI needs the
  **direct** register map at `0xda800000`, but the driver's runtime
  `soc_major_id` detection is unreliable and falls back to the legacy `0xc883a000`
  indirect window → hardware hang. Workaround (ophub commit `93c07a9`) in the DTB:

  ```dts
  &hdmi_tx {
      compatible = "amlogic,meson-g12a-dw-hdmi";   /* force direct map */
      reg = <0x0 0xda800000 0x0 0x10000>;
  };
  ```

* Flashing the stick and booting it **without touching the eMMC boot path** used
  a proven, self-clearing lever: the `update`/`start_autoscript` chain reads
  `s905_autoscript` from a FAT partition on USB. `tools/mkautoscript.py` builds
  an Amlogic autoscript — a legacy uImage with `type=script` and a
  **non-standard 64-byte header (no timestamp field)**:
  `magic 0x27051956 @0`, `hcrc @4`, `size @8`, `load @12`, `ep @16`,
  `dcrc @20` (zlib crc32 of the payload), `os/arch/type/comp @24-27 = 00 00 0e 00`,
  `name[32] @28`, `pad[4] @60 = 16 6b d9 86`, payload `@64`.
  **`hcrc` is not verified by this u-boot** — `--selftest` proves the derivation
  against the file this box has already executed.
  This changed `vout` in the kernel cmdline across a reboot with **zero direct
  writes to `/dev/env`**, and pulling the stick guaranteed recovery.
  **Caveat found later (§7.7): "zero writes" was true for *our own* `dd`s, but any
  autoscript that executes `saveenv` latches its variables permanently — ophub's
  `aml_autoscript` does, and so does our probe `tools/t1.txt`. The live env is
  therefore no longer factory; read §7.7.2 before repeating that claim.**
* A second, even cleaner trigger: `tools/reboot_mode.c`
  (`reboot(MAGIC1, MAGIC2, LINUX_REBOOT_CMD_RESTART2, "update")`) latches Amlogic
  update-mode directly.
* De-risking detail that mattered: ophub's own `s905_autoscript` chainloads
  mainline via `if fatload usb 0 0x1000000 u-boot.ext; then go 0x1000000; fi`, so
  *both* the normal and the failure path had to be disabled (all ophub scripts on
  the stick renamed `*.disabled`) or the box "hung" for a reason unrelated to our
  test.
* Built `p291_headless.dtb`: **memory 2 GB → 1 GB** (both m302a and v2 wrongly
  declare 2 GB; the board has 1 GB) and `vpu`/`hdmi-tx` `status="disabled"` to
  remove the hang. Result: mainline reached **eth0 + DHCP .126** — so the headless
  DTB worked at kernel level — but userspace never came up (no sshd, no bootlog at
  the multi-user point), and `u-boot.ext` blob choice mattered: the
  `model_database` "canonical" `u-boot-s905x-s912.bin` made the box **fully
  offline**, while `u-boot-p212.bin` (606,670 B) + our DTB was the only combo
  that ever reached networking. Direct evidence on the board beats any
  model database.
* **Abandoned**: no HDMI picture and no shell from mainline, and debugging needs a
  USB-TTL serial console that is not available. Env was then restored to pristine
  `bootcmd=run storeboot` (header verified `9e99dec5`) — **that restoration has
  since been undone** by a `saveenv` inside an autoscript; see §7.7.2 for the
  env as it reads today.

### 7.2 Stock BSP kernel: how the black screen actually works

The claim in earlier notes — "`fb0` is OSD0 and it is **not routed** to HDMI, so
headless is a kernel limitation" — is **WRONG**, and disproving it is the story
of this section. Facts established by measurement (`recon/hdmi_*.txt`,
`tools/fb*.c`):

* The **HDMI transmitter is healthy all along**: `hdmi_init=1`, `ready=1`,
  `hdmi_used=1`, `hpd_state=1`, `config → cur_VIC: 16` (1080p60),
  `video_clk 148500000`, `avmute 0`. The panel *is* outputting a valid signal; the
  problem is purely "which plane, which memory".
* **The Android boot splash is not a Linux framebuffer.** u-boot loads a logo to
  **`0x3d800000`** and bootargs say `logo=osd1,loaded,0x3d800000,…`. On top of
  that, DT `meson-fb` declares `logo_addr = <0x3f800000>` inside the reserved
  `linux,meson-fb` region at the top of the 1 GB (`0x3f800000..0x40000000`), and
  `/sys/module/fb/parameters/osd_logo_index = 1`.
* `fb0` = OSD0 at `smem_start=0x2b400000` (`use_cma_first=1`), 1920×1080,
  `line_length=7680`, `smem_len=25165824`. `fb1` = OSD1: `smem_start=0x0`,
  32×32, `line_length=128`, `smem_len=1048576` — i.e. the plane that actually
  reaches the panel is one **Linux has no usable backing store for**.
* **Z-order was the first trap**: `fb0/order = 0x2`, `fb1/order = 0x0` ⇒ **osd1
  is ON TOP of osd0**. With the logo plane enabled and full-screen
  (`window_axis 0 0 1919 1079`), whatever you draw on fb0 is covered.
* **`CONFIG_FRAMEBUFFER_CONSOLE` is not set**, so the console never appears;
  `CONFIG_DEVMEM=n` (no `/dev/mem`, `mknod` won't help) and no `/proc/kcore`, so
  there is no way to read or write `0x3d800000` to test the logo-plane theory
  directly.
* `pstore`/`ramoops` claim `pstore_en=1` on the cmdline but **pstore is empty** and
  no ramoops registered ⇒ a hang leaves **no crash log**. Every blind experiment
  costs a power cycle and yields nothing but the observation that it is blind.

### 7.3 The fix — three things had to be true at once

1. **Kill the logo plane.** `echo 1 > /sys/class/graphics/fb1/blank` ⇒
   `osd[1] enable: 0`. Proof that this is the covering plane: the moment it is
   blanked the splash is replaced by our content/black, and any
   `/sys/class/display/mode` write **re-enables it** (`enable: 1` again), which is
   exactly why earlier tests kept "going back to the splash" and looked random.
2. **The OSD canvas is latched on an ioctl, not on writes.** Amlogic's VIU picks
   up the framebuffer only when userspace does what Android's graphics backend
   does every frame:
   `ioctl(FBIOPUT_VSCREENINFO, FB_ACTIVATE_NOW|FB_ACTIVATE_FORCE)` then
   `ioctl(FBIOPAN_DISPLAY)` — both with `yoffset = 0`.
   **`yoffset` is a silent killer**: `yv` is 2160 (double-height) by default, so a
   `yoffset` of 1080 makes `mmap` fail with `EINVAL` or points the panel at an
   empty page. `fbset -fb /dev/fb0 -g 1920 1080 1920 1080 32` normalises it.
3. **`osd_do_hwc` is the missing kick.** `/sys/class/graphics/fb0/osd_do_hwc` is
   **write-only** (`cat` → *Permission denied*, which is normal for a 0200 node and
   was mistaken for "unavailable"). `echo 1 > .../osd_do_hwc` completed the
   hardware-composition path, and **fb0 content then became visible on the
   panel** — verified with a full-screen colour cycle (blue→green→red→white→
   cyan→black) reproduced from a clean state.

Supporting geometry: `echo "osd0,0,0,1919,1079" > /sys/class/display/axis` (the
syntax is `osdN,x0,y0,x1,y1`; a malformed write produced
`pan_data(0,0,0,0)/dispdata(0,0,-1,-1)` and looked like a dead plane).

### 7.4 Things that turned out to be red herrings (documented so we don't retry them)

* **`window_axis` is a cache, not a measurement.** It reads `0 0 0 0` until
  userspace writes it; using it as "proof" that the plane was unprogrammed led us
  down a wrong path for a while.
* **`screen_real_width/height = 1440x810` is constant across modes** and did not
  change on mode bounces; it describes the video layer, not the OSD clip. Be
  careful with the history here: this only disproves *that* CVBS-related guess.
  The separate `vout=…cvbs` **bootarg** gate was real (commit `81e1200`, §7.7.1)
  — the two are different claims, and only the first was wrong.
* **`osd_blend_bypass` was falsely accused.** The box once went offline right
  after `echo 1 > osd_blend_bypass` and it was blamed; with empty pstore the
  causation is *unproven*, and the "hang with the stick inserted" that was
  attributed to it is fully explained by ophub chainloading mainline (§7.1).
* `cat /sys/class/graphics/fb0/osd_reg` **segfaults busybox**. Never read it.
* `display/axis` and the various `*_axis` files are single-slot: `cat` returns
  only the last-written line.
* Reading `osd_logo_index`, `ver_clone` (`osd_clone:[OFF]`), `bist`,
  `free_scale*`, `scale_axis`, `osd_plane_alpha (0x100)`, `osd_deband (1)` all
  produced no change when poked individually — with one exception:
  `echo 1 > /sys/class/graphics/fb0/ver_clone` does toggle `osd_clone:[ON]`.
* Useful, non-obvious debug interfaces worth knowing for next time:
  `echo info > /sys/class/graphics/fb0/debug` (dumps per-OSD `pan_data`/`disp_data`/
  scale windows into `dmesg`), `echo 7 > /sys/class/graphics/fb0/log_level` (makes
  the driver log every `osd_reg_read/osd_reg_write`), and
  `echo 1 > /sys/module/fb/parameters/dump_reg_trigger`.

### 7.5 Alpha: fb0 really has 8 bits and Xorg does not know

`fb0` is **ARGB8888 with `transp=24/8`**, and the hardware **honours per-pixel
alpha**. Xorg's fbdev driver writes `0x00RRGGBB` ⇒ *fully transparent* content.
Proved with a split-screen probe (identical RGB, left `alpha=0x00`, right
`alpha=0xff`): only one half lit up.

`FBIOPUT_VSCREENINFO` with `transp.length=0` is **refused** by the driver, so the
route is **16 bpp RGB565** (`fbset -fb /dev/fb0 -g 1920 1080 1920 1080 16` gives
`transp=0/0`, `line_length=3840`), which has no alpha field at all.

### 7.6 Open problems right now

* **RESOLVED since the 2026-09-20 follow-up session — see §7.7.7.** The
  vertical luminance gradient was the OSD **free-scale unit running with a
  degenerate (all-zero) source window**; `echo "0 0 1919 1079" >
  /sys/class/graphics/fb0/free_scale_axis` fixes it, and two further probe
  bugs (`fbrows` R/B swap, `fbfill` 32 bpp mask) explained every remaining
  "wrong" readout. Xorg at 16 bpp has now been **confirmed visible** on the
  panel (tasks #16, #40 closed), and the whole sequence is persisted in
  `/usr/local/bin/display-up.sh` called from `rc.local` (task #39 closed,
  pending one live-reboot verification).
* Kernel-side facts that bound any userspace-only fix: no `fbcon`, no
  `/dev/mem`, no `/proc/kcore`, empty `/lib/modules/4.9.113`.

### 7.7 Handoff — exactly where the display stands (verified live 2026-09-20)

Everything below was re-read from the running box at the end of the last session,
so a fresh session can start from facts rather than from archaeology.

#### 7.7.1 The fourth gate: `vout` in the bootargs, not in userspace

The last root-cause found (commit `81e1200`) is a **layer above** §7.3's three:
the box used to boot with `vout=576cvbs,enable`, which binds the VIU's *output
path* to the CVBS DAC at init, so **osd0 can never reach the HDMI encoder no
matter what userspace does** — signature: `osd[0] enable: 1` but with no
geometry, `osd_fps=0`, while `hdmitx` is completely healthy (`hdmi_init=1`,
`ready=1`, `cur_VIC: 16`, `video_clk=148500000`). `storeargs` composes that
string as `vout=${outputmode},enable`, so the fix is to change `outputmode`
**before** `storeboot`, from a USB autoscript:

```
# stage/hdmi_autoscript.cmd  -> tools/mkautoscript.py -> s905_autoscript on FAT32
setenv outputmode 1080p60hz
setenv hdmimode 1080p60hz
run storeboot
```

Runtime equivalent that also works (`vout,hdmi` / `switch,hdmi` pushed through the
vout command path) produced in `dmesg`: `vout: osd0=> x:0,y:0,w:1919,h:1079`,
`osd1=> x:0,y:0,w:0,h:0`, `fb: current vmode=1080p60hz`.

#### 7.7.2 Live state at the end of the session (re-read over SSH, read-only)

* **The box has since been power-cycled, so the picture is NOT currently up** —
  the §7.3 recipe is per-boot and nothing is persisted. Expect the Android
  splash on the monitor right now.
* bootargs now: `logo=osd1,loaded,0x3d800000,720p50hz vout=720p50hz,enable
  hdmimode=720p50hz cvbsmode=576cvbs` → **the CVBS gate (§7.7.1) is already gone**
  (output went HDMI-native at boot). Do not spend a power cycle re-proving it.
* `/dev/env` read-only dump: `bootcmd=run start_autoscript; run storeboot`,
  `outputmode=720p50hz`, `hdmimode=720p50hz`, `cvbsmode=576cvbs`,
  `display_layer=osd1`, `fb_addr=0x3d800000`. **The env is no longer factory**
  (our own note "restored to pristine `run storeboot`" is stale): an autoscript
  that ran `saveenv` latched `bootcmd` + `outputmode`. Practical consequence —
  the recovery invariant is now **power-cycle with the stick OUT ⇒ Alpine**, and
  a stick left in the box *is* consulted every boot.
* `/sys/class/display/mode = 1080p60hz` at runtime while bootargs said `720p50hz`
  ⇒ **the boot-time and runtime geometries currently disagree**, which is the
  prime suspect for the vertical gradient (§7.7.4).
* `fb0`: `bits_per_pixel=32` (the 16 bpp experiment did not survive the reboot),
  `order:[0x2]`; `fb1`: `order:[0x0]` (logo plane on top, not blanked now);
  `/sys/module/fb/parameters/osd_logo_index = 1`; `fb0/blank` and `fb1/blank`
  both read **empty** — that is normal, they are write-only.
* Write-only (`--w--w----`) nodes on `/dev/fb0` worth using:
  `osd_do_hwc`, `osd_clear`, `osd_single_step`, `osd_single_step_mode`,
  `ver_update_pan`, `free_scale_switch`.
* All probe binaries **and their sources** are on the box in `/root`
  (`fbcycle fbfill fbgrid fbhemi fbsplit fbfmt fbrows fbinfo fbpan fb1q fbtest2
  wificmd reboot_mode`), X config is in place at
  `/etc/X11/xorg.conf.d/90-amlfb.conf`, and the 117 GB stick is still in the box
  (`/dev/sda{,1,2}`, unmounted).
* Busybox quirks that will otherwise waste time: `fbset -i` **does not exist**
  (use `cat /sys/class/graphics/fb0/bits_per_pixel` or the `fbinfo` ioctl tool —
  `smem_start`, `line_length`, `xoffset`, `yoffset` are **not** sysfs files);
  `ip -br addr` prints nothing (use `ifconfig -a`); `seq` absent; some sysfs
  reads come back as "Binary output" (pipe through
  `tr -cd "\11\12\40-\176"`); `pkill -f <pattern>` inside a shell whose own
  command line contains that pattern **kills your session** — use
  `kill $(pidof name)`.
* **WiFi boot persistence is now VERIFIED** (this was an open item):
  `/var/log/wifi-up.log` ends with `INTERNET via wlan0: OK`,
  `wlan0 = 192.0.2.125` (`02:aa:bb:cc:dd:01`), `eth0 = 192.0.2.126`
  (`02:aa:bb:cc:dd:01`) — exactly the lease reuse described in §3.5.

#### 7.7.3 Probe → what was painted → what the eye reported

The monitor is the **only** sensor here (no `/dev/mem`, no pstore). These are the
actual readouts in order; the ones marked *no readout* were painted but never
described, so they are **not** evidence of anything.

| # | Probe run | Content | Readout (verbatim) | What it proves |
|---|---|---|---|---|
| 1 | `fbcycle` v1 (axis + FORCE-put + pan, fb1 **not** blanked) | solid full-screen cycle | "black" / "old splash picture" | put+pan alone does nothing while osd1 covers |
| 2 | `fbcycle` v2 (re-drive the canvas every frame) | solid cycle | "1 screen flash, still old splash pic" | a latch happens, then the logo wins |
| 3 | read `osd_status` / dmesg | — | "only black" | — |
| 4 | `fbset -fb /dev/fb1 -g …` + `fb1q` (probing fb1's backing store) | — | "old splash picture" | fb1 has no usable memory (also caused the aborts, §7.7.5) |
| 5 | `echo 0 > ver_clone` + **`fb1/blank=1`** + `fbcycle` | solid cycle | "i see green to red to gray( all are gradient)" | **first visible content** — and note "all are gradient" |
| 6 | `fbcycle /dev/fb0 3000 1` | 6 solids, 3 s each | "yes, i saw all the colors and now is black" | recipe confirmed end-to-end (it ends black by design) |
| 7 | `fbsplit` | left alpha `0x00`, right `0xff`, same RGB | "only 1, it's a dark blue -- blue -- dark blue gradient from top to bottom" | per-pixel alpha is honoured **and** a *solid* shows a vertical gradient |
| 8 | `fbfmt` (`transp.length=0`) | same as 7 | "like the blue one, dark -- normal --dark from top to bottom" | driver refuses XRGB, so no change |
| 9 | `fbset … 16` + `fbfill ff0000` / `00ff00` | solid 16 bpp | "bright red on top then gradient to black on bottom" / "bright green on top to black on bottom" | depth-independent: still vertical falloff |
| 10 | `fbgrid 6 4` | 24 labelled tiles | "bright red on top then gradient to black on bottom" (×2) | grid never resolved as tiles — same falloff as a solid |
| 11 | `fbhemi bottom` | bottom half only | *no readout* (only `osd[1] enable: 0` before/after in dmesg) | untested — worth running first, it discriminates content-vs-position |
| 12 | `fbrows 8` | 8 horizontal bands | *no readout* | the measurement §7.6 needs was never taken |

#### 7.7.4 Hypotheses for the vertical falloff, each with the killing test

1. **Boot/runtime geometry disagreement** (`vout=720p50hz` from bootargs,
   `display/mode=1080p60hz` set at runtime). Cheapest test, no reboot: paint
   `fbrows 8`, read out; then `echo 720p50hz > /sys/class/display/mode`
   (**re-blank fb1 afterwards — every mode write re-enables the logo plane**),
   repaint at 1280×720, read out. If the falloff scales with the mode, this is it
   and the permanent fix is the §7.7.1 autoscript so the two agree from boot.
2. **Canvas/frame mismatch in the VIU dump.** Observed:
   `canvas.addr=0x2b400000`, `canvas.width=7680`, `canvas.height=3240`,
   `frame.width=1920`, `frame.height=1080`, `out_addr_id=0x0` — a canvas **3×**
   the frame height. Test: force `yres_virtual` down
   (`fbset -fb /dev/fb0 -g 1920 1080 1920 1080 32`) and re-read the dump; if
   `canvas.height` becomes 1080 and the falloff changes, the driver's default
   double/triple-height virtual screen is the bug.
3. **`osd_fps=0` / `osd_hold_line=0x0`** — the OSD RDMA refresh never actually
   running, i.e. content latched once with a stalled line counter. Test: read
   `/sys/class/graphics/fb0/osd_fps` (and `osd_hold_line`) **while `fbcycle` is
   animating** vs idle; if fps stays 0 with visible content, the picture is being
   fed by a path we have not modelled and `osd_single_step` / `ver_update_pan`
   become the knobs to try.
4. **TV post-processing** — discounted: solid fields would not be affected and
   they show the same gradient.

#### 7.7.5 Things that cost a power cycle (do these only with the list open)

* `echo 1 > /sys/class/graphics/fb1/osd_clear` and/or `mmap` past fb1's 1 MiB
  ⇒ kernel hang, only a power cycle recovers (there is no nc fallback anymore).
* Writing the **mapped fb1** ⇒ `Unhandled fault: imprecise external abort
  (0x1c06)` repeatedly, and it briefly took SSH with it. fb1 is read-only for us.
* `cat /sys/class/graphics/fb0/osd_reg` ⇒ **segfaults busybox**.
* Runtime `unbind`/`bind` of the `d0070000.sdio` host ⇒ `meson_mmc_remove` OOPS
  (the WiFi era found this the hard way).
* Any write to `/sys/class/display/mode` silently **re-enables the logo plane** —
  it looks like a regression in the recipe, but it is just step 1 being undone.
* Reboots need explicit user go-ahead (standing rule): each blind boot costs a
  physical unplug and yields no log (empty pstore).

#### 7.7.6 Resume order for the next session

1. `ssh root@192.0.2.126`, then apply §7.3 in one shot (blank fb1 → axis →
   `fbhemi bottom` → per-frame FORCE-put + pan + `osd_do_hwc`) and **get the
   #11/#12 readouts that were never taken**.
2. Run hypothesis 1 (mode agreement) — it is free and needs no reboot.
3. Run hypothesis 2 (canvas height) — also free.
4. Only if both fail, build `stage/hdmi_autoscript.cmd` into
   `s905_autoscript` (`tools/mkautoscript.py`) on the FAT32 stick for a boot-time
   `1080p60hz` `vout`, remember `saveenv` side effects, and pull the stick
   afterwards to return to Alpine.
5. When (and only when) the geometry is right: `fbset … 16` + start Xorg at
   16 bpp (tasks #40, #16), then persist the whole sequence in `/etc/rc.local`
   **without any env write** (task #39), keeping dropbear-first ordering so a
   broken display hook can never cost us the box.

#### 7.7.7 RESOLVED — the fifth gate: a degenerate free-scale window (next session, verified live)

Readout #12 was finally taken at the start of this session (first `fbrows 8`
run, pre-diagnosis): **"from top to bottom a white-to-black gradient"** — the
bands did not resolve at all, the ramp dominated everything, exactly matching
row 10 of §7.7.3. #11 (`fbhemi`) turned out unnecessary.

The kill shot came from reading the OSD0 block of `echo info > fb0/debug`:

```
free-scale enable.h:1 .v:1          ← vertical scaler is ON
free-scale src data: 0 0 0 0        ← …with a DEGENERATE source window
free-scale dst data: 0..1919, 0..1079  ← …driving a full-screen output
```

and `cat /sys/class/graphics/fb0/free_scale_axis` = `0 0 0 0` (never
programmed). A v-scaler fed an empty source rectangle multiplies output
luminance by a top→bottom ramp **over whatever the scan path produced** — hue
preserved, brightness position-driven, content-agnostic. That is the whole
"every pattern is a gradient" mystery. Fix:

```
echo "0 0 1919 1079" > /sys/class/graphics/fb0/free_scale_axis
```

re-latch (FORCE-put + pan + `osd_do_hwc`) and the 8 bands appeared **as
distinct strips**. This is the **fifth gate**, sitting under §7.3's three and
§7.7.1's `vout` gate. It must be re-applied after every mode write (and
therefore every boot), because it is not persisted by anything else.

Hypotheses 1 and 2 of §7.7.4 are **dead**: the ramp disappeared while boot
`720p50hz` and runtime `1080p60hz` still disagreed, with `yres_virtual` already
1080. No reboot, no autoscript, no env write was needed.

**Two probe bugs made the hardware look worse than it was** (both fixed,
synced between `tools/` and box `/root`, sha256-verified, rebuilt on box):

* `fbrows.c` put the colour's *blue* byte at `red.offset` (and red at
  `blue.offset`). The observed post-fix-but-still-wrong strip order — Y↔C
  swapped, R↔B swapped, white/green/magenta unchanged, brown→dark-blue — is
  the exact signature of an R↔B byte swap; the TV and the VIU were innocent.
  After rebuild: order read back **correct and uniform top-to-bottom**.
* `fbfill.c`'s `ch()` masked *after* shifting
  (`(v << offset) & ((1<<len)-1)`), so every 32 bpp fill degenerated to
  `0xff000000` — opaque **black**. That is the "solid red went black"
  observation; the earlier correct red/green reads predate this path (16 bpp
  hardcoded). With the fix, `pix=0xffff0000` and the user read
  **uniform bright red full-screen** — gradient fully gone on solid fills too.

**Xorg confirmed visible** (tasks #16/#40): `fbset -fb /dev/fb0 -g 1920 1080
1920 1080 16` (`transp=0/0` ⇒ §7.5's alpha trap structurally impossible),
Xorg 21.1.14 fbdev at depth 16, openbox — uniform gray desktop, verified by
eye on the panel. Note the monitor is a **4K TV** (owner confirmed); irrelevant
then, since the box emitted 1080p60 (VIC 16) and every pattern read correctly
post-fix — **that did not survive the next cold boot**: the link cannot hold
1080p60's clock and the box now runs **1080p30**, see §7.7.8.
One config gotcha: X **ServerAbortFatals with no core pointer** on
this keyboard-less board — dummy `kbd` + `vmmouse` InputDevices in
`90-amlfb.conf` (plus a ServerLayout referencing them) make it start.

**Persistence (task #39):** `/usr/local/bin/display-up.sh` (source kept at
`stage/display-up.sh`) applies the full recipe in the one proven order —
`fbset 16` → mode write → **re**-blank fb1 → `ver_clone 0` → axis →
**free_scale_axis** → **window_axis** → FORCE-put/pan via `fbfill` →
`osd_do_hwc` → Xorg+openbox → **`fblatch` + `osd_do_hwc`** — logged to
`/var/log/display-up.log`, and is hooked at the **tail** of `/etc/rc.local`
after dropbear and wifi-up, so a broken display hook can never cost us SSH.

**The sixth gate, found by the live-reboot test:** after a real reboot the
script's own sequence looked fine (X up, "gray desktop", clean logs) but **no
X content ever reached the panel** — `xsetroot` color changes and a yellow
`xmessage` window were all invisible. Two more facts came out of that
debugging round:

* X's own mode-set ioctls are **non-FORCE**, so once X owns fb0 the VIU keeps
  scanning a stale latched frame; and the failed `FBIOPUT` (`put=-1`) from a
  misused `fbpan` experiment put the plane into a state where even the plain
  recipe stopped showing. **Recovery that demonstrably works**: full recipe
  *including the `1080p60hz` mode write and an explicit
  `echo "0 0 1919 1079" > window_axis`*, then paint. Hence both now live in
  the script.
* The fix for X liveness is **`tools/fblatch.c`** — `FBIOPUT_VSCREENINFO(NOW|
  FORCE, yoffset=0)` + `FBIOPAN_DISPLAY` *without painting anything*, run
  after Xorg is up (and re-runnable any time: `DISPLAY=:0 xsetroot …; /root/
  fblatch; echo 1 > osd_do_hwc`). Final verification, all read by eye on the
  4K TV: solid red full-screen **uniform**; script end-to-end → root painted
  **blue** with a **yellow "HELLO" window** on top, **visible**.
* Refinement found right after: `fblatch` is needed **once at the ownership
  transition** (it is already the script's last step), not per update — an
  orange `xmessage` popped up and was visible with **no relatch at all**. The
  mouse **cursor is visible even before any latch** because Amlogic drives it
  as a separate hardware cursor plane, which also explains the classic
  "cursor on a black screen" readout.

**Desktop stack — now XFCE4 (superseded the lite openbox+tint2 build):** the
first usable desktop was `tint2`+`pcmanfm --desktop`+`lxterminal` under
`openbox-session`, and the key lesson from it still holds: **bare `openbox`
does not run `~/.config/openbox/autostart` — only `openbox-session` does**
(that is what turned a bare cursor into a real desktop). Once XFCE4.18
(`xfce4` meta, armhf, 466 MiB / 350 pkgs total, **199 MB RSS** with the whole
session loaded on the 989 MB box) was confirmed perfect by eye, it replaced
the lite stack as the default session: `dbus-launch startxfce4`
(`xfdesktop` draws `~/Desktop` icons natively, `thunar`, `xfce4-panel`,
`xfce4-terminal`). The now-redundant `tint2`/`pcmanfm`/`lxterminal` (+ `vte3`,
`libfm`, `menu-cache`) were `apk del`'d. `xfwm4`'s compositor is **off** (the
`xfwm4.xml` seed sets `use_compositing=false`) — this fbdev/16 bpp path has no
GLX (`swrast_dri.so` is absent) so compositing would only cost RAM. openbox is
kept as a one-line fallback. `~/Desktop` carries four icons: Terminal
(`xfce4-terminal`), Files (`thunar`), and the two power shortcuts below.

**Power without an init system (a dead end worth recording).** The box has
**no logind / ConsoleKit / seatd**, so XFCE's own logout dialog's
Restart/Shut Down buttons are **inert** (confirmed: `xfce4-session` does
reference `org.freedesktop.login1` and `…ConsoleKit`, but there is **no D-Bus
system bus at all** — only the session bus `dbus-launch` spins up). The
obvious fix — `apk add seatd` — is **wrong for this**: `seatd` provides
`libseat` (device access for X/Wayland), *not* the `login1` D-Bus interface the
dialog needs; it would light up nothing. The real native option is **elogind +
a dbus system bus**, but on a bare busybox-init box that means two more
daemons in a boot chain whose top rule is *self-recover on power-cycle* — the
owner chose **zero-risk** instead. So `seatd` was purged and power is two
confirm-prompt desktop icons: `~/Desktop/Reboot.desktop` →
`/usr/local/bin/reboot-prompt.sh` and `ShutDown.desktop` →
`shutdown-prompt.sh` (each an `xmessage` with **Cancel** as the default button,
then `/sbin/reboot` / `/sbin/poweroff`). Native-looking, no new boot surface.

**Cold-boot verification (2026-09-21) found three more things, all now fixed in
the boot script:**

* **No desktop after a *cold* boot although the warm one was fine.** rc.local
  runs with no `HOME`, so `openbox-autostart` looked for
  `/.config/openbox/autostart`, did not find it, and silently skipped the
  desktop. Fix: launch with `HOME=/root DISPLAY=:0 openbox-session`.
* **USB mouse invisible to X.** There is **no udev daemon on this box at all**
  (busybox init, no `/etc/init.d`), and Xorg 21 hotplug-off means zero input
  devices. Static config is a dead end here: with hotplugging on, X
  **disables `kbd`/`vmmouse`/`mouse` sections outright**, and `evdev` cannot
  open `/dev/input/mice` (it is the psaux multiplexer, not an event interface —
  `Unable to query fd: Not a tty`). Fix: `apk add eudev`; `udevd --daemon` +
  `udevadm trigger` **before** Xorg starts — X then attached the mouse through
  libinput and even discovered the front-panel `aml_keypad` by itself. The
  dummy kbd/vmmouse sections (earlier in this section) are **retired**;
  `90-amlfb.conf` is back to fbdev-only.
* **Cursor present but frozen — a *hardware-visible* USB fault.** `dmesg`
  showed the mouse disconnecting and re-enumerating **every ~2 seconds**
  (`USB disconnect … new low-speed USB device …` forever): `usbcore` defaults
  `autosuspend=2` on this kernel and the xhci port powers off the suspended
  low-speed device, dropping it off the bus. Fix: `echo -1 >
  /sys/module/usbcore/parameters/autosuspend` — churn stops immediately and
  survives replug. Both the udev start and the autosuspend kill are now tail
  steps of `display-up.sh`, so the whole input chain is unattended again.

#### 7.7.8 The seventh gate: the TMDS **clock**, not the recipe (verified live 2026-09-21)

The deferred cold power-cycle was performed and **the fixed chain worked**.
`display-up.sh` ran clean at boot — dmesg: `vout: new mode 1080p60hz set ok`,
`fb: osd_update_disp_axis_hw:dispdata(0,0,1919,1079)`, `fb: osd[1] enable: 0
(display-up.sh)` — Xorg/xfwm4/xfdesktop/xfce4-panel/udevd all alive, both axes
still programmed, bpp 16. **The panel still read "no signal".** All six gates
were satisfied and there was still no picture, which is what forced this below
everything examined so far.

The cause sits one layer under §7.3/§7.7.1/§7.7.7: **the link cannot hold the
148.5 MHz TMDS clock that 1080p60 (and 1080p50) require.** Measured, not inferred:

| mode set | `tmds_clk` (dmesg) | HPD drops while sampled |
|---|---|---|
| `1080p60hz` | 148500 | every **~8.16 s**, continuously |
| `720p50hz` | 74250 | 0 / 25 s |
| `1080p30hz` | 74250 | 0 / 12 s |
| `1080p25hz` | 74250 | 0 / 12 s |
| `1080i50hz` | 74250 | 0 / 12 s |

Why this hides so well: **every transmitter-side health check passes while the
link is failing** — `hdmi_init=1`, `hpd_state=1` between the drops,
`config → cur_VIC: 16`, `tmds_clk 148500`, `avmute 0`, `vid_mute 0`,
`edid_parsing ok`. The sink loses lock on the *data*, pulls HPD low for ~1 s,
and the driver treats that as a genuine unplug → re-runs EDID + mode-set →
**and that is what destroys the picture**: the mode-set re-enables the fb1 logo
plane and discards the FORCE-put/pan latch. So the §7.3 recipe is silently
undone from under you, every 8 seconds, leaving nothing in `dmesg` but
`plugout`/`plugin`.

Diagnostics that actually discriminate (`/sys/class/amhdmitx/amhdmitx0/` — note
the class is `amhdmitx`, so `cat /sys/class/amhdmitx0/hpd_state` is simply the
wrong path on this build):

* sample `hpd_state` once a second for ~20 s and **count the zeros**. A
  metronomic interval (8.16 s here) is a negotiated retry; a bad contact is
  irregular;
* `preferred_mode` (this TV's EDID answers `720p50hz`), `sink_type`,
  `edid_parsing`, `config` for `cur_VIC`, `dmesg | grep tmds_clk` for the clock;
* **`fake_plug=1` looks like a cause and is not** — clearing it left the flap at
  exactly the same rate, so read-back of that debug node is not a diagnosis;
* absent on this BSP: `hpd_state_check`, `5V_state`; `phy`,
  `hdmi_config_info` and `swap` read empty.

Resolution: run at **`1080p30hz`** — full 1920×1080 progressive at 74.25 MHz,
the highest-quality mode in the band this link locks. **This is a workaround,
not a verdict about the box.** §7.7.7 read every colour pattern correctly on this
same TV at 1080p60, so 148.5 MHz *has* worked here; something physical (cable,
TV input, the 5V/HPD handshake) changed since. Re-test 1080p60 on a different
cable or input before blaming the box's HDMI PHY.

Two changes to `stage/display-up.sh` (deployed to `/usr/local/bin/`):

1. `MODE`/`W`/`H` are variables at the top and `display/axis`,
   `free_scale_axis` and `window_axis` derive `W-1`/`H-1` from them, so fb
   geometry and OSD geometry can no longer drift apart — §7.7.4 hypothesis 1 was
   still live in the hardcoded version.
2. **Session teardown moved to the *top* of the script.** A live Xorg owns fb0
   and restores its own `var` over our `fbset`: the first cut, which tore the
   session down just before starting Xorg, produced `fb0 xres=1920 yres=1080`
   against axes programmed for `1279x719`. Teardown
   (`kill $(pidof xfce4-session)`, `kill $(pidof Xorg)` — never `pkill -f`,
   §5.3) must precede *any* fb or mode write, which also makes the script safely
   re-runnable over SSH with no reboot at all.

Current state as of this writing:
X + **XFCE4** (panel + xfdesktop icons + thunar + xfce4-terminal) live on fb0 at
16 bpp **at 1080p30hz / 1920×1080**, confirmed by eye on the 4K TV ("looks
perfect") with `hpd_low=0/25`. The cold power-cycle that §9 used to list as
outstanding **has been done**: the three §7.7.7 input-chain fixes held
(`HOME=/root`, udevd before Xorg, `autosuspend=-1` all read back correct), and
the only new failure was the clock gate above. The box warm-rebooted and
cold-booted through the persisted `rc.local` path; network and WiFi come up
unattended.

---

## 8. Reproducible runbook

```bash
# --- network MITM (attacker VM 192.0.2.220) ---
sudo bash ssrf/ota_poison.sh 20          # ARP+NDP spoof, DNS poison, :53/:80 redirect
python3 ssrf/ota_server.py 80            # BestV OttService JSON + Range serving
# package: ssrf/signzip.py + tools/mk_pk7.py with AOSP testkey (keys/)
#   -> update.zip with ELF update-binary, META-INF/com/android/metadata,
#      no-authAttrs PKCS#7 in the zip comment
#      (offer LastedVersion 3.0.2.0 > client's reported 3.0.1.4, FileHash =
#       uppercase MD5, SoftSize = exact bytes)

# --- trigger: no button, no adb. the box does it itself ---
# box UI checks UpgradeOSV2 -> shows "发现新版本" -> downloads /pkg/update.zip
#   (Range/206) -> client writes misc(mmcblk0p7) BCB "boot-recovery" +
#   --update_package=... -> reboots -> stock recovery screen -> verifies
#   testkey -> execs our update-binary as uid=0
# recovery has NO WiFi (wireless-less image): plug the Ethernet cable in BEFORE
#   confirming the upgrade, or the box is unreachable until you power-cycle.

# --- root shell in recovery ---
# payload brings eth0 up itself, then:
nc 192.0.2.126 <port>                 # uid=0 u:r:recovery:s0

# --- Android-side persistence (stock unit) ---
echo '...ro.secure=0 / service.adb.tcp.port=5555...' >> /system/etc/prop.default
adb connect 192.0.2.126:5555

# --- our Linux ---
python3 tools/mkboot.py stage/tvboot.img tvrd stage/tvboot.new.img   # then stock-header patch
dd if=tvboot.img of=/dev/recovery bs=1M conv=fsync                   # p6
dd if=tvboot.img of=/dev/boot     bs=1M conv=fsync                   # p11 = default boot
adb shell su 0 reboot recovery            # only Android's misc write is honoured

# --- daily use, now ---
ssh root@192.0.2.126                   # dropbear, key-only
/usr/local/bin/wifi-up.sh                 # once per boot (sdiohal single-load)

# --- display: persisted since §7.7.7; runs from rc.local every boot ---
# Manual re-apply (same order as /usr/local/bin/display-up.sh):
kill $(pidof xfce4-session) $(pidof Xorg) 2>/dev/null
                                              # 0. a live Xorg owns fb0 and will
                                              #    restore its own var over our
                                              #    fbset — tear it down FIRST
                                              #    (never pkill -f, §5.3)
echo 1080p30hz > /sys/class/display/mode      # 1. SEVENTH GATE (§7.7.8): stay at
                                              #    74.25 MHz. 1080p60hz/1080p50hz
                                              #    need 148.5 MHz, the TV will not
                                              #    lock, and its HPD retry every
                                              #    ~8s re-enables the logo plane
                                              #    and discards the latch below.
                                              #    Check with: sample
                                              #    amhdmitx/amhdmitx0/hpd_state
fbset -fb /dev/fb0 -g 1920 1080 1920 1080 16  # 2. RGB565: no alpha field
echo 1 > /sys/class/graphics/fb1/blank        # 3. kill the logo plane. Re-do after
echo 0 > /sys/class/graphics/fb0/ver_clone    #    ANY display/mode write.
echo "osd0,0,0,1919,1079" > /sys/class/display/axis   # 4. geometry (osdN,x0,y0,x1,y1)
echo "0 0 1919 1079" > /sys/class/graphics/fb0/free_scale_axis
                                              # 5. THE FIFTH GATE (§7.7.7): the
                                              #    boot default 0 0 0 0 makes the
                                              #    v-scaler paint a luminance ramp
                                              #    over ALL content
echo "0 0 1919 1079" > /sys/class/graphics/fb0/window_axis
                                              #    (part of the proven recovery)
/root/fbfill /dev/fb0 ff0000 5               # 6. paint + FORCE-put + pan latch
echo 1 > /sys/class/graphics/fb0/osd_do_hwc  # 7. kick hardware composition
# SIXTH gate (§7.7.7): X's mode-set is non-FORCE — display-up.sh ends with
# /root/fblatch once at the hand-off; live X updates then reach the panel
# without relatching (cursor plane is hardware, always visible).
# Input chain (cold-boot round, §7.7.7): HOME=/root on the session launcher
# (default session is now XFCE: HOME=/root dbus-launch startxfce4);
# udevd --daemon + udevadm trigger BEFORE Xorg; echo -1 >
# /sys/module/usbcore/parameters/autosuspend (kills the 2s mouse churn).
# never do these: cat fb0/osd_reg (segfaults busybox), mmap/write fb1,
#   fb1/osd_clear, unbind d0070000.sdio — each one cost a power cycle.
```

To undo to a stock-only box: restore `recovery_backup.img` to p6, wipe `/dev/cache`,
restore `install-recovery.sh`, and re-flash a vendor burn package (Android's
payload was deliberately erased; no full system/vendor backup exists).

---

## 9. Loose ends

1. **HDMI: RESOLVED** (§7.7.7) — the vertical gradient was the OSD free-scale
   unit running with its source window left at the boot default `0 0 0 0`
   (fifth gate), and the last "wrong colours" readouts were bugs in our own
   probes (`fbrows` R/B swap, `fbfill` 32 bpp mask). Xorg at 16 bpp is live on
   the panel — **with the sixth gate**: X owns the buffer non-FORCEfully, so
   `fblatch` (FORCE-put+pan, paints nothing) after X start is what makes
   rendering appear — once, at the ownership transition; live updates then
   flow without relatching. The full recipe runs from `/etc/rc.local` via
   `/usr/local/bin/display-up.sh`; the cold boot then found the three bugs
   above (all fixed and in the boot path: `HOME=/root`, eudev before Xorg,
   `autosuspend=-1`). Session is **XFCE4** now (`dbus-launch startxfce4`,
   compositor off; the lite tint2/pcmanfm stack was installed first, verified,
   then replaced and `apk del`'d).
   **Then the cold boot found a seventh gate, and it is the shipped
   configuration** (§7.7.8): the panel went "no signal" with all six gates
   satisfied, because the link cannot hold the 148.5 MHz TMDS clock 1080p60
   needs. Running at **`1080p30hz`** — 1920×1080 progressive at 74.25 MHz — is
   HPD-stable and visually confirmed. So: the earlier "one more cold power-cycle"
   item is **done and passed** (the three input-chain fixes held), and HDMI is
   resolved *at 30 Hz*. Two things follow, both deliberate:
   * `/dev/env` still boots `outputmode=720p50hz` while `display-up.sh` writes
     `1080p30hz` at runtime, so boot and runtime modes **disagree by design**.
     Unlike §7.7.4 hypothesis 1 that is harmless here — the script programs fb
     geometry and every axis *after* its own mode write, in one pass.
   * **1080p60 is a workaround casualty, not a dead box**: it worked throughout
     §7.7.7 on this same 4K TV, so re-test it on another cable/input before
     suspecting the HDMI PHY.
   Still open: a native logout dialog would need elogind + a D-Bus system bus
   (rejected on risk grounds, §7.7.7) — until then use the desktop
   **Reboot**/**ShutDown** icons.
2. **WiFi boot persistence is VERIFIED** — `/var/log/wifi-up.log` ends
   `INTERNET via wlan0: OK` after a real power-cycle. Nothing left here; the
   `sdiohal` single-load-per-boot rule (§6) is the only thing to remember.
3. **`/dev/env` is no longer factory.** It reads
   `bootcmd=run start_autoscript; run storeboot`, `outputmode=720p50hz`,
   because a `saveenv` inside an autoscript latched it (§7.1 caveat, §7.7.2), and
   `upgrade_step=2` from an early experiment is still in there too. Practical
   consequence: the recovery invariant is now **power-cycle with the USB stick
   OUT ⇒ Alpine**; a stick left in the box *is* consulted every boot. Clearing
   any of it needs a deliberate env write (approval required).
4. **Credential hygiene: CLOSED.** §9 used to claim `zte.py` / `zte_browser.py`
   hard-coded the router admin password — that predates the publication scrub.
   Re-checked in this tree: both now read
   `os.environ.get("ZTE_ROUTER_PASSWORD", "")`, and because this repo has a
   **single commit** which already contains the env-based form, the literal
   password is **not** in the git history either. `.gitignore` covers
   `*pass*`, `*.pem` (except `keys/`), `*.pcap`, `*.log`, `fw/`, `backup/`,
   `parts/`, `inis/`. Nothing left to do here.
5. Optional: Bluetooth on the same UWE5623 combo (`sprdbt_tty.ko`, carved,
   untested).

---

# PART II — 中文版(中文说明)

**提权链条一句话**(对应第 2–3 节):我们**伪造了厂商的 OTA 升级服务器**(只针对这台
机器做 DNS 投毒 `qhup.bestv.com.cn`),用与机器 `otacerts.zip` 匹配的 **AOSP testkey**
签了一个包推给它;机器自己的升级客户端下载之后,**由它亲手往 `misc` 写 boot-recovery
令牌并重启进 recovery 来安装**——那个 recovery/升级界面就是我们用假升级包叫出来的。
全程不需要组合键、不需要串口、也不需要事先有 adb。

## 0. 全程遵守的安全底线

* 机器必须**断电重启即可自恢复**——它藏在电视后面,一次死机就等于一次物理拔电。
* **绝不**写 `/dev/env`、`bootloader`、`boot0/1`、`param`,也不动路由器。U-Boot
  环境变量和 bootloader 是唯一真正不可恢复的东西。
* 不经过用户明确同意不触发重启。
* 明文口令不落盘、不进 git:只把 PMK 写进 `wpa_supplicant.conf`(0600)。
* eMMC 上的 `data`/`cache` 可以折腾(可从 Linux 重刷),boot 链不行。
* 整个过程守住了一条不变式:实验后一律把 `bootcmd` 恢复成原生的 `run storeboot`,
  所以断电一定能回到已知的 Linux。

## 1. 网络侦察

机器在 **tcp/38520** 上跑 UPnP/DLNA 媒体服务(CyberLink `CyberHTTP` 协议栈),暴露
`AVTransport` 和 `ConnectionManager`,两个服务的描述文件已抓成
`ssrf/avt.scpd.xml`、`ssrf/cm.scpd.xml`。

* **问题**:没有 ADB、没有 telnet、没有 SSH,管理面被路由器隔离。
* **突破口**:`AVTransport.SetAVTransportURI` **无需任何认证**就能让机器去拉取任意
  URL——本质上是 SSRF/请求伪造。`ssrf/oracle.py` 和 `ssrf/redir.py`(302 跳转链 +
  慢响应端点)证明机器确实会主动发起 GET 并跟随跳转,同时抓到的
  `User-Agent`/`Range` 头给协议栈做了指纹。
* UDP 全网扫描 + SSDP/WS-Discovery 探测(`ssrf/scan.py`)摸清其余服务;
* `kitchen/` 是上游项目 **AmlogicKitchen**,用来解 Amlogic 的 pkg/img 容器。始终没有
  找到公开可用的 R1200-C 固件(task #35),但后来找到的同类 S905L3B Android-9 镜像
  恰好成了 WiFi 驱动和 WiFi 校准数据的来源(见 §6)。

## 2. 把机器的流量劫持到我们这边

`ssrf/ota_poison.sh` 是整套中间人流程,**只对机器的 IP 生效**,LAN 和路由器都不受
影响:

* `minispoof.py`:手工构造 ARP 应答,污染机器的默认网关(同时反向污染路由器上的机器
  条目),退出时再重新 ARP 纠正双方;
* 机器还有一条**公网 IPv6** 地址,厂商服务器走的是 v6,**所以只做 IPv4 重定向时抓到的
  包是空的**,必须配合 `ndpspoof.py` 做 NDP 欺骗;
* `iptables/ip6tables` 把 `-s $BOX --dport 53,80` `REDIRECT` 到我们的端口;
* `dnsmasq-poison.conf` 只针对机器把 `bestv.com.cn`、`qhup.bestv.com.cn` 解析到攻击
  机 `192.0.2.220`,其余照常转发给路由器——机器全程"正常上网",只有升级检查被
  接管;
* 保留 `MASQUERADE`+`FORWARD`,劫持期间不断网,因为厂商的若干流程一旦断网就会中止。

**抓到的 OTA 协议**:升级界面(OttService)向 `qhup.bestv.com.cn` 请求,返回一个
`Response/Header{RC,RM} + Body{UpgradeMode,LastedVersion,FileHash,CompressType,
SoftName,SoftSize,SoftCode,FileURL,Desc}` 的 JSON,`ssrf/ota_server.py` 完整复刻。

* **坑 1**:客户端对字段集合极其敏感。只有"长格式"(字段齐全、按厂商一样带 `\n`
  缩进、`RC:0` 且 `RM` 为中文)才会弹出"发现新版本"对话框;语义相同但精简的 JSON 界
  面完全不动。解法:逐字节模仿抓包。
* **坑 2**:下载走 HTTP `Range`。所以服务端必须真正实现 `206`/`Content-Range`
  (整段请求时回 `200`)。忽略 Range 会让进度条卡在固定百分比且不报错。
* 不攻击时对其他端点回 `{"RC":0,"RM":"没有升级计划"}` 让机器安静。

## 3. 拿到代码执行:伪造签名 recovery 包

一句话链条:**我们冒充厂商升级服务器 → 机器下载我们的包 → 机器自己的 OTA 客户端
往 `misc` 写 boot-recovery 令牌并重启进 recovery → recovery 验签(testkey)通过 → 以
root 执行我们的 `update-binary`**。

### 3.0 触发:recovery 那个"升级页面"就是我们的假包叫起来的

原厂机器上**没有 recovery 组合键、没有串口、也没有 adb**,而手写的 `misc` 块 U-Boot
根本不认(见下面第 4 节)。唯一能进 recovery 的入口,就是**厂商自己的升级客户端**——所以我们
去当升级服务器,让机器自己走进 recovery:

1. **DNS 投毒** `qhup.bestv.com.cn`(见第 2 节劫持脚本),于是升级界面周期性的
   `POST /upgrade/OttService/UpgradeOSV2`(还有 `UpgradeInsideV2`、`GetMessage`)落到
   `ssrf/ota_server.py`。客户端在 `User-Agent` 里自报版本:
   `OSVersion/BesTV_R1200-C_QHHZ_3.0.1.4`。
2. **必须回一个"真的更新"的版本号**:`LastedVersion` 要比客户端报上来的高
   (`3.0.1.4` → 我们给 `3.0.2.0`),否则客户端认为"无可安装项",后面什么都不发生。
   其余字段也要自洽:`RC:0`、中文 `RM`、`UpgradeMode:1`、`FileHash` = **我们实际吐出的
   那串字节的大写 MD5**、`SoftSize` = 精确字节数、`FileURL` 指向我们自己的 `/pkg/…`。
   任何不一致 → "发现新版本"对话框根本不渲染。
3. 用户点"升级"(或客户端自动下),它 `GET FileURL`,并且**用 `Range` 断点续传**,所以
   `ota_server.py` 必须真做 `206`/`Content-Range`(第 2 节"问题2")。
   `ssrf/ota_requests.log` 里那一串来自 `192.0.2.125` 的 `GET /pkg/update.zip` 就是
   机器在拉载荷——`.125` 是机器 **WiFi** 上的地址(为什么地址会变,见下面 bind shell 一条)。
4. 客户端校完 MD5,做的等效于 `RecoverySystem.installPackage()` 的事:**往 `misc`
   (= mmcblk0p7)写 BCB(bootloader message)**——偏移 0 的 64 字节命令槽填
   `"boot-recovery"`,再带上 `--update_package=<路径>` 参数(上游行为见
   `tools/recovery9.cpp` 的注释:*`get_args()` writes BCB with `boot-recovery` and
   `--update_package=…`*),然后重启。
5. U-Boot 看到令牌 → 引导 **p6 = `/dev/recovery`** → 原厂 recovery 画出"正在升级"
   界面(Android logo + 红三角)。**这个界面就是我们要的入口**;它不是我们"破解"出来的
   页面,而是 recovery 在老老实实在装我们提供的包。
6. recovery 用 `res/keys` 验包(见下面 testkey / PKCS#7 两条),然后**以 `uid=0`、context
   `u:r:recovery:s0` 直接 `exec` 我们的 `update-binary`。

两个当时浪费时间的隐含结论:

* 因为 BCB 是**运行中的系统自己的升级路径**写的,它才被 U-Boot 认账;这正是我们手写
  `misc` 时不具备的身份。所以假 OTA 这条路由不是"方便",而是**没有物理接触时唯一
  的路由**。
* recovery 本身不是目的地,它只是个沙箱:在里面装的东西不会带回 Android。持久化必须
  从 recovery 内部写 `/system`(见下面 `prop.default` 一条),最终我们干脆把自己的 Linux 换进 p6(第 4 节)。

* **决定性发现**:机器是 `test-keys`,仓库 `keys/` 里的 **AOSP 标准 testkey 与机器
  `otacerts.zip` 匹配**。"无法伪造签名"当场变成"想签什么签什么"。下面的一切都建立在这
  一点上。
* Android 9 recovery 校验的是 **zip 尾部注释(ECD comment)里的整包 PKCS#7**,签名对象
  是"去掉注释字节"的归档摘要。两个硬知识点:
  1. `META-INF/com/android/metadata` 必须存在且必须在签名覆盖范围内,否则 recovery
     直接拒绝(commit `4f4a482`);
  2. `SignerInfo` **不能带 authenticationAttributes**。recovery 的校验器是
     `asn1_context` 那套(我们读了 `tools/asn1_decoder.cpp`、`tools/verifier9.cpp`
     上游源码),只对 content 做摘要。用普通 OpenSSL 命令行签出来的 PKCS#7 一定带
     authAttrs,机器只回一句 `Signature verification failed`。解法:`ssrf/signzip.py`
     (JAR v1 manifest 分段哈希)+ `tools/mk_pk7.py`(手搓无 authAttrs 的 DER
     `SignerInfo`)。
* **`update-binary` 可以是 ELF**:Android 9 recovery 会直接把 zip 里的
  `update-binary` 当文件 `exec`(signzip 路径支持 `UB_FILE` 变体)。因为内核是 armv7l
  `4.9.113` 而用户态是 Android,所以 aarch64 与 arm **两种静态 musl 都准备**。
  确认 **arm32 版能执行**(commit `774610b`)就等价于确认拿到了 recovery 里的
  `uid=0`。另外:update-binary 必须按 updater 协议输出(`ui_print`/`done`),并且要用
  `/system/bin/sh` 而不是硬编码 `/bin/sh`(commit `9986e0d`)。
* **recovery bind shell 的关键坑**:recovery 的 init 里 **eth0 是 DOWN 的**,没有
  ifup、没有 DHCP,任何反连/监听 payload 都白干。R5 载荷自己
  `ip link set eth0 up` + 静态 `192.0.2.126/24` + 路由,然后再开
  shell(commit `4c19d14`)——第一次 root shell 由此而来。
* **进 recovery 之后机器的 IP 一定会变(不是玄学,是"recovery 没有 WiFi")**。原厂
  Android 走的是 **WiFi**,地址 `192.0.2.125`(路由器 DHCP 给的),我们投毒的就是这条
  链路上的流量;而 **recovery 里根本没有 WiFi**——没有无线驱动、没有
  `wpa_supplicant`、没有任何配置,recovery 镜像里只有**有线 `eth0`**。所以机器一重启进
  recovery,WiFi 那个地址就没了,**唯一能连上它的方式是插网线**,用的是我们在载荷里写死的
  `192.0.2.126`(故意和 WiFi 的租约错开,免得搞混)。由此两条操作纪律:
  * **点"升级"之前先把网线插好。** 进了 recovery 又没插线,机器就彻底失联,只能拔电回到
    Android 重来。
  * 目标地址要预期会跳:在 Android 侧看 OTA 是 `.125`,进 recovery 之后是 `.126`。后来的
    Alpine 也沿用 `.126` 给 `eth0`;而 §6 里内置 WiFi 通了之后,`udhcpc` 又在 `wlan0` 上
    拿回了 **`.125`**——同一块无线芯片、同一个烧死在里面的 MAC
    (`02:aa:bb:cc:dd:01`),路由器认得它,把 Android WiFi 原来的租约又发了一遍。有线是
    另一个 MAC:`02:aa:bb:cc:dd:01`(来自 `backup/harvest/props.txt` 的 `ro.boot.mac`)。
* 顺手记一个浪费时间的坑:攻击机上的 `Ncat` 不接受老的 `-w 300` 写法,会静默杀掉
  重连循环,于是改成一次性批处理(`tools/boxsh.sh`)。
* **不动 bootloader 的持久化**:recovery 里确认了 SELinux 是 **permissive**、
  `/system` 等是 **无 dm-verity 的 ext4 且可写**、`/system/xbin/su` 是 **setuid 测试版
  su**;`prop.default`(= `/default.prop`)每次开机被 init 读取且优先级高于
  `build.prop`。于是追加
  `ro.secure=0 / ro.adb.secure=0 / service.adb.root=1 / service.adb.tcp.port=5555 /
  persist.sys.usb.config=adb`
  → 网络 ADB 直接 root,无 RSA 弹窗,恢复只需还原一个文件。

## 4. 把我们自己的 Linux 换进 recovery 分区

* Amlogic 的 env **不是标准 u-boot env**:magic `\xe6\x8a\x59\x42`、`k=v` 以 NUL 分隔、
  **无 CRC**,所以 `fw_printenv/fw_setenv` 根本读不了(用 `tools/mkenv.py`)。
* **坑**:手改后 dd 回去的 env 全部被拒,并且静默从工厂副本还原。
  **原因**:env 块头 4 字节是**数据哈希且不是 CRC32**(原生值 `9e99dec5`,sysfs 字节序
  显示为 `c5de999e`)。于是**把 env 当只读**。早期实验留下的 `upgrade_step=2` 是机器上
  唯一还没清掉的 env 写入(见 §9)。
* **可用触发器**:`storeboot` 失败会落到 `update`,其尾部 `recovery_from_flash`
  (p6 = `/dev/recovery`)由 **misc 分区的 boot-recovery 块**控制。但
  **只有 Android 自己写的 token 算数**,手写 `stage/misc.bin` 无效。所以用
  `adb reboot recovery` 进入我们刷在 p6 的镜像。
* `tools/mkboot.py` 能字节级重建 Android v1 boot.img(round-trip 已验证)。两个坑:
  原厂 dump 尾部有**垃圾数据**,mkboot 会拒收(要拿干净的前一版镜像做底);要保持
  厂商头一致,就把 `backup/recovery_backup.img` 的**头一页**(`page_size` 在偏移 36)
  覆盖到新 body 上,只改 `ramdisk_size`(偏移 16)。验证:与原厂只差 16–18 字节。
* 我们的 ramfs init(`tools/tvinit.c` → `tvrd/init`)踩到的坑与修法:

  | 现象 | 根因 | 修复 |
  |---|---|---|
  | `Attempted to kill init!` 内核 panic | Alpine 的 `/dev` 是**空的**,switch_root 后没有 `/dev/console` | switch_root **之前**先把 `devtmpfs/proc/sysfs` 挂进新 root |
  | U 盘根文件系统挂不上 | 该写法下 `vfat` **拒绝** `rw` data 字符串 | 改用 `MS_NOATIME`/正确标志 |
  | 死机后毫无线索 | 日志在网络阶段之后才打开 | 一开始就打开 `/mnt/usb/tvinit.log`,每次启动都留证据 |
  | **热重启**后 eth0 链路锁不上 | `dwmac` PHY 只有冷启动才 lock | `net_up()` 返回 lock 状态;失败则重写 recovery token 重启,并用 `/mnt/usb/phyfail` 计数封顶防死循环 |
  | 根文件系统挂载被烂 PHY 拖住 | 顺序问题 | `try_rootfs()` 放到救援 shell 之前,以链路 UP 为条件 |

## 5. 把 Linux 装进内置 eMMC,然后删掉 Android

* **Amlogic 的分区表是非标准的**:`mmcblk0`(8 GB `8GTF4R`)的 21 个分区
  `fdisk/blkid/lsblk` 都读不出名字;名字来自 DT 节点,规则是
  **minor N == mmcblk0pN**。关键分区:
  p3 `/dev/cache` 1.1 GB、p4 `/dev/env` 8 MB(**此后只读**)、p6 `/dev/recovery` 24 MB
  (我们的 `tvboot.img`)、p7 `misc` 256 KB(tvinit 每次开机清零)、p11 `/dev/boot`
  16 MB(**现在默认启动 Linux**)、p18 `/dev/system`、p21 `/dev/data` 3.6 GB
  (**Linux 根分区**);`boot0/boot1` **未动**。
* **递进式、每步可回退**:① U 盘(`tools/mkrootfs.sh` 造 Alpine armhf 镜像,U 盘永久
  保留作救援);② `dd` 到 p3 `/dev/cache`,不要 U 盘;③ 把同一镜像写到 p11
  `/dev/boot`——`storeboot` 是 `imgread kernel boot; bootm`,**这条路径没有 AVB 校验**;
  p11 万一坏了会继续 fall through 到 `update`→`recovery_from_flash`(p6,同一镜像),
  所以不可能锁死;④ 用户授权后删除 Android,把腾出的分区 `mkfs.ext4` 后按 UUID 挂
  `/home`、`/srv`、`/opt`,可用空间约 6 GB,`mount_emmc_root()` 顺序变为
  **data → cache → USB**,`switch_root` 前 `mount -a`。
* **专有坑**:
  - Android 会在每次开机把原厂 recovery 刷回 p6:`/system/etc/install-recovery.sh` +
    `/system/recovery-from-boot.p`,必须中和(前者改成 `exit 0` 桩,后者改名)。
  - `apk` 装不了任何东西:**没有 RTC 电池**,时钟每次回到 2020 → TLS
    `certificate verify failed`。修:rc.local 开机先 `date -u -s` 设一个合理下限,再后台
    试路由器 NTP(公网 UDP/123 可能被墙)。之后 `e2fsprogs-extra`(基础 Alpine 连
    `mke2fs` 都没有)、`dropbear`、`screen`、`iw`、`wpa_supplicant` 全通。
  - busybox 的 `cp -ax /` **并不能**按预期递归,必须逐个目录 `cp -a`。
  - **没有 scp**:Alpine 的 dropbear 不带 `sftp-server`
    (`sh: /usr/lib/ssh/sftp-server: not found`)。传文件用
    `ssh root@box 'cat > /path' < localfile` + sha256 校验。
  - **自杀式 shell 坑**:在同时包含 `/root/fbtest2` 的同一行 `sh -c` 里执行
    `pkill -f "[f]btest2"` 会把**自己的 ssh 会话**杀掉(无任何输出)。要用
    `kill $(pidof 名字)`。
  - 早期用明文 `busybox nc -lk -p 2323 -e /bin/sh` 当 root shell,后来换成
    **禁用口令登录的 dropbear**(`-s`),host key 在 rc.local 里缺失时自动生成,
    公钥放 `/root/.ssh/authorized_keys`,2323 端口关闭(commit `2f3f55f`)。

## 6. 内置 WiFi

前后六次会话,大部分早期结论都被推翻,列在这里免得重走。

| 曾经的结论 | 事实 |
|---|---|
| "Broadcom bcmdhd,内核已内建" | `grep -c ' dhd_' /proc/kallsyms = 0`。只有 Amlogic **glue** 的 `bcmdhd_init_wlan_mem`;cfg80211/mac80211 内建,但**没有任何网卡驱动**。 |
| "从别的 ROM 抓 dhd.ko" | 那些 `dhd.ko` 是 **aarch64 ELF**(`e_machine 0xb7`)、vermagic 3.14.29;32 位内核硬性拒绝,`--force` 也跨不了架构。 |
| "上主线就能同时修好 WiFi 和 HDMI" | 芯片**不是** Broadcom,主线根本没有对应驱动,主线两条都修不了。 |
| "UWE5621 ≈ Realtek RTL8821CS" | 芯片实测 `chipid 0x56630001`、`MARLIN3E_20A_W21.19.2`、`uwe5623_marlin3E_ott` → **UniWave UWE5623 = 紫光展锐 Marlin**;驱动是 `uwe5621_bsp_sdio.ko` + `sprdwl_ng.ko`。 |
| "vermagic 是 `4.9.113 … p2v8`" | 精确值是 **`4.9.y SMP preempt mod_unload modversions ARMv7`**。 |

芯片家族是靠**从活的 Android 系统里刮出来的 properties** 定性的
(`backup/harvest/props.txt`):`[vendor.bcm_wifi]: [uwe]`、
`[persist.vendor.bt_vendor]: [libbt-vendor_uwe_share.so]`、
`[ro.product.chiptype]: [AMLS905L3]`。

* **供电**:内核 `CONFIG_DEVMEM=n`、没有 `/proc/kcore`,静态逆向**不可能**,于是
  **从用户态暴力试 ioctl 号**(机器上现编 C:`wificmd.c`/`wififind.c`/`wifipoke.c`),
  还原出 magic `'m'`(0x6d)、`_IO` 无尺寸、nr 1..5;其中
  **`0x6d03 arg=1` = "Set sdio wifi power up!"** 有效(GPIO486 拉高 + sdio 重新初始化)。
  `GPIO486` 被 `aml_wifi` pinctrl 占用,**sysfs 导出会被拒绝**,只能走
  `wifi_power`(major 244)。
* **千万别**在运行时 `unbind/bind` SDIO host 去强行 rescan:BSP 的 `meson-aml-mmc`
  没有干净的移除路径,`meson_mmc_remove` 直接 **OOPS**。
* 上电后从 sysfs 读到的 SDIO ID **不可信**(节点是陈旧的,
  `vendor=0x0000 device=0x0000`),厂商驱动 `sdiohal` 走私有枚举,照样能跑。
* **vermagic 预言机(很实用)**:把一个模块 `.modinfo` 里的 `vermagic=` 二进制改成乱
  码再 insmod,加载器会把**它期望的字符串**原样打印出来——无需源码、无需 /dev/mem 就
  拿到了精确 vermagic。
* **CRC 兼容性被证明而非假设**:`CONFIG_MODVERSIONS=y` 且**没有**开
  `CONFIG_MODULE_FORCE_LOAD`,所以 CRC 不匹配是干净的 `-ENOEXEC` 拒绝("disagrees
  about version of symbol")而不是 oops——**白嫖别的 ROM 的 .ko 是安全实验**。接着用一
  个不相关的 Amlogic 4.9.y armv7 模块 `tb_detect.ko` **加载+卸载都干净**,证明邻域
  S905L3B ROM 与本机内核符号 CRC 一致:不用重编内核、不用打补丁。
* **找 .ko 的过程**:捐赠 ROM 不是 ext4/f2fs/erofs/squashfs,而是 Amlogic **USP** 镜像:
  分区由**扩展块(extent)**序列组成,每块 12 字节头
  `{magic=0x0000cac1, nblocks, 0xc+nblocks*4096}` + `nblocks*4096` 裸数据,ext2 风格的
  目录项在 DIR 扩展里。`tools/unpack_aml.py`、`karve2.py`、`carveko.py` 实现了这套遍历,
  从裸块里雕出完整的 `ET_REL/EM_ARM` 模块。踩过的细节:`e_type` 在偏移 16、`e_machine`
  在 18、ELF magic 是 **5 字节** `\x7fELF\x01`、雕出的范围必须包含 section header
  table、模块名在 `.gnu.linkonce.this_module` 的 `+12`。
* **最后只差一个 RF 校准文件**:两个模块都能加载、WCN 固件(直接**内嵌在 .ko 里**的
  `wcnmodem.bin.hex`,不需要 `/lib/firmware`)也下载成功、CP 起来并回了 AT,但:
  `wifi ini path = /vendor/etc/wifi/uwe5621/wifi_56630001_2ant.ini -> open error` →
  `LOAD_INI_DATA_FAILED` → `WIFI MAC can't be found wifimac.txt` →
  `failed to register netdev(-5)` → 没有 wlan0。
  解法:从捐赠 USP 镜像里刮出 10 个 `wifi_*.ini`(同 chipid 的 2ant/3ant 成对字节相同),
  放到 `/vendor/etc/wifi/uwe5621/`,并创建 `/data/misc/wifi/`。
* **每次开机只有一次机会**:`uwe5621_bsp_sdio` 的 init 注册 `sdiohal` 总线驱动,而
  exit **不注销**;任何 `rmmod` 之后再 insmod 都会
  `Error: Driver 'sdiohal' is already registered, aborting` → `-16` →
  `wait SDIO rescan card time out` → `chip power on fail`。
  **绝对不要 rmmod bsp**,bring-up 脚本必须先判断"wlan0 已存在就跳过 insmod"。
* **最终结果**:`/root/wificmd 0x00006d03 1` → `insmod uwe5621_bsp_sdio.ko` →
  `insmod sprdwl_ng.ko` → netdev 注册、MAC `02:aa:bb:cc:dd:01`、
  `iw dev wlan0 scan` 得到 2.4G+5G 共 15 个 BSS(RSSI -37..-81),关联 MyWiFi24
  成功,-34 dBm,**tx 173 MBit/s VHT-MCS 9 VHT-NSS 2**(2 流 11ac 真的在工作),
  `udhcpc` 在 `wlan0` 上拿到 192.0.2.125(就是当年 Android WiFi 那个租约,见第 3 节),`ping -I wlan0 223.5.5.5` 0% 丢包,1.4 MB 约 1 秒下完。
  剩下的 `TLV check failed: type=0, len=0`、`mixed HW and IP checksum settings`
  等都是无害告警。
  路由策略保持 **eth0 优先**,保证有线 SSH 这条命脉不断:
  `default via 192.0.2.1 dev eth0 src .126` + `default … dev wlan0 metric 308`。
* **wpa_supplicant 的坑(浪费了一整个周期)**:本机 v2.10 **不支持 `-f` 日志参数**,
  而且用法打印后**以 0 退出**,脚本看起来"成功"了其实进程没起。统一改用:
  `wpa_supplicant -B -Dnl80211 -iwlan0 -c… -P/run/wpa_supplicant.pid`。
  口令策略:配置里**只存 64 位十六进制 PMK**,口令通过 stdin 喂给 `wpa_passphrase`,
  过滤掉 `#psk=`,随即 `unset`,全仓库 grep 不到明文;没给口令的那个 SSID 干脆不写。

## 7. 显示(HDMI)之战

### 7.1 主线/Armbian:修好了 hang 仍然放弃

* 上游 `meson_dw_hdmi` 缺 GXLX2 glue,报
  `Unsupported HDMI controller (0d0d:0d:0d)`;2026-07-18 的
  *"drm/meson: add HDMI support for GXLX2"* 补丁集补上了,**ophub 6.18.y 已带**。
* 偶发 hang(ophub #3650)停在 `meson-drm d0100000.vpu: Queued 2 outputs on vpu`:
  GXLX2 的 HDMI 必须用 **`0xda800000` 直接映射**,但驱动的 `soc_major_id` 运行时探测不
  可靠,会退回 `0xc883a000` 间接窗口 → 硬件挂死。DTB 绕过(ophub commit `93c07a9`):
  `compatible = "amlogic,meson-g12a-dw-hdmi"` + `reg = <0 0xda800000 0 0x10000>`。
* 用**不碰 eMMC boot 链**的方式起 U 盘:`update`/`start_autoscript` 会从 USB 的 FAT 分区
  读 `s905_autoscript`。`tools/mkautoscript.py` 造 Amlogic autoscript(legacy uImage,
  `type=script`,**非标准 64 字节头:没有时间戳字段**;`0x27051956@0`、`hcrc@4`、
  `size@8`、`load@12`、`ep@16`、`dcrc@20` = payload 的 zlib crc32、
  `os/arch/type/comp@24-27 = 00 00 0e 00`、`name[32]@28`、`pad[4]@60 = 16 6b d9 86`、
  payload@64)。而且**这台机器的 u-boot 不校验 hcrc**(`--selftest` 已对已知能执行的样本
  文件逐字节验证)。于是一次重启里 `vout` 真的变了,而 `/dev/env` **一个字节都没写**,
  拔 U 盘即回 Alpine。
  **后来才发现的更正(§7.7):**"一个字节都没写"只对**我们自己那些 `dd`** 成立;任何
  执行了 `saveenv` 的 autoscript 都会把变量**永久锁进 env**——ophub 的
  `aml_autoscript` 会,我们自己的探测脚本 `tools/t1.txt` 也会。所以**现在 env 已经不是
  出厂状态**了,再引用"零写入"这个说法之前先读 §7.7.2。
* 另一个更干净的触发器:`tools/reboot_mode.c`
  (`reboot(MAGIC1, MAGIC2, LINUX_REBOOT_CMD_RESTART2, "update")`)直接锁住 Amlogic
  update 模式。
* **必须记住的去风险细节**:ophub 自带的 `s905_autoscript` 里有
  `if fatload usb 0 0x1000000 u-boot.ext; then go 0x1000000; fi`,也就是说**正常路径和
  失败路径都会去起主线**;之前所谓"插 U 盘就死"其实是它干的。要把 U 盘上所有 ophub
  脚本改名 `*.disabled`,否则你的实验结果全是噪声。
* 编了 `p291_headless.dtb`:**内存 2GB 改 1GB**(m302a 和 v2 都错填 2GB,板子只有
  1GB)+ `vpu`/`hdmi-tx` `status="disabled"`。结果主线**真的走到了 eth0 + DHCP .126**
  (说明 headless DTB 在内核层是有效的),但用户层起不来(没有 sshd、multi-user 处的
  bootlog 也没产生)。另外 `u-boot.ext` 选哪个 blob 差别很大:照
  `model_database` 挑的"标准"`u-boot-s905x-s912.bin` 让机器**彻底离线**,只有
  `u-boot-p212.bin`(606,670 B)配我们的 DTB 摸到了网络。**板子上的直接证据永远优先于
  型号数据库。**
* **最终放弃**:没图像、没 shell,而继续排障需要 USB-TTL 串口(手上没有)。之后把 env
  恢复成原生的 `bootcmd=run storeboot`(头 `9e99dec5` 已核对)——**但这次恢复后来又被
  某个 autoscript 里的 `saveenv` 冲掉了**,当前 env 实际长什么样见 §7.7.2。

### 7.2 原厂 BSP 内核:黑屏到底是怎么形成的

早期笔记里"`fb0` 是 OSD0、**没接到 HDMI**、所以无头是内核限制"这个结论是**错的**,
本节就是推翻它的过程。实测事实(`recon/hdmi_*.txt`、`tools/fb*.c`):

* **HDMI 发射端从头到尾都是好的**:`hdmi_init=1`、`ready=1`、`hdmi_used=1`、
  `hpd_state=1`、`cur_VIC: 16`(1080p60)、`video_clk 148500000`、`avmute 0`。也就是说
  屏上**一直有有效信号**,问题只在"哪一层、哪块内存"。
* **开机那个安卓 logo 不是 Linux 帧缓冲画的**:u-boot 把 logo 装到
  **`0x3d800000`**,cmdline 是 `logo=osd1,loaded,0x3d800000,…`;此外 DT 的 `meson-fb`
  还声明了 `logo_addr = <0x3f800000>`(位于 1GB 顶端的保留区
  `0x3f800000..0x40000000`),并且 `osd_logo_index = 1`。
* `fb0` = OSD0,`smem_start=0x2b400000`(`use_cma_first=1`),1920×1080,
  `line_length=7680`;`fb1` = OSD1,`smem_start=0x0`、32×32、`line_length=128`、
  `smem_len=1048576`——**真正接上面板的那一层,Linux 手里没有可用的显存**。
* **Z 序是第一道坑**:`fb0/order=0x2`、`fb1/order=0x0`,即 **osd1 盖在 osd0 上面**。
  logo 层开着且全屏(`window_axis 0 0 1919 1079`)时,你在 fb0 画什么都没意义。
* `CONFIG_FRAMEBUFFER_CONSOLE` **没开**,所以控制台永远不会出现;`CONFIG_DEVMEM=n`
  (`/dev/mem` 打不开,`mknod` 也没用)、没有 `/proc/kcore`,所以**无法**直接去读写
  `0x3d800000` 验证 logo 层理论。
* cmdline 写着 `ramoops.pstore_en=1`,但 **pstore 是空的**、也没有 ramoops 注册 ⇒
  **死机没有任何崩溃日志**。每次盲试的代价是一次拔电,而且除了"又是黑的"之外什么信息都
  拿不到。

### 7.3 让画面出来的三件事(缺一不可)

1. **干掉 logo 层**:`echo 1 > /sys/class/graphics/fb1/blank` ⇒ `osd[1] enable: 0`。
   判定它是遮挡者的证据:一切掉它之后 splash 立刻消失;而**任何对
   `/sys/class/display/mode` 的写都会把它重新打开**(`enable: 1`)——这正是之前实验
   "看起来时好时坏、又变回安卓开屏图"的原因。
2. **OSD 扫描地址是靠 ioctl 锁存的,不是靠写内存**:Amlogic VIU 只在用户态做 Android
   图形后端每帧都做的事之后才认这个缓冲:
   `ioctl(FBIOPUT_VSCREENINFO, FB_ACTIVATE_NOW|FB_ACTIVATE_FORCE)` 然后
   `ioctl(FBIOPAN_DISPLAY)`,`yoffset` 必须为 0。
   **`yoffset` 是隐形炸弹**:默认 `yv=2160`(双倍高),`yoffset=1080` 会让 `mmap` 直接
   `EINVAL`,或者把面板指到一个空页。`fbset -fb /dev/fb0 -g 1920 1080 1920 1080 32` 把
   它归一。
3. **缺的那一脚是 `osd_do_hwc`**。`/sys/class/graphics/fb0/osd_do_hwc` 是**只写节点**
   (`cat` 报 Permission denied——这是 0200 权限的正常表现,曾被误判为"不可用")。
   `echo 1 > .../osd_do_hwc` 补上硬件合成路径后,**fb0 的内容真的在面板上出现了**——
   用全屏颜色循环(蓝→绿→红→白→青→黑)在干净状态下复现确认。

配合的几何设置:`echo "osd0,0,0,1919,1079" > /sys/class/display/axis`(语法是
`osdN,x0,y0,x1,y1`;写错会得到 `pan_data(0,0,0,0)/dispdata(0,0,-1,-1)`,看起来像"这层没
编程")。

### 7.4 后来证明是干扰项的东西(记下来,别再来一遍)

* **`window_axis` 是缓存,不是测量值**:用户态没写之前就一直是 `0 0 0 0`,拿它当"平面
  没编程"的证据把实验带偏过一段时间。
* **`screen_real_width/height = 1440x810` 在各模式下恒定**,mode bounce 也不变,它描述
  的是视频层而不是 OSD 裁剪。注意:这里被否掉的只是"`screen_real` 这个数值来自 CVBS"
  这一条猜测;把 VIU 输出绑到 CVBS DAC 的 **`vout=…cvbs` 启动参数**那一关是真实存在的
  (见 §7.7.1),别把两者混为一谈。
* **`osd_blend_bypass` 曾被冤枉**:一次 `echo 1 > osd_blend_bypass` 之后机器掉线就归
  罪于它,但 pstore 为空,**因果并未证实**;而那次"插 U 盘就死"完全由 ophub 起主线解释
  (§7.1)。
* `cat /sys/class/graphics/fb0/osd_reg` 会**让 busybox 段错误**,别读。
* `display/axis` 以及各 `*_axis` 都是单槽:cat 只返回最后写入的那一行。
* `osd_logo_index`、`ver_clone`、`bist`、`free_scale*`、`scale_axis`、
  `osd_plane_alpha(0x100)`、`osd_deband(1)` 单独去戳都没让画面变化;唯一的例外是
  `echo 1 > /sys/class/graphics/fb0/ver_clone` 确实能把 `osd_clone` 切成 `ON`。
* 下次可以直接用的隐藏调试口:`echo info > /sys/class/graphics/fb0/debug`(把每个 OSD
  的 `pan_data`/`disp_data`/缩放窗打到 dmesg)、
  `echo 7 > /sys/class/graphics/fb0/log_level`(驱动会把每次 `osd_reg_read/write` 打出来)、
  `echo 1 > /sys/module/fb/parameters/dump_reg_trigger`。

### 7.5 Alpha:fb0 真的有 8 位 alpha,而 Xorg 不知道

`fb0` 是 **ARGB8888 且 `transp=24/8`**,硬件**真的按逐像素 alpha 混合**。Xorg 的 fbdev
驱动写的是 `0x00RRGGBB` ⇒ 内容**完全透明**。用分屏探针证明:左右两块 RGB 相同,只有
alpha 一个是 `0x00`、一个是 `0xff`,**只亮了一半**。
驱动**拒绝** `transp.length=0` 的 `FBIOPUT_VSCREENINFO`,所以走 **16 bpp RGB565**
(`fbset -fb /dev/fb0 -g 1920 1080 1920 1080 16` ⇒ `transp=0/0`、`line_length=3840`),
这样格式里压根没有 alpha 字段。

### 7.6 目前仍未解决的问题

* **已在 2026-09-20 的后续 session 解决——见 §7.7.7。** 纵向亮度渐变的真凶是
  OSD **自由缩放(free-scale)单元带着"全零源窗"在跑**(开机默认
  `free_scale_axis = 0 0 0 0`,而 `free_scale_enable=0x10001`);
  `echo "0 0 1919 1079" > /sys/class/graphics/fb0/free_scale_axis` 即修复。
  剩下的"颜色不对/黑屏"读数全部是我们**探针自己的 bug**(`fbrows` 红蓝通道
  写反、`fbfill` 32bpp 掩码顺序错误),硬件从头到尾是好的。Xorg 16 bpp 已
  **肉眼确认在面板上可见**(task #16、#40 关闭),整套流程已由
  `/usr/local/bin/display-up.sh` 固化进 `rc.local`(task #39 关闭,还差一次
  真实断电重启的验证)。
* 限制"纯用户态能修到哪"的内核事实:无 `fbcon`、无 `/dev/mem`、无 `/proc/kcore`、
  `/lib/modules/4.9.113` 是空的。

### 7.7 交接:显示问题现在到底走到哪一步(2026-09-20 现场复核)

下面每一条都是上个 session 结束时**从真机 SSH 只读复核**过的,新 session 不用再翻考古层。

#### 7.7.1 第四道闸:bootargs 里的 `vout`,不在用户态

最后找到的根因(commit `81e1200`)在 §7.3 那三条**之上**:机器曾经以
`vout=576cvbs,enable` 开机,VIU 的**输出通路**在 init 阶段就被绑到 CVBS DAC,于是
**osd0 永远到不了 HDMI 编码器**,用户态怎么折腾都没用——特征是 `osd[0] enable: 1`
但没有几何、`osd_fps=0`,而 hdmitx 完全正常(`hdmi_init=1`、`ready=1`、
`cur_VIC: 16`、`video_clk=148500000`)。这串参数是 `storeargs` 用
`vout=${outputmode},enable` 拼出来的,所以要在 `storeboot` **之前**用 U 盘
autoscript 改 `outputmode`:

```
# stage/hdmi_autoscript.cmd -> tools/mkautoscript.py -> FAT32 上的 s905_autoscript
setenv outputmode 1080p60hz
setenv hdmimode 1080p60hz
run storeboot
```

运行时的等效手段(往 vout 命令口推 `vout,hdmi` / `switch,hdmi`)在 dmesg 里留下:
`vout: osd0=> x:0,y:0,w:1919,h:1079`、`osd1=> x:0,y:0,w:0,h:0`、
`fb: current vmode=1080p60hz`。

#### 7.7.2 session 结束时的现场状态(只读复核)

* **机器之后被断电重启过,所以现在画面是没起来的**——§7.3 那套**每次开机都要重做**,
  什么都没固化。此刻显示器上应该还是原厂安卓 splash。
* 当前 bootargs:`logo=osd1,loaded,0x3d800000,720p50hz vout=720p50hz,enable
  hdmimode=720p50hz cvbsmode=576cvbs` ⇒ **§7.7.1 那道 CVBS 闸已经没了**(开机输出就是
  HDMI)。**别再为它花一次断电。**
* `/dev/env` 只读导出:`bootcmd=run start_autoscript; run storeboot`、
  `outputmode=720p50hz`、`hdmimode=720p50hz`、`cvbsmode=576cvbs`、
  `display_layer=osd1`、`fb_addr=0x3d800000`。**env 已经不是出厂值**(旧笔记里"已恢复成
  原生 `run storeboot`"那句作废):某个跑了 `saveenv` 的 autoscript 把 `bootcmd` 和
  `outputmode` 永久写进去了。实际影响——自恢复不变式变成**拔着 U 盘断电 ⇒ 进 Alpine**;
  而 U 盘插在机器上时**每次开机都会被读**。
* 运行时 `/sys/class/display/mode = 1080p60hz`,bootargs 却是 `720p50hz`
  ⇒ **开机几何与运行时几何现在不一致**,这是纵向渐变的首要嫌疑(§7.7.4)。
* `fb0`:`bits_per_pixel=32`(16 bpp 那次实验**没熬过重启**)、`order:[0x2]`;
  `fb1`:`order:[0x0]`(logo 层在上,现在没被 blank);
  `/sys/module/fb/parameters/osd_logo_index = 1`;`fb0/blank`、`fb1/blank` 读出来是**空的**
  ——正常,它们是只写节点。
* `/dev/fb0` 上值得用的只写节点(`--w--w----`):`osd_do_hwc`、`osd_clear`、
  `osd_single_step`、`osd_single_step_mode`、`ver_update_pan`、`free_scale_switch`。
* 所有探针的**二进制和源码都还在机器 `/root`**(`fbcycle fbfill fbgrid fbhemi fbsplit
  fbfmt fbrows fbinfo fbpan fb1q fbtest2 wificmd reboot_mode`),X 配置在
  `/etc/X11/xorg.conf.d/90-amlfb.conf`,117 GB U 盘仍插在机器上
  (`/dev/sda{,1,2}`,未挂载)。
* busybox 的坑(不知道就白花时间):`fbset -i` **不存在**(用
  `cat /sys/class/graphics/fb0/bits_per_pixel`,或 ioctl 版的 `fbinfo`;
  `smem_start`/`line_length`/`xoffset`/`yoffset` **都不是** sysfs 文件);
  `ip -br addr` 什么都不输出(用 `ifconfig -a`);没有 `seq`;部分 sysfs 读出来是
  "Binary output"(过一遍 `tr -cd "\11\12\40-\176"`);在**同一行**里
  `pkill -f <pattern>` 会把自己的 ssh 会话杀掉(用 `kill $(pidof 名字)`)。
* **WiFi 开机自启这次已确认通过**(原本是悬着的一项):`/var/log/wifi-up.log` 结尾是
  `INTERNET via wlan0: OK`,`wlan0 = 192.0.2.125`(`02:aa:bb:cc:dd:01`)、
  `eth0 = 192.0.2.126`(`02:aa:bb:cc:dd:01`)——正是 §3.5 说的"同一块无线芯片、同一个
  MAC,路由器把老租约又发了一遍"。

#### 7.7.3 探针 → 画了什么 → 眼睛看到什么

显示器是这里**唯一**的传感器(没有 `/dev/mem`、没有 pstore)。按真实顺序列出;标了
*无读数* 的行是"画了但没人看过",**不能当证据**。

| # | 探针 | 内容 | 读数(原话) | 说明 |
|---|---|---|---|---|
| 1 | `fbcycle` v1(axis + FORCE-put + pan,**没** blank fb1) | 全屏纯色循环 | "black" / "old splash picture" | logo 盖着时 put+pan 毫无意义 |
| 2 | `fbcycle` v2(每帧重新驱动 canvas) | 纯色循环 | "1 screen flash, still old splash pic" | 锁存发生了,但 logo 又赢回去 |
| 3 | 读 `osd_status`/dmesg | — | "only black" | — |
| 4 | `fbset -fb /dev/fb1 -g …` + `fb1q`(去摸 fb1 显存) | — | "old splash picture" | fb1 对我们没有可用显存(也制造了 §7.7.5 的 abort) |
| 5 | `echo 0 > ver_clone` + **`fb1/blank=1`** + `fbcycle` | 纯色循环 | "i see green to red to gray( all are gradient)" | **第一次真的看到内容**——注意括号里那句"全都是渐变" |
| 6 | `fbcycle /dev/fb0 3000 1` | 6 个纯色,每个 3 秒 | "yes, i saw all the colors and now is black" | 配方端到端成立(最后停黑是设计) |
| 7 | `fbsplit` | 左 alpha `0x00`、右 `0xff`,RGB 相同 | "only 1, it's a dark blue -- blue -- dark blue gradient from top to bottom" | 逐像素 alpha 被硬件认,**同时纯色也纵向渐变** |
| 8 | `fbfmt`(`transp.length=0`) | 同 7 | "like the blue one, dark -- normal --dark from top to bottom" | 驱动拒绝 XRGB,所以没变化 |
| 9 | `fbset … 16` + `fbfill ff0000` / `00ff00` | 16 bpp 纯色 | "bright red on top then gradient to black on bottom" / "bright green on top to black on bottom" | 与位深无关:依旧纵向衰减 |
| 10 | `fbgrid 6 4` | 24 个带标签色块 | "bright red on top then gradient to black on bottom"(两次) | 网格完全没被解析成块,和纯色同一个症状 |
| 11 | `fbhemi bottom` | 只画下半屏 | *无读数*(只有 dmesg 里 `osd[1] enable: 0`) | 未验证——最值得先跑,它能区分"跟内容"还是"跟位置" |
| 12 | `fbrows 8` | 8 条横向色带 | *无读数* | §7.6 要的纵向范围测量**至今没做** |

#### 7.7.4 纵向渐变的假设,以及各自的"一测就死"实验

1. **开机/运行时几何不一致**(bootargs `vout=720p50hz`,运行时
   `display/mode=1080p60hz`)。最便宜、不用重启:先 `fbrows 8` 取读数;然后
   `echo 720p50hz > /sys/class/display/mode`(**写完必须重新 blank fb1——任何 mode 写入
   都会把 logo 层打开**),按 1280×720 重画再取读数。如果渐变随模式缩放,就是它,永久解法
   是 §7.7.1 的 autoscript,让两者从开机就一致。
2. **VIU dump 里 canvas/frame 不匹配**。实测:`canvas.addr=0x2b400000`、
   `canvas.width=7680`、`canvas.height=3240`、`frame.width=1920`、
   `frame.height=1080`、`out_addr_id=0x0`——canvas 高度是 frame 的 **3 倍**。实验:把
   `yres_virtual` 压回去(`fbset -fb /dev/fb0 -g 1920 1080 1920 1080 32`)再读一次 dump;
   如果 `canvas.height` 变成 1080 且渐变随之改变,那驱动默认的双/三倍高虚拟屏就是病根。
3. **`osd_fps=0` / `osd_hold_line=0x0`** —— OSD RDMA 其实没在刷帧:内容只被锁存一次、
   行计数器停着。实验:在 `fbcycle` **正在动**的时候读
   `/sys/class/graphics/fb0/osd_fps` 与 `osd_hold_line`,和静止时对比;如果画面可见但
   fps 仍是 0,说明喂给面板的是我们还没建模的另一条通路,该动的是 `osd_single_step` /
   `ver_update_pan`。
4. **电视自己的后处理** —— 基本排除:纯色场不受它影响,而纯色一样渐变。

#### 7.7.5 会让人拔电的操作(动手前把这条摊在边上)

* `echo 1 > /sys/class/graphics/fb1/osd_clear`,和/或 `mmap` 越过 fb1 那 1 MiB
  ⇒ 内核挂死,只能断电(nc 后门早就没有了)。
* 写**映射后的 fb1** ⇒ 反复 `Unhandled fault: imprecise external abort (0x1c06)`,
  连 SSH 都被带走一小会儿。fb1 对我们只读。
* `cat /sys/class/graphics/fb0/osd_reg` ⇒ **busybox 段错误**。
* 运行时 `unbind`/`bind` `d0070000.sdio` ⇒ `meson_mmc_remove` **OOPS**(WiFi 阶段的老教训)。
* 任何对 `/sys/class/display/mode` 的写入都会**悄悄把 logo 层重新打开**——看着像配方退化,
  其实只是第 1 步被撤销。
* 触发重启需要用户明确同意(长期规矩):每次盲启的代价是一次物理拔电,而 pstore 是空的,
  什么日志都拿不到。

#### 7.7.6 下一个 session 的接续顺序

1. `ssh root@192.0.2.126`,一把应用 §7.3(blank fb1 → axis → `fbhemi bottom` →
   每帧 FORCE-put + pan + `osd_do_hwc`),**把 #11/#12 那两次缺失的读数补上**。
2. 跑假设 1(几何一致性)——免费、不用重启。
3. 跑假设 2(canvas 高度)——同样免费。
4. 两条都不成立,再做 `stage/hdmi_autoscript.cmd` → `s905_autoscript`
   (`tools/mkautoscript.py`)放 FAT32 U 盘,让开机就是 `1080p60hz` 的 `vout`;记住
   `saveenv` 的副作用,做完把 U 盘拔掉回 Alpine。
5. 几何对了之后(而且只在这之后):`fbset … 16` + 16 bpp 起 Xorg(task #40、#16),再把
   整条序列固化进 `/etc/rc.local`,**不写 env**(task #39),并保持 dropbear 先起、显示
   钩子后起——显示弄坏绝不能赔上整台机器。

#### 7.7.7 已解决 —— 第五道闸:退化的 free-scale 源窗(后续 session,现场验证)

本次 session 一开场就补上了 #12 读数(第一次 `fbrows 8`,未做任何诊断前):
**"从上到下是一条白→黑渐变"**,条带完全不解析,与 §7.7.3 第 10 行完全一致;
#11(`fbhemi`)后来证明不需要。

致命线索来自 OSD0 的 debug dump:

```
free-scale enable.h:1 .v:1          ← 垂直缩放是开着的
free-scale src data: 0 0 0 0        ← …但源窗是"零面积"的退化矩形
free-scale dst data: 0..1919, 0..1079  ← …输出却是全屏
```

且 `cat /sys/class/graphics/fb0/free_scale_axis` = `0 0 0 0`(从来没人编程过)。
一个源窗为空的垂直缩放器,会把**扫描通路出来的任何内容**乘上一条"顶部亮→底部暗"
的亮度斜坡——色相保留、只跟屏幕纵坐标有关、与画什么无关。这就是"所有图案都渐变"
的全部真相。修复:

```
echo "0 0 1919 1079" > /sys/class/graphics/fb0/free_scale_axis
```

再锁存(FORCE-put + pan + `osd_do_hwc`),8 条色带**立刻逐条分明**。这是继 §7.3
三条、§7.7.1 `vout` 之后的**第五道闸**;它不写就每次开机(以及每次 mode 写入)都会
回到坏状态。

§7.7.4 的假设 1、2 **作废**:斜坡消失时,开机 `720p50hz` 与运行时 `1080p60hz` 仍然
不一致,`yres_virtual` 也早已是 1080。全程没重启、没动 autoscript、没写 env。

**两个探针自己的 bug 让硬件背了更久的锅**(均已修复,`tools/` 与机器 `/root`
双向同步、sha256 核对、机器上重编):

* `fbrows.c` 把颜色的**蓝字节写进了 red.offset**(红写进 blue.offset)。修好斜坡后
  看到的"条带顺序不对"——Y↔C 互换、R↔B 互换、白/绿/品红不变、棕→暗蓝——正是
  R/B 互换的标准签名;电视和 VIU 都是无辜的。重编后条带**顺序正确、纵向亮度均匀**。
* `fbfill.c` 的 `ch()` 在移位**之后**才做掩码(`(v << offset) & ((1<<len)-1)`),
  于是 32 bpp 下任何纯色都被写成 `0xff000000`——不透明的**黑色**。这就是上一轮
  "红色填充变黑屏"的原因;更早那次红/绿读数走的是硬编码的 16 bpp 路径,所以是对的。
  修复后 `pix=0xffff0000`,肉眼确认**全屏均匀亮红**——纯色上的斜坡也彻底消失。

**Xorg 已确认可见**(task #16/#40):`fbset … 16`(`transp=0/0`,§7.5 的 alpha 坑
在结构上不可能再踩),Xorg 21.1.14 fbdev depth 16 + openbox——面板上是**均匀的
灰色桌面**。注意:显示器是一台 **4K 电视**(机主本次告知);目前无关,因为盒子输出
1080p60(VIC 16),修复后所有图案读数都正常。一个配置坑:这块板子没有键盘,X
会以 **ServerAbortFatal(no core pointer)** 退出——在 `90-amlfb.conf` 里加哑
`kbd` + `vmmouse` InputDevice(并在 ServerLayout 里引用)即可启动。

**持久化(task #39)**:`/usr/local/bin/display-up.sh`(源码存在
`stage/display-up.sh`)按本次验证过的唯一顺序跑完整配方——`fbset 16` → 写 mode →
**重新** blank fb1 → `ver_clone 0` → axis → **free_scale_axis** → **window_axis**
→ 用 `fbfill` 做 FORCE-put/pan 锁存 → `osd_do_hwc` → 起 Xorg+openbox →
**`fblatch` + `osd_do_hwc`**,日志在 `/var/log/display-up.log`;挂钩在
`/etc/rc.local` 的**最后**(dropbear 和 wifi-up 之后),显示钩子弄坏也丢不掉 SSH。

**真实重启测出来的"第六道闸"**:重启后脚本自身看起来一切正常(X 在、日志干净、
"灰桌面"),但 **X 的内容根本不上屏**——`xsetroot` 变色和黄色 `xmessage` 窗口全都
看不见。这轮排障又钉死两条事实:

* X 自己的 mode-set **不带 FORCE**,X 接管 fb0 后 VIU 一直扫旧的锁存帧;而一次
  用坏 `fbpan` 造成的失败 `FBIOPUT`(`put=-1`)能把平面拖进"连普通配方都不显示"
  的状态。**实测能救回来的顺序**:完整配方**必须含 `1080p60hz` mode 写入和显式
  `echo "0 0 1919 1079" > window_axis`**,然后再画。两步都已并进脚本。
* 让 X"活起来"的补丁是 **`tools/fblatch.c`** —— 只做
  `FBIOPUT_VSCREENINFO(NOW|FORCE, yoffset=0)` + `FBIOPAN_DISPLAY`、**不画任何
  像素**,在 Xorg 起来后执行(且随时可重放:改完 X 画面就
  `/root/fblatch; echo 1 > osd_do_hwc`)。最终肉眼验证(4K 电视):全屏纯色红
  **均匀**;跑完整脚本 → 根窗口涂**蓝**、上面浮**黄色 "HELLO" 窗口**,**全部
  可见**。
* 紧接着又修正了一点:`fblatch` 只需要在**所有权交接的那一刻**执行一次(脚本
  末尾本来就带),之后**不需要每次画面变化都重放**——橙色 `xmessage` 弹出来时
  没有做任何 latch,照样上屏。另外**光标在任何 latch 之前就能看见**,因为
  Amlogic 把鼠标光标走独立的硬件 cursor 平面——这也正是"只有光标、没有桌面"
  那种读数的来源。

**桌面栈——现在是 XFCE4(取代了早期的 openbox+tint2 轻量版)**:第一版可用桌面是
`openbox-session` 下的 `tint2`+`pcmanfm --desktop`+`lxterminal`,它留下的关键教训
至今有效:**裸 `openbox` 不会执行 `~/.config/openbox/autostart`,只有
`openbox-session` 会**(这就是"只有光标"变成"有桌面"的那一步)。等 XFCE4.18
(`xfce4` 元包,armhf,装完共 466 MiB/350 包;整套 session 在 989 MB 内存的机器上
只占 **199 MB RSS**)经肉眼确认"完美"后,就成了默认 session:`dbus-launch
startxfce4`(`xfdesktop` 原生画 `~/Desktop` 图标,`thunar`、`xfce4-panel`、
`xfce4-terminal`)。已冗余的 `tint2`/`pcmanfm`/`lxterminal`(连带 `vte3`、`libfm`、
`menu-cache`)用 `apk del` 清掉。`xfwm4` 的**合成器是关的**(预置 `xfwm4.xml` 里
`use_compositing=false`)——这条 fbdev/16bpp 路线没有 GLX(缺 `swrast_dri.so`),
开合成只会白耗内存。openbox 保留为一行可切的后备。`~/Desktop` 上有四个图标:Terminal
(`xfce4-terminal`)、Files(`thunar`)、以及下面两个电源快捷方式。

**没有 init 系统的电源方案(一个值得记录的死胡同)。** 机器上**没有 logind /
ConsoleKit / seatd**,所以 XFCE 自带退出对话框里的重启/关机按钮是**死的**(已确认:
`xfce4-session` 代码里确实引用了 `org.freedesktop.login1` 和 `…ConsoleKit`,但整机
**根本没有 D-Bus 系统总线**——只有 `dbus-launch` 起的那条 session 总线)。想当然的
修法 `apk add seatd` 在这里是**用错的**:seatd 提供的是 `libseat`(给 X/Wayland 做
设备访问),**不是**对话框要的 `login1` D-Bus 接口,装了也点不亮任何东西。真正的原生
方案是 **elogind + dbus 系统总线**,但在这台裸 busybox init 的机器上,那意味着往
"拔电必须能自愈"的开机链里再塞两个守护进程——机主选择了**零风险**。于是 `seatd` 被
`apk del` 掉,电源改用两个带确认的桌面图标:`~/Desktop/Reboot.desktop` →
`/usr/local/bin/reboot-prompt.sh`、`ShutDown.desktop` → `shutdown-prompt.sh`(各自
弹一个 **默认按钮是 Cancel** 的 `xmessage`,确认后才执行 `/sbin/reboot` /
`/sbin/poweroff`)。看着原生、且不给开机链增加任何新面积。

**冷启动实测(2026-09-21)又挖出三件事,现在全部修进开机脚本:**

* **热重启桌面正常、冷启动却没有桌面。** rc.local 环境里**没有 `HOME`**,
  `openbox-autostart` 于是去找 `/.config/openbox/autostart`,找不到就静默跳过
  整个桌面。修法:用 `HOME=/root DISPLAY=:0 openbox-session` 启动。
* **X 里完全看不见 USB 鼠标。** 这台机器上**根本没有 udev 守护进程**(busybox
  init,连 `/etc/init.d` 都没有),而 Xorg 21 没有 hotplug 就一个输入设备都不
  加。想靠静态配置绕开也是死路:hotplug 开着时 X 会**直接禁用
  `kbd`/`vmmouse`/`mouse` 静态段**;而 `evdev` 打不开 `/dev/input/mice`(那是
  psaux 复用器、不是 event 接口,报 `Unable to query fd: Not a tty`)。正确修
  法:`apk add eudev`,在 Xorg 启动**之前**跑 `udevd --daemon` +
  `udevadm trigger`——X 随即通过 libinput 挂上了鼠标,还顺手自己发现了前面板
  的 `aml_keypad`。本节早先的哑 kbd/vmmouse 方案就此**退役**,
  `90-amlfb.conf` 恢复成纯 fbdev。
* **光标在、但冻住不动——这是硬件层面的 USB 故障。** `dmesg` 里鼠标**每 ~2 秒**
  断开再枚举一次(`USB disconnect … new low-speed USB device …` 无限循环):
  这个内核的 `usbcore` 默认 `autosuspend=2`(秒),xhci 把挂起的低速设备直接
  断电摘线。修法:`echo -1 > /sys/module/usbcore/parameters/autosuspend`——
  抖动立刻停止,重新插拔也不复发。udevd 拉起和 autosuspend 关闭现在都是
  `display-up.sh` 的收尾步骤,整条输入链路重新做到开机全自动。

**第七道闸:TMDS 时钟,而不是配方(2026-09-21 实测)**

那次说要补的**冷拔电重启终于做了,而且修好的链路是通的**:`display-up.sh` 开机
干净跑完(dmesg 里有 `vout: new mode 1080p60hz set ok`、
`fb: osd_update_disp_axis_hw:dispdata(0,0,1919,1079)`、
`fb: osd[1] enable: 0 (display-up.sh)`),Xorg/xfwm4/xfdesktop/xfce4-panel/udevd
全在,两个 axis 也都还是编程好的,bpp 16。**可显示器仍然报"无信号"**——六道闸全部
满足却没有画面,这迫使排查下沉到此前谁都没看的一层。

根因:**这条链路锁不住 1080p60(以及 1080p50)所需的 148.5 MHz TMDS 时钟。**
实测数据,不是推测:

| 设置的 mode | `tmds_clk`(dmesg) | 采样期内 HPD 掉几次 |
|---|---|---|
| `1080p60hz` | 148500 | **每 ~8.16 秒一次**,持续 |
| `720p50hz` | 74250 | 0 / 25 秒 |
| `1080p30hz` | 74250 | 0 / 12 秒 |
| `1080p25hz` | 74250 | 0 / 12 秒 |
| `1080i50hz` | 74250 | 0 / 12 秒 |

为什么这么难看出来:**链路已经在失败了,发送端的健康检查却全部通过**——
`hdmi_init=1`、掉之前 `hpd_state=1`、`config → cur_VIC: 16`、`tmds_clk 148500`、
`avmute 0`、`vid_mute 0`、`edid_parsing ok`。真相是接收端**锁不住数据**,把 HPD
拉低约 1 秒;驱动把这当成一次货真价实的拔线,于是重跑 EDID + mode-set——
**而画面就是在这一步被毁掉的**:mode 写入会重新使能 fb1 logo 平面、并丢弃
FORCE-put/pan 的锁存。于是 §7.3 的配方每 8 秒被悄悄抹掉一次,`dmesg` 里除了
`plugout`/`plugin` 什么都不留。

真正能区分的诊断手段(路径是 `/sys/class/amhdmitx/amhdmitx0/`——类名是
`amhdmitx`,所以 `cat /sys/class/amhdmitx0/hpd_state` 在这个 build 上根本就是错路
径):

* 每秒采一次 `hpd_state`、采 ~20 秒,**数里面有几个 0**。间隔恒定(这里是 8.16 秒)
  说明是协商重试;接触不良是不规则的;
* `preferred_mode`(这台电视的 EDID 答的是 `720p50hz`)、`sink_type`、
  `edid_parsing`、`config` 里的 `cur_VIC`,以及 `dmesg | grep tmds_clk` 看实际时钟;
* **`fake_plug=1` 看着像元凶,其实不是**——清掉之后抖动速率分毫不差,所以这个调试
  节点的读数不能当诊断结论;
* 这个 BSP 上没有 `hpd_state_check`、`5V_state`;`phy`、`hdmi_config_info`、`swap`
  读出来是空的。

结论:**改用 `1080p30hz`**——1920×1080 逐行、74.25 MHz,是这条链路能锁住的频段里
画质最高的一档。**这是权宜之计,不是给这台机器定罪**:§7.7.7 里同一台 4K 电视在
1080p60 下每种颜色图案都读对过,说明 148.5 MHz 在这台机器上*曾经*是好的;之后再换
一根线或换一个电视输入口复测 1080p60,再谈是否怀疑 HDMI PHY。

`stage/display-up.sh`(已部署到 `/usr/local/bin/`)的两处改动:

1. `MODE`/`W`/`H` 提到文件头做变量,`display/axis`、`free_scale_axis`、
   `window_axis` 全部由 `W-1`/`H-1` 推出来,帧缓冲几何和 OSD 几何**再也不可能对不上**
   ——写死的旧版里 §7.7.4 的假设 1 其实还活着。
2. **拆旧会话的步骤移到了脚本最前面。** 活着的 Xorg 拥有 fb0,会用它自己的 `var`
   覆盖我们的 `fbset`:第一版只在启动 Xorg 前拆会话,结果就是
   `fb0 xres=1920 yres=1080` 对着按 `1279x719` 编程的 axis。所以
   (`kill $(pidof xfce4-session)`、`kill $(pidof Xorg)`——绝不能用 `pkill -f`,
   见 §5.3)必须排在**任何** fb / mode 写入之前;顺带也让脚本可以在 SSH 里安全重放、
   完全不必重启。

当前状态:X + **XFCE4** 桌面(panel + xfdesktop 图标 + thunar + xfce4-terminal)以
16 bpp 活在 fb0 上,**`1080p30hz` / 1920×1080**,由持久化脚本拉起;肉眼确认
("looks perfect"),`hpd_low=0/25`。此前挂着的**再冷拔电一次已经做完并通过**——三个
输入链修复(`HOME=/root`、Xorg 前起 eudev、`autosuspend=-1`)开机读数全部正确——唯一
的新故障就是上面那道时钟闸。热重启与冷重启都全自动跑通,有线/无线照常起来。

## 8. 复现命令速查

```bash
# --- 网络中间人(攻击机 192.0.2.220) ---
sudo bash ssrf/ota_poison.sh 20          # ARP+NDP 欺骗、DNS 投毒、:53/:80 重定向
python3 ssrf/ota_server.py 80            # BestV OttService JSON + Range 支持
# 升级包:ssrf/signzip.py + tools/mk_pk7.py,用 keys/ 里的 AOSP testkey
#   -> ELF update-binary + META-INF/com/android/metadata + 无 authAttrs 的 PKCS#7 注释
#   (Offer 的 LastedVersion 3.0.2.0 必须高于客户端自报的 3.0.1.4;
#    FileHash = 大写 MD5,SoftSize = 精确字节数)

# --- 触发:不按任何键、不用 adb,机器自己进 recovery ---
# 机器查 UpgradeOSV2 -> 弹出"发现新版本" -> 下载 /pkg/update.zip(Range/206)
#   -> 客户端自己往 misc(mmcblk0p7)写 BCB "boot-recovery" + --update_package=...
#   -> 重启 -> 原厂 recovery 升级界面 -> 验 testkey 通过 -> exec 我们的
#      update-binary(uid=0)
# recovery 镜像里没有 WiFi:点『升级』之前先把网线插好,否则只能拔电回 Android。

nc 192.0.2.126 <port>                 # recovery 里的 uid=0 / u:r:recovery:s0
adb connect 192.0.2.126:5555          # 改了 prop.default 之后

python3 tools/mkboot.py stage/tvboot.img tvrd stage/tvboot.new.img   # 再覆盖原厂头
dd if=tvboot.img of=/dev/recovery bs=1M conv=fsync        # p6
dd if=tvboot.img of=/dev/boot     bs=1M conv=fsync        # p11 = 默认启动
adb shell su 0 reboot recovery           # 只有 Android 写的 misc token 有效

ssh root@192.0.2.126                  # dropbear,仅密钥登录
/usr/local/bin/wifi-up.sh                # 每次开机只能跑一次(sdiohal 限制)

# --- 显示:§7.7.7 起已持久化,rc.local 每次开机自动跑 display-up.sh ---
# 手动重做(顺序与脚本一致):
# 第 0 步——先拆旧会话:活着的 Xorg 拥有 fb0,会用它自己的 var 覆盖我们的 fbset。
#   kill $(pidof xfce4-session) $(pidof Xorg)      # 绝不要用 pkill -f(§5.3)
# 第七道闸(§7.7.8)——mode 必须是 1080p30hz:1080p60hz/1080p50hz 要 148.5MHz,
#   这台电视锁不住,每 ~8 秒掉一次 HPD,而每次重连都会重新打开 logo 层并丢掉锁存。
#   echo 1080p30hz > /sys/class/display/mode       # 之后第 1 步要重做
#   判断有没有锁住:每秒采一次 /sys/class/amhdmitx/amhdmitx0/hpd_state,数 0 的个数
echo 1 > /sys/class/graphics/fb1/blank             # 1. 关 logo 层。任何对
echo 0 > /sys/class/graphics/fb0/ver_clone        #    display/mode 的写都会把它
fbset -fb /dev/fb0 -g 1920 1080 1920 1080 16      #    重新打开,第 1 步要重做。
echo "osd0,0,0,1919,1079" > /sys/class/display/axis   # 2. 几何(osdN,x0,y0,x1,y1)
echo "0 0 1919 1079" > /sys/class/graphics/fb0/free_scale_axis
                                              # 3. 第五道闸(§7.7.7):开机默认是
                                              #    0 0 0 0,垂直缩放器会给所有内容
                                              #    乘一条纵向亮度斜坡
echo "0 0 1919 1079" > /sys/class/graphics/fb0/window_axis
                                              #    (实测恢复步骤的一部分)
/root/fbfill /dev/fb0 ff0000 5               # 4. 画+FORCE 重设+pan 锁存
echo 1 > /sys/class/graphics/fb0/osd_do_hwc        # 5. 踢一脚硬件合成
# 第六道闸(§7.7.7):X 的 mode-set 不带 FORCE——display-up.sh 末尾用
# /root/fblatch 在交接时锁存一次即可,之后 X 的实时更新直接上屏,
# 不用每次重放(鼠标光标走硬件 cursor 平面,随时都看得见)。
# 输入链(冷启动那轮,§7.7.7):启动 session 要带 HOME=/root(默认 session 现已
# 换成 XFCE:HOME=/root DISPLAY=:0 dbus-launch startxfce4);
# Xorg 启动前先 udevd --daemon + udevadm trigger;再
# echo -1 > /sys/module/usbcore/parameters/autosuspend(治好鼠标 2 秒一抖)。
# 永远别做:cat fb0/osd_reg(busybox 段错误)、mmap/写 fb1、fb1/osd_clear、
#   unbind d0070000.sdio —— 每一个都真实烧掉一次拔电。
```

回退到"纯原厂":p6 刷回 `recovery_backup.img`、清空 `/dev/cache`、恢复
`install-recovery.sh`,然后需要厂商烧录包重刷(Android 载荷是**有意**删掉的,没有保留
完整的 system/vendor 备份)。

## 9. 收尾清单

1. **HDMI:已解决**(§7.7.7)。纵向渐变 = OSD free-scale 单元带着开机默认的
   **全零源窗**在跑(第五道闸,`echo "0 0 1919 1079" > free_scale_axis` 即修);
   最后几轮"颜色不对/变黑"全部是探针自己的 bug(`fbrows` 红蓝互换、`fbfill`
   32bpp 掩码)。Xorg 16 bpp 已**实时渲染上屏**——但要注意**第六道闸**:X 接管
   后不带 FORCE,必须用 `fblatch`(FORCE-put+pan,不画像素)重锁存才有画面更新。
   交接时锁存**一次**即可,之后 X 的实时更新自行上屏。整套配方经
   `/usr/local/bin/display-up.sh` 固化进 `rc.local`;冷启动又暴露了三个 bug 并全部
   修进开机链(`HOME=/root`、Xorg 前先起 eudev、`autosuspend=-1`)。默认 session
   现在是 **XFCE4**(`dbus-launch startxfce4`,合成器关;更早的 tint2/pcmanfm 轻量
   版验证过、随后被替换并 `apk del` 清掉)。USB 鼠标经 udev 热插拔已可用。
   **然后冷启动又暴露了第七道闸,而且它才是现在的出厂配置**(§7.7.8):六道闸全满足
   却仍然"无信号",因为链路锁不住 1080p60 要的 148.5 MHz TMDS 时钟;改成
   **`1080p30hz`**(1920×1080 逐行、74.25 MHz)后 HPD 稳定、肉眼确认完美。于是:
   挂着的"再一次冷拔电确认"**已经完成且通过**(三个输入链修复读数全部正确),
   HDMI 是**在 30Hz 下**算 resolved。两点由此而来的副作用都是有意为之:
   * `/dev/env` 仍按 `outputmode=720p50hz` 开机,而 `display-up.sh` 运行时写
     `1080p30hz`,**开机与运行时模式故意不一致**。与 §7.7.4 假设 1 不同,这里无害:
     脚本在*自己的* mode 写入之后,一次性把帧缓冲几何和所有 axis 全编程完。
   * **1080p60 是被绕过去的、不是被判死刑的**:§7.7.7 在同一台 4K 电视上以
     1080p60 读过所有图案,所以怀疑 HDMI PHY 之前,先换线/换输入口复测 1080p60。
   仍未做:让退出对话框的关机/重启按钮变活需要 **elogind + dbus 系统总线**(§7.7.7
   因风险原因否决);在那之前用桌面上的 **Reboot** / **ShutDown** 图标。
2. **WiFi 开机自启已验证通过**:真实断电重启之后 `/var/log/wifi-up.log` 以
   `INTERNET via wlan0: OK` 结束。这一项没有遗留;要记的只有 §6 那条
   `sdiohal` 每次开机只能加载一次的规矩。
3. **`/dev/env` 已经不是出厂状态**。现在读出来是
   `bootcmd=run start_autoscript; run storeboot`、`outputmode=720p50hz`,因为某个
   autoscript 里的 `saveenv` 把它锁住了(§7.1 的更正、§7.7.2);早期实验留下的
   `upgrade_step=2` 也还在里面。**实际后果**:安全不变式变成了"**拔电时把 U 盘拔掉**
   ⇒ 回 Alpine",U 盘留在机器里是每次开机都会被读的。清掉这些需要一次明确的 env 写入
   (需授权)。
4. **凭据卫生:已结**。§9 以前说 `zte.py`、`zte_browser.py` 硬编码了路由器管理口令,
   那是发布前清洗之前的旧话。在本树里复核:两个文件现在都是
   `os.environ.get("ZTE_ROUTER_PASSWORD", "")`;而且本仓库**只有一个 commit**、其中
   就已经是读环境变量的形式,所以明文口令**也不在 git 历史里**。`.gitignore` 覆盖
   `*pass*`、`*.pem`(除 `keys/`)、`*.pcap`、`*.log`、`fw/`、`backup/`、`parts/`、
   `inis/`。这一项没有遗留。
5. 可选:同一颗 UWE5623  combo 上的蓝牙(`sprdbt_tty.ko`,已雕出,未测)。

---

# PART III — Publication notes (本仓库发布说明)

## What is intentionally NOT in this repo (有意排除的内容)

* **Every credential.** The router admin password that §9.4 flagged in
  `zte.py` / `zte_browser.py` is replaced here by
  `os.environ["ZTE_ROUTER_PASSWORD"]`. `.boxrootpass`, the box's root
  password and any WPA passphrase were never stored in this tree (the
  passphrase only ever existed piped over stdin, §0 ground rules).
* **Our MITM TLS material** (`ssrf/key.pem`, `cert.pem`, `pem-all.crt`) —
  generated, not needed for reproduction; regenerate with
  `openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 365 -subj /CN=*.bestv.com.cn`
  and concatenate for the box's CA bundle as `mitmbox.sh` expects.
* **Captured traffic and logs** (`*.pcap`, `*.log`, `*.err`) — they contain
  vendor protocol exchanges tied to one unit's serial/lease.
* **Vendor firmware** (`fw/`, `backup/`, `parts/`, `kitchen/`) — BestV/Amlogic
  images are not ours to redistribute; §1.2 documents where they came from and
  AmlogicKitchen's upstream is in §1.2 as well. The UWE5623 calibration INIs
  (`inis/`) ship inside that vendor firmware, so they are omitted too.
* **Device identifiers**: serials are redacted to `REDACTED-DEVICE-SERIAL` /
  `REDACTED-GD-SERIAL` in the recon dumps (`notes/`, `recon/`, `dtb/`); the
  burned-in MACs stay, since §3.5/§6 depend on them and they are printed on
  the device label.
* `keys/` is the one "private key" we keep **deliberately**: it is the
  infamous **public AOSP testkey** (md5 of `testkey.x509.pem` `12e0c5…`-class,
  shipped with every AOSP checkout) — the entire root story rests on it being
  public (§3.2). Not a secret; reproducible signing requires it present.

## Flashable artifacts in this repo (可直接使用的成品)

This project cannot be a one-click reflash for a *stock* box — the entry point
is the fake-OTA MITM (§3.1), which must run against the vendor server shape.
But for any R1200-C already at the "root ADB in Android" stage (§3.6), these
are ready to use:

* `stage/tvboot.img` — **sha256 `b1f400e2…0e7dd4d`** — the Android-format boot
  image (kernel + `tvrd/init` hand-off ramfs) we flash to p6/p11; dd per §8.
  Rebuild it from `stage/` + `tvrd/` with `tools/mkboot.py` (§4.3).
* `ssrf/pkg/updateR5.zip` — sha256 `f3dac420…441d1a78` — the signed recovery
  payload (testkey, no-authAttrs PKCS#7, ELF update-binary) that §3.5's chain
  serves; other `update*.zip` are its predecessors kept for the record.
* `stage/display-up.sh`, `stage/rc.local`, `stage/inittab`, `stage/fstab`,
  `stage/aml_autoscript` — the on-box Linux config, final working versions.

## Credits (署名)

Field work, exploits-of-patience and this write-up: the tvbox project,
2026-09. Research/tooling assistance and the display+desktop chapters
(§7.7.x, XFCE stack): **Qoder**, an AI pair-programmer, Sep 2026 — every
"RESOLVED" above is a reproducible procedure, not luck; the dead ends are
documented with equal care on purpose.

Keep the ground rules (§0) if you try this: it is your box that is behind the TV.
