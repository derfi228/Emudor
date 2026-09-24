// superfx.cpp — GSU (SuperFX): ядро с конвейером, кэш, буферы памяти, плоттер.
#include "superfx.h"
#include "console/state_io.h"
#include <algorithm>
#include <cstring>

namespace {
// Пока ПЗУ занято GSU, CPU вместо него видит этот узор. Векторы прерываний
// ($FFE0-$FFFF) тогда указывают в WRAM $0100-$010F — туда игры заранее кладут
// обработчики, чтобы NMI не лез в недоступное ПЗУ.
constexpr uint8_t kLockedVector[16] = {
    0x00, 0x01, 0x00, 0x01, 0x04, 0x01, 0x00, 0x01,
    0x00, 0x01, 0x08, 0x01, 0x00, 0x01, 0x0C, 0x01,
};
} // namespace

uint8_t SuperFX::lockedRomByte(uint32_t addr) { return kLockedVector[addr & 15]; }

// ─── Подключение / сброс ─────────────────────────────────────────────────────
void SuperFX::connect(const std::vector<uint8_t>* rom, uint32_t ramBytes)
{
    rom_ = rom;
    // Адрес ОЗУ на железе просто маскируется, поэтому размер — степень двойки.
    uint32_t size = 1;
    while (size < ramBytes) size <<= 1;
    ram_.assign(size, 0);
    ramMask_ = size - 1;
    reset();
}

void SuperFX::reset()
{
    std::memset(r_, 0, sizeof r_);
    sfr_ = 0; pbr_ = 0; rombr_ = 0; rambr_ = 0; cbr_ = 0;
    scbr_ = 0; scmr_ = 0; colr_ = 0; por_ = 0; bramr_ = 0;
    vcr_ = 0x04; cfgr_ = 0; clsr_ = 0;
    pipeline_ = 0x01;                 // NOP: первая «команда» после старта
    sreg_ = dreg_ = 0;
    r14Mod_ = r15Mod_ = false;
    ramaddr_ = 0;
    romcl_ = 0; romdr_ = 0;
    ramcl_ = 0; ramar_ = 0; ramdr_ = 0;
    cache_.fill(0);
    flushCache();
    for (auto& p : pixcache_) p = PixelCache{};
    budget_ = 0; clocks_ = 0; instructions_ = 0;
}

// ─── Шина GSU ────────────────────────────────────────────────────────────────
// $00-$3F: ПЗУ по схеме LoROM (обе половины банка — одни и те же 32 КБ);
// $40-$5F: ПЗУ линейно по 64 КБ; $60-$7F: ОЗУ картриджа.
uint8_t SuperFX::romByte(uint32_t offset) const
{
    if (!rom_ || rom_->empty()) return 0;
    return (*rom_)[offset % rom_->size()];
}

uint8_t SuperFX::busRead(uint32_t addr) const
{
    addr &= 0xFFFFFF;
    if ((addr & 0xC00000) == 0x000000)
        return romByte(((addr & 0x3F0000) >> 1) | (addr & 0x7FFF));
    if ((addr & 0xE00000) == 0x400000)
        return romByte(addr & 0x1FFFFF);
    if ((addr & 0xE00000) == 0x600000)
        return ram_[addr & ramMask_];
    return 0;
}

void SuperFX::busWrite(uint32_t addr, uint8_t data)
{
    if ((addr & 0xE00000) == 0x600000) ram_[addr & ramMask_] = data;
}

// ─── Время и отложенные операции с памятью ──────────────────────────────────
// Чтение ПЗУ по R14 и запись в ОЗУ идут «в фоне»: железо завершает их через
// 5-6 тактов, а команда, которой нужен результат, ждёт (sync*).
void SuperFX::tick(uint32_t clocks)
{
    if (romcl_) {
        romcl_ -= std::min(clocks, romcl_);
        if (romcl_ == 0) {
            sfr_ &= (uint16_t)~SFR_R;
            romdr_ = busRead(((uint32_t)rombr_ << 16) + r_[14]);
        }
    }
    if (ramcl_) {
        ramcl_ -= std::min(clocks, ramcl_);
        if (ramcl_ == 0) busWrite(0x700000u + ((uint32_t)rambr_ << 16) + ramar_, ramdr_);
    }
    clocks_ += clocks;
}

