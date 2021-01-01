// gcc -o hid_elan1200 hid_elan1200.c -lrt

#define _GNU_SOURCE

#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef __STDC_NO_ATOMICS__
#include <stdatomic.h>
#endif
#include <linux/hidraw.h>
#include <linux/uinput.h>


#define VIRTUAL_DEV_NAME "VirtualELAN1200"
#define VIRT_VID 0x04F3
#define VIRT_PID 0x3022
#define ELAN_NAME "ELAN1200:00 04F3:3022"

// on my machine 14 ms is minimum, otherwise
// the delayed state is reported earlier than the next event arrives
#define DELAY 17
#define DELAY_NS DELAY * 1000000

#define AREA_TRESHOLD 16

// gcc -o hid_elan1200 hid_elan1200.c -lrt -DMEASURE_TIME
#ifdef MEASURE_TIME
static struct timespec start_ts, stop_ts;
#endif

#define MAX_X 3200
#define MAX_Y 2198
#define RESOLUTION 31
#define MAX_CONTACTS 5
#define MAX_SCANTIME ((255 << 8) | 255)
#define MAX_TIMESTAMP_INTERVAL 1000000

#define INPUT_MODE_REPORT_ID 0x3
#define INPUT_MODE_TOUCHPAD 0x03
#define LATENCY_MODE_REPORT_ID 0x7
#define LATENCY_MODE_NORMAL 0x00

#define MAX_EVENTS 64

#define MT_ID_NULL	(-1)
#define MT_ID_MIN	0
#define MT_ID_MAX	65535
#define MT_ID_SGN	((MT_ID_MAX + 1) >> 1)

#define INPUT_SYNC_NDELAY 4000000


#define ELAN_REPORT_ID 0x04
#define ELAN_REPORT_SIZE 14

// interrupts
static volatile sig_atomic_t stop = 0;

static void interrupt_handler(int sig)
{
	stop = 1;
}

// state
struct contact {
	int in_report;
	int x, y;
	int tool;
	int touch;
};

struct elan_usages {
	int x, y;
	int tool;
	int touch;
	int slot;
	int num_contacts;
	int scantime;
	int btn_left;
};

struct elan_application {
	int vfd;

	struct contact hw_state[MAX_CONTACTS];
	struct contact delayed_state[MAX_CONTACTS];

	int left_button_state;
	int num_expected;
	int num_received;
	atomic_bool delayed_flag_pending;
	atomic_bool delayed_flag_running;
	atomic_bool had_run;

	int last_tracking_id;
	int tracking_ids[MAX_CONTACTS];

	int area;

	struct timespec ts;
	int timestamp;
	int prev_scantime;
	int scantime_logical_max;
};

// timer
static timer_t timer_id;
static struct itimerspec ts;
static struct sigevent se;
static struct timespec input_sync_ts;
static struct timespec now_ts;

// report data
static struct input_event report[MAX_EVENTS];

// buttons
static int btn_tools[5] = { BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
			BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };


static inline unsigned long
ts_delta_usec(const struct timespec *a, const struct timespec *b)
{
	struct timespec r;
	r.tv_sec = a->tv_sec - b->tv_sec;
	r.tv_nsec = a->tv_nsec - b->tv_nsec;
	if (r.tv_nsec < 0) {
		r.tv_sec--;
		r.tv_nsec += 1000000000;
	}
	return (unsigned long)r.tv_sec * 1000000 + r.tv_nsec / 1000;
}

static inline unsigned long
ts_delta_msec(const struct timespec *a, const struct timespec *b)
{
	return ts_delta_usec(a, b) / 1000;
}


static int compute_timestamp(struct elan_application *app, int value)
{
	int delta = value - app->prev_scantime;

	clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);
	unsigned long tsdelta = ts_delta_usec(&now_ts, &app->ts);

	app->ts = now_ts;

	if (delta < 0)
		delta += app->scantime_logical_max;

	delta *= 100;

	app->prev_scantime = value;
	
	if (tsdelta > MAX_TIMESTAMP_INTERVAL)
		return 0;
	else
		return app->timestamp + delta;
}


