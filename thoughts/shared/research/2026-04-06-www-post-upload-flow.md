---
date: 2026-04-06T00:00:00-07:00
researcher: Joseph Julicher
git_commit: 442a2f1
branch: main
repository: pyro_fw
topic: "POST /www/ web file upload flow and failure modes"
tags: [research, codebase, http_server, littlefs, install, upload]
status: complete
last_updated: 2026-04-06
last_updated_by: Joseph Julicher
---

# Research: POST /www/ Web File Upload Flow and Failure Modes

**Date**: 2026-04-06  
**Researcher**: Joseph Julicher  
**Git Commit**: 442a2f1  
**Branch**: main  
**Repository**: pyro_fw

## Research Question

Uploading web files with POST (see `support/install.py`) is failing with "Remote end closed connection without response".

## Summary

The `upload_www()` function in `support/install.py` uploads files by POSTing raw bytes to `http://192.168.7.1/www/<filename>`. The firmware's `http_server.c` handles this route with a per-connection LittleFS mount-and-write pattern. There are **three code paths** where the firmware calls `tcp_close(pcb)` without sending any HTTP response — all three produce exactly the "Remote end closed connection without response" Python exception. The most structurally significant of these is an `lfs_mount` failure, which would occur whenever another part of the firmware has LittleFS concurrently mounted (specifically during active flight logging via `hal_fs_open`). Additionally, every flash write during an upload disables all interrupts, which momentarily suspends USB processing.

## Detailed Findings

### 1. `install.py` — Client Side (`support/install.py:58-74`)

`upload_www()` iterates sorted files in `www/` and for each:

```python
req = urllib.request.Request(
    f"http://{host}/www/{name}",
    data=data,        # raw bytes, no explicit Content-Type
    method="POST"
)
urllib.request.urlopen(req, timeout=10)
```

- **No explicit `Content-Type`** is set; Python's urllib adds `Content-type: application/x-www-form-urlencoded` automatically.
- **`Content-Length`** is set automatically by urllib from `len(data)`.
- **`timeout=10`** is a socket-level inactivity timeout (not total transfer time).
- Uploads are **sequential** — each `urlopen` blocks until a response is received (or the socket times out/closes).

The error "Remote end closed connection without response" is Python's `http.client.RemoteDisconnected`. It is raised when the TCP connection is closed (FIN received) before **any** HTTP bytes are received. This is distinct from a socket timeout.

### 2. Firmware — POST `/www/` Handler (`src/http_server.c:485-539`)

The handler is in `on_recv()` and is entered on the **first** TCP packet for a POST request.

**Header parsing** (`http_server.c:330-363`):
- HTTP request is copied into `char hdr[512]` (capped at 511 bytes).
- `method`, `path` (max 63 chars), and `content_length` are parsed from `hdr`.
- `body_start = strstr(hdr, "\r\n\r\n")` determines where body begins.
- Body data present in the first packet (`body_in_first`) is written immediately.

**Route check** (`http_server.c:485`):
```c
} else if (strncmp(path, "/www/", 5) == 0 && content_length > 0) {
```
Both conditions must be true. A zero-length file POST fails this check and falls to the `400 Bad Request` else-branch (which does send a response — not the failure being investigated).

**Happy path** (when everything succeeds):
1. `conn_alloc()` — get a free `conn_state_t` from an 8-slot static pool
2. `lfs_mount(&cs->lfs, &lfs_pico_flash_config)` — mount LittleFS on this connection's own `lfs_t`
3. `lfs_mkdir(&cs->lfs, "/www")` — create `/www` dir (return value ignored; `LFS_ERR_EXIST` is acceptable)
4. `lfs_file_open(&cs->lfs, &cs->file, path, LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC)` — open/truncate the file
5. Write body data from the first packet in 512-byte chunks
6. Return; subsequent packets handled by the `CONN_RECEIVING_FILE` branch (`http_server.c:271-295`)
7. When `cs->remaining == 0`: call `conn_free()` (closes file, unmounts LFS, marks slot idle), then send `201 Created`
8. `on_sent` fires, calls `conn_free()` again (no-op since `lfs_mounted` is already false), then `tcp_close(pcb)`

### 3. Three Silent Failure Paths — The Source of "No Response"

All three of the following call `tcp_close(pcb)` and send **no** HTTP response:

| Location | Condition | Code |
|---|---|---|
| `http_server.c:487-490` | `conn_alloc()` returns NULL (all 8 slots busy) | `tcp_close(pcb)` |
| `http_server.c:493-498` | `lfs_mount()` returns non-zero | `tcp_close(pcb)` |
| `http_server.c:502-507` | `lfs_file_open()` returns non-zero | `tcp_close(pcb)` |

There is no error response to the client in any of these cases — the firmware simply closes the TCP connection, which produces exactly the observed Python exception.

### 4. LittleFS Concurrent-Mount Risk (`src/hal_hardware.c:489-518`)

`hal_hardware.c` defines a **persistent** streaming file handle `hw_file`:

