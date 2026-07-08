// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FPDFAPI_EDIT_CPDF_PAGE_RESOURCE_EDITOR_H_
#define CORE_FPDFAPI_EDIT_CPDF_PAGE_RESOURCE_EDITOR_H_

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"

class CPDF_Dictionary;
class CPDF_Document;
class CPDF_PageObjectHolder;

// Append-only resource editing helpers for callers that must add new content
// streams without regenerating existing page content streams.
class CPDF_PageResourceEditor {
 public:
  static bool IsPageResourceShared(
      CPDF_Document* doc,
      RetainPtr<const CPDF_Dictionary> page_dict,
      RetainPtr<const CPDF_Dictionary> resources_dict);

  static RetainPtr<CPDF_Dictionary> EnsurePageLocalResources(
      CPDF_Document* doc,
      CPDF_PageObjectHolder* page);

  static RetainPtr<CPDF_Dictionary> EnsureLocalResourceSubdict(
      CPDF_Document* doc,
      RetainPtr<CPDF_Dictionary> resources,
      ByteStringView key);

  static ByteString AllocateUniqueResourceName(
      RetainPtr<CPDF_Dictionary> resource_dict,
      ByteStringView prefix);
};

#endif  // CORE_FPDFAPI_EDIT_CPDF_PAGE_RESOURCE_EDITOR_H_
