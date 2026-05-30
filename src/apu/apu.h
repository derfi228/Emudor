#pragma once
#include <cstdint>
#include <functional>
#include <vector>

class APU {
public:
    APU();

    void    reset();  // сброс при загрузке новой игры
    void    write(uint16_t addr, uint8_t data);
    uint8_t readStatus();
    void    clock();  // один CPU такт

    void setAudioCallback(std::function<void(float)> cb);

    // Буфер сэмплов для SDL_QueueAudio (заполняется в clock())
    const std::vector<float>& getSamples() const { return samples_; }
    void clearSamples() { samples_.clear(); }

private:
    // Pulse канал
    struct PulseChannel {
        bool    enabled   = false;
        uint8_t dutyCycle = 0;      // 0–3
        uint8_t volume    = 0;
        bool    haltFlag  = false;
        bool    constVol  = false;

        // Envelope
        uint8_t envTimer   = 0;
        uint8_t envPeriod  = 0;
        uint8_t envOutput  = 0;
        bool    envStart   = false;

        // Sweep
        bool    sweepEnabled= false;
        uint8_t sweepPeriod = 0;
        bool    sweepNegate = false;
        uint8_t sweepShift  = 0;
        bool    sweepReload = false;
        uint8_t sweepTimer  = 0;

        // Timer / sequencer
        uint16_t timerPeriod= 0;
        uint16_t timerVal   = 0;
        uint8_t  sequence_  = 0;    // текущая позиция

        // Length counter
        uint8_t lengthVal   = 0;

        bool    isChannel2  = false;  // для sweep negate

        void clockEnvelope();
        void clockSweep();
        void clockLength();
        uint8_t output() const;
    };

    // Triangle канал
    struct TriangleChannel {
        bool    enabled     = false;
        uint8_t linearCounter= 0;
        uint8_t linearPeriod = 0;
        bool    reloadFlag_  = false;
        bool    haltFlag     = false;

        uint16_t timerPeriod= 0;
        uint16_t timerVal   = 0;
        uint8_t  sequence_  = 0;
        uint8_t  lengthVal  = 0;

        void clockLinear();
        void clockLength();
        uint8_t output() const;
    };

    // Noise канал
    struct NoiseChannel {
        bool    enabled  = false;
        bool    mode     = false;   // short mode
        uint8_t volume   = 0;
        bool    haltFlag = false;
        bool    constVol = false;

        uint8_t  envTimer   = 0;
        uint8_t  envPeriod  = 0;
        uint8_t  envOutput  = 0;
        bool     envStart   = false;

        uint16_t shift_      = 1;   // 15-bit LFSR
        uint16_t timerPeriod = 0;
        uint16_t timerVal    = 0;
        uint8_t  lengthVal   = 0;

        void clockEnvelope();
        void clockLength();
        void clockTimer();
        uint8_t output() const;
    };

    PulseChannel    pulse1_, pulse2_;
    TriangleChannel triangle_;
    NoiseChannel    noise_;

    bool    frameCounterMode_ = false;  // false=4-step, true=5-step
    bool    irqInhibit_       = false;
    uint16_t frameCounter_    = 0;
    uint64_t cpuCycles_       = 0;
    double   audioAccum_      = 0.0;

    // Аппаратные аудиофильтры NES (два ФВЧ + ФНЧ)
    // HP1 ≈ 37 Hz  (charge ≈ 0.99474)
    // HP2 ≈ 447 Hz (charge ≈ 0.93835)
    // LP  ≈ 14 kHz (alpha ≈ 0.864)
    float hpPrev1_ = 0.0f;   // предыдущий вход HP1
    float hpOut1_  = 0.0f;   // предыдущий выход HP1
    float hpPrev2_ = 0.0f;   // предыдущий вход HP2
    float hpOut2_  = 0.0f;   // предыдущий выход HP2
    float lpOut_   = 0.0f;   // предыдущий выход LP

    std::function<void(float)> audioCallback_;
    std::vector<float>         samples_;

    void clockFrameCounter();
    void clockHalfFrame();
    void clockQuarterFrame();
    float mixOutput() const;

    static const uint8_t  kLengthTable[32];
    static const uint16_t kNoisePeriods[16];
    static const uint8_t  kDutyTable[4][8];
    static const uint8_t  kTriangleTable[32];
};
