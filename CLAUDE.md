# CLAUDE.md - Project Context for Future Sessions

## Project Overview

LuneCast - screen sharing for the HP TouchPad (webOS 3.0.5) that captures the device screen and streams it to a host computer over USB via novacom.

**Status**: Working. Layer compositing and capture rate fixed 2026-09-03.

## Architecture

### Device (HP TouchPad)
- ARMv7 Cortex-A8, soft-float ABI
- webOS 3.0.5, Linux kernel 2.6.35
- PalmPDK for native SDL apps
- User-writable: `/media/internal`
- Apps installed to: `/media/cryptofs/apps/usr/palm/applications/`

### Host Computer
- Python 3 + novacom (Palm SDK)
- MJPEG HTTP server for cross-platform viewing

## Key Discovery: Dual-Framebuffer Compositor

The TouchPad uses a dual-framebuffer compositor - this was critical to understand:

| Framebuffer | msm_fb_type | Role | Content |
|-------------|-------------|------|---------|
| `/dev/fb0` | `lcdc panel` | **TOP** layer | System chrome: status bar, notifications |
| `/dev/fb1` | (none, `msmfb40_30001`) | **BOTTOM** layer | App plane: launcher, cards, web apps, PDK apps |

**fb0 is composited OVER fb1, not under it.** This is the opposite of what this
file said until 2026-09-03, and the old ordering is why capture looked wrong.

Measured on device with a card app open (dumping the visible page of each
framebuffer and histogramming the alpha byte):

```
fb0   alpha=0 on 96.4% of pixels, alpha=255 on 3.6%
      ...and that 3.6% is exactly the status bar strip.
fb1   alpha=255 on 93.0%, alpha=0 on 3.6% (the same strip), partial alpha on 3.3%
```

A layer transparent over 96% of the panel cannot be the base - if it were,
almost the whole screen would composite to nothing. fb0 is the chrome plane
drawn on top; fb1 carries the actual app content underneath.

**Both framebuffers carry a real alpha channel.** The correct capture is a
per-pixel source-over blend of fb0 onto fb1, keyed on fb0's alpha - not a
colour-key pick between the two. See `composite_to_rgb()` in `fbcapture.c`.

### The 3-layer compositor vs. the 2 framebuffers

webOS documents a *three*-layer compositor (background/wallpaper, application,
system UI) but the kernel exposes only `fb0` and `fb1`. The third plane is an
MDP hardware overlay pipe: buffers in `pmem_smipool` / `kgsl` memory that the
display controller composites at scanout, programmed via `MSMFB_OVERLAY_*`
ioctls. `/sys/devices/platform` shows the two pipelines (`mdp.0`/`msm_fb.0` and
`mdp.196609`/`msm_fb.196609`).

**That third plane is not readable through `/dev/fb*` at all.** Anything drawn
by the GPU into an overlay pipe - GL PDK apps (games, emulators) and hardware
video playback - will be invisible to a framebuffer-based capture no matter how
the blend is done. Capturing those needs a different mechanism (GPU-side
readback, e.g. `glReadPixels` at `SDL_GL_SwapBuffers` via `LD_PRELOAD`).

**Both framebuffers use triple-buffering** (1024x2304 total = 3 pages of 768 lines). The visible buffer cycles through pan offsets 0, 768, 1536.

Pan offsets are read from:
- `/sys/class/graphics/fb0/pan`
- `/sys/class/graphics/fb1/pan`

## Key Files

| File | Purpose |
|------|---------|
| `fbcapture.c` | Capture daemon - reads fb0+fb1 with correct pan offsets, composites, encodes JPEG |
| `screenshare-app.c` | webOS SDL app (builds to `lunecast`) - manages daemon lifecycle, shows status UI |
| `stream-server.py` | Host-side MJPEG HTTP server - fetches JPEGs via novacom, serves to VLC/ffplay/browser |
| `Makefile` | Cross-compilation with Linaro GCC 4.9.4 |
| `package/` | IPK contents (appinfo.json, binaries, icon) |
| `CLAUDE.md` | This file - project context for AI sessions |

## Build Requirements

- Linaro GCC 4.9.4 cross-compiler at `/opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi`
- PalmPDK at `/opt/PalmPDK`
- Palm SDK (novacom, palm-package, palm-install)

