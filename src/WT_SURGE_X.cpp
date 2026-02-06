#include "plugin.hpp"
#include <osdialog.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <atomic>
#include <mutex>

// --- Wavetable constants ---
static constexpr int TABLE_SIZE = 2048;
static constexpr int NUM_FRAMES = 64;
static constexpr int MIP_LEVELS = 10;
static constexpr int NUM_BANKS = 2;

// --- Warp modes ---
enum WarpMode {
	WARP_PHASE_DISTORT = 0,
	WARP_BEND_ASYM = 1,
	WARP_MIRROR = 2,
	WARP_FOLD = 3,
	WARP_SYNC_LIKE = 4
};

// --- WAV parser: minimal PCM16/float32, stereo→mono avg ---
struct WavParser {
	static bool load(const std::string& path, std::vector<float>& out, int targetLen, int targetFrames) {
		FILE* f = std::fopen(path.c_str(), "rb");
		if (!f) return false;
		char riff[4], fmt[4];
		uint32_t chunkSize, subchunk1Size;
		uint16_t audioFormat, numChannels;
		uint32_t sampleRate, byteRate;
		uint16_t blockAlign, bitsPerSample;
		std::fread(riff, 1, 4, f);
		if (std::memcmp(riff, "RIFF", 4) != 0) { std::fclose(f); return false; }
		std::fread(&chunkSize, 4, 1, f);
		std::fread(fmt, 1, 4, f);
		if (std::memcmp(fmt, "WAVE", 4) != 0) { std::fclose(f); return false; }
		std::fread(fmt, 1, 4, f);
		std::fread(&subchunk1Size, 4, 1, f);
		std::fread(&audioFormat, 2, 1, f);
		std::fread(&numChannels, 2, 1, f);
		std::fread(&sampleRate, 4, 1, f);
		std::fread(&byteRate, 4, 1, f);
		std::fread(&blockAlign, 2, 1, f);
		std::fread(&bitsPerSample, 2, 1, f);
		char dataId[4];
		uint32_t dataSize;
		while (std::fread(dataId, 1, 4, f) == 4) {
			std::fread(&dataSize, 4, 1, f);
			if (std::memcmp(dataId, "data", 4) == 0) break;
			std::fseek(f, (long)dataSize, SEEK_CUR);
		}
		if (std::memcmp(dataId, "data", 4) != 0) { std::fclose(f); return false; }
		int bytesPerSample = bitsPerSample / 8;
		int totalSamples = (int)(dataSize / (bytesPerSample * std::max(1, (int)numChannels)));
		std::vector<float> raw;
		raw.resize(totalSamples * (int)numChannels);
		if (audioFormat == 1 && bitsPerSample == 16) {
			for (size_t i = 0; i < raw.size(); i++) {
				int16_t s;
				if (std::fread(&s, 2, 1, f) != 1) break;
				raw[i] = (float)s / 32768.f;
			}
		} else if (audioFormat == 3 && bitsPerSample == 32) {
			for (size_t i = 0; i < raw.size(); i++) {
				float s;
				if (std::fread(&s, 4, 1, f) != 1) break;
				raw[i] = s;
			}
		} else { std::fclose(f); return false; }
		std::fclose(f);
		if (numChannels == 2) {
			std::vector<float> mono(totalSamples);
			for (int i = 0; i < totalSamples; i++)
				mono[i] = 0.5f * (raw[i * 2] + raw[i * 2 + 1]);
			raw = std::move(mono);
		}
		int wantLen = targetLen * targetFrames;
		if ((int)raw.size() >= wantLen) {
			out.resize(wantLen);
			int samplesPerFrame = (int)raw.size() / targetFrames;
			for (int f = 0; f < targetFrames; f++) {
				int frameStart = f * samplesPerFrame;
				for (int s = 0; s < targetLen; s++) {
					float t = (float)s / (float)targetLen * (float)(samplesPerFrame - 1);
					int i0 = frameStart + (int)t;
					if (i0 < 0) i0 = 0;
					if (i0 >= (int)raw.size()) i0 = (int)raw.size() - 1;
					int i1 = i0 + 1;
					if (i1 >= (int)raw.size()) i1 = (int)raw.size() - 1;
					float frac = t - (float)(int)t;
					out[f * targetLen + s] = raw[i0] * (1.f - frac) + raw[i1] * frac;
				}
			}
		} else {
			out.resize(wantLen);
			for (int i = 0; i < wantLen; i++) {
				float t = (float)i / (float)wantLen * (float)raw.size();
				int i0 = (int)t; if (i0 >= (int)raw.size()) i0 = (int)raw.size() - 1;
				int i1 = i0 + 1; if (i1 >= (int)raw.size()) i1 = (int)raw.size() - 1;
				float frac = t - (float)i0;
				out[i] = raw[i0] * (1.f - frac) + raw[i1] * frac;
			}
		}
		float peak = 0.0001f;
		for (float v : out) { float a = std::fabs(v); if (a > peak) peak = a; }
		for (float& v : out) v /= peak;
		return true;
	}
};

