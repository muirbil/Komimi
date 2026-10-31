#include <chrono>
#include "device/Engine.h"
#include "app/Log.h"

using namespace std::chrono_literals;
using BudsConstants::Xiaomi::Buds6Lite::AncModes;
using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using winrt::Windows::Devices::Bluetooth::BluetoothDevice;

namespace {
    // If the earbuds do not answer a mode change within this time, treat it as failed
    constexpr auto kAncResponseTimeout = 3s;
    // Windows reports the connection before the earbuds are ready to accept RFCOMM
    constexpr auto kAutoConnectDelay = 1500ms;
}

Engine::Engine(winrt::Windows::System::DispatcherQueue uiQueue)
    : m_uiQueue(std::move(uiQueue))
{
    // Client callbacks run on background threads and may arrive even during
    // shutdown. So they do not touch Engine here at all: they only put
    // work on the UI thread queue, which stops running once the message loop exits.
    auto queue = m_uiQueue;

    m_client->OnBattery = [queue, this](XiaomiMma::BatteryInfo const& battery) {
        queue.TryEnqueue([this, battery] {
            m_state.battery = battery;
            NotifyChanged();
        });
    };
    m_client->OnAnc = [queue, this](XiaomiMma::AncMode mode) {
        queue.TryEnqueue([this, mode] {
            // Came from the earbuds: a response, a gesture switch or a confirmation of our command
            m_confirmedAnc = mode;
            m_state.anc = mode;
            if (m_state.ancPending) FinishAncRequest(true);
            NotifyChanged();
        });
    };
    m_client->OnAncLevel = [queue, this](XiaomiMma::AncMode mode, uint8_t level) {
        auto index = static_cast<size_t>(mode);
        if (index >= std::tuple_size_v<AncLevels>) return;
        queue.TryEnqueue([this, index, level] {
            m_confirmedLevels[index] = level;
            m_state.ancLevels[index] = level;
            NotifyChanged();
        });
    };
    m_client->OnAncWritten = [queue, this](bool ok) {
        queue.TryEnqueue([this, ok] {
            if (!m_state.ancPending) return;
            FinishAncRequest(ok);
            NotifyChanged();
        });
    };
    m_client->OnEq = [queue, this](XiaomiMma::EqPreset preset) {
        queue.TryEnqueue([this, preset] {
            // A read-back after SetEq or the response at connect time: this is the actual preset
            m_confirmedEq = preset;
            m_state.eq = preset;
            if (m_state.eqPending) FinishEqRequest(true);
            NotifyChanged();
        });
    };
    m_client->OnEqWritten = [queue, this](bool ok) {
        queue.TryEnqueue([this, ok] {
            if (!m_state.eqPending) return;
            FinishEqRequest(ok);
            NotifyChanged();
        });
    };
    // All traffic with the earbuds goes to the debugger's Output window (or DebugView)
    m_client->OnRawBytes = [](bool outgoing, std::vector<uint8_t> const& bytes) {
        std::wstring hex;
        for (uint8_t b : bytes) hex += std::format(L"{:02X} ", b);
        Log(L"{} {}", outgoing ? L">>" : L"<<", hex);
    };
    m_client->OnDisconnected = [queue, this] {
        queue.TryEnqueue([this] {
            Log(L"Earbuds disconnected");
            SetLink(LinkState::Disconnected);
        });
    };

    m_ancTimeout = m_uiQueue.CreateTimer();
    m_ancTimeout.Interval(kAncResponseTimeout);
    m_ancTimeout.IsRepeating(false);
    m_autoConnect = m_uiQueue.CreateTimer();
    m_autoConnect.Interval(kAutoConnectDelay);
    m_autoConnect.IsRepeating(false);
    m_autoConnect.Tick([this](auto&&, auto&&) {
        if (m_state.link == LinkState::NotFound || m_state.link == LinkState::Disconnected) ConnectAsync();
    });

    m_ancTimeout.Tick([this](auto&&, auto&&) {
        if (!m_state.ancPending) return;
        Log(L"ANC command timed out");
        FinishAncRequest(false);
        NotifyChanged();
    });

    m_eqTimeout = m_uiQueue.CreateTimer();
    m_eqTimeout.Interval(kAncResponseTimeout);
    m_eqTimeout.IsRepeating(false);
    m_eqTimeout.Tick([this](auto&&, auto&&) {
        if (!m_state.eqPending) return;
        Log(L"EQ command timed out");
        FinishEqRequest(false);
        NotifyChanged();
    });
}

Engine::~Engine() {
    m_ancTimeout.Stop();
    m_eqTimeout.Stop();
    m_autoConnect.Stop();
    if (m_device) m_device.ConnectionStatusChanged(m_deviceStatusToken);
    m_client->Close();
}

void Engine::Initialize() {
    ConnectAsync();
}

void Engine::Reconnect() {
    ConnectAsync();
}

