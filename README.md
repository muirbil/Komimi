# Komimi

An unofficial Windows tray app for **Redmi Buds 6 Lite**: battery levels, noise cancelling,
transparency and equalizer — without the phone.

<p align="center"><img src="docs/tray popup.png" alt="Tray flyout" width="340"></p>

The earbuds' control protocol was reverse engineered for this project and is documented in
**[docs/PROTOCOL.md](docs/PROTOCOL.md)** — useful on its own if you want to support these
earbuds somewhere else.

> Not affiliated with, endorsed by or connected to Xiaomi. "Xiaomi" and "Redmi" are trademarks
> of their respective owners and are used here only to name the supported device.

## Features

- Battery of the left earbud, right earbud and case, with a charging indicator
- Noise control: off / noise cancelling / transparency
- Noise cancelling level: Balanced, Light, Deep, Adaptive, Anti-wind, Deep+
- Transparency mode: Normal, Voice, Ambient, Voice+
- Equalizer presets: Balanced, Vocal, Bass, Treble, Custom
- Stays in sync when you switch modes with the earbud gesture
- Finds the paired earbuds automatically and reconnects when you take them out of the case
- Flyout next to the tray icon in the style of Windows 11, light and dark theme, system accent
  colour, per-monitor DPI
- Start with Windows (shares the switch with Task Manager → Startup apps)
- English, Russian and Japanese

## Requirements

- Windows 11, x64 (Windows 10 should work too, but has not been tested)
- Redmi Buds 6 Lite **paired in Windows Bluetooth settings**

The app looks among paired devices for one whose name contains "Redmi Buds 6 Lite" (renamed
earbuds are fine as long as the model name stays in it) or that offers Xiaomi's Bluetooth
service.

Only one program can talk to the earbuds at a time.

## Usage

- **Left click** the tray icon to open the flyout. Click outside it or press Esc to close.
- **Right click** for the menu: Reconnect, Start with Windows, Language, Exit.
- The icon turns grey while the earbuds are not connected.

## Building

1. Install **Visual Studio 2022 or newer** with the *Desktop development with C++* workload
   and a Windows 11 SDK (10.0.22000 or newer).
2. Open `Komimi.sln`. The `Microsoft.Windows.CppWinRT` NuGet package is restored on the
   first build. Newer Visual Studio versions may offer to retarget the platform toolset —
   accept.
3. Pick **Release | x64** and build. The app ends up in `bin\x64\Release\Komimi.exe`.

Starting a second copy closes the running one, so F5 in Visual Studio always runs the fresh
build even when the app is already in the tray.

Everything sent to and received from the earbuds is written to the debug output as hex — see
the Output window in Visual Studio or use
[DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview).

## Project layout

```
Komimi/
  include/ src/
    app/      entry point, localization, autostart, logging
    device/   protocol (MmaProtocol.h), RFCOMM client, state engine
    ui/       tray icon, flyout (Direct2D / DirectWrite), theme
  res/        icon and version resources
docs/         protocol description, screenshot
```

- **`include/device/MmaProtocol.h`** — frame building/parsing and payload decoding in plain
  C++17 without any Windows dependencies. Copy it if you need the protocol elsewhere.
- **`include/device/XiaomiBudsClient.h`** — C++/WinRT RFCOMM client on top of it.

No GUI framework: the flyout is drawn with Direct2D and DirectWrite, the rest is plain Win32.

### Adding a language

Add the language to `Language` in `include/app/Localization.h`, add a string table to
`src/app/Localization.cpp` (a `static_assert` checks that no string is missing) and a menu
item in `src/app/main.cpp`.

## Limitations

- Tested on one pair of Redmi Buds 6 Lite with Windows 11. Other Xiaomi earbuds using the same
  service may partly work but have not been tried.
- The custom EQ curve can be selected but not edited.

## Related projects

- [BudsTray](https://github.com/zhangjichuan1009-create/BudsTray) — WPF tray app for Xiaomi/Redmi
  and 1MORE earbuds, verified on Redmi Buds 8 Pro and 1MORE SonoFlow S.
- [Gadgetbridge](https://gadgetbridge.org/gadgets/headphones/xiaomi) — Android; supports
  Redmi Buds 5 Pro among other Xiaomi earbuds.

## Acknowledgements

- [ElectronicCats/jieli-ble-badge-research](https://github.com/ElectronicCats/jieli-ble-badge-research) —
  background on the JieLi RCSP stack the earbuds are built on.

## License

MIT — see [LICENSE.txt](LICENSE.txt).
