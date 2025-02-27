/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (c) 2014 The CyanogenMod Project
 * Copyright (c) 2014-2015 Andreas Schneider <asn@cryptomilk.org>
 * Copyright (c) 2014-2015 Christopher N. Hesse <raymanfx@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>

#define LOG_TAG "SamsungPowerHAL"
/* #define LOG_NDEBUG 0 */
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#define BOOSTPULSE_PATH "/sys/devices/system/cpu/cpu0/cpufreq/interactive/boostpulse"

#define IO_IS_BUSY_PATH "/sys/devices/system/cpu/cpu0/cpufreq/interactive/io_is_busy"

#define CPU0_MAX_FREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define CPU0_MIN_FREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq"
#define CPU4_MAX_FREQ_PATH "/sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq"
#define CPU4_MIN_FREQ_PATH "/sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq"

#define TOUCHSCREEN_PATH "/sys/class/input/input2/enabled"
#define TOUCHKEY_PATH "/sys/class/input/input1/enabled"

struct samsung_power_module {
    struct power_module base;
    pthread_mutex_t lock;
    int boostpulse_fd;
    int boostpulse_warned;
    char cpu0_max_freq[10];
    char cpu0_min_freq[10];
    char cpu4_max_freq[10];
    char cpu4_min_freq[10];
    char* touchscreen_power_path;
    char* touchkey_power_path;
    bool touchkey_blocked;
};

/* POWER_HINT_LOW_POWER */
static bool low_power_mode = false;

/**********************************************************
 *** HELPER FUNCTIONS
 **********************************************************/

static int sysfs_read(char *path, char *s, int num_bytes)
{
    char errno_str[64];
    int len;
    int ret = 0;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        strerror_r(errno, errno_str, sizeof(errno_str));
        ALOGE("Error opening %s: %s\n", path, errno_str);

        return -1;
    }

    len = read(fd, s, num_bytes - 1);
    if (len < 0) {
        strerror_r(errno, errno_str, sizeof(errno_str));
        ALOGE("Error reading from %s: %s\n", path, errno_str);

        ret = -1;
    } else {
        s[len] = '\0';
    }

    close(fd);

    return ret;
}

static void sysfs_write(const char *path, char *s)
{
    char errno_str[64];
    int len;
    int fd;

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        strerror_r(errno, errno_str, sizeof(errno_str));
        ALOGE("Error opening %s: %s\n", path, errno_str);
        return;
    }

    len = write(fd, s, strlen(s));
    if (len < 0) {
        strerror_r(errno, errno_str, sizeof(errno_str));
        ALOGE("Error writing to %s: %s\n", path, errno_str);
    }

    close(fd);
}

/**********************************************************
 *** POWER FUNCTIONS
 **********************************************************/

/* You need to request the powerhal lock before calling this function */
static int boostpulse_open(struct samsung_power_module *samsung_pwr)
{
    char errno_str[64];

    if (samsung_pwr->boostpulse_fd < 0) {
        samsung_pwr->boostpulse_fd = open(BOOSTPULSE_PATH, O_WRONLY);
        if (samsung_pwr->boostpulse_fd < 0) {
            if (!samsung_pwr->boostpulse_warned) {
                strerror_r(errno, errno_str, sizeof(errno_str));
                ALOGE("Error opening %s: %s\n", BOOSTPULSE_PATH, errno_str);
                samsung_pwr->boostpulse_warned = 1;
            }
        }
    }

    return samsung_pwr->boostpulse_fd;
}

static void find_input_nodes(struct samsung_power_module *samsung_pwr, char *dir)
{
    const char filename[] = "name";
    char errno_str[64];
    struct dirent *de;
    char file_content[20];
    char *path = NULL;
    char *node_path = NULL;
    size_t pathsize;
    size_t node_pathsize;
    DIR *d;

    d = opendir(dir);
    if (d == NULL) {
        return;
    }

    while ((de = readdir(d)) != NULL) {
        if (strncmp(filename, de->d_name, sizeof(filename)) == 0) {
            pathsize = strlen(dir) + strlen(de->d_name) + 2;
            node_pathsize = strlen(dir) + strlen("enabled") + 2;

            path = malloc(pathsize);
            node_path = malloc(node_pathsize);
            if (path == NULL || node_path == NULL) {
                strerror_r(errno, errno_str, sizeof(errno_str));
                ALOGE("Out of memory: %s\n", errno_str);
                return;
            }

            snprintf(path, pathsize, "%s/%s", dir, filename);
            sysfs_read(path, file_content, sizeof(file_content));

            snprintf(node_path, node_pathsize, "%s/%s", dir, "enabled");

            if (strncmp(file_content, "sec_touchkey", 12) == 0) {
                ALOGV("%s: found touchkey path: %s\n", __func__, node_path);
                samsung_pwr->touchkey_power_path = malloc(node_pathsize);
                if (samsung_pwr->touchkey_power_path == NULL) {
                    strerror_r(errno, errno_str, sizeof(errno_str));
                    ALOGE("Out of memory: %s\n", errno_str);
                    return;
                }
                snprintf(samsung_pwr->touchkey_power_path, node_pathsize, node_path);
            }

            if (strncmp(file_content, "sec_touchscreen", 15) == 0) {
                ALOGV("%s: found touchscreen path: %s\n", __func__, node_path);
                samsung_pwr->touchscreen_power_path = malloc(node_pathsize);
                if (samsung_pwr->touchscreen_power_path == NULL) {
                    strerror_r(errno, errno_str, sizeof(errno_str));
                    ALOGE("Out of memory: %s\n", errno_str);
                    return;
                }
                snprintf(samsung_pwr->touchscreen_power_path, node_pathsize, node_path);
            }
        }
    }

    if (path)
        free(path);
    if (node_path)
        free(node_path);
    closedir(d);
}

