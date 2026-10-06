#include <stdint.h>
#include <byteswap.h>
#include <stdlib.h>
#include <string.h>
#include <runtime.h>
#include <bluetooth.h>

struct ModulePriv {
	struct PakBtSocket *conn;
	struct PakBtDevice *dev;
	uint8_t message_counter;
};

#define MAGIC {0xFE, 0xDC, 0xBA}
#define END_BYTE 0xEF

enum CommandCodes {
	SET_SETTING_TYPE0 = 0x08,
	SET_SETTING_TYPE1 = 0xff,
	SET_SETTING_TYPE2 = 0xc0,
	GET_PROPERTIES1 = 0x03,
	GET_PROPERTIES2 = 0xc1,
	GET_PROPERTIES3 = 0x07,
};

enum PacketType {
	PACKET_COMMAND = 0xC0,
	PACKET_EVENT = 0x80,
	PACKET_RESP = 0x00,
};

// SET_SETTING_TYPE2 / 0xc0
enum PropertiesType2 {
	PROP_TOUCH_CONTROLS = 2,
	PROP_MOVIE_MUSIC_MODE = 5,
	// Unknown 7 set during init
};
// SET_SETTING_TYPE1 / 0xff
enum PropertiesType1 {
	PROP_SAFE_HEARING = 3,
	PROP_ENABLE_TOUCH_CONTROLS = 5,
};
// SET_SETTING_TYPE0 / 0x08
enum PropertiesType0 {
	PROP_EQUALIZER = 4,
	PROP_NOISE_CANCELLING = 0xd,
};

enum AncModes {
	ANC_BE_AWARE = 2,
	ANC_ON = 1,
	ANC_OFF = 0,
};

enum EqualizerPresetIndexes {
	EQ1 = 0,
	EQ2 = 1,
	EQ3 = 2,
	EQ_CUSTOM = 6,
};

enum PropertyC1Codes {
	PROP_BATTERY_STATUS = 0, // 0x64 0x64 0x64 == 100% 100% 100%
	PROP_DEVICE_NAME = 1, // "JLab GO Pods ANC"
	PROP_FW_UNKNOWN_KEY = 0xd, // "hE9yfseX6UdK7rFh"
	PROP_FW_VERSION_CODE = 0xc, // "jl_sdk_ac697_publish"
};

struct __attribute__((packed)) PacketHeader {
	uint8_t magic[3];
	uint8_t packet_type;
	uint8_t command;
	uint16_t length;
	uint8_t payload[]; // of size length
	// end byte (0xEF)
};

struct __attribute__((packed)) SetPropertyContainer {
	uint8_t transaction_counter;
	uint8_t unknown_0xff;
	uint8_t value_length;
	uint8_t property_id;
	uint8_t selected_index;
};

struct __attribute__((packed)) NoiseCancellingPayload {
	struct SetPropertyContainer hdr;

	uint8_t anc_mode; // AncModes

	// Following 2 seem to always be 0x4000
	uint16_t unknown_4000_1;
	uint16_t unknown_4000_2;
	uint16_t value_1;
	uint16_t value_2;
}noise_cancelling;

struct __attribute__((packed)) SetPropertyByte {
	uint8_t transaction_counter;
	uint8_t length;
	uint8_t property_id;
	uint8_t value;
}mode;

struct __attribute__((packed)) EqualizerValue {
	uint8_t val_31;
	uint8_t val_62;
	uint8_t val_125;
	uint8_t val_250;
	uint8_t val_500;
	uint8_t val_1k;
	uint8_t val_2k;
	uint8_t val_4k;
	uint8_t val_8k;
	uint8_t val_16k;
};
struct __attribute__((packed)) SetEqualizerPayload {
	struct SetPropertyContainer hdr;
	struct EqualizerValue value;
};

static int init(struct PakModule *mod) {
	pak_rt_set_tick_interval(mod, 1000 * 1000);
	mod->priv = calloc(1, sizeof(struct ModulePriv));
	return 0;
}