void SuperFX::updateRomBuffer()
{
    sfr_ |= SFR_R;
    romcl_ = clsr_ ? 5u : 6u;
}

uint8_t SuperFX::readRamBuffer(uint16_t addr)
{
    syncRamBuffer();
    return busRead(0x700000u + ((uint32_t)rambr_ << 16) + addr);
}

void SuperFX::writeRamBuffer(uint16_t addr, uint8_t data)
{
    syncRamBuffer();
    ramcl_ = clsr_ ? 5u : 6u;
    ramar_ = addr;
    ramdr_ = data;
}

// ─── Выборка команд: кэш 512 байт от CBR, иначе ПЗУ/ОЗУ ─────────────────────
uint8_t SuperFX::readOpcode(uint16_t addr)
{
    uint16_t offset = (uint16_t)(addr - cbr_);
    if (offset < 512) {
        if (!cacheValid_[offset >> 4]) {
            // Промах: строка кэша (16 байт) грузится целиком.
            uint16_t dp = (uint16_t)(offset & 0xFFF0);
            uint32_t sp = ((uint32_t)pbr_ << 16) + ((uint16_t)(cbr_ + dp) & 0xFFF0);
            for (int n = 0; n < 16; ++n) {
                tick(clsr_ ? 5u : 6u);
                cache_[dp++] = busRead(sp++);
            }
            cacheValid_[offset >> 4] = true;
        } else {
            tick(clsr_ ? 1u : 2u);
        }
        return cache_[offset];
    }
    if (pbr_ <= 0x5F) syncRomBuffer(); else syncRamBuffer();
    tick(clsr_ ? 5u : 6u);
    return busRead(((uint32_t)pbr_ << 16) | addr);
}

// Взять байт-операнд из конвейера и выбрать следующий.
uint8_t SuperFX::pipe()
{
    uint8_t result = pipeline_;
    ++r_[15];
    pipeline_ = readOpcode(r_[15]);
    r15Mod_ = false;
    return result;
}

// ─── Регистры и префиксы ─────────────────────────────────────────────────────
void SuperFX::setR(int n, uint16_t v)
{
    r_[n & 15] = v;
    if (n == 14) r14Mod_ = true;        // запустить чтение ПЗУ по новому адресу
    else if (n == 15) r15Mod_ = true;   // переход: не инкрементировать R15
}

void SuperFX::resetPrefix()
{
    sfr_ &= (uint16_t)~(SFR_B | SFR_ALT1 | SFR_ALT2);
    sreg_ = dreg_ = 0;
}

// ─── Исполнение ──────────────────────────────────────────────────────────────
void SuperFX::run(uint32_t masterClocks)
{
    if (!(sfr_ & SFR_G)) {
        // Остановленный чип всё равно дозавершает начатые операции с памятью.
        clocks_ = 0;
        if (romcl_ || ramcl_) tick(masterClocks);
        budget_ = 0;
        return;
    }
    budget_ += masterClocks;
    while (budget_ > 0 && (sfr_ & SFR_G)) {
        clocks_ = 0;
        step();
        budget_ -= clocks_;
    }
    if (!(sfr_ & SFR_G)) budget_ = 0;
}

// Одна команда. Исполняется байт из конвейера, а на его место уже выбирается
// байт по R15 — поэтому команда после перехода успевает исполниться.
void SuperFX::step()
{
    uint8_t op = pipeline_;
    pipeline_ = readOpcode(r_[15]);
    r15Mod_ = false;
    execute(op);
    ++instructions_;
    if (r14Mod_) { r14Mod_ = false; updateRomBuffer(); }
    if (r15Mod_) r15Mod_ = false;
    else         ++r_[15];
}

// Условный переход: смещение относительно адреса ПОСЛЕ команды перехода.
// Префиксы переход не сбрасывает.
void SuperFX::branch(bool take)
{
    int8_t disp = (int8_t)pipe();
    if (take) setR(15, (uint16_t)(r_[15] + disp));
}

