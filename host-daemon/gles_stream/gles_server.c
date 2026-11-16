/* language: C11, file: host-daemon/gles_stream/gles_server.c
 * Server-side GL command executor. Runs on a separate host-daemon thread.
 * Receives commands from /tmp/x11droid_gl.sock and executes them in the GLX context.
 *
 * Architecture notes:
 *  - The GLX context may only be current on one thread; every dispatch is wrapped
 *    in glx_lock()/glx_unlock(), sharing the mutex with the main composition path.
 *  - GLSL ES 1.00 shaders are lightly translated to desktop GLSL 1.20.
 *  - Client connections are persistent; the server handles many requests.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gles_server.h"
#include "../glx_render.h"          /* konteks GLX + glx_lock/glx_unlock */

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "gles_stream_proto.h"
#include "gl_decl.h"

/* ---------- Server state ---------- */
static int g_listen_fd = -1;
static pthread_t g_server_thread;
static bool g_server_running = false;
static pthread_mutex_t g_server_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Socket helpers ---------- */
static int create_server_socket(void) {
    unlink(GLES_STREAM_SOCK_PATH);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, GLES_STREAM_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr,
             offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path) + 1) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, MSG_WAITALL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int reply(int fd, const gles_stream_header_t *hdr, int32_t result,
                 const void *payload, uint32_t payload_size) {
    gles_stream_response_t resp = {
        .magic = GLES_STREAM_MAGIC,
        .version = GLES_STREAM_VERSION,
        .cmd = hdr->cmd,
        .seq = hdr->seq,
        .result = result,
        .payload_size = payload_size,
    };
    if (send_all(fd, &resp, sizeof(resp)) < 0)
        return -1;
    if (payload_size > 0 && payload)
        return send_all(fd, payload, payload_size);
    return 0;
}

static int reply_ok(int fd, const gles_stream_header_t *hdr) {
    return reply(fd, hdr, 0, NULL, 0);
}

static int reply_int(int fd, const gles_stream_header_t *hdr, int32_t value) {
    return reply(fd, hdr, 0, &value, sizeof(value));
}

/* ---------- GLSL ES 1.00 -> desktop GLSL 1.20 (subset) ---------- */

static bool line_starts_with(const char *line, size_t len, const char *prefix) {
    size_t i = 0;
    while (i < len && isspace((unsigned char)line[i]))
        i++;
    size_t plen = strlen(prefix);
    if (len - i < plen)
        return false;
    return strncmp(line + i, prefix, plen) == 0;
}

static void append_word_filtered(char *dst, size_t *pos, size_t cap, const char *line, size_t len) {
    size_t i = 0;
    while (i < len && (*pos + 1) < cap) {
        if ((i == 0 || isspace((unsigned char)line[i - 1]))) {
            static const char *quals[] = { "highp", "mediump", "lowp" };
            bool skipped = false;
            for (size_t q = 0; q < sizeof(quals) / sizeof(quals[0]); q++) {
                size_t ql = strlen(quals[q]);
                if (i + ql <= len && strncmp(line + i, quals[q], ql) == 0 &&
                    (i + ql == len || isspace((unsigned char)line[i + ql]))) {
                    i += ql;
                    while (i < len && isspace((unsigned char)line[i]))
                        i++;
                    skipped = true;
                    break;
                }
            }
            if (skipped)
                continue;
        }
        dst[(*pos)++] = line[i++];
    }
}

/* Light translation: strip #version/#extension OES/precision qualifiers.
 * Desktop GLSL 1.20 accepts attribute/varying/texture2D/gl_FragColor. */
static char *translate_shader_source(const char *src) {
    if (!src)
        return NULL;

    size_t src_len = strlen(src);
    size_t cap = src_len * 2 + 64;
    char *out = malloc(cap);
    if (!out)
        return NULL;

    size_t pos = 0;
    const char *prefix = "#version 120\n";
    size_t plen = strlen(prefix);
    memcpy(out, prefix, plen);
    pos = plen;

    const char *p = src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);

        bool skip = line_starts_with(p, len, "#version") ||
                    line_starts_with(p, len, "#extension GL_OES") ||
                    line_starts_with(p, len, "precision ");

        if (!skip) {
            append_word_filtered(out, &pos, cap, p, len);
            if ((pos + 1) < cap)
                out[pos++] = '\n';
        }

        if (!nl)
            break;
        p = nl + 1;
    }
    out[pos] = '\0';
    return out;
}

/* ---------- Command handlers ---------- */

