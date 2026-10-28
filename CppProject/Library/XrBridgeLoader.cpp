// XrBridgeLoader.cpp - S2-B in-process render bridge loader.
//
// Loads xr_bridge.dll (built locally from XGraphics sources, never shipped
// or committed) from the executable's directory when present, and forwards
// per-frame render calls to it through a small C ABI. When the DLL is
// absent every function below returns 0 and default behavior is unchanged,
// so the CI leak audit stays clean: this file contains no XGraphics code,
// only pointer plumbing into MI's own vertex buffers.
//
// Frame flow (called from xr_bridge_frame() on each export frame):
//   lib_xr_bridge_init(w, h, prefix)          once
//   lib_xr_bridge_begin_frame(cam...)         per frame
//     lib_xr_bridge_set_matrix(world m16)     per item
//     lib_xr_bridge_set_shape_matrix(m16)     per BODYPART shape
//     lib_xr_bridge_draw_vbuffer(vb, tint, isShape)
//   lib_xr_bridge_end_frame()                 renders + writes BMP
//
// Matrix convention: column-major (GameMaker), v' = M * v, translation
// m[12..14]. Shape matrices are composed world * shape inside the DLL,
// identical to the S2-A receiver. The DLL applies the MI Z-up -> Y-up
// permutation (S*M*S) and vertex y/z swaps internally.

#include "Generated/Scripts.hpp"

#include "Asset/VertexBuffer.hpp"
#include "Render/Mesh.hpp"
#include "Render/Vertex.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vector>

namespace CppProject
{
	// xr_bridge.dll C ABI.
	typedef int  (*XrInitFn)(int, int, const char*);
	typedef int  (*XrBeginFrameFn)(double, double, double, double, double, double, double, double, double, double);
	typedef void (*XrSetMatrixFn)(const double*);
	typedef void (*XrDrawMeshFn)(const void*, int, int, const void*, int, int);
	typedef int  (*XrEndFrameFn)(int);

	static HMODULE s_bridge = nullptr;
	static XrInitFn s_init = nullptr;
	static XrBeginFrameFn s_begin = nullptr;
	static XrSetMatrixFn s_setMatrix = nullptr;
	static XrSetMatrixFn s_setShape = nullptr;
	static XrDrawMeshFn s_draw = nullptr;
	static XrEndFrameFn s_end = nullptr;
	static bool s_tried = false;
	static int s_frameIndex = 0;

	static bool LoadBridge()
	{
		if (s_tried)
			return s_bridge != nullptr;
		s_tried = true;
		wchar_t exePath[MAX_PATH];
		if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
			return false;
		std::wstring dir(exePath);
		const size_t slash = dir.find_last_of(L"\\/");
		if (slash == std::wstring::npos)
			return false;
		dir = dir.substr(0, slash + 1) + L"xr_bridge.dll";
		s_bridge = LoadLibraryW(dir.c_str());
		if (!s_bridge)
			return false;
		s_init = (XrInitFn)GetProcAddress(s_bridge, "xr_bridge_init");
		s_begin = (XrBeginFrameFn)GetProcAddress(s_bridge, "xr_bridge_begin_frame");
		s_setMatrix = (XrSetMatrixFn)GetProcAddress(s_bridge, "xr_bridge_set_matrix");
		s_setShape = (XrSetMatrixFn)GetProcAddress(s_bridge, "xr_bridge_set_shape_matrix");
		s_draw = (XrDrawMeshFn)GetProcAddress(s_bridge, "xr_bridge_draw_mesh");
		s_end = (XrEndFrameFn)GetProcAddress(s_bridge, "xr_bridge_end_frame");
		if (!s_init || !s_begin || !s_setMatrix || !s_setShape || !s_draw || !s_end)
		{
			s_bridge = nullptr;
			return false;
		}
		return true;
	}

	// Initializes the bridge (hidden GL surface + mesh backend) and stores
	// the BMP output prefix. Returns 1 on success, 0 when the DLL is absent
	// or initialization failed.
	RealType lib_xr_bridge_init(RealType width, RealType height, StringType prefix)
	{
		if (!LoadBridge())
			return 0;
		const QByteArray u8 = ((QString)prefix).toUtf8();
		s_frameIndex = 0;
		return (RealType)s_init((int)(IntType)width, (int)(IntType)height, u8.constData());
	}

	// Camera convention (identical to xr_anim_dump): from = world_pos,
	// to = (0,1,0) transformed by the camera matrix, up = cm[8..10],
	// fov = max(1, CAM_FOV).
	RealType lib_xr_bridge_begin_frame(RealType fov, RealType fx, RealType fy, RealType fz,
		RealType tx, RealType ty, RealType tz, RealType ux, RealType uy, RealType uz)
	{
		if (!s_begin)
			return 0;
		return (RealType)s_begin((double)fov, (double)fx, (double)fy, (double)fz,
			(double)tx, (double)ty, (double)tz, (double)ux, (double)uy, (double)uz);
	}

