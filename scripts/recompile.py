#!/usr/bin/env python3
"""Recompile the game's i960 code, its TGP program and the sound board's 68000
program to native C++ and build them. Same steps on Linux, macOS and Windows.

  recompile.py [--set daytona93|daytona] [--build-dir build] [--config Release]

Needs the user's ROM set at roms/<set>.zip or roms/<set>.7z (git-ignored):
daytona93 (Daytona USA Deluxe '93, the default) or daytona (Revision A,
1994; give it its own build directory, e.g. --build-dir build-daytona).
Everything
derived from it (images, generated C++) goes under the build directory,
which is git-ignored: never commit it.
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# Exit status for "the ROM set was rejected", so setup.py can tell it from a
# failure to recompile or build (which exit 1, with the tool's own errors).
ROM_REJECTED_EXIT = 3


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, cwd=ROOT, **kw)


def tool(build, config, name):
    """Path of a built tool: single-config generators put it in the build
    directory, Visual Studio in build/<config>/."""
    exe = name + (".exe" if os.name == "nt" else "")
    for p in (os.path.join(build, exe), os.path.join(build, config, exe)):
        if os.path.exists(p):
            return p
    sys.exit(f"recompile: {name} was not built in {build}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", default="daytona93", choices=["daytona93", "daytona", "vf2"])
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--config", default="Release")
    args = ap.parse_args()
    build = os.path.join(ROOT, args.build_dir)
    cache = os.path.join(build, "rom_cache", args.set)
    roms = [os.path.join(ROOT, "roms", args.set + "." + ext) for ext in ("zip", "7z")]
    roms = [r for r in roms if os.path.exists(r)]

    build_cmd = ["cmake", "--build", build, "--config", args.config]
    # the build directory is for one ROM set (CMake's M2_ROMSET)
    run(["cmake", "-S", ".", "-B", build, "-DM2_ROMSET=" + args.set])
    if not all(os.path.exists(os.path.join(cache, f)) for f in ("tgp_program.bin", "sound_program.bin", "pcm1.bin")):
        if not roms:
            sys.exit(f"recompile: put your ROM set at roms/{args.set}.zip (or .7z) first")
        run(build_cmd + ["--target", "m2import"])
        if subprocess.run([tool(build, args.config, "m2import"), roms[0], cache], cwd=ROOT).returncode:
            sys.exit(ROM_REJECTED_EXIT)  # m2import printed which file is missing or wrong
    run(build_cmd + ["--target", "m2recomp", "m2tgprecomp", "m2sndrecomp"])

    gen = os.path.join(build, "gen", args.set)
    if os.path.isdir(gen):
        for f in os.listdir(gen):
            os.remove(os.path.join(gen, f))
    os.makedirs(gen, exist_ok=True)
    recomp = [tool(build, args.config, "m2recomp"), os.path.join(cache, "program.bin"), gen,
              "--seeds", os.path.join("seeds", args.set + ".txt")]
    hooks = os.path.join("seeds", args.set + "_hooks.txt")
    if os.path.exists(os.path.join(ROOT, hooks)):
        recomp += ["--hooks", hooks]
    if args.set == "vf2":
        recomp += ["--no-rom-mirror"]  # Model 2A: RAM at 0x220000
    run(recomp)
    tgp = os.path.join(build, "gen", args.set + "_tgp")
    os.makedirs(tgp, exist_ok=True)
    run([tool(build, args.config, "m2tgprecomp"), os.path.join(cache, "tgp_program.bin"),
         os.path.join(tgp, "tgp_gen.cpp")])

    snd = os.path.join(build, "gen", args.set + "_snd")
    os.makedirs(snd, exist_ok=True)
    snd_cmd = [tool(build, args.config, "m2sndrecomp"), os.path.join(cache, "sound_program.bin"),
               os.path.join(snd, "snd_gen.cpp")]
    snd_seeds = os.path.join("seeds", args.set + "_snd.txt")
    if os.path.exists(os.path.join(ROOT, snd_seeds)):
        snd_cmd += ["--seeds", snd_seeds]
    run(snd_cmd)

    run(["cmake", "-S", ".", "-B", build])  # picks up the generated sources
    run(build_cmd + ["--parallel"])  # the game (daytona, m2run) and the check tools


if __name__ == "__main__":
    main()