static void send_report(struct elan_application *app, int delay) {
	int j = 0;
	struct contact *ct;
	int current_touches = 0;
	int tool = MT_TOOL_FINGER;

	struct contact *state = delay ? app->delayed_state : app->hw_state;

	for (int i = 0; i < MAX_CONTACTS; i++) {
		ct = &(state[i]);
		if (!ct->in_report) {
			// sometimes the touchpad forgets to report releases
			// every contact which touches the surface is always
			// reported otherwise mark it released
			if (ct->touch)
				ct->touch = 0;
			else
				continue;
		}

		report[j].type = EV_ABS;
		report[j].code = ABS_MT_SLOT;
		report[j++].value = i;

		if (ct->touch && app->tracking_ids[i] == MT_ID_NULL)
			app->tracking_ids[i] = app->last_tracking_id++ & MT_ID_MAX;
		if (!ct->touch)
			app->tracking_ids[i] = MT_ID_NULL;

		report[j].type = EV_ABS;
		report[j].code = ABS_MT_TRACKING_ID;
		report[j++].value = app->tracking_ids[i];

		if (app->tracking_ids[i] != MT_ID_NULL) {
			current_touches++;

			if (!ct->tool)
				tool = MT_TOOL_PALM;
			report[j].type = EV_ABS;
			report[j].code = ABS_MT_TOOL_TYPE;
			report[j++].value = tool;

			report[j].type = EV_ABS;
			report[j].code = ABS_MT_POSITION_X;
			report[j++].value = ct->x;

			report[j].type = EV_ABS;
			report[j].code = ABS_MT_POSITION_Y;
			report[j++].value = ct->y;
		}

		ct->in_report = 0;
	}

	if (delay)
		memcpy(app->hw_state, app->delayed_state, sizeof(app->hw_state));

	report[j].type = EV_KEY;
	report[j].code = BTN_LEFT;
	report[j++].value = app->left_button_state;

	report[j].type = EV_KEY;
	report[j].code = BTN_TOUCH;
	report[j++].value = current_touches > 0;

	for (int i = 0; i < 5; i++) {
		report[j].type = EV_KEY;
		report[j].code = btn_tools[i];
		report[j++].value = current_touches == i + 1;
	}

	if (current_touches > 0) {
		int current_id;
		int oldest_slot = -1;
		int old_id = app->last_tracking_id;
		for (int i = 0; i < MAX_CONTACTS; i++) {
			if (app->tracking_ids[i] == MT_ID_NULL)
				continue;
			current_id = app->tracking_ids[i];
			if ((current_id - old_id) & MT_ID_SGN) {
				oldest_slot = i;
				old_id = current_id;
			}
		}
		if (oldest_slot > -1) {
			report[j].type = EV_ABS;
			report[j].code = ABS_X;
			report[j++].value = (state[oldest_slot]).x;

			report[j].type = EV_ABS;
			report[j].code = ABS_Y;
			report[j++].value = (state[oldest_slot]).y;
		}
	}

	report[j].type = EV_MSC;
	report[j].code = MSC_TIMESTAMP;
	report[j++].value = app->timestamp;

	report[j].type = EV_SYN;
	report[j++].code = SYN_REPORT;

	write(app->vfd, &report, sizeof(report[0]) * j);
}


void timer_thread(union sigval sig)
{
	struct elan_application *app = (struct elan_application*)sig.sival_ptr;
	atomic_store(&app->delayed_flag_running, 1);
	if (atomic_exchange(&app->delayed_flag_pending, 0)) {
		send_report(app, 1);
	}
	atomic_store(&app->delayed_flag_running, 0);
#ifdef MEASURE_TIME
	clock_gettime(CLOCK_MONOTONIC_RAW, &stop_ts);
	printf("Timer triggered: %lu ms\n", ts_delta_msec(&stop_ts, &start_ts));
#endif
}


void init_globals(struct elan_application *app) {
	se.sigev_notify = SIGEV_THREAD;
	se.sigev_value.sival_ptr = app;
	se.sigev_notify_function = timer_thread;
	se.sigev_notify_attributes = NULL;

	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;

	input_sync_ts.tv_nsec = INPUT_SYNC_NDELAY;

	timer_create(CLOCK_REALTIME, &se, &timer_id);

	app->left_button_state = 0;
	app->last_tracking_id = MT_ID_MIN;
	atomic_init(&app->delayed_flag_pending, 0);
	atomic_init(&app->delayed_flag_running, 0);
	app->num_received = 0;

	app->area = 0;

	clock_gettime(CLOCK_MONOTONIC_RAW, &app->ts);
	app->timestamp = 0;
	app->prev_scantime = 0;
	app->scantime_logical_max = MAX_SCANTIME;

	for (int i = 0; i < MAX_CONTACTS; i++) {
		app->hw_state[i].in_report = 0;
		app->hw_state[i].x = 0;
		app->hw_state[i].y = 0;
		app->hw_state[i].tool = 1;
		app->hw_state[i].touch = 0;
		app->tracking_ids[i] = MT_ID_NULL;
	}
}


