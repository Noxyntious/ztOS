// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The raw cameras of Qualcomm's CAMSS (sdm845 phones such as the Poco F1): a sensor
 * on a CSI PHY, through CSID and the VFE's raw dump path (RDI) to a V4L2 node, with
 * no ISP. The RDI cannot write 10-bit Bayer unpacked, so frames come MIPI-packed:
 * four pixels in five bytes, the four high bytes and then their low two bits each.
 * raw-source.h's GPU pipeline reads them so, straight from each buffer's DMA-BUF.
 *
 * The pipeline is found, not named: the sensor is whatever is fixed to a CSI PHY,
 * routed through CSID 0 to VFE 0's first RDI. The sensor's flips are turned off (on
 * the Poco F1 that reads the rear upright for the viewfinder), and its Bayer order is
 * read back from the sensor.
 * Viewfinder and photo are the same mode, the sensor's largest: the IMX363's 1080p
 * mode scales on the chip and its colours come out mixed to grey, whatever the
 * white balance does with them.
 * Exposure and gain are the sensor's V4L2 controls; focus is the lens' when the
 * device tree joins one to the sensor (lens-focus).
 * Included by raw-source.h, after its state.
 */
#include <linux/v4l2-subdev.h>

#define CS_MEDIA	"/dev/media0"
#define CS_CSID		"msm_csid0"
#define CS_RDI		"msm_vfe0_rdi0"
#define CS_VIDEO	"msm_vfe0_video0"

static struct {
	int probed;			/* 0 not yet, 1 found, -1 none */
	char sensor[64], phy[32];	/* media entity names */
	char sensor_dev[32], video_dev[32];
	int sub, lens, vid;
	int nbuf, held;
	void *map[RAW_BUFS];
	size_t len[RAW_BUFS];
	int dmabuf[RAW_BUFS];
	uint32_t seq, errors;
	int full_w, full_h;
	int lens_min, lens_max;
	long line_len;			/* pixels per line, blanking included: exposure's unit */
	double k;			/* the last mode change: old line length over new */
	uint32_t pixfmt;
	bool streaming;
} cs = { .sub = -1, .lens = -1, .vid = -1, .held = -1 };

static bool cs_ioctl(int fd, unsigned long req, void *arg) {
	int r;
	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r == 0;
}

static int cs_ctrl_get(int fd, uint32_t id, int *min, int *max) {
	struct v4l2_queryctrl q = { .id = id };
	if (min || max) {
		if (!cs_ioctl(fd, VIDIOC_QUERYCTRL, &q)) {
			return -1;
		}
		if (min) *min = q.minimum;
		if (max) *max = q.maximum;
	}
	struct v4l2_control c = { .id = id };
	return cs_ioctl(fd, VIDIOC_G_CTRL, &c) ? c.value : -1;
}

static bool cs_ctrl_set(int fd, uint32_t id, int value) {
	struct v4l2_control c = { .id = id, .value = value };
	return fd >= 0 && cs_ioctl(fd, VIDIOC_S_CTRL, &c);
}

/* An entity's device node, as media-ctl names it. */
static bool cs_node(const char *entity, char *out, size_t n) {
	char cmd[160], line[64];
	snprintf(cmd, sizeof(cmd), "media-ctl -d " CS_MEDIA " -e '%s' 2>/dev/null", entity);
	FILE *p = popen(cmd, "r");
	bool ok = p && fgets(line, sizeof(line), p) && line[0] == '/';
	if (p) pclose(p);
	if (ok) {
		line[strcspn(line, "\n")] = 0;
		snprintf(out, n, "%s", line);
	}
	return ok;
}

/* Whether this phone has a CAMSS camera: the sensor, its PHY and the capture node. */
static bool cs_probe(void) {
	if (cs.probed) {
		return cs.probed > 0;
	}
	cs.probed = -1;
	if (access(CS_MEDIA, R_OK) != 0) {
		return false;
	}
	FILE *p = popen("media-ctl -d " CS_MEDIA " -p 2>/dev/null", "r");
	char line[256], entity[64] = "";
	while (p && fgets(line, sizeof(line), p)) {
		char name[64];
		if (sscanf(line, "- entity %*d: %63[^(]", name) == 1) {
			size_t l = strlen(name);
			while (l && name[l - 1] == ' ') name[--l] = 0;
			snprintf(entity, sizeof(entity), "%s", name);
		} else if (!cs.sensor[0] && strncmp(entity, "msm_csiphy", 10) == 0 &&
				sscanf(line, " <- \"%63[^\"]\":0 [ENABLED,IMMUTABLE]", name) == 1) {
			snprintf(cs.sensor, sizeof(cs.sensor), "%s", name);
			snprintf(cs.phy, sizeof(cs.phy), "%s", entity);
		}
	}
	if (p) pclose(p);
	if (!cs.sensor[0] || !cs_node(cs.sensor, cs.sensor_dev, sizeof(cs.sensor_dev)) ||
			!cs_node(CS_VIDEO, cs.video_dev, sizeof(cs.video_dev))) {
		return false;
	}
	cs.probed = 1;
	return true;
}

