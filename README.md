# File-Hunter Browser tool for MSX  —  fhMOD fork

> Personal fork of [nataliapc/msx_filehunterbrowser](https://github.com/nataliapc/msx_filehunterbrowser)
> with a richer local file browser, .CAS launching, OCMINFO key, and a few
> UI polishes. All upstream functionality is preserved.

**File-Hunter Browser** is a file browser and downloader that connects your MSX computer directly to the [file-hunter.com](https://file-hunter.com) MSX database.

You can search, browse and download ROM cartridges, disk images, cassette files and music directly to your MSX system over a network connection.

![FH Browser Screenshot](assets/screenshot.png)

## Table of Contents
- [What this fork adds](#what-this-fork-adds)
- [Features](#features)
- [Requirements](#requirements)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Local file browser](#local-file-browser)
- [Search Filters](#search-filters)
- [Command Line Usage](#command-line-usage)
- [How to compile](#how-to-compile)
- [Thanks](#thanks)
- [License](#license)
- 🌟 [More stars!](#more-stars) 🌟

## What this fork adds

This fork stays compatible with upstream's network panels (ROM/DSK/CAS/VGM) and CLI flags, and adds:

| Area | Change |
|---|---|
| **F5 key** | Launches `OCMINFO.COM` (must be on the DOS PATH) instead of `SR.COM /S`. |
| **Local browser** | Full filesystem browser on the `[L]oc` tab, no network needed. |
| **`.CAS` support** | ENTER on a `.CAS` file boots into BASIC with LOADCAX so the cassette image plays back through the BIOS tape hooks — no manual `BASIC … BLOAD"LOADCAX"…` dance. |
| **`[..]` and `[\]`** | Synthetic navigation entries at the top of the local browser to jump one level up / to the drive root. |
| **R / D / C keys** | Work as panel shortcuts even while the local browser is open (close it and switch to ROM / DSK / CAS). |
| **F1 in local browser** | Pops the help window without leaving the local browser. |
| **"Loading game…" box** | Centred frame shown immediately when ENTER is pressed on a ROM/DSK/CAS, so the user gets feedback while LOADCAX/SROM/SRI start. |
| **Layout fix** | The first entry in the local browser used to render on row 6, leaving row 5 permanently blank. Aligned with the network panels (first entry on row 5). |
| **Footer cleanup** | `RET:Sel` removed (its meaning varied per panel and was misleading), and `F5:SofaRun` updated to `F5:OCMINFO`. |

The compiled binary is still produced as `fhMOD.com` and can replace upstream's binary 1:1.

## Features
- **Direct file-hunter.com access**: Browse and download files from the largest MSX file database
- **Multiple file types**: Support for ROM, DSK, CAS and VGM files
- **MSX generation filtering**: Filter content by MSX1, MSX2, MSX2+ or Turbo-R compatibility
- **Search functionality**: Text-based search with real-time filtering
- **Network download**: Direct download to your MSX system via UNAPI TCP/IP
- **Local filesystem browser**: Browse and launch ROM / DSK / CAS / COM / BAS files from your storage without the network
- **MSX2 optimized interface**: 80-column text mode with tabbed navigation

## Requirements
- MSX2 or higher
- MSX-DOS 2.x (or Nextor)
- UNAPI-compatible network device (only required for the network panels — the local browser works without it)
- For ROM/DSK launching: [SROM and SRI](https://www.msx.org/wiki/SofaRun) on the PATH (or `\UTILS`)
- For CAS launching: [LOADCAX](https://github.com/k0gaMSX/legacy/tree/master/APPS/CASUTILS/LOADCAX) in `A:\UTILS\LOADCAX` (the file with no extension, as distributed in the repo)
- For F5 (OCMINFO): `OCMINFO.COM` on the PATH

## Keyboard shortcuts

Global shortcuts (work on any tab):

| Key | Action |
|---|---|
| F1 | Help window |
| F2 | Search by text (network panels) |
| F3 | (on Local tab) Open / re-open the local browser |
| F4 | Switch File-Hunter server |
| F5 | Launch `OCMINFO.COM` |
| `R` / `D` / `C` / `L` | Jump to ROM / DSK / CAS / Local panel |
| Tab / Shift+Tab | Cycle panels |
| ENTER | (network) Download file. (local) Open dir / launch file. |
| ESC | Back / exit |

## Local file browser

Open with the `[L]oc` tab (or `L`). Inside:

| Key | Action |
|---|---|
| ↑ / ↓ | Move selection |
| ENTER | Open directory · `[..]` parent · `[\]` root · `.ROM/.DSK/.CAS/.COM/.BAS` launch |
| ESC | Go up one level (or close at root) |
| F1 | Help |
| F5 | Launch OCMINFO.COM |
| R / D / C / Tab | Close local browser and jump to that panel |

**ROM**: launched via `SROM` with auto-detected mapper (`Linear`, `ASCII16`, `ASCII16-X`, `ASCII8` depending on size and header).

**DSK**: launched via `SRI` (SofaRunIt).

**CAS**: handled differently because a cassette image isn't an executable — it's a raw byte dump that only BASIC's tape routines can play back. The browser:
1. Copies `A:\UTILS\LOADCAX` next to the `.CAS` (skips if already there).
2. Drops a one-line stub `FHCAS.BAS` containing `10 BLOAD"LOADCAX",R'<basename>`.
3. Queues `BASIC FHCAS.BAS` in the BIOS keyboard buffer and exits to COMMAND.COM, which boots BASIC and autoruns the stub. LOADCAX hooks the cassette BIOS, BASIC's `RUN"CAS:"` reads bytes from the `.CAS`, and the game starts.

**COM / BAS**: just queued for direct execution.

In all cases the screen shows a centred "Loading game…" frame as soon as ENTER is pressed, so there's no impression of a hang while the file IO + DOS handoff happens.

## Search Filters

### File Types
- **ROM**: Cartridge images (ROM files)
- **DSK**: Disk images (floppy disk files)
- **CAS**: Cassette tape images
- **VGM**: Video Game Music files

### MSX Generation
- **All**: No generation filtering
- **1**: MSX1 compatible files only
- **2**: MSX2 compatible files only
- **2+**: MSX2+ compatible files only
- **Turbo-R**: MSX Turbo-R compatible files only

## Command Line Usage

Options to open the browser with a specific search configuration:


```bash
FHMOD [/H][/M <gen>][/S <search>][/P <panel>]
```



### Options
- `/H` - Show help message and exit
- `/M <gen>` - Set MSX generation filter:
  - `1` - MSX1
  - `2` - MSX2
  - `2+` - MSX2+
  - `turbo-r` - MSX Turbo-R
- `/P <panel>` - Set initial file type panel:
  - `rom` - ROM cartridges
  - `dsk` - Disk images
  - `cas` - Cassette files
  - `vgm` - Music files
- `/S <search>` - Set initial search string

### Examples

```bash
FHMOD                             # Default search
FHMOD /M 2 /P dsk /S "konami"    # Search MSX2 disk images containing "konami"
FHMOD /P rom /S "gradius"         # Search ROM files containing "gradius"
FHMOD /M turbo-r                  # Browse Turbo-R compatible files
```



## How to compile

### Prerequisites to compile
- Linux
- Docker (to run SDCC toolchain)
- OpenMSX (optional, for emulation/testing)

### Folder Structure

```
/bin           # helper scripts and tools
/contrib       # bundled libraries (UNAPI TCP/IP, etc)
/externals     # external projects (sdcc_msxdos, sdcc_msxconio)
/includes      # public headers
/libs          # compiled .lib archives
/src           # project source code
/obj           # build output (.rel, .ihx, .com)
/dsk           # generated disk images
```



### Compilation

```bash
git clone --recurse-submodules https://github.com/Paranosh/msx_filehunterbrowser.git
cd msx_filehunterbrowser
make all
```

A GitHub Actions workflow (`.github/workflows/build.yml`) builds `fhMOD.com` on every push and uploads it as an artifact, so you can grab a fresh binary without setting up SDCC locally.


## Thanks
Thanks to **Natalia Pujol Cremades (nataliapc)** for the original [msx_filehunterbrowser](https://github.com/nataliapc/msx_filehunterbrowser) — this fork only adds the extras listed above on top of her work.

Also thanks to Arnaud de Klerk, @leomanes, @skillax, @ducasp, and @konamiman.

## License
This project is licensed under the [MIT License](LICENSE), inherited from the upstream project.

## More stars!

If you like this fork, please give the **[upstream project](https://github.com/nataliapc/msx_filehunterbrowser)** a star — that's where the heavy lifting happens.

[![Star History Chart](https://api.star-history.com/svg?repos=nataliapc/msx_filehunterbrowser&type=Date&theme=dark)](https://www.star-history.com/#nataliapc/msx_filehunterbrowser&Date)
