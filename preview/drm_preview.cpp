/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * drm_preview.cpp - DRM-based preview window.
 */

#include <map>
#include <string.h>
#include <unistd.h>
#include <iostream>
#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// #include "core/options.hpp"
#include "log/log.h"

#include "preview.hpp"

class DrmPreview : public Preview
{
public:
	DrmPreview(PicamOption const *options);
	~DrmPreview();
	// Display the buffer. You get given the fd back in the BufferDoneCallback
	// once its available for re-use.
	virtual void Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info) override;
	// Reset the preview window, clearing the current buffers and being ready to
	// show new ones.
	virtual void Reset() override;
	// Return the maximum image size allowed.
	virtual void MaxImageSize(unsigned int &w, unsigned int &h) const override
	{
		w = max_image_width_;
		h = max_image_height_;
	}

private:
	struct Buffer
	{
		Buffer() : fd(-1) {}
		int fd;
		size_t size;
		StreamInfo info;
		uint32_t bo_handle;
		unsigned int fb_handle;
	};
	void makeBuffer(int fd, size_t size, StreamInfo const &info, Buffer &buffer);
	void findCrtc();
	void findPlane();
	int drmfd_;
	int conId_;
	uint32_t crtcId_;
	int crtcIdx_;
	uint32_t planeId_;
	unsigned int out_fourcc_;
	unsigned int x_;
	unsigned int y_;
	unsigned int width_;
	unsigned int height_;
	unsigned int screen_width_;
	unsigned int screen_height_;
	std::map<int, Buffer> buffers_; // map the DMABUF's fd to the Buffer
	int last_fd_;
	unsigned int max_image_width_;
	unsigned int max_image_height_;
	bool first_time_;
};

#define ERRSTR strerror(errno)

void DrmPreview::findCrtc()
{
	int i;
	drmModeRes *res = drmModeGetResources(drmfd_);
	if (!res)
		throw std::runtime_error("drmModeGetResources failed: " + std::string(ERRSTR));

	if (res->count_crtcs <= 0)
		throw std::runtime_error("drm: no crts");

	max_image_width_ = res->max_width;
	max_image_height_ = res->max_height;
	printf("preview: count_crtcs=%d count_encoders=%d count_connectors=%d count_fbs=%d max_image_width=%u max_image_height=%u\n",
	res->count_crtcs, res->count_encoders, res->count_connectors, res->count_fbs,
	max_image_width_, max_image_height_);

	if (!conId_)
	{
		// log_debug("No connector ID specified.  Choosing default from list:\n");

		for (i = 0; i < res->count_connectors; i++)
		{
			if (i == this->options_->preview_hdmi) {
				log_debug("preview: CRTC connector %d: chosen\n", i);
			} else {
				log_debug("preview: CRTC connector %d: skipped because preview_hdmi=%d is specified\n", i, this->options_->preview_hdmi);
				continue;
			}
			printf("preview: inspecting connector: %d\n", i);
			drmModeConnector *con = drmModeGetConnector(drmfd_, res->connectors[i]);
			drmModeEncoder *enc = NULL;
			drmModeCrtc *crtc = NULL;

			if (con->encoder_id)
			{
				enc = drmModeGetEncoder(drmfd_, con->encoder_id);
				printf("preview: set encoder\n");
				if (enc->crtc_id)
				{
					crtc = drmModeGetCrtc(drmfd_, enc->crtc_id);
					printf("preview: set crtc\n");
				}
			}

			if (!conId_ && crtc) // TODO: Select connector by option
			{
				conId_ = con->connector_id;
				crtcId_ = crtc->crtc_id;
				printf("preview: set conId_=%d crtcId_=%u\n", conId_, crtcId_);
			}

			if (crtc)
			{
				screen_width_ = crtc->width;
				screen_height_ = crtc->height;
				printf("preview: crtc screen_width_=%u screen_height_=%u\n", screen_width_, screen_height_);
			}

			log_debug("Connector %u (crtc %u): type %u, %ux%u %s",
				con->connector_id, (crtc ? crtc->crtc_id : 0), con->connector_type, (crtc ? crtc->width : 0), (crtc ? crtc->height : 0),
				(conId_ == (int)con->connector_id ? " (chosen)" : "")
			);
			// if (options_->verbose)
			// 	std::cerr << "Connector " << con->connector_id << " (crtc " << (crtc ? crtc->crtc_id : 0) << "): type "
			// 			  << con->connector_type << ", " << (crtc ? crtc->width : 0) << "x" << (crtc ? crtc->height : 0)
			// 			  << (conId_ == (int)con->connector_id ? " (chosen)" : "") << std::endl; // TODO: (chosen) is wrong when connector is specified
		}

		if (!conId_)
			throw std::runtime_error("No suitable enabled connector found");
	}

	crtcIdx_ = -1;

	for (i = 0; i < res->count_crtcs; ++i)
	{
		if (crtcId_ == res->crtcs[i])
		{
			crtcIdx_ = i;
			break;
		}
	}

	if (crtcIdx_ == -1)
	{
		drmModeFreeResources(res);
		throw std::runtime_error("drm: CRTC " + std::to_string(crtcId_) + " not found");
	}

	if (res->count_connectors <= 0)
	{
		drmModeFreeResources(res);
		throw std::runtime_error("drm: no connectors");
	}

	drmModeConnector *c;
	c = drmModeGetConnector(drmfd_, conId_);
	if (!c)
	{
		drmModeFreeResources(res);
		throw std::runtime_error("drmModeGetConnector failed: " + std::string(ERRSTR));
	}

	if (!c->count_modes)
	{
		drmModeFreeConnector(c);
		drmModeFreeResources(res);
		throw std::runtime_error("connector supports no mode");
	}

	if (!options_->is_previewrect_enabled || width_ == 0 || height_ == 0)
	{
		// fullscreen preview
		drmModeCrtc *crtc = drmModeGetCrtc(drmfd_, crtcId_);
		x_ = crtc->x;
		y_ = crtc->y;
		width_ = crtc->width;
		height_ = crtc->height;
		printf("preview: crtc x_=%u y_=%u width_=%u height_=%u\n", x_, y_, width_, height_);
		drmModeFreeCrtc(crtc);
	}
}

