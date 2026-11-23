#include "PluginProcessor.h"

using APVTS = juce::AudioProcessorValueTreeState;

// ---------------------------------------------------------------- parameters
APVTS::ParameterLayout PsykoticAudioProcessor::createLayout()
{
    using P  = juce::AudioParameterFloat;
    using NR = juce::NormalisableRange<float>;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

    p.push_back (std::make_unique<P> ("drive", "Drive (dB)",  NR (0.f, 48.f, 0.1f), 24.f));
    p.push_back (std::make_unique<P> ("hard",  "Hard Clip",   NR (0.f, 1.f, 0.001f), 0.7f));
    p.push_back (std::make_unique<P> ("fold",  "Fold",        NR (0.f, 1.f, 0.001f), 0.25f));
    p.push_back (std::make_unique<P> ("bias",  "Asymetrie",   NR (0.f, 0.6f, 0.001f), 0.1f));
    p.push_back (std::make_unique<P> ("rate",  "Crush Rate",  NR (1.f, 40.f, 0.1f, 0.5f), 4.f));
    p.push_back (std::make_unique<P> ("bits",  "Crush Bits",  NR (3.f, 16.f, 0.1f), 10.f));
    p.push_back (std::make_unique<P> ("hpf",   "Pre HPF (Hz)", NR (20.f, 2000.f, 1.f, 0.4f), 120.f));
    p.push_back (std::make_unique<P> ("lpf",   "Post LPF (Hz)", NR (500.f, 20000.f, 1.f, 0.4f), 9000.f));
    p.push_back (std::make_unique<P> ("out",   "Output (dB)", NR (-30.f, 6.f, 0.1f), -12.f));
    p.push_back (std::make_unique<P> ("mix",   "Mix",         NR (0.f, 1.f, 0.001f), 1.f));

    return { p.begin(), p.end() };
}

PsykoticAudioProcessor::PsykoticAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

bool PsykoticAudioProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && out == l.getMainInputChannelSet();
}

// ---------------------------------------------------------------- lifecycle
void PsykoticAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock,
                                  (juce::uint32) getTotalNumOutputChannels() };

    preHighpass.prepare (spec);  preHighpass.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    postLowpass.prepare (spec);  postLowpass.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    dcBlock.prepare (spec);      dcBlock.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    dcBlock.setCutoffFrequency (20.f);

    oversampling.initProcessing ((size_t) samplesPerBlock);
    oversampling.reset();

    mixer.prepare (spec);
    mixer.setWetLatency (oversampling.getLatencyInSamples());
    setLatencySamples (juce::roundToInt (oversampling.getLatencyInSamples()));

    for (int c = 0; c < 2; ++c) { held[c] = 0.f; counter[c] = 0.f; }
}

// ---------------------------------------------------------------- waveshaper
static inline float shape (float x, float bias, float fold, float hard)
{
    x += bias;                                   // asymetrie => harmoniques paires, plus "sale"
    const float soft = std::tanh (x);            // saturation douce
    const float clip = juce::jlimit (-1.f, 1.f, x); // ecretage dur
    float y = soft + (clip - soft) * hard;       // blend soft <-> hard

    if (fold > 0.f)                              // wavefolder : replie le signal sur lui-meme
    {
        const float f = 1.f + fold * 4.f;
        const float folded = std::sin (y * f * juce::MathConstants<float>::halfPi);
        y += (folded - y) * fold;
    }
    return y;
}

// ---------------------------------------------------------------- audio
void PsykoticAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    const float drive = juce::Decibels::decibelsToGain (get ("drive"));
    const float hard  = get ("hard");
    const float fold  = get ("fold");
    const float bias  = get ("bias");
    const float rate  = get ("rate");
    const float bits  = get ("bits");
    const float outG  = juce::Decibels::decibelsToGain (get ("out"));

    preHighpass.setCutoffFrequency (get ("hpf"));
    postLowpass.setCutoffFrequency (get ("lpf"));
    mixer.setWetMixProportion (get ("mix"));

    const int nCh = juce::jmin (buffer.getNumChannels(), 2);
    const int nS  = buffer.getNumSamples();

    juce::dsp::AudioBlock<float> block (buffer);
    mixer.pushDrySamples (block);

    // 1) drive + filtre passe-haut (garde les basses "serrees" avant la distorsion)
    for (int ch = 0; ch < nCh; ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        for (int i = 0; i < nS; ++i)
            d[i] = preHighpass.processSample (ch, d[i] * drive);
    }

    // 2) distorsion en 4x surechantillonnage (moins d'aliasing parasite)
    auto up = oversampling.processSamplesUp (block);
    for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
    {
        auto* d = up.getChannelPointer (ch);
        for (size_t i = 0; i < up.getNumSamples(); ++i)
            d[i] = shape (d[i], bias, fold, hard);
    }
    oversampling.processSamplesDown (block);

    // 3) bitcrusher (sample-and-hold + quantification) -> grain lo-fi hyperpop
    const float levels = std::pow (2.f, bits) * 0.5f;
    for (int ch = 0; ch < nCh; ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        for (int i = 0; i < nS; ++i)
        {
            counter[ch] -= 1.f;
            if (counter[ch] <= 0.f)
            {
                held[ch] = std::round (juce::jlimit (-1.f, 1.f, d[i]) * levels) / levels;
                counter[ch] += rate;
            }
            d[i] = held[ch];
        }
    }

    // 4) filtre passe-bas (adoucit le haut), anti-DC, volume de sortie, securite
    for (int ch = 0; ch < nCh; ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        for (int i = 0; i < nS; ++i)
        {
            float x = postLowpass.processSample (ch, d[i]);
            x = dcBlock.processSample (ch, x);
            d[i] = juce::jlimit (-1.f, 1.f, x * outG);
        }
    }

    mixer.mixWetSamples (block);
}

// ---------------------------------------------------------------- editor / state
juce::AudioProcessorEditor* PsykoticAudioProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);   // UI auto-générée avec tous les potards
}

void PsykoticAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void PsykoticAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PsykoticAudioProcessor();
}
