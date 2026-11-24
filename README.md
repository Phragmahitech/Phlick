# Phlick

Processeur adaptatif de transitoires (VST3, JUCE 8, C++17) par Phragma.

- Moteur : `Source/TransientEngine.h` (STFT, détection de montée d'énergie par zone de fréquence, coupe ou bosse avec attaque immédiate et retour réglable, phase linéaire ou minimale). Sans dépendance, testé seul (`Tests/EngineTests.cpp`).
- Courbe de sensibilité : `Source/SensitivityCurve.h` ; FFT réelle : `Source/RealFFT.h`.
- Interface : `Source/PluginEditor.cpp` (couleurs Phragma electricity, même look que Phocus).

Compilation : `cmake -S . -B build -DPHLICK_BUILD_TESTS=ON && cmake --build build --config Release`.
