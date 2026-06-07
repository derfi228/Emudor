// snes_ppu.cpp — SNES PPU: рендер BG0-3, OBJ, Mode 0/1/7, цветовая математика
#include "snes_ppu.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

// ─── Сброс ────────────────────────────────────────────────────────────────────
void SnesPPU::reset()
{
    scanline_     = 0;
    dot_          = 0;
    frameComplete = false;
    nmiPending    = false;
    vramAddr_     = 0;
    cgramAddr_    = 0;
    cgramHalf_    = false;
    oamAddr_      = 0;
    oamFirstWrite_= false;
    oamLatch_     = 0;
    scrollLatch_  = 0;
    std::memset(regs_, 0, sizeof(regs_));
    std::memset(bgHscroll_, 0, sizeof(bgHscroll_));
    std::memset(bgVscroll_, 0, sizeof(bgVscroll_));
    vram_.fill(0);
    cgram_.fill(0);
    oam_.fill(0);
    fb_.fill(0xFF000000u);
    m7HOFS_ = m7VOFS_ = 0;
    coldata_ = 0;
}

// ─── Такт PPU (один дот) ──────────────────────────────────────────────────────
// NTSC: 341 дота × 262 сканлайна; активных строк = 1–224 (или 1–239 overscan)
void SnesPPU::clock()
{
    ++dot_;
    if (dot_ >= 341) {
        dot_ = 0;

        // Рендерим строку (сканлайн 1 → строка 0 на экране, 224 активных строки)
        if (scanline_ >= 1 && scanline_ <= (uint16_t)HEIGHT) {
            renderScanline((int)scanline_ - 1);
        }
        ++scanline_;

        if (scanline_ == 225) {
            // VBlank начинается
            nmiPending    = true;
            frameComplete = true;
            regs_[0x3F] |= 0x80;  // STAT78: VBlank флаг
        }
        if (scanline_ >= 262) {
            scanline_ = 0;
            frameComplete = false;
            regs_[0x3F] &= (uint8_t)~0x80;
        }
    }
}

