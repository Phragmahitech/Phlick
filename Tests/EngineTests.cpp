// Tests du moteur de Phlick (sans JUCE).
#include "TransientEngine.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace phlick;

static int failures = 0, checks = 0;
static constexpr double pi = 3.14159265358979323846;
static constexpr double fs = 48000.0;

static void check (bool ok, const std::string& what)
{
    ++checks;
    if (! ok)
    {
        ++failures;
        std::printf ("  ECHEC : %s\n", what.c_str());
    }
}

static double db (double a) { return 20.0 * std::log10 (std::max (a, 1.0e-12)); }

struct Stereo { std::vector<float> l, r; };

static Stereo runEngine (TransientEngine& e, const Stereo& in, const std::vector<float>* side = nullptr)
{
    Stereo out { std::vector<float> (in.l.size()), std::vector<float> (in.l.size()) };
    size_t pos = 0;
    int b = 333;
    while (pos < in.l.size())
    {
        const int n = (int) std::min<size_t> ((size_t) b, in.l.size() - pos);
        const float* src[2] = { in.l.data() + pos, in.r.data() + pos };
        float* dst[2] = { out.l.data() + pos, out.r.data() + pos };
        const float* sd[1] = { side != nullptr ? side->data() + pos : nullptr };
        e.process (src, side != nullptr ? sd : nullptr, side != nullptr ? 1 : 0, dst, n);
        pos += (size_t) n;
        b = b == 333 ? 64 : b == 64 ? 1 : b == 1 ? 1024 : 333;
    }
    return out;
}

static std::vector<float> delayed (const std::vector<float>& x, int d)
{
    std::vector<float> y (x.size(), 0.0f);
    for (size_t i = (size_t) d; i < x.size(); ++i)
        y[i] = x[i - (size_t) d];
    return y;
}

/** Énergie moyenne (dB) sur [onset + a, onset + b] ms, sommée sur tous les coups à partir du 3e. */
static double windowDb (const std::vector<float>& x, const std::vector<size_t>& onsets, double aMs, double bMs, int shift)
{
    double e = 0.0;
    size_t n = 0;
    for (size_t h = 2; h < onsets.size(); ++h)
    {
        const long from = (long) onsets[h] + shift + (long) (aMs * fs / 1000.0);
        const long to = (long) onsets[h] + shift + (long) (bMs * fs / 1000.0);
        for (long i = std::max (0L, from); i < to && i < (long) x.size(); ++i)
        {
            e += (double) x[(size_t) i] * x[(size_t) i];
            ++n;
        }
    }
    return 10.0 * std::log10 (e / (double) std::max<size_t> (1, n) + 1.0e-24);
}

/** Amplitude d'une sinusoïde sur des fenêtres après chaque coup (moyenne). */
static double toneDb (const std::vector<float>& x, const std::vector<size_t>& onsets, double aMs, double bMs, int shift, double hz)
{
    double sum = 0.0;
    int count = 0;
    for (size_t h = 2; h < onsets.size(); ++h)
    {
        const size_t from = onsets[h] + (size_t) shift + (size_t) (aMs * fs / 1000.0);
        const size_t len = (size_t) ((bMs - aMs) * fs / 1000.0);
        if (from + len > x.size())
            break;
        double c = 0.0, s = 0.0;
        for (size_t i = 0; i < len; ++i)
        {
            const double ph = 2.0 * pi * hz * (double) (from + i) / fs;
            c += x[from + i] * std::cos (ph);
            s += x[from + i] * std::sin (ph);
        }
        sum += 2.0 * std::sqrt (c * c + s * s) / (double) len;
        ++count;
    }
    return db (sum / std::max (1, count));
}

