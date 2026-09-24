#pragma once
#include <array>
#include <cstdint>
#include <vector>

// ─── Звук Game Boy ───────────────────────────────────────────────────────────
// Четыре канала: 1 — меандр со свипом частоты, 2 — меандр, 3 — волна из 32
// четырёхбитных сэмплов, 4 — шум (сдвиговый регистр). Секвенсор кадров 512 Гц
// (его тактирует делитель таймера) считает длительность, огибающую и свип.
// На выходе — моно 44100 Гц, float −1..1: каналы усредняются на интервале
// сэмпла, постоянная составляющая снимается фильтром, как конденсатором на
// выходе настоящего Game Boy.
class GbApu {
public:
    static constexpr int SAMPLE_RATE = 44100;

    void reset();
    void tick(int dots);                 // такты 4.19 МГц (не зависят от двойной скорости)
    void frameSequencerStep();           // 512 Гц, вызывает шина по делителю

    uint8_t read(uint16_t addr) const;   // $FF10-$FF3F
    void    write(uint16_t addr, uint8_t v);
    uint8_t readPcm(int pair) const;     // CGB $FF76/$FF77: текущие выходы каналов

    const std::vector<float>& samples() const { return samples_; }
    void clearSamples() { samples_.clear(); }

    template<class S> void serialize(S& s);

private:
    struct Channel {
        bool     enabled = false;        // канал звучит (биты NR52)
        bool     dacOn = false;
        uint16_t length = 0;             // счётчик длительности
        bool     lengthEnable = false;
        uint16_t freq = 0;               // 11 бит
        int32_t  timer = 0;              // тактов до следующего шага волны
        // Огибающая громкости
        uint8_t  volume = 0, envInit = 0, envPeriod = 0, envTimer = 0;
        bool     envUp = false;
        // Меандр
        uint8_t  duty = 0, dutyPos = 0;
        // Свип (канал 1)
        uint8_t  sweepPeriod = 0, sweepShift = 0, sweepTimer = 0;
        bool     sweepNeg = false, sweepEnabled = false, sweepNegUsed = false;
        uint16_t sweepShadow = 0;
        // Волна (канал 3)
        uint8_t  wavePos = 0, waveVol = 0, waveSample = 0;
        // Шум (канал 4)
        uint16_t lfsr = 0x7FFF;
        uint8_t  noiseShift = 0, noiseDiv = 0;
        bool     noiseWidth7 = false;
    };
    std::array<Channel, 4> ch_{};
    std::array<uint8_t, 0x17> regs_{};   // $FF10-$FF26: записанные значения
    std::array<uint8_t, 16>   wave_{};
    bool     power_ = true;
    uint8_t  fsStep_ = 0;

    // Выход
    double   sampleAcc_ = 0.0;           // тактов в текущем сэмпле
    double   sumOut_ = 0.0;
    float    hpfCap_ = 0.0f;
    std::vector<float> samples_;

    int32_t  period(int n) const;
    uint8_t  digital(int n) const;       // выход канала 0..15
    uint16_t sweepCalc();
    void     trigger(int n);
    void     powerOff();
};
