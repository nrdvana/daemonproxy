daemonproxy
===========

A tight efficient process manager driven by external scripts

philosophy
==========

This process manager is written in the spirit of daemontools,
much like runit, s6, perp, and others.

Their common philosophy is that the one true way to monitor a
daemon is for the daemon to be a child of a process manager,
such that the process manager receives SIGCHLD the moment the
daemon exits, sees the exit status of the daemon, and can take
appropriate steps to restart the daemon.

The other semi-common philosophy is that the process manager
should be able to restart a daemon connected to the same output
pipe (for logging) that it was originally connected to, and
also give it an identical environment each time it is started.
(this is a thing that is hard to guarantee with init scripts,
 which could be executed from any context)

The point at which daemonproxy diverges from the rest is that
it generates a stream of events which can be piped to an
external script that does the actual service-management logic,
and is in turn controlled by this script.  Thus, it is a proxy
for daemon management.

configuration
=============

While the filesystem-based configuration of daemontools and friends
is a nice simple and unix-friendly way to configure services,
there are times when setting up a tree of special scripts,
directories, and control files with special filesystem flags
is something of a hassle.  Particularly when you want to commit
them to version control (losing the special permission flags),
or want them to live on a read-only filesystem.

While each of those tools have various workarounds available,
nothing beats a plain old config file, which can be committed
to version control, stored on read-only media, or even generated
by a script on the fly.  daemonproxy makes this especially easy
by even allowing the configuration to come from stdin.

In fact, daemonproxy's config file isn't really even a config
file; it is just a stream of commands that might happen to be
read from a file (or stdin, or a variety of other inputs).
These commands might configure new services and they might
start some of them.

But, the best feature of all is that commands can configure a
"controller" service which can then issue further commands to
daemonproxy over a socket.  The controller script can be written
in whatever language you like with whatever fancy libraries you
like, without needing to add all those details to daemonproxy
itself.

daemonproxy takes over the important role of the parent process,
and can act as a watchdog for your script, while maintaining the
state of all the services and file handles that connect them to
their loggers.  It also condenses signals and other events into a
nice event stream (tab-delimited text) so your script doesn't have
to deal with the complications of signal handling, nonblocking I/O,
or waiting for children.  All you have to do is read stdin and
write stdout!

advantages over other supervisors
=================================

Daemonproxy is really just a platform for your supervisor, requiring some
additional scripting effort to build a complete program.  What makes the
effort worthwhile?

Here's a quick list:

  * The more RAM a supervisor uses, the more likely it is to get OOM-killed by
    the kernel, and the slower it forks. (because of all the page table entries
    that need to change)  Daemonproxy uses about 8M address space and 400K
    resident (on a system where 'sleep' uses 4M address space and 300K resident)
  
  * If a supervisor makes use of lots of fun/powerful libraries, it adds lots
    of potential failure points.  If the supervisor dies, all the child
    processes become orphaned, and it can be a mess to clean up.  By splitting
    the supervisor into a reliable parent and a controller script, you can take
    more risks on library usage without worrying about catastrophic failures.
    If anything goes wrong with the controller, daemonproxy can restart it and
    resynchronize, so the controller picks up where it left off.
  
  * If you want to start a service with a few extra pipes connected to things,
    other supervision programs' narrow designs won't let you.  Daemonproxy will
    let you create any number of file/pipe/socket handles and connect them
    between services however you like.  Want to set up 8 services which all
    pipe to the same logger who receives the pipes on FD 3 through 10?  no
    problem!  Want to create a service which has a port-80 bound socket on fd
    3, a read-only secret key file on fd 4, and a pipe to an authentication
    server on fd 5?  easy!  By setting up pipes and socketpairs within
    daemonproxy, you avoid the security hassle of creating user accounts and
    managing directory permissions of sockets in the filesystem.

  * daemonproxy is more suitable for small embedded systems than larger
    supervisors like systemd or upstart.  And while daemonproxy is well-suited
    for replacing init, it can be used for any number of supervision roles, and
    doesn't need to be system-wide or run as root.
    
  * daemonproxy is more flexible than simpler supervisors like daemontools,
    runit, etc. since it gives you the ability to script your own supervision
    logic, which could load service definitions from a YAML file or from a
    database.

init replacement
================

Daemonproxy is intended for lots of purposes, but I added a few special
features for acting as process 1 on embedded systems:

  * command-line options to allocate a fixed number of objects of a fixed size
    at startup, so it never needs to call malloc again.
  * a "terminate-guard" feature that prevents it from accidentally exiting
  * an "exec-on-exit" feature that can exec into an emergency cleanup program
    on fatal errors (or just when you want to shut down the system).

And design-wise, it is a single-thread process written in non-blocking style as
a collection of state machines, and it has no external library dependencies, so
it is a natural fit for static compilation.  It also has relatively few lines
of code.

