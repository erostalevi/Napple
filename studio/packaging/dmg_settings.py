# dmgbuild settings (https://dmgbuild.readthedocs.io). dmgbuild writes the Finder layout (.DS_Store) itself, so it works headless on CI.
# Invoked by make_release.sh:  dmgbuild -s dmg_settings.py -D app=... -D bg=... -D icon=... "Napple Studio" out.dmg
import os
app = defines["app"]
appname = os.path.basename(app)

format = "UDZO"
filesystem = "HFS+"
files = [app]
symlinks = {"Applications": "/Applications"}
background = defines["bg"]
if defines.get("icon"):
    icon = defines["icon"]
icon_size = 112
text_size = 13
icon_locations = {appname: (180, 190), "Applications": (480, 190)}
window_rect = ((200, 120), (660, 400))
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
include_icon_view_settings = "auto"
arrange_by = None
