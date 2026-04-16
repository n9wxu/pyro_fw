# Configuration System and Filesystem Analysis

**Date**: 2026-04-11  
**Author**: Research Analysis  
**Status**: Critical Issue Identified

## Executive Summary

The pyro_fw configuration system has a **critical gap**: configuration edits via the HTTP API are persisted to flash storage but **not applied to the running system** until after a power cycle. This creates a dangerous disconnect between what appears to be configured and what the flight software is actually using.

## Architecture Overview

### Filesystem Implementation

- **Used**: littlefs (LFS) - a power-loss resilient filesystem for embedded systems
- **Not Used**: fat_mimic library (present in codebase but unused)
- **Storage**: Flash memory via `lfs_pico_flash_config`
- **Mount Strategy**: Mount/unmount on each file operation for crash safety

### Configuration Data Flow

```
Boot Sequence:
  main() → flight_states_init() → hal_config_load(&ctx->config)
    ↓
  Reads config.ini from littlefs
    ↓
  Parses INI → populates ctx->config (in-memory struct)
    ↓
  Used by flight state machine throughout runtime

HTTP POST /api/config:
  Receives new config data
    ↓
  Writes directly to config.ini in littlefs
    ↓
  ⚠️ DOES NOT update ctx->config
    ↓
  Returns success to client
```

## Code Evidence

### 1. Configuration Loading (src/hal_hardware.c:511-524)

```c
int hal_config_load(config_t *cfg) {
    config_set_defaults(cfg);
    char buf[512];
    int n = hal_fs_read_file("config.ini", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        config_parse_ini(buf, cfg);
        return 0;
    }
    /* No config file — write defaults for next boot */
    const char *def = config_default_ini();
    hal_fs_write_file("config.ini", def, (int)strlen(def));
    return -1;
}
```

**Called once at boot** in `flight_states.c` to populate `ctx->config`.

### 2. Configuration Saving API (src/hal_hardware.c:526-532)

```c
int hal_config_save(const config_t *cfg) {
    char buf[512];
    int n = config_serialize_ini(cfg, buf, (int)sizeof(buf));
    if (n <= 0)
        return -1;
    return hal_fs_write_file("config.ini", buf, n);
}
```

**Critical Finding**: `hal_config_save()` is **never called** anywhere in the codebase.

### 3. HTTP Server POST Handler (src/http_server.c)

```c
} else if (strcmp(path, "/api/config") == 0 && content_length > 0 && content_length < 512) {
    /* Write config.ini to littlefs */
    char cfgbuf[512];
    uint16_t len = (body_in_first < content_length) ? body_in_first : content_length;
    pbuf_copy_partial(p, cfgbuf, len, body_offset);
    pbuf_free(p);
    cfgbuf[len] = '\0';
    
    lfs_t lfs;
    if (lfs_mount(&lfs, &lfs_pico_flash_config) == LFS_ERR_OK) {
        lfs_file_t f;
        if (lfs_file_open(&lfs, &f, "config.ini", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) == LFS_ERR_OK) {
            lfs_file_write(&lfs, &f, cfgbuf, len);
            lfs_file_close(&lfs, &f);
            // ⚠️ NO UPDATE TO IN-MEMORY CONFIG
            lfs_unmount(&lfs);
            resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK";
        }
    }
}
```

**The handler**:
1. ✅ Receives INI config data from HTTP client
2. ✅ Writes to littlefs `config.ini` file
3. ❌ **Does NOT reload/update `ctx->config`**
4. ✅ Returns success response

### 4. Configuration Usage (src/flight_states.c)

The in-memory config is heavily used throughout flight operations:

```c
bool should_fire_pyro(flight_context_t *ctx, uint8_t mode, uint16_t value) {
    // ... uses ctx->config.units for conversion
    int32_t max_units = cm_to_units(MAX_ALTITUDE_CM, ctx->config.units);
    // ...
}

static void try_fire_pyros(flight_context_t *ctx, uint32_t now) {
    if (!ctx->pyro1_fired && !hal_pyro_is_firing() && ctx->pyro1_continuity_good &&
        should_fire_pyro(ctx, ctx->config.pyro1_mode, ctx->config.pyro1_value)) {
        // Uses ctx->config.pyro1_mode and ctx->config.pyro1_value
    }
    // Similar for pyro2...
}
```

**Impact**: Pyro firing logic, telemetry formatting, logging headers all use `ctx->config`.

## The Problem

### Scenario

1. User powers on device → `ctx->config` loaded with **Config A**
2. User edits config via web UI (POST /api/config) → **Config B** written to flash
3. HTTP returns success → User believes **Config B** is active
4. ⚠️ **Device still using Config A** (in `ctx->config`)
5. Flight occurs with wrong pyro settings, units, etc.
6. Only after reboot does **Config B** take effect

### Safety Implications

**Critical**: For a pyrotechnics flight computer, this is a **safety hazard**:

- User sets `pyro1_mode=agl`, `pyro1_value=300` (deploy at 300 units AGL)
- Changes units from `m` to `ft` 
- Expects deployment at 300 ft (~91m)
- **Device still has old config**: deploys at 300m (984 ft)
- Result: Parachute deployment at wrong altitude

### User Experience

Even for non-safety-critical changes:
- User edits rocket name/ID → not reflected in telemetry until reboot
- User changes telemetry format → old format continues
- Confusing: "I changed it but it's not working"

## Root Cause Analysis

1. **Design Intent**: The `hal_config_save()` function exists but is unused
   - Suggests intent to support runtime config updates
   - Never implemented in HTTP handler

2. **HTTP Handler Shortcut**: Direct littlefs write
   - Bypasses `hal_config_save()` 
   - No hook to notify flight software of changes
   - No validation that written config is valid

3. **No Reload Mechanism**: No `hal_config_reload()` or similar
   - Even if HTTP handler wanted to update config, no API exists
   - Would need to:
     - Re-parse config.ini
     - Update `ctx->config`
     - Handle safety implications (mid-flight changes?)

## Potential Solutions

### Option 1: Runtime Config Reload (Recommended)

```c
// In hal.h
int hal_config_reload(config_t *cfg);

// In http_server.c POST /api/config handler
lfs_file_write(&lfs, &f, cfgbuf, len);
lfs_file_close(&lfs, &f);
lfs_unmount(&lfs);

// NEW: Reload into in-memory config
extern void flight_config_reload(void);  // Defined in flight_states.c
flight_config_reload();  // Calls hal_config_reload(&ctx->config)

resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK";
```

**Pros**:
- Immediate config changes
- Better user experience
- Uses existing `config_parse_ini()` infrastructure

**Cons**:
- Need to handle in-flight safety (disallow changes after PAD_IDLE?)
- Thread safety considerations (though single-threaded main loop)
- What if new config is invalid? Need validation before write

### Option 2: Require Reboot (Document Current Behavior)

```c
// In http_server.c
resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"
       "{\"status\":\"ok\",\"message\":\"Config saved. Reboot required.\"}";
```

**Pros**:
- Simple, no code changes to flight software
- Safe: no mid-flight config changes
- User is informed

**Cons**:
- Poor UX: manual reboot required
- User might forget to reboot

### Option 3: Auto-Reboot After Config Change

```c
// In http_server.c after successful write
resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"
       "Config saved. Rebooting...";
tcp_write(pcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
tcp_output(pcb);
// Allow response to be sent
sleep_ms(100);
watchdog_reboot(0, 0, 0);
```

**Pros**:
- Ensures new config is active
- No user action required
- Simple to implement

**Cons**:
- Disruptive: breaks active network connections
- Log data might be lost if in-flight
- User might not expect reboot

### Option 4: State-Based Config Lock

Only allow config changes in PAD_IDLE state, reject during flight:

```c
// In http_server.c
extern flight_state_t flight_get_state(void);
if (flight_get_state() != PAD_IDLE) {
    resp = "HTTP/1.1 409 Conflict\r\n...\r\n"
           "{\"error\":\"Cannot change config during flight\"}";
} else {
    // Write and reload config
}
```

**Pros**:
- Safest: no mid-flight config changes
- Allows runtime changes when safe
- Clear error message

**Cons**:
- More complex implementation
- Requires exposing flight state to HTTP layer

## Recommendations

1. **Short-term** (v2.1.x patch):
   - Update HTTP response to indicate reboot required
   - Add warning in web UI

2. **Medium-term** (v2.2.0 feature):
   - Implement Option 4: State-based config lock
   - Add runtime reload for PAD_IDLE state
   - Add config validation before write

3. **Documentation**:
   - Add to README/user guide: "Configuration changes require device reboot"
   - Web UI should show warning banner after config save

## Related Files

- `src/config.c` - Config parsing/serialization (working correctly)
- `src/config.h` - Config API definition
- `src/hal_hardware.c:511-532` - Config load/save (save unused)
- `src/http_server.c` - HTTP POST handler (missing reload)
- `src/flight_states.c` - Config consumer (working correctly)
- `lib/fat_mimic/` - Unused, can be removed

## Test Plan (If Implementing Runtime Reload)

1. Unit test: config parse/serialize round-trip
2. Integration test: POST config → verify in-memory update
3. Safety test: Attempt config change during flight → rejected
4. Persistence test: Config survives power cycle
5. Invalid config test: Malformed INI → rejected, old config retained

## Conclusion

The pyro_fw configuration system has a working persistence layer (littlefs) and a working in-memory config structure, but lacks the bridge between them for runtime updates. This creates a **critical safety and UX issue** where configuration edits appear to succeed but don't take effect until reboot.

The problem is well-scoped and solvable. The recommended solution is state-based config locking with runtime reload when safe (PAD_IDLE only), with clear user feedback about when reboots are required.