```c
static struct hal_file hw_file;  // hal_hardware.c:489

hal_file_t *hal_fs_open(const char *path, bool append) {
    if (hw_file.open) return NULL;
    if (lfs_mount(&hw_file.lfs, &lfs_pico_flash_config) != LFS_ERR_OK) return NULL;
    ...
    hw_file.open = true;
    return &hw_file;
}

void hal_fs_close(hal_file_t *f) {
    lfs_file_close(&f->lfs, &f->file);
    lfs_unmount(&f->lfs);       // hal_hardware.c:516
    f->open = false;
}
```

`hal_fs_open` is called in two places:
- `hal_hardware.c:699` — `hal_log_start()` opens `flight_log.csv` for the entire duration of flight logging
- `flight_states.c:583` — opens `flight.csv` during a flight state transition

While `hw_file.open == true`, `hw_file.lfs` is a **mounted LittleFS instance** on the same `lfs_pico_flash_config` storage as the HTTP server's per-connection `lfs_t` instances.

LittleFS does not support concurrent mounts of the same storage through separate `lfs_t` instances. If `hal_log_start()` has been called and not yet followed by the matching `hal_fs_close()` call at `hal_hardware.c:670`, any HTTP `lfs_mount()` call will fail, causing the firmware to silently close the TCP connection.

`hal_fs_read_file` and `hal_fs_write_file` (`hal_hardware.c:446-478`) also mount and unmount within a single call, so they are transient and don't create lasting conflicts by themselves.

### 5. Flash Write Interrupt Disable (`src/littlefs_driver.c:34-55`)

Every flash program or erase operation in the LittleFS driver disables all interrupts:

```c
static int pico_prog(const struct lfs_config *c, ...) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(fs_base(c) + p, buffer, size);
    restore_interrupts(ints);
    return 0;
}

static int pico_erase(const struct lfs_config *c, lfs_block_t block) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(fs_base(c) + off, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    return 0;
}
```

These are called from within `lfs_file_write()` during `on_recv()`. During each interrupt-disabled window, USB interrupts cannot fire, temporarily suspending the ECM network interface. This doesn't produce the "no response" error by itself, but it may cause TCP stalls on the host for large file uploads.

### 6. `conn_state_t` Pool and Double-Free Pattern (`src/http_server.c:41-62, 288-294`)

- Pool size is 8 (`conn_pool[8]`).
- On upload completion (`remaining == 0`): `conn_free(cs)` is called inside `on_recv`, freeing the slot and unmounting LFS. The 201 response is then queued.
- `tcp_arg(pcb, cs)` still points to the now-freed slot.
- When `on_sent` fires for the 201 response, `cs->phase == CONN_IDLE`, so it falls to the `else` branch and calls `conn_free(cs)` again (safe — `lfs_mounted` is already false) then `tcp_close(pcb)`.
- If a new connection arrives and reuses that same `conn_pool` slot between the `on_recv` completion and the `on_sent` callback firing, `on_sent` would call `conn_free` on the **new** connection's state and close the wrong PCB.

## Code References

- `support/install.py:58-74` — `upload_www()`: sequential POST loop, timeout=10, no explicit Content-Type
- `support/install.py:46-56` — `curl_post()`: OTA upload path, swallows all exceptions
- `src/http_server.c:485-539` — POST `/www/` handler
- `src/http_server.c:271-295` — `CONN_RECEIVING_FILE` continuation handler
- `src/http_server.c:41-62` — `conn_pool[8]`, `conn_alloc`, `conn_free`
- `src/http_server.c:487-490` — silent `tcp_close` on `conn_alloc` failure
- `src/http_server.c:493-498` — silent `tcp_close` on `lfs_mount` failure
- `src/http_server.c:502-507` — silent `tcp_close` on `lfs_file_open` failure
- `src/hal_hardware.c:489-518` — `hw_file` persistent LFS handle, `hal_fs_open`/`hal_fs_close`
- `src/hal_hardware.c:696-699` — `hal_log_start()` opens `flight_log.csv` keeping LFS mounted
- `src/flight_states.c:583,609` — `hal_fs_open("flight.csv", ...)` during state transitions
- `src/littlefs_driver.c:34-55` — `pico_prog`/`pico_erase` with `save_and_disable_interrupts`
- `src/littlefs_driver.c:62-74` — `lfs_pico_flash_config` (single global config used by all `lfs_t` instances)

## Architecture Documentation

The HTTP server uses a **per-connection `lfs_t`** pattern: each incoming connection that touches the filesystem allocates its own `conn_state_t` containing an `lfs_t` struct and mounts LittleFS fresh. Unmount happens in `conn_free()`. This pattern is used for both GET (serving files) and POST (uploading files).

The HAL layer (`hal_hardware.c`) uses a **single global `hw_file`** with a persistent LFS mount for streaming writes (flight logging). This global mount coexists with the HTTP server's per-connection mounts on the same `lfs_pico_flash_config` storage.

The OTA handler (`/api/ota`) bypasses LittleFS entirely, writing directly to a dedicated flash slot via `flash_range_erase`/`flash_range_program` with interrupts disabled.

## Open Questions

- Under what flight state conditions (if any) might `hw_file.open == true` when a user attempts to upload web files?
- Should the three silent `tcp_close` failure paths send an error HTTP response instead?
- Does the `on_sent`/`conn_pool` reuse race (§6) manifest in practice with rapid sequential uploads?
