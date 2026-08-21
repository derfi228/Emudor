#include "snes/cpu65816.h"
#include "snes/snes_bus.h"
#include <cstring>

// ─── Конструктор / подключение ───────────────────────────────────────────────

CPU65816::CPU65816() {
    buildLookupTable();
}

void CPU65816::connectBus(SnesBus* bus) {
    bus_ = bus;
}

// ─── Чтение/запись шины ──────────────────────────────────────────────────────

uint8_t CPU65816::readByte(uint32_t addr) {
    return bus_ ? bus_->read(addr & 0xFFFFFF) : 0;
}

void CPU65816::writeByte(uint32_t addr, uint8_t data) {
    if (bus_) bus_->write(addr & 0xFFFFFF, data);
}

// Чтение 16-бит: банк фиксируется, адрес врапается в $FFFF→$0000
uint16_t CPU65816::readWord(uint32_t addr) {
    uint32_t bank = addr & 0xFF0000;
    uint8_t  lo   = readByte(addr);
    uint8_t  hi   = readByte(bank | ((uint16_t)(addr + 1)));
    return (uint16_t)(lo | (hi << 8));
}

void CPU65816::writeWord(uint32_t addr, uint16_t data) {
    uint32_t bank = addr & 0xFF0000;
    writeByte(addr,                           (uint8_t)(data & 0xFF));
    writeByte(bank | ((uint16_t)(addr + 1)), (uint8_t)(data >> 8));
}

// Чтение 24-бит (для indirect long): пересекает банки
uint32_t CPU65816::readLong(uint32_t addr) {
    uint8_t lo  = readByte(addr & 0xFFFFFF);
    uint8_t mid = readByte((addr + 1) & 0xFFFFFF);
    uint8_t hi  = readByte((addr + 2) & 0xFFFFFF);
    return (uint32_t)(lo | ((uint32_t)mid << 8) | ((uint32_t)hi << 16));
}

// ─── Чтение из потока команд ─────────────────────────────────────────────────

uint8_t CPU65816::fetchByte() {
    uint8_t v = readByte(((uint32_t)PBR << 16) | PC);
    PC++;
    return v;
}

uint16_t CPU65816::fetchWord() {
    uint8_t lo = fetchByte();
    uint8_t hi = fetchByte();
    return (uint16_t)(lo | (hi << 8));
}

uint32_t CPU65816::fetchLong() {
    uint8_t lo  = fetchByte();
    uint8_t mid = fetchByte();
    uint8_t hi  = fetchByte();
    return (uint32_t)(lo | ((uint32_t)mid << 8) | ((uint32_t)hi << 16));
}

// ─── Стек ────────────────────────────────────────────────────────────────────

void CPU65816::push8(uint8_t v) {
    writeByte(SP, v);
    SP--;
    if (E) SP = (uint16_t)((SP & 0x00FF) | 0x0100);  // в emulation SP в странице $01
}

uint8_t CPU65816::pop8() {
    SP++;
    if (E) SP = (uint16_t)((SP & 0x00FF) | 0x0100);
    return readByte(SP);
}

void CPU65816::push16(uint16_t v) {
    push8((uint8_t)(v >> 8));
    push8((uint8_t)(v & 0xFF));
}

uint16_t CPU65816::pop16() {
    uint8_t lo = pop8();
    uint8_t hi = pop8();
    return (uint16_t)(lo | (hi << 8));
}

// ─── Флаги ───────────────────────────────────────────────────────────────────

void CPU65816::setFlag(Flag f, bool v) {
    if (v) P |= (uint8_t)f;
    else   P &= (uint8_t)~(uint8_t)f;
}

void CPU65816::setNZ8(uint8_t v) {
    setFlag(FLAG_N, (v & 0x80) != 0);
    setFlag(FLAG_Z, v == 0);
}

void CPU65816::setNZ16(uint16_t v) {
    setFlag(FLAG_N, (v & 0x8000) != 0);
    setFlag(FLAG_Z, v == 0);
}

// ─── Режимы адресации ────────────────────────────────────────────────────────

void CPU65816::am_imp() {
    addrAbs_ = 0;
}

// Immediate: M-зависимый (1 или 2 байта)
void CPU65816::am_immM() {
    addrAbs_ = ((uint32_t)PBR << 16) | PC;
    if (flagM()) {
        PC++;
    } else {
        PC += 2;
        pendingCycles_++;  // +1 такт в 16-битном режиме
    }
}

// Immediate: X-зависимый (1 или 2 байта)
void CPU65816::am_immX() {
    addrAbs_ = ((uint32_t)PBR << 16) | PC;
    if (flagX()) {
        PC++;
    } else {
        PC += 2;
        pendingCycles_++;
    }
}

// Immediate: всегда 8-бит
void CPU65816::am_imm8() {
    addrAbs_ = ((uint32_t)PBR << 16) | PC;
    PC++;
}

// Direct Page: DB:$0000 + D + offset
void CPU65816::am_dp() {
    uint8_t off = fetchByte();
    addrAbs_ = (uint32_t)((D + off) & 0xFFFF);
    if (D & 0xFF) pendingCycles_++;  // +1 если DL != 0
}

void CPU65816::am_dpX() {
    uint8_t off = fetchByte();
    addrAbs_ = (uint32_t)((D + (flagX() ? (X & 0xFF) : X) + off) & 0xFFFF);
    if (D & 0xFF) pendingCycles_++;
}

void CPU65816::am_dpY() {
    uint8_t off = fetchByte();
    addrAbs_ = (uint32_t)((D + (flagX() ? (Y & 0xFF) : Y) + off) & 0xFFFF);
    if (D & 0xFF) pendingCycles_++;
}

// (Direct Page): читает 16-битный вектор из DP, банк = DBR
void CPU65816::am_dpInd() {
    uint8_t off = fetchByte();
    uint32_t ptr = (uint32_t)((D + off) & 0xFFFF);
    if (D & 0xFF) pendingCycles_++;
    addrAbs_ = ((uint32_t)DBR << 16) | readWord(ptr);
}

// (Direct Page,X): читает вектор из DP+X
void CPU65816::am_dpIndX() {
    uint8_t off = fetchByte();
    uint16_t xv = flagX() ? (X & 0xFF) : X;
    uint32_t ptr = (uint32_t)((D + off + xv) & 0xFFFF);
    if (D & 0xFF) pendingCycles_++;
    addrAbs_ = ((uint32_t)DBR << 16) | readWord(ptr);
}

// (Direct Page),Y: читает вектор из DP, добавляет Y, банк = DBR
void CPU65816::am_dpIndY() {
    uint8_t off = fetchByte();
    uint32_t ptr = (uint32_t)((D + off) & 0xFFFF);
    if (D & 0xFF) pendingCycles_++;
    uint16_t base = readWord(ptr);
    uint16_t yv   = flagX() ? (Y & 0xFF) : Y;
    // Штраф за пересечение страницы (только в emulation mode)
    if (E && ((base & 0xFF00) != ((base + yv) & 0xFF00))) pendingCycles_++;
    addrAbs_ = ((uint32_t)DBR << 16) | (uint16_t)(base + yv);
}

// [Direct Page]: читает 24-бит вектор (без DBR)
void CPU65816::am_dpIndL() {
    uint8_t off = fetchByte();
    uint32_t ptr = (uint32_t)((D + off) & 0xFFFF);
    if (D & 0xFF) pendingCycles_++;
    addrAbs_ = readLong(ptr);
}

// [Direct Page],Y: то же + Y
void CPU65816::am_dpIndLY() {
    uint8_t off = fetchByte();
    uint32_t ptr = (uint32_t)((D + off) & 0xFFFF);
    if (D & 0xFF) pendingCycles_++;
    uint32_t base = readLong(ptr);
    uint16_t yv   = flagX() ? (Y & 0xFF) : Y;
    addrAbs_ = (base + yv) & 0xFFFFFF;
}

// Absolute: DBR:abs16
void CPU65816::am_abs() {
    uint16_t a = fetchWord();
    addrAbs_ = ((uint32_t)DBR << 16) | a;
}

// Absolute,X
void CPU65816::am_absX() {
    uint16_t a  = fetchWord();
    uint16_t xv = flagX() ? (X & 0xFF) : X;
    if (E && ((a & 0xFF00) != ((a + xv) & 0xFF00))) pendingCycles_++;
    addrAbs_ = ((uint32_t)DBR << 16) | (uint16_t)(a + xv);
}

// Absolute,Y
void CPU65816::am_absY() {
    uint16_t a  = fetchWord();
    uint16_t yv = flagX() ? (Y & 0xFF) : Y;
    if (E && ((a & 0xFF00) != ((a + yv) & 0xFF00))) pendingCycles_++;
    addrAbs_ = ((uint32_t)DBR << 16) | (uint16_t)(a + yv);
}

// Absolute Long: полный 24-бит адрес
void CPU65816::am_absL() {
    addrAbs_ = fetchLong();
}

