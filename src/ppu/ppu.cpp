#include "ppu.h"
#include "memory/memory_bus.h"
#include <cstring>

// ─── NTSC палитра (64 цвета, ARGB8888) ───────────────────────────────────────

const std::array<uint32_t, 64> PPU::kPalette = {{
    0xFF545454,0xFF001E74,0xFF081090,0xFF300088,0xFF440064,0xFF5C0030,0xFF540400,0xFF3C1800,
    0xFF202A00,0xFF083A00,0xFF004000,0xFF003C00,0xFF00323C,0xFF000000,0xFF000000,0xFF000000,
    0xFF989698,0xFF084CC4,0xFF3032EC,0xFF5C1EE4,0xFF8814B0,0xFFA01464,0xFF982220,0xFF783C00,
    0xFF545A00,0xFF287200,0xFF087C00,0xFF007628,0xFF006678,0xFF000000,0xFF000000,0xFF000000,
    0xFFECEEEC,0xFF4C9AEC,0xFF787CEC,0xFFB062EC,0xFFE454EC,0xFFEC58B4,0xFFEC6A64,0xFFD48820,
    0xFFA0AA00,0xFF74C400,0xFF4CD020,0xFF38CC6C,0xFF38B4CC,0xFF3C3C3C,0xFF000000,0xFF000000,
    0xFFECEEEC,0xFFA8CCEC,0xFFBCBCEC,0xFFD4B2EC,0xFFECAEEC,0xFFECAED4,0xFFECB4B0,0xFFE4C490,
    0xFFCCD278,0xFFB4DE78,0xFFA8E290,0xFF98E2B4,0xFFA0D6E4,0xFFA0A2A0,0xFF000000,0xFF000000,
}};

// ─── Вспомогательные функции ─────────────────────────────────────────────────

// Разворот битов байта (для горизонтального флипа спрайтов)
static uint8_t reverseBits(uint8_t b) {
    b = (uint8_t)((b & 0xF0u) >> 4 | (b & 0x0Fu) << 4);
    b = (uint8_t)((b & 0xCCu) >> 2 | (b & 0x33u) << 2);
    b = (uint8_t)((b & 0xAAu) >> 1 | (b & 0x55u) << 1);
    return b;
}

// ─── Конструктор / подключение ───────────────────────────────────────────────

PPU::PPU() = default;
void PPU::connectBus(MemoryBus* bus) { bus_ = bus; }
void PPU::setMirrorMode(MirrorMode mode) { mirrorMode_ = mode; }

void PPU::reset() {
    ctrl_.reg    = 0;
    mask_.reg    = 0;
    status_.reg  = 0;
    oamAddr_     = 0;
    addrLatch_   = false;
    ppuDataBuf_  = 0;
    scanline_    = 0;
    dot_         = 0;
    nmiPending   = false;
    frameComplete= false;
    vram_.reg    = 0;
    tram_.reg    = 0;
    fineX_       = 0;
    spriteCount_ = 0;
    bgShiftPatLo_ = bgShiftPatHi_ = bgShiftAttrLo_ = bgShiftAttrHi_ = 0;
    for (auto& s : spriteBuf_) s.active = false;
    screenBuffer.fill(0xFF000000);
}

// ─── Зеркалирование nametable ─────────────────────────────────────────────────

uint16_t PPU::mirrorNametableAddr(uint16_t addr) const {
    // addr уже в диапазоне 0x0000–0x0FFF (после addr &= 0x0FFF)
    // NT0 = physical [0x000–0x3FF], NT1 = physical [0x400–0x7FF]
    switch (mirrorMode_) {
    case MirrorMode::Vertical:
        // slot 0→NT0, slot 1→NT1, slot 2→NT0, slot 3→NT1
        return addr & 0x07FF;

    case MirrorMode::Horizontal:
        // slot 0→NT0, slot 1→NT0, slot 2→NT1, slot 3→NT1
        return ((addr >> 1) & 0x0400) | (addr & 0x03FF);

    case MirrorMode::SingleLo:
        return addr & 0x03FF;

    case MirrorMode::SingleHi:
        return (addr & 0x03FF) | 0x0400;

    case MirrorMode::FourScreen:
        // Требует дополнительного VRAM — храним только 2KB, используем Vertical
        return addr & 0x07FF;

    default:
        return addr & 0x07FF;
    }
}

// ─── VRAM чтение/запись ──────────────────────────────────────────────────────

