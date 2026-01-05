// Copyright 2024 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <vector>
#include <memory>
#include <numeric>

#include "public/fpdf_ppo.h"
#include "public/fpdf_edit.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fpdfdoc/cpdf_bookmark.h"
#include "core/fpdfdoc/cpdf_bookmarktree.h"
#include "core/fpdfdoc/cpdf_dest.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

std::vector<uint32_t> GetPageIndices(const CPDF_Document& doc,
                                     const ByteString& page_range) {
  uint32_t count = doc.GetPageCount();
  if (!page_range.IsEmpty()) {
    return ParsePageRangeString(page_range, count);
  }
  std::vector<uint32_t> page_indices(count);
  std::iota(page_indices.begin(), page_indices.end(), 0);
  return page_indices;
}

// Create destination array using Clone() for efficiency and correctness
RetainPtr<CPDF_Array> CreateDestArray(CPDF_Document* dest_doc, 
                                      int dest_page_index, 
                                      const CPDF_Array* src_dest_array) {
  if (!dest_doc || !src_dest_array || src_dest_array->size() < 1) return nullptr;
  
  RetainPtr<const CPDF_Dictionary> pPageDict = dest_doc->GetPageDictionary(dest_page_index);
  if (!pPageDict) return nullptr;

  // Clone entire array to preserve all destination parameters
  RetainPtr<CPDF_Array> pNewDest = 
      pdfium::WrapRetain(src_dest_array->Clone()->AsMutableArray());
  
  // Replace only the first element (page reference)
  pNewDest->SetAt(0, pdfium::MakeRetain<CPDF_Reference>(dest_doc, pPageDict->GetObjNum()));
  
  return pNewDest;
}

// Maximum depth for recursive bookmark copying to prevent stack overflow.
constexpr size_t kMaxBookmarkDepth = 100;
// Maximum sibling count to prevent infinite loops from circular references.
constexpr size_t kMaxSiblingCount = 1000;

void UpdateParentCounts(CPDF_Dictionary* parent, CPDF_Document* doc, int delta);