/**
    Fond continu (bruit filtré + sinus 300 Hz) et coups toutes les 250 ms (bruit qui décroît en ~8 ms,
    ou salve de sinus à hitHz si hitHz > 0).
*/
static Stereo hits (double seconds, float hitAmp, float gain, std::vector<size_t>& onsets, float hitHz = 0.0f, float bgAmp = 0.05f)
{
    const size_t n = (size_t) (seconds * fs);
    Stereo s { std::vector<float> (n), std::vector<float> (n) };
    std::mt19937 rng (7);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    float lp = 0.0f;
    onsets.clear();
    for (size_t i = 0; i < n; ++i)
    {
        lp = 0.85f * lp + 0.15f * nd (rng);
        const float bg = bgAmp * (lp * 2.0f + 0.5f * (float) std::sin (2.0 * pi * 300.0 * (double) i / fs));
        const size_t period = (size_t) (0.25 * fs);
        const size_t t = i % period;
        if (t == 0)
            onsets.push_back (i);
        const float env = std::exp (-(float) t / (float) (0.008 * fs));
        const float src = hitHz > 0.0f ? (float) std::sin (2.0 * pi * hitHz * (double) i / fs) : nd (rng);
        const float x = (bg + hitAmp * env * src) * gain;
        s.l[i] = x;
        s.r[i] = x;
    }
    return s;
}

static TransientEngine& prepared (TransientEngine& e, const EngineSettings& s, int channels = 2)
{
    e.prepare (fs, channels);
    e.setSettings (s);
    phocus::CurveSpec flat;
    for (auto& b : flat.bands) b.sens = 0.0f;
    e.setCurve (flat);
    return e;
}

//==============================================================================
static void testTransparency()
{
    std::printf ("- transparence (depth 0), latence\n");
    std::mt19937 rng (3);
    std::uniform_real_distribution<float> u (-0.8f, 0.8f);
    Stereo in { std::vector<float> (40000), std::vector<float> (40000) };
    for (size_t i = 0; i < in.l.size(); ++i) { in.l[i] = u (rng); in.r[i] = 0.5f * u (rng) + 0.3f * in.l[i]; }

    for (int win = 0; win < 3; ++win)
        for (bool minPhase : { false, true })
            for (int os : { 1, 2, 4 })
                for (int res : { 0, 3 })
                {
                    TransientEngine e;
                    EngineSettings s;
                    s.depth = 0.0f; s.window = win; s.minimumPhase = minPhase; s.oversample = os; s.resolution = res; s.midSide = (win == 1);
                    prepared (e, s);
                    const auto out = runEngine (e, in);
                    check (e.getLatency() == 2048, "latence 2048 a 48 kHz");
                    double err = 0.0;
                    for (size_t i = 6000; i < in.l.size(); ++i)
                        err = std::max (err, (double) std::abs (out.l[i] - in.l[i - 2048]) + std::abs (out.r[i] - in.r[i - 2048]));
                    check (err < 3.0e-4, "transparence fenetre " + std::to_string (win) + (minPhase ? " min" : " lin") + " os " + std::to_string (os)
                                            + " res " + std::to_string (res) + " : " + std::to_string (err));
                }

    for (auto [rate, lat] : { std::pair<double, int> { 44100.0, 2048 }, { 96000.0, 4096 }, { 192000.0, 8192 } })
    {
        TransientEngine e;
        e.prepare (rate, 1);
        check (e.getLatency() == lat, "latence a " + std::to_string ((int) rate));
    }
}

