// xr_bridge.dll - S2-B/S3/S4 in-process render bridge (LOCAL BUILD, NEVER SHIPPED).
//
// Loaded by Mine-imator.exe via Library/XrBridgeLoader.cpp when placed next
// to the executable. Renders the export frames in-process through the
// tile_exp xgraphics.lib (E-R1 core profile + E-R4 readback + GPU merge +
// XAA), receiving MI's live CPU vertex buffers by pointer - geometry never
// touches disk. S3: frames are handed to the in-process encoder.
// S4: per-mesh UVs + textures via the backend's side channel.

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <array>

#include <gl/GL.h>
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CA5
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif

#include "xraphics/renderer/opengl/OpenGLBackend.h"
#include "xraphics/renderer/XrPlatformWin32.h"

#define XRB_API extern "C" __declspec(dllexport)

// S4: the tile_exp backend's per-mesh UV/texture side channel + debug counters.
extern void XrTileSetMeshData(const void* verts, const float* uvs, uint32_t count, uint64_t texId);
extern void XrTileSetBackground(int skySet, float sr, float sg, float sb,
	int imgActive, unsigned imgTex, float ux, float uy, float uw, float uh);
extern uint64_t XrTileDebugHits();
extern uint64_t XrTileDebugMiss();
extern uint64_t XrTileDebugTexBinds();
extern uint64_t XrTileDebugRegistered();
extern uint64_t XrTileDebugCore();
extern void XrTileSetMeshData(const void* verts, const float* uvs, uint32_t count, uint64_t texId, float alpha, float emissive);
extern void XrTileSetLights(int amount, const float* data, float sx, float sy, float sz,
	float ar, float ag, float ab, int tonemapper, float exposure, float gamma);
extern void XrTileSetDefaultEmissive(float v); // S38

struct QueuedDraw {
	std::vector<MeshRasterVertex> verts;
	std::vector<uint32_t> idx;
	std::vector<float> uvs;  // S4: 2 floats per vertex
	uint64_t texId = 0;      // S4: 0 = untextured
	uint64_t texFor = 0;     // S4v diag: set_tex_for id (names the tls)
	float alpha = 1.0f;      // S4v: tl value_inherit[ALPHA]
	float emissive = 0.0f;   // S38: tl value_inherit[EMISSIVE]
	bool glow = false;       // S56: tl.glow
	bool onlyGlow = false;   // S56: tl.only_render_glow
	float glowColor[3];      // S56: tl.value_inherit[GLOW_COLOR]
	float world[16];        // column-major, already S*M*S permuted
	uint32_t cookie = 0;    // S50: stable per-content cache cookie (backend VBO cache)
};

static int frameIndexTotal = 0;
static int s34DumpFrame = -1; // S34: compact per-draw dump gate (env)
// S56t: current EXPORT frame index (end_frame param), readable from the
// draw-submission path (which only sees frameIndexTotal).
static int s_exportFrame = -1;
// S56x DIAG: first two RAW (pre-rect) UVs of the current draw + the mix
// color state, printed by the S56f truth dump.
static float s_diagRawU[2] = { 0, 0 }, s_diagRawV[2] = { 0, 0 };
static HWND s_hwnd = nullptr;
static OpenGLBackend* s_backend = nullptr;
static int s_W = 0, s_H = 0;
static std::string s_outDir;
static std::vector<QueuedDraw> s_queue;
static float s_world[16], s_shape[16];
static bool s_hasCam = false;
static bool s_framePending = false;   // end_frame rendered, pixels not yet consumed
static bool s_bmpsEnabled = false;    // XR_BRIDGE_BMPS=1 keeps the BMP side output
static double s_fov = 60, s_camF[3], s_camT[3], s_camU[3];

// S4: texture registry (id -> GL texture) + current draw texture + uvRect.
static std::unordered_map<uint64_t, GLuint> s_tex;
static uint64_t s_curTexId = 0;
static double s_uvX = 0, s_uvY = 0, s_uvW = 1, s_uvH = 1;
// S4b: sprite->page UV rect applied to the current draw's vertex UVs
// (vbuffer UVs are sprite-relative 0-1; the rect maps them onto the
// bound texture, replicating the native shader's _uVvRect transform).
static double s_curUx = 0, s_curUy = 0, s_curUw = 1, s_curUh = 1;
static uint64_t s_statWhiteBinds = 0, s_statRealBinds = 0;
// S4c DIAG: retained RGBA copies (standalone <=4MB) + frame-900 UV coverage grids.
static std::unordered_map<uint64_t, std::vector<uint8_t>> s_diagTexData;
static std::unordered_map<uint64_t, std::array<uint64_t, 16>> s_diagUvGrid;
static std::unordered_map<uint64_t, std::pair<int, int>> s_diagTexSize;

// S4t: native background state (sky clear color + optional full-screen image).
static bool s_bgSkySet = false;
static float s_bgSky[3] = { 0, 0, 0 };
static bool s_bgImgActive = false;
static uint64_t s_bgImgKey = 0;
static double s_bgUx = 0, s_bgUy = 0, s_bgUw = 1, s_bgUh = 1;
// S4v: current tl alpha (value_inherit[ALPHA]), applied to subsequent draws.
static float s_drawAlpha = 1.0f;
// S29: current tl/shape blend tint (native uBlendColor rgb, 0-1). Baked into
// the vertex colors of subsequent draws in draw_mesh.
static float s_drawTint[3] = { 1.0f, 1.0f, 1.0f };
// S30: current tl color mix (native uMixColor/uMixPercent): the final vertex
// rgb = lerp(tinted rgb, mixColor, mixPercent).
static float s_drawMix[3] = { 0.0f, 0.0f, 0.0f };
static float s_drawMixPercent = 0.0f;
// S38: current tl emissive (native uEmissive = value_inherit[EMISSIVE],
// render_world_tl L119-123). max()'d with the vertex-baked emissive.
static float s_drawEmissive = 0.0f;
// S56: current tl glow state (native uGlow / only_render_glow / uGlowColor).
static float s_drawGlow = 0.0f;
static float s_drawOnlyGlow = 0.0f;
static float s_drawGlowTex = 0.0f;
static float s_drawGlowColor[3] = { 0.0f, 0.0f, 0.0f };
// S4v diag: last texture-registry id requested via set_tex_for (tl id or
// shape instance id) - names the tls behind each queued draw.
static uint64_t s_lastTexFor = 0;

// MI (Z-up) -> mesh path (Y-up): S*M*S = swap rows/cols 1<->2.
static void SwapYZMatrix(const float* m, float* out) {
	static const int s[4] = { 0, 2, 1, 3 };
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			out[c + 4 * r] = m[s[c] + 4 * s[r]];
}

// S46: per-frame component profile (env XFAWA_FRAME_PROFILE=1). Accumulated
// wall-ms per component, flushed as one line per frame into xr_timing.txt.
static int sProf = -1;          // gate
static double sProfDraw = 0;    // draw_mesh: per-vertex conversion+swap+swizzle loop
static double sProfBake = 0;    // draw_mesh: S20 CPU world bake loop
static double sProfReg = 0;     // end_frame: XrTileSetMeshData registrations
static double sProfRender = 0;  // end_frame: RenderMeshesX (incl. backend upload/readback)
static double sProfGetPx = 0;   // end_frame: GetMeshPixels (sync stall part)
static double sProfSwiz = 0;    // read_frame: per-pixel BGRA->RGBA swizzle
static LARGE_INTEGER sProfFreq = {};
static bool ProfOn() {
	if (sProf < 0) {
		const char* e = getenv("XFAWA_FRAME_PROFILE"); sProf = (e && atoi(e) == 1) ? 1 : 0;
		if (sProf == 1) QueryPerformanceFrequency(&sProfFreq);
	}
	return sProf == 1;
}
static double QpcMsSince(const LARGE_INTEGER& t0) {
	LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
	return (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)sProfFreq.QuadPart;
}
// S47: shared buffered timing stream (begin_frame timestamps + end_frame rows).
static FILE* s_timing = nullptr;
static FILE* TimingFile() {
	if (!s_timing) {
		char tp[MAX_PATH];
		snprintf(tp, sizeof(tp), "%s\\xr_timing.txt", s_outDir.c_str());
		s_timing = fopen(tp, "ab");
		if (s_timing) setvbuf(s_timing, nullptr, _IOFBF, 65536);
	}
	return s_timing;
}

// S4f DIAG: tracing window procedure (env XFAWA_WND_TRACE=1). Logs size-related
// messages and forwards everything unchanged; no message results are altered.
static FILE* g_wndTrace = nullptr;
static WNDPROC g_origProc = nullptr;
static LRESULT CALLBACK XrbWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	if (g_wndTrace) {
		bool hit = false;
		char b[192] = "";
		switch (msg) {
		case WM_SIZE: {
			RECT r = {}, c = {};
			GetWindowRect(hwnd, &r); GetClientRect(hwnd, &c);
			snprintf(b, sizeof(b), " client=(%d,%d) wp=%u wnd=(%ld,%ld,%ld,%ld)",
				(int)(short)LOWORD(lp), (int)(short)HIWORD(lp), (unsigned)wp,
				r.left, r.top, r.right, r.bottom);
			hit = true; break;
		}
		case WM_SIZING: {
			const RECT* sr = (const RECT*)lp;
			snprintf(b, sizeof(b), " [USER-SIZING] rect=(%ld,%ld,%ld,%ld) edge=%u",
				sr->left, sr->top, sr->right, sr->bottom, (unsigned)wp);
			hit = true; break;
		}
		case WM_WINDOWPOSCHANGING: case WM_WINDOWPOSCHANGED: {
			const WINDOWPOS* wpos = (const WINDOWPOS*)lp;
			RECT r = {}, c = {};
			GetWindowRect(hwnd, &r); GetClientRect(hwnd, &c);
			snprintf(b, sizeof(b), " pos=(%d,%d,%d,%d) flags=0x%X after=%p wnd=(%ld,%ld,%ld,%ld)",
				wpos->x, wpos->y, wpos->cx, wpos->cy, wpos->flags,
				(void*)wpos->hwndInsertAfter, r.left, r.top, r.right, r.bottom);
			hit = true; break;
		}
		case WM_ENTERSIZEMOVE: snprintf(b, sizeof(b), " [USER-MODAL-BEGIN]"); hit = true; break;
		case WM_EXITSIZEMOVE: snprintf(b, sizeof(b), " [USER-MODAL-END]"); hit = true; break;
		case WM_DPICHANGED: snprintf(b, sizeof(b), " [DPI] wp=%u", (unsigned)wp); hit = true; break;
		case WM_DISPLAYCHANGE: snprintf(b, sizeof(b), " [DISPLAYCHANGE]"); hit = true; break;
		case WM_CLOSE: case WM_DESTROY: snprintf(b, sizeof(b), " [LIFECYCLE]"); hit = true; break;
		default: break;
		}
		if (hit) {
			fprintf(g_wndTrace, "tick=%llu msg=0x%04X%s dc=%p thread=%u\r\n",
				(unsigned long long)GetTickCount64(), msg, b,
				(void*)wglGetCurrentDC(), GetCurrentThreadId());
			fflush(g_wndTrace);
		}
	}
	return g_origProc ? CallWindowProcA(g_origProc, hwnd, msg, wp, lp)
	                 : DefWindowProcA(hwnd, msg, wp, lp);
}

static void SaveBmp(const char* path, const uint32_t* px, int w, int h) {
	FILE* f = fopen(path, "wb");
	if (!f) return;
	const uint32_t rowBytes = (uint32_t)w * 4, imgBytes = rowBytes * (uint32_t)h;
	uint8_t fhdr[14] = { 'B','M',0,0,0,0,0,0,0,0,54,0,0,0 };
	const uint32_t fileSize = 54 + imgBytes;
	fhdr[2] = (uint8_t)(fileSize & 0xFF); fhdr[3] = (uint8_t)((fileSize >> 8) & 0xFF);
	fhdr[4] = (uint8_t)((fileSize >> 16) & 0xFF); fhdr[5] = (uint8_t)((fileSize >> 24) & 0xFF);
	fwrite(fhdr, 1, 14, f);
	uint8_t ihdr[40] = { 0 };
	const uint32_t biSize = 40;
	memcpy(ihdr, &biSize, 4); memcpy(ihdr + 4, &w, 4); memcpy(ihdr + 8, &h, 4);
	const uint16_t planes = 1, bpp = 32;
	memcpy(ihdr + 12, &planes, 2); memcpy(ihdr + 14, &bpp, 2);
	memcpy(ihdr + 20, &imgBytes, 4);
	fwrite(ihdr, 1, 40, f);
	// S53: meshPixels_ bytes are R,G,B,A (GL_RGBA readback); BMP wants
	// B,G,R,A - swap R/B per pixel. S53b: meshPixels_ rows are BOTTOM-UP
	// (the backend flip moved into read_frame), and BMP rows are bottom-up
	// too - so row y maps directly.
	std::vector<uint8_t> row(rowBytes);
	for (int y = 0; y < h; y++) {
		const uint8_t* src = (const uint8_t*)px + (size_t)y * rowBytes;
		for (int x = 0; x < w; x++) {
			row[x * 4 + 0] = src[x * 4 + 2];
			row[x * 4 + 1] = src[x * 4 + 1];
			row[x * 4 + 2] = src[x * 4 + 0];
			row[x * 4 + 3] = src[x * 4 + 3];
		}
		fwrite(row.data(), 1, rowBytes, f);
	}
	fclose(f);
}

// ============================ ABI ============================