void DrmPreview::findPlane()
{
	drmModePlaneResPtr planes;
	drmModePlanePtr plane;
	unsigned int i;
	unsigned int j;

	planes = drmModeGetPlaneResources(drmfd_);
	if (!planes)
		throw std::runtime_error("drmModeGetPlaneResources failed: " + std::string(ERRSTR));

	try
	{
		for (i = 0; i < planes->count_planes; ++i)
		{
			plane = drmModeGetPlane(drmfd_, planes->planes[i]);
			if (!planes)
				throw std::runtime_error("drmModeGetPlane failed: " + std::string(ERRSTR));

			if (!(plane->possible_crtcs & (1 << crtcIdx_)))
			{
				drmModeFreePlane(plane);
				continue;
			}

			for (j = 0; j < plane->count_formats; ++j)
			{
				if (plane->formats[j] == out_fourcc_)
				{
					break;
				}
			}

			if (j == plane->count_formats)
			{
				drmModeFreePlane(plane);
				continue;
			}

			planeId_ = plane->plane_id;

			drmModeFreePlane(plane);
			break;
		}
	}
	catch (std::exception const &e)
	{
		drmModeFreePlaneResources(planes);
		throw;
	}

	drmModeFreePlaneResources(planes);
}

DrmPreview::DrmPreview(PicamOption const *options) : Preview(options), last_fd_(-1), first_time_(true)
{
	drmfd_ = drmOpen("vc4", NULL);
	if (drmfd_ < 0)
		throw std::runtime_error("drmOpen failed: " + std::string(ERRSTR));

	x_ = options_->preview_x;
	y_ = options_->preview_y;
	width_ = options_->preview_width;
	height_ = options_->preview_height;
	screen_width_ = 0;
	screen_height_ = 0;
	printf("preview: ctor: x_=%u y_=%u width_=%u height_=%u\n", x_, y_, width_, height_);

	try
	{
		if (!drmIsMaster(drmfd_))
			throw std::runtime_error("DRM preview unavailable - not master");

		conId_ = 0;
		findCrtc();
		out_fourcc_ = DRM_FORMAT_YUV420;
		findPlane();
	}
	catch (std::exception const &e)
	{
		close(drmfd_);
		throw;
	}

	// Default behaviour here is to go fullscreen while maintaining aspect ratio.
	if (!options_->is_previewrect_enabled || width_ == 0 || height_ == 0 || x_ + width_ > screen_width_ ||
		y_ + height_ > screen_height_)
	{
		x_ = y_ = 0;
		width_ = screen_width_;
		height_ = screen_height_;
		printf("preview: default behavior: x_=%u y_=%u width_=%u height_=%u\n", x_, y_, width_, height_);
	}
}

