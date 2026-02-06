/*
 * BuildupLooper — Build-up looper for VCV Rack 2
 *
 * 使用说明 (Usage):
 * - 连接：AUDIO IN L/R 接音源，AUDIO OUT L/R 接下游。TRIG/GATE 可选，用于按住触发。
 * - 触发：点击 BUILD 按钮进入 build（再点退出），或 TRIG 输入高电平进入、低电平退出。
 * - INTENSITY：循环加速到的最大倍率 (1x~15x)；可接 CV（0–10V=1–15x），如 LFO 由外部控制速度。
 * - TIME：从 1x 加速到最大倍率所需时间 (2~16s)。
 * - LOOP：截取的片段长度 (1/16秒~2秒)，决定 loop 内容多长。
 * - CLOCK：若接入时钟，loop 长度按小节选择（1/2/4/8 bar），由 BAR 旋钮选择；无时钟时仍用 LOOP 旋钮（秒）。
 */

#include "plugin.hpp"

namespace BuildupLooper {

// 最大采样率下 2.5s 的样本数，用于环形缓冲（ring buffer）
static constexpr int RING_MAX_SAMPLES = (int)(2.5f * 192000.f);
// 最大 loop 长度 2s @ 192k
static constexpr int LOOP_MAX_SAMPLES = (int)(2.f * 192000.f);
// 退出 build 时的 crossfade 时长 (ms)
static constexpr float EXIT_FADE_MS = 20.f;
// Loop 接缝处 crossfade 时长 (ms)，避免爆音
static constexpr float LOOP_FADE_MS = 10.f;

// Ease-in-out: progress in [0,1] -> smooth curve (techno build-up feel)
static inline float easeInOut(float x) {
	x = math::clamp(x, 0.f, 1.f);
	return x * x * (3.f - 2.f * x);
}

// 线性插值从 buffer 读取（避免锯齿，不分配内存）
static inline float readLinear(const float* buf, int size, float pos) {
	if (size <= 0) return 0.f;
	while (pos < 0.f) pos += size;
	while (pos >= size) pos -= size;
	int i0 = (int)pos;
	int i1 = i0 + 1;
	if (i1 >= size) i1 = 0;
	float f = pos - (float)i0;
	return buf[i0] * (1.f - f) + buf[i1] * f;
}

struct BuildupLooperModule : Module {
	enum ParamId {
		BUILD_PARAM,
		INTENSITY_PARAM,
		TIME_PARAM,
		LOOP_PARAM,
		BAR_PARAM,  // 1/2/4/8 bar，仅当 CLOCK 接入时有效
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,
		CLOCK_INPUT,
		INTENSITY_INPUT,
		AUDIO_L_INPUT,
		AUDIO_R_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_L_OUTPUT,
		AUDIO_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		BUILD_LIGHT,
		LIGHTS_LEN
	};

	// 环形缓冲：平时持续写入最近 2s+ 的音频；触发时从其中截取最近 L 作为 loop（锁定后不再写入该段）
	// 延迟分配，避免构造时大块分配导致宿主崩溃
	float* ringL = nullptr;
	float* ringR = nullptr;
	int ringSize = 0;
	int ringWritePos = 0;

	// 锁定后的 loop 副本（触发瞬间从 ring 拷贝，之后只读）
	float* loopL = nullptr;
	float* loopR = nullptr;
	int loopSamples = 0;  // 当前 loop 长度（锁定后不变）

	// Build 状态
	enum State { IDLE, BUILD, EXIT_FADE };
	State state = IDLE;
	float playheadL = 0.f;   // 当前播放位置 [0, loopSamples)
	float playheadR = 0.f;
	int rampSamples = 0;     // 已加速的样本数
	float exitFadeSamples = 0.f;
	float exitFadeTotal = 1.f;

	// 按钮 toggle：无 TRIG 时点击进入/退出
	bool buttonToggleState = false;
	bool prevButtonPressed = false;

	// 参数平滑（避免 zipper noise）
	float smoothedIntensity = 1.f;
	float smoothedTime = 8.f;
	float smoothedLoopSec = 0.25f;

	// Clock：检测上升沿并测量周期（假定每拍一个脉冲，1 bar = 4 拍）
	bool prevClockHigh = false;
	int clockSampleCounter = 0;
	int lastClockSample = 0;
	float beatPeriodSamples = 0.f;  // 测得的一拍长度（样本数）

