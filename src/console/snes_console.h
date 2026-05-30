#pragma once
#include "iconsole.h"
#include "snes/cpu65816.h"
#include "snes/snes_bus.h"
#include "snes/snes_ppu.h"
#include "snes/snes_apu.h"
#include <vector>

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

    // Диагностика
    int dbgFrames_ = 0;
};
