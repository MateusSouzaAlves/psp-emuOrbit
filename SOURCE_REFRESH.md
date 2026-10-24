# Source refresh — 2026-10-24

<!-- doc-status: legal; reviewed: 2026-10-24 -->

Scope: Complete the current native/JNI component export and all imported source/notice inputs.

The snapshot is component source, not a whole-app GPL compliance statement.
The Android frontend/common host scope remains unresolved. Existing per-file
licenses, upstream pins and historical commits/tags remain intact. No BIOS,
games, user progress, production keys, credentials or binary releases are
part of this publication.

A clean source-dependency/patch replay check is recorded separately from
compiling binaries, game/save testing, Play signing and legal conclusions.
Source-only changes do not change the application version or user data.

Verify this snapshot from its root with:

```sh
python -B scripts/verify-source-snapshot.py
```

Initialize recorded gitlinks recursively for builds. This check verifies files
and recorded pins; it does not claim a full app build or license certification.
