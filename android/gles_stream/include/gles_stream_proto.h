/* language: C11, file: android/gles_stream/include/gles_stream_proto.h
 * Lightweight binary protocol for streaming GLES 2.0 commands to the host GLX server.
 * No GBM/DMA-BUF — just a socket plus serialized calls.
 */
#ifndef GLES_STREAM_PROTO_H
#define GLES_STREAM_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#define GLES_STREAM_SOCK_PATH "/tmp/x11droid_gl.sock"
#define GLES_STREAM_MAGIC     0x474C4553u  /* "GLES" little-endian */
#define GLES_STREAM_VERSION   1

/* Command types (Android -> host requests) */
typedef enum {
    GLES_CMD_NONE = 0,

    /* Resource management */
    GLES_CMD_CREATE_SHADER,
    GLES_CMD_SHADER_SOURCE,
    GLES_CMD_COMPILE_SHADER,
    GLES_CMD_GET_SHADER_IV,
    GLES_CMD_GET_SHADER_INFO_LOG,
    GLES_CMD_DELETE_SHADER,

    GLES_CMD_CREATE_PROGRAM,
    GLES_CMD_ATTACH_SHADER,
    GLES_CMD_LINK_PROGRAM,
    GLES_CMD_GET_PROGRAM_IV,
    GLES_CMD_GET_PROGRAM_INFO_LOG,
    GLES_CMD_USE_PROGRAM,
    GLES_CMD_DELETE_PROGRAM,

    GLES_CMD_CREATE_SHADER_PROGRAM_V2,  /* helper: create+compile+link in one roundtrip */

    /* Buffer objects */
    GLES_CMD_GEN_BUFFERS,
    GLES_CMD_BIND_BUFFER,
    GLES_CMD_BUFFER_DATA,
    GLES_CMD_BUFFER_SUB_DATA,
    GLES_CMD_DELETE_BUFFERS,

    /* Vertex arrays / attributes */
    GLES_CMD_VERTEX_ATTRIBF,
    GLES_CMD_VERTEX_ATTRIBFV,
    GLES_CMD_VERTEX_ATTRIB_POINTER,
    GLES_CMD_ENABLE_VERTEX_ATTRIB_ARRAY,
    GLES_CMD_DISABLE_VERTEX_ATTRIB_ARRAY,

    /* Textures */
    GLES_CMD_GEN_TEXTURES,
    GLES_CMD_BIND_TEXTURE,
    GLES_CMD_TEX_IMAGE_2D,
    GLES_CMD_TEX_SUB_IMAGE_2D,
    GLES_CMD_TEX_PARAMETERI,
    GLES_CMD_TEX_PARAMETERF,
    GLES_CMD_DELETE_TEXTURES,
    GLES_CMD_ACTIVE_TEXTURE,
    GLES_CMD_GENERATE_MIPMAP,

    /* Uniforms */
    GLES_CMD_GET_UNIFORM_LOCATION,
    GLES_CMD_UNIFORM_1F,
    GLES_CMD_UNIFORM_1FV,
    GLES_CMD_UNIFORM_2FV,
    GLES_CMD_UNIFORM_3FV,
    GLES_CMD_UNIFORM_4FV,
    GLES_CMD_UNIFORM_1I,
    GLES_CMD_UNIFORM_1IV,
    GLES_CMD_UNIFORM_MATRIX_2FV,
    GLES_CMD_UNIFORM_MATRIX_3FV,
    GLES_CMD_UNIFORM_MATRIX_4FV,

    /* Draw / clear / viewport / state */
    GLES_CMD_VIEWPORT,
    GLES_CMD_CLEAR_COLOR,
    GLES_CMD_CLEAR,
    GLES_CMD_DRAW_ARRAYS,
    GLES_CMD_DRAW_ELEMENTS,
    GLES_CMD_ENABLE,
    GLES_CMD_DISABLE,
    GLES_CMD_BLEND_FUNC,
    GLES_CMD_BLEND_FUNC_SEPARATE,
    GLES_CMD_DEPTH_FUNC,
    GLES_CMD_DEPTH_MASK,
    GLES_CMD_CULL_FACE,
    GLES_CMD_FRONT_FACE,
    GLES_CMD_SCISSOR,

    /* Readback */
    GLES_CMD_READ_PIXELS,
    GLES_CMD_GET_ERROR,
    GLES_CMD_GET_STRING,

    /* Sync / flush */
    GLES_CMD_FLUSH,
    GLES_CMD_FINISH,

    /* Resource ID mapping (Android ID <-> Host ID) */
    GLES_CMD_MAP_RESOURCE,
    GLES_CMD_UNMAP_RESOURCE,

    GLES_CMD_MAX
} gles_cmd_type_t;

