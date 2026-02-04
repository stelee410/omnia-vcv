#pragma once
#include <rack.hpp>

using namespace rack;

// Declare the Plugin, defined in plugin.cpp
extern Plugin* pluginInstance;

// Declare each Model, defined in each module source file
extern Model* modelBasicOscillator;
extern Model* modelStereoEffects;
extern Model* modelMidiClockSync;
extern Model* modelChordSynth;
extern Model* modelChordPadSynth;
extern Model* modelChordPluckSynth;
extern Model* modelAmbientRandomSynth;
extern Model* modelOrganicParticleSynth;
