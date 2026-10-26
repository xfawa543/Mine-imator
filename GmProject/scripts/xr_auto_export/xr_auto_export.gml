/// xr_auto_export_startup() / xr_auto_export_tick()
/// @desc EXPERIMENT ONLY (XGraphics S1: env-gated automatic movie export).
///       No-op unless XR_AUTO_PROJECT is set. Startup hook mimics the
///       dev_mode auto-load (app_startup_interface) plus the non-dialog core
///       of action_toolbar_exportmovie_save().
///       XR_AUTO_PROJECT  .miproject path loaded at startup (after assets)
///       XR_AUTO_OUT      output movie path (mp4); export starts ~1s after load
///       XR_AUTO_FPS      export frame rate (default 30)
///       XR_AUTO_END      timeline end marker override (default: full timeline)
///       XR_AUTO_QUALITY  1 = high quality render (default 0 = shaded)

globalvar xr_auto_export_pending, xr_auto_export_out

function xr_auto_export_startup()
{
	var proj = environment_get_variable("XR_AUTO_PROJECT")
	if (proj = "" || string(proj) = "-1")
		return 0
	if (!file_exists_lib(proj))
	{
		log("XR auto export: project not found", proj)
		return 0
	}
	popup_close()
	project_load(proj)
	window_state = ""
	xr_auto_export_out = ""
	var out = environment_get_variable("XR_AUTO_OUT")
	if (out = "" || string(out) = "-1")
		return 0
	xr_auto_export_out = out
	xr_auto_export_pending = 60 // steps (~1 s) to let the editor settle
	return 1
}

function xr_auto_export_tick()
{
	if (xr_auto_export_pending <= 0)
		return 0
	if (window_state = "load_assets" || window_state = "new_assets")
		return 0
	if (window_state != "")
	{
		// Editor did not become ready (load failed or another mode) — abort.
		xr_auto_export_pending = 0
		return 0
	}
	xr_auto_export_pending -= 1
	if (xr_auto_export_pending != 0)
		return 0

	var fps = 30
	var efps = environment_get_variable("XR_AUTO_FPS")
	if (!(efps = "" || string(efps) = "-1"))
		fps = max(1, real(efps))
	var endm = -1
	var eend = environment_get_variable("XR_AUTO_END")
	if (!(eend = "" || string(eend) = "-1"))
		endm = real(eend)
	var qual = 0
	var eq = environment_get_variable("XR_AUTO_QUALITY")
	if (eq = "1")
		qual = 1

	// Dialog-equivalent state (export_update reads popup_exportmovie.*)
	popup_exportmovie.format = "mp4"
	popup_exportmovie.framespersecond = fps
	popup_exportmovie.high_quality = qual
	popup_exportmovie.bit_rate = 2500000
	popup_exportmovie.include_audio = false

	export_filename = xr_auto_export_out
	exportmovie_marker_previous = timeline_marker
	exportmovie_format = "mp4"
	exportmovie_framespersecond = fps
	exportmovie_high_quality = qual
	exportmovie_current_sound = null
	exportmovie_marker_start = 0
	exportmovie_marker_end = (endm >= 0 ? endm : timeline_length)

	log("XR auto export", xr_auto_export_out)
	movie_set(project_video_width, project_video_height, 2500000, fps, false)
	var err = movie_start(xr_auto_export_out, "mp4")
	if (err < 0)
	{
		log("XR auto export: movie_start failed", err)
		return 0
	}

	if (!is_cpp())
		exportmovie_buffer = buffer_create(project_video_width * project_video_height * 4, buffer_fixed, 4)
	render_background = true
	render_hidden = false
	render_watermark = false

	window_state = "export_movie"
	exportmovie_frame = 0
	export_sample = 0
	exportmovie_start = current_time
	render_samples = -1

	timeline_marker = exportmovie_marker_start
	action_tl_play_start()
	return 1
}
