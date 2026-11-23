#include "PluginEditor.h"

ChoirFairyAudioProcessorEditor::ChoirFairyAudioProcessorEditor(ChoirFairyAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p), spectrumView(p)
{
    setLookAndFeel(&laf);
    setSize(960, 588);

    addAndMakeVisible(spectrumView);

    // Card 1: Surgical Tracking Low-Cut & Pitch Range
    slopeBox.addItemList({ "OFF", "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct", "BRICKWALL" }, 1);
    addAndMakeVisible(slopeBox);
    phaseModeBox.addItemList({ "LINEAR PHASE", "MIN PHASE" }, 1);
    addAndMakeVisible(phaseModeBox);

    setupKnob(offsetKnob, " st", 0xffff6e5c);
    setupKnob(floorKnob,  " Hz", 0xffff6e5c);
    setupKnob(minF0Knob,  " Hz", 0xfff0c467);
    setupKnob(maxF0Knob,  " Hz", 0xfff0c467);

    // Card 2: 20-Harmonic SMS Stereo Choir Engine
    voiceModeBox.addItemList({ "2-VOICE (Tight)", "4-VOICE (Choir)" }, 1);
    addAndMakeVisible(voiceModeBox);

    setupKnob(numHarmKnob,  "",    0xffb29cff);
    setupKnob(detuneKnob,   " ct", 0xffb29cff);
    setupKnob(timingKnob,   " ms", 0xffb29cff);
    setupKnob(driftKnob,    " %",  0xffb29cff);

    setupKnob(airBlendKnob, " %",  0xff5ce6ff);
    setupKnob(tiltKnob,     " dB", 0xff5ce6ff);
    setupKnob(widthKnob,    " %",  0xff5ce6ff);
    setupKnob(mixKnob,      " %",  0xff58f5c8);

    // Card 3: Audition & Output
    auditionBox.addItemList({ "NORMAL (Dry+Wet)", "WET ONLY (Doubles)", "SINES ONLY (H1-HN)", "AIR ONLY (>HN)" }, 1);
    addAndMakeVisible(auditionBox);
    setupKnob(outGainKnob, " dB", 0xffc6d1e6);
    addAndMakeVisible(bypassBtn);

    auto& a = processorRef.apvts;
    attSlope     = std::make_unique<CAtt>(a, "lowCutSlope",   slopeBox);
    attPhaseMode = std::make_unique<CAtt>(a, "lowCutPhase",   phaseModeBox);
    attOffset    = std::make_unique<SAtt>(a, "lowCutOffset",  offsetKnob);
    attFloor     = std::make_unique<SAtt>(a, "lowCutFloor",   floorKnob);
    attMinF0     = std::make_unique<SAtt>(a, "minF0",         minF0Knob);
    attMaxF0     = std::make_unique<SAtt>(a, "maxF0",         maxF0Knob);

    attVoiceMode = std::make_unique<CAtt>(a, "voiceMode",     voiceModeBox);
    attNumHarm   = std::make_unique<SAtt>(a, "numHarmonics",  numHarmKnob);
    attDetune    = std::make_unique<SAtt>(a, "detuneCents",   detuneKnob);
    attTiming    = std::make_unique<SAtt>(a, "timingMs",      timingKnob);
    attDrift     = std::make_unique<SAtt>(a, "driftPct",      driftKnob);

    attAirBlend  = std::make_unique<SAtt>(a, "airBlendPct",   airBlendKnob);
    attTilt      = std::make_unique<SAtt>(a, "tiltDb",        tiltKnob);
    attWidth     = std::make_unique<SAtt>(a, "widthPct",      widthKnob);
    attMix       = std::make_unique<SAtt>(a, "doublerMixPct", mixKnob);

    attAudition  = std::make_unique<CAtt>(a, "audition",      auditionBox);
    attOutGain   = std::make_unique<SAtt>(a, "outGain",       outGainKnob);
    attBypass    = std::make_unique<BAtt>(a, "bypass",        bypassBtn);

    startTimerHz(30);
}

