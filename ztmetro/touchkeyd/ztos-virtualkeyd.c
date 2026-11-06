// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztos-virtualkeyd: capacitive keys that are part of the touchscreen.
 *
 * On phones like the Redmi 3S (xiaomi-land) the keys under the glass are
 * touches the touchscreen reports outside the panel (the FT5306 puts them at
 * y = 2000 on a 1280-line panel). The compositor pins such a touch to the
 * panel's nearest edge, so a key presses whatever is drawn just above it.
 * Android turns them into keys from /sys/board_properties/virtualkeys.*;
 * this does it from /etc/ztos/virtualkeys.conf:
 *
 *	# the touchscreen, by its name in /sys/class/input/input<N>/name
 *	device = generic ft5x06 (f1)
 *	# a key: its name, and the box in the touchscreen's own units, x0 y0 x1 y1
 *	key = KEY_MENU 0 1280 359 2200
 *	key = KEY_HOMEPAGE 360 1280 719 2200
 *	key = KEY_BACK 720 1280 1079 2200
 *
 * The touchscreen is grabbed and copied through uinput, unchanged, except
 * for a contact that starts in a key's box: that one is the key, pressed while
 * the contact lasts, on a second uinput device ("ztOS virtual keys"), and the
 * touchscreen's copy never sees it. A contact that starts on the panel stays
 * a touch wherever it goes. ztMETRO reads KEY_BACK and KEY_HOMEPAGE as the
 * phone's own keys and draws no nav bar; it looks once, as it starts, so this
 * runs before the session (greetd).
 *
 * Usage: ztos-virtualkeyd [-v] [config]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CONFIG		"/etc/ztos/virtualkeys.conf"
#define MAX_KEYS	8
#define MAX_SLOTS	16
#define MAX_FRAME	256
#define BITS(n)		(((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
#define TEST(bit, a)	(((a)[(bit) / (8 * sizeof(long))] >> ((bit) % (8 * sizeof(long)))) & 1)

struct vkey { int code, x0, y0, x1, y1; };
static struct vkey keys[MAX_KEYS];
static int n_keys;
static char device_name[128];
static bool verbose;

/* the contacts, by slot: where they are, and the key one stands for */
static struct { int id, x, y, key; bool fresh; } slot[MAX_SLOTS];
static int cur_slot;

static const struct { const char *name; int code; } names[] = {
	{ "KEY_BACK", KEY_BACK }, { "KEY_HOMEPAGE", KEY_HOMEPAGE }, { "KEY_MENU", KEY_MENU },
	{ "KEY_APPSELECT", KEY_APPSELECT }, { "KEY_SEARCH", KEY_SEARCH }, { "KEY_HOME", KEY_HOME },
};

static int key_code(const char *name)
{
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(name, names[i].name))
			return names[i].code;
	return -1;
}

static bool load(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256];
	if (!f) {
		fprintf(stderr, "ztos-virtualkeyd: %s: %s\n", path, strerror(errno));
		return false;
	}
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *v;
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		v = eq + 1 + strspn(eq + 1, " \t");
		v[strcspn(v, "\r\n")] = 0;
		if (!strncmp(line + strspn(line, " \t"), "device", 6)) {
			snprintf(device_name, sizeof(device_name), "%s", v);
		} else if (!strncmp(line + strspn(line, " \t"), "key", 3) && n_keys < MAX_KEYS) {
			char name[32];
			int x0, y0, x1, y1;
			if (sscanf(v, "%31s %d %d %d %d", name, &x0, &y0, &x1, &y1) == 5 && key_code(name) >= 0)
				keys[n_keys++] = (struct vkey){ key_code(name), x0, y0, x1, y1 };
			else
				fprintf(stderr, "ztos-virtualkeyd: not understood: key = %s\n", v);
		}
	}
	fclose(f);
	return device_name[0] && n_keys;
}

/* the touchscreen's event device, by its name */
static int open_device(char *path, size_t size)
{
	DIR *d = opendir("/sys/class/input");
	struct dirent *e;
	int fd = -1;
	while (d && fd < 0 && (e = readdir(d))) {
		char p[300], name[128] = "";
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/sys/class/input/%s/device/name", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f))
			name[strcspn(name, "\n")] = 0;
		fclose(f);
		if (!strcmp(name, device_name)) {
			snprintf(path, size, "/dev/input/%s", e->d_name);
			fd = open(path, O_RDONLY | O_CLOEXEC);
		}
	}
	if (d)
		closedir(d);
	return fd;
}

