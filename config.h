#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>

#define SERVICE_DATA_BUF_SIZE           256
#define SERVICE_RESTART_DELAY (2 * 1000000)
#define FD_NAME_BUF_SIZE                 32
#define FORK_RETRY_DELAY      (3 * 1000000)
#define CONTROLLER_IN_BUF_SIZE         1024
#define CONTROLLER_OUT_BUF_SIZE        1024
#define CONTROLLER_DEFAULT_PATH "/sbin/init-frame-controller"
#define CONFIG_FILE_DEFAULT_PATH "/etc/init-frame.conf"
