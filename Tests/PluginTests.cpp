// Tests du plugin complet (processeur JUCE + interface), sans DAW.
// Usage : PhlickPluginTests [dossier où écrire les captures de l'interface]

#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <random>

static int failures = 0;

static void check (bool condition, const juce::String& message)
{
    if (! condition)
    {
        ++failures;
        std::cout << "  ECHEC : " << message << std::endl;
    }
}

using Ids = PhlickAudioProcessor;

static PhlickAudioProcessor& asPhlick (juce::AudioProcessor& p)    { return dynamic_cast<PhlickAudioProcessor&> (p); }

static void setParam (juce::AudioProcessor& p, const juce::String& id, float realValue)
{
    auto* param = asPhlick (p).apvts.getParameter (id);
    jassert (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (realValue));
}

static float getParam (juce::AudioProcessor& p, const juce::String& id)
{
    return asPhlick (p).apvts.getRawParameterValue (id)->load();
}

static std::unique_ptr<juce::AudioProcessor> makePlugin (double fs = 48000.0, int block = 512, bool withSidechain = false)
{
    std::unique_ptr<juce::AudioProcessor> p (createPluginFilter());
    auto layout = p->getBusesLayout();
    if (withSidechain && layout.inputBuses.size() > 1)
        layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
    p->setBusesLayout (layout);
    p->setRateAndBufferSizeDetails (fs, block);
    p->prepareToPlay (fs, block);
    return p;
}

/** Traite in (2 canaux, plus 2 de sidechain si présents) bloc par bloc. */
static juce::AudioBuffer<float> run (juce::AudioProcessor& p, const juce::AudioBuffer<float>& in, int block,
                                     const juce::AudioBuffer<float>* side = nullptr)
{
    const int total = p.getTotalNumInputChannels();
    juce::AudioBuffer<float> out (2, in.getNumSamples());
    juce::AudioBuffer<float> io (juce::jmax (2, total), block);
    juce::MidiBuffer midi;

    for (int pos = 0; pos < in.getNumSamples(); pos += block)
    {
        const int n = juce::jmin (block, in.getNumSamples() - pos);
        io.setSize (io.getNumChannels(), n, false, false, true);
        io.clear();
        for (int c = 0; c < 2; ++c)
            io.copyFrom (c, 0, in, c, pos, n);
        if (side != nullptr && total >= 4)
            for (int c = 0; c < 2; ++c)
                io.copyFrom (2 + c, 0, *side, c, pos, n);
        p.processBlock (io, midi);
        for (int c = 0; c < 2; ++c)
            out.copyFrom (c, pos, io, c, 0, n);
    }
    return out;
}

/** Fond continu (bruit filtré) + coups toutes les 250 ms (bruit qui décroît en ~8 ms). */
static juce::AudioBuffer<float> hits (int n, double fs, float hitAmp, unsigned seed = 1, float bgAmp = 0.05f)
{
    juce::AudioBuffer<float> b (2, n);
    std::mt19937 rng (seed);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    float lp = 0.0f;
    const int period = (int) (0.25 * fs);
    for (int i = 0; i < n; ++i)
    {
        lp = 0.85f * lp + 0.15f * nd (rng);
        const float env = std::exp (-(float) (i % period) / (float) (0.008 * fs));
        const float x = bgAmp * 2.0f * lp + hitAmp * env * nd (rng);
        b.setSample (0, i, x);
        b.setSample (1, i, x * 0.9f);
    }
    return b;
}

/** Énergie (dB) sur [a, b] ms après chaque coup (à partir du 3e), décalée de la latence. */
static double hitWindowDb (const juce::AudioBuffer<float>& x, int ch, double fs, double aMs, double bMs, int shift)
{
    const int period = (int) (0.25 * fs);
    double e = 0.0;
    int count = 0;
    for (int onset = 2 * period; onset + period < x.getNumSamples(); onset += period)
        for (int i = onset + shift + (int) (aMs * fs / 1000.0); i < onset + shift + (int) (bMs * fs / 1000.0) && i < x.getNumSamples(); ++i)
        {
            e += (double) x.getSample (ch, i) * x.getSample (ch, i);
            ++count;
        }
    return 10.0 * std::log10 (e / juce::jmax (1, count) + 1.0e-24);
}

