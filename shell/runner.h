// Copyright 2016 Alex Silva Torres
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SHPP_MAIN_INTERPRETER_H
#define SHPP_MAIN_INTERPRETER_H

#include <setjmp.h>
#include <signal.h>
#include <boost/optional.hpp>

#include "interpreter/symbol-table.h"
#include "ast/ast.h"
#include "interpreter/interpreter.h"

namespace shpp {

class Runner {
 public:
  Runner();

  ~Runner() = default;

  void Exec(std::string file_name, std::vector<std::string>&& args = {});
  void ExecInterative();

 private:
  internal::Interpreter interpreter_;
};

}

#endif  // SHPP_MAIN_INTERPRETER_H
