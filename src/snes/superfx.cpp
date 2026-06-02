// superfx.cpp — интерпретатор GSU (SuperFX). Ядро + плоттинг пикселей.
#include "superfx.h"
#include <cstring>

// ─── Подключение / сброс ─────────────────────────────────────────────────────
void SuperFX::connect(const std::vector<uint8_t>* rom, uint32_t ramKB)
{
    rom_ = rom;
    ram_.assign((size_t)ramKB * 1024u, 0);
    reset();
}

void SuperFX::reset()
{
    std::memset(r_, 0, sizeof(r_));
    sfr_ = 0; pbr_ = 0; rombr_ = 0; rambr_ = 0; cbr_ = 0;
    scbr_ = 0; scmr_ = 0; colr_ = 0; por_ = 0; bramr_ = 0; cfgr_ = 0;
    clsr_ = 0;
    sreg_ = dreg_ = 0; alt_ = 0; b_ = false;
    romBuffer_ = romBufByte_ = 0;
    ramBuffer_[0] = ramBuffer_[1] = 0;
    irq_ = false;
    cache_.fill(0); cacheValid_.fill(false);
    pixValid_ = 0; pixX_ = pixY_ = 0xFFFF;
    std::memset(pixCache_, 0, sizeof(pixCache_));
}

// ─── Доступ к ROM/RAM ────────────────────────────────────────────────────────
uint8_t SuperFX::romReadByte(uint32_t addr) const
{
    if (!rom_ || rom_->empty()) return 0;
    // LoROM-маппинг SuperFX: банки $00-$5F → $8000-$FFFF, линейно
    // addr здесь — линейное ROM-смещение (24-бит pbr:offset уже преобразовано)
    return (*rom_)[addr % rom_->size()];
}

uint8_t SuperFX::ramReadByte(uint32_t addr) const
{
    if (ram_.empty()) return 0;
    return ram_[addr % ram_.size()];
}
void SuperFX::ramWriteByte(uint32_t addr, uint8_t v)
{
    if (!ram_.empty()) ram_[addr % ram_.size()] = v;
}

uint8_t SuperFX::readRam(uint32_t offset) const  { return ramReadByte(offset); }
void    SuperFX::writeRam(uint32_t offset, uint8_t data) { ramWriteByte(offset, data); }

// ─── Чтение кода (PBR:PC) с использованием кэша ──────────────────────────────
uint8_t SuperFX::readCode(uint32_t pbrPC)
{
    uint8_t pbr = (uint8_t)(pbrPC >> 16);
    uint16_t pc = (uint16_t)pbrPC;
    // Кэш: 512 байт, базируется на CBR. Если адрес попал в кэш-окно и валиден.
    if (pc >= cbr_ && pc < (uint16_t)(cbr_ + 512)) {
        uint16_t off = (uint16_t)(pc - cbr_);
        if (cacheValid_[off >> 4]) return cache_[off];
    }
    // Иначе читаем из ROM/RAM
    if (pbr <= 0x5F) {
        // LoROM: pbr банк, pc $8000-$FFFF → линейно; $0000-$7FFF тоже мапится
        uint32_t lin = (uint32_t)((pbr & 0x7F) * 0x8000u + (pc & 0x7FFF));
        return romReadByte(lin);
    } else {
        // банки $70+ → RAM
        return ramReadByte((uint32_t)((pbr - 0x70) * 0x10000u + pc));
    }
}

uint8_t SuperFX::fetchOpcode()
{
    uint32_t a = (uint32_t)(pbr_ << 16) | r_[15];
    uint8_t op = readCode(a);
    ++r_[15];
    return op;
}

// ─── Регистры источник/приёмник с учётом префиксов ───────────────────────────
uint16_t& SuperFX::src() { return r_[sreg_ & 15]; }
uint16_t& SuperFX::dst() { return r_[dreg_ & 15]; }

void SuperFX::setZS16(uint16_t v)
{
    sfr_ &= ~(SFR_Z | SFR_S);
    if (v == 0)       sfr_ |= SFR_Z;
    if (v & 0x8000)   sfr_ |= SFR_S;
}

void SuperFX::writeDst(uint16_t v)
{
    r_[dreg_ & 15] = v;
    if ((dreg_ & 15) == 15) { /* запись в PC = переход */ }
}

void SuperFX::resetPrefix()
{
    sreg_ = 0; dreg_ = 0; alt_ = 0; b_ = false;
    sfr_ &= ~(SFR_ALT1 | SFR_ALT2 | SFR_B);
}

