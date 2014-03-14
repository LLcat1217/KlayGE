// OGLESRenderWindow.cpp
// KlayGE OpenGL ES 2渲染窗口类 实现文件
// Ver 3.10.0
// 版权所有(C) 龚敏敏, 2010
// Homepage: http://www.klayge.org
//
// 3.10.0
// 初次建立 (2010.1.22)
//
// 修改记录
//////////////////////////////////////////////////////////////////////////////////

#include <KlayGE/KlayGE.hpp>
#include <KFL/ThrowErr.hpp>
#include <KFL/Util.hpp>
#include <KFL/Math.hpp>
#include <KlayGE/SceneManager.hpp>
#include <KlayGE/Context.hpp>
#include <KlayGE/RenderSettings.hpp>
#include <KlayGE/RenderFactory.hpp>
#include <KlayGE/RenderEngine.hpp>
#include <KlayGE/App3D.hpp>
#include <KlayGE/Window.hpp>

#include <map>
#include <sstream>
#include <boost/assert.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <glloader/glloader.h>

#include <KlayGE/OpenGLES/OGLESRenderWindow.hpp>

namespace KlayGE
{
	OGLESRenderWindow::OGLESRenderWindow(std::string const & name, RenderSettings const & settings)
						: OGLESFrameBuffer(false)
	{
		// Store info
		name_				= name;
		width_				= settings.width;
		height_				= settings.height;
		isFullScreen_		= settings.full_screen;
		color_bits_			= NumFormatBits(settings.color_fmt);

		WindowPtr const & main_wnd = Context::Instance().AppInstance().MainWnd();		
		on_paint_connect_ = main_wnd->OnPaint().connect(bind(&OGLESRenderWindow::OnPaint, this,
			placeholders::_1));
		on_exit_size_move_connect_ = main_wnd->OnExitSizeMove().connect(bind(&OGLESRenderWindow::OnExitSizeMove, this,
			placeholders::_1));
		on_size_connect_ = main_wnd->OnSize().connect(bind(&OGLESRenderWindow::OnSize, this,
			placeholders::_1, placeholders::_2));

		if (isFullScreen_)
		{
			left_ = 0;
			top_ = 0;
		}
		else
		{
			top_ = settings.top;
			left_ = settings.left;
		}

		display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);

		int r_size, g_size, b_size, a_size, d_size, s_size;
		switch (settings.color_fmt)
		{
		case EF_ARGB8:
		case EF_ABGR8:
			r_size = 8;
			g_size = 8;
			b_size = 8;
			a_size = 8;
			break;

		case EF_A2BGR10:
			r_size = 10;
			g_size = 10;
			b_size = 10;
			a_size = 2;
			break;

		default:
			BOOST_ASSERT(false);
			break;
		}
		switch (settings.depth_stencil_fmt)
		{
		case EF_D16:
			d_size = 16;
			s_size = 0;
			break;

		case EF_D24S8:
			d_size = 24;
			s_size = 8;
			break;

		case EF_D32F:
			d_size = 32;
			s_size = 0;
			break;

		default:
			d_size = 0;
			s_size = 0;
			break;
		}

		std::vector<std::pair<std::string, std::pair<EGLint, int> > > available_versions;
#if !defined(KLAYGE_PLATFORM_ANDROID) || (__ANDROID_API__ >= 18)
		available_versions.push_back(std::make_pair("3.0", std::make_pair(EGL_OPENGL_ES3_BIT_KHR, 3)));
#endif
		available_versions.push_back(std::make_pair("2.0", std::make_pair(EGL_OPENGL_ES2_BIT, 2)));

		std::vector<std::string> strs;
		boost::algorithm::split(strs, settings.options, boost::is_any_of(","));
		for (size_t index = 0; index < strs.size(); ++ index)
		{
			std::string& opt = strs[index];
			boost::algorithm::trim(opt);
			std::string::size_type loc = opt.find(':');
			std::string opt_name = opt.substr(0, loc);
			std::string opt_val = opt.substr(loc + 1);

			if ("version" == opt_name)
			{
				size_t feature_index = 0;
				for (size_t i = 0; i < available_versions.size(); ++ i)
				{
					if (available_versions[i].first == opt_val)
					{
						feature_index = i;
						break;
					}
				}

				if (feature_index > 0)
				{
					available_versions.erase(available_versions.begin(),
						available_versions.begin() + feature_index);
				}
			}
		}

