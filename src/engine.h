// engine.h - one Sculpt track: Source -> Grain -> Filter -> Color -> Space.
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Lightlike
//
// Everything here is original code written for Sculpt. The track is mono from the Source
// player up to Space, which spreads it to stereo. A module that is switched off costs
// nothing; a whole track that has been silent for a moment is skipped until a note wakes it.
//
// Realtime rules: all buffers are allocated in prepare(); process() only reads and writes them.

#ifndef SCULPT_ENGINE_H
#define SCULPT_ENGINE_H

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/noise.h"
#include "dsp/delay_line.h"
#include "dsp/tempo.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sculpt {

using mpc::clamp;

constexpr float kPi = 3.14159265358979f;
constexpr int kVoicesPerTrack = 2;
constexpr int kMaxGrains = 32;                    // the Grain module (after Grainbloom): up to 32 at once
constexpr int kGrainBufBits = 18;                 // 262144 samples, ~6 s at 44.1 kHz
constexpr int kGrainBufSize = 1 << kGrainBufBits;
constexpr int kGrainMask = kGrainBufSize - 1;

// A loaded loop: mono, with its own sample rate.
constexpr int kWaveCols = 128;   // columns in the on-screen waveform

// A loaded loop: mono, with its own sample rate, and a coarse picture of itself for the screen.
struct Sample
{
    std::vector<float> data;
    double rate = 44100.0;
    float peaks[kWaveCols] = {};   // loudest point per column, 0..1, scaled to the loudest column
    uint32_t id = 0;               // unique per loaded sample, so the screen knows when to redraw
    int size() const { return (int)data.size(); }

    void computePeaks()
    {
        static std::atomic<uint32_t> nextId{1};
        id = nextId.fetch_add(1);
        const int n = size();
        float top = 0.0f;
        for (int c = 0; c < kWaveCols; ++c)
        {
            const int a = (int)((long long)n * c / kWaveCols), b = (int)((long long)n * (c + 1) / kWaveCols);
            float pk = 0.0f;
            for (int i = a; i < b; ++i) pk = std::fmax(pk, std::fabs(data[(size_t)i]));
            peaks[c] = pk;
            top = std::fmax(top, pk);
        }
        // scale to the loudest column, with a little lift so quiet passages still show
        for (float& p : peaks) p = top > 1e-6f ? std::pow(p / top, 0.8f) : 0.0f;
    }
};

// The values one track needs for one block, already converted to plain units.
struct TrackParams
{
    // Source
    float speed = 1.0f, rotate = 0.0f, length = 1.0f; int pitch = 0;
    float start = 0.0f;   // where the loop region begins in the sample (0..1); Length runs from here
    float level = 0.8f, fadeMs = 10.0f; int loop = 1, sample = 0;
    // Grain
    // Grain (Grainbloom's engine): Mode, Pitch, Size (ms, or a tempo division in the rhythmic
    // modes), Density, Spread, Reverse, Feedback, Wet; Lock = Hold
    int gMode = 1, gPitch = 0, gDivision = 7; float gSizeMs = 120.0f, gDensity = 0.5f, gSpread = 0.4f;
    float gReverse = 0.25f, gFeedback = 0.2f, gWet = 0.5f;
    // Filter
    float cutoff = 8000.0f, reso = 0.3f, fDecayMs = 400.0f; int fPitch = 0, slope = 1;
    float fTone = 0.0f; int scale = 0; float fWet = 1.0f;
    // Color
    float drive = 0.2f, comp = 0.0f, crush = 0.0f, tilt = 0.0f, noise = 0.0f, nDecayMs = 200.0f,
          nTone = 0.6f, cWet = 1.0f;
    // Space
    float delay = 0.2f, timeMs = 375.0f, reverb = 0.25f, size = 0.6f, feedback = 0.45f, spread = 0.5f,
          sTone = 0.0f, sDecayMs = 2500.0f;
    // stereo chorus, first in Space: amount (0..1), LFO rate in Hz, depth (0..1), stereo width (0..1)
    float chorus = 0.0f, chRate = 0.6f, chDepth = 0.5f, chWidth = 1.0f;
    float revMix = 0.2f;
    int revType = 0;       // the reverb's type (kRevTypes below)   // reverb Mix: 0 = dry only, 0.5 = dry and reverb both at full level, 1 = reverb only
    // Module switches and per-module toggles
    bool srcOn = true, grainOn = false, filterOn = true, colorOn = false, spaceOn = true;
    bool sync = false, lock = false, freeze = false;
    bool syncTape = false;   // SYNC fits a free loop to the beat by its speed (tape), not by beat steps
    // Mix: the track's gain into the stereo output after Space (volume, pan, mute and solo combined)
    float mixL = 1.0f, mixR = 1.0f;
};

// Topology-preserving state-variable filter with direct access to k (= 1/Q).
struct Svf
{
    float g = 0.1f, k = 1.4f, a1 = 0, a2 = 0, a3 = 0, s1 = 0, s2 = 0;
    void set(float gIn, float kIn)
    {
        g = gIn; k = kIn;
        a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    void reset() { s1 = s2 = 0.0f; }
    inline void tick(float x, float& lp, float& bp)
    {
        const float v3 = x - s2;
        const float v1 = a1 * s1 + a2 * v3;
        const float v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2.0f * v1 - s1; s2 = 2.0f * v2 - s2;
        lp = v2; bp = v1;
    }
};

inline float gFor(float hz, float sr) { return std::tan(kPi * clamp(hz, 10.0f, 0.45f * sr) / sr); }
// Cheap stand-ins for sin and tanh where exactness doesn't matter (slow LFOs, a soft limit):
// they cost a few multiplies instead of a library call, which matters on the MPC's processor.
inline float fastSin01(double phase)   // sin(2 pi phase), phase in 0..1 (any value wraps)
{
    float x = (float)(phase - std::floor(phase)) * 2.0f - 1.0f;     // -1..1 = -pi..pi
    const float y = 4.0f * x * (1.0f - std::fabs(x));                // parabola
    return -(0.775f * y + 0.225f * y * std::fabs(y));                // refined (error < 0.1%)
}
inline float fastTanh(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
inline float onePoleA(float hz, float sr) { return 1.0f - std::exp(-2.0f * kPi * clamp(hz, 5.0f, 0.45f * sr) / sr); }
inline float smoothstep(float e) { return e * e * (3.0f - 2.0f * e); }

// What the host says about its transport, at the first frame of the block being rendered.
// PITCH's two read heads: each slides through a short window and fades in and out, half a window
// apart; `d` is the small nudge each one took when it restarted, so it lines up with the other
// The part of the sample that loops: where it starts, how long it is, how far Rotate turns it
// (all in samples), and the fade at its seams
struct Region
{
    double start = 0.0, len = 0.0, rot = 0.0;
    float fade = 1.0f;
    int n = 0;          // the sample's length, and which sample, when this was set
    uint32_t id = 0;
};

struct PitchShift
{
    double ph = 0.0;
    double d[2] = {0.0, 0.0};
};

struct Transport
{
    bool playing = false;
    bool ppqValid = false;
    double ppq = 0.0;      // song position in quarter notes
    double bpm = 120.0;
};

// Scales for the resonator bank (semitones above the root).
static const int kScales[4][12] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
    {0, 2, 4, 5, 7, 9, 11, -1, -1, -1, -1, -1},
    {0, 2, 3, 5, 7, 8, 10, -1, -1, -1, -1, -1},
    {0, 2, 4, 7, 9, -1, -1, -1, -1, -1, -1, -1},
};
static const int kScaleLen[4] = {12, 7, 7, 5};

class Track
{
private:
    struct Voice
    {
        bool active = false, releasing = false, started = false;
        int note = -1, dir = 1;
        float ratio = 1.0f, vel = 1.0f, rel = 1.0f, att = 0.0f;   // att: the fade-in when it starts
        double pos = 0.0;
        double opos = 0.0;     // where it reads in the region fading out after a change (unwrapped)
        PitchShift ps;         // PITCH's two read heads
        bool killing = false;
        float killGain = 1.0f;
        int pendNote = -1; float pendRatio = 1.0f, pendVel = 1.0f;
        uint32_t order = 0;
    };
    // One grain (from Grainbloom): reads the recording at its own speed under a window.
    struct Grain
    {
        bool active = false;
        double pos = 0.0;      // absolute read position in the recording (wrapped on read)
        float rate = 1.0f;     // samples per output sample; negative = reverse
        int length = 0, age = 0;
        float invLength = 0.0f, gain = 1.0f;
        bool hardEdges = false;
        float fadeSamples = 1.0f;
    };

public:
    void prepare(double sampleRate)
    {
        sr_ = (float)sampleRate;
        grainBuf_.assign((size_t)kGrainBufSize, 0.0f);
        for (int i = 0; i <= kWinSize; ++i) grainWin_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * (float)i / (float)kWinSize);
        grainWin_[kWinSize + 1] = 0.0f;
        const int maxDelay = (int)(1.6 * sampleRate) + 4;
        delL_.prepare(maxDelay); delR_.prepare(maxDelay);
        chorusLine_.prepare((int)(0.05 * sampleRate) + 8);
        const float scaleMax = kMaxScale * sr_ / 44100.0f;
        const int modMax = (int)(kModMaxSec * sr_) + 4;
        float lenMax = 0.0f, inMax = 0.0f;   // the longest lines any type asks for
        for (const RevType& m : kRevTypes) { lenMax = std::fmax(lenMax, m.lenMul); inMax = std::fmax(inMax, m.inLen); }
        swell_.prepare((int)(kSwellMaxSec * sr_) + 8);
        for (int i = 0; i < kLines; ++i)
        {
            const float longest = std::fmax(kTankLen[kLines - 1], kRippleLen[kLines - 1]) * lenMax;   // any line may be the longest
            tank_[i].prepare((int)(longest * scaleMax) + modMax + 8);
            loopAp_[i].prepare((int)(kLoopApLen[i] * scaleMax) + 8);
        }
        for (int i = 0; i < 4; ++i) inAp_[i].prepare((int)(kInApLen[i] * inMax * 2.0f * sr_ / 44100.0f) + 8);
        rng_.reseed(0x5c0157u + (uint32_t)index_ * 977u);
        reset();
    }

    void setIndex(int i) { index_ = i; }