static void testCutBoost()
{
    std::printf ("- cut / boost sur des coups\n");
    std::vector<size_t> onsets;
    const auto in = hits (3.0, 0.6f, 1.0f, onsets);
    const auto ref = delayed (in.l, 2048);

    auto measure = [&] (const EngineSettings& s, const Stereo& sig, double& attack, double& sustain)
    {
        TransientEngine e;
        prepared (e, s);
        const auto out = runEngine (e, sig);
        const auto r = delayed (sig.l, 2048);
        attack = windowDb (out.l, onsets, 0.0, 8.0, 2048) - windowDb (r, onsets, 0.0, 8.0, 2048);
        sustain = windowDb (out.l, onsets, 120.0, 240.0, 2048) - windowDb (r, onsets, 120.0, 240.0, 2048);
    };

    double att, sus;
    EngineSettings s;
    measure (s, in, att, sus);
    std::printf ("    cut defaut : attaque %+.1f dB, fond %+.2f dB\n", att, sus);
    check (att < -2.5, "cut : l'attaque baisse");
    check (std::abs (sus) < 0.6, "cut : le fond ne bouge presque pas (bruit de fond)");

    double attDeep;
    s.depth = 15.0f;
    measure (s, in, attDeep, sus);
    std::printf ("    cut depth 15 : attaque %+.1f dB\n", attDeep);
    check (attDeep < att - 3.0, "plus de depth = plus de coupe");

    EngineSettings b;
    b.boost = true;
    measure (b, in, att, sus);
    std::printf ("    boost defaut : attaque %+.1f dB, fond %+.2f dB\n", att, sus);
    check (att > 1.5, "boost : l'attaque monte");
    check (std::abs (sus) < 0.6, "boost : le fond ne bouge presque pas (bruit de fond)");

    // Indépendant du niveau
    std::vector<size_t> o2;
    const auto quiet = hits (3.0, 0.6f, 0.03f, o2);
    double attQ, attL;
    EngineSettings c;
    measure (c, in, attL, sus);
    measure (c, quiet, attQ, sus);
    std::printf ("    cut a -30 dB : attaque %+.1f dB (%+.1f a 0 dB)\n", attQ, attL);
    check (std::abs (attQ - attL) < 1.0, "reduction independante du niveau");

    // Un son tenu n'est pas touché
    {
        Stereo tone { std::vector<float> (96000), std::vector<float> (96000) };
        for (size_t i = 0; i < tone.l.size(); ++i)
            tone.l[i] = tone.r[i] = 0.5f * (float) std::sin (2.0 * pi * 1000.0 * (double) i / fs);
        TransientEngine e;
        EngineSettings t;
        t.depth = 20.0f; t.sensitivity = 10.0f;
        prepared (e, t);
        const auto out = runEngine (e, tone);
        double err = 0.0;
        for (size_t i = 20000; i < tone.l.size(); ++i)
            err = std::max (err, (double) std::abs (out.l[i] - tone.l[i - 2048]));
        std::printf ("    sinus tenu, depth 20 : ecart max %.5f\n", err);
        check (err < 0.01, "un son tenu n'est pas traite");
    }
}

static void testSensitivityDecaySharpness()
{
    std::printf ("- sensitivity, decay, sharpness, decay LF/HF\n");
    std::vector<size_t> onsets;
    const auto soft = hits (3.0, 0.12f, 1.0f, onsets);   // petits coups
    const auto ref = delayed (soft.l, 2048);

    auto attack = [&] (const EngineSettings& s)
    {
        TransientEngine e;
        prepared (e, s);
        const auto out = runEngine (e, soft);
        return windowDb (out.l, onsets, 0.0, 8.0, 2048) - windowDb (ref, onsets, 0.0, 8.0, 2048);
    };
    EngineSettings lo; lo.sensitivity = 1.0f;
    EngineSettings hi; hi.sensitivity = 9.0f;
    const double aLo = attack (lo), aHi = attack (hi);
    std::printf ("    petits coups : sensitivity 1 %+.1f dB, 9 %+.1f dB\n", aLo, aHi);
    check (aHi < aLo - 1.5, "sensitivity haute : reagit aux petits coups");

    // Decay : la queue est plus touchée
    std::vector<size_t> o2;
    const auto in = hits (3.0, 0.6f, 1.0f, o2);
    const auto r2 = delayed (in.l, 2048);
    auto tail = [&] (float decay)
    {
        TransientEngine e;
        EngineSettings s;
        s.decay = decay;
        prepared (e, s);
        const auto out = runEngine (e, in);
        return windowDb (out.l, o2, 12.0, 40.0, 2048) - windowDb (r2, o2, 12.0, 40.0, 2048);
    };
    const double t1 = tail (0.5f), t8 = tail (8.0f);
    std::printf ("    queue 12-40 ms : decay 0,5 %+.1f dB, decay 8 %+.1f dB\n", t1, t8);
    check (t8 < t1 - 1.0, "decay long : la queue est traitee");

    // Sharpness : coup à 3 kHz, sinus de fond à 300 Hz
    std::vector<size_t> o3;
    const auto tonal = hits (3.0, 0.6f, 1.0f, o3, 3000.0f, 0.15f);
    const auto r3 = delayed (tonal.l, 2048);
    auto bgDrop = [&] (float sharp)
    {
        TransientEngine e;
        EngineSettings s;
        s.sharpness = sharp; s.depth = 20.0f;   // v1.1 : courbe de depth plus douce
        prepared (e, s);
        const auto out = runEngine (e, tonal);
        return toneDb (out.l, o3, 0.0, 30.0, 2048, 300.0) - toneDb (r3, o3, 0.0, 30.0, 2048, 300.0);
    };
    const double wide = bgDrop (0.0f), narrow = bgDrop (10.0f);
    std::printf ("    300 Hz pendant un coup a 3 kHz : sharpness 0 %+.1f dB, 10 %+.1f dB\n", wide, narrow);
    check (narrow > wide + 1.0, "sharpness haute : la coupe reste etroite");

    // Decay LF/HF
    check (TransientEngine::decaySeconds (5.0f, 0.0f, 100.0f) > 2.0f * TransientEngine::decaySeconds (5.0f, 0.0f, 10000.0f), "LF/HF bas : graves plus longs");
    check (TransientEngine::decaySeconds (5.0f, 10.0f, 10000.0f) > 2.0f * TransientEngine::decaySeconds (5.0f, 10.0f, 100.0f), "LF/HF haut : aigus plus longs");
    check (std::abs (TransientEngine::decaySeconds (5.0f, 5.0f, 100.0f) - TransientEngine::decaySeconds (5.0f, 5.0f, 10000.0f)) < 1.0e-6f, "LF/HF centre : egal");
}

