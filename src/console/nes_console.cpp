#include "console/nes_console.h"
#include <ostream>
#include <istream>
#include <cstring>

// ─── Конструктор: соединяем компоненты ───────────────────────────────────────

NesConsole::NesConsole() {
    bus_.connectCPU(&cpu_);
    bus_.connectPPU(&ppu_);
    bus_.connectAPU(&apu_);
    cpu_.connectBus(&bus_);
    ppu_.connectBus(&bus_);
}

// ─── IConsole ────────────────────────────────────────────────────────────────

bool NesConsole::loadROM(const std::string& path) {
    if (!bus_.loadROM(path)) return false;

    // Сброс всех компонентов после загрузки ROM
    cpu_.reset();
    ppu_.reset();
    bus_.reset();
    apu_.reset();

    // 7 «warm-up» тактов CPU: заполняет конвейер и читает reset-вектор
    for (int i = 0; i < 7; i++) cpu_.clock();

    return true;
}

void NesConsole::reset() {
    cpu_.reset();
    ppu_.reset();
    bus_.reset();
    apu_.reset();
}

void NesConsole::setInput(int player, uint16_t buttons) {
    // NES использует только нижние 8 бит (A|B|Sel|Sta|Up|Dn|L|R)
    if (player >= 0 && player < 2)
        bus_.controller[player] = (uint8_t)(buttons & 0xFF);
}

void NesConsole::runFrame() {
    ppu_.frameComplete = false;
    while (!ppu_.frameComplete) {
        // PPU тикает в 3× быстрее CPU
        ppu_.clock();
        if (ppu_.nmiPending) { ppu_.nmiPending = false; cpu_.nmi(); }
        ppu_.clock();
        ppu_.clock();

        // IRQ от маппера (MMC3 по скэнлайну, FME-7 по CPU-тактам)
        bus_.mapperCpuClock();
        if (bus_.mapperIrqPending()) { bus_.mapperClearIrq(); cpu_.irq(); }

        cpu_.clock();
        apu_.clock();
    }
}

// ─── Save state v3 ───────────────────────────────────────────────────────────
// Формат: "NESS" + uint32 ver=3 + CPU-регистры(15) + PPU::State + RAM(2048)
//         + uint8 hasSram [+ uint32 sz + data]

bool NesConsole::saveState(std::ostream& os) const {
    os.write("NESS", 4);

    const uint32_t ver = 3;
    os.write(reinterpret_cast<const char*>(&ver), 4);

    // CPU-регистры
    os.write(reinterpret_cast<const char*>(&cpu_.A),            1);
    os.write(reinterpret_cast<const char*>(&cpu_.X),            1);
    os.write(reinterpret_cast<const char*>(&cpu_.Y),            1);
    os.write(reinterpret_cast<const char*>(&cpu_.SP),           1);
    os.write(reinterpret_cast<const char*>(&cpu_.P),            1);
    os.write(reinterpret_cast<const char*>(&cpu_.PC),           2);
    os.write(reinterpret_cast<const char*>(&cpu_.totalCycles_), 8);

    // PPU-состояние
    PPU::State ppuSt = ppu_.getState();
    os.write(reinterpret_cast<const char*>(&ppuSt), sizeof(ppuSt));

    // RAM (2 KB)
    const auto& ram = bus_.getRAM();
    os.write(reinterpret_cast<const char*>(ram.data()), (std::streamsize)ram.size());

    // PRG RAM (battery SRAM)
    const uint8_t* sramData = bus_.mapperPrgRamPtr();
    const size_t   sramSize = bus_.mapperPrgRamBytes();
    const uint8_t  hasSram  = (sramData && sramSize > 0) ? 1u : 0u;
    os.write(reinterpret_cast<const char*>(&hasSram), 1);
    if (hasSram) {
        const uint32_t sz = static_cast<uint32_t>(sramSize);
        os.write(reinterpret_cast<const char*>(&sz), 4);
        os.write(reinterpret_cast<const char*>(sramData), (std::streamsize)sramSize);
    }

    return os.good();
}

bool NesConsole::loadState(std::istream& is) {
    char sig[4];
    is.read(sig, 4);
    if (std::memcmp(sig, "NESS", 4) != 0) return false;

    uint32_t ver = 0;
    is.read(reinterpret_cast<char*>(&ver), 4);
    if (ver != 2 && ver != 3) return false;

    // CPU-регистры
    is.read(reinterpret_cast<char*>(&cpu_.A),            1);
    is.read(reinterpret_cast<char*>(&cpu_.X),            1);
    is.read(reinterpret_cast<char*>(&cpu_.Y),            1);
    is.read(reinterpret_cast<char*>(&cpu_.SP),           1);
    is.read(reinterpret_cast<char*>(&cpu_.P),            1);
    is.read(reinterpret_cast<char*>(&cpu_.PC),           2);
    is.read(reinterpret_cast<char*>(&cpu_.totalCycles_), 8);

    // PPU-состояние
    PPU::State ppuSt{};
    is.read(reinterpret_cast<char*>(&ppuSt), sizeof(ppuSt));
    ppu_.setState(ppuSt);

    // RAM (2 KB)
    auto& ram = bus_.getRAM();
    is.read(reinterpret_cast<char*>(ram.data()), (std::streamsize)ram.size());

    // v3: PRG RAM блок
    if (ver == 3) {
        uint8_t hasSram = 0;
        is.read(reinterpret_cast<char*>(&hasSram), 1);
        if (hasSram) {
            uint32_t sz = 0;
            is.read(reinterpret_cast<char*>(&sz), 4);
            uint8_t* dst = bus_.mapperPrgRamPtr();
            size_t   cap = bus_.mapperPrgRamBytes();
            if (dst && sz <= cap)
                is.read(reinterpret_cast<char*>(dst), sz);
        }
    }

    return is.good() || is.eof();
}
