// The reverb: tail length follows Decay, it stays stable at the extremes, Freeze holds, Size glides.
// clang++ -std=c++17 -O2 -Iframework -Iplugins/Sculpt/src plugins/Sculpt/tests/reverb_test.cpp -o /tmp/reverb_test
#include "engine.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <functional>
using namespace sculpt;
static int failures = 0;
#define CHECK(c, ...) do { if (c) { std::printf("ok   "); } else { std::printf("FAIL "); ++failures; } std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

// A short noise burst played once into the reverb; returns the left output.
static std::vector<float> burst(float seconds, float size, float decayMs, bool freeze = false,
                                const std::function<void(TrackParams&, int)>& change = nullptr)
{
    Sample s; s.rate = 44100.0; s.data.assign(8820, 0.0f);
    uint32_t r = 1;
    for (int i = 0; i < 2205; ++i) { r = r * 1664525u + 1013904223u; s.data[(size_t)i] = ((r >> 9) / 4194304.0f - 1.0f) * 0.5f; }
    s.computePeaks();
    Track t; t.prepare(44100.0);
    TrackParams p; p.filterOn = false; p.spaceOn = true; p.delay = 0.0f; p.chorus = 0.0f; p.loop = 0; p.fadeMs = 2.0f;
    p.reverb = 1.0f; p.size = size; p.sDecayMs = decayMs; p.sTone = 0.0f;
    if (change) change(p, 0);
    t.noteOn(60, 1.0f, 1.0f);
    std::vector<float> out;
    float L[256], R[256];
    const int blocks = (int)(seconds * 44100 / 256);
    for (int b = 0; b < blocks; ++b)
    {
        p.freeze = freeze && b * 256 > 44100 / 2;
        if (change) change(p, b);
        for (int i = 0; i < 256; ++i) L[i] = R[i] = 0.0f;
        t.process(p, s, 1.0f, 120.0, true, Transport(), L, R, 256);
        out.insert(out.end(), L, L + 256);
    }
    return out;
}
// the mid range (300 Hz - 1.5 kHz), where Decay is defined; highs fade faster by design
static std::vector<float> mids(const std::vector<float>& x)
{
    std::vector<float> y(x.size());
    const float a = 1.0f - std::exp(-2.0f * 3.14159265f * 1500.0f / 44100.0f), b = 1.0f - std::exp(-2.0f * 3.14159265f * 300.0f / 44100.0f);
    float l1 = 0, l2 = 0, h = 0;
    for (size_t i = 0; i < x.size(); ++i) { l1 += a * (x[i] - l1); l2 += a * (l1 - l2); h += b * (l2 - h); y[i] = l2 - h; }
    return y;
}
static double rms(const std::vector<float>& x, double from, double len)
{
    const size_t a = (size_t)(from * 44100), n = (size_t)(len * 44100);
    double e = 0.0; for (size_t i = a; i < a + n && i < x.size(); ++i) e += x[i] * x[i];
    return std::sqrt(e / n);
}
// time for the tail to fall 30 dB from 0.3 s, doubled (T60 from the first 30 dB of the decay)
static double t60(const std::vector<float>& x)
{
    const double ref = rms(x, 0.3, 0.05);
    for (double t = 0.35; t < x.size() / 44100.0 - 0.05; t += 0.01)
        if (rms(x, t, 0.05) < ref * 0.0316) return 2.0 * (t - 0.3);
    return 1e9;
}

