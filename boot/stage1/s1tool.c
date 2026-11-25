/*
 * Stage-1 的小工具（靜態鏈接，一個二進制多個子命令）：
 *   s1tool kexec KERNEL INITRD "CMDLINE"   kexec_file_load + reboot(KEXEC)
 *   s1tool eject /dev/sdX                  發 SCSI START STOP UNIT（LoEj=1, Start=0）＝彈出。
 *                                          設備收到後上報 "msc eject" 事件，主機據此知道目標機沒網
 * 編譯：gcc -static -Os -s -o s1tool s1tool.c（boot/build.sh 會做）
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/syscall.h>

static int do_kexec(char **a)
{
    int k = open(a[0], O_RDONLY), i = open(a[1], O_RDONLY);
    if (k < 0 || i < 0) { perror("open"); return 1; }
    if (syscall(SYS_kexec_file_load, k, i, strlen(a[2]) + 1, a[2], 0UL) != 0) { perror("kexec_file_load"); return 1; }
    sync();
    reboot(RB_KEXEC);
    perror("reboot(KEXEC)");
    return 1;
}

static int do_eject(const char *dev)
{
    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { perror(dev); return 1; }
    unsigned char cdb[6] = { 0x1B, 0, 0, 0, 0x02, 0 }, sense[32];
    struct sg_io_hdr h;
    memset(&h, 0, sizeof(h));
    h.interface_id = 'S';
    h.dxfer_direction = SG_DXFER_NONE;
    h.cmd_len = sizeof(cdb);
    h.cmdp = cdb;
    h.mx_sb_len = sizeof(sense);
    h.sbp = sense;
    h.timeout = 5000;
    if (ioctl(fd, SG_IO, &h) != 0) { perror("SG_IO"); return 1; }
    return h.status ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "kexec")) return do_kexec(argv + 2);
    if (argc == 3 && !strcmp(argv[1], "eject")) return do_eject(argv[2]);
    fprintf(stderr, "usage: s1tool kexec KERNEL INITRD CMDLINE | s1tool eject /dev/sdX\n");
    return 2;
}
