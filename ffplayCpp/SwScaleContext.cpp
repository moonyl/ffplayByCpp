#include "SwScaleContext.h"

SwScaleContext::SwScaleContext() :
	m_context(nullptr)
{
}

SwScaleContext::SwScaleContext(SwsContext * context) :
	m_context(context)
{
}


SwScaleContext::~SwScaleContext()
{
}

int SwScaleContext::scale(const uint8_t * const * data, const int * srcStride, int srcSliceY, int srcSliceH, uint8_t * const * dst, const int * dstStride)
{
	return sws_scale(m_context.get(), data, srcStride, srcSliceY, srcSliceH, dst, dstStride);
}

bool SwScaleContext::applyCachedContext(int srcWidth, int srcHeight, AVPixelFormat srcFormat, int dstWidth, int dstHeight, AVPixelFormat dstFormat, int flags, SwsFilter * srcFilter, SwsFilter * dstFilter, const double * param)
{
	SwsContext *retContext = sws_getCachedContext(m_context.get(), srcWidth, srcHeight, srcFormat, dstWidth, dstHeight, dstFormat, flags, srcFilter, dstFilter, param);
	if (!retContext) {
		return false;
	}

	if (retContext != m_context.get()) {
		m_context.reset(retContext);
	}
	
	return true;	
}
