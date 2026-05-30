#include "mapper1.h"

// ─── Конструктор ─────────────────────────────────────────────────────────────

Mapper1::Mapper1(std::vector<uint8_t> prg, std::vector<uint8_t> chr,
                 bool chrIsRam, uint8_t initMirror)
    : prg_(std::move(prg)), chr_(std::move(chr)), chrIsRam_(chrIsRam)
{
    numPrg16_ = static_cast<uint8_t>(prg_.size() / 16384);
    numChr4_  = chrIsRam_ ? 0
                           : static_cast<uint8_t>(chr_.size() / 4096);

    // Начальное зеркалирование (initMirror: 0=H, 1=V из iNES header)
    // MMC1 ctrlReg bits[1:0]: 0=SingleLo, 1=SingleHi, 2=Vertical, 3=Horizontal
    uint8_t mm = (initMirror == 1) ? 2u : 3u;
    ctrlReg_ = (ctrlReg_ & 0xFCu) | (mm & 3u);
}

// ─── Сдвиговый регистр ───────────────────────────────────────────────────────

void Mapper1::shiftWrite(uint32_t addr, uint8_t data) {
    if (data & 0x80) {
        // Принудительный сброс: PRG mode → 3
        shiftReg_ = 0x10;
        ctrlReg_ |= 0x0Cu;
        return;
    }
    // Определяем, завершён ли 5-й сдвиг (sentinel сдошёл до bit0)
    bool complete = (shiftReg_ & 1u) != 0;
    shiftReg_ = static_cast<uint8_t>((shiftReg_ >> 1) | ((data & 1u) << 4));
    if (complete) {
        applyRegister(addr, shiftReg_);
        shiftReg_ = 0x10;
    }
}

void Mapper1::applyRegister(uint32_t addr, uint8_t value) {
    // Биты 14-13 адреса выбирают регистр
    switch ((addr >> 13) & 3u) {
    case 0: ctrlReg_  = value & 0x1Fu; break;   // $8000–$9FFF
    case 1: chrBank0_ = value & 0x1Fu; break;   // $A000–$BFFF
    case 2: chrBank1_ = value & 0x1Fu; break;   // $C000–$DFFF
    case 3: prgBank_  = value & 0x1Fu; break;   // $E000–$FFFF
    }
}

// ─── PRG ROM ─────────────────────────────────────────────────────────────────

uint8_t Mapper1::prgRead(uint32_t addr) {
    uint8_t  mode    = (ctrlReg_ >> 2) & 3u;
    uint32_t prgAddr = 0;

    if (mode <= 1) {
        // 32KB: prgBank_ bit0 игнорируется
        uint32_t bank = static_cast<uint32_t>(prgBank_ & 0x0Eu) >> 1;
        prgAddr = bank * 32768u + (addr - 0x8000u);
    } else if (mode == 2) {
        // Фиксируем банк 0 на $8000–$BFFF, переключаем $C000–$FFFF
        if (addr < 0xC000u)
            prgAddr = addr - 0x8000u;
        else
            prgAddr = static_cast<uint32_t>(prgBank_ & 0x0Fu) * 16384u
                      + (addr - 0xC000u);
    } else {
        // mode 3: переключаем $8000–$BFFF, фиксируем последний банк на $C000
        if (addr < 0xC000u)
            prgAddr = static_cast<uint32_t>(prgBank_ & 0x0Fu) * 16384u
                      + (addr - 0x8000u);
        else
            prgAddr = static_cast<uint32_t>(numPrg16_ - 1) * 16384u
                      + (addr - 0xC000u);
    }

    if (!prg_.empty()) prgAddr %= prg_.size();
    return prg_[prgAddr];
}

void Mapper1::prgWrite(uint32_t addr, uint8_t data) {
    shiftWrite(addr, data);
}

// ─── CHR ROM/RAM ─────────────────────────────────────────────────────────────

uint8_t Mapper1::chrRead(uint16_t addr) {
    if (chrIsRam_)
        return (addr < static_cast<uint16_t>(chr_.size())) ? chr_[addr] : 0;

    bool     mode8kb = ((ctrlReg_ >> 4) & 1u) == 0;
    uint32_t chrAddr = 0;

    if (mode8kb) {
        // 8KB: chrBank0_ bit0 игнорируется
        uint32_t bank = static_cast<uint32_t>(chrBank0_ & 0x1Eu) >> 1;
        chrAddr = bank * 8192u + addr;
    } else {
        // 4KB: два независимых банка
        if (addr < 0x1000u)
            chrAddr = static_cast<uint32_t>(chrBank0_ & 0x1Fu) * 4096u + addr;
        else
            chrAddr = static_cast<uint32_t>(chrBank1_ & 0x1Fu) * 4096u
                      + (addr - 0x1000u);
    }

    if (!chr_.empty()) chrAddr %= chr_.size();
    return chr_[chrAddr];
}

void Mapper1::chrWrite(uint16_t addr, uint8_t data) {
    if (chrIsRam_ && addr < static_cast<uint16_t>(chr_.size()))
        chr_[addr] = data;
}

// ─── PRG RAM ($6000–$7FFF) ───────────────────────────────────────────────────

uint8_t Mapper1::prgRamRead(uint16_t addr) const {
    if ((prgBank_ >> 4) & 1u) return 0;  // RAM отключена
    return prgRam_[addr & 0x1FFFu];
}

void Mapper1::prgRamWrite(uint16_t addr, uint8_t data) {
    if (!((prgBank_ >> 4) & 1u))
        prgRam_[addr & 0x1FFFu] = data;
}

// ─── Зеркалирование ──────────────────────────────────────────────────────────

uint8_t Mapper1::mirrorMode() const {
    // ctrlReg bits[1:0]: 0=SingleLo, 1=SingleHi, 2=Vertical, 3=Horizontal
    // Возвращаем в нашем формате: 0=H, 1=V, 2=SLo, 3=SHi
    switch (ctrlReg_ & 3u) {
    case 0: return 2;   // SingleLo
    case 1: return 3;   // SingleHi
    case 2: return 1;   // Vertical
    case 3: return 0;   // Horizontal
    default: return 0;
    }
}
