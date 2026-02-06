#include "plugin.hpp"
#include <vector>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstring>
#include <ui/Menu.hpp>
#include <system.hpp>
#include <osdialog.h>

/**
 * Organic Particle Synth (有机粒子合成器)
 * 基于 Aetheria 参考实现，支持颗粒合成和外部音频文件加载
 */

// 简单的 WAV 文件读取器（支持 16-bit PCM）
struct WavReader {
	static bool loadWavFile(const std::string& path, std::vector<float>& buffer, int& sampleRate) {
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open()) return false;
		
		// 读取 RIFF 头
		char riff[4];
		file.read(riff, 4);
		if (strncmp(riff, "RIFF", 4) != 0) {
			file.close();
			return false;
		}
		
		// 跳过文件大小
		file.seekg(4, std::ios::cur);
		
		// 读取 WAVE 标识
		char wave[4];
		file.read(wave, 4);
		if (strncmp(wave, "WAVE", 4) != 0) {
			file.close();
			return false;
		}
		
		// 查找 fmt 块
		short audioFormat = 0;
		short numChannels = 0;
		short bitsPerSample = 0;
		bool foundFmt = false;
		
		while (file.good()) {
			char chunkId[4];
			if (!file.read(chunkId, 4)) break;
			
			int chunkSize = 0;
			file.read((char*)&chunkSize, 4);
			
			if (strncmp(chunkId, "fmt ", 4) == 0) {
				file.read((char*)&audioFormat, 2);
				file.read((char*)&numChannels, 2);
				file.read((char*)&sampleRate, 4);
				file.seekg(6, std::ios::cur); // 跳过 byte rate 和 block align
				file.read((char*)&bitsPerSample, 2);
				file.seekg(chunkSize - 16, std::ios::cur); // 跳过剩余的 fmt 数据
				foundFmt = true;
				break;
			} else {
				file.seekg(chunkSize, std::ios::cur);
			}
		}
		
		if (!foundFmt || audioFormat != 1) {
			file.close();
			return false; // 只支持 PCM
		}
		
		// 查找 data 块
		int dataSize = 0;
		bool foundData = false;
		
		while (file.good()) {
			char chunkId[4];
			if (!file.read(chunkId, 4)) break;
			
			file.read((char*)&dataSize, 4);
			
			if (strncmp(chunkId, "data", 4) == 0) {
				foundData = true;
				break;
			} else {
				file.seekg(dataSize, std::ios::cur);
			}
		}
		
		if (!foundData) {
			file.close();
			return false;
		}
		
		// 读取音频数据
		if (bitsPerSample == 16) {
			int numSamples = dataSize / 2 / numChannels;
			buffer.resize(numSamples);
			
			short* samples = new short[numSamples * numChannels];
			file.read((char*)samples, dataSize);
			
			// 转换为浮点数并混合多声道
			for (int i = 0; i < numSamples; i++) {
				float sum = 0.f;
				for (int ch = 0; ch < numChannels; ch++) {
					sum += (float)samples[i * numChannels + ch] / 32768.f;
				}
				buffer[i] = sum / numChannels;
			}
			delete[] samples;
		} else {
			file.close();
			return false; // 只支持 16-bit
		}
		
		file.close();
		return true;
	}
};

struct OrganicParticleSynth : Module {
	enum ParamId {
		VITALITY_PARAM,       // 活力/随机抖动
		PITCH_PARAM,          // 音高/速度 (0.5-2.0)
		GRAIN_SIZE_PARAM,     // 颗粒大小
		DENSITY_PARAM,        // 颗粒密度
		CUTOFF_PARAM,         // 低通滤波器截止频率
		RESONANCE_PARAM,      // 低通滤波器共振
		BPM_PARAM,            // BPM 主控
		VOLUME_PARAM,         // 输出音量
		IS432HZ_PARAM,        // 432Hz 调音按钮
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,          // 外部时钟输入
		VITALITY_CV_INPUT,    // Vitality CV 输入
		INPUTS_LEN
	};
	enum OutputId {
		L_OUTPUT,             // 左声道输出
		R_OUTPUT,             // 右声道输出
		OUTPUTS_LEN
	};
	enum LightId {
		SAMPLE_LOADED_LIGHT,  // 采样已加载指示灯
		IS432HZ_LIGHT,        // 432Hz 调音指示灯
		LIGHTS_LEN
	};

