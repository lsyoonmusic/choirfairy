#pragma once
#include <array>
#include <cmath>
#include <algorithm>

namespace choirfairy
{

/**
 * Real-time 20-Harmonic Spectral Peak Envelope Extractor.
 *
 * Runs a fast, dependency-free in-place 2048-pt Radix-2 FFT every 256 samples (~5.3 ms @ 48 kHz)
 * on the mono audio stream. For each harmonic k = 1..20:
 *   1. Locates the true spectral peak within a tight tolerance around k * f0 (eliminating
 *      attenuation/warble on upper harmonics H10..H20 even under vibrato or inharmonicity!).
 *   2. Refines peak amplitude with quadratic interpolation & subtracts local valley noise floor.
 *   3. Smooths each harmonic amplitude A_k(t) with click-free asymmetric attack/release followers.
 */
class HarmonicAnalyzer20
{
public:
    static constexpr int kMaxHarmonics = 20;
    static constexpr int kFftOrder     = 11;           // 2048-pt FFT (~23.4 Hz/bin @ 48 kHz)
    static constexpr int kFftSize      = 1 << kFftOrder;
    static constexpr int kNumBins      = kFftSize / 2;
    static constexpr int kHopSize      = 256;          // ~5.3 ms update @ 48 kHz

    HarmonicAnalyzer20()
    {
        initTables();
    }

    void prepare(double sampleRate)
    {
        fs = std::max(sampleRate, 8000.0);
        // Scale hop cleanly for high sample rates (96 kHz -> hop 512 keeps ~5.3 ms update)
        hopSize = (fs > 64000.0) ? 512 : 256;

        // Smooth per-sample envelope followers on extracted harmonic amplitudes
        envAtkAlpha = 1.0 - std::exp(-1.0 / (0.006 * fs)); // 6 ms crisp attack
        envRelAlpha = 1.0 - std::exp(-1.0 / (0.022 * fs)); // 22 ms lush release

        reset();
    }

    void reset()
    {
        ring.fill(0.0);
        writePos = 0;
        hopCounter = 0;
        targetAmps.fill(0.0);
        smoothAmps.fill(0.0);
        magSpec.fill(0.0);
    }

    inline void processSample(double monoIn, double trackedF0, double confidence,
                              int numHarmonics, bool forceAnalyzeNow) noexcept
    {
        ring[static_cast<size_t>(writePos)] = monoIn;
        writePos = (writePos + 1) & (kFftSize - 1);

        if (++hopCounter >= hopSize || forceAnalyzeNow)
        {
            hopCounter = 0;
            analyzeHarmonics(trackedF0, confidence, numHarmonics);
        }

        // Smooth all 20 harmonic envelopes at sample rate
        for (int k = 0; k < kMaxHarmonics; ++k)
        {
            const double tgt = targetAmps[static_cast<size_t>(k)];
            const double cur = smoothAmps[static_cast<size_t>(k)];
            const double a   = (tgt > cur) ? envAtkAlpha : envRelAlpha;
            smoothAmps[static_cast<size_t>(k)] = cur + a * (tgt - cur);
        }
    }

    const std::array<double, kMaxHarmonics>& getSmoothedAmps() const noexcept { return smoothAmps; }
    double getHarmonicAmp(int k) const noexcept
    {
        return smoothAmps[static_cast<size_t>(std::clamp(k, 0, kMaxHarmonics - 1))];
    }

private:
    void initTables()
    {
        constexpr double twoPi = 6.2831853071795864769;
        for (int i = 0; i < kFftSize; ++i)
        {
            hannWin[static_cast<size_t>(i)] =
                0.5 * (1.0 - std::cos(twoPi * static_cast<double>(i) / static_cast<double>(kFftSize - 1)));
        }

        // Bit-reversal indices
        for (int i = 0; i < kFftSize; ++i)
        {
            int x = i, rev = 0;
            for (int b = 0; b < kFftOrder; ++b)
            {
                rev = (rev << 1) | (x & 1);
                x >>= 1;
            }
            bitRev[static_cast<size_t>(i)] = rev;
        }

        // Twiddle factors for Cooley-Tukey Radix-2 FFT
        for (int i = 0; i < kNumBins; ++i)
        {
            const double ang = -twoPi * static_cast<double>(i) / static_cast<double>(kFftSize);
            twRe[static_cast<size_t>(i)] = std::cos(ang);
            twIm[static_cast<size_t>(i)] = std::sin(ang);
        }
    }

    void runFft() noexcept
    {
        for (int i = 0; i < kFftSize; ++i)
        {
            const int j = bitRev[static_cast<size_t>(i)];
            if (i < j)
            {
                std::swap(fftRe[static_cast<size_t>(i)], fftRe[static_cast<size_t>(j)]);
                std::swap(fftIm[static_cast<size_t>(i)], fftIm[static_cast<size_t>(j)]);
            }
        }

        for (int len = 2; len <= kFftSize; len <<= 1)
        {
            const int half = len >> 1;
            const int step = kFftSize / len;
            for (int i = 0; i < kFftSize; i += len)
            {
                for (int j = 0; j < half; ++j)
                {
                    const int twIdx = j * step;
                    const double wr = twRe[static_cast<size_t>(twIdx)];
                    const double wi = twIm[static_cast<size_t>(twIdx)];

                    const int uIdx = i + j;
                    const int vIdx = uIdx + half;
                    const double vr = fftRe[static_cast<size_t>(vIdx)];
                    const double vi = fftIm[static_cast<size_t>(vIdx)];

                    const double tr = wr * vr - wi * vi;
                    const double ti = wr * vi + wi * vr;

                    const double ur = fftRe[static_cast<size_t>(uIdx)];
                    const double ui = fftIm[static_cast<size_t>(uIdx)];

                    fftRe[static_cast<size_t>(uIdx)] = ur + tr;
                    fftIm[static_cast<size_t>(uIdx)] = ui + ti;
                    fftRe[static_cast<size_t>(vIdx)] = ur - tr;
                    fftIm[static_cast<size_t>(vIdx)] = ui - ti;
                }
            }
        }
    }

