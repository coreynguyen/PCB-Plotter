// Offline render of the About music synth to a WAV. Build with -DPCBP_ABOUT_AUDIO_OFFLINE.
// usage: about_audio_render out.wav [seconds=20] [seed=1]
#define PCBP_ABOUT_AUDIO_OFFLINE
#include "gui/about_audio.cpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s out.wav [sec] [seed]\n", argv[0]); return 1; }
    double sec = argc > 2 ? std::atof(argv[2]) : 20.0;
    uint32_t seed = argc > 3 ? (uint32_t)std::strtoul(argv[3], nullptr, 0) : 1u;
    static pcbp::gui::AboutSynth s; s.init(seed); s.set_fade(1.0f, 0.5f);
    int n = (int)(sec * 44100); std::vector<int16_t> pcm((size_t)n * 2);
    int fadeAt = n - 44100 / 2;
    s.render(pcm.data(), fadeAt); s.set_fade(0.0f, 0.5f); s.render(pcm.data() + 2 * (size_t)fadeAt, n - fadeAt);
    FILE* f = std::fopen(argv[1], "wb"); if (!f) return 2;
    uint32_t db = n * 4, v; auto w32 = [&](uint32_t x) { std::fwrite(&x, 4, 1, f); }; auto w16 = [&](uint16_t x) { std::fwrite(&x, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + db); std::fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32(44100); w32(44100 * 4); w16(4); w16(16);
    std::fwrite("data", 1, 4, f); w32(db); std::fwrite(pcm.data(), 2, pcm.size(), f); std::fclose(f); (void)v;
    std::printf("loop=%d frames (%.3f s)\n", pcbp::gui::AboutSynth::kLoopFrames, pcbp::gui::AboutSynth::kLoopFrames / 44100.0);
    return 0;
}