//==============================================================================
static void testParameters()
{
    std::cout << "- parametres" << std::endl;
    auto p = makePlugin();
    const auto& params = p->getParameters();
    check (params.size() == 52, "52 parametres attendus, " + juce::String (params.size()));

    juce::StringArray ids;
    for (auto* prm : params)
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
        {
            check (! ids.contains (r->getParameterID()), "identifiant en double " + r->getParameterID());
            ids.add (r->getParameterID());
            const auto text = r->getText (r->getValue(), 32);
            check (text.isNotEmpty(), "texte vide " + r->getParameterID());
        }

    for (auto id : { "mode", "depthCut", "depthBoost", "sensitivity", "sharpness", "decay", "decayTilt", "window", "phase", "stereoMode", "link", "balance", "oversample",
                     "resolution", "offlineOversample", "offlineResolution", "sidechain", "sidechainListen", "mix", "trim", "delta",
                     "bypass", "lowCut", "lowCutFreq", "lowCutSlope", "highCut", "highCutFreq", "highCutSlope",
                     "b1On", "b1Type", "b1Freq", "b1Sens", "b1Q", "b1Balance", "b4Balance" })
        check (ids.contains (id), juce::String ("parametre absent : ") + id);

    // Fréquences : écriture façon 6k3
    auto* f = asPhlick (*p).apvts.getParameter ("b1Freq");
    check (std::abs (f->convertFrom0to1 (f->getValueForText ("6k3")) - 6300.0f) < 5.0f, "6k3 = 6300 Hz");
    check (std::abs (f->convertFrom0to1 (f->getValueForText ("6,3k")) - 6300.0f) < 5.0f, "6,3k = 6300 Hz");
    check (f->getText (f->convertTo0to1 (6300.0f), 10) == "6k3", "6300 Hz affiche 6k3 : " + f->getText (f->convertTo0to1 (6300.0f), 10));

    check (p->getLatencySamples() == 2048, "latence 2048 a 48 kHz");
    auto q = makePlugin (96000.0);
    check (q->getLatencySamples() == 4096, "latence 4096 a 96 kHz");
}

