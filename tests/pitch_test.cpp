// PITCH changes the note, not the timing. Build:
// clang++ -std=c++17 -O1 -Iframework -Iplugins/Sculpt/src plugins/Sculpt/tests/pitch_test.cpp -o /tmp/pitch_test
#include "engine.h"
#include <cstdio>
#include <cmath>
using namespace sculpt;
static int failures = 0;
#define CHECK(c, ...) do { if (c) { std::printf("ok   "); } else { std::printf("FAIL "); ++failures; } std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

#include <vector>
#include <algorithm>
// the pitch you hear: the period inside short frames (autocorrelation), median over the run
static double peakHz(const std::vector<float>& x)
{
    std::vector<double> hz;
    const int frame = 1024;
    for (size_t at = 0; at + frame + 1000 < x.size(); at += 512)
    {
        std::vector<double> c(900, 0.0); double top = 0.0;
        for (int lag = 45; lag < 900; ++lag)
        {
            double xy = 0.0, xx = 1e-9, yy = 1e-9;
            for (int i = 0; i < frame; ++i) { const double u = x[at + i], v = x[at + i + lag]; xy += u * v; xx += u * u; yy += v * v; }
            c[lag] = xy / std::sqrt(xx * yy); top = std::max(top, c[lag]);
        }
        int bestLag = 45;   // the shortest period that is nearly as strong as the best one
        for (int lag = 46; lag < 899; ++lag)
            if (c[lag] >= 0.9 * top && c[lag] >= c[lag - 1] && c[lag] >= c[lag + 1]) { bestLag = lag; break; }
        const double a = c[bestLag - 1], b = c[bestLag], d = c[bestLag + 1];   // refine between samples
        const double den = a - 2.0 * b + d;
        hz.push_back(44100.0 / (bestLag + (std::fabs(den) > 1e-12 ? 0.5 * (a - d) / den : 0.0)));
    }
    std::sort(hz.begin(), hz.end());
    return hz.empty() ? 0.0 : hz[hz.size() / 2];
}
static double run(int pitch, bool sync, double& posAfter, float speed = 1.0f)
{
    Sample s; s.rate = 44100.0;
    s.data.resize(88200);
    for (size_t i = 0; i < s.data.size(); ++i) s.data[i] = 0.5f * (float)std::sin(2.0 * 3.14159265358979 * 220.0 * i / 44100.0);
    s.computePeaks();
    Track t; t.prepare(44100.0);
    TrackParams p; p.filterOn = false; p.spaceOn = false; p.pitch = pitch; p.sync = sync; p.loop = 1; p.speed = speed;
    Transport tr; tr.playing = sync; tr.ppqValid = sync; tr.bpm = 120.0;
    if (!sync) t.noteOn(60, 1.0f, 1.0f);
    float L[256], R[256];
    std::vector<float> out;
    for (int b = 0; b < 44100 / 256; ++b)
    {
        for (int i = 0; i < 256; ++i) L[i] = R[i] = 0.0f;
        t.process(p, s, 1.0f, 120.0, true, tr, L, R, 256);
        tr.ppq += 256.0 / 44100.0 * 2.0;
        if (b < 40) continue;                                // skip the fade-in
        out.insert(out.end(), L, L + 256);
        if (b == 44100 / 256 / 2) posAfter = t.playPosition();
    }
    return peakHz(out);
}

int main()
{
    double p0, p12, pm12, ps;
    const double f0 = run(0, false, p0), f12 = run(12, false, p12), fm12 = run(-12, false, pm12), f7 = run(7, false, ps);
    CHECK(std::fabs(f0 - 220.0) < 5.0, "Pitch 0 plays the sample as is (%.1f Hz)", f0);
    CHECK(std::fabs(f12 - 440.0) < 5.0, "Pitch +12 is an octave up (%.1f Hz)", f12);
    CHECK(std::fabs(fm12 - 110.0) < 3.0, "Pitch -12 is an octave down (%.1f Hz)", fm12);
    CHECK(std::fabs(f7 - 220.0 * std::pow(2.0, 7.0 / 12.0)) < 5.0, "Pitch +7 is a fifth up (%.1f Hz)", f7);
    CHECK(std::fabs(p12 - p0) < 0.01 && std::fabs(pm12 - p0) < 0.01, "and the loop moves at the same speed (%.3f / %.3f / %.3f)", p0, p12, pm12);
    double q0, q12;
    const double s0 = run(0, true, q0), s12 = run(12, true, q12);
    CHECK(std::fabs(s0 - 220.0) < 5.0 && std::fabs(s12 - 440.0) < 5.0, "with SYNC on, Pitch +12 is an octave up too (%.1f -> %.1f Hz)", s0, s12);
    CHECK(std::fabs(q12 - q0) < 0.01, "and stays in time (%.3f / %.3f)", q0, q12);
    double r0;
    const double sp2 = run(0, false, r0, 2.0f), sp2p = run(-12, false, r0, 2.0f);
    CHECK(std::fabs(sp2 - 440.0) < 5.0 && std::fabs(sp2p - 220.0) < 3.0, "Speed 2x still raises it like tape; Pitch -12 brings it back (%.1f / %.1f Hz)", sp2, sp2p);
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
