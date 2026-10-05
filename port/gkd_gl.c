/* SPDX-License-Identifier: MIT */
/* See gkd_gl.h. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
/* SDL's own copy of the GLES2 headers: the GKD sysroot has SDL's, not the
 * system's GLES2/, and functions come from SDL_GL_GetProcAddress anyway. */
#define SDL_USE_BUILTIN_OPENGL_DEFINITIONS 1
#include <SDL_opengles2.h>

#include "gkd_gl.h"

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2   /* GLES 3.0 core; gl2.h predates it */
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

/* Every GL entry point this file uses, loaded once from SDL. */
#define GKDGL_FUNCS(X) \
	X(PFNGLGETSTRINGPROC,               glGetString) \
	X(PFNGLCREATESHADERPROC,            glCreateShader) \
	X(PFNGLSHADERSOURCEPROC,            glShaderSource) \
	X(PFNGLCOMPILESHADERPROC,           glCompileShader) \
	X(PFNGLGETSHADERIVPROC,             glGetShaderiv) \
	X(PFNGLGETSHADERINFOLOGPROC,        glGetShaderInfoLog) \
	X(PFNGLDELETESHADERPROC,            glDeleteShader) \
	X(PFNGLCREATEPROGRAMPROC,           glCreateProgram) \
	X(PFNGLATTACHSHADERPROC,            glAttachShader) \
	X(PFNGLBINDATTRIBLOCATIONPROC,      glBindAttribLocation) \
	X(PFNGLLINKPROGRAMPROC,             glLinkProgram) \
	X(PFNGLGETPROGRAMIVPROC,            glGetProgramiv) \
	X(PFNGLGETPROGRAMINFOLOGPROC,       glGetProgramInfoLog) \
	X(PFNGLDELETEPROGRAMPROC,           glDeleteProgram) \
	X(PFNGLUSEPROGRAMPROC,              glUseProgram) \
	X(PFNGLGETUNIFORMLOCATIONPROC,      glGetUniformLocation) \
	X(PFNGLUNIFORM1IPROC,               glUniform1i) \
	X(PFNGLUNIFORM2FPROC,               glUniform2f) \
	X(PFNGLUNIFORMMATRIX4FVPROC,        glUniformMatrix4fv) \
	X(PFNGLGENTEXTURESPROC,             glGenTextures) \
	X(PFNGLDELETETEXTURESPROC,          glDeleteTextures) \
	X(PFNGLBINDTEXTUREPROC,             glBindTexture) \
	X(PFNGLACTIVETEXTUREPROC,           glActiveTexture) \
	X(PFNGLTEXPARAMETERIPROC,           glTexParameteri) \
	X(PFNGLTEXIMAGE2DPROC,              glTexImage2D) \
	X(PFNGLTEXSUBIMAGE2DPROC,           glTexSubImage2D) \
	X(PFNGLPIXELSTOREIPROC,             glPixelStorei) \
	X(PFNGLGENFRAMEBUFFERSPROC,         glGenFramebuffers) \
	X(PFNGLDELETEFRAMEBUFFERSPROC,      glDeleteFramebuffers) \
	X(PFNGLBINDFRAMEBUFFERPROC,         glBindFramebuffer) \
	X(PFNGLFRAMEBUFFERTEXTURE2DPROC,    glFramebufferTexture2D) \
	X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,  glCheckFramebufferStatus) \
	X(PFNGLGENBUFFERSPROC,              glGenBuffers) \
	X(PFNGLDELETEBUFFERSPROC,           glDeleteBuffers) \
	X(PFNGLBINDBUFFERPROC,              glBindBuffer) \
	X(PFNGLBUFFERDATAPROC,              glBufferData) \
	X(PFNGLVERTEXATTRIBPOINTERPROC,     glVertexAttribPointer) \
	X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
	X(PFNGLVERTEXATTRIB4FPROC,          glVertexAttrib4f) \
	X(PFNGLVIEWPORTPROC,                glViewport) \
	X(PFNGLCLEARCOLORPROC,              glClearColor) \
	X(PFNGLCLEARPROC,                   glClear) \
	X(PFNGLDRAWARRAYSPROC,              glDrawArrays) \
	X(PFNGLENABLEPROC,                  glEnable) \
	X(PFNGLDISABLEPROC,                 glDisable) \
	X(PFNGLBLENDFUNCPROC,               glBlendFunc) \
	X(PFNGLCOLORMASKPROC,               glColorMask) \
	X(PFNGLUNIFORM4FPROC,               glUniform4f) \
	X(PFNGLREADPIXELSPROC,              glReadPixels) \
	X(PFNGLGETERRORPROC,                glGetError)

