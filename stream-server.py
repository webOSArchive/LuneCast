#!/usr/bin/env python3
"""
MJPEG Streaming Server for webOS Screen Share

Fetches screenshots from the TouchPad via novacom and streams them
as MJPEG over HTTP. Compatible with VLC, ffplay, browsers, etc.

Usage:
    ./stream-server.py [--port 8080] [--fps 10]

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

        # Print FPS every 5 seconds
        now = time.time()
        if now - last_fps_time >= 5.0:
            fps = fps_frame_count / (now - last_fps_time)
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
    <title>webOS Screen Share</title>
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
    <h1>webOS Screen Share</h1>
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
        "file:///media/cryptofs/apps/usr/palm/applications/org.webosarchive.screenshare/fbcapture",
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

    parser = argparse.ArgumentParser(description='webOS Screen Share MJPEG Server')
    parser.add_argument('--port', '-p', type=int, default=8080,
                       help='HTTP port (default: 8080)')
    parser.add_argument('--fps', '-f', type=int, default=15,
                       help='Target FPS (default: 15)')
    parser.add_argument('--quality', '-q', type=int, default=50,
                       help='JPEG quality 1-100 (default: 50)')
    parser.add_argument('--low-latency', '-l', action='store_true',
                       help='Low latency mode: quality=30, fps=20')
    parser.add_argument('--force-daemon', action='store_true',
                       help="Force start/stop daemon on device (default: let device app manage it)")
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
                print("Run 'make deploy' first, or launch the Screen Share app on the device")

    # Start frame fetcher thread
    print(f"Starting frame fetcher (target: {args.fps} FPS)...")
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
    server = ThreadedHTTPServer(('', args.port), MJPEGHandler)

    def signal_handler(sig, frame):
        global running
        print("\nShutting down...")
        running = False
        # Call shutdown from a thread to avoid deadlock
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    print(f"\n{'='*60}")
    print(f"webOS Screen Share Server (quality={args.quality}, fps={args.fps})")
    print(f"{'='*60}")
    print(f"Web viewer:  http://localhost:{args.port}/")
    print(f"MJPEG stream: http://localhost:{args.port}/stream")
    print(f"")
    print(f"LOWEST LATENCY (recommended):")
    print(f"  ffplay -fflags nobuffer -flags low_delay -framedrop \\")
    print(f"         http://localhost:{args.port}/stream")
    print(f"")
    print(f"VLC (add low caching):")
    print(f"  vlc --network-caching=50 http://localhost:{args.port}/stream")
    print(f"{'='*60}")
    print(f"Press Ctrl+C to stop\n")

    try:
        server.serve_forever()
    finally:
        running = False
        if args.force_daemon:
            print("Stopping device daemon...")
            stop_daemon()
        print("Server stopped.")


if __name__ == '__main__':
    main()