uint8_t PPU::readVRAM(uint16_t addr) const {
    addr &= 0x3FFF;

    if (addr < 0x2000)
        return bus_ ? bus_->readCHR(addr) : 0;

    if (addr < 0x3F00)
        return nameTables_[mirrorNametableAddr(addr & 0x0FFF)];

    // Палитра $3F00–$3F1F (зеркало $3F20–$3FFF)
    addr &= 0x001F;
    // $3F10/$3F14/$3F18/$3F1C — зеркала $3F00/$3F04/$3F08/$3F0C
    if (addr == 0x10 || addr == 0x14 || addr == 0x18 || addr == 0x1C)
        addr &= 0x0F;
    return palette_[addr];
}

void PPU::writeVRAM(uint16_t addr, uint8_t data) {
    addr &= 0x3FFF;

    if (addr < 0x2000) {
        // CHR RAM (для игр без CHR ROM)
        if (bus_) bus_->writeCHR(addr, data);
        return;
    }

    if (addr < 0x3F00) {
        nameTables_[mirrorNametableAddr(addr & 0x0FFF)] = data;
        return;
    }

    addr &= 0x001F;
    if (addr == 0x10 || addr == 0x14 || addr == 0x18 || addr == 0x1C)
        addr &= 0x0F;
    palette_[addr] = data;
}

// getColor: pixel==0 всегда читает универсальный фон ($3F00)
uint32_t PPU::getColor(uint8_t paletteIdx, uint8_t pixel) const {
    uint16_t addr = (pixel == 0)
        ? 0x3F00
        : (uint16_t)(0x3F00 + ((uint16_t)paletteIdx << 2) + pixel);
    return kPalette[readVRAM(addr) & 0x3F];
}

// ─── Регистры PPU ($2000–$2007) ──────────────────────────────────────────────

uint8_t PPU::readRegister(uint8_t reg) {
    switch (reg) {
    case 2: {   // PPUSTATUS
        uint8_t val = (status_.reg & 0xE0) | (ppuDataBuf_ & 0x1F);
        status_.vblank = 0;
        addrLatch_     = false;
        return val;
    }
    case 4:     // OAMDATA
        return oam_[oamAddr_];
    case 7: {   // PPUDATA
        uint8_t val = ppuDataBuf_;
        ppuDataBuf_  = readVRAM(vram_.reg);
        // Палитра читается без задержки
        if (vram_.reg >= 0x3F00) val = ppuDataBuf_;
        vram_.reg += ctrl_.increment ? 32 : 1;
        return val;
    }
    default: return 0;
    }
}

void PPU::writeRegister(uint8_t reg, uint8_t data) {
    switch (reg) {
    case 0:   // PPUCTRL
        ctrl_.reg       = data;
        tram_.nametable = ctrl_.nametable;
        break;
    case 1:   // PPUMASK
        mask_.reg = data;
        break;
    case 3:   // OAMADDR
        oamAddr_ = data;
        break;
    case 4:   // OAMDATA
        oam_[oamAddr_++] = data;
        break;
    case 5:   // PPUSCROLL
        if (!addrLatch_) {
            fineX_        = data & 0x07;
            tram_.coarseX = data >> 3;
        } else {
            tram_.fineY   = data & 0x07;
            tram_.coarseY = data >> 3;
        }
        addrLatch_ = !addrLatch_;
        break;
    case 6:   // PPUADDR
        if (!addrLatch_) {
            tram_.reg = (uint16_t)((data & 0x3F) << 8) | (tram_.reg & 0x00FF);
        } else {
            tram_.reg = (tram_.reg & 0xFF00) | data;
            vram_.reg  = tram_.reg;
        }
        addrLatch_ = !addrLatch_;
        break;
    case 7:   // PPUDATA
        writeVRAM(vram_.reg, data);
        vram_.reg += ctrl_.increment ? 32 : 1;
        break;
    }
}

void PPU::writeOAMByte(uint8_t index, uint8_t data) {
    oam_[index] = data;
}

// ─── Оценка спрайтов для текущего скэнлайна ─────────────────────────────────

