#include "hid/hid_sysex.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "gui/ui_timer_manager.h"
#include "hid/display/oled.h"
#include "hid/display/seven_segment.h"
#include "hid/led/indicator_leds.h"
#include "hid/led/pad_leds.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "io/midi/sysex.h"
#include "memory/general_memory_allocator.h"
#include "processing/engines/audio_engine.h"
#include "util/pack.h"
#include <cstring>

MIDICable* midiDisplayCable = nullptr;
int32_t midiDisplayUntil = 0;
uint8_t* oledDeltaImage = nullptr;
bool oledDeltaForce = true;

#if ENABLE_PANEL_STREAM
namespace indicator_leds {
extern bool ledStates[];
}

MIDICable* panelStateCable = nullptr;
int32_t panelStateUntil = 0;
RGB panelPadsLastSent[kDisplayHeight][kDisplayWidth + kSideBarWidth];
bool panelPadsSendAll = true;
bool panelPadsPending = false;
#endif

void HIDSysex::sysexReceived(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 3) {
		return;
	}
	// first three bytes are already used, next is command
	switch (data[1]) {
	case 0:
		requestOLEDDisplay(cable, data, len);
		break;

	case 1:
		request7SegDisplay(cable, data, len);
		break;

	case 2:
		readBlock(cable);
		break;

	case 3:
		requestPanelState(cable, data, len);
		break;

	default:
		break;
	}
}

void HIDSysex::requestOLEDDisplay(MIDICable& cable, uint8_t* data, int32_t len) {
	//	if (data[4] == 0 or data[4] == 1) {
	if (data[2] == 0 or data[2] == 1) {
		// sendOLEDData(device, (data[4] == 1)); // Adjustting by -2 to correct for payload offset.
		sendOLEDData(cable, (data[2] == 1));
	}
	// else if (data[4] == 2 || data[4] == 3) {
	else if (data[2] == 2 || data[2] == 3) {
		// bool force = (data[4] == 3);
		bool force = (data[2] == 3);
		midiDisplayCable = &cable;
		// two seconds
		midiDisplayUntil = AudioEngine::audioSampleTimer + 2 * kSampleRate;
		if (display->haveOLED()) {
			if (force) {
				oledDeltaForce = true;
			}

			if (oledDeltaImage == nullptr) {
				oledDeltaImage = (uint8_t*)GeneralMemoryAllocator::get().allocMaxSpeed(
				    sizeof(uint8_t[OLED_MAIN_HEIGHT_PIXELS >> 3][OLED_MAIN_WIDTH_PIXELS]));
			}
		}
		sendDisplayIfChanged();
		if (force && display->have7SEG()) {
			send7SegData(cable);
		}
	}
	// else if (data[4] == 4) { // SWAP
	else if (data[2] == 4) { // SWAP
		deluge::hid::display::swapDisplayType();
		oledDeltaForce = true;
	}
}

void HIDSysex::sendDisplayIfChanged() {
	// NB: timer is only used for throttling, under good conditions sending
	// is driven by the display subsystem only
	uiTimerManager.unsetTimer(TimerName::SYSEX_DISPLAY);
	if (midiDisplayCable == nullptr || AudioEngine::audioSampleTimer > midiDisplayUntil) {
		return;
	}
	// not exact, but if more than half than the serial buffer is still full,
	// we need to slow down a little. (USB buffer is larger and should be consumed much quicker)
	if (midiDisplayCable->sendBufferSpace() < 512) {
		uiTimerManager.setTimer(TimerName::SYSEX_DISPLAY, 100);
		return;
	}

	if (display->haveOLED()) {
		sendOLEDDataDelta(*midiDisplayCable, false);
	}
	if (display->have7SEG()) {
		send7SegData(*midiDisplayCable);
	}
}

