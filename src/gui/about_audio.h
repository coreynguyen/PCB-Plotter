#pragma once
#include <cstdint>

namespace pcbp::gui {
// Procedural looping synthwave for Help>About. start() restarts if already playing;
// stop() fades out (~0.5 s), joins the audio thread; safe without start.
void about_audio_start(uint32_t seed);
void about_audio_stop();
bool about_audio_playing();

// Offline synth core (no waveOut). Fixed-size state, no allocations in render().
class AboutSynth {
public:
    static constexpr int kRate = 44100;
    static constexpr int kStep = 5900;              // samples per 16th (~112 BPM)
    static constexpr int kSteps = 16 * 8;           // 8 bars of 16ths
    static constexpr int kLoopFrames = kStep * kSteps;
    void init(uint32_t seed);
    void render(int16_t* out, int frames);          // interleaved stereo
    void set_fade(float target, float seconds);     // linear ramp of master gain
    float fade() const { return fade_; }
private:
    struct Osc { float ph, inc; };
    uint32_t rng_;
    int pos_ = 0;                                   // sample in loop
    // pattern
    float arpHz_[kSteps]; uint8_t arpOn_[kSteps];
    float bassHz_[kSteps]; uint8_t bassOn_[kSteps];
    uint8_t hat_[kSteps]; uint8_t zap_[kSteps];
    float padHz_[8][4];
    // voices
    Osc arp_{}, bass_{}, pad_[4][2]{}, zapO_{};
    float arpEnv_ = 0, arpLp_ = 0, bassEnv_ = 0, bassLp_ = 0, hatEnv_ = 0, hatHp_ = 0, hatPrev_ = 0;
    float zapEnv_ = 0, zapInc_ = 0, padLp_[2]{}, padHz_cur_[4]{}, padTgt_[4]{};
    uint32_t noise_ = 1;
    static constexpr int kDelay = 16538;            // ~3/16 note ping-pong
    float dl_[2][kDelay]{}; int dpos_ = 0;
    float fade_ = 0, fadeTgt_ = 0, fadeStep_ = 0;
    uint32_t rnd() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }
    void trigger(int step);
};
}