    void analyzeHarmonics(double trackedF0, double confidence, int numHarmonics) noexcept
    {
        const int nHarm = std::clamp(numHarmonics, 1, kMaxHarmonics);

        if (confidence < 0.15 || trackedF0 < 35.0)
        {
            targetAmps.fill(0.0);
            return;
        }

        // Copy latest kFftSize samples with Hann window
        for (int i = 0; i < kFftSize; ++i)
        {
            const int rIdx = (writePos + i) & (kFftSize - 1);
            fftRe[static_cast<size_t>(i)] = ring[static_cast<size_t>(rIdx)] * hannWin[static_cast<size_t>(i)];
            fftIm[static_cast<size_t>(i)] = 0.0;
        }

        runFft();

        // Coherent gain of Hann window = 0.5 -> scale by 4.0 / N for single-sided sine amplitude
        const double ampScale = 4.0 / static_cast<double>(kFftSize);
        for (int b = 0; b < kNumBins; ++b)
            magSpec[static_cast<size_t>(b)] = std::hypot(fftRe[static_cast<size_t>(b)],
                                                         fftIm[static_cast<size_t>(b)]) * ampScale;

        const double binHz = fs / static_cast<double>(kFftSize);
        const double f0Bin = trackedF0 / binHz;

        // Extract peak amplitude for each harmonic k = 1 .. nHarm
        for (int k = 0; k < kMaxHarmonics; ++k)
        {
            const int hk = k + 1;
            const double hkFreq = static_cast<double>(hk) * trackedF0;

            if (hk > nHarm || hkFreq >= 0.42 * fs)
            {
                targetAmps[static_cast<size_t>(k)] = 0.0;
                continue;
            }

            const double centerBin = hkFreq / binHz;
            // Tight local search radius: stays strictly inside +/- 34% of f0 spacing, capped at +/- 3 bins
            const int searchRad = std::max(1, std::min(3, static_cast<int>(std::floor(0.34 * f0Bin))));
            const int bLo = std::clamp(static_cast<int>(std::round(centerBin)) - searchRad, 1, kNumBins - 2);
            const int bHi = std::clamp(static_cast<int>(std::round(centerBin)) + searchRad, 1, kNumBins - 2);

            int peakBin = bLo;
            double peakVal = magSpec[static_cast<size_t>(bLo)];
            for (int b = bLo + 1; b <= bHi; ++b)
            {
                if (magSpec[static_cast<size_t>(b)] > peakVal)
                {
                    peakVal = magSpec[static_cast<size_t>(b)];
                    peakBin = b;
                }
            }

            // Quadratic peak interpolation for exact amplitude regardless of bin alignment
            const double m0 = magSpec[static_cast<size_t>(peakBin - 1)];
            const double m1 = magSpec[static_cast<size_t>(peakBin)];
            const double m2 = magSpec[static_cast<size_t>(peakBin + 1)];
            const double denom = (m0 - 2.0 * m1 + m2);
            double interpMag = m1;
            if (denom < -1e-12)
            {
                const double delta = std::clamp(0.5 * (m0 - m2) / denom, -0.5, 0.5);
                interpMag = std::max(m1, m1 - 0.25 * (m0 - m2) * delta);
            }

            // Gentle local inter-harmonic valley noise-floor subtraction
            const int vLeft  = std::clamp(static_cast<int>(std::round(centerBin - 0.5 * f0Bin)), 1, kNumBins - 2);
            const int vRight = std::clamp(static_cast<int>(std::round(centerBin + 0.5 * f0Bin)), 1, kNumBins - 2);
            const double localFloor = 0.5 * (magSpec[static_cast<size_t>(vLeft)] + magSpec[static_cast<size_t>(vRight)]);
            const double cleanAmp = std::max(0.0, interpMag - 0.65 * localFloor);

            targetAmps[static_cast<size_t>(k)] = cleanAmp * confidence;
        }
    }

    double fs = 48000.0;
    int hopSize = 256;
    double envAtkAlpha = 0.004;
    double envRelAlpha = 0.001;

    std::array<double, kFftSize> hannWin {};
    std::array<int, kFftSize>    bitRev {};
    std::array<double, kNumBins> twRe {}, twIm {};

    std::array<double, kFftSize> ring {};
    std::array<double, kFftSize> fftRe {}, fftIm {};
    std::array<double, kNumBins> magSpec {};

    int writePos = 0;
    int hopCounter = 0;

    std::array<double, kMaxHarmonics> targetAmps {};
    std::array<double, kMaxHarmonics> smoothAmps {};
};

} // namespace choirfairy