/* The lens: whichever subdevice has an absolute focus control. */
static void cs_find_lens(void) {
	DIR *d = opendir("/dev");
	struct dirent *e;
	while (d && cs.lens < 0 && (e = readdir(d))) {
		if (strncmp(e->d_name, "v4l-subdev", 10) != 0) continue;
		char path[300];
		snprintf(path, sizeof(path), "/dev/%s", e->d_name);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		struct v4l2_queryctrl q = { .id = V4L2_CID_FOCUS_ABSOLUTE };
		if (cs_ioctl(fd, VIDIOC_QUERYCTRL, &q) && !(q.flags & V4L2_CTRL_FLAG_DISABLED)) {
			cs.lens = fd;
			cs.lens_min = q.minimum;
			cs.lens_max = q.maximum;
		} else {
			close(fd);
		}
	}
	if (d) closedir(d);
}

static void cs_buffers_free(void) {
	raw_images_free();	/* the GPU's views of them first */
	for (int i = 0; i < RAW_BUFS; i++) {
		if (cs.map[i]) munmap(cs.map[i], cs.len[i]);
		if (cs.dmabuf[i] > 0) close(cs.dmabuf[i]);
		cs.map[i] = NULL;
		cs.dmabuf[i] = -1;
	}
	if (cs.vid >= 0) {
		struct v4l2_requestbuffers rb = { .count = 0, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP };
		cs_ioctl(cs.vid, VIDIOC_REQBUFS, &rb);
	}
	cs.nbuf = 0;
	cs.held = -1;
}

static void cs_stop(void) {
	if (cs.streaming) {
		int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		cs_ioctl(cs.vid, VIDIOC_STREAMOFF, &type);
		cs.streaming = false;
	}
	cs_buffers_free();
}

/* Every pad from the sensor to the RDI at one size, in the sensor's own Bayer code. */
static bool cs_pipeline(int w, int h, uint32_t code) {
	static const struct { uint32_t code; const char *name; } names[] = {
		{ MEDIA_BUS_FMT_SBGGR10_1X10, "SBGGR10_1X10" }, { MEDIA_BUS_FMT_SGBRG10_1X10, "SGBRG10_1X10" },
		{ MEDIA_BUS_FMT_SGRBG10_1X10, "SGRBG10_1X10" }, { MEDIA_BUS_FMT_SRGGB10_1X10, "SRGGB10_1X10" },
	};
	const char *fmt = NULL;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (names[i].code == code) fmt = names[i].name;
	}
	if (!fmt) {
		fprintf(stderr, "ztcamera: CAMSS: sensor code 0x%x is not 10-bit Bayer\n", code);
		return false;
	}
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
		"m() { media-ctl -d " CS_MEDIA " \"$@\" || exit 1; }; "
		"m -l '\"%s\":1->\"" CS_CSID "\":0[1]' && m -l '\"" CS_CSID "\":1->\"" CS_RDI "\":0[1]' && "
		"m -V '\"%s\":0[fmt:%s/%dx%d]' && m -V '\"%s\":0[fmt:%s/%dx%d]' && "
		"m -V '\"" CS_CSID "\":0[fmt:%s/%dx%d]' && m -V '\"" CS_CSID "\":1[fmt:%s/%dx%d]' && "
		"m -V '\"" CS_RDI "\":0[fmt:%s/%dx%d]'",
		cs.phy, cs.sensor, fmt, w, h, cs.phy, fmt, w, h, fmt, w, h, fmt, w, h, fmt, w, h);
	if (system(cmd) != 0) {
		fprintf(stderr, "ztcamera: CAMSS: media pipeline at %dx%d failed\n", w, h);
		return false;
	}
	return true;
}

