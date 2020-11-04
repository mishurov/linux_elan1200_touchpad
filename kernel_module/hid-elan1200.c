// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Elan1200 Touchpad
 *
 *  Copyright (c) 2017-2020 Alexander Mishurov <ammishurov@gmail.com>
 *
 *  This code is partly based on hid-multitouch.c:
 *
 *  Copyright (c) 2010-2012 Stephane Chatty <chatty@enac.fr>
 *  Copyright (c) 2010-2013 Benjamin Tissoires <benjamin.tissoires@gmail.com>
 *  Copyright (c) 2010-2012 Ecole Nationale de l'Aviation Civile, France
 *  Copyright (c) 2012-2013 Red Hat, Inc
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/input/mt.h>


MODULE_AUTHOR("Alexander Mishurov <ammishurov@gmail.com>");
MODULE_DESCRIPTION("Elan1200 Touchpad");
MODULE_LICENSE("GPL");


#define DELAY 16
#define DELAY_NS DELAY * 1000000

#ifdef MEASURE_TIME
static unsigned long start_j, stop_j;

static inline int
j_delta_msec(unsigned long *a, unsigned long *b) {
	return jiffies_to_msecs(*a - *b);
}
#endif

#define INPUT_DEV_TOUCHPAD_NAME "FilteredELAN1200"
#define INPUT_DEV_MOUSE_NAME "ELAN1200 Mouse"

#define MAX_CONTACTS 5
#define MAX_TIMESTAMP_INTERVAL 1000000

#define USB_VENDOR_ID_ELAN 0x04f3
#define USB_DEVICE_ID_1200 0x3022

#define INPUT_MODE_TOUCHPAD 0x03
#define LATENCY_MODE_NORMAL 0x00

#define MT_ID_NULL	(-1)
#define MT_ID_MIN	0
#define MT_ID_MAX	65535
#define MT_ID_SGN	((MT_ID_MAX + 1) >> 1)

#define DELAYED_FLAG_PENDING	0
#define DELAYED_FLAG_RUNNING	1

#define INPUT_SYNC_UDELAY 4000

#define ELAN_REPORT_ID 0x04
#define ELAN_REPORT_SIZE 14
// report size in bits without a report id byte
#define ELAN_REPORT_SIZE_BITS (ELAN_REPORT_SIZE - 1) * 8

struct contact {
	bool in_report;
	__s32 x;
	__s32 y;
	bool tool;
	bool touch;
};

struct elan_application {
	struct input_dev *input;

	struct contact hw_state[MAX_CONTACTS];
	struct contact delayed_state[MAX_CONTACTS];

	bool left_button_state;
	__s32 num_expected;
	__s32 num_received;

	int last_tracking_id;
	int tracking_ids[MAX_CONTACTS];

	unsigned long delayed_flags;
	struct timer_list timer;

	__s32 dev_time;
	unsigned long jiffies;
	int timestamp;
	int prev_scantime;
	__s32 scantime_logical_max;
};

struct elan_features {
	__s16 inputmode_report_id;
	__s16 inputmode_index;
	__s16 latency_report_id;
	__s16 latency_index;
};

struct elan_usages {
	__s32 *x, *y;
	bool *tool;
	bool *touch;
	__s32 *slot;
	__s32 *num_contacts;
	__s32 *scantime;
};

struct elan_device {
	struct hid_device *hdev;
	struct elan_usages usages;
	struct elan_application app;
	struct elan_features features;
};


static void init_app_vars(struct elan_application *app) {
	int i;

	app->timestamp = 0;
	app->jiffies = jiffies;
	app->prev_scantime = 0;

	app->left_button_state = 0;
	app->last_tracking_id = MT_ID_MIN;
	app->num_received = 0;

	for (i = 0; i < MAX_CONTACTS; i++) {
		struct contact* hw = &app->hw_state[i];
		hw->in_report = 0;
		hw->x = hw->y = 0;
		hw->tool = 1;
		hw->touch = 0;
		app->tracking_ids[i] = MT_ID_NULL;
	}

	clear_bit(DELAYED_FLAG_PENDING, &app->delayed_flags);
	clear_bit(DELAYED_FLAG_RUNNING, &app->delayed_flags);
}


