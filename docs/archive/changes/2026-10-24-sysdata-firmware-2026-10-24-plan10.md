# sysdata-firmware (plan10)
- napple-build: `sysdata::write_system_archives` writes firmware data archives 0100000000000800-0FFF (RomFS image, ≤64 MiB) to `<title dir>/sysdata/<id>.bin`; called from the pipeline when `--firmware` is given; compat note if 0823 is missing. napple-core `RomFs::image()` added. [verified on Firmware 22.1.0: 40 files]
- fs.c: OpenDataStorageByDataId also looks in `<data-dir>/../sysdata/`. [tested: test_svc_fs; verified on AC v0]
- Not done: ids beyond 0x800-0xFFF range or >64 MiB; app-bundle layout not run (only --data-only built).
