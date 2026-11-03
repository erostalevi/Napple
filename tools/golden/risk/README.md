# tools/golden/risk

Expected results of `make -C runtime test-risk-title` (runtime/tests/test_risk_title.c) on the four real titles, from the shared title data
(`$NAPPLE_WORK/games`). Verified on the Mac 2026-11-03.

| title | build | main.npdm fs_perms | consequence |
|---|---|---|---|
| 0100000000010000 (SMO) | 3ca12dfaaf9c82da | 0x4000000000000000 (Debug bit only) | every gated mount PermissionDenied |
| 0100152000022000 (MK8D) | b5c39b5b7c62a88e, fe941ed5ba14be5d | 0x4000000000040000 | same (bit 18 SaveDataBackUp is not in any gate) |
| 01006BB00C6F0000 (LAS) | ae16f71e002af8cb | 0 | same |
| 01006F8002326000 (ACNH) | ff1d1c05670db602, 7fc1baff976aeca4 | 0x4000000000040000 | same |

Last line of the test: `{"test":"risk_title","checked":158,"failed":0,"skipped":0}`. Debug/FullPermission (bits 62/63) never count (retail console).
