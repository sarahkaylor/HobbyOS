#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  main();
  exit(0);
}
#endif

char current_dir[128] = "/";

void trim_spaces(char *out, const char *in) {
    while (*in == ' ' || *in == '\t') in++;
    int len = 0;
    while (in[len]) {
        out[len] = in[len];
        len++;
    }
    out[len] = '\0';
    while (len > 0 && (out[len-1] == ' ' || out[len-1] == '\t' || out[len-1] == '\n' || out[len-1] == '\r')) {
        out[len-1] = '\0';
        len--;
    }
}

void to_upper_filename(const char *cmd, char *bin_out, char *arg_out) {
    int i = 0;
    while (cmd[i] && i < 8) {
        char c = cmd[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        bin_out[i] = c;
        arg_out[i] = c;
        i++;
    }
    bin_out[i] = '.';
    bin_out[i+1] = 'B';
    bin_out[i+2] = 'I';
    bin_out[i+3] = 'N';
    bin_out[i+4] = '\0';

    arg_out[i] = '.';
    arg_out[i+1] = 'A';
    arg_out[i+2] = 'R';
    arg_out[i+3] = 'G';
    arg_out[i+4] = '\0';
}

void sanitize_command(const char *cmd, char *bin_out, char *arg_out) {
    char temp[32];
    int len = 0;
    while (cmd[len] && len < 31) {
        temp[len] = cmd[len];
        len++;
    }
    temp[len] = '\0';
    if (len > 4 && 
        (temp[len-4] == '.' && 
         (temp[len-3] == 'b' || temp[len-3] == 'B') &&
         (temp[len-2] == 'i' || temp[len-2] == 'I') &&
         (temp[len-1] == 'n' || temp[len-1] == 'N'))) {
        temp[len-4] = '\0';
    }
    to_upper_filename(temp, bin_out, arg_out);
}

void handle_cd(const char *path, char *current_dir) {
    if (!path || path[0] == '\0') {
        current_dir[0] = '/';
        current_dir[1] = '\0';
        return;
    }
    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < 127) {
            current_dir[i] = path[i];
            i++;
        }
        current_dir[i] = '\0';
    } else {
        int cur_len = 0;
        while (current_dir[cur_len]) cur_len++;
        if (cur_len > 1) {
            current_dir[cur_len++] = '/';
        }
        int i = 0;
        while (path[i] && (cur_len + i) < 127) {
            current_dir[cur_len++] = path[i];
            i++;
        }
        current_dir[cur_len] = '\0';
    }

    char cleaned[128];
    int c_len = 0;
    cleaned[c_len++] = '/';

    char *parts[16];
    char temp_dir[128];
    int t_len = 0;
    while (current_dir[t_len]) {
        temp_dir[t_len] = current_dir[t_len];
        t_len++;
    }
    temp_dir[t_len] = '\0';

    int part_count = 0;
    char *p = temp_dir;
    while (*p) {
        while (*p == '/') {
            *p = '\0';
            p++;
        }
        if (*p == '\0') break;
        parts[part_count++] = p;
        while (*p && *p != '/') {
            p++;
        }
    }

    char *stack[16];
    int stack_top = 0;
    for (int i = 0; i < part_count; i++) {
        if (parts[i][0] == '.' && parts[i][1] == '\0') {
            continue;
        }
        if (parts[i][0] == '.' && parts[i][1] == '.' && parts[i][2] == '\0') {
            if (stack_top > 0) stack_top--;
        } else {
            if (stack_top < 16) {
                stack[stack_top++] = parts[i];
            }
        }
    }

    for (int i = 0; i < stack_top; i++) {
        if (i > 0) {
            cleaned[c_len++] = '/';
        }
        int j = 0;
        while (stack[i][j]) {
            cleaned[c_len++] = stack[i][j++];
        }
    }
    cleaned[c_len] = '\0';

    int i = 0;
    while (cleaned[i]) {
        current_dir[i] = cleaned[i];
        i++;
    }
    current_dir[i] = '\0';
}

static void write_str(int fd, const char* str) {
    int len = 0;
    while (str[len]) len++;
    write(fd, str, len);
}

