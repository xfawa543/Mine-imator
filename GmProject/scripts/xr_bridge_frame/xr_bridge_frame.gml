/// xr_bridge_frame()
/// @desc S4 textured render bridge. Called from export_update() each frame.
///       Textures resolved ONCE at init (reliable context) and registered
///       into the DLL's per-item map. Per frame, only tl.id is passed to
///       look up the stored texture — bypasses CppGen loop-scope issues.

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

	if (xr_bridge_ready != 1)
	{
		var r = external_call(lib_xr_bridge_init, project_video_width, project_video_height, prefix)
		if (r <= 0)
		{
			log("XR bridge init failed")
			xr_bridge_ready = 0
			xr_bridge_did_render = false
			return 0
		}
		xr_bridge_ready = 1
		log("XR bridge init ok", string(project_video_width) + "x" + string(project_video_height))
		// ==== S4: register all item textures ONCE at init ====
		// Init-time property reads are reliable (verified: mc=23761).
		// Loop-scope reads under CppGen resolve to 0, so we do all texture
		// resolution here and store the results in the DLL's registry.
		for (var ri = 0; ri < ds_list_size(render_list); ri++)
		{
			var rtl = render_list[|ri]
			if (rtl = timeline_camera)
				continue
			var rt = rtl.type
			var rtex = 0
			if (rt = e_tl_type.BODYPART)
			{
				if (rtl.model_part != null && ds_exists(rtl.model_part.shape_list, ds_type_list))
				{
					var arrT = rtl.model_part_shape_tex
					if (arrT != null && !is_undefined(arrT))
					{
						var tvT = arrT[0]
						if (tvT != null && !is_undefined(tvT))
							rtex = tvT
					}
				}
			}
			else if (rt = e_tl_type.BLOCK || rt = e_tl_type.SCENERY)
			{
				var resT = rtl.render_res_diffuse
				if (resT != null && !is_undefined(resT))
				{
					var btT = resT.block_sheet_texture
					if (btT != null && !is_undefined(btT))
						rtex = btT
				}
			}
			else if (rt = e_tl_type.ITEM)
			{
				var irT = rtl.item_res
				if (irT != null && !is_undefined(irT))
				{
					var itT = irT.item_sheet_texture
					if (itT != null && !is_undefined(itT))
						rtex = itT
				}
			}
			else if (rt = e_tl_type.TEXT)
			{
				if (rtl.text_texture != null && !is_undefined(rtl.text_texture))
					rtex = rtl.text_texture
			}
			else
			{
				var soT = rtl.value_inherit[e_value.TEXTURE_OBJ]
				if (soT != null && !is_undefined(soT))
				{
					var svT = soT.texture
					if (svT != null && !is_undefined(svT))
						rtex = svT
					else
					{
						var sbT = soT.block_sheet_texture
						if (sbT != null && !is_undefined(sbT))
							rtex = sbT
					}
				}
			}
			// Convert sprite id to texture-page location id
			if (rtex > 0 && sprite_exists(rtex))
				rtex = sprite_get_texture(rtex, 0)
			external_call(lib_xr_bridge_register_item_tex, rtl.id, rtex)
		}
		log("XR bridge texture registry done")
	}

	// Camera
	if (timeline_camera != null)
	{
		var cm = timeline_camera.matrix
		var to = point3D_mul_matrix(point3D(0, 1, 0), cm)
		external_call(lib_xr_bridge_begin_frame, max(1, timeline_camera.value[e_value.CAM_FOV]),
			timeline_camera.world_pos[X], timeline_camera.world_pos[Y], timeline_camera.world_pos[Z],
			to[X], to[Y], to[Z], cm[8], cm[9], cm[10])
	}

	for (var i = 0; i < ds_list_size(render_list); i++)
	{
		var tl = render_list[|i]
		if (tl = timeline_camera)
			continue
		var t = tl.type
		var m = tl.matrix_render

		// S4: per-frame texture lookup by tl id (DLL-side map)
		external_call(lib_xr_bridge_set_tex_for, tl.id)

		switch (t)
		{
			case e_tl_type.BODYPART:
			{
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
				var res = tl.render_res_diffuse
				var tintGrass = (res != null ? res.color_grass : -1)
				var tintFoliage = (res != null ? res.color_foliage : -1)
				var tintWater = (res != null ? res.color_water : -1)
				external_call(lib_xr_bridge_set_matrix,
					m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
					m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
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
				external_call(lib_xr_bridge_set_matrix,
					m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
					m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
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
				external_call(lib_xr_bridge_set_matrix,
					m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
					m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
				external_call(lib_xr_bridge_draw_vbuffer, vb, -1, 0)
				break
			}

			case e_tl_type.TEXT:
			{
				external_call(lib_xr_bridge_set_matrix,
					m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
					m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
				external_call(lib_xr_bridge_draw_vbuffer, tl.text_vbuffer, -1, 0)
				break
			}

			case e_tl_type.PATH:
			{
				external_call(lib_xr_bridge_set_matrix,
					m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
					m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
				external_call(lib_xr_bridge_draw_vbuffer, tl.path_vbuffer, -1, 0)
				break
			}

			default:
			{
				if (tl.temp != null && !is_undefined(tl.temp.shape_vbuffer))
				{
					external_call(lib_xr_bridge_set_matrix,
						m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
						m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
					external_call(lib_xr_bridge_draw_vbuffer, tl.temp.shape_vbuffer, -1, 0)
				}
				break
			}
		}
	}

	var ms = external_call(lib_xr_bridge_end_frame)
	xr_bridge_did_render = (ms > 0)
	return xr_bridge_did_render
}

function xr_bridge_shape_matrix_arr(tl, shapeId)
{
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