	// Current item world matrix (matrix_render), column-major 16 floats.
	RealType lib_xr_bridge_set_matrix(RealType m0, RealType m1, RealType m2, RealType m3,
		RealType m4, RealType m5, RealType m6, RealType m7,
		RealType m8, RealType m9, RealType m10, RealType m11,
		RealType m12, RealType m13, RealType m14, RealType m15)
	{
		if (!s_setMatrix)
			return 0;
		const double m[16] = { (double)m0, (double)m1, (double)m2, (double)m3,
			(double)m4, (double)m5, (double)m6, (double)m7,
			(double)m8, (double)m9, (double)m10, (double)m11,
			(double)m12, (double)m13, (double)m14, (double)m15 };
		s_setMatrix(m);
		return 1;
	}

	// Current BODYPART shape-local matrix (shape.matrix); identity default.
	RealType lib_xr_bridge_set_shape_matrix(RealType m0, RealType m1, RealType m2, RealType m3,
		RealType m4, RealType m5, RealType m6, RealType m7,
		RealType m8, RealType m9, RealType m10, RealType m11,
		RealType m12, RealType m13, RealType m14, RealType m15)
	{
		if (!s_setShape)
			return 0;
		const double m[16] = { (double)m0, (double)m1, (double)m2, (double)m3,
			(double)m4, (double)m5, (double)m6, (double)m7,
			(double)m8, (double)m9, (double)m10, (double)m11,
			(double)m12, (double)m13, (double)m14, (double)m15 };
		s_setShape(m);
		return 1;
	}

	// Queues every mesh of a vertex buffer with the current matrix.
	// tint: GML color (0xBBGGRR) baked into vertex RGB like the dump does,
	// -1 = white. isShape: 1 = compose world * shape.matrix.
	// Returns the queued mesh count, < 0 on unavailable CPU data.
	RealType lib_xr_bridge_draw_vbuffer(RealType vbufferId, RealType tint, RealType isShape)
	{
		if (!s_draw)
			return 0;
		VertexBuffer* vb = FindVertexBuffer((IntType)vbufferId);
		if (!vb)
			return 0;

		float tr = 1.0f, tg = 1.0f, tb = 1.0f;
		const bool bake = tint >= 0;
		if (bake)
		{
			const quint32 t = (quint32)(IntType)tint;
			tr = (float)(t & 0xFF) / 255.0f;
			tg = (float)((t >> 8) & 0xFF) / 255.0f;
			tb = (float)((t >> 16) & 0xFF) / 255.0f;
		}

		const int stride = (int)sizeof(Vertex);
		int queued = 0;
		for (const Mesh<>* mesh : vb->meshes)
		{
			if (mesh->numVertices <= 0 || !mesh->vertexData.Data())
				continue;
			const void* vd = mesh->vertexData.Data();
			std::vector<uchar> baked;
			if (bake)
			{
				// Copy + bake tint into vertex RGB (bytes [r,g,b,a], r low),
				// same convention as lib_xr_vbuffer_save.
				const Vertex* src = mesh->vertexData.Data();
				baked.resize((size_t)mesh->numVertices * sizeof(Vertex));
				Vertex* dst = (Vertex*)baked.data();
				for (qint32 v = 0; v < mesh->numVertices; v++)
				{
					dst[v] = src[v];
					const quint32 c = src[v].color;
					const quint32 r = (quint32)((float)(c & 0xFF) * tr + 0.5f);
					const quint32 g = (quint32)((float)((c >> 8) & 0xFF) * tg + 0.5f);
					const quint32 b = (quint32)((float)((c >> 16) & 0xFF) * tb + 0.5f);
					dst[v].color = (c & 0xFF000000u) | (b << 16) | (g << 8) | r;
				}
				vd = baked.data();
			}
			s_draw(vd, mesh->numVertices, stride,
				mesh->numIndices > 0 ? mesh->indexData.Data() : nullptr,
				mesh->numIndices, isShape != 0 ? 1 : 0);
			queued++;
		}
		return (RealType)queued;
	}

	// Renders the queued frame, writes <prefix>\xr_<frame>.bmp, returns
	// the render+readback time in milliseconds (int).
	RealType lib_xr_bridge_end_frame()
	{
		if (!s_end)
			return 0;
		const int ms = s_end(s_frameIndex++);
		return (RealType)ms;
	}
}
