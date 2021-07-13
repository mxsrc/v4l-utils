// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2016 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 */

#include <cstring>
#include <map>
#include <sstream>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

#include "cec-compliance.h"

enum Months { Jan = 1, Feb, Mar, Apr, May, Jun, Jul, Aug, Sep, Oct, Nov, Dec };

struct remote_test {
	const char *name;
	const unsigned tags;
	const vec_remote_subtests &subtests;
};

static int deck_status_get(struct node *node, unsigned me, unsigned la, __u8 &deck_status)
{
	struct cec_msg msg;
	deck_status = 0;

	cec_msg_init(&msg, me, la);
	cec_msg_give_deck_status(&msg, true, CEC_OP_STATUS_REQ_ONCE);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(timed_out_or_abort(&msg));
	cec_ops_deck_status(&msg, &deck_status);

	return OK;
}

static int test_play_mode(struct node *node, unsigned me, unsigned la, __u8 play_mode, __u8 expected)
{
	struct cec_msg msg;
	__u8 deck_status;

	cec_msg_init(&msg, me, la);
	cec_msg_play(&msg, play_mode);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(cec_msg_status_is_abort(&msg)); /* Assumes deck has media. */
	fail_on_test(deck_status_get(node, me, la, deck_status));
	fail_on_test(deck_status != expected);

	return OK;
}

static int one_touch_rec_on_send(struct node *node, unsigned me, unsigned la,
                                 const struct cec_op_record_src &rec_src, __u8 &rec_status)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_record_off(&msg, false);
	fail_on_test(!transmit_timeout(node, &msg));

	cec_msg_init(&msg, me, la);
	cec_msg_record_on(&msg, true, &rec_src);
	/* Allow 10s for reply because the spec says it may take several seconds to accurately respond. */
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out_or_abort(&msg));
	cec_ops_record_status(&msg, &rec_status);

	return OK;
}

static int one_touch_rec_on_send_invalid(struct node *node, unsigned me, unsigned la,
                                         const struct cec_op_record_src &rec_src)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_record_on(&msg, true, &rec_src);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);

	return OK;
}

/*
 * Returns true if the Record Status is an error indicating that the
 * request to start recording has failed.
 */
static bool rec_status_is_a_valid_error_status(__u8 rec_status)
{
	switch (rec_status) {
	case CEC_OP_RECORD_STATUS_NO_DIG_SERVICE:
	case CEC_OP_RECORD_STATUS_NO_ANA_SERVICE:
	case CEC_OP_RECORD_STATUS_NO_SERVICE:
	case CEC_OP_RECORD_STATUS_INVALID_EXT_PLUG:
	case CEC_OP_RECORD_STATUS_INVALID_EXT_PHYS_ADDR:
	case CEC_OP_RECORD_STATUS_UNSUP_CA:
	case CEC_OP_RECORD_STATUS_NO_CA_ENTITLEMENTS:
	case CEC_OP_RECORD_STATUS_CANT_COPY_SRC:
	case CEC_OP_RECORD_STATUS_NO_MORE_COPIES:
	case CEC_OP_RECORD_STATUS_NO_MEDIA:
	case CEC_OP_RECORD_STATUS_PLAYING:
	case CEC_OP_RECORD_STATUS_ALREADY_RECORDING:
	case CEC_OP_RECORD_STATUS_MEDIA_PROT:
	case CEC_OP_RECORD_STATUS_NO_SIGNAL:
	case CEC_OP_RECORD_STATUS_MEDIA_PROBLEM:
	case CEC_OP_RECORD_STATUS_NO_SPACE:
	case CEC_OP_RECORD_STATUS_PARENTAL_LOCK:
	case CEC_OP_RECORD_STATUS_OTHER:
		return true;
	default:
		return false;
	}
}

static int timer_status_is_valid(const struct cec_msg &msg)
{
	__u8 timer_overlap_warning;
	__u8 media_info;
	__u8 prog_info;
	__u8 prog_error;
	__u8 duration_hr;
	__u8 duration_min;

	cec_ops_timer_status(&msg, &timer_overlap_warning, &media_info, &prog_info,
	                     &prog_error, &duration_hr, &duration_min);
	fail_on_test(media_info > CEC_OP_MEDIA_INFO_NO_MEDIA);
	if (prog_info)
		fail_on_test(prog_info < CEC_OP_PROG_INFO_ENOUGH_SPACE ||
		             prog_info > CEC_OP_PROG_INFO_MIGHT_NOT_BE_ENOUGH_SPACE);
	else
		fail_on_test(prog_error < CEC_OP_PROG_ERROR_NO_FREE_TIMER ||
		             (prog_error > CEC_OP_PROG_ERROR_CLOCK_FAILURE &&
		              prog_error != CEC_OP_PROG_ERROR_DUPLICATE));

	return OK;
}

static int timer_cleared_status_is_valid(const struct cec_msg &msg)
{
	__u8 timer_cleared_status;

	cec_ops_timer_cleared_status(&msg, &timer_cleared_status);
	fail_on_test(timer_cleared_status != CEC_OP_TIMER_CLR_STAT_RECORDING &&
	             timer_cleared_status != CEC_OP_TIMER_CLR_STAT_NO_MATCHING &&
	             timer_cleared_status != CEC_OP_TIMER_CLR_STAT_NO_INFO &&
	             timer_cleared_status != CEC_OP_TIMER_CLR_STAT_CLEARED);

	return OK;
}

static bool timer_has_error(const struct cec_msg &msg)
{
	__u8 timer_overlap_warning;
	__u8 media_info;
	__u8 prog_info;
	__u8 prog_error;
	__u8 duration_hr;
	__u8 duration_min;

	cec_ops_timer_status(&msg, &timer_overlap_warning, &media_info, &prog_info,
	                     &prog_error, &duration_hr, &duration_min);
	if (prog_error)
		return true;

	return false;
}

static int send_timer_error(struct node *node, unsigned me, unsigned la, __u8 day, __u8 month,
                            __u8 start_hr, __u8 start_min, __u8 dur_hr, __u8 dur_min, __u8 rec_seq)
{
	struct cec_msg msg;
	cec_msg_init(&msg, me, la);
	cec_msg_set_analogue_timer(&msg, true, day, month, start_hr, start_min, dur_hr, dur_min,
	                           rec_seq, CEC_OP_ANA_BCAST_TYPE_CABLE, 7668, // 479.25 MHz
	                           node->remote[la].bcast_sys);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out(&msg));
	if (cec_msg_status_is_abort(&msg))
		fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);
	else
		fail_on_test(!timer_has_error(msg));

	return OK;
}

static bool timer_overlap_warning_is_set(const struct cec_msg &msg)
{
	__u8 timer_overlap_warning;
	__u8 media_info;
	__u8 prog_info;
	__u8 prog_error;
	__u8 duration_hr;
	__u8 duration_min;

	cec_ops_timer_status(&msg, &timer_overlap_warning, &media_info, &prog_info,
	                     &prog_error, &duration_hr, &duration_min);

	if (timer_overlap_warning)
		return true;

	return false;
}

static int send_timer_overlap(struct node *node, unsigned me, unsigned la, __u8 day, __u8 month,
                              __u8 start_hr, __u8 start_min, __u8 dur_hr, __u8 dur_min, __u8 rec_seq)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_set_analogue_timer(&msg, true, day, month, start_hr, start_min, dur_hr, dur_min,
	                           rec_seq, CEC_OP_ANA_BCAST_TYPE_CABLE, 7668, // 479.25 MHz
	                           node->remote[la].bcast_sys);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out_or_abort(&msg));
	fail_on_test(timer_has_error(msg));
	fail_on_test(!timer_overlap_warning_is_set(msg));

	return OK;
}

static int clear_timer(struct node *node, unsigned me, unsigned la, __u8 day, __u8 month,
                       __u8 start_hr, __u8 start_min, __u8 dur_hr, __u8 dur_min, __u8 rec_seq)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_clear_analogue_timer(&msg, true, day, month, start_hr, start_min, dur_hr, dur_min,
	                             rec_seq, CEC_OP_ANA_BCAST_TYPE_CABLE, 7668, // 479.25 MHz
	                             node->remote[la].bcast_sys);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out_or_abort(&msg));
	fail_on_test(timer_has_error(msg));
	fail_on_test(timer_cleared_status_is_valid(msg));

	return OK;
}

/* System Information */

int system_info_polling(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	fail_on_test(doioctl(node, CEC_TRANSMIT, &msg));
	if (node->remote_la_mask & (1 << la)) {
		if (!cec_msg_status_is_ok(&msg)) {
			fail("Polling a valid remote LA failed\n");
			return FAIL_CRITICAL;
		}
	} else {
		if (cec_msg_status_is_ok(&msg)) {
			fail("Polling an invalid remote LA was successful\n");
			return FAIL_CRITICAL;
		}
		return OK_NOT_SUPPORTED;
	}

	return 0;
}

int system_info_phys_addr(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_give_physical_addr(&msg, true);
	if (!transmit_timeout(node, &msg) || timed_out_or_abort(&msg)) {
		fail_or_warn(node, "Give Physical Addr timed out\n");
		return node->in_standby ? 0 : FAIL_CRITICAL;
	}
	fail_on_test(node->remote[la].phys_addr != ((msg.msg[2] << 8) | msg.msg[3]));
	fail_on_test(node->remote[la].prim_type != msg.msg[4]);
	return 0;
}

int system_info_version(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_get_cec_version(&msg, true);
	if (!transmit_timeout(node, &msg) || timed_out(&msg))
		return fail_or_warn(node, "Get CEC Version timed out\n");
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;

	/* This needs to be kept in sync with newer CEC versions */
	fail_on_test(msg.msg[2] < CEC_OP_CEC_VERSION_1_3A ||
		     msg.msg[2] > CEC_OP_CEC_VERSION_2_0);
	fail_on_test(node->remote[la].cec_version != msg.msg[2]);

	return 0;
}

int system_info_get_menu_lang(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	char language[4];

	cec_msg_init(&msg, me, la);
	cec_msg_get_menu_language(&msg, true);
	if (!transmit_timeout(node, &msg) || timed_out(&msg))
		return fail_or_warn(node, "Get Menu Languages timed out\n");

	/* Devices other than TVs shall send Feature Abort [Unregcognized Opcode]
	   in reply to Get Menu Language. */
	fail_on_test(!is_tv(la, node->remote[la].prim_type) && !unrecognized_op(&msg));

	if (unrecognized_op(&msg)) {
		if (is_tv(la, node->remote[la].prim_type))
			warn("TV did not respond to Get Menu Language.\n");
		return OK_NOT_SUPPORTED;
	}
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	cec_ops_set_menu_language(&msg, language);
	fail_on_test(strcmp(node->remote[la].language, language));

	return 0;
}