	// 颗粒合成状态
	struct Grain {
		bool active = false;
		float phase = 0.f;        // 在采样中的位置（采样索引）
		float duration = 0.f;      // 颗粒持续时间
		float elapsed = 0.f;       // 已播放时间
		float startPos = 0.f;      // 采样起始位置（秒）
		float playbackRate = 1.f;   // 播放速度
		float envelope = 0.f;      // 包络值
	};

	static constexpr int MAX_GRAINS = 32;
	Grain grains[MAX_GRAINS];
	
	// 采样缓冲区
	std::vector<float> sampleBuffer;
	float sampleDuration = 0.f;    // 采样时长（秒）
	int sampleBufferSize = 0;
	int sampleRate = 44100;
	bool sampleLoaded = false;
	std::string samplePath;
	
	// 颗粒调度器
	float grainTimer = 0.f;

	// 滤波器
	dsp::BiquadFilter filter;

	// 触发器
	dsp::SchmittTrigger clockTrigger;

	OrganicParticleSynth() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		
		configParam(VITALITY_PARAM, 0.f, 1.f, 0.3f, "Vitality");
		configParam(PITCH_PARAM, 0.5f, 2.0f, 1.0f, "Pitch/Speed");
		configParam(GRAIN_SIZE_PARAM, 0.02f, 0.5f, 0.12f, "Grain Size", " s");
		configParam(DENSITY_PARAM, 0.f, 1.f, 0.4f, "Density");
		configParam(CUTOFF_PARAM, 0.f, 1.f, 0.4f, "LPF Cutoff");
		configParam(RESONANCE_PARAM, 0.f, 1.f, 0.15f, "LPF Resonance");
		configParam(BPM_PARAM, 40.f, 200.f, 88.f, "BPM");
		configParam(VOLUME_PARAM, 0.f, 1.f, 0.5f, "Volume");
		// 432Hz：0=标准调音，1=432 调音，默认 1（开启）；用开关保持状态
		configSwitch(IS432HZ_PARAM, 0.f, 1.f, 1.f, "432Hz Tuning");

		configInput(CLOCK_INPUT, "Clock");
		configInput(VITALITY_CV_INPUT, "Vitality CV");

		configOutput(L_OUTPUT, "Left");
		configOutput(R_OUTPUT, "Right");