void HIDSysex::sendOLEDData(MIDICable& cable, bool rle) {
	if (display->haveOLED()) {
		const int32_t data_size = 768;
		const int32_t max_packed_size = 922;
		// 		uint8_t reply_hdr[5] = {0xf0, 0x7d, 0x02, 0x40, rle ? 0x01_u8 : 0x00_u8};
		uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x40, rle ? 0x01_u8 : 0x00_u8};
		uint8_t* reply = midiEngine.sysex_fmt_buffer;
		// 		memcpy(reply, reply_hdr, 5);
		memcpy(reply, reply_hdr, 8); //
		                             //		reply[5] = 0; // nominally 32*data[5] is start pos for a delta
		reply[8] = 0;                // nominally 32*data[8] is start pos for a delta //

		int32_t packed;
		if (rle) {
			packed =
			    //				pack_8to7_rle(reply + 6, max_packed_size,
			    // deluge::hid::display::OLED::oledCurrentImage[0], data_size);
			    pack_8to7_rle(reply + 9, max_packed_size, deluge::hid::display::OLED::oledCurrentImage[0], data_size);
		}
		else {
			//			packed = pack_8bit_to_7bit(reply + 6, max_packed_size,
			// deluge::hid::display::OLED::oledCurrentImage[0], 			                           data_size);
			packed = pack_8bit_to_7bit(reply + 9, max_packed_size, deluge::hid::display::OLED::oledCurrentImage[0],
			                           data_size); //
		}
		if (packed < 0) {
			display->popupTextTemporary("error: fail");
		}
		//		reply[6 + packed] = 0xf7; // end of transmission
		reply[9 + packed] = 0xf7;            // end of transmission
		                                     // 		device->sendSysex(reply, packed + 7); //
		cable.sendSysex(reply, packed + 10); //
	}
}

void HIDSysex::request7SegDisplay(MIDICable& cable, uint8_t* data, int32_t len) {
	if (data[2] == 0) { // was 4
		send7SegData(cable);
	}
}

void HIDSysex::send7SegData(MIDICable& cable) {
	if (display->have7SEG()) {
		// aschually 8 segments if you count the dot
		auto data = display->getLast();
		const int32_t packed_data_size = 5;
		//		uint8_t reply[12] = {0xf0, 0x7d, 0x02, 0x41, 0x00, 0x00};
		uint8_t reply[15] = {0xf0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x41, 0x00, 0x00};
		//  	pack_8bit_to_7bit(reply + 6, packed_data_size, data.data(), data.size());
		// int32_t pack_8bit_to_7bit(uint8_t* dst, int32_t dst_size, uint8_t* src, int32_t src_len);
		pack_8bit_to_7bit(reply + 9, packed_data_size, data.data(), data.size());
		//		reply[6 + packed_data_size] = 0xf7; // end of transmission
		reply[9 + packed_data_size] = 0xf7; // end of transmission
		                                    //		device->sendSysex(reply, packed_data_size + 7);
		cable.sendSysex(reply, packed_data_size + 10);
	}
}

void HIDSysex::sendOLEDDataDelta(MIDICable& cable, bool force) {
	const int32_t data_size = 768;
	const int32_t max_packed_size = 922;

	uint8_t* current = deluge::hid::display::OLED::oledCurrentImage[0];

	int32_t first_change = 9000;
	int32_t last_change = 0;
	int32_t* blkdata_new = (int32_t*)current;
	int32_t* blkdata_old = (int32_t*)oledDeltaImage;

	const int32_t word_size = data_size >> 2;

	if (force || oledDeltaForce) {
		first_change = 0;
		last_change = word_size - 1;
	}
	else {
		for (int32_t blk = 0; blk < word_size; blk++) {
			if (blkdata_new[blk] != blkdata_old[blk]) {
				if (first_change > blk) {
					first_change = blk;
				}
				last_change = blk;
			}
		}
	}

	if (first_change > word_size) {
		return;
	}

	int start = first_change / 2;
	int len = (last_change / 2) - start + 1;
	//	int8_t reply_hdr[5] = {0xf0, 0x7d, 0x02, 0x40, 0x02};
	uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x40, 0x02};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	memcpy(reply, reply_hdr, sizeof(reply_hdr));
	//	reply[5] = start;
	reply[sizeof(reply_hdr) + 0] = start;
	//  reply[6] = len;
	reply[sizeof(reply_hdr) + 1] = len;
	// 	int32_t packed = pack_8to7_rle(reply + 7, max_packed_size, current + 8 * start, 8 * len);
	int32_t packed = pack_8to7_rle(reply + 10, max_packed_size, current + 8 * start, 8 * len);
	if (packed <= 0) {
		return;
	}
	memcpy(oledDeltaImage + (8 * start), current + (8 * start), 8 * len);
	oledDeltaForce = false;
	// reply[7 + packed] = 0xf7; // end of transmission
	reply[10 + packed] = 0xf7; // end of transmission //
	// device->sendSysex(reply, packed + 8);
	cable.sendSysex(reply, packed + 11);
}