static int system_info_set_menu_lang(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_set_menu_language(&msg, "eng");
	fail_on_test(!transmit_timeout(node, &msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;

	return OK_PRESUMED;
}

int system_info_give_features(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_give_features(&msg, true);
	if (!transmit_timeout(node, &msg) || timed_out(&msg))
		return fail_or_warn(node, "Give Features timed out\n");
	if (unrecognized_op(&msg)) {
		if (node->remote[la].cec_version < CEC_OP_CEC_VERSION_2_0)
			return OK_NOT_SUPPORTED;
		fail_on_test_v2(node->remote[la].cec_version, true);
	}
	if (refused(&msg))
		return OK_REFUSED;
	if (node->remote[la].cec_version < CEC_OP_CEC_VERSION_2_0)
		info("Device has CEC Version < 2.0 but supports Give Features.\n");

	/* RC Profile and Device Features are assumed to be 1 byte. As of CEC 2.0 only
	   1 byte is used, but this might be extended in future versions. */
	__u8 cec_version, all_device_types;
	const __u8 *rc_profile, *dev_features;

	cec_ops_report_features(&msg, &cec_version, &all_device_types, &rc_profile, &dev_features);
	fail_on_test(rc_profile == nullptr || dev_features == nullptr);
	info("All Device Types: \t\t%s\n", cec_all_dev_types2s(all_device_types).c_str());
	info("RC Profile: \t%s", cec_rc_src_prof2s(*rc_profile, "").c_str());
	info("Device Features: \t%s", cec_dev_feat2s(*dev_features, "").c_str());

	if (!(cec_has_playback(1 << la) || cec_has_record(1 << la) || cec_has_tuner(1 << la)) &&
	    (*dev_features & CEC_OP_FEAT_DEV_HAS_SET_AUDIO_RATE)) {
		return fail("Only Playback, Recording or Tuner devices shall set the Set Audio Rate bit\n");
	}
	if (!(cec_has_playback(1 << la) || cec_has_record(1 << la)) &&
	    (*dev_features & CEC_OP_FEAT_DEV_HAS_DECK_CONTROL))
		return fail("Only Playback and Recording devices shall set the Supports Deck Control bit\n");
	if (!cec_has_tv(1 << la) && node->remote[la].has_rec_tv)
		return fail("Only TVs shall set the Record TV Screen bit\n");
	if (cec_has_playback(1 << la) && (*dev_features & CEC_OP_FEAT_DEV_SINK_HAS_ARC_TX))
		return fail("A Playback device cannot set the Sink Supports ARC Tx bit\n");
	if (cec_has_tv(1 << la) && (*dev_features & CEC_OP_FEAT_DEV_SOURCE_HAS_ARC_RX))
		return fail("A TV cannot set the Source Supports ARC Rx bit\n");

	fail_on_test(cec_version != node->remote[la].cec_version);
	fail_on_test(node->remote[la].rc_profile != *rc_profile);
	fail_on_test(node->remote[la].dev_features != *dev_features);
	fail_on_test(node->remote[la].all_device_types != all_device_types);
	return 0;
}

static const vec_remote_subtests system_info_subtests{
	{ "Polling Message", CEC_LOG_ADDR_MASK_ALL, system_info_polling },
	{ "Give Physical Address", CEC_LOG_ADDR_MASK_ALL, system_info_phys_addr },
	{ "Give CEC Version", CEC_LOG_ADDR_MASK_ALL, system_info_version },
	{ "Get Menu Language", CEC_LOG_ADDR_MASK_ALL, system_info_get_menu_lang },
	{ "Set Menu Language", CEC_LOG_ADDR_MASK_ALL, system_info_set_menu_lang },
	{ "Give Device Features", CEC_LOG_ADDR_MASK_ALL, system_info_give_features },
};

/* Core behavior */

int core_unknown(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	const __u8 unknown_opcode = 0xfe;

	/* Unknown opcodes should be responded to with Feature Abort, with abort
	   reason Unknown Opcode.

	   For CEC 2.0 and before, 0xfe is an unused opcode. The test possibly
	   needs to be updated for future CEC versions. */
	cec_msg_init(&msg, me, la);
	msg.len = 2;
	msg.msg[1] = unknown_opcode;
	if (!transmit_timeout(node, &msg) || timed_out(&msg))
		return fail_or_warn(node, "Unknown Opcode timed out\n");
	fail_on_test(!cec_msg_status_is_abort(&msg));

	__u8 abort_msg, reason;

	cec_ops_feature_abort(&msg, &abort_msg, &reason);
	fail_on_test(reason != CEC_OP_ABORT_UNRECOGNIZED_OP);
	fail_on_test(abort_msg != 0xfe);

	/* Unknown opcodes that are broadcast should be ignored */
	cec_msg_init(&msg, me, CEC_LOG_ADDR_BROADCAST);
	msg.len = 2;
	msg.msg[1] = unknown_opcode;
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!timed_out(&msg));

	return 0;
}

int core_abort(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	/* The Abort message should always be responded to with Feature Abort
	   (with any abort reason) */
	cec_msg_init(&msg, me, la);
	cec_msg_abort(&msg);
	if (!transmit_timeout(node, &msg) || timed_out(&msg))
		return fail_or_warn(node, "Abort timed out\n");
	fail_on_test(!cec_msg_status_is_abort(&msg));
	return 0;
}

static const vec_remote_subtests core_subtests{
	{ "Feature aborts unknown messages", CEC_LOG_ADDR_MASK_ALL, core_unknown },
	{ "Feature aborts Abort message", CEC_LOG_ADDR_MASK_ALL, core_abort },
};

/* Vendor Specific Commands */

int vendor_specific_commands_id(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_give_device_vendor_id(&msg, true);
	if (!transmit(node, &msg))
		return fail_or_warn(node, "Give Device Vendor ID timed out\n");
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	fail_on_test(node->remote[la].vendor_id !=
		     (__u32)((msg.msg[2] << 16) | (msg.msg[3] << 8) | msg.msg[4]));

	return 0;
}

static const vec_remote_subtests vendor_specific_subtests{
	{ "Give Device Vendor ID", CEC_LOG_ADDR_MASK_ALL, vendor_specific_commands_id },
};

/* Device OSD Transfer */

static int device_osd_transfer_set(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_set_osd_name(&msg, "Whatever");
	fail_on_test(!transmit_timeout(node, &msg));
	if (unrecognized_op(&msg)) {
		if (is_tv(la, node->remote[la].prim_type) &&
		    node->remote[la].cec_version >= CEC_OP_CEC_VERSION_2_0)
			warn("TV feature aborted Set OSD Name\n");
		return OK_NOT_SUPPORTED;
	}
	if (refused(&msg))
		return OK_REFUSED;

	return OK_PRESUMED;
}

int device_osd_transfer_give(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	/* Todo: CEC 2.0: devices with several logical addresses shall report
	   the same for each logical address. */
	cec_msg_init(&msg, me, la);
	cec_msg_give_osd_name(&msg, true);
	if (!transmit_timeout(node, &msg) || timed_out(&msg))
		return fail_or_warn(node, "Give OSD Name timed out\n");
	fail_on_test(!is_tv(la, node->remote[la].prim_type) && unrecognized_op(&msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	char osd_name[15];
	cec_ops_set_osd_name(&msg, osd_name);
	fail_on_test(!osd_name[0]);
	fail_on_test(strcmp(node->remote[la].osd_name, osd_name));
	fail_on_test(msg.len != strlen(osd_name) + 2);

	return 0;
}

static const vec_remote_subtests device_osd_transfer_subtests{
	{ "Set OSD Name", CEC_LOG_ADDR_MASK_ALL, device_osd_transfer_set },
	{ "Give OSD Name", CEC_LOG_ADDR_MASK_ALL, device_osd_transfer_give },
};

/* OSD Display */

static int osd_string_set_default(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	char osd[14];
	bool unsuitable = false;

	sprintf(osd, "Rept %x from %x", la, me);

	interactive_info(true, "You should see \"%s\" appear on the screen", osd);
	cec_msg_init(&msg, me, la);
	cec_msg_set_osd_string(&msg, CEC_OP_DISP_CTL_DEFAULT, osd);
	fail_on_test(!transmit_timeout(node, &msg));
	/* In CEC 2.0 it is mandatory for a TV to support this if it reports so
	   in its Device Features. */
	fail_on_test_v2(node->remote[la].cec_version,
			unrecognized_op(&msg) &&
			(node->remote[la].dev_features & CEC_OP_FEAT_DEV_HAS_SET_OSD_STRING));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg)) {
		warn("The device is in an unsuitable state or cannot display the complete message.\n");
		unsuitable = true;
	}
	node->remote[la].has_osd = true;
	if (!interactive)
		return OK_PRESUMED;

	/* The CEC 1.4b CTS specifies that one should wait at least 20 seconds for the
	   string to be cleared on the remote device */
	interactive_info(true, "Waiting 20s for OSD string to be cleared on the remote device");
	sleep(20);
	fail_on_test(!unsuitable && interactive && !question("Did the string appear and then disappear?"));

	return 0;
}

static int osd_string_set_until_clear(struct node *node, unsigned me, unsigned la, bool interactive)
{
	if (!node->remote[la].has_osd)
		return NOTAPPLICABLE;

	struct cec_msg msg;
	char osd[14];
	bool unsuitable = false;

	strcpy(osd, "Appears 1 sec");
	// Make sure the string is the maximum possible length
	fail_on_test(strlen(osd) != 13);

	interactive_info(true, "You should see \"%s\" appear on the screen for approximately three seconds.", osd);
	cec_msg_init(&msg, me, la);
	cec_msg_set_osd_string(&msg, CEC_OP_DISP_CTL_UNTIL_CLEARED, osd);
	fail_on_test(!transmit(node, &msg));
	if (cec_msg_status_is_abort(&msg) && !unrecognized_op(&msg)) {
		warn("The device is in an unsuitable state or cannot display the complete message.\n");
		unsuitable = true;
	}
	sleep(3);

	cec_msg_init(&msg, me, la);
	cec_msg_set_osd_string(&msg, CEC_OP_DISP_CTL_CLEAR, "");
	fail_on_test(!transmit_timeout(node, &msg, 250));
	fail_on_test(cec_msg_status_is_abort(&msg));
	fail_on_test(!unsuitable && interactive && !question("Did the string appear?"));

	if (interactive)
		return 0;

	return OK_PRESUMED;
}

static int osd_string_invalid(struct node *node, unsigned me, unsigned la, bool interactive)
{
	if (!node->remote[la].has_osd)
		return NOTAPPLICABLE;

	struct cec_msg msg;

	/* Send Set OSD String with an Display Control operand. A Feature Abort is
	   expected in reply. */
	interactive_info(true, "You should observe no change on the on screen display");
	cec_msg_init(&msg, me, la);
	cec_msg_set_osd_string(&msg, 0xff, "");
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(timed_out(&msg));
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(interactive && question("Did the display change?"));

	return 0;
}

static const vec_remote_subtests osd_string_subtests{
	{ "Set OSD String with default timeout", CEC_LOG_ADDR_MASK_TV, osd_string_set_default },
	{ "Set OSD String with no timeout", CEC_LOG_ADDR_MASK_TV, osd_string_set_until_clear },
	{ "Set OSD String with invalid operand", CEC_LOG_ADDR_MASK_TV, osd_string_invalid },
};

/* Routing Control */

static int routing_control_inactive_source(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	int response;

	interactive_info(true, "Please make sure that the TV is currently viewing this source.");
	mode_set_follower(node);
	cec_msg_init(&msg, me, la);
	cec_msg_inactive_source(&msg, node->phys_addr);
	fail_on_test(!transmit(node, &msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	// It may take a bit of time for the Inactive Source message to take
	// effect, so sleep a bit.
	response = util_receive(node, CEC_LOG_ADDR_TV, 3000, &msg,
				CEC_MSG_INACTIVE_SOURCE,
				CEC_MSG_ACTIVE_SOURCE, CEC_MSG_SET_STREAM_PATH);
	if (me == CEC_LOG_ADDR_TV) {
		// Inactive Source should be ignored by all other devices
		if (response >= 0)
			return fail("Unexpected reply to Inactive Source\n");
		fail_on_test(response >= 0);
	} else {
		if (response < 0)
			warn("Expected Active Source or Set Stream Path reply to Inactive Source\n");
		fail_on_test(interactive && !question("Did the TV switch away from or stop showing this source?"));
	}

	return 0;
}

static int routing_control_active_source(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	interactive_info(true, "Please switch the TV to another source.");
	cec_msg_init(&msg, me, la);
	cec_msg_active_source(&msg, node->phys_addr);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(interactive && !question("Did the TV switch to this source?"));

	if (interactive)
		return 0;

	return OK_PRESUMED;
}

static int routing_control_req_active_source(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	/* We have now said that we are active source, so receiving a reply to
	   Request Active Source should fail the test. */
	cec_msg_init(&msg, me, la);
	cec_msg_request_active_source(&msg, true);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!timed_out(&msg));

	return 0;
}

static int routing_control_set_stream_path(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	__u16 phys_addr;

	/* Send Set Stream Path with the remote physical address. We expect the
	   source to eventually send Active Source. The timeout of long_timeout
	   seconds is necessary because the device might have to wake up from standby.

	   In CEC 2.0 it is mandatory for sources to send Active Source. */
	if (is_tv(la, node->remote[la].prim_type))
		interactive_info(true, "Please ensure that the device is in standby.");
	announce("Sending Set Stream Path and waiting for reply. This may take up to %llu s.", (long long)long_timeout);
	cec_msg_init(&msg, me, la);
	cec_msg_set_stream_path(&msg, node->remote[la].phys_addr);
	msg.reply = CEC_MSG_ACTIVE_SOURCE;
	fail_on_test(!transmit_timeout(node, &msg, long_timeout * 1000));
	if (timed_out(&msg) && is_tv(la, node->remote[la].prim_type))
		return OK_NOT_SUPPORTED;
	if (timed_out(&msg) && node->remote[la].cec_version < CEC_OP_CEC_VERSION_2_0) {
		warn("Device did not respond to Set Stream Path.\n");
		return OK_NOT_SUPPORTED;
	}
	fail_on_test_v2(node->remote[la].cec_version, timed_out(&msg));
	cec_ops_active_source(&msg, &phys_addr);
	fail_on_test(phys_addr != node->remote[la].phys_addr);
	if (is_tv(la, node->remote[la].prim_type))
		fail_on_test(interactive && !question("Did the device go out of standby?"));

	if (interactive || node->remote[la].cec_version >= CEC_OP_CEC_VERSION_2_0)
		return 0;

	return OK_PRESUMED;
}

static const vec_remote_subtests routing_control_subtests{
	{ "Active Source", CEC_LOG_ADDR_MASK_TV, routing_control_active_source },
	{ "Request Active Source", CEC_LOG_ADDR_MASK_ALL, routing_control_req_active_source },
	{ "Inactive Source", CEC_LOG_ADDR_MASK_TV, routing_control_inactive_source },
	{ "Set Stream Path", CEC_LOG_ADDR_MASK_ALL, routing_control_set_stream_path },
};

/* Remote Control Passthrough */

static int rc_passthrough_user_ctrl_pressed(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	struct cec_op_ui_command rc_press;

	cec_msg_init(&msg, me, la);
	rc_press.ui_cmd = CEC_OP_UI_CMD_VOLUME_UP; // Volume up key (the key is not crucial here)
	cec_msg_user_control_pressed(&msg, &rc_press);
	fail_on_test(!transmit_timeout(node, &msg));
	/* Mandatory for all except devices which have taken logical address 15 */
	fail_on_test_v2(node->remote[la].cec_version,
			unrecognized_op(&msg) && !(cec_is_unregistered(1 << la)));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;

	return OK_PRESUMED;
}

static int rc_passthrough_user_ctrl_released(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_user_control_released(&msg);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test_v2(node->remote[la].cec_version,
			cec_msg_status_is_abort(&msg) && !(la & CEC_LOG_ADDR_MASK_UNREGISTERED));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	node->remote[la].has_remote_control_passthrough = true;

	return OK_PRESUMED;
}

static const vec_remote_subtests rc_passthrough_subtests{
	{ "User Control Pressed", CEC_LOG_ADDR_MASK_ALL, rc_passthrough_user_ctrl_pressed },
	{ "User Control Released", CEC_LOG_ADDR_MASK_ALL, rc_passthrough_user_ctrl_released },
};

/* Device Menu Control */

/*
  TODO: These are very rudimentary tests which should be expanded.
 */

static int dev_menu_ctl_request(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_menu_request(&msg, true, CEC_OP_MENU_REQUEST_QUERY);
	fail_on_test(!transmit_timeout(node, &msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	if (node->remote[la].cec_version >= CEC_OP_CEC_VERSION_2_0)
		warn("The Device Menu Control feature is deprecated in CEC 2.0\n");

	return 0;
}

static const vec_remote_subtests dev_menu_ctl_subtests{
	{ "Menu Request", static_cast<__u16>(~CEC_LOG_ADDR_MASK_TV), dev_menu_ctl_request },
	{ "User Control Pressed", CEC_LOG_ADDR_MASK_ALL, rc_passthrough_user_ctrl_pressed },
	{ "User Control Released", CEC_LOG_ADDR_MASK_ALL, rc_passthrough_user_ctrl_released },
};

/* Deck Control */

static int deck_ctl_give_status(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_give_deck_status(&msg, true, CEC_OP_STATUS_REQ_ONCE);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(timed_out(&msg));

	fail_on_test_v2(node->remote[la].cec_version,
	                node->remote[la].has_deck_ctl && cec_msg_status_is_abort(&msg));
	fail_on_test_v2(node->remote[la].cec_version,
	                !node->remote[la].has_deck_ctl && !unrecognized_op(&msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;

	__u8 deck_info;

	cec_ops_deck_status(&msg, &deck_info);
	fail_on_test(deck_info < CEC_OP_DECK_INFO_PLAY || deck_info > CEC_OP_DECK_INFO_OTHER);

	cec_msg_init(&msg, me, la);
	cec_msg_give_deck_status(&msg, true, CEC_OP_STATUS_REQ_ON);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(timed_out(&msg));
	cec_ops_deck_status(&msg, &deck_info);
	fail_on_test(deck_info < CEC_OP_DECK_INFO_PLAY || deck_info > CEC_OP_DECK_INFO_OTHER);

	cec_msg_init(&msg, me, la);
	cec_msg_give_deck_status(&msg, true, CEC_OP_STATUS_REQ_OFF);
	/*
	 * Reply would not normally be expected for CEC_OP_STATUS_REQ_OFF.
	 * If a reply is received, then the follower failed to turn off
	 * status reporting as required.
	 */
	msg.reply = CEC_MSG_DECK_STATUS;
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!timed_out(&msg));

	return OK;
}

static int deck_ctl_give_status_invalid(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_give_deck_status(&msg, true, 0); /* Invalid Operand */
	fail_on_test(!transmit_timeout(node, &msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);

	cec_msg_init(&msg, me, la);
	cec_msg_give_deck_status(&msg, true, 4); /* Invalid Operand */
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);

	return OK;
}

static int deck_ctl_deck_ctl(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	__u8 deck_status;

	cec_msg_init(&msg, me, la);
	cec_msg_deck_control(&msg, CEC_OP_DECK_CTL_MODE_STOP);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test_v2(node->remote[la].cec_version,
	                node->remote[la].has_deck_ctl && unrecognized_op(&msg));
	fail_on_test_v2(node->remote[la].cec_version,
	                !node->remote[la].has_deck_ctl && !unrecognized_op(&msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	fail_on_test(deck_status_get(node, me, la, deck_status));
	if (cec_msg_status_is_abort(&msg)) {
		if (!incorrect_mode(&msg))
			return FAIL;
		if (deck_status == CEC_OP_DECK_INFO_NO_MEDIA)
			info("Stop: no media.\n");
		else
			warn("Deck has media but returned Feature Abort with Incorrect Mode.");
		return OK;
	}
	fail_on_test(deck_status != CEC_OP_DECK_INFO_STOP && deck_status != CEC_OP_DECK_INFO_NO_MEDIA);

	cec_msg_init(&msg, me, la);
	cec_msg_deck_control(&msg, CEC_OP_DECK_CTL_MODE_SKIP_FWD);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(deck_status_get(node, me, la, deck_status));
	/*
	 * If there is no media, Skip Forward should Feature Abort with Incorrect Mode
	 * even if Stop did not.  If Skip Forward does not Feature Abort, the deck
	 * is assumed to have media.
	 */
	if (incorrect_mode(&msg)) {
		fail_on_test(deck_status != CEC_OP_DECK_INFO_NO_MEDIA);
		return OK;
	}
	fail_on_test(cec_msg_status_is_abort(&msg));
	/* Wait for Deck to finish Skip Forward. */
	for (int i = 0; deck_status == CEC_OP_DECK_INFO_SKIP_FWD && i < long_timeout; i++) {
		sleep(1);
		fail_on_test(deck_status_get(node, me, la, deck_status));
	}
	fail_on_test(deck_status != CEC_OP_DECK_INFO_PLAY);

	cec_msg_init(&msg, me, la);
	cec_msg_deck_control(&msg, CEC_OP_DECK_CTL_MODE_SKIP_REV);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(cec_msg_status_is_abort(&msg)); /* Assumes deck has media. */
	fail_on_test(deck_status_get(node, me, la, deck_status));
	/* Wait for Deck to finish Skip Reverse. */
	for (int i = 0; deck_status == CEC_OP_DECK_INFO_SKIP_REV && i < long_timeout; i++) {
		sleep(1);
		fail_on_test(deck_status_get(node, me, la, deck_status));
	}
	fail_on_test(deck_status != CEC_OP_DECK_INFO_PLAY);

	cec_msg_init(&msg, me, la);
	cec_msg_deck_control(&msg, CEC_OP_DECK_CTL_MODE_EJECT);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(cec_msg_status_is_abort(&msg));
	fail_on_test(deck_status_get(node, me, la, deck_status));
	fail_on_test(deck_status != CEC_OP_DECK_INFO_NO_MEDIA);

	return OK;
}

static int deck_ctl_deck_ctl_invalid(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_deck_control(&msg, 0); /* Invalid Deck Control operand */
	fail_on_test(!transmit_timeout(node, &msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);

	cec_msg_init(&msg, me, la);
	cec_msg_deck_control(&msg, 5); /* Invalid Deck Control operand */
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);

	return OK;
}

static int deck_ctl_play(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	__u8 deck_status;

	cec_msg_init(&msg, me, la);
	cec_msg_play(&msg, CEC_OP_PLAY_MODE_PLAY_FWD);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test_v2(node->remote[la].cec_version,
	                node->remote[la].has_deck_ctl && unrecognized_op(&msg));
	fail_on_test_v2(node->remote[la].cec_version,
	                !node->remote[la].has_deck_ctl && !unrecognized_op(&msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	fail_on_test(deck_status_get(node, me, la, deck_status));
	if (cec_msg_status_is_abort(&msg)) {
		if (!incorrect_mode(&msg))
			return FAIL;
		if (deck_status == CEC_OP_DECK_INFO_NO_MEDIA)
			info("Play Still: no media.\n");
		else
			warn("Deck has media but returned Feature Abort with Incorrect Mode.");
		return OK;
	}
	fail_on_test(deck_status != CEC_OP_DECK_INFO_PLAY);

	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_STILL, CEC_OP_DECK_INFO_STILL));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_REV, CEC_OP_DECK_INFO_PLAY_REV));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_FAST_FWD_MIN, CEC_OP_DECK_INFO_FAST_FWD));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_FAST_REV_MIN, CEC_OP_DECK_INFO_FAST_REV));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_FAST_FWD_MED, CEC_OP_DECK_INFO_FAST_FWD));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_FAST_REV_MED, CEC_OP_DECK_INFO_FAST_REV));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_FAST_FWD_MAX, CEC_OP_DECK_INFO_FAST_FWD));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_FAST_REV_MAX, CEC_OP_DECK_INFO_FAST_REV));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_SLOW_FWD_MIN, CEC_OP_DECK_INFO_SLOW));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_SLOW_REV_MIN, CEC_OP_DECK_INFO_SLOW_REV));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_SLOW_FWD_MED, CEC_OP_DECK_INFO_SLOW));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_SLOW_REV_MED, CEC_OP_DECK_INFO_SLOW_REV));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_SLOW_FWD_MAX, CEC_OP_DECK_INFO_SLOW));
	fail_on_test(test_play_mode(node, me, la, CEC_OP_PLAY_MODE_PLAY_SLOW_REV_MAX, CEC_OP_DECK_INFO_SLOW_REV));

	cec_msg_init(&msg, me, la);
	cec_msg_deck_control(&msg, CEC_OP_DECK_CTL_MODE_STOP);
	fail_on_test(!transmit_timeout(node, &msg));

	return OK;
}

