#include "ui_theme.h"
#include <filesystem>

namespace fs = std::filesystem;

ImU32 HexColor(uint32_t rgb, uint8_t alpha) {
    uint8_t r = (uint8_t)((rgb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((rgb >> 8)  & 0xFF);
    uint8_t b = (uint8_t)( rgb        & 0xFF);
    return IM_COL32(r, g, b, alpha);
}

UiTheme MakeLightTheme() {
    UiTheme t;
    t.bg          = HexColor(0xefe6d6);
    t.bg2         = HexColor(0xf7f0e2);
    t.surface     = HexColor(0xfbf6ec);
    t.surface2    = HexColor(0xf1e7d3);
    t.line        = HexColor(0xd9cab0);
    t.lineSoft    = HexColor(0xe6d9c0);
    t.ink         = HexColor(0x2a1f17);
    t.ink2        = HexColor(0x5b4a3a);
    t.ink3        = HexColor(0x8a7860);
    t.accent      = HexColor(0xb75432);
    t.accentDeep  = HexColor(0x8a3a1f);
    t.accentSoft  = HexColor(0xe3a888);
    t.accentGlow  = HexColor(0xb75432, 64);
    t.logoColor    = HexColor(0xb75432);
    t.logoExtrude1 = HexColor(0x9a4326);
    t.logoExtrude2 = HexColor(0x7d3520);
    t.logoExtrude3 = HexColor(0x5e271a);
    t.logoExtrude4 = HexColor(0x3b1810);

    t.padNes  = { HexColor(0xece1c5), HexColor(0x2a1f0e) };
    t.padSnes = { HexColor(0xe1dbef), HexColor(0x2c2042) };
    t.padGb   = { HexColor(0xd8d6c0), HexColor(0x2c2812) };
    t.padGba  = { HexColor(0xd9d0ee), HexColor(0x261a4a) };
    t.padN64  = { HexColor(0xdfd9c5), HexColor(0x2a2418) };
    t.padPs1  = { HexColor(0xdbd6c7), HexColor(0x1f1c14) };
    return t;
}

UiTheme MakeDarkTheme() {
    UiTheme t;
    t.bg          = HexColor(0x07060c);
    t.bg2         = HexColor(0x100b1a);
    t.surface     = HexColor(0x181226);
    t.surface2    = HexColor(0x0e0a18);
    t.line        = HexColor(0x2e2444);
    t.lineSoft    = HexColor(0x1f1830);
    t.ink         = HexColor(0xece4ff);
    t.ink2        = HexColor(0xb9aae0);
    t.ink3        = HexColor(0x7a6c9a);
    t.accent      = HexColor(0xb08aff);
    t.accentDeep  = HexColor(0x7e5be0);
    t.accentSoft  = HexColor(0x2a1e54);
    t.accentGlow  = HexColor(0xb08aff, 97);
    t.logoColor    = HexColor(0xc6a6ff);
    t.logoExtrude1 = HexColor(0xa684ec);
    t.logoExtrude2 = HexColor(0x7e5be0);
    t.logoExtrude3 = HexColor(0x4e359a);
    t.logoExtrude4 = HexColor(0x221643);

    t.padNes  = { HexColor(0x1c1426), HexColor(0xd8c5e8) };
    t.padSnes = { HexColor(0x1a1330), HexColor(0xdccae8) };
    t.padGb   = { HexColor(0x181128), HexColor(0xc0bce0) };
    t.padGba  = { HexColor(0x1a1130), HexColor(0xd7c5ea) };
    t.padN64  = { HexColor(0x18122a), HexColor(0xd6cce4) };
    t.padPs1  = { HexColor(0x161028), HexColor(0xd6cce4) };
    return t;
}

// Пытается загрузить шрифт по нескольким кандидатам-путям (относительно cwd
// и относительно каталога исполняемого файла — на случай запуска не из корня).
static ImFont* tryLoadFont(ImGuiIO& io, const char* relPath, float sizePx) {
    static std::string baseDir;
    if (baseDir.empty()) {
        baseDir = ".";
    }
    fs::path candidates[] = {
        fs::path(relPath),
        fs::path("assets/fonts") / fs::path(relPath).filename(),
    };
    for (const auto& p : candidates) {
        if (fs::exists(p)) {
            ImFontConfig fc;
            fc.OversampleH = 3;
            fc.OversampleV = 2;
            fc.PixelSnapH  = false;
            return io.Fonts->AddFontFromFileTTF(p.string().c_str(), sizePx, &fc);
        }
    }
    return nullptr;
}

UiFonts LoadUiFonts(ImGuiIO& io) {
    UiFonts f;
    f.logo          = tryLoadFont(io, "assets/fonts/Anton-Regular.ttf", 64.0f);
    f.logoSmall      = tryLoadFont(io, "assets/fonts/Anton-Regular.ttf", 34.0f);
    f.display       = tryLoadFont(io, "assets/fonts/InstrumentSerif-Regular.ttf", 24.0f);
    f.displayItalic  = tryLoadFont(io, "assets/fonts/InstrumentSerif-Italic.ttf", 26.0f);
    f.label         = tryLoadFont(io, "assets/fonts/Oswald-Regular.ttf", 20.0f);
    f.labelSm       = tryLoadFont(io, "assets/fonts/Oswald-Medium.ttf", 13.0f);
    f.ui            = tryLoadFont(io, "assets/fonts/Inter-Regular.ttf", 16.0f);
    f.uiMedium      = tryLoadFont(io, "assets/fonts/Inter-Medium.ttf", 16.0f);
    f.uiSemiBold    = tryLoadFont(io, "assets/fonts/Inter-SemiBold.ttf", 16.0f);
    f.uiBold        = tryLoadFont(io, "assets/fonts/Inter-Bold.ttf", 16.0f);
    f.mono          = tryLoadFont(io, "assets/fonts/JetBrainsMono-Medium.ttf", 12.0f);
    f.monoBold      = tryLoadFont(io, "assets/fonts/JetBrainsMono-Bold.ttf", 12.0f);
    return f;
}
