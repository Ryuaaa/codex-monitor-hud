#!/usr/bin/env python3
"""Source-level contract checks for the Windows milestone-1 HUD shell."""

from pathlib import Path


WINDOWS_ROOT = Path(__file__).resolve().parents[1]
SOURCE = (WINDOWS_ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
CMAKE = (WINDOWS_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")


def require(text: str, token: str, reason: str) -> None:
    if token not in text:
        raise AssertionError(f"missing {token!r}: {reason}")


def main() -> None:
    source_contracts = {
        "WS_CAPTION": "the standard caption provides native dragging",
        "WS_THICKFRAME": "the standard sizing frame provides resizing",
        "WS_MINIMIZEBOX": "the standard frame exposes native minimization",
        "ShowWindow(window, SW_MINIMIZE)": "the in-content minimize button works",
        "SetWindowPos": "topmost state is applied through the window manager",
        "HWND_TOPMOST": "the window can become topmost",
        "HWND_NOTOPMOST": "topmost mode can be disabled",
        "WM_GETMINMAXINFO": "resizing has a usable minimum size",
        "WM_DPICHANGED": "layout follows per-monitor DPI changes",
        "CODEX ACCOUNT & QUOTA": "the Codex account module has a visible placeholder",
        "TASKS & TOKEN USAGE": "the task/token module has a visible placeholder",
        "SYSTEM CPU & MEMORY": "the system metrics module has a visible placeholder",
        "TOP APPS & HEALTH": "the health module has a visible placeholder",
    }
    for token, reason in source_contracts.items():
        require(SOURCE, token, reason)

    cmake_contracts = {
        "add_executable(CodexMonitorHUD WIN32": "the executable uses the Windows GUI subsystem",
        "cxx_std_17": "the implementation has an explicit language baseline",
        "MSVC_RUNTIME_LIBRARY": "the release binary does not require a separate VC runtime install",
        "user32 gdi32": "only Windows system UI libraries are linked",
    }
    for token, reason in cmake_contracts.items():
        require(CMAKE, token, reason)

    if "CreateTimer" in SOURCE or "SetTimer" in SOURCE:
        raise AssertionError("milestone 1 must not add a background sampling timer")

    print("windows_static_contracts=pass")
    print(f"checked_source={WINDOWS_ROOT / 'src' / 'main.cpp'}")


if __name__ == "__main__":
    main()