	BuildupLooperModule() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configButton(BUILD_PARAM, "BUILD");
		configParam(INTENSITY_PARAM, 1.f, 15.f, 1.f, "INTENSITY", "x");
		configParam(TIME_PARAM, 2.f, 16.f, 8.f, "TIME", " s");
		configParam(LOOP_PARAM, 1.f / 16.f, 2.f, 0.25f, "LOOP", " s");
		configSwitch(BAR_PARAM, 0.f, 3.f, 0.f, "Bars (with clock)", {"1", "2", "4", "8"});
		configInput(TRIG_INPUT, "TRIG/GATE");
		configInput(CLOCK_INPUT, "CLOCK");
		configInput(INTENSITY_INPUT, "INTENSITY (0–10V = 1–15x)");
		configInput(AUDIO_L_INPUT, "AUDIO L");
		configInput(AUDIO_R_INPUT, "AUDIO R");
		configOutput(AUDIO_L_OUTPUT, "AUDIO L");
		configOutput(AUDIO_R_OUTPUT, "AUDIO R");
	}

	~BuildupLooperModule() {
		if (ringL) { delete[] ringL; ringL = nullptr; }
		if (ringR) { delete[] ringR; ringR = nullptr; }
		if (loopL) { delete[] loopL; loopL = nullptr; }
		if (loopR) { delete[] loopR; loopR = nullptr; }
	}

	// 首次 process 时分配缓冲，避免构造时大块分配导致加载插件崩溃
	void ensureBuffers() {
		if (ringL) return;
		ringSize = RING_MAX_SAMPLES;
		ringL = new float[ringSize];
		ringR = new float[ringSize];
		loopL = new float[LOOP_MAX_SAMPLES];
		loopR = new float[LOOP_MAX_SAMPLES];
		std::memset(ringL, 0, sizeof(float) * ringSize);
		std::memset(ringR, 0, sizeof(float) * ringSize);
		std::memset(loopL, 0, sizeof(float) * LOOP_MAX_SAMPLES);
		std::memset(loopR, 0, sizeof(float) * LOOP_MAX_SAMPLES);
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		exitFadeTotal = EXIT_FADE_MS * 0.001f * e.sampleRate;
	}