    void reset()
    {
        for (Voice& v : voices_) v = Voice();
        for (Grain& g : grains_) g = Grain();
        std::fill(grainBuf_.begin(), grainBuf_.end(), 0.0f);
        writePos_ = 0; gWrite_ = 0; gRecorded_ = 0; samplesToNext_ = 0.0; lastFb_ = 0.0f; fbS_ = 0.0f;
        stutterAnchor_ = -1.0; shatterLeft_ = 0; arpStep_ = 0; arpAnchor_ = -1.0; spawnHead_ = 0;
        std::fill(spawnBack_, spawnBack_ + kSpawnMemory, -1.0f);
        std::fill(cols_, cols_ + kGrainCols, 0.0f); colPeak_ = 0.0f; colHead_ = 0; grainFilled_ = 0;
        std::fill(colPos_, colPos_ + kGrainCols, -1.0f);
        cur_ = Region(); regX_ = 0.0f; pFade_ = 0.0f; lastPRatio_ = 1.0;
        for (Svf& s : band_) s.reset();
        std::fill(bandW_, bandW_ + kBands, 0.0f);
        fTiltLp_ = cTiltLp_ = 0.0f;
        noiseEnv_ = noiseLp_ = 0.0f;
        compEnv_ = 0.0f; compGain_ = 1.0f; compCount_ = 0;
        crushHold_ = 0.0f; crushCount_ = 0;
        delL_.reset(); delR_.reset();
        swell_.reset(); chorusLine_.reset(); chPhase_ = 0.0; chMix_ = 0.0f; chDepthS_ = 0.0f; revMixS_ = -1.0f; dMixS_ = 0.0f; revOdd_ = false; revHeld_ = revCurL_ = revCurR_ = revPrevL_ = revPrevR_ = 0.0f;
        dampL_ = dampR_ = hpL_ = hpR_ = 0.0f;
        for (int i = 0; i < kLines; ++i)
        {
            tank_[i].reset(); loopAp_[i].reset(); damp_[i] = 0.0f; lowCut_[i] = 0.0f;
            lfoPh_[i] = 0.13 * i; tankLenS_[i] = -1.0f; loopApLenS_[i] = -1.0f;
        }
        for (int i = 0; i < 4; ++i) { inAp_[i].reset(); inApLenS_[i] = -1.0f; }
        delayTime_ = -1.0f;
        level_ = -1.0f;
        runGain_ = 0.0f; runPhase_ = -1.0; xfade_ = 0.0f;
        silentFrames_ = 1 << 30;   // start asleep
        order_ = 0;
    }

    // `ratio` is the note's playback-rate ratio (1 = the loop's own pitch).
    void noteOn(int note, float ratio, float velocity)
    {
        Voice* v = nullptr;
        for (Voice& c : voices_) if (!c.active) { v = &c; break; }
        if (v == nullptr)
        {   // all busy: fade the oldest out over 5 ms, then it restarts with this note
            v = &voices_[0];
            for (Voice& c : voices_) if (c.order < v->order) v = &c;
            v->killing = true; v->killGain = 1.0f;
            v->pendNote = note; v->pendRatio = ratio; v->pendVel = velocity;
            v->order = ++order_;
        }
        else start(*v, note, ratio, velocity);
        noiseEnv_ = velocity;
        wake();
    }

    void start(Voice& v, int note, float ratio, float velocity)
    {
        v.active = true; v.releasing = false; v.started = false; v.killing = false;
        v.note = note; v.ratio = ratio; v.vel = velocity;
        v.rel = 1.0f; v.att = 0.0f; v.dir = 1; v.ps = PitchShift(); v.order = ++order_;
    }

    void noteOff(int note, bool loopOff)
    {
        for (Voice& v : voices_)
            if (v.active && v.note == note && !v.releasing && !loopOff) v.releasing = true;
    }

    void allNotesOff() { for (Voice& v : voices_) if (v.active) v.releasing = true; }

    // Where the track is in its loop, 0..1 across the whole sample (as the waveform is drawn),
    // or -1 when nothing plays. The SYNC loop wins; otherwise the most recent note.
    double playPosition() const
    {
        if (lastN_ < 64) return -1.0;
        double pos = -1.0;
        if (runGain_ > 0.0f && runPhase_ >= 0.0)
        {
            double ph = lastSpeedNeg_ ? 1.0 - runPhase_ : runPhase_;
            if (lastLoop_ == 2) ph = ph < 0.5 ? 2.0 * ph : 2.0 - 2.0 * ph;
            pos = ph * lastRegionLen_;
        }
        else
        {
            const Voice* best = nullptr;
            for (const Voice& v : voices_)
                if (v.active && v.started && !v.releasing && (best == nullptr || v.order > best->order)) best = &v;
            if (best) pos = best->pos;
        }
        if (pos < 0.0) return -1.0;
        double q = pos + lastRotOff_;
        if (q >= lastRegionLen_) q -= lastRegionLen_;
        const double a = (lastRegionStart_ + q) / (double)lastN_;
        return a - std::floor(a);
    }

    // The loudest output sample since the last call (after the mix gain), for the MIX meters.
    float takePeak() { const float pk = meterPeak_; meterPeak_ = 0.0f; return pk; }

    bool anyVoice() const { for (const Voice& v : voices_) if (v.active) return true; return false; }

