// Moving Start, Length or Rotate while a loop plays must not click. Build:
// clang++ -std=c++17 -O2 -Iframework -Iplugins/Sculpt/src plugins/Sculpt/tests/click_test.cpp -o /tmp/click_test
#include "engine.h"
#include <cstdio>
#include <cmath>
#include <functional>
#include <vector>
using namespace sculpt;
static int failures = 0;
#define CHECK(c, ...) do { if (c) { std::printf("ok   "); } else { std::printf("FAIL "); ++failures; } std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

// Play a 220 Hz tone (with a slowly changing phase, so different places in the sample differ),
// change something every 40 ms, and return the biggest step between two output samples.
static int gPitch = 0;   // Pitch from the start (turning Pitch itself is checked separately)
static float worstStep(bool sync, const std::function<void(TrackParams&, int)>& change)
{
    Sample s; s.rate = 44100.0; s.data.resize(88200);
    for (size_t i = 0; i < s.data.size(); ++i)
        s.data[i] = 0.5f * (float)std::sin(2.0 * 3.14159265358979 * 220.0 * i / 44100.0 + 0.00007 * (double)i * i / 44100.0);
    s.computePeaks();
    Track t; t.prepare(44100.0);
    TrackParams p; p.filterOn = false; p.spaceOn = false; p.loop = 1; p.sync = sync; p.fadeMs = 5.0f; p.pitch = gPitch;
    Transport tr; tr.playing = sync; tr.ppqValid = sync; tr.bpm = 120.0;
    if (!sync) t.noteOn(60, 1.0f, 1.0f);
    float L[64], R[64], last = 0.0f, worst = 0.0f;
    for (int b = 0; b < 44100 / 64; ++b)
    {
        if (b > 100 && b % 28 == 0) change(p, b);
        for (int i = 0; i < 64; ++i) L[i] = R[i] = 0.0f;
        t.process(p, s, 1.0f, 120.0, true, tr, L, R, 64);
        tr.ppq += 64.0 / 44100.0 * 2.0;
        for (int i = 0; i < 64; ++i) { if (b > 100) worst = std::fmax(worst, std::fabs(L[i] - last)); last = L[i]; }
    }
    return worst;
}

// The quietest 3 ms of a steady loop, as a share of its average level: a dip at the join shows up here.
static float joinDip(bool sync, float length, float rotate)
{
    Sample s; s.rate = 44100.0; s.data.resize(88200);
    for (size_t i = 0; i < s.data.size(); ++i)
        s.data[i] = 0.5f * (float)std::sin(2.0 * 3.14159265358979 * 220.0 * i / 44100.0 + 0.00007 * (double)i * i / 44100.0);
    s.computePeaks();
    Track t; t.prepare(44100.0);
    TrackParams p; p.filterOn = false; p.spaceOn = false; p.loop = 1; p.sync = sync; p.fadeMs = 8.0f;
    p.length = length; p.rotate = rotate;
    Transport tr; tr.playing = sync; tr.ppqValid = sync; tr.bpm = 120.0;
    if (!sync) t.noteOn(60, 1.0f, 1.0f);
    std::vector<float> out;
    float L[64], R[64];
    for (int b = 0; b < 44100 * 3 / 64; ++b)
    {
        for (int i = 0; i < 64; ++i) L[i] = R[i] = 0.0f;
        t.process(p, s, 1.0f, 120.0, true, tr, L, R, 64);
        tr.ppq += 64.0 / 44100.0 * 2.0;
        if (b > 300) out.insert(out.end(), L, L + 64);
    }
    double all = 0.0; for (float v : out) all += v * v;
    all = std::sqrt(all / out.size());
    double low = 1e9; const int w = 132;
    for (size_t a = 0; a + w < out.size(); a += 16)
    {
        double e = 0.0; for (int i = 0; i < w; ++i) e += out[a + i] * out[a + i];
        low = std::fmin(low, std::sqrt(e / w));
    }
    return (float)(low / all);
}

