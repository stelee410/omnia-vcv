#include "plugin.hpp"
#include <dsp/filter.hpp>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>

// ADSR Envelope
struct ADSR {
	float attack = 0.001f;  // Pluck: fast attack
	float decay = 0.1f;
	float sustain = 0.0f;   // Pluck: no sustain
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
		if (on) {
			// For pluck sound, always retrigger attack
			state = ATTACK;
			output = 0.f; // Reset output to start attack from beginning
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
		SQUARE,
		PIANO,
		HARP,
		ORGAN
	};
	
	Waveform waveform = SINE;
	
	// Pluck filter for piano/harp/organ
	dsp::RCFilter pluckFilter;
	
	// Smooth frequency transition to avoid clicks
	static constexpr float FREQ_SMOOTH = 0.01f;
	
	void setTargetFrequency(float target) {
		targetFrequency = target;
	}
	
	float generate(float sampleTime) {
		if (!active) return 0.f;
		
		// Smoothly transition to target frequency
		if (std::abs(frequency - targetFrequency) > 0.1f) {
			frequency += (targetFrequency - frequency) * FREQ_SMOOTH;
		} else {
			frequency = targetFrequency;
		}
		
		// Keep phase continuous
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
			case PIANO:
				// Piano: square wave with harmonics and pluck filter
				signal = phase < 0.5f ? 1.f : -1.f;
				// Add harmonics
				signal += 0.5f * std::sin(2.f * M_PI * phase * 2.f);
				signal += 0.25f * std::sin(2.f * M_PI * phase * 3.f);
				signal /= 1.75f; // Normalize
				// Apply pluck filter (high frequency rolloff)
				pluckFilter.setCutoffFreq(0.3f);
				pluckFilter.process(signal);
				signal = pluckFilter.lowpass();
				break;
			case HARP:
				// Harp: bright sine with harmonics
				signal = std::sin(2.f * M_PI * phase);
				signal += 0.3f * std::sin(2.f * M_PI * phase * 2.f);
				signal += 0.15f * std::sin(2.f * M_PI * phase * 3.f);
				signal /= 1.45f;
				pluckFilter.setCutoffFreq(0.5f);
				pluckFilter.process(signal);
				signal = pluckFilter.lowpass();
				break;
			case ORGAN:
				// Organ: multiple sine waves
				signal = std::sin(2.f * M_PI * phase);
				signal += 0.5f * std::sin(2.f * M_PI * phase * 2.f);
				signal += 0.33f * std::sin(2.f * M_PI * phase * 3.f);
				signal /= 1.83f;
				pluckFilter.setCutoffFreq(0.4f);
				pluckFilter.process(signal);
				signal = pluckFilter.lowpass();
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

// Pluck preset types
enum PluckPreset {
	PLUCK_PIANO,   // Piano
	PLUCK_HARP,    // Harp
	PLUCK_ORGAN,   // Organ
	PLUCK_SINE,    // Pure sine
	PLUCK_SQUARE,  // Square
	PLUCK_SAW,     // Saw
	PLUCK_TRIANGLE // Triangle
};

// Arpeggiator types
enum ArpType {
	ARP_UP,      // Up
	ARP_DOWN,    // Down
	ARP_RANDOM   // Random
};

// Arpeggiator range
enum ArpRange {
	ARP_1OCT,    // 1 octave
	ARP_2OCT,    // 2 octaves
	ARP_3OCT     // 3 octaves
};


struct ChordPluckSynth : Module {
	enum ParamId {
		PLUCK_PRESET_PARAM,
		OCTAVE_PARAM,
		STEP_RATE_PARAM,  // Step rate: 1/1, 1/2, 1/4, 1/8, 1/16, 1/32
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
		// ARP parameters
		ARP_RANGE_PARAM,
		ARP_TYPE_PARAM,
		ARP_VOICES_PARAM,
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
		NOTE_OUTPUT,
		OUTPUTS_LEN
	};
	
	enum LightId {
		SLOT0_LIGHT,
		SLOT1_LIGHT,
		SLOT2_LIGHT,
		SLOT3_LIGHT,
		LIGHTS_LEN
	};
	
	static constexpr int MAX_VOICES = 7;
	Voice voices[MAX_VOICES];
	ADSR envelope;
	
	// Current playing slot
	int currentSlot = 0;
	float lastClock = 0.f;
	float lastReset = 0.f;
	
	// Arpeggiator state
	int arpStep = 0;
	int arpNoteIndex = 0;
	int clockEdgeCount = 0; // Count clock edges for arpeggiator timing
	std::vector<float> arpNotes; // Current arpeggio notes
	
	// Step rate timing
	float stepRateTimer = 0.f; // Timer for current clock pulse
	float stepRateInterval = 0.f; // Time interval between arp steps within a clock pulse
	int stepRateStepsRemaining = 0; // Number of arp steps remaining in current clock pulse
	bool stepRateActive = false; // Whether we're in an active step rate cycle
	float lastClockRiseTime = 0.f; // Time of last clock rising edge
	float estimatedClockPeriod = 0.1f; // Estimated clock period (default 100ms)
	
	// Frequency detection for aux input
	float detectedFreq = 0.f;
	float lastAuxSample = 0.f;
	int zeroCrossCount = 0;
	int zeroCrossSamples = 0;
	static constexpr int MIN_ZERO_CROSS_SAMPLES = 100;
	
	// Random number generator for random arp
	std::mt19937 rng;
	
	// Current note output (for CV output)
	float currentNoteCV = 0.f;
	
	ChordPluckSynth() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		
		// Pluck preset (0-6)
		configSwitch(PLUCK_PRESET_PARAM, 0.f, 6.f, 0.f, "Pluck Preset", 
			{"Piano", "Harp", "Organ", "Sine", "Square", "Saw", "Triangle"});
		
		// Octave shift (-2 to +2 octaves)
		configParam(OCTAVE_PARAM, -2.f, 2.f, 0.f, "Octave", " oct");
		
		// Step rate: how many arp cycles per clock step (0-5: 1/1, 1/2, 1/4, 1/8, 1/16, 1/32)
		configSwitch(STEP_RATE_PARAM, 0.f, 5.f, 0.f, "Step Rate",
			{"1/1", "1/2", "1/4", "1/8", "1/16", "1/32"});
		
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
		
		// ARP parameters
		configSwitch(ARP_RANGE_PARAM, 0.f, 2.f, 0.f, "ARP Range", 
			{"1 Oct", "2 Oct", "3 Oct"});
		configSwitch(ARP_TYPE_PARAM, 0.f, 2.f, 0.f, "ARP Type", 
			{"Up", "Down", "Random"});
		configParam(ARP_VOICES_PARAM, 1.f, 7.f, 3.f, "ARP Voices");
		
		// EG (Pluck defaults: fast attack, no sustain)
		configParam(ATTACK_PARAM, 0.001f, 0.1f, 0.001f, "Attack", " s");
		configParam(DECAY_RELEASE_PARAM, 0.01f, 1.f, 0.1f, "Decay/Release", " s");
		configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.f, "Sustain");
		
		// Inputs
		configInput(CLOCK_INPUT, "Clock");
		configInput(AUX_INPUT, "Aux In");
		
		// Outputs
		configOutput(AUDIO_OUTPUT, "Audio");
		configOutput(NOTE_OUTPUT, "Note CV");
		
		// Initialize voices
		for (int i = 0; i < MAX_VOICES; i++) {
			voices[i].waveform = Voice::SINE;
		}
		
		// Initialize random number generator
		rng.seed(std::random_device{}());
		
		// Initialize first slot's arpeggio notes
		generateArpNotes(0);
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
	
	// Get pluck preset waveform
	void getPluckPreset(PluckPreset preset, Voice::Waveform& waveform) {
		switch (preset) {
			case PLUCK_PIANO:
				waveform = Voice::PIANO;
				break;
			case PLUCK_HARP:
				waveform = Voice::HARP;
				break;
			case PLUCK_ORGAN:
				waveform = Voice::ORGAN;
				break;
			case PLUCK_SINE:
				waveform = Voice::SINE;
				break;
			case PLUCK_SQUARE:
				waveform = Voice::SQUARE;
				break;
			case PLUCK_SAW:
				waveform = Voice::SAW;
				break;
			case PLUCK_TRIANGLE:
				waveform = Voice::TRIANGLE;
				break;
		}
	}
	
	// Generate arpeggio notes from chord
	void generateArpNotes(int slot) {
		arpNotes.clear();
		
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
			float octaveShift = params[OCTAVE_PARAM].getValue();
			rootFreq = detectedFreq * std::pow(2.f, octaveShift);
		} else {
			rootFreq = dsp::FREQ_C4 * std::pow(2.f, (rootNote - 0.f) / 12.f);
			float octaveShift = params[OCTAVE_PARAM].getValue();
			rootFreq *= std::pow(2.f, octaveShift);
		}
		
		// Generate notes across range
		ArpRange range = (ArpRange)(int)std::round(params[ARP_RANGE_PARAM].getValue());
		int octaves = range + 1; // 1, 2, or 3 octaves
		
		// Build note list
		std::vector<float> allNotes;
		for (int oct = 0; oct < octaves; oct++) {
			for (float interval : intervals) {
				float semitones = interval + oct * 12.f;
				float freq = rootFreq * std::pow(2.f, semitones / 12.f);
				allNotes.push_back(freq);
			}
		}
		
		// Sort and order based on ARP type
		ArpType arpType = (ArpType)(int)std::round(params[ARP_TYPE_PARAM].getValue());
		int numVoices = (int)std::round(params[ARP_VOICES_PARAM].getValue());
		
		if (arpType == ARP_UP) {
			// Up: ascending order
			std::sort(allNotes.begin(), allNotes.end());
		} else if (arpType == ARP_DOWN) {
			// Down: descending order
			std::sort(allNotes.begin(), allNotes.end(), std::greater<float>());
		} else {
			// Random: shuffle
			std::shuffle(allNotes.begin(), allNotes.end(), rng);
		}
		
		// Limit to requested number of voices
		if (numVoices < (int)allNotes.size()) {
			allNotes.resize(numVoices);
		}
		
		arpNotes = allNotes;
		arpNoteIndex = 0;
	}
	