static int deck_ctl_play_invalid(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_play(&msg, 0); /* Invalid Operand */
	fail_on_test(!transmit_timeout(node, &msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);

	cec_msg_init(&msg, me, la);
	cec_msg_play(&msg, 4); /* Invalid Operand */
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);

	cec_msg_init(&msg, me, la);
	cec_msg_play(&msg, 0x26); /* Invalid Operand */
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);

	return OK;
}

static const vec_remote_subtests deck_ctl_subtests{
	{
		"Give Deck Status",
		CEC_LOG_ADDR_MASK_PLAYBACK | CEC_LOG_ADDR_MASK_RECORD,
		deck_ctl_give_status,
	},
	{
		"Give Deck Status Invalid Operand",
		CEC_LOG_ADDR_MASK_PLAYBACK | CEC_LOG_ADDR_MASK_RECORD,
		deck_ctl_give_status_invalid,
	},
	{
		"Deck Control",
		CEC_LOG_ADDR_MASK_PLAYBACK | CEC_LOG_ADDR_MASK_RECORD,
		deck_ctl_deck_ctl,
	},
	{
		"Deck Control Invalid Operand",
		CEC_LOG_ADDR_MASK_PLAYBACK | CEC_LOG_ADDR_MASK_RECORD,
		deck_ctl_deck_ctl_invalid,
	},
	{
		"Play",
		CEC_LOG_ADDR_MASK_PLAYBACK | CEC_LOG_ADDR_MASK_RECORD,
		deck_ctl_play,
	},
	{
		"Play Invalid Operand",
		CEC_LOG_ADDR_MASK_PLAYBACK | CEC_LOG_ADDR_MASK_RECORD,
		deck_ctl_play_invalid,
	},
};

