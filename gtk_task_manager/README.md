# GTK Windows Task Manager (C)

A native Windows desktop task-manager starter built with **C + GTK 3 + Win32 APIs**.

## Features
- Live process enumeration (refreshes every 3 seconds)
- Search by executable name or PID
- Per-process working-set memory
- System-wide CPU usage sampling
- Physical memory usage
- Process details dialog
- End Task with confirmation
- Handles restricted processes gracefully

## Requirements
- Windows 10 or Windows 11
- MSYS2
- MinGW-w64 UCRT64 toolchain
- GTK 3 development package

## 1. Install MSYS2 packages

Open the **MSYS2 UCRT64** terminal and run:

```bash
pacman -Syu
```

Restart the terminal if requested, then:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-gtk3 mingw-w64-ucrt-x86_64-pkgconf
```

## 2. Build

From the project directory in the UCRT64 terminal:

```bash
gcc -Wall -Wextra -O2 src/main.c -o task_manager.exe \
  $(pkg-config --cflags --libs gtk+-3.0) \
  -lpsapi
```

## 3. Run

```bash
./task_manager.exe
```

For full process termination permissions, launch the terminal or executable as **Administrator**. Do not terminate Windows system processes unless you understand the consequences.

## Project structure

```text
gtk_task_manager/
├── src/
│   └── main.c
├── Makefile
└── README.md
```

## Important implementation notes

- Process listing uses `CreateToolhelp32Snapshot`, `Process32FirstW`, and `Process32NextW`.
- Process memory uses `GetProcessMemoryInfo` and reports the working set, not private bytes.
- CPU usage is system-wide and calculated from `GetSystemTimes` deltas.
- Windows may deny memory-query or termination access for protected/system processes. Such rows remain visible.
- The app is a practical learning project, not a replacement for every feature in Microsoft's Task Manager. Per-process CPU graphs, services, startup apps, GPU engine usage, and detailed performance history are not implemented yet.
