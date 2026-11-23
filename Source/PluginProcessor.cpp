#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

ChoirFairyAudioProcessor::ChoirFairyAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "ChoirFairyV2Params", createParameterLayout())
{
    pMinF0     = apvts.getRawParameterValue("minF0");
    pMaxF0     = apvts.getRawParameterValue("maxF0");
    pSlope     = apvts.getRawParameterValue("lowCutSlope");
    pPhaseMode = apvts.getRawParameterValue("lowCutPhase");
    pOffsetSt  = apvts.getRawParameterValue("lowCutOffset");
    pFloorHz   = apvts.getRawParameterValue("lowCutFloor");

    pVoiceMode = apvts.getRawParameterValue("voiceMode");
    pNumHarm   = apvts.getRawParameterValue("numHarmonics");
    pDetune    = apvts.getRawParameterValue("detuneCents");
    pTiming    = apvts.getRawParameterValue("timingMs");
    pDrift     = apvts.getRawParameterValue("driftPct");
    pAirBlend  = apvts.getRawParameterValue("airBlendPct");
    pTilt      = apvts.getRawParameterValue("tiltDb");
    pWidth     = apvts.getRawParameterValue("widthPct");
    pMix       = apvts.getRawParameterValue("doublerMixPct");

    pAudition  = apvts.getRawParameterValue("audition");
    pOutGain   = apvts.getRawParameterValue("outGain");
    pBypass    = apvts.getRawParameterValue("bypass");

    for (auto& a : harmAmpAtomic)
        a.store(0.0f, std::memory_order_relaxed);
}

ChoirFairyAudioProcessor::~ChoirFairyAudioProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout ChoirFairyAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // 1. Pitch Range & Surgical Tracking Low-Cut
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "minF0", 1 }, "Min Pitch",
        juce::NormalisableRange<float>(45.0f, 300.0f, 1.0f, 0.5f), 75.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "maxF0", 1 }, "Max Pitch",
        juce::NormalisableRange<float>(250.0f, 1200.0f, 1.0f, 0.5f), 680.0f));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "lowCutSlope", 1 }, "Low-Cut Slope",
        juce::StringArray { "OFF", "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct", "BRICKWALL" }, 4));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "lowCutPhase", 1 }, "Low-Cut Phase",
        juce::StringArray { "LINEAR PHASE", "MIN PHASE" }, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "lowCutOffset", 1 }, "Low-Cut Offset",
        juce::NormalisableRange<float>(-12.0f, 0.0f, 0.1f), -3.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "lowCutFloor", 1 }, "Unvoiced Floor",
        juce::NormalisableRange<float>(20.0f, 250.0f, 1.0f, 0.5f), 60.0f));

    // 2. 20-Harmonic SMS Stereo Choir Doubler
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "voiceMode", 1 }, "Choir Voices",
        juce::StringArray { "2-VOICE (Tight)", "4-VOICE (Choir)" }, 1));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "numHarmonics", 1 }, "Harmonics", 4, 20, 20));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "detuneCents", 1 }, "Detune Spread",
        juce::NormalisableRange<float>(0.0f, 35.0f, 0.2f), 11.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "timingMs", 1 }, "Stereo Timing",
        juce::NormalisableRange<float>(2.0f, 35.0f, 0.2f), 16.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "driftPct", 1 }, "Human Drift",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 45.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "airBlendPct", 1 }, "Air Blend (>HN)",
        juce::NormalisableRange<float>(0.0f, 150.0f, 1.0f), 85.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "tiltDb", 1 }, "Harmonic Tilt",
        juce::NormalisableRange<float>(-6.0f, 6.0f, 0.1f), -1.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "widthPct", 1 }, "Stereo Width",
        juce::NormalisableRange<float>(0.0f, 150.0f, 1.0f), 115.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "doublerMixPct", 1 }, "Doubler Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.5f), 45.0f));

    // 3. Audition & Output
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "audition", 1 }, "Audition",
        juce::StringArray { "NORMAL (Dry+Wet)", "WET ONLY (Doubles)", "SINES ONLY (H1-HN)", "AIR ONLY (>HN)" }, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "outGain", 1 }, "Output Gain",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return { params.begin(), params.end() };
}

void ChoirFairyAudioProcessor::prepareToPlay(double sampleRate, int)
{
    const double fs = std::max(sampleRate, 8000.0);
    currentFsAtomic.store(fs, std::memory_order_relaxed);

    engine.prepare(fs);
    const int lat = engine.getLatencySamples();
    latencyAtomic.store(lat, std::memory_order_relaxed);
    setLatencySamples(lat); // Exact DAW PDC compensation (4096 @ 48kHz = 85.3ms)

    inputScope.reset();
    outputScope.reset();

    const double rampSec = 0.015;
    smOffsetSt.reset(fs, rampSec);
    smFloorHz.reset(fs, rampSec);
    smDetune.reset(fs, rampSec);
    smTiming.reset(fs, 0.035);
    smDrift.reset(fs, rampSec);
    smAirBlend.reset(fs, rampSec);
    smTilt.reset(fs, rampSec);
    smWidth.reset(fs, rampSec);
    smMix.reset(fs, rampSec);
    smOutGain.reset(fs, rampSec);

    smOffsetSt.setCurrentAndTargetValue(pOffsetSt->load());
    smFloorHz.setCurrentAndTargetValue(pFloorHz->load());
    smDetune.setCurrentAndTargetValue(pDetune->load());
    smTiming.setCurrentAndTargetValue(pTiming->load());
    smDrift.setCurrentAndTargetValue(pDrift->load() * 0.01);
    smAirBlend.setCurrentAndTargetValue(pAirBlend->load() * 0.01);
    smTilt.setCurrentAndTargetValue(pTilt->load());
    smWidth.setCurrentAndTargetValue(pWidth->load() * 0.01);
    smMix.setCurrentAndTargetValue(pMix->load() * 0.01);
    smOutGain.setCurrentAndTargetValue(pOutGain->load());
}

