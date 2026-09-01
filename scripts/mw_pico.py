#!/usr/bin/env python3
"""Build and flash the MicroWave Pico I2S firmware.

This intentionally mirrors the useful parts of MicroRender's Pico capture
runner: use the Pico VS Code extension's OpenOCD/CMSIS-DAP path first, allow
picotool, and retain a manual BOOTSEL fallback.
"""

import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PRESETS = {"max98357a", "pcm5102a", "ns4168"}


def find_picotool():
    for cand in ("picotool", "picotool.exe"):
        try:
            r = subprocess.run([cand, "version"], capture_output=True,
                               timeout=15)
            if r.returncode == 0:
                return cand
        except (OSError, subprocess.TimeoutExpired):
            pass
    base = os.path.join(os.path.expanduser("~"), ".pico-sdk", "picotool")
    if os.path.isdir(base):
        for ver in sorted(os.listdir(base), reverse=True):
            for rel in (("picotool", "picotool.exe"), ("picotool", "picotool")):
                cand = os.path.join(base, ver, *rel)
                if os.path.exists(cand):
                    return cand
    return None


def find_openocd():
    candidates = []
    explicit = os.environ.get("OPENOCD")
    if explicit:
        candidates.append(explicit)
    for name in ("openocd", "openocd.exe"):
        found = shutil.which(name)
        if found:
            candidates.append(found)

    base = os.path.join(os.path.expanduser("~"), ".pico-sdk", "openocd")
    if os.path.isdir(base):
        for ver in sorted(os.listdir(base), reverse=True):
            root = os.path.join(base, ver)
            for rel in ("openocd.exe", os.path.join("bin", "openocd.exe"),
                        "openocd", os.path.join("bin", "openocd")):
                cand = os.path.join(root, rel)
                if os.path.exists(cand):
                    candidates.append(cand)

    explicit_scripts = os.environ.get("OPENOCD_SCRIPTS")
    seen = set()
    for tool in candidates:
        key = os.path.normcase(os.path.abspath(tool))
        if key in seen:
            continue
        seen.add(key)
        tool_dir = os.path.dirname(os.path.abspath(tool))
        roots = []
        if explicit_scripts:
            roots.append(explicit_scripts)
        roots.extend([
            os.path.join(tool_dir, "scripts"),
            os.path.join(tool_dir, "share", "openocd", "scripts"),
            os.path.join(os.path.dirname(tool_dir), "scripts"),
            os.path.join(os.path.dirname(tool_dir), "share", "openocd", "scripts"),
        ])
        for scripts in roots:
            if (os.path.exists(os.path.join(scripts, "interface", "cmsis-dap.cfg"))
                    and os.path.exists(os.path.join(scripts, "target", "rp2350.cfg"))):
                return tool, scripts
    return None, None


def pico_ping(port, baud=115200, timeout=1.5):
    try:
        import serial
    except ImportError:
        return False
    try:
        with serial.Serial(port, baud, timeout=timeout) as ser:
            time.sleep(0.15)
            ser.reset_input_buffer()
            ser.write(b"PING\n")
            ser.flush()
            deadline = time.time() + timeout
            while time.time() < deadline:
                line = ser.readline()
                if b"MWPICO1" in line:
                    return True
    except (OSError, ValueError):
        return False
    return False


def find_pico_port():
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    ports = list(list_ports.comports())
    ordered = ([p.device for p in ports if p.vid == 0x2E8A] +
               [p.device for p in ports if p.vid != 0x2E8A])
    for dev in ordered:
        if pico_ping(dev):
            return dev
    return None


