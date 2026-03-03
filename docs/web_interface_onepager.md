# Pyro MK1B Web Interface

## Configure your flight computer from any browser. No drivers. No special software.

---

**Plug in USB. Open your browser. You're ready to fly.**

The Pyro MK1B presents itself as a USB network device. Your laptop automatically connects. Navigate to `http://pyro.local/` and you have full control.

---

### Live Pre-Flight Dashboard

See everything at a glance before you walk to the pad:

- ✅ **Pyro 1**: Good continuity
- ✅ **Pyro 2**: Good continuity  
- 📊 **Altitude**: 0m (calibrated)
- 🔋 **Battery**: 7.9V
- ⚙️ **Mode**: PAD_IDLE — Ready for launch

### Edit Configuration Instantly

Change deployment settings right in your browser. No file transfers, no ejecting, no corruption risk.

```
Pyro 1: Delay 0s after apogee (drogue)
Pyro 2: AGL 300m (main)
Units: meters
```

Click **Save**. Done. Config validated on-device before it's written.

### Download Flight Data

After recovery, plug in and download your CSV with one click. View it in Excel, MATLAB, or any tool you prefer.

---

### Why Web Instead of USB Drive?

| | USB Drive (traditional) | Web Interface (Pyro MK1B) |
|---|---|---|
| **Setup** | Plug in, find drive in Finder | Plug in, open browser |
| **Edit config** | Open file in text editor, save, eject | Edit in browser, click Save |
| **See device status** | ❌ Not possible | ✅ Live dashboard |
| **Verify pyro continuity** | ❌ Need separate tool | ✅ Shown on dashboard |
| **Risk of corruption** | ⚠️ Must eject safely | ✅ No corruption possible |
| **Special software** | None | None |
| **Works on** | macOS, Windows, Linux | macOS, Windows, Linux |

---

### Technical Details

- **Connection**: USB CDC-ECM network device (no drivers needed on macOS/Linux, RNDIS for Windows)
- **Address**: `http://pyro.local/` via mDNS or `http://192.168.7.1/`
- **Storage**: littlefs on internal flash with built-in wear leveling
- **Firmware overhead**: ~48KB code, ~20KB RAM additional
- **Protocol**: Standard HTTP — works with any browser, curl, wget, or scripts
- **Simultaneous access**: Firmware reads config while you browse. No conflicts.

---

*Pyro MK1B — Because your flight computer should be as easy to configure as your router.*
