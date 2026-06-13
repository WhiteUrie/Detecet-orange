from __future__ import annotations

import subprocess
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent
APP_FILE = PROJECT_ROOT / "app.py"
VENV_PYTHON_WINDOWS = PROJECT_ROOT / ".venv" / "Scripts" / "python.exe"
VENV_PYTHON_UNIX = PROJECT_ROOT / ".venv" / "bin" / "python"
VENV311_PYTHON_WINDOWS = PROJECT_ROOT / ".venv311" / "Scripts" / "python.exe"
VENV311_PYTHON_UNIX = PROJECT_ROOT / ".venv311" / "bin" / "python"
REQUIRED_MODULES = ["flask", "requests", "numpy", "cv2", "ultralytics"]
PIP_PACKAGES = ["flask", "requests", "numpy", "opencv-python", "ultralytics"]


def find_venv_python() -> Path | None:
    if VENV_PYTHON_WINDOWS.exists():
        return VENV_PYTHON_WINDOWS
    if VENV_PYTHON_UNIX.exists():
        return VENV_PYTHON_UNIX
    return None


def find_venv311_python() -> Path | None:
    if VENV311_PYTHON_WINDOWS.exists():
        return VENV311_PYTHON_WINDOWS
    if VENV311_PYTHON_UNIX.exists():
        return VENV311_PYTHON_UNIX
    return None


def get_python_version_tuple(python_exe: str) -> tuple[int, int] | None:
    probe = subprocess.run(
        [python_exe, "-c", "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    if probe.returncode != 0:
        return None
    raw = (probe.stdout or "").strip()
    if "." not in raw:
        return None
    major_s, minor_s = raw.split(".", 1)
    try:
        return int(major_s), int(minor_s)
    except ValueError:
        return None


def is_compatible_for_yolo(python_exe: str) -> bool:
    version = get_python_version_tuple(python_exe)
    if version is None:
        return False
    major, minor = version
    return major == 3 and 9 <= minor <= 12


def find_python311_exe() -> str | None:
    candidates = [
        r"C:\Program Files\Python311\python.exe",
        r"C:\Python311\python.exe",
    ]
    for item in candidates:
        if Path(item).exists():
            return item

    # Fallback: ask py launcher.
    probe = subprocess.run(
        ["py", "-3.11", "-c", "import sys; print(sys.executable)"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    if probe.returncode == 0:
        exe = (probe.stdout or "").strip()
        if exe and Path(exe).exists():
            return exe
    return None


def ensure_venv311() -> Path | None:
    existing = find_venv311_python()
    if existing is not None and is_compatible_for_yolo(str(existing)):
        return existing

    py311 = find_python311_exe()
    if py311 is None:
        print("[START] Python 3.11 not found. Please install Python 3.11 to run YOLO reliably.")
        return None

    print("[START] Creating .venv311 with Python 3.11...")
    create = subprocess.run([py311, "-m", "venv", str(PROJECT_ROOT / ".venv311")], cwd=str(PROJECT_ROOT))
    if create.returncode != 0:
        print("[START] Failed to create .venv311")
        return None

    created = find_venv311_python()
    if created is None:
        print("[START] .venv311 created but python executable not found")
        return None
    return created


def ensure_project_venv() -> Path | None:
    existing = find_venv_python()
    if existing is not None and is_compatible_for_yolo(str(existing)):
        return existing

    current_python = Path(sys.executable).resolve()
    if not is_compatible_for_yolo(str(current_python)):
        return None

    print("[START] Creating project .venv with current compatible Python...")
    create = subprocess.run(
        [str(current_python), "-m", "venv", str(PROJECT_ROOT / ".venv")],
        cwd=str(PROJECT_ROOT),
    )
    if create.returncode != 0:
        print("[START] Failed to create project .venv")
        return None

    created = find_venv_python()
    if created is None:
        print("[START] Project .venv created but python executable not found")
        return None
    return created


def missing_modules(python_exe: str) -> list[str]:
    missing: list[str] = []
    for module in REQUIRED_MODULES:
        probe = subprocess.run(
            [python_exe, "-c", f"import {module}"],
            cwd=str(PROJECT_ROOT),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if probe.returncode != 0:
            missing.append(module)
    return missing


def ensure_dependencies(python_exe: str) -> bool:
    missing = missing_modules(python_exe)
    if not missing:
        return True

    print(f"[START] Missing modules: {', '.join(missing)}")
    print("[START] Installing required packages into selected environment...")

    install_cmd = [python_exe, "-m", "pip", "install", "--upgrade", *PIP_PACKAGES]
    install = subprocess.run(install_cmd, cwd=str(PROJECT_ROOT))
    if install.returncode != 0:
        print("[START] Failed to install dependencies. Please run: python setup.py --only-python")
        return False

    # Verify one more time after installation.
    still_missing = missing_modules(python_exe)
    if still_missing:
        print(f"[START] Modules still missing after install: {', '.join(still_missing)}")
        return False
    return True


def main() -> int:
    if not APP_FILE.exists():
        print(f"[START] Cannot find app file: {APP_FILE}")
        return 1

    venv_python = find_venv_python()
    venv311_python = find_venv311_python()
    current_python = Path(sys.executable).resolve()

    # Prefer a YOLO-compatible project-local environment first.
    project_venv = ensure_project_venv()
    if project_venv is not None:
        python_to_use = str(project_venv)
        print(f"[START] Using project virtual environment: {python_to_use}")
    elif venv311_python is not None and is_compatible_for_yolo(str(venv311_python)):
        python_to_use = str(venv311_python)
        print(f"[START] Using compatible environment: {python_to_use}")
    elif venv_python is not None and is_compatible_for_yolo(str(venv_python)):
        python_to_use = str(venv_python)
        print(f"[START] Using virtual environment Python: {python_to_use}")
    elif is_compatible_for_yolo(str(current_python)):
        project_venv = ensure_project_venv()
        if project_venv is None:
            python_to_use = str(current_python)
            print(f"[START] Using current compatible Python: {python_to_use}")
        else:
            python_to_use = str(project_venv)
            print(f"[START] Using project virtual environment: {python_to_use}")
    else:
        created = ensure_venv311()
        if created is None:
            return 1
        python_to_use = str(created)
        print(f"[START] Switched to compatible environment: {python_to_use}")

        if venv_python is not None and not is_compatible_for_yolo(str(venv_python)):
            print("[START] Note: existing .venv is Python 3.14 (not ideal for YOLO stack).")

    if python_to_use == str(current_python):
        if venv_python is None and venv311_python is None:
            print("[START] No local venv found, using current interpreter.")
        else:
            print("[START] Already running inside selected environment.")

    if not ensure_dependencies(python_to_use):
        return 1

    cmd = [python_to_use, str(APP_FILE), *sys.argv[1:]]
    print("[START] Running:", " ".join(cmd))

    completed = subprocess.run(cmd, cwd=str(PROJECT_ROOT))
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
