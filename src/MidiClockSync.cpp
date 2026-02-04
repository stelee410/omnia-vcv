#include "plugin.hpp"
#include <dsp/digital.hpp>

struct MidiClockSync : Module {
	enum ParamId {
		BPM_PARAM,
		TRIGGER_DIVISION_PARAM,
		TRIPLET_PARAM,
		RESET_PARAM,
		STOP_RUN_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		SYNC_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		CLOCK_OUTPUT,
		RESET_OUTPUT,
		TRIGGER_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		RESET_LIGHT,
		TRIPLET_LIGHT,
		STOP_RUN_LIGHT,
		LIGHTS_LEN
	};

	enum TriggerDivision {
		DIV_4_1 = 0,    // 384 pulses (16 beats)
		DIV_2_1 = 1,    // 192 pulses (8 beats)
		DIV_1_1 = 2,    // 96 pulses (4 beats)
		DIV_1_2 = 3,    // 48 pulses (2 beats)
		DIV_1_4 = 4,    // 24 pulses (1 beat)
		DIV_1_8 = 5,    // 12 pulses (1/2 beat)
		DIV_1_16 = 6,   // 6 pulses (1/4 beat)
		DIV_1_32 = 7,   // 3 pulses (1/8 beat)
		DIV_1_64 = 8    // 1.5 pulses (1/16 beat) - will use 1 or 2 pulses
	};

	// MIDI clock: 24 PPQN (Pulses Per Quarter Note)
	static constexpr int MIDI_PPQN = 24;
	
	dsp::Timer clockTimer;
	dsp::PulseGenerator clockPulse;
	dsp::PulseGenerator resetPulse;
	dsp::PulseGenerator triggerPulse;
	dsp::ClockDivider triggerDivider;
	
	uint32_t clockCounter = 0;
	int currentDivision = 24; // default 1/4
	float lastBPM = 120.f;
	bool lastResetButtonState = false;
	float lastSyncInput = 0.f;
	bool lastSyncMode = false;
	bool lastStopRunState = false;

	MidiClockSync() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(BPM_PARAM, 30.f, 300.f, 120.f, "BPM", " bpm");
		
		// Trigger Division with labels using configSwitch
		configSwitch(TRIGGER_DIVISION_PARAM, 0.f, 8.f, 4.f, "Trigger Division", {"4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64"});
		
		configButton(TRIPLET_PARAM, "Triplet");
		configButton(RESET_PARAM, "Reset");
		configButton(STOP_RUN_PARAM, "Stop/Run");
		
		configInput(SYNC_INPUT, "Sync In");
		
		configOutput(CLOCK_OUTPUT, "Clock");
		configOutput(RESET_OUTPUT, "Reset");
		configOutput(TRIGGER_OUTPUT, "Trigger");
		
		configLight(RESET_LIGHT, "Reset");
		configLight(TRIPLET_LIGHT, "Triplet");
		configLight(STOP_RUN_LIGHT, "Run");
		
