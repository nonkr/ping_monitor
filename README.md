# Ping Monitor 2026.06.19

Win11 minimal ping status monitor. It is a native Win32 C++ program that builds into a single portable `.exe` and uses only Windows system APIs.

The executable includes an embedded app icon, a Common Controls v6 manifest, Win11-style spacing, rounded input surfaces, owner-drawn buttons, and rounded windows. The window title includes `Powered by nonkr`.

## Build

Install Microsoft Visual Studio Build Tools with **Desktop development with C++**, then run:

```bat
build.bat
```

The output file is:

```text
build\PingMonitor.exe
```

## Use

1. Run `PingMonitor.exe`.
2. Enter one IPv4 address per line.
   Lines can be commented with `#`, either at the start of a line or after an address.
3. Set interval and timeout in milliseconds. Defaults are `1000` and `2000`.
4. Click `开始`.

The settings window is hidden after monitoring starts. The compact monitor window has no title bar or window buttons. It flashes a dot whenever a ping starts, then shows green when online, red when failed, yellow when the recent checks are flapping between online and offline, and gray when monitoring is stopped.

Use the tray icon or compact window right-click menu to show or hide mini ping latency sparklines under each IP. Double-click the compact window to stop monitoring, hide it, and show settings again. Dragging the compact window outside the screen snaps it back to the nearest screen edge; when it is on an edge, it collapses to status dots with the last IP segment. The right-click menu also controls always-on-top, start, stop, and exit.

The always-on-top and sparkline visibility settings are saved in the config file.

Configuration is stored under:

```text
%APPDATA%\PingMonitor\config.txt
```
