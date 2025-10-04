#!/usr/bin/env python3
import os
import sys
import platform
import subprocess

BUILD_DIR = "build"

def run_command(cmd):
    print("Running:", " ".join(cmd))
    result = subprocess.run(cmd)
    if result.returncode != 0:
        sys.exit(result.returncode)

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    os.chdir(BUILD_DIR)

    system = platform.system()

    if system == "Windows":
        generator = "Visual Studio 17 2022"
        run_command(["cmake", "..", f"-G{generator}"])
        run_command(["cmake", "--build", ".", "--config", "Release"])
    else:
        run_command(["cmake", ".."])
        run_command(["cmake", "--build", ".", "-j4"])

if __name__ == "__main__":
    main()
