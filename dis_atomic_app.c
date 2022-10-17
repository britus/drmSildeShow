#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <sys/epoll.h>
#include <sys/reboot.h> /* Definition of LINUX_REBOOT_* constants */
#include <sys/signalfd.h>
#include <sys/select.h>
#include <signal.h>

static int cnt_call = 1;

struct drm_object
{
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	uint32_t id;
};

struct modeset_buf
{
	uint32_t width;
	uint32_t height;
	uint32_t size;
	uint32_t stride;
	uint32_t handle;
	uint8_t *map;
	uint32_t fb;
};

struct modeset_device
{
	struct modeset_device *next;
	unsigned int front_buf;
	struct modeset_buf bufs[2];

	struct drm_object connector;
	struct drm_object crtc;
	struct drm_object plane;

	drmModeModeInfo mode;
	uint32_t mode_blob_id;
	uint32_t crtc_index;

	bool pflip_pending;
	bool cleanup;

	uint8_t r, g, b;
	bool r_up, g_up, b_up;
};

static struct modeset_device *device_list = NULL;

static int modeset_open(int *out, const char *node)
{
	int fd, ret;
	uint64_t cap;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0)
	{
		ret = -errno;
		fprintf(stderr, "cannot open '%s' : %m\n", node);
		return ret;
	}

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret)
	{
		fprintf(stderr, "failed to set universal planes cap,%d\n", ret);
		return ret;
	}

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret)
	{
		fprintf(stderr, "failed to set atomic cap,%d\n", ret);
		return ret;
	}

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || !cap)
	{
		fprintf(stderr, "drm device '%s' does not support dumb buffers \n", node);
		close(fd);
		return -ENOTSUP;
	}

	if (drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) < 0 || !cap)
	{
		fprintf(stderr, "drm device '%s' does not support atomic KMS \n", node);
		close(fd);
		return -ENOTSUP;
	}

	*out = fd;
	return 0;
}

static int64_t get_property_value(int fd, drmModeObjectPropertiesPtr props, const char *name)
{
	drmModePropertyPtr prop;
	uint64_t value;
	bool found;
	int j;

	found = false;

	for (j = 0; j < props->count_props && !found; j++)
	{
		prop = drmModeGetProperty(fd, props->props[j]);
		if (!strcmp(prop->name, name))
		{
			value = props->prop_values[j];
			found = true;
		}
		drmModeFreeProperty(prop);
	}

	if (!found)
		return -1;
	return value;
}

static void modeset_get_object_properties(int fd, struct drm_object *obj, uint32_t type)
{
	const char *type_str;
	unsigned int i;

	obj->props = drmModeObjectGetProperties(fd, obj->id, type);
	if (!obj->props)
	{
		switch (type)
		{
		case DRM_MODE_OBJECT_CONNECTOR:
			type_str = "connector";
			break;
		case DRM_MODE_OBJECT_PLANE:
			type_str = "plane";
			break;
		case DRM_MODE_OBJECT_CRTC:
			type_str = "crtc";
			break;
		default:
			type_str = "unknown type";
			break;
		}

		fprintf(stderr, "cannot get %s %d properties :%s \n", type_str, obj->id, strerror(errno));
		return;
	}

	obj->props_info = calloc(obj->props->count_props, sizeof(obj->props_info));
	for (i = 0; i < obj->props->count_props; i++)
	{
		obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
	}
}