static int on_try_connect_bluetooth(struct PakModule *mod, struct PakBtDevice *dev, struct PakSavedConnection *saved, int job) {
	struct PakBtSocket *conn = pak_bt_connect_to_service_channel(mod->bt, dev, "00001101-0000-1000-8000-00805f9b34fb");
	if (conn == NULL) {
		pak_debug_log(mod, "pak_bt_connect_to_service_channel");
		return -1;
	}
	mod->priv->conn = conn;
	mod->priv->dev = dev;

	pak_rt_set_session_property(mod, PAK_PROP_NAME, dev->name);
	
	return 0;
}

static int on_idle_tick(struct PakModule *mod, unsigned int us_since_last_tick) {
	pak_bt_device_update(mod->bt, mod->priv->dev);
	if (!mod->priv->dev->is_connected) pak_rt_fatal_error(mod, "Disconnected");
	return 0;
}

static int on_disconnect(struct PakModule *mod) {
	pak_bt_close_socket(mod->priv->conn);
	pak_bt_unref_device(mod->bt, mod->priv->dev);
	return 0;
}

static int on_switch_screen(struct PakModule *mod, int old_screen, int new_screen, int job) {
	return 0;
}

static int on_prop_changed(struct PakModule *mod, int job, const char *name, struct PakWidget *prop) {
	pak_global_log("on_prop_changed %s", name);
	return 0;
}

void hexdump(void *data, unsigned int length) {
	for (unsigned int i = 0 ; i < length; i++) {
		printf("%02x ", ((uint8_t *)data)[i]);
	}
	printf("\n");
}

// https://developers.google.com/nearby/fast-pair/specifications/extensions/deviceinformation
static int google_fastpair(struct PakModule *mod, struct PakBt *bt, struct PakBtDevice *dev) {
	struct PakBtSocket *sock = pak_bt_connect_to_service_channel(bt, dev, "df21fe2c-2515-4fdb-8886-f12c4d67927c");
	if (sock == NULL) {
		pak_debug_log(mod, "pak_bt_connect_to_service_channel");
		return -1;
	}

	struct FastPairPacket {
		uint8_t msggroup;
		uint8_t msgcode;
		uint16_t datalen;
		uint8_t data[10];
	};

	struct FastPairPacket send_android_info = {
		.msggroup = 3,
		.msgcode = 8,
		.datalen = bswap_16(2),
		.data = {0x1, 0x24}
	};

	pak_bt_write(sock, &send_android_info, 6);
	hexdump(&send_android_info, 6);

	// < 0000   03 01 00 03 2f 45 f5
	// < 0000   03 02 00 06 47 c0 43 c7 c7 e5
	// < 0000   03 03 00 03 64 64 37
	// > 0000   03 08 00 02 01 24

	pak_bt_close_socket(sock);
	return 0;
}