	void process(const ProcessArgs& args) override {
		ensureBuffers();  // 首次调用时分配，避免加载插件时崩溃
		float sr = args.sampleRate;
		if (exitFadeTotal <= 0.f || exitFadeTotal > 96000.f)
			exitFadeTotal = EXIT_FADE_MS * 0.001f * sr;
		bool trigConnected = inputs[TRIG_INPUT].isConnected();
		float trigV = inputs[TRIG_INPUT].getVoltage();
		bool gateHigh = trigV >= 0.5f;

		// 按钮：任意边沿翻转 toggle（按一次进 build，再按一次退 build）
		bool buttonPressed = params[BUILD_PARAM].getValue() >= 0.5f;
		if (buttonPressed != prevButtonPressed)
			buttonToggleState = !buttonToggleState;
		prevButtonPressed = buttonPressed;

		// 是否“希望处于 build”：Gate 高 或 （无 Gate 时按钮 toggle 为开）
		bool wantBuild = gateHigh || (!trigConnected && buttonToggleState);

		// 参数平滑；INTENSITY 可由旋钮或 CV 输入控制（有输入时 0–10V 映射到 1–15x）
		float intensityParam = params[INTENSITY_PARAM].getValue();
		float timeParam = params[TIME_PARAM].getValue();
		float loopParam = params[LOOP_PARAM].getValue();
		float smooth = 0.0005f;
		float targetIntensity = intensityParam;
		if (inputs[INTENSITY_INPUT].isConnected()) {
			float v = inputs[INTENSITY_INPUT].getVoltage();
			targetIntensity = 1.f + (v / 10.f) * 14.f;  // 0V=1x, 10V=15x
			targetIntensity = math::clamp(targetIntensity, 1.f, 15.f);
		}
		smoothedIntensity += (targetIntensity - smoothedIntensity) * smooth;
		smoothedTime += (timeParam - smoothedTime) * smooth;
		smoothedLoopSec += (loopParam - smoothedLoopSec) * smooth;

		// Loop 长度：有 CLOCK 时按小节（1/2/4/8 bar），否则按 LOOP 旋钮（秒）
		int L_samples;
		bool clockConnected = inputs[CLOCK_INPUT].isConnected();
		if (clockConnected) {
			float clockV = inputs[CLOCK_INPUT].getVoltage();
			bool clockHigh = clockV >= 0.5f;
			if (clockHigh && !prevClockHigh) {
				int period = clockSampleCounter - lastClockSample;
				if (period > 0 && period < (int)(sr * 4.f)) {  // 合理范围约 15~240 BPM
					beatPeriodSamples = 0.1f * beatPeriodSamples + 0.9f * (float)period;
				}
				lastClockSample = clockSampleCounter;
			}
			prevClockHigh = clockHigh;
			clockSampleCounter++;

			int barIndex = (int)(params[BAR_PARAM].getValue() + 0.5f);
			barIndex = math::clamp(barIndex, 0, 3);
			int bars = 1 << barIndex;  // 1, 2, 4, 8
			// 1 bar = 4 beats（4/4），loop = bars * 4 * 一拍
			if (beatPeriodSamples > 0.f)
				L_samples = (int)((float)(bars * 4) * beatPeriodSamples);
			else
				L_samples = (int)(smoothedLoopSec * sr);  // 尚未测到时钟时用 LOOP
			L_samples = math::clamp(L_samples, 1, ringSize > 0 ? std::min(LOOP_MAX_SAMPLES, ringSize) : LOOP_MAX_SAMPLES);
		} else {
			L_samples = (int)(smoothedLoopSec * sr);
			L_samples = math::clamp(L_samples, 1, LOOP_MAX_SAMPLES);
		}
		int Nfade = (int)(LOOP_FADE_MS * 0.001f * sr);
		Nfade = math::clamp(Nfade, 4, L_samples / 2);

		bool hasL = inputs[AUDIO_L_INPUT].isConnected();
		bool hasR = inputs[AUDIO_R_INPUT].isConnected();
		float inL = hasL ? inputs[AUDIO_L_INPUT].getVoltage() / 10.f : 0.f;
		float inR = hasR ? inputs[AUDIO_R_INPUT].getVoltage() / 10.f : 0.f;
		if (hasL && !hasR) inR = inL;
		if (hasR && !hasL) inL = inR;

		// 平时：始终写入环形缓冲（直通时也写，保证触发时有最近 L 可用）
		ringL[ringWritePos] = inL;
		ringR[ringWritePos] = inR;
		ringWritePos = (ringWritePos + 1) % ringSize;

		// ---------- 状态机 ----------
		if (state == IDLE) {
			if (wantBuild) {
				// 进入 build：锁定 loop = 最近 L_samples 从 ring 拷贝到 loopBuffer
				int start = (ringWritePos - L_samples + ringSize) % ringSize;
				for (int i = 0; i < L_samples; i++) {
					int r = (start + i) % ringSize;
					loopL[i] = ringL[r];
					loopR[i] = ringR[r];
				}
				loopSamples = L_samples;
				playheadL = 0.f;
				playheadR = 0.f;
				rampSamples = 0;
				state = BUILD;
			}
			outputs[AUDIO_L_OUTPUT].setVoltage(inL * 10.f);
			outputs[AUDIO_R_OUTPUT].setVoltage(inR * 10.f);
			lights[BUILD_LIGHT].setBrightness(0.f);
			return;
		}

		if (state == BUILD) {
			if (!wantBuild) {
				state = EXIT_FADE;
				exitFadeSamples = 0.f;
				// 本帧立即做 exit fade（mix=0），避免漏帧
			} else {
				// 加速曲线：progress 0~1 over T seconds, rate 1 -> rateMax (ease-in-out)
				rampSamples++;
				float T_samples = smoothedTime * sr;
				float progress = T_samples > 0.f ? math::clamp((float)rampSamples / T_samples, 0.f, 1.f) : 1.f;
				float rate = 1.f + (smoothedIntensity - 1.f) * easeInOut(progress);

				playheadL += rate;
				playheadR += rate;
				while (playheadL >= loopSamples) playheadL -= loopSamples;
				while (playheadR >= loopSamples) playheadR -= loopSamples;
				while (playheadL < 0.f) playheadL += loopSamples;
				while (playheadR < 0.f) playheadR += loopSamples;

				// 从 loop 带 crossfade 的读取（接缝处淡入淡出）
				float outL = readWithLoopCrossfade(loopL, loopSamples, playheadL, Nfade);
				float outR = readWithLoopCrossfade(loopR, loopSamples, playheadR, Nfade);

				// 可选：build 期间若信号很小则轻微提升增益（soft clip）
				float gain = 1.f;
				float peak = std::max(std::fabs(outL), std::fabs(outR));
				if (peak > 0.001f && peak < 0.2f) gain = 0.25f / peak;
				gain = math::clamp(gain, 1.f, 3.f);
				outL *= gain;
				outR *= gain;
				outL = math::clamp(outL, -1.2f, 1.2f);
				outR = math::clamp(outR, -1.2f, 1.2f);

				outputs[AUDIO_L_OUTPUT].setVoltage(outL * 10.f);
				outputs[AUDIO_R_OUTPUT].setVoltage(outR * 10.f);
				// BUILD 灯随 progress 变亮
				lights[BUILD_LIGHT].setBrightness(0.3f + 0.7f * progress);
				return;
			}
		}

		if (state == EXIT_FADE) {
			float mix = exitFadeTotal > 0.f ? math::clamp(exitFadeSamples / exitFadeTotal, 0.f, 1.f) : 1.f;
			float loopL_out = readWithLoopCrossfade(loopL, loopSamples, playheadL, Nfade);
			float loopR_out = readWithLoopCrossfade(loopR, loopSamples, playheadR, Nfade);
			float outL = loopL_out * (1.f - mix) + inL * mix;
			float outR = loopR_out * (1.f - mix) + inR * mix;
			outputs[AUDIO_L_OUTPUT].setVoltage(outL * 10.f);
			outputs[AUDIO_R_OUTPUT].setVoltage(outR * 10.f);
			lights[BUILD_LIGHT].setBrightness(0.3f * (1.f - mix));
			exitFadeSamples += 1.f;
			if (exitFadeSamples >= exitFadeTotal)
				state = IDLE;
		}
	}

