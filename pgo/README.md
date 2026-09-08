# Profile-guided optimisation data

`aarch64/` holds the GCC profile (`.gcda` files, one per object, plus a
`MANIFEST`) that CI and `tools/pgo_build.sh` apply with `-DDSPERATE_PGO=use`.
It is produced by `tools/pgo_refresh.sh` on a machine that has the ROMs, BIOS
and firmware the training runs need, so the public build never does.

The profile is valid for one compiler version and one set of compile flags
(`MANIFEST` records a fingerprint; the configure step refuses a mismatch) and
decays gracefully with source changes: a function whose body changed loses its
profile, the rest keep theirs. Refresh before a release, or whenever
`tools/pgo_refresh.sh --check` reports drift in hot files:

    DS_ROMS=... DS_BIOS=... tools/pgo_refresh.sh     # ~2 minutes, then commit pgo/

File names are relative to the build directory (`-fprofile-prefix-path`), so
any build tree layout finds them.