void CopyBookmarkRecursive(CPDF_Document* dest_doc,
                           CPDF_Document* src_doc,
                           const CPDF_Bookmark& src_bookmark,
                           CPDF_Dictionary* dest_parent_dict,
                           const std::map<int, int>& page_map,
                           size_t depth) {
    if (depth >= kMaxBookmarkDepth || !src_bookmark.GetDict()) return;

    auto new_dict = dest_doc->NewIndirect<CPDF_Dictionary>();
    new_dict->SetNewFor<CPDF_String>("Title", src_bookmark.GetTitle().AsStringView());

    // Copy Color and Style if present
    const CPDF_Dictionary* src_dict = src_bookmark.GetDict();
    if (src_dict->KeyExist("C")) new_dict->SetFor("C", src_dict->GetDirectObjectFor("C")->Clone());
    if (src_dict->KeyExist("F")) new_dict->SetFor("F", src_dict->GetDirectObjectFor("F")->Clone());

    // Handle Destination
    CPDF_Dest dest = src_bookmark.GetDest(src_doc);

    if (dest.GetArray()) {
        int src_page_idx = dest.GetDestPageIndex(src_doc);
        if (src_page_idx >= 0) {
            auto it = page_map.find(src_page_idx);
            if (it != page_map.end()) {
                auto new_dest_array = CreateDestArray(dest_doc, it->second, dest.GetArray());
                if (new_dest_array) {
                    new_dict->SetFor("Dest", new_dest_array);
                }
            }
        }
    } else {
        // Check for Action (similar to RemapPageLinksOnPage logic)
        // If the bookmark uses an Action (e.g., GoTo) instead of a direct Dest,
        // we need to extract the destination from the Action and map it.
        RetainPtr<const CPDF_Dictionary> action_dict = src_dict->GetDictFor("A");
        if (action_dict && action_dict->GetByteStringFor("S") == "GoTo") {
            RetainPtr<const CPDF_Object> d = action_dict->GetDirectObjectFor("D");
            if (d && d->IsArray()) {
                RetainPtr<const CPDF_Array> action_dest_array = ToArray(d);
                if (action_dest_array) {
                    CPDF_Dest action_dest(action_dest_array);
                    int src_page_idx = action_dest.GetDestPageIndex(src_doc);

                    if (src_page_idx >= 0) {
                        auto it = page_map.find(src_page_idx);
                        if (it != page_map.end()) {
                            // Remap the destination page index
                            auto new_dest_array = CreateDestArray(dest_doc, it->second, action_dest_array.Get());
                            if (new_dest_array) {
                                // Convert Action to direct Dest for simplicity and better compatibility
                                new_dict->SetFor("Dest", new_dest_array);
                            }
                        }
                    }
                }
            }
        }
    }

    // Append to parent
    if (!dest_parent_dict->KeyExist("First")) {
        dest_parent_dict->SetNewFor<CPDF_Reference>("First", dest_doc, new_dict->GetObjNum());
        dest_parent_dict->SetNewFor<CPDF_Reference>("Last", dest_doc, new_dict->GetObjNum());
    } else {
        // Append as sibling to existing bookmarks
        RetainPtr<CPDF_Reference> last_ref = ToReference(dest_parent_dict->GetMutableObjectFor("Last"));
        if (last_ref) {
            RetainPtr<CPDF_Dictionary> last_dict = last_ref->GetMutableDict();
            if (last_dict) {
                last_dict->SetNewFor<CPDF_Reference>("Next", dest_doc, new_dict->GetObjNum());
                new_dict->SetNewFor<CPDF_Reference>("Prev", dest_doc, last_dict->GetObjNum());
            }
        }
        dest_parent_dict->SetNewFor<CPDF_Reference>("Last", dest_doc, new_dict->GetObjNum());
    }

    // Update the count for the parent and ancestors.
    UpdateParentCounts(dest_parent_dict, dest_doc, 1);
    
    // Set Parent ptr for the new item
    new_dict->SetNewFor<CPDF_Reference>("Parent", dest_doc, dest_parent_dict->GetObjNum());

    // Recurse children
    RetainPtr<const CPDF_Dictionary> first_child = src_dict->GetDictFor("First");
    if (first_child) {
        CPDF_Bookmark child(first_child);
        size_t sibling_count = 0;
        while (child.GetDict() && sibling_count++ < kMaxSiblingCount) {
            CopyBookmarkRecursive(dest_doc, src_doc, child, new_dict.Get(),
                                  page_map, depth + 1);
            
            RetainPtr<const CPDF_Dictionary> next = child.GetDict()->GetDictFor("Next");
            child = CPDF_Bookmark(next);
        }
    }
}

void UpdateParentCounts(CPDF_Dictionary* parent, CPDF_Document* doc, int delta) {
  CPDF_Dictionary* current = parent;
  while (current) {
    int count = current->GetIntegerFor("Count", 0);
    bool is_root = current->GetByteStringFor("Type") == "Outlines";

    if (is_root) {
      current->SetNewFor<CPDF_Number>("Count", count + delta);
      break;
    }

    // Regular bookmark
    if (count >= 0) {
      // Open: Increment count and propagate up
      current->SetNewFor<CPDF_Number>("Count", count + delta);
    } else {
      // Closed: Decrement count (increase magnitude) and stop propagation
      current->SetNewFor<CPDF_Number>("Count", count - delta);
      break;
    }

    RetainPtr<CPDF_Reference> parent_ref =
        ToReference(current->GetMutableObjectFor("Parent"));
    if (!parent_ref) {
      break;
    }
    current = parent_ref->GetMutableDict().Get();
  }
}

