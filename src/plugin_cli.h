// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PLUGIN_CLI_H
#define PLUGIN_CLI_H

// "jtplay --plugin FILE ...": runs a plugin script headless and prints what
// it answers (see plugin_cli.cpp). Returns the process exit status.
int PluginCli(int argc, char* argv[]);

#endif
