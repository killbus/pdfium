// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef PUBLIC_FPDF_PPO_H_
#define PUBLIC_FPDF_PPO_H_

// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Experimental API.
// Import pages to a FPDF_DOCUMENT.
//
//   dest_doc     - The destination document for the pages.
//   src_doc      - The document to be imported.
//   page_indices - An array of page indices to be imported. The first page is
//                  zero. If |page_indices| is NULL, all pages from |src_doc|
//                  are imported.
//   length       - The length of the |page_indices| array.
//   index        - The page index at which to insert the first imported page
//                  into |dest_doc|. The first page is zero.
//
// Returns TRUE on success. Returns FALSE if any pages in |page_indices| is
// invalid.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_ImportPagesByIndex(FPDF_DOCUMENT dest_doc,
                        FPDF_DOCUMENT src_doc,
                        const int* page_indices,
                        unsigned long length,
                        int index);

// Import pages to a FPDF_DOCUMENT.
//
//   dest_doc  - The destination document for the pages.
//   src_doc   - The document to be imported.
//   pagerange - A page range string, Such as "1,3,5-7". The first page is one.
//               If |pagerange| is NULL, all pages from |src_doc| are imported.
//   index     - The page index at which to insert the first imported page into
//               |dest_doc|. The first page is zero.
//
// Returns TRUE on success. Returns FALSE if any pages in |pagerange| is
// invalid or if |pagerange| cannot be read.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_ImportPages(FPDF_DOCUMENT dest_doc,
                                                     FPDF_DOCUMENT src_doc,
                                                     FPDF_BYTESTRING pagerange,
                                                     int index);

// Experimental API.
// Create a new document from |src_doc|.  The pages of |src_doc| will be
// combined to provide |num_pages_on_x_axis x num_pages_on_y_axis| pages per
// |output_doc| page.
//
//   src_doc             - The document to be imported.
//   output_width        - The output page width in PDF "user space" units.
//   output_height       - The output page height in PDF "user space" units.
//   num_pages_on_x_axis - The number of pages on X Axis.
//   num_pages_on_y_axis - The number of pages on Y Axis.
//
// Return value:
//   A handle to the created document, or NULL on failure.
//
// Comments:
//   number of pages per page = num_pages_on_x_axis * num_pages_on_y_axis
//
FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
FPDF_ImportNPagesToOne(FPDF_DOCUMENT src_doc,
                       float output_width,
                       float output_height,
                       size_t num_pages_on_x_axis,
                       size_t num_pages_on_y_axis);

// Experimental API.
// Create a template to generate form xobjects from |src_doc|'s page at
// |src_page_index|, for use in |dest_doc|.
//
// Returns a handle on success, or NULL on failure. Caller owns the newly
// created object.
FPDF_EXPORT FPDF_XOBJECT FPDF_CALLCONV
FPDF_NewXObjectFromPage(FPDF_DOCUMENT dest_doc,
                        FPDF_DOCUMENT src_doc,
                        int src_page_index);

// Experimental API.
// Close an FPDF_XOBJECT handle created by FPDF_NewXObjectFromPage().
// FPDF_PAGEOBJECTs created from the FPDF_XOBJECT handle are not affected.
FPDF_EXPORT void FPDF_CALLCONV FPDF_CloseXObject(FPDF_XOBJECT xobject);

// Experimental API.
// Create a new form object from an FPDF_XOBJECT object.
//
// Returns a new form object on success, or NULL on failure. Caller owns the
// newly created object.
FPDF_EXPORT FPDF_PAGEOBJECT FPDF_CALLCONV
FPDF_NewFormObjectFromXObject(FPDF_XOBJECT xobject);

// Copy the viewer preferences from |src_doc| into |dest_doc|.
//
//   dest_doc - Document to write the viewer preferences into.
//   src_doc  - Document to read the viewer preferences from.
//
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_CopyViewerPreferences(FPDF_DOCUMENT dest_doc, FPDF_DOCUMENT src_doc);

// Experimental API.
// Copy bookmarks from source document to destination document.
//
//   dest_doc         - The destination document.
//   src_doc          - The source document to copy bookmarks from.
//   pagerange        - Page range string (e.g., "1,3,5-7"). First page is 1.
//                      If NULL or empty, all pages are included.
//   dest_start_index - The destination page index (0-based) where imported
//                      pages start.
//
// Returns TRUE on success, FALSE if documents are invalid or page range is empty.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_CopyBookmarks(
    FPDF_DOCUMENT dest_doc,
    FPDF_DOCUMENT src_doc,
    FPDF_BYTESTRING pagerange,
    int dest_start_index);

// Experimental API.
// Remap page links in the destination document pages that were imported from source.
//
//   dest_doc         - The destination document.
//   src_doc          - The source document.
//   pagerange        - Page range string of the imported pages.
//   dest_start_index - The destination page index (0-based) where imported
//                      pages start.
//
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_RemapPageLinks(
    FPDF_DOCUMENT dest_doc,
    FPDF_DOCUMENT src_doc,
    FPDF_BYTESTRING pagerange,
    int dest_start_index);


// Update bookmark destinations after page reordering.
//
//   document - The document containing bookmarks to update.
//   old_page_indices - Array of page indices before reordering.
//   new_page_indices - Array of corresponding new page indices (0-based).
//   count - Number of pages that were moved.
//
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV 
FPDF_UpdateBookmarkDestinations(FPDF_DOCUMENT document,
                                const int* old_page_indices,
                                const int* new_page_indices,
                                int count);

// Experimental API.
// Handle to a page import session.
typedef struct fpdf_pageimport_t__* FPDF_PAGEIMPORT;

// Experimental API.
// Create a page import session for importing pages to |dest_doc|.
//
// The session maintains an internal resource cache to avoid duplicating
// shared resources (fonts, images) when the same source document is
// imported multiple times.
//
//   dest_doc - Destination document.
//   reserved - Reserved argument, must be NULL. (Formerly src_doc)
//
// Returns a session handle on success, or NULL on failure.
FPDF_EXPORT FPDF_PAGEIMPORT FPDF_CALLCONV
FPDF_PageImport_Begin(FPDF_DOCUMENT dest_doc, FPDF_DOCUMENT reserved);

// Experimental API.
// Import pages from a source document using an existing session.
//
//   session   - Session handle from FPDF_PageImport_Begin().
//   src_doc   - Source document to import pages from. Must be logically the same
//               document used in previous calls for this session (e.g. same file content).
//   pagerange - Page range string (e.g., "1,3,5-7"). First page is 1.
//   index     - Destination insertion index (0-based).
//
// Returns TRUE on success, FALSE if session is invalid or page range is bad.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_PageImport_ImportPages(FPDF_PAGEIMPORT session,
                            FPDF_DOCUMENT src_doc,
                            FPDF_BYTESTRING pagerange,
                            int index);

// Experimental API.
// Close a page import session and free associated resources.
//
//   session - Session handle from FPDF_PageImport_Begin().
FPDF_EXPORT void FPDF_CALLCONV
FPDF_PageImport_End(FPDF_PAGEIMPORT session);


#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_FPDF_PPO_H_