/* Tuner Control */

static const char *bcast_type2s(__u8 bcast_type)
{
	switch (bcast_type) {
	case CEC_OP_ANA_BCAST_TYPE_CABLE:
		return "Cable";
	case CEC_OP_ANA_BCAST_TYPE_SATELLITE:
		return "Satellite";
	case CEC_OP_ANA_BCAST_TYPE_TERRESTRIAL:
		return "Terrestrial";
	default:
		return "Future use";
	}
}

static int log_tuner_service(const struct cec_op_tuner_device_info &info,
			     const char *prefix = "")
{
	printf("\t\t%s", prefix);

	if (info.is_analog) {
		double freq_mhz = (info.analog.ana_freq * 625) / 10000.0;

		printf("Analog Channel %.2f MHz (%s, %s)\n", freq_mhz,
		       bcast_system2s(info.analog.bcast_system),
		       bcast_type2s(info.analog.ana_bcast_type));

		switch (info.analog.bcast_system) {
		case CEC_OP_BCAST_SYSTEM_PAL_BG:
		case CEC_OP_BCAST_SYSTEM_SECAM_LQ:
		case CEC_OP_BCAST_SYSTEM_PAL_M:
		case CEC_OP_BCAST_SYSTEM_NTSC_M:
		case CEC_OP_BCAST_SYSTEM_PAL_I:
		case CEC_OP_BCAST_SYSTEM_SECAM_DK:
		case CEC_OP_BCAST_SYSTEM_SECAM_BG:
		case CEC_OP_BCAST_SYSTEM_SECAM_L:
		case CEC_OP_BCAST_SYSTEM_PAL_DK:
			break;
		default:
			return fail("invalid analog bcast_system %u", info.analog.bcast_system);
		}
		if (info.analog.ana_bcast_type > CEC_OP_ANA_BCAST_TYPE_TERRESTRIAL)
			return fail("invalid analog bcast_type %u\n", info.analog.ana_bcast_type);
		fail_on_test(!info.analog.ana_freq);
		return 0;
	}

	__u8 system = info.digital.dig_bcast_system;

	printf("%s Channel ", dig_bcast_system2s(system));
	if (info.digital.service_id_method) {
		__u16 major = info.digital.channel.major;
		__u16 minor = info.digital.channel.minor;

		switch (info.digital.channel.channel_number_fmt) {
		case CEC_OP_CHANNEL_NUMBER_FMT_2_PART:
			printf("%u.%u\n", major, minor);
			break;
		case CEC_OP_CHANNEL_NUMBER_FMT_1_PART:
			printf("%u\n", minor);
			break;
		default:
			printf("%u.%u\n", major, minor);
			return fail("invalid service ID method\n");
		}
		return 0;
	}


	switch (system) {
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_GEN:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_BS:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_CS:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_T: {
		__u16 tsid = info.digital.arib.transport_id;
		__u16 sid = info.digital.arib.service_id;
		__u16 onid = info.digital.arib.orig_network_id;

		printf("TSID: %u, SID: %u, ONID: %u\n", tsid, sid, onid);
		break;
	}
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_GEN:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_SAT:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_CABLE:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_T: {
		__u16 tsid = info.digital.atsc.transport_id;
		__u16 pn = info.digital.atsc.program_number;

		printf("TSID: %u, Program Number: %u\n", tsid, pn);
		break;
	}
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_GEN:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_S:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_S2:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_C:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_T: {
		__u16 tsid = info.digital.dvb.transport_id;
		__u16 sid = info.digital.dvb.service_id;
		__u16 onid = info.digital.dvb.orig_network_id;

		printf("TSID: %u, SID: %u, ONID: %u\n", tsid, sid, onid);
		break;
	}
	default:
		break;
	}

	switch (system) {
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_GEN:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_GEN:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_GEN:
		warn_once("generic digital broadcast systems should not be used");
		break;
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_BS:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_CS:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_T:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_CABLE:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_SAT:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_T:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_C:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_S:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_S2:
	case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_T:
		break;
	default:
		return fail("invalid digital broadcast system %u", system);
	}

	if (info.digital.service_id_method > CEC_OP_SERVICE_ID_METHOD_BY_CHANNEL)
		return fail("invalid service ID method %u\n", info.digital.service_id_method);

	return 0;
}

