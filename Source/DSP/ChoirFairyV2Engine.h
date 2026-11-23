#pragma once
#include "FftRadix2.h"
#include "TptSvf.h"
#include "SpectralFrameAnalyzer.h"
#include "SmsChoirSynth.h"
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>

namespace choirfairy2
{

enum class AuditionMode : int
{
    Normal    = 0, // Dry Low-Cut Center + Stereo Choir Doubles + Air
    WetOnly   = 1, // Stereo Choir Doubles + Timing-Offset Air ONLY
    SinesOnly = 2, // 20-Harmonic SMS Sine Choir ONLY
    AirOnly   = 3  // Linear-Phase Timing-Offset Original Air ONLY
};

struct EngineParams
{
    // Pitch Range
    double minF0Hz = 75.0;
    double maxF0Hz = 680.0;

    // Surgical Pitch-Tracking Low-Cut
    LowCutSlope     lowCutSlope    = LowCutSlope::dB48;
    FilterPhaseMode lowCutPhase    = FilterPhaseMode::LinearPhase;
    double          lowCutOffsetSt = -3.0;  // -12 .. 0 semitones below f0
    double          lowCutFloorHz  = 60.0;  // Resting cutoff when unvoiced

    // 20-Harmonic SMS Stereo Choir Doubler
    int    numHarmonics   = 20;    // 4 .. 20
    bool   fourVoiceChoir = false; // false = 2-Voice Tight, true = 4-Voice Ensemble
    double detuneCents    = 11.0;  // 0 .. 35 cents
    double timingMs       = 16.0;  // 2 .. 35 ms asymmetric Haas offset
    double driftAmount    = 0.45;  // 0 .. 1 organic vocal pitch wander
    double airBlend       = 0.85;  // 0 .. 1.5 high-band original stereo air
    double tiltDb         = -1.5;  // -6 .. +6 dB harmonic brightness tilt
    double stereoWidth    = 1.15;  // 0 .. 1.5 M/S width on wet doubles
    double doublerMix     = 0.45;  // 0 .. 1 wet blend

    // Utility
    AuditionMode audition = AuditionMode::Normal;
    double outputGainDb   = 0.0;
    bool   bypass         = false;
};

/**
 * Choir Fairy v2 (HQ Studio Lookahead Edition) Core Stereo Engine.
 *
 * Timing & Lookahead Alignment (ZERO lag between dry, low-cut, choir & air!):
 *   - Frame size N = 4096 (@ 44.1/48 kHz) or 8192 (@ 88.2/96 kHz), Hop H = N / 8.
 *   - Total engine latency = EXACTLY N samples (~85.3 ms @ 48/96 kHz), reported to DAW PDC.
 *   - At input sample `n`, WOLA outputs the linear-phase processed sample from `n - N`.
 *   - Simultaneously, the latest STFT analysis frame is centered at `n - N/2` - giving
 *     4 FULL HOPS (~42.7 ms) of FREE FUTURE LOOKAHEAD for:
 *       1) Non-causal 5-frame pitch continuity & single-hop glitch elimination!
 *       2) Pre-emptive McAulay-Quatieri Partial Birth/Death on exact melodic jumps!
 *       3) Zero-lag surgical Linear-Phase OR Min-Phase Low-Cut up to 96 dB/oct + Brickwall!
 */
class ChoirFairyV2Engine
{
public:
    static constexpr int kQueueSize = 16; // Power of 2; holds >= 8 hops of lookahead history

