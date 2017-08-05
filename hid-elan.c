#include <linux/hid.h>
#include <linux/module.h>
#include <linux/input/mt.h>

MODULE_AUTHOR("Alexander Mishurov <ammishurov@gmail.com>");
MODULE_DESCRIPTION("Elan1200 TouchPad");

#define INPUT_REPORT_ID 0x04
#define INPUT_REPORT_SIZE 14
#define MAX_CONTACTS 5

#define MAX_X 3200
#define MAX_Y 2198
#define RESOLUTION 31
#define MAX_TOUCH_WIDTH 15
#define RELEASE_TIMEOUT 25

#define MT_INPUTMODE_TOUCHPAD 0x03
#define USB_VENDOR_ID_ELANTECH 0x04f3
#define USB_DEVICE_ID_ELAN1200_I2C_TOUCHPAD 0x3022

#define DMAX 100


struct elan_drvdata {
	struct input_dev *input;
	int num_expected;
	int num_received;
	struct timer_list release_timer[MAX_CONTACTS];
	bool timers[MAX_CONTACTS];
	bool being_reported[MAX_CONTACTS];
	struct input_mt_pos coords[MAX_CONTACTS];
	int slots[MAX_CONTACTS];
};


static void elan_release_contact(struct elan_drvdata *td,
								 struct input_dev *input,
								 int slot_id) {
	int error;
	td->being_reported[slot_id] = true;
	error = input_mt_assign_slots(input, td->slots,
							td->coords, MAX_CONTACTS, DMAX);
	if (error)
		td->slots[slot_id] = slot_id;

	input_mt_slot(input, td->slots[slot_id]);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	input_report_abs(input, ABS_MT_POSITION_X, td->coords[slot_id].x);
	input_report_abs(input, ABS_MT_POSITION_Y, td->coords[slot_id].y);
	td->being_reported[slot_id] = false;
}


static void elan_expired_timeout(unsigned long arg)
{
	struct hid_device *hdev = (void *)arg;
	struct elan_drvdata *td = hid_get_drvdata(hdev);
	struct input_dev *input = td->input;
	int i;
	int released = 0;
	
	for(i = 0; i < MAX_CONTACTS; i++) {
		if (td->timers[i] && !td->being_reported[i] &&
			!timer_pending(&td->release_timer[i])) {
			elan_release_contact(td, input, i);
			td->timers[i] = false;
			released++;
		}
	}
	if (released > 0) {
		input_mt_sync_frame(input);
		input_sync(input);
	}
}


static void elan_report_input(struct elan_drvdata *td, u8 *data)
{
	struct input_dev *input = td->input;
	
	int error;
	int x, y, mk_x, mk_y, area_x, area_y, touch_major,
	    touch_minor, num_contacts;
	bool orientation;
	int slot_id = data[1] >> 4;
	bool is_touch = (data[1] & 0x0f) == 3;
	bool is_release = (data[1] & 0x0f) == 1;
	// the touchpad sometimes generates a fake report
	bool is_fake = (data[1] == 0x41 && data[2] == 0xc6);
	
	if (is_fake || !(is_touch || is_release) ||
	    slot_id < 0 || slot_id >= MAX_CONTACTS)
		return;

	x = (data[3] << 8) | data[2];
	y = (data[5] << 8) | data[4];

	mk_x = data[11] & 0x0f;
	mk_y = data[11] >> 4;
	
	area_x = mk_x * (MAX_X >> 1);
	area_y = mk_y * (MAX_Y >> 1);
	touch_major = max(area_x, area_y);
	touch_minor = min(area_x, area_y);
	orientation = area_x > area_y;
	
	td->coords[slot_id].x = x;
	td->coords[slot_id].y = y;

	if (is_release) {
		mod_timer(&td->release_timer[slot_id],
				  jiffies + msecs_to_jiffies(RELEASE_TIMEOUT));
		td->timers[slot_id] = true;
		return;
	} else if (is_touch && td->timers[slot_id]) {
		td->timers[slot_id] = false;
		del_timer(&td->release_timer[slot_id]);
	}
	
	num_contacts = data[8];

	if (num_contacts > 0)
		td->num_expected = num_contacts;
	td->num_received++;

	if (td->being_reported[slot_id])
		return;
	
	td->being_reported[slot_id] = true;
	
	error = input_mt_assign_slots(input, td->slots,
							td->coords, MAX_CONTACTS, DMAX);
	if (error)
		td->slots[slot_id] = slot_id;
	
	input_mt_slot(input, td->slots[slot_id]);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, is_touch);
	
	if (is_touch) {
		input_report_abs(input, ABS_MT_POSITION_X, x);
		input_report_abs(input, ABS_MT_POSITION_Y, y);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, touch_major);
		input_report_abs(input, ABS_MT_TOUCH_MINOR, touch_minor);
		input_report_abs(input, ABS_MT_ORIENTATION, orientation);
	}

	td->being_reported[slot_id] = false;

	if (td->num_received >= td->num_expected) {
		input_mt_drop_unused(input);
		input_mt_sync_frame(input);
		input_sync(input);
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

static int elan_start_multitouch(struct hid_device *hdev)
{
	struct hid_report *r;
	struct hid_report_enum *re;
	re = &(hdev->report_enum[HID_FEATURE_REPORT]);
	r = re->report_id_hash[3];
	if (r) {
		r->field[0]->value[0] = MT_INPUTMODE_TOUCHPAD;
		hid_hw_request(hdev, r, HID_REQ_SET_REPORT);
	}
	return 0;
}

static int __maybe_unused elan_reset_resume(struct hid_device *hdev)
{
	return elan_start_multitouch(hdev);
}

static int elan_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret, i;
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
	ret = elan_start_multitouch(hdev);
	if (ret)
		goto err_stop_hw;

	for(i = 0; i < MAX_CONTACTS; i++) {
		setup_timer(&drvdata->release_timer[i],
					elan_expired_timeout,
					(long)hdev);
		drvdata->timers[i] = false;
		drvdata->being_reported[i] = false;
	}
	return 0;

err_stop_hw:
	hid_hw_stop(hdev);
	return ret;
}


static void elan_remove(struct hid_device *hdev)
{
	int i;
	struct elan_drvdata *td = hid_get_drvdata(hdev);
	for(i = 0; i < MAX_CONTACTS; i++)
		del_timer_sync(&td->release_timer[i]);
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
	.input_mapping		= elan_input_mapping,
	.input_configured	= elan_input_configured,
#ifdef CONFIG_PM
	.reset_resume		= elan_reset_resume,
#endif
	.raw_event			= elan_raw_event
};


module_hid_driver(elan_driver);

MODULE_LICENSE("GPL");

