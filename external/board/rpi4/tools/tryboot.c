/* tryboot - reboot the Pi 4 once with tryboot.txt instead of config.txt.
 * If it does not start, a power cycle restores the normal config. */
#include <linux/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
    sync();
    return syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                   LINUX_REBOOT_CMD_RESTART2, "0 tryboot");
}