    // Adds this track's stereo output to outL/outR.
    void process(const TrackParams& p, const Sample& smp, float bendRatio, double bpm, bool bpmValid,
                 const Transport& tr, float* outL, float* outR, int n)
    {
        playPosNow_ = (float)playPosition();   // which part of the sample is sounding (for the Grain screen)
        if (n <= 0 || smp.size() < 64) return;
        // SYNC: the loop runs by itself while the host plays, locked to its song position
        const bool runWanted = p.sync && p.srcOn && tr.playing && tr.ppqValid && tr.bpm > 20.0;
        if (runWanted && runGain_ <= 0.0f) wake();
        const bool busy = anyVoice() || p.freeze || runWanted || runGain_ > 0.0f;
        if (!busy && silentFrames_ > (int)(0.25f * sr_)) return;   // asleep: costs nothing

        // ---- block setup ---------------------------------------------------------------
        const int N = smp.size();
        const float srcRatio = (float)(smp.rate / sr_);
        // Tape-loop region: it starts at Rotate and covers Length of the sample, wrapping round past
        // the end of the sample back to its beginning (as a loop of tape would).
        // The loop region: from Start, Length of the sample long, stopping at the sample's end.
        // Rotate turns the loop: playback begins that far into the region and wraps round inside it.
        {
            Region want;
            want.start = std::fmin(std::floor((double)p.start * N), (double)N - 64.0);
            want.len = std::fmax(64.0, std::fmin((double)p.length * N, (double)N - want.start));
            if (p.sync && !p.syncTape)
            {   // SYNC: Start and Length move in beat steps (the sample counts as a whole number of
                // beats), so the loop always plays at the one speed the tempo sets and Length never
                // changes its pitch. Length goes 1/16 beat, 1/8, ... up to the whole sample.
                const double beat = (double)N / sampleBeats(smp);
                const double steps = std::log2(sampleBeats(smp) * 16.0);   // 1/16 beat .. the whole sample
                const double k = std::round(clamp(p.length, 0.0f, 1.0f) * steps);
                want.len = std::fmin((double)N, beat / 16.0 * std::pow(2.0, k));
                const double grid = beat / 4.0;                            // Start: a 1/16 note grid
                want.start = std::floor((double)p.start * N / grid) * grid;
                if (want.start + want.len > (double)N) want.start = std::fmax(0.0, (double)N - want.len);
                want.start = std::floor(want.start);
            }
            want.rot = (double)p.rotate * want.len;
            want.fade = (float)std::fmin(want.len * 0.5, std::fmax(1.0, p.fadeMs * 0.001 * smp.rate));
            want.n = N; want.id = smp.id;
            // Moving Start, Length or Rotate would jump the read point and click. Instead the old
            // region fades out over ~15 ms while the new one fades in; while a control is being
            // turned, its newest value is taken up at the end of each crossfade.
            if (cur_.n != N || cur_.id != smp.id) { cur_ = want; regX_ = 0.0f; }
            else if (regX_ <= 0.0f && (std::fabs(want.start - cur_.start) >= 1.0 || std::fabs(want.len - cur_.len) >= 1.0
                                       || std::fabs(want.rot - cur_.rot) >= 1.0))
            { old_ = cur_; cur_ = want; regX_ = 1.0f; for (Voice& v : voices_) v.opos = v.pos; }
            else cur_.fade = want.fade;
        }
        const double regionStart = cur_.start, regionLen = cur_.len, rotOff = cur_.rot;
        const float regXStep = 1.0f / std::fmax(1.0f, 0.015f * sr_);
        lastN_ = N; lastRegionStart_ = regionStart; lastRegionLen_ = regionLen; lastRotOff_ = rotOff;
        lastSpeedNeg_ = p.speed < 0.0f; lastLoop_ = p.loop;
        float rate = p.speed * srcRatio * bendRatio;
        // PITCH changes the note but not the timing: the loop moves through time at SPEED, and two
        // overlapping read heads slide through a short window to play it at a different pitch.
        // Turning Pitch to or from 0 switches between the plain read and the shifted one: that
        // switch is crossfaded over ~15 ms too (while Pitch is 0 the last shift fades out).
        if (p.pitch != 0)
        {
            if (pFade_ <= 0.0f) { for (Voice& v : voices_) v.ps = PitchShift(); runPs_ = PitchShift(); }
            lastPRatio_ = std::pow(2.0, (double)p.pitch / 12.0);
        }
        const double pRatio = lastPRatio_;
        const float pTarget = p.pitch != 0 ? 1.0f : 0.0f;
        const float pStep = 1.0f / std::fmax(1.0f, 0.015f * sr_);
        bool shifting = p.pitch != 0 || pFade_ > 0.0f;
        const double psWin = std::fmax(64.0, std::fmin(regionLen * 0.5, 0.06 * smp.rate));
        if (p.sync && bpmValid && bpm > 20.0) rate *= (float)(bpm / 120.0);
        const float relStep = 1.0f / std::fmax(1.0f, std::fmax(5.0f, p.fadeMs) * 0.001f * sr_);
        const float killStep = 1.0f / std::fmax(1.0f, 0.005f * sr_);
        const float levelTarget = p.srcOn ? p.level * p.level : 0.0f;
        if (level_ < 0.0f) level_ = levelTarget;
        const float levelStep = (levelTarget - level_) / (float)n;

        // the running loop: its region spans a whole number of beats (a power of two), so it stays
        // in time at any tempo; Speed plays it faster or slower by whole factors of that
        double beats;
        if (!p.syncTape) beats = std::fmax(1.0 / 16.0, regionLen / ((double)N / sampleBeats(smp)));   // exact: Length is in beat steps
        else
        {   // tape: the region spans the nearest power-of-two beats (at 120 BPM); it plays faster or
            // slower to fill them, so Length and Speed change its pitch
            beats = std::pow(2.0, std::round(std::log2(std::fmax(1e-3, regionLen / smp.rate * 2.0))));
            beats = std::fmin(64.0, std::fmax(1.0, beats));
        }
        const double speedAbs = std::fabs((double)p.speed);
        // The loop plays continuously and is steered gently towards the host's song position, so a
        // coarse or jittery position never makes it jump. It only jumps (with a short crossfade) when
        // it is clearly somewhere else: a new start, or the playhead moved.
        double runStep = 0.0;
        if (runWanted || runGain_ > 0.0f)
        {
            const double cycles = tr.ppq * speedAbs / beats;
            const double hostPhase = cycles - std::floor(cycles);
            runStep = tr.bpm / 60.0 / (double)sr_ * speedAbs / beats;
            const bool fresh = tr.ppq != lastHostPpq_;   // some hosts repeat a position for several blocks
            lastHostPpq_ = tr.ppq;
            if (runPhase_ < 0.0) { runPhase_ = hostPhase; farCount_ = 0; }
            else if (fresh)
            {
                double diff = hostPhase - runPhase_;
                diff -= std::round(diff);                                   // shortest way round the loop
                const double errSec = runStep > 0.0 ? std::fabs(diff) / runStep / (double)sr_ : 0.0;
                if (errSec > 0.03 && ++farCount_ >= 2)
                {   // more than 30 ms out on two fresh readings in a row: jump, crossfading from where it was
                    oldPhase_ = runPhase_; xfade_ = 1.0f;
                    runPhase_ = hostPhase;
                    farCount_ = 0;
                }
                else if (errSec <= 0.03)
                {   // close: pull in over about half a second; at most a 0.2% speed change
                    farCount_ = 0;
                    const double corr = diff / (0.5 * (double)sr_);
                    runStep += mpc::clamp(corr, -runStep * 0.002, runStep * 0.002);
                }
            }
        }
        const float runTarget = runWanted ? 1.0f : 0.0f;
        const float runRamp = 1.0f / std::fmax(1.0f, std::fmax(5.0f, p.fadeMs) * 0.001f * sr_);
        const float xfadeStep = 1.0f / std::fmax(1.0f, 0.01f * sr_);   // 10 ms crossfade on a jump

        setupGrain(p, bpm, bpmValid, tr);
        setupFilter(p);
        setupColor(p);
        setupSpace(p);

        float peak = 0.0f;
        const float* src = smp.data.data();
        // one read inside region `r` at `pos` (wrapped), with Rotate and the seam fades
        auto readIn = [&](const Region& r, double pos) -> float
        {
            pos -= r.len * std::floor(pos / r.len);
            if (pos >= r.len) pos = 0.0;
            double q = pos + r.rot;
            if (q >= r.len) q -= r.len;
            auto at = [&](double idx)
            {
                int i0 = (int)idx; const float fr = (float)(idx - i0);
                i0 %= N; if (i0 < 0) i0 += N;
                const int i1 = (i0 + 1 == N) ? 0 : i0 + 1;
                return src[i0] + fr * (src[i1] - src[i0]);
            };
            float y = at(r.start + q);
            // The loop's join: just after it goes round, the start of the region is blended with
            // what follows the region's end, so the end runs smoothly into the start with no dip.
            // Where the region ends at the sample's end there is nothing after it: fade instead.
            const bool tail = r.start + r.len + r.fade < (double)N - 1.0;
            if (q < r.fade)
            {
                const float g = (float)(q / r.fade);
                y = tail ? at(r.start + r.len + q) * (1.0f - g) + y * g : y * g;
            }
            else if (!tail && r.len - q < r.fade) y *= (float)((r.len - q) / r.fade);
            // a one-shot (Loop Off) fades out as it reaches the end of the region
            if (p.loop == 0 && r.len - pos < r.fade) y *= clamp((float)((r.len - pos) / r.fade), 0.0f, 1.0f);
            return y;
        };
        // the current region, crossfaded from the previous one just after a change; `oldPos` is
        // where the previous region's read would be (the same place, except for the SYNC loop)
        auto readBoth = [&](double pos, double oldPos) -> float
        {
            const float a = readIn(cur_, pos);
            if (regX_ <= 0.0f) return a;
            return a + (readIn(old_, oldPos) - a) * regX_;
        };
        auto readPos = [&](double pos) -> float { return readBoth(pos, pos); };
        // PITCH: move the heads on by how far the loop moved in time (`timeInc` source samples).
        // When a head wraps round it is silent; it restarts nudged (within ~10 ms) to where its
        // waveform best matches the head that is playing, so the two blend without beating.
        const int alignReach = (int)std::fmin(psWin * 0.25, 0.01 * smp.rate);
        auto align = [&](double base, double otherPos, double readRate) -> double
        {
            constexpr int M = 32, J = 3;                 // compare 32 points, 3 samples apart
            float ref[M]; double rr = 1e-9;
            for (int j = 0; j < M; ++j) { ref[j] = readPos(otherPos + j * J * readRate); rr += ref[j] * ref[j]; }
            if (rr < 1e-7) return 0.0;                   // silence: nothing to line up with
            double best = -1e30, bestD = 0.0;
            for (int dd = -alignReach; dd <= alignReach; dd += 3)
            {
                double xy = 0.0, xx = 1e-9;
                for (int j = 0; j < M; ++j) { const float v = readPos(base + dd + j * J * readRate); xy += v * ref[j]; xx += v * v; }
                const double c = xy / std::sqrt(xx);
                if (c > best) { best = c; bestD = dd; }
            }
            return bestD;
        };
        auto advancePs = [&](PitchShift& sh, double pos, double timeInc)
        {
            if (!shifting) return;
            const double old = sh.ph, ph = old + timeInc * (pRatio - 1.0) / psWin;
            const double nph = ph - std::floor(ph);
            const bool wrap0 = std::floor(ph) != std::floor(old);
            const bool wrap1 = std::floor(ph + 0.5) != std::floor(old + 0.5);
            sh.ph = nph;
            const double ph1 = nph < 0.5 ? nph + 0.5 : nph - 0.5;
            const double next = pos + timeInc, readRate = timeInc * pRatio;
            if (wrap0) sh.d[0] = align(next + nph * psWin, next + ph1 * psWin + sh.d[1], readRate);
            if (wrap1) sh.d[1] = align(next + ph1 * psWin, next + nph * psWin + sh.d[0], readRate);
        };
        auto readShifted = [&](double pos, const PitchShift& sh, double oldPos) -> float
        {
            if (!shifting) return readBoth(pos, oldPos);
            const double ph1 = sh.ph < 0.5 ? sh.ph + 0.5 : sh.ph - 0.5;
            const float g0 = (float)std::sin(3.14159265358979 * sh.ph), g1 = (float)std::sin(3.14159265358979 * ph1);
            const double o0 = sh.ph * psWin + sh.d[0], o1 = ph1 * psWin + sh.d[1];
            const float y = readBoth(pos + o0, oldPos + o0) * g0 * g0 + readBoth(pos + o1, oldPos + o1) * g1 * g1;
            if (pFade_ >= 1.0f) return y;
            const float plain = readBoth(pos, oldPos);
            return plain + (y - plain) * pFade_;
        };
        for (int i = 0; i < n; ++i)
        {
            level_ += levelStep;
            if (regX_ > 0.0f) regX_ = std::fmax(0.0f, regX_ - regXStep);
            if (pFade_ != pTarget)
            {
                pFade_ = pFade_ < pTarget ? std::fmin(pTarget, pFade_ + pStep) : std::fmax(pTarget, pFade_ - pStep);
                shifting = p.pitch != 0 || pFade_ > 0.0f;
            }
            // ---- Source ------------------------------------------------------------------
            float x = 0.0f;
            for (Voice& v : voices_)
            {
                if (!v.active) continue;
                const float inc = rate * v.ratio;
                if (!v.started) { v.pos = inc >= 0.0f ? 0.0 : regionLen - 1.0; v.opos = v.pos; v.started = true; }
                // read with linear interpolation; Rotate turns the loop round inside the region
                const float sw = readShifted(v.pos, v.ps, v.opos);
                advancePs(v.ps, v.pos, (double)inc * v.dir);
                v.att = std::fmin(1.0f, v.att + relStep);
                x += sw * v.rel * v.vel * v.att;
                if (v.killing)
                {
                    x -= sw * v.rel * v.vel * v.att * (1.0f - v.killGain);
                    v.killGain -= killStep;
                    if (v.killGain <= 0.0f) { start(v, v.pendNote, v.pendRatio, v.pendVel); continue; }
                }
                // advance
                v.pos += (double)inc * v.dir;
                v.opos += (double)inc * v.dir;
                if (p.loop == 0)
                {
                    if (v.pos >= regionLen || v.pos < 0.0) v.active = false;
                }
                else if (p.loop == 1)
                {
                    if (v.pos >= regionLen) v.pos -= regionLen * std::floor(v.pos / regionLen);
                    else if (v.pos < 0.0) v.pos += regionLen * std::ceil(-v.pos / regionLen);
                }
                else
                {
                    if (v.pos >= regionLen) { v.pos = 2.0 * regionLen - v.pos - 1.0; v.dir = -v.dir; }
                    if (v.pos < 0.0) { v.pos = -v.pos; v.dir = -v.dir; }
                    v.pos = std::fmin(std::fmax(v.pos, 0.0), regionLen - 1.0);
                }
                if (v.releasing) { v.rel -= relStep; if (v.rel <= 0.0f) { v.rel = 0.0f; v.active = false; } }
            }
            if (runGain_ > 0.0f || runTarget > 0.0f)
            {
                runGain_ += runGain_ < runTarget ? std::fmin(runRamp, runTarget - runGain_) : -std::fmin(runRamp, runGain_ - runTarget);
                auto readAt = [&](double phase)
                {
                    double ph = p.speed < 0.0f ? 1.0 - phase : phase;
                    if (p.loop == 2) ph = ph < 0.5 ? 2.0 * ph : 2.0 - 2.0 * ph;       // Ping: there and back
                    const double pos = std::fmin(ph * regionLen, regionLen - 1.0);
                    return readShifted(pos, runPs_, std::fmin(ph * old_.len, old_.len - 1.0));
                };
                float y = readAt(runPhase_);
                {   // which way the loop is travelling through the sample right now
                    double d = p.speed < 0.0f ? -1.0 : 1.0;
                    if (p.loop == 2 && runPhase_ >= 0.5) d = -d;
                    double ph = p.speed < 0.0f ? 1.0 - runPhase_ : runPhase_;
                    if (p.loop == 2) ph = ph < 0.5 ? 2.0 * ph : 2.0 - 2.0 * ph;
                    advancePs(runPs_, ph * regionLen, d * runStep * (p.loop == 2 ? 2.0 : 1.0) * regionLen);
                }
                if (xfade_ > 0.0f)
                {
                    y = y * (1.0f - xfade_) + readAt(oldPhase_) * xfade_;
                    xfade_ -= xfadeStep;
                    oldPhase_ += runStep;
                    if (oldPhase_ >= 1.0) oldPhase_ -= 1.0;
                }
                x += y * runGain_ * 0.85f;
                runPhase_ += runStep;
                if (runPhase_ >= 1.0) runPhase_ -= 1.0;
            }
            x *= level_;

            // ---- Grain ---------------------------------------------------------------------
            if (p.grainOn) x = grainTick(x, p);
            // ---- Filter --------------------------------------------------------------------
            {   // the Filter screen's spectrum: the sound going into the filter and coming out of it
                const float pre = x;
                if (p.filterOn) x = filterTick(x, p);
                specPre_[specPos_] = pre; specPost_[specPos_] = x; specPos_ = (specPos_ + 1) & (kSpecN - 1);
            }
            // ---- Color ---------------------------------------------------------------------
            if (p.colorOn) x = colorTick(x, p);
            else noiseEnv_ *= noiseDecay_;
            // ---- Space ---------------------------------------------------------------------
            float l = x, r = x;
            if (p.spaceOn) spaceTick(x, p, l, r);

            mixGL_ += 0.002f * (p.mixL - mixGL_);
            mixGR_ += 0.002f * (p.mixR - mixGR_);
            outL[i] += l * mixGL_; outR[i] += r * mixGR_;
            meterPeak_ = std::fmax(meterPeak_, std::fmax(std::fabs(l * mixGL_), std::fabs(r * mixGR_)));
            const float a = std::fabs(l) + std::fabs(r);
            if (a > peak) peak = a;
        }

        if (runGain_ <= 0.0f && !runWanted) { runPhase_ = -1.0; xfade_ = 0.0f; }   // next start lines up afresh
        if (!anyVoice() && !p.freeze && runGain_ <= 0.0f && !runWanted && peak < 1.0e-5f) silentFrames_ += n;
        else silentFrames_ = 0;
    }

private:
    void wake()
    {
        if (silentFrames_ > (int)(0.25f * sr_))
        {
            // Waking from sleep: do not let grains read audio from long ago (unless locked).
            if (!lockedLast_) std::fill(grainBuf_.begin(), grainBuf_.end(), 0.0f);
            for (Grain& g : grains_) g.active = false;
            silentFrames_ = 0;
        }
    }