static void testProcessing()
{
    std::cout << "- traitement : transparence, reduction, mix, delta, bypass, trim" << std::endl;
    const double fs = 48000.0;
    const auto in = hits (96000, fs, 0.6f);
    const int lat = 2048;

    auto delayedIn = [&] (int ch, int i) { return i >= lat ? in.getSample (ch, i - lat) : 0.0f; };

    // Depth 0 : transparent (retard près)
    {
        auto p = makePlugin();
        setParam (*p, Ids::depthCutId, 0.0f);
        const auto out = run (*p, in, 512);
        double err = 0.0;
        for (int i = 6000; i < in.getNumSamples(); ++i)
            for (int c = 0; c < 2; ++c)
                err = juce::jmax (err, (double) std::abs (out.getSample (c, i) - delayedIn (c, i)));
        check (err < 5.0e-4, "depth 0 transparent : " + juce::String (err));
    }

    // Défaut (CUT) : l'attaque baisse, le fond reste ; BOOST : l'attaque monte
    {
        juce::AudioBuffer<float> d (2, in.getNumSamples());
        for (int i = 0; i < in.getNumSamples(); ++i) d.setSample (0, i, delayedIn (0, i));
        const double refAttack = hitWindowDb (d, 0, fs, 0.0, 8.0, lat), refBg = hitWindowDb (d, 0, fs, 120.0, 240.0, lat);

        auto p = makePlugin();
        const auto out = run (*p, in, 256);
        const double att = hitWindowDb (out, 0, fs, 0.0, 8.0, lat) - refAttack, bg = hitWindowDb (out, 0, fs, 120.0, 240.0, lat) - refBg;
        std::cout << "    cut : attaque " << att << " dB, fond " << bg << " dB" << std::endl;
        check (att < -2.5, "CUT reduit l'attaque");
        check (std::abs (bg) < 0.6, "CUT laisse le fond");
        std::cout << "    reduction affichee " << asPhlick (*p).getPeakReduction() << " dB" << std::endl;
        check (asPhlick (*p).getPeakReduction() > 0.5f, "traitement affiche");

        auto q = makePlugin();
        setParam (*q, Ids::modeId, 1.0f);
        const auto up = run (*q, in, 256);
        const double attB = hitWindowDb (up, 0, fs, 0.0, 8.0, lat) - refAttack;
        std::cout << "    boost : attaque " << attB << " dB" << std::endl;
        check (attB > 1.5, "BOOST monte l'attaque");

        // Le DEPTH du mode non choisi ne compte pas
        auto r = makePlugin();
        setParam (*r, Ids::depthBoostId, 0.0f);
        const auto cutOnly = run (*r, in, 256);
        check (std::abs ((hitWindowDb (cutOnly, 0, fs, 0.0, 8.0, lat) - refAttack) - att) < 0.01, "depth boost sans effet en CUT");
    }

    // Mix 0 : sec
    {
        auto p = makePlugin();
        setParam (*p, Ids::mixId, 0.0f);
        const auto out = run (*p, in, 512);
        double err = 0.0;
        for (int i = 24000; i < in.getNumSamples(); ++i)
            err = juce::jmax (err, (double) std::abs (out.getSample (0, i) - delayedIn (0, i)));
        check (err < 1.0e-5, "mix 0 = signal sec retarde");
    }

    // Delta : sec - mouillé ; delta + sortie = sec
    {
        auto p = makePlugin();
        const auto wet = run (*p, in, 512);
        auto q = makePlugin();
        setParam (*q, Ids::deltaId, 1.0f);
        const auto delta = run (*q, in, 512);
        double err = 0.0;
        for (int i = 6000; i < in.getNumSamples(); ++i)
            err = juce::jmax (err, (double) std::abs (wet.getSample (0, i) + delta.getSample (0, i) - delayedIn (0, i)));
        check (err < 1.0e-4, "delta + sortie = sec : " + juce::String (err));
    }

    // Bypass doux : sec retardé
    {
        auto p = makePlugin();
        setParam (*p, Ids::bypassId, 1.0f);
        p->prepareToPlay (fs, 512);
        const auto out = run (*p, in, 512);
        double err = 0.0;
        for (int i = 6000; i < in.getNumSamples(); ++i)
            err = juce::jmax (err, (double) std::abs (out.getSample (0, i) - delayedIn (0, i)));
        check (err < 1.0e-5, "bypass = sec retarde");
    }

    // Bypass de l'hôte : même retard
    {
        auto p = makePlugin();
        juce::AudioBuffer<float> io (2, 512);
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> out (2, 8192);
        for (int pos = 0; pos < 8192; pos += 512)
        {
            for (int c = 0; c < 2; ++c) io.copyFrom (c, 0, in, c, pos, 512);
            p->processBlockBypassed (io, midi);
            for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, io, c, 0, 512);
        }
        double err = 0.0;
        for (int i = 3000; i < 8192; ++i)
            err = juce::jmax (err, (double) std::abs (out.getSample (0, i) - delayedIn (0, i)));
        check (err < 1.0e-6, "bypass de l'hote : retard garde");
    }

    // Trim : seulement le mouillé (à depth 0, mouillé = sec, donc -6 dB = moitié)
    {
        auto p = makePlugin();
        setParam (*p, Ids::depthCutId, 0.0f);
        setParam (*p, Ids::trimId, -6.0206f);
        const auto out = run (*p, in, 512);
        double err = 0.0;
        for (int i = 8000; i < in.getNumSamples(); ++i)
            err = juce::jmax (err, (double) std::abs (out.getSample (0, i) - 0.5f * delayedIn (0, i)));
        check (err < 1.0e-3, "trim -6 dB sur le mouille");
    }

    // Mono
    {
        std::unique_ptr<juce::AudioProcessor> p (createPluginFilter());
        auto layout = p->getBusesLayout();
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
        layout.outputBuses.getReference (0) = juce::AudioChannelSet::mono();
        check (p->setBusesLayout (layout), "mono accepte");
        p->prepareToPlay (fs, 512);
        juce::AudioBuffer<float> io (1, 512);
        juce::MidiBuffer midi;
        bool finite = true;
        for (int pos = 0; pos + 512 <= in.getNumSamples(); pos += 512)
        {
            io.copyFrom (0, 0, in, 0, pos, 512);
            p->processBlock (io, midi);
            for (int i = 0; i < 512; ++i) finite = finite && std::isfinite (io.getSample (0, i));
        }
        check (finite, "mono : sortie finie");
    }

    // Qualité hors ligne
    {
        auto p = makePlugin();
        setParam (*p, Ids::offlineOversampleId, 3.0f);   // 4X
        setParam (*p, Ids::offlineResolutionId, 4.0f);   // ULTRA
        p->setNonRealtime (true);
        run (*p, in, 512);
        check (asPhlick (*p).getEngine().getFftSize() == 8192, "hors ligne : oversample 4X");
        check (asPhlick (*p).getEngine().getHop() == 64, "hors ligne : resolution ULTRA");
        p->setNonRealtime (false);
        run (*p, in, 512);
        check (asPhlick (*p).getEngine().getFftSize() == 2048, "temps reel : oversample 1X");
    }
}

