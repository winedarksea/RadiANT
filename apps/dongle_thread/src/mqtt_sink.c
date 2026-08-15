/* SPDX-License-Identifier: Apache-2.0 */
/*
 * mqtt_sink.c - one RADIANT_SINK_DEFINE onto a plain MQTT broker.
 *
 * docs/radiant-bridge.md section 9. Plain MQTT over TCP, not MQTT-SN: section
 * 9.0 rejects MQTT-SN because it needs a gateway daemon that nothing in a
 * Thread network, a border router or Home Assistant provides, and because the
 * constraint it was designed for (a network with no TCP) does not apply -
 * OpenThread sits under Zephyr's native IP stack, so CONFIG_NET_TCP plus
 * CONFIG_MQTT_LIB is an ordinary configuration.
 *
 * ---------------------------------------------------------------------------
 * THIS IS WHY THERE IS A SEPARATE bridge.conf
 * ---------------------------------------------------------------------------
 *
 * CONFIG_NET_TCP is deliberately ABSENT from thread.conf, and that file says
 * why in its own words: flash the gate image does not need, plus air time the
 * gate would then have to account for. Section 7.4's coexistence numbers were
 * taken on a TCP-free image. Folding MQTT into thread.conf would silently
 * invalidate them - the gate arms would be measuring a different image from
 * the one the recorded numbers came from - so MQTT lives in its own fragment
 * and the gate arms stay TCP-free. Do not merge them.
 *
 * ---------------------------------------------------------------------------
 * LAZY CONNECT, AND THE 31-SECOND TRAP IT AVOIDS
 * ---------------------------------------------------------------------------
 *
 * Nothing here runs at boot. The first publish() that finds no connection is
 * what opens the socket - the same shape thread_coex_load.c's socket uses, and
 * for a sharper reason.
 *
 * The obvious alternative is CONFIG_NET_CONFIG_NEED_IPV6, so that the stack is
 * addressable before the application starts. That makes net_config_init()
 * BLOCK until the interface has an IPv6 address, up to
 * CONFIG_NET_CONFIG_INIT_TIMEOUT (30 s by default) - and a Thread MTD has no
 * address until it has found a parent and attached. It runs before main(), so
 * the board produces no console output, no ANT stack and no answer for the
 * host for the whole wait. Measured on this bench: boot banner at
 * 00:48:16.891, application init at 00:48:47.936 - 31.04 s, reproducible to
 * the tenth, and read as a hung image by both a human and by ant_verify.py.
 * Several runs were scored against a board that was merely still waiting.
 *
 * So: no boot-time connect, no NEED_IPV6, and a connect attempt that fails is
 * retried later rather than being fatal.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS DEFERRED, DELIBERATELY
 * ---------------------------------------------------------------------------
 *
 * Section 9.0a's DNS-SD discovery of the broker rests on two things nobody has
 * verified on real hardware (whether HA's OTBR add-on enables the Discovery
 * Proxy, and whether the Mosquitto add-on advertises _mqtt._tcp), and the
 * WebSerial credential page is a browser application. Both are their own
 * phase. For v1 the broker address is a Kconfig string and it is a numeric
 * IPv6 literal - there is no resolver call here at all, so there is no name
 * lookup to hang on.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>

#include "ant_radio.h" /* ANTR_HOST_THREAD_PRIORITY */

#include "radiant_binding.h"
#include "radiant_bridge.h"

#include "bridge_app.h"

LOG_MODULE_REGISTER(mqtt_sink, LOG_LEVEL_INF);

/* ── Topics (section 9.1) ──────────────────────────────────────────────────── */

/*
 * radiant/<bridge_id>/status                     retained, LWT
 * radiant/<bridge_id>/<binding_uuid>/status      retained, per-sensor liveness
 * radiant/<bridge_id>/<binding_uuid>/<field_id>  the value
 *
 * binding_uuid, NEVER a device number. Section 5 is unambiguous about this and
 * gives three reasons: a device number is 16 bits, it is re-rollable by
 * design, and a collision across two straps in one room is a one-in-65535
 * event that will happen at a trade show. An MQTT topic has to survive a
 * battery change. There is no path in this file from a struct radiant_binding
 * to a topic that reads ::devnum, and that is the property to preserve.
 */
#define TOPIC_MAX 96

static char bridge_status_topic[TOPIC_MAX];