    // ---------------------------------------------------------------- Grain
    // The Grain module's engine comes from Lightlike's Grainbloom: the track's sound is recorded
    // (Lock holds it), and a scheduler picks slices of it in one of seven modes:
    //   Collage  tempo-synced slices of the recent past, re-ordered, sometimes reversed or an octave off
    //   Cloud    many short overlapping grains scattered back in time
    //   Stutter  one slice repeated on the beat with hard edges, re-grabbed now and then
    //   Shimmer  like Cloud, every grain an octave up (Feedback stacks the octaves)
    //   Shatter  glitch: each step a slice plays once, ratchets 2x/4x, jumps an octave, reverses or drops
    //   Arpeggio one slice replayed as a stepped melody of fifths and octaves
    //   Cascade  multi-tap echoes of slices from 1..8 steps back, each quieter
    // Size is the grain length (Cloud, Shimmer) or, in the rhythmic modes, a tempo division.
    enum GrainMode { gmCollage = 0, gmCloud, gmStutter, gmShimmer, gmShatter, gmArpeggio, gmCascade, gmCount };
    static constexpr float kGrainDriftCents = 10.0f;   // random detune per grain
    static constexpr float kGrainRepeats = 0.5f;       // Cascade: echoes and how slowly they fade

    void setupGrain(const TrackParams& p, double bpm, bool bpmValid, const Transport& tr)
    {
        lockedLast_ = p.lock;
        gMode_ = clamp(p.gMode, 0, gmCount - 1);
        gSizeSamp_ = clamp(p.gSizeMs * 0.001f * sr_, 64.0f, sr_ * 2.0f);
        const float beats = mpc::dsp::divisionBeats(p.gDivision);
        gStepSamp_ = clamp(mpc::dsp::beatsToSamples(beats, bpmValid ? bpm : 120.0, sr_), 64.0f, sr_ * 2.0f);
        const bool rhythmic = gMode_ != gmCloud && gMode_ != gmShimmer;
        // lock the rhythmic grid to the song position while the MPC plays (Shatter: between bursts)
        if (p.grainOn && rhythmic && (gMode_ != gmShatter || shatterLeft_ == 0) && tr.playing && tr.ppqValid)
        {
            const float phase = mpc::dsp::ppqPhase(tr.ppq, beats);
            samplesToNext_ = phase < 1.0e-4f ? 0.0 : (double)((1.0f - phase) * gStepSamp_);
        }
        switch (gMode_)
        {
            case gmCloud: case gmShimmer: gOverlap_ = 1.0f + p.gDensity * 7.0f; break;
            case gmCollage:               gOverlap_ = 1.0f + p.gDensity;        break;
            default:                      gOverlap_ = 1.0f;                     break;
        }
        gGain_ = (gMode_ == gmCloud || gMode_ == gmShimmer) ? 1.4f / std::sqrt(gOverlap_) : 1.0f;
        gFbTarget_ = p.lock ? 0.0f : clamp(p.gFeedback, 0.0f, 0.95f);
    }

    float grainTick(float x, const TrackParams& p)
    {
        // Lock holds the recording still, but only once there is one: a project opened with Lock
        // on records until the buffer is full once, so the grains have sound to work with
        if (!p.lock || grainFilled_ < kGrainBufSize)
        {
            // the filling only counts once sound arrives, so silence before you play is not what Lock keeps
            if (grainFilled_ < kGrainBufSize && (grainFilled_ > 0 || std::fabs(x) > 1e-4f)) ++grainFilled_;
            grainBuf_[(size_t)writePos_] = x + lastFb_;     // the sound, plus the grains fed back
            writePos_ = (writePos_ + 1) & kGrainMask;
            ++gWrite_;
            if (gRecorded_ < kGrainBufSize) ++gRecorded_;
            // the screen's picture of the recording: the loudest point of each 1/64 of it
            colPeak_ = std::fmax(colPeak_, std::fabs(x));
            if ((writePos_ & (kGrainColLen - 1)) == 0)
            {
                cols_[colHead_] = colPeak_;
                colPos_[colHead_] = colPeak_ > 1e-4f ? playPosNow_ : -1.0f;   // where in the sample it came from
                colHead_ = (colHead_ + 1) % kGrainCols; colPeak_ = 0.0f;
            }
        }
        // start new grains when it is time
        samplesToNext_ -= 1.0;
        if (samplesToNext_ <= 0.0 && gRecorded_ > 256)
        {
            double interval;
            if (gMode_ == gmCloud || gMode_ == gmShimmer)
            {
                interval = (double)(gSizeSamp_ / gOverlap_) * (0.5 + rng_.nextFloat());   // jittered
                spawnScatter(p);
            }
            else if (gMode_ == gmShatter) interval = (double)spawnShatter(p);
            else
            {
                interval = (double)gStepSamp_;
                if (gMode_ == gmCollage)       spawnCollage(p);
                else if (gMode_ == gmArpeggio) spawnArp(p);
                else if (gMode_ == gmCascade)  spawnCascade(p);
                else                           spawnStutter(p);
            }
            samplesToNext_ += interval < 32.0 ? 32.0 : interval;
        }
        // sum the grains
        float wet = 0.0f;
        const float* buf = grainBuf_.data();
        for (Grain& g : grains_)
        {
            if (!g.active) continue;
            float w;
            if (g.hardEdges)
            {
                const float edge = (float)std::min(g.age, g.length - g.age);
                w = edge >= g.fadeSamples ? 1.0f : edge / g.fadeSamples;
            }
            else
            {   // Hann window, from a table
                const float xw = (float)g.age * g.invLength * (float)kWinSize;
                const int xi = (int)xw;
                w = grainWin_[xi] + (xw - (float)xi) * (grainWin_[xi + 1] - grainWin_[xi]);
            }
            const double fl = std::floor(g.pos);
            const long long i0 = (long long)fl;
            const float fr = (float)(g.pos - fl);
            const float a = buf[(size_t)(i0 & kGrainMask)], b = buf[(size_t)((i0 + 1) & kGrainMask)];
            wet += (a + fr * (b - a)) * w * g.gain;
            g.pos += (double)g.rate;
            if (++g.age >= g.length) g.active = false;
        }
        wet = mpc::dsp::softClip(wet);
        fbS_ += 0.0015f * (gFbTarget_ - fbS_);
        lastFb_ = mpc::dsp::softClip(wet * fbS_);
        return x + p.gWet * (wet - x);
    }

