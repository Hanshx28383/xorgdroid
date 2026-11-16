/* language: C11, file: android/gles_stream/src/gles_client.c
 * Client-side GLES 2.0 subset implementation that forwards calls
 * to the host daemon over /tmp/x11droid_gl.sock.
 * Built as libGLES_x11droid.so (Android NDK).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gles_stream_proto.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_RG
#define GL_RG 0x8227
#endif

/* ---------- Client state ---------- */
static int g_sock_fd = -1;
static uint32_t g_seq = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_connected = false;

/* Mapping Android resource ID -> Host resource ID */
#define MAX_RESOURCES 4096
static uint32_t g_res_map[MAX_RESOURCES];
static pthread_mutex_t g_res_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Socket helpers ---------- */
static int connect_to_host(void) {
    if (g_connected) return 0;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, GLES_STREAM_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr,
                offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path) + 1) < 0) {
        close(fd);
        return -1;
    }

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    g_sock_fd = fd;
    g_connected = true;
    return 0;
}

static void disconnect_host(void) {
    if (g_sock_fd >= 0) {
        close(g_sock_fd);
        g_sock_fd = -1;
    }
    g_connected = false;
}

static uint32_t alloc_android_id(uint32_t host_id) {
    pthread_mutex_lock(&g_res_lock);
    for (uint32_t i = 1; i < MAX_RESOURCES; ++i) {
        if (g_res_map[i] == 0) {
            g_res_map[i] = host_id;
            pthread_mutex_unlock(&g_res_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_res_lock);
    return 0;
}

static uint32_t get_host_id(uint32_t android_id) {
    if (android_id == 0 || android_id >= MAX_RESOURCES) return 0;
    pthread_mutex_lock(&g_res_lock);
    uint32_t h = g_res_map[android_id];
    pthread_mutex_unlock(&g_res_lock);
    return h;
}

static void free_android_id(uint32_t android_id) {
    if (android_id == 0 || android_id >= MAX_RESOURCES) return;
    pthread_mutex_lock(&g_res_lock);
    g_res_map[android_id] = 0;
    pthread_mutex_unlock(&g_res_lock);
}

/* ---------- Low-level send/recv ---------- */
static int send_request_ex(uint16_t cmd, const void *payload, size_t payload_sz,
                           const void *extra, size_t extra_sz,
                           gles_stream_response_t *resp_out, void *resp_payload, size_t *resp_payload_sz) {
    pthread_mutex_lock(&g_lock);

    if (connect_to_host() < 0) {
        pthread_mutex_unlock(&g_lock);
        return -ENOTCONN;
    }

    gles_stream_header_t hdr = {
        .magic = GLES_STREAM_MAGIC,
        .version = GLES_STREAM_VERSION,
        .cmd = cmd,
        .seq = g_seq++,
        .payload_size = (uint32_t)payload_sz,
    };

    if (gles_send_all(g_sock_fd, &hdr, sizeof(hdr)) < 0) {
        disconnect_host();
        pthread_mutex_unlock(&g_lock);
        return -EPIPE;
    }
    if (payload_sz > 0 && gles_send_all(g_sock_fd, payload, payload_sz) < 0) {
        disconnect_host();
        pthread_mutex_unlock(&g_lock);
        return -EPIPE;
    }
    if (extra_sz > 0 && extra && gles_send_all(g_sock_fd, extra, extra_sz) < 0) {
        disconnect_host();
        pthread_mutex_unlock(&g_lock);
        return -EPIPE;
    }

    /* Read the response header */
    gles_stream_response_t resp;
    if (gles_recv_all(g_sock_fd, &resp, sizeof(resp)) < 0) {
        disconnect_host();
        pthread_mutex_unlock(&g_lock);
        return -EPIPE;
    }

    if (resp.magic != GLES_STREAM_MAGIC || resp.version != GLES_STREAM_VERSION ||
        resp.seq != hdr.seq || resp.cmd != cmd) {
        pthread_mutex_unlock(&g_lock);
        return -EPROTO;
    }

    if (resp_payload && resp.payload_size > 0) {
        if (resp.payload_size > *resp_payload_sz) {
            pthread_mutex_unlock(&g_lock);
            return -ENOBUFS;
        }
        if (gles_recv_all(g_sock_fd, resp_payload, resp.payload_size) < 0) {
            disconnect_host();
            pthread_mutex_unlock(&g_lock);
            return -EPIPE;
        }
        *resp_payload_sz = resp.payload_size;
    } else {
        if (resp_payload_sz) *resp_payload_sz = 0;
    }

    if (resp_out) *resp_out = resp;

    int ret = resp.result;
    pthread_mutex_unlock(&g_lock);
    return ret;
}

static int send_request(uint16_t cmd, const void *payload, size_t payload_sz,
                        gles_stream_response_t *resp_out, void *resp_payload, size_t *resp_payload_sz) {
    return send_request_ex(cmd, payload, payload_sz, NULL, 0,
                           resp_out, resp_payload, resp_payload_sz);
}

/* ---------- Resource mapping wrappers ---------- */
static uint32_t map_host_id(uint32_t host_id) {
    uint32_t aid = alloc_android_id(host_id);
    return aid ? aid : host_id;  /* fall back to host_id when the mapping is full */
}

static uint32_t unmap_android_id(uint32_t android_id) {
    uint32_t hid = get_host_id(android_id);
    free_android_id(android_id);
    return hid;
}

/* ---------- GLES 2.0 API Implementation ---------- */

/* Shader */
GLuint glCreateShader(GLenum type) {
    gles_cmd_create_shader_t payload = { .type = (uint32_t)type };
    gles_stream_response_t resp;
    gles_resp_id_t resp_payload;
    size_t resp_sz = sizeof(resp_payload);
    int ret = send_request(GLES_CMD_CREATE_SHADER, &payload, sizeof(payload), &resp, &resp_payload, &resp_sz);
    if (ret < 0) return 0;
    return map_host_id(resp_payload.id);
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
    uint32_t hshader = get_host_id(shader);
    if (!hshader) return;

    /* Concatenate the strings */
    size_t total_len = 0;
    for (int i = 0; i < count; ++i) {
        const GLint n = length ? length[i] : -1;
        total_len += (n >= 0) ? (size_t)n : strlen(string[i]);
    }

    char *buf = malloc(total_len + 1);
    if (!buf) return;
    size_t off = 0;
    for (int i = 0; i < count; ++i) {
        const GLint n = length ? length[i] : -1;
        const size_t l = (n >= 0) ? (size_t)n : strlen(string[i]);
        memcpy(buf + off, string[i], l);
        off += l;
    }
    buf[off] = '\0';

    gles_cmd_shader_source_t payload = {
        .shader = hshader,
        .count = (uint32_t)count,
        .length = (uint32_t)total_len,
    };
    send_request_ex(GLES_CMD_SHADER_SOURCE, &payload, sizeof(payload),
                    total_len ? buf : NULL, total_len, NULL, NULL, NULL);
    free(buf);
}

void glCompileShader(GLuint shader) {
    uint32_t hshader = get_host_id(shader);
    if (!hshader) return;
    gles_cmd_compile_shader_t payload = { .shader = get_host_id(shader) };
    send_request(GLES_CMD_COMPILE_SHADER, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    uint32_t hshader = get_host_id(shader);
    if (!hshader || !params) return;
    gles_cmd_get_shader_iv_t payload = { .shader = hshader, .pname = (uint32_t)pname };
    gles_stream_response_t resp;
    gles_resp_int_t resp_payload;
    size_t resp_sz = sizeof(resp_payload);
    int ret = send_request(GLES_CMD_GET_SHADER_IV, &payload, sizeof(payload), &resp, &resp_payload, &resp_sz);
    if (ret >= 0) *params = resp_payload.value;
}

void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    uint32_t hshader = get_host_id(shader);
    if (!hshader || !infoLog || bufSize <= 0) return;
    char buf[8192];
    uint32_t want = (uint32_t)bufSize;
    if (want > sizeof(buf)) want = sizeof(buf);
    gles_cmd_get_shader_info_log_t payload = { .shader = hshader, .max_length = want };
    gles_stream_response_t resp;
    size_t got = sizeof(buf);
    int ret = send_request_ex(GLES_CMD_GET_SHADER_INFO_LOG, &payload, sizeof(payload),
                              NULL, 0, &resp, buf, &got);
    if (ret >= 0) {
        size_t cp = got < (size_t)bufSize ? got : (size_t)bufSize - 1;
        memcpy(infoLog, buf, cp);
        infoLog[cp] = '\0';
        if (length) *length = (GLsizei)cp;
    }
}

void glDeleteShader(GLuint shader) {
    uint32_t hshader = unmap_android_id(shader);
    if (!hshader) return;
    gles_cmd_delete_shader_t payload = { .shader = hshader };
    send_request(GLES_CMD_DELETE_SHADER, &payload, sizeof(payload), NULL, NULL, NULL);
}

/* Program */
GLuint glCreateProgram(void) {
    gles_stream_response_t resp;
    gles_resp_id_t resp_payload;
    size_t resp_sz = sizeof(resp_payload);
    int ret = send_request(GLES_CMD_CREATE_PROGRAM, NULL, 0, &resp, &resp_payload, &resp_sz);
    if (ret < 0) return 0;
    return map_host_id(resp_payload.id);
}

void glAttachShader(GLuint program, GLuint shader) {
    uint32_t hprog = get_host_id(program);
    uint32_t hshader = get_host_id(shader);
    if (!hprog || !hshader) return;
    gles_cmd_attach_shader_t payload = { .program = hprog, .shader = hshader };
    send_request(GLES_CMD_ATTACH_SHADER, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glLinkProgram(GLuint program) {
    uint32_t hprog = get_host_id(program);
    if (!hprog) return;
    gles_cmd_link_program_t payload = { .program = hprog };
    send_request(GLES_CMD_LINK_PROGRAM, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
    uint32_t hprog = get_host_id(program);
    if (!hprog || !params) return;
    gles_cmd_get_program_iv_t payload = { .program = hprog, .pname = (uint32_t)pname };
    gles_stream_response_t resp;
    gles_resp_int_t resp_payload;
    size_t resp_sz = sizeof(resp_payload);
    int ret = send_request(GLES_CMD_GET_PROGRAM_IV, &payload, sizeof(payload), &resp, &resp_payload, &resp_sz);
    if (ret >= 0) *params = resp_payload.value;
}

void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    uint32_t hprog = get_host_id(program);
    if (!hprog || !infoLog || bufSize <= 0) return;
    char buf[8192];
    uint32_t want = (uint32_t)bufSize;
    if (want > sizeof(buf)) want = sizeof(buf);
    gles_cmd_get_program_info_log_t payload = { .program = hprog, .max_length = want };
    gles_stream_response_t resp;
    size_t got = sizeof(buf);
    int ret = send_request_ex(GLES_CMD_GET_PROGRAM_INFO_LOG, &payload, sizeof(payload),
                              NULL, 0, &resp, buf, &got);
    if (ret >= 0) {
        size_t cp = got < (size_t)bufSize ? got : (size_t)bufSize - 1;
        memcpy(infoLog, buf, cp);
        infoLog[cp] = '\0';
        if (length) *length = (GLsizei)cp;
    }
}

void glUseProgram(GLuint program) {
    uint32_t hprog = program ? get_host_id(program) : 0;
    gles_cmd_use_program_t payload = { .program = hprog };
    send_request(GLES_CMD_USE_PROGRAM, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glDeleteProgram(GLuint program) {
    uint32_t hprog = unmap_android_id(program);
    if (!hprog) return;
    gles_cmd_delete_program_t payload = { .program = hprog };
    send_request(GLES_CMD_DELETE_PROGRAM, &payload, sizeof(payload), NULL, NULL, NULL);
}

/* Buffer objects */
void glGenBuffers(GLsizei n, GLuint *buffers) {
    if (!buffers || n <= 0) return;
    gles_cmd_gen_buffers_t payload = { .n = (uint32_t)n };
    size_t resp_sz = sizeof(uint32_t) * n + sizeof(uint32_t);
    gles_resp_ids_t *resp_payload = malloc(resp_sz);
    if (!resp_payload) return;
    int ret = send_request(GLES_CMD_GEN_BUFFERS, &payload, sizeof(payload), NULL, resp_payload, &resp_sz);
    if (ret >= 0 && resp_payload->n == (uint32_t)n) {
        for (int i = 0; i < n; ++i) {
            buffers[i] = map_host_id(resp_payload->ids[i]);
        }
    }
    free(resp_payload);
}

void glBindBuffer(GLenum target, GLuint buffer) {
    uint32_t hbuffer = buffer ? get_host_id(buffer) : 0;
    gles_cmd_bind_buffer_t payload = { .target = (uint32_t)target, .buffer = hbuffer };
    send_request(GLES_CMD_BIND_BUFFER, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage) {
    gles_cmd_buffer_data_t payload = {
        .target = (uint32_t)target,
        .size = (int64_t)size,
        .usage = (uint32_t)usage,
    };
    send_request_ex(GLES_CMD_BUFFER_DATA, &payload, sizeof(payload),
                    (size > 0) ? data : NULL, (size > 0) ? (size_t)size : 0,
                    NULL, NULL, NULL);
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data) {
    gles_cmd_buffer_sub_data_t payload = {
        .target = (uint32_t)target,
        .offset = (int64_t)offset,
        .size = (int64_t)size,
    };
    send_request_ex(GLES_CMD_BUFFER_SUB_DATA, &payload, sizeof(payload),
                    (size > 0) ? data : NULL, (size > 0) ? (size_t)size : 0,
                    NULL, NULL, NULL);
}

void glDeleteBuffers(GLsizei n, const GLuint *buffers) {
    if (!buffers || n <= 0) return;
    size_t payload_sz = sizeof(uint32_t) * n + sizeof(uint32_t);
    gles_cmd_delete_buffers_t *payload = malloc(payload_sz);
    if (!payload) return;
    payload->n = (uint32_t)n;
    for (int i = 0; i < n; ++i) {
        payload->buffers[i] = unmap_android_id(buffers[i]);
    }
    send_request(GLES_CMD_DELETE_BUFFERS, payload, sizeof(uint32_t) + sizeof(uint32_t) * n, NULL, NULL, NULL);
    free(payload);
}

/* Vertex attributes */
void glVertexAttribf(GLuint index, GLfloat x) {
    gles_cmd_vertex_attrib_f_t payload = { .index = index, .x = x };
    send_request(GLES_CMD_VERTEX_ATTRIBF, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glVertexAttribfv(GLuint index, const GLfloat *v) {
    if (!v) return;
    gles_cmd_vertex_attrib_fv_t payload;
    payload.index = index;
    memcpy(payload.v, v, 4 * sizeof(float));
    send_request(GLES_CMD_VERTEX_ATTRIBFV, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                           GLboolean normalized, GLsizei stride, const GLvoid *pointer) {
    gles_cmd_vertex_attrib_pointer_t payload = {
        .index = index,
        .size = size,
        .type = (uint32_t)type,
        .normalized = normalized,
        .stride = stride,
        .offset = (int64_t)(uintptr_t)pointer,
    };
    send_request(GLES_CMD_VERTEX_ATTRIB_POINTER, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glEnableVertexAttribArray(GLuint index) {
    gles_cmd_enable_vertex_attrib_array_t payload = { .index = index };
    send_request(GLES_CMD_ENABLE_VERTEX_ATTRIB_ARRAY, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glDisableVertexAttribArray(GLuint index) {
    gles_cmd_disable_vertex_attrib_array_t payload = { .index = index };
    send_request(GLES_CMD_DISABLE_VERTEX_ATTRIB_ARRAY, &payload, sizeof(payload), NULL, NULL, NULL);
}

/* Textures */
void glGenTextures(GLsizei n, GLuint *textures) {
    if (!textures || n <= 0) return;
    gles_cmd_gen_textures_t payload = { .n = (uint32_t)n };
    size_t resp_sz = sizeof(uint32_t) * n + sizeof(uint32_t);
    gles_resp_ids_t *resp_payload = malloc(resp_sz);
    if (!resp_payload) return;
    int ret = send_request(GLES_CMD_GEN_TEXTURES, &payload, sizeof(payload), NULL, resp_payload, &resp_sz);
    if (ret >= 0 && resp_payload->n == (uint32_t)n) {
        for (int i = 0; i < n; ++i) {
            textures[i] = map_host_id(resp_payload->ids[i]);
        }
    }
    free(resp_payload);
}

void glBindTexture(GLenum target, GLuint texture) {
    uint32_t htex = texture ? get_host_id(texture) : 0;
    gles_cmd_bind_texture_t payload = { .target = (uint32_t)target, .texture = htex };
    send_request(GLES_CMD_BIND_TEXTURE, &payload, sizeof(payload), NULL, NULL, NULL);
}

static uint32_t pixel_bytes(GLenum format, GLenum type) {
    if (type == GL_UNSIGNED_BYTE) {
        switch (format) {
        case GL_RGBA: return 4;
        case GL_RGB:  return 3;
        case GL_RG:   return 2;
        case GL_RED:  return 1;
        default:      return 4;
        }
    }
    if (type == GL_UNSIGNED_SHORT) return 8;
    if (type == GL_FLOAT) return 16;
    return 4;
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels) {
    int64_t size = 0;
    if (pixels)
        size = (int64_t)width * height * pixel_bytes(format, type);
    gles_cmd_tex_image_2d_t payload = {
        .target = (uint32_t)target,
        .level = level,
        .internalformat = internalformat,
        .width = width,
        .height = height,
        .border = border,
        .format = (uint32_t)format,
        .type = (uint32_t)type,
        .size = (uint32_t)size,
    };
    send_request_ex(GLES_CMD_TEX_IMAGE_2D, &payload, sizeof(payload),
                    (pixels && size > 0) ? pixels : NULL,
                    (pixels && size > 0) ? (size_t)size : 0,
                    NULL, NULL, NULL);
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) {
    int64_t size = 0;
    if (pixels) {
        size = (int64_t)width * height * pixel_bytes(format, type);
    }
    gles_cmd_tex_sub_image_2d_t payload = {
        .target = (uint32_t)target,
        .level = level,
        .xoffset = xoffset,
        .yoffset = yoffset,
        .width = width,
        .height = height,
        .format = (uint32_t)format,
        .type = (uint32_t)type,
        .size = (uint32_t)size,
    };
    send_request_ex(GLES_CMD_TEX_SUB_IMAGE_2D, &payload, sizeof(payload),
                    (pixels && size > 0) ? pixels : NULL,
                    (pixels && size > 0) ? (size_t)size : 0,
                    NULL, NULL, NULL);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    gles_cmd_tex_parameteri_t payload = { .target = (uint32_t)target, .pname = (uint32_t)pname, .param = param };
    send_request(GLES_CMD_TEX_PARAMETERI, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    gles_cmd_tex_parameterf_t payload = { .target = (uint32_t)target, .pname = (uint32_t)pname, .param = param };
    send_request(GLES_CMD_TEX_PARAMETERF, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glDeleteTextures(GLsizei n, const GLuint *textures) {
    if (!textures || n <= 0) return;
    size_t payload_sz = sizeof(uint32_t) * n + sizeof(uint32_t);
    gles_cmd_delete_textures_t *payload = malloc(payload_sz);
    if (!payload) return;
    payload->n = (uint32_t)n;
    for (int i = 0; i < n; ++i) {
        payload->textures[i] = unmap_android_id(textures[i]);
    }
    send_request(GLES_CMD_DELETE_TEXTURES, payload, sizeof(uint32_t) + sizeof(uint32_t) * n, NULL, NULL, NULL);
    free(payload);
}

void glActiveTexture(GLenum texture) {
    gles_cmd_active_texture_t payload = { .texture = (uint32_t)texture };
    send_request(GLES_CMD_ACTIVE_TEXTURE, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glGenerateMipmap(GLenum target) {
    gles_cmd_generate_mipmap_t payload = { .target = (uint32_t)target };
    send_request(GLES_CMD_GENERATE_MIPMAP, &payload, sizeof(payload), NULL, NULL, NULL);
}

/* Uniforms */
GLint glGetUniformLocation(GLuint program, const GLchar *name) {
    uint32_t hprog = get_host_id(program);
    if (!hprog || !name) return -1;
    size_t name_len = strlen(name);
    gles_cmd_get_uniform_location_t payload = { .program = hprog, .name_len = (uint32_t)name_len };
    gles_stream_response_t resp;
    gles_resp_int_t resp_payload;
    size_t resp_sz = sizeof(resp_payload);
    int ret = send_request_ex(GLES_CMD_GET_UNIFORM_LOCATION, &payload, sizeof(payload),
                              name, name_len, &resp, &resp_payload, &resp_sz);
    if (ret < 0) return -1;
    return (GLint)resp_payload.value;
}

void glUniform1f(GLint location, GLfloat v) {
    gles_cmd_uniform_1f_t payload = { .location = (uint32_t)location, .v = v };
    send_request(GLES_CMD_UNIFORM_1F, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glUniform1fv(GLint location, GLsizei count, const GLfloat *v) {
    if (!v || count <= 0) return;
    gles_cmd_uniform_1fv_t payload = { .location = (uint32_t)location, .count = (int32_t)count };
    send_request_ex(GLES_CMD_UNIFORM_1FV, &payload, sizeof(payload),
                    v, (size_t)count * sizeof(float), NULL, NULL, NULL);
}

void glUniform2fv(GLint location, GLsizei count, const GLfloat *v) {
    if (!v || count <= 0) return;
    gles_cmd_uniform_2fv_t payload = { .location = (uint32_t)location, .count = (int32_t)count };
    send_request_ex(GLES_CMD_UNIFORM_2FV, &payload, sizeof(payload),
                    v, (size_t)count * 2 * sizeof(float), NULL, NULL, NULL);
}

void glUniform3fv(GLint location, GLsizei count, const GLfloat *v) {
    if (!v || count <= 0) return;
    gles_cmd_uniform_3fv_t payload = { .location = (uint32_t)location, .count = (int32_t)count };
    send_request_ex(GLES_CMD_UNIFORM_3FV, &payload, sizeof(payload),
                    v, (size_t)count * 3 * sizeof(float), NULL, NULL, NULL);
}

void glUniform4fv(GLint location, GLsizei count, const GLfloat *v) {
    if (!v || count <= 0) return;
    gles_cmd_uniform_4fv_t payload = { .location = (uint32_t)location, .count = (int32_t)count };
    send_request_ex(GLES_CMD_UNIFORM_4FV, &payload, sizeof(payload),
                    v, (size_t)count * 4 * sizeof(float), NULL, NULL, NULL);
}

void glUniform1i(GLint location, GLint v) {
    gles_cmd_uniform_1i_t payload = { .location = (uint32_t)location, .v = v };
    send_request(GLES_CMD_UNIFORM_1I, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glUniform1iv(GLint location, GLsizei count, const GLint *v) {
    if (!v || count <= 0) return;
    gles_cmd_uniform_1iv_t payload = { .location = (uint32_t)location, .count = (int32_t)count };
    send_request_ex(GLES_CMD_UNIFORM_1IV, &payload, sizeof(payload),
                    v, (size_t)count * sizeof(int), NULL, NULL, NULL);
}

void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *v) {
    if (!v || count <= 0) return;
    gles_cmd_uniform_matrix_2fv_t payload = { .location = (uint32_t)location, .count = (int32_t)count, .transpose = transpose };
    send_request_ex(GLES_CMD_UNIFORM_MATRIX_2FV, &payload, sizeof(payload),
                    v, (size_t)count * 4 * sizeof(float), NULL, NULL, NULL);
}

void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *v) {
    if (!v || count <= 0) return;
    gles_cmd_uniform_matrix_3fv_t payload = { .location = (uint32_t)location, .count = (int32_t)count, .transpose = transpose };
    send_request_ex(GLES_CMD_UNIFORM_MATRIX_3FV, &payload, sizeof(payload),
                    v, (size_t)count * 9 * sizeof(float), NULL, NULL, NULL);
}

void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *v) {
    if (!v || count <= 0) return;
    gles_cmd_uniform_matrix_4fv_t payload = { .location = (uint32_t)location, .count = (int32_t)count, .transpose = transpose };
    send_request_ex(GLES_CMD_UNIFORM_MATRIX_4FV, &payload, sizeof(payload),
                    v, (size_t)count * 16 * sizeof(float), NULL, NULL, NULL);
}

/* Draw / Clear / State */
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    gles_cmd_viewport_t payload = { .x = x, .y = y, .w = width, .h = height };
    send_request(GLES_CMD_VIEWPORT, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    gles_cmd_clear_color_t payload = { .r = r, .g = g, .b = b, .a = a };
    send_request(GLES_CMD_CLEAR_COLOR, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glClear(GLbitfield mask) {
    gles_cmd_clear_t payload = { .mask = (uint32_t)mask };
    send_request(GLES_CMD_CLEAR, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gles_cmd_draw_arrays_t payload = { .mode = (uint32_t)mode, .first = first, .count = count };
    send_request(GLES_CMD_DRAW_ARRAYS, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
    gles_cmd_draw_elements_t payload = {
        .mode = (uint32_t)mode,
        .count = count,
        .type = (uint32_t)type,
        .indices_offset = (int64_t)(uintptr_t)indices,
    };
    send_request(GLES_CMD_DRAW_ELEMENTS, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glEnable(GLenum cap) {
    gles_cmd_enable_t payload = { .cap = (uint32_t)cap };
    send_request(GLES_CMD_ENABLE, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glDisable(GLenum cap) {
    gles_cmd_disable_t payload = { .cap = (uint32_t)cap };
    send_request(GLES_CMD_DISABLE, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glBlendFunc(GLenum sfactor, GLenum dfactor) {
    gles_cmd_blend_func_t payload = { .sfactor = (uint32_t)sfactor, .dfactor = (uint32_t)dfactor };
    send_request(GLES_CMD_BLEND_FUNC, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB,
                         GLenum sfactorAlpha, GLenum dfactorAlpha) {
    gles_cmd_blend_func_separate_t payload = {
        .sfactor_rgb = (uint32_t)sfactorRGB,
        .dfactor_rgb = (uint32_t)dfactorRGB,
        .sfactor_alpha = (uint32_t)sfactorAlpha,
        .dfactor_alpha = (uint32_t)dfactorAlpha,
    };
    send_request(GLES_CMD_BLEND_FUNC_SEPARATE, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glDepthFunc(GLenum func) {
    gles_cmd_depth_func_t payload = { .func = (uint32_t)func };
    send_request(GLES_CMD_DEPTH_FUNC, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glDepthMask(GLboolean flag) {
    gles_cmd_depth_mask_t payload = { .flag = flag };
    send_request(GLES_CMD_DEPTH_MASK, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glCullFace(GLenum mode) {
    gles_cmd_cull_face_t payload = { .mode = (uint32_t)mode };
    send_request(GLES_CMD_CULL_FACE, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glFrontFace(GLenum mode) {
    gles_cmd_front_face_t payload = { .mode = (uint32_t)mode };
    send_request(GLES_CMD_FRONT_FACE, &payload, sizeof(payload), NULL, NULL, NULL);
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    gles_cmd_scissor_t payload = { .x = x, .y = y, .w = width, .h = height };
    send_request(GLES_CMD_SCISSOR, &payload, sizeof(payload), NULL, NULL, NULL);
}

/* Readback */
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *pixels) {
    gles_cmd_read_pixels_t payload = {
        .target = 0,  /* unused */
        .level = 0,
        .x = x, .y = y, .w = width, .h = height,
        .format = (uint32_t)format,
        .type = (uint32_t)type,
    };
    if (!pixels || width <= 0 || height <= 0) return;
    uint32_t bpp = pixel_bytes(format, type);
    size_t size = (size_t)width * (size_t)height * bpp;
    uint8_t *buf = malloc(size);
    if (!buf) return;
    gles_stream_response_t resp;
    size_t got = size;
    int ret = send_request_ex(GLES_CMD_READ_PIXELS, &payload, sizeof(payload),
                              NULL, 0, &resp, buf, &got);
    if (ret >= 0 && got > 0)
        memcpy(pixels, buf, got < size ? got : size);
    free(buf);
}

GLenum glGetError(void) {
    gles_stream_response_t resp;
    gles_resp_error_t resp_payload;
    size_t resp_sz = sizeof(resp_payload);
    int ret = send_request(GLES_CMD_GET_ERROR, NULL, 0, &resp, &resp_payload, &resp_sz);
    if (ret < 0) return GL_INVALID_OPERATION;
    return (GLenum)resp_payload.err;
}

const GLubyte *glGetString(GLenum name) {
    static char buf[8192];
    gles_cmd_get_string_t payload = { .name = (uint32_t)name };
    gles_stream_response_t resp;
    size_t got = sizeof(buf) - 1;
    int ret = send_request_ex(GLES_CMD_GET_STRING, &payload, sizeof(payload),
                              NULL, 0, &resp, buf, &got);
    if (ret >= 0 && got > 0) {
        if (got > sizeof(buf) - 1) got = sizeof(buf) - 1;
        buf[got] = '\0';
        return (const GLubyte *)buf;
    }
    return NULL;
}

void glFlush(void) {
    send_request(GLES_CMD_FLUSH, NULL, 0, NULL, NULL, NULL);
}

void glFinish(void) {
    send_request(GLES_CMD_FINISH, NULL, 0, NULL, NULL, NULL);
}

/* ---------- Init / Cleanup (called by Android apps) ---------- */
__attribute__((constructor))
static void gles_client_init(void) {
    /* Auto-connect on first use; lazy init */
}

__attribute__((destructor))
static void gles_client_fini(void) {
    disconnect_host();
}