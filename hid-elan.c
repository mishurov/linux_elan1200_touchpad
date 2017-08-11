#include <linux/hid.h>
#include <linux/module.h>
#include <linux/input/mt.h>

MODULE_AUTHOR("Alexander Mishurov <ammishurov@gmail.com>");
MODULE_DESCRIPTION("Elan1200 TouchPad");

#define INPUT_REPORT_ID 0x04
#define INPUT_REPORT_SIZE 14

// the touchpad reports fake events with the fifth contact
// drop it for the greater good
#define MAX_CONTACTS 4

#define MAX_X 3200
#define MAX_Y 2198
#define RESOLUTION 31
#define MAX_TOUCH_WIDTH 15

// may depend on a processor etc
#define RELEASE_TIMEOUT 14
// must be longer than Synaptics's MaxTapTime * 2 + RELEASE_TIMEOUT
#define TOUCH_DURATION 400

#define INPUTMODE_TOUCHPAD 0x03
#define USB_VENDOR_ID_ELANTECH 0x04f3
#define USB_DEVICE_ID_ELAN1200_I2C_TOUCHPAD 0x3022


struct elan_drvdata {
	struct input_dev *input;
	int num_expected;
	int num_received;
	struct input_mt_pos coords[MAX_CONTACTS];
	struct timer_list release_timer;
	bool timer_pending;
	unsigned long last_touch[MAX_CONTACTS];
	bool delayed[MAX_CONTACTS];
	bool dublicate[MAX_CONTACTS];
	bool in_touch[MAX_CONTACTS];
	bool syncing;
	__s16 inputmode;
	__s16 inputmode_index;
	__s16 maxcontact_report_id; 
};

static void elan_release_contact(struct elan_drvdata *td,
				struct input_dev *input, int slot_id)
{
	input_mt_slot(input, slot_id);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	input_report_abs(input, ABS_MT_POSITION_X, td->coords[slot_id].x);
	input_report_abs(input, ABS_MT_POSITION_Y, td->coords[slot_id].y);
}

static void elan_release_contacts(struct hid_device *hdev)
{
	struct elan_drvdata *td = hid_get_drvdata(hdev);
	struct input_dev *input = td->input;
	int i;
	for (i = 0; i < MAX_CONTACTS; i++) {
		elan_release_contact(td, input, i);
	}
	input_mt_sync_frame(input);
	input_sync(input);
}

static void elan_release_timeout(unsigned long arg)
{
	int slot_id = 0;
	struct hid_device *hdev = (void *)arg;
	struct elan_drvdata *td = hid_get_drvdata(hdev);
	struct input_dev *input = td->input;
	struct input_mt *mt = input->mt;
	struct input_mt_slot *slot = &mt->slots[slot_id];

	if (!td->timer_pending)
		return;
	
	if (!(input_mt_is_active(slot) && input_mt_is_used(mt, slot))) {
		elan_release_contact(td, input, slot_id);
		td->dublicate[slot_id] = true;

		if (!td->syncing) {
			td->syncing = true;
			input_mt_sync_frame(input);
			input_sync(input);
			td->syncing = false;
		}
	}

	td->timer_pending = false;
}


