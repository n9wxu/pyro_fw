# Hardware CI Plan — Proxmox Self-Hosted Runners

## Overview
4 Pyro MK1B boards on Proxmox VMs as GitHub Actions self-hosted runners.
2× MS5607 sensor, 2× BMP280 sensor. 4 parallel jobs.

## Architecture

```
Proxmox Host
├── VM: pyro-hw-1 (MS5607) → USB passthrough → Pyro #1
├── VM: pyro-hw-2 (MS5607) → USB passthrough → Pyro #2
├── VM: pyro-hw-3 (BMP280) → USB passthrough → Pyro #3
└── VM: pyro-hw-4 (BMP280) → USB passthrough → Pyro #4
└── 6 spare boards for dev/field use
```

## Runner Labels

| VM | Labels | Board |
|---|---|---|
| pyro-hw-1 | `self-hosted, hardware, ms5607` | MS5607 #1 |
| pyro-hw-2 | `self-hosted, hardware, ms5607` | MS5607 #2 |
| pyro-hw-3 | `self-hosted, hardware, bmp280` | BMP280 #1 |
| pyro-hw-4 | `self-hosted, hardware, bmp280` | BMP280 #2 |

## Proxmox USB Passthrough

Bind by physical port so each VM always gets the same board:

```bash
# Identify ports
lsusb -t

# Assign by bus-port (example)
qm set 101 -usb0 host=1-2    # pyro-hw-1 gets port 1-2
qm set 102 -usb0 host=1-3    # pyro-hw-2 gets port 1-3
qm set 103 -usb0 host=1-4    # pyro-hw-3 gets port 1-4
qm set 104 -usb0 host=2-1    # pyro-hw-4 gets port 2-1
```

Both VID/PID pairs needed (app mode + BOOTSEL):
- `2e8a:4002` — Pyro MK1B application mode
- `2e8a:0003` — RP2040 BOOTSEL mode

## VM Setup (each VM — lightweight Ubuntu 22.04)

```bash
# System deps
sudo apt update
sudo apt install -y cmake ninja-build gcc-arm-none-eabi libnewlib-arm-none-eabi \
  python3-pip git curl
pip3 install pyserial

# USB permissions
sudo usermod -aG dialout,plugdev $USER

# Install Pico SDK
git clone --depth 1 --branch 2.2.0 https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init lib/tinyusb lib/mbedtls lib/lwip lib/btstack lib/cyw43-driver

# Install picotool
# (built from Pico SDK or download binary)

# Install GitHub Actions runner
mkdir ~/actions-runner && cd ~/actions-runner
curl -o actions-runner.tar.gz -L https://github.com/actions/runner/releases/latest/download/actions-runner-linux-x64-2.321.0.tar.gz
tar xzf actions-runner.tar.gz

# Configure (get token from repo Settings → Actions → Runners → New)
./config.sh --url https://github.com/n9wxu/pyro_fw \
  --token <REGISTRATION_TOKEN> \
  --name pyro-hw-1 \
  --labels self-hosted,hardware,ms5607

# Install and start as service
sudo ./svc.sh install
sudo ./svc.sh start
```

Repeat for pyro-hw-2 (ms5607), pyro-hw-3 (bmp280), pyro-hw-4 (bmp280).

## Workflow: .github/workflows/hardware.yml

