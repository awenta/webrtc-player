/*
 * Poll Janus Admin telemetry and publish one status file per mountpoint.
 *
 * Cumulative counters and viewers are summed across active handles. Bitrate is
 * the sum of per-session/handle/MID byte rates measured with CLOCK_MONOTONIC;
 * new and reset counters only establish a baseline. RTT and remote jitter use
 * the worst maximum, while link and media-link quality use the worst minimum
 * across viewers and media. Janus reports jitter-local/jitter-remote in ms.
 * Missing non-cumulative metrics are published as blank values.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <curl/curl.h>
#include <jansson.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CHANNEL_COUNT 5
#define DEFAULT_ADMIN_URL "http://127.0.0.1:7088/admin"
#define DEFAULT_SECRET_FILE "/run/webrtc-player/janus-admin.secret"
#define MAX_ADMIN_URL 1024U
#define MAX_SECRET_SIZE 4096U
#define MAX_RESPONSE_SIZE (1024U * 1024U)
#define MAX_MID_SIZE 255U
#define REQUEST_TIMEOUT_MS 1800L
#define CONNECT_TIMEOUT_MS 500L

typedef struct CounterSample {
	uint64_t session_id;
	uint64_t handle_id;
	char mid[MAX_MID_SIZE + 1U];
	uint64_t bytes;
	struct timespec observed_at;
	struct CounterSample *next;
} CounterSample;

typedef struct {
	uint64_t viewers;
	uint64_t packets_sent;
	uint64_t bytes_sent;
	uint64_t nacks_received;
	uint64_t remote_lost;
	long double bitrate_bps;
	bool bitrate_valid;
	uint64_t rtt_ms;
	bool rtt_valid;
	uint64_t jitter_ms;
	bool jitter_valid;
	uint64_t link_quality;
	bool link_quality_valid;
	uint64_t media_link_quality;
	bool media_link_quality_valid;
} ChannelMetrics;

typedef struct {
	CURL *curl;
	struct curl_slist *headers;
	const char *secret;
	uint64_t transaction_id;
} AdminClient;

typedef struct {
	char *data;
	size_t length;
	size_t capacity;
} ResponseBuffer;

typedef struct {
	ChannelMetrics *channels;
	CounterSample *previous_samples;
	CounterSample *next_samples;
	uint64_t session_id;
	uint64_t handle_id;
	struct timespec observed_at;
} HandleAggregation;

static volatile sig_atomic_t stop_requested = 0;
static uint64_t temporary_file_id = 0;

static void handle_signal(int signal_number) {
	(void)signal_number;
	stop_requested = 1;
}

static bool install_signal_handlers(void) {
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = handle_signal;
	if(sigemptyset(&action.sa_mask) != 0)
		return false;
	return sigaction(SIGTERM, &action, NULL) == 0 &&
		sigaction(SIGINT, &action, NULL) == 0;
}

static void secure_clear(char *value) {
	volatile unsigned char *cursor = (volatile unsigned char *)value;

	if(value == NULL)
		return;
	while(*cursor != '\0') {
		*cursor = 0;
		cursor++;
	}
}

static char *read_secret_file(const char *path) {
	char buffer[MAX_SECRET_SIZE + 1U];
	char extra;
	char *secret = NULL;
	size_t length = 0U;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if(fd < 0)
		return NULL;
	while(length < MAX_SECRET_SIZE) {
		ssize_t count = read(fd, buffer + length, MAX_SECRET_SIZE - length);

		if(count < 0) {
			if(errno == EINTR)
				continue;
			(void)close(fd);
			return NULL;
		}
		if(count == 0)
			break;
		length += (size_t)count;
	}
	if(length == MAX_SECRET_SIZE) {
		ssize_t count;

		do {
			count = read(fd, &extra, 1U);
		} while(count < 0 && errno == EINTR);
		if(count != 0) {
			(void)close(fd);
			memset(buffer, 0, sizeof(buffer));
			return NULL;
		}
	}
	if(close(fd) != 0) {
		memset(buffer, 0, sizeof(buffer));
		return NULL;
	}
	while(length > 0U && (buffer[length - 1U] == '\n' || buffer[length - 1U] == '\r'))
		length--;
	if(length == 0U || memchr(buffer, '\0', length) != NULL) {
		memset(buffer, 0, sizeof(buffer));
		return NULL;
	}
	buffer[length] = '\0';
	secret = malloc(length + 1U);
	if(secret != NULL)
		memcpy(secret, buffer, length + 1U);
	memset(buffer, 0, sizeof(buffer));
	return secret;
}

static bool valid_port(const char *start, size_t length) {
	unsigned long port = 0UL;
	size_t index;

	if(length == 0U || length > 5U)
		return false;
	for(index = 0U; index < length; index++) {
		unsigned char character = (unsigned char)start[index];

		if(character < (unsigned char)'0' || character > (unsigned char)'9')
			return false;
		port = port * 10UL + (unsigned long)(character - (unsigned char)'0');
	}
	return port > 0UL && port <= 65535UL;
}

static bool validate_and_copy_admin_url(const char *input,
		char output[MAX_ADMIN_URL]) {
	static const char prefix[] = "http://";
	const char *authority;
	const char *authority_end;
	const char *path;
	const char *host_start;
	const char *host_end;
	const char *port_start = NULL;
	struct in_addr ipv4;
	struct in6_addr ipv6;
	char host[INET6_ADDRSTRLEN + 1U];
	size_t input_length;
	size_t host_length;
	bool loopback = false;

	if(input == NULL || strncmp(input, prefix, sizeof(prefix) - 1U) != 0)
		return false;
	input_length = strlen(input);
	if(input_length <= sizeof(prefix) - 1U || input_length >= MAX_ADMIN_URL ||
		strchr(input, '?') != NULL || strchr(input, '#') != NULL)
		return false;
	authority = input + sizeof(prefix) - 1U;
	path = strchr(authority, '/');
	authority_end = path != NULL ? path : input + input_length;
	if(authority == authority_end ||
		memchr(authority, '@', (size_t)(authority_end - authority)) != NULL)
		return false;

	if(*authority == '[') {
		const char *closing = memchr(authority, ']',
			(size_t)(authority_end - authority));

		if(closing == NULL)
			return false;
		host_start = authority + 1;
		host_end = closing;
		if(closing + 1 < authority_end) {
			if(closing[1] != ':')
				return false;
			port_start = closing + 2;
		} else if(closing + 1 != authority_end) {
			return false;
		}
	} else {
		const char *colon = memchr(authority, ':',
			(size_t)(authority_end - authority));

		host_start = authority;
		host_end = colon != NULL ? colon : authority_end;
		if(colon != NULL)
			port_start = colon + 1;
	}
	if(port_start != NULL &&
		!valid_port(port_start, (size_t)(authority_end - port_start)))
		return false;
	host_length = (size_t)(host_end - host_start);
	if(host_length == 0U || host_length >= sizeof(host))
		return false;
	memcpy(host, host_start, host_length);
	host[host_length] = '\0';
	if(inet_pton(AF_INET, host, &ipv4) == 1) {
		loopback = (ntohl(ipv4.s_addr) & UINT32_C(0xff000000)) ==
			UINT32_C(0x7f000000);
	} else if(inet_pton(AF_INET6, host, &ipv6) == 1) {
		static const unsigned char ipv6_loopback[16] = {
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
		};

		loopback = memcmp(ipv6.s6_addr, ipv6_loopback,
			sizeof(ipv6_loopback)) == 0;
	}
	if(!loopback)
		return false;

	memcpy(output, input, input_length + 1U);
	while(input_length > sizeof(prefix) - 1U && output[input_length - 1U] == '/') {
		output[input_length - 1U] = '\0';
		input_length--;
	}
	return true;
}

static size_t receive_response(char *data, size_t size, size_t count,
		void *userdata) {
	ResponseBuffer *response = userdata;
	size_t bytes;

	if(size != 0U && count > SIZE_MAX / size)
		return 0U;
	bytes = size * count;
	if(bytes > response->capacity - response->length)
		return 0U;
	memcpy(response->data + response->length, data, bytes);
	response->length += bytes;
	response->data[response->length] = '\0';
	return bytes;
}

static json_t *admin_request(AdminClient *client, const char *url,
		const char *operation) {
	ResponseBuffer response;
	json_t *request = NULL;
	json_t *root = NULL;
	char *body = NULL;
	char transaction[48];
	long http_status = 0L;
	CURLcode result;

	response.data = malloc(MAX_RESPONSE_SIZE + 1U);
	if(response.data == NULL)
		return NULL;
	response.length = 0U;
	response.capacity = MAX_RESPONSE_SIZE;
	response.data[0] = '\0';

	client->transaction_id++;
	if(snprintf(transaction, sizeof(transaction), "monitor-%" PRIu64,
			client->transaction_id) < 0)
		goto done;
	request = json_object();
	if(request == NULL ||
		json_object_set_new(request, "janus", json_string(operation)) != 0 ||
		json_object_set_new(request, "transaction", json_string(transaction)) != 0 ||
		json_object_set_new(request, "admin_secret", json_string(client->secret)) != 0)
		goto done;
	body = json_dumps(request, JSON_COMPACT);
	if(body == NULL)
		goto done;

	curl_easy_reset(client->curl);
	if(curl_easy_setopt(client->curl, CURLOPT_URL, url) != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_POST, 1L) != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_POSTFIELDS, body) != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_POSTFIELDSIZE_LARGE,
			(curl_off_t)strlen(body)) != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_HTTPHEADER, client->headers) != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, receive_response) != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &response) != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_CONNECTTIMEOUT_MS,
			CONNECT_TIMEOUT_MS) != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_TIMEOUT_MS, REQUEST_TIMEOUT_MS) != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_PROXY, "") != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_NOPROXY, "*") != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_PROTOCOLS_STR, "http") != CURLE_OK ||
		curl_easy_setopt(client->curl, CURLOPT_USERAGENT,
			"webrtc-player-janus-monitor/1") != CURLE_OK)
		goto done;

	result = curl_easy_perform(client->curl);
	if(result != CURLE_OK ||
		curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE, &http_status) != CURLE_OK ||
		http_status != 200L || response.length == 0U)
		goto done;
	root = json_loadb(response.data, response.length, JSON_REJECT_DUPLICATES, NULL);

done:
	free(body);
	json_decref(request);
	free(response.data);
	return root;
}

static bool janus_response_succeeded(json_t *root) {
	json_t *janus = json_is_object(root) ? json_object_get(root, "janus") : NULL;
	const char *value = json_string_value(janus);

	return value != NULL && strcmp(value, "success") == 0;
}

static bool janus_resource_disappeared(json_t *root) {
	json_t *error;
	json_t *code;
	json_int_t value;

	if(!json_is_object(root))
		return false;
	error = json_object_get(root, "error");
	code = json_is_object(error) ? json_object_get(error, "code") : NULL;
	if(!json_is_integer(code))
		return false;
	value = json_integer_value(code);
	return value == 458 || value == 459;
}

static bool json_uint64_value(json_t *value, uint64_t *output) {
	json_int_t integer;

	if(!json_is_integer(value))
		return false;
	integer = json_integer_value(value);
	if(integer < 0)
		return false;
	*output = (uint64_t)integer;
	return true;
}

static bool json_flag_value(json_t *value, bool *output) {
	if(json_is_true(value)) {
		*output = true;
		return true;
	}
	if(json_is_false(value)) {
		*output = false;
		return true;
	}
	if(json_is_integer(value)) {
		json_int_t integer = json_integer_value(value);

		if(integer == 0 || integer == 1) {
			*output = integer == 1;
			return true;
		}
	}
	return false;
}

static void add_counter(uint64_t *total, uint64_t value) {
	if(UINT64_MAX - *total < value)
		*total = UINT64_MAX;
	else
		*total += value;
}

static void update_maximum(uint64_t value, uint64_t *current, bool *valid) {
	if(!*valid || value > *current)
		*current = value;
	*valid = true;
}

static void update_minimum(uint64_t value, uint64_t *current, bool *valid) {
	if(!*valid || value < *current)
		*current = value;
	*valid = true;
}

static CounterSample *find_sample(CounterSample *samples, uint64_t session_id,
		uint64_t handle_id, const char *mid) {
	CounterSample *sample;

	for(sample = samples; sample != NULL; sample = sample->next) {
		if(sample->session_id == session_id && sample->handle_id == handle_id &&
			strcmp(sample->mid, mid) == 0)
			return sample;
	}
	return NULL;
}

static void free_samples(CounterSample *samples) {
	while(samples != NULL) {
		CounterSample *next = samples->next;

		free(samples);
		samples = next;
	}
}

static bool medium_identifier(json_t *medium, const char *fallback,
		char output[MAX_MID_SIZE + 1U]) {
	json_t *mid = json_object_get(medium, "mid");
	const char *mid_value = json_string_value(mid);
	json_t *mindex;
	int length;

	if(mid_value != NULL && mid_value[0] != '\0') {
		size_t size = strlen(mid_value);

		if(size > MAX_MID_SIZE)
			return false;
		memcpy(output, mid_value, size + 1U);
		return true;
	}
	mindex = json_object_get(medium, "mindex");
	if(json_is_integer(mindex)) {
		length = snprintf(output, MAX_MID_SIZE + 1U, "mindex:%" JSON_INTEGER_FORMAT,
			json_integer_value(mindex));
		return length > 0 && (size_t)length <= MAX_MID_SIZE;
	}
	if(fallback == NULL || fallback[0] == '\0' || strlen(fallback) > MAX_MID_SIZE)
		return false;
	memcpy(output, fallback, strlen(fallback) + 1U);
	return true;
}

static long double elapsed_seconds(const struct timespec *current,
		const struct timespec *previous) {
	return (long double)(current->tv_sec - previous->tv_sec) +
		(long double)(current->tv_nsec - previous->tv_nsec) / 1000000000.0L;
}

static bool record_byte_sample(HandleAggregation *aggregation,
		ChannelMetrics *metrics, const char *mid, uint64_t bytes) {
	CounterSample *old_sample;
	CounterSample *new_sample;

	if(find_sample(aggregation->next_samples, aggregation->session_id,
			aggregation->handle_id, mid) != NULL)
		return true;
	new_sample = calloc(1U, sizeof(*new_sample));
	if(new_sample == NULL)
		return false;
	new_sample->session_id = aggregation->session_id;
	new_sample->handle_id = aggregation->handle_id;
	memcpy(new_sample->mid, mid, strlen(mid) + 1U);
	new_sample->bytes = bytes;
	new_sample->observed_at = aggregation->observed_at;
	new_sample->next = aggregation->next_samples;
	aggregation->next_samples = new_sample;

	old_sample = find_sample(aggregation->previous_samples, aggregation->session_id,
		aggregation->handle_id, mid);
	if(old_sample != NULL && bytes >= old_sample->bytes) {
		long double elapsed = elapsed_seconds(&aggregation->observed_at,
			&old_sample->observed_at);

		if(elapsed > 0.0L) {
			metrics->bitrate_bps +=
				(long double)(bytes - old_sample->bytes) * 8.0L / elapsed;
			metrics->bitrate_valid = true;
		}
	}
	return true;
}

static bool object_has_rtcp_metrics(json_t *object) {
	return json_object_get(object, "rtt") != NULL ||
		json_object_get(object, "lost-by-remote") != NULL ||
		json_object_get(object, "jitter-remote") != NULL ||
		json_object_get(object, "out-link-quality") != NULL ||
		json_object_get(object, "out-media-link-quality") != NULL;
}

static void aggregate_rtcp(json_t *node, ChannelMetrics *metrics,
		unsigned int depth) {
	uint64_t value;

	if(node == NULL || depth > 4U)
		return;
	if(json_is_array(node)) {
		size_t index;
		json_t *item;

		json_array_foreach(node, index, item)
			aggregate_rtcp(item, metrics, depth + 1U);
		return;
	}
	if(!json_is_object(node))
		return;
	if(!object_has_rtcp_metrics(node)) {
		const char *key;
		json_t *item;

		json_object_foreach(node, key, item) {
			(void)key;
			aggregate_rtcp(item, metrics, depth + 1U);
		}
		return;
	}
	if(json_uint64_value(json_object_get(node, "lost-by-remote"), &value))
		add_counter(&metrics->remote_lost, value);
	if(json_uint64_value(json_object_get(node, "rtt"), &value))
		update_maximum(value, &metrics->rtt_ms, &metrics->rtt_valid);
	if(json_uint64_value(json_object_get(node, "jitter-remote"), &value))
		update_maximum(value, &metrics->jitter_ms, &metrics->jitter_valid);
	if(json_uint64_value(json_object_get(node, "out-link-quality"), &value))
		update_minimum(value, &metrics->link_quality, &metrics->link_quality_valid);
	if(json_uint64_value(json_object_get(node, "out-media-link-quality"), &value))
		update_minimum(value, &metrics->media_link_quality,
			&metrics->media_link_quality_valid);
}

static bool aggregate_medium(json_t *medium, const char *fallback,
		HandleAggregation *aggregation, ChannelMetrics *metrics) {
	json_t *direction;
	json_t *stats;
	json_t *out_stats;
	json_t *in_stats;
	uint64_t value;
	uint64_t bytes;
	bool sends = false;
	bool receives = true;
	bool have_sends;
	bool have_receives;
	char mid[MAX_MID_SIZE + 1U];

	if(!json_is_object(medium))
		return true;
	direction = json_object_get(medium, "direction");
	have_sends = json_is_object(direction) &&
		json_flag_value(json_object_get(direction, "send"), &sends);
	have_receives = json_is_object(direction) &&
		json_flag_value(json_object_get(direction, "recv"), &receives);
	stats = json_object_get(medium, "stats");
	out_stats = json_is_object(stats) ? json_object_get(stats, "out") : NULL;
	in_stats = json_is_object(stats) ? json_object_get(stats, "in") : NULL;
	if(json_is_object(out_stats)) {
		if(json_uint64_value(json_object_get(out_stats, "packets"), &value))
			add_counter(&metrics->packets_sent, value);
		if(json_uint64_value(json_object_get(out_stats, "bytes"), &bytes)) {
			add_counter(&metrics->bytes_sent, bytes);
			if(medium_identifier(medium, fallback, mid) &&
				!record_byte_sample(aggregation, metrics, mid, bytes))
				return false;
		}
	}
	if(have_sends && sends && have_receives && !receives && json_is_object(in_stats) &&
		json_uint64_value(json_object_get(in_stats, "nacks"), &value))
		add_counter(&metrics->nacks_received, value);
	if(have_sends && sends)
		aggregate_rtcp(json_object_get(medium, "rtcp"), metrics, 0U);
	return true;
}

static bool object_looks_like_medium(json_t *object) {
	return json_object_get(object, "stats") != NULL ||
		json_object_get(object, "direction") != NULL ||
		json_object_get(object, "mid") != NULL ||
		json_object_get(object, "mindex") != NULL;
}

static bool aggregate_media(json_t *media, HandleAggregation *aggregation,
		ChannelMetrics *metrics) {
	if(json_is_array(media)) {
		size_t index;
		json_t *medium;

		json_array_foreach(media, index, medium) {
			char fallback[48];

			if(snprintf(fallback, sizeof(fallback), "index:%zu", index) < 0 ||
				!aggregate_medium(medium, fallback, aggregation, metrics))
				return false;
		}
		return true;
	}
	if(json_is_object(media)) {
		const char *mid;
		json_t *medium;

		if(object_looks_like_medium(media))
			return aggregate_medium(media, "single", aggregation, metrics);
		json_object_foreach(media, mid, medium) {
			if(!aggregate_medium(medium, mid, aggregation, metrics))
				return false;
		}
	}
	return true;
}

static bool plugin_state_is_active(json_t *plugin_specific,
		unsigned int *channel_index) {
	static const char *const inactive_flags[] = {
		"stopping", "destroyed", "hangingup"
	};
	json_t *mountpoint;
	json_int_t mountpoint_id;
	bool flag;
	size_t index;

	if(!json_is_object(plugin_specific))
		return false;
	mountpoint = json_object_get(plugin_specific, "mountpoint_id");
	if(!json_is_integer(mountpoint))
		return false;
	mountpoint_id = json_integer_value(mountpoint);
	if(mountpoint_id < 1 || mountpoint_id > CHANNEL_COUNT)
		return false;
	if(!json_flag_value(json_object_get(plugin_specific, "started"), &flag) || !flag)
		return false;
	if(!json_flag_value(json_object_get(plugin_specific, "paused"), &flag) || flag)
		return false;
	for(index = 0U; index < sizeof(inactive_flags) / sizeof(inactive_flags[0]); index++) {
		json_t *value = json_object_get(plugin_specific, inactive_flags[index]);

		if(value != NULL && (!json_flag_value(value, &flag) || flag))
			return false;
	}
	*channel_index = (unsigned int)mountpoint_id - 1U;
	return true;
}

static bool aggregate_handle(json_t *root, uint64_t session_id,
		uint64_t handle_id, HandleAggregation *aggregation) {
	json_t *info = json_object_get(root, "info");
	json_t *plugin;
	json_t *plugin_specific;
	json_t *webrtc;
	const char *plugin_name;
	unsigned int channel_index;
	ChannelMetrics *metrics;

	if(!json_is_object(info))
		return false;
	plugin = json_object_get(info, "plugin");
	plugin_name = json_string_value(plugin);
	if(plugin_name == NULL || strcmp(plugin_name, "janus.plugin.streaming") != 0)
		return true;
	plugin_specific = json_object_get(info, "plugin_specific");
	if(!plugin_state_is_active(plugin_specific, &channel_index))
		return true;
	metrics = &aggregation->channels[channel_index];
	add_counter(&metrics->viewers, 1U);
	webrtc = json_object_get(info, "webrtc");
	if(!json_is_object(webrtc))
		return true;
	if(clock_gettime(CLOCK_MONOTONIC, &aggregation->observed_at) != 0)
		return false;
	aggregation->session_id = session_id;
	aggregation->handle_id = handle_id;
	return aggregate_media(json_object_get(webrtc, "media"), aggregation, metrics);
}

static bool response_id_array(json_t *root, const char *field, json_t **array) {
	json_t *value;
	size_t index;
	json_t *item;

	if(!janus_response_succeeded(root))
		return false;
	value = json_object_get(root, field);
	if(!json_is_array(value))
		return false;
	json_array_foreach(value, index, item) {
		json_int_t id;

		(void)index;
		if(!json_is_integer(item))
			return false;
		id = json_integer_value(item);
		if(id <= 0)
			return false;
	}
	*array = value;
	return true;
}

static bool build_resource_url(const char *base_url, uint64_t session_id,
		uint64_t handle_id, char output[MAX_ADMIN_URL + 64U]) {
	int length;

	if(handle_id == 0U) {
		length = snprintf(output, MAX_ADMIN_URL + 64U, "%s/%" PRIu64,
			base_url, session_id);
	} else {
		length = snprintf(output, MAX_ADMIN_URL + 64U, "%s/%" PRIu64 "/%" PRIu64,
			base_url, session_id, handle_id);
	}
	return length > 0 && (size_t)length < MAX_ADMIN_URL + 64U;
}

static bool poll_cycle(AdminClient *client, const char *base_url,
		CounterSample *previous_samples, ChannelMetrics channels[CHANNEL_COUNT],
		CounterSample **next_samples) {
	json_t *sessions_root = NULL;
	json_t *sessions;
	HandleAggregation aggregation;
	size_t session_index;
	json_t *session_value;
	bool success = false;

	memset(channels, 0, sizeof(*channels) * CHANNEL_COUNT);
	memset(&aggregation, 0, sizeof(aggregation));
	aggregation.channels = channels;
	aggregation.previous_samples = previous_samples;
	sessions_root = admin_request(client, base_url, "list_sessions");
	if(sessions_root == NULL || !response_id_array(sessions_root, "sessions", &sessions))
		goto done;

	json_array_foreach(sessions, session_index, session_value) {
		uint64_t session_id = (uint64_t)json_integer_value(session_value);
		char session_url[MAX_ADMIN_URL + 64U];
		json_t *handles_root;
		json_t *handles;
		size_t handle_index;
		json_t *handle_value;

		(void)session_index;
		if(stop_requested)
			goto done;
		if(!build_resource_url(base_url, session_id, 0U, session_url))
			goto done;
		handles_root = admin_request(client, session_url, "list_handles");
		if(handles_root == NULL)
			goto done;
		if(janus_resource_disappeared(handles_root)) {
			json_decref(handles_root);
			continue;
		}
		if(!response_id_array(handles_root, "handles", &handles)) {
			json_decref(handles_root);
			goto done;
		}
		json_array_foreach(handles, handle_index, handle_value) {
			uint64_t handle_id = (uint64_t)json_integer_value(handle_value);
			char handle_url[MAX_ADMIN_URL + 64U];
			json_t *handle_root;

			(void)handle_index;
			if(stop_requested) {
				json_decref(handles_root);
				goto done;
			}
			if(!build_resource_url(base_url, session_id, handle_id, handle_url)) {
				json_decref(handles_root);
				goto done;
			}
			handle_root = admin_request(client, handle_url, "handle_info");
			if(handle_root == NULL) {
				json_decref(handles_root);
				goto done;
			}
			if(janus_resource_disappeared(handle_root)) {
				json_decref(handle_root);
				continue;
			}
			if(!janus_response_succeeded(handle_root) ||
				!aggregate_handle(handle_root, session_id, handle_id, &aggregation)) {
				json_decref(handle_root);
				json_decref(handles_root);
				goto done;
			}
			json_decref(handle_root);
		}
		json_decref(handles_root);
	}
	success = true;
	*next_samples = aggregation.next_samples;
	aggregation.next_samples = NULL;

done:
	json_decref(sessions_root);
	free_samples(aggregation.next_samples);
	return success;
}

static bool write_all(int fd, const char *data, size_t length) {
	size_t written = 0U;

	while(written < length) {
		ssize_t count = write(fd, data + written, length - written);

		if(count < 0) {
			if(errno == EINTR)
				continue;
			return false;
		}
		if(count == 0)
			return false;
		written += (size_t)count;
	}
	return true;
}

static bool atomic_write_status(unsigned int channel_id, const char *contents,
		size_t contents_length) {
	char final_path[128];
	char temporary_path[192];
	int fd = -1;
	unsigned int attempt;
	bool success = false;

	if(snprintf(final_path, sizeof(final_path),
			"/run/webrtc-player/channels/%u/janus-output.status", channel_id) < 0)
		return false;
	for(attempt = 0U; attempt < 10U; attempt++) {
		temporary_file_id++;
		if(snprintf(temporary_path, sizeof(temporary_path),
				"/run/webrtc-player/channels/%u/.janus-output.status.tmp.%ld.%" PRIu64,
				channel_id, (long)getpid(), temporary_file_id) < 0)
			return false;
		fd = open(temporary_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
		if(fd >= 0 || errno != EEXIST)
			break;
	}
	if(fd < 0)
		return false;
	if(fchmod(fd, 0640) != 0 || !write_all(fd, contents, contents_length))
		goto done;
	if(close(fd) != 0) {
		fd = -1;
		goto done;
	}
	fd = -1;
	if(rename(temporary_path, final_path) != 0)
		goto done;
	success = true;

done:
	if(fd >= 0)
		(void)close(fd);
	if(!success)
		(void)unlink(temporary_path);
	return success;
}

static void format_optional_uint64(char output[32], bool valid, uint64_t value) {
	if(!valid) {
		output[0] = '\0';
		return;
	}
	(void)snprintf(output, 32U, "%" PRIu64, value);
}

static bool publish_status_files(bool available,
		const ChannelMetrics channels[CHANNEL_COUNT]) {
	bool all_succeeded = true;
	unsigned int index;

	for(index = 0U; index < CHANNEL_COUNT; index++) {
		const ChannelMetrics *metrics = &channels[index];
		char bitrate[32];
		char rtt[32];
		char jitter[32];
		char link_quality[32];
		char media_link_quality[32];
		char contents[768];
		uint64_t rounded_bitrate = 0U;
		int length;

		if(metrics->bitrate_valid) {
			if(metrics->bitrate_bps >= (long double)UINT64_MAX)
				rounded_bitrate = UINT64_MAX;
			else
				rounded_bitrate = (uint64_t)(metrics->bitrate_bps + 0.5L);
		}
		format_optional_uint64(bitrate, metrics->bitrate_valid, rounded_bitrate);
		format_optional_uint64(rtt, metrics->rtt_valid, metrics->rtt_ms);
		format_optional_uint64(jitter, metrics->jitter_valid, metrics->jitter_ms);
		format_optional_uint64(link_quality, metrics->link_quality_valid,
			metrics->link_quality);
		format_optional_uint64(media_link_quality, metrics->media_link_quality_valid,
			metrics->media_link_quality);
		length = snprintf(contents, sizeof(contents),
			"available=%s\n"
			"viewers=%" PRIu64 "\n"
			"packets_sent=%" PRIu64 "\n"
			"bytes_sent=%" PRIu64 "\n"
			"bitrate_bps=%s\n"
			"nacks_received=%" PRIu64 "\n"
			"remote_lost=%" PRIu64 "\n"
			"rtt_ms=%s\n"
			"jitter_ms=%s\n"
			"link_quality=%s\n"
			"media_link_quality=%s\n",
			available ? "true" : "false", metrics->viewers,
			metrics->packets_sent, metrics->bytes_sent, bitrate,
			metrics->nacks_received, metrics->remote_lost, rtt, jitter,
			link_quality, media_link_quality);
		if(length < 0 || (size_t)length >= sizeof(contents) ||
			!atomic_write_status(index + 1U, contents, (size_t)length))
			all_succeeded = false;
	}
	return all_succeeded;
}

static void sleep_until_next_cycle(const struct timespec *cycle_started) {
	struct timespec deadline = *cycle_started;

	deadline.tv_sec++;
	while(!stop_requested) {
		int result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
			&deadline, NULL);

		if(result == 0)
			return;
		if(result != EINTR)
			return;
	}
}

static bool append_header(struct curl_slist **headers, const char *value) {
	struct curl_slist *updated = curl_slist_append(*headers, value);

	if(updated == NULL)
		return false;
	*headers = updated;
	return true;
}

int main(void) {
	const char *configured_url = getenv("JANUS_ADMIN_URL");
	const char *secret_path = getenv("JANUS_ADMIN_SECRET_FILE");
	char base_url[MAX_ADMIN_URL];
	AdminClient client;
	CounterSample *previous_samples = NULL;
	ChannelMetrics channels[CHANNEL_COUNT];
	int last_available = -1;
	bool last_publish_succeeded = true;
	int exit_status = EXIT_SUCCESS;

	memset(&client, 0, sizeof(client));
	memset(channels, 0, sizeof(channels));
	if(configured_url == NULL || configured_url[0] == '\0')
		configured_url = DEFAULT_ADMIN_URL;
	if(secret_path == NULL || secret_path[0] == '\0')
		secret_path = DEFAULT_SECRET_FILE;
	if(!install_signal_handlers()) {
		fprintf(stderr, "janus-monitor: failed to install signal handlers\n");
		return EXIT_FAILURE;
	}
	if(!validate_and_copy_admin_url(configured_url, base_url)) {
		(void)publish_status_files(false, channels);
		fprintf(stderr, "janus-monitor: JANUS_ADMIN_URL must use a literal loopback HTTP address\n");
		return EXIT_FAILURE;
	}
	if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		(void)publish_status_files(false, channels);
		fprintf(stderr, "janus-monitor: libcurl initialization failed\n");
		return EXIT_FAILURE;
	}
	client.curl = curl_easy_init();
	if(client.curl == NULL ||
		!append_header(&client.headers, "Content-Type: application/json") ||
		!append_header(&client.headers, "Accept: application/json") ||
		!append_header(&client.headers, "Expect:")) {
		(void)publish_status_files(false, channels);
		fprintf(stderr, "janus-monitor: HTTP client initialization failed\n");
		exit_status = EXIT_FAILURE;
		goto cleanup;
	}

	while(!stop_requested) {
		struct timespec cycle_started;
		CounterSample *next_samples = NULL;
		char *secret;
		bool available = false;
		bool publish_succeeded;

		if(clock_gettime(CLOCK_MONOTONIC, &cycle_started) != 0) {
			memset(channels, 0, sizeof(channels));
			(void)publish_status_files(false, channels);
			fprintf(stderr, "janus-monitor: monotonic clock failed\n");
			exit_status = EXIT_FAILURE;
			break;
		}
		secret = read_secret_file(secret_path);
		if(secret != NULL) {
			client.secret = secret;
			available = poll_cycle(&client, base_url, previous_samples,
				channels, &next_samples);
			client.secret = NULL;
			secure_clear(secret);
			free(secret);
		} else {
			memset(channels, 0, sizeof(channels));
		}
		if(stop_requested) {
			free_samples(next_samples);
			break;
		}
		free_samples(previous_samples);
		previous_samples = NULL;
		if(available) {
			previous_samples = next_samples;
			next_samples = NULL;
		} else {
			free_samples(next_samples);
			memset(channels, 0, sizeof(channels));
		}

		publish_succeeded = publish_status_files(available, channels);
		if((available ? 1 : 0) != last_available) {
			fprintf(stderr, "janus-monitor: Janus Admin API %s\n",
				available ? "available" : "unavailable");
			last_available = available ? 1 : 0;
		}
		if(!publish_succeeded && last_publish_succeeded)
			fprintf(stderr, "janus-monitor: status publication failed\n");
		else if(publish_succeeded && !last_publish_succeeded)
			fprintf(stderr, "janus-monitor: status publication recovered\n");
		last_publish_succeeded = publish_succeeded;
		sleep_until_next_cycle(&cycle_started);
	}

cleanup:
	free_samples(previous_samples);
	curl_slist_free_all(client.headers);
	if(client.curl != NULL)
		curl_easy_cleanup(client.curl);
	curl_global_cleanup();
	return exit_status;
}