XRB_API int xr_bridge_init(int w, int h, const char* outDirUtf8)
{
	if (w < 16 || h < 16 || w > 7680 || h > 4320) return 0;
	if (!outDirUtf8 || !outDirUtf8[0]) return 0;

	// Renderer feature env (the receiver used the same settings via shell).
	// XFAWA_MESH_AA: pre-set env wins; default = none (XAA is opt-in via bat).
	{
		const char* aa = getenv("XFAWA_MESH_AA");
		if (!aa || !aa[0]) SetEnvironmentVariableA("XFAWA_MESH_AA", "none");
	}
	SetEnvironmentVariableA("XFAWA_GL_CORE", "1");
	SetEnvironmentVariableA("XFAWA_NO_VSYNC", "1");
	// S38 FIX (stretch bug): render meshes into an exact-size offscreen FBO
	// and read back from it. Without this the meshes render into the MI
	// window surface, which is smaller than the 4K export size - the
	// readback then contains clipped/garbled geometry (the "stretched fist").
	// The offscreen FBO costs ~0 extra (same draws, same readback volume).
	// NOTE: _putenv, NOT SetEnvironmentVariableA - the lib reads this via
	// getenv (the CRT snapshot taken at dll load), which Win32-level env
	// changes do not reach.
	_putenv("XFAWA_FBO_TEST=1");
	SetEnvironmentVariableA("XFAWA_FBO_TEST", "1");

	s_W = w; s_H = h;
	s_outDir = outDirUtf8;
	CreateDirectoryA(s_outDir.c_str(), nullptr);
	{
		const char* bm = getenv("XR_BRIDGE_BMPS");
		s_bmpsEnabled = (bm && atoi(bm) != 0);
	}

	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "XrBridgeWnd";
	RegisterClassA(&wc);
	s_hwnd = CreateWindowExA(0, "XrBridgeWnd", "xr_bridge", WS_OVERLAPPEDWINDOW,
		20, 20, w, h, nullptr, nullptr, wc.hInstance, nullptr);
	if (!s_hwnd) return 0;
	// S4f DIAG: window message trace (env XFAWA_WND_TRACE=1, default off).
	// Behavior-neutral subclass: logs size-related messages with full rects
	// then forwards everything to the original procedure. WM_SIZING /
	// WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE presence = user-drag signature;
	// their absence with WM_WINDOWPOSCHANGED = programmatic resize.
	{
		const char* wt = getenv("XFAWA_WND_TRACE");
		if (wt && atoi(wt) != 0) {
			char tp[MAX_PATH];
			snprintf(tp, sizeof(tp), "%s\\wnd_trace.txt", outDirUtf8);
			g_wndTrace = fopen(tp, "w");
			if (g_wndTrace) {
				RECT r = {}, c = {};
				GetWindowRect(s_hwnd, &r); GetClientRect(s_hwnd, &c);
				fprintf(g_wndTrace, "created hwnd=%p wnd=(%ld,%ld,%ld,%ld) client=(%ld,%ld,%ld,%ld) thread=%u tick=%llu\r\n",
					(void*)s_hwnd, r.left, r.top, r.right, r.bottom,
					c.left, c.top, c.right, c.bottom, GetCurrentThreadId(),
					(unsigned long long)GetTickCount64());
				fflush(g_wndTrace);
				g_origProc = (WNDPROC)SetWindowLongPtrA(s_hwnd, GWLP_WNDPROC, (LONG_PTR)XrbWndProc);
				fprintf(g_wndTrace, "trace hook installed (orig=%p)\r\n", (void*)g_origProc);
				fflush(g_wndTrace);
			}
		}
	}

	static OpenGLBackend backend;
	RenderConfig cfg; cfg.width = w; cfg.height = h; cfg.enableProfiling = false;
	if (!backend.Init(XrWrapWindowHandle(s_hwnd), cfg)) return 0;
	s_backend = &backend;

	for (int i = 0; i < 16; i++) { s_world[i] = 0; s_shape[i] = 0; }
	s_world[0] = s_world[5] = s_world[10] = s_world[15] = 1.0f;
	s_shape[0] = s_shape[5] = s_shape[10] = s_shape[15] = 1.0f;

	// S4: 1x1 WHITE fallback texture at GL name 1 (untextured items sample
	// this; the shader always samples - no uUseTex branch to fail).
	{
		GLuint wt = 0;
		glGenTextures(1, &wt);
		glBindTexture(GL_TEXTURE_2D, wt);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		const uint8_t white[4] = { 255, 255, 255, 255 };
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
		s_tex[1] = wt; // GL name 1 reserved for the white fallback
	}

	// S4 debug: which context did we actually get?
	{
		const char* ver = (const char*)glGetString(GL_VERSION);
		const char* ren = (const char*)glGetString(GL_RENDERER);
		char dbg[220];
		snprintf(dbg, sizeof(dbg), "GL_VERSION=%s RENDERER=%s\r\n", ver ? ver : "?", ren ? ren : "?");
		OutputDebugStringA(dbg);
		char lp[MAX_PATH];
		snprintf(lp, sizeof(lp), "%s\\xr_bridge_log.txt", s_outDir.c_str());
		FILE* lf = fopen(lp, "ab");
		if (lf) { fputs(dbg, lf); fclose(lf); }
	}
	return 1;
}

// S4l diag: gated logging (first 3 frames, every 300th, frame 900) to the
// bridge sidecar. Read-only probes; no render behavior change.
static bool XrDiagGate() {
	return frameIndexTotal < 3 || frameIndexTotal % 300 == 0 || frameIndexTotal == 420 || frameIndexTotal == 450 || frameIndexTotal == 900 || frameIndexTotal == 810 || frameIndexTotal == 330;
}
static void XrDiagLine(const char* s) {
	char lp[MAX_PATH];
	snprintf(lp, sizeof(lp), "%s\\xr_bridge_log.txt", s_outDir.c_str());
	FILE* lf = fopen(lp, "ab");
	if (lf) { fputs(s, lf); fputs("\r\n", lf); fclose(lf); }
}

XRB_API int xr_bridge_begin_frame(double fov, double fx, double fy, double fz,
	double tx, double ty, double tz, double ux, double uy, double uz)
{
	if (s34DumpFrame < 0) {
	const char* e = getenv("XFAWA_MESH_DUMP_FRAME");
	s34DumpFrame = (e && *e) ? atoi(e) : -1;
	}
	if (!s_backend) return 0;
	s_fov = fov;
	s_camF[0] = fx; s_camF[1] = fy; s_camF[2] = fz;
	s_camT[0] = tx; s_camT[1] = ty; s_camT[2] = tz;
	s_camU[0] = ux; s_camU[1] = uy; s_camU[2] = uz;
	s_hasCam = true;
	s_queue.clear();
	s_bgImgActive = false; // S4t: image re-armed each frame by set_bg_image
	s_drawAlpha = 1.0f;    // S29: reset per-frame (the GML re-arms per tl)
	s_drawTint[0] = s_drawTint[1] = s_drawTint[2] = 1.0f;
	s_drawMix[0] = s_drawMix[1] = s_drawMix[2] = 0.0f;
	s_drawMixPercent = 0.0f;
	s_drawEmissive = 0.0f; // S38
	s_drawGlow = 0.0f; s_drawOnlyGlow = 0.0f; s_drawGlowTex = 0.0f; // S56
	s_drawGlowColor[0] = s_drawGlowColor[1] = s_drawGlowColor[2] = 0.0f; // S56
	// S47: per-frame begin timestamp (raw QPC) - lets the offline analysis
	// split exe-side wall (scene update + encode) from dll-side time.
	{
		LARGE_INTEGER ts;
		QueryPerformanceCounter(&ts);
		if (!sProfFreq.QuadPart) QueryPerformanceFrequency(&sProfFreq);
		if (TimingFile()) {
			char tb[64];
			snprintf(tb, sizeof(tb), "ts f%d %lld\r\n", frameIndexTotal, (long long)ts.QuadPart);
			fputs(tb, s_timing);
		}
	}
	if (XrDiagGate()) {
		char dbg[320];
		snprintf(dbg, sizeof(dbg),
			"diagcam f=%d fov=%.4f from=(%.4f,%.4f,%.4f) to=(%.4f,%.4f,%.4f) up=(%.4f,%.4f,%.4f)",
			frameIndexTotal, fov, fx, fy, fz, tx, ty, tz, ux, uy, uz);
		XrDiagLine(dbg);
	}
	if (XrDiagGate() && !s_queue.empty()) {
		// S4o DIAG: local-vertex position bounds of the first queued draw
		// (post Y/Z swap, pre-matrix). Degenerate bounds = extraction bug;
		// sane bounds + black frame = transform path bug.
		const QueuedDraw& q0 = s_queue[0];
		float bx0 = 1e30f, by0 = 1e30f, bz0 = 1e30f, bx1 = -1e30f, by1 = -1e30f, bz1 = -1e30f;
		int bn = (int)q0.verts.size(); if (bn > 256) bn = 256;
		for (int v = 0; v < bn; v++) {
			const MeshRasterVertex& vv = q0.verts[v];
			if (vv.px < bx0) bx0 = vv.px; if (vv.px > bx1) bx1 = vv.px;
			if (vv.py < by0) by0 = vv.py; if (vv.py > by1) by1 = vv.py;
			if (vv.pz < bz0) bz0 = vv.pz; if (vv.pz > bz1) bz1 = vv.pz;
		}
		char dbg[220];
		snprintf(dbg, sizeof(dbg),
			"diagverts f=%d n=%d x=[%.3f,%.3f] y=[%.3f,%.3f] z=[%.3f,%.3f]",
			frameIndexTotal, bn, bx0, bx1, by0, by1, bz0, bz1);
		XrDiagLine(dbg);
	}
	return 1;
}

XRB_API void xr_bridge_set_matrix(const double* m) {
	for (int i = 0; i < 16; i++) s_world[i] = (float)m[i];
}

XRB_API void xr_bridge_set_shape_matrix(const double* m) {
	for (int i = 0; i < 16; i++) s_shape[i] = (float)m[i];
}

// S38: native per-tl emissive (uEmissive = value_inherit[EMISSIVE]).
// max()'d with the vertex-baked emissive in draw_mesh.
XRB_API void xr_bridge_set_draw_emissive(double v) {
	s_drawEmissive = (v < 0.0) ? 0.0f : (float)v;
}

// S50: content-hash conversion cache. Key = FNV-1a over the raw exe vertex+
// index bytes AND every conversion input (tint, mix, alpha, uv rect). With
// CPU bake OFF the conversion is a pure function of these, so identical
// scene content across frames hits the cache: the per-vertex decode is
// skipped and the backend cookie stays stable so the per-cookie VBO cache
// skips the re-upload entirely (cookie reuse is never mixed with content
// change: entries are immortal for the process lifetime).
struct XrMeshCacheEntry {
	std::vector<MeshRasterVertex> verts;
	std::vector<uint32_t> idx;
	std::vector<float> uvs;
	uint32_t cookie = 0;
};
static std::unordered_map<uint64_t, std::shared_ptr<XrMeshCacheEntry>> s_meshCache;
static uint32_t s_meshCookieSeq = 0;
static uint64_t s_meshCacheHits = 0, s_meshCacheMisses = 0;
static inline void XrHashU32(uint64_t& h, uint32_t v) { h ^= v; h *= 1099511628211ULL; }
static inline uint64_t XrHashBytes(uint64_t h, const void* p, size_t bytes) {
	const uint32_t* p32 = (const uint32_t*)p;
	const size_t n32 = bytes / 4;
	for (size_t i = 0; i < n32; i++) XrHashU32(h, p32[i]);
	const uint8_t* pb = (const uint8_t*)p + n32 * 4;
	for (size_t i = 0; i < bytes - n32 * 4; i++) { h ^= pb[i]; h *= 1099511628211ULL; }
	return h;
}

