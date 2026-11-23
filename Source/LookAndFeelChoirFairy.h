#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"
#include <array>
#include <cmath>

class LookAndFeelChoirFairy : public juce::LookAndFeel_V4
{
public:
    static constexpr const char* kAccentColourProp = "cfAccentCol";

    LookAndFeelChoirFairy()
    {
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff151822));
        setColour(juce::ComboBox::textColourId,       juce::Colour(0xffe9edf7));
        setColour(juce::ComboBox::outlineColourId,    juce::Colour(0xff394359));
        setColour(juce::ComboBox::arrowColourId,      juce::Colour(0xffa994ff));
        setColour(juce::PopupMenu::backgroundColourId,            juce::Colour(0xff171b26));
        setColour(juce::PopupMenu::textColourId,                  juce::Colour(0xffe9edf7));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(0xff463b73));
        setColour(juce::PopupMenu::highlightedTextColourId,       juce::Colour(0xffffffff));
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider& slider) override
    {
        const auto b = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                              static_cast<float>(width), static_cast<float>(height));
        const float cx = b.getCentreX();
        const float cy = b.getCentreY() + 1.0f;
        const float r  = juce::jmin(b.getWidth(), b.getHeight()) * 0.35f;
        const float angle = startAngle + sliderPos * (endAngle - startAngle);

        const uint32_t colHex = static_cast<uint32_t>(
            static_cast<int>(slider.getProperties().getWithDefault(kAccentColourProp, static_cast<int>(0xffa894ff))));
        const juce::Colour accent(colHex);

        // Background track arc
        const float arcR = r + 5.2f;
        juce::Path track;
        track.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour(juce::Colour(0xff272e3e));
        g.strokePath(track, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Active value arc
        if (sliderPos > 0.002f)
        {
            juce::Path valArc;
            valArc.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, angle, true);
            g.setColour(accent);
            g.strokePath(valArc, juce::PathStrokeType(2.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Knob shadow & matte body
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillEllipse(cx - r + 1.0f, cy - r + 2.5f, r * 2.0f, r * 2.0f);

        juce::ColourGradient body(juce::Colour(0xff2e3444), cx - r * 0.5f, cy - r * 0.5f,
                                  juce::Colour(0xff13161f), cx + r * 0.6f, cy + r * 0.7f, false);
        g.setGradientFill(body);
        g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
        g.setColour(juce::Colour(0xff4c556b));
        g.drawEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f, 1.0f);

        // Pointer tick
        const float ptrA = angle - juce::MathConstants<float>::halfPi;
        g.setColour(accent.brighter(0.32f));
        g.drawLine(cx + std::cos(ptrA) * (r * 0.28f), cy + std::sin(ptrA) * (r * 0.28f),
                   cx + std::cos(ptrA) * (r * 0.88f), cy + std::sin(ptrA) * (r * 0.88f), 2.3f);
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& btn,
                          bool isHighlighted, bool) override
    {
        auto b = btn.getLocalBounds().toFloat().reduced(1.5f);
        const bool isOn = btn.getToggleState();

        g.setColour(isOn ? juce::Colour(0xff7d2b2b) : juce::Colour(0xff1c202c));
        g.fillRoundedRectangle(b, 4.0f);
        g.setColour(isHighlighted ? juce::Colour(0xffc2b0ff)
                                  : (isOn ? juce::Colour(0xffe05a5a) : juce::Colour(0xff3e475d)));
        g.drawRoundedRectangle(b, 4.0f, 1.2f);

        g.setColour(isOn ? juce::Colour(0xffffdede) : juce::Colour(0xffced6e8));
        g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
        g.drawText(btn.getButtonText(), b.toNearestInt(), juce::Justification::centred);
    }
};