// ─── PLOT: записать colr_ в пиксельный кэш на (R1,R2) ────────────────────────
// SCMR биты определяют bpp (2/4/8). Кэш — колонка 8 пикселей.
void SuperFX::flushPixCache()
{
    if (pixValid_ == 0) { return; }
    // Преобразуем 8-пиксельный кэш в bitplanes и пишем в game-pak RAM
    // по адресу, вычисленному из SCBR/SCMR. Формат — char-based как у SNES.
    int bpp = 4;
    switch (scmr_ & 0x03) {        // height bits + color
        default: break;
    }
    // Определяем bpp по битам 0,1,4 SCMR (упрощённо): 00->4bpp common
    uint8_t md = (uint8_t)(((scmr_ >> 2) & 1) | ((scmr_ >> 4) & 2));
    if (md == 0) bpp = 4; else if (md == 1) bpp = 4; else if (md == 2) bpp = 8; else bpp = 2;

    uint16_t x = pixX_, y = pixY_;
    // Адрес char в RAM: 16x16 char-плоскость, SCBR*0x400 базовый
    uint32_t charX = x >> 3, charY = y >> 3;
    uint32_t base  = (uint32_t)scbr_ << 10;
    // screen width: бит 5 SCMR / por
    uint32_t charsPerRow = 32;
    uint32_t charNo = charY * charsPerRow + charX;
    uint32_t charBytes = (uint32_t)(bpp * 8);
    uint32_t addr = base + charNo * charBytes + (y & 7) * 2;

    int planes = bpp / 2;
    for (int p = 0; p < planes; ++p) {
        uint8_t lo = 0, hi = 0;
        for (int px = 0; px < 8; ++px) {
            if (!((pixValid_ >> px) & 1)) continue;
            uint8_t c = pixCache_[px];
            uint8_t bit0 = (c >> (p * 2)) & 1;
            uint8_t bit1 = (c >> (p * 2 + 1)) & 1;
            if (bit0) lo |= (uint8_t)(0x80 >> px);
            if (bit1) hi |= (uint8_t)(0x80 >> px);
        }
        uint32_t pa = addr + (uint32_t)p * 16;
        ramWriteByte(pa,     (uint8_t)(ramReadByte(pa)     | lo));
        ramWriteByte(pa + 1, (uint8_t)(ramReadByte(pa + 1) | hi));
    }
    pixValid_ = 0;
}

void SuperFX::plot()
{
    uint16_t x = r_[1], y = r_[2];
    // Если новая колонка/строка — сбрасываем кэш предыдущей
    if ((x >> 3) != (pixX_ >> 3) || (y) != (pixY_) ) {
        if (pixX_ != 0xFFFF) flushPixCache();
        pixX_ = x; pixY_ = y;
        pixValid_ = 0;
    }
    uint8_t col = colr_;
    // POR бит0: transparent (color 0 не рисуется), если не задано иначе
    bool transparent = (col == 0) && !(por_ & 0x08);
    if (!transparent) {
        pixCache_[x & 7] = col;
        pixValid_ |= (uint8_t)(0x80 >> (x & 7));
        // На самом деле бит позиции — (x&7). Используем прямой индекс:
        pixValid_ |= (uint8_t)(1 << (x & 7));
    }
    ++r_[1];   // PLOT инкрементирует R1 (X)
}

uint8_t SuperFX::rpix()
{
    flushPixCache();
    // RPIX читает пиксель — упрощённо возвращаем 0
    return 0;
}