static int mt_compute_timestamp(struct elan_application *app, __s32 value)
{
	long delta = value - app->prev_scantime;
	unsigned long jdelta = jiffies_to_usecs(jiffies - app->jiffies);

	app->jiffies = jiffies;

	if (delta < 0)
		delta += app->scantime_logical_max;

	delta *= 100;

	app->prev_scantime = value;

	if (jdelta > MAX_TIMESTAMP_INTERVAL)
		return 0;
	else
		return app->timestamp + delta;
}


static void check_button_state(struct hid_device *hdev, struct hid_report *report,
				struct elan_application* app) {
	int r, n;
	unsigned count;
	struct hid_field *field;
	struct hid_usage *usage;
	__s32 value;

	for (r = 0; r < report->maxfield; r++) {
		field = report->field[r];
		count = field->report_count;

		if (!(HID_MAIN_ITEM_VARIABLE & field->flags))
			continue;
		for (n = 0; n < count; n++) {
			usage = &field->usage[n];
			value = field->value[n];
			if (!usage->type || !(hdev->claimed & HID_CLAIMED_INPUT))
				continue;
			if (usage->type == EV_KEY && usage->code == BTN_LEFT) {
				app->left_button_state = value;
				continue;
			}
		}
	}
}


static void send_report(struct elan_application *app, bool delay)
{
	struct input_dev *input = app->input;

	int i;
	int oldest_slot;
	int old_id;
	int current_id;
	int current_touches = 0;
	int tool = MT_TOOL_FINGER;
	struct contact *ct;
	struct contact *state = delay ? app->delayed_state : app->hw_state;

	for (i = 0; i < MAX_CONTACTS; i++) {
		ct = &(state[i]);
		if (ct->touch)
			current_touches++;
		if (!ct->in_report)
			continue;
		input_mt_slot(input, i);

		if (ct->touch && app->tracking_ids[i] == MT_ID_NULL)
			app->tracking_ids[i] = app->last_tracking_id++ & MT_ID_MAX;
		if (!ct->touch)
			app->tracking_ids[i] = MT_ID_NULL;

		input_event(input, EV_ABS, ABS_MT_TRACKING_ID, app->tracking_ids[i]);

		if (app->tracking_ids[i] != MT_ID_NULL) {
			if (!ct->tool)
				tool = MT_TOOL_PALM;
			input_event(input, EV_ABS, ABS_MT_TOOL_TYPE, tool);
			input_event(input, EV_ABS, ABS_MT_POSITION_X, ct->x);
			input_event(input, EV_ABS, ABS_MT_POSITION_Y, ct->y);
		}

		app->hw_state[i].in_report = 0;
	}

	if (current_touches > 0) {
		oldest_slot = 0;
		old_id = app->last_tracking_id % MT_ID_MAX + 1;
		for (i = 0; i < MAX_CONTACTS; i++) {
			if (app->tracking_ids[i] == MT_ID_NULL)
				continue;
			current_id = app->tracking_ids[i];
			if ((current_id - old_id) & MT_ID_SGN) {
				oldest_slot = i;
				old_id = current_id;
			}
		}
		if (app->tracking_ids[oldest_slot] != MT_ID_NULL) {
			input_event(input, EV_ABS, ABS_X, (state[oldest_slot]).x);
			input_event(input, EV_ABS, ABS_Y, (state[oldest_slot]).y);
		}
	}

	input_event(input, EV_KEY, BTN_TOUCH, current_touches > 0);
	input_mt_report_finger_count(input, current_touches);

	input_event(input, EV_KEY, BTN_LEFT, app->left_button_state);

	//if (delay)
		input_event(input, EV_MSC, MSC_TIMESTAMP, app->timestamp);
	input_sync(input);
}


static void timer_thread(struct timer_list *t)
{
	struct elan_application *app = from_timer(app, t, timer);

	set_bit(DELAYED_FLAG_RUNNING, &app->delayed_flags);
	if (test_and_clear_bit(DELAYED_FLAG_PENDING, &app->delayed_flags)) {
		send_report(app, 1);
	}
	clear_bit(DELAYED_FLAG_RUNNING, &app->delayed_flags);
#ifdef MEASURE_TIME
	stop_j = jiffies;
	printk("Timer triggered: %d ms\n", j_delta_msec(&stop_j, &start_j));
#endif
}


