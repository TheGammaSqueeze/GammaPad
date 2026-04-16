#include "gammapad_config.h"
#include "gammapad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <linux/input.h>
#include <sys/stat.h>
#include "input-event-codes.h"
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h> 
#include <limits.h>

/* External globals and functions from your project */
extern int controllerFd;
extern void destroy_virtual_device(int fd);
extern int create_virtual_controller(int *fd_out);

extern char* g_uiname;
extern int g_uibus;
extern int g_uivid;
extern int g_uiproduct;
extern int g_uiversion;

extern int g_ffPwmEnabled;
extern int g_ffPwmMaxMagnitude;

extern void triggerCalibration(void);

extern int g_deadzone;

extern int g_physCount;
extern int g_physFds[];

extern int g_epfd;

/* NEW: Declare aggregatorReuploadAllEffects() from gammapad_ff.c */
extern void aggregatorReuploadAllEffects(void);

/* Mutex to synchronize updates to global configuration values */
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CONFIG_DIR "/data/GammaPad"
#define EVENT_BUF_LEN (1024 * (sizeof(struct inotify_event) + NAME_MAX + 1))
#define MAPPINGS_FILE "MAPPINGS"
#define BITS_PER_LONG (sizeof(unsigned long)*8)

/* Custom key-mapping array: -1 = no mapping */
int g_customKeyMap[KEY_MAX + 1];

/* Helper to (re)initialize mapping to “no mapping” */
static void init_custom_key_mappings(void) {
    for (int i = 0; i <= KEY_MAX; i++) {
        g_customKeyMap[i] = -1;
    }
}

/* Auto-generated name→code table. Update via generate_key_name_map.sh */
static const struct {
    const char *name;
    int code;
} key_name_map[] = {
#include "key_name_map.inc"
};
static const int key_name_map_size =
    sizeof(key_name_map) / sizeof(key_name_map[0]);

/* Dump every supported key from the source pad into MAPPINGS */
static void dump_default_mappings(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_DIR, MAPPINGS_FILE);
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("Creating default MAPPINGS");
        return;
    }

    /* Pick the real source pad FD if we have one, else fall back */
    int srcFd = (g_physCount > 0 ? g_physFds[0] : controllerFd);

    /* Ask the physical device which keys it supports */
    unsigned long bits[(KEY_MAX + BITS_PER_LONG) / BITS_PER_LONG] = {0};
    if (ioctl(srcFd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
        perror("EVIOCGBIT on source pad");
        fclose(f);
        return;
    }

    for (int code = 0; code <= KEY_MAX; code++) {
        if (bits[code / BITS_PER_LONG] & (1UL << (code % BITS_PER_LONG))) {
            const char *nm = NULL;
            for (int i = 0; i < key_name_map_size; i++) {
                if (key_name_map[i].code == code) {
                    nm = key_name_map[i].name;
                    break;
                }
            }
            if (nm) {
                fprintf(f, "%s %s\n", nm, nm);
            } else {
                fprintf(f, "%d %d\n", code, code);
            }
        }
    }

    fclose(f);
    chmod(path, 0777);
    fprintf(stderr, "[Config] Created default MAPPINGS dump from fd=%d\n", srcFd);
}

// After load_custom_key_mappings()
static void apply_custom_key_capabilities(int fd) {
    for (int sc = 0; sc <= KEY_MAX; sc++) {
        int dst = g_customKeyMap[sc];
        if (dst >= 0) {
            ioctl(fd, UI_SET_KEYBIT, dst);
        }
    }
}

/* Parse one token: either decimal/0x number or a name from key_name_map */
static int parse_key_token(const char *tok) {
    if (isdigit((unsigned char)tok[0])) {
        long v = strtol(tok, NULL, 0);
        return (v >= 0 && v <= KEY_MAX) ? (int)v : -1;
    } else {
        for (int i = 0; i < key_name_map_size; i++) {
            if (strcmp(tok, key_name_map[i].name) == 0)
                return key_name_map[i].code;
        }
        return -1;
    }
}

