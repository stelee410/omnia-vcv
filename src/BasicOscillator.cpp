#include "plugin.hpp"

struct BasicOscillator : Module {
	enum ParamId {
		FREQ_PARAM,
		BEAT_FREQ_PARAM,
		WAVEFORM_SINE_BUTTON_PARAM,
		WAVEFORM_SQUARE_BUTTON_PARAM,
		WAVEFORM_TRI_BUTTON_PARAM,
		WAVEFORM_SAW_BUTTON_PARAM,
		HARMONIC_COUNT_PARAM,
		HARMONIC_STRENGTH_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_SYNC_INPUT,
		FM_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		WAVEFORM_SINE_LIGHT,
		WAVEFORM_SQUARE_LIGHT,
		WAVEFORM_TRI_LIGHT,
		WAVEFORM_SAW_LIGHT,
		LIGHTS_LEN
	};

	enum WaveformType {
		WAVEFORM_SINE = 0,
		WAVEFORM_SQUARE = 1,
		WAVEFORM_TRI = 2,
		WAVEFORM_SAW = 3
	};

	float phaseL = 0.f;
	float phaseR = 0.f;
	float lastClock = 0.f;

	BasicOscillator() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FREQ_PARAM, -54.f, 54.f, 0.f, "Carrier Frequency", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
		configParam(BEAT_FREQ_PARAM, 0.f, 40.f, 10.f, "Beat Frequency", " Hz");
		configButton(WAVEFORM_SINE_BUTTON_PARAM, "Sine");
		configButton(WAVEFORM_SQUARE_BUTTON_PARAM, "Square");
		configButton(WAVEFORM_TRI_BUTTON_PARAM, "Tri");
		configButton(WAVEFORM_SAW_BUTTON_PARAM, "Saw");
		configParam(HARMONIC_COUNT_PARAM, 0.f, 16.f, 0.f, "Harmonic Count");
		configParam(HARMONIC_STRENGTH_PARAM, 0.f, 1.f, 0.f, "Harmonic Strength");
		configInput(CLOCK_SYNC_INPUT, "Reset");
		configInput(FM_INPUT, "FM");
		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");
		
