"""Connect a Windows/Linux workstation (and Maya) to the shared NAS repository."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "maya"))
from tourmaline_asset_cache import (AssetClient, default_cache,
                                    default_client_config)


def configure(url, token, cache=None, config=None):
    # Validate the URL and token without requiring a network connection.
    client = AssetClient(url, token, cache=cache or default_cache())
    destination = Path(config or default_client_config()).expanduser().resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    settings = {"schema_version": 1, "url": client.url, "token": token,
                "cache": str(client.cache)}
    pending = destination.with_suffix(".tmp")
    try:
        pending.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
        if os.name != "nt":
            pending.chmod(0o600)
        pending.replace(destination)
    finally:
        pending.unlink(missing_ok=True)
    return destination


def install_maya(scripts_dir=None):
    if scripts_dir is None:
        if os.environ.get("MAYA_APP_DIR"):
            scripts_dir = Path(os.environ["MAYA_APP_DIR"]) / "scripts"
        elif os.name == "nt":
            scripts_dir = Path.home() / "Documents" / "maya" / "scripts"
        else:
            scripts_dir = Path.home() / "maya" / "scripts"
    root = Path(scripts_dir).expanduser()
    root.mkdir(parents=True, exist_ok=True)
    copies = []
    for name in ("tourmaline_asset_versions.py", "tourmaline_asset_cache.py"):
        target = root / name
        shutil.copy2(ROOT / "tools" / "maya" / name, target)
        copies.append(target)
    return copies


def terminal():
    print("Tourmaline NAS asset repository — workstation connection")
    url = input("Server URL (e.g. http://nas.local:8765): ").strip()
    token = input("Access token from the NAS host: ").strip()
    cache = input(f"Local cache [{default_cache()}]: ").strip() or str(default_cache())
    path = configure(url, token, cache)
    print("Client config:", path)
    if input("Install Maya picker and cache modules? [Y/n]: ").strip().lower() in ("", "y", "yes"):
        print("Maya scripts:", *install_maya(), sep="\n  ")
    print("The cache uses verified hashes and is independent of the NAS share.")


def gui():
    import tkinter as tk
    from tkinter import messagebox, filedialog
    ui = tk.Tk()
    ui.title("Tourmaline — Connect to Shared Asset Repository")
    ui.geometry("700x340")
    ui.columnconfigure(1, weight=1)
    tk.Label(ui, text="Connect Maya to the authoritative NAS asset history",
             font=("Arial", 12, "bold")).grid(row=0,column=0,columnspan=3,
                                                padx=12,pady=15,sticky="w")
    fields = {}
    for n, (key,title,initial) in enumerate((
            ("url","NAS API URL","http://nas.local:8765"),
            ("token","Access token",""),
            ("cache","Local verified cache",str(default_cache()))), 1):
        variable = tk.StringVar(value=initial)
        fields[key] = variable
        tk.Label(ui, text=title).grid(row=n,column=0,padx=12,sticky="w")
        tk.Entry(ui,textvariable=variable,show="*" if key=="token" else "").grid(
            row=n,column=1,pady=8,sticky="ew")
        if key=="cache":
            tk.Button(ui,text="Browse",command=lambda x=variable: x.set(
                filedialog.askdirectory(initialdir=x.get() or str(Path.home())) or x.get())
            ).grid(row=n,column=2,padx=10)
    maya = tk.BooleanVar(value=True)
    tk.Checkbutton(ui,text="Install Maya version picker and cache module",
                   variable=maya).grid(row=4,column=0,columnspan=3,padx=12,pady=10,sticky="w")

    def save():
        try:
            config = configure(*(fields[k].get().strip() for k in ("url","token","cache")))
            copies = install_maya() if maya.get() else []
        except Exception as exc:
            messagebox.showerror("Invalid configuration", str(exc))
            return
        messagebox.showinfo("Connected", "Settings saved: " + str(config) +
                            ("\nMaya scripts: " + ", ".join(str(x) for x in copies)
                             if copies else "") +
                            "\nOpen Maya and run: import tourmaline_asset_versions as tm; tm.show_picker()")
        ui.destroy()

    tk.Button(ui,text="Save connection",command=save,padx=20,pady=8).grid(
        row=5,column=0,columnspan=3,pady=15)
    ui.mainloop()


def main(argv=None):
    p = argparse.ArgumentParser(description="Configure workstation access to Tourmaline NAS")
    p.add_argument("--terminal", action="store_true")
    p.add_argument("--url")
    p.add_argument("--token")
    p.add_argument("--cache")
    p.add_argument("--config")
    p.add_argument("--install-maya", action="store_true")
    p.add_argument("--check", action="store_true")
    args = p.parse_args(argv)
    if args.url or args.token:
        if not (args.url and args.token):
            p.error("--url and --token are both required")
        if args.check:
            # A failed connection must not replace a working client config.
            probe = AssetClient(args.url, args.token, cache=args.cache or default_cache())
            count = len(probe.sources())
        path = configure(args.url, args.token, args.cache, args.config)
        print("Client config:", path)
        if args.install_maya:
            print("Installed:", *install_maya(), sep="\n  ")
        if args.check:
            print("Connected to repository; source assets:", count)
    elif args.terminal:
        terminal()
    else:
        try:
            gui()
        except Exception as exc:
            print("GUI unavailable:", exc)
            terminal()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
