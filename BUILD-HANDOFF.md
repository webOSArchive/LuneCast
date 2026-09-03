# Build Handoff: rebuild `fbcapture` on the Linux VM

This file is for a Claude session (or a human) running on the Linux VM that has
the cross-toolchain installed. It explains what is broken, what to build, and
how to verify it.

## The problem (diagnosed 2026-09-03 from the Mac)

The receiver (`stream-server.py`) connects but never shows a frame because the
capture daemon is not running on the TouchPad. Specifically:

- The installed app directory on the device,
  `/media/cryptofs/apps/usr/palm/applications/org.webosarchive.screenshare/`,
  contains only `appinfo.json`, `icon.png`, and `screenshare`. **There is no
  `fbcapture` binary.**
- `screenshare-app.c` forks and `execl()`s `fbcapture` from that directory
  (fallback: `/media/internal/fbcapture`, also absent). Both exec calls fail,
  the child exits, and the app loops on "Status: Starting..." forever.
- So `/media/internal/screen.jpg` is never written, and every
  `novacom get file:///media/internal/screen.jpg` from the host fails with
  `file open failed`.
- Why: `.gitignore` excludes `fbcapture` and `package/fbcapture`. The committed
  IPK (`org.webosarchive.screenshare_1.0.0_all.ipk`, commit `acfe573`) was
  built from a `package/` directory that did not contain the daemon, so the
  IPK ships without it. `fbcapture` was never committed anywhere, and the Mac
  has no ARM cross-compiler (the Linaro path in the Makefile does not exist
  there, and Linaro's download URL now redirects to a contact page).

Nothing is wrong with `fbcapture.c`, `screenshare-app.c`, or
`stream-server.py`. This is purely a missing build artifact.

## What to build

Only `fbcapture` is required. Rebuilding `screenshare` too is fine but not
necessary (the installed one works).

```bash
cd webos-screenshare
make daemon            # builds ./fbcapture from fbcapture.c, links -ljpeg from /opt/PalmPDK/device/lib
make strip             # optional (strips both; if screenshare isn't built, run the strip binary on fbcapture only)
```

The Makefile expects:

- Linaro GCC 4.9.4 at `/opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi`
  (override with `make LINARO_GCC=/path/to/toolchain daemon` if it lives elsewhere)
- PalmPDK at `/opt/PalmPDK` (needs `include/jpeglib.h` and `device/lib/libjpeg.so`)

Required flags are already in the Makefile: `-march=armv7-a -mfloat-abi=softfp
-mfpu=neon`, dynamic link against the PDK's `libjpeg.so.62`.

## Compatibility check before deploying

The device is older than you might expect:

| Item | Device value |
|------|--------------|
| Kernel | Linux 2.6.35 |
| glibc | **2.8** (`/lib/libc-2.8.so`) |
| libjpeg | `libjpeg.so.62.0.0` in `/usr/lib` |
| ABI | ARMv7, soft-float (`softfp`) |

After building, confirm the binary does not require glibc symbol versions
newer than 2.8:

```bash
/opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi/bin/arm-linux-gnueabi-objdump -T fbcapture | grep GLIBC_ | awk '{print $5}' | sort -u
```

Every version listed must be `GLIBC_2.4` through `GLIBC_2.8`. Anything higher
(e.g. `GLIBC_2.15`, `GLIBC_2.17`) will fail at load time on the device.
`fbcapture.c` only uses `fscanf`, `gettimeofday`, `ioctl`, `mmap`, `rename`,
`snprintf`, `usleep`, so this should pass. Also confirm
`readelf -A fbcapture` shows `Tag_ABI_VFP_args` is absent (softfp, not hard-float).

## Deploy and verify

Either do this from the VM (if novacom is installed there and the TouchPad is
attached) or copy `fbcapture` back to the Mac and run the same commands there.

Quick test without reinstalling the IPK (the app dir on the device is
world-writable, and the running app retries the exec every frame, so it picks
this up immediately):

```bash
novacom put file:///media/cryptofs/apps/usr/palm/applications/org.webosarchive.screenshare/fbcapture < fbcapture
novacom run file://bin/chmod -- +x /media/cryptofs/apps/usr/palm/applications/org.webosarchive.screenshare/fbcapture
# With the Screen Share app open on the device:
novacom run file://bin/pidof -- fbcapture                     # should print a PID
novacom get file:///media/internal/screen.jpg > test.jpg      # should be a JPEG
file test.jpg
./stream-server.py                                            # then open http://localhost:8080/
```

If the app was not open, you can also test the daemon standalone:

```bash
novacom run file:///media/cryptofs/apps/usr/palm/applications/org.webosarchive.screenshare/fbcapture -- -o /media/internal/screen.jpg -q 75
novacom get file:///media/internal/screen.jpg > test.jpg
```

## Make the fix durable

Once verified, rebuild the IPK so it actually contains the daemon, and
reinstall it:

```bash
make package      # copies fbcapture + screenshare into package/, runs palm-package
tar tzf <(ar p org.webosarchive.screenshare_1.0.0_all.ipk data.tar.gz) | grep fbcapture   # must list it
make install      # palm-install; close the Screen Share app on the device first
```

Then commit the regenerated `.ipk` so the repo's shipped package is correct.
Consider also committing the stripped `fbcapture` binary (remove it from
`.gitignore`) or adding a CI/release step, so this can't silently regress
again. The `screenshare` binary is already committed, so committing
`fbcapture` alongside it would be consistent.

## What to bring back to the Mac (if deploying from there)

- `fbcapture` (stripped, ARM binary)
- the regenerated `org.webosarchive.screenshare_1.0.0_all.ipk`
