#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;

	// Add modules here
	p->addModel(modelBasicOscillator);
	p->addModel(modelStereoEffects);
	p->addModel(modelMidiClockSync);
	p->addModel(modelChordSynth);
	p->addModel(modelChordPadSynth);
	p->addModel(modelChordPluckSynth);
	p->addModel(modelAmbientRandomSynth);
	p->addModel(modelOrganicParticleSynth);
	p->addModel(modelBuildupLooper);
}
