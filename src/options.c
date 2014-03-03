/* options.c - routines for parsing daemonproxy commandline options
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#include "config.h"
#include "daemonproxy.h"

/* The following global variables can be set by option processing.
 * Some can also be altered by controller commands.
 */

bool        opt_daemonize= false;
int         opt_fd_pool_count= 0;
int         opt_fd_pool_size_each= 0;
int         opt_svc_pool_count= 0;
int         opt_svc_pool_size_each= 0;
const char *opt_socket_path= NULL;
const char *opt_config_file= NULL;
bool        opt_exec_on_exit= false;
char        opt_exec_on_exit_buf[256];
strseg_t    opt_exec_on_exit_args;
bool        opt_interactive= false;
bool        opt_mlockall= false;
int64_t     opt_terminate_guard= 0;

static void parse_option(char shortname, char* longname, char ***argv);

/* The script generate_options_data.pl reads this source file to generate the
 * options table.  It parses the POD to determine short, long, argname, and
 * usage text.
 *
 * The manual page is also generated from this POD.
 */

struct option_table_entry_s {
	char shortname;
	const char *longname;
	const char *argname;
	void (*handler)(char **argv);
	const char *help;
};

#include "options_data.autogen.c"

/** Parse options from main's argv.
 */
void parse_opts(char **argv) {
	char *current;
	
	while ((current= *argv++)) {
		if (current[0] == '-') {
			if (current[1] == '-')
				parse_option(0, current+2, &argv);
			else for (++current; *current; current++)
				parse_option(*current, NULL, &argv);
		}
		else
			// if failsafe, will not exit()
			fatal(EXIT_BAD_OPTIONS, "Unexpected argument \"%s\"\n", current);
	}
}

void parse_option(char shortname, char* longname, char ***argv) {
	const struct option_table_entry_s *entry;
	int i, argc;
	
	for (entry= option_table; entry->handler; entry++) {
		if ((shortname && (shortname == entry->shortname))
			|| (longname && (0 == strcmp(longname, entry->longname)))
		) {
			argc= entry->argname? 1 : 0;
			for (i= 0; i < argc; i++)
				if (!(*argv)[i]) {
					fatal(EXIT_BAD_OPTIONS, "Missing argument for -%c%s",
						longname? '-' : shortname, longname? longname : "");
				}
			entry->handler(*argv);
			(*argv)+= argc;
			return;
		}
	}
	fatal(EXIT_BAD_OPTIONS, "Unknown option -%c%s  (see --help)", longname? '-' : shortname, longname? longname : "");
}

/*
=head1 OPTIONS

=over 24

=item -c

=item --config FILENAME

Read command stream from FILENAME at startup.  The config file is used only
once and cannot be "re-loaded" later.

=cut
*/
void set_opt_configfile(char** argv ) {
	struct stat st;
	if (strcmp(argv[0], "-") != 0)
		if (stat(argv[0], &st))
			fatal(EXIT_BAD_OPTIONS, "Cannot stat configfile \"%s\"", argv[0]);
	opt_config_file= argv[0];
}

/*
=item -i

=item --interactive

Use STDIN+STDOUT as a controller communication pipe.  Daemonproxy will
terminate at EOF (unless exit-guard is set; then it will keep running
in the background).

=cut
*/
void set_opt_stdin(char **argv) {
	opt_interactive= true;
}

/*
=item -S

=item --socket PATH

Listen on PATH for controller connections.  This is not needed for your
controller script, but might be helpful for debugging or receiving external
events (but your controller script is the better place to receive external
events).  By default, daemonproxy doesn't listen to anything.

=cut
*/
void opt_set_socket_path(char **argv) {
	opt_socket_path= argv[0];
}

