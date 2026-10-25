/// xr_anim_dump_frame()
/// @desc EXPERIMENT ONLY (XGraphics animation bridge, third-phase sprint).
///       Called from the end of app_update_animate() — AFTER tl_update_matrix()
///       has written this frame's world matrices and AFTER the active camera
///       (timeline_camera) has been selected — so every value below is from the
///       SAME frame's animation update, not a cached one.
///       Enabled ONLY when the environment variable XR_ANIM_DUMP is set to a
///       file path prefix; otherwise returns immediately (zero behavior change,
///       no file I/O). The file is opened in append mode and closed every frame
///       so partial data survives abnormal termination.
///       Format (per frame, text):
///         FRAME <index> marker=<timeline_marker>
///         CAM id=<instance id> fov=<deg> from=x,y,z to=x,y,z up=x,y,z
///         TL id=<instance id> type=<e_tl_type> depth=<d>
///         TLMAT <16 floats, comma separated>
///         ENDFRAME
///       Camera convention (identical to render_update_camera.gml L50-56):
///         from = camera world_pos (matrix translation)
///         to   = (0,1,0) transformed by the camera matrix
///         up   = (m[8], m[9], m[10])
///         fov  = max(1, value[e_value.CAM_FOV])
///       Matrices: GameMaker column-major, v' = M * v, translation m[12..14],
///       local->world after parent/IK/pose composition (tl_update_matrix).
///       Object identity: GameMaker instance id (unique), NOT the name string.

globalvar xr_anim_dump_frames_written

function xr_anim_dump_mat16(f, m)
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

function xr_anim_dump_frame()
{
	var path = environment_get_variable("XR_ANIM_DUMP")
	if (path = "" || string(path) = "-1")
		return 0
	var f = file_text_open_append(path + "_frames.txt")
	if (f <= 0)
		return 0

	file_text_write_string(f, "FRAME " + string(xr_anim_dump_frames_written) + " marker=" + string(timeline_marker))
	file_text_writeln(f)

	if (timeline_camera != null)
	{
		var cam = timeline_camera
		var cm = cam.matrix
		var to = point3D_mul_matrix(point3D(0, 1, 0), cm)
		file_text_write_string(f, "CAM id=" + string(cam.id) + " fov=" + string(max(1, cam.value[e_value.CAM_FOV])) +
			" from=" + string(cam.world_pos[X]) + "," + string(cam.world_pos[Y]) + "," + string(cam.world_pos[Z]) +
			" to=" + string(to[X]) + "," + string(to[Y]) + "," + string(to[Z]) +
			" up=" + string(cm[8]) + "," + string(cm[9]) + "," + string(cm[10]))
		file_text_writeln(f)
	}
	else
	{
		file_text_write_string(f, "CAM none")
		file_text_writeln(f)
	}

	for (var i = 0; i < ds_list_size(render_list); i++)
	{
		var tl = render_list[|i]
		if (tl = timeline_camera)
			continue
		file_text_write_string(f, "TL id=" + string(tl.id) + " type=" + string(tl.type) + " depth=" + string(tl.depth))
		file_text_writeln(f)
		file_text_write_string(f, "TLMAT ")
		xr_anim_dump_mat16(f, tl.matrix_render)
	}

	file_text_write_string(f, "ENDFRAME")
	file_text_writeln(f)
	file_text_close(f)
	xr_anim_dump_frames_written++
	return 1
}
