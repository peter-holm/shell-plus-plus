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

#include "stmt-executor.h"

#include <string>
#include <iostream>
#include <boost/variant.hpp>

#include "scope-executor.h"
#include "assign-executor.h"
#include "expr-executor.h"
#include "objects/func-object.h"
#include "cmd-executor.h"
#include "utils/scope-exit.h"
#include "utils/check.h"
#include "objects/obj-type.h"

namespace shpp {
namespace internal {

void StmtListExecutor::Exec(AstNode* node) {
  StatementList* stmt_list = static_cast<StatementList*>(node);
  StmtExecutor stmt_exec(this, symbol_table_stack());

  for (AstNode* stmt: stmt_list->children()) {
    // when stop flag is set inside some control struct of function
    // it don't pass ahead this point, because this struct must
    // set parent only if it will not use the flag
    // for example: loops must not set this flag for continue and
    // break, but must set this flag for return and throw
    if (stop_flag_ == StopFlag::kGo) {
      stmt_exec.Exec(stmt);
    } else {
      return;
    }
  }
}

void StmtListExecutor::set_stop(StopFlag flag) {
  stop_flag_ = flag;

  if (parent() == nullptr) {
    return;
  }

  parent()->set_stop(flag);
}

ObjectPtr FuncDeclExecutor::FuncObj(AstNode* node) {
  if (node->type() == AstNode::NodeType::kFunctionDeclaration) {
    return FuncObjAux<FunctionDeclaration*>(
      static_cast<FunctionDeclaration*>(node));
  } else {
    return FuncObjAux<FunctionExpression*>(
      static_cast<FunctionExpression*>(node));
  }
}

template<class T>
ObjectPtr FuncDeclExecutor::FuncObjAux(T fdecl_node) {
  // handle Function ast node, only decide if it is Declaration or
  // Expression when it is needed
  auto vec = fdecl_node->children();
  size_t variadic_count = 0;
  std::vector<std::string> param_names;
  std::unordered_map<std::string, ObjectPtr> default_values;

  // if the method is declared inside of a class
  // insert the parameter this
  if (method_ && !fstatic_) {
    param_names.push_back(std::string("this"));
  }

  // flag to help to check if defaults values are only in the last params
  bool default_value = false;
  AssignableListExecutor assign_value_exec(this, symbol_table_stack());

  for (FunctionParam* param: vec) {
    if (param->variadic()) {
      variadic_count++;
    }

    if (!((variadic_count != 0) && param->has_value())) {
      param_names.push_back(param->id()->name());
    }

    // check if the param has default value
    if (param->has_value()) {
      default_value = true;
      ObjectPtr obj_value(assign_value_exec.ExecAssignable(param->value()));
      default_values.insert(std::pair<std::string, ObjectPtr>(
          param->id()->name(), obj_value));
    } else {
      if (default_value) {
        // error, only the param in the end can have default values
        throw RunTimeError(RunTimeError::ErrorCode::INCOMPATIBLE_TYPE,
                           boost::format("no default value can't appear "
                                         "after a default value parameter"),
                           param->pos());
      }
    }
  }

  // only the last parameter can be variadic
  if (variadic_count > 1) {
    throw RunTimeError(RunTimeError::ErrorCode::INCOMPATIBLE_TYPE,
        boost::format("not allowed more than 1 variadic parameter"),
        fdecl_node->pos());
  }

  // if has variadic argument the last parameters has to have default values
  if (variadic_count == 1) {
    size_t i = vec.size() - 1;

    // iterate over all parameter after variadic and verify if all of them
    // has default value
    while (!vec[i]->variadic()) {
      if (!vec[i]->has_value()) {
        throw RunTimeError(RunTimeError::ErrorCode::INCOMPATIBLE_TYPE,
                       boost::format("all parameters must have default values "
                          "after variadic parameter"), fdecl_node->pos());
      }

      i--;
    }
  }

  SymbolTableStack st_stack(symbol_table_stack());

  std::string func_name = "";
  bool fstatic = false;

  // now we need to know if the function is lambda or declaration
  // TODO: on c++17 change this shit piece of code to if constexpr () {}
  if (fdecl_node->type() == AstNode::NodeType::kFunctionDeclaration) {
    // convert to AstNode and after convert to the correct type to avoid error
    // because FunctionExpression and FunctionDeclaration has no relationship
    // between one and anoter
    FunctionDeclaration* fdecl = static_cast<FunctionDeclaration*>(
        static_cast<AstNode*>(fdecl_node));
    func_name = fdecl->name()->name();
    fstatic = fdecl->fstatic();
  }

  try {
    ObjectPtr fobj(obj_factory_.NewFuncDeclObject(func_name,
        fdecl_node->block(), std::move(st_stack), std::move(param_names),
        std::move(default_values), variadic_count == 1?true:false, lambda_,
        fstatic));

    return fobj;
  } catch (RunTimeError& e) {
    throw RunTimeError(e.err_code(), e.msg(), fdecl_node->pos(), e.messages());
  }
}

void FuncDeclExecutor::Exec(AstNode* node) {
  FunctionDeclaration* fdecl_node = static_cast<FunctionDeclaration*>(node);

  ObjectPtr fobj(FuncObj(node));

  // global symbol
  SymbolAttr entry(fobj, true);

  try {
    symbol_table_stack().InsertEntry(fdecl_node->name()->name(),
                                     std::move(entry));
  } catch (RunTimeError& e) {
    throw RunTimeError(e.err_code(), e.msg(), node->pos(), e.messages());
  }
}

void FuncDeclExecutor::set_stop(StopFlag flag) {
  if (parent() == nullptr) {
    return;
  }

  parent()->set_stop(flag);
}

ObjectPtr ClassDeclExecutor::SuperClass(Expression* super) {
  ExpressionExecutor expr_exec(this, symbol_table_stack());
  ObjectPtr base_obj = expr_exec.Exec(super);

  if (base_obj->type() != Object::ObjectType::TYPE) {
    throw RunTimeError(RunTimeError::ErrorCode::INCOMPATIBLE_TYPE,
                       boost::format("'%1%' is not a valid type for super"
                       " class")%base_obj->ObjectName(), super->pos());
  }

  return base_obj;
}

void ClassDeclExecutor::Exec(AstNode* node, bool inner,
    ObjectPtr inner_type_obj) {
  ClassDeclaration* class_decl_node = static_cast<ClassDeclaration*>(node);

  // handle class block
  ClassBlock* block = class_decl_node->block();
  ClassDeclList* decl_list = block->decl_list();

  // handle super class
  ObjectPtr base;
  if (class_decl_node->has_parent()) {
    base = SuperClass(class_decl_node->parent());
  }

  ObjectPtr type_obj;

  try {
    // if the class implements some interfaces, verify if it is a valid one
    std::vector<ObjectPtr> ifaces;
    if (class_decl_node->has_interfaces()) {
      ifaces = InterfaceDeclExecutor::HandleInterfaces(this,
          class_decl_node->interfaces(), symbol_table_stack());
    }

    type_obj = obj_factory_.NewDeclType(class_decl_node->name()->name(), base,
        std::move(ifaces), class_decl_node->abstract(),
        class_decl_node->is_final());
  } catch (RunTimeError& e) {
    throw RunTimeError(e.err_code(), e.msg(),
        class_decl_node->pos(), e.messages());
  }

  // insert all declared methods on symbol table
  std::vector<AstNode*> decl_vec = decl_list->children();

  DeclClassType& decl_class = static_cast<DeclClassType&>(*type_obj);

  for (auto decl: decl_vec) {
    try {
      if (decl->type() == AstNode::NodeType::kFunctionDeclaration) {
        // insert method on symbol table of class
        FunctionDeclaration* fdecl = static_cast<FunctionDeclaration*>(decl);

        // the last argument specify that is a static method inside the class
        FuncDeclExecutor fexec(this, symbol_table_stack(), true, false,
            fdecl->fstatic());

        // handle no abstract method
        if (fdecl->has_block()) {
          decl_class.RegiterMethod(fdecl->name()->name(), fexec.FuncObj(decl));
        }
      } else if (decl->type() == AstNode::NodeType::kClassDeclaration) {
        ClassDeclaration* class_decl = static_cast<ClassDeclaration*>(decl);

        // insert inner class on type_obj symbol table, insted of its own
        ClassDeclExecutor class_exec(this, decl_class.GlobalSymTableStack());
        class_exec.Exec(class_decl, true, type_obj);
      } else if (decl->type() == AstNode::NodeType::kVariableDeclaration) {
        ExecVarDecl(decl, decl_class);
      }
    } catch (RunTimeError& e) {
      throw RunTimeError(e.err_code(), e.msg(), decl->pos(), e.messages());
    }
  }

  try {
    // check if declared class implemented all abstract methods
    decl_class.CheckInterfaceCompatibility();
  } catch (RunTimeError& e) {
    throw RunTimeError(e.err_code(), e.msg(),
        class_decl_node->interfaces()->pos(), e.messages());
  }

  if (inner) {
    SymbolAttr symbol_obj(type_obj, true);
    static_cast<DeclClassType&>(*inner_type_obj).SymTableStack().InsertEntry(
        class_decl_node->name()->name(), std::move(symbol_obj));
    return;
  }

  SymbolAttr symbol_obj(type_obj, true);
  symbol_table_stack().InsertEntry(class_decl_node->name()->name(),
                                   std::move(symbol_obj));
}

void ClassDeclExecutor::ExecVarDecl(AstNode* node, DeclClassType& decl_class) {
  VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(node);

  std::string name = var_decl->name()->name();

  AssignableListExecutor assign_exec(this, symbol_table_stack());
  ObjectPtr obj_value = assign_exec.ExecAssignable(var_decl->value());

  decl_class.RegisterAttr(name, obj_value);
}

void ClassDeclExecutor::set_stop(StopFlag flag) {
  if (parent() == nullptr) {
    return;
  }

  parent()->set_stop(flag);
}

std::vector<ObjectPtr> InterfaceDeclExecutor::HandleInterfaces(
    Executor* parent, ExpressionList* ifaces_node,
    SymbolTableStack& symbol_table_stack) {
  ExprListExecutor expr_list(parent, symbol_table_stack);
  std::vector<ObjectPtr> ifaces_obj = expr_list.Exec(ifaces_node);

  // check if all objects are intefaces
  for (auto& iface: ifaces_obj) {
    if (iface->type() != Object::ObjectType::DECL_IFACE) {
      throw RunTimeError(RunTimeError::ErrorCode::INCOMPATIBLE_TYPE,
                       boost::format("'%1%' is not an interface")
                       %iface->ObjectName(), ifaces_node->pos());
    }
  }

  return ifaces_obj;
}

void InterfaceDeclExecutor::Exec(AstNode* node) {
  InterfaceDeclaration* iface_node = static_cast<InterfaceDeclaration*>(node);
  InterfaceBlock* block = iface_node->block();
  InterfaceDeclList* decl_list = block->decl_list();

  std::vector<AstNode*> decl_vec = decl_list->children();

  // if the class implements some interfaces, verify if it is a valid one
  std::vector<ObjectPtr> ifaces;

  try {
    if (iface_node->has_interfaces()) {
    ifaces = InterfaceDeclExecutor::HandleInterfaces(this,
        iface_node->interfaces(), symbol_table_stack());
    }
  } catch (RunTimeError& e) {
    throw RunTimeError(e.err_code(), e.msg(), iface_node->pos(), e.messages());
  }

  const std::string& iface_name = iface_node->name()->name();

  ObjectPtr iface_obj = obj_factory_.NewDeclIFace(iface_name,
      std::move(ifaces));

  for (auto decl: decl_vec) {
    try {
      if (decl->type() == AstNode::NodeType::kFunctionDeclaration) {
        FuncDeclExecutor fexec(this, symbol_table_stack(), true);
        AbstractMethod abstract_method(static_cast<FuncObject&>(
            *fexec.FuncObj(decl)));

        DeclInterface& decl_iface = static_cast<DeclInterface&>(*iface_obj);
        const std::string& fname =
            static_cast<FunctionDeclaration*>(decl)->name()->name();
        decl_iface.AddMethod(fname, std::move(abstract_method));
      }
    } catch (RunTimeError& e) {
      throw RunTimeError(e.err_code(), e.msg(), decl->pos(), e.messages());
    }
  }

  SymbolAttr symbol_obj(iface_obj, true);
  symbol_table_stack().InsertEntry(iface_name, std::move(symbol_obj));
}

void TryCatchExecutor::Exec(TryCatchStatement* node) {
  bool catch_executed = false;
  bool finally_executed = false;

  // create a new table for while scope
  symbol_table_stack().NewTable();

  // scope exit case an excpetion thrown
  auto cleanup = MakeScopeExit([&]() {
    // remove the scope
    symbol_table_stack().Pop();
  });
  IgnoreUnused(cleanup);

  BlockExecutor block_exec(this, symbol_table_stack());

  try {
    // execute try block
    block_exec.Exec(node->try_block());
  } catch (RunTimeError& e) {
    ObjectPtr ojb_excpt;

    // check if the exception is an object throw by the user
    // or a intern error
    if (e.is_object_expection()) {
      ojb_excpt = e.except_obj();
    } else {
      ojb_excpt = MapExceptionError(e, symbol_table_stack());
    }

    // iterate over catch statement
    for (auto& catch_block: node->catch_list()) {
      ExprListExecutor expr_list_exec(this, symbol_table_stack());
      std::vector<ObjectPtr> obj_res_list = expr_list_exec.Exec(
          catch_block->exp_list());

      // verify if the exception object is intance of any object from
      // catch list
      if (IsInstanceOfCaseObject(obj_res_list, ojb_excpt)) {
        if (catch_block->has_var()) {
          // if the var was set by catch XXX as my_var, insert this var
          // on symbol table
          InsertCatchVar(catch_block->var()->name(), ojb_excpt,
              catch_block->pos());
        }

        // mark that one catch block was executed and execute the catch block
        catch_executed = true;
        block_exec.Exec(catch_block->block());

        // only one catch must be executed, so out of the loop
        break;
      }
    }

    // verify if try catch has finally block, and execute, becuase finally
    // must be executed always, even if no catch match the exception
    // and this exception was not captured
    if (node->has_finally()) {
      finally_executed = true;
      block_exec.Exec(node->finally()->block());
    }

    // if the exception was not captured, throw the exception
    if (!catch_executed) {
      throw e;
    }
  }

  // if there is no exception, finally block must be executed anyway
  if (node->has_finally() && !finally_executed) {
    block_exec.Exec(node->finally()->block());
  }
}

bool TryCatchExecutor::IsInstanceOfCaseObject(
    std::vector<ObjectPtr>& obj_res_list, ObjectPtr& ojb_except) {
  for (auto& exp_obj: obj_res_list) {
    if (InstanceOf(ojb_except, exp_obj)) {
      return true;
    }
  }

  return false;
}

void TryCatchExecutor::InsertCatchVar(const std::string& name,
    ObjectPtr& ojb_excpt, Position pos) {
  SymbolAttr entry(ojb_excpt, true);

  try {
    symbol_table_stack().InsertEntry(name, std::move(entry));
  } catch (RunTimeError& e) {
    throw RunTimeError(e.err_code(), e.msg(), pos, e.messages());
  }
}

void TryCatchExecutor::set_stop(StopFlag flag) {
  if (parent() == nullptr) {
    return;
  }

  if (flag == StopFlag::kThrow) {
    parent()->set_stop(StopFlag::kGo);
  } else {
    parent()->set_stop(flag);
  }
}

void ThrowExecutor::Exec(ThrowStatement* node) {
  ExpressionExecutor expr_executor(this, symbol_table_stack());
  ObjectPtr obj_throw = expr_executor.Exec(node->exp());

  throw RunTimeError(obj_throw, node->pos());
}

void StmtExecutor::Exec(AstNode* node) {
  switch (node->type()) {
    case AstNode::NodeType::kAssignmentStatement: {
      AssignExecutor exec(this, symbol_table_stack());
      return exec.Exec(node);
    } break;

    case AstNode::NodeType::kExpressionStatement: {
      ExpressionExecutor exec(this, symbol_table_stack());
      exec.Exec(static_cast<ExpressionStatement&>(*node).exp());
      return;
    } break;

    case AstNode::NodeType::kFunctionCall: {
      ExpressionExecutor exec(this, symbol_table_stack());
      exec.Exec(static_cast<FunctionCall*>(node));
      return;
    } break;

    case AstNode::NodeType::kFunctionDeclaration: {
      FuncDeclExecutor fdecl_executor(this, symbol_table_stack());
      fdecl_executor.Exec(node);
    } break;

    case AstNode::NodeType::kReturnStatement: {
      ReturnExecutor ret_executor(this, symbol_table_stack());
      ret_executor.Exec(node);
    } break;

    case AstNode::NodeType::kIfStatement: {
      IfElseExecutor ifelse_executor(this, symbol_table_stack());
      ifelse_executor.Exec(static_cast<IfStatement*>(node));
    } break;

    case AstNode::NodeType::kWhileStatement: {
      WhileExecutor while_executor(this, symbol_table_stack());
      while_executor.Exec(static_cast<WhileStatement*>(node));
    } break;

    case AstNode::NodeType::kForInStatement: {
      ForInExecutor for_executor(this, symbol_table_stack());
      for_executor.Exec(static_cast<ForInStatement*>(node));
    } break;

    case AstNode::NodeType::kClassDeclaration: {
      ClassDeclExecutor class_decl_executor(this, symbol_table_stack());
      class_decl_executor.Exec(static_cast<ClassDeclaration*>(node));
    } break;

    case AstNode::NodeType::kInterfaceDeclaration: {
      InterfaceDeclExecutor iface_decl_executor(this, symbol_table_stack());
      iface_decl_executor.Exec(static_cast<InterfaceDeclaration*>(node));
    } break;

    case AstNode::NodeType::kBreakStatement: {
      BreakExecutor break_executor(this, symbol_table_stack());
      break_executor.Exec(static_cast<BreakStatement*>(node));
    } break;

    case AstNode::NodeType::kContinueStatement: {
      ContinueExecutor continue_executor(this, symbol_table_stack());
      continue_executor.Exec(static_cast<ContinueStatement*>(node));
    } break;

    case AstNode::NodeType::kCmdFull: {
      CmdExecutor cmd_full(this, symbol_table_stack());
      cmd_full.Exec(static_cast<CmdFull*>(node));
    } break;

    case AstNode::NodeType::kSwitchStatement: {
      SwitchExecutor switch_stmt(this, symbol_table_stack());
      switch_stmt.Exec(static_cast<SwitchStatement*>(node));
    } break;

    case AstNode::NodeType::kDeferStatement: {
      DeferExecutor defer(this, symbol_table_stack());
      defer.Exec(static_cast<DeferStatement*>(node));
    } break;

    case AstNode::NodeType::kCmdDeclaration: {
      CmdDeclExecutor cmd(this, symbol_table_stack());
      cmd.Exec(static_cast<DeferStatement*>(node));
    } break;

    case AstNode::NodeType::kImportStatement: {
      ImportExecutor import(this, symbol_table_stack());
      import.Exec(static_cast<ImportStatement*>(node));
    } break;

    case AstNode::NodeType::kAliasDeclaration: {
      AliasDeclExecutor alias(this, symbol_table_stack());
      alias.Exec(static_cast<AliasDeclaration*>(node));
    } break;

    case AstNode::NodeType::kDelStatement: {
      DelStmtExecutor del(this, symbol_table_stack());
      del.Exec(static_cast<DelStatement*>(node));
    } break;

    case AstNode::NodeType::kTryCatchStatement: {
      TryCatchExecutor try_catch(this, symbol_table_stack());
      try_catch.Exec(static_cast<TryCatchStatement*>(node));
    } break;

    case AstNode::NodeType::kThrowStatement: {
      ThrowExecutor throw_exec(this, symbol_table_stack());
      throw_exec.Exec(static_cast<ThrowStatement*>(node));
    } break;

    case AstNode::NodeType::kStatementList: {
      StmtListExecutor stmt_list(this, symbol_table_stack());
      stmt_list.Exec(node);
    } break;

    case AstNode::NodeType::kBlock: {
      StmtListExecutor stmt_list(this, symbol_table_stack());
      stmt_list.Exec(static_cast<Block*>(node)->stmt_list());
    } break;

    case AstNode::NodeType::kVarEnvStatement: {
      VarEnvExecutor varenv_exec(this, symbol_table_stack());
      varenv_exec.Exec(static_cast<VarEnvStatement*>(node));
    } break;

    case AstNode::NodeType::kGlobalAssignmentStatement: {
      GlobalAssignmentExecutor global_exec(this, symbol_table_stack());
      global_exec.Exec(static_cast<GlobalAssignmentStatement*>(node));
    } break;

    default: {
      throw RunTimeError(RunTimeError::ErrorCode::INVALID_OPCODE,
          boost::format("invalid opcode of statement: %1%")%
          AstNodeStr(static_cast<size_t>(node->type())), node->pos());
    }
  }
}

void StmtExecutor::set_stop(StopFlag flag) {
  if (parent() == nullptr) {
    return;
  }

  parent()->set_stop(flag);
}

void ReturnExecutor::Exec(AstNode* node) {
  ReturnStatement* ret_node = static_cast<ReturnStatement*>(node);

  if (!ret_node->is_void()) {
    AssignableListExecutor assign_list(this, symbol_table_stack());
    std::vector<ObjectPtr> vret = assign_list.Exec(ret_node->assign_list());

    // if vret there is only one element, return this element
    if (vret.size() == 1) {
      symbol_table_stack().SetEntryOnFunc("%return", vret[0]);
    } else {
      // convert vector to tuple object and insert it on symbol table
      // with reserved name
      ObjectPtr tuple_obj(obj_factory_.NewTuple(std::move(vret)));
      symbol_table_stack().SetEntryOnFunc("%return", tuple_obj);
    }
  } else {
    // return null
    ObjectPtr null_obj(obj_factory_.NewNull());
    symbol_table_stack().SetEntryOnFunc("%return", null_obj);
  }

  // set stop return
  parent()->set_stop(StopFlag::kReturn);
}

void ReturnExecutor::set_stop(StopFlag flag) {
  parent()->set_stop(flag);
}

void IfElseExecutor::Exec(IfStatement* node) {
  // create a new table for if else scope
  symbol_table_stack().NewTable();

  // scope exit case an excpetion thrown
  auto cleanup = MakeScopeExit([&]() {
    // remove the scope
    symbol_table_stack().Pop();
  });
  IgnoreUnused(cleanup);

  // Executes if expresion
  ExpressionExecutor expr_exec(this, symbol_table_stack());
  ObjectPtr obj_exp = expr_exec.Exec(node->exp());

  bool cond;

  try {
    cond = static_cast<BoolObject&>(*obj_exp->ObjBool()).value();
  } catch (RunTimeError& e) {
    throw RunTimeError(e.err_code(), e.msg(), node->exp()->pos(), e.messages());
  }

  BlockExecutor block_exec(this, symbol_table_stack());

  if (cond) {
    block_exec.Exec(node->then_block());
  } else {
    if (node->has_else()) {
      // chain multiple if-else statements: if {...} else if {...} else
      if (node->else_block()->type() == AstNode::NodeType::kIfStatement) {
        IfElseExecutor if_exec(this, symbol_table_stack());
        if_exec.Exec(static_cast<IfStatement*>(node->else_block()));
      } else {
        block_exec.Exec(node->else_block());
      }
    }
  }
}

void IfElseExecutor::set_stop(StopFlag flag) {
  parent()->set_stop(flag);
}

void WhileExecutor::Exec(WhileStatement* node) {
  // create a new table for while scope
  symbol_table_stack().NewTable();

  // scope exit case an excpetion thrown
  auto cleanup = MakeScopeExit([&]() {
    // remove the scope
    symbol_table_stack().Pop();
  });
  IgnoreUnused(cleanup);

  // Executes if expresion
  ExpressionExecutor expr_exec(this, symbol_table_stack());

  auto fn_exp = [&](Expression* exp)-> bool {
    // if break was called or throw or return must exit from loop
    if (stop_flag_ == StopFlag::kBreak || stop_flag_ == StopFlag::kThrow ||
        stop_flag_ == StopFlag::kReturn) {
      return false;
    }

    ObjectPtr obj_exp = expr_exec.Exec(exp);

    try {
      return static_cast<BoolObject&>(*obj_exp->ObjBool()).value();
    } catch (RunTimeError& e) {
      throw RunTimeError(e.err_code(), e.msg(), node->exp()->pos(),
          e.messages());
    }
  };

  while (fn_exp(node->exp())) {
    // create a new table for while scope
    symbol_table_stack().NewTable();

    // scope exit case an excpetion thrown
    auto cleanup = MakeScopeExit([&]() {
      // remove the scope
      symbol_table_stack().Pop();
    });
    IgnoreUnused(cleanup);

    BlockExecutor block_exec(this, symbol_table_stack());
    block_exec.Exec(node->block());
  }
}

void WhileExecutor::set_stop(StopFlag flag) {
  stop_flag_ = flag;

  if (parent() == nullptr) {
    return;
  }

  if (flag == StopFlag::kBreak || flag == StopFlag::kContinue) {
    parent()->set_stop(StopFlag::kGo);
  } else {
    parent()->set_stop(flag);
  }
}

void ForInExecutor::Exec(ForInStatement* node) {
  // create a new table for while scope
  symbol_table_stack().NewTable();

  // scope exit case an excpetion thrown
  auto cleanup = MakeScopeExit([&]() {
    // remove the scope
    symbol_table_stack().Pop();
  });
  IgnoreUnused(cleanup);

  std::vector<Expression*> exp_list = node->exp_list()->children();

  // Executes the test side of for statemente
  ExprListExecutor expr_list(this, symbol_table_stack());
  auto containers = expr_list.Exec(node->test_list());

  std::vector<ObjectPtr> it_values;

  // get iterator of each object
  for (auto& it: containers) {
    ObjectPtr obj(it->ObjIter(it));
    it_values.push_back(obj);
  }

  auto fn_exp = [&]()-> bool {
    // if break was called or throw or return must exit from loop
    if (stop_flag_ == StopFlag::kBreak || stop_flag_ == StopFlag::kThrow ||
        stop_flag_ == StopFlag::kReturn) {
      return false;
    }

    // check if all items on it_values has next
    for (auto& it: it_values) {
      // as it is a reference, change the pointer inside it_values
      ObjectPtr has_next_obj = it->HasNext();

      if (has_next_obj->type() != Object::ObjectType::BOOL) {
        throw RunTimeError(RunTimeError::ErrorCode::INCOMPATIBLE_TYPE,
                           boost::format("expect bool from __has_next__"),
                           node->test_list()->pos());
      }

      bool v = static_cast<BoolObject&>(*has_next_obj).value();
      if (!v) {
        return false;
      }
    }

    try {
      Assign(exp_list, it_values);
    } catch (RunTimeError& e) {
      throw RunTimeError(e.err_code(), e.msg(), node->pos(), e.messages());
    }

    return true;
  };

  // executes for statement in fact
  while (fn_exp()) {
    // create a new table for while scope
    symbol_table_stack().NewTable();

    // scope exit case an excpetion thrown
    auto cleanup = MakeScopeExit([&]() {
      // remove the scope
      symbol_table_stack().Pop();
    });
    IgnoreUnused(cleanup);

    BlockExecutor block_exec(this, symbol_table_stack());
    block_exec.Exec(node->block());
  }
}

void ForInExecutor::Assign(std::vector<Expression*>& exp_list,
    std::vector<ObjectPtr>& it_values) {
  // assign the it_values->Next to vars references to be used inside
  // block of for statemente
  AssignExecutor assign_exec(this, symbol_table_stack());

  std::vector<ObjectPtr> values;
  for (size_t i = 0; i < it_values.size(); i++) {
    values.push_back(it_values[i]->Next());
  }

  assign_exec.Assign(exp_list, values);
}

void ForInExecutor::set_stop(StopFlag flag) {
  stop_flag_ = flag;

  if (parent() == nullptr) {
    return;
  }

  if (flag != StopFlag::kBreak && flag != StopFlag::kContinue) {
    parent()->set_stop(flag);
  }
}

void BreakExecutor::Exec(BreakStatement* /*node*/) {
  // set stop break
  parent()->set_stop(StopFlag::kBreak);
}

void BreakExecutor::set_stop(StopFlag flag) {
  parent()->set_stop(flag);
}

void ContinueExecutor::Exec(ContinueStatement* /*node*/) {
  // set stop break
  parent()->set_stop(StopFlag::kContinue);
}

void ContinueExecutor::set_stop(StopFlag flag) {
  parent()->set_stop(flag);
}

bool SwitchExecutor::MatchAnyExp(ObjectPtr exp,
                                 std::vector<ObjectPtr> &&exp_list) {
  // compare each expression from list with exp
  for (auto& e: exp_list) {
    ObjectPtr res(exp->Equal(e));

    if (res->type() != Object::ObjectType::BOOL) {
      continue;
    }

    BoolObject& obj_test = static_cast<BoolObject&>(*res);

    if (obj_test.value()) {
      return true;
    }
  }

  return false;
}

void SwitchExecutor::Exec(SwitchStatement* node) {
  // create a new table for while scope
  symbol_table_stack().NewTable();

  // scope exit case an excpetion thrown
  auto cleanup = MakeScopeExit([&]() {
    // remove the scope
    symbol_table_stack().Pop();
  });
  IgnoreUnused(cleanup);

  BlockExecutor block_exec(this, symbol_table_stack());
  ExpressionExecutor expr_exec(this, symbol_table_stack());
  ObjectPtr obj_exp_switch;

  if (node->has_exp()) {
     obj_exp_switch = expr_exec.Exec(node->exp());
  } else {
    // if the switch doesn't have expression, compare with true
    ObjectFactory obj_factory(symbol_table_stack());
    obj_exp_switch = obj_factory.NewBool(true);
  }

  // flag to mark if any case statement was executed
  bool any_case_executed = false;

  // executes each case and compare each expression
  std::vector<CaseStatement*> case_list = node->case_list();

  for (auto& c: case_list) {
    // create a new table for while scope
    symbol_table_stack().NewTable();

    // scope exit case an excpetion thrown
    auto cleanup = MakeScopeExit([&]() {
      // remove the scope
      symbol_table_stack().Pop();
    });
    IgnoreUnused(cleanup);

    ExprListExecutor expr_list_exec(this, symbol_table_stack());
    std::vector<ObjectPtr> obj_res_list = expr_list_exec.Exec(c->exp_list());

    // compare each expression with switch expression
    bool comp;
    try {
      comp = MatchAnyExp(obj_exp_switch, std::move(obj_res_list));
    } catch (RunTimeError& e) {
      throw RunTimeError(e.err_code(), e.msg(), c->pos(), e.messages());
    }

    if (comp) {
      any_case_executed = true;

      // if any expression match with case expression, executes case's block
      block_exec.Exec(c->block());
    }
  }

  // if any case was not executed, so execute the default clause
  // if switch statement has one
  if (!any_case_executed && node->has_default()) {
    block_exec.Exec(node->default_stmt()->block());
  }
}

void SwitchExecutor::set_stop(StopFlag flag) {
  parent()->set_stop(flag);
}

void DeferExecutor::Exec(DeferStatement *node) {
  // push the statement on main block parent
  Executor* exec = GetMainExecutor();

  if (exec != nullptr) {
    SymbolTableStack sym_stack(symbol_table_stack());
    if (symbol_table_stack().HasClassTable()) {
      sym_stack.Append(symbol_table_stack().GetUntilClassTable());
    } else {
      sym_stack.Append(symbol_table_stack().GetUntilFuncTable());
    }

    std::tuple<Statement*, SymbolTableStack> t(node->stmt(), sym_stack);
    static_cast<ScopeExecutor*>(exec)->PushDeferStmt(t);
  }
}

void DeferExecutor::set_stop(StopFlag flag) {
  parent()->set_stop(flag);
}

void CmdDeclExecutor::Exec(AstNode* node) {
  CmdDeclaration* cmd_decl = static_cast<CmdDeclaration*>(node);

  CmdEntryPtr cmd_ptr(new CmdDeclEntry(cmd_decl->block(),
                                       symbol_table_stack()));

  std::string id = cmd_decl->id()->name();

  try {
    symbol_table_stack().SetCmd(id, cmd_ptr);
  } catch (RunTimeError& e) {
    throw RunTimeError(e.err_code(), e.msg(), node->pos(), e.messages());
  }
}

void CmdDeclExecutor::set_stop(StopFlag flag) {
  if (parent() == nullptr) {
    return;
  }

  parent()->set_stop(flag);
}

void ImportExecutor::Exec(ImportStatement *node) {
  if (node->is_import_path()) {
    // module path has to has "as" parameter
    if (!node->has_as()) {
      throw RunTimeError(RunTimeError::ErrorCode::IMPORT,
                         boost::format("import has not a name given by 'as'"),
                         node->pos());
    }

    auto value = node->import<Literal>()->value();
    std::string module_path = boost::get<std::string>(value);

    ObjectPtr obj_module;

    try {
      ObjectPtr path_obj = symbol_table_stack().Lookup("__path__", false)
          .SharedAccess();
      SHPP_FUNC_CHECK_PARAM_TYPE(path_obj, import, STRING)
      std::string path_str = static_cast<StringObject&>(*path_obj).value();
      obj_module = ProcessModule(module_path, path_str);
    } catch (RunTimeError& e) {
      throw RunTimeError(e.err_code(), e.msg(), node->pos(), e.messages());
    }

    // module entry on symbol table
    std::string id_entry = node->as()->name();

    try {
      symbol_table_stack().SetEntry(id_entry, obj_module);
    } catch (RunTimeError& e) {
      throw RunTimeError(e.err_code(), e.msg(), node->pos(), e.messages());
    }
  }
}

ObjectPtr ImportExecutor::ProcessModule(const std::string& module,
    const std::string& path) {
  std::string full_path = path + std::string("/") + module;

  ObjectPtr module_obj = EnvShell::instance()->GetImportTable()
      .GetModule(full_path);

  // check if module was already processed
  if (module_obj) {
    return module_obj;
  }

  // process the module and store it on import table
  ObjectFactory obj_factory(symbol_table_stack());
  module_obj = obj_factory.NewModule(full_path);

  EnvShell::instance()->GetImportTable().AddModule(full_path, module_obj);
  static_cast<ModuleImportObject&>(*module_obj).Execute();

  return module_obj;
}

void ImportExecutor::set_stop(StopFlag flag) {
  if (parent() == nullptr) {
    return;
  }

  parent()->set_stop(flag);
}

void AliasDeclExecutor::Exec(AliasDeclaration *node) {
  std::string alias_name = node->name()->name();
  SimpleCmdExecutor cmd_exec(this, symbol_table_stack());
  std::vector<std::string> cmd_pieces = cmd_exec.Exec(node->cmd());

  symbol_table_stack().SetCmdAlias(alias_name, std::move(cmd_pieces));
}

void DelStmtExecutor::Exec(DelStatement *node) {
  ExpressionList* expr_list_node = node->exp_list();

  std::vector<Expression*> expr_vec = expr_list_node->children();
  for (AstNode* value: expr_vec) {
    Del(static_cast<Expression*>(value));
  }
}

void DelStmtExecutor::Del(Expression* node) {
  switch (node->type()) {
    case AstNode::NodeType::kIdentifier:
      DelId(static_cast<Identifier*>(node));
      break;

    case AstNode::NodeType::kArray:
      DelArray(static_cast<Array*>(node));
      break;

    default:
      throw RunTimeError(RunTimeError::ErrorCode::INCOMPATIBLE_TYPE,
                         boost::format("expression not valid for del"),
                         node->pos());
  }
}

void DelStmtExecutor::DelId(Identifier* id_node) {
  // remove the entry of variable on symbol table,
  // it doesn't remove the object from memotry
  // if the counter of shared pointer is larger than
  // one, the object keep in the memory
  const std::string& name = id_node->name();

  if (!symbol_table_stack().Remove(name)) {
    throw RunTimeError(RunTimeError::ErrorCode::ID_NOT_FOUND,
                       boost::format("variable %1% not found")%name,
                       id_node->pos());
  }
}

void DelStmtExecutor::DelArray(Array* array_node) {
  Expression* arr_exp = array_node->arr_exp();

  ExpressionExecutor expr(this, symbol_table_stack());
  ObjectPtr array_obj = expr.Exec(arr_exp);

  // Executes index expression of array
  ObjectPtr index = expr.Exec(array_node->index_exp());

  array_obj->DelItem(index);
}

void VarEnvExecutor::Exec(VarEnvStatement *node) {
  const std::string& var = node->var()->name();

  ExpressionExecutor expr(this, symbol_table_stack());

  try {
    ObjectPtr obj_exp = expr.Exec(node->exp());

    std::string value;

    // check if the object is string or has string object interface
    if (obj_exp->type() == Object::ObjectType::STRING) {
      value = static_cast<StringObject&>(*obj_exp).value();
    } else {
      ObjectPtr str_obj = obj_exp->ObjString();

      if (str_obj->type() != Object::ObjectType::STRING) {
        throw RunTimeError(RunTimeError::ErrorCode::ID_NOT_FOUND,
            boost::format("cast for string not valid"), node->exp()->pos());
      }

      value = static_cast<StringObject&>(*str_obj).value();
    }

    int r = setenv(var.c_str(), value.c_str(), 1);

    // if there is an error, throw an error exception
    if (r != 0) {
      throw RunTimeError(RunTimeError::ErrorCode::ID_NOT_FOUND,
          boost::format("fail on set varenv: '%1%'")%var,
          node->exp()->pos());
    }
  } catch (RunTimeError& e) {
    throw RunTimeError(e.err_code(), e.msg(), node->exp()->pos(),
        e.messages());
  }
}

void GlobalAssignmentExecutor::Exec(GlobalAssignmentStatement *node) {
  // check if we are on the main scope
  if (parent()->inside_root_scope()) {
    // assign as global variables
    AssignExecutor exec(true, this, symbol_table_stack());
    exec.Exec(node->assign());
  } else {
    throw RunTimeError(RunTimeError::ErrorCode::SYMBOL_DEF,
          boost::format("global must be defined only on main scope"),
          node->assign()->pos());
  }
}

}
}
