# Windows installer

See [AVC Windows 安装包构建指南](BUILD_INSTALLER.zh-CN.md) for the complete
Release build, runtime bundling, Inno Setup, verification, and signing workflow.

RVC is distributed separately. See
[AVC RVC 扩展安装包构建指南](BUILD_RVC_EXTENSION.zh-CN.md) for the full CPU
extension, ONNX Runtime, model importer, and self-contained converter package.

# Realtime privileges

The engine is only as good as the scheduling class its audio thread gets. `avc`
reports what it actually received on startup; anything other than SCHED_FIFO or
SCHED_RR means the graph will glitch as soon as the machine is busy.

PipeWire's `libpipewire-module-rt` tries two paths, in order:

1. `sched_setscheduler()` directly, which needs `RLIMIT_RTPRIO` on the login
   session. This is the reliable path.
2. rtkit over D-Bus, used when path 1 fails. rtkit caps the priority at 20 and
   can demote the thread again afterwards, so a grant here is not a guarantee.

PipeWire already ships `/etc/security/limits.d/25-pw-rlimits.conf`, which gives
the `@pipewire` group `rtprio 95`. Distributions create the group but leave it
empty, so path 1 fails until someone is added to it:

```bash
sudo usermod -aG pipewire "$USER"
# log out and back in -- PAM limits are applied at login, not at exec
```

Verify:

```bash
ulimit -r          # want 95, not 0
./build/avc --list # then run the engine and read the "audio thread:" line
```

To check any running process by hand:

```bash
for t in /proc/$(pgrep -x avc)/task/*; do
    printf '%-14s %s\n' "$(cat "$t"/comm)" \
        "$(sed 's/.*) //' "$t"/stat | awk '{print "rt_prio="$38" policy="$39}')"
done
# policy: 0=OTHER 1=FIFO 2=RR
```

# Windows desktop editor

On Windows, running `avc.exe` opens the built-in editor in a native WebView2
window. The editor HTML, RPC calls, telemetry stream and extension UI events all
stay inside the process; the default launch does not bind an HTTP port. Windows
11 includes the Evergreen WebView2 Runtime. On supported Windows 10 systems it
can be installed with the Evergreen bootstrapper if AVC reports that it is
missing.

The former browser control plane remains available for development and remote
automation only when explicitly requested with `--http`, `--port=N` or
`--bind=ADDR`. Use `--no-desktop --http` for the old headless/server shape.

# Windows audio capture sources

The Windows capture node accepts three kinds of source from the same picker:

- microphones and other WASAPI capture endpoints;
- render endpoints, captured with WASAPI device loopback (everything currently
  playing through that output);
- visible windows, captured with Windows process loopback (the selected
  window's process and its child processes).

The picker refreshes its device/window list whenever it is opened. Process
loopback requires Windows 10 build 20348 or newer and captures audio only, not
the window's pixels. Its `wasapi-process:<pid>` source is intentionally treated
as transient. If that process exits, the graph keeps running and the capture
node produces silence; reopen the picker and select the application again after
it restarts. Device loopback remains available on older supported Windows versions.

# Windows USB/IP virtual audio

The Windows backend publishes virtual microphones and speakers through a local
USB/IP server and the signed `usbip-win2` driver. Windows x64 builds download
the official 0.9.7.7 installer at configure time, verify its pinned SHA-256,
and ship it under `bin\drivers`. AVC verifies the same digest again before
launching it; it does not build or sign a kernel driver itself.

1. In the editor's **音频设备 → USB/IP 驱动** section, choose **安装** and
   approve the administrator prompt. Installation briefly resets all USB 3
   hubs, so finish important USB audio/video/storage work first.
2. Reboot Windows if the driver remains unavailable after installation.
3. Add a `virtual_mic` or `virtual_speaker` node. On the
   first graph activation, approve the single Windows administrator prompt.

Set `AVC_BUNDLE_USBIP_DRIVER_INSTALLER=OFF` to build without downloading or
redistributing the installer. Non-x64 builds currently omit it because upstream
0.9.7.7 publishes no other installer asset. In that case, install a supported
signed release manually and leave `usbip.exe` on `PATH` or under one of the
standard `Program Files\USBip` / `Program Files\usbip-win2` locations. Do not
install 0.9.7.8.

AVC listens only on `127.0.0.1:3240`. Each graph node is exported as one UAC1
USB 2.0 high-speed device using the Windows in-box `usbaudio.sys` class driver. A UAC1
device contains both playback and recording endpoints; AVC connects the
playback endpoint for `virtual_speaker` and the recording endpoint for
`virtual_mic`. The unused endpoint remains silent.

Current limits are signed 16-bit PCM, 8–192 kHz, 1–32 channels and at most 32
devices. A format must fit USB 2.0's 3072-byte high-speed isochronous budget per
millisecond—for example, 32 channels at 48 kHz or 16 channels at 96 kHz.
Renames and format changes are applied by detaching only the obsolete bus and
attaching its replacement; restarting the engine does not remove devices.
AVC refuses exactly `usbip-win2` 0.9.7.8 before attaching a device because that
release can corrupt memory and cause a BSOD. Version 0.9.7.7 and releases other
than 0.9.7.8 are not rejected by this version check.

For diagnosis, run these commands in an elevated terminal:

```powershell
usbip.exe list -r 127.0.0.1
usbip.exe port
```

AVC changes only the requested bus IDs whose port description points at
`usbip://127.0.0.1:3240/`. It does not alter remote USB/IP connections.