int main()
{
    for (int sync = 0; sync < 2; ++sync)
        for (float rot : {0.0f, 0.3f})
        {
            const float d = joinDip(sync, 0.25f, rot);
            CHECK(d > 0.8f, "%s loop, Rotate %.0f%%: no dip where it goes round (quietest 3 ms is %.0f%% of its level)",
                  sync ? "SYNC" : "pad", rot * 100.0f, d * 100.0f);
        }
    const float tone = 0.5f * 0.8f * 0.8f * 2.0f * 3.14159265f * 300.0f / 44100.0f;   // the tone's own steepest step, roughly
    const float limit = tone * 2.5f;
    auto stepStart = [](TrackParams& p, int b) { p.start = (float)((b / 28) % 7) * 0.11f; };
    auto stepLen = [](TrackParams& p, int b) { p.length = 0.2f + (float)((b / 28) % 5) * 0.17f; };
    auto stepRot = [](TrackParams& p, int b) { p.rotate = (float)((b / 28) % 9) * 0.1f; };
    auto none = [](TrackParams&, int) {};
    const float base = worstStep(false, none);
    std::printf("     steady tone: biggest step %.4f (limit %.4f)\n", base, limit);
    for (int sync = 0; sync < 2; ++sync)
    {
        const char* m = sync ? "SYNC" : "pad";
        const float a = worstStep(sync, stepStart), b = worstStep(sync, stepLen), c = worstStep(sync, stepRot);
        CHECK(a < limit, "%s: jumping Start does not click (biggest step %.4f)", m, a);
        CHECK(b < limit, "%s: jumping Length does not click (biggest step %.4f)", m, b);
        CHECK(c < limit, "%s: jumping Rotate does not click (biggest step %.4f)", m, c);
    }
    gPitch = 7;
    const float pp = worstStep(false, none);
    CHECK(pp < limit, "Pitch +7 on its own plays smoothly (biggest step %.4f)", pp);
    const float d = worstStep(false, stepStart);
    CHECK(d < limit, "with Pitch +7, jumping Start does not click (biggest step %.4f)", d);
    {   // the chorus: wide when Width is up, never clicks when its amount or depth jump
        Sample sm; sm.rate = 44100.0; sm.data.resize(88200);
        for (size_t i = 0; i < sm.data.size(); ++i) sm.data[i] = 0.5f * (float)std::sin(2.0 * 3.14159265358979 * 220.0 * i / 44100.0);
        sm.computePeaks();
        auto run = [&](float width, bool jumpy, float& worst) {
            Track t; t.prepare(44100.0);
            TrackParams p; p.filterOn = false; p.spaceOn = true; p.delay = 0.0f; p.reverb = 0.0f; p.loop = 1;
            p.chorus = 1.0f; p.chDepth = 1.0f; p.chWidth = width; p.chRate = 2.0f;
            t.noteOn(60, 1.0f, 1.0f);
            float L[64], R[64], last = 0.0f; double diff = 0.0, all = 0.0; worst = 0.0f;
            for (int b = 0; b < 44100 / 64; ++b)
            {
                if (jumpy && b > 100 && b % 28 == 0) { p.chorus = (b / 28) % 2 ? 1.0f : 0.0f; p.chDepth = (b / 28) % 3 * 0.5f; }
                for (int i = 0; i < 64; ++i) L[i] = R[i] = 0.0f;
                t.process(p, sm, 1.0f, 120.0, true, Transport(), L, R, 64);
                for (int i = 0; i < 64; ++i)
                {
                    if (b > 100) { worst = std::fmax(worst, std::fabs(L[i] - last)); diff += (L[i] - R[i]) * (L[i] - R[i]); all += L[i] * L[i]; }
                    last = L[i];
                }
            }
            return (float)std::sqrt(diff / std::fmax(1e-9, all));
        };
        float w0, w1, wj;
        const float mono = run(0.0f, false, w0), wide = run(1.0f, false, w1);
        run(1.0f, true, wj);
        CHECK(mono < 0.01f && wide > 0.2f, "chorus: Width 0 is centred (L-R %.3f), Width 100%% is wide (L-R %.3f)", mono, wide);
        CHECK(wj < limit, "chorus: jumping its amount and depth does not click (biggest step %.4f)", wj);
    }
    gPitch = 0;
    const float e = worstStep(false, [](TrackParams& p, int b) { p.pitch = ((b / 28) % 3) * 5 - 5; });
    CHECK(e < limit, "turning Pitch itself does not click (biggest step %.4f)", e);
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
