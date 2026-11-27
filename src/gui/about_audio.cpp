#include "gui/about_audio.h"
#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>

namespace pcbp::gui {

static float midi_hz(int n) { return 440.0f * std::pow(2.0f, (n - 69) / 12.0f); }

void AboutSynth::init(uint32_t seed) {
    rng_ = seed * 2654435761u + 0x9E3779B9u; if (!rng_) rng_ = 1;
    for (int i = 0; i < 8; ++i) rnd();
    pos_ = 0; dpos_ = 0; std::memset(dl_, 0, sizeof dl_);
    arp_ = bass_ = zapO_ = {}; std::memset(pad_, 0, sizeof pad_);
    arpEnv_ = arpLp_ = bassEnv_ = bassLp_ = hatEnv_ = hatHp_ = hatPrev_ = zapEnv_ = 0;
    padLp_[0] = padLp_[1] = 0; fade_ = fadeTgt_ = fadeStep_ = 0;
    // natural-minor progressions (scale degrees), root A..E region
    static const int scale[7] = {0, 2, 3, 5, 7, 8, 10};
    static const int progs[4][4] = {{0, 5, 2, 6}, {0, 3, 5, 4}, {0, 6, 5, 6}, {0, 5, 3, 4}};
    int root = 45 + (int)(rnd() % 7);                // A2..D#3
    const int* pr = progs[rnd() % 4];
    static const int arpPat[4][8] = {{0,1,2,3,2,1,0,1},{0,2,1,3,0,2,1,3},{0,1,2,1,3,1,2,1},{3,2,1,0,1,2,3,2}};
    const int* ap = arpPat[rnd() % 4];
    uint32_t hatMask = (rnd() & 1) ? 0xAAAAu : 0xEEEEu;
    for (int bar = 0; bar < 8; ++bar) {
        int deg = pr[bar / 2 % 4];
        int tone[4];
        for (int k = 0; k < 4; ++k) {                 // 7th chord stacked in thirds
            int d = deg + 2 * k;
            tone[k] = root + 12 * (d / 7) + scale[d % 7];
        }
        for (int k = 0; k < 4; ++k) padHz_[bar][k] = midi_hz(tone[k] + 12);
        for (int s = 0; s < 16; ++s) {
            int i = bar * 16 + s;
            int oct = (s >= 8 && (bar & 1)) ? 36 : 24;
            arpHz_[i] = midi_hz(tone[ap[s & 7]] + oct);
            arpOn_[i] = (bar >= 1 || s >= 0) && ((rnd() % 9) != 0 || s == 0);
            bassOn_[i] = (s % 2 == 0) && ((s % 4 == 0) || (rnd() % 3 != 0));
            bassHz_[i] = midi_hz(tone[(s == 14 && (rnd() & 1)) ? 2 : 0] - 12);
            hat_[i] = (hatMask >> s) & 1 ? ((s % 4 == 2) ? 2 : 1) : 0;
            zap_[i] = 0;
        }
    }
    int nz = 1 + rnd() % 2;
    for (int z = 0; z < nz; ++z) zap_[(rnd() % 8) * 16 + 4 * (1 + rnd() % 3) - 1] = 1;
    for (int k = 0; k < 4; ++k) padHz_cur_[k] = padTgt_[k] = padHz_[0][k];
}

void AboutSynth::set_fade(float target, float seconds) {
    fadeTgt_ = target;
    float n = seconds * kRate; fadeStep_ = n > 1 ? std::fabs(target - fade_) / n : 1.0f;
}

void AboutSynth::trigger(int i) {
    if (arpOn_[i]) { arpEnv_ = 1; arp_.inc = arpHz_[i] / kRate; }
    if (bassOn_[i]) { bassEnv_ = 1; bass_.inc = bassHz_[i] / kRate; }
    if (hat_[i]) hatEnv_ = hat_[i] == 2 ? 0.8f : 0.45f;
    if (zap_[i]) { zapEnv_ = 1; zapO_.inc = 2400.0f / kRate; zapInc_ = 0.99982f; }
    if (i % 16 == 0) for (int k = 0; k < 4; ++k) padTgt_[k] = padHz_[i / 16][k];
}

void AboutSynth::render(int16_t* out, int frames) {
    const float arpDec = std::exp(-1.0f / (0.11f * kRate));
    const float bassDec = std::exp(-1.0f / (0.22f * kRate));
    const float hatDec = std::exp(-1.0f / (0.025f * kRate));
    const float zapDec = std::exp(-1.0f / (0.18f * kRate));
    static const float det[2] = {0.997f, 1.003f};
    for (int f = 0; f < frames; ++f) {
        if (pos_ % kStep == 0) trigger(pos_ / kStep);
        // arp: pulse 25%, lowpass tracking envelope
        arp_.ph += arp_.inc; arp_.ph -= (float)(int)arp_.ph;
        float a = (arp_.ph < 0.25f ? 1.0f : -0.33f) * arpEnv_;
        arpLp_ += (0.08f + 0.25f * arpEnv_) * (a - arpLp_);
        arpEnv_ *= arpDec;
        // bass: saw -> lowpass
        bass_.ph += bass_.inc; bass_.ph -= (float)(int)bass_.ph;
        float b = (2 * bass_.ph - 1) * bassEnv_;
        bassLp_ += (0.03f + 0.06f * bassEnv_) * (b - bassLp_);
        bassEnv_ *= bassDec;
        // pad: detuned saws, gliding chord, stereo split
        float pl = 0, pr = 0;
        for (int k = 0; k < 4; ++k) {
            padHz_cur_[k] += (padTgt_[k] - padHz_cur_[k]) * 0.0004f;
            for (int d = 0; d < 2; ++d) {
                Osc& o = pad_[k][d]; o.ph += padHz_cur_[k] * det[d] / kRate; o.ph -= (float)(int)o.ph;
                float s = 2 * o.ph - 1; (d ? pr : pl) += s;
            }
        }
        padLp_[0] += 0.02f * (pl - padLp_[0]); padLp_[1] += 0.02f * (pr - padLp_[1]);
        // hats: highpassed noise
        noise_ = noise_ * 1664525u + 1013904223u;
        float n = (int32_t)noise_ * (1.0f / 2147483648.0f);
        hatHp_ = 0.6f * (hatHp_ + n - hatPrev_); hatPrev_ = n;
        float h = hatHp_ * hatEnv_; hatEnv_ *= hatDec;
        // zap: downward sine sweep with ring-ish fold
        float z = 0;
        if (zapEnv_ > 1e-4f) {
            zapO_.ph += zapO_.inc; zapO_.ph -= (float)(int)zapO_.ph; zapO_.inc *= zapInc_;
            float s = std::sin(6.2831853f * zapO_.ph);
            z = (s > 0 ? 1 : -1) * std::sqrt(std::fabs(s)) * zapEnv_ * ((n > 0.6f) ? 0.5f : 1.0f);
            zapEnv_ *= zapDec;
        }
        // mix + ping-pong delay on arp
        float dL = dl_[0][dpos_], dR = dl_[1][dpos_];
        dl_[0][dpos_] = arpLp_ * 0.5f + dR * 0.45f;
        dl_[1][dpos_] = dL * 0.45f;
        if (++dpos_ == kDelay) dpos_ = 0;
        float c = 0.30f * bassLp_ + 0.06f * h;
        float L = c + 0.22f * arpLp_ + 0.16f * dL + 0.035f * padLp_[0] + 0.07f * z;
        float R = c + 0.18f * arpLp_ + 0.16f * dR + 0.035f * padLp_[1] - 0.07f * z;
        if (fade_ < fadeTgt_) { fade_ += fadeStep_; if (fade_ > fadeTgt_) fade_ = fadeTgt_; }
        else if (fade_ > fadeTgt_) { fade_ -= fadeStep_; if (fade_ < fadeTgt_) fade_ = fadeTgt_; }
        float g = 0.55f * fade_;
        L = std::tanh(L * g * 1.5f) * 0.66f; R = std::tanh(R * g * 1.5f) * 0.66f;
        out[2 * f] = (int16_t)(L * 32767.0f); out[2 * f + 1] = (int16_t)(R * 32767.0f);
        if (++pos_ == kLoopFrames) pos_ = 0;
    }
}

} // namespace pcbp::gui