#define X(type, name) static type p_##name;
GKDGL_FUNCS(X)
#undef X

/* The attribute slots every program is linked with. COLOR is a constant
 * white: the shaders read it, nothing here varies it. */
enum { A_VERTEX = 0, A_TEXCOORD = 1, A_COLOR = 2 };

typedef struct {
	unsigned prog;
	int u_mvp, u_dir, u_count, u_out, u_tex, u_in, u_sampler;
	int u_origtex, u_origin, u_origsamp;
	bool linear;
	int  scale;
	/* Its output, when it does not draw into the rect itself. */
	unsigned fbo, tex;
	int tw, th;
} pass;

static pass     g_pass[GKDGL_MAX_PASSES];
static int      g_npass;
static pass     g_copy;        /* None, and the final copy after a scaled last pass */
static bool     g_final_linear;
static unsigned g_vbo;
static unsigned g_src;         /* the frame */
static int      g_src_w, g_src_h;
static diatom_pixfmt g_src_fmt;
static bool     g_bgra;        /* EXT_texture_format_BGRA8888 */
static uint8_t *g_swiz;        /* XRGB8888 -> RGBA when it is missing */
static size_t   g_swiz_cap;
static unsigned g_frame;
static pass     g_ovp, g_fillp;   /* overlay and solid fill, both blended */
static int      g_fill_color;
static unsigned g_ov_tex;
static int      g_ov_w, g_ov_h;

static const char COPY_SRC[] =
	"#if defined(VERTEX)\n"
	"in vec4 VertexCoord; in vec4 TexCoord; out vec2 uv;\n"
	"uniform mat4 MVPMatrix;\n"
	"void main() { gl_Position = MVPMatrix * VertexCoord; uv = TexCoord.xy; }\n"
	"#elif defined(FRAGMENT)\n"
	"precision mediump float;\n"
	"in vec2 uv; out vec4 FragColor; uniform sampler2D Texture;\n"
	"void main() { FragColor = vec4(texture(Texture, uv).rgb, 1.0); }\n"
	"#endif\n";

/* As COPY_SRC, alpha kept: the overlay is drawn blended. */
static const char OVERLAY_SRC[] =
	"#if defined(VERTEX)\n"
	"in vec4 VertexCoord; in vec4 TexCoord; out vec2 uv;\n"
	"uniform mat4 MVPMatrix;\n"
	"void main() { gl_Position = MVPMatrix * VertexCoord; uv = TexCoord.xy; }\n"
	"#elif defined(FRAGMENT)\n"
	"in vec2 uv; out vec4 FragColor; uniform sampler2D Texture;\n"
	"void main() { FragColor = texture(Texture, uv); }\n"
	"#endif\n";

static const char FILL_SRC[] =
	"#if defined(VERTEX)\n"
	"in vec4 VertexCoord; uniform mat4 MVPMatrix;\n"
	"void main() { gl_Position = MVPMatrix * VertexCoord; }\n"
	"#elif defined(FRAGMENT)\n"
	"out vec4 FragColor; uniform vec4 Color;\n"
	"void main() { FragColor = Color; }\n"
	"#endif\n";

/* The shader text minus what would break it as one of two stages: any
 * #version (ours goes first) and `#pragma parameter` lines, which a GLES
 * compiler may refuse. Left undefined, PARAMETER_UNIFORM makes every
 * parameter its #define'd default - the "defaults only" the grill chose. */