// Absolute Long,X
void CPU65816::am_absLX() {
    uint32_t a  = fetchLong();
    uint16_t xv = flagX() ? (X & 0xFF) : X;
    addrAbs_ = (a + xv) & 0xFFFFFF;
}

// (Absolute): читает 16-бит вектор из банка 0 (только для JMP)
void CPU65816::am_absInd() {
    uint16_t a = fetchWord();
    addrAbs_ = readWord(a);  // банк 0
}

// [Absolute]: читает 24-бит вектор из банка 0 (JML)
void CPU65816::am_absIndL() {
    uint16_t a = fetchWord();
    addrAbs_ = readLong(a);  // банк 0
}

// (Absolute,X): читает вектор из PBR:abs+X (JSR/JMP)
void CPU65816::am_absIndX() {
    uint16_t a  = fetchWord();
    uint16_t xv = flagX() ? (X & 0xFF) : X;
    uint32_t ptr = ((uint32_t)PBR << 16) | (uint16_t)(a + xv);
    addrAbs_ = readWord(ptr);
}

// Stack Relative: SP + offset (банк 0)
void CPU65816::am_sr() {
    uint8_t off = fetchByte();
    addrAbs_ = (uint32_t)((SP + off) & 0xFFFF);
}

// (Stack Relative),Y
void CPU65816::am_srIndY() {
    uint8_t  off = fetchByte();
    uint32_t ptr = (uint32_t)((SP + off) & 0xFFFF);
    uint16_t base = readWord(ptr);
    uint16_t yv   = flagX() ? (Y & 0xFF) : Y;
    addrAbs_ = ((uint32_t)DBR << 16) | (uint16_t)(base + yv);
}

// Relative 8-bit (ветвления)
void CPU65816::am_rel() {
    int8_t off  = (int8_t)fetchByte();
    addrAbs_ = (uint32_t)(int32_t)(int16_t)(PC + (int16_t)off);
}

// Relative 16-bit (BRL)
void CPU65816::am_relL() {
    int16_t off = (int16_t)fetchWord();
    addrAbs_ = (uint32_t)(uint16_t)(PC + off);
}

// Block Move (MVP/MVN): читает src/dst банки
void CPU65816::am_blk() {
    uint8_t dst = fetchByte();
    uint8_t src = fetchByte();
    addrAbs_ = ((uint32_t)src << 8) | dst;
}

// ─── Ветвление ───────────────────────────────────────────────────────────────

void CPU65816::doBranch(bool cond) {
    if (!cond) return;
    pendingCycles_++;
    // В emulation mode: +1 такт если смена страницы
    if (E && ((PC & 0xFF00) != ((uint16_t)addrAbs_ & 0xFF00)))
        pendingCycles_++;
    PC = (uint16_t)addrAbs_;
}

// ─── Опкоды ──────────────────────────────────────────────────────────────────

void CPU65816::op_ADC() {
    if (flagM()) {
        uint8_t  v = readByte(addrAbs_);
        uint16_t r = (uint16_t)(A & 0xFF) + v + (uint16_t)(P & FLAG_C);
        if (P & FLAG_D) {  // BCD
            if ((r & 0x0F) > 9) r += 6;
            if ((r & 0xF0) > 0x90) { r += 0x60; }
        }
        setFlag(FLAG_C, r > 0xFF);
        setFlag(FLAG_V, (~((A & 0xFF) ^ v) & ((A & 0xFF) ^ r) & 0x80) != 0);
        A = (uint16_t)((A & 0xFF00) | (r & 0xFF));
        setNZ8((uint8_t)(A & 0xFF));
    } else {
        uint16_t v = readWord(addrAbs_);
        uint32_t r = (uint32_t)A + v + (uint32_t)(P & FLAG_C);
        if (P & FLAG_D) {  // BCD 16-бит
            if ((r & 0x000F) > 9)    r += 0x0006;
            if ((r & 0x00F0) > 0x90) r += 0x0060;
            if ((r & 0x0F00) > 0x900) r += 0x0600;
            if (r > 0x9999)           r += 0x6000;
        }
        setFlag(FLAG_C, r > 0xFFFF);
        setFlag(FLAG_V, (~(A ^ v) & (A ^ r) & 0x8000) != 0);
        A = (uint16_t)(r & 0xFFFF);
        setNZ16(A);
        pendingCycles_++;
    }
}

void CPU65816::op_AND() {
    if (flagM()) {
        A = (uint16_t)((A & 0xFF00) | ((A & 0xFF) & readByte(addrAbs_)));
        setNZ8((uint8_t)(A & 0xFF));
    } else {
        A &= readWord(addrAbs_);
        setNZ16(A);
        pendingCycles_++;
    }
}

void CPU65816::op_ASL() {
    if (flagM()) {
        uint8_t v = readByte(addrAbs_);
        setFlag(FLAG_C, (v & 0x80) != 0);
        v = (uint8_t)(v << 1);
        writeByte(addrAbs_, v);
        setNZ8(v);
    } else {
        uint16_t v = readWord(addrAbs_);
        setFlag(FLAG_C, (v & 0x8000) != 0);
        v <<= 1;
        writeWord(addrAbs_, v);
        setNZ16(v);
        pendingCycles_++;
    }
}

void CPU65816::op_ASL_a() {
    if (flagM()) {
        setFlag(FLAG_C, (A & 0x80) != 0);
        A = (uint16_t)((A & 0xFF00) | ((A << 1) & 0xFF));
        setNZ8((uint8_t)(A & 0xFF));
    } else {
        setFlag(FLAG_C, (A & 0x8000) != 0);
        A <<= 1;
        setNZ16(A);
    }
}

void CPU65816::op_BCC() { doBranch(!(P & FLAG_C)); }
void CPU65816::op_BCS() { doBranch( (P & FLAG_C)); }
void CPU65816::op_BEQ() { doBranch( (P & FLAG_Z)); }
void CPU65816::op_BMI() { doBranch( (P & FLAG_N)); }
void CPU65816::op_BNE() { doBranch(!(P & FLAG_Z)); }
void CPU65816::op_BPL() { doBranch(!(P & FLAG_N)); }
void CPU65816::op_BVC() { doBranch(!(P & FLAG_V)); }
void CPU65816::op_BVS() { doBranch( (P & FLAG_V)); }
void CPU65816::op_BRA() { doBranch(true); }

void CPU65816::op_BRL() {
    PC = (uint16_t)addrAbs_;
}

void CPU65816::op_BIT() {
    if (flagM()) {
        uint8_t v = readByte(addrAbs_);
        setFlag(FLAG_N, (v & 0x80) != 0);
        setFlag(FLAG_V, (v & 0x40) != 0);
        setFlag(FLAG_Z, ((A & 0xFF) & v) == 0);
    } else {
        uint16_t v = readWord(addrAbs_);
        setFlag(FLAG_N, (v & 0x8000) != 0);
        setFlag(FLAG_V, (v & 0x4000) != 0);
        setFlag(FLAG_Z, (A & v) == 0);
        pendingCycles_++;
    }
}

void CPU65816::op_BIT_imm() {
    // BIT #imm: не трогает N,V
    if (flagM()) {
        setFlag(FLAG_Z, ((A & 0xFF) & readByte(addrAbs_)) == 0);
    } else {
        setFlag(FLAG_Z, (A & readWord(addrAbs_)) == 0);
        pendingCycles_++;
    }
}

void CPU65816::op_BRK() {
    fetchByte();  // сигнатурный байт (игнорируем)
    if (E) {
        // Emulation: BRK как 65C02
        push16(PC);
        push8((uint8_t)(P | FLAG_XB));   // B=1
        setFlag(FLAG_I, true);
        setFlag(FLAG_D, false);
        uint16_t vec = readWord(0xFFFE);
        PC = vec;
    } else {
        // Native
        push8(PBR);
        push16(PC);
        push8(P);
        setFlag(FLAG_I, true);
        setFlag(FLAG_D, false);
        PBR = 0;
        PC = readWord(0xFFE6);
    }
}

void CPU65816::op_CLC() { setFlag(FLAG_C, false); }
void CPU65816::op_CLD() { setFlag(FLAG_D, false); }
void CPU65816::op_CLI() { setFlag(FLAG_I, false); }
void CPU65816::op_CLV() { setFlag(FLAG_V, false); }
void CPU65816::op_SEC() { setFlag(FLAG_C, true);  }
void CPU65816::op_SED() { setFlag(FLAG_D, true);  }
void CPU65816::op_SEI() { setFlag(FLAG_I, true);  }

void CPU65816::op_CMP() {
    if (flagM()) {
        uint8_t  v = readByte(addrAbs_);
        uint16_t r = (uint16_t)(A & 0xFF) - v;
        setFlag(FLAG_C, (A & 0xFF) >= v);
        setNZ8((uint8_t)(r & 0xFF));
    } else {
        uint16_t v = readWord(addrAbs_);
        uint32_t r = (uint32_t)A - v;
        setFlag(FLAG_C, A >= v);
        setNZ16((uint16_t)(r & 0xFFFF));
        pendingCycles_++;
    }
}

