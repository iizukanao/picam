/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2021, Raspberry Pi (Trading) Ltd.
 *
 * preview.cpp - preview window interface
 */

#include <iostream>

#include "log/log.h"

#include "preview.hpp"

Preview *make_null_preview(PicamOption const *options);
Preview *make_egl_preview(PicamOption const *options);
Preview *make_drm_preview(PicamOption const *options);
// Preview *make_qt_preview(Options const *options);

Preview *make_preview(PicamOption const *options)
{
	if (!options->is_preview_enabled) {
		// Do not show preview
		return make_null_preview(options);
	}
// #if QT_PRESENT
// 	else if (options->qt_preview)
// 	{
// 		Preview *p = make_qt_preview(options);
// 		if (p)
// 			std::cerr << "Made QT preview window" << std::endl;
// 		return p;
// 	}
// #endif
	else
	{
		try
		{
#if LIBEGL_PRESENT
			// EGL is used when X Window System is running.
			// DRM cannot be used if X is running.
			Preview *p = make_egl_preview(options);
			if (p) {
				log_debug("Made X/EGL preview window\n");
			}
			return p;
#else
			throw std::runtime_error("egl libraries unavailable.");
#endif
		}
		catch (std::exception const &e)
		{
			log_debug("make_egl_preview error: %s\n", e.what());
			try
			{
#if LIBDRM_PRESENT
				// DRM (Direct Rendering Mangaer) is used when X is not running.
				Preview *p = make_drm_preview(options);
				if (p) {
					log_debug("Made DRM preview window\n");
				}
				return p;
#else
				throw std::runtime_error("drm libraries unavailable.");
#endif
			}
			catch (std::exception const &e)
			{
				std::cerr << "Preview window unavailable: " << e.what() << std::endl;
				return make_null_preview(options);
			}
		}
	}

	return nullptr; // prevents compiler warning in debug builds
}
