#pragma once
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.System.h>
#include "device/XiaomiBudsClient.h"

enum class LinkState {
    Disconnected, // was connected and the link dropped
    Connecting,
    Connected,
    NotFound,     // could not connect: earbuds in the case, off or not paired
};

// A code, not text: translating it is the UI's job
enum class EngineError {
    None,
    AncWriteFailed,
    EqWriteFailed,
};

// Stored levels per mode, indexed by AncModes. The earbuds remember a separate level for each mode.
using AncLevels = std::array<std::optional<uint8_t>, 3>;

// Everything the UI knows about the earbuds.
struct EarbudsState {
    LinkState link = LinkState::NotFound;
    std::optional<BudsConstants::Xiaomi::Buds6Lite::AncModes> anc; // actual mode; nullopt = not known yet
    AncLevels ancLevels;                                            // nullopt = level of this mode not known yet
    bool ancPending = false;                                        // command sent, waiting for the response
    std::optional<XiaomiMma::EqPreset> eq;                          // EQ preset; nullopt = not known yet
    bool eqPending = false;
    XiaomiMma::BatteryInfo battery;                                 // percent == nullopt = no data
    EngineError error = EngineError::None;                          // last error, to show in the UI
};

// Engine is the business logic layer on top of XiaomiBudsClient (RFCOMM + MMA protocol).
//
// Client callbacks arrive on the WinRT thread pool; Engine marshals them to the
// UI thread through DispatcherQueue, so State() and onStateChanged live
// on the UI thread only and no mutexes are needed.
class Engine {
public:
    explicit Engine(winrt::Windows::System::DispatcherQueue uiQueue);
    ~Engine();
    Engine(Engine const&) = delete;
    Engine& operator=(Engine const&) = delete;

    void Initialize(); // finds the paired earbuds and connects to them
    void Reconnect();
    void SetAncMode(BudsConstants::Xiaomi::Buds6Lite::AncModes mode);
    void SetAncLevel(uint8_t level); // for the current mode (noise cancelling or transparency)
    void SetEqPreset(XiaomiMma::EqPreset preset);

    EarbudsState const& State() const { return m_state; }

    // Called on the UI thread whenever State() changes
    std::function<void()> onStateChanged;

private:
    winrt::fire_and_forget ConnectAsync();
    winrt::fire_and_forget WatchDeviceAsync();
    void SetLink(LinkState link);
    void FinishAncRequest(bool ok);
    void FinishEqRequest(bool ok);
    void NotifyChanged();

    winrt::Windows::System::DispatcherQueue m_uiQueue{ nullptr };
    winrt::Windows::System::DispatcherQueueTimer m_ancTimeout{ nullptr };
    winrt::Windows::System::DispatcherQueueTimer m_eqTimeout{ nullptr };
    winrt::Windows::System::DispatcherQueueTimer m_autoConnect{ nullptr };
    uint64_t m_deviceAddress = 0; // 0 = the earbuds have not been found among paired devices yet
    EarbudsState m_state;
    // Last state confirmed by the earbuds: we roll back to it if a command fails
    std::optional<BudsConstants::Xiaomi::Buds6Lite::AncModes> m_confirmedAnc;
    AncLevels m_confirmedLevels;
    std::optional<XiaomiMma::EqPreset> m_confirmedEq;
    bool m_retriedConnect = false;

    // We watch the classic Bluetooth connection to connect to the earbuds
    // as soon as Windows connects them (taken out of the case)
    winrt::Windows::Devices::Bluetooth::BluetoothDevice m_device{ nullptr };
    winrt::event_token m_deviceStatusToken{};

    std::shared_ptr<XiaomiBudsClient> m_client = std::make_shared<XiaomiBudsClient>();
};
