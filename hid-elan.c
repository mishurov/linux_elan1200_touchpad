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
#define MAX_TOUCH_WIDTH 22
#define RESOLUTION 31

#define MT_INPUTMODE_TOUCHPAD 0x03
#define USB_VENDOR_ID_ELANTECH 0x04f3
#define USB_DEVICE_ID_ELAN1200_I2C_TOUCHPAD 0x3022

#define TRKID_SGN ((TRKID_MAX + 1) >> 1)


struct elan_drvdata {
	struct input_dev *input;
};

static void elan_report_tool_width(struct input_dev *input)
{
	struct input_mt *mt = input->mt;
	struct input_mt_slot *oldest;
	int oldid, count, i;

	oldest = NULL;
	oldid = mt->trkid;
	count = 0;

	for (i = 0; i < mt->num_slots; ++i) {
		struct input_mt_slot *ps = &mt->slots[i];
		int id = input_mt_get_value(ps, ABS_MT_TRACKING_ID);

		if (id < 0)
			continue;
		if ((id - oldid) & TRKID_SGN) {
			oldest = ps;
			oldid = id;
		}
		count++;
	}

	if (oldest) {
		input_report_abs(input, ABS_TOOL_WIDTH,
			input_mt_get_value(oldest, ABS_MT_TOUCH_MAJOR));
	}
}

static void elan_report_input(struct input_dev *input, u8 *data)
{
	int x, y, width, height, touch_major, touch_minor;
	bool orientation;
	int toolType = (data[9] & 0x0f) < 9 ? MT_TOOL_FINGER : MT_TOOL_PALM;

	int slot = data[1] >> 4;
	bool is_touch = (data[1] & 0x0f) == 3;
	bool is_release = (data[1] & 0x0f) == 1;
	// the touchpad sometimes generates a fake report
	bool is_fake = (data[1] == 0x41 && data[2] == 0xc6);

	if (is_fake || !(is_touch || is_release)) return;
	
	input_mt_slot(input, slot);
	input_mt_report_slot_state(input, toolType, is_touch);

	x = (data[3] << 8) | data[2];
	y = (data[5] << 8) | data[4];

	width = data[11] & 0x0f;
	height = data[11] >> 4;

	touch_major = int_sqrt(width * width + height * height);
	touch_minor = min(width, height);
	orientation = width > height;

	input_report_abs(input, ABS_MT_POSITION_X, x);
	input_report_abs(input, ABS_MT_POSITION_Y, y);

	if (is_touch) {
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, touch_major);
		input_report_abs(input, ABS_MT_TOUCH_MINOR, touch_minor);
		input_report_abs(input, ABS_MT_ORIENTATION, orientation);
	}

	input_report_key(input, BTN_TOUCH, is_touch);

	// the first slot always reports contacts count
	if (is_touch && slot == 0) {
		int contacts = data[8];
		input_event(input, EV_KEY, BTN_TOOL_FINGER, contacts == 1);
		input_event(input, EV_KEY, BTN_TOOL_DOUBLETAP, contacts == 2);
		input_event(input, EV_KEY, BTN_TOOL_TRIPLETAP, contacts == 3);
		input_event(input, EV_KEY, BTN_TOOL_QUADTAP, contacts == 4);
		input_event(input, EV_KEY, BTN_TOOL_QUINTTAP, contacts == 5);
	}

	elan_report_tool_width(input);
	
	input_report_key(input, BTN_LEFT, data[9] & 0x01);
	input_mt_sync_frame(input);
	input_sync(input);
}

static int elan_raw_event(struct hid_device *hdev,
		struct hid_report *report, u8 *data, int size)
{
	struct elan_drvdata *drvdata = hid_get_drvdata(hdev);

	if (data[0] == INPUT_REPORT_ID && size == INPUT_REPORT_SIZE) {
		elan_report_input(drvdata->input, data);
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
	input_set_abs_params(input, ABS_TOOL_WIDTH, 0, MAX_TOUCH_WIDTH, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, MAX_TOUCH_WIDTH, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MINOR, 0, MAX_TOUCH_WIDTH, 0, 0);
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
	ret = elan_start_multitouch(hdev);
	if (ret)
		goto err_stop_hw;

	return 0;

err_stop_hw:
	hid_hw_stop(hdev);
	return ret;
}


static void elan_remove(struct hid_device *hdev)
{
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

