#include "config.h"
#include "daemonproxy.h"

static void parse_option(char shortname, char* longname, char ***argv);

struct option_table_entry_s {
	char shortname;
	const char *longname;
	const char *argname;
	void (*handler)(char **argv);
	const char *help;
};

#include "options_data.autogen.c"

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

=item -c FILENAME

=item --config FILENAME

Read command stream from FILENAME at startup.  The config file is used only
once and cannot be "re-loaded" later.

=cut
*/
void set_opt_configfile(char** argv ) {
	struct stat st;
	if (stat(argv[0], &st))
		fatal(EXIT_BAD_OPTIONS, "Cannot stat configfile \"%s\"", argv[0]);
	main_cfgfile= argv[0];
}

/*
=item --stdin

Use STDIN+STDOUT as a controller communication pipe.

=cut
*/
void set_opt_stdin(char **argv) {
	main_use_stdin= true;
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

	main_terminate_guard= n;
}

/*
=item -E TSV_ARGS

=item --exit-exec TSV_ARGS

exec() args in any trappable exit scenario.

This causes daemonproxy to exec into another program on any condition which
would otherwise cause daemonproxy to exit.  Tis includes anything from normal
program termination to fatal signals like SIGSEGV.

=cut
*/
void set_opt_exec_on_exit(char **argv) {
	if (!set_exec_on_exit(STRSEG(argv[0])))
		fatal(EXIT_BAD_OPTIONS, "exec-on-exit arguments exceed buffer size");
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
	int64_t val_n, val_m= FD_OBJ_SIZE;
	strseg_t arg= STRSEG(argv[0]), arg_n;

	if (strseg_tok_next(&arg, 'x', &arg_n)?
		!(strseg_atoi(&arg_n, &val_n) && arg_n.len == 0
			&& strseg_parse_size(&arg, &val_m) && arg.len == 0)
		: !(strseg_atoi(&arg_n, &val_n) && arg_n.len == 0)
	) {
		fatal(EXIT_BAD_OPTIONS, "Expected 'N' or 'NxM' where N and M are integers and M has an optional size suffix");
	}

	if (val_n < FD_POOL_SIZE_MIN) {
		log_warn("At least %d fd objects required; using minimum", FD_POOL_SIZE_MIN);
		val_n= FD_POOL_SIZE_MIN;
	} else if (val_n > FD_POOL_SIZE_MAX) {
		log_warn("fd pool size exceeds max number of allowed file descriptors; limiting to %d", FD_POOL_SIZE_MAX);
		val_n= FD_POOL_SIZE_MAX;
	}

	if (val_m < min_fd_obj_size) {
		log_warn("fd obj size increased to minimum of %d", min_fd_obj_size);
		val_m= min_fd_obj_size;
	} else if (val_m > max_fd_obj_size) {
		log_warn("fd obj size limited to maximum of %d", max_fd_obj_size);
		val_m= max_fd_obj_size;
	}

	main_fd_pool_count= (int) val_n;
	main_fd_pool_size_each= (int) val_m;
}

/*
=item -M

=item --mlockall

Call mlockall() after allocating structures.

This is primarily intended for use with the fixed-size memory pools for
services and file handles when running as process 1.

=cut
*/
void set_opt_mlockall(char **argv) {
	main_mlockall= true;
}

/*
=item -v

=item --verbose

Enable another level of logging output.

=cut
*/
void set_opt_verbose(char** argv) {
	log_set_filter(log_filter-1);
}

/*
=item -q

=item --quiet

Suppress another level of logging output.

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
	printf("daemonproxy version %s\noptions:\n", version_git_tag);
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
	printf("daemonproxy version %s\n"
		" build timestamp: %lld (%4d-%02d-%02d %02d:%02d:%02d)\n"
		" git HEAD: %s\n",
		version_git_tag, (long long) version_build_ts,
		cal.tm_year+1900, cal.tm_mon+1, cal.tm_mday, cal.tm_hour, cal.tm_min, cal.tm_sec,
		version_git_head);
	
	// now exit, unless they also specified exec-on-exit
	fatal(EXIT_NO_OP, "");
}

/*
=back

=cut
*/