	// 在 loop 结尾 Nfade 与开头 Nfade 做 crossfade，无爆音
	float readWithLoopCrossfade(const float* buf, int len, float pos, int Nfade) {
		if (len <= 0) return 0.f;
		float p = pos;
		while (p >= len) p -= len;
		while (p < 0.f) p += len;
		float end = readLinear(buf, len, p);
		if (Nfade <= 0 || len < 2 * Nfade) return end;
		float startPos = p - (float)(len - Nfade);
		if (startPos < 0.f) startPos += len;
		float start = readLinear(buf, len, startPos);
		float w = 0.f;
		if (p >= (float)(len - Nfade))
			w = (p - (float)(len - Nfade)) / (float)Nfade;
		return end * (1.f - w) + start * w;
	}
};

// 与 StereoEffects 相同的按钮组件，避免 LEDBezel 在某些 Rack 环境下导致崩溃
struct BuildButton : app::SvgSwitch {
	BuildButton() {
		momentary = false;
		latch = true;
		shadow->opacity = 0.0;
		addFrame(Svg::load(asset::system("res/ComponentLibrary/TL1105_0.svg")));
		addFrame(Svg::load(asset::system("res/ComponentLibrary/TL1105_1.svg")));
	}
};

struct BuildupLooperWidget : ModuleWidget {
	BuildupLooperWidget(BuildupLooperModule* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/BuildupLooper.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		float cx = 22.86f;
		addParam(createParamCentered<BuildButton>(mm2px(Vec(cx, 26)), module, BuildupLooperModule::BUILD_PARAM));
		addChild(createLightCentered<SmallSimpleLight<GreenLight>>(mm2px(Vec(cx, 21)), module, BuildupLooperModule::BUILD_LIGHT));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx - 8, 46)), module, BuildupLooperModule::INTENSITY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx + 8, 46)), module, BuildupLooperModule::TIME_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx - 8, 64)), module, BuildupLooperModule::LOOP_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx + 8, 64)), module, BuildupLooperModule::BAR_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(cx - 8, 76)), module, BuildupLooperModule::INTENSITY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(cx - 8, 86)), module, BuildupLooperModule::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(cx + 8, 86)), module, BuildupLooperModule::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(cx - 8, 102)), module, BuildupLooperModule::AUDIO_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(cx + 8, 102)), module, BuildupLooperModule::AUDIO_R_INPUT));
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(cx - 8, 118)), module, BuildupLooperModule::AUDIO_L_OUTPUT));
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(cx + 8, 118)), module, BuildupLooperModule::AUDIO_R_OUTPUT));
	}
};

} // namespace BuildupLooper

Model* modelBuildupLooper = createModel<BuildupLooper::BuildupLooperModule, BuildupLooper::BuildupLooperWidget>("BuildupLooper");
