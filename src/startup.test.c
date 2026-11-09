/* gcc -DCURRENT_UNIX_TIMESTAMP=0 -Ibuild -Isrc src/startup.test.c \
 *   src/fd.c src/service.c src/signal.c src/controller.c src/Contained_RBTree.c \
 *   src/log.c src/strseg.c src/options.c src/control-socket.c \
 *   src/version_data.autogen.c -o build/startup.test
 */
#define main daemonproxy_main
#include "daemonproxy.c"
#undef main

int main() {
	int mask, i, status;
	assert(setup_standard_fds());
	for (mask= 1; mask < 8; mask++) {
		pid_t pid= fork();
		assert(pid >= 0);
		if (!pid) {
			struct stat original[3], now, null_stat;
			bool seen[FD_SETSIZE]= { false };
			fd_t *fd= NULL;
			int anchor= open("/dev/null", O_RDONLY);
			assert(anchor >= 3);
			for (i= 0; i < 3; i++) {
				assert(fstat(i, &original[i]) == 0);
				if (mask & (1 << i)) close(i);
			}
			assert(setup_standard_fds());
			fd_init();
			assert(fd_init_special_handles() && fd_dev_null >= 3);
			assert(register_open_fds());
			assert(fstat(fd_dev_null, &null_stat) == 0);
			for (i= 0; i < 3; i++) {
				const char *name= i == 0? "stdin" : i == 1? "stdout" : "stderr";
				assert(fd_get_fdnum(fd_by_name(STRSEG(name))) == i);
				assert(fstat(i, &now) == 0);
				if (mask & (1 << i)) {
					assert(S_ISCHR(now.st_mode) && now.st_rdev == null_stat.st_rdev);
					assert((fcntl(i, F_GETFL) & O_ACCMODE) == O_RDWR);
				}
				else
					assert(now.st_dev == original[i].st_dev && now.st_ino == original[i].st_ino);
			}
			while ((fd= fd_iter_next(fd, ""))) {
				int num= fd_get_fdnum(fd);
				if (num < 0) continue;
				assert(num < FD_SETSIZE && !seen[num]);
				seen[num]= true;
			}
			for (i= 0; i < 3; i++)
				fd_delete(fd_by_name(i == 0? STRSEG("stdin") : i == 1? STRSEG("stdout") : STRSEG("stderr")));
			assert(fcntl(anchor, F_GETFD) >= 0);
			assert(fcntl(fd_dev_null, F_GETFD) >= 0);
			_exit(0);
		}
		assert(waitpid(pid, &status, 0) == pid);
		assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	return 0;
}
