"""Start the host UI headlessly with isolated writable settings for eight seconds."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sonix-ui-smoke-") as directory:
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
               SONIX_CONFIG=f"{directory}/config.ini", SONIX_EBOOK_CONFIG=f"{directory}/ebook.ini",
               SONIX_SD_ROOT=directory, SONIX_NO_MOUNT="1", SONIX_NO_SYSSERVER="1")
    process = subprocess.Popen(["./sonix_player_host"], cwd=root, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        code = process.wait(timeout=8)
        print(process.stdout.read().decode(errors="replace")[-5000:])
        raise SystemExit(f"Simulator exited unexpectedly: {code}")
    except subprocess.TimeoutExpired:
        print("Simulator startup passed: remained running for eight seconds")
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
