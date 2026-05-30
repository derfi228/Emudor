#include "cpu.h"
#include "memory/memory_bus.h"
#include <sstream>
#include <iomanip>
#include <cassert>

// ─── Вспомогательные ────────────────────────────────────────────────────────

CPU::CPU() { buildLookupTable(); }

void CPU::connectBus(MemoryBus* bus) { bus_ = bus; }

uint8_t CPU::read(uint16_t addr)           { return bus_->read(addr); }
void    CPU::write(uint16_t addr, uint8_t d){ bus_->write(addr, d); }

bool CPU::getFlag(Flag f) const { return (P & f) != 0; }
void CPU::setFlag(Flag f, bool v) {
    if (v) P |= f;
    else   P &= ~f;
}

void CPU::push(uint8_t val) {
    write(0x0100 + SP, val);
    SP--;
}

uint8_t CPU::pop() {
    SP++;
    return read(0x0100 + SP);
}

uint8_t CPU::fetch() {
    if (!(lookup_[opcode_].addrmode == &CPU::IMP ||
          lookup_[opcode_].addrmode == &CPU::ACC))
        fetchedVal_ = read(addrAbs_);
    return fetchedVal_;
}

// ─── Reset / IRQ / NMI ──────────────────────────────────────────────────────

void CPU::reset() {
    uint16_t lo = read(0xFFFC);
    uint16_t hi = read(0xFFFD);
    PC = (hi << 8) | lo;

    A = X = Y = 0;
    SP = 0xFD;
    P  = 0x24;  // I и U установлены

    addrAbs_ = addrRel_ = 0;
    fetchedVal_ = 0;
    remainingCycles = 7;
    totalCycles_ = 7;
}

void CPU::irq() {
    if (getFlag(I)) return;
    push((PC >> 8) & 0xFF);
    push(PC & 0xFF);
    // Пушим P до установки I — RTI восстановит оригинальный I
    setFlag(B, false);
    setFlag(U, true);
    push(P);
    setFlag(I, true);
    uint16_t lo = read(0xFFFE);
    uint16_t hi = read(0xFFFF);
    PC = (hi << 8) | lo;
    remainingCycles = 7;
}

void CPU::nmi() {
    push((PC >> 8) & 0xFF);
    push(PC & 0xFF);
    // Пушим P до установки I — RTI восстановит оригинальный I
    setFlag(B, false);
    setFlag(U, true);
    push(P);
    setFlag(I, true);
    uint16_t lo = read(0xFFFA);
    uint16_t hi = read(0xFFFB);
    PC = (hi << 8) | lo;
    remainingCycles = 8;
}

// ─── Clock ──────────────────────────────────────────────────────────────────

void CPU::clock() {
    if (remainingCycles > 0) {
        remainingCycles--;
        totalCycles_++;
        return;
    }

    opcode_ = read(PC);
    PC++;

    isAccumulatorMode_ = (lookup_[opcode_].addrmode == &CPU::ACC);

    setFlag(U, true);

    const auto& ins = lookup_[opcode_];
    uint8_t extraCycles = (this->*ins.addrmode)();
    extraCycles       &= (this->*ins.operate)();

    remainingCycles = ins.cycles + extraCycles - 1;
    totalCycles_++;
}

// ─── Trace ──────────────────────────────────────────────────────────────────

