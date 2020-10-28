#include <linux/hid.h>
#include <linux/module.h>
#include <linux/input/mt.h>

MODULE_AUTHOR("Alexander Mishurov <ammishurov@gmail.com>");
MODULE_DESCRIPTION("Elan1200 TouchPad");
MODULE_LICENSE("GPL");


#define RELEASE_TIMEOUT 25
#define MAX_CONTACTS 5

#define USB_VENDOR_ID_ELANTECH 0x04f3
#define USB_DEVICE_ID_ELAN1200_I2C_TOUCHPAD 0x3022
#define MAX_TIMESTAMP_INTERVAL 1000000

struct contact {
	bool in_report;
	__s32 x;
	__s32 y;
	bool tool;
	bool touch;
};

struct elan_application {
	struct contact hw_state[MAX_CONTACTS];
	struct contact prev_state[MAX_CONTACTS];
	struct contact delayed_state[MAX_CONTACTS];

	bool left_button_state;
	int delayed_slot;
	__s32 num_expected;
	__s32 num_received;
	
	__s32 dev_time;
	unsigned long jiffies;
	int timestamp;
	int prev_scantime;
	__s32 scantime_logical_max;
};

struct elan_usages {
	__s32 *x, *y;
	__s32 *contactid;
	bool *touch;
	bool *tool;
	__s32 *slot;
	__s32 *num_contacts;
	__s32 *scantime;
};

struct elan_device {
	struct timer_list release_timer;
	struct input_dev *input;
	struct hid_device *hdev;
	struct elan_usages usages;
	struct elan_application app;
	spinlock_t lock;
};

static void init_app(struct elan_application *app) {
	int i;
	app->timestamp = 0;
	app->jiffies = 0;
	app->prev_scantime = 0;

	app->left_button_state = 0;
	app->delayed_slot = -1;
	for (i = 0; i < MAX_CONTACTS; i++) {
		struct contact* hw = &app->hw_state[i];
		struct contact* pr = &app->prev_state[i];
		struct contact* de = &app->delayed_state[i];
		hw->in_report = pr->in_report = de->in_report = 0;
		hw->x = hw->y = pr->x = pr->y = de->x = de->y = 0;
		hw->tool = pr->tool = de->tool = 0;
		hw->touch = pr->touch = de->touch = 0;
	}
}