/**********************************************************
 *** INIT FUNCTIONS
 **********************************************************/

static void init_cpufreqs(struct samsung_power_module *samsung_pwr)
{
    int rc;
    struct stat sb;

    sysfs_read(CPU0_MAX_FREQ_PATH, samsung_pwr->cpu0_max_freq,
               sizeof(samsung_pwr->cpu0_max_freq));
    sysfs_read(CPU0_MIN_FREQ_PATH, samsung_pwr->cpu0_min_freq,
               sizeof(samsung_pwr->cpu0_min_freq));
    ALOGV("%s: CPU 0 max freq: %s\n", __func__, samsung_pwr->cpu0_max_freq);
    ALOGV("%s: CPU 0 min freq: %s\n", __func__, samsung_pwr->cpu0_min_freq);

    rc = stat(CPU4_MAX_FREQ_PATH, &sb);
    if (rc == 0) {
        sysfs_read(CPU4_MAX_FREQ_PATH, samsung_pwr->cpu4_max_freq,
                   sizeof(samsung_pwr->cpu4_max_freq));
        sysfs_read(CPU4_MIN_FREQ_PATH, samsung_pwr->cpu4_min_freq,
                   sizeof(samsung_pwr->cpu4_min_freq));
        ALOGV("%s: CPU 4 max freq: %s\n", __func__, samsung_pwr->cpu4_max_freq);
        ALOGV("%s: CPU 4 min freq: %s\n", __func__, samsung_pwr->cpu4_min_freq);
    }
}

static void init_touch_input_power_path(struct samsung_power_module *samsung_pwr)
{
    char dir[1024];
    char errno_str[64];
    uint32_t i;

    for (i = 0; i < 20; i++) {
        snprintf(dir, sizeof(dir), "/sys/class/input/input%d", i);
        find_input_nodes(samsung_pwr, dir);
    }
}

/*
 * The init function performs power management setup actions at runtime
 * startup, such as to set default cpufreq parameters.  This is called only by
 * the Power HAL instance loaded by PowerManagerService.
 */
static void samsung_power_init(struct power_module *module)
{
    struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;

    init_cpufreqs(samsung_pwr);
    init_touch_input_power_path(samsung_pwr);
}

/*
 * The setInteractive function performs power management actions upon the
 * system entering interactive state (that is, the system is awake and ready
 * for interaction, often with UI devices such as display and touchscreen
 * enabled) or non-interactive state (the system appears asleep, display
 * usually turned off).  The non-interactive state is usually entered after a
 * period of inactivity, in order to conserve battery power during such
 * inactive periods.
 *
 * Typical actions are to turn on or off devices and adjust cpufreq parameters.
 * This function may also call the appropriate interfaces to allow the kernel
 * to suspend the system to low-power sleep state when entering non-interactive
 * state, and to disallow low-power suspend when the system is in interactive
 * state.  When low-power suspend state is allowed, the kernel may suspend the
 * system whenever no wakelocks are held.
 *
 * on is non-zero when the system is transitioning to an interactive / awake
 * state, and zero when transitioning to a non-interactive / asleep state.
 *
 * This function is called to enter non-interactive state after turning off the
 * screen (if present), and called to enter interactive state prior to turning
 * on the screen.
 */
