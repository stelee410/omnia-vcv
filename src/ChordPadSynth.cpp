#include "plugin.hpp"
#include <dsp/filter.hpp>
#include <cmath>
#include <vector>

// ADSR Envelope
struct ADSR {
	float attack = 0.01f;
	float decay = 0.1f;
	float sustain = 0.7f;
	float release = 0.2f;
	
	enum State {
		IDLE,
		ATTACK,
		DECAY,
		SUSTAIN,
		RELEASE
	};
	
	State state = IDLE;
	float output = 0.f;
	
	void setAttack(float a) { attack = std::max(0.001f, a); }
	void setDecay(float d) { decay = std::max(0.001f, d); }
	void setSustain(float s) { sustain = math::clamp(s, 0.f, 1.f); }
	void setRelease(float r) { release = std::max(0.001f, r); }
	
	void gate(bool on, float sampleTime) {
		if (on && state == IDLE) {
			state = ATTACK;
		} else if (!on && state != IDLE && state != RELEASE) {
			state = RELEASE;
		}
	}
	
	void process(float sampleTime) {
		switch (state) {
			case IDLE:
				output = 0.f;
				break;
			case ATTACK:
				output += sampleTime / attack;
				if (output >= 1.f) {
					output = 1.f;
					state = DECAY;
				}
				break;
			case DECAY:
				output -= sampleTime / decay;
				if (output <= sustain) {
					output = sustain;
					state = SUSTAIN;
				}
				break;
			case SUSTAIN:
				output = sustain;
				break;
			case RELEASE:
				output -= sampleTime / release;
				if (output <= 0.f) {
					output = 0.f;
					state = IDLE;
				}
				break;
		}
	}
};

// Voice structure for each chord note
struct Voice {
	float phase = 0.f;
	float frequency = 0.f;
	float targetFrequency = 0.f;
	bool active = false;
	
	enum Waveform {
		SINE,
		TRIANGLE,
		SAW,
		SQUARE
	};
	
	Waveform waveform = SINE;
	
	// Smooth frequency transition to avoid clicks
	// Using exponential smoothing for smooth transitions
	static constexpr float FREQ_SMOOTH = 0.01f; // Smoothing coefficient (lower = smoother, but slower)
	
	void setTargetFrequency(float target) {
		targetFrequency = target;
	}
	
	float generate(float sampleTime) {
		if (!active) return 0.f;
		
		// Smoothly transition to target frequency to avoid clicks
		if (std::abs(frequency - targetFrequency) > 0.1f) {
			frequency += (targetFrequency - frequency) * FREQ_SMOOTH;
		} else {
			frequency = targetFrequency;
		}
		
		// Keep phase continuous - don't reset it
		phase += frequency * sampleTime;
		if (phase >= 1.f) phase -= 1.f;
		if (phase < 0.f) phase += 1.f;
		
		float signal = 0.f;
		switch (waveform) {
			case SINE:
				signal = std::sin(2.f * M_PI * phase);
				break;
			case TRIANGLE:
				signal = phase < 0.5f ? 4.f * phase - 1.f : 3.f - 4.f * phase;
				break;
			case SAW:
				signal = 2.f * phase - 1.f;
				break;
			case SQUARE:
				signal = phase < 0.5f ? 1.f : -1.f;
				break;
		}
		
		return signal;
	}
};

// Chord types
enum ChordType {
	MAJOR,      // 0: [0, 4, 7]
	MINOR,      // 1: [0, 3, 7]
	DIMINISHED, // 2: [0, 3, 6]
	AUGMENTED,  // 3: [0, 4, 8]
	SEVENTH,    // 4: [0, 4, 7, 10]
	SUSPENDED   // 5: [0, 5, 7]
};

// Pad preset types
enum PadPreset {
	PAD_UNIVERSE, // Soft sine with reverb
	PAD_OCEAN,    // Triangle with filter
	PAD_DESERT,   // Saw with lowpass
	PAD_HARP,     // Bright sine
	PAD_PIANO     // Square with harmonics
};