DrmPreview::~DrmPreview()
{
	close(drmfd_);
}

// DRM doesn't seem to have userspace definitions of its enums, but the properties
// contain enum-name-to-value tables. So the code below ends up using strings and
// searching for name matches. I suppose it works...

static void get_colour_space_info(std::optional<libcamera::ColorSpace> const &cs, char const *&encoding,
								  char const *&range)
{
	static char const encoding_601[] = "601", encoding_709[] = "709";
	static char const range_limited[] = "limited", range_full[] = "full";
	encoding = encoding_601;
	range = range_limited;

	if (cs == libcamera::ColorSpace::Jpeg)
		range = range_full;
	else if (cs == libcamera::ColorSpace::Smpte170m)
		/* all good */;
	else if (cs == libcamera::ColorSpace::Rec709)
		encoding = encoding_709;
	else
		std::cerr << "DrmPreview: unexpected colour space " << libcamera::ColorSpace::toString(cs) << std::endl;
}

static int drm_set_property(int fd, int plane_id, char const *name, char const *val)
{
	drmModeObjectPropertiesPtr properties = nullptr;
	drmModePropertyPtr prop = nullptr;
	int ret = -1;
	properties = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);

	for (unsigned int i = 0; i < properties->count_props; i++)
	{
		int prop_id = properties->props[i];
		prop = drmModeGetProperty(fd, prop_id);
		if (!prop)
			continue;

		if (!drm_property_type_is(prop, DRM_MODE_PROP_ENUM) || !strstr(prop->name, name))
		{
			drmModeFreeProperty(prop);
			prop = nullptr;
			continue;
		}

		// We have found the right property from its name, now search the enum table
		// for the numerical value that corresponds to the value name that we have.
		for (int j = 0; j < prop->count_enums; j++)
		{
			if (!strstr(prop->enums[j].name, val))
				continue;

			ret = drmModeObjectSetProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, prop_id, prop->enums[j].value);
			if (ret < 0)
				std::cerr << "DrmPreview: failed to set value " << val << " for property " << name << std::endl;
			goto done;
		}

		std::cerr << "DrmPreview: failed to find value " << val << " for property " << name << std::endl;
		goto done;
	}

	std::cerr << "DrmPreview: failed to find property " << name << std::endl;
done:
	if (prop)
		drmModeFreeProperty(prop);
	if (properties)
		drmModeFreeObjectProperties(properties);
	return ret;
}

static void setup_colour_space(int fd, int plane_id, std::optional<libcamera::ColorSpace> const &cs)
{
	char const *encoding, *range;
	get_colour_space_info(cs, encoding, range);

	drm_set_property(fd, plane_id, "COLOR_ENCODING", encoding);
	drm_set_property(fd, plane_id, "COLOR_RANGE", range);
}

void DrmPreview::makeBuffer(int fd, size_t size, StreamInfo const &info, Buffer &buffer)
{
	if (first_time_)
	{
		first_time_ = false;

		setup_colour_space(drmfd_, planeId_, info.colour_space);
	}

	buffer.fd = fd;
	buffer.size = size;
	buffer.info = info;

	if (drmPrimeFDToHandle(drmfd_, fd, &buffer.bo_handle))
		throw std::runtime_error("drmPrimeFDToHandle failed for fd " + std::to_string(fd));

	uint32_t offsets[4] = {
		0, // start offset for Y plane
		info.stride * info.height, // start offset for U plane
		info.stride * info.height + (info.stride / 2) * (info.height / 2) // start offset for V plane
		};
	uint32_t pitches[4] = {
		info.stride, // stride (== width) of Y plane
		info.stride / 2, // stride (== width) of U plane
		info.stride / 2 // stride (== width) of V plane
		};
	uint32_t bo_handles[4] = { buffer.bo_handle, buffer.bo_handle, buffer.bo_handle };
	printf("makeBuffer: stride=%u width=%u height=%u pitches[0]=%u [1]=%u [2]=%u [3]=%u offsets[0]=%u [1]=%u [2]=%u [3]=%u\n",
		info.stride, info.width, info.height,
		pitches[0], pitches[1], pitches[2], pitches[3],
		offsets[0], offsets[1], offsets[2], offsets[3]
	);

	if (drmModeAddFB2(drmfd_, info.width, info.height, out_fourcc_, bo_handles, pitches, offsets, &buffer.fb_handle, 0))
		throw std::runtime_error("drmModeAddFB2 failed: " + std::string(ERRSTR));
}

