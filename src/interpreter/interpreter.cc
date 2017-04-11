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

#include "interpreter.h"

#include <string>
#include <memory>
#include <iostream>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include "stmt-executor.h"
#include "executor.h"
#include "scope-executor.h"
#include "parser/parser.h"
#include "parser/lexer.h"
#include "modules/std-funcs.h"
#include "objects/object-factory.h"
#include "modules/std-cmds.h"
#include "modules/env.h"

namespace shpp {
namespace internal {

Interpreter::Interpreter(bool main)
    : symbol_table_(SymbolTablePtr(new SymbolTable))
    , symbol_table_stack_(symbol_table_)
    , main_(main) {
  AlocTypes(symbol_table_stack_);

  module::stdf::RegisterModule(symbol_table_stack_);
  module::env::RegisterModule(symbol_table_stack_);
  cmds::stdf::RegisterCmds(symbol_table_stack_);
}

void Interpreter::InsertVar(const std::string& name, ObjectPtr obj) {
  SymbolAttr symbol(obj, true);
  symbol_table_stack_.InsertEntry(name, std::move(symbol));
}

void Interpreter::RegisterVars() {
  ObjectFactory obj_factory(symbol_table_stack_);

  InsertVar("__main__", obj_factory.NewBool(main_));
}

void Interpreter::RegisterFileVars(const std::string& file) {
  namespace fs = boost::filesystem;

  ObjectFactory obj_factory(symbol_table_stack_);

  fs::path full_path = fs::system_complete(file);
  InsertVar("__file_path__", obj_factory.NewString(full_path.string()));

  fs::path file_name = full_path.filename();
  InsertVar("__file__", obj_factory.NewString(file_name.string()));

  fs::path parent_path = full_path.parent_path();
  InsertVar("__path__", obj_factory.NewString(parent_path.string()));
}

void Interpreter::RegisterArgs(std::vector<std::string>&& args) {
  std::vector<ObjectPtr> vec;
  ObjectFactory obj_factory(symbol_table_stack_);

  for (const auto& arg: args) {
    ObjectPtr obj = obj_factory.NewString(arg);
    vec.push_back(obj);
  }

  ObjectPtr obj_args = obj_factory.NewArray(std::move(vec));
  InsertVar("args", obj_args);
}

void Interpreter::Exec(ScriptStream& file, std::vector<std::string>&& args) {
  std::stringstream buffer;
  buffer << file.fs().rdbuf();

  Lexer l(buffer.str());
  TokenStream ts = l.Scanner();
  Parser p(std::move(ts));
  auto res = p.AstGen();
  stmt_list_ = res.MoveAstNode();
  try {
    if (p.nerrors() == 0) {
      RegisterVars();
      RegisterFileVars(file.filename());
      RegisterArgs(std::move(args));

      RootExecutor executor(symbol_table_stack_);
      executor.Exec(stmt_list_.get());
    } else {
      Message msg = p.Msgs();
      throw RunTimeError(RunTimeError::ErrorCode::PARSER, msg.msg(),
                         Position{msg.line(), msg.pos()});
    }
  } catch (RunTimeError& e) {
    // split the lines of file in a array to set line of error on each message
    std::vector<std::string> file_lines = SplitFileLines(buffer.str());

    // set filename and the line of the erro on each message
    for (auto& msg: e.messages()) {
      msg.file(file.filename());
      // subtract 1 because vector starts on 0 and line on 1
      msg.line_error(file_lines[msg.line()-1]);
    }

    e.file(file.filename());
    e.line_error(file_lines[e.pos().line-1]);
    throw e;
  }
}

void Interpreter::ExecInterative(
    const std::function<std::string(Executor*, bool concat)>& func) {
  RootExecutor executor(symbol_table_stack_);
  bool concat = false;
  std::string str_source;

  while (true) {
    std::string line = func(&executor, concat);
    if (concat) {
      str_source += std::string("\n") +  line;
    } else {
      str_source = line;
    }

    if (str_source.empty()) {
      continue;
    }

    Lexer l(str_source);
    TokenStream ts = l.Scanner();
    Parser p(std::move(ts));
    auto res = p.AstGen();
    std::unique_ptr<StatementList> stmt_list = res.MoveAstNode();

    if (p.nerrors() == 0) {
      concat = false;
      RegisterVars();
      executor.Exec(stmt_list.get());
    } else {
      if (p.StmtIncomplete()) {
        concat = true;
        continue;
      } else {
        concat = false;
        Message msg = p.Msgs();
        throw RunTimeError(RunTimeError::ErrorCode::PARSER, msg.msg(),
                           Position{msg.line(), msg.pos()});
      }
    }
  }
}

std::shared_ptr<Object> Interpreter::LookupSymbol(const std::string& name) {
  std::shared_ptr<Object> obj;
  bool exists = false;

  std::tie(obj, exists) = symbol_table_stack_.LookupObj(name);

  if (exists) {
    return obj;
  }

  return obj = std::shared_ptr<Object>(nullptr);
}

Executor* Interpreter::ExecutorPtr() {

}

std::vector<std::string> SplitFileLines(const std::string str_file) {
  std::vector<std::string> strs;
  boost::split(strs, str_file, boost::is_any_of("\n"));

  return strs;
}

}
}