static int tuner_ctl_test(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	struct cec_op_tuner_device_info info = {};
	std::vector<struct cec_op_tuner_device_info> info_vec;
	bool has_tuner = (1 << la) & (CEC_LOG_ADDR_MASK_TV | CEC_LOG_ADDR_MASK_TUNER);
	int ret;

	cec_msg_init(&msg, me, la);
	cec_msg_give_tuner_device_status(&msg, true, CEC_OP_STATUS_REQ_ONCE);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!has_tuner && !timed_out_or_abort(&msg));
	if (!has_tuner)
		return OK_NOT_SUPPORTED;
	if (timed_out(&msg) || unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (cec_msg_status_is_abort(&msg))
		return OK_REFUSED;

	printf("\t    Start Channel Scan\n");
	cec_ops_tuner_device_status(&msg, &info);
	info_vec.push_back(info);
	ret = log_tuner_service(info);
	if (ret)
		return ret;

	while (true) {
		cec_msg_init(&msg, me, la);
		cec_msg_tuner_step_increment(&msg);
		fail_on_test(!transmit(node, &msg));
		fail_on_test(cec_msg_status_is_abort(&msg));
		if (cec_msg_status_is_abort(&msg)) {
			fail_on_test(abort_reason(&msg) == CEC_OP_ABORT_UNRECOGNIZED_OP);
			if (abort_reason(&msg) == CEC_OP_ABORT_REFUSED) {
				warn("Tuner step increment does not wrap.\n");
				break;
			}

			warn("Tuner at end of service list did not receive feature abort refused.\n");
			break;
		}
		cec_msg_init(&msg, me, la);
		cec_msg_give_tuner_device_status(&msg, true, CEC_OP_STATUS_REQ_ONCE);
		fail_on_test(!transmit_timeout(node, &msg));
		fail_on_test(timed_out_or_abort(&msg));
		memset(&info, 0, sizeof(info));
		cec_ops_tuner_device_status(&msg, &info);
		if (!memcmp(&info, &info_vec[0], sizeof(info)))
			break;
		ret = log_tuner_service(info);
		if (ret)
			return ret;
		info_vec.push_back(info);
	}
	printf("\t    Finished Channel Scan\n");

	printf("\t    Start Channel Test\n");
	for (const auto &iter : info_vec) {
		cec_msg_init(&msg, me, la);
		log_tuner_service(iter, "Select ");
		if (iter.is_analog)
			cec_msg_select_analogue_service(&msg, iter.analog.ana_bcast_type,
				iter.analog.ana_freq, iter.analog.bcast_system);
		else
			cec_msg_select_digital_service(&msg, &iter.digital);
		fail_on_test(!transmit(node, &msg));
		fail_on_test(cec_msg_status_is_abort(&msg));
		cec_msg_init(&msg, me, la);
		cec_msg_give_tuner_device_status(&msg, true, CEC_OP_STATUS_REQ_ONCE);
		fail_on_test(!transmit_timeout(node, &msg));
		fail_on_test(timed_out_or_abort(&msg));
		memset(&info, 0, sizeof(info));
		cec_ops_tuner_device_status(&msg, &info);
		if (memcmp(&info, &iter, sizeof(info))) {
			log_tuner_service(info);
			log_tuner_service(iter);
		}
		fail_on_test(memcmp(&info, &iter, sizeof(info)));
	}
	printf("\t    Finished Channel Test\n");

	cec_msg_init(&msg, me, la);
	cec_msg_select_analogue_service(&msg, 3, 16000, 9);
	printf("\t\tSelect invalid analog channel\n");
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);
	cec_msg_init(&msg, me, la);
	info.digital.service_id_method = CEC_OP_SERVICE_ID_METHOD_BY_DIG_ID;
	info.digital.dig_bcast_system = CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_S2;
	info.digital.dvb.transport_id = 0;
	info.digital.dvb.service_id = 0;
	info.digital.dvb.orig_network_id = 0;
	cec_msg_select_digital_service(&msg, &info.digital);
	printf("\t\tSelect invalid digital channel\n");
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);

	return 0;
}

static const vec_remote_subtests tuner_ctl_subtests{
	{ "Tuner Control", CEC_LOG_ADDR_MASK_TUNER | CEC_LOG_ADDR_MASK_TV, tuner_ctl_test },
};

/* One Touch Record */

