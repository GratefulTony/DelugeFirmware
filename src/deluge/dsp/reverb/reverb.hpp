#pragma once
#include "base.hpp"
#include "deluge/dsp/reverb/reverb.hpp"
#include "digital.hpp"
#include "featherverb.hpp"
#include "freeverb/freeverb.hpp"
#include "mutable.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <variant>

namespace deluge::dsp {

class [[gnu::hot]] Reverb : reverb::Base {
public:
	enum class Model {
		FEATHERVERB = 0, // Lightweight FDN reverb (default)
		FREEVERB,
		MUTABLE,
		DIGITAL,
	};

	Reverb()
	    : base_(&std::get<0>(reverb_)),     //<
	      room_size_(base_->getRoomSize()), //<
	      damping_(base_->getDamping()),    //<
	      lpf_(base_->getLPF()),            //<
	      width_(base_->getWidth()) {
		// Note: allocate() must be called after memory allocator is initialized
		// This is done in AudioEngine::init()
	}
	~Reverb() override = default;

	// Allocate reverb buffer - call after construction or setModel()
	// Returns false if allocation fails (out of memory)
	[[nodiscard]] bool allocate() {
		using namespace reverb;
		switch (model_) {
		case Model::FEATHERVERB:
			return reverb_as<Featherverb>().allocate();
		case Model::FREEVERB:
			return reverb_as<Freeverb>().allocate();
		case Model::MUTABLE:
			return reverb_as<Mutable>().allocate();
		case Model::DIGITAL:
			return reverb_as<Digital>().allocate();
		}
		return false;
	}

	[[nodiscard]] bool isAllocated() const {
		// Use std::visit to safely check whichever variant is active
		// (avoids race condition during model switch where model_ doesn't match variant)
		return std::visit([](const auto& r) { return r.isAllocated(); }, reverb_);
	}

	// Request model change - actual switch happens in audio thread via process()
	// TODO: Very rapid model switching can still cause crashes - consider adding
	// a cooldown/debounce or mutex if this becomes a user-facing issue
	void setModel(Model m) { pendingModel_.store(static_cast<int8_t>(m), std::memory_order_release); }

	Model getModel() { return model_; }

	void process(std::span<int32_t> input, StereoBuffer<q31_t> output) override {
		// Check for pending model change and apply it (in audio thread context)
		int8_t pending = pendingModel_.load(std::memory_order_acquire);
		if (pending >= 0) {
			applyModelChange(static_cast<Model>(pending));
			pendingModel_.store(-1, std::memory_order_release);
		}

		// Use std::visit to safely process whichever variant is active
		// Each model's process() has its own allocation check for safety
		std::visit([&](auto& r) { r.process(input, output); }, reverb_);
	}

private:
	// Actually perform the model switch - called from audio thread only
	void applyModelChange(Model m) {
		using namespace reverb;

		// Deallocate current model's buffer
		switch (model_) {
		case Model::FEATHERVERB:
			reverb_as<Featherverb>().deallocate();
			break;
		case Model::FREEVERB:
			reverb_as<Freeverb>().deallocate();
			break;
		case Model::MUTABLE:
			reverb_as<Mutable>().deallocate();
			break;
		case Model::DIGITAL:
			reverb_as<Digital>().deallocate();
			break;
		}

		// Emplace new model and allocate its buffer
		switch (m) {
		case Model::FEATHERVERB:
			reverb_.emplace<Featherverb>();
			base_ = &std::get<Featherverb>(reverb_);
			(void)std::get<Featherverb>(reverb_).allocate();
			break;
		case Model::FREEVERB:
			reverb_.emplace<Freeverb>();
			base_ = &std::get<Freeverb>(reverb_);
			(void)std::get<Freeverb>(reverb_).allocate();
			break;
		case Model::DIGITAL:
			reverb_.emplace<Digital>();
			base_ = &std::get<Digital>(reverb_);
			(void)std::get<Digital>(reverb_).allocate();
			break;
		case Model::MUTABLE:
			reverb_.emplace<Mutable>();
			base_ = &std::get<Mutable>(reverb_);
			(void)std::get<Mutable>(reverb_).allocate();
			break;
		}
		base_->setRoomSize(room_size_);
		base_->setDamping(damping_);
		base_->setWidth(width_);
		base_->setHPF(hpf_);
		base_->setLPF(lpf_);
		base_->setPanLevels(panLeft_, panRight_);
		model_ = m;
	}

public:
	void setPanLevels(const int32_t amplitude_left, const int32_t amplitude_right) {
		panLeft_ = amplitude_left;
		panRight_ = amplitude_right;
		base_->setPanLevels(amplitude_left, amplitude_right);
	}