static int compute_timestamp(struct elan_application *app, __s32 value)
{
	long delta = value - app->prev_scantime;
	unsigned long jdelta = jiffies_to_usecs(jiffies - app->jiffies);

	app->jiffies = jiffies;

	if (delta < 0)
		delta += app->scantime_logical_max;

	delta *= 100;

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


static void send_report(struct elan_device *td, bool fr_timer)
{
	struct elan_application *app = &td->app;
	struct input_dev *input = td->input;

	int i;
	bool delay = 0;
	bool touch;
	int tool = MT_TOOL_FINGER;
	int current_touches = 0;
	int prev_touches = 0;
	struct contact *ct;
	struct contact *state = fr_timer ? app->delayed_state : app->hw_state;

	for (i = 0; i < MAX_CONTACTS; i++) {
		if (fr_timer)
			break;
		if (app->hw_state[i].touch)
			current_touches++;
		if (app->prev_state[i].touch)
			prev_touches++;
	}

	delay = !fr_timer && prev_touches == 1 && current_touches == 0;
	if (delay)
		memcpy(app->delayed_state, app->hw_state, sizeof(app->hw_state));

	for (i = 0; i < MAX_CONTACTS; i++) {
		ct = &(state[i]);
		if (!ct->in_report)
			continue;
		if (!ct->tool)
			tool = MT_TOOL_PALM;

		touch = ct->touch;
		if (delay && !ct->touch) {
			app->delayed_slot = i;
			current_touches = 1;
			touch = 1;
		}

		input_mt_slot(input, i);
		input_mt_report_slot_state(input, tool, touch);
		if (touch) {
			input_event(input, EV_ABS, ABS_MT_POSITION_X, ct->x);
			input_event(input, EV_ABS, ABS_MT_POSITION_Y, ct->y);
		}
		ct->in_report = 0;
	}

	if (!fr_timer)
		input_event(input, EV_KEY, BTN_LEFT, app->left_button_state);
	input_mt_sync_frame(input);
	if (!fr_timer)
		input_event(input, EV_MSC, MSC_TIMESTAMP, app->timestamp);

	if (delay) {
		mod_timer(&td->release_timer,
			jiffies + msecs_to_jiffies(RELEASE_TIMEOUT));
	}

	input_sync(input);
}


static void elan_expired_timeout(struct timer_list *t)
{
	struct elan_device *td = from_timer(td, t, release_timer);
	spin_lock(&td->lock);
	if (td->app.delayed_slot > -1) {
		send_report(td, 1);
		td->app.delayed_slot = -1;
	}
	spin_unlock(&td->lock);
}


static void elan_report(struct hid_device *hdev, struct hid_report *report)
{
	struct hid_field *field = report->field[0];
	struct elan_device *td = hid_get_drvdata(hdev);
	struct elan_application *app = &td->app;
	
	int slot;

	if (!(hdev->claimed & HID_CLAIMED_INPUT))
		return;

	if (!field || !field->hidinput || !field->hidinput->input)
		return;

	if (*td->usages.num_contacts > 0) {
		app->num_expected = *td->usages.num_contacts;
		app->num_received = 0;
		check_button_state(hdev, report, app);
	}

	app->num_received++;

	slot = *td->usages.slot;

	spin_lock_bh(&td->lock);
	if (app->delayed_slot > -1 &&
	    (slot == app->delayed_slot || app->num_expected > 1)) {
		app->delayed_slot = -1;
		del_timer(&td->release_timer);
	}
	spin_unlock_bh(&td->lock);

	app->hw_state[slot].in_report = 1;
	app->hw_state[slot].x = *td->usages.x;
	app->hw_state[slot].y = *td->usages.y;
	app->hw_state[slot].tool = *td->usages.tool;
	app->hw_state[slot].touch = *td->usages.touch;

	if (app->num_received == app->num_expected) {
		spin_lock_bh(&td->lock);
		app->timestamp = compute_timestamp(app, *td->usages.scantime);
		td->input = field->hidinput->input;
		send_report(td, 0);
		memcpy(app->prev_state, app->hw_state, sizeof(app->hw_state));
		spin_unlock_bh(&td->lock);
	}
}


static int elan_event(struct hid_device *hdev, struct hid_field *field,
				struct hid_usage *usage, __s32 value)
{
	if (hdev->claimed & HID_CLAIMED_HIDDEV && hdev->hiddev_hid_event)
		hdev->hiddev_hid_event(hdev, field, usage, value);
	return 1;
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
		return -1;

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
	return -1;
}


static int elan_input_configured(struct hid_device *hdev, struct hid_input *hi)
{
	int ret;
	struct input_dev *input = hi->input;

	__set_bit(INPUT_PROP_BUTTONPAD, input->propbit);

	ret = input_mt_init_slots(input, MAX_CONTACTS, INPUT_MT_POINTER);
	if (ret)
		return ret;
	return 0;
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

	init_app(&td->app);

	timer_setup(&td->release_timer, elan_expired_timeout, 0);
	spin_lock_init(&td->lock);

	ret = hid_parse(hdev);
	if (ret != 0)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	return 0;
}

static void elan_release_contacts(struct hid_device *hdev)
{
	struct elan_device *td = hid_get_drvdata(hdev);
	struct input_dev *input = td->input;
	int i;

	spin_lock_bh(&td->lock);
	for (i = 0; i < MAX_CONTACTS; i++) {
		input_mt_slot(input, i);
		input_mt_report_slot_inactive(input);
	}
	input_mt_sync_frame(input);
	input_sync(input);
	spin_unlock_bh(&td->lock);
}

#ifdef CONFIG_PM
static int elan_reset_resume(struct hid_device *hdev)
{
	elan_release_contacts(hdev);
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
	del_timer_sync(&td->release_timer);
	hid_hw_stop(hdev);
}

static const struct hid_device_id elan_devices[] = {
	{ HID_I2C_DEVICE(USB_VENDOR_ID_ELANTECH,
		USB_DEVICE_ID_ELAN1200_I2C_TOUCHPAD), 0 },
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
	.input_mapping			= elan_input_mapping,
	.input_mapped			= elan_input_mapped,
	.input_configured 		= elan_input_configured,
	.usage_table			= elan_grabbed_usages,
	.event				= elan_event,
	.report				= elan_report,
#ifdef CONFIG_PM
	.reset_resume			= elan_reset_resume,
	.resume 			= elan_resume,
#endif
};


module_hid_driver(elan_driver);