// ─── Запись регистров ─────────────────────────────────────────────────────────
void SnesPPU::writeReg(uint16_t addr, uint8_t data)
{
    if (addr < 0x2100 || addr > 0x213F) return;
    uint8_t reg = (uint8_t)(addr - 0x2100);
    regs_[reg] = data;

    switch (addr) {
    // ── VRAM ──────────────────────────────────────────────────────────────────
    case 0x2115: // VMAIN
        vramHiLatch_ = (data & 0x80) != 0;
        // биты 1:0 — шаг инкремента: 00=+1, 01=+32, 10=+128, 11=+128
        {
            static const uint8_t kSteps[4] = {1, 32, 128, 128};
            vramStep_ = kSteps[data & 3];
        }
        // биты 3:2 — режим ремаппинга адреса
        vramRemap_ = (uint8_t)((data >> 2) & 3);
        break;
    case 0x2116: // VMADDL
        vramAddr_ = (uint16_t)((vramAddr_ & 0xFF00) | data);
        // Prefetch с эффективным (ремаппинг!) адресом
        vramPrefetchLo_ = (uint8_t)(vram_[vramEffAddr()]);
        vramPrefetchHi_ = (uint8_t)(vram_[vramEffAddr()] >> 8);
        break;
    case 0x2117: // VMADDH
        vramAddr_ = (uint16_t)((vramAddr_ & 0x00FF) | ((uint16_t)data << 8));
        vramPrefetchLo_ = (uint8_t)(vram_[vramEffAddr()]);
        vramPrefetchHi_ = (uint8_t)(vram_[vramEffAddr()] >> 8);
        break;
    case 0x2118: { // VMDATAL
        uint16_t eff = vramEffAddr();
        vram_[eff] = (uint16_t)((vram_[eff] & 0xFF00) | data);
        if (!vramHiLatch_) vramAddr_ = (uint16_t)(vramAddr_ + vramStep_);
        break;
    }
    case 0x2119: { // VMDATAH
        uint16_t eff = vramEffAddr();
        vram_[eff] = (uint16_t)((vram_[eff] & 0x00FF) | ((uint16_t)data << 8));
        if (vramHiLatch_) vramAddr_ = (uint16_t)(vramAddr_ + vramStep_);
        break;
    }

    // ── CGRAM ─────────────────────────────────────────────────────────────────
    case 0x2121: // CGADD
        cgramAddr_ = (uint16_t)(data * 2);  // half-words (hi/lo)
        cgramHalf_ = false;
        break;
    case 0x2122: { // CGDATA — BGR555, 16-bit за 2 записи
        // Формат слова: bit15=0, bits[14:10]=B, bits[9:5]=G, bits[4:0]=R
        // Первый байт → bits[7:0] (R[4:0] + G[2:0])
        // Второй байт → bits[14:8] (G[4:3] + B[4:0]), бит7 игнорируется
        uint8_t idx = (uint8_t)(cgramAddr_ / 2);
        if (!cgramHalf_) {
            cgram_[idx] = (uint16_t)((cgram_[idx] & 0xFF00) | data);
        } else {
            cgram_[idx] = (uint16_t)((cgram_[idx] & 0x00FF) | ((uint16_t)(data & 0x7F) << 8));
            cgramAddr_ += 2;
        }
        cgramHalf_ = !cgramHalf_;
        break;
    }

    // ── OAM ───────────────────────────────────────────────────────────────────
    case 0x2102: // OAMADDL
        oamAddr_ = (uint16_t)((oamAddr_ & 0x0200) | ((uint16_t)data << 1));
        oamFirstWrite_ = false;
        break;
    case 0x2103: // OAMADDH
        oamAddr_ = (uint16_t)((oamAddr_ & 0x01FE) | ((data & 1) << 9));
        oamFirstWrite_ = false;
        break;
    case 0x2104: // OAMDATA
        if (oamAddr_ < 0x200) {
            // Основная часть OAM: пишем попарно
            if ((oamAddr_ & 1) == 0) {
                oamLatch_ = data;
            } else {
                oam_[(uint16_t)(oamAddr_ - 1)] = oamLatch_;
                oam_[oamAddr_]                 = data;
            }
        } else {
            // Расширение OAM (32 байта)
            oam_[oamAddr_ & 0x21F] = data;
        }
        oamAddr_ = (uint16_t)((oamAddr_ + 1) & 0x3FF);
        break;

    // ── Scroll ────────────────────────────────────────────────────────────────
    // HOFS/VOFS: каждая запись — полный 10-битный update.
    // Формула (bsnes): HOFS = (data<<8) | (scrollLatch_ & ~7) | ((HOFS>>8) & 7)
    // Игры пишут low-byte первым (он становится latch), затем high-byte.
    // После каждой записи scrollLatch_ = data.
    case 0x210D: // BG1HOFS + M7HOFS
        bgHscroll_[0] = (uint16_t)((data << 8) | (scrollLatch_ & ~7) | ((bgHscroll_[0] >> 8) & 7));
        scrollLatch_ = data;
        // M7HOFS: 13-битный знаковый, write low-then-high через m7Latch_
        m7HOFS_ = (int16_t)(((uint16_t)data << 8) | m7Latch_);
        m7Latch_ = data;
        break;
    case 0x210E: // BG1VOFS + M7VOFS
        bgVscroll_[0] = (uint16_t)((data << 8) | scrollLatch_);
        scrollLatch_ = data;
        m7VOFS_ = (int16_t)(((uint16_t)data << 8) | m7Latch_);
        m7Latch_ = data;
        break;
    case 0x210F: // BG2HOFS
        bgHscroll_[1] = (uint16_t)((data << 8) | (scrollLatch_ & ~7) | ((bgHscroll_[1] >> 8) & 7));
        scrollLatch_ = data;
        break;
    case 0x2110: // BG2VOFS
        bgVscroll_[1] = (uint16_t)((data << 8) | scrollLatch_);
        scrollLatch_ = data;
        break;
    case 0x2111: // BG3HOFS
        bgHscroll_[2] = (uint16_t)((data << 8) | (scrollLatch_ & ~7) | ((bgHscroll_[2] >> 8) & 7));
        scrollLatch_ = data;
        break;
    case 0x2112: // BG3VOFS
        bgVscroll_[2] = (uint16_t)((data << 8) | scrollLatch_);
        scrollLatch_ = data;
        break;
    case 0x2113: // BG4HOFS
        bgHscroll_[3] = (uint16_t)((data << 8) | (scrollLatch_ & ~7) | ((bgHscroll_[3] >> 8) & 7));
        scrollLatch_ = data;
        break;
    case 0x2114: // BG4VOFS
        bgVscroll_[3] = (uint16_t)((data << 8) | scrollLatch_);
        scrollLatch_ = data;
        break;

    // ── Mode 7 матрица ────────────────────────────────────────────────────────
    case 0x211B: // M7A
        m7A_ = (int16_t)((data << 8) | m7Latch_);
        m7Latch_ = data;
        break;
    case 0x211C: // M7B
        m7B_ = (int16_t)((data << 8) | m7Latch_);
        m7Latch_ = data;
        break;
    case 0x211D: // M7C
        m7C_ = (int16_t)((data << 8) | m7Latch_);
        m7Latch_ = data;
        break;
    case 0x211E: // M7D
        m7D_ = (int16_t)((data << 8) | m7Latch_);
        m7Latch_ = data;
        break;
    case 0x211F: // M7X
        m7X_ = (int16_t)((data << 8) | m7Latch_);
        m7Latch_ = data;
        break;
    case 0x2120: // M7Y
        m7Y_ = (int16_t)((data << 8) | m7Latch_);
        m7Latch_ = data;
        break;

    // ── COLDATA $2132 — фиксированный цвет для color math ─────────────────────
    // Биты 7=B, 6=G, 5=R выбирают канал; биты 4:0 — 5-битное значение
    case 0x2132: {
        uint16_t v = (uint16_t)(data & 0x1F);
        if (data & 0x20) coldata_ = (uint16_t)((coldata_ & ~0x001Fu) | v);
        if (data & 0x40) coldata_ = (uint16_t)((coldata_ & ~0x03E0u) | (v << 5));
        if (data & 0x80) coldata_ = (uint16_t)((coldata_ & ~0x7C00u) | (v << 10));
        break;
    }

    default:
        break;
    }
}

