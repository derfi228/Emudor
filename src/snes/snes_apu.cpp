// snes_apu.cpp — SPC700 CPU skeleton + заглушка DSP
// SPC700 = Sony 8-bit CPU, ~1.024 MHz, архитектура похожа на 6502.
// Все операции с памятью идут через spcRead/spcWrite для корректного
// маппинга портов и IPL ROM.
#include "snes_apu.h"
#include <cstring>

// ─── IPL ROM (Sony, неизменный) ───────────────────────────────────────────────
// Оригинальный 64-байтовый загрузчик SNES APU.
const uint8_t kSpcIplRom[64] = {
    0xCD,0xEF,0xBD,0xE8,0x00,0xC6,0x1D,0xD0,0xFC,0x8F,0xAA,0xF4,0x8F,0xBB,0xF5,0x78,
    0xCC,0xF4,0xD0,0xFB,0x2F,0x19,0xEB,0xF4,0xD0,0xFC,0x7E,0xF4,0xD0,0x0B,0xE4,0xF5,
    0xCB,0xF4,0xD7,0x00,0xFC,0xD0,0xF3,0xAB,0x01,0x10,0xEF,0x7E,0xF4,0x10,0xEB,0xBA,
    0xF6,0xDA,0x00,0xBA,0xF4,0xC4,0xF4,0xDD,0x5D,0xD0,0xDB,0x1F,0x00,0x00,0xC0,0xFF
};

// ─── Reset ────────────────────────────────────────────────────────────────────
void SnesAPU::reset()
{
    std::memset(portIn_,  0, sizeof(portIn_));
    std::memset(portOut_, 0, sizeof(portOut_));
    samples_.clear();
    sampleDiv_ = 0;
    dsp_.fill(0);
    for (auto& p : dspPhase_) p = 0;
    dspKonLatch_ = 0;

    spcReset();
}

void SnesAPU::spcReset()
{
    ram_.fill(0);
    // Загружаем IPL ROM в $FFC0–$FFFF
    std::memcpy(&ram_[0xFFC0], kSpcIplRom, 64);

    // Вектор сброса
    spcPC_  = (uint16_t)(ram_[0xFFFE] | (ram_[0xFFFF] << 8));
    spcA_   = 0;
    spcX_   = 0;
    spcY_   = 0;
    spcSP_  = 0xEF;
    spcPSW_ = 0x02;  // Z=1

    // Порты: при старте IPL отвечает $AA $BB
    portOut_[0] = 0xAA;
    portOut_[1] = 0xBB;
    portOut_[2] = 0;
    portOut_[3] = 0;

    // Таймеры
    for (int i = 0; i < 3; ++i) {
        timerPeriod_[i]  = 0;
        timerIntern_[i]  = 0;
        timerCounter_[i] = 0;
        timerDiv_[i]     = 0;
    }
    timerEnabled_ = 0;
}

// ─── Тик таймеров SPC700 ─────────────────────────────────────────────────────
void SnesAPU::tickTimers()
{
    for (int i = 0; i < 3; ++i) {
        if (!(timerEnabled_ & (1u << i))) continue;
        uint32_t div = (i < 2) ? TIMER_DIV01 : TIMER_DIV2;
        if (++timerDiv_[i] >= div) {
            timerDiv_[i] = 0;
            // Инкрементируем внутренний счётчик и проверяем период
            ++timerIntern_[i];
            uint8_t target = timerPeriod_[i];          // 0 = период 256
            if (timerIntern_[i] == target) {           // совпадение → тик выхода
                timerIntern_[i] = 0;
                timerCounter_[i] = (uint8_t)((timerCounter_[i] + 1) & 0x0F);
            }
        }
    }
}

// ─── Синхронизирующий burst SPC ──────────────────────────────────────────────
// Вызывается перед чтением портов $2140-$2143 чтобы SPC успел обновить portOut.
void SnesAPU::runCatchup(int steps)
{
    for (int i = 0; i < steps; ++i) {
        spcStep();
        tickTimers();
    }
}

// ─── Такт APU ─────────────────────────────────────────────────────────────────
void SnesAPU::clock()
{
    // Шагаем SPC700
    spcStep();
    tickTimers();

    // Генерируем аудио-сэмпл: ~1,024,000 / 32000 ≈ 32 тактов на сэмпл
    ++sampleDiv_;
    if (sampleDiv_ >= 32) {
        sampleDiv_ = 0;
        // DSP-микшер: тишина пока не реализован полный BRR-декодер.
        // Упрощённый меандр-микшер был слишком шумным и склонным к артефактам.
        samples_.push_back(0);
        samples_.push_back(0);
    }
}

// ─── Порты коммуникации ────────────────────────────────────────────────────────
void SnesAPU::writePort(uint8_t port, uint8_t data)
{
    if (port < 4) portIn_[port] = data;

    // Burst только во время загрузки IPL ROM (SPC PC в диапазоне $FFC0–$FFFF).
    // Гарантирует завершение upload-протокола без замедления во время игры.
    if (port == 0 && spcPC_ >= 0xFFC0) {
        for (int i = 0; i < 4000 && portOut_[0] != data; ++i) {
            spcStep();
            tickTimers();
        }
    }
}

uint8_t SnesAPU::readPort(uint8_t port)
{
    if (port < 4) return portOut_[port];
    return 0;
}

