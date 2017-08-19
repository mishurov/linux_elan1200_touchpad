/*
 * Proof of concept for FTE1001 driver - built using the Hidraw Userspace Example
 *
 * Copyright (c) 2010 Alan Ott <alan@signal11.us>
 * Copyright (c) 2010 Signal 11 Software
 *
 * The code may be used by anyone for any purpose,
 * and can serve as a starting point for developing
 * applications using hidraw.
 */

/* Linux */
#include <linux/types.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <linux/hidraw.h>

/* Unix */
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* C */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#define MAX_CONTACTS 5
#define MAX_X 0x0c80
#define MAX_Y 0x0896
#define MAX_TOUCH_WIDTH 14

int createUIDev(int ifd) {
	int res = ioctl(ifd, UI_SET_EVBIT, EV_SYN);
	res = ioctl(ifd, UI_SET_EVBIT, EV_KEY);
	res = ioctl(ifd, UI_SET_KEYBIT, BTN_LEFT);
	res = ioctl(ifd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
	res = ioctl(ifd, UI_SET_KEYBIT, BTN_TOUCH);
	res = ioctl(ifd, UI_SET_KEYBIT, BTN_TOOL_DOUBLETAP);
	res = ioctl(ifd, UI_SET_KEYBIT, BTN_TOOL_TRIPLETAP);
	res = ioctl(ifd, UI_SET_KEYBIT, BTN_TOOL_QUADTAP);
	res = ioctl(ifd, UI_SET_EVBIT, EV_ABS);
	res = ioctl(ifd, UI_SET_ABSBIT, ABS_X);
	res = ioctl(ifd, UI_SET_ABSBIT, ABS_Y);
	res = ioctl(ifd, UI_SET_ABSBIT, ABS_TOOL_WIDTH);
	res = ioctl(ifd, UI_SET_ABSBIT, ABS_DISTANCE);
	res = ioctl(ifd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
	res = ioctl(ifd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
	res = ioctl(ifd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
	res = ioctl(ifd, UI_SET_ABSBIT, ABS_MT_SLOT);
	res = ioctl(ifd, UI_SET_ABSBIT, ABS_MT_TOOL_TYPE);

	res = ioctl(ifd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
	res = ioctl(ifd, UI_SET_ABSBIT, ABS_MT_TOUCH_MINOR);
	res = ioctl(ifd, UI_SET_ABSBIT, ABS_MT_ORIENTATION);

	res = ioctl(ifd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
	res = ioctl(ifd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);

	struct uinput_user_dev uidev;
	memset(&uidev, 0, sizeof(uidev));
	snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "ELAN1200:00 04F3:3022 Userspace Driver");
	uidev.id.bustype = BUS_I2C;
	uidev.id.vendor  = 0x04f3;
	uidev.id.product = 0x3022;
	uidev.id.version = 1;
	uidev.absmax[ABS_X] = uidev.absmax[ABS_MT_POSITION_X] = MAX_X;
	uidev.absmax[ABS_Y] = uidev.absmax[ABS_MT_POSITION_Y] = MAX_Y;
	uidev.absmax[ABS_MT_POSITION_X] = MAX_X;
	uidev.absmax[ABS_MT_POSITION_Y] = MAX_Y;
	uidev.absmax[ABS_DISTANCE] = 1;
	uidev.absmax[ABS_MT_ORIENTATION] = 1;
	uidev.absmax[ABS_MT_SLOT] = MAX_CONTACTS - 1;
	uidev.absmax[ABS_MT_TOUCH_MAJOR] = uidev.absmax[ABS_TOOL_WIDTH] = MAX_TOUCH_WIDTH;
	uidev.absmax[ABS_MT_TOUCH_MINOR] = uidev.absmax[ABS_TOOL_WIDTH];

	res = write(ifd, &uidev, sizeof(uidev));
	res = ioctl(ifd, UI_DEV_CREATE);

	return res;
}

int startMultiTouch(int fd) {
	unsigned char buf[4];
	/* Set Feature */
	buf[0] = 0x01; /* Report Number */
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = 0x00;
	int res = ioctl(fd, HIDIOCSFEATURE(5), buf);
	if (res < 0)
		perror("HIDIOCSFEATURE");

	return res;
}


int mainLoop(int fd, int ifd) {
	unsigned char buf[12];
	int res = 0;

	while(1) {
		res = read(fd, buf, sizeof(buf));
		// ignore 0x40 event
		// event 0x41 can be also fake
		if (buf[0] == 0x04 && buf[1] != 0x40) {
			struct input_event ev[34];
			int evt_idx = 0;
			int is_touch = (buf[1] & 0x0f) == 3;
			int is_release = (buf[1] & 0x0f) == 1;

			if (is_touch || is_release) {
				int slot = buf[1] >> 4;
				int tracking_id = is_touch ? slot : -1;

				int pos_x = ((buf[3] & 0x0f) << 8) | buf[2];
				int pos_y = ((buf[5] & 0x0f) << 8) | buf[4];

				int mk_x = (buf[11] & 0x0f);
				int mk_y = (buf[11] >> 4);

				int num_contacts = buf[8];

				int major = MAX(mk_x, mk_y);
				int minor = MIN(mk_x, mk_y);
				int orientation = mk_x > mk_y;

				int con = buf[9];
				int con2 = buf[10];
				
				// buf[6] least significant bits of a counter
				// buf[7] most significant bits of a counter
				// buf[8] num contacts
				// buf[9] hover data
				// buf[10] ?
				// buf[11] width/height

				if (slot == 0)
					fprintf(stderr, "r %i id %i x %4d        y %4d        c %2x c2 %2x w %2d h %2d t %i \n", num_contacts, slot, pos_x, pos_y, con,  con2, mk_x, mk_y, is_touch);
				if (slot == 1)
					fprintf(stderr, "r %i id %i -       %4d  -       %4d  - %2x -  %2x - %2d - %2d - %i \n", num_contacts, slot, pos_x, pos_y, con,  con2, mk_x, mk_y, is_touch);



				/*
				fprintf(stderr, "width %i\n", mk_x);
				fprintf(stderr, "height %i\n", mk_y);
				fprintf(stderr, "num_contacts %i\n", num_contacts);
				fprintf(stderr, "trackingId %i\n", contacts[i]);
				*/

				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_MT_SLOT;
				ev[evt_idx++].value = slot;
				
				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_MT_TOOL_TYPE;
				ev[evt_idx++].value = MT_TOOL_FINGER;

				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_MT_TRACKING_ID;
				ev[evt_idx++].value = tracking_id;

				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_MT_POSITION_X;
				ev[evt_idx++].value = pos_x;
				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_MT_POSITION_Y;
				ev[evt_idx++].value = pos_y;

				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_X;
				ev[evt_idx++].value = pos_x;
				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_Y;
				ev[evt_idx++].value = pos_y;
				
				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_MT_TOUCH_MAJOR;
				ev[evt_idx++].value = major;
				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_MT_TOUCH_MINOR;
				ev[evt_idx++].value = minor;
				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_MT_ORIENTATION;
				ev[evt_idx++].value = orientation;

				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_TOOL_WIDTH;
				ev[evt_idx++].value = mk_x;

				ev[evt_idx].type = EV_ABS;
				ev[evt_idx].code = ABS_DISTANCE;
				ev[evt_idx++].value = con != 0;

				ev[evt_idx].type = EV_KEY;
				ev[evt_idx].code = BTN_TOUCH;
				ev[evt_idx++].value = is_touch;

				ev[evt_idx].type = EV_KEY;
				ev[evt_idx].code = BTN_TOOL_FINGER;
				ev[evt_idx++].value = num_contacts == 1;

				ev[evt_idx].type = EV_KEY;
				ev[evt_idx].code = BTN_TOOL_DOUBLETAP;
				ev[evt_idx++].value = num_contacts == 2;

				ev[evt_idx].type = EV_KEY;
				ev[evt_idx].code = BTN_TOOL_TRIPLETAP;
				ev[evt_idx++].value = num_contacts == 3;

				ev[evt_idx].type = EV_KEY;
				ev[evt_idx].code = BTN_TOOL_QUADTAP;
				ev[evt_idx++].value = num_contacts == 4;

				ev[evt_idx].type = EV_SYN;
				ev[evt_idx].code = SYN_MT_REPORT;
				ev[evt_idx++].value = 0;
				ev[evt_idx].type = EV_SYN;
				ev[evt_idx].code = SYN_REPORT;
				ev[evt_idx++].value = 0;

				//res = write(ifd, &ev, sizeof(ev[0])*evt_idx);
			}
		}
	}
	return res;
}


int main(int argc, char **argv)
{
	int fd, ifd;
	const char *device = "/dev/hidraw0";
	const char *uinput = "/dev/uinput";

	if (argc > 1)
		device = argv[1];

	if (argc > 2)
		uinput = argv[2];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("Unable to open device");
		return 1;
	}

	ifd = open(uinput, O_WRONLY | O_NONBLOCK);
	if(ifd < 0) {
                perror("Unable to open uinput");
                return 1;
	}

	//createUIDev(ifd);
	//startMultiTouch(fd);
	mainLoop(fd, ifd);

	close(ifd);
	close(fd);

	return 0;
}