static void testSidechain()
{
    std::cout << "- sidechain" << std::endl;
    const double fs = 48000.0;
    auto p = makePlugin (fs, 512, true);
    check (p->getTotalNumInputChannels() == 4, "entree sidechain stereo");

    const auto main = hits (96000, fs, 0.0f, 3);   // fond tenu
    const auto key = hits (96000, fs, 0.6f, 4);     // coups

    setParam (*p, Ids::depthCutId, 10.0f);
    const auto without = run (*p, main, 512, &key);
    check (asPhlick (*p).isSidechainConnected(), "sidechain vu comme branche");

    auto q = makePlugin (fs, 512, true);
    setParam (*q, Ids::depthCutId, 10.0f);
    setParam (*q, Ids::sidechainId, 1.0f);
    const auto with = run (*q, main, 512, &key);

    const double a = hitWindowDb (without, 0, fs, 0.0, 8.0, 2048), b = hitWindowDb (with, 0, fs, 0.0, 8.0, 2048);
    std::cout << "    fond pendant les coups de la cle : cle coupee " << a << " dB, cle active " << b << " dB" << std::endl;
    check (b < a - 2.0, "la cle commande le traitement");

    setParam (*q, Ids::sidechainListenId, 1.0f);
    const auto heard = run (*q, main, 512, &key);
    double err = 0.0;
    for (int i = 6000; i < 96000; ++i)
        err = juce::jmax (err, (double) std::abs (heard.getSample (0, i) - key.getSample (0, i - 2048)));
    check (err < 1.0e-5, "ecoute du sidechain");
}

static void testHistory()
{
    std::cout << "- annuler / retablir, A / B, etat" << std::endl;
    auto p = makePlugin();
    auto& ph = asPhlick (*p);
    auto* depth = ph.apvts.getParameter (Ids::depthCutId);

    auto turn = [] (juce::RangedAudioParameter* prm, float real)
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 (real));
        prm->endChangeGesture();
        juce::Thread::sleep (200);
    };

    check (! ph.canUndo(), "historique vide au depart");
    turn (depth, 10.0f);
    turn (depth, 12.0f);
    check (ph.canUndo(), "historique apres un reglage");
    ph.undo();
    check (std::abs (getParam (*p, Ids::depthCutId) - 10.0f) < 0.02f, "annuler : depth 10");
    ph.undo();
    check (std::abs (getParam (*p, Ids::depthCutId) - 8.0f) < 0.02f, "annuler : depth 8 (defaut)");
    ph.redo();
    check (std::abs (getParam (*p, Ids::depthCutId) - 10.0f) < 0.02f, "retablir : depth 10");

    // A / B
    ph.switchAb();                                    // B = copie de A
    check (ph.getAbSlot() == 1, "slot B");
    turn (depth, 2.0f);
    ph.switchAb();
    check (std::abs (getParam (*p, Ids::depthCutId) - 10.0f) < 0.02f, "retour en A");
    ph.switchAb();
    check (std::abs (getParam (*p, Ids::depthCutId) - 2.0f) < 0.02f, "B garde son reglage");

    // L'état garde A et B
    juce::MemoryBlock state;
    p->getStateInformation (state);
    auto q = makePlugin();
    q->setStateInformation (state.getData(), (int) state.getSize());
    check (asPhlick (*q).getAbSlot() == 1, "etat : slot B");
    check (std::abs (getParam (*q, Ids::depthCutId) - 2.0f) < 0.02f, "etat : depth");
    asPhlick (*q).switchAb();
    check (std::abs (getParam (*q, Ids::depthCutId) - 10.0f) < 0.02f, "etat : A recharge");

    // Le delta et le bypass ne sont pas dans l'historique
    turn (ph.apvts.getParameter (Ids::deltaId), 1.0f);
    ph.undo();
    check (getParam (*p, Ids::deltaId) > 0.5f, "delta hors historique");

    // États abîmés
    q->setStateInformation ("nimporte", 8);
    check (std::abs (getParam (*q, Ids::depthCutId) - 10.0f) < 0.02f, "etat invalide ignore");

    // Points de départ d'usine
    check (Ids::getNumFactoryPresets() == 9, "9 points de depart");
    ph.presets.loadFactory (1);
    check (ph.presets.getDisplayName() == "MOUTH CLICKS", "MOUTH CLICKS charge");
    check (std::abs (getParam (*p, Ids::lowCutFreqId) - 1500.0f) < 1.0f, "MOUTH CLICKS : low cut");
    check (getParam (*p, Ids::windowId) < 0.5f, "MOUTH CLICKS : fenetre SHORT");
    check (! ph.presets.isModified(), "MOUTH CLICKS non modifie");
    check (getParam (*p, Ids::deltaId) > 0.5f, "un preset ne touche pas au delta");
}

