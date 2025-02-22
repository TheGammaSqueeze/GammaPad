#ifndef GAMMAPAD_CONFIG_H
#define GAMMAPAD_CONFIG_H

/* 
 * start_config_watcher() loads the persistent configuration from /data/GammaPad
 * (creating the files if necessary) and starts a thread that watches for changes.
 * Note: load_initial_config() is called early (before device creation) so that
 * persistent values override command-line defaults.
 */
void start_config_watcher(void);

#endif
