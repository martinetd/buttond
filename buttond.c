// SPDX-License-Identifier: MIT
/*
 * Handle evdev button press
 * Copyright (c) 2021 Atmark Techno,Inc.
 */

#define _POSIX_C_SOURCE 199309L
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define xassert(cond, fmt, args...) \
	if (!(cond)) { \
		fprintf(stderr, "ERROR: "fmt"\n", ##args); \
		exit(EXIT_FAILURE); \
	}

static int debug = 0;
#define DEFAULT_LONG_PRESS_MSECS 5000
#define DEFAULT_SHORT_PRESS_MSECS 1000
#define DEBOUNCE_MSECS 10

ssize_t read_safe(int fd, void *buf, size_t count, size_t partial_read) {
	size_t total = partial_read % count;

	while (total < count) {
		ssize_t n;

		n = read(fd, (char*)buf + total, count - total);
		if (n < 0 && errno == EINTR)
			continue;
		else if (n < 0)
			return errno == EAGAIN ? 0 : -errno;
		else if (n == 0)
			break;
		total += n;
	}
	return total;
}

#define NSECS_IN_SEC  1000000000L
#define NSECS_IN_MSEC 1000000L
#define NSECS_IN_USEC 1000L
#define USECS_IN_SEC  1000000L
#define USECS_IN_MSEC 1000L

/* time difference in msecs */
static int64_t time_diff_ts(struct timespec *ts1, struct timespec *ts2) {
	return (ts1->tv_nsec - ts2->tv_nsec) / NSECS_IN_MSEC
		+ (ts1->tv_sec - ts2->tv_sec) * 1000;
}

static int64_t time_diff_tv(struct timeval *tv1, struct timeval *tv2) {
	return (tv1->tv_usec - tv2->tv_usec) / USECS_IN_MSEC
		+ (tv1->tv_sec - tv2->tv_sec) * 1000;
}

/* convert timeval to timespec and add offset msec */
static void time_tv2ts(struct timespec *ts, struct timeval *base, int msec) {
	ts->tv_nsec = base->tv_usec * NSECS_IN_USEC + msec * NSECS_IN_MSEC;
	ts->tv_sec = base->tv_sec + ts->tv_nsec / NSECS_IN_SEC;
	ts->tv_nsec %= NSECS_IN_SEC;
}
/* convert timespec to timeval and add offset msec */
static void time_ts2tv(struct timeval *tv, struct timespec *base, int msec) {
	tv->tv_usec = base->tv_nsec / NSECS_IN_USEC + msec * USECS_IN_MSEC;
	tv->tv_sec = base->tv_sec + tv->tv_usec / USECS_IN_SEC;
	tv->tv_usec %= USECS_IN_SEC;
}

#define OPT_TEST 257

static struct option long_options[] = {
	{"input",	required_argument,	0, 'i' },
	{"short",	required_argument,	0, 's' },
	{"long",	required_argument,	0, 'l' },
	{"action",	required_argument,	0, 'a' },
	{"time",	required_argument,	0, 't' },
	{"verbose",	no_argument,		0, 'v' },
	{"version",	no_argument,		0, 'V' },
	{"help",	no_argument,		0, 'h' },
	{"test_mode",	no_argument,		0, OPT_TEST },
	{0,		0,			0,  0  }
};

static void version(char *argv0) {
	printf("%s version 0.1\n", argv0);
}

static void help(char *argv0) {
	printf("Usage: %s [options]\n", argv0);
	printf("Options:\n");
	printf("  -i, --input <file>: file to get event from e.g. /dev/input/event2\n");
	printf("                      pass multiple times to monitor multiple files\n");
	printf("  -s/--short <key>  [-t/--time <time ms>] -a/--action <command>: action on short key press\n");
	printf("  -l/--long <key> [-t/--time <time ms>] -a/--action <command>: action on long key press\n");
	printf("  -h, --help: show this help\n");
	printf("  -V, --version: show version\n");
	printf("  -v, --verbose: verbose (repeatable)\n\n");

	printf("<key> code can be found in uapi/linux/input-event-code.h or by running\n");
	printf("with -vv\n\n");

	printf("Semantics: a short press action happens on release, if and only if\n");
	printf("the button was released before <time> (default %d) milliseconds.\n",
	       DEFAULT_SHORT_PRESS_MSECS);
	printf("a long press action happens even if key is still pressed, if it has been\n");
	printf("held for at least <time> (default %d) milliseconds.\n\n",
	       DEFAULT_LONG_PRESS_MSECS);

	printf("Note some keyboards have repeat built in firmware so quick repetitions\n");
	printf("(<%dms) are handled as if key were pressed continuously\n",
	       DEBOUNCE_MSECS);
}

struct action {
	/* type of action (long/short press) */
	enum type {
		LONG_PRESS,
		SHORT_PRESS,
	} type;
	/* cutoff time for action */
	int trigger_time;
	/* command to run */
	char const *action;
};

struct key {
	/* key code */
	uint16_t code;

	/* whether ts_wakeup below is valid */
	bool has_wakeup;

	/* key actions */
	int action_count;
	struct action *actions;

	/* when key was pressed - valid for state == KEY_PRESSED or KEY_DEBOUNCE */
	struct timeval tv_pressed;
	/* valid when KEY_DEBOUNCE */
	struct timeval tv_released;
	/* when next to wakeup if has_wakeup is set */
	struct timespec ts_wakeup;

	/* state machine:
	 * - RELEASED/PRESSED state
	 * - DEBOUNCE: immediately after being released for DEBOUNCE_MSECS
	 * - HANDLED: long press already handled (ignore until release)
	 */
	enum state {
		KEY_RELEASED,
		KEY_PRESSED,
		KEY_DEBOUNCE,
		KEY_HANDLED,
	} state;
};


static uint16_t strtou16(const char *str) {
	char *endptr;
	long val;

	val = strtol(str, &endptr, 0);
	xassert(*endptr == 0,
		"Argument %s must be a full integer", str);
	xassert(val >= 0 && val <= 0xffff,
		"Argument %s must be a 16 bit integer", str);
	errno = 0;
	return val;
}

static uint32_t strtoint(const char *str) {
	char *endptr;
	long val;

	val = strtol(str, &endptr, 0);
	xassert(*endptr == 0,
		"Argument %s must be a full integer", str);
	xassert(val >= INT_MIN && val <= INT_MAX,
		"Argument %s must fit in a C int", str);
	errno = 0;
	return val;
}

static void print_key(struct input_event *event, const char *filename,
		      const char *message) {
	if (debug < 1)
		return;
	switch (event->type) {
	case 0:
		/* extra info pertaining previous event: don't print */
		return;
	case 1:
		printf("[%ld.%03ld] %s%s%d %s: %s\n",
		       event->time.tv_sec, event->time.tv_usec / 1000,
		       debug > 2 ? filename : "",
		       debug > 2 ? " " : "",
		       event->code, event->value ? "pressed" : "released",
		       message);
		break;
	default:
		printf("[%ld.%03ld] %s%s%d %d %d: %s\n",
		       event->time.tv_sec, event->time.tv_usec / 1000,
		       debug > 2 ? filename : "",
		       debug > 2 ? " " : "",
		       event->type, event->code, event->value,
		       message);
	}
}

static void handle_key(struct input_event *event, struct key *key) {
	switch (key->state) {
	case KEY_RELEASED:
	case KEY_DEBOUNCE:
		/* new key press -- can be a release if program started with key or handled long press */
		if (event->value == 0)
			break;
		/* don't reset timestamp on debounce */
		if (key->state == KEY_RELEASED) {
			key->tv_pressed = event->time;
		}
		key->state = KEY_PRESSED;

		struct action *action = &key->actions[key->action_count-1];
		if (action->type == LONG_PRESS) {
			key->has_wakeup = true;
			time_tv2ts(&key->ts_wakeup, &key->tv_pressed,
				   action->trigger_time);
		}
		break;
	case KEY_PRESSED:
		/* ignore repress */
		if (event->value != 0)
			break;
		/* mark key for debounce, we will handle event after timeout */
		key->state = KEY_DEBOUNCE;
		key->tv_released = event->time;
		key->has_wakeup = true;
		time_tv2ts(&key->ts_wakeup, &key->tv_pressed,
			   DEBOUNCE_MSECS);
		break;
	case KEY_HANDLED:
		/* ignore until key down */
		if (event->value != 0)
			break;
		key->state = KEY_RELEASED;
	}
}

static int compute_timeout(struct key *keys, int key_count) {
	int i;
	int timeout = -1;
	struct timespec ts;

	xassert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0,
		"Could not get time: %m");

	for (i = 0; i < key_count; i++) {
		if (keys[i].has_wakeup) {
			int64_t diff = time_diff_ts(&keys[i].ts_wakeup, &ts);
			if (diff < 0)
				timeout = 0;
			else if (timeout == -1 || diff < timeout)
				timeout = diff;
		}
	}
	return timeout;
}

