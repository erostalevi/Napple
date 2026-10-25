# ac-services (plan9): Animal Crossing service blockers

- fsp-srv 202 OpenDataStorageByDataId (alias of 30): DLC ids as before, else `<data-dir>/sysdata/<id>.bin`, else NOT_FOUND_DATA. [tested, verified on AC v0: reaches it]
- ICommonStateGetter 900: accepted no-op. [tested, verified on AC 3.0.1/3.0.3: reaches past it]
- Tests: `runtime/tests/test_svc_fs.c` (new), cmd 900 check in `test_svc_applet.c`. `make -C runtime test test-services` all pass.
- Files: `runtime/src/services/fs.c`, `applet.c`, tests, `docs/runtime-services.md` (append).

New first failures (45 s headless, scripted, not played): see integrators file.