		// 初始化默认采样缓冲区
		onSampleRateChange();
	}

	void onSampleRateChange() override {
		// 如果没有加载外部采样，生成默认采样
		if (!sampleLoaded) {
			float sampleRate = APP->engine->getSampleRate();
			sampleDuration = 2.0f;
			sampleBufferSize = (int)(sampleDuration * sampleRate);
			sampleBuffer.resize(sampleBufferSize, 0.f);
			
			// 生成一个复杂的采样（使用可听频率）
			for (int i = 0; i < sampleBufferSize; i++) {
				float t = (float)i / sampleRate; // 使用实际时间（秒）
				// 生成一个衰减的正弦波，带有谐波（使用可听频率）
				float sample = 0.f;
				sample += std::sin(2.f * M_PI * 110.f * t) * (1.f - t * 0.5f); // 110Hz 基频
				sample += std::sin(2.f * M_PI * 220.f * t) * 0.3f * (1.f - t * 0.5f); // 二次谐波
				sample += std::sin(2.f * M_PI * 330.f * t) * 0.2f * (1.f - t * 0.5f); // 三次谐波
				sampleBuffer[i] = sample * 0.5f; // 归一化
			}
			this->sampleRate = (int)sampleRate;
		}
	}

	void loadSampleFile(const std::string& path) {
		std::vector<float> newBuffer;
		int newSampleRate = 44100;
		
		if (WavReader::loadWavFile(path, newBuffer, newSampleRate)) {
			sampleBuffer = newBuffer;
			sampleBufferSize = (int)newBuffer.size();
			sampleRate = newSampleRate;
			sampleDuration = (float)sampleBufferSize / sampleRate;
			sampleLoaded = true;
			samplePath = path;
		} else {
			// 加载失败，使用默认采样
			sampleLoaded = false;
			onSampleRateChange();
		}
	}

	void triggerGrain(float time, float grainSize, float pitch, float vitality) {
		if (sampleBufferSize == 0) return;

		// 查找空闲的颗粒
		for (int i = 0; i < MAX_GRAINS; i++) {
			if (!grains[i].active) {
				grains[i].active = true;
				grains[i].elapsed = 0.f;
				grains[i].duration = grainSize;
				grains[i].playbackRate = pitch;
				
				// 计算起始位置：基础偏移 + Vitality 带来的抖动
				float baseOffset = 0.1f * sampleDuration;
				float jitter = vitality * sampleDuration * 0.4f;
				float startPos = baseOffset + (random::uniform() - 0.5f) * jitter;
				startPos = clamp(startPos, 0.f, sampleDuration - grainSize);
				grains[i].startPos = startPos;
				// 将时间位置转换为采样索引
				grains[i].phase = startPos * sampleBufferSize / sampleDuration;
				grains[i].envelope = 0.f;
				break;
			}
		}
	}

	float processGrain(Grain& grain, float dt) {
		if (!grain.active) return 0.f;

		grain.elapsed += dt;
		
		// 包络：攻击和释放
		float attack = grain.duration * 0.1f;
		float release = grain.duration * 0.4f;
		
		if (grain.elapsed < attack) {
			grain.envelope = grain.elapsed / attack;
		} else if (grain.elapsed < grain.duration - release) {
			grain.envelope = 1.f;
		} else {
			float releaseTime = grain.elapsed - (grain.duration - release);
			grain.envelope = 1.f - (releaseTime / release);
		}

		// 更新相位（基于采样索引，考虑播放速度）
		float phaseIncrement = dt * grain.playbackRate * sampleBufferSize / sampleDuration;
		grain.phase += phaseIncrement;
		
		// 检查是否超出采样范围或持续时间
		if (grain.phase >= sampleBufferSize || grain.elapsed >= grain.duration) {
			grain.active = false;
			return 0.f;
		}

		// 从采样缓冲区读取（线性插值）
		int idx0 = (int)grain.phase;
		int idx1 = idx0 + 1;
		float frac = grain.phase - idx0;
		
		if (idx1 >= sampleBufferSize) {
			grain.active = false;
			return 0.f;
		}

		float sample = sampleBuffer[idx0] * (1.f - frac) + sampleBuffer[idx1] * frac;
		return sample * grain.envelope;
	}

	void process(const ProcessArgs& args) override {
		// 加载采样按钮（通过菜单触发，这里不需要处理）

		// 432Hz 状态由开关参数直接表示（1=432 调音，0=标准调音）
		bool is432Hz = params[IS432HZ_PARAM].getValue() >= 0.5f;

		// 获取参数值
		float vitality = params[VITALITY_PARAM].getValue();
		if (inputs[VITALITY_CV_INPUT].isConnected()) {
			vitality += inputs[VITALITY_CV_INPUT].getVoltage() / 10.f;
			vitality = clamp(vitality, 0.f, 1.f);
		}

		float pitch = params[PITCH_PARAM].getValue();
		float grainSize = params[GRAIN_SIZE_PARAM].getValue();
		float density = params[DENSITY_PARAM].getValue();
		float cutoff = params[CUTOFF_PARAM].getValue();
		float resonance = params[RESONANCE_PARAM].getValue();
		float bpm = params[BPM_PARAM].getValue();
		float volume = params[VOLUME_PARAM].getValue();

		// 颗粒合成模式
		// 使用连续调度算法（参考 Aetheria 实现）
		float beatSec = 60.f / bpm;
		float densityFactor = 1.f + density * 12.f;
		float grainInterval = beatSec / densityFactor;
		
		// 检查外部时钟输入
		if (inputs[CLOCK_INPUT].isConnected()) {
			if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage())) {
				// 外部时钟触发时，重置内部定时器并确保至少触发一个颗粒
				grainTimer = 0.f;
				triggerGrain(0.f, grainSize, pitch, vitality);
			}
			// 即使有外部时钟，也使用内部调度器来增加密度
			grainTimer += args.sampleTime;
			while (grainTimer >= grainInterval) {
				grainTimer -= grainInterval;
				// 使用密度作为触发概率
				if (random::uniform() < density) {
					triggerGrain(0.f, grainSize, pitch, vitality);
				}
			}
		} else {
			// 仅使用内部 BPM 同步
			grainTimer += args.sampleTime;
			while (grainTimer >= grainInterval) {
				grainTimer -= grainInterval;
				// 使用密度作为触发概率
				if (random::uniform() < density) {
					triggerGrain(0.f, grainSize, pitch, vitality);
				}
			}
		}

		// 处理所有活跃的颗粒
		float output = 0.f;
		for (int i = 0; i < MAX_GRAINS; i++) {
			if (grains[i].active) {
				output += processGrain(grains[i], args.sampleTime);
			}
		}
		// 增加输出增益，确保有足够的音量
		output *= 2.0f;

		// 应用低通滤波器
		// 截止频率映射：与参考代码一致，使用 40 * 400^cutoff
		float cutoffFreq = 40.f * std::pow(400.f, cutoff);
		float q = resonance * 20.f + vitality * 5.f;
		filter.setParameters(dsp::BiquadFilter::LOWPASS, 
			clamp(cutoffFreq / args.sampleRate, 0.f, 0.45f), 
			clamp(q, 0.1f, 20.f), 
			1.0f);
		output = filter.process(output);

		// 应用音量
		output *= volume * 5.f; // 5V 输出范围

		// 输出到左右声道（单声道）
		outputs[L_OUTPUT].setVoltage(output);
		outputs[R_OUTPUT].setVoltage(output);
		
		// 更新指示灯
		lights[SAMPLE_LOADED_LIGHT].setBrightness(sampleLoaded ? 1.f : 0.f);
		lights[IS432HZ_LIGHT].setBrightness(is432Hz ? 1.f : 0.f);
	}
};

