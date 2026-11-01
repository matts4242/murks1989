#!/usr/bin/env python
# Builds the Murks GDExtension (core library + Godot binding) into game/bin/.
#
#   scons                         # debug build for this platform (used by the editor)
#   scons target=template_release # release build, for exported games
#   scons platform=windows use_mingw=yes   # cross-compile from Linux
import os

env = SConscript("godot-cpp/SConstruct")

env.Append(CPPPATH=["core/include", "extension/src"])
sources = Glob("core/src/*.cpp") + Glob("extension/src/*.cpp")

library = env.SharedLibrary(
    "game/bin/libmurks{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
    source=sources,
)

Default(library)