std::string CPU::trace(int ppuScanline, int ppuDot) {
    // Читаем байты инструкции без побочных эффектов
    uint8_t op  = bus_->read(PC, true);
    uint8_t b1  = bus_->read(PC + 1, true);
    uint8_t b2  = bus_->read(PC + 2, true);

    const auto& ins = lookup_[op];

    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0');

    // Адрес
    oss << std::setw(4) << (unsigned)PC << "  ";

    // Байты инструкции
    auto addrFn = ins.addrmode;
    bool is2 = (addrFn == &CPU::IMM || addrFn == &CPU::ZP0 ||
                addrFn == &CPU::ZPX || addrFn == &CPU::ZPY ||
                addrFn == &CPU::IZX || addrFn == &CPU::IZY ||
                addrFn == &CPU::REL);
    bool is3 = (addrFn == &CPU::ABS || addrFn == &CPU::ABX ||
                addrFn == &CPU::ABY || addrFn == &CPU::IND);

    // Байты: opcode (2) + space (1) + остаток = 10 символов итого
    oss << std::setw(2) << (unsigned)op << " ";
    if (is2)      oss << std::setw(2) << (unsigned)b1 << "     ";       // 7 символов
    else if (is3) oss << std::setw(2) << (unsigned)b1 << " " << std::setw(2) << (unsigned)b2 << "  "; // 7
    else          oss << "       ";  // 7 символов — 1-байтовая инструкция

    // Мнемоника: ровно 3 символа без ведущего пробела, начинается с позиции 16
    oss << ins.name;

    // Операнд: пробел-разделитель + 28 символов = итого позиции 19–47
    oss << " ";
    if (addrFn == &CPU::IMM) {
        oss << "#$" << std::setw(2) << (unsigned)b1 << "                        ";  // 4+24=28
    } else if (addrFn == &CPU::ZP0) {
        uint8_t  val = bus_->read(b1, true);
        oss << "$" << std::setw(2) << (unsigned)b1 << " = " << std::setw(2) << (unsigned)val << "                    ";
    } else if (addrFn == &CPU::ZPX) {
        uint8_t eff = (b1 + X) & 0xFF;
        uint8_t val = bus_->read(eff, true);
        oss << "$" << std::setw(2) << (unsigned)b1 << ",X @ " << std::setw(2) << (unsigned)eff
            << " = " << std::setw(2) << (unsigned)val << "             ";
    } else if (addrFn == &CPU::ZPY) {
        uint8_t eff = (b1 + Y) & 0xFF;
        uint8_t val = bus_->read(eff, true);
        oss << "$" << std::setw(2) << (unsigned)b1 << ",Y @ " << std::setw(2) << (unsigned)eff
            << " = " << std::setw(2) << (unsigned)val << "             ";
    } else if (addrFn == &CPU::IZX) {
        uint8_t zp  = (b1 + X) & 0xFF;
        uint16_t lo = bus_->read(zp,     true);
        uint16_t hi = bus_->read((zp+1)&0xFF, true);
        uint16_t eff = (hi << 8) | lo;
        uint8_t  val = bus_->read(eff, true);
        oss << "($" << std::setw(2) << (unsigned)b1 << ",X) @ " << std::setw(2) << (unsigned)zp
            << " = " << std::setw(4) << (unsigned)eff << " = " << std::setw(2) << (unsigned)val << "    ";
    } else if (addrFn == &CPU::IZY) {
        uint16_t lo = bus_->read(b1,       true);
        uint16_t hi = bus_->read((b1+1)&0xFF, true);
        uint16_t base = (hi << 8) | lo;
        uint16_t eff  = base + Y;
        uint8_t  val  = bus_->read(eff, true);
        oss << "($" << std::setw(2) << (unsigned)b1 << "),Y = " << std::setw(4) << (unsigned)base
            << " @ " << std::setw(4) << (unsigned)eff << " = " << std::setw(2) << (unsigned)val << "  ";  // 26+2=28
    } else if (addrFn == &CPU::ABS) {
        uint16_t addr = (b2 << 8) | b1;
        auto opFn = ins.operate;
        if (opFn == &CPU::JMP || opFn == &CPU::JSR) {
            oss << "$" << std::setw(4) << (unsigned)addr << "                       ";
        } else {
            uint8_t val = bus_->read(addr, true);
            oss << "$" << std::setw(4) << (unsigned)addr << " = " << std::setw(2) << (unsigned)val << "                  ";
        }
    } else if (addrFn == &CPU::ABX) {
        uint16_t base = (b2 << 8) | b1;
        uint16_t eff  = base + X;
        uint8_t  val  = bus_->read(eff, true);
        oss << "$" << std::setw(4) << (unsigned)base << ",X @ " << std::setw(4) << (unsigned)eff
            << " = " << std::setw(2) << (unsigned)val << "         ";
    } else if (addrFn == &CPU::ABY) {
        uint16_t base = (b2 << 8) | b1;
        uint16_t eff  = base + Y;
        uint8_t  val  = bus_->read(eff, true);
        oss << "$" << std::setw(4) << (unsigned)base << ",Y @ " << std::setw(4) << (unsigned)eff
            << " = " << std::setw(2) << (unsigned)val << "         ";
    } else if (addrFn == &CPU::IND) {
        uint16_t ptr = (b2 << 8) | b1;
        uint16_t lo, hi;
        if ((ptr & 0xFF) == 0xFF)
            lo = bus_->read(ptr, true), hi = bus_->read(ptr & 0xFF00, true);
        else
            lo = bus_->read(ptr, true), hi = bus_->read(ptr + 1, true);
        uint16_t eff = (hi << 8) | lo;
        oss << "($" << std::setw(4) << (unsigned)ptr << ") = " << std::setw(4) << (unsigned)eff << "              ";
    } else if (addrFn == &CPU::REL) {
        int8_t  off  = static_cast<int8_t>(b1);
        uint16_t eff = PC + 2 + off;
        oss << "$" << std::setw(4) << (unsigned)eff << "                       ";
    } else if (addrFn == &CPU::ACC) {
        oss << "A                           ";
    } else {
        oss << "                            ";
    }

    // Регистры: начинаются с позиции 48, без ведущего пробела
    oss << "A:" << std::setw(2) << (unsigned)A
        << " X:" << std::setw(2) << (unsigned)X
        << " Y:" << std::setw(2) << (unsigned)Y
        << " P:" << std::setw(2) << (unsigned)P
        << " SP:" << std::setw(2) << (unsigned)SP;

    // PPU и циклы
    oss << std::dec
        << " PPU:" << std::setw(3) << std::setfill(' ') << ppuScanline
        << "," << std::setw(3) << std::setfill(' ') << ppuDot
        << " CYC:" << totalCycles_;

    return oss.str();
}

