/* Parser for /etc/finit.conf and /etc/finit.d/<SVC>.conf
 *
 * Copyright (c) 2012-2015  Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"		/* Generated by configure script */

#include <dirent.h>
#include <string.h>
#include <sys/resource.h>
#include <lite/lite.h>

#include "finit.h"
#include "cond.h"
#include "service.h"
#include "tty.h"
#include "helpers.h"

#define MATCH_CMD(l, c, x) \
	(!strncasecmp(l, c, strlen(c)) && (x = (l) + strlen(c)))

static int parse_conf(char *file);


void conf_parse_cmdline(void)
{
	FILE *fp;
	char line[LINE_SIZE];

	fp = fopen("/proc/cmdline", "r");
	if (fp) {
		fgets(line, sizeof(line), fp);
		_d("Kernel command line: %s", line);

		if (strstr(line, "finit_debug") || strstr(line, "--debug"))
			debug = 1;

		if (!debug && strstr(line, "quiet"))
			quiet = 1;

		fclose(fp);
	}
}

/* Convert optional "[!123456789S]" string into a bitmask */
int conf_parse_runlevels(char *runlevels)
{
	int i, not = 0, bitmask = 0;

	if (!runlevels)
		runlevels = "[234]";
	i = 1;
	while (i) {
		int level;
		char lvl = runlevels[i++];

		if (']' == lvl || 0 == lvl)
			break;
		if ('!' == lvl) {
			not = 1;
			bitmask = 0x3FE;
			continue;
		}

		if ('s' == lvl || 'S' == lvl)
			lvl = '0';

		level = lvl - '0';
		if (level > 9 || level < 0)
			continue;

		if (not)
			CLRBIT(bitmask, level);
		else
			SETBIT(bitmask, level);
	}

	return bitmask;
}

void conf_parse_cond(svc_t *svc, char *cond)
{
	size_t i = 0;
	char *ptr;

	if (!svc) {
		_e("Invalid service pointer");
		return;
	}

	/* By default we assume UNIX daemons support SIGHUP */
	if (svc_is_daemon(svc))
		svc->sighup = 1;

	if (!cond)
		return;

	/* First character must be '!' if SIGHUP is not supported. */
	ptr = cond;
	if (ptr[i] == '!') {
		svc->sighup = 0;
		ptr++;
	}

	while (ptr[i] != '>' && ptr[i] != 0)
		i++;
	ptr[i] = 0;

	if (i >= sizeof(svc->cond)) {
		logit(LOG_WARNING, "Too long event list in declaration of %s: %s", svc->cmd, ptr);
		return;
	}

	strlcpy(svc->cond, ptr, sizeof(svc->cond));
}

struct rlimit_name {
	char *name;
	int val;
};

static const struct rlimit_name rlimit_names[] = {
	{ "as",         RLIMIT_AS         },
	{ "core",       RLIMIT_CORE       },
	{ "cpu",        RLIMIT_CPU        },
	{ "data",       RLIMIT_DATA       },
	{ "fsize",      RLIMIT_FSIZE      },
	{ "locks",      RLIMIT_LOCKS      },
	{ "memlock",    RLIMIT_MEMLOCK    },
	{ "msgqueue",   RLIMIT_MSGQUEUE   },
	{ "nice",       RLIMIT_NICE       },
	{ "nofile",     RLIMIT_NOFILE     },
	{ "nproc",      RLIMIT_NPROC      },
	{ "rss",        RLIMIT_RSS        },
	{ "rtprio",     RLIMIT_RTPRIO     },
#ifdef RLIMIT_RTTIME
	{ "rttime",     RLIMIT_RTTIME     },
#endif
	{ "sigpending", RLIMIT_SIGPENDING },
	{ "stack",      RLIMIT_STACK      },

	{ NULL, 0 }
};

