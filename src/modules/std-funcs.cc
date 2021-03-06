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

#include "std-funcs.h"

#include "utils/check.h"
#include "utils/glob.h"
#include "parser/parser.h"
#include "interpreter/scope-executor.h"
#include "run_time_error.h"
#include "env-shell.h"

namespace shpp {
namespace internal {
namespace module {
namespace stdf {

static void PrintHelper(Object::Args&& args, Object::KWArgs&& kw_args, bool err) {
  auto it = kw_args.find("end");
  std::string str_end = "\n";
  bool flush = true;

  if (it != kw_args.end()) {
    SHPP_FUNC_CHECK_PARAM_TYPE(it->second, print, STRING)
    str_end = static_cast<StringObject&>(*kw_args["end"]).value();
  }

  it = kw_args.find("flush");

  if (it != kw_args.end()) {
    SHPP_FUNC_CHECK_PARAM_TYPE(it->second, print, BOOL)
    flush = static_cast<BoolObject&>(*kw_args["flush"]).value();
  }

  if (err) {
    for (auto& e: args) {
      std::cerr << e->Print();
    }

    std::cerr  << str_end;
  } else {
    for (auto& e: args) {
      std::cout << e->Print();
    }

    std::cout  << str_end;
  }

  if (flush) {
    std::cout.flush();
  }
}

ObjectPtr PrintFunc::Call(Executor*, Args&& args, KWArgs&& kw_args) {
  PrintHelper(std::move(args), std::move(kw_args), false);

  return obj_factory_.NewNull();
}

ObjectPtr PrintErrFunc::Call(Executor*, Args&& args, KWArgs&& kw_args) {
  PrintHelper(std::move(args), std::move(kw_args), true);

  return obj_factory_.NewNull();
}

ObjectPtr ReadFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS_UNTIL(params, 1, read)

  if (params.size()  == 1) {
    SHPP_FUNC_CHECK_PARAM_TYPE(params[0], prompt, STRING)
    std::cout << static_cast<StringObject&>(*params[0]).value();
  }

  std::string str;

  if (!std::getline(std::cin, str)) {
    return obj_factory_.NewNull();
  }

  return obj_factory_.NewString(str);
}

ObjectPtr LenFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 1, len)

  long int size = params[0]->Len();

  return obj_factory_.NewInt(static_cast<int>(size));
}

ObjectPtr CopyFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 1, copy)

  return params[0]->Copy();
}

ObjectPtr CompFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 2, comp)

  ObjectPtr obj_resp = params[0]->Lesser(params[1]);

  if (obj_resp->type() != Object::ObjectType::BOOL) {
    throw RunTimeError(RunTimeError::ErrorCode::INCOMPATIBLE_TYPE,
                       boost::format("operator less must return bool"));
  }

  return obj_resp;
}

ObjectPtr RangeFunc::Call(Executor*, Args&& params, KWArgs&&) {
  ObjectPtr obj = obj_factory_.NewRangeIterType();
  RangeIterType& range_it_type = static_cast<RangeIterType&>(*obj);
  return range_it_type.Constructor(nullptr, std::move(params));
}

ObjectPtr AssertFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS_AT_LEAST(params, 1, assert)
  SHPP_FUNC_CHECK_NUM_PARAMS_UNTIL(params, 2, assert)
  SHPP_FUNC_CHECK_PARAM_TYPE(params[0], test, BOOL)

  std::string msg = "Assert throw error";
  if (params.size() == 2) {
    SHPP_FUNC_CHECK_PARAM_TYPE(params[1], msg, STRING)
    std::string msg = static_cast<StringObject&>(*params[1]).value();
  }

  bool v = static_cast<BoolObject&>(*params[0]).value();

  if (!v) {
    throw RunTimeError(RunTimeError::ErrorCode::ASSERT,
                       boost::format(msg));
  }

  return obj_factory_.NewNull();
}

ObjectPtr GetAttrObjFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 1, object)
  SHPP_FUNC_CHECK_PARAM_TYPE(params[0], object, DECL_OBJ)

  DeclClassObject& obj = static_cast<DeclClassObject&>(*params[0]);
  auto& sym_table = obj.SymTable();

  std::map<std::string, ObjectPtr> map = sym_table->SymMap();
  std::vector<std::pair<ObjectPtr, ObjectPtr>> elements;
  ObjectFactory obj_factory(symbol_table_stack());

  for (auto& m: map) {
    elements.push_back(std::pair<ObjectPtr, ObjectPtr>(
        obj_factory.NewString(m.first), m.second));
  }

  ObjectPtr obj_map = obj_factory.NewMap(std::move(elements));
  return obj_map;
}

