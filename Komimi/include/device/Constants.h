#pragma once
#include <winrt/Windows.Foundation.h>
#include <cstdint>
#include "device/MmaProtocol.h" // packet format, opcodes, config IDs, ANC modes

namespace BudsConstants {
    using namespace winrt;

    namespace Xiaomi {
        namespace Buds6Lite {

            // Model name: the window title and the substring used to find the earbuds among paired devices
            // (Windows shows the device name, which can be customised, e.g. "<Name>'s Redmi Buds 6 Lite")
            inline constexpr wchar_t DisplayName[] = L"Redmi Buds 6 Lite";

            // The earbuds' address is not hardcoded: XiaomiBudsClient::FindPairedAsync looks for them among
            // paired devices. A laptop cannot see these earbuds over BLE, so the link is RFCOMM.

            // RFCOMM services in the order the Xiaomi app tries them.
            // FD2D (a 16-bit UUID assigned to Xiaomi) was verified on a real device.
            inline const guid SppMiuiUuid{ 0x0000FD2D, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };
            inline const guid SppUuid{ 0x00001101, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };

            // BLE variant of the same protocol, for reference (Windows does not see these earbuds over BLE).
            inline const guid BleServiceUuid{ 0x0000AF00, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };
            inline const guid BleWriteUuid{ 0x0000AF05, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };
            inline const guid BleNotifyUuid{ 0x0000AF06, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };

            using AncModes = XiaomiMma::AncMode; // Off = 0, NoiseCancelling = 1, Transparency = 2
        }
    }

    namespace Samsung {
        namespace GalaxyBuds {}
    }
}
