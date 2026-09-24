// gb_ppu.cpp — видеочип Game Boy / Game Boy Color.
#include "gb_ppu.h"
#include "console/state_io.h"
#include <algorithm>

namespace {
// Классическая зеленоватая палитра экрана DMG: от светлого к тёмному.
constexpr uint32_t kDmgShades[4] = { 0xFFE0F8D0u, 0xFF88C070u, 0xFF346856u, 0xFF081820u };
} // namespace

// ─── Сброс ────────────────────────────────────────────────────────────────────
void GbPpu::reset(bool cgb)
{
    cgb_ = cgb;
    for (auto& bank : vram_) bank.fill(0);
    oam_.fill(0);
    bgPal_.fill(0xFF);          // белые палитры, как после загрузчика CGB
    objPal_.fill(0xFF);
    lcdc_ = 0x91; stat_ = 0x00; scy_ = 0; scx_ = 0; lyc_ = 0;
    bgp_ = 0xFC; obp0_ = 0xFF; obp1_ = 0xFF; wy_ = 0; wx_ = 0;
    vbk_ = 0; bcps_ = 0; ocps_ = 0; opri_ = cgb ? 0 : 1;
    line_ = 0; ly_ = 0; dot_ = 0; mode_ = 0;
    lycEqual_ = false; statLine_ = false;
    windowTriggered_ = false; windowLine_ = 0;
    hblankEvent_ = false; dmaActive_ = false; frameDone = false;
    spriteCount_ = 0;
    fb_.fill(cgb ? 0xFFFFFFFFu : kDmgShades[0]);
    startLine();
    updateLyc();
}

// ─── Ход времени ─────────────────────────────────────────────────────────────
void GbPpu::tick(int dots)
{
    if (!(lcdc_ & 0x80)) return;
    while (dots-- > 0) {
        ++dot_;
        if (line_ < 144) {
            if (dot_ == 80) {
                enterMode(3);
                mode3End_ = (uint16_t)(80 + mode3Length());
            } else if (dot_ == mode3End_) {
                renderLine();
                enterMode(0);
                hblankEvent_ = true;
            }
        } else if (line_ == 153 && dot_ == 4) {
            ly_ = 0;                          // на строке 153 LY почти сразу показывает 0
            updateLyc();
        }
        if (dot_ < 456) continue;

        dot_ = 0;
        if (++line_ == 154) line_ = 0;
        if (line_ == 0) { windowLine_ = 0; windowTriggered_ = false; }
        ly_ = line_;
        if (line_ < 144) {
            startLine();
        } else if (line_ == 144) {
            // Причуда железа: в начале VBlank срабатывает и условие режима 2.
            if ((stat_ & 0x20) && !statLine_) { requestIrq(1); statLine_ = true; }
            enterMode(1);
            requestIrq(0);                    // VBlank
            frameDone = true;
        }
        updateLyc();
    }
}

void GbPpu::startLine()
{
    if (ly_ == wy_) windowTriggered_ = true;
    enterMode(2);
    oamScan();
}

void GbPpu::enterMode(uint8_t m)
{
    mode_ = m;
    updateStatLine();
}

void GbPpu::updateLyc()
{
    lycEqual_ = (ly_ == lyc_);
    updateStatLine();
}

// Прерывание STAT приходит по ФРОНТУ общей линии: пока хоть одно разрешённое
// условие держится, повторного прерывания нет (это важно для игр).
void GbPpu::updateStatLine()
{
    bool line = ((stat_ & 0x08) && mode_ == 0) || ((stat_ & 0x10) && mode_ == 1)
             || ((stat_ & 0x20) && mode_ == 2) || ((stat_ & 0x40) && lycEqual_);
    if (line && !statLine_) requestIrq(1);
    statLine_ = line;
}

// Первые 10 спрайтов (в порядке OAM), задевающих строку.
void GbPpu::oamScan()
{
    spriteCount_ = 0;
    const int height = (lcdc_ & 0x04) ? 16 : 8;
    for (int i = 0; i < 40 && spriteCount_ < 10; ++i) {
        int row = ly_ + 16 - oam_[i * 4];
        if (row >= 0 && row < height) sprites_[spriteCount_++] = (uint8_t)i;
    }
}

