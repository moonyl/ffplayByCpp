#pragma once

#include <memory>
#include <SDL.h>

class Renderer
{
public:
	Renderer(SDL_Window &window);
	~Renderer();

public:
	int setDrawColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a);
	int clear();
	void present();
	int fillRectangle(const SDL_Rect &rect);
	int copyEx(SDL_Texture &src, const SDL_Rect &srcRect, const SDL_Rect &tgtRect, double angle, const SDL_Point &center, const SDL_RendererFlip &filp);
	int copyEx(SDL_Texture &src, const SDL_Rect &tgtRect, double angle, const SDL_RendererFlip &flip);
	int copy(SDL_Texture &src, const SDL_Rect &srcRect, const SDL_Rect &tgtRect);
	int copy(SDL_Texture &src);

public:
	// temporarily
	SDL_Renderer *renderer() { return m_renderer.get(); }

private:
	struct SDLRendererDestroyer
	{
		void operator()(SDL_Renderer* w) const
		{
			SDL_DestroyRenderer(w);
		}
	};

private:
	std::unique_ptr<SDL_Renderer, SDLRendererDestroyer> m_renderer;
};