int main()
{
    for (float d : {2.0f, 8.0f})
    {
        const auto x = mids(burst(d * 0.8f + 1.0f, 0.5f, d * 1000.0f));
        const double m = t60(x);
        CHECK(m > d * 0.8 && m < d * 1.2, "Decay %.0f s: the tail fades 60 dB in about %.1f s", d, m);
    }
    {
        const auto raw = burst(30.0f, 1.0f, 60000.0f);
        float peak = 0.0f; for (float v : raw) peak = std::fmax(peak, std::fabs(v));
        const auto x = mids(raw);
        const double late = rms(x, 25.0, 1.0), early = rms(x, 1.0, 1.0);
        CHECK(std::isfinite(peak) && peak < 2.0f && late < early, "Size 100%% + Decay 60 s is stable (peak %.2f) and still fading (%.4f -> %.4f)", peak, early, late);
        CHECK(late > early * 0.04, "and really is long: after 24 s it is only %.0f dB down (about -24 expected)", 20.0 * std::log10(late / early));
    }
    {
        const auto x = burst(12.0f, 0.7f, 1500.0f, true);
        const double a = rms(x, 1.0, 0.5), b = rms(x, 11.0, 0.5);
        CHECK(a > 1e-4 && b > a * 0.5, "Freeze holds the tail (%.4f -> %.4f after 10 s)", a, b);
    }
    {   // echo density: 0.2-1.0 s in, short windows stay close to the average level (a wash, not echoes)
        const auto x = burst(2.0f, 0.8f, 6000.0f);
        double worst = 0.0;
        const double avg = rms(x, 0.4, 0.5);
        for (double t = 0.4; t < 0.9; t += 0.005) worst = std::fmax(worst, rms(x, t, 0.005) / avg);
        CHECK(worst < 2.5, "the tail is a dense wash, not separate echoes (loudest 5 ms is %.1fx the average)", worst);
    }
    {   // sweeping Size while the tail rings: no clicks (compared with the same tail, Size still)
        auto worstStep = [](const std::vector<float>& x) { float w = 0.0f; for (size_t i = 44100 / 2; i + 1 < x.size(); ++i) w = std::fmax(w, std::fabs(x[i + 1] - x[i])); return w; };
        const auto still = burst(4.0f, 0.5f, 8000.0f);
        const auto swept = burst(4.0f, 0.5f, 8000.0f, false, [](TrackParams& p, int b) { if (b > 60) p.size = 0.5f + 0.4f * (float)std::sin(b * 0.03); });
        const float a = worstStep(still), c = worstStep(swept);
        CHECK(c < a * 2.0f, "sweeping Size bends the tail but does not click (biggest step %.4f, Size still %.4f)", c, a);
    }
    {   // Mix: 0 = dry only, 1 = reverb only, 0.5 = both at full level
        auto level = [](float mix, double from, double len) {
            const auto x = burst(1.5f, 0.6f, 4000.0f, false, [mix](TrackParams& p, int) { p.revMix = mix; });
            return rms(x, from, len);
        };
        const double dry0 = level(0.0f, 0.0, 0.04), tail0 = level(0.0f, 0.6, 0.5);
        const double dry1 = level(1.0f, 0.0, 0.01), tail1 = level(1.0f, 0.6, 0.5), tailH = level(0.5f, 0.6, 0.5), dryH = level(0.5f, 0.0, 0.04);
        CHECK(dry0 > 0.05 && tail0 < 1e-6, "Mix 0%%: only the dry sound (dry %.3f, tail %.1e)", dry0, tail0);
        CHECK(dry1 < dry0 * 0.05 && tail1 > 1e-3, "Mix 100%%: only the reverb (first 10 ms %.4f, tail %.4f)", dry1, tail1);
        CHECK(std::fabs(dryH - dry0) < dry0 * 0.05 && std::fabs(tailH - tail1) < tail1 * 0.05,
              "Mix 50%%: dry and reverb both at full level, as before (dry %.3f/%.3f, tail %.4f/%.4f)", dryH, dry0, tailH, tail1);
    }
    {   // the eight types: all stable, Decay still sets the tail, and they really differ
        const char* names[8] = {"Hall", "Plate", "Bloom", "Cloud", "Echoes", "Canyon", "Ripple", "Vast"};
        double attack[8], peaky[8];
        for (int ty = 0; ty < 8; ++ty)
        {
            const auto raw = burst(9.0f, 0.6f, 4000.0f, false, [ty](TrackParams& p, int) { p.revType = ty; p.revMix = 1.0f; });
            float peak = 0.0f; for (float v : raw) peak = std::fmax(peak, std::fabs(v));
            const auto x = mids(raw);
            // T60 from the loudest point, over 250 ms windows (so the gaps between echoes do not count)
            double top = 0.0, at = 0.0;
            for (double t = 0.05; t < 2.0; t += 0.01) { const double r = rms(x, t, 0.25); if (r > top) { top = r; at = t; } }
            double m = 1e9;
            for (double t = at; t < x.size() / 44100.0 - 0.25; t += 0.01) if (rms(x, t, 0.25) < top * 0.0316) { m = 2.0 * (t - at); break; }
            // attack: when the reverb's level first reaches half its loudest (in 10 ms steps)
            double loud = 0.0; for (double t = 0.0; t < 2.0; t += 0.01) loud = std::fmax(loud, rms(raw, t, 0.01));
            attack[ty] = 0.0; for (double t = 0.06; t < 2.0; t += 0.01) if (rms(raw, t, 0.01) > loud * 0.5) { attack[ty] = t; break; }
            // how peaky the early tail is (echoes) against a smooth wash: loudest 5 ms / average, 0.1-0.6 s
            const double avg = rms(raw, 0.1, 0.5); peaky[ty] = 0.0;
            for (double t = 0.1; t < 0.6; t += 0.005) peaky[ty] = std::fmax(peaky[ty], rms(raw, t, 0.005) / avg);
            CHECK(std::isfinite(peak) && peak < 2.0f && m > 2.4 && m < 6.0,
                  "%-7s stable (peak %.2f), Decay 4 s gives %.1f s, attack %.2f s, peakiness %.1f", names[ty], peak, m, attack[ty], peaky[ty]);
        }
        {
            double lo = 1e9, hi = 0.0;
            for (int ty = 0; ty < 8; ++ty)
            {
                const auto r = burst(3.0f, 0.6f, 4000.0f, false, [ty](TrackParams& p, int) { p.revType = ty; p.revMix = 1.0f; });
                double top = 0.0; for (double t = 0.05; t < 2.0; t += 0.01) top = std::fmax(top, rms(r, t, 0.25));
                lo = std::fmin(lo, top); hi = std::fmax(hi, top);
            }
            CHECK(hi / lo < 2.0, "the types are about equally loud (within %.1f dB)", 20.0 * std::log10(hi / lo));
        }
        CHECK(attack[3] > attack[2] && attack[2] > attack[0] && attack[1] <= attack[0] + 0.02,
              "attack: Cloud (%.2f) slower than Bloom (%.2f) slower than Hall (%.2f); Plate is instant (%.2f)", attack[3], attack[2], attack[0], attack[1]);
        CHECK(peaky[4] > peaky[0] * 1.5 && peaky[5] > peaky[0] * 1.5,
              "Echoes (%.1f) and Canyon (%.1f) have clear echoes; Hall is a smooth wash (%.1f)", peaky[4], peaky[5], peaky[0]);
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
