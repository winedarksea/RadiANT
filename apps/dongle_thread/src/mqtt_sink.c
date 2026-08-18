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
#include "radiant_naming.h"

#include "bridge_app.h"

LOG_MODULE_REGISTER(mqtt_sink, LOG_LEVEL_INF);

/* ── Topics (section 9.1) ──────────────────────────────────────────────────── */

/*
 * radiant/<bridge_id>/status                     retained, LWT
 * radiant/<bridge_id>/<binding_uuid>/status      retained, per-sensor liveness
 * radiant/<bridge_id>/<binding_uuid>/<field_key> the value ("d"-prefixed when
 *                                                the sample is derived - see
 *                                                field_key() below)
 *
 * binding_uuid, NEVER a device number. Section 5 is unambiguous about this and
 * gives three reasons: a device number is 16 bits, it is re-rollable by
 * design, and a collision across two straps in one room is a one-in-65535
 * event that will happen at a trade show. An MQTT topic has to survive a
 * battery change. There is no path in this file from a struct radiant_binding
 * to a topic that reads ::devnum, and that is the property to preserve.
 *
 * THE DISCOVERY *NAME* DOES CARRY THE DEVICE NUMBER, and that is not a
 * loosening of the rule above: a name is what a human reads on a card in Home
 * Assistant and the number is printed on the strap, while a topic is an
 * identity that automations and history are keyed on. A re-rolled device
 * number changes the displayed name and nothing else. See radiant_naming.h.
 */
#define TOPIC_MAX 96

static char bridge_status_topic[TOPIC_MAX];

static void uuid_str(uint64_t uuid, char *out, size_t out_len)
{
	(void)snprintf(out, out_len, "%08x%08x", (unsigned int)(uuid >> 32),
		       (unsigned int)(uuid & 0xFFFFFFFFu));
}

/*
 * ── The field key, and why it is not just the field id ──────────────────────
 *
 * A field id is unique per source ONLY within one producer. radiant_bridge.h
 * allocates 0x00-0x1F to whichever profile adapter owns the source and
 * 0x20-0x3F to the common-page adapter - but radiant_rules.c posts its derived
 * booleans onto THE SAME SOURCE starting from id 0 (RADIANT_RULE_FIELD_WORN),
 * straight over the top of the profile block.
 *
 * MEASURED ON REAL HARDWARE, nine collisions in one 210 s capture:
 *
 *   heart-rate strap #8265, id 0x00 = computed bpm AND "worn"
 *                           id 0x01 = beat count  AND "active"
 *                           id 0x02 = beat time   AND "at rest"
 *   FE-C trainer #52233,    id 0x00 = active power AND "worn"
 *
 * Keyed on the id alone, each of those pairs shared one state topic, one
 * unique_id and one bit of `announced` - so a strap published its bpm and its
 * worn boolean to the same topic, alternating a heart rate with a 0/1, and only
 * whichever arrived first was ever announced to Home Assistant.
 *
 * The derived samples are the ones that do not own the id space, so they are
 * the ones that move: they get a "d" prefix and everything else keeps the topic
 * it already had. THE MATTER PLANE NEEDS NO EQUIVALENT - radiant_matter.c maps
 * only OCCUPANCY, TEMPERATURE, HUMIDITY, PRESSURE and BATTERY_SOC, so no
 * endpoint is ever created for a profile-block id and (source, field_id) is
 * genuinely unique there.
 */