def wait_for_pico(timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        port = find_pico_port()
        if port:
            return port
        time.sleep(0.5)
    return None


def find_outputs(preset):
    build = os.path.join(ROOT, "microwave", "build-" + preset)
    elf = os.path.join(build, "microwave.elf")
    uf2 = os.path.join(build, "microwave.uf2")
    if os.path.exists(elf) and os.path.exists(uf2):
        return elf, uf2
    found_elf = None
    found_uf2 = None
    if os.path.isdir(build):
        for root, _dirs, files in os.walk(build):
            for name in files:
                if name == "microwave.elf":
                    found_elf = os.path.join(root, name)
                elif name == "microwave.uf2":
                    found_uf2 = os.path.join(root, name)
    return found_elf, found_uf2


def build(preset, extra):
    cmd = [os.path.join(ROOT, "mw.bat"), "build", "pico", preset,
           "serial=ON"] + extra
    print("pico: building %s" % preset)
    try:
        return subprocess.run(cmd, cwd=ROOT, timeout=900).returncode == 0
    except (OSError, subprocess.TimeoutExpired) as exc:
        print("      build failed: %s" % exc)
        return False


def manual_flash(uf2):
    print()
    print("pico: manual BOOTSEL flash")
    print("      1. Put the RP2350 into BOOTSEL mode.")
    print("      2. Drag this UF2 onto the RPI-RP2 drive:")
    print("           %s" % uf2)
    print("      3. The firmware starts playing immediately after reboot.")
    print("      Waiting up to five minutes for MWPICO1 ...")
    port = wait_for_pico(300.0)
    if port:
        print("      MicroWave is answering on %s" % port)
        return True
    print("      no MWPICO1 response seen; audio may still be running if pyserial")
    print("      is not installed or USB serial is unavailable.")
    return False


def flash_swd(elf):
    tool, scripts = find_openocd()
    if not tool:
        print("      Pico OpenOCD/CMSIS-DAP setup not found.")
        return False
    elf_tcl = os.path.abspath(elf).replace("\\", "/")
    cmd = [tool, "-s", scripts,
           "-f", "interface/cmsis-dap.cfg",
           "-f", "target/rp2350.cfg",
           "-c", "adapter speed 5000; program {%s} verify reset exit" % elf_tcl]
    print("pico: SWD flashing %s" % os.path.basename(elf))
    print("      OpenOCD: %s" % tool)
    try:
        r = subprocess.run(cmd, cwd=ROOT, timeout=180)
    except (OSError, subprocess.TimeoutExpired) as exc:
        print("      SWD flash failed: %s" % exc)
        return False
    if r.returncode != 0:
        print("      SWD flash failed (OpenOCD exit %d)" % r.returncode)
        return False
    port = wait_for_pico(20.0)
    if port:
        print("      flash complete; MicroWave is answering on %s" % port)
    else:
        print("      OpenOCD verified the image; no USB PING response was seen.")
    return True


def flash_picotool(uf2):
    tool = find_picotool()
    if not tool:
        print("pico: picotool not found")
        return False
    print("pico: picotool flashing %s" % os.path.basename(uf2))
    try:
        r = subprocess.run([tool, "load", "-f", "-x", uf2], timeout=180)
    except (OSError, subprocess.TimeoutExpired) as exc:
        print("      picotool flash failed: %s" % exc)
        return False
    if r.returncode != 0:
        print("      picotool failed; is the target connected?")
        return False
    port = wait_for_pico(20.0)
    if port:
        print("      flash complete; MicroWave is answering on %s" % port)
    else:
        print("      picotool completed; no USB PING response was seen.")
    return True


def main(argv):
    preset = argv[1].lower() if len(argv) > 1 else "max98357a"
    method = argv[2].lower() if len(argv) > 2 else "swd"
    extra = argv[3:]

    if preset not in PRESETS:
        print("ERROR: preset must be max98357a, pcm5102a, or ns4168")
        return 2
    if method not in ("swd", "picotool", "manual"):
        print("ERROR: flash method must be swd, picotool, or manual")
        return 2

    if not build(preset, extra):
        return 1
    elf, uf2 = find_outputs(preset)
    if not uf2:
        print("ERROR: no microwave.uf2 produced for %s" % preset)
        return 1

    if method == "manual":
        return 0 if manual_flash(uf2) else 1
    if method == "picotool":
        if flash_picotool(uf2):
            return 0
        print("      falling back to manual BOOTSEL.")
        return 0 if manual_flash(uf2) else 1

    if elf and flash_swd(elf):
        return 0
    if not elf:
        print("      no microwave.elf produced for SWD programming.")
    print("      SWD unavailable or failed; falling back to manual BOOTSEL.")
    return 0 if manual_flash(uf2) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