void HIDSysex::readBlock(MIDICable& cable) {
	const int32_t data_size = 768;
	const int32_t max_packed_size = 922;
	uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x40, 0x00};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	// 		memcpy(reply, reply_hdr, 5);
	memcpy(reply, reply_hdr, 8); //
	                             //		reply[5] = 0; // nominally 32*data[5] is start pos for a delta
	reply[8] = 0;                // nominally 32*data[8] is start pos for a delta //

	int32_t packed;

	//			packed = pack_8bit_to_7bit(reply + 6, max_packed_size,
	// deluge::hid::display::OLED::oledCurrentImage[0],
	uint8_t* srcBlock = (uint8_t*)smDeserializer.fileClusterBuffer; //  deluge::hid::display::OLED::oledCurrentImage[0];

	packed = pack_8bit_to_7bit(reply + 9, max_packed_size, srcBlock,
	                           data_size); //
	if (packed < 0) {
		display->popupTextTemporary("error: fail");
	}
	//		reply[6 + packed] = 0xf7; // end of transmission
	reply[9 + packed] = 0xf7;            // end of transmission
	                                     // 		device->sendSysex(reply, packed + 7); //
	cable.sendSysex(reply, packed + 10); //
}

#if ENABLE_PANEL_STREAM

namespace {

bool panelStreamActive() {
	return panelStateCable != nullptr && AudioEngine::audioSampleTimer <= (uint32_t)panelStateUntil;
}

void sendPanelEvent(uint8_t type, uint8_t a, uint8_t b, uint8_t c) {
	if (!panelStreamActive()) {
		return;
	}
	if (panelStateCable->sendBufferSpace() < 32) {
		return; // Events are ephemeral - drop rather than block
	}
	uint8_t reply[12] = {0xF0,
	                     0x00,
	                     0x21,
	                     0x7B,
	                     0x01,
	                     0x02,
	                     0x43,
	                     type,
	                     static_cast<uint8_t>(a & 0x7F),
	                     static_cast<uint8_t>(b & 0x7F),
	                     static_cast<uint8_t>(c & 0x7F),
	                     0xF7};
	panelStateCable->sendSysex(reply, sizeof(reply));
}

void sendPanelLedSnapshot() {
	using indicator_leds::LED;
	const LED allLeds[] = {
	    LED::AFFECT_ENTIRE, LED::SESSION_VIEW, LED::CLIP_VIEW, LED::SYNTH,      LED::KIT,
	    LED::MIDI,          LED::CV,           LED::KEYBOARD,  LED::SCALE_MODE, LED::CROSS_SCREEN_EDIT,
	    LED::BACK,          LED::LOAD,         LED::SAVE,      LED::LEARN,      LED::TAP_TEMPO,
	    LED::SYNC_SCALING,  LED::TRIPLETS,     LED::PLAY,      LED::RECORD,     LED::SHIFT,
	    LED::MOD_0,         LED::MOD_1,        LED::MOD_2,     LED::MOD_3,      LED::MOD_4,
	    LED::MOD_5,         LED::MOD_6,        LED::MOD_7,
	};
	for (LED led : allLeds) {
		uint8_t l = static_cast<uint8_t>(led);
		sendPanelEvent(0, l, indicator_leds::ledStates[l] ? 1 : 0, 0);
	}
}

} // namespace

