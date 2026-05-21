#include "wt_system.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

int wtSystemReliable(const char *cmd) {
    struct sigaction prev;
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGCHLD, &dfl, &prev);
    int rc = system(cmd);
    sigaction(SIGCHLD, &prev, NULL);
    return rc;
}