/**
 * Real-Time 20 Hz - 16 kHz Dual-Trace Spectrum & 20-Harmonic Choir Visualizer.
 * Displays:
 *   - Input spectrum (amber) vs Output stereo choir spectrum (lavender-cyan)
 *   - Steep Pitch-Tracking Butterworth Low-Cut transfer curve (up to 96 dB/oct!)
 *   - All 20 tracked harmonic energy pillars (H1 .. H20)
 *   - Dynamic Air Crossover line where timing-offset original high-band air begins
 */
class ChoirSpectrumView : public juce::Component
{
public:
    explicit ChoirSpectrumView(ChoirFairyAudioProcessor& proc)
        : processorRef(proc),
          fft(ChoirFairyAudioProcessor::kFftOrder)
    {
        constexpr int N = ChoirFairyAudioProcessor::kFftSize;
        for (int i = 0; i < N; ++i)
            hannWin[static_cast<size_t>(i)] = 0.5f * (1.0f - std::cos(6.2831853f * static_cast<float>(i) / static_cast<float>(N - 1)));

        smoothInDb.fill(-96.0f);
        smoothOutDb.fill(-96.0f);
    }

    void updateFrame(int lowCutSlopeIdx, int numHarmonics, float airBlendPct)
    {
        slopeIndex  = lowCutSlopeIdx;
        activeHarms = numHarmonics;
        airBlend    = airBlendPct * 0.01f;

        trackedF0  = processorRef.getTrackedF0();
        confidence = processorRef.getConfidence();
        lowCutHz   = processorRef.getLowCutCutoffHz();
        xoverHz    = processorRef.getXoverHz();
        sampleRate = static_cast<float>(processorRef.getCurrentSampleRate());

        for (int k = 0; k < ChoirFairyAudioProcessor::kMaxHarmonics; ++k)
            harmAmps[static_cast<size_t>(k)] = processorRef.getHarmonicAmp(k);

        computeSpectrum(processorRef.getInputScope(), smoothInDb);
        computeSpectrum(processorRef.getOutputScope(), smoothOutDb);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();

        juce::ColourGradient bg(juce::Colour(0xff0f121b), b.getX(), b.getY(),
                                juce::Colour(0xff090b12), b.getX(), b.getBottom(), false);
        g.setGradientFill(bg);
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(juce::Colour(0xff30384c));
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.2f);

        auto plot = b.reduced(12.0f, 10.0f);
        plot.removeFromTop(21.0f);
        plot.removeFromBottom(16.0f);

        const float fMin = 20.0f;
        const float fMax = 16000.0f;
        const float logMin = std::log(fMin);
        const float logMax = std::log(fMax);

        auto freqToX = [&](float f) -> float
        {
            const float norm = (std::log(juce::jlimit(fMin, fMax, f)) - logMin) / (logMax - logMin);
            return plot.getX() + norm * plot.getWidth();
        };

        auto dbToY = [&](float db) -> float
        {
            const float norm = juce::jlimit(0.0f, 1.0f, (db - (-84.0f)) / 84.0f);
            return plot.getBottom() - norm * plot.getHeight();
        };

        // 1. Frequency & dB Grid
        static constexpr float gridFreqs[] = { 30, 50, 80, 100, 200, 300, 500, 1000, 2000, 4000, 8000, 12000 };
        g.setFont(juce::Font(juce::FontOptions(8.8f, juce::Font::plain)));
        for (float gf : gridFreqs)
        {
            const float gx = freqToX(gf);
            g.setColour(juce::Colour(0xff1d2332));
            g.drawVerticalLine(static_cast<int>(gx), plot.getY(), plot.getBottom());

            g.setColour(juce::Colour(0xff6f7c96));
            const juce::String lbl = (gf >= 1000.0f) ? (juce::String(static_cast<int>(gf / 1000.0f)) + "k")
                                                     : juce::String(static_cast<int>(gf));
            g.drawText(lbl, static_cast<int>(gx - 15), static_cast<int>(plot.getBottom() + 2), 30, 13,
                       juce::Justification::centred);
        }

