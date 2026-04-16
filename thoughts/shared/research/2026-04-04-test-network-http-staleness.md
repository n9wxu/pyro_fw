---
date: 2026-04-04T21:11:38+0000
researcher: Joseph Julicher
git_commit: 111637357a337d93f1c1f3f3953d3e908eff2717
branch: main
repository: pyro_fw
topic: "test_network.py HTTP tests stale relative to current implementation"
tags: [research, codebase, http_server, test_network, web_ui, config, api]
status: complete
last_updated: 2026-04-04
last_updated_by: Joseph Julicher
---

# Research: test_network.py HTTP tests stale relative to current implementation

**Date**: 2026-04-04T21:11:38+0000
**Researcher**: Joseph Julicher
**Git Commit**: 111637357a337d93f1c1f3f3953d3e908eff2717
**Branch**: main
**Repository**: pyro_fw

## Research Question

The `test_network.py` script does not pass the HTTP tests. The script may be stale because the web page updates correctly in the browser. What has changed in the implementation since the test was written, and what are the specific mismatches?

## Summary

`support/test_network.py` was committed on Mar 5, 2026 at `c9ec807` (moved into `support/`). Since then, the HTTP server has received significant updates across ~16 commits, and `www/app.js` has grown from 2,472 to 19,159 bytes. Despite these changes, most of the test's checks still align with the current implementation.

The primary source of test failures is a **structural mismatch in how config is accessed**: the web UI reads config exclusively from `/api/status` JSON (always populated from in-memory defaults), never from `GET /api/config`. The test, however, calls `GET /api/config` and checks for `"[pyro]"` — this file only exists in littlefs if the user has explicitly saved config via `POST /api/config` from the web UI. On a freshly-flashed or never-configured-via-UI device, this file does not exist and the server returns `404 Not Found\r\n\r\nNo config.ini`, causing the test to fail even though the browser shows live data.

## Detailed Findings

### `GET /api/config` — Primary Mismatch

**Test check** (`support/test_network.py:211-214`):
```python
idx = add_test("GET /api/config returns INI")
def t_config(h):
    c = curl(h, "/api/config")
    return "[pyro]" in c, f"{len(c)}b"
```

**Server behavior** (`src/http_server.c:214-218`):
```c
static conn_state_t *serve_api_config(struct tcp_pcb *pcb) {
    return serve_lfs_file_streaming(
        pcb, "config.ini", "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n" CORS_HDR "Connection: close\r\n\r\n",
        "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNo config.ini");
}
```

If `config.ini` is absent from littlefs, the body is `"No config.ini"` — no `[pyro]` → **test FAILS**.

**Why the browser works anyway**: `www/app.js` never issues `GET /api/config`. It reads config from `/api/status` fields (`pyro1_mode`, `pyro1_value`, etc.) which are always populated from in-memory defaults regardless of whether config.ini exists on-flash. Config is only written to littlefs when the user explicitly saves it from the web UI (`POST /api/config`).

```javascript
// www/app.js:44 — only polls /api/status
fetch('/api/status').then(function(r){return r.json()}).then(function(d) {
    // reads d.pyro1_mode, d.pyro1_value, d.rocket_id, etc.
```

```javascript
// www/app.js:206 — writes to /api/config (only on explicit user save)
fetch('/api/config', {method:'POST', headers:{'Content-Type':'text/plain'}, body:ini})
```

### `/api/status` JSON Expansion

The status JSON has grown substantially since the test was written. The test checks for 5 fields; all 5 are still present.

| Attribute | At `c9ec807` | Current |
|-----------|-------------|---------|
| Buffer size | `char buf[512]` | `char buf[768]` |
| State count | 4 (`< 4` guard) | 7 (`< 7` guard) |
| JSON fields | 15 | 22 |

**New fields added** (`src/http_server.c:170-172`): `pyro1_mode`, `pyro1_value`, `pyro2_mode`, `pyro2_value`, `units`, `rocket_id`, `rocket_name`.

**Test field checks** (`support/test_network.py:205`) — all still present in current JSON:
```python
ok = all(k in d for k in ("state", "pressure_pa", "fw_version", "uptime", "pyro1_cont"))
```

### `serve_api_config` Refactored to Streaming

At `c9ec807` the function was synchronous (blocking read loop). Now it uses the shared streaming infrastructure (`serve_lfs_file_streaming`). External behavior is unchanged for HTTP clients.

### File Serving — `www/app.js` Size and Content

| Check | At `c9ec807` | Current | Test threshold |
|-------|-------------|---------|----------------|
| `app.js` size | 2,472 bytes | 19,159 bytes | `> 2000` |
| `index.html` size | — | 7,531 bytes | `> 900` |
| `style.css` size | — | 2,076 bytes | `> 300` |

The `"function update"` substring check (`support/test_network.py:233`) still matches — `function update()` exists at `www/app.js:43`.

`"<!DOCTYPE"` check for `GET /` still matches — `www/index.html:1` starts with `<!DOCTYPE html>`.

### CORS Header

`serve_api_status` includes `CORS_HDR` (`"Access-Control-Allow-Origin: *\r\n"`) at `src/http_server.c:163`. The test's curl `-D-` check for this header still passes.

File serving (`GET /www/app.js`, `/www/style.css`, `/`) does NOT include the CORS header (`src/http_server.c:418-420`), but the test only checks CORS on `/api/status`, so no failure there.

## Code References

- `support/test_network.py:211-214` — `GET /api/config` test, checks for `"[pyro]"`
- `support/test_network.py:195-198` — `GET /api/status` test, checks for `"fw_version"`
- `support/test_network.py:200-208` — JSON field validation test
- `support/test_network.py:224-236` — File serving tests (size + content checks)
- `src/http_server.c:155-181` — `serve_api_status`, 22-field JSON in 768-byte buffer
- `src/http_server.c:214-218` — `serve_api_config`, streams `config.ini` or returns 404
- `src/http_server.c:404` — 404 response for missing files: `"Not found"`
- `www/app.js:43` — `function update()` — status polling, reads all config from `/api/status`
- `www/app.js:206` — `POST /api/config` — only write path, never a GET

## Architecture Documentation

**Config storage flow**:
- Device boots → in-memory defaults loaded from `src/config.c`
- `/api/status` always returns in-memory config fields (no filesystem dependency)
- `GET /api/config` reads `config.ini` from littlefs — only exists after explicit user save
- `POST /api/config` writes `config.ini` to littlefs → device reboots to apply

**File serving**:
- Web files stored at `/www/index.html`, `/www/app.js`, `/www/style.css` in littlefs
- Uploaded via `POST /www/<filename>`
- `GET /` maps to `/www/index.html` in littlefs (`src/http_server.c:381`)
- `DEFAULT_PAGE` fallback served if littlefs mount fails or `/www/index.html` absent (only 175 bytes, fails `> 900` size test)

## Open Questions

- Whether the test should be updated to skip `GET /api/config` when config.ini doesn't exist, or whether the test setup should `POST` a known config before running HTTP tests.
- Whether a default `config.ini` should be written to littlefs on first boot (currently, in-memory defaults are used without creating the file).