static int handle_create_shader(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_create_shader_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    GLuint id = glCreateShader((GLenum)payload.type);
    return reply(fd, hdr, id ? 0 : -EINVAL, &id, sizeof(id));
}

static int handle_shader_source(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_shader_source_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.length == 0 || payload.length > 4 * 1024 * 1024)
        return reply(fd, hdr, -EINVAL, NULL, 0);

    char *raw = malloc(payload.length + 1);
    if (!raw)
        return -1;
    if (recv_all(fd, raw, payload.length) < 0) {
        free(raw);
        return -1;
    }
    raw[payload.length] = '\0';

    char *translated = translate_shader_source(raw);
    free(raw);
    if (!translated)
        return -1;

    const GLchar *strings[1] = { translated };
    GLint lengths[1] = { (GLint)strlen(translated) };
    glShaderSource(payload.shader, 1, strings, lengths);
    free(translated);
    return reply_ok(fd, hdr);
}

static int handle_compile_shader(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_compile_shader_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glCompileShader(payload.shader);
    return reply_ok(fd, hdr);
}

static int handle_get_shader_iv(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_get_shader_iv_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    GLint value = 0;
    glGetShaderiv(payload.shader, (GLenum)payload.pname, &value);
    return reply_int(fd, hdr, (int32_t)value);
}

static int handle_get_shader_info_log(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_get_shader_info_log_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    uint32_t cap = payload.max_length ? payload.max_length : 1024;
    if (cap > 65536)
        cap = 65536;
    char *buf = malloc(cap);
    if (!buf)
        return -1;
    buf[0] = '\0';
    GLsizei len = 0;
    glGetShaderInfoLog(payload.shader, (GLsizei)cap, &len, buf);
    if (len < 0)
        len = 0;
    if ((uint32_t)len > cap)
        len = (GLsizei)cap;
    int rc = reply(fd, hdr, 0, buf, (uint32_t)len);
    free(buf);
    return rc;
}

static int handle_delete_shader(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_delete_shader_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glDeleteShader(payload.shader);
    return reply_ok(fd, hdr);
}

static int handle_create_program(int fd, const gles_stream_header_t *hdr) {
    GLuint id = glCreateProgram();
    return reply(fd, hdr, id ? 0 : -EINVAL, &id, sizeof(id));
}

static int handle_attach_shader(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_attach_shader_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glAttachShader(payload.program, payload.shader);
    return reply_ok(fd, hdr);
}

static int handle_link_program(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_link_program_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glLinkProgram(payload.program);
    return reply_ok(fd, hdr);
}

static int handle_get_program_iv(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_get_program_iv_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    GLint value = 0;
    glGetProgramiv(payload.program, (GLenum)payload.pname, &value);
    return reply_int(fd, hdr, (int32_t)value);
}

static int handle_get_program_info_log(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_get_program_info_log_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    uint32_t cap = payload.max_length ? payload.max_length : 1024;
    if (cap > 65536)
        cap = 65536;
    char *buf = malloc(cap);
    if (!buf)
        return -1;
    buf[0] = '\0';
    GLsizei len = 0;
    glGetProgramInfoLog(payload.program, (GLsizei)cap, &len, buf);
    if (len < 0)
        len = 0;
    if ((uint32_t)len > cap)
        len = (GLsizei)cap;
    int rc = reply(fd, hdr, 0, buf, (uint32_t)len);
    free(buf);
    return rc;
}

static int handle_use_program(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_use_program_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glUseProgram(payload.program);
    return reply_ok(fd, hdr);
}

static int handle_delete_program(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_delete_program_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glDeleteProgram(payload.program);
    return reply_ok(fd, hdr);
}