	// Trigger arpeggio step
	void triggerArpStep(float sampleTime) {
		if (arpNotes.empty()) {
			generateArpNotes(currentSlot);
		}
		
		if (arpNotes.empty()) return;
		
		// Get current note
		float noteFreq = arpNotes[arpNoteIndex];
		
		// Convert frequency to MIDI note (for CV output)
		// MIDI note = 69 + 12 * log2(freq / 440)
		float midiNote = 69.f + 12.f * std::log2(noteFreq / 440.f);
		currentNoteCV = (midiNote - 60.f) / 12.f; // Convert to 1V/Oct (C4 = 0V)
		
		// Get pluck preset
		Voice::Waveform waveform = Voice::SINE;
		if (!inputs[AUX_INPUT].isConnected() || detectedFreq <= 20.f || detectedFreq >= 20000.f) {
			PluckPreset preset = (PluckPreset)(int)std::round(params[PLUCK_PRESET_PARAM].getValue());
			getPluckPreset(preset, waveform);
		} else {
			waveform = Voice::SINE; // Use sine when aux input is connected
		}
		
		// Set up single voice for pluck
		// Set frequency directly for immediate response (no smoothing delay for pluck)
		voices[0].frequency = noteFreq;
		voices[0].targetFrequency = noteFreq;
		voices[0].waveform = waveform;
		voices[0].active = true;
		
		// Deactivate other voices
		for (int i = 1; i < MAX_VOICES; i++) {
			voices[i].setTargetFrequency(0.f);
			voices[i].active = false;
		}
		
		// Trigger envelope - always retrigger for pluck sound
		envelope.gate(true, sampleTime);
		
		// Advance to next note
		arpNoteIndex = (arpNoteIndex + 1) % arpNotes.size();
	}
	
