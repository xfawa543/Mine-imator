/// xr_export_probe()
/// @desc EXPERIMENT ONLY (XGraphics S1 export-chain probe).
///       Called from export_update() after a movie frame has been encoded,
///       so everything recorded here belongs to a COMPLETED export frame.
///       Enabled ONLY when the environment variable XR_EXPORT_PROBE is set
///       (to anything other than "0") AND XR_EXPORT_PROBE_DIR points to an
///       existing or creatable directory; otherwise returns immediately
///       (zero behavior change, no file I/O).
///       XR_EXPORT_PROBE_DIR    output directory (REQUIRED — no fallback,
///                              keeps the CppGen function set minimal)
///       XR_EXPORT_PROBE_EVERY  snapshot stride in frames (default 30)
///       Records per frame (probe.txt, append + close every frame):
///         F <frame> marker=<timeline_marker> size=<w>x<h> t=<ms since first frame>
///       Every XR_EXPORT_PROBE_EVERY frames the export surface is read back
///       (buffer_get_surface) and saved raw as snap_<frame>.bin.

globalvar xr_export_probe_on, xr_export_probe_dir, xr_export_probe_every, xr_export_probe_t0

function xr_export_probe()
{
	var env = environment_get_variable("XR_EXPORT_PROBE")
	if (env = "" || string(env) = "-1" || env = "0")
		return 0
	if (xr_export_probe_on != 1)
	{
		var d = environment_get_variable("XR_EXPORT_PROBE_DIR")
		if (d = "" || string(d) = "-1")
			return 0
		if (!directory_exists_lib(d))
			directory_create_lib(d)
		if (!directory_exists_lib(d))
		{
			log("XR export probe: cannot create dir", d)
			return 0
		}
		xr_export_probe_on = 1
		xr_export_probe_dir = d
		var e2 = environment_get_variable("XR_EXPORT_PROBE_EVERY")
		xr_export_probe_every = (e2 = "" || string(e2) = "-1" ? 30 : max(1, real(e2)))
		xr_export_probe_t0 = current_time
		var f = file_text_open_write(xr_export_probe_dir + "\probe.txt")
		file_text_write_string(f, "PROBE_ON every=" + string(xr_export_probe_every) + " video=" + string(project_video_width) + "x" + string(project_video_height))
		file_text_writeln(f)
		file_text_close(f)
	}
	var f = file_text_open_append(xr_export_probe_dir + "\probe.txt")
	file_text_write_string(f, "F " + string(exportmovie_frame) + " marker=" + string(timeline_marker) + " size=" + string(project_video_width) + "x" + string(project_video_height) + " t=" + string(current_time - xr_export_probe_t0))
	file_text_writeln(f)
	file_text_close(f)
	if (exportmovie_frame mod xr_export_probe_every = 0)
	{
		var b = buffer_create(project_video_width * project_video_height * 4, buffer_fixed, 4)
		buffer_get_surface(b, export_surface, 0)
		buffer_save(b, xr_export_probe_dir + "\snap_" + string(exportmovie_frame) + ".bin")
		buffer_delete(b)
	}
	return 1
}
