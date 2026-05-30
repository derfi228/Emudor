#pragma once
#include <string>
#include <algorithm>

// ─── Определение типа консоли по расширению файла ────────────────────────────
enum class ConsoleType { Unknown, NES, SNES };

inline ConsoleType detectConsole(const std::string& path)
{
    // Берём расширение (всё после последней точки), переводим в нижний регистр
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return ConsoleType::Unknown;

    std::string ext = path.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });

    if (ext == "nes")                      return ConsoleType::NES;
    if (ext == "sfc" || ext == "smc" ||
        ext == "fig" || ext == "swc")      return ConsoleType::SNES;

    return ConsoleType::Unknown;
}