#ifndef PCBP_ABOUT_AUDIO_OFFLINE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

namespace pcbp::gui {
namespace {
constexpr int kBufs = 4, kFrames = 1024;
struct Player {
    AboutSynth synth;
    int16_t pcm[kBufs][kFrames * 2];
    WAVEHDR hdr[kBufs];
};
std::mutex g_mx;
std::thread g_thr;
std::atomic<bool> g_quit{false}, g_play{false};
Player* g_p = nullptr;

void run(Player* p, HANDLE ev) {
    HWAVEOUT wo = nullptr;
    WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = AboutSynth::kRate;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = AboutSynth::kRate * 4;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) { g_play = false; return; }
    p->synth.set_fade(1.0f, 0.5f);
    for (int i = 0; i < kBufs; ++i) {
        p->hdr[i] = {}; p->hdr[i].lpData = (LPSTR)p->pcm[i]; p->hdr[i].dwBufferLength = sizeof p->pcm[i];
        waveOutPrepareHeader(wo, &p->hdr[i], sizeof(WAVEHDR));
        p->synth.render(p->pcm[i], kFrames);
        waveOutWrite(wo, &p->hdr[i], sizeof(WAVEHDR));
    }
    bool fading = false; int tail = -1;
    for (;;) {
        WaitForSingleObject(ev, 50);
        if (g_quit && !fading) { p->synth.set_fade(0.0f, 0.5f); fading = true; }
        for (int i = 0; i < kBufs; ++i) {
            if (!(p->hdr[i].dwFlags & WHDR_DONE)) continue;
            if (tail >= 0) { if (--tail < 0) tail = -2; continue; }
            if (tail == -2) continue;
            p->synth.render(p->pcm[i], kFrames);
            p->hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(wo, &p->hdr[i], sizeof(WAVEHDR));
            if (fading && p->synth.fade() <= 0.0f) tail = kBufs;   // drain queued silence
        }
        if (tail == -2) break;
    }
    waveOutReset(wo);
    for (int i = 0; i < kBufs; ++i) waveOutUnprepareHeader(wo, &p->hdr[i], sizeof(WAVEHDR));
    waveOutClose(wo);
    g_play = false;
}

void stop_locked() {
    if (!g_thr.joinable()) return;
    g_quit = true; g_thr.join();
    delete g_p; g_p = nullptr;
}
} // namespace

void about_audio_start(uint32_t seed) {
    std::lock_guard<std::mutex> lk(g_mx);
    stop_locked();
    g_p = new Player();
    g_p->synth.init(seed);
    g_quit = false; g_play = true;
    g_thr = std::thread([p = g_p] {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev) { run(p, ev); CloseHandle(ev); } else g_play = false;
    });
}
void about_audio_stop() { std::lock_guard<std::mutex> lk(g_mx); stop_locked(); }
bool about_audio_playing() { return g_play.load(); }
} // namespace pcbp::gui
#endif
