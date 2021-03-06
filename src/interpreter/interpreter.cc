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
#include "modules/sys.h"
#include "env-shell.h"

namespace shpp {
namespace internal {

Interpreter::Interpreter(bool main)
    : symbol_table_(SymbolTablePtr(new SymbolTable))
    , symbol_table_stack_(symbol_table_)
    , sys_symbol_table_stack_(symbol_table_stack_.SysTable()->ptr())
    , main_(main) {
  AlocTypes(symbol_table_stack_);

  module::stdf::RegisterModule(sys_symbol_table_stack_);
  module::env::RegisterModule(sys_symbol_table_stack_);
  module::sys::RegisterModule(symbol_table_stack_);
  cmds::stdf::RegisterCmds(symbol_table_stack_);
}

void Interpreter::InsertVar(const std::string& name, ObjectPtr obj) {
  SymbolAttr symbol(obj, true);
  symbol_table_stack_.InsertEntry(name, std::move(symbol));
}

void Interpreter::RegisterVars() {
  ObjectFactory obj_factory(symbol_table_stack_);

  InsertVar("__main__", obj_factory.NewBool(main_));
  RegisterSysVars();
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
  // avoid overwrite arguments with invalid value
  if (!main_) {
    return;
  }

  ObjectFactory obj_factory(symbol_table_stack_);
  std::shared_ptr<Object> sys_mod;
  sys_mod = symbol_table_stack_.LookupSys("sys").SharedAccess();

  std::vector<ObjectPtr> vec_objs;

  for (const auto& arg: args) {
    vec_objs.push_back(obj_factory.NewString(arg));
  }

  ObjectPtr obj_argv = obj_factory.NewArray(std::move(vec_objs));
  sys_mod->symbol_table_stack().SetEntry("argv", obj_argv);
}

void Interpreter::RegisterSysVars() {
  ObjectFactory obj_factory(symbol_table_stack_);
  std::shared_ptr<Object> sys_mod;
  sys_mod = symbol_table_stack_.LookupSys("sys").SharedAccess();

  ObjectPtr obj_argv = obj_factory.NewString("0.0.1");
  sys_mod->symbol_table_stack().SetEntry("version", obj_argv);
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

      if (main_) {
        RegisterMainModule(file.filename());
      }

      RootExecutor executor(symbol_table_stack_);
      executor.Exec(stmt_list_.get());
    } else {
      Message msg = p.Msgs();
      throw RunTimeError(RunTimeError::ErrorCode::PARSER, msg.msg(),
                         Position{msg.line(), msg.pos()});
    }
  } catch (RunTimeError& e) {
    ShowErrors(e, buffer.str(), file.filename());
  }
}

void Interpreter::ShowErrors(RunTimeError& e, const std::string& code,
    const std::string& filename) {
  // split the lines of file in a array to set line of error on each message
  std::vector<std::string> file_lines = SplitFileLines(code);

  // set filename and the line of the erro on each message
  for (auto& msg: e.messages()) {
    if (msg.file().empty()) {
      msg.file(filename);
    }

    // if the type of error is EVAL, the line of file doesn't match with the
    // line of real code, so at this time, the line won't be show
    if (e.err_code() == RunTimeError::ErrorCode::EVAL) {
      // insert empty line for EVAL error
      msg.line_error("");
    } else {
      std::string msg_code;
      std::ifstream fs(msg.file());

      if (fs.is_open()) {
        std::stringstream buffer;
        buffer << fs.rdbuf();
        msg_code = buffer.str();

        // subtract 1 because vector starts on 0 and line on 1
        msg.line_error(SplitFileLines(msg_code)[msg.line()-1]);
      } else {
        msg.line_error("");
      }
    }
  }

  e.file(filename);
  int pos = e.pos().line < 1? 1: e.pos().line;
  e.line_error(file_lines[pos - 1]);
  throw e;
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

void Interpreter::RegisterMainModule(const std::string& full_path) {
  // create a symbol table on the start
  SymbolTableStack table_stack;
  auto main_tab = symbol_table_stack_.MainTable();
  table_stack.Push(main_tab, true);

  auto obj_type = symbol_table_stack_.LookupSys("module").SharedAccess();
  ObjectPtr module_obj = ObjectPtr(new ModuleMainObject(obj_type,
      std::move(table_stack)));

  namespace fs = boost::filesystem;
  fs::path path = fs::system_complete(full_path);

  EnvShell::instance()->GetImportTable().AddModule(path.string(), module_obj);
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

std::vector<std::string> SplitFileLines(const std::string str_file) {
  std::vector<std::string> strs;
  boost::split(strs, str_file, boost::is_any_of("\n"));

  return strs;
}

}
}