static bool action_match(struct action *action, int time) {
	switch (action->type) {
	case LONG_PRESS:
		return time >= action->trigger_time;
	case SHORT_PRESS:
		return time < action->trigger_time;
	default:
		xassert(false, "invalid action!!");
	}
}

static struct action *find_key_action(struct key *key, int time) {
	/* check from the end to get the best match */
	for (int i = key->action_count - 1; i >= 0; i--) {
		if (action_match(&key->actions[i], time))
			return &key->actions[i];
	}
	return NULL;
}

static void handle_timeouts(struct key *keys, int key_count) {
	int i;
	struct timespec ts;

	xassert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0,
		"Could not get time: %m");

	for (i = 0; i < key_count; i++) {
		if (keys[i].has_wakeup
		    && (time_diff_ts(&keys[i].ts_wakeup, &ts) <= 0)) {

			if (keys[i].state != KEY_DEBOUNCE) {
				/* key still pressed - set artifical release time */
				time_ts2tv(&keys[i].tv_released, &ts, 0);
			}

			int64_t diff = time_diff_tv(&keys[i].tv_released, &keys[i].tv_pressed);
			struct action *action = find_key_action(&keys[i], diff);
			if (action) {
				if (debug)
					printf("running %s\n", action->action);
				system(action->action);
			} else if (debug) {
				printf("ignoring key %d released after %"PRId64" ms\n",
				       keys[i].code, diff);
			}

			keys[i].has_wakeup = false;
			if (keys[i].state == KEY_DEBOUNCE)
				keys[i].state = KEY_RELEASED;
			else
				keys[i].state = KEY_HANDLED;
		}
	}
}

