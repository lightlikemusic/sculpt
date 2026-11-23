// Sculpt - a four-track loop-shaping instrument for the MPC, by Lightlike.
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Lightlike
//
// Signal flow, per track (four tracks, summed to one stereo output):
//
//   loop in Content/Samples  ->  SOURCE  ->  GRAIN  ->  FILTER  ->  COLOR  ->  SPACE  ->  out
//                                varispeed    cloud of     lowpass +    drive, comp,   delay +
//                                player with  grains read  scale-tuned  crush, tilt,   reverb
//                                rotate/len   from the     resonators   noise burst    (stereo)
//                                region       recent past
//
// Every module has eight parameters (the eight knobs on its screen) and an on/off switch.
// SOURCE, GRAIN and SPACE also have one toggle each: Sync (the loop runs by itself with the
// host's transport, locked to the beat),
// Lock (the grain buffer stops recording) and Freeze (the room holds forever).
//
// Playing it: notes on MIDI channel 1 play the track picked on the screen (Edit Track),
// chromatically: the track's Root note plays its loop at its own pitch. Channels 2-5 always
// play tracks 1-4. The four Play parameters start (on) and stop (off) a track at its Root.
//
// Parameter layout (permanent; append new parameters at the end only):
//   0        Volume
//   1..4     Play 1..4 (the screen's track keys)
//   5 + 48*t ... track t (0..3): 8 Source, 8 Grain, 8 Filter, 8 Color, 8 Space,
//            5 module switches, Sync, Lock, Freeze
//   197 + 8*t    track t's sample browser: Folder, File, Load, Folder -/+, File -/+, Has File
//   229      Edit Track (the track the screen shows)
//   230..285 the view: copies of the edited track's 56 settings, which the screen is bound to
//   286..289 Root of tracks 1-4 (the note that plays the loop at its own pitch)
//   290      the view's Root
//   291 + 5*t   track t's mix: Volume, Pan, Mute, Solo, Meter (the meter is written by the plugin)
//   311      the master meter
//   312      Playhead: where the edited track plays, for the line on the Source waveform
//   313      Browser (the list browser is open)
//   314..333 the list rows: 10 folders, then 10 files around the current choice (text written by the plugin)
//   334..461 the waveform: 128 columns of the edited track's sound (written by the plugin)
//   462..465 Start of tracks 1-4 (where the loop begins in the sample); 466 the view's Start

#include "mpc_plugin.h"
#include "dsp/wav.h"
#include "engine.h"
#include <complex>
#include <vector>
#include "library.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kNumTracks = 4;
constexpr int kVolume = 0;
constexpr int kPlay = 1;                 // 1..4
constexpr int kTrackBase = 5;
constexpr int kPerTrack = 48;

// offsets inside a track block
enum TrackParam
{
    // Source
    pSpeed = 0, pRotate, pLength, pPitch, pLevel, pFade, pLoop, pSample,
    // Grain
    pGPitch, pGDensity, pGSize, pGSpread, pGReverse, pGFeedback, pGMode, pGWet,   // (Grainbloom's engine)
    // Filter
    pCutoff, pReso, pFDecay, pFPitch, pSlope, pFTone, pScale, pFWet,
    // Color
    pDrive, pComp, pCrush, pTilt, pNoise, pNDecay, pNTone, pCWet,
    // Space
    pDelay, pTime, pReverb, pSize, pFeedback, pSpread, pSTone, pSDecay,
    // switches and toggles
    pSrcOn, pGrainOn, pFilterOn, pColorOn, pSpaceOn, pSync, pLock, pFreeze,
    pCount
};
static_assert(pCount == kPerTrack, "track block size");

inline int idx(int track, int p) { return kTrackBase + track * kPerTrack + p; }
// Grain Size: in the rhythmic modes the same control picks a tempo division, 1/64 .. 2/1
inline int grainDivision(float sizeNormalised) { return mpc::clamp((int)std::lround(sizeNormalised * (mpc::dsp::kNumDivisions - 1)), 0, mpc::dsp::kNumDivisions - 1); }
inline bool grainRhythmic(int mode) { return mode != 1 && mode != 3; }   // all but Cloud and Shimmer

// Sample browser, appended after the four track blocks (8 per track).
constexpr int kBrowseBase = kTrackBase + 4 * kPerTrack;   // 197
enum BrowseParam { bFolder = 0, bFile, bLoad, bFolderPrev, bFolderNext, bFilePrev, bFileNext, bHasFile, bCount };
inline int bidx(int track, int b) { return kBrowseBase + track * bCount + b; }

// The screen edits one track at a time. Edit Track picks it, and the "view" parameters mirror that
// track's 48 module settings and 8 browser settings: the screen and Q-Links are bound to the view,
// and the plugin copies changes both ways. 229 = Edit Track, 230.. = the view.
constexpr int kEditTrack = kBrowseBase + 4 * bCount;     // 229
constexpr int kViewBase = kEditTrack + 1;                 // 230
constexpr int kRootBase = kViewBase + kPerTrack + bCount; // 286: Root, one per track (added later)
constexpr int kViewRoot = kRootBase + 4;                  // 290: the view's Root
constexpr int kNumView = kPerTrack + bCount + 8;          // 64 mirrored: 48 + 8 browser + Root + Start + 4 chorus + reverb Mix and Type
// Mix, after the view's Root: per track Volume, Pan, Mute, Solo and a meter; then the master meter.
constexpr int kMixBase = kViewRoot + 1;                   // 291
enum MixParam { mVolume = 0, mPan, mMute, mSolo, mMeter, mCount };
inline int midx(int t, int m) { return kMixBase + t * mCount + m; }
constexpr int kMasterMeter = kMixBase + 4 * mCount;       // 311
constexpr int kPlayhead = kMasterMeter + 1;               // 312: where the edited track plays (0 = hidden)
// The list browser on the Source page: open/closed, then ten folder rows and ten file rows. Each
// row's text is written by the plugin (the names around the current choice); tapping a row flips it.
constexpr int kBrowserOpen = kPlayhead + 1;               // 313
constexpr int kListRows = 10, kListMid = 4;               // row 5 of 10 is the current choice
constexpr int kFolderRow0 = kBrowserOpen + 1;             // 314..323
constexpr int kFileRow0 = kFolderRow0 + kListRows;        // 324..333
// The waveform of the edited track's sound: one setting per column, written by the plugin.
constexpr int kWave0 = kFileRow0 + kListRows;             // 334..461
constexpr int kWaveCols = sculpt::kWaveCols;
// Start (where the loop begins in the sample), one per track, then its view twin.
constexpr int kStartBase = kWave0 + kWaveCols;            // 462..465
constexpr int kViewStart = kStartBase + 4;                // 466
// Markers on the Source waveform, written by the plugin for the edited track: where the loop ends,
// and where Rotate makes it begin (0 = hidden: Rotate is at 0)
constexpr int kLoopEnd = kViewStart + 1;                  // 467
constexpr int kRotateAt = kLoopEnd + 1;                   // 468
// Stereo chorus (in Space), added later: 4 settings per track, then the view's 4
enum ChorusParam { cAmount = 0, cRate, cDepth, cWidth, cCount };
constexpr int kChorusBase = kRotateAt + 1;                // 469..484
inline int cidx(int t, int c) { return kChorusBase + t * cCount + c; }
constexpr int kViewChorus = kChorusBase + 4 * cCount;     // 485..488
constexpr int kVChorus = kPerTrack + bCount + 2;          // view setting 58: the first chorus one
// Reverb Mix (dry against reverb), added later: one per track, then the view's
constexpr int kRevMixBase = kViewChorus + cCount;         // 489..492
constexpr int kViewRevMix = kRevMixBase + 4;              // 493
constexpr int kVRevMix = kVChorus + cCount;               // view setting 62
// Reverb Type, added later: one per track, then the view's
constexpr int kRevTypeBase = kViewRevMix + 1;             // 494..497
constexpr int kViewRevType = kRevTypeBase + 4;            // 498
constexpr int kVRevType = kVRevMix + 1;                   // view setting 63
// The Grain screen ("Tide"), written by the plugin for the edited track, 64 spots across (oldest
// sound on the left): the last ~3 s of sound the grains pick from (the landscape's height there);
// the same, but only inside the zone Scan and Spray pick from (0 elsewhere: the lit stretch); and
// a ripple at each spot a grain landed on, as one picture number: height and how far it spread.
constexpr int kGrainCols = sculpt::Track::kGrainCols;
constexpr int kGrainCol0 = kViewRevType + 1;              // 499..562: the recording
constexpr int kGrainHot0 = kGrainCol0 + kGrainCols;       // 563..626: the recording, inside the zone only
constexpr int kGrainHit0 = kGrainHot0 + kGrainCols;       // 627..690: where grains play now
// The Grain screen's pixel waveform: the loaded sample (as on Source) in 128 columns; each sends one
// picture number = its height x kPixDissolve + how far it has dissolved (0 = solid: where grains pick).
constexpr int kPixCols = 128, kPixHeights = 18, kPixDissolve = 8;
constexpr int kGrainPix0 = kGrainHit0 + kGrainCols;       // 691..818
constexpr int kGrainEnd = kGrainPix0 + kPixCols;          // 819
// The Filter screen's live spectrum: 64 bands from 20 Hz to 20 kHz on Cutoff's own (log) scale,
// for the sound going into the filter and the sound coming out of it (0 = -66 dB or less, 1 = -24 dB).
constexpr int kSpecBands = 64;
constexpr int kSpecIn0 = kGrainEnd;                       // 819..882
constexpr int kSpecOut0 = kSpecIn0 + kSpecBands;          // 883..946
constexpr int kSpecEnd = kSpecOut0 + kSpecBands;          // 947
// Track tabs: each track's tab shows one module page at a time (Page, per track), and 12 "knob"
// settings per track follow the cells on that page, so the Q-Links and the screen's cells always
// work the page you're on. The plugin copies knob <-> setting both ways.
constexpr int kModules = 5, kKnobs = 12;
constexpr int kPage0 = kSpecEnd;                          // 947..950
constexpr int kKnob0 = kPage0 + kNumTracks;               // 951..998
constexpr int kTabEnd = kKnob0 + kNumTracks * kKnobs;     // 999
inline int knobIdx(int t, int i) { return kKnob0 + t * kKnobs + i; }
inline bool isDisplayIndex(int i) { return i >= kGrainCol0 && i < kSpecEnd; }
constexpr float kMixMinDb = -60.0f, kMixMaxDb = 6.0f;      // faders and meters share this scale
// View setting `v` (0..56): its parameter index, and the parameter it mirrors on track `t`.
inline int viewParam(int v)
{
    if (v == kVRevMix) return kViewRevMix;
    if (v == kVRevType) return kViewRevType;
    if (v >= kVChorus) return kViewChorus + (v - kVChorus);
    return v < kPerTrack + bCount ? kViewBase + v : (v == kPerTrack + bCount ? kViewRoot : kViewStart);
}
inline int viewTarget(int t, int v)
{
    if (v < kPerTrack) return idx(t, v);
    if (v < kPerTrack + bCount) return bidx(t, v - kPerTrack);
    if (v == kVRevMix) return kRevMixBase + t;
    if (v == kVRevType) return kRevTypeBase + t;
    if (v >= kVChorus) return cidx(t, v - kVChorus);
    return v == kPerTrack + bCount ? kRootBase + t : kStartBase + t;
}
// The view setting a parameter index belongs to, or -1.
inline int viewOf(int index)
{
    if (index >= kViewBase && index < kViewBase + kPerTrack + bCount) return index - kViewBase;
    if (index == kViewRoot) return kPerTrack + bCount;
    if (index >= kViewChorus && index < kViewChorus + cCount) return kVChorus + (index - kViewChorus);
    if (index == kViewRevMix) return kVRevMix;
    if (index == kViewRevType) return kVRevType;
    return index == kViewStart ? kPerTrack + bCount + 1 : -1;
}

// What the k-th cell on module page m of track t is (matches MODULES in art/make_skin.py); -1 = none.
inline int cellTarget(int t, int m, int k)
{
    static const int kBlock[4][8] = {
        {-1, pLength, pRotate, pSpeed, pLoop, pFade, pPitch, pLevel},             // Source (cell 0 = Start)
        {pGMode, pGPitch, pGSize, pGDensity, pGSpread, pGReverse, pGFeedback, pGWet},
        {pCutoff, pReso, pFDecay, pFPitch, pSlope, pFTone, pScale, pFWet},
        {pDrive, pComp, pCrush, pTilt, pNoise, pNDecay, pNTone, pCWet}};
    if (m < 0 || m >= kModules || k < 0 || k >= kKnobs) return -1;
    if (m < 4)
    {
        if (m == 0 && k == 8) return bidx(t, bFolder);   // Source's third Q-Link bank browses
        if (m == 0 && k == 9) return bidx(t, bFile);
        if (k >= 8) return -1;
        if (m == 0 && k == 0) return kStartBase + t;
        return idx(t, kBlock[m][k]);
    }
    // Space: chorus (4), delay (4), reverb (4); each column ends with its Mix
    static const int kChorus[4] = {cRate, cDepth, cWidth, cAmount};
    if (k < 4) return cidx(t, kChorus[k]);
    static const int kDelay[4] = {pTime, pFeedback, pSpread, pDelay};
    if (k < 8) return idx(t, kDelay[k - 4]);
    if (k == 8) return kRevTypeBase + t;
    if (k == 9) return idx(t, pSize);
    if (k == 10) return idx(t, pSDecay);
    return kRevMixBase + t;
}