struct ChordPadSynth : Module {
	enum ParamId {
		PAD_PRESET_PARAM,
		OCTAVE_PARAM,
		// Slot 0
		SLOT0_PITCH_PARAM,
		SLOT0_TYPE_PARAM,
		// Slot 1
		SLOT1_PITCH_PARAM,
		SLOT1_TYPE_PARAM,
		// Slot 2
		SLOT2_PITCH_PARAM,
		SLOT2_TYPE_PARAM,
		// Slot 3
		SLOT3_PITCH_PARAM,
		SLOT3_TYPE_PARAM,
		// EG
		ATTACK_PARAM,
		DECAY_RELEASE_PARAM,
		SUSTAIN_PARAM,
		PARAMS_LEN
	};
	
	enum InputId {
		CLOCK_INPUT,
		RESET_INPUT,
		AUX_INPUT,
		INPUTS_LEN
	};
	
	enum OutputId {
		AUDIO_OUTPUT,
		OUTPUTS_LEN
	};
	
	enum LightId {
		SLOT0_LIGHT,
		SLOT1_LIGHT,
		SLOT2_LIGHT,
		SLOT3_LIGHT,
		LIGHTS_LEN
	};
	
	static constexpr int MAX_VOICES = 4;
	Voice voices[MAX_VOICES];
	ADSR envelope;
	
	// Current playing slot
	int currentSlot = 0;
	float lastClock = 0.f;
	float lastReset = 0.f;
	
	// Filter for pad sound
	dsp::RCFilter filter;
	
	// Frequency detection for aux input
	float detectedFreq = 0.f;
	float lastAuxSample = 0.f;
	int zeroCrossCount = 0;
	int zeroCrossSamples = 0;
	static constexpr int MIN_ZERO_CROSS_SAMPLES = 100; // Minimum samples for frequency detection
	
	ChordPadSynth() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		
		// Pad preset (0-4)
		configSwitch(PAD_PRESET_PARAM, 0.f, 4.f, 0.f, "Pad Preset", 
			{"Universe", "Ocean", "Desert", "Harp", "Piano"});
		
		// Octave shift (-2 to +2 octaves)
		configParam(OCTAVE_PARAM, -2.f, 2.f, 0.f, "Octave", " oct");
		
