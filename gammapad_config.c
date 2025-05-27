#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "gammapad.h"

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

/* NEW: Declare aggregatorReuploadAllEffects() from gammapad_ff.c */
extern void aggregatorReuploadAllEffects(void);

/* Mutex to synchronize updates to global configuration values */
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CONFIG_DIR "/data/GammaPad"
#define EVENT_BUF_LEN (1024 * (sizeof(struct inotify_event) + NAME_MAX + 1))

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
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_uibus);
            fclose(f);
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
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_uivid);
            fclose(f);
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
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_uiproduct);
            fclose(f);
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
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_uiversion);
            fclose(f);
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
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_ffPwmEnabled);
            fclose(f);
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
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_ffPwmMaxMagnitude);
            fclose(f);
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
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_abxy_layout);
            fclose(f);
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
    } else {
        /* create default file */
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_dpad_analog_swap);
            fclose(f);
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
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_left_stick_invert);
            fclose(f);
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
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_right_stick_invert);
            fclose(f);
            fprintf(stderr, "Created RIGHTSTICKINVERT file with value: %d\n", g_right_stick_invert);
        } else {
            perror("fopen RIGHTSTICKINVERT for writing");
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
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_analog_sensitivity);
            fclose(f);
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
    } else {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "%d\n", g_deadzone);
            fclose(f);
            fprintf(stderr, "Created persistent DEADZONE file with value: %d%%\n", g_deadzone);
        } else {
            perror("fopen DEADZONE for writing");
        }
    }
    pthread_mutex_unlock(&config_mutex);
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
        if (length < 0) {
            perror("inotify read");
            break;
        }
        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];
            if (event->len) {
                if (event->mask & (IN_CREATE | IN_MODIFY)) {
                    char filepath[PATH_MAX];
                    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, event->name);
                    FILE *f = fopen(filepath, "r");
                    if (f) {
                        char new_value[256] = {0};
                        if (fgets(new_value, sizeof(new_value), f)) {
                            char *nl = strchr(new_value, '\n');
                            if (nl) *nl = '\0';
                            fprintf(stderr, "File %s changed; new value: %s\n", event->name, new_value);
                            update_parameter(event->name, new_value);
                        }
                        fclose(f);
                    }
                }
            }
            i += sizeof(struct inotify_event) + event->len;
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