static int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj, const char *name, uint64_t value)
{
	int i;
	uint32_t prop_id = 0;

	for (i = 0; i < obj->props->count_props; i++)
	{
		if (!strcmp(obj->props_info[i]->name, name))
		{
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id == 0)
	{
		fprintf(stderr, "no object propert :%s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj->id, prop_id, value);
}

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn, struct modeset_device *dev)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	uint32_t crtc;
	struct modeset_device *iter;

	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc)
	{
		if (enc->crtc_id)
		{
			crtc = enc->crtc_id;
			for (iter = device_list; iter; iter = iter->next)
			{
				if (iter->crtc.id == crtc)
				{
					crtc = 0;
					break;
				}
			}

			if (crtc > 0)
			{
				drmModeFreeEncoder(enc);
				dev->crtc.id = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	for (i = 0; i < conn->count_encoders; ++i)
	{
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc)
		{
			fprintf(stderr, "cannot retrieve encoder %u:%u (%d):%m\n", i, conn->encoders[i], errno);
			continue;
		}

		for (j = 0; j < res->count_crtcs; ++j)
		{
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			crtc = res->crtcs[j];
			for (iter = device_list; iter; iter = iter->next)
			{
				if (iter->crtc.id == crtc)
				{
					crtc = 0;
					break;
				}
			}

			if (crtc > 0)
			{
				fprintf(stdout, "crtc %u found for encoder %u ,will need a full modeset \n", crtc, conn->encoders[i]);
				drmModeFreeEncoder(enc);
				dev->crtc.id = crtc;
				dev->crtc_index = j;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable crtc for connector %u \n", conn->connector_id);
	return -ENOENT;
}

static int modeset_find_plane(int fd, struct modeset_device *dev)
{
	drmModePlaneResPtr plane_res;
	bool found_primary = false;
	int i, ret = -EINVAL;

	plane_res = drmModeGetPlaneResources(fd);
	if (!plane_res)
	{
		fprintf(stderr, "drmModeGetPlaneResources Failed:%s\n", strerror(errno));
		return -ENOENT;
	}

	for (i = 0; (i < plane_res->count_planes) && !found_primary; i++)
	{
		int plane_id = plane_res->planes[i];

		drmModePlanePtr plane = drmModeGetPlane(fd, plane_id);
		if (!plane)
		{
			fprintf(stderr, "drmModeGetPlane(%u) failed :%s \n", plane_id, strerror(errno));
			continue;
		}

		if (plane->possible_crtcs & (1 << dev->crtc_index))
		{
			drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
			if (get_property_value(fd, props, "type") == DRM_PLANE_TYPE_PRIMARY)
			{
				found_primary = true;
				dev->plane.id = plane_id;
				ret = 0;
			}

			drmModeFreeObjectProperties(props);
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);

	if (found_primary)
		fprintf(stdout, "found primary plane ,id : %d\n", dev->plane.id);
	else
		fprintf(stdout, "couldn't find primary plane\n");
	return ret;
}

static void modeset_drm_object_finish(struct drm_object *obj)
{
	int i;
	for (i = 0; i < obj->props->count_props; i++)
		drmModeFreeProperty(obj->props_info[i]);
	free(obj->props_info);
	drmModeFreeObjectProperties(obj->props);
}

static int modeset_setup_objects(int fd, struct modeset_device *dev)
{
	struct drm_object *connector = &dev->connector;
	struct drm_object *crtc = &dev->crtc;
	struct drm_object *plane = &dev->plane;

	modeset_get_object_properties(fd, connector, DRM_MODE_OBJECT_CONNECTOR);
	if (!connector->props)
		goto out_conn;

	modeset_get_object_properties(fd, crtc, DRM_MODE_OBJECT_CRTC);
	if (!crtc->props)
		goto out_crtc;

	modeset_get_object_properties(fd, plane, DRM_MODE_OBJECT_PLANE);
	if (!plane->props)
		goto out_plane;

	return 0;

out_plane:
	modeset_drm_object_finish(crtc);
out_crtc:
	modeset_drm_object_finish(connector);
out_conn:
	return -ENOMEM;
}

static void modeset_destroy_objects(int fd, struct modeset_device *dev)
{
	modeset_drm_object_finish(&dev->connector);
	modeset_drm_object_finish(&dev->crtc);
	modeset_drm_object_finish(&dev->plane);
}

static int modeset_create_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	int ret;
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

	memset(&creq, 0, sizeof(creq));
	creq.width = buf->width;
	creq.height = buf->height;
	creq.bpp = 32;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0)
	{
		fprintf(stderr, "cannot create dumb buffer (%d):%m\n", errno);
		return -errno;
	}
	buf->stride = creq.pitch;
	buf->handle = creq.handle;
	buf->size = creq.size;

	handles[0] = buf->handle;
	pitches[0] = buf->stride;

	ret = drmModeAddFB2(fd, buf->width, buf->height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &buf->fb, 0);

	if (ret)
	{
		fprintf(stderr, "cannot create framebuffer (%d):%m\n", errno);
		ret = -errno;
		goto err_destroy;
	}

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = buf->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret)
	{
		fprintf(stderr, "cannot prepare framebuffer for mapping (%d):%m\n", errno);
		ret = -errno;
		goto err_fb;
	}

	buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
	if (buf->map == MAP_FAILED)
	{
		fprintf(stderr, "cannot mmap dumb buffer (%d):%m\n", errno);
		ret = -errno;
		goto err_fb;
	}

	memset(buf->map, 0, buf->size);

	return 0;

err_fb:
	drmModeRmFB(fd, buf->fb);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}

static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

	munmap(buf->map, buf->size);
	drmModeRmFB(fd, buf->fb);
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

static int modeset_setup_framebuffer(int fd, drmModeConnector *conn, struct modeset_device *dev)
{
	int i, ret;

	for (i = 0; i < 2; i++)
	{
		dev->bufs[i].width = conn->modes[0].hdisplay;
		dev->bufs[i].height = conn->modes[0].vdisplay;

		ret = modeset_create_fb(fd, &dev->bufs[i]);
		if (ret)
		{
			if (i == 1)
			{
				modeset_destroy_fb(fd, &dev->bufs[0]);
			}
			return ret;
		}
	}

	return 0;
}

static void modeset_device_destory(int fd, struct modeset_device *dev)
{
	modeset_destroy_objects(fd, dev);

	modeset_destroy_fb(fd, &dev->bufs[0]);
	modeset_destroy_fb(fd, &dev->bufs[1]);

	drmModeDestroyPropertyBlob(fd, dev->mode_blob_id);

	free(dev);
}

static struct modeset_device *modeset_device_create(int fd, drmModeRes *res, drmModeConnector *conn)
{
	int ret;
	struct modeset_device *dev;

	dev = malloc(sizeof(*dev));
	memset(dev, 0, sizeof(*dev));
	dev->connector.id = conn->connector_id;

	if (conn->connection != DRM_MODE_CONNECTED)
	{
		fprintf(stderr, "ignoring unused connector %u\n", conn->connector_id);
		goto dev_error;
	}

	if (conn->count_modes == 0)
	{
		fprintf(stderr, "no valid mode for connector %u\n", conn->connector_id);
		goto dev_error;
	}

	memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
	if (drmModeCreatePropertyBlob(fd, &dev->mode, sizeof(dev->mode), &dev->mode_blob_id) != 0)
	{
		fprintf(stderr, "couldn't create a blob property\n");
		goto dev_error;
	}

	ret = modeset_find_crtc(fd, res, conn, dev);
	if (ret)
	{
		fprintf(stderr, "no valid crtc for connector %u\n", conn->connector_id);
		goto dev_blob;
	}

	ret = modeset_find_plane(fd, dev);
	if (ret)
	{
		fprintf(stderr, "no valid plane for crtc %u\n", dev->crtc.id);
		goto dev_blob;
	}

	ret = modeset_setup_objects(fd, dev);
	if (ret)
	{
		fprintf(stderr, "cannnot get properties \n");
		goto dev_obj;
	}

	ret = modeset_setup_framebuffer(fd, conn, dev);
	if (ret)
	{
		fprintf(stderr, "connot create framebuffers for connector %u\n", conn->connector_id);
		goto dev_obj;
	}

	fprintf(stderr, "mode for connector %u is %ux%u\n", conn->connector_id, dev->bufs[0].width, dev->bufs[0].height);
	return dev;

dev_obj:
	modeset_destroy_objects(fd, dev);
dev_blob:
	drmModeDestroyPropertyBlob(fd, dev->mode_blob_id);
dev_error:
	free(dev);
	return NULL;
}

static int modeset_prepare(int fd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_device *dev;

	res = drmModeGetResources(fd);
	if (!res)
	{
		fprintf(stderr, "cannot retrieve DRM resources (%d):%m\n", errno);
		return -errno;
	}

	for (i = 0; i < res->count_connectors; ++i)
	{
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn)
		{
			fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d):%m\n", i, res->connectors[i], errno);
			continue;
		}

		dev = modeset_device_create(fd, res, conn);
		drmModeFreeConnector(conn);
		if (!dev)
			continue;

		dev->next = device_list;
		device_list = dev;
	}
	if (!device_list)
	{
		fprintf(stderr, "couldn't create any devices\n");
		return -1;
	}

	drmModeFreeResources(res);
	return 0;
}

static int modeset_atomic_prepare_commit(int fd, struct modeset_device *dev, drmModeAtomicReq *req)
{
	struct drm_object *plane = &dev->plane;
	struct modeset_buf *buf = &dev->bufs[dev->front_buf ^ 1];

	if (set_drm_object_property(req, &dev->connector, "CRTC_ID", dev->crtc.id) < 0)
		return -1;

	if (set_drm_object_property(req, &dev->crtc, "MODE_ID", dev->mode_blob_id) < 0)
		return -1;

	if (set_drm_object_property(req, &dev->crtc, "ACTIVE", 1) < 0)
		return -1;

	if (set_drm_object_property(req, plane, "FB_ID", buf->fb) < 0)
		return -1;

	if (set_drm_object_property(req, plane, "CRTC_ID", dev->crtc.id) < 0)
		return -1;

	if (set_drm_object_property(req, plane, "SRC_X", 0) < 0)
		return -1;

	if (set_drm_object_property(req, plane, "SRC_Y", 0) < 0)
		return -1;

	if (set_drm_object_property(req, plane, "SRC_W", buf->width << 16) < 0)
		return -1;

	if (set_drm_object_property(req, plane, "SRC_H", buf->height << 16) < 0)
		return -1;

	if (set_drm_object_property(req, plane, "CRTC_X", 0) < 0)
		return -1;

	if (set_drm_object_property(req, plane, "CRTC_Y", 0) < 0)
		return -1;

	if (set_drm_object_property(req, plane, "CRTC_W", buf->width) < 0)
		return -1;

	if (set_drm_object_property(req, plane, "CRTC_H", buf->height) < 0)
		return -1;

	return 0;
}

#if 0
static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod)
{
	uint8_t next;

	next = cur + (*up ? 1 : -1) * (rand() % mod);
	if ((*up && next < cur) || (!*up && next > cur))
	{
		*up = !*up;
		next = cur;
	}

	return next;
}
#endif

