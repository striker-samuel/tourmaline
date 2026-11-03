# Tourmaline Asset Repository — Windows/Linux NAS manual

This is the setup manual for a **single authoritative shared asset service** on a
Windows or Linux NAS, plus per-workstation caches. A local/offline alternative
is covered in the last section. Requires Python 3.10+; the code uses the
standard library only. Tkinter is optional for the graphical installers.

## Design and identity boundaries

| Layer | Files | Identity | Purpose |
| --- | --- | --- | --- |
| Working sources | Maya .ma/.mb, .obj, .fbx, .exr, .png, arbitrary files | **source UUID**, protected immutable source revisions | Artist/agent-writable SMB folder with arbitrary layout |
| Published representations | .glb, .ktx2, processed/approved mesh or texture files | **product UUID** for source + representation + variant; immutable published versions | Explicitly published copies OUTSIDE the source share |
| Cooked build catalog | .pak, .tapp, generated project.h | Existing Tourmaline **content IDs** in assets.db | Runtime build contents only; NO source version history |

The existing cooked catalog is **untouched**. This repository does not assign
cooked IDs, rewrite .meta sidecars, change project.h, or change .pak/.tapp
formats. Source identities and published products are separate database tables.

Each source UUID is allocated in the protected repository database. An
overwrite retains identity. A single unambiguous same-content move may retain
identity, but ambiguous moves aren't guessed. Duplicate bytes are deduplicated
even when two source UUIDs are different.

## Recommended folders

Linux NAS:

~~~text
/srv/smb/project/                        # only unrestricted source assets
  characters/hero.ma
  models/hero.obj
  textures/albedo.exr

/var/lib/tourmaline/repository/archive/  # protected content + SQLite history
/var/lib/tourmaline/repository/cache/    # replaceable server cache
/etc/tourmaline/shared-assets.json       # server settings + token
~~~

Windows NAS:

~~~text
D:\Shares\project\                       # actual NAS share root
D:\TourmalineHistory\archive\            # private archive, NOT artist-writable
D:\TourmalineHistory\server-cache\
C:\ProgramData\Tourmaline\AssetRepository\server.json
~~~

Windows Maya workstation:

~~~text
G:\project\                              # mapped SMB share (arbitrary layout)
%LOCALAPPDATA%\Tourmaline\SharedAssets\client.json
%LOCALAPPDATA%\Tourmaline\SharedAssetCache\
~~~