ChoirFairyAudioProcessorEditor::~ChoirFairyAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

void ChoirFairyAudioProcessorEditor::setupKnob(juce::Slider& s, const juce::String& suffix, uint32_t accentHex)
{
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 15);
    s.setTextValueSuffix(suffix);
    s.setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xffe1e8f5));
    s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0x550b0e16));
    s.getProperties().set(LookAndFeelChoirFairy::kAccentColourProp, static_cast<int>(accentHex));
    s.setRotaryParameters(juce::degreesToRadians(220.0f), juce::degreesToRadians(500.0f), true);
    addAndMakeVisible(s);
}

void ChoirFairyAudioProcessorEditor::timerCallback()
{
    spectrumView.updateFrame(
        slopeBox.getSelectedItemIndex(),
        phaseModeBox.getSelectedItemIndex(),
        static_cast<int>(std::round(numHarmKnob.getValue())),
        static_cast<float>(airBlendKnob.getValue())
    );
}

void ChoirFairyAudioProcessorEditor::paint(juce::Graphics& g)
{
    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());

    juce::ColourGradient bg(juce::Colour(0xff191d2a), 0.0f, 0.0f,
                            juce::Colour(0xff10131c), 0.0f, h, false);
    g.setGradientFill(bg);
    g.fillAll();

    // Header Bar
    g.setColour(juce::Colour(0xff0d1018));
    g.fillRect(0.0f, 0.0f, w, 44.0f);
    g.setColour(juce::Colour(0xff2c3347));
    g.drawHorizontalLine(44, 0.0f, w);

    g.setColour(juce::Colour(0xff808ca8));
    g.setFont(juce::Font(juce::FontOptions(9.8f, juce::Font::bold)));
    g.drawText("ASIMORPH's FELLAS", 20, 7, 200, 12, juce::Justification::centredLeft);

    g.setColour(juce::Colour(0xfff3f5fc));
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.drawText("CHOIR FAIRY  v2", 20, 19, 210, 21, juce::Justification::centredLeft);

    g.setColour(juce::Colour(0xffb5a3ff));
    g.setFont(juce::Font(juce::FontOptions(9.8f, juce::Font::bold)));
    g.drawText("HQ LOOKAHEAD SMS 20-HARMONIC CHOIR  +  LINEAR-PHASE BRICKWALL LOW-CUT  |  PDC ALIGNED",
               230, 14, 710, 16, juce::Justification::centredRight);

    // Three Module Cards
    auto drawCard = [&g](juce::Rectangle<float> r, const juce::String& title, juce::Colour accent)
    {
        g.setColour(juce::Colour(0xff141824));
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(juce::Colour(0xff2d354a));
        g.drawRoundedRectangle(r, 6.0f, 1.0f);

        g.setColour(accent);
        g.fillRoundedRectangle(r.getX() + 10.0f, r.getY() + 9.0f, 3.0f, 12.0f, 1.5f);

        g.setColour(juce::Colour(0xffe6ebf7));
        g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
        g.drawText(title, static_cast<int>(r.getX() + 18), static_cast<int>(r.getY() + 7),
                   static_cast<int>(r.getWidth() - 26), 16, juce::Justification::centredLeft);
    };

    drawCard({ 16.0f,  270.0f, 256.0f, 302.0f }, "SURGICAL LOW-CUT & f0",   juce::Colour(0xffff6e5c));
    drawCard({ 284.0f, 270.0f, 452.0f, 302.0f }, "HQ LOOKAHEAD SMS CHOIR",  juce::Colour(0xffb29cff));
    drawCard({ 748.0f, 270.0f, 196.0f, 302.0f }, "MONITOR & OUTPUT",        juce::Colour(0xff5ce6ff));

    // Inner section dividers
    g.setColour(juce::Colour(0xff252c3e));
    g.drawHorizontalLine(464, 28.0f,  260.0f);
    g.drawHorizontalLine(438, 298.0f, 722.0f);

    auto lbl = [&g](const juce::String& txt, int x, int y, int width, juce::Colour col = juce::Colour(0xff9aa7c2))
    {
        g.setColour(col);
        g.setFont(juce::Font(juce::FontOptions(8.9f, juce::Font::bold)));
        g.drawText(txt, x, y, width, 13, juce::Justification::centred);
    };

    // Card 1 Labels
    lbl("SLOPE",   30, 298, 110, juce::Colour(0xffff968a));
    lbl("PHASE",  148, 298, 110, juce::Colour(0xffff968a));
    lbl("OFFSET",  38, 354,  96);
    lbl("FLOOR",  150, 354,  96);
    lbl("MIN f0",  38, 470,  96, juce::Colour(0xfff0c467));
    lbl("MAX f0", 150, 470,  96, juce::Colour(0xfff0c467));

    // Card 2 Labels (Header Voice selector + 2x4 Knob Grid)
    lbl("ENSEMBLE",   570, 277, 64, juce::Colour(0xffc7b8ff));

    lbl("HARMONICS",  296, 316, 100, juce::Colour(0xffc7b8ff));
    lbl("DETUNE",     404, 316, 100, juce::Colour(0xffc7b8ff));
    lbl("TIMING",     512, 316, 100, juce::Colour(0xffc7b8ff));
    lbl("HUMAN DRIFT",620, 316, 100, juce::Colour(0xffc7b8ff));

    lbl("AIR BLEND",  296, 446, 100, juce::Colour(0xff7eeeff));
    lbl("TONE TILT",  404, 446, 100, juce::Colour(0xff7eeeff));
    lbl("WIDTH",      512, 446, 100, juce::Colour(0xff7eeeff));
    lbl("DOUBLER MIX",620, 446, 100, juce::Colour(0xff6bf5cf));

    // Card 3 Labels
    lbl("AUDITION MODE", 762, 300, 168, juce::Colour(0xff7eeeff));
    lbl("OUTPUT GAIN",   792, 366, 108, juce::Colour(0xffc6d1e6));

    g.setColour(juce::Colour(0xff6f7c98));
    g.setFont(juce::Font(juce::FontOptions(8.6f, juce::Font::plain)));
    g.drawFittedText("4096-pt 8x WOLA Linear-Phase\n5-Frame Lookahead f0 Guard\nMcAulay-Quatieri Birth/Death\n(Zero Pitch-Jump Warble)",
                     758, 472, 176, 54, juce::Justification::centred, 4);
}

