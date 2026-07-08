// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fpdfapi/edit/cpdf_page_resource_editor.h"

#include <set>
#include <utility>

#include "constants/page_object.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/object_tree_traversal_util.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/containers/contains.h"

namespace {

RetainPtr<CPDF_Dictionary> GetMutableDirectDict(
    RetainPtr<CPDF_Object> object) {
  if (!object) {
    return nullptr;
  }
  return ToDictionary(object->GetMutableDirect());
}

RetainPtr<CPDF_Dictionary> CloneDictionaryIndirect(
    CPDF_Document* doc,
    RetainPtr<const CPDF_Dictionary> dict) {
  if (!dict) {
    return nullptr;
  }

  RetainPtr<CPDF_Dictionary> cloned = ToDictionary(dict->Clone());
  if (!cloned) {
    return nullptr;
  }

  doc->AddIndirectObject(cloned);
  return cloned;
}

}  // namespace

// static
bool CPDF_PageResourceEditor::IsPageResourceShared(
    CPDF_Document* doc,
    RetainPtr<const CPDF_Dictionary> page_dict,
    RetainPtr<const CPDF_Dictionary> resources_dict) {
  CHECK(doc);
  CHECK(page_dict);
  CHECK(resources_dict);

  const uint32_t resources_object_number = resources_dict->GetObjNum();
  if (resources_object_number) {
    // If `resources_dict` is not an inline object, then check to see if is a
    // shared object.
    if (pdfium::Contains(GetObjectsWithMultipleReferences(doc),
                         resources_object_number)) {
      return true;
    }
  }

  // The check above may not catch all cases. e.g. inline objects. Check all
  // pages in the document to see if another page is using the same resources.
  int page_dict_seen = 0;
  int resources_dict_seen = 0;
  for (int i = 0; i < doc->GetPageCount(); ++i) {
    RetainPtr<CPDF_Dictionary> current_page_dict =
        doc->GetMutablePageDictionary(i);
    if (!current_page_dict) {
      continue;
    }

    // Check to see if the current page's page dictionary is seen twice: Once
    // for this page, and once for another. If the same page dictionary is in
    // use twice, then the resource dictionary within must also be shared.
    if (current_page_dict == page_dict) {
      ++page_dict_seen;
      if (page_dict_seen == 2) {
        return true;
      }
    }

    // Similar check as above, for the current page's resource dictionary.
    auto current_page =
        pdfium::MakeRetain<CPDF_Page>(doc, std::move(current_page_dict));
    if (resources_dict == current_page->GetMutableResources()) {
      ++resources_dict_seen;
      if (resources_dict_seen == 2) {
        return true;
      }
    }
  }

  return false;
}

// static
RetainPtr<CPDF_Dictionary> CPDF_PageResourceEditor::EnsurePageLocalResources(
    CPDF_Document* doc,
    CPDF_PageObjectHolder* page) {
  RetainPtr<CPDF_Dictionary> page_dict = page->GetMutableDict();
  if (!page_dict) {
    return nullptr;
  }

  RetainPtr<CPDF_Object> direct_resources_object =
      page_dict->GetMutableObjectFor(pdfium::page_object::kResources);
  RetainPtr<CPDF_Dictionary> direct_resources =
      GetMutableDirectDict(direct_resources_object);
  RetainPtr<CPDF_Dictionary> effective_resources = page->GetMutableResources();

  if (!effective_resources) {
    auto new_resources = doc->NewIndirect<CPDF_Dictionary>();
    page_dict->SetNewFor<CPDF_Reference>(pdfium::page_object::kResources,
                                         doc,
                                         new_resources->GetObjNum());
    // Update the object graph and the holder's effective resources. This does
    // not refresh already-parsed page content; callers that need to render or
    // inspect appended streams through a live FPDF_PAGE must close and reload.
    page->SetResources(new_resources);
    return new_resources;
  }

  const bool needs_clone =
      !direct_resources || direct_resources != effective_resources ||
      IsPageResourceShared(doc, page_dict, effective_resources);
  if (!needs_clone) {
    return direct_resources;
  }

  RetainPtr<CPDF_Dictionary> cloned =
      CloneDictionaryIndirect(doc, effective_resources);
  if (!cloned) {
    return nullptr;
  }
  page_dict->SetNewFor<CPDF_Reference>(pdfium::page_object::kResources,
                                       doc, cloned->GetObjNum());
  // See the new-resources case above: this keeps the holder's effective
  // resources aligned with the page dictionary, not the parsed-content cache.
  page->SetResources(cloned);
  return cloned;
}

// static
RetainPtr<CPDF_Dictionary> CPDF_PageResourceEditor::EnsureLocalResourceSubdict(
    CPDF_Document* doc,
    RetainPtr<CPDF_Dictionary> resources,
    ByteStringView key) {
  ByteString key_string(key);
  RetainPtr<CPDF_Object> subdict_object = resources->GetMutableObjectFor(key);
  if (!subdict_object) {
    return resources->SetNewFor<CPDF_Dictionary>(key_string);
  }

  RetainPtr<CPDF_Dictionary> subdict = GetMutableDirectDict(subdict_object);
  if (!subdict) {
    return nullptr;
  }

  RetainPtr<CPDF_Reference> reference = ToReference(subdict_object);
  if (!reference) {
    // The caller is about to mutate this direct nested resource dictionary by
    // adding a resource name. In incremental saves, CPDF_Creator only writes
    // dirty indirect objects, so mark the owning /Resources object dirty too;
    // otherwise the nested direct mutation can be omitted from the new
    // revision even though the subdictionary itself is dirty.
    resources->SetDirty(true);
    return subdict;
  }

  // This append-only editor is about to mutate the resource subdictionary by
  // adding a new resource name. If the subdictionary is indirect, clone it
  // before mutation so old indirect objects remain byte-stable and sibling
  // pages cannot observe the new resource entry through a shared reference.
  RetainPtr<CPDF_Dictionary> cloned =
      CloneDictionaryIndirect(doc, subdict);
  if (!cloned) {
    return nullptr;
  }
  resources->SetNewFor<CPDF_Reference>(key_string, doc,
                                       cloned->GetObjNum());
  return cloned;
}

// static
ByteString CPDF_PageResourceEditor::AllocateUniqueResourceName(
    RetainPtr<CPDF_Dictionary> resource_dict,
    ByteStringView prefix) {
  for (int index = 0; index < 10000; ++index) {
    ByteString candidate(prefix);
    candidate += ByteString::Format("%d", index);
    if (!resource_dict->KeyExist(candidate.AsStringView())) {
      return candidate;
    }
  }
  return ByteString();
}