/* Load MAPPINGS into g_customKeyMap[], logging errors */
static void load_custom_key_mappings(void) {
    init_custom_key_mappings();

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_DIR, MAPPINGS_FILE);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;

        char *src = p;
        char *sp = strpbrk(src, " \t");
        if (!sp) {
            fprintf(stderr,
                    "[Config] %s:%d missing separator\n",
                    MAPPINGS_FILE, lineno);
            continue;
        }
        *sp++ = '\0';
        while (isspace((unsigned char)*sp)) sp++;
        char *dst = sp;
        char *e = strpbrk(dst, "\r\n");
        if (e) *e = '\0';

        int s = parse_key_token(src);
        int d = parse_key_token(dst);
        if (s < 0 || d < 0) {
            fprintf(stderr,
                    "[Config] %s:%d invalid '%s'→'%s'\n",
                    MAPPINGS_FILE, lineno, src, dst);
        } else {
            g_customKeyMap[s] = d;
            fprintf(stderr,
                    "[Config] MAPPINGS: %d(%s) → %d(%s)\n",
                    s, src, d, dst);
        }
    }

    fclose(f);
}

/* Helper: Recursively create directory (like "mkdir -p") */
static int mkdir_recursive(const char *dir, mode_t mode) {
    char tmp[PATH_MAX];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if(len == 0)
        return -1;
    if(tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    
    for(p = tmp + 1; *p; p++) {
        if(*p == '/') {
            *p = '\0';
            if(mkdir(tmp, mode) != 0) {
                if(errno != EEXIST)
                    return -1;
            }
            *p = '/';
        }
    }
    if(mkdir(tmp, mode) != 0) {
        if(errno != EEXIST)
            return -1;
    }
    return 0;
}

/* load_initial_config:
   For each configuration item, if its file exists then read its contents (overriding the default),
   otherwise create the file using the current in‑memory value.
   This function is called early in main so that persistent values override command‑line defaults.
*/
static void load_initial_config(void) {
    char filepath[PATH_MAX];
    FILE *f;
    char buf[256];

    /* Ensure CONFIG_DIR exists */
    if (mkdir_recursive(CONFIG_DIR, 0777) != 0) {
        perror("mkdir_recursive CONFIG_DIR in load_initial_config");
    } else {
        chmod(CONFIG_DIR, 0777);
    }

    /* If MAPPINGS doesn’t exist, create with defaults */
    char mp[PATH_MAX];
    snprintf(mp, sizeof(mp), "%s/%s", CONFIG_DIR, MAPPINGS_FILE);
    if (access(mp, F_OK) != 0) {
        dump_default_mappings();
    }

    /* uiname: string */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "uiname");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            if (g_uiname) {
                free(g_uiname);
            }
            g_uiname = strdup(buf);
            fprintf(stderr, "Loaded persistent uiname: %s\n", g_uiname);
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%s\n", g_uiname);
            fclose(f);
            fprintf(stderr, "Created persistent uiname file with value: %s\n", g_uiname);
        } else {
            perror("fopen uiname for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* uibus: integer */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "uibus");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            int value = atoi(buf);
            if (value > 0) {
                g_uibus = value;
                fprintf(stderr, "Loaded persistent uibus: %d\n", g_uibus);
            }
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_uibus);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created persistent uibus file with value: %d\n", g_uibus);
        } else {
            perror("fopen uibus for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* uivid: integer */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "uivid");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            int value = (int)strtol(buf, NULL, 0);
            if (value > 0) {
                g_uivid = value;
                fprintf(stderr, "Loaded persistent uivid: %d\n", g_uivid);
            }
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_uivid);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created persistent uivid file with value: %d\n", g_uivid);
        } else {
            perror("fopen uivid for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* uiproduct: integer */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "uiproduct");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            int value = (int)strtol(buf, NULL, 0);
            if (value > 0) {
                g_uiproduct = value;
                fprintf(stderr, "Loaded persistent uiproduct: %d\n", g_uiproduct);
            }
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_uiproduct);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created persistent uiproduct file with value: %d\n", g_uiproduct);
        } else {
            perror("fopen uiproduct for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* uiversion: integer */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "uiversion");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            int value = (int)strtol(buf, NULL, 0);
            if (value > 0) {
                g_uiversion = value;
                fprintf(stderr, "Loaded persistent uiversion: %d\n", g_uiversion);
            }
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_uiversion);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created persistent uiversion file with value: %d\n", g_uiversion);
        } else {
            perror("fopen uiversion for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* ffpwm: integer */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "ffpwm");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            int value = atoi(buf);
            g_ffPwmEnabled = value;
            fprintf(stderr, "Loaded persistent ffpwm: %d\n", g_ffPwmEnabled);
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_ffPwmEnabled);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created persistent ffpwm file with value: %d\n", g_ffPwmEnabled);
        } else {
            perror("fopen ffpwm for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* ffpwmmax: integer */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "ffpwmmax");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            int value = atoi(buf);
            if (value > 0) {
                g_ffPwmMaxMagnitude = value;
                fprintf(stderr, "Loaded persistent ffpwmmax: %d\n", g_ffPwmMaxMagnitude);
            }
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_ffPwmMaxMagnitude);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created persistent ffpwmmax file with value: %d\n", g_ffPwmMaxMagnitude);
        } else {
            perror("fopen ffpwmmax for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* ABXY_LAYOUT: integer 0 or 1 */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "ABXY_LAYOUT");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            int value = atoi(buf);
            g_abxy_layout = (value != 0);
            fprintf(stderr, "Loaded persistent ABXY_LAYOUT: %d\n", g_abxy_layout);
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_abxy_layout);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created persistent ABXY_LAYOUT file with value: %d\n", g_abxy_layout);
        } else {
            perror("fopen ABXY_LAYOUT for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* DPAD_ANALOG_SWAP: integer 0 or 1 */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "DPAD_ANALOG_SWAP");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            int value = atoi(buf);
            g_dpad_analog_swap = (value != 0);
            fprintf(stderr, "Loaded persistent DPAD_ANALOG_SWAP: %d\n", g_dpad_analog_swap);
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        /* create default file */
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_dpad_analog_swap);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created DPAD_ANALOG_SWAP file with default %d\n", g_dpad_analog_swap);
        } else {
            perror("fopen DPAD_ANALOG_SWAP for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* LEFTSTICKINVERT: integer 0 or 1 */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "LEFTSTICKINVERT");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            g_left_stick_invert = (atoi(buf) != 0);
            fprintf(stderr, "Loaded persistent LEFTSTICKINVERT: %d\n", g_left_stick_invert);
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_left_stick_invert);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created LEFTSTICKINVERT file with value: %d\n", g_left_stick_invert);
        } else {
            perror("fopen LEFTSTICKINVERT for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* RIGHTSTICKINVERT: integer 0 or 1 */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "RIGHTSTICKINVERT");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            g_right_stick_invert = (atoi(buf) != 0);
            fprintf(stderr, "Loaded persistent RIGHTSTICKINVERT: %d\n", g_right_stick_invert);
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_right_stick_invert);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created RIGHTSTICKINVERT file with value: %d\n", g_right_stick_invert);
        } else {
            perror("fopen RIGHTSTICKINVERT for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* RIGHTSTICKINVERT_Z_RZ: integer 0 or 1 */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "RIGHTSTICKINVERT_Z_RZ");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            g_right_stick_invert_z_rz = (atoi(buf) != 0);
            fprintf(stderr, "Loaded persistent RIGHTSTICKINVERT_Z_RZ: %d\n", g_right_stick_invert_z_rz);
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_right_stick_invert_z_rz);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created RIGHTSTICKINVERT_Z_RZ file with default: %d\n", g_right_stick_invert_z_rz);
        } else {
            perror("fopen RIGHTSTICKINVERT_Z_RZ for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* ANALOGSENSITIVITY: integer -3..3 */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "ANALOGSENSITIVITY");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            int v = atoi(buf);
            if (v < -3) v = -3;
            if (v >  3) v =  3;
            g_analog_sensitivity = v;
            fprintf(stderr, "Loaded persistent ANALOGSENSITIVITY: %d\n", g_analog_sensitivity);
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_analog_sensitivity);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created ANALOGSENSITIVITY file with default %d\n",
                    g_analog_sensitivity);
        } else {
            perror("fopen ANALOGSENSITIVITY for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* DEADZONE: integer 0..100 */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "DEADZONE");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            int v = atoi(buf);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            g_deadzone = v;
            fprintf(stderr, "Loaded persistent DEADZONE: %d%%\n", g_deadzone);
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_deadzone);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created persistent DEADZONE file with value: %d%%\n", g_deadzone);
        } else {
            perror("fopen DEADZONE for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    /* WAKE_DEBOUNCE_MS: integer 0..2000 (Issue #249 fix) */
    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, "WAKE_DEBOUNCE_MS");
    f = fopen(filepath, "r");
    pthread_mutex_lock(&config_mutex);
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            int v = atoi(buf);
            if (v < 0) v = 0;
            if (v > 2000) v = 2000;
            g_wakeDebounceMs = v;
            fprintf(stderr, "Loaded persistent WAKE_DEBOUNCE_MS: %dms\n", g_wakeDebounceMs);
        }
        fclose(f);
        chmod(filepath, 0777);
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_wakeDebounceMs);
            fclose(f);
            chmod(filepath, 0777);
            fprintf(stderr, "Created persistent WAKE_DEBOUNCE_MS file with value: %dms\n",
                    g_wakeDebounceMs);
        } else {
            perror("fopen WAKE_DEBOUNCE_MS for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);

    {
        const char *files[] = {
            MAPPINGS_FILE,
            "uiname", "uibus", "uivid", "uiproduct", "uiversion",
            "ffpwm", "ffpwmmax",
            "ABXY_LAYOUT", "DPAD_ANALOG_SWAP",
            "LEFTSTICKINVERT", "RIGHTSTICKINVERT",
            "RIGHTSTICKINVERT_Z_RZ",
            "ANALOGSENSITIVITY", "DEADZONE",
            "CALIBRATION_MODE",
            "WAKE_DEBOUNCE_MS"
        };
        char fp[PATH_MAX];
        for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
            snprintf(fp, sizeof(fp), "%s/%s", CONFIG_DIR, files[i]);
            if (access(fp, F_OK) == 0) {
                chmod(fp, 0777);
            }
        }
    }

    /* Finally, load our custom button→key mappings */
    load_custom_key_mappings();
}