static void modeset_draw_framebuffer(struct modeset_device *dev)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;
	char time_left[5];
	cairo_t *cr;
	cairo_surface_t *surface, *image;

	cairo_text_extents_t te;

	buf = &dev->bufs[dev->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j)
	{
		for (k = 0; k < buf->width; ++k)
		{
			off = buf->stride * j + k * 4;
			*(uint32_t *)&buf->map[off] = (0 << 16) | (0 << 8) | 0;
		}
	}

	image = cairo_image_surface_create_from_png("/etc/boot/boot-01.png");
	surface = cairo_image_surface_create_for_data(buf->map, CAIRO_FORMAT_ARGB32,
												  buf->width, buf->height, buf->stride);
	cr = cairo_create(surface);
	cairo_set_source_rgb(cr, 255.0, 255.0, 255.0);
	cairo_select_font_face(cr, "Georgia",
			CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 100);

	if (cnt_call <= 1)
	{
		//	itoa(cnt_call,time_left,10);
		sprintf(time_left, "%d", 10 - cnt_call);

		cairo_text_extents(cr, "a", &te);
		cairo_move_to(cr, 350, buf->height / 2);
		cairo_show_text(cr, "Please wait, staring CarIOS...");
		cairo_text_extents(cr, "a", &te);
		cairo_move_to(cr, buf->width / 2, buf->height / 2 + 150);
		cairo_show_text(cr, time_left);
		cnt_call++;
	}
	else
	{
		cairo_set_source_surface(cr, image, 0, 0);
		cairo_paint(cr);
	}
}

