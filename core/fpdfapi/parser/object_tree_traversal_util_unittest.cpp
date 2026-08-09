// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fpdfapi/parser/object_tree_traversal_util.h"

#include <set>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_test_document.h"
#include "core/fxcrt/retain_ptr.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

TEST(ObjectTreeTraversalUtilTest, CountsIndirectAndInlineAliases) {
  CPDF_TestDocument document;
  RetainPtr<CPDF_Dictionary> root = document.NewIndirect<CPDF_Dictionary>();
  document.SetRoot(root);

  RetainPtr<CPDF_Dictionary> shared =
      document.NewIndirect<CPDF_Dictionary>();
  RetainPtr<CPDF_Dictionary> indirect_owner =
      document.NewIndirect<CPDF_Dictionary>();
  RetainPtr<CPDF_Dictionary> second_owner =
      document.NewIndirect<CPDF_Dictionary>();

  indirect_owner->SetNewFor<CPDF_Reference>("Shared", &document,
                                             shared->GetObjNum());
  second_owner->SetNewFor<CPDF_Reference>("Shared", &document,
                                          shared->GetObjNum());
  root->SetNewFor<CPDF_Reference>("Indirect", &document,
                                  indirect_owner->GetObjNum());
  root->SetNewFor<CPDF_Reference>("Second", &document,
                                  second_owner->GetObjNum());

  // This reference is nested in an inline dictionary. It must retain the
  // indirect owner as its top-level reference owner.
  RetainPtr<CPDF_Dictionary> inline_child =
      indirect_owner->SetNewFor<CPDF_Dictionary>("Inline");
  inline_child->SetNewFor<CPDF_Reference>("Shared", &document,
                                          shared->GetObjNum());

  EXPECT_EQ(GetObjectsWithMultipleReferences(&document),
            std::set<uint32_t>{shared->GetObjNum()});
}

TEST(ObjectTreeTraversalUtilTest, IgnoresSelfAndCircularReferences) {
  CPDF_TestDocument document;
  RetainPtr<CPDF_Dictionary> root = document.NewIndirect<CPDF_Dictionary>();
  document.SetRoot(root);

  RetainPtr<CPDF_Dictionary> self = document.NewIndirect<CPDF_Dictionary>();
  self->SetNewFor<CPDF_Reference>("Self", &document, self->GetObjNum());
  root->SetNewFor<CPDF_Reference>("Self", &document, self->GetObjNum());

  RetainPtr<CPDF_Dictionary> first =
      document.NewIndirect<CPDF_Dictionary>();
  RetainPtr<CPDF_Dictionary> second =
      document.NewIndirect<CPDF_Dictionary>();
  RetainPtr<CPDF_Dictionary> terminal =
      document.NewIndirect<CPDF_Dictionary>();
  root->SetNewFor<CPDF_Reference>("Cycle", &document, first->GetObjNum());
  first->SetNewFor<CPDF_Reference>("Next", &document, second->GetObjNum());
  // The first edge makes `second` a seen reference owner. The second edge is
  // then suppressed because both sides of the cycle have already acted as
  // reference owners, matching the existing encounter-order rule.
  second->SetNewFor<CPDF_Reference>("Anchor", &document,
                                    terminal->GetObjNum());
  second->SetNewFor<CPDF_Reference>("Cycle", &document, first->GetObjNum());

  EXPECT_TRUE(GetObjectsWithMultipleReferences(&document).empty());
}

}  // namespace
