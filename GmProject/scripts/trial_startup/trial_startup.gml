/// trial_startup()

function trial_startup()
{
	// XR build: the trial/activation system is removed entirely.
	// Full version from the first launch — no keys, no upgrade popups,
	// no trial watermark defaults.
	globalvar trial_version;
	trial_version = false
}
