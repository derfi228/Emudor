#include "memory_bus.h"
#include "cpu/cpu.h"
#include "ppu/ppu.h"
#include "apu/apu.h"
#include "mapper/mapper0.h"
#include "mapper/mapper1.h"
#include "mapper/mapper4.h"
#include "mapper/mapper69.h"
#include <fstream>
#include <algorithm>
#include <cstdio>

void MemoryBus::connectCPU(CPU* cpu) { cpu_ = cpu; }
void MemoryBus::connectPPU(PPU* ppu) { ppu_ = ppu; }
void MemoryBus::connectAPU(APU* apu) { apu_ = apu; }

void MemoryBus::reset() {
    ram_.fill(0);
    controllerShift_[0] = controllerShift_[1] = 0;
    controllerLatch_ = 0;
}

// ─── Синхронизация зеркалирования с PPU ─────────────────────────────────────

void MemoryBus::syncMirrorMode() {
    if (!mapper_ || !ppu_) return;
    uint8_t m = mapper_->mirrorMode();
    if (m == lastMirror_) return;
    lastMirror_ = m;
    static const PPU::MirrorMode table[] = {
        PPU::MirrorMode::Horizontal,
        PPU::MirrorMode::Vertical,
        PPU::MirrorMode::SingleLo,
        PPU::MirrorMode::SingleHi,
    };
    ppu_->setMirrorMode(table[m & 3u]);
}

// ─── Загрузка iNES ROM ───────────────────────────────────────────────────────

bool MemoryBus::loadROM(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    uint8_t header[16];
    f.read(reinterpret_cast<char*>(header), 16);
    if (header[0]!='N'||header[1]!='E'||header[2]!='S'||header[3]!=0x1A)
        return false;

    uint8_t prgBanks = header[4];
    uint8_t chrBanks = header[5];

    // Номер маппера из байтов 6–7 заголовка
    uint8_t mapperNum = (header[7] & 0xF0u) | (header[6] >> 4);

    // Пропустить trainer если есть
    if (header[6] & 0x04u)
        f.seekg(512, std::ios::cur);

    std::vector<uint8_t> prg(prgBanks * 16384);
    f.read(reinterpret_cast<char*>(prg.data()), (std::streamsize)prg.size());

    bool chrIsRam = (chrBanks == 0);
    std::vector<uint8_t> chr;
    if (chrIsRam) {
        chr.resize(8192, 0);
    } else {
        chr.resize(chrBanks * 8192);
        f.read(reinterpret_cast<char*>(chr.data()), (std::streamsize)chr.size());
    }

    // Начальное зеркалирование из заголовка (0=H, 1=V)
    uint8_t mirrorBit = header[6] & 0x01u;
    hasBattery_ = (header[6] & 0x02u) != 0;
    sramDirty_  = false;

    // Фабрика маппера
    switch (mapperNum) {
    case 0:
        mapper_ = std::make_unique<Mapper0>(
            std::move(prg), std::move(chr), chrIsRam, mirrorBit);
        break;
    case 1:
        mapper_ = std::make_unique<Mapper1>(
            std::move(prg), std::move(chr), chrIsRam, mirrorBit);
        break;
    case 4:
        mapper_ = std::make_unique<Mapper4>(
            std::move(prg), std::move(chr), chrIsRam, mirrorBit);
        break;
    case 69:
        mapper_ = std::make_unique<Mapper69>(
            std::move(prg), std::move(chr), chrIsRam, mirrorBit);
        break;
    default:
        // Неподдерживаемый маппер
        mapper_.reset();
        return false;
    }

    lastMirror_ = 0xFF;
    syncMirrorMode();
    return true;
}

// ─── Интерфейс CHR для PPU ───────────────────────────────────────────────────

uint8_t MemoryBus::readCHR(uint16_t addr) {
    return mapper_ ? mapper_->chrRead(addr) : 0;
}

void MemoryBus::writeCHR(uint16_t addr, uint8_t data) {
    if (mapper_) mapper_->chrWrite(addr, data);
}

// ─── Маппинг чтения $0000–$FFFF ─────────────────────────────────────────────