void ChoirFairyAudioProcessorEditor::resized()
{
    spectrumView.setBounds(16, 54, 928, 206);

    // Card 1: Surgical Low-Cut & Pitch Range
    slopeBox.setBounds(30,     314, 110, 26);
    phaseModeBox.setBounds(148, 314, 110, 26);
    offsetKnob.setBounds(42,   370,  88, 86);
    floorKnob.setBounds(154,   370,  88, 86);
    minF0Knob.setBounds(42,    484,  88, 80);
    maxF0Knob.setBounds(154,   484,  88, 80);

    // Card 2: HQ Lookahead SMS Choir Engine
    voiceModeBox.setBounds(636, 274, 92, 22);

    numHarmKnob.setBounds(300,  332, 92, 96);
    detuneKnob.setBounds(408,   332, 92, 96);
    timingKnob.setBounds(516,   332, 92, 96);
    driftKnob.setBounds(624,    332, 92, 96);

    airBlendKnob.setBounds(300, 462, 92, 100);
    tiltKnob.setBounds(408,     462, 92, 100);
    widthKnob.setBounds(516,    462, 92, 100);
    mixKnob.setBounds(624,      462, 92, 100);

    // Card 3: Monitor & Output
    auditionBox.setBounds(762, 318, 168, 28);
    outGainKnob.setBounds(796, 380, 100, 88);
    bypassBtn.setBounds(784,   532, 124, 28);
}
