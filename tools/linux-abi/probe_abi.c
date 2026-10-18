/* tools/linux-abi/probe_abi.c — GB-010 ground truth: emit the real numbers the
 * Linux x86_64 kernel ABI uses for the structs the compat layer must match
 * (offsets/sizes as compiled by the host libc, which mirrors
 * arch/x86/include/uapi/asm + asm-generic).  Run: make -f tools/linux-abi/Makefile
 * Output feeds docs/linux-abi.md.  Never hand-edit numbers there from memory.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <asm/prctl.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <elf.h>
#include <dirent.h>
#include <time.h>
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define P(expr) printf("%-44s = %zu\n", #expr, (size_t)(expr))
#define S(t)    printf("sizeof(%s)%*s = %zu\n", #t, 44 - 8 - (int)sizeof(#t), "", sizeof(t))

static void off(const char *name, size_t v) { printf("  %-42s = %zu\n", name, v); }

int main(void) {
  printf("=== arch: sizeof(long)=%zu  sizeof(void*)=%zu  PAGE=%ld\n",
         sizeof(long), sizeof(void *), sysconf(_SC_PAGESIZE));

  printf("\n=== numbers that must match ==================================\n");
  printf("O_RDONLY=0 O_WRONLY=1 O_RDWR=2 O_CREAT=0x%x O_TRUNC=0x%x O_APPEND=0x%x O_NONBLOCK=0x%x O_DIRECTORY=0x%x\n",
         O_CREAT, O_TRUNC, O_APPEND, O_NONBLOCK, O_DIRECTORY);
  printf("AT_FDCWD=%d  SEEK_SET=%d SEEK_CUR=%d SEEK_END=%d\n", AT_FDCWD, SEEK_SET, SEEK_CUR, SEEK_END);
  printf("S_IFMT=0%o S_IFREG=0%o S_IFDIR=0%o S_IFCHR=0%o S_IFLNK=0%o S_ISUID=0%o\n",
         S_IFMT, S_IFREG, S_IFDIR, S_IFCHR, S_IFLNK, S_ISUID);
  printf("PROT_READ=0x%x PROT_WRITE=0x%x PROT_EXEC=0x%x MAP_PRIVATE=0x%x MAP_SHARED=0x%x MAP_ANONYMOUS=0x%x MAP_FIXED=0x%x\n",
         PROT_READ, PROT_WRITE, PROT_EXEC, MAP_PRIVATE, MAP_SHARED, MAP_ANONYMOUS, MAP_FIXED);
  printf("CLOCK_REALTIME=%d CLOCK_MONOTONIC=%d O_CLOEXEC=0x%x\n", CLOCK_REALTIME, CLOCK_MONOTONIC, O_CLOEXEC);
  printf("POLLIN=0x%x POLLOUT=0x%x POLLERR=0x%x POLLHUP=0x%x\n", POLLIN, POLLOUT, POLLERR, POLLHUP);
  printf("F_GETFL=%d F_SETFL=%d F_GETLK=%d F_SETLK=%d F_SETLKW=%d F_DUPFD=%d\n",
         F_GETFL, F_SETFL, F_GETLK, F_SETLK, F_SETLKW, F_DUPFD);
  printf("WIFEXITED=%d WIFSIGNALED=%d WEXITSTATUS(7<<8)=%d WTERMSIG(9)=%d\n",
         WIFEXITED(1 << 8), WIFSIGNALED(1), WEXITSTATUS(7 << 8), WTERMSIG(9));
  printf("SIG_BLOCK=%d SIG_UNBLOCK=%d SIG_SETMASK=%d SA_SIGINFO=0x%x SA_RESTART=0x%x SA_RESTORER=0x%x\n",
         SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK, SA_SIGINFO, SA_RESTART, 0x04000000 /*SA_RESTORER, kernel-only spelling*/);
  printf("SIGCHLD=%d SIGSEGV=%d SIGINT=%d SIGWINCH=%d SIGKILL=%d SIGSTOP=%d\n",
         SIGCHLD, SIGSEGV, SIGINT, SIGWINCH, SIGKILL, SIGSTOP);
  printf("WNOHANG=%d  ARCH_SET_FS=0x%x  EPOLLIN=0x%x EPOLLOUT=0x%x EPOLLET=0x%x\n",
         WNOHANG, ARCH_SET_FS, EPOLLIN, EPOLLOUT, EPOLLET);
  printf("TCGETS=0x%lx TIOCGWINSZ=0x%lx FIONREAD=0x%lx FIONBIO=0x%lx\n",
         (unsigned long)TCGETS, (unsigned long)TIOCGWINSZ, (unsigned long)FIONREAD, (unsigned long)FIONBIO);
  printf("FBIOGET_VSCREENINFO=0x%lx FBIOGET_FSCREENINFO=0x%lx FBIOBLANK=0x%lx\n",
         (unsigned long)FBIOGET_VSCREENINFO, (unsigned long)FBIOGET_FSCREENINFO, (unsigned long)FBIOBLANK);
  printf("EVIOCGVERSION=0x%lx EVIOCGBIT(0,..)=0x%lx EVIOCGNAME=%#lx\n",
         (unsigned long)EVIOCGVERSION, (unsigned long)EVIOCGBIT(0, 0), (unsigned long)EVIOCGNAME(0));
  printf("RLIMIT_NOFILE=%d  DT_REG=%d DT_DIR=%d DT_CHR=%d\n", RLIMIT_NOFILE, DT_REG, DT_DIR, DT_CHR);
  printf("NOTE: syscall numbers below come from the kernel headers, not libc\n");

  printf("\n=== syscall numbers (x86_64) ==================================\n");
  struct { const char *n; long v; } sc[] = {
    {"read", SYS_read}, {"write", SYS_write}, {"open", SYS_open}, {"close", SYS_close},
    {"stat", SYS_stat}, {"fstat", SYS_fstat}, {"lstat", SYS_lstat}, {"poll", SYS_poll},
    {"lseek", SYS_lseek}, {"mmap", SYS_mmap}, {"mprotect", SYS_mprotect}, {"munmap", SYS_munmap},
    {"brk", SYS_brk}, {"rt_sigaction", SYS_rt_sigaction}, {"rt_sigprocmask", SYS_rt_sigprocmask},
    {"rt_sigreturn", SYS_rt_sigreturn}, {"ioctl", SYS_ioctl}, {"pread64", SYS_pread64},
    {"pwrite64", SYS_pwrite64}, {"readv", SYS_readv}, {"writev", SYS_writev}, {"access", SYS_access},
    {"pipe", SYS_pipe}, {"select", SYS_select}, {"sched_yield", SYS_sched_yield},
    {"mremap", SYS_mremap}, {"msync", SYS_msync}, {"mincore", SYS_mincore}, {"madvise", SYS_madvise},
    {"dup", SYS_dup}, {"dup2", SYS_dup2}, {"nanosleep", SYS_nanosleep}, {"getitimer", SYS_getitimer},
    {"getpid", SYS_getpid}, {"socket", SYS_socket}, {"connect", SYS_connect}, {"accept", SYS_accept},
    {"sendto", SYS_sendto}, {"recvfrom", SYS_recvfrom}, {"clone", SYS_clone}, {"fork", SYS_fork},
    {"vfork", SYS_vfork}, {"execve", SYS_execve}, {"exit", SYS_exit}, {"wait4", SYS_wait4},
    {"kill", SYS_kill}, {"uname", SYS_uname}, {"fcntl", SYS_fcntl}, {"fsync", SYS_fsync},
    {"getcwd", SYS_getcwd}, {"chdir", SYS_chdir}, {"rename", SYS_rename}, {"mkdir", SYS_mkdir},
    {"rmdir", SYS_rmdir}, {"creat", SYS_creat}, {"link", SYS_link}, {"unlink", SYS_unlink},
    {"readlink", SYS_readlink}, {"chmod", SYS_chmod}, {"fchmod", SYS_fchmod}, {"umask", SYS_umask},
    {"gettimeofday", SYS_gettimeofday}, {"getrlimit", SYS_getrlimit}, {"getrusage", SYS_getrusage},
    {"sysinfo", SYS_sysinfo}, {"times", SYS_times}, {"getuid", SYS_getuid}, {"getgid", SYS_getgid},
    {"geteuid", SYS_geteuid}, {"getegid", SYS_getegid}, {"setpgid", SYS_setpgid}, {"getppid", SYS_getppid},
    {"arch_prctl", SYS_arch_prctl}, {"gettid", SYS_gettid}, {"futex", SYS_futex},
    {"sched_setaffinity", SYS_sched_setaffinity}, {"sched_getaffinity", SYS_sched_getaffinity},
    {"fadvise64", SYS_fadvise64}, {"exit_group", SYS_exit_group}, {"epoll_wait", SYS_epoll_wait},
    {"epoll_ctl", SYS_epoll_ctl}, {"epoll_create", SYS_epoll_create}, {"epoll_create1", SYS_epoll_create1},
    {"tgkill", SYS_tgkill}, {"openat", SYS_openat}, {"mkdirat", SYS_mkdirat}, {"fstatat", SYS_newfstatat},
    {"unlinkat", SYS_unlinkat}, {"renameat", SYS_renameat}, {"readlinkat", SYS_readlinkat},
    {"fchmodat", SYS_fchmodat}, {"faccessat", SYS_faccessat}, {"pselect6", SYS_pselect6},
    {"ppoll", SYS_ppoll}, {"set_robust_list", SYS_set_robust_list}, {"splice", SYS_splice},
    {"tee", SYS_tee}, {"utimensat", SYS_utimensat}, {"signalfd", SYS_signalfd},
    {"eventfd", SYS_eventfd}, {"eventfd2", SYS_eventfd2}, {"epoll_pwait", SYS_epoll_pwait},
    {"dup3", SYS_dup3}, {"pipe2", SYS_pipe2}, {"inotify_init1", SYS_inotify_init1},
    {"preadv", SYS_preadv}, {"pwritev", SYS_pwritev}, {"prlimit64", SYS_prlimit64},
    {"dup3", SYS_dup3}, {"clock_gettime", SYS_clock_gettime}, {"clock_getres", SYS_clock_getres},
    {"clock_nanosleep", SYS_clock_nanosleep}, {"getrandom", SYS_getrandom}, {"memfd_create", SYS_memfd_create},
    {"statx", SYS_statx}, {"rseq", SYS_rseq}, {"clone3", SYS_clone3}, {"close_range", SYS_close_range},
    {"faccessat2", SYS_faccessat2}, {"process_madvise", SYS_process_madvise},
  };
  for (size_t i = 0; i < sizeof(sc) / sizeof(sc[0]); i++) printf("  %-22s %ld\n", sc[i].n, sc[i].v);

  printf("\n=== errno values =============================================\n");