static void uuid_str(uint64_t uuid, char *out, size_t out_len)
{
	(void)snprintf(out, out_len, "%08x%08x", (unsigned int)(uuid >> 32),
		       (unsigned int)(uuid & 0xFFFFFFFFu));
}

/* ── The Home Assistant discovery table (section 9.2) ──────────────────────── */

/*
 * THE ONE CONTRACT IN THE BRIDGE THAT THIS PROJECT DOES NOT OWN. Section 9.2
 * says so in as many words: Home Assistant versions its discovery schema and
 * it will move. The mitigation it prescribes is exactly this - keep the
 * generator in ONE place, keep it table-driven off the vocabulary, and treat a
 * schema change as a table edit. So every field below is a row here and
 * nothing about HA's JSON is spelled out anywhere else in the tree.
 *
 * device_class is omitted where HA has no matching class rather than guessed
 * at: an unknown device_class makes HA reject the whole discovery message, so
 * a guess costs the entity rather than costing precision.
 */
struct ha_row {
	uint8_t     field_type;
	const char *unit;         /* unit_of_measurement; NULL to omit */
	const char *device_class; /* NULL to omit */
};

static const struct ha_row ha_table[] = {
	/* Booleans and enums: no unit, no class. Published as sensors rather
	 * than binary_sensors because the rule outputs travel as 0/1 integers
	 * on the same path as everything else, and one entity component means
	 * one discovery generator. */
	{ RADIANT_FIELD_BOOLEAN_STATE,      NULL,     NULL },
	{ RADIANT_FIELD_OCCUPANCY,          NULL,     NULL },
	{ RADIANT_FIELD_CONTACT,            NULL,     NULL },
	{ RADIANT_FIELD_ONOFF,              NULL,     NULL },

	{ RADIANT_FIELD_TEMPERATURE,        "K",      "temperature" },
	{ RADIANT_FIELD_HUMIDITY,           "%",      "humidity" },
	{ RADIANT_FIELD_PRESSURE,           "Pa",     "pressure" },
	{ RADIANT_FIELD_ILLUMINANCE,        "lx",     "illuminance" },
	{ RADIANT_FIELD_SPEED,              "m/s",    "speed" },
	{ RADIANT_FIELD_ACTIVE_POWER,       "W",      "power" },
	{ RADIANT_FIELD_VOLTAGE,            "V",      "voltage" },
	{ RADIANT_FIELD_CURRENT,            "A",      "current" },
	{ RADIANT_FIELD_FREQUENCY,          "Hz",     "frequency" },
	{ RADIANT_FIELD_BATTERY_SOC,        "%",      "battery" },

	/* The stated non-SI exception (radiant_bridge.h): heart rate is bpm.
	 * HA has no heart-rate device_class, so there is none here. */
	{ RADIANT_FIELD_HEART_RATE,         "bpm",    NULL },

	{ RADIANT_FIELD_ENERGY,             "J",      "energy" },
	{ RADIANT_FIELD_DISTANCE,           "m",      "distance" },
	{ RADIANT_FIELD_DURATION,           "s",      "duration" },
	/* Beat count and revolutions are dimensionless counters. */
	{ RADIANT_FIELD_EVENT_COUNT,        NULL,     NULL },
	{ RADIANT_FIELD_REVOLUTIONS,        NULL,     NULL },
};

static const struct ha_row *ha_row_for(uint8_t field_type)
{
	size_t i;

	for (i = 0u; i < ARRAY_SIZE(ha_table); i++) {
		if (ha_table[i].field_type == field_type) {
			return &ha_table[i];
		}
	}
	return NULL;
}

/* ── Client state ──────────────────────────────────────────────────────────── */

static struct mqtt_client   client;
static struct sockaddr_in6  broker;
static uint8_t              rx_buffer[256];
static uint8_t              tx_buffer[512];
/* Statically initialised, not k_mutex_init()'d from a thread: publish() runs
 * on the pump thread and the keepalive thread runs on its own, and whichever
 * of the two the scheduler starts first must find a usable mutex. */
static K_MUTEX_DEFINE(lock);
static bool                 connected;
static bool                 online_published;

/* Publishing is a JSON assembly job and the buffer is not small; it is static
 * and mutex-protected rather than on a stack, because the pump thread's stack
 * would otherwise have to carry a discovery message's worth of it. */
