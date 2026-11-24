#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    using Param = juce::AudioProcessorValueTreeState;

    juce::String hzToText (float hz)
    {
        if (hz >= 1000.0f)
        {
            // 6k3 = 6300 Hz, 12k = 12000 Hz
            const int hundreds = juce::roundToInt (hz / 100.0f);
            const int k = hundreds / 10, rest = hundreds % 10;
            return juce::String (k) + "k" + (rest != 0 ? juce::String (rest) : juce::String());
        }
        return juce::String (juce::roundToInt (hz));
    }

    float textToHz (const juce::String& raw)
    {
        auto t = raw.trim().toLowerCase().replace (",", ".").removeCharacters (" hz");
        if (t.containsChar ('k'))
        {
            const auto left = t.upToFirstOccurrenceOf ("k", false, false);
            const auto right = t.fromFirstOccurrenceOf ("k", false, false);
            if (left.containsChar ('.'))
                return left.getFloatValue() * 1000.0f;
            return left.getFloatValue() * 1000.0f + (right.isNotEmpty() ? right.getFloatValue() * std::pow (10.0f, 3.0f - (float) right.length()) : 0.0f);
        }
        return t.getFloatValue();
    }

    juce::NormalisableRange<float> logRange (float lo, float hi, float interval = 0.0f)
    {
        juce::NormalisableRange<float> r (lo, hi, interval);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    }

    std::unique_ptr<juce::AudioParameterFloat> floatParam (const juce::String& id, const juce::String& name,
                                                           juce::NormalisableRange<float> range, float def,
                                                           std::function<juce::String (float, int)> toText,
                                                           std::function<float (const juce::String&)> fromText = {})
    {
        auto attr = juce::AudioParameterFloatAttributes().withStringFromValueFunction (std::move (toText));
        if (fromText)
            attr = attr.withValueFromStringFunction (std::move (fromText));
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, def, attr);
    }

    auto oneDecimal = [] (float v, int) { return juce::String (v, 1); };
    auto percent    = [] (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; };
    auto decibels   = [] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; };
    auto millis     = [] (float v, int) { return v < 10.0f ? juce::String (v, 1) + " ms" : juce::String (juce::roundToInt (v)) + " ms"; };
    auto hertz      = [] (float v, int) { return hzToText (v); };
    auto balanceTxt = [] (float v, int)
    {
        const int a = v <= 0.0f ? 100 : juce::roundToInt (100.0f - v);
        const int b = v >= 0.0f ? 100 : juce::roundToInt (100.0f + v);
        return juce::String (a) + " / " + juce::String (b);
    };
}

const juce::StringArray& PhlickAudioProcessor::bandTypeNames()
{
    static const juce::StringArray names { "PEAK", "LOW SHELF", "HIGH SHELF", "BAND SHELF", "REJECT", "TILT" };
    return names;
}

