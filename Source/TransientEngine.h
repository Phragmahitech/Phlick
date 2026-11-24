#pragma once

#include "RealFFT.h"
#include "SensitivityCurve.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <vector>

/**
    Moteur de Phlick : processeur adaptatif de transitoires, par analyse spectrale (STFT).

    Principe
      - Trames de N échantillons (WINDOW : SHORT 512, MEDIUM 1024, LONG 2048 jusqu'à 64 kHz ;
        × 2 jusqu'à 128 kHz, × 4 au-delà), racine de Hann à l'analyse et à la synthèse,
        complétées de zéros jusqu'à M = 2 × N × OVERSAMPLE (le filtre de chaque trame a la place de
        s'étaler sans se replier).
      - Pour chaque zone de fréquence, on suit le niveau dans le temps : une montée rapide au-dessus
        du niveau de fond de la même zone est un transitoire. La mesure est un rapport (dB), donc
        indépendante du niveau d'entrée. SENSITIVITY règle la montée minimale.
      - CUT creuse, BOOST remonte, uniquement là où le transitoire a lieu. DEPTH a une valeur par mode.
        SHARPNESS règle la largeur en fréquence de chaque coupe ou bosse (bas = large, comme un
        transient shaper classique ; haut = étroit).
      - L'attaque est immédiate ; DECAY règle le retour (plus haut = le traitement mord sur la queue).
        DECAY LF/HF penche ce retour autour de 1 kHz (bas : graves plus longs, haut : aigus plus longs).
      - PHASE LINEAR : gain réel par case (pré-écho possible dans la trame). MINIMUM : le même gain en
        phase minimale (cepstre), sans pré-écho.
      - Reconstruction par addition des trames normalisée par la somme des fenêtres.
      - Latence fixe : celle de la fenêtre LONG, quelle que soit la fenêtre choisie (les fenêtres plus
        courtes sont retardées d'autant). Changer de fenêtre vide l'état (courte coupure).

    Le moteur rend le signal mouillé seulement (mix, delta, trim : dans le processeur).
    Sans dépendance. Toute la mémoire est réservée par prepare().
*/
namespace phlick
{

using phocus::CurveSpec;
using phocus::CutSpec;
using phocus::BandSpec;

struct EngineSettings
{
    bool boost = false;
    float depth = 5.0f;           // 0 .. 20 (celle du mode choisi)
    float sensitivity = 5.0f;     // 0 .. 10
    float sharpness = 3.0f;       // 0 .. 10
    float decay = 3.0f;           // 0 .. 10
    float decayTilt = 5.0f;       // 0 .. 10, 5 = égal
    int window = 1;               // 0 SHORT, 1 MEDIUM, 2 LONG
    bool minimumPhase = false;
    int oversample = 1;           // 1, 2, 4
    int resolution = 1;           // 0 ECO, 1 NORMAL, 2 HIGH, 3 ULTRA
    bool midSide = false;
    float link = 1.0f;            // 0 .. 1
    float balance = 0.0f;         // -1 (A) .. +1 (B)
};

/** Retard simple. */
class DelayLine
{
public:
    void prepare (int maxDelay)
    {
        buffer.assign ((size_t) maxDelay + 1, 0.0f);
        pos = 0;
    }

    void reset()                            { std::fill (buffer.begin(), buffer.end(), 0.0f); pos = 0; }

    float process (float x, int delay) noexcept
    {
        const int size = (int) buffer.size();
        buffer[(size_t) pos] = x;
        int r = pos - delay;
        if (r < 0)
            r += size;
        const float y = buffer[(size_t) r];
        if (++pos == size)
            pos = 0;
        return y;
    }

private:
    std::vector<float> buffer;
    int pos = 0;
};

class TransientEngine
{
public:
    static constexpr int maxChannels = 2;
    static constexpr int displayPoints = 256;
    static constexpr float maxCutDb = 40.0f, maxBoostDb = 18.0f;
    static constexpr float displayMinHz = 20.0f, displayMaxHz = 20000.0f;

