#include "Renderer.h"
#include <SDL.h>
extern "C"
{
#include <libavutil/avutil.h>
}


Renderer::Renderer(SDL_Window & window) :
	m_renderer(SDL_CreateRenderer(&window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC))
{
	if (!m_renderer) {
		av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
		m_renderer.reset(SDL_CreateRenderer(&window, -1, 0));
	}
	if (m_renderer) {
		SDL_RendererInfo info;
		if (!SDL_GetRendererInfo(const_cast<SDL_Renderer*>(m_renderer.get()), &info)) {
			av_log(nullptr, AV_LOG_VERBOSE, "Initialized %s renderer.\n", info.name);
		}
	}
}

Renderer::~Renderer()
{	
}

int Renderer::setDrawColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	return SDL_SetRenderDrawColor(const_cast<SDL_Renderer*>(m_renderer.get()), r, g, b, a);
}

int Renderer::clear()
{
	return SDL_RenderClear(const_cast<SDL_Renderer*>(m_renderer.get()));
}

void Renderer::present()
{
	SDL_RenderPresent(const_cast<SDL_Renderer*>(m_renderer.get()));
}

int Renderer::fillRectangle(const SDL_Rect &rect)
{
	return SDL_RenderFillRect(const_cast<SDL_Renderer*>(m_renderer.get()), &rect);
}

int Renderer::copyEx(SDL_Texture & src, const SDL_Rect & srcRect, const SDL_Rect & dstRect, double angle, const SDL_Point & center, const SDL_RendererFlip &flip)
{	
	return SDL_RenderCopyEx(const_cast<SDL_Renderer*>(m_renderer.get()), &src, 
		&srcRect, &dstRect, angle, &center, flip);
}

int Renderer::copyEx(SDL_Texture & src, const SDL_Rect & dstRect, double angle, const SDL_RendererFlip & flip)
{
	return SDL_RenderCopyEx(const_cast<SDL_Renderer*>(m_renderer.get()), &src,
		nullptr, &dstRect, angle, nullptr, flip);
}

int Renderer::copy(SDL_Texture & src, const SDL_Rect & srcRect, const SDL_Rect & tgtRect)
{
	return SDL_RenderCopy(const_cast<SDL_Renderer*>(m_renderer.get()), &src,
		&srcRect, &tgtRect);
}

int Renderer::copy(SDL_Texture & src)
{	
	return SDL_RenderCopy(const_cast<SDL_Renderer*>(m_renderer.get()), &src,
		nullptr, nullptr);
}
