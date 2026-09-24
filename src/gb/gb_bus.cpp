// gb_bus.cpp — шина Game Boy: память, порты, таймер, джойпад, DMA, HDMA.
#include "gb_bus.h"
#include "console/state_io.h"
#include <cstdio>
#include <cstdlib>

// ─── Инициализация: состояние после загрузчика ───────────────────────────────
void GbBus::init(GbCart* cart, bool cgb)
{
    cart_ = cart;
    cgb_ = cgb;
    doubleSpeed_ = false;
    key1_ = 0;
    wram_.fill(0);
    hram_.fill(0);
    svbk_ = 1;
    ie_ = 0;
    if_ = 0xE1;
    div_ = cgb ? 0x1EA0 : 0xABCC;
    tima_ = 0; tma_ = 0; tac_ = 0xF8; timaReload_ = false;
    p1_ = 0x30; buttons_ = 0;
    sb_ = 0; sc_ = 0; serialTimer_ = 0;
    dmaActive_ = false; dmaReg_ = 0xFF; dmaSrc_ = 0; dmaIndex_ = 0; dmaDelay_ = 0;
    hdmaSrc_ = 0; hdmaDst_ = 0; hdmaRemain_ = 0; hdmaActive_ = false; stall_ = 0;
    dots_ = 0;
    serialLog_.clear();
    ppu.connect(&if_);
    ppu.reset(cgb);
    apu.reset(cgb);
}

// ─── Один M-цикл ──────────────────────────────────────────────────────────────
void GbBus::tick()
{
    timerStep();

    if (dmaActive_) {
        if (dmaDelay_) {
            --dmaDelay_;
        } else {
            ppu.dmaWriteOam(dmaIndex_, dmaSourceRead((uint16_t)(dmaSrc_ + dmaIndex_)));
            if (++dmaIndex_ >= 160) { dmaActive_ = false; ppu.setDmaActive(false); }
        }
    }

    // Порт без партнёра: при внутреннем тактировании 8 бит уходят за 8×512
    // тактов, а внутрь приходят единицы.
    if (serialTimer_ > 0 && (serialTimer_ -= 4) <= 0) {
        serialTimer_ = 0;
        sb_ = 0xFF;
        sc_ &= 0x7F;
        requestInterrupt(3);
    }

    const int dots = doubleSpeed_ ? 2 : 4;   // экран и звук идут в своём темпе
    ppu.tick(dots);
    apu.tick(dots);
    dots_ += (uint64_t)dots;

    const bool hblank = ppu.takeHblankEvent();
    if (hdmaActive_ && hblank) hdmaBlock();
}

bool GbBus::consumeStall()
{
    if (!stall_) return false;
    --stall_;
    return true;
}

// ─── Таймер ───────────────────────────────────────────────────────────────────
// TIMA растёт по СПАДУ сигнала «выбранный бит делителя И таймер включён».
// Поэтому сброс DIV и запись TAC тоже могут дать лишний шаг — как на железе.
bool GbBus::timerSignal(uint16_t div, uint8_t tac) const
{
    static const uint16_t kMask[4] = { 1u << 9, 1u << 3, 1u << 5, 1u << 7 };
    return (tac & 4) && (div & kMask[tac & 3]);
}

void GbBus::divChanged(uint16_t oldDiv, uint16_t newDiv)
{
    if (timerSignal(oldDiv, tac_) && !timerSignal(newDiv, tac_)) {
        if (++tima_ == 0) timaReload_ = true;
    }
    // Секвенсор звука: спад бита 12 (при двойной скорости — 13), 512 Гц.
    const uint16_t fsBit = doubleSpeed_ ? 0x2000 : 0x1000;
    if ((oldDiv & fsBit) && !(newDiv & fsBit)) apu.frameSequencerStep();
}

void GbBus::timerStep()
{
    // Переполнение TIMA: один M-цикл читается 0, затем TMA и прерывание.
    if (timaReload_) {
        timaReload_ = false;
        tima_ = tma_;
        requestInterrupt(2);
    }
    const uint16_t old = div_;
    div_ = (uint16_t)(div_ + 4);
    divChanged(old, div_);
}

bool GbBus::stopInstruction()
{
    const uint16_t old = div_;
    div_ = 0;                                  // STOP сбрасывает делитель
    divChanged(old, 0);
    if (cgb_ && (key1_ & 1)) {
        doubleSpeed_ = !doubleSpeed_;
        key1_ = 0;
        return true;
    }
    return false;
}

// ─── Джойпад ─────────────────────────────────────────────────────────────────
uint8_t GbBus::joypadLines() const
{
    uint8_t lines = 0x0F;                                          // 1 = не нажато
    if (!(p1_ & 0x10)) lines &= (uint8_t)~(buttons_ & 0x0F);        // крестовина
    if (!(p1_ & 0x20)) lines &= (uint8_t)~((buttons_ >> 4) & 0x0F); // A B Select Start
    return lines;
}

