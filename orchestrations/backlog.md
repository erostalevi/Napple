# Backlog (ranked; next rounds)

- nvn: `nvnTextureBuilderGetViewOffset` looked up by ACNH but not implemented (needs `gfx/src/nvnapi/names.txt` + definition) - reported by gfx-msaa-views r1
- input/IR camera (irs) - M, +0.05
- nvdrv nvhost-gpu/as-gpu/ctrl-gpu layouts vs real traces - needs Switch hardware (unreachable on Mac)
- media: AAC movie audio hookup (no title of the four creates a type-4 decoder) - media-live r1
- media/nvdrv: nvjpg/msenc/nvdisp on a title (none of the four opens them) - media-live r1

## After round 2 (from the 2026-11-01-ae99ac1 plan)
- svcGetInfo ids 19/27 (`kernel.c:440`) and kernel follow-ups — after sched-applets lands
- IPC depth for the applet (am) interfaces in `applet*.c` — after sched-applets lands
- translator `fail()` paths gfx-shader-views finds but cannot fix in round 2
- scan.py: read `runtime/tests/ipc_reference/` so IPC depth is measured, not estimated (catalog change)
- needs material, not reachable with the four titles: tess/XFB on a title, nvjpg/msenc/nvdisp/caps on a title, NCA0/NCA2 retail files; in-game GS draw needs a later-game SMO save
