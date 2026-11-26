/*
 * Stage-1 的小工具（靜態鏈接，一個二進制多個子命令）：
 *   s1tool kexec KERNEL INITRD "CMDLINE"   kexec_file_load + reboot(KEXEC)
 *   s1tool eject /dev/sdX                  發 SCSI START STOP UNIT（LoEj=1, Start=0）＝彈出。
 *                                          設備收到後上報 "msc eject" 事件，主機據此知道目標機沒網
 *   s1tool msg /dev/sdX "TEXT"             廠商命令 FB "TMSG"：把一行文本交給啟動盤那塊板子，它經藍牙轉給主機
 *   s1tool getcfg /dev/sdX                 廠商命令 FB "GCFG"：讀板子裡存的配置（key=value 每行一條），打到標準輸出
 *   s1tool next /dev/sdX "MODE [SECONDS]"  廠商命令 FB "NEXT"：讓板子在這台機器下次重啟時換成 MODE（hid | hid+msc | msc），
 *                                          只一次；給了 SECONDS 就在那之後換回來。裝完硬盤後用：重啟時不再從板子啟動
 * 進了完整系統之後也用它報告（Stage-2 的種子裡帶一份）。
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

/* 廠商命令 FB + 四個字母。dir = SG_DXFER_TO_DEV / SG_DXFER_FROM_DEV；返回實際傳輸的字節數，失敗 -1 */
static int vendor(const char *dev, const char *tag, int dir, void *data, unsigned len)
{
    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { perror(dev); return -1; }
    unsigned char cdb[10] = { 0xFB, tag[0], tag[1], tag[2], tag[3], 0, 0, (unsigned char)(len >> 8), (unsigned char)len, 0 }, sense[32];
    struct sg_io_hdr h;
    memset(&h, 0, sizeof(h));
    h.interface_id = 'S';
    h.dxfer_direction = dir;
    h.dxferp = data;
    h.dxfer_len = len;
    h.cmd_len = sizeof(cdb);
    h.cmdp = cdb;
    h.mx_sb_len = sizeof(sense);
    h.sbp = sense;
    h.timeout = 3000;
    int rc = ioctl(fd, SG_IO, &h);
    close(fd);
    if (rc != 0 || h.status || h.host_status || h.driver_status) return -1;
    return (int)(len - h.resid);
}

static int do_msg(const char *dev, const char *text)
{
    char buf[120];
    size_t n = strlen(text);
    if (n > sizeof(buf)) n = sizeof(buf);
    memcpy(buf, text, n);
    return vendor(dev, "TMSG", SG_DXFER_TO_DEV, buf, (unsigned)n) < 0;
}

static int do_next(const char *dev, const char *arg)
{
    char buf[40];
    size_t n = strlen(arg);
    if (n >= sizeof(buf)) return 1;
    memcpy(buf, arg, n);
    return vendor(dev, "NEXT", SG_DXFER_TO_DEV, buf, (unsigned)n) < 0;
}

static int do_getcfg(const char *dev)
{
    char buf[1024];
    int n = vendor(dev, "GCFG", SG_DXFER_FROM_DEV, buf, sizeof(buf));
    if (n < 0) return 1;                /* 舊固件不認這條命令：當作沒有配置 */
    fwrite(buf, 1, strnlen(buf, (size_t)n), stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "kexec")) return do_kexec(argv + 2);
    if (argc == 3 && !strcmp(argv[1], "eject")) return do_eject(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "msg")) return do_msg(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "getcfg")) return do_getcfg(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "next")) return do_next(argv[2], argv[3]);
    fprintf(stderr, "usage: s1tool kexec KERNEL INITRD CMDLINE | eject DEV | msg DEV TEXT | getcfg DEV | next DEV \"MODE [SECONDS]\"\n");
    return 2;
}