// 432 调音开关：点击保持状态，再点击切换（非瞬时按钮）
struct Toggle432Button : app::SvgSwitch {
	Toggle432Button() {
		momentary = false;
		latch = true;
		shadow->opacity = 0.0;
		addFrame(Svg::load(asset::system("res/ComponentLibrary/TL1105_0.svg")));
		addFrame(Svg::load(asset::system("res/ComponentLibrary/TL1105_1.svg")));
	}
};

struct LoadSampleMenuItem : ui::MenuItem {
	OrganicParticleSynth* module;
	
	void onAction(const ActionEvent& e) override {
		// 使用 osdialog 打开文件选择对话框（只支持 WAV 格式）
		osdialog_filters* filters = osdialog_filters_parse("WAV files:wav");
		char* pathC = osdialog_file(OSDIALOG_OPEN, NULL, NULL, filters);
		if (filters) {
			osdialog_filters_free(filters);
		}
		if (pathC) {
			std::string path = pathC;
			std::free(pathC);
			
			// 检查文件扩展名
			std::string ext = system::getExtension(path);
			if (ext != ".wav" && ext != ".WAV") {
				// 显示错误消息
				osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK, "只支持 WAV 格式文件。\n请使用未压缩的 WAV 文件（16-bit PCM）。");
				return;
			}
			
			module->loadSampleFile(path);
		}
	}
};

struct OrganicParticleSynthWidget : ModuleWidget {
	// 组件垂直偏移量（毫米），正值使所有组件上移
	static constexpr float VERTICAL_OFFSET_MM = 10.f;

	OrganicParticleSynthWidget(OrganicParticleSynth* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/OrganicParticleSynth.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// 第一行：Vitality、Pitch、采样加载指示灯（右键菜单加载文件）
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(9, 38 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::VITALITY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(22, 38 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::PITCH_PARAM));
		addChild(createLightCentered<SmallSimpleLight<GreenLight>>(mm2px(Vec(30.96, 38 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::SAMPLE_LOADED_LIGHT));

		// 第二行：Grain Size、Density
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(9, 59 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::GRAIN_SIZE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(22, 59 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::DENSITY_PARAM));

		// 第三行：Cutoff、Resonance
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(9, 80 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::CUTOFF_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(22, 80 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::RESONANCE_PARAM));

		// 第四行：BPM、Volume
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(9, 101 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::BPM_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(22, 101 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::VOLUME_PARAM));

		// 432Hz 指示灯（在按钮上方）
		addChild(createLightCentered<SmallSimpleLight<GreenLight>>(mm2px(Vec(30.96, 91 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::IS432HZ_LIGHT));
		// 432Hz 开关（点击保持，再点击取消）
		addParam(createParamCentered<Toggle432Button>(mm2px(Vec(30.96, 101 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::IS432HZ_PARAM));

		// 输入
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(9, 115 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22, 115 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::VITALITY_CV_INPUT));

		// 输出
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(9, 128 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::L_OUTPUT));
		addOutput(createOutputCentered<PJ3410Port>(mm2px(Vec(22, 128 - VERTICAL_OFFSET_MM)), module, OrganicParticleSynth::R_OUTPUT));
	}
	
	void appendContextMenu(ui::Menu* menu) override {
		OrganicParticleSynth* module = dynamic_cast<OrganicParticleSynth*>(this->module);
		assert(module);
		
		menu->addChild(new ui::MenuSeparator);
		
		LoadSampleMenuItem* loadItem = createMenuItem<LoadSampleMenuItem>("Load Sample File");
		loadItem->module = module;
		menu->addChild(loadItem);
	}
};

Model* modelOrganicParticleSynth = createModel<OrganicParticleSynth, OrganicParticleSynthWidget>("OrganicParticleSynth");
