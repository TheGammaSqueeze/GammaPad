// gammapad_config.h
#ifndef GAMMAPAD_CONFIG_H
#define GAMMAPAD_CONFIG_H

#include <linux/input.h>  // for KEY_MAX

/*
 * start_config_watcher() loads the persistent configuration from /data/GammaPad
 * (creating the files if necessary) and starts a thread that watches for changes.
 * Note: load_initial_config() is called early (before device creation) so that
 * persistent values override command-line defaults.
 */
void start_config_watcher(void);

/*
 * Custom button→key mapping. Index is source keycode; value is
 * destination keycode, or -1 for “no mapping”.
 */
extern int g_customKeyMap[KEY_MAX + 1];

#endif /* GAMMAPAD_CONFIG_H */
