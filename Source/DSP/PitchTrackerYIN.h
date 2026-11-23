#pragma once
#include "TptSvf.h"
#include <array>
#include <cmath>
#include <algorithm>

namespace choirfairy
{

class PitchTrackerYIN
{
public:
    static constexpr int kRingSize  = 1024;
    static constexpr int kYinWindow = 264;  // ~22 ms at 12 kHz
    static constexpr int kMaxTau    = 320;  // Down to ~37.5 Hz at 12 kHz
    static constexpr int kHopSize   = 44;   // ~3.6 ms hop at 12 kHz for crisp jump detection

    void prepare(double hostSampleRate)
    {
        hostFs = std::max(hostSampleRate, 8000.0);

        decimFactor = std::max(1, static_cast<int>(std::round(hostFs / 12000.0)));
        decimFs = hostFs / static_cast<double>(decimFactor);

        aaLpf1.prepare(hostFs);
        aaLpf2.prepare(hostFs);
        aaLpf1.setParams(std::min(1650.0, 0.30 * decimFs), 0.5411961);
        aaLpf2.setParams(std::min(1650.0, 0.30 * decimFs), 1.3065630);

        sideHpf1.prepare(hostFs);
        sideHpf2.prepare(hostFs);
        lastSideHpfHz = 55.0;
        sideHpf1.setParams(lastSideHpfHz, 0.5411961);
        sideHpf2.setParams(lastSideHpfHz, 1.3065630);

        confAtkAlpha = 1.0 - std::exp(-1.0 / (0.010 * hostFs));
        confRelAlpha = 1.0 - std::exp(-1.0 / (0.045 * hostFs));

        // Smooth vibrato tracker (~9 ms log-frequency time constant for within-note intonation)
        vibAlpha = 1.0 - std::exp(-1.0 / (0.009 * hostFs));

        reset();
    }

    void reset()
    {
        aaLpf1.reset(); aaLpf2.reset();
        sideHpf1.reset(); sideHpf2.reset();
        ring.fill(0.0);
        cmndfBuf.fill(1.0);
        writePos = 0;
        decimCounter = 0;
        hopCounter = 0;
        activeF0 = 180.0;
        targetF0 = 180.0;
        targetConfidence = 0.0;
        smoothedConfidence = 0.0;
        hasValidLock = false;
        pendingJumpCount = 0;
        pendingJumpF0 = 180.0;
        jumpTriggeredFlag = false;
    }

    /**
     * Feed one mono host-rate sample.
     * Returns true on the exact sample a confirmed note jump occurred (so synth banks crossfade!).
     */
    inline bool processSample(double monoIn, double minF0Hz, double maxF0Hz) noexcept
    {
        jumpTriggeredFlag = false;

        const double desiredHpf = std::clamp(minF0Hz * 0.78, 30.0, 260.0);
        if (std::abs(desiredHpf - lastSideHpfHz) > 0.5)
        {
            lastSideHpfHz = desiredHpf;
            sideHpf1.setParams(lastSideHpfHz, 0.5411961);
            sideHpf2.setParams(lastSideHpfHz, 1.3065630);
        }

        double x = sideHpf2.processHighPass(sideHpf1.processHighPass(monoIn));
        x = aaLpf2.processLowPass(aaLpf1.processLowPass(x));

        if (++decimCounter >= decimFactor)
        {
            decimCounter = 0;
            ring[static_cast<size_t>(writePos)] = x;
            writePos = (writePos + 1) & (kRingSize - 1);

            if (++hopCounter >= kHopSize)
            {
                hopCounter = 0;
                runYinAnalysis(minF0Hz, maxF0Hz);
            }
        }

        const double cAlpha = (targetConfidence > smoothedConfidence) ? confAtkAlpha : confRelAlpha;
        smoothedConfidence += cAlpha * (targetConfidence - smoothedConfidence);

        // Within-note vibrato smoothing only! (Large note jumps snap via jumpTriggeredFlag)
        if (!jumpTriggeredFlag && targetConfidence > 0.25)
        {
            const double logCur = std::log(std::max(activeF0, 20.0));
            const double logTgt = std::log(std::clamp(targetF0, 20.0, 2000.0));
            activeF0 = std::exp(logCur + vibAlpha * (logTgt - logCur));
        }

        return jumpTriggeredFlag;
    }