static void field_key(const struct radiant_sample *s, char *out, size_t out_len)
{
	(void)snprintf(out, out_len, "%s%u",
		       ((s->flags & RADIANT_SAMPLE_DERIVED) != 0u) ? "d" : "",
		       s->field_id);
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
	/*
	 * WIND TAKES THIS PLANE AND ONLY THIS PLANE (package D, finding 7).
	 * Common page 84 subpages 4 and 5 carry wind speed and direction, and
	 * the Matter data model has flow, pressure, humidity, temperature,
	 * occupancy and air-quality clusters and no wind anything - so
	 * radiant_matter.c gets no row for either and they surface here
	 * instead. That is the SECOND instance of section 1's rule, after heart
	 * rate below, and it is the same rule for the same reason: mapping a
	 * quantity onto a cluster that means something else is worse than not
	 * mapping it.
	 *
	 * Speed already had a row (it is a bicycle speed as much as a wind
	 * speed). Direction is an angle in radians, and HA has no
	 * wind-direction device_class in the canonical list - the schema's
	 * `wind_direction` is degrees-only - so it goes out with a unit and no
	 * class, per this table's own "a guess costs the entity" rule.
	 */
	{ RADIANT_FIELD_SPEED,              "m/s",    "speed" },
	{ RADIANT_FIELD_ANGLE,              "rad",    NULL },
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
/*
 * 64 BITS, NOT 32, AND THE WIDTH IS THE WHOLE POINT. radiant_bridge.h allocates
 * field ids 0x00-0x1F to the profile adapter and 0x20-0x3F to the common-page
 * adapter, so the id space is exactly 64 wide. This was a uint32_t guarded by
 * `field_id < 32`, which silently announced nothing at all for the entire
 * common-page block: page 84's temperature, humidity and pressure published
 * their values to topics Home Assistant had never been told about, so no entity
 * ever appeared for them and the values were discarded at the broker. Nothing
 * failed and nothing logged - the guard just skipped, which is why a bitmap
 * narrower than its index needs to be impossible rather than merely correct
 * today. The BUILD_ASSERT below is what makes it impossible.
 */
static uint64_t announced[RADIANT_BINDING_MAX];

/* The derived booleans re-use the profile block's ids (see field_key()), so
 * they need their own plane or one would mask the other. */
static uint64_t announced_derived[RADIANT_BINDING_MAX];

#ifdef CONFIG_ANT_DONGLE_NAMING_DEBUG
/* The same two planes again, for the bench log rather than the broker - so a
 * payload is printed once per key instead of at the sample rate. */
static uint64_t logged[RADIANT_BINDING_MAX];
static uint64_t logged_derived[RADIANT_BINDING_MAX];
#endif

BUILD_ASSERT((RADIANT_FIELD_ID_COMMON_MAX + 1u) <= (sizeof(announced[0]) * 8u),
	     "the announce bitmap is narrower than the field id space it indexes");

/*
 * The FE-C equipment type per source, for radiant_naming_format(). One byte,
 * fed from the bus by publish() below rather than read out of the binding
 * table: it is a fact learned from traffic, not one read off the channel at
 * bind time - see radiant_naming.h.
 *
 * A RE-ANNOUNCE IS WHAT MAKES A LATE ARRIVAL VISIBLE HERE. Discovery is
 * retained and sent once per (source, field), so an entity announced before
 * the first page 16 keeps the "Fitness Equip" name until something re-arms
 * `announced`. That is what the subtype-changed branch in publish() does, and
 * it is the MQTT analogue of ant_bridge.cpp's labelDirty.
 *
 * Field id 0x0D is RADIANT_POWER_FIELD_FEC_TYPE, hardcoded with the citation
 * rather than by including radiant_power_adapter.h - the same line
 * radiant_rules.c draws for the FE state code, and this file is a sink, which
 * is even further from the page codecs.
 */
static uint8_t subtype[RADIANT_BINDING_MAX];

#define FEC_TYPE_FIELD_ID 0x0Du
#define FEC_DEVICE_TYPE   0x11u

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

/*
 * ── The reconnect backoff, and the bug it exists for ────────────────────────
 *
 * The CONNACK wait below is bounded, and its comment says why: this runs on the
 * bridge's drain thread, so "an unbounded wait here would stop the bus being
 * drained for as long as a broker is unreachable, and the ring would fill and
 * start dropping". That reasoning was right and the guard was in the wrong
 * place - mqtt_connect() ITSELF blocks, doing the TCP handshake synchronously,
 * and against an unreachable broker it returns -ETIMEDOUT only when the stack
 * gives up.
 *
 * MEASURED ON THIS BENCH with no Thread parent and the placeholder broker
 * address: one attempt per sample, each taking 3.0 s, forever. Sensors were
 * producing roughly eight samples a second, so the bus drained at about a
 * thirtieth of its input and the ring dropped the rest - precisely the outcome
 * the bounded wait was written to prevent, reached one line above it.
 *
 * IT IS NOT AN MQTT-ONLY FAULT, WHICH IS WHY IT MATTERS. Sinks are drained
 * serially on one thread, so a stalled MQTT sink also starves the Matter sink
 * and radiant_rules.c. In a 240 s capture with a real strap on air, not one
 * derived occupancy boolean was ever posted: the rule never saw a coherent
 * accumulating series because most of them had been dropped.
 *
 * So a failed attempt now costs one comparison per sample instead of a TCP
 * timeout, and attempts are spaced out to a cap. A broker that is simply not
 * there yet - the normal state during Thread attach - must not be able to hold
 * the whole bus down.
 */
#define MQTT_BACKOFF_MIN_MS 5000
#define MQTT_BACKOFF_MAX_MS 300000

static int64_t mqtt_next_attempt_ms;
static uint32_t mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;

/* Caller holds `lock`. Doubling rather than fixed, so a broker that is down for
 * an hour costs a handful of stalls rather than 720 of them. */
static void arm_backoff(void)
{
	mqtt_next_attempt_ms = k_uptime_get() + (int64_t)mqtt_backoff_ms;
	if (mqtt_backoff_ms < MQTT_BACKOFF_MAX_MS) {
		mqtt_backoff_ms *= 2u;
		if (mqtt_backoff_ms > MQTT_BACKOFF_MAX_MS) {
			mqtt_backoff_ms = MQTT_BACKOFF_MAX_MS;
		}
	}
}

static void clear_backoff(void)
{
	mqtt_next_attempt_ms = 0;
	mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
}

/* Caller holds `lock`. Returns true if the client is usable afterwards. */
static bool ensure_connected(void)
{
	static struct mqtt_utf8 will_message;
	static struct mqtt_topic will_topic;
	int rc;
	int waited_ms;
	int64_t now;

	if (connected) {
		return true;
	}

	/* Cheap refusal on the drain thread. Never blocks. */
	now = k_uptime_get();
	if (mqtt_next_attempt_ms != 0 && now < mqtt_next_attempt_ms) {
		return false;
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
		/* Straight to the cap: a misconfigured address cannot start
		 * working, so retrying it on a short interval buys nothing. */
		mqtt_backoff_ms = MQTT_BACKOFF_MAX_MS;
		arm_backoff();
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
		mqtt_stats.connect_failed++;
		arm_backoff();
		LOG_WRN("mqtt_connect: %d (next attempt in %u ms)", rc,
			mqtt_backoff_ms);
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
		(void)mqtt_abort(&client);
		mqtt_stats.connect_failed++;
		arm_backoff();
		LOG_WRN("no CONNACK within 5 s (next attempt in %u ms)",
			mqtt_backoff_ms);
		return false;
	}

	clear_backoff();

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

/*
 * Fills work_topic and work_payload with the retained discovery message, and
 * publishes nothing. Split out from publish_discovery() so that a bench build
 * can SEE the payload: this sink cannot reach a broker without a Thread network
 * and a border router, so until now the only evidence for the JSON's content
 * was that the code appeared to build it correctly. Caller holds `lock`.
 */
static bool compose_discovery(uint32_t source, const struct radiant_sample *s)
{
	const struct radiant_binding *b = radiant_binding_get(source);
	const struct ha_row *row;
	char name[RADIANT_NAMING_MAX];
	char uuid[17];
	char key[8];
	char avail[TOPIC_MAX];
	char state[TOPIC_MAX];
	uint32_t expire;
	int n;

	if (b == NULL) {
		return false;
	}

	uuid_str(b->uuid, uuid, sizeof(uuid));
	field_key(s, key, sizeof(key));
	row = ha_row_for(s->field_type);

	/*
	 * NO expire_after ON A DERIVED OR ROTATED FIELD, and both halves are
	 * corrections made against a measurement.
	 *
	 * expire_after_s() is the channel period x 3 - "three consecutive misses
	 * and the reading is stale" - which is right for a series that
	 * republishes at the channel rate. radiant_rules.c publishes its
	 * booleans ONLY WHEN THE DEBOUNCED STATE CHANGES, which on a worn strap
	 * is minutes apart and on a resting one is never.
	 *
	 * Observed in the composed payload on this bench: every derived entity
	 * carried "expire_after":1, so Home Assistant would have marked "Worn",
	 * "Active" and "At Rest" unavailable one second after each change and
	 * left them there - the four endpoints the whole naming change exists to
	 * make usable, permanently greyed out.
	 *
	 * Omitting the key is the honest answer rather than guessing a longer
	 * one: section 9.3's SECOND level already covers "this sensor is gone"
	 * through the per-binding availability topic, which is published from
	 * RADIANT_SAMPLE_STALE and does not depend on a field's update rate.
	 *
	 * ROTATED is the same mistake in a second disguise, and it took a real
	 * FE-C trainer to see it. A field carried on an interleaved page repeats
	 * at the ROTATION's rate, not the channel's: the Wahoo's speed, FE state
	 * and equipment type each arrive about once a second on a 4 Hz channel,
	 * and a common page as rarely as one message in 121. Every one of them
	 * was published with "expire_after":1, so Home Assistant would have
	 * greyed out the trainer's entire entity set between its own pages,
	 * forever. The channel period says nothing about these fields and the
	 * decoder is the only layer that knows which page a value came off,
	 * which is why the flag is set there (radiant_bridge.h).
	 */
	expire = ((s->flags & (RADIANT_SAMPLE_DERIVED | RADIANT_SAMPLE_ROTATED))
		  != 0u)
			 ? 0u
			 : expire_after_s(b->period);

	(void)snprintf(avail, sizeof(avail), "radiant/%s/%s/status",
		       CONFIG_ANT_DONGLE_MQTT_BRIDGE_ID, uuid);
	(void)snprintf(state, sizeof(state), "radiant/%s/%s/%s",
		       CONFIG_ANT_DONGLE_MQTT_BRIDGE_ID, uuid, key);

	(void)snprintf(work_topic, sizeof(work_topic),
		       "homeassistant/sensor/%s_%s/config", uuid, key);

	/*
	 * The entity name. This used to be `label` or the literal "RadiANT",
	 * followed by the raw field id - so a Home Assistant user saw
	 * "RadiANT 4" and had five of them for one heart-rate strap, all
	 * indistinguishable. The composition lives in radiant_naming.c, shared
	 * with the Matter bridge, so the two planes cannot drift into naming
	 * the same endpoint two different things.
	 */
	(void)radiant_naming_format(name, sizeof(name), b->devtype, b->devnum,
				    subtype[source], b->label, s->field_type,
				    s->field_id);

	n = snprintf(work_payload, sizeof(work_payload),
		     "{\"name\":\"%s\","
		     "\"unique_id\":\"%s_%s\","
		     "\"state_topic\":\"%s\","
		     "\"availability_topic\":\"%s\","
		     "\"payload_available\":\"online\","
		     "\"payload_not_available\":\"offline\","
		     /* state_class: total_increasing if the accumulate bit is
		      * set, else measurement - section 9.2's table, verbatim.
		      * The bit is the vocabulary's, so this needs no per-field
		      * knowledge. */
		     "\"state_class\":\"%s\"",
		     name, uuid, key, state, avail,
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
		return true;
	}

	/* Truncated rather than malformed: publishing a half-written JSON
	 * object would leave a broken retained message on the broker that
	 * survives a reboot of this device. */
	LOG_ERR("discovery message for source %u field %u did not fit", source,
		s->field_id);
	return false;
}

/* Caller holds `lock`. */
static void publish_discovery(uint32_t source, const struct radiant_sample *s)
{
	if (compose_discovery(source, s)) {
		(void)publish_raw(work_topic, work_payload, true);
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

#ifdef CONFIG_ANT_DONGLE_NAMING_DEBUG
	/*
	 * BEFORE the connection gate, deliberately. This sink needs a Thread
	 * network and a border router to reach a broker, neither of which exists
	 * on a bench, so everything below ensure_connected() is unreachable here
	 * and the discovery JSON had never once been looked at on real hardware.
	 * Logging it costs one compose per (source, field key) and is compiled
	 * out of every shipping image.
	 */
	if (s->field_id <= RADIANT_FIELD_ID_COMMON_MAX) {
		uint64_t *shown = ((s->flags & RADIANT_SAMPLE_DERIVED) != 0u)
					  ? &logged_derived[s->source]
					  : &logged[s->source];

		if ((*shown & BIT64(s->field_id)) == 0u &&
		    radiant_binding_get(s->source) != NULL) {
			if (compose_discovery(s->source, s)) {
				*shown |= BIT64(s->field_id);
				LOG_INF("discovery %s", work_topic);
				LOG_INF("  %s", work_payload);
			}
		}
	}
#endif

	if (!ensure_connected()) {
		k_mutex_unlock(&lock);
		return;
	}

	b = radiant_binding_get(s->source);
	if (b == NULL) {
		k_mutex_unlock(&lock);
		return;
	}

	/*
	 * The equipment type, cached for the discovery name. Gated on the
	 * binding's device type as well as the field id, because ids 0x00-0x1F
	 * belong to whichever profile adapter owns the source
	 * (radiant_bridge.h's allocation block) - 0x0D is only "equipment type"
	 * on an FE-C binding.
	 *
	 * A CHANGE RE-ARMS DISCOVERY for this source, which is the whole point:
	 * the retained config message is sent once, so an entity announced
	 * before the first page 16 would otherwise carry the generic name
	 * forever. Only on a change - the value repeats at 4 Hz, and
	 * re-publishing a retained config four times a second would be a
	 * broker-side storm.
	 */
	if (s->field_type == RADIANT_FIELD_ENUM_GENERIC &&
	    s->field_id == FEC_TYPE_FIELD_ID && b->devtype == FEC_DEVICE_TYPE &&
	    s->raw >= 0 && s->raw <= (int64_t)UINT8_MAX &&
	    subtype[s->source] != (uint8_t)s->raw) {
		subtype[s->source] = (uint8_t)s->raw;
		announced[s->source] = 0u;
		announced_derived[s->source] = 0u;
#ifdef CONFIG_ANT_DONGLE_NAMING_DEBUG
		/* Re-arm the bench log too, or it shows only the pre-page-16
		 * "Fitness Equip" payload and never the corrected one - which
		 * would hide the very mechanism being demonstrated. */
		logged[s->source] = 0u;
		logged_derived[s->source] = 0u;
#endif
	}

	/* Two planes, for the same reason field_key() has a "d" prefix: a
	 * derived boolean and a profile field can carry the same id, and one
	 * bitmap would announce whichever arrived first and silently skip the
	 * other for the life of the binding. */
	if (s->field_id <= RADIANT_FIELD_ID_COMMON_MAX) {
		uint64_t *bits = ((s->flags & RADIANT_SAMPLE_DERIVED) != 0u)
					 ? &announced_derived[s->source]
					 : &announced[s->source];

		if ((*bits & BIT64(s->field_id)) == 0u) {
			publish_discovery(s->source, s);
			*bits |= BIT64(s->field_id);
		}
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

	{
		char key[8];

		field_key(s, key, sizeof(key));
		(void)snprintf(work_topic, sizeof(work_topic), "radiant/%s/%s/%s",
			       CONFIG_ANT_DONGLE_MQTT_BRIDGE_ID, uuid, key);
	}

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
	announced_derived[source] = 0u;
#ifdef CONFIG_ANT_DONGLE_NAMING_DEBUG
	logged[source] = 0u;
	logged_derived[source] = 0u;
#endif
	/* And the slot may now hold a different physical machine, so the
	 * learned equipment type goes with it rather than naming a rower
	 * "Treadmill". */
	subtype[source] = 0u;

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
