#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/wait.h>

void error(const char* msg) 
{
    if (errno == EPIPE) 
    {
        return; // ignore
    }

    fprintf(stderr, "reaper: %s: %s\n", msg, strerror(errno));
    exit(1);
    /* NOTREACHED */
}

int main(int argc, char** argv) 
{
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) == -1) 
    {
        error("prctl");
    }

    struct sigaction sa = {.sa_handler = SIG_IGN};
    sigaction(SIGPIPE, &sa, 0);

    pid_t pid;
    while ((pid = wait(NULL)) != -1) 
    {
        // Shove the pid directly down the pipe binary style. Bit weird to use
        // binary data with stdout but eh shrug. The tracer doesn't need to be
        // told the ending status since they'd already know that.
        errno = 0;
        if (fwrite(&pid, sizeof(pid), 1, stdout) != 1) 
        {
            error("writing pid");
        }
        if (fflush(stdout) != 0) 
        {
            error("flushing");
        }
    }

    if (errno != ECHILD) 
    {
        error("wait");
    }

    return 0;
}