/* a uinput copy of the touchscreen: its keys, axes (with their ranges) and properties */
static int copy_device(int src)
{
	unsigned long evbits[BITS(EV_MAX + 1)] = { 0 }, keybits[BITS(KEY_MAX + 1)] = { 0 },
		absbits[BITS(ABS_MAX + 1)] = { 0 }, props[BITS(INPUT_PROP_MAX + 1)] = { 0 };
	struct uinput_setup setup = { 0 };
	struct input_id id;
	char name[80];
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ioctl(src, EVIOCGBIT(0, sizeof(evbits)), evbits);
	ioctl(src, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
	ioctl(src, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
	ioctl(src, EVIOCGPROP(sizeof(props)), props);
	for (int t = 0; t <= EV_MAX; t++)
		if (TEST(t, evbits))
			ioctl(fd, UI_SET_EVBIT, t);
	for (int k = 0; k <= KEY_MAX; k++)
		if (TEST(k, keybits))
			ioctl(fd, UI_SET_KEYBIT, k);
	for (int p = 0; p <= INPUT_PROP_MAX; p++)
		if (TEST(p, props))
			ioctl(fd, UI_SET_PROPBIT, p);
	for (int a = 0; a <= ABS_MAX; a++) {
		if (!TEST(a, absbits))
			continue;
		struct uinput_abs_setup abs = { .code = a };
		ioctl(src, EVIOCGABS(a), &abs.absinfo);
		ioctl(fd, UI_SET_ABSBIT, a);
		ioctl(fd, UI_ABS_SETUP, &abs);
	}
	ioctl(src, EVIOCGID, &id);
	ioctl(src, EVIOCGNAME(sizeof(name)), name);
	setup.id = id;
	snprintf(setup.name, sizeof(setup.name), "%.60s (ztOS)", name);
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int key_device(void)
{
	struct uinput_setup setup = { .id = { BUS_VIRTUAL, 0x7a70, 0x0001, 1 } };
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (int i = 0; i < n_keys; i++)
		ioctl(fd, UI_SET_KEYBIT, keys[i].code);
	snprintf(setup.name, sizeof(setup.name), "ztOS virtual keys");
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void emit(int fd, int type, int code, int value)
{
	struct input_event ev = { .type = type, .code = code, .value = value };
	if (write(fd, &ev, sizeof(ev)) < 0 && verbose)
		perror("ztos-virtualkeyd: write");
}

static int key_at(int x, int y)
{
	for (int i = 0; i < n_keys; i++)
		if (x >= keys[i].x0 && x <= keys[i].x1 && y >= keys[i].y0 && y <= keys[i].y1)
			return keys[i].code;
	return 0;
}

int main(int argc, char **argv)
{
	const char *config = CONFIG;
	char path[300];
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			verbose = true;
		else
			config = argv[i];
	}
	if (!load(config)) {
		fprintf(stderr, "ztos-virtualkeyd: %s names no device or no key\n", config);
		return 1;
	}
	int src = open_device(path, sizeof(path));
	if (src < 0) {
		fprintf(stderr, "ztos-virtualkeyd: no touchscreen named \"%s\"\n", device_name);
		return 1;
	}
	int touch = copy_device(src), keyfd = key_device();
	if (touch < 0 || keyfd < 0) {
		fprintf(stderr, "ztos-virtualkeyd: uinput: %s\n", strerror(errno));
		return 1;
	}
	/* where every slot stands now, before the first event */
	for (int s = 0; s < MAX_SLOTS; s++)
		slot[s].id = -1;
	struct input_absinfo a;
	if (ioctl(src, EVIOCGABS(ABS_MT_SLOT), &a) == 0)
		cur_slot = a.value;
	if (cur_slot < 0 || cur_slot >= MAX_SLOTS)
		cur_slot = 0;
	/* the kernel sends a position only when it changes: start from its own */
	if (ioctl(src, EVIOCGABS(ABS_MT_POSITION_X), &a) == 0)
		slot[cur_slot].x = a.value;
	if (ioctl(src, EVIOCGABS(ABS_MT_POSITION_Y), &a) == 0)
		slot[cur_slot].y = a.value;
	if (ioctl(src, EVIOCGRAB, 1) < 0) {
		fprintf(stderr, "ztos-virtualkeyd: %s: grab: %s\n", path, strerror(errno));
		return 1;
	}
	fprintf(stderr, "ztos-virtualkeyd: %s (%s), %d keys\n", path, device_name, n_keys);

	struct input_event frame[MAX_FRAME];
	int n = 0, frame_slot[MAX_FRAME];
	for (;;) {
		struct input_event ev;
		if (read(src, &ev, sizeof(ev)) != sizeof(ev)) {
			if (errno == EINTR)
				continue;
			perror("ztos-virtualkeyd: read");
			return 1;
		}
		if (ev.type == EV_ABS) {
			int s = cur_slot >= 0 && cur_slot < MAX_SLOTS ? cur_slot : 0;
			if (ev.code == ABS_MT_SLOT)
				cur_slot = ev.value;
			else if (ev.code == ABS_MT_TRACKING_ID) {
				if (ev.value >= 0 && slot[s].id < 0)
					slot[s].fresh = true;
				slot[s].id = ev.value;
			} else if (ev.code == ABS_MT_POSITION_X)
				slot[s].x = ev.value;
			else if (ev.code == ABS_MT_POSITION_Y)
				slot[s].y = ev.value;
		}
		if (!(ev.type == EV_SYN && ev.code == SYN_REPORT)) {
			if (n < MAX_FRAME) {
				frame_slot[n] = ev.type == EV_ABS && ev.code >= ABS_MT_TOUCH_MAJOR && ev.code != ABS_MT_SLOT ?
					(cur_slot >= 0 && cur_slot < MAX_SLOTS ? cur_slot : 0) : -1;
				frame[n++] = ev;
			}
			continue;
		}

		/* the frame is whole: which contacts are keys */
		bool keys_changed = false, screen_touched = false, was_key[MAX_SLOTS];
		for (int s = 0; s < MAX_SLOTS; s++) {
			was_key[s] = slot[s].key != 0;
			if (slot[s].fresh) {
				slot[s].fresh = false;
				slot[s].key = key_at(slot[s].x, slot[s].y);
				if (slot[s].key) {
					emit(keyfd, EV_KEY, slot[s].key, 1);
					keys_changed = true;
					if (verbose)
						fprintf(stderr, "key %d down (%d,%d)\n", slot[s].key, slot[s].x, slot[s].y);
				}
			}
			if (slot[s].id < 0 && slot[s].key) {
				emit(keyfd, EV_KEY, slot[s].key, 0);
				keys_changed = true;
				if (verbose)
					fprintf(stderr, "key %d up\n", slot[s].key);
				slot[s].key = 0;
			}
			if (slot[s].id >= 0 && !slot[s].key)
				screen_touched = true;
		}
		if (keys_changed)
			emit(keyfd, EV_SYN, SYN_REPORT, 0);

		/* the rest goes on as it came: not a key's contact, and the
		 * single-touch copy (BTN_TOUCH, ABS_X/Y) only for the panel's */
		bool sent = false;
		for (int i = 0; i < n; i++) {
			struct input_event *e = &frame[i];
			int s = frame_slot[i];
			if (s >= 0 && (slot[s].key || was_key[s]))
				continue;	/* a key's contact, to its last event */
			bool single = (e->type == EV_KEY && (e->code == BTN_TOUCH || e->code == BTN_TOOL_FINGER)) ||
				(e->type == EV_ABS && (e->code == ABS_X || e->code == ABS_Y ||
						       e->code == ABS_PRESSURE));
			if (single && e->type == EV_KEY && e->value == 1 && !screen_touched)
				continue;	/* the press was a key's */
			if (single && e->type == EV_ABS && !screen_touched)
				continue;
			emit(touch, e->type, e->code, e->value);
			sent = true;
		}
		if (sent)
			emit(touch, EV_SYN, SYN_REPORT, 0);
		n = 0;
	}
}
