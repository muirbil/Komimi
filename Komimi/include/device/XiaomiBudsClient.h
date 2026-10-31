#pragma once
// Redmi Buds 6 Lite client over RFCOMM (C++/WinRT).
//
//   auto buds = std::make_shared<XiaomiBudsClient>();
//   buds->OnBattery = [](XiaomiMma::BatteryInfo const& b) { ... };
//   buds->OnAnc     = [](XiaomiMma::AncMode m) { ... };
//   uint64_t address = co_await XiaomiBudsClient::FindPairedAsync();   // 0 = not found
//   bool ok = co_await buds->ConnectAsync(address);
//   buds->SetAnc(XiaomiMma::AncMode::Transparency);                     // like the gesture: uses the stored level
//   buds->SetAnc(XiaomiMma::AncMode::NoiseCancelling,
//                uint8_t(XiaomiMma::NoiseCancellingLevel::DeepPlus));   // store the level and switch on
//   buds->SetEq(XiaomiMma::EqPreset::Bass);                             // applied immediately
//
// Callbacks run on a background thread. To update the UI, switch to its thread
// (DispatcherQueue::TryEnqueue / co_await winrt::resume_foreground(...)).
// Set the callbacks once before the first ConnectAsync and do not change them afterwards.
//
// Create only through std::make_shared: the background read/write loops hold
// a reference to the client so they never touch a destroyed object.
//
// No authentication needed: on the FD2D RFCOMM service the earbuds answer commands right away.

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "device/Constants.h"
#include "device/MmaProtocol.h"

class XiaomiBudsClient : public std::enable_shared_from_this<XiaomiBudsClient> {
public:
    std::function<void(XiaomiMma::BatteryInfo const&)> OnBattery;
    std::function<void(XiaomiMma::AncMode)> OnAnc;                         // actual mode
    std::function<void(XiaomiMma::AncMode mode, uint8_t level)> OnAncLevel; // stored level of the given mode
    std::function<void(bool ok)> OnAncWritten;                              // earbuds' response to SetAnc
    std::function<void(XiaomiMma::EqPreset)> OnEq;                          // current EQ preset
    std::function<void(bool ok)> OnEqWritten;                               // earbuds' response to SetEq
    std::function<void()> OnDisconnected;
    // Debugging: sent frames (outgoing = true) and received bytes exactly as they came from the socket
    std::function<void(bool outgoing, std::vector<uint8_t> const& bytes)> OnRawBytes;

    ~XiaomiBudsClient() { Close(); }

