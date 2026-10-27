# Render scale screenshots (2026-10-27)

Headless runs of the four Testing Material titles with `tools/golden/render_scale.py shots` (`NAPPLE_OUTPUT_SIZE=WxH` is the drawable; every file's size equals it, checked by the driver). JPEG copies (q86) of the PNG captures; the scenes are: LAS title screen (frame 4500), SMO cutscene (4000, menu 1500 for the upscaler rows), ACNH airport fade-in (300), MK8D cup select (1500).

Config names: `16x9-Nx` = render scale N in a 1280x720 window (SSAA down to the window), `21x9-window` / `9x16-window` = follow the window in 2560x1080 / 720x1280 (bars, no stretch), `21x9-fill` crop, `16x10-1x`, `4x3-1x`, `9x16-1x` native render in those windows, `4k-window` / `4k-1x` 3840x2160 window, `16x9-fsr|cas|metalfx` window 2560x1440 with the three upscalers (render 0.89x / 1.07x of the game's size), `16x9-ssaa2` 2x supersampling, `21x9-report` = tell the game the window aspect.

`upscaler-matrix.png`: reference, nearest, bilinear, bicubic, Lanczos, built-in (FSR-like), CAS, MetalFX on a synthetic scene (from `gfx/tests/t_scale_upscale.m`).

| title | config | size | file |
|---|---|---|---|
| las | 16x9-1x | 1280x720 | [las-16x9-1x.jpg](las-16x9-1x.jpg) |
| las | 16x9-2x | 1280x720 | [las-16x9-2x.jpg](las-16x9-2x.jpg) |
| las | 16x9-4x | 1280x720 | [las-16x9-4x.jpg](las-16x9-4x.jpg) |
| las | 21x9-window | 2560x1080 | [las-21x9-window.jpg](las-21x9-window.jpg) |
| las | 9x16-window | 720x1280 | [las-9x16-window.jpg](las-9x16-window.jpg) |
| smo | 16x9-1x | 1280x720 | [smo-16x9-1x.jpg](smo-16x9-1x.jpg) |
| smo | 16x9-2x | 1280x720 | [smo-16x9-2x.jpg](smo-16x9-2x.jpg) |
| smo | 16x9-4x | 1280x720 | [smo-16x9-4x.jpg](smo-16x9-4x.jpg) |
| smo | 21x9-window | 2560x1080 | [smo-21x9-window.jpg](smo-21x9-window.jpg) |
| smo | 9x16-window | 720x1280 | [smo-9x16-window.jpg](smo-9x16-window.jpg) |
| acnh | 16x9-1x | 1280x720 | [acnh-16x9-1x.jpg](acnh-16x9-1x.jpg) |
| acnh | 16x9-2x | 1280x720 | [acnh-16x9-2x.jpg](acnh-16x9-2x.jpg) |
| acnh | 16x9-4x | 1280x720 | [acnh-16x9-4x.jpg](acnh-16x9-4x.jpg) |
| acnh | 21x9-window | 2560x1080 | [acnh-21x9-window.jpg](acnh-21x9-window.jpg) |
| acnh | 9x16-window | 720x1280 | [acnh-9x16-window.jpg](acnh-9x16-window.jpg) |
| mk8d | 16x9-1x | 1280x720 | [mk8d-16x9-1x.jpg](mk8d-16x9-1x.jpg) |
| mk8d | 16x9-2x | 1280x720 | [mk8d-16x9-2x.jpg](mk8d-16x9-2x.jpg) |
| mk8d | 16x9-4x | 1280x720 | [mk8d-16x9-4x.jpg](mk8d-16x9-4x.jpg) |
| mk8d | 21x9-window | 2560x1080 | [mk8d-21x9-window.jpg](mk8d-21x9-window.jpg) |
| mk8d | 9x16-window | 720x1280 | [mk8d-9x16-window.jpg](mk8d-9x16-window.jpg) |
| acnh | 16x9-0.5x | 1280x720 | [acnh-16x9-0.5x.jpg](acnh-16x9-0.5x.jpg) |
| acnh | 16x9-1.37x | 1280x720 | [acnh-16x9-1.37x.jpg](acnh-16x9-1.37x.jpg) |
| acnh | 16x9-ssaa2 | 1280x720 | [acnh-16x9-ssaa2.jpg](acnh-16x9-ssaa2.jpg) |
| acnh | 4k-window | 3840x2160 | [acnh-4k-window.jpg](acnh-4k-window.jpg) |
| acnh | 4k-1x | 3840x2160 | [acnh-4k-1x.jpg](acnh-4k-1x.jpg) |
| acnh | 21x9-fill | 2560x1080 | [acnh-21x9-fill.jpg](acnh-21x9-fill.jpg) |
| acnh | 16x10-1x | 1920x1200 | [acnh-16x10-1x.jpg](acnh-16x10-1x.jpg) |
| acnh | 4x3-1x | 1024x768 | [acnh-4x3-1x.jpg](acnh-4x3-1x.jpg) |
| acnh | 9x16-1x | 720x1280 | [acnh-9x16-1x.jpg](acnh-9x16-1x.jpg) |
| acnh | 16x9-fsr | 2560x1440 | [acnh-16x9-fsr.jpg](acnh-16x9-fsr.jpg) |
| smo | 16x9-fsr | 2560x1440 | [smo-16x9-fsr.jpg](smo-16x9-fsr.jpg) |
| acnh | 16x9-cas | 2560x1440 | [acnh-16x9-cas.jpg](acnh-16x9-cas.jpg) |
| smo | 16x9-cas | 2560x1440 | [smo-16x9-cas.jpg](smo-16x9-cas.jpg) |
| acnh | 16x9-metalfx | 2560x1440 | [acnh-16x9-metalfx.jpg](acnh-16x9-metalfx.jpg) |
| smo | 16x9-metalfx | 2560x1440 | [smo-16x9-metalfx.jpg](smo-16x9-metalfx.jpg) |
| acnh | 21x9-report | 2560x1080 | [acnh-21x9-report.jpg](acnh-21x9-report.jpg) |
| smo | 21x9-report | 2560x1080 | [smo-21x9-report.jpg](smo-21x9-report.jpg) |
| mk8d | 21x9-report | 2560x1080 | [mk8d-21x9-report.jpg](mk8d-21x9-report.jpg) |