/* Common header for every message */
typedef struct {
    uint32_t magic;        /* GLES_STREAM_MAGIC */
    uint16_t version;      /* GLES_STREAM_VERSION */
    uint16_t cmd;          /* gles_cmd_type_t */
    uint32_t seq;          /* sequence number (request) */
    uint32_t payload_size; /* payload size after this header */
} gles_stream_header_t;

/* Standard response from host -> Android */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t cmd;          /* command being responded to */
    uint32_t seq;          /* sequence number of the request being answered */
    int32_t  result;       /* 0 = OK, <0 = error (negative errno) */
    uint32_t payload_size; /* response payload size */
} gles_stream_response_t;

/* Per-command payloads (only fields that take arguments) */
typedef struct { uint32_t type; }                                    gles_cmd_create_shader_t;        /* -> returns shader_id */
typedef struct { uint32_t shader; uint32_t count; uint32_t length; } gles_cmd_shader_source_t;     /* followed by the source string */
typedef struct { uint32_t shader; }                                  gles_cmd_compile_shader_t;
typedef struct { uint32_t shader; uint32_t pname; }                  gles_cmd_get_shader_iv_t;
typedef struct { uint32_t shader; uint32_t max_length; }             gles_cmd_get_shader_info_log_t;
typedef struct { uint32_t shader; }                                  gles_cmd_delete_shader_t;

typedef struct { }                                                   gles_cmd_create_program_t;     /* -> returns program_id */
typedef struct { uint32_t program; uint32_t shader; }                gles_cmd_attach_shader_t;
typedef struct { uint32_t program; }                                 gles_cmd_link_program_t;
typedef struct { uint32_t program; uint32_t pname; }                 gles_cmd_get_program_iv_t;
typedef struct { uint32_t program; uint32_t max_length; }            gles_cmd_get_program_info_log_t;
typedef struct { uint32_t program; }                                 gles_cmd_use_program_t;
typedef struct { uint32_t program; }                                 gles_cmd_delete_program_t;

/* Helper: create+compile+link vertex+fragment shaders in one call */
typedef struct {
    uint32_t vs_len;
    uint32_t fs_len;
    /* followed by vs_source (vs_len bytes) + fs_source (fs_len bytes) */
} gles_cmd_create_shader_program_v2_t;  /* -> returns program_id */

typedef struct { uint32_t n; }                                       gles_cmd_gen_buffers_t;        /* -> array buffer_ids[n] */
typedef struct { uint32_t target; uint32_t buffer; }                 gles_cmd_bind_buffer_t;
typedef struct { uint32_t target; int64_t size; uint32_t usage; }    gles_cmd_buffer_data_t;        /* followed by data[size] when size > 0 */
typedef struct { uint32_t target; int64_t offset; int64_t size; }    gles_cmd_buffer_sub_data_t;    /* followed by data[size] */
typedef struct { uint32_t n; uint32_t buffers[0]; }                  gles_cmd_delete_buffers_t;     /* flexible array */

typedef struct { uint32_t index; float x; }                          gles_cmd_vertex_attrib_f_t;
typedef struct { uint32_t index; float v[4]; }                       gles_cmd_vertex_attrib_fv_t;   /* v[0..3] */
typedef struct {
    uint32_t index;
    int32_t  size;
    uint32_t type;
    uint8_t  normalized;
    int32_t  stride;
    int64_t  offset;  /* byte offset in the bound buffer */
} gles_cmd_vertex_attrib_pointer_t;
typedef struct { uint32_t index; }                                   gles_cmd_enable_vertex_attrib_array_t;
typedef struct { uint32_t index; }                                   gles_cmd_disable_vertex_attrib_array_t;

typedef struct { uint32_t n; }                                       gles_cmd_gen_textures_t;       /* -> array texture_ids[n] */
typedef struct { uint32_t target; uint32_t texture; }                gles_cmd_bind_texture_t;
typedef struct {
    uint32_t target;
    int32_t  level;
    int32_t  internalformat;
    int32_t  width;
    int32_t  height;
    int32_t  border;
    uint32_t format;
    uint32_t type;
    int64_t  size;  /* 0 = NULL pointer */
} gles_cmd_tex_image_2d_t;               /* followed by pixel data[size] when size > 0 */
typedef struct {
    uint32_t target;
    int32_t  level;
    int32_t  xoffset;
    int32_t  yoffset;
    int32_t  width;
    int32_t  height;
    uint32_t format;
    uint32_t type;
    int64_t  size;
} gles_cmd_tex_sub_image_2d_t;           /* followed by pixel data[size] */
typedef struct { uint32_t target; uint32_t pname; int32_t param; }   gles_cmd_tex_parameteri_t;
typedef struct { uint32_t target; uint32_t pname; float param; }     gles_cmd_tex_parameterf_t;
typedef struct { uint32_t n; uint32_t textures[0]; }                 gles_cmd_delete_textures_t;
typedef struct { uint32_t texture; }                                 gles_cmd_active_texture_t;
typedef struct { uint32_t target; }                                  gles_cmd_generate_mipmap_t;

