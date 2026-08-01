#pragma once
#include <imgui.h>
#include <cstdint>

// ─── Дизайн-токены Emudor UI ──────────────────────────────────────────────────
// Точные значения из макета design_handoff_emudor_ui (Emudor Library.html).
// Light: тёплый кремовый + терракота. Dark: чернильно-фиолетовый + лавандовый неон.

struct UiTheme {
    ImU32 bg, bg2, surface, surface2, line, lineSoft;
    ImU32 ink, ink2, ink3;
    ImU32 accent, accentDeep, accentSoft, accentGlow;
    ImU32 logoColor, logoExtrude1, logoExtrude2, logoExtrude3, logoExtrude4;

    // Полоса геймпада на карточке: фон + цвет линий, по консоли.
    struct PadTint { ImU32 bg, fg; };
    PadTint padNes, padSnes, padGb, padGba, padN64, padPs1;

    float radius   = 14.0f;
    float radiusSm = 8.0f;
};

// Шрифты дизайна (порт токенов --font-*). Каждый указатель может быть nullptr,
// если файл шрифта не нашёлся — тогда используется ImGui::GetIO().FontDefault.
struct UiFonts {
    ImFont* logo         = nullptr; // Anton — вордмарк EMUDOR (крупный, в шапке)
    ImFont* logoSmall    = nullptr; // Anton меньшего размера (если нужно)
    ImFont* display      = nullptr; // Instrument Serif — счётчик игр, заголовки
    ImFont* displayItalic= nullptr; // Instrument Serif Italic — "Settings", подписи
    ImFont* label        = nullptr; // Oswald — имя консоли (NES/SNES/...)
    ImFont* labelSm      = nullptr; // Oswald маленький — SELECT/START на пэдах
    ImFont* ui           = nullptr; // Inter Regular — обычный текст
    ImFont* uiMedium     = nullptr; // Inter Medium
    ImFont* uiSemiBold   = nullptr; // Inter SemiBold — кнопки, акценты
    ImFont* uiBold       = nullptr; // Inter Bold
    ImFont* mono         = nullptr; // JetBrains Mono Medium — section-label, kbd
    ImFont* monoBold     = nullptr; // JetBrains Mono Bold
};

// Загружает все нужные .ttf из assets/fonts (пропускает отсутствующие без ошибки).
// pixelDensity позволяет один раз оверскейлить под HiDPI (обычно 1.0f).
UiFonts LoadUiFonts(ImGuiIO& io);

UiTheme MakeLightTheme();
UiTheme MakeDarkTheme();

// Утилита: "#rrggbb" → ImU32 (альфа = 255), либо с альфой явным параметром.
ImU32 HexColor(uint32_t rgb, uint8_t alpha = 255);
