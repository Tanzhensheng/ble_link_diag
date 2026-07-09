# BLE Link Diagnostic

Out-of-tree diagnostic tool for validating the 005 robot BLE host link/protocol layer on Ubuntu/BlueZ.

The tool builds a small executable against the robot project's `ble_link.c` and calls:

- `ble_link_open`
- `ble_link_send`
- `ble_link_receive`
- `ble_link_close`
- `ble_protocol_send_packet`
- `ble_protocol_receive_packet`

Raw hex mode is intended for link-layer byte send/receive checks. `--packet` mode exercises the 005 BLE protocol layer, including ACK, packet send, packet receive, and receive-side ACK.

## Build

```bash
sudo apt install -y bluez libglib2.0-dev pkg-config gcc

cd ~/ble_link_diag
chmod +x build_linux.sh
export ROBOT_REPO=~/bluetooth/csp_whl_robot
./build_linux.sh
```

`ROBOT_REPO` must point to a checkout containing:

```text
src/platform/module/mod_ble/ble_link.c
src/platform/module/mod_ble/ble_link.h
src/platform/module/mod_ble/ble_protocol.c
src/platform/module/mod_ble/ble_protocol.h
```

## Run

Open/close only:

```bash
sudo ./ble_link_diag <mac> <service_uuid> <write_char_uuid> <notify_char_uuid>
```

Send one hex payload and wait for one Notify response:

```bash
sudo ./ble_link_diag <mac> <service_uuid> <write_char_uuid> <notify_char_uuid> "A5 06 00 A5 43 00 80 10 01 02 03 D9 96" 3000
```

Send one near-field protocol packet and wait for a complete protocol packet reply:

```bash
sudo ./ble_link_diag <mac> <service_uuid> <write_char_uuid> <notify_char_uuid> --packet '{"topic":"/sdttu/subdevice/realtime/get","body":{"messageId":"diag-1","timestamp":"2026-07-09 00:00:00","body":[]}}' 8000
```

Expected success indicators:

```text
ble_link_open ret=0
ble_link_send ret=0
ble_link_receive ret=<positive length>
rx len=<positive length>: ...
```

Protocol packet success indicators:

```text
ble_protocol_open ret=0
ble_protocol_send_packet ret=0
ble_protocol_receive_packet ret=0
rx_payload text="..."
```

## Notes

- Run on Ubuntu/Linux with BlueZ. Windows cannot run the `org.bluez` D-Bus path.
- Start Notify must be established by `ble_link_open` before the first payload is sent.
- The `stubs/robot_include.h` file only provides logging macros for this standalone diagnostic binary.