The NAS scanner sees its own host-local/mounted source path, **not G:** (a
mapping on the artist's workstation). The archive and cache must be outside
the source share. Protect the archive with filesystem permissions.

## Linux NAS installation

Check out the feature PR branch (until merged):

~~~bash
git clone https://github.com/striker-samuel/tourmaline.git
cd tourmaline
git switch feature/shared-asset-repository-and-catalog
python3 --version
~~~

Interactive GUI wizard:

~~~bash
sh install_shared_assets.sh
~~~

Headless text wizard:

~~~bash
python3 tools/install_shared_assets.py --terminal
~~~

For an **always-on Linux NAS**, configure a dedicated unprivileged account,
protected data directories, and a systemd system service:

~~~bash
sudo useradd --system --home /var/lib/tourmaline --shell /usr/sbin/nologin tourmaline
sudo mkdir -p /var/lib/tourmaline/repository/archive
sudo mkdir -p /var/lib/tourmaline/repository/cache
sudo python3 tools/install_shared_assets.py --source /srv/smb/project --archive /var/lib/tourmaline/repository/archive --cache /var/lib/tourmaline/repository/cache --config /etc/tourmaline/shared-assets.json --listen-lan --install-systemd-system --service-user tourmaline
~~~

Make sure the service account can READ source files, and ordinary artists
cannot WRITE the protected archive/config. The installer doesn't modify
permissions on the artist share. The startup unit refers to the current
checkout and Python interpreter, which must remain accessible.

~~~bash
sudo systemctl status tourmaline-shared-assets
sudo systemctl restart tourmaline-shared-assets
sudo systemctl stop tourmaline-shared-assets
sudo journalctl -u tourmaline-shared-assets -f
~~~

Manual startup:

~~~bash
python3 tools/shared_asset_server.py --config /etc/tourmaline/shared-assets.json serve
~~~

## Windows NAS installation

From the repository root, double-click **install_shared_assets.bat**.
The wizard configures the source, archive, cache, token, LAN binding and
optional user-login startup.

For a dedicated NAS without interactive login, open an **administrator**
PowerShell and install a Windows boot-time scheduled task (one command):

~~~powershell
py -3 tools/install_shared_assets.py --source 'D:\Shares\project' --archive 'D:\TourmalineHistory\archive' --cache 'D:\TourmalineHistory\server-cache' --config 'C:\ProgramData\Tourmaline\AssetRepository\server.json' --listen-lan --install-windows-task
schtasks /Run /TN TourmalineSharedAssetRepository
schtasks /Query /TN TourmalineSharedAssetRepository /V /FO LIST
~~~

This runs under SYSTEM, not the logged-in artist account. Use **host-local**
paths rather than a user-mapped G: drive, and set NTFS ACLs accordingly.
To troubleshoot, run the Python server in a terminal and press Ctrl+C to stop:

~~~powershell
py -3 tools/shared_asset_server.py --config 'C:\ProgramData\Tourmaline\AssetRepository\server.json' serve
~~~

Only one scanner may own an archive SQLite database at a time.

## Shared server configuration

Example server.json (schema 2, replace token with generated secret):

~~~json
{
  "schema_version": 2,
  "mode": "shared",
  "source": "/srv/smb/project",
  "archive": "/var/lib/tourmaline/repository/archive",
  "cache": "/var/lib/tourmaline/repository/cache",
  "access_token": "YOUR_GENERATED_SERVER_TOKEN",
  "bind": "0.0.0.0",
  "port": 8765,
  "poll_seconds": 3.0,
  "settle_seconds": 2.0,
  "cache_gib": 20,
  "consumer_ttl_seconds": 90
}
~~~

- **source**: unrestricted source tree; artists write their files here.
- **archive**: permanent protected source AND published-product bytes/history.
- **cache**: replaceable server-local materialization cache.
- **access_token**: shared repository secret (minimum 24 characters).
- **bind/port**: 127.0.0.1 for host-only access, 0.0.0.0 for LAN, default port 8765.
- **poll_seconds**: scanner interval; **settle_seconds**: file-age threshold
  before an observed file is captured.
- **cache_gib**: evictable server cache budget; pinned versions may exceed it.
- **consumer_ttl_seconds**: stale consumer heartbeat expiry (default 90 s).

Restart after editing configuration. The server requires authentication for
assets, publishes and read-presence data. Plain HTTP on a LAN does **not**
encrypt the access token or assets: for production, use a trusted LAN or
preferably an HTTPS reverse proxy, restrict the firewall, and never expose
port 8765 directly to the Internet. Back up the WHOLE archive, including
history.sqlite3 and blobs. SQLite needs a consistent backup/snapshot.

## Configure a Windows or Linux workstation

Windows: run **configure_asset_client.bat**.

Linux: run **sh configure_asset_client.sh**.

Headless: run **python3 tools/configure_asset_client.py --terminal**.

Example Windows PowerShell:

~~~powershell
py -3 tools/configure_asset_client.py --url 'http://nas.local:8765' --token 'YOUR_GENERATED_SERVER_TOKEN' --cache "$env:LOCALAPPDATA\Tourmaline\SharedAssetCache" --install-maya --check
~~~

The client saves a JSON config:
- Windows: %LOCALAPPDATA%\Tourmaline\SharedAssets\client.json
- Linux: ~/.config/Tourmaline/SharedAssets/client.json

~~~json
{
  "schema_version": 1,
  "url": "https://assets.example.lan",
  "token": "YOUR_GENERATED_SERVER_TOKEN",
  "cache": "C:\\Users\\Artist\\AppData\\Local\\Tourmaline\\SharedAssetCache"
}
~~~

The workstation wizard also installs both the Maya picker and verified
cache module. In Maya's Python Script Editor:

~~~python
import tourmaline_asset_versions as tm
tm.show_picker()
tm.install_shelf_button()  # optional persistent shortcut
~~~

The picker works with independent source UUIDs and selects immutable
revisions. It downloads content to a local hashed cache, verifying SHA-256
before use; subsequent selects reuse cached bytes rather than streaming from
the NAS again. The working source share is never modified by a resolve.

## Source revisions versus published representations

Artist copies/overwrites a source (e.g. G:\project\characters\hero.ma).
The scanner records observed settled whole-file revisions, without a
checkout. Find stable IDs and revisions on the NAS:

~~~bash
python3 tools/shared_asset_server.py --config /path/to/server.json scan
python3 tools/shared_asset_server.py --config /path/to/server.json sources
python3 tools/shared_asset_server.py --config /path/to/server.json versions SOURCE_UUID
~~~

Publishing a representation is a different, deliberate action. For example:

~~~bash
python3 tools/shared_asset_server.py --config /path/to/server.json publish SOURCE_UUID 3 mesh /path/to/hero.glb --variant game
~~~

Remote worker/workstation publishing:

~~~python
from tourmaline_asset_cache import AssetClient
client = AssetClient("https://assets.example.lan", "YOUR_GENERATED_SERVER_TOKEN")
result = client.publish_product(
    "SOURCE_UUID", 3, "mesh", "C:/work/build/hero.glb", variant="game")
print(result["product_id"], result["version"])
~~~

This records the source UUID, pinned source revision/hash, representation,
variant, immutable published byte hash and product version. Published
representations live in private storage, NOT in G:\project. Existing cooked
catalog identities remain independent.

## Observing readers (no locks)

Open **http://NAS_HOST:8765/consumers**, or its HTTPS reverse-proxied
equivalent. Enter the server token in the page to view which instrumented
clients report using which source and published-product versions. The page
refreshes every ten seconds.

A client heartbeat is an advisory, expiring **complete set** of reads. An
empty set means idle; sessions expire after the configured TTL. It does not
block writes, grant access, or inspect normal raw SMB file handles.
Clients can release their presence explicitly on exit.

Maya's shared-cache client automatically announces source/product versions
it resolves and sends periodic heartbeats while running. Custom editors and
agents can use the endpoint themselves:

~~~bash
curl -X POST https://assets.example.lan/v2/consumers/heartbeat -H 'Authorization: Bearer YOUR_GENERATED_SERVER_TOKEN' -H 'Content-Type: application/json' -d '{"session_id":"11111111-1111-4111-8111-111111111111","label":"agent-layout","kind":"agent","reads":[{"kind":"source","id":"SOURCE_UUID","version":3}]}'
~~~

Use real canonical source/product UUIDs and existing versions. A session may
announce several reads. Each heartbeat replaces that session's whole read
set. To release:

~~~bash
curl -X POST https://assets.example.lan/v2/consumers/release -H 'Authorization: Bearer YOUR_GENERATED_SERVER_TOKEN' -H 'Content-Type: application/json' -d '{"session_id":"11111111-1111-4111-8111-111111111111"}'
~~~

A filtered GET /v2/consumers?resource_kind=source&resource_id=SOURCE_UUID
shows only sessions reading that source. Products use resource_kind=product.
Labels are self-reported, not verified individual authentication. This
infrastructure can support future collaboration visibility but intentionally
contains NO locking/checkout/write ownership semantics.

## API summary

| Endpoint | Purpose |
| --- | --- |
| GET /health | Liveness, no authentication |
| GET /v2/sources | Source UUIDs and current paths |
| GET /v2/sources/{uuid}/revisions | Immutable source revisions |
| GET /v2/resolve?asset_id={uuid}&revision=N | Source revision metadata/hash |
| GET /v2/products?source_asset_id={uuid} | Published representations and versions |
| GET /v2/blobs/{sha256} | Authenticated immutable-byte streaming |
| PUT /v2/publish | Explicit immutable product publication |
| GET /consumers | Dashboard login page (data fetched with token) |
| GET /v2/consumers | Active readers, optionally filtered |
| POST /v2/consumers/heartbeat | Replace/refresh a consumer's read set |
| POST /v2/consumers/release | Remove one consumer session |
| GET /v1/assets and /v1/versions | Original path-based legacy views |

Legacy /v1/resolve may return a NAS-host-local path; remote clients should
use /v2/resolve plus /v2/blobs and their own verified cache.

## Offline-only alternative and operational limits

For one workstation with no NAS service, use the existing installers:
**install_offline_assets.bat** or **sh install_offline_assets.sh**.
Or run the legacy service using its config:

~~~bash
python3 tools/offline_asset_service.py --config PATH/TO/config.json serve
~~~

The local/offline archive can resolve previously captured revisions with the
source share disconnected; it does NOT automatically sync history to the
shared repository. See offline_asset_service.md for details.

The scanner currently uses polling and a settle interval, not an SMB
write-interception transaction hook. Rapid intermediate overwrites can be
missed. Strict pre-overwrite protection requires NAS filesystem snapshots
or a write interception path. There is no coherent multi-file transaction
snapshot yet; pin compatible texture/scene revisions yourself.

A workstation with cached bytes can keep using those local paths while
the NAS is down, but fresh metadata queries need the server. Read-presence
only tracks cooperating clients, and stale sessions expire automatically.

## Validation

~~~bash
python3 -m unittest discover -s tools/tests -p test_shared_asset_repository.py -v
python3 -m unittest discover -s tools/tests -p test_asset_read_presence.py -v
python3 -m unittest discover -s tools/tests -p test_offline_asset_service.py -v
python3 -m unittest discover -s tools/tests -p test_maya_asset_versions.py -v
~~~

Shared asset repository CI tests run on Windows/Linux with Python 3.10/3.12.
Maya automation uses mocked Maya APIs; interactive Maya validation is still
necessary.