#define E(n) printf("  %-12s = %d\n", #n, n)
  E(EPERM); E(ENOENT); E(ESRCH); E(EINTR); E(EIO); E(ENXIO); E(E2BIG); E(ENOEXEC); E(EBADF); E(ECHILD);
  E(EAGAIN); E(ENOMEM); E(EACCES); E(EFAULT); E(EBUSY); E(EEXIST); E(EXDEV); E(ENODEV); E(ENOTDIR);
  E(EISDIR); E(EINVAL); E(ENFILE); E(EMFILE); E(ENOTTY); E(ETXTBSY); E(EFBIG); E(ENOSPC); E(ESPIPE);
  E(EROFS); E(EMLINK); E(EPIPE); E(EDOM); E(ERANGE); E(ENAMETOOLONG); E(ENOSYS); E(ENOTEMPTY); E(ELOOP);
  E(EOVERFLOW); E(EMSGSIZE); E(EPROTOTYPE); E(ENOPROTOOPT); E(EPROTONOSUPPORT); E(EAFNOSUPPORT);
  E(EADDRINUSE); E(EADDRNOTAVAIL); E(ENETDOWN); E(ENETUNREACH); E(ECONNABORTED); E(ECONNRESET);
  E(ENOBUFS); E(EISCONN); E(ENOTCONN); E(ETIMEDOUT); E(ECONNREFUSED); E(EHOSTUNREACH); E(EALREADY);
  E(EINPROGRESS); E(EDESTADDRREQ); E(ENOTSOCK); E(ENOTSUP); E(EWOULDBLOCK); E(EDEADLK); E(ENOLCK);

  printf("\n=== struct layouts ===========================================\n");
  S(struct stat);
  off("st_dev", offsetof(struct stat, st_dev));
  off("st_ino", offsetof(struct stat, st_ino));
  off("st_nlink", offsetof(struct stat, st_nlink));
  off("st_mode", offsetof(struct stat, st_mode));
  off("st_uid", offsetof(struct stat, st_uid));
  off("st_gid", offsetof(struct stat, st_gid));
  off("st_rdev", offsetof(struct stat, st_rdev));
  off("st_size", offsetof(struct stat, st_size));
  off("st_blksize", offsetof(struct stat, st_blksize));
  off("st_blocks", offsetof(struct stat, st_blocks));
  off("st_atim", offsetof(struct stat, st_atim));
  off("st_mtim", offsetof(struct stat, st_mtim));
  off("st_ctim", offsetof(struct stat, st_ctim));

  S(struct statx); off("stx_mask", offsetof(struct statx, stx_mask));
  off("stx_size", offsetof(struct statx, stx_size)); off("stx_mode", offsetof(struct statx, stx_mode));

  S(struct sigaction);
  off("sa_handler", offsetof(struct sigaction, sa_handler));
  off("sa_mask", offsetof(struct sigaction, sa_mask));
  off("sa_flags", offsetof(struct sigaction, sa_flags));
  off("sa_restorer", offsetof(struct sigaction, sa_restorer));

  printf("sizeof(ucontext_t) = %zu\n", sizeof(ucontext_t));
  off("uc_flags", offsetof(ucontext_t, uc_flags));
  off("uc_link", offsetof(ucontext_t, uc_link));
  off("uc_stack", offsetof(ucontext_t, uc_stack));
  off("uc_mcontext", offsetof(ucontext_t, uc_mcontext));
  off("uc_sigmask", offsetof(ucontext_t, uc_sigmask));
  S(mcontext_t);
  off("gregs", offsetof(mcontext_t, gregs));
  off("fpregs", offsetof(mcontext_t, fpregs));
  off("(RDI idx)", (size_t)REG_RDI); off("(RSI idx)", (size_t)REG_RSI);
  off("(RDX idx)", (size_t)REG_RDX); off("(RAX idx)", (size_t)REG_RAX);
  off("(RSP idx)", (size_t)REG_RSP); off("(RIP idx)", (size_t)REG_RIP);
  S(stack_t);
  printf("SS_ONSTACK=%d SS_DISABLE=%d MINSIGSTKSZ=%d SIGSTKSZ=%d\n", SS_ONSTACK, SS_DISABLE, MINSIGSTKSZ, SIGSTKSZ);

  S(struct termios);
  off("c_iflag", offsetof(struct termios, c_iflag));
  off("c_oflag", offsetof(struct termios, c_oflag));
  off("c_cflag", offsetof(struct termios, c_cflag));
  off("c_lflag", offsetof(struct termios, c_lflag));
  off("c_line", offsetof(struct termios, c_line));
  off("c_cc", offsetof(struct termios, c_cc));
  off("c_ispeed", offsetof(struct termios, c_ispeed));
  off("c_ospeed", offsetof(struct termios, c_ospeed));
  S(struct winsize);
  off("ws_row", offsetof(struct winsize, ws_row)); off("ws_col", offsetof(struct winsize, ws_col));
  off("ws_xpixel", offsetof(struct winsize, ws_xpixel)); off("ws_ypixel", offsetof(struct winsize, ws_ypixel));

  S(struct epoll_event); off("data", offsetof(struct epoll_event, data)); off("events", offsetof(struct epoll_event, events));
  S(struct pollfd); off("fd", offsetof(struct pollfd, fd)); off("events", offsetof(struct pollfd, events)); off("revents", offsetof(struct pollfd, revents));
  S(struct iovec); off("iov_base", offsetof(struct iovec, iov_base)); off("iov_len", offsetof(struct iovec, iov_len));
  S(struct timespec); off("tv_sec", offsetof(struct timespec, tv_sec)); off("tv_nsec", offsetof(struct timespec, tv_nsec));
  S(struct timeval); off("tv_sec", offsetof(struct timeval, tv_sec)); off("tv_usec", offsetof(struct timeval, tv_usec));
  S(struct timezone);
  S(struct itimerval); S(struct rusage); S(struct rlimit); S(struct sysinfo);
  S(struct dirent); off("d_ino", offsetof(struct dirent, d_ino)); off("d_off", offsetof(struct dirent, d_off));
  off("d_reclen", offsetof(struct dirent, d_reclen)); off("d_type", offsetof(struct dirent, d_type)); off("d_name", offsetof(struct dirent, d_name));
  S(struct dirent64); off("d_ino", offsetof(struct dirent64, d_ino)); off("d_off", offsetof(struct dirent64, d_off));
  off("d_reclen", offsetof(struct dirent64, d_reclen)); off("d_type", offsetof(struct dirent64, d_type)); off("d_name", offsetof(struct dirent64, d_name));
  S(struct utsname); /* <sys/utsname.h> */
  off("sysname", offsetof(struct utsname, sysname)); off("nodename", offsetof(struct utsname, nodename));
  off("release", offsetof(struct utsname, release)); off("version", offsetof(struct utsname, version));
  off("machine", offsetof(struct utsname, machine)); off("domainname", offsetof(struct utsname, domainname));
  S(struct flock);
  off("l_type", offsetof(struct flock, l_type)); off("l_whence", offsetof(struct flock, l_whence));
  off("l_start", offsetof(struct flock, l_start)); off("l_len", offsetof(struct flock, l_len)); off("l_pid", offsetof(struct flock, l_pid));
  S(stack_t);
  S(struct fb_var_screeninfo); off("xres", offsetof(struct fb_var_screeninfo, xres)); off("yres", offsetof(struct fb_var_screeninfo, yres));
  off("bits_per_pixel", offsetof(struct fb_var_screeninfo, bits_per_pixel)); off("xoffset", offsetof(struct fb_var_screeninfo, xoffset));
  off("red", offsetof(struct fb_var_screeninfo, red)); off("green", offsetof(struct fb_var_screeninfo, green));
  off("blue", offsetof(struct fb_var_screeninfo, blue)); off("transp", offsetof(struct fb_var_screeninfo, transp));
  off("activate", offsetof(struct fb_var_screeninfo, activate)); off("vmode", offsetof(struct fb_var_screeninfo, vmode));
  off("rotate", offsetof(struct fb_var_screeninfo, rotate));
  S(struct fb_fix_screeninfo); off("smem_start", offsetof(struct fb_fix_screeninfo, smem_start));
  off("smem_len", offsetof(struct fb_fix_screeninfo, smem_len)); off("line_length", offsetof(struct fb_fix_screeninfo, line_length));
  off("type", offsetof(struct fb_fix_screeninfo, type)); off("visual", offsetof(struct fb_fix_screeninfo, visual));
  printf("FB_TYPE_PACKED_PIXELS=%d FB_VISUAL_TRUECOLOR=%d FB_ACTIVATE_NOW=%d FB_VMODE_NONINTERLACED=%d\n",
         FB_TYPE_PACKED_PIXELS, FB_VISUAL_TRUECOLOR, FB_ACTIVATE_NOW, FB_VMODE_NONINTERLACED);
  S(struct input_event);
  off("time", offsetof(struct input_event, time)); off("type", offsetof(struct input_event, type));
  off("code", offsetof(struct input_event, code)); off("value", offsetof(struct input_event, value));
  S(struct input_absinfo);
  printf("EV_SYN=%d EV_KEY=%d EV_REL=%d EV_ABS=%d EV_MSC=%d EV_LED=%d EV_REP=%d\n",
         EV_SYN, EV_KEY, EV_REL, EV_ABS, EV_MSC, EV_LED, EV_REP);
  printf("BTN_LEFT=0x%x BTN_RIGHT=0x%x BTN_MIDDLE=0x%x\n", BTN_LEFT, BTN_RIGHT, BTN_MIDDLE);
  printf("ABS_X=0x%x ABS_Y=0x%x REL_X=0x%x REL_Y=0x%x REL_WHEEL=0x%x\n",
         ABS_X, ABS_Y, REL_X, REL_Y, REL_WHEEL);
  printf("KEY_A=%d KEY_ENTER=%d KEY_ESC=%d KEY_SPACE=%d\n", KEY_A, KEY_ENTER, KEY_ESC, KEY_SPACE);
  /* GB-042 gamepad/joystick: the evdev BTN_ and ABS_ codes a gamepad node
   * exposes (which src/include/linux_joystick.h locks against <linux/input.h>
   * in joypad_test_host).  Print them here so the header's numbers are
   * reproducible from the canonical measurement tool, not memory. */
  printf("GAMEPAD BTN_A=0x%x BTN_B=0x%x BTN_X=0x%x BTN_Y=0x%x BTN_TL=0x%x BTN_TR=0x%x BTN_TL2=0x%x BTN_TR2=0x%x\n",
         BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2);
  printf("GAMEPAD BTN_SELECT=0x%x BTN_START=0x%x BTN_MODE=0x%x BTN_THUMBL=0x%x BTN_THUMBR=0x%x\n",
         BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR);
  printf("GAMEPAD ABS_X=0x%x ABS_Y=0x%x ABS_Z=0x%x ABS_RX=0x%x ABS_RY=0x%x ABS_RZ=0x%x ABS_HAT0X=0x%x ABS_HAT0Y=0x%x\n",
         ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y);

  printf("\n=== ELF/auxv/proc constants ==================================\n");
  printf("AT_NULL=%d AT_PHDR=%d AT_PHNUM=%d AT_PAGESZ=%d AT_ENTRY=%d AT_BASE=%d AT_EXECFN=%d AT_RANDOM=%d AT_HWCAP=%d AT_SYSINFO_EHDR=%d\n",
         AT_NULL, AT_PHDR, AT_PHNUM, AT_PAGESZ, AT_ENTRY, AT_BASE, AT_EXECFN, AT_RANDOM, AT_HWCAP, AT_SYSINFO_EHDR);
  printf("R_X86_64_RELATIVE=%d R_X86_64_JUMP_SLOT=%d R_X86_64_GLOB_DAT=%d\n",
         R_X86_64_RELATIVE, R_X86_64_JUMP_SLOT, R_X86_64_GLOB_DAT);
  return 0;
}