    static int rateFactor (double sampleRate) noexcept
    {
        return sampleRate <= 64000.0 ? 1 : sampleRate <= 128000.0 ? 2 : 4;
    }

    static int frameSizeFor (int window, double sampleRate) noexcept
    {
        return (512 << std::clamp (window, 0, 2)) * rateFactor (sampleRate);
    }

    static int overlap (int resolution) noexcept
    {
        static constexpr int values[] = { 2, 4, 8, 16 };
        return values[std::clamp (resolution, 0, 3)];
    }

    static float displayFrequency (int i) noexcept
    {
        return displayMinHz * std::pow (displayMaxHz / displayMinHz, (float) i / (float) (displayPoints - 1));
    }

    /** Temps de retour (s) à une fréquence. */
    static float decaySeconds (float decay, float tilt, float hz) noexcept
    {
        const float base = 0.003f * std::pow (2.0f, 0.75f * std::clamp (decay, 0.0f, 10.0f));   // 3 ms .. 545 ms
        const float t = (std::clamp (tilt, 0.0f, 10.0f) - 5.0f) / 5.0f;
        const float f = std::clamp (hz, 20.0f, 20000.0f) / 1000.0f;
        return base * std::pow (f, 0.6f * t);
    }

    TransientEngine()
    {
        for (auto& a : displayGain)  a.store (0.0f);
        for (auto& a : displayLevel) a.store (-120.0f);
    }

    //==========================================================================
    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        channels = std::clamp (numChannels, 1, maxChannels);
        Nmax = frameSizeFor (2, sampleRate);
        const int minN = frameSizeFor (0, sampleRate);

        // FFT de toutes les tailles possibles : 2 × N (SHORT, 1X) .. 8 × N (LONG, 4X)
        for (int i = 0; i < numFfts; ++i)
            ffts[i].setSize ((2 * minN) << i);

        const int maxM = 8 * Nmax, maxBins = maxM / 2 + 1;

        for (int c = 0; c < maxChannels; ++c)
        {
            inRing[c].assign ((size_t) Nmax, 0.0f);
            sideRing[c].assign ((size_t) Nmax, 0.0f);
            acc[c].assign ((size_t) (2 * Nmax), 0.0f);
            re[c].assign ((size_t) maxBins, 0.0f);
            im[c].assign ((size_t) maxBins, 0.0f);
            level[c].assign ((size_t) maxBins, -200.0f);
            smoothLevel[c].assign ((size_t) maxBins, -200.0f);
            floorDb[c].assign ((size_t) maxBins, -200.0f);
            target[c].assign ((size_t) maxBins, 0.0f);
            own[c].assign ((size_t) maxBins, 0.0f);
            state[c].assign ((size_t) maxBins, 0.0f);
            depthMul[c].assign ((size_t) maxBins, 1.0f);
            weight[c].assign ((size_t) maxBins, 1.0f);
            outDelay[c].prepare (Nmax);
        }
        wsum.assign ((size_t) (2 * Nmax), 0.0f);
        window.assign ((size_t) Nmax, 0.0f);
        frame.assign ((size_t) maxM, 0.0f);
        cep.assign ((size_t) maxM, 0.0f);
        gRe.assign ((size_t) maxBins, 1.0f);
        gIm.assign ((size_t) maxBins, 0.0f);
        cRe.assign ((size_t) maxBins, 0.0f);
        cIm.assign ((size_t) maxBins, 0.0f);
        sideRe.assign ((size_t) maxBins, 0.0f);
        sideIm.assign ((size_t) maxBins, 0.0f);
        linked.assign ((size_t) maxBins, -200.0f);
        linkedFloor.assign ((size_t) maxBins, -200.0f);
        temp.assign ((size_t) maxBins, 0.0f);
        prefix.assign ((size_t) maxBins + 1, 0.0);
        listen.assign ((size_t) maxBins, 0.0f);
        decayCoef.assign ((size_t) maxBins, 0.0f);
        displayBin.assign ((size_t) displayPoints, 0.0f);

