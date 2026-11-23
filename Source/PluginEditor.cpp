#include "PluginEditor.h"

ChoirFairyAudioProcessorEditor::ChoirFairyAudioProcessorEditor(ChoirFairyAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p), spectrumView(p)
{
    setLookAndFeel(&laf);
    setSize(940, 566);

    addAndMakeVisible(spectrumView);

    // Card 1: Surgical Tracking Low-Cut & Pitch Range
    slopeBox.addItemList({ "OFF", "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct" }, 1);
    addAndMakeVisible(slopeBox);
    setupKnob(offsetKnob, " st", 0xffff6e5c);
    setupKnob(floorKnob,  " Hz", 0xffff6e5c);
    setupKnob(minF0Knob,  " Hz", 0xfff0c467);
    setupKnob(maxF0Knob,  " Hz", 0xfff0c467);

    // Card 2: 20-Harmonic Stereo Choir Doubler
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
    attSlope    = std::make_unique<CAtt>(a, "lowCutSlope",   slopeBox);
    attOffset   = std::make_unique<SAtt>(a, "lowCutOffset",  offsetKnob);
    attFloor    = std::make_unique<SAtt>(a, "lowCutFloor",   floorKnob);
    attMinF0    = std::make_unique<SAtt>(a, "minF0",         minF0Knob);
    attMaxF0    = std::make_unique<SAtt>(a, "maxF0",         maxF0Knob);

    attNumHarm  = std::make_unique<SAtt>(a, "numHarmonics",  numHarmKnob);
    attDetune   = std::make_unique<SAtt>(a, "detuneCents",   detuneKnob);
    attTiming   = std::make_unique<SAtt>(a, "timingMs",      timingKnob);
    attDrift    = std::make_unique<SAtt>(a, "driftPct",      driftKnob);

    attAirBlend = std::make_unique<SAtt>(a, "airBlendPct",   airBlendKnob);
    attTilt     = std::make_unique<SAtt>(a, "tiltDb",        tiltKnob);
    attWidth    = std::make_unique<SAtt>(a, "widthPct",      widthKnob);
    attMix      = std::make_unique<SAtt>(a, "doublerMixPct", mixKnob);

    attAudition = std::make_unique<CAtt>(a, "audition",      auditionBox);
    attOutGain  = std::make_unique<SAtt>(a, "outGain",       outGainKnob);
    attBypass   = std::make_unique<BAtt>(a, "bypass",        bypassBtn);

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
        static_cast<int>(std::round(numHarmKnob.getValue())),
        static_cast<float>(airBlendKnob.getValue())
    );
}

