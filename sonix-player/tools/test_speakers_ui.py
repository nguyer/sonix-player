"""Link real UI and playback facades; no speaker requests or live settings."""
from pathlib import Path
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sonix-speakers-ui-") as directory:
    temp=Path(directory)
    (temp/"settings.ini").write_text("")
    cflags=shlex.split(subprocess.check_output(["pkg-config","--cflags","sdl2","freetype2"],text=True))
    subprocess.run(["cc","-Wall","-Wextra","-Werror","-I.","-DHOST_BUILD=1","-DLV_CONF_INCLUDE_SIMPLE=1",
                    *cflags,"-c","tools/test_speakers_ui.c","-o",str(temp/"test.o")],cwd=root,check=True)
    subprocess.run(["objcopy","--redefine-sym","main=sonix_simulator_main","build_host/main.o",str(temp/"main.o")],cwd=root,check=True)
    objects=sorted(str(p) for p in (root/"build_host").rglob("*") if p.suffix in (".o",".opp")
                   and p not in (root/"build_host/main.o",root/"build_host/gui/wireless/speakers.o"))
    flags=shlex.split(subprocess.check_output(["pkg-config","--libs","sdl2","freetype2","opusfile","wavpack"],text=True))
    subprocess.run(["g++",str(temp/"test.o"),str(temp/"main.o"),*objects,*flags,
                    *["-Wl,--wrap="+name for name in ("sonos_discover","sonos_refresh","sonos_connect","sonos_get_playback","sonos_get_room_volume","sonos_volume","sonos_join","sonos_leave","sonos_group_volume","wifi_available","wifi_get_status")],
                    "-lpthread","-lm","-ldl","-lasound","-o",str(temp/"test")],cwd=root,check=True)
    subprocess.run([str(temp/"test"),str(temp/"settings.ini")],cwd=root,check=True,timeout=30)
    subprocess.run(["cc","-Wall","-Wextra","-Werror","-I.","-DLV_CONF_INCLUDE_SIMPLE=1",
                    *cflags,"-c","tools/test_speakers_ui.c","-o",str(temp/"device-ui.o")],cwd=root,check=True)
    subprocess.run(["g++",str(temp/"device-ui.o"),str(temp/"main.o"),*objects,*flags,
                    *["-Wl,--wrap="+name for name in ("sonos_discover","sonos_refresh","sonos_connect","sonos_get_playback","sonos_get_room_volume","sonos_volume","sonos_join","sonos_leave","sonos_group_volume","wifi_available","wifi_get_status")],
                    "-lpthread","-lm","-ldl","-lasound","-o",str(temp/"device-ui")],cwd=root,check=True)
    subprocess.run([str(temp/"device-ui"),str(temp/"settings.ini")],cwd=root,check=True,timeout=30)
    # Exercise real device/queue/audio/volume facades against the mock speaker.
    subprocess.run(["cc","-Wall","-Wextra","-Werror","-I.","-DHOST_BUILD=1","-DFULL_APP_TEST=1",
                    "-DLV_CONF_INCLUDE_SIMPLE=1",*cflags,"-c","tools/test_speaker_output.c","-o",str(temp/"output.o")],cwd=root,check=True)
    output_objects=sorted(str(p) for p in (root/"build_host").rglob("*") if p.suffix in (".o",".opp")
                          and p not in (root/"build_host/main.o",root/"build_host/system/remote/speaker_output.o"))
    subprocess.run(["g++",str(temp/"output.o"),str(temp/"main.o"),*output_objects,*flags,"-lpthread","-lm","-ldl","-lasound","-o",str(temp/"output")],cwd=root,check=True)
    subprocess.run([str(temp/"output"),str(temp/"settings.ini")],cwd=root,check=True,timeout=30)