/* One mode, streaming: the pipeline, the node's format, buffers out as DMA-BUFs. */
static bool cs_mode(int w, int h) {
	long was_line = cs.line_len;
	cs_stop();
	struct v4l2_subdev_format sf = { .which = V4L2_SUBDEV_FORMAT_ACTIVE, .pad = 0 };
	if (!cs_ioctl(cs.sub, VIDIOC_SUBDEV_G_FMT, &sf)) {
		return false;
	}
	uint32_t code = sf.format.code;
	if (!cs_pipeline(w, h, code)) {
		return false;
	}
	/* red's place in the 2x2 cell, and the packed format of that order */
	switch (code) {
	case MEDIA_BUS_FMT_SRGGB10_1X10: raw.first[0] = 0; raw.first[1] = 0; cs.pixfmt = V4L2_PIX_FMT_SRGGB10P; break;
	case MEDIA_BUS_FMT_SGRBG10_1X10: raw.first[0] = 1; raw.first[1] = 0; cs.pixfmt = V4L2_PIX_FMT_SGRBG10P; break;
	case MEDIA_BUS_FMT_SGBRG10_1X10: raw.first[0] = 0; raw.first[1] = 1; cs.pixfmt = V4L2_PIX_FMT_SGBRG10P; break;
	default: raw.first[0] = 1; raw.first[1] = 1; cs.pixfmt = V4L2_PIX_FMT_SBGGR10P; break;
	}
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.pixelformat = cs.pixfmt;
	f.fmt.pix_mp.num_planes = 1;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (!cs_ioctl(cs.vid, VIDIOC_S_FMT, &f) || f.fmt.pix_mp.pixelformat != cs.pixfmt) {
		fprintf(stderr, "ztcamera: CAMSS: the capture node will not take %dx%d packed Bayer\n", w, h);
		return false;
	}
	struct v4l2_requestbuffers rb = { .count = RAW_BUFS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP };
	if (!cs_ioctl(cs.vid, VIDIOC_REQBUFS, &rb) || rb.count < 2) {
		return false;
	}
	cs.nbuf = rb.count > RAW_BUFS ? RAW_BUFS : rb.count;
	for (int i = 0; i < cs.nbuf; i++) {
		struct v4l2_plane pl = { 0 };
		struct v4l2_buffer b = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP, .m.planes = &pl, .length = 1 };
		if (!cs_ioctl(cs.vid, VIDIOC_QUERYBUF, &b)) {
			cs_buffers_free();
			return false;
		}
		cs.len[i] = pl.length;
		cs.map[i] = mmap(NULL, pl.length, PROT_READ, MAP_SHARED, cs.vid, pl.m.mem_offset);
		if (cs.map[i] == MAP_FAILED) {
			cs.map[i] = NULL;
			cs_buffers_free();
			return false;
		}
		struct v4l2_exportbuffer eb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .index = i,
			.plane = 0, .flags = O_RDONLY | O_CLOEXEC };
		cs.dmabuf[i] = cs_ioctl(cs.vid, VIDIOC_EXPBUF, &eb) ? eb.fd : -1;
		if (!cs_ioctl(cs.vid, VIDIOC_QBUF, &b)) {
			cs_buffers_free();
			return false;
		}
	}
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (!cs_ioctl(cs.vid, VIDIOC_STREAMON, &type)) {
		fprintf(stderr, "ztcamera: CAMSS: stream on at %dx%d: %s\n", w, h, strerror(errno));
		cs_buffers_free();
		return false;
	}
	cs.streaming = true;
	raw.info.w = w;
	raw.info.h = h;
	raw.info.stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
	raw.info.area = f.fmt.pix_mp.plane_fmt[0].sizeimage;
	raw.info.index = 0;
	raw.info.bayer = 0;
	raw.info.stopped = 0;
	raw.packed = true;
	/* exposure counts lines; a line is the width and its blanking */
	int hb = cs_ctrl_get(cs.sub, V4L2_CID_HBLANK, NULL, NULL);
	cs.line_len = w + (hb > 0 ? hb : 0);
	cs.k = was_line > 0 ? (double)was_line / cs.line_len : 1;
	int lo, hi;
	if (cs_ctrl_get(cs.sub, V4L2_CID_EXPOSURE, &lo, &hi) >= 0) {
		raw.coarse_max = hi;
	}
	return true;
}

static void cs_exposure(int lines, int g) {
	cs_ctrl_set(cs.sub, V4L2_CID_EXPOSURE, lines < 1 ? 1 : lines > raw.coarse_max ? raw.coarse_max : lines);
	/* 32 is unity, as the S9's; the Sony gain code is 512 - 512 / gain */
	int code = g <= 32 ? 0 : 512 - 16384 / g;
	int lo = 0, hi = 480;
	cs_ctrl_get(cs.sub, V4L2_CID_ANALOGUE_GAIN, &lo, &hi);
	cs_ctrl_set(cs.sub, V4L2_CID_ANALOGUE_GAIN, code < lo ? lo : code > hi ? hi : code);
}

