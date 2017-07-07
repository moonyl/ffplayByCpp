#pragma once

#include <memory>
extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
struct SwsContext;
struct SwsFilter;
class SwScaleContext
{
public:
	SwScaleContext();
	explicit SwScaleContext(SwsContext *context);
	~SwScaleContext();

public:
	int scale(const uint8_t * const *data, const int *srcStride,
		int srcSliceY, int srcSliceH, uint8_t * const *dst, const int *dstStride);
	bool applyCachedContext(int srcWidth, int srcHeight, AVPixelFormat srcFormat,
		int dstWidth, int dstHeight, AVPixelFormat dstFormat, int flags, SwsFilter *srcFilter = nullptr, 
		SwsFilter *dstFilter = nullptr, const double *param = nullptr);

private:
	struct SwsDestroyer
	{
		void operator()(SwsContext* c) const
		{
			sws_freeContext(c);
		}
	};
	
private:
	std::unique_ptr<SwsContext, SwsDestroyer> m_context = nullptr;
	bool m_hasContext;
};