// --- Wavetable bank with mipmaps ---
struct WavetableBank {
	float tables[NUM_BANKS][NUM_FRAMES][MIP_LEVELS][TABLE_SIZE];
	std::atomic<bool> tableReady{true};
	std::mutex loadMutex;

	void generateDefault() {
		for (int bank = 0; bank < NUM_BANKS; bank++) {
			for (int frame = 0; frame < NUM_FRAMES; frame++) {
				float t = (float)frame / (float)(NUM_FRAMES - 1);
				for (int s = 0; s < TABLE_SIZE; s++) {
					float phase = (float)s / (float)TABLE_SIZE;
					float sample = 0.f;
					if (bank == 0) {
						float sine = std::sin(2.f * M_PI * phase);
						float saw = 2.f * phase - 1.f;
						float sq = (phase < 0.5f) ? 1.f : -1.f;
						float rich = sine;
						for (int h = 2; h <= 8; h++)
							rich += 0.3f / h * std::sin(2.f * M_PI * phase * h);
						rich /= 1.8f;
						sample = sine * (1.f - t * 0.7f) + saw * (t * 0.4f) + sq * (t * 0.3f) + rich * (t * 0.5f);
					} else {
						float sine = std::sin(2.f * M_PI * phase);
						float formant = 0.f;
						float w0 = 1.f, w1 = 0.5f * (1.f + std::sin(t * 2.f * M_PI));
						float w2 = 0.3f * (1.f + std::cos(t * 3.f * M_PI));
						for (int h = 1; h <= 12; h++) {
							float w = w0 / h + w1 / (h + 2) + w2 / (h + 4);
							formant += w * std::sin(2.f * M_PI * phase * h);
						}
						formant /= 2.5f;
						sample = sine * (1.f - t) + formant * t;
					}
					tables[bank][frame][0][s] = sample;
				}
				float peak = 0.0001f;
				for (int s = 0; s < TABLE_SIZE; s++) {
					float a = std::fabs(tables[bank][frame][0][s]);
					if (a > peak) peak = a;
				}
				for (int s = 0; s < TABLE_SIZE; s++)
					tables[bank][frame][0][s] /= peak;
				for (int mip = 1; mip < MIP_LEVELS; mip++) {
					int step = 1 << mip;
					for (int s = 0; s < TABLE_SIZE; s++) {
						float sum = 0.f; int n = 0;
						for (int k = s - step / 2; k <= s + step / 2; k += step) {
							int idx = (k % TABLE_SIZE + TABLE_SIZE) % TABLE_SIZE;
							sum += tables[bank][frame][0][idx];
							n++;
						}
						tables[bank][frame][mip][s] = (n > 0) ? (sum / (float)n) : tables[bank][frame][mip - 1][s];
					}
				}
			}
		}
	}

	void loadWav(int bank, const std::string& path) {
		std::lock_guard<std::mutex> lock(loadMutex);
		tableReady = false;
		std::vector<float> buf;
		if (WavParser::load(path, buf, TABLE_SIZE, NUM_FRAMES)) {
			for (int f = 0; f < NUM_FRAMES; f++) {
				for (int s = 0; s < TABLE_SIZE; s++)
					tables[bank][f][0][s] = buf[f * TABLE_SIZE + s];
				for (int mip = 1; mip < MIP_LEVELS; mip++) {
					int step = 1 << mip;
					for (int s = 0; s < TABLE_SIZE; s++) {
						float sum = 0.f; int n = 0;
						for (int k = s - step / 2; k <= s + step / 2; k += step) {
							int idx = (k % TABLE_SIZE + TABLE_SIZE) % TABLE_SIZE;
							sum += tables[bank][f][0][idx];
							n++;
						}
						tables[bank][f][mip][s] = (n > 0) ? (sum / (float)n) : tables[bank][f][mip - 1][s];
					}
				}
			}
		}
		tableReady = true;
	}
};