void CPU65816::op_COP() {
    fetchByte();
    if (E) {
        push16(PC);
        push8(P);
        setFlag(FLAG_I, true);
        setFlag(FLAG_D, false);
        PC = readWord(0xFFF4);
    } else {
        push8(PBR);
        push16(PC);
        push8(P);
        setFlag(FLAG_I, true);
        setFlag(FLAG_D, false);
        PBR = 0;
        PC  = readWord(0xFFE4);
    }
}

void CPU65816::op_CPX() {
    if (flagX()) {
        uint8_t  v = readByte(addrAbs_);
        setFlag(FLAG_C, (X & 0xFF) >= v);
        setNZ8((uint8_t)((X & 0xFF) - v));
    } else {
        uint16_t v = readWord(addrAbs_);
        setFlag(FLAG_C, X >= v);
        setNZ16((uint16_t)(X - v));
        pendingCycles_++;
    }
}

void CPU65816::op_CPY() {
    if (flagX()) {
        uint8_t  v = readByte(addrAbs_);
        setFlag(FLAG_C, (Y & 0xFF) >= v);
        setNZ8((uint8_t)((Y & 0xFF) - v));
    } else {
        uint16_t v = readWord(addrAbs_);
        setFlag(FLAG_C, Y >= v);
        setNZ16((uint16_t)(Y - v));
        pendingCycles_++;
    }
}

void CPU65816::op_DEC() {
    if (flagM()) {
        uint8_t v = (uint8_t)(readByte(addrAbs_) - 1);
        writeByte(addrAbs_, v);
        setNZ8(v);
    } else {
        uint16_t v = (uint16_t)(readWord(addrAbs_) - 1);
        writeWord(addrAbs_, v);
        setNZ16(v);
        pendingCycles_++;
    }
}

void CPU65816::op_DEC_a() {
    if (flagM()) {
        A = (uint16_t)((A & 0xFF00) | ((A - 1) & 0xFF));
        setNZ8((uint8_t)(A & 0xFF));
    } else {
        A--;
        setNZ16(A);
    }
}

void CPU65816::op_DEX() {
    if (flagX()) { X = (uint16_t)((X - 1) & 0xFF); setNZ8((uint8_t)(X & 0xFF)); }
    else         { X--; setNZ16(X); }
}

void CPU65816::op_DEY() {
    if (flagX()) { Y = (uint16_t)((Y - 1) & 0xFF); setNZ8((uint8_t)(Y & 0xFF)); }
    else         { Y--; setNZ16(Y); }
}

void CPU65816::op_EOR() {
    if (flagM()) {
        A = (uint16_t)((A & 0xFF00) | ((A ^ readByte(addrAbs_)) & 0xFF));
        setNZ8((uint8_t)(A & 0xFF));
    } else {
        A ^= readWord(addrAbs_);
        setNZ16(A);
        pendingCycles_++;
    }
}

void CPU65816::op_INC() {
    if (flagM()) {
        uint8_t v = (uint8_t)(readByte(addrAbs_) + 1);
        writeByte(addrAbs_, v);
        setNZ8(v);
    } else {
        uint16_t v = (uint16_t)(readWord(addrAbs_) + 1);
        writeWord(addrAbs_, v);
        setNZ16(v);
        pendingCycles_++;
    }
}

void CPU65816::op_INC_a() {
    if (flagM()) {
        A = (uint16_t)((A & 0xFF00) | ((A + 1) & 0xFF));
        setNZ8((uint8_t)(A & 0xFF));
    } else {
        A++;
        setNZ16(A);
    }
}

void CPU65816::op_INX() {
    if (flagX()) { X = (uint16_t)((X + 1) & 0xFF); setNZ8((uint8_t)(X & 0xFF)); }
    else         { X++; setNZ16(X); }
}

void CPU65816::op_INY() {
    if (flagX()) { Y = (uint16_t)((Y + 1) & 0xFF); setNZ8((uint8_t)(Y & 0xFF)); }
    else         { Y++; setNZ16(Y); }
}

void CPU65816::op_JMP() {
    PC = (uint16_t)(addrAbs_ & 0xFFFF);
}

void CPU65816::op_JML() {
    PBR = (uint8_t)(addrAbs_ >> 16);
    PC  = (uint16_t)(addrAbs_ & 0xFFFF);
}

void CPU65816::op_JSR() {
    push16(PC - 1);
    PC = (uint16_t)(addrAbs_ & 0xFFFF);
}

void CPU65816::op_JSL() {
    push8(PBR);
    push16(PC - 1);
    PBR = (uint8_t)(addrAbs_ >> 16);
    PC  = (uint16_t)(addrAbs_ & 0xFFFF);
}

void CPU65816::op_JSR_indX() {
    push16(PC - 1);
    PC = (uint16_t)(addrAbs_ & 0xFFFF);
}

void CPU65816::op_LDA() {
    if (flagM()) {
        uint8_t v = readByte(addrAbs_);
        A = (uint16_t)((A & 0xFF00) | v);
        setNZ8(v);
    } else {
        A = readWord(addrAbs_);
        setNZ16(A);
        pendingCycles_++;
    }
}

void CPU65816::op_LDX() {
    if (flagX()) {
        uint8_t v = readByte(addrAbs_);
        X = v;
        setNZ8(v);
    } else {
        X = readWord(addrAbs_);
        setNZ16(X);
        pendingCycles_++;
    }
}

void CPU65816::op_LDY() {
    if (flagX()) {
        uint8_t v = readByte(addrAbs_);
        Y = v;
        setNZ8(v);
    } else {
        Y = readWord(addrAbs_);
        setNZ16(Y);
        pendingCycles_++;
    }
}

void CPU65816::op_LSR() {
    if (flagM()) {
        uint8_t v = readByte(addrAbs_);
        setFlag(FLAG_C, (v & 0x01) != 0);
        v >>= 1;
        writeByte(addrAbs_, v);
        setNZ8(v);
    } else {
        uint16_t v = readWord(addrAbs_);
        setFlag(FLAG_C, (v & 0x01) != 0);
        v >>= 1;
        writeWord(addrAbs_, v);
        setNZ16(v);
        pendingCycles_++;
    }
}

void CPU65816::op_LSR_a() {
    if (flagM()) {
        setFlag(FLAG_C, (A & 0x01) != 0);
        A = (uint16_t)((A & 0xFF00) | ((A >> 1) & 0x7F));
        setNZ8((uint8_t)(A & 0xFF));
    } else {
        setFlag(FLAG_C, (A & 0x01) != 0);
        A >>= 1;
        setNZ16(A);
    }
}

void CPU65816::op_MVP() {
    // MVP dstBank, srcBank: C = count, X = src, Y = dst
    uint8_t dst = (uint8_t)(addrAbs_ & 0xFF);
    uint8_t src = (uint8_t)(addrAbs_ >> 8);
    uint8_t v = readByte(((uint32_t)src << 16) | X);
    writeByte(((uint32_t)dst << 16) | Y, v);
    DBR = dst;
    X--; Y--; A--;
    if (A != 0xFFFF) PC -= 3;  // повтор
}

void CPU65816::op_MVN() {
    uint8_t dst = (uint8_t)(addrAbs_ & 0xFF);
    uint8_t src = (uint8_t)(addrAbs_ >> 8);
    uint8_t v = readByte(((uint32_t)src << 16) | X);
    writeByte(((uint32_t)dst << 16) | Y, v);
    DBR = dst;
    X++; Y++; A--;
    if (A != 0xFFFF) PC -= 3;
}

void CPU65816::op_NOP() {}
void CPU65816::op_WDM() { /* будущий NOP */ }
void CPU65816::op_STP() { stopped_ = true; }
void CPU65816::op_WAI() { waiting_ = true; }

void CPU65816::op_ORA() {
    if (flagM()) {
        A = (uint16_t)((A & 0xFF00) | ((A | readByte(addrAbs_)) & 0xFF));
        setNZ8((uint8_t)(A & 0xFF));
    } else {
        A |= readWord(addrAbs_);
        setNZ16(A);
        pendingCycles_++;
    }
}

void CPU65816::op_PEA() {
    push16((uint16_t)(addrAbs_));
}

void CPU65816::op_PEI() {
    // PEI (dp): читает слово из DP и толкает его
    uint16_t v = readWord(addrAbs_);
    push16(v);
}

void CPU65816::op_PER() {
    // PER label: толкает PC + offset (уже вычислен в addrAbs_)
    push16((uint16_t)addrAbs_);
}

void CPU65816::op_PHA() {
    if (flagM()) push8((uint8_t)(A & 0xFF));
    else         { push16(A); pendingCycles_++; }
}

void CPU65816::op_PHB() { push8(DBR); }
void CPU65816::op_PHD() { push16(D); }
void CPU65816::op_PHK() { push8(PBR); }
void CPU65816::op_PHP() { push8(P); }