// Set on threads whose value formatting must not count as "the MPC is drawing this" (the debug log).

// The built-in loops (Content/Samples), in the order of the SAMPLE setting.
constexpr int kBuiltins = 10;
const char* const kSampleFiles[kBuiltins] = {"Samples/1 Kalimba.wav", "Samples/2 Tape.wav", "Samples/3 Haze.wav",
                                             "Samples/4 Glass.wav", "Samples/5 Drone.wav", "Samples/6 Analog Pad.wav",
                                             "Samples/7 Synth Arp.wav", "Samples/8 Air Pad.wav", "Samples/9 Dark Drone.wav",
                                             "Samples/10 Club Kick.wav"};

} // namespace

class Sculpt : public mpc::Plugin
{
public:
    Sculpt() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("volume", "Volume", -60.0f, 6.0f, -3.0f, "dB", 1));   // same scale as the MIX faders
        for (int t = 0; t < kNumTracks; ++t)
        {
            // the on-screen track keys: on = the track plays (tap to start, tap again to stop)
            addParam(boolParam(("play" + std::to_string(t + 1)).c_str(), ("Play " + std::to_string(t + 1)).c_str(), false));
        }
        for (int t = 0; t < kNumTracks; ++t) addTrackParams(t);
        for (int t = 0; t < kNumTracks; ++t) addBrowseParams(t);
        addViewParams();
        addMixParams();

        // Units are part of each value's text (formatValue below), so the screen shows "120 ms"
        // whether or not the host appends the unit label itself.
        for (Param& p : params_) { units_.push_back(p.unit); p.unit.clear(); }

        buildPrograms();
        loadSamples();
        // the waveform columns start out showing track 1's loop, so a project saved before any audio
        // has run already holds the right picture
        for (int t = 0; t < kNumTracks; ++t)   // each track's waveform starts as its built-in loop
            if (t == 0) for (int c = 0; c < kWaveCols; ++c) setNormalised(kWave0 + c, samples_[mpc::clamp(getInt(idx(t, pSample)), 0, kBuiltins - 1)].peaks[c]);
        scanner_.start();            // find WAV files on the drives in the background
        loader_.start(&scanner_);
        hostSets_ = std::vector<std::atomic<uint32_t>>((size_t)numParams());
        debugThread_ = std::thread([this] { debugLoop(); });
    }

    ~Sculpt() override
    {
        debugStop_ = true;
        if (debugThread_.joinable()) debugThread_.join();
        loader_.stop();
        scanner_.stop();
        for (sculpt::Sample*& s : active_) { delete s; s = nullptr; }
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        for (int t = 0; t < kNumTracks; ++t) { tracks_[t].setIndex(t); tracks_[t].prepare(sampleRate); }
        nq_.assign((size_t)numParams(), 0); nqFlag_.assign((size_t)numParams(), 0); nqHead_ = nqCount_ = 0;
        reset();
    }

    void reset() override
    {
        // after a load the settings are what counts: the knobs re-read them on the next block
        for (int t = 0; t < kNumTracks; ++t) lastPage_[t] = -1;
        for (sculpt::Track& tr : tracks_) tr.reset();
        for (bool& b : playHeld_) b = false;
        for (int t = 0; t < kNumTracks; ++t)
        {
            for (int b = 0; b < bCount; ++b) keyHeld_[t][b] = getBool(bidx(t, b));
            lastFolder_[t] = getInt(bidx(t, bFolder));
            lastFile_[t] = getInt(bidx(t, bFile));
            loadDue_[t] = 0;
            lastSample_[t] = getInt(idx(t, pSample));
        }
        for (int r = 0; r < 2 * kListRows; ++r) rowHeld_[r] = getBool(kFolderRow0 + r);
        listSig_ = -1;
        waveId_ = 0;   // redraw the waveforms
        for (uint32_t& w : waveIds_) w = 0;
        // the view starts from what it holds now: only later changes are copied either way
        lastEdit_ = getInt(kEditTrack);
        for (int v = 0; v < kNumView; ++v)
        {
            lastView_[v] = getNormalised(viewParam(v));
            lastTarget_[v] = getNormalised(viewTarget(lastEdit_, v));
        }
        gain_ = mpc::dsp::dbToGain(get(kVolume));
        bend_ = 1.0f;
        notifyAll_ = true;   // a program or state load changed values silently: tell the screen
    }

    void process(mpc::ProcessContext& ctx) override
    {
        audioThread_.store(std::this_thread::get_id(), std::memory_order_relaxed);
        blocks_.fetch_add(1, std::memory_order_relaxed);
        const int n = ctx.numFrames;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];

        syncKnobs();   // knobs first, so a knob's change reaches its setting and the view in the same block
        syncView();
        for (int t = 0; t < kNumTracks; ++t) readTrack(t, tp_[t]);
        setupMix();
        browse(n);

        // on-screen track keys (momentary)
        for (int t = 0; t < kNumTracks; ++t)
        {
            const bool held = getBool(kPlay + t);
            if (held && !playHeld_[t]) tracks_[t].noteOn(kPlayNote, 1.0f, 0.85f);
            if (!held && playHeld_[t]) tracks_[t].noteOff(kPlayNote, tp_[t].loop == 0);
            playHeld_[t] = held;
        }

        const double bpm = ctx.transport.bpm;
        const bool bpmValid = ctx.transport.valid && ctx.transport.tempoValid;
        sculpt::Transport tr;
        tr.playing = ctx.transport.valid && ctx.transport.playing;
        tr.ppqValid = ctx.transport.valid && ctx.transport.ppqValid;
        tr.bpm = bpmValid ? bpm : 120.0;
        const double ppqPerFrame = tr.bpm / 60.0 / ctx.sampleRate;
        lastTransport_.store(((tr.playing ? 1 : 0) | (tr.ppqValid ? 2 : 0) | (bpmValid ? 4 : 0)), std::memory_order_relaxed);
        lastPpq_.store(ctx.transport.ppqPos, std::memory_order_relaxed);
        lastBpm_.store(bpm, std::memory_order_relaxed);

        // Render in segments split at MIDI events so notes land on their exact sample.
        int frame = 0, mi = 0;
        while (frame < n)
        {
            while (mi < ctx.numMidiEvents && ctx.midi[mi].sampleOffset <= frame) handleMidi(ctx.midi[mi++]);
            int end = n;
            if (mi < ctx.numMidiEvents) end = mpc::clamp(ctx.midi[mi].sampleOffset, frame + 1, n);
            tr.ppq = ctx.transport.ppqPos + (double)frame * ppqPerFrame;
            for (int t = 0; t < kNumTracks; ++t)
                tracks_[t].process(tp_[t], active_[t] ? *active_[t] : samples_[mpc::clamp(tp_[t].sample, 0, kBuiltins - 1)],
                                   bend_, bpm, bpmValid, tr,
                                   outL + frame, outR + frame, end - frame);
            frame = end;
        }

        // master volume, smoothed; then a soft ceiling so four loud tracks cannot clip hard
        const float target = get(kVolume) <= kMixMinDb + 0.05f ? 0.0f : mpc::dsp::dbToGain(get(kVolume));
        const float step = (target - gain_) / (float)n;
        float masterPeak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            gain_ += step;
            outL[i] = mpc::dsp::softClip(outL[i] * gain_ * 0.7f);
            if (outR != outL) outR[i] = mpc::dsp::softClip(outR[i] * gain_ * 0.7f);
            masterPeak = std::fmax(masterPeak, std::fmax(std::fabs(outL[i]), std::fabs(outR[i])));
        }
        updateMeters(masterPeak, n);

        if (notifyAll_)
        {
            notifyAll_ = false;
            for (int i = 0; i < numParams(); ++i)
                if ((i < kPlay || i >= kPlay + kNumTracks) && !isMeter(i)) notifyLater(i);
        }
        flushNotifications(kNotifyPerBlock);
    }

    // SYNC plays the loop at whole-octave speeds only (x0.25 .. x2, either way), so it always
    // fits the bar: any other speed would drift against the beat.
    static float syncSpeed(float s)
    {
        const float a = std::exp2(std::round(std::log2(std::fmax(std::fabs(s), 0.25f))));
        return s < 0.0f ? -a : a;
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index >= kKnob0 && index < kTabEnd)
        {   // a knob reads like the setting it stands for; and the MPC only asks for the tab it shows
            const int t = (index - kKnob0) / kKnobs, k = (index - kKnob0) % kKnobs;
            if (t != 0) { out = ""; return true; }   // only the first 12 knobs are used: the screen's
            const int target = cellTarget(screenTrack(), mpc::clamp(getInt(kPage0), 0, kModules - 1), k);
            if (target < 0) { out = ""; return true; }
            const float tp = param(target).toPlain(plain);
            if (!formatValue(target, tp, out)) out = param(target).display(tp);
            return true;
        }
        for (int t = 0; t < kNumTracks; ++t)
        {
            // Grain Size: a tempo division in the rhythmic modes (all but Cloud and Shimmer)
            if (index == idx(t, pGSize) && grainRhythmic(getInt(idx(t, pGMode))))
            {
                out = mpc::dsp::divisionName(grainDivision(param(index).toNormalised(plain)));
                return true;
            }
            // Speed in SYNC: whole octaves (x0.25, x0.5, x1, x2, backwards too)
            if (index == idx(t, pSpeed) && getBool(idx(t, pSync)) && getInt(idx(t, pLoop)) < 3)
            {
                char b[16];
                const float v = syncSpeed(plain);
                std::snprintf(b, sizeof b, std::fabs(v) < 1.0f ? "%+.2fx" : "%+.0fx", v);
                out = b;
                return true;
            }
            // Length in SYNC: beat steps, shown as note values and bars
            if (index == idx(t, pLength) && getBool(idx(t, pSync)) && getInt(idx(t, pLoop)) < 3)
            {
                const sculpt::Sample* smp = active_[t] ? active_[t] : &samples_[mpc::clamp(getInt(idx(t, pSample)), 0, kBuiltins - 1)];
                if (smp && smp->size() >= 64)
                {
                    const double sb = sculpt::Track::sampleBeats(*smp);
                    const double k = std::round(mpc::clamp(plain / 100.0f, 0.0f, 1.0f) * std::log2(sb * 16.0));
                    const double beats = std::fmin(sb, std::pow(2.0, k) / 16.0);
                    char b[24];
                    if (beats >= 4.0) std::snprintf(b, sizeof b, beats >= 8.0 ? "%.0f bars" : "%.0f bar", beats / 4.0);
                    else std::snprintf(b, sizeof b, "1/%.0f", 4.0 / beats);
                    out = b;
                    return true;
                }
            }
            if (index == midx(t, mVolume) && plain <= kMixMinDb + 0.05f) { out = "-inf dB"; return true; }
            if (index == kVolume && plain <= kMixMinDb + 0.05f) { out = "-inf dB"; return true; }
            if (index == midx(t, mPan))
            {
                const int v = (int)std::lround(plain);
                out = v == 0 ? "C" : (v < 0 ? "L" + std::to_string(-v) : "R" + std::to_string(v));
                return true;
            }
            if (index == midx(t, mMeter)) { out = ""; return true; }
        }
        if (index == kMasterMeter || index == kPlayhead || index == kLoopEnd || index == kRotateAt) { out = ""; return true; }
        if (isDisplayIndex(index)) { out = ""; return true; }
        if (index >= kFolderRow0 && index < kFileRow0 + kListRows) { out = listRowText(index); return true; }
        if (index >= kWave0 && index < kWave0 + kWaveCols) { out = ""; return true; }
        if (viewOf(index) >= 0)   // a view value reads like its track's
            return formatValue(viewTarget(mpc::clamp(getInt(kEditTrack), 0, kNumTracks - 1), viewOf(index)), plain, out);
        if ((index >= kRootBase && index < kRootBase + kNumTracks))
        {   // MPC note names: 60 = C3, 24 = C0
            static const char* const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            const int nn = mpc::clamp((int)std::lround(plain), 0, 127);
            out = std::string(names[nn % 12]) + std::to_string(nn / 12 - 2) + "  (" + std::to_string(nn) + ")";
            return true;
        }
        if (browseText(index, plain, out)) return true;
        const mpc::Param& p = param(index);
        if (p.kind == mpc::Param::Kind::Bool || p.kind == mpc::Param::Kind::Choice) return false;
        const std::string& u = units_[(size_t)index];
        std::string v = p.display(plain);
        const bool signedRange = p.min < 0.0f && p.max > 0.0f && index != kVolume;
        if (signedRange && plain > 0.0f && v != "0" && v != "0.00") v = "+" + v;
        if (u == "x") { out = v + "x"; return true; }
        if (u == "%") { out = v + "%"; return true; }
        if (u == "Hz" && plain >= 1000.0f)
        {
            char b[32]; std::snprintf(b, sizeof b, "%.1f kHz", (double)plain / 1000.0); out = b; return true;
        }
        if (u == "ms" && plain >= 1000.0f)
        {
            char b[32]; std::snprintf(b, sizeof b, "%.2f s", (double)plain / 1000.0); out = b; return true;
        }
        out = u.empty() ? v : v + " " + u;
        return true;
    }

    // Diagnostics: count the parameter changes that come from the host (the screen, Q-Links,
    // automation) rather than from Sculpt itself. Trivial and safe from any thread.
    void parameterChanged(int index, float plainValue) override
    {
        if (std::this_thread::get_id() == audioThread_.load(std::memory_order_relaxed)) return;
        if (index < 0 || index >= (int)hostSets_.size()) return;
        hostSets_[(size_t)index].fetch_add(1, std::memory_order_relaxed);
        const uint32_t k = recentCount_.fetch_add(1, std::memory_order_relaxed);
        recent_[k % kRecent].index.store(index, std::memory_order_relaxed);
        recent_[k % kRecent].value.store(plainValue, std::memory_order_relaxed);
    }

    // Project state beyond the parameters: the file each track plays ("" = its built-in loop).
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        out.push_back('S'); out.push_back('C'); out.push_back('U'); out.push_back('1');
        for (int t = 0; t < kNumTracks; ++t)
        {
            const std::string p = loader_.path(t);
            const size_t n = std::min<size_t>(p.size(), 65535);
            out.push_back((uint8_t)(n & 255)); out.push_back((uint8_t)(n >> 8));
            out.insert(out.end(), p.begin(), p.begin() + (long)n);
        }
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        if (size < 4 || std::memcmp(data, "SCU1", 4) != 0) return;
        size_t pos = 4;
        for (int t = 0; t < kNumTracks && pos + 2 <= size; ++t)
        {
            const size_t n = (size_t)data[pos] | ((size_t)data[pos + 1] << 8);
            pos += 2;
            if (pos + n > size) break;
            loader_.requestPath(t, std::string(reinterpret_cast<const char*>(data + pos), n));
            pos += n;
        }
    }