static void modeset_draw_output(int fd, struct modeset_device *dev)
{
	drmModeAtomicReq *req;
	int ret, flags;

	modeset_draw_framebuffer(dev);
	req = drmModeAtomicAlloc();
	ret = modeset_atomic_prepare_commit(fd, dev, req);
	if (ret < 0)
	{
		fprintf(stderr, "prepare atomic commit failed, %d \n", errno);
		return;
	}

	flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	drmModeAtomicFree(req);

	if (ret < 0)
	{
		fprintf(stderr, "atomic commit failed ,%d\n", errno);
		return;
	}

	dev->front_buf ^= 1;
	dev->pflip_pending = true;
}

static void modeset_page_flip_event(int fd, unsigned int frame, unsigned int sec, unsigned int usec, unsigned int crtc_id, void *data)
{
	struct modeset_device *dev, *iter;

	dev = NULL;
	for (iter = device_list; iter; iter = iter->next)
	{
		if (iter->crtc.id == crtc_id)
		{
			dev = iter;
			break;
		}
	}

	if (dev == NULL)
		return;

	dev->pflip_pending = false;
	if (!dev->cleanup)
		modeset_draw_output(fd, dev);
}

static int modeset_perform_modeset(int fd)
{
	int ret, flags;
	struct modeset_device *iter;
	drmModeAtomicReq *req;

	req = drmModeAtomicAlloc();
	for (iter = device_list; iter; iter = iter->next)
	{
		ret = modeset_atomic_prepare_commit(fd, iter, req);
		if (ret < 0)
			break;
	}

	if (ret < 0)
	{
		fprintf(stderr, "prepare atomic commit failed,%d\n", errno);
		return ret;
	}

	flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	if (ret < 0)
	{
		fprintf(stderr, "test-only atomic commit failed,%d\n", errno);
		drmModeAtomicFree(req);
		return ret;
	}

	for (iter = device_list; iter; iter = iter->next)
	{
		iter->r = rand() % 0xff;
		iter->g = rand() % 0xff;
		iter->b = rand() % 0xff;
		iter->r_up = iter->g_up = iter->b_up = true;

		modeset_draw_framebuffer(iter);
	}

	flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	if (ret < 0)
		fprintf(stderr, "test-only atomic commit failed,%d\n", errno);

	drmModeAtomicFree(req);
	return ret;
}

