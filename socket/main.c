#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SOCKET_PATH "/tmp/ide_bridge.sock"
#define STREAM_BUFFER_SIZE 4096

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} dynamic_buffer;

static void buffer_init(dynamic_buffer *buffer) {
    buffer->capacity = 8192;
    buffer->length = 0;
    buffer->data = malloc(buffer->capacity);
    if (buffer->data != NULL) {
        buffer->data[0] = '\0';
    }
}

static void buffer_reset(dynamic_buffer *buffer) {
    buffer->length = 0;
    if (buffer->data != NULL) {
        buffer->data[0] = '\0';
    }
}

static int buffer_append(dynamic_buffer *buffer, const char *data, size_t len) {
    if (buffer->data == NULL) {
        return -1;
    }

    if (buffer->length + len + 1 > buffer->capacity) {
        size_t new_capacity = buffer->capacity;
        while (buffer->length + len + 1 > new_capacity) {
            new_capacity *= 2;
        }
        char *grown = realloc(buffer->data, new_capacity);
        if (grown == NULL) {
            return -1;
        }
        buffer->data = grown;
        buffer->capacity = new_capacity;
    }

    memcpy(buffer->data + buffer->length, data, len);
    buffer->length += len;
    buffer->data[buffer->length] = '\0';
    return 0;
}