        N = 0;
        M = 0;
        reset();
    }

    void reset()
    {
        for (int c = 0; c < maxChannels; ++c)
        {
            std::fill (inRing[c].begin(), inRing[c].end(), 0.0f);
            std::fill (sideRing[c].begin(), sideRing[c].end(), 0.0f);
            std::fill (acc[c].begin(), acc[c].end(), 0.0f);
            std::fill (state[c].begin(), state[c].end(), 0.0f);
            outDelay[c].reset();
        }
        std::fill (wsum.begin(), wsum.end(), 0.0f);
        time = 0;
        sinceFrame = 0;
        floorsValid = false;
        linkedValid = false;
        for (auto& a : displayGain)  a.store (0.0f, std::memory_order_relaxed);
        for (auto& a : displayLevel) a.store (-120.0f, std::memory_order_relaxed);
    }

    int getLatency() const noexcept               { return Nmax; }
    int getFrameSize() const noexcept             { return N; }
    int getFftSize() const noexcept               { return M; }
    int getHop() const noexcept                   { return N / overlap (settings.resolution); }

    void setSettings (const EngineSettings& s) noexcept     { settings = s; }
    const EngineSettings& getSettings() const noexcept      { return settings; }

    void setCurve (const CurveSpec& c) noexcept
    {
        if (c != curve)
        {
            curve = c;
            curveDirty = true;
        }
    }

    /** Écoute d'une bande : -1 = aucune ; 0..3 = bandes ; 4 = LOW CUT ; 5 = HIGH CUT. */
    void setListen (int band) noexcept
    {
        if (band != listenBand)
        {
            listenBand = band;
            curveDirty = true;
        }
    }

    void process (const float* const* in, const float* const* side, int sideChannels, float* const* out, int n) noexcept
    {
        const bool ms = settings.midSide && channels == 2;
        const bool useSide = side != nullptr && sideChannels > 0;

        for (int i = 0; i < n; ++i)
        {
            // Changement de fenêtre (ou premier passage) : on repart à zéro
            const int wantN = frameSizeFor (settings.window, fs);
            if (wantN != N)
                changeFrame (wantN);

            const int hop = N / overlap (settings.resolution);

            float a[maxChannels] = {}, s[maxChannels] = {};
            toInternal (in, i, channels, ms, a);
            if (useSide)
            {
                const float* sideIn[maxChannels] = { side[0], side[sideChannels > 1 ? 1 : 0] };
                toInternal (sideIn, i, channels, ms, s);
            }

            const int w = (int) (time % (unsigned long long) N);
            for (int c = 0; c < channels; ++c)
            {
                inRing[c][(size_t) w] = a[c];
                sideRing[c][(size_t) w] = s[c];
            }

            if (++sinceFrame >= hop && time + 1 >= (unsigned long long) N)
            {
                sinceFrame = 0;
                runFrame (useSide);
            }

            const int ringSize = 2 * N;
            const int r = (int) ((time + (unsigned long long) ringSize - (unsigned long long) N) % (unsigned long long) ringSize);
            const float norm = wsum[(size_t) r];
            float y[maxChannels] = {};
            for (int c = 0; c < channels; ++c)
            {
                y[c] = acc[c][(size_t) r] / std::max (norm, 0.5f);
                acc[c][(size_t) r] = 0.0f;
                y[c] = outDelay[c].process (y[c], Nmax - N);   // latence fixe = fenêtre LONG
            }
            wsum[(size_t) r] = 0.0f;

            fromInternal (y, out, i, channels, ms);
            ++time;
        }
    }

    //==========================================================================
    /** Gain affiché (dB, négatif = coupe, positif = bosse), max des canaux. */
    float getDisplayGain (int i) const noexcept        { return displayGain[(size_t) i].load (std::memory_order_relaxed); }
    float getDisplayLevel (int i) const noexcept       { return displayLevel[(size_t) i].load (std::memory_order_relaxed); }

