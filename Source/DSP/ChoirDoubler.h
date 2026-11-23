#pragma once
#include "TptSvf.h"
#include "HarmonicAnalyzer20.h"
#include <array>
#include <cmath>
#include <algorithm>

namespace choirfairy
{

/**
 * Stereo Harmonic Resynthesis Doubler & High-Band Air Widener ("Choir Fairy" Core).
 *
 * Key architecture:
 *   1. Dual-Bank Ping-Pong Voice Crossfade (Bank A <-> Bank B, 22 ms equal-power fade):
 *      On confirmed note jumps, the old bank holds its pitch and fades out while the fresh
 *      bank snaps cleanly to the new note and fades in -> ZERO pitch-jump gulping (warble/glissando)!
 *   2. Independent Left & Right 20-Harmonic Additive Sine Banks:
 *      - L detuned down, R detuned up (+/- detuneCents)
 *      - Independent slow organic human pitch wander & vibrato drift per side
 *      - Asymmetric L/R Haas micro-delay lines (0.72x / 1.18x Timing ms) applied to the
 *        synthesized voices so they sit wide & natural around the dry center!
 *   3. Dynamic LR4 High-Band Air Split above (N_harm + 0.5) * f0:
 *      - Original high-frequency air, breath & consonants pass through independent L/R
 *        subtly chorus-modulated micro-delay lines synced to the doubler timing!
 */
class ChoirDoubler
{
public:
    static constexpr int kMaxHarmonics = HarmonicAnalyzer20::kMaxHarmonics;
    static constexpr int kDelayBufSize = 8192; // >85 ms @ 96 kHz, >170 ms @ 48 kHz

    struct Output
    {
        double wetSinesL;
        double wetSinesR;
        double wetAirL;
        double wetAirR;
    };

    void prepare(double sampleRate)
    {
        fs = std::max(sampleRate, 8000.0);
        invFs = 1.0 / fs;

        // 22 ms equal-power bank crossfade on confirmed pitch jumps
        xfadeStep = 1.0 / (0.022 * fs);
        xoverSmoothAlpha = 1.0 - std::exp(-1.0 / (0.015 * fs));

        for (int ch = 0; ch < 2; ++ch)
        {
            hp1[ch].prepare(fs);
            hp2[ch].prepare(fs);
        }

        // Uncorrelated slow human pitch-wander LFO rates (Hz) for L & R
        driftInc[0][0] = 6.283185307179586 * 0.43 * invFs;
        driftInc[0][1] = 6.283185307179586 * 1.17 * invFs;
        driftInc[1][0] = 6.283185307179586 * 0.59 * invFs;
        driftInc[1][1] = 6.283185307179586 * 1.41 * invFs;

        // Subtle chorus modulation rates for high-band air taps (Hz)
        airModInc[0] = 6.283185307179586 * 0.37 * invFs;
        airModInc[1] = 6.283185307179586 * 0.53 * invFs;

        reset();
    }

    void reset()
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            hp1[ch].reset();
            hp2[ch].reset();
            sineDelayBuf[ch].fill(0.0);
            airDelayBuf[ch].fill(0.0);
            driftPh[ch][0] = 0.8 + 1.9 * ch;
            driftPh[ch][1] = 2.1 + 1.3 * ch;
            airModPh[ch]   = 0.5 + 2.4 * ch;
        }
        delayWritePos = 0;

        activeBank = 0;
        xfadePos = 1.0;
        currentXoverHz = 3600.0;