void conf_parse_rlimit(char *line)
{
	struct rlimit rlim;
	rlim_t new, *set;
	const struct rlimit_name *name;
	int resource = -1;

	char *tok = strtok(line, " \t");

	if (tok && !strcmp(tok, "soft"))
		set = &rlim.rlim_cur;
	else if (tok && !strcmp(tok, "hard"))
		set = &rlim.rlim_max;
	else
		goto error;

	tok = strtok(NULL, " \t");
	if (!tok)
		goto error;

	for (name = rlimit_names; name->name; name++) {
		if (!strcmp(tok, name->name))
			resource = name->val;
	}

	if (resource < 0)
		goto fail;

	tok = strtok(NULL, " \t");
	if (!tok)
		goto fail;

	if (!strcmp(tok, "infinity")) {
		new = RLIM_INFINITY;
	} else {
		const char *err = NULL;

		new = strtonum(tok, 0, (long long)2 << 31, &err);
		if (err)
			goto fail;
	}

	if (getrlimit(resource, &rlim))
		goto fail;

	*set = new;
	if (setrlimit(resource, &rlim))
		goto fail;

	return;

fail:
	logit(LOG_WARNING, "rlimit: Failed setting rlimit %s", name->name ? : "unknown");
	return;
error:
	logit(LOG_WARNING, "rlimit: parse error");
}

static void parse_static(char *line)
{
	char *x;
	char cmd[CMD_SIZE];

	/* Do this before mounting / read-write
	 * XXX: Move to plugin which checks /etc/fstab instead */
	if (MATCH_CMD(line, "check ", x)) {
		char *dev = strip_line(x);

		strcpy(cmd, "/sbin/fsck -C -a ");
		strlcat(cmd, dev, sizeof(cmd));
		run_interactive(cmd, "Checking file system %s", dev);

		return;
	}

	if (MATCH_CMD(line, "user ", x)) {
		if (username) free(username);
		username = strdup(strip_line(x));
		return;
	}

	if (MATCH_CMD(line, "host ", x)) {
		if (hostname) free(hostname);
		hostname = strdup(strip_line(x));
		return;
	}

	if (MATCH_CMD(line, "module ", x)) {
		char *mod = strip_line(x);

		strcpy(cmd, "/sbin/modprobe ");
		strlcat(cmd, mod, sizeof(cmd));
		run_interactive(cmd, "Loading kernel module %s", mod);

		return;
	}

	if (MATCH_CMD(line, "mknod ", x)) {
		char *dev = strip_line(x);

		strcpy(cmd, "/bin/mknod ");
		strlcat(cmd, dev, sizeof(cmd));
		run_interactive(cmd, "Creating device node %s", dev);

		return;
	}

	if (MATCH_CMD(line, "network ", x)) {
		if (network) free(network);
		network = strdup(strip_line(x));
		return;
	}

	if (MATCH_CMD(line, "runparts ", x)) {
		if (runparts) free(runparts);
		runparts = strdup(strip_line(x));
		return;
	}

	if (MATCH_CMD(line, "include ", x)) {
		char *file = strip_line(x);

		strlcpy(cmd, file, sizeof(cmd));
		if (!fexist(cmd)) {
			_e("Cannot find include file %s, absolute path required!", x);
			return;
		}

		parse_conf(cmd);
		return;
	}

	if (MATCH_CMD(line, "startx ", x)) {
		service_register(SVC_TYPE_SERVICE, strip_line(x), 0, username);
		return;
	}

	if (MATCH_CMD(line, "shutdown ", x)) {
		if (sdown) free(sdown);
		sdown = strdup(strip_line(x));
		return;
	}

	/* The desired runlevel to start when leaving bootstrap (S).
	 * Finit supports 1-9, but most systems only use 1-6, where
	 * 6 is reserved for reboot */
	if (MATCH_CMD(line, "runlevel ", x)) {
		char *token = strip_line(x);
		const char *err = NULL;

		cfglevel = strtonum(token, 1, 9, &err);
		if (err)
			cfglevel = RUNLEVEL;
		if (cfglevel < 1 || cfglevel > 9 || cfglevel == 6)
			cfglevel = 2; /* Fallback */
		return;
	}

	/* TODO: Make console & tty dynamically loadable from /etc/finit.d */
	if (MATCH_CMD(line, "console ", x)) {
		if (console) free(console);
		console = strdup(strip_line(x));
		return;
	}

	/* TODO: Make console & tty dynamically loadable from /etc/finit.d */
	if (MATCH_CMD(line, "tty ", x)) {
		tty_register(strip_line(x));
		return;
	}
}

