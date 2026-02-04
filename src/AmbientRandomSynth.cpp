#include "plugin.hpp"
#include <vector>
#include <algorithm>
#include <cmath>

/**
 * Ambient Random Synth (基于 Omnia 参考实现)
 * 一个生成式环境合成器，能够根据设定的频率和比例自动播放音符。
 */
struct AmbientRandomSynth : Module {
	enum ParamId {
		TEMPO_PARAM,
		DENSITY_PARAM,
		MOTION_PARAM,
		TONE_PARAM,
		SPACE_PARAM,
		MIX_PARAM,
		ROOT_PARAM,
		SCALE_PARAM,
		FREEZE_PARAM,
		RESET_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLK_INPUT,
		RST_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		VOCT_OUTPUT,
		GATE_OUTPUT,
		L_OUTPUT,
		R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		FREEZE_LIGHT,
		LIGHTS_LEN
	};

	struct Voice {
		bool active = false;
		float freq = 0.f;
		float phase[3] = {0.f, 0.f, 0.f};
		float env = 0.f;
		float envPhase = 0.f;
		float attack = 0.5f;
		float release = 3.0f;
		float midiNote = 0.f;

		void trigger(float f, float att, float rel, float note) {
			active = true;
			freq = f;
			attack = att;
			release = rel;
			midiNote = note;
			envPhase = 0.f;
			// 不重置相位以获得更平滑的声音
		}

		float process(float dt) {
			if (!active) return 0.f;

			// 包络逻辑：模拟 JS 中的线性上升和指数下降
			if (envPhase < attack) {
				env = (envPhase / attack) * 0.2f;
			} else {
				float t = envPhase - attack;
				// 指数衰减到约 0.001
				env = 0.2f * std::exp(-5.3f * t / release);
				if (env < 0.001f) {
					active = false;
					env = 0.f;
					return 0.f;
				}
			}
			envPhase += dt;

			float out = 0.f;
			// 3个振荡器提供厚实的 Pad 声音，带有轻微失谐 (约 +/- 5 cents)
			float detunes[3] = {1.f, 1.00289f, 0.99712f}; 
			for (int i = 0; i < 3; i++) {
				phase[i] += freq * detunes[i] * dt;
				if (phase[i] >= 1.f) phase[i] -= 1.f;
				out += std::sin(2.f * M_PI * phase[i]);
			}
			return out / 3.f * env;
		}
	};

	Voice voices[16];
	dsp::BiquadFilter filter;
	
	// 延迟缓存
	std::vector<float> delayBufferL;
	std::vector<float> delayBufferR;
	int delayWritePtr = 0;
	
	float tickTimer = 0.f;
	dsp::SchmittTrigger clkTrigger;
	dsp::SchmittTrigger rstTrigger;
	dsp::SchmittTrigger resetBtnTrigger;
	dsp::SchmittTrigger freezeBtnTrigger;
	bool freeze = false;
	
	float lastVoct = 0.f;
	float gateTimer = 0.f;

	const std::vector<std::vector<int>> SCALES = {
		{0, 2, 4, 5, 7, 9, 11}, // Major
		{0, 2, 3, 5, 7, 8, 10}, // Minor
		{0, 2, 4, 7, 9},         // Pentatonic
		{0, 2, 4, 6, 7, 9, 11}, // Lydian
		{0, 1, 3, 5, 7, 8, 10}, // Phrygian
		{0, 2, 3, 5, 7, 9, 10}  // Dorian
	};

	AmbientRandomSynth() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		
		configParam(TEMPO_PARAM, 20.f, 180.f, 60.f, "Tempo", " BPM");
		configParam(DENSITY_PARAM, 0.f, 1.f, 0.4f, "Density");
		configParam(MOTION_PARAM, 0.f, 1.f, 0.5f, "Motion");
		configParam(TONE_PARAM, 200.f, 4000.f, 800.f, "Tone", " Hz");
		configParam(SPACE_PARAM, 0.f, 1.f, 0.6f, "Space");
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix");
		configParam(ROOT_PARAM, 0.f, 11.f, 0.f, "Root");
		configParam(SCALE_PARAM, 0.f, 5.f, 2.f, "Scale");
		configButton(FREEZE_PARAM, "Freeze");
		configButton(RESET_PARAM, "Reset");

