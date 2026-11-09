/* Build like controller.test.c, replacing service.c with controller.c.
 * This test includes service.c below to force realloc to move or fail.
 */
#define main daemonproxy_main
#include "daemonproxy.c"
#undef main

static void *move_ptr;
static size_t move_len;
static int moves, fail_realloc;
static void *moving_realloc(void *ptr, size_t size) {
	if (ptr && ptr == move_ptr) {
		void *next;
		if (fail_realloc) return NULL;
		next= malloc(size);
		assert(next);
		memcpy(next, ptr, move_len < size? move_len : size);
		free(ptr);
		moves++;
		return next;
	}
	return realloc(ptr, size);
}
#define realloc moving_realloc
#include "service.c"
#undef realloc

static void force_move(service_t *svc) {
	move_ptr= (void*)svc->vars.data;
	move_len= svc->vars.len;
}

int main() {
	service_t *svc;
	svc_init();
	svc= svc_by_name(STRSEG("test"), true);
	assert(svc && svc_set_tags(svc, STRSEG("tag")));
	force_move(svc);
	assert(svc_set_argv(svc, STRSEG("/bin/true")));
	assert(moves == 1 && !strcmp(svc_get_tags(svc), "tag"));
	force_move(svc);
	assert(svc_set_fds(svc, STRSEG("stderr")));
	assert(moves == 2 && !strcmp(svc_get_argv(svc), "/bin/true"));
	force_move(svc);
	assert(svc_set_tags(svc, STRSEG("longer-tag")));
	assert(moves == 3 && !strcmp(svc_get_fds(svc), "stderr"));
	force_move(svc);
	assert(svc_set_argv(svc, STRSEG("/a/much/longer/path")));
	assert(moves == 4 && !strcmp(svc_get_tags(svc), "longer-tag"));
	force_move(svc);
	fail_realloc= 1;
	assert(!svc_set_tags(svc, STRSEG("this allocation must fail")));
	assert(svc->vars.data == move_ptr && svc->vars.len == move_len);
	assert(!strcmp(svc_get_tags(svc), "longer-tag"));
	assert(!strcmp(svc_get_argv(svc), "/a/much/longer/path"));
	assert(!strcmp(svc_get_fds(svc), "stderr"));
	fail_realloc= 0;
	assert(svc_set_tags(svc, STRSEG("")));
	assert(!strcmp(svc_get_tags(svc), ""));
	assert(!strcmp(svc_get_argv(svc), "/a/much/longer/path"));
	assert(svc_set_argv(svc, STRSEG("")));
	assert(!strcmp(svc_get_fds(svc), "stderr"));
	// Preserve historical PID metadata without retaining the live PID index.
	wake->now= 1LL << 32;
	svc->state= SVC_STATE_UP;
	svc_change_pid(svc, 12345);
	assert(svc_by_pid(12345) == svc);
	svc_handle_reaped(svc, 0);
	assert(!svc_by_pid(12345) && svc_get_pid(svc) == 12345);
	assert(!svc_send_signal(svc, SIGTERM, false));
	svc_delete(svc);
	return 0;
}