// Длина режима 3: 172 точки плюс задержки на прокрутку, окно и спрайты.
uint16_t GbPpu::mode3Length() const
{
    int len = 172 + (scx_ & 7);
    if ((lcdc_ & 0x20) && windowTriggered_ && wx_ <= 166) len += 6;
    if (lcdc_ & 0x02) {
        for (int i = 0; i < spriteCount_; ++i) {
            int x = oam_[sprites_[i] * 4 + 1];
            len += 6 + std::max(0, 5 - ((x + scx_) & 7));
        }
    }
    return (uint16_t)std::min(len, 289);
}

// ─── Цвета ────────────────────────────────────────────────────────────────────
uint32_t GbPpu::dmgColor(uint8_t shade) const { return kDmgShades[shade & 3]; }

uint32_t GbPpu::cgbColor(const std::array<uint8_t, 64>& pal, int palette, int index) const
{
    int i = palette * 8 + index * 2;
    uint16_t c = (uint16_t)(pal[i] | (pal[i + 1] << 8));
    uint32_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// ─── Отрисовка строки ────────────────────────────────────────────────────────
void GbPpu::renderLine()
{
    if (ly_ >= HEIGHT) return;
    uint32_t* row = &fb_[ly_ * WIDTH];
    uint8_t bgIndex[WIDTH];                 // номер цвета фона 0-3 (для приоритетов)
    bool    bgPrio[WIDTH];                  // CGB: тайл фона поверх спрайтов

    // ── Фон и окно ────────────────────────────────────────────────────────────
    // DMG: бит 0 LCDC гасит фон и окно (белый). CGB: фон есть всегда, а бит 0
    // решает, могут ли тайлы фона перекрывать спрайты.
    const bool bgOn = cgb_ || (lcdc_ & 0x01);
    const bool windowLine = (lcdc_ & 0x20) && windowTriggered_ && wx_ <= 166;
    const int  winX0 = (int)wx_ - 7;
    bool windowDrawn = false;
    for (int x = 0; x < WIDTH; ++x) {
        if (!bgOn) { bgIndex[x] = 0; bgPrio[x] = false; row[x] = dmgColor(0); continue; }
        const bool inWindow = windowLine && x >= winX0;
        uint16_t mapBase;
        uint8_t px, py;
        if (inWindow) {
            windowDrawn = true;
            mapBase = (lcdc_ & 0x40) ? 0x1C00 : 0x1800;
            px = (uint8_t)(x - winX0);
            py = windowLine_;
        } else {
            mapBase = (lcdc_ & 0x08) ? 0x1C00 : 0x1800;
            px = (uint8_t)(x + scx_);
            py = (uint8_t)(ly_ + scy_);
        }
        const uint16_t mapAddr = (uint16_t)(mapBase + (py >> 3) * 32 + (px >> 3));
        const uint8_t tile = vram_[0][mapAddr];
        const uint8_t attr = cgb_ ? vram_[1][mapAddr] : 0;
        int tileRow = py & 7;
        if (attr & 0x40) tileRow = 7 - tileRow;
        int col = px & 7;
        if (attr & 0x20) col = 7 - col;
        uint16_t addr = (lcdc_ & 0x10) ? (uint16_t)(tile * 16)
                                       : (uint16_t)(0x1000 + (int8_t)tile * 16);
        addr = (uint16_t)(addr + tileRow * 2);
        const auto& bank = vram_[(attr & 0x08) ? 1 : 0];
        const int shift = 7 - col;
        const uint8_t ci = (uint8_t)(((bank[addr] >> shift) & 1) | (((bank[addr + 1] >> shift) & 1) << 1));
        bgIndex[x] = ci;
        bgPrio[x] = (attr & 0x80) != 0;
        row[x] = cgb_ ? cgbColor(bgPal_, attr & 7, ci) : dmgColor((uint8_t)(bgp_ >> (ci * 2)));
    }
    if (windowDrawn) ++windowLine_;

    // ── Спрайты ──────────────────────────────────────────────────────────────
    if (!(lcdc_ & 0x02) || spriteCount_ == 0) return;
    const int height = (lcdc_ & 0x04) ? 16 : 8;

    // Порядок приоритета: у DMG — меньший X, при равенстве меньший индекс OAM;
    // у CGB — просто порядок OAM.
    uint8_t order[10];
    std::copy(sprites_, sprites_ + spriteCount_, order);
    if (!cgb_ || opri_) {
        std::stable_sort(order, order + spriteCount_, [this](uint8_t a, uint8_t b) {
            return oam_[a * 4 + 1] < oam_[b * 4 + 1];
        });
    }

    for (int x = 0; x < WIDTH; ++x) {
        for (int k = 0; k < spriteCount_; ++k) {
            const uint8_t* o = &oam_[order[k] * 4];
            const int sx = o[1] - 8;
            if (x < sx || x >= sx + 8) continue;
            const uint8_t attr = o[3];
            int spriteRow = ly_ + 16 - o[0];
            if (attr & 0x40) spriteRow = height - 1 - spriteRow;
            uint8_t tile = o[2];
            if (height == 16) tile = (uint8_t)((tile & 0xFE) | (spriteRow >= 8 ? 1 : 0));
            spriteRow &= 7;
            int col = x - sx;
            if (attr & 0x20) col = 7 - col;
            const auto& bank = vram_[(cgb_ && (attr & 0x08)) ? 1 : 0];
            const uint16_t addr = (uint16_t)(tile * 16 + spriteRow * 2);
            const int shift = 7 - col;
            const uint8_t ci = (uint8_t)(((bank[addr] >> shift) & 1) | (((bank[addr + 1] >> shift) & 1) << 1));
            if (ci == 0) continue;           // прозрачный — смотрим следующий спрайт

            // Нашёлся верхний непрозрачный спрайт: он или рисуется, или его
            // закрывает фон — нижние спрайты в этой точке уже не видны.
            bool bgWins;
            if (cgb_) bgWins = (lcdc_ & 0x01) && bgIndex[x] != 0 && (bgPrio[x] || (attr & 0x80));
            else      bgWins = (attr & 0x80) && bgIndex[x] != 0;
            if (!bgWins) {
                row[x] = cgb_ ? cgbColor(objPal_, attr & 7, ci)
                              : dmgColor((uint8_t)(((attr & 0x10) ? obp1_ : obp0_) >> (ci * 2)));
            }
            break;
        }
    }
}

// ─── Доступ процессора ────────────────────────────────────────────────────────
uint8_t GbPpu::cpuReadVram(uint16_t addr) const
{
    if (lcdOn() && mode_ == 3) return 0xFF;
    return vram_[vbk_][addr & 0x1FFF];
}

void GbPpu::cpuWriteVram(uint16_t addr, uint8_t v)
{
    if (lcdOn() && mode_ == 3) return;
    vram_[vbk_][addr & 0x1FFF] = v;
}

uint8_t GbPpu::cpuReadOam(uint16_t addr) const
{
    if (dmaActive_ || (lcdOn() && (mode_ == 2 || mode_ == 3))) return 0xFF;
    return oam_[(addr - 0xFE00) % 160];
}

void GbPpu::cpuWriteOam(uint16_t addr, uint8_t v)
{
    if (dmaActive_ || (lcdOn() && (mode_ == 2 || mode_ == 3))) return;
    oam_[(addr - 0xFE00) % 160] = v;
}

// ─── Регистры ─────────────────────────────────────────────────────────────────
uint8_t GbPpu::readReg(uint16_t addr) const
{
    switch (addr) {
    case 0xFF40: return lcdc_;
    case 0xFF41: return (uint8_t)(0x80 | (stat_ & 0x78) | (lycEqual_ ? 0x04 : 0) | (lcdOn() ? mode_ : 0));
    case 0xFF42: return scy_;
    case 0xFF43: return scx_;
    case 0xFF44: return ly_;
    case 0xFF45: return lyc_;
    case 0xFF47: return bgp_;
    case 0xFF48: return obp0_;
    case 0xFF49: return obp1_;
    case 0xFF4A: return wy_;
    case 0xFF4B: return wx_;
    case 0xFF4F: return cgb_ ? (uint8_t)(0xFE | vbk_) : 0xFF;
    case 0xFF68: return cgb_ ? (uint8_t)(bcps_ | 0x40) : 0xFF;
    case 0xFF69: return cgb_ ? bgPal_[bcps_ & 0x3F] : 0xFF;
    case 0xFF6A: return cgb_ ? (uint8_t)(ocps_ | 0x40) : 0xFF;
    case 0xFF6B: return cgb_ ? objPal_[ocps_ & 0x3F] : 0xFF;
    case 0xFF6C: return cgb_ ? (uint8_t)(0xFE | opri_) : 0xFF;
    default:     return 0xFF;
    }
}

void GbPpu::writeReg(uint16_t addr, uint8_t v)
{
    switch (addr) {
    case 0xFF40: {
        const bool wasOn = lcdOn();
        lcdc_ = v;
        if (wasOn && !(v & 0x80)) {
            // Экран выключен: PPU стоит, LY = 0, на экране пусто.
            ly_ = 0; line_ = 0; dot_ = 0; mode_ = 0;
            windowLine_ = 0; windowTriggered_ = false;
            statLine_ = false; hblankEvent_ = false;
            fb_.fill(cgb_ ? 0xFFFFFFFFu : kDmgShades[0]);
        } else if (!wasOn && (v & 0x80)) {
            // Включён: кадр начинается с первой строки.
            line_ = 0; ly_ = 0; dot_ = 0;
            windowLine_ = 0; windowTriggered_ = false;
            startLine();
            updateLyc();
        }
        break;
    }
    case 0xFF41:
        stat_ = (uint8_t)(v & 0x78);
        if (lcdOn()) updateStatLine();
        break;
    case 0xFF42: scy_ = v; break;
    case 0xFF43: scx_ = v; break;
    case 0xFF45: lyc_ = v; if (lcdOn()) updateLyc(); break;
    case 0xFF47: bgp_ = v; break;
    case 0xFF48: obp0_ = v; break;
    case 0xFF49: obp1_ = v; break;
    case 0xFF4A: wy_ = v; break;
    case 0xFF4B: wx_ = v; break;
    case 0xFF4F: if (cgb_) vbk_ = v & 1; break;
    case 0xFF68: if (cgb_) bcps_ = (uint8_t)(v & 0xBF); break;
    case 0xFF69:
        if (cgb_) {
            bgPal_[bcps_ & 0x3F] = v;
            if (bcps_ & 0x80) bcps_ = (uint8_t)(0x80 | ((bcps_ + 1) & 0x3F));
        }
        break;
    case 0xFF6A: if (cgb_) ocps_ = (uint8_t)(v & 0xBF); break;
    case 0xFF6B:
        if (cgb_) {
            objPal_[ocps_ & 0x3F] = v;
            if (ocps_ & 0x80) ocps_ = (uint8_t)(0x80 | ((ocps_ + 1) & 0x3F));
        }
        break;
    case 0xFF6C: if (cgb_) opri_ = v & 1; break;
    default: break;
    }
}

// ─── Save state ───────────────────────────────────────────────────────────────
template<class S> void GbPpu::serialize(S& s)
{
    s.io(vram_); s.io(oam_); s.io(fb_);
    s.io(lcdc_); s.io(stat_); s.io(scy_); s.io(scx_); s.io(ly_); s.io(lyc_);
    s.io(bgp_); s.io(obp0_); s.io(obp1_); s.io(wy_); s.io(wx_);
    s.io(vbk_); s.io(bcps_); s.io(ocps_); s.io(opri_); s.io(bgPal_); s.io(objPal_);
    s.io(line_); s.io(dot_); s.io(mode3End_); s.io(mode_);
    s.io(lycEqual_); s.io(statLine_); s.io(windowTriggered_); s.io(windowLine_);
    s.io(hblankEvent_); s.io(dmaActive_); s.io(frameDone);
    s.io(spriteCount_); s.io(sprites_);
}

template void GbPpu::serialize<StateWriter>(StateWriter&);
template void GbPpu::serialize<StateReader>(StateReader&);
