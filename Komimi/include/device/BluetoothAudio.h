#pragma once
#include <cstdint>

// Connects and disconnects a Bluetooth device's audio (stereo and hands-free) the same way the
// Connect / Disconnect buttons in Windows Bluetooth settings do: through the one-shot
// KSPROPSETID_BtAudio properties of the Bluetooth audio driver.
//
// Synchronous and quick (the driver only takes the request), call on a COM-initialized thread.
namespace BluetoothAudio {
    // Number of audio profiles the request reached; 0 = Windows has no audio endpoints for this device
    int Connect(uint64_t address);
    int Disconnect(uint64_t address);
}
