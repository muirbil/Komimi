#pragma once
// Xiaomi MMA protocol (on top of JieLi RCSP), as used by the Xiaomi Earbuds app.
// Plain C++17, no WinRT: frame building/parsing and payload decoding.
// Verified on Redmi Buds 6 Lite over the FD2D RFCOMM service.
//
// Frame: FE DC BA | flags | opcode | len(2, BE) | body(len) | EF
//   flags: 0x80 = command (otherwise a response), 0x40 = response expected, bits 0-2 = targetApp
//   command body:  sn, payload...
//   response body: status, sn, payload...

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace XiaomiMma {

    enum class OpCode : uint8_t {
        GetTargetInfo      = 0x02, // PC → earbuds, payload: attribute mask (4 bytes BE)
        SetTargetInfo      = 0x08, // PC → earbuds: [len][type][value]; type 4 = ACTUALLY switch ANC
        GetDeviceRunInfo   = 0x09, // PC → earbuds, payload: mask (4 bytes BE); attribute 9 = current ANC
        ReportDeviceStatus = 0x0E, // earbuds → PC, unsolicited; expect a response with status=0
        SetDeviceConfig    = 0xF2, // PC → earbuds, payload: list of configs
        GetDeviceConfig    = 0xF3, // PC → earbuds, payload: config IDs, 2 bytes BE each
        NotifyDeviceConfig = 0xF4, // earbuds → PC, unsolicited; expect a response with status=0
    };

    // Config IDs (GetDeviceConfig / SetDeviceConfig / NotifyDeviceConfig)
    enum class ConfigId : uint16_t {
        EqPreset        = 0x0007, // EQ preset, 1 byte; F2 applies it immediately
        NoiseModeChoose = 0x000A, // which modes the gesture cycles through (left, right)
        NoiseLevel      = 0x000B, // STORED [mode, level]. Writing it with F2 does NOT switch the mode,
                                  // but the level is applied by the next switch with 0x08
        WearStatus      = 0x000C, // changes when an earbud is taken out; bits not decoded
    };

    enum class AncMode : uint8_t { Off = 0, NoiseCancelling = 1, Transparency = 2 };

    // EQ presets. The Xiaomi app offers exactly five for Buds 6 Lite (verified):
    //   0 Balanced sound, 1 Enhanced vocal, 5 Enhanced bass, 6 Enhanced treble, 10 Custom.
    // The earbuds accept the other values from Xiaomi's shared list, but they are not meant for this model.
    enum class EqPreset : uint8_t {
        Balanced = 0, Vocal = 1, Bass = 5, Treble = 6, Custom = 10,
        // for other Xiaomi models:
        Rock = 2, Classic = 3, Pop = 4, VolumeBoost = 7, SoothingBass = 13, Harman = 14, Standard = 16
    };
    inline constexpr std::array<EqPreset, 5> Buds6LitePresets{
        EqPreset::Balanced, EqPreset::Vocal, EqPreset::Bass, EqPreset::Treble, EqPreset::Custom };

    // Levels (second byte of config 0x000B); their meaning depends on the mode.
    // Verified on Buds 6 Lite: the earbuds accept 0-5, the difference between 1 and 5 is audible.
    enum class NoiseCancellingLevel : uint8_t {
        Balanced = 0, Light = 1, Deep = 2, Adaptive = 3, AntiWind = 4, DeepPlus = 5
    };
    enum class TransparencyLevel : uint8_t {
        Transparent = 0, VoiceEnhance = 1, AmbientEnhance = 2, VoiceEnhancePlus = 3
    };

    // Attributes [len][type][value × (len-1)]
    inline constexpr uint8_t AttrTargetInfoBattery   = 7; // GetTargetInfo response
    inline constexpr uint8_t AttrDeviceStatusBattery = 0; // ReportDeviceStatus
    inline constexpr uint8_t AttrRunInfoAnc          = 9; // GetDeviceRunInfo response: actual ANC mode
    inline constexpr uint8_t TargetInfoSetAnc        = 4; // SetTargetInfo: switch ANC

    inline constexpr uint8_t FlagCommand      = 0x80;
    inline constexpr uint8_t FlagNeedResponse = 0x40;

    struct Packet {
        uint8_t flags = 0;
        OpCode opcode{};
        bool isCommand = false;      // came from the earbuds as a command (otherwise a response to our request)
        bool needsResponse = false;
        uint8_t status = 0;          // responses only
        uint8_t sn = 0;
        std::vector<uint8_t> payload;
    };

    // ---------- building ----------

    inline std::vector<uint8_t> BuildFrame(uint8_t flags, OpCode op, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> f{ 0xFE, 0xDC, 0xBA, flags, static_cast<uint8_t>(op),
                                static_cast<uint8_t>(body.size() >> 8), static_cast<uint8_t>(body.size()) };
        f.insert(f.end(), body.begin(), body.end());
        f.push_back(0xEF);
        return f;
    }

    inline std::vector<uint8_t> BuildCommand(OpCode op, uint8_t sn, const std::vector<uint8_t>& payload = {}) {
        std::vector<uint8_t> body{ sn };
        body.insert(body.end(), payload.begin(), payload.end());
        return BuildFrame(FlagCommand | FlagNeedResponse, op, body);
    }

    inline std::vector<uint8_t> BuildResponse(OpCode op, uint8_t sn, uint8_t status = 0) {
        return BuildFrame(0x00, op, { status, sn });
    }

    // Ready-made requests
    inline std::vector<uint8_t> GetTargetInfo(uint8_t sn, uint32_t mask = 0xFFFFFFFF) {
        return BuildCommand(OpCode::GetTargetInfo, sn,
            { uint8_t(mask >> 24), uint8_t(mask >> 16), uint8_t(mask >> 8), uint8_t(mask) });
    }

    inline std::vector<uint8_t> GetDeviceConfig(uint8_t sn, std::initializer_list<ConfigId> ids) {
        std::vector<uint8_t> p;
        for (auto id : ids) { p.push_back(uint8_t(uint16_t(id) >> 8)); p.push_back(uint8_t(id)); }
        return BuildCommand(OpCode::GetDeviceConfig, sn, p);
    }

    inline std::vector<uint8_t> GetDeviceRunInfo(uint8_t sn, uint32_t mask = 0xFFFFFFFF) {
        return BuildCommand(OpCode::GetDeviceRunInfo, sn,
            { uint8_t(mask >> 24), uint8_t(mask >> 16), uint8_t(mask >> 8), uint8_t(mask) });
    }

    // Actually switches the mode (like the gesture and the Xiaomi app). The earbuds use the stored level.
    inline std::vector<uint8_t> SwitchAnc(uint8_t sn, AncMode mode) {
        return BuildCommand(OpCode::SetTargetInfo, sn, { 2, TargetInfoSetAnc, uint8_t(mode) });
    }

    // Only stores [mode, level] and does not switch the mode. The level is applied by SwitchAnc.
    inline std::vector<uint8_t> StoreAncLevel(uint8_t sn, AncMode mode, uint8_t level) {
        return BuildCommand(OpCode::SetDeviceConfig, sn,
            { 4, 0x00, uint8_t(ConfigId::NoiseLevel), uint8_t(mode), level });
    }

    inline std::vector<uint8_t> SetEq(uint8_t sn, EqPreset preset) {
        return BuildCommand(OpCode::SetDeviceConfig, sn, { 3, 0x00, uint8_t(ConfigId::EqPreset), uint8_t(preset) });
    }

    // ---------- stream parsing ----------

    // Assembles frames from a byte stream (RFCOMM may split and glue packets at will).
    class Framer {
    public:
        std::vector<Packet> Feed(const uint8_t* data, size_t size) {
            m_buf.insert(m_buf.end(), data, data + size);
            std::vector<Packet> out;
            for (;;) {
                size_t i = FindHeader();
                if (i == npos) { // keep the tail: the header may have been split
                    if (m_buf.size() > 2) m_buf.erase(m_buf.begin(), m_buf.end() - 2);
                    return out;
                }
                m_buf.erase(m_buf.begin(), m_buf.begin() + i);
                if (m_buf.size() < 8) return out;
                size_t len = (size_t(m_buf[5]) << 8) | m_buf[6];
                if (m_buf.size() < 8 + len) return out;
                bool ok = m_buf[7 + len] == 0xEF && len >= 1;
                if (ok) {
                    if (auto p = Parse(m_buf.data(), 8 + len)) out.push_back(std::move(*p));
                    m_buf.erase(m_buf.begin(), m_buf.begin() + 8 + len);
                } else {
                    m_buf.erase(m_buf.begin()); // false header, keep looking
                }
            }
        }
        std::vector<Packet> Feed(const std::vector<uint8_t>& d) { return Feed(d.data(), d.size()); }

        static std::optional<Packet> Parse(const uint8_t* f, size_t n) {
            Packet p;
            p.flags = f[3];
            p.opcode = static_cast<OpCode>(f[4]);
            p.isCommand = (p.flags & FlagCommand) != 0;
            p.needsResponse = (p.flags & FlagNeedResponse) != 0;
            const uint8_t* body = f + 7;
            size_t len = n - 8;
            size_t off;
            if (p.isCommand) { p.sn = body[0]; off = 1; }
            else { if (len < 2) return std::nullopt; p.status = body[0]; p.sn = body[1]; off = 2; }
            p.payload.assign(body + off, body + len);
            return p;
        }

    private:
        static constexpr size_t npos = size_t(-1);
        size_t FindHeader() const {
            for (size_t i = 0; i + 2 < m_buf.size(); ++i)
                if (m_buf[i] == 0xFE && m_buf[i + 1] == 0xDC && m_buf[i + 2] == 0xBA) return i;
            return npos;
        }
        std::vector<uint8_t> m_buf;
    };

    // ---------- payload parsing ----------

    // [len][type][value × (len-1)] — GetTargetInfo, GetDeviceRunInfo, ReportDeviceStatus
    template <class F> void ForEachAttr(const std::vector<uint8_t>& d, F&& f) {
        size_t i = 0;
        while (i + 2 <= d.size()) {
            size_t len = d[i];
            if (len == 0 || i + 1 + len > d.size()) break;
            f(d[i + 1], d.data() + i + 2, len - 1);
            i += len + 1;
        }
    }

    // [len][id(2, BE)][value × (len-2)] — Get/Set/NotifyDeviceConfig
    template <class F> void ForEachConfig(const std::vector<uint8_t>& d, F&& f) {
        size_t i = 0;
        while (i + 3 <= d.size()) {
            size_t len = d[i];
            if (len == 0xFF || len < 2 || i + 1 + len > d.size()) break; // 0xFF is a special format for large data, not needed
            uint16_t id = uint16_t((d[i + 1] << 8) | d[i + 2]);
            f(static_cast<ConfigId>(id), d.data() + i + 3, len - 2);
            i += len + 1;
        }
    }

    struct BatteryLevel {
        std::optional<uint8_t> percent; // no value = no data (0xFF)
        bool charging = false;
    };
    struct BatteryInfo { BatteryLevel left, right, caseBox; };

    inline BatteryLevel DecodeBattery(uint8_t b) {
        if (b == 0xFF) return {};
        return { uint8_t(b & 0x7F), (b & 0x80) != 0 };
    }

    // What useful data a packet carried
    struct Update {
        std::optional<BatteryInfo> battery;
        std::optional<AncMode> anc;        // actual mode
        std::optional<uint8_t> ancLevel;   // stored level...
        AncMode ancLevelMode{};            // ...and the mode it belongs to (from the same [mode, level] pair)
        std::optional<EqPreset> eq;
    };

    inline Update Interpret(const Packet& p) {
        Update u;
        auto battery = [&](const uint8_t* v, size_t n) {
            if (n >= 3) u.battery = BatteryInfo{ DecodeBattery(v[0]), DecodeBattery(v[1]), DecodeBattery(v[2]) };
        };
        if (!p.isCommand && p.status != 0) return u;
        switch (p.opcode) {
        case OpCode::GetTargetInfo:
            if (!p.isCommand)
                ForEachAttr(p.payload, [&](uint8_t t, const uint8_t* v, size_t n) { if (t == AttrTargetInfoBattery) battery(v, n); });
            break;
        case OpCode::GetDeviceRunInfo:
            if (!p.isCommand)
                ForEachAttr(p.payload, [&](uint8_t t, const uint8_t* v, size_t n) {
                    if (t == AttrRunInfoAnc && n >= 1) u.anc = static_cast<AncMode>(v[0]);
                });
            break;
        case OpCode::ReportDeviceStatus:
            ForEachAttr(p.payload, [&](uint8_t t, const uint8_t* v, size_t n) { if (t == AttrDeviceStatusBattery) battery(v, n); });
            break;
        case OpCode::GetDeviceConfig:     // response to a request: its mode may be stale, take only the level
        case OpCode::NotifyDeviceConfig:  // notification after an actual switch (gesture or 0x08): the mode is correct
            ForEachConfig(p.payload, [&](ConfigId id, const uint8_t* v, size_t n) {
                if (id == ConfigId::EqPreset && n >= 1) { u.eq = static_cast<EqPreset>(v[0]); return; }
                if (id != ConfigId::NoiseLevel || n < 1) return;
                if (p.opcode == OpCode::NotifyDeviceConfig) u.anc = static_cast<AncMode>(v[0]);
                if (n >= 2) {
                    u.ancLevel = v[1];
                    u.ancLevelMode = static_cast<AncMode>(v[0]);
                }
            });
            break;
        default:
            break;
        }
        return u;
    }
}