void SuperFX::execute(uint8_t op)
{
    const int n = op & 0x0F;
    const bool fz  = (sfr_ & SFR_Z)  != 0;
    const bool fc  = (sfr_ & SFR_CY) != 0;
    const bool fs  = (sfr_ & SFR_S)  != 0;
    const bool fov = (sfr_ & SFR_OV) != 0;

    switch (op >> 4) {
    // ── $00-$0F: служебные и переходы ────────────────────────────────────────
    case 0x0:
        switch (n) {
        case 0x0: {   // STOP
            if (!(cfgr_ & 0x80)) sfr_ |= SFR_IRQ;
            sfr_ &= (uint16_t)~SFR_G;
            pipeline_ = 0x01;
            resetPrefix();
            // Досбрасываем недописанное: CPU сейчас пойдёт читать результат.
            syncRamBuffer();
            flushPixelCache(pixcache_[1]);
            flushPixelCache(pixcache_[0]);
            break;
        }
        case 0x1: resetPrefix(); break;                    // NOP
        case 0x2:                                          // CACHE
            if (cbr_ != (r_[15] & 0xFFF0)) { cbr_ = (uint16_t)(r_[15] & 0xFFF0); flushCache(); }
            resetPrefix();
            break;
        case 0x3: {                                        // LSR
            uint16_t s = sr();
            setFlag(SFR_CY, s & 1);
            uint16_t v = (uint16_t)(s >> 1);
            setDr(v); setSZ(v); resetPrefix();
            break;
        }
        case 0x4: {                                        // ROL
            uint16_t s = sr();
            uint16_t v = (uint16_t)((s << 1) | (fc ? 1 : 0));
            setDr(v); setSZ(v); setFlag(SFR_CY, (s & 0x8000) != 0); resetPrefix();
            break;
        }
        case 0x5: branch(true);        break;  // BRA
        case 0x6: branch(fs == fov);   break;  // BGE
        case 0x7: branch(fs != fov);   break;  // BLT
        case 0x8: branch(!fz);         break;  // BNE
        case 0x9: branch(fz);          break;  // BEQ
        case 0xA: branch(!fs);         break;  // BPL
        case 0xB: branch(fs);          break;  // BMI
        case 0xC: branch(!fc);         break;  // BCC
        case 0xD: branch(fc);          break;  // BCS
        case 0xE: branch(!fov);        break;  // BVC
        case 0xF: branch(fov);         break;  // BVS
        }
        break;

    // ── $10-$1F: TO Rn, после WITH — MOVE Rn ─────────────────────────────────
    case 0x1:
        if (!(sfr_ & SFR_B)) dreg_ = (uint8_t)n;
        else { setR(n, sr()); resetPrefix(); }
        break;

    // ── $20-$2F: WITH Rn ─────────────────────────────────────────────────────
    case 0x2:
        sreg_ = dreg_ = (uint8_t)n;
        sfr_ |= SFR_B;
        break;

    // ── $30-$3F: STW/STB (Rn), LOOP, ALT1-3 ──────────────────────────────────
    case 0x3:
        if (n <= 0xB) {
            ramaddr_ = r_[n];
            writeRamBuffer(ramaddr_, (uint8_t)sr());
            if (!alt1()) writeRamBuffer((uint16_t)(ramaddr_ ^ 1), (uint8_t)(sr() >> 8));
            resetPrefix();
        } else if (n == 0xC) {                             // LOOP
            setR(12, (uint16_t)(r_[12] - 1));
            setSZ(r_[12]);
            if (r_[12] != 0) setR(15, r_[13]);
            resetPrefix();
        } else if (n == 0xD) {                             // ALT1
            sfr_ &= (uint16_t)~SFR_B; sfr_ |= SFR_ALT1;
        } else if (n == 0xE) {                             // ALT2
            sfr_ &= (uint16_t)~SFR_B; sfr_ |= SFR_ALT2;
        } else {                                           // ALT3
            sfr_ &= (uint16_t)~SFR_B; sfr_ |= SFR_ALT1 | SFR_ALT2;
        }
        break;

    // ── $40-$4F: LDW/LDB (Rn), PLOT/RPIX, SWAP, COLOR/CMODE, NOT ──────────────
    case 0x4:
        if (n <= 0xB) {
            ramaddr_ = r_[n];
            uint16_t v = readRamBuffer(ramaddr_);
            if (!alt1()) v = (uint16_t)(v | (readRamBuffer((uint16_t)(ramaddr_ ^ 1)) << 8));
            setDr(v);
            resetPrefix();
        } else if (n == 0xC) {
            if (!alt1()) {                                 // PLOT
                plot((uint8_t)r_[1], (uint8_t)r_[2]);
                setR(1, (uint16_t)(r_[1] + 1));
            } else {                                       // RPIX
                uint16_t v = rpix((uint8_t)r_[1], (uint8_t)r_[2]);
                setDr(v); setSZ(v);
            }
            resetPrefix();
        } else if (n == 0xD) {                             // SWAP
            uint16_t s = sr();
            uint16_t v = (uint16_t)((s >> 8) | (s << 8));
            setDr(v); setSZ(v); resetPrefix();
        } else if (n == 0xE) {
            if (!alt1()) colr_ = color((uint8_t)sr());     // COLOR
            else         por_  = (uint8_t)sr();            // CMODE
            resetPrefix();
        } else {                                           // NOT
            uint16_t v = (uint16_t)~sr();
            setDr(v); setSZ(v); resetPrefix();
        }
        break;

    // ── $50-$5F: ADD / ADC / ADD #n / ADC #n ─────────────────────────────────
    case 0x5: {
        uint32_t s = sr();
        uint32_t b = alt2() ? (uint32_t)n : r_[n];
        uint32_t r = s + b + ((alt1() && fc) ? 1u : 0u);
        setFlag(SFR_OV, (~(s ^ b) & (b ^ r) & 0x8000) != 0);
        setFlag(SFR_CY, r >= 0x10000);
        setDr((uint16_t)r); setSZ((uint16_t)r); resetPrefix();
        break;
    }

    // ── $60-$6F: SUB / SBC / SUB #n / CMP ────────────────────────────────────
    case 0x6: {
        const bool a1 = alt1(), a2 = alt2();
        int32_t s = sr();
        int32_t b = (!a2 || a1) ? (int32_t)r_[n] : n;     // ALT3 (CMP) — регистр
        int32_t r = s - b - ((!a2 && a1 && !fc) ? 1 : 0); // ALT1 — SBC
        setFlag(SFR_OV, ((s ^ b) & (s ^ r) & 0x8000) != 0);
        setFlag(SFR_CY, r >= 0);
        setSZ((uint16_t)r);
        if (!a2 || !a1) setDr((uint16_t)r);               // CMP результат не пишет
        resetPrefix();
        break;
    }

    // ── $70: MERGE, $71-$7F: AND / BIC / AND #n / BIC #n ─────────────────────
    case 0x7:
        if (n == 0) {
            uint16_t v = (uint16_t)((r_[7] & 0xFF00) | (r_[8] >> 8));
            setDr(v);
            setFlag(SFR_OV, (v & 0xC0C0) != 0);
            setFlag(SFR_S,  (v & 0x8080) != 0);
            setFlag(SFR_CY, (v & 0xE0E0) != 0);
            setFlag(SFR_Z,  (v & 0xF0F0) != 0);
        } else {
            uint16_t b = alt2() ? (uint16_t)n : r_[n];
            uint16_t v = (uint16_t)(sr() & (alt1() ? (uint16_t)~b : b));
            setDr(v); setSZ(v);
        }
        resetPrefix();
        break;

    // ── $80-$8F: MULT / UMULT (8×8) ──────────────────────────────────────────
    case 0x8: {
        uint16_t b = alt2() ? (uint16_t)n : r_[n];
        uint16_t v = !alt1()
            ? (uint16_t)((int16_t)(int8_t)sr() * (int16_t)(int8_t)b)
            : (uint16_t)((uint16_t)(uint8_t)sr() * (uint16_t)(uint8_t)b);
        setDr(v); setSZ(v); resetPrefix();
        tick((cfgr_ & 0x20) ? 2u : 4u);
        break;
    }

    // ── $90-$9F: SBK, LINK, SEX, ASR/DIV2, ROR, JMP/LJMP, LOB, FMULT/LMULT ───
    case 0x9:
        if (n == 0x0) {                                    // SBK
            writeRamBuffer(ramaddr_, (uint8_t)sr());
            writeRamBuffer((uint16_t)(ramaddr_ ^ 1), (uint8_t)(sr() >> 8));
            resetPrefix();
        } else if (n <= 0x4) {                             // LINK #n
            setR(11, (uint16_t)(r_[15] + n));
            resetPrefix();
        } else if (n == 0x5) {                             // SEX
            uint16_t v = (uint16_t)(int16_t)(int8_t)sr();
            setDr(v); setSZ(v); resetPrefix();
        } else if (n == 0x6) {                             // ASR / DIV2
            uint16_t s = sr();
            setFlag(SFR_CY, s & 1);
            // DIV2 отличается только тем, что -1/2 даёт 0, а не -1.
            uint16_t v = (uint16_t)(((int16_t)s >> 1) + (alt1() ? (((uint32_t)s + 1) >> 16) : 0));
            setDr(v); setSZ(v); resetPrefix();
        } else if (n == 0x7) {                             // ROR
            uint16_t s = sr();
            uint16_t v = (uint16_t)((fc ? 0x8000 : 0) | (s >> 1));
            setDr(v); setSZ(v); setFlag(SFR_CY, s & 1); resetPrefix();
        } else if (n <= 0xD) {
            if (!alt1()) {                                 // JMP Rn
                setR(15, r_[n]);
            } else {                                       // LJMP Rn: банк из Rn, адрес из Sreg
                pbr_ = (uint8_t)(r_[n] & 0x7F);
                setR(15, sr());
                cbr_ = (uint16_t)(r_[15] & 0xFFF0);
                flushCache();
            }
            resetPrefix();
        } else if (n == 0xE) {                             // LOB
            uint16_t v = (uint16_t)(sr() & 0xFF);
            setDr(v);
            setFlag(SFR_S, (v & 0x80) != 0); setFlag(SFR_Z, v == 0);
            resetPrefix();
        } else {                                           // FMULT / LMULT
            uint32_t res = (uint32_t)((int32_t)(int16_t)sr() * (int32_t)(int16_t)r_[6]);
            if (alt1()) setR(4, (uint16_t)res);
            uint16_t v = (uint16_t)(res >> 16);
            setDr(v);
            setFlag(SFR_S, (res & 0x80000000u) != 0);
            setFlag(SFR_CY, (res & 0x8000) != 0);
            setFlag(SFR_Z, v == 0);
            resetPrefix();
            tick(((cfgr_ & 0x20) ? 3u : 7u) * (clsr_ ? 1u : 2u));
        }
        break;

    // ── $A0-$AF: IBT Rn,#pp / LMS Rn,(yy) / SMS (yy),Rn ──────────────────────
    case 0xA:
        if (alt1()) {
            ramaddr_ = (uint16_t)(pipe() << 1);
            uint8_t lo = readRamBuffer(ramaddr_);
            setR(n, (uint16_t)((readRamBuffer((uint16_t)(ramaddr_ ^ 1)) << 8) | lo));
        } else if (alt2()) {
            ramaddr_ = (uint16_t)(pipe() << 1);
            writeRamBuffer(ramaddr_, (uint8_t)r_[n]);
            writeRamBuffer((uint16_t)(ramaddr_ ^ 1), (uint8_t)(r_[n] >> 8));
        } else {
            setR(n, (uint16_t)(int16_t)(int8_t)pipe());
        }
        resetPrefix();
        break;

    // ── $B0-$BF: FROM Rn, после WITH — MOVES Rn ──────────────────────────────
    case 0xB:
        if (!(sfr_ & SFR_B)) {
            sreg_ = (uint8_t)n;
        } else {
            uint16_t v = r_[n];
            setDr(v);
            setFlag(SFR_OV, (v & 0x80) != 0);
            setSZ(v);
            resetPrefix();
        }
        break;

    // ── $C0: HIB, $C1-$CF: OR / XOR / OR #n / XOR #n ─────────────────────────
    case 0xC:
        if (n == 0) {
            uint16_t v = (uint16_t)(sr() >> 8);
            setDr(v);
            setFlag(SFR_S, (v & 0x80) != 0); setFlag(SFR_Z, v == 0);
        } else {
            uint16_t b = alt2() ? (uint16_t)n : r_[n];
            uint16_t v = alt1() ? (uint16_t)(sr() ^ b) : (uint16_t)(sr() | b);
            setDr(v); setSZ(v);
        }
        resetPrefix();
        break;

    // ── $D0-$DE: INC Rn, $DF: GETC / RAMB / ROMB ─────────────────────────────
    case 0xD:
        if (n <= 0xE) {
            setR(n, (uint16_t)(r_[n] + 1));
            setSZ(r_[n]);
        } else if (!alt2()) {                              // GETC (и с ALT1)
            colr_ = color(readRomBuffer());
        } else if (!alt1()) {                              // ALT2: RAMB
            syncRamBuffer();
            rambr_ = (uint8_t)(sr() & 0x01);
        } else {                                           // ALT3: ROMB
            syncRomBuffer();
            rombr_ = (uint8_t)(sr() & 0x7F);
        }
        resetPrefix();
        break;

    // ── $E0-$EE: DEC Rn, $EF: GETB / GETBH / GETBL / GETBS ───────────────────
    case 0xE:
        if (n <= 0xE) {
            setR(n, (uint16_t)(r_[n] - 1));
            setSZ(r_[n]);
        } else {
            uint16_t v = 0;
            switch ((alt2() ? 2 : 0) | (alt1() ? 1 : 0)) {
            case 0: v = readRomBuffer(); break;
            case 1: v = (uint16_t)((readRomBuffer() << 8) | (uint8_t)sr()); break;
            case 2: v = (uint16_t)((sr() & 0xFF00) | readRomBuffer()); break;
            case 3: v = (uint16_t)(int16_t)(int8_t)readRomBuffer(); break;
            }
            setDr(v);
        }
        resetPrefix();
        break;

    // ── $F0-$FF: IWT Rn,#xx / LM Rn,(xx) / SM (xx),Rn ────────────────────────
    case 0xF:
        if (alt1() || alt2()) {
            uint16_t lo = pipe();
            ramaddr_ = (uint16_t)(lo | (pipe() << 8));
            if (alt1()) {
                uint8_t vlo = readRamBuffer(ramaddr_);
                setR(n, (uint16_t)((readRamBuffer((uint16_t)(ramaddr_ ^ 1)) << 8) | vlo));
            } else {
                writeRamBuffer(ramaddr_, (uint8_t)r_[n]);
                writeRamBuffer((uint16_t)(ramaddr_ ^ 1), (uint8_t)(r_[n] >> 8));
            }
        } else {
            uint16_t lo = pipe();
            setR(n, (uint16_t)(lo | (pipe() << 8)));
        }
        resetPrefix();
        break;
    }
}

