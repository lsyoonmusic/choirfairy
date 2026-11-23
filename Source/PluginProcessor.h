#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DSP/ChoirFairyEngine.h"
#include <array>
#include <atomic>

template <int Capacity>
class LockFreeAudioScope
{
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    void reset() noexcept
    {
        buffer.fill(0.0f);
        writePos.store(0, std::memory_order_relaxed);
    }

    inline void push(float sample) noexcept
    {
        const int idx = writePos.load(std::memory_order_relaxed);
        buffer[static_cast<size_t>(idx)] = sample;
        writePos.store((idx + 1) & (Capacity - 1), std::memory_order_release);
    }

    void copyLatest(float* dest, int numSamples) const noexcept
    {
        const int end = writePos.load(std::memory_order_acquire);
        int start = (end - numSamples) & (Capacity - 1);
        for (int i = 0; i < numSamples; ++i)
        {
            dest[i] = buffer[static_cast<size_t>(start)];
            start = (start + 1) & (Capacity - 1);
        }
    }

private:
    std::array<float, Capacity> buffer {};
    std::atomic<int> writePos { 0 };
};

class ChoirFairyAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kScopeCapacity = 8192;
    static constexpr int kFftOrder      = 12;   // 4096-pt display FFT
    static constexpr int kFftSize       = 1 << kFftOrder;
    static constexpr int kMaxHarmonics  = choirfairy::HarmonicAnalyzer20::kMaxHarmonics;

    ChoirFairyAudioProcessor();
    ~ChoirFairyAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
   #endif

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.15; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Lock-free telemetry for UI
    double getCurrentSampleRate() const noexcept  { return currentFsAtomic.load(std::memory_order_relaxed); }
    float  getTrackedF0() const noexcept          { return trackedF0Atomic.load(std::memory_order_relaxed); }
    float  getConfidence() const noexcept         { return confidenceAtomic.load(std::memory_order_relaxed); }
    float  getLowCutCutoffHz() const noexcept     { return lowCutHzAtomic.load(std::memory_order_relaxed); }
    float  getXoverHz() const noexcept            { return xoverHzAtomic.load(std::memory_order_relaxed); }
    float  getHarmonicAmp(int k) const noexcept
    {
        return harmAmpAtomic[static_cast<size_t>(juce::jlimit(0, kMaxHarmonics - 1, k))].load(std::memory_order_relaxed);
    }

    const LockFreeAudioScope<kScopeCapacity>& getInputScope() const noexcept  { return inputScope; }
    const LockFreeAudioScope<kScopeCapacity>& getOutputScope() const noexcept { return outputScope; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    choirfairy::ChoirFairyEngine engine;

    // Smoothed continuous parameters
    juce::SmoothedValue<double> smOffsetSt, smFloorHz;
    juce::SmoothedValue<double> smDetune, smTiming, smDrift, smAirBlend, smTilt, smWidth, smMix, smOutGain;

    // Cached raw parameter pointers
    std::atomic<float>* pMinF0      = nullptr;
    std::atomic<float>* pMaxF0      = nullptr;
    std::atomic<float>* pSlope      = nullptr;
    std::atomic<float>* pOffsetSt   = nullptr;
    std::atomic<float>* pFloorHz    = nullptr;

    std::atomic<float>* pNumHarm    = nullptr;
    std::atomic<float>* pDetune     = nullptr;
    std::atomic<float>* pTiming     = nullptr;
    std::atomic<float>* pDrift      = nullptr;
    std::atomic<float>* pAirBlend   = nullptr;
    std::atomic<float>* pTilt       = nullptr;
    std::atomic<float>* pWidth      = nullptr;
    std::atomic<float>* pMix        = nullptr;

    std::atomic<float>* pAudition   = nullptr;
    std::atomic<float>* pOutGain    = nullptr;
    std::atomic<float>* pBypass     = nullptr;

    // Lock-free telemetry atomics
    std::atomic<double> currentFsAtomic  { 48000.0 };
    std::atomic<float>  trackedF0Atomic  { 180.0f };
    std::atomic<float>  confidenceAtomic { 0.0f };
    std::atomic<float>  lowCutHzAtomic   { 60.0f };
    std::atomic<float>  xoverHzAtomic    { 3600.0f };
    std::array<std::atomic<float>, kMaxHarmonics> harmAmpAtomic {};

    LockFreeAudioScope<kScopeCapacity> inputScope;
    LockFreeAudioScope<kScopeCapacity> outputScope;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChoirFairyAudioProcessor)
};