typedef struct { uint32_t program; uint32_t name_len; }              gles_cmd_get_uniform_location_t; /* followed by name[name_len] */
typedef struct { uint32_t location; float v; }                       gles_cmd_uniform_1f_t;
typedef struct { uint32_t location; int32_t count; }                 gles_cmd_uniform_1fv_t;        /* followed by float v[count] */
typedef struct { uint32_t location; int32_t count; }                 gles_cmd_uniform_2fv_t;        /* followed by float v[count*2] */
typedef struct { uint32_t location; int32_t count; }                 gles_cmd_uniform_3fv_t;        /* followed by float v[count*3] */
typedef struct { uint32_t location; int32_t count; }                 gles_cmd_uniform_4fv_t;        /* followed by float v[count*4] */
typedef struct { uint32_t location; int32_t v; }                     gles_cmd_uniform_1i_t;
typedef struct { uint32_t location; int32_t count; }                 gles_cmd_uniform_1iv_t;        /* followed by int v[count] */
typedef struct { uint32_t location; int32_t count; uint8_t transpose; } gles_cmd_uniform_matrix_2fv_t; /* float v[count*4] */
typedef struct { uint32_t location; int32_t count; uint8_t transpose; } gles_cmd_uniform_matrix_3fv_t; /* float v[count*9] */
typedef struct { uint32_t location; int32_t count; uint8_t transpose; } gles_cmd_uniform_matrix_4fv_t; /* float v[count*16] */

typedef struct { int32_t x; int32_t y; int32_t w; int32_t h; }       gles_cmd_viewport_t;
typedef struct { float r, g, b, a; }                                 gles_cmd_clear_color_t;
typedef struct { uint32_t mask; }                                    gles_cmd_clear_t;
typedef struct { uint32_t mode; int32_t first; int32_t count; }      gles_cmd_draw_arrays_t;
typedef struct { uint32_t mode; int32_t count; uint32_t type; int64_t indices_offset; } gles_cmd_draw_elements_t;
typedef struct { uint32_t cap; }                                     gles_cmd_enable_t;
typedef struct { uint32_t cap; }                                     gles_cmd_disable_t;
typedef struct { uint32_t sfactor; uint32_t dfactor; }               gles_cmd_blend_func_t;
typedef struct { uint32_t sfactor_rgb; uint32_t dfactor_rgb; uint32_t sfactor_alpha; uint32_t dfactor_alpha; } gles_cmd_blend_func_separate_t;
typedef struct { uint32_t func; }                                    gles_cmd_depth_func_t;
typedef struct { uint8_t flag; }                                     gles_cmd_depth_mask_t;
typedef struct { uint32_t mode; }                                    gles_cmd_cull_face_t;
typedef struct { uint32_t mode; }                                    gles_cmd_front_face_t;
typedef struct { int32_t x, y, w, h; }                               gles_cmd_scissor_t;

typedef struct { uint32_t target; int32_t level; int32_t x; int32_t y; int32_t w; int32_t h; uint32_t format; uint32_t type; } gles_cmd_read_pixels_t; /* -> pixel data response */

typedef struct { } gles_cmd_get_error_t;
typedef struct { uint32_t name; } gles_cmd_get_string_t;

typedef struct { } gles_cmd_flush_t;
typedef struct { } gles_cmd_finish_t;

typedef struct { uint32_t android_id; uint32_t host_id; }            gles_cmd_map_resource_t;
typedef struct { uint32_t android_id; }                              gles_cmd_unmap_resource_t;

/* Response payloads (for commands that return a value) */
typedef struct { uint32_t id; }       gles_resp_id_t;          /* shader, program, buffer, texture */
typedef struct { uint32_t n; uint32_t ids[0]; } gles_resp_ids_t; /* array ID */
typedef struct { int32_t value; }     gles_resp_int_t;          /* getShaderiv, getProgramiv, getUniformLocation, getError */
typedef struct { int64_t value; }     gles_resp_int64_t;        /* getShaderiv etc. for large values */
typedef struct { uint32_t len; }      gles_resp_string_t;       /* followed by string[len] */
typedef struct { uint32_t len; }      gles_resp_data_t;         /* followed by data[len] (readPixels, shaderInfoLog, programInfoLog) */
typedef struct { int32_t err; }       gles_resp_error_t;        /* getError */

/* Socket communication helpers */
static inline int gles_send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static inline int gles_recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, MSG_WAITALL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

#endif /* GLES_STREAM_PROTO_H */