const juce::StringArray& PhlickAudioProcessor::slopeNames()
{
    static const juce::StringArray names { "6 dB", "12 dB", "24 dB", "48 dB" };
    return names;
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout PhlickAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    auto add = [&layout] (auto p) { layout.add (std::move (p)); };

    // Principal
    add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { modeId, 1 }, "Mode", juce::StringArray { "CUT", "BOOST" }, 0));
    add (floatParam (depthCutId, "Depth Cut", { 0.0f, 20.0f, 0.01f }, 5.0f, oneDecimal));
    add (floatParam (depthBoostId, "Depth Boost", { 0.0f, 20.0f, 0.01f }, 5.0f, oneDecimal));
    add (floatParam (sensitivityId, "Sensitivity", { 0.0f, 10.0f, 0.01f }, 5.0f, oneDecimal));
    add (floatParam (sharpnessId, "Sharpness", { 0.0f, 10.0f, 0.01f }, 3.0f, oneDecimal));

    // Retour
    add (floatParam (decayId, "Decay", { 0.0f, 10.0f, 0.01f }, 3.0f, oneDecimal));
    add (floatParam (decayTiltId, "Decay LF/HF", { 0.0f, 10.0f, 0.01f }, 5.0f, oneDecimal));

    // Stéréo
    add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { stereoModeId, 1 }, "Stereo Mode", juce::StringArray { "L/R", "M/S" }, 0));
    add (floatParam (linkId, "Stereo Link", { 0.0f, 100.0f, 0.1f }, 100.0f, percent));
    add (floatParam (balanceId, "Stereo Balance", { -100.0f, 100.0f, 0.1f }, 0.0f, balanceTxt));

    // Qualité
    add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { windowId, 1 }, "Window", juce::StringArray { "SHORT", "MEDIUM", "LONG" }, 1));
    add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { phaseId, 1 }, "Phase", juce::StringArray { "LINEAR", "MINIMUM" }, 0));
    add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { oversampleId, 1 }, "Oversample", juce::StringArray { "1X", "2X", "4X" }, 0));
    add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { resolutionId, 1 }, "Resolution", juce::StringArray { "ECO", "NORMAL", "HIGH", "ULTRA" }, 1));
    add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { offlineOversampleId, 1 }, "Offline Oversample", juce::StringArray { "SAME", "1X", "2X", "4X" }, 0));
    add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { offlineResolutionId, 1 }, "Offline Resolution", juce::StringArray { "SAME", "ECO", "NORMAL", "HIGH", "ULTRA" }, 0));

    // Sidechain et sortie
    add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { sidechainId, 1 }, "Sidechain", false));
    add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { sidechainListenId, 1 }, "Sidechain Listen", false));
    add (floatParam (mixId, "Mix", { 0.0f, 100.0f, 0.1f }, 100.0f, percent));
    add (floatParam (trimId, "Trim", { -24.0f, 12.0f, 0.01f }, 0.0f, decibels));
    add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { deltaId, 1 }, "Delta", false));
    add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { bypassId, 1 }, "Bypass", false));

    // Coupures de la courbe de sensibilité
    add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { lowCutId, 1 }, "Low Cut", false));
    add (floatParam (lowCutFreqId, "Low Cut Freq", logRange (minFreq, maxFreq, 0.1f), 30.0f, hertz, textToHz));
    add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { lowCutSlopeId, 1 }, "Low Cut Slope", slopeNames(), 1));
    add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { highCutId, 1 }, "High Cut", false));
    add (floatParam (highCutFreqId, "High Cut Freq", logRange (minFreq, maxFreq, 0.1f), 18000.0f, hertz, textToHz));
    add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { highCutSlopeId, 1 }, "High Cut Slope", slopeNames(), 1));

    // Bandes
    struct BandDefault { int type; float freq, sens, q; };
    static constexpr BandDefault defaults[numBands] = {
        { phocus::peak,      200.0f,   0.0f, 1.0f },
        { phocus::peak,      800.0f,   0.0f, 1.0f },
        { phocus::peak,      5000.0f,  0.0f, 1.0f },
        { phocus::highShelf, 12000.0f, 0.0f, 0.7f } };

    for (int b = 0; b < numBands; ++b)
    {
        const auto name = "Band " + juce::String (b + 1) + " ";
        add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { bandId (b, "On"), 1 }, name + "On", true));
        add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { bandId (b, "Type"), 1 }, name + "Type", bandTypeNames(), defaults[b].type));
        add (floatParam (bandId (b, "Freq"), name + "Freq", logRange (minFreq, maxFreq, 0.1f), defaults[b].freq, hertz, textToHz));
        add (floatParam (bandId (b, "Sens"), name + "Sens", { -24.0f, 24.0f, 0.01f }, defaults[b].sens, decibels));
        add (floatParam (bandId (b, "Q"), name + "Q", logRange (0.1f, 10.0f, 0.001f), defaults[b].q,
                         [] (float v, int) { return juce::String (v, v < 1.0f ? 2 : 1); }));
        add (floatParam (bandId (b, "Balance"), name + "Balance", { -100.0f, 100.0f, 0.1f }, 0.0f, balanceTxt));
    }

    return layout;
}

//==============================================================================
// Points de départ d'usine (réglés à l'oreille... pas encore : réglés sur des mesures, à écouter)
namespace
{
    struct FactoryPreset
    {
        const char* name;
        std::vector<std::pair<juce::String, float>> values;   // le reste : valeur par défaut
    };