XRB_API void xr_bridge_draw_mesh(const void* vdata, int numVerts, int stride,
	const void* idata, int numIndices, int isShape)
{
	if (!s_backend || !vdata || numVerts <= 0 || stride < 36) return;
	static int sCpuBake = -1;
	if (sCpuBake < 0) { const char* e = getenv("XFAWA_CPU_BAKE"); sCpuBake = (e && atoi(e) == 0) ? 0 : 1; } // S52b: default ON - the GPU model-matrix path renders BLACK (the backend shader ignores the per-draw model matrix); S20's bake was the fix, not a diagnostic.

	QueuedDraw q;
	const bool profDraw = ProfOn();
	LARGE_INTEGER pT0 = {};
	if (profDraw) QueryPerformanceCounter(&pT0);

	// S52b: compose the model matrix FIRST - with bake ON the conversion
	// output is world-space, so the cache key must cover the matrix too.
	float prod[16], fin[16];
	if (isShape) {
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) {
				float acc = 0;
				for (int k = 0; k < 4; k++) acc += s_world[k + 4 * r] * s_shape[c + 4 * k];
				prod[c + 4 * r] = acc;
			}
	} else {
		memcpy(prod, s_world, sizeof(prod));
	}
	SwapYZMatrix(prod, fin);

	uint64_t hsh = 1469598103934665603ULL;
	XrMeshCacheEntry* hit = nullptr;
	{
		hsh = XrHashBytes(hsh, vdata, (size_t)numVerts * (size_t)stride);
		if (idata && numIndices > 0) hsh = XrHashBytes(hsh, idata, (size_t)numIndices * 4);
		XrHashU32(hsh, (uint32_t)numVerts); XrHashU32(hsh, (uint32_t)numIndices);
		XrHashU32(hsh, (uint32_t)stride);
		for (int k = 0; k < 3; k++) { uint32_t t; memcpy(&t, &s_drawTint[k], 4); XrHashU32(hsh, t); }
		for (int k = 0; k < 3; k++) { uint32_t t; memcpy(&t, &s_drawMix[k], 4); XrHashU32(hsh, t); }
		{ uint32_t t; memcpy(&t, &s_drawMixPercent, 4); XrHashU32(hsh, t); }
		{ uint32_t t; memcpy(&t, &s_drawAlpha, 4); XrHashU32(hsh, t); }
		{ uint64_t t; memcpy(&t, &s_curUx, 8); hsh ^= t; hsh *= 1099511628211ULL; }
		{ uint64_t t; memcpy(&t, &s_curUy, 8); hsh ^= t; hsh *= 1099511628211ULL; }
		{ uint64_t t; memcpy(&t, &s_curUw, 8); hsh ^= t; hsh *= 1099511628211ULL; }
		{ uint64_t t; memcpy(&t, &s_curUh, 8); hsh ^= t; hsh *= 1099511628211ULL; }
		if (sCpuBake == 1) {
			for (int k = 0; k < 16; k++) { uint32_t t; memcpy(&t, &fin[k], 4); XrHashU32(hsh, t); }
		}
		auto it = s_meshCache.find(hsh);
		if (it != s_meshCache.end()) {
			hit = it->second.get();
			q.verts = hit->verts;
			q.idx = hit->idx;
			q.uvs = hit->uvs;
			q.cookie = hit->cookie;
			s_meshCacheHits++;
		}
	}
	const bool wasMiss = (hit == nullptr);
	if (wasMiss) {
	q.verts.resize(numVerts);
	const uint8_t* src = (const uint8_t*)vdata;
	for (int v = 0; v < numVerts; v++, src += stride) {
		float x, y, z; uint32_t normal, color;
		memcpy(&x, src, 4); memcpy(&y, src + 4, 4); memcpy(&z, src + 8, 4);
		memcpy(&normal, src + 12, 4); memcpy(&color, src + 16, 4);
		MeshRasterVertex& o = q.verts[v];
		o.px = x; o.py = y; o.pz = z;
		o.nx = (float)(normal & 0xFF) / 127.0f - 1.0f;
		o.ny = (float)((normal >> 8) & 0xFF) / 127.0f - 1.0f;
		o.nz = (float)((normal >> 16) & 0xFF) / 127.0f - 1.0f;
		const uint32_t r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
		// S29: bake the per-tl/shape blend tint (native uBlendColor) into the
		// vertex RGB. Alpha rides the uAlpha uniform (S4v); the byte here is
		// informational only (the shader reads 3 color bytes).
		float fr = r * s_drawTint[0];
		float fg = g * s_drawTint[1];
		float fb = b * s_drawTint[2];
		// S30: color mix (uMixColor/uMixPercent) - lerp toward the mix color.
		if (s_drawMixPercent > 0.0f) {
			fr = fr + (s_drawMix[0] - fr) * s_drawMixPercent;
			fg = fg + (s_drawMix[1] - fg) * s_drawMixPercent;
			fb = fb + (s_drawMix[2] - fb) * s_drawMixPercent;
		}
		const uint32_t r2 = (fr > 255.f) ? 255u : (uint32_t)(fr + 0.5f);
		const uint32_t g2 = (fg > 255.f) ? 255u : (uint32_t)(fg + 0.5f);
		const uint32_t b2 = (fb > 255.f) ? 255u : (uint32_t)(fb + 0.5f);
		const uint32_t a2 = (uint32_t)(s_drawAlpha * 255.0f + 0.5f);
		o.color = (a2 << 24) | (r2 << 16) | (g2 << 8) | b2;
		// MI Z-up -> mesh path Y-up: swap y/z (position + normal).
		const float ty = o.py, tz = o.pz; o.py = tz; o.pz = ty;
		const float tny = o.ny, tnz = o.nz; o.ny = tnz; o.nz = tny;
		// S4: UVs. MI vertex (CppProject Vertex, 36B): pos 0-12, normal 12-16,
		// color 16-20, u,v 20-28 (page-relative 0-1, v=0 = top row of the
		// page; the page QImage is uploaded top-row-first so v matches).
		float u, vv;
		memcpy(&u, src + 20, 4); memcpy(&vv, src + 24, 4);
		// S4b: sprite-relative UVs -> registered texture-space UVs.
		// BODYPART (isShape) vbuffers already carry texture-space UVs
		// (page-relative for paged skins, texture-relative for standalone
		// skins) - baking again double-transforms them into empty atlas
		// regions. Only non-shape (world sprite) vbuffers need the rect.
		// S4b: bake the registered rect for ALL draws (identity for
		// standalone textures). First real test with working UV uploads.
		// S56x DIAG: keep the FIRST two RAW (pre-rect) UVs of the draw for
		// the truth dump - settles frame-relative vs page-relative directly.
		if (v == 0) { s_diagRawU[0] = u; s_diagRawV[0] = vv; }
		else if (v == 1) { s_diagRawU[1] = u; s_diagRawV[1] = vv; }
		u = (float)(s_curUx + u * s_curUw);
		vv = (float)(s_curUy + vv * s_curUh);
		q.uvs.push_back(u);
		q.uvs.push_back(vv);
		// S38 BISECT: per-vertex emissive DISABLED (uv back to 2 floats) to
		// isolate the f228 fist-stretch regression.
		// uint32_t vdat; memcpy(&vdat, src + 28, 4);
		// const float vemis = (float)((vdat >> 8) & 0xFF) / 255.0f;
		// q.uvs.push_back(vemis > s_drawEmissive ? vemis : s_drawEmissive);
	}
	q.idx.reserve(numIndices);
	const uint8_t* ip = (const uint8_t*)idata;
	for (int i = 0; i < numIndices; i++, ip += 4) {
		uint32_t ix; memcpy(&ix, ip, 4);
		q.idx.push_back(ix);
	}
	}
	if (profDraw) sProfDraw += QpcMsSince(pT0);
	// S4: translate the page key to its GL texture name (the backend binds
	// GL names; key 1 = the white fallback).
	const bool whiteBind = !s_tex.count(s_curTexId);
	if (whiteBind) s_statWhiteBinds++; else s_statRealBinds++;
	q.texId = (whiteBind ? 1 : s_tex[s_curTexId]);
	// S56k DIAG (XFAWA_FORCE_WHITE=1): force the white fallback on small
	// meshes - decides "texture kills the fragments" vs "draw never runs"
	// for the invisible eyeball/iris shapes.
	{
		static int fw = -1;
		if (fw < 0) { const char* e = getenv("XFAWA_FORCE_WHITE"); fw = (e && atoi(e) == 1) ? 1 : 0; }
		if (fw == 1 && numVerts >= 24 && numVerts <= 300 && numIndices <= 400) {
			q.texId = 1;
			static int fwLog = 0;
			if (fwLog < 6) { fwLog++; XrDiagLine("force_white applied"); }
		}
	}
	q.texFor = s_lastTexFor; // S4v diag
	q.alpha = s_drawAlpha; // S4v
	q.emissive = s_drawEmissive; // S38
	q.glow = (s_drawGlow > 0.5f); q.onlyGlow = (s_drawOnlyGlow > 0.5f); // S56
	q.glowColor[0] = s_drawGlowColor[0]; q.glowColor[1] = s_drawGlowColor[1]; q.glowColor[2] = s_drawGlowColor[2]; // S56

	// S56: glow-only objects (only_render_glow) are dropped from the main
	// pass - native renders them only in the COLOR_GLOW pass (not yet
	// implemented), so drawing them opaque produced the f330 white bands.
	if (q.glow && q.onlyGlow) {
		if (profDraw) sProfDraw += QpcMsSince(pT0);
		return;
	}

	// ==== S20/S52b: CPU vertex pre-bake ====
	// Transform positions (and normals, via the inverse-transpose) to WORLD
	// space on the CPU here, and hand the backend an IDENTITY model matrix.
	// S52b: runs ONLY on cache misses (a hit's verts are already world-baked);
	// the backend's per-draw model-matrix path renders black, so bake stays.
	if (sCpuBake == 1) {
		if (wasMiss && !q.verts.empty()) {
		// fin is a column-major array: math element M(r,c) = fin[4*c + r].
		const float m00 = fin[0], m10 = fin[1], m20 = fin[2];
		const float m01 = fin[4], m11 = fin[5], m21 = fin[6];
		const float m02 = fin[8], m12 = fin[9], m22 = fin[10];
		const float tx = fin[12], ty = fin[13], tz = fin[14];
		const float det = m00 * (m11 * m22 - m12 * m21)
			- m01 * (m10 * m22 - m12 * m20)
			+ m02 * (m10 * m21 - m11 * m20);
		// inverse-transpose = cofactor/det (math rows), for world normals.
		float n00 = 1, n01 = 0, n02 = 0, n10 = 0, n11 = 1, n12 = 0, n20 = 0, n21 = 0, n22 = 1;
		if (fabsf(det) > 1e-30f) {
			const float id = 1.0f / det;
			n00 = (m11 * m22 - m12 * m21) * id;  n01 = -(m10 * m22 - m12 * m20) * id;  n02 = (m10 * m21 - m11 * m20) * id;
			n10 = -(m01 * m22 - m02 * m21) * id; n11 = (m00 * m22 - m02 * m20) * id;   n12 = -(m00 * m21 - m01 * m20) * id;
			n20 = (m01 * m12 - m02 * m11) * id;  n21 = -(m00 * m12 - m02 * m10) * id;  n22 = (m00 * m11 - m01 * m10) * id;
		}
		LARGE_INTEGER pB0 = {};
		if (profDraw) QueryPerformanceCounter(&pB0);
		// S56j: view direction in GL space (the bake output space). The
		// begin_frame camera arrives in MI coords; the GL swap is y/z.
		// Recomputed whenever the camera moves (XFAWA_DEPTH_EPS=0 disables).
		static float vdX = 0.f, vdY = 0.f, vdZ = 1.f, vdEps = -1.f;
		static double lastFx = 1e300, lastFy = 0, lastFz = 0, lastTx = 0, lastTy = 0, lastTz = 0;
		if (vdEps < 0.f || s_camF[0] != lastFx || s_camF[1] != lastFy || s_camF[2] != lastFz
			|| s_camT[0] != lastTx || s_camT[1] != lastTy || s_camT[2] != lastTz) {
			lastFx = s_camF[0]; lastFy = s_camF[1]; lastFz = s_camF[2];
			lastTx = s_camT[0]; lastTy = s_camT[1]; lastTz = s_camT[2];
			float dx = (float)(s_camT[0] - s_camF[0]);
			float dy = (float)(s_camT[1] - s_camF[1]);
			float dz = (float)(s_camT[2] - s_camF[2]);
			// MI (x,y,z) -> GL (x,z,y): the direction swaps the same way.
			float gx = dx, gy = dz, gz = dy;
			float gl2 = sqrtf(gx * gx + gy * gy + gz * gz);
			if (gl2 > 1e-9f) { vdX = gx / gl2; vdY = gy / gl2; vdZ = gz / gl2; }
			const char* ve = getenv("XFAWA_DEPTH_EPS");
			float e = (ve && ve[0]) ? (float)atof(ve) : 0.01f;
			vdEps = -e; // move TOWARD the camera (against the view dir)
		}
		// S58: native D3D11 depth LESS = at exact ties the EARLIER draw
		// wins; GL_LEQUAL here lets the LATER draw cover it. The item
		// stickers (pupils, glints) sit flush on the face and draw before
		// the head bodyparts, so LEQUAL buried them. A per-draw bias makes
		// EARLIER draws sit CLOSER to the camera, so at ties the earlier
		// draw now wins under LEQUAL - replicating native's LESS without
		// touching the frozen backend. The bias ramps over the frame's
		// draw queue (max = epsilon at draw 0, 0 at the last draw), so the
		// total artificial depth spread stays at epsilon.
		float drawBias = vdEps;
		if (vdEps != 0.f) {
			const float idx = (float)(s_queue.size() & 2047);
			drawBias = vdEps * (1.0f - idx / 2048.0f);
		}
		for (size_t vi = 0; vi < q.verts.size(); vi++) {
			MeshRasterVertex& o = q.verts[vi];
			const float x = o.px, y = o.py, z = o.pz;
			o.px = m00 * x + m01 * y + m02 * z + tx;
			o.py = m10 * x + m11 * y + m12 * z + ty;
			o.pz = m20 * x + m21 * y + m22 * z + tz;
			// S56j: native D3D11 uses depth LESS (ties -> the EARLIER draw);
			// the frozen backend uses GL_LEQUAL (ties -> the LATER draw).
			// The eyeball/iris sit FLUSH with the head skin and draw EARLIER
			// (native tl order), so LEQUAL lets the skin cover them. A tiny
			// toward-camera nudge turns exact ties into earlier-draw wins,
			// matching native without touching the frozen backend.
			o.px += vdX * drawBias; o.py += vdY * drawBias; o.pz += vdZ * drawBias;
			const float nx = o.nx, ny = o.ny, nz = o.nz;
			float wx = n00 * nx + n01 * ny + n02 * nz;
			float wy = n10 * nx + n11 * ny + n12 * nz;
			float wz = n20 * nx + n21 * ny + n22 * nz;
			const float l = sqrtf(wx * wx + wy * wy + wz * wz);
			if (l > 1e-9f) { wx /= l; wy /= l; wz /= l; } else { wx = 0.f; wy = 0.f; wz = 1.f; }
			o.nx = wx; o.ny = wy; o.nz = wz;
		}
		if (profDraw) sProfBake += QpcMsSince(pB0);
		}
		// identity model matrix -> backend applies only the view matrix
		// (both paths: a cache hit's verts are already world-baked)
		memset(q.world, 0, sizeof(q.world));
		q.world[0] = q.world[5] = q.world[10] = q.world[15] = 1.0f;
	} else {
		memcpy(q.world, fin, sizeof(fin));
	}
	// S52b: cache store AFTER the bake (post-bake content), on the SECOND
	// sighting of a key only - moving parts get fresh keys every frame and
	// must never enter the cache (each entry pins a backend VBO).
	if (wasMiss) {
		static std::unordered_map<uint64_t, uint8_t> s_seen;
		if (s_seen.size() > 8192) s_seen.clear();
		const bool second = (s_seen[hsh] != 0);
		s_seen[hsh] = 1;
		if (second) {
			auto e = std::make_shared<XrMeshCacheEntry>();
			e->verts = q.verts; e->idx = q.idx; e->uvs = q.uvs;
			e->cookie = ++s_meshCookieSeq;
			s_meshCache[hsh] = e;
			q.cookie = e->cookie;
			s_meshCacheMisses++;
		}
	}
	// S4 debug: log the first mesh's first 4 UV pairs + texture key per frame.
	static int uvFrameDbg = 0;
	if (uvFrameDbg < 3 && q.uvs.size() >= 8) {
		uvFrameDbg++;
		char dbg[256];
		snprintf(dbg, sizeof(dbg), "frame %d draw: texId=%llu uv0=(%.3f,%.3f) uv1=(%.3f,%.3f) uv2=(%.3f,%.3f) uv3=(%.3f,%.3f)\r\n",
			frameIndexTotal, (unsigned long long)q.texId,
			q.uvs[0], q.uvs[1], q.uvs[2], q.uvs[3], q.uvs[4], q.uvs[5], q.uvs[6], q.uvs[7]);
		OutputDebugStringA(dbg);
		char lp[MAX_PATH];
		snprintf(lp, sizeof(lp), "%s\\xr_bridge_log.txt", s_outDir.c_str());
		FILE* lf = fopen(lp, "ab");
		if (lf) { fputs(dbg, lf); fclose(lf); }
	}
	// S4b DIAG: per-draw dump for target frames (key, GL name, uv range).
	// S4r: also dump at 450 (ribbon moment) with matrix summary of the
	// composed GL matrix (fin): translation, basis lengths (= scale), the
	// pre-swap world translation (MI coords), and local vertex bounds
	// (post y/z swap).
	if (frameIndexTotal == 450 || frameIndexTotal == 900 || frameIndexTotal == 1290 || frameIndexTotal == 455 || frameIndexTotal == 475 || frameIndexTotal == 495 || frameIndexTotal == 515 || frameIndexTotal == 1680 || frameIndexTotal == 330 || frameIndexTotal == 810
		|| (s_exportFrame >= 149 && s_exportFrame <= 151) || (s_exportFrame >= 169 && s_exportFrame <= 171)
		|| (s_exportFrame >= 595 && s_exportFrame <= 605)) {
		float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
		float emisMax = 0.f; // S38
		for (size_t i = 0; i + 2 < q.uvs.size(); i += 3) {
			if (q.uvs[i] < umin) umin = q.uvs[i];
			if (q.uvs[i] > umax) umax = q.uvs[i];
			if (q.uvs[i + 1] < vmin) vmin = q.uvs[i + 1];
			if (q.uvs[i + 1] > vmax) vmax = q.uvs[i + 1];
			if (q.uvs[i + 2] > emisMax) emisMax = q.uvs[i + 2];
		}
		const float bx = sqrtf(fin[0] * fin[0] + fin[1] * fin[1] + fin[2] * fin[2]);
		const float by = sqrtf(fin[4] * fin[4] + fin[5] * fin[5] + fin[6] * fin[6]);
		const float bz = sqrtf(fin[8] * fin[8] + fin[9] * fin[9] + fin[10] * fin[10]);
		float lx0 = 1e9f, lx1 = -1e9f, ly0 = 1e9f, ly1 = -1e9f, lz0 = 1e9f, lz1 = -1e9f;
		for (int v = 0; v < numVerts; v++) {
			const MeshRasterVertex& vv = q.verts[v];
			if (vv.px < lx0) lx0 = vv.px; if (vv.px > lx1) lx1 = vv.px;
			if (vv.py < ly0) ly0 = vv.py; if (vv.py > ly1) ly1 = vv.py;
			if (vv.pz < lz0) lz0 = vv.pz; if (vv.pz > lz1) lz1 = vv.pz;
		}
		char dbg[400];
		snprintf(dbg, sizeof(dbg),
			"f%d d%d texFor=%llu curKey=%llu gl=%llu verts=%d uvU=[%.4f,%.4f] uvV=[%.4f,%.4f] emisMax=%.3f t=(%.3f,%.3f,%.3f) b=(%.4f,%.4f,%.4f) wt=(%.2f,%.2f,%.2f) loc=x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f]\r\n",
			frameIndexTotal, (int)s_queue.size(), (unsigned long long)q.texFor, (unsigned long long)s_curTexId, (unsigned long long)q.texId,
			numVerts, umin, umax, vmin, vmax, emisMax,
			fin[12], fin[13], fin[14], bx, by, bz,
			s_world[12], s_world[13], s_world[14],
			lx0, lx1, ly0, ly1, lz0, lz1);
		char lp[MAX_PATH];
		snprintf(lp, sizeof(lp), "%s\\f%d_draws.txt", s_outDir.c_str(), frameIndexTotal);
		FILE* lf = fopen(lp, "ab");
		if (lf) { fputs(dbg, lf); fclose(lf); }
		// S56f TRUTH: raw baked UVs (2/vertex!), the registry rect used at
		// enqueue, and the color state - the stride-3 summary above is an
		// artifact (uvs are pairs, not triples).
		if (q.uvs.size() >= 8) {
			char dbg2[400];
			const float detD = fin[0] * (fin[5] * fin[10] - fin[6] * fin[9])
				- fin[4] * (fin[1] * fin[10] - fin[2] * fin[9])
				+ fin[8] * (fin[1] * fin[6] - fin[2] * fin[5]);
			snprintf(dbg2, sizeof(dbg2),
				"  uv0=(%.4f,%.4f) uv1=(%.4f,%.4f) uv2=(%.4f,%.4f) uv3=(%.4f,%.4f) rect=(%.4f,%.4f,%.4f,%.4f) tint=(%.3f,%.3f,%.3f) a=%.3f emis=%.3f mixPct=%.3f mixCol=(%.3f,%.3f,%.3f) rawUV0=(%.4f,%.4f) rawUV1=(%.4f,%.4f) texKey=%llu det=%.4f idx=%d\r\n",
				q.uvs[0], q.uvs[1], q.uvs[2], q.uvs[3],
				q.uvs[4], q.uvs[5], q.uvs[6], q.uvs[7],
				s_curUx, s_curUy, s_curUw, s_curUh,
				s_drawTint[0], s_drawTint[1], s_drawTint[2],
				s_drawAlpha, s_drawEmissive, s_drawMixPercent,
				s_drawMix[0], s_drawMix[1], s_drawMix[2],
				s_diagRawU[0], s_diagRawV[0], s_diagRawU[1], s_diagRawV[1],
				(unsigned long long)s_curTexId, detD, (int)q.idx.size());
			FILE* lf2 = fopen(lp, "ab");
			if (lf2) { fputs(dbg2, lf2); fclose(lf2); }
		}
		// S4c DIAG: per-key 16x16 UV coverage grid (baked UVs = shader space).
		auto& grid = s_diagUvGrid[s_curTexId];
		for (size_t i = 0; i + 2 < q.uvs.size(); i += 3) { // S38: 3 floats/vertex
			int c = (int)(q.uvs[i] * 16.0); if (c < 0) c = 0; if (c > 15) c = 15;
			int r = (int)(q.uvs[i + 1] * 16.0); if (r < 0) r = 0; if (r > 15) r = 15;
			grid[r] |= (uint64_t)1 << c;
		}
	}
	// S34 DIAG: compact per-draw dump at one gated frame (env
	// XFAWA_MESH_DUMP_FRAME): texture/registry/alpha/tint/mix/emissive.
	if (frameIndexTotal == s34DumpFrame && s34DumpFrame >= 0) {
		float s34emis = 0.f; // S38: max per-vertex emissive of this draw
		for (size_t i = 2; i < q.uvs.size(); i += 3)
			if (q.uvs[i] > s34emis) s34emis = q.uvs[i];
		char dbg[400];
		snprintf(dbg, sizeof(dbg),
			"f%d d%d texFor=%llu curKey=%llu gl=%llu verts=%d uv=[%.4f,%.4f]/[%.4f,%.4f] alpha=%.3f tint=(%.2f,%.2f,%.2f) mix=(%.2f,%.2f,%.2f)x%.2f uEmis=%.3f vemis=%.3f white=%d\r\n",
			frameIndexTotal, (int)s_queue.size(), (unsigned long long)s_lastTexFor, (unsigned long long)s_curTexId,
			(unsigned long long)q.texId, numVerts,
			s_curUx, s_curUx + s_curUw, s_curUy, s_curUy + s_curUh,
			s_drawAlpha, s_drawTint[0], s_drawTint[1], s_drawTint[2],
			s_drawMix[0], s_drawMix[1], s_drawMix[2], s_drawMixPercent,
			s_drawEmissive, s34emis, whiteBind ? 1 : 0);
		char lp2[MAX_PATH];
		snprintf(lp2, sizeof(lp2), "%s\\f%d_draws.txt", s_outDir.c_str(), frameIndexTotal);
		FILE* lf2 = fopen(lp2, "ab");
		if (lf2) { fputs(dbg, lf2); fclose(lf2); }
		// S39: per-vertex dump (normal/color/uv) for the gated frame.
		{
			char lp3[MAX_PATH];
			snprintf(lp3, sizeof(lp3), "%s\\f%d_verts.txt", s_outDir.c_str(), frameIndexTotal);
			FILE* lf3 = fopen(lp3, "ab");
			if (lf3) {
				fprintf(lf3, "draw %u key=%llu tex=%llu verts=%d idx=%d isShape=%d\r\n",
					(unsigned)s_queue.size(), (unsigned long long)s_curTexId,
					(unsigned long long)q.texId, numVerts, numIndices, isShape);
				for (int v = 0; v < numVerts; v++) {
					const MeshRasterVertex& o = q.verts[v];
					fprintf(lf3, "v%d n=%.3f,%.3f,%.3f c=%u,%u,%u uv=%.4f,%.4f\r\n",
						v, o.nx, o.ny, o.nz,
						(o.color >> 16) & 0xFF, (o.color >> 8) & 0xFF, o.color & 0xFF,
						q.uvs[v * 2], q.uvs[v * 2 + 1]);
				}
				fclose(lf3);
			}
		}
	}
	s_queue.push_back(std::move(q));
}