        for (float db = -72.0f; db <= -12.0f; db += 12.0f)
        {
            const float gy = dbToY(db);
            g.setColour(juce::Colour(0xff191e2b));
            g.drawHorizontalLine(static_cast<int>(gy), plot.getX(), plot.getRight());
        }

        g.saveState();
        g.reduceClipRegion(plot.toNearestInt());

        // 2. Shade High-Band Stereo Air Region above X-Over
        if (xoverHz >= fMin && xoverHz < fMax && airBlend > 0.01f)
        {
            const float xx = freqToX(xoverHz);
            g.setColour(juce::Colour(0xff52e0ff).withAlpha(0.055f * std::min(airBlend, 1.0f)));
            g.fillRect(xx, plot.getY(), plot.getRight() - xx, plot.getHeight());
        }

        // 3. Dual-Trace Spectra (Input Amber + Output Lavender-Cyan)
        auto buildSpecPath = [&](const std::array<float, kBins>& dbArr, bool closeFill) -> juce::Path
        {
            juce::Path path;
            const int numPts = 260;
            for (int i = 0; i <= numPts; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(numPts);
                const float f = std::exp(logMin + t * (logMax - logMin));
                const float binF = f * static_cast<float>(ChoirFairyAudioProcessor::kFftSize) / std::max(sampleRate, 8000.0f);
                const int b0 = juce::jlimit(1, kBins - 2, static_cast<int>(binF));
                const float frac = binF - static_cast<float>(b0);
                const float db = dbArr[static_cast<size_t>(b0)] * (1.0f - frac)
                               + dbArr[static_cast<size_t>(b0 + 1)] * frac;

                const float px = plot.getX() + t * plot.getWidth();
                const float py = dbToY(db);
                if (i == 0) path.startNewSubPath(px, py);
                else        path.lineTo(px, py);
            }
            if (closeFill)
            {
                path.lineTo(plot.getRight(), plot.getBottom());
                path.lineTo(plot.getX(), plot.getBottom());
                path.closeSubPath();
            }
            return path;
        };

        auto inFill = buildSpecPath(smoothInDb, true);
        g.setColour(juce::Colour(0xffe09c5a).withAlpha(0.11f));
        g.fillPath(inFill);
        auto inStroke = buildSpecPath(smoothInDb, false);
        g.setColour(juce::Colour(0xffe09c5a).withAlpha(0.40f));
        g.strokePath(inStroke, juce::PathStrokeType(1.0f));

        auto outFill = buildSpecPath(smoothOutDb, true);
        g.setColour(juce::Colour(0xff9b85ff).withAlpha(0.16f));
        g.fillPath(outFill);
        auto outStroke = buildSpecPath(smoothOutDb, false);
        g.setColour(juce::Colour(0xffb5a3ff));
        g.strokePath(outStroke, juce::PathStrokeType(1.5f));

        // 4. Live 20-Harmonic Tracked Pillars (H1 .. H20)
        if (confidence > 0.10f)
        {
            g.setFont(juce::Font(juce::FontOptions(8.2f, juce::Font::bold)));
            for (int k = 0; k < activeHarms; ++k)
            {
                const float hkFreq = trackedF0 * static_cast<float>(k + 1);
                if (hkFreq < fMin || hkFreq > fMax)
                    continue;

                const float hx = freqToX(hkFreq);
                const float ampDb = 20.0f * std::log10(std::max(harmAmps[static_cast<size_t>(k)], 1e-5f));
                const float hy = dbToY(ampDb);

                const juce::Colour pCol = (k == 0)
                    ? juce::Colour(0xffffd166)
                    : juce::Colour(0xffb29cff).interpolatedWith(juce::Colour(0xff5be8e0), static_cast<float>(k) / 19.0f);

                const float barW = (k < 4) ? 3.2f : 2.0f;
                g.setColour(pCol.withAlpha(0.75f * confidence));
                g.fillRect(hx - barW * 0.5f, hy, barW, std::max(0.0f, plot.getBottom() - hy));

                // Label key landmarks (H1, H2, H3, H5, H10, H15, H20) so upper harmonics don't overlap text
                if (k == 0 || k == 1 || k == 2 || k == 4 || k == 9 || k == 14 || k == activeHarms - 1)
                {
                    g.setColour(pCol.withAlpha(0.92f * confidence));
                    g.drawText("H" + juce::String(k + 1),
                               static_cast<int>(hx - 12), static_cast<int>(plot.getY() + 1), 24, 11,
                               juce::Justification::centred);
                }
            }
        }