    const std::vector<FactoryPreset>& factoryPresets()
    {
        static const std::vector<FactoryPreset> list {
            { "DEFAULT", {} },
            { "MOUTH CLICKS", { { "depthCut", 8.0f }, { "sensitivity", 7.0f }, { "sharpness", 6.0f }, { "decay", 0.5f }, { "window", 0.0f },
                                { "lowCut", 1.0f }, { "lowCutFreq", 1500.0f }, { "lowCutSlope", 2.0f }, { "b3Freq", 4500.0f }, { "b3Sens", 6.0f } } },
            { "PLOSIVES", { { "depthCut", 9.0f }, { "sensitivity", 6.0f }, { "sharpness", 1.0f }, { "decay", 3.0f }, { "decayTilt", 2.0f }, { "phase", 1.0f },
                            { "highCut", 1.0f }, { "highCutFreq", 300.0f }, { "highCutSlope", 2.0f } } },
            { "SOFT PICK", { { "depthCut", 6.0f }, { "sensitivity", 5.0f }, { "sharpness", 4.0f }, { "decay", 1.5f },
                             { "lowCut", 1.0f }, { "lowCutFreq", 1200.0f }, { "highCut", 1.0f }, { "highCutFreq", 9000.0f } } },
            { "TAME HATS", { { "depthCut", 7.0f }, { "sensitivity", 6.0f }, { "sharpness", 2.0f }, { "decay", 1.0f }, { "window", 0.0f },
                             { "lowCut", 1.0f }, { "lowCutFreq", 5000.0f }, { "lowCutSlope", 2.0f } } },
            { "DRUM PUNCH", { { "mode", 1.0f }, { "depthBoost", 6.0f }, { "sensitivity", 5.0f }, { "sharpness", 1.0f }, { "decay", 2.0f }, { "phase", 1.0f } } },
            { "KICK BAM", { { "mode", 1.0f }, { "depthBoost", 7.0f }, { "sensitivity", 4.0f }, { "sharpness", 2.0f }, { "decay", 2.5f }, { "decayTilt", 3.0f },
                            { "phase", 1.0f }, { "highCut", 1.0f }, { "highCutFreq", 4000.0f }, { "b1Freq", 2500.0f }, { "b1Sens", 6.0f } } },
            { "MASTER DYNAMICS", { { "mode", 1.0f }, { "depthBoost", 4.0f }, { "sensitivity", 5.0f }, { "sharpness", 0.0f }, { "decay", 2.0f },
                                   { "mix", 50.0f }, { "phase", 1.0f }, { "resolution", 2.0f } } },
            { "FRETLESS BASS", { { "depthCut", 10.0f }, { "sensitivity", 6.0f }, { "sharpness", 3.0f }, { "decay", 5.0f }, { "decayTilt", 7.0f }, { "window", 2.0f },
                                 { "highCut", 1.0f }, { "highCutFreq", 6000.0f } } },
        };
        return list;
    }
}

int PhlickAudioProcessor::getNumFactoryPresets()                  { return (int) factoryPresets().size(); }
juce::String PhlickAudioProcessor::getFactoryPresetName (int i)   { return factoryPresets()[(size_t) juce::jlimit (0, getNumFactoryPresets() - 1, i)].name; }

void PhlickAudioProcessor::applyFactory (int index)
{
    const auto& preset = factoryPresets()[(size_t) juce::jlimit (0, getNumFactoryPresets() - 1, index)];
    const auto excluded = transientParameters();

    for (auto* p : getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (ranged == nullptr || excluded.contains (ranged->getParameterID()))
            continue;

        float normalised = ranged->getDefaultValue();
        for (const auto& [id, value] : preset.values)
            if (id == ranged->getParameterID())
                normalised = ranged->convertTo0to1 (value);

        ranged->beginChangeGesture();
        ranged->setValueNotifyingHost (normalised);
        ranged->endChangeGesture();
    }
}

int PhlickAudioProcessor::matchingFactory() const
{
    const auto excluded = transientParameters();

    for (int i = 0; i < getNumFactoryPresets(); ++i)
    {
        const auto& preset = factoryPresets()[(size_t) i];
        bool same = true;

        for (auto* p : getParameters())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
            if (ranged == nullptr || excluded.contains (ranged->getParameterID()))
                continue;

            float target = ranged->getDefaultValue();
            for (const auto& [id, value] : preset.values)
                if (id == ranged->getParameterID())
                    target = ranged->convertTo0to1 (value);

            if (std::abs (ranged->getValue() - target) > 1.0e-4f)
            {
                same = false;
                break;
            }
        }

        if (same)
            return i;
    }
    return -1;
}

PresetLibrary::Factory PhlickAudioProcessor::makeFactory()
{
    return { [] { return getNumFactoryPresets(); },
             [] (int i) { return getFactoryPresetName (i); },
             [this] (int i) { applyFactory (i); },
             [this] { return matchingFactory(); } };
}

