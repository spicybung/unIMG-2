# unIMG 2

`unimg.exe` rebuilds WRLD/tex files and recovers exact resource payloads from companion IMG archives used by **Grand Theft Auto: Liberty City Stories** and **Grand Theft Auto: Vice City Stories**.

## How to use

Place the matching `.lvz` and `.img` files together, then drag the `.lvz` onto `unimg.exe`, or run:

```bat
unimg beach.lvz
```

The matching IMG is found from the LVZ stem, such as `beach.lvz` + `beach.img`.

## Game selection

Automatic detection is the default:

```bat
unimg beach.lvz --game auto
```

You can force either Stories layout:

```bat
unimg indust.lvz --game lcs
unimg beach.lvz --game vcs
```

## Exact resource recovery

To focus recovery on known missing resource IDs:

```bat
unimg beach.lvz --game vcs --wanted 1881,1828,1802
```

For targeted IDs, the program tests the known exact IMG pointer forms, including the continuation-relative `pointer - 0x20` case used by LVZ-empty resources that actually continue in IMG.

To test all supported pointer forms for every resource row:

```bat
unimg beach.lvz --all-pointer-variants
```

This can generate duplicates, so targeted recovery is the safer default.

## Options

```text
--img <file.img>            Explicit companion IMG path
--out <folder>              Explicit output directory
--game auto|lcs|vcs         Select or auto-detect the Stories layout
--wanted <id,id,...>        Restrict/report exact resource IDs
--all-pointer-variants      Test all supported pointer interpretations
--no-continuations          Skip rebuilt WRLD/xet continuation files
--no-resources              Skip resource payload files; still write CSVs
--resources-only            Skip continuation files and extract resources
--help                      Show command-line help
```

## Output

The default output folder is `<lvz-name>_unpacked` and contains:

- `continuations/` — rebuilt DLRW/xet continuation files
- `resources/` — exact IMG resource payload candidates
- `master_resources.csv` — master LVZ Resource[] rows
- `sector_containers.csv` — IMG-backed sector/slave containers
- `sector_resources.csv` — exact resource rows and resolved offsets
- `summary.txt` — detection and extraction totals
- `unpack.log` — detailed parser log

## Windows build

The included executable is 64-bit Windows and uses the included `zlib1.dll`.

To rebuild with MinGW-w64:

```bat
x86_64-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -static-libgcc unimg.c -lz -o unimg.exe
```

On a normal Windows MinGW installation, use the corresponding `gcc` command if the cross-compiler-prefixed command is not present.