// --- Linear interpolate ---
static inline float lerp(float a, float b, float t) {
	return a + (b - a) * t;
}

// --- Cubic interpolate ---
static inline float cubicHermite(float y0, float y1, float y2, float y3, float t) {
	float t2 = t * t, t3 = t2 * t;
	float m0 = (y2 - y0) * 0.5f, m1 = (y3 - y1) * 0.5f;
	return (2.f * t3 - 3.f * t2 + 1.f) * y1 + (t3 - 2.f * t2 + t) * m0 + (-2.f * t3 + 3.f * t2) * y2 + (t3 - t2) * m1;
}

struct WT_SURGE_X : Module {
	enum ParamId {
		COARSE_PARAM, FINE_PARAM, FM_AMT_PARAM,
		X_POS_PARAM, Y_POS_PARAM, XFADE_PARAM,
		WARP_A_MODE_PARAM, WARP_A_AMT_PARAM,
		WARP_B_MODE_PARAM, WARP_B_AMT_PARAM,
		UNISON_PARAM, DETUNE_PARAM, SPREAD_PARAM,
		QUALITY_PARAM, LEVEL_PARAM, PHASE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		PITCH_INPUT, FM_INPUT, WT_X_INPUT, WT_Y_INPUT, XFADE_INPUT,
		WARP_A_INPUT, WARP_B_INPUT, SYNC_INPUT,
		INPUTS_LEN
	};
	enum OutputId { OUT_L_OUTPUT, OUT_R_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	WavetableBank wavetable;
	dsp::SchmittTrigger syncTrigger;
	float phaseStore[4] = {0.f, 0.f, 0.f, 0.f};

	WT_SURGE_X() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(COARSE_PARAM, -48.f, 48.f, 0.f, "Coarse", " semitones");
		configParam(FINE_PARAM, -50.f, 50.f, 0.f, "Fine", " cents");
		configParam(FM_AMT_PARAM, -1.f, 1.f, 0.f, "FM Amount");
		configParam(X_POS_PARAM, 0.f, 1.f, 0.f, "X Position (Bank A)");
		configParam(Y_POS_PARAM, 0.f, 1.f, 0.f, "Y Position (Bank B)");
		configParam(XFADE_PARAM, 0.f, 1.f, 0.5f, "Crossfade A/B");
		configParam(WARP_A_MODE_PARAM, 0.f, 4.f, 0.f, "Warp A Mode");
		configParam(WARP_A_AMT_PARAM, 0.f, 1.f, 0.f, "Warp A Amount");
		configParam(WARP_B_MODE_PARAM, 0.f, 4.f, 0.f, "Warp B Mode");
		configParam(WARP_B_AMT_PARAM, 0.f, 1.f, 0.f, "Warp B Amount");
		configParam(UNISON_PARAM, 1.f, 4.f, 1.f, "Unison Voices");
		configParam(DETUNE_PARAM, 0.f, 1.f, 0.2f, "Detune");
		configParam(SPREAD_PARAM, 0.f, 1.f, 0.5f, "Stereo Spread");
		configParam(QUALITY_PARAM, 0.f, 2.f, 1.f, "Quality"); // 0=low, 1=med, 2=high
		configParam(LEVEL_PARAM, 0.f, 1.f, 0.8f, "Level");
		configParam(PHASE_PARAM, 0.f, 1.f, 0.f, "Phase (Sync Start)");
		configInput(PITCH_INPUT, "Pitch (1V/Oct)");
		configInput(FM_INPUT, "FM");
		configInput(WT_X_INPUT, "WT X");
		configInput(WT_Y_INPUT, "WT Y");
		configInput(XFADE_INPUT, "XFADE");
		configInput(WARP_A_INPUT, "Warp A CV");
		configInput(WARP_B_INPUT, "Warp B CV");
		configInput(SYNC_INPUT, "Sync");
		configOutput(OUT_L_OUTPUT, "Left");
		configOutput(OUT_R_OUTPUT, "Right");
		wavetable.generateDefault();
	}

	void onAdd(const AddEvent& e) override { wavetable.generateDefault(); }