static void handle_input(int fd, struct key *keys, int key_count,
			 const char *filename) {
	struct input_event event;
	int partial_read = 0;

	while ((partial_read = read_safe(fd, &event, sizeof(event), partial_read))
			== sizeof(event)) {
		/* ignore non-keyboard events */
		if (event.type != 1) {
			if (debug > 2)
				print_key(&event, filename, "non-keyboard event ignored");
			continue;
		}

		struct key *key = NULL;
		for (int i = 0; i < key_count; i++) {
			if (keys[i].code == event.code) {
				key = &keys[i];
				break;
			}
		}
		/* ignore unconfigured key */
		if (!key) {
			if (debug > 1)
				print_key(&event, filename, "ignored");
			continue;
		}
		print_key(&event, filename, "processing");

		handle_key(&event, key);
	}
	if (partial_read < 0) {
		fprintf(stderr, "read error: %d\n", -partial_read);
		exit(EXIT_FAILURE);
	}
	if (partial_read > 0) {
		fprintf(stderr, "got a partial read from %s: %d. Aborting.\n",
			filename, partial_read);
		exit(EXIT_FAILURE);
	}
}

static struct action *add_short_action(struct key *key) {
	/* can only have one short key */
	for (int i = 0; i < key->action_count; i++) {
		xassert(key->actions[i].type != SHORT_PRESS,
			"duplicate short key for key %d, aborting.",
			key->code);
	}
	struct action *action = &key->actions[key->action_count];
	action->type = SHORT_PRESS;
	action->trigger_time = DEFAULT_SHORT_PRESS_MSECS;
	return action;
}

static struct action *add_long_action(struct key *key) {
	/* insert at the end, we'll move it when setting time */
	struct action *action = &key->actions[key->action_count];
	action->type = LONG_PRESS;
	action->trigger_time = DEFAULT_LONG_PRESS_MSECS;
	return action;
}

static int sort_actions_compare(const void *v1, const void *v2) {
	const struct action *a1 = (const struct action*)v1;
	const struct action *a2 = (const struct action*)v2;
	if (a1->type == SHORT_PRESS)
		return -1;
	if (a2->type == SHORT_PRESS)
		return 1;
	if (a1->trigger_time < a2->trigger_time)
		return -1;
	if (a1->trigger_time > a2->trigger_time)
		return 1;
	return 0;
}
static void sort_actions(struct key *key) {
	qsort(key->actions, key->action_count,
		sizeof(key->actions[0]), sort_actions_compare);
}