// S4: uploads (or replaces) a texture from RGBA bytes. id is the MI-side
// texture key; GL texture names are internal. Filtering: linear, clamp.
XRB_API int xr_bridge_tex(uint64_t id, int w, int h, const uint8_t* rgba)
{
	if (!rgba || w <= 0 || h <= 0) return 0;
	// S42: content hash + dedupe. The exe re-uploads regenerated name-tag
	// textures with NEW keys every frame (~2.7MB/frame -> ~5.4GB of uploads
	// per full export); identical content now aliases to the existing GL
	// texture and skips the upload + retention entirely. Also fixes the
	// unbounded VRAM growth (one texture per tag per frame before).
	const size_t byteCount = (size_t)w * h * 4;
	uint64_t hsh = 1469598103934665603ULL;
	for (size_t i = 0; i < byteCount; i++) { hsh ^= rgba[i]; hsh *= 1099511628211ULL; }
	static std::unordered_map<uint64_t, uint64_t> s_keyHash;   // key -> content hash
	static std::unordered_map<uint64_t, uint64_t> s_hashOwner; // hash -> first key
	auto kh = s_keyHash.find(id);
	if (kh != s_keyHash.end()) {
		if (kh->second == hsh) return 1; // same key, same content - GL already correct
		// Content changed for an existing key: fresh GL name so a possibly
		// aliased texture is never mutated under other keys.
		GLuint nt = 0; glGenTextures(1, &nt); s_tex[id] = nt; kh->second = hsh;
	} else {
		s_keyHash[id] = hsh;
		auto ho = s_hashOwner.find(hsh);
		if (ho != s_hashOwner.end() && s_tex.count(ho->second)) {
			s_tex[id] = s_tex[ho->second]; // alias identical content
			return 1;
		}
		GLuint nt = 0; glGenTextures(1, &nt); s_tex[id] = nt;
	}
	s_hashOwner[hsh] = id;
	GLuint t = s_tex[id];
	{
		// S55: alpha histogram per upload - decides whether transparent PNGs
		// arrive with their alpha intact (blend-mode bug) or flattened to
		// opaque (upload-conversion bug) for the white-band defect.
		size_t aOpaque = 0, aZero = 0, aMid = 0;
		for (size_t i = 3; i < byteCount; i += 4) {
			if (rgba[i] >= 250) aOpaque++;
			else if (rgba[i] == 0) aZero++;
			else aMid++;
		}
		const size_t total = (size_t)w * h;
		char dbg[192];
		snprintf(dbg, sizeof(dbg), "texUpload key=%llu %dx%d hash=%016llx alpha: opaque=%.3f zero=%.3f mid=%.3f\r\n",
			(unsigned long long)id, w, h, (unsigned long long)hsh,
			(double)aOpaque / (double)total, (double)aZero / (double)total, (double)aMid / (double)total);
		char lp[MAX_PATH];
		snprintf(lp, sizeof(lp), "%s\\xr_bridge_log.txt", s_outDir.c_str());
		FILE* lf = fopen(lp, "ab");
		if (lf) { fputs(dbg, lf); fclose(lf); }
	}
	glBindTexture(GL_TEXTURE_2D, t);
	// S39b: point sampling option - MI renders 1-texel-per-face pixel-art
	// skins with point sampling; GL_LINEAR blends every face edge 50/50 with
	// the neighbouring atlas texel (dark seams + mush). Env A/B gate.
	static int sTexNearest = -1;
	if (sTexNearest < 0) { const char* e = getenv("XFAWA_TEX_NEAREST"); sTexNearest = (e && atoi(e) == 1) ? 1 : 0; }
	const GLint filt = sTexNearest ? GL_NEAREST : GL_LINEAR;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	// S4 probe: log GL error for EVERY upload (not just the first).
	{
		GLenum gerr = glGetError();
		char dbg[160];
		snprintf(dbg, sizeof(dbg), "texUpload key=%llu %dx%d glErr=0x%04X ctx=%p dc=%p thread=%u\r\n",
			(unsigned long long)id, w, h, (unsigned)gerr,
			(void*)wglGetCurrentContext(), (void*)wglGetCurrentDC(), GetCurrentThreadId());
		char lp[MAX_PATH];
		snprintf(lp, sizeof(lp), "%s\\xr_bridge_log.txt", s_outDir.c_str());
		FILE* lf = fopen(lp, "ab");
		if (lf) { fputs(dbg, lf); fclose(lf); }
	}
	// S39 probe: dump the first two pages (4096) for reference, keyed by id.
	// S41: gated behind XFAWA_TEX_DUMP=1 (134MB of BMPs per run by default
	// starves a 4GB box).
	static int pageDumped = 0;
	static int sPageDumpGate = -1;
	if (sPageDumpGate < 0) { const char* e = getenv("XFAWA_TEX_DUMP"); sPageDumpGate = (e && atoi(e) == 1) ? 1 : 0; }
	if (sPageDumpGate && w >= 4096 && pageDumped < 2) {
		pageDumped++;
		char pp[MAX_PATH];
		snprintf(pp, sizeof(pp), "%s\\page_dump_%llu.bmp", s_outDir.c_str(), (unsigned long long)id);
		SaveBmp(pp, (const uint32_t*)rgba, w, h);
	}
	// S4c DIAG: dump every standalone upload (cap 48) + retain RGBA for CPU
	// expected-sampling. Byte order: QImage Format_RGBA8888 rows, R,G,B,A,
	// top row first; uploaded to GL unflipped (v=0 = top row).
	static int diagDumps = 0;
	// S38: the text-size exception (every regenerated text texture) flooded
	// ~2000 BMPs per run and throttled the export; cap-only now.
	// S41: same XFAWA_TEX_DUMP gate for the standalone tex dumps.
	static int sTexDumpGate = -1;
	if (sTexDumpGate < 0) { const char* e = getenv("XFAWA_TEX_DUMP"); sTexDumpGate = (e && atoi(e) == 1) ? 1 : 0; }
	if (sTexDumpGate && w < 4096 && diagDumps < 48) {
		diagDumps++;
		char pp[MAX_PATH];
		snprintf(pp, sizeof(pp), "%s\\tex_%llu_%dx%d.bmp", s_outDir.c_str(),
			(unsigned long long)id, w, h);
		SaveBmp(pp, (const uint32_t*)rgba, w, h);
	}
	// S42: retention now byte-capped (was unbounded - name-tag re-uploads
	// with fresh keys grew ~5GB of RAM per full export on a 4GB box).
	if (byteCount <= 4194304) {
		static size_t sDiagBytes = 0;
		if (sDiagBytes + byteCount > 128ull * 1024 * 1024) {
			s_diagTexData.clear(); s_diagTexSize.clear(); sDiagBytes = 0;
		}
		s_diagTexData[id].assign(rgba, rgba + byteCount);
		s_diagTexSize[id] = { w, h };
		sDiagBytes += byteCount;
	}
	// S4 debug: verify the page pixels are not blank/zero.
	static int texDbg = 0;
	if (texDbg < 3) {
		texDbg++;
		uint64_t nz = 0;
		const size_t total = (size_t)w * h * 4;
		for (size_t i = 0; i < total; i++) if (rgba[i] != 0) nz++;
		char dbg[160];
		snprintf(dbg, sizeof(dbg), "tex id=%llu %dx%d nonzeroBytes=%llu (%.1f%%)\r\n",
			(unsigned long long)id, w, h, (unsigned long long)nz, 100.0 * nz / total);
		OutputDebugStringA(dbg);
		char lp[MAX_PATH];
		snprintf(lp, sizeof(lp), "%s\\xr_bridge_log.txt", s_outDir.c_str());
		FILE* lf = fopen(lp, "ab");
		if (lf) { fputs(dbg, lf); fclose(lf); }
	}
	return 1;
}