static void testEditor (const juce::File& outputDir)
{
    std::cout << "- interface" << std::endl;
    auto p = makePlugin();
    auto& ph = asPhlick (*p);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditorAndMakeActive());
    auto* editor = dynamic_cast<PhlickAudioProcessorEditor*> (ed.get());
    check (editor != nullptr, "editeur");
    if (editor == nullptr)
        return;

    auto& facade = editor->getFacade();
    check (editor->getWidth() == PhlickFacade::width && editor->getHeight() == PhlickFacade::height, "taille");

    // Tout est dans la façade et rien ne se chevauche (hors panneau SAVE et écran)
    juce::Array<juce::Component*> visible;
    for (auto* c : facade.getChildren())
        if (c->isVisible())
        {
            check (facade.getLocalBounds().contains (c->getBounds()), "hors de la facade : " + c->getComponentID());
            visible.add (c);
        }
    for (int i = 0; i < visible.size(); ++i)
        for (int j = i + 1; j < visible.size(); ++j)
        {
            auto* a = visible[i]; auto* b = visible[j];
            if (a->getComponentID() == "graph" || b->getComponentID() == "graph")
            {
                auto* other = a->getComponentID() == "graph" ? b : a;
                if (other->getComponentID() == "range")
                    continue;
            }
            check (! a->getBounds().intersects (b->getBounds()), "chevauchement : " + a->getComponentID() + " / " + b->getComponentID());
        }

    // Bande choisie : les contrôles suivent
    facade.selectBand (0);
    auto& graph = facade.getGraph();
    const auto node = graph.nodePosition (0);
    check (std::abs (node.x - graph.xForFreq (200.0f)) < 1.0f, "noeud 1 a 200 Hz");

    // Glisser le nœud 1 vers la droite d'une octave
    {
        const auto start = graph.nodePosition (0);
        const float target = graph.xForFreq (400.0f);
        juce::MouseInputSource source = juce::Desktop::getInstance().getMainMouseSource();
        auto make = [&] (juce::Point<float> pos, juce::ModifierKeys mods)
        {
            return juce::MouseEvent (source, pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &graph, &graph, juce::Time::getCurrentTime(),
                                     start, juce::Time::getCurrentTime(), 1, false);
        };
        graph.mouseDown (make (start, juce::ModifierKeys::leftButtonModifier));
        graph.mouseDrag (make ({ target, start.y - 20.0f }, juce::ModifierKeys::leftButtonModifier));
        graph.mouseUp (make ({ target, start.y - 20.0f }, {}));
        check (std::abs (getParam (*p, Ids::bandId (0, "Freq")) - 400.0f) < 8.0f, "glisser : 400 Hz, " + juce::String (getParam (*p, Ids::bandId (0, "Freq"))));
        check (getParam (*p, Ids::bandId (0, "Sens")) > 1.0f, "glisser vers le haut : plus de sensibilite");

        // Alt : écoute pendant le glisser
        graph.mouseDown (make (graph.nodePosition (0), juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier)));
        check (ph.getBandListen() == 0, "alt : ecoute de la bande");
        graph.mouseUp (make (graph.nodePosition (0), {}));
        check (ph.getBandListen() == -1, "fin de l'ecoute");

        // Double-clic : éteint la bande
        graph.mouseDoubleClick (make (graph.nodePosition (0), juce::ModifierKeys::leftButtonModifier));
        check (getParam (*p, Ids::bandId (0, "On")) < 0.5f, "double-clic eteint la bande");
    }

    // Les coupures montrent la pente, pas le type
    facade.selectBand (4);
    check (facade.findControl ("lowCutSlope_0") != nullptr, "pentes de la coupure basse");
    check (facade.findControl ("b1Type_0") == nullptr, "types caches pour une coupure");
    facade.selectBand (2);
    check (facade.findControl ("b3Type_0") != nullptr, "types de la bande 3");

    // CUT / BOOST : le gros bouton DEPTH suit le mode
    if (auto* boostButton = dynamic_cast<juce::Button*> (facade.findControl ("mode_1")))
    {
        auto* depthKnob = dynamic_cast<juce::Slider*> (facade.findControl ("depth"));
        check (depthKnob != nullptr, "bouton DEPTH");
        if (depthKnob != nullptr)
        {
            depthKnob->setValue (7.0, juce::sendNotificationSync);
            check (std::abs (getParam (*p, Ids::depthCutId) - 7.0f) < 0.02f, "DEPTH en CUT");
        }
        boostButton->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        check (getParam (*p, Ids::modeId) > 0.5f, "bouton BOOST");
        if (depthKnob != nullptr)
        {
            check (std::abs (depthKnob->getValue() - 8.0) < 0.02, "DEPTH affiche la valeur BOOST");
            depthKnob->setValue (3.0, juce::sendNotificationSync);
            check (std::abs (getParam (*p, Ids::depthBoostId) - 3.0f) < 0.02f, "DEPTH en BOOST");
            check (std::abs (getParam (*p, Ids::depthCutId) - 7.0f) < 0.02f, "DEPTH CUT garde sa valeur");
        }
    }
    else
        check (false, "bouton BOOST absent");

    // Écoute de la bande par le bouton
    if (auto* listen = dynamic_cast<juce::Button*> (facade.findControl ("bandListen")))
    {
        listen->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (ph.getBandListen() == 2, "LISTEN de la bande 3");
        listen->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (ph.getBandListen() == -1, "LISTEN coupe");
    }

    // Captures
    const auto in = hits (48000, 48000.0, 0.6f);
    setParam (*p, Ids::modeId, 1.0f);
    run (*p, in, 512);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    const bool wantBoostShot = true;

    auto snapshot = [&] (const juce::String& name)
    {
        if (outputDir == juce::File())
            return;
        outputDir.createDirectory();
        const auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat png;
        juce::FileOutputStream os (outputDir.getChildFile (name + ".png"));
        if (os.openedOk())
        {
            os.setPosition (0);
            os.truncate();
            png.writeImageToStream (img, os);
        }
    };
    if (wantBoostShot)
        snapshot ("phlick-boost");
    setParam (*p, Ids::modeId, 0.0f);
    run (*p, in, 512);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    snapshot ("phlick");
    facade.selectBand (4);
    snapshot ("phlick-lowcut");

    editor->setUiScale (0.75f);
    snapshot ("phlick-75");

    p->editorBeingDeleted (editor);
    ed.reset();
}

