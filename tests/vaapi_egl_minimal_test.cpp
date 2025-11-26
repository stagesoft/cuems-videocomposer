/**
 * VAAPI EGL Minimal Test
 * 
 * A standalone test program to isolate and debug the VAAPI frozen frames issue.
 * This minimal test decodes a video using VAAPI and displays frames via EGL/OpenGL.
 * 
 * Build:
 *   g++ -o vaapi_egl_test vaapi_egl_minimal_test.cpp \
 *       $(pkg-config --cflags --libs libavformat libavcodec libavutil egl gl x11 libdrm libva) \
 *       -lX11 -lGL -lEGL
 * 
 * Run:
 *   ./vaapi_egl_test path/to/video.mp4
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>

// FFmpeg headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
}

// VA-API headers
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <drm_fourcc.h>

// EGL/X11/OpenGL headers
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <ctime>

// EGL extension constants
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274

// Function pointer types
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, void*);
typedef void (*PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)(GLenum, void*, const GLint*);

// Global state
struct TestState {
    // X11
    Display* display;
    Window window;
    int width;
    int height;
    
    // EGL
    EGLDisplay eglDisplay;
    EGLContext eglContext;
    EGLSurface eglSurface;
    
    // EGL extensions
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC glEGLImageTargetTexStorageEXT;
    bool useTexStorage;  // Use immutable textures (Desktop GL)
    
    // OpenGL
    GLuint textureY;
    GLuint textureUV;
    GLuint shaderProgram;
    
    // Experimental options
    bool forceNewTextures;  // Create new textures each frame
    
    // VAAPI
    int drmFd;
    VADisplay vaDisplay;
    
    // FFmpeg
    AVFormatContext* formatCtx;
    AVCodecContext* codecCtx;
    AVBufferRef* hwDeviceCtx;
    int videoStreamIdx;
    
    // Current frame
    EGLImageKHR eglImageY;
    EGLImageKHR eglImageUV;
    AVFrame* currentFrame;
    int frameCount;
};

static TestState g_state = {};

void initTestState() {
    memset(&g_state, 0, sizeof(g_state));
    g_state.forceNewTextures = true;  // Enable new textures per frame
    g_state.drmFd = -1;
}

// Simple NV12 to RGB fragment shader
const char* vertexShaderSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

const char* fragmentShaderSrc = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexY;
uniform sampler2D uTexUV;
void main() {
    float y = texture(uTexY, vTexCoord).r;
    vec2 uv = texture(uTexUV, vTexCoord).rg;
    float u = uv.r - 0.5;
    float v = uv.g - 0.5;
    float r = y + 1.402 * v;
    float g = y - 0.344 * u - 0.714 * v;
    float b = y + 1.772 * u;
    fragColor = vec4(r, g, b, 1.0);
}
)";

bool initX11() {
    g_state.display = XOpenDisplay(nullptr);
    if (!g_state.display) {
        fprintf(stderr, "Failed to open X display\n");
        return false;
    }
    
    g_state.width = 1280;
    g_state.height = 720;
    
    Window root = DefaultRootWindow(g_state.display);
    XSetWindowAttributes attrs;
    attrs.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask;
    
    g_state.window = XCreateWindow(
        g_state.display, root,
        0, 0, g_state.width, g_state.height, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWEventMask, &attrs);
    
    XStoreName(g_state.display, g_state.window, "VAAPI EGL Test");
    XMapWindow(g_state.display, g_state.window);
    
    printf("[X11] Window created %dx%d\n", g_state.width, g_state.height);
    return true;
}

bool initEGL() {
    g_state.eglDisplay = eglGetDisplay((EGLNativeDisplayType)g_state.display);
    if (g_state.eglDisplay == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return false;
    }
    
    EGLint major, minor;
    if (!eglInitialize(g_state.eglDisplay, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return false;
    }
    printf("[EGL] Version %d.%d\n", major, minor);
    
    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    
    EGLConfig config;
    EGLint numConfigs;
    if (!eglChooseConfig(g_state.eglDisplay, configAttribs, &config, 1, &numConfigs) || numConfigs == 0) {
        fprintf(stderr, "Failed to choose EGL config\n");
        return false;
    }
    
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "Failed to bind OpenGL API\n");
        return false;
    }
    
    EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    
    g_state.eglContext = eglCreateContext(g_state.eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (g_state.eglContext == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        return false;
    }
    
    g_state.eglSurface = eglCreateWindowSurface(g_state.eglDisplay, config, g_state.window, nullptr);
    if (g_state.eglSurface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL surface\n");
        return false;
    }
    
    if (!eglMakeCurrent(g_state.eglDisplay, g_state.eglSurface, g_state.eglSurface, g_state.eglContext)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        return false;
    }
    
    // Get extension functions
    g_state.eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    g_state.eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    g_state.glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    g_state.glEGLImageTargetTexStorageEXT = (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)eglGetProcAddress("glEGLImageTargetTexStorageEXT");
    
    if (!g_state.eglCreateImageKHR || !g_state.eglDestroyImageKHR) {
        fprintf(stderr, "Missing required EGL extensions\n");
        return false;
    }
    
    // Prefer TexStorage (immutable) on Desktop GL - this is what mpv does
    if (g_state.glEGLImageTargetTexStorageEXT) {
        g_state.useTexStorage = true;
        printf("[EGL] Using glEGLImageTargetTexStorageEXT (immutable textures - mpv approach)\n");
    } else if (g_state.glEGLImageTargetTexture2DOES) {
        g_state.useTexStorage = false;
        printf("[EGL] Using glEGLImageTargetTexture2DOES (mutable textures)\n");
    } else {
        fprintf(stderr, "No EGL image target function available\n");
        return false;
    }
    
    printf("[EGL] Context created with DMA-BUF extensions\n");
    printf("[GL] Version: %s\n", glGetString(GL_VERSION));
    return true;
}

bool initVAAPI() {
    // Open DRM device
    g_state.drmFd = open("/dev/dri/renderD128", O_RDWR);
    if (g_state.drmFd < 0) {
        fprintf(stderr, "Failed to open DRM device\n");
        return false;
    }
    
    // Create VAAPI display
    g_state.vaDisplay = vaGetDisplayDRM(g_state.drmFd);
    if (!g_state.vaDisplay) {
        fprintf(stderr, "Failed to get VA display\n");
        return false;
    }
    
    int major, minor;
    VAStatus status = vaInitialize(g_state.vaDisplay, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "Failed to initialize VA-API: %s\n", vaErrorStr(status));
        return false;
    }
    
    printf("[VAAPI] Version %d.%d\n", major, minor);
    printf("[VAAPI] Driver: %s\n", vaQueryVendorString(g_state.vaDisplay));
    return true;
}

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader compilation failed: %s\n", log);
        return 0;
    }
    return shader;
}

bool initOpenGL() {
    // Create shader program
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexShaderSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSrc);
    if (!vs || !fs) return false;
    
    g_state.shaderProgram = glCreateProgram();
    glAttachShader(g_state.shaderProgram, vs);
    glAttachShader(g_state.shaderProgram, fs);
    glLinkProgram(g_state.shaderProgram);
    
    GLint success;
    glGetProgramiv(g_state.shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(g_state.shaderProgram, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader link failed: %s\n", log);
        return false;
    }
    
    glDeleteShader(vs);
    glDeleteShader(fs);
    
    // Create textures
    glGenTextures(1, &g_state.textureY);
    glGenTextures(1, &g_state.textureUV);
    
    // Set texture parameters
    glBindTexture(GL_TEXTURE_2D, g_state.textureY);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, g_state.textureUV);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    printf("[GL] Shader program and textures created\n");
    return true;
}

bool openVideo(const char* filename) {
    // Open video file
    if (avformat_open_input(&g_state.formatCtx, filename, nullptr, nullptr) < 0) {
        fprintf(stderr, "Failed to open video file: %s\n", filename);
        return false;
    }
    
    if (avformat_find_stream_info(g_state.formatCtx, nullptr) < 0) {
        fprintf(stderr, "Failed to find stream info\n");
        return false;
    }
    
    // Find video stream
    g_state.videoStreamIdx = -1;
    for (unsigned i = 0; i < g_state.formatCtx->nb_streams; i++) {
        if (g_state.formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            g_state.videoStreamIdx = i;
            break;
        }
    }
    
    if (g_state.videoStreamIdx < 0) {
        fprintf(stderr, "No video stream found\n");
        return false;
    }
    
    AVCodecParameters* codecPar = g_state.formatCtx->streams[g_state.videoStreamIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        return false;
    }
    
    g_state.codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(g_state.codecCtx, codecPar);
    
    // Create VAAPI hardware device context
    AVBufferRef* hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
    AVHWDeviceContext* hwctx = (AVHWDeviceContext*)hwDeviceCtx->data;
    AVVAAPIDeviceContext* vactx = (AVVAAPIDeviceContext*)hwctx->hwctx;
    vactx->display = g_state.vaDisplay;
    
    if (av_hwdevice_ctx_init(hwDeviceCtx) < 0) {
        fprintf(stderr, "Failed to init VAAPI device context\n");
        return false;
    }
    
    g_state.hwDeviceCtx = hwDeviceCtx;
    g_state.codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
    
    // Set pixel format callback
    g_state.codecCtx->get_format = [](AVCodecContext* ctx, const AVPixelFormat* pix_fmts) -> AVPixelFormat {
        for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p == AV_PIX_FMT_VAAPI) return *p;
        }
        return AV_PIX_FMT_NONE;
    };
    
    if (avcodec_open2(g_state.codecCtx, codec, nullptr) < 0) {
        fprintf(stderr, "Failed to open codec\n");
        return false;
    }
    
    g_state.currentFrame = av_frame_alloc();
    
    printf("[FFmpeg] Video: %dx%d, codec: %s, VAAPI hardware decoding enabled\n",
           codecPar->width, codecPar->height, codec->name);
    return true;
}

bool importVAAPISurface(AVFrame* frame) {
    // Save old texture IDs for deletion after new ones are created
    GLuint oldTextureY = g_state.textureY;
    GLuint oldTextureUV = g_state.textureUV;
    
    // Release previous EGL images
    if (g_state.eglImageY != EGL_NO_IMAGE_KHR) {
        g_state.eglDestroyImageKHR(g_state.eglDisplay, g_state.eglImageY);
        g_state.eglImageY = EGL_NO_IMAGE_KHR;
    }
    if (g_state.eglImageUV != EGL_NO_IMAGE_KHR) {
        g_state.eglDestroyImageKHR(g_state.eglDisplay, g_state.eglImageUV);
        g_state.eglImageUV = EGL_NO_IMAGE_KHR;
    }
    
    // Get VAAPI surface
    VASurfaceID surface = (VASurfaceID)(uintptr_t)frame->data[3];
    printf("[VAAPI] Frame %d: Surface %u\n", g_state.frameCount, surface);
    
    // Sync surface
    VAStatus status = vaSyncSurface(g_state.vaDisplay, surface);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaSyncSurface failed: %s\n", vaErrorStr(status));
    }
    
    // Export to DMA-BUF
    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    
    status = vaExportSurfaceHandle(
        g_state.vaDisplay, surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &desc);
    
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaExportSurfaceHandle failed: %s\n", vaErrorStr(status));
        return false;
    }
    
    printf("[DMA-BUF] Exported: %dx%d, layers=%d, fourcc=0x%x\n",
           desc.width, desc.height, desc.num_layers, desc.fourcc);
    
    // Create EGL images
    int yFd = desc.objects[desc.layers[0].object_index[0]].fd;
    int uvFd = desc.objects[desc.layers[1].object_index[0]].fd;
    
    printf("[DMA-BUF] FDs: Y=%d UV=%d\n", yFd, uvFd);
    
    EGLint yAttribs[] = {
        EGL_WIDTH, (EGLint)desc.width,
        EGL_HEIGHT, (EGLint)desc.height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)desc.layers[0].drm_format,
        EGL_DMA_BUF_PLANE0_FD_EXT, yFd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)desc.layers[0].offset[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)desc.layers[0].pitch[0],
        EGL_NONE
    };
    
    g_state.eglImageY = g_state.eglCreateImageKHR(
        g_state.eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, yAttribs);
    
    EGLint uvAttribs[] = {
        EGL_WIDTH, (EGLint)(desc.width / 2),
        EGL_HEIGHT, (EGLint)(desc.height / 2),
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)desc.layers[1].drm_format,
        EGL_DMA_BUF_PLANE0_FD_EXT, uvFd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)desc.layers[1].offset[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)desc.layers[1].pitch[0],
        EGL_NONE
    };
    
    g_state.eglImageUV = g_state.eglCreateImageKHR(
        g_state.eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, uvAttribs);
    
    // Close DMA-BUF FDs
    for (unsigned i = 0; i < desc.num_objects; i++) {
        close(desc.objects[i].fd);
    }
    
    if (g_state.eglImageY == EGL_NO_IMAGE_KHR || g_state.eglImageUV == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "Failed to create EGL images\n");
        return false;
    }
    
    printf("[EGL] Images created: Y=%p UV=%p\n", g_state.eglImageY, g_state.eglImageUV);
    
    // EXPERIMENTAL: Create new textures each frame to avoid GPU caching
    if (g_state.forceNewTextures) {
        // Create new textures
        glGenTextures(1, &g_state.textureY);
        glGenTextures(1, &g_state.textureUV);
        
        // Set texture parameters BEFORE binding EGL image
        glBindTexture(GL_TEXTURE_2D, g_state.textureY);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        glBindTexture(GL_TEXTURE_2D, g_state.textureUV);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    
    // Bind EGL images to textures
    glBindTexture(GL_TEXTURE_2D, g_state.textureY);
    if (g_state.useTexStorage && g_state.glEGLImageTargetTexStorageEXT) {
        g_state.glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, g_state.eglImageY, nullptr);
    } else {
        g_state.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, g_state.eglImageY);
    }
    
    glBindTexture(GL_TEXTURE_2D, g_state.textureUV);
    if (g_state.useTexStorage && g_state.glEGLImageTargetTexStorageEXT) {
        g_state.glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, g_state.eglImageUV, nullptr);
    } else {
        g_state.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, g_state.eglImageUV);
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
    glFlush();
    glFinish();  // Force GPU completion
    
    printf("[GL] Textures bound: Y=%u UV=%u (%s, %s)\n", 
           g_state.textureY, g_state.textureUV,
           g_state.forceNewTextures ? "NEW" : "reused",
           g_state.useTexStorage ? "immutable" : "mutable");
    
    // Delete old textures AFTER new ones are created and bound
    if (g_state.forceNewTextures && oldTextureY != 0) {
        glDeleteTextures(1, &oldTextureY);
        glDeleteTextures(1, &oldTextureUV);
    }
    
    return true;
}

void render() {
    glViewport(0, 0, g_state.width, g_state.height);
    glClearColor(0.0f, 0.0f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Use shader-based NV12 rendering
    glUseProgram(g_state.shaderProgram);
    
    // Bind Y texture to unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_state.textureY);
    glUniform1i(glGetUniformLocation(g_state.shaderProgram, "uTexY"), 0);
    
    // Bind UV texture to unit 1
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_state.textureUV);
    glUniform1i(glGetUniformLocation(g_state.shaderProgram, "uTexUV"), 1);
    
    // Reset to unit 0
    glActiveTexture(GL_TEXTURE0);
    
    // Draw fullscreen quad using VAO/VBO for proper shader rendering
    static GLuint vao = 0, vbo = 0;
    if (vao == 0) {
        float vertices[] = {
            // positions   // texcoords
            -1.0f, -1.0f,  0.0f, 1.0f,
             1.0f, -1.0f,  1.0f, 1.0f,
             1.0f,  1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f,  0.0f, 0.0f,
        };
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    }
    
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
    
    glUseProgram(0);
    
    // Unbind textures
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    eglSwapBuffers(g_state.eglDisplay, g_state.eglSurface);
}

bool decodeNextFrame() {
    AVPacket* packet = av_packet_alloc();
    bool gotFrame = false;
    
    while (!gotFrame) {
        int ret = av_read_frame(g_state.formatCtx, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // Seek to beginning
                av_seek_frame(g_state.formatCtx, g_state.videoStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(g_state.codecCtx);
                av_packet_unref(packet);
                continue;
            }
            av_packet_free(&packet);
            return false;
        }
        
        if (packet->stream_index != g_state.videoStreamIdx) {
            av_packet_unref(packet);
            continue;
        }
        
        ret = avcodec_send_packet(g_state.codecCtx, packet);
        av_packet_unref(packet);
        
        if (ret < 0) continue;
        
        ret = avcodec_receive_frame(g_state.codecCtx, g_state.currentFrame);
        if (ret == 0) {
            gotFrame = true;
        }
    }
    
    av_packet_free(&packet);
    
    if (gotFrame) {
        g_state.frameCount++;
        return importVAAPISurface(g_state.currentFrame);
    }
    
    return false;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <video_file>\n", argv[0]);
        return 1;
    }
    
    initTestState();
    
    printf("=== VAAPI EGL Minimal Test ===\n\n");
    printf("Experimental options: forceNewTextures=%s\n\n", 
           g_state.forceNewTextures ? "ON" : "OFF");
    
    if (!initX11()) return 1;
    if (!initEGL()) return 1;
    if (!initVAAPI()) return 1;
    if (!initOpenGL()) return 1;
    if (!openVideo(argv[1])) return 1;
    
    printf("\n=== Starting playback ===\n");
    printf("Press 'q' to quit, 'n' for next frame\n\n");
    
    // Process events and render
    bool running = true;
    while (running) {
        // Handle X11 events
        while (XPending(g_state.display)) {
            XEvent event;
            XNextEvent(g_state.display, &event);
            
            if (event.type == KeyPress) {
                char key;
                XLookupString(&event.xkey, &key, 1, nullptr, nullptr);
                if (key == 'q') running = false;
                if (key == 'n') {
                    if (!decodeNextFrame()) {
                        printf("Failed to decode frame\n");
                    }
                    render();
                }
            } else if (event.type == ConfigureNotify) {
                g_state.width = event.xconfigure.width;
                g_state.height = event.xconfigure.height;
            } else if (event.type == Expose) {
                render();
            }
        }
        
        // Decode and display frames at ~30fps
        static int lastFrameTime = 0;
        int now = time(nullptr);
        if (now != lastFrameTime) {
            lastFrameTime = now;
            if (decodeNextFrame()) {
                render();
            }
        }
        
        usleep(33333);  // ~30fps
    }
    
    printf("\n=== Cleanup ===\n");
    
    // Cleanup
    if (g_state.currentFrame) av_frame_free(&g_state.currentFrame);
    if (g_state.codecCtx) avcodec_free_context(&g_state.codecCtx);
    if (g_state.formatCtx) avformat_close_input(&g_state.formatCtx);
    if (g_state.hwDeviceCtx) av_buffer_unref(&g_state.hwDeviceCtx);
    
    if (g_state.vaDisplay) vaTerminate(g_state.vaDisplay);
    if (g_state.drmFd >= 0) close(g_state.drmFd);
    
    if (g_state.eglContext) {
        eglMakeCurrent(g_state.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(g_state.eglDisplay, g_state.eglContext);
    }
    if (g_state.eglSurface) eglDestroySurface(g_state.eglDisplay, g_state.eglSurface);
    if (g_state.eglDisplay) eglTerminate(g_state.eglDisplay);
    
    if (g_state.display) XCloseDisplay(g_state.display);
    
    printf("Done.\n");
    return 0;
}

