#include <linux/hid.h>
#include <linux/module.h>
#include <linux/input/mt.h>

MODULE_AUTHOR("Alexander Mishurov <ammishurov@gmail.com>");
MODULE_DESCRIPTION("Elan1200 TouchPad");

#define INPUT_REPORT_ID 0x04
#define INPUT_REPORT_SIZE 14

// the touchpad reports fake release events with the fifth contact
// drop it for the greater good
#define MAX_CONTACTS 4

#define MAX_X 3200
#define MAX_Y 2198
#define RESOLUTION 31
#define MAX_TOUCH_WIDTH 15

#define MT_INPUTMODE_TOUCHPAD 0x03
#define USB_VENDOR_ID_ELANTECH 0x04f3
#define USB_DEVICE_ID_ELAN1200_I2C_TOUCHPAD 0x3022


struct elan_drvdata {
	struct input_dev *input;
	int num_expected;
	int num_received;
};

static void elan_report_input(struct elan_drvdata *td, u8 *data)
{
	struct input_dev *input = td->input;
	struct input_mt *mt = input->mt;
	
	int x, y, mk_x, mk_y, num_contacts;

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
		return;

	x = (data[3] << 8) | data[2];
	y = (data[5] << 8) | data[4];

	mk_x = data[11] & 0x0f;
	mk_y = data[11] >> 4;

	// reduces random taps during two finger scrolls
	// 4 and 3 due to the width / height ratio
	if (mk_x < 4 || mk_y < 3)
		is_touch = false;

	input_mt_slot(input, slot_id);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, is_touch);
	
	input_report_abs(input, ABS_MT_POSITION_X, x);
	input_report_abs(input, ABS_MT_POSITION_Y, y);

	if (td->num_received >= td->num_expected) {
		input_mt_sync_frame(input);
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