static void testFuzz()
{
    std::cout << "- reglages au hasard, blocs de taille variable" << std::endl;
    std::mt19937 rng (5);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    for (double fs : { 44100.0, 96000.0 })
    {
        auto p = makePlugin (fs, 1024, true);
        juce::AudioBuffer<float> io (4, 1024);
        juce::MidiBuffer midi;
        bool finite = true;
        for (int iter = 0; iter < 300; ++iter)
        {
            for (auto* prm : p->getParameters())
                if (u (rng) < 0.1f)
                    prm->setValueNotifyingHost (u (rng));
            asPhlick (*p).setBandListen ((int) (u (rng) * 8.0f) - 2);
            const int n = 1 + (int) (u (rng) * 1023.0f);
            io.setSize (4, n, false, false, true);
            for (int c = 0; c < 4; ++c)
                for (int i = 0; i < n; ++i)
                    io.setSample (c, i, (u (rng) - 0.5f) * (c < 2 ? 1.0f : 0.3f));
            p->processBlock (io, midi);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    finite = finite && std::isfinite (io.getSample (c, i)) && std::abs (io.getSample (c, i)) < 20.0f;
        }
        check (finite, "sortie finie et bornee a " + juce::String ((int) fs));
    }
}

#include "PresetTests.h"

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto outputDir = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[1])) : juce::File();

    testParameters();
    testProcessing();
    testSidechain();
    testHistory();
    testEditor (outputDir);
    testFuzz();
    testPresetsAndWindowSize<PhlickAudioProcessor, PhlickAudioProcessorEditor, PhlickFacade> (Ids::transientParameters(), outputDir,
                                                                                             Ids::getNumFactoryPresets());

    std::cout << (failures == 0 ? "TOUS LES TESTS PASSENT" : juce::String (failures) + " ECHEC(S)") << std::endl;
    return failures == 0 ? 0 : 1;
}