static void elan_report_input(struct elan_drvdata *td, u8 *data)
{
	struct input_dev *input = td->input;
	struct input_mt *mt = input->mt;

	int x, y, mk_x, mk_y, area_x, area_y, touch_major,
	    touch_minor, num_contacts;
	
	bool orientation;
	int slot_id = data[1] >> 4;
	struct input_mt_slot *slot = &mt->slots[slot_id];
	bool is_touch = (data[1] & 0x0f) == 3;
	bool is_release = (data[1] & 0x0f) == 1;

	if (!(is_touch || is_release) ||
	    slot_id < 0 || slot_id >= MAX_CONTACTS)
		return;

	num_contacts = data[8];

	if (num_contacts > MAX_CONTACTS)
		num_contacts = MAX_CONTACTS;

	if (num_contacts > 0)
		td->num_expected = num_contacts;
	td->num_received++;

	// ignore dublicates
	if (input_mt_is_active(slot) && input_mt_is_used(mt, slot))
		td->dublicate[slot_id] = true;
	else
		td->dublicate[slot_id] = false;

	x = (data[3] << 8) | data[2];
	y = (data[5] << 8) | data[4];

	td->coords[slot_id].x = x;
	td->coords[slot_id].y = y;

	mk_x = data[11] & 0x0f;
	mk_y = data[11] >> 4;
	
	area_x = mk_x * (MAX_X >> 1);
	area_y = mk_y * (MAX_Y >> 1);
	touch_major = max(area_x, area_y);
	touch_minor = min(area_x, area_y);
	orientation = area_x > area_y;

	// the workaround only for slot 0 and slot 1
	// to prevent random events during two-finger scrolling

	// only the touches after the artifical releases are that fast
	if (!td->dublicate[slot_id] && is_touch && td->timer_pending) {
		td->timer_pending = false;
		del_timer(&td->release_timer);
	}

	// there're no reasons to set the timer on every release
	// because the delayed releases slighly increase the UI latency
	// set it only if the second contact reported touches recently
	if (!td->dublicate[slot_id] && is_release && slot_id == 0 &&
	    jiffies - td->last_touch[1] < msecs_to_jiffies(TOUCH_DURATION)) {
		td->delayed[slot_id] = true;
	}

	if (is_touch) {
		td->last_touch[slot_id] = jiffies;
		td->in_touch[slot_id] = true;
	}

	if (is_release) {
		td->in_touch[slot_id] = false;
	}


	if (!td->dublicate[slot_id] && !td->delayed[slot_id]) {
		input_mt_slot(input, slot_id);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, is_touch);
	
		input_report_abs(input, ABS_MT_POSITION_X, x);
		input_report_abs(input, ABS_MT_POSITION_Y, y);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, touch_major);
		input_report_abs(input, ABS_MT_TOUCH_MINOR, touch_minor);
		input_report_abs(input, ABS_MT_ORIENTATION, orientation);
	}

	input_report_key(input, BTN_LEFT, data[9] & 0x01);

	if (td->num_received >= td->num_expected) {
		// set the timer after the subsequent reports were collected
		if ((td->num_received == 1 &&
		     slot_id == 0 && td->delayed[0]) ||
		    (td->num_received == 1 &&
		     slot_id == 1 && !td->in_touch[1]) ||
		    (td->num_received > 1 &&
		     (td->delayed[0] || !td->in_touch[1]))) {
			td->timer_pending = true;
			mod_timer(&td->release_timer,
				  jiffies + msecs_to_jiffies(RELEASE_TIMEOUT));
		}
		td->delayed[0] = false;

		if (!td->syncing) {
			td->syncing = true;
			input_mt_sync_frame(input);
			input_sync(input);
			td->syncing = false;
		}
		td->num_received = 0;
	}
}

static int elan_raw_event(struct hid_device *hdev,
		struct hid_report *report, u8 *data, int size)
{
	struct elan_drvdata *drvdata = hid_get_drvdata(hdev);

	if (data[0] == INPUT_REPORT_ID && size == INPUT_REPORT_SIZE) {
		elan_report_input(drvdata, data);
		return 1;
	}

	return 0;
}

static int elan_input_configured(struct hid_device *hdev, struct hid_input *hi)
{
	struct input_dev *input = hi->input;
	struct elan_drvdata *drvdata = hid_get_drvdata(hdev);

	int ret;

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, MAX_X, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, MAX_Y, 0, 0);
	input_abs_set_res(input, ABS_MT_POSITION_X, RESOLUTION);
	input_abs_set_res(input, ABS_MT_POSITION_Y, RESOLUTION);

	// MAX_X is greater than MAX_Y
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0,
	                     MAX_TOUCH_WIDTH * (MAX_X >> 1), 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MINOR, 0,
	                     MAX_TOUCH_WIDTH * (MAX_X >> 1), 0, 0);
	input_set_abs_params(input, ABS_MT_ORIENTATION, 0, 1, 0, 0);

	__set_bit(INPUT_PROP_BUTTONPAD, input->propbit);
	__set_bit(BTN_LEFT, input->keybit);

	ret = input_mt_init_slots(input, MAX_CONTACTS, INPUT_MT_POINTER);

	if (ret) {
		hid_err(hdev, "Elan input mt init slots failed: %d\n", ret);
		return ret;
	}

	drvdata->input = input;

	return 0;
}