    double getTrackedF0() const noexcept  { return activeF0; }
    double getConfidence() const noexcept { return smoothedConfidence; }

private:
    double measureToneMag(int startIdx, double freqHz) const noexcept
    {
        const double w = 6.283185307179586 * freqHz / decimFs;
        double re = 0.0, im = 0.0;
        for (int j = 0; j < kYinWindow; ++j)
        {
            const double s = ring[static_cast<size_t>((startIdx + j) & (kRingSize - 1))];
            const double ph = w * static_cast<double>(j);
            re += s * std::cos(ph);
            im -= s * std::sin(ph);
        }
        return std::hypot(re, im);
    }

    void runYinAnalysis(double minF0Hz, double maxF0Hz) noexcept
    {
        const double fMin = std::clamp(minF0Hz, 40.0, 400.0);
        const double fMax = std::clamp(maxF0Hz, fMin + 40.0, 1500.0);

        const int tauMin = std::clamp(static_cast<int>(std::floor(decimFs / fMax)), 6, kMaxTau - 4);
        const int tauMax = std::clamp(static_cast<int>(std::ceil(decimFs / fMin)), tauMin + 2, kMaxTau - 1);

        // 1. Silence gate (~ -56 dBFS)
        double energy = 0.0;
        const int startIdx = (writePos - kYinWindow) & (kRingSize - 1);
        for (int j = 0; j < kYinWindow; ++j)
        {
            const double s = ring[static_cast<size_t>((startIdx + j) & (kRingSize - 1))];
            energy += s * s;
        }
        const double rms = std::sqrt(energy / static_cast<double>(kYinWindow));
        if (rms < 0.0014)
        {
            targetConfidence = 0.0;
            pendingJumpCount = 0;
            return;
        }

        // 2. YIN CMNDF
        cmndfBuf[0] = 1.0;
        double runningSum = 0.0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            double sum = 0.0;
            for (int j = 0; j < kYinWindow; ++j)
            {
                const double a = ring[static_cast<size_t>((startIdx + j) & (kRingSize - 1))];
                const double b = ring[static_cast<size_t>((startIdx + j - tau) & (kRingSize - 1))];
                const double d = a - b;
                sum += d * d;
            }
            runningSum += sum;
            cmndfBuf[static_cast<size_t>(tau)] = (runningSum > 1e-12)
                ? (sum * static_cast<double>(tau) / runningSum)
                : 1.0;
        }

        // 3. First valley below threshold
        constexpr double yinThresh = 0.20;
        int bestTau = -1;
        for (int tau = tauMin; tau < tauMax; ++tau)
        {
            if (cmndfBuf[static_cast<size_t>(tau)] < yinThresh)
            {
                while (tau + 1 < tauMax && cmndfBuf[static_cast<size_t>(tau + 1)] < cmndfBuf[static_cast<size_t>(tau)])
                    ++tau;
                bestTau = tau;
                break;
            }
        }

        if (bestTau < 0)
        {
            double minVal = 10.0;
            for (int tau = tauMin; tau <= tauMax; ++tau)
            {
                if (cmndfBuf[static_cast<size_t>(tau)] < minVal)
                {
                    minVal = cmndfBuf[static_cast<size_t>(tau)];
                    bestTau = tau;
                }
            }
        }

        if (bestTau <= 1 || bestTau >= tauMax)
        {
            targetConfidence = 0.0;
            return;
        }