// ─── Режимы адресации ────────────────────────────────────────────────────────

uint8_t CPU::IMP() { fetchedVal_ = A; return 0; }
uint8_t CPU::ACC() { fetchedVal_ = A; return 0; }

uint8_t CPU::IMM() {
    addrAbs_ = PC++;
    return 0;
}

uint8_t CPU::ZP0() {
    addrAbs_ = read(PC++) & 0x00FF;
    return 0;
}

uint8_t CPU::ZPX() {
    addrAbs_ = (read(PC++) + X) & 0x00FF;
    return 0;
}

uint8_t CPU::ZPY() {
    addrAbs_ = (read(PC++) + Y) & 0x00FF;
    return 0;
}

uint8_t CPU::REL() {
    addrRel_ = read(PC++);
    if (addrRel_ & 0x80) addrRel_ |= 0xFF00;  // знаковое расширение
    return 0;
}

uint8_t CPU::ABS() {
    uint16_t lo = read(PC++);
    uint16_t hi = read(PC++);
    addrAbs_ = (hi << 8) | lo;
    return 0;
}

uint8_t CPU::ABX() {
    uint16_t lo = read(PC++);
    uint16_t hi = read(PC++);
    addrAbs_ = ((hi << 8) | lo) + X;
    return ((addrAbs_ & 0xFF00) != (hi << 8)) ? 1 : 0;
}

uint8_t CPU::ABY() {
    uint16_t lo = read(PC++);
    uint16_t hi = read(PC++);
    addrAbs_ = ((hi << 8) | lo) + Y;
    return ((addrAbs_ & 0xFF00) != (hi << 8)) ? 1 : 0;
}

uint8_t CPU::IND() {
    uint16_t lo = read(PC++);
    uint16_t hi = read(PC++);
    uint16_t ptr = (hi << 8) | lo;
    // Баг 6502: если lo-байт == $FF, старший берётся из $xx00
    if ((ptr & 0x00FF) == 0x00FF)
        addrAbs_ = (read(ptr & 0xFF00) << 8) | read(ptr);
    else
        addrAbs_ = (read(ptr + 1) << 8) | read(ptr);
    return 0;
}

uint8_t CPU::IZX() {
    uint8_t t  = read(PC++);
    uint8_t lo = read((uint16_t)(t + X) & 0x00FF);
    uint8_t hi = read((uint16_t)(t + X + 1) & 0x00FF);
    addrAbs_ = (hi << 8) | lo;
    return 0;
}

uint8_t CPU::IZY() {
    uint8_t t  = read(PC++);
    uint16_t lo = read(t & 0x00FF);
    uint16_t hi = read((t + 1) & 0x00FF);
    addrAbs_ = ((hi << 8) | lo) + Y;
    return ((addrAbs_ & 0xFF00) != (hi << 8)) ? 1 : 0;
}

// ─── Инструкции ─────────────────────────────────────────────────────────────

uint8_t CPU::ADC() {
    fetch();
    uint16_t result = (uint16_t)A + fetchedVal_ + getFlag(C);
    setFlag(C, result > 0xFF);
    setFlag(Z, (result & 0xFF) == 0);
    setFlag(N, result & 0x80);
    setFlag(V, (~((uint16_t)A ^ fetchedVal_) & ((uint16_t)A ^ result)) & 0x80);
    A = result & 0xFF;
    return 1;
}

uint8_t CPU::SBC() {
    fetch();
    // SBC = ADC с операндом XOR 0xFF
    uint16_t val    = (uint16_t)fetchedVal_ ^ 0x00FF;
    uint16_t result = (uint16_t)A + val + getFlag(C);
    setFlag(C, result > 0xFF);
    setFlag(Z, (result & 0xFF) == 0);
    setFlag(N, result & 0x80);
    setFlag(V, ((uint16_t)A ^ result) & (val ^ result) & 0x80);
    A = result & 0xFF;
    return 1;
}

uint8_t CPU::AND() {
    fetch();
    A = A & fetchedVal_;
    setFlag(Z, A == 0);
    setFlag(N, A & 0x80);
    return 1;
}

uint8_t CPU::EOR() {
    fetch();
    A = A ^ fetchedVal_;
    setFlag(Z, A == 0);
    setFlag(N, A & 0x80);
    return 1;
}

uint8_t CPU::ORA() {
    fetch();
    A = A | fetchedVal_;
    setFlag(Z, A == 0);
    setFlag(N, A & 0x80);
    return 1;
}