	float readWavetable(int bank, float framePos, float phase, float freqHz, float sampleRate, int quality) {
		if (!wavetable.tableReady.load()) return 0.f;
		float fc = freqHz / sampleRate * TABLE_SIZE;
		int mip = 0;
		if (fc < 32.f) mip = 0;
		else if (fc < 64.f) mip = 1;
		else if (fc < 128.f) mip = 2;
		else if (fc < 256.f) mip = 3;
		else if (fc < 512.f) mip = 4;
		else if (fc < 1024.f) mip = 5;
		else if (fc < 2048.f) mip = 6;
		else if (fc < 4096.f) mip = 7;
		else if (fc < 8192.f) mip = 8;
		else mip = 9;
		if (quality == 0) mip = std::min(mip + 2, MIP_LEVELS - 1);
		float fIdx = framePos * (NUM_FRAMES - 1);
		int f0 = (int)fIdx; if (f0 < 0) f0 = 0; if (f0 >= NUM_FRAMES - 1) f0 = NUM_FRAMES - 2;
		int f1 = f0 + 1;
		float fracF = fIdx - (float)f0;
		float pos = phase * TABLE_SIZE;
		int s0 = (int)pos; float frac = pos - (float)s0;
		s0 = (s0 % TABLE_SIZE + TABLE_SIZE) % TABLE_SIZE;
		int s1 = (s0 + 1) % TABLE_SIZE;
		float v00 = wavetable.tables[bank][f0][mip][s0];
		float v01 = wavetable.tables[bank][f0][mip][s1];
		float v10 = wavetable.tables[bank][f1][mip][s0];
		float v11 = wavetable.tables[bank][f1][mip][s1];
		float v0 = lerp(v00, v01, frac);
		float v1 = lerp(v10, v11, frac);
		if (quality == 2) {
			int sm1 = (s0 - 1 + TABLE_SIZE) % TABLE_SIZE, sp2 = (s1 + 1) % TABLE_SIZE;
			v0 = cubicHermite(
				wavetable.tables[bank][f0][mip][sm1], v00, v01, wavetable.tables[bank][f0][mip][sp2], frac);
			v1 = cubicHermite(
				wavetable.tables[bank][f1][mip][sm1], v10, v11, wavetable.tables[bank][f1][mip][sp2], frac);
		}
		return lerp(v0, v1, fracF);
	}

	float applyWarp(float sample, float phase, int mode, float amount) {
		if (amount < 0.0001f) return sample;
		float warped = sample;
		switch (mode) {
			case WARP_PHASE_DISTORT: {
				float p = phase;
				float k = 0.5f + amount * 1.5f;
				p = std::pow(p, k);
				warped = std::sin(2.f * M_PI * p) * 0.9f;
				break;
			}
			case WARP_BEND_ASYM: {
				float b = 1.f + amount * 3.f;
				warped = std::tanh(sample * b);
				break;
			}
			case WARP_MIRROR: {
				float s = sample;
				if (s < 0.f) s = -s;
				if (phase > 0.5f) s = -s;
				warped = s * (1.f - amount) + sample * amount;
				break;
			}
			case WARP_FOLD: {
				float s = sample;
				while (s > 1.f) s = 2.f - s;
				while (s < -1.f) s = -2.f - s;
				warped = lerp(sample, s, amount);
				break;
			}
			case WARP_SYNC_LIKE: {
				float mult = 1.f + amount * 7.f;
				float p = phase * mult;
				p = p - std::floor(p);
				warped = std::sin(2.f * M_PI * p) * 0.9f;
				break;
			}
			default: break;
		}
		return lerp(sample, warped, amount);
	}

	float softClip(float x) {
		return x / (1.f + std::fabs(x));
	}

