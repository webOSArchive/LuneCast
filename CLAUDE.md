# CLAUDE.md - Project Context for Future Sessions

## Project Overview

Screen sharing solution for HP TouchPad (webOS 3.0.5) that captures the device screen and streams it to a host computer over USB via novacom.

**Status**: Working solution with known workaround for launcher capture.

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
| `screenshare-app.c` | webOS SDL app - manages daemon lifecycle, shows UI, Clear Buffer button |
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

1. Install IPK on TouchPad: `palm-install org.webosarchive.screenshare_1.0.0_all.ipk`
2. Launch "Screen Share" from device launcher
3. Run `./stream-server.py` on host
4. View at http://localhost:8080/ or with ffplay/VLC

## Known Issues & Workarounds

### Stale fb1 Content When Viewing Launcher

**Problem**: When switching from a fullscreen app to the launcher, stale content from fb1 (the overlay framebuffer) remains visible, causing ghosting artifacts.

**Root Cause**: fb1 retains the last fullscreen app's content even after the app closes. The compositor doesn't clear it.

**Current Workaround**: "Clear Buffer" button in the Screen Share app fills the screen with black for 2 seconds, which clears fb1. User taps this when they see artifacts.

**Attempted Solutions That Failed**:
1. Alpha channel detection in fb1 - fb1 doesn't use alpha the way we expected
2. Checking if fb0 has significant black content - false positives with dark apps
3. Auto-clearing periodically - only works when Screen Share app is fullscreen (defeats purpose)

**Potential Future Solutions**:
- Hook into webOS window manager events to detect app close
- Investigate HP's open-sourced system software for how built-in screenshot works
- Find a way to detect when LunaSysMgr (compositor) is in launcher mode

### Performance Notes

- ~15 FPS achievable over USB
- JPEG quality 50 is good balance of size/quality (~15-25KB per frame)
- `--low-latency` mode: quality=30, fps=20
- Latency ~100-200ms with proper viewer settings

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

## Distribution

The IPK is fully self-contained:
- `screenshare` binary (SDL app)
- `fbcapture` binary (capture daemon)
- `icon.png`
- `appinfo.json`

Users only need the IPK + stream-server.py on their host.

## Future Work

1. **Solve fb1 ghosting properly** - Investigate webOS internals or HP open source
2. **Reduce latency** - Currently ~100-200ms, could potentially improve
3. **Audio capture** - Not currently implemented
4. **Wireless streaming** - Currently USB-only via novacom
