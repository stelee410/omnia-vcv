#include "plugin.hpp"
#include <dsp/filter.hpp>
#include <dsp/midi.hpp>
#include <dsp/ringbuffer.hpp>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>

// ========== Helper Structures ==========

// Simple delay line for effects
struct DelayLine {
	float* buffer;
	size_t size;
	size_t writePos;
	
	DelayLine(size_t maxSize) {
		size = maxSize;
		buffer = new float[size];
		writePos = 0;
		clear();
	}
	
	~DelayLine() {
		delete[] buffer;
	}
	
	void clear() {
		for (size_t i = 0; i < size; i++) {
			buffer[i] = 0.f;
		}
	}
	
	void push(float sample) {
		buffer[writePos] = sample;
		writePos = (writePos + 1) % size;
	}
	
	float read(size_t delay) {
		if (delay >= size) delay = size - 1;
		size_t readPos = (writePos - delay + size) % size;
		return buffer[readPos];
	}
};

// Simple reverb
struct SimpleReverb {
	static constexpr int NUM_DELAYS = 8;
	DelayLine* delays[NUM_DELAYS];
	float delayTimes[NUM_DELAYS];
	float feedback = 0.5f;
	
	SimpleReverb(float sampleRate) {
		int baseDelay = (int)(sampleRate * 0.03f);
		delayTimes[0] = baseDelay * 1.0f;
		delayTimes[1] = baseDelay * 1.3f;
		delayTimes[2] = baseDelay * 1.7f;
		delayTimes[3] = baseDelay * 2.1f;
		delayTimes[4] = baseDelay * 2.3f;
		delayTimes[5] = baseDelay * 2.7f;
		delayTimes[6] = baseDelay * 3.1f;
		delayTimes[7] = baseDelay * 3.7f;
		
		for (int i = 0; i < NUM_DELAYS; i++) {
			delays[i] = new DelayLine((size_t)(delayTimes[i] * 2.0f));
		}
	}
	
	~SimpleReverb() {
		for (int i = 0; i < NUM_DELAYS; i++) {
			delete delays[i];
		}
	}
	
	void setFeedback(float fb) {
		feedback = fb;
	}
	
	float process(float input) {
		float output = 0.f;
		for (int i = 0; i < NUM_DELAYS; i++) {
			float delayed = delays[i]->read((size_t)delayTimes[i]);
			output += delayed * (1.0f / NUM_DELAYS);
			delays[i]->push(input + delayed * feedback);
		}
		return output;
	}
	
	void clear() {
		for (int i = 0; i < NUM_DELAYS; i++) {
			delays[i]->clear();
		}
	}
};

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

// LFO
struct LFO {
	enum Waveform {
		SINE,
		TRIANGLE,
		SQUARE,
		RANDOM
	};
	
	float phase = 0.f;
	float rate = 1.f;
	Waveform waveform = SINE;
	bool tempoSync = false;
	float randomValue = 0.f;
	
	float process(float sampleTime, float tempo = 120.f) {
		float freq = rate;
		if (tempoSync) {
			freq = rate * tempo / 60.f; // rate in beats per minute
		}
		
		phase += freq * sampleTime;
		if (phase >= 1.f) {
			phase -= 1.f;
			if (waveform == RANDOM) {
				randomValue = (float)rand() / RAND_MAX * 2.f - 1.f;
			}
		}
		
		switch (waveform) {
			case SINE:
				return std::sin(2.f * M_PI * phase);
			case TRIANGLE:
				return phase < 0.5f ? 4.f * phase - 1.f : 3.f - 4.f * phase;
			case SQUARE:
				return phase < 0.5f ? 1.f : -1.f;
			case RANDOM:
				return randomValue;
			default:
				return 0.f;
		}
	}
};

// Voice structure for each chord note
struct Voice {
	float phase = 0.f;
	float frequency = 0.f;
	float detune = 0.f;
	float centsOffset = 0.f;
	ADSR envelope;
	bool active = false;
	float pan = 0.f; // -1 to 1 for stereo spread
	
