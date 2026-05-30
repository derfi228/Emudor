#include "mapper4.h"

Mapper4::Mapper4(std::vector<uint8_t> prg,
                 std::vector<uint8_t> chr,
                 bool                 chrIsRam,
                 uint8_t              mirrorBit)
    : prg_(std::move(prg))
    , chr_(std::move(chr))
    , chrIsRam_(chrIsRam)
    , mirror_(mirrorBit)
{}

// ─── PRG чтение ──────────────────────────────────────────────────────────────
// Режим PRG (бит 6 bankSel_):
//   0: R6 → $8000, фиксированный предпоследний → $C000
//   1: фиксированный предпоследний → $8000, R6 → $C000
// $A000 всегда R7. $E000 всегда последний банк.

uint8_t Mapper4::prgRead(uint32_t addr) {
    uint32_t numBanks = (uint32_t)(prg_.size() / 8192);
    if (numBanks == 0) return 0;

    uint8_t  mask     = (uint8_t)(numBanks - 1);
    bool     prgMode  = (bankSel_ >> 6) & 1u;
    uint32_t bank;

    if (addr <= 0x9FFFu) {
        bank = prgMode ? (numBanks - 2) : (reg_[6] & mask);
    } else if (addr <= 0xBFFFu) {
        bank = reg_[7] & mask;
    } else if (addr <= 0xDFFFu) {
        bank = prgMode ? (reg_[6] & mask) : (numBanks - 2);
    } else {
        bank = numBanks - 1;
    }

    return prg_[bank * 8192 + (addr & 0x1FFFu)];
}

// ─── PRG запись (регистры MMC3) ───────────────────────────────────────────────

void Mapper4::prgWrite(uint32_t addr, uint8_t data) {
    bool odd = (addr & 1u) != 0;

    if (addr <= 0x9FFFu) {
        if (!odd) {
            bankSel_ = data;
        } else {
            reg_[bankSel_ & 0x07u] = data;
        }
    } else if (addr <= 0xBFFFu) {
        if (!odd) {
            // 0 = Vertical (наш код: 1), 1 = Horizontal (наш код: 0)
            mirror_ = (data & 1u) ? 0u : 1u;
        }
        // $A001 PRG RAM protect — не эмулируем полностью
    } else if (addr <= 0xDFFFu) {
        if (!odd) {
            irqLatch_ = data;
        } else {
            irqCounter_ = 0;
            irqReload_  = true;
        }
    } else {
        if (!odd) {
            irqPending_ = false;
            irqEnable_  = false;
        } else {
            irqEnable_ = true;
        }
    }
}

// ─── CHR чтение ──────────────────────────────────────────────────────────────

uint8_t Mapper4::chrRead(uint16_t addr) {
    if (chrIsRam_) return chr_[addr & 0x1FFFu];
    uint32_t mapped = chrBankAddr(addr) % (uint32_t)chr_.size();
    return chr_[mapped];
}

void Mapper4::chrWrite(uint16_t addr, uint8_t data) {
    if (chrIsRam_) chr_[addr & 0x1FFFu] = data;
}

// CHR-адрес → физический индекс в chr_
// chrMode (бит 7 bankSel_):
//   0: R0/R1 (2KB) в $0000-$0FFF, R2-R5 (1KB) в $1000-$1FFF
//   1: R2-R5 (1KB) в $0000-$0FFF, R0/R1 (2KB) в $1000-$1FFF

uint32_t Mapper4::chrBankAddr(uint16_t addr) const {
    bool chrMode = (bankSel_ >> 7) & 1u;

    if (!chrMode) {
        if      (addr < 0x0800u) return ((uint32_t)(reg_[0] & 0xFEu) << 10) | (addr & 0x7FFu);
        else if (addr < 0x1000u) return ((uint32_t)(reg_[1] & 0xFEu) << 10) | (addr & 0x7FFu);
        else if (addr < 0x1400u) return ((uint32_t)reg_[2]           << 10) | (addr & 0x3FFu);
        else if (addr < 0x1800u) return ((uint32_t)reg_[3]           << 10) | (addr & 0x3FFu);
        else if (addr < 0x1C00u) return ((uint32_t)reg_[4]           << 10) | (addr & 0x3FFu);
        else                     return ((uint32_t)reg_[5]           << 10) | (addr & 0x3FFu);
    } else {
        if      (addr < 0x0400u) return ((uint32_t)reg_[2]           << 10) | (addr & 0x3FFu);
        else if (addr < 0x0800u) return ((uint32_t)reg_[3]           << 10) | (addr & 0x3FFu);
        else if (addr < 0x0C00u) return ((uint32_t)reg_[4]           << 10) | (addr & 0x3FFu);
        else if (addr < 0x1000u) return ((uint32_t)reg_[5]           << 10) | (addr & 0x3FFu);
        else if (addr < 0x1800u) return ((uint32_t)(reg_[0] & 0xFEu) << 10) | (addr & 0x7FFu);
        else                     return ((uint32_t)(reg_[1] & 0xFEu) << 10) | (addr & 0x7FFu);
    }
}

// ─── PRG RAM ($6000–$7FFF) ────────────────────────────────────────────────────

uint8_t Mapper4::prgRamRead(uint16_t addr) const {
    return prgRam_[(addr - 0x6000u) & 0x1FFFu];
}

void Mapper4::prgRamWrite(uint16_t addr, uint8_t data) {
    prgRam_[(addr - 0x6000u) & 0x1FFFu] = data;
}

// ─── Счётчик IRQ (вызывается из PPU на dot 260 видимых скэнлайнов) ───────────
// IRQ генерируется ТОЛЬКО при декременте счётчика до 0, но не при перезагрузке.
// Ошибка «стрелять при reload» ломает SMB3 (преждевременный IRQ в mode select).

void Mapper4::scanline() {
    if (irqReload_ || irqCounter_ == 0) {
        // Перезагрузка — IRQ не генерируем
        irqCounter_ = irqLatch_;
        irqReload_  = false;
    } else {
        irqCounter_--;
        if (irqCounter_ == 0 && irqEnable_) {
            irqPending_ = true;
        }
    }
}