winrt::fire_and_forget Engine::ConnectAsync() {
    if (m_state.link == LinkState::Connecting) co_return;
    SetLink(LinkState::Connecting);

    // Keep looking among paired devices until found: the earbuds may be paired after startup
    if (!m_deviceAddress) {
        uint64_t found = 0;
        try {
            found = co_await XiaomiBudsClient::FindPairedAsync();
        }
        catch (winrt::hresult_error const& ex) {
            Log(L"Device search error: {}", ex.message().c_str());
        }
        co_await winrt::resume_foreground(m_uiQueue);

        if (!found) {
            Log(L"No paired earbuds found");
            SetLink(LinkState::NotFound);
            co_return;
        }
        Log(L"Found earbuds: {:012X}", found);
        m_deviceAddress = found;
        WatchDeviceAsync();
    }

    bool ok = false;
    try {
        ok = co_await m_client->ConnectAsync(m_deviceAddress);
    }
    catch (winrt::hresult_error const& ex) {
        Log(L"Connect error: {}", ex.message().c_str());
    }
    co_await winrt::resume_foreground(m_uiQueue);

    Log(L"Connect: {}", ok ? L"ok" : L"failed");
    SetLink(ok ? LinkState::Connected : LinkState::NotFound);

    // Windows considers the earbuds connected but RFCOMM did not open: usually the channel
    // has not been released yet (for example, the previous copy of the app has just closed it).
    // Try again, but only once in a row, to avoid looping.
    bool audioConnected = m_device && m_device.ConnectionStatus() == BluetoothConnectionStatus::Connected;
    if (ok) {
        m_retriedConnect = false;
    }
    else if (audioConnected && !m_retriedConnect) {
        m_retriedConnect = true;
        m_autoConnect.Start();
    }
}

winrt::fire_and_forget Engine::WatchDeviceAsync() {
    BluetoothDevice device{ nullptr };
    try {
        device = co_await BluetoothDevice::FromBluetoothAddressAsync(m_deviceAddress);
    }
    catch (winrt::hresult_error const& ex) {
        Log(L"Device lookup error: {}", ex.message().c_str());
    }
    co_await winrt::resume_foreground(m_uiQueue);
    if (!device) co_return;

    m_device = device;
    auto queue = m_uiQueue;
    m_deviceStatusToken = m_device.ConnectionStatusChanged([queue, this](BluetoothDevice const& sender, auto&&) {
        if (sender.ConnectionStatus() != BluetoothConnectionStatus::Connected) return;
        queue.TryEnqueue([this] {
            if (m_state.link != LinkState::NotFound && m_state.link != LinkState::Disconnected) return;
            Log(L"Earbuds connected to Windows, connecting RFCOMM");
            m_autoConnect.Start();
        });
    });
}

void Engine::SetAncMode(AncModes mode) {
    if (m_state.link != LinkState::Connected || m_state.ancPending) return;
    if (m_state.anc == mode) return;

    // Optimistically show the new mode right away; FinishAncRequest rolls it back on failure
    m_state.anc = mode;
    m_state.ancPending = true;
    m_state.error = EngineError::None;
    m_ancTimeout.Start();
    NotifyChanged();
    m_client->SetAnc(mode);
}

void Engine::SetAncLevel(uint8_t level) {
    if (m_state.link != LinkState::Connected || m_state.ancPending) return;
    if (!m_state.anc || *m_state.anc == AncModes::Off) return;

    auto mode = *m_state.anc;
    auto& current = m_state.ancLevels[static_cast<size_t>(mode)];
    if (current == level) return;

    // A level only applies when the mode is switched on, so the client sends F2 (store) + 0x08 (switch)
    current = level;
    m_state.ancPending = true;
    m_state.error = EngineError::None;
    m_ancTimeout.Start();
    NotifyChanged();
    m_client->SetAnc(mode, level);
}

void Engine::SetEqPreset(XiaomiMma::EqPreset preset) {
    if (m_state.link != LinkState::Connected || m_state.eqPending) return;
    if (m_state.eq == preset) return;

    // Optimistically show it right away; FinishEqRequest rolls it back on failure
    m_state.eq = preset;
    m_state.eqPending = true;
    m_state.error = EngineError::None;
    m_eqTimeout.Start();
    NotifyChanged();
    m_client->SetEq(preset);
}

void Engine::FinishEqRequest(bool ok) {
    m_eqTimeout.Stop();
    m_state.eqPending = false;
    if (ok) {
        m_confirmedEq = m_state.eq;
    }
    else {
        m_state.eq = m_confirmedEq;
        m_state.error = EngineError::EqWriteFailed;
    }
}

void Engine::FinishAncRequest(bool ok) {
    m_ancTimeout.Stop();
    m_state.ancPending = false;
    if (ok) {
        m_confirmedAnc = m_state.anc;
        m_confirmedLevels = m_state.ancLevels;
        m_state.error = EngineError::None;
    }
    else {
        m_state.anc = m_confirmedAnc;
        m_state.ancLevels = m_confirmedLevels;
        m_state.error = EngineError::AncWriteFailed;
    }
}

void Engine::SetLink(LinkState link) {
    m_state.link = link;
    if (link == LinkState::Connected) {
        m_state.error = EngineError::None;
    }
    else {
        m_ancTimeout.Stop();
        m_eqTimeout.Stop();
        m_state.ancPending = false;
        m_state.eqPending = false;
    }
    if (link == LinkState::Disconnected || link == LinkState::NotFound) {
        // Stale percentages and mode would be misleading
        m_state.battery = {};
        m_state.anc.reset();
        m_confirmedAnc.reset();
        m_state.ancLevels = {};
        m_confirmedLevels = {};
        m_state.eq.reset();
        m_confirmedEq.reset();
    }
    NotifyChanged();
}

void Engine::NotifyChanged() {
    if (onStateChanged) onStateChanged();
}
