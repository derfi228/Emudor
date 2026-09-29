#pragma once
#include <cstdint>

class N64System;

// ─── RSP: сигнальный процессор внутри RCP ────────────────────────────────────
// Скалярное ядро — урезанный 32-битный MIPS (без умножения/деления, без
// исключений, BREAK останавливает процессор), 4 КБ памяти команд (IMEM) и
// 4 КБ памяти данных (DMEM), адреса заворачиваются внутри 4 КБ.
//
// Векторный блок (COP2): 32 регистра по 8 полос × 16 бит, 48-битный
// аккумулятор на полосу, флаги VCO/VCC/VCE и блок обратных величин
// (VRCP/VRSQ). На нём работают микрокоды графики и звука.
//
// Регистры COP0 — это регистры SP (0-7) и DP (8-15), их обслуживает шина.
class Rsp {
public:
    void connect(N64System* sys, uint8_t* spMem) { sys_ = sys; dmem_ = spMem; imem_ = spMem + 0x1000; }
    void reset();

    // Исполнить cycles тактов (1 команда = 1 такт). Возвращает раньше, если
    // RSP остановился (BREAK или запись halt).
    void run(uint32_t cycles);

    uint32_t pc = 0;                  // 12 бит, адрес в IMEM
    void     setPc(uint32_t address); // запись SP_PC

    template<class S> void serialize(S& s);

    // ── Для тестов ───────────────────────────────────────────────────────────
    uint32_t gpr(int n) const { return r_[n & 31]; }
    uint16_t velement(int v, int e) const { return (uint16_t)vr_[v & 31][e & 7]; }
    void     setVelement(int v, int e, uint16_t x) { vr_[v & 31][e & 7] = (int16_t)x; }

private:
    N64System* sys_ = nullptr;
    uint8_t* dmem_ = nullptr;
    uint8_t* imem_ = nullptr;

    uint32_t r_[32]{};
    uint32_t nextPc_ = 4;             // после слота задержки — цель перехода
    uint32_t curPc_ = 0;              // адрес исполняемой команды

    // ── Векторный блок ───────────────────────────────────────────────────────
    int16_t  vr_[32][8]{};            // полоса 0 — старшие байты 128-битного регистра
    int64_t  acc_[8]{};               // 48 бит со знаком
    uint8_t  vcoLo_ = 0, vcoHi_ = 0;  // перенос / «не равно»   (бит n — полоса n)
    uint8_t  vccLo_ = 0, vccHi_ = 0;  // результаты сравнений
    uint8_t  vce_ = 0;                // расширение для VCL
    int16_t  divIn_ = 0, divOut_ = 0;
    bool     divDp_ = false;          // VRCPH/VRSQH подготовили двойную точность

    void     step();
    void     execute(uint32_t instr);
    void     branch(bool take, uint32_t target);

    // Память данных (big-endian, адрес заворачивается)
    uint8_t  rd8(uint32_t a) const { return dmem_[a & 0xFFF]; }
    void     wr8(uint32_t a, uint8_t v) { dmem_[a & 0xFFF] = v; }
    uint32_t rd32(uint32_t a) const;
    void     wr32(uint32_t a, uint32_t v);

    // Байты векторного регистра в порядке big-endian
    uint8_t  vbyte(int v, int k) const;
    void     setVbyte(int v, int k, uint8_t x);

    // Аккумулятор: срезы 47-32, 31-16, 15-0
    int16_t  acch(int n) const { return (int16_t)(acc_[n] >> 32); }
    int16_t  accm(int n) const { return (int16_t)(acc_[n] >> 16); }
    int16_t  accl(int n) const { return (int16_t)acc_[n]; }
    void     accSet(int n, int64_t v) { acc_[n] = (int64_t)((uint64_t)v << 16) >> 16; }
    void     accAdd(int n, int64_t v) { accSet(n, acc_[n] + v); }
    void     setAccl(int n, int16_t v) { acc_[n] = (acc_[n] & ~0xFFFFll) | (uint16_t)v; }
    uint16_t saturate(int n, bool mid, uint16_t neg, uint16_t pos) const;

    void     cop2(uint32_t instr);
    void     vectorOp(uint32_t instr);
    void     vectorLoad(uint32_t instr);
    void     vectorStore(uint32_t instr);
    void     divide(uint32_t funct, int vd, int de, int vt, int e);
};