    void prepare(double sampleRate)
    {
        fs = std::max(sampleRate, 8000.0);
        // Keep ~85 ms lookahead & ~11.7 Hz/bin spectral resolution consistent across 44.1..96 kHz
        N = (fs > 64000.0) ? 8192 : 4096;
        H = N / 8;
        numBins = N / 2 + 1;
        olaNorm = (2.0 * static_cast<double>(H)) / static_cast<double>(N); // = 0.25 for 8x sqrt-Hann

        fft.prepare(N);
        analyzer.prepare(fs, N);
        smsSynth.prepare(fs, H);
        minPhaseLowCut.prepare(fs);

        // Periodic sqrt-Hann analysis/synthesis window (win^2 = periodic Hann -> exact COLA!)
        sqrtHann.resize(static_cast<size_t>(N));
        constexpr double twoPi = 6.2831853071795864769;
        for (int i = 0; i < N; ++i)
        {
            const double hann = 0.5 * (1.0 - std::cos(twoPi * static_cast<double>(i) / static_cast<double>(N)));
            sqrtHann[static_cast<size_t>(i)] = std::sqrt(std::max(0.0, hann));
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            inRing[ch].assign(static_cast<size_t>(N), 0.0);
            rawDelayRing[ch].assign(static_cast<size_t>(N), 0.0);
            olaDryRing[ch].assign(static_cast<size_t>(N), 0.0);
            olaAirRing[ch].assign(static_cast<size_t>(N), 0.0);
            specRe[ch].assign(static_cast<size_t>(N), 0.0);
            specIm[ch].assign(static_cast<size_t>(N), 0.0);
        }

        monoTimeScratch.assign(static_cast<size_t>(N), 0.0);
        monoSpecRe.assign(static_cast<size_t>(N), 0.0);
        monoSpecIm.assign(static_cast<size_t>(N), 0.0);
        ifftRe.assign(static_cast<size_t>(N), 0.0);
        ifftIm.assign(static_cast<size_t>(N), 0.0);
        maskLc.assign(static_cast<size_t>(numBins), 1.0);
        maskAir.assign(static_cast<size_t>(numBins), 0.0);
        maskAirTotal.assign(static_cast<size_t>(numBins), 0.0);

        minPhaseCutAlpha = 1.0 - std::exp(-1.0 / (0.005 * fs));

        reset();
    }

    void reset()
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            std::fill(inRing[ch].begin(),       inRing[ch].end(),       0.0);
            std::fill(rawDelayRing[ch].begin(), rawDelayRing[ch].end(), 0.0);
            std::fill(olaDryRing[ch].begin(),   olaDryRing[ch].end(),   0.0);
            std::fill(olaAirRing[ch].begin(),   olaAirRing[ch].end(),   0.0);
        }

        inPos = 0;
        outPos = 0;
        hopCounter = 0;
        frameWriteIdx = 0;
        for (auto& f : frameQueue)
            f = SpectralFrame {};

        smsSynth.reset();
        minPhaseLowCut.reset();

        smoothLinearLcHz  = 60.0;
        smoothLinearAirHz = 3600.0;
        smoothMinPhaseLcHz = 60.0;