static void testPhase()
{
    std::printf ("- phase minimale : pas de pre-echo\n");
    // Clé : salves à 2 kHz (bosse étroite). Son traité : clics larges bande aux mêmes instants, silence autour.
    // Un filtre étroit sur un clic sonne : symétrique en phase linéaire, après le clic en phase minimale.
    const size_t n = (size_t) (3.0 * fs);
    Stereo in { std::vector<float> (n), std::vector<float> (n) };
    std::vector<float> key (n);
    std::vector<size_t> onsets;
    for (size_t i = 0; i < n; ++i)
    {
        const size_t t = i % 12000;
        if (t == 0) onsets.push_back (i);
        in.l[i] = in.r[i] = t < 3 ? 0.5f : 0.0f;
        key[i] = t < 2400 ? 0.8f * std::exp (-(float) t / 600.0f) * (float) std::sin (2.0 * pi * 2000.0 * (double) t / fs) : 0.0f;
    }
    auto pre = [&] (bool minPhase)
    {
        TransientEngine e;
        EngineSettings s;
        s.window = 2; s.depth = 14.0f; s.minimumPhase = minPhase; s.boost = true; s.sharpness = 10.0f;
        prepared (e, s);
        const auto out = runEngine (e, in, &key);
        return windowDb (out.l, onsets, -20.0, -0.3, 2048) - windowDb (out.l, onsets, 0.0, 4.0, 2048);
    };
    const double lin = pre (false), mn = pre (true);
    std::printf ("    energie avant le clic / apres (boost etroit, fenetre LONG) : lineaire %.1f dB, minimale %.1f dB\n", lin, mn);
    check (lin > -100.0, "phase lineaire : pre-echo mesurable (le test voit quelque chose)");
    check (mn < lin - 15.0, "phase minimale : beaucoup moins de pre-echo");
}