static void parse_redirection(char* cmd_line, char* out_file, char* err_file) {
    out_file[0] = '\0';
    err_file[0] = '\0';
    
    int i = 0;
    while (cmd_line[i]) {
        if ((cmd_line[i] == '1' || cmd_line[i] == '2') && cmd_line[i+1] == '>') {
            int is_err = (cmd_line[i] == '2');
            int op_start = i;
            i += 2;
            while (cmd_line[i] == ' ' || cmd_line[i] == '\t') i++;
            int file_start = i;
            while (cmd_line[i] && cmd_line[i] != ' ' && cmd_line[i] != '\t' && cmd_line[i] != '>' && cmd_line[i] != '|') i++;
            int file_len = i - file_start;
            if (file_len > 0) {
                char* target = is_err ? err_file : out_file;
                int k = 0;
                while (k < file_len && k < 63) {
                    target[k] = cmd_line[file_start + k];
                    k++;
                }
                target[k] = '\0';
                
                int shift_len = i - op_start;
                int m = op_start;
                while (cmd_line[m + shift_len]) {
                    cmd_line[m] = cmd_line[m + shift_len];
                    m++;
                }
                cmd_line[m] = '\0';
                i = 0;
                continue;
            }
        }
        else if (cmd_line[i] == '>') {
            int op_start = i;
            i += 1;
            while (cmd_line[i] == ' ' || cmd_line[i] == '\t') i++;
            int file_start = i;
            while (cmd_line[i] && cmd_line[i] != ' ' && cmd_line[i] != '\t' && cmd_line[i] != '>' && cmd_line[i] != '|') i++;
            int file_len = i - file_start;
            if (file_len > 0) {
                int k = 0;
                while (k < file_len && k < 63) {
                    out_file[k] = cmd_line[file_start + k];
                    k++;
                }
                out_file[k] = '\0';
                
                int shift_len = i - op_start;
                int m = op_start;
                while (cmd_line[m + shift_len]) {
                    cmd_line[m] = cmd_line[m + shift_len];
                    m++;
                }
                cmd_line[m] = '\0';
                i = 0;
                continue;
            }
        }
        i++;
    }
}

