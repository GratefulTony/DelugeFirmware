#include "definitions_cxx.hpp"
#include "io/midi/midi_device_manager.h"

// Panel state + input event streaming for host-side panel mirrors. Runtime-gated by a
// subscription with a 2s keepalive (same idiom as the OLED mirror); this flag additionally
// compiles the whole feature out for minimal builds.
#ifndef ENABLE_PANEL_STREAM
#define ENABLE_PANEL_STREAM 1
#endif

namespace HIDSysex {
void requestOLEDDisplay(MIDICable& cable, uint8_t* data, int32_t len);
void request7SegDisplay(MIDICable& cable, uint8_t* data, int32_t len);
void sysexReceived(MIDICable& cable, uint8_t* data, int32_t len);
void sendOLEDData(MIDICable& cable, bool rle);
void sendOLEDDataDelta(MIDICable& cable, bool force);
void send7SegData(MIDICable& cable);
void sendDisplayIfChanged();
void readBlock(MIDICable& cable);

#if ENABLE_PANEL_STREAM
void requestPanelState(MIDICable& cable, uint8_t* data, int32_t len);
void sendPanelPadsIfChanged();
void panelPadsDirty();
void panelLedEvent(uint8_t ledCode, uint8_t state);
void panelKnobIndicatorEvent(uint8_t whichKnob, uint8_t level);
void panelButtonEvent(uint8_t buttonCode, bool on);
void panelPadInputEvent(uint8_t x, uint8_t y, uint8_t velocity);
void panelEncoderEvent(uint8_t encoderId, int32_t delta);
#else
inline void requestPanelState(MIDICable&, uint8_t*, int32_t) {
}
inline void sendPanelPadsIfChanged() {
}
inline void panelPadsDirty() {
}
inline void panelLedEvent(uint8_t, uint8_t) {
}
inline void panelKnobIndicatorEvent(uint8_t, uint8_t) {
}
inline void panelButtonEvent(uint8_t, bool) {
}
inline void panelPadInputEvent(uint8_t, uint8_t, uint8_t) {
}
inline void panelEncoderEvent(uint8_t, int32_t) {
}
#endif
} // namespace HIDSysex