ObjectPtr GetAttrTypeFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 1, object)

  ObjectPtr obj;

  if (params[0]->type() == ObjectType::TYPE) {
    obj = params[0];
  } else {
    obj = params[0]->ObjType();
  }

  std::vector<std::pair<ObjectPtr, ObjectPtr>> elements;
  ObjectFactory obj_factory(symbol_table_stack());

  while (obj) {
    TypeObject& tobj = static_cast<TypeObject&>(*obj);
    auto& sym_table = tobj.SymTable();

    std::map<std::string, ObjectPtr> map = sym_table->SymMap();

    for (auto& m: map) {
      elements.push_back(std::pair<ObjectPtr, ObjectPtr>(
          obj_factory.NewString(m.first), m.second));
    }

    obj = obj->BaseType();
  }

  ObjectPtr obj_map = obj_factory.NewMap(std::move(elements));
  return obj_map;
}

ObjectPtr GetLastForegroundPidFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 0, params)

  int pid = EnvShell::instance()->last_foreground_pid();

  if (pid < 0) {
    throw RunTimeError(RunTimeError::ErrorCode::INVALID_ARGS,
        boost::format("no command in foreground was executed"));
  }

  return obj_factory_.NewInt(pid);
}

ObjectPtr GetLastBackgroundPidFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 0, params)

  int pid = EnvShell::instance()->last_background_pid();

  if (pid < 0) {
    throw RunTimeError(RunTimeError::ErrorCode::INVALID_ARGS,
        boost::format("no command in background was executed"));
  }

  return obj_factory_.NewInt(pid);
}

ObjectPtr GetLastForegroundExitFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 0, params)

  int pid = EnvShell::instance()->last_foreground_exit_code();

  if (pid < 0) {
    throw RunTimeError(RunTimeError::ErrorCode::INVALID_ARGS,
        boost::format("no command in foreground was executed"));
  }

  return obj_factory_.NewInt(pid);
}

ObjectPtr IsInteractiveFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 0, params)

  int shell_terminal;
  int shell_is_interactive;

  // see if we are running interactively
  shell_terminal = STDIN_FILENO;
  shell_is_interactive = isatty(shell_terminal);

  bool v = shell_is_interactive?true:false;

  return obj_factory_.NewBool(v);
}

ObjectPtr GlobFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 1, params)
  SHPP_FUNC_CHECK_PARAM_TYPE(params[0], msg, STRING)

  ObjectFactory obj_factory(symbol_table_stack());
  const std::string& glob_str = static_cast<StringObject&>(*params[0]).value();

  Args glob_obj = ExecGlob(glob_str, symbol_table_stack());
  return obj_factory.NewArray(std::move(glob_obj));
}

ObjectPtr GlobRFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 1, params)
  SHPP_FUNC_CHECK_PARAM_TYPE(params[0], msg, STRING)

  ObjectFactory obj_factory(symbol_table_stack());
  const std::string& glob_str = static_cast<StringObject&>(*params[0]).value();

  Args glob_obj =
      ListTree(boost::filesystem::current_path(), glob_str,
      symbol_table_stack());

  return obj_factory.NewArray(std::move(glob_obj));
}

ObjectPtr DumpSymbolTableFunc::SpecialCall(Executor*,
    Args&& params, KWArgs&&, SymbolTableStack& curret_sym_tab) {
  if (params.size() > 0) {
    params[0]->symbol_table_stack().Dump();
    return obj_factory_.NewNull();
  }

  curret_sym_tab.Dump();
  return obj_factory_.NewNull();
}

ObjectPtr EvalFunc::SpecialCall(Executor*,
    Args&& params, KWArgs&&, SymbolTableStack& curret_sym_tab) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 1, params)
  SHPP_FUNC_CHECK_PARAM_TYPE(params[0], code, STRING)

  ObjectFactory obj_factory(symbol_table_stack());
  const std::string& code_str = static_cast<StringObject&>(*params[0]).value();

  Lexer l(code_str);
  TokenStream ts = l.Scanner();
  Parser p(std::move(ts));
  auto res = p.AstGen();
  auto stmt_list = res.MoveAstNode();

  try {
    if (p.nerrors() == 0) {
      RootExecutor executor(curret_sym_tab);
      executor.Exec(stmt_list.get());
    } else {
      Message msg = p.Msgs();
      throw RunTimeError(RunTimeError::ErrorCode::PARSER, msg.msg(),
                         Position{msg.line(), msg.pos()});
    }
  } catch (RunTimeError& e) {
    Message msg(Message::Severity::ERR, boost::format(e.msg()), e.pos().line,
                e.pos().col);

    throw RunTimeError (RunTimeError::ErrorCode::EVAL,
                        boost::format("eval error"))
        .AppendMsg(std::move(msg));
  }

  return obj_factory_.NewNull();
}

ObjectPtr InstanceOfFunc::Call(Executor*, Args&& params, KWArgs&&) {
  SHPP_FUNC_CHECK_NUM_PARAMS(params, 2, params)

  bool r = InstanceOf(params[0], params[1]);

  ObjectFactory obj_factory(symbol_table_stack());
  return obj_factory.NewBool(r);
}

}
}
}
}
