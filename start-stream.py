#!/usr/bin/env python3
"""
MJPEG Streaming Server for LuneCast

Fetches screenshots from the TouchPad via novacom and streams them
as MJPEG over HTTP. Compatible with VLC, ffplay, browsers, etc.

Usage:
    ./start-stream.py [--port 8080] [--fps 10]

View with:
    VLC:     vlc http://localhost:8080/stream
    ffplay:  ffplay http://localhost:8080/stream
    Browser: http://localhost:8080/
"""

import argparse
import http.server
import io
import os
import signal
import socketserver
import subprocess
import sys
import threading
import time
import webbrowser
from typing import Optional

# Global state
frame_lock = threading.Lock()
current_frame: Optional[bytes] = None
frame_count = 0
running = True


def fetch_frame() -> Optional[bytes]:
    """Fetch a single frame from the device via novacom"""
    try:
        # Use shorter timeout and direct stdout pipe for lower latency
        result = subprocess.run(
            ["novacom", "get", "file:///media/internal/screen.jpg"],
            capture_output=True,
            timeout=1  # Reduced from 5s
        )
        if result.returncode == 0 and result.stdout:
            return result.stdout
    except subprocess.TimeoutExpired:
        pass  # Don't print for timeout, just retry
    except FileNotFoundError as e:
        print(f"Fetch error: {e}", file=sys.stderr)
    return None


def _read_exact(pipe, n):
    """Read exactly n bytes, or None if the pipe closed first."""
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = pipe.read(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def stream_fetcher(quality: int, interval_ms: int, fast_dct: bool = False):
    """Read framed JPEGs from ONE persistent `novacom run ... -S`.

    The file transport spawned a fresh `novacom get` per frame - roughly 53ms
    of process spawn and USB session setup each time - and made the device
    write a JPEG to VFAT every frame. Here the daemon encodes to memory and
    pushes frames down a single pipe, so the host receives every frame the
    device produces, in order, with no polling and no sampling.

    Wire format per frame: b"LCF1" + 4-byte big-endian length + JPEG.
    """
    global current_frame, frame_count, running, _fps_line_active

    cmd = ["novacom", "run", f"file://{DEVICE_APP_DIR}/fbcapture", "--",
           "-S", "-i", str(interval_ms), "-q", str(quality)]
    if fast_dct:
        cmd.append("-F")

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, bufsize=0)
    pipe = proc.stdout

    last_fps_time = time.time()
    fps_frame_count = 0

    try:
        while running:
            header = _read_exact(pipe, 8)
            if header is None:
                break

            if header[:4] != STREAM_MAGIC:
                # Lost sync. Walk forward one byte at a time until the magic
                # lines up again rather than abandoning the stream.
                window = bytearray(header)
                resynced = False
                while running:
                    idx = bytes(window).find(STREAM_MAGIC)
                    if idx != -1 and len(window) - idx >= 8:
                        header = bytes(window[idx:idx + 8])
                        resynced = True
                        break
                    nxt = pipe.read(1)
                    if not nxt:
                        break
                    window += nxt
                    if len(window) > 4096:
                        del window[:len(window) - 64]
                if not resynced:
                    break

            length = int.from_bytes(header[4:8], "big")
            if length == 0 or length > 8 * 1024 * 1024:
                break   # implausible: treat as corruption and stop

            payload = _read_exact(pipe, length)
            if payload is None:
                break

            with frame_lock:
                current_frame = payload
                frame_count += 1
            fps_frame_count += 1

            now = time.time()
            if now - last_fps_time >= 5.0:
                fps = fps_frame_count / (now - last_fps_time)
                if sys.stdout.isatty():
                    print(f"\rStream FPS: {fps:4.1f}", end="", flush=True)
                    _fps_line_active = True
                else:
                    print(f"Stream FPS: {fps:.1f}")
                fps_frame_count = 0
                last_fps_time = now
    finally:
        try:
            proc.terminate()
        except Exception:
            pass


