#include "retrogamepad2xbox.h"

void bus_error_handler(int sig) {
    // Log the bus error
    fprintf(stderr, "Caught bus error (signal %d). Ignoring it.\n", sig);
}