    // ---- Grainbloom's grain helpers ----
    Grain* freeGrain()
    {
        for (Grain& g : grains_) if (!g.active) return &g;
        Grain* best = &grains_[0];   // all busy: take the one furthest through
        float bestT = -1.0f;
        for (Grain& g : grains_) { const float t = (float)g.age * g.invLength; if (t > bestT) { bestT = t; best = &g; } }
        return best;
    }
    static float semis(float st) { return std::pow(2.0f, st / 12.0f); }
    float grainRate(const TrackParams& p, float extraSemis)
    {
        const float drift = kGrainDriftCents * (rng_.nextFloat() * 2.0f - 1.0f);
        float rate = semis((float)p.gPitch + extraSemis + drift * 0.01f);
        if (rng_.nextFloat() < p.gReverse) rate = -rate;
        return rate;
    }
    // a start point `extraBack` behind the writer, safe so a fast forward grain never overtakes it
    double startFor(float rate, int length, float extraBack)
    {
        const float minBack = (rate > 1.0f ? (rate - 1.0f) * (float)length : 0.0f) + 64.0f;
        float back = minBack + extraBack;
        const float limit = (float)std::min(gRecorded_, (long long)kGrainBufSize) - (float)length * std::fabs(rate) - 64.0f;
        if (back > limit) back = std::max(minBack, limit);
        spawnBack_[spawnHead_] = back; spawnHead_ = (spawnHead_ + 1) % kSpawnMemory;   // for the screen
        return (double)gWrite_ - (double)back;
    }
    void startGrain(Grain& g, double pos, float rate, int length, bool hard, float fade, float gain)
    {
        g.active = true; g.pos = pos; g.rate = rate;
        g.length = length < 16 ? 16 : length;
        g.age = 0; g.invLength = 1.0f / (float)g.length;
        g.hardEdges = hard; g.fadeSamples = fade < 1.0f ? 1.0f : fade;
        g.gain = gain;
    }
    void spawnScatter(const TrackParams& p)
    {
        const float len = gSizeSamp_ * (0.75f + 0.5f * rng_.nextFloat());
        const float rate = grainRate(p, gMode_ == gmShimmer ? 12.0f : 0.0f);
        const float scatter = p.gSpread * rng_.nextFloat() * 2.0f * sr_;   // up to 2 s back
        startGrain(*freeGrain(), startFor(rate, (int)len, scatter), rate, (int)len, false, 1.0f, gGain_);
    }
    void spawnCollage(const TrackParams& p)
    {
        const float len = gStepSamp_ * (1.0f + p.gDensity);   // overlap up to 2 slices
        float extra = 0.0f;
        const float r = rng_.nextFloat();
        if (r < p.gSpread * 0.35f) extra = 12.0f; else if (r < p.gSpread * 0.5f) extra = -12.0f;
        const float rate = grainRate(p, extra);
        const int back = 1 + (int)(rng_.nextFloat() * (1.0f + p.gSpread * 7.0f));
        startGrain(*freeGrain(), startFor(rate, (int)len, gStepSamp_ * (float)back), rate, (int)len, false, 1.0f, 1.0f);
    }
    void spawnStutter(const TrackParams& p)
    {
        const float len = gStepSamp_ * (0.3f + 0.7f * p.gDensity);   // gate length
        const float rate = semis((float)p.gPitch);
        const double oldest = (double)gWrite_ - (double)std::min(gRecorded_, (long long)kGrainBufSize) + 4.0 * gStepSamp_;
        if (stutterAnchor_ < 0.0 || rng_.nextFloat() < 0.1f + p.gSpread * 0.6f || stutterAnchor_ < oldest)
        {
            stutterAnchor_ = startFor(rate, (int)len, gStepSamp_);
            stutterReverse_ = rng_.nextFloat() < p.gReverse;
        }
        const float fade = std::min(0.004f * sr_, len * 0.25f);
        const double pos = stutterReverse_ ? stutterAnchor_ + (double)len * rate : stutterAnchor_;
        startGrain(*freeGrain(), pos, stutterReverse_ ? -rate : rate, (int)len, true, fade, 1.0f);
    }
    float spawnShatter(const TrackParams& p)
    {
        if (shatterLeft_ == 0)
        {
            const float r = rng_.nextFloat();
            shatterHits_ = r < 0.15f + 0.35f * p.gDensity ? (r < 0.05f + 0.15f * p.gDensity ? 4 : 2) : 1;
            shatterLeft_ = shatterHits_;
            shatterMute_ = rng_.nextFloat() < 0.25f * (1.0f - p.gDensity);
            float st = 0.0f;
            const float q = rng_.nextFloat();
            if (q < p.gSpread * 0.25f) st = 12.0f; else if (q < p.gSpread * 0.4f) st = -12.0f;
            shatterRate_ = semis((float)p.gPitch + st);
            shatterReverse_ = rng_.nextFloat() < p.gReverse;
            const int back = 1 + (int)(rng_.nextFloat() * (1.0f + p.gSpread * 7.0f));
            shatterAnchor_ = startFor(shatterRate_, (int)gStepSamp_, gStepSamp_ * (float)back);
        }
        const float hit = gStepSamp_ / (float)shatterHits_;
        --shatterLeft_;
        if (!shatterMute_)
        {
            const float len = hit * 0.92f;
            const float fade = std::min(0.003f * sr_, len * 0.25f);
            const double pos = shatterReverse_ ? shatterAnchor_ + (double)len * shatterRate_ : shatterAnchor_;
            startGrain(*freeGrain(), pos, shatterReverse_ ? -shatterRate_ : shatterRate_, (int)len, true, fade, 1.0f);
        }
        return hit;
    }
    void spawnArp(const TrackParams& p)
    {
        static const float kUp[5] = {0.0f, 7.0f, 12.0f, 19.0f, 24.0f};
        static const float kUpDown[8] = {0.0f, 7.0f, 12.0f, 19.0f, 24.0f, 19.0f, 12.0f, 7.0f};
        const int notes = 2 + (int)(p.gSpread * 6.0f + 0.5f);   // 2..8
        if (arpStep_ >= notes || arpAnchor_ < 0.0) arpStep_ = 0;
        if (arpStep_ == 0) arpAnchor_ = startFor(semis(24.0f + (float)p.gPitch), (int)gStepSamp_, gStepSamp_);
        int idx = arpStep_;
        if (p.gSpread > 0.8f) idx = (int)(rng_.nextFloat() * (float)notes) % notes;
        const float rate = grainRate(p, notes <= 5 ? kUp[idx % 5] : kUpDown[idx % 8]);
        const float len = gStepSamp_ * (0.35f + 0.6f * p.gDensity);
        const float fade = std::min(0.004f * sr_, len * 0.25f);
        const double pos = rate < 0.0f ? arpAnchor_ + (double)len * -rate : arpAnchor_;
        startGrain(*freeGrain(), pos, rate, (int)len, true, fade, 0.9f);
        ++arpStep_;
    }
    void spawnCascade(const TrackParams& p)
    {
        const int taps = 1 + (int)(kGrainRepeats * 7.0f + 0.5f);
        const float decay = 0.55f + 0.35f * kGrainRepeats;
        const float norm = 1.0f / std::sqrt(1.0f + 0.5f * (float)(taps - 1));
        for (int k = 1; k <= taps; ++k)
        {
            float st = 0.0f;
            if (k > 1 && rng_.nextFloat() < p.gSpread * 0.6f) st = (k % 2 == 0) ? 12.0f : -12.0f;
            const float rate = grainRate(p, st);
            const float len = gStepSamp_ * (0.5f + 0.6f * p.gDensity);
            startGrain(*freeGrain(), startFor(rate, (int)len, gStepSamp_ * (float)k), rate, (int)len, false, 1.0f,
                       norm * std::pow(decay, (float)(k - 1)));
        }
    }

public:
    // ---- the Grain screen (audio thread) ----
    // The recording, oldest (0) to newest (63), scaled for the screen.
    void grainColumns(float* out) const
    {
        for (int c = 0; c < kGrainCols; ++c)
        {
            const float pk = cols_[(colHead_ + c) % kGrainCols];
            out[c] = clamp(std::pow(pk * 1.4f, 0.7f), 0.0f, 1.0f);
        }
    }
    // Which parts of the loaded sample the grains are picking from: marks[b] = 1 for each of `bins`
    // stretches of the sample that sounded inside the pick zone (or where a grain reads now).
    // Returns how many were marked.
    int grainSampleMarks(const TrackParams& p, unsigned char* marks, int bins) const
    {
        for (int b = 0; b < bins; ++b) marks[b] = 0;
        float lo = 0.0f, hi = 1.0f;
        grainZone(p, lo, hi);
        int count = 0;
        auto markSpan = [&](float a, float b)
        {
            if (a < 0.0f) return;
            int i0 = (int)(a * bins), i1 = i0;
            if (b >= a && b - a < 0.25f) i1 = (int)(b * bins);   // the stretch it played through (no wrap)
            for (int i = std::max(0, i0); i <= std::min(bins - 1, i1); ++i) if (!marks[i]) { marks[i] = 1; ++count; }
        };
        for (int c = 0; c < kGrainCols; ++c)
        {
            const float mid = (c + 0.5f) / kGrainCols;
            if (mid < lo || mid > hi) continue;
            const float a = colPos_[(colHead_ + c) % kGrainCols];
            const float b = c + 1 < kGrainCols ? colPos_[(colHead_ + c + 1) % kGrainCols] : a;
            markSpan(a, b);
        }
        float dots[kMaxGrains];
        grainMarks(dots);
        for (float d : dots)
            if (d > 0.0f) { const int c = std::min(kGrainCols - 1, (int)(d * kGrainCols)); const float a = colPos_[(colHead_ + c) % kGrainCols]; markSpan(a, a); }
        return count;
    }
    // Where grains are being picked from, as 0..1 across the recording (1 = now): the span of the
    // last few grains' start points, whatever the mode.
    void grainZone(const TrackParams&, float& lo, float& hi) const
    {
        float nearB = 1e30f, farB = -1.0f;
        for (float b : spawnBack_) if (b >= 0.0f) { nearB = std::min(nearB, b); farB = std::max(farB, b); }
        if (farB < 0.0f) { lo = hi = 0.0f; return; }
        const float B = (float)kGrainBufSize;
        lo = clamp(1.0f - (farB + gSizeSamp_) / B, 0.0f, 1.0f);
        hi = clamp(1.0f - nearB / B, 0.0f, 1.0f);
    }
    // Where each grain is reading right now, 0..1 across the recording; 0 = no grain in that slot.
    void grainMarks(float* out) const
    {
        for (int k = 0; k < kMaxGrains; ++k)
        {
            const Grain& g = grains_[k];
            if (!g.active) { out[k] = 0.0f; continue; }
            double back = (double)gWrite_ - g.pos;
            back -= std::floor(back / kGrainBufSize) * kGrainBufSize;
            out[k] = std::fmax(0.001f, 1.0f - (float)(back / kGrainBufSize));
        }
    }
    static constexpr int kGrainCols = 64;
    // The last kSpecN samples before and after the filter, oldest first (for the Filter screen).
    static constexpr int kSpecN = 2048;
    void spectrumInput(float* pre, float* post) const
    {
        for (int i = 0; i < kSpecN; ++i)
        {
            const int k = (specPos_ + i) & (kSpecN - 1);
            pre[i] = specPre_[k]; post[i] = specPost_[k];
        }
    }
    static constexpr int kGrainColLen = kGrainBufSize / kGrainCols;   // 4096 samples per column
private:

public:
    // How many beats the whole sample counts as in SYNC: its length at 120 BPM, rounded to a power
    // of two (1 to 64), like a loop cut to whole bars.
    static double sampleBeats(const Sample& smp)
    {
        const double sec = (double)smp.size() / smp.rate;
        return std::fmin(64.0, std::fmax(1.0, std::pow(2.0, std::round(std::log2(std::fmax(1e-3, sec * 2.0))))));
    }
private:

    // ---------------------------------------------------------------- Filter
    void setupFilter(const TrackParams& p)
    {
        if (!p.filterOn) return;
        {   // the bands only need working out again when a setting they depend on changes
            const float key[8] = {p.cutoff, p.reso, p.fDecayMs, (float)p.fPitch, (float)p.slope, p.fTone, (float)p.scale, sr_};
            if (std::memcmp(key, filterKey_, sizeof key) == 0) return;
            std::memcpy(filterKey_, key, sizeof key);
        }
        // A bank of up to 32 resonant bands, each tuned to a note of the Scale (moved by Pitch),
        // spread from C1 to E9. Cutoff, Slope (low / band / high), Reso and Tone set how loud each
        // band is. With a short Decay the bands are wide and overlap, so the bank passes the sound
        // through, shaped; a longer Decay narrows them until each one rings at its note.
        const int sc = clamp(p.scale, 0, 3);
        int notes[112], n = 0;
        for (int m = 24; m <= 124; ++m)
        {
            const int d = ((m - p.fPitch) % 12 + 12) % 12;
            for (int j = 0; j < kScaleLen[sc]; ++j)
                if (kScales[sc][j] == d) { notes[n++] = m; break; }
        }
        nBands_ = std::min(kBands, n);
        const float spacing = std::fmax(1.0f, (float)(notes[n - 1] - notes[0]) / (float)(nBands_ - 1));   // semitones
        const float qWide = 0.9f * 17.3f / spacing;          // neighbours overlap: the bank passes sound through
        const float lc = std::log2(clamp(p.cutoff, 20.0f, 20000.0f));
        for (int b = 0; b < nBands_; ++b)
        {
            const int note = notes[(int)std::lround((float)b * (float)(n - 1) / (float)(nBands_ - 1))];
            const float f = mpc::dsp::midiToHz((float)note);
            if (f > 0.42f * sr_) { bandTarget_[b] = 0.0f; continue; }
            const float qRing = kPi * f * p.fDecayMs * 0.001f / 6.91f;   // rings for about Decay
            const float q = clamp(std::fmax(qWide, qRing), 0.5f, 3000.0f);
            band_[b].set(gFor(f, sr_), 1.0f / q);
            bandK_[b] = 1.0f / q;                             // each band peaks at unity
            const float x = std::log2(f) - lc;                // octaves from the cutoff
            float shape = p.slope == 0 ? 1.0f / (1.0f + std::exp(4.0f * x))
                        : p.slope == 1 ? std::exp(-1.5f * x * x)
                                       : 1.0f / (1.0f + std::exp(-4.0f * x));
            shape += p.reso * 6.0f * std::exp(-x * x * 4.0f);   // Reso: the bands round the cutoff stand out
            const float tilt = std::exp2(p.fTone * 0.5f * std::log2(f / 1000.0f));   // Tone: +-3 dB an octave
            const float ring = std::fmin(12.0f, std::sqrt(q / qWide));   // narrow bands catch less: make up for it
            bandTarget_[b] = 0.6f * shape * tilt * ring;
        }
        for (int b = nBands_; b < kBands; ++b) bandTarget_[b] = 0.0f;
        fTiltA_ = onePoleA(700.0f, sr_);
    }

    float filterTick(float x, const TrackParams& p)
    {
        float y = 0.0f, lp, bp;
        for (int b = 0; b < nBands_; ++b)
        {
            bandW_[b] += 0.004f * (bandTarget_[b] - bandW_[b]);   // glide band levels: no zipper noise
            band_[b].tick(x, lp, bp);
            y += bp * bandK_[b] * bandW_[b];
        }
        y = fastTanh(y);   // ringing bands can pile up: keep them in bounds, warmly
        return x + p.fWet * (y - x);
    }

    // ---------------------------------------------------------------- Color
    void setupColor(const TrackParams& p)
    {
        driveK_ = 1.0f + p.drive * p.drive * 24.0f;
        driveNorm_ = 1.0f / mpc::dsp::fastTanh(driveK_);
        compRatio_ = 1.0f + p.comp * 9.0f;
        compThr_ = mpc::dsp::dbToGain(-6.0f - p.comp * 24.0f);
        compMakeup_ = std::pow(1.0f / compThr_, (1.0f - 1.0f / compRatio_) * 0.6f);
        compAtk_ = onePoleA(1.0f / 0.005f / (2.0f * kPi), sr_);
        compRel_ = onePoleA(1.0f / 0.12f / (2.0f * kPi), sr_);
        crushLevels_ = std::pow(2.0f, 16.0f - p.crush * 13.0f) * 0.5f;
        crushHoldN_ = 1 + (int)std::lround(p.crush * p.crush * 18.0f);
        noiseDecay_ = std::exp(-1.0f / std::fmax(1.0f, p.nDecayMs * 0.001f * sr_ / 6.91f));
        noiseA_ = onePoleA(300.0f * std::pow(50.0f, p.nTone), sr_);
        cTiltA_ = onePoleA(700.0f, sr_);
    }

    float colorTick(float x, const TrackParams& p)
    {
        float y = x;
        // noise burst, struck by each note, shaped by its own decay and tone
        noiseEnv_ *= noiseDecay_;
        if (p.noise > 0.001f)
        {
            noiseLp_ += noiseA_ * (rng_.nextBipolar() - noiseLp_);
            y += noiseLp_ * noiseEnv_ * p.noise * 0.6f;
        }
        if (p.drive > 0.001f) y = mpc::dsp::fastTanh(y * driveK_) * driveNorm_;
        if (p.comp > 0.001f)
        {
            const float a = std::fabs(y);
            compEnv_ += (a > compEnv_ ? compAtk_ : compRel_) * (a - compEnv_);
            if (--compCount_ <= 0)
            {
                compCount_ = 16;
                compTarget_ = compEnv_ > compThr_ ? std::pow(compThr_ / compEnv_, 1.0f - 1.0f / compRatio_) : 1.0f;
            }
            compGain_ += 0.01f * (compTarget_ - compGain_);
            y *= compGain_ * compMakeup_;
        }
        if (p.crush > 0.001f)
        {
            if (--crushCount_ <= 0) { crushCount_ = crushHoldN_; crushHold_ = std::round(y * crushLevels_) / crushLevels_; }
            y = crushHold_;
        }
        if (std::fabs(p.tilt) > 0.001f)
        {
            cTiltLp_ += cTiltA_ * (y - cTiltLp_);
            const float hp = y - cTiltLp_;
            y = cTiltLp_ * (1.0f - 0.7f * p.tilt) + hp * (1.0f + 0.7f * p.tilt);
        }
        return x + p.cWet * (y - x);
    }

    // ---------------------------------------------------------------- Space
    void setupSpace(const TrackParams& p)
    {
        const float target = clamp(p.timeMs * 0.001f * sr_, 4.0f, (float)delL_.maxDelay() - 4.0f);
        if (delayTime_ < 0.0f) delayTime_ = target;
        delayTarget_ = target;
        // tone: below centre darkens the repeats and the room, above centre thins them
        const float t = p.sTone;
        dampA_ = onePoleA(t < 0.0f ? 16000.0f * std::pow(2.0f, t * 4.0f) : 16000.0f, sr_);
        hpA_ = onePoleA(t > 0.0f ? 20.0f * std::pow(2.0f, t * 7.0f) : 20.0f, sr_);
        {   // the rest only needs working out again when a setting it depends on changes
            const float key[6] = {p.sTone, p.size, p.sDecayMs, (float)p.revType, p.freeze ? 1.0f : 0.0f, sr_};
            if (std::memcmp(key, spaceKey_, sizeof key) == 0) return;
            std::memcpy(spaceKey_, key, sizeof key);
        }
        const float rs = 0.5f * sr_;   // the reverb's own (half) sample rate
        // The reverb: Size scales every line from a small room (x0.5) to a vast space (x4);
        // Decay sets how long the tail takes to fade by 60 dB.
        const float scale = kMinScale * std::pow(kMaxScale / kMinScale, p.size) * rs / 44100.0f;
        const float t60 = std::fmax(0.1f, p.sDecayMs * 0.001f);
        // Tone: below centre the highs die away much sooner (dark); above centre they last nearly
        // as long as the mids, and the lows fade sooner instead (thin)
        const RevType& ty = kRevTypes[clamp(p.revType, 0, kNumRevTypes - 1)];
        const float toneHf = p.sTone < 0.0f ? 0.5f * std::pow(2.0f, p.sTone * 2.0f) : 0.5f + 0.45f * p.sTone;
        const float hfRatio = std::fmin(1.0f, toneHf * ty.hf / 0.5f);
        const float lowRatio = (p.sTone > 0.0f ? 1.0f - 0.75f * p.sTone : 1.0f) * ty.lo;
        inG_ = ty.inG; loopG_ = ty.loopG;
        revGain_ = kRevGain[clamp(p.revType, 0, kNumRevTypes - 1)];
        swellLen_ = ty.attackSec * rs;
        for (int i = 0; i < kLines; ++i)
        {
            const float spreadLen = kTankMean + (kTankLen[i] - kTankMean) * ty.spread;
            tankLen_[i] = (ty.ratioLines ? kRippleLen[i] : spreadLen) * ty.lenMul * scale;
            loopApLen_[i] = kLoopApLen[i] * scale;
            // the gain per trip round the loop, for its whole length (line + in-loop smear)
            const float pass = tankLen_[i] + loopApLen_[i];
            tankG_[i] = p.freeze ? 0.99995f : std::pow(10.0f, -3.0f * pass / (t60 * rs));
            // highs and lows fade at their own pace: per trip they lose exactly what makes their
            // tail hfRatio (or lowRatio) times as long as the mids'
            hiG_[i] = p.freeze ? 1.0f : std::pow(10.0f, -3.0f * pass / (t60 * hfRatio * rs)) / tankG_[i];
            loG_[i] = p.freeze ? 1.0f : std::pow(10.0f, -3.0f * pass / (t60 * lowRatio * rs)) / tankG_[i];
        }
        const float inScale = std::fmin(scale, 2.0f * rs / 44100.0f) * ty.inLen;   // longer = slower attack
        for (int i = 0; i < 4; ++i) inApLen_[i] = kInApLen[i] * inScale;
        // the slow drift of every line: deeper in bigger spaces, and as deep as the type wants
        modDepth_ = std::fmin(kModMaxSec, (0.0004f + 0.0026f * p.size) * ty.mod) * rs;
        // the band edges for the highs and lows (above): 2.5 kHz and 200 Hz
        hiA_ = onePoleA(2500.0f, rs);
        loA_ = onePoleA(200.0f, rs);
    }