    /** Traitement actuel (dB, toujours positif) à une fréquence, pour les tests. */
    float getAmountAtFrequency (int channel, float hz) const noexcept
    {
        if (M == 0)
            return 0.0f;
        const int k = std::clamp ((int) std::lround (hz * (float) M / (float) fs), 0, M / 2);
        return state[channel][(size_t) k];
    }

private:
    static constexpr double pi = 3.14159265358979323846;
    static constexpr int numFfts = 5;   // 2 × SHORT .. 8 × LONG

    static void toInternal (const float* const* in, int i, int numChannels, bool ms, float* a) noexcept
    {
        if (numChannels == 1) { a[0] = in[0][i]; return; }
        const float l = in[0][i], r = in[1][i];
        if (ms) { a[0] = 0.5f * (l + r); a[1] = 0.5f * (l - r); }
        else    { a[0] = l; a[1] = r; }
    }

    static void fromInternal (const float* y, float* const* out, int i, int numChannels, bool ms) noexcept
    {
        if (numChannels == 1) { out[0][i] = y[0]; return; }
        if (ms) { out[0][i] = y[0] + y[1]; out[1][i] = y[0] - y[1]; }
        else    { out[0][i] = y[0]; out[1][i] = y[1]; }
    }

    void changeFrame (int newN) noexcept
    {
        N = newN;
        double sum = 0.0;
        for (int i = 0; i < N; ++i)
        {
            window[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) N));
            sum += window[(size_t) i];
        }
        levelScale = (float) (2.0 / sum);

        for (int c = 0; c < maxChannels; ++c)
        {
            std::fill (inRing[c].begin(), inRing[c].end(), 0.0f);
            std::fill (sideRing[c].begin(), sideRing[c].end(), 0.0f);
            std::fill (acc[c].begin(), acc[c].end(), 0.0f);
        }
        std::fill (wsum.begin(), wsum.end(), 0.0f);
        time = 0;
        sinceFrame = 0;
        M = 0;   // reconfiguré à la trame suivante
    }

    void configure()
    {
        const int os = settings.oversample >= 4 ? 4 : settings.oversample >= 2 ? 2 : 1;
        // Toujours au moins 2 × N : le filtre de chaque trame déborde dans les zéros au lieu de
        // se replier sur la trame (sinon pré-écho artificiel, même en phase minimale).
        const int newM = 2 * N * os;
        if (newM == M)
            return;

        M = newM;
        const int minN = frameSizeFor (0, fs);
        int idx = 0;
        while (((2 * minN) << idx) < M && idx < numFfts - 1)
            ++idx;
        fft = &ffts[idx];
        bins = M / 2 + 1;
        binHz = (float) (fs / (double) M);

        for (int c = 0; c < maxChannels; ++c)
            std::fill (state[c].begin(), state[c].end(), 0.0f);
        floorsValid = false;
        linkedValid = false;

        for (int i = 0; i < displayPoints; ++i)
            displayBin[(size_t) i] = displayFrequency (i) / binHz;

        curveDirty = true;
        coefKey = -1.0f;
    }

    void updateCurve()
    {
        curveSymmetric = true;
        for (const auto& b : curve.bands)
            if (b.on && std::abs (b.balance) > 1.0e-6f)
                curveSymmetric = false;

        for (int k = 0; k < bins; ++k)
        {
            const float f = (float) k * binHz;
            const auto p = phocus::evaluateCurve (curve, f);
            // Une bande décale DEPTH : ±24 dB de sensibilité = × 4 / ÷ 4
            depthMul[0][(size_t) k] = std::pow (2.0f, p.sensA / 12.0f);
            depthMul[1][(size_t) k] = std::pow (2.0f, p.sensB / 12.0f);
            weight[0][(size_t) k] = p.weightA;
            weight[1][(size_t) k] = p.weightB;

            float shape = 1.0f;
            if (listenBand >= 0 && listenBand < CurveSpec::numBands)
            {
                const auto& band = curve.bands[listenBand];
                if (band.type == phocus::tilt)
                    shape = 1.0f;
                else if (f <= 0.0f)
                    shape = band.type == phocus::lowShelf ? 1.0f : 0.0f;
                else
                    shape = std::clamp (phocus::bandShape (band, std::log2 (f / std::max (1.0f, band.freq))), 0.0f, 1.0f);
            }
            else if (listenBand == CurveSpec::numBands)
            {
                CutSpec cut = curve.lowCut; cut.on = true;
                shape = 1.0f - phocus::cutWeight (cut, f, false);
            }
            else if (listenBand == CurveSpec::numBands + 1)
            {
                CutSpec cut = curve.highCut; cut.on = true;
                shape = 1.0f - phocus::cutWeight (cut, f, true);
            }
            listen[(size_t) k] = shape;
        }
        curveDirty = false;
    }

    void updateCoefficients (int hop)
    {
        const float key = settings.decay * 1.0e3f + settings.decayTilt * 10.0f + (float) hop * 1.0e6f + (float) M * 1.0e-3f;
        if (! (key < coefKey) && ! (coefKey < key))
            return;
        coefKey = key;

        const float hopSec = (float) hop / (float) fs;
        for (int k = 0; k < bins; ++k)
            decayCoef[(size_t) k] = std::exp (-hopSec / decaySeconds (settings.decay, settings.decayTilt, (float) k * binHz));

        // Fond : monte lentement (un son qui reste n'est plus un transitoire), descend vite (prêt pour le coup suivant)
        floorUp = 1.0f - std::exp (-hopSec / 0.060f);
        floorDown = 1.0f - std::exp (-hopSec / 0.012f);
    }

    void analyse (const std::vector<float>& ring, float* outRe, float* outIm) noexcept
    {
        const int start = (int) ((time + 1) % (unsigned long long) N);
        for (int j = 0; j < N; ++j)
        {
            int idx = start + j;
            if (idx >= N)
                idx -= N;
            frame[(size_t) j] = ring[(size_t) idx] * window[(size_t) j];
        }
        std::fill (frame.begin() + N, frame.begin() + M, 0.0f);
        fft->forward (frame.data(), outRe, outIm);
    }

    void levels (const float* r, const float* i, float* out) const noexcept
    {
        const float scale2 = levelScale * levelScale;
        for (int k = 0; k < bins; ++k)
            out[k] = 10.0f * std::log10 ((r[k] * r[k] + i[k] * i[k]) * scale2 + 1.0e-24f);
    }

    /** Moyenne glissante sur une largeur en octaves (sommes cumulées). exponent 1 = linéaire, sinon moyenne de puissance p (valeurs >= 0). */
    void smooth (const float* in, float* out, float octaves, float minHalfWidth, float exponent) noexcept
    {
        const bool linear = exponent < 1.0001f;
        const bool quartic = std::abs (exponent - 4.0f) < 1.0e-4f;
        prefix[0] = 0.0;
        for (int k = 0; k < bins; ++k)
        {
            const double x = (double) in[k];
            prefix[(size_t) k + 1] = prefix[(size_t) k] + (linear ? x : (quartic ? (x * x) * (x * x) : std::pow (std::max (0.0, x), (double) exponent)));
        }

        const float spread = 0.5f * (std::pow (2.0f, 0.5f * octaves) - std::pow (2.0f, -0.5f * octaves));
        const double total = (double) bins;

        auto integral = [this, total] (double x) noexcept
        {
            x = std::clamp (x, 0.0, total);
            const int i0 = (int) x;
            if (i0 >= bins)
                return prefix[(size_t) bins];
            return prefix[(size_t) i0] + (x - (double) i0) * (prefix[(size_t) i0 + 1] - prefix[(size_t) i0]);
        };

        for (int k = 0; k < bins; ++k)
        {
            const double h = (double) std::max (minHalfWidth, spread * (float) k);
            const double centre = (double) k + 0.5;
            const double lo = std::max (0.0, centre - h), hi = std::min (total, centre + h);
            const double mean = (integral (hi) - integral (lo)) / std::max (1.0e-9, hi - lo);
            out[k] = linear ? (float) mean : (float) (quartic ? std::sqrt (std::sqrt (std::max (0.0, mean))) : std::pow (std::max (0.0, mean), 1.0 / (double) exponent));
        }
    }

    /**
        Traitement voulu (dB, positif) pour un niveau lissé lev et son fond fl.
        Met à jour le fond.
    */
    void detect (const float* lev, float* fl, const float* mul, const float* w, float* out) noexcept
    {
        const auto& s = settings;
        const float sens = std::clamp (s.sensitivity, 0.0f, 10.0f);
        const float sharp = std::clamp (s.sharpness, 0.0f, 10.0f);
        const float depth = std::clamp (s.depth, 0.0f, 20.0f);

        const float threshold = 14.0f - 1.2f * sens;            // montée minimale : 14 dB (0) .. 2 dB (10)
        const float knee = 3.0f;
        const float gain = 0.09f * depth * (1.0f + 1.5f * (depth / 20.0f) * (depth / 20.0f));
        const float cap = s.boost ? maxBoostDb : maxCutDb;
        const float spreadOct = 8.0f * std::exp (-0.45f * sharp);   // 8 octaves (0, quasi large bande) .. 0,09 (10)
        const float os = (float) M / (float) N;

        float top = -200.0f;
        for (int k = 1; k < bins; ++k)
            top = std::max (top, lev[k]);
        const float gateDb = std::max (top - 70.0f, -110.0f);

        if (! floorsValid)
            std::copy (lev, lev + bins, fl);

        for (int k = 0; k < bins; ++k)
        {
            const float rise = lev[k] - fl[k];
            const float x = rise - threshold;
            float r;
            if (x <= -0.5f * knee)      r = 0.0f;
            else if (x >= 0.5f * knee)  r = x;
            else                        r = (x + 0.5f * knee) * (x + 0.5f * knee) / (2.0f * knee);

            const float g = std::clamp ((lev[k] - gateDb) / 10.0f, 0.0f, 1.0f);
            temp[(size_t) k] = std::min (cap, r * gain * mul[k]) * g * w[k];

            fl[k] += (rise > 0.0f ? floorUp : floorDown) * rise;
        }

        // Moyenne de puissance 4 : un transitoire étroit garde du poids dans une fenêtre large
        smooth (temp.data(), out, spreadOct, 0.75f * os, 4.0f);
    }

    void runFrame (bool useSide) noexcept
    {
        configure();
        if (curveDirty)
            updateCurve();

        const int hop = N / overlap (settings.resolution);
        updateCoefficients (hop);

        // Analyse ; niveau lissé sur ~1/12 d'octave (moins de bruit de case)
        const float os = (float) M / (float) N;
        for (int c = 0; c < channels; ++c)
        {
            analyse (inRing[c], re[c].data(), im[c].data());
            if (useSide)
            {
                analyse (sideRing[c], sideRe.data(), sideIm.data());
                levels (sideRe.data(), sideIm.data(), level[c].data());
            }
            else
            {
                levels (re[c].data(), im[c].data(), level[c].data());
            }
            smooth (level[c].data(), smoothLevel[c].data(), 1.0f / 12.0f, 1.0f * os, 1.0f);
        }

        float wA = 1.0f, wB = 1.0f;
        phocus::balanceWeights (channels == 2 ? settings.balance : 0.0f, wA, wB);
        const float chanWeight[maxChannels] = { wA, wB };

        if (channels == 1)
        {
            detect (smoothLevel[0].data(), floorDb[0].data(), depthMul[0].data(), weight[0].data(), target[0].data());
        }
        else
        {
            const float link = std::clamp (settings.link, 0.0f, 1.0f);

            if (link > 0.0f)
            {
                for (int k = 0; k < bins; ++k)
                {
                    const float pa = std::pow (10.0f, 0.1f * smoothLevel[0][(size_t) k]);
                    const float pb = std::pow (10.0f, 0.1f * smoothLevel[1][(size_t) k]);
                    linked[(size_t) k] = 10.0f * std::log10 (0.5f * (pa + pb) + 1.0e-24f);
                }
                // Le fond lié n'est mis à jour qu'une fois par trame ; courbe différente par canal :
                // le canal B repart d'une copie du fond d'avant la mise à jour.
                if (! linkedValid)
                    std::copy (linked.begin(), linked.begin() + bins, linkedFloor.begin());   // LINK remonté depuis 0
                linkedValid = true;

                const bool wasValid = floorsValid;
                if (! curveSymmetric)
                    std::copy (linkedFloor.begin(), linkedFloor.begin() + bins, own[1].begin());
                detect (linked.data(), linkedFloor.data(), depthMul[0].data(), weight[0].data(), target[0].data());
                if (curveSymmetric)
                {
                    std::copy (target[0].begin(), target[0].begin() + bins, target[1].begin());
                }
                else
                {
                    floorsValid = wasValid;
                    detect (linked.data(), own[1].data(), depthMul[1].data(), weight[1].data(), target[1].data());
                }
                floorsValid = wasValid;
            }
            else
            {
                linkedValid = false;
                std::fill (target[0].begin(), target[0].begin() + bins, 0.0f);
                std::fill (target[1].begin(), target[1].begin() + bins, 0.0f);
            }

            if (link < 1.0f)
            {
                const bool wasValid = floorsValid;
                for (int c = 0; c < 2; ++c)
                {
                    floorsValid = wasValid;
                    detect (smoothLevel[c].data(), floorDb[c].data(), depthMul[c].data(), weight[c].data(), own[c].data());
                    for (int k = 0; k < bins; ++k)
                        target[c][(size_t) k] = link * target[c][(size_t) k] + (1.0f - link) * own[c][(size_t) k];
                }
            }
            else
            {
                // garder les fonds par canal à jour pour un changement de LINK sans saut
                for (int c = 0; c < 2; ++c)
                    for (int k = 0; k < bins; ++k)
                    {
                        const float rise = smoothLevel[c][(size_t) k] - floorDb[c][(size_t) k];
                        floorDb[c][(size_t) k] = floorsValid ? floorDb[c][(size_t) k] + (rise > 0.0f ? floorUp : floorDown) * rise
                                                             : smoothLevel[c][(size_t) k];
                    }
            }
        }
        floorsValid = true;

        // Enveloppe (attaque immédiate, retour DECAY), gain, synthèse
        const int ringSize = 2 * N;
        const int start = (int) ((time + 1 + (unsigned long long) ringSize - (unsigned long long) N) % (unsigned long long) ringSize);
        const bool listening = listenBand >= 0;
        const float sign = settings.boost ? 1.0f : -1.0f;

        for (int c = 0; c < channels; ++c)
        {
            auto& st = state[c];
            float* xr = re[c].data();
            float* xi = im[c].data();
            const float cw = chanWeight[c];

            for (int k = 0; k < bins; ++k)
            {
                const float t = target[c][(size_t) k] * cw;
                st[(size_t) k] = t >= st[(size_t) k] ? t : t + decayCoef[(size_t) k] * (st[(size_t) k] - t);
            }

            if (settings.minimumPhase)
                minimumPhaseGain (st, sign);
            else
                for (int k = 0; k < bins; ++k)
                {
                    gRe[(size_t) k] = std::exp (sign * 0.11512925f * st[(size_t) k]);
                    gIm[(size_t) k] = 0.0f;
                }

            for (int k = 0; k < bins; ++k)
            {
                float mr = gRe[(size_t) k], mi = gIm[(size_t) k];
                if (listening)
                {
                    mr = (1.0f - mr) * listen[(size_t) k];
                    mi = -mi * listen[(size_t) k];
                }
                const float a = xr[k], b = xi[k];
                xr[k] = a * mr - b * mi;
                xi[k] = a * mi + b * mr;
            }

            fft->inverse (xr, xi, frame.data());

            auto& ac = acc[c];
            for (int j = 0; j < N; ++j)
            {
                int idx = start + j;
                if (idx >= ringSize)
                    idx -= ringSize;
                ac[(size_t) idx] += frame[(size_t) j] * window[(size_t) j];
            }
        }

        for (int j = 0; j < N; ++j)
        {
            int idx = start + j;
            if (idx >= ringSize)
                idx -= ringSize;
            wsum[(size_t) idx] += window[(size_t) j] * window[(size_t) j];
        }

        publishDisplay (sign);
    }

    /** Gain en phase minimale à partir des dB : cepstre réel, repli causal, exponentielle complexe. */
    void minimumPhaseGain (const std::vector<float>& amountDb, float sign) noexcept
    {
        for (int k = 0; k < bins; ++k)
        {
            cRe[(size_t) k] = sign * 0.11512925f * amountDb[(size_t) k];   // ln |G|
            cIm[(size_t) k] = 0.0f;
        }
        fft->inverse (cRe.data(), cIm.data(), cep.data());

        const int half = M / 2;
        for (int n = 1; n < half; ++n)
            cep[(size_t) n] *= 2.0f;
        for (int n = half + 1; n < M; ++n)
            cep[(size_t) n] = 0.0f;

        fft->forward (cep.data(), cRe.data(), cIm.data());
        for (int k = 0; k < bins; ++k)
        {
            const float mag = std::exp (cRe[(size_t) k]);
            gRe[(size_t) k] = mag * std::cos (cIm[(size_t) k]);
            gIm[(size_t) k] = mag * std::sin (cIm[(size_t) k]);
        }
    }

    void publishDisplay (float sign) noexcept
    {
        for (int i = 0; i < displayPoints; ++i)
        {
            const float b = std::clamp (displayBin[(size_t) i], 0.0f, (float) (bins - 1));
            const int k0 = std::min ((int) b, bins - 2);
            const float t = b - (float) k0;

            float amount = 0.0f, lev = -200.0f;
            for (int c = 0; c < channels; ++c)
            {
                amount = std::max (amount, state[c][(size_t) k0] + t * (state[c][(size_t) k0 + 1] - state[c][(size_t) k0]));
                lev = std::max (lev, level[c][(size_t) k0] + t * (level[c][(size_t) k0 + 1] - level[c][(size_t) k0]));
            }
            displayGain[(size_t) i].store (sign * amount, std::memory_order_relaxed);
            displayLevel[(size_t) i].store (lev, std::memory_order_relaxed);
        }
    }

    //==========================================================================
    EngineSettings settings;
    CurveSpec curve;
    bool curveDirty = true, curveSymmetric = true, floorsValid = false, linkedValid = false;
    int listenBand = -1;

    double fs = 48000.0;
    int channels = 2, N = 0, Nmax = 2048, M = 0, bins = 0;
    float binHz = 1.0f, levelScale = 1.0f, coefKey = -1.0f, floorUp = 0.1f, floorDown = 0.3f;
    unsigned long long time = 0;
    int sinceFrame = 0;

    RealFFT ffts[numFfts];
    RealFFT* fft = &ffts[0];

    std::vector<float> window, frame, wsum, cep;
    std::vector<float> inRing[maxChannels], sideRing[maxChannels], acc[maxChannels];
    std::vector<float> re[maxChannels], im[maxChannels], level[maxChannels], smoothLevel[maxChannels], floorDb[maxChannels];
    std::vector<float> target[maxChannels], own[maxChannels], state[maxChannels], depthMul[maxChannels], weight[maxChannels];
    std::vector<float> gRe, gIm, cRe, cIm, sideRe, sideIm, linked, linkedFloor, temp, listen, decayCoef, displayBin;
    std::vector<double> prefix;
    DelayLine outDelay[maxChannels];

    std::atomic<float> displayGain[displayPoints], displayLevel[displayPoints];
};

} // namespace phlick
