#pragma once
#include "SpectralFrameAnalyzer.h"
#include <array>
#include <cmath>
#include <algorithm>

namespace choirfairy2
{

/**
 * High-speed Lookahead McAulay-Quatieri SMS 2/4-Voice Stereo Choir Synthesizer.
 * Uses a 4096-entry linear-interpolated sine LUT (THD < -135 dB) and hop-cached tilt gains.
 */
class SmsChoirSynth
{
public:
    static constexpr int kMaxVoices    = 4;
    static constexpr int kDelayBufSize = 8192;
    static constexpr int kSinTableSize = 4096;
    static constexpr int kSinTableMask = kSinTableSize - 1;

    struct Output
    {
        double wetSinesL;
        double wetSinesR;
        double wetAirL;
        double wetAirR;
    };

    SmsChoirSynth()
    {
        constexpr double twoPi = 6.2831853071795864769;
        for (int i = 0; i <= kSinTableSize; ++i)
            sinTable[static_cast<size_t>(i)] = std::sin(twoPi * static_cast<double>(i) / static_cast<double>(kSinTableSize));
    }

    void prepare(double sampleRate, int hopSize)
    {
        fs = std::max(sampleRate, 8000.0);
        invFs = 1.0 / fs;
        H = std::max(64, hopSize);
        invH = 1.0 / static_cast<double>(H);

        // 4 independent slow organic vocal pitch-drift LFO pairs (Hz), updated every 8 samples
        static constexpr double f1[4] = { 0.37, 0.51, 0.43, 0.63 };
        static constexpr double f2[4] = { 1.09, 1.31, 0.97, 1.47 };
        for (int v = 0; v < kMaxVoices; ++v)
        {
            driftInc[v][0] = 6.283185307179586 * f1[v] * invFs * 8.0;
            driftInc[v][1] = 6.283185307179586 * f2[v] * invFs * 8.0;
        }

        airModInc[0] = 6.283185307179586 * 0.33 * invFs * 8.0;
        airModInc[1] = 6.283185307179586 * 0.47 * invFs * 8.0;

        reset();
    }

    void reset()
    {
        hopSampleIdx = 0;
        ctrlCounter = 0;
        isJumpHop = false;
        curFrameA = SpectralFrame {};
        curFrameB = SpectralFrame {};
        lastTiltDb = -999.0;
        tiltGains.fill(1.0);
        vDetRatio.fill(1.0);
        airModVal[0] = 0.0;
        airModVal[1] = 0.0;

        for (int v = 0; v < kMaxVoices; ++v)
        {
            driftPh[v][0] = 0.7 + 1.6 * v;
            driftPh[v][1] = 1.9 + 1.1 * v;
            for (int k = 0; k < kMaxHarmonics; ++k)
            {
                // Phase stored normalized in [0 .. kSinTableSize) for branchless LUT lookup!
                const double ph = std::fmod(0.61803398875 * (k + 1) + 0.271828 * (v + 1), 1.0)
                                * static_cast<double>(kSinTableSize);
                phaseMain[v][static_cast<size_t>(k)]  = ph;
                phaseDeath[v][static_cast<size_t>(k)] = ph;
            }
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            sineDelayBuf[ch].fill(0.0);
            airDelayBuf[ch].fill(0.0);
            airModPh[ch] = 0.4 + 2.3 * ch;
        }
        delayWritePos = 0;
    }

    void beginHop(const SpectralFrame& frameA, const SpectralFrame& frameB) noexcept
    {
        curFrameA = frameA;
        curFrameB = frameB;
        hopSampleIdx = 0;

        const bool aVoiced = (frameA.confidence > 0.20);
        const bool bVoiced = (frameB.confidence > 0.20);
        if (aVoiced && bVoiced)
        {
            const double diffSemis = std::abs(12.0 * std::log2(
                std::max(frameB.cleanF0, 20.0) / std::max(frameA.cleanF0, 20.0)));
            isJumpHop = (diffSemis > 0.75);
        }
        else
        {
            isJumpHop = (aVoiced != bVoiced);
        }

        if (isJumpHop)
            phaseDeath = phaseMain;
    }

