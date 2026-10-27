/// xr_geo_dump()
/// @desc EXPERIMENT ONLY (XGraphics S2 export bridge, geometry side).
///       Called from export_update() on the first export frame (env-gated).
///       Dumps the CPU-side vertex/index data of every render_list item's
///       vertex buffers + a manifest joining geometry to timeline ids (the
///       per-frame matrices come from xr_anim_dump, joined by tl id).
///       Enabled ONLY when XR_GEO_DUMP is set to an output directory.
///       Per-vbuffer binary files (lib_xr_vbuffer_save, tint baked into
///       vertex colors): geo_<vbuffer id>.bin
///       Manifest lines:
///         GEODUMP version=1
///         ITEM tl=<id> type=<e_tl_type> kind=block cat=<d>*12+<c> file=geo_<id>.bin
///         ITEM tl=<id> type=<e_tl_type> kind=single file=geo_<id>.bin
///         SHAPE tl=<id> shape=<shape id> file=geo_<id>.bin mat=<16 floats>
///       Category tints are baked at dump time from the item's render
///       resource colors (grass/foliage/leaves/water), so the xr side needs
///       no tint logic.

globalvar xr_geo_dump_done

function xr_geo_dump_mat16(f, m)
{
	var s = ""
	for (var i = 0; i < 16; i++)
	{
		if (i > 0)
			s += ","
		s += string(m[i])
	}
	file_text_write_string(f, s)
	file_text_writeln(f)
}

function xr_geo_dump_one(f, dir, vbuffer, tag, tint)
{
	if (vbuffer = null)
		return 0
	if (vbuffer_is_empty(vbuffer))
		return 0
	var fn = dir + "/geo_" + string(vbuffer) + ".bin"
	var n = xr_vbuffer_save(vbuffer, fn, tint)
	if (n < 0)
		return 0
	file_text_write_string(f, tag + " file=geo_" + string(vbuffer) + ".bin verts=" + string(n))
	file_text_writeln(f)
	return 1
}

function xr_geo_dump_block_vbuffers(f, dir, blockVbuffer, tint)
{
	// block_vbuffer[e_block_depth, e_block_vbuffer] — 3 depths x 12 categories.
	for (var d = 0; d < 3; d++)
	{
		for (var c = 0; c < 12; c++)
		{
			var vb = blockVbuffer[d, c]
			if (vb = null)
				continue
			if (vbuffer_is_empty(vb))
				continue
			xr_geo_dump_one(f, dir, vb, "kind=block cat=" + string(d * 12 + c), tint)
		}
	}
}