    // Looks for the earbuds among devices paired with Windows; returns the Bluetooth address or 0.
    // A device matches if the model is in its name or Xiaomi's RFCOMM service (FD2D) is in the Windows cache;
    // the cache needs no connection, so the search is fast and does not wake the earbuds.
    // Among several matches, a name match wins first, then a device connected right now.
    static winrt::Windows::Foundation::IAsyncOperation<uint64_t> FindPairedAsync() {
        using namespace winrt::Windows::Devices::Bluetooth;
        using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
        using namespace winrt::Windows::Devices::Enumeration;
        namespace C = BudsConstants::Xiaomi::Buds6Lite;

        auto lower = [](std::wstring_view s) {
            std::wstring out(s);
            std::transform(out.begin(), out.end(), out.begin(), ::towlower);
            return out;
        };
        const std::wstring model = lower(C::DisplayName);

        uint64_t best = 0;
        int bestScore = -1;
        auto paired = co_await DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true));
        for (auto const& info : paired) {
            try {
                auto device = co_await BluetoothDevice::FromIdAsync(info.Id());
                if (!device) continue;

                bool nameMatches = lower(device.Name()).find(model) != std::wstring::npos;
                auto services = co_await device.GetRfcommServicesForIdAsync(
                    RfcommServiceId::FromUuid(C::SppMiuiUuid), BluetoothCacheMode::Cached);
                bool hasXiaomiService = services.Services().Size() > 0;

                if (nameMatches || hasXiaomiService) {
                    int score = (nameMatches ? 2 : 0) + (device.ConnectionStatus() == BluetoothConnectionStatus::Connected ? 1 : 0);
                    if (score > bestScore) {
                        bestScore = score;
                        best = device.BluetoothAddress();
                    }
                }
                device.Close();
            }
            catch (winrt::hresult_error const&) {
                // The device does not respond or access is denied: just skip it
            }
        }
        co_return best;
    }

    // false = the earbuds are not paired, off or in the case
    winrt::Windows::Foundation::IAsyncOperation<bool> ConnectAsync(uint64_t address) {
        using namespace winrt::Windows::Devices::Bluetooth;
        using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
        using namespace winrt::Windows::Networking::Sockets;
        using namespace winrt::Windows::Storage::Streams;
        namespace C = BudsConstants::Xiaomi::Buds6Lite;

        auto self = shared_from_this();
        Close();

        try {
            auto device = co_await BluetoothDevice::FromBluetoothAddressAsync(address);
            if (!device) co_return false;

            RfcommDeviceService service{ nullptr };
            const std::array<winrt::guid, 2> ids{ C::SppMiuiUuid, C::SppUuid };
            for (auto const& uuid : ids) {
                auto res = co_await device.GetRfcommServicesForIdAsync(
                    RfcommServiceId::FromUuid(uuid), BluetoothCacheMode::Uncached);
                if (res.Services().Size() > 0) { service = res.Services().GetAt(0); break; }
            }
            if (!service) co_return false;

            StreamSocket socket;
            // Throws if the earbuds are unavailable (for example, in the case)
            co_await socket.ConnectAsync(service.ConnectionHostName(), service.ConnectionServiceName());

            {
                std::lock_guard lock(m_mutex);
                m_socket = socket;
                m_writer = DataWriter(socket.OutputStream());
                m_queue.clear();
            }
            m_connected = true;
            ReadLoop(socket, ++m_generation);
        }
        catch (winrt::hresult_error const&) {
            Close();
            co_return false;
        }

        // The earbuds send neither battery nor mode on connect, so we ask.
        Send(XiaomiMma::GetTargetInfo(NextSn()));
        Send(XiaomiMma::GetDeviceRunInfo(NextSn()));                                      // actual ANC mode
        Send(XiaomiMma::GetDeviceConfig(NextSn(),
            { XiaomiMma::ConfigId::NoiseLevel, XiaomiMma::ConfigId::EqPreset }));  // ANC level and EQ preset
        co_return true;
    }

    // Switches the mode. Without level the earbuds use the stored level (like the gesture).
    // With level: store it first (F2), then switch the mode (0x08). This is the only way to apply a level.
    void SetAnc(XiaomiMma::AncMode mode, std::optional<uint8_t> level = std::nullopt) {
        if (level && mode != XiaomiMma::AncMode::Off) {
            uint8_t sn = NextSn();
            m_ancStoreSn = sn;
            Send(XiaomiMma::StoreAncLevel(sn, mode, *level));
        }
        Send(XiaomiMma::SwitchAnc(NextSn(), mode));
    }

    // The EQ preset is applied immediately and saved in the earbuds.
    void SetEq(XiaomiMma::EqPreset preset) {
        uint8_t sn = NextSn();
        m_eqSn = sn;
        Send(XiaomiMma::SetEq(sn, preset));
        Send(XiaomiMma::GetDeviceConfig(NextSn(), { XiaomiMma::ConfigId::EqPreset })); // read it back → OnEq
    }
    void RequestBattery() { Send(XiaomiMma::GetTargetInfo(NextSn())); }
    void RequestAnc() { Send(XiaomiMma::GetDeviceRunInfo(NextSn())); }
    void RequestAncLevel() { Send(XiaomiMma::GetDeviceConfig(NextSn(), { XiaomiMma::ConfigId::NoiseLevel })); }

    void Close() {
        m_connected = false;
        ++m_generation; // the old ReadLoop must not report the new connection as disconnected
        winrt::Windows::Networking::Sockets::StreamSocket socket{ nullptr };
        {
            std::lock_guard lock(m_mutex);
            socket = std::exchange(m_socket, nullptr);
            m_writer = nullptr;
            m_queue.clear();
        }
        if (socket) socket.Close();
    }

