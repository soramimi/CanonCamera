# Canon Wi-Fi Photo Transfer

A small Windows app that transfers photos from a Wi-Fi–connected Canon EOS
camera. It speaks the same **PTP/IP** protocol as Canon's *EOS Utility*, but
implemented from scratch in C++ — no libgphoto2, no Canon SDK.

Verified against a **Canon EOS R10** in *EOS Utility* connection mode.

## What it does

- Connects to the camera over Wi-Fi (PTP/IP, TCP port 15740).
- Lists the images on the card, with optional filtering (all / JPEG only / RAW only).
- Downloads a selected image into memory.
- Previews JPEGs in a Qt GUI (double-click a row).

The heavy lifting lives in a reusable, Qt-independent library class
(`CanonCamera`); the Qt widgets are only the front end.

## Screens / usage

The GUI is intentionally minimal:

1. Launch `bin/canon.exe`.
2. Click the button to connect and list JPEG images from the camera.
3. Double-click a row to download that image and show it in the preview pane.

> The camera IP is currently hard-coded to `192.168.1.2` in the GUI.
> Put the camera into *EOS Utility* connection mode and on the same LAN first.

## Building

Requirements:

- **Qt 6** (built/verified with 6.12, MSVC 2022 64-bit)
- **MSVC** toolchain (Visual Studio Build Tools)
- Windows (uses Winsock2)

Open `canon.pro` in Qt Creator and build, or from a Qt/MSVC command prompt:

```
qmake canon.pro
nmake
```

The output is `bin/canon.exe`.

> **Note:** sources are UTF-8 with Japanese comments. MSVC must be given
> `/utf-8` (already set in `canon.pro`), otherwise it misreads them as CP932
> and fails to compile.

## Project layout

| Path | Role |
|---|---|
| `canoncamera.h` / `canoncamera.cpp` | Core library: the `CanonCamera` class implementing PTP/IP. Qt-independent. |
| `main.cpp` | Qt application entry point. |
| `src/MainWindow.*` | Main window: connect button, image table, preview pane. |
| `src/ImageView.*` | Widget that draws a `QImage` scaled to fit. |
| `src/MemoryReader.*` | `QIODevice` adapter so `QImage` can decode an in-memory buffer. |
| `canon.pro` | qmake project. |
| `ptpip_prototype.cpp` | Original standalone prototype. Kept for reference; not built. |
| `AGENTS.md` | Protocol notes and implementation gotchas (Japanese). |

## Library API

```cpp
CanonCamera cam;
if (cam.open("192.168.1.2")) {
    for (const CanonCamera::Item &it : cam.list(CanonCamera::Filter::Jpeg)) {
        if (auto data = cam.get(it)) {
            // *data is std::vector<uint8_t> holding the image bytes
        }
    }
    cam.close();
}
// No exceptions are thrown; failures return false / empty / std::nullopt,
// and cam.lastError() holds the reason.
```

- `Item` — `handle / storageId / format / size / filename`, plus `isJpeg()` / `isRaw()`.
- `list(Filter)` — enumerate image files (folders excluded). `Filter::{All, Jpeg, Raw}`.
- `get(Item)` — download an object; returns `std::optional<std::vector<uint8_t>>`.
- `open()` / `close()` / `isOpen()` / `setClientGuid()` / `lastError()`.

## Status & limitations

- Single-shot download into memory (fine for multi-MB CR3/JPEG); no chunking or resume yet.
- `list()` and `get()` run synchronously on the calling thread — the GUI blocks
  during enumeration/download (threading is a planned improvement).
- RAW (CR3) files download correctly but cannot be previewed (Qt can't decode CR3).
- Camera IP is hard-coded in the GUI.

See [AGENTS.md](AGENTS.md) for the protocol details and the reverse-engineering
notes behind this implementation.