uint8_t MemoryBus::read(uint16_t addr, bool readOnly) {
    // $0000–$1FFF: RAM 2KB зеркалированный × 4
    if (addr <= 0x1FFF)
        return ram_[addr & 0x07FFu];

    // $2000–$3FFF: PPU регистры (8 зеркаленных)
    if (addr <= 0x3FFF) {
        if (readOnly) return 0;
        return ppu_ ? ppu_->readRegister(addr & 0x0007u) : 0;
    }

    // $4016–$4017: контроллеры
    if (addr == 0x4016) {
        uint8_t val = (controllerShift_[0] >> 7) & 0x01u;
        controllerShift_[0] = (uint8_t)(controllerShift_[0] << 1);
        return val;
    }
    if (addr == 0x4017) {
        uint8_t val = (controllerShift_[1] >> 7) & 0x01u;
        controllerShift_[1] = (uint8_t)(controllerShift_[1] << 1);
        return val;
    }

    // $4015: APU статус
    if (addr == 0x4015)
        return apu_ ? apu_->readStatus() : 0;

    // $6000–$7FFF: PRG RAM (если маппер поддерживает)
    if (addr >= 0x6000u && addr <= 0x7FFFu) {
        if (mapper_ && mapper_->hasPrgRam())
            return mapper_->prgRamRead(addr);
        return 0;
    }

    // $8000–$FFFF: PRG ROM через маппер
    if (addr >= 0x8000u)
        return mapper_ ? mapper_->prgRead(addr) : 0;

    return 0;
}

// ─── Маппинг записи $0000–$FFFF ─────────────────────────────────────────────

void MemoryBus::write(uint16_t addr, uint8_t data) {
    if (addr <= 0x1FFFu) {
        ram_[addr & 0x07FFu] = data;
        return;
    }

    if (addr <= 0x3FFFu) {
        if (ppu_) ppu_->writeRegister(addr & 0x0007u, data);
        return;
    }

    // $4014: OAM DMA
    if (addr == 0x4014u) {
        uint16_t page = (uint16_t)(data << 8);
        if (ppu_) {
            for (uint16_t i = 0; i < 256; i++)
                ppu_->writeOAMByte(i, read(page + i));
        }
        return;
    }

    // $4016: strobe контроллера
    if (addr == 0x4016u) {
        controllerLatch_ = data & 0x01u;
        if (controllerLatch_) {
            controllerShift_[0] = controller[0];
            controllerShift_[1] = controller[1];
        }
        return;
    }

    // $4000–$4017: APU
    if (addr <= 0x4017u) {
        if (apu_) apu_->write(addr, data);
        return;
    }

    // $6000–$7FFF: PRG RAM
    if (addr >= 0x6000u && addr <= 0x7FFFu) {
        if (mapper_ && mapper_->hasPrgRam()) {
            mapper_->prgRamWrite(addr, data);
            if (hasBattery_) sramDirty_ = true;
        }
        return;
    }

    // $8000–$FFFF: запись в маппер (регистры банков)
    if (addr >= 0x8000u) {
        if (mapper_) {
            mapper_->prgWrite(addr, data);
            syncMirrorMode();   // MMC1/MMC3 могут сменить зеркалирование
        }
    }
}

// ─── Battery SRAM: save / load ───────────────────────────────────────────────

bool MemoryBus::saveSram(const std::string& path) const {
    if (!mapper_) return false;
    const uint8_t* data = mapper_->prgRamPtr();
    size_t         size = mapper_->prgRamBytes();
    if (!data || size == 0) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), (std::streamsize)size);
    return f.good();
}

bool MemoryBus::loadSram(const std::string& path) {
    if (!mapper_) return false;
    uint8_t* data = mapper_->prgRamPtr();
    size_t   size = mapper_->prgRamBytes();
    if (!data || size == 0) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(data), (std::streamsize)size);
    return f.gcount() > 0;
}

uint8_t* MemoryBus::mapperPrgRamPtr() {
    return mapper_ ? mapper_->prgRamPtr() : nullptr;
}
const uint8_t* MemoryBus::mapperPrgRamPtr() const {
    return mapper_ ? mapper_->prgRamPtr() : nullptr;
}
size_t MemoryBus::mapperPrgRamBytes() const {
    return mapper_ ? mapper_->prgRamBytes() : 0;
}

// ─── Маппер: IRQ / счётчик скэнлайнов ────────────────────────────────────────

void MemoryBus::mapperScanline() {
    if (mapper_) mapper_->scanline();
}

void MemoryBus::mapperCpuClock() {
    if (mapper_) mapper_->cpuClock();
}

bool MemoryBus::mapperIrqPending() const {
    return mapper_ && mapper_->irqPending();
}

void MemoryBus::mapperClearIrq() {
    if (mapper_) mapper_->clearIRQ();
}
