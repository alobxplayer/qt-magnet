# qt-magnet

qt-magnet is a desktop utility that forwards `.torrent` files and `magnet:` links to remote torrent clients, with a file selection dialog, category and tag support.

It currently supports qBittorrent, Transmission and Aria2.

Itself, it does not act as standalone torrent client, and requires installed one of the remote torrent clients mentioned earlier.

## Installation

Prebuilt binaries for Windows and Linux are available on the [Releases](../../releases) page.

## Building from source

### Prerequisites

- Qt 6
- CMake
- A C++17 compiler
- libsecret for linux

### Windows

```powershell
.\build.ps1			# build to dist/win_arch
.\build.ps1 -Test			# build and run smoke tests
```

### Linux

```
chmod +x build.sh
./build.sh			# build to dist/linux_arch
./build.sh --test			# build and run smoke tests
```

## Usage

```
qt-magnet "magnet:?xt=urn:btih:…"   add a magnet link (default action)
qt-magnet                            open settings
qt-magnet /register                  register as magnet: handler
qt-magnet /unregister                unregister
qt-magnet /settings                  open settings
qt-magnet /quick    "magnet:…"       quick mode (no dialog)
qt-magnet /dialog   "magnet:…"       force show dialog
qt-magnet /help                      show help
```