void CPU65816::op_PHX() {
    if (flagX()) push8((uint8_t)(X & 0xFF));
    else         { push16(X); pendingCycles_++; }
}

void CPU65816::op_PHY() {
    if (flagX()) push8((uint8_t)(Y & 0xFF));
    else         { push16(Y); pendingCycles_++; }
}

void CPU65816::op_PLA() {
    if (flagM()) {
        uint8_t v = pop8();
        A = (uint16_t)((A & 0xFF00) | v);
        setNZ8(v);
    } else {
        A = pop16();
        setNZ16(A);
        pendingCycles_++;
    }
}

void CPU65816::op_PLB() {
    DBR = pop8();
    setNZ8(DBR);
}

void CPU65816::op_PLD() {
    D = pop16();
    setNZ16(D);
}

void CPU65816::op_PLP() {
    P = pop8();
    if (E) { P |= FLAG_M | FLAG_XB; }  // в emulation M и X всегда 1
    // При X=1 старший байт X и Y обнуляется
    if (P & FLAG_XB) { X &= 0xFF; Y &= 0xFF; }
}

void CPU65816::op_PLX() {
    if (flagX()) { X = pop8(); setNZ8((uint8_t)X); }
    else         { X = pop16(); setNZ16(X); pendingCycles_++; }
}

void CPU65816::op_PLY() {
    if (flagX()) { Y = pop8(); setNZ8((uint8_t)Y); }
    else         { Y = pop16(); setNZ16(Y); pendingCycles_++; }
}

void CPU65816::op_REP() {
    uint8_t mask = readByte(addrAbs_);
    P &= ~mask;
    if (E) P |= (FLAG_M | FLAG_XB);  // нельзя сбросить в emulation mode
}

void CPU65816::op_SEP() {
    uint8_t mask = readByte(addrAbs_);
    P |= mask;
    if (P & FLAG_XB) { X &= 0xFF; Y &= 0xFF; }  // обнуляем старший байт
}

void CPU65816::op_ROL() {
    uint8_t oldC = (uint8_t)(P & FLAG_C);
    if (flagM()) {
        uint8_t v = readByte(addrAbs_);
        setFlag(FLAG_C, (v & 0x80) != 0);
        v = (uint8_t)((v << 1) | oldC);
        writeByte(addrAbs_, v);
        setNZ8(v);
    } else {
        uint16_t v = readWord(addrAbs_);
        setFlag(FLAG_C, (v & 0x8000) != 0);
        v = (uint16_t)((v << 1) | oldC);
        writeWord(addrAbs_, v);
        setNZ16(v);
        pendingCycles_++;
    }
}

void CPU65816::op_ROL_a() {
    uint8_t oldC = (uint8_t)(P & FLAG_C);
    if (flagM()) {
        setFlag(FLAG_C, (A & 0x80) != 0);
        A = (uint16_t)((A & 0xFF00) | (((A << 1) | oldC) & 0xFF));
        setNZ8((uint8_t)(A & 0xFF));
    } else {
        setFlag(FLAG_C, (A & 0x8000) != 0);
        A = (uint16_t)((A << 1) | oldC);
        setNZ16(A);
    }
}

void CPU65816::op_ROR() {
    uint8_t oldC = (uint8_t)(P & FLAG_C);
    if (flagM()) {
        uint8_t v = readByte(addrAbs_);
        setFlag(FLAG_C, (v & 0x01) != 0);
        v = (uint8_t)((v >> 1) | (oldC << 7));
        writeByte(addrAbs_, v);
        setNZ8(v);
    } else {
        uint16_t v = readWord(addrAbs_);
        setFlag(FLAG_C, (v & 0x0001) != 0);
        v = (uint16_t)((v >> 1) | ((uint16_t)oldC << 15));
        writeWord(addrAbs_, v);
        setNZ16(v);
        pendingCycles_++;
    }
}

void CPU65816::op_ROR_a() {
    uint8_t oldC = (uint8_t)(P & FLAG_C);
    if (flagM()) {
        setFlag(FLAG_C, (A & 0x01) != 0);
        // В 8-битном режиме сдвигать нужно ТОЛЬКО младший байт: иначе бит 8
        // (младший бит скрытого регистра B) протекает в бит 7 результата.
        A = (uint16_t)((A & 0xFF00) | ((((A & 0xFF) >> 1) | (oldC << 7)) & 0xFF));
        setNZ8((uint8_t)(A & 0xFF));
    } else {
        setFlag(FLAG_C, (A & 0x0001) != 0);
        A = (uint16_t)((A >> 1) | ((uint16_t)oldC << 15));
        setNZ16(A);
    }
}

void CPU65816::op_RTI() {
    P = pop8();
    if (E) { P |= (FLAG_M | FLAG_XB); }
    PC = pop16();
    if (!E) PBR = pop8();
    if (P & FLAG_XB) { X &= 0xFF; Y &= 0xFF; }
}

void CPU65816::op_RTL() {
    PC  = (uint16_t)(pop16() + 1);
    PBR = pop8();
}

void CPU65816::op_RTS() {
    PC = (uint16_t)(pop16() + 1);
}

void CPU65816::op_SBC() {
    if (flagM()) {
        uint8_t  v = readByte(addrAbs_);
        uint8_t  c = (uint8_t)(P & FLAG_C);
        uint16_t r = (uint16_t)(A & 0xFF) - v - (1 - c);
        if (P & FLAG_D) {
            if (((A & 0x0F) - (1 - c)) < (v & 0x0F)) r -= 6;
            if (r > 0x99) r -= 0x60;
        }
        setFlag(FLAG_C, r < 0x100);
        setFlag(FLAG_V, ((A & 0xFF) ^ v) & ((A & 0xFF) ^ r) & 0x80);
        A = (uint16_t)((A & 0xFF00) | (r & 0xFF));
        setNZ8((uint8_t)(A & 0xFF));
    } else {
        uint16_t v = readWord(addrAbs_);
        uint8_t  c = (uint8_t)(P & FLAG_C);
        uint32_t r = (uint32_t)A - v - (1 - c);
        if (P & FLAG_D) {
            if (((A & 0x000F) - (1 - c)) < (v & 0x000F)) r -= 0x0006;
            if (r > 0x9999) r -= 0x6000;
        }
        setFlag(FLAG_C, r < 0x10000);
        setFlag(FLAG_V, (A ^ v) & (A ^ r) & 0x8000);
        A = (uint16_t)(r & 0xFFFF);
        setNZ16(A);
        pendingCycles_++;
    }
}

void CPU65816::op_STA() {
    if (flagM()) {
        writeByte(addrAbs_, (uint8_t)(A & 0xFF));
    } else {
        writeWord(addrAbs_, A);
        pendingCycles_++;
    }
}

void CPU65816::op_STX() {
    if (flagX()) writeByte(addrAbs_, (uint8_t)(X & 0xFF));
    else         { writeWord(addrAbs_, X); pendingCycles_++; }
}

void CPU65816::op_STY() {
    if (flagX()) writeByte(addrAbs_, (uint8_t)(Y & 0xFF));
    else         { writeWord(addrAbs_, Y); pendingCycles_++; }
}

void CPU65816::op_STZ() {
    if (flagM()) writeByte(addrAbs_, 0);
    else         { writeWord(addrAbs_, 0); pendingCycles_++; }
}

void CPU65816::op_TAX() {
    if (flagX()) { X = (uint16_t)(A & 0xFF); setNZ8((uint8_t)X); }
    else         { X = A; setNZ16(X); }
}

void CPU65816::op_TAY() {
    if (flagX()) { Y = (uint16_t)(A & 0xFF); setNZ8((uint8_t)Y); }
    else         { Y = A; setNZ16(Y); }
}

void CPU65816::op_TCS() {
    SP = A;  // native: полный 16-бит; emulation: $01xx
    if (E) SP = (uint16_t)((SP & 0x00FF) | 0x0100);
}

void CPU65816::op_TCD() { D = A; setNZ16(D); }
void CPU65816::op_TDC() { A = D; setNZ16(A); }
void CPU65816::op_TSC() { A = SP; setNZ16(A); }

void CPU65816::op_TSX() {
    if (flagX()) { X = (uint16_t)(SP & 0xFF); setNZ8((uint8_t)X); }
    else         { X = SP; setNZ16(X); }
}

void CPU65816::op_TXA() {
    if (flagM()) { A = (uint16_t)((A & 0xFF00) | (X & 0xFF)); setNZ8((uint8_t)(A & 0xFF)); }
    else         { A = X; setNZ16(A); }
}

void CPU65816::op_TXS() {
    SP = X;
    if (E) SP = (uint16_t)((SP & 0x00FF) | 0x0100);
}

void CPU65816::op_TXY() {
    if (flagX()) { Y = (uint16_t)(X & 0xFF); setNZ8((uint8_t)Y); }
    else         { Y = X; setNZ16(Y); }
}