        lastValidF0 = 180.0;
        liveF0 = 180.0;
        liveConf = 0.0;
        liveLowCutHz = 60.0;
        liveAirXoverHz = 3600.0;
        liveHarmAmps.fill(0.0);
    }

    int getLatencySamples() const noexcept { return N; }

    inline void processStereoSample(double& left, double& right, const EngineParams& p) noexcept
    {
        // 1. Read latency-aligned raw audio (for Bypass & Min-Phase Low-Cut) and WOLA Linear-Phase outputs
        //    At input time `n`, rawDelayRing[inPos] holds the sample from `n - N` (exact match to WOLA!)
        const double delayedRawL = rawDelayRing[0][static_cast<size_t>(inPos)];
        const double delayedRawR = rawDelayRing[1][static_cast<size_t>(inPos)];

        const double wolaDryL = olaDryRing[0][static_cast<size_t>(outPos)];
        const double wolaDryR = olaDryRing[1][static_cast<size_t>(outPos)];
        const double wolaAirL = olaAirRing[0][static_cast<size_t>(outPos)];
        const double wolaAirR = olaAirRing[1][static_cast<size_t>(outPos)];

        olaDryRing[0][static_cast<size_t>(outPos)] = 0.0;
        olaDryRing[1][static_cast<size_t>(outPos)] = 0.0;
        olaAirRing[0][static_cast<size_t>(outPos)] = 0.0;
        olaAirRing[1][static_cast<size_t>(outPos)] = 0.0;

        // 2. Push current incoming sample into analysis & raw delay rings
        inRing[0][static_cast<size_t>(inPos)]       = left;
        inRing[1][static_cast<size_t>(inPos)]       = right;
        rawDelayRing[0][static_cast<size_t>(inPos)] = left;
        rawDelayRing[1][static_cast<size_t>(inPos)] = right;

        inPos  = (inPos  + 1) & (N - 1);
        outPos = (outPos + 1) & (N - 1);

        // 3. Every H samples, run centered 4096/8192-pt WOLA analysis + lookahead SMS frame dispatch!
        if (++hopCounter >= H)
        {
            hopCounter = 0;
            processHop(p);
        }

        // True latency-compensated bypass (zero click / zero timing shift when toggling bypass!)
        if (p.bypass)
        {
            left  = delayedRawL;
            right = delayedRawR;
            return;
        }

        // 4. Select Linear-Phase WOLA Low-Cut OR Lookahead-Guided Min-Phase Low-Cut for the Dry path
        double cleanDryL = wolaDryL;
        double cleanDryR = wolaDryR;

        // Track live smoothed cutoff for Min-Phase SVF using the exact lookahead-cleaned frame cutoff
        const double logCurMp = std::log(std::max(smoothMinPhaseLcHz, 15.0));
        const double logTgtMp = std::log(std::max(liveLowCutHz, 15.0));
        smoothMinPhaseLcHz = std::exp(logCurMp + minPhaseCutAlpha * (logTgtMp - logCurMp));

        if (p.lowCutPhase == FilterPhaseMode::MinPhase)
        {
            cleanDryL = delayedRawL;
            cleanDryR = delayedRawR;
            minPhaseLowCut.processStereoSample(cleanDryL, cleanDryR, p.lowCutSlope, smoothMinPhaseLcHz);
        }

        // 5. Synthesize 1 sample of Lookahead McAulay-Quatieri 20-Harmonic SMS Choir + Air Doubles
        const auto sOut = smsSynth.processSample(
            wolaAirL, wolaAirR,
            p.numHarmonics, p.fourVoiceChoir,
            p.detuneCents, p.timingMs, p.driftAmount, p.tiltDb, p.airBlend
        );

        // Combine wet harmonic choir + wet timing-offset high-band air & apply M/S Stereo Width
        double wetL = sOut.wetSinesL + sOut.wetAirL;
        double wetR = sOut.wetSinesR + sOut.wetAirR;

        const double wMid  = 0.5 * (wetL + wetR);
        const double wSide = 0.5 * (wetL - wetR) * std::clamp(p.stereoWidth, 0.0, 1.5);
        wetL = wMid + wSide;
        wetR = wMid - wSide;

        // 6. Audition Routing & Balanced Dry/Wet Mix
        double outL = 0.0, outR = 0.0;
        const double mix = std::clamp(p.doublerMix, 0.0, 1.0);

        switch (p.audition)
        {
            case AuditionMode::Normal:
            {
                const double dryGain = 1.0 - 0.35 * mix;
                const double wetGain = 0.92 * mix;
                outL = dryGain * cleanDryL + wetGain * wetL;
                outR = dryGain * cleanDryR + wetGain * wetR;
                break;
            }
            case AuditionMode::WetOnly:
                outL = wetL;
                outR = wetR;
                break;
            case AuditionMode::SinesOnly:
                outL = sOut.wetSinesL;
                outR = sOut.wetSinesR;
                break;
            case AuditionMode::AirOnly:
                outL = sOut.wetAirL;
                outR = sOut.wetAirR;
                break;
        }

        const double outLin = std::pow(10.0, p.outputGainDb * 0.05);
        left  = outL * outLin;
        right = outR * outLin;
    }

    double getTrackedF0() const noexcept        { return liveF0; }
    double getConfidence() const noexcept       { return liveConf; }
    double getLowCutCutoffHz() const noexcept   { return liveLowCutHz; }
    double getAirXoverHz() const noexcept       { return liveAirXoverHz; }
    double getHarmonicAmp(int k) const noexcept
    {
        return liveHarmAmps[static_cast<size_t>(std::clamp(k, 0, kMaxHarmonics - 1))];
    }