static void testStereoSidechainListen()
{
    std::printf ("- stereo, sidechain, ecoute\n");
    std::vector<size_t> onsets;
    auto in = hits (3.0, 0.6f, 1.0f, onsets);
    // Coups seulement à gauche
    std::vector<size_t> o2;
    const auto steady = hits (3.0, 0.0f, 1.0f, o2);
    in.r = steady.r;

    auto rightChange = [&] (float link)
    {
        TransientEngine e;
        EngineSettings s;
        s.depth = 10.0f; s.link = link;
        prepared (e, s);
        const auto out = runEngine (e, in);
        const auto r = delayed (in.r, 2048);
        return windowDb (out.r, onsets, 0.0, 8.0, 2048) - windowDb (r, onsets, 0.0, 8.0, 2048);
    };
    const double unlinked = rightChange (0.0f), linked = rightChange (1.0f);
    std::printf ("    canal droit sans coups : link 0 %+.2f dB, link 100 %+.1f dB\n", unlinked, linked);
    check (std::abs (unlinked) < 1.0, "link 0 : le canal droit presque pas touche (bruit de fond seulement)");
    check (linked < unlinked - 1.5, "link 100 : le canal droit suit");

    // Sidechain : fond tenu, coups dans la clé
    {
        TransientEngine e;
        EngineSettings s;
        s.depth = 10.0f;
        prepared (e, s);
        const auto out = runEngine (e, steady, &in.l);
        const auto r = delayed (steady.l, 2048);
        const double d = windowDb (out.l, onsets, 0.0, 8.0, 2048) - windowDb (r, onsets, 0.0, 8.0, 2048);
        std::printf ("    fond tenu, coups dans la cle : %+.1f dB pendant les coups\n", d);
        check (d < -2.0, "la cle sidechain commande le traitement");
    }

    // Écoute à depth 0 : silence
    {
        TransientEngine e;
        EngineSettings s;
        s.depth = 0.0f;
        prepared (e, s);
        e.setListen (2);
        const auto out = runEngine (e, in);
        double peak = 0.0;
        for (size_t i = 5000; i < out.l.size(); ++i) peak = std::max (peak, (double) std::abs (out.l[i]));
        check (peak < 1.0e-4, "ecoute a depth 0 : silence");
    }

    // Mono
    {
        TransientEngine e;
        EngineSettings s;
        prepared (e, s, 1);
        std::vector<float> out (in.l.size());
        for (size_t pos = 0; pos < in.l.size(); pos += 512)
        {
            const int k = (int) std::min<size_t> (512, in.l.size() - pos);
            const float* src[1] = { in.l.data() + pos };
            float* dst[1] = { out.data() + pos };
            e.process (src, nullptr, 0, dst, k);
        }
        const auto r = delayed (in.l, 2048);
        check (windowDb (out, onsets, 0.0, 8.0, 2048) - windowDb (r, onsets, 0.0, 8.0, 2048) < -2.0, "mono : reduction");
    }
}


/**
    Ligne de basse (dents de scie 55 Hz, notes de 60 ms en doubles croches à 190 BPM, 3 notes sur 4),
    précédée de 2 s de silence. Chaque note doit être traitée comme les autres, la première comprise.
*/
static void testStability()
{
    std::printf ("- stabilite : la premiere note apres un silence\n");
    const double sixteenth = 60.0 / 190.0 / 4.0;
    const size_t silence = (size_t) (2.0 * fs), step = (size_t) (sixteenth * fs);
    const size_t n = silence + 32 * step;
    Stereo in { std::vector<float> (n, 0.0f), std::vector<float> (n, 0.0f) };
    std::vector<size_t> onsets;
    double phase = 0.0;
    for (int note = 0; note < 32; ++note)
    {
        if (note % 4 == 0)
            continue;                                   // place du kick
        const size_t start = silence + (size_t) note * step;
        onsets.push_back (start);
        for (size_t t = 0; t < (size_t) (0.06 * fs) && start + t < n; ++t)
        {
            phase += 55.0 / fs;
            const double saw = 2.0 * (phase - std::floor (phase)) - 1.0;
            const double env = std::min (1.0, (double) t / 48.0) * std::exp (-(double) t / (0.025 * fs));
            in.l[start + t] = in.r[start + t] = (float) (0.5 * saw * env);
        }
    }

    for (bool boost : { true, false })
        for (float sens : { 5.0f, 10.0f })
        {
            TransientEngine e;
            EngineSettings s;
            s.boost = boost; s.depth = 5.0f; s.sensitivity = sens; s.sharpness = 10.0f;
            prepared (e, s);
            const auto out = runEngine (e, in);
            const auto ref = delayed (in.l, 2048);

            std::vector<double> change;
            for (size_t h = 0; h < onsets.size(); ++h)
            {
                const std::vector<size_t> one { 0, 0, onsets[h] };   // windowDb ignore les deux premiers
                change.push_back (windowDb (out.l, one, 0.0, 10.0, 2048) - windowDb (ref, one, 0.0, 10.0, 2048));
            }
            double others = 0.0;
            for (size_t h = 1; h < change.size(); ++h) others += change[h];
            others /= (double) (change.size() - 1);
            std::printf ("    %s sens %.0f : 1re note %+.2f dB, suivantes %+.2f dB\n", boost ? "boost" : "cut", sens, change[0], others);
            check (std::abs (change[0] - others) < 1.0, std::string ("premiere note traitee comme les autres (") + (boost ? "boost" : "cut") + ")");
            double spread = 0.0;
            for (size_t h = 1; h < change.size(); ++h) spread = std::max (spread, std::abs (change[h] - others));
            std::printf ("    ecart max entre notes suivantes : %.2f dB\n", spread);
            check (spread < 1.0, "notes suivantes regulieres");
        }
}

