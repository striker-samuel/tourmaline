"""One-run, dependency-free installer for the offline source asset service.

Windows: py -3 tools/install_offline_assets.py
Linux:   python3 tools/install_offline_assets.py
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def default_home() -> Path:
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    else:
        base = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))
    return base / "Tourmaline" / "OfflineAssets"


def install_maya_script(scripts_dir: Path | None = None) -> Path:
    """Copy Maya's native version picker to the user scripts folder.

    This does not edit Maya preferences, write to the project share, or
    require Maya to be running. A shelf button can be created from Maya later.
    """
    if scripts_dir is None:
        base = os.environ.get("MAYA_APP_DIR")
        if base:
            scripts_dir = Path(base) / "scripts"
        elif os.name == "nt":
            scripts_dir = Path.home() / "Documents" / "maya" / "scripts"
        else:
            scripts_dir = Path.home() / "maya" / "scripts"
    scripts_dir.mkdir(parents=True, exist_ok=True)
    source = ROOT / "tools" / "maya" / "tourmaline_asset_versions.py"
    target = scripts_dir / source.name
    shutil.copy2(source, target)
    return target


def default_config() -> Path:
    return default_home() / "config.json"


def configure(source: str, archive: str, cache: str, config: Path | None = None,
              port: int = 8765, poll: float = 3.0, settle: float = 2.0,
              cache_gib: float = 20.0) -> Path:
    sys.path.insert(0, str(ROOT))
    from tools.offline_asset_service import Archive

    if not 1024 <= port <= 65535 or poll < 0.5 or settle < 0.5 or cache_gib < 0:
        raise ValueError("invalid port, polling interval, settle window or cache size")
    # Setup must validate an accessible source; unlike first-time setup,
    # a previously configured service may restart while SMB is offline.
    if not Path(source).expanduser().is_dir():
        raise ValueError("select an accessible project folder before installing")
    Archive(source, archive, cache, settle=settle, max_cache_bytes=int(cache_gib * 1024**3))
    destination = (config or default_config()).expanduser().resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    settings = {
        "schema_version": 1,
        "source": str(Path(source).expanduser().resolve()),
        "archive": str(Path(archive).expanduser().resolve()),
        "cache": str(Path(cache).expanduser().resolve()),
        "port": port, "poll_seconds": poll, "settle_seconds": settle,
        "cache_gib": cache_gib
    }
    temp = destination.with_suffix(".tmp")
    try:
        temp.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
        if os.name != "nt":
            temp.chmod(0o600)
        temp.replace(destination)
    finally:
        temp.unlink(missing_ok=True)
    return destination


def command(config: Path) -> list[str]:
    return [sys.executable, str(ROOT / "tools" / "offline_asset_service.py"),
            "--config", str(config), "serve"]


def run_detached(config: Path) -> None:
    kwargs = {"cwd": ROOT, "stdin": subprocess.DEVNULL,
              "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    if os.name == "nt":
        kwargs["creationflags"] = (subprocess.DETACHED_PROCESS |
                                   subprocess.CREATE_NEW_PROCESS_GROUP)
    else:
        kwargs["start_new_session"] = True
    subprocess.Popen(command(config), **kwargs)


def install_startup(config: Path) -> str:
    if os.name == "nt":
        startup = (Path(os.environ["APPDATA"]) / "Microsoft" / "Windows" /
                   "Start Menu" / "Programs" / "Startup")
        startup.mkdir(parents=True, exist_ok=True)
        exe = Path(sys.executable).with_name("pythonw.exe")
        if not exe.is_file():
            exe = Path(sys.executable)
        startup_file = startup / "Tourmaline Offline Asset Service.cmd"
        startup_file.write_text(
            '@echo off\ncd /d "' + str(ROOT) + '"\nstart "" /min "' +
            str(exe) + '" "' + str(ROOT / "tools" / "offline_asset_service.py") +
            '" --config "' + str(config) + '" serve\n', encoding="utf-8")
        run_detached(config)
        return f"Started; autostart installed: {startup_file}"

    unit_dir = Path.home() / ".config" / "systemd" / "user"
    unit_dir.mkdir(parents=True, exist_ok=True)
    unit = unit_dir / "tourmaline-offline-assets.service"

    def quote(s: str) -> str:
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%") + '"'

    unit.write_text(
        "[Unit]\nDescription=Tourmaline offline source revision archive\n"
        "[Service]\nType=simple\nWorkingDirectory=" + quote(str(ROOT)) +
        "\nExecStart=" + " ".join(quote(x) for x in command(config)) +
        "\nRestart=on-failure\nRestartSec=5\n"
        "[Install]\nWantedBy=default.target\n", encoding="utf-8")
    try:
        subprocess.run(["systemctl", "--user", "daemon-reload"], check=True,
                       capture_output=True)
        subprocess.run(["systemctl", "--user", "enable", "--now", unit.name],
                       check=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError):
        run_detached(config)
        return f"Started standalone (systemd user service unavailable); startup file: {unit}"
    return f"Installed and started {unit.name}"


def terminal_setup() -> None:
    base = default_home()
    print("Tourmaline offline asset history setup")
    print("The source share remains writable. Archive and cache MUST be outside it.")
    source = input("Mapped project folder (e.g. G:\\project): ").strip().strip('"')
    archive = input(f"Protected archive [{base / 'archive'}]: ").strip() or str(base / "archive")
    cache = input(f"Local cache [{base / 'cache'}]: ").strip() or str(base / "cache")
    config = configure(source, archive, cache)
    print(f"Config saved to {config}")
    if input("Auto-start at login? [Y/n]: ").strip().lower() in ("", "y", "yes"):
        print(install_startup(config))
    else:
        run_detached(config)
        print("Started for this login session")
    if input("Install Maya version picker script? [Y/n]: ").strip().lower() in ("", "y", "yes"):
        print(f"Maya picker installed: {install_maya_script()}")
    print("Asset browser: http://127.0.0.1:8765/")


def gui_setup() -> None:
    import tkinter as tk
    from tkinter import filedialog, messagebox

    base = default_home()
    app = tk.Tk()
    app.title("Tourmaline Offline Asset Service — Install")
    app.geometry("720x370")
    app.columnconfigure(1, weight=1)
    tk.Label(app, text="Unrestricted SMB share + protected version history",
             font=("Arial", 12, "bold")).grid(
                 row=0, column=0, columnspan=3, sticky="w", padx=16, pady=16)
    fields = {}
    for idx, (name, title, value) in enumerate((
        ("source", "Project share (G:\\project)", ""),
        ("archive", "Protected revision archive", str(base / "archive")),
        ("cache", "Local DCC version cache", str(base / "cache")),
    ), start=1):
        var = tk.StringVar(value=value)
        fields[name] = var
        tk.Label(app, text=title).grid(row=idx, column=0, sticky="w", padx=16)
        tk.Entry(app, textvariable=var).grid(row=idx, column=1, sticky="ew", pady=9)
        tk.Button(app, text="Browse", command=lambda v=var: v.set(
            filedialog.askdirectory(initialdir=v.get() or str(Path.home())) or v.get())
        ).grid(row=idx, column=2, padx=12)

    auto = tk.BooleanVar(value=True)
    tk.Checkbutton(app, text="Run automatically at login", variable=auto).grid(
        row=4, column=0, columnspan=3, sticky="w", padx=16, pady=9)

    maya = tk.BooleanVar(value=True)
    tk.Checkbutton(app, text="Install Maya version picker script", variable=maya).grid(
        row=5, column=0, columnspan=3, sticky="w", padx=16)

    def install():
        try:
            config = configure(*(fields[k].get().strip() for k in ("source", "archive", "cache")))
            info = install_startup(config) if auto.get() else "Started for this session"
            if maya.get():
                info += "\\nMaya picker: " + str(install_maya_script())
            if not auto.get():
                run_detached(config)
        except Exception as exc:
            messagebox.showerror("Installation failed", str(exc))
            return
        messagebox.showinfo("Installation complete", info +
                            "\n\nBrowse at http://127.0.0.1:8765/")
        app.destroy()

    tk.Button(app, text="Install and Start", command=install, padx=20, pady=9
              ).grid(row=6, column=0, columnspan=3, pady=15)
    app.mainloop()


def main(argv=None):
    p = argparse.ArgumentParser(description="Tourmaline offline asset service installer")
    p.add_argument("--terminal", action="store_true")
    args = p.parse_args(argv)
    if args.terminal:
        terminal_setup()
        return
    try:
        gui_setup()
    except Exception as exc:
        # Tkinter can be importable on headless Linux but Tk() still fails
        # with TclError when DISPLAY or an X server is unavailable.
        print(f"GUI unavailable ({exc}); using terminal setup")
        terminal_setup()


if __name__ == "__main__":
    main()
