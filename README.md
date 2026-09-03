# LuneCast

Share your HP TouchPad's screen with your computer over USB.

## Features

- **Cross-platform viewer** - Works with VLC, ffplay, or any browser
- **MJPEG streaming** - Standard HTTP stream at ~15 FPS
- **Correct layer compositing** - Alpha-composites the fb0 chrome plane over the fb1 app plane
- **webOS app** - Launches from device launcher, auto-stops on close
- **No network required** - Everything works over USB via novacom
- **Automatic port negotiation** - If 8080 is taken on the host, the server
  picks another and tells the device, which updates its on-screen instructions

## Quick Start

### 1. Install the app on your TouchPad

**Option A: Download pre-built IPK** (recommended for end users)
```bash
palm-install org.webosarchive.lunecast_1.0.0_all.ipk
```

**Option B: Build from source**
```bash
make install
```

### 2. Launch "LuneCast" from the device launcher

The app will start the capture daemon and show connection instructions.

### 3. On your computer, run the streaming server

```bash
./stream-server.py
```

For lowest latency:
```bash
./stream-server.py --low-latency
```

### 4. View the stream

**ffplay (lowest latency):**
```bash
ffplay -fflags nobuffer -flags low_delay -framedrop http://localhost:8080/stream
```

**VLC:**
```bash
vlc --network-caching=50 http://localhost:8080/stream
```

**Browser:**
Open http://localhost:8080/

### 5. Close the app on the device

Swipe the card away to stop streaming.

## Distribution

The IPK package is fully self-contained:
- `lunecast` - The webOS app (manages daemon, shows UI)
- `fbcapture` - The capture daemon (bundled, no separate install needed)
- `icon.png` - App icon

Users only need to:
1. Install the IPK on their TouchPad
2. Run `stream-server.py` on their computer (requires Python 3 + novacom)

## Components

| File | Description |
|------|-------------|
| `lunecast` | webOS app - shows UI and manages daemon |
| `fbcapture` | Daemon - captures framebuffer to JPEG |
| `stream-server.py` | Host - MJPEG HTTP server for VLC/browsers |

## Build Requirements

### Host
- Linaro GCC 4.9.4 cross-compiler (`/opt/gcc-linaro-4.9.4-*`)
- PalmPDK (`/opt/PalmPDK`)
- Palm SDK (novacom, palm-package, palm-install)
- Python 3 (for stream server)

### Device
- HP TouchPad with webOS 3.0.x
- USB debugging enabled (Developer Mode)

## Build Commands

```bash
make              # Build everything
make daemon       # Build fbcapture only
make app          # Build lunecast app only
make package      # Create IPK package
make install      # Install to device
make uninstall    # Remove from device
```

## Testing (without app)

```bash
make deploy       # Deploy daemon to /media/internal
make start-daemon # Start daemon manually
make stop-daemon  # Stop daemon

# Then on host:
./stream-server.py --force-daemon
```

## Stream Server Options

```
./stream-server.py [options]

Options:
  --port, -p PORT     HTTP port (default: 8080)
  --fps, -f FPS       Target FPS (default: 15)
  --quality, -q QUAL  JPEG quality 1-100 (default: 75)
  --low-latency, -l   Low latency mode (quality=30, fps=20)
  --force-daemon      Force start/stop daemon (default: let device app manage it)
```

## Performance

| Metric | Value |
|--------|-------|
| Resolution | 1024×768 |
| Frame rate | ~15 FPS |
| JPEG size | ~15-25KB |
| Latency | ~100-200ms |

## Architecture

```
┌─────────────────┐         USB          ┌─────────────────┐
│   HP TouchPad   │◄────────────────────►│  Host Computer  │
│                 │       novacom        │                 │
│  ┌───────────┐  │                      │  ┌───────────┐  │
│  │ lunecast  │  │                      │  │  stream-  │  │
│  │   (app)   │  │                      │  │ server.py │  │
│  └─────┬─────┘  │                      │  └─────┬─────┘  │
│        │fork    │                      │        │        │
│  ┌─────▼─────┐  │    novacom get       │        │HTTP    │
│  │ fbcapture │──┼──────────────────────┼───────►│        │
│  │ (daemon)  │  │   screen.jpg         │        ▼        │
│  └─────┬─────┘  │                      │  ┌───────────┐  │
│        │        │                      │  │ VLC/ffplay│  │
│   ┌────┴────┐   │                      │  │ /browser  │  │
│   ▼         ▼   │                      │  └───────────┘  │
│ /dev/fb0  /dev/fb1                     │                 │
│ (compositor) (overlay)                 │                 │
└─────────────────┘                      └─────────────────┘
```

### Dual Framebuffer

The HP TouchPad uses a dual-framebuffer compositor:

| Framebuffer | Purpose | Content |
|-------------|---------|---------|
| `/dev/fb0` | Base layer | Launcher, app switcher, carded apps, web apps |
| `/dev/fb1` | Overlay | Fullscreen PDK apps (native/Qt apps) |

Both framebuffers use triple-buffering (1024×2304 total, 3 pages of 768 lines).
The daemon reads the current pan offset from `/sys/class/graphics/fb*/pan` to
capture the correct visible buffer.

### Layer order

`fb0` is the TOP layer (system chrome: status bar, notifications) and `fb1`
is the app plane beneath it - the opposite of what earlier versions assumed.
Measured on device, fb0 is `alpha=0` over 96.4% of the panel and opaque only
in the status bar strip, so it cannot be the base layer. The daemon
alpha-composites fb0 over fb1 per pixel.

Earlier versions colour-keyed on "non-black RGB" instead, which is why dark
app content used to disappear and why stale fb1 content used to ghost
through. The old "Clear Buffer" workaround for that ghosting is no longer
needed and has been removed.

## Troubleshooting

### "No webOS device connected"
- Check USB cable connection
- Enable Developer Mode on device
- Run `novacom -l` to verify connection

### App crashes on launch
- Check that both `lunecast` and `fbcapture` are in the package
- Verify font files exist on device

### Low FPS or stuttering
- Try lower quality: `./stream-server.py -q 50`
- Use a better USB cable
- Close other apps on device

### VLC shows artifacts
- Add `--network-caching=100` to VLC command
- Try ffplay instead

## License

MIT License
