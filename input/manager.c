/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2008  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/hidp.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "logging.h"
#include "textfile.h"
#include "../hcid/adapter.h"
#include "../hcid/device.h"

#include "device.h"
#include "server.h"
#include "error.h"
#include "manager.h"
#include "storage.h"
#include "glib-helper.h"

static int idle_timeout = 0;

static DBusConnection *connection = NULL;

static void epox_endian_quirk(unsigned char *data, int size)
{
	/* USAGE_PAGE (Keyboard)	05 07
	 * USAGE_MINIMUM (0)		19 00
	 * USAGE_MAXIMUM (65280)	2A 00 FF   <= must be FF 00
	 * LOGICAL_MINIMUM (0)		15 00
	 * LOGICAL_MAXIMUM (65280)	26 00 FF   <= must be FF 00
	 */
	unsigned char pattern[] = { 0x05, 0x07, 0x19, 0x00, 0x2a, 0x00, 0xff,
						0x15, 0x00, 0x26, 0x00, 0xff };
	int i;

	if (!data)
		return;

	for (i = 0; i < size - sizeof(pattern); i++) {
		if (!memcmp(data + i, pattern, sizeof(pattern))) {
			data[i + 5] = 0xff;
			data[i + 6] = 0x00;
			data[i + 10] = 0xff;
			data[i + 11] = 0x00;
		}
	}
}

static void extract_hid_record(sdp_record_t *rec, struct hidp_connadd_req *req)
{
	sdp_data_t *pdlist, *pdlist2;
	uint8_t attr_val;

	pdlist = sdp_data_get(rec, 0x0101);
	pdlist2 = sdp_data_get(rec, 0x0102);
	if (pdlist) {
		if (pdlist2) {
			if (strncmp(pdlist->val.str, pdlist2->val.str, 5)) {
				strncpy(req->name, pdlist2->val.str, 127);
				strcat(req->name, " ");
			}
			strncat(req->name, pdlist->val.str, 127 - strlen(req->name));
		} else
			strncpy(req->name, pdlist->val.str, 127);
	} else {
		pdlist2 = sdp_data_get(rec, 0x0100);
		if (pdlist2)
			strncpy(req->name, pdlist2->val.str, 127);
 	}

	pdlist = sdp_data_get(rec, SDP_ATTR_HID_PARSER_VERSION);
	req->parser = pdlist ? pdlist->val.uint16 : 0x0100;

	pdlist = sdp_data_get(rec, SDP_ATTR_HID_DEVICE_SUBCLASS);
	req->subclass = pdlist ? pdlist->val.uint8 : 0;

	pdlist = sdp_data_get(rec, SDP_ATTR_HID_COUNTRY_CODE);
	req->country = pdlist ? pdlist->val.uint8 : 0;

	pdlist = sdp_data_get(rec, SDP_ATTR_HID_VIRTUAL_CABLE);
	attr_val = pdlist ? pdlist->val.uint8 : 0;
	if (attr_val)
		req->flags |= (1 << HIDP_VIRTUAL_CABLE_UNPLUG);

	pdlist = sdp_data_get(rec, SDP_ATTR_HID_BOOT_DEVICE);
	attr_val = pdlist ? pdlist->val.uint8 : 0;
	if (attr_val)
		req->flags |= (1 << HIDP_BOOT_PROTOCOL_MODE);

	pdlist = sdp_data_get(rec, SDP_ATTR_HID_DESCRIPTOR_LIST);
	if (pdlist) {
		pdlist = pdlist->val.dataseq;
		pdlist = pdlist->val.dataseq;
		pdlist = pdlist->next;

		req->rd_data = g_try_malloc0(pdlist->unitSize);
		if (req->rd_data) {
			memcpy(req->rd_data, (unsigned char *) pdlist->val.str,
								pdlist->unitSize);
			req->rd_size = pdlist->unitSize;
			epox_endian_quirk(req->rd_data, req->rd_size);
		}
	}
}

/*
 * Stored inputs registration functions
 */

