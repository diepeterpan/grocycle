# Grocycle

> **Native Linux Directory Growth Monitor & Interactive TUI Explorer**

[![CI](https://github.com/diepeterpan/grocycle/actions/workflows/ci.yml/badge.svg)](https://github.com/diepeterpan/grocycle/actions/workflows/ci.yml)
[![Latest Release](https://img.shields.io/github/v/release/diepeterpan/grocycle?color=blue)](https://github.com/diepeterpan/grocycle/releases)
[![Language](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Platform](https://img.shields.io/badge/Platform-Linux-orange.svg)](https://www.kernel.org/)
[![Database](https://img.shields.io/badge/Storage-SQLite%20WAL-lightgrey.svg)](https://www.sqlite.org/wal.html)
[![TUI](https://img.shields.io/badge/TUI-FTXUI-brightgreen.svg)](https://github.com/ArthurSonzogni/FTXUI)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

Grocycle solves a ubiquitous system administration challenge: **"What directory is growing the fastest right now, and exactly where is that growth occurring?"**

Unlike standard disk usage utilities (`du`, `ncdu`, `df`) that only reflect instantaneous static size, Grocycle combines a low-overhead background snapshot daemon (`grocycled`) with an interactive, colored terminal explorer (`grocycle`) to calculate, rank, and visualize storage **growth velocity** (bytes/hour and growth deltas) across customizable time horizons (from **10 minutes** up to **30 days**).

---

## Architecture & How It Works

```
+-------------------------------------------------------------------------+
|                               USER / CLI                                |
+-------------------------------------------------------------------------+
              |                                            |
              | (IPC: Unix Domain Socket)                  | (Read-Only SQL)
              v                                            v
+-------------------------------+            +----------------------------+
|         grocycle CLI          |            |     grocycle TUI (FTXUI)   |
| (Service control, config,     |            | (Drill-down tree, colors,  |
|  headless top growth reports) |            |  time-window picker, modals|
+-------------------------------+            +----------------------------+
              |                                            |
              +---------------------+----------------------+
                                    |
+-------------------------------------------------------------------------+
|                       IPC / Local Control Socket                        |
|                  (/run/grocycle/grocycle.sock)                          |
+-------------------------------------------------------------------------+
                                    ^
                                    | (Commands: status, scan, config)
+-------------------------------------------------------------------------+
|                        grocycled (Background Service)                   |
|  * I/O priority: ionice IDLE (IOPRIO_CLASS_IDLE)                        |
|  * CPU priority: nice -n 19 / SCHED_IDLE                                |
|  * Staggered traversal: rate-limited getdents64 / fstatat               |
|  * Filesystem boundary awareness (one-file-system / st_dev check)       |
|  * O_NOATIME (prevents disk write thrashing during scans)               |
+-------------------------------------------------------------------------+
                                    |
                                    v (WAL mode transactions)
+-------------------------------------------------------------------------+
|                    Storage: SQLite Database (WAL Mode)                  |
|                      (/var/lib/grocycle/grocycle.db)                    |
+-------------------------------------------------------------------------+
```

### High-Level Data Flow
1. **Background Collection (`grocycled`)**: At each scheduled interval (default: 1 hour; min: 10m; max: 30d), the daemon performs a throttled walk over configured filesystem roots and writes directory metrics into an embedded SQLite database.
2. **Normalized Hierarchy**: Directory paths are deduplicated into a hierarchical path dictionary, keeping disk storage compact even across thousands of snapshots.
3. **Concurrent Analytical Queries**: Thanks to SQLite's **Write-Ahead Logging (WAL)**, the interactive TUI or CLI can query deltas between arbitrary snapshot timestamps instantaneously with zero lock contention.
4. **Dual Size Metrics**: Grocycle records both **allocated block size** (`st_blocks * 512`, true physical space) and **apparent size** (`st_size`), correctly accounting for sparse files and compression.

---

## Design Principles: Low-Impact & Staggered Traversal

Grocycle is engineered specifically to avoid degrading system performance during disk scans:

* **Kernel I/O Priority (`ionice -c 3`)**: Uses the `ioprio_set` system call to assign `IOPRIO_CLASS_IDLE`. The Linux kernel will only service Grocycle disk requests when no other process is waiting for I/O.
* **CPU Priority (`nice -n 19`)**: Configured with the lowest CPU scheduling priority and `SCHED_IDLE`/`SCHED_BATCH` scheduler policy.
* **Staggered Sleep Throttling**: A built-in token-bucket rate limiter pauses execution (default: 15ms every 250 directories) during traversal, avoiding I/O saturation and queue buildup.
* **Filesystem Boundary Guard (`st_dev`)**: By default, scans respect the filesystem boundary of the monitored root. It will not traverse into pseudo-filesystems (`/proc`, `/sys`, `/dev`), docker overlays, or network mounts (`nfs`, `cifs`, `smb`).
* **`O_NOATIME` Filesystem Flag**: Directory inspections open file descriptors with `O_NOATIME`, ensuring file access timestamps are not written back to disk.

---

## Installation & Packaging

Grocycle provides pre-built `.deb` and `.tar.gz` packages directly on the [**GitHub Releases**](https://github.com/diepeterpan/grocycle/releases) page for every release.

### Method 1: Debian / Ubuntu (`.deb` Package) — Recommended

Download the latest `.deb` package from [Releases](https://github.com/diepeterpan/grocycle/releases) and install:

```bash
# Install the Debian package (auto-starts daemon service)
sudo apt install ./grocycle_*_amd64.deb

# Verify the service is running
grocycle service status
```

To remove:
```bash
sudo apt remove grocycle        # keeps configuration and historical data
sudo apt purge grocycle         # removes configuration and data database
```

---

### Method 2: Portable Linux Tarball (`.tar.gz`)

For non-Debian distributions (Fedora, RHEL, Arch, Alpine, etc.):

```bash
# 1. Extract the tarball
tar -xzf dist/grocycle_1.0.0_linux_x86_64.tar.gz
cd grocycle_1.0.0_linux_x86_64

# 2. Run the automated installer
sudo ./install.sh
```

To uninstall later:
```bash
sudo ./uninstall.sh
```

---

### Method 3: Build from Source (Using Docker Dev Container)

A container environment is provided so no host compiler or library dependencies are required:

```bash
# 1. Build the developer container
docker build -t grocycle-dev -f Dockerfile.dev .

# 2. Compile executables and run test suite
docker run --rm -v $(pwd):/workspace grocycle-dev bash -c "mkdir -p build && cd build && cmake .. && make -j\$(nproc) && ctest --output-on-failure"

# 3. Generate .deb and .tar.gz distribution packages
docker run --rm -v $(pwd):/workspace grocycle-dev bash -c "cd build && make package"

# 4. Code Quality & Linting (clang-tidy & clang-format)
docker run --rm -v $(pwd):/workspace grocycle-dev bash -c "cd build && make lint"    # Static analysis
docker run --rm -v $(pwd):/workspace grocycle-dev bash -c "cd build && make format"  # Code auto-formatting
```

---

### Method 4: Run with Docker Compose (Background Monitor + Web App)

Grocycle can be run as a containerized stack using Docker Compose. The container automatically starts the background growth monitor daemon (`grocycled`) and exposes the interactive Web Terminal UI (`grocycle web`) directly in your browser.

```bash
# 1. (Optional) Customize environment variables such as monitored host path or port
cp .env.example .env

# 2. Build and launch the container in the background
docker compose up -d --build

# 3. Open the interactive web terminal explorer in your browser
#    URL: http://localhost:8080
```

#### Common Container Tasks

```bash
# Inspect service status and active scan progress inside container
docker compose exec grocycle grocycle service status

# Query top growing directories in terminal
docker compose exec grocycle grocycle top

# View live container logs
docker compose logs -f

# Gracefully stop the container
docker compose down
```

For advanced configuration (such as scan interval, custom exclusions, HTTP basic auth, or custom mount paths), edit your `.env` file before launching.


### Developer & Maintainer Scripts

Two helper scripts are provided in `scripts/` to streamline pre-checkin validation and releases:

#### 1. Pre-Checkin Verification (`scripts/verify.sh`)
Runs auto-formatting (`clang-format`), compilation, the unit test suite (`ctest`), static analysis (`clang-tidy`), and packaging tests. Automatically runs inside the `grocycle-dev` Docker container if Docker is present:
```bash
./scripts/verify.sh
```

#### 2. Interactive Release Helper (`scripts/release.sh`)
Fetches tags, shows the top 5 most recent release tags, prompts for a new semantic version tag (e.g., `v1.0.1`), tags the commit, and pushes to `origin`. This triggers the GitHub Actions release workflow to publish distribution packages:
```bash
./scripts/release.sh
```

---

## Interactive Terminal UI (TUI) Explorer

Launch the full-color interactive tree explorer:

```bash
grocycle
# or explicitly:
grocycle tui
```

```
 GROCYCLE  Linux Directory Growth Explorer               [M/F2: Daemon Settings] [Q: Quit] 
───────────────────────────────────────────────────────────────────────────────────────────
Path: /var/log
Window: [1:10m] [2:1h] [3:6h] [4:24h] [5:7d] [6:30d]     Sort: [G]rowth | [R]ate | [S]ize | [N]ame
───────────────────────────────────────────────────────────────────────────────────────────
  Directory Name                    Current Size    Growth (Δ)    Growth Rate     Relative 
> journal/                              2.45 GiB    +350.2 MiB     350.2 MiB/h    [████████  ]
  nginx/                              412.00 MiB     +45.1 MiB      45.1 MiB/h    [█         ]
  audit/                              120.50 MiB      +2.1 MiB       2.1 MiB/h    [░         ]
  apt/                                 18.20 MiB           0 B           0 B/h    [          ]
───────────────────────────────────────────────────────────────────────────────────────────
Viewing: /var/log | Snapshots: #12 -> #13 (4 entries)   Enter: Drilldown | Backspace: Up | Arrows
```

### Keybindings & Controls

| Key | Action |
| :--- | :--- |
| `Up` / `Down` or `k` / `j` | Move highlight cursor up or down |
| `Enter` / `Right` or `l` | **Drill down** into selected directory |
| `Backspace` / `Left` or `h` | **Go up** to parent directory |
| `1` – `6` | Quick-switch time window (`10m`, `1h`, `6h`, `24h`, `7d`, `30d`) |
| `G` / `g` | Sort by **Absolute Growth Delta** (Default) |
| `R` / `r` | Sort by **Growth Rate** (Bytes per hour) |
| `S` / `s` | Sort by **Current Total Size** |
| `N` / `n` | Sort **Alphabetically** |
| `M` / `F2` | Open **Daemon Settings & Control Modal** |
| `Q` / `q` | Quit Grocycle |

---

## Web Terminal UI (`grocycle web`)

Grocycle includes an embedded, high-performance C++ web server and WebSocket gateway that streams the full interactive terminal UI directly to any modern browser. It requires **zero external runtime dependencies** and runs 100% offline with bundled assets.

```bash
# Start local web terminal on default port 8080
grocycle web

# Start on custom port and bind across local network
grocycle web --port 9000 --host 0.0.0.0

# Protect with HTTP Basic Authentication
grocycle web -p 8080 -H 0.0.0.0 --auth admin:secret123

# Launch server and immediately open in default browser
grocycle web -p 8080 --browser
```

### Options:
| Flag | Description | Default |
| :--- | :--- | :--- |
| `-p, --port <port>` | TCP port to listen on (range: 1024–65535) | `8080` |
| `-H, --host <ip>` | Network interface IP to bind | `127.0.0.1` |
| `-a, --auth <user:pass>` | Optional HTTP Basic Auth credentials | *(None)* |
| `-b, --browser` | Automatically launch default system browser (`xdg-open`) | *(Disabled)* |
| `--max-clients <n>` | Maximum concurrent web terminal sessions (range: 1–50) | `5` |

Each connected browser tab is assigned an isolated pseudoterminal (PTY) session, allowing multiple users or windows to explore storage metrics independently.

---

## Headless CLI Guide (Scripting & Automation)

### 1. `grocycle top` — Query Top Growing Directories
Ideal for cron jobs, terminal reports, or monitoring scripts:

```bash
# Top 15 fastest growing directories over the last 1 hour (default)
grocycle top

# Top 10 growing directories over the last 24 hours under /var
grocycle top --window 24h --limit 10 --path /var

# Machine-readable JSON output (for alerting pipelines or Datadog/Prometheus exporters)
grocycle top --window 6h -f json
```

Example JSON output:
```json
[
  {
    "path": "/var/log/journal",
    "name": "journal",
    "current_allocated_bytes": 2631925760,
    "delta_allocated_bytes": 367001600,
    "growth_rate_bytes_per_hour": 367001600.0,
    "growth_percent": 16.2
  }
]
```

---

### 2. `grocycle service` — Daemon Management

The CLI communicates with the background daemon over `/run/grocycle/grocycle.sock`:

```bash
# Check service status, active scan progress, and next scheduled scan
grocycle service status

# Trigger an immediate out-of-schedule scan
grocycle service scan

# Stop the running background service
grocycle service stop

# Start the background daemon
grocycle service start
```

---

### 3. `grocycle config` — Dynamic Configuration

Modify settings live without restarting the daemon:

```bash
# Display active configuration and monitored paths
grocycle config show

# Adjust scan interval (Range: 10m to 30d)
grocycle config set-interval 30m
grocycle config set-interval 2h
grocycle config set-interval 7d

# Add or remove monitored filesystem roots
grocycle config add-root /data
grocycle config remove-root /data
```

---

### 4. `grocycle clean` — Database Reset & Purge

Wipe all historical metrics, directory snapshots, and path dictionaries to start measuring from a clean slate:

```bash
# Prompt for confirmation before cleaning:
grocycle clean

# Clean immediately without confirmation prompt and initiate a fresh scan:
grocycle clean -y
```

* Works seamlessly whether `grocycled` is currently running or stopped.
* Automatically compacts the database (`VACUUM`) and schedules an immediate fresh scan so growth measurement restarts cleanly.

---

## Configuration Reference

The system configuration file resides at `/etc/grocycle/config.json` (or `~/.config/grocycle/config.json` for user-session runs):

```json
{
    "db_path": "/var/lib/grocycle/grocycle.db",
    "socket_path": "/run/grocycle/grocycle.sock",
    "log_file": "/var/log/grocycle.log",
    "scan_interval_seconds": 3600,
    "sleep_every_n_dirs": 250,
    "sleep_duration_ms": 15,
    "monitored_roots": [
        "/"
    ],
    "excluded_prefixes": [
        "/proc",
        "/sys",
        "/dev",
        "/run",
        "/tmp",
        "/var/tmp"
    ]
}
```

### Configuration Parameters

| Parameter | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `db_path` | String | `/var/lib/grocycle/grocycle.db` | Path to SQLite database file. |
| `socket_path` | String | `/run/grocycle/grocycle.sock` | Unix domain socket for IPC commands. |
| `scan_interval_seconds`| Integer| `3600` (1 hour) | Seconds between scans (Min: `600`, Max: `2592000`). |
| `sleep_every_n_dirs` | Integer | `250` | Traversal batch threshold before sleeping. |
| `sleep_duration_ms` | Integer | `15` | Sleep duration in milliseconds during traversal. |
| `monitored_roots` | Array | `["/"]` | List of target filesystem roots to measure. |
| `excluded_prefixes` | Array | `["/proc", ...]` | Path prefixes automatically skipped during scanning. |

---

## Systemd Service Management

When installed as a system service, `grocycled` is managed through standard `systemctl` commands:

```bash
# Check service unit status
sudo systemctl status grocycle.service

# View live daemon logs
sudo journalctl -u grocycle.service -f

# Restart daemon
sudo systemctl restart grocycle.service
```

---

## Troubleshooting & FAQ

#### Q: How much disk space does the database consume?
The SQLite database stores directory metrics with deduplicated paths. Historical snapshots older than 30 days are automatically pruned at the end of each scan. For typical systems, the database size remains under 20–50 MB.

#### Q: Can I run Grocycle without root?
Yes. If run as an unprivileged user, Grocycle defaults to user-local paths:
- Config: `~/.config/grocycle/config.json`
- Database: `~/.local/share/grocycle/grocycle.db`
- Socket: `~/.cache/grocycle/grocycle.sock`

Note: Running as an unprivileged user will only measure directories that your user account has read permissions to traverse. To measure system directories (e.g. `/var/log`, `/var/lib/docker`), run `grocycled` as root or grant `CAP_DAC_READ_SEARCH`.

#### Q: How do I verify the daemon is responsive?
Run `grocycle service status`. If the daemon is running, it will return the PID, interval, active scan state, and last scan timestamp.

---

## License

MIT License. See [LICENSE](LICENSE) for details.