uint8_t CPU::ASL() {
    fetch();
    uint16_t tmp = (uint16_t)fetchedVal_ << 1;
    setFlag(C, tmp & 0xFF00);
    setFlag(Z, (tmp & 0xFF) == 0);
    setFlag(N, tmp & 0x80);
    if (isAccumulatorMode_) A = tmp & 0xFF;
    else write(addrAbs_, tmp & 0xFF);
    return 0;
}

uint8_t CPU::LSR() {
    fetch();
    setFlag(C, fetchedVal_ & 0x01);
    uint8_t tmp = fetchedVal_ >> 1;
    setFlag(Z, tmp == 0);
    setFlag(N, tmp & 0x80);
    if (isAccumulatorMode_) A = tmp;
    else write(addrAbs_, tmp);
    return 0;
}

uint8_t CPU::ROL() {
    fetch();
    uint16_t tmp = ((uint16_t)fetchedVal_ << 1) | getFlag(C);
    setFlag(C, tmp & 0xFF00);
    setFlag(Z, (tmp & 0xFF) == 0);
    setFlag(N, tmp & 0x80);
    if (isAccumulatorMode_) A = tmp & 0xFF;
    else write(addrAbs_, tmp & 0xFF);
    return 0;
}

uint8_t CPU::ROR() {
    fetch();
    uint16_t tmp = ((uint16_t)getFlag(C) << 7) | (fetchedVal_ >> 1);
    setFlag(C, fetchedVal_ & 0x01);
    setFlag(Z, (tmp & 0xFF) == 0);
    setFlag(N, tmp & 0x80);
    if (isAccumulatorMode_) A = tmp & 0xFF;
    else write(addrAbs_, tmp & 0xFF);
    return 0;
}

uint8_t CPU::BIT() {
    fetch();
    uint8_t tmp = A & fetchedVal_;
    setFlag(Z, tmp == 0);
    setFlag(N, fetchedVal_ & 0x80);
    setFlag(V, fetchedVal_ & 0x40);
    return 0;
}

uint8_t CPU::CMP() {
    fetch();
    uint16_t tmp = (uint16_t)A - fetchedVal_;
    setFlag(C, A >= fetchedVal_);
    setFlag(Z, (tmp & 0xFF) == 0);
    setFlag(N, tmp & 0x80);
    return 1;
}

uint8_t CPU::CPX() {
    fetch();
    uint16_t tmp = (uint16_t)X - fetchedVal_;
    setFlag(C, X >= fetchedVal_);
    setFlag(Z, (tmp & 0xFF) == 0);
    setFlag(N, tmp & 0x80);
    return 0;
}

uint8_t CPU::CPY() {
    fetch();
    uint16_t tmp = (uint16_t)Y - fetchedVal_;
    setFlag(C, Y >= fetchedVal_);
    setFlag(Z, (tmp & 0xFF) == 0);
    setFlag(N, tmp & 0x80);
    return 0;
}

uint8_t CPU::INC() {
    fetch();
    uint8_t tmp = fetchedVal_ + 1;
    write(addrAbs_, tmp);
    setFlag(Z, tmp == 0);
    setFlag(N, tmp & 0x80);
    return 0;
}

uint8_t CPU::DEC() {
    fetch();
    uint8_t tmp = fetchedVal_ - 1;
    write(addrAbs_, tmp);
    setFlag(Z, tmp == 0);
    setFlag(N, tmp & 0x80);
    return 0;
}

uint8_t CPU::INX() { X++; setFlag(Z, X==0); setFlag(N, X&0x80); return 0; }
uint8_t CPU::INY() { Y++; setFlag(Z, Y==0); setFlag(N, Y&0x80); return 0; }
uint8_t CPU::DEX() { X--; setFlag(Z, X==0); setFlag(N, X&0x80); return 0; }
uint8_t CPU::DEY() { Y--; setFlag(Z, Y==0); setFlag(N, Y&0x80); return 0; }

uint8_t CPU::LDA() { fetch(); A = fetchedVal_; setFlag(Z,A==0); setFlag(N,A&0x80); return 1; }
uint8_t CPU::LDX() { fetch(); X = fetchedVal_; setFlag(Z,X==0); setFlag(N,X&0x80); return 1; }
uint8_t CPU::LDY() { fetch(); Y = fetchedVal_; setFlag(Z,Y==0); setFlag(N,Y&0x80); return 1; }

uint8_t CPU::STA() { write(addrAbs_, A); return 0; }
uint8_t CPU::STX() { write(addrAbs_, X); return 0; }
uint8_t CPU::STY() { write(addrAbs_, Y); return 0; }

