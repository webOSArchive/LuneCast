# Remote input - the hidd plugin route (SUPERSEDED)

> **This approach is no longer used. Do not follow the install steps below.**
>
> `lunecast-input`, shipped in the IPK, sends touch events straight to the
> socket LunaSysMgr binds. It needs no plugin, no file in `/usr/lib`, no edit
> to `/etc/hidd/HidPlugins.xml` and no hidd restart, and `palm-uninstall`
> removes it completely. Its worst failure is a tap that does nothing, rather
> than an input daemon that crash-loops and leaves the tablet with no
> touchscreen.
>
> This document is kept because the investigation below is how the wire format
> and the CLOCK_MONOTONIC requirement were found, and because the hidd plugin
> ABI is not documented anywhere else. The dead ends are worth knowing too.

Enable remote input with:

```bash
./start-stream.py --enable-input
```

The viewer page at `/` then makes the image clickable. `/stream` stays what the
device's on-screen instructions point at; input needs the page because you need
an element to attach handlers to.

---

## Why a hidd plugin, and not something simpler

The TouchPad's touchscreen is **not an evdev device**. It is absent from
`/proc/bus/input/devices`; only `gpio-keys`, `pmic8058_pwrkey` and `headset`
are there. Touch arrives over `/dev/ctp_uart` + `/dev/i2c-5` into `hidd`, which
forwards it to LunaSysMgr - and LunaSysMgr holds no input fd at all, only
`fb0`, `fb1`, `kgsl*` and `pmem_smipool`.

```
/dev/ctp_uart ──> hidd (libhidtouchpanel.so) ──> TouchpanelEventSocket ──> LunaSysMgr
/dev/input/*  ──> hidd (libhidinputdev.so)   ──> InputDevEventSocket   ──> LunaSysMgr
```

Things that were tried and do **not** work:

- **uinput.** A virtual touchscreen is created fine and `hidd` even opens it
  (its InputDev plugin hotplug-scans), but the events land on
  `InputDevEventSocket`, which LunaSysMgr treats as HID/keyboard. Neither
  single-touch nor multitouch protocol A produced any response.
- **TouchpanelCmdSocket.** Disassembling `_PluginCommandCallback` shows it
  reads a 4-byte command code and accepts only 1..4, dispatching to
  `SuspendPlugin` / `ResumePlugin` / `ExitPlugin`. It is lifecycle control. The
  only thing you can do through it is turn touch *off*.
- **The touchpanel LS2 service** (`com.palm.casper.touchpanel`) exposes
  `_SubscribeToRawData` / `_UnsubscribeFromRawData` - you can *read* touch
  data, not inject it.
- **Gremlins.** `hidd --help` has four Gremlin options and `libhid.so` exports
  `HidGenerateRandomEvents`, but no gremlin plugin ships on a retail device,
  LunaSysMgr has no gremlin support, and there is nothing gremlin-related
  anywhere in the Palm SDK or PDK.

What does work is `hidd`'s own plugin interface. `HidPlugins.xml` already ships
two plugins sharing one event socket (`HidKeypad` and `HidAvrcp` both publish
to `KeypadEventSocket`), so a new plugin can publish alongside the real
touchpanel one.

## The plugin ABI

Recovered from `/usr/bin/hidd`, whose symbols are intact. hidd `dlopen`s the
`.so` and `dlsym`s **`PluginTable`**, an array of 6 function pointers:

| Offset | Called by | Purpose |
|--------|-----------|---------|
| +0 | `main` | `SetReportCallback(fn)` - hidd passes `&ReportEvent` |
| +4 | `main` | `Init(ctx)` - `ctx[1]` is your plugin index; return 0 = success |
| +8 | `ExitPlugin` | `Exit()` |
| +12 | `SuspendPlugin` | `Suspend()` |
| +16 | `ResumePlugin` | `Resume()` |
| +20 | `_PluginCommandCallback` | `Command()` |

```c
int ReportEvent(void *events, int count, int type, int pluginIndex);
```

`type 0` tail-calls `_ReportStandardEvent`, which reads each element as
`ldm{tv_sec,tv_usec}; ldrh +8; ldrh +10; ldr +12` - a plain 16-byte Linux
`struct input_event`. Plugin info structs are 368 bytes apart; the dlopen
handle lands at +272 and the `PluginTable` pointer at +276.

## The wire format

Static analysis got this **wrong twice**. It was settled by binding
`HidTouchpanel`'s event socket (see `touchcap.c`) and recording a real finger:

```
DOWN (5 events): type7/0/id, BTN_TOUCH=1, ABS_X, ABS_Y, SYN_REPORT
MOVE (4 events): type7/0/id,              ABS_X, ABS_Y, SYN_REPORT
UP   (5 events): type7/0/id, ABS_X, ABS_Y, BTN_TOUCH=0, SYN_REPORT
```

- `type 7` is a webOS-specific contact/finger id, value from the plugin's
  per-finger context. Finger id 0 is valid.
- There **is** a trailing `SYN_REPORT`. `CreateFingerUpEvent` does not add it,
  which is what made reading that function alone misleading.
- `BTN_TOUCH` comes *before* the coordinates on down, *after* them on up.
- Real moves arrive about 10ms apart.

### The part that actually blocked it

**Timestamps must be `CLOCK_MONOTONIC`.** The captured stream carries
`tv_sec` around 47 while the wall clock read 1788459443 - a completely
different time domain. `libhid.so`'s `HidGetTimeStamp` disassembles to
`clock_gettime(CLOCK_MONOTONIC, ...)`. LunaSysMgr does gesture timing (tap vs
hold, flick velocity) on these values, so events stamped with `gettimeofday()`
are 1.7 billion seconds adrift of the stream they join and get discarded
silently. Fixing this made taps work on the first attempt, with a format that
had already been correct for two rounds.

## Install

Requires the Linaro cross-compiler and PalmPDK (see the main README).

```bash
ARM=/opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi/bin/arm-linux-gnueabi
$ARM-gcc -O2 -fPIC -shared -march=armv7-a -mfpu=neon -mfloat-abi=softfp \
    -D_GNU_SOURCE -o liblunecastinject.so lunecast_inject.c -lpthread -lrt
$ARM-gcc -O2 -march=armv7-a -mfpu=neon -mfloat-abi=softfp -o tapsend tapsend.c

# BACK UP THE STOCK CONFIG FIRST - to a partition that survives a Doctor
novacom get file:///etc/hidd/HidPlugins.xml > HidPlugins.xml.stock
novacom put file:///media/internal/HidPlugins.xml.orig < HidPlugins.xml.stock

novacom put file:///usr/lib/liblunecastinject.so < liblunecastinject.so
novacom run file://bin/chmod -- 755 /usr/lib/liblunecastinject.so
novacom put file:///media/internal/tapsend < tapsend
novacom run file://bin/chmod -- +x /media/internal/tapsend
```

Then add to `/etc/hidd/HidPlugins.xml`, before `</plugins>`:

```xml
    <plugin>
        <name>LuneCastInject</name>
        <eventsDeferIdle>true</eventsDeferIdle>
        <eventSocketAddress>/var/run/hidd/TouchpanelEventSocket</eventSocketAddress>
        <cmdSocketAddress>/var/run/hidd/LuneCastCmdSocket</cmdSocketAddress>
        <path>/usr/lib/liblunecastinject.so</path>
    </plugin>
```

and restart hidd (upstart respawns it):

```bash
novacom run file://bin/kill -- $(novacom run file://bin/pidof -- hidd)
```

`/media/internal/lunecast-plugin.log` should then show `SetReportCallback`,
the init line with the plugin index, and the inject socket being bound.
Note `killall` does not exist on the device; kill by pid.

## Removal

```bash
novacom run file://bin/cp -- /media/internal/HidPlugins.xml.orig /etc/hidd/HidPlugins.xml
novacom run file://bin/rm -- -f /usr/lib/liblunecastinject.so /media/internal/tapsend
novacom run file://bin/kill -- $(novacom run file://bin/pidof -- hidd)
```

## Known limitations

- **novacom contention.** Each tap spawns a short `novacom run` while the frame
  stream already holds a session open. Fine at human click rates, but the right
  fix is to carry taps on the existing pipe by having `fbcapture -S` read
  commands on stdin.
- **Taps only.** No drag, no multi-touch, no gestures. `send_move()` exists in
  the plugin and the captured format shows how a drag would work.
- A bad plugin means hidd crash-loops and the tablet has **no touch input**
  until the stock XML is restored. novacom runs as root independently of the
  UI, so this is recoverable - but back up first.

## Files

| File | Purpose |
|------|---------|
| `lunecast_inject.c` | The hidd plugin. Binds `/var/run/hidd/LuneCastInject`, takes `{kind,p1,p2}` datagrams: kind 0 = tap(x,y), kind 1 = key(code,value) |
| `tapsend.c` | Device-side helper that sends one of those datagrams |
| `touchcap.c` | The recorder used to capture the real wire format |
| `HidPlugins.xml.stock` | Pristine config, for reference and recovery |