//==============================================================================
PhlickAudioProcessor::PhlickAudioProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                       .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "Phocus", createParameterLayout()),
      presets (apvts, "Phocus", ".phlpreset", makeFactory(), transientParameters(), "UNTITLED")
{
    presets.restoreFromState();   // projet neuf : DEFAULT
    addListener (this);
}

PhlickAudioProcessor::~PhlickAudioProcessor()
{
    removeListener (this);
}

double PhlickAudioProcessor::getTailLengthSeconds() const
{
    return (double) latency / juce::jmax (1.0, currentSampleRate);
}

bool PhlickAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != out)
        return false;

    if (layouts.inputBuses.size() > 1)
    {
        const auto side = layouts.getChannelSet (true, 1);
        if (! side.isDisabled() && side != juce::AudioChannelSet::mono() && side != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void PhlickAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    const int channels = juce::jlimit (1, 2, getMainBusNumOutputChannels());

    engine.prepare (sampleRate, channels);
    latency = engine.getLatency();
    setLatencySamples (latency);

    for (auto& d : dryDelay)  d.prepare (latency);
    for (auto& d : sideDelay) d.prepare (latency);

    const int block = juce::jmax (1, samplesPerBlock);
    wetBuffer.setSize (2, block, false, false, true);
    dryBuffer.setSize (2, block, false, false, true);
    sideBuffer.setSize (2, block, false, false, true);

    smoothing = 1.0f - std::exp (-1.0f / (0.02f * (float) sampleRate));   // 20 ms
    mixNow = apvts.getRawParameterValue (mixId)->load() * 0.01f;
    trimNow = juce::Decibels::decibelsToGain (apvts.getRawParameterValue (trimId)->load());
    bypassNow = apvts.getRawParameterValue (bypassId)->load() > 0.5f ? 1.0f : 0.0f;
}

void PhlickAudioProcessor::reset()
{
    engine.reset();
    for (auto& d : dryDelay)  d.reset();
    for (auto& d : sideDelay) d.reset();
}

phocus::CurveSpec PhlickAudioProcessor::readCurve() const
{
    auto raw = [this] (const juce::String& id) { return apvts.getRawParameterValue (id)->load(); };

    phocus::CurveSpec c;
    c.lowCut  = { raw (lowCutId) > 0.5f,  raw (lowCutFreqId),  juce::roundToInt (raw (lowCutSlopeId)) };
    c.highCut = { raw (highCutId) > 0.5f, raw (highCutFreqId), juce::roundToInt (raw (highCutSlopeId)) };
    for (int b = 0; b < numBands; ++b)
        c.bands[b] = { raw (bandId (b, "On")) > 0.5f, juce::roundToInt (raw (bandId (b, "Type"))), raw (bandId (b, "Freq")),
                       raw (bandId (b, "Sens")), raw (bandId (b, "Q")), raw (bandId (b, "Balance")) * 0.01f };
    return c;
}

void PhlickAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    const int channels = juce::jlimit (1, 2, getMainBusNumOutputChannels());
    if (n == 0)
        return;

    if (wetBuffer.getNumSamples() < n)
    {
        // L'hôte envoie un bloc plus grand que prévu : rare, on agrandit (pas en temps réel strict).
        wetBuffer.setSize (2, n, false, false, true);
        dryBuffer.setSize (2, n, false, false, true);
        sideBuffer.setSize (2, n, false, false, true);
    }

    auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    // Réglages du moteur (qualité hors ligne si l'hôte rend hors ligne)
    phlick::EngineSettings s;
    s.boost = raw (modeId) > 0.5f;
    s.depth = s.boost ? raw (depthBoostId) : raw (depthCutId);
    s.sensitivity = raw (sensitivityId);
    s.sharpness = raw (sharpnessId);
    s.decay = raw (decayId);
    s.decayTilt = raw (decayTiltId);
    s.window = juce::jlimit (0, 2, juce::roundToInt (raw (windowId)));
    s.minimumPhase = raw (phaseId) > 0.5f;
    int os = juce::roundToInt (raw (oversampleId)), res = juce::roundToInt (raw (resolutionId));
    if (isNonRealtime())
    {
        const int offOs = juce::roundToInt (raw (offlineOversampleId)), offRes = juce::roundToInt (raw (offlineResolutionId));
        if (offOs > 0) os = offOs - 1;
        if (offRes > 0) res = offRes - 1;
    }
    s.oversample = 1 << juce::jlimit (0, 2, os);
    s.resolution = juce::jlimit (0, 3, res);
    s.midSide = raw (stereoModeId) > 0.5f;
    s.link = raw (linkId) * 0.01f;
    s.balance = raw (balanceId) * 0.01f;
    engine.setSettings (s);
    engine.setCurve (readCurve());
    const int listenBand = bandListen.load (std::memory_order_relaxed);
    engine.setListen (listenBand);

    // Entrée et sidechain
    for (int c = 0; c < channels; ++c)
        dryBuffer.copyFrom (c, 0, buffer, c, 0, n);

    float inPeak = 0.0f;
    for (int c = 0; c < channels; ++c)
        inPeak = juce::jmax (inPeak, buffer.getMagnitude (c, 0, n));
    inputPeak.store (juce::jmax (inputPeak.load (std::memory_order_relaxed), inPeak), std::memory_order_relaxed);

    int sideChannels = 0;
    if (getBusCount (true) > 1)
    {
        auto sideBus = getBusBuffer (buffer, true, 1);
        sideChannels = juce::jmin (2, sideBus.getNumChannels());
        for (int c = 0; c < sideChannels; ++c)
            sideBuffer.copyFrom (c, 0, sideBus, c, 0, n);
    }
    sidechainConnected.store (sideChannels > 0, std::memory_order_relaxed);
    const bool useSide = sideChannels > 0 && raw (sidechainId) > 0.5f;
    const bool listenSide = useSide && raw (sidechainListenId) > 0.5f;

    // Mouillé
    const float* in[2] = { dryBuffer.getReadPointer (0), dryBuffer.getReadPointer (channels > 1 ? 1 : 0) };
    const float* side[2] = { sideBuffer.getReadPointer (0), sideBuffer.getReadPointer (sideChannels > 1 ? 1 : 0) };
    float* wet[2] = { wetBuffer.getWritePointer (0), wetBuffer.getWritePointer (1) };
    engine.process (in, useSide ? side : nullptr, sideChannels, wet, n);

    // Sortie : mix, trim (mouillé seulement), delta, écoute, contournement doux
    const float mixTarget = raw (mixId) * 0.01f;
    const float trimTarget = juce::Decibels::decibelsToGain (raw (trimId));
    const float bypassTarget = raw (bypassId) > 0.5f ? 1.0f : 0.0f;
    const bool delta = raw (deltaId) > 0.5f;
    const bool listening = listenBand >= 0;

    float outPeak = 0.0f;
    float mixS = mixNow, trimS = trimNow, bypassS = bypassNow;

    for (int i = 0; i < n; ++i)
    {
        mixS += smoothing * (mixTarget - mixS);
        trimS += smoothing * (trimTarget - trimS);
        bypassS += smoothing * (bypassTarget - bypassS);

        for (int c = 0; c < channels; ++c)
        {
            const float dry = dryDelay[c].process (dryBuffer.getSample (c, i), latency);
            const float sc = sideDelay[c].process (sideChannels > 0 ? sideBuffer.getSample (juce::jmin (c, sideChannels - 1), i) : 0.0f, latency);
            const float w = wetBuffer.getSample (c, i);

            float y;
            if (listenSide)       y = sc;
            else if (listening)   y = w;                       // le moteur rend déjà ce que la bande retire
            else if (delta)       y = mixS * (dry - w);
            else                  y = dry * (1.0f - mixS) + w * mixS * trimS;

            y = bypassS * dry + (1.0f - bypassS) * y;
            buffer.setSample (c, i, y);
            outPeak = juce::jmax (outPeak, std::abs (y));
        }
    }

    mixNow = mixS; trimNow = trimS; bypassNow = bypassS;

    for (int c = channels; c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, n);

    outputPeak.store (juce::jmax (outputPeak.load (std::memory_order_relaxed), outPeak), std::memory_order_relaxed);

    float peakRed = 0.0f;
    for (int i = 0; i < phlick::TransientEngine::displayPoints; ++i)
        peakRed = juce::jmax (peakRed, std::abs (engine.getDisplayGain (i)));
    peakReduction.store (peakRed * mixTarget, std::memory_order_relaxed);
}

void PhlickAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Contournement par l'hôte : le signal sec, retardé de la latence (pas de décalage à la remise en route).
    const int n = buffer.getNumSamples();
    const int channels = juce::jlimit (1, 2, getMainBusNumOutputChannels());
    for (int c = 0; c < channels; ++c)
    {
        auto* d = buffer.getWritePointer (c);
        for (int i = 0; i < n; ++i)
            d[i] = dryDelay[c].process (d[i], latency);
    }
    for (int c = channels; c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, n);
    engine.reset();
}

//==============================================================================
// Historique et A / B
bool PhlickAudioProcessor::isHistoryParameter (const juce::AudioProcessorParameter& p) const
{
    if (auto* ranged = dynamic_cast<const juce::RangedAudioParameter*> (&p))
        return ! transientParameters().contains (ranged->getParameterID());
    return false;
}

PhlickAudioProcessor::Snapshot PhlickAudioProcessor::takeSnapshot() const
{
    Snapshot s;
    for (auto* p : getParameters())
        s.push_back (p->getValue());
    return s;
}

void PhlickAudioProcessor::applySnapshot (const Snapshot& s)
{
    const auto& params = getParameters();
    if (s.size() != (size_t) params.size())
        return;

    const juce::ScopedValueSetter<bool> guard (applyingHistory, true);
    for (int i = 0; i < params.size(); ++i)
    {
        auto* p = params[i];
        if (! isHistoryParameter (*p) || std::abs (p->getValue() - s[(size_t) i]) < 1.0e-6f)
            continue;
        p->beginChangeGesture();
        p->setValueNotifyingHost (s[(size_t) i]);
        p->endChangeGesture();
    }
}

