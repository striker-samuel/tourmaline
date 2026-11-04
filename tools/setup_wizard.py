"""Tourmaline first-run project wizard (Windows GUI, Linux CLI).

Uses the native Project executable for project identity; credentials are kept in
per-user or protected host configuration, never under project content.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = (Path(sys.executable).resolve().parent if getattr(sys, "frozen", False)\n        else Path(__file__).resolve().parents[1])
sys.path.insert(0, str(ROOT))
from tools import project_init
from tools.configure_asset_client import configure as configure_client, install_maya
from tools.install_shared_assets import (configure as configure_host,\n                                         run_detached, wait_for_health)
from tools.maya.tourmaline_asset_cache import AssetClient, default_cache


def create_project(name, destination, project_exe=None):
    args = ["--name", name, "--path", str(destination), "--readme", "--gitignore"]
    if project_exe:
        args += ["--project-exe", str(project_exe)]
    planned = project_init.plan(project_init.parser().parse_args(args))
    # Never create an incomplete project when the required native executable is absent.
    if not Path(planned["native_command"][0]).is_file():
        raise RuntimeError("Project executable not found. Build Tourmaline or select Project.exe.")
    if project_init.main(args) != 0:
        raise RuntimeError("Native Project initialization failed. See error output.")
    return Path(planned["root"])


def validate_remote(url, token, cache):
    # Verify credentials and endpoint before persisting them.
    return len(AssetClient(url, token, cache=cache).sources())


def setup(name, destination, mode="none", *, project_exe=None, url=None,
          token=None, cache=None, maya=False, start_local=True, port=8765,
          archive=None, host_config=None, client_config=None):
    if mode not in ("none", "local", "remote"):
        raise ValueError("storage mode must be none, local or remote")
    root = Path(destination).expanduser().resolve()
    if mode == "remote":
        if not url or not token:
            raise ValueError("remote setup requires an HTTPS URL and access token")
        if not url.startswith("https://") and not url.startswith("http://127.0.0.1:"):
            raise ValueError("remote repositories must use HTTPS (except localhost)")
        validate_remote(url, token, cache or default_cache())

    # Validate project creation before altering asset-service configuration.
    project = create_project(name, root, project_exe)
    result = {"project": str(project), "mode": mode}
    if mode == "local":
        # Protected host history is a sibling, never inside the artist's
        # writable project tree.
        private = Path(archive).expanduser().resolve() if archive else (
            root.parent / (root.name + "-asset-history")).resolve()
        host = Path(host_config).expanduser() if host_config else (
            private / "server.json")
        cfg, secret = configure_host(
            str(root / "content"), str(private / "archive"),
            str(private / "server-cache"), config=host, port=port)
        # The local server is deliberately localhost-only, with no WAN exposure.
        if start_local:
            if getattr(sys, "frozen", False):
                # A bundled GUI executable cannot run Python scripts with its
                # sys.executable. Ship a distinct frozen server executable.
                server = ROOT / "TourmalineAssetServer.exe"
                if not server.is_file():
                    raise RuntimeError("TourmalineAssetServer.exe is missing from the installation")
                log_file = cfg.parent / "server.log"
                with log_file.open("ab") as output:
                    process = subprocess.Popen(
                        [str(server), "--config", str(cfg), "serve"],
                        cwd=str(ROOT), stdin=subprocess.DEVNULL, stdout=output,
                        stderr=subprocess.STDOUT,
                        creationflags=(subprocess.DETACHED_PROCESS |
                                       subprocess.CREATE_NEW_PROCESS_GROUP))
                try:
                    wait_for_health(cfg, process)
                except Exception:
                    if process.poll() is None:
                        process.terminate()
                    raise
            else:
                run_detached(cfg)
        local_url = f"http://127.0.0.1:{port}"
        client_path = configure_client(local_url, secret, cache, client_config)
        result.update(server_config=str(cfg), client_config=str(client_path),
                      endpoint=local_url, started=start_local)
    elif mode == "remote":
        client_path = configure_client(url, token, cache, client_config)
        result.update(client_config=str(client_path), endpoint=url)
    if maya:
        result["maya_scripts"] = [str(path) for path in install_maya()]
    return result


def gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    window = tk.Tk()
    window.title("Tourmaline - New Project Setup")
    window.geometry("760x610")
    window.columnconfigure(1, weight=1)
    fields = {}
    rows = [
        ("name", "Project name", "My Game"),
        ("destination", "Project directory", str(Path.home() / "TourmalineProjects" / "MyGame")),
        ("project_exe", "Project.exe (native CLI)", str(ROOT / "out" / "build" / "x64-Release" / "Project.exe")),
        ("url", "Remote HTTPS server URL", ""),
        ("token", "Repository access token", ""),
        ("cache", "Workstation cache", str(default_cache())),
        ("archive", "Local history folder (optional)", ""),
    ]
    tk.Label(window, text="Create a project and configure its asset repository",
             font=("Segoe UI", 13, "bold")).grid(row=0, column=0, columnspan=3,
                                                 sticky="w", padx=16, pady=18)
    for index, (key, title, default) in enumerate(rows, start=1):
        var = tk.StringVar(value=default)
        fields[key] = var
        tk.Label(window, text=title).grid(row=index, column=0, sticky="w", padx=16, pady=7)
        ttk.Entry(window, textvariable=var, show="*" if key == "token" else "").grid(
            row=index, column=1, sticky="ew", pady=7)
        if key in ("destination", "archive", "cache", "project_exe"):
            picker = (filedialog.askopenfilename if key == "project_exe" else
                      filedialog.askdirectory)
            ttk.Button(window, text="Browse", command=lambda v=var, fn=picker:
                       v.set(fn() or v.get())).grid(row=index, column=2, padx=10)

    mode = tk.StringVar(value="local")
    tk.Label(window, text="Asset mode").grid(row=8, column=0, sticky="w", padx=16)
    modes = ttk.Frame(window)
    modes.grid(row=8, column=1, sticky="w")
    for value, label in (("local", "Local (offline)"), ("remote", "Remote server"),
                         ("none", "Project only")):
        ttk.Radiobutton(modes, text=label, variable=mode, value=value).pack(side="left", padx=5)
    maya = tk.BooleanVar(value=False)
    ttk.Checkbutton(window, text="Install Maya picker scripts", variable=maya).grid(
        row=9, column=1, sticky="w", pady=8)
    status = tk.StringVar(value="Local mode starts a localhost-only asset server.")
    ttk.Label(window, textvariable=status, wraplength=630).grid(
        row=10, column=0, columnspan=3, padx=16, pady=12, sticky="w")

    def submit():
        try:
            options = {key: val.get().strip() for key, val in fields.items()}
            result = setup(options["name"], options["destination"], mode.get(),
                           project_exe=options["project_exe"] or None,
                           url=options["url"] or None, token=options["token"] or None,
                           cache=options["cache"] or None,
                           archive=options["archive"] or None, maya=maya.get())
        except Exception as exc:
            messagebox.showerror("Setup failed", str(exc))
            return
        messagebox.showinfo("Project created",
                            "Project: " + result["project"] + "\nMode: " + mode.get() +
                            "\nSettings saved. See the project README for next steps.")
        window.destroy()

    ttk.Button(window, text="Create project", command=submit).grid(
        row=11, column=0, columnspan=3, pady=16)
    window.mainloop()


def main(argv=None):
    parser = argparse.ArgumentParser(description="Tourmaline first-run project setup")
    parser.add_argument("--terminal", action="store_true",
                        help="script-based setup (Linux/headless)")
    parser.add_argument("--name")
    parser.add_argument("--path")
    parser.add_argument("--mode", choices=("none", "local", "remote"), default="local")
    parser.add_argument("--project-exe")
    parser.add_argument("--server-url")
    parser.add_argument("--token-env", default="TOURMALINE_ASSET_TOKEN")
    parser.add_argument("--cache")
    parser.add_argument("--archive")
    parser.add_argument("--no-start", action="store_true")
    parser.add_argument("--install-maya", action="store_true")
    args = parser.parse_args(argv)
    if not args.name and not args.path and not args.terminal:
        gui()
        return 0
    if not args.name or not args.path:
        parser.error("--name and --path are required for CLI setup")
    try:
        token = os.environ.get(args.token_env)
        if args.mode == "remote" and not token:
            import getpass
            token = getpass.getpass("Repository bearer token: ")
        result = setup(args.name, args.path, args.mode,
                       project_exe=args.project_exe, url=args.server_url,
                       token=token, cache=args.cache, archive=args.archive,
                       maya=args.install_maya, start_local=not args.no_start)
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, RuntimeError, project_init.InitError) as exc:
        print(f"setup failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