static int on_run_test(struct PakModule *mod, int job) {
	pak_debug_log(mod, "Hello");
	struct PakBtAdapter *adapter = pak_bt_get_adapter(mod->bt, 0);
	if (adapter == NULL) return -1;
	struct PakBtDevice *dev = pak_bt_get_device(mod->bt, adapter, 0, PAK_FILTER_CONNECTED);
	if (dev == NULL) return -2;
	pak_debug_log(mod, "Connecting to %s", dev->name);
	if (on_try_connect_bluetooth(mod, dev, NULL, job)) return -1;

	int message_counter = 0x40;
	char buffer[0xff];
	int of = 0;

	//google_fastpair(mod, mod->bt, dev);

//	((struct PacketHeader *)&buffer[of])[0] = (struct PacketHeader){
//		.magic = MAGIC,
//		.packet_type = PACKET_COMMAND,
//		.command = SET_SETTING_TYPE0,
//		.length = bswap_16(0xd),
//	};
//	of += sizeof(struct PacketHeader);
//	((struct NoiseCancellingPayload *)&buffer[of])[0] = (struct NoiseCancellingPayload){
//		.hdr = {message_counter++, 0xff, 0xa, PROP_NOISE_CANCELLING},
//		.anc_mode = ANC_OFF,
//		.unknown_4000_1 = bswap_16(0x4000),
//		.unknown_4000_2 = bswap_16(0x4000),
//		.value_1 = bswap_16(0x0),
//		.value_2 = bswap_16(0x0),
//	};
//	of += sizeof(struct NoiseCancellingPayload);
//	buffer[of++] = END_BYTE;

	// FE DC BA C0 06 00 02 00 01 EF

//	((struct PacketHeader *)&buffer[of])[0] = (struct PacketHeader){
//		.magic = MAGIC,
//		.packet_type = PACKET_COMMAND,
//		.command = GET_PROPERTIES2,
//		.length = bswap_16(0x5),
//	};
//	of += sizeof(struct PacketHeader);
//	buffer[of++] = 0x47;
//	buffer[of++] = 0xff;
//	buffer[of++] = 0xff;
//	buffer[of++] = 0xff;
//	buffer[of++] = 0xff;
//	buffer[of++] = END_BYTE;

	if (1) {
		((struct PacketHeader *)&buffer[of])[0] = (struct PacketHeader){
			.magic = MAGIC,
			.packet_type = PACKET_COMMAND,
			.command = 0x6,
			.length = bswap_16(0x2),
		};
		of += sizeof(struct PacketHeader);
		buffer[of++] = 0x00;
		buffer[of++] = 0x01;
		buffer[of++] = END_BYTE;

		hexdump(buffer, of);

		int rc = pak_bt_write(mod->priv->conn, buffer, of);

		unsigned char magic_packet[17] = {0x0, 0x67, 0xc6, 0x69, 0x73, 0x51, 0xff, 0x4a, 0xec, 0x29, 0xcd, 0xba, 0xab, 0xf2, 0xfb, 0xe3, 0x46};
		pak_bt_write(mod->priv->conn, magic_packet, sizeof(magic_packet));
		rc = pak_bt_read(mod->priv->conn, magic_packet, sizeof(magic_packet));
		if (rc != sizeof(magic_packet)) {
			pak_debug_log(mod, "Failed to read handshake response");
			return -1;
		}
		hexdump(magic_packet, rc);
		pak_bt_write(mod->priv->conn, (uint8_t[]){0x2, 0x70, 0x61, 0x73, 0x73}, 5);
		rc = pak_bt_read(mod->priv->conn, magic_packet, sizeof(magic_packet));
		if (rc != sizeof(magic_packet)) {
			pak_debug_log(mod, "Failed to read handshake response");
			return -1;
		}
		hexdump(magic_packet, rc);
		unsigned char magic_packet2[] = {0x1, 0xc3, 0x44, 0xc3, 0x1b, 0xda, 0xc2, 0x20, 0x2c, 0x4d, 0xd1, 0xb2, 0x5f, 0xec, 0x41, 0x7a, 0xdb};
		pak_bt_write(mod->priv->conn, magic_packet2, sizeof(magic_packet2));
		rc = pak_bt_read(mod->priv->conn, magic_packet2, sizeof(magic_packet2));
		if (rc != 5) {
			pak_debug_log(mod, "Failed to read handshake response");
			return -1;
		}
		hexdump(magic_packet, rc);
	}

	on_disconnect(mod);
	return 0;
}

int get_module(struct PakModule *mod) {
	mod->init = init;
	mod->on_try_connect_bluetooth = on_try_connect_bluetooth;
	mod->on_idle_tick = on_idle_tick;
	mod->on_run_test = on_run_test;
	mod->on_disconnect = on_disconnect;
	mod->on_switch_screen = on_switch_screen;
	mod->on_setting_changed = on_prop_changed;
	return 0;
}
