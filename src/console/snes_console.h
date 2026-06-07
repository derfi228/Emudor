#pragma once
#include "iconsole.h"
#include "snes/cpu65816.h"
#include "snes/snes_bus.h"
#include "snes/snes_ppu.h"
#include "snes/snes_apu.h"
#include <vector>
#include <cstdint>

// ─── Тайминг SNES NTSC (единая шкала мастер-тактов) ───────────────────────────
// 1 PPU-дот  = 4 мастер-такта; 1 такт SPC700 (~1.024 МГц) = 24 мастер-такта;
// 1 такт CPU 65816 (~3.58 МГц) = 6 мастер-тактов. Кадр = 341×262 = 89342 дота.
namespace snes_timing {
    constexpr int      MASTER_PER_PPU_DOT  = 4;
    // SPC700 (1.024 МГц) относительно ГЛАВНОГО кристалла 21.477 МГц:
    // 21477272 / 1024000 ≈ 20.97 → 21 мастер-такт на такт SPC. (Не 24 — 24 это
    // домен собственного кристалла SPC 24.576 МГц, а наш masterClock_ — главный.)
    constexpr int      MASTER_PER_SPC_CYCLE = 21;
    constexpr int      MASTER_PER_CPU_CYCLE = 6;
    constexpr int      DOTS_PER_FRAME       = 341 * 262;          // 89342
    constexpr uint64_t MASTER_PER_FRAME     = (uint64_t)DOTS_PER_FRAME * MASTER_PER_PPU_DOT;
}

// ─── SnesConsole ─────────────────────────────────────────────────────────────
// Реализует IConsole для SNES: CPU 65C816 + SnesBus + SnesPPU + SnesAPU.
// Размер кадра: 256 × 239.
class SnesConsole : public IConsole {
public:
    SnesConsole();
    ~SnesConsole() override = default;

    // ── IConsole ──────────────────────────────────────────────────────────────
    bool loadROM(const std::string& path) override;
    void reset()    override;
    void runFrame() override;

    uint32_t*       getFramebuffer()       override;
    const uint32_t* getFramebuffer() const override;
    int getFrameWidth()  const override { return SnesPPU::WIDTH;  }
    int getFrameHeight() const override { return SnesPPU::HEIGHT; }

    void setInput(int player, uint16_t buttons) override;

    const std::vector<float>& getAudioSamples() const override { return audioF_; }
    void clearAudioSamples() override { audioF_.clear(); apu_.clearSamples(); }

    std::string getConsoleName() const override { return "SNES"; }

    bool hasBattery()                         const override { return bus_.hasBattery(); }
    bool isSramDirty()                        const override { return bus_.isSramDirty(); }
    void clearSramDirty()                           override { bus_.clearSramDirty(); }
    bool saveSram(const std::string& path)    const override { return bus_.saveSram(path); }
    bool loadSram(const std::string& path)          override { return bus_.loadSram(path); }

    bool saveState(std::ostream& os) const override;
    bool loadState(std::istream& is)       override;

private:
    SnesBus    bus_;
    CPU65816   cpu_;
    SnesPPU    ppu_;
    SnesAPU    apu_;

    // Аудио-буфер в формате float (для App)
    std::vector<float> audioF_;

    // Конвертация int16 → float (−1.0 … +1.0)
    void flushAudio();

    // ─── Co-scheduler: общая шкала мастер-тактов (свободно бегущая) ────────────
    uint64_t masterClock_ = 0;  // текущий мастер-такт (не сбрасывается между кадрами)
    uint64_t spcNextTick_ = 0;  // мастер-такт следующей инструкции SPC700

    // Фаза ресэмплера DSP 32 кГц → 44.1 кГц (сохраняется между кадрами)
    double   resamplePos_ = 0.0;
    float    lastMono_    = 0.0f;  // последний сэмпл прошлого кадра (для интерполяции через стык)

    // Диагностика
    int dbgFrames_ = 0;
};