		// Set default: Sine button pressed
		params[WAVEFORM_SINE_BUTTON_PARAM].setValue(1.f);
	}

	float generateWaveform(WaveformType type, float phase, int harmonicCount, float harmonicStrength) {
		float signal = 0.f;
		
		// Generate base waveform
		switch (type) {
			case WAVEFORM_SINE:
				signal = std::sin(2.f * M_PI * phase);
				break;
			
			case WAVEFORM_SQUARE:
				signal = (phase < 0.5f) ? 1.f : -1.f;
				break;
			
			case WAVEFORM_TRI: {
				// Triangle wave: goes from -1 to 1 linearly, then back to -1
				if (phase < 0.5f) {
					signal = 4.f * phase - 1.f; // -1 to 1
				} else {
					signal = 3.f - 4.f * phase; // 1 to -1
				}
				break;
			}
			
			case WAVEFORM_SAW:
				signal = 2.f * phase - 1.f;
				break;
			
			default:
				signal = std::sin(2.f * M_PI * phase);
		}
		
		// Add harmonics if enabled
		if (harmonicCount > 0 && harmonicStrength > 0.f) {
			float harmonicSum = 0.f;
			float totalAmplitude = 0.f;
			
			// Add harmonics: 2nd, 3rd, 4th, etc.
			for (int h = 1; h <= harmonicCount; h++) {
				float harmonicPhase = phase * (h + 1); // h+1 because h=1 is 2nd harmonic
				harmonicPhase -= std::floor(harmonicPhase); // Wrap to [0, 1)
				
				// Generate harmonic using same waveform type
				float harmonicValue = 0.f;
				switch (type) {
					case WAVEFORM_SINE:
						harmonicValue = std::sin(2.f * M_PI * harmonicPhase);
						break;
					
					case WAVEFORM_SQUARE:
						harmonicValue = (harmonicPhase < 0.5f) ? 1.f : -1.f;
						break;
					
					case WAVEFORM_TRI: {
						if (harmonicPhase < 0.5f) {
							harmonicValue = 4.f * harmonicPhase - 1.f;
						} else {
							harmonicValue = 3.f - 4.f * harmonicPhase;
						}
						break;
					}
					
					case WAVEFORM_SAW:
						harmonicValue = 2.f * harmonicPhase - 1.f;
						break;
					
					default:
						harmonicValue = std::sin(2.f * M_PI * harmonicPhase);
				}
				
				// Amplitude decreases with harmonic number (1/n)
				float amplitude = 1.f / (h + 1);
				harmonicSum += harmonicValue * amplitude;
				totalAmplitude += amplitude;
			}
			
			// Normalize and mix harmonics with base signal
			if (totalAmplitude > 0.f) {
				harmonicSum /= totalAmplitude;
			}
			
			// Mix base signal with harmonics
			signal = signal * (1.f - harmonicStrength) + harmonicSum * harmonicStrength;
		}
		
		return signal;
	}

	void process(const ProcessArgs& args) override {
		// Get the carrier frequency from the knob
		float pitch = params[FREQ_PARAM].getValue();

		// Add FM input if connected (typically 1V/Oct scaling)
		float fm = 0.f;
		if (inputs[FM_INPUT].isConnected()) {
			fm = inputs[FM_INPUT].getVoltage() * 12.f;
		}
		pitch += fm;

		// Convert pitch to carrier frequency
		float carrierFreq = dsp::FREQ_C4 * std::pow(2.f, pitch / 12.f);
		
		// Get beat frequency
		float beatFreq = params[BEAT_FREQ_PARAM].getValue();
		
		// Calculate left and right channel frequencies for binaural beats
		// Left: carrier + beat/2, Right: carrier - beat/2
		float freqL = carrierFreq + beatFreq * 0.5f;
		float freqR = carrierFreq - beatFreq * 0.5f;

		// Clock sync: detect rising edge and reset phase
		if (inputs[CLOCK_SYNC_INPUT].isConnected()) {
			float clock = inputs[CLOCK_SYNC_INPUT].getVoltage();
			if (clock > 1.f && lastClock <= 1.f) {
				// Rising edge detected, reset phases
				phaseL = 0.f;
				phaseR = 0.f;
			}
			lastClock = clock;
		}

		// Accumulate phases for both channels
		phaseL += freqL * args.sampleTime;
		phaseR += freqR * args.sampleTime;
		
		// Wrap phases
		if (phaseL >= 1.f) phaseL -= 1.f;
		if (phaseR >= 1.f) phaseR -= 1.f;
		if (phaseL < 0.f) phaseL += 1.f;
		if (phaseR < 0.f) phaseR += 1.f;

		// Get waveform type from buttons (exclusive selection)
		// Check which button is pressed (latch buttons stay high when pressed)
		WaveformType waveformType = WAVEFORM_SINE; // default
		if (params[WAVEFORM_SAW_BUTTON_PARAM].getValue() > 0.5f) {
			waveformType = WAVEFORM_SAW;
		} else if (params[WAVEFORM_TRI_BUTTON_PARAM].getValue() > 0.5f) {
			waveformType = WAVEFORM_TRI;
		} else if (params[WAVEFORM_SQUARE_BUTTON_PARAM].getValue() > 0.5f) {
			waveformType = WAVEFORM_SQUARE;
		} else {
			waveformType = WAVEFORM_SINE; // default
		}

		// Get harmonic parameters
		int harmonicCount = (int)std::round(params[HARMONIC_COUNT_PARAM].getValue());
		float harmonicStrength = params[HARMONIC_STRENGTH_PARAM].getValue();

		// Generate waveforms for both channels
		float leftSignal = generateWaveform(waveformType, phaseL, harmonicCount, harmonicStrength);
		float rightSignal = generateWaveform(waveformType, phaseR, harmonicCount, harmonicStrength);

		// Update lights based on selected waveform button
		lights[WAVEFORM_SINE_LIGHT].setBrightness(params[WAVEFORM_SINE_BUTTON_PARAM].getValue() > 0.5f ? 1.f : 0.f);
		lights[WAVEFORM_SQUARE_LIGHT].setBrightness(params[WAVEFORM_SQUARE_BUTTON_PARAM].getValue() > 0.5f ? 1.f : 0.f);
		lights[WAVEFORM_TRI_LIGHT].setBrightness(params[WAVEFORM_TRI_BUTTON_PARAM].getValue() > 0.5f ? 1.f : 0.f);
		lights[WAVEFORM_SAW_LIGHT].setBrightness(params[WAVEFORM_SAW_BUTTON_PARAM].getValue() > 0.5f ? 1.f : 0.f);

		// Output 5V signals (bipolar -5V to +5V)
		outputs[LEFT_OUTPUT].setVoltage(5.f * leftSignal);
		outputs[RIGHT_OUTPUT].setVoltage(5.f * rightSignal);
	}
};

