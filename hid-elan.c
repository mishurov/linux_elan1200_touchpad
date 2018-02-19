#include <linux/hid.h>
#include <linux/module.h>
#include <linux/input/mt.h>

MODULE_AUTHOR("Alexander Mishurov <ammishurov@gmail.com>");
MODULE_DESCRIPTION("Elan1200 TouchPad");

#define INPUT_REPORT_ID 0x04
#define INPUT_REPORT_SIZE 14

#define MAX_X 3200
#define MAX_Y 2198
#define RESOLUTION 31

#define MAX_CONTACTS 5

#define RELEASE_TIMEOUT 22
#define MAX_TIMESTAMP 1000000

#define INPUT_MODE_TOUCHPAD 0x03
#define LATENCY_MODE_NORMAL 0x00
#define USB_VENDOR_ID_ELANTECH 0x04f3
#define USB_DEVICE_ID_ELAN1200_I2C_TOUCHPAD 0x3022

struct slot {
	struct input_mt_pos coords;
	bool in_report;
	bool in_touch;
	bool delayed;
};

struct elan_drvdata {
	struct input_dev *input;
	struct hid_device *hdev;
	int num_expected;
	int num_received;
	int prev_time;
	int timestamp;
	struct timer_list release_timer;
	struct slot *slots;
	enum {
		INITIAL,
		TWO_IN_TOUCH,
		ONE_RELEASED,
		TWO_RELEASED
	} state;
	__s16 inputmode;
	__s16 inputmode_index;
	__s16 maxcontact_report_id; 
	__s16 latency_report_id;
	__s16 latency_index;
};

static void elan_report_contact(struct elan_drvdata *td,
				struct input_dev *input, int slot_id,
				bool in_touch)
{
	input_mt_slot(input, slot_id);
	input_mt_report_slot_state(input,
			MT_TOOL_FINGER, in_touch);

	if (in_touch) {
		input_report_abs(input, ABS_MT_POSITION_X,
					td->slots[slot_id].coords.x);
		input_report_abs(input, ABS_MT_POSITION_Y,
					td->slots[slot_id].coords.y);
	}
}

static void elan_release_contacts(struct hid_device *hdev)
{
	struct elan_drvdata *td = hid_get_drvdata(hdev);
	struct input_dev *input = td->input;
	int i;
	for (i = 0; i < MAX_CONTACTS; i++) {
		td->slots[i].in_touch = false;
		td->slots[i].in_report = false;
		elan_report_contact(td, input, i, false);
	}
	input_mt_sync_frame(input);
	input_sync(input);
	td->state = INITIAL;
}

static void elan_release_timeout(struct timer_list *t)
{
	struct elan_drvdata *td = from_timer(td, t, release_timer);
	struct hid_device *hdev = td->hdev;
	elan_release_contacts(hdev);
}

static void elan_report_contacts(struct elan_drvdata *td,
				struct input_dev *input)
{
	/*
	A workaround only for slot 0 and slot 1
	to prevent random events during two-finger scrolling.
	*/
	int i;

	if (td->num_received > 1 &&
	    td->slots[0].in_touch && td->slots[1].in_touch)
		td->state = TWO_IN_TOUCH;

	if (td->num_received > 2)
		td->state = INITIAL;

	switch (td->state) {
	case INITIAL:
		break;
	case TWO_IN_TOUCH:
		if (td->num_received > 1 &&
		    td->slots[0].in_touch != td->slots[1].in_touch) {
			td->state = ONE_RELEASED;
		}
		if (td->num_received > 1 &&
		    !td->slots[0].in_touch && !td->slots[1].in_touch) {
			td->state = TWO_RELEASED;
		}
		break;
	case ONE_RELEASED:
		if (td->num_received == 1 &&
		    !td->slots[0].in_touch && !td->slots[1].in_touch) {
			td->state = TWO_RELEASED;
		}
		break;
	case TWO_RELEASED:
		if (td->num_received == 1 &&
		    td->slots[0].in_touch != td->slots[1].in_touch) {
			td->state = ONE_RELEASED;
		}
		break;
	}

	for (i = 0; i < MAX_CONTACTS; i++) {
		if (td->slots[i].in_report && td->slots[i].delayed) {
			elan_report_contact(td, input, i, true);
		} else {
			elan_report_contact(
				td, input, i, td->slots[i].in_touch
			);
		}
	}

	input_mt_sync_frame(input);
	input_event(input, EV_MSC, MSC_TIMESTAMP, td->timestamp);
	input_sync(input);