    void spaceTick(float x, const TrackParams& p, float& outL, float& outR)
    {
        // Chorus: two copies of the sound, each delayed by ~12 ms that a slow wave stretches and
        // squeezes. The right copy's wave runs up to half a cycle behind the left's (Width), which
        // spreads it across the stereo field. Amount and depth glide, so turning them never clicks.
        float cl = x, cr = x;
        chMix_ += 0.0015f * (p.chorus - chMix_);
        chDepthS_ += 0.0015f * (p.chDepth - chDepthS_);
        chorusLine_.write(x);
        if (chMix_ > 0.0005f)
        {
            const double tau = 6.283185307179586;
            const float base = 0.012f * sr_, swing = (0.0005f + 0.0075f * chDepthS_) * sr_;
            const float lfoL = fastSin01(chPhase_);
            const float lfoR = fastSin01(chPhase_ + 0.5 * p.chWidth);
            const float wl = chorusLine_.readLinear(base + swing * lfoL), wr = chorusLine_.readLinear(base + swing * lfoR);
            // Mix: the dry sound and the chorus each stay at full level up to the middle, then the
            // other one fades out (as the reverb's Mix does)
            const float dry = std::fmin(1.0f, 2.0f * (1.0f - chMix_)), wet = std::fmin(1.0f, 2.0f * chMix_) * 0.8f;
            cl = x * dry + wl * wet;
            cr = x * dry + wr * wet;
        }
        chPhase_ += clamp(p.chRate, 0.01f, 10.0f) / sr_;
        if (chPhase_ >= 1.0) chPhase_ -= 1.0;

        float dl = 0.0f, dr = 0.0f;
        dMixS_ += 0.0015f * (p.delay - dMixS_);
        if (p.delay > 0.001f || dMixS_ > 0.001f)
        {
            delayTime_ += 0.0005f * (delayTarget_ - delayTime_);
            float rl = delL_.readLinear(delayTime_), rr = delR_.readLinear(delayTime_);
            dampL_ += dampA_ * (rl - dampL_); dampR_ += dampA_ * (rr - dampR_);
            hpL_ += hpA_ * (dampL_ - hpL_); hpR_ += hpA_ * (dampR_ - hpR_);
            rl = dampL_ - hpL_; rr = dampR_ - hpR_;
            const float fb = p.feedback * 0.95f, s = p.spread;
            const float in = x;
            delL_.write(mpc::dsp::softClip(in + fb * ((1.0f - s) * rl + s * rr)));
            delR_.write(mpc::dsp::softClip(in * (1.0f - s) + fb * ((1.0f - s) * rr + s * rl)));
            dl = rl; dr = rr;
        }
        else { delL_.write(0.0f); delR_.write(0.0f); }

        float wl = 0.0f, wr = 0.0f;
        const bool reverbOn = (p.reverb > 0.001f && p.revMix > 0.001f) || p.freeze;
        if (reverbOn || reverbAwake_ > 0)
        {
            // Lengths glide when Size changes, so the tail bends rather than clicks.
            auto glide = [](float& v, float target) { v = v < 0.0f ? target : v + 0.0004f * (target - v); };
            float in = p.freeze ? 0.0f : (x + 0.5f * (dl + dr)) * p.reverb;
            // The reverb runs at half the sample rate (every other sample), which halves its cost;
            // its tail is darkened above 2.5 kHz anyway, so nothing audible is lost.
            revOdd_ = !revOdd_;
            if (revOdd_) revHeld_ = in;
            else
            {
            in = 0.5f * (in + revHeld_);
            // the swell: the sound arrives as copies spread over the type's attack time, getting
            // louder towards the end, so it fades into the space instead of starting at once
            swell_.write(in);
            if (swellLen_ > 1.0f)
            {
                float acc = 0.0f;
                for (int k = 0; k < kSwellTaps; ++k)
                    acc += swell_.readInt((int)(swellLen_ * (float)(k + 1) / kSwellTaps)) * (float)(k + 1);
                in = acc * (2.0f / (kSwellTaps * (kSwellTaps + 1))) * 2.2f;
            }
            for (int i = 0; i < 4; ++i)
            {   // input diffusion: smears the attack into a wash before it enters the space
                glide(inApLenS_[i], inApLen_[i]);
                const float d = inAp_[i].readLinear(inApLenS_[i]);
                const float v = in + inG_ * d;
                inAp_[i].write(v);
                in = d - inG_ * v;
            }
            float o[kLines];
            for (int i = 0; i < kLines; ++i)
            {
                glide(tankLenS_[i], tankLen_[i]);
                glide(loopApLenS_[i], loopApLen_[i]);
                // each line drifts slowly at its own rate: no metallic ringing, a lush moving tail
                const float mod = modDepth_ * (0.5f + 0.5f * fastSin01(lfoPh_[i]));
                lfoPh_[i] += kLfoHz[i] * 2.0 / sr_;
                if (lfoPh_[i] >= 1.0) lfoPh_[i] -= 1.0;
                float y = tank_[i].readAllpass(tankLenS_[i] + mod);   // allpass interpolation: no dulling, pass after pass
                damp_[i] += hiA_ * (y - damp_[i]);       // below 2.5 kHz
                lowCut_[i] += loA_ * (y - lowCut_[i]);   // below 200 Hz
                y = lowCut_[i] * loG_[i] + (damp_[i] - lowCut_[i]) + (y - damp_[i]) * hiG_[i];
                const float d = loopAp_[i].readInt((int)(loopApLenS_[i] + 0.5f));  // smear inside the loop:
                const float v = y + loopG_ * d;                          // it grows denser as it fades
                loopAp_[i].write(v);
                o[i] = (d - loopG_ * v) * tankG_[i];
            }
            // mix every line into every other (an 8x8 Hadamard matrix, energy-preserving)
            float h[kLines];
            for (int i = 0; i < kLines; ++i) h[i] = o[i];
            for (int len = 1; len < kLines; len <<= 1)
                for (int i = 0; i < kLines; i += len << 1)
                    for (int j = i; j < i + len; ++j) { const float a = h[j], b = h[j + len]; h[j] = a + b; h[j + len] = a - b; }
            static const float inSign[kLines] = {1, -1, 1, 1, -1, 1, -1, -1};
            for (int i = 0; i < kLines; ++i)   // a soft limit keeps a frozen or huge tail from ever running away
                tank_[i].write(mpc::dsp::softClip(h[i] * 0.35355339f + in * inSign[i] * 0.5f));
            // left and right hear different blends of the lines: a wide tail from a mono input
            static const float sL[kLines] = {1, 1, 1, 1, -1, -1, 1, -1}, sR[kLines] = {1, -1, 1, -1, 1, 1, -1, -1};
            revGainS_ += 0.0005f * (revGain_ - revGainS_);   // glides when Type changes
            const float lvl = (p.freeze ? std::fmax(p.reverb, 0.35f) : 1.0f) * 0.42f * revGainS_;
            for (int i = 0; i < kLines; ++i) { wl += o[i] * sL[i]; wr += o[i] * sR[i]; }
            wl *= lvl; wr *= lvl;
            revPrevL_ = revCurL_; revCurL_ = wl; revPrevR_ = revCurR_; revCurR_ = wr;
            }
            // back to the full rate: halfway between the last two outputs, then the newest
            wl = revOdd_ ? revCurL_ : 0.5f * (revCurL_ + revPrevL_);
            wr = revOdd_ ? revCurR_ : 0.5f * (revCurR_ + revPrevR_);
            // keep running a while after the send closes, so the tail rings out
            reverbAwake_ = reverbOn ? (int)(t60Samples(p) + sr_) : reverbAwake_ - 1;
        }
        // reverb Mix: the dry side (with chorus and delay) and the reverb each stay at full level
        // up to the middle, then the other one fades out; it glides, so turning it never clicks
        revMixS_ = revMixS_ < 0.0f ? p.revMix : revMixS_ + 0.0015f * (p.revMix - revMixS_);
        const float dryG = std::fmin(1.0f, 2.0f * (1.0f - revMixS_)), wetG = std::fmin(1.0f, 2.0f * revMixS_);
        // delay Mix: the same shape, between the (chorused) dry sound and the echoes
        const float dDry = std::fmin(1.0f, 2.0f * (1.0f - dMixS_)), dWet = std::fmin(1.0f, 2.0f * dMixS_);
        outL = (cl * dDry + dl * dWet) * dryG + wl * wetG;
        outR = (cr * dDry + dr * dWet) * dryG + wr * wetG;
    }