private:
    void processHop(const EngineParams& p) noexcept
    {
        // A. Gather chronological N-sample stereo frames [n - N + 1 .. n]
        //    Pack Left into Real and Right into Imag -> ONE complex FFT computes BOTH stereo spectra!
        for (int i = 0; i < N; ++i)
        {
            const int rIdx = (inPos + i) & (N - 1);
            const double w = sqrtHann[static_cast<size_t>(i)];
            const double l = inRing[0][static_cast<size_t>(rIdx)];
            const double r = inRing[1][static_cast<size_t>(rIdx)];

            ifftRe[static_cast<size_t>(i)] = l * w;
            ifftIm[static_cast<size_t>(i)] = r * w;

            const double mono = 0.5 * (l + r);
            monoTimeScratch[static_cast<size_t>(i)] = mono;
            monoSpecRe[static_cast<size_t>(i)] = mono * w * w;
            monoSpecIm[static_cast<size_t>(i)] = 0.0;
        }

        // B. Only TWO Forward FFTs (Mono Hann + Packed Stereo WOLA)!
        fft.forward(monoSpecRe.data(), monoSpecIm.data());
        fft.forward(ifftRe.data(), ifftIm.data());

        // Unpack L & R Hermitian spectra in O(N)
        specRe[0][0] = ifftRe[0]; specIm[0][0] = 0.0;
        specRe[1][0] = ifftIm[0]; specIm[1][0] = 0.0;
        const int nyq = N / 2;
        specRe[0][static_cast<size_t>(nyq)] = ifftRe[static_cast<size_t>(nyq)]; specIm[0][static_cast<size_t>(nyq)] = 0.0;
        specRe[1][static_cast<size_t>(nyq)] = ifftIm[static_cast<size_t>(nyq)]; specIm[1][static_cast<size_t>(nyq)] = 0.0;

        for (int b = 1; b < nyq; ++b)
        {
            const size_t bu = static_cast<size_t>(b);
            const size_t mu = static_cast<size_t>(N - b);
            const double zrP = ifftRe[bu], ziP = ifftIm[bu];
            const double zrM = ifftRe[mu], ziM = ifftIm[mu];

            // X_L(b) = 0.5 * (Z(b) + conj(Z(N-b))),  X_R(b) = -0.5j * (Z(b) - conj(Z(N-b)))
            specRe[0][bu] = 0.5 * (zrP + zrM);
            specIm[0][bu] = 0.5 * (ziP - ziM);
            specRe[1][bu] = 0.5 * (ziP + ziM);
            specIm[1][bu] = 0.5 * (zrM - zrP);
        }

        // C. Analyze centered frame (representing audio at t - N/2 = 4 hops in future!)
        SpectralFrame newFrame {};
        newFrame.rawF0   = lastValidF0;
        newFrame.cleanF0 = lastValidF0;
        analyzer.analyzeFrame(monoTimeScratch.data(), monoSpecRe.data(), monoSpecIm.data(),
                              p.minF0Hz, p.maxF0Hz, p.numHarmonics, newFrame);
        if (newFrame.confidence >= 0.20)
            lastValidF0 = newFrame.rawF0;

        // Compute target Low-Cut & High-Band Air Crossover for this centered frame
        const double floorHz = std::clamp(p.lowCutFloorHz, 18.0, 400.0);
        const double c = std::clamp(newFrame.confidence, 0.0, 1.0);
        const double voicedLc = std::clamp(newFrame.rawF0 * std::pow(2.0, p.lowCutOffsetSt / 12.0),
                                           floorHz, 1400.0);
        newFrame.lowCutHz = std::exp((1.0 - c) * std::log(floorHz) + c * std::log(voicedLc));

        const double voicedAir = std::clamp((static_cast<double>(p.numHarmonics) + 0.5) * newFrame.rawF0,
                                            350.0, 14500.0);
        const double unvoicedAir = 1400.0;
        newFrame.airXoverHz = std::exp((1.0 - c) * std::log(unvoicedAir) + c * std::log(voicedAir));

        // Store frame into lookahead queue
        frameWriteIdx = (frameWriteIdx + 1) & (kQueueSize - 1);
        frameQueue[static_cast<size_t>(frameWriteIdx)] = newFrame;

        // D. Smooth WOLA spectral filter frequencies gently per hop (zero frame-edge click!)
        smoothLinearLcHz  = std::exp(0.60 * std::log(std::max(smoothLinearLcHz, 15.0))
                                   + 0.40 * std::log(std::max(newFrame.lowCutHz, 15.0)));
        smoothLinearAirHz = std::exp(0.65 * std::log(std::max(smoothLinearAirHz, 200.0))
                                   + 0.35 * std::log(std::max(newFrame.airXoverHz, 200.0)));

        // E. Build Zero-Phase (Exact Linear-Phase!) Magnitude Masks for Low-Cut & High-Band Air
        buildLinearPhaseMasks(p.lowCutSlope, smoothLinearLcHz, smoothLinearAirHz);

        // F. Apply Linear-Phase Masks via PACKED STEREO IFFT (only TWO Inverse FFTs total!)
        //    1) Packed Stereo Dry Low-Cut: Z(b) = M(b) * (X_L(b) + j * X_R(b))
        applyStereoMaskAndIfft(maskLc.data(), olaDryRing[0].data(), olaDryRing[1].data());

        //    2) Packed Stereo High-Band Air: M_airTotal(b) = maskLc(b) * maskAir(b)
        for (int b = 0; b < numBins; ++b)
            maskAirTotal[static_cast<size_t>(b)] = maskLc[static_cast<size_t>(b)] * maskAir[static_cast<size_t>(b)];
        applyStereoMaskAndIfft(maskAirTotal.data(), olaAirRing[0].data(), olaAirRing[1].data());

        // G. NON-CAUSAL 5-FRAME LOOKAHEAD CLEANING FOR CURRENT PLAYBACK HOP (q[-4] -> q[-3])!
        //    Remember: WOLA is currently outputting audio from `n - N` (exact center of q[-4])
        //    moving to `n - N + H` (exact center of q[-3]), while q[-2], q[-1], q[0] are in the FUTURE!
        auto getQ = [&](int offsetFromLatest) -> SpectralFrame&
        {
            return frameQueue[static_cast<size_t>((frameWriteIdx + offsetFromLatest) & (kQueueSize - 1))];
        };

        // Clean pitch of frame at offset -3 using its exact centered neighbors (-4, -3, -2)!
        cleanFramePitchNonCausal(getQ(-4), getQ(-3), getQ(-2));

        const SpectralFrame& playStart = getQ(-4); // Exact playback time t = n - N
        const SpectralFrame& playEnd   = getQ(-3); // Exact future time   t = n - N + H

        // Dispatch exact zero-lag [playStart -> playEnd] hop to the McAulay-Quatieri SMS Choir!
        smsSynth.beginHop(playStart, playEnd);

        // Publish exact playback-synchronized telemetry to GUI
        liveF0         = playStart.cleanF0;
        liveConf       = playStart.confidence;
        liveLowCutHz   = playStart.lowCutHz;
        liveAirXoverHz = playStart.airXoverHz;
        liveHarmAmps   = playStart.partialAmp;
    }

    /**
     * Centered 3-frame non-causal pitch continuity filter:
     * Wipes out isolated single-hop glitches with ZERO lag relative to playback!
     */
    static void cleanFramePitchNonCausal(const SpectralFrame& prev,
                                         SpectralFrame& curr,
                                         const SpectralFrame& next) noexcept
    {
        if (curr.confidence < 0.20)
            return;

        const bool prevOk = (prev.confidence > 0.25);
        const bool nextOk = (next.confidence > 0.25);

        if (prevOk && nextOk)
        {
            const double dPrevNext = std::abs(12.0 * std::log2(next.rawF0 / std::max(prev.cleanF0, 20.0)));
            const double dPrevCurr = std::abs(12.0 * std::log2(curr.rawF0 / std::max(prev.cleanF0, 20.0)));
            // If prev & next agree closely (< 1.2 st) while curr jumped away (> 1.5 st), curr is a 1-hop glitch!
            if (dPrevNext < 1.2 && dPrevCurr > 1.5)
            {
                const double fixedF0 = 0.5 * (prev.cleanF0 + next.rawF0);
                const double ratio = fixedF0 / std::max(curr.rawF0, 20.0);
                curr.cleanF0 = fixedF0;
                for (int k = 0; k < kMaxHarmonics; ++k)
                    curr.partialFreq[static_cast<size_t>(k)] = fixedF0 * static_cast<double>(k + 1);
                // Re-anchor lowCutHz cleanly as well
                curr.lowCutHz = std::clamp(curr.lowCutHz * ratio, 18.0, 1400.0);
                return;
            }
        }
        curr.cleanF0 = curr.rawF0;
    }

    void buildLinearPhaseMasks(LowCutSlope slope, double fcLc, double fcAir) noexcept
    {
        const double binHz = fs / static_cast<double>(N);
        const int slopeIdx = static_cast<int>(slope);

        maskLc[0]  = (slopeIdx > 0) ? 0.0 : 1.0;
        maskAir[0] = 0.0;

        static constexpr int orders[6] = { 2, 4, 6, 8, 12, 16 };
        const bool isBrickwall = (slope == LowCutSlope::Brickwall);
        const int lcOrder = (slopeIdx >= 1 && slopeIdx <= 6) ? orders[slopeIdx - 1] : 16;

        for (int b = 1; b < numBins; ++b)
        {
            const double f = static_cast<double>(b) * binHz;

            // 1. Surgical Linear-Phase Low-Cut Mask
            if (slopeIdx == 0)
            {
                maskLc[static_cast<size_t>(b)] = 1.0;
            }
            else if (isBrickwall)
            {
                // Ultra-steep raised-cosine transition over +/- 1.5 bins (~18 Hz) around fcLc -> >100 dB brickwall!
                const double trans = std::max(1.5 * binHz, fcLc * 0.06);
                const double u = std::clamp((f - (fcLc - trans)) / (2.0 * trans), 0.0, 1.0);
                maskLc[static_cast<size_t>(b)] = 0.5 - 0.5 * std::cos(3.14159265358979323846 * u);
            }
            else
            {
                const double ratio = fcLc / f;
                const double logR  = std::log10(std::max(ratio, 1e-6));
                const double exp2n = static_cast<double>(2 * lcOrder) * logR;
                const double powTerm = (exp2n > 12.0) ? 1e12 : std::pow(10.0, exp2n);
                maskLc[static_cast<size_t>(b)] = 1.0 / std::sqrt(1.0 + powTerm);
            }

            // 2. Smooth 24 dB/oct Linear-Phase Air High-Pass Mask above (N_harm + 0.5)*f0
            const double rAir = fcAir / f;
            const double rAir8 = std::min(1e12, std::pow(rAir, 8.0)); // 4th-order Butterworth = 24 dB/oct
            maskAir[static_cast<size_t>(b)] = 1.0 / std::sqrt(1.0 + rAir8);
        }
    }

    void applyStereoMaskAndIfft(const double* realMask,
                                double* olaDestL, double* olaDestR) noexcept
    {
        // Pack Z(b) = realMask(b) * (X_L(b) + j * X_R(b)) for both positive & negative freqs
        const int nyq = N / 2;
        ifftRe[0] = specRe[0][0] * realMask[0];
        ifftIm[0] = specRe[1][0] * realMask[0];
        ifftRe[static_cast<size_t>(nyq)] = specRe[0][static_cast<size_t>(nyq)] * realMask[nyq];
        ifftIm[static_cast<size_t>(nyq)] = specRe[1][static_cast<size_t>(nyq)] * realMask[nyq];

        for (int b = 1; b < nyq; ++b)
        {
            const size_t bu = static_cast<size_t>(b);
            const size_t mu = static_cast<size_t>(N - b);
            const double m  = realMask[b];
            const double lr = specRe[0][bu] * m;
            const double li = specIm[0][bu] * m;
            const double rr = specRe[1][bu] * m;
            const double ri = specIm[1][bu] * m;

            // Positive freq b:   (lr + j*li) + j*(rr + j*ri) = (lr - ri) + j*(li + rr)
            // Negative freq N-b: (lr - j*li) + j*(rr - j*ri) = (lr + ri) + j*(-li + rr)
            ifftRe[bu] = lr - ri;
            ifftIm[bu] = li + rr;
            ifftRe[mu] = lr + ri;
            ifftIm[mu] = rr - li;
        }

        fft.inverse(ifftRe.data(), ifftIm.data());

        for (int i = 0; i < N; ++i)
        {
            const double ws = sqrtHann[static_cast<size_t>(i)] * olaNorm;
            const int dstIdx = (outPos + i) & (N - 1);
            olaDestL[dstIdx] += ifftRe[static_cast<size_t>(i)] * ws;
            olaDestR[dstIdx] += ifftIm[static_cast<size_t>(i)] * ws;
        }
    }

    double fs = 48000.0;
    int N = 4096;
    int H = 512;
    int numBins = 2049;
    double olaNorm = 0.25;

    FftRadix2 fft;
    SpectralFrameAnalyzer analyzer;
    SmsChoirSynth smsSynth;
    MinPhaseLowCut minPhaseLowCut;

    std::vector<double> sqrtHann;
    std::vector<double> inRing[2];
    std::vector<double> rawDelayRing[2];
    std::vector<double> olaDryRing[2];
    std::vector<double> olaAirRing[2];
    std::vector<double> specRe[2], specIm[2];

    std::vector<double> monoTimeScratch;
    std::vector<double> monoSpecRe, monoSpecIm;
    std::vector<double> ifftRe, ifftIm;
    std::vector<double> maskLc, maskAir, maskAirTotal;

    int inPos = 0;
    int outPos = 0;
    int hopCounter = 0;

    std::array<SpectralFrame, kQueueSize> frameQueue {};
    int frameWriteIdx = 0;

    double smoothLinearLcHz = 60.0;
    double smoothLinearAirHz = 3600.0;
    double smoothMinPhaseLcHz = 60.0;
    double minPhaseCutAlpha = 0.004;

    double lastValidF0 = 180.0;
    double liveF0 = 180.0;
    double liveConf = 0.0;
    double liveLowCutHz = 60.0;
    double liveAirXoverHz = 3600.0;
    std::array<double, kMaxHarmonics> liveHarmAmps {};
};

} // namespace choirfairy2
