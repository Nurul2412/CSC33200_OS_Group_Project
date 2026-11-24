#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    size_t chunk = 10 * 1024 * 1024;
    for (;;) {
        char *p = malloc(chunk);
        if (!p)
            return 3;
        memset(p, 1, chunk);
        usleep(20000);
    }
}
