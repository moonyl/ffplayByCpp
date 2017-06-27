#pragma once

#include <memory>
#include <SDL.h>

class Window
{
public:
	struct WindowConfig
	{
		const char *title;
		bool isFull;
		bool isBorderless;
		int screenWidth;
		int screenHeight;
	};

public:
	Window(WindowConfig &config);
	~Window();

private:
	SDL_Window *handle() { return m_window.get(); }

public:
	void setSize(int w, int h);
	friend class Renderer;

private:
	struct SDLWindowDestroyer
	{
		void operator()(SDL_Window* w) const
		{
			SDL_DestroyWindow(w);
		}
	};

private:
	std::unique_ptr<SDL_Window, SDLWindowDestroyer> m_window;
};