	void process(const ProcessArgs& args) override {
		if (!wavetable.tableReady.load()) {
			outputs[OUT_L_OUTPUT].setVoltage(0.f);
			outputs[OUT_R_OUTPUT].setVoltage(0.f);
			return;
		}
		float pitchV = inputs[PITCH_INPUT].getVoltage();
		if (!inputs[PITCH_INPUT].isConnected()) pitchV = 0.f;
		float coarse = params[COARSE_PARAM].getValue();
		float fine = params[FINE_PARAM].getValue() / 100.f;
		float fmAmt = params[FM_AMT_PARAM].getValue();
		float fm = 0.f;
		if (inputs[FM_INPUT].isConnected())
			fm = inputs[FM_INPUT].getVoltage() * fmAmt * 12.f;
		float pitch = pitchV + coarse + fine + fm;
		float freqHz = dsp::FREQ_C4 * std::pow(2.f, pitch / 12.f);
		freqHz = math::clamp(freqHz, 1.f, 20000.f);

		float xPos = params[X_POS_PARAM].getValue();
		if (inputs[WT_X_INPUT].isConnected())
			xPos += inputs[WT_X_INPUT].getVoltage() / 10.f * 0.5f;
		xPos = math::clamp(xPos, 0.f, 1.f);

		float yPos = params[Y_POS_PARAM].getValue();
		if (inputs[WT_Y_INPUT].isConnected())
			yPos += inputs[WT_Y_INPUT].getVoltage() / 10.f * 0.5f;
		yPos = math::clamp(yPos, 0.f, 1.f);

		float xfade = params[XFADE_PARAM].getValue();
		if (inputs[XFADE_INPUT].isConnected())
			xfade += inputs[XFADE_INPUT].getVoltage() / 10.f * 0.5f;
		xfade = math::clamp(xfade, 0.f, 1.f);

		int warpAMode = (int)std::round(params[WARP_A_MODE_PARAM].getValue());
		float warpAAmt = params[WARP_A_AMT_PARAM].getValue();
		if (inputs[WARP_A_INPUT].isConnected())
			warpAAmt += inputs[WARP_A_INPUT].getVoltage() / 10.f * 0.5f;
		warpAAmt = math::clamp(warpAAmt, 0.f, 1.f);

		int warpBMode = (int)std::round(params[WARP_B_MODE_PARAM].getValue());
		float warpBAmt = params[WARP_B_AMT_PARAM].getValue();
		if (inputs[WARP_B_INPUT].isConnected())
			warpBAmt += inputs[WARP_B_INPUT].getVoltage() / 10.f * 0.5f;
		warpBAmt = math::clamp(warpBAmt, 0.f, 1.f);

		int voices = (int)std::round(params[UNISON_PARAM].getValue());
		voices = (int)math::clamp((float)voices, 1.f, 4.f);
		float detune = params[DETUNE_PARAM].getValue() * 30.f / 1200.f;
		float spread = params[SPREAD_PARAM].getValue();
		int quality = (int)std::round(params[QUALITY_PARAM].getValue());
		quality = (int)math::clamp((float)quality, 0.f, 2.f);
		float level = params[LEVEL_PARAM].getValue();
		float phaseStart = params[PHASE_PARAM].getValue();

		bool sync = syncTrigger.process(rescale(inputs[SYNC_INPUT].getVoltage(), 0.1f, 2.f, 0.f, 1.f));

		float outL = 0.f, outR = 0.f;
		float invVoices = 1.f / (float)voices;

		for (int v = 0; v < voices; v++) {
			float detuneCents = (v - (voices - 1) * 0.5f) * detune * 30.f;
			float vFreq = freqHz * std::pow(2.f, detuneCents / 12.f);
			float inc = vFreq * args.sampleTime;

			if (sync) phaseStore[v] = phaseStart;
			phaseStore[v] += inc;
			if (phaseStore[v] >= 1.f) phaseStore[v] -= 1.f;
			if (phaseStore[v] < 0.f) phaseStore[v] += 1.f;
			float phase = phaseStore[v];

			float sA = readWavetable(0, xPos, phase, vFreq, args.sampleRate, quality);
			float sB = readWavetable(1, yPos, phase, vFreq, args.sampleRate, quality);
			float base = lerp(sA, sB, xfade);
			base = applyWarp(base, phase, warpAMode, warpAAmt);
			base = applyWarp(base, phase, warpBMode, warpBAmt);

			float pan = (voices > 1) ? (v / (float)(voices - 1) - 0.5f) * 2.f * spread : 0.f;
			float gL = (1.f - pan) * invVoices;
			float gR = (1.f + pan) * invVoices;
			outL += base * gL;
			outR += base * gR;
		}

		outL = softClip(outL * level * 5.f);
		outR = softClip(outR * level * 5.f);
		outputs[OUT_L_OUTPUT].setVoltage(outL);
		outputs[OUT_R_OUTPUT].setVoltage(outR);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		return root;
	}

	void dataFromJson(json_t* root) override { (void)root; }
};

