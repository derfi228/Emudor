// gb_cpu.cpp — Sharp SM83: все 512 опкодов, прерывания, HALT/STOP.
#include "gb_cpu.h"
#include "gb_bus.h"
#include "console/state_io.h"

// ─── Сброс: регистры после загрузчика ────────────────────────────────────────
// Игры для цветного Game Boy узнают железо по A = $11 после старта.
void GbCpu::reset(bool cgb)
{
    if (cgb) { a = 0x11; f = 0x80; b = 0x00; c = 0x00; d = 0xFF; e = 0x56; h = 0x00; l = 0x0D; }
    else     { a = 0x01; f = 0xB0; b = 0x00; c = 0x13; d = 0x00; e = 0xD8; h = 0x01; l = 0x4D; }
    sp = 0xFFFE;
    pc = 0x0100;
    ime = false;
    halted = stopped = locked = false;
    eiDelay_ = 0;
    haltBug_ = false;
}

// ─── Доступ к шине: каждое обращение — один M-цикл ───────────────────────────
uint8_t GbCpu::read(uint16_t addr)            { return bus_->cpuRead(addr); }
void    GbCpu::write(uint16_t addr, uint8_t v) { bus_->cpuWrite(addr, v); }
void    GbCpu::idle()                          { bus_->cpuIdle(); }

uint8_t GbCpu::fetch8()
{
    uint8_t v = read(pc);
    if (haltBug_) haltBug_ = false;   // баг HALT: PC не сдвинулся, байт прочтётся ещё раз
    else          ++pc;
    return v;
}

uint16_t GbCpu::fetch16()
{
    uint8_t lo = fetch8();
    uint8_t hi = fetch8();
    return (uint16_t)(lo | (hi << 8));
}

void GbCpu::push16(uint16_t v)
{
    idle();
    write(--sp, (uint8_t)(v >> 8));
    write(--sp, (uint8_t)v);
}

uint16_t GbCpu::pop16()
{
    uint8_t lo = read(sp++);
    uint8_t hi = read(sp++);
    return (uint16_t)(lo | (hi << 8));
}

// ─── Регистры по номеру из опкода ────────────────────────────────────────────
uint8_t GbCpu::getR(int i)
{
    switch (i) {
    case 0: return b;  case 1: return c;  case 2: return d;  case 3: return e;
    case 4: return h;  case 5: return l;  case 6: return read(hl());
    default: return a;
    }
}

void GbCpu::setR(int i, uint8_t v)
{
    switch (i) {
    case 0: b = v; break;  case 1: c = v; break;  case 2: d = v; break;  case 3: e = v; break;
    case 4: h = v; break;  case 5: l = v; break;  case 6: write(hl(), v); break;
    default: a = v; break;
    }
}

uint16_t GbCpu::getRP(int p) const
{
    switch (p) { case 0: return bc(); case 1: return de(); case 2: return hl(); default: return sp; }
}

void GbCpu::setRP(int p, uint16_t v)
{
    switch (p) { case 0: setBC(v); break; case 1: setDE(v); break; case 2: setHL(v); break; default: sp = v; break; }
}

bool GbCpu::cond(int cc) const
{
    switch (cc & 3) {
    case 0: return !(f & FZ);
    case 1: return (f & FZ) != 0;
    case 2: return !(f & FC);
    default: return (f & FC) != 0;
    }
}

// ─── Арифметика ──────────────────────────────────────────────────────────────
void GbCpu::alu(int op, uint8_t v)
{
    switch (op) {
    case 0: case 1: {                                      // ADD / ADC
        int cin = (op == 1 && (f & FC)) ? 1 : 0;
        int r = a + v + cin;
        f = (uint8_t)(((uint8_t)r ? 0 : FZ) | (((a & 0xF) + (v & 0xF) + cin) > 0xF ? FH : 0)
                      | (r > 0xFF ? FC : 0));
        a = (uint8_t)r;
        break;
    }
    case 2: case 3: case 7: {                              // SUB / SBC / CP
        int cin = (op == 3 && (f & FC)) ? 1 : 0;
        int r = a - v - cin;
        f = (uint8_t)(FN | ((uint8_t)r ? 0 : FZ) | (((a & 0xF) - (v & 0xF) - cin) < 0 ? FH : 0)
                      | (r < 0 ? FC : 0));
        if (op != 7) a = (uint8_t)r;
        break;
    }
    case 4: a &= v; f = (uint8_t)((a ? 0 : FZ) | FH); break;  // AND
    case 5: a ^= v; f = a ? 0 : FZ; break;                     // XOR
    default: a |= v; f = a ? 0 : FZ; break;                    // OR
    }
}