		configInput(CLK_INPUT, "Clock");
		configInput(RST_INPUT, "Reset");
		
		configOutput(VOCT_OUTPUT, "V/Oct");
		configOutput(GATE_OUTPUT, "Gate");
		configOutput(L_OUTPUT, "Left Audio");
		configOutput(R_OUTPUT, "Right Audio");

		delayBufferL.resize(192000, 0.f); // 48k 下 4 秒
		delayBufferR.resize(192000, 0.f);
	}

	void onSampleRateChange() override {
		delayBufferL.assign(delayBufferL.size(), 0.f);
		delayBufferR.assign(delayBufferR.size(), 0.f);
	}

	void playNote() {
		int root = (int)params[ROOT_PARAM].getValue();
		int scaleIdx = (int)params[SCALE_PARAM].getValue();
		const auto& scaleArr = SCALES[scaleIdx];
		
		int octave = (int)(random::uniform() * 3) + 2; // 2 到 4 八度
		int scaleNote = scaleArr[(int)(random::uniform() * scaleArr.size())];
		int midiNote = root + scaleNote + octave * 12;
		float freq = 440.f * std::pow(2.f, (midiNote - 69) / 12.f);
		
		float motion = params[MOTION_PARAM].getValue();
		float attack = 0.5f + motion * 2.f;
		float release = 3.f + motion * 4.f;

		// 查找空闲声部
		for (int i = 0; i < 16; i++) {
			if (!voices[i].active) {
				voices[i].trigger(freq, attack, release, (float)midiNote);
				lastVoct = (midiNote - 60) / 12.f; // 0V at C4
				gateTimer = 0.15f; // 150ms 门信号
				break;
			}
		}
	}

	void process(const ProcessArgs& args) override {
		// 重置逻辑
		if (rstTrigger.process(inputs[RST_INPUT].getVoltage()) || resetBtnTrigger.process(params[RESET_PARAM].getValue())) {
			params[TEMPO_PARAM].setValue(60.f);
			params[DENSITY_PARAM].setValue(0.4f);
			params[MOTION_PARAM].setValue(0.5f);
			params[TONE_PARAM].setValue(800.f);
			params[SPACE_PARAM].setValue(0.6f);
			params[MIX_PARAM].setValue(0.5f);
			params[ROOT_PARAM].setValue(0.f);
			params[SCALE_PARAM].setValue(2.f);
			freeze = false;
			for (int i = 0; i < 16; i++) voices[i].active = false;
		}

		if (freezeBtnTrigger.process(params[FREEZE_PARAM].getValue())) {
			freeze = !freeze;
		}
		lights[FREEZE_LIGHT].setBrightness(freeze ? 1.f : 0.f);

		// 触发逻辑
		float tempo = params[TEMPO_PARAM].getValue();
		float interval = (60.f / tempo) * 0.5f; 
		
		bool tick = false;
		if (clkTrigger.process(inputs[CLK_INPUT].getVoltage())) {
			tick = true;
		} else {
			tickTimer += args.sampleTime;
			if (tickTimer >= interval) {
				tickTimer = 0.f;
				tick = true;
			}
		}

		if (tick && !freeze) {
			float density = params[DENSITY_PARAM].getValue();
			if (random::uniform() < density) {
				playNote();
			}
		}

		// 音频处理
		float dryL = 0.f;
		for (int i = 0; i < 16; i++) {
			dryL += voices[i].process(args.sampleTime);
		}
		
		// 滤波器
		float tone = params[TONE_PARAM].getValue();
		filter.setParameters(dsp::BiquadFilter::LOWPASS, clamp(tone / args.sampleRate, 0.f, 0.45f), 0.707f, 1.0f);
		float filtered = filter.process(dryL);

		// 延迟效果 (带交叉反馈的立体声延迟)
		float space = params[SPACE_PARAM].getValue();
		float delayTime = 0.2f + (1.0f - space) * 0.8f;
		float feedback = std::min(0.85f, 0.3f + space * 0.55f);
		
		int delaySamples = (int)(delayTime * args.sampleRate);
		delaySamples = clamp(delaySamples, 1, (int)delayBufferL.size() - 1);
		
		int readPtr = (delayWritePtr - delaySamples + delayBufferL.size()) % delayBufferL.size();
		float delayedL = delayBufferL[readPtr];
		float delayedR = delayBufferR[readPtr];
		
		delayBufferL[delayWritePtr] = filtered + delayedR * feedback;
		delayBufferR[delayWritePtr] = filtered + delayedL * feedback;
		
		delayWritePtr = (delayWritePtr + 1) % delayBufferL.size();

		// 混合 (Dry/Wet)
		float mix = params[MIX_PARAM].getValue();
		float outL = filtered * (1.f - mix) + delayedL * mix;
		float outR = filtered * (1.f - mix) + delayedR * mix;

		// 最终增益控制，增加约 30% 音量 (从 5.0f 提升至 6.5f)
		float finalGain = 6.5f;
		outputs[L_OUTPUT].setVoltage(outL * finalGain);
		outputs[R_OUTPUT].setVoltage(outR * finalGain);
		
		// CV 输出
		outputs[VOCT_OUTPUT].setVoltage(lastVoct);
		if (gateTimer > 0.f) {
			outputs[GATE_OUTPUT].setVoltage(10.f);
			gateTimer -= args.sampleTime;
		} else {
			outputs[GATE_OUTPUT].setVoltage(0.f);
		}
	}
};

