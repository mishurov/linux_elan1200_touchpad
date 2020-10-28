// gcc -o hid_elan1200 hid_elan1200.c -lrt -lpthread

#define _GNU_SOURCE

#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <linux/uinput.h>


#define VIRTUAL_DEV_NAME "VirtualELAN1200"
#define ELAN_NAME "ELAN1200:00 04F3:3022"

// on my machine 14 ms is minimum, otherwise
// the timer fires earlier than the next event arrives
#define DELAY 25
#define DELAY_NS DELAY * 1000000

#define MAX_X 3200
#define MAX_Y 2198
#define RESOLUTION 31
#define MAX_CONTACTS 5

#define INPUT_MODE_REPORT_ID 0x3
#define INPUT_MODE_TOUCHPAD 0x03
#define LATENCY_MODE_REPORT_ID 0x7
#define LATENCY_MODE_NORMAL 0x00

#define MAX_EVENTS 64

#define MT_ID_NULL	(-1)
#define MT_ID_MIN	0
#define MT_ID_MAX	65535

// interrupts
static volatile sig_atomic_t stop = 0;

static void interrupt_handler(int sig)
{
	stop = 1;
}

// state
struct contact {
	int in_report;
	int x;
	int y;
	int tool;
	int touch;
};

static int btn_left = 0;
static struct contact hw_state[MAX_CONTACTS];
static struct contact prev_state[MAX_CONTACTS];
static struct contact delayed_state[MAX_CONTACTS];

// tracking ids
static unsigned int last_tracking_id = MT_ID_MIN;
static unsigned int tracking_ids[MAX_CONTACTS];

// report data
struct input_event report[MAX_EVENTS];

// buttons
int btn_tools[5] = { BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
			BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };

// timer
static timer_t timer_id;
static struct itimerspec ts;
static struct sigevent se;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


static int delayed_slot = -1;
static int abs_slot = 0;

void send_report(int vfd, int from_timer) {
	int current_touches = 0;
	int prev_touches = 0;
	struct contact *state = from_timer ? delayed_state : hw_state;

	for (int i = 0; i < MAX_CONTACTS; i++) {
		if (from_timer)
			break;
		if (hw_state[i].touch)
			current_touches++;
		if (prev_state[i].touch)
			prev_touches++;
	}

	int delay = !from_timer && prev_touches == 1 && current_touches == 0;
	if (delay)
		memcpy(delayed_state, hw_state, sizeof(hw_state));

	int j = 0;
	unsigned int id;
	struct contact *ct;
	for (int i = 0; i < MAX_CONTACTS; i++) {
		ct = &(state[i]);
		if (!ct->in_report)
			continue;
		if (delay && !ct->touch)
			delayed_slot = i;

		report[j].type = EV_ABS;
		report[j].code = ABS_MT_SLOT;
		report[j++].value = i;

		if (delay && !ct->touch) {
			id = tracking_ids[i];
			current_touches = 1;
		} else {
			if (ct->touch && tracking_ids[i] == MT_ID_NULL)
				tracking_ids[i] = last_tracking_id++ & MT_ID_MAX;
			if (!ct->touch)
				tracking_ids[i] = MT_ID_NULL;
			id = tracking_ids[i];
		}

		report[j].type = EV_ABS;
		report[j].code = ABS_MT_TRACKING_ID;
		report[j++].value = id;

		if (id != MT_ID_NULL) {
			report[j].type = EV_ABS;
			report[j].code = ABS_MT_TOOL_TYPE;
			report[j++].value = ct->tool;

			report[j].type = EV_ABS;
			report[j].code = ABS_MT_POSITION_X;
			report[j++].value = ct->x;

			report[j].type = EV_ABS;
			report[j].code = ABS_MT_POSITION_Y;
			report[j++].value = ct->y;
		}

		ct->in_report = 0;
	}

	if (!from_timer) {
		if (tracking_ids[abs_slot] != MT_ID_NULL) {
			report[j].type = EV_ABS;
			report[j].code = ABS_X;
			report[j++].value = hw_state[abs_slot].x;

			report[j].type = EV_ABS;
			report[j].code = ABS_Y;
			report[j++].value = hw_state[abs_slot].y;
		} else {
			for (int i = 0; i < MAX_CONTACTS; i++) {
				if (tracking_ids[i] != MT_ID_NULL) {
					abs_slot = i;
					break;
				}
			}
		}
	} else {
		abs_slot = 0;
	}

	report[j].type = EV_KEY;
	report[j].code = BTN_TOUCH;
	report[j++].value = current_touches > 0;

	for (int i = 0; i < 5; i++) {
		report[j].type = EV_KEY;
		report[j].code = btn_tools[i];
		report[j++].value = current_touches == i + 1;
	}

	report[j].type = EV_KEY;
	report[j].code = BTN_LEFT;
	report[j++].value = !from_timer && btn_left;

	report[j].type = EV_SYN;
	report[j++].code = SYN_REPORT;

	if (delay) {
		ts.it_value.tv_nsec = DELAY_NS;
		timer_settime(timer_id, 0, &ts, 0);
	}

	write(vfd, &report, sizeof(report[0]) * j);
}