static void samsung_power_set_interactive(struct power_module *module, int on)
{
    struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;
    struct stat sb;
    char buf[80];
    char touchkey_node[2];
    int touchkey_enabled;
    int rc;

    ALOGV("power_set_interactive: %d\n", on);

    sysfs_write(samsung_pwr->touchscreen_power_path, on ? "1" : "0");

    rc = stat(samsung_pwr->touchkey_power_path, &sb);
    if (rc < 0) {
        goto out;
    }

    if (!on) {
        if (sysfs_read(TOUCHKEY_PATH, touchkey_node, sizeof(touchkey_node)) == 0) {
            touchkey_enabled = touchkey_node[0] - '0';
            /*
             * If touchkey_enabled is 0, the keys have been disabled by another component
             * (for example cmhw), which means we don't want them to be enabled when resuming
             * from suspend.
             */
            if (touchkey_enabled == 0) {
                samsung_pwr->touchkey_blocked = true;
            } else {
                samsung_pwr->touchkey_blocked = false;
                sysfs_write(samsung_pwr->touchkey_power_path, "0");
            }
        }
    } else if (!samsung_pwr->touchkey_blocked) {
        sysfs_write(samsung_pwr->touchkey_power_path, "1");
    }

    sysfs_write(IO_IS_BUSY_PATH, on ? "1" : "0");

out:
    ALOGV("power_set_interactive: %d done\n", on);
}

/*
 * The powerHint function is called to pass hints on power requirements, which
 * may result in adjustment of power/performance parameters of the cpufreq
 * governor and other controls.
 *
 * The possible hints are:
 *
 * POWER_HINT_VSYNC
 *
 *     Foreground app has started or stopped requesting a VSYNC pulse
 *     from SurfaceFlinger.  If the app has started requesting VSYNC
 *     then CPU and GPU load is expected soon, and it may be appropriate
 *     to raise speeds of CPU, memory bus, etc.  The data parameter is
 *     non-zero to indicate VSYNC pulse is now requested, or zero for
 *     VSYNC pulse no longer requested.
 *
 * POWER_HINT_INTERACTION
 *
 *     User is interacting with the device, for example, touchscreen
 *     events are incoming.  CPU and GPU load may be expected soon,
 *     and it may be appropriate to raise speeds of CPU, memory bus,
 *     etc.  The data parameter is unused.
 *
 * POWER_HINT_LOW_POWER
 *
 *     Low power mode is activated or deactivated. Low power mode
 *     is intended to save battery at the cost of performance. The data
 *     parameter is non-zero when low power mode is activated, and zero
 *     when deactivated.
 *
 * POWER_HINT_CPU_BOOST
 *
 *     An operation is happening where it would be ideal for the CPU to
 *     be boosted for a specific duration. The data parameter is an
 *     integer value of the boost duration in microseconds.
 */
static void samsung_power_hint(struct power_module *module,
                                  power_hint_t hint,
                                  void *data)
{
    struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;
    char errno_str[64];
    int len;

    switch (hint) {
        case POWER_HINT_INTERACTION: {

            ALOGV("%s: POWER_HINT_INTERACTION", __func__);

            if (boostpulse_open(samsung_pwr) >= 0) {
                len = write(samsung_pwr->boostpulse_fd, "1", 1);

                if (len < 0) {
                    strerror_r(errno, errno_str, sizeof(errno_str));
                    ALOGE("Error writing to %s: %s\n", BOOSTPULSE_PATH, errno_str);
                }
            }

            break;
        }
        case POWER_HINT_VSYNC: {

            ALOGV("%s: POWER_HINT_VSYNC", __func__);

            break;
        }
        case POWER_HINT_LOW_POWER: {
            int rc;
            struct stat sb;

            ALOGV("%s: POWER_HINT_LOW_POWER", __func__);

            pthread_mutex_lock(&samsung_pwr->lock);

            if (data) {
                sysfs_write(CPU0_MAX_FREQ_PATH, samsung_pwr->cpu0_min_freq);
                rc = stat(CPU4_MAX_FREQ_PATH, &sb);
                if (rc == 0) {
                    sysfs_write(CPU4_MAX_FREQ_PATH, samsung_pwr->cpu4_min_freq);
                }
            } else {
                sysfs_write(CPU0_MAX_FREQ_PATH, samsung_pwr->cpu0_max_freq);
                rc = stat(CPU4_MAX_FREQ_PATH, &sb);
                if (rc == 0) {
                    sysfs_write(CPU4_MAX_FREQ_PATH, samsung_pwr->cpu4_max_freq);
                }
            }
            low_power_mode = data;

            pthread_mutex_unlock(&samsung_pwr->lock);
            break;
        }
        default:
            break;
    }
}

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct samsung_power_module HAL_MODULE_INFO_SYM = {
    .base = {
        .common = {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = POWER_MODULE_API_VERSION_0_2,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = POWER_HARDWARE_MODULE_ID,
            .name = "Samsung Power HAL",
            .author = "The CyanogenMod Project",
            .methods = &power_module_methods,
        },

        .init = samsung_power_init,
        .setInteractive = samsung_power_set_interactive,
        .powerHint = samsung_power_hint,
    },

    .lock = PTHREAD_MUTEX_INITIALIZER,
    .boostpulse_fd = -1,
    .boostpulse_warned = 0,
};
