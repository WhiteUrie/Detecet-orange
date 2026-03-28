#!/usr/bin/env python3
"""
Project bootstrap script for Orange Detection + Sorting system.

What this script does:
1) Installs required Python packages for Flask + YOLO pipeline.
2) Optionally installs ESP32 board core and Arduino libraries if arduino-cli is available.
3) Performs basic project sanity checks.

Usage:
  python setup.py
  python setup.py --skip-esp32
  python setup.py --only-python
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


PYTHON_PACKAGES = [
    "pip>=24.0",
    "setuptools>=70.0.0",
    "wheel>=0.43.0",
    "flask>=3.0.0",
    "requests>=2.31.0",
    "numpy>=1.26.0",
    "opencv-python>=4.9.0.80",
    "ultralytics>=8.2.0",
]


ARDUINO_LIBRARIES = [
    "ESP32Servo",
]


def run_command(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    print("$", " ".join(cmd))
    return subprocess.run(cmd, check=check)


def check_python_version() -> None:
    if sys.version_info < (3, 9):
        raise SystemExit("Python 3.9+ is required. Current: " + sys.version.split()[0])


def install_python_dependencies() -> None:
    print("\n[1/3] Installing Python dependencies...")
    run_command([sys.executable, "-m", "pip", "install", "--upgrade", *PYTHON_PACKAGES])


def find_arduino_cli() -> str | None:
    # 1) Prefer PATH if user already configured it.
    cli = shutil.which("arduino-cli")
    if cli:
        return cli

    # 2) Common Arduino IDE 2.x bundled locations on Windows.
    candidates: list[Path] = []
    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        candidates.extend(
            [
                Path(local_app_data)
                / "Programs"
                / "Arduino IDE"
                / "resources"
                / "app"
                / "lib"
                / "backend"
                / "resources"
                / "arduino-cli.exe",
                Path(local_app_data)
                / "Programs"
                / "Arduino IDE"
                / "resources"
                / "app"
                / "lib"
                / "backend"
                / "arduino-cli.exe",
            ]
        )

    for env_var in ("PROGRAMFILES", "PROGRAMFILES(X86)"):
        base = os.environ.get(env_var)
        if base:
            candidates.append(
                Path(base)
                / "Arduino IDE"
                / "resources"
                / "app"
                / "lib"
                / "backend"
                / "resources"
                / "arduino-cli.exe"
            )

    for path in candidates:
        if path.exists():
            return str(path)

    return None


def install_esp32_dependencies() -> None:
    print("\n[2/3] Setting up ESP32 dependencies (optional)...")
    arduino_cli = find_arduino_cli()
    if not arduino_cli:
        print(
            "- arduino-cli not found in PATH or common Arduino IDE paths. "
            "Skipping ESP32 auto setup.\n"
            "  Manual setup in Arduino IDE:\n"
            "  1) Board Manager -> install 'esp32' by Espressif Systems.\n"
            "  2) Library Manager -> install 'ESP32Servo'."
        )
        return

    print(f"- Using arduino-cli at: {arduino_cli}")
    run_command([arduino_cli, "config", "init"], check=False)
    run_command([arduino_cli, "core", "update-index"])
    run_command([arduino_cli, "core", "install", "esp32:esp32"])

    for lib in ARDUINO_LIBRARIES:
        run_command([arduino_cli, "lib", "install", lib])


def validate_project_files(project_root: Path) -> None:
    print("\n[3/3] Validating project files...")
    required_paths = [
        project_root / "app.py",
        project_root / "templates" / "index.html",
        project_root / "ESP32C3" / "esp32c3.ino",
        project_root / "ESP32CAM" / "esp32cam.ino",
    ]

    missing = [str(path) for path in required_paths if not path.exists()]
    if missing:
        print("- Missing required files:")
        for item in missing:
            print("  -", item)
    else:
        print("- Required project files: OK")

    model_path = project_root / "best.pt"
    if not model_path.exists():
        print(
            "- Warning: best.pt not found. Place your YOLO model at project root "
            "before running app.py."
        )
    else:
        print("- YOLO model best.pt: OK")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Setup environment for Detect Orange project")
    parser.add_argument(
        "--skip-esp32",
        action="store_true",
        help="Skip ESP32 environment setup",
    )
    parser.add_argument(
        "--only-python",
        action="store_true",
        help="Install only Python dependencies",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    project_root = Path(__file__).resolve().parent

    print("=== Detect Orange Project Setup ===")
    print("Project root:", project_root)

    check_python_version()
    install_python_dependencies()

    if not (args.skip_esp32 or args.only_python):
        install_esp32_dependencies()
    else:
        print("\n[2/3] Skipped ESP32 setup by option.")

    validate_project_files(project_root)

    print("\nSetup completed.")
    print("Run the web app with: python app.py")


if __name__ == "__main__":
    main()
