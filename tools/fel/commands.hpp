// Fabric Efficiency Ledger - command line tool.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

namespace fel_cli {

// Runs the command line tool. Returns the process exit code:
//   0 success, 1 runtime failure, 2 usage error.
int run(int argc, char** argv);

}  // namespace fel_cli