private:
    winrt::fire_and_forget ReadLoop(winrt::Windows::Networking::Sockets::StreamSocket socket, uint32_t generation) {
        using namespace winrt::Windows::Storage::Streams;
        auto self = shared_from_this();
        XiaomiMma::Framer framer; // one per connection, so the tail of an old stream is never glued to a new one
        try {
            DataReader reader(socket.InputStream());
            reader.InputStreamOptions(InputStreamOptions::Partial);
            for (;;) {
                uint32_t n = co_await reader.LoadAsync(1024);
                if (n == 0) break;
                std::vector<uint8_t> buf(n);
                reader.ReadBytes(buf);
                if (OnRawBytes) OnRawBytes(false, buf);
                for (auto& p : framer.Feed(buf)) Handle(p);
            }
        }
        catch (...) {}
        if (generation == m_generation && m_connected.exchange(false) && OnDisconnected) OnDisconnected();
    }

    void Handle(XiaomiMma::Packet const& p) {
        // Commands from the earbuds (0x0E, 0xF4) need an acknowledgement, otherwise they keep repeating them.
        if (p.isCommand && p.needsResponse) Send(XiaomiMma::BuildResponse(p.opcode, p.sn, 0));

        if (!p.isCommand) {
            // The result of SetAnc is the response to 0x08 (the actual switch)
            if (p.opcode == XiaomiMma::OpCode::SetTargetInfo && OnAncWritten) OnAncWritten(p.status == 0);

            // F2 is shared by the ANC level and the EQ: the request sn tells whose response this is.
            // Storing the ANC level only matters if it fails: 0x08 would then use the old level.
            if (p.opcode == XiaomiMma::OpCode::SetDeviceConfig) {
                if (p.sn == m_eqSn && OnEqWritten) OnEqWritten(p.status == 0);
                else if (p.sn == m_ancStoreSn && p.status != 0 && OnAncWritten) OnAncWritten(false);
            }
        }

        auto u = XiaomiMma::Interpret(p);
        if (u.battery && OnBattery) OnBattery(*u.battery);
        if (u.ancLevel && OnAncLevel) OnAncLevel(u.ancLevelMode, *u.ancLevel);
        if (u.anc && OnAnc) OnAnc(*u.anc);
        if (u.eq && OnEq) OnEq(*u.eq);
    }

    // Writes are queued: a socket cannot have two StoreAsync calls in flight.
    void Send(std::vector<uint8_t> frame) {
        {
            std::lock_guard lock(m_mutex);
            if (!m_connected) return;
            m_queue.push_back(std::move(frame));
            if (m_writing) return;
            m_writing = true;
        }
        Pump();
    }

    winrt::fire_and_forget Pump() {
        auto self = shared_from_this();
        for (;;) {
            std::vector<uint8_t> frame;
            winrt::Windows::Storage::Streams::DataWriter writer{ nullptr };
            {
                std::lock_guard lock(m_mutex);
                if (m_queue.empty() || !m_connected || !m_writer) { m_queue.clear(); m_writing = false; co_return; }
                frame = std::move(m_queue.front());
                m_queue.pop_front();
                writer = m_writer;
            }
            if (OnRawBytes) OnRawBytes(true, frame);
            try {
                writer.WriteBytes(frame);
                co_await writer.StoreAsync();
            }
            catch (...) {
                std::lock_guard lock(m_mutex);
                m_queue.clear(); m_writing = false; co_return;
            }
        }
    }

    uint8_t NextSn() { return uint8_t(++m_sn); }

    winrt::Windows::Networking::Sockets::StreamSocket m_socket{ nullptr };
    winrt::Windows::Storage::Streams::DataWriter m_writer{ nullptr };
    std::atomic<bool> m_connected{ false };
    std::atomic<uint32_t> m_generation{ 0 };
    std::atomic<uint8_t> m_sn{ 0 };
    std::atomic<int> m_ancStoreSn{ -1 }; // sn of the latest F2 commands, used to tell responses apart
    std::atomic<int> m_eqSn{ -1 };
    std::mutex m_mutex; // m_socket, m_writer, m_queue, m_writing
    std::deque<std::vector<uint8_t>> m_queue;
    bool m_writing = false;
};