        for (int b = 0; b < 2; ++b)
        {
            bankF0[b] = 180.0;
            bankAmps[b].fill(0.0);
            for (int ch = 0; ch < 2; ++ch)
            {
                // Golden-ratio uncorrelated initial harmonic phases between L & R (zero mono phase cancellation!)
                for (int k = 0; k < kMaxHarmonics; ++k)
                {
                    bankPhases[b][ch][static_cast<size_t>(k)] =
                        std::fmod(0.61803398875 * (k + 1) + 0.37 * ch + 0.23 * b, 1.0) * 6.283185307179586;
                }
            }
        }
    }

    /**
     * Trigger a clean Ping-Pong bank crossfade when PitchTrackerYIN confirms a note jump!
     */
    inline void triggerPitchJump(double newF0, const std::array<double, kMaxHarmonics>& currentAmps) noexcept
    {
        const int nextBank = 1 - activeBank;
        bankF0[nextBank]   = std::clamp(newF0, 35.0, 1500.0);
        bankAmps[nextBank] = currentAmps;
        activeBank = nextBank;
        xfadePos   = 0.0; // Start smooth 22 ms equal-power crossfade from oldBank -> activeBank
    }

    inline Output processStereoSample(double inL, double inR,
                                      double trackedF0, double confidence,
                                      int numHarmonics,
                                      const std::array<double, kMaxHarmonics>& harmAmps,
                                      double detuneCents,
                                      double timingMs,
                                      double driftAmount01,
                                      double tiltDb,
                                      double airBlend) noexcept
    {
        const int nHarm = std::clamp(numHarmonics, 1, kMaxHarmonics);
        const double f0 = std::clamp(trackedF0, 35.0, 1500.0);

        // Update active bank pitch & amplitudes smoothly (old fading-out bank keeps its frozen pitch!)
        bankF0[activeBank]   = f0;
        bankAmps[activeBank] = harmAmps;

        if (xfadePos < 1.0)
            xfadePos = std::min(1.0, xfadePos + xfadeStep);

        // Equal-power raised-cosine crossfade gains
        const double gainNew = std::sin(xfadePos * 1.5707963267948966);
        const double gainOld = std::cos(xfadePos * 1.5707963267948966);
        const int oldBank = 1 - activeBank;

        // 1. Update uncorrelated organic L/R human pitch drift (cents)
        double driftCents[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            driftPh[ch][0] += driftInc[ch][0];
            driftPh[ch][1] += driftInc[ch][1];
            if (driftPh[ch][0] >= 6.283185307179586) driftPh[ch][0] -= 6.283185307179586;
            if (driftPh[ch][1] >= 6.283185307179586) driftPh[ch][1] -= 6.283185307179586;

            // Up to +/- 14 cents organic slow vocal wander when Drift = 100%
            driftCents[ch] = driftAmount01 * 14.0
                           * (0.65 * std::sin(driftPh[ch][0]) + 0.35 * std::sin(driftPh[ch][1]));
        }

        // L detuned slightly down, R detuned slightly up
        const double detRatio[2] = {
            std::pow(2.0, (-detuneCents + driftCents[0]) / 1200.0),
            std::pow(2.0, ( detuneCents + driftCents[1]) / 1200.0)
        };

        // 2. Synthesize 20-Harmonic Stereo Additive Banks (active + fading-out bank if crossfading)
        double rawSine[2] = { 0.0, 0.0 };
        const double tiltSlope = tiltDb / 18.0; // Smooth dB tilt across H1..H20

        auto renderBank = [&](int bIdx, double bankGain)
        {
            if (bankGain <= 1e-5)
                return;

            const double bf0 = bankF0[bIdx];
            constexpr double twoPi = 6.2831853071795864769;

            for (int k = 0; k < nHarm; ++k)
            {
                const double amp = bankAmps[bIdx][static_cast<size_t>(k)];
                if (amp <= 1e-6)
                    continue;

                const double hk = static_cast<double>(k + 1);
                const double baseFreq = hk * bf0;
                if (baseFreq >= 0.42 * fs)
                    break;

                // Subtle tilt shaping (0 dB at H1, +/- tiltDb toward H20)
                const double tiltGain = (std::abs(tiltSlope) > 1e-4)
                    ? std::pow(10.0, (tiltSlope * std::log2(hk)) * 0.05)
                    : 1.0;

                const double effAmp = amp * tiltGain * bankGain;

                for (int ch = 0; ch < 2; ++ch)
                {
                    // Tiny per-harmonic micro-spread (0.04 cents * k) adds rich chorus bloom on upper harmonics
                    const double harmSpread = 1.0 + (ch == 0 ? -1.0 : 1.0) * (2.5e-5 * k * driftAmount01);
                    const double fInst = std::min(baseFreq * detRatio[ch] * harmSpread, 0.44 * fs);

                    double ph = bankPhases[bIdx][ch][static_cast<size_t>(k)] + twoPi * fInst * invFs;
                    if (ph >= twoPi) ph -= twoPi;
                    bankPhases[bIdx][ch][static_cast<size_t>(k)] = ph;

                    rawSine[ch] += effAmp * std::sin(ph);
                }
            }
        };

        renderBank(activeBank, gainNew);
        if (xfadePos < 1.0)
            renderBank(oldBank, gainOld);

        // 3. Extract High-Band Original Air above (N_harm + 0.5) * f0 via Dynamic LR4 High-Pass
        //    When unvoiced (confidence -> 0), lower crossover gently to ~1.4 kHz so consonants get airy width!
        const double voicedXover   = std::clamp((static_cast<double>(nHarm) + 0.5) * f0, 350.0, 14500.0);
        const double unvoicedXover = 1400.0;
        const double c = std::clamp(confidence, 0.0, 1.0);
        const double targetXover = std::exp((1.0 - c) * std::log(unvoicedXover) + c * std::log(voicedXover));

        const double logCurX = std::log(std::max(currentXoverHz, 200.0));
        const double logTgtX = std::log(targetXover);
        currentXoverHz = std::exp(logCurX + xoverSmoothAlpha * (logTgtX - logCurX));

        constexpr double qButt = 0.7071067811865475;
        double highAir[2];
        const double inPair[2] = { inL, inR };
        for (int ch = 0; ch < 2; ++ch)
        {
            hp1[ch].setParams(currentXoverHz, qButt);
            hp2[ch].setParams(currentXoverHz, qButt);
            highAir[ch] = hp2[ch].processHighPass(hp1[ch].processHighPass(inPair[ch]));
        }

        // 4. Write both Resynthesized Sines & Original High-Band Air into Asymmetric L/R Delay Lines
        for (int ch = 0; ch < 2; ++ch)
        {
            sineDelayBuf[ch][static_cast<size_t>(delayWritePos)] = rawSine[ch];
            airDelayBuf[ch][static_cast<size_t>(delayWritePos)]  = highAir[ch];

            airModPh[ch] += airModInc[ch];
            if (airModPh[ch] >= 6.283185307179586) airModPh[ch] -= 6.283185307179586;
        }

        // Asymmetric L/R Haas timing offsets (0.72x on Left, 1.18x on Right) + subtle air chorus drift
        const double baseMs = std::clamp(timingMs, 0.5, 40.0);
        const double delaySamplesSine[2] = {
            std::clamp((baseMs * 0.72) * 0.001 * fs, 1.0, static_cast<double>(kDelayBufSize - 4)),
            std::clamp((baseMs * 1.18) * 0.001 * fs, 1.0, static_cast<double>(kDelayBufSize - 4))
        };

        // Air taps match the timing feel + gentle 0.55 ms chorus micro-modulation so air never static-flanges
        const double airChorusMs = 0.55 * driftAmount01;
        const double delaySamplesAir[2] = {
            std::clamp((baseMs * 0.78 + airChorusMs * std::sin(airModPh[0])) * 0.001 * fs, 1.0, static_cast<double>(kDelayBufSize - 4)),
            std::clamp((baseMs * 1.24 + airChorusMs * std::sin(airModPh[1])) * 0.001 * fs, 1.0, static_cast<double>(kDelayBufSize - 4))
        };

        Output out {};
        out.wetSinesL = readDelayLinear(sineDelayBuf[0], delaySamplesSine[0]);
        out.wetSinesR = readDelayLinear(sineDelayBuf[1], delaySamplesSine[1]);
        out.wetAirL   = readDelayLinear(airDelayBuf[0],  delaySamplesAir[0]) * airBlend;
        out.wetAirR   = readDelayLinear(airDelayBuf[1],  delaySamplesAir[1]) * airBlend;

        delayWritePos = (delayWritePos + 1) & (kDelayBufSize - 1);
        return out;
    }

    double getCurrentXoverHz() const noexcept { return currentXoverHz; }