	mod_timer(&td->release_timer,
		jiffies + msecs_to_jiffies(RELEASE_TIMEOUT));

	for (i = 0; i < MAX_CONTACTS; i++) {
		td->slots[i].delayed = false;
		td->slots[i].in_report = false;
	}
}

static void elan_report_input(struct elan_drvdata *td, u8 *data)
{
	struct input_dev *input = td->input;

	int num_contacts = data[8];
	int slot_id = data[1] >> 4;
	bool is_touch = (data[1] & 0x0f) == 3;
	bool is_release = (data[1] & 0x0f) == 1;
	int ts, delta;

	if (!(is_touch || is_release) ||
	    slot_id < 0 || slot_id >= MAX_CONTACTS)
		return;

	del_timer(&td->release_timer);

	td->slots[slot_id].in_report = true;
	
	td->slots[slot_id].in_touch = is_touch;

	//width = data[11] & 0x0f;
	//height = data[11] >> 4;

	ts = (data[7] << 8) + data[6];
	delta = ts - td->prev_time;
	if (delta <= 0) delta = 1;
	td->timestamp += delta;
	td->prev_time = ts;

	td->slots[slot_id].coords.x = (data[3] << 8) | data[2];
	td->slots[slot_id].coords.y = (data[5] << 8) | data[4];

	if (is_release && slot_id < 2 &&
	    (td->state == TWO_IN_TOUCH || td->state == ONE_RELEASED)) {
		td->slots[slot_id].delayed = true;
	}

	input_report_key(input, BTN_LEFT, data[9] & 0x01);

	if (num_contacts > MAX_CONTACTS)
		num_contacts = MAX_CONTACTS;

	if (num_contacts > 0)
		td->num_expected = num_contacts;

	td->num_received++;
}

static void elan_report(struct hid_device *hdev, struct hid_report *report)
{
	struct elan_drvdata *td = hid_get_drvdata(hdev);
	struct input_dev *input = td->input;
	struct hid_field *field = report->field[0];

	if (!(hdev->claimed & HID_CLAIMED_INPUT))
		return;

	if (field && field->hidinput && field->hidinput->input &&
	    td->num_received >= td->num_expected) {
		elan_report_contacts(td, input);
		td->num_received = 0;
		if (td->timestamp > MAX_TIMESTAMP)
			td->timestamp = 0;
	}
}

static int elan_raw_event(struct hid_device *hdev,
		struct hid_report *report, u8 *data, int size)
{
	struct elan_drvdata *td = hid_get_drvdata(hdev);

	if (data[0] == INPUT_REPORT_ID && size == INPUT_REPORT_SIZE) {
		elan_report_input(td, data);

		return 1;
	}
	return 0;
}

static int elan_event(struct hid_device *hdev, struct hid_field *field,
				struct hid_usage *usage, __s32 value)
{
	if (field->report->id == INPUT_REPORT_ID) {
		if (hdev->claimed & HID_CLAIMED_HIDDEV &&
		    hdev->hiddev_hid_event) {
			hdev->hiddev_hid_event(hdev, field, usage, value);
		}
		return 1;
	}
	return 0;
}