		std::vector<EGLint> visual_attr;
		visual_attr.push_back(EGL_RENDERABLE_TYPE);
		visual_attr.push_back(available_versions[0].second.first);
		visual_attr.push_back(EGL_RED_SIZE);
		visual_attr.push_back(r_size);
		visual_attr.push_back(EGL_GREEN_SIZE);
		visual_attr.push_back(g_size);
		visual_attr.push_back(EGL_BLUE_SIZE);
		visual_attr.push_back(b_size);
		visual_attr.push_back(EGL_ALPHA_SIZE);
		visual_attr.push_back(a_size);
		if (d_size > 0)
		{
			visual_attr.push_back(EGL_DEPTH_SIZE);
			visual_attr.push_back(d_size);
		}
		if (s_size > 0)
		{
			visual_attr.push_back(EGL_STENCIL_SIZE);
			visual_attr.push_back(s_size);
		}
		if (settings.sample_count > 1)
		{
			visual_attr.push_back(EGL_SAMPLES);
			visual_attr.push_back(settings.sample_count);
		}
		visual_attr.push_back(EGL_NONE);				// end of list

		EGLint major_ver, minor_ver;
		EGLint num_cfgs;
		eglInitialize(display_, &major_ver, &minor_ver);
		eglGetConfigs(display_, nullptr, 0, &num_cfgs);

		EGLint client_version = -1;
		for (size_t i = 0; i < available_versions.size(); ++ i)
		{
			visual_attr[1] = available_versions[i].second.first;
			if (eglChooseConfig(display_, &visual_attr[0], &cfg_, 1, &num_cfgs))
			{
				client_version = available_versions[i].second.second;
				break;
			}
		}
		BOOST_ASSERT(client_version != -1);

		NativeWindowType wnd;
#if defined KLAYGE_PLATFORM_WINDOWS
		wnd = hWnd_ = main_wnd->HWnd();
#elif defined KLAYGE_PLATFORM_LINUX
		wnd = x_window_ = main_wnd->XWindow();
#elif defined KLAYGE_PLATFORM_ANDROID
		wnd = a_window_ = main_wnd->AWindow();
		EGLint format;
		eglGetConfigAttrib(display_, cfg_, EGL_NATIVE_VISUAL_ID, &format);
		ANativeWindow_setBuffersGeometry(wnd, 0, 0, format);
#endif

		surf_ = eglCreateWindowSurface(display_, cfg_, wnd, nullptr);

		EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, client_version, EGL_NONE };
		context_ = eglCreateContext(display_, cfg_, EGL_NO_CONTEXT, ctx_attr);

		eglMakeCurrent(display_, surf_, surf_, context_);

		if (!glloader_GLES_VERSION_2_0())
		{
			THR(errc::function_not_supported);
		}

		eglSwapInterval(display_, 0);

		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		viewport_->left = 0;
		viewport_->top = 0;
		viewport_->width = width_;
		viewport_->height = height_;

		std::wstring vendor, renderer, version;
		Convert(vendor, reinterpret_cast<char const *>(glGetString(GL_VENDOR)));
		Convert(renderer, reinterpret_cast<char const *>(glGetString(GL_RENDERER)));
		Convert(version, reinterpret_cast<char const *>(glGetString(GL_VERSION)));
		std::wostringstream oss;
		oss << vendor << L" " << renderer << L" " << version;
		if (settings.sample_count > 1)
		{
			oss << L" (" << settings.sample_count << L"x AA)";
		}
		description_ = oss.str();
	}

	OGLESRenderWindow::~OGLESRenderWindow()
	{
		on_paint_connect_.disconnect();
		on_exit_size_move_connect_.disconnect();
		on_size_connect_.disconnect();

		this->Destroy();
	}

	std::wstring const & OGLESRenderWindow::Description() const
	{
		return description_;
	}

	// 改变窗口大小
	/////////////////////////////////////////////////////////////////////////////////
	void OGLESRenderWindow::Resize(uint32_t width, uint32_t height)
	{
		width_ = width;
		height_ = height;

		// Notify viewports of resize
		viewport_->width = width;
		viewport_->height = height;

		App3DFramework& app = Context::Instance().AppInstance();
		app.OnResize(width, height);
	}

	// 改变窗口位置
	/////////////////////////////////////////////////////////////////////////////////
	void OGLESRenderWindow::Reposition(uint32_t left, uint32_t top)
	{
		left_ = left;
		top_ = top;
	}

	// 获取是否是全屏状态
	/////////////////////////////////////////////////////////////////////////////////
	bool OGLESRenderWindow::FullScreen() const
	{
		return isFullScreen_;
	}

	// 设置是否是全屏状态
	/////////////////////////////////////////////////////////////////////////////////
	void OGLESRenderWindow::FullScreen(bool fs)
	{
		if (isFullScreen_ != fs)
		{
			left_ = 0;
			top_ = 0;

#if defined KLAYGE_PLATFORM_WINDOWS
			uint32_t style;
			if (fs)
			{
				DEVMODE devMode;
				devMode.dmSize = sizeof(devMode);
				devMode.dmBitsPerPel = color_bits_;
				devMode.dmPelsWidth = width_;
				devMode.dmPelsHeight = height_;
				devMode.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
				::ChangeDisplaySettings(&devMode, CDS_FULLSCREEN);

				style = WS_POPUP;
			}
			else
			{
				::ChangeDisplaySettings(nullptr, 0);

				style = WS_OVERLAPPEDWINDOW;
			}

			::SetWindowLongPtrW(hWnd_, GWL_STYLE, style);

			RECT rc = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
			::AdjustWindowRect(&rc, style, false);
			width_ = rc.right - rc.left;
			height_ = rc.bottom - rc.top;

			isFullScreen_ = fs;

			::ShowWindow(hWnd_, SW_SHOWNORMAL);
			::UpdateWindow(hWnd_);
#elif defined KLAYGE_PLATFORM_LINUX
			isFullScreen_ = fs;
			XFlush(x_display_);
#elif defined KLAYGE_PLATFORM_ANDROID
			isFullScreen_ = fs;
#endif
		}
	}

	void OGLESRenderWindow::WindowMovedOrResized(Window const & win)
	{
#if defined KLAYGE_PLATFORM_WINDOWS
		UNREF_PARAM(win);

		::RECT rect;
		::GetClientRect(hWnd_, &rect);

		uint32_t new_left = rect.left;
		uint32_t new_top = rect.top;
		if ((new_left != left_) || (new_top != top_))
		{
			this->Reposition(new_left, new_top);
		}

		uint32_t new_width = rect.right - rect.left;
		uint32_t new_height = rect.bottom - rect.top;
#elif defined KLAYGE_PLATFORM_LINUX
		int screen = DefaultScreen(x_display_);
		uint32_t new_width = DisplayWidth(x_display_, screen);
		uint32_t new_height = DisplayHeight(x_display_, screen);
#elif defined KLAYGE_PLATFORM_ANDROID
		uint32_t new_left = win.Left() / 2;
		uint32_t new_top = win.Top() / 2;
		if ((new_left != left_) || (new_top != top_))
		{
			this->Reposition(new_left, new_top);
		}

		EGLint w, h;
		eglQuerySurface(display_, surf_, EGL_WIDTH, &w);
		eglQuerySurface(display_, surf_, EGL_HEIGHT, &h);

		uint32_t new_width = w - new_left;
		uint32_t new_height = h - new_top;
#endif

		if ((new_width != width_) || (new_height != height_))
		{
			Context::Instance().RenderFactoryInstance().RenderEngineInstance().Resize(new_width, new_height);
		}
	}

	void OGLESRenderWindow::Destroy()
	{
#if defined KLAYGE_PLATFORM_WINDOWS
		if (hWnd_ != nullptr)
		{
			if (isFullScreen_)
			{
				::ChangeDisplaySettings(nullptr, 0);
				::ShowCursor(TRUE);
			}
		}
#elif defined KLAYGE_PLATFORM_LINUX
#elif defined KLAYGE_PLATFORM_ANDROID
#endif

		if (display_ != nullptr)
		{
			eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			eglDestroyContext(display_, context_);
			eglTerminate(display_);

			display_ = nullptr;
		}
	}

	void OGLESRenderWindow::SwapBuffers()
	{
		eglSwapBuffers(display_, surf_);
	}

	void OGLESRenderWindow::OnPaint(Window const & win)
	{
		// If we get WM_PAINT messges, it usually means our window was
		// comvered up, so we need to refresh it by re-showing the contents
		// of the current frame.
		if (win.Active() && win.Ready())
		{
			Context::Instance().SceneManagerInstance().Update();
			this->SwapBuffers();
		}
	}

	void OGLESRenderWindow::OnExitSizeMove(Window const & win)
	{
		this->WindowMovedOrResized(win);
	}

	void OGLESRenderWindow::OnSize(Window const & win, bool active)
	{
		if (active)
		{
			if (win.Ready())
			{
				this->WindowMovedOrResized(win);
			}
		}
	}
}
