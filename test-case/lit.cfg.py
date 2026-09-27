# -*- Python -*-
#
# lit configuration for the SimpleDCE pass tests.
#
# Usage (from the repository root):
#   build/bin/llvm-lit -v test-case
#   build/bin/llvm-lit -v test-case --param llvm_bin=build-assert/bin
#
# opt and FileCheck are taken from the directory given by the "llvm_bin"
# parameter, which defaults to <repo>/build/bin.

import os

import lit.formats

config.name = "SimpleDCE"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".ll"]

repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
llvm_bin = os.path.abspath(
    lit_config.params.get("llvm_bin", os.path.join(repo_root, "build", "bin"))
)

for tool in ("opt", "FileCheck"):
    if not os.path.exists(os.path.join(llvm_bin, tool)):
        lit_config.fatal(f"'{tool}' not found in llvm_bin={llvm_bin}")

config.test_source_root = os.path.dirname(os.path.abspath(__file__))
# Keep lit's temporary files out of the source tree.
config.test_exec_root = os.path.join(os.path.dirname(llvm_bin), "test-case")

# Put the selected tools first on PATH so RUN lines never pick up a system opt.
config.environment["PATH"] = os.pathsep.join(
    [llvm_bin, config.environment.get("PATH", "")]
)

lit_config.note(f"using tools from {llvm_bin}")
