/* SPDX-License-Identifier: MIT */
/* The vibration motor where the kernel offers it as force feedback: an input
 * node with FF_RUMBLE (the H700's "ANBERNIC-keys", the Rotate's
 * "sc27xx:vibrator"; TortOS plorpos-5tm.4). One effect, uploaded again when
 * the strength changes, played until stopped (a length of 0 is no end for a
 * memless driver). Included by the ports that have such a motor; a device
 * without the node answers "no motor", as the weak default does. */
#ifndef DIATOM_PORT_FF_H
#define DIATOM_PORT_FF_H

#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int      g_ff_fd = -2;      /* -2 not looked for yet, -1 none */
static int      g_ff_id = -1;      /* the uploaded effect */
static uint16_t g_ff_at;           /* the strength playing, 0 stopped */

static void ff_find(void)
{
	int i;

	g_ff_fd = -1;
	for (i = 0; i < 32; i++) {
		unsigned long bits[FF_MAX / (8 * sizeof(unsigned long)) + 1];
		char path[32];
		int fd;

		snprintf(path, sizeof path, "/dev/input/event%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		memset(bits, 0, sizeof bits);
		if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof bits), bits) >= 0 &&
		    (bits[FF_RUMBLE / (8 * sizeof(unsigned long))] >>
		     (FF_RUMBLE % (8 * sizeof(unsigned long))) & 1)) {
			g_ff_fd = fd;
			return;
		}
		close(fd);
	}
}

bool diatom_port_has_rumble(void)
{
	if (g_ff_fd == -2) ff_find();
	return g_ff_fd >= 0;
}

void diatom_port_rumble(uint16_t strength)
{
	struct input_event ev;

	if (!diatom_port_has_rumble() || strength == g_ff_at) return;
	memset(&ev, 0, sizeof ev);
	ev.type = EV_FF;
	if (strength) {
		struct ff_effect e;

		memset(&e, 0, sizeof e);
		e.type = FF_RUMBLE;
		e.id = (int16_t)g_ff_id;
		e.u.rumble.strong_magnitude = strength;
		e.u.rumble.weak_magnitude = strength;
		if (ioctl(g_ff_fd, EVIOCSFF, &e) < 0) return;
		g_ff_id = e.id;
		ev.value = 1;
	} else if (g_ff_id < 0) {
		return;
	}
	ev.code = (uint16_t)g_ff_id;
	if (write(g_ff_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) g_ff_at = strength;
}

#endif