// ─── Один опкод ──────────────────────────────────────────────────────────────
void SuperFX::step()
{
    uint8_t op = fetchOpcode();
    uint16_t& Rs = src();
    (void)Rs;

    auto branch = [&](bool cond) {
        int8_t disp = (int8_t)fetchOpcode();
        if (cond) r_[15] = (uint16_t)(r_[15] + disp);
    };

    switch (op) {
    case 0x00: // STOP
        sfr_ &= ~SFR_GO;
        irq_ = true;
        resetPrefix();
        return;
    case 0x01: // NOP
        resetPrefix(); return;
    case 0x02: // CACHE
        if (cbr_ != (r_[15] & 0xFFF0)) { cbr_ = r_[15] & 0xFFF0; cacheValid_.fill(false); }
        resetPrefix(); return;
    case 0x03: { // LSR
        uint16_t s = src();
        sfr_ &= ~SFR_CY; if (s & 1) sfr_ |= SFR_CY;
        uint16_t r = (uint16_t)(s >> 1);
        setZS16(r); writeDst(r); resetPrefix(); return;
    }
    case 0x04: { // ROL
        uint16_t s = src();
        uint16_t cin = (sfr_ & SFR_CY) ? 1 : 0;
        sfr_ &= ~SFR_CY; if (s & 0x8000) sfr_ |= SFR_CY;
        uint16_t r = (uint16_t)((s << 1) | cin);
        setZS16(r); writeDst(r); resetPrefix(); return;
    }
    case 0x05: branch(true); resetPrefix(); return;             // BRA
    case 0x06: branch((sfr_&SFR_S)!=0 ? ((sfr_&SFR_OV)==0):((sfr_&SFR_OV)!=0)); resetPrefix(); return; // BGE? -> use below
    case 0x07: branch(((sfr_&SFR_S)!=0)==((sfr_&SFR_OV)!=0)); resetPrefix(); return; // BGE
    case 0x08: branch((sfr_&SFR_Z)==0); resetPrefix(); return;  // BNE
    case 0x09: branch((sfr_&SFR_Z)!=0); resetPrefix(); return;  // BEQ
    case 0x0A: branch((sfr_&SFR_S)==0); resetPrefix(); return;  // BPL
    case 0x0B: branch((sfr_&SFR_S)!=0); resetPrefix(); return;  // BMI
    case 0x0C: branch((sfr_&SFR_CY)==0); resetPrefix(); return; // BCC
    case 0x0D: branch((sfr_&SFR_CY)!=0); resetPrefix(); return; // BCS
    case 0x0E: branch((sfr_&SFR_OV)==0); resetPrefix(); return; // BVC
    case 0x0F: branch((sfr_&SFR_OV)!=0); resetPrefix(); return; // BVS
    default: break;
    }

    // $10-$1F: TO Rn (или MOVE если B-префикс)
    if (op >= 0x10 && op <= 0x1F) {
        uint8_t n = op & 15;
        if (b_) { r_[n] = src(); resetPrefix(); }   // MOVE
        else    { dreg_ = n; /* префиксы НЕ сбрасываем для TO */ }
        return;
    }
    // $20-$2F: WITH Rn
    if (op >= 0x20 && op <= 0x2F) {
        uint8_t n = op & 15;
        sreg_ = dreg_ = n; b_ = true; sfr_ |= SFR_B;
        return;
    }
    // $30-$3B: STORE (Rm) — STW/STB
    if (op >= 0x30 && op <= 0x3B) {
        uint8_t n = op & 15;
        uint32_t a = (uint32_t)((rambr_ << 16) | r_[n]);
        if (alt_ & 1) { // STB
            ramWriteByte(a, (uint8_t)src());
        } else {        // STW
            ramWriteByte(a,     (uint8_t)src());
            ramWriteByte(a + 1, (uint8_t)(src() >> 8));
        }
        resetPrefix(); return;
    }
    if (op == 0x3C) { // LOOP
        --r_[12];
        setZS16(r_[12]);
        if (r_[12] != 0) r_[15] = r_[13];
        resetPrefix(); return;
    }
    if (op == 0x3D) { alt_ |= 1; sfr_ |= SFR_ALT1; return; } // ALT1
    if (op == 0x3E) { alt_ |= 2; sfr_ |= SFR_ALT2; return; } // ALT2
    if (op == 0x3F) { alt_ |= 3; sfr_ |= (SFR_ALT1|SFR_ALT2); return; } // ALT3

    // $40-$4B: LOAD (Rm) — LDW/LDB
    if (op >= 0x40 && op <= 0x4B) {
        uint8_t n = op & 15;
        uint32_t a = (uint32_t)((rambr_ << 16) | r_[n]);
        if (alt_ & 1) { // LDB
            writeDst((uint16_t)ramReadByte(a));
        } else {        // LDW
            uint16_t v = (uint16_t)(ramReadByte(a) | (ramReadByte(a + 1) << 8));
            writeDst(v);
        }
        resetPrefix(); return;
    }
    if (op == 0x4C) { // PLOT / RPIX
        if (alt_ & 1) { writeDst(rpix()); }
        else          { plot(); }
        resetPrefix(); return;
    }
    if (op == 0x4D) { // SWAP
        uint16_t s = src();
        uint16_t r = (uint16_t)((s >> 8) | (s << 8));
        setZS16(r); writeDst(r); resetPrefix(); return;
    }
    if (op == 0x4E) { // COLOR / CMODE
        if (alt_ & 1) { por_ = (uint8_t)src(); }     // CMODE
        else          { colr_ = (uint8_t)src(); }    // COLOR
        resetPrefix(); return;
    }
    if (op == 0x4F) { // NOT
        uint16_t r = (uint16_t)~src();
        setZS16(r); writeDst(r); resetPrefix(); return;
    }

    // $50-$5F: ADD/ADC/ADDi
    if (op >= 0x50 && op <= 0x5F) {
        uint8_t n = op & 15;
        uint32_t b = (alt_ & 2) ? n : r_[n];          // ALT2 → immediate n
        uint32_t cin = ((alt_ & 1) && (sfr_ & SFR_CY)) ? 1 : 0;  // ALT1 → ADC
        uint32_t s = src();
        uint32_t res = s + b + cin;
        sfr_ &= ~(SFR_CY|SFR_OV);
        if (res & 0x10000) sfr_ |= SFR_CY;
        if (~(s ^ b) & (s ^ res) & 0x8000) sfr_ |= SFR_OV;
        setZS16((uint16_t)res); writeDst((uint16_t)res); resetPrefix(); return;
    }
    // $60-$6F: SUB/SBC/CMP
    if (op >= 0x60 && op <= 0x6F) {
        uint8_t n = op & 15;
        uint32_t b = (alt_ & 2) ? n : r_[n];
        uint32_t s = src();
        uint32_t cin = ((alt_ & 1) && !(sfr_ & SFR_CY)) ? 1 : 0;  // ALT1 → SBC
        uint32_t res = s - b - cin;
        sfr_ &= ~(SFR_CY|SFR_OV);
        if (!(res & 0x10000)) sfr_ |= SFR_CY;     // нет заёма
        if ((s ^ b) & (s ^ res) & 0x8000) sfr_ |= SFR_OV;
        setZS16((uint16_t)res);
        if (alt_ == 3) { /* CMP: только флаги */ }
        else writeDst((uint16_t)res);
        resetPrefix(); return;
    }
    if (op == 0x70) { // MERGE
        uint16_t r = (uint16_t)((r_[7] & 0xFF00) | (r_[8] >> 8));
        setZS16(r); writeDst(r); resetPrefix(); return;
    }
    // $71-$7F: AND/BIC
    if (op >= 0x71 && op <= 0x7F) {
        uint8_t n = op & 15;
        uint32_t b = (alt_ & 2) ? n : r_[n];
        uint16_t r;
        if (alt_ & 1) r = (uint16_t)(src() & ~b);  // BIC
        else          r = (uint16_t)(src() &  b);  // AND
        setZS16(r); writeDst(r); resetPrefix(); return;
    }
    // $80-$8F: MULT/UMULT
    if (op >= 0x80 && op <= 0x8F) {
        uint8_t n = op & 15;
        int32_t res;
        uint32_t b = (alt_ & 2) ? n : r_[n];
        if (alt_ & 1) { // UMULT
            res = (uint32_t)(uint8_t)src() * (uint32_t)(uint8_t)b;
        } else {        // MULT (signed 8x8)
            res = (int32_t)(int8_t)src() * (int32_t)(int8_t)b;
        }
        setZS16((uint16_t)res); writeDst((uint16_t)res); resetPrefix(); return;
    }
    if (op == 0x90) { // SBK
        uint32_t a = (uint32_t)((rambr_ << 16) | ramBuffer_[0] | (ramBuffer_[1] << 8));
        ramWriteByte(a,     (uint8_t)src());
        ramWriteByte(a + 1, (uint8_t)(src() >> 8));
        resetPrefix(); return;
    }
    if (op >= 0x91 && op <= 0x94) { // LINK #n
        r_[11] = (uint16_t)(r_[15] + (op & 15));
        resetPrefix(); return;
    }
    if (op == 0x95) { // SEX
        uint16_t r = (uint16_t)(int16_t)(int8_t)src();
        setZS16(r); writeDst(r); resetPrefix(); return;
    }
    if (op == 0x96) { // ASR / DIV2
        uint16_t s = src();
        sfr_ &= ~SFR_CY; if (s & 1) sfr_ |= SFR_CY;
        uint16_t r = (uint16_t)((int16_t)s >> 1);
        setZS16(r); writeDst(r); resetPrefix(); return;
    }
    if (op == 0x97) { // ROR
        uint16_t s = src();
        uint16_t cin = (sfr_ & SFR_CY) ? 0x8000 : 0;
        sfr_ &= ~SFR_CY; if (s & 1) sfr_ |= SFR_CY;
        uint16_t r = (uint16_t)((s >> 1) | cin);
        setZS16(r); writeDst(r); resetPrefix(); return;
    }
    if (op >= 0x98 && op <= 0x9D) { // JMP / LJMP Rn
        uint8_t n = op & 15;
        if (alt_ & 1) { pbr_ = (uint8_t)r_[n]; r_[15] = src(); } // LJMP
        else          { r_[15] = r_[n]; }                        // JMP
        resetPrefix(); return;
    }
    if (op == 0x9E) { // LOB
        uint16_t r = (uint16_t)(src() & 0xFF);
        sfr_ &= ~(SFR_Z|SFR_S); if (r==0) sfr_|=SFR_Z; if (r&0x80) sfr_|=SFR_S;
        writeDst(r); resetPrefix(); return;
    }
    if (op == 0x9F) { // FMULT / LMULT
        int32_t res = (int32_t)(int16_t)src() * (int32_t)(int16_t)r_[6];
        if (alt_ & 1) r_[4] = (uint16_t)res;   // LMULT: low в R4
        uint16_t hi = (uint16_t)(res >> 16);
        sfr_ &= ~SFR_CY; if (res & 0x8000) sfr_ |= SFR_CY;
        setZS16(hi); writeDst(hi); resetPrefix(); return;
    }
    // $A0-$AF: IBT / LMS / SMS
    if (op >= 0xA0 && op <= 0xAF) {
        uint8_t n = op & 15;
        if (alt_ == 0) {        // IBT Rn,#imm
            int8_t imm = (int8_t)fetchOpcode();
            r_[n] = (uint16_t)(int16_t)imm;
        } else if (alt_ & 1) {  // SMS (RAM store word, addr = imm*2)
            uint8_t a = fetchOpcode();
            uint32_t addr = (uint32_t)((rambr_ << 16) | (a << 1));
            ramWriteByte(addr,     (uint8_t)r_[n]);
            ramWriteByte(addr + 1, (uint8_t)(r_[n] >> 8));
        } else {                // LMS
            uint8_t a = fetchOpcode();
            uint32_t addr = (uint32_t)((rambr_ << 16) | (a << 1));
            r_[n] = (uint16_t)(ramReadByte(addr) | (ramReadByte(addr+1) << 8));
        }
        resetPrefix(); return;
    }
    // $B0-$BF: FROM Rn (или MOVES если B)
    if (op >= 0xB0 && op <= 0xBF) {
        uint8_t n = op & 15;
        if (b_) { uint16_t v = r_[n]; setZS16(v); writeDst(v); resetPrefix(); } // MOVES
        else    { sreg_ = n; }                                                   // FROM
        return;
    }
    if (op == 0xC0) { // HIB
        uint16_t r = (uint16_t)(src() >> 8);
        sfr_ &= ~(SFR_Z|SFR_S); if (r==0) sfr_|=SFR_Z; if (r&0x80) sfr_|=SFR_S;
        writeDst(r); resetPrefix(); return;
    }
    // $C1-$CF: OR/XOR
    if (op >= 0xC1 && op <= 0xCF) {
        uint8_t n = op & 15;
        uint32_t b = (alt_ & 2) ? n : r_[n];
        uint16_t r;
        if (alt_ & 1) r = (uint16_t)(src() ^ b); // XOR
        else          r = (uint16_t)(src() | b); // OR
        setZS16(r); writeDst(r); resetPrefix(); return;
    }
    // $D0-$DE: INC Rn
    if (op >= 0xD0 && op <= 0xDE) {
        uint8_t n = op & 15;
        r_[n] = (uint16_t)(r_[n] + 1);
        setZS16(r_[n]); resetPrefix(); return;
    }
    if (op == 0xDF) { // GETC / RAMB / ROMB
        if (alt_ == 0)      colr_ = romBufByte_;          // GETC
        else if (alt_ & 1)  rombr_ = (uint8_t)src();      // ROMB
        else                rambr_ = (uint8_t)src();      // RAMB
        resetPrefix(); return;
    }
    // $E0-$EE: DEC Rn
    if (op >= 0xE0 && op <= 0xEE) {
        uint8_t n = op & 15;
        r_[n] = (uint16_t)(r_[n] - 1);
        setZS16(r_[n]); resetPrefix(); return;
    }
    if (op == 0xEF) { // GETB / GETBH / GETBL / GETBS
        uint16_t v;
        if (alt_ == 0)      v = (uint16_t)romBufByte_;                    // GETB
        else if (alt_ == 1) v = (uint16_t)((src() & 0x00FF) | (romBufByte_ << 8)); // GETBH
        else if (alt_ == 2) v = (uint16_t)((src() & 0xFF00) | romBufByte_);        // GETBL
        else                v = (uint16_t)(int16_t)(int8_t)romBufByte_;            // GETBS
        writeDst(v); resetPrefix(); return;
    }
    // $F0-$FF: IWT / LM / SM
    if (op >= 0xF0) {
        uint8_t n = op & 15;
        if (alt_ == 0) {        // IWT Rn,#imm16
            uint8_t lo = fetchOpcode(), hi = fetchOpcode();
            r_[n] = (uint16_t)(lo | (hi << 8));
        } else if (alt_ & 1) {  // SM (RAM store word, addr from next word)
            uint8_t lo = fetchOpcode(), hi = fetchOpcode();
            uint32_t addr = (uint32_t)((rambr_ << 16) | (lo | (hi << 8)));
            ramWriteByte(addr,     (uint8_t)r_[n]);
            ramWriteByte(addr + 1, (uint8_t)(r_[n] >> 8));
        } else {                // LM
            uint8_t lo = fetchOpcode(), hi = fetchOpcode();
            uint32_t addr = (uint32_t)((rambr_ << 16) | (lo | (hi << 8)));
            r_[n] = (uint16_t)(ramReadByte(addr) | (ramReadByte(addr+1) << 8));
        }
        resetPrefix(); return;
    }

    // Неизвестный опкод — просто сбрасываем префиксы
    resetPrefix();
}