// ─── Плоттер ─────────────────────────────────────────────────────────────────
// COLOR/GETC с опциями CMODE: взять старший полубайт источника или заморозить
// старший полубайт текущего цвета.
uint8_t SuperFX::color(uint8_t source) const
{
    if (por_ & 0x04) return (uint8_t)((colr_ & 0xF0) | (source >> 4));
    if (por_ & 0x08) return (uint8_t)((colr_ & 0xF0) | (source & 0x0F));
    return source;
}

// Глубина цвета из SCMR.MD: 0 → 2 бита, 1 → 4, 3 → 8 (2 ведёт себя как 4).
uint32_t SuperFX::bitsPerPixel() const
{
    uint32_t md = scmr_ & 0x03;
    return 2u << (md - (md >> 1));
}

// Адрес строки тайла в ОЗУ для пикселя (x, y). Экран GSU — столбцы тайлов:
// номер тайла растёт сначала вниз, высота 128/160/192 строки (SCMR.HT) или
// раскладка «как спрайты» 16×16 тайлов (CMODE бит 4 или HT = 3).
uint32_t SuperFX::charAddress(uint8_t x, uint8_t y) const
{
    uint32_t ht = ((scmr_ & 0x20) ? 2u : 0u) | ((scmr_ & 0x04) ? 1u : 0u);
    if (por_ & 0x10) ht = 3;
    uint32_t cn = 0;
    switch (ht) {
    case 0: cn = ((x & 0xF8u) << 1) + ((y & 0xF8u) >> 3); break;
    case 1: cn = ((x & 0xF8u) << 1) + ((x & 0xF8u) >> 1) + ((y & 0xF8u) >> 3); break;
    case 2: cn = ((x & 0xF8u) << 1) + (x & 0xF8u) + ((y & 0xF8u) >> 3); break;
    case 3: cn = ((y & 0x80u) << 2) + ((x & 0x80u) << 1) + ((y & 0x78u) << 1) + ((x & 0x78u) >> 3); break;
    }
    uint32_t bpp = bitsPerPixel();
    return 0x700000u + cn * (bpp << 3) + ((uint32_t)scbr_ << 10) + (uint32_t)(y & 7) * 2;
}

