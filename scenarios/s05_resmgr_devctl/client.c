// SPDX-License-Identifier: Apache-2.0
//
// The client half of the Tier 2 probe: open the registered pathname and issue
// a devctl. The vendor server sample it was written against is not bundled.

#include <fcntl.h>
#include <stdio.h>
#include <sys/iofunc.h>

#define DCMD_HELLO _DIOC_FIRST_USER

int main(void)
{
    int fd = open("/dev/hello", O_RDWR);
    if (fd < 0) {
        perror("open /dev/hello");
        return 1;
    }
    char buffer[64] = { 0 };
    if (devctl(fd, DCMD_HELLO, buffer, sizeof buffer, NULL) != EOK) {
        perror("devctl");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}