void HIDSysex::requestPanelState(MIDICable& cable, uint8_t* data, int32_t len) {
	if (data[2] == 2 || data[2] == 3) {
		panelStateCable = &cable;
		panelStateUntil = AudioEngine::audioSampleTimer + 2 * kSampleRate;
		if (data[2] == 3) {
			panelPadsSendAll = true;
			sendPanelLedSnapshot();
		}
		panelPadsPending = true;
		sendPanelPadsIfChanged();
	}
}

void HIDSysex::sendPanelPadsIfChanged() {
	if (!panelStreamActive()) {
		return;
	}
	if (!panelPadsPending && !panelPadsSendAll) {
		return;
	}
	if (panelStateCable->sendBufferSpace() < 600) {
		return; // Stays pending; retried on next pad update or keepalive
	}

	constexpr int32_t numCols = kDisplayWidth + kSideBarWidth;
	constexpr int32_t rowBytes = numCols * 3;
	static_assert(sizeof(RGB) == 3, "pad stream assumes packed RGB");
	uint8_t* current = reinterpret_cast<uint8_t*>(&PadLEDs::image[0][0]);
	uint8_t* last = reinterpret_cast<uint8_t*>(&panelPadsLastSent[0][0]);

	int32_t firstRow = 0;
	int32_t lastRow = kDisplayHeight - 1;
	if (!panelPadsSendAll) {
		firstRow = -1;
		for (int32_t r = 0; r < kDisplayHeight; r++) {
			if (memcmp(current + r * rowBytes, last + r * rowBytes, rowBytes) != 0) {
				if (firstRow < 0) {
					firstRow = r;
				}
				lastRow = r;
			}
		}
		if (firstRow < 0) {
			panelPadsPending = false;
			return;
		}
	}
	int32_t numRows = lastRow - firstRow + 1;

	uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x42, 0x00};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	memcpy(reply, reply_hdr, sizeof(reply_hdr));
	reply[8] = firstRow;
	reply[9] = numRows;
	int32_t packed = pack_8to7_rle(reply + 10, 600, current + firstRow * rowBytes, numRows * rowBytes);
	if (packed <= 0) {
		return;
	}
	memcpy(last + firstRow * rowBytes, current + firstRow * rowBytes, numRows * rowBytes);
	panelPadsSendAll = false;
	panelPadsPending = false;
	reply[10 + packed] = 0xF7;
	panelStateCable->sendSysex(reply, packed + 11);
}

void HIDSysex::panelPadsDirty() {
	panelPadsPending = true;
	sendPanelPadsIfChanged();
}

void HIDSysex::panelLedEvent(uint8_t ledCode, uint8_t state) {
	sendPanelEvent(0, ledCode, state, 0);
}

void HIDSysex::panelKnobIndicatorEvent(uint8_t whichKnob, uint8_t level) {
	sendPanelEvent(1, whichKnob, level > 127 ? 127 : level, 0);
}

void HIDSysex::panelButtonEvent(uint8_t buttonCode, bool on) {
	sendPanelEvent(2, buttonCode, on ? 1 : 0, 0);
}

void HIDSysex::panelPadInputEvent(uint8_t x, uint8_t y, uint8_t velocity) {
	sendPanelEvent(3, x, y, velocity > 127 ? 127 : velocity);
}

void HIDSysex::panelEncoderEvent(uint8_t encoderId, int32_t delta) {
	int32_t clamped = delta < -63 ? -63 : (delta > 63 ? 63 : delta);
	sendPanelEvent(4, encoderId, static_cast<uint8_t>(clamped + 64), 0);
}

#endif // ENABLE_PANEL_STREAM
