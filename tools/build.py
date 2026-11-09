#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLCHAIN = ROOT / ".toolchain"
BIN_DIR = TOOLCHAIN / ("Scripts" if os.name == "nt" else "bin")
EXE = ".exe" if os.name == "nt" else ""


def run(cmd):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, cwd=ROOT)


def system_tools():
    cmake = shutil.which("cmake")
    has_compiler = any(shutil.which(c) for c in ("c++", "g++", "clang++", "cl"))
    return cmake if cmake and has_compiler else None


def ensure_private_toolchain():
    python = BIN_DIR / f"python{EXE}"
    if not python.exists():
        run([sys.executable, "-m", "venv", TOOLCHAIN])
    probe = subprocess.run(
        [str(python), "-c", "import ziglang, cmake, ninja"], capture_output=True
    )
    if probe.returncode != 0:
        run([python, "-m", "pip", "install", "--quiet", "--disable-pip-version-check",
             "ziglang", "cmake", "ninja"])
    probe = subprocess.check_output(
        [str(python), "-c", "import ziglang, os; print(os.path.dirname(ziglang.__file__))"],
        text=True,
    )
    zig = Path(probe.strip()) / f"zig{EXE}"
    return BIN_DIR / f"cmake{EXE}", BIN_DIR / f"ninja{EXE}", zig


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test", action="store_true", help="run the tests after building")
    parser.add_argument("--debug", action="store_true", help="build without optimization")
    parser.add_argument("--sanitize", action="store_true",
                        help="build with UndefinedBehaviorSanitizer")
    args = parser.parse_args()

    build_type = "Debug" if args.debug else "RelWithDebInfo"
    # A separate folder, because CMake would otherwise keep the sanitizer flags
    # in its cache and apply them to later normal builds.
    build_dir = ROOT / "build" / (build_type.lower() + ("-ubsan" if args.sanitize else ""))
    configure = ["-S", ROOT, "-B", build_dir, f"-DCMAKE_BUILD_TYPE={build_type}"]

    cmake = system_tools()
    if cmake:
        print(f"Using system toolchain: {cmake}")
    else:
        cmake, ninja, zig = ensure_private_toolchain()
        print(f"Using private toolchain: {zig}")
        zig_cmake = zig.as_posix()
        configure += ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja.as_posix()}",
                      f"-DCMAKE_CXX_COMPILER={zig_cmake};c++"]
    if args.sanitize:
        configure += ["-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize-recover=undefined"]

    run([cmake, *configure])
    run([cmake, "--build", build_dir, "--parallel"])

    if args.test:
        candidates = list(build_dir.glob(f"**/ad_engine_tests{EXE}"))
        if not candidates:
            sys.exit("test binary not found")
        run([candidates[0]])


if __name__ == "__main__":
    main()
