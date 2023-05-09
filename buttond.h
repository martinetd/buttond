// SPDX-License-Identifier: MIT

#ifndef BUTTOND_H
#define BUTTOND_H

#include <stdbool.h>

#include "utils.h"
#include "time_utils.h"

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
	/* whether to stop after action has been processed */
	bool exit_after;
};

struct key {
	/* key code */
	uint16_t code;
	const char *name;

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
	enum key_state {
		KEY_RELEASED,
		KEY_PRESSED,
		KEY_DEBOUNCE,
		KEY_HANDLED,
	} state;
};

struct input_file {
	/* first is full path, second is path in directory */
	char *filename;
	char *dirent;
	int inotify_wd;
};

struct state {
	struct key *keys;
	struct input_file *input_files;
	struct pollfd *pollfds;
	int key_count;
	int input_count;
};

extern int debug;
extern int test_mode;


void reopen_input(struct state *state, int i);
void handle_inotify(struct state *state);

#endif
