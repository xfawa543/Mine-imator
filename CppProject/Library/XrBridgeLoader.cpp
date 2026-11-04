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
#include "Asset/Sprite.hpp"

#include <set>

#include "Asset/VertexBuffer.hpp"
#include "Render/Mesh.hpp"
#include "Render/Vertex.hpp"
#include "Render/Texture.hpp"
#include "Render/TexturePage.hpp"
#include "Render/GraphicsApiHandler.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vector>
#include <QHash>

namespace CppProject
{
	// xr_bridge.dll C ABI.
	typedef int  (*XrInitFn)(int, int, const char*);
	typedef int  (*XrBeginFrameFn)(double, double, double, double, double, double, double, double, double, double);
	typedef void (*XrSetMatrixFn)(const double*);
	typedef void (*XrDrawMeshFn)(const void*, int, int, const void*, int, int);
	typedef int  (*XrEndFrameFn)(int);
	typedef int  (*XrReadFrameFn)(uint8_t*, int, int);
	typedef int  (*XrTexFn)(uint64_t, int, int, const uint8_t*);
	typedef void (*XrSetTexFn)(uint64_t);
	typedef void (*XrSetUvRectFn)(double, double, double, double);
	typedef void (*XrRegTexFn)(uint64_t, uint64_t);
	typedef void (*XrRegTex2Fn)(uint64_t, uint64_t, double, double, double, double);
	typedef void (*XrSetTexForFn)(uint64_t);

