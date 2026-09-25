// n64_console.cpp — Nintendo 64 как IConsole.
#include "n64_console.h"
#include <fstream>
#include <iterator>

bool N64Console::loadROM(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return loadROMData(std::move(rom));
}

bool N64Console::loadROMData(std::vector<uint8_t> rom)
{
    return sys_.loadRom(std::move(rom));
}

void N64Console::reset()
{
    sys_.reset();
}

void N64Console::setInput(int player, uint16_t b)
{
    uint16_t n = 0;
    if (b & (1u << 7))  n |= 0x8000;               // A
    if (b & (1u << 15)) n |= 0x4000;               // B
    if (b & (1u << 5))  n |= 0x2000;               // L → Z
    if (b & (1u << 13)) n |= 0x2000;               // Select → Z
    if (b & (1u << 12)) n |= 0x1000;               // Start
    if (b & (1u << 14)) n |= 0x0020;               // Y → L
    if (b & (1u << 4))  n |= 0x0010;               // R
    if (b & (1u << 6))  n |= 0x0008;               // X → C-вверх
    n |= b & 0x000F;                               // C-кнопки
    int8_t x = 0, y = 0;
    if (b & (1u << 11)) y = 80;
    if (b & (1u << 10)) y = -80;
    if (b & (1u << 9))  x = -80;
    if (b & (1u << 8))  x = 80;
    sys_.setController(player, n, x, y);
}

bool N64Console::saveSram(const std::string& path) const
{
    if (!sys_.hasSave()) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const std::vector<uint8_t> data = sys_.saveData();
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return f.good();
}

bool N64Console::loadSram(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return sys_.loadSaveData(data);
}