// Helper: Update a destination using CPDF_Dest abstraction
bool UpdateDestinationPageReference(
    CPDF_Document* doc,
    CPDF_Array* dest_array,
    const std::map<uint32_t, int>& page_obj_to_new_index) {
  
  if (!dest_array || dest_array->size() == 0) {
    return false;
  }

  // Use CPDF_Dest abstraction to leverage existing functionality
  CPDF_Dest dest(pdfium::WrapRetain(dest_array));
  int current_page_index = dest.GetDestPageIndex(doc);
  
  if (current_page_index < 0) {
    return false; // Invalid destination
  }
  
  RetainPtr<CPDF_Object> first = dest_array->GetMutableDirectObjectAt(0);
  
  // Case 1: Direct page index (convert to reference for stability)
  // This improves robustness as references survive page reordering better
  if (first->IsNumber()) {
    if (current_page_index >= 0 && current_page_index < doc->GetPageCount()) {
      RetainPtr<const CPDF_Dictionary> page_dict = doc->GetPageDictionary(current_page_index);
      if (page_dict) {
        // Upgrade numeric index to robust Page Object Reference
        dest_array->SetAt(0, pdfium::MakeRetain<CPDF_Reference>(
            doc, page_dict->GetObjNum()));
        return true;
      }
    }
    return false;
  }
  
  // Case 2: Page dictionary reference (standard)
  // Check if this page was moved and needs remapping
  if (first->IsReference()) {
    uint32_t page_obj_num = ToReference(first)->GetRefObjNum();
    auto it = page_obj_to_new_index.find(page_obj_num);
    
    if (it != page_obj_to_new_index.end()) {
      int new_index = it->second;
      RetainPtr<const CPDF_Dictionary> new_page_dict = 
          doc->GetPageDictionary(new_index);
      
      if (new_page_dict) {
        // Update reference to point to the page at new position
        dest_array->SetAt(0, pdfium::MakeRetain<CPDF_Reference>(
            doc, new_page_dict->GetObjNum()));
        return true;
      }
    }
  }
  
  return false;
}

// Recursive bookmark traversal and update
void UpdateBookmarksRecursive(
    CPDF_Document* doc,
    CPDF_Dictionary* bookmark_dict,
    const std::map<uint32_t, int>& page_obj_to_new_index,
    std::set<uint32_t>& visited,
    size_t depth) {
  
  if (!bookmark_dict) return;
  if (depth > kMaxBookmarkDepth) return;
  
  uint32_t obj_num = bookmark_dict->GetObjNum();
  if (obj_num != 0 && !visited.insert(obj_num).second) {
    return; // Circular reference detected
  }

  // Update direct Dest
  RetainPtr<CPDF_Array> dest_array = bookmark_dict->GetMutableArrayFor("Dest");
  if (dest_array) {
    UpdateDestinationPageReference(doc, dest_array.Get(), page_obj_to_new_index);
  }
  
  // Update Action/GoTo Dest
  RetainPtr<CPDF_Dictionary> action_dict = bookmark_dict->GetMutableDictFor("A");
  if (action_dict) {
    ByteString action_type = action_dict->GetByteStringFor("S");
    
    if (action_type == "GoTo") {
      RetainPtr<CPDF_Object> d_obj = action_dict->GetMutableDirectObjectFor("D");
      
      if (d_obj && d_obj->IsArray()) {
        RetainPtr<CPDF_Array> action_dest = action_dict->GetMutableArrayFor("D");
        UpdateDestinationPageReference(doc, action_dest.Get(), page_obj_to_new_index);
      }
    }
  }
  
  // Recurse to children
  RetainPtr<CPDF_Dictionary> first_child = 
      bookmark_dict->GetMutableDictFor("First");
  if (first_child) {
    UpdateBookmarksRecursive(doc, first_child.Get(), 
                          page_obj_to_new_index, visited, depth + 1);
  }
  
  // Recurse to siblings (iterative)
  RetainPtr<CPDF_Dictionary> current = pdfium::WrapRetain(bookmark_dict);
  size_t sibling_count = 0;
  
  while (current && sibling_count++ < kMaxSiblingCount) {
    RetainPtr<CPDF_Dictionary> next_sibling = 
        current->GetMutableDictFor("Next");
    if (!next_sibling) break;
    
    UpdateBookmarksRecursive(doc, next_sibling.Get(), 
                          page_obj_to_new_index, visited, depth);
    current = next_sibling;
  }
}

}  // namespace