private:
    inline double readDelayLinear(const std::array<double, kDelayBufSize>& buf, double delaySamp) const noexcept
    {
        const double readPos = static_cast<double>(delayWritePos) - delaySamp;
        const int i0 = static_cast<int>(std::floor(readPos));
        const double frac = readPos - static_cast<double>(i0);
        const int idx0 = i0 & (kDelayBufSize - 1);
        const int idx1 = (i0 + 1) & (kDelayBufSize - 1);
        return buf[static_cast<size_t>(idx0)] + frac * (buf[static_cast<size_t>(idx1)] - buf[static_cast<size_t>(idx0)]);
    }

    double fs = 48000.0;
    double invFs = 1.0 / 48000.0;

    int activeBank = 0;
    double xfadePos = 1.0;
    double xfadeStep = 0.001;

    double bankF0[2] = { 180.0, 180.0 };
    std::array<double, kMaxHarmonics> bankAmps[2] {};
    std::array<double, kMaxHarmonics> bankPhases[2][2] {};

    double driftPh[2][2] {};
    double driftInc[2][2] {};

    double airModPh[2] {};
    double airModInc[2] {};

    TptSvf hp1[2], hp2[2];
    double xoverSmoothAlpha = 0.0015;
    double currentXoverHz = 3600.0;

    std::array<double, kDelayBufSize> sineDelayBuf[2] {};
    std::array<double, kDelayBufSize> airDelayBuf[2] {};
    int delayWritePos = 0;
};

} // namespace choirfairy
