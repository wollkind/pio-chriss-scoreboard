# PlatformIO extra script: adds an "ota" target that sends the firmware already built for this
# environment to the panel over WiFi. It reuses .pio/build/<env>, so nothing is rebuilt that a
# normal build already produced (a separate OTA environment would compile everything again).
#
#   pio run -e esp32s3 -t ota
#
# Environment variables:
#   SCOREBOARD_OTA_PASSWORD   same as OTA_PASSWORD in src/secrets.h (empty: no password)
#   SCOREBOARD_HOST           panel address; default scoreboard.local (use the IP if that fails)
#
# espota is run without --debug, which is what printed a "Chunk response" line for every 1 KB.

import os
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO)

ESPOTA = os.path.join(env.PioPlatform().get_package_dir("framework-arduinoespressif32"), "tools", "espota.py")  # noqa: F821


def ota_upload(target, source, env):
    host = os.environ.get("SCOREBOARD_HOST", "scoreboard.local")
    password = os.environ.get("SCOREBOARD_OTA_PASSWORD", "")
    cmd = [env.subst("$PYTHONEXE"), ESPOTA, "--progress", "-i", host, "-p", "3232", "-f", str(source[0])]
    if password:
        cmd += ["-a", password]
    print("OTA upload to %s (%s)" % (host, "with password" if password else "no password"))
    return subprocess.call(cmd)


env.AddCustomTarget(  # noqa: F821
    name="ota",
    dependencies="$BUILD_DIR/${PROGNAME}.bin",
    actions=[ota_upload],
    title="OTA upload",
    description="Build if needed, then upload over WiFi with espota",
)