// ─── Чтение/запись памяти SPC700 ─────────────────────────────────────────────
uint8_t SnesAPU::spcRead(uint16_t addr)
{
    // IPL ROM: видим только если бит CONTROL[7] = 1
    if (addr >= 0xFFC0 && (ram_[0x00F1] & 0x80)) {
        return kSpcIplRom[addr - 0xFFC0];
    }
    // Порты SNES→SPC ($F4–$F7)
    if (addr >= 0x00F4 && addr <= 0x00F7) {
        return portIn_[addr - 0x00F4];
    }
    // Счётчики таймеров $FD–$FF: возвращаем 4-бит значение и сбрасываем
    if (addr == 0x00FD) { uint8_t v = timerCounter_[0] & 0x0F; timerCounter_[0] = 0; return v; }
    if (addr == 0x00FE) { uint8_t v = timerCounter_[1] & 0x0F; timerCounter_[1] = 0; return v; }
    if (addr == 0x00FF) { uint8_t v = timerCounter_[2] & 0x0F; timerCounter_[2] = 0; return v; }
    // DSP-регистр читается через $F3
    if (addr == 0x00F3) {
        uint8_t idx = (uint8_t)(ram_[0x00F2] & 0x7F);
        return dsp_[idx];
    }
    return ram_[addr];
}

void SnesAPU::spcWrite(uint16_t addr, uint8_t data)
{
    ram_[addr] = data;

    // Порты SPC→SNES ($F4–$F7)
    if (addr >= 0x00F4 && addr <= 0x00F7) {
        portOut_[addr - 0x00F4] = data;
        return;
    }

    // CONTROL регистр $F1: управление таймерами и IPL ROM
    if (addr == 0x00F1) {
        // Биты 0-2: включить/выключить таймеры
        for (int i = 0; i < 3; ++i) {
            bool wasEnabled = (timerEnabled_ & (1u << i)) != 0;
            bool nowEnabled = (data & (1u << i)) != 0;
            if (!wasEnabled && nowEnabled) {
                // При включении — полный сброс
                timerDiv_[i]     = 0;
                timerIntern_[i]  = 0;
                timerCounter_[i] = 0;
            }
        }
        timerEnabled_ = data & 0x07;
        // Бит 4: сброс portIn_[0/1]; бит 5: сброс portIn_[2/3]
        if (data & 0x10) { portIn_[0] = 0; portIn_[1] = 0; }
        if (data & 0x20) { portIn_[2] = 0; portIn_[3] = 0; }
        return;
    }

    // Периоды таймеров $FA–$FC
    if (addr == 0x00FA) { timerPeriod_[0] = data; return; }
    if (addr == 0x00FB) { timerPeriod_[1] = data; return; }
    if (addr == 0x00FC) { timerPeriod_[2] = data; return; }

    // DSP-регистры: $F3 пишет в dsp_[ram_[$F2] & 0x7F]
    if (addr == 0x00F3) {
        uint8_t idx = (uint8_t)(ram_[0x00F2] & 0x7F);
        dsp_[idx] = data;
        // KON ($4C): при включении нового голоса сбрасываем фазу
        if (idx == 0x4C) {
            for (int v = 0; v < 8; ++v)
                if ((data & (1 << v)) && !(dspKonLatch_ & (1 << v)))
                    dspPhase_[v] = 0;
            dspKonLatch_ = data;
        }
        return;
    }
}

// ─── Вспомогательные методы ───────────────────────────────────────────────────
uint8_t SnesAPU::spcFetch()
{
    return spcRead(spcPC_++);
}

void SnesAPU::spcPush(uint8_t v)
{
    ram_[0x0100 + spcSP_--] = v;
}

uint8_t SnesAPU::spcPop()
{
    return ram_[0x0100 + ++spcSP_];
}

void SnesAPU::setNZ(uint8_t v)
{
    spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N | FL_Z))
        | (v == 0    ? FL_Z : 0)
        | (v & 0x80  ? FL_N : 0));
}

