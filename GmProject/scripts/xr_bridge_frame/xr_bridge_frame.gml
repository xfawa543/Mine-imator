/// xr_bridge_frame()
/// @desc S2-B/S3/S4 in-process render bridge (EXPERIMENT). Called from
///       export_update() on every export frame. Renders the current frame
///       through xr_bridge.dll (loaded from the executable directory when
///       present; every call no-ops when it is missing). The DLL renders
///       the queued meshes via the XGraphics mesh path and feeds the frame
///       to the in-process encoder (S3 takeover) — and optionally BMPs.
///       S4: per-item textures resolved INLINE (TEXTURE_OBJ chain +
///       mc_res fallback + sprite_get_texture conversion + per-shape
///       override for BODYPARTs).
///       Enabled ONLY when the environment variable XR_BRIDGE is set to an
///       output directory, and only in C++ builds.

globalvar xr_bridge_ready
globalvar xr_bridge_did_render
globalvar xr_bridge_takeover_active

function xr_bridge_frame()
{
	if (!is_cpp())
	{
		xr_bridge_did_render = false
		return 0
	}
	var prefix = environment_get_variable("XR_BRIDGE")
	if (prefix = "" || string(prefix) = "-1")
	{
		xr_bridge_did_render = false
		return 0
	}

	// One-time init at export start (project video resolution).
	if (xr_bridge_ready != 1)
	{
		var r = external_call(lib_xr_bridge_init, project_video_width, project_video_height, prefix)
		if (r <= 0)
		{
			log("XR bridge init failed (xr_bridge.dll missing next to exe?)")
			xr_bridge_ready = 0
			xr_bridge_did_render = false
			return 0
		}
		xr_bridge_ready = 1
		log("XR bridge init ok", string(project_video_width) + "x" + string(project_video_height))
	}

	// Camera: same convention as xr_anim_dump (from = world_pos,
	// to = (0,1,0) through the camera matrix, up = cm[8..10]).
	if (timeline_camera != null)
	{
		var cm = timeline_camera.matrix
		var to = point3D_mul_matrix(point3D(0, 1, 0), cm)
		external_call(lib_xr_bridge_begin_frame, max(1, timeline_camera.value[e_value.CAM_FOV]),
			timeline_camera.world_pos[X], timeline_camera.world_pos[Y], timeline_camera.world_pos[Z],
			to[X], to[Y], to[Z], cm[8], cm[9], cm[10])
	}

	// Queue every render_list item with its current world matrix.
	for (var i = 0; i < ds_list_size(render_list); i++)
	{
		var tl = render_list[|i]
		if (tl = timeline_camera)
			continue
		var t = tl.type
		var m = tl.matrix_render

		// S4: per-item texture. TEXTURE_OBJ chain first; fallback = mc_res
		// (the block atlas, like render_world_block's "if (res = null)
		// res = mc_res" - plain cubes have no TEXTURE_OBJ). BODYPART:
		// model_part_shape_tex overrides (per-shape skin). Finally the
		// sprite id converts to the texture-page location id.
		var itemTex = 0
		var tobjT = tl.value_inherit[e_value.TEXTURE_OBJ]
		if (tobjT != null && !is_undefined(tobjT))
		{
			var btT = tobjT.block_sheet_texture
			if (btT != null && !is_undefined(btT))
				itemTex = btT
			else
			{
				var svT = tobjT.texture
				if (svT != null && !is_undefined(svT))
					itemTex = svT
			}
		}
		if (itemTex = 0)
		{
			var mcv = mc_res.block_sheet_texture
			if (mcv != null && !is_undefined(mcv))
				itemTex = mcv
		}
		if (t = e_tl_type.BODYPART)
		{
			var stexP = 0
			if (tl.model_part != null && ds_exists(tl.model_part.shape_list, ds_type_list))
			{
				var arrT = tl.model_part_shape_tex
				if (arrT != null && !is_undefined(arrT))
				{
					var tvT = arrT[0]
					if (tvT != null && !is_undefined(tvT))
						stexP = tvT
				}
			}
			if (stexP != 0)
				itemTex = stexP
		}
		if (itemTex > 0 && sprite_exists(itemTex))
			itemTex = sprite_get_texture(itemTex, 0)

		switch (t)
		{
			case e_tl_type.BODYPART:
			{
				// World matrix once, then per-shape local matrices; the DLL
				// composes world * shape (same order as the S2-A receiver).
				external_call(lib_xr_bridge_set_matrix,
					m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
					m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
				if (ds_exists(tl.model_shape_vbuffer_map, ds_type_map))
				{
					var key = ds_map_find_first(tl.model_shape_vbuffer_map)
					while (!is_undefined(key))
					{
						var vb = tl.model_shape_vbuffer_map[?key]
						if (vb != null && !vbuffer_is_empty(vb))
						{
							var sm = xr_bridge_shape_matrix_arr(tl, string_get_real(key))
							external_call(lib_xr_bridge_set_shape_matrix,
								sm[0], sm[1], sm[2], sm[3], sm[4], sm[5], sm[6], sm[7],
								sm[8], sm[9], sm[10], sm[11], sm[12], sm[13], sm[14], sm[15])
							external_call(lib_xr_bridge_set_texture, itemTex)
							external_call(lib_xr_bridge_draw_vbuffer, vb, -1, 1)
						}
						key = ds_map_find_next(tl.model_shape_vbuffer_map, key)
					}
				}
				break
			}

			case e_tl_type.BLOCK:
			{
				if (tl.temp = null)
					break
				var res = is_undefined(tl.render_res_diffuse) ? null : tl.render_res_diffuse
				var tintGrass = (res != null ? res.color_grass : -1)
				var tintFoliage = (res != null ? res.color_foliage : -1)
				var tintWater = (res != null ? res.color_water : -1)
				external_call(lib_xr_bridge_set_matrix,
					m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
					m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
				external_call(lib_xr_bridge_set_texture, itemTex)
				for (var d = 0; d < 3; d++)
				{
					for (var c = 0; c < 12; c++)
					{
						var vb = tl.temp.block_vbuffer[d, c]
						if (vb = null)
							continue
						if (vbuffer_is_empty(vb))
							continue
						var tint = -1
						if (c = e_block_vbuffer.GRASS)
							tint = tintGrass
						else if (c = e_block_vbuffer.FOLIAGE)
							tint = tintFoliage
						else if (c = e_block_vbuffer.WATER)
							tint = tintWater
						else if (c >= e_block_vbuffer.LEAVES_OAK && c <= e_block_vbuffer.LEAVES_MANGROVE)
							tint = tintFoliage
						external_call(lib_xr_bridge_draw_vbuffer, vb, tint, 0)
					}
				}
				break
			}

			case e_tl_type.SCENERY:
			{
				if (tl.temp = null || tl.temp.scenery = null)
					break
				var tintGrass = -1
				var tintFoliage = -1
				var tintWater = -1
				external_call(lib_xr_bridge_set_matrix,
					m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
					m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
				external_call(lib_xr_bridge_set_texture, itemTex)
				for (var d = 0; d < 3; d++)
				{
					for (var c = 0; c < 12; c++)
					{
						var vb = tl.temp.scenery.block_vbuffer[d, c]
						if (vb = null)
							continue
						if (vbuffer_is_empty(vb))
							continue
						external_call(lib_xr_bridge_draw_vbuffer, vb, -1, 0)
					}
				}
				break
			}

			case e_tl_type.MODEL:
			{
				if (tl.temp = null || tl.temp.model = null)
					break
				external_call(lib_xr_bridge_set_matrix,
					m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
					m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
				for (var d = 0; d < 3; d++)
				{
					for (var c = 0; c < 12; c++)
					{
						var vb = tl.temp.model.block_vbuffer[d, c]
						if (vb = null)
							continue
						if (vbuffer_is_empty(vb))
							continue
						external_call(lib_xr_bridge_draw_vbuffer, vb, -1, 0)
					}
				}
				break
			}

			case e_tl_type.ITEM:
			{
				var vb = (tl.item_vbuffer != null ? tl.item_vbuffer : tl.temp.item_vbuffer)
				external_call(lib_xr_bridge_set_texture, itemTex)
				xr_bridge_draw_single(vb, m, -1)
				break
			}

			case e_tl_type.TEXT:
			{
				external_call(lib_xr_bridge_set_texture, itemTex)
				xr_bridge_draw_single(tl.text_vbuffer, m, -1)
				break
			}

			case e_tl_type.PATH:
			{
				external_call(lib_xr_bridge_set_texture, itemTex)
				xr_bridge_draw_single(tl.path_vbuffer, m, -1)
				break
			}

			default:
			{
				if (tl.temp != null && !is_undefined(tl.temp.shape_vbuffer))
				{
					external_call(lib_xr_bridge_set_texture, itemTex)
					xr_bridge_draw_single(tl.temp.shape_vbuffer, m, -1)
				}
				break
			}
		}
	}

	// S3: when the bridge rendered this frame, its pixels are handed to the
	// in-process encoder via XrBridgeTakeFrame (MovieLib.cpp) and the
	// native D3D11 render is skipped by export_update.
	var ms = external_call(lib_xr_bridge_end_frame)
	xr_bridge_did_render = (ms > 0)
	return xr_bridge_did_render
}

function xr_bridge_draw_single(vb, m, tint)
{
	if (vb = null)
		return 0
	if (vbuffer_is_empty(vb))
		return 0
	external_call(lib_xr_bridge_set_matrix,
		m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
		m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
	external_call(lib_xr_bridge_draw_vbuffer, vb, tint, 0)
	return 1
}

function xr_bridge_shape_matrix_arr(tl, shapeId)
{
	// Returns the shape instance's local matrix array (16 floats), or an
	// identity array when the shape is not found.
	var ident = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
	if (tl.model_part = null)
		return ident
	if (ds_exists(tl.model_part.shape_list, ds_type_list))
	{
		for (var s = 0; s < ds_list_size(tl.model_part.shape_list); s++)
		{
			var shape = tl.model_part.shape_list[|s]
			if (shape.id = shapeId && !is_undefined(shape.matrix))
				return shape.matrix
		}
	}
	return ident
}