// Custom button widget for exclusive waveform selection
// Defined after BasicOscillator so it can access the enum values
struct WaveformButton : app::SvgSwitch {
	WaveformButton() {
		momentary = false;
		latch = true;
		shadow->opacity = 0.0;
		// Use TL1105 button (smaller than VCVButton)
		addFrame(Svg::load(asset::system("res/ComponentLibrary/TL1105_0.svg")));
		addFrame(Svg::load(asset::system("res/ComponentLibrary/TL1105_1.svg")));
	}
	
	void onChange(const ChangeEvent& e) override {
		app::SvgSwitch::onChange(e);
		engine::ParamQuantity* pq = getParamQuantity();
		if (!pq || !pq->module) return;
		
		BasicOscillator* module = dynamic_cast<BasicOscillator*>(pq->module);
		if (!module) return;
		
		// If this button was pressed (value > 0.5), turn off other buttons
		if (pq->getValue() > 0.5f) {
			for (int i = BasicOscillator::WAVEFORM_SINE_BUTTON_PARAM; i <= BasicOscillator::WAVEFORM_SAW_BUTTON_PARAM; i++) {
				if (i != paramId && module->params[i].getValue() > 0.5f) {
					module->params[i].setValue(0.f);
				}
			}
		}
	}
};

struct BasicOscillatorWidget : ModuleWidget {
	BasicOscillatorWidget(BasicOscillator* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/BasicOscillator.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Waveform buttons - arranged horizontally below "PureFreq" text (y=12mm, buttons at y=20mm)
		// Center them around x=15.24mm with spacing of 7mm between button centers for better visibility
		float buttonY = 20.0f;
		float buttonSpacing = 7.0f; // spacing between button centers (increased from 5mm)
		float startX = 15.24f - (buttonSpacing * 1.5f); // center 4 buttons with 7mm spacing
		
		// Sine button with light (light is 2 pixels above button)
		addParam(createParamCentered<WaveformButton>(mm2px(Vec(startX, buttonY)), module, BasicOscillator::WAVEFORM_SINE_BUTTON_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(startX, buttonY - 0.68f)), module, BasicOscillator::WAVEFORM_SINE_LIGHT));
		
		// Square button with light
		addParam(createParamCentered<WaveformButton>(mm2px(Vec(startX + buttonSpacing, buttonY)), module, BasicOscillator::WAVEFORM_SQUARE_BUTTON_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(startX + buttonSpacing, buttonY - 0.68f)), module, BasicOscillator::WAVEFORM_SQUARE_LIGHT));
		
		// Tri button with light
		addParam(createParamCentered<WaveformButton>(mm2px(Vec(startX + buttonSpacing * 2.0f, buttonY)), module, BasicOscillator::WAVEFORM_TRI_BUTTON_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(startX + buttonSpacing * 2.0f, buttonY - 0.68f)), module, BasicOscillator::WAVEFORM_TRI_LIGHT));
		
		// Saw button with light
		addParam(createParamCentered<WaveformButton>(mm2px(Vec(startX + buttonSpacing * 3.0f, buttonY)), module, BasicOscillator::WAVEFORM_SAW_BUTTON_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(startX + buttonSpacing * 3.0f, buttonY - 0.68f)), module, BasicOscillator::WAVEFORM_SAW_LIGHT));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(15.24, 38.0)), module, BasicOscillator::FREQ_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(15.24, 54.0)), module, BasicOscillator::BEAT_FREQ_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(15.24, 70.0)), module, BasicOscillator::HARMONIC_COUNT_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(15.24, 86.0)), module, BasicOscillator::HARMONIC_STRENGTH_PARAM));

		// Inputs: Clock Sync and FM, arranged horizontally (top row)
		// Positioned with good spacing, similar to reference design
		float inputY = 100.0f;
		float portHorizontalSpacing = 12.0f; // spacing between port centers
		float centerX = 15.24f; // panel center
		
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(centerX - portHorizontalSpacing / 2.0f, inputY)), module, BasicOscillator::CLOCK_SYNC_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(centerX + portHorizontalSpacing / 2.0f, inputY)), module, BasicOscillator::FM_INPUT));

		// Outputs: Left and Right, arranged horizontally (bottom row)
		// Positioned with matching spacing for visual consistency
		float outputY = 115.0f;
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(centerX - portHorizontalSpacing / 2.0f, outputY)), module, BasicOscillator::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(centerX + portHorizontalSpacing / 2.0f, outputY)), module, BasicOscillator::RIGHT_OUTPUT));
	}
};

Model* modelBasicOscillator = createModel<BasicOscillator, BasicOscillatorWidget>("BasicOscillator");