/*
=item -D

=item --daemonize

Fork into the background.  This prints the new PID on stdout, closes stdin,
stdout, and stderr, and calls setsid() to become a session leader.

This option cannot be used when running as PID 1, and is incompatible with
--interactive, and suppresses the default logging.  (see: log.dest command)

=cut
*/
void opt_set_daemonize(char **argv) {
	opt_daemonize= true;
}

/*
=item --exit-guard INTEGER

Guard against accidental termination.  INTEGER must be a nonzero integer, up
to 64 bits in length.  With this feature enabled, daemonproxy will refuse to
exit for any reason less than a fatal signal.  The integer must be supplied
in order to disable the feature or ask daemonproxy to exit.

=cut
*/
void set_opt_failsafe(char **argv) {
	char *end= NULL;
	long n= strtol(argv[0], &end, 10);
	
	if (*end)
		fatal(EXIT_BAD_OPTIONS, "Terminate guard must be an integer");

	opt_terminate_guard= n;
}

/*
=item -E

=item --exit-exec TSV_ARGS

exec() args in any trappable exit scenario.

This causes daemonproxy to exec into another program on any condition which
would otherwise cause daemonproxy to exit.  This includes anything from normal
program termination to fatal signals like SIGSEGV.

=cut
*/
void set_opt_exec_on_exit(char **argv) {
	if (!set_exec_on_exit(STRSEG(argv[0])))
		fatal(EXIT_BAD_OPTIONS, "exec-on-exit arguments exceed buffer size");
}

bool set_exec_on_exit(strseg_t args) {
	int i;
	
	// empty string disables the feature
	if (args.len <= 0) {
		opt_exec_on_exit= false;
		return true;
	}
	
	// Stored in a fixed-size buffer...
	if (args.len >= sizeof(opt_exec_on_exit_buf))
		return false;

	memcpy(opt_exec_on_exit_buf, args.data, args.len);
	opt_exec_on_exit_buf[args.len]= '\0';
	
	// convert tab-delimited arguments to NUL-delimited
	for (i= 0; i < args.len; i++)
		if (opt_exec_on_exit_buf[i] == '\t')
			opt_exec_on_exit_buf[i]= '\0';
	
	opt_exec_on_exit= true;
	opt_exec_on_exit_args= (strseg_t){ opt_exec_on_exit_buf, args.len };
	return true;
}

static void parse_NxM(strseg_t arg, int64_t *count, int64_t *size) {
	strseg_t count_str;

	if (!strseg_tok_next(&arg, 'x', &count_str)
		|| !strseg_atoi(&count_str, count)
		|| count_str.len > 0
		|| !(
			arg.len <= 0
			|| (strseg_parse_size(&arg, size) && arg.len == 0)
		))
		fatal(EXIT_BAD_OPTIONS, "Expected 'N' or 'NxM' where N and M are integers and M has an optional size suffix");
}

/*
=item --fd-pool N[xM]

Pre-allocate N named handles [of M bytes each].  This sets the total allocation
size for file descriptor objects, and prevents further dynamic allocations.
It also restricts you to a fixed number of total handle objects, each of a fixed
size that might not be large enough for long filenames.

=cut
*/
void set_opt_fd_prealloc(char **argv) {
	int64_t val_n, val_m= FD_DATA_SIZE_DEFAULT;

	parse_NxM(STRSEG(argv[0]), &val_n, &val_m);

	if (val_n < FD_POOL_SIZE_MIN) {
		log_warn("At least %d fd objects required; using minimum", FD_POOL_SIZE_MIN);
		val_n= FD_POOL_SIZE_MIN;
	} else if (val_n > FD_POOL_SIZE_MAX) {
		log_warn("fd pool size exceeds max number of allowed file descriptors; limiting to %d", FD_POOL_SIZE_MAX);
		val_n= FD_POOL_SIZE_MAX;
	}

	if (val_m < FD_DATA_SIZE_MIN) {
		log_warn("fd obj size increased to minimum of %d", FD_DATA_SIZE_MIN);
		val_m= FD_DATA_SIZE_MIN;
	} else if (val_m > FD_DATA_SIZE_MAX) {
		log_warn("fd obj size limited to maximum of %d", FD_DATA_SIZE_MAX);
		val_m= FD_DATA_SIZE_MAX;
	}

	opt_fd_pool_count= (int) val_n;
	opt_fd_pool_size_each= (int) val_m;
}