static int load_stored(const char *source, const char *destination,
			struct hidp_connadd_req *hidp)
{
	char filename[PATH_MAX + 1];
	char *value;

	/* load the input stored */
	create_name(filename, PATH_MAX, STORAGEDIR, destination, "input");

	value = textfile_get(filename, destination);
	if (!value)
		return -EINVAL;

	memset(&hidp, 0, sizeof(hidp));

	return parse_stored_device_info(value, hidp);
}

int input_probe(struct btd_device_driver *driver,
		struct btd_device *device, GSList *records)
{
	struct adapter *adapter = device_get_adapter(device);
	const gchar *path = device_get_path(device);
	const char *source, *destination;
	struct hidp_connadd_req hidp;
	bdaddr_t src, dst;

	DBG("path %s", path);

	memset(&hidp, 0, sizeof(hidp));

	source = adapter_get_address(adapter);
	destination = device_get_address(device);

	if (load_stored(source, destination, &hidp) == 0)
		goto done;

	hidp.idle_to = idle_timeout * 60;

	extract_hid_record(records->data, &hidp);

done:
	str2ba(source, &src);
	str2ba(destination, &dst);

	store_device_info(&src, &dst, &hidp);

	if (hidp.rd_data)
		g_free(hidp.rd_data);

	return input_device_register(connection, &src, &dst, &hidp, path);
}

void input_remove(struct btd_device_driver *driver,
			struct btd_device *device)
{
	const gchar *path = device_get_path(device);

	DBG("path %s", path);

	input_device_unregister(connection, path);
}

int headset_input_probe(struct btd_device_driver *driver,
			struct btd_device *device, GSList *records)
{
	struct adapter *adapter = device_get_adapter(device);
	const gchar *path = device_get_path(device);
	sdp_record_t *record = records->data;
	sdp_list_t *protos;
	uint8_t ch;
	const char *source, *destination;
	bdaddr_t src, dst;

	DBG("path %s", path);

	if (sdp_get_access_protos(record, &protos) < 0) {
		error("Invalid record");
		return -EINVAL;
	}

	ch = sdp_get_proto_port(protos, RFCOMM_UUID);
	sdp_list_foreach(protos, (sdp_list_func_t) sdp_list_free, NULL);
	sdp_list_free(protos, NULL);

	if (ch <= 0) {
		error("Invalid RFCOMM channel");
		return -EINVAL;
	}

	source = adapter_get_address(adapter);
	destination = device_get_address(device);

	str2ba(source, &src);
	str2ba(destination, &dst);

	return fake_input_register(connection, &src, &dst, ch, path);
}

void headset_input_remove(struct btd_device_driver *driver,
				struct btd_device *device)
{
	const gchar *path = device_get_path(device);

	DBG("path %s", path);

	input_device_unregister(connection, path);
}

static struct btd_device_driver input_driver = {
	.name	= "input",
	.uuids	= BTD_UUIDS(HID_UUID),
	.probe	= input_probe,
	.remove	= input_remove,
};

static struct btd_device_driver input_headset_driver = {
	.name	= "input-headset",
	.uuids	= BTD_UUIDS(HSP_HS_UUID),
	.probe	= headset_input_probe,
	.remove	= headset_input_remove,
};

int input_manager_init(DBusConnection *conn, GKeyFile *config)
{
	GError *err = NULL;

	if (config) {
		idle_timeout = g_key_file_get_integer(config, "General",
						"IdleTimeout", &err);
		if (err) {
			debug("input.conf: %s", err->message);
			g_error_free(err);
		}
	}

	connection = dbus_connection_ref(conn);

	server_start();

	btd_register_device_driver(&input_driver);
	btd_register_device_driver(&input_headset_driver);

	return 0;
}

void input_manager_exit(void)
{
	server_stop();

	btd_unregister_device_driver(&input_driver);
	btd_unregister_device_driver(&input_headset_driver);

	dbus_connection_unref(connection);

	connection = NULL;
}