static int needs_delay(struct contact *state) {
	int i;
	int current_touches = 0;
	int num_reported = 0;

	for (i = 0; i < MAX_CONTACTS; i++) {
		if ((state[i]).in_report) {
			num_reported++;
		}
		if ((state[i]).touch)
			current_touches++;
	}

	return num_reported == 1 && current_touches == 0;
}


static void elan_touchpad_report(struct elan_application *app,
					struct elan_usages *usages) {
	struct contact *ct;

	if (test_and_clear_bit(DELAYED_FLAG_PENDING, &app->delayed_flags)) {
		if (*usages->num_contacts == 1) {
			send_report(app, 1);
			udelay(INPUT_SYNC_UDELAY);
		}
#ifdef MEASURE_TIME
		stop_j = jiffies;
		printk("Next event arrived: %d ms\n", j_delta_msec(&stop_j, &start_j));
#endif
	} else if (test_bit(DELAYED_FLAG_RUNNING, &app->delayed_flags)) {
		udelay(INPUT_SYNC_UDELAY);
	}

	if (*usages->num_contacts > 0)
		app->num_expected = *usages->num_contacts;

	app->num_received++;

	ct = &app->hw_state[*usages->slot];
	ct->in_report = 1;
	ct->tool = *usages->tool;
	ct->x = *usages->x;
	ct->y = *usages->y;
	ct->touch = *usages->touch;

	if (app->num_received != app->num_expected)
		return;

	app->timestamp = mt_compute_timestamp(app, *usages->scantime);

	if (needs_delay(app->hw_state)) {
		memcpy(app->delayed_state, app->hw_state, sizeof(app->hw_state));
		mod_timer(&app->timer, jiffies + nsecs_to_jiffies(DELAY_NS));
		set_bit(DELAYED_FLAG_PENDING, &app->delayed_flags);
#ifdef MEASURE_TIME
		printk("Timer started\n");
		start_j = jiffies;
#endif
	} else {
		send_report(app, 0);
	}

	app->num_received = 0;
}


static void elan_report(struct hid_device *hdev, struct hid_report *report)
{
	struct hid_field *field = report->field[0];
	struct elan_device *td = hid_get_drvdata(hdev);

	if (!(hdev->claimed & HID_CLAIMED_INPUT))
		return;

	if (report->id == ELAN_REPORT_ID && report->size == ELAN_REPORT_SIZE_BITS) {
		check_button_state(hdev, report, &td->app);
		return elan_touchpad_report(&td->app, &td->usages);
	}

	if (field && field->hidinput && field->hidinput->input)
		input_sync(field->hidinput->input);
}


static int elan_event(struct hid_device *hdev, struct hid_field *field,
				struct hid_usage *usage, __s32 value)
{
	if (field->report && field->report->id == ELAN_REPORT_ID &&
	    field->report->size == ELAN_REPORT_SIZE_BITS) {
		if (hdev->claimed & HID_CLAIMED_HIDDEV && hdev->hiddev_hid_event)
			hdev->hiddev_hid_event(hdev, field, usage, value);
		return 1;
	}
	return 0;
}


static void set_abs(struct input_dev *input, unsigned int code,
		struct hid_field *field)
{
	int fmin = field->logical_minimum;
	int fmax = field->logical_maximum;
	input_set_abs_params(input, code, fmin, fmax, 0, 0);
	input_abs_set_res(input, code, hidinput_calc_abs_res(field, code));
}