static int elan_input_configured(struct hid_device *hdev, struct hid_input *hi)
{
	struct input_dev *input = hi->input;
	struct elan_drvdata *td = hid_get_drvdata(hdev);

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

	td->input = input;

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

static void elan_set_latency(struct hid_device *hdev)
{
	struct elan_drvdata *td = hid_get_drvdata(hdev);
	struct hid_report *r;
	struct hid_report_enum *re;

	if (td->latency_report_id < 0)
		return;

	re = &(hdev->report_enum[HID_FEATURE_REPORT]);
	r = re->report_id_hash[td->latency_report_id];
	if (r) {
		r->field[0]->value[td->latency_index] = LATENCY_MODE_NORMAL;
		hid_hw_request(hdev, r, HID_REQ_SET_REPORT);
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
		r->field[0]->value[td->inputmode_index] = INPUT_MODE_TOUCHPAD;
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
	case 0xd0060:
		// latency mode usage (Page 0x0D, Usage 0x60)
		td->latency_report_id = field->report->id;
		td->latency_index = usage->usage_index;
		break;
	default:
		if (usage->usage_index == 0) {
			elan_get_feature(hdev, field->report);
		}
	}
}

#ifdef CONFIG_PM
static int elan_reset_resume(struct hid_device *hdev)
{
	elan_release_contacts(hdev);
	elan_set_latency(hdev);
	elan_set_maxcontacts(hdev);
	elan_set_input_mode(hdev);
	return 0;
}

static int elan_resume(struct hid_device *hdev)
{
	hid_hw_idle(hdev, 0, 0, HID_REQ_SET_IDLE);
	return 0;
}
#endif

static int elan_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret, i;
	struct elan_drvdata *td;

	td = devm_kzalloc(&hdev->dev, sizeof(*td), GFP_KERNEL);
	if (td == NULL) {
		hid_err(hdev, "Can't alloc Elan descriptor\n");
		return -ENOMEM;
	}

	td->inputmode = -1;
	td->inputmode_index = -1;
	td->maxcontact_report_id = -1; 
	td->latency_report_id = -1;
	td->latency_index = -1;

	td->slots = devm_kmalloc_array(&hdev->dev, MAX_CONTACTS,
					sizeof(struct slot),
					GFP_KERNEL);
	if (td == NULL) {
		hid_err(hdev, "Can't alloc slots data\n");
		return -ENOMEM;
	}

	for (i = 0; i < MAX_CONTACTS; i++) {
		td->slots[i].delayed = false;
		td->slots[i].in_report = false;
		td->slots[i].in_touch = false;
		td->slots[i].coords.x = 0;
		td->slots[i].coords.y = 0;
	}

	td->state = INITIAL;
	timer_setup(&td->release_timer, elan_release_timeout, 0);

	hid_set_drvdata(hdev, td);

	td->hdev = hdev;

	hdev->quirks |= HID_QUIRK_NO_INPUT_SYNC;
	hdev->quirks |= HID_QUIRK_NO_EMPTY_INPUT;
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

	if (!td->input) {
		hid_err(hdev, "Elan input not registered\n");
		ret = -ENOMEM;
		goto err_stop_hw;
	}

	td->input->name = "Elan TouchPad";

	elan_set_latency(hdev);
	elan_set_maxcontacts(hdev);
	elan_set_input_mode(hdev);

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

	int code;
	if (field->application != HID_DG_TOUCHSCREEN &&
	    field->application != HID_DG_PEN &&
	    field->application != HID_DG_TOUCHPAD &&
	    field->application != HID_GD_KEYBOARD &&
	    field->application != HID_CP_CONSUMER_CONTROL)
		return -1;

	if (field->physical == HID_DG_STYLUS)
		return 0;
	else if (field->physical == 0 &&
		 field->report->id != INPUT_REPORT_ID)
		return 0;

	if (field->application == HID_DG_TOUCHSCREEN ||
	    field->application == HID_DG_TOUCHPAD) {
		switch (usage->hid & HID_USAGE_PAGE) {
		case HID_UP_GENDESK:
			switch (usage->hid) {
			case HID_GD_X:
				return 1;
			case HID_GD_Y:
				return 1;
			}
			return 0;
		case HID_UP_DIGITIZER:
			switch (usage->hid) {
			case HID_DG_INRANGE:
				return 1;
			case HID_DG_CONFIDENCE:
				return 1;
			case HID_DG_TIPSWITCH:
				return 1;
			case HID_DG_CONTACTID:
				return 1;
			case HID_DG_CONTACTCOUNT:
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
			return 1;
		case 0xff000000:
			return -1;
		}
	}
	return 0;
}

static int elan_input_mapped(struct hid_device *hdev,
		struct hid_input *hi, struct hid_field *field,
		struct hid_usage *usage, unsigned long **bit,
		int *max)
{
	if (field->physical == HID_DG_STYLUS)
		return 0;

	if (field->application == HID_DG_TOUCHSCREEN ||
	    field->application == HID_DG_TOUCHPAD) {
		if (usage->type == EV_KEY || usage->type == EV_ABS)
			set_bit(usage->type, hi->input->evbit);
		return -1;
	}
	return 0;
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
	.name				= "hid-elan",
	.id_table			= elan_devices,
	.probe 				= elan_probe,
	.remove				= elan_remove,
	.input_mapping			= elan_input_mapping,
	.input_mapped			= elan_input_mapped,
	.input_configured		= elan_input_configured,
	.feature_mapping 		= elan_feature_mapping,
	.usage_table			= elan_grabbed_usages,
	.raw_event			= elan_raw_event,
	.event				= elan_event,
	.report				= elan_report,
#ifdef CONFIG_PM
	.reset_resume			= elan_reset_resume,
	.resume 			= elan_resume,
#endif
};

module_hid_driver(elan_driver);

MODULE_LICENSE("GPL");