void PhlickAudioProcessor::audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int parameterIndex)
{
    if (applyingHistory || ! juce::MessageManager::existsAndIsCurrentThread())
        return;

    const auto& params = getParameters();
    if (! juce::isPositiveAndBelow (parameterIndex, params.size()) || ! isHistoryParameter (*params[parameterIndex]))
        return;

    // Un chargement de preset touche tous les paramètres en un instant : une seule étape.
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastSnapshotTime < 150 && ! undoStack.empty())
        return;
    lastSnapshotTime = now;

    undoStack.push_back (takeSnapshot());
    if (undoStack.size() > 64)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
}

void PhlickAudioProcessor::undo()
{
    if (undoStack.empty())
        return;
    redoStack.push_back (takeSnapshot());
    const auto s = undoStack.back();
    undoStack.pop_back();
    applySnapshot (s);
}

void PhlickAudioProcessor::redo()
{
    if (redoStack.empty())
        return;
    undoStack.push_back (takeSnapshot());
    const auto s = redoStack.back();
    redoStack.pop_back();
    applySnapshot (s);
}

void PhlickAudioProcessor::switchAb()
{
    slots[abSlot] = takeSnapshot();
    abSlot = 1 - abSlot;
    if (slots[abSlot].empty())
        slots[abSlot] = slots[1 - abSlot];
    applySnapshot (slots[abSlot]);
    apvts.state.setProperty ("abSlot", abSlot, nullptr);
}

void PhlickAudioProcessor::copyAbToOther()
{
    slots[1 - abSlot] = takeSnapshot();
}

//==============================================================================
juce::AudioProcessorEditor* PhlickAudioProcessor::createEditor()
{
    return new PhlickAudioProcessorEditor (*this);
}

void PhlickAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Le réglage A / B qui n'est pas affiché est gardé avec le projet
    slots[abSlot] = takeSnapshot();
    const auto& other = slots[1 - abSlot];
    juce::String text;
    for (auto v : other)
        text << juce::String (v, 6) << " ";
    state.setProperty ("abOther", text.trim(), nullptr);
    state.setProperty ("abSlot", abSlot, nullptr);

    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PhlickAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
    {
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
        presets.restoreFromState();

        abSlot = juce::jlimit (0, 1, (int) apvts.state.getProperty ("abSlot", 0));
        slots[abSlot] = takeSnapshot();

        juce::StringArray values;
        values.addTokens (apvts.state.getProperty ("abOther").toString(), " ", {});
        Snapshot other;
        for (auto& v : values)
            other.push_back (juce::jlimit (0.0f, 1.0f, v.getFloatValue()));
        slots[1 - abSlot] = other.size() == (size_t) getParameters().size() ? other : Snapshot();

        undoStack.clear();
        redoStack.clear();
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PhlickAudioProcessor();
}