uint8_t GbCpu::inc8(uint8_t v)
{
    uint8_t r = (uint8_t)(v + 1);
    f = (uint8_t)((f & FC) | (r ? 0 : FZ) | ((v & 0xF) == 0xF ? FH : 0));
    return r;
}

uint8_t GbCpu::dec8(uint8_t v)
{
    uint8_t r = (uint8_t)(v - 1);
    f = (uint8_t)((f & FC) | FN | (r ? 0 : FZ) | ((v & 0xF) == 0 ? FH : 0));
    return r;
}

void GbCpu::addHL(uint16_t v)
{
    uint16_t x = hl();
    uint32_t r = (uint32_t)x + v;
    f = (uint8_t)((f & FZ) | (((x & 0xFFF) + (v & 0xFFF)) > 0xFFF ? FH : 0) | (r > 0xFFFF ? FC : 0));
    setHL((uint16_t)r);
    idle();
}

// SP + e8 (ADD SP,e и LD HL,SP+e): флаги по младшему байту, как беззнаковое.
uint16_t GbCpu::spPlus(uint8_t e8)
{
    f = (uint8_t)((((sp & 0xF) + (e8 & 0xF)) > 0xF ? FH : 0) | (((sp & 0xFF) + e8) > 0xFF ? FC : 0));
    return (uint16_t)(sp + (int8_t)e8);
}

uint8_t GbCpu::cbRotate(int op, uint8_t v)
{
    uint8_t r;
    bool carry;
    switch (op) {
    case 0: carry = v & 0x80; r = (uint8_t)((v << 1) | (v >> 7)); break;              // RLC
    case 1: carry = v & 0x01; r = (uint8_t)((v >> 1) | (v << 7)); break;              // RRC
    case 2: carry = v & 0x80; r = (uint8_t)((v << 1) | ((f & FC) ? 1 : 0)); break;    // RL
    case 3: carry = v & 0x01; r = (uint8_t)((v >> 1) | ((f & FC) ? 0x80 : 0)); break; // RR
    case 4: carry = v & 0x80; r = (uint8_t)(v << 1); break;                           // SLA
    case 5: carry = v & 0x01; r = (uint8_t)((v >> 1) | (v & 0x80)); break;           // SRA
    case 6: carry = false;    r = (uint8_t)((v >> 4) | (v << 4)); break;             // SWAP
    default: carry = v & 0x01; r = (uint8_t)(v >> 1); break;                          // SRL
    }
    f = (uint8_t)((r ? 0 : FZ) | (carry ? FC : 0));
    return r;
}

// DAA: поправка A после сложения/вычитания двоично-десятичных чисел.
void GbCpu::daa()
{
    uint8_t adj = 0;
    bool carry = (f & FC) != 0;
    if (!(f & FN)) {
        if (carry || a > 0x99)                 { adj |= 0x60; carry = true; }
        if ((f & FH) || (a & 0x0F) > 0x09)     adj |= 0x06;
        a = (uint8_t)(a + adj);
    } else {
        if (carry)     adj |= 0x60;
        if (f & FH)    adj |= 0x06;
        a = (uint8_t)(a - adj);
    }
    f = (uint8_t)((f & FN) | (a ? 0 : FZ) | (carry ? FC : 0));
}

// ─── Шаг процессора ──────────────────────────────────────────────────────────
void GbCpu::step()
{
    if (locked) { idle(); return; }
    if (bus_->consumeStall()) { idle(); return; }     // идёт HDMA: процессор стоит

    const uint8_t pending = bus_->pendingInterrupts();
    if (stopped) {
        if (!bus_->anyButtonPressed()) { idle(); return; }
        stopped = false;
    }
    if (halted) {
        if (!pending) { idle(); return; }
        halted = false;                     // любое прерывание будит, даже при IME = 0
    }
    if (ime && pending) { serviceInterrupt(); return; }

    execute(fetch8());
    if (eiDelay_ && --eiDelay_ == 0) ime = true;
}

