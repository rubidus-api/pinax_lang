# Vendored proven_c_lib

A copied snapshot of proven_c_lib (MIT, same author). Do not edit files here; if a proven
defect is found or a feature is missing, report it to proven_c_lib and resync from a fixed
release.

- Snapshot: tag `v0.112.3`, commit `8bd8d205`, taken 2026-11-09 with `git archive v0.112.3`
  (`include/`, `src/`, `platform/`, `LICENSE`, `THIRD_PARTY_NOTICES.md`); it replaced `v0.108.0`
  (`1d9989c4`, 2026-11-02), which replaced `v0.33.0` (`b0de5ec`, 2026-10-12). The whole tree is
  copied; `nob.c` lists the files Pinax compiles. Since v0.111.0 the network, TLS and HTTP parts
  are a library of their own (`net/`, proven_net), not copied; Pinax never compiled them.
- Build flags: `nob.c` compiles these files with proven's own flag set
  (`-std=c23 -Wall -Wextra -Werror`, no `-pedantic`, which rejects its `__int128`).
  Pinax's own sources keep `-pedantic`.
