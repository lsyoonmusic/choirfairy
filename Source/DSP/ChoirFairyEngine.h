#pragma once
#include "PitchTrackerYIN.h"
#include "TrackingLowCut.h"
#include "HarmonicAnalyzer20.h"
#include "ChoirDoubler.h"
#include <cmath>
#include <algorithm>

namespace choirfairy
{

enum class AuditionMode : int
{
    Normal     = 0, // Dry Center + Wet Stereo Choir Doubles
    WetOnly    = 1, // Wet Stereo Doubles ONLY (Sines + High-Band Air)
    SinesOnly  = 2, // Resynthesized 20-Harmonic Sine Choir ONLY
    AirOnly    = 3  // Timing-Offset High-Band Original Air ONLY
};

struct EngineParams
{
    // Pitch Tracker
    double minF0Hz = 75.0;
    double maxF0Hz = 680.0;

    // Pitch-Tracking Surgical Low-Cut
    LowCutSlope lowCutSlope = LowCutSlope::dB36;
    double lowCutOffsetSt   = -3.5;  // -12 .. 0 semitones below f0
    double lowCutFloorHz    = 60.0;  // Resting cutoff when unvoiced

    // 20-Harmonic Stereo Choir Doubler
    int    numHarmonics = 20;        // 4 .. 20
    double detuneCents  = 11.0;      // 0 .. 35 cents (+/- L/R)
    double timingMs     = 16.0;      // 2 .. 35 ms asymmetric L/R Haas delay
    double driftAmount  = 0.45;      // 0 .. 1 organic vocal pitch wander
    double airBlend     = 0.85;      // 0 .. 1.5 high-band original stereo air level
    double tiltDb       = -1.5;      // -6 .. +6 dB harmonic brightness tilt
    double stereoWidth  = 1.15;      // 0 .. 1.5 M/S width on wet doubles
    double doublerMix   = 0.42;      // 0 .. 1 wet doubler blend amount

    // Utility
    AuditionMode audition = AuditionMode::Normal;
    double outputGainDb   = 0.0;
    bool   bypass         = false;
};

class ChoirFairyEngine
{
public:
    void prepare(double sampleRate)
    {
        fs = std::max(sampleRate, 8000.0);
        tracker.prepare(fs);
        lowCut.prepare(fs);
        analyzer.prepare(fs);
        doubler.prepare(fs);
        reset();
    }

    void reset()
    {
        tracker.reset();
        lowCut.reset();
        analyzer.reset();
        doubler.reset();
    }

    inline void processStereoSample(double& left, double& right, const EngineParams& p) noexcept
    {
        const double dryInL = left;
        const double dryInR = right;

        // Always keep pitch tracker alive so un-bypassing is instant
        const double rawMono = 0.5 * (dryInL + dryInR);
        const bool jumped = tracker.processSample(rawMono, p.minF0Hz, p.maxF0Hz);

        if (p.bypass)
            return;

        const double f0   = tracker.getTrackedF0();
        const double conf = tracker.getConfidence();

        // 1. Surgical Pitch-Tracking Low-Cut (up to 96 dB/oct!) cleans rumble before dry & wet
        double cleanL = dryInL;
        double cleanR = dryInR;
        lowCut.processStereoSample(cleanL, cleanR, p.lowCutSlope, f0, conf, p.lowCutOffsetSt, p.lowCutFloorHz);

        // 2. 20-Harmonic Spectral Peak Envelope Extraction on clean post-low-cut mono stream
        const double cleanMono = 0.5 * (cleanL + cleanR);
        analyzer.processSample(cleanMono, f0, conf, p.numHarmonics, jumped);

        // On confirmed melodic jump: trigger Bank A <-> Bank B crossfade (ZERO pitch-sweep warble!)
        if (jumped)
            doubler.triggerPitchJump(f0, analyzer.getSmoothedAmps());

        // 3. Stereo 20-Harmonic Resynthesis Doubler + Timing-Offset High-Band Original Air
        const auto dOut = doubler.processStereoSample(
            cleanL, cleanR, f0, conf,
            p.numHarmonics, analyzer.getSmoothedAmps(),
            p.detuneCents, p.timingMs, p.driftAmount, p.tiltDb, p.airBlend
        );

        // Combine wet sines + wet high-band air & apply Stereo Width (M/S)
        double wetL = dOut.wetSinesL + dOut.wetAirL;
        double wetR = dOut.wetSinesR + dOut.wetAirR;

        const double wMid  = 0.5 * (wetL + wetR);
        const double wSide = 0.5 * (wetL - wetR) * std::clamp(p.stereoWidth, 0.0, 1.5);
        wetL = wMid + wSide;
        wetR = wMid - wSide;

        // 4. Audition Routing & Dry/Wet Blend
        //    In Normal mode: Dry stays solid center/original (zero latency!), Wet adds lush stereo doubles
        //    around it (with gentle dry compensation at high mix so output stays balanced).
        double outL = 0.0, outR = 0.0;
        const double mix = std::clamp(p.doublerMix, 0.0, 1.0);

        switch (p.audition)
        {
            case AuditionMode::Normal:
            {
                const double dryGain = 1.0 - 0.35 * mix;
                const double wetGain = 0.92 * mix;
                outL = dryGain * cleanL + wetGain * wetL;
                outR = dryGain * cleanR + wetGain * wetR;
                break;
            }
            case AuditionMode::WetOnly:
                outL = wetL;
                outR = wetR;
                break;
            case AuditionMode::SinesOnly:
                outL = dOut.wetSinesL;
                outR = dOut.wetSinesR;
                break;
            case AuditionMode::AirOnly:
                outL = dOut.wetAirL;
                outR = dOut.wetAirR;
                break;
        }

        const double outLin = std::pow(10.0, p.outputGainDb * 0.05);
        left  = outL * outLin;
        right = outR * outLin;
    }

    double getTrackedF0() const noexcept        { return tracker.getTrackedF0(); }
    double getConfidence() const noexcept       { return tracker.getConfidence(); }
    double getLowCutCutoffHz() const noexcept   { return lowCut.getCurrentCutoffHz(); }
    double getXoverHz() const noexcept          { return doubler.getCurrentXoverHz(); }
    double getHarmonicAmp(int k) const noexcept { return analyzer.getHarmonicAmp(k); }

private:
    double fs = 48000.0;
    PitchTrackerYIN    tracker;
    TrackingLowCut     lowCut;
    HarmonicAnalyzer20 analyzer;
    ChoirDoubler       doubler;
};

} // namespace choirfairy
