#pragma once
#include <cstdint>

class GbBus;

// ─── Процессор Game Boy: Sharp SM83 (LR35902) ─────────────────────────────────
// 8-битный, родственник Z80: регистры A F B C D E H L, SP, PC; флаги Z N H C
// в старшей тетраде F. 256 основных опкодов + 256 с префиксом CB.
//
// Точность — до машинного цикла (M-цикл = 4 такта): каждое обращение к памяти
// и каждый внутренний такт проходят через шину, которая за это время двигает
// таймер, видео, звук и DMA. Поэтому порядок событий внутри инструкции как у
// железа (важно для таймера и опроса STAT).
class GbCpu {
public:
    // ── Регистры ─────────────────────────────────────────────────────────────
    uint8_t  a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    uint16_t sp = 0xFFFE;
    uint16_t pc = 0x0100;
    bool     ime     = false;   // глобальное разрешение прерываний
    bool     halted  = false;   // HALT: ждём прерывания
    bool     stopped = false;   // STOP: ждём нажатия кнопки
    bool     locked  = false;   // недопустимый опкод: процессор завис, как железо

    enum : uint8_t { FZ = 0x80, FN = 0x40, FH = 0x20, FC = 0x10 };

    void connect(GbBus* bus) { bus_ = bus; }
    // Состояние после встроенного загрузчика (его мы не исполняем).
    void reset(bool cgb);
    // Одна инструкция, обслуживание прерывания или один M-цикл простоя.
    void step();

    uint16_t af() const { return (uint16_t)((a << 8) | f); }
    uint16_t bc() const { return (uint16_t)((b << 8) | c); }
    uint16_t de() const { return (uint16_t)((d << 8) | e); }
    uint16_t hl() const { return (uint16_t)((h << 8) | l); }

    template<class S> void serialize(S& s);

private:
    GbBus*  bus_ = nullptr;
    uint8_t eiDelay_ = 0;      // EI разрешает прерывания только после следующей инструкции
    bool    haltBug_ = false;  // HALT при IME=0 и готовом прерывании: байт читается дважды

    void setBC(uint16_t v) { b = (uint8_t)(v >> 8); c = (uint8_t)v; }
    void setDE(uint16_t v) { d = (uint8_t)(v >> 8); e = (uint8_t)v; }
    void setHL(uint16_t v) { h = (uint8_t)(v >> 8); l = (uint8_t)v; }
    void setAF(uint16_t v) { a = (uint8_t)(v >> 8); f = (uint8_t)(v & 0xF0); }

    uint8_t  read(uint16_t addr);
    void     write(uint16_t addr, uint8_t v);
    void     idle();
    uint8_t  fetch8();
    uint16_t fetch16();
    void     push16(uint16_t v);   // включает внутренний такт
    uint16_t pop16();

    uint8_t  getR(int i);          // B C D E H L (HL) A
    void     setR(int i, uint8_t v);
    uint16_t getRP(int p) const;   // BC DE HL SP
    void     setRP(int p, uint16_t v);
    bool     cond(int cc) const;   // NZ Z NC C

    void     alu(int op, uint8_t v);
    uint8_t  inc8(uint8_t v);
    uint8_t  dec8(uint8_t v);
    void     addHL(uint16_t v);
    uint16_t spPlus(uint8_t e8);
    uint8_t  cbRotate(int op, uint8_t v);
    void     daa();

    void     execute(uint8_t op);
    void     executeCB();
    void     serviceInterrupt();
};