static char *clean_source(const char *src)
{
	size_t n = strlen(src);
	char *out = malloc(n + 1), *o = out;
	const char *l = src;

	if (!out) return NULL;
	while (*l) {
		const char *e = strchr(l, '\n');
		size_t len = e ? (size_t)(e - l) + 1 : strlen(l);
		const char *t = l;

		while (*t == ' ' || *t == '\t') t++;
		if (strncmp(t, "#version", 8) && strncmp(t, "#pragma parameter", 17)) {
			memcpy(o, l, len);
			o += len;
		}
		l += len;
	}
	*o = '\0';

	/* mediump to highp, word for word. Nearly every NextUI shader declares
	 * its size uniforms mediump, which Mali runs as 16-bit floats: fine on a
	 * 640-wide screen, not on this 1600x1440 one. scanline.glsl multiplies
	 * OutputSize by TextureSize, overflows 65504 and draws black, and above
	 * 1024 a 16-bit float cannot even name every pixel (plorpos-gkd.72.6).
	 * Padded, so the source keeps its length. */
	for (o = out; (o = strstr(o, "mediump")); o += 7) {
		bool word = (o == out || !(isalnum((unsigned char)o[-1]) || o[-1] == '_')) &&
		            !(isalnum((unsigned char)o[7]) || o[7] == '_');
		if (word) memcpy(o, "highp  ", 7);
	}
	return out;
}

static unsigned compile(unsigned kind, const char *version, const char *stage,
                        const char *body, char *err, size_t errcap)
{
	const char *parts[3] = { version, stage, body };
	unsigned s = p_glCreateShader(kind);
	int ok = 0;

	p_glShaderSource(s, 3, parts, NULL);
	p_glCompileShader(s);
	p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		p_glGetShaderInfoLog(s, sizeof log, NULL, log);
		snprintf(err, errcap, "%.*s: %s", (int)strcspn(stage + 8, "\n"), stage + 8, log);
		p_glDeleteShader(s);
		return 0;
	}
	return s;
}

/* 300 es first, the way the shaders' own __VERSION__ >= 130 branches expect;
 * 100 for one that only builds the old way. */
static unsigned build_program(const char *body, char *err, size_t errcap)
{
	static const char *const versions[] = { "#version 300 es\n", "#version 100\n" };
	size_t i;

	for (i = 0; i < 2; i++) {
		unsigned vs, fs, p;
		int ok = 0;

		vs = compile(GL_VERTEX_SHADER, versions[i], "#define VERTEX\n", body, err, errcap);
		if (!vs) continue;
		/* A default float precision, which GLES requires of a fragment
		 * shader and two of NextUI's (barrel-distortion, scale3x) never
		 * declare. One the shader does declare still wins: it comes later. */
		fs = compile(GL_FRAGMENT_SHADER, versions[i],
		             "#define FRAGMENT\n"
		             "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n"
		             "#else\nprecision mediump float;\n#endif\n",
		             body, err, errcap);
		if (!fs) { p_glDeleteShader(vs); continue; }
		p = p_glCreateProgram();
		p_glAttachShader(p, vs);
		p_glAttachShader(p, fs);
		p_glBindAttribLocation(p, A_VERTEX, "VertexCoord");
		p_glBindAttribLocation(p, A_TEXCOORD, "TexCoord");
		p_glBindAttribLocation(p, A_COLOR, "COLOR");
		p_glLinkProgram(p);
		p_glDeleteShader(vs);
		p_glDeleteShader(fs);
		p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (ok) return p;
		p_glGetProgramInfoLog(p, (int)errcap, NULL, err);
		p_glDeleteProgram(p);
	}
	return 0;
}

static void locate(pass *ps)
{
	ps->u_mvp     = p_glGetUniformLocation(ps->prog, "MVPMatrix");
	ps->u_dir     = p_glGetUniformLocation(ps->prog, "FrameDirection");
	ps->u_count   = p_glGetUniformLocation(ps->prog, "FrameCount");
	ps->u_out     = p_glGetUniformLocation(ps->prog, "OutputSize");
	ps->u_tex     = p_glGetUniformLocation(ps->prog, "TextureSize");
	ps->u_in      = p_glGetUniformLocation(ps->prog, "InputSize");
	ps->u_sampler = p_glGetUniformLocation(ps->prog, "Texture");
	ps->u_origtex = p_glGetUniformLocation(ps->prog, "OrigTextureSize");
	ps->u_origin  = p_glGetUniformLocation(ps->prog, "OrigInputSize");
	ps->u_origsamp = p_glGetUniformLocation(ps->prog, "OrigTexture");
}