static char work_topic[TOPIC_MAX];
static char work_payload[512];

static struct {
	uint32_t published;
	uint32_t publish_failed;
	uint32_t connect_attempts;
	uint32_t connect_failed;
} mqtt_stats;

/*
 * Which (source, field_id) pairs have had their retained discovery message
 * sent. One bit per field, cleared for a source when its binding changes, so a
 * re-pair re-announces.
 *
 * SECTION 9.2 SAYS DISCOVERY IS "GENERATED FROM binding_changed", AND THIS IS
 * A DEVIATION FROM THAT WORDING - recorded here rather than quietly done. The
 * reason is that a binding does not know which fields its source will produce:
 * struct radiant_binding carries devnum/devtype/trans_type, and field_type
 * arrives with the first sample. Announcing at binding time would mean
 * hardcoding "an 0x78 binding produces these three fields", which is exactly
 * the per-profile knowledge the adapter layer exists to keep out of the sinks.
 * So binding_changed() ARMS the announcement (and publishes the per-binding
 * availability topic, which it does know) and the first sample of each field
 * emits it.
 */
static uint32_t announced[RADIANT_BINDING_MAX];

/* ── Connection ────────────────────────────────────────────────────────────── */

static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("CONNACK refused: %d", evt->result);
			connected = false;
		} else {
			connected = true;
			LOG_INF("connected to broker %s:%d",
				CONFIG_ANT_DONGLE_MQTT_BROKER,
				CONFIG_ANT_DONGLE_MQTT_PORT);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		LOG_WRN("broker disconnected: %d", evt->result);
		connected = false;
		online_published = false;
		break;
	default:
		break;
	}
}

/* Caller holds `lock`. */
static int publish_raw(const char *topic, const char *payload, bool retain)
{
	struct mqtt_publish_param p;

	memset(&p, 0, sizeof(p));
	p.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	p.message.topic.topic.utf8 = (const uint8_t *)topic;
	p.message.topic.topic.size = strlen(topic);
	p.message.payload.data = (uint8_t *)payload;
	p.message.payload.len = strlen(payload);
	p.message_id = 0u;
	p.dup_flag = 0u;
	p.retain_flag = retain ? 1u : 0u;

	return mqtt_publish(&client, &p);
}

/* Caller holds `lock`. Returns true if the client is usable afterwards. */
static bool ensure_connected(void)
{
	static struct mqtt_utf8 will_message;
	static struct mqtt_topic will_topic;
	int rc;
	int waited_ms;

	if (connected) {
		return true;
	}

	if (bridge_status_topic[0] == '\0') {
		(void)snprintf(bridge_status_topic, sizeof(bridge_status_topic),
			       "radiant/%s/status",
			       CONFIG_ANT_DONGLE_MQTT_BRIDGE_ID);
	}

	mqtt_stats.connect_attempts++;

	memset(&broker, 0, sizeof(broker));
	broker.sin6_family = AF_INET6;
	broker.sin6_port = htons(CONFIG_ANT_DONGLE_MQTT_PORT);
	if (zsock_inet_pton(AF_INET6, CONFIG_ANT_DONGLE_MQTT_BROKER,
			    &broker.sin6_addr) != 1) {
		/* Loud, and it stays loud: an unparseable broker address means
		 * this sink will publish NOTHING for the life of the image,
		 * and a silent version of that is indistinguishable from "no
		 * sensors in range". Section 9.0a's DNS-SD discovery is
		 * deferred, so a hostname here is not a supported input, not
		 * merely an unimplemented one. */
		LOG_ERR("CONFIG_ANT_DONGLE_MQTT_BROKER='%s' is not a numeric "
			"IPv6 address - this sink will publish nothing",
			CONFIG_ANT_DONGLE_MQTT_BROKER);
		mqtt_stats.connect_failed++;
		return false;
	}

	mqtt_client_init(&client);

	client.broker = &broker;
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = (const uint8_t *)CONFIG_ANT_DONGLE_MQTT_BRIDGE_ID;
	client.client_id.size = strlen(CONFIG_ANT_DONGLE_MQTT_BRIDGE_ID);
	client.password = NULL;
	client.user_name = NULL;
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	/*
	 * The LWT, section 9.3's first of three availability levels. It is set
	 * at connect time because that is the only time a broker will accept
	 * it - a will registered after the CONNECT is not a will. The bridge
	 * can be up while a strap is gone (that is the per-binding status
	 * topic) and a strap can be present while one field has stopped
	 * advancing (that is expire_after); none of the three substitutes for
	 * another.
	 */
	will_topic.topic.utf8 = (const uint8_t *)bridge_status_topic;
	will_topic.topic.size = strlen(bridge_status_topic);
	will_topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	will_message.utf8 = (const uint8_t *)"offline";
	will_message.size = 7u;
	client.will_topic = &will_topic;
	client.will_message = &will_message;
	client.will_retain = 1u;

	rc = mqtt_connect(&client);
	if (rc != 0) {
		LOG_WRN("mqtt_connect: %d (will retry on the next sample)", rc);
		mqtt_stats.connect_failed++;
		return false;
	}

	/*
	 * Wait for the CONNACK, bounded. mqtt_connect() only opens the socket
	 * and sends CONNECT; `connected` is set by the event handler, which
	 * runs from mqtt_input(). Bounded rather than K_FOREVER because this
	 * runs on the pump thread: an unbounded wait here would stop the bus
	 * being drained for as long as a broker is unreachable, and the ring
	 * would fill and start dropping.
	 */
	for (waited_ms = 0; waited_ms < 5000 && !connected; waited_ms += 50) {
		(void)mqtt_input(&client);
		if (connected) {
			break;
		}
		k_sleep(K_MSEC(50));
	}

	if (!connected) {
		LOG_WRN("no CONNACK within 5 s");
		(void)mqtt_abort(&client);
		mqtt_stats.connect_failed++;
		return false;
	}

	if (!online_published) {
		(void)publish_raw(bridge_status_topic, "online", true);
		online_published = true;
	}

	return true;
}