static void modeset_draw(int fd)
{
	drmEventContext ev;
	int ret;

	memset(&ev, 0, sizeof(ev));
	ev.version = 3;
	ev.page_flip_handler2 = modeset_page_flip_event;

	modeset_perform_modeset(fd);
	drmHandleEvent(fd, &ev);
}

static void modeset_cleanup(int fd)
{
	struct modeset_device *iter;
	drmEventContext ev;
	int ret;

	memset(&ev, 0, sizeof(ev));
	ev.version = 3;
	ev.page_flip_handler2 = modeset_page_flip_event;

	while (device_list)
	{
		iter = device_list;

		iter->cleanup = true;
		fprintf(stderr, "wait for pending page-flip to complete...\n");
		while (iter->pflip_pending)
		{
			ret = drmHandleEvent(fd, &ev);
			if (ret)
				break;
		}

		device_list = iter->next;

		modeset_device_destory(fd, iter);
	}
}

#if 0
static int g_terminate = 0;
void signal_handler(int signo)
{
    switch (signo) {
        case SIGUSR1:
        case SIGUSR2:
        case SIGTERM:
        case SIGKILL:
            g_terminate = 1;
            break;
        default:
            break;
    }
}

/* Install OS signal handler */
sighandler_t sig_install_handler(int sig, sighandler_t handler)
{
    struct sigaction add_sig;

    /* get current signal action */
    if (sigaction(sig, NULL, &add_sig) < 0) {
        qCritical() << "sigaction() #1: failed:" << strerror(errno);
        return SIG_ERR;
    }

    add_sig.sa_handler = handler;

    /* set signal number to signal set mask */
    if (sigaddset(&add_sig.sa_mask, sig)) {
        qCritical() << "sigaddset() #2: failed:" << strerror(errno);
        return SIG_ERR;
    }

    /* set our signal action */
    add_sig.sa_flags = SA_RESTART;
    if (sigaction(sig, &add_sig, NULL) < 0) {
        qCritical() << "sigaction() #3: failed:" << strerror(errno);
        return SIG_ERR;
    }

    return add_sig.sa_handler;
}

static inline void catchsignals()
{
    if (sig_install_handler(SIGUSR1, signal_handler) == SIG_ERR) {
        return -EINVAL;
    }
    if (sig_install_handler(SIGUSR2, signal_handler) == SIG_ERR) {
        return -EINVAL;
    }
    if (sig_install_handler(SIGTERM, signal_handler) == SIG_ERR) {
        return -EINVAL;
    }
}
#endif

