#pragma once
#include <string>
#include <algorithm>

// ─── Определение типа консоли по расширению файла ────────────────────────────
enum class ConsoleType { Unknown, NES, SNES, GB, N64 };

// Расширение файла (всё после последней точки) в нижнем регистре.
inline std::string romExtension(const std::string& path)
{
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return {};
    std::string ext = path.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return ext;
}

inline ConsoleType detectConsole(const std::string& path)
{
    const std::string ext = romExtension(path);
    if (ext == "nes")                      return ConsoleType::NES;
    if (ext == "sfc" || ext == "smc" ||
        ext == "fig" || ext == "swc")      return ConsoleType::SNES;
    if (ext == "gb"  || ext == "gbc" ||
        ext == "cgb" || ext == "sgb")      return ConsoleType::GB;
    if (ext == "z64" || ext == "n64" ||
        ext == "v64")                      return ConsoleType::N64;

    return ConsoleType::Unknown;
}

// Идентификатор консоли для библиотеки и оформления: NES, SNES, GB, GBC, N64.
inline std::string consoleIdFor(const std::string& path)
{
    switch (detectConsole(path)) {
    case ConsoleType::NES:  return "NES";
    case ConsoleType::SNES: return "SNES";
    case ConsoleType::N64:  return "N64";
    case ConsoleType::GB: {
        const std::string ext = romExtension(path);
        return (ext == "gbc" || ext == "cgb") ? "GBC" : "GB";
    }
    default:                return {};
    }
}