void SuperFX::plot(uint8_t x, uint8_t y)
{
    // Прозрачность (CMODE бит 0 = 0): нулевой цвет не рисуется.
    if (!(por_ & 0x01)) {
        if ((scmr_ & 0x03) == 3) {
            if (por_ & 0x08) { if ((colr_ & 0x0F) == 0) return; }
            else             { if (colr_ == 0) return; }
        } else {
            if ((colr_ & 0x0F) == 0) return;
        }
    }

    uint8_t c = colr_;
    if ((por_ & 0x02) && (scmr_ & 0x03) != 3) {        // дизеринг шахматкой
        if ((x ^ y) & 1) c >>= 4;
        c &= 0x0F;
    }

    uint16_t offset = (uint16_t)((y << 5) + (x >> 3));
    if (pixcache_[0].offset != offset) {
        flushPixelCache(pixcache_[1]);
        pixcache_[1] = pixcache_[0];
        pixcache_[0].bitpend = 0;
        pixcache_[0].offset = offset;
    }

    int bit = (x & 7) ^ 7;
    pixcache_[0].data[bit] = c;
    pixcache_[0].bitpend |= (uint8_t)(1 << bit);
    if (pixcache_[0].bitpend == 0xFF) {
        flushPixelCache(pixcache_[1]);
        pixcache_[1] = pixcache_[0];
        pixcache_[0].bitpend = 0;
    }
}

