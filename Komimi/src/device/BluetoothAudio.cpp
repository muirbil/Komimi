#include <windows.h>
#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <vector>
#include <winrt/base.h>
#include <initguid.h> // DEVPKEY_* are only declared without it
#include <devpkey.h>
#include "device/BluetoothAudio.h"
#include "app/Log.h"

namespace {
    // Windows groups a device and everything it brings (audio endpoints, drivers of each profile)
    // into one container. The hands-free driver's ID does not contain the address, the container is the reliable link.
    std::optional<GUID> ContainerOf(wchar_t const* instanceId) {
        DEVINST node = 0;
        if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(instanceId), CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS) {
            return std::nullopt;
        }
        GUID container{};
        DEVPROPTYPE type = 0;
        ULONG size = sizeof(container);
        if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ContainerId, &type,
                reinterpret_cast<BYTE*>(&container), &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_GUID) {
            return std::nullopt;
        }
        return container;
    }

    // The device node is BTHENUM\DEV_<address>\...
    std::optional<GUID> DeviceContainer(uint64_t address) {
        ULONG length = 0;
        if (CM_Get_Device_ID_List_SizeW(&length, L"BTHENUM", CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS) return std::nullopt;
        std::vector<wchar_t> list(length);
        if (CM_Get_Device_ID_ListW(L"BTHENUM", list.data(), length, CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS) return std::nullopt;

        std::wstring prefix = std::format(L"BTHENUM\\DEV_{:012X}\\", address);
        for (wchar_t const* id = list.data(); *id; id += wcslen(id) + 1) {
            if (_wcsnicmp(id, prefix.c_str(), prefix.size()) == 0) return ContainerOf(id);
        }
        return std::nullopt;
    }

    struct AudioFilter {
        std::wstring instanceId; // the profile's driver: A2DP (BTHENUM\{0000110B-...}) or hands-free (BTHHFENUM\...)
        std::wstring path;       // its KSCATEGORY_AUDIO interface
    };

    // Audio drivers of the device's profiles. Found through their interfaces rather than the audio endpoints:
    // the hands-free endpoints are only active during a call, the driver is there all the time.
    std::vector<AudioFilter> AudioFilters(GUID const& container) {
        GUID category = KSCATEGORY_AUDIO;
        ULONG length = 0;
        if (CM_Get_Device_Interface_List_SizeW(&length, &category, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return {};
        std::vector<wchar_t> list(length);
        if (CM_Get_Device_Interface_ListW(&category, nullptr, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return {};

        std::vector<AudioFilter> filters;
        for (wchar_t const* path = list.data(); *path; path += wcslen(path) + 1) {
            wchar_t instanceId[MAX_DEVICE_ID_LEN]{};
            DEVPROPTYPE type = 0;
            ULONG size = sizeof(instanceId);
            if (CM_Get_Device_Interface_PropertyW(path, &DEVPKEY_Device_InstanceId, &type,
                    reinterpret_cast<BYTE*>(instanceId), &size, 0) != CR_SUCCESS) {
                continue;
            }
            if (ContainerOf(instanceId) == container) filters.push_back({ instanceId, path });
        }
        return filters;
    }

    int SendOneShot(uint64_t address, KSPROPERTY_BTAUDIO property) {
        auto container = DeviceContainer(address);
        if (!container) {
            Log(L"Bluetooth audio: device node not found");
            return 0;
        }

        auto enumerator = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
        std::vector<std::wstring> done; // a driver may have several interfaces, one request per driver is enough
        for (auto const& filter : AudioFilters(*container)) {
            if (std::find(done.begin(), done.end(), filter.instanceId) != done.end()) continue;

            // The audio stack names a KS filter "{2}." + its interface path
            winrt::com_ptr<IMMDevice> device;
            winrt::com_ptr<IKsControl> control;
            if (FAILED(enumerator->GetDevice((L"{2}." + filter.path).c_str(), device.put())) ||
                FAILED(device->Activate(__uuidof(IKsControl), CLSCTX_ALL, nullptr, control.put_void()))) {
                Log(L"Bluetooth audio: cannot open {}", filter.path);
                continue;
            }
            KSPROPERTY request{};
            request.Set = KSPROPSETID_BtAudio;
            request.Id = property;
            request.Flags = KSPROPERTY_TYPE_GET;
            ULONG returned = 0;
            HRESULT hr = control->KsProperty(&request, sizeof(request), nullptr, 0, &returned);
            Log(L"Bluetooth audio {}: {} -> 0x{:08X}",
                property == KSPROPERTY_ONESHOT_RECONNECT ? L"connect" : L"disconnect", filter.instanceId, static_cast<uint32_t>(hr));
            if (SUCCEEDED(hr)) done.push_back(filter.instanceId);
        }
        return static_cast<int>(done.size());
    }
}

namespace BluetoothAudio {
    int Connect(uint64_t address) {
        return SendOneShot(address, KSPROPERTY_ONESHOT_RECONNECT);
    }

    int Disconnect(uint64_t address) {
        return SendOneShot(address, KSPROPERTY_ONESHOT_DISCONNECT);
    }
}
