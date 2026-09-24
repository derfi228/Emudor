// gb_console.cpp — Game Boy / Game Boy Color как IConsole.
#include "gb_console.h"
#include "state_io.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

namespace {
constexpr uint64_t kDotsPerFrame  = 70224;     // 154 строки × 456 точек
constexpr uint64_t kDotsPerSecond = 4194304;
} // namespace

bool GbConsole::loadROM(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return loadROMData(std::move(rom));
}

bool GbConsole::loadROMData(std::vector<uint8_t> rom)
{
    if (!cart_.load(std::move(rom))) return false;
    reset();
    return true;
}

// Включение питания: картридж помнит своё ОЗУ, остальное — как после загрузчика.
void GbConsole::reset()
{
    const bool cgb = cart_.cgbSupported();
    cart_.resetMapper();
    bus_.init(&cart_, cgb);
    cpu_.connect(&bus_);
    cpu_.reset(cgb);
    rtcDots_ = 0;
}

// Кадр кончается с началом VBlank. Если экран выключен, кадра нет — тогда
// отмеряем те же 70224 точки, чтобы время шло с нормальной скоростью.
void GbConsole::runFrame()
{
    const uint64_t start = bus_.dots();
    bus_.ppu.frameDone = false;
    while (!bus_.ppu.frameDone && bus_.dots() - start < kDotsPerFrame)
        cpu_.step();

    // Диагностика: состояние раз в кадр (EMUDOR_GB_TRACE=1).
    static const bool s_trace = std::getenv("EMUDOR_GB_TRACE") != nullptr;
    if (s_trace) {
        std::fprintf(stderr, "GB pc=%04X sp=%04X ime=%d halt=%d IE=%02X IF=%02X LCDC=%02X STAT=%02X LY=%3d SCX=%02X\n",
                     cpu_.pc, cpu_.sp, (int)cpu_.ime, (int)cpu_.halted, bus_.read(0xFFFF), bus_.read(0xFF0F),
                     bus_.read(0xFF40), bus_.read(0xFF41), bus_.read(0xFF44), bus_.read(0xFF43));
    }

    // Часы MBC3 идут по эмулированному времени.
    rtcDots_ += bus_.dots() - start;
    while (rtcDots_ >= kDotsPerSecond) {
        rtcDots_ -= kDotsPerSecond;
        cart_.advanceRtc(1);
    }
}

void GbConsole::setInput(int player, uint16_t buttons)
{
    if (player != 0) return;
    uint8_t gb = 0;
    if (buttons & 0x01) gb |= 0x01;    // Right
    if (buttons & 0x02) gb |= 0x02;    // Left
    if (buttons & 0x08) gb |= 0x04;    // Up
    if (buttons & 0x04) gb |= 0x08;    // Down
    if (buttons & 0x80) gb |= 0x10;    // A
    if (buttons & 0x40) gb |= 0x20;    // B
    if (buttons & 0x20) gb |= 0x40;    // Select
    if (buttons & 0x10) gb |= 0x80;    // Start
    bus_.setButtons(gb);
}

// ─── Батарейка ────────────────────────────────────────────────────────────────
bool GbConsole::saveSram(const std::string& path) const
{
    if (!cart_.hasBattery()) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const std::vector<uint8_t> data = cart_.saveData();
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return f.good();
}

bool GbConsole::loadSram(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return cart_.loadSaveData(data, (uint64_t)std::time(nullptr));
}

// ─── Save state ("GBSS" v1) ──────────────────────────────────────────────────
static constexpr uint32_t GB_SAVE_MAGIC   = 0x53534247u;   // "GBSS"
static constexpr uint32_t GB_SAVE_VERSION = 1u;

template<class S> void GbConsole::serialize(S& s)
{
    s.expect(GB_SAVE_MAGIC);
    s.expect(GB_SAVE_VERSION);
    s.expect(cart_.romHash());          // состояние от другой игры не подойдёт
    cart_.serialize(s);
    bus_.serialize(s);
    cpu_.serialize(s);
    s.io(rtcDots_);
}

bool GbConsole::saveState(std::ostream& os) const
{
    StateWriter w(os);
    const_cast<GbConsole*>(this)->serialize(w);   // запись ничего не меняет
    return w.ok();
}

// Загрузка атомарная: чужой или обрезанный файл откатывается.
bool GbConsole::loadState(std::istream& is)
{
    std::stringstream backup;
    StateWriter w(backup);
    serialize(w);

    StateReader r(is);
    serialize(r);
    if (!r.ok()) {
        StateReader undo(backup);
        serialize(undo);
        return false;
    }
    bus_.apu.clearSamples();
    return true;
}
