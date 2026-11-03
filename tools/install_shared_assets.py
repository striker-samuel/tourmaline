"""Install/configure one authoritative shared asset repository on Windows/Linux.

Requires Python 3.10+; no database server, pip packages or container.
A NAS host can be Linux or Windows; the filesystem view is the only input.
"""
from __future__ import annotations

import argparse
import getpass
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def default_home():
    if os.name == "nt":
        return Path(os.environ.get("PROGRAMDATA", str(Path.home()))) / "Tourmaline" / "AssetRepository"
    return Path.home() / ".local" / "share" / "Tourmaline" / "AssetRepository"


def default_config():
    return default_home() / "server.json"


def configure(source, archive, cache, config=None, *, listen_lan=False,
              port=8765, poll=3.0, settle=2.0, cache_gib=20.0, token=None):
    sys.path.insert(0, str(ROOT))
    from tools.shared_asset_repository import SharedArchive
    source_path = Path(source).expanduser()
    if not source_path.is_dir():
        raise ValueError("source directory must exist and be mounted on the NAS host")
    if not 1024 <= port <= 65535 or poll < 0.5 or settle < 0.5 or cache_gib < 0:
        raise ValueError("invalid port, scan interval, settle time or cache size")
    # This verifies that no protected data can be stored underneath the share.
    SharedArchive(source, archive, cache, settle, int(cache_gib * 1024**3))
    output = Path(config or default_config()).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if token is None and output.exists():
        old = json.loads(output.read_text(encoding="utf-8"))
        token = old.get("access_token")
    token = token or secrets.token_urlsafe(40)
    if len(token) < 24:
        raise ValueError("token must be at least 24 characters")
    config_data = {
        "schema_version": 2,
        "mode": "shared",
        "source": str(source_path.resolve()),
        "archive": str(Path(archive).expanduser().resolve()),
        "cache": str(Path(cache).expanduser().resolve()),
        "access_token": token,
        "bind": "0.0.0.0" if listen_lan else "127.0.0.1",
        "port": port,
        "poll_seconds": poll,
        "settle_seconds": settle,
        "cache_gib": cache_gib,
        "consumer_ttl_seconds": 90
    }
    temporary = output.with_suffix(".tmp")
    try:
        temporary.write_text(json.dumps(config_data, indent=2) + "\n", encoding="utf-8")
        if os.name != "nt":
            temporary.chmod(0o600)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return output, token


def server_command(config):
    return [sys.executable, str(ROOT / "tools" / "shared_asset_server.py"),
            "--config", str(config), "serve"]