        // 4. Phantom sub-octave disambiguation
        const int halfTau = bestTau / 2;
        if (halfTau >= tauMin + 1 && halfTau <= tauMax - 1)
        {
            int localHalf = halfTau;
            for (int t = std::max(tauMin, halfTau - 2); t <= std::min(tauMax - 1, halfTau + 2); ++t)
                if (cmndfBuf[static_cast<size_t>(t)] < cmndfBuf[static_cast<size_t>(localHalf)])
                    localHalf = t;

            if (cmndfBuf[static_cast<size_t>(localHalf)] < 0.38)
            {
                const double magLow  = measureToneMag(startIdx, decimFs / static_cast<double>(bestTau));
                const double magHigh = measureToneMag(startIdx, decimFs / static_cast<double>(localHalf));
                if (magLow < 0.35 * magHigh)
                    bestTau = localHalf;
            }
        }

        const double bestCmndf = cmndfBuf[static_cast<size_t>(bestTau)];
        const double conf = std::clamp(1.0 - (bestCmndf - 0.22) / 0.16, 0.0, 1.0);
        targetConfidence = conf;

        if (conf < 0.25)
        {
            pendingJumpCount = 0;
            return;
        }

        // 5. Parabolic interpolation
        const double y0 = cmndfBuf[static_cast<size_t>(bestTau - 1)];
        const double y1 = cmndfBuf[static_cast<size_t>(bestTau)];
        const double y2 = cmndfBuf[static_cast<size_t>(bestTau + 1)];
        const double denom = 2.0 * (y0 - 2.0 * y1 + y2);
        double refinedTau = static_cast<double>(bestTau);
        if (denom > 1e-12)
            refinedTau += std::clamp((y0 - y2) / denom, -0.5, 0.5);

        const double candF0 = std::clamp(decimFs / std::max(refinedTau, 2.0), fMin, fMax);

        if (!hasValidLock)
        {
            activeF0 = candF0;
            targetF0 = candF0;
            hasValidLock = true;
            jumpTriggeredFlag = true;
            pendingJumpCount = 0;
            return;
        }

        // 6. Dual-Mode Decision: Vibrato Smooth vs Confirmed Note-Jump Snap!
        const double diffSemis = std::abs(12.0 * std::log2(candF0 / std::max(activeF0, 20.0)));

        if (diffSemis <= 0.78)
        {
            // Expressive vibrato / pitch bend within the same note: smoothly track!
            targetF0 = candF0;
            pendingJumpCount = 0;
        }
        else
        {
            // Potential note jump (> 0.78 semitones): confirm over 2 consistent hops (~7.2 ms)
            // so single-frame breath/consonant glitches NEVER trigger false jumps!
            const double pendingDiff = std::abs(12.0 * std::log2(candF0 / std::max(pendingJumpF0, 20.0)));
            if (pendingJumpCount > 0 && pendingDiff <= 0.65)
            {
                ++pendingJumpCount;
                pendingJumpF0 = 0.5 * (pendingJumpF0 + candF0);
            }
            else
            {
                pendingJumpCount = 1;
                pendingJumpF0 = candF0;
            }

            if (pendingJumpCount >= 2)
            {
                // Confirmed melodic jump! Snap instantly & trigger Bank A <-> Bank B crossfade!
                activeF0 = pendingJumpF0;
                targetF0 = pendingJumpF0;
                pendingJumpCount = 0;
                jumpTriggeredFlag = true;
            }
        }
    }

    double hostFs = 48000.0;
    double decimFs = 12000.0;
    int decimFactor = 4;
    double lastSideHpfHz = 55.0;

    TptSvf aaLpf1, aaLpf2, sideHpf1, sideHpf2;

    std::array<double, kRingSize> ring {};
    std::array<double, kMaxTau + 2> cmndfBuf {};
    int writePos = 0;
    int decimCounter = 0;
    int hopCounter = 0;

    bool hasValidLock = false;
    double activeF0 = 180.0;
    double targetF0 = 180.0;
    double targetConfidence = 0.0;
    double smoothedConfidence = 0.0;
    double confAtkAlpha = 0.002;
    double confRelAlpha = 0.0005;
    double vibAlpha = 0.002;

    int pendingJumpCount = 0;
    double pendingJumpF0 = 180.0;
    bool jumpTriggeredFlag = false;
};

} // namespace choirfairy