		triggerDivider.setDivision(24); // default to 1/4
	}
	
	void onReset() override {
		clockTimer.reset();
		clockPulse.reset();
		resetPulse.reset();
		triggerPulse.reset();
		clockCounter = 0;
		triggerDivider.reset();
		currentDivision = 24;
		lastBPM = 120.f;
		lastSyncInput = 0.f;
		lastSyncMode = false;
		lastStopRunState = false;
	}

	void process(const ProcessArgs& args) override {
		// Get BPM parameter
		float bpm = params[BPM_PARAM].getValue();
		
		// Stop/Run button (latch/toggle)
		bool stopRunState = params[STOP_RUN_PARAM].getValue() > 0.5f;
		bool isRunning = stopRunState; // true = run, false = stop
		
		// Detect transition from stop to run: trigger reset
		if (isRunning && !lastStopRunState) {
			// Transition from stop to run: trigger reset
			resetPulse.trigger(1e-3f);
			clockTimer.reset();
			clockCounter = 0;
			triggerDivider.reset();
		}
		
		// Detect transition from run to stop: immediately stop all pulses
		if (!isRunning && lastStopRunState) {
			clockPulse.reset();
			triggerPulse.reset();
		}
		
		lastStopRunState = stopRunState;
		lights[STOP_RUN_LIGHT].setBrightness(isRunning ? 1.f : 0.f);
		
		// Manual reset button (edge detection)
		bool resetButtonState = params[RESET_PARAM].getValue() > 0.5f;
		if (resetButtonState && !lastResetButtonState) {
			// Rising edge: trigger reset
			resetPulse.trigger(1e-3f);
			clockTimer.reset();
			clockCounter = 0;
			triggerDivider.reset();
		}
		lastResetButtonState = resetButtonState;
		lights[RESET_LIGHT].setBrightness(resetButtonState ? 1.f : 0.f);
		
		// Check if BPM changed significantly (for auto reset)
		if (std::abs(bpm - lastBPM) > 1.0f) {
			lastBPM = bpm;
			clockTimer.reset();
			clockCounter = 0;
		}
		
		// Calculate clock period (24 PPQN = 24 pulses per quarter note)
		// Time per pulse = 60 / (BPM * 24) seconds
		float pulsePeriod = 60.f / (bpm * MIDI_PPQN);
		
		// Get trigger division parameter (0-8) and triplet mode
		int divisionIndex = (int)std::round(params[TRIGGER_DIVISION_PARAM].getValue());
		if (divisionIndex < 0) divisionIndex = 0;
		if (divisionIndex > 8) divisionIndex = 8;
		bool tripletMode = params[TRIPLET_PARAM].getValue() > 0.5f;
		
		// Determine division value based on index and triplet mode
		// Triplet: multiply by 2/3 (same time, 3 notes instead of 2)
		// Note: 1/64 = 1.5 pulses, we use 1 pulse (closest integer)
		int division = 24; // default 1/4
		switch (divisionIndex) {
			case DIV_4_1:   division = tripletMode ? 384 : 384; break; // 16 beats (4/1T same as 4/1)
			case DIV_2_1:   division = tripletMode ? 192 : 192; break; // 8 beats (2/1T same as 2/1)
			case DIV_1_1:   division = tripletMode ? 96 : 96; break;   // 4 beats (1/1T same as 1/1)
			case DIV_1_2:   division = tripletMode ? 32 : 48; break;   // 2 beats → 1/2T: 48*2/3=32
			case DIV_1_4:   division = tripletMode ? 16 : 24; break;   // 1 beat → 1/4T: 24*2/3=16
			case DIV_1_8:   division = tripletMode ? 8 : 12; break;    // 1/2 beat → 1/8T: 12*2/3=8
			case DIV_1_16:  division = tripletMode ? 4 : 6; break;     // 1/4 beat → 1/16T: 6*2/3=4
			case DIV_1_32:  division = tripletMode ? 2 : 3; break;     // 1/8 beat → 1/32T: 3*2/3=2
			case DIV_1_64:  division = tripletMode ? 1 : 2; break;     // 1/16 beat (1.5→2 pulses, 1/64T→1 pulse: 1.5*2/3=1)
			default:        division = 24; break;
		}
		
		// Update triplet light
		lights[TRIPLET_LIGHT].setBrightness(tripletMode ? 1.f : 0.f);
		
		// Update divider if division changed
		if (division != currentDivision) {
			currentDivision = division;
			triggerDivider.setDivision(division);
		}
		
		// Check for sync input
		bool syncMode = inputs[SYNC_INPUT].isConnected();
		
		// Reset timer when switching from sync mode to internal clock mode
		if (!syncMode && lastSyncMode) {
			clockTimer.reset();
		}
		
		// Only generate clock if running
		if (isRunning) {
			if (syncMode) {
				// Sync mode: detect rising edge from sync input
				float syncInput = inputs[SYNC_INPUT].getVoltage();
				if (syncInput > 1.f && lastSyncInput <= 1.f) {
					// Rising edge detected: trigger clock pulse
					clockPulse.trigger(1e-3f); // 1ms pulse
					clockCounter++;
					
					// Check trigger divider
					if (triggerDivider.process()) {
						triggerPulse.trigger(1e-3f); // 1ms trigger pulse
					}
				}
				lastSyncInput = syncInput;
			} else {
				// Internal clock mode: use timer
				float elapsed = clockTimer.process(args.sampleTime);
				
				// Generate clock pulse every pulsePeriod
				if (elapsed >= pulsePeriod) {
					clockTimer.reset();
					clockPulse.trigger(1e-3f); // 1ms pulse
					clockCounter++;
					
					// Check trigger divider
					if (triggerDivider.process()) {
						triggerPulse.trigger(1e-3f); // 1ms trigger pulse
					}
				}
			}
		} else {
			// Stop mode: don't process timer or sync input, but still update lastSyncInput to avoid false triggers
			if (syncMode) {
				lastSyncInput = inputs[SYNC_INPUT].getVoltage();
			}
		}
		
		lastSyncMode = syncMode;
		
		// Process pulses
		bool clockHigh = clockPulse.process(args.sampleTime);
		bool resetHigh = resetPulse.process(args.sampleTime);
		bool triggerHigh = triggerPulse.process(args.sampleTime);
		
		// Output clock (10V pulse)
		outputs[CLOCK_OUTPUT].setVoltage(clockHigh ? 10.f : 0.f);
		
		// Output reset (10V pulse)
		outputs[RESET_OUTPUT].setVoltage(resetHigh ? 10.f : 0.f);
		
		// Output trigger (10V pulse)
		outputs[TRIGGER_OUTPUT].setVoltage(triggerHigh ? 10.f : 0.f);
	}
};