static int write_all(int fd, const void *buffer, size_t len) {
    const char *cursor = buffer;
    while (len > 0) {
        ssize_t written = write(fd, cursor, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        cursor += written;
        len -= (size_t) written;
    }
    return 0;
}

static int send_header(int client_fd, const char *header) {
    if (write_all(client_fd, header, strlen(header)) != 0) {
        return -1;
    }
    return write_all(client_fd, "\n", 1);
}

static int send_frame(int client_fd, const char *type, const char *payload, size_t len) {
    char header[64];
    snprintf(header, sizeof(header), "%s %zu", type, len);
    if (send_header(client_fd, header) != 0) {
        return -1;
    }
    if (len > 0 && write_all(client_fd, payload, len) != 0) {
        return -1;
    }
    return 0;
}

static void send_error(int client_fd, const char *message) {
    send_frame(client_fd, "ERROR", message, strlen(message));
    send_header(client_fd, "END");
}

static char *read_line(int fd) {
    size_t capacity = 128;
    size_t length = 0;
    char *line = malloc(capacity);
    if (line == NULL) {
        return NULL;
    }

    while (1) {
        char ch;
        ssize_t read_size = read(fd, &ch, 1);
        if (read_size == 0) {
            break;
        }
        if (read_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(line);
            return NULL;
        }
        if (ch == '\n') {
            break;
        }
        if (length + 1 >= capacity) {
            capacity *= 2;
            char *grown = realloc(line, capacity);
            if (grown == NULL) {
                free(line);
                return NULL;
            }
            line = grown;
        }
        line[length++] = ch;
    }

    line[length] = '\0';
    return line;
}

static char *read_exact_payload(int fd, size_t len) {
    char *payload = malloc(len + 1);
    if (payload == NULL) {
        return NULL;
    }

    size_t offset = 0;
    while (offset < len) {
        ssize_t read_size = read(fd, payload + offset, len - offset);
        if (read_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(payload);
            return NULL;
        }
        if (read_size == 0) {
            break;
        }
        offset += (size_t) read_size;
    }
    payload[offset] = '\0';
    return payload;
}

static char *read_named_payload(int fd, const char *expected_name) {
    char *header = read_line(fd);
    if (header == NULL) {
        return NULL;
    }

    char *separator = strchr(header, ' ');
    if (separator == NULL) {
        free(header);
        return NULL;
    }

    *separator = '\0';
    size_t payload_len = (size_t) strtoul(separator + 1, NULL, 10);
    char *payload = NULL;
    if (strcmp(header, expected_name) == 0) {
        payload = read_exact_payload(fd, payload_len);
    }

    free(header);
    return payload;
}

static int read_end(int fd) {
    char *header = read_line(fd);
    if (header == NULL) {
        return -1;
    }
    int ok = strcmp(header, "END") == 0 ? 0 : -1;
    free(header);
    return ok;
}

static int run_and_stream(
    int client_fd,
    const char *working_dir,
    char *const argv[],
    int stream_output,
    dynamic_buffer *captured_output
) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);

        if (working_dir != NULL && chdir(working_dir) != 0) {
            dprintf(STDERR_FILENO, "Failed to change directory to %s: %s\n", working_dir, strerror(errno));
            _exit(127);
        }

        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "Failed to launch %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(pipe_fds[1]);
    char chunk[STREAM_BUFFER_SIZE];
    while (1) {
        ssize_t read_size = read(pipe_fds[0], chunk, sizeof(chunk));
        if (read_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (read_size == 0) {
            break;
        }

        buffer_append(captured_output, chunk, (size_t) read_size);
        if (stream_output && client_fd >= 0) {
            send_frame(client_fd, "CHUNK", chunk, (size_t) read_size);
        }
    }
    close(pipe_fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

static int run_simple_command(const char *working_dir, char *const argv[], dynamic_buffer *captured_output) {
    return run_and_stream(-1, working_dir, argv, 0, captured_output);
}

static int is_git_repo(const char *project_path) {
    dynamic_buffer output;
    buffer_init(&output);
    char *args[] = {"git", "-C", (char *) project_path, "rev-parse", "--is-inside-work-tree", NULL};
    int ok = run_simple_command(NULL, args, &output);
    int result = ok == 0 && strstr(output.data, "true") != NULL;
    free(output.data);
    return result;
}

static const char *resolve_codex_binary(void) {
    return "/home/linux/.vscode/extensions/openai.chatgpt-26.5401.11717-linux-x64/bin/linux-x86_64/codex";
}

static void handle_apply(int client_fd, const char *project_path, dynamic_buffer *output) {
    send_frame(client_fd, "STATUS", "Applying patch", strlen("Applying patch"));

    char *apply_args[] = {(char *) resolve_codex_binary(), "apply", NULL};
    int apply_ok = run_and_stream(client_fd, project_path, apply_args, 1, output);

    if (apply_ok == 0 && is_git_repo(project_path)) {
        dynamic_buffer git_output;
        buffer_init(&git_output);
        char *add_args[] = {"git", "-C", (char *) project_path, "add", "-A", "--", ".", ":(exclude).intellijPlatform", NULL};
        int add_ok = run_simple_command(NULL, add_args, &git_output);
        if (add_ok != 0 && git_output.length > 0) {
            send_frame(client_fd, "CHUNK", git_output.data, git_output.length);
        }
        free(git_output.data);
    }

    if (apply_ok != 0 && output->length == 0) {
        send_error(client_fd, "Failed to apply changes.");
        return;
    }

    if (output->length == 0) {
        const char *done = "Patch applied.";
        send_frame(client_fd, "DONE", done, strlen(done));
    } else {
        send_frame(client_fd, "DONE", "", 0);
    }
    send_header(client_fd, "END");
}

static void handle_rollback(int client_fd, const char *project_path) {
    if (!is_git_repo(project_path)) {
        send_error(client_fd, "Rollback requires a git repository.");
        return;
    }

    send_frame(client_fd, "STATUS", "Rolling back changes", strlen("Rolling back changes"));

    dynamic_buffer git_output;
    buffer_init(&git_output);
    char *restore_args[] = {"git", "-C", (char *) project_path, "restore", "--source=HEAD", "--staged", "--worktree", ".", NULL};
    char *clean_args[] = {"git", "-C", (char *) project_path, "clean", "-fd", NULL};
    int restore_ok = run_simple_command(NULL, restore_args, &git_output);
    int clean_ok = run_simple_command(NULL, clean_args, &git_output);

    if (restore_ok != 0 || clean_ok != 0) {
        if (git_output.length == 0) {
            buffer_append(&git_output, "Rollback failed.", strlen("Rollback failed."));
        }
        send_error(client_fd, git_output.data);
    } else {
        const char *done = "Workspace rolled back to HEAD.";
        send_frame(client_fd, "DONE", done, strlen(done));
        send_header(client_fd, "END");
    }

    free(git_output.data);
}

static void handle_exec(int client_fd, const char *project_path, const char *message, dynamic_buffer *output) {
    send_frame(client_fd, "STATUS", "Thinking", strlen("Thinking"));
    
    const char* codex_bin = resolve_codex_binary();
    
    printf("[DEBUG] Executing Codex at: %s\n", codex_bin);

    char *exec_args[] = {(char *) codex_bin, "exec", (char *) message, NULL};
    
    int exec_ok = run_and_stream(client_fd, project_path, exec_args, 1, output);
    
    if (exec_ok != 0 && output->length == 0) {
        send_error(client_fd, "Codex binary not found or execution failed. Check path!");
        return;
    }

    if (is_git_repo(project_path)) {
        send_frame(client_fd, "ACTIONS", "apply,rollback", 14);
    }
    send_frame(client_fd, "DONE", "", 0);
    send_header(client_fd, "END");
}

int main(void) {
    int server_fd;
    struct sockaddr_un addr;
    dynamic_buffer output;
    buffer_init(&output);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket error");
        exit(1);
    }

    unlink(SOCKET_PATH);
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1) {
        perror("Bind error");
        exit(1);
    }

    if (listen(server_fd, 8) != 0) {
        perror("Listen error");
        exit(1);
    }

    printf("AI Bridge Server Active on %s\n", SOCKET_PATH);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            continue;
        }

        char *project_path = read_named_payload(client_fd, "PATH");
        char *message = read_named_payload(client_fd, "MSG");
        int protocol_ok = read_end(client_fd);
        buffer_reset(&output);

        if (project_path == NULL || message == NULL || protocol_ok != 0) {
            send_error(client_fd, "Malformed request received by bridge server.");
            free(project_path);
            free(message);
            close(client_fd);
            continue;
        }

        if (project_path[0] == '\0' || access(project_path, F_OK) != 0) {
            send_error(client_fd, "Project path not found.");
            free(project_path);
            free(message);
            close(client_fd);
            continue;
        }

        if (strcmp(message, "CMD:APPLY") == 0) {
            handle_apply(client_fd, project_path, &output);
        } else if (strcmp(message, "CMD:ROLLBACK") == 0) {
            handle_rollback(client_fd, project_path);
        } else {
            handle_exec(client_fd, project_path, message, &output);
        }

        free(project_path);
        free(message);
        close(client_fd);
    }

    free(output.data);
    return 0;
}