def run_detached(config):
    kwargs = {"cwd": str(ROOT), "stdin": subprocess.DEVNULL,
              "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    if os.name == "nt":
        kwargs["creationflags"] = (subprocess.DETACHED_PROCESS |
                                   subprocess.CREATE_NEW_PROCESS_GROUP)
    else:
        kwargs["start_new_session"] = True
    subprocess.Popen(server_command(config), **kwargs)


def install_windows_boot_task(config):
    """Always-on Windows NAS: start at boot as SYSTEM, not at user logon.

    Only use host-local paths (not per-user mapped drives); SYSTEM needs read
    access to sources and write access to archive/cache/config.
    """
    if os.name != "nt":
        raise OSError("Windows scheduled task installation requires Windows")
    launcher = Path(config).parent / "start-shared-assets.cmd"
    launcher.write_text('@echo off\ncd /d "' + str(ROOT) +
        '"\n"' + str(sys.executable) + '" "' +
        str(ROOT / "tools" / "shared_asset_server.py") + '" --config "' +
        str(config) + '" serve\n', encoding="utf-8")
    subprocess.run([
        "schtasks.exe", "/Create", "/F", "/TN", "TourmalineSharedAssetRepository",
        "/SC", "ONSTART", "/RU", "SYSTEM", "/TR",
        'cmd.exe /c "' + str(launcher) + '"'
    ], check=True)
    return launcher


def install_user_autostart(config):
    if os.name == "nt":
        root = (Path(os.environ["APPDATA"]) / "Microsoft" / "Windows" /
                "Start Menu" / "Programs" / "Startup")
        root.mkdir(parents=True, exist_ok=True)
        launcher = root / "Tourmaline Shared Asset Repository.cmd"
        interpreter = Path(sys.executable).with_name("pythonw.exe")
        if not interpreter.is_file():
            interpreter = Path(sys.executable)
        launcher.write_text('@echo off\ncd /d "' + str(ROOT) +
            '"\nstart "" /min "' + str(interpreter) + '" "' +
            str(ROOT / "tools" / "shared_asset_server.py") + '" --config "' +
            str(config) + '" serve\n', encoding="utf-8")
        run_detached(config)
        return launcher
    unit_dir = Path.home() / ".config" / "systemd" / "user"
    unit_dir.mkdir(parents=True, exist_ok=True)
    unit = unit_dir / "tourmaline-shared-assets.service"
    unit.write_text(unit_text(config, None), encoding="utf-8")
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
    subprocess.run(["systemctl", "--user", "enable", "--now", unit.name], check=True)
    return unit


def systemd_quote(value):
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%") + '"'


def unit_text(config, user):
    lines = [
        "[Unit]",
        "Description=Tourmaline authoritative shared asset repository",
        "Wants=network-online.target",
        "After=network-online.target remote-fs.target",
        "[Service]",
        "Type=simple",
        "WorkingDirectory=" + systemd_quote(ROOT),
        "ExecStart=" + " ".join(systemd_quote(v) for v in server_command(config)),
        "Environment=PYTHONUNBUFFERED=1",
        "Restart=always",
        "RestartSec=5",
    ]
    if user:
        lines.append("User=" + user)
    lines += ["[Install]", "WantedBy=" +
              ("multi-user.target" if user else "default.target"), ""]
    return "\n".join(lines)


def install_systemd_system(config, service_user):
    """Install an always-on NAS host service (requires Linux root)."""
    if sys.platform != "linux" or os.geteuid() != 0:
        raise PermissionError("systemd system installation requires root on Linux")
    import pwd
    account = pwd.getpwnam(service_user)
    data = json.loads(Path(config).read_text(encoding="utf-8"))
    for path in (Path(config), Path(data["archive"]), Path(data["cache"])):
        if path.is_dir():
            for folder, directories, files in os.walk(path):
                os.chown(folder, account.pw_uid, account.pw_gid)
                for file in files:
                    os.chown(str(Path(folder) / file), account.pw_uid, account.pw_gid)
        else:
            os.chown(path, account.pw_uid, account.pw_gid)
    unit = Path("/etc/systemd/system/tourmaline-shared-assets.service")
    unit.write_text(unit_text(config, service_user), encoding="utf-8")
    subprocess.run(["systemctl", "daemon-reload"], check=True)
    subprocess.run(["systemctl", "enable", "--now", unit.name], check=True)
    return unit


def terminal_setup():
    home = default_home()
    print("Tourmaline shared NAS asset repository — setup")
    print("Run this on the machine hosting (or reliably mounting) the source folder.")
    source = input("Work/source folder (not an archive): ").strip().strip('"')
    archive = input(f"Protected archive [{home / 'archive'}]: ").strip() or str(home / "archive")
    cache = input(f"Server-local resolver cache [{home / 'cache'}]: ").strip() or str(home / "cache")
    lan = input("Allow access from other workstations? [y/N]: ").strip().lower() in ("y", "yes")
    config, token = configure(source, archive, cache, listen_lan=lan)
    print(f"\nServer config: {config}")
    print("Client bearer token (save securely):", token)
    print("API port: 8765; bind:", "0.0.0.0" if lan else "127.0.0.1")
    print("HTTP over LAN is not encrypted. Configure HTTPS/reverse proxy for production.")
    if input("Install per-user autostart? [y/N]: ").strip().lower() in ("y", "yes"):
        print("Launcher:", install_user_autostart(config))
    else:
        run_detached(config)
        print("Started in background for this session")
    print("Start manually with:", " ".join(server_command(config)))


def gui_setup():
    import tkinter as tk
    from tkinter import filedialog, messagebox
    home = default_home()
    ui = tk.Tk()
    ui.title("Tourmaline Shared Repository — NAS Host")
    ui.geometry("760x390")
    ui.columnconfigure(1, weight=1)
    tk.Label(ui, text="Source share → protected revisions → published products",
             font=("Arial", 12, "bold")).grid(row=0,column=0,columnspan=3,
                                                padx=12,pady=15,sticky="w")
    fields = {}
    for n, (key,title,default) in enumerate((
            ("source", "Source folder on NAS host", ""),
            ("archive", "Protected archive / products", str(home / "archive")),
            ("cache", "Server-local cache", str(home / "cache"))), 1):
        var = tk.StringVar(value=default)
        fields[key] = var
        tk.Label(ui,text=title).grid(row=n,column=0,sticky="w",padx=12)
        tk.Entry(ui,textvariable=var).grid(row=n,column=1,sticky="ew",pady=8)
        tk.Button(ui,text="Browse",command=lambda x=var: x.set(
            filedialog.askdirectory(initialdir=x.get() or str(Path.home())) or x.get())
        ).grid(row=n,column=2,padx=12)
    lan = tk.BooleanVar(value=True)
    auto = tk.BooleanVar(value=False)
    tk.Checkbutton(ui,text="Listen on LAN (requires access token; HTTPS recommended)",
                   variable=lan).grid(row=4,column=0,columnspan=3,sticky="w",padx=12,pady=6)
    tk.Checkbutton(ui,text="Install per-user startup (for an always-on NAS prefer systemd system service)",
                   variable=auto).grid(row=5,column=0,columnspan=3,sticky="w",padx=12,pady=6)

    def install():
        try:
            config, token = configure(*(fields[k].get().strip()
                for k in ("source","archive","cache")), listen_lan=lan.get())
            if auto.get():
                status = "Startup launcher: " + str(install_user_autostart(config))
            else:
                run_detached(config)
                status = "Started for this session. Install a system service for always-on operation."
        except Exception as exc:
            messagebox.showerror("Setup failed", str(exc))
            return
        messagebox.showinfo("Repository configured",
                            f"Config: {config}\nToken: {token}\n\n{status}\n"
                            "Keep the token private. See docs/asset_repository_manual.md.")
        ui.destroy()
    tk.Button(ui,text="Configure repository",command=install,padx=15,pady=8).grid(
        row=6,column=0,columnspan=3,pady=12)
    ui.mainloop()


def main(argv=None):
    parser = argparse.ArgumentParser(description="Tourmaline shared NAS asset host installer")
    parser.add_argument("--terminal", action="store_true")
    parser.add_argument("--source")
    parser.add_argument("--archive")
    parser.add_argument("--cache")
    parser.add_argument("--config")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--listen-lan", action="store_true")
    parser.add_argument("--service-user", default="tourmaline")
    parser.add_argument("--install-systemd-system", action="store_true")
    parser.add_argument("--user-autostart", action="store_true")
    parser.add_argument("--install-windows-task", action="store_true")
    args = parser.parse_args(argv)
    if args.source or args.archive or args.cache:
        if not (args.source and args.archive and args.cache):
            parser.error("--source, --archive and --cache must be supplied together")
        config_target = (args.config or "/etc/tourmaline/shared-assets.json") if args.install_systemd_system else args.config
        config, token = configure(
            args.source,args.archive,args.cache,config=config_target,
            listen_lan=args.listen_lan,port=args.port)
        if args.install_systemd_system:
            print("Systemd unit:",install_systemd_system(config,args.service_user))
        elif args.install_windows_task:
            print("Windows boot task:",install_windows_boot_task(config))
        elif args.user_autostart:
            print("User launcher:",install_user_autostart(config))
        print("Config:",config)
        print("Client token:",token)
        return 0
    if args.install_systemd_system or args.install_windows_task:
        parser.error("system service installation requires --source, --archive and --cache")
    if args.terminal:
        terminal_setup()
    else:
        try:
            gui_setup()
        except Exception as exc:
            print("GUI unavailable:",exc)
            terminal_setup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
