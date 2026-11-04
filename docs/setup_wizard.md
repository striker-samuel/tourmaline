# Tourmaline project setup wizard

The new first-run wizard creates a **native Tourmaline project** and optionally
connects the workstation to a shared asset repository, or starts a local
localhost-only asset archive. It reuses the canonical `Project init` command:
the native Project executable owns the project GUID and `project.tourproj`.

## Windows installer (recommended)

A Windows packaging pipeline is provided in
`packaging/build_windows_installer.ps1`. It produces an Inno Setup installer
containing a standalone wizard, local asset-server executable, native
`Project.exe` runtime, and optional Maya client modules. The installed wizard
does not require a separate Python installation.

**Packager prerequisites:** Build Tourmaline's native Project CLI for Windows
first. On the build machine install Python 3.10+ and Inno Setup 6. The packaging
script installs PyInstaller into the build Python unless told otherwise.
Invoke from the repository root:

~~~powershell
powershell -ExecutionPolicy Bypass -File packaging/build_windows_installer.ps1 -NativeBinDir out/build/x64-Release
~~~

The generated installer is under `out/installers/TourmalineSetup-Windows.exe`.
**This repository contains the installer recipe, not a prebuilt signed EXE.**
The packager copies the supplied native output directory, so its binaries
must be suitable for redistribution with the matching Project executable.
Check DLL dependencies and sign the installer before public distribution.

After installation choose **Tourmaline Setup Wizard** from Start Menu.
On first launch, supply a name and destination, then select:

- **Local:** creates `project.tourproj` and a `game.tourrecipe`, stores source
  revisions outside the project tree, and starts a localhost-only asset server.
  The generated client token is written to the workstation configuration.
  Choose a free port (default 8765). Closing the wizard does not stop the server.
- **Remote:** asks for an HTTPS asset server URL and an individual user bearer
  token, verifies the connection, then creates the project and saves credentials
  to the workstation's local client configuration. The project tree does not
  include credentials or a registry copy.
- **Project only:** creates the native project, recipe and README without
  starting an asset service.

Optionally check **Install Maya picker scripts** to copy the two Maya modules
into the current user's scripts directory. The wizard does not install Maya or
OS-wide account management. Remote accounts and permissions are provisioned on
the NAS server, not on each workstation. A new project recipe is a scaffold:
the scene must be authored/imported before it is buildable.

## Linux / headless script

Python 3.10+ and a built native `Project` executable are required. There is no
Linux GUI installer or packaging dependency.

~~~bash
sh setup_tourmaline.sh
# or fully noninteractive:
python3 tools/setup_wizard.py --name "My Game" --path ~/games/my-game \
  --project-exe out/build/linux-Release/Project --mode local
# remote (do not put tokens in command-line arguments):
TOURMALINE_ASSET_TOKEN='YOUR_USER_TOKEN' python3 tools/setup_wizard.py \
  --name "My Game" --path ~/games/my-game \
  --project-exe out/build/linux-Release/Project \
  --mode remote --server-url https://assets.example.lan
~~~

If you choose remote mode, use a protected environment variable or the
interactive secure token prompt. In local mode `--no-start` creates the
configuration but does not launch the service.

## Security and operational limits

- Authentication uses an asset-service bearer token; the setup wizard does not
  accept, store or transmit user passwords.
- Remote connections require HTTPS, except for explicit localhost connections.
- A token identifies a server-side account when a permission registry is
  enabled; configure users/groups on the server separately.
- The installer is a project/workstation setup, **not** a multi-user NAS host
  administrator or server-wide permissions editor. Continue using
  `install_shared_assets.py` for production NAS service installation.
- Local archives are project-specific. Additional local projects require a
  different API port or connection to an already-running remote repository.
- Configurations and local history are created outside the project's source
  content. Protect local client configs from other OS accounts.
- The installer does not download or install GPU drivers, Maya, the Visual C++
  runtime, build tools, or system dependencies for every optional Tourmaline
  editor component.

## Developer checks

~~~bash
python3 -m unittest discover -s tools/tests -p test_setup_wizard.py -v
python3 -m compileall -q tools/setup_wizard.py
~~~