        // 5. Dynamic Air Crossover Dashed Boundary
        if (xoverHz >= fMin && xoverHz <= fMax)
        {
            const float xx = freqToX(xoverHz);
            g.setColour(juce::Colour(0xff67e4ff).withAlpha(0.65f));
            const float dashes[2] = { 4.0f, 4.0f };
            g.drawDashedLine(juce::Line<float>(xx, plot.getY() + 13.0f, xx, plot.getBottom()),
                             dashes, 2, 1.2f);
            g.setFont(juce::Font(juce::FontOptions(8.2f, juce::Font::bold)));
            g.drawText("AIR >", static_cast<int>(xx + 3), static_cast<int>(plot.getY() + 14), 38, 11,
                       juce::Justification::centredLeft);
        }

        // 6. Live Surgical Pitch-Tracking Low-Cut Curve (up to 96 dB/oct = 16th order!)
        if (slopeIndex > 0)
        {
            static constexpr int orders[6] = { 2, 4, 6, 8, 12, 16 };
            const int order = orders[juce::jlimit(0, 5, slopeIndex - 1)];

            juce::Path lcCurve;
            const int pts = 220;
            for (int i = 0; i <= pts; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(pts);
                const float f = std::exp(logMin + t * (logMax - logMin));
                const float ratio = lowCutHz / std::max(f, 1.0f);
                // Guard std::pow overflow on 16th-order (2*order = 32)
                const float logRatio = std::log10(std::max(ratio, 1e-4f));
                const float db = (ratio <= 1.0f)
                    ? (-10.0f * std::log10(1.0f + std::pow(ratio, static_cast<float>(2 * order))))
                    : (-10.0f * std::log10(1.0f + std::pow(10.0f, std::min(12.0f, static_cast<float>(2 * order) * logRatio))));

                const float py = dbToY(-6.0f + db);
                const float px = plot.getX() + t * plot.getWidth();
                if (i == 0) lcCurve.startNewSubPath(px, py);
                else        lcCurve.lineTo(px, py);
            }
            g.setColour(juce::Colour(0xffff6e5c));
            g.strokePath(lcCurve, juce::PathStrokeType(2.2f));

            const float cx = freqToX(lowCutHz);
            const float cy = dbToY(-9.0f);
            g.setColour(juce::Colour(0xffff6e5c));
            g.fillEllipse(cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
        }

        g.restoreState();

        // 7. Top Live Telemetry Header Strip
        const auto topStrip = juce::Rectangle<float>(b.getX() + 14.0f, b.getY() + 7.0f, b.getWidth() - 28.0f, 18.0f);
        const bool locked = (confidence > 0.30f);

        g.setColour(locked ? juce::Colour(0xff58f5c8) : juce::Colour(0xff424c5f));
        g.fillEllipse(topStrip.getX(), topStrip.getCentreY() - 4.0f, 8.0f, 8.0f);

        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.setColour(locked ? juce::Colour(0xffffdf8a) : juce::Colour(0xff7f8ca6));
        const juce::String noteStr = locked
            ? ("f0 LOCK: " + freqToNoteName(trackedF0) + "  (" + juce::String(trackedF0, 1) + " Hz)")
            : "f0: UNVOICED / STANDBY";
        g.drawText(noteStr, static_cast<int>(topStrip.getX() + 14), static_cast<int>(topStrip.getY()),
                   230, 18, juce::Justification::centredLeft);

        static constexpr const char* slopeNames[7] = { "OFF", "12dB", "24dB", "36dB", "48dB", "72dB", "96dB" };
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.setColour(juce::Colour(0xffff8a7a));
        g.drawText("LOW-CUT: " + juce::String(lowCutHz, 1) + " Hz (" + slopeNames[juce::jlimit(0, 6, slopeIndex)] + ")",
                   static_cast<int>(topStrip.getX() + 255), static_cast<int>(topStrip.getY()),
                   195, 18, juce::Justification::centredLeft);

        g.setColour(juce::Colour(0xff7ceaff));
        g.drawText("H1-H" + juce::String(activeHarms) + " SINES  |  AIR > " + juce::String(xoverHz, 0) + " Hz",
                   static_cast<int>(topStrip.getX() + 465), static_cast<int>(topStrip.getY()),
                   235, 18, juce::Justification::centredLeft);

        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        g.setColour(juce::Colour(0xffe09c5a));
        g.drawText("IN", static_cast<int>(topStrip.getRight() - 78), static_cast<int>(topStrip.getY()),
                   34, 18, juce::Justification::centredRight);
        g.setColour(juce::Colour(0xffb5a3ff));
        g.drawText("OUT", static_cast<int>(topStrip.getRight() - 38), static_cast<int>(topStrip.getY()),
                   38, 18, juce::Justification::centredRight);
    }

private:
    static constexpr int kBins = ChoirFairyAudioProcessor::kFftSize / 2;