/* ── Discovery ─────────────────────────────────────────────────────────────── */

/*
 * expire_after (section 9.2): "the sparse heartbeat interval, or the channel
 * period x 3". This is the ONE key section 9.2 singles out as the one to get
 * right, because it is the MQTT expression of the envelope's rule that "no
 * data" must never be produced silently - and without it a strap that walks
 * out of range leaves its last reading on screen indefinitely, which for a
 * heart-rate-driven AC is not a stale number, it is a wrong actuator.
 *
 * The same 3x rule radiant_liveness.c applies, from the same
 * struct radiant_binding::period, so the two cannot disagree. Rounded UP to a
 * whole second, and never to zero: HA reads expire_after 0 as "never expire",
 * so rounding 750 ms down would turn the one key that matters into its exact
 * opposite. A binding with no period gets no expire_after key at all rather
 * than a guessed one.
 */
static uint32_t expire_after_s(uint16_t period_32k)
{
	uint64_t us;

	if (period_32k == 0u) {
		return 0u;
	}
	us = ((uint64_t)period_32k * 3u * 15625u) / 512u;
	return (uint32_t)((us + 999999u) / 1000000u);
}

static void publish_discovery(uint32_t source, const struct radiant_sample *s)
{
	const struct radiant_binding *b = radiant_binding_get(source);
	const struct ha_row *row;
	char uuid[17];
	char avail[TOPIC_MAX];
	char state[TOPIC_MAX];
	uint32_t expire;
	int n;

	if (b == NULL) {
		return;
	}

	uuid_str(b->uuid, uuid, sizeof(uuid));
	row = ha_row_for(s->field_type);
	expire = expire_after_s(b->period);

	(void)snprintf(avail, sizeof(avail), "radiant/%s/%s/status",
		       CONFIG_ANT_DONGLE_MQTT_BRIDGE_ID, uuid);
	(void)snprintf(state, sizeof(state), "radiant/%s/%s/%u",
		       CONFIG_ANT_DONGLE_MQTT_BRIDGE_ID, uuid, s->field_id);

	(void)snprintf(work_topic, sizeof(work_topic),
		       "homeassistant/sensor/%s_%u/config", uuid, s->field_id);

	n = snprintf(work_payload, sizeof(work_payload),
		     "{\"name\":\"%s %u\","
		     "\"unique_id\":\"%s_%u\","
		     "\"state_topic\":\"%s\","
		     "\"availability_topic\":\"%s\","
		     "\"payload_available\":\"online\","
		     "\"payload_not_available\":\"offline\","
		     /* state_class: total_increasing if the accumulate bit is
		      * set, else measurement - section 9.2's table, verbatim.
		      * The bit is the vocabulary's, so this needs no per-field
		      * knowledge. */
		     "\"state_class\":\"%s\"",
		     (b->label[0] != '\0') ? b->label : "RadiANT",
		     s->field_id, uuid, s->field_id, state, avail,
		     ((s->flags & RADIANT_SAMPLE_ACCUMULATING) != 0u)
			     ? "total_increasing"
			     : "measurement");

	if (row != NULL && row->unit != NULL && n > 0 &&
	    n < (int)sizeof(work_payload)) {
		n += snprintf(work_payload + n, sizeof(work_payload) - n,
			      ",\"unit_of_measurement\":\"%s\"", row->unit);
	}
	if (row != NULL && row->device_class != NULL && n > 0 &&
	    n < (int)sizeof(work_payload)) {
		n += snprintf(work_payload + n, sizeof(work_payload) - n,
			      ",\"device_class\":\"%s\"", row->device_class);
	}
	if (expire != 0u && n > 0 && n < (int)sizeof(work_payload)) {
		n += snprintf(work_payload + n, sizeof(work_payload) - n,
			      ",\"expire_after\":%u", expire);
	}
	/* suggested_display_precision from the field's own exp: a sample with
	 * exp -2 is hundredths, so two decimals. Positive exponents carry no
	 * fractional digits at all. */
	if (n > 0 && n < (int)sizeof(work_payload)) {
		n += snprintf(work_payload + n, sizeof(work_payload) - n,
			      ",\"suggested_display_precision\":%d",
			      (s->exp < 0) ? -s->exp : 0);
	}
	if (n > 0 && n < (int)sizeof(work_payload) - 2) {
		work_payload[n++] = '}';
		work_payload[n] = '\0';
		(void)publish_raw(work_topic, work_payload, true);
	} else {
		/* Truncated rather than malformed: publishing a half-written
		 * JSON object would leave a broken retained message on the
		 * broker that survives a reboot of this device. */
		LOG_ERR("discovery message for source %u field %u did not fit",
			source, s->field_id);
	}
}