	enum Waveform {
		SINE,
		TRIANGLE,
		SAW,
		SQUARE
	};
	
	Waveform waveform = SINE;
	
	float generate(float sampleTime) {
		if (!active) return 0.f;
		
		// Calculate actual frequency with detune and cents offset
		float actualFreq = frequency * std::pow(2.f, (detune + centsOffset / 100.f) / 12.f);
		
		phase += actualFreq * sampleTime;
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
		
		envelope.process(sampleTime);
		return signal * envelope.output;
	}
};

// ========== Main Module ==========

struct ChordSynth : Module {
	enum ParamId {
		// Chord parameters
		CHORD_PARAM,
		VOICES_PARAM,
		SPREAD_PARAM,
		DETUNE_PARAM,
		TUNE_PARAM,
		
		// Motion
		MOTION_PARAM,
		
		// Filter
		CUTOFF_PARAM,
		RESONANCE_PARAM,
		
		// FX
		FX_MIX_PARAM,
		
		// LFO
		LFO_RATE_PARAM,
		LFO_WAVEFORM_PARAM,
		LFO_TEMPO_SYNC_PARAM,
		
		// ADSR
		ATTACK_PARAM,
		DECAY_PARAM,
		SUSTAIN_PARAM,
		RELEASE_PARAM,
		
		// Oscillator
		WAVEFORM_PARAM,
		
		// Modulation
		MOD_PITCH_PARAM,
		MOD_CUTOFF_PARAM,
		MOD_AMP_PARAM,
		
		PARAMS_LEN
	};
	
	enum InputId {
		PITCH_INPUT,
		GATE_INPUT,
		CV_INPUT,
		CHORD_CV_INPUT,
		LFO_RATE_INPUT,
		MOD_INPUT,
		INPUTS_LEN
	};
	
	enum OutputId {
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		LFO_OUTPUT,
		OUTPUTS_LEN
	};
	
	enum LightId {
		LIVE_LIGHT,
		LIGHTS_LEN
	};
	
	// Chord types
	enum ChordType {
		MAJOR,
		MINOR,
		DIMINISHED,
		AUGMENTED,
		SEVENTH,
		SUSPENDED,
		CUSTOM
	};
	
	// Voicing modes
	enum VoicingMode {
		STACK,
		SPREAD,
		RANDOM
	};
	
	// Tuning systems
	enum TuningSystem {
		TET_12,
		TET_24,
		JUST_INTONATION,
		CUSTOM_CENTS
	};
	
	static constexpr int MAX_VOICES = 8;
	Voice voices[MAX_VOICES];
	
	// Chord generation
	ChordType chordType = MAJOR;
	VoicingMode voicingMode = STACK;
	TuningSystem tuningSystem = TET_12;
	int activeVoiceCount = 3;
	
	// Current root note (MIDI note number)
	float rootNote = 60.f; // C4
	
	// Effects
	dsp::RCFilter filter;
	DelayLine* delayLineL;
	DelayLine* delayLineR;
	SimpleReverb* reverbL;
	SimpleReverb* reverbR;
	
	// Modulation
	LFO lfo;
	
	// State
	bool gateState = false;
	float lastGate = 0.f;
	
	// Motion
	float motionPhase = 0.f;
	float motionAmount = 0.f;
	
	// Custom chord intervals (in semitones)
	float customIntervals[8] = {0.f, 4.f, 7.f, 12.f, 16.f, 19.f, 24.f, 28.f};
	
	ChordSynth() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		
		// Chord parameters
		configSwitch(CHORD_PARAM, 0.f, 6.f, 0.f, "Chord Type", {"Major", "Minor", "Dim", "Aug", "7th", "Sus", "Custom"});
		configSwitch(VOICES_PARAM, 2.f, 8.f, 3.f, "Voice Count", {"2", "3", "4", "5", "6", "7", "8"});
		configParam(SPREAD_PARAM, 0.f, 1.f, 0.5f, "Stereo Spread");
		configParam(DETUNE_PARAM, -0.5f, 0.5f, 0.f, "Detune", " semitones");
		configParam(TUNE_PARAM, -1.f, 1.f, 0.f, "Fine Tune", " semitones");
		