/* Create + compile + link in a single roundtrip. */
static int handle_create_shader_program_v2(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_create_shader_program_v2_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.vs_len == 0 || payload.fs_len == 0 ||
        payload.vs_len > 4 * 1024 * 1024 || payload.fs_len > 4 * 1024 * 1024)
        return reply(fd, hdr, -EINVAL, NULL, 0);

    char *vs = malloc(payload.vs_len + 1);
    char *fs = malloc(payload.fs_len + 1);
    if (!vs || !fs) {
        free(vs);
        free(fs);
        return -1;
    }
    if (recv_all(fd, vs, payload.vs_len) < 0 || recv_all(fd, fs, payload.fs_len) < 0) {
        free(vs);
        free(fs);
        return -1;
    }
    vs[payload.vs_len] = '\0';
    fs[payload.fs_len] = '\0';

    char *vst = translate_shader_source(vs);
    char *fst = translate_shader_source(fs);
    free(vs);
    free(fs);
    if (!vst || !fst) {
        free(vst);
        free(fst);
        return -1;
    }

    GLuint vsh = glCreateShader(GL_VERTEX_SHADER);
    GLuint fsh = glCreateShader(GL_FRAGMENT_SHADER);
    GLuint prog = glCreateProgram();
    int result = 0;

    if (!vsh || !fsh || !prog) {
        result = -EINVAL;
    } else {
        const GLchar *vsrc[1] = { vst };
        const GLchar *fsrc[1] = { fst };
        GLint vlen[1] = { (GLint)strlen(vst) };
        GLint flen[1] = { (GLint)strlen(fst) };
        glShaderSource(vsh, 1, vsrc, vlen);
        glShaderSource(fsh, 1, fsrc, flen);
        glCompileShader(vsh);
        glCompileShader(fsh);

        GLint ok = 0;
        glGetShaderiv(vsh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            result = -EINVAL;
        } else {
            glGetShaderiv(fsh, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                result = -EINVAL;
            } else {
                glAttachShader(prog, vsh);
                glAttachShader(prog, fsh);
                glLinkProgram(prog);
                glGetProgramiv(prog, GL_LINK_STATUS, &ok);
                if (!ok)
                    result = -EINVAL;
            }
        }
    }

    if (result != 0) {
        if (prog) glDeleteProgram(prog);
        if (vsh) glDeleteShader(vsh);
        if (fsh) glDeleteShader(fsh);
    }
    free(vst);
    free(fst);

    if (result != 0)
        return reply(fd, hdr, result, NULL, 0);
    return reply(fd, hdr, 0, &prog, sizeof(prog));
}

/* Buffers */
static int handle_gen_buffers(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_gen_buffers_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.n == 0 || payload.n > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    GLuint *ids = calloc(payload.n, sizeof(GLuint));
    if (!ids)
        return -1;
    glGenBuffers((GLsizei)payload.n, ids);
    uint32_t total = sizeof(uint32_t) + payload.n * sizeof(GLuint);
    uint8_t *out = malloc(total);
    if (!out) {
        free(ids);
        return -1;
    }
    memcpy(out, &payload.n, sizeof(uint32_t));
    memcpy(out + sizeof(uint32_t), ids, payload.n * sizeof(GLuint));
    int rc = reply(fd, hdr, 0, out, total);
    free(out);
    free(ids);
    return rc;
}

static int handle_bind_buffer(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_bind_buffer_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glBindBuffer((GLenum)payload.target, payload.buffer);
    return reply_ok(fd, hdr);
}

static int handle_buffer_data(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_buffer_data_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.size < 0 || payload.size > (int64_t)(256 * 1024 * 1024))
        return reply(fd, hdr, -EINVAL, NULL, 0);

    void *data = NULL;
    if (payload.size > 0) {
        data = malloc((size_t)payload.size);
        if (!data)
            return -1;
        if (recv_all(fd, data, (size_t)payload.size) < 0) {
            free(data);
            return -1;
        }
    }
    glBufferData((GLenum)payload.target, (GLsizeiptr)payload.size, data, (GLenum)payload.usage);
    free(data);
    return reply_ok(fd, hdr);
}

static int handle_buffer_sub_data(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_buffer_sub_data_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.size < 0 || payload.size > (int64_t)(256 * 1024 * 1024))
        return reply(fd, hdr, -EINVAL, NULL, 0);

    void *data = NULL;
    if (payload.size > 0) {
        data = malloc((size_t)payload.size);
        if (!data)
            return -1;
        if (recv_all(fd, data, (size_t)payload.size) < 0) {
            free(data);
            return -1;
        }
    }
    glBufferSubData((GLenum)payload.target, (GLintptr)payload.offset,
                    (GLsizeiptr)payload.size, data);
    free(data);
    return reply_ok(fd, hdr);
}

