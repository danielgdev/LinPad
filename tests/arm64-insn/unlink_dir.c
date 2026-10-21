// unlink() of a directory must fail with EISDIR as on Linux, not the host's
// (Darwin) EPERM: Bun's recursive rm only falls back to rmdir on EISDIR, so the
// Claude Code installer failed with "EPERM: operation not permitted, rm".
// Files still unlink, and a missing path is still ENOENT.
// Guest-only:  gcc -O1 -o unlink_dir unlink_dir.c && ./unlink_dir    expected: "unlink_dir ok"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void expect(const char *what, int r, int err) {
    if (r != -1 || errno != err) {
        printf("%s: got %d (%s), want -1 (%s)\n", what, r, r == -1 ? strerror(errno) : "success", strerror(err));
        fails++;
    }
}

int main(void) {
    char dir[] = "/tmp/unlink_dir.XXXXXX";
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    char sub[64], file[64];
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    snprintf(file, sizeof(file), "%s/file", dir);
    if (mkdir(sub, 0755) < 0 || close(open(file, O_CREAT | O_WRONLY, 0644)) < 0) {
        perror("setup");
        return 1;
    }

    expect("unlink(empty dir)", unlink(sub), EISDIR);
    expect("unlinkat(empty dir, 0)", unlinkat(AT_FDCWD, sub, 0), EISDIR);
    expect("unlink(non-empty dir)", unlink(dir), EISDIR);
    expect("unlink(missing)", unlink("/tmp/unlink_dir.missing"), ENOENT);
    if (unlink(file) != 0) {
        printf("unlink(file): %s\n", strerror(errno));
        fails++;
    }
    if (unlinkat(AT_FDCWD, sub, AT_REMOVEDIR) != 0 || rmdir(dir) != 0) {
        printf("rmdir: %s\n", strerror(errno));
        fails++;
    }

    if (fails)
        return 1;
    puts("unlink_dir ok");
    return 0;
}
