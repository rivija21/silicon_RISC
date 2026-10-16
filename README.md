# Silicon Path

Follow a C program all the way down to the hardware: tokens → syntax tree → symbols → IR and
optimisation passes → RISC-V assembly → machine code → linking → a 5-stage pipeline with caches.
Everything is implemented from scratch in Swift, following Patterson & Hennessy (RISC-V edition).

## Build and run

```
./build.sh
open "Silicon Path.app"
```

Needs only the Xcode Command Line Tools.

## Layout

| Folder | What is in it |
| --- | --- |
| `Sources/SiliconCore/Frontend` | lexer + preprocessor, parser, AST, semantic analysis, reference interpreter |
| `Sources/SiliconCore/IR` | three-address IR, lowering from the AST, the 7 optimiser passes |
| `Sources/SiliconCore/Backend` | register allocation, code generation, ISA tables, assembler, linker |
| `Sources/SiliconCore/Machine` | memory, ISA simulator, 5-stage pipeline, caches |
| `Sources/SiliconCore/Runtime` | crt0, syscall wrappers, the small C library |
| `Sources/SiliconUI` | the app's views (one per station) |
| `Sources/spcheck` | test driver: hand tests and the random program generator |
| `Sources/uishot` | renders the window offscreen; `uishot verify` checks the examples |

## Testing

```
swift build -c release --product spcheck
.build/release/spcheck hand          # hand-written tests, checked against native clang
.build/release/spcheck fuzz 300 1    # 300 random programs × 18 build variants × 5 pipeline configs
.build/release/spcheck clang 100 1   # random programs, also checked against native clang
```
