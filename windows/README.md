# Silicon Path for Windows

A native Windows port of Silicon Path in C++20. The compiler, assembler, linker, simulators and
every station of the app work the same as on macOS. The UI uses plain Win32, Direct2D and
DirectWrite, so it needs no frameworks or runtime DLLs.

## Build

1. Install [MSYS2](https://www.msys2.org/), then in the **MSYS2 UCRT64** shell:

   ```
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make
   ```

2. Build from this folder (from the UCRT64 shell, or any shell with `C:\msys64\ucrt64\bin` on `PATH`):

   ```
   mingw32-make -j8
   ```

3. Run `build\SiliconPath.exe`. You can also pass a `.c` or `.s` file to open it.

Everything is linked statically. You can copy the `.exe` files to any Windows 10 or 11 PC and run
them there without MSYS2.

| Target | What it does |
| --- | --- |
| `mingw32-make` | builds `SiliconPath.exe`, `spcheck.exe` and `uishot.exe` into `build/` |
| `mingw32-make check` | runs the built-in examples on both CPU models, then the hand-written tests |
| `mingw32-make icon` | redraws `res/app.ico` (the same design as `scripts/make_icon.swift`) |
| `mingw32-make clean` | removes `build/` |

## Layout

| Folder | What is in it |
| --- | --- |
| `src/core` | the whole toolchain and machine: lexer, parser, sema, IR + passes, register allocation, codegen, assembler, linker, ISA simulator, pipeline, caches, interpreter, C runtime |
| `src/ui` | the view toolkit (views, scroll and split views, text editing) on Direct2D/DirectWrite, plus one view per station |
| `src/app` | `main.cpp` (window, menus, shortcuts, file dialogs) and `uishot.cpp` |
| `src/spcheck` | test driver: hand tests and the random program generator |
| `res` | icon, manifest (per-monitor DPI v2, UTF-8) and version resource |
| `tools` | `make_icon.cpp` |

The port follows the Swift sources file by file. For example, `src/core/Parser.cpp` matches
`Sources/SiliconCore/Frontend/Parser.swift`. A fix on one side should be easy to carry over.

## Testing

```
build\spcheck.exe verify          # every example on the ISA simulator, the pipeline and the interpreter
build\spcheck.exe hand            # hand-written tests, checked against native gcc
build\spcheck.exe fuzz 300 1      # 300 random programs × 18 build variants × 5 pipeline configs
build\spcheck.exe native 100 1    # random programs, also checked against native gcc
```

The native compiler is `gcc` from `PATH`. Set `SPCHECK_CC` to use another one, such as `clang`.

`uishot` renders the window offscreen to a PNG after running a script of commands. Use it to check
layouts without a screen:

```
build\uishot.exe out.png example:1 station:8 caches:1 cycles:40
build\uishot.exe out.png example:1 station:8 bench:120     # time 120 frames of playback
```

The comment at the top of `src/app/uishot.cpp` lists every command.

## Notes

- Settings (the current program, options and window placement) are saved in
  `%APPDATA%\SiliconPath\settings.ini`.
- Set `SILICONPATH_LOG` to a file path to log each paint's size and time.
- Shortcuts use Ctrl where the Mac uses ⌘. F5 runs or pauses, Ctrl+J/K/L steps a cycle, an
  instruction or a C line, and Ctrl+1…Ctrl+0 switches stations. The menus list them all.
