# applets-live (b7, 2026-10-28, Linux cloud): tested, not verified

Branch `claude/applets-live-2026-10-27-b7`. Nothing here was compiled on macOS or run against a title.

- **swkbd text**: `applet_swkbd_parse` fills `header`/`sub`/`guide` (UTF-8, from KeyboardConfig +0x24/+0xA6/+0x1A8) via the new portable `applet_utf16le_to_utf8` (surrogates handled). The inline path (`la_push_interactive`) passed empty strings; it now passes these. Initial text stays empty inline (the inline protocol sets it with a separate request this code does not model).
- **Profiles**: new `account_core.{c,h}`; `NAPPLE_PROFILES=n` (1..8, default 1). `account.c` registers `applet_set_profile_lister`; `GetProfile` returns per-uid IProfile (name "Player N"). Player select cancel was already scripted (`NAPPLE_PLAYER_SELECT=cancel`) and is tested.
- **Applet 42**: `IPC_IMPL`, failing 0x1480 (same as 41), logged once.
- **Offline web**: percent-decoding, encoded `..`/backslash/NUL rejected, symlink containment (realpath), nested-path tests.
- **Tests**: swkbd header text (core, foreground, inline), min-length cancel, accounts (3 profiles, lister), applet 42, web resolve. `tests/linux/run_applet.sh` was broken at the base (missing web/album/caps/jpeg cores in the link): fixed.
- **Not done**: swkbd initial text inline; legal-information/system-data web pages (firmware romfs); HtmlDocument language subdirectories (layout unverified); loose-end scan of applet.c found nothing else unhandled.