		// Motion
		configParam(MOTION_PARAM, 0.f, 1.f, 0.f, "Motion Amount");
		
		// Filter
		configParam(CUTOFF_PARAM, 20.f, 20000.f, 10000.f, "Filter Cutoff", " Hz");
		configParam(RESONANCE_PARAM, 0.f, 1.f, 0.f, "Resonance");
		
		// FX
		configParam(FX_MIX_PARAM, 0.f, 1.f, 0.3f, "FX Mix");
		
		// LFO
		configParam(LFO_RATE_PARAM, 0.1f, 20.f, 1.f, "LFO Rate", " Hz");
		configParam(LFO_WAVEFORM_PARAM, 0.f, 3.f, 0.f, "LFO Waveform");
		configParam(LFO_TEMPO_SYNC_PARAM, 0.f, 1.f, 0.f, "LFO Tempo Sync");
		
		// ADSR
		configParam(ATTACK_PARAM, 0.001f, 2.f, 0.01f, "Attack", " s");
		configParam(DECAY_PARAM, 0.001f, 2.f, 0.1f, "Decay", " s");
		configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.7f, "Sustain");
		configParam(RELEASE_PARAM, 0.001f, 2.f, 0.2f, "Release", " s");
		
		// Oscillator
		configParam(WAVEFORM_PARAM, 0.f, 3.f, 0.f, "Waveform");
		
		// Modulation
		configParam(MOD_PITCH_PARAM, 0.f, 1.f, 0.f, "LFO -> Pitch");
		configParam(MOD_CUTOFF_PARAM, 0.f, 1.f, 0.f, "LFO -> Cutoff");
		configParam(MOD_AMP_PARAM, 0.f, 1.f, 0.f, "LFO -> Amp");
		
		// Inputs
		configInput(PITCH_INPUT, "Pitch (1V/Oct)");
		configInput(GATE_INPUT, "Gate");
		configInput(CV_INPUT, "CV");
		configInput(CHORD_CV_INPUT, "Chord CV");
		configInput(LFO_RATE_INPUT, "LFO Rate CV");
		configInput(MOD_INPUT, "Modulation");
		
		// Outputs
		configOutput(OUT_L_OUTPUT, "Left");
		configOutput(OUT_R_OUTPUT, "Right");
		configOutput(LFO_OUTPUT, "LFO");
		
		// Initialize delay lines and reverb
		delayLineL = new DelayLine(44100 * 2); // 2 seconds max
		delayLineR = new DelayLine(44100 * 2);
		reverbL = nullptr;
		reverbR = nullptr;
		
		// Initialize voices
		for (int i = 0; i < MAX_VOICES; i++) {
			voices[i].waveform = Voice::SINE;
		}
	}
	
	~ChordSynth() {
		delete delayLineL;
		delete delayLineR;
		if (reverbL) delete reverbL;
		if (reverbR) delete reverbR;
	}
	
	// Generate chord intervals based on type
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
			case CUSTOM:
				// Use custom intervals array
				for (int i = 0; i < activeVoiceCount && i < 8; i++) {
					intervals.push_back(customIntervals[i]);
				}
				break;
		}
		
		return intervals;
	}
	
	// Apply voicing mode to intervals
	void applyVoicing(std::vector<float>& intervals, VoicingMode mode) {
		if (mode == SPREAD) {
			// Spread across octaves
			for (size_t i = 0; i < intervals.size(); i++) {
				if (i > 0 && intervals[i] < intervals[i-1]) {
					intervals[i] += 12.f;
				}
			}
		} else if (mode == RANDOM) {
			// Randomize order (but keep root at 0)
			static std::mt19937 rng;
			std::shuffle(intervals.begin() + 1, intervals.end(), rng);
		}
		// STACK mode: no change
	}
	
	// Convert semitones to frequency with microtonal tuning
	float semitonesToFrequency(float semitones, float rootFreq) {
		switch (tuningSystem) {
			case TET_12:
				return rootFreq * std::pow(2.f, semitones / 12.f);
			case TET_24:
				return rootFreq * std::pow(2.f, semitones / 24.f);
			case JUST_INTONATION: {
				// Just intonation ratios for common intervals
				float ratio = 1.f;
				float octaves = std::floor(semitones / 12.f);
				float remainder = semitones - octaves * 12.f;
				
				// Common just intonation ratios
				if (std::abs(remainder - 0.f) < 0.1f) ratio = 1.f / 1.f;
				else if (std::abs(remainder - 3.86f) < 0.1f) ratio = 6.f / 5.f; // minor third
				else if (std::abs(remainder - 4.f) < 0.1f) ratio = 5.f / 4.f; // major third
				else if (std::abs(remainder - 7.02f) < 0.1f) ratio = 3.f / 2.f; // perfect fifth
				else if (std::abs(remainder - 9.69f) < 0.1f) ratio = 5.f / 3.f; // major sixth
				else ratio = std::pow(2.f, remainder / 12.f); // fallback to 12-TET
				
				return rootFreq * std::pow(2.f, octaves) * ratio;
			}
			case CUSTOM_CENTS:
				// Use cents directly (100 cents = 1 semitone)
				return rootFreq * std::pow(2.f, semitones / 12.f);
			default:
				return rootFreq * std::pow(2.f, semitones / 12.f);
		}
	}
	
	// Update chord notes
	void updateChord(float rootPitch) {
		rootNote = rootPitch;
		float rootFreq = dsp::FREQ_C4 * std::pow(2.f, (rootPitch - 60.f) / 12.f);
		
		// Get chord intervals
		std::vector<float> intervals = getChordIntervals(chordType);
		activeVoiceCount = std::min((int)intervals.size(), (int)params[VOICES_PARAM].getValue());
		
		// Apply voicing
		applyVoicing(intervals, voicingMode);
		
		// Apply motion (slow interval drift)
		motionAmount = params[MOTION_PARAM].getValue();
		if (motionAmount > 0.f) {
			motionPhase += 0.0001f * motionAmount; // Slow drift
			if (motionPhase >= 1.f) motionPhase -= 1.f;
			
			for (size_t i = 0; i < intervals.size(); i++) {
				float drift = std::sin(2.f * M_PI * motionPhase + i * 0.5f) * 0.5f * motionAmount;
				intervals[i] += drift;
			}
		}
		
		// Set voice frequencies
		float spread = params[SPREAD_PARAM].getValue();
		float detune = params[DETUNE_PARAM].getValue();
		float tune = params[TUNE_PARAM].getValue();
		
		for (int i = 0; i < activeVoiceCount; i++) {
			if (i < (int)intervals.size()) {
				float semitones = intervals[i] + tune;
				voices[i].frequency = semitonesToFrequency(semitones, rootFreq);
				voices[i].detune = detune * (float)(i - activeVoiceCount / 2) / activeVoiceCount;
				voices[i].pan = (i % 2 == 0 ? -1.f : 1.f) * spread * (float)i / activeVoiceCount;
			}
			
			// Set waveform
			int waveform = (int)params[WAVEFORM_PARAM].getValue();
			voices[i].waveform = (Voice::Waveform)waveform;
		}
	}
	
	void process(const ProcessArgs& args) override {
		// Initialize reverb if needed
		if (!reverbL) {
			reverbL = new SimpleReverb(args.sampleRate);
			reverbR = new SimpleReverb(args.sampleRate);
		}
		
		// Get pitch input (root note)
		float pitch = 60.f; // Default C4
		if (inputs[PITCH_INPUT].isConnected()) {
			pitch = inputs[PITCH_INPUT].getVoltage() * 12.f + 60.f; // 1V/Oct, C4 = 60
		}
		// CV input can also modulate pitch
		if (inputs[CV_INPUT].isConnected()) {
			pitch += inputs[CV_INPUT].getVoltage() * 12.f;
		}
		
		// Get gate
		float gate = inputs[GATE_INPUT].getVoltage();
		bool gateOn = gate > 1.f;
		
		// Detect gate edge
		if (gateOn && !gateState) {
			// Gate on: trigger all voices
			for (int i = 0; i < activeVoiceCount; i++) {
				voices[i].envelope.gate(true, args.sampleTime);
				voices[i].active = true;
			}
			updateChord(pitch);
		} else if (!gateOn && gateState) {
			// Gate off: release all voices
			for (int i = 0; i < activeVoiceCount; i++) {
				voices[i].envelope.gate(false, args.sampleTime);
			}
		}
		gateState = gateOn;
		
		// Update ADSR parameters
		for (int i = 0; i < MAX_VOICES; i++) {
			voices[i].envelope.setAttack(params[ATTACK_PARAM].getValue());
			voices[i].envelope.setDecay(params[DECAY_PARAM].getValue());
			voices[i].envelope.setSustain(params[SUSTAIN_PARAM].getValue());
			voices[i].envelope.setRelease(params[RELEASE_PARAM].getValue());
		}
		
		// Update chord type (0-6 maps to chord types)
		float chordValue;
		
		// Use CV input if connected, otherwise use knob value
		if (inputs[CHORD_CV_INPUT].isConnected()) {
			// CV input: 0-10V maps to 0-6 chord types
			// 0V = Major (0), ~1.43V = Minor (1), ~2.86V = Dim (2), etc.
			// 10V = Custom (6)
			float cvValue = inputs[CHORD_CV_INPUT].getVoltage();
			// Map 0-10V to 0-6 range
			chordValue = math::clamp(cvValue * 0.6f, 0.f, 6.f);
		} else {
			chordValue = params[CHORD_PARAM].getValue();
		}
		
		int chordParamValue = (int)std::round(chordValue);
		if (chordParamValue < 0) chordParamValue = 0;
		if (chordParamValue > 6) chordParamValue = 6;
		chordType = (ChordType)chordParamValue;
		
		// Update voicing mode (could be a parameter, but for now use STACK)
		voicingMode = STACK; // Could add a parameter for this
		
		// Update tuning system (could be a parameter, but for now use TET_12)
		tuningSystem = TET_12; // Could add a parameter for this
		
		if (gateOn) {
			updateChord(pitch);
		}
		
		// Process LFO
		float lfoRate = params[LFO_RATE_PARAM].getValue();
		if (inputs[LFO_RATE_INPUT].isConnected()) {
			lfoRate += inputs[LFO_RATE_INPUT].getVoltage() * 5.f;
		}
		lfo.rate = lfoRate;
		lfo.waveform = (LFO::Waveform)(int)params[LFO_WAVEFORM_PARAM].getValue();
		lfo.tempoSync = params[LFO_TEMPO_SYNC_PARAM].getValue() > 0.5f;
		float lfoOut = lfo.process(args.sampleTime);
		outputs[LFO_OUTPUT].setVoltage(lfoOut * 5.f);
		
		// Generate voices
		float leftSum = 0.f;
		float rightSum = 0.f;
		
		for (int i = 0; i < activeVoiceCount; i++) {
			float voiceOut = voices[i].generate(args.sampleTime);
			
			// Apply LFO modulation
			float modPitch = params[MOD_PITCH_PARAM].getValue();
			if (modPitch > 0.f) {
				voices[i].centsOffset = lfoOut * modPitch * 50.f; // ±50 cents max
			}
			
			// Panning
			float pan = voices[i].pan;
			leftSum += voiceOut * (1.f - pan) * 0.5f;
			rightSum += voiceOut * (1.f + pan) * 0.5f;
		}
		
			// Apply filter
		float cutoff = params[CUTOFF_PARAM].getValue();
		float resonance = params[RESONANCE_PARAM].getValue();
		float modCutoff = params[MOD_CUTOFF_PARAM].getValue();
		if (modCutoff > 0.f) {
			cutoff *= (1.f + lfoOut * modCutoff);
		}
		if (inputs[MOD_INPUT].isConnected()) {
			cutoff += inputs[MOD_INPUT].getVoltage() * 1000.f;
		}
		cutoff = math::clamp(cutoff, 20.f, 20000.f);
		filter.setCutoffFreq(cutoff / args.sampleRate);
		
		// Process filter (RCFilter::process() is void, need to call lowpass() to get output)
		filter.process(leftSum);
		float filteredL = filter.lowpass();
		filter.process(rightSum);
		float filteredR = filter.lowpass();
		
		// Apply resonance feedback (simple approximation)
		if (resonance > 0.f) {
			filteredL += (leftSum - filteredL) * resonance * 0.5f;
			filteredR += (rightSum - filteredR) * resonance * 0.5f;
		}
		
		leftSum = filteredL;
		rightSum = filteredR;
		
		// Apply effects
		float fxMix = params[FX_MIX_PARAM].getValue();
		
		// Delay
		size_t delaySamples = (size_t)(0.3f * args.sampleRate); // 300ms delay
		float delayedL = delayLineL->read(delaySamples);
		float delayedR = delayLineR->read(delaySamples);
		delayLineL->push(leftSum);
		delayLineR->push(rightSum);
		
		leftSum = leftSum + delayedL * fxMix * 0.3f;
		rightSum = rightSum + delayedR * fxMix * 0.3f;
		
		// Reverb
		float reverbL_out = reverbL->process(leftSum) * fxMix * 0.5f;
		float reverbR_out = reverbR->process(rightSum) * fxMix * 0.5f;
		leftSum = leftSum * (1.f - fxMix * 0.3f) + reverbL_out;
		rightSum = rightSum * (1.f - fxMix * 0.3f) + reverbR_out;
		
		// Apply LFO to amplitude
		float modAmp = params[MOD_AMP_PARAM].getValue();
		if (modAmp > 0.f) {
			float ampMod = 1.f + lfoOut * modAmp * 0.5f;
			leftSum *= ampMod;
			rightSum *= ampMod;
		}
		
		// Output
		outputs[OUT_L_OUTPUT].setVoltage(leftSum * 5.f);
		outputs[OUT_R_OUTPUT].setVoltage(rightSum * 5.f);
		
		// Update light
		lights[LIVE_LIGHT].setBrightness(gateState ? 1.f : 0.f);
	}
};