// ─── Чтение регистров ─────────────────────────────────────────────────────────
uint8_t SnesPPU::readReg(uint16_t addr)
{
    switch (addr) {
    case 0x2138: { // OAMDATAREAD
        // OAM = 544 байта: low table 0-511, high table 512-543 ($200-$21F).
        // Адрес может быть до 0x3FF — приводим к валидному диапазону.
        uint16_t oa = oamAddr_;
        uint16_t idx = (oa < 0x200) ? oa : (uint16_t)(0x200 + (oa & 0x1F));
        uint8_t v = oam_[idx];
        oamAddr_ = (uint16_t)((oamAddr_ + 1) & 0x3FF);
        return v;
    }
    case 0x2139: { // VMDATALREAD
        uint8_t v = vramPrefetchLo_;
        if (!vramHiLatch_) {
            vramAddr_ = (uint16_t)(vramAddr_ + vramStep_);
            uint16_t eff = vramEffAddr();
            vramPrefetchLo_ = (uint8_t)(vram_[eff]);
            vramPrefetchHi_ = (uint8_t)(vram_[eff] >> 8);
        }
        return v;
    }
    case 0x213A: { // VMDATAHREAD
        uint8_t v = vramPrefetchHi_;
        if (vramHiLatch_) {
            vramAddr_ = (uint16_t)(vramAddr_ + vramStep_);
            uint16_t eff = vramEffAddr();
            vramPrefetchLo_ = (uint8_t)(vram_[eff]);
            vramPrefetchHi_ = (uint8_t)(vram_[eff] >> 8);
        }
        return v;
    }
    case 0x213B: { // CGDATAREAD
        // Первый байт → bits[7:0], второй байт → bits[14:8] (bit15 возвращаем 0)
        uint8_t idx = (uint8_t)(cgramAddr_ / 2);
        uint8_t v;
        if (!cgramHalf_) {
            v = (uint8_t)(cgram_[idx] & 0xFF);
        } else {
            v = (uint8_t)((cgram_[idx] >> 8) & 0x7F);
            cgramAddr_ += 2;
        }
        cgramHalf_ = !cgramHalf_;
        return v;
    }
    case 0x2137: {                         // SLHV — программная защёлка H/V счётчиков
        hvToggleH_ = false;
        hvToggleV_ = false;
        return 0;                          // открытая шина
    }
    case 0x213C: {                         // OPHCT — горизонтальный счётчик луча (9 бит)
        uint16_t h = dot_;                 // 0..339
        uint8_t v = hvToggleH_ ? (uint8_t)((h >> 8) & 1) : (uint8_t)(h & 0xFF);
        hvToggleH_ = !hvToggleH_;
        return v;
    }
    case 0x213D: {                         // OPVCT — вертикальный счётчик луча (9 бит)
        uint16_t vc = scanline_;           // 0..261 (текущая строка луча)
        uint8_t v = hvToggleV_ ? (uint8_t)((vc >> 8) & 1) : (uint8_t)(vc & 0xFF);
        hvToggleV_ = !hvToggleV_;
        return v;
    }
    case 0x213E: return 0x01;              // STAT77: version 1
    case 0x213F: {                         // STAT78: PPU2 version (+ сброс H/V toggle)
        hvToggleH_ = false;
        hvToggleV_ = false;
        uint8_t v = (uint8_t)(regs_[0x3F] | 0x02);   // version 2, прочее — 0
        return v;
    }
    default:
        return 0;
    }
}

// ─── Ремаппинг VRAM-адреса (биты 3:2 регистра $2115 VMAIN) ───────────────────
// SNES PPU перед каждым доступом к VRAM пропускает адрес через перестановщик
// битов; сам vramAddr_ при этом не меняется (инкремент идёт по исходному).
// Формула: верхняя часть остаётся, низкая (5/6/7 бит) ротируется так:
//   8-bit  : aaaaaaaa BBB xxxxx → aaaaaaaa xxxxx BBB
//   9-bit  : aaaaaaa BBB xxxxxx → aaaaaaa xxxxxx BBB
//   10-bit : aaaaaa BBB xxxxxxx → aaaaaa xxxxxxx BBB
uint16_t SnesPPU::vramEffAddr() const
{
    uint16_t a = (uint16_t)(vramAddr_ & 0x7FFF);
    switch (vramRemap_) {
    case 1: // 8-bit
        return (uint16_t)((a & 0xFF00) | ((a << 3) & 0x00F8) | ((a >> 5) & 0x0007));
    case 2: // 9-bit
        return (uint16_t)((a & 0xFE00) | ((a << 3) & 0x01F8) | ((a >> 6) & 0x0007));
    case 3: // 10-bit
        return (uint16_t)((a & 0xFC00) | ((a << 3) & 0x03F8) | ((a >> 7) & 0x0007));
    default:
        return a;
    }
}