	void setRoomSize(float value) override {
		room_size_ = value;
		base_->setRoomSize(value);
	}

	[[nodiscard]] float getRoomSize() const override { return base_->getRoomSize(); };

	void setDamping(float value) override {
		damping_ = value;
		base_->setDamping(value);
	}

	[[nodiscard]] float getDamping() const override { return base_->getDamping(); }

	void setWidth(float value) override {
		width_ = value;
		base_->setWidth(value);
	}

	[[nodiscard]] float getWidth() const override { return base_->getWidth(); };

	void setHPF(float f) override {
		hpf_ = f;
		base_->setHPF(f);
	}
	[[nodiscard]] float getHPF() const override { return base_->getHPF(); }

	void setLPF(float f) override {
		lpf_ = f;
		base_->setLPF(f);
	}
	[[nodiscard]] float getLPF() const override { return base_->getLPF(); }

	// === Featherverb-specific parameters (only valid when model == FEATHERVERB) ===

	void setFeatherZone1(int32_t value) {
		if (model_ == Model::FEATHERVERB) {
			reverb_as<reverb::Featherverb>().setZone1(value);
		}
	}
	[[nodiscard]] int32_t getFeatherZone1() const {
		if (model_ == Model::FEATHERVERB) {
			return std::get<reverb::Featherverb>(reverb_).getZone1();
		}
		return 0;
	}

	void setFeatherZone2(int32_t value) {
		if (model_ == Model::FEATHERVERB) {
			reverb_as<reverb::Featherverb>().setZone2(value);
		}
	}
	[[nodiscard]] int32_t getFeatherZone2() const {
		if (model_ == Model::FEATHERVERB) {
			return std::get<reverb::Featherverb>(reverb_).getZone2();
		}
		return 0;
	}

	void setFeatherZone3(int32_t value) {
		if (model_ == Model::FEATHERVERB) {
			reverb_as<reverb::Featherverb>().setZone3(value);
		}
	}
	[[nodiscard]] int32_t getFeatherZone3() const {
		if (model_ == Model::FEATHERVERB) {
			return std::get<reverb::Featherverb>(reverb_).getZone3();
		}
		return 0;
	}

	void setFeatherPredelay(float value) {
		if (model_ == Model::FEATHERVERB) {
			reverb_as<reverb::Featherverb>().setPredelay(value);
		}
	}
	[[nodiscard]] float getFeatherPredelay() const {
		if (model_ == Model::FEATHERVERB) {
			return std::get<reverb::Featherverb>(reverb_).getPredelay();
		}
		return 0.0f;
	}

	template <typename T>
	constexpr T& reverb_as() {
		return std::get<T>(reverb_);
	}

	std::variant<            //<
	    reverb::Featherverb, //<
	    reverb::Freeverb,    //<
	    reverb::Mutable,     //<
	    reverb::Digital      //<
	    >
	    reverb_{};

	Model model_ = Model::FEATHERVERB;

	reverb::Base* base_ = nullptr;

	// Pending model change (-1 = none, >=0 = requested model)
	// UI sets this, audio thread applies it to avoid race conditions
	std::atomic<int8_t> pendingModel_{-1};

	float room_size_;
	float damping_;
	float width_;
	float hpf_;
	float lpf_;
	int32_t panLeft_{0};
	int32_t panRight_{0};
};
} // namespace deluge::dsp