uint8_t CPU::TAX() { X=A; setFlag(Z,X==0); setFlag(N,X&0x80); return 0; }
uint8_t CPU::TAY() { Y=A; setFlag(Z,Y==0); setFlag(N,Y&0x80); return 0; }
uint8_t CPU::TXA() { A=X; setFlag(Z,A==0); setFlag(N,A&0x80); return 0; }
uint8_t CPU::TYA() { A=Y; setFlag(Z,A==0); setFlag(N,A&0x80); return 0; }
uint8_t CPU::TSX() { X=SP; setFlag(Z,X==0); setFlag(N,X&0x80); return 0; }
uint8_t CPU::TXS() { SP=X; return 0; }

uint8_t CPU::PHA() { push(A); return 0; }
uint8_t CPU::PHP() { push(P | B | U); return 0; }

uint8_t CPU::PLA() {
    A = pop();
    setFlag(Z, A==0);
    setFlag(N, A&0x80);
    return 0;
}

uint8_t CPU::PLP() {
    P = pop();
    setFlag(U, true);
    setFlag(B, false);
    return 0;
}

uint8_t CPU::JMP() { PC = addrAbs_; return 0; }

uint8_t CPU::JSR() {
    PC--;  // пушим PC-1
    push((PC >> 8) & 0xFF);
    push(PC & 0xFF);
    PC = addrAbs_;
    return 0;
}

uint8_t CPU::RTS() {
    uint16_t lo = pop();
    uint16_t hi = pop();
    PC = ((hi << 8) | lo) + 1;
    return 0;
}

uint8_t CPU::BRK() {
    PC++;  // signature byte — пропускаем, PC теперь указывает на инструкцию после BRK+сигнатуры
    push((PC >> 8) & 0xFF);
    push(PC & 0xFF);
    // B=1 и U=1 идут только в пушнутую копию P, не в реальный регистр
    push(P | B | U);
    setFlag(I, true);   // I ставим ПОСЛЕ пуша, как на реальном 6502
    setFlag(B, false);
    uint16_t lo = read(0xFFFE);
    uint16_t hi = read(0xFFFF);
    PC = (hi << 8) | lo;
    return 0;
}

uint8_t CPU::RTI() {
    P = pop();
    setFlag(U, true);
    setFlag(B, false);
    uint16_t lo = pop();
    uint16_t hi = pop();
    PC = (hi << 8) | lo;
    return 0;
}

// Ветвления — общий шаблон
static uint8_t branchIf(bool cond, uint16_t& PC, uint16_t addrRel) {
    if (!cond) return 0;
    uint8_t extra = 1;
    uint16_t newPC = PC + addrRel;
    if ((newPC & 0xFF00) != (PC & 0xFF00)) extra = 2;
    PC = newPC;
    return extra;
}

uint8_t CPU::BCC() { return branchIf(!getFlag(C), PC, addrRel_); }
uint8_t CPU::BCS() { return branchIf( getFlag(C), PC, addrRel_); }
uint8_t CPU::BEQ() { return branchIf( getFlag(Z), PC, addrRel_); }
uint8_t CPU::BNE() { return branchIf(!getFlag(Z), PC, addrRel_); }
uint8_t CPU::BMI() { return branchIf( getFlag(N), PC, addrRel_); }
uint8_t CPU::BPL() { return branchIf(!getFlag(N), PC, addrRel_); }
uint8_t CPU::BVC() { return branchIf(!getFlag(V), PC, addrRel_); }
uint8_t CPU::BVS() { return branchIf( getFlag(V), PC, addrRel_); }

uint8_t CPU::CLC() { setFlag(C,false); return 0; }
uint8_t CPU::CLD() { setFlag(D,false); return 0; }
uint8_t CPU::CLI() { setFlag(I,false); return 0; }
uint8_t CPU::CLV() { setFlag(V,false); return 0; }
uint8_t CPU::SEC() { setFlag(C,true);  return 0; }
uint8_t CPU::SED() { setFlag(D,true);  return 0; }
uint8_t CPU::SEI() { setFlag(I,true);  return 0; }

uint8_t CPU::NOP() { return 1; }
uint8_t CPU::XXX() { return 0; }

// ─── Таблица опкодов (256 записей) ──────────────────────────────────────────