/* The S9 driver's text commands, for the parts of raw-source.h that speak them. */
static bool cs_say(const char *cmd) {
	int a, b;
	if (strcmp(cmd, "rear") == 0 || strcmp(cmd, "rearfull") == 0) {
		/* one mode for both: no switch, so the photo is the frame after the shutter */
		return (int)raw.info.w == cs.full_w || cs_mode(cs.full_w, cs.full_h);
	}
	if (sscanf(cmd, "exp %d %d", &a, &b) == 2) {
		cs_exposure(a, b);
		return true;
	}
	if (sscanf(cmd, "af %d", &a) == 1) {
		return cs_ctrl_set(cs.lens, V4L2_CID_FOCUS_ABSOLUTE,
			cs.lens_min + (int)((long)a * (cs.lens_max - cs.lens_min) / 511));
	}
	return false;	/* no front camera, no aperture */
}

/* The newest frame; older ones go straight back to the driver. */
static bool cs_next(void) {
	for (;;) {
		struct v4l2_plane pl = { 0 };
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
			.m.planes = &pl, .length = 1 };
		if (!cs_ioctl(cs.vid, VIDIOC_DQBUF, &b)) {
			return errno == EAGAIN;
		}
		if (b.flags & V4L2_BUF_FLAG_ERROR) {
			raw.info.errors = ++cs.errors;
			cs_ioctl(cs.vid, VIDIOC_QBUF, &b);
			continue;
		}
		if (cs.held >= 0) {
			struct v4l2_plane hp = { 0 };
			struct v4l2_buffer h = { .index = cs.held, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				.memory = V4L2_MEMORY_MMAP, .m.planes = &hp, .length = 1 };
			cs_ioctl(cs.vid, VIDIOC_QBUF, &h);
		}
		cs.held = b.index;
		raw.info.index = b.index;
		raw.info.seq = ++cs.seq;
	}
}

static void cs_close(void) {
	cs_stop();
	if (cs.vid >= 0) close(cs.vid);
	if (cs.sub >= 0) close(cs.sub);
	if (cs.lens >= 0) close(cs.lens);
	cs.vid = cs.sub = cs.lens = -1;
	cs.line_len = 0;
}

static bool cs_open(void) {
	cs_close();
	if (!cs_probe()) {
		return false;
	}
	cs.sub = open(cs.sensor_dev, O_RDWR | O_CLOEXEC);
	cs.vid = open(cs.video_dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (cs.sub < 0 || cs.vid < 0) {
		fprintf(stderr, "ztcamera: CAMSS: %s or %s: %s\n", cs.sensor_dev, cs.video_dev, strerror(errno));
		cs_close();
		return false;
	}
	for (int i = 0; i < RAW_BUFS; i++) cs.dmabuf[i] = -1;
	/* no flips: the IMX363's read upright this way (both on turned the picture over), and
	 * its Bayer order then stays RGGB in every mode (flipped, preview and photo differed) */
	cs_ctrl_set(cs.sub, V4L2_CID_HFLIP, 0);
	cs_ctrl_set(cs.sub, V4L2_CID_VFLIP, 0);
	/* the largest size the sensor offers is the photo's */
	struct v4l2_subdev_format sf = { .which = V4L2_SUBDEV_FORMAT_ACTIVE, .pad = 0 };
	cs_ioctl(cs.sub, VIDIOC_SUBDEV_G_FMT, &sf);
	cs.full_w = cs.full_h = 0;
	for (unsigned i = 0; i < 16; i++) {
		struct v4l2_subdev_frame_size_enum fse = { .index = i, .pad = 0, .code = sf.format.code,
			.which = V4L2_SUBDEV_FORMAT_ACTIVE };
		if (!cs_ioctl(cs.sub, VIDIOC_SUBDEV_ENUM_FRAME_SIZE, &fse)) break;
		if ((int)(fse.max_width * fse.max_height) > cs.full_w * cs.full_h) {
			cs.full_w = fse.max_width;
			cs.full_h = fse.max_height;
		}
	}
	cs_find_lens();
	if (!cs.full_w || !cs_mode(cs.full_w, cs.full_h)) {
		cs_close();
		return false;
	}
	cs.seq = 0;
	cs.errors = 0;
	fprintf(stderr, "ztcamera: CAMSS: %s at %ux%u (photo %dx%d), %s\n", cs.sensor, raw.info.w, raw.info.h,
		cs.full_w, cs.full_h, cs.lens >= 0 ? "with focus" : "fixed focus");
	return true;
}
