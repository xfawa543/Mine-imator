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
		external_call(lib_xr_bridge_auto_tex)
	}

	// ==== S4b: per-frame texture registration (native-equivalent) ====
	// Every tl re-resolves its texture each frame, exactly like the native
	// render pass does: late-visible tls, skin changes and per-type texture
	// sources all stay correct. Registration is dedup'd DLL-side.
	xr_bridge_register_textures()

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

		// S4: per-frame texture lookup. BODYPART is keyed per SHAPE inside
		// its vbuffer-map loop below; all other types by tl id here.
		if (t != e_tl_type.BODYPART)
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
							external_call(lib_xr_bridge_set_tex_for, string_get_real(key))
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
				// S4b: per-texture-name faces (native render_world_block_map).
				// Registration used the same ds_map iteration order, so the
				// slot index here matches the registered synth key.
				if (tl.temp.model.model_block_map != null
					&& ds_exists(tl.temp.model.model_block_map, ds_type_map))
				{
					var mi = 0
					var mk = ds_map_find_first(tl.temp.model.model_block_map)
					while (!is_undefined(mk) && mi < 12)
					{
						var mvb = tl.temp.model.model_block_map[?mk]
						if (mvb != null && !vbuffer_is_empty(mvb))
						{
							external_call(lib_xr_bridge_set_tex_for, -(tl.id * 16 + 3 + mi))
							external_call(lib_xr_bridge_draw_vbuffer, mvb, -1, 0)
						}
						mi++
						mk = ds_map_find_next(tl.temp.model.model_block_map)
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
				// S4b: text_vbuffer/text_texture are [fill, outline] ARRAYS
				// (tl_event_create); the old single-buffer call failed the
				// buffer lookup and never drew. Native order: fill first,
				// then the outline layer (render_world_text).
				external_call(lib_xr_bridge_set_matrix,
					m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
					m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
				if (tl.text_vbuffer[0] != null && !vbuffer_is_empty(tl.text_vbuffer[0]))
				{
					external_call(lib_xr_bridge_set_tex_for, -(tl.id * 16 + 1))
					external_call(lib_xr_bridge_draw_vbuffer, tl.text_vbuffer[0], -1, 0)
				}
				if (tl.value[e_value.TEXT_OUTLINE] && tl.text_vbuffer[1] != null
					&& !vbuffer_is_empty(tl.text_vbuffer[1]))
				{
					external_call(lib_xr_bridge_set_tex_for, -(tl.id * 16 + 2))
					external_call(lib_xr_bridge_draw_vbuffer, tl.text_vbuffer[1], -1, 0)
				}
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

// ==== S4b: per-frame per-type texture registration ====
// Resolves each tl's texture exactly like the native render pass
// (render_world_tl) does, and registers it into the DLL's per-item map:
//   BODYPART        -> per-shape skins (model_part_shape_tex[s])
//   BLOCK/SCENERY   -> res.block_sheet_texture
//   ITEM            -> item_sheet_texture, fallback .texture
//   TEXT            -> text_texture[0] fill + [1] outline (synth keys)
//   MODEL           -> res.block_sheet_texture + model_block_map entries
//   PATH            -> spr_shape / own texture
//   shapes (default)-> temp_get_shape_tex(temp_get_shape_texobj(...))
// Paged sprites register page + uvRect (loader-side); standalone sprites
// their own texture. Synth keys: -(tl.id * 16 + slot), slot 1..14.
function xr_bridge_register_textures()
{
	for (var ri = 0; ri < ds_list_size(render_list); ri++)
	{
		var rtl = render_list[|ri]
		if (rtl = timeline_camera)
			continue
		var rt = rtl.type

		if (rt = e_tl_type.BODYPART)
		{
			if (rtl.model_part != null && ds_exists(rtl.model_part.shape_list, ds_type_list)
				&& rtl.model_part_shape_tex != null && !is_undefined(rtl.model_part_shape_tex))
			{
				for (var rs = 0; rs < ds_list_size(rtl.model_part.shape_list); rs++)
				{
					var rspr = rtl.model_part_shape_tex[rs]
					if (rspr > 0 && sprite_exists(rspr))
						external_call(lib_xr_bridge_reg_shape, rtl.model_part.shape_list[|rs].id, rspr)
				}
			}
		}
		else if (rt = e_tl_type.BLOCK || rt = e_tl_type.SCENERY)
		{
			var bres = rtl.render_res_diffuse
			if (bres = null || !res_is_ready(bres))
				bres = mc_res
			var bspr = bres.block_sheet_texture
			if (bspr > 0 && sprite_exists(bspr))
				external_call(lib_xr_bridge_reg_shape, rtl.id, bspr)
		}
		else if (rt = e_tl_type.ITEM)
		{
			var ires = rtl.item_res
			if (ires = null || !res_is_ready(ires))
				ires = mc_res
			var ispr = (ires.item_sheet_texture != null ? ires.item_sheet_texture : ires.texture)
			if (ispr > 0 && sprite_exists(ispr))
				external_call(lib_xr_bridge_reg_shape, rtl.id, ispr)
		}
		else if (rt = e_tl_type.TEXT)
		{
			for (var ti = 0; ti < 2; ti++)
			{
				var tspr = rtl.text_texture[ti]
				if (tspr > 0 && sprite_exists(tspr))
					external_call(lib_xr_bridge_reg_shape, -(rtl.id * 16 + 1 + ti), tspr)
			}
		}
		else if (rt = e_tl_type.MODEL)
		{
			if (rtl.temp != null && rtl.temp.model != null)
			{
				var mres = rtl.value_inherit[e_value.TEXTURE_OBJ]
				if (mres = null)
					mres = rtl.temp.model_tex
				if (mres = null || mres.block_sheet_texture = null)
					mres = mc_res
				var mspr = mres.block_sheet_texture
				if (mspr > 0 && sprite_exists(mspr))
					external_call(lib_xr_bridge_reg_shape, rtl.id, mspr)
				if (rtl.temp.model.model_block_map != null
					&& ds_exists(rtl.temp.model.model_block_map, ds_type_map))
				{
					var mi = 0
					var mk = ds_map_find_first(rtl.temp.model.model_block_map)
					while (!is_undefined(mk) && mi < 12)
					{
						var mtex = -1
						with (mres)
							mtex = res_get_model_texture(mk)
						if (mtex > 0 && sprite_exists(mtex))
							external_call(lib_xr_bridge_reg_shape, -(rtl.id * 16 + 3 + mi), mtex)
						mi++
						mk = ds_map_find_next(rtl.temp.model.model_block_map)
					}
				}
			}
		}
		else if (rt = e_tl_type.PATH)
		{
			var ptex = spr_shape
			if (rtl.value_inherit[e_value.TEXTURE_OBJ] != null)
				ptex = rtl.value_inherit[e_value.TEXTURE_OBJ].texture
			if (ptex > 0 && sprite_exists(ptex))
				external_call(lib_xr_bridge_reg_shape, rtl.id, ptex)
		}
		else
		{
			// Shapes (cube/cylinder/...): native default case. Capture the
			// texture object BEFORE entering with(temp) - inside it, `other`
			// would be the calling instance, not the tl. The fallback vars
			// (shape_texture/shape_tex) then resolve against temp exactly
			// like the native with(temp) does.
			if (rtl.temp != null)
			{
				var stexobj = rtl.value_inherit[e_value.TEXTURE_OBJ]
				var stex = -1
				with (rtl.temp)
					stex = temp_get_shape_tex(temp_get_shape_texobj(stexobj))
				if (stex > 0 && sprite_exists(stex))
					external_call(lib_xr_bridge_reg_shape, rtl.id, stex)
			}
		}
	}
}