void CPU::buildLookupTable() {
    // Инициализируем все нелегальными
    for (auto& e : lookup_)
        e = {"???", &CPU::XXX, &CPU::IMP, 2};

    // clang-format off
    lookup_[0x00] = {"BRK", &CPU::BRK, &CPU::IMP, 7};
    lookup_[0x01] = {"ORA", &CPU::ORA, &CPU::IZX, 6};
    lookup_[0x05] = {"ORA", &CPU::ORA, &CPU::ZP0, 3};
    lookup_[0x06] = {"ASL", &CPU::ASL, &CPU::ZP0, 5};
    lookup_[0x08] = {"PHP", &CPU::PHP, &CPU::IMP, 3};
    lookup_[0x09] = {"ORA", &CPU::ORA, &CPU::IMM, 2};
    lookup_[0x0A] = {"ASL", &CPU::ASL, &CPU::ACC, 2};
    lookup_[0x0D] = {"ORA", &CPU::ORA, &CPU::ABS, 4};
    lookup_[0x0E] = {"ASL", &CPU::ASL, &CPU::ABS, 6};

    lookup_[0x10] = {"BPL", &CPU::BPL, &CPU::REL, 2};
    lookup_[0x11] = {"ORA", &CPU::ORA, &CPU::IZY, 5};
    lookup_[0x15] = {"ORA", &CPU::ORA, &CPU::ZPX, 4};
    lookup_[0x16] = {"ASL", &CPU::ASL, &CPU::ZPX, 6};
    lookup_[0x18] = {"CLC", &CPU::CLC, &CPU::IMP, 2};
    lookup_[0x19] = {"ORA", &CPU::ORA, &CPU::ABY, 4};
    lookup_[0x1D] = {"ORA", &CPU::ORA, &CPU::ABX, 4};
    lookup_[0x1E] = {"ASL", &CPU::ASL, &CPU::ABX, 7};

    lookup_[0x20] = {"JSR", &CPU::JSR, &CPU::ABS, 6};
    lookup_[0x21] = {"AND", &CPU::AND, &CPU::IZX, 6};
    lookup_[0x24] = {"BIT", &CPU::BIT, &CPU::ZP0, 3};
    lookup_[0x25] = {"AND", &CPU::AND, &CPU::ZP0, 3};
    lookup_[0x26] = {"ROL", &CPU::ROL, &CPU::ZP0, 5};
    lookup_[0x28] = {"PLP", &CPU::PLP, &CPU::IMP, 4};
    lookup_[0x29] = {"AND", &CPU::AND, &CPU::IMM, 2};
    lookup_[0x2A] = {"ROL", &CPU::ROL, &CPU::ACC, 2};
    lookup_[0x2C] = {"BIT", &CPU::BIT, &CPU::ABS, 4};
    lookup_[0x2D] = {"AND", &CPU::AND, &CPU::ABS, 4};
    lookup_[0x2E] = {"ROL", &CPU::ROL, &CPU::ABS, 6};

    lookup_[0x30] = {"BMI", &CPU::BMI, &CPU::REL, 2};
    lookup_[0x31] = {"AND", &CPU::AND, &CPU::IZY, 5};
    lookup_[0x35] = {"AND", &CPU::AND, &CPU::ZPX, 4};
    lookup_[0x36] = {"ROL", &CPU::ROL, &CPU::ZPX, 6};
    lookup_[0x38] = {"SEC", &CPU::SEC, &CPU::IMP, 2};
    lookup_[0x39] = {"AND", &CPU::AND, &CPU::ABY, 4};
    lookup_[0x3D] = {"AND", &CPU::AND, &CPU::ABX, 4};
    lookup_[0x3E] = {"ROL", &CPU::ROL, &CPU::ABX, 7};

    lookup_[0x40] = {"RTI", &CPU::RTI, &CPU::IMP, 6};
    lookup_[0x41] = {"EOR", &CPU::EOR, &CPU::IZX, 6};
    lookup_[0x45] = {"EOR", &CPU::EOR, &CPU::ZP0, 3};
    lookup_[0x46] = {"LSR", &CPU::LSR, &CPU::ZP0, 5};
    lookup_[0x48] = {"PHA", &CPU::PHA, &CPU::IMP, 3};
    lookup_[0x49] = {"EOR", &CPU::EOR, &CPU::IMM, 2};
    lookup_[0x4A] = {"LSR", &CPU::LSR, &CPU::ACC, 2};
    lookup_[0x4C] = {"JMP", &CPU::JMP, &CPU::ABS, 3};
    lookup_[0x4D] = {"EOR", &CPU::EOR, &CPU::ABS, 4};
    lookup_[0x4E] = {"LSR", &CPU::LSR, &CPU::ABS, 6};

    lookup_[0x50] = {"BVC", &CPU::BVC, &CPU::REL, 2};
    lookup_[0x51] = {"EOR", &CPU::EOR, &CPU::IZY, 5};
    lookup_[0x55] = {"EOR", &CPU::EOR, &CPU::ZPX, 4};
    lookup_[0x56] = {"LSR", &CPU::LSR, &CPU::ZPX, 6};
    lookup_[0x58] = {"CLI", &CPU::CLI, &CPU::IMP, 2};
    lookup_[0x59] = {"EOR", &CPU::EOR, &CPU::ABY, 4};
    lookup_[0x5D] = {"EOR", &CPU::EOR, &CPU::ABX, 4};
    lookup_[0x5E] = {"LSR", &CPU::LSR, &CPU::ABX, 7};

    lookup_[0x60] = {"RTS", &CPU::RTS, &CPU::IMP, 6};
    lookup_[0x61] = {"ADC", &CPU::ADC, &CPU::IZX, 6};
    lookup_[0x65] = {"ADC", &CPU::ADC, &CPU::ZP0, 3};
    lookup_[0x66] = {"ROR", &CPU::ROR, &CPU::ZP0, 5};
    lookup_[0x68] = {"PLA", &CPU::PLA, &CPU::IMP, 4};
    lookup_[0x69] = {"ADC", &CPU::ADC, &CPU::IMM, 2};
    lookup_[0x6A] = {"ROR", &CPU::ROR, &CPU::ACC, 2};
    lookup_[0x6C] = {"JMP", &CPU::JMP, &CPU::IND, 5};
    lookup_[0x6D] = {"ADC", &CPU::ADC, &CPU::ABS, 4};
    lookup_[0x6E] = {"ROR", &CPU::ROR, &CPU::ABS, 6};

    lookup_[0x70] = {"BVS", &CPU::BVS, &CPU::REL, 2};
    lookup_[0x71] = {"ADC", &CPU::ADC, &CPU::IZY, 5};
    lookup_[0x75] = {"ADC", &CPU::ADC, &CPU::ZPX, 4};
    lookup_[0x76] = {"ROR", &CPU::ROR, &CPU::ZPX, 6};
    lookup_[0x78] = {"SEI", &CPU::SEI, &CPU::IMP, 2};
    lookup_[0x79] = {"ADC", &CPU::ADC, &CPU::ABY, 4};
    lookup_[0x7D] = {"ADC", &CPU::ADC, &CPU::ABX, 4};
    lookup_[0x7E] = {"ROR", &CPU::ROR, &CPU::ABX, 7};

    lookup_[0x81] = {"STA", &CPU::STA, &CPU::IZX, 6};
    lookup_[0x84] = {"STY", &CPU::STY, &CPU::ZP0, 3};
    lookup_[0x85] = {"STA", &CPU::STA, &CPU::ZP0, 3};
    lookup_[0x86] = {"STX", &CPU::STX, &CPU::ZP0, 3};
    lookup_[0x88] = {"DEY", &CPU::DEY, &CPU::IMP, 2};
    lookup_[0x8A] = {"TXA", &CPU::TXA, &CPU::IMP, 2};
    lookup_[0x8C] = {"STY", &CPU::STY, &CPU::ABS, 4};
    lookup_[0x8D] = {"STA", &CPU::STA, &CPU::ABS, 4};
    lookup_[0x8E] = {"STX", &CPU::STX, &CPU::ABS, 4};

    lookup_[0x90] = {"BCC", &CPU::BCC, &CPU::REL, 2};
    lookup_[0x91] = {"STA", &CPU::STA, &CPU::IZY, 6};
    lookup_[0x94] = {"STY", &CPU::STY, &CPU::ZPX, 4};
    lookup_[0x95] = {"STA", &CPU::STA, &CPU::ZPX, 4};
    lookup_[0x96] = {"STX", &CPU::STX, &CPU::ZPY, 4};
    lookup_[0x98] = {"TYA", &CPU::TYA, &CPU::IMP, 2};
    lookup_[0x99] = {"STA", &CPU::STA, &CPU::ABY, 5};
    lookup_[0x9A] = {"TXS", &CPU::TXS, &CPU::IMP, 2};
    lookup_[0x9D] = {"STA", &CPU::STA, &CPU::ABX, 5};

    lookup_[0xA0] = {"LDY", &CPU::LDY, &CPU::IMM, 2};
    lookup_[0xA1] = {"LDA", &CPU::LDA, &CPU::IZX, 6};
    lookup_[0xA2] = {"LDX", &CPU::LDX, &CPU::IMM, 2};
    lookup_[0xA4] = {"LDY", &CPU::LDY, &CPU::ZP0, 3};
    lookup_[0xA5] = {"LDA", &CPU::LDA, &CPU::ZP0, 3};
    lookup_[0xA6] = {"LDX", &CPU::LDX, &CPU::ZP0, 3};
    lookup_[0xA8] = {"TAY", &CPU::TAY, &CPU::IMP, 2};
    lookup_[0xA9] = {"LDA", &CPU::LDA, &CPU::IMM, 2};
    lookup_[0xAA] = {"TAX", &CPU::TAX, &CPU::IMP, 2};
    lookup_[0xAC] = {"LDY", &CPU::LDY, &CPU::ABS, 4};
    lookup_[0xAD] = {"LDA", &CPU::LDA, &CPU::ABS, 4};
    lookup_[0xAE] = {"LDX", &CPU::LDX, &CPU::ABS, 4};

    lookup_[0xB0] = {"BCS", &CPU::BCS, &CPU::REL, 2};
    lookup_[0xB1] = {"LDA", &CPU::LDA, &CPU::IZY, 5};
    lookup_[0xB4] = {"LDY", &CPU::LDY, &CPU::ZPX, 4};
    lookup_[0xB5] = {"LDA", &CPU::LDA, &CPU::ZPX, 4};
    lookup_[0xB6] = {"LDX", &CPU::LDX, &CPU::ZPY, 4};
    lookup_[0xB8] = {"CLV", &CPU::CLV, &CPU::IMP, 2};
    lookup_[0xB9] = {"LDA", &CPU::LDA, &CPU::ABY, 4};
    lookup_[0xBA] = {"TSX", &CPU::TSX, &CPU::IMP, 2};
    lookup_[0xBC] = {"LDY", &CPU::LDY, &CPU::ABX, 4};
    lookup_[0xBD] = {"LDA", &CPU::LDA, &CPU::ABX, 4};
    lookup_[0xBE] = {"LDX", &CPU::LDX, &CPU::ABY, 4};

    lookup_[0xC0] = {"CPY", &CPU::CPY, &CPU::IMM, 2};
    lookup_[0xC1] = {"CMP", &CPU::CMP, &CPU::IZX, 6};
    lookup_[0xC4] = {"CPY", &CPU::CPY, &CPU::ZP0, 3};
    lookup_[0xC5] = {"CMP", &CPU::CMP, &CPU::ZP0, 3};
    lookup_[0xC6] = {"DEC", &CPU::DEC, &CPU::ZP0, 5};
    lookup_[0xC8] = {"INY", &CPU::INY, &CPU::IMP, 2};
    lookup_[0xC9] = {"CMP", &CPU::CMP, &CPU::IMM, 2};
    lookup_[0xCA] = {"DEX", &CPU::DEX, &CPU::IMP, 2};
    lookup_[0xCC] = {"CPY", &CPU::CPY, &CPU::ABS, 4};
    lookup_[0xCD] = {"CMP", &CPU::CMP, &CPU::ABS, 4};
    lookup_[0xCE] = {"DEC", &CPU::DEC, &CPU::ABS, 6};

    lookup_[0xD0] = {"BNE", &CPU::BNE, &CPU::REL, 2};
    lookup_[0xD1] = {"CMP", &CPU::CMP, &CPU::IZY, 5};
    lookup_[0xD5] = {"CMP", &CPU::CMP, &CPU::ZPX, 4};
    lookup_[0xD6] = {"DEC", &CPU::DEC, &CPU::ZPX, 6};
    lookup_[0xD8] = {"CLD", &CPU::CLD, &CPU::IMP, 2};
    lookup_[0xD9] = {"CMP", &CPU::CMP, &CPU::ABY, 4};
    lookup_[0xDD] = {"CMP", &CPU::CMP, &CPU::ABX, 4};
    lookup_[0xDE] = {"DEC", &CPU::DEC, &CPU::ABX, 7};

    lookup_[0xE0] = {"CPX", &CPU::CPX, &CPU::IMM, 2};
    lookup_[0xE1] = {"SBC", &CPU::SBC, &CPU::IZX, 6};
    lookup_[0xE4] = {"CPX", &CPU::CPX, &CPU::ZP0, 3};
    lookup_[0xE5] = {"SBC", &CPU::SBC, &CPU::ZP0, 3};
    lookup_[0xE6] = {"INC", &CPU::INC, &CPU::ZP0, 5};
    lookup_[0xE8] = {"INX", &CPU::INX, &CPU::IMP, 2};
    lookup_[0xE9] = {"SBC", &CPU::SBC, &CPU::IMM, 2};
    lookup_[0xEA] = {"NOP", &CPU::NOP, &CPU::IMP, 2};
    lookup_[0xEC] = {"CPX", &CPU::CPX, &CPU::ABS, 4};
    lookup_[0xED] = {"SBC", &CPU::SBC, &CPU::ABS, 4};
    lookup_[0xEE] = {"INC", &CPU::INC, &CPU::ABS, 6};

    lookup_[0xF0] = {"BEQ", &CPU::BEQ, &CPU::REL, 2};
    lookup_[0xF1] = {"SBC", &CPU::SBC, &CPU::IZY, 5};
    lookup_[0xF5] = {"SBC", &CPU::SBC, &CPU::ZPX, 4};
    lookup_[0xF6] = {"INC", &CPU::INC, &CPU::ZPX, 6};
    lookup_[0xF8] = {"SED", &CPU::SED, &CPU::IMP, 2};
    lookup_[0xF9] = {"SBC", &CPU::SBC, &CPU::ABY, 4};
    lookup_[0xFD] = {"SBC", &CPU::SBC, &CPU::ABX, 4};
    lookup_[0xFE] = {"INC", &CPU::INC, &CPU::ABX, 7};
    // clang-format on
}
