#pragma once
#include <cstdint>
#include <array>
#include <vector>

// ─── SNES APU (Sony SPC700 + DSP) ─────────────────────────────────────────────
// SPC700 — собственный 8-битный CPU (похожий на 6502) с 64 KB ОЗУ.
// DSP (сопроцессор) выводит 8-канальный ADPCM звук 32000 Hz стерео 16-бит.
//
// В этой реализации SPC700 исполняет код, а DSP генерирует тишину до Фазы 4+.

class SnesAPU {
public:
    static constexpr int SAMPLE_RATE = 32000;

    void reset();

    // ─── Cycle-accurate синхронизация ─────────────────────────────────────────
    // Модель: каждый PPU-дот добавляем бюджет тактов SPC (addCycles), но реально
    // исполняем SPC (flush) ТОЛЬКО когда CPU обращается к портам $2140-$2143 и в
    // конце кадра. Так SPC всегда «догнан» до точного момента взаимодействия.
    void addCycles(double spcCycles) { owed_ += spcCycles; }
    void flush();                       // исполнить накопленный бюджет
    static constexpr double SPC_PER_DOT = 1024000.0 / 5364480.0; // ≈0.19089

    // Совместимость со старым интерфейсом (1 «такт» = 4 дота)
    void clock() { addCycles(4 * SPC_PER_DOT); }
    void runCatchup(int) { flush(); }   // устар.: просто flush

    // Порты связи с CPU SNES ($2140–$2143)
    void    writePort(uint8_t port, uint8_t data);
    uint8_t readPort (uint8_t port);

    // Аудио-буфер (стерео, int16: L R L R …)
    const std::vector<int16_t>& samples() const { return samples_; }
    void clearSamples() { samples_.clear(); }

    // Диагностика
    uint16_t dbgSpcPC()  const { return spcPC_; }
    uint8_t  dbgPort(uint8_t i) const { return portOut_[i & 3]; }

private:
    // ─── SPC700 состояние ────────────────────────────────────────────────────
    uint16_t spcPC_    = 0xFFC0;
    uint8_t  spcA_     = 0;
    uint8_t  spcX_     = 0;
    uint8_t  spcY_     = 0;
    uint8_t  spcSP_    = 0xFF;
    uint8_t  spcPSW_   = 0;  // N V P B H I Z C

    // 64 KB RAM SPC700 (содержит IPL ROM в $FFC0–$FFFF при старте)
    std::array<uint8_t, 0x10000> ram_{};

    // ─── Минимальный DSP ─────────────────────────────────────────────────────
    // Полный SNES DSP — это 8-канальный BRR ADPCM микшер с эхо.
    // Здесь — упрощённая модель: храним 128 регистров и для каждого голоса
    // генерируем меандр на частоте, пропорциональной PITCH-регистрам.
    // Это не «настоящий» BRR-семпл, но даёт слышимый звуковой отклик.
    std::array<uint8_t, 128> dsp_{};
    uint32_t dspPhase_[8] = {0,0,0,0,0,0,0,0};
    uint8_t  dspKonLatch_ = 0;

    // ─── Cycle-accurate бюджет + аудио-делитель ───────────────────────────────
    double   owed_ = 0.0;       // накопленный бюджет тактов SPC к исполнению
    uint32_t audioAcc_ = 0;     // счётчик тактов до следующего аудио-сэмпла (32 кГц)
    static constexpr uint32_t AUDIO_DIV = 32; // 1.024МГц / 32 = 32 кГц

    // ─── Состояние голосов DSP (BRR ADPCM) ────────────────────────────────────
    struct Voice {
        bool     active = false;
        uint16_t curAddr = 0;     // адрес текущего BRR-блока в RAM SPC
        int      bufPos = 0;      // позиция в декодированном буфере (0..15)
        bool     bufValid = false;
        int16_t  buf[16] = {0};   // 16 декодированных сэмплов текущего блока
        int16_t  prev0 = 0, prev1 = 0; // история для BRR-фильтра
        uint32_t pitchAcc = 0;    // 12-бит дробный аккумулятор шага
        uint16_t loopAddr = 0;
        int      env = 0;         // огибающая 0..2047
        int      envMode = 0;     // 0=release,1=attack,3=sustain
    } voice_[8];

    // Порты связи с SNES CPU (4 байта в каждую сторону)
    uint8_t portIn_[4]{};   // SNES CPU → SPC700 (pишет CPU, читает SPC)
    uint8_t portOut_[4]{};  // SPC700 → SNES CPU (читает CPU)

    // ─── DSP / Audio ─────────────────────────────────────────────────────────
    std::vector<int16_t> samples_;
    uint32_t sampleDiv_ = 0;  // делитель для 32 kHz

    // ─── Таймеры SPC700 ($FA–$FC: период; $FD–$FF: счётчик) ─────────────────
    // Timer 0,1: 8192 Гц (тик каждые ~125 циклов SPC700)
    // Timer 2  : 64000 Гц (тик каждые ~16 циклов SPC700)
    uint8_t  timerPeriod_[3] = {0, 0, 0}; // $FA-$FC: целевой период (0 = 256 тиков)
    uint8_t  timerIntern_[3] = {0, 0, 0}; // внутренний счётчик 0..period
    uint8_t  timerCounter_[3]= {0, 0, 0}; // $FD-$FF: 4-бит выходной счётчик
    uint32_t timerDiv_[3]    = {0, 0, 0}; // делитель тактовой частоты
    uint8_t  timerEnabled_   = 0;          // биты 0-2 из $F1
    static constexpr uint32_t TIMER_DIV01 = 128; // ~8 kHz при ~1.024 МГц
    static constexpr uint32_t TIMER_DIV2  = 16;  // ~64 kHz

    // ─── SPC700 внутренние методы ─────────────────────────────────────────────
    void     spcReset();
    uint8_t  spcRead (uint16_t addr);
    void     spcWrite(uint16_t addr, uint8_t data);
    int      spcStep ();             // одна инструкция SPC700, возвращает такты
    void     tickTimers();           // тик всех таймеров
    void     genSample();            // один аудио-сэмпл (DSP-микс)

    uint8_t  spcFetch();             // PC++
    void     spcPush (uint8_t v);
    uint8_t  spcPop  ();

    // Флаги PSW
    enum PSW : uint8_t {
        FL_C = 0x01, FL_Z = 0x02, FL_I = 0x04, FL_H = 0x08,
        FL_B = 0x10, FL_P = 0x20, FL_V = 0x40, FL_N = 0x80
    };
    void setNZ(uint8_t v);
};

// ─── IPL ROM (64 байта, неизменный загрузчик SNES) ──────────────────────────
// Вшитый ROM загружает SPC700-программу через порты коммуникации.
// Содержимое — стандартный IPL boot ROM от Sony.
extern const uint8_t kSpcIplRom[64];
