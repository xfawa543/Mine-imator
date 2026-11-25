/// xr_bridge_frame()
/// @desc S4 textured render bridge. Called from export_update() each frame.
///       Textures resolved ONCE at init (reliable context) and registered
///       into the DLL's per-item map. Per frame, only tl.id is passed to
///       look up the stored texture — bypasses CppGen loop-scope issues.

globalvar xr_bridge_ready
globalvar xr_bridge_did_render
globalvar xr_bridge_takeover_active
globalvar xr_prof_n, xr_prof_text, xr_prof_reg, xr_prof_draw, xr_prof_end
globalvar lib_xr_bridge_set_draw_state_glow
globalvar xr_gml_frame
// S56r: per (sheet sprite, slot) captured sprites for ITEM registration.
globalvar xr_item_sheet_cap
globalvar xr_item_sheet_reg

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
	xr_gml_frame += 1

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
		xr_item_sheet_cap = ds_map_create()
		xr_item_sheet_reg = ds_map_create()
		external_call(lib_xr_bridge_auto_tex)
	}

	// ==== S4b: per-frame texture registration (native-equivalent) ====
	// Every tl re-resolves its texture each frame, exactly like the native
	// render pass does: late-visible tls, skin changes and per-type texture
	// sources all stay correct. Registration is dedup'd DLL-side.
	// S33: render_start runs render_update_text()/render_update_item() BEFORE
	// the world pass - the bridge bypasses render_start, so without calling
	// them here the text sprites (and item textures) were generated once in
	// the editor and NEVER updated during the export (the 15-18s credits
	// stuck at "Song by: Skillet").
	// S53d: per-frame section timing (env XR_FRAME_PROF=1) - one summary
	// line per 60 frames into MI's log, localizes the exe-side wall.
	var profOn = (environment_get_variable("XR_FRAME_PROF") = "1")
	var pt = 0
	if (profOn) pt = current_time
	render_update_text()
	render_update_item()
	if (profOn) { xr_prof_text += current_time - pt; pt = current_time }
	// S36: scene lights + camera post (the native
	// shader_color_fog_lights_set inputs, read engine-side by the loader).
	external_call(lib_xr_bridge_set_lights_app)
	xr_bridge_register_textures()
	if (profOn) { xr_prof_reg += current_time - pt; pt = current_time }

	// Camera
	if (timeline_camera != null)
	{
		var cm = timeline_camera.matrix
		var to = point3D_mul_matrix(point3D(0, 1, 0), cm)
		external_call(lib_xr_bridge_begin_frame, max(1, timeline_camera.value[e_value.CAM_FOV]),
			timeline_camera.world_pos[X], timeline_camera.world_pos[Y], timeline_camera.world_pos[Z],
			to[X], to[Y], to[Z], cm[8], cm[9], cm[10])
	}

	// S4t: native background (render_low L18 draws render_world_background
	// BEFORE the world pass): sky color clear + optional full-screen image.
	external_call(lib_xr_bridge_set_bg, background_sky_color)
	if (background_image_show && background_image != null && background_image_type = "image")
		external_call(lib_xr_bridge_set_bg_image, background_image.texture)

	for (var i = 0; i < ds_list_size(render_list); i++)
	{
		var tl = render_list[|i]
		if (tl = timeline_camera)
			continue
		// S56n DIAG: the FULL tl census at frame 600 - type, visibility,
		// alpha and world translation for every tl in the render list.
		if (xr_gml_frame = 600)
		{
			var df2 = file_text_open_append(prefix + "_tlcensus.txt")
			if (df2 > 0)
			{
				file_text_write_string(df2, "i=" + string(i) +
					" id=" + string(tl.id) +
					" name=" + string(tl.name) +
					" type=" + string(tl.type) +
					" depth=" + string(tl.depth) +
					" vis=" + string(tl.value_inherit[e_value.VISIBLE]) +
					" alpha=" + string(tl.value_inherit[e_value.ALPHA]) +
					" mat=(" + string(tl.matrix_render[12]) + "," + string(tl.matrix_render[13]) + "," + string(tl.matrix_render[14]) + ")")
				file_text_writeln(df2)
				file_text_close(df2)
			}
		}
		// S4k: native visibility culling (render_world_tl L22 + L71).
		// value_inherit is refreshed per frame by tl_update_matrix and already
		// includes the parent-chain inheritance, matching the native data the
		// render path consumes.
		if (!tl.value_inherit[e_value.VISIBLE])
			continue
		if ((tl.value_inherit[e_value.ALPHA] * 1000) = 0)
			continue
		// S4q: native tl_get_visible() HQ/LQ hide flags (the export-only
		// hiding layer). Transition/squash tls carry hq_hiding/lq_hiding and
		// are invisible in native exports - without this they render as
		// stretched ribbons / paper-thin sheets / black lines.
		if ((app.exportmovie_high_quality && tl.hq_hiding) || (!app.exportmovie_high_quality && tl.lq_hiding))
			continue
		// S4u: native tl_get_visible() L8 - the timeline eye toggle. The Feel
		// template carries 46 hide=true tls (scratch objects with arbitrary
		// transforms); native never renders them, the bridge did - they show
		// up as the stretched ribbons / paper-thin sheets in exports.
		if (tl.hide && !tl.render_hidden)
			continue
		// S56: native render_world_tl - glow objects with only_render_glow render
		// ONLY in the COLOR_GLOW pass (the glow re-render), never in the main
		// color pass - the light-effect overlays (the f330 white bands).
		if (tl.glow && tl.only_render_glow)
			continue
		// S53c: the whole per-tl draw state in ONE external_call (the four
		// separate calls were ~1500-2500 GML->C++ boundary crossings/frame).
		// S56: + glow state. The CppGen parser rejects ternaries in the arg
		// list, so the flags are precomputed into plain vars.
		var glowF = 0
		if (tl.glow) glowF = 1
		var onlyGlowF = 0
		if (tl.only_render_glow) onlyGlowF = 1
		external_call(lib_xr_bridge_set_draw_state,
			clamp(tl.value_inherit[e_value.ALPHA], 0, 1),
			tl.value_inherit[e_value.RGB_MUL],
			tl.value_inherit[e_value.MIX_COLOR],
			tl.value_inherit[e_value.MIX_PERCENT],
			tl.value_inherit[e_value.EMISSIVE])
		external_call(lib_xr_bridge_set_draw_state_glow,
			glowF,
			onlyGlowF,
			tl.glow_texture,
			tl.value_inherit[e_value.GLOW_COLOR])
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
				// S4i: native per-shape chain (render_world_model_part L287-321):
				//   rendermatrix = shape.matrix × world
				//   + shape.item_bounce (post Z, ease quad)
				//   + shape.face_camera (matrix_remove_rotation → billboard toward camera)
				// then draw with the FULL composed matrix (isShape=0).
				if (ds_exists(tl.model_shape_vbuffer_map, ds_type_map))
				{
					// S56i: iterate shape_list (the NATIVE draw order,
					// render_world_model_part) instead of the ds_map. The
					// map's hash order is arbitrary: the eyeball/iris shapes
					// drew BEFORE the head skin, and the skin (same depth,
					// drawn later, GL_LEQUAL) won the socket pixels - the
					// pupils vanished. Native order = skin first, eyes last.
					var shapeCount = 0
					if (tl.model_part != null && ds_exists(tl.model_part.shape_list, ds_type_list))
						shapeCount = ds_list_size(tl.model_part.shape_list)
					for (var si = 0; si < shapeCount; si++)
					{
						var shape = tl.model_part.shape_list[|si]
						var vb = tl.model_shape_vbuffer_map[?shape.id]
						if (vb != null && !vbuffer_is_empty(vb))
						{
							var sid = shape.id
							var sm = xr_bridge_shape_matrix_at(tl, sid, si)
							var rm = matrix_multiply(sm, m) // native order (render_world_model_part L290): shape-local placement FIRST, then world
							var sflags = xr_bridge_shape_flags_at(tl, sid, si)
							// S29+S35: per-shape tint/alpha (native L239-268):
							// uBlendColor = tl RGB_MUL x (model blend color when
							// shape.use_model_color) x shape.color_blend.
							var _bcol = tl.value_inherit[e_value.RGB_MUL]
							if (tl.temp != null && tl.temp.model_use_blend_color)
								_bcol = color_multiply(_bcol, tl.temp.model_blend_color)
							var glowF = 0
							if (tl.glow) glowF = 1
							var onlyGlowF = 0
							if (tl.only_render_glow) onlyGlowF = 1
							external_call(lib_xr_bridge_set_draw_state,
								clamp(tl.value_inherit[e_value.ALPHA] * sflags[4], 0, 1),
								color_multiply(_bcol, sflags[3]),
								tl.value_inherit[e_value.MIX_COLOR],
								tl.value_inherit[e_value.MIX_PERCENT],
								tl.value_inherit[e_value.EMISSIVE])
							external_call(lib_xr_bridge_set_draw_state_glow,
								glowF,
								onlyGlowF,
								tl.glow_texture,
								tl.value_inherit[e_value.GLOW_COLOR])
							if (rs < 3)
							{
								// S4j DIAG: matrix values at diag frame 900
								external_call(lib_xr_bridge_diag_m, 0, tl.id, 0, 0, 0, 0, 0, 0, 0, 0)
								external_call(lib_xr_bridge_diag_m, 1, rs, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7])
								external_call(lib_xr_bridge_diag_m, 2, rs, m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
								external_call(lib_xr_bridge_diag_m, 3, rs, sm[0], sm[1], sm[2], sm[3], sm[4], sm[5], sm[6], sm[7])
								external_call(lib_xr_bridge_diag_m, 4, rs, sm[8], sm[9], sm[10], sm[11], sm[12], sm[13], sm[14], sm[15])
								external_call(lib_xr_bridge_diag_m, 5, rs, rm[0], rm[1], rm[2], rm[3], rm[4], rm[5], rm[6], rm[7])
								external_call(lib_xr_bridge_diag_m, 6, rs, rm[8], rm[9], rm[10], rm[11], rm[12], rm[13], rm[14], rm[15])
								external_call(lib_xr_bridge_diag_m, 7, rs, sflags[0], sflags[1], sflags[2], sid, 0, 0, 0, 0)
							}
							if (sflags[0] = 1 && sflags[1])
							{
								var bd = 60 * 3
								var bt = (app.background_time mod bd) * 2
								var offz = 0
								if (bt < bd)
									offz = ease("easeinoutquad", bt / bd) * 2 - 1
								else
									offz = 1 - ease("easeinoutquad", (bt - bd) / bd) * 2
								rm = matrix_multiply(rm, matrix_build(0, 0, offz, 0, 0, 0, 1, 1, 1))
							}
							if (sflags[0] = 1 && sflags[2])
							{
								// inline matrix_remove_rotation (strip rotation, keep scale)
								var xs = sqrt(rm[0] * rm[0] + rm[1] * rm[1] + rm[2] * rm[2])
								var ys = sqrt(rm[4] * rm[4] + rm[5] * rm[5] + rm[6] * rm[6])
								var zs = sqrt(rm[8] * rm[8] + rm[9] * rm[9] + rm[10] * rm[10])
								rm[0] = xs; rm[1] = 0; rm[2] = 0
								rm[4] = 0; rm[5] = ys; rm[6] = 0
								rm[8] = 0; rm[9] = 0; rm[10] = zs
								var rotx = -point_zdirection(rm[12], rm[13], rm[14], proj_from[X], proj_from[Y], proj_from[Z])
								var rotz = 90 + point_direction(rm[12], rm[13], proj_from[X], proj_from[Y])
								rm = matrix_multiply(matrix_build(0, 0, 0, rotx, 0, rotz, 1, 1, 1), rm)
							}
							external_call(lib_xr_bridge_set_matrix,
								rm[0], rm[1], rm[2], rm[3], rm[4], rm[5], rm[6], rm[7],
								rm[8], rm[9], rm[10], rm[11], rm[12], rm[13], rm[14], rm[15])
							external_call(lib_xr_bridge_set_tex_for, sid)
							external_call(lib_xr_bridge_draw_vbuffer, vb, -1, 0)
						}
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
						mk = ds_map_find_next(tl.temp.model.model_block_map, mk)
					}
				}
				break
			}

			case e_tl_type.ITEM:
			{
				// S56m DIAG: why do the Pupils Right/Left items never reach
				// the draw queue? Log every ITEM tl's state at frames 598-602.
				if (xr_gml_frame >= 598 && xr_gml_frame <= 602)
				{
					var df = file_text_open_append(prefix + "_itemdiag.txt")
					if (df > 0)
					{
						file_text_write_string(df, "f" + string(xr_gml_frame) +
							" id=" + string(tl.id) +
							" name=" + string(tl.name) +
							" ivb=" + string(tl.item_vbuffer != null) +
							" temp=" + string(tl.temp != null) +
							" tvb=" + string(tl.temp != null && tl.temp.item_vbuffer != null) +
							" vis=" + string(tl.value_inherit[e_value.VISIBLE]) +
							" alpha=" + string(tl.value_inherit[e_value.ALPHA]) +
							" mat=(" + string(m[12]) + "," + string(m[13]) + "," + string(m[14]) + ")")
						file_text_writeln(df)
						file_text_close(df)
					}
				}
				// S4h: replicate render_world_item transforms in native order:
				// 1) facecamera pre (pivot rotation at item axis point (8, .5*is3d))
				// 2) rotate/spin pre (same pivot, continuous Z rotation)
				// 3) bounce POST (world-space Z offset, MI Z-up = vertical)
				var vb = (tl.item_vbuffer != null ? tl.item_vbuffer : tl.temp.item_vbuffer)
				var im = m
				var is3d = tl.temp.item_3d
				if (tl.temp.item_face_camera)
				{
					var frotz = 90 + point_direction(im[12], im[13], proj_from[X], proj_from[Y])
					var fm = matrix_multiply(matrix_build(-8, -0.5 * is3d, 0, 0, 0, 0, 1, 1, 1),
						matrix_build(8, 0.5 * is3d, 0, 0, 0, frotz, 1, 1, 1))
					im = matrix_multiply(fm, im)
				}
				if (tl.temp.item_spin)
				{
					// native: d=60*6; t=(background_time mod d)*360; offz=t/360
					var sd = 60 * 6
					var soffz = (app.background_time mod sd)
					var sm2 = matrix_multiply(matrix_build(-8, -0.5 * is3d, 0, 0, 0, 0, 1, 1, 1),
						matrix_build(8, 0.5 * is3d, 0, 0, 0, soffz, 1, 1, 1))
					im = matrix_multiply(sm2, im)
				}
				if (tl.temp.item_bounce)
				{
					// native: d=60*3; t=(background_time mod d)*2; ease quad up/down;
					// matrix_world_multiply_POST (world-space Z offset)
					var bd = 60 * 3
					var bt = (app.background_time mod bd) * 2
					var offz = 0
					if (bt < bd)
						offz = ease("easeinoutquad", bt / bd) * 2 - 1
					else
						offz = 1 - ease("easeinoutquad", (bt - bd) / bd) * 2
					im = matrix_multiply(im, matrix_build(0, 0, offz, 0, 0, 0, 1, 1, 1))
				}
				external_call(lib_xr_bridge_set_matrix,
					im[0], im[1], im[2], im[3], im[4], im[5], im[6], im[7],
					im[8], im[9], im[10], im[11], im[12], im[13], im[14], im[15])
				external_call(lib_xr_bridge_draw_vbuffer, vb, -1, 0)
				break
			}

			case e_tl_type.TEXT:
			{
				// S4h: replicate render_world_text pre-transforms.
				// Native order: facecamera pre-rotate (aim the text plane at the
				// camera) THEN the non-minecraft font scale (8/48 on x/z), both
				// matrix_world_multiply_pre (= world := pre * world).
				var tm = m
				if (tl.temp != null && tl.temp.text_face_camera)
				{
					// rotx/rotz from the text plane position (mat[12..14]) toward
					// proj_from (= cam_from, render_world_start.gml:14)
					var trotx = -point_zdirection(tm[12], tm[13], tm[14], proj_from[X], proj_from[Y], proj_from[Z])
					var trotz = 90 + point_direction(tm[12], tm[13], proj_from[X], proj_from[Y])
					tm = matrix_multiply(matrix_build(0, 0, 0, trotx, 0, trotz, 1, 1, 1), tm)
				}
				if (tl.text_res != null && !tl.text_res.font_minecraft)
				{
					var tsca = 8 / 48
					tm = matrix_multiply(matrix_build(0, 0, 0, 0, 0, 0, tsca, 1, tsca), tm)
				}
				external_call(lib_xr_bridge_set_matrix,
					tm[0], tm[1], tm[2], tm[3], tm[4], tm[5], tm[6], tm[7],
					tm[8], tm[9], tm[10], tm[11], tm[12], tm[13], tm[14], tm[15])
				// text_vbuffer/text_texture are [fill, outline] ARRAYS
				// (tl_event_create); native order: fill first, then the outline
				// layer (render_world_text).
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
					// S4h: render_world_shape applies face_camera ONLY for
					// SURFACE shapes (cubes/cylinders ignore it).
					if (tl.temp.type = e_temp_type.SURFACE && tl.temp.shape_face_camera)
					{
						var srotx = -point_zdirection(m[12], m[13], m[14], proj_from[X], proj_from[Y], proj_from[Z])
						var srotz = 90 + point_direction(m[12], m[13], proj_from[X], proj_from[Y])
						m = matrix_multiply(matrix_build(0, 0, 0, srotx, 0, srotz, 1, 1, 1), m)
					}
					external_call(lib_xr_bridge_set_matrix,
						m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
						m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15])
					external_call(lib_xr_bridge_draw_vbuffer, tl.temp.shape_vbuffer, -1, 0)
				}
				break
			}
		}
	}

	if (profOn) { xr_prof_draw += current_time - pt; pt = current_time }

	var ms = external_call(lib_xr_bridge_end_frame)
	if (profOn) { xr_prof_end += current_time - pt; pt = current_time }
	xr_bridge_did_render = (ms > 0)
	if (profOn)
	{
		xr_prof_n += 1
		if (xr_prof_n mod 60 = 0)
		{
			log("xrprof n=" + string(xr_prof_n) + " text=" + string(xr_prof_text) + " reg=" + string(xr_prof_reg) + " draw=" + string(xr_prof_draw) + " end=" + string(xr_prof_end))
			xr_prof_text = 0
			xr_prof_reg = 0
			xr_prof_draw = 0
			xr_prof_end = 0
		}
	}
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