void DrmPreview::Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info)
{
	Buffer &buffer = buffers_[fd];
	if (buffer.fd == -1) {
		printf("makeBuffer: fd=%d size=%u\n", fd, span.size());
		makeBuffer(fd, span.size(), info, buffer);
	}
	printf("DrmPreview::Show fd=%d last_fd=%d size=%u width=%u height=%u stride=%u buffer.width=%d buffer.height=%d\n",
		fd, last_fd_, span.size(), info.width, info.height, info.stride, buffer.info.width, buffer.info.height);

	// info.width = 1920 (camera capture width)
	// info.height = 1080 (camera capture height)
	// info.stride = 1920 (camera capture stride)
	// buffer.info.width = 1920 (this buffer frame width)
	// buffer.info.height = 1080 (this buffer frame height)
	// x_ = 100 (requested preview offset x)
	// y_ = 200 (requested preview offset y)
	// width_ = 300 (requested preview width)
	// height_ = 400 (requested preview height)
	int x_off = 0, y_off = 0;
	unsigned int w = width_, h = height_;
	// if (info.width * height_ > width_ * info.height) {
	// 	h = width_ * info.height / info.width, y_off = (height_ - h) / 2;
	// 	printf("h=%u width_=%u y_off=%d height_=%u\n", h, width_, y_off, height_);
	// } else {
	// 	w = height_ * info.width / info.height, x_off = (width_ - w) / 2;
	// 	printf("w=%u width_=%u x_off=%d height_=%u\n", w, width_, x_off, height_);
	// }

	unsigned int crtc_x;
	unsigned int crtc_y;
	if (x_off < 0 && x_off + (int)x_ < 0) {
		crtc_x = 0;
		printf("reset crtc_x to 0\n");
	} else {
		crtc_x = x_off + x_;
	}
	if (y_off < 0 && y_off + (int)y_ < 0) {
		crtc_y = 0;
		printf("reset crtc_y to 0\n");
	} else {
		crtc_y = y_off + y_;
	}
	printf("drmfd_=%d planeId=%u crtcId_=%u fb_id=%u crtc_x=%d crtc_y=%d crtc_w=%u crtc_h=%u src_x=%u src_y=%u src_w=%u src_h=%u src_w<<16=%u src_h<<16=%u\n",
		drmfd_, planeId_, crtcId_, buffer.fb_handle,
		// x_off + x_, // crtc_x
		// y_off + y_, // crtc_y
		crtc_x, // crtc_x
		crtc_y, // crtc_y
		w, // crtc_w
		h, // crtc_h
		0 << 16, // src_x in Q16.16
		0 << 16, // src_y in Q16.16
		buffer.info.width, // src_w in Q16.16
		buffer.info.height, // src_h in Q16.16
		buffer.info.width << 16, buffer.info.height << 16);
	if (drmModeSetPlane(drmfd_, planeId_, crtcId_, buffer.fb_handle,
		0, // flags
		crtc_x,
		crtc_y,
		w, // crtc_w
		h, // crtc_h
		0 << 16, // src_x in Q16.16
		0 << 16, // src_y in Q16.16
		buffer.info.width << 16, // src_w in Q16.16
		buffer.info.height << 16 // src_h in Q16.16
		)) {
		throw std::runtime_error("drmModeSetPlane failed: " + std::string(ERRSTR));
	}
	if (last_fd_ >= 0) {
		done_callback_(last_fd_);
	}
	last_fd_ = fd;
}

void DrmPreview::Reset()
{
	for (auto &it : buffers_)
		drmModeRmFB(drmfd_, it.second.fb_handle);
	buffers_.clear();
	last_fd_ = -1;
	first_time_ = true;
}

Preview *make_drm_preview(PicamOption const *options)
{
	return new DrmPreview(options);
}