// ─── SPC700: одна инструкция ──────────────────────────────────────────────────
// Реализованы наиболее частые опкоды (~ достаточно для IPL ROM).
// Полная реализация выходит за рамки данной фазы.
void SnesAPU::spcStep()
{
    uint8_t op = spcFetch();
    uint8_t dp = (spcPSW_ & FL_P) ? 0x01 : 0x00;  // Direct Page: $00xx или $01xx

    auto dpAddr = [&](uint8_t d) -> uint16_t { return (uint16_t)((dp << 8) | d); };
    auto absAddr = [&]() -> uint16_t {
        uint8_t lo = spcFetch(), hi = spcFetch();
        return (uint16_t)(lo | (hi << 8));
    };

    switch (op) {
    // ── NOP ──────────────────────────────────────────────────────────────────
    case 0x00: break;

    // ── MOV A, #imm ──────────────────────────────────────────────────────────
    case 0xE8: spcA_ = spcFetch(); setNZ(spcA_); break;

    // ── MOV X, #imm ──────────────────────────────────────────────────────────
    case 0xCD: spcX_ = spcFetch(); setNZ(spcX_); break;

    // ── MOV Y, #imm ──────────────────────────────────────────────────────────
    case 0x8D: spcY_ = spcFetch(); setNZ(spcY_); break;

    // ── MOV A, dp ─────────────────────────────────────────────────────────────
    case 0xE4: { uint8_t d = spcFetch(); spcA_ = spcRead(dpAddr(d)); setNZ(spcA_); break; }

    // ── MOV A, dp+X ──────────────────────────────────────────────────────────
    case 0xF4: { uint8_t d = spcFetch(); spcA_ = spcRead(dpAddr((uint8_t)(d+spcX_))); setNZ(spcA_); break; }

    // ── MOV A, !abs ──────────────────────────────────────────────────────────
    case 0xE5: { uint16_t a = absAddr(); spcA_ = spcRead(a); setNZ(spcA_); break; }

    // ── MOV A, X ──────────────────────────────────────────────────────────────
    case 0x7D: spcA_ = spcX_; setNZ(spcA_); break;

    // ── MOV A, Y ──────────────────────────────────────────────────────────────
    case 0xDD: spcA_ = spcY_; setNZ(spcA_); break;

    // ── MOV X, A ──────────────────────────────────────────────────────────────
    case 0x5D: spcX_ = spcA_; setNZ(spcX_); break;

    // ── MOV Y, A ──────────────────────────────────────────────────────────────
    case 0xFD: spcY_ = spcA_; setNZ(spcY_); break;

    // ── MOV X, dp ─────────────────────────────────────────────────────────────
    case 0xF8: { uint8_t d = spcFetch(); spcX_ = spcRead(dpAddr(d)); setNZ(spcX_); break; }

    // ── MOV Y, dp ─────────────────────────────────────────────────────────────
    case 0xEB: { uint8_t d = spcFetch(); spcY_ = spcRead(dpAddr(d)); setNZ(spcY_); break; }

    // ── MOV dp, A ────────────────────────────────────────────────────────────
    case 0xC4: { uint8_t d = spcFetch(); spcWrite(dpAddr(d), spcA_); break; }

    // ── MOV dp+X, A ──────────────────────────────────────────────────────────
    case 0xD4: { uint8_t d = spcFetch(); spcWrite(dpAddr((uint8_t)(d+spcX_)), spcA_); break; }

    // ── MOV !abs, A ──────────────────────────────────────────────────────────
    case 0xC5: { uint16_t a = absAddr(); spcWrite(a, spcA_); break; }

    // ── MOV dp, #imm ─────────────────────────────────────────────────────────
    case 0x8F: { uint8_t imm = spcFetch(); uint8_t d = spcFetch(); spcWrite(dpAddr(d), imm); break; }

    // ── MOV (X), A ───────────────────────────────────────────────────────────
    case 0xC6: spcWrite(dpAddr(spcX_), spcA_); break;

    // ── MOV A, (X)+ (post-increment) ─────────────────────────────────────────
    case 0xBF: { spcA_ = spcRead(dpAddr(spcX_++)); setNZ(spcA_); break; }

    // ── MOV (X)+, A ──────────────────────────────────────────────────────────
    case 0xAF: spcWrite(dpAddr(spcX_++), spcA_); break;

    // ── MOV dp, dp ───────────────────────────────────────────────────────────
    case 0xFA: { uint8_t s = spcFetch(), d = spcFetch();
                 spcWrite(dpAddr(d), spcRead(dpAddr(s))); break; }

    // ── PUSH A ───────────────────────────────────────────────────────────────
    case 0x2D: spcPush(spcA_); break;
    case 0x4D: spcPush(spcX_); break;
    case 0x6D: spcPush(spcY_); break;
    case 0x0D: spcPush(spcPSW_); break;

    // ── POP A ────────────────────────────────────────────────────────────────
    case 0xAE: spcA_   = spcPop(); break;
    case 0xCE: spcX_   = spcPop(); break;
    case 0xEE: spcY_   = spcPop(); break;
    case 0x8E: spcPSW_ = spcPop(); break;

    // ── ADC A, #imm ──────────────────────────────────────────────────────────
    case 0x88: {
        uint8_t imm = spcFetch();
        uint16_t r = (uint16_t)(spcA_ + imm + (spcPSW_ & FL_C));
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_V|FL_H|FL_Z|FL_C))
            | (r > 0xFF  ? FL_C : 0)
            | ((r & 0xFF) == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0)
            | (((spcA_ ^ r) & (imm ^ r) & 0x80) ? FL_V : 0));
        spcA_ = (uint8_t)r;
        break;
    }

    // ── SBC A, #imm ──────────────────────────────────────────────────────────
    case 0xA8: {
        uint8_t imm = spcFetch();
        uint16_t r = (uint16_t)(spcA_ - imm - !(spcPSW_ & FL_C));
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_V|FL_H|FL_Z|FL_C))
            | (spcA_ >= (uint16_t)(imm + !(spcPSW_&FL_C)) ? FL_C : 0)
            | ((r & 0xFF) == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0)
            | (((spcA_ ^ imm) & (spcA_ ^ r) & 0x80) ? FL_V : 0));
        spcA_ = (uint8_t)r;
        break;
    }

    // ── CMP A, #imm ──────────────────────────────────────────────────────────
    case 0x68: {
        uint8_t imm = spcFetch();
        uint8_t r = (uint8_t)(spcA_ - imm);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcA_ >= imm ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── CMP X, #imm ──────────────────────────────────────────────────────────
    case 0xC8: {
        uint8_t imm = spcFetch();
        uint8_t r = (uint8_t)(spcX_ - imm);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcX_ >= imm ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── CMP A, dp ────────────────────────────────────────────────────────────
    case 0x64: {
        uint8_t d = spcFetch();
        uint8_t val = spcRead(dpAddr(d));
        uint8_t r = (uint8_t)(spcA_ - val);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcA_ >= val ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── AND A, #imm ──────────────────────────────────────────────────────────
    case 0x28: spcA_ &= spcFetch(); setNZ(spcA_); break;

    // ── OR A, #imm ───────────────────────────────────────────────────────────
    case 0x08: spcA_ |= spcFetch(); setNZ(spcA_); break;

    // ── EOR A, #imm ──────────────────────────────────────────────────────────
    case 0x48: spcA_ ^= spcFetch(); setNZ(spcA_); break;

    // ── INC A ────────────────────────────────────────────────────────────────
    case 0xBC: ++spcA_; setNZ(spcA_); break;
    case 0x3D: ++spcX_; setNZ(spcX_); break;
    case 0xFC: ++spcY_; setNZ(spcY_); break;

    // ── DEC A ────────────────────────────────────────────────────────────────
    case 0x9C: --spcA_; setNZ(spcA_); break;
    case 0x1D: --spcX_; setNZ(spcX_); break;
    case 0xDC: --spcY_; setNZ(spcY_); break;

    // ── INC dp ───────────────────────────────────────────────────────────────
    case 0xAB: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d);
        uint8_t v = (uint8_t)(spcRead(a) + 1); spcWrite(a, v); setNZ(v); break;
    }
    // ── DEC dp ───────────────────────────────────────────────────────────────
    case 0x8B: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d);
        uint8_t v = (uint8_t)(spcRead(a) - 1); spcWrite(a, v); setNZ(v); break;
    }

    // ── ASL A ────────────────────────────────────────────────────────────────
    case 0x1C: {
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (spcA_ >> 7));
        spcA_ <<= 1; setNZ(spcA_); break;
    }
    // ── LSR A ────────────────────────────────────────────────────────────────
    case 0x5C: {
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (spcA_ & FL_C));
        spcA_ >>= 1; setNZ(spcA_); break;
    }
    // ── ROL A ────────────────────────────────────────────────────────────────
    case 0x3C: {
        uint8_t c = spcPSW_ & FL_C;
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (spcA_ >> 7));
        spcA_ = (uint8_t)((spcA_ << 1) | c); setNZ(spcA_); break;
    }
    // ── ROR A ────────────────────────────────────────────────────────────────
    case 0x7C: {
        uint8_t c = spcPSW_ & FL_C;
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (spcA_ & FL_C));
        spcA_ = (uint8_t)((spcA_ >> 1) | (c << 7)); setNZ(spcA_); break;
    }

    // ── BRA (always branch) ────────────────────────────────────────────────
    case 0x2F: {
        int8_t off = (int8_t)spcFetch();
        spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── BEQ ──────────────────────────────────────────────────────────────────
    case 0xF0: {
        int8_t off = (int8_t)spcFetch();
        if (spcPSW_ & FL_Z) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── BNE ──────────────────────────────────────────────────────────────────
    case 0xD0: {
        int8_t off = (int8_t)spcFetch();
        if (!(spcPSW_ & FL_Z)) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── BCC ──────────────────────────────────────────────────────────────────
    case 0x90: {
        int8_t off = (int8_t)spcFetch();
        if (!(spcPSW_ & FL_C)) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── BCS ──────────────────────────────────────────────────────────────────
    case 0xB0: {
        int8_t off = (int8_t)spcFetch();
        if (spcPSW_ & FL_C) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── BPL ──────────────────────────────────────────────────────────────────
    case 0x10: {
        int8_t off = (int8_t)spcFetch();
        if (!(spcPSW_ & FL_N)) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── BMI ──────────────────────────────────────────────────────────────────
    case 0x30: {
        int8_t off = (int8_t)spcFetch();
        if (spcPSW_ & FL_N) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── CBNE dp, rel ─────────────────────────────────────────────────────────
    case 0x2E: {
        uint8_t d = spcFetch(); int8_t off = (int8_t)spcFetch();
        if (spcA_ != spcRead(dpAddr(d))) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── DBNZ Y, rel ──────────────────────────────────────────────────────────
    case 0xFE: {
        int8_t off = (int8_t)spcFetch();
        --spcY_;
        if (spcY_ != 0) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── JMP !abs ─────────────────────────────────────────────────────────────
    case 0x5F: {
        uint16_t a = absAddr();
        spcPC_ = a;
        break;
    }

    // ── CALL !abs ────────────────────────────────────────────────────────────
    case 0x3F: {
        uint16_t a = absAddr();
        spcPush((uint8_t)(spcPC_ >> 8));
        spcPush((uint8_t)(spcPC_));
        spcPC_ = a;
        break;
    }
    // ── RET ──────────────────────────────────────────────────────────────────
    case 0x6F: {
        uint8_t lo = spcPop(), hi = spcPop();
        spcPC_ = (uint16_t)(lo | (hi << 8));
        break;
    }

    // ── MOV YA, dp (слово) ───────────────────────────────────────────────────
    case 0xBA: {
        uint8_t d = spcFetch();
        spcA_ = spcRead(dpAddr(d));
        spcY_ = spcRead(dpAddr((uint8_t)(d + 1)));
        setNZ(spcA_);
        break;
    }
    // ── MOV dp, YA ───────────────────────────────────────────────────────────
    case 0xDA: {
        uint8_t d = spcFetch();
        spcWrite(dpAddr(d), spcA_);
        spcWrite(dpAddr((uint8_t)(d + 1)), spcY_);
        break;
    }

    // ── CLRP (clear P flag) ──────────────────────────────────────────────────
    case 0x20: spcPSW_ &= (uint8_t)~FL_P; break;
    // ── SETP ─────────────────────────────────────────────────────────────────
    case 0x40: spcPSW_ |= FL_P; break;
    // ── CLRC ─────────────────────────────────────────────────────────────────
    case 0x60: spcPSW_ &= (uint8_t)~FL_C; break;
    // ── SETC ─────────────────────────────────────────────────────────────────
    case 0x80: spcPSW_ |= FL_C; break;
    // ── CLRV ─────────────────────────────────────────────────────────────────
    case 0xE0: spcPSW_ &= (uint8_t)~(FL_V | FL_H); break;
    // ── EI / DI ──────────────────────────────────────────────────────────────
    case 0xA0: spcPSW_ |= FL_I;  break;
    case 0xC0: spcPSW_ &= (uint8_t)~FL_I; break;

    // ── SET1/CLR1 dp.bit (0x12, 0x32, 0x52, 0x72, 0x92, 0xB2, 0xD2, 0xF2)
    // CLR1: 0x12, 0x32, 0x52, 0x72, 0x92, 0xB2, 0xD2, 0xF2
    // SET1: 0x02, 0x22, 0x42, 0x62, 0x82, 0xA2, 0xC2, 0xE2
    case 0x02: case 0x22: case 0x42: case 0x62:
    case 0x82: case 0xA2: case 0xC2: case 0xE2: {
        uint8_t bit = (op >> 5) & 7;
        uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        spcWrite(a, (uint8_t)(spcRead(a) | (1u << bit)));
        break;
    }
    case 0x12: case 0x32: case 0x52: case 0x72:
    case 0x92: case 0xB2: case 0xD2: case 0xF2: {
        uint8_t bit = (op >> 5) & 7;
        uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        spcWrite(a, (uint8_t)(spcRead(a) & ~(1u << bit)));
        break;
    }

    // ── BBS/BBC dp.bit, rel ───────────────────────────────────────────────────
    // BBS: 0x03,0x23,0x43,0x63,0x83,0xA3,0xC3,0xE3
    // BBC: 0x13,0x33,0x53,0x73,0x93,0xB3,0xD3,0xF3
    case 0x03: case 0x23: case 0x43: case 0x63:
    case 0x83: case 0xA3: case 0xC3: case 0xE3: {
        uint8_t bit = (op >> 5) & 7;
        uint8_t d = spcFetch(); int8_t off = (int8_t)spcFetch();
        if (spcRead(dpAddr(d)) & (1u << bit)) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    case 0x13: case 0x33: case 0x53: case 0x73:
    case 0x93: case 0xB3: case 0xD3: case 0xF3: {
        uint8_t bit = (op >> 5) & 7;
        uint8_t d = spcFetch(); int8_t off = (int8_t)spcFetch();
        if (!(spcRead(dpAddr(d)) & (1u << bit))) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── TCALL n (вектор по адресу $FFxx) ─────────────────────────────────────
    case 0x01: case 0x11: case 0x21: case 0x31:
    case 0x41: case 0x51: case 0x61: case 0x71:
    case 0x91: case 0xB1: case 0xD1: case 0xF1: {
        uint8_t n = (op >> 4) & 0x0F;
        uint16_t vec = (uint16_t)(0xFFDE - n * 2);
        spcPush((uint8_t)(spcPC_ >> 8));
        spcPush((uint8_t)(spcPC_));
        spcPC_ = (uint16_t)(spcRead(vec) | (spcRead((uint16_t)(vec+1)) << 8));
        break;
    }

    // ── MOV A, (X) ───────────────────────────────────────────────────────────
    case 0xE6: spcA_ = spcRead(dpAddr(spcX_)); setNZ(spcA_); break;

    // ── MOV SP, X ($BD) ──────────────────────────────────────────────────────
    case 0xBD: spcSP_ = spcX_; break;

    // ── MOV X, SP ($9D) ──────────────────────────────────────────────────────
    case 0x9D: spcX_ = spcSP_; setNZ(spcX_); break;

    // ── MOV dp, Y ($CB) ──────────────────────────────────────────────────────
    case 0xCB: { uint8_t d = spcFetch(); spcWrite(dpAddr(d), spcY_); break; }

    // ── MOV dp, X ($D8) ──────────────────────────────────────────────────────
    case 0xD8: { uint8_t d = spcFetch(); spcWrite(dpAddr(d), spcX_); break; }

    // ── MOV Y, dp+X ($FB) ────────────────────────────────────────────────────
    case 0xFB: { uint8_t d = spcFetch(); spcY_ = spcRead(dpAddr((uint8_t)(d+spcX_))); setNZ(spcY_); break; }

    // ── MOV [dp]+Y, A ($D7) — косвенная запись с Y-индексом ─────────────────
    case 0xD7: {
        uint8_t  d   = spcFetch();
        uint16_t ptr = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        spcWrite((uint16_t)(ptr + spcY_), spcA_);
        break;
    }

    // ── MOV A, [dp]+Y ($F7) — косвенное чтение с Y-индексом ─────────────────
    case 0xF7: {
        uint8_t  d   = spcFetch();
        uint16_t ptr = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        spcA_ = spcRead((uint16_t)(ptr + spcY_));
        setNZ(spcA_);
        break;
    }

    // ── MOV A, [dp+X] ($E7) — прединдексная косвенная адресация ─────────────
    case 0xE7: {
        uint8_t  d   = spcFetch();
        uint16_t ptr = (uint16_t)(spcRead(dpAddr((uint8_t)(d+spcX_)))
                     | (spcRead(dpAddr((uint8_t)(d+spcX_+1))) << 8));
        spcA_ = spcRead(ptr);
        setNZ(spcA_);
        break;
    }

    // ── CMP Y, dp ($7E) ──────────────────────────────────────────────────────
    case 0x7E: {
        uint8_t d = spcFetch();
        uint8_t val = spcRead(dpAddr(d));
        uint8_t r = (uint8_t)(spcY_ - val);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcY_ >= val ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── CMP dp, #imm ($78) — operand order: imm, dp ──────────────────────────
    case 0x78: {
        uint8_t imm = spcFetch(); uint8_t d = spcFetch();
        uint8_t val = spcRead(dpAddr(d));
        uint8_t r = (uint8_t)(val - imm);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (val >= imm ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── CMP Y, #imm ($AD) ────────────────────────────────────────────────────
    case 0xAD: {
        uint8_t imm = spcFetch();
        uint8_t r = (uint8_t)(spcY_ - imm);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcY_ >= imm ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── ADC dp, #imm ($98) ───────────────────────────────────────────────────
    case 0x98: {
        uint8_t imm = spcFetch(); uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        uint8_t  v = spcRead(a);
        uint16_t r = (uint16_t)(v + imm + (spcPSW_ & FL_C));
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_V|FL_H|FL_Z|FL_C))
            | (r > 0xFF ? FL_C : 0)
            | ((r&0xFF)==0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0)
            | (((v^r)&(imm^r)&0x80) ? FL_V : 0));
        spcWrite(a, (uint8_t)r);
        break;
    }

    // ── AND dp, #imm ($38) ───────────────────────────────────────────────────
    case 0x38: {
        uint8_t imm = spcFetch(); uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        uint8_t v = (uint8_t)(spcRead(a) & imm);
        spcWrite(a, v); setNZ(v);
        break;
    }

    // ── OR dp, #imm ($18) ────────────────────────────────────────────────────
    case 0x18: {
        uint8_t imm = spcFetch(); uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        uint8_t v = (uint8_t)(spcRead(a) | imm);
        spcWrite(a, v); setNZ(v);
        break;
    }

    // ── EOR dp, #imm ($58) ───────────────────────────────────────────────────
    case 0x58: {
        uint8_t imm = spcFetch(); uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        uint8_t v = (uint8_t)(spcRead(a) ^ imm);
        spcWrite(a, v); setNZ(v);
        break;
    }

    // ── JMP [abs+X] ($1F) — абсолютная косвенная адресация с X ──────────────
    case 0x1F: {
        uint16_t base = absAddr();
        uint16_t ea   = (uint16_t)(base + spcX_);
        spcPC_ = (uint16_t)(spcRead(ea) | (spcRead((uint16_t)(ea+1)) << 8));
        break;
    }

    // ── XCN A ($9F) — swap nibbles: A = (A>>4) | (A<<4) ─────────────────────
    case 0x9F: {
        spcA_ = (uint8_t)((spcA_ >> 4) | (spcA_ << 4));
        setNZ(spcA_);
        break;
    }

    // ── MUL YA ($CF): YA = Y * A (16-bit result) ─────────────────────────────
    case 0xCF: {
        uint16_t r = (uint16_t)(spcY_ * spcA_);
        spcA_ = (uint8_t)(r & 0xFF);
        spcY_ = (uint8_t)(r >> 8);
        setNZ(spcY_);
        break;
    }

    // ── DIV YA, X ($9E) ──────────────────────────────────────────────────────
    case 0x9E: {
        if (spcX_ == 0) {
            spcA_ = 0xFF; spcY_ = 0xFF;  // division by zero
        } else {
            uint16_t ya = (uint16_t)((spcY_ << 8) | spcA_);
            spcA_ = (uint8_t)(ya / spcX_);
            spcY_ = (uint8_t)(ya % spcX_);
        }
        setNZ(spcA_);
        break;
    }

    // ── SLEEP ($EF) / STOP ($FF) — заглушка (NOP) ────────────────────────────
    case 0xEF:
    case 0xFF:
        break;

    // ── ADDW YA, dp ($7A) ────────────────────────────────────────────────────
    case 0x7A: {
        uint8_t  d  = spcFetch();
        uint16_t ya = (uint16_t)((spcY_ << 8) | spcA_);
        uint16_t v  = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        uint32_t r  = (uint32_t)(ya + v);
        spcA_ = (uint8_t)(r & 0xFF);
        spcY_ = (uint8_t)((r >> 8) & 0xFF);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_V|FL_H|FL_Z|FL_C))
            | (r > 0xFFFF ? FL_C : 0)
            | ((r&0xFFFF)==0 ? FL_Z : 0)
            | (r & 0x8000 ? FL_N : 0));
        break;
    }

    // ── SUBW YA, dp ($9A) ────────────────────────────────────────────────────
    case 0x9A: {
        uint8_t  d  = spcFetch();
        uint16_t ya = (uint16_t)((spcY_ << 8) | spcA_);
        uint16_t v  = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        uint32_t r  = (uint32_t)(ya - v);
        spcA_ = (uint8_t)(r & 0xFF);
        spcY_ = (uint8_t)((r >> 8) & 0xFF);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_V|FL_H|FL_Z|FL_C))
            | (ya >= v ? FL_C : 0)
            | ((r&0xFFFF)==0 ? FL_Z : 0)
            | (r & 0x8000 ? FL_N : 0));
        break;
    }

    // ── CMPW YA, dp ($5A) ────────────────────────────────────────────────────
    case 0x5A: {
        uint8_t  d  = spcFetch();
        uint16_t ya = (uint16_t)((spcY_ << 8) | spcA_);
        uint16_t v  = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        uint32_t r  = (uint32_t)(ya - v);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (ya >= v ? FL_C : 0)
            | ((r&0xFFFF)==0 ? FL_Z : 0)
            | (r & 0x8000 ? FL_N : 0));
        break;
    }

    // ── MOV dp, dp ($CA — это и MOV !abs, Y) ─────────────────────────────────
    // $CA = MOV !abs, Y (absolute store Y)
    case 0xCA: { uint16_t a = absAddr(); spcWrite(a, spcY_); break; }

    // ── MOV X, #imm ($CD уже есть) / MOV Y, !abs ($EC) ──────────────────────
    case 0xEC: { uint16_t a = absAddr(); spcY_ = spcRead(a); setNZ(spcY_); break; }

    // ── NOTC ($ED) — инвертировать C ─────────────────────────────────────────
    case 0xED: spcPSW_ ^= FL_C; break;

    // ── DEC dp ($8B уже) / INC X dp ($3D уже) ────────────────────────────────
    // Заглушки для MOV abs, A ($C5 уже), MOV abs, X ($C9):
    case 0xC9: { uint16_t a = absAddr(); spcWrite(a, spcX_); break; }

    // ── DBNZ dp, rel ($6E) ────────────────────────────────────────────────────
    case 0x6E: {
        uint8_t d   = spcFetch(); int8_t off = (int8_t)spcFetch();
        uint16_t a  = dpAddr(d);
        uint8_t  v  = (uint8_t)(spcRead(a) - 1);
        spcWrite(a, v);
        if (v != 0) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── AND A, dp ($24) ──────────────────────────────────────────────────────
    case 0x24: { uint8_t d = spcFetch(); spcA_ &= spcRead(dpAddr(d)); setNZ(spcA_); break; }
    // ── OR A, dp ($04) ───────────────────────────────────────────────────────
    case 0x04: { uint8_t d = spcFetch(); spcA_ |= spcRead(dpAddr(d)); setNZ(spcA_); break; }
    // ── EOR A, dp ($44) ──────────────────────────────────────────────────────
    case 0x44: { uint8_t d = spcFetch(); spcA_ ^= spcRead(dpAddr(d)); setNZ(spcA_); break; }
    // ── ADC A, dp ($84) ──────────────────────────────────────────────────────
    case 0x84: {
        uint8_t d = spcFetch(); uint8_t v = spcRead(dpAddr(d));
        uint16_t r = (uint16_t)(spcA_ + v + (spcPSW_ & FL_C));
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_V|FL_H|FL_Z|FL_C))
            | (r > 0xFF ? FL_C : 0) | ((r&0xFF)==0 ? FL_Z : 0) | (r&0x80 ? FL_N : 0)
            | (((spcA_^r)&(v^r)&0x80) ? FL_V : 0));
        spcA_ = (uint8_t)r; break;
    }
    // ── SBC A, dp ($A4) ──────────────────────────────────────────────────────
    case 0xA4: {
        uint8_t d = spcFetch(); uint8_t v = spcRead(dpAddr(d));
        uint16_t r = (uint16_t)(spcA_ - v - !(spcPSW_ & FL_C));
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_V|FL_H|FL_Z|FL_C))
            | (spcA_ >= (uint16_t)(v + !(spcPSW_&FL_C)) ? FL_C : 0)
            | ((r&0xFF)==0 ? FL_Z : 0) | (r&0x80 ? FL_N : 0)
            | (((spcA_^v)&(spcA_^r)&0x80) ? FL_V : 0));
        spcA_ = (uint8_t)r; break;
    }

    // ── ASL dp ($0B) ─────────────────────────────────────────────────────────
    case 0x0B: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d); uint8_t v = spcRead(a);
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (v >> 7));
        v <<= 1; spcWrite(a, v); setNZ(v); break;
    }
    // ── LSR dp ($4B) ─────────────────────────────────────────────────────────
    case 0x4B: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d); uint8_t v = spcRead(a);
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (v & 1));
        v >>= 1; spcWrite(a, v); setNZ(v); break;
    }
    // ── ROL dp ($2B) ─────────────────────────────────────────────────────────
    case 0x2B: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d); uint8_t v = spcRead(a);
        uint8_t c = spcPSW_ & FL_C;
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (v >> 7));
        v = (uint8_t)((v << 1) | c); spcWrite(a, v); setNZ(v); break;
    }
    // ── ROR dp ($6B) ─────────────────────────────────────────────────────────
    case 0x6B: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d); uint8_t v = spcRead(a);
        uint8_t c = spcPSW_ & FL_C;
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (v & 1));
        v = (uint8_t)((v >> 1) | (c << 7)); spcWrite(a, v); setNZ(v); break;
    }

    // ── CBNE dp+X, rel ($DE) ─────────────────────────────────────────────────
    case 0xDE: {
        uint8_t d = spcFetch(); int8_t off = (int8_t)spcFetch();
        if (spcA_ != spcRead(dpAddr((uint8_t)(d+spcX_)))) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── MOV !abs+X, A ($D5) ──────────────────────────────────────────────────
    case 0xD5: { uint16_t a = absAddr(); spcWrite((uint16_t)(a + spcX_), spcA_); break; }
    // ── MOV !abs+Y, A ($D6) ──────────────────────────────────────────────────
    case 0xD6: { uint16_t a = absAddr(); spcWrite((uint16_t)(a + spcY_), spcA_); break; }
    // ── MOV A, !abs+X ($F5) ──────────────────────────────────────────────────
    case 0xF5: { uint16_t a = absAddr(); spcA_ = spcRead((uint16_t)(a + spcX_)); setNZ(spcA_); break; }
    // ── MOV A, !abs+Y ($F6) ──────────────────────────────────────────────────
    case 0xF6: { uint16_t a = absAddr(); spcA_ = spcRead((uint16_t)(a + spcY_)); setNZ(spcA_); break; }
    // ── MOV X, !abs ($E9) ─────────────────────────────────────────────────────
    case 0xE9: { uint16_t a = absAddr(); spcX_ = spcRead(a); setNZ(spcX_); break; }
    // ── MOV !abs, Y ($CC) ─────────────────────────────────────────────────────
    case 0xCC: { uint16_t a = absAddr(); spcWrite(a, spcY_); break; }
    // ── MOV X, dp+Y ($F9) ────────────────────────────────────────────────────
    case 0xF9: { uint8_t d = spcFetch(); spcX_ = spcRead(dpAddr((uint8_t)(d+spcY_))); setNZ(spcX_); break; }
    // ── MOV dp+Y, A ($D9) ─ store A (не X!) ──────────────────────────────────
    case 0xD9: { uint8_t d = spcFetch(); spcWrite(dpAddr((uint8_t)(d+spcY_)), spcA_); break; }
    // ── CMP X, !abs ($1E) ────────────────────────────────────────────────────
    case 0x1E: {
        uint16_t a = absAddr(); uint8_t v = spcRead(a);
        uint8_t r = (uint8_t)(spcX_ - v);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcX_ >= v ? FL_C : 0) | (r == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        break;
    }
    // ── CMP Y, !abs ($5E) ────────────────────────────────────────────────────
    case 0x5E: {
        uint16_t a = absAddr(); uint8_t v = spcRead(a);
        uint8_t r = (uint8_t)(spcY_ - v);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcY_ >= v ? FL_C : 0) | (r == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        break;
    }
    // ── CMP X, dp ($3E) ──────────────────────────────────────────────────────
    case 0x3E: {
        uint8_t d = spcFetch(); uint8_t v = spcRead(dpAddr(d));
        uint8_t r = (uint8_t)(spcX_ - v);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcX_ >= v ? FL_C : 0) | (r == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        break;
    }
    // ── CMP A, !abs ($65) ────────────────────────────────────────────────────
    case 0x65: {
        uint16_t a = absAddr(); uint8_t v = spcRead(a);
        uint8_t r = (uint8_t)(spcA_ - v);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcA_ >= v ? FL_C : 0) | (r == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        break;
    }
    // ── INCW dp ($3A) / DECW dp ($1A) — 16-битные dp-инкременты ─────────────
    case 0x3A: {
        uint8_t d = spcFetch();
        uint16_t v = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        v = (uint16_t)(v + 1);
        spcWrite(dpAddr(d), (uint8_t)(v & 0xFF));
        spcWrite(dpAddr((uint8_t)(d+1)), (uint8_t)(v >> 8));
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z))
            | (v == 0 ? FL_Z : 0) | (v & 0x8000 ? FL_N : 0));
        break;
    }
    case 0x1A: {
        uint8_t d = spcFetch();
        uint16_t v = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        v = (uint16_t)(v - 1);
        spcWrite(dpAddr(d), (uint8_t)(v & 0xFF));
        spcWrite(dpAddr((uint8_t)(d+1)), (uint8_t)(v >> 8));
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z))
            | (v == 0 ? FL_Z : 0) | (v & 0x8000 ? FL_N : 0));
        break;
    }
    // ── PCALL u ($4F) — call $FFxx ────────────────────────────────────────
    case 0x4F: {
        uint8_t u = spcFetch();
        spcPush((uint8_t)(spcPC_ >> 8));
        spcPush((uint8_t)(spcPC_));
        spcPC_ = (uint16_t)(0xFF00 | u);
        break;
    }
    // ── BVC/BVS ($50/$70) ─────────────────────────────────────────────────
    case 0x50: { int8_t off = (int8_t)spcFetch(); if (!(spcPSW_ & FL_V)) spcPC_ = (uint16_t)(spcPC_ + off); break; }
    case 0x70: { int8_t off = (int8_t)spcFetch(); if   (spcPSW_ & FL_V)  spcPC_ = (uint16_t)(spcPC_ + off); break; }
    // ── BCC уже есть как $90, BCS как $B0 ─────────────────────────────────
    // ── ADC dp, dp ($89) / SBC dp, dp ($A9) ───────────────────────────────
    case 0x89: {
        uint8_t s = spcFetch(); uint8_t d = spcFetch();
        uint8_t vs = spcRead(dpAddr(s)); uint8_t vd = spcRead(dpAddr(d));
        uint16_t r = (uint16_t)(vd + vs + (spcPSW_ & FL_C));
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_V|FL_H|FL_Z|FL_C))
            | (r > 0xFF ? FL_C : 0) | ((r & 0xFF) == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        spcWrite(dpAddr(d), (uint8_t)r);
        break;
    }
    case 0xA9: {
        uint8_t s = spcFetch(); uint8_t d = spcFetch();
        uint8_t vs = spcRead(dpAddr(s)); uint8_t vd = spcRead(dpAddr(d));
        uint16_t r = (uint16_t)(vd - vs - !(spcPSW_ & FL_C));
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_V|FL_H|FL_Z|FL_C))
            | (vd >= (uint16_t)(vs + !(spcPSW_ & FL_C)) ? FL_C : 0)
            | ((r & 0xFF) == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        spcWrite(dpAddr(d), (uint8_t)r);
        break;
    }
    // ── OR/AND/EOR/CMP/ADC/SBC (X),(Y) ─────────────────────────────────────
    case 0x19: { // OR (X), (Y): mem[X] |= mem[Y]
        uint16_t ax = dpAddr(spcX_), ay = dpAddr(spcY_);
        uint8_t v = (uint8_t)(spcRead(ax) | spcRead(ay));
        spcWrite(ax, v); setNZ(v);
        break;
    }
    case 0x39: {
        uint16_t ax = dpAddr(spcX_), ay = dpAddr(spcY_);
        uint8_t v = (uint8_t)(spcRead(ax) & spcRead(ay));
        spcWrite(ax, v); setNZ(v);
        break;
    }
    case 0x59: {
        uint16_t ax = dpAddr(spcX_), ay = dpAddr(spcY_);
        uint8_t v = (uint8_t)(spcRead(ax) ^ spcRead(ay));
        spcWrite(ax, v); setNZ(v);
        break;
    }
    case 0x79: {
        uint8_t vx = spcRead(dpAddr(spcX_)), vy = spcRead(dpAddr(spcY_));
        uint8_t r = (uint8_t)(vx - vy);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (vx >= vy ? FL_C : 0) | (r == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        break;
    }
    // ── RETI ($7F) ────────────────────────────────────────────────────────
    case 0x7F: {
        spcPSW_ = spcPop();
        uint8_t lo = spcPop(), hi = spcPop();
        spcPC_ = (uint16_t)(lo | (hi << 8));
        break;
    }
    // ── NOP-like для редких неизвестных опкодов ───────────────────────────
    // Неизвестный опкод — пропускаем (NOP-like)
    default:
        break;
    }
}