uint8_t SuperFX::rpix(uint8_t x, uint8_t y)
{
    flushPixelCache(pixcache_[1]);
    flushPixelCache(pixcache_[0]);

    uint32_t addr = charAddress(x, y);
    uint32_t bpp = bitsPerPixel();
    int bit = (x & 7) ^ 7;
    uint8_t data = 0;
    for (uint32_t n = 0; n < bpp; ++n) {
        uint32_t byte = ((n >> 1) << 4) + (n & 1);    // 0,1,16,17,32,33,48,49
        tick(clsr_ ? 5u : 6u);
        data |= (uint8_t)(((busRead(addr + byte) >> bit) & 1) << n);
    }
    return data;
}

// Записать 8 пикселей строки тайла в ОЗУ битовыми плоскостями. Если заданы
// не все 8, остальные биты берутся из того, что уже лежит в памяти.
void SuperFX::flushPixelCache(PixelCache& cache)
{
    if (cache.bitpend == 0) return;

    uint8_t x = (uint8_t)(cache.offset << 3);
    uint8_t y = (uint8_t)(cache.offset >> 5);
    uint32_t addr = charAddress(x, y);
    uint32_t bpp = bitsPerPixel();

    for (uint32_t n = 0; n < bpp; ++n) {
        uint32_t byte = ((n >> 1) << 4) + (n & 1);
        uint8_t data = 0;
        for (int px = 0; px < 8; ++px) data |= (uint8_t)(((cache.data[px] >> n) & 1) << px);
        if (cache.bitpend != 0xFF) {
            tick(clsr_ ? 5u : 6u);
            data &= cache.bitpend;
            data |= (uint8_t)(busRead(addr + byte) & ~cache.bitpend);
        }
        tick(clsr_ ? 5u : 6u);
        busWrite(addr + byte, data);
    }
    cache.bitpend = 0;
}