void timer_thread (union sigval sig)
{
	pthread_mutex_lock(&mutex);
	if (delayed_slot > -1) {
		int vfd = *(int*)sig.sival_ptr;
		send_report(vfd, 1);
		delayed_slot = -1;
	}
	pthread_mutex_unlock(&mutex);
}

void init_globals(int *vfd) {
	se.sigev_notify = SIGEV_THREAD;
	se.sigev_value.sival_ptr = vfd;
	se.sigev_notify_function = timer_thread;
	se.sigev_notify_attributes = NULL;

	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;

	timer_create(CLOCK_REALTIME, &se, &timer_id);

	for (int i = 0; i < MAX_CONTACTS; i++) {
		hw_state[i].in_report = 0;
		hw_state[i].x = 0;
		hw_state[i].y = 0;
		hw_state[i].tool = MT_TOOL_FINGER;
		hw_state[i].touch = 0;

		tracking_ids[i] = MT_ID_NULL;
	}

	memcpy(prev_state, hw_state, sizeof(hw_state));
}

void do_capture(int fd, int vfd) {

	init_globals(&vfd);

	unsigned char buf[12];
	int slot;
	int x;
	int y;
	int num_expected;
	int num_received;
	int tool;

	int rc;

	while(!stop) {
		if ((rc = read(fd, buf, sizeof(buf))) < 0 && !stop) {
			fprintf(stderr, "Error reading the hidraw device file.\n");
			break;
		}
		// ignore 0x40 event
		if (buf[0] != 0x04 || buf[1] == 0x40)
			continue;
		
		int is_touch = (buf[1] & 0x0f) == 3;
		int is_release = (buf[1] & 0x0f) == 1;

		if (!is_touch && !is_release)
			continue;

		slot = buf[1] >> 4;
		hw_state[slot].in_report = 1;

		if (buf[8] > 0) {
			num_received = 0;
			num_expected = buf[8];
		}
		num_received++;

		tool = buf[9] > 64 ? MT_TOOL_PALM : MT_TOOL_FINGER;
		x = ((buf[3] & 0x0f) << 8) | buf[2];
		y = ((buf[5] & 0x0f) << 8) | buf[4];
		btn_left = buf[9] & 0x01;
		// width = (buf[11] & 0x0f)
		// height = (buf[11] >> 4)
		// timestamp = (buf[7] << 8) + buf[6]
		// ? = buf[10]
		hw_state[slot].x = x;
		hw_state[slot].y = y;
		hw_state[slot].tool = tool;
		hw_state[slot].touch = is_touch;

		pthread_mutex_lock(&mutex);
		if (delayed_slot > -1 &&
		    (slot == delayed_slot || num_expected > 1)) {
			delayed_slot = -1;
			ts.it_value.tv_nsec = 0;
			timer_settime(timer_id, 0, &ts, 0);
		}
		pthread_mutex_unlock(&mutex);

		if (num_received == num_expected) {
			pthread_mutex_lock(&mutex);
			send_report(vfd, 0);
			memcpy(prev_state, hw_state, sizeof(hw_state));
			pthread_mutex_unlock(&mutex);
		}
	}
}