def publish_host_capture(enable: bool):
    """Claim (or release) capture ownership so the device app does not also run
    a daemon. The app polls this file once a second."""
    try:
        if enable:
            subprocess.run(["novacom", "put", f"file://{DEVICE_HOST_FILE}"],
                           input=b"1\n", capture_output=True, timeout=5)
        else:
            subprocess.run(["novacom", "run", "file://bin/rm", "--", "-f", DEVICE_HOST_FILE],
                           capture_output=True, timeout=5)
    except Exception:
        pass


def frame_fetcher(target_fps: int):
    """Background thread that continuously fetches frames"""
    global current_frame, frame_count, running

    interval = 1.0 / target_fps
    last_fps_time = time.time()
    fps_frame_count = 0

    while running:
        start = time.time()

        frame = fetch_frame()
        if frame:
            with frame_lock:
                current_frame = frame
                frame_count += 1
            fps_frame_count += 1

        # Report FPS every 5 seconds.
        #
        # On a terminal this rewrites one line in place, so the instructions
        # printed at startup stay on screen instead of scrolling away. When
        # stdout is redirected to a file or a pipe, carriage returns would
        # just produce one unreadable line, so fall back to normal lines.
        # Fixed width keeps a shorter value (9.2) from leaving a digit of a
        # longer one (13.8) behind.
        now = time.time()
        if now - last_fps_time >= 5.0:
            fps = fps_frame_count / (now - last_fps_time)
            global _fps_line_active
            if sys.stdout.isatty():
                print(f"\rFetch FPS: {fps:4.1f}", end="", flush=True)
                _fps_line_active = True
            else:
                print(f"Fetch FPS: {fps:.1f}")
            fps_frame_count = 0
            last_fps_time = now

        # Sleep for remaining interval
        elapsed = time.time() - start
        sleep_time = interval - elapsed
        if sleep_time > 0:
            time.sleep(sleep_time)


class MJPEGHandler(http.server.BaseHTTPRequestHandler):
    """HTTP handler for MJPEG streaming"""

    def log_message(self, format, *args):
        """Suppress default logging"""
        pass

    def do_GET(self):
        if self.path == '/':
            self.serve_index()
        elif self.path == '/stream':
            self.serve_mjpeg_stream()
        elif self.path == '/snapshot':
            self.serve_snapshot()
        elif self.path == '/status':
            self.serve_status()
        else:
            self.send_error(404)

    def serve_index(self):
        """Serve a simple HTML page with the stream"""
        html = b"""<!DOCTYPE html>
<html>
<head>
    <title>LuneCast</title>
    <style>
        body {
            margin: 0;
            background: #1a1a1a;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            min-height: 100vh;
            font-family: system-ui, sans-serif;
            color: #fff;
        }
        h1 { margin-bottom: 20px; }
        img {
            max-width: 100%;
            border: 2px solid #333;
            border-radius: 8px;
        }
        .info {
            margin-top: 20px;
            color: #888;
            font-size: 14px;
        }
        a { color: #4af; }
    </style>
</head>
<body>
    <h1>LuneCast</h1>
    <img src="/stream" alt="webOS Screen">
    <div class="info">
        <p>Stream URL: <a href="/stream">/stream</a> (for VLC, ffplay, etc.)</p>
        <p>Snapshot: <a href="/snapshot">/snapshot</a></p>
        <p>Status: <a href="/status">/status</a></p>
    </div>
</body>
</html>"""
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', len(html))
        self.end_headers()
        self.wfile.write(html)

    def serve_mjpeg_stream(self):
        """Serve continuous MJPEG stream"""
        self.send_response(200)
        self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=frame')
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        self.end_headers()

        last_frame_count = -1

        try:
            while running:
                with frame_lock:
                    frame = current_frame
                    fc = frame_count

                # Only send if we have a new frame
                if frame and fc != last_frame_count:
                    last_frame_count = fc
                    self.wfile.write(b'--frame\r\n')
                    self.wfile.write(b'Content-Type: image/jpeg\r\n')
                    self.wfile.write(f'Content-Length: {len(frame)}\r\n'.encode())
                    self.wfile.write(b'\r\n')
                    self.wfile.write(frame)
                    self.wfile.write(b'\r\n')
                    self.wfile.flush()
                else:
                    time.sleep(0.01)  # Small sleep to avoid busy-waiting

        except (BrokenPipeError, ConnectionResetError):
            pass  # Client disconnected

    def serve_snapshot(self):
        """Serve a single JPEG snapshot"""
        with frame_lock:
            frame = current_frame

        if frame:
            self.send_response(200)
            self.send_header('Content-Type', 'image/jpeg')
            self.send_header('Content-Length', len(frame))
            self.send_header('Cache-Control', 'no-cache')
            self.end_headers()
            self.wfile.write(frame)
        else:
            self.send_error(503, 'No frame available')

    def serve_status(self):
        """Serve JSON status"""
        import json
        with frame_lock:
            has_frame = current_frame is not None
            fc = frame_count

        status = {
            'running': running,
            'frame_count': fc,
            'has_frame': has_frame
        }

        body = json.dumps(status).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', len(body))
        self.end_headers()
        self.wfile.write(body)