struct WT_SURGE_XWidget : ModuleWidget {
	WT_SURGE_XWidget(WT_SURGE_X* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WT_SURGE_X.svg")));

		float px = 5.08f;
		float w = 22.f * px;  // 22HP = 111.76mm

		addChild(createWidget<ScrewSilver>(Vec(px, 0)));
		addChild(createWidget<ScrewSilver>(Vec(w - 2 * px, 0)));
		addChild(createWidget<ScrewSilver>(Vec(px, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(w - 2 * px, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// 22HP 等分布局：边距 8mm，可用宽度 ~96mm
		// 6 列等分：8 + 96/7*(1,2,3,4,5,6) ≈ 22, 36, 49, 63, 77, 90
		float x6[6] = { 22.f, 36.f, 50.f, 64.f, 78.f, 92.f };
		// 5 列等分：8 + 96/6*(1,2,3,4,5) ≈ 24, 40, 56, 72, 88
		float x5[5] = { 24.f, 40.f, 56.f, 72.f, 88.f };
		// 2 列等分：38, 74
		float x2[2] = { 38.f, 74.f };

		// A) Pitch / FM / Sync（6 项）
		float row1 = 26.f;
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x6[0], row1)), module, WT_SURGE_X::PITCH_INPUT));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x6[1], row1)), module, WT_SURGE_X::COARSE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x6[2], row1)), module, WT_SURGE_X::FINE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x6[3], row1)), module, WT_SURGE_X::FM_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(x6[4], row1)), module, WT_SURGE_X::FM_AMT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x6[5], row1)), module, WT_SURGE_X::SYNC_INPUT));

		// B) Wavetable 双 Bank（6 项）
		float row2 = 50.f;
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(x6[0], row2)), module, WT_SURGE_X::X_POS_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x6[1], row2)), module, WT_SURGE_X::WT_X_INPUT));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(x6[2], row2)), module, WT_SURGE_X::Y_POS_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x6[3], row2)), module, WT_SURGE_X::WT_Y_INPUT));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(x6[4], row2)), module, WT_SURGE_X::XFADE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x6[5], row2)), module, WT_SURGE_X::XFADE_INPUT));

		// C) Warp（6 项）
		float row3 = 78.f;
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x6[0], row3)), module, WT_SURGE_X::WARP_A_MODE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x6[1], row3)), module, WT_SURGE_X::WARP_A_AMT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x6[2], row3)), module, WT_SURGE_X::WARP_A_INPUT));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x6[3], row3)), module, WT_SURGE_X::WARP_B_MODE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x6[4], row3)), module, WT_SURGE_X::WARP_B_AMT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x6[5], row3)), module, WT_SURGE_X::WARP_B_INPUT));

		// Unison / Output（5 项）
		float row4 = 102.f;
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x5[0], row4)), module, WT_SURGE_X::UNISON_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x5[1], row4)), module, WT_SURGE_X::DETUNE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x5[2], row4)), module, WT_SURGE_X::SPREAD_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x5[3], row4)), module, WT_SURGE_X::QUALITY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x5[4], row4)), module, WT_SURGE_X::LEVEL_PARAM));

		float row5 = 115.f;
		addParam(createParamCentered<Trimpot>(mm2px(Vec(x6[0], row5)), module, WT_SURGE_X::PHASE_PARAM));

		float row6 = 118.f;
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(x2[0], row6)), module, WT_SURGE_X::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(x2[1], row6)), module, WT_SURGE_X::OUT_R_OUTPUT));
	}

	struct LoadWavMenuItem : ui::MenuItem {
		WT_SURGE_X* mod;
		int bank;
		void onAction(const ActionEvent& e) override {
			osdialog_filters* filters = osdialog_filters_parse("WAV files:wav");
			char* pathC = osdialog_file(OSDIALOG_OPEN, NULL, NULL, filters);
			if (filters) osdialog_filters_free(filters);
			if (pathC) {
				std::string path = pathC;
				std::free(pathC);
				mod->wavetable.loadWav(bank, path);
			}
		}
	};

	void appendContextMenu(Menu* menu) override {
		WT_SURGE_X* m = dynamic_cast<WT_SURGE_X*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		LoadWavMenuItem* loadA = createMenuItem<LoadWavMenuItem>("Load WAV → Bank A");
		loadA->mod = m;
		loadA->bank = 0;
		menu->addChild(loadA);
		LoadWavMenuItem* loadB = createMenuItem<LoadWavMenuItem>("Load WAV → Bank B");
		loadB->mod = m;
		loadB->bank = 1;
		menu->addChild(loadB);
	}
};

Model* modelWT_SURGE_X = createModel<WT_SURGE_X, WT_SURGE_XWidget>("WT_SURGE_X");