// S4: sets the texture applied to subsequent draw_mesh calls (0 = none).
static uint64_t s_statTexCalls = 0, s_statTexNonzero = 0;
XRB_API void xr_bridge_set_uvrect(double x, double y, double w, double h)
{
	static int uvDbg = 0;
	if (uvDbg < 6) {
		uvDbg++;
		char dbg[128];
		snprintf(dbg, sizeof(dbg), "uvRect x=%.4f y=%.4f w=%.4f h=%.4f\r\n", x, y, w, h);
		char lp[MAX_PATH];
		snprintf(lp, sizeof(lp), "%s\\xr_bridge_log.txt", s_outDir.c_str());
		FILE* lf = fopen(lp, "ab");
		if (lf) { fputs(dbg, lf); fclose(lf); }
	}
	s_uvX = x; s_uvY = y; s_uvW = w; s_uvH = h;
}

// S4b: per-item texture registry + sprite->page UV rect, filled per frame
// (native-equivalent resolution), queried per draw by tl id. Paged sprites
// register their page key + the sprite's uvRect (vbuffer UVs are
// sprite-relative 0-1); standalone sprites register their own texture with
// an identity rect.
struct ItemTex { uint64_t texId = 1; double ux = 0, uy = 0, uw = 1, uh = 1; };
static std::unordered_map<uint64_t, ItemTex> s_itemTex;
XRB_API void xr_bridge_register_item_tex(uint64_t tlId, uint64_t texId)
{
	ItemTex& e = s_itemTex[tlId];
	e.texId = texId; e.ux = 0; e.uy = 0; e.uw = 1; e.uh = 1;
}
XRB_API void xr_bridge_register_item_tex2(uint64_t tlId, uint64_t texId,
	double ux, double uy, double uw, double uh)
{
	ItemTex& e = s_itemTex[tlId];
	e.texId = texId; e.ux = ux; e.uy = uy; e.uw = uw; e.uh = uh;
	// S55 DIAG: registration rect dump (env XFAWA_UVDUMP=1, first N regs).
	static int uvDump = -1;
	if (uvDump < 0) { const char* ev = getenv("XFAWA_UVDUMP"); uvDump = (ev && *ev == '1') ? 400 : 0; }
	if (uvDump > 0) {
		uvDump--;
		char dbg[160];
		snprintf(dbg, sizeof(dbg), "regItem tl=%llu tex=%llu rect=(%.4f,%.4f,%.4f,%.4f)\r\n",
			(unsigned long long)tlId, (unsigned long long)texId, ux, uy, uw, uh);
		char lp[MAX_PATH];
		snprintf(lp, sizeof(lp), "%s\\xr_bridge_log.txt", s_outDir.c_str());
		FILE* lf = fopen(lp, "ab");
		if (lf) { fputs(dbg, lf); fclose(lf); }
	}
}
XRB_API void xr_bridge_set_tex_for(uint64_t tlId)
{
	s_lastTexFor = tlId;
	auto it = s_itemTex.find(tlId);
	s_statTexCalls++;
	if (it != s_itemTex.end()) {
		s_statTexNonzero++;
		s_curTexId = it->second.texId;
		s_curUx = it->second.ux; s_curUy = it->second.uy;
		s_curUw = it->second.uw; s_curUh = it->second.uh;
		// S55 DIAG: the registry resolved a key that has NO GL texture yet -
		// the draw would sample an incomplete texture (GL = opaque black).
		// Env XFAWA_UVDUMP=1 logs the first 200 misses.
		static int missLog = -1;
		if (missLog < 0) { const char* ev = getenv("XFAWA_UVDUMP"); missLog = (ev && *ev == '1') ? 200 : 0; }
		if (missLog > 0 && !s_tex.count(s_curTexId)) {
			missLog--;
			char dbg[160];
			snprintf(dbg, sizeof(dbg), "texMISS tl=%llu key=%llu rect=(%.4f,%.4f,%.4f,%.4f)\r\n",
				(unsigned long long)tlId, (unsigned long long)s_curTexId,
				s_curUx, s_curUy, s_curUw, s_curUh);
			char lp[MAX_PATH];
			snprintf(lp, sizeof(lp), "%s\\xr_bridge_log.txt", s_outDir.c_str());
			FILE* lf = fopen(lp, "ab");
			if (lf) { fputs(dbg, lf); fclose(lf); }
		}
	} else {
		// Registry miss: white fallback (visible, never a wrong atlas).
		s_curTexId = 1;
		s_curUx = 0; s_curUy = 0; s_curUw = 1; s_curUh = 1;
	}
}

XRB_API void xr_bridge_set_texture(uint64_t id)
{
	static uint64_t lastLogged = 0;
	if (id != lastLogged) {
		lastLogged = id;
		char dbg[96];
		snprintf(dbg, sizeof(dbg), "bind pageKey=%llu\r\n", (unsigned long long)id);
		char lp[MAX_PATH];
		snprintf(lp, sizeof(lp), "%s\\xr_bridge_log.txt", s_outDir.c_str());
		FILE* lf = fopen(lp, "ab");
		if (lf) { fputs(dbg, lf); fclose(lf); }
	}
	s_statTexCalls++;
	if (id != 0) s_statTexNonzero++;
	s_curTexId = id;
	s_curUx = 0; s_curUy = 0; s_curUw = 1; s_curUh = 1;
}

