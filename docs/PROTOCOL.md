# Redmi Buds 6 Lite control protocol

This document describes the protocol the official **Xiaomi Earbuds** app uses to read
battery levels and to control noise cancelling (ANC), transparency and the equalizer on
**Redmi Buds 6 Lite**. It was reverse engineered for interoperability: by observing the
traffic and the app's behaviour, and by testing every command on a real pair of earbuds.

Everything below was verified on one pair of Redmi Buds 6 Lite from a Windows 11 PC unless
marked as *unverified*. Other Xiaomi earbuds that expose the same service very likely speak
the same protocol, but their config IDs and value ranges may differ.

A ready-made C++17 implementation (no Windows dependencies) lives in
[`Komimi/include/device/MmaProtocol.h`](../Komimi/include/device/MmaProtocol.h).

---

## 1. Transport

| | |
|---|---|
| Link | Classic Bluetooth (BR/EDR), **RFCOMM** |
| Service UUID | `0000FD2D-0000-1000-8000-00805F9B34FB` (16-bit UUID `0xFD2D`, assigned to Xiaomi) |
| Fallback | Serial Port Profile `00001101-0000-1000-8000-00805F9B34FB` (the app tries it second; FD2D was enough here) |
| Authentication | **None needed** on FD2D: the earbuds answer commands right after the socket opens |
| Clients | **One at a time.** While one program holds the RFCOMM channel, another cannot connect |

The app also contains a BLE variant (service `AF00`, write characteristic `AF05`, notify
characteristic `AF06`). A Windows laptop does not see these earbuds over BLE, so the BLE path
is *unverified*.

The underlying stack is JieLi RCSP. RCSP has an optional authentication handshake; it was not
required on the FD2D channel.

