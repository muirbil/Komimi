#include <windows.h>
#include <iterator>
#include "app/Localization.h"

namespace {
    constexpr size_t kStringCount = static_cast<size_t>(Str::Count);

    constexpr std::wstring_view kEnglish[] = {
        L"Connected",
        L"Connecting…",
        L"Not found · retry",
        L"Disconnected · retry",

        L"Connected",
        L"Connecting…",
        L"Disconnected",
        L"Earbuds not found",

        L"Left",
        L"Right",
        L"Case",

        L"Noise control",
        L"Off",
        L"ANC",
        L"Transparency",
        L"Couldn't switch mode",

        L"Noise cancelling level",
        L"Transparency mode",
        L"Level",
        L"Not available when noise control is off",
        L"Balanced",
        L"Light",
        L"Deep",
        L"Adaptive",
        L"Anti-wind",
        L"Deep+",
        L"Normal",
        L"Voice",
        L"Ambient",
        L"Voice+",

        L"Equalizer",
        L"Couldn't change equalizer",
        L"Balanced",
        L"Vocal",
        L"Bass",
        L"Treble",
        L"Custom",

        L"Reconnect",
        L"Start with Windows",
        L"Language",
        L"System default",
        L"Exit",
    };

    constexpr std::wstring_view kRussian[] = {
        L"Подключено",
        L"Подключение…",
        L"Не найдены · повторить",
        L"Отключено · повторить",

        L"Подключено",
        L"Подключение…",
        L"Отключено",
        L"Наушники не найдены",

        L"Левый",
        L"Правый",
        L"Кейс",

        L"Шумоподавление",
        L"Выкл.",
        L"Вкл.",
        L"Прозрачность",
        L"Не удалось переключить режим",

        L"Сила шумоподавления",
        L"Режим прозрачности",
        L"Уровень",
        L"Недоступно, когда режим выключен",
        L"Баланс",
        L"Лёгкий",
        L"Глубокий",
        L"Адаптивный",
        L"Ветрозащита",
        L"Глубокий+",
        L"Обычный",
        L"Голос",
        L"Окружение",
        L"Голос+",

        L"Эквалайзер",
        L"Не удалось сменить эквалайзер",
        L"Баланс",
        L"Вокал",
        L"Бас",
        L"Высокие",
        L"Свой",

        L"Переподключить",
        L"Запускать вместе с Windows",
        L"Язык",
        L"Как в системе",
        L"Выход",
    };

    constexpr std::wstring_view kJapanese[] = {
        L"接続済み",
        L"接続中…",
        L"見つかりません · 再試行",
        L"切断されました · 再試行",

        L"接続済み",
        L"接続中…",
        L"切断されました",
        L"イヤホンが見つかりません",

        L"左",
        L"右",
        L"ケース",

        L"ノイズコントロール",
        L"オフ",
        L"ANC",
        L"外音取り込み",
        L"切り替えに失敗しました",

        L"ノイズキャンセリングの強さ",
        L"外音取り込みモード",
        L"レベル",
        L"ノイズコントロールがオフのときは使用できません",
        L"バランス",
        L"ライト",
        L"ディープ",
        L"アダプティブ",
        L"風切り音低減",
        L"ディープ+",
        L"標準",
        L"音声強調",
        L"環境音強調",
        L"音声強調+",

        L"イコライザー",
        L"変更に失敗しました",
        L"バランス",
        L"ボーカル",
        L"低音",
        L"高音",
        L"カスタム",

        L"再接続",
        L"Windows の起動時に開始",
        L"言語",
        L"システムの既定",
        L"終了",
    };

    static_assert(std::size(kEnglish) == kStringCount, "kEnglish: number of strings doesn't match Str");
    static_assert(std::size(kRussian) == kStringCount, "kRussian: number of strings doesn't match Str");
    static_assert(std::size(kJapanese) == kStringCount, "kJapanese: number of strings doesn't match Str");

    constexpr wchar_t kRegistryKey[] = L"Software\\Komimi";
    constexpr wchar_t kRegistryValue[] = L"Language";

    Language g_language = Language::System;
    Language g_effective = Language::English;

    Language ResolveSystemLanguage() {
        switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_RUSSIAN:  return Language::Russian;
        case LANG_JAPANESE: return Language::Japanese;
        default:            return Language::English;
        }
    }

    void Apply(Language language) {
        g_language = language;
        g_effective = language == Language::System ? ResolveSystemLanguage() : language;
    }

    std::wstring_view const* Table() {
        switch (g_effective) {
        case Language::Russian:  return kRussian;
        case Language::Japanese: return kJapanese;
        default:                 return kEnglish;
        }
    }
}

namespace Localization {
    void Init() {
        DWORD value = 0;
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, kRegistryValue, RRF_RT_REG_DWORD,
                nullptr, &value, &size) != ERROR_SUCCESS || value > static_cast<DWORD>(Language::Japanese)) {
            value = static_cast<DWORD>(Language::System);
        }
        Apply(static_cast<Language>(value));
    }

    Language GetLanguage() {
        return g_language;
    }

    void SetLanguage(Language language) {
        Apply(language);
        DWORD value = static_cast<DWORD>(language);
        RegSetKeyValueW(HKEY_CURRENT_USER, kRegistryKey, kRegistryValue, REG_DWORD, &value, sizeof(value));
    }

    wchar_t const* LocaleName() {
        switch (g_effective) {
        case Language::Russian:  return L"ru-ru";
        case Language::Japanese: return L"ja-jp";
        default:                 return L"en-us";
        }
    }
}

std::wstring_view Tr(Str id) {
    return Table()[static_cast<size_t>(id)];
}