static int elan_input_mapping(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	int code;
	__s32 **target;
	struct elan_device *td = hid_get_drvdata(hdev);

	if (field->application != HID_DG_TOUCHPAD)
		return 0;

	switch (usage->hid & HID_USAGE_PAGE) {
	case HID_UP_GENDESK:
		switch (usage->hid) {
		case HID_GD_X:
			code = ABS_MT_POSITION_X;
			set_abs(hi->input, code, field);
			td->usages.x = &field->value[usage->usage_index];
			return 1;
		case HID_GD_Y:
			code = ABS_MT_POSITION_Y;
			set_abs(hi->input, code, field);
			td->usages.y = &field->value[usage->usage_index];
			return 1;
		}
		return 0;
	case HID_UP_DIGITIZER:
		switch (usage->hid) {
		case HID_DG_CONFIDENCE:
			input_set_abs_params(hi->input,
					ABS_MT_TOOL_TYPE,
					MT_TOOL_FINGER,
					MT_TOOL_PALM, 0, 0);
			target = (__s32**)(char*)&(td->usages.tool);
			*target = &field->value[usage->usage_index];
			return 1;
		case HID_DG_TIPSWITCH:
			input_set_capability(hi->input, EV_KEY, BTN_TOUCH);

			target = (__s32**)(char*)&(td->usages.touch);
			*target = &field->value[usage->usage_index];
			return 1;
		case HID_DG_CONTACTID:
			td->usages.slot = &field->value[usage->usage_index];
			return 1;
		case HID_DG_CONTACTCOUNT:
			td->usages.num_contacts = &field->value[usage->usage_index];
			return 1;
		case HID_DG_SCANTIME:
			input_set_capability(hi->input, EV_MSC, MSC_TIMESTAMP);
			td->usages.scantime = &field->value[usage->usage_index];
			td->app.scantime_logical_max = field->logical_maximum;
			return 1;
		case HID_DG_CONTACTMAX:
			return -1;
		case HID_DG_TOUCH:
			return -1;
		}
		return 0;
	case HID_UP_BUTTON:
		code = BTN_MOUSE + ((usage->hid - 1) & HID_USAGE);
		if (field->application == HID_DG_TOUCHPAD &&
		    (usage->hid & HID_USAGE) > 1)
			code--;
		hid_map_usage(hi, usage, bit, max, EV_KEY, code);
		if (!*bit)
			return -1;
		input_set_capability(hi->input, EV_KEY, code);
		return 1;
	case 0xff000000:
		return -1;
	}

	return 0;
}


static int elan_input_mapped(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	if (hi->application == HID_DG_TOUCHPAD) {
		return -1;
	}
	return 0;
}


static int elan_input_configured(struct hid_device *hdev, struct hid_input *hi)
{
	int ret;
	struct input_dev *input = hi->input;
	struct elan_device *td = hid_get_drvdata(hdev);
	char *input_name;
	const char *name = NULL;

	switch (hi->application) {
	case HID_GD_MOUSE:
		name = INPUT_DEV_MOUSE_NAME;
		break;
	case HID_DG_TOUCHPAD:
		td->app.input = input;
		name = INPUT_DEV_TOUCHPAD_NAME;
		break;
	default:
		return 0;
	}

	input_name = devm_kzalloc(&hi->input->dev, strlen(name) + 1, GFP_KERNEL);
	if (input_name) {
		sprintf(input_name, "%s", name);
		hi->input->name = name;
	}

	if (hi->application != HID_DG_TOUCHPAD)
		return 0;

	__set_bit(INPUT_PROP_BUTTONPAD, input->propbit);
	ret = input_mt_init_slots(input, MAX_CONTACTS, INPUT_MT_POINTER);
	if (ret)
		return ret;

	return 0;
}


static void mt_get_feature(struct hid_device *hdev, struct hid_report *report)
{
	int ret;
	u32 size = hid_report_len(report);
	u8 *buf;

	if (hdev->quirks & HID_QUIRK_NO_INIT_REPORTS)
		return;

	buf = hid_alloc_report_buf(report, GFP_KERNEL);
	if (!buf)
		return;

	ret = hid_hw_raw_request(hdev, report->id, buf, size,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0) {
		dev_warn(&hdev->dev, "failed to fetch feature %d\n",
			 report->id);
	} else {
		ret = hid_report_raw_event(hdev, HID_FEATURE_REPORT, buf,
					   size, 0);
		if (ret)
			dev_warn(&hdev->dev, "failed to report feature\n");
	}

	kfree(buf);
}


static void elan_feature_mapping(struct hid_device *hdev,
		struct hid_field *field, struct hid_usage *usage)
{
	struct elan_device *td = hid_get_drvdata(hdev);