## Build Commands

```bash
make              # Build everything
make package      # Create IPK
make install      # Install to device
```

## Usage

1. Install IPK on TouchPad: `palm-install org.webosarchive.lunecast_1.0.0_all.ipk`
2. Launch "LuneCast" from device launcher
3. Run `./stream-server.py` on host
4. View at http://localhost:8080/ or with ffplay/VLC

## Known Issues & Workarounds

### Stale fb1 Content When Viewing Launcher - RESOLVED (2026-09-03)

**This is fixed and the "Clear Buffer" workaround has been removed.**

The ghosting was never the compositor retaining stale pixels - it was the
capture picking the wrong ones. The old `composite_to_rgb()` selected fb1
wherever fb1's RGB was non-black, so whatever fb1 happened to hold bled
through. With the corrected layer order and per-pixel alpha compositing
(see "Key Discovery" above), fb1 is only shown where fb0's chrome plane is
transparent, and the compositor keeps fb1 current on its own.

Verified by reproducing the original scenario: a fullscreen PDK app was
closed (its pixels in fb1) and the card view captured immediately after -
0.6% near-black across successive frames, where a ghost of the app's dark UI
would have been ~90%.

If ghosting ever reappears, `fbcapture -0` (fb0-only) and the removed
button are both in git history.

### Performance Notes

Measured on device (idle, `fbcapture -T`):

| Stage | Cost |
|-------|------|
| Composite (1024x768, both planes) | ~20 ms |
| JPEG encode (q50-75) | ~120 ms |
| Unchanged frame (encode skipped) | ~25 ms |
| Host `novacom get` per frame | ~53 ms (~19 fps ceiling) |

- The JPEG encode dominates. This is stock 2011 libjpeg62 with no NEON;
  `JDCT_IFAST` is already enabled.
- Framebuffer memory is UNCACHED - read it with 32-bit words, not bytes.
- The daemon skips the encode when the composited frame is unchanged, and
  backs its poll interval off when idle (~27% of a core, was ~62%).
- Remaining wins not yet done: a persistent novacom stream instead of a
  per-frame `novacom get`, and downscaling before encode.

## Code Patterns

### Reading Pan Offset
```c
static int get_pan_offset(const char *path, int *x, int *y) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (fscanf(f, "%d,%d", x, y) != 2) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}
```

### Compositing fb0 + fb1
The capture reads both framebuffers at their current pan offsets and composites them. When fb1 has content (fullscreen app), it's used; otherwise fb0 shows through.

## Stream Server Behavior

- **Default**: Does NOT manage daemon (device app handles it)
- **--force-daemon**: Server starts/stops daemon on device

### Port negotiation

Both ends default to 8080, but the host may not have it free (nginx is the
common culprit). The server resolves this and tells the device:

1. `bind_server()` tries the preferred port, then scans upward through
   `PORT_SCAN_SPAN` (20) ports. `HTTPServer` sets `SO_REUSEADDR`, which lets
   it rebind a port in TIME_WAIT but still fails against a live listener, so
   a genuine conflict is detected rather than silently shadowed.
2. Having bound, it writes the chosen port to
   `/media/internal/lunecast-port.txt` on the device via `novacom put`.
3. The app re-reads that file once a second (`refresh_stream_port()`) and
   renders the port in its on-screen instructions. `/media/internal` is
   bind-mounted rw into the PDK jail, so the jailed app can read it.
4. On a clean exit the server deletes the file and the app falls back to
   8080 - which is what the server will try first next time.

Caveat: if the server is killed uncleanly the file is left behind, and the
app will advertise a port nothing is listening on until the next run.

## Distribution

The IPK is fully self-contained:
- `lunecast` binary (SDL app)
- `fbcapture` binary (capture daemon)
- `icon.png` (64px launcher icon), `splash-icon.png` (256px), `status-icon.png` (128px, shown in-app)
- `appinfo.json`

Users only need the IPK + stream-server.py on their host.

## Future Work

1. **Solve fb1 ghosting properly** - Investigate webOS internals or HP open source
2. **Reduce latency** - Currently ~100-200ms, could potentially improve
3. **Audio capture** - Not currently implemented
4. **Wireless streaming** - Currently USB-only via novacom