void buf_to_usages(unsigned char *buf, struct elan_usages *usages,
			struct elan_application *app) {
	int height, width;

	usages->slot = buf[1] >> 4;
	usages->touch = (buf[1] & 0x0f) == 3;
	usages->x = ((buf[3] & 0x0f) << 8) | buf[2];
	usages->y = ((buf[5] & 0x0f) << 8) | buf[4];
	usages->scantime = (buf[7] << 8) | buf[6];
	usages->num_contacts = buf[8];
	usages->tool = (buf[9] >> 1) < 38;
	usages->btn_left = buf[9] & 0x01;

	// ? = buf[9]
	// ? = buf[10]
	// some combined data of the duration of contact
	// which resets after inactivity,
	// contact area and clickpad button

	width = buf[11] & 0x0f;
	height = buf[11] >> 4;
	app->area = width * height;
}


static void do_capture(int fd, int vfd) {

	struct elan_usages usages;
	struct elan_application app;
	app.vfd = vfd;

	init_globals(&app);

	unsigned char buf[ELAN_REPORT_SIZE];
	int is_touch;
	int is_release;
	int rc;

	while(!stop) {
		if ((rc = read(fd, buf, sizeof(buf))) < 0 && !stop) {
			fprintf(stderr, "Error reading the hidraw device file.\n");
			break;
		}
		// ignore 0x40 event
		if (buf[0] != ELAN_REPORT_ID || buf[1] == 0x40)
			continue;

		// ignore irrelevant states if any
		is_touch = (buf[1] & 0x0f) == 3;
		is_release = (buf[1] & 0x0f) == 1;
		if (!is_touch && !is_release)
			continue;
		buf_to_usages(buf, &usages, &app);

		if (atomic_exchange(&app.delayed_flag_pending, 0)) {
			if (usages.num_contacts == 1) {
				send_report(&app, 1);
				nanosleep(&input_sync_ts, &input_sync_ts);
			}
#ifdef MEASURE_TIME
			clock_gettime(CLOCK_MONOTONIC_RAW, &stop_ts);
			printf("Next event arrived: %lu ms\n",
					ts_delta_msec(&stop_ts, &start_ts));
#endif
		} else if (atomic_load(&app.delayed_flag_running)) {
			nanosleep(&input_sync_ts, &input_sync_ts);
		}

		if (usages.num_contacts) {
			app.num_expected = usages.num_contacts;
			app.num_received = 0;
		}

		app.num_received++;

		struct contact *ct = &app.hw_state[usages.slot];
		ct->in_report = 1;
		ct->tool = usages.tool;
		ct->x = usages.x;
		ct->y = usages.y;
		ct->touch = usages.touch;

		if (app.num_received < app.num_expected)
			continue;

		app.left_button_state = usages.btn_left;

		app.timestamp = compute_timestamp(&app, usages.scantime);

		if (usages.num_contacts == 1 && !usages.touch && app.area > AREA_TRESHOLD) {
			memcpy(app.delayed_state, app.hw_state, sizeof(app.hw_state));
			ts.it_value.tv_nsec = DELAY_NS;
			timer_settime(timer_id, 0, &ts, 0);
			atomic_store(&app.delayed_flag_pending, 1);
#ifdef MEASURE_TIME
			printf("Timer started\n");
			clock_gettime(CLOCK_MONOTONIC_RAW, &start_ts);
#endif
		} else {
			send_report(&app, 0);
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
	devsetup.id.vendor = VIRT_VID;
	devsetup.id.product = VIRT_PID;
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
	ioctl(vfd, UI_SET_EVBIT, EV_MSC);
	ioctl(vfd, UI_SET_MSCBIT, MSC_TIMESTAMP);

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
