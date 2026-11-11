# Vendored proven_c_lib

A copied snapshot of proven_c_lib (MIT, same author). Do not edit files here; if a proven
defect is found or a feature is missing, report it to proven_c_lib and resync from a fixed
release.

- Snapshot: tag `v0.113.0`, commit `bd93f44d`, taken 2026-11-11 with `git archive v0.113.0`
  (`include/`, `src/`, `platform/`, `LICENSE`, `THIRD_PARTY_NOTICES.md`): proven_base only. It
  replaced `v0.112.3` (`8bd8d205`, 2026-11-09), which replaced `v0.108.0` (`1d9989c4`,
  2026-11-02) and `v0.33.0` (`b0de5ec`, 2026-10-12). `nob.c` lists the files Pinax compiles.
- Not copied: since v0.113.0 the cryptography (`crypto/`, proven_crypto) and compression
  (`codec/`, proven_codec) parts, and since v0.111.0 the network parts (`net/`, proven_net), are
  libraries of their own. Pinax calls none of them (CRC-32 stays in proven_base's `hash.h`).
- Build flags: `nob.c` compiles these files with proven's own flag set
  (`-std=c23 -Wall -Wextra -Werror`, no `-pedantic`, which rejects its `__int128`).
  Pinax's own sources keep `-pedantic`.