extern "C" {

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_CopyBookmarks(
    FPDF_DOCUMENT dest_doc_handle,
    FPDF_DOCUMENT src_doc_handle,
    FPDF_BYTESTRING pagerange,
    int dest_start_index) 
{
    CPDF_Document* dest_doc = CPDFDocumentFromFPDFDocument(dest_doc_handle);
    CPDF_Document* src_doc = CPDFDocumentFromFPDFDocument(src_doc_handle);
    if (!dest_doc || !src_doc) return false;

    // 1. Build Page Map
    std::vector<uint32_t> src_indices = GetPageIndices(*src_doc, pagerange);
    if (src_indices.empty()) return false;

    std::map<int, int> page_map;
    for (size_t i = 0; i < src_indices.size(); ++i) {
        page_map[static_cast<int>(src_indices[i])] = dest_start_index + static_cast<int>(i);
    }

    // 2. Check Source Bookmarks first to avoid creating empty Outlines in Dest
    const CPDF_Dictionary* src_root_dict = src_doc->GetRoot();
    if (!src_root_dict) return true; // No src root, nothing to copy

    RetainPtr<const CPDF_Dictionary> src_outlines = src_root_dict->GetDictFor("Outlines");
    if (!src_outlines) return true; // src bookmarks empty -> nothing to copy

    // 3. Prepare Dest Root (Outlines) - Only if we have something to copy
    RetainPtr<CPDF_Dictionary> dest_root = dest_doc->GetMutableRoot();
    if (!dest_root) return false;

    RetainPtr<CPDF_Dictionary> dest_outlines = dest_root->GetMutableDictFor("Outlines");
    if (!dest_outlines) {
        dest_outlines = dest_doc->NewIndirect<CPDF_Dictionary>();
        dest_outlines->SetNewFor<CPDF_Name>("Type", "Outlines");
        dest_outlines->SetNewFor<CPDF_Number>("Count", 0);
        dest_root->SetNewFor<CPDF_Reference>("Outlines", dest_doc, dest_outlines->GetObjNum());
    } else if (!dest_outlines->KeyExist("Count")) {
        // Ensure existing outlines have a count.
        dest_outlines->SetNewFor<CPDF_Number>("Count", 0);
    }

    // 4. Copy Bookmarks
    CPDF_Bookmark src_root_bookmark(src_outlines);
    
    // We iterate the top-level children of src outlines
    RetainPtr<const CPDF_Dictionary> first_child = src_outlines->GetDictFor("First");
    if (first_child) {
        CPDF_Bookmark child(first_child);
        size_t sibling_count = 0;
        while (child.GetDict() && sibling_count++ < kMaxSiblingCount) {
            CopyBookmarkRecursive(dest_doc, src_doc, child, dest_outlines.Get(),
                                  page_map, 0);
            
            RetainPtr<const CPDF_Dictionary> next = child.GetDict()->GetDictFor("Next");
            child = CPDF_Bookmark(next);
        }
    }

    return true;
}

void RemapPageLinksOnPage(CPDF_Document* dest_doc,
                           CPDF_Dictionary* page_dict,
                           const std::map<int, int>& page_map) {
    if (!page_dict) return;

    RetainPtr<CPDF_Array> pAnnots = page_dict->GetMutableArrayFor("Annots");
    if (!pAnnots) return;

    for (size_t i = 0; i < pAnnots->size(); ++i) {
        RetainPtr<CPDF_Dictionary> annot_dict = pAnnots->GetMutableDictAt(i);
        if (!annot_dict || annot_dict->GetByteStringFor("Subtype") != "Link") continue;

        // Check Dest - optimize to single lookup for performance
        RetainPtr<CPDF_Object> dest_obj = annot_dict->GetMutableDirectObjectFor("Dest");
        RetainPtr<CPDF_Array> dest_array;
        
        if (dest_obj) {
            if (dest_obj->IsArray()) {
                // Direct access, no second lookup needed
                dest_array = annot_dict->GetMutableArrayFor("Dest");
            } else if (dest_obj->IsString()) {
                // Named dest - resolution is complex, skipping for now
            }
        } 
        
        // If not direct Dest, check Action - optimize to single lookup
        if (!dest_array) {
           RetainPtr<CPDF_Dictionary> action_dict = annot_dict->GetMutableDictFor("A");
           if (action_dict && action_dict->GetByteStringFor("S") == "GoTo") {
               RetainPtr<CPDF_Object> d = action_dict->GetMutableDirectObjectFor("D");
               if (d && d->IsArray()) {
                   // Direct access, no second lookup needed
                   dest_array = action_dict->GetMutableArrayFor("D");
               }
           }
        }

        if (dest_array && dest_array->size() > 0) {
            // Check first element - optimize to single lookup
            RetainPtr<CPDF_Object> pFirst = dest_array->GetMutableDirectObjectAt(0);
            if (pFirst->IsNumber()) {
                // It is a 0-based page index. This definitely needs fixing!
                int old_page_idx = pFirst->GetInteger();
                auto it = page_map.find(old_page_idx);
                if (it != page_map.end()) {
                    // Now perform actual modification on mutable dest_array
                    dest_array->SetAt(0, pdfium::MakeRetain<CPDF_Number>(it->second));
                }
            } else if (pFirst->IsReference()) {
                // It is a reference to a Page Dictionary.
                // FPDF_ImportPages automatically remaps page object references
                // via UpdateReference() for pages that were actually imported.
                // If the link points to a non-imported page, the reference
                // becomes invalid. Integer page indices are not remapped by
                // ImportPages, which is why we handle them explicitly above.
            }
        }
    }
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_RemapPageLinks(
    FPDF_DOCUMENT dest_doc_handle,
    FPDF_DOCUMENT src_doc_handle,
    FPDF_BYTESTRING pagerange,
    int dest_start_index) 
{
    CPDF_Document* dest_doc = CPDFDocumentFromFPDFDocument(dest_doc_handle);
    CPDF_Document* src_doc = CPDFDocumentFromFPDFDocument(src_doc_handle);
    if (!dest_doc || !src_doc) return false;

    // 1. Build Page Map
    std::vector<uint32_t> src_indices = GetPageIndices(*src_doc, pagerange);
    if (src_indices.empty()) return false;

    std::map<int, int> page_map;
    for (size_t i = 0; i < src_indices.size(); ++i) {
        page_map[static_cast<int>(src_indices[i])] = dest_start_index + static_cast<int>(i);
    }

    // 2. Iterate newly added pages in Dest Doc
    for (size_t i = 0; i < src_indices.size(); ++i) {
        int dest_page_idx = dest_start_index + static_cast<int>(i);
        RetainPtr<CPDF_Dictionary> page_dict = dest_doc->GetMutablePageDictionary(dest_page_idx);
        if (page_dict) {
            RemapPageLinksOnPage(dest_doc, page_dict.Get(), page_map);
        }
    }

    return true;
}



FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV 
FPDF_UpdateBookmarkDestinations(
    FPDF_DOCUMENT document,
    const int* old_page_indices,
    const int* new_page_indices,
    int count) {
  
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || !old_page_indices || !new_page_indices || count <= 0) {
    return false;
  }
  
  // Build mapping: page_obj_num -> new_index
  // Get object numbers from old indices
  std::map<uint32_t, int> page_obj_to_new_index;
  for (int i = 0; i < count; ++i) {
    int old_index = old_page_indices[i];
    if (old_index < 0 || old_index >= doc->GetPageCount()) {
      continue; // Skip invalid indices
    }
    
    RetainPtr<const CPDF_Dictionary> page_dict = doc->GetPageDictionary(old_index);
    if (page_dict) {
      uint32_t page_obj_num = page_dict->GetObjNum();
      page_obj_to_new_index[page_obj_num] = new_page_indices[i];
    }
  }
  
  // Get Outlines root
  RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
  if (!root) {
    return false;
  }
  
  RetainPtr<CPDF_Dictionary> outlines = root->GetMutableDictFor("Outlines");
  if (!outlines) {
    return true; // No bookmarks to update
  }
  
  // Track visited nodes to prevent infinite loops
  std::set<uint32_t> visited;
  
  // Traverse all top-level bookmarks
  RetainPtr<CPDF_Dictionary> first_child = outlines->GetMutableDictFor("First");
  if (first_child) {
    UpdateBookmarksRecursive(doc, first_child.Get(), 
                          page_obj_to_new_index, visited, 0);
  }
  
  return true;
}

} // extern "C"