void CPU65816::op_TYA() {
    if (flagM()) { A = (uint16_t)((A & 0xFF00) | (Y & 0xFF)); setNZ8((uint8_t)(A & 0xFF)); }
    else         { A = Y; setNZ16(A); }
}

void CPU65816::op_TYX() {
    if (flagX()) { X = (uint16_t)(Y & 0xFF); setNZ8((uint8_t)X); }
    else         { X = Y; setNZ16(X); }
}

void CPU65816::op_TRB() {
    if (flagM()) {
        uint8_t v = readByte(addrAbs_);
        setFlag(FLAG_Z, ((A & 0xFF) & v) == 0);
        writeByte(addrAbs_, (uint8_t)(v & ~(A & 0xFF)));
    } else {
        uint16_t v = readWord(addrAbs_);
        setFlag(FLAG_Z, (A & v) == 0);
        writeWord(addrAbs_, (uint16_t)(v & ~A));
        pendingCycles_++;
    }
}

void CPU65816::op_TSB() {
    if (flagM()) {
        uint8_t v = readByte(addrAbs_);
        setFlag(FLAG_Z, ((A & 0xFF) & v) == 0);
        writeByte(addrAbs_, (uint8_t)(v | (A & 0xFF)));
    } else {
        uint16_t v = readWord(addrAbs_);
        setFlag(FLAG_Z, (A & v) == 0);
        writeWord(addrAbs_, (uint16_t)(v | A));
        pendingCycles_++;
    }
}

void CPU65816::op_XBA() {
    A = (uint16_t)((A >> 8) | (A << 8));
    setNZ8((uint8_t)(A & 0xFF));
}

void CPU65816::op_XCE() {
    bool oldE = E;
    bool oldC = (P & FLAG_C) != 0;
    E = oldC;
    setFlag(FLAG_C, oldE);
    if (E) {
        // Переход в emulation: M=1, X=1, SP.h = $01
        P |= (FLAG_M | FLAG_XB);
        SP = (uint16_t)((SP & 0x00FF) | 0x0100);
        X &= 0xFF; Y &= 0xFF;
    }
}

void CPU65816::op_XXX() {
    // Нелегальный опкод — NOP-like
}

// ─── Таблица опкодов (256 записей) ───────────────────────────────────────────

