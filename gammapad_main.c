/*****************************************************
 * gammapad_main.c
 *
 * Main entry point:
 *  - If user specifies devices (e.g. multiple controllers plus optional --ffdev=...),
 *    we open them in aggregator mode.
 *  - Create the virtual pad + mouse.
 *  - Immediately after creating the virtual pad, remove the /dev/input/event* node
 *    of the primary physical device so only the virtual controller remains visible.
 *  - On exit, we perform cleanup.
 *****************************************************/

 #include "gammapad.h"
 #include "gammapad_inputdefs.h"
 #include "gammapad_capture.h"
 #include "gammapad_config.h"  // NEW: Include config watcher header
 #include <sys/epoll.h>
 #include <linux/input.h>
 #include <fcntl.h>
 #include <signal.h>
 #include <errno.h>
 #include <dirent.h>
 #include <sys/stat.h>
 #include <stdbool.h>
 #include <stdio.h>
 #include <string.h>
 #include <unistd.h>
 #include <pthread.h>
 
 #define MAX_ACTIVE_EVENTS 64
 #define EPOLL_MAX_EVENTS  16
 
 enum EventType {
     EVENT_TYPE_KEY,
     EVENT_TYPE_ABS
 };
 
 struct ActiveEvent {
     enum EventType type;
     int code;
     int value;
     unsigned long long startMs;
     unsigned long long durationMs;
 };
 
 static struct ActiveEvent activeEvents[MAX_ACTIVE_EVENTS];
 
 /*
  * Global FDs:
  *  - controllerFd: virtual gamepad FD
  *  - mouseFd: virtual mouse FD
  *  - g_physicalFd: legacy physical device FD
  *  - g_physFds: aggregator physical device FDs
  *  - g_ffPhysicalFd: physical force feedback device FD
  */
 int controllerFd = -1;
 int mouseFd = -1;
 int g_physicalFd = -1;
 
 #define MAX_PHYSICAL_DEVS 16
 int g_physFds[MAX_PHYSICAL_DEVS];
 int g_physCount = 0;
 
 int g_ffPhysicalFd = -1;
 int g_hasPhysicalFF = 0;
 
 static int g_shouldExit = 0;
 static void sigintHandler(int sig)
 {
     (void)sig;
     g_shouldExit = 1;
 }
 
 /* New virtual controller parameters – defaults */
 char* g_uiname = NULL;  // Will be set to default via strdup if not provided
 int g_uibus = BUS_BLUETOOTH;
 int g_uivid = 0x045e;
 int g_uiproduct = 0x02fd;
 int g_uiversion = 0x0003;
 
 /* Aggregator device strings */
 static char* g_allAggregatorDevices[MAX_PHYSICAL_DEVS];
 static int g_allAggCount = 0;
 
 /* Optional FF device argument (--ffdev=...) */
 static char* g_ffArg = NULL;
 
 /* New parameters for FF and UI */
 int g_ffDivisor = 1;
 float g_ffMagnitudeMultiplier = 1.0f;
 int g_ffPwmEnabled = 0;
 int g_ffPwmMaxMagnitude = 32767;
 
 /*
  * function prototypes...
  */
 int create_virtual_controller(int* fd_out);
 int create_virtual_mouse(int* fd_out);
 void destroy_virtual_device(int fd);
 
 int dummy_upload_ff_effect(struct ff_effect* effect);
 int dummy_erase_ff_effect(int kernel_id);
 void storeUploadedEffect(struct ff_effect* eff);
 void ff_play_effect(int kernel_id, int doPlay);
 
 void parseCommand(const char* line);
 
 static int g_epfd = -1;
 static void cleanupOnExit(void)
 {
     fprintf(stderr,"[GammaPad] cleanupOnExit => calling unbindAndRebind.\n");
     unbindAndRebind();
 }
 
 static void sendEvent(int code, enum EventType t, int value, unsigned long long dur)
 {
     for(int i = 0; i < MAX_ACTIVE_EVENTS; i++){
         if(activeEvents[i].code == 0 && activeEvents[i].value == 0){
             activeEvents[i].type = t;
             activeEvents[i].code = code;
             activeEvents[i].value = value;
             activeEvents[i].startMs = getTimeMs();
             activeEvents[i].durationMs = dur;
             break;
         }
     }
     if(controllerFd < 0) return;
     
     struct input_event ev[2];
     memset(ev, 0, sizeof(ev));
     
     if(t == EVENT_TYPE_KEY){
         ev[0].type = EV_KEY;
         ev[0].code = code;
         ev[0].value = value;
     } else {
         ev[0].type = EV_ABS;
         ev[0].code = code;
         ev[0].value = value;
     }
     ev[1].type = EV_SYN;
     ev[1].code = SYN_REPORT;
     ev[1].value = 0;
     write(controllerFd, &ev, sizeof(ev));
 }
 
 void scheduleEvent(int code, int isKey, int value, unsigned long long durationMs)
 {
     sendEvent(code, (isKey ? EVENT_TYPE_KEY : EVENT_TYPE_ABS), value, durationMs);
 }
 
 /*
  * resetEvent(): auto-release for short-press events.
  */
 static void resetEvent(int code, enum EventType t)
 {
     if(controllerFd < 0) return;
     
     struct input_event ev[2];
     memset(ev, 0, sizeof(ev));
     
     if(t == EVENT_TYPE_KEY){
         ev[0].type = EV_KEY;
         ev[0].code = code;
         ev[0].value = 0;
     } else {
         ev[0].type = EV_ABS;
         ev[0].code = code;
         ev[0].value = 0;
     }
     ev[1].type = EV_SYN;
     ev[1].code = SYN_REPORT;
     ev[1].value = 0;
     write(controllerFd, &ev, sizeof(ev));
 }
 
 /*
  * checkEventTimeouts(): auto-release events that have expired.
  */
 static void checkEventTimeouts(void)
 {
     unsigned long long now = getTimeMs();
     for(int i = 0; i < MAX_ACTIVE_EVENTS; i++){
         if(activeEvents[i].code != 0 || activeEvents[i].value != 0){
             unsigned long long elapsed = now - activeEvents[i].startMs;
             if(elapsed >= activeEvents[i].durationMs){
                 resetEvent(activeEvents[i].code, activeEvents[i].type);
                 activeEvents[i].type = EVENT_TYPE_KEY;
                 activeEvents[i].code = 0;
                 activeEvents[i].value = 0;
                 activeEvents[i].startMs = 0;
                 activeEvents[i].durationMs = 0;
             }
         }
     }
 }
 
 /*
  * handleFFRequest(): handles FF upload/erase events.
  */
 static void handleFFRequest(const struct input_event* ev)
 {
     if(!ev) return;
     if(ev->code == UI_FF_UPLOAD){
         struct uinput_ff_upload ffup;
         memset(&ffup, 0, sizeof(ffup));
         ffup.request_id = ev->value;
         if(!ioctl(controllerFd, UI_BEGIN_FF_UPLOAD, &ffup)){
             dummy_upload_ff_effect(&ffup.effect);
             ffup.retval = 0;
             if(!ioctl(controllerFd, UI_END_FF_UPLOAD, &ffup)){
                 storeUploadedEffect(&ffup.effect);
             }
         }
     } else if(ev->code == UI_FF_ERASE){
         struct uinput_ff_erase fferase;
         memset(&fferase, 0, sizeof(fferase));
         fferase.request_id = ev->value;
         if(!ioctl(controllerFd, UI_BEGIN_FF_ERASE, &fferase)){
             dummy_erase_ff_effect(fferase.effect_id);
             fferase.retval = 0;
             ioctl(controllerFd, UI_END_FF_ERASE, &fferase);
         }
     }
 }
 
 /*
  * handleFFPlayStop(): handles start/stop of FF effects.
  */
 static void handleFFPlayStop(int kid, int doPlay)
 {
     ff_play_effect(kid, doPlay);
 }
 
 /*
  * processControllerFdEvent(): reads from the virtual controller.
  */
 static void processControllerFdEvent(void)
 {
     struct input_event ie;
     while(1){
         ssize_t n = read(controllerFd, &ie, sizeof(ie));
         if(n < 0){
             if(errno == EAGAIN || errno == EWOULDBLOCK) break;
             break;
         }
         if(n == 0) break;
         if((size_t)n < sizeof(ie)) break;
         
         if(ie.type == EV_UINPUT){
             handleFFRequest(&ie);
         } else if(ie.type == EV_FF){
             handleFFPlayStop(ie.code, ie.value);
         }
     }
 }
 
 /*
  * processPhysicalDeviceEvent(): forwards aggregator device events.
  */
 static void processPhysicalDeviceEvent(int physical_fd)
 {
     struct input_event ev;
     while(1){
         ssize_t n = read(physical_fd, &ev, sizeof(ev));
         if(n < 0){
             if(errno == EAGAIN || errno == EWOULDBLOCK) break;
             break;
         }
         if(n == 0) break;
         if((size_t)n < sizeof(ev)) break;
         
         forward_physical_event(&ev);
     }
 }
 
 /*
  * processPhysicalFFDeviceEvent(): forwards events from a real FF device.
  */
 static void processPhysicalFFDeviceEvent(int fd)
 {
     struct input_event ev;
     while(1){
         ssize_t n = read(fd, &ev, sizeof(ev));
         if(n < 0){
             if(errno == EAGAIN || errno == EWOULDBLOCK) break;
             break;
         }
         if(n == 0) break;
         if((size_t)n < sizeof(ev)) break;
         
         if(ev.type == EV_FF){
             if(controllerFd >= 0){
                 write(controllerFd, &ev, sizeof(ev));
             }
         }
     }
 }
 
 /*
  * add_epoll_fd(): helper function.
  */
 static void add_epoll_fd(int epfd, int fd)
 {
     if(fd < 0) return;
     struct epoll_event ev;
     memset(&ev, 0, sizeof(ev));
     ev.events = EPOLLIN | EPOLLET;
     ev.data.fd = fd;
     if(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0){
         fprintf(stderr, "epoll_ctl ADD fd=%d => %s\n", fd, strerror(errno));
     }
     fcntl(fd, F_SETFL, O_NONBLOCK);
 }
 
 /* remove_epoll_fd(): helper function. */
 static void remove_epoll_fd(int epfd, int fd)
 {
     if(fd < 0) return;
     epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
     close(fd);
 }
 
 /*
  * maybeResolveDevicePath(): returns a device path.
  */
 static char* maybeResolveDevicePath(const char* arg)
 {
     if(!arg) return NULL;
     if(strstr(arg, "/dev/") != NULL){
         return strdup(arg);
     }
     const int MAX_EVENT_SEARCH = 64;
     for(int i = 0; i < MAX_EVENT_SEARCH; i++){
         char devPath[128];
         snprintf(devPath, sizeof(devPath), "/dev/input/event%d", i);
         int fd = open(devPath, O_RDONLY);
         if(fd < 0) continue;
         char devName[256];
         memset(devName, 0, sizeof(devName));
         if(ioctl(fd, EVIOCGNAME(sizeof(devName)), devName) >= 0){
             if(!strcmp(devName, arg)){
                 close(fd);
                 return strdup(devPath);
             }
         }
         close(fd);
     }
     return strdup(arg);
 }
 
 /* forward declaration for open_physical_device() */
 extern int open_physical_device(const char* path);
 
 /* open_physical_ff_device() for FF devices */
 static int open_physical_ff_device(const char* path);
 
 /* doPollForDevicesThread(): polls for new devices. */
 static void* doPollForDevicesThread(void* arg)
 {
     (void)arg;
     #define MAX_KNOWN_NODES 128
     char knownNodes[MAX_KNOWN_NODES][128];
     int knownCount = 0;
     {
         DIR* d = opendir("/dev/input");
         if(d){
             struct dirent* de;
             while((de = readdir(d))){
                 if(!strncmp(de->d_name, "event", 5)){
                     snprintf(knownNodes[knownCount], sizeof(knownNodes[knownCount]), "%s", de->d_name);
                     knownCount++;
                     if(knownCount >= MAX_KNOWN_NODES) break;
                 }
             }
             closedir(d);
         }
     }
     while(!g_shouldExit){
         sleep(1);
         char currentNodes[MAX_KNOWN_NODES][128];
         int currCount = 0;
         {
             DIR* d = opendir("/dev/input");
             if(d){
                 struct dirent* de;
                 while((de = readdir(d))){
                     if(!strncmp(de->d_name, "event", 5)){
                         snprintf(currentNodes[currCount], sizeof(currentNodes[currCount]), "%s", de->d_name);
                         currCount++;
                         if(currCount >= MAX_KNOWN_NODES) break;
                     }
                 }
                 closedir(d);
             }
         }
         for(int i = 0; i < currCount; i++){
             int found = 0;
             for(int k = 0; k < knownCount; k++){
                 if(!strcmp(currentNodes[i], knownNodes[k])){
                     found = 1;
                     break;
                 }
             }
             if(!found){
                 char fullPath[256];
                 snprintf(fullPath, sizeof(fullPath), "/dev/input/%s", currentNodes[i]);
                 if(g_ffPhysicalFd < 0 && g_ffArg){
                     int testFd = open_physical_ff_device(fullPath);
                     if(testFd >= 0){
                         if(g_ffPhysicalFd >= 0){
                             remove_epoll_fd(g_epfd, g_ffPhysicalFd);
                             g_ffPhysicalFd = -1;
                             g_hasPhysicalFF = 0;
                         }
                         g_ffPhysicalFd = testFd;
                         g_hasPhysicalFF = 1;
                         fprintf(stderr, "[GammaPad] Poll => recaptured ffdev => %s => fd=%d\n", fullPath, testFd);
                         add_epoll_fd(g_epfd, g_ffPhysicalFd);
                         continue;
                     }
                 }
                 for(int dIndex = 0; dIndex < g_allAggCount; dIndex++){
                     int testAggFd = open_physical_device(fullPath);
                     if(testAggFd >= 0){
                         char devName[256];
                         memset(devName, 0, sizeof(devName));
                         if(ioctl(testAggFd, EVIOCGNAME(sizeof(devName)), devName) < 0){
                             devName[0] = '\0';
                         }
                         if(strcmp(devName, g_allAggregatorDevices[dIndex]) != 0){
                             close(testAggFd);
                             testAggFd = -1;
                             continue;
                         }
                         for(int p = 0; p < g_physCount; p++){
                             if(g_physFds[p] >= 0){
                                 remove_epoll_fd(g_epfd, g_physFds[p]);
                                 g_physFds[p] = -1;
                             }
                         }
                         g_physCount = 0;
                         g_physFds[0] = testAggFd;
                         g_physCount = 1;
                         add_epoll_fd(g_epfd, testAggFd);
                         if(dIndex == 0){
                             fprintf(stderr, "[GammaPad] Poll => recaptured aggregator => primary => removing node.\n");
                             char rmCmd[256];
                             snprintf(rmCmd, sizeof(rmCmd), "rm -f '%s'", fullPath);
                             system(rmCmd);
                         } else {
                             fprintf(stderr, "[GammaPad] Poll => recaptured aggregator => secondary => no node removal.\n");
                         }
                         fprintf(stderr, "[GammaPad] Poll => recaptured aggregator => %s => fd=%d => dIndex=%d\n", fullPath, testAggFd, dIndex);
                         break;
                     }
                 }
             }
         }
         for(int k = 0; k < knownCount; k++){
             int found = 0;
             for(int i = 0; i < currCount; i++){
                 if(!strcmp(knownNodes[k], currentNodes[i])){
                     found = 1;
                     break;
                 }
             }
             if(!found){
                 fprintf(stderr, "[GammaPad] Poll => node removed => %s => ignoring.\n", knownNodes[k]);
             }
         }
         knownCount = currCount;
         for(int i = 0; i < knownCount; i++){
             strcpy(knownNodes[i], currentNodes[i]);
         }
     }
     return NULL;
 }
 
 /*
  * main(): entry point.
  */
 int main(int argc, char** argv)
 {
     signal(SIGINT, sigintHandler);
 
     /* Set default values from command line.
        For uiname, if not provided, set to default using strdup.
     */
     if (!g_uiname) {
         g_uiname = strdup("Xbox Wireless Controller");
     }
 
     for (int i = 1; i < argc; i++) {
         if (!strncmp(argv[i], "--ffdev=", 8)) {
             g_ffArg = argv[i] + 8;
         } else if (!strncmp(argv[i], "--ffdiv=", 8)) {
             g_ffDivisor = atoi(argv[i] + 8);
             if (g_ffDivisor <= 0) g_ffDivisor = 1;
         } else if (!strncmp(argv[i], "--ffmag=", 8)) {
             g_ffMagnitudeMultiplier = atof(argv[i] + 8);
             if (g_ffMagnitudeMultiplier == 0.0f) g_ffMagnitudeMultiplier = 1.0f;
         } else if (!strncmp(argv[i], "--ffpwmmax=", 11)) {
             g_ffPwmMaxMagnitude = atoi(argv[i] + 11);
             if (g_ffPwmMaxMagnitude <= 0) g_ffPwmMaxMagnitude = 32767;
         } else if (!strcmp(argv[i], "--ffpwm")) {
             g_ffPwmEnabled = 1;
         } else if (!strncmp(argv[i], "--uibus=", 8)) {
             g_uibus = atoi(argv[i] + 8);
         } else if (!strncmp(argv[i], "--uivid=", 8)) {
             g_uivid = (int)strtol(argv[i] + 8, NULL, 0);
         } else if (!strncmp(argv[i], "--uiproduct=", 12)) {
             g_uiproduct = (int)strtol(argv[i] + 12, NULL, 0);
         } else if (!strncmp(argv[i], "--uiversion=", 12)) {
             g_uiversion = (int)strtol(argv[i] + 12, NULL, 0);
         } else if (!strncmp(argv[i], "--uiname=", 9)) {
             if (g_uiname) free(g_uiname);
             /* FIX: Always strdup the value so that g_uiname is heap allocated */
             g_uiname = strdup(argv[i] + 9);
         } else {
             if (g_allAggCount < MAX_PHYSICAL_DEVS) {
                 g_allAggregatorDevices[g_allAggCount] = argv[i];
                 g_allAggCount++;
             }
         }
     }
 
     /* NEW: Load persistent configuration and start the config watcher.
        This will override defaults if files exist.
     */
     start_config_watcher();
 
     if (g_ffArg) {
         char* resolvedFF = maybeResolveDevicePath(g_ffArg);
         g_ffPhysicalFd = open_physical_ff_device(resolvedFF);
         free(resolvedFF);
         if (g_ffPhysicalFd >= 0) {
             g_hasPhysicalFF = 1;
         }
     }
 
     for (int i = 0; i < g_allAggCount; i++) {
         if (g_physCount >= MAX_PHYSICAL_DEVS) break;
         char* resolved = maybeResolveDevicePath(g_allAggregatorDevices[i]);
         int fd = open_physical_device(resolved);
         free(resolved);
         if (fd < 0) {
             fprintf(stderr, "[GammaPad] Could not open aggregator '%s'.\n", g_allAggregatorDevices[i]);
         } else {
             g_physFds[g_physCount] = fd;
             g_physCount++;
         }
     }
 
     if (create_virtual_controller(&controllerFd) < 0) {
         fprintf(stderr, "[GammaPad] create_virtual_controller => fail.\n");
         for (int i = 0; i < g_physCount; i++) {
             if (g_physFds[i] >= 0) {
                 close(g_physFds[i]);
                 g_physFds[i] = -1;
             }
         }
         if (g_ffPhysicalFd >= 0) {
             close(g_ffPhysicalFd);
             g_ffPhysicalFd = -1;
             g_hasPhysicalFF = 0;
         }
         return 1;
     }
     if (create_virtual_mouse(&mouseFd) < 0) {
         fprintf(stderr, "[GammaPad] create_virtual_mouse => fail.\n");
         destroy_virtual_device(controllerFd);
         for (int i = 0; i < g_physCount; i++) {
             if (g_physFds[i] >= 0) {
                 close(g_physFds[i]);
                 g_physFds[i] = -1;
             }
         }
         if (g_ffPhysicalFd >= 0) {
             close(g_ffPhysicalFd);
             g_ffPhysicalFd = -1;
             g_hasPhysicalFF = 0;
         }
         return 1;
     }
 
     fprintf(stderr, "GammaPad Virtual Controller (fd=%d)\n", controllerFd);
     fprintf(stderr, "GammaPad Virtual Mouse       (fd=%d)\n", mouseFd);
 
     removePrimaryPhysicalNode();
 
     g_epfd = epoll_create1(0);
     if (g_epfd < 0) {
         perror("epoll_create1");
         for (int i = 0; i < g_physCount; i++) {
             if (g_physFds[i] >= 0) {
                 close(g_physFds[i]);
                 g_physFds[i] = -1;
             }
         }
         destroy_virtual_device(mouseFd);
         destroy_virtual_device(controllerFd);
         if (g_ffPhysicalFd >= 0) {
             close(g_ffPhysicalFd);
             g_ffPhysicalFd = -1;
             g_hasPhysicalFF = 0;
         }
         return 1;
     }
 
     add_epoll_fd(g_epfd, controllerFd);
     for (int i = 0; i < g_physCount; i++) {
         add_epoll_fd(g_epfd, g_physFds[i]);
     }
     add_epoll_fd(g_epfd, STDIN_FILENO);
     if (g_ffPhysicalFd >= 0) {
         add_epoll_fd(g_epfd, g_ffPhysicalFd);
     }
 
     pthread_t pollThread;
     if (pthread_create(&pollThread, NULL, doPollForDevicesThread, NULL) != 0) {
         fprintf(stderr, "[GammaPad] Could not create poll thread => no re-capture logic.\n");
     }
 
     fprintf(stderr,
         "=== GAMMAPAD COMMANDS ===\n"
         " press <button> [ms]\n"
         " push <axis> <value> [ms]\n"
         " exit\n\n"
         "Buttons:\n"
         "   up, down, left, right,\n"
         "   a, b, c, x, y, z,\n"
         "   l1, l2, l3, r1, r2, r3,\n"
         "   select, start, back, mode, gamepad,\n"
         "   volumedown, volumeup, power, 1, 2\n\n"
         "Axes:\n"
         "   abs_x, abs_y, abs_z, abs_rz,\n"
         "   abs_gas, abs_brake, abs_hat0x, abs_hat0y\n"
         "==========================\n"
     );
 
     struct epoll_event events[EPOLL_MAX_EVENTS];
     while(!g_shouldExit){
         checkEventTimeouts();
         int n = epoll_wait(g_epfd, events, EPOLL_MAX_EVENTS, 500);
         if(n < 0){
             if(errno == EINTR) continue;
             perror("epoll_wait");
             break;
         }
         for(int i = 0; i < n; i++){
             int fd = events[i].data.fd;
             if(fd == controllerFd){
                 if(events[i].events & EPOLLIN){
                     processControllerFdEvent();
                 }
             }
             else if(fd == STDIN_FILENO){
                 if(events[i].events & EPOLLIN){
                     char line[256];
                     memset(line, 0, sizeof(line));
                     if(!fgets(line, sizeof(line), stdin)) continue;
                     char* nl = strchr(line, '\n');
                     if(nl) *nl = 0;
                     if(!strcasecmp(line, "exit")){
                         g_shouldExit = 1;
                         continue;
                     }
                     parseCommand(line);
                 }
             }
             else if(fd == g_ffPhysicalFd && g_ffPhysicalFd >= 0){
                 if(events[i].events & EPOLLIN){
                     processPhysicalFFDeviceEvent(fd);
                 }
             }
             else {
                 if(events[i].events & EPOLLIN){
                     processPhysicalDeviceEvent(fd);
                 }
             }
         }
     }
 
     close(g_epfd);
     g_shouldExit = 1;
     pthread_join(pollThread, NULL);
     for(int i = 0; i < g_physCount; i++){
         if(g_physFds[i] >= 0){
             ioctl(g_physFds[i], EVIOCGRAB, 0);
             close(g_physFds[i]);
             g_physFds[i] = -1;
         }
     }
     g_physCount = 0;
     if(g_ffPhysicalFd >= 0){
         close(g_ffPhysicalFd);
         g_ffPhysicalFd = -1;
         g_hasPhysicalFF = 0;
     }
     destroy_virtual_device(mouseFd);
     destroy_virtual_device(controllerFd);
     fprintf(stderr, "[GammaPad] Exiting.\n");
     return 0;
 }
 
 /* open_physical_ff_device() remains unchanged */
 static int open_physical_ff_device(const char* path)
 {
     if(!path) return -1;
     int fd = open(path, O_RDWR | O_NONBLOCK);
     if(fd < 0){
         fprintf(stderr, "[GammaPad] Could not open FF device '%s': %s\n", path, strerror(errno));
         return -1;
     }
     unsigned long evbit[8];
     memset(evbit, 0, sizeof(evbit));
     if(ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0){
         fprintf(stderr, "[GammaPad] EVIOCGBIT(0) fail for FF dev '%s': %s\n", path, strerror(errno));
         close(fd);
         return -1;
     }
     if(!(evbit[EV_FF/(8*sizeof(long))] & (1UL<<(EV_FF%(8*sizeof(long)))))){
         fprintf(stderr, "[GammaPad] Device '%s' does not support EV_FF.\n", path);
         close(fd);
         return -1;
     }
     fprintf(stderr, "[GammaPad] Found real FF device '%s'. We'll forward FF to it.\n", path);
     if(ioctl(fd, EVIOCGRAB, 1) < 0){
         fprintf(stderr, "[GammaPad] EVIOCGRAB on FF dev '%s' => %s\n", path, strerror(errno));
     }
     return fd;
 }
 