		// Slot 0
		configSwitch(SLOT0_PITCH_PARAM, 0.f, 11.f, 0.f, "Slot 0 Pitch", 
			{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
		configSwitch(SLOT0_TYPE_PARAM, 0.f, 5.f, 0.f, "Slot 0 Type", 
			{"Major", "Minor", "Dim", "Aug", "7", "Sus"});
		
		// Slot 1
		configSwitch(SLOT1_PITCH_PARAM, 0.f, 11.f, 0.f, "Slot 1 Pitch", 
			{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
		configSwitch(SLOT1_TYPE_PARAM, 0.f, 5.f, 0.f, "Slot 1 Type", 
			{"Major", "Minor", "Dim", "Aug", "7", "Sus"});
		
		// Slot 2
		configSwitch(SLOT2_PITCH_PARAM, 0.f, 11.f, 0.f, "Slot 2 Pitch", 
			{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
		configSwitch(SLOT2_TYPE_PARAM, 0.f, 5.f, 0.f, "Slot 2 Type", 
			{"Major", "Minor", "Dim", "Aug", "7", "Sus"});
		
		// Slot 3
		configSwitch(SLOT3_PITCH_PARAM, 0.f, 11.f, 0.f, "Slot 3 Pitch", 
			{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
		configSwitch(SLOT3_TYPE_PARAM, 0.f, 5.f, 0.f, "Slot 3 Type", 
			{"Major", "Minor", "Dim", "Aug", "7", "Sus"});
		
		// EG
		configParam(ATTACK_PARAM, 0.001f, 2.f, 0.01f, "Attack", " s");
		configParam(DECAY_RELEASE_PARAM, 0.001f, 2.f, 0.1f, "Decay/Release", " s");
		configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.7f, "Sustain");
		
		// Inputs
		configInput(CLOCK_INPUT, "Clock");
		configInput(RESET_INPUT, "Reset");
		configInput(AUX_INPUT, "Aux In");
		
		// Outputs
		configOutput(AUDIO_OUTPUT, "Audio");
		
		// Initialize voices
		for (int i = 0; i < MAX_VOICES; i++) {
			voices[i].waveform = Voice::SINE;
		}
	}
	
	// Get chord intervals based on type
	std::vector<float> getChordIntervals(ChordType type) {
		std::vector<float> intervals;
		
		switch (type) {
			case MAJOR:
				intervals = {0.f, 4.f, 7.f};
				break;
			case MINOR:
				intervals = {0.f, 3.f, 7.f};
				break;
			case DIMINISHED:
				intervals = {0.f, 3.f, 6.f};
				break;
			case AUGMENTED:
				intervals = {0.f, 4.f, 8.f};
				break;
			case SEVENTH:
				intervals = {0.f, 4.f, 7.f, 10.f};
				break;
			case SUSPENDED:
				intervals = {0.f, 5.f, 7.f};
				break;
		}
		
		return intervals;
	}
	
	// Get pad preset waveform and filter settings
	void getPadPreset(PadPreset preset, Voice::Waveform& waveform, float& cutoffRatio) {
		switch (preset) {
			case PAD_UNIVERSE:
				waveform = Voice::SINE;
				cutoffRatio = 0.8f; // Soft
				break;
			case PAD_OCEAN:
				waveform = Voice::TRIANGLE;
				cutoffRatio = 0.6f;
				break;
			case PAD_DESERT:
				waveform = Voice::SAW;
				cutoffRatio = 0.4f; // Lowpass
				break;
			case PAD_HARP:
				waveform = Voice::SINE;
				cutoffRatio = 1.0f; // Bright
				break;
			case PAD_PIANO:
				waveform = Voice::SQUARE;
				cutoffRatio = 0.7f;
				break;
		}
	}
	
	// Trigger a chord slot
	void triggerSlot(int slot) {
		// Get pitch and type for this slot
		int pitchParam, typeParam;
		switch (slot) {
			case 0:
				pitchParam = SLOT0_PITCH_PARAM;
				typeParam = SLOT0_TYPE_PARAM;
				break;
			case 1:
				pitchParam = SLOT1_PITCH_PARAM;
				typeParam = SLOT1_TYPE_PARAM;
				break;
			case 2:
				pitchParam = SLOT2_PITCH_PARAM;
				typeParam = SLOT2_TYPE_PARAM;
				break;
			case 3:
				pitchParam = SLOT3_PITCH_PARAM;
				typeParam = SLOT3_TYPE_PARAM;
				break;
			default:
				return;
		}
		
		int rootNote = (int)std::round(params[pitchParam].getValue());
		ChordType chordType = (ChordType)(int)std::round(params[typeParam].getValue());
		
		// Get chord intervals
		std::vector<float> intervals = getChordIntervals(chordType);
		
		// Calculate root frequency
		float rootFreq;
		bool useAuxInput = inputs[AUX_INPUT].isConnected() && detectedFreq > 20.f && detectedFreq < 20000.f;
		
		if (useAuxInput) {
			// Use detected frequency from aux input as central C (C4)
			// Apply octave shift
			float octaveShift = params[OCTAVE_PARAM].getValue();
			rootFreq = detectedFreq * std::pow(2.f, octaveShift);
		} else {
			// Use pitch parameter (C4 = 60, so rootNote 0 = C4)
			rootFreq = dsp::FREQ_C4 * std::pow(2.f, (rootNote - 0.f) / 12.f);
			// Apply octave shift
			float octaveShift = params[OCTAVE_PARAM].getValue();
			rootFreq *= std::pow(2.f, octaveShift);
		}
		
		// Get pad preset (only used if aux input is not connected)
		Voice::Waveform waveform = Voice::SINE; // Default initialization
		float cutoffRatio = 0.8f; // Default initialization
		if (!useAuxInput) {
			PadPreset preset = (PadPreset)(int)std::round(params[PAD_PRESET_PARAM].getValue());
			getPadPreset(preset, waveform, cutoffRatio);
		} else {
			// When using aux input, use sine wave and moderate filter
			waveform = Voice::SINE;
			cutoffRatio = 0.8f;
		}
		
		// Set up voices
		int activeVoiceCount = std::min((int)intervals.size(), MAX_VOICES);
		for (int i = 0; i < activeVoiceCount; i++) {
			float semitones = intervals[i];
			float newFreq = rootFreq * std::pow(2.f, semitones / 12.f);
			// Set target frequency instead of directly setting frequency
			// This allows smooth transition without clicks
			voices[i].setTargetFrequency(newFreq);
			voices[i].waveform = waveform;
			voices[i].active = true;
			// Don't reset phase - keep it continuous to avoid clicks
		}
		
		// Deactivate unused voices smoothly
		for (int i = activeVoiceCount; i < MAX_VOICES; i++) {
			// Set target frequency to 0 for smooth fade out
			voices[i].setTargetFrequency(0.f);
			// Only deactivate if frequency is very low
			if (voices[i].frequency < 1.f) {
				voices[i].active = false;
			}
		}
		
		// Trigger envelope smoothly
		// If envelope is in sustain or decay, keep it going (no restart)
		// Only trigger new attack if envelope is idle or in release
		// This prevents clicks from envelope restarting mid-cycle
		if (envelope.state == ADSR::IDLE || envelope.state == ADSR::RELEASE) {
			envelope.gate(true, 1.f / 44100.f);
		} else if (envelope.state == ADSR::SUSTAIN) {
			// If in sustain, we can smoothly transition by keeping gate on
			// The envelope will naturally continue
			envelope.gate(true, 1.f / 44100.f);
		}
		// If in attack or decay, don't restart - let it continue naturally
	}
	
	void process(const ProcessArgs& args) override {
		// Update ADSR parameters
		envelope.setAttack(params[ATTACK_PARAM].getValue());
		float decayRelease = params[DECAY_RELEASE_PARAM].getValue();
		envelope.setDecay(decayRelease);
		envelope.setRelease(decayRelease); // Use same value for decay and release
		envelope.setSustain(params[SUSTAIN_PARAM].getValue());
		
		// Detect frequency from aux input using zero-crossing detection
		if (inputs[AUX_INPUT].isConnected()) {
			float auxSample = inputs[AUX_INPUT].getVoltage() / 5.f; // Normalize to -1 to 1
			
			// Zero-crossing detection
			if ((lastAuxSample <= 0.f && auxSample > 0.f) || (lastAuxSample >= 0.f && auxSample < 0.f)) {
				zeroCrossCount++;
				if (zeroCrossCount >= 2) {
					// Calculate frequency from zero-crossing period (period = time between two zero crossings)
					if (zeroCrossSamples > 0) {
						float period = (float)zeroCrossSamples * args.sampleTime;
						if (period > 0.f) {
							float freq = 1.f / period;
							// Smooth the detected frequency to avoid jitter
							if (freq > 20.f && freq < 20000.f) {
								detectedFreq = detectedFreq * 0.9f + freq * 0.1f;
							}
						}
					}
					// Reset for next period measurement
					zeroCrossSamples = 0;
				} else {
					// First zero crossing, reset counter
					zeroCrossSamples = 0;
				}
			}
			zeroCrossSamples++;
			lastAuxSample = auxSample;
		} else {
			// Reset frequency detection when aux input is disconnected
			detectedFreq = 0.f;
			zeroCrossCount = 0;
			zeroCrossSamples = 0;
		}
		
		// Handle reset input
		if (inputs[RESET_INPUT].isConnected()) {
			float reset = inputs[RESET_INPUT].getVoltage();
			bool resetRising = reset > 1.f && lastReset <= 1.f;
			
			if (resetRising) {
				// Reset to slot 0
				currentSlot = 0;
			}
			
			lastReset = reset;
		}
		
		// Detect clock rising edge
		if (inputs[CLOCK_INPUT].isConnected()) {
			float clock = inputs[CLOCK_INPUT].getVoltage();
			if (clock > 1.f && lastClock <= 1.f) {
				// Rising edge detected, trigger next slot
				triggerSlot(currentSlot);
				currentSlot = (currentSlot + 1) % 4; // Cycle through 4 slots
			}
			lastClock = clock;
		}
		
		// Process envelope
		envelope.process(args.sampleTime);
		
		// Generate voices
		float sum = 0.f;
		for (int i = 0; i < MAX_VOICES; i++) {
			if (voices[i].active) {
				sum += voices[i].generate(args.sampleTime);
			}
		}
		
		// Normalize by number of active voices
		int activeCount = 0;
		for (int i = 0; i < MAX_VOICES; i++) {
			if (voices[i].active) activeCount++;
		}
		if (activeCount > 0) {
			sum /= (float)activeCount;
		}
		
		// Apply envelope
		sum *= envelope.output;
		
		// Apply filter based on pad preset (only if aux input is not used)
		bool useAuxInput = inputs[AUX_INPUT].isConnected() && detectedFreq > 20.f && detectedFreq < 20000.f;
		float cutoffRatio = 0.8f; // Default initialization
		if (!useAuxInput) {
			PadPreset preset = (PadPreset)(int)std::round(params[PAD_PRESET_PARAM].getValue());
			Voice::Waveform waveform = Voice::SINE; // Default initialization
			getPadPreset(preset, waveform, cutoffRatio);
		}
		
		// Set filter cutoff (lower cutoff for softer pads)
		float cutoff = 20000.f * cutoffRatio;
		filter.setCutoffFreq(cutoff / args.sampleRate);
		filter.process(sum);
		sum = filter.lowpass();
		
		// Update lights
		lights[SLOT0_LIGHT].setBrightness(currentSlot == 0 ? 1.f : 0.f);
		lights[SLOT1_LIGHT].setBrightness(currentSlot == 1 ? 1.f : 0.f);
		lights[SLOT2_LIGHT].setBrightness(currentSlot == 2 ? 1.f : 0.f);
		lights[SLOT3_LIGHT].setBrightness(currentSlot == 3 ? 1.f : 0.f);
		
		// Output (scale to 5V range)
		outputs[AUDIO_OUTPUT].setVoltage(sum * 5.f);
	}
};

// Widget
struct ChordPadSynthWidget : ModuleWidget {
	ChordPadSynthWidget(ChordPadSynth* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ChordPadSynth.svg")));
		
		// Screws
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		
		// Top row: Inputs on left, Pad and Octave on right
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.0, 25.0)), module, ChordPadSynth::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.0, 25.0)), module, ChordPadSynth::RESET_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.0, 25.0)), module, ChordPadSynth::AUX_INPUT));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(45.0, 25.0)), module, ChordPadSynth::PAD_PRESET_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(55.0, 25.0)), module, ChordPadSynth::OCTAVE_PARAM));
		
		// Slot 0 (left side, top)
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(15.0, 40.0)), module, ChordPadSynth::SLOT0_PITCH_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(15.0, 55.0)), module, ChordPadSynth::SLOT0_TYPE_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(15.0, 70.0)), module, ChordPadSynth::SLOT0_LIGHT));
		
		// Slot 1 (right side, top)
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(45.96, 40.0)), module, ChordPadSynth::SLOT1_PITCH_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(45.96, 55.0)), module, ChordPadSynth::SLOT1_TYPE_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(45.96, 70.0)), module, ChordPadSynth::SLOT1_LIGHT));
		
		// Slot 2 (left side, bottom)
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(15.0, 80.0)), module, ChordPadSynth::SLOT2_PITCH_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(15.0, 95.0)), module, ChordPadSynth::SLOT2_TYPE_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(15.0, 110.0)), module, ChordPadSynth::SLOT2_LIGHT));
		
		// Slot 3 (right side, bottom)
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(45.96, 80.0)), module, ChordPadSynth::SLOT3_PITCH_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(45.96, 95.0)), module, ChordPadSynth::SLOT3_TYPE_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(45.96, 110.0)), module, ChordPadSynth::SLOT3_LIGHT));
		
		// EG knobs (vertical, center, outside slot borders)
		// Center of panel is at x=30.48
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.48, 55.0)), module, ChordPadSynth::ATTACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.48, 70.0)), module, ChordPadSynth::DECAY_RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.48, 85.0)), module, ChordPadSynth::SUSTAIN_PARAM));
		
		// Audio output
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(30.48, 120.0)), module, ChordPadSynth::AUDIO_OUTPUT));
	}
};

Model* modelChordPadSynth = createModel<ChordPadSynth, ChordPadSynthWidget>("ChordPadSynth");