static int fd_epoll;
static int register_signals(sigset_t* prevSigset, sigset_t* newSigset)
{
    struct epoll_event event;
    sigset_t mask;
    int sfd;

    sigemptyset(newSigset);
    sigemptyset(prevSigset);

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGKILL);
    sigaddset(&mask, SIGTERM);

    if (sigprocmask(SIG_SETMASK, &mask, prevSigset) == -1) {
        fprintf(stderr, "Failed to setup signal proc mask!\n");
        return -EINVAL;
    }

    sfd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (sfd == -1) {
        fprintf(stderr, "Failed to get signal fd!\n");
        return -EINVAL;
    }

    /* Add fd to be monitored by epoll */
    event.events = EPOLLIN;
    event.data.fd = sfd;
    if (epoll_ctl(fd_epoll, EPOLL_CTL_ADD, sfd, &event) == -1) {
        fprintf(stderr, "Failed to register signal: %d\n", sfd);
        close(sfd);
        return -EINVAL;
    }

    (*newSigset) = mask;
    return sfd;
}

static int fd_signals;
static sigset_t g_sigset_prev;
static sigset_t g_sigset_new;

static inline int catch_signals()
{    
	/* allocate epoll file descriptor */
    fd_epoll = epoll_create(6);
    if (fd_epoll < 0) {
        return -EINVAL;
    }

    /* monitor process signals, so we can terminate thread
     * loop with registred SIGxxx signals. This terminates
     * the CarIOS main process with SIGINT too. */
    if ((fd_signals = register_signals(&g_sigset_prev, &g_sigset_new)) < 0) {
        fprintf(stderr, "Failed to register signals. err=%d\n", errno);
        close(fd_epoll);
        fd_epoll = -1;
        return -EINVAL;
    }

	return 0;
}

/* check epoll event error flags  */
static int check_event_flags(unsigned long flags)
{
    bool result = 0;
    if (!(flags & EPOLLIN) && ((flags & EPOLLERR) || (flags & EPOLLHUP))) {
        if (flags & EPOLLERR) {
            fprintf(stderr, "EPOLLERR detected.\n");
            result = -EINVAL;
        }
        if (flags & EPOLLHUP) {
            fprintf(stderr, "EPOLLHUP detected.\n");
            result = -EINVAL;
        }
    }
    return result;
}

static int should_terminate(const int fd)
{
    struct signalfd_siginfo sfd_si;
    int ret;

    /* read signals */
    while ((ret = read(fd, &sfd_si, sizeof(sfd_si)))) {

        if (ret <= 0)
            break;

        switch (sfd_si.ssi_signo) {
			case SIGHUP:
			case SIGUSR1:
			case SIGUSR2:
            case SIGTERM:
            case SIGKILL:
            case SIGINT: {
                fprintf(stderr, "Terminate signal: %d\n", sfd_si.ssi_signo);
				return 1;
            }
            default: {
                fprintf(stderr, "Unhandled signal: %d\n", sfd_si.ssi_signo);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
	int ret, fd;
	const char *card;
	struct epoll_event event;

	if (argc > 1)
		card = argv[1];
	else
		card = "/dev/dri/card0";

	fprintf(stderr, "using card '%s'\n", card);

	ret = catch_signals();
	if (ret)
		return EXIT_FAILURE;

	ret = modeset_open(&fd, card);
	if (ret)
		goto out_return;

	ret = modeset_prepare(fd);
	if (ret)
		goto out_close;

	modeset_draw(fd);
	
	/* main loop SIGUSR1, SIGUSR2, SIGTERM exit loop */
	while(1) { 
        if ((ret = epoll_pwait(fd_epoll, &event, 1, -1, (const __sigset_t*)&g_sigset_new)) < 0) {
            fprintf(stderr, "epoll_wait() failed. terminate with err: %d\n", errno);
            break;
        }
		if (check_event_flags(event.events)) {
			break;
		}
		/* skip invalid FDs */
        if (event.data.fd == 0) {
            continue;
		}
        /* event on signalfd */
        if (event.data.fd == fd_signals) {
            if (should_terminate(fd_signals))
                break;
            /* skip event */
            continue;
        }
	}
	
	modeset_cleanup(fd);
	ret = 0;

out_close:
	close(fd);
	
out_return:
	close(fd_epoll);
	if (ret)
	{
		errno = -ret;
		fprintf(stderr, "modeset failed with error %d\n", errno);
	}
	return ret;
}
