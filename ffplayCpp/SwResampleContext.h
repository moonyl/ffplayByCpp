#pragma once

#include <cstdint>
extern "C" {
#include <libavutil/samplefmt.h>
}
struct SwrContext;

class SwResampleContext
{
public:
	SwResampleContext();
	~SwResampleContext();

public:
	bool isApplied() const { return m_context == nullptr ? false : true; }
	int applyOptionedContext(int64_t outChannelLayout,
		AVSampleFormat outFormat, int outSampleRate,
		int64_t inChannelLayout, AVSampleFormat inFormat,
		int inSampleRate, int logOffset = 0, void *logCtx = nullptr);
	int init();
	int setCompensation(int sampleDelta, int compensationDistance);
	int convert(uint8_t **out, int outCount, const uint8_t **in, int inCount);

private:
	SwrContext *m_context = nullptr;
};

