#pragma once
#include "TptSvf.h"
#include <cmath>
#include <algorithm>

namespace choirfairy
{

enum class LowCutSlope : int
{
    Off   = 0,
    dB12  = 1, // 2nd-order Butterworth  (1 SVF)
    dB24  = 2, // 4th-order Butterworth  (2 SVFs)
    dB36  = 3, // 6th-order Butterworth  (3 SVFs)
    dB48  = 4, // 8th-order Butterworth  (4 SVFs)
    dB72  = 5, // 12th-order Butterworth (6 SVFs)
    dB96  = 6  // 16th-order Butterworth (8 SVFs)
};

class TrackingLowCut
{
public:
    static constexpr int kMaxStages = 8;

    void prepare(double sampleRate)
    {
        fs = std::max(sampleRate, 8000.0);
        cutoffSmoothAlpha = 1.0 - std::exp(-1.0 / (0.008 * fs));
        for (int ch = 0; ch < 2; ++ch)
            for (int st = 0; st < kMaxStages; ++st)
                stages[ch][st].prepare(fs);
        reset();
    }

    void reset()
    {
        currentCutoffHz = 65.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int st = 0; st < kMaxStages; ++st)
                stages[ch][st].reset();
    }

    inline void processStereoSample(double& left, double& right,
                                    LowCutSlope slope,
                                    double trackedF0, double confidence,
                                    double offsetSemis, double floorHz) noexcept
    {
        const double floorClamped = std::clamp(floorHz, 18.0, 400.0);
        const double voicedTarget = std::clamp(trackedF0 * std::pow(2.0, offsetSemis / 12.0),
                                               floorClamped, 1400.0);

        const double c = std::clamp(confidence, 0.0, 1.0);
        const double targetHz = std::exp((1.0 - c) * std::log(floorClamped) + c * std::log(voicedTarget));

        const double logCur = std::log(std::max(currentCutoffHz, 15.0));
        const double logTgt = std::log(std::max(targetHz, 15.0));
        currentCutoffHz = std::exp(logCur + cutoffSmoothAlpha * (logTgt - logCur));

        const int slopeIdx = static_cast<int>(slope);
        if (slopeIdx <= 0)
            return;

        static constexpr int stageCounts[6] = { 1, 2, 3, 4, 6, 8 };
        static constexpr double qTable[6][8] = {
            // 12 dB/oct (2nd order)
            { 0.70710678, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
            // 24 dB/oct (4th order)
            { 0.54119610, 1.30656296, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
            // 36 dB/oct (6th order)
            { 0.51763809, 0.70710678, 1.93185165, 0.0, 0.0, 0.0, 0.0, 0.0 },
            // 48 dB/oct (8th order)
            { 0.50979558, 0.60134489, 0.89997622, 2.56291545, 0.0, 0.0, 0.0, 0.0 },
            // 72 dB/oct (12th order)
            { 0.50431448, 0.54119610, 0.63023621, 0.82133982, 1.30656296, 3.83064879, 0.0, 0.0 },
            // 96 dB/oct (16th order)
            { 0.50241929, 0.52249861, 0.56694403, 0.64682178, 0.78815462, 1.06067769, 1.72244710, 5.10114862 }
        };

        const int row = std::clamp(slopeIdx - 1, 0, 5);
        const int numStages = stageCounts[row];

        double l = left;
        double r = right;
        for (int st = 0; st < numStages; ++st)
        {
            const double q = qTable[row][st];
            stages[0][st].setParams(currentCutoffHz, q);
            stages[1][st].setParams(currentCutoffHz, q);
            l = stages[0][st].processHighPass(l);
            r = stages[1][st].processHighPass(r);
        }
        left  = l;
        right = r;
    }

    double getCurrentCutoffHz() const noexcept { return currentCutoffHz; }

private:
    double fs = 48000.0;
    double cutoffSmoothAlpha = 0.003;
    double currentCutoffHz = 65.0;
    TptSvf stages[2][kMaxStages];
};

} // namespace choirfairy