void CPU65816::buildLookupTable() {
    // Инициализируем все как XXX, затем перезаписываем известные
    for (auto& e : lookup_)
        e = { "???", &CPU65816::am_imp, &CPU65816::op_XXX, 2 };

    // $00–$0F
    lookup_[0x00] = { "BRK", &CPU65816::am_imm8,    &CPU65816::op_BRK, 7 };
    lookup_[0x01] = { "ORA", &CPU65816::am_dpIndX,  &CPU65816::op_ORA, 6 };
    lookup_[0x02] = { "COP", &CPU65816::am_imm8,    &CPU65816::op_COP, 7 };
    lookup_[0x03] = { "ORA", &CPU65816::am_sr,      &CPU65816::op_ORA, 4 };
    lookup_[0x04] = { "TSB", &CPU65816::am_dp,      &CPU65816::op_TSB, 5 };
    lookup_[0x05] = { "ORA", &CPU65816::am_dp,      &CPU65816::op_ORA, 3 };
    lookup_[0x06] = { "ASL", &CPU65816::am_dp,      &CPU65816::op_ASL, 5 };
    lookup_[0x07] = { "ORA", &CPU65816::am_dpIndL,  &CPU65816::op_ORA, 6 };
    lookup_[0x08] = { "PHP", &CPU65816::am_imp,     &CPU65816::op_PHP, 3 };
    lookup_[0x09] = { "ORA", &CPU65816::am_immM,    &CPU65816::op_ORA, 2 };
    lookup_[0x0A] = { "ASL", &CPU65816::am_imp,     &CPU65816::op_ASL_a, 2 };
    lookup_[0x0B] = { "PHD", &CPU65816::am_imp,     &CPU65816::op_PHD, 4 };
    lookup_[0x0C] = { "TSB", &CPU65816::am_abs,     &CPU65816::op_TSB, 6 };
    lookup_[0x0D] = { "ORA", &CPU65816::am_abs,     &CPU65816::op_ORA, 4 };
    lookup_[0x0E] = { "ASL", &CPU65816::am_abs,     &CPU65816::op_ASL, 6 };
    lookup_[0x0F] = { "ORA", &CPU65816::am_absL,    &CPU65816::op_ORA, 5 };
    // $10–$1F
    lookup_[0x10] = { "BPL", &CPU65816::am_rel,     &CPU65816::op_BPL, 2 };
    lookup_[0x11] = { "ORA", &CPU65816::am_dpIndY,  &CPU65816::op_ORA, 5 };
    lookup_[0x12] = { "ORA", &CPU65816::am_dpInd,   &CPU65816::op_ORA, 5 };
    lookup_[0x13] = { "ORA", &CPU65816::am_srIndY,  &CPU65816::op_ORA, 7 };
    lookup_[0x14] = { "TRB", &CPU65816::am_dp,      &CPU65816::op_TRB, 5 };
    lookup_[0x15] = { "ORA", &CPU65816::am_dpX,     &CPU65816::op_ORA, 4 };
    lookup_[0x16] = { "ASL", &CPU65816::am_dpX,     &CPU65816::op_ASL, 6 };
    lookup_[0x17] = { "ORA", &CPU65816::am_dpIndLY, &CPU65816::op_ORA, 6 };
    lookup_[0x18] = { "CLC", &CPU65816::am_imp,     &CPU65816::op_CLC, 2 };
    lookup_[0x19] = { "ORA", &CPU65816::am_absY,    &CPU65816::op_ORA, 4 };
    lookup_[0x1A] = { "INC", &CPU65816::am_imp,     &CPU65816::op_INC_a, 2 };
    lookup_[0x1B] = { "TCS", &CPU65816::am_imp,     &CPU65816::op_TCS, 2 };
    lookup_[0x1C] = { "TRB", &CPU65816::am_abs,     &CPU65816::op_TRB, 6 };
    lookup_[0x1D] = { "ORA", &CPU65816::am_absX,    &CPU65816::op_ORA, 4 };
    lookup_[0x1E] = { "ASL", &CPU65816::am_absX,    &CPU65816::op_ASL, 7 };
    lookup_[0x1F] = { "ORA", &CPU65816::am_absLX,   &CPU65816::op_ORA, 5 };
    // $20–$2F
    lookup_[0x20] = { "JSR", &CPU65816::am_abs,     &CPU65816::op_JSR, 6 };
    lookup_[0x21] = { "AND", &CPU65816::am_dpIndX,  &CPU65816::op_AND, 6 };
    lookup_[0x22] = { "JSL", &CPU65816::am_absL,    &CPU65816::op_JSL, 8 };
    lookup_[0x23] = { "AND", &CPU65816::am_sr,      &CPU65816::op_AND, 4 };
    lookup_[0x24] = { "BIT", &CPU65816::am_dp,      &CPU65816::op_BIT, 3 };
    lookup_[0x25] = { "AND", &CPU65816::am_dp,      &CPU65816::op_AND, 3 };
    lookup_[0x26] = { "ROL", &CPU65816::am_dp,      &CPU65816::op_ROL, 5 };
    lookup_[0x27] = { "AND", &CPU65816::am_dpIndL,  &CPU65816::op_AND, 6 };
    lookup_[0x28] = { "PLP", &CPU65816::am_imp,     &CPU65816::op_PLP, 4 };
    lookup_[0x29] = { "AND", &CPU65816::am_immM,    &CPU65816::op_AND, 2 };
    lookup_[0x2A] = { "ROL", &CPU65816::am_imp,     &CPU65816::op_ROL_a, 2 };
    lookup_[0x2B] = { "PLD", &CPU65816::am_imp,     &CPU65816::op_PLD, 5 };
    lookup_[0x2C] = { "BIT", &CPU65816::am_abs,     &CPU65816::op_BIT, 4 };
    lookup_[0x2D] = { "AND", &CPU65816::am_abs,     &CPU65816::op_AND, 4 };
    lookup_[0x2E] = { "ROL", &CPU65816::am_abs,     &CPU65816::op_ROL, 6 };
    lookup_[0x2F] = { "AND", &CPU65816::am_absL,    &CPU65816::op_AND, 5 };
    // $30–$3F
    lookup_[0x30] = { "BMI", &CPU65816::am_rel,     &CPU65816::op_BMI, 2 };
    lookup_[0x31] = { "AND", &CPU65816::am_dpIndY,  &CPU65816::op_AND, 5 };
    lookup_[0x32] = { "AND", &CPU65816::am_dpInd,   &CPU65816::op_AND, 5 };
    lookup_[0x33] = { "AND", &CPU65816::am_srIndY,  &CPU65816::op_AND, 7 };
    lookup_[0x34] = { "BIT", &CPU65816::am_dpX,     &CPU65816::op_BIT, 4 };
    lookup_[0x35] = { "AND", &CPU65816::am_dpX,     &CPU65816::op_AND, 4 };
    lookup_[0x36] = { "ROL", &CPU65816::am_dpX,     &CPU65816::op_ROL, 6 };
    lookup_[0x37] = { "AND", &CPU65816::am_dpIndLY, &CPU65816::op_AND, 6 };
    lookup_[0x38] = { "SEC", &CPU65816::am_imp,     &CPU65816::op_SEC, 2 };
    lookup_[0x39] = { "AND", &CPU65816::am_absY,    &CPU65816::op_AND, 4 };
    lookup_[0x3A] = { "DEC", &CPU65816::am_imp,     &CPU65816::op_DEC_a, 2 };
    lookup_[0x3B] = { "TSC", &CPU65816::am_imp,     &CPU65816::op_TSC, 2 };
    lookup_[0x3C] = { "BIT", &CPU65816::am_absX,    &CPU65816::op_BIT, 4 };
    lookup_[0x3D] = { "AND", &CPU65816::am_absX,    &CPU65816::op_AND, 4 };
    lookup_[0x3E] = { "ROL", &CPU65816::am_absX,    &CPU65816::op_ROL, 7 };
    lookup_[0x3F] = { "AND", &CPU65816::am_absLX,   &CPU65816::op_AND, 5 };
    // $40–$4F
    lookup_[0x40] = { "RTI", &CPU65816::am_imp,     &CPU65816::op_RTI, 6 };
    lookup_[0x41] = { "EOR", &CPU65816::am_dpIndX,  &CPU65816::op_EOR, 6 };
    lookup_[0x42] = { "WDM", &CPU65816::am_imm8,    &CPU65816::op_WDM, 2 };
    lookup_[0x43] = { "EOR", &CPU65816::am_sr,      &CPU65816::op_EOR, 4 };
    lookup_[0x44] = { "MVP", &CPU65816::am_blk,     &CPU65816::op_MVP, 7 };
    lookup_[0x45] = { "EOR", &CPU65816::am_dp,      &CPU65816::op_EOR, 3 };
    lookup_[0x46] = { "LSR", &CPU65816::am_dp,      &CPU65816::op_LSR, 5 };
    lookup_[0x47] = { "EOR", &CPU65816::am_dpIndL,  &CPU65816::op_EOR, 6 };
    lookup_[0x48] = { "PHA", &CPU65816::am_imp,     &CPU65816::op_PHA, 3 };
    lookup_[0x49] = { "EOR", &CPU65816::am_immM,    &CPU65816::op_EOR, 2 };
    lookup_[0x4A] = { "LSR", &CPU65816::am_imp,     &CPU65816::op_LSR_a, 2 };
    lookup_[0x4B] = { "PHK", &CPU65816::am_imp,     &CPU65816::op_PHK, 3 };
    lookup_[0x4C] = { "JMP", &CPU65816::am_abs,     &CPU65816::op_JMP, 3 };
    lookup_[0x4D] = { "EOR", &CPU65816::am_abs,     &CPU65816::op_EOR, 4 };
    lookup_[0x4E] = { "LSR", &CPU65816::am_abs,     &CPU65816::op_LSR, 6 };
    lookup_[0x4F] = { "EOR", &CPU65816::am_absL,    &CPU65816::op_EOR, 5 };
    // $50–$5F
    lookup_[0x50] = { "BVC", &CPU65816::am_rel,     &CPU65816::op_BVC, 2 };
    lookup_[0x51] = { "EOR", &CPU65816::am_dpIndY,  &CPU65816::op_EOR, 5 };
    lookup_[0x52] = { "EOR", &CPU65816::am_dpInd,   &CPU65816::op_EOR, 5 };
    lookup_[0x53] = { "EOR", &CPU65816::am_srIndY,  &CPU65816::op_EOR, 7 };
    lookup_[0x54] = { "MVN", &CPU65816::am_blk,     &CPU65816::op_MVN, 7 };
    lookup_[0x55] = { "EOR", &CPU65816::am_dpX,     &CPU65816::op_EOR, 4 };
    lookup_[0x56] = { "LSR", &CPU65816::am_dpX,     &CPU65816::op_LSR, 6 };
    lookup_[0x57] = { "EOR", &CPU65816::am_dpIndLY, &CPU65816::op_EOR, 6 };
    lookup_[0x58] = { "CLI", &CPU65816::am_imp,     &CPU65816::op_CLI, 2 };
    lookup_[0x59] = { "EOR", &CPU65816::am_absY,    &CPU65816::op_EOR, 4 };
    lookup_[0x5A] = { "PHY", &CPU65816::am_imp,     &CPU65816::op_PHY, 3 };
    lookup_[0x5B] = { "TCD", &CPU65816::am_imp,     &CPU65816::op_TCD, 2 };
    lookup_[0x5C] = { "JML", &CPU65816::am_absL,    &CPU65816::op_JML, 4 };
    lookup_[0x5D] = { "EOR", &CPU65816::am_absX,    &CPU65816::op_EOR, 4 };
    lookup_[0x5E] = { "LSR", &CPU65816::am_absX,    &CPU65816::op_LSR, 7 };
    lookup_[0x5F] = { "EOR", &CPU65816::am_absLX,   &CPU65816::op_EOR, 5 };
    // $60–$6F
    lookup_[0x60] = { "RTS", &CPU65816::am_imp,     &CPU65816::op_RTS, 6 };
    lookup_[0x61] = { "ADC", &CPU65816::am_dpIndX,  &CPU65816::op_ADC, 6 };
    lookup_[0x62] = { "PER", &CPU65816::am_relL,    &CPU65816::op_PER, 6 };
    lookup_[0x63] = { "ADC", &CPU65816::am_sr,      &CPU65816::op_ADC, 4 };
    lookup_[0x64] = { "STZ", &CPU65816::am_dp,      &CPU65816::op_STZ, 3 };
    lookup_[0x65] = { "ADC", &CPU65816::am_dp,      &CPU65816::op_ADC, 3 };
    lookup_[0x66] = { "ROR", &CPU65816::am_dp,      &CPU65816::op_ROR, 5 };
    lookup_[0x67] = { "ADC", &CPU65816::am_dpIndL,  &CPU65816::op_ADC, 6 };
    lookup_[0x68] = { "PLA", &CPU65816::am_imp,     &CPU65816::op_PLA, 4 };
    lookup_[0x69] = { "ADC", &CPU65816::am_immM,    &CPU65816::op_ADC, 2 };
    lookup_[0x6A] = { "ROR", &CPU65816::am_imp,     &CPU65816::op_ROR_a, 2 };
    lookup_[0x6B] = { "RTL", &CPU65816::am_imp,     &CPU65816::op_RTL, 6 };
    lookup_[0x6C] = { "JMP", &CPU65816::am_absInd,  &CPU65816::op_JMP, 5 };
    lookup_[0x6D] = { "ADC", &CPU65816::am_abs,     &CPU65816::op_ADC, 4 };
    lookup_[0x6E] = { "ROR", &CPU65816::am_abs,     &CPU65816::op_ROR, 6 };
    lookup_[0x6F] = { "ADC", &CPU65816::am_absL,    &CPU65816::op_ADC, 5 };
    // $70–$7F
    lookup_[0x70] = { "BVS", &CPU65816::am_rel,     &CPU65816::op_BVS, 2 };
    lookup_[0x71] = { "ADC", &CPU65816::am_dpIndY,  &CPU65816::op_ADC, 5 };
    lookup_[0x72] = { "ADC", &CPU65816::am_dpInd,   &CPU65816::op_ADC, 5 };
    lookup_[0x73] = { "ADC", &CPU65816::am_srIndY,  &CPU65816::op_ADC, 7 };
    lookup_[0x74] = { "STZ", &CPU65816::am_dpX,     &CPU65816::op_STZ, 4 };
    lookup_[0x75] = { "ADC", &CPU65816::am_dpX,     &CPU65816::op_ADC, 4 };
    lookup_[0x76] = { "ROR", &CPU65816::am_dpX,     &CPU65816::op_ROR, 6 };
    lookup_[0x77] = { "ADC", &CPU65816::am_dpIndLY, &CPU65816::op_ADC, 6 };
    lookup_[0x78] = { "SEI", &CPU65816::am_imp,     &CPU65816::op_SEI, 2 };
    lookup_[0x79] = { "ADC", &CPU65816::am_absY,    &CPU65816::op_ADC, 4 };
    lookup_[0x7A] = { "PLY", &CPU65816::am_imp,     &CPU65816::op_PLY, 4 };
    lookup_[0x7B] = { "TDC", &CPU65816::am_imp,     &CPU65816::op_TDC, 2 };
    lookup_[0x7C] = { "JMP", &CPU65816::am_absIndX, &CPU65816::op_JMP, 6 };
    lookup_[0x7D] = { "ADC", &CPU65816::am_absX,    &CPU65816::op_ADC, 4 };
    lookup_[0x7E] = { "ROR", &CPU65816::am_absX,    &CPU65816::op_ROR, 7 };
    lookup_[0x7F] = { "ADC", &CPU65816::am_absLX,   &CPU65816::op_ADC, 5 };
    // $80–$8F
    lookup_[0x80] = { "BRA", &CPU65816::am_rel,     &CPU65816::op_BRA, 3 };
    lookup_[0x81] = { "STA", &CPU65816::am_dpIndX,  &CPU65816::op_STA, 6 };
    lookup_[0x82] = { "BRL", &CPU65816::am_relL,    &CPU65816::op_BRL, 4 };
    lookup_[0x83] = { "STA", &CPU65816::am_sr,      &CPU65816::op_STA, 4 };
    lookup_[0x84] = { "STY", &CPU65816::am_dp,      &CPU65816::op_STY, 3 };
    lookup_[0x85] = { "STA", &CPU65816::am_dp,      &CPU65816::op_STA, 3 };
    lookup_[0x86] = { "STX", &CPU65816::am_dp,      &CPU65816::op_STX, 3 };
    lookup_[0x87] = { "STA", &CPU65816::am_dpIndL,  &CPU65816::op_STA, 6 };
    lookup_[0x88] = { "DEY", &CPU65816::am_imp,     &CPU65816::op_DEY, 2 };
    lookup_[0x89] = { "BIT", &CPU65816::am_immM,    &CPU65816::op_BIT_imm, 2 };
    lookup_[0x8A] = { "TXA", &CPU65816::am_imp,     &CPU65816::op_TXA, 2 };
    lookup_[0x8B] = { "PHB", &CPU65816::am_imp,     &CPU65816::op_PHB, 3 };
    lookup_[0x8C] = { "STY", &CPU65816::am_abs,     &CPU65816::op_STY, 4 };
    lookup_[0x8D] = { "STA", &CPU65816::am_abs,     &CPU65816::op_STA, 4 };
    lookup_[0x8E] = { "STX", &CPU65816::am_abs,     &CPU65816::op_STX, 4 };
    lookup_[0x8F] = { "STA", &CPU65816::am_absL,    &CPU65816::op_STA, 5 };
    // $90–$9F
    lookup_[0x90] = { "BCC", &CPU65816::am_rel,     &CPU65816::op_BCC, 2 };
    lookup_[0x91] = { "STA", &CPU65816::am_dpIndY,  &CPU65816::op_STA, 6 };
    lookup_[0x92] = { "STA", &CPU65816::am_dpInd,   &CPU65816::op_STA, 5 };
    lookup_[0x93] = { "STA", &CPU65816::am_srIndY,  &CPU65816::op_STA, 7 };
    lookup_[0x94] = { "STY", &CPU65816::am_dpX,     &CPU65816::op_STY, 4 };
    lookup_[0x95] = { "STA", &CPU65816::am_dpX,     &CPU65816::op_STA, 4 };
    lookup_[0x96] = { "STX", &CPU65816::am_dpY,     &CPU65816::op_STX, 4 };
    lookup_[0x97] = { "STA", &CPU65816::am_dpIndLY, &CPU65816::op_STA, 6 };
    lookup_[0x98] = { "TYA", &CPU65816::am_imp,     &CPU65816::op_TYA, 2 };
    lookup_[0x99] = { "STA", &CPU65816::am_absY,    &CPU65816::op_STA, 5 };
    lookup_[0x9A] = { "TXS", &CPU65816::am_imp,     &CPU65816::op_TXS, 2 };
    lookup_[0x9B] = { "TXY", &CPU65816::am_imp,     &CPU65816::op_TXY, 2 };
    lookup_[0x9C] = { "STZ", &CPU65816::am_abs,     &CPU65816::op_STZ, 4 };
    lookup_[0x9D] = { "STA", &CPU65816::am_absX,    &CPU65816::op_STA, 5 };
    lookup_[0x9E] = { "STZ", &CPU65816::am_absX,    &CPU65816::op_STZ, 5 };
    lookup_[0x9F] = { "STA", &CPU65816::am_absLX,   &CPU65816::op_STA, 5 };
    // $A0–$AF
    lookup_[0xA0] = { "LDY", &CPU65816::am_immX,    &CPU65816::op_LDY, 2 };
    lookup_[0xA1] = { "LDA", &CPU65816::am_dpIndX,  &CPU65816::op_LDA, 6 };
    lookup_[0xA2] = { "LDX", &CPU65816::am_immX,    &CPU65816::op_LDX, 2 };
    lookup_[0xA3] = { "LDA", &CPU65816::am_sr,      &CPU65816::op_LDA, 4 };
    lookup_[0xA4] = { "LDY", &CPU65816::am_dp,      &CPU65816::op_LDY, 3 };
    lookup_[0xA5] = { "LDA", &CPU65816::am_dp,      &CPU65816::op_LDA, 3 };
    lookup_[0xA6] = { "LDX", &CPU65816::am_dp,      &CPU65816::op_LDX, 3 };
    lookup_[0xA7] = { "LDA", &CPU65816::am_dpIndL,  &CPU65816::op_LDA, 6 };
    lookup_[0xA8] = { "TAY", &CPU65816::am_imp,     &CPU65816::op_TAY, 2 };
    lookup_[0xA9] = { "LDA", &CPU65816::am_immM,    &CPU65816::op_LDA, 2 };
    lookup_[0xAA] = { "TAX", &CPU65816::am_imp,     &CPU65816::op_TAX, 2 };
    lookup_[0xAB] = { "PLB", &CPU65816::am_imp,     &CPU65816::op_PLB, 4 };
    lookup_[0xAC] = { "LDY", &CPU65816::am_abs,     &CPU65816::op_LDY, 4 };
    lookup_[0xAD] = { "LDA", &CPU65816::am_abs,     &CPU65816::op_LDA, 4 };
    lookup_[0xAE] = { "LDX", &CPU65816::am_abs,     &CPU65816::op_LDX, 4 };
    lookup_[0xAF] = { "LDA", &CPU65816::am_absL,    &CPU65816::op_LDA, 5 };
    // $B0–$BF
    lookup_[0xB0] = { "BCS", &CPU65816::am_rel,     &CPU65816::op_BCS, 2 };
    lookup_[0xB1] = { "LDA", &CPU65816::am_dpIndY,  &CPU65816::op_LDA, 5 };
    lookup_[0xB2] = { "LDA", &CPU65816::am_dpInd,   &CPU65816::op_LDA, 5 };
    lookup_[0xB3] = { "LDA", &CPU65816::am_srIndY,  &CPU65816::op_LDA, 7 };
    lookup_[0xB4] = { "LDY", &CPU65816::am_dpX,     &CPU65816::op_LDY, 4 };
    lookup_[0xB5] = { "LDA", &CPU65816::am_dpX,     &CPU65816::op_LDA, 4 };
    lookup_[0xB6] = { "LDX", &CPU65816::am_dpY,     &CPU65816::op_LDX, 4 };
    lookup_[0xB7] = { "LDA", &CPU65816::am_dpIndLY, &CPU65816::op_LDA, 6 };
    lookup_[0xB8] = { "CLV", &CPU65816::am_imp,     &CPU65816::op_CLV, 2 };
    lookup_[0xB9] = { "LDA", &CPU65816::am_absY,    &CPU65816::op_LDA, 4 };
    lookup_[0xBA] = { "TSX", &CPU65816::am_imp,     &CPU65816::op_TSX, 2 };
    lookup_[0xBB] = { "TYX", &CPU65816::am_imp,     &CPU65816::op_TYX, 2 };
    lookup_[0xBC] = { "LDY", &CPU65816::am_absX,    &CPU65816::op_LDY, 4 };
    lookup_[0xBD] = { "LDA", &CPU65816::am_absX,    &CPU65816::op_LDA, 4 };
    lookup_[0xBE] = { "LDX", &CPU65816::am_absY,    &CPU65816::op_LDX, 4 };
    lookup_[0xBF] = { "LDA", &CPU65816::am_absLX,   &CPU65816::op_LDA, 5 };
    // $C0–$CF
    lookup_[0xC0] = { "CPY", &CPU65816::am_immX,    &CPU65816::op_CPY, 2 };
    lookup_[0xC1] = { "CMP", &CPU65816::am_dpIndX,  &CPU65816::op_CMP, 6 };
    lookup_[0xC2] = { "REP", &CPU65816::am_imm8,    &CPU65816::op_REP, 3 };
    lookup_[0xC3] = { "CMP", &CPU65816::am_sr,      &CPU65816::op_CMP, 4 };
    lookup_[0xC4] = { "CPY", &CPU65816::am_dp,      &CPU65816::op_CPY, 3 };
    lookup_[0xC5] = { "CMP", &CPU65816::am_dp,      &CPU65816::op_CMP, 3 };
    lookup_[0xC6] = { "DEC", &CPU65816::am_dp,      &CPU65816::op_DEC, 5 };
    lookup_[0xC7] = { "CMP", &CPU65816::am_dpIndL,  &CPU65816::op_CMP, 6 };
    lookup_[0xC8] = { "INY", &CPU65816::am_imp,     &CPU65816::op_INY, 2 };
    lookup_[0xC9] = { "CMP", &CPU65816::am_immM,    &CPU65816::op_CMP, 2 };
    lookup_[0xCA] = { "DEX", &CPU65816::am_imp,     &CPU65816::op_DEX, 2 };
    lookup_[0xCB] = { "WAI", &CPU65816::am_imp,     &CPU65816::op_WAI, 3 };
    lookup_[0xCC] = { "CPY", &CPU65816::am_abs,     &CPU65816::op_CPY, 4 };
    lookup_[0xCD] = { "CMP", &CPU65816::am_abs,     &CPU65816::op_CMP, 4 };
    lookup_[0xCE] = { "DEC", &CPU65816::am_abs,     &CPU65816::op_DEC, 6 };
    lookup_[0xCF] = { "CMP", &CPU65816::am_absL,    &CPU65816::op_CMP, 5 };
    // $D0–$DF
    lookup_[0xD0] = { "BNE", &CPU65816::am_rel,     &CPU65816::op_BNE, 2 };
    lookup_[0xD1] = { "CMP", &CPU65816::am_dpIndY,  &CPU65816::op_CMP, 5 };
    lookup_[0xD2] = { "CMP", &CPU65816::am_dpInd,   &CPU65816::op_CMP, 5 };
    lookup_[0xD3] = { "CMP", &CPU65816::am_srIndY,  &CPU65816::op_CMP, 7 };
    lookup_[0xD4] = { "PEI", &CPU65816::am_dp,      &CPU65816::op_PEI, 6 };
    lookup_[0xD5] = { "CMP", &CPU65816::am_dpX,     &CPU65816::op_CMP, 4 };
    lookup_[0xD6] = { "DEC", &CPU65816::am_dpX,     &CPU65816::op_DEC, 6 };
    lookup_[0xD7] = { "CMP", &CPU65816::am_dpIndLY, &CPU65816::op_CMP, 6 };
    lookup_[0xD8] = { "CLD", &CPU65816::am_imp,     &CPU65816::op_CLD, 2 };
    lookup_[0xD9] = { "CMP", &CPU65816::am_absY,    &CPU65816::op_CMP, 4 };
    lookup_[0xDA] = { "PHX", &CPU65816::am_imp,     &CPU65816::op_PHX, 3 };
    lookup_[0xDB] = { "STP", &CPU65816::am_imp,     &CPU65816::op_STP, 3 };
    lookup_[0xDC] = { "JML", &CPU65816::am_absIndL, &CPU65816::op_JML, 6 };
    lookup_[0xDD] = { "CMP", &CPU65816::am_absX,    &CPU65816::op_CMP, 4 };
    lookup_[0xDE] = { "DEC", &CPU65816::am_absX,    &CPU65816::op_DEC, 7 };
    lookup_[0xDF] = { "CMP", &CPU65816::am_absLX,   &CPU65816::op_CMP, 5 };
    // $E0–$EF
    lookup_[0xE0] = { "CPX", &CPU65816::am_immX,    &CPU65816::op_CPX, 2 };
    lookup_[0xE1] = { "SBC", &CPU65816::am_dpIndX,  &CPU65816::op_SBC, 6 };
    lookup_[0xE2] = { "SEP", &CPU65816::am_imm8,    &CPU65816::op_SEP, 3 };
    lookup_[0xE3] = { "SBC", &CPU65816::am_sr,      &CPU65816::op_SBC, 4 };
    lookup_[0xE4] = { "CPX", &CPU65816::am_dp,      &CPU65816::op_CPX, 3 };
    lookup_[0xE5] = { "SBC", &CPU65816::am_dp,      &CPU65816::op_SBC, 3 };
    lookup_[0xE6] = { "INC", &CPU65816::am_dp,      &CPU65816::op_INC, 5 };
    lookup_[0xE7] = { "SBC", &CPU65816::am_dpIndL,  &CPU65816::op_SBC, 6 };
    lookup_[0xE8] = { "INX", &CPU65816::am_imp,     &CPU65816::op_INX, 2 };
    lookup_[0xE9] = { "SBC", &CPU65816::am_immM,    &CPU65816::op_SBC, 2 };
    lookup_[0xEA] = { "NOP", &CPU65816::am_imp,     &CPU65816::op_NOP, 2 };
    lookup_[0xEB] = { "XBA", &CPU65816::am_imp,     &CPU65816::op_XBA, 3 };
    lookup_[0xEC] = { "CPX", &CPU65816::am_abs,     &CPU65816::op_CPX, 4 };
    lookup_[0xED] = { "SBC", &CPU65816::am_abs,     &CPU65816::op_SBC, 4 };
    lookup_[0xEE] = { "INC", &CPU65816::am_abs,     &CPU65816::op_INC, 6 };
    lookup_[0xEF] = { "SBC", &CPU65816::am_absL,    &CPU65816::op_SBC, 5 };
    // $F0–$FF
    lookup_[0xF0] = { "BEQ", &CPU65816::am_rel,     &CPU65816::op_BEQ, 2 };
    lookup_[0xF1] = { "SBC", &CPU65816::am_dpIndY,  &CPU65816::op_SBC, 5 };
    lookup_[0xF2] = { "SBC", &CPU65816::am_dpInd,   &CPU65816::op_SBC, 5 };
    lookup_[0xF3] = { "SBC", &CPU65816::am_srIndY,  &CPU65816::op_SBC, 7 };
    lookup_[0xF4] = { "PEA", &CPU65816::am_abs,     &CPU65816::op_PEA, 5 };
    lookup_[0xF5] = { "SBC", &CPU65816::am_dpX,     &CPU65816::op_SBC, 4 };
    lookup_[0xF6] = { "INC", &CPU65816::am_dpX,     &CPU65816::op_INC, 6 };
    lookup_[0xF7] = { "SBC", &CPU65816::am_dpIndLY, &CPU65816::op_SBC, 6 };
    lookup_[0xF8] = { "SED", &CPU65816::am_imp,     &CPU65816::op_SED, 2 };
    lookup_[0xF9] = { "SBC", &CPU65816::am_absY,    &CPU65816::op_SBC, 4 };
    lookup_[0xFA] = { "PLX", &CPU65816::am_imp,     &CPU65816::op_PLX, 4 };
    lookup_[0xFB] = { "XCE", &CPU65816::am_imp,     &CPU65816::op_XCE, 2 };
    lookup_[0xFC] = { "JSR", &CPU65816::am_absIndX, &CPU65816::op_JSR_indX, 8 };
    lookup_[0xFD] = { "SBC", &CPU65816::am_absX,    &CPU65816::op_SBC, 4 };
    lookup_[0xFE] = { "INC", &CPU65816::am_absX,    &CPU65816::op_INC, 7 };
    lookup_[0xFF] = { "SBC", &CPU65816::am_absLX,   &CPU65816::op_SBC, 5 };
}