private:
    static constexpr int kPlayNote = 1000;   // an id outside MIDI, so a Play key never clashes with a note
    static constexpr int kMaxText = 31;   // the host shows at most 31 characters of a value

    void addBrowseParams(int t)
    {
        using namespace mpc;
        const std::string tp = "t" + std::to_string(t + 1) + "_";
        const std::string np = "T" + std::to_string(t + 1) + " ";
        auto add = [&](const char* id, const char* name, Param p, bool automatable)
        {
            p.id = tp + id; p.name = np + name; p.automatable = automatable; addParam(p);
        };
        add("folder", "Folder", intParam("", "", 0, 4095, 0), true);       // index into the folders found
        add("file",   "File",   intParam("", "", 0, 4095, 0), true);       // index into that folder's WAVs
        // The screen keys: each tap flips the value and any flip counts as one press, so they work
        // whether or not the host supports momentary buttons.
        add("load",   "Load",   boolParam("", "", false), true);
        add("fold_prev", "Folder -", boolParam("", "", false), true);
        add("fold_next", "Folder +", boolParam("", "", false), true);
        add("file_prev", "File -",   boolParam("", "", false), true);
        add("file_next", "File +",   boolParam("", "", false), true);
        add("has_file",  "Has File", boolParam("", "", false), false);     // set by the plugin: a file is playing
    }

    static bool isMeter(int i)
    {
        return i == kMasterMeter || i == kPlayhead || i == kLoopEnd || i == kRotateAt || isDisplayIndex(i) || (i >= kWave0 && i < kWave0 + kWaveCols) || (i >= kMixBase && i < kMasterMeter && (i - kMixBase) % mCount == mMeter);
    }

    void setAndNotify(int index, float plain) { set(index, plain); notifyLater(index); }

    // Telling the MPC that a value changed is a call into the host, and a track switch changes
    // hundreds of values at once (the view, the waveforms, the meters): sending them all in one
    // block can stall the audio and click. So on the audio thread they queue (each index once)
    // and go out at most kNotifyPerBlock per block, oldest first. Anywhere else they go at once.
    static constexpr int kNotifyPerBlock = 24;
    void notifyLater(int index)
    {
        if (nq_.empty() || std::this_thread::get_id() != audioThread_.load(std::memory_order_relaxed))
        {
            notifyHostParameterChanged(index);
            return;
        }
        if (index < 0 || index >= (int)nqFlag_.size() || nqFlag_[(size_t)index]) return;
        nqFlag_[(size_t)index] = 1;
        nq_[(nqHead_ + nqCount_) % nq_.size()] = index;
        ++nqCount_;
    }
    void flushNotifications(int budget)
    {
        while (nqCount_ > 0 && budget-- > 0)
        {
            const int index = nq_[nqHead_];
            nqHead_ = (nqHead_ + 1) % nq_.size(); --nqCount_;
            nqFlag_[(size_t)index] = 0;
            notifyHostParameterChanged(index);
        }
    }

    // Tell the screen a track parameter's text changed, and its view twin too when that track is shown.
    void notifyTrackParam(int t, int index)
    {
        notifyLater(index);
        if (t != getInt(kEditTrack)) return;
        for (int v = 0; v < kNumView; ++v)
            if (viewTarget(t, v) == index) { notifyLater(viewParam(v)); break; }
    }

    void addViewParams()
    {
        mpc::Param e = mpc::choiceParam("edit_track", "Edit Track", {"1", "2", "3", "4"}, 0);
        addParam(e);
        // The screen shows these names in its boxes, so they match the box captions exactly.
        static const char* const kScreenNames[kPerTrack] = {
            "Speed", "Rotate", "Length", "Pitch", "Level", "Fade", "Loop", "Sample",
            "Pitch", "Density", "Size", "Spread", "Reverse", "Feedback", "Mode", "Wet",
            "Cutoff", "Reso", "Decay", "Pitch", "Slope", "Tone", "Scale", "Wet",
            "Drive", "Comp", "Crush", "Tilt", "Noise", "N Decay", "N Tone", "Wet",
            "Send", "Time", "Send", "Size", "Feedback", "Spread", "Tone", "Decay",   // under DELAY / REVERB headings
            "Source On", "Grain On", "Filter On", "Color On", "Space On", "Sync", "Lock", "Freeze"};
        auto addView = [&](int v)
        {
            mpc::Param p = param(viewTarget(0, v));   // same kind, range and default as track 1's
            p.id = "view_" + p.id.substr(3);           // "t1_cutoff" -> "view_cutoff"
            p.name = v < kPerTrack ? kScreenNames[v] : p.name.substr(3);   // "T1 Root" -> "Root"
            addParam(p);
        };
        for (int v = 0; v < kPerTrack + bCount; ++v) addView(v);
        // Root, added after the first view block: the note that plays each track's loop at its own
        // pitch. 24 (C0) is pad A01 in the MPC's usual pad layout.
        for (int t = 0; t < kNumTracks; ++t)
            addParam(mpc::intParam(("t" + std::to_string(t + 1) + "_root").c_str(), ("T" + std::to_string(t + 1) + " Root").c_str(), 0, 127, 24));
        addView(kPerTrack + bCount);   // Root
    }

    void addMixParams()
    {
        using namespace mpc;
        for (int t = 0; t < kNumTracks; ++t)
        {
            const std::string tp = "t" + std::to_string(t + 1) + "_", np = "T" + std::to_string(t + 1) + " ";
            addParam(floatParam((tp + "vol").c_str(), (np + "Volume").c_str(), kMixMinDb, kMixMaxDb, 0.0f, "dB", 1));
            addParam(floatParam((tp + "pan").c_str(), (np + "Pan").c_str(), -100.0f, 100.0f, 0.0f, "", 0));
            addParam(boolParam((tp + "mute").c_str(), (np + "Mute").c_str(), false));
            addParam(boolParam((tp + "solo").c_str(), (np + "Solo").c_str(), false));
            Param m = floatParam((tp + "meter").c_str(), (np + "Meter").c_str(), 0.0f, 1.0f, 0.0f, "", 2);
            m.automatable = false;   // written by the plugin for the MIX screen
            addParam(m);
        }
        Param m = floatParam("master_meter", "Master Meter", 0.0f, 1.0f, 0.0f, "", 2);
        m.automatable = false;
        addParam(m);
        Param ph = floatParam("playhead", "Playhead", 0.0f, 1.0f, 0.0f, "", 3);   // 0 = nothing playing
        ph.automatable = false;
        addParam(ph);
        addParam(boolParam("browser_open", "Browser", false));
        for (int k = 0; k < kListRows; ++k)
            addParam(floatParam(("folder_row" + std::to_string(k + 1)).c_str(), ("Folder Row " + std::to_string(k + 1)).c_str(), 0.0f, 1.0f, 0.0f, "", 0));
        for (int k = 0; k < kListRows; ++k)
            addParam(floatParam(("file_row" + std::to_string(k + 1)).c_str(), ("File Row " + std::to_string(k + 1)).c_str(), 0.0f, 1.0f, 0.0f, "", 0));
        for (int c = 0; c < kWaveCols; ++c)
        {   // the waveform columns: written by the plugin, not for automation
            Param w = floatParam(("wave" + std::to_string(c + 1)).c_str(), ("Wave " + std::to_string(c + 1)).c_str(), 0.0f, 1.0f, 0.0f, "", 2);
            w.automatable = false;
            addParam(w);
        }
        // Start, added after the waveform: where each track's loop begins in the sample, and the
        // view's twin of it (the screen's START box)
        for (int t = 0; t < kNumTracks; ++t)
            addParam(floatParam(("t" + std::to_string(t + 1) + "_start").c_str(), ("T" + std::to_string(t + 1) + " Start").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
        {
            Param v = param(kStartBase);
            v.id = "view_start"; v.name = "Start"; addParam(v);
        }
        Param le = floatParam("loop_end", "Loop End", 0.0f, 1.0f, 1.0f, "", 3);
        le.automatable = false;
        addParam(le);
        Param ra = floatParam("rotate_at", "Rotate At", 0.0f, 1.0f, 0.0f, "", 3);
        ra.automatable = false;
        addParam(ra);
        // the stereo chorus, per track, then the view's twins (the screen's CHORUS row)
        auto chorusParams = [&](const std::string& id, const std::string& nm, std::vector<Param>& out)
        {
            out.push_back(floatParam((id + "chorus").c_str(), (nm + "Chorus").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
            out.push_back(floatParam((id + "ch_rate").c_str(), (nm + "Ch Rate").c_str(), 0.05f, 5.0f, 0.6f, "Hz", 2, 0.4f));
            out.push_back(floatParam((id + "ch_depth").c_str(), (nm + "Ch Depth").c_str(), 0.0f, 100.0f, 50.0f, "%", 0));
            out.push_back(floatParam((id + "ch_width").c_str(), (nm + "Ch Width").c_str(), 0.0f, 100.0f, 100.0f, "%", 0));
        };
        for (int t = 0; t < kNumTracks; ++t)
        {
            std::vector<Param> ps;
            chorusParams("t" + std::to_string(t + 1) + "_", "T" + std::to_string(t + 1) + " ", ps);
            for (Param& q : ps) addParam(q);
        }
        {
            std::vector<Param> ps;
            chorusParams("view_", "", ps);
            const char* caption[cCount] = {"Mix", "Rate", "Depth", "Width"};   // the boxes' captions
            for (int c = 0; c < cCount; ++c) { ps[(size_t)c].name = caption[c]; addParam(ps[(size_t)c]); }
        }
        // reverb Mix per track (0 = dry only, 50 = both at full level, 100 = reverb only), then the view's
        for (int t = 0; t < kNumTracks; ++t)
            addParam(floatParam(("t" + std::to_string(t + 1) + "_rev_mix").c_str(), ("T" + std::to_string(t + 1) + " Rev Mix").c_str(),
                                0.0f, 100.0f, 20.0f, "%", 0));
        {
            Param q = floatParam("view_rev_mix", "Mix", 0.0f, 100.0f, 20.0f, "%", 0);
            addParam(q);
        }
        // reverb Type per track, then the view's (the screen's TYPE box)
        const std::vector<std::string> types = {"Hall", "Plate", "Bloom", "Cloud", "Echoes", "Canyon", "Ripple", "Vast"};
        for (int t = 0; t < kNumTracks; ++t)
            addParam(mpc::choiceParam(("t" + std::to_string(t + 1) + "_rev_type").c_str(), ("T" + std::to_string(t + 1) + " Rev Type").c_str(), types, 0));
        {
            Param q = mpc::choiceParam("view_rev_type", "Type", types, 0);
            addParam(q);
        }
        // the Grain screen (written by the plugin; not for automation)
        auto display = [&](const std::string& pid, const std::string& pname, float def)
        {
            Param q = floatParam(pid.c_str(), pname.c_str(), 0.0f, 1.0f, def, "", 3);
            q.automatable = false;
            addParam(q);
        };
        for (int c = 0; c < kGrainCols; ++c) display("grain_col" + std::to_string(c + 1), "Grain Col " + std::to_string(c + 1), 0.0f);
        for (int c = 0; c < kGrainCols; ++c) display("grain_hot" + std::to_string(c + 1), "Grain Zone " + std::to_string(c + 1), 0.0f);
        for (int c = 0; c < kGrainCols; ++c) display("grain_hit" + std::to_string(c + 1), "Grain Hit " + std::to_string(c + 1), 0.0f);
        for (int c = 0; c < kPixCols; ++c) display("grain_pix" + std::to_string(c + 1), "Grain Pixels " + std::to_string(c + 1), 0.0f);
        for (int b = 0; b < kSpecBands; ++b) display("spec_in" + std::to_string(b + 1), "Filter In " + std::to_string(b + 1), 0.0f);
        for (int b = 0; b < kSpecBands; ++b) display("spec_out" + std::to_string(b + 1), "Filter Out " + std::to_string(b + 1), 0.0f);
        // the track tabs: which module page each shows, then each track's 12 knobs
        for (int t = 0; t < kNumTracks; ++t)
            addParam(mpc::choiceParam(("t" + std::to_string(t + 1) + "_page").c_str(), ("T" + std::to_string(t + 1) + " Page").c_str(),
                                      {"Source", "Grain", "Filter", "Color", "Space"}, 0));
        for (int t = 0; t < kNumTracks; ++t)
            for (int k = 0; k < kKnobs; ++k)
            {
                Param q = floatParam(("t" + std::to_string(t + 1) + "_knob" + std::to_string(k + 1)).c_str(),
                                     ("T" + std::to_string(t + 1) + " Knob " + std::to_string(k + 1)).c_str(), 0.0f, 1.0f, 0.0f, "", 3);
                addParam(q);
            }
    }

    // The mix gains for this block: volume, pan (balance), mute, and solo across the tracks.
    void setupMix()
    {
        bool anySolo = false;
        for (int t = 0; t < kNumTracks; ++t) anySolo = anySolo || getBool(midx(t, mSolo));
        for (int t = 0; t < kNumTracks; ++t)
        {
            const float db = get(midx(t, mVolume));
            float g = db <= kMixMinDb + 0.05f ? 0.0f : mpc::dsp::dbToGain(db);
            if (getBool(midx(t, mMute)) || (anySolo && !getBool(midx(t, mSolo)))) g = 0.0f;
            const float pan = mpc::clamp(get(midx(t, mPan)) * 0.01f, -1.0f, 1.0f);
            tp_[t].mixL = g * (pan > 0.0f ? 1.0f - pan : 1.0f);
            tp_[t].mixR = g * (pan < 0.0f ? 1.0f + pan : 1.0f);
        }
    }

    // Meters: a fast-rise, slow-fall level per track and for the master, on the faders' dB scale,
    // sent to the screen at most about 30 times a second.
    void updateMeters(float masterPeak, int n)
    {
        const float fall = std::exp(-(float)n / (0.3f * (float)sampleRate()));
        meterTimer_ += n;
        const bool send = meterTimer_ >= (int)(sampleRate() / 30.0);
        if (send) meterTimer_ = 0;
        for (int k = 0; k <= kNumTracks; ++k)
        {
            const float pk = k < kNumTracks ? tracks_[k].takePeak() : masterPeak;
            meterEnv_[k] = std::fmax(pk, meterEnv_[k] * fall);
            if (!send) continue;
            const float db = meterEnv_[k] > 1e-6f ? 20.0f * std::log10(meterEnv_[k]) : -120.0f;
            const float norm = mpc::clamp((db - kMixMinDb) / (kMixMaxDb - kMixMinDb), 0.0f, 1.0f);
            const int index = k < kNumTracks ? midx(k, mMeter) : kMasterMeter;
            if (std::fabs(norm - getNormalised(index)) >= 0.004f)
            {
                setNormalised(index, norm);
                notifyLater(index);
            }
        }
        if (send)
        {   // The picked track's pictures, only while its page is on screen.
            ++grainTick_;
            const int et = screenTrack(), page = mpc::clamp(getInt(kPage0), 0, kModules - 1);
            if (page == 0)
            {   // the playhead line on the Source waveform: 0 hides it, otherwise 0.001..1
                const double pos = tracks_[et].playPosition();
                const float v = pos < 0.0 ? 0.0f : 0.001f + 0.999f * (float)pos;
                if (v != getNormalised(kPlayhead) && (v == 0.0f || std::fabs(v - getNormalised(kPlayhead)) >= 0.0005f))
                {
                    setNormalised(kPlayhead, v);
                    notifyLater(kPlayhead);
                }
                publishMarkers(et);
            }
            const int pixKey = et * kModules + page;
            if (page == 1 && (grainTick_ % 3 == 0 || pixPage_[0] != pixKey)) publishPixels(et);   // ~10 a second
            pixPage_[0] = pixKey;
            if (page == 2) publishSpectrum(et, grainTick_ & 1);   // in and out alternate: each ~15 a second
        }
    }

    // The Grain screen for the edited track: what is recorded, where grains are picked from, and
    // where each grain is reading. Nothing moves while the track's Grain is off.
    void publishGrain(bool columns)
    {
        const int et = screenTrack();
        const bool on = tp_[et].grainOn;
        auto put = [&](int index, float v, float step)
        {
            const float cur = getNormalised(index);
            if (std::fabs(v - cur) >= step || ((v == 0.0f) != (cur == 0.0f)))
            {
                setNormalised(index, v);
                notifyLater(index);
            }
        };
        float lo = 0.0f, hi = 1.0f;
        tracks_[et].grainZone(tp_[et], lo, hi);
        if (columns || et != grainEdit_ || lo != grainLo_ || hi != grainHi_)
        {   // the recording, and the same bars lit inside the zone
            float cols[kGrainCols];
            if (on) tracks_[et].grainColumns(cols); else std::fill(cols, cols + kGrainCols, 0.0f);
            for (int c = 0; c < kGrainCols; ++c)
            {
                const float mid = (c + 0.5f) / kGrainCols;
                put(kGrainCol0 + c, cols[c], 0.01f);
                // inside the zone: never 0 (0 is "not lit"), so even a silent spot shows its line
                put(kGrainHot0 + c, on && mid >= lo && mid <= hi ? 0.04f + 0.96f * cols[c] : 0.0f, 0.01f);
            }
            std::copy(cols, cols + kGrainCols, lastCols_);
            grainLo_ = lo; grainHi_ = hi;
        }
        // a full bar in each column a grain is playing in
        float dots[sculpt::kMaxGrains];
        if (on) tracks_[et].grainMarks(dots); else std::fill(dots, dots + sculpt::kMaxGrains, 0.0f);
        // A ripple per spot: a grain landing starts it (1), and it spreads and fades as that falls
        // to 0, over longer for longer grains. Sent as one picture number: 0 = none, else
        // 1 + height x 10 + spread (height 0..7 from the landscape there, spread 0..9).
        const float tau = std::fmax(0.12f, tp_[et].gSizeMs * 0.0012f);          // seconds
        const float fall = std::exp(-1.0f / (30.0f * tau));                     // per screen update
        for (int c = 0; c < kGrainCols; ++c) hitLevel_[c] = hitLevel_[c] * fall < 0.05f ? 0.0f : hitLevel_[c] * fall;
        for (int k = 0; k < sculpt::kMaxGrains; ++k)
            if (dots[k] > 0.0f) hitLevel_[mpc::clamp((int)(dots[k] * kGrainCols), 0, kGrainCols - 1)] = 1.0f;
        for (int c = 0; c < kGrainCols; ++c)
        {
            float v = 0.0f;
            if (hitLevel_[c] > 0.0f)
            {
                const int spread = mpc::clamp((int)((1.0f - hitLevel_[c]) * 10.0f), 0, 9);
                const int height = mpc::clamp((int)std::lround(lastCols_[c] * 7.0f), 0, 7);
                v = (float)(1 + height * 10 + spread) / (float)kRippleFrames1;
            }
            put(kGrainHit0 + c, v, 0.4f / kRippleFrames1);
        }
        grainEdit_ = et;
    }

    // Track t's Grain page: the sample's shape in pixels, solid where its grains pick from right
    // now, dissolving to either side (a few columns per step); all solid when nothing is picked.
    void publishPixels(int t)
    {
        const float step = 0.4f / (kPixHeights * kPixDissolve);
        const sculpt::Sample* src = active_[t] ? active_[t] : &samples_[mpc::clamp(tp_[t].sample, 0, kBuiltins - 1)];
        unsigned char marks[kPixCols];
        const int marked = tp_[t].grainOn ? tracks_[t].grainSampleMarks(tp_[t], marks, kPixCols) : 0;
        int dist[kPixCols];
        for (int c = 0; c < kPixCols; ++c) dist[c] = marked && marks[c] ? 0 : 1 << 20;
        for (int c = 1; c < kPixCols; ++c) dist[c] = std::min(dist[c], dist[c - 1] + 1);
        for (int c = kPixCols - 2; c >= 0; --c) dist[c] = std::min(dist[c], dist[c + 1] + 1);
        for (int c = 0; c < kPixCols; ++c)
        {
            const int h = mpc::clamp((int)std::lround(src->peaks[c] * (kPixHeights - 1)), 0, kPixHeights - 1);
            const int d = marked == 0 ? 0 : mpc::clamp((dist[c] + 3) / 4, 0, kPixDissolve - 1);
            const float v = (float)(h * kPixDissolve + d) / (float)(kPixHeights * kPixDissolve - 1);
            const int index = kGrainPix0 + c;
            if (std::fabs(v - getNormalised(index)) >= step)
            {
                setNormalised(index, v);
                notifyLater(index);
            }
        }
    }

    // The Filter screen: what the edited track sounds like going into the filter and coming out,
    // in 64 bands. Each band jumps up at once and falls back gently, like a hardware analyser.
    void publishSpectrum(int et, int only)
    {
        constexpr int N = sculpt::Track::kSpecN;
        tracks_[et].spectrumInput(specA_.data(), specB_.data());
        const float sr = (float)sampleRate(), binHz = sr / N;
        for (int which = only; which == only; ++which)   // one analysis per call keeps each block light
        {
            const float* in = which == 0 ? specA_.data() : specB_.data();
            for (int i = 0; i < N; ++i) fft_[(size_t)i] = std::complex<float>(in[i] * window_[(size_t)i], 0.0f);
            fftInPlace(fft_);
            for (int b = 0; b < kSpecBands; ++b)
            {
                const float fLo = 20.0f * std::pow(1000.0f, (float)b / kSpecBands);
                const float fHi = 20.0f * std::pow(1000.0f, (float)(b + 1) / kSpecBands);
                int i0 = (int)std::ceil(fLo / binHz), i1 = (int)std::floor(fHi / binHz);
                if (i1 < i0) i0 = i1 = (int)std::lround(std::sqrt(fLo * fHi) / binHz);   // narrower than one bin
                i0 = mpc::clamp(i0, 1, N / 2 - 1); i1 = mpc::clamp(i1, i0, N / 2 - 1);
                float peak = 0.0f;
                for (int i = i0; i <= i1; ++i) peak = std::fmax(peak, std::abs(fft_[(size_t)i]));
                const float amp = 2.0f * peak / windowSum_;
                // +3 dB per octave above 1 kHz (less below), so music sits level instead of sloping down
                const float tilt = 3.0f * std::log2(std::sqrt(fLo * fHi) / 1000.0f);
                const float db = 20.0f * std::log10(std::fmax(1e-7f, amp)) + tilt;
                const float v = mpc::clamp((db + 66.0f) / 42.0f, 0.0f, 1.0f);   // -66 dB = empty, -24 dB = full height
                float& held = specHeld_[et][which][b];
                held = std::fmax(v, held - 0.045f);
                const int index = (which == 0 ? kSpecIn0 : kSpecOut0) + b;
                if (std::fabs(held - getNormalised(index)) >= 0.012f || (held == 0.0f) != (getNormalised(index) == 0.0f))
                {
                    setNormalised(index, held);
                    notifyLater(index);
                }
            }
        }
    }

    static void fftInPlace(std::vector<std::complex<float>>& a)
    {   // iterative radix-2; the size is a power of two
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const float ang = -2.0f * 3.14159265358979f / (float)len;
            const std::complex<float> wl(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<float> w(1.0f, 0.0f);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v; a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    // The loop's end and the Rotate point on the waveform, for the edited track (as the engine plays them)
    void publishMarkers(int et)
    {
        const float start = mpc::clamp(get(kStartBase + et) * 0.01f, 0.0f, 1.0f);
        const float end = std::fmin(1.0f, start + get(idx(et, pLength)) * 0.01f);
        const float rot = get(idx(et, pRotate)) * 0.01f;
        const float at = rot <= 0.0f ? 0.0f : std::fmax(0.001f, start + rot * (end - start));
        const float vals[2] = {end, at};
        const int ids[2] = {kLoopEnd, kRotateAt};
        for (int k = 0; k < 2; ++k)
            if (std::fabs(vals[k] - getNormalised(ids[k])) >= 0.0005f || (vals[k] == 0.0f) != (getNormalised(ids[k]) == 0.0f))
            {
                setNormalised(ids[k], vals[k]);
                notifyLater(ids[k]);
            }
    }

    // The screen, its pictures, the Q-Links, the browser list and channel-1 pads all follow the
    // track the track keys pick (Edit Track).
    int screenTrack() const { return mpc::clamp(getInt(kEditTrack), 0, kNumTracks - 1); }
    int listTrack() const { return screenTrack(); }

    // The 12 Q-Link knobs (track 1's set) stand for the cells of the page on screen, for the picked
    // track. When the page or the track changes they take the new cells' values; after that a knob
    // moving moves its setting, and a setting moving (the view, automation, presets) moves its knob.
    // The positions (normalised) a stepped setting can take, in order; 0 for a smooth one.
    int stepPositions(int t, int target, float* pos) const
    {
        const mpc::Param& pr = param(target);
        const bool grid = getBool(idx(t, pSync)) && getInt(idx(t, pLoop)) < 3;
        if (target == idx(t, pLength) && grid)
        {   // SYNC Length: 1/16 beat .. the whole sample, in doublings
            const sculpt::Sample* smp = active_[t] ? active_[t] : &samples_[mpc::clamp(getInt(idx(t, pSample)), 0, kBuiltins - 1)];
            const int K = (int)std::lround(std::log2(sculpt::Track::sampleBeats(*smp) * 16.0));
            for (int k = 0; k <= K; ++k) pos[k] = pr.toNormalised(std::fmax(1.0f, 100.0f * (float)k / (float)K));
            return K + 1;
        }
        if (target == idx(t, pSpeed) && grid)
        {
            static const float sp[8] = {-2.0f, -1.0f, -0.5f, -0.25f, 0.25f, 0.5f, 1.0f, 2.0f};
            for (int k = 0; k < 8; ++k) pos[k] = pr.toNormalised(sp[k]);
            return 8;
        }
        if (target == idx(t, pGSize) && grainRhythmic(getInt(idx(t, pGMode))))
        {
            const int n = mpc::dsp::kNumDivisions;
            for (int k = 0; k < n; ++k) pos[k] = (float)k / (float)(n - 1);
            return n;
        }
        int n = 0;
        if (pr.kind == mpc::Param::Kind::Choice) n = (int)pr.choices.size();
        else if (pr.kind == mpc::Param::Kind::Bool) n = 2;
        else if (pr.kind == mpc::Param::Kind::Int) n = (int)std::lround(pr.max - pr.min) + 1;
        if (n < 2 || n > 128) return 0;
        for (int k = 0; k < n; ++k) pos[k] = (float)k / (float)(n - 1);
        return n;
    }

    void syncKnobs()
    {
        const int et = screenTrack(), page = mpc::clamp(getInt(kPage0), 0, kModules - 1);
        const int key = et * kModules + page;
        const bool newPage = key != lastPage_[0];
        lastPage_[0] = key;
        for (int k = 0; k < kKnobs; ++k)
        {
            const int knob = knobIdx(0, k), target = cellTarget(et, page, k);
            const float kv = getNormalised(knob), tv = target >= 0 ? getNormalised(target) : 0.0f;
            if (target < 0) { lastKnob_[0][k] = kv; continue; }   // nothing behind this knob on this page
            if (newPage)
            {
                setNormalised(knob, tv); notifyLater(knob);
                lastKnob_[0][k] = lastKnobTarget_[0][k] = tv;
            }
            else if (kv != lastKnob_[0][k])
            {   // turned on the screen, the data wheel or a Q-Link: move the setting. A setting with
                // steps (Loop, Mode, Slope, Scale, Length in beats...) moves at least one step for
                // any turn, so each click of the wheel changes it.
                float pos[128];
                const int np = stepPositions(et, target, pos);
                float nv = kv;
                if (np >= 2)
                {
                    int cur = 0, near = 0;
                    for (int j = 1; j < np; ++j)
                    {
                        if (std::fabs(pos[j] - tv) < std::fabs(pos[cur] - tv)) cur = j;
                        if (std::fabs(pos[j] - kv) < std::fabs(pos[near] - kv)) near = j;
                    }
                    if (near == cur) near = mpc::clamp(cur + (kv > lastKnob_[0][k] ? 1 : -1), 0, np - 1);
                    nv = pos[near];
                    setNormalised(knob, nv); notifyLater(knob);
                }
                setNormalised(target, nv); notifyLater(target);
                lastKnob_[0][k] = lastKnobTarget_[0][k] = nv;
            }
            else if (tv != lastKnobTarget_[0][k])
            {
                setNormalised(knob, tv); notifyLater(knob);
                lastKnob_[0][k] = lastKnobTarget_[0][k] = tv;
            }
        }
    }

    // Copy between the view and the track being edited. Audio thread, once per block.
    void syncView()
    {
        const int et = screenTrack();
        if (et != lastEdit_)
        {   // another track picked: the view shows it
            lastEdit_ = et;
            for (int v = 0; v < kNumView; ++v)
            {
                const float tn = getNormalised(viewTarget(et, v));
                setNormalised(viewParam(v), tn);
                notifyLater(viewParam(v));
                lastView_[v] = tn; lastTarget_[v] = tn;
            }
            return;
        }
        for (int v = 0; v < kNumView; ++v)
        {
            const int target = viewTarget(et, v);
            const float vn = getNormalised(viewParam(v)), tn = getNormalised(target);
            if (vn != lastView_[v])
            {   // pressed or turned on the screen (ON / toggle / browser): change the track
                setNormalised(target, vn);
                lastView_[v] = vn; lastTarget_[v] = vn;
            }
            else if (tn != lastTarget_[v])
            {   // the track changed some other way (a knob, automation, a preset, the browser): show it
                setNormalised(viewParam(v), tn);
                notifyLater(viewParam(v));
                lastView_[v] = tn; lastTarget_[v] = tn;
            }
        }
    }



    // Browser keys, loads finishing, and going back to the built-in loop. Audio thread, once per block.
    // The text of a list row: the folder or file that many places from the current choice.
    std::string listRowText(int index)
    {
        const sculpt::Library* lib = scanner_.library();
        const bool fileRow = index >= kFileRow0;
        const int k = index - (fileRow ? kFileRow0 : kFolderRow0);
        if (!lib) return k == kListMid && !fileRow ? "Scanning drives..." : "";
        if (lib->folders.empty()) return k == kListMid && !fileRow ? "No WAV files found" : "";
        const int t = listTrack();
        const int nFolders = (int)lib->folders.size();
        const int fo = mpc::clamp(getInt(bidx(t, bFolder)), 0, nFolders - 1);
        if (!fileRow)
        {
            const int i = fo + k - kListMid;
            if (i < 0 || i >= nFolders) return "";
            return fitTail(lib->folders[(size_t)i].name);
        }
        const sculpt::Folder& f = lib->folders[(size_t)fo];
        const int i = mpc::clamp(getInt(bidx(t, bFile)), 0, (int)f.files.size() - 1) + k - kListMid;
        if (i < 0 || i >= (int)f.files.size()) return "";
        std::string name = f.files[(size_t)i];
        if (name.size() > 4) name.resize(name.size() - 4);
        return fit(name);
    }

    // Taps on the list rows. Only the highlighted (middle) file row acts: it loads that file. The
    // other rows are for reading; scrolling is one item at a time with the up/down keys (the
    // Folder -/+ and File -/+ settings), the Q-Links or the data wheel.
    void listTaps(const sculpt::Library* lib)
    {
        bool tapped[2 * kListRows];
        for (int r = 0; r < 2 * kListRows; ++r)
        {
            const bool v = getBool(kFolderRow0 + r);
            tapped[r] = v != rowHeld_[r];
            rowHeld_[r] = v;
        }
        if (!lib || lib->folders.empty() || !tapped[kListRows + kListMid]) return;
        const int t = listTrack();
        const int fo = mpc::clamp(getInt(bidx(t, bFolder)), 0, (int)lib->folders.size() - 1);
        const int nFiles = (int)lib->folders[(size_t)fo].files.size();
        loader_.requestIndex(t, fo, mpc::clamp(getInt(bidx(t, bFile)), 0, nFiles - 1));
    }

    // Redraw the list rows whenever what they show could have changed.
    void refreshList(const sculpt::Library* lib)
    {
        const int t = listTrack();
        const long sig = (long)t * 1000003L + (long)getInt(bidx(t, bFolder)) * 4099L + (long)getInt(bidx(t, bFile))
                       + (lib ? 7L : 0L);
        if (sig == listSig_) return;
        listSig_ = sig;
        for (int r = 0; r < 2 * kListRows; ++r) notifyLater(kFolderRow0 + r);
    }

    void browse(int n)
    {
        const sculpt::Library* lib = scanner_.library();
        if (lib && !libraryAnnounced_)
        {   // the scan finished: folder and file names can now be shown
            libraryAnnounced_ = true;
            for (int t = 0; t < kNumTracks; ++t) locate(t, lib, soundPath(t));
            for (int t = 0; t < kNumTracks; ++t) { notifyTrackParam(t, bidx(t, bFolder)); notifyTrackParam(t, bidx(t, bFile)); }
        }
        listTaps(lib);
        for (int t = 0; t < kNumTracks; ++t)
        {
            // a press is any change of the key's value (on -> off counts as much as off -> on)
            auto edge = [&](int b) { const bool v = getBool(bidx(t, b)); const bool r = v != keyHeld_[t][b]; keyHeld_[t][b] = v; return r; };
            const bool load = edge(bLoad), fPrev = edge(bFolderPrev), fNext = edge(bFolderNext);
            const bool iPrev = edge(bFilePrev), iNext = edge(bFileNext);

            if (lib && !lib->folders.empty())
            {
                // The data wheel and Q-Links move a setting by a slice of its whole range (dozens of
                // items here). Any such jump counts as one step in its direction, so the list moves
                // one item per click.
                auto oneStep = [&](int index, int last, int count)
                {
                    const int raw = getInt(index);
                    if (raw == last) return mpc::clamp(raw, 0, count - 1);
                    const int v = mpc::clamp(mpc::clamp(last, 0, count - 1) + (raw > last ? 1 : -1), 0, count - 1);
                    if (v != raw) setAndNotify(index, (float)v);
                    return v;
                };
                const int nFolders = (int)lib->folders.size();
                const int prevFo = lastFolder_[t], prevFi = lastFile_[t];
                int fo = oneStep(bidx(t, bFolder), lastFolder_[t], nFolders);
                if (fPrev || fNext)
                {
                    fo = (fo + (fNext ? 1 : -1) + nFolders) % nFolders;
                    setAndNotify(bidx(t, bFolder), (float)fo);
                }
                if (fo != lastFolder_[t])
                {   // a new folder starts at its first file
                    lastFolder_[t] = fo;
                    setAndNotify(bidx(t, bFile), 0.0f);
                    lastFile_[t] = 0;
                }
                const int nFiles = (int)lib->folders[(size_t)fo].files.size();
                int fi = oneStep(bidx(t, bFile), lastFile_[t], nFiles);
                if (iPrev || iNext)
                {
                    fi = (fi + (iNext ? 1 : -1) + nFiles) % nFiles;
                    setAndNotify(bidx(t, bFile), (float)fi);
                }
                lastFile_[t] = fi;
                // Whatever is highlighted loads by itself, once it has rested for a moment, so a
                // quick scroll through a long list does not load every file on the way.
                if (fo != prevFo || fi != prevFi) loadDue_[t] = (int)(0.15 * sampleRate());
                if (loadDue_[t] > 0 && (loadDue_[t] -= n) <= 0) { loadDue_[t] = 0; loader_.requestIndex(t, fo, fi); }
                if (load) { loadDue_[t] = 0; loader_.requestIndex(t, fo, fi); }
            }

            // choosing a built-in loop in SAMPLE drops the file
            if (tp_[t].sample != lastSample_[t])
            {
                lastSample_[t] = tp_[t].sample;
                if (active_[t]) { loader_.release(t, active_[t]); loader_.requestClear(t); setAndNotify(bidx(t, bHasFile), 0.0f); }
                locate(t, lib, builtinPath(t));
            }
            if (loader_.takeDrop(t) && active_[t])
            {
                loader_.release(t, active_[t]);
                setAndNotify(bidx(t, bHasFile), 0.0f);
            }
            if (loader_.swap(t, active_[t]))
            {   // a file arrived (picked here, or reloaded with a project): show it in the browser row
                setAndNotify(bidx(t, bHasFile), 1.0f);
                locate(t, lib, loader_.path(t));
            }
            if (loader_.takeDirty(t)) notifyTrackParam(t, idx(t, pSample));
        }
        refreshList(lib);
        publishWave();
    }

    // What track t plays: its file, or else its built-in loop in Sculpt's Content folder.
    std::string builtinPath(int t) const { return mpc::contentPath(kSampleFiles[mpc::clamp(getInt(idx(t, pSample)), 0, kBuiltins - 1)]); }
    std::string soundPath(int t)
    {
        const std::string p = active_[t] ? loader_.path(t) : std::string();
        return p.empty() ? builtinPath(t) : p;
    }

    // Point track t's Folder and File at `path`, if the drive scan found it, without reloading it.
    // Rare (a load, a SAMPLE change, the end of the scan), so the string work is fine here.
    void locate(int t, const sculpt::Library* lib, const std::string& path)
    {
        if (!lib || path.empty()) return;
        const size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) return;
        const std::string dir = path.substr(0, slash), file = path.substr(slash + 1);
        for (size_t fo = 0; fo < lib->folders.size(); ++fo)
        {
            const sculpt::Folder& f = lib->folders[fo];
            if (f.path != dir) continue;
            for (size_t fi = 0; fi < f.files.size(); ++fi)
            {
                if (f.files[fi] != file) continue;
                lastFolder_[t] = (int)fo; lastFile_[t] = (int)fi; loadDue_[t] = 0;
                if (getInt(bidx(t, bFolder)) != (int)fo) setAndNotify(bidx(t, bFolder), (float)fo);
                if (getInt(bidx(t, bFile)) != (int)fi) setAndNotify(bidx(t, bFile), (float)fi);
                notifyTrackParam(t, bidx(t, bFolder)); notifyTrackParam(t, bidx(t, bFile));
                return;
            }
        }
    }

    // The screen's waveform follows the edited track's sound: rewrite the 128 columns whenever that
    // sound changes (a file loaded, a built-in loop picked) or another track is picked.
    void publishWave()
    {   // the picked track's waveform, rewritten when its sound changes or another track is picked
        const int t = screenTrack();
        const sculpt::Sample* src = active_[t] ? active_[t] : &samples_[mpc::clamp(tp_[t].sample, 0, kBuiltins - 1)];
        const bool fresh = src->id != waveIds_[0];
        waveIds_[0] = src->id;
        for (int c = 0; c < kWaveCols; ++c)
        {   // also puts back any column something else wrote to
            if (!fresh && getNormalised(kWave0 + c) == src->peaks[c]) continue;
            setNormalised(kWave0 + c, src->peaks[c]);
            notifyLater(kWave0 + c);
        }
    }


    static std::string fit(const std::string& s, int max = kMaxText)
    {
        if ((int)s.size() <= max) return s;
        return s.substr(0, (size_t)max - 3) + "...";
    }
    // Folder names keep their end ("... / Breaks") since that is the part that tells them apart.
    static std::string fitTail(const std::string& s, int max = kMaxText)
    {
        if ((int)s.size() <= max) return s;
        return "..." + s.substr(s.size() - (size_t)(max - 3));
    }
    // The browser row shows its names in large type, so they are shortened further there.
    static constexpr int kFolderChars = 21, kFileChars = 24;

    // Value text for the browser parameters and for SAMPLE when a file plays. Any thread.
    bool browseText(int index, float plain, std::string& out)
    {
        if (index >= kBrowseBase && index < kBrowseBase + kNumTracks * bCount)
        {
            const int t = (index - kBrowseBase) / bCount, b = (index - kBrowseBase) % bCount;
            if (b != bFolder && b != bFile) return false;
            const sculpt::Library* lib = scanner_.library();
            if (!lib) { out = "Scanning drives..."; return true; }
            if (lib->folders.empty()) { out = b == bFolder ? "No WAV files found" : "-"; return true; }
            const int nFolders = (int)lib->folders.size();
            const int fo = mpc::clamp(b == bFolder ? (int)std::lround(plain) : getInt(bidx(t, bFolder)), 0, nFolders - 1);
            const sculpt::Folder& f = lib->folders[(size_t)fo];
            if (b == bFolder) { out = fitTail(f.name, kFolderChars); return true; }
            const int fi = mpc::clamp((int)std::lround(plain), 0, (int)f.files.size() - 1);
            std::string name = f.files[(size_t)fi];
            if (name.size() > 4) name.resize(name.size() - 4);
            out = fit(name, kFileChars);
            return true;
        }
        for (int t = 0; t < kNumTracks; ++t)
        {
            if (index != idx(t, pSample)) continue;
            const std::string st = loader_.status(t);
            if (!st.empty()) { out = fit(st); return true; }
            const std::string nm = loader_.name(t);
            if (!nm.empty()) { out = fit(nm); return true; }
            return false;
        }
        return false;
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Sculpt";
        info.vendor       = "Lightlike";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC('L', 'a', 'l', '3');   // == plugin.json "uid"; never change
        info.version      = 1000;
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 0;   // depends on Space decay and feedback
        return info;
    }

    void addTrackParams(int t)
    {
        using namespace mpc;
        const std::string tp = "t" + std::to_string(t + 1) + "_";
        const std::string np = "T" + std::to_string(t + 1) + " ";
        auto id = [&](const char* s) { return tp + s; };
        auto nm = [&](const char* s) { return np + s; };
        auto F = [&](const char* i, const char* n, float lo, float hi, float d, const char* u, int dec)
        { addParam(floatParam(id(i).c_str(), nm(n).c_str(), lo, hi, d, u, dec)); };
        auto L = [&](const char* i, const char* n, float lo, float hi, float d, const char* u, int dec)
        { addParam(logParam(id(i).c_str(), nm(n).c_str(), lo, hi, d, u, dec)); };
        auto I = [&](const char* i, const char* n, int lo, int hi, int d, const char* u)
        { addParam(intParam(id(i).c_str(), nm(n).c_str(), lo, hi, d, u)); };
        auto C = [&](const char* i, const char* n, std::vector<std::string> c, int d)
        { addParam(choiceParam(id(i).c_str(), nm(n).c_str(), std::move(c), d)); };
        auto B = [&](const char* i, const char* n, bool d) { addParam(boolParam(id(i).c_str(), nm(n).c_str(), d)); };

        // Source: the loop player
        F("speed",  "Speed",  -2.0f, 2.0f, 1.0f, "x", 2);          // varispeed; below 0 plays backwards
        F("rotate", "Rotate", 0.0f, 100.0f, 0.0f, "%", 0);         // turns the loop: where in it playback begins (wraps inside it)
        F("length", "Length", 1.0f, 100.0f, 100.0f, "%", 0);       // how long the loop is, from Start (up to the sample's end)
        I("pitch",  "Pitch",  -24, 24, 0, "st");                    // coarse transpose (also changes speed)
        F("level",  "Level",  0.0f, 100.0f, 80.0f, "%", 0);
        L("fade",   "Fade",   1.0f, 2000.0f, 8.0f, "ms", 0);        // fade at the region edges and on release
        // Loop: Fwd / Ping keep SYNC on the beat grid (Start, Length and Speed in steps); Fwd Tape /
        // Ping Tape let them move freely, and SYNC fits the loop to the beat by changing its speed
        C("loop",   "Loop",   {"Off", "Fwd", "Ping", "Fwd Tape", "Ping Tape"}, 1);
        C("sample", "Sample", {"Kalimba", "Tape", "Haze", "Glass", "Drone", "Analog Pad", "Synth Arp", "Air Pad",
                               "Dark Drone", "Club Kick"}, t);
        // Grain (Grainbloom's engine): slices of the last few seconds of this track
        I("gpitch",  "G Pitch",   -24, 24, 0, "st");
        F("gdensity","G Density", 0.0f, 100.0f, 50.0f, "%", 0);     // overlap / gate / note / echo length
        L("gsize",   "G Size",    10.0f, 1000.0f, 120.0f, "ms", 0); // grain length; a tempo division in rhythmic modes
        F("gspread", "G Spread",  0.0f, 100.0f, 40.0f, "%", 0);     // how far back, and how wild
        F("greverse","G Reverse", 0.0f, 100.0f, 25.0f, "%", 0);     // chance a grain plays backwards
        F("gfeedback","G Feedback", 0.0f, 95.0f, 20.0f, "%", 0);    // the grains fed back into the recording
        C("gmode",   "G Mode", {"Collage", "Cloud", "Stutter", "Shimmer", "Shatter", "Arpeggio", "Cascade"}, 1);
        F("gwet",    "G Wet",     0.0f, 100.0f, 60.0f, "%", 0);
        // Filter: a bank of resonant bands tuned to a scale, shaped by cutoff, slope, reso and tone
        L("cutoff",  "Cutoff",  20.0f, 20000.0f, 9000.0f, "Hz", 0);
        F("reso",    "Reso",    0.0f, 100.0f, 35.0f, "%", 0);
        L("fdecay",  "F Decay", 20.0f, 4000.0f, 300.0f, "ms", 0);   // how long the bands ring
        I("fpitch",  "F Pitch", -24, 24, 0, "st");                  // moves every band (0 = the scale on C)
        C("slope",   "Slope",   {"Low", "Band", "High"}, 0);
        F("ftone",   "F Tone",  -100.0f, 100.0f, 0.0f, "%", 0);
        C("scale",   "Scale",   {"Chrom", "Major", "Minor", "Penta"}, 3);
        F("fwet",    "F Wet",   0.0f, 100.0f, 100.0f, "%", 0);
        // Color
        F("drive",   "Drive",   0.0f, 100.0f, 25.0f, "%", 0);
        F("comp",    "Comp",    0.0f, 100.0f, 20.0f, "%", 0);
        F("crush",   "Crush",   0.0f, 100.0f, 0.0f, "%", 0);
        F("tilt",    "Tilt",    -100.0f, 100.0f, 0.0f, "%", 0);
        F("noise",   "Noise",   0.0f, 100.0f, 0.0f, "%", 0);        // a noise burst struck by each note
        L("ndecay",  "N Decay", 10.0f, 2000.0f, 150.0f, "ms", 0);
        F("ntone",   "N Tone",  0.0f, 100.0f, 60.0f, "%", 0);
        F("cwet",    "C Wet",   0.0f, 100.0f, 100.0f, "%", 0);
        // Space
        F("delay",    "Delay",    0.0f, 100.0f, 20.0f, "%", 0);     // delay Mix (like the reverb's: 50 = both at full level)
        L("time",     "Time",     20.0f, 1500.0f, 375.0f, "ms", 0);
        F("reverb",   "Reverb",   0.0f, 100.0f, 100.0f, "%", 0);    // reverb send (off screen: Mix sets the level)
        F("size",     "Size",     0.0f, 100.0f, 60.0f, "%", 0);
        F("feedback", "Feedback", 0.0f, 100.0f, 40.0f, "%", 0);
        F("spread",   "Spread",   0.0f, 100.0f, 50.0f, "%", 0);     // ping-pong amount
        F("stone",    "S Tone",   -100.0f, 100.0f, 0.0f, "%", 0);
        L("sdecay",   "S Decay",  200.0f, 60000.0f, 3500.0f, "ms", 0);  // up to a minute
        // module switches
        B("src_on",    "Source On", true);
        B("grain_on",  "Grain On",  false);
        B("filter_on", "Filter On", false);
        B("color_on",  "Color On",  false);
        B("space_on",  "Space On",  true);
        // per-module toggles
        B("sync",   "Sync",   false);
        B("lock",   "Lock",   false);
        B("freeze", "Freeze", false);
    }

    void readTrack(int t, sculpt::TrackParams& p) const
    {
        auto f = [&](int k) { return get(idx(t, k)); };
        auto i = [&](int k) { return getInt(idx(t, k)); };
        auto b = [&](int k) { return getBool(idx(t, k)); };
        p.start = get(kStartBase + t) * 0.01f;
        p.chorus = get(cidx(t, cAmount)) * 0.01f; p.chRate = get(cidx(t, cRate));
        p.chDepth = get(cidx(t, cDepth)) * 0.01f; p.chWidth = get(cidx(t, cWidth)) * 0.01f;
        p.revMix = get(kRevMixBase + t) * 0.01f;
        p.revType = getInt(kRevTypeBase + t);
        p.speed = f(pSpeed);
        if (b(pSync) && i(pLoop) < 3) p.speed = syncSpeed(p.speed);   // SYNC: whole octaves, so the loop stays on the beat
        p.rotate = f(pRotate) * 0.01f; p.length = f(pLength) * 0.01f; p.pitch = i(pPitch);
        p.level = f(pLevel) * 0.01f; p.fadeMs = f(pFade); p.loop = i(pLoop); p.syncTape = p.loop >= 3; if (p.loop >= 3) p.loop -= 2; p.sample = i(pSample);
        p.gPitch = i(pGPitch); p.gDensity = f(pGDensity) * 0.01f; p.gSizeMs = f(pGSize); p.gSpread = f(pGSpread) * 0.01f;
        p.gReverse = f(pGReverse) * 0.01f; p.gFeedback = f(pGFeedback) * 0.01f; p.gMode = i(pGMode); p.gWet = f(pGWet) * 0.01f;
        p.gDivision = grainDivision(getNormalised(idx(t, pGSize)));
        p.cutoff = f(pCutoff); p.reso = f(pReso) * 0.01f; p.fDecayMs = f(pFDecay); p.fPitch = i(pFPitch);
        p.slope = i(pSlope); p.fTone = f(pFTone) * 0.01f; p.scale = i(pScale); p.fWet = f(pFWet) * 0.01f;
        p.drive = f(pDrive) * 0.01f; p.comp = f(pComp) * 0.01f; p.crush = f(pCrush) * 0.01f; p.tilt = f(pTilt) * 0.01f;
        p.noise = f(pNoise) * 0.01f; p.nDecayMs = f(pNDecay); p.nTone = f(pNTone) * 0.01f; p.cWet = f(pCWet) * 0.01f;
        p.delay = f(pDelay) * 0.01f; p.timeMs = f(pTime); p.reverb = f(pReverb) * 0.01f; p.size = f(pSize) * 0.01f;
        p.feedback = f(pFeedback) * 0.01f; p.spread = f(pSpread) * 0.01f; p.sTone = f(pSTone) * 0.01f; p.sDecayMs = f(pSDecay);
        p.srcOn = b(pSrcOn); p.grainOn = b(pGrainOn); p.filterOn = b(pFilterOn); p.colorOn = b(pColorOn); p.spaceOn = b(pSpaceOn);
        p.sync = b(pSync); p.lock = b(pLock); p.freeze = b(pFreeze);
    }

    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn())
        {   // diagnostics: remember the last notes (no allocation; plain atomics)
            const uint32_t k = noteCount_.fetch_add(1, std::memory_order_relaxed);
            notes_[k % kRecent].store((e.channel() << 8) | e.note(), std::memory_order_relaxed);
        }
        if (e.isNoteOn())
        {
            // Channel 1 (and 6-16) play the track picked on the screen; channels 2-5 are tracks 1-4,
            // so a sequence can address each track whatever is picked.
            const int ch = e.channel();
            const int track = (ch >= 1 && ch <= kNumTracks) ? ch - 1 : screenTrack();
            const float ratio = std::pow(2.0f, (float)(e.note() - getInt(kRootBase + track)) / 12.0f);
            tracks_[track].noteOn(e.note(), ratio, 0.25f + 0.75f * (float)e.velocity() / 127.0f);
        }
        else if (e.isNoteOff())
        {   // the picked track may have changed since the note started: release it wherever it plays
            for (int t = 0; t < kNumTracks; ++t) tracks_[t].noteOff(e.note(), tp_[t].loop == 0);
        }
        else if (e.isAllNotesOff())
        {
            for (sculpt::Track& tr : tracks_) tr.allNotesOff();
        }
        else if (e.isPitchBend())
        {
            bend_ = std::pow(2.0f, 2.0f * (float)e.pitchBend() / 8192.0f / 12.0f);
        }
    }

    // Loops live in Content/Samples. If one is missing, a soft plucked tone stands in for it.
    void loadSamples()
    {
        for (int s = 0; s < kBuiltins; ++s)
        {
            dsp::WavData w;
            sculpt::Sample& out = samples_[s];
            if (dsp::loadWav(mpc::contentPath(kSampleFiles[s]), w) && w.numFrames() >= 64)
            {
                const size_t nf = w.numFrames();
                out.data.assign(nf, 0.0f);
                const float g = 1.0f / (float)w.numChannels();
                for (int c = 0; c < w.numChannels(); ++c)
                    for (size_t i = 0; i < nf; ++i) out.data[i] += w.channels[(size_t)c][i] * g;
                out.rate = w.sampleRate;
            }
            else
            {
                mpc::log("Sculpt: %s not found, using a stand-in tone", kSampleFiles[s]);
                const int nf = 176400;
                out.data.assign((size_t)nf, 0.0f);
                out.rate = 44100.0;
                const float f = 220.0f * std::pow(2.0f, (float)s * 3.0f / 12.0f);
                for (int i = 0; i < nf; ++i)
                {
                    const float t = (float)(i % 22050) / 44100.0f;
                    out.data[(size_t)i] = 0.5f * std::sin(2.0f * sculpt::kPi * f * (float)i / 44100.0f) * std::exp(-t * 4.0f);
                }
            }
            out.computePeaks();
        }
    }

    // ---------------------------------------------------------------- programs
    // A program starts from every parameter's default and changes a few per track.
    struct Edit { int track, param; float value; };

    void addPreset(const char* name, std::initializer_list<Edit> edits)
    {
        std::vector<float> v((size_t)numParams());
        for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
        for (const Edit& e : edits)   // a preset that sets a track's filter turns that Filter on (it is off by default)
            if (e.track >= 0 && e.param >= pCutoff && e.param <= pFWet) v[(size_t)idx(e.track, pFilterOn)] = 1.0f;
        for (const Edit& e : edits)
        {
            if (e.track < 0) v[(size_t)e.param] = e.value;
            else if (e.param == pReverb)
            {   // presets were written with a reverb send; the level is now Mix (wet = 2 x Mix)
                v[(size_t)(kRevMixBase + e.track)] = e.value * 0.5f;
            }
            else v[(size_t)idx(e.track, e.param)] = e.value;
        }
        v[(size_t)kEditTrack] = 0.0f;   // presets open on track 1, with the view showing it
        // each track's knobs hold what they stand for on its page, so loading never moves a setting
        for (int t = 0; t < kNumTracks; ++t)
            for (int k = 0; k < kKnobs; ++k)
            {
                const int target = cellTarget(t, mpc::clamp((int)std::lround(v[(size_t)(kPage0 + t)]), 0, kModules - 1), k);
                v[(size_t)knobIdx(t, k)] = target >= 0 ? param(target).toNormalised(v[(size_t)target]) : 0.0f;
            }
        for (int k = 0; k < kNumView; ++k) v[(size_t)viewParam(k)] = v[(size_t)viewTarget(0, k)];
        addProgram(name, std::move(v));
    }

    void buildPrograms()
    {
        // helpers for the settings outside the 48-per-track block
        auto ST = [](int t, float pc) { return Edit{-1, kStartBase + t, pc}; };          // Start, %
        auto RM = [](int t, float pc) { return Edit{-1, kRevMixBase + t, pc}; };         // reverb Mix, %
        auto RT = [](int t, int type) { return Edit{-1, kRevTypeBase + t, (float)type}; };   // reverb Type
        auto CH = [](int t, int c, float v) { return Edit{-1, cidx(t, c), v}; };         // chorus
        enum { Kalimba, Tape, Haze, Glass, Drone, AnalogPad, SynthArp, AirPad, DarkDrone, ClubKick };
        enum { Hall, Plate, Bloom, Cloud, Echoes, Canyon, Ripple, Vast };

        addPreset("Init", {});
        // ---- the everyday set: one of each thing Sculpt does
        addPreset("Four Loops", {
            {0, pDelay, 25}, {0, pTime, 375}, {0, pFeedback, 35},
            {1, pColorOn, 1}, {1, pDrive, 30}, {1, pComp, 40}, {1, pLevel, 70},
            {2, pDelay, 10}, RM(2, 35), RT(2, Hall),
            {3, pScale, 3}, {3, pSlope, 1}, {3, pCutoff, 2500}, {3, pFDecay, 600}, {3, pReso, 30}, RM(3, 30)});
        addPreset("Club Night", {
            {0, pSample, ClubKick}, {0, pSync, 1}, {0, pLevel, 90}, {0, pDelay, 0}, RM(0, 5),
            {1, pSample, SynthArp}, {1, pSync, 1}, {1, pCutoff, 2400}, {1, pReso, 45}, {1, pSlope, 0}, {1, pFDecay, 80},
            {1, pDelay, 30}, {1, pTime, 375}, {1, pFeedback, 45}, {1, pLevel, 70},
            {2, pSample, AnalogPad}, {2, pSync, 1}, {2, pLevel, 55}, CH(2, cAmount, 40), RM(2, 30), RT(2, Plate),
            {3, pSample, DarkDrone}, {3, pSync, 1}, {3, pLevel, 45}, {3, pDelay, 0}});
        addPreset("Deep Drone", {
            {0, pSample, Drone}, RM(0, 45), RT(0, Vast), {0, pSize, 90}, {0, pSDecay, 20000}, {0, pDelay, 0},
            {1, pSample, DarkDrone}, {1, pLevel, 55}, CH(1, cAmount, 45), CH(1, cRate, 0.15f), RM(1, 35), RT(1, Canyon),
            {2, pSample, AirPad}, {2, pLevel, 45}, {2, pGrainOn, 1}, {2, pGMode, 1}, {2, pGSize, 500}, {2, pGWet, 60}, RM(2, 50),
            {3, pSample, Drone}, {3, pPitch, -12}, {3, pLevel, 40}});
        addPreset("Analog Dreams", {
            {0, pSample, AnalogPad}, CH(0, cAmount, 50), CH(0, cDepth, 60), RM(0, 30), RT(0, Plate),
            {1, pSample, AirPad}, {1, pLevel, 55}, {1, pDelay, 35}, {1, pTime, 500}, {1, pFeedback, 50}, {1, pSpread, 100},
            {2, pSample, SynthArp}, {2, pSpeed, 0.5f}, {2, pLevel, 50}, {2, pCutoff, 1800}, {2, pReso, 30},
            {3, pSample, Kalimba}, {3, pLevel, 45}, {3, pGrainOn, 1}, {3, pGMode, 3}, {3, pGWet, 50}, RM(3, 40)});
        addPreset("Ring Bells", {
            {0, pSample, Glass}, {0, pSlope, 1}, {0, pScale, 1}, {0, pCutoff, 1500}, {0, pFDecay, 2500}, {0, pReso, 60}, RM(0, 30),
            {1, pSample, Kalimba}, {1, pSlope, 0}, {1, pScale, 3}, {1, pCutoff, 2500}, {1, pFDecay, 1200}, {1, pReso, 40},
            {2, pSample, Drone}, {2, pSlope, 2}, {2, pScale, 2}, {2, pCutoff, 600}, {2, pFDecay, 3000}, {2, pFWet, 80}, {2, pLevel, 50},
            {3, pSample, AirPad}, {3, pLevel, 35}});
        addPreset("Crushed Kit", {
            {0, pSample, Tape}, {0, pColorOn, 1}, {0, pDrive, 70}, {0, pCrush, 60}, {0, pNoise, 30}, {0, pComp, 60}, {0, pDelay, 0},
            {1, pSample, ClubKick}, {1, pSync, 1}, {1, pColorOn, 1}, {1, pDrive, 45}, {1, pComp, 50}, {1, pDelay, 0}, RM(1, 0),
            {2, pSample, Tape}, {2, pSpeed, 0.5f}, {2, pColorOn, 1}, {2, pCrush, 80}, {2, pTilt, -40}, {2, pLevel, 55},
            {3, pSample, SynthArp}, {3, pColorOn, 1}, {3, pCrush, 45}, {3, pDrive, 50}, {3, pLevel, 50}});
        addPreset("Glass Field", {
            {3, pGrainOn, 1}, {3, pGMode, 3}, {3, pGDensity, 60}, {3, pGSize, 260}, {3, pGSpread, 55}, {3, pGFeedback, 35}, {3, pGWet, 70},
            RM(3, 30), {3, pSize, 90}, {3, pSDecay, 7000}, {3, pDelay, 30}, {3, pTime, 500},
            {2, pSpeed, 0.5f}, {2, pLevel, 60}, {2, pCutoff, 2500}, RM(2, 25),
            {0, pSpeed, -1.0f}, {0, pReso, 55}, {0, pFDecay, 900}, {0, pCutoff, 1800},
            {1, pLevel, 0}});
        addPreset("Tape Garden", {
            {1, pColorOn, 1}, {1, pDrive, 55}, {1, pComp, 50}, {1, pCrush, 35}, {1, pTilt, -30}, {1, pNoise, 25},
            RM(1, 8), {1, pSize, 30}, {1, pSync, 1},
            ST(0, 50), {0, pLength, 25}, {0, pLoop, 2}, {0, pCutoff, 3000}, {0, pDelay, 35}, {0, pSync, 1}});
        addPreset("Haze Smear", {
            {2, pGrainOn, 1}, {2, pGMode, 1}, {2, pGSize, 600}, {2, pGDensity, 70}, {2, pGSpread, 70}, {2, pGReverse, 40},
            {2, pGFeedback, 30}, {2, pGWet, 100}, RM(2, 35), {2, pSDecay, 9000}, {2, pFade, 400},
            {2, pCutoff, 4000}, {2, pReso, 40}, {2, pScale, 2}});
        addPreset("Pentatonic Rain", {
            {0, pGrainOn, 1}, {0, pGMode, 5}, {0, pGDensity, 55}, {0, pGSize, 60}, {0, pGSpread, 60}, {0, pGWet, 50},
            {0, pReso, 80}, {0, pFDecay, 1500}, {0, pCutoff, 2200}, {0, pScale, 3}, {0, pSlope, 0},
            {0, pDelay, 45}, {0, pFeedback, 60}, {0, pSpread, 100}, {0, pTime, 250}});
        addPreset("Night Bus", {
            {1, pSpeed, 0.5f}, {1, pColorOn, 1}, {1, pDrive, 40}, {1, pTilt, -50}, {1, pCrush, 20},
            RM(1, 20), {1, pSize, 70}, {1, pSTone, -50},
            {2, pSpeed, -0.5f}, {2, pLevel, 55}, {2, pCutoff, 1200},
            {3, pSpeed, 0.25f}, RM(3, 28), {3, pSDecay, 10000}});
        // ---- experiments
        addPreset("Frozen Sky", {
            {0, pSample, AirPad}, {0, pFreeze, 1}, RM(0, 60), RT(0, Vast), {0, pSize, 100},
            {1, pSample, Glass}, {1, pGrainOn, 1}, {1, pGMode, 6}, {1, pGFeedback, 60}, {1, pGWet, 80}, RM(1, 45), RT(1, Bloom),
            {2, pSample, Haze}, {2, pSpeed, -0.5f}, {2, pLoop, 4}, {2, pLevel, 55},
            {3, pSample, Drone}, {3, pPitch, -12}, {3, pLevel, 45}});
        addPreset("Shatter Machine", {
            {0, pSample, SynthArp}, {0, pSync, 1}, {0, pGrainOn, 1}, {0, pGMode, 4}, {0, pGWet, 100}, {0, pGFeedback, 50},
            {1, pSample, ClubKick}, {1, pSync, 1}, {1, pGrainOn, 1}, {1, pGMode, 2}, {1, pGWet, 60}, {1, pDelay, 0},
            {2, pSample, Tape}, {2, pSync, 1}, {2, pGrainOn, 1}, {2, pGMode, 0}, {2, pGWet, 80}, {2, pGReverse, 50},
            {3, pSample, DarkDrone}, {3, pColorOn, 1}, {3, pCrush, 70}, {3, pLevel, 50}});
        addPreset("Tape Warp", {
            {0, pSample, Kalimba}, {0, pSync, 1}, {0, pLoop, 3}, {0, pLength, 37},
            {1, pSample, Tape}, {1, pSync, 1}, {1, pLoop, 3}, {1, pLength, 62}, {1, pRotate, 25},
            {2, pSample, Haze}, {2, pSync, 1}, {2, pLoop, 4}, {2, pLength, 70}, {2, pLevel, 55},
            {3, pSample, Glass}, {3, pSync, 1}, {3, pLoop, 3}, {3, pLength, 23}, {3, pRotate, 50}, {3, pLevel, 50}});
        addPreset("Spectral Ring", {
            {0, pSample, Tape}, {0, pSlope, 1}, {0, pScale, 3}, {0, pCutoff, 800}, {0, pFDecay, 3500}, {0, pReso, 80},
            {1, pSample, ClubKick}, {1, pSync, 1}, {1, pSlope, 0}, {1, pScale, 2}, {1, pCutoff, 400}, {1, pFDecay, 2000}, {1, pReso, 70},
            {2, pSample, Drone}, {2, pSlope, 2}, {2, pScale, 1}, {2, pCutoff, 1200}, {2, pFDecay, 4000}, {2, pLevel, 50},
            {3, pSample, AirPad}, {3, pLevel, 30}, RM(3, 50)});
        addPreset("Backwards Cathedral", {
            {0, pSample, Kalimba}, {0, pSpeed, -1.0f}, RM(0, 45), RT(0, Cloud), {0, pSDecay, 30000}, {0, pDelay, 30}, {0, pSpread, 100},
            {1, pSample, Glass}, {1, pSpeed, -0.5f}, RM(1, 50), RT(1, Cloud), {1, pSDecay, 30000},
            {2, pSample, Haze}, {2, pSpeed, -1.0f}, {2, pPitch, 12}, {2, pLevel, 50}, RM(2, 60),
            {3, pSample, SynthArp}, {3, pSpeed, -1.0f}, {3, pGrainOn, 1}, {3, pGMode, 3}, {3, pLevel, 40}});
    }

    sculpt::Track tracks_[kNumTracks];
    sculpt::TrackParams tp_[kNumTracks];
    sculpt::Sample samples_[kBuiltins];
    // sample browser: the scanner is declared before the loader so the loader stops first
    sculpt::Scanner scanner_;
    sculpt::Loader loader_;
    sculpt::Sample* active_[kNumTracks] = {nullptr, nullptr, nullptr, nullptr};   // a loaded file, audio thread only
    bool keyHeld_[kNumTracks][bCount] = {};
    bool rowHeld_[2 * kListRows] = {};
    long listSig_ = -1;
    uint32_t waveId_ = 0;   // which sample the screen's waveform shows
    int lastFolder_[kNumTracks] = {0, 0, 0, 0};
    int lastFile_[kNumTracks] = {0, 0, 0, 0};
    int loadDue_[kNumTracks] = {0, 0, 0, 0};   // samples until the highlighted file loads (0 = nothing due)
    int lastSample_[kNumTracks] = {0, 1, 2, 3};
    bool libraryAnnounced_ = false;

    // ---- diagnostics: Content/debug.txt, rewritten about once a second when something changed
    static constexpr uint32_t kRecent = 40;
    struct Recent { std::atomic<int> index{-1}; std::atomic<float> value{0.0f}; };
    std::vector<std::atomic<uint32_t>> hostSets_;
    Recent recent_[kRecent];
    std::atomic<uint32_t> recentCount_{0};
    std::atomic<int> notes_[kRecent];
    std::atomic<uint32_t> noteCount_{0};
    std::atomic<std::thread::id> audioThread_{};
    std::atomic<uint64_t> blocks_{0};
    float meterEnv_[kNumTracks + 1] = {0, 0, 0, 0, 0};
    int meterTimer_ = 0;
    std::atomic<int> lastTransport_{0};
    std::atomic<double> lastPpq_{0.0}, lastBpm_{0.0};
    std::atomic<bool> debugStop_{false};
    std::thread debugThread_;

    void debugLoop()
    {
        std::string last;
        int tick = 0;
        while (!debugStop_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (++tick % 10 != 0) continue;
            std::string s;
            char b[256];
            const sculpt::Library* lib = scanner_.library();
            std::snprintf(b, sizeof b, "Sculpt debug  audio blocks seen: %s\nlibrary: %s (%d folders)\nTrack on screen = %d (pictures, channel-1 pads)\npage: %d\n",
                          blocks_.load() ? "yes" : "NO", lib ? "ready" : "scanning", lib ? (int)lib->folders.size() : 0,
                          screenTrack() + 1, getInt(kPage0) + 1);
            s += b;
            {
                const int tf = lastTransport_.load();
                std::snprintf(b, sizeof b, "transport: playing=%d songpos(ppq) valid=%d tempo valid=%d  bpm=%.2f  ppq=%.1f\n",
                              tf & 1, (tf >> 1) & 1, (tf >> 2) & 1, lastBpm_.load(), std::floor(lastPpq_.load() * 2.0) / 2.0);
                s += b;
            }
            for (int t = 0; t < kNumTracks; ++t)
            {
                std::snprintf(b, sizeof b, "track %d: sample=%s file=\"%s\" status=\"%s\" play=%d\n", t + 1,
                              displayText(idx(t, pSample)).c_str(), loader_.name(t).c_str(), loader_.status(t).c_str(),
                              getBool(kPlay + t) ? 1 : 0);
                s += b;
            }
            s += "host changes per parameter (index name: count, value now):\n";
            for (int i = 0; i < numParams() && i < (int)hostSets_.size(); ++i)
            {
                const uint32_t c = hostSets_[(size_t)i].load();
                if (c == 0) continue;
                std::snprintf(b, sizeof b, "  %3d %-14s %5u  %s\n", i, param(i).name.c_str(), c, displayText(i).c_str());
                s += b;
            }
            s += "last host changes, oldest first:\n";
            const uint32_t n = recentCount_.load();
            for (uint32_t k = n > kRecent ? n - kRecent : 0; k < n; ++k)
            {
                const Recent& r = recent_[k % kRecent];
                const int i = r.index.load();
                if (i < 0 || i >= numParams()) continue;
                std::snprintf(b, sizeof b, "  %3d %-14s = %g\n", i, param(i).name.c_str(), (double)r.value.load());
                s += b;
            }
            s += "last notes received (note / MIDI channel 1-16):\n";
            const uint32_t nn = noteCount_.load();
            for (uint32_t k = nn > kRecent ? nn - kRecent : 0; k < nn; ++k)
            {
                const int v = notes_[k % kRecent].load();
                std::snprintf(b, sizeof b, "  note %3d  ch %2d\n", v & 255, (v >> 8) + 1);
                s += b;
            }
            if (s == last) continue;
            last = s;
            if (std::FILE* f = std::fopen(mpc::contentPath("debug.txt").c_str(), "wb"))
            {
                std::fwrite(s.data(), 1, s.size(), f);
                std::fclose(f);
            }
        }
    }
    int lastEdit_ = 0;
    float lastView_[kNumView] = {};
    int grainTick_ = 0, grainEdit_ = -1;
    int lastPage_[kNumTracks] = {-1, -1, -1, -1};
    int pixPage_[kNumTracks] = {-1, -1, -1, -1};
    uint32_t waveIds_[kNumTracks] = {};
    float lastKnob_[kNumTracks][kKnobs] = {}, lastKnobTarget_[kNumTracks][kKnobs] = {};
    // the queue of values to tell the MPC about (sized in prepare, used only by the audio thread)
    std::vector<int> nq_;
    std::vector<unsigned char> nqFlag_;
    size_t nqHead_ = 0, nqCount_ = 0;
    // the Filter screen's analysis (sized once here, never in the audio thread)
    std::vector<float> specA_ = std::vector<float>(sculpt::Track::kSpecN), specB_ = std::vector<float>(sculpt::Track::kSpecN);
    std::vector<std::complex<float>> fft_ = std::vector<std::complex<float>>(sculpt::Track::kSpecN);
    std::vector<float> window_ = makeWindow();
    float windowSum_ = 0.5f * sculpt::Track::kSpecN;
    float specHeld_[kNumTracks][2][kSpecBands] = {};
    static std::vector<float> makeWindow()
    {
        std::vector<float> w((size_t)sculpt::Track::kSpecN);
        for (size_t i = 0; i < w.size(); ++i) w[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * (float)i / (float)(w.size() - 1));
        return w;
    }
    float grainLo_ = -1.0f, grainHi_ = -1.0f;
    float hitLevel_[kGrainCols] = {}, lastCols_[kGrainCols] = {};
    static constexpr int kRippleFrames1 = 80;   // ripple picture numbers 0..80 (81 frames)
    float lastTarget_[kNumView] = {};
    bool playHeld_[kNumTracks] = {false, false, false, false};
    float gain_ = 0.7f, bend_ = 1.0f;
    bool notifyAll_ = false;
    std::vector<std::string> units_;
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(Sculpt)
