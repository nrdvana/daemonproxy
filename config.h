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
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SERVICE_POOL_SIZE           256
#define SERVICE_OBJ_SIZE            512
#define SERVICE_RESTART_DELAY        (2 * 1000000)
#define FD_POOL_SIZE                256
#define FD_OBJ_SIZE                  96
#define NAME_MAX                     63
#define FORK_RETRY_DELAY             (3 * 1000000)
#define CONTROLLER_WRITE_TIMEOUT    (30 * 1000000)
#define CONTROLLER_RECV_BUF_SIZE   1024
#define CONTROLLER_SEND_BUF_SIZE   2048
#define CONTROLLER_MAX_CLIENTS        3
#define CONTROLLER_SOCKET_NAME	"controller.sock"
#define CONFIG_FILE_DEFAULT_PATH   "/etc/init-frame.conf"
