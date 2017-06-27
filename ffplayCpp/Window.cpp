#include "Window.h"

Window::Window(WindowConfig &config)	
{
	int flags = SDL_WINDOW_SHOWN;
	if (config.isFull) {
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	}
	if (config.isBorderless) {
		flags |= SDL_WINDOW_BORDERLESS;
	}
	else {
		flags |= SDL_WINDOW_RESIZABLE;
	}

	m_window.reset(SDL_CreateWindow(config.title, SDL_WINDOWPOS_UNDEFINED, 
			SDL_WINDOWPOS_UNDEFINED, config.screenWidth, config.screenHeight, flags));
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
}

Window::~Window()
{
}

void Window::setSize(int w, int h)
{
	SDL_SetWindowSize(const_cast<SDL_Window*>(m_window.get()), w, h);
}
