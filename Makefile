# LuneCast - Makefile
# Cross-compilation for HP TouchPad (ARMv7)

# Toolchain - use system Linaro GCC
LINARO_GCC ?= /opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi
ARM_PREFIX = arm-linux-gnueabi
CC = $(LINARO_GCC)/bin/$(ARM_PREFIX)-gcc
STRIP = $(LINARO_GCC)/bin/$(ARM_PREFIX)-strip

# PalmPDK paths
PALM_PDK = /opt/PalmPDK
PDK_INCLUDE = $(PALM_PDK)/include
PDK_LIB = $(PALM_PDK)/device/lib

# Compiler flags - match device ABI
CFLAGS = -Wall -Wextra -std=c99 -O2
CFLAGS += -march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp
CFLAGS += -I$(PDK_INCLUDE) -I$(PDK_INCLUDE)/SDL
CFLAGS += -D_GNU_SOURCE

# Linker flags
LDFLAGS = -L$(PDK_LIB)
LDFLAGS += -Wl,-rpath-link,$(PDK_LIB)
LDFLAGS += -Wl,--allow-shlib-undefined

# Package info
APP_ID = org.webosarchive.lunecast
VERSION = 1.0.0

# Targets
DAEMON = fbcapture
APP = lunecast
INPUT = lunecast-input

.PHONY: all clean strip deploy test package install help app daemon input

# Build everything
all: $(DAEMON) $(APP) $(INPUT)

# Build just the daemon
daemon: $(DAEMON)

# Build just the app
app: $(APP)

# Build just the input helper (EXPERIMENTAL remote tap injection)
input: $(INPUT)

# Daemon (fbcapture)
$(DAEMON): fbcapture.o
	$(CC) $(LDFLAGS) -o $@ $^ -ljpeg

fbcapture.o: fbcapture.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Input helper - no SDL/PDK, just libc + librt for clock_gettime
$(INPUT): lunecast-input.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lrt

# App (lunecast)
$(APP): screenshare-app.o
	$(CC) $(LDFLAGS) -o $@ $^ -lSDL -lSDL_ttf -lSDL_image -lpdl

screenshare-app.o: screenshare-app.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Strip both binaries
strip: $(DAEMON) $(APP) $(INPUT)
	$(STRIP) $(DAEMON) $(APP) $(INPUT)

# Clean build artifacts
clean:
	rm -f *.o $(DAEMON) $(APP) $(INPUT)
	rm -f package/$(DAEMON) package/$(APP) package/$(INPUT)
	rm -f *.ipk

# Deploy daemon to /media/internal for testing
deploy-daemon: $(DAEMON)
	$(STRIP) $(DAEMON)
	@echo "Deploying fbcapture to device..."
	novacom put file:///media/internal/fbcapture < $(DAEMON)
	novacom run file://bin/chmod -- +x /media/internal/fbcapture
	@echo "Deployed to /media/internal/fbcapture"

# Legacy alias
deploy: deploy-daemon

# Run a single capture test
test: deploy-daemon
	@echo "Running single capture test..."
	novacom run file:///media/internal/fbcapture -- -o /media/internal/screen.jpg -q 75
	@echo "Fetching screenshot..."
	novacom get file:///media/internal/screen.jpg > screen_test.jpg
	@echo "Screenshot saved to screen_test.jpg"
	@ls -la screen_test.jpg
	@file screen_test.jpg

# Start daemon on device (background)
start-daemon: deploy-daemon
	@echo "Starting capture daemon on device..."
	novacom run file:///media/internal/fbcapture -- -D -i 100 -o /media/internal/screen.jpg
	@sleep 0.5
	@novacom run file://bin/pidof -- fbcapture || echo "Warning: daemon may not have started"
	@echo "Daemon started."

# Stop daemon on device
stop-daemon:
	@echo "Stopping capture daemon..."
	-novacom run file://bin/killall -- fbcapture 2>/dev/null || true
	@echo "Daemon stopped."

# Package for webOS installation
package: $(DAEMON) $(APP) $(INPUT) strip
	@echo "Creating IPK package..."
	@mkdir -p package
	@cp $(DAEMON) package/
	@cp $(APP) package/
	@cp $(INPUT) package/
	palm-package package -o .
	@echo "Package created:"
	@ls -la *.ipk

# Install package to device
install: package
	palm-install *.ipk
	@echo ""
	@echo "App installed! Find 'LuneCast' in the launcher."

# Uninstall from device
uninstall:
	-palm-install -r $(APP_ID) || true

# Show help
help:
	@echo "LuneCast Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all           - Build daemon and app"
	@echo "  daemon        - Build fbcapture daemon only"
	@echo "  app           - Build lunecast app only"
	@echo "  strip         - Strip debug symbols"
	@echo "  clean         - Remove build artifacts"
	@echo ""
	@echo "Testing (daemon only):"
	@echo "  deploy-daemon - Copy daemon to /media/internal"
	@echo "  test          - Deploy and capture single screenshot"
	@echo "  start-daemon  - Start daemon in background"
	@echo "  stop-daemon   - Stop daemon"
	@echo ""
	@echo "Installation:"
	@echo "  package       - Create IPK package"
	@echo "  install       - Install IPK to device"
	@echo "  uninstall     - Remove from device"
	@echo ""
	@echo "Toolchain: $(LINARO_GCC)"
	@echo "PalmPDK: $(PALM_PDK)"