static void parse_dynamic(char *line, time_t mtime)
{
	char *x;

	/* Skip comments, i.e. lines beginning with # */
	if (MATCH_CMD(line, "#", x))
		return;

	/* Monitored daemon, will be respawned on exit */
	if (MATCH_CMD(line, "service ", x)) {
		service_register(SVC_TYPE_SERVICE, x, mtime, NULL);
		return;
	}

	/* One-shot task, will not be respawned */
	if (MATCH_CMD(line, "task ", x)) {
		service_register(SVC_TYPE_TASK, x, mtime, NULL);
		return;
	}

	/* Like task but waits for completion, useful w/ [S] */
	if (MATCH_CMD(line, "run ", x)) {
		service_register(SVC_TYPE_RUN, x, mtime, NULL);
		return;
	}

	/* Classic inetd service */
	if (MATCH_CMD(line, "inetd ", x)) {
#ifndef INETD_DISABLED
		service_register(SVC_TYPE_INETD, x, mtime, NULL);
#else
		_e("Finit built with inetd support disabled, cannot register service inetd %s!", x);
#endif
		return;
	}

	if (MATCH_CMD(line, "rlimit ", x)) {
		conf_parse_rlimit(x);
		return;
	}
}

static void tabstospaces(char *line)
{
	if (!line)
		return;

	for (int i = 0; line[i]; i++) {
		if (line[i] == '\t')
			line[i] = ' ';
	}
}

static int parse_conf_dynamic(char *file, time_t mtime)
{
	FILE *fp = fopen(file, "r");

	if (!fp) {
		_pe("Failed opening %s", file);
		return 1;
	}

	while (!feof(fp)) {
		char line[LINE_SIZE] = "";

		if (!fgets(line, sizeof(line), fp))
			continue;

		chomp(line);
		tabstospaces(line);
		_d("dyn conf: %s", line);

		parse_dynamic(line, mtime);
	}

	fclose(fp);

	return 0;
}

static int parse_conf(char *file)
{
	FILE *fp;
	char line[LINE_SIZE] = "";
	char *x;

	fp = fopen(file, "r");
	if (!fp)
		return 1;

	/*
	 * If not standard finit.conf, then we want to show just the base name
	 * Loading configuration ............. vs
	 * Loading services configuration ....
	 */
	if (!string_match (file, FINIT_CONF)) {
		/* Remove leading path */
		x = strrchr(file, '/');
		if (!x) x = file;
		else	x++;

		/* Remove ending .conf */
		strlcpy(line, x, sizeof(line));
		x = strstr(line, ".conf");
		if (x) *x = 0;

		/* Add empty space. */
		strcat(line, " ");
	}

	if (!silent)
		print(0, "Loading %sconfiguration", line);
	while (!feof(fp)) {
		if (!fgets(line, sizeof(line), fp))
			continue;

		chomp(line);
		tabstospaces(line);
		_d("conf: %s", line);

		parse_static(line);
		parse_dynamic(line, 0);
	}

	fclose(fp);

	return 0;
}

/* Reload all *.conf in /etc/finit.d/ */
void conf_reload_dynamic(void)
{
	int i, num;
	char *dir = rcsd;
	struct dirent **e;

	/* Mark and sweep */
	svc_mark_dynamic();

	num = scandir(dir, &e, NULL, alphasort);
	if (num < 0) {
		_d("No files found in %s, skipping ...", dir);
		return;
	}

	for (i = 0; i < num; i++) {
		char *name = e[i]->d_name;
		char  path[CMD_SIZE];
		struct stat st;

		snprintf(path, sizeof(path), "%s/%s", dir, name);

		/* Check that it's an actual file ... */
		if (stat(path, &st)) {
			_d("Cannot even read .conf file %s, skipping ...", path);
			continue;
		}
		if (S_ISEXEC(st.st_mode) || S_ISDIR(st.st_mode))
			continue;

		/* Check that file ends with '.conf' */
		if (strcmp(&path[strlen(path) - 5], ".conf")) {
			_d("File %s is not a .conf, skipping ... ", path);
			continue;
		}

		parse_conf_dynamic(path, st.st_mtime);
	}

	while (num--)
		free(e[num]);
	free(e);

	set_hostname(&hostname);
}

int conf_parse_config(void)
{
	username = strdup(DEFUSER);
	hostname = strdup(DEFHOST);

	return parse_conf(FINIT_CONF);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
