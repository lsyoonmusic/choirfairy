#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "LookAndFeelChoirFairy.h"

class ChoirFairyAudioProcessorEditor : public juce::AudioProcessorEditor,
                                       private juce::Timer
{
public:
    explicit ChoirFairyAudioProcessorEditor(ChoirFairyAudioProcessor&);
    ~ChoirFairyAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void setupKnob(juce::Slider& s, const juce::String& suffix, uint32_t accentHex);

    ChoirFairyAudioProcessor& processorRef;
    LookAndFeelChoirFairy laf;
    ChoirSpectrumView spectrumView;

    // Card 1: Surgical Tracking Low-Cut & Pitch Range
    juce::ComboBox slopeBox, phaseModeBox;
    juce::Slider offsetKnob, floorKnob, minF0Knob, maxF0Knob;

    // Card 2: 20-Harmonic SMS Stereo Choir Engine
    juce::ComboBox voiceModeBox;
    juce::Slider numHarmKnob, detuneKnob, timingKnob, driftKnob;
    juce::Slider airBlendKnob, tiltKnob, widthKnob, mixKnob;

    // Card 3: Audition & Output
    juce::ComboBox auditionBox;
    juce::Slider outGainKnob;
    juce::ToggleButton bypassBtn { "BYPASS" };

    using SAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using CAtt = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using BAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<CAtt> attSlope, attPhaseMode;
    std::unique_ptr<SAtt> attOffset, attFloor, attMinF0, attMaxF0;
    std::unique_ptr<CAtt> attVoiceMode;
    std::unique_ptr<SAtt> attNumHarm, attDetune, attTiming, attDrift;
    std::unique_ptr<SAtt> attAirBlend, attTilt, attWidth, attMix;
    std::unique_ptr<CAtt> attAudition;
    std::unique_ptr<SAtt> attOutGain;
    std::unique_ptr<BAtt> attBypass;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChoirFairyAudioProcessorEditor)
};
