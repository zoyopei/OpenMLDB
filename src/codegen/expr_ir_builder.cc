/*
 * expr_ir_builder.cc
 * Copyright (C) 4paradigm.com 2019 wangtaize <wangtaize@4paradigm.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "codegen/expr_ir_builder.h"
#include <string>
#include <utility>
#include <vector>
#include "codegen/buf_ir_builder.h"
#include "codegen/cond_select_ir_builder.h"
#include "codegen/context.h"
#include "codegen/date_ir_builder.h"
#include "codegen/fn_ir_builder.h"
#include "codegen/ir_base_builder.h"
#include "codegen/list_ir_builder.h"
#include "codegen/struct_ir_builder.h"
#include "codegen/timestamp_ir_builder.h"
#include "codegen/type_ir_builder.h"
#include "codegen/udf_ir_builder.h"
#include "codegen/window_ir_builder.h"
#include "glog/logging.h"
#include "proto/fe_common.pb.h"
#include "udf/default_udf_library.h"
#include "vm/schemas_context.h"

namespace fesql {
namespace codegen {

ExprIRBuilder::ExprIRBuilder(CodeGenContext* ctx) : ctx_(ctx) {}

ExprIRBuilder::~ExprIRBuilder() {}

// TODO(chenjing): 修改GetFunction, 直接根据参数生成signature
::llvm::Function* ExprIRBuilder::GetFuncion(
    const std::string& name,
    const std::vector<const node::TypeNode*>& args_types,
    base::Status& status) {
    std::string fn_name = name;
    if (!args_types.empty()) {
        for (const node::TypeNode* type_node : args_types) {
            fn_name.append(".").append(type_node->GetName());
        }
    }

    auto module = ctx_->GetModule();
    ::llvm::Function* fn = module->getFunction(::llvm::StringRef(fn_name));
    if (nullptr != fn) {
        return fn;
    }

    if (!args_types.empty() && !args_types[0]->generics_.empty()) {
        switch (args_types[0]->generics_[0]->base_) {
            case node::kTimestamp:
            case node::kVarchar:
            case node::kDate: {
                fn_name.append(".").append(
                    args_types[0]->generics_[0]->GetName());
                fn = module->getFunction(::llvm::StringRef(fn_name));
                break;
            }
            default: {
            }
        }
    }
    if (nullptr == fn) {
        status.code = common::kCallMethodError;
        status.msg = "fail to find func with name " + fn_name;
        LOG(WARNING) << status;
        return fn;
    }
    return fn;
}

Status ExprIRBuilder::Build(const ::fesql::node::ExprNode* node,
                            NativeValue* output) {
    CHECK_TRUE(node != nullptr && output != nullptr, kCodegenError,
               "Node or output is null");
    if (node->GetOutputType() == nullptr) {
        LOG(WARNING) << node->GetExprString() << " not fully resolved";
    }
    switch (node->GetExprType()) {
        case ::fesql::node::kExprColumnRef: {
            const ::fesql::node::ColumnRefNode* n =
                (const ::fesql::node::ColumnRefNode*)node;
            return BuildColumnRef(n, output);
        }
        case ::fesql::node::kExprCall: {
            const ::fesql::node::CallExprNode* fn =
                (const ::fesql::node::CallExprNode*)node;
            return BuildCallFn(fn, output);
        }
        case ::fesql::node::kExprPrimary: {
            auto const_node =
                dynamic_cast<const ::fesql::node::ConstNode*>(node);
            return BuildConstExpr(const_node, output);
        }
        case ::fesql::node::kExprId: {
            ::fesql::node::ExprIdNode* id_node =
                (::fesql::node::ExprIdNode*)node;
            DLOG(INFO) << "id node spec " << id_node->GetExprString();
            CHECK_TRUE(id_node->IsResolved(), kCodegenError,
                       "Detect unresolved expr id: " + id_node->GetName());
            NativeValue val;
            VariableIRBuilder variable_ir_builder(
                ctx_->GetCurrentBlock(), ctx_->GetCurrentScope()->sv());
            Status status;
            CHECK_TRUE(variable_ir_builder.LoadValue(id_node->GetExprString(),
                                                     &val, status),
                       kCodegenError, "Fail to find var ",
                       id_node->GetExprString());
            *output = val;
            return Status::OK();
        }
        case ::fesql::node::kExprCast: {
            return BuildCastExpr((::fesql::node::CastExprNode*)node, output);
        }
        case ::fesql::node::kExprBinary: {
            return BuildBinaryExpr((::fesql::node::BinaryExpr*)node, output);
        }
        case ::fesql::node::kExprUnary: {
            return BuildUnaryExpr(
                dynamic_cast<const ::fesql::node::UnaryExpr*>(node), output);
        }
        case ::fesql::node::kExprStruct: {
            return BuildStructExpr((fesql::node::StructExpr*)node, output);
        }
        case ::fesql::node::kExprGetField: {
            return BuildGetFieldExpr(
                dynamic_cast<const ::fesql::node::GetFieldExpr*>(node), output);
        }
        case ::fesql::node::kExprCond: {
            return BuildCondExpr(
                dynamic_cast<const ::fesql::node::CondExpr*>(node), output);
        }
        case ::fesql::node::kExprCase: {
            return BuildCaseExpr(
                dynamic_cast<const ::fesql::node::CaseWhenExprNode*>(node),
                output);
        }
        default: {
            return Status(kCodegenError,
                          "Expression Type " +
                              node::ExprTypeName(node->GetExprType()) +
                              " not supported");
        }
    }
}

Status ExprIRBuilder::BuildConstExpr(const ::fesql::node::ConstNode* const_node,
                                     NativeValue* output) {
    ::llvm::IRBuilder<> builder(ctx_->GetCurrentBlock());
    switch (const_node->GetDataType()) {
        case ::fesql::node::kNull: {
            *output = NativeValue::CreateNull(
                llvm::Type::getTokenTy(builder.getContext()));
            break;
        }
        case ::fesql::node::kInt16: {
            *output = NativeValue::Create(
                builder.getInt16(const_node->GetSmallInt()));
            break;
        }
        case ::fesql::node::kInt32: {
            *output =
                NativeValue::Create(builder.getInt32(const_node->GetInt()));
            break;
        }
        case ::fesql::node::kInt64: {
            *output =
                NativeValue::Create(builder.getInt64(const_node->GetLong()));
            break;
        }
        case ::fesql::node::kFloat: {
            llvm::Value* raw_float = nullptr;
            CHECK_TRUE(GetConstFloat(ctx_->GetLLVMContext(),
                                     const_node->GetFloat(), &raw_float),
                       kCodegenError);

            *output = NativeValue::Create(raw_float);
            break;
        }
        case ::fesql::node::kDouble: {
            llvm::Value* raw_double = nullptr;
            CHECK_TRUE(GetConstDouble(ctx_->GetLLVMContext(),
                                      const_node->GetDouble(), &raw_double),
                       kCodegenError);

            *output = NativeValue::Create(raw_double);
            break;
        }
        case ::fesql::node::kVarchar: {
            std::string val(const_node->GetStr(), strlen(const_node->GetStr()));
            llvm::Value* raw_str = nullptr;
            CHECK_TRUE(GetConstFeString(val, ctx_->GetCurrentBlock(), &raw_str),
                       kCodegenError);
            *output = NativeValue::Create(raw_str);
            break;
        }
        case ::fesql::node::kDate: {
            auto date_int = builder.getInt32(const_node->GetLong());
            DateIRBuilder date_builder(ctx_->GetModule());
            ::llvm::Value* date = nullptr;
            CHECK_TRUE(
                date_builder.NewDate(ctx_->GetCurrentBlock(), date_int, &date),
                kCodegenError);
            *output = NativeValue::Create(date);
            break;
        }
        case ::fesql::node::kTimestamp: {
            auto ts_int = builder.getInt64(const_node->GetLong());
            TimestampIRBuilder date_builder(ctx_->GetModule());
            ::llvm::Value* ts = nullptr;
            CHECK_TRUE(
                date_builder.NewTimestamp(ctx_->GetCurrentBlock(), ts_int, &ts),
                kCodegenError);
            *output = NativeValue::Create(ts);
            break;
        }
        default: {
            return Status(kCodegenError,
                          "Fail to codegen primary expression for type: " +
                              node::DataTypeName(const_node->GetDataType()));
        }
    }
    return Status::OK();
}

Status ExprIRBuilder::BuildCallFn(const ::fesql::node::CallExprNode* call,
                                  NativeValue* output) {
    const node::FnDefNode* fn_def = call->GetFnDef();
    if (fn_def->GetType() == node::kExternalFnDef) {
        auto extern_fn = dynamic_cast<const node::ExternalFnDefNode*>(fn_def);
        if (!extern_fn->IsResolved()) {
            Status status;
            BuildCallFnLegacy(call, output, status);
            return status;
        }
    }

    std::vector<NativeValue> arg_values;
    std::vector<const node::TypeNode*> arg_types;

    // TODO(xxx): remove this
    bool is_udaf = false;
    if (call->GetChildNum() > 0) {
        auto first_node_type = call->GetChild(0)->GetOutputType();
        if (first_node_type != nullptr &&
            first_node_type->base_ == node::kList) {
            is_udaf = true;
        }
    }
    ExprIRBuilder sub_builder(ctx_);
    sub_builder.set_frame(this->frame_arg_, this->frame_);
    for (size_t i = 0; i < call->GetChildNum(); ++i) {
        node::ExprNode* arg_expr = call->GetChild(i);
        NativeValue arg_value;
        CHECK_STATUS(sub_builder.Build(arg_expr, &arg_value), "Build argument ",
                     arg_expr->GetExprString(), " failed");
        arg_values.push_back(arg_value);
        arg_types.push_back(arg_expr->GetOutputType());
    }
    UDFIRBuilder udf_builder(ctx_, frame_arg_, frame_);
    return udf_builder.BuildCall(fn_def, arg_types, arg_values, output);
}

bool ExprIRBuilder::BuildCallFnLegacy(
    const ::fesql::node::CallExprNode* call_fn, NativeValue* output,
    ::fesql::base::Status& status) {  // NOLINT

    // TODO(chenjing): return status;
    if (call_fn == NULL || output == NULL) {
        status.code = common::kNullPointer;
        status.msg = "call fn or output is null";
        LOG(WARNING) << status.msg;
        return false;
    }
    auto named_fn =
        dynamic_cast<const node::ExternalFnDefNode*>(call_fn->GetFnDef());
    std::string function_name = named_fn->function_name();

    ::llvm::BasicBlock* block = ctx_->GetCurrentBlock();
    ::llvm::IRBuilder<> builder(block);
    ::llvm::StringRef name(function_name);

    std::vector<::llvm::Value*> llvm_args;
    std::vector<::fesql::node::ExprNode*>::const_iterator it =
        call_fn->children_.cbegin();
    std::vector<const ::fesql::node::TypeNode*> generics_types;
    std::vector<const ::fesql::node::TypeNode*> args_types;

    for (; it != call_fn->children_.cend(); ++it) {
        const ::fesql::node::ExprNode* arg = dynamic_cast<node::ExprNode*>(*it);
        NativeValue llvm_arg_wrapper;
        // TODO(chenjing): remove out_name
        status = Build(arg, &llvm_arg_wrapper);
        if (status.isOK()) {
            const ::fesql::node::TypeNode* value_type = nullptr;
            if (false == GetFullType(ctx_->node_manager(),
                                     llvm_arg_wrapper.GetType(), &value_type)) {
                status.msg = "fail to handle arg type ";
                status.code = common::kCodegenError;
                return false;
            }
            args_types.push_back(value_type);
            // TODO(chenjing): 直接使用list TypeNode
            // handle list type
            // 泛型类型还需要优化，目前是hard
            // code识别list或者迭代器类型，然后取generic type
            if (fesql::node::kList == value_type->base() ||
                fesql::node::kIterator == value_type->base()) {
                generics_types.push_back(value_type);
            }
            ::llvm::Value* llvm_arg = llvm_arg_wrapper.GetValue(&builder);
            llvm_args.push_back(llvm_arg);
        } else {
            LOG(WARNING) << "fail to build arg for " << status.msg;
            std::ostringstream oss;
            oss << "faild to build args: " << *call_fn;
            status.msg = oss.str();
            status.code = common::kCodegenError;
            return false;
        }
    }

    ::llvm::Function* fn = GetFuncion(function_name, args_types, status);

    if (common::kOk != status.code) {
        return false;
    }

    if (call_fn->children_.size() == fn->arg_size()) {
        ::llvm::ArrayRef<::llvm::Value*> array_ref(llvm_args);
        *output = NativeValue::Create(
            builder.CreateCall(fn->getFunctionType(), fn, array_ref));
        return true;
    } else if (call_fn->children_.size() == fn->arg_size() - 1) {
        auto it = fn->arg_end();
        it--;
        ::llvm::Argument* last_arg = &*it;
        if (!TypeIRBuilder::IsStructPtr(last_arg->getType())) {
            status.msg = ("Incorrect arguments passed");
            status.code = (common::kCallMethodError);
            LOG(WARNING) << status.msg;
            return false;
        }
        ::llvm::Type* struct_type =
            reinterpret_cast<::llvm::PointerType*>(last_arg->getType())
                ->getElementType();
        ::llvm::Value* struct_value = builder.CreateAlloca(struct_type);
        llvm_args.push_back(struct_value);
        ::llvm::Value* ret =
            builder.CreateCall(fn->getFunctionType(), fn,
                               ::llvm::ArrayRef<::llvm::Value*>(llvm_args));
        if (nullptr == ret) {
            status.code = common::kCallMethodError;
            status.msg = "Fail to Call Function";
            LOG(WARNING) << status.msg;
            return false;
        }
        *output = NativeValue::Create(struct_value);
        return true;

    } else {
        status.msg = ("Incorrect arguments passed");
        status.code = (common::kCallMethodError);
        LOG(WARNING) << status.msg;
        return false;
    }
    return false;
}

// Build Struct Expr IR:
// TODO(chenjing): support method memeber
// @param node
// @param output
// @return
Status ExprIRBuilder::BuildStructExpr(const ::fesql::node::StructExpr* node,
                                      NativeValue* output) {
    std::vector<::llvm::Type*> members;
    if (nullptr != node->GetFileds() && !node->GetFileds()->children.empty()) {
        for (auto each : node->GetFileds()->children) {
            node::FnParaNode* field = dynamic_cast<node::FnParaNode*>(each);
            ::llvm::Type* type = nullptr;
            CHECK_TRUE(ConvertFeSQLType2LLVMType(field->GetParaType(),
                                                 ctx_->GetModule(), &type),
                       kCodegenError,
                       "Invalid struct with unacceptable field type: " +
                           field->GetParaType()->GetName());
            members.push_back(type);
        }
    }
    ::llvm::StringRef name(node->GetName());
    ::llvm::StructType* llvm_struct =
        ::llvm::StructType::create(ctx_->GetLLVMContext(), name);
    ::llvm::ArrayRef<::llvm::Type*> array_ref(members);
    llvm_struct->setBody(array_ref);
    *output = NativeValue::Create((::llvm::Value*)llvm_struct);
    return Status::OK();
}

// Get inner window with given frame
Status ExprIRBuilder::BuildWindow(NativeValue* output) {  // NOLINT
    ::llvm::IRBuilder<> builder(ctx_->GetCurrentBlock());
    NativeValue window_ptr_value;
    const std::string frame_str =
        nullptr == frame_ ? "" : frame_->GetExprString();
    // Load Inner Window If Exist
    VariableIRBuilder variable_ir_builder(ctx_->GetCurrentBlock(),
                                          ctx_->GetCurrentScope()->sv());
    Status status;
    bool ok =
        variable_ir_builder.LoadWindow(frame_str, &window_ptr_value, status);
    ::llvm::Value* window_ptr = nullptr;
    if (!window_ptr_value.IsConstNull()) {
        window_ptr = window_ptr_value.GetValue(&builder);
    }

    if (ok && window_ptr != nullptr) {
        *output = NativeValue::Create(window_ptr);
        return Status::OK();
    }

    // Load Big Window, throw error if big window not exist
    ok = variable_ir_builder.LoadWindow("", &window_ptr_value, status);
    CHECK_TRUE(ok && nullptr != window_ptr_value.GetValue(&builder),
               kCodegenError, "Fail to find window " + status.str());

    // Build Inner Window based on Big Window and frame info
    // ListRef* { int8_t* } -> int8_t**
    ::llvm::Value* list_ref_ptr = window_ptr_value.GetValue(&builder);
    list_ref_ptr = builder.CreatePointerCast(
        list_ref_ptr, builder.getInt8PtrTy()->getPointerTo());
    ::llvm::Value* list_ptr = builder.CreateLoad(list_ref_ptr);

    MemoryWindowDecodeIRBuilder window_ir_builder(ctx_->schemas_context(),
                                                  ctx_->GetCurrentBlock());
    if (frame_->frame_range() != nullptr) {
        ok = window_ir_builder.BuildInnerRangeList(
            list_ptr, frame_->GetHistoryRangeEnd(),
            frame_->GetHistoryRangeStart(), &window_ptr);
    } else if (frame_->frame_rows() != nullptr) {
        ok = window_ir_builder.BuildInnerRowsList(
            list_ptr, -1 * frame_->GetHistoryRowsEnd(),
            -1 * frame_->GetHistoryRowsStart(), &window_ptr);
    }

    // int8_t** -> ListRef* { int8_t* }
    ::llvm::Value* inner_list_ref_ptr =
        builder.CreateAlloca(window_ptr->getType());
    builder.CreateStore(window_ptr, inner_list_ref_ptr);
    window_ptr =
        builder.CreatePointerCast(inner_list_ref_ptr, window_ptr->getType());

    CHECK_TRUE(ok && nullptr != window_ptr, kCodegenError,
               "Fail to build inner window " + frame_str);

    CHECK_TRUE(variable_ir_builder.StoreWindow(frame_str, window_ptr, status),
               kCodegenError, "Fail to store window ", frame_str, ": ",
               status.msg);
    DLOG(INFO) << "store window " << frame_str;

    *output = NativeValue::Create(window_ptr);
    return Status::OK();
}

// Get col with given col name, set list struct pointer into output
// param col
// param output
// return
Status ExprIRBuilder::BuildColumnRef(const ::fesql::node::ColumnRefNode* node,
                                     NativeValue* output) {
    const std::string relation_name = node->GetRelationName();
    const std::string col = node->GetColumnName();
    const RowSchemaInfo* info;
    CHECK_TRUE(FindRowSchemaInfo(relation_name, col, &info), kCodegenError,
               "Fail to find context with " + relation_name + "." + col);

    ::llvm::Value* value = NULL;
    const std::string frame_str =
        nullptr == frame_ ? "" : frame_->GetExprString();
    DLOG(INFO) << "get table column " << col;
    // not found
    VariableIRBuilder variable_ir_builder(ctx_->GetCurrentBlock(),
                                          ctx_->GetCurrentScope()->sv());
    Status status;
    bool ok = variable_ir_builder.LoadColumnRef(info->table_name_, col,
                                                frame_str, &value, status);
    if (ok) {
        *output = NativeValue::Create(value);
        return Status::OK();
    }

    NativeValue window;
    CHECK_STATUS(BuildWindow(&window), "Fail to build window");

    DLOG(INFO) << "get table column " << col;
    // NOT reuse for iterator
    MemoryWindowDecodeIRBuilder window_ir_builder(ctx_->schemas_context(),
                                                  ctx_->GetCurrentBlock());
    ok =
        window_ir_builder.BuildGetCol(col, window.GetRaw(), info->idx_, &value);
    CHECK_TRUE(ok && value != nullptr, kCodegenError,
               "fail to find column " + col);

    ok = variable_ir_builder.StoreColumnRef(info->table_name_, col, frame_str,
                                            value, status);
    CHECK_TRUE(ok, kCodegenError, "fail to store col for ", status.str());
    *output = NativeValue::Create(value);
    return Status::OK();
}

Status ExprIRBuilder::BuildUnaryExpr(const ::fesql::node::UnaryExpr* node,
                                     NativeValue* output) {
    CHECK_TRUE(node != nullptr && output != nullptr, kCodegenError,
               "Input node or output is null");
    CHECK_TRUE(node->GetChildNum() == 1, kCodegenError,
               "Invalid unary expr node");

    DLOG(INFO) << "build unary "
               << ::fesql::node::ExprOpTypeName(node->GetOp());
    NativeValue left;
    CHECK_STATUS(Build(node->GetChild(0), &left), "Fail to build left node");

    PredicateIRBuilder predicate_ir_builder(ctx_->GetCurrentBlock());
    ArithmeticIRBuilder arithmetic_ir_builder(ctx_->GetCurrentBlock());
    switch (node->GetOp()) {
        case ::fesql::node::kFnOpNot: {
            CHECK_STATUS(predicate_ir_builder.BuildNotExpr(left, output));
            break;
        }
        case ::fesql::node::kFnOpMinus: {
            ::llvm::IRBuilder<> builder(ctx_->GetCurrentBlock());
            CHECK_STATUS(arithmetic_ir_builder.BuildSubExpr(
                NativeValue::Create(builder.getInt16(0)), left, output));
            break;
        }
        case ::fesql::node::kFnOpBracket: {
            *output = left;
            break;
        }
        case ::fesql::node::kFnOpIsNull: {
            CHECK_STATUS(predicate_ir_builder.BuildIsNullExpr(left, output));
            break;
        }
        case ::fesql::node::kFnOpNonNull: {
            // just ignore any null flag
            *output = NativeValue::Create(left.GetValue(ctx_));
            break;
        }
        default: {
            return Status(kCodegenError,
                          "Invalid op " + ExprOpTypeName(node->GetOp()));
        }
    }

    // check llvm type and inferred type
    if (node->GetOutputType() == nullptr) {
        LOG(WARNING) << "Unary op type not inferred";
    } else {
        ::llvm::Type* expect_llvm_ty = nullptr;
        GetLLVMType(ctx_->GetModule(), node->GetOutputType(), &expect_llvm_ty);
        if (expect_llvm_ty != output->GetType()) {
            LOG(WARNING) << "Inconsistent return llvm type: "
                         << GetLLVMObjectString(output->GetType())
                         << ", expect " << GetLLVMObjectString(expect_llvm_ty);
        }
    }
    return Status::OK();
}

Status ExprIRBuilder::BuildCastExpr(const ::fesql::node::CastExprNode* node,
                                    NativeValue* output) {
    CHECK_TRUE(node != nullptr && output != nullptr, kCodegenError,
               "Input node or output is null");

    DLOG(INFO) << "build cast expr: " << node::ExprString(node);
    NativeValue left;
    CHECK_STATUS(Build(node->expr(), &left), "Fail to build left node");

    CastExprIRBuilder cast_builder(ctx_->GetCurrentBlock());
    ::llvm::Type* cast_type = NULL;
    CHECK_TRUE(GetLLVMType(ctx_->GetModule(), node->cast_type_, &cast_type),
               kCodegenError, "Fail to cast expr: dist type invalid");

    if (cast_builder.IsSafeCast(left.GetType(), cast_type)) {
        return cast_builder.SafeCast(left, cast_type, output);
    } else {
        return cast_builder.UnSafeCast(left, cast_type, output);
    }
}

Status ExprIRBuilder::BuildBinaryExpr(const ::fesql::node::BinaryExpr* node,
                                      NativeValue* output) {
    CHECK_TRUE(node != nullptr && output != nullptr, kCodegenError,
               "Input node or output is null");
    CHECK_TRUE(node->GetChildNum() == 2, kCodegenError,
               "Invalid binary expr node");

    DLOG(INFO) << "build binary "
               << ::fesql::node::ExprOpTypeName(node->GetOp());
    NativeValue left;
    CHECK_STATUS(Build(node->children_[0], &left), "Fail to build left node");

    NativeValue right;
    CHECK_STATUS(Build(node->children_[1], &right), "Fail to build right node");

    ArithmeticIRBuilder arithmetic_ir_builder(ctx_->GetCurrentBlock());
    PredicateIRBuilder predicate_ir_builder(ctx_->GetCurrentBlock());

    switch (node->GetOp()) {
        case ::fesql::node::kFnOpAdd: {
            CHECK_STATUS(
                arithmetic_ir_builder.BuildAddExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpMulti: {
            CHECK_STATUS(
                arithmetic_ir_builder.BuildMultiExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpFDiv: {
            CHECK_STATUS(
                arithmetic_ir_builder.BuildFDivExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpDiv: {
            CHECK_STATUS(
                arithmetic_ir_builder.BuildSDivExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpMinus: {
            CHECK_STATUS(
                arithmetic_ir_builder.BuildSubExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpMod: {
            CHECK_STATUS(
                arithmetic_ir_builder.BuildModExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpAnd: {
            CHECK_STATUS(
                predicate_ir_builder.BuildAndExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpOr: {
            CHECK_STATUS(predicate_ir_builder.BuildOrExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpXor: {
            CHECK_STATUS(
                predicate_ir_builder.BuildXorExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpEq: {
            CHECK_STATUS(predicate_ir_builder.BuildEqExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpNeq: {
            CHECK_STATUS(
                predicate_ir_builder.BuildNeqExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpGt: {
            CHECK_STATUS(predicate_ir_builder.BuildGtExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpGe: {
            CHECK_STATUS(predicate_ir_builder.BuildGeExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpLt: {
            CHECK_STATUS(predicate_ir_builder.BuildLtExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpLe: {
            CHECK_STATUS(predicate_ir_builder.BuildLeExpr(left, right, output));
            break;
        }
        case ::fesql::node::kFnOpAt: {
            CHECK_STATUS(BuildAsUDF(node, "at", {left, right}, output));
            break;
        }
        default: {
            return Status(kCodegenError,
                          "Invalid op " + ExprOpTypeName(node->GetOp()));
        }
    }

    // check llvm type and inferred type
    if (node->GetOutputType() == nullptr) {
        LOG(WARNING) << "Binary op type not inferred";
    } else {
        ::llvm::Type* expect_llvm_ty = nullptr;
        GetLLVMType(ctx_->GetModule(), node->GetOutputType(), &expect_llvm_ty);
        if (expect_llvm_ty != output->GetType()) {
            LOG(WARNING) << "Inconsistent return llvm type: "
                         << GetLLVMObjectString(output->GetType())
                         << ", expect " << GetLLVMObjectString(expect_llvm_ty);
        }
    }
    return Status::OK();
}

Status ExprIRBuilder::BuildAsUDF(const node::ExprNode* expr,
                                 const std::string& name,
                                 const std::vector<NativeValue>& args,
                                 NativeValue* output) {
    CHECK_TRUE(args.size() == expr->GetChildNum(), kCodegenError);
    auto library = udf::DefaultUDFLibrary::get();

    std::vector<node::ExprNode*> proxy_args;
    for (size_t i = 0; i < expr->GetChildNum(); ++i) {
        auto child = expr->GetChild(i);
        auto arg = ctx_->node_manager()->MakeExprIdNode("proxy_arg_" +
                                                        std::to_string(i));
        arg->SetOutputType(child->GetOutputType());
        arg->SetNullable(child->nullable());
        proxy_args.push_back(arg);
    }
    node::ExprNode* transformed = nullptr;
    CHECK_STATUS(library->Transform(name, proxy_args, ctx_->node_manager(),
                                    &transformed));

    node::ExprNode* target_expr = nullptr;
    passes::ResolveFnAndAttrs resolver(ctx_->node_manager(), library,
                                       *ctx_->schemas_context());
    CHECK_STATUS(resolver.VisitExpr(transformed, &target_expr));

    // Insert a transient binding scope between current scope and parent
    // Thus temporal binding of udf proxy arg can be dropped after build
    ScopeVar* cur_sv = ctx_->GetCurrentScope()->sv();
    ScopeVar proxy_sv_scope(cur_sv->parent());
    for (size_t i = 0; i < args.size(); ++i) {
        proxy_sv_scope.AddVar(proxy_args[i]->GetExprString(), args[i]);
    }
    cur_sv->SetParent(&proxy_sv_scope);

    Status status = Build(target_expr, output);

    cur_sv->SetParent(proxy_sv_scope.parent());
    return status;
}

Status ExprIRBuilder::BuildGetFieldExpr(const ::fesql::node::GetFieldExpr* node,
                                        NativeValue* output) {
    // build input
    Status status;
    NativeValue input_value;
    CHECK_STATUS(this->Build(node->GetRow(), &input_value));

    auto input_type = node->GetRow()->GetOutputType();
    if (input_type->base() == node::kTuple) {
        CHECK_TRUE(input_value.IsTuple() && input_value.GetFieldNum() ==
                                                input_type->GetGenericSize(),
                   kCodegenError, "Illegal input for kTuple, expect ",
                   input_type->GetName());
        try {
            size_t idx = std::stoi(node->GetColumnName());
            CHECK_TRUE(0 <= idx && idx < input_value.GetFieldNum(),
                       kCodegenError, "Tuple idx out of range: ", idx);
            *output = input_value.GetField(idx);
        } catch (std::invalid_argument err) {
            return Status(kCodegenError,
                          "Invalid Tuple index: " + node->GetColumnName());
        }

    } else if (input_type->base() == node::kRow) {
        auto& llvm_ctx = ctx_->GetLLVMContext();
        auto ptr_ty = llvm::Type::getInt8Ty(llvm_ctx)->getPointerTo();
        auto int64_ty = llvm::Type::getInt64Ty(llvm_ctx);
        auto int32_ty = llvm::Type::getInt32Ty(llvm_ctx);

        auto row_type = dynamic_cast<const node::RowTypeNode*>(
            node->GetRow()->GetOutputType());
        vm::SchemasContext schemas_context(row_type->schema_source());

        const vm::RowSchemaInfo* schema_info = nullptr;
        bool ok = schemas_context.ColumnRefResolved(
            node->GetRelationName(), node->GetColumnName(), &schema_info);
        CHECK_TRUE(ok, kCodegenError, "Fail to resolve column ",
                   node->GetExprString(), row_type);
        auto row_ptr = input_value.GetValue(ctx_);

        ::llvm::Module* module = ctx_->GetModule();
        ::llvm::IRBuilder<> builder(ctx_->GetCurrentBlock());
        auto slice_idx = builder.getInt64(schema_info->idx_);
        auto get_slice_func = module->getOrInsertFunction(
            "fesql_storage_get_row_slice",
            ::llvm::FunctionType::get(ptr_ty, {ptr_ty, int64_ty}, false));
        auto get_slice_size_func = module->getOrInsertFunction(
            "fesql_storage_get_row_slice_size",
            ::llvm::FunctionType::get(int64_ty, {ptr_ty, int64_ty}, false));
        ::llvm::Value* slice_ptr =
            builder.CreateCall(get_slice_func, {row_ptr, slice_idx});
        ::llvm::Value* slice_size = builder.CreateIntCast(
            builder.CreateCall(get_slice_size_func, {row_ptr, slice_idx}),
            int32_ty, false);

        BufNativeIRBuilder buf_builder(schema_info->schema_,
                                       ctx_->GetCurrentBlock(),
                                       ctx_->GetCurrentScope()->sv());
        CHECK_TRUE(buf_builder.BuildGetField(node->GetColumnName(), slice_ptr,
                                             slice_size, output),
                   kCodegenError);
    } else {
        return Status(common::kCodegenError,
                      "Get field's input is neither tuple nor row");
    }
    return Status::OK();
}

Status ExprIRBuilder::BuildCaseExpr(const ::fesql::node::CaseWhenExprNode* node,
                                    NativeValue* output) {
    CHECK_TRUE(nullptr != node && nullptr != node->when_expr_list() &&
                   node->when_expr_list()->GetChildNum() > 0,
               kCodegenError);
    node::NodeManager* nm = ctx_->node_manager();
    node::ExprNode* expr =
        nullptr == node->else_expr() ? nm->MakeConstNode() : node->else_expr();
    for (auto iter = node->when_expr_list()->children_.rbegin();
         iter != node->when_expr_list()->children_.rend(); iter++) {
        auto when_expr = dynamic_cast<::fesql::node::WhenExprNode*>(*iter);
        expr = nm->MakeCondExpr(when_expr->when_expr(), when_expr->then_expr(),
                                expr);
    }
    return BuildCondExpr(dynamic_cast<::fesql::node::CondExpr*>(expr), output);
}

Status ExprIRBuilder::BuildCondExpr(const ::fesql::node::CondExpr* node,
                                    NativeValue* output) {
    // build condition
    ::llvm::BasicBlock* block = ctx_->GetCurrentBlock();
    ::llvm::IRBuilder<> builder(block);
    Status status;
    NativeValue cond_value;
    CHECK_STATUS(this->Build(node->GetCondition(), &cond_value));

    // build left
    NativeValue left_value;
    CHECK_STATUS(this->Build(node->GetLeft(), &left_value));

    // build right
    NativeValue right_value;
    CHECK_STATUS(this->Build(node->GetRight(), &right_value));

    CondSelectIRBuilder cond_select_builder;
    return cond_select_builder.Select(block, cond_value, left_value,
                                      right_value, output);
}

bool ExprIRBuilder::FindRowSchemaInfo(const std::string& relation_name,
                                      const std::string& col_name,
                                      const RowSchemaInfo** info) {
    if (nullptr == ctx_->schemas_context()) {
        LOG(WARNING)
            << "fail to find row schema info with null schemas context";
        return false;
    }
    return ctx_->schemas_context()->ColumnRefResolved(relation_name, col_name,
                                                      info);
}

}  // namespace codegen
}  // namespace fesql