	// Trigger a chord slot (generates arpeggio notes)
	void triggerSlot(int slot) {
		currentSlot = slot;
		generateArpNotes(slot);
		arpNoteIndex = 0;
		arpStep = 0;
		clockEdgeCount = 0;
	}
	
	void process(const ProcessArgs& args) override {
		// Update ADSR parameters
		envelope.setAttack(params[ATTACK_PARAM].getValue());
		float decayRelease = params[DECAY_RELEASE_PARAM].getValue();
		envelope.setDecay(decayRelease);
		envelope.setRelease(decayRelease);
		envelope.setSustain(params[SUSTAIN_PARAM].getValue());
		
		// Detect frequency from aux input
		if (inputs[AUX_INPUT].isConnected()) {
			float auxSample = inputs[AUX_INPUT].getVoltage() / 5.f;
			
			if ((lastAuxSample <= 0.f && auxSample > 0.f) || (lastAuxSample >= 0.f && auxSample < 0.f)) {
				zeroCrossCount++;
				if (zeroCrossCount >= 2) {
					if (zeroCrossSamples > 0) {
						float period = (float)zeroCrossSamples * args.sampleTime;
						if (period > 0.f) {
							float freq = 1.f / period;
							if (freq > 20.f && freq < 20000.f) {
								detectedFreq = detectedFreq * 0.9f + freq * 0.1f;
							}
						}
					}
					zeroCrossSamples = 0;
				} else {
					zeroCrossSamples = 0;
				}
			}
			zeroCrossSamples++;
			lastAuxSample = auxSample;
		} else {
			detectedFreq = 0.f;
			zeroCrossCount = 0;
			zeroCrossSamples = 0;
		}
		
		// Handle reset input
		if (inputs[RESET_INPUT].isConnected()) {
			float reset = inputs[RESET_INPUT].getVoltage();
			bool resetRising = reset > 1.f && lastReset <= 1.f;
			
			if (resetRising) {
				// Reset arpeggiator state
				currentSlot = 0;
				arpNoteIndex = 0;
				clockEdgeCount = 0;
				stepRateActive = false;
				stepRateTimer = 0.f;
				stepRateStepsRemaining = 0;
				generateArpNotes(currentSlot);
			}
			
			lastReset = reset;
		}
		
		// Detect clock and handle arpeggiator
		if (inputs[CLOCK_INPUT].isConnected()) {
			float clock = inputs[CLOCK_INPUT].getVoltage();
			
			// Detect rising edge
			bool clockRising = clock > 1.f && lastClock <= 1.f;
			
			if (clockRising) {
				// Ensure we have arp notes for current slot
				if (arpNotes.empty()) {
					generateArpNotes(currentSlot);
				}
				
				// Check if we've completed all arp steps for current slot
				// Before triggering, check if we're about to play the first note again
				// This means we've completed a full cycle
				if (!arpNotes.empty() && arpNoteIndex == 0 && clockEdgeCount > 0) {
					// Move to next slot
					currentSlot = (currentSlot + 1) % 4;
					generateArpNotes(currentSlot);
					arpNoteIndex = 0;
				}
				
				// Estimate clock period from last clock pulse
				float currentTime = args.frame / args.sampleRate;
				if (lastClockRiseTime > 0.f) {
					float period = currentTime - lastClockRiseTime;
					if (period > 0.001f && period < 10.f) { // Sanity check: 1ms to 10s
						estimatedClockPeriod = period;
					}
				}
				lastClockRiseTime = currentTime;
				
				// Get step rate multiplier (1, 2, 4, 8, 16, 32)
				int stepRateIndex = (int)std::round(params[STEP_RATE_PARAM].getValue());
				int stepRateMultiplier = 1 << stepRateIndex; // 2^stepRateIndex: 1, 2, 4, 8, 16, 32
				
				// Calculate interval between arp steps within this clock pulse
				stepRateInterval = estimatedClockPeriod / (float)stepRateMultiplier;
				stepRateStepsRemaining = stepRateMultiplier;
				stepRateTimer = 0.f;
				stepRateActive = true;
				
				// Trigger first arp step immediately
				triggerArpStep(args.sampleTime);
				stepRateStepsRemaining--;
				
				// Increment clock edge counter (counts input clock pulses)
				clockEdgeCount++;
			}
			
			// Handle step rate timing: trigger additional arp steps during clock pulse
			if (stepRateActive && stepRateStepsRemaining > 0) {
				stepRateTimer += args.sampleTime;
				
				// Check if it's time for next arp step
				if (stepRateTimer >= stepRateInterval) {
					stepRateTimer -= stepRateInterval;
					triggerArpStep(args.sampleTime);
					stepRateStepsRemaining--;
					
					// If we've completed all steps, deactivate
					if (stepRateStepsRemaining <= 0) {
						stepRateActive = false;
						stepRateTimer = 0.f;
					}
				}
			}
			
			lastClock = clock;
		} else {
			// No clock input: continuously play current slot's arpeggio
			// Regenerate notes if slot changed or parameters changed
			static int lastSlot = -1;
			if (currentSlot != lastSlot) {
				generateArpNotes(currentSlot);
				lastSlot = currentSlot;
				arpNoteIndex = 0;
			}
			
			// Auto-advance arpeggio based on sample rate
			static float arpTimer = 0.f;
			float rateTime = 0.1f; // Default 100ms per step
			
			arpTimer += args.sampleTime;
			if (arpTimer >= rateTime) {
				triggerArpStep(args.sampleTime);
				arpTimer = 0.f;
			}
			
			lastClock = 0.f;
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
		
		// Normalize
		int activeCount = 0;
		for (int i = 0; i < MAX_VOICES; i++) {
			if (voices[i].active) activeCount++;
		}
		if (activeCount > 0) {
			sum /= (float)activeCount;
		}
		
		// Apply envelope
		sum *= envelope.output;
		
		// Update lights
		lights[SLOT0_LIGHT].setBrightness(currentSlot == 0 ? 1.f : 0.f);
		lights[SLOT1_LIGHT].setBrightness(currentSlot == 1 ? 1.f : 0.f);
		lights[SLOT2_LIGHT].setBrightness(currentSlot == 2 ? 1.f : 0.f);
		lights[SLOT3_LIGHT].setBrightness(currentSlot == 3 ? 1.f : 0.f);
		
		// Output audio (scale to 5V range)
		outputs[AUDIO_OUTPUT].setVoltage(sum * 5.f);
		
		// Output note CV (1V/Oct)
		outputs[NOTE_OUTPUT].setVoltage(currentNoteCV);
	}
};

// Widget
struct ChordPluckSynthWidget : ModuleWidget {
	ChordPluckSynthWidget(ChordPluckSynth* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ChordPluckSynth.svg")));
		
		// Knob spacing configuration
		const float KNOB_SPACING = 15.0f; // Vertical spacing between knobs in mm
		const float CENTER_X = 30.48f; // X position for center column (ARP + ADSR)
		const float ARP_START_Y = 38.0f; // Starting Y position for ARP knobs
		
		// Screws
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		
		// Top row: Inputs on left, Pluck, Step Rate, and Octave on right
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.0, 25.0)), module, ChordPluckSynth::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.0, 25.0)), module, ChordPluckSynth::RESET_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.0, 25.0)), module, ChordPluckSynth::AUX_INPUT));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(35.0, 25.0)), module, ChordPluckSynth::PLUCK_PRESET_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(45.0, 25.0)), module, ChordPluckSynth::STEP_RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(55.0, 25.0)), module, ChordPluckSynth::OCTAVE_PARAM));
		
		// ARP parameters (vertical, center, aligned with ADSR)
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(CENTER_X, ARP_START_Y)), module, ChordPluckSynth::ARP_RANGE_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(CENTER_X, ARP_START_Y + KNOB_SPACING)), module, ChordPluckSynth::ARP_TYPE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(CENTER_X, ARP_START_Y + 2 * KNOB_SPACING)), module, ChordPluckSynth::ARP_VOICES_PARAM));
		
		// Slot 0 (left side, top) - inside box (x=5-25, y=33-73)
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(12.5, 45.0)), module, ChordPluckSynth::SLOT0_PITCH_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(12.5, 58.0)), module, ChordPluckSynth::SLOT0_TYPE_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(12.5, 68.0)), module, ChordPluckSynth::SLOT0_LIGHT));
		
		// Slot 1 (right side, top) - inside box (x=35.96-55.96, y=33-73)
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(43.46, 45.0)), module, ChordPluckSynth::SLOT1_PITCH_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(43.46, 58.0)), module, ChordPluckSynth::SLOT1_TYPE_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(43.46, 68.0)), module, ChordPluckSynth::SLOT1_LIGHT));
		
		// Slot 2 (left side, bottom) - inside box (x=5-25, y=73-113)
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(12.5, 85.0)), module, ChordPluckSynth::SLOT2_PITCH_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(12.5, 98.0)), module, ChordPluckSynth::SLOT2_TYPE_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(12.5, 108.0)), module, ChordPluckSynth::SLOT2_LIGHT));
		
		// Slot 3 (right side, bottom) - inside box (x=35.96-55.96, y=73-113)
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(43.46, 85.0)), module, ChordPluckSynth::SLOT3_PITCH_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(43.46, 98.0)), module, ChordPluckSynth::SLOT3_TYPE_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(43.46, 108.0)), module, ChordPluckSynth::SLOT3_LIGHT));
		
		// EG knobs (vertical, center, outside slot borders)
		// ADSR starts after ARP knobs (3 knobs * spacing)
		const float ADSR_START_Y = ARP_START_Y + 3 * KNOB_SPACING;
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(CENTER_X, ADSR_START_Y)), module, ChordPluckSynth::ATTACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(CENTER_X, ADSR_START_Y + KNOB_SPACING)), module, ChordPluckSynth::DECAY_RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(CENTER_X, ADSR_START_Y + 2 * KNOB_SPACING)), module, ChordPluckSynth::SUSTAIN_PARAM));
		
		// Audio output
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(20.0, 120.0)), module, ChordPluckSynth::AUDIO_OUTPUT));
		
		// Note CV output
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(40.96, 120.0)), module, ChordPluckSynth::NOTE_OUTPUT));
	}
};

Model* modelChordPluckSynth = createModel<ChordPluckSynth, ChordPluckSynthWidget>("ChordPluckSynth");