// Прерывание джойпада — когда любая выбранная линия падает в 0.
void GbBus::setButtons(uint8_t pressed)
{
    const uint8_t before = joypadLines();
    buttons_ = pressed;
    if (before & ~joypadLines()) requestInterrupt(4);
}

// ─── DMA ──────────────────────────────────────────────────────────────────────
uint8_t GbBus::dmaSourceRead(uint16_t addr)
{
    if (addr >= 0xE000) addr = (uint16_t)(0xC000 | (addr & 0x1FFF));
    if (addr < 0x8000 || (addr >= 0xA000 && addr < 0xC000)) return cart_->read(addr);
    if (addr < 0xA000) return ppu.vramRaw(addr);
    if (addr < 0xD000) return wram_[addr - 0xC000];
    return wram_[svbk_ * 0x1000u + (addr - 0xD000u)];
}

// HDMA (CGB): блок из 16 байт в видеопамять; процессор на это время стоит.
void GbBus::hdmaBlock()
{
    for (int i = 0; i < 16; ++i) {
        uint8_t v = dmaSourceRead((uint16_t)(hdmaSrc_ + i));
        ppu.hdmaWriteVram((uint16_t)(0x8000 | ((hdmaDst_ + i) & 0x1FFF)), v);
    }
    hdmaSrc_ = (uint16_t)(hdmaSrc_ + 16);
    hdmaDst_ = (uint16_t)((hdmaDst_ + 16) & 0x1FF0);
    if (hdmaRemain_ && --hdmaRemain_ == 0) hdmaActive_ = false;
    stall_ += doubleSpeed_ ? 16u : 8u;
}

// ─── Чтение/запись ────────────────────────────────────────────────────────────
uint8_t GbBus::read(uint16_t addr)
{
    if (addr < 0x8000) return cart_->read(addr);
    if (addr < 0xA000) return ppu.cpuReadVram(addr);
    if (addr < 0xC000) return cart_->read(addr);
    if (addr < 0xD000) return wram_[addr - 0xC000];
    if (addr < 0xE000) return wram_[svbk_ * 0x1000u + (addr - 0xD000u)];
    if (addr < 0xFE00) return read((uint16_t)(addr - 0x2000));      // зеркало WRAM
    if (addr < 0xFEA0) return ppu.cpuReadOam(addr);
    if (addr < 0xFF00) return 0x00;                                 // недоступная область
    if (addr < 0xFF80) return readIO(addr);
    if (addr < 0xFFFF) return hram_[addr - 0xFF80];
    return ie_;
}

void GbBus::write(uint16_t addr, uint8_t v)
{
    if (addr < 0x8000)      { cart_->write(addr, v); return; }
    if (addr < 0xA000)      { ppu.cpuWriteVram(addr, v); return; }
    if (addr < 0xC000)      { cart_->write(addr, v); return; }
    if (addr < 0xD000)      { wram_[addr - 0xC000] = v; return; }
    if (addr < 0xE000)      { wram_[svbk_ * 0x1000u + (addr - 0xD000u)] = v; return; }
    if (addr < 0xFE00)      { write((uint16_t)(addr - 0x2000), v); return; }
    if (addr < 0xFEA0)      { ppu.cpuWriteOam(addr, v); return; }
    if (addr < 0xFF00)      return;
    if (addr < 0xFF80)      { writeIO(addr, v); return; }
    if (addr < 0xFFFF)      { hram_[addr - 0xFF80] = v; return; }
    ie_ = v;
}

uint8_t GbBus::readIO(uint16_t addr)
{
    switch (addr) {
    case 0xFF00: return (uint8_t)(0xC0 | p1_ | joypadLines());
    case 0xFF01: return sb_;
    case 0xFF02: return (uint8_t)(sc_ | (cgb_ ? 0x7C : 0x7E));
    case 0xFF04: return (uint8_t)(div_ >> 8);
    case 0xFF05: return tima_;
    case 0xFF06: return tma_;
    case 0xFF07: return (uint8_t)(tac_ | 0xF8);
    case 0xFF0F: return (uint8_t)(if_ | 0xE0);
    case 0xFF46: return dmaReg_;
    case 0xFF4D: return cgb_ ? (uint8_t)(0x7E | (doubleSpeed_ ? 0x80 : 0) | (key1_ & 1)) : 0xFF;
    case 0xFF55:
        if (!cgb_) return 0xFF;
        return (uint8_t)((hdmaActive_ ? 0x00 : 0x80) | ((hdmaRemain_ - 1) & 0x7F));
    case 0xFF70: return cgb_ ? (uint8_t)(0xF8 | svbk_) : 0xFF;
    case 0xFF76: return cgb_ ? apu.readPcm(0) : 0xFF;
    case 0xFF77: return cgb_ ? apu.readPcm(1) : 0xFF;
    default: break;
    }
    if (addr >= 0xFF10 && addr <= 0xFF3F) return apu.read(addr);
    if ((addr >= 0xFF40 && addr <= 0xFF4B) || addr == 0xFF4F || (addr >= 0xFF68 && addr <= 0xFF6C))
        return ppu.readReg(addr);
    return 0xFF;
}

