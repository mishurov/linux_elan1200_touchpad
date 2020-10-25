// gcc mirror_elan1200.c

#define _GNU_SOURCE

#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/input.h>

#include <linux/uinput.h>

#include <stdio.h>

#define DEV_INPUT_EVENT "/dev/input"
#define EVENT_DEV_NAME "event"
#define DEV_NAME "ELAN1200:00 04F3:3022 Touchpad"
#define VIRTUAL_DEV_NAME "VirtualELAN1200"

#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array)	((array[LONG(bit)] >> OFF(bit)) & 1)


static int is_event_device(const struct dirent *dir) {
	return strncmp(EVENT_DEV_NAME, dir->d_name, 5) == 0;
}

static int is_elan(const char *name) {
	return strncmp(DEV_NAME, name, 30) == 0;
}

static char* get_src_device() {
	char *filename;
	struct dirent **namelist;
	int i, ndev, devnum;

	ndev = scandir(DEV_INPUT_EVENT, &namelist, is_event_device, versionsort);
	if (ndev <= 0)
		return NULL;

	for (i = 0; i < ndev; i++)
	{
		char fname[PATH_MAX];
		int fd = -1;
		char name[256] = "???";

		snprintf(fname, sizeof(fname), "%s/%s",
			DEV_INPUT_EVENT, namelist[i]->d_name);

		fd = open(fname, O_RDONLY);
		if (fd < 0)
			continue;
		ioctl(fd, EVIOCGNAME(sizeof(name)), name);
		close(fd);

		if (!is_elan(name))
			continue;

		sscanf(namelist[i]->d_name, "event%d", &devnum);
		break;
	}

	for (i = 0; i < ndev; i++)
		free(namelist[i]);

	if (asprintf(&filename, "%s/%s%d",
		     DEV_INPUT_EVENT, EVENT_DEV_NAME,
		     devnum) < 0)
		return NULL;
	return filename;
}

static volatile sig_atomic_t stop = 0;

static void interrupt_handler(int sig)
{
	stop = 1;
}

static int capture_events(int fd, int vfd)
{
	struct input_event ev;
	struct input_event events[64];
	int report_len = 0;
	int rd;
	fd_set rdfs;

	FD_ZERO(&rdfs);
	FD_SET(fd, &rdfs);

	while (!stop) {
		select(fd + 1, &rdfs, NULL, NULL, NULL);
		if (stop)
			break;
		rd = read(fd, &ev, sizeof(struct input_event));

		if (rd < (int) sizeof(struct input_event)) {
			printf("expected %d bytes, got %d\n",
				(int) sizeof(struct input_event), rd);
			perror("\nevtest: error reading");
			return 1;
		}

		events[report_len++] = ev;

		// inputs events should be analysed here and
		// conditionally delayed but is less reliable
		// than using raw hid data from the device

		if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			write(vfd, &events, sizeof(events[0]) * report_len);
			report_len = 0;
		}
	}
	
	ioctl(vfd, UI_DEV_DESTROY);
   	close(vfd);
	ioctl(fd, EVIOCGRAB, (void*)0);
	return EXIT_SUCCESS;
}


static void set_absdata(int fd, int vfd, int code)
{
	struct uinput_abs_setup abssetup;
	abssetup.code = code;
	ioctl(fd, EVIOCGABS(code), &abssetup.absinfo);
	ioctl(vfd, UI_ABS_SETUP, &abssetup);
}


static int create_virtual_device(int fd)
{
	int vfd;
	if ((vfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK)) < 0) {
		fprintf(stderr, "Error upening uinput device.\n");
		return -1;
	}

	struct uinput_setup devsetup;

	unsigned int type, code;
	unsigned short id[4];
	unsigned long bit[EV_MAX][NBITS(KEY_MAX)];
	unsigned int prop;
	unsigned long propbits[INPUT_PROP_MAX];

	memset(&devsetup, 0, sizeof(devsetup));

	ioctl(fd, EVIOCGID, id);

	devsetup.id.bustype = id[ID_BUS];
	devsetup.id.vendor = id[ID_VENDOR];
	devsetup.id.product = id[ID_PRODUCT];
	strcpy(devsetup.name, VIRTUAL_DEV_NAME);

	memset(bit, 0, sizeof(bit));
	ioctl(fd, EVIOCGBIT(0, EV_MAX), bit[0]);

	ioctl(vfd, UI_SET_EVBIT, EV_KEY);
	ioctl(vfd, UI_SET_EVBIT, EV_SYN);
	ioctl(vfd, UI_SET_EVBIT, EV_ABS);
	ioctl(vfd, UI_SET_EVBIT, EV_MSC);

	for (type = 0; type < EV_MAX; type++) {
		if (test_bit(type, bit[0]) && type != EV_REP) {
			if (type == EV_SYN)
				continue;
			ioctl(fd, EVIOCGBIT(type, KEY_MAX), bit[type]);
			for (code = 0; code < KEY_MAX; code++) {
				if (test_bit(code, bit[type])) {
					switch (type) {
					case EV_MSC:
						ioctl(vfd, UI_SET_MSCBIT, code);
						break;
					case EV_KEY:
						ioctl(vfd, UI_SET_KEYBIT, code);
						break;
					case EV_ABS:
						set_absdata(fd, vfd, code);
						break;
					}
				}
			}
		}
	}

	memset(propbits, 0, sizeof(propbits));
	ioctl(fd, EVIOCGPROP(sizeof(propbits)), propbits);
	for (prop = 0; prop < INPUT_PROP_MAX; prop++) {
		if (test_bit(prop, propbits))
			ioctl(vfd, UI_SET_PROPBIT, prop);
	}

	ioctl(vfd, UI_DEV_SETUP, &devsetup);
	ioctl(vfd, UI_DEV_CREATE);

	sleep(1);
	
	return vfd;
}


static int do_mirror()
{
	int fd;
	char *filename = get_src_device();
	
	if ((fd = open(filename, O_RDONLY)) < 0) {
		if (errno == EACCES && getuid() != 0)
			fprintf(stderr, "You do not have access to %s. Try "
					"running as root instead.\n",
					filename);
		goto error;
	}

	ioctl(fd, EVIOCGRAB, (void*)1);

	int vfd;
	if ((vfd = create_virtual_device(fd)) < 0)
		goto error;

	signal(SIGINT, interrupt_handler);
	signal(SIGTERM, interrupt_handler);

	free(filename);
	return capture_events(fd, vfd);
error:
	free(filename);
	return EXIT_FAILURE;
}



int main (int argc, char **argv)
{
	return do_mirror();
};
