$(srcdir)/sig_list.txt:
	man 7 signal | perl -e 'while (<>) { print "$$_\n" for /(SIG\w+)/g }' | sort -u > $@.tmp && mv $@.tmp $@

$(srcdir)/controller_data.autogen.c: $(srcdir)/controller.c $(scriptdir)/generate_controller_data.pl
	perl $(scriptdir)/generate_controller_data.pl < $< > $@.tmp && mv $@.tmp $@

$(srcdir)/signal_data.autogen.c: $(srcdir)/sig_list.txt $(srcdir)/signal.c $(scriptdir)/generate_signal_data.pl
	$(PERL) $(scriptdir)/generate_signal_data.pl < $(srcdir)/sig_list.txt > $@.tmp && mv $@.tmp $@

$(srcdir)/options_data.autogen.c: $(srcdir)/options.c $(scriptdir)/generate_options_data.pl
	$(PERL) $(scriptdir)/generate_options_data.pl < $< > $@.tmp && mv $@.tmp $@

$(srcdir)/version_data.autogen.c: $(scriptdir)/../.git/index $(scriptdir)/../ChangeLog $(scriptdir)/generate_version_data.pl
	env PROJ_ROOT="$(scriptdir)/.." $(PERL) "$(scriptdir)/generate_version_data.pl" > $@.tmp && mv $@.tmp $@

autogen_files: $(sutogen_src)

.PHONY: autogen_files