// ─── Исполнение пачки тактов ─────────────────────────────────────────────────
void SuperFX::run(int cycles)
{
    int guard = 0;
    while ((sfr_ & SFR_GO) && guard < cycles) {
        step();
        ++guard;
    }
}

// ─── Регистры CPU-доступа $3000-$32FF ────────────────────────────────────────
uint8_t SuperFX::readReg(uint16_t addr)
{
    uint16_t a = (uint16_t)(addr & 0x3FF);
    if (a < 0x20) {                         // $3000-$301F: R0-R15 (по 2 байта)
        int reg = a >> 1;
        return (a & 1) ? (uint8_t)(r_[reg] >> 8) : (uint8_t)r_[reg];
    }
    switch (a) {
        case 0x30: return (uint8_t)sfr_;
        case 0x31: return (uint8_t)(sfr_ >> 8);
        case 0x34: return pbr_;
        case 0x36: return rombr_;
        case 0x3B: return vcr_;       // version
        case 0x3C: return rambr_;
        case 0x3E: return (uint8_t)cbr_;
        case 0x3F: return (uint8_t)(cbr_ >> 8);
    }
    return 0;
}

void SuperFX::writeReg(uint16_t addr, uint8_t data)
{
    uint16_t a = (uint16_t)(addr & 0x3FF);
    if (a < 0x20) {                         // R0-R15
        int reg = a >> 1;
        if (a & 1) r_[reg] = (uint16_t)((r_[reg] & 0x00FF) | (data << 8));
        else       r_[reg] = (uint16_t)((r_[reg] & 0xFF00) | data);
        // Запись в старший байт R15 запускает GSU
        if (reg == 15 && (a & 1)) {
            sfr_ |= SFR_GO;
        }
        return;
    }
    switch (a) {
        case 0x30: sfr_ = (uint16_t)((sfr_ & 0xFF00) | data); break;
        case 0x31: sfr_ = (uint16_t)((sfr_ & 0x00FF) | (data << 8));
                   if (!(data & (SFR_GO >> 8))) {} break;
        case 0x33: bramr_ = data; break;
        case 0x34: pbr_ = data; break;
        case 0x37: cfgr_ = data; break;
        case 0x38: scbr_ = data; break;
        case 0x39: clsr_ = data; break;
        case 0x3A: scmr_ = data; break;
        default: break;
    }
}