	switch (usage->hid) {
	case HID_DG_CONTACTMAX:
		mt_get_feature(hdev, field->report);
		break;
	case HID_DG_INPUTMODE:
		if (td->features.inputmode_report_id < 0) {
			td->features.inputmode_report_id = field->report->id;
			td->features.inputmode_index = usage->usage_index;
		}
		break;
	case HID_DG_BUTTONTYPE:
		if (usage->usage_index >= field->report_count) {
			dev_err(&hdev->dev, "HID_DG_BUTTONTYPE out of range\n");
			break;
		}
		mt_get_feature(hdev, field->report);
		break;
	case HID_DG_LATENCYMODE:
		td->features.latency_report_id = field->report->id;
		td->features.latency_index = usage->usage_index;
		break;
	case 0xff0000c5:
		/* Retrieve the Win8 blob once to enable some devices */
		if (usage->usage_index == 0)
			mt_get_feature(hdev, field->report);
		break;
	}
}


static void elan_set_modes(struct hid_device *hdev)
{
	struct elan_device *td = hid_get_drvdata(hdev);
	struct hid_report *r;
	struct hid_report_enum *re;

	if (td->features.inputmode_report_id < 0 ||
	    td->features.latency_report_id < 0)
		return;
	re = &(hdev->report_enum[HID_FEATURE_REPORT]);
	r = re->report_id_hash[td->features.inputmode_report_id];
	if (r) {
		r->field[0]->value[td->features.inputmode_index] = INPUT_MODE_TOUCHPAD;
		hid_hw_request(hdev, r, HID_REQ_SET_REPORT);
	}
	r = re->report_id_hash[td->features.latency_report_id];
	if (r) {
		r->field[0]->value[td->features.latency_index] = LATENCY_MODE_NORMAL;
		hid_hw_request(hdev, r, HID_REQ_SET_REPORT);
	}
}


static int elan_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct elan_device *td;

	td = devm_kzalloc(&hdev->dev, sizeof(struct elan_device), GFP_KERNEL);
	if (!td) {
		dev_err(&hdev->dev, "cannot allocate elan data\n");
		return -ENOMEM;
	}

	td->hdev = hdev;
	hid_set_drvdata(hdev, td);

	init_app_vars(&td->app);
	td->features.inputmode_report_id = -1;
	td->features.latency_report_id = -1;

	hdev->quirks |= HID_QUIRK_NO_INPUT_SYNC;
	hdev->quirks |= HID_QUIRK_INPUT_PER_APP;

	timer_setup(&td->app.timer, timer_thread, 0);

	ret = hid_parse(hdev);
	if (ret != 0)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	elan_set_modes(hdev);

	return 0;
}


#ifdef CONFIG_PM
static int elan_reset_resume(struct hid_device *hdev)
{
	elan_set_modes(hdev);
	return 0;
}

static int elan_resume(struct hid_device *hdev)
{
	hid_hw_idle(hdev, 0, 0, HID_REQ_SET_IDLE);
	return 0;
}
#endif


static void elan_remove(struct hid_device *hdev)
{
	struct elan_device *td = hid_get_drvdata(hdev);
	del_timer_sync(&td->app.timer);
	hid_hw_stop(hdev);
}


static const struct hid_device_id elan_devices[] = {
	{ HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_ELAN, USB_DEVICE_ID_1200) },
	{ }
};

MODULE_DEVICE_TABLE(hid, elan_devices);

static const struct hid_usage_id elan_grabbed_usages[] = {
	{ HID_ANY_ID, HID_ANY_ID, HID_ANY_ID },
	{ HID_ANY_ID - 1, HID_ANY_ID - 1, HID_ANY_ID - 1}
};

static struct hid_driver elan_driver = {
	.name				= "hid-elan1200",
	.id_table			= elan_devices,
	.probe 				= elan_probe,
	.remove				= elan_remove,
	.feature_mapping		= elan_feature_mapping,
	.input_mapping			= elan_input_mapping,
	.input_mapped			= elan_input_mapped,
	.input_configured		= elan_input_configured,
	.usage_table			= elan_grabbed_usages,
	.event				= elan_event,
	.report				= elan_report,
#ifdef CONFIG_PM
	.reset_resume			= elan_reset_resume,
	.resume 			= elan_resume,
#endif
};


module_hid_driver(elan_driver);
