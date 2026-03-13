// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/parser/cpdf_indirect_object_holder.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_read_validator.h"
#include "core/fxcrt/check.h"

namespace {

const CPDF_Object* FilterInvalidObjNum(const CPDF_Object* obj) {
  return obj && obj->GetObjNum() != CPDF_Object::kInvalidObjNum ? obj : nullptr;
}

class ScopedIndirectObjectEmptyEntryEraser {
 public:
  FX_STACK_ALLOCATED();
  ScopedIndirectObjectEmptyEntryEraser(
      std::map<uint32_t, RetainPtr<CPDF_Object>>* map,
      std::map<uint32_t, RetainPtr<CPDF_Object>>::iterator it)
      : map_(map), it_(it) {}
  ~ScopedIndirectObjectEmptyEntryEraser() {
    if (map_) {
      map_->erase(it_);
    }
  }
  void Success() { map_ = nullptr; }

 private:
  UnownedPtr<std::map<uint32_t, RetainPtr<CPDF_Object>>> map_;
  std::map<uint32_t, RetainPtr<CPDF_Object>>::iterator it_;
};

}  // namespace

CPDF_IndirectObjectHolder::CPDF_IndirectObjectHolder()
    : byte_string_pool_(std::make_unique<ByteStringPool>()) {}

CPDF_IndirectObjectHolder::~CPDF_IndirectObjectHolder() {
  byte_string_pool_.DeleteObject();  // Make weak.
}

RetainPtr<CPDF_ReadValidator> CPDF_IndirectObjectHolder::GetValidator() const {
  return nullptr;
}

RetainPtr<const CPDF_Object> CPDF_IndirectObjectHolder::GetIndirectObject(
    uint32_t objnum) const {
  return pdfium::WrapRetain(GetIndirectObjectInternal(objnum));
}

RetainPtr<CPDF_Object> CPDF_IndirectObjectHolder::GetMutableIndirectObject(
    uint32_t objnum) {
  return pdfium::WrapRetain(
      const_cast<CPDF_Object*>(GetIndirectObjectInternal(objnum)));
}

const CPDF_Object* CPDF_IndirectObjectHolder::GetIndirectObjectInternal(
    uint32_t objnum) const {
  auto it = indirect_objs_.find(objnum);
  if (it == indirect_objs_.end()) {
    return nullptr;
  }

  return FilterInvalidObjNum(it->second.Get());
}

RetainPtr<CPDF_Object> CPDF_IndirectObjectHolder::GetOrParseIndirectObject(
    uint32_t objnum) {
  return pdfium::WrapRetain(GetOrParseIndirectObjectInternal(objnum));
}

CPDF_Object* CPDF_IndirectObjectHolder::GetOrParseIndirectObjectInternal(
    uint32_t objnum) {
  if (objnum == 0 || objnum == CPDF_Object::kInvalidObjNum) {
    return nullptr;
  }

  // Add item anyway to prevent recursively parsing of same object.
  auto insert_result = indirect_objs_.insert(std::make_pair(objnum, nullptr));
  if (!insert_result.second) {
    return const_cast<CPDF_Object*>(
        FilterInvalidObjNum(insert_result.first->second.Get()));
  }

  ScopedIndirectObjectEmptyEntryEraser eraser_guard(&indirect_objs_,
                                                     insert_result.first);
  RetainPtr<CPDF_Object> pNewObj;
  RetainPtr<CPDF_ReadValidator> validator = GetValidator();
  {
    std::optional<CPDF_ReadValidator::ScopedSession> session;
    if (validator) {
      session.emplace(validator);
    }
    pNewObj = ParseIndirectObject(objnum);
    bool bReadProblem = validator && validator->has_read_problems();
    if (bReadProblem || !pNewObj) {
      if (!pNewObj) {
        return nullptr;
      }
      pNewObj->SetObjNum(objnum);
      // Best effort: Return the object for immediate use in the current operation,
      // but do NOT call eraser_guard.Success(). This ensures that the (potentially
      // tainted) object is not committed to the long-term cache, allowing
      // recovery once underlying I/O issues (e.g., file locks) are resolved.
      return pNewObj.Get();
    }
  }

  eraser_guard.Success();
  pNewObj->SetObjNum(objnum);
  last_obj_num_ = std::max(last_obj_num_, objnum);

  CPDF_Object* result = pNewObj.Get();
  insert_result.first->second = std::move(pNewObj);
  return result;
}

RetainPtr<CPDF_Object> CPDF_IndirectObjectHolder::ParseIndirectObject(
    uint32_t objnum) {
  return nullptr;
}

uint32_t CPDF_IndirectObjectHolder::AddIndirectObject(
    RetainPtr<CPDF_Object> pObj) {
  CHECK(!pObj->GetObjNum());
  pObj->SetObjNum(++last_obj_num_);
  pObj->SetDirty(true);
  indirect_objs_[last_obj_num_] = std::move(pObj);
  return last_obj_num_;
}

bool CPDF_IndirectObjectHolder::ReplaceIndirectObjectIfHigherGeneration(
    uint32_t objnum,
    RetainPtr<CPDF_Object> pObj) {
  DCHECK(objnum);
  if (!pObj || objnum == CPDF_Object::kInvalidObjNum) {
    return false;
  }

  auto& obj_holder = indirect_objs_[objnum];
  const CPDF_Object* old_object = FilterInvalidObjNum(obj_holder.Get());
  if (old_object && pObj->GetGenNum() <= old_object->GetGenNum()) {
    return false;
  }

  pObj->SetObjNum(objnum);
  obj_holder = std::move(pObj);
  last_obj_num_ = std::max(last_obj_num_, objnum);
  return true;
}

void CPDF_IndirectObjectHolder::DeleteIndirectObject(uint32_t objnum) {
  auto it = indirect_objs_.find(objnum);
  if (it == indirect_objs_.end() || !FilterInvalidObjNum(it->second.Get())) {
    return;
  }

  indirect_objs_.erase(it);
}