    inline Output processSample(double airInL, double airInR,
                                int numHarmonics,
                                bool fourVoiceChoir,
                                double detuneCents,
                                double timingMs,
                                double driftAmount01,
                                double tiltDb,
                                double airBlend) noexcept
    {
        const double t = std::clamp(static_cast<double>(hopSampleIdx) * invH, 0.0, 1.0);
        if (hopSampleIdx < H)
            ++hopSampleIdx;

        const int nHarm = std::clamp(numHarmonics, 1, kMaxHarmonics);
        const int numVoices = fourVoiceChoir ? 4 : 2;

        // Recompute harmonic tilt table only when tiltDb changes
        if (std::abs(tiltDb - lastTiltDb) > 1e-4)
        {
            lastTiltDb = tiltDb;
            const double slope = tiltDb / 18.0;
            for (int k = 0; k < kMaxHarmonics; ++k)
                tiltGains[static_cast<size_t>(k)] = std::pow(10.0, (slope * std::log2(static_cast<double>(k + 1))) * 0.05);
        }

        static constexpr double vDetScale[4] = { -1.0,  1.0, -0.42,  0.46 };
        static constexpr double vGainL[4]    = {  1.0,  0.0,  0.80,  0.28 };
        static constexpr double vGainR[4]    = {  0.0,  1.0,  0.28,  0.80 };
        const double voiceNorm = fourVoiceChoir ? 0.58 : 1.0;

        // Update slow LFO pitch-drift ratios & air chorus mod every 8 samples (6 kHz control rate!)
        if ((ctrlCounter++ & 7) == 0)
        {
            for (int v = 0; v < kMaxVoices; ++v)
            {
                driftPh[v][0] += driftInc[v][0];
                driftPh[v][1] += driftInc[v][1];
                if (driftPh[v][0] >= 6.283185307179586) driftPh[v][0] -= 6.283185307179586;
                if (driftPh[v][1] >= 6.283185307179586) driftPh[v][1] -= 6.283185307179586;

                const double wander = driftAmount01 * 12.5
                    * (0.65 * std::sin(driftPh[v][0]) + 0.35 * std::sin(driftPh[v][1]));
                vDetRatio[static_cast<size_t>(v)] = std::pow(2.0, (detuneCents * vDetScale[v] + wander) / 1200.0);
            }

            for (int ch = 0; ch < 2; ++ch)
            {
                airModPh[ch] += airModInc[ch];
                if (airModPh[ch] >= 6.283185307179586) airModPh[ch] -= 6.283185307179586;
                airModVal[ch] = std::sin(airModPh[ch]);
            }
        }

        const double tableScale = static_cast<double>(kSinTableSize) * invFs;
        const double maxFreq = 0.44 * fs;
        const double fadeIn  = isJumpHop ? (0.5 - 0.5 * fastCosPi(t)) : t;
        const double fadeOut = 1.0 - fadeIn;

        double rawL = 0.0;
        double rawR = 0.0;

        for (int k = 0; k < nHarm; ++k)
        {
            const size_t ku = static_cast<size_t>(k);
            const double aA = curFrameA.partialAmp[ku];
            const double aB = curFrameB.partialAmp[ku];
            if (aA <= 1e-6 && aB <= 1e-6)
                continue;

            const double tGain = tiltGains[ku] * voiceNorm;

            if (!isJumpHop)
            {
                const double fBase = curFrameA.partialFreq[ku] + t * (curFrameB.partialFreq[ku] - curFrameA.partialFreq[ku]);
                const double amp   = (aA + t * (aB - aA)) * tGain;

                for (int v = 0; v < numVoices; ++v)
                {
                    const double fV = std::min(fBase * vDetRatio[static_cast<size_t>(v)], maxFreq);
                    double ph = phaseMain[static_cast<size_t>(v)][ku] + fV * tableScale;
                    if (ph >= static_cast<double>(kSinTableSize)) ph -= static_cast<double>(kSinTableSize);
                    phaseMain[static_cast<size_t>(v)][ku] = ph;

                    const double s = amp * fastSinLut(ph);
                    rawL += s * vGainL[v];
                    rawR += s * vGainR[v];
                }
            }
            else
            {
                const double fOld   = curFrameA.partialFreq[ku];
                const double fNew   = curFrameB.partialFreq[ku];
                const double ampOld = aA * fadeOut * tGain;
                const double ampNew = aB * fadeIn  * tGain;

                for (int v = 0; v < numVoices; ++v)
                {
                    const double ratio = vDetRatio[static_cast<size_t>(v)];
                    double s = 0.0;
                    if (ampOld > 1e-6)
                    {
                        const double fVO = std::min(fOld * ratio, maxFreq);
                        double phD = phaseDeath[static_cast<size_t>(v)][ku] + fVO * tableScale;
                        if (phD >= static_cast<double>(kSinTableSize)) phD -= static_cast<double>(kSinTableSize);
                        phaseDeath[static_cast<size_t>(v)][ku] = phD;
                        s += ampOld * fastSinLut(phD);
                    }
                    if (ampNew > 1e-6)
                    {
                        const double fVN = std::min(fNew * ratio, maxFreq);
                        double phM = phaseMain[static_cast<size_t>(v)][ku] + fVN * tableScale;
                        if (phM >= static_cast<double>(kSinTableSize)) phM -= static_cast<double>(kSinTableSize);
                        phaseMain[static_cast<size_t>(v)][ku] = phM;
                        s += ampNew * fastSinLut(phM);
                    }
                    rawL += s * vGainL[v];
                    rawR += s * vGainR[v];
                }
            }
        }

        // Write into stereo micro-delay lines
        sineDelayBuf[0][static_cast<size_t>(delayWritePos)] = rawL;
        sineDelayBuf[1][static_cast<size_t>(delayWritePos)] = rawR;
        airDelayBuf[0][static_cast<size_t>(delayWritePos)]  = airInL;
        airDelayBuf[1][static_cast<size_t>(delayWritePos)]  = airInR;

        const double baseMs = std::clamp(timingMs, 0.5, 42.0);
        const double maxDel = static_cast<double>(kDelayBufSize - 4);
        const double msToSamp = 0.001 * fs;

        const double dSineL = std::clamp(baseMs * 0.72 * msToSamp, 1.0, maxDel);
        const double dSineR = std::clamp(baseMs * 1.18 * msToSamp, 1.0, maxDel);

        const double modMs = 0.50 * driftAmount01;
        const double dAirL1 = std::clamp((baseMs * 0.76 + modMs * airModVal[0]) * msToSamp, 1.0, maxDel);
        const double dAirR1 = std::clamp((baseMs * 1.22 + modMs * airModVal[1]) * msToSamp, 1.0, maxDel);

        Output out {};
        out.wetSinesL = readDelayLinear(sineDelayBuf[0], dSineL);
        out.wetSinesR = readDelayLinear(sineDelayBuf[1], dSineR);

        if (!fourVoiceChoir)
        {
            out.wetAirL = readDelayLinear(airDelayBuf[0], dAirL1) * airBlend;
            out.wetAirR = readDelayLinear(airDelayBuf[1], dAirR1) * airBlend;
        }
        else
        {
            const double dAirL2 = std::clamp((baseMs * 0.44 - modMs * 0.7 * airModVal[1]) * msToSamp, 1.0, maxDel);
            const double dAirR2 = std::clamp((baseMs * 0.62 + modMs * 0.7 * airModVal[0]) * msToSamp, 1.0, maxDel);
            out.wetAirL = (0.64 * readDelayLinear(airDelayBuf[0], dAirL1)
                         + 0.44 * readDelayLinear(airDelayBuf[0], dAirL2)) * airBlend;
            out.wetAirR = (0.64 * readDelayLinear(airDelayBuf[1], dAirR1)
                         + 0.44 * readDelayLinear(airDelayBuf[1], dAirR2)) * airBlend;
        }

        delayWritePos = (delayWritePos + 1) & (kDelayBufSize - 1);
        return out;
    }

private:
    inline double fastSinLut(double phaseNorm) const noexcept
    {
        const int i0 = static_cast<int>(phaseNorm) & kSinTableMask;
        const double frac = phaseNorm - std::floor(phaseNorm);
        return sinTable[static_cast<size_t>(i0)] + frac * (sinTable[static_cast<size_t>(i0 + 1)] - sinTable[static_cast<size_t>(i0)]);
    }

