#!/usr/bin/env python
# SConstruct
#
# Copyright (c) 2026-present Cafecito Games LLC.
# This file is part of BaristaMCP, a Godot GDExtension.
# SPDX-License-Identifier: MIT

import os
import sys

from methods import print_error


libname = "barista_mcp"
projectdir = "project"

local_env = Environment(tools=["default"], PLATFORM="")
local_env["build_profile"] = "build_profile.json"

customs = [os.path.abspath("custom.py")]
opts = Variables(customs, ARGUMENTS)
opts.Update(local_env)
Help(opts.GenerateHelpText(local_env))

if not (os.path.isdir("godot-cpp") and os.listdir("godot-cpp")):
    print_error(
        "godot-cpp is unavailable. Run `git submodule update --init --recursive` before building."
    )
    sys.exit(1)

env = local_env.Clone()
env = SConscript(
    "godot-cpp/SConstruct",
    {"env": env, "customs": customs, "api_version": "4.7"},
)

env.Append(CPPPATH=["src/"])
sources = Glob("src/*.cpp")
suffix = env["suffix"].replace(".universal", "")
library_name = "{}{}{}{}".format(
    env.subst("$SHLIBPREFIX"), libname, suffix, env.subst("$SHLIBSUFFIX")
)
library = env.SharedLibrary(
    "bin/{}/{}".format(env["platform"], library_name),
    source=sources,
)
installed = env.Install("{}/bin/{}/".format(projectdir, env["platform"]), library)

Default(library, installed)