    void computeSpectrum(const LockFreeAudioScope<ChoirFairyAudioProcessor::kScopeCapacity>& scope,
                         std::array<float, kBins>& targetDb)
    {
        constexpr int N = ChoirFairyAudioProcessor::kFftSize;
        fftWork.fill(0.0f);
        scope.copyLatest(fftWork.data(), N);

        for (int i = 0; i < N; ++i)
            fftWork[static_cast<size_t>(i)] *= hannWin[static_cast<size_t>(i)];

        fft.performFrequencyOnlyForwardTransform(fftWork.data());

        const float norm = 2.0f / static_cast<float>(N);
        for (int b = 0; b < kBins; ++b)
        {
            const float mag = fftWork[static_cast<size_t>(b)] * norm;
            const float db  = juce::jlimit(-96.0f, 0.0f, 20.0f * std::log10(std::max(mag, 1e-5f)));
            const float cur = targetDb[static_cast<size_t>(b)];
            const float alpha = (db > cur) ? 0.55f : 0.22f;
            targetDb[static_cast<size_t>(b)] = cur + alpha * (db - cur);
        }
    }

    static juce::String freqToNoteName(float hz)
    {
        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        if (hz < 16.0f) return "---";
        const int midi = static_cast<int>(std::round(69.0 + 12.0 * std::log2(hz / 440.0)));
        const int note = ((midi % 12) + 12) % 12;
        const int oct  = (midi / 12) - 1;
        return juce::String(names[note]) + juce::String(oct);
    }

    ChoirFairyAudioProcessor& processorRef;
    juce::dsp::FFT fft;
    std::array<float, ChoirFairyAudioProcessor::kFftSize> hannWin {};
    std::array<float, ChoirFairyAudioProcessor::kFftSize * 2> fftWork {};
    std::array<float, kBins> smoothInDb {};
    std::array<float, kBins> smoothOutDb {};

    float sampleRate  = 48000.0f;
    float trackedF0   = 180.0f;
    float confidence  = 0.0f;
    float lowCutHz    = 60.0f;
    float xoverHz     = 3600.0f;
    int   slopeIndex  = 3;
    int   activeHarms = 20;
    float airBlend    = 0.85f;
    std::array<float, ChoirFairyAudioProcessor::kMaxHarmonics> harmAmps {};
};