// Вызов обработчика: 5 M-циклов. Вектор выбирается ПОСЛЕ записи старшего
// байта PC: если эта запись попала в IE ($FFFF) и погасила прерывание,
// переход идёт на $0000 (так ведёт себя железо).
void GbCpu::serviceInterrupt()
{
    // EI; HALT при висящем прерывании: баг HALT уже взведён, но прерывание
    // успевает раньше следующей выборки. Железо тогда возвращается на сам
    // HALT, а не читает дважды первый байт обработчика.
    if (haltBug_) { haltBug_ = false; --pc; }
    ime = false;
    idle();
    idle();
    write(--sp, (uint8_t)(pc >> 8));
    const uint8_t pending = bus_->pendingInterrupts();
    write(--sp, (uint8_t)pc);
    if (!pending) {
        pc = 0x0000;
    } else {
        int bit = 0;
        while (!(pending & (1 << bit))) ++bit;
        bus_->ackInterrupt(bit);
        pc = (uint16_t)(0x40 + bit * 8);
    }
    idle();
}

// ─── Основные опкоды ─────────────────────────────────────────────────────────
// Разбор по полям x/y/z (биты 7-6 / 5-3 / 2-0): блоки 0x40-0x7F и 0x80-0xBF
// регулярные, остальное — таблица ниже.
void GbCpu::execute(uint8_t op)
{
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

    if (x == 1) {                                          // LD r,r' и HALT
        if (op == 0x76) {
            if (!ime && bus_->pendingInterrupts()) haltBug_ = true;
            else                                   halted = true;
        } else {
            setR(y, getR(z));
        }
        return;
    }
    if (x == 2) { alu(y, getR(z)); return; }               // ALU A,r

    if (x == 0) {
        switch (z) {
        case 0:
            switch (y) {
            case 0: break;                                             // NOP
            case 1: { uint16_t nn = fetch16();                         // LD (nn),SP
                      write(nn, (uint8_t)sp); write((uint16_t)(nn + 1), (uint8_t)(sp >> 8)); break; }
            case 2: fetch8();                                          // STOP
                    if (!bus_->stopInstruction()) stopped = true;
                    break;
            case 3: { int8_t d8 = (int8_t)fetch8(); pc = (uint16_t)(pc + d8); idle(); break; }  // JR e
            default: { int8_t d8 = (int8_t)fetch8();                   // JR cc,e
                       if (cond(y - 4)) { pc = (uint16_t)(pc + d8); idle(); } break; }
            }
            return;
        case 1:
            if (!q) setRP(p, fetch16());                               // LD rr,nn
            else    addHL(getRP(p));                                   // ADD HL,rr
            return;
        case 2: {
            uint16_t addr;
            switch (p) {
            case 0: addr = bc(); break;
            case 1: addr = de(); break;
            case 2: addr = hl(); setHL((uint16_t)(addr + 1)); break;  // (HL+)
            default: addr = hl(); setHL((uint16_t)(addr - 1)); break; // (HL-)
            }
            if (!q) write(addr, a);
            else    a = read(addr);
            return;
        }
        case 3:                                                         // INC/DEC rr
            setRP(p, (uint16_t)(getRP(p) + (q ? -1 : 1)));
            idle();
            return;
        case 4: setR(y, inc8(getR(y))); return;                        // INC r
        case 5: setR(y, dec8(getR(y))); return;                        // DEC r
        case 6: setR(y, fetch8()); return;                             // LD r,n
        default:
            switch (y) {
            case 0: a = cbRotate(0, a); f &= FC; break;                // RLCA
            case 1: a = cbRotate(1, a); f &= FC; break;                // RRCA
            case 2: a = cbRotate(2, a); f &= FC; break;                // RLA
            case 3: a = cbRotate(3, a); f &= FC; break;                // RRA
            case 4: daa(); break;                                      // DAA
            case 5: a = (uint8_t)~a; f |= FN | FH; break;              // CPL
            case 6: f = (uint8_t)((f & FZ) | FC); break;               // SCF
            default: f = (uint8_t)((f & FZ) | ((f & FC) ^ FC)); break; // CCF
            }
            return;
        }
    }

    // x == 3
    switch (z) {
    case 0:
        switch (y) {
        case 4: write((uint16_t)(0xFF00 | fetch8()), a); break;                 // LDH (n),A
        case 5: { uint8_t e8 = fetch8(); sp = spPlus(e8); idle(); idle(); break; } // ADD SP,e
        case 6: a = read((uint16_t)(0xFF00 | fetch8())); break;                 // LDH A,(n)
        case 7: { uint8_t e8 = fetch8(); setHL(spPlus(e8)); idle(); break; }    // LD HL,SP+e
        default:                                                                 // RET cc
            idle();
            if (cond(y)) { pc = pop16(); idle(); }
            break;
        }
        return;
    case 1:
        if (!q) {                                                               // POP rr
            uint16_t v = pop16();
            if (p == 3) setAF(v); else setRP(p, v);
            return;
        }
        switch (p) {
        case 0: pc = pop16(); idle(); break;                                    // RET
        case 1: pc = pop16(); idle(); ime = true; eiDelay_ = 0; break;          // RETI
        case 2: pc = hl(); break;                                               // JP HL
        default: sp = hl(); idle(); break;                                      // LD SP,HL
        }
        return;
    case 2:
        switch (y) {
        case 4: write((uint16_t)(0xFF00 | c), a); break;                        // LD (C),A
        case 5: write(fetch16(), a); break;                                     // LD (nn),A
        case 6: a = read((uint16_t)(0xFF00 | c)); break;                        // LD A,(C)
        case 7: a = read(fetch16()); break;                                     // LD A,(nn)
        default: { uint16_t nn = fetch16(); if (cond(y)) { pc = nn; idle(); } break; }  // JP cc,nn
        }
        return;
    case 3:
        switch (y) {
        case 0: pc = fetch16(); idle(); break;                                  // JP nn
        case 1: executeCB(); break;                                             // префикс CB
        case 6: ime = false; eiDelay_ = 0; break;                               // DI
        case 7: if (!ime && !eiDelay_) eiDelay_ = 2; break;                     // EI
        default: locked = true; break;                                          // D3 DB E3 EB
        }
        return;
    case 4:
        if (y < 4) {                                                            // CALL cc,nn
            uint16_t nn = fetch16();
            if (cond(y)) { push16(pc); pc = nn; }
        } else {
            locked = true;                                                      // E4 EC F4 FC
        }
        return;
    case 5:
        if (!q) {                                                               // PUSH rr
            push16(p == 3 ? af() : getRP(p));
        } else if (p == 0) {                                                    // CALL nn
            uint16_t nn = fetch16();
            push16(pc);
            pc = nn;
        } else {
            locked = true;                                                      // DD ED FD
        }
        return;
    case 6: alu(y, fetch8()); return;                                           // ALU A,n
    default: push16(pc); pc = (uint16_t)(y * 8); return;                        // RST
    }
}

// ─── Опкоды с префиксом CB: сдвиги, BIT, RES, SET ────────────────────────────
void GbCpu::executeCB()
{
    const uint8_t op = fetch8();
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v = getR(z);
    switch (x) {
    case 0: setR(z, cbRotate(y, v)); break;
    case 1: f = (uint8_t)((f & FC) | FH | ((v & (1 << y)) ? 0 : FZ)); break;    // BIT
    case 2: setR(z, (uint8_t)(v & ~(1 << y))); break;                          // RES
    default: setR(z, (uint8_t)(v | (1 << y))); break;                          // SET
    }
}

// ─── Save state ───────────────────────────────────────────────────────────────
template<class S> void GbCpu::serialize(S& s)
{
    s.io(a); s.io(f); s.io(b); s.io(c); s.io(d); s.io(e); s.io(h); s.io(l);
    s.io(sp); s.io(pc);
    s.io(ime); s.io(halted); s.io(stopped); s.io(locked);
    s.io(eiDelay_); s.io(haltBug_);
}

template void GbCpu::serialize<StateWriter>(StateWriter&);
template void GbCpu::serialize<StateReader>(StateReader&);