static void *xcalloc(size_t nmemb, size_t size) {
	void *ptr = calloc(nmemb, size);
	xassert(ptr, "Allocation failure");
	return ptr;
}

static void *xreallocarray(void *ptr, size_t nmemb, size_t size) {
	ptr = realloc(ptr, nmemb * size);
	xassert(ptr, "Allocation failure");
	return ptr;
}

int main(int argc, char *argv[]) {
	char const **event_inputs = NULL;
	struct key *keys = NULL;
	struct action *cur_action = NULL;
	int input_count = 0;
	int key_count = 0;
	bool test_mode = false;

	int c;
	/* and real argument parsing now! */
	while ((c = getopt_long(argc, argv, "i:s:l:a:t:vVh", long_options, NULL)) >= 0) {
		switch (c) {
		case 'i':
			event_inputs = xreallocarray(event_inputs, input_count + 1,
					             sizeof(*event_inputs));
			event_inputs[input_count] = optarg;
			input_count++;
			break;
		case 's':
		case 'l':
			xassert(!cur_action || cur_action->action != NULL,
				"Must set action before specifying next key!");
			uint16_t code = strtou16(optarg);
			struct key *cur_key = NULL;
			for (int i = 0; i < key_count; i++) {
				if (keys[i].code == code) {
					cur_key = &keys[i];
					break;
				}
			}
			if (!cur_key) {
				keys = xreallocarray(keys, key_count + 1,
						     sizeof(*keys));
				cur_key = &keys[key_count];
				key_count++;
				memset(cur_key, 0, sizeof(*cur_key));
				cur_key->code = strtou16(optarg);
				cur_key->state = KEY_RELEASED;
			}
			cur_key->actions = xreallocarray(cur_key->actions,
							 cur_key->action_count + 1,
							 sizeof(*cur_key->actions));
			if (c == 's') {
				cur_action = add_short_action(cur_key);
			} else {
				cur_action = add_long_action(cur_key);
			}
			cur_action->action = NULL;
			cur_key->action_count++;
			break;
		case 'a':
			xassert(cur_action,
				"Action can only be provided after setting key code");
			cur_action->action = optarg;
			break;
		case 't':
			xassert(cur_action,
				"Action timeout can only be set after setting key code");
			cur_action->trigger_time = strtoint(optarg);
			break;
		case 'v':
			debug++;
			break;
		case 'V':
			version(argv[0]);
			exit(EXIT_SUCCESS);
		case 'h':
			help(argv[0]);
			exit(EXIT_SUCCESS);
		case OPT_TEST:
			test_mode = true;
			break;
		default:
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}
	xassert(optind >= argc,
		"Non-option argument: %s. Did you forget to quote action?",
		optind >= 0 ? argv[optind] : "???");
	xassert(input_count > 0,
		"No input have been given, exiting");
	xassert(key_count > 0 || debug > 1,
		"No action given, exiting");
	xassert(!cur_action || cur_action->action != NULL,
		"Last key press was defined without action");
	for (int i = 0; i < key_count; i++) {
		sort_actions(&keys[i]);
	}

	struct pollfd *pollfd = xcalloc(input_count, sizeof(*pollfd));
	for (int i = 0; i < input_count; i++) {
		int fd = open(event_inputs[i], O_RDONLY|O_NONBLOCK);
		xassert(fd >= 0,
			"Open %s failed: %m", event_inputs[i]);
		c = CLOCK_MONOTONIC;
		/* we use a pipe for testing which won't understand this */
		xassert(test_mode || ioctl(fd, EVIOCSCLOCKID, &c) == 0,
			"Could not request clock monotonic timestamps from %s, aborting",
			event_inputs[i]);

		pollfd[i].fd = fd;
		pollfd[i].events = POLLIN;
	}

	while (1) {
		int timeout = compute_timeout(keys, key_count);
		int n = poll(pollfd, input_count, timeout);
		if (n < 0 && errno == EINTR)
			continue;
		xassert(n >= 0, "Poll failure: %m");

		handle_timeouts(keys, key_count);
		if (n == 0)
			continue;
		for (int i = 0; i < input_count; i++) {
			if (pollfd[i].revents == 0)
				continue;
			if (!(pollfd[i].revents & POLLIN)) {
				if (test_mode)
					exit(0);
				xassert(false, "got HUP/ERR on %s, aborting",
					event_inputs[i]);
			}
			handle_input(pollfd[i].fd, keys, key_count, event_inputs[i]);
		}
	}

	/* unreachable */
}
