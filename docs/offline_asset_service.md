# Offline asset history service (first slice)

For a **shared Windows/Linux NAS repository**, immutable published
representations, stable source UUIDs, workstation-local caches and active
reader visibility, see the [Asset repository manual](asset_repository_manual.md).
The NAS-capable service can also run entirely on localhost for offline use.

This is an **offline-first** revision archive for arbitrary source files on a
mapped SMB share such as `G:\\project`, a mounted Linux share, or a local folder.
The origin host OS is not part of the protocol; normal file I/O is enough.

## Simple installation

- Windows: double-click `install_offline_assets.bat` (Python 3.10+ needed).
- Linux/other desktop: `sh install_offline_assets.sh`.
- Headless: `python3 tools/install_offline_assets.py --terminal`.

The wizard asks for the mounted source folder, a **protected archive outside the
share**, a per-user local cache, and whether to install the Maya picker script
into the user's scripts directory. By default the archive and cache are under
the user's local Tourmaline data folder. Select auto-start for a user-login
launcher on Windows or a systemd **user** service on Linux. There is no database
server or internet dependency. The repository and Python interpreter must remain
at their install paths (the launcher refers to them).

On Windows, install the agent on a machine that can see the mapped `G:` at
login. A background service running under another user may not have that mapped
drive; if necessary use a UNC path, e.g. `\\\\nas\\project`.

After setup, browse <http://127.0.0.1:8765/> for assets and revisions.
The API binds **127.0.0.1 only**. It does not expose the share over HTTP.

## What it does

- Artists and agents freely create, delete, rename, or overwrite files anywhere
  under the share, including OBJ, FBX, EXR, PNG, Maya files, and arbitrary names.
- The scanner periodically checks settled files, copies their **entire bytes**
  into protected content-addressed SHA-256 blob storage, and commits a SQLite
  revision entry after the copy is complete.
- Deleting a source only marks its path missing; archived versions remain.
- Duplicate bytes share one immutable blob. Path history is deliberately
  path-based in this first slice; detecting renames as the same logical asset
  and importing metadata GUIDs can be added later.
- A version resolver materializes selected historical revisions into a local
  content cache with native file extensions for DCC tools. SHA-256 verifies
  cache reads; frequently accessed/pinned files survive size-based eviction.
- A disconnected share does not delete its history. The archive and local
  resolver continue to function without network access, including after a
  service restart.

The source folder is **never edited** by the agent. The protected archive is
never placed in the source tree. Do not let share users write to the archive.


## Maya version picker

Run the installer wizard first, on the **same workstation** that runs Maya.
If **Install Maya version picker script** was selected, the module is copied to
the user's Maya scripts folder and can be imported directly. Start Maya, then
execute in the **Python** tab of Maya's Script Editor:

```python
import tourmaline_asset_versions as tm
tm.show_picker()
```

Optional permanent Maya shelf button:

```python
tm.install_shelf_button()
```

If the wizard's Maya option was disabled, manually add
`tools/maya/` to Maya's Python path or copy `tourmaline_asset_versions.py`
to Maya's user scripts directory. The module must remain importable for a
persistent shelf button.

- Browse any archived path (including a file deleted from the share).
- Select a specific immutable revision and **Import** OBJ / FBX / Maya scene,
  or **Reference** an `.ma` or `.mb` scene.
- For textures and other non-scene assets, click **Resolve texture / file**
  to obtain an absolute, pinned local path for a Maya file/texture node.
- Each import/reference writes a small `network` metadata node **inside the
  Maya scene** recording logical source path, revision, hash and local cache
  location. It does not write any metadata alongside shared source assets.
- API users can call `tm.version_records()` to list scene records and
  `tm.switch_reference_version(record_node, revision)` to switch an existing
  Maya scene reference to a different pinned revision. Use the **Repair this
  scene's pinned Maya references** button to rebind local cache paths after
  opening a scene from another workstation. Imported mesh geometry
  is not live-updated; import the alternate revision as a new object.

The picker communicates with the local service at
`http://127.0.0.1:8765` by default. To override the port, set
`TOURMALINE_ASSET_URL=http://127.0.0.1:PORT` in Maya's environment.

## CLI

```sh
python3 tools/offline_asset_service.py --config PATH/TO/config.json scan
python3 tools/offline_asset_service.py --config PATH/TO/config.json list
python3 tools/offline_asset_service.py --config PATH/TO/config.json versions "models/rock.obj"
python3 tools/offline_asset_service.py --config PATH/TO/config.json resolve "models/rock.obj" --revision 1 --pin
python3 tools/offline_asset_service.py --config PATH/TO/config.json status
python3 tools/offline_asset_service.py --config PATH/TO/config.json serve
```

Read-only localhost endpoints: `/health`, `/v1/status`, `/v1/assets`,
`/v1/versions?path=models/rock.obj`,
`/v1/resolve?path=models/rock.obj&revision=1&pin=1`.
Resolve populates the cache and returns a local path; it never restores into the
live share. There is no destructive restore endpoint.

## Safety boundaries and limitations

**Polling is not a filesystem transaction hook.** A fast overwrite followed by
another overwrite before the next scan can lose the intermediate state; an
existing file can be overwritten before the service ever captured it.
The stable-copy probe rejects concurrent file-stat changes, but cannot guarantee
that a writer will not pause between chunks, especially over SMB. The
post-write settle window helps reduce partial captures; it cannot eliminate all
of them. Guaranteeing *every* historical write requires SMB-server snapshots,
write interception, or a server-owned write path, regardless of whether that
server uses Linux or Windows.

The Maya version browser is included (see below). There is no C++ Tourmaline
editor integration yet, no remote worker/cooker, no cross-machine service API,
no credentials/authentication and no guaranteed coherent multi-file snapshot. It supplies the offline service, source archival, cache,
localhost API, and installer that the future Maya/Editor version picker can use.
Only run one scanning instance for a given archive database. Back up the
protected archive independently.

Run regression checks:

```sh
python3 -m unittest discover -s tools/tests -p test_offline_asset_service.py -v
```