static int handle_delete_buffers(int fd, const gles_stream_header_t *hdr) {
    uint32_t n = 0;
    if (recv_all(fd, &n, sizeof(n)) < 0)
        return -1;
    if (n > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    GLuint *ids = malloc(n * sizeof(GLuint));
    if (!ids)
        return -1;
    if (recv_all(fd, ids, n * sizeof(GLuint)) < 0) {
        free(ids);
        return -1;
    }
    glDeleteBuffers((GLsizei)n, ids);
    free(ids);
    return reply_ok(fd, hdr);
}

/* Vertex attributes */
static int handle_vertex_attribf(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_vertex_attrib_f_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glVertexAttrib1f(payload.index, payload.x);
    return reply_ok(fd, hdr);
}

static int handle_vertex_attribfv(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_vertex_attrib_fv_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glVertexAttrib1fv(payload.index, payload.v);
    return reply_ok(fd, hdr);
}

static int handle_vertex_attrib_pointer(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_vertex_attrib_pointer_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glVertexAttribPointer(payload.index, payload.size, (GLenum)payload.type,
                          (GLboolean)payload.normalized, payload.stride,
                          (const void *)(intptr_t)payload.offset);
    return reply_ok(fd, hdr);
}

static int handle_enable_vertex_attrib_array(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_enable_vertex_attrib_array_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glEnableVertexAttribArray(payload.index);
    return reply_ok(fd, hdr);
}

static int handle_disable_vertex_attrib_array(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_disable_vertex_attrib_array_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glDisableVertexAttribArray(payload.index);
    return reply_ok(fd, hdr);
}

/* Textures */
static int handle_gen_textures(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_gen_textures_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.n == 0 || payload.n > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    GLuint *ids = calloc(payload.n, sizeof(GLuint));
    if (!ids)
        return -1;
    glGenTextures((GLsizei)payload.n, ids);
    uint32_t total = sizeof(uint32_t) + payload.n * sizeof(GLuint);
    uint8_t *out = malloc(total);
    if (!out) {
        free(ids);
        return -1;
    }
    memcpy(out, &payload.n, sizeof(uint32_t));
    memcpy(out + sizeof(uint32_t), ids, payload.n * sizeof(GLuint));
    int rc = reply(fd, hdr, 0, out, total);
    free(out);
    free(ids);
    return rc;
}

static int handle_bind_texture(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_bind_texture_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glBindTexture((GLenum)payload.target, payload.texture);
    return reply_ok(fd, hdr);
}

static uint32_t texture_data_size(int32_t w, int32_t h, uint32_t format, uint32_t type) {
    if (w <= 0 || h <= 0)
        return 0;
    uint32_t bpp = 4;
    if (type == GL_UNSIGNED_BYTE) {
        switch (format) {
        case GL_RGBA:
        case GL_BGRA:
            bpp = 4;
            break;
        case GL_RGB:
            bpp = 3;
            break;
        case GL_RG:
            bpp = 2;
            break;
        case GL_RED:
            bpp = 1;
            break;
        default:
            bpp = 4;
            break;
        }
    } else if (type == GL_UNSIGNED_SHORT) {
        bpp = 8; /* RGBA16 */
    } else if (type == GL_FLOAT) {
        bpp = 16; /* RGBA32F */
    } else {
        bpp = 4;
    }
    uint64_t total = (uint64_t)w * (uint64_t)h * bpp;
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

static int handle_tex_image_2d(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_tex_image_2d_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    void *data = NULL;
    if (payload.size > 0) {
        if (payload.size > (int64_t)(512 * 1024 * 1024))
            return reply(fd, hdr, -EINVAL, NULL, 0);
        data = malloc((size_t)payload.size);
        if (!data)
            return -1;
        if (recv_all(fd, data, (size_t)payload.size) < 0) {
            free(data);
            return -1;
        }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D((GLenum)payload.target, payload.level, payload.internalformat,
                 payload.width, payload.height, payload.border,
                 (GLenum)payload.format, (GLenum)payload.type, data);
    free(data);
    return reply_ok(fd, hdr);
}

static int handle_tex_sub_image_2d(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_tex_sub_image_2d_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    void *data = NULL;
    if (payload.size > 0) {
        if (payload.size > (int64_t)(512 * 1024 * 1024))
            return reply(fd, hdr, -EINVAL, NULL, 0);
        data = malloc((size_t)payload.size);
        if (!data)
            return -1;
        if (recv_all(fd, data, (size_t)payload.size) < 0) {
            free(data);
            return -1;
        }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D((GLenum)payload.target, payload.level, payload.xoffset, payload.yoffset,
                    payload.width, payload.height, (GLenum)payload.format,
                    (GLenum)payload.type, data);
    free(data);
    return reply_ok(fd, hdr);
}

static int handle_tex_parameteri(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_tex_parameteri_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glTexParameteri((GLenum)payload.target, (GLenum)payload.pname, payload.param);
    return reply_ok(fd, hdr);
}

static int handle_tex_parameterf(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_tex_parameterf_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glTexParameterf((GLenum)payload.target, (GLenum)payload.pname, payload.param);
    return reply_ok(fd, hdr);
}

static int handle_delete_textures(int fd, const gles_stream_header_t *hdr) {
    uint32_t n = 0;
    if (recv_all(fd, &n, sizeof(n)) < 0)
        return -1;
    if (n > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    GLuint *ids = malloc(n * sizeof(GLuint));
    if (!ids)
        return -1;
    if (recv_all(fd, ids, n * sizeof(GLuint)) < 0) {
        free(ids);
        return -1;
    }
    glDeleteTextures((GLsizei)n, ids);
    free(ids);
    return reply_ok(fd, hdr);
}

static int handle_active_texture(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_active_texture_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glActiveTexture((GLenum)payload.texture);
    return reply_ok(fd, hdr);
}

static int handle_generate_mipmap(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_generate_mipmap_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glGenerateMipmap((GLenum)payload.target);
    return reply_ok(fd, hdr);
}

/* Uniforms */
static int handle_get_uniform_location(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_get_uniform_location_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.name_len > 4096)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    char *name = malloc(payload.name_len + 1);
    if (!name)
        return -1;
    if (recv_all(fd, name, payload.name_len) < 0) {
        free(name);
        return -1;
    }
    name[payload.name_len] = '\0';
    GLint loc = glGetUniformLocation(payload.program, name);
    free(name);
    return reply_int(fd, hdr, (int32_t)loc);
}

static int handle_uniform_1f(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_uniform_1f_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glUniform1f((GLint)payload.location, payload.v);
    return reply_ok(fd, hdr);
}

static int handle_uniform_1fv(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_uniform_1fv_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.count <= 0 || payload.count > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    size_t bytes = (size_t)payload.count * sizeof(GLfloat);
    GLfloat *v = malloc(bytes);
    if (!v)
        return -1;
    if (recv_all(fd, v, bytes) < 0) {
        free(v);
        return -1;
    }
    glUniform1fv((GLint)payload.location, payload.count, v);
    free(v);
    return reply_ok(fd, hdr);
}

static int handle_uniform_2fv(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_uniform_2fv_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.count <= 0 || payload.count > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    size_t bytes = (size_t)payload.count * 2 * sizeof(GLfloat);
    GLfloat *v = malloc(bytes);
    if (!v)
        return -1;
    if (recv_all(fd, v, bytes) < 0) {
        free(v);
        return -1;
    }
    glUniform2fv((GLint)payload.location, payload.count, v);
    free(v);
    return reply_ok(fd, hdr);
}

static int handle_uniform_3fv(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_uniform_3fv_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.count <= 0 || payload.count > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    size_t bytes = (size_t)payload.count * 3 * sizeof(GLfloat);
    GLfloat *v = malloc(bytes);
    if (!v)
        return -1;
    if (recv_all(fd, v, bytes) < 0) {
        free(v);
        return -1;
    }
    glUniform3fv((GLint)payload.location, payload.count, v);
    free(v);
    return reply_ok(fd, hdr);
}

static int handle_uniform_4fv(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_uniform_4fv_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.count <= 0 || payload.count > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    size_t bytes = (size_t)payload.count * 4 * sizeof(GLfloat);
    GLfloat *v = malloc(bytes);
    if (!v)
        return -1;
    if (recv_all(fd, v, bytes) < 0) {
        free(v);
        return -1;
    }
    glUniform4fv((GLint)payload.location, payload.count, v);
    free(v);
    return reply_ok(fd, hdr);
}

static int handle_uniform_1i(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_uniform_1i_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glUniform1i((GLint)payload.location, payload.v);
    return reply_ok(fd, hdr);
}

static int handle_uniform_1iv(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_uniform_1iv_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.count <= 0 || payload.count > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    size_t bytes = (size_t)payload.count * sizeof(GLint);
    GLint *v = malloc(bytes);
    if (!v)
        return -1;
    if (recv_all(fd, v, bytes) < 0) {
        free(v);
        return -1;
    }
    glUniform1iv((GLint)payload.location, payload.count, v);
    free(v);
    return reply_ok(fd, hdr);
}

static int handle_uniform_matrix_2fv(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_uniform_matrix_2fv_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.count <= 0 || payload.count > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    size_t bytes = (size_t)payload.count * 4 * sizeof(GLfloat);
    GLfloat *v = malloc(bytes);
    if (!v)
        return -1;
    if (recv_all(fd, v, bytes) < 0) {
        free(v);
        return -1;
    }
    glUniformMatrix2fv((GLint)payload.location, payload.count,
                       (GLboolean)payload.transpose, v);
    free(v);
    return reply_ok(fd, hdr);
}

static int handle_uniform_matrix_3fv(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_uniform_matrix_3fv_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.count <= 0 || payload.count > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    size_t bytes = (size_t)payload.count * 9 * sizeof(GLfloat);
    GLfloat *v = malloc(bytes);
    if (!v)
        return -1;
    if (recv_all(fd, v, bytes) < 0) {
        free(v);
        return -1;
    }
    glUniformMatrix3fv((GLint)payload.location, payload.count,
                       (GLboolean)payload.transpose, v);
    free(v);
    return reply_ok(fd, hdr);
}

static int handle_uniform_matrix_4fv(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_uniform_matrix_4fv_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    if (payload.count <= 0 || payload.count > 65536)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    size_t bytes = (size_t)payload.count * 16 * sizeof(GLfloat);
    GLfloat *v = malloc(bytes);
    if (!v)
        return -1;
    if (recv_all(fd, v, bytes) < 0) {
        free(v);
        return -1;
    }
    glUniformMatrix4fv((GLint)payload.location, payload.count,
                       (GLboolean)payload.transpose, v);
    free(v);
    return reply_ok(fd, hdr);
}

/* State / draw */
static int handle_viewport(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_viewport_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glViewport(payload.x, payload.y, payload.w, payload.h);
    return reply_ok(fd, hdr);
}

static int handle_clear_color(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_clear_color_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glClearColor(payload.r, payload.g, payload.b, payload.a);
    return reply_ok(fd, hdr);
}

static int handle_clear(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_clear_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glClear((GLbitfield)payload.mask);
    return reply_ok(fd, hdr);
}

static int handle_draw_arrays(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_draw_arrays_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glDrawArrays((GLenum)payload.mode, payload.first, payload.count);
    return reply_ok(fd, hdr);
}

static int handle_draw_elements(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_draw_elements_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glDrawElements((GLenum)payload.mode, payload.count, (GLenum)payload.type,
                   (const void *)(intptr_t)payload.indices_offset);
    return reply_ok(fd, hdr);
}

static int handle_enable(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_enable_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glEnable((GLenum)payload.cap);
    return reply_ok(fd, hdr);
}

static int handle_disable(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_disable_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glDisable((GLenum)payload.cap);
    return reply_ok(fd, hdr);
}

static int handle_blend_func(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_blend_func_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glBlendFunc((GLenum)payload.sfactor, (GLenum)payload.dfactor);
    return reply_ok(fd, hdr);
}

static int handle_blend_func_separate(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_blend_func_separate_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glBlendFuncSeparate((GLenum)payload.sfactor_rgb, (GLenum)payload.dfactor_rgb,
                        (GLenum)payload.sfactor_alpha, (GLenum)payload.dfactor_alpha);
    return reply_ok(fd, hdr);
}

static int handle_depth_func(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_depth_func_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glDepthFunc((GLenum)payload.func);
    return reply_ok(fd, hdr);
}

static int handle_depth_mask(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_depth_mask_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glDepthMask((GLboolean)payload.flag);
    return reply_ok(fd, hdr);
}

static int handle_cull_face(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_cull_face_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glCullFace((GLenum)payload.mode);
    return reply_ok(fd, hdr);
}

static int handle_front_face(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_front_face_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glFrontFace((GLenum)payload.mode);
    return reply_ok(fd, hdr);
}

static int handle_scissor(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_scissor_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    glScissor(payload.x, payload.y, payload.w, payload.h);
    return reply_ok(fd, hdr);
}

/* Readback / misc */
static int handle_read_pixels(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_read_pixels_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    uint32_t size = texture_data_size(payload.w, payload.h, payload.format, payload.type);
    if (size == 0 || size > 512u * 1024u * 1024u)
        return reply(fd, hdr, -EINVAL, NULL, 0);
    void *buf = malloc(size);
    if (!buf)
        return -1;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(payload.x, payload.y, payload.w, payload.h,
                 (GLenum)payload.format, (GLenum)payload.type, buf);
    int rc = reply(fd, hdr, 0, buf, size);
    free(buf);
    return rc;
}

static int handle_get_error(int fd, const gles_stream_header_t *hdr) {
    GLenum err = glGetError();
    return reply_int(fd, hdr, (int32_t)err);
}

static int handle_get_string(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_get_string_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    const GLubyte *str = glGetString((GLenum)payload.name);
    if (!str)
        return reply(fd, hdr, 0, NULL, 0);
    size_t len = strlen((const char *)str);
    if (len > 8191)
        len = 8191;
    return reply(fd, hdr, 0, str, (uint32_t)len);
}

static int handle_flush(int fd, const gles_stream_header_t *hdr) {
    glFlush();
    return reply_ok(fd, hdr);
}

static int handle_finish(int fd, const gles_stream_header_t *hdr) {
    glFinish();
    return reply_ok(fd, hdr);
}

/* Map/unmap is not used by the client yet; reply OK for compatibility. */
static int handle_map_resource(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_map_resource_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    return reply(fd, hdr, 0, &payload.host_id, sizeof(payload.host_id));
}

static int handle_unmap_resource(int fd, const gles_stream_header_t *hdr) {
    gles_cmd_unmap_resource_t payload;
    if (recv_all(fd, &payload, sizeof(payload)) < 0)
        return -1;
    return reply_ok(fd, hdr);
}

/* ---------- Dispatch ---------- */
typedef int (*handler_fn)(int fd, const gles_stream_header_t *);

static handler_fn g_handlers[GLES_CMD_MAX] = {
    [GLES_CMD_CREATE_SHADER]              = handle_create_shader,
    [GLES_CMD_SHADER_SOURCE]              = handle_shader_source,
    [GLES_CMD_COMPILE_SHADER]             = handle_compile_shader,
    [GLES_CMD_GET_SHADER_IV]              = handle_get_shader_iv,
    [GLES_CMD_GET_SHADER_INFO_LOG]        = handle_get_shader_info_log,
    [GLES_CMD_DELETE_SHADER]              = handle_delete_shader,

    [GLES_CMD_CREATE_PROGRAM]             = handle_create_program,
    [GLES_CMD_ATTACH_SHADER]              = handle_attach_shader,
    [GLES_CMD_LINK_PROGRAM]               = handle_link_program,
    [GLES_CMD_GET_PROGRAM_IV]             = handle_get_program_iv,
    [GLES_CMD_GET_PROGRAM_INFO_LOG]       = handle_get_program_info_log,
    [GLES_CMD_USE_PROGRAM]                = handle_use_program,
    [GLES_CMD_DELETE_PROGRAM]             = handle_delete_program,
    [GLES_CMD_CREATE_SHADER_PROGRAM_V2]   = handle_create_shader_program_v2,

    [GLES_CMD_GEN_BUFFERS]                = handle_gen_buffers,
    [GLES_CMD_BIND_BUFFER]                = handle_bind_buffer,
    [GLES_CMD_BUFFER_DATA]                = handle_buffer_data,
    [GLES_CMD_BUFFER_SUB_DATA]            = handle_buffer_sub_data,
    [GLES_CMD_DELETE_BUFFERS]             = handle_delete_buffers,

    [GLES_CMD_VERTEX_ATTRIBF]             = handle_vertex_attribf,
    [GLES_CMD_VERTEX_ATTRIBFV]            = handle_vertex_attribfv,
    [GLES_CMD_VERTEX_ATTRIB_POINTER]      = handle_vertex_attrib_pointer,
    [GLES_CMD_ENABLE_VERTEX_ATTRIB_ARRAY] = handle_enable_vertex_attrib_array,
    [GLES_CMD_DISABLE_VERTEX_ATTRIB_ARRAY]= handle_disable_vertex_attrib_array,

    [GLES_CMD_GEN_TEXTURES]               = handle_gen_textures,
    [GLES_CMD_BIND_TEXTURE]               = handle_bind_texture,
    [GLES_CMD_TEX_IMAGE_2D]               = handle_tex_image_2d,
    [GLES_CMD_TEX_SUB_IMAGE_2D]           = handle_tex_sub_image_2d,
    [GLES_CMD_TEX_PARAMETERI]             = handle_tex_parameteri,
    [GLES_CMD_TEX_PARAMETERF]             = handle_tex_parameterf,
    [GLES_CMD_DELETE_TEXTURES]            = handle_delete_textures,
    [GLES_CMD_ACTIVE_TEXTURE]             = handle_active_texture,
    [GLES_CMD_GENERATE_MIPMAP]            = handle_generate_mipmap,

    [GLES_CMD_GET_UNIFORM_LOCATION]       = handle_get_uniform_location,
    [GLES_CMD_UNIFORM_1F]                 = handle_uniform_1f,
    [GLES_CMD_UNIFORM_1FV]                = handle_uniform_1fv,
    [GLES_CMD_UNIFORM_2FV]                = handle_uniform_2fv,
    [GLES_CMD_UNIFORM_3FV]                = handle_uniform_3fv,
    [GLES_CMD_UNIFORM_4FV]                = handle_uniform_4fv,
    [GLES_CMD_UNIFORM_1I]                 = handle_uniform_1i,
    [GLES_CMD_UNIFORM_1IV]                = handle_uniform_1iv,
    [GLES_CMD_UNIFORM_MATRIX_2FV]         = handle_uniform_matrix_2fv,
    [GLES_CMD_UNIFORM_MATRIX_3FV]         = handle_uniform_matrix_3fv,
    [GLES_CMD_UNIFORM_MATRIX_4FV]         = handle_uniform_matrix_4fv,

    [GLES_CMD_VIEWPORT]                   = handle_viewport,
    [GLES_CMD_CLEAR_COLOR]                = handle_clear_color,
    [GLES_CMD_CLEAR]                      = handle_clear,
    [GLES_CMD_DRAW_ARRAYS]                = handle_draw_arrays,
    [GLES_CMD_DRAW_ELEMENTS]              = handle_draw_elements,
    [GLES_CMD_ENABLE]                     = handle_enable,
    [GLES_CMD_DISABLE]                    = handle_disable,
    [GLES_CMD_BLEND_FUNC]                 = handle_blend_func,
    [GLES_CMD_BLEND_FUNC_SEPARATE]        = handle_blend_func_separate,
    [GLES_CMD_DEPTH_FUNC]                 = handle_depth_func,
    [GLES_CMD_DEPTH_MASK]                 = handle_depth_mask,
    [GLES_CMD_CULL_FACE]                  = handle_cull_face,
    [GLES_CMD_FRONT_FACE]                 = handle_front_face,
    [GLES_CMD_SCISSOR]                    = handle_scissor,

    [GLES_CMD_READ_PIXELS]                = handle_read_pixels,
    [GLES_CMD_GET_ERROR]                  = handle_get_error,
    [GLES_CMD_GET_STRING]                 = handle_get_string,
    [GLES_CMD_FLUSH]                      = handle_flush,
    [GLES_CMD_FINISH]                     = handle_finish,

    [GLES_CMD_MAP_RESOURCE]               = handle_map_resource,
    [GLES_CMD_UNMAP_RESOURCE]             = handle_unmap_resource,
};

/* ---------- Server thread ---------- */
static void *server_thread(void *arg) {
    (void)arg;

    g_listen_fd = create_server_socket();
    if (g_listen_fd < 0) {
        fprintf(stderr, "[gles_server] failed to create socket: %s\n", strerror(errno));
        return NULL;
    }

    printf("[gles_server] listening on %s\n", GLES_STREAM_SOCK_PATH);

    while (true) {
        pthread_mutex_lock(&g_server_lock);
        bool running = g_server_running;
        pthread_mutex_unlock(&g_server_lock);
        if (!running)
            break;

        int client_fd = accept4(g_listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED)
                continue;
            break;
        }

        /* Timeout so a stalled client cannot freeze composition. */
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        /* Persistent connection: serve many requests until the client disconnects. */
        gles_stream_header_t hdr;
        while (recv_all(client_fd, &hdr, sizeof(hdr)) == 0) {
            if (hdr.magic != GLES_STREAM_MAGIC || hdr.version != GLES_STREAM_VERSION ||
                hdr.cmd >= GLES_CMD_MAX || !g_handlers[hdr.cmd]) {
                reply(client_fd, &hdr, -EINVAL, NULL, 0);
                continue;
            }

            /* The GLX context may only be current on one thread; serialize with
             * the main composition thread through glx_lock/glx_unlock. */
            glx_lock();
            int rc = g_handlers[hdr.cmd](client_fd, &hdr);
            glx_unlock();
            if (rc < 0)
                break;
        }
        close(client_fd);
    }

    close(g_listen_fd);
    g_listen_fd = -1;
    printf("[gles_server] stopped\n");
    return NULL;
}

/* ---------- Public API ---------- */
bool gles_server_init(void) {
    pthread_mutex_lock(&g_server_lock);
    if (g_server_running) {
        pthread_mutex_unlock(&g_server_lock);
        return true;
    }
    g_server_running = true;
    pthread_mutex_unlock(&g_server_lock);

    if (pthread_create(&g_server_thread, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "[gles_server] failed to create thread: %s\n", strerror(errno));
        g_server_running = false;
        return false;
    }
    return true;
}

void gles_server_stop(void) {
    pthread_mutex_lock(&g_server_lock);
    if (!g_server_running) {
        pthread_mutex_unlock(&g_server_lock);
        return;
    }
    g_server_running = false;
    pthread_mutex_unlock(&g_server_lock);

    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    pthread_join(g_server_thread, NULL);
}

bool gles_server_running(void) {
    pthread_mutex_lock(&g_server_lock);
    bool r = g_server_running;
    pthread_mutex_unlock(&g_server_lock);
    return r;
}