void PPU::evaluateSprites() {
    // Высота спрайта: 8 или 16 пикселей
    int height = ctrl_.spriteSize ? 16 : 8;
    spriteCount_ = 0;

    for (auto& s : spriteBuf_) s.active = false;

    for (int i = 0; i < 64 && spriteCount_ < 8; i++) {
        // OAM Y хранит screen_Y - 1 (аппаратное смещение NES)
        int sprY = (int)oam_[i * 4 + 0] + 1;
        int row  = scanline_ - sprY;
        if (row < 0 || row >= height) continue;

        uint8_t tile = oam_[i * 4 + 1];
        uint8_t attr = oam_[i * 4 + 2];
        uint8_t sprX = oam_[i * 4 + 3];

        bool flipV = (attr & 0x80) != 0;
        bool flipH = (attr & 0x40) != 0;

        if (flipV) row = (height - 1) - row;

        uint16_t ptBase;
        if (ctrl_.spriteSize == 0) {
            // 8×8: банк задаётся PPUCTRL.spriteTable
            ptBase = ((uint16_t)ctrl_.spriteTable << 12)
                   + ((uint16_t)tile << 4);
        } else {
            // 8×16: бит 0 тайла = банк (0x0000 или 0x1000), тайл = tile & 0xFE
            uint16_t bank = (tile & 0x01) ? 0x1000 : 0x0000;
            tile &= 0xFEu;
            if (row >= 8) { tile += 1; row -= 8; }
            ptBase = bank + ((uint16_t)tile << 4);
        }

        uint8_t lo = readVRAM(ptBase + (uint16_t)row);
        uint8_t hi = readVRAM(ptBase + (uint16_t)row + 8);

        if (flipH) {
            lo = reverseBits(lo);
            hi = reverseBits(hi);
        }

        spriteBuf_[spriteCount_].x         = sprX;
        spriteBuf_[spriteCount_].attr      = attr;
        spriteBuf_[spriteCount_].patLo     = lo;
        spriteBuf_[spriteCount_].patHi     = hi;
        spriteBuf_[spriteCount_].isSprite0 = (i == 0);
        spriteBuf_[spriteCount_].active    = true;
        spriteCount_++;
    }

    if (spriteCount_ >= 8)
        status_.spriteOverflow = 1;
}

// ─── Конвейер фонового рендеринга ────────────────────────────────────────────

void PPU::loadBgShifters() {
    bgShiftPatLo_  = (bgShiftPatLo_  & 0xFF00) | bgNextPtLo_;
    bgShiftPatHi_  = (bgShiftPatHi_  & 0xFF00) | bgNextPtHi_;
    bgShiftAttrLo_ = (bgShiftAttrLo_ & 0xFF00) | ((bgNextAtByte_ & 0x01) ? 0xFF : 0x00);
    bgShiftAttrHi_ = (bgShiftAttrHi_ & 0xFF00) | ((bgNextAtByte_ & 0x02) ? 0xFF : 0x00);
}

void PPU::shiftBgRegisters() {
    if (mask_.showBg) {
        bgShiftPatLo_  <<= 1;
        bgShiftPatHi_  <<= 1;
        bgShiftAttrLo_ <<= 1;
        bgShiftAttrHi_ <<= 1;
    }
}

void PPU::incrementScrollX() {
    if (!mask_.showBg && !mask_.showSprites) return;
    if (vram_.coarseX == 31) {
        vram_.coarseX   = 0;
        vram_.nametable ^= 0x01;
    } else {
        vram_.coarseX++;
    }
}

void PPU::incrementScrollY() {
    if (!mask_.showBg && !mask_.showSprites) return;
    if (vram_.fineY < 7) {
        vram_.fineY++;
    } else {
        vram_.fineY = 0;
        if (vram_.coarseY == 29) {
            vram_.coarseY   = 0;
            vram_.nametable ^= 0x02;
        } else if (vram_.coarseY == 31) {
            vram_.coarseY = 0;
        } else {
            vram_.coarseY++;
        }
    }
}

void PPU::transferAddressX() {
    if (!mask_.showBg && !mask_.showSprites) return;
    vram_.nametable = (vram_.nametable & 0x02) | (tram_.nametable & 0x01);
    vram_.coarseX   = tram_.coarseX;
}

void PPU::transferAddressY() {
    if (!mask_.showBg && !mask_.showSprites) return;
    vram_.fineY     = tram_.fineY;
    vram_.nametable = (vram_.nametable & 0x01) | (tram_.nametable & 0x02);
    vram_.coarseY   = tram_.coarseY;
}

// ─── Основной такт PPU ───────────────────────────────────────────────────────