void ChoirFairyAudioProcessorEditor::paint(juce::Graphics& g)
{
    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());

    // Deep twilight slate chassis
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
    g.drawText("ASIMORPH's FELLAS", 20, 7, 190, 12, juce::Justification::centredLeft);

    g.setColour(juce::Colour(0xfff3f5fc));
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.drawText("CHOIR FAIRY", 20, 19, 190, 21, juce::Justification::centredLeft);

    g.setColour(juce::Colour(0xffb5a3ff));
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawText("20-HARMONIC STEREO DOUBLER  +  96 dB/oct TRACKING LOW-CUT  |  PING-PONG JUMP SNAP",
               210, 14, 710, 16, juce::Justification::centredRight);

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

    drawCard({ 16.0f,  268.0f, 240.0f, 282.0f }, "SURGICAL LOW-CUT & f0",   juce::Colour(0xffff6e5c));
    drawCard({ 268.0f, 268.0f, 444.0f, 282.0f }, "20-HARMONIC CHOIR ENGINE", juce::Colour(0xffb29cff));
    drawCard({ 724.0f, 268.0f, 200.0f, 282.0f }, "MONITOR & OUTPUT",        juce::Colour(0xff5ce6ff));

    // Horizontal inner dividers
    g.setColour(juce::Colour(0xff252c3e));
    g.drawHorizontalLine(444, 28.0f,  244.0f); // Low-Cut vs Pitch Range
    g.drawHorizontalLine(416, 282.0f, 698.0f); // Harmonic Synthesis vs Air/Space

    auto lbl = [&g](const juce::String& txt, int x, int y, int width, juce::Colour col = juce::Colour(0xff9aa7c2))
    {
        g.setColour(col);
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        g.drawText(txt, x, y, width, 13, juce::Justification::centred);
    };

    // Card 1 Labels
    lbl("SLOPE (UP TO 96 dB/oct)", 36, 296, 200, juce::Colour(0xffff968a));
    lbl("OFFSET",  34, 344, 96);
    lbl("FLOOR",  142, 344, 96);
    lbl("MIN f0",  34, 450, 96, juce::Colour(0xfff0c467));
    lbl("MAX f0", 142, 450, 96, juce::Colour(0xfff0c467));

    // Card 2 Labels (Top Row: 20-Harmonic Resynthesis)
    lbl("HARMONICS",  282, 298, 98, juce::Colour(0xffc7b8ff));
    lbl("DETUNE",     388, 298, 98, juce::Colour(0xffc7b8ff));
    lbl("TIMING",     494, 298, 98, juce::Colour(0xffc7b8ff));
    lbl("HUMAN DRIFT",600, 298, 98, juce::Colour(0xffc7b8ff));

    // Card 2 Labels (Bottom Row: High-Band Air & Stereo Field)
    lbl("AIR BLEND",  282, 424, 98, juce::Colour(0xff7eeeff));
    lbl("TONE TILT",  388, 424, 98, juce::Colour(0xff7eeeff));
    lbl("WIDTH",      494, 424, 98, juce::Colour(0xff7eeeff));
    lbl("DOUBLER MIX",600, 424, 98, juce::Colour(0xff6bf5cf));

    // Card 3 Labels
    lbl("AUDITION MODE", 740, 298, 168, juce::Colour(0xff7eeeff));
    lbl("OUTPUT GAIN",   770, 360, 108, juce::Colour(0xffc6d1e6));

    g.setColour(juce::Colour(0xff6b7894));
    g.setFont(juce::Font(juce::FontOptions(8.6f, juce::Font::plain)));
    g.drawFittedText("H1-HN: Ping-Pong 20-Sine Choir\n>HN: Delayed Original Stereo Air\nDry Path: Zero-Latency Center",
                     736, 462, 176, 42, juce::Justification::centred, 3);
}

void ChoirFairyAudioProcessorEditor::resized()
{
    // Top 20 Hz - 16 kHz Spectrum & 20-Harmonic Visualizer
    spectrumView.setBounds(16, 54, 908, 204);

    // Card 1: Low-Cut & Pitch Range
    slopeBox.setBounds(38, 312, 196, 25);
    offsetKnob.setBounds(38,  358, 88, 82);
    floorKnob.setBounds(146,  358, 88, 82);
    minF0Knob.setBounds(38,   464, 88, 80);
    maxF0Knob.setBounds(146,  464, 88, 80);

    // Card 2: 20-Harmonic Choir Engine (2x4 Grid)
    numHarmKnob.setBounds(286,  314, 90, 94);
    detuneKnob.setBounds(392,   314, 90, 94);
    timingKnob.setBounds(498,   314, 90, 94);
    driftKnob.setBounds(604,    314, 90, 94);

    airBlendKnob.setBounds(286, 440, 90, 98);
    tiltKnob.setBounds(392,     440, 90, 98);
    widthKnob.setBounds(498,    440, 90, 98);
    mixKnob.setBounds(604,      440, 90, 98);

    // Card 3: Monitor & Output
    auditionBox.setBounds(740, 316, 168, 28);
    outGainKnob.setBounds(774, 374, 100, 86);
    bypassBtn.setBounds(762,   510, 124, 28);
}