void GbBus::writeIO(uint16_t addr, uint8_t v)
{
    switch (addr) {
    case 0xFF00: {
        const uint8_t before = joypadLines();
        p1_ = (uint8_t)(v & 0x30);
        if (before & ~joypadLines()) requestInterrupt(4);
        return;
    }
    case 0xFF01: sb_ = v; return;
    case 0xFF02: {
        sc_ = (uint8_t)(v & (cgb_ ? 0x83 : 0x81));
        if ((v & 0x81) == 0x81) {
            serialTimer_ = 8 * ((cgb_ && (v & 0x02)) ? 16 : 512);
            serialLog_.push_back((char)sb_);
            if (serialLog_.size() > 1024) serialLog_.erase(0, serialLog_.size() - 1024);
            // Тестовые ROM (Blargg) печатают результат в порт: EMUDOR_GB_SERIAL=1
            // выводит отправленные байты в stdout.
            static const bool s_serial = std::getenv("EMUDOR_GB_SERIAL") != nullptr;
            if (s_serial) { std::fputc(sb_, stdout); std::fflush(stdout); }
        }
        return;
    }
    case 0xFF04: { const uint16_t old = div_; div_ = 0; divChanged(old, 0); return; }
    case 0xFF05: tima_ = v; timaReload_ = false; return;       // запись отменяет перезагрузку
    case 0xFF06: tma_ = v; return;
    case 0xFF07: {
        const bool before = timerSignal(div_, tac_);
        tac_ = (uint8_t)(0xF8 | (v & 7));
        if (before && !timerSignal(div_, tac_) && ++tima_ == 0) timaReload_ = true;
        return;
    }
    case 0xFF0F: if_ = (uint8_t)(v & 0x1F); return;
    case 0xFF46:                                               // OAM DMA
        dmaReg_ = v;
        dmaSrc_ = (uint16_t)(v << 8);
        dmaIndex_ = 0;
        dmaDelay_ = 1;
        dmaActive_ = true;
        ppu.setDmaActive(true);
        return;
    case 0xFF4D: if (cgb_) key1_ = v & 1; return;
    case 0xFF51: hdmaSrc_ = (uint16_t)((hdmaSrc_ & 0x00F0) | (v << 8)); return;
    case 0xFF52: hdmaSrc_ = (uint16_t)((hdmaSrc_ & 0xFF00) | (v & 0xF0)); return;
    case 0xFF53: hdmaDst_ = (uint16_t)((hdmaDst_ & 0x00F0) | ((v & 0x1F) << 8)); return;
    case 0xFF54: hdmaDst_ = (uint16_t)((hdmaDst_ & 0x1F00) | (v & 0xF0)); return;
    case 0xFF55:
        if (!cgb_) return;
        if (hdmaActive_ && !(v & 0x80)) { hdmaActive_ = false; return; }   // остановка HBlank-DMA
        hdmaRemain_ = (uint8_t)((v & 0x7F) + 1);
        if (v & 0x80) {
            hdmaActive_ = true;                                // по блоку в каждый HBlank
        } else {
            while (hdmaRemain_) hdmaBlock();                   // общий DMA — сразу целиком
        }
        return;
    case 0xFF70: if (cgb_) { svbk_ = (uint8_t)(v & 7); if (!svbk_) svbk_ = 1; } return;
    default: break;
    }
    if (addr >= 0xFF10 && addr <= 0xFF3F) { apu.write(addr, v); return; }
    if ((addr >= 0xFF40 && addr <= 0xFF4B) || addr == 0xFF4F || (addr >= 0xFF68 && addr <= 0xFF6C))
        ppu.writeReg(addr, v);
}

// ─── Save state ───────────────────────────────────────────────────────────────
template<class S> void GbBus::serialize(S& s)
{
    s.io(cgb_); s.io(doubleSpeed_); s.io(key1_);
    s.io(wram_); s.io(hram_); s.io(svbk_); s.io(ie_); s.io(if_);
    s.io(div_); s.io(tima_); s.io(tma_); s.io(tac_); s.io(timaReload_);
    s.io(p1_); s.io(buttons_);
    s.io(sb_); s.io(sc_); s.io(serialTimer_);
    s.io(dmaActive_); s.io(dmaReg_); s.io(dmaSrc_); s.io(dmaIndex_); s.io(dmaDelay_);
    s.io(hdmaSrc_); s.io(hdmaDst_); s.io(hdmaRemain_); s.io(hdmaActive_); s.io(stall_);
    s.io(dots_);
    ppu.serialize(s);
    apu.serialize(s);
}

template void GbBus::serialize<StateWriter>(StateWriter&);
template void GbBus::serialize<StateReader>(StateReader&);
