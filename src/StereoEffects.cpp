#include "plugin.hpp"
#include <dsp/ringbuffer.hpp>

// Simple delay line implementation
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

// Simple reverb using multiple delay lines
struct SimpleReverb {
	static constexpr int NUM_DELAYS = 8;
	DelayLine* delays[NUM_DELAYS];
	float delayTimes[NUM_DELAYS];
	float feedback = 0.5f;
	
	SimpleReverb(float sampleRate) {
		// Create delay lines with different lengths (prime numbers for better diffusion)
		int baseDelay = (int)(sampleRate * 0.03f); // 30ms base
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

struct StereoEffects : Module {
	enum ParamId {
		LEVEL_PARAM,
		DELAY_ENABLE_PARAM,
		DELAY_TIME_PARAM,
		DELAY_FEEDBACK_PARAM,
		REVERB_ENABLE_PARAM,
		REVERB_SIZE_PARAM,
		REVERB_DAMPING_PARAM,
		ECHO_ENABLE_PARAM,
		ECHO_TIME_PARAM,
		ECHO_FEEDBACK_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		LEFT_INPUT,
		RIGHT_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		DELAY_LIGHT,
		REVERB_LIGHT,
		ECHO_LIGHT,
		LIGHTS_LEN
	};

	// Delay effect (stereo processing)
	DelayLine* delayLineL;
	DelayLine* delayLineR;
	float delayFeedback = 0.0f;
	
	// Reverb effect (stereo processing)
	SimpleReverb* reverbL;
	SimpleReverb* reverbR;
	
	// Echo effect (stereo processing)
	DelayLine* echoLineL;
	DelayLine* echoLineR;
	float echoFeedback = 0.0f;
	
	float sampleRate = 44100.f;

	StereoEffects() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Level");
		configButton(DELAY_ENABLE_PARAM, "Delay Enable");
		configParam(DELAY_TIME_PARAM, 0.001f, 1.f, 0.3f, "Delay Time", " s");
		configParam(DELAY_FEEDBACK_PARAM, 0.f, 0.95f, 0.3f, "Delay Feedback");
		configButton(REVERB_ENABLE_PARAM, "Reverb Enable");
		configParam(REVERB_SIZE_PARAM, 0.1f, 1.f, 0.5f, "Reverb Size");
		configParam(REVERB_DAMPING_PARAM, 0.f, 1.f, 0.5f, "Reverb Damping");
		configButton(ECHO_ENABLE_PARAM, "Echo Enable");
		configParam(ECHO_TIME_PARAM, 0.01f, 0.5f, 0.2f, "Echo Time", " s");
		configParam(ECHO_FEEDBACK_PARAM, 0.f, 0.9f, 0.4f, "Echo Feedback");
		
		configInput(LEFT_INPUT, "Left");
		configInput(RIGHT_INPUT, "Right");
		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");
		
		// Initialize delay lines (max 1 second at 48kHz = 48000 samples)
		delayLineL = new DelayLine(48000);
		delayLineR = new DelayLine(48000);
		echoLineL = new DelayLine(24000); // Max 0.5 second
		echoLineR = new DelayLine(24000);
		
		// Reverb will be initialized in process() when we know the sample rate
		reverbL = nullptr;
		reverbR = nullptr;
	}
	
	~StereoEffects() {
		delete delayLineL;
		delete delayLineR;
		delete echoLineL;
		delete echoLineR;
		if (reverbL) delete reverbL;
		if (reverbR) delete reverbR;
	}

	void process(const ProcessArgs& args) override {
		// Initialize reverb if needed
		if (!reverbL) {
			reverbL = new SimpleReverb(args.sampleRate);
			reverbR = new SimpleReverb(args.sampleRate);
		}
		
		// Get input signals
		bool leftConnected = inputs[LEFT_INPUT].isConnected();
		bool rightConnected = inputs[RIGHT_INPUT].isConnected();
		
		float inL = leftConnected ? inputs[LEFT_INPUT].getVoltage() / 10.f : 0.f;
		float inR = rightConnected ? inputs[RIGHT_INPUT].getVoltage() / 10.f : 0.f;
		
		// If only left is connected (mono input), copy to right channel
		if (leftConnected && !rightConnected) {
			inR = inL;
		}
		// If only right is connected, copy to left channel
		if (rightConnected && !leftConnected) {
			inL = inR;
		}
		
		// Process effects on both channels simultaneously
		float outL = inL;
		float outR = inR;
		
		// Delay effect - applied to both channels
		if (params[DELAY_ENABLE_PARAM].getValue() > 0.5f) {
			float delayTime = params[DELAY_TIME_PARAM].getValue();
			float feedback = params[DELAY_FEEDBACK_PARAM].getValue();
			size_t delaySamples = (size_t)(delayTime * args.sampleRate);
			
			float delayedL = delayLineL->read(delaySamples);
			float delayedR = delayLineR->read(delaySamples);
			
			outL += delayedL;
			outR += delayedR;
			
			delayLineL->push(inL + delayedL * feedback);
			delayLineR->push(inR + delayedR * feedback);
			
			lights[DELAY_LIGHT].setBrightness(1.f);
		} else {
			lights[DELAY_LIGHT].setBrightness(0.f);
		}
		
		// Reverb effect - applied to both channels
		if (params[REVERB_ENABLE_PARAM].getValue() > 0.5f) {
			float size = params[REVERB_SIZE_PARAM].getValue();
			float damping = params[REVERB_DAMPING_PARAM].getValue();
			reverbL->setFeedback(damping * 0.7f);
			reverbR->setFeedback(damping * 0.7f);
			
			float reverbL_out = reverbL->process(outL) * size;
			float reverbR_out = reverbR->process(outR) * size;
			
			outL = outL * (1.f - size * 0.5f) + reverbL_out;
			outR = outR * (1.f - size * 0.5f) + reverbR_out;
			
			lights[REVERB_LIGHT].setBrightness(1.f);
		} else {
			lights[REVERB_LIGHT].setBrightness(0.f);
		}
		
		// Echo effect - applied to both channels
		if (params[ECHO_ENABLE_PARAM].getValue() > 0.5f) {
			float echoTime = params[ECHO_TIME_PARAM].getValue();
			float feedback = params[ECHO_FEEDBACK_PARAM].getValue();
			size_t echoSamples = (size_t)(echoTime * args.sampleRate);
			
			float echoL = echoLineL->read(echoSamples);
			float echoR = echoLineR->read(echoSamples);
			
			outL += echoL * 0.5f;
			outR += echoR * 0.5f;
			
			echoLineL->push(outL + echoL * feedback);
			echoLineR->push(outR + echoR * feedback);
			
			lights[ECHO_LIGHT].setBrightness(1.f);
		} else {
			lights[ECHO_LIGHT].setBrightness(0.f);
		}
		
		// Apply level
		float level = params[LEVEL_PARAM].getValue();
		outL *= level;
		outR *= level;
		
		// Output (convert back to ±10V)
		outputs[LEFT_OUTPUT].setVoltage(outL * 10.f);
		outputs[RIGHT_OUTPUT].setVoltage(outR * 10.f);
	}
};

// Button widget for effect enable/disable
struct EffectButton : app::SvgSwitch {
	EffectButton() {
		momentary = false;
		latch = true;
		shadow->opacity = 0.0;
		addFrame(Svg::load(asset::system("res/ComponentLibrary/TL1105_0.svg")));
		addFrame(Svg::load(asset::system("res/ComponentLibrary/TL1105_1.svg")));
	}
};

struct StereoEffectsWidget : ModuleWidget {
	// Layout parameters - adjust these to change spacing
	// Vertical (Y-axis) spacing parameters
	static constexpr float BUTTON_Y = 50.0f;                    // Y position of enable buttons
	static constexpr float BUTTON_TO_KNOB_SPACING = 12.0f;      // Vertical spacing from button to first knob (mm)
	static constexpr float KNOB_VERTICAL_SPACING = 16.0f;       // Vertical spacing between knobs in each column (mm)
	