// Latch button for Triplet (toggle on/off)
struct TripletButton : app::SvgSwitch {
	TripletButton() {
		momentary = false;
		latch = true;
		shadow->opacity = 0.0;
		addFrame(Svg::load(asset::system("res/ComponentLibrary/TL1105_0.svg")));
		addFrame(Svg::load(asset::system("res/ComponentLibrary/TL1105_1.svg")));
	}
};

struct BpmDisplay : LedDisplay {
	LedDisplayTextField* textField;
	MidiClockSync* module;
	
	BpmDisplay() {
		box.size = mm2px(Vec(25, 8));
		textField = new LedDisplayTextField;
		textField->box.pos = Vec(0, 0);
		textField->box.size = box.size;
		addChild(textField);
	}
	
	void step() override {
		if (module) {
			float bpm = module->params[MidiClockSync::BPM_PARAM].getValue();
			textField->text = string::f("%.0f BPM", bpm);
		}
		LedDisplay::step();
	}
};

struct MidiClockSyncWidget : ModuleWidget {
	MidiClockSyncWidget(MidiClockSync* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/MidiClockSync.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// BPM display (centered, below title)
		BpmDisplay* bpmDisplay = new BpmDisplay;
		bpmDisplay->module = module;
		bpmDisplay->box.pos = mm2px(Vec(2.74, 15.0)); // Center: (30.48-25)/2 = 2.74
		addChild(bpmDisplay);

		// Sync In input (left) and BPM knob (right, larger size)
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0, 35.0)), module, MidiClockSync::SYNC_INPUT));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(21.0, 35.0)), module, MidiClockSync::BPM_PARAM));
		
		// Stop/Run button (below sync in input, left side)
		addParam(createParamCentered<TripletButton>(mm2px(Vec(10.0, 42.0)), module, MidiClockSync::STOP_RUN_PARAM));
		addChild(createLightCentered<SmallSimpleLight<GreenLight>>(mm2px(Vec(10.0, 42.0 - 0.68f)), module, MidiClockSync::STOP_RUN_LIGHT));
		
		// Trigger Division knob (snap knob for discrete values 0-8 with labels)
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(15.24, 55.0)), module, MidiClockSync::TRIGGER_DIVISION_PARAM));
		
		// Triplet button (latch/toggle button)
		addParam(createParamCentered<TripletButton>(mm2px(Vec(15.24, 68.0)), module, MidiClockSync::TRIPLET_PARAM));
		addChild(createLightCentered<SmallSimpleLight<YellowLight>>(mm2px(Vec(15.24, 68.0 - 0.68f)), module, MidiClockSync::TRIPLET_LIGHT));
		
		// Reset button
		addParam(createParamCentered<LEDButton>(mm2px(Vec(15.24, 78.0)), module, MidiClockSync::RESET_PARAM));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(15.24, 78.0)), module, MidiClockSync::RESET_LIGHT));

		// Outputs
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(15.24, 92.0)), module, MidiClockSync::CLOCK_OUTPUT));
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(15.24, 104.0)), module, MidiClockSync::RESET_OUTPUT));
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(15.24, 116.0)), module, MidiClockSync::TRIGGER_OUTPUT));
	}
};

Model* modelMidiClockSync = createModel<MidiClockSync, MidiClockSyncWidget>("MidiClockSync");