static void drop_target(pass *ps)
{
	if (ps->fbo) p_glDeleteFramebuffers(1, &ps->fbo);
	if (ps->tex) p_glDeleteTextures(1, &ps->tex);
	ps->fbo = ps->tex = 0;
	ps->tw = ps->th = 0;
}

static void drop_chain(void)
{
	int i;

	for (i = 0; i < g_npass; i++) {
		drop_target(&g_pass[i]);
		if (g_pass[i].prog) p_glDeleteProgram(g_pass[i].prog);
	}
	memset(g_pass, 0, sizeof g_pass);
	g_npass = 0;
}

bool gkdgl_init(void)
{
	return gkdgl_init_with(SDL_GL_GetProcAddress);
}

bool gkdgl_init_with(void *(*getproc)(const char *name))
{
	/* Two strips over all of clip space, as NextUI draws them
	 * (generic_video.c): its shaders are written for an identity MVPMatrix,
	 * and stock.glsl ignores the matrix outright, so a quad that needed the
	 * matrix to land drew that shader into one corner. The flip lives in
	 * TexCoord instead: vertices 0-3 draw into a texture (v = 0 at the
	 * bottom, row 0), 4-7 onto the screen (v = 0 at the top). */
	static const float quad[] = {
		/* VertexCoord x,y   TexCoord u,v */
		-1, -1,  0, 0,
		 1, -1,  1, 0,
		-1,  1,  0, 1,
		 1,  1,  1, 1,
		-1, -1,  0, 1,
		 1, -1,  1, 1,
		-1,  1,  0, 0,
		 1,  1,  1, 0,
	};
	char err[512];
	const char *ext;

#define X(type, name) \
	if (!(p_##name = (type)getproc(#name))) { \
		fprintf(stderr, "gkdgl: no %s\n", #name); return false; }
	GKDGL_FUNCS(X)
#undef X

	ext = (const char *)p_glGetString(GL_EXTENSIONS);
	g_bgra = ext && strstr(ext, "GL_EXT_texture_format_BGRA8888");

	g_copy.prog = build_program(COPY_SRC, err, sizeof err);
	if (!g_copy.prog) { fprintf(stderr, "gkdgl: copy pass: %s\n", err); return false; }
	locate(&g_copy);
	g_ovp.prog = build_program(OVERLAY_SRC, err, sizeof err);
	g_fillp.prog = build_program(FILL_SRC, err, sizeof err);
	if (!g_ovp.prog || !g_fillp.prog) { fprintf(stderr, "gkdgl: overlay/fill: %s\n", err); return false; }
	locate(&g_ovp);
	locate(&g_fillp);
	g_fill_color = p_glGetUniformLocation(g_fillp.prog, "Color");
	p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	p_glGenBuffers(1, &g_vbo);
	p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	p_glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
	p_glVertexAttribPointer(A_VERTEX, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
	p_glVertexAttribPointer(A_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
	                        (void *)(2 * sizeof(float)));
	p_glEnableVertexAttribArray(A_VERTEX);
	p_glEnableVertexAttribArray(A_TEXCOORD);
	p_glVertexAttrib4f(A_COLOR, 1, 1, 1, 1);

	p_glGenTextures(1, &g_src);
	return true;
}

void gkdgl_shutdown(void)
{
	drop_chain();
	if (g_copy.prog) p_glDeleteProgram(g_copy.prog);
	if (g_ovp.prog) p_glDeleteProgram(g_ovp.prog);
	if (g_fillp.prog) p_glDeleteProgram(g_fillp.prog);
	if (g_ov_tex) p_glDeleteTextures(1, &g_ov_tex);
	memset(&g_ovp, 0, sizeof g_ovp);
	memset(&g_fillp, 0, sizeof g_fillp);
	g_ov_tex = 0;
	if (g_src) p_glDeleteTextures(1, &g_src);
	if (g_vbo) p_glDeleteBuffers(1, &g_vbo);
	free(g_swiz);
	memset(&g_copy, 0, sizeof g_copy);
	g_src = g_vbo = 0;
	g_swiz = NULL;
	g_swiz_cap = 0;
	/* The sizes the textures were allocated at go with them. Kept, the next
	 * init's first upload of the same size skips the allocation and every
	 * pass samples an empty texture: black (plorpos-reo.4.2, the Brick, which
	 * brings the window down and up in one process; the GKD never does). */
	g_src_w = g_src_h = 0;
	g_src_fmt = DIATOM_PIX_RGB565;
	g_ov_w = g_ov_h = 0;
}

bool gkdgl_set_chain(const gkdgl_pass *p, int n, bool final_linear,
                     char *err, size_t errcap)
{
	pass built[GKDGL_MAX_PASSES];
	int i;

	if (n < 0 || n > GKDGL_MAX_PASSES) {
		snprintf(err, errcap, "%d passes (max %d)", n, GKDGL_MAX_PASSES);
		return false;
	}
	memset(built, 0, sizeof built);
	for (i = 0; i < n; i++) {
		char *text = NULL, *body;
		size_t len;

		text = SDL_LoadFile(p[i].path, &len);
		if (!text) { snprintf(err, errcap, "%s: %s", p[i].path, SDL_GetError()); goto fail; }
		body = clean_source(text);
		SDL_free(text);
		if (!body) { snprintf(err, errcap, "%s: out of memory", p[i].path); goto fail; }
		{
			char why[400] = "";
			built[i].prog = build_program(body, why, sizeof why);
			if (!built[i].prog)
				snprintf(err, errcap, "%s: %s", p[i].path, why);
		}
		free(body);
		if (!built[i].prog) goto fail;
		locate(&built[i]);
		built[i].linear = p[i].linear;
		built[i].scale  = p[i].scale;
	}

	drop_chain();
	memcpy(g_pass, built, sizeof built);
	g_npass = n;
	g_final_linear = final_linear;
	return true;

fail:
	while (i-- > 0) if (built[i].prog) p_glDeleteProgram(built[i].prog);
	return false;
}

void gkdgl_upload(const void *src, int w, int h, size_t pitch, diatom_pixfmt fmt)
{
	bool fresh = w != g_src_w || h != g_src_h || fmt != g_src_fmt;
	unsigned ifmt, efmt, type;
	int bpp = fmt == DIATOM_PIX_RGB565 ? 2 : 4;
	const void *pix = src;
	int row = (int)(pitch / (size_t)bpp);

	if (fmt == DIATOM_PIX_RGB565) {
		ifmt = efmt = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5;
	} else if (g_bgra) {
		ifmt = efmt = GL_BGRA_EXT; type = GL_UNSIGNED_BYTE;
	} else {
		/* XRGB8888 is B,G,R,X in memory; GLES without the BGRA extension
		 * takes R,G,B,A only, so swap it here rather than in every shader. */
		size_t need = (size_t)w * (size_t)h * 4;
		int x, y;

		if (need > g_swiz_cap) {
			uint8_t *nb = realloc(g_swiz, need);
			if (!nb) return;
			g_swiz = nb;
			g_swiz_cap = need;
		}
		for (y = 0; y < h; y++) {
			const uint8_t *s = (const uint8_t *)src + (size_t)y * pitch;
			uint8_t *d = g_swiz + (size_t)y * (size_t)w * 4;
			for (x = 0; x < w; x++, s += 4, d += 4) {
				d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
			}
		}
		pix = g_swiz;
		row = w;
		ifmt = efmt = GL_RGBA; type = GL_UNSIGNED_BYTE;
	}

	p_glActiveTexture(GL_TEXTURE0);
	p_glBindTexture(GL_TEXTURE_2D, g_src);
	p_glPixelStorei(GL_UNPACK_ALIGNMENT, bpp == 2 ? 2 : 4);
	p_glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
	if (fresh) {
		p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		p_glTexImage2D(GL_TEXTURE_2D, 0, (int)ifmt, w, h, 0, efmt, type, pix);
		g_src_w = w; g_src_h = h; g_src_fmt = fmt;
	} else {
		p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, efmt, type, pix);
	}
	p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

/* An offscreen target of w x h for this pass, made or remade as needed. */
static bool ensure_target(pass *ps, int w, int h)
{
	if (ps->fbo && ps->tw == w && ps->th == h) return true;
	drop_target(ps);
	p_glGenTextures(1, &ps->tex);
	p_glBindTexture(GL_TEXTURE_2D, ps->tex);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	p_glGenFramebuffers(1, &ps->fbo);
	p_glBindFramebuffer(GL_FRAMEBUFFER, ps->fbo);
	p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ps->tex, 0);
	if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		drop_target(ps);
		return false;
	}
	ps->tw = w; ps->th = h;
	return true;
}

/* One pass: `in` through ps into whatever framebuffer is bound,
 * over the viewport (x, y, w, h).
 *
 * Every texture here holds the image top row first, the way the core wrote
 * it. Into a texture, v = 0 goes to the bottom of the target (row 0); onto
 * the screen, to the top - the quad says which, not MVPMatrix - so the
 * picture is upright on glass and nothing flips in between. */
static void run(const pass *ps, unsigned in, bool linear,
                int x, int y, int w, int h, bool to_screen)
{
	static const float mvp[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
	int f = linear ? GL_LINEAR : GL_NEAREST;

	p_glViewport(x, y, w, h);
	p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, to_screen ? GL_FALSE : GL_TRUE);
	p_glUseProgram(ps->prog);
	p_glActiveTexture(GL_TEXTURE0);
	p_glBindTexture(GL_TEXTURE_2D, in);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
	if (ps->u_mvp >= 0)     p_glUniformMatrix4fv(ps->u_mvp, 1, GL_FALSE, mvp);
	if (ps->u_dir >= 0)     p_glUniform1i(ps->u_dir, 1);
	if (ps->u_count >= 0)   p_glUniform1i(ps->u_count, (int)g_frame);
	if (ps->u_out >= 0)     p_glUniform2f(ps->u_out, (float)w, (float)h);
	/* The frame's size for every pass, not the pass's own input: NextUI's
	 * presets are all srctype/scaletype "source", and lcd3x after pixellate
	 * is drawing a grid per GAME pixel, not per pixel of what it samples
	 * (plorpos-gkd.72.6). TexCoord still spans the whole input. */
	if (ps->u_tex >= 0)     p_glUniform2f(ps->u_tex, (float)g_src_w, (float)g_src_h);
	if (ps->u_in >= 0)      p_glUniform2f(ps->u_in, (float)g_src_w, (float)g_src_h);
	if (ps->u_sampler >= 0) p_glUniform1i(ps->u_sampler, 0);
	if (ps->u_origtex >= 0) p_glUniform2f(ps->u_origtex, (float)g_src_w, (float)g_src_h);
	if (ps->u_origin >= 0)  p_glUniform2f(ps->u_origin, (float)g_src_w, (float)g_src_h);
	/* The core's frame itself on unit 1, whatever this pass's input is: a
	 * shader that is a chain's last pass - Pixel Transparency after LCD 3x -
	 * reads the raw frame through OrigTexture (plorpos-gkd.86.1). Left unset
	 * it was unit 0, the previous pass. */
	if (ps->u_origsamp >= 0) {
		p_glActiveTexture(GL_TEXTURE1);
		p_glBindTexture(GL_TEXTURE_2D, g_src);
		p_glActiveTexture(GL_TEXTURE0);
		p_glUniform1i(ps->u_origsamp, 1);
	}
	p_glDrawArrays(GL_TRIANGLE_STRIP, to_screen ? 4 : 0, 4);
}

void gkdgl_draw(int sw, int sh, diatom_rect dst, bool none_linear)
{
	/* GL's window origin is bottom-left; diatom_rect's is top-left. */
	int gx = dst.x, gy = sh - dst.y - dst.h;
	unsigned in = g_src;
	int iw = g_src_w, ih = g_src_h, i;

	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	p_glViewport(0, 0, sw, sh);
	p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	p_glClearColor(0, 0, 0, 1);
	p_glClear(GL_COLOR_BUFFER_BIT);
	if (!g_src_w || dst.w <= 0 || dst.h <= 0) return;

	if (!g_npass) {
		run(&g_copy, in, none_linear, gx, gy, dst.w, dst.h, true);
		g_frame++;
		return;
	}

	for (i = 0; i < g_npass; i++) {
		pass *ps = &g_pass[i];
		bool last = i == g_npass - 1;
		int ow = ps->scale ? iw * ps->scale : dst.w;
		int oh = ps->scale ? ih * ps->scale : dst.h;

		if (last && !ps->scale) {
			p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
			run(ps, in, ps->linear, gx, gy, dst.w, dst.h, true);
			g_frame++;
			return;
		}
		if (!ensure_target(ps, ow, oh)) return;
		p_glBindFramebuffer(GL_FRAMEBUFFER, ps->fbo);
		run(ps, in, ps->linear, 0, 0, ow, oh, false);
		in = ps->tex; iw = ow; ih = oh;
	}

	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	run(&g_copy, in, g_final_linear, gx, gy, dst.w, dst.h, true);
	g_frame++;
}

/* XRGB8888 and the overlay's BGRA share a layout; this uploads either into
 * `tex`, swapping bytes when the BGRA extension is missing. */
static void upload_bgra(unsigned tex, const uint8_t *bgra, int w, int h, bool fresh)
{
	const void *pix = bgra;
	unsigned fmt = GL_BGRA_EXT;

	if (!g_bgra) {
		size_t need = (size_t)w * (size_t)h * 4, i;

		if (need > g_swiz_cap) {
			uint8_t *nb = realloc(g_swiz, need);
			if (!nb) return;
			g_swiz = nb;
			g_swiz_cap = need;
		}
		for (i = 0; i < need; i += 4) {
			g_swiz[i] = bgra[i + 2]; g_swiz[i + 1] = bgra[i + 1];
			g_swiz[i + 2] = bgra[i]; g_swiz[i + 3] = bgra[i + 3];
		}
		pix = g_swiz;
		fmt = GL_RGBA;
	}
	p_glActiveTexture(GL_TEXTURE0);
	p_glBindTexture(GL_TEXTURE_2D, tex);
	p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	if (fresh) {
		p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		p_glTexImage2D(GL_TEXTURE_2D, 0, (int)fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pix);
	} else {
		p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, pix);
	}
}

void gkdgl_overlay_set(const uint8_t *bgra, int w, int h)
{
	if (!bgra || w <= 0 || h <= 0) { g_ov_w = g_ov_h = 0; return; }
	if (!g_ov_tex) p_glGenTextures(1, &g_ov_tex);
	upload_bgra(g_ov_tex, bgra, w, h, w != g_ov_w || h != g_ov_h);
	g_ov_w = w;
	g_ov_h = h;
}

void gkdgl_overlay_draw(int sw, int sh, diatom_rect r)
{
	if (!g_ov_w) return;
	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	p_glEnable(GL_BLEND);
	run(&g_ovp, g_ov_tex, false, r.x, sh - r.y - r.h, r.w, r.h, true);
	p_glDisable(GL_BLEND);
	(void)sw;
}

void gkdgl_fill(int sw, int sh, diatom_rect r, uint8_t red, uint8_t green,
                uint8_t blue, uint8_t alpha)
{
	if (r.w <= 0 || r.h <= 0) return;
	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (alpha < 255) p_glEnable(GL_BLEND);
	p_glViewport(r.x, sh - r.y - r.h, r.w, r.h);
	p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	p_glUseProgram(g_fillp.prog);
	if (g_fillp.u_mvp >= 0) {
		static const float mvp[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		p_glUniformMatrix4fv(g_fillp.u_mvp, 1, GL_FALSE, mvp);
	}
	p_glUniform4f(g_fill_color, red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f);
	p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	p_glDisable(GL_BLEND);
	(void)sw;
}

bool gkdgl_read(int sw, int sh, uint8_t *rgba)
{
	size_t row = (size_t)sw * 4;
	uint8_t *tmp;
	int y;

	while (p_glGetError() != GL_NO_ERROR) { }
	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	p_glPixelStorei(GL_PACK_ALIGNMENT, 4);
	p_glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	if (p_glGetError() != GL_NO_ERROR) return false;
	/* GL reads bottom row first. */
	tmp = malloc(row);
	if (!tmp) return false;
	for (y = 0; y < sh / 2; y++) {
		uint8_t *a = rgba + (size_t)y * row, *b = rgba + (size_t)(sh - 1 - y) * row;
		memcpy(tmp, a, row); memcpy(a, b, row); memcpy(b, tmp, row);
	}
	free(tmp);
	return true;
}
