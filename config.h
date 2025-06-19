#ifndef __CONFIG_H__
#define __CONFIG_H__

#define PROJECT_VERSION "1.1.0"
#define GETTEXT_PACKAGE "mechsim"
#define PKEXEC_PATH "/usr/bin/pkexec"

// Prefix macro should be defined by compiler flags, fallback to /usr
#ifndef PACKAGE_PREFIX
#define PACKAGE_PREFIX "/usr"
#endif

#define PACKAGE_BINDIR PACKAGE_PREFIX "/bin"
#define PACKAGE_LOCALEDIR PACKAGE_PREFIX "/share/locale"

// MechSim specific paths relative to prefix
#define MECHSIM_DATA_DIR PACKAGE_PREFIX "/share/mechsim"
#define MECHSIM_BIN_DIR PACKAGE_PREFIX "/bin"

#endif