void PPU::clock() {
    // Оценка спрайтов в начале каждого видимого скэнлайна
    if (scanline_ >= 0 && scanline_ < 240 && dot_ == 0)
        evaluateSprites();

    // Пресканлайн: сброс флагов
    if (scanline_ == -1 && dot_ == 1) {
        status_.vblank         = 0;
        status_.sprite0Hit     = 0;
        status_.spriteOverflow = 0;
    }

    // Видимые и пресканлайн
    bool renderingLine = (scanline_ >= -1 && scanline_ < 240);
    bool fetchCycle    = (dot_ >= 1 && dot_ <= 256) || (dot_ >= 321 && dot_ <= 336);

    if (renderingLine) {
        // Сдвигаем shift-регистры фона
        if ((dot_ >= 2 && dot_ <= 257) || (dot_ >= 322 && dot_ <= 337))
            shiftBgRegisters();

        if (fetchCycle) {
            switch ((dot_ - 1) & 0x07) {
            case 0:  // NT байт
                loadBgShifters();
                bgNextNtByte_ = readVRAM(0x2000 | (vram_.reg & 0x0FFF));
                break;
            case 2:  // AT байт
            {
                uint16_t atAddr = 0x23C0
                    | (vram_.nametable << 10)
                    | ((vram_.coarseY >> 2) << 3)
                    | (vram_.coarseX >> 2);
                bgNextAtByte_ = readVRAM(atAddr);
                if (vram_.coarseY & 0x02) bgNextAtByte_ >>= 4;
                if (vram_.coarseX & 0x02) bgNextAtByte_ >>= 2;
                bgNextAtByte_ &= 0x03;
            }
                break;
            case 4:  // PT low
                bgNextPtLo_ = readVRAM(
                    ((uint16_t)ctrl_.bgTable << 12)
                    + ((uint16_t)bgNextNtByte_ << 4)
                    + vram_.fineY);
                break;
            case 6:  // PT high
                bgNextPtHi_ = readVRAM(
                    ((uint16_t)ctrl_.bgTable << 12)
                    + ((uint16_t)bgNextNtByte_ << 4)
                    + vram_.fineY + 8);
                break;
            case 7:  // Инкремент X
                incrementScrollX();
                break;
            }
        }

        if (dot_ == 256) incrementScrollY();
        if (dot_ == 257) transferAddressX();

        // Счётчик IRQ маппера (MMC3): тикает на dot 260 каждого видимого скэнлайна
        if (dot_ == 260 && scanline_ >= 0 && scanline_ < 240)
            if (bus_) bus_->mapperScanline();

        // Копирование Y на пресканлайне (dots 280–304)
        if (scanline_ == -1 && dot_ >= 280 && dot_ <= 304)
            transferAddressY();
    }

    // VBlank
    if (scanline_ == 241 && dot_ == 1) {
        status_.vblank = 1;
        if (ctrl_.nmiEnable) nmiPending = true;
    }

    // Вывод пикселя
    if (scanline_ >= 0 && scanline_ < 240 && dot_ >= 1 && dot_ <= 256) {
        int x = dot_ - 1;  // x-координата пикселя [0..255]

        // ── Фоновый пиксель ──
        uint8_t bgPixel   = 0;
        uint8_t bgPalette = 0;

        bool showBgHere = mask_.showBg && (x >= 8 || mask_.showBgLeft);
        if (showBgHere) {
            uint16_t mux = 0x8000u >> fineX_;
            uint8_t  p0  = (bgShiftPatLo_  & mux) ? 1 : 0;
            uint8_t  p1  = (bgShiftPatHi_  & mux) ? 1 : 0;
            bgPixel = (uint8_t)((p1 << 1) | p0);

            uint8_t b0  = (bgShiftAttrLo_ & mux) ? 1 : 0;
            uint8_t b1  = (bgShiftAttrHi_ & mux) ? 1 : 0;
            bgPalette = (uint8_t)((b1 << 1) | b0);
        }

        // ── Спрайтовый пиксель ──
        uint8_t spPixel   = 0;
        uint8_t spPalette = 0;
        bool    spBehind  = false;
        bool    spIs0     = false;

        bool showSpHere = mask_.showSprites && (x >= 8 || mask_.showSpLeft);
        if (showSpHere) {
            for (int i = 0; i < spriteCount_; i++) {
                if (!spriteBuf_[i].active) continue;
                int dx = x - (int)spriteBuf_[i].x;
                if (dx < 0 || dx >= 8) continue;

                uint8_t bit = (uint8_t)(7 - dx);
                uint8_t lo  = (spriteBuf_[i].patLo >> bit) & 1u;
                uint8_t hi  = (spriteBuf_[i].patHi >> bit) & 1u;
                uint8_t pix = (uint8_t)((hi << 1) | lo);
                if (pix == 0) continue;

                spPixel   = pix;
                // Спрайтовые палитры: 4–7
                spPalette = (uint8_t)((spriteBuf_[i].attr & 0x03) + 4);
                spBehind  = (spriteBuf_[i].attr & 0x20) != 0;
                spIs0     = spriteBuf_[i].isSprite0;
                break;  // первый непрозрачный спрайт побеждает
            }
        }

        // ── Sprite 0 hit ──
        if (spIs0 && bgPixel != 0 && spPixel != 0
            && mask_.showBg && mask_.showSprites
            && x != 255) {
            status_.sprite0Hit = 1;
        }

        // ── Смешивание фон + спрайты ──
        uint32_t color;
        if (!bgPixel && !spPixel) {
            color = getColor(0, 0);
        } else if (!spPixel) {
            color = getColor(bgPalette, bgPixel);
        } else if (!bgPixel) {
            color = getColor(spPalette, spPixel);
        } else {
            // Оба непрозрачные — приоритет спрайта (bit5 атрибута: 0=спереди, 1=сзади)
            color = spBehind ? getColor(bgPalette, bgPixel)
                             : getColor(spPalette, spPixel);
        }

        screenBuffer[(size_t)scanline_ * 256 + (size_t)x] = color;
    }

    // Переход к следующему дот/скэнлайну
    dot_++;
    if (dot_ > 340) {
        dot_ = 0;
        scanline_++;
        if (scanline_ > 260) {
            scanline_     = -1;
            frameComplete = true;
        }
    }
}