void execute_command(const char *cmd_line) {
    char trimmed[256];
    trim_spaces(trimmed, cmd_line);
    if (trimmed[0] == '\0') return;

    char out_file[64];
    char err_file[64];
    parse_redirection(trimmed, out_file, err_file);

    char cleaned_cmd[256];
    trim_spaces(cleaned_cmd, trimmed);
    int m = 0;
    while (cleaned_cmd[m]) {
        trimmed[m] = cleaned_cmd[m];
        m++;
    }
    trimmed[m] = '\0';
    if (trimmed[0] == '\0') return;

    int out_fd = -1;
    int err_fd = -1;
    if (out_file[0] != '\0') {
        out_fd = open(out_file);
        if (out_fd < 0) {
            print("sh: failed to open stdout redirect file\n");
            return;
        }
    }
    if (err_file[0] != '\0') {
        err_fd = open(err_file);
        if (err_fd < 0) {
            if (out_fd >= 0) close(out_fd);
            print("sh: failed to open stderr redirect file\n");
            return;
        }
    }

    int stdout_param = (out_fd >= 0) ? out_fd : 1;
    int stderr_param = (err_fd >= 0) ? err_fd : 2;

    int is_bg = 0;
    int len = 0;
    while (trimmed[len]) len++;
    if (len > 0 && trimmed[len - 1] == '&') {
        is_bg = 1;
        trimmed[len - 1] = '\0';
        char temp[256];
        trim_spaces(temp, trimmed);
        int i = 0;
        while (temp[i]) {
            trimmed[i] = temp[i];
            i++;
        }
        trimmed[i] = '\0';
    }

    // Check for pipe
    int pipe_idx = -1;
    for (int i = 0; trimmed[i]; i++) {
        if (trimmed[i] == '|') {
            pipe_idx = i;
            break;
        }
    }

    if (pipe_idx >= 0) {
        trimmed[pipe_idx] = '\0';
        char left[128];
        char right[128];
        trim_spaces(left, trimmed);
        trim_spaces(right, trimmed + pipe_idx + 1);

        char left_cmd[32];
        char left_args[128];
        int i = 0;
        while (left[i] && left[i] != ' ' && left[i] != '\t') {
            left_cmd[i] = left[i];
            i++;
        }
        left_cmd[i] = '\0';
        while (left[i] == ' ' || left[i] == '\t') i++;
        int j = 0;
        while (left[i]) {
            left_args[j++] = left[i++];
        }
        left_args[j] = '\0';

        char right_cmd[32];
        char right_args[128];
        i = 0;
        while (right[i] && right[i] != ' ' && right[i] != '\t') {
            right_cmd[i] = right[i];
            i++;
        }
        right_cmd[i] = '\0';
        while (right[i] == ' ' || right[i] == '\t') i++;
        j = 0;
        while (right[i]) {
            right_args[j++] = right[i++];
        }
        right_args[j] = '\0';

        int p[2];
        if (pipe(p) != 0) {
            print("sh: failed to create pipe\n");
            if (out_fd >= 0) close(out_fd);
            if (err_fd >= 0) close(err_fd);
            return;
        }

        char left_bin[32], left_arg_file[32];
        sanitize_command(left_cmd, left_bin, left_arg_file);

        char right_bin[32], right_arg_file[32];
        sanitize_command(right_cmd, right_bin, right_arg_file);

        int pid_left = spawn2(left_bin, 0, p[1], stderr_param, left_args);
        int pid_right = spawn2(right_bin, p[0], stdout_param, stderr_param, right_args);

        close(p[0]);
        close(p[1]);

        if (pid_left >= 0 && pid_right >= 0) {
            while (kill(pid_left, 0) == 0 || kill(pid_right, 0) == 0) {
                yield();
            }
        } else {
            print("sh: failed to spawn piped commands\n");
        }

        if (out_fd >= 0) close(out_fd);
        if (err_fd >= 0) close(err_fd);
        return;
    }

    char cmd_name[32];
    char args[128];
    int i = 0;
    while (trimmed[i] && trimmed[i] != ' ' && trimmed[i] != '\t') {
        cmd_name[i] = trimmed[i];
        i++;
    }
    cmd_name[i] = '\0';

    while (trimmed[i] == ' ' || trimmed[i] == '\t') i++;
    int j = 0;
    while (trimmed[i]) {
        args[j++] = trimmed[i++];
    }
    args[j] = '\0';

    if (cmd_name[0] == 'c' && cmd_name[1] == 'd' && cmd_name[2] == '\0') {
        handle_cd(args, current_dir);
    } else if (cmd_name[0] == 'c' && cmd_name[1] == 'l' && cmd_name[2] == 'e' && cmd_name[3] == 'a' && cmd_name[4] == 'r' && cmd_name[5] == '\0') {
        write_str(stdout_param, "\033[2J\033[H\f");
    } else if (cmd_name[0] == 'e' && cmd_name[1] == 'c' && cmd_name[2] == 'h' && cmd_name[3] == 'o' && cmd_name[4] == '\0') {
        write_str(stdout_param, args);
        write_str(stdout_param, "\n");
    } else if (cmd_name[0] == 'h' && cmd_name[1] == 'e' && cmd_name[2] == 'l' && cmd_name[3] == 'p' && cmd_name[4] == '\0') {
        write_str(stdout_param, "HobbyOS Bash-like Shell\n");
        write_str(stdout_param, "Available Built-ins:\n");
        write_str(stdout_param, "  cd [dir]   - Change directory (simulated)\n");
        write_str(stdout_param, "  help       - Display this help message\n");
        write_str(stdout_param, "  clear      - Clear terminal screen\n");
        write_str(stdout_param, "  echo [msg] - Print message\n");
        write_str(stdout_param, "Available External Commands:\n");
        write_str(stdout_param, "  ls         - List files in current directory\n");
        write_str(stdout_param, "  cat        - Concat and display files\n");
        write_str(stdout_param, "  grep       - Search for pattern in files or stdin\n");
        write_str(stdout_param, "  less       - File pager\n");
        write_str(stdout_param, "  tail       - Display last lines of a file\n");
        write_str(stdout_param, "  head       - Display first lines of a file\n");
    } else {
        char bin_file[32], arg_file[32];
        sanitize_command(cmd_name, bin_file, arg_file);

        int pid = spawn2(bin_file, 0, stdout_param, stderr_param, args);
        if (pid < 0) {
            print("sh: command not found: ");
            print(cmd_name);
            print("\n");
        } else {
            if (is_bg) {
                print("[bg] spawned ");
                print(cmd_name);
                print(" (pid ");
                print_hex(pid);
                print(")\n");
            } else {
                while (kill(pid, 0) == 0) {
                    yield();
                }
            }
        }
    }

    if (out_fd >= 0) close(out_fd);
    if (err_fd >= 0) close(err_fd);
}

int main(void) {
    print("\f=== Welcome to HobbyOS Shell ===\nType 'help' to see list of commands.\n\n");
    
    char line_buf[256];
    int line_len = 0;

    print("user@hobbyos:");
    print(current_dir);
    print("$ ");
    while (1) {
        char c;
        int r = read(0, &c, 1);
        if (r > 0) {
            if (c == '\n') {
                line_buf[line_len] = '\0';
                print("\n");
                execute_command(line_buf);
                
                print("user@hobbyos:");
                print(current_dir);
                print("$ ");
                line_len = 0;
            } else if (c == '\b') {
                if (line_len > 0) {
                    line_len--;
                    write(1, "\b", 1);
                }
            } else if (c == 27) {
                // Consume arrow keys / escape sequences
                char seq[2];
                if (read(0, seq, 2) != 2) {
                    // Just escape
                }
            } else if (c >= 32 && c <= 126) {
                if (line_len < 255) {
                    line_buf[line_len++] = c;
                    write(1, &c, 1);
                }
            }
        } else {
            print("sh: stdin EOF, exiting\n");
            break;
        }
    }
    return 0;
}
