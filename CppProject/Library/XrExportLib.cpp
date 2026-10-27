// XrExportLib.cpp - env-gated geometry dump support for the XGraphics export
// bridge (S2). Provides native functions callable from GML via the standard
// external_call -> C++ replacement mechanism (same convention as FileLib).
// Nothing here changes default behavior: the functions only run when the
// GML-side dump hooks are enabled via environment variables.

#include "Generated/Scripts.hpp"

#include "Asset/Asset.hpp"
#include "Asset/VertexBuffer.hpp"
#include "Render/Mesh.hpp"
#include "Render/Vertex.hpp"

#include <QFile>

namespace CppProject
{
	// Dumps the CPU-side vertex+index data of a vertex buffer to a binary file.
	// Format (little-endian):
	//   u32 magic 0x31425658 ('XVB1')
	//   u32 numMeshes
	//   per mesh: u32 numVertices, u32 numIndices
	//   per mesh: Vertex[numVertices] (raw 28-byte struct: x,y,z floats,
	//             u32 packed normal, u32 packed RGBA color, u,v floats,
	//             u32 data, u32 packed tangent) then u32[numIndices]
	// tint: GML color int (0xBBGGRR) multiplied into every vertex's RGB
	//       before writing (bakes block category tints such as grass/water);
	//       pass -1 for white (no tint).
	// Returns the total vertex count, or < 0 on error:
	//   -1 bad id / empty buffer, -2 CPU data was released (mesh unavailable),
	//   -3 file could not be opened.
	RealType lib_xr_vbuffer_save(RealType vbufferId, StringType filename, RealType tint)
	{
		VertexBuffer* vb = FindVertexBuffer((IntType)vbufferId);
		if (!vb || vb->meshes.isEmpty())
			return -1;

		// Decode GML color tint (r = low byte), -1 = white.
		float tr = 1.0f, tg = 1.0f, tb = 1.0f;
		if (tint >= 0)
		{
			const quint32 t = (quint32)(IntType)tint;
			tr = (float)(t & 0xFF) / 255.0f;
			tg = (float)((t >> 8) & 0xFF) / 255.0f;
			tb = (float)((t >> 16) & 0xFF) / 255.0f;
		}

		// Size pass + CPU data availability check.
		qint64 totalBytes = 8;
		qint64 totalVertices = 0;
		for (const Mesh<>* mesh : vb->meshes)
		{
			if (mesh->numVertices > 0 && !mesh->vertexData.Data())
				return -2;
			if (mesh->numIndices > 0 && !mesh->indexData.Data())
				return -2;
			totalBytes += 8 + (qint64)mesh->numVertices * (qint64)sizeof(Vertex)
				+ (qint64)mesh->numIndices * 4;
			totalVertices += mesh->numVertices;
		}

		QByteArray bytes;
		bytes.resize((int)totalBytes);
		uchar* out = (uchar*)bytes.data();
		qint64 pos = 0;

		const quint32 magic = 0x31425658;
		memcpy(out + pos, &magic, 4); pos += 4;
		const quint32 numMeshes = (quint32)vb->meshes.size();
		memcpy(out + pos, &numMeshes, 4); pos += 4;

		for (const Mesh<>* mesh : vb->meshes)
		{
			const quint32 nv = (quint32)mesh->numVertices;
			const quint32 ni = (quint32)mesh->numIndices;
			memcpy(out + pos, &nv, 4); pos += 4;
			memcpy(out + pos, &ni, 4); pos += 4;
			if (nv)
			{
				if (tint >= 0)
				{
					// Copy + bake tint into vertex RGB (bytes [r,g,b,a], r low).
					const Vertex* src = mesh->vertexData.Data();
					Vertex* dst = (Vertex*)(out + pos);
					for (quint32 v = 0; v < nv; v++)
					{
						dst[v] = src[v];
						const quint32 c = src[v].color;
						const quint32 r = (quint32)((float)(c & 0xFF) * tr + 0.5f);
						const quint32 g = (quint32)((float)((c >> 8) & 0xFF) * tg + 0.5f);
						const quint32 b = (quint32)((float)((c >> 16) & 0xFF) * tb + 0.5f);
						dst[v].color = (c & 0xFF000000u) | (b << 16) | (g << 8) | r;
					}
				}
				else
					memcpy(out + pos, mesh->vertexData.Data(), (size_t)nv * sizeof(Vertex));
				pos += (qint64)nv * sizeof(Vertex);
			}
			if (ni)
			{
				memcpy(out + pos, mesh->indexData.Data(), (size_t)ni * 4);
				pos += (qint64)ni * 4;
			}
		}

		QFile file((QString)filename);
		if (!file.open(QIODevice::WriteOnly))
			return -3;
		file.write(bytes);
		file.close();

		return (RealType)totalVertices;
	}
}
