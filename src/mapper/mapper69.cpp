#include "mapper69.h"

Mapper69::Mapper69(std::vector<uint8_t> prg,
                   std::vector<uint8_t> chr,
                   bool                 chrIsRam,
                   uint8_t              mirrorBit)
    : prg_(std::move(prg))
    , chr_(std::move(chr))
    , chrIsRam_(chrIsRam)
    , mirror_(mirrorBit)
{
    // Начальные значения PRG банков.
    // $E000-$FFFF фиксирован на последнем банке — инициализация не нужна.
    // Остальные слоты ($8000/$A000/$C000/$6000) начинают с банка 0.
    prgBank_[0] = 0;  // $8000-$9FFF
    prgBank_[1] = 0;  // $A000-$BFFF
    prgBank_[2] = 0;  // $C000-$DFFF
    prgBank_[3] = 0;  // $6000-$7FFF
}

// ─── PRG чтение ($8000–$FFFF) ────────────────────────────────────────────────

uint8_t Mapper69::prgRead(uint32_t addr) {
    if (addr < 0x8000u) return 0;

    uint32_t numBanks = (uint32_t)(prg_.size() / 8192);
    if (numBanks == 0) return 0;

    // $E000-$FFFF: ФИКСИРОВАНО = последний банк (содержит векторы и boot-код)
    if (addr >= 0xE000u) {
        return prg_[(numBanks - 1) * 8192u + (addr & 0x1FFFu)];
    }

    // $8000-$DFFF: три переключаемых слота
    //   slot 0 = $8000-$9FFF → prgBank_[0]
    //   slot 1 = $A000-$BFFF → prgBank_[1]
    //   slot 2 = $C000-$DFFF → prgBank_[2]
    uint8_t  slot = (uint8_t)((addr - 0x8000u) >> 13);  // 0, 1 или 2
    uint32_t bank = prgBank_[slot] % numBanks;
    return prg_[bank * 8192u + (addr & 0x1FFFu)];
}

// ─── PRG запись (регистры FME-7) ─────────────────────────────────────────────
// $8000–$9FFF: регистр команды (биты 3–0)
// $A000–$BFFF: параметр текущей команды

void Mapper69::prgWrite(uint32_t addr, uint8_t data) {
    if (addr <= 0x9FFFu) {
        cmdReg_ = data & 0x0Fu;
        return;
    }
    if (addr > 0xBFFFu) return;  // запись только в $A000-$BFFF

    switch (cmdReg_) {
    // CHR 1KB банки (команды 0–7)
    case 0: case 1: case 2: case 3:
    case 4: case 5: case 6: case 7:
        chrBank_[cmdReg_] = data;
        break;

    // PRG банки 8KB
    // Команда $08: слот $6000-$7FFF — может быть ROM или RAM (бит 6)
    case 0x8:
        slot0Ram_    = (data & 0x40u) != 0;
        prgBank_[3]  = data & 0x3Fu;   // банк для $6000-$7FFF
        break;
    // Команда $09: слот $8000-$9FFF
    case 0x9: prgBank_[0] = data & 0x3Fu; break;
    // Команда $0A: слот $A000-$BFFF
    case 0xA: prgBank_[1] = data & 0x3Fu; break;
    // Команда $0B: слот $C000-$DFFF
    case 0xB: prgBank_[2] = data & 0x3Fu; break;

    // Зеркалирование (команда $C)
    // 0=Vertical, 1=Horizontal, 2=SingleLo, 3=SingleHi
    case 0xC:
        switch (data & 0x03u) {
        case 0: mirror_ = 1; break;  // Vertical
        case 1: mirror_ = 0; break;  // Horizontal
        case 2: mirror_ = 2; break;  // SingleLo
        case 3: mirror_ = 3; break;  // SingleHi
        }
        break;

    // IRQ control (команда $D)
    // бит 7: разрешить отсчёт; бит 0: разрешить прерывание
    case 0xD:
        irqCountEn_ = (data & 0x80u) != 0;
        irqEnable_  = (data & 0x01u) != 0;
        if (!irqEnable_) irqPending_ = false;
        break;

    // IRQ counter low (команда $E)
    case 0xE:
        irqCounter_ = (irqCounter_ & 0xFF00u) | data;
        break;

    // IRQ counter high (команда $F)
    case 0xF:
        irqCounter_ = (irqCounter_ & 0x00FFu) | ((uint16_t)data << 8);
        break;
    }
}

// ─── CHR чтение ──────────────────────────────────────────────────────────────

uint8_t Mapper69::chrRead(uint16_t addr) {
    if (chrIsRam_) return chr_[addr & 0x1FFFu];
    uint8_t  slot     = (uint8_t)(addr >> 10);  // 0–7, по 1KB
    uint32_t numBanks = (uint32_t)(chr_.size() / 1024);
    if (numBanks == 0) return 0;
    uint32_t bank = chrBank_[slot] % numBanks;
    return chr_[bank * 1024u + (addr & 0x3FFu)];
}

void Mapper69::chrWrite(uint16_t addr, uint8_t data) {
    if (chrIsRam_) chr_[addr & 0x1FFFu] = data;
}

// ─── PRG $6000–$7FFF (слот команды $08) ──────────────────────────────────────
// Если slot0Ram_=true  — это 8KB PRG RAM (запись разрешена).
// Если slot0Ram_=false — это PRG ROM банк, выбранный командой $08.

uint8_t Mapper69::prgRamRead(uint16_t addr) const {
    uint16_t off = addr - 0x6000u;
    if (slot0Ram_) {
        return prgRam_[off & 0x1FFFu];
    }
    // PRG ROM банк для $6000-$7FFF
    uint32_t numBanks = (uint32_t)(prg_.size() / 8192);
    if (numBanks == 0) return 0;
    uint32_t bank = prgBank_[3] % numBanks;
    return prg_[bank * 8192u + (off & 0x1FFFu)];
}

void Mapper69::prgRamWrite(uint16_t addr, uint8_t data) {
    // Запись в $6000-$7FFF работает только когда слот настроен как RAM
    if (slot0Ram_) {
        prgRam_[(addr - 0x6000u) & 0x1FFFu] = data;
    }
    // Если ROM — запись игнорируется
}

// ─── CPU-тактовый IRQ (счётчик уменьшается каждый CPU-такт) ─────────────────

void Mapper69::cpuClock() {
    if (!irqCountEn_) return;

    if (irqCounter_ == 0) {
        if (irqEnable_) irqPending_ = true;
        irqCounter_ = 0xFFFFu;  // wraparound
    } else {
        irqCounter_--;
    }
}
