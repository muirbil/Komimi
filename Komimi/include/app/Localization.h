#pragma once
#include <string_view>

enum class Language {
    System,  // follows the Windows display language
    English,
    Russian,
    Japanese,
};

// IDs of all UI strings. A new string is added here
// and to every table in Localization.cpp (a static_assert there checks the count).
enum class Str {
    StatusConnected,
    StatusConnecting,
    StatusNotFoundRetry,
    StatusDisconnectedRetry,

    TipConnected,
    TipConnecting,
    TipDisconnected,
    TipNotFound,

    BatteryLeft,
    BatteryRight,
    BatteryCase,

    NoiseControl,
    AncOff,
    AncOn,
    AncTransparency,
    ErrorAncWrite,

    LevelHeaderNoiseCancelling,
    LevelHeaderTransparency,
    LevelHeaderGeneric,
    LevelUnavailableWhenOff,
    // Same order as XiaomiMma::NoiseCancellingLevel / TransparencyLevel
    AncLevelBalanced,
    AncLevelLight,
    AncLevelDeep,
    AncLevelAdaptive,
    AncLevelAntiWind,
    AncLevelDeepPlus,
    TransparencyLevelNormal,
    TransparencyLevelVoice,
    TransparencyLevelAmbient,
    TransparencyLevelVoicePlus,

    Equalizer,
    ErrorEqWrite,
    // Same order as XiaomiMma::Buds6LitePresets
    EqBalanced,
    EqVocal,
    EqBass,
    EqTreble,
    EqCustom,

    MenuReconnect,
    MenuStartWithWindows,
    MenuLanguage,
    MenuLanguageSystem,
    MenuExit,

    Count
};

namespace Localization {
    // Reads the saved choice from the registry; call once at startup
    void Init();

    Language GetLanguage();               // what the user picked (may be System)
    void SetLanguage(Language language);  // applies it and saves it to the registry

    // Locale of the effective language ("ja-jp" etc.): DirectWrite uses it to pick the font for
    // characters missing from the main one (Japanese text gets Yu Gothic UI instead of Segoe UI)
    wchar_t const* LocaleName();
}

std::wstring_view Tr(Str id);