static int create_virtual_device() {
	int vfd;
	if ((vfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK)) < 0) {
		fprintf(stderr, "Error upening uinput device.\n");
		return -1;
	}

	struct uinput_setup devsetup;
	memset(&devsetup, 0, sizeof(devsetup));
	devsetup.id.bustype = BUS_I2C;
	devsetup.id.vendor = 0x04F3;
	devsetup.id.product = 0x3022;
	strcpy(devsetup.name, VIRTUAL_DEV_NAME);

	ioctl(vfd, UI_SET_EVBIT, EV_SYN);

	ioctl(vfd, UI_SET_EVBIT, EV_KEY);
	int key_bits[7] = { BTN_LEFT, BTN_TOUCH,
			BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
			BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
	for (int i = 0; i < 7; i++)
		ioctl(vfd, UI_SET_KEYBIT, key_bits[i]);

	ioctl(vfd, UI_SET_EVBIT, EV_ABS);
	int abs_bits[7] = { ABS_X, ABS_Y, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
			ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_TOOL_TYPE };
	struct uinput_abs_setup abssetup;
	abssetup.absinfo.minimum = 0;
	abssetup.absinfo.value = 0;
	abssetup.absinfo.flat = 0;
	abssetup.absinfo.fuzz = 0;
	for (int i = 0; i < 7; i++) {
		abssetup.code = abs_bits[i];
		abssetup.absinfo.resolution = 0;
		switch (abs_bits[i]) {
		case ABS_MT_POSITION_X:
		case ABS_X:
			abssetup.absinfo.maximum = MAX_X;
			abssetup.absinfo.resolution = RESOLUTION;
			break;
		case ABS_MT_POSITION_Y:
		case ABS_Y:
			abssetup.absinfo.maximum = MAX_Y;
			abssetup.absinfo.resolution = RESOLUTION;
			break;
		case ABS_MT_SLOT:
			abssetup.absinfo.maximum = MAX_CONTACTS - 1;
			break;
		case ABS_MT_TOOL_TYPE:
			abssetup.absinfo.maximum = 2;
			break;
		case ABS_MT_TRACKING_ID:
			abssetup.absinfo.maximum = MT_ID_MAX;
			break;
		}
		ioctl(vfd, UI_ABS_SETUP, &abssetup);
	}

	unsigned int prop_bits[2] = { INPUT_PROP_POINTER, INPUT_PROP_BUTTONPAD };
	for (int i = 0; i < 2; i++)
		ioctl(vfd, UI_SET_PROPBIT, prop_bits[i]);

	ioctl(vfd, UI_DEV_SETUP, &devsetup);
	ioctl(vfd, UI_DEV_CREATE);

	sleep(1);

	return vfd;
}


static const char *direntcmp;
static int startswith(const struct dirent *dir) {
	return strncmp(direntcmp, dir->d_name, strlen(direntcmp)) == 0;
}

static int is_elan(const char *name) {
	return strncmp(ELAN_NAME, name, strlen(ELAN_NAME)) == 0;
}

static int get_src_device(const char *dir, const char *prefix) {
	char *filename;
	struct dirent **namelist;
	int i, ndev, fd;
	int devnum = -1;

	direntcmp = prefix;
	ndev = scandir(dir, &namelist, startswith, versionsort);
	if (ndev <= 0)
		return -1;

	for (i = 0; i < ndev; i++)
	{
		char fname[PATH_MAX];
		int fd = -1;
		char hid_name[256] = "???";

		snprintf(fname, sizeof(fname), "%s/%s", dir, namelist[i]->d_name);

		fd = open(fname, O_RDONLY);
		if (fd < 0)
			continue;
		ioctl(fd, HIDIOCGRAWNAME(sizeof(hid_name)), hid_name);
		close(fd);

		if (!is_elan(hid_name))
			continue;

		char scanstr[64] = "";
		strcat(strcat(scanstr, prefix), "%d");
		sscanf(namelist[i]->d_name, scanstr, &devnum);
		break;
	}

	if (devnum < 0)
		return -1;

	for (i = 0; i < ndev; i++)
		free(namelist[i]);

	if (asprintf(&filename, "%s/%s%d", dir, prefix, devnum) < 0)
		return -1;
	
	if ((fd = open(filename, O_RDONLY)) < 0) {
		return -1;
	}
	return fd;
}

static int set_features(int fd) {
	int res;
	unsigned char buf[2];
	buf[0] = INPUT_MODE_REPORT_ID;
	buf[1] = INPUT_MODE_TOUCHPAD;
	res = ioctl(fd, HIDIOCSFEATURE(3), buf);
	if (res < 0)
		return res;
	buf[0] = LATENCY_MODE_REPORT_ID;
	buf[1] = LATENCY_MODE_NORMAL;
	res = ioctl(fd, HIDIOCSFEATURE(3), buf);
	if (res < 0)
		return res;
	return 0;
}

static int start_capture() {
	int fd;
	if ((fd = get_src_device("/dev", "hidraw")) < 0) {
		perror("Unable to open hid device");
		goto error;
	}

	int ret;
	if ((ret = set_features(fd)) < 0) {
		perror("HIDIOCSFEATURE");
		goto error;
	}

	int vfd;
	if ((vfd = create_virtual_device()) < 0) {
		perror("Unable to create virtual device");
		goto error;
	}

	struct sigaction int_action = { .sa_handler = interrupt_handler };
	sigaction(SIGINT, &int_action, 0);
	sigaction(SIGTERM, &int_action, 0);

	do_capture(fd, vfd);

	ioctl(vfd, UI_DEV_DESTROY);
	close(vfd);
	close(fd);
	return 0;
error:
	return 1;
}


int main(int argc, char **argv)
{
	return start_capture();
}