	static HMODULE s_bridge = nullptr;
	static XrInitFn s_init = nullptr;
	static bool s_tried = false;
	static XrBeginFrameFn s_begin = nullptr;
	static XrSetMatrixFn s_setMatrix = nullptr;
	static XrSetMatrixFn s_setShape = nullptr;
	static XrDrawMeshFn s_draw = nullptr;
	static XrEndFrameFn s_end = nullptr;
	static XrReadFrameFn s_read = nullptr;
	static XrTexFn s_texUp = nullptr;
	static XrSetTexFn s_setTex = nullptr;
	static XrSetUvRectFn s_setUv = nullptr;
	static XrRegTexFn s_regTex = nullptr;
	static XrRegTex2Fn s_regTex2 = nullptr; // optional: + sprite uvRect
	static XrSetTexForFn s_setTexFor = nullptr;
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
		s_read = (XrReadFrameFn)GetProcAddress(s_bridge, "xr_bridge_read_frame");
		s_texUp = (XrTexFn)GetProcAddress(s_bridge, "xr_bridge_tex");
		s_setTex = (XrSetTexFn)GetProcAddress(s_bridge, "xr_bridge_set_texture");
		s_setUv = (XrSetUvRectFn)GetProcAddress(s_bridge, "xr_bridge_set_uvrect");
		s_regTex = (XrRegTexFn)GetProcAddress(s_bridge, "xr_bridge_register_item_tex");
		s_regTex2 = (XrRegTex2Fn)GetProcAddress(s_bridge, "xr_bridge_register_item_tex2");
		s_setTexFor = (XrSetTexForFn)GetProcAddress(s_bridge, "xr_bridge_set_tex_for");
		if (!s_init || !s_begin || !s_setMatrix || !s_setShape || !s_draw || !s_end || !s_read || !s_texUp || !s_setTex || !s_setUv || !s_regTex || !s_setTexFor)
		{
			s_bridge = nullptr;
			return false;
		}
		return true;
	}

	// S4: diagnostics file (<prefix>\xr_loader_log.txt), set at init.
	static std::string s_diagPath;

	static void XrDiag(const std::string& msg)
	{
		if (s_diagPath.empty())
			return;
		FILE* f = fopen(s_diagPath.c_str(), "ab");
		if (f)
		{
			fputs((msg + "\r\n").c_str(), f);
			fclose(f);
		}
	}

	// Initializes the bridge (hidden GL surface + mesh backend) and stores
	// the BMP output prefix. Returns 1 on success, 0 when the DLL is absent
	// or initialization failed.
	RealType lib_xr_bridge_init(RealType width, RealType height, StringType prefix)
	{
		if (!LoadBridge())
			return 0;
		const QByteArray u8 = ((QString)prefix).toUtf8();
		s_diagPath = std::string(u8.constData()) + "\\xr_loader_log.txt";
		XrDiag("init ok");
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

	// S3: copies the pending xr frame into the encoder's RGBA buffer.
	// Returns true when a bridge frame was consumed, false when the caller
	// should fall back to the native surface readback.
	bool XrBridgeTakeFrame(uchar* dst, IntType w, IntType h)
	{
		if (!s_read || !dst)
			return false;
		return s_read((uint8_t*)dst, (int)w, (int)h) != 0;
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

	// ==== S4 texture upload helpers (shared by auto_tex + reg_shape) ====
	static std::set<uint64_t> s_uploadedTex;

	static bool XrUploadQImage(const QImage& img, uint64_t key)
	{
		if (!s_texUp || img.isNull())
			return false;
		const int w = img.width(), h = img.height();
		QByteArray rgba((qint64)w * h * 4, 0);
		for (int y = 0; y < h; y++)
		{
			const uchar* src = img.constScanLine(y);
			uchar* dst = (uchar*)rgba.data() + (qint64)y * w * 4;
			memcpy(dst, src, w * 4);
		}
		s_texUp(key, w, h, (const uint8_t*)rgba.constData());
		return true;
	}

	// ==== S4: AUTO texture - upload ALL pages + diagnostics ====
	// Every texture page is uploaded once, keyed by the page pointer (unique,
	// stable, no dependency on lazy GL texture creation). The current texture
	// becomes the page holding mc_res.block_sheet_texture (fallback: first
	// uploaded page). Typed member access bypasses dynamic-index uncertainty.
	RealType lib_xr_bridge_auto_tex()
	{
		if (!s_texUp || !s_setTex)
		{
			XrDiag("auto_tex: pfns null");
			return 0;
		}
		int uploaded = 0;
		IntType firstKey = 0;
		const int pageCount = (int)TexturePage::pages.size();
		for (int pi = 0; pi < pageCount; pi++)
		{
			TexturePage* page = TexturePage::pages[pi];
			if (!page || page->image.isNull())
				continue;
			const IntType key = (IntType)(size_t)page;
			if (!s_uploadedTex.count((uint64_t)key))
			{
				s_uploadedTex.insert((uint64_t)key);
				XrUploadQImage(page->image, (uint64_t)key);
			}
			if (!firstKey)
				firstKey = key;
			uploaded++;
		}
		XrDiag("auto_tex: pages=" + std::to_string(pageCount) +
			" uploaded=" + std::to_string(uploaded));

		IntType curKey = firstKey;
		obj_resource* res = ObjTypeOpt(obj_resource, global::mc_res);
		const IntType texSprite = res ? res->block_sheet_texture : 0;
		XrDiag("auto_tex: mc_res=" + std::to_string((IntType)global::mc_res) +
			" block_sheet_texture=" + std::to_string(texSprite));
		if (texSprite > 0)
		{
			const IntType locId = (IntType)sprite_get_texture(texSprite, 0);
			TexturePageLocation* loc = TexturePageLocation::Find(locId);
			XrDiag("auto_tex: locId=" + std::to_string(locId) +
				" loc=" + std::to_string((size_t)loc) +
				" page=" + std::to_string(loc ? (size_t)loc->page : 0));
			if (loc && loc->page)
				curKey = (IntType)(size_t)loc->page;
		}
		XrDiag("auto_tex: curKey=" + std::to_string(curKey));
		s_setTex((uint64_t)curKey);
		return uploaded > 0 ? 1 : 0;
	}

	// ==== S4b: per-shape / per-item texture registry ====
	// Resolves a sprite to its texture and registers it under a GML-side
	// key (a shape instance id for BODYPART shapes, a tl id otherwise).
	// Paged sprites bind their page + the sprite's uvRect (vbuffer UVs are
	// sprite-relative 0-1 and the DLL bakes the rect per draw, replicating
	// the native shader's _uVvRect transform); standalone sprites (model
	// skins, useTexturePage off) bind their own texture with an identity
	// rect (UVs are 0-1 sprite-relative, matching MI's raw vTexCoord
	// shader sampling). Reads the sprite's frame pageLoc directly,
	// bypassing TexturePageLocation::Find (its idMap can go stale).
	RealType lib_xr_bridge_reg_shape(RealType shapeId, RealType spriteId)
	{
		if (!s_texUp || !s_regTex)
			return 0;
		const IntType sid = (IntType)spriteId;
		if (sid <= 0)
			return 0;
		Sprite* spr = FindSprite(sid);
		if (!spr || spr->frames.isEmpty())
			return 0;
		Sprite::Frame* fr = spr->frames[0];
		uint64_t key = 0;
		double ux = 0, uy = 0, uw = 1, uh = 1;
		if (fr->pageLoc && fr->pageLoc->page)
		{
			key = (uint64_t)(size_t)fr->pageLoc->page;
			if (!s_uploadedTex.count(key))
			{
				s_uploadedTex.insert(key);
				XrUploadQImage(fr->pageLoc->page->image, key);
			}
			// S4b: vbuffer UVs are sprite-relative 0-1; pass the sprite's
			// page uvRect so the DLL can map them onto the atlas (the old
			// "page-relative UVs" assumption was wrong).
			const UvRect& uv = fr->pageLoc->uvRect;
			ux = (double)uv.x; uy = (double)uv.y;
			uw = (double)uv.w; uh = (double)uv.h;
		}
		else if (!fr->image.isNull())
		{
			key = (uint64_t)(IntType)sid;
			if (!s_uploadedTex.count(key))
			{
				s_uploadedTex.insert(key);
				XrUploadQImage(fr->image, key);
			}
			XrDiag("reg_shape: standalone sprite " + std::to_string(sid) +
				" " + std::to_string(fr->image.width()) + "x" +
				std::to_string(fr->image.height()));
		}
		if (!key)
			return 0;
		if (s_regTex2)
			s_regTex2((uint64_t)(IntType)shapeId, key, ux, uy, uw, uh);
		else
			s_regTex((uint64_t)(IntType)shapeId, key);
		return 1;
	}
	// S4c DIAG: per-shape vbuffer state, recorded only at diag frame 900.
	// Called from xr_bridge_frame's registration loop for every BODYPART
	// shape every frame; state: 0=not in vbuffer map, 1=null, 2=empty, 3=ok.
	// Read-only: resolves the vbuffer for vertex/index counts, changes nothing.
	RealType lib_xr_bridge_diag_shape(RealType tlId, RealType shapeIdx, RealType vbId, RealType state)
	{
		if (s_frameIndex != 900)
			return 0;
		int verts = -1, idxs = -1, meshes = -1;
		IntType vbi = (IntType)vbId;
		if (vbi > 0)
		{
			VertexBuffer* vb = FindVertexBuffer(vbi);
			if (vb)
			{
				meshes = (int)vb->meshes.size();
				verts = 0; idxs = 0;
				for (const Mesh<>* m : vb->meshes)
					if (m) { verts += m->numVertices; idxs += m->numIndices; }
			}
			else meshes = -2; // id present but lookup failed
		}
		XrDiag("shape tl=" + std::to_string((IntType)tlId) +
			" idx=" + std::to_string((IntType)shapeIdx) +
			" vb=" + std::to_string(vbi) +
			" state=" + std::to_string((IntType)state) +
			" meshes=" + std::to_string(meshes) +
			" verts=" + std::to_string(verts) +
			" indices=" + std::to_string(idxs));
		return 1;
	}
}