static void elan_set_maxcontacts(struct hid_device *hdev)
{
	struct elan_drvdata *td = hid_get_drvdata(hdev);
	struct hid_report *r;
	struct hid_report_enum *re;

	if (td->maxcontact_report_id < 0)
		return;

	re = &hdev->report_enum[HID_FEATURE_REPORT];
	r = re->report_id_hash[td->maxcontact_report_id];
	if (r) {
		if (r->field[0]->value[0] != MAX_CONTACTS) {
			r->field[0]->value[0] = MAX_CONTACTS;
			hid_hw_request(hdev, r, HID_REQ_SET_REPORT);
		}
	}
}

static void elan_set_input_mode(struct hid_device *hdev)
{
	struct elan_drvdata *td = hid_get_drvdata(hdev);
	struct hid_report *r;
	struct hid_report_enum *re;

	if (td->inputmode < 0)
		return;

	re = &(hdev->report_enum[HID_FEATURE_REPORT]);
	r = re->report_id_hash[td->inputmode];
	if (r) {
		r->field[0]->value[td->inputmode_index] = INPUTMODE_TOUCHPAD;
		hid_hw_request(hdev, r, HID_REQ_SET_REPORT);
	}
}

static void elan_get_feature(struct hid_device *hdev, struct hid_report *report)
{
	int ret, size = hid_report_len(report);
	u8 *buf;

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

	struct elan_drvdata *td = hid_get_drvdata(hdev);

	switch (usage->hid) {
	case HID_DG_INPUTMODE:
		if (td->inputmode < 0) {
			td->inputmode = field->report->id;
			td->inputmode_index = usage->usage_index;
		}
		break;
	case HID_DG_CONTACTMAX:
		elan_get_feature(hdev, field->report);
		td->maxcontact_report_id = field->report->id;
		break;
	default:
		if (usage->usage_index == 0) {
			elan_get_feature(hdev, field->report);
		}
	}
}


static int __maybe_unused elan_reset_resume(struct hid_device *hdev)
{
	elan_release_contacts(hdev);
	elan_set_maxcontacts(hdev);
	elan_set_input_mode(hdev);
	return 0;
}

static int elan_resume(struct hid_device *hdev)
{
	hid_hw_idle(hdev, 0, 0, HID_REQ_SET_IDLE);
	return 0;
}

static int elan_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct elan_drvdata *drvdata;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (drvdata == NULL) {
		hid_err(hdev, "Can't alloc Elan descriptor\n");
		return -ENOMEM;
	}

	hid_set_drvdata(hdev, drvdata);
	hdev->quirks |= HID_QUIRK_NO_INIT_REPORTS;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "Elan hid parse failed: %d\n", ret);
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "Elan hw start failed: %d\n", ret);
		return ret;
	}

	if (!drvdata->input) {
		hid_err(hdev, "Elan input not registered\n");
		ret = -ENOMEM;
		goto err_stop_hw;
	}

	drvdata->input->name = "Elan TouchPad";

	elan_set_maxcontacts(hdev);
	elan_set_input_mode(hdev);
	
	setup_timer(&drvdata->release_timer,
			elan_release_timeout,
			(unsigned long)hdev);
	return 0;

err_stop_hw:
	hid_hw_stop(hdev);
	return ret;
}


static void elan_remove(struct hid_device *hdev)
{
	struct elan_drvdata *td = hid_get_drvdata(hdev);
	del_timer_sync(&td->release_timer);
	hid_hw_stop(hdev);
}

static int elan_input_mapping(struct hid_device *hdev,
		struct hid_input *hi, struct hid_field *field,
		struct hid_usage *usage, unsigned long **bit,
		int *max)
{
	return -1;
}

static const struct hid_device_id elan_devices[] = {
	{ HID_I2C_DEVICE(USB_VENDOR_ID_ELANTECH,
		USB_DEVICE_ID_ELAN1200_I2C_TOUCHPAD), 0 },
	{ }
};

MODULE_DEVICE_TABLE(hid, elan_devices);

static struct hid_driver elan_driver = {
	.name				= "hid-elan",
	.id_table			= elan_devices,
	.probe 				= elan_probe,
	.remove				= elan_remove,
	.feature_mapping 		= elan_feature_mapping,
	.input_mapping			= elan_input_mapping,
	.input_configured		= elan_input_configured,
#ifdef CONFIG_PM
	.reset_resume			= elan_reset_resume,
	.resume 			= elan_resume,
#endif
	.raw_event			= elan_raw_event
};


module_hid_driver(elan_driver);

MODULE_LICENSE("GPL");