    inline double fastCosPi(double t01) const noexcept
    {
        // cos(pi * t) = sin(pi/2 + pi * t) -> phaseNorm = (0.25 + 0.5*t) * kSinTableSize
        return fastSinLut((0.25 + 0.5 * t01) * static_cast<double>(kSinTableSize));
    }

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
    int H = 512;
    double invH = 1.0 / 512.0;

    int hopSampleIdx = 0;
    int ctrlCounter = 0;
    bool isJumpHop = false;
    SpectralFrame curFrameA, curFrameB;

    std::array<double, kSinTableSize + 1> sinTable {};
    std::array<double, kMaxHarmonics> tiltGains {};
    double lastTiltDb = -999.0;

    std::array<std::array<double, kMaxHarmonics>, kMaxVoices> phaseMain {};
    std::array<std::array<double, kMaxHarmonics>, kMaxVoices> phaseDeath {};
    std::array<double, kMaxVoices> vDetRatio {};

    double driftPh[kMaxVoices][2] {};
    double driftInc[kMaxVoices][2] {};
    double airModPh[2] {};
    double airModInc[2] {};
    double airModVal[2] {};

    std::array<double, kDelayBufSize> sineDelayBuf[2] {};
    std::array<double, kDelayBufSize> airDelayBuf[2] {};
    int delayWritePos = 0;
};

} // namespace choirfairy2
