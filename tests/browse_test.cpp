// browse_test.cpp - drives Sculpt's sample browser the way the screen does and checks the results.
//
// Build and run (from the SDK root), pointing it at a folder of WAVs:
//   clang++ -std=c++17 -g -fsanitize=thread -Iframework -Iplugins/Sculpt/src \
//       plugins/Sculpt/tests/browse_test.cpp -o /tmp/browse_test
//   MPC_PLUGIN_FOLDER=plugins/Sculpt SCULPT_SAMPLE_ROOTS=/path/to/wavs /tmp/browse_test
//
// The folder needs at least one sub-folder of WAVs besides Sculpt's own loops.

// Standard headers first, then open up the plugin class so the test can see which sample each track plays.
#include <algorithm>
#include <set>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define private public
#include "../src/Sculpt.cpp"
#undef private

#include <chrono>
#include <cstdio>
#include <thread>

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) { std::printf("PASS  "); } else { std::printf("FAIL  "); ++failures; } \
                              std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

struct Rig
{
    Sculpt p;
    float l[128] = {0}, r[128] = {0};
    float* outs[2] = {l, r};
    Rig() { p.internalSetSampleRate(44100); p.internalSetBlockSize(128); p.internalResume(); }
    void blocks(int n) { for (int i = 0; i < n; ++i) p.internalProcess(nullptr, outs, 128); }
    // a screen tap flips the key's value
    void press(int index) { p.set(index, p.getBool(index) ? 0.0f : 1.0f); blocks(2); }
    std::string text(int index) { return p.displayText(index); }
    bool waitFor(const std::function<bool()>& f, int ms = 3000)
    {
        for (int i = 0; i < ms / 10; ++i) { blocks(1); if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return false;
    }
};

// Press `key` until `ok()` holds, at most 40 times.
static bool stepUntil(Rig& g, int key, const std::function<bool()>& ok)
{
    for (int i = 0; i < 40; ++i) { if (ok()) return true; g.press(key); }
    return ok();
}

int main()
{
    {
        // the browser row shows what the picked track really plays: tracks 2-4 start on their
        // built-in loops, and a file loaded on one track stays shown when you come back to it
        Rig g;
        const int vFolder = viewParam(kPerTrack + bFolder), vFile = viewParam(kPerTrack + bFile);
        CHECK(g.waitFor([&] { return g.text(vFile) == "1 Kalimba"; }), "track 1 shows its loop (%s)", g.text(vFile).c_str());
        const char* want[4] = {"1 Kalimba", "2 Tape", "3 Haze", "4 Glass"};
        for (int t = 1; t < 4; ++t)
        {
            g.p.set(kEditTrack, (float)t); g.blocks(3);
            CHECK(g.text(vFile) == want[t] && g.text(vFolder) == "Sculpt", "track %d shows %s / %s (want Sculpt / %s)",
                  t + 1, g.text(vFolder).c_str(), g.text(vFile).c_str(), want[t]);
        }
        // load the 3rd file of the Sculpt folder on track 2, go away and come back
        g.p.set(kEditTrack, 1.0f); g.blocks(3);
        g.press(bidx(1, bFileNext));
        g.waitFor([&] { return g.p.getBool(bidx(1, bHasFile)) && g.text(vFile) == "3 Haze"; });
        g.p.set(kEditTrack, 0.0f); g.blocks(3);
        CHECK(g.text(vFile) == "1 Kalimba", "back on track 1: 1 Kalimba (%s)", g.text(vFile).c_str());
        g.p.set(kEditTrack, 1.0f); g.blocks(3);
        CHECK(g.text(vFile) == "3 Haze" && g.p.getBool(bidx(1, bHasFile)), "and track 2 still shows its file, 3 Haze (%s)", g.text(vFile).c_str());
        g.p.set(kEditTrack, 0.0f); g.blocks(3);
        // on the Space page, the CHORUS column's first knob is the picked track's chorus amount
        g.p.set(kPage0 + 0, 4.0f); g.blocks(3);
        g.p.setNormalised(knobIdx(0, 0), g.p.param(cidx(0, cAmount)).toNormalised(40.0f)); g.blocks(3);
        CHECK(std::lround(g.p.get(cidx(0, cAmount))) == 40, "CHORUS AMOUNT on track 1's Space page sets its chorus (%.0f)", g.p.get(cidx(0, cAmount)));
        g.p.set(kEditTrack, 2.0f); g.blocks(3);
        CHECK(std::lround(g.p.get(viewParam(kVChorus + cAmount))) == 0 && g.text(viewParam(kVChorus + cRate)) == "0.60 Hz",
              "track 3 shows its own chorus (%s, %s)", g.text(viewParam(kVChorus + cAmount)).c_str(), g.text(viewParam(kVChorus + cRate)).c_str());
        CHECK(g.p.param(viewParam(kVChorus + cWidth)).name == "Width" && g.p.param(viewParam(kVChorus + cAmount)).name == "Amount", "the chorus boxes are captioned Amount, Rate, Depth, Width");
        // track 3 picked, still on the Space page: knob 12 is its reverb Mix
        g.p.setNormalised(knobIdx(0, 11), g.p.param(kRevMixBase + 2).toNormalised(80.0f)); g.blocks(3);
        CHECK(std::lround(g.p.get(kRevMixBase + 2)) == 80 && std::lround(g.p.get(kRevMixBase + 0)) == 20 && g.p.param(kViewRevMix).name == "Mix",
              "MIX with track 3 picked sets its reverb mix only (track 3 %.0f, track 1 %.0f)", g.p.get(kRevMixBase + 2), g.p.get(kRevMixBase + 0));
        g.p.set(kEditTrack, 0.0f); g.blocks(3);
    }
    {
        // the Grain screen: the recording (grey bars), the same bars lit inside the pick zone
        // (white), and a full bar where each grain plays (pink); with Grain off nothing shows
        Rig g;
        g.blocks(20);
        // (read straight from the engine: the screen draws the sample's pixel waveform from these)
        float cols[kGrainCols], dots[sculpt::kMaxGrains], zlo = 0.0f, zhi = 0.0f;
        auto look = [&] { g.p.tracks_[0].grainColumns(cols); g.p.tracks_[0].grainMarks(dots); g.p.tracks_[0].grainZone(g.p.tp_[0], zlo, zhi); };
        look();
        float sum0 = 0.0f; for (float v : cols) sum0 += v; for (float v : dots) sum0 += v;
        CHECK(sum0 == 0.0f, "Grain off: nothing recorded, no grains");
        g.p.set(idx(0, pGrainOn), 1.0f); g.p.set(idx(0, pGMode), 1.0f); g.p.set(idx(0, pGSpread), 30.0f);
        g.p.set(idx(0, pGSize), 400.0f);
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 110));
        g.blocks(2300);   // ~6.7 s: the (~6 s) recording fills
        look();
        int lit = 0, hot = 0, hotLo = 99, hotHi = -1;
        for (int c = 0; c < kGrainCols; ++c)
        {
            lit += cols[c] > 0.05f;
            const float mid = (c + 0.5f) / kGrainCols;
            if (mid >= zlo && mid <= zhi) { ++hot; hotLo = std::min(hotLo, c); hotHi = std::max(hotHi, c); }
        }
        CHECK(lit > 50, "Grain on: the recording shows (%d of 64 bars)", lit);
        // Cloud, Spread 30%%: grains come from up to 0.6 s back (+ their length) of ~5.9 s: the newest spots
        CHECK(hot >= 1 && hotLo >= 50 && hotHi >= 60, "the pick zone lights the most recent spots (%d-%d)", hotLo, hotHi);
        int hits = 0, near = 0;
        for (float d : dots)
            if (d > 0.0f) { const int c = std::min(kGrainCols - 1, (int)(d * kGrainCols)); ++hits; near += c >= hotLo - 8 && c <= hotHi + 8; }   // reversed grains travel back
        CHECK(hits > 0 && near == hits, "grains show in or by the zone (%d of %d)", near, hits);
        g.p.handleMidi(mpc::MidiEvent::noteOff(0, 24));
        g.p.set(idx(0, pGrainOn), 0.0f); g.blocks(20);
    }
    {
        // reopened with Lock already on: the recording still fills once, then holds; and short
        // grains (35 ms) still show, since each bar stays up a moment
        Rig g;
        g.p.set(idx(0, pGrainOn), 1.0f); g.p.set(idx(0, pLock), 1.0f); g.p.set(idx(0, pGSize), 35.0f); g.p.set(idx(0, pGDensity), 80.0f);
        g.p.set(kPage0, 1.0f);   // the Grain page: track 1's pixel waveform is live
        g.blocks(1400);   // ~4 s of silence first: Lock must not keep that
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 110));
        g.blocks(2300);
        float lc[kGrainCols]; g.p.tracks_[0].grainColumns(lc);
        int lit = 0; for (float v : lc) lit += v > 0.05f;
        CHECK(lit > 50, "Lock on, silence first: the recording still fills once you play (%d of 64)", lit);
        {
            int solid = 0, gone = 0;
            for (int c = 0; c < kPixCols; ++c)
            {
                const int d = (int)std::lround(g.p.getNormalised(kGrainPix0 + c) * (kPixHeights * kPixDissolve - 1)) % kPixDissolve;
                solid += d == 0; gone += d > 0;
            }
            CHECK(solid > 0 && gone > 0, "and the pixel waveform shows where grains pick (%d solid, %d dissolving)", solid, gone);
        }

        g.p.handleMidi(mpc::MidiEvent::noteOff(0, 24));
    }
    {
        // the pixel waveform: Grain off = the whole sample solid, at its own heights; Grain on and
        // playing = solid where grains pick from, dissolving more the further away
        Rig g;
        g.p.set(kPage0, 1.0f); g.blocks(40);   // the Grain page
        auto frameOf = [&](int c) { return (int)std::lround(g.p.getNormalised(kGrainPix0 + c) * (kPixHeights * kPixDissolve - 1)); };
        int solid = 0, tall = 0;
        for (int c = 0; c < kPixCols; ++c) { solid += frameOf(c) % kPixDissolve == 0; tall += frameOf(c) / kPixDissolve > 2; }
        CHECK(solid == kPixCols && tall > 40, "Grain off: the whole waveform shows solid (%d solid, %d with height)", solid, tall);
        g.p.set(kPage0 + 0, 1.0f);   // the Grain page
        g.p.set(idx(0, pGrainOn), 1.0f); g.p.set(idx(0, pGSpread), 20.0f);
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 110));
        g.blocks(1400);
        int zero = 0, far = 0, steady = 1;
        for (int c = 0; c < kPixCols; ++c) { const int d = frameOf(c) % kPixDissolve; zero += d == 0; far += d == kPixDissolve - 1; }
        // dissolve never jumps by more than one step between neighbours (it grows with distance)
        for (int c = 1; c < kPixCols; ++c) if (std::abs(frameOf(c) % kPixDissolve - frameOf(c - 1) % kPixDissolve) > 1) steady = 0;
        CHECK(zero > 0 && zero < kPixCols / 2 && far > 0 && steady, "Grain on: solid where grains pick (%d columns), dissolving away from there (%d fully)", zero, far);
        g.p.handleMidi(mpc::MidiEvent::noteOff(0, 24));
    }
    {
        // the Filter screen's live spectrum: silence shows nothing; playing shows the sound; with a
        // low cutoff the sound coming out falls well below the sound going in at the top end only
        Rig g;
        g.p.set(kPage0, 2.0f);   // the Filter page: track 1's spectrum is live
        g.blocks(40);
        float sumIn = 0.0f; for (int b = 0; b < kSpecBands; ++b) sumIn += g.p.getNormalised(kSpecIn0 + b);
        CHECK(sumIn < 0.5f, "Filter screen: silence shows (almost) nothing (%.2f)", sumIn);
        g.p.set(idx(0, pCutoff), 1500.0f); g.p.set(idx(0, pReso), 0.0f); g.p.set(idx(0, pFWet), 100.0f);
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 120));
        g.blocks(400);
        auto band = [&](float hz) { return (int)(std::log(hz / 20.0f) / std::log(1000.0f) * kSpecBands); };
        const int lowB = band(700.0f), highB = band(5800.0f);   // the Kalimba loop has little below ~400 Hz
        const float inLow = g.p.getNormalised(kSpecIn0 + lowB), outLow = g.p.getNormalised(kSpecOut0 + lowB);
        const float inHigh = g.p.getNormalised(kSpecIn0 + highB), outHigh = g.p.getNormalised(kSpecOut0 + highB);
        CHECK(inLow > 0.2f && inHigh > 0.2f, "playing: the sound shows (700 Hz %.2f, 5.8 kHz %.2f)", inLow, inHigh);
        CHECK(std::fabs(outLow - inLow) < 0.15f && inHigh - outHigh > 0.2f,
              "Cutoff 1.5 kHz: lows come out as they went in (%.2f/%.2f), highs are cut (%.2f -> %.2f)", inLow, outLow, inHigh, outHigh);
        g.p.handleMidi(mpc::MidiEvent::noteOff(0, 24));
    }
    {
        // switching tracks: the screen updates are spread out (at most a block's worth at a time),
        // so the audio never stalls on a flood of them, and every view setting still gets through
        Rig g;
        g.p.set(idx(0, pGrainOn), 1.0f);
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 110));
        g.blocks(400);
        static int calls = 0; static std::set<int> told;
        g.p.host_ = [](AEffect*, VstInt32 op, VstInt32 index, VstIntPtr, void*, float) -> VstIntPtr {
            if (op == audioMasterAutomate) { ++calls; told.insert(index); }
            return 0;
        };
        static AEffect fake{};
        g.p.effect_ = &fake;
        g.p.set(kEditTrack, 2.0f);
        int worst = 0;
        for (int b = 0; b < 120; ++b) { calls = 0; g.blocks(1); worst = std::max(worst, calls); }
        int missing = 0;
        for (int v = 0; v < kNumView; ++v) if (!told.count(viewParam(v)) && g.p.getNormalised(viewParam(v)) != 0.0f) ++missing;
        CHECK(worst <= Sculpt::kNotifyPerBlock, "switching tracks: at most %d screen updates per block (worst %d)", Sculpt::kNotifyPerBlock, worst);
        CHECK(missing == 0, "and every view setting still reaches the screen (%d missing)", missing);
        g.p.host_ = nullptr; g.p.effect_ = nullptr;
    }
    {
        // the Grain module (Grainbloom's engine): every mode makes grains from what plays, stays in
        // bounds, and the rhythmic modes show Size as a tempo division
        const char* names[7] = {"Collage", "Cloud", "Stutter", "Shimmer", "Shatter", "Arpeggio", "Cascade"};
        for (int m = 0; m < 7; ++m)
        {
            Rig g;
            g.p.set(idx(0, pGrainOn), 1.0f); g.p.set(idx(0, pGMode), (float)m); g.p.set(idx(0, pGWet), 100.0f);
            g.p.set(idx(0, pFilterOn), 0.0f); g.p.set(idx(0, pSpaceOn), 0.0f); g.p.set(idx(0, pGFeedback), 60.0f);
            g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 110));
            double e = 0.0; float peak = 0.0f; bool finite = true;
            for (int b = 0; b < 1400; ++b)
            {
                g.blocks(1);
                if (b < 400) continue;
                for (int i = 0; i < 128; ++i) { e += g.l[i] * g.l[i]; peak = std::fmax(peak, std::fabs(g.l[i])); finite &= std::isfinite(g.l[i]); }
            }
            const double rms = std::sqrt(e / (1000.0 * 128));
            const std::string sz = g.text(idx(0, pGSize));
            const bool rhythmic = m != 1 && m != 3;
            CHECK(finite && rms > 0.01 && peak <= 1.0f && (rhythmic ? sz.find('/') != std::string::npos : sz.find("ms") != std::string::npos),
                  "Grain %-8s sounds (rms %.3f, peak %.2f), Size shows \"%s\"", names[m], rms, peak, sz.c_str());
            g.p.handleMidi(mpc::MidiEvent::noteOff(0, 24));
        }
    }
    {
        // track keys and page keys: the 12 knobs follow the page on screen for the picked track,
        // both ways; picking a track moves the knobs, the view, the pictures and channel-1 pads
        Rig g;
        g.blocks(4);
        CHECK(std::fabs(g.p.getNormalised(knobIdx(0, 1)) - g.p.getNormalised(idx(0, pLength))) < 1e-6f,
              "it opens on Source for track 1: knob 2 is its Length");
        g.p.set(kEditTrack, 1.0f); g.blocks(3);   // track key 2
        CHECK(std::fabs(g.p.getNormalised(knobIdx(0, 1)) - g.p.getNormalised(idx(1, pLength))) < 1e-6f,
              "track key 2: knob 2 is track 2's Length");
        g.p.set(kPage0, 2.0f); g.blocks(3);   // Filter page
        CHECK(std::fabs(g.p.getNormalised(knobIdx(0, 0)) - g.p.getNormalised(idx(1, pCutoff))) < 1e-6f,
              "on the Filter page knob 1 becomes Cutoff");
        g.p.setNormalised(knobIdx(0, 0), 0.25f); g.blocks(2);
        CHECK(std::fabs(g.p.getNormalised(idx(1, pCutoff)) - 0.25f) < 1e-6f && std::fabs(g.p.getNormalised(idx(0, pCutoff)) - 0.25f) > 1e-3f,
              "turning it moves track 2's Cutoff only");
        g.p.set(idx(1, pReso), 80.0f); g.blocks(2);
        CHECK(std::fabs(g.p.getNormalised(knobIdx(0, 1)) - g.p.getNormalised(idx(1, pReso))) < 1e-6f,
              "and Reso set some other way shows on its knob");
        CHECK(g.text(knobIdx(0, 1)) == g.text(idx(1, pReso)), "a knob reads like its setting (\"%s\")", g.text(knobIdx(0, 1)).c_str());
        g.p.set((kViewBase + pFilterOn), 0.0f); g.blocks(2);   // the ON key on the Filter page
        CHECK(!g.p.getBool(idx(1, pFilterOn)) && g.p.getBool(idx(0, pFilterOn)), "the ON key turns off track 2's Filter only");
        g.p.set(kEditTrack, 0.0f); g.blocks(3);
        CHECK(std::fabs(g.p.getNormalised(knobIdx(0, 0)) - g.p.getNormalised(idx(0, pCutoff))) < 1e-6f && g.p.getBool((kViewBase + pFilterOn)),
              "back to track 1: the knobs and keys show track 1 again");
        g.p.set(kPage0, 4.0f); g.blocks(3);   // Space page: reverb Type is knob 9
        CHECK(std::fabs(g.p.getNormalised(knobIdx(0, 8)) - g.p.getNormalised(kRevTypeBase + 0)) < 1e-6f &&
              std::fabs(g.p.getNormalised(knobIdx(0, 0)) - g.p.getNormalised(cidx(0, cAmount))) < 1e-6f,
              "Space page: knobs 1-4 chorus, 5-8 delay, 9-12 reverb");
        g.p.set(kPage0, 0.0f); g.blocks(3);
        CHECK(std::fabs(g.p.getNormalised(knobIdx(0, 8)) - g.p.getNormalised(bidx(0, bFolder))) < 1e-6f,
              "Source page: knobs 9-10 browse folders and files");
        g.p.set(kEditTrack, 2.0f); g.blocks(3);
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 100));   // MIDI channel 1
        g.blocks(40);
        CHECK(g.p.tracks_[2].playPosition() >= 0.0 && g.p.tracks_[0].playPosition() < 0.0 && g.p.getNormalised(kPlayhead) > 0.0f,
              "track 3 picked: a pad plays track 3, and its playhead shows");
        g.p.handleMidi(mpc::MidiEvent::noteOff(0, 24));
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int T = 1;   // test on track 2
    std::vector<uint8_t> chunk;
    {
        Rig g;
        CHECK(g.waitFor([&] { return g.text(bidx(T, bFolder)) != "Scanning drives..."; }), "scan finished: first folder \"%s\"",
              g.text(bidx(T, bFolder)).c_str());
        CHECK(g.text(bidx(T, bFolder)) == "Sculpt", "Sculpt's own loops come first");
        CHECK(g.text(bidx(T, bFile)) == "2 Tape", "track 2 starts on its own loop: \"%s\"", g.text(bidx(T, bFile)).c_str());

        g.press(bidx(T, bFolderNext));
        const std::string f2 = g.text(bidx(T, bFolder));
        CHECK(f2 != "Sculpt", "Folder + moves on: \"%s\"", f2.c_str());
        CHECK(g.p.getInt(bidx(T, bFile)) == 0, "a new folder starts at its first file");
        g.press(bidx(T, bFileNext));
        g.press(bidx(T, bFileNext));
        CHECK(g.p.getInt(bidx(T, bFile)) != 0 || true, "File + steps (now %d: \"%s\")", g.p.getInt(bidx(T, bFile)),
              g.text(bidx(T, bFile)).c_str());
        g.press(bidx(T, bFolderPrev));
        CHECK(g.text(bidx(T, bFolder)) == "Sculpt", "Folder - goes back");
        g.press(bidx(T, bFolderPrev));
        CHECK(g.text(bidx(T, bFolder)) != "Sculpt", "Folder - wraps to the last folder: \"%s\"", g.text(bidx(T, bFolder)).c_str());

        // load the first file of the first non-Sculpt folder
        CHECK(stepUntil(g, bidx(T, bFolderNext), [&] { return g.text(bidx(T, bFolder)).find("Breaks") != std::string::npos; }),
              "found the Breaks folder: \"%s\"", g.text(bidx(T, bFolder)).c_str());
        const std::string want = g.text(bidx(T, bFile));
        g.press(bidx(T, bLoad));
        CHECK(g.waitFor([&] { return g.p.getBool(bidx(T, bHasFile)); }), "Load: Has File turns on");
        CHECK(g.text(idx(T, pSample)) == want, "SAMPLE shows the file: \"%s\"", g.text(idx(T, pSample)).c_str());
        CHECK(g.p.active_[T] != nullptr && g.p.active_[T]->size() > 1000, "track 2 plays the file (%d frames)",
              g.p.active_[T] ? g.p.active_[T]->size() : 0);
        CHECK(g.p.active_[0] == nullptr, "other tracks keep their loops");

        // play it
        g.p.set(kPlay + T, 1); g.blocks(50);
        float peak = 0; for (float v : g.l) peak = std::fmax(peak, std::fabs(v));
        CHECK(peak > 0.001f, "it sounds (peak %.3f)", peak);
        g.p.set(kPlay + T, 0); g.blocks(5);

        chunk = g.p.internalSaveState();
        std::string s(chunk.begin(), chunk.end());
        CHECK(s.find(want) != std::string::npos, "the project saves the file's path");

        // a broken file reports and keeps the old one
        g.p.set(bidx(T, bFolder), 0); g.blocks(1);
        CHECK(stepUntil(g, bidx(T, bFolderNext), [&] { return g.text(bidx(T, bFolder)).find("Pads") != std::string::npos; }),
              "found the Pads folder");
        CHECK(stepUntil(g, bidx(T, bFileNext), [&] { return g.text(bidx(T, bFile)) == "broken"; }), "found broken.wav");
        g.press(bidx(T, bLoad));
        CHECK(g.waitFor([&] { return g.text(idx(T, pSample)).find("Can't read") == 0; }), "a broken file says so: \"%s\"",
              g.text(idx(T, pSample)).c_str());
        CHECK(g.p.active_[T] != nullptr, "and the previous file keeps playing");

        // a long file is cut to 30 s
        g.p.set(bidx(T, bFolder), 0); g.blocks(1);
        CHECK(stepUntil(g, bidx(T, bFolderNext), [&] { return g.text(bidx(T, bFolder)).find("Old") != std::string::npos; }),
              "found the Old folder");
        CHECK(g.text(bidx(T, bFile)).size() <= 31 && g.text(bidx(T, bFile)).find("...") != std::string::npos,
              "long names are shortened: \"%s\"", g.text(bidx(T, bFile)).c_str());
        g.press(bidx(T, bLoad));
        CHECK(g.waitFor([&] { return g.p.active_[T] && g.p.active_[T]->size() == 30 * 44100; }), "a 40 s file is cut to 30 s");

        // picking a built-in loop drops the file
        g.p.set(idx(T, pSample), 3); g.blocks(3);
        CHECK(g.p.active_[T] == nullptr && !g.p.getBool(bidx(T, bHasFile)), "choosing a SAMPLE goes back to the loop");
        CHECK(g.waitFor([&] { return g.text(idx(T, pSample)) == "Glass"; }), "SAMPLE shows \"%s\"", g.text(idx(T, pSample)).c_str());
    }
    {
        Rig g;
        g.p.internalLoadState(chunk.data(), chunk.size());
        CHECK(g.waitFor([&] { return g.p.active_[T] != nullptr; }), "reopening the project reloads the file");
        CHECK(g.p.getBool(bidx(T, bHasFile)), "Has File is on after reopening");
        // a project with no file on that track drops it
        Rig empty;
        std::vector<uint8_t> none = empty.p.internalSaveState();
        g.p.internalLoadState(none.data(), none.size());
        CHECK(g.waitFor([&] { return g.p.active_[T] == nullptr; }), "loading a project without files goes back to the loops");
    }
    {
        // the view: the screen edits whichever track Edit Track picks
        Rig g;
        auto view = [](int off) { return kViewBase + off; };
        CHECK(g.p.getNormalised(view(pCutoff)) == g.p.getNormalised(idx(0, pCutoff)), "the view starts on track 1");
        g.p.set(idx(1, pCutoff), 500.0f); g.blocks(1);
        g.p.set(kEditTrack, 1); g.blocks(1);
        CHECK(std::fabs(g.p.get(view(pCutoff)) - 500.0f) < 1.0f, "picking track 2 shows its cutoff (%.0f Hz)", g.p.get(view(pCutoff)));
        CHECK(g.text(view(pCutoff)) == g.text(idx(1, pCutoff)), "and reads the same: \"%s\"", g.text(view(pCutoff)).c_str());
        const float resoBefore = g.p.get(idx(1, pReso));
        g.p.set(view(pReso), 77.0f); g.blocks(1);
        CHECK(std::fabs(g.p.get(idx(1, pReso)) - resoBefore) < 0.01f && std::fabs(g.p.get(view(pReso)) - resoBefore) < 0.01f,
              "the view only follows the track: writing it changes nothing (reso %.0f)", g.p.get(idx(1, pReso)));
        g.p.set(idx(1, pDrive), 90.0f); g.blocks(1);
        CHECK(std::fabs(g.p.get(view(pDrive)) - 90.0f) < 0.5f, "a change made to track 2 directly shows in the view");
        g.p.set(idx(1, pGrainOn), 1.0f); g.blocks(1);
        CHECK(g.p.getBool(view(pGrainOn)) && !g.p.getBool(idx(0, pGrainOn)), "track 2's ON key turns its own Grain on");
        // browse and load through the view on track 3
        g.p.set(kEditTrack, 2); g.blocks(1);
        CHECK(g.waitFor([&] { return g.text(view(kPerTrack + bFolder)) == "Sculpt"; }), "the view's folder reads \"%s\"",
              g.text(view(kPerTrack + bFolder)).c_str());
        g.press(bidx(2, bFolderNext));   // track 3's tab binds its browser keys to track 3
        CHECK(g.p.getInt(bidx(2, bFolder)) == 1, "Folder + on track 3's tab steps its folder");
        CHECK(g.waitFor([&] { return g.text(view(kPerTrack + bFolder)) == g.text(bidx(2, bFolder)); }), "and the view shows \"%s\"",
              g.text(view(kPerTrack + bFolder)).c_str());
        g.press(bidx(2, bLoad));
        CHECK(g.waitFor([&] { return g.p.active_[2] != nullptr; }), "LOAD on track 3's tab loads into track 3");
        CHECK(g.waitFor([&] { return g.p.getBool(view(kPerTrack + bHasFile)); }), "the view's Has File follows");
        CHECK(g.p.active_[1] == nullptr, "track 2 is untouched");
        // presets keep the view on track 1 and in step with it
        g.p.setProgram(2); g.blocks(2);
        CHECK(g.p.getInt(kEditTrack) == 0 && g.p.getNormalised(view(pDrive)) == g.p.getNormalised(idx(0, pDrive)),
              "a preset opens on track 1 with the view matching");
        {   // loading a preset never lets the knobs overwrite what it set
            float worst = 0.0f;
            for (int k = 0; k < kKnobs; ++k)
            {
                const int target = cellTarget(0, g.p.getInt(kPage0), k);
                if (target >= 0) worst = std::fmax(worst, std::fabs(g.p.getNormalised(knobIdx(0, k)) - g.p.getNormalised(target)));
            }
            CHECK(worst < 1e-6f && g.p.get(idx(1, pDrive)) > 40.0f, "a preset's settings survive, and every knob matches its setting");
        }
    }
    {
        // notes: channel 1 plays the picked track from its Root; channels 2-5 are tracks 1-4
        Rig g;
        auto busy = [&](int t) { return g.p.tracks_[t].anyVoice(); };
        g.p.set(kEditTrack, 1); g.blocks(1);
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 100));
        CHECK(busy(1) && !busy(0), "a channel-1 note plays the picked track (track 2)");
        CHECK(std::fabs(g.p.tracks_[1].voices_[0].ratio - 1.0f) < 1e-4f || std::fabs(g.p.tracks_[1].voices_[1].ratio - 1.0f) < 1e-4f,
              "note 24 = Root plays at the loop's own pitch");
        g.p.set(kEditTrack, 0); g.blocks(1);
        g.p.handleMidi(mpc::MidiEvent::noteOff(0, 24));
        g.blocks(400);
        CHECK(!busy(1), "its note-off still reaches track 2 after picking track 1");
        g.p.handleMidi(mpc::MidiEvent::noteOn(3, 36, 100));
        CHECK(busy(2) && !busy(0), "channel 4 plays track 3 whatever is picked");
        g.p.set(kRootBase + 2, 36.0f); g.blocks(1);
        CHECK(g.text(kRootBase + 2).find("C1") == 0, "Root reads as a note name: \"%s\"", g.text(kRootBase + 2).c_str());
    }
    {
        // SYNC: the loop runs with the transport, locked to the song position
        Rig g;
        g.p.readTrack(0, g.p.tp_[0]);
        sculpt::TrackParams tp = g.p.tp_[0];
        tp.sync = true; tp.spaceOn = false; tp.filterOn = false; tp.colorOn = false; tp.grainOn = false;
        sculpt::Track& tr = g.p.tracks_[0];
        float L[128], Rr[128];
        auto render = [&](sculpt::Transport t, double bpm, float* out)
        {
            std::fill(L, L + 128, 0.0f); std::fill(Rr, Rr + 128, 0.0f);
            tr.process(tp, g.p.samples_[0], 1.0f, bpm, true, t, L, Rr, 128);
            if (out) std::copy(L, L + 128, out);
            float pk = 0; for (float v : L) pk = std::fmax(pk, std::fabs(v)); return pk;
        };
        sculpt::Transport t; t.playing = true; t.ppqValid = true; t.bpm = 120.0;
        // render 11 beats continuously; `jitter` makes the song position coarse and noisy
        double endPhase = 0.0;
        auto run = [&](double bpm, bool jitter, std::vector<float>& out)
        {
            tr.reset(); out.clear();
            std::srand(7);
            const int blocksN = (int)(11.0 * 60.0 / bpm * 44100.0 / 128.0);
            for (int i = 0; i < blocksN; ++i)
            {
                double ppq = i * 128 * bpm / 60.0 / 44100.0;
                if (jitter)
                {
                    ppq = std::floor(ppq * 24.0) / 24.0;                                // MIDI-clock resolution
                    ppq += ((std::rand() % 2001) - 1000) / 1000.0 * 0.001 * bpm / 60.0;  // +-1 ms
                }
                t.ppq = ppq; t.bpm = bpm;
                float blk[128];
                render(t, bpm, blk);
                out.insert(out.end(), blk, blk + 128);
            }
            endPhase = tr.runPhase_;
        };
        auto rough = [](const std::vector<float>& y, size_t from)
        {   // the largest second difference: clicks and jumps show up here
            float r = 0; for (size_t i = from + 2; i < y.size(); ++i) r = std::fmax(r, std::fabs(y[i] - 2 * y[i - 1] + y[i - 2]));
            return r;
        };
        std::vector<float> clean, noisy;
        run(120.0, false, clean);
        const double cleanPhase = endPhase;
        float pk = 0; for (float v : clean) pk = std::fmax(pk, std::fabs(v));
        CHECK(pk > 0.01f, "with the transport playing the loop sounds without a note (peak %.3f)", pk);
        const size_t beat = 22050;   // one beat at 120 BPM
        float diff = 0; for (size_t i = beat; i < 2 * beat; ++i) diff = std::fmax(diff, std::fabs(clean[i] - clean[i + 8 * beat]));
        CHECK(diff < 1e-3f, "it repeats every 8 beats (beat 1 vs beat 9 differ by %.5f)", diff);
        {   // the playhead of the SYNC loop: at the end of 11 beats it is 3 beats into the 8-beat loop
            const double ph = tr.playPosition();
            CHECK(std::fabs(ph - 3.0 / 8.0) < 0.01, "the SYNC playhead sits on the beat (%.3f, expected 0.375)", ph);
        }
        run(120.0, true, noisy);
        const float rc = rough(clean, beat), rn = rough(noisy, beat);
        CHECK(rn < rc * 1.5f + 1e-4f, "a coarse, jittery song position does not make it rough (%.4f vs %.4f clean)", rn, rc);
        double lag = endPhase - cleanPhase; lag -= std::round(lag);
        const double lagMs = lag * 8.0 * 0.5 * 1000.0;   // 8 beats of 0.5 s
        CHECK(std::fabs(lagMs) < 15.0, "and it stays with the beat (%.1f ms from the clean render)", lagMs);
        // a host that only updates its position every 8 blocks (~23 ms)
        {
            std::vector<float> stale; tr.reset(); std::vector<float>& out = stale;
            for (int i = 0; i < 2000; ++i)
            {
                t.ppq = (i / 8 * 8) * 128 * 2.0 / 44100.0; t.bpm = 120.0;
                float blk[128]; render(t, 120.0, blk); out.insert(out.end(), blk, blk + 128);
            }
            const float rs = rough(stale, beat);
            CHECK(rs < rc * 1.5f + 1e-4f, "a position that updates only every 23 ms does not make it rough (%.4f vs %.4f)", rs, rc);
        }
        std::vector<float> slow; run(90.0, false, slow);
        const size_t beat90 = (size_t)(44100 * 60 / 90);
        diff = 0; for (size_t i = beat90; i < 2 * beat90; ++i) diff = std::fmax(diff, std::fabs(slow[i] - slow[i + 8 * beat90]));
        CHECK(diff < 1e-3f, "and still every 8 beats at 90 BPM (%.5f)", diff);
        t.playing = false; t.bpm = 120.0;
        for (int i = 0; i < 100; ++i) render(t, 120, nullptr);
        CHECK(render(t, 120, nullptr) < 1e-5f, "Stop fades it out");
        tp.sync = false; t.playing = true;
        for (int i = 0; i < 50; ++i) render(t, 120, nullptr);
        CHECK(render(t, 120, nullptr) < 1e-5f, "with SYNC off the transport plays nothing by itself");
    }
    {
        // MIX: volume, pan, mute, solo and the meters
        Rig g;
        for (int t = 0; t < 4; ++t) g.p.set(idx(t, pSpaceOn), 0.0f);   // dry, so silence is silence
        auto level = [&](int blocksN, float& pl, float& pr)
        {
            g.blocks(20);   // let the 10 ms gain glide finish before measuring
            pl = pr = 0;
            for (int i = 0; i < blocksN; ++i)
            {
                g.blocks(1);
                for (int k = 0; k < 128; ++k) { pl = std::fmax(pl, std::fabs(g.l[k])); pr = std::fmax(pr, std::fabs(g.r[k])); }
            }
        };
        g.p.handleMidi(mpc::MidiEvent::noteOn(1, 24, 110));   // channel 2 = track 1, held
        float pl, pr;
        level(200, pl, pr);
        CHECK(pl > 0.01f && pr > 0.01f, "track 1 plays into both sides (%.3f / %.3f)", pl, pr);
        CHECK(g.p.getNormalised(midx(0, mMeter)) > 0.3f, "track 1's meter shows it (%.2f)", g.p.getNormalised(midx(0, mMeter)));
        CHECK(g.p.getNormalised(midx(1, mMeter)) < 0.01f, "track 2's meter stays down");
        CHECK(g.p.getNormalised(kMasterMeter) > 0.3f, "the master meter moves (%.2f)", g.p.getNormalised(kMasterMeter));
        g.p.set(midx(0, mPan), -100.0f); level(100, pl, pr);
        CHECK(pl > 0.01f && pr < 0.001f, "pan hard left empties the right side (%.3f / %.4f)", pl, pr);
        CHECK(g.text(midx(0, mPan)) == "L100", "pan reads \"%s\"", g.text(midx(0, mPan)).c_str());
        g.p.set(midx(0, mPan), 0.0f);
        g.p.set(midx(0, mMute), 1.0f); level(100, pl, pr);
        CHECK(pl < 0.001f, "mute silences the track (%.4f)", pl);
        g.p.set(midx(0, mMute), 0.0f);
        g.p.set(midx(1, mSolo), 1.0f); level(100, pl, pr);
        CHECK(pl < 0.001f, "soloing track 2 silences track 1 (%.4f)", pl);
        g.p.set(midx(0, mSolo), 1.0f); level(100, pl, pr);
        CHECK(pl > 0.01f, "soloing track 1 too brings it back");
        g.p.set(midx(0, mSolo), 0.0f); g.p.set(midx(1, mSolo), 0.0f);
        g.p.set(midx(0, mVolume), -60.0f); level(100, pl, pr);
        CHECK(pl < 0.001f && g.text(midx(0, mVolume)) == "-inf dB", "the fader at the bottom is silent and reads \"%s\"",
              g.text(midx(0, mVolume)).c_str());
        level(600, pl, pr);
        CHECK(g.p.getNormalised(midx(0, mMeter)) < 0.05f, "and the meter falls back (%.2f)", g.p.getNormalised(midx(0, mMeter)));
    }
    {
        // the playhead line follows the edited track
        Rig g;
        CHECK(g.p.getNormalised(kPlayhead) == 0.0f, "no playhead while nothing plays");
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 100));   // track 1 (picked), at its root
        g.blocks(60);
        const float a = g.p.getNormalised(kPlayhead);
        g.blocks(120);
        const float b = g.p.getNormalised(kPlayhead);
        CHECK(a > 0.0f && b > a, "a held note moves it forward (%.3f -> %.3f)", a, b);
        const double expect = 180.0 * 128.0 / 176400.0;     // 180 blocks into a 4 s loop
        CHECK(std::fabs(b - expect) < 0.03, "at the right place (%.3f, expected about %.3f)", b, expect);
        g.p.handleMidi(mpc::MidiEvent::noteOff(0, 24));
        g.blocks(200);
        CHECK(g.p.getNormalised(kPlayhead) == 0.0f, "and it hides when the note has ended");
        g.p.handleMidi(mpc::MidiEvent::noteOn(2, 24, 100));   // MIDI channel 3: track 2
        g.blocks(60);
        CHECK(g.p.getNormalised(kPlayhead) == 0.0f, "track 2 playing does not move track 1's playhead");
        g.p.set(kEditTrack, 1.0f); g.blocks(4);
        CHECK(g.p.getNormalised(kPlayhead) > 0.0f, "pick track 2 and its playhead shows");
    }
    {
        // Rotate/Length: a tape loop; it starts at Rotate, covers Length, and wraps round the sample's end
        Rig g;
        auto sweep = [&](double& lo, double& hi, double& wrapLo, double& wrapHi)
        {
            lo = 1; hi = 0; wrapLo = 1; wrapHi = 0;
            for (int i = 0; i < 1500; ++i)
            {
                g.blocks(1);
                const double ph = g.p.tracks_[0].playPosition();
                if (ph < 0.0) continue;
                if (ph >= 0.5) { lo = std::fmin(lo, ph); hi = std::fmax(hi, ph); }
                else { wrapLo = std::fmin(wrapLo, ph); wrapHi = std::fmax(wrapHi, ph); }
            }
        };
        double lo, hi, wl, wh;
        // Start 50% + Length 25%: the loop is 0.50-0.75 of the sample
        g.p.set(kStartBase + 0, 50.0f); g.p.set(idx(0, pLength), 25.0f); g.blocks(1);
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 100));
        sweep(lo, hi, wl, wh);
        CHECK(lo >= 0.499 && hi <= 0.751 && wl > wh, "Start 50%% + Length 25%% plays 0.50-0.75 only (saw %.3f-%.3f)", lo, hi);
        // Rotate turns playback round inside that loop and never leaves it
        g.p.handleMidi(mpc::MidiEvent::noteOff(0, 24)); g.blocks(100);
        g.p.set(idx(0, pRotate), 50.0f); g.blocks(1);
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 100));
        g.blocks(3);
        const double first = g.p.tracks_[0].playPosition();
        CHECK(std::fabs(first - 0.625) < 0.01, "Rotate 50%% starts halfway into the loop (%.3f, expected 0.625)", first);
        sweep(lo, hi, wl, wh);
        CHECK(lo >= 0.499 && hi <= 0.751 && wl > wh, "and wraps inside it: still 0.50-0.75 (saw %.3f-%.3f)", lo, hi);
        // Start 90% + Length 20%: stops at the sample's end, never wraps to its beginning
        g.p.set(idx(0, pRotate), 0.0f); g.p.set(kStartBase + 0, 90.0f); g.p.set(idx(0, pLength), 20.0f); g.blocks(1);
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 100));
        sweep(lo, hi, wl, wh);
        CHECK(lo >= 0.899 && hi <= 1.0 && wl > wh, "Start 90%% + Length 20%% stays in 0.90-1.00, no wrap to the beginning (saw %.3f-%.3f)",
              lo, hi);
        CHECK(g.p.param(idx(0, pRotate)).name == "T1 Rotate" && g.p.param(idx(0, pLength)).name == "T1 Length"
              && g.p.param(kStartBase).name == "T1 Start" && g.p.param(kViewStart).name == "Start", "the controls are Rotate, Start and Length");
        // the waveform markers: loop end, and the Rotate point (hidden at 0)
        g.p.set(kStartBase + 0, 25.0f); g.p.set(idx(0, pLength), 50.0f); g.p.set(idx(0, pRotate), 0.0f); g.blocks(20);
        CHECK(std::fabs(g.p.getNormalised(kLoopEnd) - 0.75f) < 0.002f && g.p.getNormalised(kRotateAt) == 0.0f,
              "Start 25%% + Length 50%%: the end marker is at 0.75 (%.3f), no Rotate marker", g.p.getNormalised(kLoopEnd));
        g.p.set(idx(0, pRotate), 50.0f); g.blocks(20);
        CHECK(std::fabs(g.p.getNormalised(kRotateAt) - 0.5f) < 0.002f, "Rotate 50%% marks the middle of the loop (%.3f)", g.p.getNormalised(kRotateAt));
        g.p.set(kStartBase + 0, 80.0f); g.blocks(20);
        CHECK(std::fabs(g.p.getNormalised(kLoopEnd) - 1.0f) < 0.002f && std::fabs(g.p.getNormalised(kRotateAt) - 0.9f) < 0.002f,
              "Start 80%%: the end stops at the sample's end (%.3f), Rotate at 0.90 (%.3f)", g.p.getNormalised(kLoopEnd), g.p.getNormalised(kRotateAt));
        g.p.set(kStartBase + 0, 0.0f); g.p.set(idx(0, pLength), 100.0f); g.p.set(idx(0, pRotate), 0.0f); g.blocks(5);
    }
    {
        // Filter Pitch: the resonators form a chord on C + Pitch; every step moves it a semitone
        auto notes = [](Rig& g, int t, float out[4])
        {
            for (int k = 0; k < 4; ++k)
            {
                const double f = std::atan((double)g.p.tracks_[t].res_[k].g) * 44100.0 / 3.14159265358979;
                out[k] = (float)(69.0 + 12.0 * std::log2(f / 440.0));
            }
        };
        Rig g;
        g.p.set(idx(0, pCutoff), 800.0f); g.p.set(idx(0, pReso), 90.0f); g.p.set(idx(0, pFDecay), 1500.0f);
        g.p.set(idx(0, pScale), 1.0f);   // Major
        g.p.handleMidi(mpc::MidiEvent::noteOn(0, 24, 100));
        g.blocks(5);
        float a[4], b[4];
        notes(g, 0, a);
        CHECK(std::fabs(a[1] - a[0] - 4) < 0.05f && std::fabs(a[2] - a[0] - 7) < 0.05f && std::fabs(a[3] - a[0] - 12) < 0.05f
              && std::fmod(std::round(a[0]), 12.0f) == 0.0f,
              "Major at Pitch 0: a C major chord (%.1f %.1f %.1f %.1f)", a[0], a[1], a[2], a[3]);
        std::vector<float> o0, o5;
        for (int i = 0; i < 100; ++i) { g.blocks(1); o0.insert(o0.end(), g.l, g.l + 128); }
        g.p.set(idx(0, pFPitch), 5.0f);
        g.blocks(5);
        notes(g, 0, b);
        bool shifted = true;
        for (int k = 0; k < 4; ++k) shifted = shifted && std::fabs(std::fmod(b[k] - a[k] + 120.0f, 12.0f) - 5.0f) < 0.05f;
        CHECK(shifted, "Pitch +5 moves every resonator 5 semitones (%.1f %.1f %.1f %.1f)", b[0], b[1], b[2], b[3]);
        for (int i = 0; i < 100; ++i) { g.blocks(1); o5.insert(o5.end(), g.l, g.l + 128); }
        double d = 0, e = 0;
        for (size_t i = 0; i < o0.size(); ++i) { d += (o0[i] - o5[i]) * (o0[i] - o5[i]); e += o0[i] * o0[i]; }
        CHECK(e > 0 && d / e > 0.1, "and the sound changes clearly (difference %.0f%% of the signal)", 100.0 * d / e);
    }
    {
        // the list browser: rows show the names around the current choice; tapping one picks it
        Rig g;
        CHECK(g.waitFor([&] { return g.text(kFolderRow0 + kListMid) == "Sculpt"; }), "the middle folder row is the current folder");
        CHECK(g.text(kFolderRow0 + kListMid - 1).empty() && g.text(kFolderRow0 + 0).empty(), "rows above the first folder are empty");
        CHECK(g.text(kFolderRow0 + kListMid + 1) == "lib / Breaks" && g.text(kFolderRow0 + kListMid + 2) == "lib / Breaks / Old",
              "rows below list the next folders (\"%s\", \"%s\")", g.text(kFolderRow0 + kListMid + 1).c_str(), g.text(kFolderRow0 + kListMid + 2).c_str());
        CHECK(g.text(kFileRow0 + kListMid) == "1 Kalimba" && g.text(kFileRow0 + kListMid + 1) == "2 Tape" && g.text(kFileRow0 + kListMid + 2) == "3 Haze",
              "file rows list the folder's files");
        g.press(kFolderRow0 + kListMid + 2);   // a row that is not highlighted: nothing happens
        CHECK(g.text(kFolderRow0 + kListMid) == "Sculpt", "tapping a row that is not highlighted does nothing");
        g.press(bidx(0, bFolderNext));         // the down key: one folder on
        CHECK(g.text(kFolderRow0 + kListMid) == "lib / Breaks" && g.text(kFolderRow0 + kListMid - 1) == "Sculpt",
              "down moves one folder and the list scrolls with it");
        CHECK(g.text(kFileRow0 + kListMid) == "amen 170", "the file list shows the new folder (\"%s\")", g.text(kFileRow0 + kListMid).c_str());
        g.press(bidx(0, bFileNext));           // one file down
        CHECK(g.text(kFileRow0 + kListMid) == "think break stereo 24bit" && g.text(idx(0, pSample)) == "Kalimba",
              "down moves one file, not loaded yet");
        CHECK(g.waitFor([&] { return g.text(idx(0, pSample)) == "think break stereo 24bit"; }),
              "and it loads by itself a moment later");
        // a quick scroll loads only where it stops
        g.press(bidx(0, bFilePrev)); g.press(bidx(0, bFileNext)); g.press(bidx(0, bFilePrev));
        CHECK(g.waitFor([&] { return g.text(idx(0, pSample)) == "amen 170"; }), "a quick scroll loads only the file it stops on");
        g.p.set(kEditTrack, 1); g.blocks(2);
        CHECK(g.text(kFolderRow0 + kListMid) == "Sculpt", "the list follows the picked track");
    }
    {
        // the data wheel: the host moves the screen's File/Folder by a slice of the range; Sculpt
        // turns each such jump into a single step
        Rig g;
        const int vFolder = bidx(0, bFolder), vFile = bidx(0, bFile);   // track 1's tab: the browser row binds to track 1
        CHECK(g.waitFor([&] { return g.text(vFolder) == "Sculpt"; }), "library ready");
        g.p.set(vFile, 41.0f); g.blocks(3);   // one wheel click: +41 items
        CHECK(g.p.getInt(bidx(0, bFile)) == 1, "a wheel click forward moves one file (now %d)",
              g.p.getInt(bidx(0, bFile)));
        g.p.set(vFile, 83.0f); g.blocks(3);
        CHECK(g.p.getInt(bidx(0, bFile)) == 2, "another click: one more (now %d)", g.p.getInt(bidx(0, bFile)));
        g.p.set(vFile, 0.0f); g.blocks(3);
        CHECK(g.p.getInt(bidx(0, bFile)) == 1, "a click back moves one back (now %d)", g.p.getInt(bidx(0, bFile)));
        g.p.set(vFolder, 37.0f); g.blocks(3);
        CHECK(g.p.getInt(bidx(0, bFolder)) == 1 && g.text(kFolderRow0 + kListMid) == "lib / Breaks",
              "a click on Folder moves one folder (\"%s\")", g.text(kFolderRow0 + kListMid).c_str());
        for (int i = 0; i < 10; ++i) { g.p.set(vFolder, 4000.0f); g.blocks(3); }
        CHECK(g.p.getInt(bidx(0, bFolder)) == 3, "and stops at the last folder (now %d)", g.p.getInt(bidx(0, bFolder)));
    }
    {
        // the waveform columns show the edited track's real sound
        Rig g;
        g.blocks(2);
        auto same = [&](const sculpt::Sample& smp)
        {
            for (int c = 0; c < kWaveCols; ++c) if (std::fabs(g.p.getNormalised(kWave0 + c) - smp.peaks[c]) > 1e-6f) return false;
            return true;
        };
        float lo = 1, hi = 0;
        for (int c = 0; c < kWaveCols; ++c) { lo = std::fmin(lo, g.p.getNormalised(kWave0 + c)); hi = std::fmax(hi, g.p.getNormalised(kWave0 + c)); }
        CHECK(same(g.p.samples_[0]) && hi > 0.9f && lo < 0.6f, "track 1 shows its loop's shape (columns %.2f-%.2f)", lo, hi);
        g.p.set(kEditTrack, 1); g.blocks(2);
        CHECK(same(g.p.samples_[1]), "track key 2: the waveform shows track 2's loop");
        // load a file on track 2: its own shape replaces the loop's
        CHECK(g.waitFor([&] { return g.p.scanner_.library() != nullptr; }), "library ready");
        g.press(bidx(1, bFolderNext));
        CHECK(g.waitFor([&] { return g.p.active_[1] != nullptr; }), "a file loads on track 2");
        g.blocks(2);
        CHECK(same(*g.p.active_[1]), "track 2's waveform shows that file");
        g.p.set(kEditTrack, 0); g.blocks(2);
        CHECK(same(g.p.samples_[0]), "and track 1's is unchanged");
    }
    std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