// S4i DIAG helper: per-shape flags for the native transform chain.
// Returns [found, item_bounce, face_camera, color_blend, color_alpha];
// found=0 when the shape id is absent. S29: also carries the per-shape
// blend color/alpha (native render_world_model_part L239-240) so the
// bridge can bake the same tint the uBlendColor uniform applies.
function xr_bridge_shape_flags(tl, shapeId)
{
	var res = [0, 0, 0, c_white, 1]
	if (tl.model_part = null)
		return res
	if (ds_exists(tl.model_part.shape_list, ds_type_list))
	{
		for (var s = 0; s < ds_list_size(tl.model_part.shape_list); s++)
		{
			var shape = tl.model_part.shape_list[|s]
			if (shape.id = shapeId)
			{
				res[0] = 1
				res[1] = shape.item_bounce
				res[2] = shape.face_camera
				res[3] = shape.color_blend
				res[4] = shape.color_alpha
				return res
			}
		}
	}
	return res
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
					// S4c DIAG: per-shape vbuffer state at the diag frame
					// (read-only; recorded DLL-side only at frame 900).
					var dshid = rtl.model_part.shape_list[|rs].id
					var dvb = -1
					var dst = 0
					if (ds_exists(rtl.model_shape_vbuffer_map, ds_type_map))
					{
						var dmv = rtl.model_shape_vbuffer_map[?string(dshid)]
						if (!is_undefined(dmv))
						{
							if (dmv = null)
								dst = 1
							else if (vbuffer_is_empty(dmv))
								dst = 2
							else { dst = 3; dvb = dmv }
						}
					}
					external_call(lib_xr_bridge_diag_shape, rtl.id, rs, dvb, dst)
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
			if (ires.item_sheet_texture != null)
			{
				// S56v: item_sheet_texture is a GM TEXTURE (texture_create
				// from the item's PNG; native render_world_item binds it
				// with render_set_texture). Two dead ends proven by dumps:
				// the sprite path passes the texture id into FindSprite()
				// (id collision -> an unrelated white sprite), and the
				// texture path reg_tex_force skips it because a
				// texture_create texture has NO CPU page mirror
				// (page->image is null -> white fallback). Draw the
				// texture into a cached surface with MI's own
				// draw_texture_part (the same call
				// res_load_pack_item_textures uses), then register the
				// surface texture - texture_surface results carry a real
				// CPU mirror (the working TEXT path).
				var ck = string(ires.item_sheet_texture)
				var ssurf = 0
				if (ds_map_exists(xr_item_sheet_cap, ck))
					ssurf = ds_map_find_value(xr_item_sheet_cap, ck)
				if (ssurf <= 0 || !surface_exists(ssurf))
				{
					var tw = texture_width(ires.item_sheet_texture)
					var th = texture_height(ires.item_sheet_texture)
					if (tw > 0 && th > 0)
					{
						ssurf = surface_create(tw, th)
						if (ssurf > 0)
						{
							surface_set_target(ssurf)
							draw_clear_alpha(c_black, 0)
							draw_texture_part(ires.item_sheet_texture, 0, 0, 0, 0, tw, th, 1, 1)
							surface_reset_target()
							ds_map_delete(xr_item_sheet_cap, ck)
							ds_map_add(xr_item_sheet_cap, ck, ssurf)
						}
					}
				}
				if (ssurf > 0 && surface_exists(ssurf))
				{
					// texture_surface() = MI's wrapper over
					// sprite_create_from_surface - returns a SPRITE with a
					// real CPU-captured image. Register via the sprite
					// force-upload path (reads fr->image), exactly like the
					// working TEXT registration.
					var cspr = texture_surface(ssurf)
					if (cspr > 0 && sprite_exists(cspr))
						external_call(lib_xr_bridge_reg_shape_force, rtl.id, cspr)
				}
			}
			else
			{
				var ispr = ires.texture
				if (ispr > 0 && sprite_exists(ispr))
					external_call(lib_xr_bridge_reg_shape_force, rtl.id, ispr)
			}
		}
		else if (rt = e_tl_type.TEXT)
		{
			// S32: texture_surface() = sprite_create_from_surface() - it
			// returns a SPRITE id (fresh, with a GPU->CPU captured image per
			// text change). Register via the sprite force-upload path (the
			// S30 texture-id detour resolved garbage = blank paper).
			for (var ti = 0; ti < 2; ti++)
			{
				var tspr = rtl.text_texture[ti]
				if (tspr > 0 && sprite_exists(tspr))
					external_call(lib_xr_bridge_reg_shape_force, -(rtl.id * 16 + 1 + ti), tspr)
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
					mk = ds_map_find_next(rtl.temp.model.model_block_map, mk)
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


// S52c: O(n) shape lookups - the caller builds an id->index ds_map once per
// bodypart per frame; the old functions linear-rescanned shape_list per
// shape (O(n^2) per bodypart in interpreted GML).
function xr_bridge_shape_matrix_at(tl, sid, si)
{
	var ident = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
	if (tl.model_part = null)
		return ident
	if (!ds_exists(tl.model_part.shape_list, ds_type_list))
		return ident
	if (si >= 0 && si < ds_list_size(tl.model_part.shape_list))
	{
		var shape = tl.model_part.shape_list[|si]
		if (shape.id = sid && !is_undefined(shape.matrix))
			return shape.matrix
	}
	return ident
}

function xr_bridge_shape_flags_at(tl, sid, si)
{
	var res = [0, 0, 0, c_white, 1]
	if (tl.model_part = null)
		return res
	if (!ds_exists(tl.model_part.shape_list, ds_type_list))
		return res
	if (si >= 0 && si < ds_list_size(tl.model_part.shape_list))
	{
		var shape = tl.model_part.shape_list[|si]
		if (shape.id = sid)
		{
			res[0] = 1
			res[1] = shape.item_bounce
			res[2] = shape.face_camera
			res[3] = shape.color_blend
			res[4] = shape.color_alpha
		}
	}
	return res
}
