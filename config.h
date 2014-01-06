#define _GNU_SOURCE

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>

#define SERVICE_POOL_SIZE           256
#define SERVICE_OBJ_SIZE            512
#define SERVICE_RESTART_DELAY        (2 * 1000000)
#define FD_POOL_SIZE                256
#define FD_OBJ_SIZE                  96
#define NAME_MAX                     63
#define FORK_RETRY_DELAY             (3 * 1000000)
#define CONTROLLER_WRITE_TIMEOUT    (30 * 1000000)
#define CONTROLLER_IN_BUF_SIZE     1024
#define CONTROLLER_OUT_BUF_SIZE    1024
#define CONTROLLER_DEFAULT_PATH    "/sbin/init-frame-controller"
#define CONFIG_FILE_DEFAULT_PATH   "/etc/init-frame.conf"