```yaml
name: Hardware Tests

on:
  push:
    branches: [main]
  workflow_dispatch:

jobs:
  build:
    runs-on: ubuntu-latest
    outputs:
      version: ${{ steps.ver.outputs.version }}
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake ninja-build gcc-arm-none-eabi libnewlib-arm-none-eabi
      - name: Install Pico SDK
        run: |
          git clone --depth 1 --branch 2.2.0 https://github.com/raspberrypi/pico-sdk.git /tmp/pico-sdk
          cd /tmp/pico-sdk && git submodule update --init lib/tinyusb lib/mbedtls lib/lwip lib/btstack lib/cyw43-driver
      - name: Build
        run: |
          mkdir build && cd build
          cmake -G Ninja -DPICO_SDK_PATH=/tmp/pico-sdk ..
          ninja
        env:
          CI_BUILD: 1
      - name: Read version
        id: ver
        run: echo "version=$(cat VERSION)" >> $GITHUB_OUTPUT
      - name: Upload firmware
        uses: actions/upload-artifact@v4
        with:
          name: firmware
          path: |
            build/pyro_fw_c.uf2
            build/pyro_fw_c.bin
            build/pyro_fw_c_fota_image.bin
            build/_deps/pico_fota_bootloader-build/pico_fota_bootloader.uf2
            www/
            support/

  hardware-test:
    runs-on: [self-hosted, hardware]
    needs: build
    strategy:
      matrix:
        sensor: [ms5607, bmp280]
      max-parallel: 4
    concurrency:
      group: hw-${{ runner.name }}
      cancel-in-progress: false
    steps:
      - uses: actions/checkout@v4

      - uses: actions/download-artifact@v4
        with:
          name: firmware

      - name: Flash firmware
        run: ./support/flash_picotool.sh

      - name: Upload web files
        run: ./support/upload_www.sh 192.168.7.1

      - name: Wait for device
        run: |
          for i in $(seq 1 30); do
            curl -s --max-time 2 http://192.168.7.1/api/status && break
            sleep 1
          done

      - name: Verify firmware version
        run: |
          VER=$(curl -s http://192.168.7.1/api/status | python3 -c "import sys,json; print(json.load(sys.stdin)['fw_version'])")
          echo "Device running: $VER"

      - name: Verify sensor detected
        run: |
          # Check that the device detected a pressure sensor (not stuck in boot)
          STATE=$(curl -s http://192.168.7.1/api/status | python3 -c "import sys,json; print(json.load(sys.stdin)['state'])")
          echo "State: $STATE"
          [ "$STATE" = "PAD_IDLE" ] || [ "$STATE" = "LANDED" ]

      - name: Run network tests
        run: python3 support/test_network.py 192.168.7.1 --all --log hw-test.log

      - name: Test config save/reboot cycle
        run: |
          # Save a test config
          curl -s -X POST -H "Content-Type: text/plain" \
            -d $'[pyro]\r\npyro1_mode=delay\r\npyro1_value=5\r\npyro2_mode=agl\r\npyro2_value=200\r\nunits=ft\r\n' \
            http://192.168.7.1/api/config
          # Reboot
          curl -s -X POST http://192.168.7.1/api/reboot
          sleep 8
          # Verify config applied
          P2VAL=$(curl -s http://192.168.7.1/api/status | python3 -c "import sys,json; print(json.load(sys.stdin)['pyro2_value'])")
          echo "pyro2_value after reboot: $P2VAL"
          [ "$P2VAL" = "200" ]
          # Restore default config
          curl -s -X POST -H "Content-Type: text/plain" \
            -d $'[pyro]\r\npyro1_mode=delay\r\npyro1_value=0\r\npyro2_mode=agl\r\npyro2_value=300\r\nunits=m\r\n' \
            http://192.168.7.1/api/config
          curl -s -X POST http://192.168.7.1/api/reboot
          sleep 8

      - name: Test OTA update
        run: |
          # OTA the same firmware to test the update path
          curl -s -X POST --data-binary @build/pyro_fw_c_fota_image.bin \
            http://192.168.7.1/api/ota
          sleep 10
          VER=$(curl -s http://192.168.7.1/api/status | python3 -c "import sys,json; print(json.load(sys.stdin)['fw_version'])")
          echo "Post-OTA version: $VER"

      - name: Upload test log
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: hw-test-${{ runner.name }}
          path: hw-test.log
```

## Security Notes

- Self-hosted runners on PUBLIC repos can execute arbitrary code from PRs
- Options:
  1. Restrict hardware workflow to `push` only (not `pull_request`) ← recommended
  2. Use `workflow_dispatch` for manual trigger
  3. Make repo private
- The workflow above only triggers on `push` to main

## Verification Checklist

After setup, verify each runner:
- [ ] `lsusb` shows Pyro board in VM
- [ ] `picotool info` detects the board
- [ ] `flash_picotool.sh` succeeds
- [ ] `curl http://192.168.7.1/api/status` returns JSON
- [ ] Runner shows "Idle" in GitHub repo Settings → Actions → Runners
- [ ] Test workflow dispatches and completes on each runner