static void testFuzz()
{
    std::printf ("- reglages au hasard, 44,1 a 192 kHz\n");
    std::mt19937 rng (99);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    bool finite = true;
    float peak = 0.0f;
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        TransientEngine e;
        e.prepare (rate, 2);
        const size_t n = (size_t) (rate * 1.2);
        std::vector<float> l (n), r (n), side (n);
        std::normal_distribution<float> nd (0.0f, 1.0f);
        for (size_t i = 0; i < n; ++i)
        {
            const size_t t = i % 7000;
            l[i] = (t < 300 ? 2.0f : 0.05f) * nd (rng);
            r[i] = 1.5f * (float) std::sin (2.0 * pi * 60.0 * (double) i / rate);
            side[i] = u (rng) - 0.5f;
        }
        std::vector<float> oL (n), oR (n);
        for (size_t pos = 0; pos < n; )
        {
            EngineSettings s;
            s.boost = u (rng) < 0.5f;
            s.depth = u (rng) * 20.0f;
            s.sensitivity = u (rng) * 10.0f;
            s.sharpness = u (rng) * 10.0f;
            s.decay = u (rng) * 10.0f;
            s.decayTilt = u (rng) * 10.0f;
            s.window = u (rng) < 0.9f ? s.window : (int) (u (rng) * 2.99f);
            s.minimumPhase = u (rng) < 0.5f;
            s.oversample = 1 << (int) (u (rng) * 2.99f);
            s.resolution = (int) (u (rng) * 3.99f);
            s.midSide = u (rng) < 0.5f;
            s.link = u (rng);
            s.balance = u (rng) * 2.0f - 1.0f;
            e.setSettings (s);
            phocus::CurveSpec c;
            c.lowCut = { u (rng) < 0.5f, 10.0f + u (rng) * 990.0f, (int) (u (rng) * 3.99f) };
            c.highCut = { u (rng) < 0.5f, 1000.0f + u (rng) * 21000.0f, (int) (u (rng) * 3.99f) };
            for (auto& b : c.bands)
                b = { u (rng) < 0.8f, (int) (u (rng) * 5.99f), 20.0f + u (rng) * 19980.0f, u (rng) * 48.0f - 24.0f, 0.1f + u (rng) * 9.9f, u (rng) * 2.0f - 1.0f };
            e.setCurve (c);
            e.setListen ((int) (u (rng) * 8.0f) - 2);

            const int k = (int) std::min<size_t> ((size_t) (1 + u (rng) * 3000.0f), n - pos);
            const float* in[2] = { l.data() + pos, r.data() + pos };
            float* out[2] = { oL.data() + pos, oR.data() + pos };
            const float* sd[1] = { side.data() + pos };
            e.process (in, u (rng) < 0.3f ? sd : nullptr, 1, out, k);
            pos += (size_t) k;
        }
        for (size_t i = 0; i < n; ++i)
        {
            finite = finite && std::isfinite (oL[i]) && std::isfinite (oR[i]);
            peak = std::max (peak, std::max (std::abs (oL[i]), std::abs (oR[i])));
        }
        for (int i = 0; i < TransientEngine::displayPoints; ++i)
            finite = finite && std::isfinite (e.getDisplayGain (i)) && std::isfinite (e.getDisplayLevel (i));
    }
    check (finite, "sortie finie");
    std::printf ("    crete de sortie %.2f\n", peak);
    check (peak < 60.0f, "pas d'emballement");
}

int main()
{
    testTransparency();
    testCutBoost();
    testSensitivityDecaySharpness();
    testPhase();
    testStereoSidechainListen();
    testStability();
    testFuzz();
    std::printf ("%d verifications, %d echec(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