/* ── The sink ──────────────────────────────────────────────────────────────── */

static bool mqtt_want(const struct radiant_sample *s)
{
	/* The bus's own drop counter is diagnostics, not telemetry, and a
	 * broker is not where it belongs; it also has no binding and therefore
	 * no uuid to build a topic from. */
	return s->source < RADIANT_BINDING_MAX;
}

static void mqtt_publish_sample(const struct radiant_sample *s)
{
	const struct radiant_binding *b;
	char uuid[17];

	k_mutex_lock(&lock, K_FOREVER);

	if (!ensure_connected()) {
		k_mutex_unlock(&lock);
		return;
	}

	b = radiant_binding_get(s->source);
	if (b == NULL) {
		k_mutex_unlock(&lock);
		return;
	}

	if (s->field_id < 32u &&
	    (announced[s->source] & BIT(s->field_id)) == 0u) {
		publish_discovery(s->source, s);
		announced[s->source] |= BIT(s->field_id);
	}

	uuid_str(b->uuid, uuid, sizeof(uuid));

	/*
	 * STALE is carried as the per-sensor availability topic going
	 * "offline", not as a value. Section 9.3's three levels again: the
	 * value topic keeps the last reading (HA's expire_after is what ages
	 * it out on its own), while availability is how the bridge SAYS the
	 * sensor is gone rather than leaving it to a timeout that the broker
	 * and HA have to agree about.
	 */
	if ((s->flags & RADIANT_SAMPLE_STALE) != 0u) {
		(void)snprintf(work_topic, sizeof(work_topic),
			       "radiant/%s/%s/status",
			       CONFIG_ANT_DONGLE_MQTT_BRIDGE_ID, uuid);
		if (publish_raw(work_topic, "offline", true) == 0) {
			mqtt_stats.published++;
		} else {
			mqtt_stats.publish_failed++;
		}
		k_mutex_unlock(&lock);
		return;
	}

	(void)snprintf(work_topic, sizeof(work_topic), "radiant/%s/%s/%u",
		       CONFIG_ANT_DONGLE_MQTT_BRIDGE_ID, uuid, s->field_id);

	/*
	 * value_SI = raw * 10^exp (radiant_bridge.h). Emitted as an integer
	 * mantissa and an exponent applied here rather than as a float: this
	 * image has no FP printf, and a hand-rolled fixed-point formatter is
	 * how a bridge starts publishing 6.9999999. Non-negative exponents are
	 * the rare case and are expanded; negative ones become a decimal
	 * point, which is what HA parses.
	 */
	if (s->exp == 0) {
		(void)snprintf(work_payload, sizeof(work_payload), "%lld",
			       (long long)s->raw);
	} else if (s->exp < 0) {
		int      digits = -s->exp;
		int64_t  scale = 1;
		long long whole;
		long long frac;
		int      i;

		for (i = 0; i < digits && i < 18; i++) {
			scale *= 10;
		}
		whole = (long long)(s->raw / scale);
		frac = (long long)(s->raw % scale);
		if (frac < 0) {
			frac = -frac;
		}
		(void)snprintf(work_payload, sizeof(work_payload),
			       "%s%lld.%0*lld",
			       (s->raw < 0 && whole == 0) ? "-" : "", whole,
			       digits, frac);
	} else {
		int64_t v = s->raw;
		int     i;

		for (i = 0; i < s->exp && i < 18; i++) {
			v *= 10;
		}
		(void)snprintf(work_payload, sizeof(work_payload), "%lld",
			       (long long)v);
	}

	if (publish_raw(work_topic, work_payload, false) == 0) {
		mqtt_stats.published++;
	} else {
		mqtt_stats.publish_failed++;
	}

	k_mutex_unlock(&lock);
}