function xr_geo_dump()
{
	var dir = environment_get_variable("XR_GEO_DUMP")
	if (dir = "" || string(dir) = "-1")
		return 0
	if (xr_geo_dump_done = 1)
		return 0
	xr_geo_dump_done = 1
	if (!is_cpp())
		return 0 // C++ builds only (native vbuffer access)

	if (!directory_exists_lib(dir))
		directory_create_lib(dir)

	var f = file_text_open_write(dir + "/geo_manifest.txt")
	file_text_write_string(f, "GEODUMP version=1")
	file_text_writeln(f)

	for (var i = 0; i < ds_list_size(render_list); i++)
	{
		var tl = render_list[|i]
		var t = tl.type
		var tlId = string(tl.id)

		switch (t)
		{
			case e_tl_type.BLOCK:
			{
				if (tl.temp = null)
					break
				// Category tints from the render resource (GML colors).
				var res = is_undefined(tl.render_res_diffuse) ? null : tl.render_res_diffuse
				var tintGrass = (res != null ? res.color_grass : -1)
				var tintFoliage = (res != null ? res.color_foliage : -1)
				var tintWater = (res != null ? res.color_water : -1)
				var tintWhite = -1
				for (var d = 0; d < 3; d++)
				{
					for (var c = 0; c < 12; c++)
					{
						var vb = tl.temp.block_vbuffer[d, c]
						if (vb = null)
							continue
						if (vbuffer_is_empty(vb))
							continue
						var tint = tintWhite
						if (c = e_block_vbuffer.GRASS)
							tint = tintGrass
						else if (c = e_block_vbuffer.FOLIAGE)
							tint = tintFoliage
						else if (c = e_block_vbuffer.WATER)
							tint = tintWater
						else if (c >= e_block_vbuffer.LEAVES_OAK && c <= e_block_vbuffer.LEAVES_MANGROVE)
							tint = tintFoliage
						xr_geo_dump_one(f, dir, vb, "ITEM tl=" + tlId + " type=BLOCK kind=block cat=" + string(d * 12 + c), tint)
					}
				}
				break
			}

			case e_tl_type.SCENERY:
			{
				if (tl.temp = null || tl.temp.scenery = null)
					break
				var res = is_undefined(tl.render_res_diffuse) ? null : tl.render_res_diffuse
				var tintGrass = (res != null ? res.color_grass : -1)
				var tintFoliage = (res != null ? res.color_foliage : -1)
				var tintWater = (res != null ? res.color_water : -1)
				var tintWhite = -1
				for (var d = 0; d < 3; d++)
				{
					for (var c = 0; c < 12; c++)
					{
						var vb = tl.temp.scenery.block_vbuffer[d, c]
						if (vb = null)
							continue
						if (vbuffer_is_empty(vb))
							continue
						var tint = tintWhite
						if (c = e_block_vbuffer.GRASS)
							tint = tintGrass
						else if (c = e_block_vbuffer.FOLIAGE)
							tint = tintFoliage
						else if (c = e_block_vbuffer.WATER)
							tint = tintWater
						else if (c >= e_block_vbuffer.LEAVES_OAK && c <= e_block_vbuffer.LEAVES_MANGROVE)
							tint = tintFoliage
						xr_geo_dump_one(f, dir, vb, "ITEM tl=" + tlId + " type=SCENERY kind=block cat=" + string(d * 12 + c), tint)
					}
				}
				break
			}

			case e_tl_type.MODEL:
			{
				if (tl.temp = null || tl.temp.model = null)
					break
				xr_geo_dump_block_vbuffers_plain(f, dir, tl.temp.model.block_vbuffer, id, "MODEL")
				break
			}

			case e_tl_type.ITEM:
			{
				var vb = (tl.item_vbuffer != null ? tl.item_vbuffer : tl.temp.item_vbuffer)
				xr_geo_dump_one(f, dir, vb, "ITEM tl=" + tlId + " type=ITEM kind=single", -1)
				break
			}

			case e_tl_type.TEXT:
			{
				xr_geo_dump_one(f, dir, tl.text_vbuffer, "ITEM tl=" + tlId + " type=TEXT kind=single", -1)
				break
			}

			case e_tl_type.PATH:
			{
				xr_geo_dump_one(f, dir, tl.path_vbuffer, "ITEM tl=" + tlId + " type=PATH kind=single", -1)
				break
			}

			case e_tl_type.BODYPART:
			{
				// Per-shape buffers + static shape matrices; per-frame the
				// renderer composes shape.matrix * matrix_render (from the
				// anim dump). Bent/deforming shapes are dumped at frame-0
				// state (v1 limitation).
				if (ds_exists(tl.model_shape_vbuffer_map, ds_type_map))
				{
					var key = ds_map_find_first(tl.model_shape_vbuffer_map)
					while (!is_undefined(key))
					{
						var shapeId = string_get_real(key)
						var vb = tl.model_shape_vbuffer_map[?key]
						var smat = xr_geo_dump_shape_matrix(tl, shapeId)
						if (!xr_geo_dump_one(f, dir, vb, "SHAPE tl=" + tlId + " shape=" + string(shapeId) + " mat=" + smat, -1))
							break
						key = ds_map_find_next(tl.model_shape_vbuffer_map, key)
					}
				}
				break
			}

			default:
			{
				// Shapes (CUBE/CONE/CYLINDER/SPHERE/SURFACE/...)
				if (tl.temp != null && !is_undefined(tl.temp.shape_vbuffer))
					xr_geo_dump_one(f, dir, tl.temp.shape_vbuffer, "ITEM tl=" + tlId + " type=" + string(t) + " kind=single", -1)
				break
			}
		}
	}

	file_text_close(f)
	log("XR geo dump complete", dir)
	return 1
}

function xr_geo_dump_block_vbuffers_plain(f, dir, blockVbuffer, tlId, typeName)
{
	for (var d = 0; d < 3; d++)
	{
		for (var c = 0; c < 12; c++)
		{
			var vb = blockVbuffer[d, c]
			if (vb = null)
				continue
			if (vbuffer_is_empty(vb))
				continue
			xr_geo_dump_one(f, dir, vb, "ITEM tl=" + tlId + " type=" + typeName + " kind=block cat=" + string(d * 12 + c), -1)
		}
	}
}

function xr_geo_dump_shape_matrix(tl, shapeId)
{
	// Find the shape instance in the model part's shape list and return its
	// matrix as a comma string (identity if not found).
	var ident = "1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1"
	if (tl.model_part = null)
		return ident
	if (ds_exists(tl.model_part.shape_list, ds_type_list))
	{
		for (var s = 0; s < ds_list_size(tl.model_part.shape_list); s++)
		{
			var shape = tl.model_part.shape_list[|s]
			if (shape.id = shapeId && !is_undefined(shape.matrix))
			{
				var m = shape.matrix
				var s2 = ""
				for (var i = 0; i < 16; i++)
				{
					if (i > 0)
						s2 += ","
					s2 += string(m[i])
				}
				return s2
			}
		}
	}
	return ident
}

function xr_vbuffer_save(vbuffer, filename, tint)
{
	// thin wrapper so the GML side reads like the C++ side
	return external_call(lib_xr_vbuffer_save, vbuffer, filename, tint)
}