    // The reverb: 8 lines (lengths at 44.1 kHz and Size x1, chosen so their echoes never line up),
    // a smear inside each loop, 4 smears on the way in, and a slow drift per line.
    static constexpr int kLines = 8;
    static constexpr float kTankLen[kLines] = {743.0f, 937.0f, 1153.0f, 1381.0f, 1621.0f, 1873.0f, 2143.0f, 2423.0f};
    static constexpr float kLoopApLen[kLines] = {131.0f, 173.0f, 211.0f, 257.0f, 307.0f, 353.0f, 401.0f, 449.0f};
    static constexpr float kInApLen[4] = {142.0f, 107.0f, 379.0f, 277.0f};
    static constexpr float kLfoHz[kLines] = {0.11f, 0.17f, 0.23f, 0.29f, 0.37f, 0.43f, 0.53f, 0.61f};
    static constexpr float kMinScale = 0.5f, kMaxScale = 4.0f, kModMaxSec = 0.004f;
    // Ripple's lines are in simple ratios to each other, so their echoes fall into patterns
    static constexpr float kRippleLen[kLines] = {600.0f, 750.0f, 900.0f, 1200.0f, 1500.0f, 1800.0f, 2250.0f, 3000.0f};
    // The reverb types. lenMul: how long the lines are; inG/inLen: how hard and how long the
    // attack is smeared (long = a slow swell); loopG: smear inside the loop (low = clear echoes
    // that thicken slowly, high = a dense wash at once); mod: how deep the drift; hf/lo: how long
    // highs and lows last against the mids (0.5 / 1 = neutral).
    struct RevType { float lenMul, spread, inG, inLen, loopG, mod, hf, lo; bool ratioLines; float attackSec; };
    // spread: how different the 8 line lengths are (1 = fully spread, a dense wash at once; near 0 =
    // nearly equal, so their echoes line up into clear repeats that blur a little more each time)
    // attackSec: how long the sound takes to swell into the reverb (0 = at once)
    static constexpr int kNumRevTypes = 8;
    static constexpr RevType kRevTypes[kNumRevTypes] = {
        {1.0f, 1.00f, 0.70f, 1.0f, 0.50f, 0.6f, 0.45f, 1.0f, false, 0.0f},    // Hall: fast, dense, darkens naturally
        {0.6f, 1.00f, 0.75f, 0.6f, 0.60f, 0.4f, 0.90f, 1.0f, false, 0.0f},    // Plate: instant, very dense, bright
        {1.3f, 1.00f, 0.80f, 3.0f, 0.60f, 1.0f, 0.60f, 1.0f, false, 0.35f},   // Bloom: slow swell, long, lush
        {1.7f, 1.00f, 0.82f, 5.0f, 0.65f, 1.6f, 0.55f, 1.0f, false, 0.8f},    // Cloud: slowest swell, enormous
        {3.5f, 0.05f, 0.30f, 0.5f, 0.15f, 0.5f, 0.60f, 0.9f, false, 0.0f},    // Echoes: clear repeats that thicken
        {5.0f, 0.04f, 0.30f, 1.0f, 0.12f, 0.4f, 0.50f, 0.8f, false, 0.12f},   // Canyon: very long repeats
        {1.0f, 1.00f, 0.40f, 0.5f, 0.25f, 0.2f, 0.80f, 1.0f, true, 0.0f},     // Ripple: patterns and resonances
        {2.6f, 0.35f, 0.45f, 1.0f, 0.40f, 1.2f, 0.70f, 1.0f, false, 0.1f},    // Vast: sparse, builds to huge
    };
    static constexpr float kTankMean = 1534.25f;
    // per-type output level, so changing Type does not jump in loudness (measured on a noise burst)
    static constexpr float kRevGain[kNumRevTypes] = {1.0f, 0.62f, 1.3f, 1.9f, 1.2f, 2.1f, 1.0f, 1.75f};   // the average of kTankLen
    static constexpr int kSwellTaps = 8;
    static constexpr float kSwellMaxSec = 1.0f;
    float t60Samples(const TrackParams& p) const { return std::fmax(0.1f, p.sDecayMs * 0.001f) * sr_; }

    float sr_ = 44100.0f;
    int index_ = 0;
    Voice voices_[kVoicesPerTrack];
    uint32_t order_ = 0;
    float level_ = -1.0f;
    float mixGL_ = 1.0f, mixGR_ = 1.0f, meterPeak_ = 0.0f;
    int lastN_ = 0, lastLoop_ = 1;                 // the last block's region, for playPosition()
    double lastRegionStart_ = 0.0, lastRegionLen_ = 1.0, lastRotOff_ = 0.0;
    Region cur_, old_;          // the loop region playing, and the one fading out after a change
    float regX_ = 0.0f;
    float pFade_ = 0.0f;        // how much of the pitch-shifted read is heard (0 = plain, 1 = shifted)
    double lastPRatio_ = 1.0;   // the shift in use (kept while it fades out after Pitch goes to 0)         // how much of the old region is still heard (1 -> 0 over ~15 ms)
    bool lastSpeedNeg_ = false;
    float runGain_ = 0.0f;      // the SYNC loop's fade in/out
    double runPhase_ = -1.0;    // where the SYNC loop is (0..1 of the region); -1 = not running
    double oldPhase_ = 0.0;
    PitchShift runPs_;          // PITCH's read heads for the SYNC loop     // where it was before a jump, faded out over 10 ms
    float xfade_ = 0.0f;
    double lastHostPpq_ = -1.0; // the last song position the host gave
    int farCount_ = 0;          // fresh readings in a row that were far off
    int silentFrames_ = 0;

    // grain
    std::vector<float> grainBuf_;
    int writePos_ = 0;
    float cols_[64] = {}, colPeak_ = 0.0f;   // the recording's picture (kGrainCols)
    float colPos_[64] = {};                  // where in the sample each column came from (-1 = silence)
    float playPosNow_ = -1.0f;
    float specPre_[2048] = {}, specPost_[2048] = {};   // kSpecN
    int specPos_ = 0;
    int colHead_ = 0;                        // the oldest column
    int grainFilled_ = 0;                    // samples recorded since the start (up to the buffer's size)
    // Grain engine state (Grainbloom)
    long long gWrite_ = 0, gRecorded_ = 0;
    int gMode_ = 1;
    float gSizeSamp_ = 4000.0f, gStepSamp_ = 10000.0f, gOverlap_ = 1.0f, gGain_ = 1.0f;
    float gFbTarget_ = 0.0f, fbS_ = 0.0f, lastFb_ = 0.0f;
    double samplesToNext_ = 0.0, stutterAnchor_ = -1.0, shatterAnchor_ = 0.0, arpAnchor_ = -1.0;
    bool stutterReverse_ = false, shatterMute_ = false, shatterReverse_ = false;
    int shatterLeft_ = 0, shatterHits_ = 1, arpStep_ = 0;
    float shatterRate_ = 1.0f;
    static constexpr int kWinSize = 1024;
    float grainWin_[1026] = {};                  // the grain window (Hann), kWinSize + 2 entries
    static constexpr int kSpawnMemory = 12;      // recent grain start points, for the screen
    float spawnBack_[kSpawnMemory] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int spawnHead_ = 0;
    bool lockedLast_ = false;
    Grain grains_[kMaxGrains];

    // filter
    static constexpr int kBands = 32;
    Svf band_[kBands];
    float bandK_[kBands] = {}, bandW_[kBands] = {}, bandTarget_[kBands] = {};
    int nBands_ = 0;
    float filterKey_[8] = {-1, -1, -1, -1, -1, -1, -1, -1}, spaceKey_[6] = {-1, -1, -1, -1, -1, -1};
    float fTiltLp_ = 0.0f, fTiltA_ = 0.1f;

    // color
    float driveK_ = 1.0f, driveNorm_ = 1.0f;
    float compRatio_ = 1.0f, compThr_ = 1.0f, compMakeup_ = 1.0f, compAtk_ = 0.1f, compRel_ = 0.01f;
    float compEnv_ = 0.0f, compGain_ = 1.0f, compTarget_ = 1.0f; int compCount_ = 0;
    float crushLevels_ = 32768.0f, crushHold_ = 0.0f; int crushHoldN_ = 1, crushCount_ = 0;
    float noiseEnv_ = 0.0f, noiseDecay_ = 0.99f, noiseLp_ = 0.0f, noiseA_ = 0.5f;
    float cTiltLp_ = 0.0f, cTiltA_ = 0.1f;

    // space
    mpc::dsp::DelayLine delL_, delR_, chorusLine_;
    mpc::dsp::DelayLine tank_[kLines], loopAp_[kLines], inAp_[4];
    float tankLen_[kLines] = {}, loopApLen_[kLines] = {}, tankG_[kLines] = {}, inApLen_[4] = {};
    float tankLenS_[kLines] = {}, loopApLenS_[kLines] = {}, inApLenS_[4] = {};   // the gliding lengths
    float damp_[kLines] = {}, lowCut_[kLines] = {};
    double lfoPh_[kLines] = {};
    float modDepth_ = 0.0f, hiA_ = 0.0f, loA_ = 0.0f, hiG_[kLines] = {}, loG_[kLines] = {};
    int reverbAwake_ = 0;
    bool revOdd_ = false;
    float revHeld_ = 0.0f, revCurL_ = 0.0f, revCurR_ = 0.0f, revPrevL_ = 0.0f, revPrevR_ = 0.0f;
    float revMixS_ = -1.0f, dMixS_ = 0.0f;
    float inG_ = 0.7f, loopG_ = 0.5f;   // the type's smear gains
    mpc::dsp::DelayLine swell_;          // the type's slow attack
    float swellLen_ = 0.0f;
    float revGain_ = 1.0f, revGainS_ = 1.0f;   // -1: not set yet (starts at the setting, then glides)
    double chPhase_ = 0.0;                    // the chorus wave, 0..1
    float chMix_ = 0.0f, chDepthS_ = 0.0f;    // chorus amount and depth, glided
    float delayTime_ = -1.0f, delayTarget_ = 1000.0f;
    float dampA_ = 1.0f, hpA_ = 0.0f, dampL_ = 0, dampR_ = 0, hpL_ = 0, hpR_ = 0;

    mpc::dsp::Xorshift32 rng_;
};

constexpr float Track::kTankLen[Track::kLines];
constexpr float Track::kLoopApLen[Track::kLines];
constexpr float Track::kInApLen[4];
constexpr float Track::kLfoHz[Track::kLines];
constexpr float Track::kRippleLen[Track::kLines];
constexpr Track::RevType Track::kRevTypes[Track::kNumRevTypes];
constexpr float Track::kTankMean;
constexpr float Track::kRevGain[Track::kNumRevTypes];

} // namespace sculpt

#endif // SCULPT_ENGINE_H