// ─── Регистры, видимые CPU ($3000-$34FF) ─────────────────────────────────────
uint8_t SuperFX::readIO(uint16_t addr)
{
    addr = (uint16_t)(0x3000 | (addr & 0x3FF));
    if (addr >= 0x3100 && addr <= 0x32FF)
        return cache_[(uint16_t)(addr - 0x3100 + cbr_) & 511];
    if (addr <= 0x301F)
        return (uint8_t)(r_[(addr >> 1) & 15] >> ((addr & 1) * 8));

    switch (addr) {
    case 0x3030: return (uint8_t)sfr_;
    case 0x3031: {                        // чтение снимает IRQ
        uint8_t v = (uint8_t)(sfr_ >> 8);
        sfr_ &= (uint16_t)~SFR_IRQ;
        return v;
    }
    case 0x3034: return pbr_;
    case 0x3036: return rombr_;
    case 0x303B: return vcr_;
    case 0x303C: return rambr_;
    case 0x303E: return (uint8_t)cbr_;
    case 0x303F: return (uint8_t)(cbr_ >> 8);
    }
    return 0;
}

void SuperFX::writeIO(uint16_t addr, uint8_t data)
{
    addr = (uint16_t)(0x3000 | (addr & 0x3FF));
    if (addr >= 0x3100 && addr <= 0x32FF) {
        // CPU может сам залить код в кэш: строка становится валидной, когда
        // записан её последний байт.
        uint16_t a = (uint16_t)(addr - 0x3100 + cbr_) & 511;
        cache_[a] = data;
        if ((a & 15) == 15) cacheValid_[a >> 4] = true;
        return;
    }
    if (addr <= 0x301F) {
        int n = (addr >> 1) & 15;
        if (addr & 1) r_[n] = (uint16_t)((r_[n] & 0x00FF) | (data << 8));
        else          r_[n] = (uint16_t)((r_[n] & 0xFF00) | data);
        if (n == 14) updateRomBuffer();
        if (addr == 0x301F) sfr_ |= SFR_G;   // запись старшего байта R15 — старт
        return;
    }

    switch (addr) {
    case 0x3030: {
        bool wasRunning = (sfr_ & SFR_G) != 0;
        sfr_ = (uint16_t)((sfr_ & 0xFF00) | data);
        if (wasRunning && !(sfr_ & SFR_G)) {  // CPU остановил чип
            cbr_ = 0;
            flushCache();
        }
        break;
    }
    case 0x3031: sfr_ = (uint16_t)((data << 8) | (sfr_ & 0x00FF)); break;
    case 0x3033: bramr_ = (uint8_t)(data & 0x01); break;
    case 0x3034: pbr_ = (uint8_t)(data & 0x7F); flushCache(); break;
    case 0x3037: cfgr_ = data; break;
    case 0x3038: scbr_ = data; break;
    case 0x3039: clsr_ = (uint8_t)(data & 0x01); break;
    case 0x303A: scmr_ = data; break;
    default: break;
    }
}

// ─── Save state ───────────────────────────────────────────────────────────────
template<class S> void SuperFX::serialize(S& s)
{
    s.io(r_); s.io(sfr_); s.io(pbr_); s.io(rombr_); s.io(rambr_); s.io(cbr_);
    s.io(scbr_); s.io(scmr_); s.io(colr_); s.io(por_); s.io(bramr_); s.io(vcr_);
    s.io(cfgr_); s.io(clsr_);
    s.io(pipeline_); s.io(sreg_); s.io(dreg_); s.io(r14Mod_); s.io(r15Mod_); s.io(ramaddr_);
    s.io(romcl_); s.io(romdr_); s.io(ramcl_); s.io(ramar_); s.io(ramdr_);
    s.io(cache_); s.io(cacheValid_); s.io(pixcache_);
    s.vec(ram_);
    s.io(budget_); s.io(clocks_); s.io(instructions_);
}
template void SuperFX::serialize<StateWriter>(StateWriter&);
template void SuperFX::serialize<StateReader>(StateReader&);