/* update_parameter():
   Updates the in-memory configuration and, for UI parameters, rebinds the virtual controller
   and reuploads aggregator FF effects.
*/
static void update_parameter(const char *filename, const char *new_value) {
    pthread_mutex_lock(&config_mutex);
    if (strcmp(filename, "ffpwmmax") == 0) {
        int new_max = atoi(new_value);
        if (new_max > 0) {
            g_ffPwmMaxMagnitude = new_max;
            fprintf(stderr, "Updated ffpwmmax to %d\n", g_ffPwmMaxMagnitude);
        }
    }
    else if (strcmp(filename, "ffpwm") == 0) {
        int enable = atoi(new_value);
        g_ffPwmEnabled = enable;
        fprintf(stderr, "Updated ffpwm to %d\n", g_ffPwmEnabled);
    }
    else if (strcmp(filename, "uiname") == 0) {
        if (g_uiname) free(g_uiname);
        g_uiname = strdup(new_value);
        fprintf(stderr, "Updated uiname to %s\n", g_uiname);
        if (controllerFd >= 0) {
            destroy_virtual_device(controllerFd);
        }
        if (create_virtual_controller(&controllerFd) < 0) {
            fprintf(stderr, "Failed to recreate virtual controller with new uiname\n");
        } else {
            fprintf(stderr, "Recreated virtual controller with new uiname: %s\n", g_uiname);
            aggregatorReuploadAllEffects();
        }
    }
    else if (strcmp(filename, "uibus") == 0) {
        int new_bus = atoi(new_value);
        if (new_bus > 0) {
            g_uibus = new_bus;
            fprintf(stderr, "Updated uibus to %d\n", g_uibus);
            if (controllerFd >= 0) {
                destroy_virtual_device(controllerFd);
            }
            if (create_virtual_controller(&controllerFd) < 0) {
                fprintf(stderr, "Failed to recreate virtual controller with new uibus\n");
            } else {
                fprintf(stderr, "Recreated virtual controller with new uibus: %d\n", g_uibus);
                aggregatorReuploadAllEffects();
            }
        }
    }
    else if (strcmp(filename, "uivid") == 0) {
        int new_vid = (int)strtol(new_value, NULL, 0);
        if (new_vid > 0) {
            g_uivid = new_vid;
            fprintf(stderr, "Updated uivid to %d\n", g_uivid);
            if (controllerFd >= 0) {
                destroy_virtual_device(controllerFd);
            }
            if (create_virtual_controller(&controllerFd) < 0) {
                fprintf(stderr, "Failed to recreate virtual controller with new uivid\n");
            } else {
                fprintf(stderr, "Recreated virtual controller with new uivid: %d\n", g_uivid);
                aggregatorReuploadAllEffects();
            }
        }
    }
    else if (strcmp(filename, "uiproduct") == 0) {
        int new_product = (int)strtol(new_value, NULL, 0);
        if (new_product > 0) {
            g_uiproduct = new_product;
            fprintf(stderr, "Updated uiproduct to %d\n", g_uiproduct);
            if (controllerFd >= 0) {
                destroy_virtual_device(controllerFd);
            }
            if (create_virtual_controller(&controllerFd) < 0) {
                fprintf(stderr, "Failed to recreate virtual controller with new uiproduct\n");
            } else {
                fprintf(stderr, "Recreated virtual controller with new uiproduct: %d\n", g_uiproduct);
                aggregatorReuploadAllEffects();
            }
        }
    }
    else if (strcmp(filename, "uiversion") == 0) {
        int new_version = (int)strtol(new_value, NULL, 0);
        if (new_version > 0) {
            g_uiversion = new_version;
            fprintf(stderr, "Updated uiversion to %d\n", g_uiversion);
            if (controllerFd >= 0) {
                destroy_virtual_device(controllerFd);
            }
            if (create_virtual_controller(&controllerFd) < 0) {
                fprintf(stderr, "Failed to recreate virtual controller with new uiversion\n");
            } else {
                fprintf(stderr, "Recreated virtual controller with new uiversion: %d\n", g_uiversion);
                aggregatorReuploadAllEffects();
            }
        }
    }
    else if (strcmp(filename, "ABXY_LAYOUT") == 0) {
        int enable = atoi(new_value) != 0;
        g_abxy_layout = enable;
        fprintf(stderr, "Updated ABXY_LAYOUT to %d\n", g_abxy_layout);
    }
    else if (strcmp(filename, "DPAD_ANALOG_SWAP") == 0) {
        int enable = atoi(new_value) != 0;
        g_dpad_analog_swap = enable;
        fprintf(stderr, "Updated DPAD_ANALOG_SWAP to %d\n", g_dpad_analog_swap);
    }
    else if (strcmp(filename, "LEFTSTICKINVERT") == 0) {
        g_left_stick_invert = (atoi(new_value) != 0);
        fprintf(stderr, "Updated LEFTSTICKINVERT to %d\n", g_left_stick_invert);
    }
    else if (strcmp(filename, "RIGHTSTICKINVERT") == 0) {
        g_right_stick_invert = (atoi(new_value) != 0);
        fprintf(stderr, "Updated RIGHTSTICKINVERT to %d\n", g_right_stick_invert);
    }
    else if (strcmp(filename, "RIGHTSTICKINVERT_Z_RZ") == 0) {
        g_right_stick_invert_z_rz = (atoi(new_value) != 0);
        fprintf(stderr, "Updated RIGHTSTICKINVERT_Z_RZ to %d\n",
                g_right_stick_invert_z_rz);
    }
    else if (strcmp(filename, "ANALOGSENSITIVITY") == 0) {
        int v = atoi(new_value);
        if (v < -3) v = -3;
        if (v >  3) v =  3;
        g_analog_sensitivity = v;
        fprintf(stderr, "Updated ANALOGSENSITIVITY to %d\n", g_analog_sensitivity);
    }
    else if (strcmp(filename, "CALIBRATION_MODE") == 0) {
        int mode = atoi(new_value);
        if (mode == 1) {
            fprintf(stderr, "[Config] Calibration mode triggered\n");
            triggerCalibration();
        }
    }
    else if (strcmp(filename, "DEADZONE") == 0) {
        int v = atoi(new_value);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        g_deadzone = v;
        fprintf(stderr, "Updated DEADZONE to %d%%\n", g_deadzone);
    }
    else if (strcmp(filename, "WAKE_DEBOUNCE_MS") == 0) {
        int v = atoi(new_value);
        if (v < 0) v = 0;
        if (v > 2000) v = 2000;
        g_wakeDebounceMs = v;
        fprintf(stderr, "Updated WAKE_DEBOUNCE_MS to %dms\n", g_wakeDebounceMs);
    }
    else if (strcmp(filename, MAPPINGS_FILE) == 0) {
        fprintf(stderr, "[Config] MAPPINGS modified; reloading.\n");

        // 1) reload the in-memory map
        load_custom_key_mappings();

        // 2) figure out if any mapped dst code is *not* on the real pad
        int srcFd = (g_physCount > 0 ? g_physFds[0] : controllerFd);
        unsigned long bits[(KEY_MAX + BITS_PER_LONG) / BITS_PER_LONG] = {0};
        if (ioctl(srcFd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
            perror("EVIOCGBIT on source pad");
        }

        bool needRecreate = false;
        for (int sc = 0; sc <= KEY_MAX; sc++) {
            int dst = g_customKeyMap[sc];
            if (dst >= 0) {
                // if src pad *doesn't* support dst bit, we must recreate
                if (!(bits[dst / BITS_PER_LONG] & (1UL << (dst % BITS_PER_LONG)))) {
                    needRecreate = true;
                    break;
                }
            }
        }

        if (!needRecreate) {
            fprintf(stderr, "[Config] No new codes to advertise, skipping controller recreate.\n");
        } else {
            int oldFd = controllerFd;

            // 3) destroy & build a fresh uinput device
            destroy_virtual_device(oldFd);
            if (create_virtual_controller(&controllerFd) < 0) {
                fprintf(stderr, "[Config] Failed to recreate virtual controller after remap\n");
            } else {
                fprintf(stderr, "[Config] Recreated virtual controller with updated mappings\n");
                aggregatorReuploadAllEffects();

                // 4) rewire the epoll watcher so we keep servicing the new FD (so FF still works!)
                struct epoll_event ev;
                memset(&ev, 0, sizeof(ev));
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = controllerFd;
                epoll_ctl(g_epfd, EPOLL_CTL_DEL, oldFd, NULL);
                epoll_ctl(g_epfd, EPOLL_CTL_ADD, controllerFd, &ev);
                fcntl(controllerFd, F_SETFL, O_NONBLOCK);
            }
        }
    }

    pthread_mutex_unlock(&config_mutex);
}

/* config_watcher_thread(): Uses inotify to watch CONFIG_DIR */
void *config_watcher_thread(void *arg) {
    (void)arg;
    fprintf(stderr, "Config watcher thread starting. Ensuring directory %s exists...\n", CONFIG_DIR);
    if (mkdir_recursive(CONFIG_DIR, 0777) != 0) {
        perror("mkdir_recursive CONFIG_DIR");
    } else {
        chmod(CONFIG_DIR, 0777);
        fprintf(stderr, "Directory %s created/existed with full permissions.\n", CONFIG_DIR);
    }
    
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        perror("inotify_init");
        return NULL;
    }
    
    int wd = inotify_add_watch(inotify_fd, CONFIG_DIR, IN_CREATE | IN_MODIFY);
    if (wd < 0) {
        perror("inotify_add_watch");
        close(inotify_fd);
        return NULL;
    }
    
    char buffer[EVENT_BUF_LEN];
    while (1) {
        int length = read(inotify_fd, buffer, EVENT_BUF_LEN);
        if (length < 0) break;
        int i = 0;
        while (i < length) {
            struct inotify_event *ev = (struct inotify_event *)&buffer[i];
            if (ev->len && (ev->mask & (IN_CREATE|IN_MODIFY))) {
                if (strcmp(ev->name, MAPPINGS_FILE) == 0) {
                    /* reload all mappings */
                    update_parameter(ev->name, NULL);
                } else {
                    /* existing single-value configs */
                    char filepath[PATH_MAX];
                    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, ev->name);
                    FILE *f = fopen(filepath, "r");
                    if (f) {
                        char newv[256];
                        if (fgets(newv, sizeof(newv), f)) {
                            char *nl = strchr(newv,'\n');
                            if (nl) *nl = '\0';
                            update_parameter(ev->name, newv);
                        }
                        fclose(f);
                    }
                }
            }
            i += sizeof(struct inotify_event) + ev->len;
        }
    }
    inotify_rm_watch(inotify_fd, wd);
    close(inotify_fd);
    return NULL;
}

/* start_config_watcher() loads initial configuration and starts the watcher thread */
void start_config_watcher(void) {
    load_initial_config();
    pthread_t watcher_thread;
    if (pthread_create(&watcher_thread, NULL, config_watcher_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create config watcher thread\n");
    } else {
        pthread_detach(watcher_thread);
    }
}