	// Horizontal (X-axis) spacing parameters
	static constexpr float CENTER_COLUMN_X = 22.86f;            // X position of center column (Reverb) (mm) - adjusted for wider panel
	static constexpr float COLUMN_HORIZONTAL_SPACING = 12.0f;   // Horizontal spacing between columns (mm) - adjust this to change spacing between Delay/Reverb/Echo
	
	StereoEffectsWidget(StereoEffects* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/StereoEffects.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Level knob
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(CENTER_COLUMN_X, 25.0)), module, StereoEffects::LEVEL_PARAM));

		// Calculate positions based on spacing parameters
		float firstKnobY = BUTTON_Y + BUTTON_TO_KNOB_SPACING;
		float secondKnobY = firstKnobY + KNOB_VERTICAL_SPACING;
		
		// Calculate column X positions
		float delayX = CENTER_COLUMN_X - COLUMN_HORIZONTAL_SPACING;
		float reverbX = CENTER_COLUMN_X;
		float echoX = CENTER_COLUMN_X + COLUMN_HORIZONTAL_SPACING;

		// Delay section
		addParam(createParamCentered<EffectButton>(mm2px(Vec(delayX, BUTTON_Y)), module, StereoEffects::DELAY_ENABLE_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(delayX, BUTTON_Y - 5.0)), module, StereoEffects::DELAY_LIGHT));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(delayX, firstKnobY)), module, StereoEffects::DELAY_TIME_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(delayX, secondKnobY)), module, StereoEffects::DELAY_FEEDBACK_PARAM));

		// Reverb section
		addParam(createParamCentered<EffectButton>(mm2px(Vec(reverbX, BUTTON_Y)), module, StereoEffects::REVERB_ENABLE_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(reverbX, BUTTON_Y - 5.0)), module, StereoEffects::REVERB_LIGHT));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(reverbX, firstKnobY)), module, StereoEffects::REVERB_SIZE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(reverbX, secondKnobY)), module, StereoEffects::REVERB_DAMPING_PARAM));

		// Echo section
		addParam(createParamCentered<EffectButton>(mm2px(Vec(echoX, BUTTON_Y)), module, StereoEffects::ECHO_ENABLE_PARAM));
		addChild(createLightCentered<SmallSimpleLight<WhiteLight>>(mm2px(Vec(echoX, BUTTON_Y - 5.0)), module, StereoEffects::ECHO_LIGHT));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(echoX, firstKnobY)), module, StereoEffects::ECHO_TIME_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(echoX, secondKnobY)), module, StereoEffects::ECHO_FEEDBACK_PARAM));

		// Inputs - positioned relative to panel center
		float panelCenterX = CENTER_COLUMN_X;
		float portSpacing = 12.0f;
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(panelCenterX - portSpacing, 100.0)), module, StereoEffects::LEFT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(panelCenterX + portSpacing, 100.0)), module, StereoEffects::RIGHT_INPUT));

		// Outputs - positioned relative to panel center
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(panelCenterX - portSpacing, 115.0)), module, StereoEffects::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(panelCenterX + portSpacing, 115.0)), module, StereoEffects::RIGHT_OUTPUT));
	}
};

Model* modelStereoEffects = createModel<StereoEffects, StereoEffectsWidget>("StereoEffects");