// S4t: native background (render_world_background). The GML calls set_bg
// every frame (sky clear color, GML 0xBBGGRR) and set_bg_image when the
// background image is active. The image's page was already uploaded by the
// loader (xr_bridge_tex) before this call, so the key resolves here.
XRB_API void xr_bridge_set_bg(double skyColor)
{
	const uint32_t c = (uint32_t)(int64_t)skyColor;
	s_bgSkySet = true;
	s_bgSky[0] = (float)(c & 0xFF) / 255.0f;
	s_bgSky[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	s_bgSky[2] = (float)((c >> 16) & 0xFF) / 255.0f;
}

XRB_API void xr_bridge_set_bg_image(double texKey, double ux, double uy, double uw, double uh)
{
	s_bgImgKey = (uint64_t)texKey;
	s_bgUx = ux; s_bgUy = uy; s_bgUw = uw; s_bgUh = uh;
	s_bgImgActive = s_bgImgKey != 0 && s_tex.count(s_bgImgKey) > 0;
}

// S4v: native per-tl alpha (render_set_uniform_color uBlendColor alpha).
// The GML sets this before a tl's draws; the draw queue bakes it per mesh.
XRB_API void xr_bridge_set_draw_alpha(double a)
{
	if (a < 0.0) a = 0.0;
	if (a > 1.0) a = 1.0;
	s_drawAlpha = (float)a;
}

// S29: native per-tl/shape blend color (uBlendColor rgb, GML 0xBBGGRR).
// Baked into the vertex colors of subsequent draws in draw_mesh.
XRB_API void xr_bridge_set_draw_color(double color)
{
	const uint32_t c = (uint32_t)(int64_t)color;
	s_drawTint[0] = (float)(c & 0xFF) / 255.0f;        // R (low byte)
	s_drawTint[1] = (float)((c >> 8) & 0xFF) / 255.0f; // G
	s_drawTint[2] = (float)((c >> 16) & 0xFF) / 255.0f; // B
}

// S30: native per-tl color mix (uMixColor/uMixPercent). GML color 0xBBGGRR,
// percent 0-1.
XRB_API void xr_bridge_set_draw_mix(double color, double percent)
{
	const uint32_t c = (uint32_t)(int64_t)color;
	s_drawMix[0] = (float)(c & 0xFF) / 255.0f;
	s_drawMix[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	s_drawMix[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	if (percent < 0.0) percent = 0.0;
	if (percent > 1.0) percent = 1.0;
	s_drawMixPercent = (float)percent;
}

// S53c: batched per-tl draw state (native render_world_tl uniforms):
// alpha, uBlendColor, uMixColor, uMixPercent, uEmissive.
// S56 COMPAT: exactly 5 args - the older exes (f669ff8 and earlier) call
// this with 5 args; a 9-arg version here read 4 garbage values from the
// stack (random only_render_glow drops = the broken black character).
XRB_API void xr_bridge_set_draw_state(double a, double color, double mixColor,
	double mixPercent, double emissive)
{
	if (a < 0.0) a = 0.0;
	if (a > 1.0) a = 1.0;
	s_drawAlpha = (float)a;
	const uint32_t c = (uint32_t)(int64_t)color;
	s_drawTint[0] = (float)(c & 0xFF) / 255.0f;
	s_drawTint[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	s_drawTint[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	const uint32_t mc = (uint32_t)(int64_t)mixColor;
	s_drawMix[0] = (float)(mc & 0xFF) / 255.0f;
	s_drawMix[1] = (float)((mc >> 8) & 0xFF) / 255.0f;
	s_drawMix[2] = (float)((mc >> 16) & 0xFF) / 255.0f;
	if (mixPercent < 0.0) mixPercent = 0.0;
	if (mixPercent > 1.0) mixPercent = 1.0;
	s_drawMixPercent = (float)mixPercent;
	s_drawEmissive = (emissive < 0.0) ? 0.0f : (float)emissive;
}

// S56: the glow state in a SEPARATE export - the new GML calls this right
// after set_draw_state. Old exes never call it (the glow state simply
// stays 0 = the pre-glow behaviour).
XRB_API void xr_bridge_set_draw_state_glow(double glow, double onlyGlow,
	double glowTex, double glowColorAbgr)
{
	s_drawGlow = (glow != 0.0) ? 1.0f : 0.0f;
	s_drawOnlyGlow = (onlyGlow != 0.0) ? 1.0f : 0.0f;
	s_drawGlowTex = (glowTex != 0.0) ? 1.0f : 0.0f;
	const uint32_t gc = (uint32_t)(int64_t)glowColorAbgr;
	s_drawGlowColor[0] = (float)(gc & 0xFF) / 255.0f;
	s_drawGlowColor[1] = (float)((gc >> 8) & 0xFF) / 255.0f;
	s_drawGlowColor[2] = (float)((gc >> 16) & 0xFF) / 255.0f;
}

// S36: scene lights (the native uLightData layout: 8 floats per light =
// pos xyz, range | rgb, 1; light 0 = the sun) + sun direction + ambient +
// the camera post (tonemapper/exposure/gamma). The loader reads the engine
// globals directly and hands them over here.
XRB_API void xr_bridge_set_lights(int amount, const double* data, double sx, double sy, double sz,
	double ambientAbgr, double tonemapper, double exposure, double gamma)
{
	int n = amount;
	if (n < 0) n = 0;
	if (n > 64) n = 64;
	// S56 REVISED: lights 1+ are spot lights natively (cone via uLightMatrix
	// projection); the frozen backend shader only has the point path. s55
	// proved the point approximation matches native at f330 (99.1%) - the
	// "overbright" that motivated the drop was garbage-args corruption from
	// the 9-arg ABI mismatch, NOT the light shape. Keep all lights.
	// Env XFAWA_DROP_LIGHTS=1 restores the drop (diagnostics only).
	{
		static int dp = -1;
		if (dp < 0) { const char* e = getenv("XFAWA_DROP_LIGHTS"); dp = (e && atoi(e) == 1) ? 1 : 0; }
		if (dp == 1 && n > 1) n = 1;
	}
	float buf[64 * 8];
	for (int i = 0; i < n; i++) {
		const int o = i * 8;
		// S37: the light data is MI-world (Z-up); the bridge's vertex world
		// space is the swapped GL space (Y-up) - swap y/z per light or the
		// lights sit in wrong positions (black at many angles).
		buf[o + 0] = (float)data[o + 0];
		buf[o + 1] = (float)data[o + 2];
		buf[o + 2] = (float)data[o + 1];
		buf[o + 3] = (float)data[o + 3];
		buf[o + 4] = (float)data[o + 4];
		buf[o + 5] = (float)data[o + 5];
		buf[o + 6] = (float)data[o + 6];
		buf[o + 7] = (float)data[o + 7];
	}
	const uint32_t amb = (uint32_t)(int64_t)ambientAbgr;
	static int sLightLog = 0;
	if (sLightLog < 3) {
		sLightLog++;
		char dbg[256];
		snprintf(dbg, sizeof(dbg), "lights n=%d sun=(%.3f,%.3f,%.3f) amb=%u tm=%d exp=%.3f gamma=%.3f data0=(%.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f)\r\n",
			n, sx, sy, sz, amb, (int)(int64_t)tonemapper, exposure, gamma,
			data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
		char lp3[MAX_PATH];
		snprintf(lp3, sizeof(lp3), "%s\\xr_bridge_log.txt", s_outDir.c_str());
		FILE* lf3 = fopen(lp3, "ab");
		if (lf3) { fputs(dbg, lf3); fclose(lf3); }
	}
	// S37: the sun direction swaps y/z the same way.
	XrTileSetLights(n, buf,
		(float)sx, (float)sz, (float)sy,
		(float)(amb & 0xFF) / 255.0f,
		(float)((amb >> 8) & 0xFF) / 255.0f,
		(float)((amb >> 16) & 0xFF) / 255.0f,
		(int)(int64_t)tonemapper, (float)exposure, (float)gamma);
}

// S3: hand the pending frame's pixels to the in-process encoder.
// dst receives RGBA, top-down (the backend's readback is BGRA bottom-up,
// so this is a row flip + B<->R swizzle). Returns 1 when a frame was
// pending, 0 when the encoder should fall back to the native surface.
XRB_API int xr_bridge_read_frame(uint8_t* dst, int w, int h)
{
	if (!s_backend || !s_framePending || !dst || w != s_W || h != s_H) {
		char dbg[128];
		snprintf(dbg, sizeof(dbg), "read_frame REJECT: pending=%d w=%d h=%d (want %dx%d)\r\n",
			s_framePending ? 1 : 0, w, h, s_W, s_H);
		OutputDebugStringA(dbg);
		return 0;
	}
	const uint32_t* px = s_backend->GetMeshPixels();
	if (!px) return 0;
	// S53: the backend reads back GL_RGBA. S53b: meshPixels_ is BOTTOM-UP
	// raw (the backend's flip pass was removed) - fuse the flip into this
	// copy (the encoder needs RGBA top-down).
	const bool profSwiz = ProfOn();
	LARGE_INTEGER pS0 = {};
	if (profSwiz) QueryPerformanceCounter(&pS0);
	for (int y = 0; y < h; y++) {
		memcpy(dst + (size_t)y * w * 4, px + (size_t)(h - 1 - y) * w, (size_t)w * 4);
	}
	if (profSwiz) sProfSwiz += QpcMsSince(pS0);
	s_framePending = false;
	return 1;
}

XRB_API int xr_bridge_end_frame(int frameIndex)
{
	if (!s_backend || !s_hasCam) return 0;
	s_exportFrame = frameIndex;

	RenderView rv;
	rv.posX = (float)s_camF[0]; rv.posY = (float)s_camF[2]; rv.posZ = (float)s_camF[1];
	float fx = (float)(s_camT[0] - s_camF[0]);
	float fy = (float)(s_camT[2] - s_camF[2]);
	float fz = (float)(s_camT[1] - s_camF[1]);
	float l = sqrtf(fx * fx + fy * fy + fz * fz); if (l < 1e-6f) l = 1;
	rv.pitch = asinf(fy / l) * 57.2957795f;
	rv.yaw = atan2f(-fx / l, -fz / l) * 57.2957795f;
	rv.fov = s_fov > 0 ? (float)s_fov : 60.0f;
	rv.nearPlane = 1.0f;
	rv.farPlane = 30000.0f; // S4b: match the project default (was 5000, clipped large scenes)
	rv.cameraMode = 1; rv.overrideW = s_W; rv.overrideH = s_H;
	// S4j DIAG: pass the camera up vector (MI Z-up cm[8..10] → renderer Y-up
	// via the same Y/Z swap as the position, L571). Normalized; upValid gates
	// the env-gated look_at experiment (XFAWA_CAM_UP=1).
	{
		float ux = (float)s_camU[0], uy = (float)s_camU[2], uz = (float)s_camU[1];
		float l = sqrtf(ux * ux + uy * uy + uz * uz);
		if (l > 1e-6f) { ux /= l; uy /= l; uz /= l; rv.upValid = 1; }
		rv.upX = ux; rv.upY = uy; rv.upZ = uz;
	}
	if (XrDiagGate()) {
		char dbg[340];
		snprintf(dbg, sizeof(dbg),
			"diagrv f=%d pos=(%.4f,%.4f,%.4f) pitch=%.3f yaw=%.3f fov=%.4f near=%.2f far=%.2f %dx%d upV=%d up=(%.4f,%.4f,%.4f)",
			frameIndexTotal, rv.posX, rv.posY, rv.posZ, rv.pitch, rv.yaw, rv.fov,
			rv.nearPlane, rv.farPlane, s_W, s_H, (int)rv.upValid, rv.upX, rv.upY, rv.upZ);
		XrDiagLine(dbg);
		if (rv.pitch > 88.0f || rv.pitch < -88.0f)
			XrDiagLine("diagrv WARN pitchNear90: look_at cross(f,up) degenerate");
	}

	// Stable rasters storage + draw items (pointers must stay valid).
	// S4u FIX: built AFTER the diagtri injection below - the gated push_back
	// reallocated s_queue and left items[].modelMatrix pointing into freed
	// memory, so every GPU-side diagnostic on gated frames read garbage
	// matrices (the draws-file matrices vs bbox/diaguni contradiction).

	// S4l diag: replicate the backend cameraMode 1 proj/view (build source =
	// tile_exp/OpenGLBackend_tile.cpp, formulas verified against it) and
	// predict the screen pixel of the first 3 draws' mesh origins. Offline
	// comparison against the rendered content decides camera-side vs
	// backend-side vs model-side without another probe round-trip.
	if (XrDiagGate() && !s_queue.empty()) {
		const float aspectD = (float)s_W / (float)s_H;
		const float nearD = rv.nearPlane, farD = rv.farPlane;
		const float tanHalf = tanf(rv.fov * 3.14159265f / 360.0f);
		float P[16];
		P[0] = 1.0f / (aspectD * tanHalf); P[4] = 0.0f; P[8] = 0.0f; P[12] = 0.0f;
		P[1] = 0.0f; P[5] = 1.0f / tanHalf; P[9] = 0.0f; P[13] = 0.0f;
		P[2] = 0.0f; P[6] = 0.0f; P[10] = -(farD + nearD) / (farD - nearD); P[14] = -(2.0f * farD * nearD) / (farD - nearD);
		P[3] = 0.0f; P[7] = 0.0f; P[11] = -1.0f; P[15] = 0.0f;
		const float yawRad = rv.yaw * 3.14159265f / 180.0f;
		const float pitchRad = rv.pitch * 3.14159265f / 180.0f;
		const float cy = cosf(yawRad), sy = sinf(yawRad);
		const float cp = cosf(pitchRad), sp = sinf(pitchRad);
		const float fwx = -sy * cp, fwy = sp, fwz = -cy * cp;
		float upX = 0.0f, upY = 1.0f, upZ = 0.0f;
		{
			const char* e = getenv("XFAWA_CAM_UP");
			if (e && atoi(e) != 0 && rv.upValid) {
				upX = rv.upX; upY = rv.upY; upZ = rv.upZ;
				const float dp = fabsf(fwx * upX + fwy * upY + fwz * upZ);
				if (dp > 0.9999f) { upX = 0.0f; upY = 1.0f; upZ = 0.0f; }
			}
		}
		float sx = fwy * upZ - fwz * upY;
		float syy = fwz * upX - fwx * upZ;
		float sz = fwx * upY - fwy * upX;
		const float sl = sqrtf(sx * sx + syy * syy + sz * sz);
		{
			char dbg[128];
			snprintf(dbg, sizeof(dbg), "diaglook f=%d fwd=(%.4f,%.4f,%.4f) sLen=%.6f",
				frameIndexTotal, fwx, fwy, fwz, sl);
			XrDiagLine(dbg);
		}
		if (sl > 1e-9f) {
			const float inv = 1.0f / sl; sx *= inv; syy *= inv; sz *= inv;
			const float uux = syy * fwz - sz * fwy, uuy = sz * fwx - sx * fwz, uuz = sx * fwy - syy * fwx;
			const float tx = -(sx * rv.posX + syy * rv.posY + sz * rv.posZ);
			const float tyy = -(uux * rv.posX + uuy * rv.posY + uuz * rv.posZ);
			const float tz = (fwx * rv.posX + fwy * rv.posY + fwz * rv.posZ);
			// S25 FIX: match the CORRECTED BuildMeshCamera (true column-major:
			// columns = s, u, -f, translation). The previous literal copied
			// the old transposed layout.
			const float V[16] = {
				sx,  uux, -fwx, 0.0f,
				syy, uuy, -fwy, 0.0f,
				sz,  uuz, -fwz, 0.0f,
				tx,  tyy, tz,  1.0f,
			};
			// S4v: project EVERY queued draw (was capped at 16) so the ribbon
			// draws can be matched by screen extents directly.
			for (size_t q = 0; q < s_queue.size(); q++) {
				const float* m = s_queue[q].world;
				const float p[4] = { m[12], m[13], m[14], 1.0f };
				float vv[4], cc[4];
				for (int r = 0; r < 4; r++)
					vv[r] = V[0 + 4 * r] * p[0] + V[1 + 4 * r] * p[1] + V[2 + 4 * r] * p[2] + V[3 + 4 * r] * p[3];
				for (int r = 0; r < 4; r++)
					cc[r] = P[0 + 4 * r] * vv[0] + P[1 + 4 * r] * vv[1] + P[2 + 4 * r] * vv[2] + P[3 + 4 * r] * vv[3];
				const float cw = (fabsf(cc[3]) < 1e-9f) ? 1e-9f : cc[3];
				const float ndcX = cc[0] / cw, ndcY = cc[1] / cw;
				const float pxX = (ndcX + 1.0f) * 0.5f * (float)s_W;
				const float pxY = (1.0f - (ndcY + 1.0f) * 0.5f) * (float)s_H;
				const bool off = (cw <= 0.0f || pxX < -100.0f || pxX > (float)s_W + 100.0f || pxY < -100.0f || pxY > (float)s_H + 100.0f);
				// S4v: project the 8 local-bbox corners through the same
				// MVP to get the draw's SCREEN RECT (origin-only was not
				// enough: the stripes come from large planes whose origin
				// sits on-screen while their geometry spans wide).
				float rminx = 1e30f, rmaxx = -1e30f, rminy = 1e30f, rmaxy = -1e30f;
				{
					float lx0 = 1e30f, lx1 = -1e30f, ly0 = 1e30f, ly1 = -1e30f, lz0 = 1e30f, lz1 = -1e30f;
					for (size_t vi = 0; vi < s_queue[q].verts.size(); vi++) {
						const MeshRasterVertex& vv2 = s_queue[q].verts[vi];
						if (vv2.px < lx0) lx0 = vv2.px; if (vv2.px > lx1) lx1 = vv2.px;
						if (vv2.py < ly0) ly0 = vv2.py; if (vv2.py > ly1) ly1 = vv2.py;
						if (vv2.pz < lz0) lz0 = vv2.pz; if (vv2.pz > lz1) lz1 = vv2.pz;
					}
					const float xs[2] = { lx0, lx1 }, ys[2] = { ly0, ly1 }, zs[2] = { lz0, lz1 };
					for (int ci = 0; ci < 8; ci++) {
						const float lx = xs[ci & 1], ly = ys[(ci >> 1) & 1], lz = zs[(ci >> 2) & 1];
						const float lp[4] = { lx, ly, lz, 1.0f };
						float mv4[4], cl4[4];
						for (int r = 0; r < 4; r++)
							mv4[r] = m[0 + 4 * r] * lp[0] + m[1 + 4 * r] * lp[1] + m[2 + 4 * r] * lp[2] + m[3 + 4 * r] * lp[3];
						for (int r = 0; r < 4; r++)
							cl4[r] = V[0 + 4 * r] * mv4[0] + V[1 + 4 * r] * mv4[1] + V[2 + 4 * r] * mv4[2] + V[3 + 4 * r] * mv4[3];
						float w2 = cl4[3];
						if (w2 <= 1e-6f) continue;
						float c2[4];
						for (int r = 0; r < 4; r++)
							c2[r] = P[0 + 4 * r] * cl4[0] + P[1 + 4 * r] * cl4[1] + P[2 + 4 * r] * cl4[2] + P[3 + 4 * r] * cl4[3];
						const float sx2 = (c2[0] / w2 + 1.0f) * 0.5f * (float)s_W;
						const float sy2 = (1.0f - (c2[1] / w2 + 1.0f) * 0.5f) * (float)s_H;
						if (sx2 < rminx) rminx = sx2; if (sx2 > rmaxx) rmaxx = sx2;
						if (sy2 < rminy) rminy = sy2; if (sy2 > rmaxy) rmaxy = sy2;
					}
				}
				char dbg[400];
				snprintf(dbg, sizeof(dbg),
					"diagproj f=%d i=%d tex=%llu texFor=%llu px=(%.0f,%.0f) rect=[%.0f,%.0f]x[%.0f,%.0f] b=(%.3f,%.3f,%.3f) clipW=%.3f%s",
					frameIndexTotal, (int)q, (unsigned long long)s_queue[q].texId, (unsigned long long)s_queue[q].texFor,
					pxX, pxY, rminx, rmaxx, rminy, rmaxy,
					sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]),
					sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]),
					sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]),
					cc[3],
					off ? " OFFSCREEN" : "");
				XrDiagLine(dbg);
			}
		} else {
			XrDiagLine("diaglook DEGENERATE sLen~0: view basis collapsed");
		}
	}

	// S4o DIAG: hardcoded test triangle 5 units in front of the camera.
	// S27: OFF by default - the injected R/G/B triangle showed up in the
	// user's exports as the "colored triangles in some frames". Opt in via
	// XFAWA_DIAGTRI=1 for transform debugging only.
	{
		static int sDiagTri = -1;
		if (sDiagTri < 0) { const char* e = getenv("XFAWA_DIAGTRI"); sDiagTri = (e && atoi(e) != 0) ? 1 : 0; }
	if (XrDiagGate() && sDiagTri == 1) {
		QueuedDraw t;
		const float fwx = -sinf(rv.yaw * 3.14159265f / 180.0f) * cosf(rv.pitch * 3.14159265f / 180.0f);
		const float fwy = sinf(rv.pitch * 3.14159265f / 180.0f);
		const float fwz = -cosf(rv.yaw * 3.14159265f / 180.0f) * cosf(rv.pitch * 3.14159265f / 180.0f);
		float rx = -fwz, ry = 0.0f, rz = fwx;
		const float rl = sqrtf(rx * rx + ry * ry + rz * rz);
		if (rl > 1e-9f) { rx /= rl; ry /= rl; rz /= rl; }
		const float uux = ry * fwz - rz * fwy, uuy = rz * fwx - rx * fwz, uuz = rx * fwy - ry * fwx;
		const float cx2 = rv.posX + 5.0f * fwx, cy2 = rv.posY + 5.0f * fwy, cz2 = rv.posZ + 5.0f * fwz;
		t.verts.resize(3);
		MeshRasterVertex& A = t.verts[0];
		A.px = cx2 - rx - uux; A.py = cy2 - ry - uuy; A.pz = cz2 - rz - uuz;
		A.nx = 0.0f; A.ny = 0.0f; A.nz = 1.0f; A.color = 0xFF0000FFu;
		MeshRasterVertex& B = t.verts[1];
		B.px = cx2 + rx - uux; B.py = cy2 + ry - uuy; B.pz = cz2 + rz - uuz;
		B.nx = 0.0f; B.ny = 0.0f; B.nz = 1.0f; B.color = 0xFF00FF00u;
		MeshRasterVertex& C = t.verts[2];
		C.px = cx2 + uux * 1.5f; C.py = cy2 + uuy * 1.5f; C.pz = cz2 + uuz * 1.5f;
		C.nx = 0.0f; C.ny = 0.0f; C.nz = 1.0f; C.color = 0xFFFF0000u;
		t.uvs = { 0.2f, 0.2f, 0.0f, 0.8f, 0.2f, 0.0f, 0.5f, 0.8f, 0.0f }; // S38: 3 floats/vertex
		t.idx = { 0u, 1u, 2u };
		t.texId = 1; // white fallback
		memset(t.world, 0, sizeof(t.world));
		t.world[0] = t.world[5] = t.world[10] = t.world[15] = 1.0f;
		t.alpha = 1.0f; // S4v: the test triangle must be opaque (the GML's
		                // last tl alpha would otherwise hide it)
		s_queue.push_back(std::move(t));
		XrDiagLine("diagtri injected (5 units ahead, identity model, white tex)");
	}
	}

	// S4u: stable rasters + draw items - built AFTER the diagtri push_back
	// above so no s_queue reallocation can dangle the matrix pointers.
	// S50: vectors reused across frames (clear keeps capacity; no per-frame
	// heap churn).
	static std::vector<MeshRaster> rasters;
	static std::vector<IRenderBackend::MeshDrawItem> items;
	rasters.clear(); rasters.resize(s_queue.size());
	items.clear(); items.resize(s_queue.size());
	for (size_t q = 0; q < s_queue.size(); q++) {
		rasters[q].vertices = s_queue[q].verts.data();
		rasters[q].vertexCount = (uint32_t)s_queue[q].verts.size();
		rasters[q].indices = s_queue[q].idx.data();
		rasters[q].indexCount = (uint32_t)s_queue[q].idx.size();
		rasters[q].cookie = s_queue[q].cookie; // S50: 0 = legacy always-upload
		items[q].mesh = &rasters[q];
		items[q].modelMatrix = s_queue[q].world;
		// S4: register this mesh's UVs + texture with the backend's
		// side channel (keyed by the vertex array pointer).
		if (!s_queue[q].uvs.empty()) {
			const bool profReg = ProfOn();
			LARGE_INTEGER pR0 = {};
			if (profReg) QueryPerformanceCounter(&pR0);
			XrTileSetMeshData(s_queue[q].verts.data(), s_queue[q].uvs.data(),
				(uint32_t)s_queue[q].verts.size(), s_queue[q].texId, s_queue[q].alpha, s_queue[q].emissive);
			if (profReg) sProfReg += QpcMsSince(pR0);
		}
	}

	// S4t: hand the background state to the backend (sky clear color +
	// optional full-screen image; the GL name resolves here because the
	// page upload happens earlier this frame via xr_bridge_tex).
	GLuint bgTex = 0;
	if (s_bgImgActive) {
		auto it = s_tex.find(s_bgImgKey);
		if (it != s_tex.end()) bgTex = it->second;
	}
	XrTileSetBackground(s_bgSkySet ? 1 : 0, s_bgSky[0], s_bgSky[1], s_bgSky[2],
		bgTex != 0 ? 1 : 0, (unsigned)bgTex,
		(float)s_bgUx, (float)s_bgUy, (float)s_bgUw, (float)s_bgUh);

	LARGE_INTEGER freq, t0, t1;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	const bool profFrame = ProfOn();
	LARGE_INTEGER tA = t0;
	if (profFrame) QueryPerformanceCounter(&tA);
	s_backend->RenderMeshesX(rv, items.data(), (uint32_t)items.size());
	if (profFrame) sProfRender = QpcMsSince(tA);
	// S4 probe: catch per-frame GL errors (first 5 frames, to the log FILE).
	if (frameIndexTotal < 5) {
		GLenum gerr = glGetError();
		char dbg[96];
		snprintf(dbg, sizeof(dbg), "frame %d postRender glErr=0x%04X\r\n", frameIndexTotal, (unsigned)gerr);
		char lp[MAX_PATH];
		snprintf(lp, sizeof(lp), "%s\\xr_bridge_log.txt", s_outDir.c_str());
		FILE* lf = fopen(lp, "ab");
		if (lf) { fputs(dbg, lf); fclose(lf); }
	}
	if (profFrame) QueryPerformanceCounter(&tA);
	const uint32_t* px = s_backend->GetMeshPixels();
	if (profFrame) sProfGetPx = QpcMsSince(tA);
	// Disk discipline (C-drive flood): XR_BRIDGE_BMPS_FROM/TO limit BMP saving
	// to a frame window (default = every frame, the old behavior).
	static int bmpFrom = -1, bmpTo = -1;
	if (bmpFrom < 0) {
		const char* ef = getenv("XR_BRIDGE_BMPS_FROM"); bmpFrom = (ef && *ef) ? atoi(ef) : 0;
		const char* et = getenv("XR_BRIDGE_BMPS_TO");   bmpTo = (et && *et) ? atoi(et) : 0x7FFFFFFF;
	}
	int saved = 0;
	if (px) {
		s_framePending = true;
		if (s_bmpsEnabled && frameIndex >= bmpFrom && frameIndex <= bmpTo) {
			char path[MAX_PATH];
			snprintf(path, sizeof(path), "%s\\xr_%06d.bmp", s_outDir.c_str(), frameIndex);
			SaveBmp(path, px, s_W, s_H);
			saved = 1;
		}
	} else {
		OutputDebugStringA("NO PIXELS\r\n");
	}
	// S56h DEPTH VIZ: at the eye frame, dump the mesh FBO's depth buffer as
	// grayscale (near=white) + log depth/color samples in the eye bbox -
	// decides occlusion vs not-drawn for the missing eyeballs.
	if (frameIndexTotal == 810) {
		typedef void (WINAPI * PFN_BFB)(GLenum, GLuint);
		PFN_BFB pfnBindFB = (PFN_BFB)wglGetProcAddress("glBindFramebuffer");
		GLint fboNow = 0;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboNow);
		GLint readNow = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readNow);
		if (pfnBindFB) { pfnBindFB(GL_DRAW_FRAMEBUFFER, (GLuint)fboNow); pfnBindFB(GL_READ_FRAMEBUFFER, (GLuint)fboNow); }
		std::vector<float> depth((size_t)s_W * s_H, 0.f);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, s_W, s_H, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
		GLenum derr = glGetError();
		// grayscale BMP, GL bottom-up -> BMP top-down
		std::vector<uint32_t> gray((size_t)s_W * s_H, 0);
		for (int y = 0; y < s_H; y++) {
			const float* src = &depth[(size_t)(s_H - 1 - y) * s_W];
			uint32_t* dst = &gray[(size_t)y * s_W];
			for (int x = 0; x < s_W; x++) {
				int v = (int)(src[x] * 255.0f + 0.5f);
				if (v > 255) v = 255;
				dst[x] = 0xFF000000u | (v << 16) | (v << 8) | v;
			}
		}
		char dp[MAX_PATH];
		snprintf(dp, sizeof(dp), "%s\\depth_%06d.bmp", s_outDir.c_str(), frameIndex);
		SaveBmp(dp, gray.data(), s_W, s_H);
		// depth+color samples across the native eye band (bmp coords)
		char db[512];
		snprintf(db, sizeof(db), "depthviz f=%d fbo=%d read=%d glErr=0x%04X\r\n",
			frameIndex, fboNow, readNow, (unsigned)derr);
		char lp2[MAX_PATH];
		snprintf(lp2, sizeof(lp2), "%s\\xr_bridge_log.txt", s_outDir.c_str());
		FILE* lf2 = fopen(lp2, "ab");
		if (lf2) { fputs(db, lf2); fclose(lf2); }
		struct { int x, y; } pts[8] = {
			{2073, 1045}, {2000, 1045}, {2150, 1045}, {2073, 950},
			{2073, 1150}, {1920, 540}, {2073, 1500}, {2500, 1045}
		};
		std::vector<uint32_t> colr(8, 0);
		glReadBuffer(fboNow == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0);
		for (int i = 0; i < 8; i++) {
			float d = 0.f;
			glReadPixels(pts[i].x, s_H - 1 - pts[i].y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d);
			uint32_t c = 0;
			glReadPixels(pts[i].x, s_H - 1 - pts[i].y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &c);
			colr[i] = c;
			char sb2[160];
			snprintf(sb2, sizeof(sb2), "depthpt (%d,%d) depth=%.5f rgba=%08X\r\n",
				pts[i].x, pts[i].y, d, c);
			FILE* lf3 = fopen(lp2, "ab");
			if (lf3) { fputs(sb2, lf3); fclose(lf3); }
		}
		if (pfnBindFB) pfnBindFB(GL_READ_FRAMEBUFFER, (GLuint)readNow);
	}
	QueryPerformanceCounter(&t1);
	const int ms = (int)((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
	char msg[260];
	snprintf(msg, sizeof(msg), "frame %d: %d draws, %d ms, texCalls=%llu nonzero=%llu uvHits=%llu uvMiss=%llu binds=%llu reg=%llu core=%llu whiteBinds=%llu realBinds=%llu%s\r\n",
		frameIndex, (int)s_queue.size(), ms, (unsigned long long)s_statTexCalls, (unsigned long long)s_statTexNonzero,
		(unsigned long long)XrTileDebugHits(), (unsigned long long)XrTileDebugMiss(),
		(unsigned long long)XrTileDebugTexBinds(), (unsigned long long)XrTileDebugRegistered(),
		(unsigned long long)XrTileDebugCore(), (unsigned long long)s_statWhiteBinds, (unsigned long long)s_statRealBinds, saved ? "" : " (NO PIXELS)");
	OutputDebugStringA(msg);
	// S41: lightweight buffered timing log (one line per frame, flushed
	// periodically so a hard kill keeps most data; no fopen/fclose per frame).
	if (s_timing) {
		fputs(msg, s_timing);
		static int sTimingFlush = 0;
		if ((++sTimingFlush % 60) == 0) fflush(s_timing);
		if (profFrame) {
			char pm[256];
			snprintf(pm, sizeof(pm), "prof f%d render=%.2f getpx=%.2f draw=%.2f bake=%.2f reg=%.2f swiz=%.2f\r\n",
				frameIndex, sProfRender, sProfGetPx, sProfDraw, sProfBake, sProfReg, sProfSwiz);
			fputs(pm, s_timing);
			sProfDraw = sProfBake = sProfReg = sProfSwiz = 0;
		}
	}
	// S4d DIAG: framebuffer/window state at frames 450 AND 900 (read-only).
	if (frameIndex == 420 || frameIndex == 450 || frameIndex == 900) {
		HWND wnd = FindWindowA("XrBridgeWnd", nullptr);
		RECT rc = { 0, 0, 0, 0 }, cr = { 0, 0, 0, 0 };
		POINT cTL = { 0, 0 }, cBR = { 0, 0 };
		BOOL got = FALSE, gotC = FALSE, zoomed = FALSE;
		if (wnd) {
			got = GetWindowRect(wnd, &rc);
			gotC = GetClientRect(wnd, &cr);
			cTL = { cr.left, cr.top }; cBR = { cr.right, cr.bottom };
			ClientToScreen(wnd, &cTL); ClientToScreen(wnd, &cBR);
			zoomed = IsZoomed(wnd);
		}
		// DPI (dynamic; mark unavailable on failure)
		typedef UINT (WINAPI * PFN_GDFW)(HWND);
		PFN_GDFW pDpiW = (PFN_GDFW)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
		UINT dpiW = (pDpiW && wnd) ? pDpiW(wnd) : 0;
		typedef void* (WINAPI * PFN_GPDAC)(void);
		typedef void* (WINAPI * PFN_GTDAC)(void);
		typedef int (WINAPI * PFN_GAFDAC)(void*);
		PFN_GPDAC pGPDAC = (PFN_GPDAC)GetProcAddress(GetModuleHandleA("user32.dll"), "GetProcessDpiAwarenessContext");
		PFN_GTDAC pGTDAC = (PFN_GTDAC)GetProcAddress(GetModuleHandleA("user32.dll"), "GetThreadDpiAwarenessContext");
		PFN_GAFDAC pGAF = (PFN_GAFDAC)GetProcAddress(GetModuleHandleA("user32.dll"), "GetAwarenessFromDpiAwarenessContext");
		int procAw = -1, thrAw = -1;
		if (pGAF && pGPDAC) procAw = pGAF(pGPDAC());
		if (pGAF && pGTDAC) thrAw = pGAF(pGTDAC());
		GLint dfb = 0, rfb = 0, vp[4] = { 0, 0, 0, 0 }, dbuf = 0, rbuf = 0;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
		glGetIntegerv(GL_VIEWPORT, vp);
		glGetIntegerv(GL_DRAW_BUFFER, &dbuf);
		glGetIntegerv(GL_READ_BUFFER, &rbuf);
		typedef GLenum (WINAPI * PFN_CHKFB2)(GLenum);
		PFN_CHKFB2 pfnChkFb2 = (PFN_CHKFB2)wglGetProcAddress("glCheckFramebufferStatus");
		GLenum st2 = pfnChkFb2 ? pfnChkFb2(0x8909) : 0;
		int nb = 0;
		if (px) {
			size_t total = (size_t)s_W * s_H;
			for (size_t i = 0; i < total; i++) if ((px[i] & 0x00FFFFFF) != 0) nb++;
		}
		char lp[MAX_PATH];
		snprintf(lp, sizeof(lp), "%s\\fb_state.txt", s_outDir.c_str());
		FILE* f = fopen(lp, "a");
		if (f) {
			fprintf(f, "frame %d: hwnd=%p wndRect=(%ld,%ld,%ld,%ld) got=%d clientRect=(%ld,%ld,%ld,%ld) clientTLscreen=(%ld,%ld) clientBRscreen=(%ld,%ld) zoomed=%d\r\n",
				frameIndex, (void*)wnd, rc.left, rc.top, rc.right, rc.bottom, (int)got,
				cr.left, cr.top, cr.right, cr.bottom, cTL.x, cTL.y, cBR.x, cBR.y, (int)zoomed);
			fprintf(f, "  dpi: windowDpi=%u procAwareness=%d threadAwareness=%d (0=unaware,1=system,2=permonitor,-1=unavailable)\r\n", dpiW, procAw, thrAw);
			fprintf(f, "  gl: dfb=%d rfb=%d status=0x%X drawBuf=0x%X readBuf=0x%X vp=(%d,%d,%d,%d) pxNonBlack=%d thread=%u ctx=%p\r\n",
				dfb, rfb, st2, dbuf, rbuf, vp[0], vp[1], vp[2], vp[3], nb, GetCurrentThreadId(),
				(void*)wglGetCurrentContext());
			fclose(f);
		}
		// S4v: dump several early frames too (the whole-project scaffold
		// stretch shows from f0) plus 450.
		if (px && (frameIndex == 60 || frameIndex == 150 || frameIndex == 300 || frameIndex == 450 || frameIndex == 1290 || (frameIndexTotal >= 1288 && frameIndexTotal <= 1294) || frameIndexTotal == 1680 || frameIndexTotal == 600 || frameIndex == 597 || frameIndex == 601)) {
			char pp[MAX_PATH];
			snprintf(pp, sizeof(pp), "%s\\f%d_gpu.bmp", s_outDir.c_str(), frameIndex);
			SaveBmp(pp, px, s_W, s_H);
		}
	}
	// S4c DIAG (frame 900): UV coverage grids + CPU expected colors + one GPU frame.
	if (frameIndex == 900 || frameIndex == 597 || frameIndex == 601) {
		char p1[MAX_PATH], p2[MAX_PATH], p3[MAX_PATH];
		snprintf(p1, sizeof(p1), "%s\\f900_uvgrid.txt", s_outDir.c_str());
		snprintf(p2, sizeof(p2), "%s\\f900_expected.txt", s_outDir.c_str());
		snprintf(p3, sizeof(p3), "%s\\f900_gpu.bmp", s_outDir.c_str());
		FILE* g = fopen(p1, "w");
		FILE* e = fopen(p2, "w");
		if (g) {
			for (auto& kv : s_diagUvGrid) {
				fprintf(g, "key=%llu\r\n", (unsigned long long)kv.first);
				for (int r = 0; r < 16; r++) {
					for (int c = 0; c < 16; c++)
						fputc((kv.second[r] >> c) & 1 ? '#' : '.', g);
					fputc('\r', g); fputc('\n', g);
				}
			}
			fclose(g);
		}
		if (e) {
			fprintf(e, "approx: nearest sample at each covered 16x16 cell center; GPU = texel * vColor(lit~0.51-0.85 x tint)\r\n");
			for (auto& kv : s_diagUvGrid) {
				auto td = s_diagTexData.find(kv.first);
				if (td == s_diagTexData.end()) {
					fprintf(e, "key=%llu  (page or >4MB: no retained RGBA)\r\n", (unsigned long long)kv.first);
					continue;
				}
				int w = s_diagTexSize[kv.first].first, h = s_diagTexSize[kv.first].second;
				int covered = 0, black = 0; double sr = 0, sg = 0, sb = 0;
				for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) {
					if (!((kv.second[r] >> c) & 1)) continue;
					covered++;
					int x = (int)(((c + 0.5) / 16.0) * w); if (x >= w) x = w - 1;
					int y = (int)(((r + 0.5) / 16.0) * h); if (y >= h) y = h - 1;
					const uint8_t* p = &td->second[((size_t)y * w + x) * 4];
					if (p[0] <= 8 && p[1] <= 8 && p[2] <= 8) black++;
					sr += p[0]; sg += p[1]; sb += p[2];
				}
				if (covered)
					fprintf(e, "key=%llu %dx%d covered=%d/256 black=%d(%.0f%%) avgRGB=%.0f,%.0f,%.0f\r\n",
						(unsigned long long)kv.first, w, h, covered, black,
						100.0 * black / covered, sr / covered, sg / covered, sb / covered);
			}
			fclose(e);
		}
		if (px) SaveBmp(p3, px, s_W, s_H);
		// S4d DIAG: GPU texture object readback + framebuffer verification.
		// Read-only; saves/restores GL_TEXTURE_BINDING_2D and GL_PACK_ALIGNMENT.
		// Runs after the backend's own readback, so frame rendering is unaffected.
		{
			char p4[MAX_PATH];
			snprintf(p4, sizeof(p4), "%s\\f900_readback.txt", s_outDir.c_str());
			FILE* rf = fopen(p4, "w");
			fprintf(rf, "thread=%u ctx=%p dc=%p\r\n", GetCurrentThreadId(),
				(void*)wglGetCurrentContext(), (void*)wglGetCurrentDC());
			typedef GLenum (WINAPI * PFN_CHKFB)(GLenum);
			PFN_CHKFB pfnChkFb = (PFN_CHKFB)wglGetProcAddress("glCheckFramebufferStatus");
			for (auto& kv : s_diagTexData) {
				auto szIt = s_diagTexSize.find(kv.first);
				if (szIt == s_diagTexSize.end()) continue;
				int w = szIt->second.first, h = szIt->second.second;
				if (w != 128 || h != 128) continue; // the character skins this session
				auto glIt = s_tex.find(kv.first);
				GLuint glName = (glIt != s_tex.end()) ? glIt->second : 0;
				GLint bound = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
				glBindTexture(GL_TEXTURE_2D, glName);
				GLint tw = 0, th = 0, ifmt = 0, mf = 0, mg = 0;
				glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
				glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
				glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
				glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mf);
				glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mg);
				GLint pa = 0; glGetIntegerv(GL_PACK_ALIGNMENT, &pa);
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				std::vector<uint8_t> gpu((size_t)tw * th * 4, 0);
				glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, gpu.data());
				GLenum errRb = glGetError();
				glBindTexture(GL_TEXTURE_2D, bound);
				glPixelStorei(GL_PACK_ALIGNMENT, pa);
				const std::vector<uint8_t>& cpu = kv.second;
				int n = w * h, black = 0, maxDiff = 0;
				double sr = 0, sg = 0, sb = 0, sumDiff = 0;
				for (int i = 0; i < n; i++) {
					const uint8_t* gp = &gpu[(size_t)i * 4];
					const uint8_t* cp = &cpu[(size_t)i * 4];
					if (gp[0] <= 8 && gp[1] <= 8 && gp[2] <= 8) black++;
					sr += gp[0]; sg += gp[1]; sb += gp[2];
					for (int ch = 0; ch < 3; ch++) {
						int d = gp[ch] > cp[ch] ? gp[ch] - cp[ch] : cp[ch] - gp[ch];
						if (d > maxDiff) maxDiff = d;
						sumDiff += d;
					}
				}
				fprintf(rf, "tex key=%llu gl=%u glSize=%dx%d ifmt=0x%X minFilter=%d magFilter=%d errRb=0x%04X\r\n",
					(unsigned long long)kv.first, glName, tw, th, ifmt, mf, mg, errRb);
				fprintf(rf, "  gpuAvg=(%.0f,%.0f,%.0f) gpuBlack=%d/%d(%.0f%%) cpuBlack~82%%pink maxDiff=%d avgDiff=%.2f\r\n",
					sr / n, sg / n, sb / n, black, n, 100.0 * black / n, maxDiff, sumDiff / (n * 3.0));
				for (int s = 0; s < 3; s++) {
					int i = (n / 4) * s + 7;
					fprintf(rf, "  px[%d]: gpu=(%d,%d,%d) cpu=(%d,%d,%d)\r\n", i,
						gpu[(size_t)i * 4], gpu[(size_t)i * 4 + 1], gpu[(size_t)i * 4 + 2],
						cpu[(size_t)i * 4], cpu[(size_t)i * 4 + 1], cpu[(size_t)i * 4 + 2]);
				}
				char pp[MAX_PATH];
				snprintf(pp, sizeof(pp), "%s\\f900_texreadback_%llu.bmp", s_outDir.c_str(),
					(unsigned long long)kv.first);
				SaveBmp(pp, (const uint32_t*)gpu.data(), tw, th);
			}
			GLint dfb = 0, rfb = 0, vp[4] = { 0, 0, 0, 0 };
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
			glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
			glGetIntegerv(GL_VIEWPORT, vp);
			GLenum st = pfnChkFb ? pfnChkFb(0x8909) : 0;
			GLenum e0 = glGetError();
			fprintf(rf, "fb: draw=%d read=%d status=0x%X vp=(%d,%d,%d,%d) errAfterQuery=0x%04X\r\n",
				dfb, rfb, st, vp[0], vp[1], vp[2], vp[3], e0);
			// S56p: read back EVERY registered GL texture (the alias map
			// included) - the eyeball's texId content was the last unknown.
			{
				GLint bound2 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound2);
				GLint pa2 = 0; glGetIntegerv(GL_PACK_ALIGNMENT, &pa2);
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				for (auto& skv : s_tex) {
					if (skv.first == 1) continue; // the white fallback
					GLuint gn = skv.second;
					if (gn == 0) continue;
					GLint tw2 = 0, th2 = 0;
					glBindTexture(GL_TEXTURE_2D, gn);
					glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw2);
					glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th2);
					if (tw2 <= 0 || th2 <= 0 || tw2 > 2048 || th2 > 2048) continue;
					std::vector<uint8_t> gpu2((size_t)tw2 * th2 * 4, 0);
					glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, gpu2.data());
					char tp[MAX_PATH];
					snprintf(tp, sizeof(tp), "%s\\texgl_%u_key%llu_%dx%d.bmp",
						s_outDir.c_str(), gn, (unsigned long long)skv.first, tw2, th2);
					SaveBmp(tp, (const uint32_t*)gpu2.data(), tw2, th2);
				}
				glBindTexture(GL_TEXTURE_2D, bound2);
				glPixelStorei(GL_PACK_ALIGNMENT, pa2);
			}
			if (px) {
				// S4e DIAG: read the DRAWABLE region for both A/B groups
				// (min(drawable, viewport)), per phase-8 readback alignment.
				HWND rbWnd = FindWindowA("XrBridgeWnd", nullptr);
				RECT rbCr = { 0, 0, 0, 0 };
				int rbW = s_W, rbH = s_H;
				if (rbWnd && GetClientRect(rbWnd, &rbCr)) {
					rbW = rbCr.right - rbCr.left; rbH = rbCr.bottom - rbCr.top;
					if (rbW > s_W) rbW = s_W;
					if (rbH > s_H) rbH = s_H;
				}
				if (rbW < 1) rbW = 1; if (rbH < 1) rbH = 1;
				const char* abE = getenv("XFAWA_VP_AB");
				int abGroup = (abE && atoi(abE) != 0) ? 1 : 0;
				std::vector<uint32_t> fb((size_t)rbW * rbH, 0);
				GLint pa = 0; glGetIntegerv(GL_PACK_ALIGNMENT, &pa);
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				glReadPixels(0, 0, rbW, rbH, GL_BGRA, GL_UNSIGNED_BYTE, fb.data());
				GLenum e1 = glGetError();
				glPixelStorei(GL_PACK_ALIGNMENT, pa);
				int same = 0, diff = 0, fbNb = 0, pxNb = 0;
				size_t total = (size_t)rbW * rbH;
				int minx = INT_MAX, maxx = -1, miny = INT_MAX, maxy = -1;
				for (size_t i = 0; i < total; i++) {
					if (fb[i] == px[i]) same++; else diff++;
					if ((fb[i] & 0x00FFFFFF) != 0) {
						fbNb++;
						int x = (int)(i % (size_t)rbW), y = (int)(i / (size_t)rbW);
						if (x < minx) minx = x; if (x > maxx) maxx = x;
						if (y < miny) miny = y; if (y > maxy) maxy = y;
					}
					if ((px[i] & 0x00FFFFFF) != 0) pxNb++;
				}
				fprintf(rf, "group=%s glReadPixels: origin=(0,0) size=%dx%d (drawable client) fmt=GL_BGRA type=GL_UNSIGNED_BYTE bytes=%llu err=0x%04X same=%d diff=%d fbNonBlack=%d pxNonBlack(full3840)=%d (total=%d)\r\n",
					abGroup ? "B" : "A", rbW, rbH, (unsigned long long)(total * 4), e1, same, diff, fbNb, pxNb, (int)((size_t)s_W * s_H));
				if (fbNb > 0 && maxx >= 0)
					fprintf(rf, "  nonBlackBbox(GL-bottom-up y): x=[%d,%d] y=[%d,%d]\r\n", minx, maxx, miny, maxy);
				struct Corn { const char* n; int x0, y0; } corns[4] = {
					{"BL", 0, 0}, {"BR", rbW - 128, 0}, {"TL", 0, rbH - 128}, {"TR", rbW - 128, rbH - 128} };
				for (int ci = 0; ci < 4; ci++) {
					int cnb = 0;
					for (int yy = 0; yy < 128; yy++) for (int xx = 0; xx < 128; xx++) {
						uint32_t v = fb[(size_t)(corns[ci].y0 + yy) * rbW + (corns[ci].x0 + xx)];
						if ((v & 0x00FFFFFF) != 0) cnb++;
					}
					fprintf(rf, "  corner %s (%d,%d)+128x128: nonBlack=%d/16384\r\n", corns[ci].n, corns[ci].x0, corns[ci].y0, cnb);
				}
				char pp[MAX_PATH];
				snprintf(pp, sizeof(pp), "%s\\f900_fbreadback_%s.bmp", s_outDir.c_str(), abGroup ? "B" : "A");
				SaveBmp(pp, fb.data(), rbW, rbH);
				// px drawable-region crop (bottom-up BGRA: rows 0..rbH = GL bottom)
				char pp2[MAX_PATH];
				snprintf(pp2, sizeof(pp2), "%s\\f900_pxdrawable_%s.bmp", s_outDir.c_str(), abGroup ? "B" : "A");
				SaveBmp(pp2, px, rbW, rbH);
			} else {
				fprintf(rf, "glReadPixels: skipped (px null)\r\n");
			}
			fclose(rf);
		}
	}
	s_queue.clear();
	frameIndexTotal++;
	return ms;
}

