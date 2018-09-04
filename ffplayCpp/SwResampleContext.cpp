#include "SwResampleContext.h"

extern "C" {
#include <libswresample/swresample.h>
}

SwResampleContext::SwResampleContext()
{

}

int SwResampleContext::applyOptionedContext(int64_t outChannelLayout,
	AVSampleFormat outFormat, int outSampleRate,
	int64_t inChannelLayout, AVSampleFormat inFormat,
	int inSampleRate, int logOffset, void *logCtx) 
{
	swr_free(&m_context);
	m_context = swr_alloc_set_opts(nullptr, outChannelLayout, outFormat,
		outSampleRate, inChannelLayout, inFormat, inSampleRate,
		logOffset, logCtx);
	if (!m_context || init() < 0) {
		av_log(nullptr, AV_LOG_ERROR,
			"Cannot create sample rate converter for conversion of %d Hz %s 0x%lu channel layout to %d Hz %s 0x%lu channel layout!\n",
			inSampleRate, av_get_sample_fmt_name(inFormat), inChannelLayout,
			outSampleRate, av_get_sample_fmt_name(outFormat), outChannelLayout);
		swr_free(&m_context);
		return -1;
	}
	return 0;
}

int SwResampleContext::init()
{
	return swr_init(m_context);
}

int SwResampleContext::setCompensation(int sampleDelta, int compensationDistance)
{
	return swr_set_compensation(m_context, sampleDelta, compensationDistance);
}

int SwResampleContext::convert(uint8_t ** out, int outCount, const uint8_t ** in, int inCount)
{
	return swr_convert(m_context, out, outCount, in, inCount);
}


SwResampleContext::~SwResampleContext()
{
	swr_free(&m_context);
}