static int one_touch_rec_tv_screen(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_record_tv_screen(&msg, true);
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test_v2(node->remote[la].cec_version,
			node->remote[la].has_rec_tv && unrecognized_op(&msg));
	fail_on_test_v2(node->remote[la].cec_version,
			!node->remote[la].has_rec_tv && !unrecognized_op(&msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	/* Follower should ignore this message if it is not sent by a recording device */
	if (node->prim_devtype != CEC_OP_PRIM_DEVTYPE_RECORD) {
		fail_on_test(!timed_out(&msg));
		return OK;
	}
	fail_on_test(timed_out(&msg));

	struct cec_op_record_src rec_src = {};

	cec_ops_record_on(&msg, &rec_src);

	fail_on_test(rec_src.type < CEC_OP_RECORD_SRC_OWN ||
	             rec_src.type > CEC_OP_RECORD_SRC_EXT_PHYS_ADDR);

	if (rec_src.type == CEC_OP_RECORD_SRC_DIGITAL) {
		switch (rec_src.digital.dig_bcast_system) {
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_GEN:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_GEN:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_GEN:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_BS:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_CS:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_T:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_CABLE:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_SAT:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_T:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_C:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_S:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_S2:
		case CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_T:
			break;
		default:
			return fail("Invalid digital service broadcast system operand.\n");
		}

		if (rec_src.digital.service_id_method == CEC_OP_SERVICE_ID_METHOD_BY_CHANNEL)
			fail_on_test(rec_src.digital.channel.channel_number_fmt < CEC_OP_CHANNEL_NUMBER_FMT_1_PART ||
			             rec_src.digital.channel.channel_number_fmt > CEC_OP_CHANNEL_NUMBER_FMT_2_PART);
	}

	if (rec_src.type == CEC_OP_RECORD_SRC_ANALOG) {
		fail_on_test(rec_src.analog.ana_bcast_type > CEC_OP_ANA_BCAST_TYPE_TERRESTRIAL);
		fail_on_test(rec_src.analog.bcast_system > CEC_OP_BCAST_SYSTEM_PAL_DK &&
		             rec_src.analog.bcast_system != CEC_OP_BCAST_SYSTEM_OTHER);
		fail_on_test(rec_src.analog.ana_freq == 0 || rec_src.analog.ana_freq == 0xffff);
	}

	if (rec_src.type == CEC_OP_RECORD_SRC_EXT_PLUG)
		fail_on_test(rec_src.ext_plug.plug == 0);

	return OK;
}

static int one_touch_rec_on(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	struct cec_op_record_src rec_src = {};

	rec_src.type = CEC_OP_RECORD_SRC_OWN;
	cec_msg_init(&msg, me, la);
	cec_msg_record_on(&msg, true, &rec_src);
	/* Allow 10s for reply because the spec says it may take several seconds to accurately respond. */
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out(&msg));
	if (unrecognized_op(&msg)) {
		fail_on_test(node->remote[la].prim_type == CEC_OP_PRIM_DEVTYPE_RECORD);
		return OK_NOT_SUPPORTED;
	}
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;

	__u8 rec_status;

	cec_ops_record_status(&msg, &rec_status);
	if (rec_status != CEC_OP_RECORD_STATUS_CUR_SRC)
		fail_on_test(!rec_status_is_a_valid_error_status(rec_status));

	/* In the following tests, these digital services are taken from the cec-follower tuner emulation. */
	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_SRC_DIGITAL;
	rec_src.digital.service_id_method = CEC_OP_SERVICE_ID_METHOD_BY_DIG_ID;
	rec_src.digital.dig_bcast_system = CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_BS;
	rec_src.digital.arib.transport_id = 1032;
	rec_src.digital.arib.service_id = 30203;
	rec_src.digital.arib.orig_network_id = 1;
	fail_on_test(one_touch_rec_on_send(node, me, la, rec_src, rec_status));
	if (rec_status != CEC_OP_RECORD_STATUS_DIG_SERVICE)
		fail_on_test(!rec_status_is_a_valid_error_status(rec_status));

	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_SRC_DIGITAL;
	rec_src.digital.service_id_method = CEC_OP_SERVICE_ID_METHOD_BY_CHANNEL;
	rec_src.digital.dig_bcast_system = CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_T;
	rec_src.digital.channel.channel_number_fmt = CEC_OP_CHANNEL_NUMBER_FMT_2_PART;
	rec_src.digital.channel.major = 4;
	rec_src.digital.channel.minor = 1;
	fail_on_test(one_touch_rec_on_send(node, me, la, rec_src, rec_status));
	if (rec_status != CEC_OP_RECORD_STATUS_DIG_SERVICE)
		fail_on_test(!rec_status_is_a_valid_error_status(rec_status));

	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_SRC_DIGITAL;
	rec_src.digital.service_id_method = CEC_OP_SERVICE_ID_METHOD_BY_DIG_ID;
	rec_src.digital.dig_bcast_system = CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_T;
	rec_src.digital.dvb.transport_id = 1004;
	rec_src.digital.dvb.service_id = 1040;
	rec_src.digital.dvb.orig_network_id = 8945;
	fail_on_test(one_touch_rec_on_send(node, me, la, rec_src, rec_status));
	if (rec_status != CEC_OP_RECORD_STATUS_DIG_SERVICE)
		fail_on_test(!rec_status_is_a_valid_error_status(rec_status));

	/* In the following tests, these channels taken from the cec-follower tuner emulation. */
	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_STATUS_ANA_SERVICE;
	rec_src.analog.ana_bcast_type = CEC_OP_ANA_BCAST_TYPE_CABLE;
	rec_src.analog.ana_freq = (471250 * 10) / 625;
	rec_src.analog.bcast_system = CEC_OP_BCAST_SYSTEM_PAL_BG;
	fail_on_test(one_touch_rec_on_send(node, me, la, rec_src, rec_status));
	if (rec_status != CEC_OP_RECORD_STATUS_ANA_SERVICE)
		fail_on_test(!rec_status_is_a_valid_error_status(rec_status));

	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_STATUS_ANA_SERVICE;
	rec_src.analog.ana_bcast_type = CEC_OP_ANA_BCAST_TYPE_SATELLITE;
	rec_src.analog.ana_freq = (551250 * 10) / 625;
	rec_src.analog.bcast_system = CEC_OP_BCAST_SYSTEM_SECAM_BG;
	fail_on_test(one_touch_rec_on_send(node, me, la, rec_src, rec_status));
	if (rec_status != CEC_OP_RECORD_STATUS_ANA_SERVICE)
		fail_on_test(!rec_status_is_a_valid_error_status(rec_status));

	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_STATUS_ANA_SERVICE;
	rec_src.analog.ana_bcast_type = CEC_OP_ANA_BCAST_TYPE_TERRESTRIAL;
	rec_src.analog.ana_freq = (185250 * 10) / 625;
	rec_src.analog.bcast_system = CEC_OP_BCAST_SYSTEM_PAL_DK;
	fail_on_test(one_touch_rec_on_send(node, me, la, rec_src, rec_status));
	if (rec_status != CEC_OP_RECORD_STATUS_ANA_SERVICE)
		fail_on_test(!rec_status_is_a_valid_error_status(rec_status));

	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_SRC_EXT_PLUG;
	rec_src.ext_plug.plug = 1;
	fail_on_test(one_touch_rec_on_send(node, me, la, rec_src, rec_status));
	if (rec_status != CEC_OP_RECORD_STATUS_EXT_INPUT)
		fail_on_test(!rec_status_is_a_valid_error_status(rec_status));

	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_SRC_EXT_PHYS_ADDR;
	fail_on_test(one_touch_rec_on_send(node, me, la, rec_src, rec_status));
	if (rec_status != CEC_OP_RECORD_STATUS_EXT_INPUT)
		fail_on_test(!rec_status_is_a_valid_error_status(rec_status));

	return OK;
}

static int one_touch_rec_on_invalid(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_record_on_own(&msg);
	msg.msg[2] = 0;  /* Invalid source operand */
	fail_on_test(!transmit_timeout(node, &msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);

	cec_msg_init(&msg, me, la);
	cec_msg_record_on_own(&msg);
	msg.msg[2] = 6;  /* Invalid source operand */
	fail_on_test(!transmit_timeout(node, &msg));
	fail_on_test(!cec_msg_status_is_abort(&msg));
	fail_on_test(abort_reason(&msg) != CEC_OP_ABORT_INVALID_OP);

	struct cec_op_record_src rec_src = {};

	rec_src.type = CEC_OP_RECORD_SRC_DIGITAL;
	rec_src.digital.service_id_method = CEC_OP_SERVICE_ID_METHOD_BY_CHANNEL;
	rec_src.digital.dig_bcast_system = 0x7f; /* Invalid digital service broadcast system operand */
	rec_src.digital.channel.channel_number_fmt = CEC_OP_CHANNEL_NUMBER_FMT_1_PART;
	rec_src.digital.channel.major = 0;
	rec_src.digital.channel.minor = 30203;
	fail_on_test(one_touch_rec_on_send_invalid(node, me, la, rec_src));

	rec_src.type = CEC_OP_RECORD_SRC_DIGITAL;
	rec_src.digital.service_id_method = CEC_OP_SERVICE_ID_METHOD_BY_CHANNEL;
	rec_src.digital.dig_bcast_system = CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_BS;
	rec_src.digital.channel.channel_number_fmt = 0; /* Invalid channel number format operand */
	rec_src.digital.channel.major = 0;
	rec_src.digital.channel.minor = 30609;
	fail_on_test(one_touch_rec_on_send_invalid(node, me, la, rec_src));

	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_SRC_ANALOG;
	rec_src.analog.ana_bcast_type = 0xff; /* Invalid analog broadcast type */
	rec_src.analog.ana_freq = (519250 * 10) / 625;
	rec_src.analog.bcast_system = CEC_OP_BCAST_SYSTEM_PAL_BG;
	fail_on_test(one_touch_rec_on_send_invalid(node, me, la, rec_src));

	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_SRC_ANALOG;
	rec_src.analog.ana_bcast_type = CEC_OP_ANA_BCAST_TYPE_SATELLITE;
	rec_src.analog.ana_freq = (703250 * 10) / 625;
	rec_src.analog.bcast_system = 0xff; /* Invalid analog broadcast system */
	fail_on_test(one_touch_rec_on_send_invalid(node, me, la, rec_src));

	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_SRC_ANALOG;
	rec_src.analog.ana_bcast_type = CEC_OP_ANA_BCAST_TYPE_TERRESTRIAL;
	rec_src.analog.ana_freq = 0; /* Invalid frequency */
	rec_src.analog.bcast_system = CEC_OP_BCAST_SYSTEM_NTSC_M;
	fail_on_test(one_touch_rec_on_send_invalid(node, me, la, rec_src));

	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_SRC_ANALOG;
	rec_src.analog.ana_bcast_type = CEC_OP_ANA_BCAST_TYPE_CABLE;
	rec_src.analog.ana_freq = 0xffff; /* Invalid frequency */
	rec_src.analog.bcast_system = CEC_OP_BCAST_SYSTEM_SECAM_L;
	fail_on_test(one_touch_rec_on_send_invalid(node, me, la, rec_src));

	memset(&rec_src, 0, sizeof(rec_src));
	rec_src.type = CEC_OP_RECORD_SRC_EXT_PLUG;
	rec_src.ext_plug.plug = 0; /* Invalid plug */
	fail_on_test(one_touch_rec_on_send_invalid(node, me, la, rec_src));

	return OK;
}

static int one_touch_rec_off(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_record_off(&msg, true);
	/* Allow 10s for reply because the spec says it may take several seconds to accurately respond. */
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	if (unrecognized_op(&msg)) {
		fail_on_test(node->remote[la].prim_type == CEC_OP_PRIM_DEVTYPE_RECORD);
		return OK_NOT_SUPPORTED;
	}
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	if (timed_out(&msg))
		return OK_PRESUMED;

	__u8 rec_status;

	cec_ops_record_status(&msg, &rec_status);

	fail_on_test(rec_status != CEC_OP_RECORD_STATUS_TERMINATED_OK &&
	             rec_status != CEC_OP_RECORD_STATUS_ALREADY_TERM);

	return OK;
}

static const vec_remote_subtests one_touch_rec_subtests{
	{ "Record TV Screen", CEC_LOG_ADDR_MASK_TV, one_touch_rec_tv_screen },
	{
		"Record On",
		CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP,
		one_touch_rec_on,
	},
	{
		"Record On Invalid Operand",
		CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP,
		one_touch_rec_on_invalid,
	},
	{ "Record Off", CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP, one_touch_rec_off },

};

/* Timer Programming */

static int timer_prog_set_analog_timer(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	/* Set timer to start tomorrow, at current time, for 2 hr, 30 min. */
	time_t tomorrow = node->current_time + (24 * 60 * 60);
	struct tm *t = localtime(&tomorrow);
	cec_msg_set_analogue_timer(&msg, true, t->tm_mday, t->tm_mon + 1, t->tm_hour, t->tm_min, 2, 30,
	                           0x7f, CEC_OP_ANA_BCAST_TYPE_CABLE, 7668, // 479.25 MHz
	                           node->remote[la].bcast_sys);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out(&msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	fail_on_test(timer_status_is_valid(msg));

	return OK;
}

static int timer_prog_set_digital_timer(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	struct cec_op_digital_service_id digital_service_id = {};

	digital_service_id.service_id_method = CEC_OP_SERVICE_ID_METHOD_BY_CHANNEL;
	digital_service_id.channel.channel_number_fmt = CEC_OP_CHANNEL_NUMBER_FMT_1_PART;
	digital_service_id.channel.minor = 1;
	digital_service_id.dig_bcast_system = node->remote[la].dig_bcast_sys;
	cec_msg_init(&msg, me, la);
	/* Set timer to start 2 days from now, at current time, for 4 hr, 30 min. */
	time_t two_days_ahead = node->current_time + (2 * 24 * 60 * 60);
	struct tm *t = localtime(&two_days_ahead);
	cec_msg_set_digital_timer(&msg, true, t->tm_mday, t->tm_mon + 1, t->tm_hour,
	                          t->tm_min, 4, 30, CEC_OP_REC_SEQ_ONCE_ONLY, &digital_service_id);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out(&msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	fail_on_test(timer_status_is_valid(msg));

	return 0;
}

static int timer_prog_set_ext_timer(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	/* Set timer to start 3 days from now, at current time, for 6 hr, 30 min. */
	time_t three_days_ahead = node->current_time + (3 * 24 * 60 * 60);
	struct tm *t = localtime(&three_days_ahead);
	cec_msg_set_ext_timer(&msg, true, t->tm_mday, t->tm_mon + 1, t->tm_hour, t->tm_min, 6, 30,
	                      CEC_OP_REC_SEQ_ONCE_ONLY, CEC_OP_EXT_SRC_PHYS_ADDR, 0, node->phys_addr);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out(&msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	fail_on_test(timer_status_is_valid(msg));

	return 0;
}

static int timer_prog_clear_analog_timer(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	/* Clear timer set to start tomorrow, at current time, for 2 hr, 30 min. */
	time_t tomorrow = node->current_time + (24 * 60 * 60);
	struct tm *t = localtime(&tomorrow);
	cec_msg_clear_analogue_timer(&msg, true, t->tm_mday, t->tm_mon + 1, t->tm_hour, t->tm_min, 2, 30,
	                             0x7f, CEC_OP_ANA_BCAST_TYPE_CABLE,7668, // 479.25 MHz
	                             node->remote[la].bcast_sys);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out(&msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	fail_on_test(timer_cleared_status_is_valid(msg));

	return 0;
}

static int timer_prog_clear_digital_timer(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;
	struct cec_op_digital_service_id digital_service_id = {};

	digital_service_id.service_id_method = CEC_OP_SERVICE_ID_METHOD_BY_CHANNEL;
	digital_service_id.channel.channel_number_fmt = CEC_OP_CHANNEL_NUMBER_FMT_1_PART;
	digital_service_id.channel.minor = 1;
	digital_service_id.dig_bcast_system = node->remote[la].dig_bcast_sys;
	cec_msg_init(&msg, me, la);
	/* Clear timer set to start 2 days from now, at current time, for 4 hr, 30 min. */
	time_t two_days_ahead = node->current_time + (2 * 24 * 60 * 60);
	struct tm *t = localtime(&two_days_ahead);
	cec_msg_clear_digital_timer(&msg, true, t->tm_mday, t->tm_mon + 1, t->tm_hour,
	                            t->tm_min, 4, 30, CEC_OP_REC_SEQ_ONCE_ONLY, &digital_service_id);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out(&msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	fail_on_test(timer_cleared_status_is_valid(msg));

	return 0;
}

static int timer_prog_clear_ext_timer(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	/* Clear timer set to start 3 days from now, at current time, for 6 hr, 30 min. */
	time_t three_days_ahead = node->current_time + (3 * 24 * 60 * 60);
	struct tm *t = localtime(&three_days_ahead);
	cec_msg_clear_ext_timer(&msg, true, t->tm_mday, t->tm_mon + 1, t->tm_hour, t->tm_min, 6, 30,
	                        CEC_OP_REC_SEQ_ONCE_ONLY, CEC_OP_EXT_SRC_PHYS_ADDR, 0, node->phys_addr);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out(&msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;
	if (cec_msg_status_is_abort(&msg))
		return OK_PRESUMED;
	fail_on_test(timer_cleared_status_is_valid(msg));

	return 0;
}

static int timer_prog_set_prog_title(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_set_timer_program_title(&msg, "Super-Hans II");
	fail_on_test(!transmit_timeout(node, &msg));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	if (refused(&msg))
		return OK_REFUSED;

	return OK_PRESUMED;
}

static int timer_errors(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	/* Day error: November 31, at 6:00 am, for 1 hr. */
	fail_on_test(send_timer_error(node, me, la, 31, Nov, 6, 0, 1, 0, CEC_OP_REC_SEQ_ONCE_ONLY));

	/* Day error: December 32, at 6:00 am, for 1 hr. */
	fail_on_test(send_timer_error(node, me, la, 32, Dec, 6, 0, 1, 0, CEC_OP_REC_SEQ_ONCE_ONLY));

	/* Day error: 0, in January, at 6:00 am, for 1 hr. Day range begins at 1. */
	fail_on_test(send_timer_error(node, me, la, 0, Jan, 6, 0, 1, 0, CEC_OP_REC_SEQ_ONCE_ONLY));

	/* Month error: 0, on day 5, at 6:00 am, for 1 hr. CEC month range is 1-12. */
	fail_on_test(send_timer_error(node, me, la, 5, 0, 6, 0, 1, 0, CEC_OP_REC_SEQ_ONCE_ONLY));

	/* Month error: 13, on day 5, at 6:00 am, for 1 hr. */
	fail_on_test(send_timer_error(node, me, la, 5, 13, 6, 0, 1, 0, CEC_OP_REC_SEQ_ONCE_ONLY));

	/* Start hour error: 24 hr, on August 5, for 1 hr. Start hour range is 0-23. */
	fail_on_test(send_timer_error(node, me, la, 5, Aug, 24, 0, 1, 0, CEC_OP_REC_SEQ_ONCE_ONLY));

	/* Start min error: 60 min, on August 5, for 1 hr. Start min range is 0-59. */
	fail_on_test(send_timer_error(node, me, la, 5, Aug, 0, 60, 1, 0, CEC_OP_REC_SEQ_ONCE_ONLY));

	/* Recording duration error: 0 hr, 0 min on August 5, at 6:00am. */
	fail_on_test(send_timer_error(node, me, la, 5, Aug, 6, 0, 0, 0, CEC_OP_REC_SEQ_ONCE_ONLY));

	/* Duplicate timer error: start 2 hrs from now, for 1 hr. */
	time_t two_hours_ahead = node->current_time + (2 * 60 * 60);
	struct tm *t = localtime(&two_hours_ahead);
	cec_msg_init(&msg, me, la);
	cec_msg_set_analogue_timer(&msg, true, t->tm_mday, t->tm_mon + 1, t->tm_hour, t->tm_min, 1, 0,
	                           CEC_OP_REC_SEQ_ONCE_ONLY,CEC_OP_ANA_BCAST_TYPE_CABLE,
	                           7668, // 479.25 MHz
	                           node->remote[la].bcast_sys);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out_or_abort(&msg));
	fail_on_test(timer_has_error(msg)); /* The first timer should be set. */
	fail_on_test(send_timer_error(node, me, la, t->tm_mday, t->tm_mon + 1, t->tm_hour,
	                              t->tm_min, 1, 0, CEC_OP_REC_SEQ_ONCE_ONLY));

	/* Clear the timer that was set to test duplicate timers. */
	fail_on_test(clear_timer(node, me, la, t->tm_mday, t->tm_mon + 1, t->tm_hour, t->tm_min, 1, 0,
	                         CEC_OP_REC_SEQ_ONCE_ONLY));

	/* Recording sequence error: 0xff, on August 5, at 6:00 am, for 1 hr. */
	fail_on_test(send_timer_error(node, me, la, 5, Aug, 6, 0, 1, 0, 0xff));

	/* Error in last day of February, at 6:00 am, for 1 hr. */
	time_t current_time = node->current_time;
	t = localtime(&current_time);
	if ((t->tm_mon + 1) > Feb)
		t->tm_year++; /* The timer will be for next year. */
	if (!(t->tm_year % 4) && ((t->tm_year % 100) || !(t->tm_year % 400)))
		fail_on_test(send_timer_error(node, me, la, 30, Feb, 6, 0, 1, 0, CEC_OP_REC_SEQ_ONCE_ONLY));
	else
		fail_on_test(send_timer_error(node, me, la, 29, Feb, 6, 0, 1, 0, CEC_OP_REC_SEQ_ONCE_ONLY));

	return OK;
}

static int timer_overlap_warning(struct node *node, unsigned me, unsigned la, bool interactive)
{
	struct cec_msg msg;

	time_t tomorrow = node->current_time + (24 * 60 * 60);
	struct tm *t = localtime(&tomorrow);

	/* No overlap: set timer for tomorrow at 8:00 am for 2 hr. */
	cec_msg_init(&msg, me, la);
	cec_msg_set_analogue_timer(&msg, true, t->tm_mday, t->tm_mon + 1, 8, 0, 2, 0,
	                           CEC_OP_REC_SEQ_ONCE_ONLY, CEC_OP_ANA_BCAST_TYPE_CABLE,
	                           7668, // 479.25 MHz
	                           node->remote[la].bcast_sys);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	if (unrecognized_op(&msg))
		return OK_NOT_SUPPORTED;
	fail_on_test(timed_out_or_abort(&msg));
	fail_on_test(timer_has_error(msg));
	fail_on_test(timer_overlap_warning_is_set(msg));

	/* No overlap, just adjacent: set timer for tomorrow at 10:00 am for 15 min. */
	cec_msg_init(&msg, me, la);
	cec_msg_set_analogue_timer(&msg, true, t->tm_mday, t->tm_mon + 1, 10, 0, 0, 15,
	                           CEC_OP_REC_SEQ_ONCE_ONLY, CEC_OP_ANA_BCAST_TYPE_CABLE,
	                           7668, // 479.25 MHz
	                           node->remote[la].bcast_sys);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out_or_abort(&msg));
	fail_on_test(timer_has_error(msg));
	fail_on_test(timer_overlap_warning_is_set(msg));

	/* No overlap, just adjacent: set timer for tomorrow at 7:45 am for 15 min. */
	cec_msg_init(&msg, me, la);
	cec_msg_set_analogue_timer(&msg, true, t->tm_mday, t->tm_mon + 1, 7, 45, 0, 15,
	                           CEC_OP_REC_SEQ_ONCE_ONLY, CEC_OP_ANA_BCAST_TYPE_CABLE,
	                           7668, // 479.25 MHz
	                           node->remote[la].bcast_sys);
	fail_on_test(!transmit_timeout(node, &msg, 10000));
	fail_on_test(timed_out_or_abort(&msg));
	fail_on_test(timer_has_error(msg));
	fail_on_test(timer_overlap_warning_is_set(msg));

	/* Overlap tail end: set timer for tomorrow at 9:00 am for 2 hr, repeats on Sun. */
	fail_on_test(send_timer_overlap(node, me, la, t->tm_mday, t->tm_mon + 1, 9, 0, 2, 0, 0x1));

	/* Overlap front end: set timer for tomorrow at 7:00 am for 1 hr, 30 min. */
	fail_on_test(send_timer_overlap(node, me, la, t->tm_mday, t->tm_mon + 1, 7, 0, 1, 30, 0x1));

	/* Overlap same start time: set timer for tomorrow at 8:00 am for 30 min. */
	fail_on_test(send_timer_overlap(node, me, la, t->tm_mday, t->tm_mon + 1, 8, 0, 0, 30, 0x1));

	/* Overlap same end time: set timer for tomorrow at 9:30 am for 30 min. */
	fail_on_test(send_timer_overlap(node, me, la, t->tm_mday, t->tm_mon + 1, 9, 30, 0, 30, 0x1));

	/* Overlap all timers: set timer for tomorrow at 6:00 am for 6 hr. */
	fail_on_test(send_timer_overlap(node, me, la, t->tm_mday, t->tm_mon + 1, 6, 0, 6, 0, 0x1));

	/* Clear all the timers. */
	fail_on_test(clear_timer(node, me, la, t->tm_mday, t->tm_mon + 1, 8, 0, 2, 0,
	                         CEC_OP_REC_SEQ_ONCE_ONLY));
	fail_on_test(clear_timer(node, me, la, t->tm_mday, t->tm_mon + 1, 10, 0, 0, 15,
	                         CEC_OP_REC_SEQ_ONCE_ONLY));
	fail_on_test(clear_timer(node, me, la, t->tm_mday, t->tm_mon + 1, 7, 45, 0, 15,
	                         CEC_OP_REC_SEQ_ONCE_ONLY));
	fail_on_test(clear_timer(node, me, la, t->tm_mday, t->tm_mon + 1, 9, 0, 2, 0, 0x1));
	fail_on_test(clear_timer(node, me, la, t->tm_mday, t->tm_mon + 1, 7, 0, 1, 30, 0x1));
	fail_on_test(clear_timer(node, me, la, t->tm_mday, t->tm_mon + 1, 8, 0, 0, 30, 0x1));
	fail_on_test(clear_timer(node, me, la, t->tm_mday, t->tm_mon + 1, 9, 30, 0, 30, 0x1));
	fail_on_test(clear_timer(node, me, la, t->tm_mday, t->tm_mon + 1, 6, 0, 6, 0, 0x1));

	return OK;
}

static const vec_remote_subtests timer_prog_subtests{
	{
		"Set Analogue Timer",
		CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP,
		timer_prog_set_analog_timer,
	},
	{
		"Set Digital Timer",
		CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP,
		timer_prog_set_digital_timer,
	},
	{
		"Set Timer Program Title",
		CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP,
		timer_prog_set_prog_title,
	},
	{
		"Set External Timer",
		CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP,
		timer_prog_set_ext_timer,
	},
	{
		"Clear Analogue Timer",
		CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP,
		timer_prog_clear_analog_timer,
	},
	{
		"Clear Digital Timer",
		CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP,
		timer_prog_clear_digital_timer,
	},
	{
		"Clear External Timer",
		CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP,
		timer_prog_clear_ext_timer,
	},
	{
		"Set Timers with Errors",
		CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP,
		timer_errors,
	},
	{
		"Set Overlapping Timers",
		CEC_LOG_ADDR_MASK_RECORD | CEC_LOG_ADDR_MASK_BACKUP,
		timer_overlap_warning,
	},
};

static const char *hec_func_state2s(__u8 hfs)
{
	switch (hfs) {
	case CEC_OP_HEC_FUNC_STATE_NOT_SUPPORTED:
		return "HEC Not Supported";
	case CEC_OP_HEC_FUNC_STATE_INACTIVE:
		return "HEC Inactive";
	case CEC_OP_HEC_FUNC_STATE_ACTIVE:
		return "HEC Active";
	case CEC_OP_HEC_FUNC_STATE_ACTIVATION_FIELD:
		return "HEC Activation Field";
	default:
		return "Unknown";
	}
}

static const char *host_func_state2s(__u8 hfs)
{
	switch (hfs) {
	case CEC_OP_HOST_FUNC_STATE_NOT_SUPPORTED:
		return "Host Not Supported";
	case CEC_OP_HOST_FUNC_STATE_INACTIVE:
		return "Host Inactive";
	case CEC_OP_HOST_FUNC_STATE_ACTIVE:
		return "Host Active";
	default:
		return "Unknown";
	}
}

static const char *enc_func_state2s(__u8 efs)
{
	switch (efs) {
	case CEC_OP_ENC_FUNC_STATE_EXT_CON_NOT_SUPPORTED:
		return "Ext Con Not Supported";
	case CEC_OP_ENC_FUNC_STATE_EXT_CON_INACTIVE:
		return "Ext Con Inactive";
	case CEC_OP_ENC_FUNC_STATE_EXT_CON_ACTIVE:
		return "Ext Con Active";
	default:
		return "Unknown";
	}
}

static const char *cdc_errcode2s(__u8 cdc_errcode)
{
	switch (cdc_errcode) {
	case CEC_OP_CDC_ERROR_CODE_NONE:
		return "No error";
	case CEC_OP_CDC_ERROR_CODE_CAP_UNSUPPORTED:
		return "Initiator does not have requested capability";
	case CEC_OP_CDC_ERROR_CODE_WRONG_STATE:
		return "Initiator is in wrong state";
	case CEC_OP_CDC_ERROR_CODE_OTHER:
		return "Other error";
	default:
		return "Unknown";
	}
}

static int cdc_hec_discover(struct node *node, unsigned me, unsigned la, bool print)
{
	/* TODO: For future use cases, it might be necessary to store the results
	   from the HEC discovery to know which HECs are possible to form, etc. */
	struct cec_msg msg;
	__u32 mode = CEC_MODE_INITIATOR | CEC_MODE_FOLLOWER;
	bool has_cdc = false;

	doioctl(node, CEC_S_MODE, &mode);
	cec_msg_init(&msg, me, la);
	cec_msg_cdc_hec_discover(&msg);
	fail_on_test(!transmit(node, &msg));

	/* The spec describes that we shall wait for messages
	   up to 1 second, and extend the deadline for every received
	   message. The maximum time to wait for incoming state reports
	   is 5 seconds. */
	unsigned ts_start = get_ts_ms();
	while (get_ts_ms() - ts_start < 5000) {
		__u8 from;

		memset(&msg, 0, sizeof(msg));
		msg.timeout = 1000;
		if (doioctl(node, CEC_RECEIVE, &msg))
			break;
		from = cec_msg_initiator(&msg);
		if (msg.msg[1] == CEC_MSG_FEATURE_ABORT) {
			if (from == la)
				return fail("Device replied Feature Abort to broadcast message\n");

			warn("Device %d replied Feature Abort to broadcast message\n", cec_msg_initiator(&msg));
		}
		if (msg.msg[1] != CEC_MSG_CDC_MESSAGE)
			continue;
		if (msg.msg[4] != CEC_MSG_CDC_HEC_REPORT_STATE)
			continue;

		__u16 phys_addr, target_phys_addr, hec_field;
		__u8 hec_func_state, host_func_state, enc_func_state, cdc_errcode, has_field;

		cec_ops_cdc_hec_report_state(&msg, &phys_addr, &target_phys_addr,
					     &hec_func_state, &host_func_state,
					     &enc_func_state, &cdc_errcode,
					     &has_field, &hec_field);

		if (target_phys_addr != node->phys_addr)
			continue;
		if (phys_addr == node->remote[la].phys_addr)
			has_cdc = true;
		if (!print)
			continue;

		from = cec_msg_initiator(&msg);
		info("Received CDC HEC State report from device %d (%s):\n", from, cec_la2s(from));
		info("Physical address                 : %x.%x.%x.%x\n",
		     cec_phys_addr_exp(phys_addr));
		info("Target physical address          : %x.%x.%x.%x\n",
		     cec_phys_addr_exp(target_phys_addr));
		info("HEC Functionality State          : %s\n", hec_func_state2s(hec_func_state));
		info("Host Functionality State         : %s\n", host_func_state2s(host_func_state));
		info("ENC Functionality State          : %s\n", enc_func_state2s(enc_func_state));
		info("CDC Error Code                   : %s\n", cdc_errcode2s(cdc_errcode));

		if (has_field) {
			std::ostringstream oss;

			/* Bit 14 indicates whether or not the device's HDMI
			   output has HEC support/is active. */
			if (!hec_field)
				oss << "None";
			else {
				if (hec_field & (1 << 14))
					oss << "out, ";
				for (int i = 13; i >= 0; i--) {
					if (hec_field & (1 << i))
						oss << "in" << (14 - i) << ", ";
				}
				oss << "\b\b ";
			}
			info("HEC Support Field    : %s\n", oss.str().c_str());
		}
	}

	mode = CEC_MODE_INITIATOR;
	doioctl(node, CEC_S_MODE, &mode);

	if (has_cdc)
		return 0;
	return OK_NOT_SUPPORTED;
}

static const vec_remote_subtests cdc_subtests{
	{ "CDC_HEC_Discover", CEC_LOG_ADDR_MASK_ALL, cdc_hec_discover },
};

/* Post-test checks */

static int post_test_check_recognized(struct node *node, unsigned me, unsigned la, bool interactive)
{
	bool fail = false;

	for (unsigned i = 0; i < 256; i++) {
		if (node->remote[la].recognized_op[i] && node->remote[la].unrecognized_op[i]) {
			struct cec_msg msg = {};
			msg.msg[1] = i;
			fail("Opcode %s has been both recognized by and has been replied\n", opcode2s(&msg).c_str());
			fail("Feature Abort [Unrecognized Opcode] to by the device.\n");
			fail = true;
		}
	}
	fail_on_test(fail);

	return 0;
}

static const vec_remote_subtests post_test_subtests{
	{ "Recognized/unrecognized message consistency", CEC_LOG_ADDR_MASK_ALL, post_test_check_recognized },
};

static const remote_test tests[] = {
	{ "Core", TAG_CORE, core_subtests },
	{ "Give Device Power Status feature", TAG_POWER_STATUS, power_status_subtests },
	{ "System Information feature", TAG_SYSTEM_INFORMATION, system_info_subtests },
	{ "Vendor Specific Commands feature", TAG_VENDOR_SPECIFIC_COMMANDS, vendor_specific_subtests },
	{ "Device OSD Transfer feature", TAG_DEVICE_OSD_TRANSFER, device_osd_transfer_subtests },
	{ "OSD String feature", TAG_OSD_DISPLAY, osd_string_subtests },
	{ "Remote Control Passthrough feature", TAG_REMOTE_CONTROL_PASSTHROUGH, rc_passthrough_subtests },
	{ "Device Menu Control feature", TAG_DEVICE_MENU_CONTROL, dev_menu_ctl_subtests },
	{ "Deck Control feature", TAG_DECK_CONTROL, deck_ctl_subtests },
	{ "Tuner Control feature", TAG_TUNER_CONTROL, tuner_ctl_subtests },
	{ "One Touch Record feature", TAG_ONE_TOUCH_RECORD, one_touch_rec_subtests },
	{ "Timer Programming feature", TAG_TIMER_PROGRAMMING, timer_prog_subtests },
	{ "Capability Discovery and Control feature", TAG_CAP_DISCOVERY_CONTROL, cdc_subtests },
	{ "Dynamic Auto Lipsync feature", TAG_DYNAMIC_AUTO_LIPSYNC, dal_subtests },
	{ "Audio Return Channel feature", TAG_ARC_CONTROL, arc_subtests },
	{ "System Audio Control feature", TAG_SYSTEM_AUDIO_CONTROL, sac_subtests },
	{ "Audio Rate Control feature", TAG_AUDIO_RATE_CONTROL, audio_rate_ctl_subtests },
	{ "Routing Control feature", TAG_ROUTING_CONTROL, routing_control_subtests },
	{ "Standby/Resume and Power Status", TAG_POWER_STATUS | TAG_STANDBY_RESUME, standby_resume_subtests },
	{ "Post-test checks", TAG_CORE, post_test_subtests },
};

static std::map<std::string, int> mapTests;
static std::map<std::string, bool> mapTestsNoWarnings;

void collectTests()
{
	std::map<std::string, __u64> mapTestFuncs;

	for (const auto &test : tests) {
		for (const auto &subtest : test.subtests) {
			std::string name = safename(subtest.name);
			auto func = (__u64)subtest.test_fn;

			if (mapTestFuncs.find(name) != mapTestFuncs.end() &&
			    mapTestFuncs[name] != func) {
				fprintf(stderr, "Duplicate subtest name, but different tests: %s\n", subtest.name);
				std::exit(EXIT_FAILURE);
			}
			mapTestFuncs[name] = func;
			mapTests[name] = DONT_CARE;
			mapTestsNoWarnings[name] = false;
		}
	}
}

void listTests()
{
	for (const auto &test : tests) {
		printf("%s:\n", test.name);
		for (const auto &subtest : test.subtests) {
			printf("\t%s\n", safename(subtest.name).c_str());
		}
	}
}

int setExpectedResult(char *optarg, bool no_warnings)
{
	char *equal = std::strchr(optarg, '=');

	if (!equal || equal == optarg || !isdigit(equal[1]))
		return 1;
	*equal = 0;
	std::string name = safename(optarg);
	if (mapTests.find(name) == mapTests.end())
		return 1;
	mapTests[name] = strtoul(equal + 1, nullptr, 0);
	mapTestsNoWarnings[name] = no_warnings;
	return 0;
}

void testRemote(struct node *node, unsigned me, unsigned la, unsigned test_tags,
		bool interactive)
{
	printf("testing CEC local LA %d (%s) to remote LA %d (%s):\n",
	       me, cec_la2s(me), la, cec_la2s(la));

	if (!util_interactive_ensure_power_state(node, me, la, interactive, CEC_OP_POWER_STATUS_ON))
		return;
	if (node->remote[la].in_standby && !interactive) {
		announce("The remote device is in standby. It should be powered on when testing. Aborting.");
		return;
	}
	if (!node->remote[la].has_power_status) {
		announce("The device didn't support Give Device Power Status.");
		announce("Assuming that the device is powered on.");
	}

	/* Ensure that the remote device knows the initiator's primary device type.*/
	struct cec_msg msg;

	cec_msg_init(&msg, me, la);
	cec_msg_report_physical_addr(&msg, node->phys_addr, node->prim_devtype);
	transmit_timeout(node, &msg);

	int ret = 0;

	for (const auto &test : tests) {
		if ((test.tags & test_tags) != test.tags)
			continue;

		printf("\t%s:\n", test.name);
		for (const auto &subtest : test.subtests) {
			const char *name = subtest.name;

			if (subtest.for_cec20 &&
			    (node->remote[la].cec_version < CEC_OP_CEC_VERSION_2_0 || !node->has_cec20))
				continue;

			if (subtest.in_standby) {
				struct cec_log_addrs laddrs = { };
				doioctl(node, CEC_ADAP_G_LOG_ADDRS, &laddrs);

				if (!laddrs.log_addr_mask)
					continue;
			}
			node->in_standby = subtest.in_standby;
			mode_set_initiator(node);
			unsigned old_warnings = warnings;
			ret = subtest.test_fn(node, me, la, interactive);
			bool has_warnings = old_warnings < warnings;
			if (!(subtest.la_mask & (1 << la)) && !ret)
				ret = OK_UNEXPECTED;

			if (mapTests[safename(name)] != DONT_CARE) {
				if (ret != mapTests[safename(name)])
					printf("\t    %s: %s (Expected '%s', got '%s')\n",
					       name, ok(FAIL),
					       result_name(mapTests[safename(name)], false),
					       result_name(ret, false));
				else if (has_warnings && mapTestsNoWarnings[safename(name)])
					printf("\t    %s: %s (Expected no warnings, but got %d)\n",
					       name, ok(FAIL), warnings - old_warnings);
				else if (ret == FAIL)
					printf("\t    %s: %s\n", name, ok(OK_EXPECTED_FAIL));
				else
					printf("\t    %s: %s\n", name, ok(ret));
			} else if (ret != NOTAPPLICABLE)
				printf("\t    %s: %s\n", name, ok(ret));
			if (ret == FAIL_CRITICAL)
				return;
		}
		printf("\n");
	}
}