The earbuds do **not** push their state when a client connects — ask for it (see
[section 6](#6-typical-session)).

---

## 2. Frame format

Every message in both directions is one frame. RFCOMM is a byte stream, so frames can arrive
split or glued together — reassemble them by the header and the length field.

```
FE DC BA | flags | opcode | length (2 bytes, big-endian) | body (length bytes) | EF
```

| Field | Meaning |
|---|---|
| `FE DC BA` | Start marker |
| `flags` | `0x80` = command (otherwise a response), `0x40` = the sender expects a response, bits 0–2 = target app |
| `opcode` | See [section 3](#3-opcodes) |
| `length` | Size of the body only |
| `EF` | End marker |

**Body of a command:** `sn, payload…` — `sn` is a sequence number chosen by the sender
(one byte, wraps around). The reply carries the same `sn`.

**Body of a response:** `status, sn, payload…` — `status` `0x00` means success; any other
value is an error (the codes were not catalogued).

Commands sent by the PC use `flags = C0` (command + response expected). Commands sent by the
earbuds were seen with `flags = C7`.

### Acknowledging commands from the earbuds

The earbuds send some commands on their own (`0E`, `F4`) with the "response expected" bit set.
**Reply to each of them with `status = 0`**, otherwise they keep repeating it:

```
FE DC BA 00 <opcode> 00 02 00 <sn> EF
```

---

## 3. Opcodes

| Opcode | Name | Direction | Payload |
|---|---|---|---|
| `02` | GetTargetInfo | PC → earbuds | 4-byte attribute mask, big-endian (`FF FF FF FF` = everything) |
| `08` | SetTargetInfo | PC → earbuds | Attribute list (see below). Type `4` **switches the ANC mode** |
| `09` | GetDeviceRunInfo | PC → earbuds | 4-byte attribute mask (`FF FF FF FF`) |
| `0E` | ReportDeviceStatus | earbuds → PC | Attribute list; needs an acknowledgement |
| `F2` | SetDeviceConfig | PC → earbuds | Config list |
| `F3` | GetDeviceConfig | PC → earbuds | List of config IDs, 2 bytes each, big-endian |
| `F4` | NotifyDeviceConfig | earbuds → PC | Config list; needs an acknowledgement |

### Attribute list (`02`, `08`, `09`, `0E`)

```
[len][type][value × (len − 1)] [len][type][value…] …
```

`len` counts the type byte plus the value bytes.

### Config list (`F2`, `F3` response, `F4`)

```
[len][id high][id low][value × (len − 2)] …
```

`len` counts the two ID bytes plus the value bytes. A `len` of `FF` marks a special format for
large data; it is not needed for anything in this document.

---

## 4. Reading state

### Battery

**On request** — `02 GetTargetInfo`, attribute **7**: `[left][right][case]`.
**Pushed by the earbuds** — `0E ReportDeviceStatus`, attribute **0**: same three bytes.

Each byte: bits 0–6 = percent, bit 7 = charging, `FF` = no data (for example, the case is
closed or an earbud is in it).

```
>> FE DC BA C0 02 00 05 01 FF FF FF FF EF               GetTargetInfo, sn 01, all attributes
<< FE DC BA 00 02 00 46 00 01 … 04 07 5F 2C FF … EF     attribute 7: left 95 %, right 44 %, case —

<< FE DC BA C7 0E 00 06 17 04 00 5E 2C FF EF            pushed: left 94 %, right 44 %
>> FE DC BA 00 0E 00 02 00 17 EF                        acknowledgement
```

Other attributes seen in the `02` response: **0** = device name (UTF-8), **3** = 4 bytes
`27 17 50 8B` (`0x2717` is Xiaomi's USB vendor ID, so probably vendor/product ID). The rest
were not decoded.

### Current ANC mode

`09 GetDeviceRunInfo`, attribute **9** = the mode that is **actually active**:
`0` off, `1` noise cancelling, `2` transparency.

```
>> FE DC BA C0 09 00 05 02 FF FF FF FF EF
<< FE DC BA 00 09 00 35 00 02 07 00 XX XX XX XX XX XX 07 01 XX XX XX XX XX XX … 02 09 01 … EF
                                                                               ^^^^^^^^ attribute 9 = 1 (ANC)
```

Attributes 0 and 1 of this response hold 6-byte Bluetooth addresses (redacted above).

### Stored ANC level and equalizer preset

`F3 GetDeviceConfig` with the config IDs you want:

```
>> FE DC BA C0 F3 00 05 03 00 0B 00 07 EF               configs 0x000B and 0x0007
<< FE DC BA 00 F3 00 07 00 03 04 00 0B 01 05 … EF       0x000B = [mode 1, level 5]
```

> **Do not use `0x000B` from `F3` as the current mode.** It is the *stored* `[mode, level]`
> pair, which can differ from what is active. Read the real mode with `09`.

---

## 5. Changing settings

### Switching ANC mode — `08`, type 4

This is what the earbud gesture and the official app do. The earbuds use the level stored for
the target mode.

```
>> FE DC BA C0 08 00 04 04 02 04 02 EF                  [len 2][type 4][mode 2 = transparency]
<< FE DC BA C7 F4 00 06 4E 04 00 0B 02 03 EF            pushed: now [transparency, level 3]
>> FE DC BA 00 F4 00 02 00 4E EF                        acknowledgement
<< FE DC BA 00 08 00 02 00 04 EF                        response, status 0
```

After every real switch (from `08` or from a gesture) the earbuds push `F4` with config
`0x000B` holding the new mode and its level. Use it to stay in sync with gestures.

### Setting the ANC / transparency level — `F2` then `08`

> **The trap:** writing `0x000B` with `F2` looks like it changes the mode — the earbuds answer
> `status 0` and `F3` reads the new value back — but **it only stores the setting**. Nothing
> changes until the mode is switched with `08`.

To apply a level: store it with `F2`, then switch with `08` — this works even if that mode is
already active.

```
>> FE DC BA C0 F2 00 06 0A 04 00 0B 01 02 EF            store [ANC, level 2 = Deep]
>> FE DC BA C0 08 00 04 0B 02 04 01 EF                  switch to ANC
<< FE DC BA 00 F2 00 02 00 0A EF                        stored
<< FE DC BA C7 F4 00 06 52 04 00 0B 01 02 EF            pushed: now [ANC, Deep]
<< FE DC BA 00 08 00 02 00 0B EF                        switched
```

The earbuds remember the level **separately for each mode**: switching to transparency
restores the transparency level, switching back restores the ANC level.

### Equalizer preset — `F2`, config `0x0007`

Applied immediately and remembered by the earbuds.

```
>> FE DC BA C0 F2 00 05 <sn> 03 00 07 05 EF             preset 5 = enhanced bass
<< FE DC BA 00 F2 00 02 00 <sn> EF
```

Both the ANC level and the EQ preset are written with `F2`; if you send both, tell the
responses apart by `sn`.

---

## 6. Typical session

1. Open RFCOMM to service `FD2D`.
2. Ask for the state: `02` (battery), `09` (real ANC mode), `F3` with `00 0B 00 07`
   (stored level, EQ preset).
3. Keep reading frames: answer every `0E` / `F4` with `status 0`, update battery from `0E` and
   mode/level from `F4`.
4. To change things use `08` (mode), `F2` + `08` (level), `F2` (EQ).

---

## 7. Values

### Config IDs

| ID | Name | Value | Notes |
|---|---|---|---|
| `0x0007` | EQ preset | 1 byte | `F2` applies immediately |
| `0x000A` | Gesture mode cycle | 2 bytes (left, right) | Which modes the gesture cycles through; seen as `07 07`, probably a bit mask — *unverified* |
| `0x000B` | Stored ANC setting | `[mode][level]` | `F2` stores only; applied by the next `08` |
| `0x000C` | Wear status | 1 byte | Changes when an earbud is taken out; seen as `0C`; bits not decoded |

### ANC modes

| Value | Mode |
|---|---|
| `0` | Off |
| `1` | Noise cancelling |
| `2` | Transparency |

### Noise cancelling levels (mode 1)

| Value | Level |
|---|---|
| `0` | Balanced |
| `1` | Light |
| `2` | Deep |
| `3` | Adaptive |
| `4` | Anti-wind |
| `5` | Deep+ |

### Transparency levels (mode 2)

| Value | Level |
|---|---|
| `0` | Transparent |
| `1` | Voice enhance |
| `2` | Ambient enhance |
| `3` | Voice enhance+ |

### EQ presets

The official app offers exactly five presets for Redmi Buds 6 Lite:

| Value | Preset |
|---|---|
| `0` | Balanced sound |
| `1` | Enhanced vocal |
| `5` | Enhanced bass |
| `6` | Enhanced treble |
| `10` | Custom (the curve set up in the official app) |

Values from the shared Xiaomi list that the earbuds accept but that are not meant for this
model: `2` Rock, `3` Classic, `4` Pop, `7` Volume boost, `13` Soothing bass, `14` Harman,
`16` Standard.

---

## 8. Not covered yet

- Custom EQ curve (band values)
- Gesture configuration beyond `0x000A`
- Meaning of the wear-status bits, remaining `02` / `09` attributes, firmware version
- Error status codes
- The BLE transport