// Widget
struct ChordSynthWidget : ModuleWidget {
	ChordSynthWidget(ChordSynth* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ChordSynth.svg")));
		
		// Screws
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		
		// Main chord knob
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(30.48, 30.94)), module, ChordSynth::CHORD_PARAM));
		
		// Tuning/Voicing knobs
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(18.5, 53.76)), module, ChordSynth::TUNE_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(18.17, 68.90)), module, ChordSynth::VOICES_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(42.33, 54.42)), module, ChordSynth::SPREAD_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(42.33, 68.90)), module, ChordSynth::DETUNE_PARAM));
		
		// Motion/Filter/FX knobs
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(18.5, 89.71)), module, ChordSynth::MOTION_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.48, 89.71)), module, ChordSynth::CUTOFF_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(42.5, 89.71)), module, ChordSynth::FX_MIX_PARAM));
		
		// Inputs - positions match SVG design
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.470834, 106.925)), module, ChordSynth::PITCH_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(28.0, 106.925)), module, ChordSynth::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.658311, 118.79999)), module, ChordSynth::CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(27.611969, 118.70192)), module, ChordSynth::CHORD_CV_INPUT));
		
		// Outputs - positions match SVG design
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(47.766663, 104.53336)), module, ChordSynth::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(47.766663, 116.12086)), module, ChordSynth::OUT_R_OUTPUT));
		
		// Light
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(9.2, 19.5)), module, ChordSynth::LIVE_LIGHT));
	}
};

Model* modelChordSynth = createModel<ChordSynth, ChordSynthWidget>("ChordSynth");