void ChoirFairyAudioProcessor::releaseResources()
{
    engine.reset();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool ChoirFairyAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto outSet = layouts.getMainOutputChannelSet();
    const auto inSet  = layouts.getMainInputChannelSet();
    if (outSet != juce::AudioChannelSet::mono() && outSet != juce::AudioChannelSet::stereo())
        return false;
    if (inSet != juce::AudioChannelSet::mono() && inSet != juce::AudioChannelSet::stereo())
        return false;
    return true;
}
#endif

void ChoirFairyAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numInCh    = getTotalNumInputChannels();
    const int numOutCh   = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    for (int i = std::max(2, numInCh); i < numOutCh; ++i)
        buffer.clear(i, 0, numSamples);

    if (numSamples <= 0 || numInCh <= 0 || numOutCh <= 0)
        return;

    smOffsetSt.setTargetValue(pOffsetSt->load());
    smFloorHz.setTargetValue(pFloorHz->load());
    smDetune.setTargetValue(pDetune->load());
    smTiming.setTargetValue(pTiming->load());
    smDrift.setTargetValue(pDrift->load() * 0.01);
    smAirBlend.setTargetValue(pAirBlend->load() * 0.01);
    smTilt.setTargetValue(pTilt->load());
    smWidth.setTargetValue(pWidth->load() * 0.01);
    smMix.setTargetValue(pMix->load() * 0.01);
    smOutGain.setTargetValue(pOutGain->load());

    choirfairy2::EngineParams p;
    p.minF0Hz        = pMinF0->load();
    p.maxF0Hz        = pMaxF0->load();
    p.lowCutSlope    = static_cast<choirfairy2::LowCutSlope>(static_cast<int>(std::round(pSlope->load())));
    p.lowCutPhase    = static_cast<choirfairy2::FilterPhaseMode>(static_cast<int>(std::round(pPhaseMode->load())));
    p.fourVoiceChoir = pVoiceMode->load() > 0.5f;
    p.numHarmonics   = static_cast<int>(std::round(pNumHarm->load()));
    p.audition       = static_cast<choirfairy2::AuditionMode>(static_cast<int>(std::round(pAudition->load())));
    p.bypass         = pBypass->load() > 0.5f;

    const bool hasInR  = (numInCh >= 2);
    const bool hasOutR = (numOutCh >= 2);

    float* chL = buffer.getWritePointer(0);
    float* chR = hasOutR ? buffer.getWritePointer(1) : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        p.lowCutOffsetSt = smOffsetSt.getNextValue();
        p.lowCutFloorHz  = smFloorHz.getNextValue();
        p.detuneCents    = smDetune.getNextValue();
        p.timingMs       = smTiming.getNextValue();
        p.driftAmount    = smDrift.getNextValue();
        p.airBlend       = smAirBlend.getNextValue();
        p.tiltDb         = smTilt.getNextValue();
        p.stereoWidth    = smWidth.getNextValue();
        p.doublerMix     = smMix.getNextValue();
        p.outputGainDb   = smOutGain.getNextValue();

        double l = static_cast<double>(chL[i]);
        double r = hasInR ? static_cast<double>(chR[i]) : l;

        const float inMono = static_cast<float>(0.5 * (l + r));
        engine.processStereoSample(l, r, p);
        const float outMono = static_cast<float>(0.5 * (l + r));

        if (hasOutR)
        {
            chL[i] = static_cast<float>(l);
            chR[i] = static_cast<float>(r);
        }
        else
        {
            chL[i] = outMono;
        }

        inputScope.push(inMono);
        outputScope.push(outMono);
    }

    trackedF0Atomic.store(static_cast<float>(engine.getTrackedF0()), std::memory_order_relaxed);
    confidenceAtomic.store(static_cast<float>(engine.getConfidence()), std::memory_order_relaxed);
    lowCutHzAtomic.store(static_cast<float>(engine.getLowCutCutoffHz()), std::memory_order_relaxed);
    xoverHzAtomic.store(static_cast<float>(engine.getAirXoverHz()), std::memory_order_relaxed);

    for (int k = 0; k < kMaxHarmonics; ++k)
        harmAmpAtomic[static_cast<size_t>(k)].store(
            static_cast<float>(engine.getHarmonicAmp(k)), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* ChoirFairyAudioProcessor::createEditor()
{
    return new ChoirFairyAudioProcessorEditor(*this);
}

void ChoirFairyAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void ChoirFairyAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ChoirFairyAudioProcessor();
}
