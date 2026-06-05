#pragma once

// Forces -steam onto the engine's command line so it runs under Steam (avoids
// the VAC error message box). On Windows the engine reads the real process
// command line via GetCommandLineA/W (it ignores the lpCmdLine passed to
// LauncherMain), so we hook those. On other platforms this is a no-op: the unix
// launcher injects -steam into argv directly, which the engine does use.
//
// Call once, early, for the client only (not dedicated servers).
void InstallCommandLineHook();