/*
=item --service-pool N[xM]

Pre-allocate N services [of M bytes each].  This sets the total allocation
size for service objects, and prevents further dynamic allocations.
It also restricts you to a fixed number of total services, each of a fixed
size that might not be large enough for long argument lists.

=cut
*/
void set_opt_svc_prealloc(char **argv) {
	int64_t val_n, val_m= SERVICE_DATA_SIZE_DEFAULT;

	parse_NxM(STRSEG(argv[0]), &val_n, &val_m);

	if (val_n < SERVICE_POOL_SIZE_MIN) {
		log_warn("At least %d service objects required; using minimum", SERVICE_POOL_SIZE_MIN);
		val_n= SERVICE_POOL_SIZE_MIN;
	} else if (val_n > SERVICE_POOL_SIZE_MAX) {
		log_warn("service pool size exceeds maximum; limiting to %d", SERVICE_POOL_SIZE_MAX);
		val_n= SERVICE_POOL_SIZE_MAX;
	}

	if (val_m < SERVICE_DATA_SIZE_MIN) {
		log_warn("servce obj size increased to minimum of %d", SERVICE_DATA_SIZE_MIN);
		val_m= SERVICE_DATA_SIZE_MIN;
	}

	opt_svc_pool_count= (int) val_n;
	opt_svc_pool_size_each= (int) val_m;
}

/*
=item -M

=item --mlockall

Call mlockall() after allocating structures.  This is primarily intended for
use with --fd-pool or --service-pool when running as process 1.

=cut
*/
void set_opt_mlockall(char **argv) {
	opt_mlockall= true;
}

/*
=item -v

=item --verbose

Enable another level of logging output.  (see also: log.filter command)

=cut
*/
void set_opt_verbose(char** argv) {
	log_set_filter(log_filter-1);
}

/*
=item -q

=item --quiet

Suppress another level of logging output.  (see also: log.filter command)

=cut
*/
void set_opt_quiet(char** argv) {
	log_set_filter(log_filter+1);
}

/*
=item --help

Quick usage synopsis.

=cut
*/
void show_help(char **argv) {
	printf("daemonproxy version %d.%d.%d%s\noptions:\n",
		version_major, version_minor, version_release, version_suffix);
	const struct option_table_entry_s *entry;
	for (entry= option_table; entry->handler; entry++)
		if (entry->help)
			printf("  %c%c --%-12s %-8s  %s\n",
				entry->shortname? '-':' ', entry->shortname? entry->shortname : ' ',
				entry->longname, entry->argname? entry->argname : "", entry->help);
	puts("");
	
	// now exit, unless they also specified exec-on-exit
	fatal(EXIT_NO_OP, "");
}

/*
=item --version

Print complete version information.  First line will remain a consistent format,
other text is subject to change.

=cut
*/
void show_version(char **argv) {
	struct tm cal;
	localtime_r(&version_build_ts, &cal);
	printf("daemonproxy version %d.%d.%d%s\n"
		" build timestamp: %lld (%4d-%02d-%02d %02d:%02d:%02d)\n"
		" git HEAD: %s%s\n",
		version_major, version_minor, version_release, version_suffix,
		(long long) version_build_ts,
		cal.tm_year+1900, cal.tm_mon+1, cal.tm_mday, cal.tm_hour, cal.tm_min, cal.tm_sec,
		version_git_head, version_git_dirty? " (dirty)":"");
	
	// now exit, unless they also specified exec-on-exit
	fatal(EXIT_NO_OP, "");
}

/*
=back

=cut
*/
