# Vendored ggwave

Copied, not forked. Nothing in this directory is edited: to update, re-copy from
upstream and change the commit below.

- Upstream: https://github.com/ggerganov/ggwave
- Commit: `060aec73dd7123ccac200442f75bdc7369795ffe` (2026-04-16)
- Release at the time of vendoring: v0.4.3
- License: MIT (`LICENSE`), and the vendored Reed Solomon carries its own
  (`src/reed-solomon/LICENSE`)

## Files, and why only these

`src/CMakeLists.txt` and everything under `examples/` are deliberately not here.
ggwave is a codec with no audio I/O and no external dependencies, so the whole
library is one translation unit:

| File | |
| --- | --- |
| `include/ggwave/ggwave.h` | Public C and C++ API |
| `src/ggwave.cpp` | The only translation unit to compile |
| `src/fft.h` | Header only FFT, included by `ggwave.cpp` |
| `src/reed-solomon/{gf,poly,rs}.hpp` | Header only ECC |

## Include paths

`ggwave.cpp` includes `"ggwave/ggwave.h"`, `"fft.h"` and
`"reed-solomon/rs.hpp"`. The last two resolve relative to `ggwave.cpp` itself,
so the only include directory a consumer needs to add is `include/`.

## Never define `GGWAVE_CONFIG_FEW_PROTOCOLS`

It looks like a binary size win. At `ggwave.h:44-51` it is what compiles out the
`AUDIBLE_*` and `ULTRASOUND_*` protocols, which are the ones this module exists
to use, and it silently renumbers `ggwave_ProtocolId` so every id in
`src/index.ts` would shift by six.