struct AmbientRandomSynthWidget : ModuleWidget {
	AmbientRandomSynthWidget(AmbientRandomSynth* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/AmbientRandomSynth.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// 第一行旋钮
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.55, 30)), module, AmbientRandomSynth::TEMPO_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.64, 30)), module, AmbientRandomSynth::DENSITY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(67.73, 30)), module, AmbientRandomSynth::MOTION_PARAM));

		// 第二行旋钮
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.55, 58)), module, AmbientRandomSynth::TONE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.64, 58)), module, AmbientRandomSynth::SPACE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(67.73, 58)), module, AmbientRandomSynth::MIX_PARAM));

		// 控制行
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(18, 82.35)), module, AmbientRandomSynth::ROOT_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(35, 82.35)), module, AmbientRandomSynth::SCALE_PARAM));
		
		// 按钮位置匹配 SVG 中的设计
		addParam(createParamCentered<VCVButton>(mm2px(Vec(59, 83)), module, AmbientRandomSynth::FREEZE_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(59, 91)), module, AmbientRandomSynth::RESET_PARAM));
		
		addChild(createLightCentered<SmallSimpleLight<BlueLight>>(mm2px(Vec(59, 78)), module, AmbientRandomSynth::FREEZE_LIGHT));

		// I/O 区域
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10, 107.24)), module, AmbientRandomSynth::CLK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22, 107.24)), module, AmbientRandomSynth::RST_INPUT));
		
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(37, 107.24)), module, AmbientRandomSynth::VOCT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(49, 107.24)), module, AmbientRandomSynth::GATE_OUTPUT));
		
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(62, 107.24)), module, AmbientRandomSynth::L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(74, 107.24)), module, AmbientRandomSynth::R_OUTPUT));
	}
};

Model* modelAmbientRandomSynth = createModel<AmbientRandomSynth, AmbientRandomSynthWidget>("AmbientRandomSynth");