// ─── Конвертация цвета BGR555 → RGBA8888 ─────────────────────────────────────
uint32_t SnesPPU::cgToRGBA(uint16_t bgr555, uint8_t brightness)
{
    // BGR555: [14:10]=B, [9:5]=G, [4:0]=R
    uint8_t r5 = (uint8_t)( bgr555        & 0x1F);
    uint8_t g5 = (uint8_t)((bgr555 >>  5) & 0x1F);
    uint8_t b5 = (uint8_t)((bgr555 >> 10) & 0x1F);

    // 5-бит → 8-бит + применяем яркость (0–15 → множитель 0–1)
    auto scale = [brightness](uint8_t c5) -> uint8_t {
        uint16_t c8 = (uint16_t)((c5 << 3) | (c5 >> 2));
        return (uint8_t)(c8 * (brightness + 1) / 16);
    };
    uint8_t r = scale(r5);
    uint8_t g = scale(g5);
    uint8_t b = scale(b5);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// ─── Битов на пиксель для BG-слоя ────────────────────────────────────────────
int SnesPPU::bgBpp(int bgIdx) const
{
    uint8_t mode = regs_[0x05] & 0x07;  // биты [2:0] = режим
    switch (mode) {
    case 0: return 2;  // все BG = 2bpp
    case 1:
        if (bgIdx < 2) return 4;
        return 2;
    case 2: return 4;  // BG1=4bpp, BG2=4bpp (affine BG2)
    case 3:
        if (bgIdx == 0) return 8;
        return 4;
    case 4:
        if (bgIdx == 0) return 8;
        return 2;
    case 5:
        if (bgIdx == 0) return 4;
        return 2;
    case 7: return 8;  // Mode 7
    default: return 2;
    }
}

// ─── Смещение палитры для BG-слоя ────────────────────────────────────────────
int SnesPPU::bgPaletteOffset(int bgIdx, uint8_t palNum) const
{
    uint8_t mode = regs_[0x05] & 0x07;
    int bpp = bgBpp(bgIdx);
    int colors = 1 << bpp;  // 4/16/256 цветов
    // В Mode 0 у каждого BG-слоя своя область палитры (CGRAM):
    // BG1: 0–15, BG2: 16–31, BG3: 32–47, BG4: 48–63
    if (mode == 0) return bgIdx * 0x20 + palNum * colors;
    return palNum * colors;
}

// ─── Получить пиксель фона BG#bgIdx ──────────────────────────────────────────
SnesPPU::BgPixel SnesPPU::getBGPixel(int bgIdx, int screenX, int screenY) const
{
    BgPixel result{0, false};

    int bpp = bgBpp(bgIdx);
    if (bpp == 0) return result;

    // Tilemap base: reg $2107–$210A
    uint8_t scReg = regs_[0x07 + bgIdx];
    uint16_t tmBase = (uint16_t)((scReg >> 2) & 0x3F) << 10;  // слова
    bool sc2x = (scReg & 1) != 0;   // 2× по X
    bool sc2y = (scReg & 2) != 0;   // 2× по Y

    // CHR base: reg $210B–$210C
    uint16_t chrBase;
    if (bgIdx < 2) {
        int shift = (bgIdx == 0) ? 0 : 4;
        chrBase = (uint16_t)(((regs_[0x0B] >> shift) & 0x0F) << 12);  // слова
    } else {
        int shift = (bgIdx == 2) ? 0 : 4;
        chrBase = (uint16_t)(((regs_[0x0C] >> shift) & 0x0F) << 12);
    }

    // Размер тайла: 8×8 или 16×16 (бит в $2105 [7:4] для BG4–BG1)
    bool bigTile = (regs_[0x05] >> (4 + bgIdx)) & 1;
    int tileSize = bigTile ? 16 : 8;

    // Прокрутка
    int effX = (screenX + (int)bgHscroll_[bgIdx]) & 0x3FF;
    int effY = (screenY + (int)bgVscroll_[bgIdx]) & 0x3FF;

    // Тайл-координаты
    int tileCol = effX / tileSize;
    int tileRow = effY / tileSize;
    int pixInCol = effX % tileSize;
    int pixInRow = effY % tileSize;

    // Тайловая карта 32×32 тайла; при sc2x/sc2y используем 64×32 / 32×64 / 64×64
    int mapCols = sc2x ? 64 : 32;
    int mapRows = sc2y ? 64 : 32;
    tileCol &= (mapCols - 1);
    tileRow &= (mapRows - 1);

    // Вычисляем адрес тайловой карты (в словах VRAM)
    // При sc2x или sc2y: 2-й экран смещён на 0x400 или 0x800 слов
    uint16_t mapOffset = 0;
    if (sc2x && tileCol >= 32) { mapOffset = 0x400; tileCol -= 32; }
    if (sc2y && tileRow >= 32) { mapOffset = (uint16_t)(mapOffset + 0x800); tileRow -= 32; }

    uint16_t mapAddr = (uint16_t)((tmBase + mapOffset + tileRow * 32 + tileCol) & 0x7FFF);
    uint16_t entry   = vram_[mapAddr];

    // Разбираем запись тайловой карты: vhopppcc cccccccc
    // bit15=vFlip, bit14=hFlip, bit13=priority, bits12:10=palette, bits9:0=tile
    uint16_t tileNum  = entry & 0x03FF;
    bool     hFlip    = (entry >> 14) & 1;
    bool     vFlip    = (entry >> 15) & 1;
    uint8_t  palNum   = (uint8_t)((entry >> 10) & 0x07);
    bool     priority = (entry >> 13) & 1;

    // При 16×16 тайлах выбираем суб-тайл 8×8: +1 за правый столбец, +16 за нижний ряд.
    if (bigTile) {
        int subCol = pixInCol / 8;
        int subRow = pixInRow / 8;
        if (hFlip) subCol = 1 - subCol;
        if (vFlip) subRow = 1 - subRow;
        tileNum = (uint16_t)((tileNum + subCol + subRow * 16) & 0x3FF);
        pixInCol %= 8;
        pixInRow %= 8;
    }

    // Применяем flip
    int px = hFlip ? (7 - pixInCol) : pixInCol;
    int py = vFlip ? (7 - pixInRow) : pixInRow;

    // Получаем цвет из тайла
    int wordsPerTile = (bpp == 2) ? 8 : (bpp == 4) ? 16 : 32;
    uint16_t tileAddr = (uint16_t)((chrBase + tileNum * wordsPerTile) & 0x7FFF);

    uint8_t colorIdx = 0;
    for (int plane = 0; plane < bpp; plane += 2) {
        // Каждая пара плоскостей — 8 слов подряд
        int pairOffset = plane / 2;
        uint16_t wordAddr = (uint16_t)((tileAddr + pairOffset * 8 + py) & 0x7FFF);
        uint16_t w = vram_[wordAddr];
        uint8_t lo = (uint8_t)(w & 0xFF);
        uint8_t hi = (uint8_t)(w >> 8);
        // Бит 7 = левый пиксель
        uint8_t bit0 = (lo >> (7 - px)) & 1;
        uint8_t bit1 = (hi >> (7 - px)) & 1;
        colorIdx = (uint8_t)(colorIdx | (bit0 << plane) | (bit1 << (plane + 1)));
    }

    if (colorIdx == 0) return result;  // прозрачно

    int palOff = bgPaletteOffset(bgIdx, palNum);
    result.color    = (uint8_t)(palOff + colorIdx);
    result.priority = priority;
    return result;
}

// ─── Mode 7 ───────────────────────────────────────────────────────────────────
// Каноническая формула SNES PPU:
//   ox = (A*(X+HOFS-Cx) + B*(Y+VOFS-Cy)) >> 8 + Cx
//   oy = (C*(X+HOFS-Cx) + D*(Y+VOFS-Cy)) >> 8 + Cy
// где Cx=M7X, Cy=M7Y, A/B/C/D=матрица. Все смещения — 13-битные знаковые.
SnesPPU::BgPixel SnesPPU::getMode7Pixel(int screenX, int screenY) const
{
    auto sext13 = [](int16_t v) -> int {
        return (v & 0x1000) ? ((int)v | ~0x1FFF) : ((int)v & 0x1FFF);
    };
    int hOff = sext13(m7HOFS_);
    int vOff = sext13(m7VOFS_);
    int cx   = sext13(m7X_);
    int cy   = sext13(m7Y_);

    int dx = (screenX + hOff - cx);
    int dy = (screenY + vOff - cy);

    int ox = ((m7A_ * dx) + (m7B_ * dy)) >> 8;
    int oy = ((m7C_ * dx) + (m7D_ * dy)) >> 8;
    ox += cx;
    oy += cy;

    // $211A M7SEL: bit7=vFlip, bit6=hFlip, bits[1:0]=overflow режим
    uint8_t m7sel = regs_[0x1A];
    if (m7sel & 0x40) ox = ~ox;
    if (m7sel & 0x80) oy = ~oy;

    bool outOfMap = ((unsigned)ox >= 1024u) || ((unsigned)oy >= 1024u);
    uint8_t overflowMode = (uint8_t)(m7sel & 3);
    if (outOfMap) {
        if (overflowMode == 3) {
            BgPixel r{0, false};  // прозрачно
            return r;
        }
        if (overflowMode == 2) {
            // Tile 0: показываем character 0 для overflow-региона
            ox &= 7;
            oy &= 7;
        } else {
            // wrap (modes 0/1)
            ox &= 0x3FF;
            oy &= 0x3FF;
        }
    }

    // Тайловая карта 128×128 (одно слово на тайл, tile# в low-байте)
    uint8_t  tileX   = (uint8_t)((ox >> 3) & 0x7F);
    uint8_t  tileY   = (uint8_t)((oy >> 3) & 0x7F);
    uint16_t mapAddr = (uint16_t)(tileY * 128 + tileX);
    uint8_t  tileNum = (overflowMode == 2 && outOfMap) ? 0 : (uint8_t)(vram_[mapAddr] & 0xFF);

    // Данные тайла: пиксель в hi-байте, 8×8 = 64 слова на тайл
    int px = ox & 7;
    int py = oy & 7;
    uint16_t dataAddr = (uint16_t)(tileNum * 64 + py * 8 + px);
    uint8_t  word     = (uint8_t)(vram_[dataAddr & 0x7FFF] >> 8);

    if (word == 0) {
        BgPixel r{0, false};
        return r;
    }
    BgPixel r;
    r.color    = word;
    r.priority = false;
    return r;
}

// ─── Спрайты ──────────────────────────────────────────────────────────────────
SnesPPU::ObjPixel SnesPPU::getObjPixel(int screenX, int screenY) const
{
    ObjPixel result{0, 4};  // priority 4 = нет спрайта

    // OBSEL: biт[2:0] = base tile addr (× 0x2000 слов), биты[4:3] = gap
    uint16_t objBase = (uint16_t)((regs_[0x01] & 0x07) << 13);
    // Размеры спрайтов: regs_[0x01] bits [7:5] = S (small/large size pair)
    // Упрощённо: всегда 8×8 (small) и 16×16 (large)
    static const int kSmall[8] = {8,8,8,16,16,32,16,16};
    static const int kLarge[8] = {16,32,64,32,64,64,32,32};
    int sizeKey = (regs_[0x01] >> 5) & 7;
    int smW = kSmall[sizeKey], smH = smW;
    int lgW = kLarge[sizeKey], lgH = lgW;

    // Проходим 128 спрайтов, находим первый совпадающий
    for (int s = 0; s < 128; ++s) {
        // Основные 4 байта
        uint16_t base = (uint16_t)(s * 4);
        int  xLo  = oam_[base + 0];
        int  y    = oam_[base + 1];
        uint8_t tile  = oam_[base + 2];
        uint8_t attr  = oam_[base + 3];

        // Расширение OAM (32 байта по 4 спрайта)
        uint8_t ext = oam_[0x200 + (s >> 2)];
        int ext2bit = (s & 3) * 2;
        int  xHi   = (ext >> ext2bit) & 1;
        bool large = ((ext >> (ext2bit + 1)) & 1) != 0;

        int objX = xLo | (xHi ? 0x100 : 0);
        if (objX >= 256) objX -= 512;  // знаковая X-координата

        int sprW = large ? lgW : smW;
        int sprH = large ? lgH : smH;

        // Проверяем попадание по Y (с учётом wrap-around в 256 строк)
        int dy = (screenY - y) & 0xFF;
        if (dy >= sprH) continue;

        // Проверяем попадание по X
        int dx = screenX - objX;
        if (dx < 0 || dx >= sprW) continue;

        // Атрибуты спрайта
        bool hFlip = (attr >> 6) & 1;
        bool vFlip = (attr >> 7) & 1;
        uint8_t pal    = (attr >> 1) & 7;
        uint8_t pri    = (attr >> 4) & 3;
        bool    nameHi = (attr & 1) != 0;

        // Sub-tile в 8×8 единицах
        int subX = dx / 8, subY = dy / 8;
        if (hFlip) subX = (sprW / 8 - 1) - subX;
        if (vFlip) subY = (sprH / 8 - 1) - subY;

        int pxInTile = hFlip ? (7 - dx % 8) : (dx % 8);
        int pyInTile = vFlip ? (7 - dy % 8) : (dy % 8);

        uint16_t tileNum = (uint16_t)(tile + subX + subY * 16);
        tileNum &= 0xFF;
        if (nameHi) tileNum = (uint16_t)(tileNum + 256);

        // 4bpp спрайты
        uint16_t tileAddr = (uint16_t)((objBase + tileNum * 16) & 0x7FFF);
        uint8_t  colorIdx = 0;
        for (int plane = 0; plane < 4; plane += 2) {
            int pairOffset = plane / 2;
            uint16_t wordAddr = (uint16_t)((tileAddr + pairOffset * 8 + pyInTile) & 0x7FFF);
            uint16_t w = vram_[wordAddr];
            uint8_t  lo = (uint8_t)(w);
            uint8_t  hi = (uint8_t)(w >> 8);
            uint8_t  b0 = (lo >> (7 - pxInTile)) & 1;
            uint8_t  b1 = (hi >> (7 - pxInTile)) & 1;
            colorIdx = (uint8_t)(colorIdx | (b0 << plane) | (b1 << (plane + 1)));
        }

        if (colorIdx == 0) continue;  // прозрачно

        // Цвет спрайта: CGRAM начиная с $80 + pal*16
        uint8_t cgramIdx = (uint8_t)(0x80 + pal * 16 + colorIdx);
        result.color    = cgram_[cgramIdx];
        result.priority = pri;
        return result;
    }
    return result;
}

// ─── Рендеринг одной строки ───────────────────────────────────────────────────
void SnesPPU::renderScanline(int y)
{
    if (y < 0 || y >= HEIGHT) return;

    uint8_t brightness = this->brightness();
    bool    enabled    = displayEnabled();

    if (!enabled || brightness == 0) {
        uint32_t* row = &fb_[(size_t)(y * WIDTH)];
        for (int x = 0; x < WIDTH; ++x) row[x] = 0xFF000000u;
        return;
    }

    uint8_t mode   = regs_[0x05] & 0x07;
    uint8_t mainEn = regs_[0x2C];  // TM $212C: [4]=OBJ,[3]=BG4,[2]=BG3,[1]=BG2,[0]=BG1

    // ── Предварительный кэш спрайтов для текущего сканлайна ──────────────────
    // Вместо итерации 128 спрайтов на каждый пиксель — один проход 128 спрайтов,
    // кэшируем до 32 (лимит SNES) подходящих по Y. В пиксельном цикле только ≤32.
    struct SprCache {
        int16_t  x;       // знаковая X-координата
        uint8_t  dy;      // смещение строки внутри спрайта
        uint8_t  sprW;    // ширина спрайта в пикселях
        uint8_t  tile;
        uint8_t  attr;
        uint16_t tileAddr; // начало tile-данных в VRAM (слова)
    };
    SprCache sprCache[32];
    int nCached = 0;

    if (mainEn & 0x10) {
        uint16_t objBase = (uint16_t)((regs_[0x01] & 0x07) << 13);
        static const int kSmall[8] = {8,8,8,16,16,32,16,16};
        static const int kLarge[8] = {16,32,64,32,64,64,32,32};
        int sizeKey = (regs_[0x01] >> 5) & 7;
        int smW = kSmall[sizeKey], smH = smW;
        int lgW = kLarge[sizeKey], lgH = lgW;

        for (int s = 0; s < 128 && nCached < 32; ++s) {
            int sprY = oam_[(uint16_t)(s * 4 + 1)];
            uint8_t ext    = oam_[(uint16_t)(0x200 + (s >> 2))];
            int     ext2   = (s & 3) * 2;
            bool    large  = ((ext >> (ext2 + 1)) & 1) != 0;
            int     sprH   = large ? lgH : smH;
            int     dy     = (y - sprY) & 0xFF;
            if (dy >= sprH) continue;       // не попадает по Y

            int xLo  = oam_[(uint16_t)(s * 4 + 0)];
            int xHi  = (ext >> ext2) & 1;
            int objX = xLo | (xHi ? 0x100 : 0);
            if (objX >= 256) objX -= 512;   // знаковая X

            int sprW = large ? lgW : smW;
            if (objX + sprW <= 0 || objX >= WIDTH) continue;  // вне экрана

            uint8_t tile = oam_[(uint16_t)(s * 4 + 2)];
            uint8_t attr = oam_[(uint16_t)(s * 4 + 3)];
            bool nameHi  = (attr & 1) != 0;

            // Предвычисляем Y-субтайл (учёт vFlip)
            bool vFlip  = (attr >> 7) & 1;
            int  subY   = vFlip ? ((sprH / 8 - 1) - (dy / 8)) : (dy / 8);
            int  pyInT  = vFlip ? (7 - dy % 8) : (dy % 8);

            // tileNum для первого x-субтайла (subX добавляется позже)
            // Запоминаем только базу без subX, чтобы не дублировать
            uint16_t tileBase = (uint16_t)(tile + subY * 16);
            if (nameHi) tileBase = (uint16_t)(tileBase + 256);
            tileBase &= 0x1FF;
            uint16_t tileAddr = (uint16_t)((objBase + (uint32_t)tileBase * 16 + (uint32_t)pyInT) & 0x7FFF);

            sprCache[nCached] = {
                (int16_t)objX,
                (uint8_t)dy,
                (uint8_t)sprW,
                tile, attr,
                tileAddr   // tileAddr включает Y-субтайл и pyInTile
            };
            ++nCached;
        }
    }

    // ── Пиксельный цикл ───────────────────────────────────────────────────────
    uint32_t* row = &fb_[(size_t)(y * WIDTH)];

    for (int x = 0; x < WIDTH; ++x) {
        uint32_t finalColor = cgToRGBA(cgram_[0], brightness);  // backdrop

        if (mode == 7) {
            if (mainEn & 1) {
                BgPixel bp = getMode7Pixel(x, y);
                if (bp.color != 0)
                    finalColor = cgToRGBA(cgram_[bp.color], brightness);
            }
        } else {
            // Layer.src: 0=BG1, 1=BG2, 2=BG3, 3=BG4, 4=OBJ — для color math
            struct Layer { uint16_t cgColor; int prio; uint8_t src; bool objHiPal; };
            Layer mainLs[16]; int mainN = 0;
            Layer subLs[16];  int subN  = 0;

            uint8_t subEnReg = regs_[0x2D];   // TS

            // ── Сбор слоёв BG1..BG4 для main и sub-screen ─────────────────────
            auto pushBG = [&](int bgIdx, int loP, int hiP) {
                BgPixel bp = getBGPixel(bgIdx, x, y);
                if (bp.color == 0) return;
                int p = bp.priority ? hiP : loP;
                if (mainEn   & (1 << bgIdx))
                    mainLs[mainN++] = { cgram_[bp.color], p, (uint8_t)bgIdx, false };
                if (subEnReg & (1 << bgIdx))
                    subLs[subN++]   = { cgram_[bp.color], p, (uint8_t)bgIdx, false };
            };
            pushBG(0, 3, 9);   // BG1
            pushBG(1, 5, 7);   // BG2
            {
                BgPixel bp = getBGPixel(2, x, y);
                if (bp.color != 0) {
                    bool bg3Hi = (regs_[0x05] & 0x08) != 0;
                    int p = bp.priority ? (bg3Hi ? 11 : 2) : 1;
                    if (mainEn   & 4) mainLs[mainN++] = { cgram_[bp.color], p, 2, false };
                    if (subEnReg & 4) subLs[subN++]   = { cgram_[bp.color], p, 2, false };
                }
            }
            pushBG(3, 0, 1);   // BG4

            // ── OBJ ────────────────────────────────────────────────────────────
            // Спрайты с палитрой 4..7 участвуют в color math (objHiPal=true).
            if (nCached > 0) {
                static const int kObjPrio[4] = { 4, 6, 8, 10 };
                uint16_t objBase = (uint16_t)((regs_[0x01] & 0x07) << 13);
                for (int si = 0; si < nCached; ++si) {
                    int dx = x - (int)sprCache[si].x;
                    if (dx < 0 || dx >= (int)sprCache[si].sprW) continue;

                    uint8_t attr   = sprCache[si].attr;
                    bool    hFlip  = (attr >> 6) & 1;
                    uint8_t pal    = (attr >> 1) & 7;
                    uint8_t pri    = (attr >> 4) & 3;
                    bool    nameHi = (attr & 1) != 0;
                    uint8_t tile   = sprCache[si].tile;
                    uint8_t dy     = sprCache[si].dy;
                    int     sprW   = sprCache[si].sprW;
                    bool    vFlip  = (attr >> 7) & 1;
                    int     subX   = hFlip ? ((sprW / 8 - 1) - (dx / 8)) : (dx / 8);
                    int     subY   = vFlip ? ((sprW / 8 - 1) - (dy / 8)) : (dy / 8);
                    int     pxInT  = hFlip ? (7 - dx % 8) : (dx % 8);
                    int     pyInT  = vFlip ? (7 - dy % 8) : (dy % 8);
                    uint16_t tileNum = (uint16_t)(tile + subX + subY * 16);
                    tileNum &= 0xFF;
                    if (nameHi) tileNum = (uint16_t)(tileNum + 256);
                    uint16_t tileAddr = (uint16_t)((objBase + (uint32_t)tileNum * 16 + (uint32_t)pyInT) & 0x7FFF);
                    uint8_t colorIdx = 0;
                    for (int plane = 0; plane < 4; plane += 2) {
                        uint16_t wAddr = (uint16_t)((tileAddr + (plane/2) * 8) & 0x7FFF);
                        uint16_t w  = vram_[wAddr];
                        uint8_t  b0 = ((uint8_t)(w)       >> (7 - pxInT)) & 1;
                        uint8_t  b1 = ((uint8_t)(w >> 8)  >> (7 - pxInT)) & 1;
                        colorIdx = (uint8_t)(colorIdx | (b0 << plane) | (b1 << (plane + 1)));
                    }
                    if (colorIdx == 0) continue;
                    uint8_t cgramIdx = (uint8_t)(0x80 + pal * 16 + colorIdx);
                    Layer L = { cgram_[cgramIdx], kObjPrio[pri & 3], 4, (pal >= 4) };
                    if (mainEn   & 0x10) mainLs[mainN++] = L;
                    if (subEnReg & 0x10) subLs[subN++]   = L;
                    break;
                }
            }

            // ── Выбор лучших пикселей на main и sub-screen ────────────────────
            auto bestOf = [](Layer* ls, int n, uint16_t backdrop) -> Layer {
                Layer best{ backdrop, -1, 5, false };
                for (int i = 0; i < n; ++i)
                    if (ls[i].prio > best.prio) best = ls[i];
                return best;
            };
            Layer mainPx = bestOf(mainLs, mainN, cgram_[0]);
            Layer subPx  = bestOf(subLs,  subN,  cgram_[0]);

            // Если main = backdrop, используем backdrop как «верхний слой» (src=5)
            uint16_t mainColor = (mainPx.prio < 0) ? cgram_[0] : mainPx.cgColor;
            uint8_t  mainSrc   = (mainPx.prio < 0) ? 5 : mainPx.src;
            bool     mainObjHi = (mainPx.prio >= 0) ? mainPx.objHiPal : false;

            // ── Color math (CGADSUB $2131 / CGWSEL $2130) ─────────────────────
            // Применяем ТОЛЬКО если:
            //  - CGADSUB не нулевой
            //  - текущий слой имеет соотв. бит в CGADSUB
            //  - CGWSEL bit 1 (использовать sub-screen) И sub-screen имеет
            //    видимый пиксель ИЛИ CGWSEL bit 1 = 0 (использовать COLDATA)
            uint8_t cgadsub = regs_[0x31];
            uint8_t cgwsel  = regs_[0x30];

            bool mathOnLayer = false;
            if (cgadsub != 0) {
                if      (mainSrc <= 3)              mathOnLayer = (cgadsub & (1 << mainSrc)) != 0;
                else if (mainSrc == 4 && mainObjHi) mathOnLayer = (cgadsub & 0x10) != 0;
                else if (mainSrc == 5)              mathOnLayer = (cgadsub & 0x20) != 0;
            }

            // CGWSEL bit1: 0 = операнд фиксированный цвет (COLDATA),
            //              1 = операнд sub-screen. Если sub-screen в этом пикселе
            //              прозрачен (backdrop), его backdrop = ФИКСИРОВАННЫЙ ЦВЕТ
            //              (COLDATA), а НЕ cgram[0]. Это даёт голубое небо SMW.
            bool useSub = (cgwsel & 0x02) != 0;
            bool subIsBackdrop = (subPx.prio < 0);

            if (mathOnLayer) {
                uint16_t op;
                if (useSub)
                    op = subIsBackdrop ? coldata_ : subPx.cgColor;
                else
                    op = coldata_;
                bool subtract = (cgadsub & 0x40) != 0;
                // Halve не применяется когда операнд — backdrop sub-screen'а
                bool halve    = (cgadsub & 0x80) != 0 && useSub && !subIsBackdrop;

                int r1 = (mainColor       & 0x1F);
                int g1 = ((mainColor >> 5) & 0x1F);
                int b1 = ((mainColor >>10) & 0x1F);
                int r2 = (op       & 0x1F);
                int g2 = ((op >> 5) & 0x1F);
                int b2 = ((op >>10) & 0x1F);

                int r, g, b;
                if (subtract) { r = r1 - r2; g = g1 - g2; b = b1 - b2; }
                else          { r = r1 + r2; g = g1 + g2; b = b1 + b2; }
                if (halve) { r >>= 1; g >>= 1; b >>= 1; }
                if (r < 0) r = 0; else if (r > 31) r = 31;
                if (g < 0) g = 0; else if (g > 31) g = 31;
                if (b < 0) b = 0; else if (b > 31) b = 31;
                mainColor = (uint16_t)(r | (g << 5) | (b << 10));
            }

            finalColor = cgToRGBA(mainColor, brightness);
        }

        row[x] = finalColor;
    }
}

// ─── State save/restore ────────────────────────────────────────────────────────
SnesPPU::State SnesPPU::getState() const
{
    State s{};
    s.scanline = scanline_;
    s.dot      = dot_;
    std::memcpy(s.regs, regs_, sizeof(regs_));
    return s;
}

void SnesPPU::setState(const State& s)
{
    scanline_ = s.scanline;
    dot_      = s.dot;
    std::memcpy(regs_, s.regs, sizeof(regs_));
}