// ─── Прерывания ──────────────────────────────────────────────────────────────

void CPU65816::reset() {
    E   = true;
    P   = (uint8_t)(FLAG_M | FLAG_XB | FLAG_I);
    D   = 0;
    DBR = 0;
    PBR = 0;
    SP  = 0x01FF;
    stopped_ = false;
    waiting_ = false;
    // Вектор сброса: $FFFC/$FFFD (всегда банк 0)
    PC = readWord(0xFFFC);
    totalCycles_   = 0;
    pendingCycles_ = 7;
}

void CPU65816::nmi() {
    waiting_ = false;
    if (E) {
        push16(PC);
        push8(P);
        PBR = 0;
        PC = readWord(0xFFFA);
    } else {
        push8(PBR);
        push16(PC);
        push8(P);
        PBR = 0;
        PC = readWord(0xFFEA);
    }
    setFlag(FLAG_I, true);
    pendingCycles_ += 7;
}

void CPU65816::irq() {
    if (P & FLAG_I) return;
    waiting_ = false;
    if (E) {
        push16(PC);
        push8((uint8_t)(P & ~FLAG_XB));
        PBR = 0;
        PC = readWord(0xFFFE);
    } else {
        push8(PBR);
        push16(PC);
        push8(P);
        PBR = 0;
        PC = readWord(0xFFEE);
    }
    setFlag(FLAG_I, true);
    pendingCycles_ += 7;
}

// ─── Главный цикл ────────────────────────────────────────────────────────────

void CPU65816::clock() {
    if (stopped_) return;
    if (waiting_) return;  // ждём прерывания (WAI)

    opcode_ = fetchByte();
    const Instruction& instr = lookup_[opcode_];
    pendingCycles_ = instr.cycles;

    (this->*instr.addrmode)();
    (this->*instr.operation)();

    totalCycles_ += (uint64_t)pendingCycles_;
}