// ─── Save state ──────────────────────────────────────────────────────────────

PPU::State PPU::getState() const {
    State s;
    s.vramAddr      = vram_.reg;
    s.tramAddr      = tram_.reg;
    s.fineX         = fineX_;
    s.ppuCtrl       = ctrl_.reg;
    s.ppuMask       = mask_.reg;
    s.ppuStatus     = status_.reg;
    s.oamAddr       = oamAddr_;
    s.ppuDataBuf    = ppuDataBuf_;
    s.addressLatch  = addrLatch_;
    s.bgNextNtByte  = bgNextNtByte_;
    s.bgNextAtByte  = bgNextAtByte_;
    s.bgNextPtLo    = bgNextPtLo_;
    s.bgNextPtHi    = bgNextPtHi_;
    s.bgShiftPatLo  = bgShiftPatLo_;
    s.bgShiftPatHi  = bgShiftPatHi_;
    s.bgShiftAttrLo = bgShiftAttrLo_;
    s.bgShiftAttrHi = bgShiftAttrHi_;
    s.scanline      = scanline_;
    s.dot           = dot_;
    s.mirrorMode    = static_cast<uint8_t>(mirrorMode_);
    s.screenBuffer  = screenBuffer;
    s.nameTables    = nameTables_;
    s.palette       = palette_;
    s.oam           = oam_;
    return s;
}

void PPU::setState(const State& s) {
    vram_.reg      = s.vramAddr;
    tram_.reg      = s.tramAddr;
    fineX_         = s.fineX;
    ctrl_.reg      = s.ppuCtrl;
    mask_.reg      = s.ppuMask;
    status_.reg    = s.ppuStatus;
    oamAddr_       = s.oamAddr;
    ppuDataBuf_    = s.ppuDataBuf;
    addrLatch_     = s.addressLatch;
    bgNextNtByte_  = s.bgNextNtByte;
    bgNextAtByte_  = s.bgNextAtByte;
    bgNextPtLo_    = s.bgNextPtLo;
    bgNextPtHi_    = s.bgNextPtHi;
    bgShiftPatLo_  = s.bgShiftPatLo;
    bgShiftPatHi_  = s.bgShiftPatHi;
    bgShiftAttrLo_ = s.bgShiftAttrLo;
    bgShiftAttrHi_ = s.bgShiftAttrHi;
    scanline_      = s.scanline;
    dot_           = s.dot;
    mirrorMode_    = static_cast<MirrorMode>(s.mirrorMode);
    screenBuffer   = s.screenBuffer;
    nameTables_    = s.nameTables;
    palette_       = s.palette;
    oam_           = s.oam;
    // Сброс буфера спрайтов — будет перезаполнен при следующем dot_==0
    for (auto& sp : spriteBuf_) sp.active = false;
    spriteCount_ = 0;
}