def open_viewer(port: int, open_with: str):
    """Open a viewer once the server is listening.

    Opens /stream directly: it fills the viewport with the image instead of
    sitting inside a page. Verified rendering a top-level
    multipart/x-mixed-replace in both engines available here - Firefox and
    Chromium (tested via Edge). WebKit/Safari is untested; the viewer page at
    / still embeds the stream in an <img> if a browser ever refuses the raw
    endpoint.

    Fires on a short timer so the request lands after serve_forever() is
    actually accepting.
    """
    page_url = f"http://localhost:{port}/"
    stream_url = f"http://localhost:{port}/stream"

    if open_with == 'none':
        return

    # No GUI (ssh session, headless box) - opening anything is pointless.
    if (sys.platform.startswith('linux')
            and not os.environ.get('DISPLAY')
            and not os.environ.get('WAYLAND_DISPLAY')):
        print(f"No display detected - open {page_url} yourself.")
        return

    target = stream_url
    friendly = {'browser': 'your default browser', 'vlc': 'VLC', 'ffplay': 'ffplay'}[open_with]
    print(f"Opening {target} in {friendly} now...")
    print("  (if that does not work, open the URL above yourself)\n")

    def launch():
        try:
            if open_with == 'browser':
                webbrowser.open(page_url)
            elif open_with == 'vlc':
                subprocess.Popen(['vlc', '--network-caching=50', stream_url],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            elif open_with == 'ffplay':
                subprocess.Popen(['ffplay', '-fflags', 'nobuffer', '-flags', 'low_delay',
                                  '-framedrop', stream_url],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except FileNotFoundError:
            print(f"Could not launch '{open_with}' - open {page_url} yourself.")
        except Exception as exc:
            print(f"Could not open viewer ({exc}) - open {page_url} yourself.")

    threading.Timer(1.0, launch).start()


_fps_line_active = False   # an in-place "Fetch FPS" line is awaiting a newline

DEVICE_PORT_FILE = "/media/internal/lunecast-port.txt"
DEVICE_HOST_FILE = "/media/internal/lunecast-host.txt"
DEVICE_APP_DIR = "/media/cryptofs/apps/usr/palm/applications/org.webosarchive.lunecast"
STREAM_MAGIC = b"LCF1"
PORT_SCAN_SPAN = 20


def bind_server(preferred_port):
    """Bind the HTTP server, falling back if the preferred port is taken.

    Tries the preferred port first (the default both ends assume), then scans
    upward. HTTPServer sets SO_REUSEADDR, which lets us rebind a port sitting
    in TIME_WAIT but still fails against something actively listening - so a
    real conflict (nginx on 8080) is detected rather than silently shadowed.

    Returns (server, port).
    """
    candidates = [preferred_port]
    candidates += [p for p in range(preferred_port, preferred_port + PORT_SCAN_SPAN)
                   if p != preferred_port]

    last_error = None
    for port in candidates:
        try:
            return ThreadedHTTPServer(('', port), MJPEGHandler), port
        except OSError as exc:
            last_error = exc
            continue

    raise SystemExit(
        f"Could not bind any port in {preferred_port}-{preferred_port + PORT_SCAN_SPAN - 1}: {last_error}")


def publish_port(port: int) -> bool:
    """Tell the device which port we actually bound.

    The app polls this file and renders it in its on-screen instructions, so
    the two ends agree even when the default was unavailable.
    """
    try:
        result = subprocess.run(
            ["novacom", "put", f"file://{DEVICE_PORT_FILE}"],
            input=f"{port}\n".encode(),
            capture_output=True,
            timeout=5,
        )
        return result.returncode == 0
    except Exception:
        return False


def clear_published_port():
    """Remove the published port on a clean exit, so the device does not keep
    advertising a port that nothing is listening on any more."""
    try:
        subprocess.run(
            ["novacom", "run", "file://bin/rm", "--", "-f", DEVICE_PORT_FILE],
            capture_output=True,
            timeout=5,
        )
    except Exception:
        pass


class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    """HTTP server that handles requests in threads"""
    allow_reuse_address = True
    daemon_threads = True


def check_device():
    """Check if device is connected"""
    try:
        result = subprocess.run(
            ["novacom", "-l"],
            capture_output=True,
            text=True,
            timeout=5
        )
        return "usb" in result.stdout.lower()
    except:
        return False


def check_daemon():
    """Check if capture daemon is running on device"""
    try:
        result = subprocess.run(
            ["novacom", "run", "file://bin/pidof", "--", "fbcapture"],
            capture_output=True,
            timeout=5
        )
        return result.returncode == 0 and result.stdout.strip()
    except:
        return False


def start_daemon(fps: int = 15, quality: int = 50):
    """Start the capture daemon on device"""
    interval_ms = max(33, int(1000 / fps))  # Cap at ~30 FPS (33ms min)

    # Try installed app path first, then fall back to /media/internal
    paths = [
        "file:///media/cryptofs/apps/usr/palm/applications/org.webosarchive.lunecast/fbcapture",
        "file:///media/internal/fbcapture"
    ]

    for path in paths:
        try:
            subprocess.run(
                ["novacom", "run", path, "--",
                 "-D", "-i", str(interval_ms), "-q", str(quality),
                 "-o", "/media/internal/screen.jpg"],
                timeout=10
            )
            time.sleep(0.5)
            if check_daemon():
                return True
        except:
            continue
    return False


def stop_daemon():
    """Stop the capture daemon on device"""
    try:
        subprocess.run(
            ["novacom", "run", "file://bin/killall", "--", "fbcapture"],
            timeout=5
        )
    except:
        pass


def main():
    global running

    parser = argparse.ArgumentParser(description='LuneCast MJPEG Server')
    parser.add_argument('--port', '-p', type=int, default=8080,
                       help='HTTP port (default: 8080)')
    parser.add_argument('--fps', '-f', type=int, default=15,
                       help='Target FPS (default: 15)')
    parser.add_argument('--quality', '-q', type=int, default=85,
                       help='JPEG quality 1-100 (default: 85)')
    parser.add_argument('--low-latency', '-l', action='store_true',
                       help='Low latency mode: quality=30, fps=20')
    parser.add_argument('--force-daemon', action='store_true',
                       help="Force start/stop daemon on device (default: let device app manage it)")
    parser.add_argument('--fast-dct', action='store_true',
                       help='Faster, less accurate DCT (~15ms/frame). Can ring on text edges.')
    parser.add_argument('--transport', default='stream', choices=['stream', 'file'],
                       help='stream: one persistent novacom pipe (default). '
                            'file: legacy per-frame novacom get')
    parser.add_argument('--open', dest='open_with', default='browser',
                       choices=['browser', 'vlc', 'ffplay', 'none'],
                       help='Viewer to open automatically once the server is up (default: browser)')
    args = parser.parse_args()

    # Apply low-latency mode settings
    if args.low_latency:
        args.quality = 30
        args.fps = 20
        print("Low-latency mode: quality=30, fps=20")

    # Check device connection
    print("Checking device connection...")
    if not check_device():
        print("ERROR: No webOS device connected via USB")
        print("Connect your TouchPad and ensure USB debugging is enabled")
        sys.exit(1)
    print("Device connected!")

    # Check/start daemon
    if args.force_daemon:
        if check_daemon():
            print("Capture daemon already running on device")
        else:
            print(f"Starting capture daemon (FPS: {args.fps}, quality: {args.quality})...")
            if start_daemon(args.fps, args.quality):
                print("Daemon started successfully")
            else:
                print("WARNING: Could not start daemon. Is fbcapture deployed?")
                print("Run 'make deploy' first, or launch the LuneCast app on the device")

    # Start frame fetcher thread
    print(f"Starting frame fetcher (target: {args.fps} FPS)...")
    if args.transport == 'stream':
        # Claim capture first, then give the device app a moment to notice and
        # shut its own daemon down - otherwise both capture for a second or two
        # and compete for the framebuffer. The app polls the marker at 1Hz.
        print("Claiming capture (persistent novacom stream)...")
        publish_host_capture(True)
        time.sleep(1.5)
        interval_ms = max(20, int(1000 / args.fps))
        fetcher_thread = threading.Thread(
            target=stream_fetcher, args=(args.quality, interval_ms, args.fast_dct), daemon=True)
    else:
        fetcher_thread = threading.Thread(target=frame_fetcher, args=(args.fps,), daemon=True)
    fetcher_thread.start()

    # Wait for first frame
    print("Waiting for first frame...")
    for _ in range(50):  # 5 second timeout
        with frame_lock:
            if current_frame:
                break
        time.sleep(0.1)
    else:
        print("WARNING: No frames received yet. Check device daemon.")

    # Start HTTP server
    server, port = bind_server(args.port)
    if port != args.port:
        print(f"Port {args.port} is in use; using {port} instead.")

    if publish_port(port):
        print(f"Told the device to use port {port}.")
    else:
        print(f"WARNING: could not publish port {port} to the device; "
              f"its on-screen instructions may show the default.")

    def signal_handler(sig, frame):
        global running, _fps_line_active
        print("\nShutting down...")
        _fps_line_active = False   # the \n above already closed the FPS line
        running = False
        # Call shutdown from a thread to avoid deadlock
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    print(f"\n{'='*60}")
    print(f"LuneCast Server (quality={args.quality}, fps={args.fps})")
    print(f"{'='*60}")
    print(f"OPEN THE STREAM AT:  http://localhost:{port}/")
    print(f"")
    print(f"Other ways to view it:")
    print(f"  Direct MJPEG stream:  http://localhost:{port}/stream")
    print(f"  Lowest latency:       ffplay -fflags nobuffer -flags low_delay -framedrop \\")
    print(f"                                http://localhost:{port}/stream")
    print(f"  VLC:                  vlc --network-caching=50 http://localhost:{port}/stream")
    print(f"{'='*60}")
    print(f"Press Ctrl+C to stop\n")

    open_viewer(port, args.open_with)

    try:
        server.serve_forever()
    finally:
        running = False
        if args.transport == 'stream':
            publish_host_capture(False)   # hand capture back to the device app
        if _fps_line_active:
            print()   # close the in-place FPS line so the prompt starts clean
        clear_published_port()
        if args.force_daemon:
            print("Stopping device daemon...")
            stop_daemon()
        print("Server stopped.")


if __name__ == '__main__':
    main()