static void mqtt_binding_changed(uint32_t source, const struct radiant_binding *b)
{
	char uuid[17];

	if (source >= RADIANT_BINDING_MAX) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);

	/* Re-arm discovery for every field of this source: a re-pair gets a
	 * new uuid (radiant_binding.h says so explicitly), so its retained
	 * discovery messages are new topics and must be sent again. */
	announced[source] = 0u;

	if (b != NULL && connected) {
		uuid_str(b->uuid, uuid, sizeof(uuid));
		(void)snprintf(work_topic, sizeof(work_topic),
			       "radiant/%s/%s/status",
			       CONFIG_ANT_DONGLE_MQTT_BRIDGE_ID, uuid);
		(void)publish_raw(work_topic, "online", true);
	}

	k_mutex_unlock(&lock);
}

RADIANT_SINK_DEFINE(radiant_mqtt_sink, mqtt_want, mqtt_publish_sample,
		    mqtt_binding_changed);

/* ── Keepalive ─────────────────────────────────────────────────────────────── */

/*
 * mqtt_live() must be called often enough to send a PINGREQ inside the
 * keepalive interval, and mqtt_input() is what lets the client see the PINGRESP
 * and any DISCONNECT. Neither can hang off publish(): a bridge whose sensors
 * have all walked away stops publishing entirely, and that is exactly when a
 * broker would otherwise drop the connection for missing pings - so the
 * quietest case would be the one that breaks.
 *
 * Same priority as the pump, below the ANT host thread, for the same reason.
 */
#define MQTT_KEEPALIVE_STACK 1536

static void mqtt_keepalive_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		static uint32_t passes;

		k_sleep(K_MSEC(500));

		k_mutex_lock(&lock, K_FOREVER);
		if (connected) {
			(void)mqtt_live(&client);
			(void)mqtt_input(&client);
		}

		/*
		 * Once a minute, on the same console everything else in this
		 * image reports to. Counters that are only ever incremented
		 * are counters nobody reads, and "the bridge published
		 * nothing" needs to be distinguishable from "the bridge
		 * published and every publish failed" without a debugger -
		 * which is the situation on every board but this one.
		 */
		if ((++passes % 120u) == 0u) {
			LOG_INF("mqtt: %s, published=%u failed=%u "
				"connects=%u/%u attempted",
				connected ? "connected" : "disconnected",
				mqtt_stats.published, mqtt_stats.publish_failed,
				mqtt_stats.connect_attempts -
					mqtt_stats.connect_failed,
				mqtt_stats.connect_attempts);
		}
		k_mutex_unlock(&lock);
	}
}

K_THREAD_DEFINE(mqtt_keepalive_tid, MQTT_KEEPALIVE_STACK, mqtt_keepalive_fn,
		NULL, NULL, NULL, ANTR_HOST_THREAD_PRIORITY + 2, 0, 0);
